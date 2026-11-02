/*
 *----------------------------------------------------------------------------
 *              usbvideo class: usbvideo.device, the application side
 *----------------------------------------------------------------------------
 * The interface is <devices/usbvideodevice.h>. One unit is one capture stream.
 * Commands that only read the unit are answered here, in the caller's
 * context; everything that talks to the camera goes to the subtask of the
 * unit's binding.
 */

#include "debug.h"

#include "usbvideo.class.h"

DEVBASETYPEPTR devInit(DEVBASETYPEPTR base asm("d0"), BPTR seglist asm("a0"), struct ExecBase * SysBase asm("a6"))
{
    KPRINTF(10, ("devInit base: 0x%08lx seglist: 0x%08lx SysBase: 0x%08lx\n",
                 base, seglist, SysBase));

    base->np_Library.lib_Node.ln_Type = NT_DEVICE;
    base->np_Library.lib_Node.ln_Name = DEVNAME;
    base->np_Library.lib_Flags        = LIBF_SUMUSED|LIBF_CHANGED;
    base->np_Library.lib_Version      = NEPTUNE_VERSION;
    base->np_Library.lib_Revision     = NEPTUNE_REVISION;
    base->np_Library.lib_IdString     = VERSION_STRING;

    /* Store segment */
    base->np_SegList = seglist;

    if((base->np_UtilityBase = OpenLibrary("utility.library", 0)))
    {
        KPRINTF(10, ("devInit: Ok\n"));
        return base;
    }
    return NULL;
}

#undef UtilityBase
#define	UtilityBase	base->np_UtilityBase

/* Exec calls the open, close and expunge vectors under Forbid() */
DEVBASETYPEPTR (devOpen)(struct IOStdReq * ioreq asm("a1"), ULONG unit asm("d0"), ULONG flags asm("d1"), struct NepVideoDevBase * base asm("a6"))
{
    struct NepVideoStream *ns;

    KPRINTF(10, ("devOpen ioreq: 0x%08lx unit: %ld flags: 0x%08lx base: 0x%08lx\n",
                 ioreq, unit, flags, base));

    ++base->np_Library.lib_OpenCnt;
    base->np_Library.lib_Flags &= ~LIBF_DELEXP;

    /* Some programs leave mn_Length zero; a request too short to be an IOStdReq is an error */
    if(ioreq->io_Message.mn_Length && (ioreq->io_Message.mn_Length < sizeof(struct IOStdReq)))
    {
        ioreq->io_Error = IOERR_BADLENGTH;
    } else {
        /* A unit can be opened while its camera is there, and by one opener only */
        ioreq->io_Error = IOERR_OPENFAIL;
        ns = (struct NepVideoStream *) base->np_ClsBase->nh_Units.lh_Head;
        while(ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ)
        {
            if((ns->ns_UnitNo == unit) && ns->ns_Binding)
            {
                /* in use - or the stream of the last opener is still being stopped */
                if(ns->ns_Unit.unit_OpenCnt || ns->ns_Streaming)
                {
                    ioreq->io_Error = IOERR_UNITBUSY;
                    break;
                }
                ns->ns_Unit.unit_OpenCnt++;
                ns->ns_OpenFlags = flags;
                ns->ns_Mode.um_Format = NULL; /* every opener negotiates its own mode */
                /* the unit lives in the class base: keep the class from being expunged */
                base->np_ClsBase->nh_Library.lib_OpenCnt++;
                ioreq->io_Unit = (struct Unit *) ns;
                ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
                ioreq->io_Error = 0;
                return base;
            }
            ns = (struct NepVideoStream *) ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ;
        }
        KPRINTF(20, ("devOpen: could not open unit!\n"));
    }

    ioreq->io_Unit   = (APTR) -1;
    ioreq->io_Device = (APTR) -1;
    base->np_Library.lib_OpenCnt--;

    return NULL;
}

