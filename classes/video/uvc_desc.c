/*
 *----------------------------------------------------------------------------
 *              usbvideo class: descriptors to function model
 *----------------------------------------------------------------------------
 * Pure: descriptor bytes in, model out. No USB I/O, no allocation, no
 * call into the system - the binding code walks the descriptors and feeds
 * them here.
 *
 * Rules (the specification's interoperability guidance, and what cameras in
 * the field need):
 *  - every descriptor is checked against its own variable parts before a
 *    field is read; one that fails is counted in uf_Skipped and dropped
 *  - unknown subtypes are skipped, trailing bytes ignored
 *  - counts a descriptor states about other descriptors are not trusted:
 *    what is recorded is what was parsed
 *  - sizes and bit rates of frame descriptors are not recorded at all: only
 *    the negotiated block says what a mode needs
 */

#include <string.h>

#include "usbvideo.h"

/* The wire structures are laid out as the specifications say: were the
   packing ever lost, the build would stop here */
_Static_assert(sizeof(struct UsbVideoProbeCommit) == 48, "probe/commit block");
_Static_assert(sizeof(struct UsbVideoStillProbeCommit) == 11, "still probe/commit block");
_Static_assert(sizeof(struct UsbVideoHeaderDesc) == 12, "control header");
_Static_assert(sizeof(struct UsbVideoCameraTermDesc) == 15, "camera terminal");
_Static_assert(sizeof(struct UsbVideoExtensionUnitDesc) == 22, "extension unit");
_Static_assert(sizeof(struct UsbVideoInputHeaderDesc) == 13, "input header");
_Static_assert(sizeof(struct UsbVideoFrameDesc) == 26, "uncompressed and MJPEG frame");
_Static_assert(sizeof(struct UsbVideoFrameBasedFrameDesc) == 26, "frame based frame");
_Static_assert(sizeof(struct UsbVideoH264FormatDesc) == 52, "H.264 format");
_Static_assert(sizeof(struct UsbVideoH264FrameDesc) == 44, "H.264 frame");
_Static_assert(sizeof(struct UsbVideoVP8FrameDesc) == 31, "VP8 frame");
_Static_assert(sizeof(struct UsbVideoSCR) == 6, "source clock reference");
_Static_assert(sizeof(struct UvcxVideoConfig) == 46, "H.264 unit configuration");
_Static_assert(sizeof(struct UvcxMuxHeader) == 22, "embedded stream header");
_Static_assert(sizeof(struct UsbVideoGenerateKeyFrame) == 1, "generate key frame");
_Static_assert(sizeof(struct UsbVideoSyncRefFrame) == 4, "sync and reference frame");

/* A frame-based format named HEVC: H.265, which has no UVC format of its own,
   under the name the class gives it (UVFCC_H265), whichever cameras use */
#define UVC_FOURCC_HEVC 0x48455643 /* 'HEVC' */

/* The payload-format table: one row is all the class knows about a format.
   A format without a row is recorded as present, but not streamable. The
   order of the rows is the order of preference: a request that names no
   format gets the camera's format of the earliest row. */
#define UVC_FORMAT(type) \
    sizeof(struct type), offsetof(struct type, bDefaultFrameIndex)
#define UVC_FRAME(type, countfield) \
    sizeof(struct type), offsetof(struct type, wWidth), offsetof(struct type, wHeight), \
    offsetof(struct type, dwDefaultFrameInterval), offsetof(struct type, countfield), \
    offsetof(struct type, dwFrameInterval)
#define UVC_GUID(type) offsetof(struct type, guidFormat)

