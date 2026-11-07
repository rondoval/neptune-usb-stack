/*
 *----------------------------------------------------------------------------
 *            usbvideo class: class requests and stream negotiation
 *----------------------------------------------------------------------------
 * Everything here runs on the binding's subtask, which owns the EP0 pipe.
 */

#include "debug.h"
#include "usbvideo.class.h"

static const STRPTR libname = CLASS_NAME;

#define ps ncv->ncv_Base

#define UVCX_IFRAME_PERIOD_MAX 0xffff /* the H.264 unit counts the time between key frames in
                                         16 bits: a good minute at most */

/* /// "uvcRequest()" */
/* The one place that builds a UVC class request: to an interface (entity 0)
   or to a terminal or unit behind it. Returns the USB error. A reply shorter
   than asked for is accepted and the rest of the buffer zeroed - cameras
   answer with the length of an older revision. If the camera refuses, its
   reason (UVREC_) goes to *reason, if the caller wants it. */
LONG uvcRequest(struct NepClassVideo *ncv, ULONG request, ULONG entity, ULONG ifnum,
                ULONG selector, APTR data, ULONG len, UBYTE *reason)
{
    BOOL isget = (request & 0x80) != 0;
    LONG ioerr;

    psdPipeSetup(ncv->ncv_EP0Pipe,
                 (isget ? URTF_IN : URTF_OUT)|URTF_CLASS|URTF_INTERFACE,
                 request, selector << 8, (entity << 8) | ifnum);
    ioerr = psdDoPipe(ncv->ncv_EP0Pipe, data, len);
    if(!ioerr && isget)
    {
        ULONG actual = psdGetPipeActual(ncv->ncv_EP0Pipe);
        if(actual < len)
        {
            memset((UBYTE *) data + actual, 0, len - actual);
        }
    }
    if(ioerr == UHIOERR_STALL)
    {
        /* the camera says no; why is in the request error code of the control interface */
        UBYTE code = UVREC_UNKNOWN;
        psdPipeSetup(ncv->ncv_EP0Pipe, URTF_IN|URTF_CLASS|URTF_INTERFACE,
                     UVUDR_GET_CUR, UVVCCS_REQUEST_ERROR_CODE_CONTROL << 8, ncv->ncv_IfNum);
        psdDoPipe(ncv->ncv_EP0Pipe, &code, 1);
        KPRINTF(10, ("Request %02lx entity %ld if %ld selector %02lx refused, error code %ld\n",
                     request, entity, ifnum, selector, (ULONG) code));
        if(reason)
        {
            *reason = code;
        }
    }
    return ioerr;
}
/* \\\ */

/* /// "uvcPassRequest()" */
/* NSCMD_UV_REQUEST: one request of an application or of the settings window
   to the control interface or to a terminal or unit behind it, passed on as
   it is - what it means is the sender's business. The streaming interfaces
   are out of its reach, and with them the negotiated mode. Returns 0 or the
   error for io_Error. */
LONG uvcPassRequest(struct NepClassVideo *ncv, struct IOStdReq *ioreq)
{
    struct UsbVideoRequest *uvr = (struct UsbVideoRequest *) ioreq->io_Data;
    LONG ioerr;

    if(!uvr || (ioreq->io_Length < sizeof(struct UsbVideoRequest)) || (uvr->uvr_Length && !uvr->uvr_Data))
    {
        return IOERR_BADLENGTH;
    }
    uvr->uvr_ErrorCode = UVREC_NO_ERROR;
    uvr->uvr_Actual = 0;
    ioerr = uvcRequest(ncv, uvr->uvr_Request, uvr->uvr_Entity, ncv->ncv_IfNum, uvr->uvr_Selector,
                       uvr->uvr_Data, uvr->uvr_Length, &uvr->uvr_ErrorCode);
    if(ioerr)
    {
        return (ioerr == UHIOERR_STALL) ? UVERR_REFUSED : UVERR_USB;
    }
    uvr->uvr_Actual = psdGetPipeActual(ncv->ncv_EP0Pipe);
    ioreq->io_Actual = sizeof(struct UsbVideoRequest);
    return 0;
}
/* \\\ */

