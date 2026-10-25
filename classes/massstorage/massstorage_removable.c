/*
 *----------------------------------------------------------------------------
 *                         massstorage class for Neptune
 *----------------------------------------------------------------------------
 * Based on the Poseidon massstorage class by Chris Hodges <chrisly@platon42.de>.
 */

#include "debug.h"

#include "massstorage.class.h"

#include "mounter/mounter.h"   /* MountDrive() result for the stats message */

extern const STRPTR libname;

/*
 * The removable task: one sub-task per class that polls every removable unit
 * with TEST UNIT READY, turns the answers into media edges (insert, remove,
 * change), mounts what appears, and feeds the ROM boot gate through
 * ncm_MediaUnsettled / UCM_MediaPending. Pre-DOS it runs as a plain Task and
 * watches for dos.library; when DOS appears it retires itself and is
 * reincarnated as a Process, which is what makes DoPkt()-based mounting legal
 * (see nOpenDOS). The Process runs until the class is expunged.
 *
 * The file reads bottom-up: every static is defined before its callers.
 * In order:
 *
 *   1. Task spawn             nStartRemovableTask (before the ps macro:
 *                             its parameter is the neptune.library base itself)
 *   2. Setup and teardown     nAllocRT, nFreeRT
 *   3. Small helpers          pre-DOS predicate, auto-unmount
 *   4. Media state machine    one TEST UNIT READY and its two outcomes
 *   5. Mount pass             change announcement and the mount dispatch
 *   6. Tick                   per-unit step, poll cadence, timer re-arm
 *   7. DOS arrival            the pre-DOS watch that retires this incarnation
 *   8. Task main loop         nRemovableTask
 */

/*
 *----------------------------------------------------------------------------
 * 1. Task spawn
 *----------------------------------------------------------------------------
 */

/* /// "nStartRemovableTask()" */
BOOL nStartRemovableTask(struct Library *ps, struct NepMSBase *nh)
{
    struct Task *tmptask;
    ObtainSemaphore(&nh->nh_TaskLock);
    if(nh->nh_RemovableTask)
    {
        ReleaseSemaphore(&nh->nh_TaskLock);
        return(TRUE);
    }

    nh->nh_ReadySignal = SIGB_SINGLE;
    nh->nh_ReadySigTask = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    if((tmptask = psdSpawnSubTask(CLASS_NAME " Removable Task", nRemovableTask, nh)))
    {
        psdBorrowLocksWait(tmptask, 1UL<<nh->nh_ReadySignal);
    }
    nh->nh_ReadySigTask = NULL;
    //FreeSignal(nh->nh_ReadySignal);
    if(nh->nh_RemovableTask)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Removable Task started.");
        ReleaseSemaphore(&nh->nh_TaskLock);
        return(TRUE);
    }
    ReleaseSemaphore(&nh->nh_TaskLock);
    return(FALSE);
}
/* \\\ */

#undef  ps
#define ps nh->nh_PsdBase
#undef  ExpansionBase
#define ExpansionBase nh->nh_ExpansionBase

/*
 *----------------------------------------------------------------------------
 * 2. Setup and teardown
 *----------------------------------------------------------------------------
 */

/* /// "nAllocRT()" */
struct NepMSBase * nAllocRT(void)
{
    struct Task *thistask;
    struct NepMSBase *nh;