BPTR (devClose)(struct IOStdReq * ioreq asm("a1"), struct NepVideoDevBase * base asm("a6"))
{
    BPTR ret = BNULL;
    struct NepVideoStream *ns = (struct NepVideoStream *) ioreq->io_Unit;

    KPRINTF(10, ("devClose ioreq: 0x%08lx base: 0x%08lx\n", ioreq, base));

    /* Reads and waits for events the opener left behind go back to it. A
       stream it left running is stopped by the binding's subtask, which the
       signal wakes: that takes USB traffic, and this is the caller's context
       under Forbid(). */
    uvcAbortReads(ns, IOERR_ABORTED);
    ns->ns_Unit.unit_OpenCnt--;
    if(ns->ns_Binding)
    {
        uvcEventFlush(ns->ns_Binding, &ns->ns_Unit, IOERR_ABORTED);
        Signal(ns->ns_Binding->ncv_Task, 1UL<<ns->ns_Binding->ncv_CmdPort->mp_SigBit);
    }
    base->np_ClsBase->nh_Library.lib_OpenCnt--;
    ioreq->io_Unit   = (APTR) -1;
    ioreq->io_Device = (APTR) -1;

    if(--base->np_Library.lib_OpenCnt == 0)
    {
        if(base->np_Library.lib_Flags & LIBF_DELEXP)
        {
            KPRINTF(5, ("devClose: calling expunge...\n"));
            ret = devExpunge(base, base);
        }
    }
    return ret;
}

BPTR (devExpunge)(DEVBASETYPEPTR extralh asm("d0"), struct NepVideoDevBase * base asm("a6"))
{
    BPTR ret;

    KPRINTF(10, ("devExpunge base: 0x%08lx\n", base));

    if(base->np_Library.lib_OpenCnt == 0)
    {
        CloseLibrary(base->np_UtilityBase);

        ret = base->np_SegList;

        Remove(&base->np_Library.lib_Node);
        FreeMem((char *) base - base->np_Library.lib_NegSize,
                (ULONG) (base->np_Library.lib_NegSize + base->np_Library.lib_PosSize));

        KPRINTF(5, ("devExpunge: Unloading done! " DEVNAME " expunged!\n\n"));
        return ret;
    }
    KPRINTF(5, ("devExpunge: Could not expunge, LIBF_DELEXP set!\n"));
    base->np_Library.lib_Flags |= LIBF_DELEXP;
    return BNULL;
}

DEVBASETYPEPTR (devReserved)(struct NepVideoDevBase * base asm("a6"))
{
    return NULL;
}

/* /// "Mode table" */
/* The intervals of one frame as the mode table lists them: the list itself,
   or the three numbers of a range */
static ULONG nModeIntervals(const struct UvcFrame *ufr)
{
    return ufr->ufr_NumIntervals ? ufr->ufr_NumIntervals : 3;
}

static ULONG nModeSize(const struct UvcFrame *ufr)
{
    return offsetof(struct UsbVideoMode, uvm_Intervals) + nModeIntervals(ufr) * sizeof(ULONG);
}

/* Walk the modes of a unit's stream: one per frame of every streamable
   format. Counts them and adds up the size of their table; given a buffer,
   writes the first records that fit whole and returns the bytes written.
   The default mode is the one a request without a size gets: the user's, if
   the settings stored for the camera name one, else the camera's own. */
