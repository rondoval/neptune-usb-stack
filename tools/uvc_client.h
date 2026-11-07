#ifndef UVC_CLIENT_H
#define UVC_CLIENT_H
/*
 *----------------------------------------------------------------------------
 *              UVC tools: usbvideo.device client helpers
 *----------------------------------------------------------------------------
 * Shared by USBVideo and USBVideoTest, below uvc_record.h and uvc_preview.h.
 * Device API only, no neptune.library calls.
 *  - camera:  open a unit, read NSCMD_UV_GETINFO and the mode table, commands
 *  - text:    print camera info, mode table, negotiated mode
 *  - capture: UVC_NUM_READS CMD_READs kept queued, complete frames returned
 *             one at a time
 *  - reads:   the same reads driven by the caller's own loop
 *  - frames:  what a frame's bytes hold
 *
 * Each structure is this module's: others read the fields marked so and
 * write none, rd_UserData aside. A failure is described by uvcReport(), by
 * default to the Shell. A program with no Shell behind it, or that calls this
 * from a process of its own, sets uvcReportTo.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <devices/usbvideodevice.h>

#define UVC_NUM_READS 4 /* CMD_READ requests kept queued during a capture */
#define UVC_KEEP_READS 2 /* of them, what a loop that lends reads out leaves at the device */
#define UVC_INTERVAL_UNITS 10000000 /* frame interval unit is 100 ns: units per second */
#define UVC_TEXT_SIZE 160 /* a failure's text in uvcReportTo, with its NUL */
#define UVC_FOURCC_SIZE 5 /* a FourCC as text, with its NUL */
#define UVC_NAME_SIZE 256 /* a file's name, with its NUL */

/* Go through a mode table: its entries differ in length */
#define UVC_EACH_MODE(uvm, table, size) \
    for(const struct UsbVideoMode *uvm = (table); (const UBYTE *) uvm < (const UBYTE *) (table) + (size); \
        uvm = (const struct UsbVideoMode *) ((const UBYTE *) uvm + uvm->uvm_Size))

/* Go through a capture's reads */
#define UVC_EACH_READ(rd, cp) \
    for(struct UvcRead *rd = (struct UvcRead *) (cp)->cp_Reads; rd < (cp)->cp_Reads + UVC_NUM_READS; rd++)

extern UBYTE *uvcReportTo; /* UVC_TEXT_SIZE bytes that take a failure's text; NULL: the Shell */

/* An open usbvideo.device unit with its info and mode table. Others read
   cm_Info and the mode table, and send raw commands on cm_Request with
   replies to cm_Port. */
struct UvcCamera
{
    ULONG                cm_Unit;
    struct MsgPort      *cm_Port;      /* reply port for commands and capture reads */
    struct IOStdReq     *cm_Request;   /* command request */
    BOOL                 cm_Open;      /* OpenDevice() succeeded on cm_Request */
    struct UsbVideoInfo  cm_Info;      /* NSCMD_UV_GETINFO result, read at open */
    struct UsbVideoMode *cm_Modes;     /* NSCMD_UV_GETMODES table, cm_ModesSize bytes; NULL if empty */
    ULONG                cm_ModesSize;
};

/* One read of a capture: its request, its buffer, and who has them. A loop
   that lends the buffer out reads rd_Req and rd_Buffer and keeps its
   borrower's handle in rd_UserData. */
struct UvcRead
{
    struct IOVideoReq *rd_Req;
    APTR               rd_Memory;   /* as allocated */
    UBYTE             *rd_Buffer;   /* inside it, aligned to a cache line: the room, then the frame */
    BOOL               rd_AtDevice; /* sent and not taken back: the only time WaitIO() is allowed.
                                       A read taken with GetMsg() must not get it: at an unplug
                                       that is a second Remove() through stale links */
    APTR               rd_UserData; /* the caller's */
};

/* Capture state: reads, current frame, statistics. Others read
   cp_Streaming, cp_Error, cp_Interrupted and the counts, which uvcLost()
   sums. */
struct UvcCapture
{
    struct UvcCamera  *cp_Camera;
    struct UvcRead     cp_Reads[UVC_NUM_READS]; /* through UVC_EACH_READ() */
    struct UvcRead    *cp_InHand;      /* the frame last returned by uvcNextFrame(): resubmitted
                                          on the next call */
    BOOL               cp_Streaming;   /* CMD_START succeeded */
    ULONG              cp_LastSeq;     /* ivr_Sequence of the last frame received, 0 before the first */
    ULONG              cp_Missed;      /* sum of sequence gaps: frames dropped before delivery */
    ULONG              cp_TooLong;     /* frames completed with UVERR_OVERFLOW, discarded */
    LONG               cp_Error;       /* device error that ended the capture, 0 if none */
    BOOL               cp_Interrupted; /* ended by Ctrl-C */
};

