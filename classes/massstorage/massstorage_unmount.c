/*
 *----------------------------------------------------------------------------
 *                         massstorage class for Neptune
 *----------------------------------------------------------------------------
 * Based on the Poseidon massstorage class by Chris Hodges <chrisly@platon42.de>.
 */

#include "debug.h"

#include "massstorage.class.h"

extern const STRPTR libname;

/*
 * Unmount and eject: the DOS devices riding a unit, and taking them down.
 * When the unit has gone, the removable task unmounts them, keeping those
 * whose volumes are still in use until the unit is back (nUnmountPartition(),
 * nRelistKeptNodes(), nRemountKeptNodes()); when the user ejects the device,
 * all of its units' devices go, or none (nSafeEjectDevice()).
 *
 * The file reads bottom-up: every static is defined before its callers.
 * In order:
 *
 *   1. DOS access          dos.library, for a caller that is a Process
 *   2. Our DOS devices     which nodes ride a unit; taking them out of the list
 *   3. One handler         what it holds, the inhibit and its verdict, the end
 *   4. Unplug              a gone unit's devices, and the ones kept in use
 *   5. Safe eject          all of a device's devices, or none
 */

#undef  ps
#define ps nh->nh_PsdBase
/* Each expands against the function's own nh local. ExpansionBase is opened
   and owned by the removable task (nAllocRT), DOSBase by nOpenDOS(). */
#undef  ExpansionBase
#define ExpansionBase nh->nh_ExpansionBase
#undef  DOSBase
#define DOSBase nh->nh_DOSBase

/* dol_Startup of a DOS device is a BPTR to a FileSysStartupMsg, or a small
   number for a handler that takes one (a mount entry's Startup = 1): nothing
   at or below this address is a startup message */
#define STARTUP_POINTER_MIN     0x1000
/* past the last DE_* index any filesystem uses: an environment that claims a
   table this large is not one */
#define ENVEC_TABLESIZE_LIMIT   32
/* room for "Volume (DEVICE:)" in an eject's veto message */
#define BUSYNAME_LENGTH         64

/*
 *----------------------------------------------------------------------------
 * 1. DOS access
 *----------------------------------------------------------------------------
 */

/* /// "nOpenDOS()" */
/**
 * nOpenDOS() - Make dos.library usable for the calling task.
 * @nh: the class base.
 *
 * Only a Process may send the packets this file sends. The removable task
 * runs as a plain Task until DOS is up and is then started again as a Process
 * (massstorage_removable.c); a safe eject runs on its caller's task.
 * dos.library is opened once, for the class.
 *
 * Return: TRUE if the caller is a Process and dos.library is open.
 */
