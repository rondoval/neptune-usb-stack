#ifndef UVC_PREVIEW_H
#define UVC_PREVIEW_H
/*
 *----------------------------------------------------------------------------
 *              UVC tools: the stream process
 *----------------------------------------------------------------------------
 * A camera run by a process of its own: its reads, the decoder, the preview
 * window, the files. The caller may block on anything (a held menu, a
 * requester, its own window's layers) and no read comes back late, which
 * would leave a hole in an H.264 stream and in a recording.
 *
 * All of the camera belongs to the process. Commands are answered before
 * these calls return, so make them from one task, the one that called
 * uvpOpen(). What the camera is and what the stream did can be read at any
 * time, through the const pointers.
 *
 * MJPEG and H.264 get a preview window where the machine can decode them,
 * through videocodec.library. Without one (no library, not a Picasso96
 * screen, another format) the stream runs all the same, for saving, and
 * uvpWhy() says what is missing.
 */

#include <exec/types.h>
#include <intuition/intuition.h>

#include "uvc_client.h"

#define UVP_KEY_INTERVAL 2000 /* ms between H.264 key frames that suits a preview: a hole heals soon */

/* What a stream did since its start. One process writes, anyone reads; each
   field is a long, so a reader gets a whole value, not a set taken at one
   moment */
struct UvpCounts
{
    ULONG uc_Frames;   /* came from the camera */
    ULONG uc_Bytes;    /* their bytes */
    ULONG uc_Lost;     /* the camera started them and none came: no read was waiting, or too long */
    ULONG uc_Held;     /* came, and were kept from the decoder: nothing in them */
    ULONG uc_Sent;     /* to the decoder */
    ULONG uc_Pictures; /* from the decoder */
    ULONG uc_Shown;    /* into the window; of many that came at once only the newest is */
    ULONG uc_Saved;    /* to files */
    ULONG uc_Lag;      /* frames sent and not yet back as pictures, when the last picture came */
    ULONG uc_LagMost;  /* the most uc_Lag was */
};

struct UvcPreview;

/* Start the process and open the unit. stopmask: the signals of the calling
   task to set when the stream ends by itself or loses its preview. tickmask:
   those to set once for each second of frames, to look at the counts; 0 for
   none. NULL if it failed; the reason in words is then in why, UVC_TEXT_SIZE
   bytes. */
struct UvcPreview *uvpOpen(ULONG unit, ULONG stopmask, ULONG tickmask, UBYTE *why);
/* Stop whatever runs, end the process and free everything. Pointers from the
   calls below are gone then. */
void uvpClose(struct UvcPreview *up);

const struct UvcCamera *uvpCamera(const struct UvcPreview *up);
const struct UvpCounts *uvpCounts(const struct UvcPreview *up);
const struct UsbVideoModeReq *uvpMode(const struct UvcPreview *up); /* the mode the camera settled on */
CONST_STRPTR uvpWhy(const struct UvcPreview *up); /* the latest failure, "" if none since the start */
BOOL uvpRunning(const struct UvcPreview *up);
struct Window *uvpWindow(const struct UvcPreview *up); /* the preview's, NULL if there is none */

/* Keep the preview window off this box of its screen, as far as there is room:
   the caller's own window, say. For the windows of the starts that follow. */
void uvpKeepClear(struct UvcPreview *up, const struct IBox *box);
/* Set the mode and run the stream. stem names the files uvpRecord() and
   uvpSnapshot() write (uvcOpenFiles()); NULL if nothing is to be saved. */
BOOL uvpStart(struct UvcPreview *up, const struct UsbVideoModeReq *uvq, STRPTR stem);
void uvpStop(struct UvcPreview *up);
/* Save every frame from here on, or no more. An H.264 file starts at a key
   frame, which the camera is asked for. */
BOOL uvpRecord(struct UvcPreview *up, BOOL on);
/* Save the next frame */
BOOL uvpSnapshot(struct UvcPreview *up);
/* Open the camera's settings window */
BOOL uvpSettings(struct UvcPreview *up);

#endif /* UVC_PREVIEW_H */