/* /// "uvcEncoderHasSync()" */
/* Does an Encoding Unit take its sync frame control while the stream runs?
   Its descriptor's second bitmap says, a selector's bit being selector - 1. */
static BOOL uvcEncoderHasSync(const struct UvcEntity *eu)
{
    ULONG bit = UVEUCS_SYNC_REF_FRAME_CONTROL - 1;
    ULONG byte = bit / 8;
    const UBYTE *runtime = eu->ue_Controls + eu->ue_ControlSize;

    return (byte < eu->ue_ControlSize) && ((runtime[byte] >> (bit % 8)) & 1);
}
/* \\\ */

/* /// "uvcKeyFrame()" */
/* NSCMD_UV_KEYFRAME: ask the camera for a key frame in the stream of the
   mode in force, the way the mode's row says (UPKF_): an encoder that lacks
   its control at run time leaves the streaming interface's, and a format
   whose bmaControls lack that leaves nothing. Does not wait for the frame.
   Returns 0 once asked, UVERR_NOMODE without a mode, UVERR_NOCONTROL when
   there is no way to ask, else the request's error. */
LONG uvcKeyFrame(struct NepClassVideo *ncv, struct NepVideoStream *ns)
{
    const struct UvcFormat *ufo = ns->ns_Mode.um_Format;

    if(!ufo)
    {
        return UVERR_NOMODE;
    }

    ULONG method = ufo->ufo_Payload->up_KeyFrame;
    struct UvcEntity *eu = (method == UPKF_ENCODER)
                           ? uvcPathEntity(ncv->ncv_Function, ns->ns_Desc, UDST_VIDEO_CTRL_ENCODING_UNIT) : NULL;

    if((method == UPKF_ENCODER) && !(eu && uvcEncoderHasSync(eu)))
    {
        method = UPKF_STREAM;
    }
    if((method == UPKF_STREAM) && !(ufo->ufo_Controls & (1 << UVIHB_GENERATE_KEY_FRAME)))
    {
        method = UPKF_NONE;
    }

    LONG ioerr;

    switch(method)
    {
        case UPKF_UVCX:
        {
            struct UvcxPictureType picture = { 0, AROS_WORD2LE(UVCXPT_NEW_SETS) };

            ioerr = uvcRequest(ncv, UVUDR_SET_CUR, uvcFindH264Unit(ncv->ncv_Function)->ue_ID, ncv->ncv_IfNum,
                               UVCX_PICTURE_TYPE_CONTROL, &picture, sizeof(picture), NULL);
            break;
        }

        case UPKF_ENCODER:
        {
            /* one frame, now: no interval, no gradual refresh */
            struct UsbVideoSyncRefFrame sync = { UVSFT_KEY_FRAME, 0, 0 };

            ioerr = uvcRequest(ncv, UVUDR_SET_CUR, eu->ue_ID, ncv->ncv_IfNum,
                               UVEUCS_SYNC_REF_FRAME_CONTROL, &sync, sizeof(sync), NULL);
            break;
        }

        case UPKF_STREAM:
        {
            struct UsbVideoGenerateKeyFrame generate = { UVGKF_GENERATE };

            ioerr = uvcRequest(ncv, UVUDR_SET_CUR, 0, ns->ns_Desc->us_IfNum,
                               UVVSCS_GENERATE_KEY_FRAME_CONTROL, &generate, sizeof(generate), NULL);
            break;
        }

        default:
            return UVERR_NOCONTROL;
    }
    if(ioerr)
    {
        return (ioerr == UHIOERR_STALL) ? UVERR_REFUSED : UVERR_USB;
    }
    return 0;
}
/* \\\ */