static const struct UvcPayload UvcPayloads[] =
{
    {
        UDST_VIDEO_STREAM_FORMAT_MJPEG, UDST_VIDEO_STREAM_FRAME_MJPEG,
        UPFR_FRAME, UPF_JPEGSTART,
        UVC_FORMAT(UsbVideoMJPEGFormatDesc),
        UVC_FRAME(UsbVideoFrameDesc, bFrameIntervalType),
        0, 0, 1, UPKF_NONE, UVFCC_MJPG
    },
    {
        /* YUY2, NV12 and whatever else a camera names: its frames are laid
           out like the MJPEG ones */
        UDST_VIDEO_STREAM_FORMAT_UNCOMPRESSED, UDST_VIDEO_STREAM_FRAME_UNCOMPRESSED,
        UPFR_FRAME, UPF_FIXEDSIZE,
        UVC_FORMAT(UsbVideoUncompressedFormatDesc),
        UVC_FRAME(UsbVideoFrameDesc, bFrameIntervalType),
        UVC_GUID(UsbVideoUncompressedFormatDesc), 0, 1, UPKF_NONE, 0
    },
    {
        /* whatever a camera names that comes in frames of varying size:
           H.264 from cameras older than the H.264 format below, and H.265,
           which has no format of its own */
        UDST_VIDEO_STREAM_FORMAT_FRAME_BASED, UDST_VIDEO_STREAM_FRAME_FRAME_BASED,
        UPFR_FRAME, 0,
        UVC_FORMAT(UsbVideoFrameBasedFormatDesc),
        UVC_FRAME(UsbVideoFrameBasedFrameDesc, bFrameIntervalType),
        UVC_GUID(UsbVideoFrameBasedFormatDesc), 0, 1, UPKF_STREAM, 0
    },
    {
        /* asked for as a plain single stream: the usage every frame of the
           format has to offer (H.264 Payload 1.5, table 3-2, bmUsages D16) */
        UDST_VIDEO_STREAM_FORMAT_H264, UDST_VIDEO_STREAM_FRAME_H264,
        UPFR_TEMPORAL, 0,
        UVC_FORMAT(UsbVideoH264FormatDesc),
        UVC_FRAME(UsbVideoH264FrameDesc, bNumFrameIntervals),
        0, UVPCU_FILE_STORAGE_IP, 1, UPKF_ENCODER, UVFCC_H264
    },
    {
        /* three bytes of flags in its payload headers (VP8 Payload 1.5, table 2-2) */
        UDST_VIDEO_STREAM_FORMAT_VP8, UDST_VIDEO_STREAM_FRAME_VP8,
        UPFR_TEMPORAL, 0,
        UVC_FORMAT(UsbVideoVP8FormatDesc),
        UVC_FRAME(UsbVideoVP8FrameDesc, bNumFrameIntervals),
        0, UVPCU_FILE_STORAGE_IP, 3, UPKF_ENCODER, UVFCC_VP80
    },
    {
        /* H.264 that a camera from before these formats puts into the frames
           of its MJPEG format. No descriptor says so: uvcAddEmbeddedFormat()
           makes the entry, with the MJPEG format's frames. */
        0, 0,
        UPFR_FRAME, UPF_JPEGSTART|UPF_EMBEDDED,
        0, 0,
        0, 0, 0,
        0, 0, 0,
        0, 0, 1, UPKF_UVCX, UVFCC_H264
    },
};

#define UVC_NUM_PAYLOADS (sizeof(UvcPayloads) / sizeof(UvcPayloads[0]))
#define UVC_ROW_MJPEG    (&UvcPayloads[0])
#define UVC_ROW_EMBEDDED (&UvcPayloads[UVC_NUM_PAYLOADS - 1])

/* A format GUID that stands for a FourCC is the four characters followed by
   these twelve bytes (Uncompressed Payload 1.5, table 2-1, in wire order) */
static const UBYTE UvcFourCCGUID[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };

#define UVC_FOURCC_CHARS      4
#define UVC_FOURCC_CHAR_FIRST ' ' /* printable; spaces pad a short name */
#define UVC_FOURCC_CHAR_LAST  '~'

/* The descriptor subtypes that open a format group (UVC 1.5, A.6) */
#define UVC_FORMAT_TYPES ((1UL << UDST_VIDEO_STREAM_FORMAT_UNCOMPRESSED) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_MJPEG) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_MPEG2TS) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_DV) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_FRAME_BASED) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_STREAM_BASED) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_H264) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_H264_SIMULCAST) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_VP8) | \
                          (1UL << UDST_VIDEO_STREAM_FORMAT_VP8_SIMULCAST))

/* Does a format GUID stand for a FourCC? A Direct3D format's GUID has the
   same twelve bytes behind a number (D3DFMT_L8: 0x32), not characters. */
static BOOL uvcIsFourCCGUID(const UBYTE *guid)
{
    for(ULONG cnt = 0; cnt < UVC_FOURCC_CHARS; cnt++)
    {
        if((guid[cnt] < UVC_FOURCC_CHAR_FIRST) || (guid[cnt] > UVC_FOURCC_CHAR_LAST))
        {
            return FALSE;
        }
    }
    for(ULONG cnt = 0; cnt < sizeof(UvcFourCCGUID); cnt++)
    {
        if(guid[UVC_FOURCC_CHARS + cnt] != UvcFourCCGUID[cnt])
        {
            return FALSE;
        }
    }
    return TRUE;
}

/* The row for a format descriptor. A format named by a GUID has one only if
   the GUID stands for a FourCC: any other could not be named in the mode
   list. */
