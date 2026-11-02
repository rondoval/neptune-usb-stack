#ifndef USBVIDEO_H
#define USBVIDEO_H

#include <devices/usb.h>
#include <devices/usb_video.h>
#include <devices/usbvideodevice.h>         /* struct UsbVideoStats */
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/lists.h>
#include <exec/io.h>
#include <utility/hooks.h>
#include <dos/dos.h>                  /* BPTR */

/*
 * The parsed description of one video function. Built once at bind time by
 * uvc_desc.c, never modified afterwards, freed with the binding. It is one
 * allocation; the variable-length parts - source lists, control bitmaps,
 * frame intervals, GUIDs - are not copied but point into the descriptors
 * neptune.library keeps for as long as the device exists.
 */

/* One row of the payload-format table: all the class knows about a format */
struct UvcPayload
{
    UBYTE               up_FormatType;    /* format descriptor subtype */
    UBYTE               up_FrameType;     /* frame descriptor subtype, 0 = has none */
    UBYTE               up_Framing;       /* UPFR_ */
    UBYTE               up_Flags;         /* UPF_ */
    UBYTE               up_FormatSize;    /* shortest valid format descriptor */
    UBYTE               up_DefFrame;      /* format: offset of the default frame index */
    UBYTE               up_FrameSize;     /* shortest valid frame descriptor */
    UBYTE               up_Width;         /* frame: offsets of width, height (words), */
    UBYTE               up_Height;
    UBYTE               up_DefInterval;   /* the default interval (long), */
    UBYTE               up_IntervalType;  /* the interval count (0 = min/max/step) */
    UBYTE               up_Intervals;     /* and the first interval */
    UBYTE               up_GUID;          /* format: offset of the GUID that names it, the bits per
                                             pixel behind it; 0 = it has none, up_FourCC names it */
    UBYTE               up_Usage;         /* what to ask for in bUsage of a 48 byte negotiation
                                             block; 0 = the format has no such thing */
    UBYTE               up_InfoBytes;     /* bytes of flags in a payload header; the time stamp
                                             and the clock pair follow them */
    ULONG               up_FourCC;        /* the name the mode list reports, if up_GUID is 0 */
};

#define UPFR_FRAME      0   /* FID / EOF delimit samples */
#define UPFR_STREAM     1   /* no delimiting */
#define UPFR_TEMPORAL   2   /* EOF required, slices */

#define UPF_FIXEDSIZE   0x01 /* every sample has the size of its picture */
#define UPF_JPEGSTART   0x02 /* a sample starts with a JPEG start-of-image marker */
#define UPF_EMBEDDED    0x04 /* the sample is H.264 that the camera put into a JPEG frame
                                (<devices/usb_video.h>): what is handed over is that */

/* JPEG, as far as the class looks into a frame. A picture begins with the
   start-of-image marker. Segments follow, each a marker - 0xff and a code -
   and a length: a big-endian word that counts itself and the segment's
   data. Behind the start-of-scan segment comes the picture data. */
#define JPEG_MARKER         0xff
#define JPEG_SOI            0xd8 /* start of image */
#define JPEG_SOS            0xda /* start of scan */
#define JPEG_MARKER_SIZE    2
#define JPEG_LENGTH_SIZE    2

struct UvcFrame
{
    UBYTE               ufr_Index;        /* bFrameIndex */
    UBYTE               ufr_NumIntervals; /* 0 = continuous: minimum, maximum, step */
    UWORD               ufr_Width;
    UWORD               ufr_Height;
    ULONG               ufr_DefInterval;  /* 100 ns units, inside the supported range */
    const ULONG        *ufr_Intervals;    /* in the descriptor: little-endian, at any address, shortest first */
};

struct UvcFormat
{
    UBYTE               ufo_Index;        /* bFormatIndex */
    UBYTE               ufo_Type;         /* format descriptor subtype */
    UBYTE               ufo_NumFrames;    /* frames parsed, not the descriptor's count */
    UBYTE               ufo_DefFrame;     /* bDefaultFrameIndex */
    const struct UvcPayload *ufo_Payload; /* NULL = present, but not streamable */
    struct UvcFrame    *ufo_Frames;
    const UBYTE        *ufo_Desc;         /* the format descriptor */
};

struct UvcStreamDesc
{
    UBYTE               us_IfNum;         /* the VideoStreaming interface */
    UBYTE               us_HeaderType;    /* input or output header subtype, 0 = none found */
    UBYTE               us_EPAddr;        /* bEndpointAddress of the video endpoint */
    UBYTE               us_Info;          /* input header bmInfo */
    UBYTE               us_TerminalLink;  /* the terminal this stream is attached to */
    UBYTE               us_StillMethod;   /* bStillCaptureMethod */
    UBYTE               us_NumFormats;    /* formats parsed */
    UBYTE               us_Pad;
    struct UvcFormat   *us_Formats;
};

