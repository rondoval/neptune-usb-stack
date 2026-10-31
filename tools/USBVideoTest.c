/*
 *----------------------------------------------------------------------------
 *                          USBVideoTest - UVC test bench for Neptune
 *----------------------------------------------------------------------------
 * Test bench for usbvideo.usbclass via usbvideo.device: mode negotiation,
 * capture with per-frame validation, controls, status events and the VHI
 * driver. Every test prints PASS or FAIL. Built with the stack, not
 * shipped; the user tool is USBVideo.
 *
 *   USBVideoTest                   unit 0: camera info and mode table
 *   USBVideoTest UNIT=1            another unit
 *   USBVideoTest WIDTH=1280 HEIGHT=720  negotiate that size at its default rate
 *   USBVideoTest WIDTH=640 HEIGHT=480 FPS=15
 *   USBVideoTest PROBE             negotiate every mode in the table
 *   USBVideoTest FRAMES=300        capture 300 frames in the default mode (or
 *                                  WIDTH/HEIGHT/FPS); check count, rate and
 *                                  frame contents
 *   USBVideoTest FRAMES=10 TO=RAM:cam   ... and save them as RAM:cam-0001.jpg ...
 *   USBVideoTest FORMAT=YUY2 FRAMES=10  capture in a format other than the default
 *                                  (MJPG if available). Uncompressed: every
 *                                  frame must be exactly uvq_MaxFrameSize,
 *                                  saved raw as RAM:cam-0001.yuy2. H264: no
 *                                  sequence gap allowed, saved as one stream,
 *                                  RAM:cam.h264
 *   USBVideoTest FORMAT=H264 BITRATE=1000000 KEYFRAME=2000 FRAMES=300
 *                                  ... with encoder bit rate (bit/s) and key
 *                                  frame interval (ms). Only H264 modes of a
 *                                  camera with an H.264 extension unit accept
 *                                  them (<devices/usb_video.h>)
 *   USBVideoTest CYCLES=20         20 x start, capture FRAMES (default 10),
 *                                  stop; then close the unit while streaming,
 *                                  reopen and capture again
 *   USBVideoTest CONTROLS          print entity topology and named controls;
 *                                  write one control, read back, restore
 *   USBVideoTest EVENTS=20         while streaming: write each asynchronous
 *                                  control and expect its status event, toggle
 *                                  each auto mode and back, then listen 20 s
 *   USBVideoTest VHI               drive LIBS:VHI/neptune.vhi as a VHI client:
 *                                  check the offered sizes against the mode
 *                                  table, then grab FRAMES pictures (default
 *                                  30) at the size nearest WIDTH x HEIGHT
 *                                  (default mode if omitted); TO saves them.
 *                                  The driver delivers JPEG, or YUY2 if the
 *                                  camera has no MJPG; YUY2 is not saved
 *   USBVideoTest PREVIEW           run the stream process of USBVideo: FRAMES
 *                                  (default 300) through the decoder into its
 *                                  window, which moves and changes size on the
 *                                  way; the pictures must keep up. In the
 *                                  mode of WIDTH/HEIGHT/FPS/FORMAT, TO saves
 *                                  too. With CYCLES=n, then n x start, 30
 *                                  frames, stop, and all memory must be back
 *
 * Device API only, no neptune.library calls.
 */

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/videocodec.h>

#include <dos/dos.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/usb_video.h>
#include <devices/usbvideodevice.h>
#include <libraries/videocodec.h>
#include <vhi/vhi.h>

#include <inline/macros.h>
#include <string.h>
#include <neptune_version.h>

#include "uvc_client.h"
#include "uvc_preview.h"
#include "mjpegdht.h"

/* ReadArgs indices, in template order */
enum
{
    ARGS_UNIT,
    ARGS_PROBE,
    ARGS_WIDTH,
    ARGS_HEIGHT,
    ARGS_FPS,
    ARGS_FRAMES,
    ARGS_TO,
    ARGS_CYCLES,
    ARGS_VHI,
    ARGS_CONTROLS,
    ARGS_EVENTS,
    ARGS_FORMAT,
    ARGS_BITRATE,
    ARGS_KEYFRAME,
    ARGS_PREVIEW,
    ARGS_SIZEOF
};

#define TICKS_PER_SEC 50
#define PREVIEW_FRAMES       300 /* frames of a preview test */
#define PREVIEW_CYCLE_FRAMES 30  /* frames of each start and stop */
#define PREVIEW_POLL_TICKS   5
#define PREVIEW_SLOW_FPS     5   /* a camera in the dark: what the time limit allows for */
#define PREVIEW_TOLERATED    10  /* in 100 pictures, those that may be missing at the end: the decoder's lag */
#define PREVIEW_MEMORY_SLACK 65536 /* bytes not back after the cycles: what others may have taken */

const char USBVideoTest_prgname[] = "USBVideoTest";
static const char *template = "UNIT/N/K,PROBE/S,WIDTH/N/K,HEIGHT/N/K,FPS/N/K,FRAMES/N/K,TO/K,CYCLES/N/K,VHI/S,CONTROLS/S,EVENTS/N/K,FORMAT/K,BITRATE/N/K,KEYFRAME/N/K,PREVIEW/S";
const char USBVideoTest_version[] = NEP_VER("USBVideoTest");
static LONG ArgsArray[ARGS_SIZEOF];

/* Set the mode asked for in uvq (NSCMD_UV_SETMODE), print the negotiated mode
   and leave it in uvq. Its bit rate and key interval are encoder requests,
   0 = none; fail if one is given and the mode's encoder ignores it.
   TRUE = negotiated. */
static BOOL setMode(struct IOStdReq *ioreq, struct UsbVideoModeReq *uvq)
{
    ULONG bitrate = uvq->uvq_BitRate;
    ULONG keyinterval = uvq->uvq_KeyInterval;
    LONG err;

    uvq->uvq_MaxFrameSize = 0;
    if((err = uvcCommand(ioreq, NSCMD_UV_SETMODE, uvq, sizeof(*uvq))))
    {
        Printf("FAIL: %s (%ld)\n", uvcErrorText(err), err);
        return FALSE;
    }
    uvcPrintMode(uvq);
    Printf(", frames up to %ld bytes\n", uvq->uvq_MaxFrameSize);
    if((bitrate && !uvq->uvq_BitRate) || (keyinterval && !uvq->uvq_KeyInterval))
    {
        PutStr("FAIL: BITRATE and KEYFRAME are not for this mode: its encoder takes no such wish\n");
        return FALSE;
    }
    return TRUE;
}

/* Negotiate every mode in the table at its default interval. TRUE = all
   passed. */
static BOOL probeModes(struct IOStdReq *ioreq, struct UsbVideoMode *table, ULONG tablesize)
{
    ULONG passed = 0;
    ULONG failed = 0;

    UVC_EACH_MODE(uvm, table, tablesize)
    {
        struct UsbVideoModeReq uvq = { .uvq_FourCC = uvm->uvm_FourCC, .uvq_Width = uvm->uvm_Width, .uvq_Height = uvm->uvm_Height };

        Printf("  %ldx%ld: ", (ULONG) uvm->uvm_Width, (ULONG) uvm->uvm_Height);
        if(setMode(ioreq, &uvq))
        {
            passed++;
        } else {
            failed++;
        }
        if(SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        {
            break;
        }
    }
    Printf("\n%ld modes negotiated, %ld failed: %s\n", passed, failed, (passed && !failed) ? "PASS" : "FAIL");
    return passed && !failed;
}

/* Return the current time in ticks (1/50 s) */
static ULONG ticksNow(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG) ds.ds_Days * (24 * 60 * 60 * TICKS_PER_SEC) +
           (ULONG) ds.ds_Minute * (60 * TICKS_PER_SEC) + (ULONG) ds.ds_Tick;
}

/* Return the total payload of UVCX_MUX_MARKER APP segments before SOS: a
   second stream (H.264) muxed into an MJPEG frame, see
   <devices/usb_video.h>. 0 for a plain JPEG. */
static ULONG embeddedStreamBytes(const UBYTE *jpeg, ULONG len)
{
    ULONG pos = 2;
    ULONG inside = 0;

    if((len < 4) || (jpeg[0] != 0xff) || (jpeg[1] != 0xd8))
    {
        return 0;
    }
    while((pos + 4 <= len) && (jpeg[pos] == 0xff) && (jpeg[pos + 1] != 0xda))
    {
        ULONG seglen = (jpeg[pos + 2] << 8) | jpeg[pos + 3];

        if((jpeg[pos + 1] == UVCX_MUX_MARKER) && (seglen >= 2))
        {
            inside += seglen - 2;
        }
        pos += 2 + seglen;
    }
    return inside;
}

/* Print the NSCMD_UV_GETSTATS counters (since CMD_START) */
static void printDeviceStats(struct IOStdReq *ioreq)
{
    struct UsbVideoStats uvs;

    if(!uvcCommand(ioreq, NSCMD_UV_GETSTATS, &uvs, sizeof(uvs)))
    {
        Printf("the camera started %ld frames: %ld delivered, %ld with no read waiting, %ld damaged, "
               "%ld longer than the buffer\n",
               uvs.uvs_Frames, uvs.uvs_Delivered, uvs.uvs_Skipped, uvs.uvs_Damaged, uvs.uvs_Overflowed);
        Printf("transfers: %ld lost on the bus, %ld without a valid header, %ld flagged by the camera\n",
               uvs.uvs_LostIntervals, uvs.uvs_BadPayloads, uvs.uvs_CameraErrors);
    }
}