static const struct UvcPayload * uvcFindPayload(ULONG formattype, const UBYTE *desc, ULONG len)
{
    for(ULONG cnt = 0; cnt < UVC_NUM_PAYLOADS; cnt++)
    {
        const struct UvcPayload *up = &UvcPayloads[cnt];
        if((up->up_FormatType == formattype) && (len >= up->up_FormatSize) &&
           (!up->up_GUID || uvcIsFourCCGUID(&desc[up->up_GUID])))
        {
            return up;
        }
    }
    return NULL;
}

/* /// "uvcFunctionSize()" */
/* The memory a model needs, given how many class specific descriptors the
   function's interfaces have */
ULONG uvcFunctionSize(ULONG ctrldescs, ULONG streams, ULONG streamdescs)
{
    /* a descriptor yields at most one entity, or one format, or one frame.
       The one format no descriptor stands for (uvcAddEmbeddedFormat()) takes
       a place that a frame descriptor left free. */
    return sizeof(struct UvcFunction) +
           ctrldescs * sizeof(struct UvcEntity) +
           streams * sizeof(struct UvcStreamDesc) +
           streamdescs * (sizeof(struct UvcFormat) + sizeof(struct UvcFrame));
}
/* \\\ */

/* /// "uvcInitFunction()" */
/* An empty model in memory of that size */
struct UvcFunction * uvcInitFunction(APTR mem, ULONG ctrldescs, ULONG streams, ULONG streamdescs)
{
    struct UvcFunction *uf = (struct UvcFunction *) mem;

    memset(mem, 0, uvcFunctionSize(ctrldescs, streams, streamdescs));
    uf->uf_Version = UVC_VERSION_10;
    uf->uf_Entities = (struct UvcEntity *) (uf + 1);
    uf->uf_Streams = (struct UvcStreamDesc *) (uf->uf_Entities + ctrldescs);
    uf->uf_Formats = (struct UvcFormat *) (uf->uf_Streams + streams);
    uf->uf_Frames = (struct UvcFrame *) (uf->uf_Formats + streamdescs);
    return uf;
}
/* \\\ */

/* /// "uvcAddControlDesc()" */
/* One class specific descriptor of the VideoControl interface: the header
   gives the revision and the clock, a terminal or unit becomes an entity.
   One that is shorter than what it says it holds is counted and dropped. */