static BOOL nOpenDOS(struct NepMSBase *nh)
{
    if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
    {
        return FALSE;
    }
    if(!nh->nh_DOSBase)
    {
        nh->nh_DOSBase = OpenLibrary("dos.library", 39);
    }
    return nh->nh_DOSBase != NULL;
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 2. Our DOS devices
 *----------------------------------------------------------------------------
 */

/* /// "nBstrEquals()" */
/**
 * nBstrEquals() - Tell whether a BSTR holds a name.
 * @bstr: the BSTR: a length byte, then the characters.
 * @name: the name.
 *
 * A length that counts a terminating NUL as well matches too: MakeDosNode()
 * stores device names that way, a mount entry need not.
 *
 * Return: TRUE if @bstr holds @name.
 */
static BOOL nBstrEquals(const UBYTE *bstr, const char *name)
{
    ULONG length = strlen(name);
    ULONG stored = bstr[0];

    if((stored != length) && !((stored == length + 1) && !bstr[stored]))
    {
        return FALSE;
    }
    return memcmp(bstr + 1, name, length) == 0;
}
/* \\\ */

/* /// "nDosEntryMatches()" */
/**
 * nDosEntryMatches() - Tell whether a DOS device rides a unit.
 * @ncm: the unit.
 * @list: the DOS device entry; the caller holds a DOS device list lock.
 *
 * It does if its startup names usbscsi.device and the unit (anything the
 * mounter, or a user's DOSDriver, made for the medium), or usbscsivd.device
 * and one of the unit's virtual drives.
 *
 * Return: TRUE if @list rides on @ncm.
 */
static BOOL nDosEntryMatches(struct NepClassMS *ncm, struct DosList *list)
{
    struct FileSysStartupMsg *fssm = BADDR(list->dol_misc.dol_handler.dol_Startup);

    if((ULONG) fssm <= STARTUP_POINTER_MIN)
    {
        return FALSE;
    }
    struct DosEnvec *environment = BADDR(fssm->fssm_Environ);
    UBYTE *device = BADDR(fssm->fssm_Device);
    if(!TypeOfMem(environment) || !TypeOfMem(device) ||
       (environment->de_TableSize == 0) || (environment->de_TableSize >= ENVEC_TABLESIZE_LIMIT))
    {
        return FALSE;
    }
    if(nBstrEquals(device, DEVNAME))
    {
        return fssm->fssm_Unit == ncm->ncm_UnitNo;
    }
    if(nBstrEquals(device, VDNAME))
    {
        return VDRIVE_PARENT(fssm->fssm_Unit) == ncm->ncm_UnitNo;
    }
    return FALSE;
}
/* \\\ */

/* /// "nUnlistDosNode()" */
/**
 * nUnlistDosNode() - Take a DeviceNode out of the DOS list.
 * @nh: the class base.
 * @node: the node; its handler is still running.
 *
 * Done before the handler is inhibited. Inhibiting takes the volume away,
 * which makes a desktop rescan its disk icons; a device it still finds then
 * answers ACTION_DISK_INFO with ID_BUSY and gets a "BUSY" icon. Taking the
 * node away later changes no volume, so nothing makes the desktop look again
 * and the icon stays.
 *
 * Return: TRUE if the node was in the list.
 */
static BOOL nUnlistDosNode(struct NepMSBase *nh, struct DeviceNode *node)
{
    LockDosList(LDF_DEVICES | LDF_WRITE);
    BOOL unlisted = RemDosEntry((struct DosList *) node);
    UnLockDosList(LDF_DEVICES | LDF_WRITE);
    return unlisted;
}
/* \\\ */

/* /// "nUnlistNextNode()" */
/**
 * nUnlistNextNode() - Take the next DOS device riding a unit out of the DOS
 * list.
 * @nh: the class base.
 * @ncm: the unit.
 *
 * Found and removed under one write lock, so no other task (a safe eject) can
 * take the node in between; before the inhibit, as nUnlistDosNode() says.
 *
 * Return: the node, out of the DOS list, or NULL when none is left.
 */
static struct DeviceNode * nUnlistNextNode(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    struct DeviceNode *node = NULL;
    struct DosList *list = LockDosList(LDF_DEVICES | LDF_WRITE);

    while((list = NextDosEntry(list, LDF_DEVICES | LDF_WRITE)))
    {
        if(nDosEntryMatches(ncm, list))
        {
            RemDosEntry(list);
            node = (struct DeviceNode *) list;
            break;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_WRITE);
    return node;
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 3. One handler
 *----------------------------------------------------------------------------
 */

/* What a handler holds, as the DOS list shows it (nHandlerVolume()) */
enum VolumeHold
{
    HOLD_NONE,      /* no volume: nothing mounted, or dropped with its medium */
    HOLD_MOUNTED,   /* a volume mounted (dol_Task is the handler) */
    HOLD_PARKED,    /* a volume kept for its medium, with locks parked on it */
    HOLD_ADOPTED    /* none, and the volume it held is another handler's now */
};

/* /// "nHoldsLock()" */
/**
 * nHoldsLock() - Tell whether a list of locks has one of a handler's.
 * @lock: the first lock: a volume's dol_LockList, or NULL.
 * @port: the handler's port.
 *
 * Return: TRUE if a lock's fl_Task is @port.
 */
static BOOL nHoldsLock(struct FileLock *lock, struct MsgPort *port)
{
    for(; lock; lock = BADDR(lock->fl_Link))
    {
        if(lock->fl_Task == port)
        {
            return TRUE;
        }
    }
    return FALSE;
}
/* \\\ */

/* /// "nHandlerVolume()" */
/**
 * nHandlerVolume() - Tell what a DOS device's handler holds, from the DOS list
 * alone, and name it.
 * @nh: the class base.
 * @node: the DOS device node.
 * @volume: NULL, or the volume the handler held when last asked (NULL for
 *          none), only compared, never read: set to the one it holds now.
 * @name: where the name goes, or NULL: the volume and the device
 *        ("PenDrive (UMSD0:)"), or the bare device name with no volume.
 * @size: size of @name.
 *
 * A handler whose medium is taken away drops a volume with nothing open on it,
 * and keeps one with files or locks open, as for a removed disk: listed, with
 * no task (dol_Task NULL) and the locks parked in its dol_LockList, each
 * lock's fl_Task still the handler's port. The volume goes once its last lock
 * is freed. A handler that finds a parked volume of its own disk takes it over
 * with the locks, as from a floppy moved to another drive: that is how the
 * volume a handler held can turn out another handler's (HOLD_ADOPTED). Locks
 * and Forbid() only, never a packet: PFS3 moves the head of a parked lock list,
 * and frees a volume node, under Forbid() alone, not under the DOS list lock.
 * So the name is read under both.
 *
 * Return: the handler's hold on a volume; HOLD_NONE for a handler with no
 * process.
 */
static enum VolumeHold nHandlerVolume(struct NepMSBase *nh, struct DeviceNode *node,
                                      struct DosList **volume, STRPTR name, ULONG size)
{
    enum VolumeHold hold = HOLD_NONE;
    struct DosList *held = NULL;
    struct DosList *before = volume ? *volume : NULL;
    BOOL adopted = FALSE;
    struct DosList *list = LockDosList(LDF_VOLUMES | LDF_READ);

    Forbid();
    /* without a process, dol_Task NULL would match every parked volume */
    while(node->dn_Task && (hold == HOLD_NONE) &&
          (list = NextDosEntry(list, LDF_VOLUMES | LDF_READ)))
    {
        struct FileLock *locks = BADDR(list->dol_misc.dol_volume.dol_LockList);
        if(list->dol_Task == node->dn_Task)
        {
            hold = HOLD_MOUNTED;
            held = list;
        }
        else if(!list->dol_Task && nHoldsLock(locks, node->dn_Task))
        {
            hold = HOLD_PARKED;
            held = list;
        }
        else if((list == before) && (list->dol_Task || locks))
        {
            /* not this handler's any more, and not dropped: mounted by
               another, or parked with another's locks */
            adopted = TRUE;
        }
    }
    if((hold == HOLD_NONE) && adopted)
    {
        hold = HOLD_ADOPTED;
    }
    if(name)
    {
        /* %b prints a BSTR in place (RawDoFmt(), V36) */
        if(held)
        {
            psdSafeRawDoFmt(name, size, "%b (%b:)", held->dol_Name, node->dn_Name);
        }
        else
        {
            psdSafeRawDoFmt(name, size, "%b:", node->dn_Name);
        }
    }
    Permit();
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    if(volume)
    {
        *volume = held;
    }
    return hold;
}
/* \\\ */

/* /// "nInhibitHandler()" */
/**
 * nInhibitHandler() - Inhibit a DOS device's handler, and tell whether
 * anything on its volume is still in use.
 * @nh: the class base.
 * @node: the DOS device node, out of the DOS list; its handler has a process.
 * @volume: NULL, or as for nHandlerVolume(); set to the volume left parked, if
 *          the inhibit went through.
 * @inhibited: set to whether the handler is inhibited now.
 * @error: set to the handler's refusal; to ERROR_OBJECT_IN_USE if it took the
 *         inhibit but left locks parked; to 0 if it took it with none.
 *
 * A handler inhibited with files or locks open need not refuse: PFS3 takes the
 * volume away as if the disk had been removed and parks the locks on it
 * (nHandlerVolume()), to take them back when the disk returns. Told to die
 * then, it exits with them, and their next use (a Close(), an UnLock()) goes
 * to a port that is gone. So the volume is in use if the inhibit left locks
 * parked, or if the handler refused it for any reason but two: it has no
 * volume (ERROR_DEVICE_NOT_MOUNTED), or it cannot be inhibited
 * (ERROR_ACTION_NOT_KNOWN, some CD filesystems). The caller is a Process:
 * this is a DoPkt().
 *
 * Return: TRUE if the volume is in use.
 */
static BOOL nInhibitHandler(struct NepMSBase *nh, struct DeviceNode *node,
                            struct DosList **volume, BOOL *inhibited, LONG *error)
{
    *inhibited = (DoPkt(node->dn_Task, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0) != DOSFALSE);
    if(*inhibited)
    {
        BOOL parked = (nHandlerVolume(nh, node, volume, NULL, 0) == HOLD_PARKED);
        *error = parked ? ERROR_OBJECT_IN_USE : 0;
        return parked;
    }
    *error = IoErr();
    return (*error != ERROR_DEVICE_NOT_MOUNTED) && (*error != ERROR_ACTION_NOT_KNOWN);
}
/* \\\ */

/* /// "nRemoveDosNode()" */
/**
 * nRemoveDosNode() - Finish the unmount of an unlisted DeviceNode.
 * @ncm: the unit the node rides on.
 * @node: the node, out of the DOS list; its handler told to die, or left idle
 *        for good (nAbandonHandler()).
 *
 * Logs the unmount and unlinks the node's stale BootNode, if any, which the
 * mounter would otherwise count its partition mounted by.
 */
static void nRemoveDosNode(struct NepClassMS *ncm, struct DeviceNode *node)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;

    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Unmounting partition %b...",
                   node->dn_Name);
    /* drop a stale BootNode (pre-DOS boot mounts) so the name stays reusable;
       the node itself is expansion-owned, so just unlink it. ExpansionBase is
       the removable task's - guard for the (theoretical) window after nFreeRT. */
    if(nh->nh_ExpansionBase)
    {
        Forbid();
        struct BootNode *bn = (struct BootNode *) ExpansionBase->MountList.lh_Head;
        while(bn->bn_Node.ln_Succ)
        {
            struct BootNode *succ = (struct BootNode *) bn->bn_Node.ln_Succ;
            if(bn->bn_DeviceNode == node)
            {
                Remove(&bn->bn_Node);
            }
            bn = succ;
        }
        Permit();
    }
}
/* \\\ */

/* /// "nRetireHandler()" */
/**
 * nRetireHandler() - Shut down an unlisted node's handler and finish the node.
 * @ncm: the unit the node rides on.
 * @node: the node, out of the DOS list.
 * @inhibited: whether its handler is inhibited already.
 *
 * Packets fired blind, results ignored: there is nobody left to veto for. A
 * never-accessed handler has no process, and gets no packet.
 */
static void nRetireHandler(struct NepClassMS *ncm, struct DeviceNode *node, BOOL inhibited)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;

    if(node->dn_Task)
    {
        if(!inhibited)
        {
            DoPkt(node->dn_Task, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0);
        }
        DoPkt(node->dn_Task, ACTION_DIE, 0, 0, 0, 0, 0);
    }
    nRemoveDosNode(ncm, node);
}
/* \\\ */

