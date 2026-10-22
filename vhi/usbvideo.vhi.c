/*
 *----------------------------------------------------------------------------
 *               usbvideo.vhi - VHI driver for USB video cameras
 *----------------------------------------------------------------------------
 * VHI is the video capture interface applications such as cAMIra and VHI
 * Studio speak: a library in LIBS:VHI/ with one function, vhi_api()
 * (<vhi/vhi.h>).
 * This one contains no USB code. It is a client of usbvideo.device, the
 * application interface of usbvideo.usbclass, and maps VHI onto it:
 *
 *   an input              a unit of usbvideo.device, that is a camera;
 *                         input n is unit n
 *   the maximum size      the largest picture the camera makes in the
 *                         format the driver takes from it
 *   scaling               the camera's other picture sizes: an application
 *                         that asks for a size gets the nearest the camera
 *                         has, one that asks for none the camera's default
 *   digitizing a picture  one CMD_READ, straight into the memory the
 *                         application's allocator returned
 *
 * A picture is handed over as the camera sent it, as a JPEG file in memory
 * (VHI_JPEG), with the standard Huffman tables put in if the camera left
 * them out. Nothing is decoded here. A camera that makes no JPEG pictures
 * is asked for raw ones (YUY2), and their bytes are sorted into the three
 * planes of VHI_YUV_422.
 *
 * VHI is a pull interface - one picture per call - and the device is a
 * stream. The stream is started by the first picture asked for and runs
 * until the driver is closed or the input or the picture size changes;
 * between two calls no read waits at the device, and the class skips the
 * frames at next to no cost. The driver remembers what it started (struct
 * VhiStream) and asks the unit nothing before a picture; what it remembers
 * is dropped when a read fails.
 *
 * A handle belongs to the task that made the VHI_METHOD_OPEN call: the reply
 * port of its requests is that task's, and a call from any other task is
 * refused.
 */

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <dos/dos.h>                  /* BPTR */
#include <devices/timer.h>
#include <devices/usbvideodevice.h>

#define EXEC_BASE_NAME SysBase        /* a local in every function that calls Exec */
#include <proto/exec.h>

#include <string.h>

#include <neptune_version.h>

#include <vhi/vhi.h>
#include "mjpegdht.h"

#define VHI_NAME          "usbvideo.vhi"
#define VHI_MAX_UNITS     8           /* units looked at for cameras: inputs 0..7 */
#define VHI_FRAME_TIMEOUT 5           /* seconds a picture may take before the stream counts as stuck */
#define VHI_EMPTY_FRAMES  16          /* frames without a picture in a row before the stream counts as empty */
#define VHI_EVERYTHING    0x7fff      /* a width and a height no camera has: asks for its largest picture */

/* An application asks for a picture size with both of them, or for none */
#define VHI_SIZED(width, height) ((width) && (height))

/* A plane of a YUV picture is allocated 16 bytes longer than its data, and
   the data begins here (the VHI autodoc's LONGWORDALIGN) */
#define VHI_PLANE_EXTRA   16
#define VHI_PLANE_DATA(plane) ((UBYTE *) (((ULONG) (plane) + 8) & ~3UL))

#define VHI_MIN(a, b) (((a) < (b)) ? (a) : (b))

/* The formats this driver takes pictures in, the one it prefers first */
static const ULONG VhiFormats[] = { UVFCC_MJPG, UVFCC_YUY2 };
#define VHI_NUM_FORMATS (sizeof(VhiFormats) / sizeof(VhiFormats[0]))

struct VhiBase
{
    struct Library      vb_Library;
    BPTR                vb_SegList;
    struct ExecBase    *vb_SysBase;
};

/* What the driver knows of the stream without asking the unit. A read that fails clears it,
   and so does a move to another input: then the next picture asks the unit
   again and does what is missing (vhiStream()). */
struct VhiStream
{
    BOOL  vs_Running;       /* the stream runs, for the size asked for and in the mode below */
    ULONG vs_AskWidth;      /* the size the application asked for; 0 x 0 = none */
    ULONG vs_AskHeight;
    ULONG vs_FourCC;        /* the mode that size led to */
    ULONG vs_Width;
    ULONG vs_Height;
    ULONG vs_MaxFrameSize;  /* no frame of the mode is longer: the size for a read's buffer */
};

/* What VHI_METHOD_OPEN hands out. It is the calling task's: so is the port
   the replies come to, and no other task may call with it. */
struct VhiHandle
{
    struct ExecBase        *vh_SysBase;
    struct MsgPort         *vh_Port;    /* the replies of the requests below come here */
    struct IOStdReq        *vh_Video;   /* the open unit; its commands go through this */
    struct IOStdReq        *vh_Second;  /* a second request: a read goes out on it, so that
                                           vh_Video stays free for CMD_STOP, and another
                                           unit is opened on it - to ask it, or to move
                                           to it */
    struct timerequest     *vh_Timer;   /* ends the wait for a picture that does not come */
    ULONG                   vh_Unit;    /* the unit open on vh_Video = the selected input */
    struct VhiStream        vh_Stream;  /* what was started there */
};