/* Capture frames frames and validate each against its format; print rate,
   size, timestamp and loss statistics. brief = print the result line only.
   TRUE = PASS. */
static BOOL captureFrames(struct UvcCamera *cm, struct UsbVideoModeReq *uvq, ULONG frames, STRPTR stem, BOOL brief)
{
    struct UvcCapture cp;
    struct IOVideoReq *ivr;
    ULONG got = 0;
    ULONG notjpeg = 0;
    ULONG wrongsize = 0;
    ULONG empty = 0;
    ULONG withtables = 0;
    ULONG bytes = 0;
    ULONG smallest = 0xffffffff;
    ULONG largest = 0;
    ULONG firstseq = 0;
    ULONG lastseq = 0;
    ULONG stamped = 0;
    ULONG firstpts = 0;
    ULONG lastpts = 0;
    ULONG firsttick = 0;
    ULONG lasttick = 0;
    /* validation by format: MJPG = complete JPEG; stream (H264) = start code
       and key frame count; VP8 = variable size, not checked; any other =
       fixed size uvq_MaxFrameSize */
    BOOL isjpeg = (uvq->uvq_FourCC == UVFCC_MJPG);
    BOOL isstream = uvcIsStream(uvq->uvq_FourCC);
    BOOL isfixed = !isjpeg && !isstream && (uvq->uvq_FourCC != UVFCC_VP80);
    BOOL nostart = FALSE;
    ULONG keyframes = 0;
    ULONG embedded = 0;
    ULONG embeddedbytes = 0;
    ULONG paired = 0;
    ULONG pairticks = 0;
    ULONG pairms = 0;
    ULONG prevclock = 0;
    ULONG prevframe = 0;

    if(uvcStartCapture(&cp, cm, uvq, stem))
    {
        while((got < frames) && (ivr = uvcNextFrame(&cp)))
        {
            UBYTE *jpeg = (UBYTE *) ivr->ivr_Std.io_Data;
            ULONG len = ivr->ivr_Std.io_Actual;

            lasttick = ticksNow();
            if(!got)
            {
                firsttick = lasttick;
                firstseq = ivr->ivr_Sequence;
                firstpts = ivr->ivr_PTS;
            }
            lastseq = ivr->ivr_Sequence;
            lastpts = ivr->ivr_PTS;
            if(ivr->ivr_Flags & IVRF_PTS)
            {
                stamped++;
            }
            if(ivr->ivr_Flags & IVRF_SCR)
            {
                /* SCR pairs: accumulate camera clock ticks against elapsed
                   USB frames (1 ms each) between consecutive pairs. The
                   11-bit SOF counter wraps every 2048 ms, hence the masked
                   delta; stop before pairticks overflows. */
                if(paired && (pairticks < 0x80000000))
                {
                    pairticks += ivr->ivr_SCRClock - prevclock;
                    pairms += (ivr->ivr_SCRFrame - prevframe) & UVPH_SCR_FRAME_MASK;
                }
                prevclock = ivr->ivr_SCRClock;
                prevframe = ivr->ivr_SCRFrame;
                paired++;
            }
            got++;
            bytes += len;
            if(len < smallest)
            {
                smallest = len;
            }
            if(len > largest)
            {
                largest = len;
            }
            if(isjpeg)
            {
                ULONG inside = embeddedStreamBytes(jpeg, len);

                embedded += inside ? 1 : 0;
                embeddedbytes += inside;
            }
            if(isfixed)
            {
                wrongsize += (len != uvq->uvq_MaxFrameSize) ? 1 : 0;
            }
            else if(!isjpeg)
            {
                /* Annex B: the first frame must begin with a 00 00 01 or
                   00 00 00 01 start code; later frames are not checked */
                if(isstream && (got == 1) &&
                   ((len < 4) || jpeg[0] || jpeg[1] || ((jpeg[2] != 1) && (jpeg[2] || (jpeg[3] != 1)))))
                {
                    nostart = TRUE;
                }
                if(isstream)
                {
                    keyframes += uvcH264Key(jpeg, len);
                }
            }
            else if((len < 4) || (jpeg[0] != 0xff) || (jpeg[1] != 0xd8) ||
                    (jpeg[len - 2] != 0xff) || (jpeg[len - 1] != 0xd9))
            {
                notjpeg++;
            }
            else if(!mjpegFrameHeader(jpeg, len))
            {
                empty++; /* SOI/EOI present but no SOF */
            }
            else if(!mjpegMissingDHT(jpeg, len))
            {
                withtables++;
            }
        }
    }
    uvcStopCapture(&cp);

    /* overflowed frames count as missed: they were not delivered intact */
    ULONG overflows = cp.cp_TooLong;
    ULONG missed = cp.cp_Missed + cp.cp_TooLong;
    LONG fatal = cp.cp_Error;
    BOOL interrupted = cp.cp_Interrupted;
    BOOL saved = !cp.cp_NotSaved;

    if(!got)
    {
        if(!saved)
        {
            PutStr("FAIL: could not save\n");
        } else {
            Printf("No frame arrived%s\nFAIL\n", fatal ? " (the stream ended)" : "");
        }
        return FALSE;
    }
    {
        ULONG ticks = lasttick - firsttick;
        /* fps x 10, measured from the first frame to the last */
        ULONG rate10 = ticks ? (got - 1) * 10 * TICKS_PER_SEC / ticks : 0;
        ULONG want10 = uvq->uvq_Interval ? 10 * UVC_INTERVAL_UNITS / uvq->uvq_Interval : 0;
        STRPTR why = NULL;

        Printf("%ld frames in %ld.%ld s: %ld.%ld per second (negotiated %ld.%ld); %ld missed in between\n",
               got, ticks / TICKS_PER_SEC, (ticks % TICKS_PER_SEC) * 10 / TICKS_PER_SEC,
               rate10 / 10, rate10 % 10, want10 / 10, want10 % 10, missed);
        if(!brief)
        {
            struct UsbVideoInfo uvi;

            if(isjpeg)
            {
                Printf("%ld..%ld bytes, average %ld; %ld not a whole JPEG picture, %ld without a picture in it, "
                       "%ld longer than the buffer; Huffman tables %s\n",
                       smallest, largest, bytes / got, notjpeg, empty, overflows,
                       (withtables == got - notjpeg - empty) ? "in every frame" : (withtables ? "in some frames" : "left out by the camera"));
                if(embedded)
                {
                    Printf("%ld of them carry another stream inside, %ld bytes of it in all\n", embedded, embeddedbytes);
                }
            }
            else if(isfixed)
            {
                Printf("%ld..%ld bytes, a picture has %ld; %ld of another size, %ld longer than the buffer\n",
                       smallest, largest, uvq->uvq_MaxFrameSize, wrongsize, overflows);
            } else {
                Printf("%ld..%ld bytes, average %ld; %ld longer than the buffer\n",
                       smallest, largest, bytes / got, overflows);
                if(isstream && ticks)
                {
                    /* bit/s = bytes * 8 / seconds, split to stay within 32 bits */
                    Printf("%ld key frames; about %ld bits a second\n", keyframes,
                           (bytes / ticks) * (8 * TICKS_PER_SEC) + ((bytes % ticks) * (8 * TICKS_PER_SEC)) / ticks);
                }
            }
            /* frame rate by the camera clock: PTS delta over sequence delta */
            if(!uvcCommand(cm->cm_Request, NSCMD_UV_GETINFO, &uvi, sizeof(uvi)))
            {
                ULONG perframe = (lastseq > firstseq) ? (lastpts - firstpts) / (lastseq - firstseq) : 0;

                if(pairms)
                {
                    /* Hz = ticks per ms * 1000, split to stay within 32 bits */
                    Printf("clock pairs in %ld of them: by the camera's own count of USB frames its clock makes %ld Hz\n",
                           paired, (pairticks / pairms) * 1000 + ((pairticks % pairms) * 1000) / pairms);
                } else {
                    Printf("clock pairs in %ld of them\n", paired);
                }
                Printf("time stamps in %ld of them, clock %ld Hz", stamped, uvi.uvi_ClockFrequency);
                if((stamped == got) && perframe && uvi.uvi_ClockFrequency)
                {
                    ULONG pts10 = (uvi.uvi_ClockFrequency / perframe) * 10 +
                                  (uvi.uvi_ClockFrequency % perframe) * 10 / perframe;
                    Printf(": %ld.%ld frames per second by the camera's clock", pts10 / 10, pts10 % 10);
                }
                PutStr("\n");
            }
            printDeviceStats(cm->cm_Request);
            uvcPrintSaved(&cp.cp_Files);
        }
        if(interrupted)
        {
            why = "interrupted";
        }
        else if(!saved)
        {
            why = "could not save";
        }
        else if(fatal)
        {
            why = uvcErrorText(fatal);
        }
        else if(got < frames)
        {
            why = "the stream ended early";
        }
        else if(notjpeg)
        {
            why = "frames that are not whole JPEG pictures";
        }
        else if(wrongsize)
        {
            why = "frames that do not have the size of their picture";
        }
        else if(nostart)
        {
            why = "the stream does not begin with a start code";
        }
        else if(isstream && missed)
        {
            why = "frames missed: the stream has a hole";
        }
        else if(overflows)
        {
            why = "frames longer than the negotiated size";
        }
        else if(missed * 100 > got)
        {
            why = "more than 1% of the frames missed";
        }
        else if((got > 30) && (rate10 * 10 > want10 * 11))
        {
            why = "frame rate more than 10% above the negotiated one";
        }
        /* a rate below the negotiated one is not a failure: with no frames
           lost, the camera itself is slow (long exposure in low light) */
        else if((got > 30) && (rate10 * 10 < want10 * 9) && !brief)
        {
            PutStr("The camera makes fewer frames than negotiated - a long exposure does that.\n");
        }
        if(why)
        {
            Printf("FAIL: %s\n", why);
            return FALSE;
        }
        if(!brief)
        {
            PutStr("PASS\n");
        }
    }
    return TRUE;
}