void uvcAddControlDesc(struct UvcFunction *uf, const UBYTE *desc, ULONG len)
{
    struct UvcEntity ue = { 0 };
    BOOL valid = FALSE;

    if(len < sizeof(struct UsbVideoDesc))
    {
        uf->uf_Skipped++;
        return;
    }
    ue.ue_Type = ((const struct UsbVideoDesc *) desc)->bDescriptorSubType;
    ue.ue_Desc = desc;
    switch(ue.ue_Type)
    {
        case UDST_VIDEO_CTRL_HEADER:
        {
            const struct UsbVideoHeaderDesc *hd = (const struct UsbVideoHeaderDesc *) desc;

            if(len >= sizeof(*hd))
            {
                uf->uf_Version = AROS_LE2WORD(hd->bcdUVC);
                uf->uf_ClockFrequency = AROS_LE2LONG(hd->dwClockFrequency);
                return;
            }
            break;
        }

        case UDST_VIDEO_CTRL_INPUT_TERMINAL:
        {
            /* a camera terminal is an input terminal with more behind it */
            const struct UsbVideoCameraTermDesc *ct = (const struct UsbVideoCameraTermDesc *) desc;

            if(len >= sizeof(struct UsbVideoInputTermDesc))
            {
                ue.ue_ID = ct->bTerminalID;
                ue.ue_TermType = AROS_LE2WORD(ct->wTerminalType);
                if((ue.ue_TermType == UVITT_CAMERA) && (len >= sizeof(*ct)) && (len >= sizeof(*ct) + ct->bControlSize))
                {
                    ue.ue_ControlSize = ct->bControlSize;
                    ue.ue_Controls = ct->bmControls;
                }
                /* a terminal type without a high byte could not be told from a unit */
                valid = (ue.ue_TermType & 0xff00) != 0;
            }
            break;
        }

        case UDST_VIDEO_CTRL_OUTPUT_TERMINAL:
        {
            const struct UsbVideoOutputTermDesc *ot = (const struct UsbVideoOutputTermDesc *) desc;

            if(len >= sizeof(*ot))
            {
                ue.ue_ID = ot->bTerminalID;
                ue.ue_TermType = AROS_LE2WORD(ot->wTerminalType);
                ue.ue_NumSources = 1;
                ue.ue_Sources = &ot->bSourceID;
                valid = (ue.ue_TermType & 0xff00) != 0;
            }
            break;
        }

        case UDST_VIDEO_CTRL_SELECTOR_UNIT:
        {
            const struct UsbVideoSelectorUnitDesc *su = (const struct UsbVideoSelectorUnitDesc *) desc;
            ULONG fixed = sizeof(*su) + 1; /* and the string index behind the source IDs */

            if((len >= fixed) && (len >= fixed + su->bNrInPins))
            {
                ue.ue_ID = su->bUnitID;
                ue.ue_NumSources = su->bNrInPins;
                ue.ue_Sources = su->baSourceID;
                valid = TRUE;
            }
            break;
        }

        case UDST_VIDEO_CTRL_PROCESSING_UNIT:
        {
            const struct UsbVideoProcessingUnitDesc *pu = (const struct UsbVideoProcessingUnitDesc *) desc;
            /* behind the bitmap: the string index and, appended by UVC 1.1,
               the video standards bitmap */
            ULONG fixed = sizeof(*pu) + 1 + ((uf->uf_Version >= UVC_VERSION_11) ? 1 : 0);

            if((len >= sizeof(*pu) + 1) && (len >= fixed + pu->bControlSize))
            {
                ue.ue_ID = pu->bUnitID;
                ue.ue_NumSources = 1;
                ue.ue_Sources = &pu->bSourceID;
                ue.ue_ControlSize = pu->bControlSize;
                ue.ue_Controls = pu->bmControls;
                valid = TRUE;
            }
            break;
        }

        case UDST_VIDEO_CTRL_EXTENSION_UNIT:
        {
            const struct UsbVideoExtensionUnitDesc *xu = (const struct UsbVideoExtensionUnitDesc *) desc;
            ULONG fixed = sizeof(*xu) + 2; /* and the bitmap size and the string index */

            if((len >= fixed) && (len >= fixed + xu->bNrInPins))
            {
                ULONG pins = xu->bNrInPins;
                const UBYTE *bitmap = &xu->baSourceID[pins]; /* its size, then its bytes */
                ULONG ctrlsize = bitmap[0];

                if(len >= fixed + pins + ctrlsize)
                {
                    ue.ue_ID = xu->bUnitID;
                    ue.ue_NumSources = pins;
                    ue.ue_Sources = xu->baSourceID;
                    /* the bitmap says which controls exist; bNumControls is not trusted */
                    ue.ue_ControlSize = ctrlsize;
                    ue.ue_Controls = &bitmap[1];
                    valid = TRUE;
                }
            }
            break;
        }

        case UDST_VIDEO_CTRL_ENCODING_UNIT:
        {
            const struct UsbVideoEncodingUnitDesc *eu = (const struct UsbVideoEncodingUnitDesc *) desc;

            /* two bitmaps of the stated size: controls, and controls at run time */
            if((len >= sizeof(*eu)) &&
               (len >= offsetof(struct UsbVideoEncodingUnitDesc, bmControls) + 2 * (ULONG) eu->bControlSize))
            {
                ue.ue_ID = eu->bUnitID;
                ue.ue_NumSources = 1;
                ue.ue_Sources = &eu->bSourceID;
                ue.ue_ControlSize = eu->bControlSize;
                ue.ue_Controls = eu->bmControls;
                valid = TRUE;
            }
            break;
        }

        default: /* a subtype of a later revision */
            return;
    }
    if(valid)
    {
        uf->uf_Entities[uf->uf_NumEntities++] = ue;
    } else {
        uf->uf_Skipped++;
    }
}
/* \\\ */

/* /// "uvcAddStream()" */
/* A VideoStreaming interface begins; its descriptors follow through
   uvcAddStreamDesc() */
struct UvcStreamDesc * uvcAddStream(struct UvcFunction *uf, ULONG ifnum)
{
    struct UvcStreamDesc *us = &uf->uf_Streams[uf->uf_NumStreams++];

    us->us_IfNum = ifnum;
    us->us_Formats = &uf->uf_Formats[uf->uf_NumFormats];
    return us;
}
/* \\\ */

/* /// "uvcAddStreamHeader()" */
/* The header of a VideoStreaming interface, input or output: where its
   stream comes from. FALSE = shorter than what it has to hold. */