/* /// "Library" */
extern const UBYTE endOfCode;
static const APTR initTable[4];

LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void) { return -1; }

ULONG vhi_api(ULONG method asm("d2"), ULONG submethod asm("d3"), APTR attr asm("d4"),
              ULONG *errcode asm("d5"), APTR handle asm("d6"), struct VhiBase *base asm("a6"));

static struct VhiBase * LibInit(struct VhiBase *base asm("d0"), BPTR seglist asm("a0"),
                                struct ExecBase *sysbase asm("a6"))
{
    base->vb_SegList = seglist;
    base->vb_SysBase = sysbase;
    base->vb_Library.lib_Revision = NEPTUNE_REVISION;
    return base;
}

static struct VhiBase * LibOpen(struct VhiBase *base asm("a6"))
{
    base->vb_Library.lib_OpenCnt++;
    base->vb_Library.lib_Flags &= ~LIBF_DELEXP;
    return base;
}

static BPTR LibExpunge(struct VhiBase *base asm("a6"))
{
    struct ExecBase *SysBase = base->vb_SysBase;
    BPTR seglist = base->vb_SegList;

    if(base->vb_Library.lib_OpenCnt > 0)
    {
        base->vb_Library.lib_Flags |= LIBF_DELEXP;
        return BNULL;
    }
    
    Forbid();
    Remove(&base->vb_Library.lib_Node);
    Permit();

    FreeMem((UBYTE *) base - base->vb_Library.lib_NegSize,
            (ULONG) base->vb_Library.lib_NegSize + base->vb_Library.lib_PosSize);
    return seglist;
}

static BPTR LibClose(struct VhiBase *base asm("a6"))
{
    if((--base->vb_Library.lib_OpenCnt == 0) && (base->vb_Library.lib_Flags & LIBF_DELEXP))
    {
        return LibExpunge(base);
    }
    return BNULL;
}

static ULONG LibNull(void) { return 0; }

static const APTR funcTable[] =
{
    (APTR) LibOpen,
    (APTR) LibClose,
    (APTR) LibExpunge,
    (APTR) LibNull,
    (APTR) vhi_api,
    (APTR) -1
};

static const APTR initTable[4] =
{
    (APTR) sizeof(struct VhiBase),
    (APTR) funcTable,
    (APTR) 0,
    (APTR) LibInit
};

static const char libName[]     = VHI_NAME;
static const char libIdString[] = NEP_VER(VHI_NAME);

const struct Resident romTag __attribute__((used, section(".text.modhdr"))) =
{
    RTC_MATCHWORD,
    (struct Resident *) &romTag,
    (APTR) &endOfCode,
    RTF_AUTOINIT,
    NEPTUNE_VERSION,
    NT_LIBRARY,
    0,
    (char *) libName,
    (char *) libIdString,
    (APTR) initTable
};
/* \\\ */

/* /// "vhiCommand()" */
/* One command to the open unit, done before this returns */
static LONG vhiCommand(struct VhiHandle *vh, ULONG command, APTR data, ULONG len)
{
    struct ExecBase *SysBase = vh->vh_SysBase;

    vh->vh_Video->io_Command = command;
    vh->vh_Video->io_Data = data;
    vh->vh_Video->io_Length = len;
    return DoIO((struct IORequest *) vh->vh_Video);
}
/* \\\ */

/* /// "vhiError()" */
/* The device's errors in VHI's terms */
static ULONG vhiError(LONG err)
{
    switch(err)
    {
        case UVERR_GONE:
            return VHI_ERR_NO_HARDWARE;

        case UVERR_NOMODE:
        case UVERR_BADMODE:
        case UVERR_REFUSED:
        case UVERR_NOBANDWIDTH:
            return VHI_ERR_COULD_NOT_INIT;

        case IOERR_ABORTED: /* the wait for a picture was given up */
            return VHI_ERR_NO_INPUT_DATA;
    }
    return VHI_ERR_ERR_WHILE_DIG;
}
/* \\\ */

/* /// "vhiModeDistance()" */
/* How far one of the camera's modes is from the size asked for; the mode
   with the smallest number is the one to take.
   Asked for a size: the number of pixels that lie in only one of the two
   pictures when they are laid corner on corner. 0 is that very size.
   Asked for none: 0 for the camera's default mode, 1 for every other. */
static ULONG vhiModeDistance(const struct UsbVideoMode *uvm, ULONG askwidth, ULONG askheight)
{
    if(!VHI_SIZED(askwidth, askheight))
    {
        return (uvm->uvm_Flags & UVMF_DEFAULT) ? 0 : 1;
    }
    ULONG width = uvm->uvm_Width;
    ULONG height = uvm->uvm_Height;
    ULONG shared = VHI_MIN(width, askwidth) * VHI_MIN(height, askheight);

    return width * height + askwidth * askheight - 2 * shared;
}
/* \\\ */

/* /// "vhiFindMode()" */
/* Of the camera's modes of one format, the nearest to the size asked for;
   of several that are as near, the one the camera lists first. NULL if the
   camera has no mode of that format. The table is a row of records of
   varying length (NSCMD_UV_GETMODES). */
