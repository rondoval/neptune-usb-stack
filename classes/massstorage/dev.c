/* dev.c - usbscsi.device by Chris Hodges
*/

#include "debug.h"

#include "massstorage.class.h"

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

    return(base);

}

DEVBASETYPEPTR (devOpen)(struct IORequest * ioreq asm("a1"), ULONG unitnum asm("d0"), ULONG flags asm("d1"), struct NepMSDevBase * base asm("a6"))
{
    
    struct NepClassMS *ncm;

    KPRINTF(10, ("devOpen ioreq: 0x%08lx unit: %ld flags: 0x%08lx base: 0x%08lx\n",
                 ioreq, unitnum, flags, base));

    if(!base->np_UtilityBase)
    {
        if(!(base->np_UtilityBase = OpenLibrary("utility.library", 0)))
        {
            ioreq->io_Error = IOERR_OPENFAIL;
            return(NULL);
        }
    }

    ++base->np_Library.lib_OpenCnt;
    base->np_Library.lib_Flags &= ~LIBF_DELEXP;

    KPRINTF(10, ("devOpen: openCnt = %ld\n", base->np_Library.lib_OpenCnt));

    /* Damn f*cking programs which leave this field to zero! */
    if(ioreq->io_Message.mn_Length &&
       (ioreq->io_Message.mn_Length < sizeof(struct IOStdReq)))
    {
        KPRINTF(20, ("devOpen: invalid MN_LENGTH (%ld < %ld)!\n",
                ioreq->io_Message.mn_Length, sizeof(struct IOStdReq)));

        ioreq->io_Error = IOERR_BADLENGTH;
    } else {
        /* Default to open failure. */
        ioreq->io_Error = IOERR_OPENFAIL;

        ioreq->io_Unit = NULL;
        MS_FOREACH_UNIT(base->np_ClsBase, ncm)
        {
            if(ncm->ncm_UnitNo == unitnum)
            {
                if(!ncm->ncm_DenyRequests)
                {
                    ioreq->io_Unit = (struct Unit *) ncm;
                    break;
                }
            }
        }

        if(!ioreq->io_Unit)
        {
            ioreq->io_Error = IOERR_OPENFAIL;
            KPRINTF(20, ("devOpen: could not open unit!\n"));
        } else {
            /* Opended ok! */
            ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
            ioreq->io_Error                   = 0;
            ioreq->io_Unit->unit_OpenCnt++;

            return base;
        }
    }

    ioreq->io_Unit   = (APTR) -1;
    ioreq->io_Device = (APTR) -1;
    base->np_Library.lib_OpenCnt--;

    return(NULL);
    
}


BPTR (devClose)(struct IORequest * ioreq asm("a1"), struct NepMSDevBase * base asm("a6"))
{
    
    BPTR ret;
    struct NepClassMS *unit = (struct NepClassMS *) ioreq->io_Unit;

    KPRINTF(10, ("devClose ioreq: 0x%08lx base: 0x%08lx\n", ioreq, base));

    ret = BNULL;
    unit->ncm_Unit.unit_OpenCnt--;
    ioreq->io_Unit   = (APTR) -1;
    ioreq->io_Device = (APTR) -1;

    if(--base->np_Library.lib_OpenCnt == 0)
    {
        if(base->np_Library.lib_Flags & LIBF_DELEXP)
        {
            KPRINTF(5, ("devClose: calling expunge...\n"));
            ret = devExpunge(base, base);   /* extralh(d0)=base, base(a6)=base */
        }
    }

    KPRINTF(5, ("devClose: lib_OpenCnt = %ld\n", base->np_Library.lib_OpenCnt));

    return(ret);
    
}


BPTR (devExpunge)(DEVBASETYPEPTR extralh asm("d0"), struct NepMSDevBase * base asm("a6"))
{

    BPTR ret;

    KPRINTF(10, ("devExpunge base: 0x%08lx\n", base));

    ret = BNULL;

    if(base->np_Library.lib_OpenCnt == 0)
    {
        KPRINTF(5, ("devExpunge: Unloading...\n"));

        CloseLibrary(base->np_UtilityBase);

        ret = base->np_SegList;

        KPRINTF(5, ("devExpunge: removing device node 0x%08lx\n",
                    &base->np_Library.lib_Node));
        Remove(&base->np_Library.lib_Node);

        KPRINTF(5, ("devExpunge: FreeMem()...\n"));
        FreeMem((char *) base - base->np_Library.lib_NegSize,
                (ULONG) (base->np_Library.lib_NegSize + base->np_Library.lib_PosSize));

        KPRINTF(5, ("devExpunge: Unloading done! " DEVNAME " expunged!\n\n"));

        return(ret);
    }
    else
    {
        KPRINTF(5, ("devExpunge: Could not expunge, LIBF_DELEXP set!\n"));
        base->np_Library.lib_Flags |= LIBF_DELEXP;
    }

    return(BNULL);
    
}

