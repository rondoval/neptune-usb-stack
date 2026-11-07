#ifndef USBVIDEO_CLASS_H
#define USBVIDEO_CLASS_H

/*
 *----------------------------------------------------------------------------
 *                         Includes for usbvideo class
 *----------------------------------------------------------------------------
 */

#include "common.h"

#include <devices/newstyle.h>
#include <devices/usbvideodevice.h>

#include "usbvideo.h"
#include "dev.h"

/* Protos */

struct NepClassVideo * usbAttemptInterfaceBinding(struct NepVideoBase *nh, struct PsdInterface *pif);
struct NepClassVideo * usbForceInterfaceBinding(struct NepVideoBase *nh, struct PsdInterface *pif);
void usbReleaseInterfaceBinding(struct NepVideoBase *nh, struct NepClassVideo *ncv);

struct NepClassVideo * nAllocVideo(void);
void nFreeVideo(struct NepClassVideo *ncv);

void nVideoTask();

/* uvc_ctrl.c - class requests and negotiation; called on the binding's subtask */

LONG uvcRequest(struct NepClassVideo *ncv, ULONG request, ULONG entity, ULONG ifnum,
                ULONG selector, APTR data, ULONG len, UBYTE *reason);
LONG uvcPassRequest(struct NepClassVideo *ncv, struct IOStdReq *ioreq);
LONG uvcKeyFrame(struct NepClassVideo *ncv, struct NepVideoStream *ns);
LONG uvcEmbedCommit(struct NepClassVideo *ncv, const struct UvcxVideoConfig *cfg);
LONG uvcNegotiateEmbedded(struct NepClassVideo *ncv, struct NepVideoStream *ns,
                          struct UvcFormat *ufo, struct UvcFrame *ufr, ULONG interval,
                          ULONG bitrate, ULONG keyinterval);
LONG uvcNegotiate(struct NepClassVideo *ncv, struct NepVideoStream *ns,
                  struct UvcFormat *ufo, struct UvcFrame *ufr, ULONG interval);

/* uvc_controls.c - the standard controls by name; called on the binding's subtask */

LONG uvcControlQuery(struct NepClassVideo *ncv, struct UsbVideoControl *uvc, BOOL next);
LONG uvcControlWrite(struct NepClassVideo *ncv, ULONG id, ULONG entity, LONG value, UBYTE *reason);
LONG uvcControlCommand(struct NepClassVideo *ncv, struct IOStdReq *ioreq);

/* uvc_status.c - the camera's status events. Start, Stop and Done run on the
   binding's subtask; the queue functions are callable from any context. */

void uvcStatusStart(struct NepClassVideo *ncv);
void uvcStatusStop(struct NepClassVideo *ncv);
void uvcStatusDone(struct NepClassVideo *ncv);
LONG uvcEventWait(struct NepClassVideo *ncv, struct IOStdReq *ioreq);
BOOL uvcEventAbort(struct NepClassVideo *ncv, struct IOStdReq *ioreq);
void uvcEventFlush(struct NepClassVideo *ncv, struct Unit *unit, LONG error);

/* usbvideo.gui.c - the settings window of a camera, a task of its own */

LONG nOpenBindingCfgWindow(struct NepVideoBase *nh, struct NepClassVideo *ncv);
void nGUITask();

/* uvc_stream.c */

struct PsdInterface * uvcFindAlternate(struct NepClassVideo *ncv, struct UvcStreamDesc *us,
                                       ULONG payloadsize, struct PsdEndpoint **pepptr);
BOOL uvcStreamIsBulk(struct NepClassVideo *ncv, struct UvcStreamDesc *us);
BOOL uvcIsConnected(struct NepClassVideo *ncv);
LONG uvcStart(struct NepClassVideo *ncv, struct NepVideoStream *ns);
void uvcStop(struct NepClassVideo *ncv, struct NepVideoStream *ns);
void uvcAbortReads(struct NepVideoStream *ns, LONG error);
/* the payload engine, for the transports */
ULONG uvcPayloadBegin(struct NepVideoStream *ns, const UBYTE *hdr, ULONG len, BOOL wireerror, UWORD busframe);
void uvcPayloadData(struct NepVideoStream *ns, const UBYTE *data, ULONG len);
void uvcPayloadEnd(struct NepVideoStream *ns);

/* uvc_iso.c - the isochronous transport, for uvcStart() and uvcStop() */

LONG uvcIsoStart(struct NepClassVideo *ncv, struct NepVideoStream *ns, struct PsdEndpoint *pep);
void uvcIsoStop(struct NepClassVideo *ncv, struct NepVideoStream *ns);
void uvcZeroBandwidth(struct NepClassVideo *ncv, struct UvcStreamDesc *us);

/* uvc_bulk.c - the bulk transport, for uvcStart() and uvcStop(); uvcBulkDone() for the subtask's loop */

ULONG uvcBulkBufferSize(struct NepVideoStream *ns);
LONG uvcBulkStart(struct NepClassVideo *ncv, struct NepVideoStream *ns, struct PsdEndpoint *pep);
void uvcBulkStop(struct NepClassVideo *ncv, struct NepVideoStream *ns);
void uvcBulkDone(struct NepClassVideo *ncv, struct NepVideoStream *ns);

#endif /* USBVIDEO_CLASS_H */