static const struct UsbVideoMode * vhiFindMode(const UBYTE *table, ULONG len, ULONG fourcc,
                                               ULONG askwidth, ULONG askheight)
{
    const struct UsbVideoMode *best = NULL;
    ULONG least = 0;

    for(ULONG pos = 0; pos < len; )
    {
        const struct UsbVideoMode *uvm = (const struct UsbVideoMode *) (table + pos);

        pos += uvm->uvm_Size;
        if(uvm->uvm_FourCC != fourcc)
        {
            continue;
        }
        ULONG distance = vhiModeDistance(uvm, askwidth, askheight);

        if(!best || (distance < least))
        {
            best = uvm;
            least = distance;
        }
    }
    return best;
}
/* \\\ */

/* /// "vhiPictureSize()" */
/* The picture an application gets for the size it asks for.
     width, height - in: the size asked for. 0 x 0 asks for none, and gets
                     the camera's default; VHI_EVERYTHING or more gets the
                     largest. Any other size gets that size if the camera
                     has it, else the nearest it has.
                     out: the size that will come
     fourcc        - out: the format the driver takes that picture in - the
                     first of VhiFormats the camera has
     uvi           - out: what the unit says about itself right now
   Returns 0, or the VHI error; then size and format are left as they were.
   The mode table is read from the device each time: nothing is kept. */
static ULONG vhiPictureSize(struct VhiHandle *vh, struct UsbVideoInfo *uvi, ULONG *width, ULONG *height, ULONG *fourcc)
{
    struct ExecBase *SysBase = vh->vh_SysBase;
    ULONG askwidth = VHI_MIN(*width, VHI_EVERYTHING);
    ULONG askheight = VHI_MIN(*height, VHI_EVERYTHING);
    LONG err = vhiCommand(vh, NSCMD_UV_GETINFO, uvi, sizeof(*uvi));

    if(err)
    {
        return vhiError(err);
    }
    UBYTE *table = AllocVec(uvi->uvi_ModeTableSize, MEMF_PUBLIC);

    if(!table)
    {
        return VHI_ERR_OUT_OF_MEMORY;
    }
    if(!(err = vhiCommand(vh, NSCMD_UV_GETMODES, table, uvi->uvi_ModeTableSize)))
    {
        ULONG len = vh->vh_Video->io_Actual;
        const struct UsbVideoMode *mode = NULL;

        /* the first of the driver's formats that the camera has a mode of */
        for(ULONG cnt = 0; !mode && (cnt < VHI_NUM_FORMATS); cnt++)
        {
            mode = vhiFindMode(table, len, VhiFormats[cnt], askwidth, askheight);
        }
        if(mode)
        {
            *width = mode->uvm_Width;
            *height = mode->uvm_Height;
            *fourcc = mode->uvm_FourCC;
        } else {
            err = UVERR_BADMODE; /* a camera with no pictures this driver can hand out */
        }
    }
    FreeVec(table);
    return err ? vhiError(err) : 0;
}
/* \\\ */

/* /// "vhiStream()" */
/* The stream, running in the mode that the picture size asked for means;
   what it is, is in vh_Stream afterwards. 0 or the VHI error.
   If the size is the one asked for last time and the stream is remembered
   as running, nothing is done and the unit is asked nothing. Else the
   unit says what it is doing and the driver does what is missing: after the
   driver was opened, when another size is asked for, and after what was
   remembered was dropped - the camera was unplugged, a picture did not come
   and the stream was given up, the input was changed. */
static ULONG vhiStream(struct VhiHandle *vh, const struct vhi_dimensions *dim)
{
    struct VhiStream *vs = &vh->vh_Stream;
    ULONG width = dim->dst_width;
    ULONG height = dim->dst_height;

    if(vs->vs_Running && (width == vs->vs_AskWidth) && (height == vs->vs_AskHeight))
    {
        return 0;
    }
    /* nothing is remembered until all of the following has worked */
    vs->vs_Running = FALSE;

    struct UsbVideoInfo uvi;
    ULONG fourcc = 0;
    ULONG vhierr = vhiPictureSize(vh, &uvi, &width, &height, &fourcc);

    if(vhierr)
    {
        return vhierr;
    }
    /* the mode in force, if it is the one wanted; else NSCMD_UV_SETMODE
       writes the one the camera settled on over it */
    struct UsbVideoModeReq uvq =
    {
        .uvq_FourCC = fourcc, .uvq_Width = width, .uvq_Height = height,
        .uvq_MaxFrameSize = uvi.uvi_MaxFrameSize
    };
    BOOL running = (uvi.uvi_Flags & UVIF_STREAMING) != 0;
    LONG err = 0;

    /* another size or format - or no mode at all, which the unit reports as
       0 x 0: newly opened, or its camera was unplugged and came back */
    if((width != uvi.uvi_Width) || (height != uvi.uvi_Height) || (fourcc != uvi.uvi_FourCC))
    {
        vhiCommand(vh, CMD_STOP, NULL, 0);
        running = FALSE;
        err = vhiCommand(vh, NSCMD_UV_SETMODE, &uvq, sizeof(uvq));
    }
    if(!err && !running)
    {
        err = vhiCommand(vh, CMD_START, NULL, 0);
    }
    if(err)
    {
        return vhiError(err);
    }
    vs->vs_AskWidth = dim->dst_width;
    vs->vs_AskHeight = dim->dst_height;
    vs->vs_FourCC = uvq.uvq_FourCC;
    vs->vs_Width = uvq.uvq_Width;
    vs->vs_Height = uvq.uvq_Height;
    vs->vs_MaxFrameSize = uvq.uvq_MaxFrameSize;
    vs->vs_Running = TRUE;
    return 0;
}
/* \\\ */