DEVBASETYPEPTR (devReserved)(struct NepMSDevBase * base asm("a6"))
{
    return NULL;
}

/* The write fence: a virtual drive's blocks belong to usbscsivd.device, and on
   our own unit they are read-only to everyone else (mounter/vdrive.h). Returns
   0, or the error that completes the request. */
static WORD nVDriveFence(struct NepClassMS *unit, struct IOStdReq *ioreq)
{
    struct VDriveParent *parent = unit->ncm_VDriveParent;
    uint64_t offset = ioreq->io_Offset;

    switch(ioreq->io_Command)
    {
        case HD_SCSICMD:
            /* refused: the CHECK CONDITION and its sense are already in place */
            return VDriveScsiAllowed(parent, (struct IORequest *) ioreq,
                                     (struct SCSICmd *) ioreq->io_Data) ? 0 : HFERR_BadStatus;

        case TD_WRITE64:
        case TD_FORMAT64:
        case NSCMD_TD_WRITE64:
        case NSCMD_TD_FORMAT64:
            offset |= (uint64_t) ioreq->io_Actual << 32;
            /* fall through */
        case CMD_WRITE:
        case TD_FORMAT:
            return VDriveWriteAllowed(parent, (struct IORequest *) ioreq, offset,
                                      ioreq->io_Length) ? 0 : TDERR_WriteProt;

        default:
            return 0;
    }
}

void (devBeginIO)(struct IOStdReq * ioreq asm("a1"), struct NepMSDevBase * base asm("a6"))
{
    
    struct NepClassMS *unit = (struct NepClassMS *) ioreq->io_Unit;
    WORD ret = IOERR_NOCMD;

    KPRINTF(5, ("devBeginIO ioreq: 0x%08lx base: 0x%08lx cmd: %lu\n", ioreq, base, ioreq->io_Command));

    ioreq->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->io_Error                   = 0;

    if(ioreq->io_Command < NSCMD_DEVICEQUERY)
    {
        switch (ioreq->io_Command)
        {
            case TD_ADDCHANGEINT:
                Forbid();
                AddTail(&unit->ncm_DCInts, (struct Node *) ioreq);
                Permit();
                ret = RC_DONTREPLY;
                break;

            case TD_REMCHANGEINT:
                Forbid();
                Remove((struct Node *) ioreq);
                Permit();
                ret = 0;
                ioreq->io_Flags &= ~IOF_QUICK;
                break;

            case TD_CHANGENUM:
                ioreq->io_Actual = unit->ncm_ChangeCount;
                ret = 0;
                break;

            case TD_CHANGESTATE:
                ioreq->io_Actual = unit->ncm_UnitReady ? 0 : ~0;
                ret = 0;
                break;

            case TD_PROTSTATUS:
                ioreq->io_Actual = unit->ncm_WriteProtect ? ~0 : 0;
                ret = 0;
                break;

            case CMD_START:
            case CMD_STOP:
            case CMD_RESET:
           	case CMD_FLUSH:
            case CMD_READ:
            case CMD_WRITE:
            case TD_SEEK:
            case TD_FORMAT:
            case TD_GETGEOMETRY:
            case TD_EJECT:
            case TD_READ64:
            case TD_WRITE64:
            case TD_SEEK64:
            case TD_FORMAT64:
            case HD_SCSICMD:
                if(unit->ncm_DenyRequests)
                {
                    ret = TDERR_DiskChanged;
                } else if((ret = nVDriveFence(unit, ioreq)) == 0) {
                    ioreq->io_Flags &= ~IOF_QUICK;
                    ret = RC_DONTREPLY;
                    PutMsg(&unit->ncm_Unit.unit_MsgPort, (struct Message *) ioreq);
                }
                break;

            /* NOPs */
            case CMD_CLEAR:
            case CMD_UPDATE:
            case TD_MOTOR:
                ret = 0;
                break;

            default:
                ret = IOERR_NOCMD;
                break;
        }
    } else {
        switch(ioreq->io_Command)
        {
            case NSCMD_DEVICEQUERY:
                ret = cmdNSDeviceQuery((struct IOStdReq *) ioreq, unit, base);
                break;

            case NSCMD_TD_READ64:
            case NSCMD_TD_WRITE64:
            case NSCMD_TD_FORMAT64:
            case NSCMD_TD_SEEK64:
                if(unit->ncm_DenyRequests)
                {
                    ret = IOERR_ABORTED;
                } else if((ret = nVDriveFence(unit, ioreq)) == 0) {
                    ioreq->io_Flags &= ~IOF_QUICK;
                    ret = RC_DONTREPLY;
                    PutMsg(&unit->ncm_Unit.unit_MsgPort, (struct Message *) ioreq);
                }
                break;

            default:
                ret = IOERR_NOCMD;
                break;
        }
    }

    if(ret != RC_DONTREPLY)
    {
        KPRINTF(1, ("TermIO\n"));
        if (ret != RC_OK)
        {
            /* Set error codes  */
            ioreq->io_Error = ret & 0xff;
        }
        /* Terminate the iorequest */
        TermIO(ioreq, base);
    }
    
}