static BOOL uvcAddStreamHeader(struct UvcStreamDesc *us, ULONG subtype, const UBYTE *desc, ULONG len)
{
    if(subtype == UDST_VIDEO_STREAM_INPUT_HEADER)
    {
        const struct UsbVideoInputHeaderDesc *ih = (const struct UsbVideoInputHeaderDesc *) desc;

        if(len < sizeof(*ih))
        {
            return FALSE;
        }
        us->us_EPAddr = ih->bEndpointAddress;
        us->us_Info = ih->bmInfo;
        us->us_TerminalLink = ih->bTerminalLink;
        us->us_StillMethod = ih->bStillCaptureMethod;
        /* a row per format, if all of them are there */
        if(len >= sizeof(*ih) + (ULONG) ih->bNumFormats * ih->bControlSize)
        {
            us->us_ControlSize = ih->bControlSize;
            us->us_ControlRows = ih->bNumFormats;
            us->us_Controls = ih->bmaControls;
        }
    } else {
        const struct UsbVideoOutputHeaderDesc *oh = (const struct UsbVideoOutputHeaderDesc *) desc;

        /* UVC 1.0 ends before the bitmap size */
        if(len < offsetof(struct UsbVideoOutputHeaderDesc, bControlSize))
        {
            return FALSE;
        }
        us->us_EPAddr = oh->bEndpointAddress;
        us->us_TerminalLink = oh->bTerminalLink;
    }
    us->us_HeaderType = subtype;
    return TRUE;
}
/* \\\ */

/* /// "uvcAddFormat()" */
/* A format descriptor opens a format, whose frames follow. It is always
   recorded, so that those frames never attach to the format before it;
   whether it can be streamed is the row's business. */
static void uvcAddFormat(struct UvcFunction *uf, struct UvcStreamDesc *us, ULONG subtype, const UBYTE *desc, ULONG len)
{
    struct UvcFormat *ufo = &uf->uf_Formats[uf->uf_NumFormats++];

    us->us_NumFormats++;
    ufo->ufo_Type = subtype;
    ufo->ufo_Desc = desc;
    ufo->ufo_Index = (len > offsetof(struct UsbVideoFormatDesc, bFormatIndex))
                     ? ((const struct UsbVideoFormatDesc *) desc)->bFormatIndex : 0;
    ufo->ufo_Payload = uvcFindPayload(subtype, desc, len);
    ufo->ufo_DefFrame = ufo->ufo_Payload ? desc[ufo->ufo_Payload->up_DefFrame] : 0;
    ufo->ufo_Frames = &uf->uf_Frames[uf->uf_NumFrames];
    /* the header comes before the formats; its rows go by format index */
    BOOL hasrow = us->us_ControlSize && ufo->ufo_Index && (ufo->ufo_Index <= us->us_ControlRows);
    ufo->ufo_Controls = hasrow ? us->us_Controls[(ufo->ufo_Index - 1) * us->us_ControlSize] : 0;
}
/* \\\ */

/* /// "uvcAddFrame()" */
/* A descriptor that is neither header nor format. A frame descriptor does
   not name its format: it belongs to the format opened last, and it is one
   of its frames if it has the subtype that format's row gives its frames.
   Anything else passes by - still image and colour matching descriptors, and
   the frames of formats without a row. FALSE = a frame, but shorter than
   what it says it holds. */
static BOOL uvcAddFrame(struct UvcFunction *uf, struct UvcStreamDesc *us, ULONG subtype, const UBYTE *desc, ULONG len)
{
    if(!us->us_NumFormats)
    {
        return TRUE;
    }
    struct UvcFormat *ufo = &us->us_Formats[us->us_NumFormats - 1];
    const struct UvcPayload *up = ufo->ufo_Payload;

    if(!up || !up->up_FrameType || (subtype != up->up_FrameType))
    {
        return TRUE;
    }
    if(len < up->up_FrameSize)
    {
        return FALSE;
    }
    ULONG numintervals = desc[up->up_IntervalType];
    if(len < up->up_Intervals + 4 * (numintervals ? numintervals : 3))
    {
        return FALSE;
    }

    struct UvcFrame *ufr = &uf->uf_Frames[uf->uf_NumFrames++];
    ufo->ufo_NumFrames++;
    /* where a frame's fields are is the row's knowledge: one parser for all
       the kinds of frame */
    ufr->ufr_Index = ((const struct UsbVideoFrameHeadDesc *) desc)->bFrameIndex;
    ufr->ufr_NumIntervals = numintervals;
    ufr->ufr_Width = AROS_LE2WORD(*(const UWORD *) &desc[up->up_Width]);
    ufr->ufr_Height = AROS_LE2WORD(*(const UWORD *) &desc[up->up_Height]);
    ufr->ufr_Intervals = (const ULONG *) &desc[up->up_Intervals];

    /* cameras state defaults they do not support */
    ULONG interval = AROS_LE2LONG(*(const ULONG *) &desc[up->up_DefInterval]);
    ULONG shortest = uvcFrameMinInterval(ufr);
    ULONG longest = uvcFrameMaxInterval(ufr);
    if(interval < shortest)
    {
        interval = shortest;
    }
    if(interval > longest)
    {
        interval = longest;
    }
    ufr->ufr_DefInterval = interval;
    return TRUE;
}
/* \\\ */

