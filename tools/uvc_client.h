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
 *  - files:   frame file naming and output
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <devices/usbvideodevice.h>

#define UVC_NUM_READS 3 /* CMD_READ requests kept queued during a capture */
#define UVC_INTERVAL_UNITS 10000000 /* frame interval unit is 100 ns: units per second */

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
};

/* Capture output: one file per frame, or one file for a stream format */
struct UvcFiles
{
    STRPTR fl_Stem;   /* file name stem; NULL disables saving */
    ULONG  fl_FourCC; /* frame format */
    BPTR   fl_Stream; /* handle of the stream file while open */
    ULONG  fl_Saved;  /* frames written */
};

/* Capture state: queued reads, current frame, statistics */
struct UvcCapture
{
    struct UvcCamera  *cp_Camera;
    struct IOVideoReq *cp_Reads[UVC_NUM_READS]; /* NULL where CreateIORequest() failed */
    struct IOVideoReq *cp_InHand;      /* the only read off the device: the frame last returned by
                                          uvcNextFrame(), or the read that ended the capture;
                                          resubmitted on the next call */
    struct UvcFiles    cp_Files;
    BOOL               cp_Streaming;   /* CMD_START succeeded */
    ULONG              cp_LastSeq;     /* ivr_Sequence of the last frame received, 0 before the first */
    ULONG              cp_Missed;      /* sum of sequence gaps: frames dropped before delivery */
    ULONG              cp_TooLong;     /* frames completed with UVERR_OVERFLOW, discarded */
    LONG               cp_Error;       /* device error that ended the capture, 0 if none */
    BOOL               cp_Interrupted; /* ended by Ctrl-C */
    BOOL               cp_NotSaved;    /* ended by a file write failure */
};

/* Camera */
BOOL uvcOpenCamera(struct UvcCamera *cm, ULONG unit);
void uvcCloseCamera(struct UvcCamera *cm);
LONG uvcCommand(struct IOStdReq *ioreq, UWORD command, APTR data, ULONG len);
STRPTR uvcErrorText(LONG err);

/* Text */
void uvcPrintCamera(const struct UvcCamera *cm);
void uvcPrintModes(const struct UvcCamera *cm);
void uvcPrintMode(const struct UsbVideoModeReq *uvq);
void uvcPrintEncoder(const struct UsbVideoModeReq *uvq);
void uvcPrintRate(ULONG interval);
void uvcPrintFourCC(ULONG fourcc);
ULONG uvcParseFourCC(STRPTR text);

/* Capture */
BOOL uvcStartCapture(struct UvcCapture *cp, struct UvcCamera *cm, const struct UsbVideoModeReq *uvq, STRPTR stem);
struct IOVideoReq *uvcNextFrame(struct UvcCapture *cp);
void uvcStopCapture(struct UvcCapture *cp);

/* Files */
BOOL uvcIsStream(ULONG fourcc);
BOOL uvcOpenFiles(struct UvcFiles *fl, STRPTR stem, ULONG fourcc);
BOOL uvcWriteFrame(struct UvcFiles *fl, const UBYTE *frame, ULONG len);
void uvcCloseFiles(struct UvcFiles *fl);
void uvcPrintSaved(const struct UvcFiles *fl);

#endif /* UVC_CLIENT_H */