/* /// "nAbandonHandler()" */
/**
 * nAbandonHandler() - Let go of an unlisted node whose volume another handler
 * took over, without retiring its handler.
 * @ncm: the unit the node rides on.
 * @node: the node, out of the DOS list.
 * @inhibited: whether its handler is inhibited already.
 *
 * The medium went into another drive, or was mounted again under another
 * name, and that drive's handler took the parked volume over with its open
 * files (nHandlerVolume()). PFS3 then goes on using the memory the first
 * handler allocated the volume and its locks from, and a handler that dies
 * frees all of that memory. So this one is never told to die: inhibited, it
 * stays idle until the next reboot.
 */
static void nAbandonHandler(struct NepClassMS *ncm, struct DeviceNode *node, BOOL inhibited)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;

    if(!inhibited)
    {
        DoPkt(node->dn_Task, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0);
    }
    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                   "%b: its volume went on in another drive, "
                   "so its filesystem stays idle until the next reboot.",
                   node->dn_Name);
    nRemoveDosNode(ncm, node);
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 4. Unplug
 *----------------------------------------------------------------------------
 */

/* A DOS device a gone unit keeps because something on its volume is still in
   use (nUnmountPartition()): out of the DOS list, its handler inhibited unless
   it refused, until the medium is back (nRemountKeptNodes()). AllocVec()'d,
   one per kept device, in ncm_KeptNodes. */