/* /// "uvcAddStreamDesc()" */
/* One class specific descriptor of a VideoStreaming interface: its header,
   a format, or a frame of the format before it. One that is shorter than
   what it says it holds is counted and dropped. */
void uvcAddStreamDesc(struct UvcFunction *uf, struct UvcStreamDesc *us, const UBYTE *desc, ULONG len)
{
    BOOL valid = FALSE;

    if(len >= sizeof(struct UsbVideoDesc))
    {
        ULONG subtype = ((const struct UsbVideoDesc *) desc)->bDescriptorSubType;

        if((subtype == UDST_VIDEO_STREAM_INPUT_HEADER) || (subtype == UDST_VIDEO_STREAM_OUTPUT_HEADER))
        {
            valid = uvcAddStreamHeader(us, subtype, desc, len);
        }
        else if((subtype < 32) && ((UVC_FORMAT_TYPES >> subtype) & 1))
        {
            uvcAddFormat(uf, us, subtype, desc, len);
            valid = TRUE;
        } else {
            valid = uvcAddFrame(uf, us, subtype, desc, len);
        }
    }
    if(!valid)
    {
        uf->uf_Skipped++;
    }
}
/* \\\ */

/* /// "uvcFindH264Unit()" */
/* The extension unit through which a camera from before the H.264 format is
   told to put H.264 into its MJPEG frames; NULL if it has none */
struct UvcEntity * uvcFindH264Unit(const struct UvcFunction *uf)
{
    static const UBYTE guid[16] = UVCX_H264_XU_GUID;

    for(ULONG cnt = 0; cnt < uf->uf_NumEntities; cnt++)
    {
        struct UvcEntity *ue = &uf->uf_Entities[cnt];
        ULONG same = 0;

        if(ue->ue_Type != UDST_VIDEO_CTRL_EXTENSION_UNIT)
        {
            continue;
        }
        const struct UsbVideoExtensionUnitDesc *xu = (const struct UsbVideoExtensionUnitDesc *) ue->ue_Desc;
        while((same < sizeof(guid)) && (xu->guidExtensionCode[same] == guid[same]))
        {
            same++;
        }
        if(same == sizeof(guid))
        {
            return ue;
        }
    }
    return NULL;
}
/* \\\ */

/* /// "uvcAddEmbeddedFormat()" */
/* After a stream's descriptors, before the next stream's: if the function
   has the H.264 unit and the stream an MJPEG format, the stream gets one
   more format - H.264, in every size the MJPEG format has. Its entry is the
   MJPEG format's under another row: same descriptor, same frames, same index
   on the wire. */
void uvcAddEmbeddedFormat(struct UvcFunction *uf, struct UvcStreamDesc *us)
{
    if(!uvcFindH264Unit(uf))
    {
        return;
    }
    for(ULONG cnt = 0; cnt < us->us_NumFormats; cnt++)
    {
        struct UvcFormat *ufo = &us->us_Formats[cnt];
        if((ufo->ufo_Payload == UVC_ROW_MJPEG) && ufo->ufo_NumFrames)
        {
            /* There is a place for it: the array has one per descriptor of
               the streams (uvcFunctionSize()), and this format has a frame,
               whose descriptor took none. */
            struct UvcFormat *twin = &uf->uf_Formats[uf->uf_NumFormats++];

            us->us_NumFormats++;
            *twin = *ufo;
            twin->ufo_Payload = UVC_ROW_EMBEDDED;
            return;
        }
    }
}
/* \\\ */

/* /// "uvcFindEntity()" */
/* The terminal or unit with this ID; NULL = there is none */
struct UvcEntity * uvcFindEntity(const struct UvcFunction *uf, ULONG id)
{
    for(ULONG cnt = 0; cnt < uf->uf_NumEntities; cnt++)
    {
        if(uf->uf_Entities[cnt].ue_ID == id)
        {
            return &uf->uf_Entities[cnt];
        }
    }
    return NULL;
}
/* \\\ */