/* /// "vhiString()" */
/* Two parts as one string the caller frees with FreeVec() */
static char * vhiString(struct VhiHandle *vh, const char *head, const char *tail, ULONG *errcode)
{
    struct ExecBase *SysBase = vh->vh_SysBase;
    ULONG headlen = strlen(head);
    ULONG taillen = strlen(tail);
    char *str = AllocVec(headlen + taillen + 1, MEMF_PUBLIC);

    if(str)
    {
        memcpy(str, head, headlen);
        memcpy(str + headlen, tail, taillen + 1);
    } else {
        *errcode = VHI_ERR_OUT_OF_MEMORY;
    }
    return str;
}
/* \\\ */

/* /// "vhiProbe()" */
/* What is behind a unit. TRUE = a camera. uvi_ProductName is set in every
   case, to what the list of inputs shows for the unit:
     the camera's name
     "Camera in use" - another program has the unit open: a camera is there
                       but cannot be asked, and nothing else of uvi is set
     "No camera"     - and nothing else of uvi is set either
   The handle's own unit is asked through its own request. Any other is
   opened for a moment, on the second request. */
static BOOL vhiProbe(struct VhiHandle *vh, ULONG unit, struct UsbVideoInfo *uvi)
{
    struct ExecBase *SysBase = vh->vh_SysBase;
    struct IOStdReq *probe = vh->vh_Second;
    LONG err;

    if(unit == vh->vh_Unit)
    {
        err = vhiCommand(vh, NSCMD_UV_GETINFO, uvi, sizeof(*uvi));
    }
    else if(!(err = OpenDevice(USBVIDEO_NAME, unit, (struct IORequest *) probe, 0)))
    {
        probe->io_Command = NSCMD_UV_GETINFO;
        probe->io_Data = uvi;
        probe->io_Length = sizeof(*uvi);
        /* a camera that went between the two calls is not there */
        err = DoIO((struct IORequest *) probe);
        CloseDevice((struct IORequest *) probe);
    }
    if(err)
    {
        strcpy((char *) uvi->uvi_ProductName, (err == IOERR_UNITBUSY) ? "Camera in use" : "No camera");
    }
    return !err || (err == IOERR_UNITBUSY);
}
/* \\\ */

/* /// "vhiNumInputs()" */
/* Input n is unit n, so there are as many inputs as the highest unit with a
   camera says - at least up to the one that is open */
static ULONG vhiNumInputs(struct VhiHandle *vh)
{
    struct UsbVideoInfo uvi;

    for(ULONG unit = VHI_MAX_UNITS; --unit > vh->vh_Unit; )
    {
        if(vhiProbe(vh, unit, &uvi))
        {
            return unit + 1;
        }
    }
    return vh->vh_Unit + 1;
}
/* \\\ */

/* /// "vhiInputName()" */
/* What the list of inputs shows for a unit: what is behind it, and its
   number */
static char * vhiInputName(struct VhiHandle *vh, ULONG unit, ULONG *errcode)
{
    struct UsbVideoInfo uvi;
    char tail[] = " (unit 0)";

    vhiProbe(vh, unit, &uvi);
    tail[sizeof(tail) - 3] += unit % 10;
    return vhiString(vh, (const char *) uvi.uvi_ProductName, tail, errcode);
}
/* \\\ */

/* /// "vhiSelectInput()" */
/* Move to another unit. The new one is opened first, on the second request:
   if it cannot be had, the old one stays. */
static ULONG vhiSelectInput(struct VhiHandle *vh, ULONG unit)
{
    struct ExecBase *SysBase = vh->vh_SysBase;
    struct IOStdReq *newunit = vh->vh_Second;

    if(unit == vh->vh_Unit)
    {
        return 0;
    }
    if(OpenDevice(USBVIDEO_NAME, unit, (struct IORequest *) newunit, 0))
    {
        return VHI_ERR_NO_HARDWARE;
    }
    vhiCommand(vh, CMD_STOP, NULL, 0);
    CloseDevice((struct IORequest *) vh->vh_Video);
    /* the two requests change places: the one the old unit was open on is
       the second from now on */
    vh->vh_Second = vh->vh_Video;
    vh->vh_Video = newunit;
    vh->vh_Unit = unit;
    /* nothing runs on the new unit */
    vh->vh_Stream.vs_Running = FALSE;
    return 0;
}
/* \\\ */