LONG (devAbortIO)(struct IOStdReq * ioreq asm("a1"), struct NepMSDevBase * base asm("a6"))
{

    struct NepClassMS *unit = (struct NepClassMS *) ioreq->io_Unit;
    struct IOStdReq *iocmp;

    KPRINTF(5, ("devAbortIO ioreq: 0x%08lx\n", ioreq));

    /* Is it pending? */
    if(ioreq->io_Message.mn_Node.ln_Type == NT_MESSAGE)
    {
        Forbid(); /* the unit task owns these structures */
        /* Still queued? Pull it out and reply it ourselves. */
        iocmp = (struct IOStdReq *) unit->ncm_XFerQueue.lh_Head;
        while(iocmp->io_Message.mn_Node.ln_Succ)
        {
            if(iocmp == ioreq)
            {
                Remove((struct Node *) ioreq);
                ioreq->io_Error = IOERR_ABORTED;
                nReplyIO(base->np_ClsBase, ioreq);
                Permit();
                return(0);
            }
            iocmp = (struct IOStdReq *) iocmp->io_Message.mn_Node.ln_Succ;
        }
        /* In flight on a UAS tag? Flag it and poke the task - the pipes are
           aborted in the task's own context and the request completes with
           IOERR_ABORTED through the normal reap (an abort is a wish). */
        MS_FOREACH_TAG(unit, ut)
        {
            if((ut->ut_State != UTS_FREE) && (ut->ut_IOReq == ioreq))
            {
                ut->ut_AbortReq = TRUE;
                nWakeTransport(unit);
                Permit();
                return(0);
            }
        }
        Permit();
    }
    /* Anything else is a BOT request the unit task is executing right now: it
       runs to completion in that task's context, so there is nothing here to
       take away from it. Refuse the abort (an abort is a wish). */
    return(-1);

}

/* NSD stuff */

static
const UWORD NSDSupported[] =
{
    CMD_CLEAR, CMD_RESET,
    CMD_FLUSH, CMD_READ,
    CMD_WRITE, CMD_START,
    CMD_STOP, CMD_UPDATE,
    TD_SEEK,
    TD_MOTOR, TD_EJECT,
    TD_CHANGENUM, TD_PROTSTATUS,
    TD_CHANGESTATE,
    TD_FORMAT,
    TD_GETGEOMETRY,
    TD_ADDCHANGEINT, TD_REMCHANGEINT,
    TD_READ64, TD_WRITE64, TD_SEEK64,
    TD_FORMAT64,
    HD_SCSICMD,
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_TD_FORMAT64,
    NSCMD_TD_SEEK64,
    NSCMD_DEVICEQUERY, 0
};

WORD cmdNSDeviceQuery(struct IOStdReq *ioreq,
                      struct NepClassMS *unit,
                      struct NepMSDevBase *base)
{
    struct my_NSDeviceQueryResult *query;

    query = (struct my_NSDeviceQueryResult *) ioreq->io_Data;

    KPRINTF(10, ("NSCMD_DEVICEQUERY ioreq: 0x%08lx query: 0x%08lx\n", ioreq, query));

    /* NULL ptr?
       Enough data?
       Valid request?
    */
    if((!query) ||
       (ioreq->io_Length < sizeof(struct my_NSDeviceQueryResult)) ||
       (query->DevQueryFormat != 0) ||
       (query->SizeAvailable != 0))
    {
        /* Return error. This is special handling, since iorequest is only
           guaranteed to be sizeof(struct IOStdReq). If we'd let our
           devBeginIO dispatcher return the error, it would trash some
           memory past end of the iorequest (ios2_WireError field).
         */
         ioreq->io_Error = IOERR_NOCMD;
         TermIO((struct IOStdReq *) ioreq, base);

         /* Don't reply, we already did. */
         return RC_DONTREPLY;
    }

    ioreq->io_Actual         = query->SizeAvailable
                             = sizeof(struct my_NSDeviceQueryResult);
    query->DeviceType        = NSDEVTYPE_TRACKDISK;
    query->DeviceSubType     = 0;
    query->SupportedCommands = NSDSupported;

    /* Return success (note that this will NOT poke ios2_WireError).
    */
    return RC_OK;
}

void TermIO(struct IOStdReq *ioreq,
            struct NepMSDevBase *base)
{
    ioreq->io_Message.mn_Node.ln_Type = NT_FREEMSG;

    /* If not quick I/O, reply the message */
    if(!(ioreq->io_Flags & IOF_QUICK))
    {
        nReplyIO(base->np_ClsBase, ioreq);
    }
}