struct KeptDosNode
{
    struct Node         kdn_Link;
    struct DeviceNode  *kdn_Node;
    struct DosList     *kdn_Volume;     /* the volume its handler holds, as
                                           nHandlerVolume() last found it */
    BOOL                kdn_Inhibited;
    BOOL                kdn_Stranded;   /* could not go back into the DOS list
                                           (nRelistKeptNodes()): stays kept */
};

/* /// "nUnmountPartition()" */
/**
 * nUnmountPartition() - Take a gone unit's DOS devices down, but keep those
 * whose volumes are still in use.
 * @ncm: the unit; its device is gone (ncm_DenyRequests).
 *
 * Runs on every removable-task pass while the unit is gone and has mounts.
 * Each DOS device leaves the DOS list first (nUnlistNextNode()), and its
 * handler is inhibited. One whose volume is still in use (nInhibitHandler())
 * is kept instead of retired: out of the list, so no desktop shows it busy,
 * and inhibited, so its handler never touches the missing medium (PFS3 on a
 * partition that is not floppy-like does not watch for disk changes, and
 * reads at once when let go). It comes back with the medium
 * (nRelistKeptNodes(), nRemountKeptNodes()), and is retired once its handler
 * holds nothing any more (nHandlerVolume()), or let go without being retired
 * once another handler holds its volume (nAbandonHandler()); one that refused
 * the inhibit is asked again on each pass. Every other device is retired at
 * once.
 *
 * Return: TRUE when no DOS device of the unit is left to take down.
 */
BOOL nUnmountPartition(struct NepClassMS *ncm)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;

    if(!nOpenDOS(nh))
    {
        return FALSE;   /* no DOS to talk to yet: the next pass tries again */
    }

    /* the ones kept on an earlier pass: retired once nothing is held, let go
       once their volume is another handler's */
    struct KeptDosNode *kept = (struct KeptDosNode *) ncm->ncm_KeptNodes.lh_Head;
    while(kept->kdn_Link.ln_Succ)
    {
        struct KeptDosNode *next = (struct KeptDosNode *) kept->kdn_Link.ln_Succ;
        enum VolumeHold hold = nHandlerVolume(nh, kept->kdn_Node, &kept->kdn_Volume, NULL, 0);
        BOOL inUse = (hold == HOLD_MOUNTED) || (hold == HOLD_PARKED);
        if(inUse && !kept->kdn_Inhibited)
        {
            LONG error;
            inUse = nInhibitHandler(nh, kept->kdn_Node, &kept->kdn_Volume,
                                    &kept->kdn_Inhibited, &error);
        }
        if(hold == HOLD_ADOPTED)
        {
            Remove(&kept->kdn_Link);
            nAbandonHandler(ncm, kept->kdn_Node, kept->kdn_Inhibited);
            FreeVec(kept);
        }
        else if(!inUse)
        {
            Remove(&kept->kdn_Link);
            nRetireHandler(ncm, kept->kdn_Node, kept->kdn_Inhibited);
            FreeVec(kept);
        }
        kept = next;
    }

    /* the ones still in the DOS list, one at a time: each leaves it, kept or
       retired */
    BOOL done = TRUE;
    ULONG added = 0;
    for(;;)
    {
        /* allocated first: a device that has to be kept cannot be without it,
           so with no memory the rest are left listed, for the next pass */
        struct KeptDosNode *entry = AllocVec(sizeof(struct KeptDosNode), MEMF_PUBLIC|MEMF_CLEAR);
        if(!entry)
        {
            done = FALSE;
            break;
        }
        struct DeviceNode *node = nUnlistNextNode(nh, ncm);
        if(!node)
        {
            FreeVec(entry);
            break;
        }
        struct DosList *volume = NULL;
        BOOL inhibited = FALSE;
        LONG error;
        if(node->dn_Task && nInhibitHandler(nh, node, &volume, &inhibited, &error))
        {
            entry->kdn_Node = node;
            entry->kdn_Volume = volume;
            entry->kdn_Inhibited = inhibited;
            AddTail(&ncm->ncm_KeptNodes, &entry->kdn_Link);
            added++;
        }
        else
        {
            FreeVec(entry);
            nRetireHandler(ncm, node, inhibited);
        }
    }

    if(added)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "'%s' went with %ld volume(s) still in use: they come back with the drive.",
                       ncm->ncm_LUNIDStr, added);
    }
    return done && IsListEmpty(&ncm->ncm_KeptNodes);
}
/* \\\ */

