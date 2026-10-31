/*
 *----------------------------------------------------------------------------
 *              UVC tools: usbvideo.device client helpers
 *----------------------------------------------------------------------------
 * Implements uvc_client.h, in the same section order. Device API only, no
 * neptune.library calls.
 */

#include <proto/dos.h>
#include <proto/exec.h>

#include <dos/dos.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/usb_video.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "uvc_client.h"
#include "mjpegdht.h"

#define NAME_SIZE   256 /* frame file name buffer */
#define DIGITS_MAX  10  /* decimal digits of a ULONG */
#define FOURCC_LEN  4
#define ENDING_SIZE (1 + DIGITS_MAX + 1 + FOURCC_LEN + 1) /* suffix after the stem: "-", number,
                                                            ".", extension, NUL */
#define REPORT_SIZE (NAME_SIZE + 64) /* a failure's text in the Shell, a file name in it */
#define READ_ALIGN  64 /* of a read's buffer: a cache line, for a decoder that reads it in place */

#define NAL_TYPE_MASK   0x1f
#define NAL_SLICE       1 /* of a frame that is no key frame */
#define NAL_SLICE_LAST  4 /* its data partitions, up to here */
#define NAL_SLICE_IDR   5

_Static_assert(UVC_JPEG_ROOM == MJPEG_STD_DHT_SIZE, "the room in front of a frame takes the tables exactly");

UBYTE *uvcReportTo;