static ULONG nModeTable(const struct NepVideoStream *ns, UBYTE *buf, ULONG buflen,
                        ULONG *nummodes, ULONG *tablesize)
{
    const struct UvcStreamDesc *us = ns->ns_Desc;
    const struct UvcFormat *deffmt = uvcFindFormat(us, 0);
    const struct UvcFrame *deffrm = deffmt ? uvcDefaultFrame(deffmt, ns->ns_Default.ud_Width,
                                                             ns->ns_Default.ud_Height) : NULL;
    ULONG written = 0;

    *nummodes = 0;
    *tablesize = 0;
    for(ULONG fmt = 0; fmt < us->us_NumFormats; fmt++)
    {
        const struct UvcFormat *ufo = &us->us_Formats[fmt];
        if(!uvcIsStreamable(ufo))
        {
            continue;
        }
        for(ULONG frm = 0; frm < ufo->ufo_NumFrames; frm++)
        {
            const struct UvcFrame *ufr = &ufo->ufo_Frames[frm];
            ULONG size = nModeSize(ufr);

            (*nummodes)++;
            *tablesize += size;
            if(buf && (written + size > buflen))
            {
                buf = NULL; /* full: the table ends with the last record that fit */
            }
            if(buf)
            {
                struct UsbVideoMode *uvm = (struct UsbVideoMode *) (buf + written);
                uvm->uvm_Size = size;
                /* the format too: two formats can share their frames */
                uvm->uvm_Flags = (((ufo == deffmt) && (ufr == deffrm)) ? UVMF_DEFAULT : 0) |
                                 (ufr->ufr_NumIntervals ? 0 : UVMF_CONTINUOUS);
                uvm->uvm_FourCC = uvcFourCC(ufo);
                uvm->uvm_Width = ufr->ufr_Width;
                uvm->uvm_Height = ufr->ufr_Height;
                uvm->uvm_DefInterval = uvcDefaultInterval(ufr, ns->ns_Default.ud_Width, ns->ns_Default.ud_Height,
                                                          ns->ns_Default.ud_Interval);
                uvm->uvm_NumIntervals = nModeIntervals(ufr);
                for(ULONG cnt = 0; cnt < uvm->uvm_NumIntervals; cnt++)
                {
                    uvm->uvm_Intervals[cnt] = uvcFrameInterval(ufr, cnt);
                }
                written += size;
            }
        }
    }
    return written;
}
/* \\\ */

/* /// "Topology" */
/* Walk the terminals and units of a function, in the order of its
   descriptors. Counts them and adds up the size of their table; given a
   buffer, writes the first records that fit whole and returns the bytes
   written. */
static ULONG nTopology(const struct UvcFunction *uf, UBYTE *buf, ULONG buflen,
                       ULONG *numentities, ULONG *tablesize)
{
    ULONG written = 0;

    *numentities = uf->uf_NumEntities;
    *tablesize = 0;
    for(ULONG cnt = 0; cnt < uf->uf_NumEntities; cnt++)
    {
        const struct UvcEntity *ue = &uf->uf_Entities[cnt];
        /* a record starts on an even address */
        ULONG size = (offsetof(struct UsbVideoEntity, uve_Data) + ue->ue_NumSources + ue->ue_ControlSize + 1) & ~1UL;

        *tablesize += size;
        if(buf && (written + size > buflen))
        {
            buf = NULL; /* full: the table ends with the last record that fit */
        }
        if(buf)
        {
            struct UsbVideoEntity *uve = (struct UsbVideoEntity *) (buf + written);

            memset(uve, 0, size);
            uve->uve_Size = size;
            uve->uve_ID = ue->ue_ID;
            uve->uve_Type = ue->ue_Type;
            uve->uve_TermType = ue->ue_TermType;
            uve->uve_NumSources = ue->ue_NumSources;
            uve->uve_ControlSize = ue->ue_ControlSize;
            if(ue->ue_Type == UDST_VIDEO_CTRL_EXTENSION_UNIT)
            {
                memcpy(uve->uve_GUID, ((const struct UsbVideoExtensionUnitDesc *) ue->ue_Desc)->guidExtensionCode,
                       sizeof(uve->uve_GUID));
            }
            memcpy(uve->uve_Data, ue->ue_Sources, ue->ue_NumSources);
            memcpy(uve->uve_Data + ue->ue_NumSources, ue->ue_Controls, ue->ue_ControlSize);
            written += size;
        }
    }
    return written;
}
/* \\\ */

/* /// "cmdGetInfo()" */
/* The structure has grown since its first form and may grow again: a caller
   gets as much of it as it has room for, from the first form up. */