/* /// "nRelistKeptNodes()" */
/**
 * nRelistKeptNodes() - Put the DOS devices a gone unit kept back into the DOS
 * list, its medium being back.
 * @ncm: the unit, ready again.
 *
 * Called before the mount pass, which then counts their volumes mounted
 * already rather than mounting them a second time. Their handlers stay
 * inhibited until nRemountKeptNodes(), after the pass has brought the unit's
 * virtual drives online. A device whose name was taken while the drive was
 * away cannot go back. The mounter, not seeing it, mounts its partition again
 * under another name, and the new handler may take the parked volume over
 * (nHandlerVolume()); the old handler must never run beside it, so it stays
 * kept and inhibited from then on (kdn_Stranded). Once the drive is away
 * again, it is let go or retired like any kept device.
 */
void nRelistKeptNodes(struct NepClassMS *ncm)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;

    struct KeptDosNode *kept = (struct KeptDosNode *) ncm->ncm_KeptNodes.lh_Head;
    for(; kept->kdn_Link.ln_Succ; kept = (struct KeptDosNode *) kept->kdn_Link.ln_Succ)
    {
        if(!kept->kdn_Stranded && !AddDosEntry((struct DosList *) kept->kdn_Node))
        {
            kept->kdn_Stranded = TRUE;
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "%b: cannot come back, its name was taken while the drive was away: "
                           "its partition is mounted again under another name.",
                           kept->kdn_Node->dn_Name);
        }
    }
}
/* \\\ */

/* /// "nRemountKeptNodes()" */
/**
 * nRemountKeptNodes() - Have the kept DOS devices' handlers remount their
 * volumes, after the mount pass.
 * @ncm: the unit.
 *
 * Lifting the inhibit makes each handler look for its volume, find it on the
 * medium that is back and take its parked locks over, the way DiskChange
 * does. If the unit went again during the pass, the devices leave the DOS list
 * once more and stay kept; a stranded one (nRelistKeptNodes()) stays kept
 * either way. The caller is the removable task: these are DoPkt()s.
 */
void nRemountKeptNodes(struct NepClassMS *ncm)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    BOOL gone = ncm->ncm_DenyRequests || !ncm->ncm_UnitReady;
    ULONG remounted = 0;

    struct KeptDosNode *kept = (struct KeptDosNode *) ncm->ncm_KeptNodes.lh_Head;
    while(kept->kdn_Link.ln_Succ)
    {
        struct KeptDosNode *next = (struct KeptDosNode *) kept->kdn_Link.ln_Succ;
        if(!kept->kdn_Stranded)
        {
            if(gone)
            {
                nUnlistDosNode(nh, kept->kdn_Node);
            }
            else
            {
                Remove(&kept->kdn_Link);
                if(kept->kdn_Inhibited)
                {
                    DoPkt(kept->kdn_Node->dn_Task, ACTION_INHIBIT, DOSFALSE, 0, 0, 0, 0);
                }
                FreeVec(kept);
                remounted++;
            }
        }
        kept = next;
    }
    if(remounted)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "'%s' is back with %ld volume(s) kept in use.",
                       ncm->ncm_LUNIDStr, remounted);
    }
}
/* \\\ */

/*
 *----------------------------------------------------------------------------
 * 5. Safe eject (UCM_SafeEject)
 *----------------------------------------------------------------------------
 *
 * The unit the caller happened to name is only a handle on its device:
 * everything below works on all of that device's units, because one USB
 * device is what the user unplugs.
 */