struct UvcEntity
{
    UBYTE               ue_ID;
    UBYTE               ue_Type;          /* descriptor subtype (UDST_VIDEO_CTRL_) */
    UWORD               ue_TermType;      /* terminals: wTerminalType */
    UBYTE               ue_NumSources;
    UBYTE               ue_ControlSize;   /* bytes of ue_Controls */
    const UBYTE        *ue_Sources;       /* IDs this entity takes its input from */
    const UBYTE        *ue_Controls;      /* control bitmap, as the device declares it */
    const UBYTE        *ue_Desc;          /* the whole descriptor */
};

struct UvcFunction
{
    UWORD               uf_Version;       /* bcdUVC */
    ULONG               uf_ClockFrequency; /* Hz, the control header's: the unit of PTS and SCR */
    UWORD               uf_NumEntities;
    UWORD               uf_NumStreams;
    UWORD               uf_NumFormats;    /* over all streams */
    UWORD               uf_NumFrames;     /* over all formats */
    UWORD               uf_Skipped;       /* descriptors dropped as malformed */
    struct UvcEntity   *uf_Entities;
    struct UvcStreamDesc *uf_Streams;
    struct UvcFormat   *uf_Formats;
    struct UvcFrame    *uf_Frames;
};

/* uvc_desc.c - descriptor bytes to model. No USB I/O and no allocation: the
   caller counts the class-specific descriptors, provides the memory and feeds
   them in descriptor order, the VideoControl interface's first. */

ULONG uvcFunctionSize(ULONG ctrldescs, ULONG streams, ULONG streamdescs);
struct UvcFunction * uvcInitFunction(APTR mem, ULONG ctrldescs, ULONG streams, ULONG streamdescs);
void uvcAddControlDesc(struct UvcFunction *uf, const UBYTE *desc, ULONG len);
struct UvcStreamDesc * uvcAddStream(struct UvcFunction *uf, ULONG ifnum);
void uvcAddStreamDesc(struct UvcFunction *uf, struct UvcStreamDesc *us, const UBYTE *desc, ULONG len);

void uvcAddEmbeddedFormat(struct UvcFunction *uf, struct UvcStreamDesc *us);
struct UvcEntity * uvcFindEntity(const struct UvcFunction *uf, ULONG id);
struct UvcEntity * uvcFindH264Unit(const struct UvcFunction *uf);
ULONG uvcFrameInterval(const struct UvcFrame *ufr, ULONG idx);
ULONG uvcFrameMinInterval(const struct UvcFrame *ufr);
ULONG uvcFrameMaxInterval(const struct UvcFrame *ufr);

/* Looking modes up. A format counts as streamable when the class has a
   payload row for it and at least one of its frames parsed. */
BOOL uvcIsStreamable(const struct UvcFormat *ufo);
ULONG uvcFourCC(const struct UvcFormat *ufo);
ULONG uvcPictureSize(const struct UvcFormat *ufo, const struct UvcFrame *ufr);
struct UvcFormat * uvcFindFormat(const struct UvcStreamDesc *us, ULONG fourcc);
struct UvcFrame * uvcFindFrame(const struct UvcFormat *ufo, ULONG width, ULONG height);
struct UvcFrame * uvcSmallestFrame(const struct UvcFormat *ufo);
struct UvcFrame * uvcDefaultFrame(const struct UvcFormat *ufo, ULONG width, ULONG height);
ULONG uvcDefaultInterval(const struct UvcFrame *ufr, ULONG width, ULONG height, ULONG interval);
struct UvcFormat * uvcFormatByIndex(const struct UvcStreamDesc *us, ULONG index);
struct UvcFrame * uvcFrameByIndex(const struct UvcFormat *ufo, ULONG index);
ULONG uvcSnapInterval(const struct UvcFrame *ufr, ULONG interval);

struct NepVideoBase
{
    struct Library      nh_Library;       /* standard */
    UWORD               nh_Flags;         /* various flags */

    BPTR                nh_SegList;       /* load seglist (stored by the class skeleton) */
    struct Library     *nh_UtilityBase;   /* utility base */

    struct NepVideoDevBase *nh_DevBase;   /* base of device created */
    struct List         nh_Units;         /* List of units (struct NepVideoStream) */
    struct List         nh_Bindings;      /* Bindings whose subtask runs (struct NepClassVideo) */
};

struct NepVideoDevBase
{
    struct Library      np_Library;       /* standard */
    UWORD               np_Flags;         /* various flags */