static WORD cmdGetInfo(struct IOStdReq *ioreq, struct NepVideoStream *ns)
{
    struct UsbVideoInfo info;
    struct UsbVideoInfo *uvi = &info;
    ULONG len = (ioreq->io_Length < sizeof(info)) ? ioreq->io_Length : sizeof(info);
    WORD ret = UVERR_GONE;

    if(!ioreq->io_Data || (len < offsetof(struct UsbVideoInfo, uvi_NumEntities)))
    {
        return IOERR_BADLENGTH;
    }
    memset(uvi, 0, sizeof(struct UsbVideoInfo));

    /* the binding and its model stay while this runs; so does the mode */
    Forbid();
    if(ns->ns_Binding)
    {
        uvi->uvi_VendorID = ns->ns_VendorID;
        uvi->uvi_ProductID = ns->ns_ProductID;
        uvi->uvi_Version = ns->ns_Binding->ncv_Function->uf_Version;
        uvi->uvi_Flags = (ns->ns_IsBulk ? UVIF_BULK : 0) | (ns->ns_Streaming ? UVIF_STREAMING : 0) |
                         (ns->ns_Binding->ncv_Status.ust_Pipe ? UVIF_EVENTS : 0);
        nModeTable(ns, NULL, 0, &uvi->uvi_NumModes, &uvi->uvi_ModeTableSize);
        nTopology(ns->ns_Binding->ncv_Function, NULL, 0, &uvi->uvi_NumEntities, &uvi->uvi_TopologySize);
        if(ns->ns_Mode.um_Format)
        {
            uvi->uvi_FourCC = uvcFourCC(ns->ns_Mode.um_Format);
            uvi->uvi_Width = ns->ns_Mode.um_Frame->ufr_Width;
            uvi->uvi_Height = ns->ns_Mode.um_Frame->ufr_Height;
            uvi->uvi_Interval = ns->ns_Mode.um_Interval;
            uvi->uvi_MaxFrameSize = ns->ns_Mode.um_MaxFrameSize;
            uvi->uvi_ClockFrequency = ns->ns_Mode.um_ClockFrequency;
        }
        memcpy(uvi->uvi_ProductName, ns->ns_ProductName, sizeof(uvi->uvi_ProductName));
        memcpy(uvi->uvi_SerialNumber, ns->ns_SerialNumber, sizeof(uvi->uvi_SerialNumber));
        memcpy(uvi->uvi_Manufacturer, ns->ns_Manufacturer, sizeof(uvi->uvi_Manufacturer));
        ioreq->io_Actual = len;
        ret = RC_OK;
    }
    Permit();
    memcpy(ioreq->io_Data, uvi, len);
    return ret;
}
/* \\\ */

/* /// "cmdGetTopology()" */
/* NSCMD_UV_GETTOPOLOGY: the camera's terminals and units into the caller's
   buffer, as much as fits; answered in the caller's context */
static WORD cmdGetTopology(struct IOStdReq *ioreq, struct NepVideoStream *ns)
{
    WORD ret = UVERR_GONE;
    ULONG numentities;
    ULONG tablesize;

    if(!ioreq->io_Data)
    {
        return IOERR_BADLENGTH;
    }
    Forbid();
    if(ns->ns_Binding)
    {
        ioreq->io_Actual = nTopology(ns->ns_Binding->ncv_Function, ioreq->io_Data, ioreq->io_Length,
                                     &numentities, &tablesize);
        ret = RC_OK;
    }
    Permit();
    return ret;
}
/* \\\ */

/* /// "cmdGetModes()" */
/* NSCMD_UV_GETMODES: the stream's formats, sizes and intervals into the
   caller's buffer, as much as fits; answered in the caller's context */
static WORD cmdGetModes(struct IOStdReq *ioreq, struct NepVideoStream *ns)
{
    WORD ret = UVERR_GONE;
    ULONG nummodes;
    ULONG tablesize;

    if(!ioreq->io_Data)
    {
        return IOERR_BADLENGTH;
    }
    Forbid();
    if(ns->ns_Binding)
    {
        ioreq->io_Actual = nModeTable(ns, ioreq->io_Data, ioreq->io_Length, &nummodes, &tablesize);
        ret = RC_OK;
    }
    Permit();
    return ret;
}
/* \\\ */