/* /// "vhiRead()" */
/* The next frame of the running stream into buf. Returns the device's error,
   IOERR_ABORTED if none came in VHI_FRAME_TIMEOUT seconds. */
static LONG vhiRead(struct VhiHandle *vh, APTR buf, ULONG size, ULONG *actual)
{
    struct ExecBase *SysBase = vh->vh_SysBase;
    /* on the second request, so that the unit's own stays free for CMD_STOP */
    struct IOStdReq *read = vh->vh_Second;
    struct IORequest *timer = (struct IORequest *) vh->vh_Timer;
    ULONG sigmask = 1UL << vh->vh_Port->mp_SigBit;

    /* a second request for an open unit is one that names the same device
       and unit */
    read->io_Device = vh->vh_Video->io_Device;
    read->io_Unit = vh->vh_Video->io_Unit;
    read->io_Command = CMD_READ;
    read->io_Data = buf;
    read->io_Length = size;
    vh->vh_Timer->tr_node.io_Command = TR_ADDREQUEST;
    vh->vh_Timer->tr_time.tv_secs = VHI_FRAME_TIMEOUT;
    vh->vh_Timer->tr_time.tv_micro = 0;
    SendIO((struct IORequest *) read);
    SendIO(timer);
    while(!CheckIO((struct IORequest *) read) && !CheckIO(timer))
    {
        Wait(sigmask);
    }
    if(!CheckIO((struct IORequest *) read))
    {
        /* No frame in all that time: whatever the camera is doing, it is not
           streaming. Stopping the stream is what brings the read back for
           certain - one that is being filled waits for the next transfer
           otherwise - and the next picture asked for starts it afresh. */
        AbortIO((struct IORequest *) read);
        vhiCommand(vh, CMD_STOP, NULL, 0);
    }
    WaitIO((struct IORequest *) read);
    AbortIO(timer);
    WaitIO(timer);
    SetSignal(0, sigmask);

    /* Whatever went wrong - the camera is gone, the stream was just given
       up - what is remembered of the stream is not to be relied on: the
       next picture asks the unit. */
    if(read->io_Error)
    {
        vh->vh_Stream.vs_Running = FALSE;
    }
    *actual = read->io_Actual;
    return read->io_Error;
}
/* \\\ */

/* /// "vhiDigitize()" */
/* The picture's memory is the application's, by its allocator if it names one */
static APTR vhiAlloc(struct VhiHandle *vh, struct vhi_digitize *dig, ULONG size, ULONG flags)
{
    struct ExecBase *SysBase = vh->vh_SysBase;

    return dig->custom_memhandling ? dig->CstAllocVec(size, flags) : AllocVec(size, flags);
}

static void vhiFree(struct VhiHandle *vh, struct vhi_digitize *dig, APTR mem)
{
    struct ExecBase *SysBase = vh->vh_SysBase;

    if(!mem)
    {
        return;
    }
    if(dig->custom_memhandling)
    {
        dig->CstFreeVec(mem);
    } else {
        FreeVec(mem);
    }
}

/* The bytes of a raw picture - Y0 U Y1 V for every two pixels - sorted into
   three planes: no arithmetic, the values are the camera's */
static void vhiSortPlanes(const UBYTE *src, ULONG pairs, UBYTE *y, UBYTE *u, UBYTE *v)
{
    for(; pairs; pairs--)
    {
        *y++ = src[0];
        *u++ = src[1];
        *y++ = src[2];
        *v++ = src[3];
        src += 4;
    }
}

/* One raw frame of the running stream into the three planes of img. The
   frame is read into memory of the driver's own. 0 or the VHI error. */
static ULONG vhiReadPlanes(struct VhiHandle *vh, struct vhi_image *img, ULONG pairs)
{
    struct ExecBase *SysBase = vh->vh_SysBase;
    ULONG size = vh->vh_Stream.vs_MaxFrameSize;
    UBYTE *frame = AllocVec(size, MEMF_PUBLIC);

    if(!frame)
    {
        return VHI_ERR_OUT_OF_MEMORY;
    }
    ULONG len = 0;
    LONG err = vhiRead(vh, frame, size, &len);

    if(!err)
    {
        /* a frame is as long as its picture; never beyond what came */
        vhiSortPlanes(frame, VHI_MIN(pairs, len / 4),
                      VHI_PLANE_DATA(img->y), VHI_PLANE_DATA(img->u), VHI_PLANE_DATA(img->v));
    }
    FreeVec(frame);
    return err ? vhiError(err) : 0;
}

/* A raw picture into img, for the camera that makes no JPEG ones: three
   planes of the application's memory, which VHI calls YUV 4:2:2. 0 or the
   VHI error; what was allocated is in img either way. */