/* A DOS device an eject takes down: one entry of the array nUnlistEjectNodes()
   allocates */
struct SafeEjectNode
{
    struct DeviceNode *sen_Node;
    struct NepClassMS *sen_LUN;
    BOOL               sen_Inhibited;
};

/* /// "nNextLUN()" */
/**
 * nNextLUN() - Step through the units of one device.
 * @nh: the class base.
 * @pd: the device.
 * @lun: the previous unit, or NULL to start.
 *
 * Return: the device's next unit, or NULL at the end.
 */
static struct NepClassMS * nNextLUN(struct NepMSBase *nh, struct PsdDevice *pd,
                                    struct NepClassMS *lun)
{
    lun = lun ? (struct NepClassMS *) lun->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ
              : (struct NepClassMS *) nh->nh_Units.lh_Head;
    while(lun->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ)
    {
        if(lun->ncm_Device == pd)
        {
            return lun;
        }
        lun = (struct NepClassMS *) lun->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ;
    }
    return NULL;
}
/* \\\ */

/* /// "nSetEjectLatch()" */
/**
 * nSetEjectLatch() - Arm or disarm the remount block on all of a device's
 * units at once.
 * @nh: the class base.
 * @pd: the device.
 * @on: TRUE to arm.
 *
 * While armed, the removable task's tick cannot mount behind an eject in
 * progress.
 */
static void nSetEjectLatch(struct NepMSBase *nh, struct PsdDevice *pd, BOOL on)
{
    Forbid();
    for(struct NepClassMS *lun = nNextLUN(nh, pd, NULL); lun; lun = nNextLUN(nh, pd, lun))
    {
        lun->ncm_Ejected = on;
    }
    Permit();
}
/* \\\ */

/* /// "nLUNForDosEntry()" */
/**
 * nLUNForDosEntry() - Find which of a device's units a DOS device rides on.
 * @nh: the class base.
 * @pd: the device.
 * @list: the DOS device entry; the caller holds a DOS device list lock.
 *
 * Return: the unit, or NULL if @list rides on none of the device's.
 */
static struct NepClassMS * nLUNForDosEntry(struct NepMSBase *nh, struct PsdDevice *pd,
                                           struct DosList *list)
{
    for(struct NepClassMS *lun = nNextLUN(nh, pd, NULL); lun; lun = nNextLUN(nh, pd, lun))
    {
        if(nDosEntryMatches(lun, list))
        {
            return lun;
        }
    }
    return NULL;
}
/* \\\ */

/* /// "nUnlistEjectNodes()" */
/**
 * nUnlistEjectNodes() - Take every DOS device node of the device out of the
 * DOS list.
 * @nh: the class base.
 * @pd: the device.
 * @nodes: where the array of the nodes and their units goes, AllocVec()'d
 *         for exactly as many: the caller FreeVec()s it. NULL with none.
 * @count: where their number goes.
 *
 * Counts the nodes riding one of the device's units, allocates the array, and
 * collects and removes them, all under one write lock and without sending a
 * packet while holding it (an inhibit may need the list). They leave the list
 * before the inhibit for the desktop's sake (nUnlistDosNode());
 * nRelistEjectNodes() puts them back on a veto.
 *
 * Return: FALSE, with none removed, if the array could not be allocated.
 */
static BOOL nUnlistEjectNodes(struct NepMSBase *nh, struct PsdDevice *pd,
                              struct SafeEjectNode **nodes, ULONG *count)
{
    struct SafeEjectNode *array = NULL;
    ULONG found = 0;
    struct DosList *head = LockDosList(LDF_DEVICES | LDF_WRITE);

    for(struct DosList *list = head; (list = NextDosEntry(list, LDF_DEVICES | LDF_WRITE)); )
    {
        if(nLUNForDosEntry(nh, pd, list))
        {
            found++;
        }
    }
    if(found && !(array = AllocVec(found * sizeof(struct SafeEjectNode), MEMF_PUBLIC|MEMF_CLEAR)))
    {
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Eject: no memory for the list of %ld partitions.", found);
        return FALSE;
    }
    ULONG filled = 0;
    for(struct DosList *list = head;
        (filled < found) && (list = NextDosEntry(list, LDF_DEVICES | LDF_WRITE)); )
    {
        struct NepClassMS *lun = nLUNForDosEntry(nh, pd, list);
        if(lun)
        {
            array[filled].sen_Node = (struct DeviceNode *) list;
            array[filled].sen_LUN = lun;
            filled++;
        }
    }
    /* removed after the walk: NextDosEntry() follows the current entry */
    for(ULONG idx = 0; idx < filled; idx++)
    {
        RemDosEntry((struct DosList *) array[idx].sen_Node);
    }
    UnLockDosList(LDF_DEVICES | LDF_WRITE);
    *nodes = array;
    *count = filled;
    return TRUE;
}
/* \\\ */

/* /// "nRelistEjectNodes()" */
/**
 * nRelistEjectNodes() - Put the nodes nUnlistEjectNodes() took back into the
 * DOS list, after a veto.
 * @nh: the class base.
 * @nodes: the nodes.
 * @count: how many.
 */