    thistask = FindTask(NULL);
    nh = thistask->tc_UserData;
    do
    {
        if(!(ExpansionBase = (APTR) OpenLibrary("expansion.library", 37)))
        {
            Alert(AG_OpenLib | AO_ExpansionLib);
            break;
        }
        if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
        {
            Alert(AG_OpenLib | AO_Unknown);
            break;
        }
        if(!(nh->nh_IOMsgPort = CreateMsgPort()))
        {
            break;
        }
        nh->nh_IOReq.io_Message.mn_ReplyPort = nh->nh_IOMsgPort;
        if(!(nh->nh_TimerMsgPort = CreateMsgPort()))
        {
            break;
        }
        if(!(nh->nh_TimerIOReq = (struct timerequest *) CreateIORequest(nh->nh_TimerMsgPort, sizeof(struct timerequest))))
        {
            break;
        }
        if(OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *) nh->nh_TimerIOReq, 0))
        {
            break;
        }
        /* Start removable interrupt */
        nh->nh_TimerIOReq->tr_node.io_Command = TR_ADDREQUEST;
        nh->nh_TimerIOReq->tr_time.tv_secs = 0;
        nh->nh_TimerIOReq->tr_time.tv_micro = 50;
        SendIO((struct IORequest *) nh->nh_TimerIOReq);
        nh->nh_RemovableTask = thistask;
        return(nh);
    } while(FALSE);
    if(ExpansionBase)
    {
        CloseLibrary((struct Library *) ExpansionBase);
        ExpansionBase = NULL;
    }
    if(ps)
    {
        CloseLibrary(ps);
        ps = NULL;
    }

    if(nh->nh_TimerIOReq)
    {
        if(nh->nh_TimerIOReq->tr_node.io_Device)
        {
            CloseDevice((struct IORequest *) nh->nh_TimerIOReq);
        }
        DeleteIORequest((struct IORequest *) nh->nh_TimerIOReq);
        nh->nh_TimerIOReq = NULL;
    }
    if(nh->nh_TimerMsgPort)
    {
        DeleteMsgPort(nh->nh_TimerMsgPort);
        nh->nh_TimerMsgPort = NULL;
    }
    if(nh->nh_IOMsgPort)
    {
        DeleteMsgPort(nh->nh_IOMsgPort);
        nh->nh_IOMsgPort = NULL;
    }
    Forbid();
    nh->nh_RemovableTask = NULL;
    if(nh->nh_ReadySigTask)
    {
        Signal(nh->nh_ReadySigTask, 1L<<nh->nh_ReadySignal);
    }
    return(NULL);
}
/* \\\ */