static ULONG vhiDigitizePlanar(struct VhiHandle *vh, struct vhi_digitize *dig, struct vhi_image *img)
{
    const struct VhiStream *vs = &vh->vh_Stream;
    ULONG pairs = vs->vs_Width * vs->vs_Height / 2;

    img->y = vhiAlloc(vh, dig, pairs * 2 + VHI_PLANE_EXTRA, MEMF_PUBLIC);
    img->u = vhiAlloc(vh, dig, pairs + VHI_PLANE_EXTRA, MEMF_PUBLIC);
    img->v = vhiAlloc(vh, dig, pairs + VHI_PLANE_EXTRA, MEMF_PUBLIC);
    if(!img->y || !img->u || !img->v)
    {
        return VHI_ERR_OUT_OF_MEMORY;
    }
    img->width = vs->vs_Width;
    img->height = vs->vs_Height;
    img->type = VHI_YUV_422;
    return vhiReadPlanes(vh, img, pairs);
}

/* The next frame of the running stream that has a picture in it. A grabber
   whose source gave none sends frames without one: those are passed over,
   and after VHI_EMPTY_FRAMES of them in a row it is as if no frame had come
   (IOERR_ABORTED). Else the error of the read. */
static LONG vhiReadJpeg(struct VhiHandle *vh, UBYTE *frame, ULONG size, ULONG *len)
{
    for(ULONG empty = 0; empty < VHI_EMPTY_FRAMES; empty++)
    {
        LONG err = vhiRead(vh, frame, size, len);

        if(err || mjpegFrameHeader(frame, *len))
        {
            return err;
        }
    }
    return IOERR_ABORTED;
}

/* A frame that lies MJPEG_STD_DHT_SIZE bytes into buf becomes a JPEG file
   that begins where buf begins, with the standard Huffman tables in it if
   the camera left its own out. One move either way: all of the frame, or
   the short part of it before the tables. Returns the file's length. */
static ULONG vhiJpegFile(UBYTE *buf, ULONG len)
{
    const UBYTE *frame = buf + MJPEG_STD_DHT_SIZE;
    ULONG dhtpos = mjpegMissingDHT(frame, len);

    if(!dhtpos)
    {
        memmove(buf, frame, len);
        return len;
    }
    memmove(buf, frame, dhtpos);
    memcpy(buf + dhtpos, MJPEGStdDHT, MJPEG_STD_DHT_SIZE);
    return len + MJPEG_STD_DHT_SIZE;
}

/* A JPEG picture into img: the frame as the camera sent it, made a file in
   the application's memory. 0 or the VHI error; what was allocated is in img
   either way. */
static ULONG vhiDigitizeJpeg(struct VhiHandle *vh, struct vhi_digitize *dig, struct vhi_image *img)
{
    ULONG size = vh->vh_Stream.vs_MaxFrameSize;
    /* room for the tables in front of the frame (vhiJpegFile()) */
    UBYTE *buf = vhiAlloc(vh, dig, MJPEG_STD_DHT_SIZE + size, MEMF_PUBLIC);

    img->chunky = buf;
    if(!buf)
    {
        return VHI_ERR_OUT_OF_MEMORY;
    }
    ULONG len = 0;
    LONG err = vhiReadJpeg(vh, buf + MJPEG_STD_DHT_SIZE, size, &len);

    if(err)
    {
        return vhiError(err);
    }
    /* a JPEG picture is handed over with its length in bytes for a width */
    img->width = vhiJpegFile(buf, len);
    img->height = 0;
    img->type = VHI_JPEG;
    return 0;
}

/* VHI_DIGITIZE_PICTURE: the stream in the mode the size asked for means,
   then one picture of it in memory the application will free - a JPEG file,
   or three planes from a camera that makes no JPEG pictures. NULL and the
   VHI error if there is none. */
static struct vhi_image * vhiDigitize(struct VhiHandle *vh, struct vhi_digitize *dig, ULONG *errcode)
{
    if((*errcode = vhiStream(vh, &dig->dim)))
    {
        return NULL;
    }
    struct vhi_image *img = vhiAlloc(vh, dig, sizeof(struct vhi_image), MEMF_PUBLIC|MEMF_CLEAR);