static void nRelistEjectNodes(struct NepMSBase *nh, struct SafeEjectNode *nodes, ULONG count)
{
    /* last first: AddDosEntry() puts each at the head of the list, so they
       come back in the order they were found */
    for(ULONG idx = count; idx-- > 0; )
    {
        struct DeviceNode *node = nodes[idx].sen_Node;
        if(!AddDosEntry((struct DosList *) node))
        {
            /* the name was taken meanwhile: the handler runs on, its volume
               is still reachable by name */
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Eject: could not put %b: back into the DOS list.",
                           node->dn_Name);
        }
    }
}
/* \\\ */

/* /// "nInhibitAll()" */
/**
 * nInhibitAll() - Flush and inhibit every unlisted node's handler, stopping
 * at the first whose volume is in use.
 * @nh: the class base.
 * @nodes: the nodes, out of the DOS list (nUnlistEjectNodes()).
 * @count: how many.
 * @busybuf: where the name of the volume in use goes, or NULL.
 * @busybuflen: size of @busybuf.
 *
 * In use is what nInhibitHandler() says. The handlers inhibited by then are
 * left for nUninhibitAll(), once the nodes are back in the DOS list. The
 * caller is a Process: these are DoPkt()s.
 *
 * Return: SAFEEJECT_OK, or SAFEEJECT_BUSY with @busybuf naming the volume.
 */
static IPTR nInhibitAll(struct NepMSBase *nh, struct SafeEjectNode *nodes, ULONG count,
                        STRPTR busybuf, ULONG busybuflen)
{
    for(ULONG idx = 0; idx < count; idx++)
    {
        struct DeviceNode *node = nodes[idx].sen_Node;
        if(!node->dn_Task)   /* never-accessed handler: no process, nothing to flush */
        {
            continue;
        }
        /* named before the inhibit, which takes the volume away */
        UBYTE busyname[BUSYNAME_LENGTH];
        nHandlerVolume(nh, node, NULL, busyname, sizeof(busyname));
        DoPkt(node->dn_Task, ACTION_FLUSH, 0, 0, 0, 0, 0);
        LONG error;
        if(!nInhibitHandler(nh, node, NULL, &nodes[idx].sen_Inhibited, &error))
        {
            if(error == ERROR_ACTION_NOT_KNOWN)
            {
                /* the flush above already ran; a veto would make such volumes
                   impossible to eject */
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Eject: a filesystem does not support Inhibit(), "
                               "proceeding after flush.");
            }
            continue;
        }
        if(busybuf)
        {
            psdSafeRawDoFmt(busybuf, busybuflen, "%s", busyname);
        }
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Eject vetoed: %s is still in use (error %ld).",
                       busyname, error);
        return SAFEEJECT_BUSY;
    }
    return SAFEEJECT_OK;
}
/* \\\ */

/* /// "nUninhibitAll()" */
/**
 * nUninhibitAll() - Lift the inhibits nInhibitAll() set, after a veto.
 * @nh: the class base.
 * @nodes: the nodes.
 * @count: how many.
 *
 * The caller must be a Process: these are DoPkt()s.
 */
static void nUninhibitAll(struct NepMSBase *nh, struct SafeEjectNode *nodes, ULONG count)
{
    for(ULONG idx = 0; idx < count; idx++)
    {
        if(nodes[idx].sen_Inhibited)
        {
            DoPkt(nodes[idx].sen_Node->dn_Task, ACTION_INHIBIT, DOSFALSE, 0, 0, 0, 0);
        }
    }
}
/* \\\ */

/* /// "nQuiesceLUN()" */
/**
 * nQuiesceLUN() - Leave one unit ready to be pulled: cache synced, removal
 * allowed, spun down.
 * @lun: the unit.
 * @ioreq: an I/O request of the caller's own.
 *
 * Best effort throughout: the data is safe already.
 */