/* /// "nFreeRT()" */
void nFreeRT(struct NepMSBase *nh)
{
    if(nh->nh_DOSBase)
    {
        CloseLibrary(nh->nh_DOSBase);
        nh->nh_DOSBase = NULL;
    }
    CloseLibrary((struct Library *) ExpansionBase);
    ExpansionBase = NULL;
    CloseLibrary(ps);
    ps = NULL;

    AbortIO((struct IORequest *) nh->nh_TimerIOReq);
    WaitIO((struct IORequest *) nh->nh_TimerIOReq);
    CloseDevice((struct IORequest *) nh->nh_TimerIOReq);
    DeleteIORequest((struct IORequest *) nh->nh_TimerIOReq);
    DeleteMsgPort(nh->nh_TimerMsgPort);
    nh->nh_TimerMsgPort = NULL;
    nh->nh_TimerIOReq = NULL;

    Forbid();
    nh->nh_RemovableTask = NULL;
    if(nh->nh_ReadySigTask)
    {
        Signal(nh->nh_ReadySigTask, 1L<<nh->nh_ReadySignal);
    }
    if(nh->nh_RestartIt)
    {
        // wake up every task to relaunch removable task
        struct NepClassMS *ncm;
        MS_FOREACH_UNIT(nh, ncm)
        {
            nWakeUnitTask(ncm);
        }
        nh->nh_RestartIt = FALSE;
    }
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 3. Small helpers
 *----------------------------------------------------------------------------
 */

/* Pre-DOS the removable task is spawned as a plain Task; once DOS exists it
   is retired (nRTDOSArrived) and reincarnated as a Process, so the node type
   doubles as "does the boot gate still watch us" (nOpenDOS keys DoPkt()
   legality off the same test). */
static inline BOOL nRTIsPreDOS(struct NepMSBase *nh)
{
    return (nh->nh_RemovableTask->tc_Node.ln_Type == NT_TASK);
}

/* /// "nRTAutoUnmount()" */
/* A gone unit takes its partitions with it, if so configured. */
static void nRTAutoUnmount(struct NepClassMS *ncm)
{
    if(ncm->ncm_DenyRequests && ncm->ncm_CUC->cuc_AutoUnmount && ncm->ncm_HasMounted)
    {
        nUnmountPartition(ncm);
        ncm->ncm_HasMounted = FALSE;
    }
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 4. Media state machine - one TEST UNIT READY and its two outcomes
 *----------------------------------------------------------------------------
 */

/* /// "nRTMediaFailed()" */
/* The TUR failed: classify the sense into the unit's boot-gate verdict and
   media edges. */
static void nRTMediaFailed(struct NepMSBase *nh, struct NepClassMS *ncm,
                           struct SCSICmd *scsicmd, const UBYTE *sensedata)
{
    /* Bytes the device did not actually return are garbage, not this answer.
       A command that fails without autosense is exactly what a bus reset
       produces, so believing stale bytes here is how a warm boot ends up
       misclassified. */
    BOOL  haveSense = (scsicmd->scsi_SenseActual >= 14);
    UBYTE sensekey  = haveSense ? (sensedata[2] & SK_MASK) : 0xff;
    UBYTE asc       = haveSense ? sensedata[12] : 0;

    KPRINTF(1, ("Test unit ready yielded: %ld/%ld\n", sensekey, asc));
    /* Which failure this is decides whether anyone waiting on us should keep
       waiting.

       ASC 04 is LOGICAL UNIT NOT READY - spinning up, reading the TOC,
       initialising - and a mount is still coming. ASC 3A is MEDIUM NOT
       PRESENT: an empty tray or a cardless reader, which will never mount and
       must not hold a boot open; that is the one answer that settles the unit
       negatively.

       UNIT ATTENTION is the first answer after any bus reset (ASC 29), and a
       warm reboot re-enumerates the whole bus without cutting power, so an
       already spun-up disc greets us with it. It says nothing about the
       medium: keep waiting and re-test. ASC 28 and 3A additionally mean the
       medium may have changed under us, so force a re-mount - the transports
       only do that under PFF_REM_SUPPORT, which is not a default flag.

       Anything else, including an answer we could not read, is inconclusive
       rather than negative, so it also keeps the unit unsettled - but on a
       budget, so a wedged drive costs a bounded delay instead of the gate's
       full media timeout on every boot. */
    if((sensekey == SK_NOT_READY) && (asc == 0x3a))
    {
        ncm->ncm_MediaUnsettled = FALSE;
    }
    else if((sensekey == SK_NOT_READY) && (asc == 0x04))
    {
        /* Unbudgeted: the drive is explicitly asking us to
           wait, and the gate's own cap bounds that. */
        ncm->ncm_MediaUnsettled = TRUE;
    }
    else if(ncm->ncm_SenseRetries)
    {
        ncm->ncm_SenseRetries--;
        ncm->ncm_MediaUnsettled = TRUE;
    }
    else
    {
        ncm->ncm_MediaUnsettled = FALSE;
    }

    if((sensekey == SK_UNIT_ATTENTION) &&
       ((asc == 0x28) || (asc == 0x3a)))
    {
        ncm->ncm_ChangeCount++;
        KPRINTF(10, ("Diskchange: Unit attention (count = %ld)!\n", ncm->ncm_ChangeCount));
    }

    /* Check for MEDIUM NOT PRESENT */
    if((sensekey == SK_NOT_READY) &&
       ((asc == 0x3a) || (asc == 0x04)))
    {
        if(ncm->ncm_UnitReady)
        {
            ncm->ncm_UnitReady = FALSE;
            ncm->ncm_ChangeCount++;
            KPRINTF(10, ("Diskchange: Medium removed (count = %ld)!\n", ncm->ncm_ChangeCount));
            if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
            {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Diskchange: Medium removed (count = %ld)",
                               ncm->ncm_ChangeCount);
            }
        }
    }
}
/* \\\ */

/* /// "nRTMediaReady()" */
/* A medium is there - but that is not the answer the boot gate is waiting
   for, which is the volume on the mount list. So the unit deliberately stays
   unsettled until the mount below resolves it, and the gate cannot release in
   the window between this TUR and the BootNode appearing. */
static void nRTMediaReady(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    if(!ncm->ncm_UnitReady)
    {
        ncm->ncm_UnitReady = TRUE;
        ncm->ncm_ChangeCount++;
        KPRINTF(10, ("Diskchange: Medium inserted (count = %ld)!\n", ncm->ncm_ChangeCount));
        if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Diskchange: Medium inserted (count = %ld)",
                           ncm->ncm_ChangeCount);
        }
        if(ncm->ncm_CSType == MS_UFI_SUBCLASS)
        {
            nh->nh_IOReq.io_Command = CMD_START;
            nIOCmdTunnel(ncm, &nh->nh_IOReq);
        }
    } else {
        if(ncm->ncm_CDC->cdc_PatchFlags & PFF_REM_SUPPORT)
        {
            nGetWriteProtect(ncm);
        }
        /* Ready, and no change outstanding to act on:
           whatever left this unit unsettled - a UNIT
           ATTENTION carrying no media change, say - is
           answered, and no mount is coming. */
        if(ncm->ncm_LastChange == ncm->ncm_ChangeCount)
        {
            ncm->ncm_MediaUnsettled = FALSE;
        }
    }
}
/* \\\ */