/* /// "uvcPathEntity()" */
/* The first terminal or unit of a subtype (UDST_VIDEO_CTRL_) on the path
   that ends in a stream's terminal, walked from there towards the source
   along each entity's first input; NULL = the path has none */
struct UvcEntity * uvcPathEntity(const struct UvcFunction *uf, const struct UvcStreamDesc *us, ULONG type)
{
    struct UvcEntity *ue = uvcFindEntity(uf, us->us_TerminalLink);

    /* an entity count bounds the walk: a malformed loop of sources ends too */
    for(ULONG steps = 0; ue && (steps < uf->uf_NumEntities); steps++)
    {
        if(ue->ue_Type == type)
        {
            return ue;
        }
        ue = ue->ue_NumSources ? uvcFindEntity(uf, ue->ue_Sources[0]) : NULL;
    }
    return NULL;
}
/* \\\ */

/* /// "uvcFrameInterval()" */
/* The intervals stay in the descriptor; a list has ufr_NumIntervals entries,
   a continuous range (ufr_NumIntervals 0) minimum, maximum and step. */
ULONG uvcFrameInterval(const struct UvcFrame *ufr, ULONG idx)
{
    ULONG interval = AROS_LE2LONG(ufr->ufr_Intervals[idx]);

    /* cameras list intervals of zero; never hand out a divisor of 0 */
    return interval ? interval : 1;
}

ULONG uvcFrameMinInterval(const struct UvcFrame *ufr)
{
    return uvcFrameInterval(ufr, 0);
}

ULONG uvcFrameMaxInterval(const struct UvcFrame *ufr)
{
    return uvcFrameInterval(ufr, ufr->ufr_NumIntervals ? ufr->ufr_NumIntervals - 1 : 1);
}
/* \\\ */


/* /// "uvcIsStreamable()" */
/* Can the class stream this format? It needs a row in the payload table
   and at least one frame that parsed. */
BOOL uvcIsStreamable(const struct UvcFormat *ufo)
{
    return ufo->ufo_Payload && ufo->ufo_NumFrames;
}
/* \\\ */

/* /// "uvcFourCC()" */
/* The name of a streamable format: the row's, or the four characters its
   GUID begins with - HEVC listed as H265 */
ULONG uvcFourCC(const struct UvcFormat *ufo)
{
    const struct UvcPayload *up = ufo->ufo_Payload;

    if(!up->up_GUID)
    {
        return up->up_FourCC;
    }

    /* four characters in reading order: a big-endian long */
    ULONG fourcc = AROS_BE2LONG(*(const ULONG *) &ufo->ufo_Desc[up->up_GUID]);

    return (fourcc == UVC_FOURCC_HEVC) ? UVFCC_H265 : fourcc;
}
/* \\\ */

/* /// "uvcPictureSize()" */
/* The bytes of one picture of a format whose samples all have that size;
   0 = the format does not say (it is compressed, or states no depth) */
ULONG uvcPictureSize(const struct UvcFormat *ufo, const struct UvcFrame *ufr)
{
    const struct UvcPayload *up = ufo->ufo_Payload;

    if(!(up->up_Flags & UPF_FIXEDSIZE))
    {
        return 0;
    }
    /* only an uncompressed format has samples of one size; a planar 4:2:0
       format has 12 bits a pixel */
    const struct UsbVideoUncompressedFormatDesc *fd = (const struct UsbVideoUncompressedFormatDesc *) ufo->ufo_Desc;
    return (ULONG) ufr->ufr_Width * ufr->ufr_Height * fd->bBitsPerPixel / 8;
}
/* \\\ */

/* /// "uvcFindFormat()" */
/* By FourCC; 0 asks for the format a request gets that names none: of the
   streamable ones the one whose row comes first in the table, so that a
   camera with MJPEG answers with MJPEG wherever it lists it */
struct UvcFormat * uvcFindFormat(const struct UvcStreamDesc *us, ULONG fourcc)
{
    struct UvcFormat *best = NULL;

    for(ULONG cnt = 0; cnt < us->us_NumFormats; cnt++)
    {
        struct UvcFormat *ufo = &us->us_Formats[cnt];
        if(!uvcIsStreamable(ufo))
        {
            continue;
        }
        if(fourcc)
        {
            if(uvcFourCC(ufo) == fourcc)
            {
                return ufo;
            }
        }
        else if(!best || (ufo->ufo_Payload < best->ufo_Payload))
        {
            best = ufo;
        }
    }
    return best;
}
/* \\\ */