/* /// "uvcNegotiate()" */
/* Propose a mode and take what the camera makes of it. Only PROBE SET_CUR
   and GET_CUR are sent: the requests camera firmware gets wrong are the ones
   left out. Returns 0 or a UVERR_ code; on success the unit holds the mode.
     ncv      - the camera (the binding); the requests go through it
     ns       - the unit, that is the stream the mode is for. On success the
                mode is stored in its ns_Mode
     ufo      - the format to propose: one of that stream's formats
     ufr      - the picture size to propose: one of that format's frames
     interval - the frame interval to propose, in 100 ns units. The caller
                passes one the frame offers (uvcSnapInterval(),
                uvcDefaultInterval())
   The camera may answer with another format, size or interval than was
   proposed: what is stored is what it answered. */
LONG uvcNegotiate(struct NepClassVideo *ncv, struct NepVideoStream *ns,
                  struct UvcFormat *ufo, struct UvcFrame *ufr, ULONG interval)
{
    struct UvcStreamDesc *us = ns->ns_Desc;
    UWORD version = ncv->ncv_Function->uf_Version;
    ULONG size = (version >= UVC_VERSION_15) ? UVPC_SIZE_15 :
                 ((version >= UVC_VERSION_11) ? UVPC_SIZE_11 : UVPC_SIZE_10);
    struct UsbVideoProbeCommit probe;
    LONG ioerr;

    memset(&probe, 0, sizeof(probe));
    probe.bmHint = AROS_WORD2LE(UVPCHF_FRAME_INTERVAL);
    probe.bFormatIndex = ufo->ufo_Index;
    probe.bFrameIndex = ufr->ufr_Index;
    probe.dwFrameInterval = AROS_LONG2LE(interval);
    /* what the stream is for; a format without the notion asks for nothing.
       The field is of UVC 1.5: an older device is not sent it. */
    probe.bUsage = ufo->ufo_Payload->up_Usage;
    ioerr = uvcRequest(ncv, UVUDR_SET_CUR, 0, us->us_IfNum, UVVSCS_PROBE_CONTROL, &probe, size, NULL);
    if(!ioerr)
    {
        ioerr = uvcRequest(ncv, UVUDR_GET_CUR, 0, us->us_IfNum, UVVSCS_PROBE_CONTROL, &probe, size, NULL);
    }
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Negotiating %ldx%ld failed: %s (%ld)",
                       (ULONG) ufr->ufr_Width, (ULONG) ufr->ufr_Height,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return (ioerr == UHIOERR_STALL) ? UVERR_REFUSED : UVERR_USB;
    }

    /* the camera may have settled on something else than was proposed; a
       format it kept stays the entry that was asked for (two entries can
       share an index: H.264 inside MJPEG frames is the MJPEG format's twin) */
    if(probe.bFormatIndex != ufo->ufo_Index)
    {
        ufo = uvcFormatByIndex(us, probe.bFormatIndex);
    }
    ufr = (ufo && uvcIsStreamable(ufo)) ? uvcFrameByIndex(ufo, probe.bFrameIndex) : NULL;
    if(!ufr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "The camera answered with a mode it does not have (format %ld, frame %ld).",
                       (ULONG) probe.bFormatIndex, (ULONG) probe.bFrameIndex);
        return UVERR_REFUSED;
    }

    /* The block itself is kept as received - it goes back to the camera
       unchanged at commit time; the class works with these three, repaired
       where firmware is known to slip. */
    ULONG payloadsize = AROS_LE2LONG(probe.dwMaxPayloadTransferSize);
    ULONG framesize = AROS_LE2LONG(probe.dwMaxVideoFrameSize);
    ULONG negotiated = AROS_LE2LONG(probe.dwFrameInterval);
    if((payloadsize & 0xffff0000) == 0xffff0000)
    {
        payloadsize &= 0xffff; /* a 16 bit value, sign-extended by the camera */
    }
    /* a format whose samples all have the size of the picture: that size,
       whatever the camera put into the block - a frame of any other length
       is not a picture */
    ULONG picturesize = uvcPictureSize(ufo, ufr);
    if(picturesize)
    {
        framesize = picturesize;
    }
    if(!framesize)
    {
        framesize = (ULONG) ufr->ufr_Width * ufr->ufr_Height * 2; /* raw 4:2:2: no sample is larger */
    }
    if(!negotiated)
    {
        negotiated = interval;
    }
    /* the clock of PTS and SCR: named by the block from UVC 1.1 on, else -
       and for a camera that leaves it zero - by the control header */
    ULONG clock = (size >= UVPC_SIZE_11) ? AROS_LE2LONG(probe.dwClockFrequency) : 0;
    if(!clock)
    {
        clock = ncv->ncv_Function->uf_ClockFrequency;
    }

    struct PsdEndpoint *pep = NULL;
    struct PsdInterface *altif = uvcFindAlternate(ncv, us, payloadsize, &pep);
    if(!altif)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "%ldx%ld needs %ld bytes per interval: more than any alternate setting carries.",
                       (ULONG) ufr->ufr_Width, (ULONG) ufr->ufr_Height, payloadsize);
        return UVERR_NOBANDWIDTH;
    }
    IPTR altnum = 0;
    psdGetAttrs(PGA_INTERFACE, altif, IFA_AlternateNum, &altnum, TAG_END);
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Unit %ld: %ldx%ld at %ld.%ld fps, frames up to %ld bytes, %ld bytes per %s (alternate %ld).",
                   ns->ns_UnitNo, (ULONG) ufr->ufr_Width, (ULONG) ufr->ufr_Height,
                   10000000 / negotiated, (100000000 / negotiated) % 10,
                   framesize, payloadsize, ns->ns_IsBulk ? "transfer" : "interval", altnum);

    /* readers of the unit (NSCMD_UV_GETINFO) look under Forbid() */
    Forbid();
    ns->ns_Mode.um_Probe = probe;
    ns->ns_Mode.um_Frame = ufr;
    ns->ns_Mode.um_Interval = negotiated;
    ns->ns_Mode.um_MaxFrameSize = framesize;
    ns->ns_Mode.um_ClockFrequency = clock;
    ns->ns_Mode.um_PayloadSize = payloadsize;
    ns->ns_Mode.um_Format = ufo;
    Permit();
    return 0;
}
/* \\\ */

