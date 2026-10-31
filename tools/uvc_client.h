#ifndef UVC_CLIENT_H
#define UVC_CLIENT_H
/*
 *----------------------------------------------------------------------------
 *              UVC tools: usbvideo.device client helpers
 *----------------------------------------------------------------------------
 * Shared by USBVideo and USBVideoTest. Device API only, no neptune.library calls.
 *  - camera:  open a unit, read NSCMD_UV_GETINFO and the mode table
 *  - text:    print camera info, mode table, negotiated mode
 *  - capture: UVC_NUM_READS CMD_READs kept queued, complete frames returned
 *             one at a time and optionally written to files
 *  - reads:   the same reads driven by the caller's own loop
 *  - frames:  what a frame holds, for a program that decodes it
 *  - files:   frame file naming and output
 *
 * A failure is described by uvcReport(), by default to the Shell. A program
 * with no Shell behind it, or that calls this from a process of its own, sets
 * uvcReportTo.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <devices/usbvideodevice.h>

#define UVC_NUM_READS 4 /* CMD_READ requests kept queued during a capture */
#define UVC_INTERVAL_UNITS 10000000 /* frame interval unit is 100 ns: units per second */
#define UVC_JPEG_ROOM 420 /* what uvcJpegFill() wants free before a frame: the standard Huffman tables */
#define UVC_TEXT_SIZE 160 /* a failure's text in uvcReportTo, with its NUL */
#define UVC_FOURCC_SIZE 5 /* a FourCC as text, with its NUL */

/* Go through a mode table: its entries differ in length */
#define UVC_EACH_MODE(uvm, table, size) \
    for(const struct UsbVideoMode *uvm = (table); (const UBYTE *) uvm < (const UBYTE *) (table) + (size); \
        uvm = (const struct UsbVideoMode *) ((const UBYTE *) uvm + uvm->uvm_Size))

extern UBYTE *uvcReportTo; /* UVC_TEXT_SIZE bytes that take a failure's text; NULL: the Shell */

/* An open usbvideo.device unit with its info and mode table */
struct UvcCamera
{
    ULONG                cm_Unit;
    struct MsgPort      *cm_Port;      /* reply port for commands and capture reads */
    struct IOStdReq     *cm_Request;   /* command request */
    BOOL                 cm_Open;      /* OpenDevice() succeeded on cm_Request */
    struct UsbVideoInfo  cm_Info;      /* NSCMD_UV_GETINFO result, read at open */
    struct UsbVideoMode *cm_Modes;     /* NSCMD_UV_GETMODES table, cm_ModesSize bytes; NULL if empty */
    ULONG                cm_ModesSize;
    UBYTE                cm_H264Unit;  /* the H.264 extension unit's ID, 0 if it has none */
};

/* Capture output: one file per frame, or one file for a stream format */
struct UvcFiles
{
    STRPTR fl_Stem;   /* file name stem; NULL disables saving */
    ULONG  fl_FourCC; /* frame format */
    BPTR   fl_Stream; /* handle of the stream file while open */
    ULONG  fl_Saved;  /* frames written */
};

/* One read of a capture: its request, its buffer, and who has them */
struct UvcRead
{
    struct IOVideoReq *rd_Req;
    APTR               rd_Memory;   /* as allocated */
    UBYTE             *rd_Buffer;   /* inside it, aligned to a cache line */
    BOOL               rd_AtDevice; /* sent and not taken back: the only time WaitIO() is allowed.
                                       A read taken with GetMsg() must not get it: at an unplug
                                       that is a second Remove() through stale links */
    APTR               rd_UserData; /* the caller's */
};