    if(!img)
    {
        *errcode = VHI_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    img->scaled = VHI_SIZED(dig->dim.dst_width, dig->dim.dst_height);
    *errcode = (vh->vh_Stream.vs_FourCC == UVFCC_MJPG) ? vhiDigitizeJpeg(vh, dig, img)
                                                       : vhiDigitizePlanar(vh, dig, img);
    if(!*errcode)
    {
        return img;
    }
    /* no picture: what was allocated for it goes back (vhiFree() passes
       over what was not) */
    vhiFree(vh, dig, img->chunky);
    vhiFree(vh, dig, img->y);
    vhiFree(vh, dig, img->u);
    vhiFree(vh, dig, img->v);
    vhiFree(vh, dig, img);
    return NULL;
}
/* \\\ */

/* /// "vhiGet()" */
static ULONG vhiGet(struct VhiHandle *vh, ULONG submethod, APTR attr, ULONG *errcode)
{
    struct UsbVideoInfo uvi;

    switch(submethod)
    {
        case VHI_CARD_NAME:
            if(vhiCommand(vh, NSCMD_UV_GETINFO, &uvi, sizeof(uvi)))
            {
                return (ULONG) vhiString(vh, "USB video camera", "", errcode);
            }
            return (ULONG) vhiString(vh, (const char *) uvi.uvi_ProductName, "", errcode);

        case VHI_CARD_MANUFACTURER:
            if(vhiCommand(vh, NSCMD_UV_GETINFO, &uvi, sizeof(uvi)))
            {
                return (ULONG) vhiString(vh, "n/a", "", errcode);
            }
            return (ULONG) vhiString(vh, (const char *) uvi.uvi_Manufacturer, "", errcode);

        case VHI_CARD_DRIVERAUTHOR:
            return (ULONG) vhiString(vh, NEPTUNE_DIST_NAME, "", errcode);

        /* the "card" is the camera: its version is the revision of the USB
           video class it follows, which is binary coded decimal */
        case VHI_CARD_VERSION:
        case VHI_CARD_REVISION:
            if(vhiCommand(vh, NSCMD_UV_GETINFO, &uvi, sizeof(uvi)))
            {
                return 0;
            }
            return (submethod == VHI_CARD_VERSION) ? (ULONG) (uvi.uvi_Version >> 8)
                   : (ULONG) (((uvi.uvi_Version >> 4) & 15) * 10 + (uvi.uvi_Version & 15));

        case VHI_CARD_DRIVERVERSION:
            return NEPTUNE_VERSION;

        case VHI_CARD_DRIVERREVISION:
            return NEPTUNE_REVISION;

        case VHI_SUPPORTED_MODES:
            return VHI_MODE_COLOR;

        case VHI_COLOR_MODE:
            return 1;

        case VHI_NUMBER_OF_INPUTS:
            return vhiNumInputs(vh);

        case VHI_NAME_OF_INPUT:
            return (ULONG) vhiInputName(vh, (ULONG) attr, errcode);

        case VHI_SUPPORTED_OPTIONS:
            return VHI_OPT_INPUT;

        /* a camera has no video standard; the applications want one named */
        case VHI_SUPPORTED_VIDEOFORMATS:
            return VHI_FORMAT_PAL;

        case VHI_MAXIMUM_SIZE:
        {
            struct vhi_size *size = (struct vhi_size *) attr;
            ULONG width = VHI_EVERYTHING;
            ULONG height = VHI_EVERYTHING;
            ULONG fourcc = 0;

            *errcode = vhiPictureSize(vh, &uvi, &width, &height, &fourcc);
            size->max_width = *errcode ? 0 : width;
            size->max_height = *errcode ? 0 : height;
            size->fixed = TRUE;     /* a picture comes whole: a JPEG file cannot be cut, and
                                       a raw one is handed over the same way */
            size->scalable = TRUE;  /* by the camera: the picture sizes it has */
            return 0;
        }

        case VHI_DIGITAL_CAMERA:
            return VHI_DC_TYPE_NOCAM;
    }
    *errcode = VHI_ERR_UNKNOWN_METHOD;
    return 0;
}
/* \\\ */

/* /// "vhiSet()" */
/* 0 = done */
static ULONG vhiSet(struct VhiHandle *vh, ULONG submethod, APTR attr, ULONG *errcode)
{
    switch(submethod)
    {
        case VHI_OPTIONS:
        {
            struct vhi_setoptions *opt = (struct vhi_setoptions *) attr;

            *errcode = (opt->option == VHI_OPT_INPUT) ? vhiSelectInput(vh, opt->value)
                                                      : VHI_ERR_UNKNOWN_OPTION;
            return *errcode;
        }

        case VHI_COLOR_MODE:
            /* colour is what the pictures are */
            *errcode = attr ? 0 : VHI_ERR_UNKNOWN_OPTION;
            return *errcode;

        case VHI_VIDEOFORMAT:
            return 0;
    }
    /* among them the "trust me" mode, in which the size is set once and not
       looked at again for every picture: the driver does not work a size
       out twice as it is (vhiStream()) */
    *errcode = VHI_ERR_UNKNOWN_METHOD;
    return *errcode;
}
/* \\\ */

/* /// "vhiPerform()" */
static ULONG vhiPerform(struct VhiHandle *vh, ULONG submethod, APTR attr, ULONG *errcode)
{
    switch(submethod)
    {
        case VHI_CHECK_DIGITIZE_SIZE:
        {
            /* The whole picture - there is no cropping - at the size it will
               come in if one is asked for. Without the camera to ask, the
               request stays as it is. */
            struct vhi_dimensions *dim = (struct vhi_dimensions *) attr;
            struct UsbVideoInfo uvi;
            ULONG maxwidth = VHI_EVERYTHING;
            ULONG maxheight = VHI_EVERYTHING;
            ULONG width = dim->dst_width;
            ULONG height = dim->dst_height;
            ULONG fourcc = 0;
            BOOL sized = VHI_SIZED(width, height);

            if((*errcode = vhiPictureSize(vh, &uvi, &maxwidth, &maxheight, &fourcc)) ||
               (sized && (*errcode = vhiPictureSize(vh, &uvi, &width, &height, &fourcc))))
            {
                return 0;
            }
            dim->x1 = 0;
            dim->y1 = 0;
            dim->x2 = maxwidth - 1;
            dim->y2 = maxheight - 1;
            dim->dst_width = sized ? width : 0;
            dim->dst_height = sized ? height : 0;
            return 0;
        }

        case VHI_DIGITIZE_PICTURE:
            return (ULONG) vhiDigitize(vh, (struct vhi_digitize *) attr, errcode);
    }
    *errcode = VHI_ERR_UNKNOWN_METHOD;
    return 0;
}
/* \\\ */

/* /// "vhiDispose()" */
/* The handle and what was made for it go back. Both delete calls take a
   NULL, so this serves an open that failed half way as well. */
static void vhiDispose(struct VhiHandle *vh)
{
    struct ExecBase *SysBase = vh->vh_SysBase;

    DeleteIORequest((struct IORequest *) vh->vh_Timer);
    DeleteIORequest((struct IORequest *) vh->vh_Second);
    DeleteIORequest((struct IORequest *) vh->vh_Video);
    DeleteMsgPort(vh->vh_Port);
    FreeVec(vh);
}
/* \\\ */

/* /// "vhiOpen()" */
/* VHI_METHOD_OPEN: a handle for the calling task - its reply port, the
   requests, the timer, and the first camera that can be had. NULL and the
   VHI error if one of them cannot be had. */
static struct VhiHandle * vhiOpen(struct ExecBase *SysBase, ULONG *errcode)
{
    struct VhiHandle *vh = AllocVec(sizeof(struct VhiHandle), MEMF_PUBLIC|MEMF_CLEAR);