/* /// "uvcEmbedCommit()" */
/* Write a configuration to the camera's H.264 unit and put it in force.
   Returns the USB error. */
LONG uvcEmbedCommit(struct NepClassVideo *ncv, const struct UvcxVideoConfig *cfg)
{
    ULONG unit = uvcFindH264Unit(ncv->ncv_Function)->ue_ID;
    LONG ioerr;

    ioerr = uvcRequest(ncv, UVUDR_SET_CUR, unit, ncv->ncv_IfNum, UVCX_VIDEO_CONFIG_PROBE,
                       (APTR) cfg, sizeof(*cfg), NULL);
    if(!ioerr)
    {
        ioerr = uvcRequest(ncv, UVUDR_SET_CUR, unit, ncv->ncv_IfNum, UVCX_VIDEO_CONFIG_COMMIT,
                           (APTR) cfg, sizeof(*cfg), NULL);
    }
    return ioerr;
}
/* \\\ */

/* /// "uvcNegotiateEmbedded()" */
/* A mode of H.264 inside MJPEG frames (<devices/usb_video.h>): the
   camera's H.264 unit is asked for the size and rate, and the MJPEG stream
   that will carry it is negotiated while the unit's answer is in force,
   because the camera's numbers for the stream depend on it. The carrier is
   the smallest picture the MJPEG format has: nobody looks at it, and a
   picture of the H.264's own size beside the H.264 is more than the bus
   carries at 1920x1080. Then the unit gets back what it had: the camera puts
   H.264 into its frames only while a stream of such a mode runs (uvcStart()
   writes the answer again; the camera drops it by itself when the stream
   ends), and is a plain MJPEG camera to everybody else.

   What the unit had is also what the mode is made of: profile, rate
   control and the rest are taken as found, and so are the bit rate and the
   time between key frames where the request names none (0). On the camera
   at hand that is then the unit's own choice: it forgets what it was given
   when a stream ends. Returns 0 or a UVERR_ code. */
