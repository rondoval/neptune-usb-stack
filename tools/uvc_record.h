#ifndef UVC_RECORD_H
#define UVC_RECORD_H
/*
 *----------------------------------------------------------------------------
 *              UVC tools: recording
 *----------------------------------------------------------------------------
 * A camera's frames into Matroska files: H.264, MJPEG, YUY2 or NV12, each
 * file up to 4 GiB, the recording going on in the next. The caller owns the
 * recording and gives it the frames its reads bring (uvc_client.h); H.264
 * waits for a key frame, which the recording asks the camera for.
 */

#include "uvc_client.h"

/* A recording; rc_Saved and the names may be read, nothing written */
struct UvcRecording
{
    struct UvcMovie *rc_Movie; /* while recording; NULL: nothing is recorded */
    ULONG            rc_Saved; /* frames written, in all its files */
    ULONG            rc_Files; /* files begun */
    UBYTE            rc_First[UVC_NAME_SIZE]; /* the first file's name */
    UBYTE            rc_Name[UVC_NAME_SIZE];  /* the last's */
};

/* Return TRUE for a format that can be recorded */
BOOL uvcCanRecord(ULONG fourcc);
/* Start recording, in the mode set on cm, into new files, <stem>-NNNN.mkv at the first free number */
BOOL uvcStartRecording(struct UvcRecording *rc, struct UvcCamera *cm, CONST_STRPTR stem,
                       const struct UsbVideoModeReq *uvq);
/* Add the frame a read brought; nothing if no recording is open; FALSE ends the recording */
BOOL uvcRecordFrame(struct UvcRecording *rc, const struct IOVideoReq *ivr);
/* Finish the file, if one is open */
void uvcStopRecording(struct UvcRecording *rc);
/* Print the frames recorded and the files' names; nothing if none */
void uvcPrintSaved(const struct UvcRecording *rc);

#endif /* UVC_RECORD_H */