    *errcode = VHI_ERR_OUT_OF_MEMORY;
    if(!vh)
        return NULL;

    vh->vh_SysBase = SysBase;
    if((vh->vh_Port = CreateMsgPort()) &&
       (vh->vh_Video = (struct IOStdReq *) CreateIORequest(vh->vh_Port, sizeof(struct IOStdReq))) &&
       (vh->vh_Second = (struct IOStdReq *) CreateIORequest(vh->vh_Port, sizeof(struct IOStdReq))) &&
       (vh->vh_Timer = (struct timerequest *) CreateIORequest(vh->vh_Port, sizeof(struct timerequest))))
    {
        *errcode = VHI_ERR_COULD_NOT_INIT;
        if(!OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *) vh->vh_Timer, 0))
        {
            for(ULONG unit = 0; unit < VHI_MAX_UNITS; unit++)
            {
                if(!OpenDevice(USBVIDEO_NAME, unit, (struct IORequest *) vh->vh_Video, 0))
                {
                    vh->vh_Unit = unit;
                    *errcode = 0;
                    return vh;
                }
            }
            CloseDevice((struct IORequest *) vh->vh_Timer);
            *errcode = VHI_ERR_NO_HARDWARE;
        }
    }
    vhiDispose(vh);
    return NULL;
}
/* \\\ */

/* /// "vhiClose()" */
/* VHI_METHOD_CLOSE: a stream that still runs is stopped, and all goes back */
static void vhiClose(struct VhiHandle *vh)
{
    struct ExecBase *SysBase = vh->vh_SysBase;

    vhiCommand(vh, CMD_STOP, NULL, 0);
    CloseDevice((struct IORequest *) vh->vh_Video);
    CloseDevice((struct IORequest *) vh->vh_Timer);
    vhiDispose(vh);
}
/* \\\ */

/* /// "vhi_api()" */
ULONG vhi_api(ULONG method asm("d2"), ULONG submethod asm("d3"), APTR attr asm("d4"),
              ULONG *errcode asm("d5"), APTR handle asm("d6"), struct VhiBase *base asm("a6"))
{
    struct ExecBase *SysBase = base->vb_SysBase;
    struct VhiHandle *vh = (struct VhiHandle *) handle;
    ULONG noerrcode;

    if(!errcode)
    {
        errcode = &noerrcode;
    }
    *errcode = 0;

    if(method == VHI_METHOD_OPEN)
    {
        return (ULONG) vhiOpen(SysBase, errcode);
    }
    /* A handle is its opener's. The replies of its requests signal that
       task: another one would wait for them for ever, and at
       VHI_METHOD_CLOSE would give back a signal that is not its own. */
    if(!vh || (vh->vh_Port->mp_SigTask != FindTask(NULL)))
    {
        *errcode = VHI_ERR_INTERNAL_ERROR;
        /* The error is in *errcode for every method. What is returned is the
           method's result, and 0 is "none" for all but VHI_METHOD_SET: there
           it is "done" (vhiSet()), which is all cAMIra looks at after it
           switched the input. */
        return (method == VHI_METHOD_SET) ? VHI_ERR_INTERNAL_ERROR : 0;
    }
    switch(method)
    {
        case VHI_METHOD_CLOSE:
            vhiClose(vh);
            return 0;

        case VHI_METHOD_SET:
            return vhiSet(vh, submethod, attr, errcode);

        case VHI_METHOD_GET:
            return vhiGet(vh, submethod, attr, errcode);

        case VHI_METHOD_PERFORM:
            return vhiPerform(vh, submethod, attr, errcode);
    }
    *errcode = VHI_ERR_UNKNOWN_METHOD;
    return 0;
}
/* \\\ */