LONG uvcNegotiateEmbedded(struct NepClassVideo *ncv, struct NepVideoStream *ns,
                          struct UvcFormat *ufo, struct UvcFrame *ufr, ULONG interval,
                          ULONG bitrate, ULONG keyinterval)
{
    ULONG unit = uvcFindH264Unit(ncv->ncv_Function)->ue_ID;
    struct UvcxVideoConfig was;
    struct UvcxVideoConfig cfg;
    LONG ioerr;
    LONG err;

    if((ioerr = uvcRequest(ncv, UVUDR_GET_CUR, unit, ncv->ncv_IfNum, UVCX_VIDEO_CONFIG_PROBE, &was, sizeof(was), NULL)))
    {
        return (ioerr == UHIOERR_STALL) ? UVERR_REFUSED : UVERR_USB;
    }
    /* what the unit has, with this size and rate held to - beside whatever
       it was told to hold to before - as a byte stream */
    ULONG hints = AROS_LE2WORD(was.bmHints)|UVCXHF_RESOLUTION|UVCXHF_FRAME_INTERVAL;

    cfg = was;
    cfg.wWidth = AROS_WORD2LE(ufr->ufr_Width);
    cfg.wHeight = AROS_WORD2LE(ufr->ufr_Height);
    cfg.dwFrameInterval = AROS_LONG2LE(interval);
    /* ...and with what the request asks of the encoder */
    if(bitrate)
    {
        cfg.dwBitRate = AROS_LONG2LE(bitrate);
        hints |= UVCXHF_BIT_RATE;
    }
    if(keyinterval)
    {
        cfg.wIFramePeriod = AROS_WORD2LE(min(keyinterval, UVCX_IFRAME_PERIOD_MAX));
        hints |= UVCXHF_IFRAME_PERIOD;
    }
    cfg.bmHints = AROS_WORD2LE(hints);
    cfg.bStreamMuxOption = UVCXMF_ENABLE|UVCXMF_H264;
    cfg.bStreamFormat = 0;
    ioerr = uvcRequest(ncv, UVUDR_SET_CUR, unit, ncv->ncv_IfNum, UVCX_VIDEO_CONFIG_PROBE, &cfg, sizeof(cfg), NULL);
    if(!ioerr)
    {
        ioerr = uvcRequest(ncv, UVUDR_GET_CUR, unit, ncv->ncv_IfNum, UVCX_VIDEO_CONFIG_PROBE, &cfg, sizeof(cfg), NULL);
    }
    /* a camera that cannot do it answers with no size, or with another */
    if(ioerr || (AROS_LE2WORD(cfg.wWidth) != ufr->ufr_Width) || (AROS_LE2WORD(cfg.wHeight) != ufr->ufr_Height))
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "The camera's H.264 unit does not do %ldx%ld.",
                       (ULONG) ufr->ufr_Width, (ULONG) ufr->ufr_Height);
        return (ioerr && (ioerr != UHIOERR_STALL)) ? UVERR_USB : UVERR_REFUSED;
    }
    if((ioerr = uvcEmbedCommit(ncv, &cfg)))
    {
        return (ioerr == UHIOERR_STALL) ? UVERR_REFUSED : UVERR_USB;
    }
    struct UvcFrame *carrier = uvcSmallestFrame(ufo);
    err = uvcNegotiate(ncv, ns, ufo, carrier, uvcSnapInterval(carrier, interval));
    uvcEmbedCommit(ncv, &was);
    if(!err)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Unit %ld: H.264 %ldx%ld inside those frames, %ld bits a second, a key frame every %ld ms.",
                       ns->ns_UnitNo, (ULONG) ufr->ufr_Width, (ULONG) ufr->ufr_Height,
                       AROS_LE2LONG(cfg.dwBitRate), (ULONG) AROS_LE2WORD(cfg.wIFramePeriod));
        /* The mode is the H.264's, not the carrier's: its size and its
           rate are what the unit says of itself. Readers of the unit look
           under Forbid(). */
        Forbid();
        ns->ns_Mode.um_Frame = ufr;
        ns->ns_Mode.um_Interval = AROS_LE2LONG(cfg.dwFrameInterval);
        ns->ns_Mode.um_Embed = cfg;
        Permit();
    }
    return err;
}
/* \\\ */