/* /// "uvcFindFrame()" */
/* By size; 0 x 0 asks for the format's default frame - the first one if the
   descriptor names a default that does not exist */
struct UvcFrame * uvcFindFrame(const struct UvcFormat *ufo, ULONG width, ULONG height)
{
    for(ULONG cnt = 0; cnt < ufo->ufo_NumFrames; cnt++)
    {
        struct UvcFrame *ufr = &ufo->ufo_Frames[cnt];
        if((width || height) ? ((ufr->ufr_Width == width) && (ufr->ufr_Height == height))
                             : (ufr->ufr_Index == ufo->ufo_DefFrame))
        {
            return ufr;
        }
    }
    return (width || height || !ufo->ufo_NumFrames) ? NULL : ufo->ufo_Frames;
}
/* \\\ */

/* /// "uvcSmallestFrame()" */
/* The frame of a streamable format with the fewest pixels */
struct UvcFrame * uvcSmallestFrame(const struct UvcFormat *ufo)
{
    struct UvcFrame *smallest = ufo->ufo_Frames;

    for(ULONG cnt = 1; cnt < ufo->ufo_NumFrames; cnt++)
    {
        struct UvcFrame *ufr = &ufo->ufo_Frames[cnt];
        if((ULONG) ufr->ufr_Width * ufr->ufr_Height < (ULONG) smallest->ufr_Width * smallest->ufr_Height)
        {
            smallest = ufr;
        }
    }
    return smallest;
}
/* \\\ */

/* /// "uvcDefaultFrame()" */
/* The frame a request without a size gets: the one of the user's default
   size if there is one and the format has it, else the format's own */
struct UvcFrame * uvcDefaultFrame(const struct UvcFormat *ufo, ULONG width, ULONG height)
{
    struct UvcFrame *ufr = (width || height) ? uvcFindFrame(ufo, width, height) : NULL;

    return ufr ? ufr : uvcFindFrame(ufo, 0, 0);
}
/* \\\ */

/* /// "uvcDefaultInterval()" */
/* The interval a request without one gets for a frame: the user's default
   interval if the frame has the user's default size, else the frame's own;
   either way one the frame has */
ULONG uvcDefaultInterval(const struct UvcFrame *ufr, ULONG width, ULONG height, ULONG interval)
{
    BOOL users = (ufr->ufr_Width == width) && (ufr->ufr_Height == height);

    return uvcSnapInterval(ufr, users ? interval : 0);
}
/* \\\ */

/* /// "uvcFormatByIndex()" */
/* By the one-based indices of the wire protocol */
struct UvcFormat * uvcFormatByIndex(const struct UvcStreamDesc *us, ULONG index)
{
    for(ULONG cnt = 0; cnt < us->us_NumFormats; cnt++)
    {
        if(us->us_Formats[cnt].ufo_Index == index)
        {
            return &us->us_Formats[cnt];
        }
    }
    return NULL;
}

struct UvcFrame * uvcFrameByIndex(const struct UvcFormat *ufo, ULONG index)
{
    for(ULONG cnt = 0; cnt < ufo->ufo_NumFrames; cnt++)
    {
        if(ufo->ufo_Frames[cnt].ufr_Index == index)
        {
            return &ufo->ufo_Frames[cnt];
        }
    }
    return NULL;
}
/* \\\ */

/* /// "uvcSnapInterval()" */
/* The supported frame interval nearest to the one asked for; 0 asks for the
   frame's default */
ULONG uvcSnapInterval(const struct UvcFrame *ufr, ULONG interval)
{
    if(!interval)
    {
        return ufr->ufr_DefInterval;
    }
    if(!ufr->ufr_NumIntervals)
    {
        /* a range: minimum, maximum, step */
        ULONG shortest = uvcFrameMinInterval(ufr);
        ULONG longest = uvcFrameMaxInterval(ufr);
        ULONG step = uvcFrameInterval(ufr, 2);
        if(interval <= shortest)
        {
            return shortest;
        }
        if(interval >= longest)
        {
            return longest;
        }
        return shortest + ((interval - shortest + step / 2) / step) * step;
    }
    ULONG best = uvcFrameInterval(ufr, 0);
    for(ULONG cnt = 1; cnt < ufr->ufr_NumIntervals; cnt++)
    {
        ULONG cand = uvcFrameInterval(ufr, cnt);
        ULONG canddist = (cand > interval) ? cand - interval : interval - cand;
        ULONG bestdist = (best > interval) ? best - interval : interval - best;
        if(canddist < bestdist)
        {
            best = cand;
        }
    }
    return best;
}
/* \\\ */