/* Capture state: reads, current frame, statistics */
struct UvcCapture
{
    struct UvcCamera  *cp_Camera;
    struct UvcRead     cp_Reads[UVC_NUM_READS];
    struct UvcRead    *cp_InHand;      /* the frame last returned by uvcNextFrame(): resubmitted
                                          on the next call */
    struct UvcFiles    cp_Files;
    BOOL               cp_Streaming;   /* CMD_START succeeded */
    ULONG              cp_LastSeq;     /* ivr_Sequence of the last frame received, 0 before the first */
    ULONG              cp_Missed;      /* sum of sequence gaps: frames dropped before delivery */
    ULONG              cp_TooLong;     /* frames completed with UVERR_OVERFLOW, discarded */
    LONG               cp_Error;       /* device error that ended the capture, 0 if none */
    BOOL               cp_Interrupted; /* ended by Ctrl-C */
    BOOL               cp_NotSaved;    /* ended by a file write failure */
};

/* Describe a failure: into uvcReportTo, else to the Shell */
void uvcReport(const char *format, ...);

/* Camera */

/* Open a unit, read its info and mode table; call uvcCloseCamera() either way */
BOOL uvcOpenCamera(struct UvcCamera *cm, ULONG unit);
/* Free what uvcOpenCamera() allocated; safe after a partial open */
void uvcCloseCamera(struct UvcCamera *cm);
/* Execute one command with DoIO(); return io_Error */
LONG uvcCommand(struct IOStdReq *ioreq, UWORD command, APTR data, ULONG len);
/* Send a raw class request (NSCMD_UV_REQUEST); return io_Error, *reason takes the camera's error code */
LONG uvcRequest(struct IOStdReq *ioreq, ULONG request, ULONG entity, ULONG selector, APTR data, ULONG len, UBYTE *reason);
/* Return a device error code as text */
STRPTR uvcErrorText(LONG err);
/* Ask the H.264 unit for a key frame, not waiting for it; FALSE if there is no unit or it refused */
BOOL uvcRequestKeyFrame(struct UvcCamera *cm);

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

/* Start a capture in the mode already set, saving if stem is set; call uvcStopCapture() either way */
BOOL uvcStartCapture(struct UvcCapture *cp, struct UvcCamera *cm, const struct UsbVideoModeReq *uvq, STRPTR stem);
/* Return the next complete frame, waiting for one; NULL ends the capture */
struct IOVideoReq *uvcNextFrame(struct UvcCapture *cp);
/* Stop the stream, free the reads, close the output */
void uvcStopCapture(struct UvcCapture *cp);

/* Reads */

/* Prepare reads, none sent, for the caller's own loop; call uvcStopCapture() either way */
BOOL uvcMakeReads(struct UvcCapture *cp, struct UvcCamera *cm, ULONG room, ULONG size, ULONG memflags);
/* Send a read that is in hand to the device */
void uvcSendRead(struct UvcRead *rd);
/* Send every read that is in hand */
void uvcSendReads(struct UvcCapture *cp);
/* Return a read that came back with a frame, NULL if none; call until NULL before every Wait() */
struct UvcRead *uvcTakeFrame(struct UvcCapture *cp);
/* Send the reads, then CMD_START */
BOOL uvcStartStream(struct UvcCapture *cp);
/* Stop the stream and take every read back, nothing freed */
void uvcHaltCapture(struct UvcCapture *cp);

/* Frames */

/* Return TRUE if an H.264 access unit is a key frame */
BOOL uvcH264Key(const UBYTE *data, ULONG len);
/* Make a JPEG at buffer of the frame UVC_JPEG_ROOM bytes in, unmoved; return its length, 0 to skip it */
ULONG uvcJpegFill(UBYTE *buffer, ULONG len);

/* Files */

/* Return TRUE for a format saved as one file, not one file per frame */
BOOL uvcIsStream(ULONG fourcc);
/* Initialise the output, opening the file of a stream format; a NULL stem disables saving */
BOOL uvcOpenFiles(struct UvcFiles *fl, STRPTR stem, ULONG fourcc);
/* Write the next frame to a new file, or append it to the stream file */
BOOL uvcWriteFrame(struct UvcFiles *fl, const UBYTE *frame, ULONG len);
/* Close the stream file, if open */
void uvcCloseFiles(struct UvcFiles *fl);
/* Print the number of frames saved and their file names; nothing if none */
void uvcPrintSaved(const struct UvcFiles *fl);

#endif /* UVC_CLIENT_H */