    BPTR                np_SegList;       /* device seglist */
    struct NepVideoBase *np_ClsBase;      /* pointer to class base */
    struct Library     *np_UtilityBase;   /* cached utilitybase */
};

/* Status events (uvc_status.c): one transfer waits at the camera's status
   endpoint, if it has one, and what comes goes to whoever waits for it */
struct UvcStatus
{
    struct PsdPipe     *ust_Pipe;         /* NULL = no status endpoint, or it failed for good */
    UBYTE              *ust_Buf;          /* One status packet */
    ULONG               ust_Size;
    ULONG               ust_Errors;       /* Transfers failed in a row */
    struct List         ust_Queue;        /* NSCMD_UV_WAITEVENT requests; every access under Forbid() */
};

/* One bound VideoControl interface = one video function */
struct NepClassVideo
{
    struct Node         ncv_Node;         /* In nh_Bindings while ncv_Task is set */
    struct NepVideoBase *ncv_ClsBase;     /* Up linkage */
    struct Library     *ncv_Base;         /* neptune.library base */
    struct PsdDevice   *ncv_Device;       /* Up linkage */
    struct PsdInterface *ncv_Interface;   /* The VideoControl interface */
    struct Task        *ncv_ReadySigTask; /* Task to send ready signal to */
    LONG                ncv_ReadySignal;  /* Signal to send when ready */
    struct Task        *ncv_Task;         /* Subtask */
    struct MsgPort     *ncv_TaskMsgPort;  /* Message Port of Subtask (pipe replies) */
    struct MsgPort     *ncv_CmdPort;      /* Device commands for the subtask */
    struct PsdPipe     *ncv_EP0Pipe;      /* Endpoint 0 pipe */
    ULONG               ncv_IfNum;        /* Number of the VideoControl interface */

    struct UvcFunction *ncv_Function;     /* Parsed descriptors */

    struct UvcStatus    ncv_Status;       /* The camera's status events */

    /* What is stored for this camera (struct UvcCfgChunk) is filed under these */
    STRPTR              ncv_DevIDString;  /* Device ID String */
    STRPTR              ncv_IfIDString;   /* Interface ID String */
    BOOL                ncv_UsingDefaultCfg; /* Nothing is stored for it */

    /* The settings window (usbvideo.gui.c) */
    struct Task        *ncv_GUITask;      /* Its task, NULL = not open */
    struct Library     *ncv_MUIBase;      /* MUI master base of that task (mui_base.h) */
};

/*
 * What the settings window stores for a camera, in the library's per-device
 * settings: two chunks. The class writes the control values in the order of
 * their records - whoever stores them puts them in an order that works,
 * automatic modes before what they govern.
 */
struct UvcCfgChunk
{
    ULONG               ucc_ChunkID;      /* UVCFG_ */
    ULONG               ucc_Length;       /* bytes of ucc_Data */
    UBYTE               ucc_Data[2];
};

#define UVCFG_VALUES    0x55564356 /* 'UVCV': records of struct UvcCfgValue */
#define UVCFG_MODES     0x5556434d /* 'UVCM': records of struct UvcCfgMode */

/* One control value, to be written with uvcControlWrite() */
struct UvcCfgValue
{
    UWORD               ucv_ID;           /* UVCID_ */
    UBYTE               ucv_Entity;       /* the terminal or unit that has the control */
    UBYTE               ucv_Pad;
    LONG                ucv_Value;
};

/* The default mode of one stream: what a request without a size gets */
struct UvcCfgMode
{
    UWORD               ucm_IfNum;        /* the stream's interface */
    UWORD               ucm_Width;        /* 0 x 0 = the camera's own default */
    UWORD               ucm_Height;
    ULONG               ucm_Interval;     /* 100 ns units, 0 = that size's default */
};

/* From the settings window to the binding's subtask, beside the device's
   commands for controls: write the values of the chunk at io_Data. Not a
   command of the device. */
#define UVCMD_APPLY     0xfffe

/*
 * The parts of a unit (struct NepVideoStream, below).
 */

/* The mode in force, valid while um_Format is set */
struct UvcMode
{
    struct UvcFormat   *um_Format;
    struct UvcFrame    *um_Frame;
    ULONG               um_Interval;      /* frame interval, 100 ns units */
    ULONG               um_MaxFrameSize;  /* no frame is longer */
    ULONG               um_ClockFrequency; /* Hz, the unit of the frames' PTS; 0 = unknown */
    ULONG               um_PayloadSize;   /* bytes per payload transfer */
    struct UsbVideoProbeCommit um_Probe;  /* the negotiated block, as the camera sent it */
    struct UvcxVideoConfig um_Embed;      /* a mode of H.264 inside MJPEG frames: what the
                                             camera's H.264 unit agreed to */
};