/* Describe a failure: into uvcReportTo, else to the Shell */
void uvcReport(const char *format, ...);

/* Camera */

/* Open a unit, read its info and mode table; call uvcCloseCamera() either way */
BOOL uvcOpenCamera(struct UvcCamera *cm, ULONG unit);
/* Free what uvcOpenCamera() allocated; safe after a partial open */
void uvcCloseCamera(struct UvcCamera *cm);
/* Execute one command with DoIO(); return io_Error. Reads that reply to the
   same port meanwhile wait there for the caller. */
LONG uvcCommand(struct IOStdReq *ioreq, UWORD command, APTR data, ULONG len);
/* Send a raw class request (NSCMD_UV_REQUEST); return io_Error, *reason takes the camera's error code */
LONG uvcRequest(struct IOStdReq *ioreq, ULONG request, ULONG entity, ULONG selector, APTR data, ULONG len, UBYTE *reason);
/* Return a device error code as text */
STRPTR uvcErrorText(LONG err);
/* Set a mode (NSCMD_UV_SETMODE), uvq taking what the camera settled on; report a failure, return io_Error */
LONG uvcSetMode(struct UvcCamera *cm, struct UsbVideoModeReq *uvq);
/* Close the unit, keeping its info and mode table for reading */
void uvcCloseUnit(struct UvcCamera *cm);
/* Close the unit and open it again, waiting out IOERR_UNITBUSY; return the OpenDevice() error */
LONG uvcReopenCamera(struct UvcCamera *cm, ULONG *waited);

/* Text */

/* Print a one-line camera summary */
void uvcPrintCamera(const struct UvcCamera *cm);
/* Print the mode table, one line per mode */
void uvcPrintModes(const struct UvcCamera *cm);
/* Print the negotiated mode, no line end */
void uvcPrintMode(const struct UsbVideoModeReq *uvq);
/* Write a FourCC into text, UVC_FOURCC_SIZE bytes; return text */
UBYTE *uvcFourCCText(ULONG fourcc, UBYTE *text);
/* Convert a string to a FourCC, space-padded; return 0 for NULL or empty */
ULONG uvcParseFourCC(STRPTR text);

/* Capture */

/* Start a capture in the mode already set; call uvcStopCapture() either way */
BOOL uvcStartCapture(struct UvcCapture *cp, struct UvcCamera *cm, const struct UsbVideoModeReq *uvq);
/* Return the next complete frame, waiting for one; NULL ends the capture */
struct IOVideoReq *uvcNextFrame(struct UvcCapture *cp);
/* Stop the stream, free the reads */
void uvcStopCapture(struct UvcCapture *cp);
/* Return the frames the camera started that did not come whole */
static inline ULONG uvcLost(const struct UvcCapture *cp)
{
    return cp->cp_Missed + cp->cp_TooLong;
}

/* Reads */

/* Prepare reads, none sent, for the caller's own loop; call uvcStopCapture() either way */
BOOL uvcMakeReads(struct UvcCapture *cp, struct UvcCamera *cm, ULONG room, ULONG size, ULONG memflags);
/* Send a read that is in hand to the device */
void uvcSendRead(struct UvcRead *rd);
/* Send every read that is in hand */
void uvcSendReads(struct UvcCapture *cp);
/* Return how many reads are at the device; lend one out only while UVC_KEEP_READS stay */
ULONG uvcReadsAtDevice(const struct UvcCapture *cp);
/* Return a read that came back with a frame, NULL if none; call until NULL before every Wait() */
struct UvcRead *uvcTakeFrame(struct UvcCapture *cp);
/* Send the reads, then CMD_START */
BOOL uvcStartStream(struct UvcCapture *cp);
/* Stop the stream and take every read back, nothing freed */
void uvcHaltCapture(struct UvcCapture *cp);

/* Frames */

/* Return TRUE for a format of NAL units in Annex B (H.264, H.265) */
BOOL uvcIsAnnexB(ULONG fourcc);
/* Return TRUE if a frame of the format is a key frame; any frame of a format not of NAL units is one */
BOOL uvcIsKeyFrame(ULONG fourcc, const UBYTE *data, ULONG len);
/* Return a JPEG frame's length up to and with its EOI; 0 if it holds no whole picture */
ULONG uvcJpegLength(const UBYTE *frame, ULONG len);

#endif /* UVC_CLIENT_H */