/* /// "cmdGetStats()" */
/* NSCMD_UV_GETSTATS: the stream's counts since its last start */
static WORD cmdGetStats(struct IOStdReq *ioreq, struct NepVideoStream *ns)
{
    if(!ioreq->io_Data || (ioreq->io_Length < sizeof(struct UsbVideoStats)))
    {
        return IOERR_BADLENGTH;
    }
    /* the transport counts in the controller driver's task: kept out, the
       copy is of one moment. The counts outlive the stream and the camera. */
    Forbid();
    memcpy(ioreq->io_Data, &ns->ns_Stats, sizeof(struct UsbVideoStats));
    Permit();
    ioreq->io_Actual = sizeof(struct UsbVideoStats);
    return RC_OK;
}
/* \\\ */

/* /// "cmdNSDeviceQuery()" */
/* NSCMD_DEVICEQUERY: what kind of device this is and the commands it knows */
static const UWORD NSDSupported[] =
{
    CMD_READ, CMD_START, CMD_STOP, CMD_FLUSH,
    NSCMD_UV_GETINFO, NSCMD_UV_GETMODES, NSCMD_UV_SETMODE, NSCMD_UV_GETSTATS,
    NSCMD_UV_REQUEST, NSCMD_UV_GETTOPOLOGY, NSCMD_UV_WAITEVENT,
    NSCMD_UV_GETCONTROL, NSCMD_UV_NEXTCONTROL, NSCMD_UV_SETCONTROL, NSCMD_UV_SETTINGS,
    NSCMD_DEVICEQUERY, 0
};

static WORD cmdNSDeviceQuery(struct IOStdReq *ioreq)
{
    struct my_NSDeviceQueryResult *query = (struct my_NSDeviceQueryResult *) ioreq->io_Data;

    if((!query) ||
       (ioreq->io_Length < sizeof(struct my_NSDeviceQueryResult)) ||
       (query->DevQueryFormat != 0) ||
       (query->SizeAvailable != 0))
    {
        return IOERR_NOCMD;
    }
    ioreq->io_Actual         = query->SizeAvailable
                             = sizeof(struct my_NSDeviceQueryResult);
    query->DeviceType        = NSDEVTYPE_UNKNOWN;
    query->DeviceSubType     = 0;
    query->SupportedCommands = NSDSupported;
    return RC_OK;
}
/* \\\ */