/* The user's default mode, from the settings stored for the camera: what a
   request without a size gets. Zero = the camera's own. Written by the
   subtask under Forbid(). */
struct UvcDefault
{
    UWORD               ud_Width;
    UWORD               ud_Height;
    ULONG               ud_Interval;
};

/* The reads: CMD_READ requests waiting for a frame, and the one being
   filled. What applications, the subtask and the transport share: every
   access under Forbid() - but for the engine, which gives ur_Cur back
   without (see uvcDeliver()). */
struct UvcReads
{
    struct List         ur_Queue;
    struct IOStdReq    *ur_Cur;           /* the read being filled; NULL = nobody takes this frame */
    volatile BOOL       ur_AbortCur;      /* AbortIO() or CMD_FLUSH wants ur_Cur back */
};

/* The isochronous transport (uvc_iso.c) */
struct UvcIso
{
    struct PsdRTIsoHandler *ui_Handler;
    struct Hook         ui_ReqHook;
    struct Hook         ui_DoneHook;
};

/* The bulk transport (uvc_bulk.c). A payload may take several transfers. */
struct UvcBulk
{
    struct PsdPipe     *ub_Pipe;          /* the one transfer in flight */
    ULONG               ub_PayDone;       /* bytes of the payload in progress so far; 0 = between two */
    BOOL                ub_PaySkip;       /* it began with no valid header: the rest is not looked at */
    UWORD               ub_Errors;        /* transfers failed in a row */
};

/* Frame assembly: the frame and the payload in progress. While the stream
   runs only the transport's context touches these - the subtask stops the
   transport first. */
struct UvcAssembly
{
    BOOL                ua_Open;          /* inside a frame */
    BOOL                ua_Damaged;       /* the frame lost data or was flagged by the camera */
    BOOL                ua_Overflow;      /* the frame did not fit the read's buffer */
    BOOL                ua_HasPTS;
    BOOL                ua_HasSCR;
    UBYTE               ua_FID;           /* frame identifier of the open frame */
    UBYTE               ua_PayInfo;       /* header flags of the payload in progress */
    UWORD               ua_BusFrame;      /* USB frame number in which the frame began */
    ULONG               ua_Fill;          /* bytes of the frame stored so far */
    ULONG               ua_PTS;           /* its presentation time stamp, if ua_HasPTS */
    ULONG               ua_SCRClock;      /* the camera's clock and its count of USB frames at */
    UWORD               ua_SCRFrame;      /* the payload that opened the frame, if ua_HasSCR */
};

/*
 * One capture stream = one unit of usbvideo.device. Units outlive their
 * camera: they are kept in nh_Units until the class is expunged, so that an
 * application holding one open never looks at freed memory, and a camera
 * that comes back gets its unit number back - unless another camera was
 * given it meanwhile, which happens only to a unit nobody holds open (so an
 * application waiting with its unit open finds its camera there again).
 * ns_Binding says whether there
 * is a camera behind the unit; it changes under Forbid() only.
 */
struct NepVideoStream
{
    struct Unit         ns_Unit;          /* Unit structure */
    ULONG               ns_UnitNo;        /* Unit number */
    struct NepClassVideo *ns_Binding;     /* The function this stream belongs to, NULL = gone */
    struct UvcStreamDesc *ns_Desc;        /* Its description, in the binding's model */
    UWORD               ns_VendorID;      /* Which camera and stream the unit stands for */
    UWORD               ns_ProductID;
    UWORD               ns_IfNum;
    BOOL                ns_IsBulk;        /* Bulk transport (else isochronous) */
    ULONG               ns_OpenFlags;     /* UVOF_ of the opener */
    UBYTE               ns_ProductName[80];
    UBYTE               ns_SerialNumber[44];
    UBYTE               ns_Manufacturer[sizeof(((struct UsbVideoInfo *) NULL)->uvi_Manufacturer)];

    struct UvcMode      ns_Mode;          /* The mode in force */
    struct UvcDefault   ns_Default;       /* The mode a request without a size gets */
    struct UvcReads     ns_Reads;         /* Reads waiting, and the one being filled */

    /* The running stream; the subtask starts and stops it */
    BOOL                ns_Streaming;
    UBYTE              *ns_Scratch;       /* one service interval, or one bulk transfer, lands here */
    ULONG               ns_ScratchSize;
    struct UvcIso       ns_Iso;           /* its transport: this one, */
    struct UvcBulk      ns_Bulk;          /* or this one, as ns_IsBulk says */
    struct UvcAssembly  ns_Asm;           /* the frame being put together */
    struct UsbVideoStats ns_Stats;        /* since CMD_START; uvs_Frames numbers the frames */
};

#endif /* USBVIDEO_H */