/* Describe a failure: into uvcReportTo, else to the Shell */
void uvcReport(const char *format, ...)
{
    UBYTE text[REPORT_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf((char *) (uvcReportTo ? uvcReportTo : text), uvcReportTo ? UVC_TEXT_SIZE : sizeof(text), format, args);
    va_end(args);
    if(!uvcReportTo)
    {
        Printf("%s\n", text);
    }
}

/*
 * Camera
 */

/* Return a device error code as text */
STRPTR uvcErrorText(LONG err)
{
    switch(err)
    {
        case 0:                 return "ok";
        case UVERR_NOMODE:      return "no mode has been set";
        case UVERR_BADMODE:     return "the camera has no such mode";
        case UVERR_REFUSED:     return "the camera refused";
        case UVERR_NOBANDWIDTH: return "the mode does not fit the bus";
        case UVERR_BUSY:        return "busy";
        case UVERR_OVERFLOW:    return "frame longer than the buffer";
        case UVERR_GONE:        return "the camera is gone";
        case UVERR_USB:         return "USB transfer failed";
        case UVERR_NOSTATUS:    return "the camera has no status endpoint";
        case IOERR_NOCMD:       return "command not supported";
        case IOERR_OPENFAIL:    return "could not open";
        case IOERR_BADLENGTH:   return "bad length";
        case IOERR_ABORTED:     return "aborted";
    }
    return "error";
}

/* Execute one command synchronously (DoIO); return io_Error */
LONG uvcCommand(struct IOStdReq *ioreq, UWORD command, APTR data, ULONG len)
{
    ioreq->io_Command = command;
    ioreq->io_Data = data;
    ioreq->io_Length = len;
    return DoIO((struct IORequest *) ioreq);
}

/* Send a raw class request to an entity (NSCMD_UV_REQUEST), for controls the
   device has no name for. Return io_Error; *reason receives the camera's
   request error code. */
LONG uvcRequest(struct IOStdReq *ioreq, ULONG request, ULONG entity, ULONG selector, APTR data, ULONG len, UBYTE *reason)
{
    struct UsbVideoRequest uvr =
    {
        .uvr_Request = request,
        .uvr_Entity = entity,
        .uvr_Selector = selector,
        .uvr_ErrorCode = UVREC_NO_ERROR,
        .uvr_Length = len,
        .uvr_Data = data
    };
    LONG err = uvcCommand(ioreq, NSCMD_UV_REQUEST, &uvr, sizeof(uvr));

    *reason = uvr.uvr_ErrorCode;
    return err;
}

/* Read the mode table (NSCMD_UV_GETMODES) into an AllocVec'd buffer.
   On failure print the reason and return FALSE. */
static BOOL readModes(struct UvcCamera *cm)
{
    ULONG size = cm->cm_Info.uvi_ModeTableSize;

    if(!size)
    {
        return TRUE; /* no format the class can stream: empty table */
    }
    if(!(cm->cm_Modes = AllocVec(size, MEMF_ANY)))
    {
        uvcReport("Not enough memory for the list of modes");
        return FALSE;
    }

    LONG err = uvcCommand(cm->cm_Request, NSCMD_UV_GETMODES, cm->cm_Modes, size);

    if(err)
    {
        uvcReport("Could not read the modes: %s", uvcErrorText(err));
        return FALSE;
    }
    cm->cm_ModesSize = cm->cm_Request->io_Actual;
    return TRUE;
}

/* Find the camera's H.264 extension unit (NSCMD_UV_GETTOPOLOGY); cm_H264Unit
   stays 0 if it has none */
static void findH264Unit(struct UvcCamera *cm)
{
    static const UBYTE guid[sizeof(((struct UsbVideoEntity *) 0)->uve_GUID)] = UVCX_H264_XU_GUID;
    ULONG size = cm->cm_Info.uvi_TopologySize;
    UBYTE *table = AllocVec(size, MEMF_ANY);

    if(table && !uvcCommand(cm->cm_Request, NSCMD_UV_GETTOPOLOGY, table, size))
    {
        ULONG actual = cm->cm_Request->io_Actual;

        for(ULONG pos = 0; (pos < actual) && !cm->cm_H264Unit; )
        {
            const struct UsbVideoEntity *uve = (const struct UsbVideoEntity *) (table + pos);

            if((uve->uve_Type == UDST_VIDEO_CTRL_EXTENSION_UNIT) && !memcmp(uve->uve_GUID, guid, sizeof(guid)))
            {
                cm->cm_H264Unit = uve->uve_ID;
            }
            pos += uve->uve_Size;
        }
    }
    FreeVec(table);
}

/* Create port and request, open the unit, read NSCMD_UV_GETINFO and the
   mode table. On failure print the reason and return FALSE. Call
   uvcCloseCamera() in either case. */
BOOL uvcOpenCamera(struct UvcCamera *cm, ULONG unit)
{
    memset(cm, 0, sizeof(*cm));
    cm->cm_Unit = unit;
    cm->cm_Port = CreateMsgPort();
    cm->cm_Request = (struct IOStdReq *) CreateIORequest(cm->cm_Port, sizeof(struct IOVideoReq));
    if(!cm->cm_Request)
    {
        uvcReport("Not enough memory");
        return FALSE;
    }

    LONG err = OpenDevice(USBVIDEO_NAME, unit, (struct IORequest *) cm->cm_Request, 0);

    if(err)
    {
        uvcReport((err == IOERR_UNITBUSY) ? "The camera on unit %ld is in use by another program."
                                          : "There is no camera on unit %ld.", unit);
        return FALSE;
    }
    cm->cm_Open = TRUE;
    if((err = uvcCommand(cm->cm_Request, NSCMD_UV_GETINFO, &cm->cm_Info, sizeof(cm->cm_Info))))
    {
        uvcReport("The camera did not answer: %s", uvcErrorText(err));
        return FALSE;
    }
    findH264Unit(cm);
    return readModes(cm);
}

/* Free everything uvcOpenCamera() allocated; safe after a partial open */
void uvcCloseCamera(struct UvcCamera *cm)
{
    FreeVec(cm->cm_Modes);
    cm->cm_Modes = NULL;
    if(cm->cm_Open)
    {
        CloseDevice((struct IORequest *) cm->cm_Request);
        cm->cm_Open = FALSE;
    }
    DeleteIORequest((struct IORequest *) cm->cm_Request);
    DeleteMsgPort(cm->cm_Port);
}

/* Ask the camera's encoder for a key frame that brings its parameter sets
   (the stream's next picture). Return FALSE if the camera has no H.264 unit,
   or refused: the key frame then comes when the camera's interval says so.
   Does not wait for the picture; while the stream runs, a read may reply
   during the command, so look at the port afterwards whatever came. */
BOOL uvcRequestKeyFrame(struct UvcCamera *cm)
{
    struct UvcxPictureType pic = { 0, AROS_WORD2LE(2) };
    UBYTE reason;

    return cm->cm_H264Unit && !uvcRequest(cm->cm_Request, UVUDR_SET_CUR, cm->cm_H264Unit, UVCX_PICTURE_TYPE_CONTROL,
                                          &pic, sizeof(pic), &reason);
}

/*
 * Text
 */

/* Print the frame rate with one decimal for an interval in 100 ns units;
   "?" for 0 */
static void printRate(ULONG interval)
{
    if(interval)
    {
        Printf("%ld.%ld", UVC_INTERVAL_UNITS / interval, (10 * UVC_INTERVAL_UNITS / interval) % 10);
    } else {
        PutStr("?");
    }
}

/* Write a FourCC as four characters into text, UVC_FOURCC_SIZE bytes; return text */
UBYTE *uvcFourCCText(ULONG fourcc, UBYTE *text)
{
    text[0] = fourcc >> 24;
    text[1] = fourcc >> 16;
    text[2] = fourcc >> 8;
    text[3] = fourcc;
    text[4] = 0;
    return text;
}

/* Convert a string to a FourCC, space-padded to four characters, case
   kept; return 0 for NULL or empty */
ULONG uvcParseFourCC(STRPTR text)
{
    ULONG fourcc = 0;

    if(!text || !text[0])
    {
        return 0;
    }
    for(ULONG cnt = 0; cnt < FOURCC_LEN; cnt++)
    {
        fourcc = (fourcc << 8) | (UBYTE) (*text ? *text++ : ' ');
    }
    return fourcc;
}

/* Print a one-line camera summary: unit, product, manufacturer, VID:PID,
   serial number, UVC version, transfer type, mode count */
void uvcPrintCamera(const struct UvcCamera *cm)
{
    const struct UsbVideoInfo *uvi = &cm->cm_Info;

    Printf("Unit %ld: %s by %s (%04lx:%04lx), serial number %s, UVC %lx.%02lx, %s, %ld modes\n", cm->cm_Unit,
           uvi->uvi_ProductName, uvi->uvi_Manufacturer[0] ? uvi->uvi_Manufacturer : (const UBYTE *) "n/a",
           (ULONG) uvi->uvi_VendorID, (ULONG) uvi->uvi_ProductID,
           uvi->uvi_SerialNumber,
           (ULONG) uvi->uvi_Version >> 8, (ULONG) uvi->uvi_Version & 0xff,
           (uvi->uvi_Flags & UVIF_BULK) ? "bulk" : "isochronous", uvi->uvi_NumModes);
}

/* Print the mode table, one line per mode */
void uvcPrintModes(const struct UvcCamera *cm)
{
    UVC_EACH_MODE(uvm, cm->cm_Modes, cm->cm_ModesSize)
    {
        UBYTE name[UVC_FOURCC_SIZE];

        Printf("  %s %4ldx%-4ld  ", uvcFourCCText(uvm->uvm_FourCC, name), (ULONG) uvm->uvm_Width, (ULONG) uvm->uvm_Height);
        if(uvm->uvm_Flags & UVMF_CONTINUOUS)
        {
            printRate(uvm->uvm_Intervals[1]);
            PutStr(" to ");
            printRate(uvm->uvm_Intervals[0]);
            PutStr(" fps");
        } else {
            for(ULONG cnt = 0; cnt < uvm->uvm_NumIntervals; cnt++)
            {
                printRate(uvm->uvm_Intervals[cnt]);
                PutStr((cnt + 1 < uvm->uvm_NumIntervals) ? ", " : " fps");
            }
        }
        PutStr(", default ");
        printRate(uvm->uvm_DefInterval);
        PutStr((uvm->uvm_Flags & UVMF_DEFAULT) ? "  <- the default (the settings window's, else the camera's)\n" : "\n");
    }
}

/* Print the negotiated mode returned by NSCMD_UV_SETMODE: format, size,
   frame rate, and the bit rate and key frame interval if the mode's encoder
   accepted either */
void uvcPrintMode(const struct UsbVideoModeReq *uvq)
{
    UBYTE name[UVC_FOURCC_SIZE];

    Printf("%s %ldx%ld at ", uvcFourCCText(uvq->uvq_FourCC, name), (ULONG) uvq->uvq_Width, (ULONG) uvq->uvq_Height);
    printRate(uvq->uvq_Interval);
    PutStr(" fps");
    if(uvq->uvq_BitRate || uvq->uvq_KeyInterval)
    {
        Printf(", %ld bits a second, a key frame every %ld ms", uvq->uvq_BitRate, uvq->uvq_KeyInterval);
    }
}

/*
 * Capture
 */

/* Start a capture in the mode already set: open the output if stem is not
   NULL, submit the reads, send CMD_START. On failure report the reason and
   return FALSE. Call uvcStopCapture() in either case. */
BOOL uvcStartCapture(struct UvcCapture *cp, struct UvcCamera *cm, const struct UsbVideoModeReq *uvq, STRPTR stem)
{
    return uvcMakeReads(cp, cm, 0, uvq->uvq_MaxFrameSize, MEMF_ANY) &&
           uvcOpenFiles(&cp->cp_Files, stem, uvq->uvq_FourCC) && uvcStartStream(cp);
}

/* Return the read holding the next complete frame, waiting for one.
   Resubmit cp_InHand first, write the frame if the capture has output. NULL
   ends the capture; cp_Error, cp_Interrupted or cp_NotSaved gives the reason.
   Ctrl-C is checked before every take, so a continuous frame flow cannot
   starve it. */
struct IOVideoReq *uvcNextFrame(struct UvcCapture *cp)
{
    ULONG signals = (1UL << cp->cp_Camera->cm_Port->mp_SigBit) | SIGBREAKF_CTRL_C;

    if(cp->cp_InHand)
    {
        uvcSendRead(cp->cp_InHand);
        cp->cp_InHand = NULL;
    }
    while(!(SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C))
    {
        struct UvcRead *rd = uvcTakeFrame(cp);

        if(rd)
        {
            struct IOVideoReq *ivr = rd->rd_Req;

            cp->cp_InHand = rd;
            if(!uvcWriteFrame(&cp->cp_Files, ivr->ivr_Std.io_Data, ivr->ivr_Std.io_Actual))
            {
                cp->cp_NotSaved = TRUE;
                return NULL;
            }
            return ivr;
        }
        if(cp->cp_Error)
        {
            return NULL;
        }
        if(Wait(signals) & SIGBREAKF_CTRL_C)
        {
            break;
        }
    }
    cp->cp_Interrupted = TRUE;
    return NULL;
}

/* End a capture: stop the stream, reclaim and free all reads, close the
   output */
void uvcStopCapture(struct UvcCapture *cp)
{
    uvcHaltCapture(cp);
    for(ULONG cnt = 0; cnt < UVC_NUM_READS; cnt++)
    {
        struct UvcRead *rd = &cp->cp_Reads[cnt];

        FreeVec(rd->rd_Memory);
        rd->rd_Memory = NULL;
        DeleteIORequest((struct IORequest *) rd->rd_Req);
        rd->rd_Req = NULL;
    }
    uvcCloseFiles(&cp->cp_Files);
}

/*
 * Reads
 */

/* Prepare a capture whose loop is the caller's: UVC_NUM_READS requests on the
   camera's port, none sent, each with a buffer of memflags memory that takes
   size bytes after room bytes, which stay free and clear. On failure report
   the reason and return FALSE. Call uvcStopCapture() in either case. */
BOOL uvcMakeReads(struct UvcCapture *cp, struct UvcCamera *cm, ULONG room, ULONG size, ULONG memflags)
{
    struct IOStdReq *ioreq = cm->cm_Request;

    memset(cp, 0, sizeof(*cp));
    cp->cp_Camera = cm;
    for(ULONG cnt = 0; cnt < UVC_NUM_READS; cnt++)
    {
        struct UvcRead *rd = &cp->cp_Reads[cnt];
        struct IOVideoReq *ivr = (struct IOVideoReq *) CreateIORequest(cm->cm_Port, sizeof(struct IOVideoReq));

        rd->rd_Req = ivr;
        rd->rd_Memory = AllocVec(room + size + READ_ALIGN - 1, memflags);
        if(!ivr || !rd->rd_Memory)
        {
            uvcReport("Not enough memory for %ld buffers of %ld bytes", (ULONG) UVC_NUM_READS, room + size);
            return FALSE;
        }
        rd->rd_Buffer = (UBYTE *) (((ULONG) rd->rd_Memory + READ_ALIGN - 1) & ~(ULONG) (READ_ALIGN - 1));
        memset(rd->rd_Buffer, 0, room);
        ivr->ivr_Std.io_Device = ioreq->io_Device;
        ivr->ivr_Std.io_Unit = ioreq->io_Unit;
        ivr->ivr_Std.io_Command = CMD_READ;
        ivr->ivr_Std.io_Data = rd->rd_Buffer + room;
        ivr->ivr_Std.io_Length = size;
    }
    return TRUE;
}

/* Send a read that is in hand to the device */
void uvcSendRead(struct UvcRead *rd)
{
    rd->rd_AtDevice = TRUE;
    SendIO((struct IORequest *) rd->rd_Req);
}

/* Send every read that is in hand */
void uvcSendReads(struct UvcCapture *cp)
{
    for(ULONG cnt = 0; cnt < UVC_NUM_READS; cnt++)
    {
        if(!cp->cp_Reads[cnt].rd_AtDevice)
        {
            uvcSendRead(&cp->cp_Reads[cnt]);
        }
    }
}

/* Return a read that came back, now in hand; NULL if none did */
static struct UvcRead *takeRead(struct UvcCapture *cp)
{
    struct IOVideoReq *ivr = (struct IOVideoReq *) GetMsg(cp->cp_Camera->cm_Port);

    for(ULONG cnt = 0; ivr && (cnt < UVC_NUM_READS); cnt++)
    {
        struct UvcRead *rd = &cp->cp_Reads[cnt];

        if(rd->rd_Req == ivr)
        {
            rd->rd_AtDevice = FALSE;
            return rd;
        }
    }
    return NULL;
}

/* Return a read that came back with a frame, now in hand, sequence gaps
   counted; NULL if none did. It gets no WaitIO(). A frame too long for the
   buffer is counted, dropped and its read sent again. What ends the capture
   sets cp_Error, the read staying in hand. Call until NULL before every
   Wait(): a DoIO() on the camera's request waits on the same port and can eat
   the signal of a read that came meanwhile. */
struct UvcRead *uvcTakeFrame(struct UvcCapture *cp)
{
    struct UvcRead *rd;

    while((rd = takeRead(cp)))
    {
        struct IOVideoReq *ivr = rd->rd_Req;
        LONG err = ivr->ivr_Std.io_Error;

        if(err && (err != UVERR_OVERFLOW))
        {
            cp->cp_Error = err;
            return NULL;
        }
        if(cp->cp_LastSeq)
        {
            cp->cp_Missed += ivr->ivr_Sequence - cp->cp_LastSeq - 1;
        }
        cp->cp_LastSeq = ivr->ivr_Sequence;
        if(!err)
        {
            return rd;
        }
        cp->cp_TooLong++;
        uvcSendRead(rd);
    }
    return NULL;
}

/* Send the reads, then CMD_START. On failure report the reason and return
   FALSE. */
BOOL uvcStartStream(struct UvcCapture *cp)
{
    uvcSendReads(cp);

    LONG err = uvcCommand(cp->cp_Camera->cm_Request, CMD_START, NULL, 0);

    if(err)
    {
        uvcReport("Could not start the stream: %s", uvcErrorText(err));
        return FALSE;
    }
    cp->cp_Streaming = TRUE;
    return TRUE;
}

/* Stop the stream and take every read back, nothing freed: the memory of
   the reads is the caller's to use until uvcStopCapture(). CMD_STOP comes
   first: it hands back the read being filled, which an abort alone would not
   while a silent camera holds it. A read taken with GetMsg() is not at the
   device and gets no WaitIO(). */
void uvcHaltCapture(struct UvcCapture *cp)
{
    if(cp->cp_Streaming)
    {
        uvcCommand(cp->cp_Camera->cm_Request, CMD_STOP, NULL, 0);
        cp->cp_Streaming = FALSE;
    }
    for(ULONG cnt = 0; cnt < UVC_NUM_READS; cnt++)
    {
        struct UvcRead *rd = &cp->cp_Reads[cnt];

        if(rd->rd_AtDevice)
        {
            if(!CheckIO((struct IORequest *) rd->rd_Req))
            {
                AbortIO((struct IORequest *) rd->rd_Req);
            }
            WaitIO((struct IORequest *) rd->rd_Req);
            rd->rd_AtDevice = FALSE;
        }
    }
    cp->cp_InHand = NULL;
}

/*
 * Frames
 */

/* Return TRUE if an H.264 access unit in byte stream form is a key frame: its
   first slice says, and AUD, SEI and parameter sets may come before that. */
BOOL uvcH264Key(const UBYTE *data, ULONG len)
{
    for(ULONG pos = 0; pos + 3 < len; pos++)
    {
        if(data[pos] || data[pos + 1] || (data[pos + 2] != 1))
        {
            continue;
        }

        ULONG kind = data[pos + 3] & NAL_TYPE_MASK;

        if(kind == NAL_SLICE_IDR)
        {
            return TRUE;
        }
        if((kind >= NAL_SLICE) && (kind <= NAL_SLICE_LAST))
        {
            return FALSE;
        }
        pos += 2;
    }
    return FALSE;
}

/* Is a picture in this frame, and all of it? A frame without SOF is a JPEG
   with nothing in it, which some cameras send when the source gives them
   no picture. Return the length of the frame up to and with its EOI, past
   any padding; 0 if it has no picture or no EOI. */
static ULONG jpegLength(const UBYTE *frame, ULONG len)
{
    if(!mjpegFrameHeader(frame, len))
    {
        return 0;
    }
    while((len > 2) && (!frame[len - 1] || (frame[len - 1] == 0xff)))
    {
        len--;
    }
    return ((frame[len - 2] == 0xff) && (frame[len - 1] == 0xd9)) ? len : 0;
}

/* A camera's frame of len bytes that lies UVC_JPEG_ROOM bytes into buffer
   becomes a JPEG that begins at buffer, without moving it: the room and the
   frame's own two SOI bytes are written over, ending just before the
   frame's next marker. What fills them is a new SOI, then the standard
   Huffman tables if the frame has none (they may come before the frame
   header), else a comment of the same length, so the sizes come out alike.
   The comment's bytes are what the room held, uvcMakeReads() cleared it, or
   old tables. Return the JPEG's length; 0 if the frame is not to be decoded, left as it was. */
ULONG uvcJpegFill(UBYTE *buffer, ULONG len)
{
    static const UBYTE comment[] = { 0xff, 0xd8, 0xff, 0xfe, (MJPEG_STD_DHT_SIZE - 2) >> 8, (MJPEG_STD_DHT_SIZE - 2) & 0xff };
    const UBYTE *frame = buffer + MJPEG_STD_DHT_SIZE;
    ULONG end = jpegLength(frame, len);

    if(!end)
    {
        return 0;
    }
    if(mjpegMissingDHT(frame, end))
    {
        buffer[0] = 0xff;
        buffer[1] = 0xd8;
        memcpy(&buffer[2], MJPEGStdDHT, MJPEG_STD_DHT_SIZE);
    } else {
        memcpy(buffer, comment, sizeof(comment));
    }
    return end + MJPEG_STD_DHT_SIZE;
}

/*
 * Files
 */

/* Return TRUE for formats whose frames are segments of one elementary
   stream, saved to a single file */
BOOL uvcIsStream(ULONG fourcc)
{
    return fourcc == UVFCC_H264;
}

/* Write the NUL-terminated file extension for a FourCC: "jpg" for MJPG,
   else the FourCC lowercased without its padding (yuy2, h264) */
static void extension(ULONG fourcc, UBYTE *ext)
{
    if(fourcc == UVFCC_MJPG)
    {
        strcpy((char *) ext, "jpg");
        return;
    }
    uvcFourCCText(fourcc, ext);
    ext[strcspn((const char *) ext, " ")] = 0;
    strlwr((char *) ext);
}

/* Build the file name for a frame number: <stem>-NNNN.<ext>, or
   <stem>.<ext> for a stream format. uvcOpenFiles() guarantees the result
   fits NAME_SIZE; snprintf() would cut it rather than overrun. */
static void fileName(const struct UvcFiles *fl, ULONG number, UBYTE *name)
{
    UBYTE ext[UVC_FOURCC_SIZE];

    extension(fl->fl_FourCC, ext);
    if(uvcIsStream(fl->fl_FourCC))
    {
        snprintf((char *) name, NAME_SIZE, "%s.%s", (char *) fl->fl_Stem, (char *) ext);
    } else {
        snprintf((char *) name, NAME_SIZE, "%s-%04lu.%s", (char *) fl->fl_Stem, number, (char *) ext);
    }
}

/* Write len bytes; return TRUE if all were written */
static BOOL writeAll(BPTR fh, const UBYTE *data, ULONG len)
{
    return Write(fh, (APTR) data, len) == (LONG) len;
}

/* Write one frame to an open file. An MJPEG frame without DHT gets the
   standard Huffman tables inserted before SOS (MJPEG cameras omit them,
   JPEG decoders need them); other formats are written unchanged. */
static BOOL writeFrameData(BPTR fh, ULONG fourcc, const UBYTE *frame, ULONG len)
{
    ULONG dhtpos = (fourcc == UVFCC_MJPG) ? mjpegMissingDHT(frame, len) : 0;

    if(!dhtpos)
    {
        return writeAll(fh, frame, len);
    }
    return writeAll(fh, frame, dhtpos) && writeAll(fh, MJPEGStdDHT, MJPEG_STD_DHT_SIZE) &&
           writeAll(fh, frame + dhtpos, len - dhtpos);
}

/* Initialise the output; open the file now for a stream format. A NULL
   stem disables saving. If the stem is too long for NAME_SIZE or the stream
   file cannot be opened, print the reason, leave saving disabled and
   return FALSE. */
BOOL uvcOpenFiles(struct UvcFiles *fl, STRPTR stem, ULONG fourcc)
{
    fl->fl_Stem = NULL;
    fl->fl_FourCC = fourcc;
    fl->fl_Stream = 0;
    fl->fl_Saved = 0;
    if(!stem)
    {
        return TRUE;
    }
    if(strlen(stem) + ENDING_SIZE > NAME_SIZE)
    {
        uvcReport("The name '%s' is too long", stem);
        return FALSE;
    }
    fl->fl_Stem = stem;
    if(!uvcIsStream(fourcc))
    {
        return TRUE;
    }

    UBYTE name[NAME_SIZE];

    fileName(fl, 0, name);
    if(!(fl->fl_Stream = Open(name, MODE_NEWFILE)))
    {
        uvcReport("Could not open '%s' for writing", name);
        fl->fl_Stem = NULL;
        return FALSE;
    }
    return TRUE;
}

/* Write the next frame to a new file, or append it to the stream file. On
   failure print the reason and return FALSE. */
BOOL uvcWriteFrame(struct UvcFiles *fl, const UBYTE *frame, ULONG len)
{
    UBYTE name[NAME_SIZE];

    if(!fl->fl_Stem)
    {
        return TRUE;
    }
    fileName(fl, fl->fl_Saved + 1, name);

    BPTR fh = fl->fl_Stream ? fl->fl_Stream : Open(name, MODE_NEWFILE);

    if(!fh)
    {
        uvcReport("Could not open '%s' for writing", name);
        return FALSE;
    }

    BOOL ok = writeFrameData(fh, fl->fl_FourCC, frame, len);

    if(!fl->fl_Stream)
    {
        Close(fh);
    }
    if(!ok)
    {
        uvcReport("Could not write '%s'", name);
        return FALSE;
    }
    fl->fl_Saved++;
    return TRUE;
}

/* Close the stream file, if open */
void uvcCloseFiles(struct UvcFiles *fl)
{
    if(fl->fl_Stream)
    {
        Close(fl->fl_Stream);
        fl->fl_Stream = 0;
    }
}

/* Print the number of frames saved and the first..last file name; print
   nothing if no frame was saved */
void uvcPrintSaved(const struct UvcFiles *fl)
{
    if(!fl->fl_Saved)
    {
        return;
    }

    UBYTE first[NAME_SIZE];

    fileName(fl, 1, first);
    Printf("%ld %s saved as %s", fl->fl_Saved, (fl->fl_Saved == 1) ? "frame" : "frames", first);
    if(!uvcIsStream(fl->fl_FourCC) && (fl->fl_Saved > 1))
    {
        UBYTE last[NAME_SIZE];

        fileName(fl, fl->fl_Saved, last);
        Printf(" ... %s", last);
    }
    PutStr("\n");
}