/* /// "nRTPollMedia()" */
/* One TEST UNIT READY, dispatched to the outcome above that matches. */
static void nRTPollMedia(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    UBYTE cmd6[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    UBYTE sensedata[18];
    struct SCSICmd scsicmd;

    scsicmd.scsi_Data = NULL;
    scsicmd.scsi_Length = 0;
    scsicmd.scsi_Command = cmd6;
    scsicmd.scsi_CmdLength = 6;
    scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
    scsicmd.scsi_SenseData = sensedata;
    scsicmd.scsi_SenseLength = 18;
    scsicmd.scsi_SenseActual = 0;
    if(nScsiDirectTunnel(ncm, &scsicmd))
    {
        nRTMediaFailed(nh, ncm, &scsicmd, sensedata);
    } else {
        nRTMediaReady(nh, ncm);
    }
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 5. Mount pass
 *----------------------------------------------------------------------------
 */

/* /// "nRTAnnounceChange()" */
/* A real media change: refresh what the old medium's answers no longer cover,
   then fire the registered disk-change interrupts. */
static void nRTAnnounceChange(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    if(ncm->ncm_UnitReady)
    {
        nGetWriteProtect(ncm);
        if(ncm->ncm_CDC->cdc_PatchFlags & PFF_REM_SUPPORT)
        {
            nh->nh_IOReq.io_Command = TD_GETGEOMETRY;
            nh->nh_IOReq.io_Data = &ncm->ncm_Geometry;
            nh->nh_IOReq.io_Length = sizeof(ncm->ncm_Geometry);
            nIOCmdTunnel(ncm, &nh->nh_IOReq);
        }
    }

    /* the filesystems add and take away their requests under Forbid()
       (devBeginIO), each from its own task */
    Forbid();
    struct IOStdReq *ioreq = (struct IOStdReq *) ncm->ncm_DCInts.lh_Head;

    while(((struct Node *) ioreq)->ln_Succ)
    {
        Cause(ioreq->io_Data);
        ioreq = (struct IOStdReq *) ((struct Node *) ioreq)->ln_Succ;
    }
    Permit();
}
/* \\\ */

/* /// "nRTMountPass()" */
/* Two reasons to run the mount dispatch, and only one of them is a media
   change. The synthetic one - DOS has just appeared, so retry whatever needed
   a loadable handler - must not Cause() the disk-change interrupts: telling
   DOS the medium changed under the volume it is booting from is what produces
   "Please replace volume X in any drive". */
static void nRTMountPass(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    BOOL mediaChanged = (ncm->ncm_LastChange != ncm->ncm_ChangeCount);

    if((!mediaChanged) && (!ncm->ncm_RemountPending))
    {
        return;
    }
    ncm->ncm_RemountPending = FALSE;

    if(mediaChanged)
    {
        nRTAnnounceChange(nh, ncm);
    }
    BOOL retryMount = FALSE;

    /* the safe-eject latch vetoes re-mounting only; LastChange
       still advances below so latched ChangeCount bumps (UNIT
       ATTENTION after STOP, TUR edges) are consumed, not
       re-tested every tick */
    if(ncm->ncm_UnitReady && (!ncm->ncm_Ejected))
    {
        // obtain blocksize first
        if(!ncm->ncm_BlockSize)
        {
            nh->nh_IOReq.io_Command = TD_GETGEOMETRY;
            nh->nh_IOReq.io_Data = &ncm->ncm_Geometry;
            nh->nh_IOReq.io_Length = sizeof(ncm->ncm_Geometry);
            nIOCmdTunnel(ncm, &nh->nh_IOReq);
        }
        // mount the medium (RDB/MBR/GPT/superfloppy/ISO9660);
        // the mounter dispatches on the device type
        struct MountResult mstats;

        ncm->ncm_HasMounted = nMountDrive(ncm, &mstats);

        psdAddErrorMsg(mstats.renamed ? RETURN_WARN : RETURN_OK,
                       (STRPTR) libname,
                       "Unit %ld: %ld mounted, %ld already, %ld deferred, %ld renamed.",
                       ncm->ncm_UnitNo, mstats.mounted, mstats.alreadyMounted,
                       mstats.deferred, mstats.renamed);

        /* A mount can fail on a drive that has only just become
           ready: the mounter retries READ TOC and the PVD read
           three times back to back with no delay, which a
           still-settling optical drive loses. Before DOS exists
           that is fatal rather than cosmetic, because
           AddBootNode() only yields a bootable node while there
           is no dos.library - a later attempt can never put the
           drive in the boot menu. So retry, on a budget, by
           leaving LastChange behind for the next tick.

           A deferred volume is not a failed one: the medium read
           fine and only its handler needs DOS, which no retry can
           conjure. */
        retryMount = (!ncm->ncm_HasMounted) && (mstats.deferred == 0) &&
                     (ncm->ncm_MountRetries != 0) &&
                     nRTIsPreDOS(nh);
    }

    if(retryMount)
    {
        ncm->ncm_MountRetries--;
        ncm->ncm_MediaUnsettled = TRUE;
        KPRINTF(10, ("Mount failed, %ld pre-DOS retries left\n",
                     (ULONG) ncm->ncm_MountRetries));
    } else {
        ncm->ncm_MediaUnsettled = FALSE;
        ncm->ncm_LastChange = ncm->ncm_ChangeCount;
    }
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 6. Tick - per-unit step, poll cadence, timer re-arm
 *----------------------------------------------------------------------------
 */

/* /// "nRTHandleUnit()" */
/* One unit's share of a tick. */
static void nRTHandleUnit(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    if((!ncm->ncm_Task) || ncm->ncm_DenyRequests)
    {
        nRTAutoUnmount(ncm);
        return;
    }
    if(ncm->ncm_Removable && ncm->ncm_Running)
    {
        nRTPollMedia(nh, ncm);
    }
    /* The mount pass runs for EVERY live unit, not just removable ones - that
       is how fixed disks get mounted: their ChangeCount edge comes from the
       unit task at bind time, not from a TUR here. */
    nRTMountPass(nh, ncm);
}
/* \\\ */

/* /// "nRTArmTimer()" */
/* While the ROM boot gate is still watching us - no DOS yet - and some unit
   has not settled, poll hard. The gate's only guaranteed wait after mass
   storage binds is shorter than the housekeeping period, so one answer that
   arrives a period late costs the drive its place in the boot menu, which is
   unrecoverable (see the mount retry above). Once DOS exists this is the
   unchanged three second poll. */
static void nRTArmTimer(struct NepMSBase *nh)
{
    ULONG pollMS = 0;

    if(nRTIsPreDOS(nh))
    {
        struct NepClassMS *ncm;
        MS_FOREACH_UNIT(nh, ncm)
        {
            if(ncm->ncm_MediaUnsettled && (!ncm->ncm_DenyRequests))
            {
                pollMS = RT_FAST_POLL_MS;
                break;
            }
        }
    }

    nh->nh_TimerIOReq->tr_time.tv_secs  = pollMS ? 0 : RT_POLL_SECS;
    nh->nh_TimerIOReq->tr_time.tv_micro = pollMS * 1000;
    SendIO((struct IORequest *) nh->nh_TimerIOReq);
}
/* \\\ */

/* /// "nRTHandleTick()" */
/* One timer tick: every unit's poll, then the re-arm - exactly one SendIO per
   received timer message, the single in-flight timerequest is the invariant. */
static void nRTHandleTick(struct NepMSBase *nh)
{
    KPRINTF(2, ("Timer interrupt\n"));

    struct NepClassMS *ncm;

    MS_FOREACH_UNIT(nh, ncm)
    {
        nRTHandleUnit(nh, ncm);
    }
    nRTArmTimer(nh);
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 7. DOS arrival
 *----------------------------------------------------------------------------
 */

/* /// "nRTDOSArrived()" */
/* TRUE = dos.library exists now, and this pre-DOS incarnation has arranged
   its own succession: the unit tasks are poked (via nh_RestartIt in nFreeRT)
   to relaunch us as a Process. */
static BOOL nRTDOSArrived(struct NepMSBase *nh)
{
    APTR doslib = OpenLibrary("dos.library", 39);

    if(!doslib)
    {
        return FALSE;
    }
    CloseLibrary(doslib);
    /* Ask for one more mount pass now that handlers can be loaded
       from L:. Not a ChangeCount bump: nothing about the medium has
       changed, and saying otherwise unmounts the volume DOS may be
       booting off right now. The mounter skips extents it already
       mounted, so this only picks up what the pre-DOS pass could
       not reach. */
    struct NepClassMS *ncm;
    MS_FOREACH_UNIT(nh, ncm)
    {
        /* Only units with something still to gain. A drive that
           mounted completely must not be probed again: the
           mounter would find its DOS names taken and mount a
           second set of nodes beside the live ones, which breaks
           the boot it is booting from. */
        if(ncm->ncm_MountDeferred || (!ncm->ncm_HasMounted))
        {
            ncm->ncm_RemountPending = TRUE;
            ncm->ncm_ForceRTCheck = TRUE;
        }
    }

    /* restart task */
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "DOS found, stopping removable task...");
    nh->nh_RestartIt = TRUE;
    return TRUE;
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 8. Task main loop
 *----------------------------------------------------------------------------
 */

/* /// "nRemovableTask()" */
void nRemovableTask(void)
{
    struct NepMSBase *nh = nAllocRT();

    if(!nh)
    {
        return;
    }
    Forbid();
    if(nh->nh_ReadySigTask)
    {
        Signal(nh->nh_ReadySigTask, 1L<<nh->nh_ReadySignal);
    }
    Permit();

    ULONG sigmask = (1L<<nh->nh_TimerMsgPort->mp_SigBit)|
                    SIGBREAKF_CTRL_C;
    ULONG sigs = 0;

    /* No leaving when the last unit has gone: a unit bound meanwhile would
       find this task still there (nStartRemovableTask) and never be mounted,
       and a fixed disk asks only once. Only DOS arrival (a successor) and
       CTRL-C (class expunge) end it. */
    do
    {
        while(GetMsg(nh->nh_TimerMsgPort))
        {
            nRTHandleTick(nh);
        }

        if(nRTIsPreDOS(nh) && nRTDOSArrived(nh))
        {
            break;
        }
        sigs = Wait(sigmask);
    } while(!(sigs & SIGBREAKF_CTRL_C));

    struct NepClassMS *ncm;
    MS_FOREACH_UNIT(nh, ncm)
    {
        nRTAutoUnmount(ncm);
    }
    KPRINTF(20, ("Going down the river!\n"));
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Removable Task stopped.");
    nFreeRT(nh);
}
/* \\\ */