static void nQuiesceLUN(struct NepClassMS *lun, struct IOStdReq *ioreq)
{
    if(OpenDevice(DEVNAME, lun->ncm_UnitNo, (struct IORequest *) ioreq, 0))
    {
        return;
    }
    if(!(lun->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI))
    {
        struct SCSICmd scsicmd;
        UBYTE cmd10[10] = { SCSI_DA_SYNCHRONIZE_CACHE, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        UBYTE sensedata[18];
        scsicmd.scsi_Data = NULL;
        scsicmd.scsi_Length = 0;
        scsicmd.scsi_Command = cmd10;
        scsicmd.scsi_CmdLength = 10;
        scsicmd.scsi_Flags = SCSIF_AUTOSENSE|0x80;
        scsicmd.scsi_SenseData = sensedata;
        scsicmd.scsi_SenseLength = 18;
        ioreq->io_Command = HD_SCSICMD;
        ioreq->io_Data = &scsicmd;
        ioreq->io_Length = sizeof(scsicmd);
        DoIO((struct IORequest *) ioreq);   /* many sticks reject 0x35 - ignore */

        UBYTE cmd6[6] = { SCSI_DA_PREVENT_ALLOW_MEDIUM_REMOVAL, 0, 0, 0, 0, 0 };
        scsicmd.scsi_Command = cmd6;        /* byte 4 = 0 -> allow removal */
        scsicmd.scsi_CmdLength = 6;
        ioreq->io_Command = HD_SCSICMD;
        DoIO((struct IORequest *) ioreq);
    }
    /* spindown rides the unit task's START/STOP arm: gets the UAS drain
       barrier and nStartStop's own SIMPLE_SCSI gate */
    ioreq->io_Command = CMD_STOP;
    ioreq->io_Data = NULL;
    ioreq->io_Length = 0;
    DoIO((struct IORequest *) ioreq);
    CloseDevice((struct IORequest *) ioreq);
}
/* \\\ */

/* /// "nQuiesceDevice()" */
/**
 * nQuiesceDevice() - Quiesce all of a device's units (nQuiesceLUN()).
 * @nh: the class base.
 * @pd: the device.
 *
 * Over an I/O channel of our own: nh_IOReq and nIOCmdTunnel() belong to the
 * removable task's tick and must not be raced (AutoDetectMaxTransfer() does
 * I/O from a foreign task the same way).
 */
static void nQuiesceDevice(struct NepMSBase *nh, struct PsdDevice *pd)
{
    struct MsgPort *mp = CreateMsgPort();

    if(mp)
    {
        struct IOStdReq *ioreq = (struct IOStdReq *) CreateIORequest(mp, sizeof(struct IOStdReq));
        if(ioreq)
        {
            for(struct NepClassMS *lun = nNextLUN(nh, pd, NULL); lun; lun = nNextLUN(nh, pd, lun))
            {
                nQuiesceLUN(lun, ioreq);
            }
            DeleteIORequest((struct IORequest *) ioreq);
        }
        DeleteMsgPort(mp);
    }
}
/* \\\ */

/* /// "nSafeEjectDevice()" */
/**
 * nSafeEjectDevice() - Unmount every volume on all of a device's units, all
 * or none, then sync and spin the drive down (UCM_SafeEject).
 * @nh: the class base.
 * @ncm: one of the device's units.
 * @busybuf: where the name of a volume still in use goes, or NULL.
 * @busybuflen: size of @busybuf.
 *
 * Each handler is flushed and inhibited; one volume in use rolls everything
 * back. Runs on the caller's task, which must be a Process (DoPkt()). Taking
 * the device off the bus afterwards is psdSafeEjectDevice()'s half of the job.
 *
 * Return: SAFEEJECT_OK, SAFEEJECT_BUSY with @busybuf naming the volume, or
 * SAFEEJECT_FAIL.
 */
IPTR nSafeEjectDevice(struct NepMSBase *nh, struct NepClassMS *ncm,
                      STRPTR busybuf, ULONG busybuflen)
{
    struct PsdDevice *pd = ncm->ncm_Device;

    if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Safe eject needs a Process context.");
        return SAFEEJECT_FAIL;
    }
    if(ncm->ncm_Ejected)
    {
        return SAFEEJECT_OK;   /* idempotent: all of a device's units latch together */
    }
    if(!nOpenDOS(nh))
    {
        return SAFEEJECT_FAIL;
    }

    /* Latch first: the removable task's 3s tick must not (re)mount anything
       between the unlist below and the port disable at the end. Rolled back
       on any failure, after the nodes are back in the DOS list. */
    nSetEjectLatch(nh, pd, TRUE);

    struct SafeEjectNode *nodes;
    ULONG count;
    if(!nUnlistEjectNodes(nh, pd, &nodes, &count))
    {
        nSetEjectLatch(nh, pd, FALSE);
        return SAFEEJECT_FAIL;
    }
    IPTR result = nInhibitAll(nh, nodes, count, busybuf, busybuflen);
    if(result != SAFEEJECT_OK)
    {
        /* undone in reverse: no volume comes back without its device */
        nRelistEjectNodes(nh, nodes, count);
        nUninhibitAll(nh, nodes, count);
        FreeVec(nodes);
        nSetEjectLatch(nh, pd, FALSE);
        return result;
    }

    /* Point of no return: the data is safe, so shut the handlers down and
       finish the nodes, results ignored from here on. */
    for(ULONG idx = 0; idx < count; idx++)
    {
        nRetireHandler(nodes[idx].sen_LUN, nodes[idx].sen_Node, nodes[idx].sen_Inhibited);
        nodes[idx].sen_LUN->ncm_HasMounted = FALSE;
    }
    FreeVec(nodes);

    nQuiesceDevice(nh, pd);

    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "'%s' safely ejected.", ncm->ncm_LUNIDStr);
    /* taking the device off the bus is psdSafeEjectDevice()'s job */
    return SAFEEJECT_OK;
}
/* \\\ */