/* Run cycles x start/capture/stop. Then CMD_START and CloseDevice() with
   the stream running: the class must stop it. Reopen (retrying while
   IOERR_UNITBUSY), check the mode was reset (CMD_START gives UVERR_NOMODE),
   renegotiate and capture. Closes and reopens cm_Request directly; cm_Open
   is valid on return. TRUE = PASS. */
static BOOL startStopCycles(struct UvcCamera *cm, struct UsbVideoModeReq *uvq, ULONG cycles, ULONG frames)
{
    struct IOStdReq *ioreq = cm->cm_Request;
    struct UsbVideoModeReq asked = *uvq;
    ULONG waited = 0;
    LONG err;

    for(ULONG cnt = 1; cnt <= cycles; cnt++)
    {
        Printf("cycle %ld: ", cnt);
        if(!captureFrames(cm, uvq, frames, NULL, TRUE))
        {
            return FALSE;
        }
        if(SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        {
            PutStr("FAIL: interrupted\n");
            return FALSE;
        }
    }

    if((err = uvcCommand(ioreq, CMD_START, NULL, 0)))
    {
        Printf("FAIL: could not start the stream once more: %s (%ld)\n", uvcErrorText(err), err);
        return FALSE;
    }
    CloseDevice((struct IORequest *) ioreq);
    cm->cm_Open = FALSE;
    /* IOERR_UNITBUSY until the class has stopped the orphaned stream */
    while((err = OpenDevice(USBVIDEO_NAME, cm->cm_Unit, (struct IORequest *) ioreq, 0)) == IOERR_UNITBUSY)
    {
        if(++waited > 5 * TICKS_PER_SEC)
        {
            break;
        }
        Delay(1);
    }
    if(err)
    {
        Printf("FAIL: the unit closed with its stream running did not open again (%ld)\n", err);
        return FALSE;
    }
    cm->cm_Open = TRUE;
    Printf("closed with the stream running; open again after %ld ms\n", waited * (1000 / TICKS_PER_SEC));
    if((err = uvcCommand(ioreq, CMD_START, NULL, 0)) != UVERR_NOMODE)
    {
        Printf("FAIL: a newly opened unit started without a mode (%ld)\n", err);
        return FALSE;
    }
    PutStr("after that: ");
    *uvq = asked;
    if(!setMode(ioreq, uvq))
    {
        return FALSE;
    }
    PutStr("            ");
    if(!captureFrames(cm, uvq, frames, NULL, TRUE))
    {
        return FALSE;
    }
    Printf("%ld cycles\nPASS\n", cycles);
    return TRUE;
}

/*
 * ***********************************************************************
 * * VHI driver, exercised as a VHI client                               *
 * ***********************************************************************
 */

#define VHI_DRIVER "LIBS:VHI/neptune.vhi"

/* The VHI driver's single entry point (LVO -30) */
#define vhi_api(method, submethod, attr, errcode, handle) \
    LP5(0x1e, ULONG, vhi_api, ULONG, method, d2, ULONG, submethod, d3, APTR, attr, d4, \
        ULONG *, errcode, d5, APTR, handle, d6, , VHIBase)

/* Client allocator for vhi_digitize CstAllocVec/CstFreeVec; VhiAllocs
   counts the allocations */
static ULONG VhiAllocs;

static APTR programAllocVec(ULONG size, ULONG flags)
{
    VhiAllocs++;
    return AllocVec(size, flags);
}

static void programFreeVec(APTR mem)
{
    FreeVec(mem);
}

/* Print a VHI_METHOD_GET string and free it (the driver allocates it) */
static void printVhiString(struct Library *VHIBase, APTR handle, STRPTR what, ULONG submethod, ULONG attr)
{
    ULONG vhierr = 0;
    STRPTR str = (STRPTR) vhi_api(VHI_METHOD_GET, submethod, (APTR) attr, &vhierr, handle);

    Printf("%s%s\n", what, str ? str : (STRPTR) "(none)");
    FreeVec(str);
}

/* Return TRUE if jpeg is a complete JPEG file (EOI, DHT present, SOF
   found), and its SOF picture size in width and height */
static BOOL completeJpegSize(const UBYTE *jpeg, ULONG len, ULONG *width, ULONG *height)
{
    ULONG pos;

    if(!jpeg || (len < 4) || (jpeg[len - 2] != 0xff) || (jpeg[len - 1] != 0xd9) || mjpegMissingDHT(jpeg, len) ||
       !(pos = mjpegFrameHeader(jpeg, len)) || (pos + 9 > len))
    {
        return FALSE;
    }
    *height = (jpeg[pos + 5] << 8) | jpeg[pos + 6];
    *width = (jpeg[pos + 7] << 8) | jpeg[pos + 8];
    return TRUE;
}

#define MODE_AREA(uvm) ((ULONG) (uvm)->uvm_Width * (uvm)->uvm_Height)

/* Grab frames pictures through the driver at askwidth x askheight, passed
   unchecked as a client would (0 x 0 = default mode def). Alternate client
   and system allocation. Each picture must be a complete JPEG (driver uses
   MJPG) or three YUV planes, at the size VHI_CHECK_DIGITIZE_SIZE reported.
   Return the failure reason, NULL on success. */
static STRPTR takeVhiPictures(struct Library *VHIBase, APTR handle, ULONG askwidth, ULONG askheight,
                              const struct UsbVideoMode *def, ULONG frames, STRPTR stem)
{
    BOOL isjpeg = (def->uvm_FourCC == UVFCC_MJPG);
    struct vhi_digitize dig;
    struct UvcFiles files;
    ULONG vhierr = 0;
    ULONG got = 0;
    ULONG bad = 0;
    ULONG custom = 0;
    ULONG bytes = 0;
    ULONG firsttick = 0;
    ULONG lasttick = 0;
    STRPTR why = NULL;

    memset(&dig, 0, sizeof(dig));
    dig.dim.dst_width = askwidth;
    dig.dim.dst_height = askheight;
    vhi_api(VHI_METHOD_PERFORM, VHI_CHECK_DIGITIZE_SIZE, &dig.dim, &vhierr, handle);
    if(vhierr)
    {
        Printf("checking %ldx%ld: VHI error %ld\n", askwidth, askheight, vhierr);
        return "a size could not be checked";
    }
    ULONG wantwidth = dig.dim.dst_width ? dig.dim.dst_width : (ULONG) def->uvm_Width;
    ULONG wantheight = dig.dim.dst_height ? dig.dim.dst_height : (ULONG) def->uvm_Height;

    dig.dim.dst_width = askwidth;
    dig.dim.dst_height = askheight;
    dig.CstAllocVec = programAllocVec;
    dig.CstFreeVec = programFreeVec;
    VhiAllocs = 0;
    /* only JPEG pictures are saved, YUV planes are not */
    if(!uvcOpenFiles(&files, isjpeg ? stem : NULL, UVFCC_MJPG))
    {
        return "could not save";
    }
    while(!why && (got < frames))
    {
        struct vhi_image *img;
        ULONG width = 0;
        ULONG height = 0;

        /* alternate: system allocator first, then the client's */
        dig.custom_memhandling = (got & 1) ? TRUE : FALSE;
        img = (struct vhi_image *) vhi_api(VHI_METHOD_PERFORM, VHI_DIGITIZE_PICTURE, &dig, &vhierr, handle);
        if(!img)
        {
            Printf("picture %ld: VHI error %ld\n", got + 1, vhierr);
            why = "a picture was not delivered";
            break;
        }
        UBYTE *jpeg = (UBYTE *) img->chunky;
        ULONG len = isjpeg ? img->width : img->width * img->height * 2;
        BOOL proper;

        lasttick = ticksNow();
        if(!got)
        {
            firsttick = lasttick;
        }
        got++;
        custom += dig.custom_memhandling ? 1 : 0;
        bytes += len;
        if(isjpeg)
        {
            proper = (img->type == VHI_JPEG) && !img->height && completeJpegSize(jpeg, len, &width, &height);
        } else {
            proper = (img->type == VHI_YUV_422) && img->y && img->u && img->v;
            width = img->width;
            height = img->height;
        }
        if(!proper)
        {
            bad++;
        }
        else if((width != wantwidth) || (height != wantheight))
        {
            Printf("picture %ld is %ldx%ld\n", got, width, height);
            why = "a picture did not have the size the driver said it would";
        }
        else if(!uvcWriteFrame(&files, jpeg, len))
        {
            why = "could not save";
        }
        FreeVec(jpeg);
        FreeVec(img->y);
        FreeVec(img->u);
        FreeVec(img->v);
        FreeVec(img);
        if(SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        {
            why = "interrupted";
        }
    }

    if(askwidth || askheight)
    {
        Printf("%ldx%ld asked: ", askwidth, askheight);
    } else {
        PutStr("no size asked: ");
    }
    Printf("%ld pictures of %ldx%ld", got, wantwidth, wantheight);
    if(got > 1)
    {
        ULONG ticks = lasttick - firsttick;
        ULONG rate10 = ticks ? (got - 1) * 10 * TICKS_PER_SEC / ticks : 0;

        Printf(" in %ld.%ld s: %ld.%ld per second", ticks / TICKS_PER_SEC,
               (ticks % TICKS_PER_SEC) * 10 / TICKS_PER_SEC, rate10 / 10, rate10 % 10);
    }
    Printf("; average %ld bytes; %ld not a %s\n", got ? bytes / got : 0, bad,
           isjpeg ? "complete JPEG file" : "picture in three planes");
    uvcCloseFiles(&files);
    uvcPrintSaved(&files);
    if(!why && bad)
    {
        why = isjpeg ? "pictures that are not complete JPEG files" : "pictures that are not three planes";
    }
    /* client allocations per picture: vhi_image + JPEG data (2), or
       vhi_image + three planes (4) */
    if(!why && (VhiAllocs != custom * (isjpeg ? 2 : 4)))
    {
        why = "the program's allocator was not used as asked";
    }
    return why;
}

/* Open the driver, query it as a client does, check the sizes it offers
   against the mode table, grab pictures. TRUE = PASS. */
/* Return the mode with that FourCC and size, NULL if none; width = height
   = 0 matches any size */
static const struct UsbVideoMode *findMode(const struct UsbVideoMode *table, ULONG tablesize, ULONG fourcc, ULONG width, ULONG height)
{
    UVC_EACH_MODE(uvm, table, tablesize)
    {
        if((uvm->uvm_FourCC == fourcc) &&
           (!(width || height) || ((uvm->uvm_Width == width) && (uvm->uvm_Height == height))))
        {
            return uvm;
        }
    }
    return NULL;
}

static BOOL testVhiDriver(ULONG input, const struct UsbVideoMode *table, ULONG tablesize,
                          ULONG width, ULONG height, ULONG frames, STRPTR stem)
{
    struct Library *VHIBase;
    struct vhi_size size = { 0, 0, FALSE, FALSE };
    const struct UsbVideoMode *smallest = NULL;
    const struct UsbVideoMode *largest = NULL;
    const struct UsbVideoMode *def = NULL;
    APTR handle;
    ULONG vhierr = 0;
    STRPTR why = NULL;

    /* the driver uses the MJPG modes, or YUY2 if the camera has no MJPG */
    ULONG fourcc = findMode(table, tablesize, UVFCC_MJPG, 0, 0) ? UVFCC_MJPG : UVFCC_YUY2;

    UVC_EACH_MODE(uvm, table, tablesize)
    {
        if(uvm->uvm_FourCC != fourcc)
        {
            continue;
        }
        if(!smallest || (MODE_AREA(uvm) < MODE_AREA(smallest)))
        {
            smallest = uvm;
        }
        if(!largest || (MODE_AREA(uvm) > MODE_AREA(largest)))
        {
            largest = uvm;
        }
        /* default: the UVMF_DEFAULT mode, else the first */
        if(!def || ((uvm->uvm_Flags & UVMF_DEFAULT) && !(def->uvm_Flags & UVMF_DEFAULT)))
        {
            def = uvm;
        }
    }
    if(!def)
    {
        PutStr("FAIL: the camera has no MJPG and no YUY2 mode\n");
        return FALSE;
    }

    if(!(VHIBase = OpenLibrary(VHI_DRIVER, 0)))
    {
        PutStr("FAIL: could not open " VHI_DRIVER "\n");
        return FALSE;
    }
    if(!(handle = (APTR) vhi_api(VHI_METHOD_OPEN, 0, NULL, &vhierr, NULL)))
    {
        Printf("FAIL: the driver did not open: VHI error %ld%s\n", vhierr,
               (vhierr == VHI_ERR_NO_HARDWARE) ? " (no camera)" : "");
        CloseLibrary(VHIBase);
        return FALSE;
    }

    Printf("%s %ld.%ld\n", VHI_DRIVER,
           vhi_api(VHI_METHOD_GET, VHI_CARD_DRIVERVERSION, NULL, &vhierr, handle),
           vhi_api(VHI_METHOD_GET, VHI_CARD_DRIVERREVISION, NULL, &vhierr, handle));
    ULONG inputs = vhi_api(VHI_METHOD_GET, VHI_NUMBER_OF_INPUTS, NULL, &vhierr, handle);
    for(ULONG cnt = 0; cnt < inputs; cnt++)
    {
        Printf("input %ld: ", cnt);
        printVhiString(VHIBase, handle, "", VHI_NAME_OF_INPUT, cnt);
    }
    if(input)
    {
        struct vhi_setoptions opt = { VHI_OPT_INPUT, input };

        if(vhi_api(VHI_METHOD_SET, VHI_OPTIONS, &opt, &vhierr, handle) || vhierr)
        {
            Printf("FAIL: input %ld could not be selected: VHI error %ld\n", input, vhierr);
            vhi_api(VHI_METHOD_CLOSE, 0, NULL, &vhierr, handle);
            CloseLibrary(VHIBase);
            return FALSE;
        }
    }
    printVhiString(VHIBase, handle, "camera: ", VHI_CARD_NAME, 0);
    printVhiString(VHIBase, handle, "made by: ", VHI_CARD_MANUFACTURER, 0);
    printVhiString(VHIBase, handle, "driver by: ", VHI_CARD_DRIVERAUTHOR, 0);
    vhi_api(VHI_METHOD_GET, VHI_MAXIMUM_SIZE, &size, &vhierr, handle);
    Printf("maximum size %ldx%ld%s%s\n", size.max_width, size.max_height,
           size.fixed ? ", no cropping" : "", size.scalable ? ", scalable" : "");

    /* an unknown method must fail with VHI_ERR_UNKNOWN_METHOD */
    vhi_api(VHI_METHOD_GET, 999, NULL, &vhierr, handle);
    if(vhierr != VHI_ERR_UNKNOWN_METHOD)
    {
        why = "an unknown request was not refused";
    }

    /* maximum size = largest mode, reported scalable and fixed (no cropping) */
    if(!why && ((size.max_width != largest->uvm_Width) || (size.max_height != largest->uvm_Height) ||
                !size.scalable || !size.fixed))
    {
        why = "the maximum size is not the camera's largest picture, scalable and whole";
    }

    /* every mode size must be accepted unchanged, with a full-frame crop */
    UVC_EACH_MODE(uvm, table, tablesize)
    {
        struct vhi_dimensions dim = { 5, 5, 9, 9, uvm->uvm_Width, uvm->uvm_Height };

        if(uvm->uvm_FourCC != fourcc)
        {
            continue;
        }
        vhi_api(VHI_METHOD_PERFORM, VHI_CHECK_DIGITIZE_SIZE, &dim, &vhierr, handle);
        if(vhierr || (dim.dst_width != uvm->uvm_Width) || (dim.dst_height != uvm->uvm_Height) ||
           dim.x1 || dim.y1 || (dim.x2 != size.max_width - 1) || (dim.y2 != size.max_height - 1))
        {
            Printf("%ldx%ld checked: VHI error %ld, (%ld,%ld)-(%ld,%ld) at %ldx%ld\n",
                   (ULONG) uvm->uvm_Width, (ULONG) uvm->uvm_Height, vhierr,
                   dim.x1, dim.y1, dim.x2, dim.y2, dim.dst_width, dim.dst_height);
            why = "a picture size of the camera was not offered as it is";
            break;
        }
    }

    /* 0 x 0 must stay 0 x 0; any other request must map to a mode size */
    static const UWORD asked[][2] = { { 0, 0 }, { 1, 1 }, { 300, 300 }, { 1000, 700 }, { 30000, 30000 } };

    for(ULONG cnt = 0; !why && (cnt < sizeof(asked) / sizeof(asked[0])); cnt++)
    {
        struct vhi_dimensions dim = { 0, 0, 0, 0, asked[cnt][0], asked[cnt][1] };

        vhi_api(VHI_METHOD_PERFORM, VHI_CHECK_DIGITIZE_SIZE, &dim, &vhierr, handle);
        Printf("%ldx%ld asked gives %ldx%ld\n", (ULONG) asked[cnt][0], (ULONG) asked[cnt][1],
               dim.dst_width, dim.dst_height);
        if(vhierr || (asked[cnt][0] ? !findMode(table, tablesize, fourcc, dim.dst_width, dim.dst_height)
                                    : (dim.dst_width || dim.dst_height)))
        {
            why = "a size was answered with one the camera does not have";
        }
    }

    /* grab at the smallest size, the largest, then the requested size; each
       step is a mode switch */
    if(!why)
    {
        why = takeVhiPictures(VHIBase, handle, smallest->uvm_Width, smallest->uvm_Height, def, 1, NULL);
    }
    if(!why)
    {
        why = takeVhiPictures(VHIBase, handle, largest->uvm_Width, largest->uvm_Height, def, 1, NULL);
    }
    if(!why)
    {
        why = takeVhiPictures(VHIBase, handle, width, height, def, frames, stem);
    }
    vhi_api(VHI_METHOD_CLOSE, 0, NULL, &vhierr, handle);
    CloseLibrary(VHIBase);

    if(why)
    {
        Printf("FAIL: %s\n", why);
        return FALSE;
    }
    PutStr("PASS\n");
    return TRUE;
}

/* /// "Controls" */
#define MAX_CONTROLS 64 /* control list capacity */

/* Return a UVC request error code (bRequestErrorCode) as a ", ..." suffix */
static STRPTR reasonText(ULONG code)
{
    switch(code)
    {
        case UVREC_NO_ERROR:        return "";
        case UVREC_NOT_READY:       return ", the camera says: not ready";
        case UVREC_WRONG_STATE:     return ", the camera says: not in this state";
        case UVREC_POWER:           return ", the camera says: not in this power mode";
        case UVREC_OUT_OF_RANGE:    return ", the camera says: out of range";
        case UVREC_INVALID_UNIT:    return ", the camera says: no such unit";
        case UVREC_INVALID_CONTROL: return ", the camera says: no such control";
        case UVREC_INVALID_REQUEST: return ", the camera says: no such request";
        case UVREC_INVALID_VALUE:   return ", the camera says: invalid value";
    }
    return ", the camera gives no reason";
}

/* Enumerate the named controls (NSCMD_UV_NEXTCONTROL) into list. Return the
   count; *end = UVERR_NOCONTROL after the last control, 0 if the list filled
   up first. */
static ULONG listControls(struct IOStdReq *ioreq, struct UsbVideoControl *list, ULONG max, LONG *end)
{
    struct UsbVideoControl ctl;
    ULONG num = 0;

    memset(&ctl, 0, sizeof(ctl));
    while(!(*end = uvcCommand(ioreq, NSCMD_UV_NEXTCONTROL, &ctl, sizeof(ctl))) && (num < max))
    {
        list[num++] = ctl;
    }
    return num;
}

/* Read a control's current value and attributes (NSCMD_UV_GETCONTROL) */
static LONG readControl(struct IOStdReq *ioreq, struct UsbVideoControl *ctl)
{
    return uvcCommand(ioreq, NSCMD_UV_GETCONTROL, ctl, sizeof(*ctl));
}

/* Write a control value (NSCMD_UV_SETCONTROL). Return io_Error; on refusal
   uvc_Reason holds the camera's error code. */
static LONG writeControl(struct IOStdReq *ioreq, struct UsbVideoControl *ctl, LONG value)
{
    ctl->uvc_Value = value;
    return uvcCommand(ioreq, NSCMD_UV_SETCONTROL, ctl, sizeof(*ctl));
}

/* Print a control value: on/off, choice name, or number */
static void printControlValue(const struct UsbVideoControl *ctl, LONG value)
{
    if(ctl->uvc_Kind == UVCK_SWITCH)
    {
        PutStr(value ? "on" : "off");
    }
    else if((ctl->uvc_Kind == UVCK_CHOICE) && (value >= 0) && (value <= ctl->uvc_Max))
    {
        PutStr((STRPTR) ctl->uvc_ChoiceNames[value]);
    } else {
        Printf("%ld", value);
    }
}

/* Print a control: value, range, default, choices, entity and flags */
static void printControl(const struct UsbVideoControl *ctl)
{
    Printf("  %s: ", ctl->uvc_Name);
    if(!(ctl->uvc_Flags & UVCF_GET))
    {
        PutStr("cannot be read");
    } else {
        printControlValue(ctl, ctl->uvc_Value);
        if((ctl->uvc_Kind == UVCK_RANGE) && (ctl->uvc_Flags & UVCF_RANGE))
        {
            Printf(" (%ld to %ld, step %ld)", ctl->uvc_Min, ctl->uvc_Max, ctl->uvc_Step);
        }
        if(ctl->uvc_Flags & UVCF_DEFAULT)
        {
            PutStr(", default ");
            printControlValue(ctl, ctl->uvc_Default);
        }
        if(ctl->uvc_Kind == UVCK_CHOICE)
        {
            PutStr(", the camera has:");
            for(ULONG choice = 0; ctl->uvc_ChoiceNames[choice]; choice++)
            {
                if(ctl->uvc_Choices & (1UL << choice))
                {
                    Printf(" %s;", ctl->uvc_ChoiceNames[choice]);
                }
            }
        }
    }
    Printf(" [unit %ld, %s%s%s%s]\n", (ULONG) ctl->uvc_Entity,
           (ctl->uvc_Flags & UVCF_SET) ? "can be set" : "read only",
           (ctl->uvc_Flags & UVCF_INACTIVE) ? ", switched off by an automatic mode" : "",
           (ctl->uvc_Flags & UVCF_VOLATILE) ? ", changes by itself" : "",
           (ctl->uvc_Flags & UVCF_SLOW) ? ", asynchronous" : "");
}

/* Print an entity: type and ID, terminal type, extension unit GUID, sources */
static void printEntity(const struct UsbVideoEntity *uve)
{
    static const STRPTR types[] =
    {
        "Input terminal", "Output terminal", "Selector unit", "Processing unit", "Extension unit", "Encoding unit"
    };
    ULONG type = uve->uve_Type - UDST_VIDEO_CTRL_INPUT_TERMINAL;

    if((uve->uve_Type == UDST_VIDEO_CTRL_INPUT_TERMINAL) && (uve->uve_TermType == UVITT_CAMERA))
    {
        Printf("Camera terminal %ld", (ULONG) uve->uve_ID);
    } else {
        Printf("%s %ld", (type < sizeof(types) / sizeof(types[0])) ? types[type] : (STRPTR) "Entity",
               (ULONG) uve->uve_ID);
    }
    if(uve->uve_TermType && (uve->uve_TermType != UVITT_CAMERA))
    {
        Printf(", terminal type %04lx", (ULONG) uve->uve_TermType);
    }
    if(uve->uve_Type == UDST_VIDEO_CTRL_EXTENSION_UNIT)
    {
        const UBYTE *guid = uve->uve_GUID;

        Printf(" {%08lx-%04lx-%04lx-%02lx%02lx-%02lx%02lx%02lx%02lx%02lx%02lx}",
               AROS_LE2LONG(*(const ULONG *) guid), (ULONG) AROS_LE2WORD(*(const UWORD *) &guid[4]),
               (ULONG) AROS_LE2WORD(*(const UWORD *) &guid[6]),
               (ULONG) guid[8], (ULONG) guid[9], (ULONG) guid[10], (ULONG) guid[11],
               (ULONG) guid[12], (ULONG) guid[13], (ULONG) guid[14], (ULONG) guid[15]);
    }
    if(uve->uve_NumSources)
    {
        PutStr(", fed by");
        for(ULONG cnt = 0; cnt < uve->uve_NumSources; cnt++)
        {
            Printf(" %ld", (ULONG) uve->uve_Data[cnt]);
        }
    }
    PutStr("\n");
}

/* Count the named controls of an entity; several list entries for the same
   selector count once */
static ULONG countNamedControls(const struct UsbVideoControl *list, ULONG num, ULONG entity)
{
    ULONG named = 0;

    for(ULONG cnt = 0; cnt < num; cnt++)
    {
        BOOL counted = (list[cnt].uvc_Entity != entity);

        for(ULONG prev = 0; !counted && (prev < cnt); prev++)
        {
            counted = (list[prev].uvc_Entity == entity) && (list[prev].uvc_Selector == list[cnt].uvc_Selector);
        }
        named += counted ? 0 : 1;
    }
    return named;
}

/* Print the entity topology (NSCMD_UV_GETTOPOLOGY): sources, declared
   control count and how many are named. For extension units, query
   GET_LEN/GET_INFO of every declared selector; failures are added to
   *missing. FALSE = topology unreadable (message printed). */
static BOOL printTopology(struct IOStdReq *ioreq, struct UsbVideoInfo *uvi,
                          const struct UsbVideoControl *list, ULONG num, ULONG *missing)
{
    UBYTE *topology;
    UBYTE reason;
    LONG err;

    if(!uvi->uvi_TopologySize)
    {
        PutStr("FAIL: this usbvideo.usbclass does not list the camera's units - it is older than this tool\n");
        return FALSE;
    }
    if(!(topology = AllocVec(uvi->uvi_TopologySize, MEMF_ANY)))
    {
        PutStr("FAIL: out of memory\n");
        return FALSE;
    }
    if((err = uvcCommand(ioreq, NSCMD_UV_GETTOPOLOGY, topology, uvi->uvi_TopologySize)))
    {
        Printf("FAIL: could not read the camera's units: %s (%ld)\n", uvcErrorText(err), err);
        FreeVec(topology);
        return FALSE;
    }
    ULONG toposize = ioreq->io_Actual;

    for(ULONG pos = 0; pos < toposize; )
    {
        struct UsbVideoEntity *uve = (struct UsbVideoEntity *) (topology + pos);
        const UBYTE *bitmap = uve->uve_Data + uve->uve_NumSources;
        ULONG declared = 0;

        pos += uve->uve_Size;
        printEntity(uve);
        for(ULONG bit = 0; bit < (ULONG) uve->uve_ControlSize * 8; bit++)
        {
            UWORD lendata = 0;
            UBYTE info = 0;

            if(!((bitmap[bit / 8] >> (bit % 8)) & 1))
            {
                continue;
            }
            declared++;
            if(uve->uve_Type != UDST_VIDEO_CTRL_EXTENSION_UNIT)
            {
                continue;
            }
            /* vendor controls: only length and capabilities can be queried */
            Printf("  selector %ld: ", bit + 1);
            if(uvcRequest(ioreq, UVUDR_GET_LEN, uve->uve_ID, bit + 1, &lendata, sizeof(lendata), &reason) ||
               uvcRequest(ioreq, UVUDR_GET_INFO, uve->uve_ID, bit + 1, &info, 1, &reason))
            {
                Printf("no answer%s\n", reasonText(reason));
                (*missing)++;
            } else {
                Printf("%ld bytes [%s%s]\n", (ULONG) AROS_LE2WORD(lendata),
                       (info & UVGIF_GET) ? "can be read" : "cannot be read",
                       (info & UVGIF_SET) ? ", can be set" : "");
            }
        }
        if(declared && (uve->uve_Type != UDST_VIDEO_CTRL_EXTENSION_UNIT))
        {
            Printf("  %ld controls, %ld of them have a name below\n", declared, countNamedControls(list, num, uve->uve_ID));
        }
    }
    FreeVec(topology);
    return TRUE;
}

/* Return TRUE for a range control that is readable, writable, not inactive,
   with Max > Min */
static BOOL controlCanMove(const struct UsbVideoControl *ctl)
{
    return (ctl->uvc_Kind == UVCK_RANGE) && (ctl->uvc_Flags & UVCF_RANGE) && (ctl->uvc_Max > ctl->uvc_Min) &&
           ((ctl->uvc_Flags & (UVCF_GET|UVCF_SET|UVCF_INACTIVE)) == (UVCF_GET|UVCF_SET));
}

/* Return the current value plus one step, or minus one step at the top of
   the range */
static LONG nextControlValue(const struct UsbVideoControl *ctl)
{
    return (ctl->uvc_Value + ctl->uvc_Step <= ctl->uvc_Max) ? ctl->uvc_Value + ctl->uvc_Step
                                                           : ctl->uvc_Value - ctl->uvc_Step;
}

/* CONTROLS: print the topology and the named controls; write one picture
   control, read it back, restore it. TRUE = PASS. */
static BOOL testControls(struct IOStdReq *ioreq, struct UsbVideoInfo *uvi, const struct UsbVideoControl *list, ULONG num)
{
    const struct UsbVideoControl *test = NULL;
    ULONG failed = 0;
    ULONG missing = 0;

    if(!printTopology(ioreq, uvi, list, num, &missing))
    {
        return FALSE;
    }
    PutStr("\nControls by name:\n");
    for(ULONG cnt = 0; cnt < num; cnt++)
    {
        const struct UsbVideoControl *ctl = &list[cnt];

        printControl(ctl);
        if(!(ctl->uvc_Flags & UVCF_GET))
        {
            /* write-only controls are allowed; a refused GET_CUR on a listed
               control is a failure */
            if(ctl->uvc_Reason != UVREC_NO_ERROR)
            {
                Printf("  ! %s: the camera does not answer for it%s\n", ctl->uvc_Name, reasonText(ctl->uvc_Reason));
                failed++;
            }
            continue;
        }
        missing += (ctl->uvc_Flags & UVCF_DEFAULT) ? 0 : 1;
        missing += (ctl->uvc_Flags & UVCF_RANGE) ? 0 : 1;
        if((ctl->uvc_Kind == UVCK_RANGE) && (ctl->uvc_Flags & UVCF_RANGE) &&
           ((ctl->uvc_Value < ctl->uvc_Min) || (ctl->uvc_Value > ctl->uvc_Max)))
        {
            Printf("  ! %s: the value is outside its range\n", ctl->uvc_Name);
            failed++;
        }
        /* write test target: a picture control, never a motor (pan/tilt/focus) */
        if(!test && (UVCID_GROUP(ctl->uvc_ID) == UVCG_PICTURE) && controlCanMove(ctl))
        {
            test = ctl;
        }
    }

    if(test)
    {
        struct UsbVideoControl ctl = *test;
        LONG was = ctl.uvc_Value;
        LONG target = nextControlValue(&ctl);
        LONG there = was;
        LONG back = target;
        LONG err;

        Printf("\nWriting %s: %ld", ctl.uvc_Name, was);
        if(!(err = writeControl(ioreq, &ctl, target)) && !(err = readControl(ioreq, &ctl)))
        {
            there = ctl.uvc_Value;
            Printf(" -> %ld", there);
        }
        /* restore regardless of the write result */
        if(!writeControl(ioreq, &ctl, was) && !readControl(ioreq, &ctl))
        {
            back = ctl.uvc_Value;
            Printf(" -> %ld", back);
        }
        if(err || (there != target) || (back != was))
        {
            Printf(": expected %ld, then %ld again%s%s\n", target, was,
                   err ? ": " : "", err ? uvcErrorText(err) : (STRPTR) "");
            failed++;
        } else {
            PutStr(": as written, and restored\n");
        }
    } else {
        PutStr("\nNo control of the picture can be written: the write test is left out.\n");
    }

    Printf("%ld controls, %ld did not answer as they have to, %ld answers a control may leave out are missing: %s\n",
           num, failed, missing, (num && !failed) ? "PASS" : "FAIL");
    return num && !failed;
}
/* \\\ */

/* /// "Events" */
#define NUM_EVENTS    3   /* NSCMD_UV_WAITEVENT requests kept queued */
#define EVENT_SIZE    32  /* max status packet size */
#define EVENT_TIMEOUT (3 * TICKS_PER_SEC) /* max time for an asynchronous control to report */
#define EVENT_SETTLE  (2 * TICKS_PER_SEC) /* listen time after switching an auto mode */

/* Return TRUE if a VideoControl status packet refers to ctl (bOriginator =
   entity, bSelector = selector) */
static BOOL eventIsFor(const UBYTE *packet, ULONG len, const struct UsbVideoControl *ctl)
{
    const struct UsbVideoStatusVC *vc = (const struct UsbVideoStatusVC *) packet;

    return (len >= sizeof(*vc)) && ((vc->bStatusType & UVSPT_MASK) == UVSPT_VIDEOCONTROL) &&
           (vc->bOriginator == ctl->uvc_Entity) && (vc->bSelector == ctl->uvc_Selector);
}

/* Decode and print a status packet, with the current value of the control
   it names and a hex dump */
static void printEvent(struct IOStdReq *ioreq, const struct UsbVideoControl *list, ULONG num,
                       const UBYTE *packet, ULONG len)
{
    static const STRPTR attributes[] = { "new value", "new state", "failed", "new minimum", "new maximum" };
    const struct UsbVideoStatusVS *vs = (const struct UsbVideoStatusVS *) packet;
    const struct UsbVideoStatusVC *vc = (const struct UsbVideoStatusVC *) packet;

    PutStr("  event: ");
    if((len > sizeof(*vs)) && ((vs->bStatusType & UVSPT_MASK) == UVSPT_VIDEOSTREAMING))
    {
        if(vs->bEvent)
        {
            Printf("streaming interface %ld, event %ld", (ULONG) vs->bOriginator, (ULONG) vs->bEvent);
        } else {
            Printf("button %s (streaming interface %ld)", vs->bValue[0] ? "pressed" : "released",
                   (ULONG) vs->bOriginator);
        }
    }
    else if((len >= sizeof(*vc)) && ((vc->bStatusType & UVSPT_MASK) == UVSPT_VIDEOCONTROL))
    {
        ULONG attribute = vc->bAttribute;
        ULONG named = 0;

        Printf("unit %ld, ", (ULONG) vc->bOriginator);
        for(ULONG cnt = 0; cnt < num; cnt++)
        {
            struct UsbVideoControl ctl = list[cnt];

            if(!eventIsFor(packet, len, &ctl))
            {
                continue;
            }
            Printf("%s%s", named ? " / " : "", ctl.uvc_Name);
            /* VALUE/MIN/MAX changes carry raw control data; read the
               control to print the decoded value */
            if((attribute != UVSPA_INFO_CHANGE) && (attribute != UVSPA_FAILURE_CHANGE) &&
               !readControl(ioreq, &ctl) && (ctl.uvc_Flags & UVCF_GET))
            {
                PutStr(" now ");
                printControlValue(&ctl, ctl.uvc_Value);
            }
            named++;
        }
        if(!named)
        {
            Printf("selector %ld", (ULONG) vc->bSelector);
        }
        Printf(": %s", (attribute < sizeof(attributes) / sizeof(attributes[0])) ? attributes[attribute]
                                                                               : (STRPTR) "unknown kind of change");
        if((attribute == UVSPA_FAILURE_CHANGE) && (len > sizeof(*vc)))
        {
            PutStr(reasonText(vc->bValue[0]));
        }
    } else {
        PutStr("not a status packet this tool knows");
    }
    PutStr(" [");
    for(ULONG cnt = 0; cnt < len; cnt++)
    {
        Printf("%s%02lx", cnt ? " " : "", (ULONG) packet[cnt]);
    }
    PutStr("]\n");
}

/* Event test state */
struct EventTest
{
    struct IOStdReq *ioreq;     /* command request */
    struct MsgPort  *port;      /* reply port of the WAITEVENT requests */
    const struct UsbVideoControl *list;
    ULONG            num;
    struct IOStdReq *inhand;    /* WAITEVENT not queued at the device; its packet is
                                   valid until the next nextEvent() */
    BOOL             stop;      /* Ctrl-C, or the device terminated the waits */
};

/* Wait up to ticks for the next event. Resubmit the request held from the
   previous call first; hold the new one on return. TRUE = event received
   and printed. FALSE = timeout, or stop set (Ctrl-C or device error). */
static BOOL nextEvent(struct EventTest *et, ULONG ticks)
{
    ULONG until = ticksNow() + ticks;
    struct IOStdReq *wait;

    if(et->inhand)
    {
        SendIO((struct IORequest *) et->inhand);
        et->inhand = NULL;
    }
    while(!(wait = (struct IOStdReq *) GetMsg(et->port)))
    {
        if(SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        {
            et->stop = TRUE;
            return FALSE;
        }
        if((LONG) (until - ticksNow()) <= 0)
        {
            return FALSE;
        }
        Delay(2);
    }
    et->inhand = wait;
    if(wait->io_Error)
    {
        Printf("Waiting for events ended: %s (%ld)\n", uvcErrorText(wait->io_Error), (LONG) wait->io_Error);
        et->stop = TRUE;
        return FALSE;
    }
    printEvent(et->ioreq, et->list, et->num, wait->io_Data, wait->io_Actual);
    return TRUE;
}

/* Print events for ticks; return the count */
static ULONG listenForEvents(struct EventTest *et, ULONG ticks)
{
    ULONG until = ticksNow() + ticks;
    ULONG events = 0;
    LONG left;

    while(!et->stop && ((left = (LONG) (until - ticksNow())) > 0))
    {
        if(nextEvent(et, left))
        {
            events++;
        }
    }
    return events;
}

/* Wait for the event of one control, printing other events and counting
   all in *events. TRUE = received within EVENT_TIMEOUT. */
static BOOL awaitEvent(struct EventTest *et, const struct UsbVideoControl *ctl, ULONG *events)
{
    BOOL reported = FALSE;

    while(!reported && !et->stop && nextEvent(et, EVENT_TIMEOUT))
    {
        (*events)++;
        reported = eventIsFor(et->inhand->io_Data, et->inhand->io_Actual, ctl);
    }
    return reported;
}

/* EVENTS; the caller must have the camera streaming (an idle sensor
   reports nothing).
   1. Write every asynchronous control; each must send its status event.
   2. Switch every auto mode (UVCF_GOVERNS) to another setting and back;
      the resulting events are camera-specific and only printed.
   3. Listen for seconds.
   TRUE = PASS. */
static BOOL testEvents(struct IOStdReq *ioreq, struct MsgPort *port, const struct UsbVideoControl *list, ULONG num,
                       ULONG seconds)
{
    struct EventTest test = { ioreq, port, list, num, NULL, FALSE };
    struct EventTest *et = &test;
    struct IOStdReq *waits[NUM_EVENTS];
    UBYTE packets[NUM_EVENTS][EVENT_SIZE];
    ULONG written = 0;
    ULONG silent = 0;
    ULONG events = 0;
    LONG err;

    for(ULONG cnt = 0; cnt < NUM_EVENTS; cnt++)
    {
        if(!(waits[cnt] = (struct IOStdReq *) CreateIORequest(port, sizeof(struct IOStdReq))))
        {
            while(cnt--)
            {
                AbortIO((struct IORequest *) waits[cnt]);
                WaitIO((struct IORequest *) waits[cnt]);
                DeleteIORequest((struct IORequest *) waits[cnt]);
            }
            PutStr("FAIL: out of memory\n");
            return FALSE;
        }
        waits[cnt]->io_Device = ioreq->io_Device;
        waits[cnt]->io_Unit = ioreq->io_Unit;
        waits[cnt]->io_Command = NSCMD_UV_WAITEVENT;
        waits[cnt]->io_Data = packets[cnt];
        waits[cnt]->io_Length = EVENT_SIZE;
        SendIO((struct IORequest *) waits[cnt]);
    }

    for(ULONG cnt = 0; !et->stop && (cnt < num); cnt++)
    {
        struct UsbVideoControl ctl = list[cnt];

        /* re-read: an auto mode may have changed it since enumeration */
        if(readControl(ioreq, &ctl) || !(ctl.uvc_Flags & UVCF_SLOW) || !controlCanMove(&ctl))
        {
            continue;
        }
        LONG was = ctl.uvc_Value;
        LONG target = nextControlValue(&ctl);
        ULONG start = ticksNow();

        Printf("Writing %s: %ld -> %ld\n", ctl.uvc_Name, was, target);
        if((err = writeControl(ioreq, &ctl, target)))
        {
            Printf("  ! not written: %s (%ld)%s\n", uvcErrorText(err), err, reasonText(ctl.uvc_Reason));
            silent++;
            continue;
        }
        written++;
        if(awaitEvent(et, &ctl, &events))
        {
            ULONG ticks = ticksNow() - start;

            Printf("  reported after %ld.%02ld s\n", ticks / TICKS_PER_SEC, (ticks % TICKS_PER_SEC) * 100 / TICKS_PER_SEC);
        }
        else if(!et->stop)
        {
            PutStr("  ! no event for it\n");
            silent++;
        }
        /* restore; retry while the control reports NOT_READY */
        start = ticksNow();
        while((err = writeControl(ioreq, &ctl, was)) && (ctl.uvc_Reason == UVREC_NOT_READY) &&
              (ticksNow() - start < EVENT_TIMEOUT))
        {
            Delay(5);
        }
        if(err)
        {
            Printf("  ! not restored: %s (%ld)%s\n", uvcErrorText(err), err, reasonText(ctl.uvc_Reason));
            silent++;
        }
        /* consume the restore's event so it is not taken for the next control's */
        awaitEvent(et, &ctl, &events);
    }
    if(!written && !silent)
    {
        PutStr("No control of this camera is asynchronous and can be written now: nothing to check by writing.\n");
    }

    for(ULONG cnt = 0; !et->stop && (cnt < num); cnt++)
    {
        struct UsbVideoControl ctl = list[cnt];
        LONG other = -1;

        if(!(ctl.uvc_Flags & UVCF_GOVERNS) || readControl(ioreq, &ctl) ||
           ((ctl.uvc_Flags & (UVCF_GET|UVCF_SET)) != (UVCF_GET|UVCF_SET)))
        {
            continue;
        }
        LONG was = ctl.uvc_Value;

        /* alternative setting: inverted switch, or another supported choice */
        if(ctl.uvc_Kind == UVCK_SWITCH)
        {
            other = !was;
        }
        for(LONG choice = 0; (other < 0) && (ctl.uvc_Kind == UVCK_CHOICE) && (choice <= ctl.uvc_Max); choice++)
        {
            if((ctl.uvc_Choices & (1UL << choice)) && (choice != was))
            {
                other = choice;
            }
        }
        if(other < 0)
        {
            continue;
        }
        Printf("Switching %s to ", ctl.uvc_Name);
        printControlValue(&ctl, other);
        PutStr(" and back\n");
        if((err = writeControl(ioreq, &ctl, other)))
        {
            Printf("  not switched: %s (%ld)%s\n", uvcErrorText(err), err, reasonText(ctl.uvc_Reason));
            continue;
        }
        events += listenForEvents(et, EVENT_SETTLE);
        if((err = writeControl(ioreq, &ctl, was)))
        {
            Printf("  ! not switched back: %s (%ld)%s\n", uvcErrorText(err), err, reasonText(ctl.uvc_Reason));
            silent++;
        }
        events += listenForEvents(et, EVENT_SETTLE);
    }

    if(seconds && !et->stop)
    {
        Printf("Listening for %ld seconds...\n", seconds);
        events += listenForEvents(et, seconds * TICKS_PER_SEC);
    }
    if(!events)
    {
        PutStr("Nothing was reported. Either this camera reports nothing by itself, or its\n"
               "status endpoint does not deliver: this run cannot tell which.\n");
    }

    for(ULONG cnt = 0; cnt < NUM_EVENTS; cnt++)
    {
        /* the held request is not queued at the device */
        if(waits[cnt] != et->inhand)
        {
            AbortIO((struct IORequest *) waits[cnt]);
            WaitIO((struct IORequest *) waits[cnt]);
        }
        DeleteIORequest((struct IORequest *) waits[cnt]);
    }
    Printf("%ld events; %ld asynchronous controls written; %ld writes refused or left without their event: %s\n",
           events, written, silent, silent ? "FAIL" : "PASS");
    return !silent;
}
/* \\\ */

/* Enumerate the named controls, then run CONTROLS and/or EVENTS on that
   list. TRUE = PASS. */
static BOOL testControlsAndEvents(struct IOStdReq *ioreq, struct MsgPort *port, struct UsbVideoInfo *uvi)
{
    struct UsbVideoControl *list = AllocVec(MAX_CONTROLS * sizeof(struct UsbVideoControl), MEMF_ANY);
    BOOL ok = TRUE;
    LONG err;

    if(!list)
    {
        PutStr("FAIL: out of memory\n");
        return FALSE;
    }
    ULONG num = listControls(ioreq, list, MAX_CONTROLS, &err);

    if(err != UVERR_NOCONTROL)
    {
        Printf("FAIL: could not list the camera's controls: %s (%ld)\n",
               err ? uvcErrorText(err) : (STRPTR) "more of them than this tool has room for", err);
        ok = FALSE;
    } else {
        if(ArgsArray[ARGS_CONTROLS])
        {
            ok = testControls(ioreq, uvi, list, num);
        }
        if(ArgsArray[ARGS_EVENTS])
        {
            struct UsbVideoModeReq uvq = { .uvq_FourCC = 0 };

            if(!(uvi->uvi_Flags & UVIF_EVENTS))
            {
                PutStr("The camera has no status endpoint: it reports nothing by itself.\n");
            }
            /* stream in the default mode; no reads queued, frames are dropped */
            else if(!setMode(ioreq, &uvq))
            {
                ok = FALSE;
            }
            else if((err = uvcCommand(ioreq, CMD_START, NULL, 0)))
            {
                Printf("FAIL: could not start the stream: %s (%ld)\n", uvcErrorText(err), err);
                ok = FALSE;
            } else {
                PutStr("The camera streams while this runs.\n");
                if(!testEvents(ioreq, port, list, num, *((ULONG *) ArgsArray[ARGS_EVENTS])))
                {
                    ok = FALSE;
                }
                uvcCommand(ioreq, CMD_STOP, NULL, 0);
            }
        }
    }
    FreeVec(list);
    return ok;
}

/* What the preview window does while a test runs: left, top, width and
   height change by these, as they would by a user's drags. Intuition reports
   them alike. */
static const WORD PreviewSteps[][4] =
{
    { 100, 50, 0, 0 },
    { 0, 0, 320, 180 },
    { 0, 0, -240, -135 }
};
#define PREVIEW_STEPS (sizeof(PreviewSteps) / sizeof(PreviewSteps[0]))

/* Take step step of PreviewSteps, the window kept on its screen */
static void stepWindow(struct Window *win, ULONG step)
{
    LONG width = win->Width + PreviewSteps[step][2];
    LONG height = win->Height + PreviewSteps[step][3];
    LONG left = win->LeftEdge + PreviewSteps[step][0];
    LONG top = win->TopEdge + PreviewSteps[step][1];

    if(left + width > win->WScreen->Width)
    {
        left = win->WScreen->Width - width;
    }
    if(top + height > win->WScreen->Height)
    {
        top = win->WScreen->Height - height;
    }
    ChangeWindowBox(win, (left < 0) ? 0 : left, (top < 0) ? 0 : top, width, height);
}

/* Print what a stream did. TRUE if the pictures kept up with what was sent. */
static BOOL previewCounts(const struct UvpCounts *uc)
{
    Printf("%ld frames, %ld bytes, %ld lost, %ld held back\n", uc->uc_Frames, uc->uc_Bytes, uc->uc_Lost, uc->uc_Held);
    Printf("%ld sent, %ld pictures, %ld shown, %ld saved; lag %ld frames at the end, %ld at most\n", uc->uc_Sent,
           uc->uc_Pictures, uc->uc_Shown, uc->uc_Saved, uc->uc_Lag, uc->uc_LagMost);
    if(!uc->uc_Sent || !uc->uc_Shown)
    {
        PutStr("FAIL: no picture reached the window\n");
        return FALSE;
    }
    if(uc->uc_Pictures * 100 < uc->uc_Sent * (100 - PREVIEW_TOLERATED))
    {
        PutStr("FAIL: the decoder fell behind\n");
        return FALSE;
    }
    return TRUE;
}

/* Run a stream until frames frames came; if steps, the window takes its
   steps on the way, and pictures must come after the last. TRUE = PASS. */
static BOOL previewStream(struct UvcPreview *up, const struct UsbVideoModeReq *uvq, ULONG frames, BOOL steps, STRPTR stem)
{
    const struct UvpCounts *uc = uvpCounts(up);
    ULONG limit = (frames / PREVIEW_SLOW_FPS + 10) * TICKS_PER_SEC;
    ULONG ticks = 0;
    ULONG step = 0;
    ULONG shownthen = 0;

    if(!uvpStart(up, uvq, stem))
    {
        Printf("FAIL: %s\n", uvpWhy(up));
        return FALSE;
    }
    if(uvpWhy(up)[0])
    {
        Printf("%s\n", uvpWhy(up));
    }
    if(stem && !uvpRecord(up, TRUE))
    {
        Printf("FAIL: %s\n", uvpWhy(up));
        uvpStop(up);
        return FALSE;
    }
    while(uvpRunning(up) && (uc->uc_Frames < frames) && (ticks < limit) && !(SetSignal(0, 0) & SIGBREAKF_CTRL_C))
    {
        Delay(PREVIEW_POLL_TICKS);
        ticks += PREVIEW_POLL_TICKS;
        if(steps && (step < PREVIEW_STEPS) && uvpWindow(up) && (uc->uc_Frames >= (step + 1) * frames / (PREVIEW_STEPS + 1)))
        {
            stepWindow(uvpWindow(up), step++);
            shownthen = uc->uc_Shown;
        }
    }

    BOOL ran = uvpRunning(up);
    BOOL preview = uvpWindow(up) != NULL;

    uvpStop(up);
    if(!ran)
    {
        Printf("FAIL: the stream ended: %s\n", uvpWhy(up));
        return FALSE;
    }
    if(uc->uc_Frames < frames)
    {
        Printf("FAIL: only %ld frames came\n", uc->uc_Frames);
        return FALSE;
    }
    if(!preview)
    {
        Printf("FAIL: no preview: %s\n", uvpWhy(up));
        return FALSE;
    }
    if(steps && (uc->uc_Shown <= shownthen))
    {
        PutStr("FAIL: no picture after the window's last step\n");
        return FALSE;
    }
    return previewCounts(uc);
}

/* Return how many sessions the first backend has open; ~0 if unknown */
static ULONG openSessions(void)
{
    struct VCInfo info;

    memset(&info, 0, sizeof(info));
    info.Size = sizeof(info);
    return (VideoCodecBase && !VC_GetBackendInfo(0, &info)) ? info.Sessions : ~0UL;
}

/* Start and stop a stream again and again: nothing may stay behind */
static BOOL previewCycles(struct UvcPreview *up, const struct UsbVideoModeReq *uvq, ULONG cycles)
{
    ULONG before = AvailMem(MEMF_ANY);

    for(ULONG cnt = 0; cnt < cycles; cnt++)
    {
        if(!previewStream(up, uvq, PREVIEW_CYCLE_FRAMES, FALSE, NULL))
        {
            Printf("in cycle %ld\n", cnt + 1);
            return FALSE;
        }
    }

    ULONG after = AvailMem(MEMF_ANY);
    ULONG sessions = openSessions();

    Printf("%ld cycles: %ld bytes free before, %ld after, %ld sessions open\n", cycles, before, after, sessions);
    if(sessions)
    {
        PutStr("FAIL: a session stayed open\n");
        return FALSE;
    }
    if((after < before) && (before - after > PREVIEW_MEMORY_SLACK))
    {
        PutStr("FAIL: memory stayed behind\n");
        return FALSE;
    }
    return TRUE;
}

/* The stream process of USBVideo: its own open of the unit, FRAMES through
   the decoder, then the cycles. TRUE = PASS. */
static BOOL testPreview(ULONG unit, const struct UsbVideoModeReq *uvq, ULONG frames, ULONG cycles, STRPTR stem)
{
    UBYTE why[UVC_TEXT_SIZE];
    BOOL ok = FALSE;
    struct UvcPreview *up = uvpOpen(unit, 0, 0, why);

    if(up)
    {
        ok = previewStream(up, uvq, frames, TRUE, stem) && (!cycles || previewCycles(up, uvq, cycles));
        uvpClose(up);
    } else {
        Printf("FAIL: %s\n", why);
    }
    PutStr(ok ? "PASS\n" : "");
    return ok;
}

/* Return the value of a /N argument, or none if it is absent */
static ULONG numberArgument(ULONG arg, ULONG none)
{
    return ArgsArray[arg] ? *((ULONG *) ArgsArray[arg]) : none;
}

/* Run the test selected by the arguments; with none, print the mode table.
   TRUE = PASS. */
static BOOL runTest(struct UvcCamera *cm)
{
    ULONG width = numberArgument(ARGS_WIDTH, 0);
    ULONG height = numberArgument(ARGS_HEIGHT, 0);
    ULONG fps = numberArgument(ARGS_FPS, 0);
    ULONG frames = numberArgument(ARGS_FRAMES, 0);
    ULONG cycles = numberArgument(ARGS_CYCLES, 0);
    ULONG fourcc = uvcParseFourCC((STRPTR) ArgsArray[ARGS_FORMAT]);
    STRPTR stem = (STRPTR) ArgsArray[ARGS_TO];
    BOOL preview = ArgsArray[ARGS_PREVIEW] != 0;
    struct UsbVideoModeReq uvq =
    {
        .uvq_FourCC = fourcc,
        .uvq_Width = width,
        .uvq_Height = height,
        .uvq_Interval = fps ? UVC_INTERVAL_UNITS / fps : 0,
        .uvq_BitRate = numberArgument(ARGS_BITRATE, 0),
        .uvq_KeyInterval = numberArgument(ARGS_KEYFRAME, (preview && (fourcc == UVFCC_H264)) ? UVP_KEY_INTERVAL : 0)
    };

    if(ArgsArray[ARGS_VHI] || preview)
    {
        /* the driver or the stream process opens the unit itself: close ours */
        CloseDevice((struct IORequest *) cm->cm_Request);
        cm->cm_Open = FALSE;
    }
    if(ArgsArray[ARGS_VHI])
    {
        return testVhiDriver(cm->cm_Unit, cm->cm_Modes, cm->cm_ModesSize, width, height, frames ? frames : 30, stem);
    }
    if(preview)
    {
        return testPreview(cm->cm_Unit, &uvq, frames ? frames : PREVIEW_FRAMES, cycles, stem);
    }
    if(ArgsArray[ARGS_CONTROLS] || ArgsArray[ARGS_EVENTS])
    {
        return testControlsAndEvents(cm->cm_Request, cm->cm_Port, &cm->cm_Info);
    }
    if(ArgsArray[ARGS_PROBE])
    {
        return probeModes(cm->cm_Request, cm->cm_Modes, cm->cm_ModesSize);
    }
    if(!(width || height || fps || frames || cycles || fourcc))
    {
        uvcPrintModes(cm);
        return TRUE;
    }

    if(!setMode(cm->cm_Request, &uvq))
    {
        return FALSE;
    }
    if(cycles)
    {
        return startStopCycles(cm, &uvq, cycles, frames ? frames : 10);
    }
    return !frames || captureFrames(cm, &uvq, frames, stem, FALSE);
}

int main(int argc, char *argv[])
{
    struct RDArgs *rdargs = ReadArgs(template, ArgsArray, NULL);

    if(!rdargs)
    {
        PutStr("Wrong arguments!\n");
        return RETURN_FAIL;
    }

    struct UvcCamera cm;
    LONG rc = RETURN_FAIL;

    if(uvcOpenCamera(&cm, numberArgument(ARGS_UNIT, 0)))
    {
        uvcPrintCamera(&cm);
        rc = runTest(&cm) ? RETURN_OK : RETURN_WARN;
    }
    uvcCloseCamera(&cm);
    FreeArgs(rdargs);
    return rc;
}