void (devBeginIO)(struct IOStdReq * ioreq asm("a1"), struct NepVideoDevBase * base asm("a6"))
{
    struct NepVideoStream *ns = (struct NepVideoStream *) ioreq->io_Unit;
    struct NepClassVideo *ncv;
    WORD ret;

    KPRINTF(1, ("devBeginIO ioreq: 0x%08lx base: 0x%08lx cmd: %lu len: %ld\n", ioreq, base, ioreq->io_Command, ioreq->io_Length));

    ioreq->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->io_Error                   = 0;
    ioreq->io_Actual                  = 0;

    switch(ioreq->io_Command)
    {
        case NSCMD_UV_GETINFO:
            ret = cmdGetInfo(ioreq, ns);
            break;

        case NSCMD_UV_GETMODES:
            ret = cmdGetModes(ioreq, ns);
            break;

        case NSCMD_UV_GETSTATS:
            ret = cmdGetStats(ioreq, ns);
            break;

        case NSCMD_UV_GETTOPOLOGY:
            ret = cmdGetTopology(ioreq, ns);
            break;

        case NSCMD_UV_WAITEVENT:
            /* waits in the binding's queue until the camera reports something */
            ret = UVERR_GONE;
            Forbid();
            if((ncv = ns->ns_Binding))
            {
                ret = uvcEventWait(ncv, ioreq);
            }
            Permit();
            break;

        case CMD_READ:
            /* waits in the unit's queue until the transport has a frame for it */
            ret = UVERR_GONE;
            Forbid();
            if(ns->ns_Binding)
            {
                ioreq->io_Flags &= ~IOF_QUICK;
                AddTail(&ns->ns_Reads.ur_Queue, &ioreq->io_Message.mn_Node);
                ret = RC_DONTREPLY;
            }
            Permit();
            break;

        case CMD_FLUSH:
            uvcAbortReads(ns, IOERR_ABORTED);
            ret = RC_OK;
            break;

        case NSCMD_UV_SETMODE:
        case NSCMD_UV_REQUEST:
        case NSCMD_UV_GETCONTROL:
        case NSCMD_UV_NEXTCONTROL:
        case NSCMD_UV_SETCONTROL:
        case NSCMD_UV_SETTINGS:
        case CMD_START:
        case CMD_STOP:
            /* talks to the camera: the binding's subtask does it and replies */
            ret = UVERR_GONE;
            Forbid();
            if((ncv = ns->ns_Binding))
            {
                ioreq->io_Flags &= ~IOF_QUICK;
                PutMsg(ncv->ncv_CmdPort, &ioreq->io_Message);
                ret = RC_DONTREPLY;
            }
            Permit();
            break;

        case NSCMD_DEVICEQUERY:
            ret = cmdNSDeviceQuery(ioreq);
            break;

        default:
            ret = IOERR_NOCMD;
            break;
    }

    if(ret != RC_DONTREPLY)
    {
        ioreq->io_Error = ret;
        TermIO(ioreq, base);
    }
}

/* Only what waits can be called back - a read, a wait for an event; a
   command at the subtask runs to its end. */
LONG (devAbortIO)(struct IOStdReq * ioreq asm("a1"), struct NepVideoDevBase * base asm("a6"))
{
    struct NepVideoStream *ns = (struct NepVideoStream *) ioreq->io_Unit;
    struct IOStdReq *iocmp;
    LONG ret = -1;

    KPRINTF(5, ("devAbortIO ioreq: 0x%08lx\n", ioreq));

    Forbid();
    if((ioreq->io_Message.mn_Node.ln_Type == NT_MESSAGE) && (ioreq->io_Command == NSCMD_UV_WAITEVENT))
    {
        /* without its binding it has been replied already */
        ret = (ns->ns_Binding && uvcEventAbort(ns->ns_Binding, ioreq)) ? 0 : -1;
    }
    else if((ioreq->io_Message.mn_Node.ln_Type == NT_MESSAGE) && (ioreq->io_Command == CMD_READ))
    {
        if(ioreq == ns->ns_Reads.ur_Cur)
        {
            /* being filled: the engine gives it up at the next payload */
            ns->ns_Reads.ur_AbortCur = TRUE;
            ret = 0;
        } else {
            iocmp = (struct IOStdReq *) ns->ns_Reads.ur_Queue.lh_Head;
            while(iocmp->io_Message.mn_Node.ln_Succ)
            {
                if(iocmp == ioreq)
                {
                    Remove(&ioreq->io_Message.mn_Node);
                    ioreq->io_Actual = 0;
                    ioreq->io_Error = IOERR_ABORTED;
                    ReplyMsg(&ioreq->io_Message);
                    ret = 0;
                    break;
                }
                iocmp = (struct IOStdReq *) iocmp->io_Message.mn_Node.ln_Succ;
            }
        }
    }
    Permit();
    return ret;
}

/*
 *===========================================================
 * TermIO(ioreq, base)
 *===========================================================
 *
 * Return completed ioreq to sender.
 *
 */

void TermIO(struct IOStdReq *ioreq, struct NepVideoDevBase *base)
{
    ioreq->io_Message.mn_Node.ln_Type = NT_FREEMSG;

    /* If not quick I/O, reply the message
    */
    if(!(ioreq->io_Flags & IOF_QUICK))
    {
        ReplyMsg(&ioreq->io_Message);
    }
}
