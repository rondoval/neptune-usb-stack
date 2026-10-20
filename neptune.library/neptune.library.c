/****************************************************************************
 * Neptune - an incompatible fork of the Poseidon USB stack.
 *
 * Poseidon: designed and written by
 *   Chris Hodges <chrisly@platon42.de>
 * Copyright (c) 2009-2026 The AROS Dev Team.
 * Copyright (c) 2002-2009 Chris Hodges.
 ****************************************************************************/

/*
 *----------------------------------------------------------------------------
 *                          Neptune main library
 *----------------------------------------------------------------------------
 * Based on the Poseidon main library by Chris Hodges <chrisly@platon42.de>.
 */

#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)

#include "debug.h"

#include "neptune.library.h"

#include "numtostr.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/usbclass.h>
#include <proto/timer.h>

#include <string.h>

/* amiga.lib veneer (jumps to the device's BEGINIO vector); declared in
 * <clib/alib_protos.h>, which we don't pull in -- exec_protos.h has DoIO/SendIO
 * but not BeginIO.  Used by pSubmitPipe() for traditional IOF_QUICK quick-I/O. */
extern VOID BeginIO(struct IORequest *ioReq);

#define NewList(list) NEWLIST(list)

#define MOD_NAME_STRING NEPTUNENAME

#define min(x,y) (((x) < (y)) ? (x) : (y))
#define max(x,y) (((x) > (y)) ? (x) : (y))

extern const struct PsdWStringMap usbclasscodestr[];
extern const struct PsdULStringMap usbcomboclasscodestr[];
extern const struct PsdULStringMap usbdesctypestr[];
extern const struct PsdWStringMap usbhwioerrstr[];
extern const struct PsdUWStringMap usblangids[];
extern const struct PsdUWStringMap usbvendorids[];

/* Static data */
const char libname[]     = MOD_NAME_STRING;

#define UsbClsBase puc->puc_ClassBase
#define DOSBase ps->ps_DosBase
#define TimerBase ps->ps_TimerIOReq.tr_node.io_Device

/* LibInit */
int libInit(struct PsdBase * ps)
{
    KPRINTF(10, ("libInit ps: 0x%08lx SysBase: 0x%08lx\n",
                 ps, EXEC_BASE_NAME));

    ps->ps_StackInit = FALSE;
    ps->ps_UtilityBase = (struct UtilityBase *) OpenLibrary("utility.library", 39);

#define UtilityBase ps->ps_UtilityBase

    if (UtilityBase) {
        NewList(&ps->ps_Hardware);
        NewList(&ps->ps_Classes);
        NewList(&ps->ps_ErrorMsgs);
        NewList(&ps->ps_EventHooks);
        memset(&ps->ps_EventReplyPort, 0, sizeof(ps->ps_EventReplyPort));
        ps->ps_EventReplyPort.mp_Flags = PA_IGNORE;
        NewList(&ps->ps_EventReplyPort.mp_MsgList);
        NewList(&ps->ps_ConfigRoot);
        NewList(&ps->ps_AlienConfigs);

        NewList(&ps->ps_DeadlockDebug);

        InitSemaphore(&ps->ps_ReentrantLock);
        InitSemaphore(&ps->ps_NotifyLock);
        InitSemaphore(&ps->ps_StallRecoverySem);
        InitSemaphore(&ps->ps_MemPoolSem);

        if((ps->ps_MemPool = CreatePool(MEMF_CLEAR|MEMF_PUBLIC, 16384, 1024))) {
            if((ps->ps_SemaMemPool = CreatePool(MEMF_CLEAR|MEMF_PUBLIC, 16*sizeof(struct PsdReadLock), sizeof(struct PsdBorrowLock)))) {
                pInitSem(ps, &ps->ps_Lock, "PBase");
                pInitSem(ps, &ps->ps_ConfigLock, "ConfigLock");
                KPRINTF(20, ("libInit: Done!\n"));
                return TRUE;
            }
            DeletePool(ps->ps_MemPool);
        } else {
            KPRINTF(20, ("libInit: CreatePool() failed!\n"));
        }
        CloseLibrary((struct Library *) UtilityBase);
    } else {
        KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
    }
    return FALSE;
}

/* LibOpen */
int libOpen(struct PsdBase * ps)
{
    struct PsdIFFContext *pic;

    KPRINTF(5, ("libOpen ps: 0x%08lx\n", ps));
    ObtainSemaphore(&ps->ps_ReentrantLock);
    if(!ps->ps_StackInit) {
        ps->ps_TimerIOReq.tr_node.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        ps->ps_TimerIOReq.tr_node.io_Message.mn_ReplyPort    = NULL;
        ps->ps_TimerIOReq.tr_node.io_Message.mn_Length       = sizeof(struct timerequest);
        if(!OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *) &ps->ps_TimerIOReq, 0)) {
            ps->ps_TimerIOReq.tr_node.io_Message.mn_Node.ln_Name = "Neptune";
            ps->ps_TimerIOReq.tr_node.io_Command = TR_ADDREQUEST;

            ps->ps_ReleaseVersion = RELEASEVERSION;
            ps->ps_OSVersion = MAKE_ID('A','R','O','S');

            pic = pAllocForm(ps, NULL, IFFFORM_PSDCFG);
            if((ps->ps_GlobalCfg = psdAllocVec(sizeof(struct PsdGlobalCfg)))) {
                ps->ps_GlobalCfg->pgc_ChunkID = AROS_LONG2BE(IFFCHNK_GLOBALCFG);
                ps->ps_GlobalCfg->pgc_Length = AROS_LONG2BE(sizeof(struct PsdGlobalCfg)-8);
                ps->ps_GlobalCfg->pgc_LogInfo = TRUE;
                ps->ps_GlobalCfg->pgc_LogWarning = TRUE;
                ps->ps_GlobalCfg->pgc_LogError = TRUE;
                ps->ps_GlobalCfg->pgc_LogFailure = TRUE;
                ps->ps_GlobalCfg->pgc_BootDelay = 2;
                ps->ps_GlobalCfg->pgc_SubTaskPri = 5;
                ps->ps_GlobalCfg->pgc_PopupDeviceNew = PGCP_ISNEW;
                ps->ps_GlobalCfg->pgc_PopupDeviceGone = TRUE;
                ps->ps_GlobalCfg->pgc_PopupDeviceDeath = TRUE;
                ps->ps_GlobalCfg->pgc_PopupCloseDelay = 5;
                ps->ps_GlobalCfg->pgc_PopupActivateWin = FALSE;
                ps->ps_GlobalCfg->pgc_PopupWinToFront = TRUE;
                ps->ps_GlobalCfg->pgc_AutoDisableLP = FALSE;
                ps->ps_GlobalCfg->pgc_AutoDisableDead = FALSE;
                ps->ps_GlobalCfg->pgc_AutoRestartDead = TRUE;
                ps->ps_GlobalCfg->pgc_PowerSaving = FALSE;
                ps->ps_GlobalCfg->pgc_ForceSuspend = FALSE;
                ps->ps_GlobalCfg->pgc_SuspendTimeout = 30;
                /* also the value every prefs file written before this field
                   existed inherits, so link power keeps working as it did */
                ps->ps_GlobalCfg->pgc_LinkPowerMgmt = TRUE;

                ps->ps_GlobalCfg->pgc_PrefsVersion = 0; // is updated on writing
                ps->ps_ConfigRead = FALSE;
                if(pic) {
                    pic = pAllocForm(ps, pic, IFFFORM_STACKCFG);
                    if(pic) {
                        pAddCfgChunk(ps, pic, ps->ps_GlobalCfg);
                    }
                }
                ps->ps_Notify.nt_InsertSndFile = psdCopyStr(NEP_PRESETDRAWER "/Connect.iff");
                ps->ps_Notify.nt_RemoveSndFile = psdCopyStr(NEP_PRESETDRAWER "/Disconnect.iff");

                /* VERSION_STRING is the $VER cookie ("$VER: neptune.library <ver> (date) ...");
                 * skip the 6-char "$VER: " tag for the welcome banner. */
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Started %s (0x%08lx).",
                               (STRPTR) VERSION_STRING + 6, ps->ps_ReleaseVersion);

                KPRINTF(10, ("libOpen: Ok\n"));
                ps->ps_StackInit = TRUE;
                ReleaseSemaphore(&ps->ps_ReentrantLock);
                pStartEventHandler(ps);

                return TRUE;
            } else {
                KPRINTF(20, ("libOpen: No memory for cfg!\n"));
            }
        } else {
            KPRINTF(20, ("libOpen: OpenDevice(timer.device) failed!\n"));
        }
        ReleaseSemaphore(&ps->ps_ReentrantLock);
        return FALSE;
    }
    ReleaseSemaphore(&ps->ps_ReentrantLock);
    KPRINTF(5, ("libOpen: openCnt = %ld\n", ps->ps_Library.lib_OpenCnt));
    return TRUE;
}

int libExpunge(struct PsdBase * ps)
{
    struct PsdHardware *phw = (struct PsdHardware *) ps->ps_Hardware.lh_Head;
    struct PsdUsbClass *puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
    struct PsdErrorMsg *pem = (struct PsdErrorMsg *) ps->ps_ErrorMsgs.lh_Head;
    struct PsdIFFContext *pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
    KPRINTF(10, ("libExpunge ps: 0x%08lx\n", ps));
    while(phw->phw_Node.ln_Succ) {
        psdRemHardware(phw);
        phw = (struct PsdHardware *) ps->ps_Hardware.lh_Head;
    }
    while(puc->puc_Node.ln_Succ) {
        psdRemClass(puc);
        puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
    }
    while(pem->pem_Node.ln_Succ) {
        psdRemErrorMsg(pem);
        pem = (struct PsdErrorMsg *) ps->ps_ErrorMsgs.lh_Head;
    }

    while(pic->pic_Node.ln_Succ) {
        pFreeForm(ps, pic);
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
    }

    if(ps->ps_Notify.nt_Task) {
        ps->ps_Notify.nt_ReadySignal = SIGB_SINGLE;
        ps->ps_Notify.nt_ReadySigTask = FindTask(NULL);
        Signal(ps->ps_Notify.nt_Task, SIGBREAKF_CTRL_C);
        while(ps->ps_Notify.nt_Task) {
            Wait(1L<<ps->ps_Notify.nt_ReadySignal);
        }
        ps->ps_Notify.nt_ReadySigTask = NULL;
        //FreeSignal(ps->ps_Notify.nt_ReadySignal);
    }
    if(ps->ps_EventHandler.ph_Task) {
        ps->ps_EventHandler.ph_ReadySignal = SIGB_SINGLE;
        ps->ps_EventHandler.ph_ReadySigTask = FindTask(NULL);
        Signal(ps->ps_EventHandler.ph_Task, SIGBREAKF_CTRL_C);
        while(ps->ps_EventHandler.ph_Task) {
            Wait(1L<<ps->ps_EventHandler.ph_ReadySignal);
        }
        ps->ps_EventHandler.ph_ReadySigTask = NULL;
        //FreeSignal(ps->ps_EventHandler.ph_ReadySignal);
    }
    psdFreeVec(ps->ps_Notify.nt_InsertSndFile);
    psdFreeVec(ps->ps_Notify.nt_RemoveSndFile);
    pGarbageCollectEvents(ps);

    CloseDevice((struct IORequest *) &ps->ps_TimerIOReq);
    DeletePool(ps->ps_SemaMemPool);
    DeletePool(ps->ps_MemPool);

    KPRINTF(1, ("libExpunge: closelibrary utilitybase 0x%08lx\n",
                UtilityBase));
    CloseLibrary((struct Library *) UtilityBase);

    CloseLibrary(DOSBase);

    KPRINTF(1, ("libExpunge: removing library node 0x%08lx\n",
                &ps->ps_Library.lib_Node));
    Remove(&ps->ps_Library.lib_Node);

    return TRUE;
}
/* \\\ */


/*
 * ***********************************************************************
 * * Library functions                                                   *
 * ***********************************************************************
 */

static const ULONG * const PsdPTArray[PGA_LAST+1];

/* *** Memory *** */

struct psdMemHeader
{
    APTR  pmem_raw;  /* Pointer returned by AllocPooled() */
    ULONG size;      /* Size requested by caller */
};

/* Debug builds guard every block: a pattern follows it and is checked when
   the block is freed, and a block that is freed twice is refused. Both are
   reported with the block's size and the task that frees it. */
#ifdef DEBUG
#define NEP_MEM_TAIL        16
#define NEP_MEM_TAILBYTE(n) ((UBYTE) (0xa5 ^ (n)))
#else
#define NEP_MEM_TAIL        0
#endif
/* /// "psdAllocVec()" */
APTR (psdAllocVec)(ULONG size asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct psdMemHeader *hdr;
    APTR raw;
    ULONG alloc_size;
    UBYTE *p;
    UBYTE *aligned;
    IPTR mask;
    APTR result;

    KPRINTF(1, ("psdAllocVec(%ld)\n", size));

    /* Space = requested size + header + alignment slop + the debug tail */
    alloc_size = size + sizeof(struct psdMemHeader) + (AROS_WORSTALIGN - 1) + NEP_MEM_TAIL;

    ObtainSemaphore(&ps->ps_MemPoolSem);
    raw = AllocPooled(ps->ps_MemPool, alloc_size);
    if (raw)
    {
        ps->ps_MemAllocated += size;
    }
    ReleaseSemaphore(&ps->ps_MemPoolSem);
    if (raw)
    {
        /* Start alignment after the header */
        p    = (UBYTE *)raw + sizeof(struct psdMemHeader);
        mask = (IPTR)AROS_WORSTALIGN - 1;

        aligned = (UBYTE *)(((IPTR)p + mask) & ~mask);
        hdr     = (struct psdMemHeader *)(aligned - sizeof(struct psdMemHeader));

        hdr->pmem_raw = raw;
        hdr->size     = size;

#ifdef DEBUG
        for (ULONG cnt = 0; cnt < NEP_MEM_TAIL; cnt++)
        {
            aligned[size + cnt] = NEP_MEM_TAILBYTE(cnt);
        }
#endif

        result = (APTR)aligned;
        return result;
    }

    return NULL;
}
/* \\\ */

/* /// "psdFreeVec()" */
void (psdFreeVec)(APTR pmem asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct psdMemHeader *hdr;
    ULONG size;
    ULONG alloc_size;

    KPRINTF(1, ("psdFreeVec(0x%08lx)\n", pmem));

    if (pmem)
    {
        /* Header is located immediately before the returned aligned pointer */
        hdr  = ((struct psdMemHeader *)pmem) - 1;
        size = hdr->size;
        APTR raw = hdr->pmem_raw;

#ifdef DEBUG
        STRPTR taskname = FindTask(NULL)->tc_Node.ln_Name;

        if (!raw)
        {
            KPRINTF(200, ("memory: 0x%08lx (%ld bytes) is freed twice, by %s\n",
                          pmem, size, taskname ? taskname : (STRPTR) "?"));
            return;
        }
        for (ULONG cnt = 0; cnt < NEP_MEM_TAIL; cnt++)
        {
            if (((UBYTE *)pmem)[size + cnt] != NEP_MEM_TAILBYTE(cnt))
            {
                KPRINTF(200, ("memory: 0x%08lx (%ld bytes) was written past its end (byte +%ld), freed by %s\n",
                              pmem, size, cnt, taskname ? taskname : (STRPTR) "?"));
                break;
            }
        }
        hdr->pmem_raw = NULL;
#endif

        alloc_size = size + sizeof(struct psdMemHeader) + (AROS_WORSTALIGN - 1) + NEP_MEM_TAIL;

        /* Free using the original pointer from AllocPooled() and the original size */
        ObtainSemaphore(&ps->ps_MemPoolSem);
        ps->ps_MemAllocated -= size;
        FreePooled(ps->ps_MemPool, raw, alloc_size);
        ReleaseSemaphore(&ps->ps_MemPoolSem);
    }

}
/* \\\ */

/* *** PBase *** */

/* /// "pDebugSemaInfo()" */
void pDebugSemaInfo(struct PsdBase * ps, struct PsdSemaInfo *psi)
{
    struct PsdReadLock *prl;
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Semaphore 0x%08lx %s (Excl/SharedLockCount %ld/%ld) (Owner: %s):",
                   psi->psi_LockSem,
                   psi->psi_LockSem->pls_Node.ln_Name,
                   psi->psi_LockSem->pls_ExclLockCount,
                   psi->psi_LockSem->pls_SharedLockCount,
                   psi->psi_LockSem->pls_Owner ? (STRPTR)psi->psi_LockSem->pls_Owner->tc_Node.ln_Name : (STRPTR)"None");

    prl = (struct PsdReadLock *) psi->psi_LockSem->pls_WaitQueue.lh_Head;
    while(prl->prl_Node.ln_Succ) {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "  Waiting Task: 0x%08lx (%s) %s",
                       prl->prl_Task, prl->prl_Task->tc_Node.ln_Name,
                       prl->prl_IsExcl ? "Excl" : "Shared");
        prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ;
    }
    prl = (struct PsdReadLock *) psi->psi_LockSem->pls_ReadLocks.lh_Head;
    while(prl->prl_Node.ln_Succ) {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "  Readlock Task: 0x%08lx (%s), Count %ld",
                       prl->prl_Task, prl->prl_Task->tc_Node.ln_Name,
                       prl->prl_Count);
        prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ;
    }
}
/* \\\ */

/* /// "pInitSem()" */
void pInitSem(struct PsdBase * ps, struct PsdLockSem *pls, STRPTR name)
{
    struct PsdSemaInfo *psi = NULL;
    NewList(&pls->pls_WaitQueue);
    NewList(&pls->pls_ReadLocks);
    pls->pls_Node.ln_Name = name;
    // struct should be nulled anyway
    pls->pls_Owner = NULL;
    pls->pls_ExclLockCount = 0;
    pls->pls_SharedLockCount = 0;
    pls->pls_Dead = FALSE;

    Forbid();
    psi = (struct PsdSemaInfo *) AllocPooled(ps->ps_SemaMemPool, sizeof(struct PsdSemaInfo));
    if(!psi) {
        Permit();
        return;
    }
    psi->psi_LockSem = pls;
    AddTail(&ps->ps_DeadlockDebug, &psi->psi_Node);
    Permit();
}
/* \\\ */

/* /// "pDeleteSem()" */
void pDeleteSem(struct PsdBase * ps, struct PsdLockSem *pls)
{
    struct PsdSemaInfo *psi;
    Forbid();
    pls->pls_Dead = TRUE;
    psi = (struct PsdSemaInfo *) ps->ps_DeadlockDebug.lh_Head;
    while(psi->psi_Node.ln_Succ) {
        if(psi->psi_LockSem == pls) {
            if(pls->pls_SharedLockCount + pls->pls_ExclLockCount) {
                psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname, "Semaphore still locked when attempting to delete it!\n");
                pDebugSemaInfo(ps, psi);
            } else {
                Remove(&psi->psi_Node);
                FreePooled(ps->ps_SemaMemPool, psi, sizeof(struct PsdSemaInfo));
            }
            break;
        }
        psi = (struct PsdSemaInfo *) psi->psi_Node.ln_Succ;
    }
    Permit();
}
/* \\\ */

/* /// "pLockSemExcl()" */
void pLockSemExcl(struct PsdBase * ps, struct PsdLockSem *pls)
{
    struct PsdReadLock waitprl;
    struct Task *thistask = FindTask(NULL);

    waitprl.prl_Task = thistask;
    waitprl.prl_IsExcl = TRUE;

    Forbid();
    do {
        // it's already mine!!
        if(thistask == pls->pls_Owner) {
            break;
        }
        if(!pls->pls_ExclLockCount) {
            // easy case: no shared locks, no exclusive locker
            if(!pls->pls_SharedLockCount) {
                break;
            }
            // sole readlock promotion case
            if((pls->pls_SharedLockCount == 1) && ((struct PsdReadLock *) pls->pls_ReadLocks.lh_Head)->prl_Task == thistask) {
                KPRINTF(1, ("Promoting read lock (0x%08lx) to write lock!\n", thistask));
                break;
            }
        }

        // okay, bad luck, we've got to wait somehow
        AddHead(&pls->pls_WaitQueue, &waitprl.prl_Node);
        thistask->tc_SigRecvd &= ~SIGF_SINGLE;

        Wait(SIGF_SINGLE);

        Remove(&waitprl.prl_Node);
    } while(TRUE);
    pls->pls_Owner = thistask;
    pls->pls_ExclLockCount++;
    Permit();
}
/* \\\ */

/* /// "pLockSemShared()" */
void pLockSemShared(struct PsdBase * ps, struct PsdLockSem *pls)
{
    struct PsdReadLock *prl;
    struct Task *thistask = FindTask(NULL);

    Forbid();
    // is this already locked exclusively by me?
    if(thistask == pls->pls_Owner) {
        // yes? then just increase exclusive lock count
        pls->pls_ExclLockCount++;
        Permit();
        return;
    }

    // find existing readlock
    prl = (struct PsdReadLock *) pls->pls_ReadLocks.lh_Head;
    while(prl->prl_Node.ln_Succ) {
        if(prl->prl_Task == thistask) {
            KPRINTF(1, ("Increasing ReadLock (0x%08lx) count to %ld\n", thistask, prl->prl_Count));
            prl->prl_Count++;
            Permit();
            return;
        }
        prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ;
    }

    // this is a new readlock, generate context
    if(!(prl = (struct PsdReadLock *) AllocPooled(ps->ps_SemaMemPool, sizeof(struct PsdReadLock)))) {
        KPRINTF(20, ("No mem for shared lock! context (0x%08lx) on 0x%08lx\n", thistask, pls));
        // try exclusive lock as fallback (needs no memory)
        Permit();
        pLockSemExcl(ps, pls);
        return;
    }

    KPRINTF(1, ("New ReadLockShared context (0x%08lx) on 0x%08lx\n", thistask, pls));
    prl->prl_Task = thistask;
    prl->prl_Count = 0;
    prl->prl_IsExcl = FALSE;

    // if it's exclusively locked, wait for this lock to vanish
    while(pls->pls_Owner) {
        AddTail(&pls->pls_WaitQueue, &prl->prl_Node);
        thistask->tc_SigRecvd &= ~SIGF_SINGLE;

        Wait(SIGF_SINGLE);

        Remove(&prl->prl_Node);
    }

    if(prl->prl_IsExcl) {
        // we got promoted by BorrowLocks during the process! So we don't need the shared stuff anymore
        FreePooled(ps->ps_SemaMemPool, prl, sizeof(struct PsdReadLock));
        pls->pls_Owner = thistask;
        pls->pls_ExclLockCount++;
    } else {
        // got the lock!
        AddHead(&pls->pls_ReadLocks, &prl->prl_Node);
        prl->prl_Count++;
        pls->pls_SharedLockCount++;
    }
    Permit();
    return;
}
/* \\\ */

/* /// "pUnlockSem()" */
void pUnlockSem(struct PsdBase * ps, struct PsdLockSem *pls)
{
    struct PsdReadLock *prl;
    struct Task *thistask = FindTask(NULL);
    BOOL gotit = FALSE;

    Forbid();
    if(pls->pls_Owner) {
        // exclusively locked, this means unlocking task must be owner
        if(pls->pls_Owner != thistask) {
            Permit();
            psdDebugSemaphores();
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Attempt to unlock exclusive semaphore 0x%08lx not owned by task %s!",
                           pls, thistask->tc_Node.ln_Name);
            return;

        }
        if(--pls->pls_ExclLockCount) {
            // still locked
            Permit();
            return;
        }
        pls->pls_Owner = NULL;
        // otherwise drop through and notify
    } else {
        if(!pls->pls_SharedLockCount) {
            Permit();
            psdDebugSemaphores();
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Attempt to unlock (free) semaphore 0x%08lx once too often by task %s!",
                           pls, thistask->tc_Node.ln_Name);
            return;
        }
        // find readlock
        prl = (struct PsdReadLock *) pls->pls_ReadLocks.lh_Head;
        while(prl->prl_Node.ln_Succ) {
            if(prl->prl_Task == thistask) {
                if(--prl->prl_Count) {
                    // can't be the last lock, so just reduce count and return
                    Permit();
                    return;
                }
                // remove read lock, it's no longer needed
                KPRINTF(1, ("Removing read lock context (0x%08lx) on 0x%08lx!\n", thistask, pls));
                Remove(&prl->prl_Node);
                FreePooled(ps->ps_SemaMemPool, prl, sizeof(struct PsdReadLock));
                gotit = TRUE;
                // losing a designated lock
                pls->pls_SharedLockCount--;
                break;
            }
            prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ;
        }
        if(!gotit) {
            Permit();
            psdDebugSemaphores();
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Attempt to unlock (shared) semaphore 0x%08lx once too often by task %s!",
                           pls, thistask->tc_Node.ln_Name);
            return;
        }

        // we need to notify anyway, because the waiter could already have a shared lock
        // on the same semaphore, and if we only notified on LockCount reaching zero,
        // the locker would wait forever.
    }

    // notify waiting tasks
    prl = (struct PsdReadLock *) pls->pls_WaitQueue.lh_Head;
    while(prl->prl_Node.ln_Succ) {
        Signal(prl->prl_Task, SIGF_SINGLE);
        prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ;
    }
    Permit();
}
/* \\\ */

/* /// "psdDebugSemaphores()" */
void (psdDebugSemaphores)(struct PsdBase * ps asm("a6"))
{
    struct Task *thistask = FindTask(NULL);
    struct PsdSemaInfo *psi;

    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Debug Semaphores (0x%08lx)", thistask);

    Forbid();
    // search for context
    psi = (struct PsdSemaInfo *) ps->ps_DeadlockDebug.lh_Head;
    while(psi->psi_Node.ln_Succ) {
        pDebugSemaInfo(ps, psi);
        psi = (struct PsdSemaInfo *) psi->psi_Node.ln_Succ;
    }
    Permit();
}
/* \\\ */

/* /// "psdLockReadPBase()" */
void (psdLockReadPBase)(struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdLockReadPBase(0x%08lx)\n", FindTask(NULL)));
    pLockSemShared(ps, &ps->ps_Lock);
}
/* \\\ */

/* /// "psdLockWritePBase()" */
void (psdLockWritePBase)(struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdLockWritePBase(0x%08lx)\n", FindTask(NULL)));
    pLockSemExcl(ps, &ps->ps_Lock);
}
/* \\\ */

/* /// "psdUnlockPBase()" */
void (psdUnlockPBase)(struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdUnlockPBase(0x%08lx)\n", FindTask(NULL)));
    pUnlockSem(ps, &ps->ps_Lock);
}
/* \\\ */

/* /// "psdBorrowLocksWait()" */
ULONG (psdBorrowLocksWait)(struct Task * task asm("a1"), ULONG signals asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct Task *thistask = FindTask(NULL);
    ULONG cnt = 0;
    ULONG sigmask;
    struct PsdSemaInfo *psi;
    struct PsdLockSem *pls;
    struct PsdReadLock *prl;
    struct PsdBorrowLock *pbl;
    struct List borrows;
    struct List reclaims;
    BOOL moveowner;

    /* the target's name is not printed: a short-lived subtask may already be gone */
    XPRINTF(10, ("Borrowing locks from 0x%08lx (%s) to 0x%08lx!\n",
                 thistask, thistask->tc_Node.ln_Name, task));

    Forbid();
    psi = (struct PsdSemaInfo *) ps->ps_DeadlockDebug.lh_Head;
    while(psi->psi_Node.ln_Succ) {
        pls = psi->psi_LockSem;
        if(pls->pls_Owner == thistask) {
            cnt++;
        }
        if(pls->pls_SharedLockCount) {
            struct PsdReadLock *prl = (struct PsdReadLock *) pls->pls_ReadLocks.lh_Head;
            do {
                if(prl->prl_Task == thistask) {
                    cnt++;
                    break;
                }
            } while((prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ)->prl_Node.ln_Succ);
        }
        psi = (struct PsdSemaInfo *) psi->psi_Node.ln_Succ;
    }
    if(!cnt) {
        Permit();
        XPRINTF(10, ("Nothing to borrow!\n"));
        return(Wait(signals));
    }

    NewList(&borrows);
    NewList(&reclaims);
    XPRINTF(10, ("Borrowing %ld locks\n", cnt));

    psi = (struct PsdSemaInfo *) ps->ps_DeadlockDebug.lh_Head;
    while(psi->psi_Node.ln_Succ) {
        moveowner = TRUE;
        pls = psi->psi_LockSem;
        if(pls->pls_Owner == thistask) {
            // check if the target task is already waiting for that lock
            // in this case, we simply remove our exclusive lock and let
            // the other task catch it
            prl = (struct PsdReadLock *) pls->pls_WaitQueue.lh_Head;
            while(prl->prl_Node.ln_Succ) {
                if(prl->prl_Task == task) {
                    if(!prl->prl_IsExcl) {
                        // if we hand over the excl lock, we have to make sure that the exclusiveness is kept
                        // and no other thread may catch it while it is shared.
                        // hence we will need set this lock exclusive aswell
                        // this no optimal solution, but it guarantees the same
                        // behaviour with pending lock and no pending lock
                        prl->prl_IsExcl = TRUE;
                        XPRINTF(10, ("Promo waiting lock to excl\n"));
                    }
                    // move shared lock to top of the list
                    Remove(&prl->prl_Node);
                    AddHead(&pls->pls_WaitQueue, &prl->prl_Node);
                    if((pbl = (struct PsdBorrowLock *) AllocPooled(ps->ps_SemaMemPool, sizeof(struct PsdBorrowLock)))) {
                        pbl->pbl_LockSem = pls;
                        pbl->pbl_ExclLockCount = pls->pls_ExclLockCount;
                        AddTail(&reclaims, &pbl->pbl_Node);

                        // unlock exclusive lock
                        pls->pls_ExclLockCount = 0;
                        pls->pls_Owner = NULL;
                        Signal(task, SIGF_SINGLE);
                        XPRINTF(10, ("Waiting lock 0x%08lx transfer\n", pls));
                    }
                    moveowner = FALSE;
                    break;
                }
                prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ;
            }
            if(moveowner) {
                if((pbl = (struct PsdBorrowLock *) AllocPooled(ps->ps_SemaMemPool, sizeof(struct PsdBorrowLock)))) {
                    pbl->pbl_LockSem = pls;
                    pbl->pbl_ExclLockCount = pls->pls_ExclLockCount;
                    AddTail(&borrows, &pbl->pbl_Node);
                    pls->pls_Owner = task;
                    XPRINTF(10, ("Lock 0x%08lx transfer\n", pls));
                }
            }
        }
        if(pls->pls_SharedLockCount) {
            prl = (struct PsdReadLock *) pls->pls_ReadLocks.lh_Head;
            do {
                if(prl->prl_Task == thistask) {
                    // check if target task is waiting for this task
                    struct PsdReadLock *prl2 = (struct PsdReadLock *) pls->pls_WaitQueue.lh_Head;
                    while(prl2->prl_Node.ln_Succ) {
                        if(prl2->prl_Task == task) {
                            // move lock to top of the list
                            Remove(&prl2->prl_Node);
                            AddHead(&pls->pls_WaitQueue, &prl2->prl_Node);
                            if((pbl = (struct PsdBorrowLock *) AllocPooled(ps->ps_SemaMemPool, sizeof(struct PsdBorrowLock)))) {
                                pbl->pbl_LockSem = pls;
                                pbl->pbl_ReadLock = prl;
                                pbl->pbl_Count = prl->prl_Count;
                                AddHead(&reclaims, &pbl->pbl_Node);

                                // unlock shared lock
                                Remove(&prl->prl_Node);
                                FreePooled(ps->ps_SemaMemPool, prl, sizeof(struct PsdReadLock));
                                pls->pls_SharedLockCount--;
                                Signal(task, SIGF_SINGLE);
                            }
                            moveowner = FALSE;
                            XPRINTF(10, ("Waiting shared lock 0x%08lx transfer\n", pls));
                            break;
                        }
                        prl2 = (struct PsdReadLock *) prl2->prl_Node.ln_Succ;
                    }
                    if(moveowner) {
                        // check if target task already has a shared lock on this
                        prl2 = (struct PsdReadLock *) pls->pls_ReadLocks.lh_Head;
                        do {
                            if(prl2->prl_Task == task) {
                                if((pbl = (struct PsdBorrowLock *) AllocPooled(ps->ps_SemaMemPool, sizeof(struct PsdBorrowLock)))) {
                                    // we redirect to this other lock
                                    pbl->pbl_LockSem = pls;
                                    pbl->pbl_ReadLock = prl2;
                                    pbl->pbl_Count = prl->prl_Count; // save the old lock count
                                    AddTail(&borrows, &pbl->pbl_Node);

                                    // unlock shared lock
                                    Remove(&prl->prl_Node);
                                    FreePooled(ps->ps_SemaMemPool, prl, sizeof(struct PsdReadLock));
                                    pls->pls_SharedLockCount--;
                                    // just increase lockcount, so a split occurs automatically
                                    prl2->prl_Count += pbl->pbl_Count;
                                }
                                XPRINTF(10, ("Already locked 0x%08lx transfer\n", pls));
                                moveowner = FALSE;
                                break;
                            }
                        } while((prl2 = (struct PsdReadLock *) prl2->prl_Node.ln_Succ)->prl_Node.ln_Succ);
                    }
                    if(moveowner) {
                        if((pbl = (struct PsdBorrowLock *) AllocPooled(ps->ps_SemaMemPool, sizeof(struct PsdBorrowLock)))) {
                            pbl->pbl_LockSem = pls;
                            pbl->pbl_ReadLock = prl;
                            pbl->pbl_Count = prl->prl_Count;
                            AddTail(&borrows, &pbl->pbl_Node);
                            prl->prl_Task = task;
                            XPRINTF(10, ("Std lock 0x%08lx transfer\n", pls));
                        }
                    }
                    break;
                }
            } while((prl = (struct PsdReadLock *) prl->prl_Node.ln_Succ)->prl_Node.ln_Succ);
        }
        psi = (struct PsdSemaInfo *) psi->psi_Node.ln_Succ;
    }

    sigmask = Wait(signals);

    // try to get moved locks back first
    pbl = (struct PsdBorrowLock *) borrows.lh_Head;
    while(pbl->pbl_Node.ln_Succ) {
        Remove(&pbl->pbl_Node);
        pls = pbl->pbl_LockSem;
        if(pbl->pbl_ExclLockCount) {
            if(pbl->pbl_ExclLockCount == pls->pls_ExclLockCount) {
                // all fine, other task didn't use the locks or returned them already
                pls->pls_Owner = thistask;
                FreePooled(ps->ps_SemaMemPool, pbl, sizeof(struct PsdBorrowLock));
            } else {
                // okay, bad thing, release lock and try to obtain it again -- eventually the other task should free the lock again
                pls->pls_ExclLockCount -= pbl->pbl_ExclLockCount;
                AddTail(&reclaims, &pbl->pbl_Node);
            }
        } else {
            if(pls->pls_Owner == task) {
                // oh, damn. The other task converted our shared lock into an exclusive lock --
                // we cannot claim this back right now. This gets tricky now.
                if(pbl->pbl_Count == pbl->pbl_ReadLock->prl_Count) {
                    // luckily, the count didn't change, so we just release the shared lock and requeue us into the reclaim list
                    Remove(&pbl->pbl_ReadLock->prl_Node);
                    FreePooled(ps->ps_SemaMemPool, pbl->pbl_ReadLock, sizeof(struct PsdReadLock));
                    pbl->pbl_ReadLock = NULL;
                    pls->pls_SharedLockCount--; // should turn to 0
                } else {
                    // can it get worse? obviously, the alien task also has added some read locks
                    // this means we need to split up!
                    // therefore we leave a few lock counts and requeue
                    pbl->pbl_ReadLock->prl_Count -= pbl->pbl_Count;
                    pbl->pbl_ReadLock = NULL;
                }
                AddHead(&reclaims, &pbl->pbl_Node);
            } else {
                if(pbl->pbl_Count == pbl->pbl_ReadLock->prl_Count) {
                    // the count didn't change, just so just change owner
                    pbl->pbl_ReadLock->prl_Task = thistask;
                    FreePooled(ps->ps_SemaMemPool, pbl, sizeof(struct PsdBorrowLock));
                } else {
                    // the alien task still has some read locks
                    // this means we need to split up!
                    // therefore we leave a few lock counts and requeue
                    pbl->pbl_ReadLock->prl_Count -= pbl->pbl_Count;
                    pbl->pbl_ReadLock = NULL;
                    AddHead(&reclaims, &pbl->pbl_Node);
                }
            }
        }
        pbl = (struct PsdBorrowLock *) borrows.lh_Head;
    }

    // try to reclaim released locks
    pbl = (struct PsdBorrowLock *) reclaims.lh_Head;
    while(pbl->pbl_Node.ln_Succ) {
        Remove(&pbl->pbl_Node);
        pls = pbl->pbl_LockSem;
        while(pbl->pbl_Count) {
            pLockSemShared(ps, pls);
            --pbl->pbl_Count;
        }
        while(pbl->pbl_ExclLockCount) {
            pLockSemExcl(ps, pls);
            --pbl->pbl_ExclLockCount;
        }
        FreePooled(ps->ps_SemaMemPool, pbl, sizeof(struct PsdBorrowLock));
        pbl = (struct PsdBorrowLock *) reclaims.lh_Head;
    }
    Permit();

    return(sigmask);
}
/* \\\ */

/* *** Support *** */

/* /// "psdCopyStr()" */
STRPTR (psdCopyStr)(CONST_STRPTR name asm("a0"), struct PsdBase * ps asm("a6"))
{
    STRPTR rs = psdAllocVec((ULONG) strlen(name)+1);
    KPRINTF(1, ("psdCopyStr(%s)\n", name));
    if(rs) {
        strcpy(rs, name);
    }
    return(rs);
}
/* \\\ */

/* /// "psdSafeRawDoFmtA()" */
void (psdSafeRawDoFmtA)(STRPTR buf asm("a0"), ULONG len asm("d0"), CONST_STRPTR fmtstr asm("a1"), RAWARG fmtdata asm("a2"), struct PsdBase * ps asm("a6"))
{
    struct PsdRawDoFmt rdf;

    if(len > 0) {
        rdf.rdf_Len = len;
        rdf.rdf_Buf = buf;
        RawDoFmt(fmtstr, fmtdata, (void (*)()) pPutChar, &rdf);
        buf[len-1] = 0;
    }
}
/* \\\ */

/* /// "pPutChar()" */
void pPutChar(char ch asm("d0"), struct PsdRawDoFmt * rdf asm("a3"))
{
    if(rdf->rdf_Len) {
        rdf->rdf_Len--;
        *rdf->rdf_Buf++ = ch;
    }
}
/* \\\ */

/* /// "psdCopyStrFmtA()" */
STRPTR (psdCopyStrFmtA)(CONST_STRPTR fmtstr asm("a0"), RAWARG fmtdata asm("a1"), struct PsdBase * ps asm("a6"))
{
    ULONG len = 0;
    STRPTR buf;

    RawDoFmt(fmtstr, fmtdata, (void (*)()) pRawFmtLength, &len);
    buf = psdAllocVec(len+1);
    if(buf) {
        psdSafeRawDoFmtA(buf, len+1, fmtstr, fmtdata);
    }
    return(buf);
}
/* \\\ */

/* /// "pRawFmtLength()" */
void pRawFmtLength(char ch asm("d0"), ULONG * len asm("a3"))
{
    (*len)++;
}
/* \\\ */

/* /// "psdDelayMS()" */
void (psdDelayMS)(ULONG milli asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct MsgPort mp;
    struct timerequest tr;

    /* Clear memory for messageport */
    memset(&mp, 0, sizeof(mp));

    KPRINTF(1, ("psdDelayMS(%ld)\n", milli));
    mp.mp_Flags = PA_SIGNAL;
    mp.mp_SigBit = SIGB_SINGLE;
    mp.mp_SigTask = FindTask(NULL);
    NewList(&mp.mp_MsgList);
    CopyMem(&ps->ps_TimerIOReq, &tr, sizeof(tr));
    tr.tr_node.io_Message.mn_ReplyPort = &mp;
    tr.tr_time.tv_secs  = 0;
    tr.tr_time.tv_micro = milli * 1000;
    DoIO((struct IORequest *) &tr);
}
/* \\\ */

/* /// "psdSpawnSubTask()" */
struct Task * (psdSpawnSubTask)(STRPTR name asm("a0"), APTR initpc asm("a1"), APTR userdata asm("a2"), struct PsdBase * ps asm("a6"))
{
    /* The notifier and every class GUI run MUI on these stacks, and MUI 5 warns below
       32K. Not AROS_STACKSIZE: that stays the shell-command default. */
#define SUBTASKSTACKSIZE 32768
    struct {
        struct MemList mrm_ml;
        struct MemEntry mtm_me[2];
    } memlist;

    struct MemList *newmemlist;
    struct MemEntry *me;
    struct Task *nt;
    struct Process *subtask;

    if(!(name && initpc)) {
        return(NULL);
    }

    /* If there's dos available, create a process instead of a task */
    if(pOpenDOS(ps)) {
        /* NP_UserData is an AROS/OS4 tag; OS 3.2's dos.library silently ignores it,
         * leaving tc_UserData unset - the subtask then reads garbage and crashes. */
        Forbid();
        subtask = CreateNewProcTags(NP_Entry, (IPTR)initpc,
                                    NP_StackSize, SUBTASKSTACKSIZE,
                                    NP_Priority, ps->ps_GlobalCfg->pgc_SubTaskPri,
                                    NP_Name, (IPTR)name,
                                    NP_CopyVars, FALSE,
                                    TAG_END);
        if(subtask) {
            subtask->pr_Task.tc_UserData = userdata;
        }
        Permit();
        return((struct Task *) subtask);
    }

    /* Allocate memory of memlist */

    memlist.mrm_ml.ml_Node.ln_Type = NT_MEMORY;
    memlist.mrm_ml.ml_Node.ln_Pri = 0;
    memlist.mrm_ml.ml_Node.ln_Name = NULL;
    memlist.mrm_ml.ml_NumEntries = 3;
    me = &memlist.mrm_ml.ml_ME[0];
    me[1].me_Un.meu_Reqs = memlist.mrm_ml.ml_ME[0].me_Un.meu_Reqs = MEMF_CLEAR|MEMF_PUBLIC;
    me[0].me_Length = sizeof(struct Task);
    me[1].me_Length = SUBTASKSTACKSIZE;
    me[2].me_Un.meu_Reqs = MEMF_PUBLIC;
    me[2].me_Length = strlen(name) + 1;

    newmemlist = AllocEntry(&memlist.mrm_ml);
    if((IPTR) newmemlist & 0x80000000)
    {
        return(NULL);
    }
    me = &newmemlist->ml_ME[0];
    nt = me[0].me_Un.meu_Addr;
    nt->tc_Node.ln_Name = me[2].me_Un.meu_Addr;
    strcpy(nt->tc_Node.ln_Name, name);
    nt->tc_Node.ln_Type = NT_TASK;
    nt->tc_Node.ln_Pri = ps->ps_GlobalCfg->pgc_SubTaskPri;
    nt->tc_SPLower = me[1].me_Un.meu_Addr;
    nt->tc_SPUpper = nt->tc_SPReg = (APTR) ((IPTR) nt->tc_SPLower + SUBTASKSTACKSIZE);
    nt->tc_UserData = userdata;
    NewList(&nt->tc_MemEntry);
    AddTail(&nt->tc_MemEntry, (struct Node *) newmemlist);
#if !defined(__AROSEXEC_SMP__)
    KPRINTF(1, ("TDNestCnt=%ld\n", EXEC_BASE_NAME->TDNestCnt));
#endif
    if((nt = AddTask(nt, initpc, NULL))) {
        XPRINTF(10, ("Started task 0x%08lx (%s)\n", nt, name));
        return(nt);
    }
    FreeEntry(newmemlist);
    return(NULL);
}
/* \\\ */

/* /// "psdNumToStr()" */
STRPTR (psdNumToStr)(UWORD type asm("d0"), LONG idx asm("d1"), STRPTR defstr asm("a0"), struct PsdBase * ps asm("a6"))
{
    switch(type) {
    case NTS_IOERR: {
        const struct PsdWStringMap *psm = usbhwioerrstr;
        while(psm->psm_ID) {
            if(psm->psm_ID == idx) {
                return(psm->psm_String);
            }
            psm++;
        }
        break;
    }

    case NTS_LANGID: {
        const struct PsdUWStringMap *psm = usblangids;
        while(psm->psm_ID) {
            if(psm->psm_ID == idx) {
                return(psm->psm_String);
            }
            psm++;
        }
        break;
    }

    case NTS_TRANSTYPE:
        switch(idx) {
        case USEAF_CONTROL:
            return("control");
        case USEAF_ISOCHRONOUS:
            return("isochronous");
        case USEAF_BULK:
            return("bulk");
        case USEAF_INTERRUPT:
            return("interrupt");
        }
        break;

    case NTS_SYNCTYPE:
        switch(idx) {
        case USEAF_NOSYNC:
            return("no synchronization");
        case USEAF_ASYNC:
            return("asynchronous");
        case USEAF_ADAPTIVE:
            return("adaptive");
        case USEAF_SYNC:
            return("synchronous");
        }
        break;

    case NTS_USAGETYPE:
        switch(idx) {
        case USEAF_DATA:
            return("data");
        case USEAF_FEEDBACK:
            return("feedback");
        case USEAF_IMPLFEEDBACK:
            return("implicit feedback data");
        }
        break;

    case NTS_VENDORID: {
        const struct PsdUWStringMap *psm = usbvendorids;
        while(psm->psm_ID) {
            if(psm->psm_ID == idx) {
                return(psm->psm_String);
            }
            psm++;
        }
        break;
    }

    case NTS_CLASSCODE: {
        const struct PsdWStringMap *psm = usbclasscodestr;
        while(psm->psm_ID) {
            if(psm->psm_ID == idx) {
                return(psm->psm_String);
            }
            psm++;
        }
        break;
    }

    case NTS_DESCRIPTOR: {
        const struct PsdULStringMap *psm = usbdesctypestr;
        while(psm->psm_ID) {
            if(psm->psm_ID == idx) {
                return(psm->psm_String);
            }
            psm++;
        }
        break;
    }

    case NTS_COMBOCLASS: {
        const struct PsdULStringMap *psm = usbcomboclasscodestr;
        if(idx & (NTSCCF_CLASS|NTSCCF_SUBCLASS|NTSCCF_PROTO)) {
            while(psm->psm_ID) {
                BOOL take;
                take = TRUE;
                if(psm->psm_ID & NTSCCF_CLASS) {
                    if((!(idx & NTSCCF_CLASS)) || ((idx & 0x0000ff) != (psm->psm_ID & 0x0000ff))) {
                        take = FALSE;
                    }
                }
                if(psm->psm_ID & NTSCCF_SUBCLASS) {
                    if((!(idx & NTSCCF_SUBCLASS)) || ((idx & 0x00ff00) != (psm->psm_ID & 0x00ff00))) {
                        take = FALSE;
                    }
                }
                if(psm->psm_ID & NTSCCF_PROTO) {
                    if((!(idx & NTSCCF_PROTO)) || ((idx & 0xff0000) != (psm->psm_ID & 0xff0000))) {
                        take = FALSE;
                    }
                }
                if(take) {
                    return(psm->psm_String);
                }
                psm++;
            }
        }
        break;
    }
    }
    return(defstr);
}
/* \\\ */

/* *** Endpoint *** */

/* /// "pFreeEndpoint()" */
void pFreeEndpoint(struct PsdEndpoint *pep)
{
    struct PsdBase * ps = pep->pep_Interface->pif_Config->pc_Device->pd_Hardware->phw_Base;
    KPRINTF(2, ("    FreeEndpoint()\n"));
    Remove(&pep->pep_Node);
    psdFreeVec(pep);
}
/* \\\ */

/* /// "pAllocEndpoint()" */
struct PsdEndpoint * pAllocEndpoint(struct PsdInterface *pif)
{
    struct PsdBase * ps = pif->pif_Config->pc_Device->pd_Hardware->phw_Base;
    struct PsdEndpoint *pep;
    if((pep = psdAllocVec(sizeof(struct PsdEndpoint)))) {
        pep->pep_Interface = pif;
        pep->pep_StreamBase = 0;
        pep->pep_MaxStreams = 0;
        AddTail(&pif->pif_EPs, &pep->pep_Node);
        return(pep);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdFindEndpointA()" */
struct PsdEndpoint * (psdFindEndpointA)(struct PsdInterface * pif asm("a0"), struct PsdEndpoint * pep asm("a2"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct TagItem *ti;
    BOOL takeit;

    KPRINTF(2, ("psdFindEndpointA(0x%08lx, 0x%08lx, 0x%08lx)\n", pif, pep, tags));
    if(!pep) {
        pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
    } else {
        pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ;
    }
    while(pep->pep_Node.ln_Succ) {
        takeit = TRUE;
        if((ti = FindTagItem(EA_IsIn, tags))) {
            if((ti->ti_Data && !pep->pep_Direction) || (!ti->ti_Data && pep->pep_Direction)) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(EA_EndpointNum, tags))) {
            if(ti->ti_Data != pep->pep_EPNum) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(EA_TransferType, tags))) {
            if(ti->ti_Data != pep->pep_TransType) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(EA_MaxPktSize, tags))) {
            if(ti->ti_Data != pep->pep_MaxPktSize) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(EA_Interval, tags))) {
            if(ti->ti_Data != pep->pep_Interval) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(EA_MaxBurst, tags))) {
            if(ti->ti_Data != pep->pep_MaxBurst) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(EA_CompAttributes, tags))) {
            if(ti->ti_Data != pep->pep_CompAttributes) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(EA_BytesPerInterval, tags))) {
            if(ti->ti_Data != pep->pep_BytesPerInterval) {
                takeit = FALSE;
            }
        }

        if(takeit) {
            return(pep);
        }
        pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ;
    }
    return(NULL);
}
/* \\\ */

/* *** Interface *** */

/* /// "pFreeInterface()" */
void pFreeInterface(struct PsdInterface *pif)
{
    struct PsdBase * ps = pif->pif_Config->pc_Device->pd_Hardware->phw_Base;
    struct PsdEndpoint *pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
    struct PsdInterface *altif = (struct PsdInterface *) pif->pif_AlterIfs.lh_Head;
    KPRINTF(2, ("   FreeInterface()\n"));
    /* Remove alternate interfaces */
    while(altif->pif_Node.ln_Succ) {
        pFreeInterface(altif);
        altif = (struct PsdInterface *) pif->pif_AlterIfs.lh_Head;
    }
    /* Remove endpoints */
    while(pep->pep_Node.ln_Succ) {
        pFreeEndpoint(pep);
        pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
    }
    psdFreeVec(pif->pif_IfStr);
    psdFreeVec(pif->pif_IDString);
    Remove(&pif->pif_Node);
    psdFreeVec(pif);
}
/* \\\ */

/* /// "pAllocInterface()" */
struct PsdInterface * pAllocInterface(struct PsdConfig *pc)
{
    struct PsdBase * ps = pc->pc_Device->pd_Hardware->phw_Base;
    struct PsdInterface *pif;
    if((pif = psdAllocVec(sizeof(struct PsdInterface)))) {
        pif->pif_Config = pc;
        NewList(&pif->pif_EPs);
        NewList(&pif->pif_AlterIfs);
        AddTail(&pc->pc_Interfaces, &pif->pif_Node);
        return(pif);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdFindInterfaceA()" */
struct PsdInterface * (psdFindInterfaceA)(struct PsdDevice * pd asm("a0"), struct PsdInterface * pif asm("a2"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdConfig *pc;
    struct TagItem *ti;
    BOOL takeit;
    BOOL searchalt = FALSE;
    BOOL isalt = FALSE;
    struct PsdInterface *oldpif = NULL;

    KPRINTF(2, ("psdFindInterfaceA(0x%08lx, 0x%08lx, 0x%08lx)\n", pd, pif, tags));
    if(!pif) {
        pc = pd->pd_CurrentConfig;
        if(pc) {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
        }
        if(!pif) {
            return(NULL);
        }
    } else {
        if(FindTagItem(IFA_AlternateNum, tags)) {
            searchalt = TRUE;
        }
        if(pif->pif_ParentIf) {
            // special case: we are in an alternate interface right now
            searchalt = TRUE;
            if(pif->pif_Node.ln_Succ) {
                isalt = TRUE;
                oldpif = pif->pif_ParentIf;
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            } else {
                pif = (struct PsdInterface *) pif->pif_ParentIf->pif_Node.ln_Succ;
            }
        } else {
            // go into alt interfaces
            if(searchalt && pif->pif_AlterIfs.lh_Head->ln_Succ) {
                isalt = TRUE;
                oldpif = pif;
                pif = (struct PsdInterface *) pif->pif_AlterIfs.lh_Head;
            } else {
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
        }
    }

    while(pif->pif_Node.ln_Succ) {
        takeit = TRUE;
        if((ti = FindTagItem(IFA_InterfaceNum, tags))) {
            if(ti->ti_Data != pif->pif_IfNum) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(IFA_AlternateNum, tags))) {
            searchalt = TRUE;
            if(ti->ti_Data <= 0xff) { // if alternate number is greater than 0xff, don't check compliance, but just enable alternate interface searching
                if(ti->ti_Data != pif->pif_Alternate) {
                    takeit = FALSE;
                }
            }
        }
        if((ti = FindTagItem(IFA_NumEndpoints, tags))) {
            if(ti->ti_Data != pif->pif_NumEPs) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(IFA_Class, tags))) {
            if(ti->ti_Data != pif->pif_IfClass) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(IFA_SubClass, tags))) {
            if(ti->ti_Data != pif->pif_IfSubClass) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(IFA_Protocol, tags))) {
            if(ti->ti_Data != pif->pif_IfProto) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(IFA_Binding, tags))) {
            if((APTR) ti->ti_Data != pif->pif_IfBinding) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(IFA_InterfaceName, tags))) {
            if(strcmp((STRPTR) ti->ti_Data, pif->pif_IfStr)) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(IFA_IDString, tags))) {
            if(strcmp((STRPTR) ti->ti_Data, pif->pif_IDString)) {
                takeit = FALSE;
            }
        }

        if(takeit) {
            return(pif);
        }
        if(searchalt) {
            if(isalt) {
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                if(!pif->pif_Node.ln_Succ) {
                    pif = (struct PsdInterface *) oldpif->pif_Node.ln_Succ;
                    isalt = FALSE;
                }
            } else {
                oldpif = pif;
                pif = (struct PsdInterface *) pif->pif_AlterIfs.lh_Head;
                if(!pif->pif_Node.ln_Succ) {
                    pif = (struct PsdInterface *) oldpif->pif_Node.ln_Succ;
                } else {
                    isalt = TRUE;
                }
            }
        } else {
            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
        }
    }
    return(NULL);
}
/* \\\ */

/* *** Config *** */

/* /// "pFreeConfig()" */
void pFreeConfig(struct PsdConfig *pc)
{
    struct PsdBase * ps = pc->pc_Device->pd_Hardware->phw_Base;
    struct PsdInterface *pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
    KPRINTF(2, ("  FreeConfig()\n"));
    while(pif->pif_Node.ln_Succ) {
        psdReleaseIfBinding(pif);
        pFreeInterface(pif);
        pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
    }
    psdFreeVec(pc->pc_CfgStr);
    Remove(&pc->pc_Node);
    psdFreeVec(pc);
}
/* \\\ */

/* /// "pAllocConfig()" */
struct PsdConfig * pAllocConfig(struct PsdDevice *pd)
{
    struct PsdBase * ps = pd->pd_Hardware->phw_Base;
    struct PsdConfig *pc;
    KPRINTF(2, ("  AllocConfig()\n"));
    if((pc = psdAllocVec(sizeof(struct PsdConfig)))) {
        pc->pc_Device = pd;
        NewList(&pc->pc_Interfaces);
        AddTail(&pd->pd_Configs, &pc->pc_Node);
        return(pc);
    }
    return(NULL);
}
/* \\\ */

/* *** Descriptors *** */

/* /// "pFreeDescriptor()" */
void pFreeDescriptor(struct PsdDescriptor *pdd)
{
    struct PsdBase * ps = pdd->pdd_Device->pd_Hardware->phw_Base;
    KPRINTF(2, ("  FreeDescriptor()\n"));
    //psdFreeVec(pdd->pdd_Data); // part of the structure alloc
    Remove(&pdd->pdd_Node);
    psdFreeVec(pdd);
}
/* \\\ */

/* /// "pAllocDescriptor()" */
struct PsdDescriptor * pAllocDescriptor(struct PsdDevice *pd, UBYTE *buf)
{
    struct PsdBase * ps = pd->pd_Hardware->phw_Base;
    struct PsdDescriptor *pdd;

    KPRINTF(2, ("  AllocDescriptor()\n"));
    if((pdd = psdAllocVec(sizeof(struct PsdDescriptor) + (ULONG) buf[0]))) {
        pdd->pdd_Device = pd;
        pdd->pdd_Data = ((UBYTE *) pdd) + sizeof(struct PsdDescriptor);
        pdd->pdd_Length = buf[0];
        pdd->pdd_Type = buf[1];
        if((pdd->pdd_Type >= UDT_CS_UNDEFINED) && (pdd->pdd_Type <= UDT_CS_ENDPOINT)) {
            pdd->pdd_CSSubType = buf[2];
        }
        pdd->pdd_Name = psdNumToStr(NTS_DESCRIPTOR, (LONG) pdd->pdd_Type, "<unknown>");
        CopyMem(buf, pdd->pdd_Data, (ULONG) buf[0]);
        AddTail(&pd->pd_Descriptors, &pdd->pdd_Node);
        return(pdd);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdFindDescriptorA()" */
struct PsdDescriptor * (psdFindDescriptorA)(struct PsdDevice * pd asm("a0"), struct PsdDescriptor * pdd asm("a2"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdConfig *pc = pd->pd_CurrentConfig;
    struct TagItem *ti;
    BOOL takeit;

    KPRINTF(2, ("psdFindDescriptorA(0x%08lx, 0x%08lx, 0x%08lx)\n", pd, pdd, tags));
    if(!pdd) {
        pdd = (struct PsdDescriptor *) pd->pd_Descriptors.lh_Head;
    } else {
        pdd = (struct PsdDescriptor *) pdd->pdd_Node.ln_Succ;
    }

    while(pdd->pdd_Node.ln_Succ) {
        takeit = TRUE;

        if((ti = FindTagItem(DDA_Config, tags))) {
            // special case to workaround default: with NULL given, all configs are matched
            if(ti->ti_Data && (((struct PsdConfig *) ti->ti_Data) != pdd->pdd_Config)) {
                takeit = FALSE;
            }
        } else {
            // only take descriptors from the current configuration by default
            if(pc != pdd->pdd_Config) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DDA_Interface, tags))) {
            if(((struct PsdInterface *) ti->ti_Data) != pdd->pdd_Interface) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DDA_Endpoint, tags))) {
            if(((struct PsdEndpoint *) ti->ti_Data) != pdd->pdd_Endpoint) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DDA_DescriptorType, tags))) {
            if(ti->ti_Data != pdd->pdd_Type) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DDA_CS_SubType, tags))) {
            if(ti->ti_Data != pdd->pdd_CSSubType) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DDA_DescriptorLength, tags))) {
            if(ti->ti_Data != pdd->pdd_Length) {
                takeit = FALSE;
            }
        }

        if(takeit) {
            return(pdd);
        }
        pdd = (struct PsdDescriptor *) pdd->pdd_Node.ln_Succ;
    }
    return(NULL);
}
/* \\\ */

/* *** Device *** */

/* /// "pFreeBindings()" */
void pFreeBindings(struct PsdBase * ps, struct PsdDevice *pd)
{
    struct PsdHardware *phw = pd->pd_Hardware;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    KPRINTF(3, (" FreeBindings(0x%08lx)\n", pd));

    /* move device to list of dead devices first
       This caused a lot of trouble as it could
       happen that a device got into class scan
       right after the bindings had been released. */
    psdLockWritePBase();
    Remove(&pd->pd_Node);
    AddTail(&phw->phw_DeadDevices, &pd->pd_Node);
    psdUnlockPBase();

    /* If there are bindings, get rid of them. */
    psdLockWriteDevice(pd);
    psdReleaseDevBinding(pd);

    pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
    while(pc->pc_Node.ln_Succ) {
        pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
        while(pif->pif_Node.ln_Succ) {
            psdReleaseIfBinding(pif);
            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
        }
        pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
    }
    psdUnlockDevice(pd);
}
/* \\\ */

/* /// "pFreeDevice()" */
void pFreeDevice(struct PsdBase * ps, struct PsdDevice *pd)
{
    struct PsdHardware *phw = pd->pd_Hardware;
    struct PsdConfig *pc;
    struct PsdDescriptor *pdd;

    psdCalculatePower(phw);
    psdLockWriteDevice(pd);
    if(pd->pd_UseCnt) {
        KPRINTF(20, ("Couldn't free device, use cnt %ld\n", pd->pd_UseCnt));
        pd->pd_Flags &= ~PDFF_CONNECTED;
        pd->pd_Flags |= PDFF_DELEXPUNGE;
        psdUnlockDevice(pd);
    } else {
        /* disarm the deferred collector: hop_DestroyDevice may pump a temporary
           EP0 pipe through psdFreePipe, which would re-enter us otherwise */
        pd->pd_Flags &= ~PDFF_DELEXPUNGE;
        pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        while(pc->pc_Node.ln_Succ) {
            pFreeConfig(pc);
            pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        }

        pdd = (struct PsdDescriptor *) pd->pd_Descriptors.lh_Head;
        while(pdd->pdd_Node.ln_Succ) {
            pFreeDescriptor(pdd);
            pdd = (struct PsdDescriptor *) pd->pd_Descriptors.lh_Head;
        }

        psdFreeVec(pd->pd_LangIDArray);
        pd->pd_LangIDArray = NULL;
        psdFreeVec(pd->pd_MnfctrStr);
        pd->pd_MnfctrStr = NULL;
        /*if(!ps->ps_Notify.nt_Task) // keep name at least
        {
            psdFreeVec(pd->pd_ProductStr);
            pd->pd_ProductStr = NULL;
        }*/
        psdFreeVec(pd->pd_OldProductStr);
        pd->pd_OldProductStr = NULL;
        psdFreeVec(pd->pd_SerNumStr);
        pd->pd_SerNumStr = NULL;
        psdFreeVec(pd->pd_IDString);
        pd->pd_IDString = NULL;
        /* Guard is for the unbound-hardware case (backend never bound) */
        if(phw->phw_HCDOps) {
            /* release backend addressing state (legacy: the phw_DevArray slot) */
            phw->phw_HCDOps->hop_DestroyDevice(ps, pd);
        }
        psdUnlockDevice(pd);
        psdLockWritePBase();
        Remove(&pd->pd_Node);
        psdUnlockPBase();
        pDeleteSem(ps, &pd->pd_Lock);
        /* cannot free this vector -- tasks might still call LockDevice */
        //psdFreeVec(pd);
    }
    KPRINTF(3, ("FreeDevice done\n"));
}
/* \\\ */

/* /// "psdFreeDevice()" */
void (psdFreeDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdHardware *phw = pd->pd_Hardware;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct PsdRTIsoHandler *prt;
    struct PsdRTIsoHandler *nextprt;

    KPRINTF(3, (" FreeDevice(0x%08lx)\n", pd));

    /* move device to list of dead devices first
       This caused a lot of trouble as it could
       happen that a device got into class scan
       right after the bindings had been released. */
    psdLockWritePBase();
    Remove(&pd->pd_Node);
    AddTail(&phw->phw_DeadDevices, &pd->pd_Node);
    pd->pd_Flags &= ~PDFF_DELEXPUNGE;
    psdUnlockPBase();

    psdLockWriteDevice(pd);

    /* Inform all ISO handlers about the device going offline */
    prt = (struct PsdRTIsoHandler *) pd->pd_RTIsoHandlers.lh_Head;
    while((nextprt = (struct PsdRTIsoHandler *) prt->prt_Node.ln_Succ)) {
        if(prt->prt_ReleaseHook) {
            CallHookPkt(prt->prt_ReleaseHook, prt, NULL);
        }
        prt = nextprt;
    }

    /* If there are bindings, get rid of them. */
    psdHubReleaseDevBinding(pd);

    pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
    while(pc->pc_Node.ln_Succ) {
        pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
        while(pif->pif_Node.ln_Succ) {
            psdHubReleaseIfBinding(pif);
            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
        }
        pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
    }
    psdUnlockDevice(pd);

    pFreeDevice(ps, pd);
}
/* \\\ */

/* /// "psdAllocDevice()" */
struct PsdDevice * (psdAllocDevice)(struct PsdHardware * phw asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd;
    KPRINTF(2, ("psdAllocDevice(0x%08lx)\n", phw));
    if((pd = psdAllocVec(sizeof(struct PsdDevice)))) {
        memset(pd, 0, sizeof(struct PsdDevice));
        pd->pd_Hardware = phw;
        pd->pd_Hub = NULL;
        pd->pd_MaxPktSize0 = 8;

        pInitSem(ps, &pd->pd_Lock, "Device");

        NewList(&pd->pd_Configs);
        NewList(&pd->pd_Descriptors);
        NewList(&pd->pd_RTIsoHandlers);

        // init prefs
        pd->pd_NotifyCfg.ntc_ChunkID = AROS_LONG2BE(IFFCHNK_POPUP);
        pd->pd_NotifyCfg.ntc_Length = AROS_LONG2BE(sizeof(struct PsdNotifyCfg) - 8);
        pd->pd_NotifyCfg.ntc_InhibitPopup = FALSE;
        pd->pd_NotifyCfg.ntc_NoClassBind = FALSE;
        pd->pd_NotifyCfg.ntc_OverridePowerInfo = POCP_TRUST_DEVICE;
        pd->pd_NotifyCfg.ntc_LinkPowerOverride = POCL_INHERIT;
        pd->pd_NotifyCfg.ntc_NoAutoSuspend = FALSE;

        psdLockWritePBase();
        AddTail(&phw->phw_Devices, &pd->pd_Node);
        psdUnlockPBase();
        return(pd);
    } else {
        KPRINTF(20, ("psdAllocDevice(): out of memory!\n"));
    }
    return(NULL);
}
/* \\\ */

/* /// "psdLockReadDevice()" */
void (psdLockReadDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdLockReadDevice(0x%08lx, 0x%08lx)\n", pd, FindTask(NULL)));
    pLockSemShared(ps, &pd->pd_Lock);
}
/* \\\ */

/* /// "psdLockWriteDevice()" */
void (psdLockWriteDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdLockWriteDevice(0x%08lx, 0x%08lx)\n", pd, FindTask(NULL)));
    pLockSemExcl(ps, &pd->pd_Lock);
}
/* \\\ */

/* /// "psdUnlockDevice()" */
void (psdUnlockDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdUnlockDevice(0x%08lx, 0x%08lx)\n", pd, FindTask(NULL)));
    pUnlockSem(ps, &pd->pd_Lock);
}
/* \\\ */

/* /// "pAllocDevAddr()" */
/* LEGACY backend only (pLegacyAddressDevice / pLegacyDestroyDevice): software
 * bus-address bookkeeping in phw_DevArray.  Context HCDs own addressing - the
 * handle is opaque and pd_DevAddr stays 0 on that backend. */
UWORD pAllocDevAddr(struct PsdDevice *pd)
{
    struct PsdHardware *phw = pd->pd_Hardware;
    UWORD da;
    if(pd->pd_DevAddr) {
        return(pd->pd_DevAddr);
    }
    for(da = 1; da < 128; da++) {
        if(!phw->phw_DevArray[da]) {
            phw->phw_DevArray[da] = pd;
            pd->pd_DevAddr = da;
            return(da);
        }
    }
    return(0);
}
/* \\\ */

/* /// "psdGetStringDescriptor()" */
STRPTR (psdGetStringDescriptor)(struct PsdPipe * pp asm("a1"), UWORD idx asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd = pp->pp_Device;
    ULONG len;
    UBYTE buf[256];
    UWORD *tmpptr;
    UWORD *tmpbuf;
    STRPTR rs;
    STRPTR cbuf;
    LONG ioerr;
    UWORD widechar;
    KPRINTF(1, ("psdGetStringDescriptor(0x%08lx, %ld)\n", pp, idx));

    buf[0] = 0;
    if(!pd->pd_LangIDArray) {
        KPRINTF(10,("Trying to get language array...\n"));
        psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE,
                     USR_GET_DESCRIPTOR, UDT_STRING<<8, 0);
        ioerr = psdDoPipe(pp, buf, 2);
        if(ioerr == UHIOERR_OVERFLOW) {
            ioerr = 0;
            psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname, "Language array overflow.");
        }
        if(ioerr) {
            ioerr = psdDoPipe(pp, buf, 256);
            if(ioerr == UHIOERR_RUNTPACKET) {
                ioerr = 0;
            }
        }
        if(!ioerr) {
            len = buf[0];
            if((pd->pd_LangIDArray = psdAllocVec(max(len, 4)))) {
                tmpbuf = tmpptr = pd->pd_LangIDArray;
                KPRINTF(1, ("Getting LangID Array length %ld\n", len));
                // generate minimal sized array
                if(len < 4) {
                    len = 4;
                    *tmpbuf++ = 0;
                    *tmpbuf = 0x0409;
                    /*psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Language array descriptor too small (len %ld), using dummy.",
                                   len);*/
                    ioerr = 0;
                } else {
                    ioerr = psdDoPipe(pp, tmpbuf++, len);
                }
                if(!ioerr) {
                    len >>= 1;
                    while(--len) {
                        KPRINTF(1, ("LangID: %04lx\n", AROS_LE2WORD(*tmpbuf)));
                        *tmpptr++ = AROS_LE2WORD(*tmpbuf);
                        tmpbuf++;
                    }
                    *tmpptr = 0;
                    tmpptr = pd->pd_LangIDArray;
                    pd->pd_CurrLangID = *tmpptr;
                    while(*tmpptr) {
                        if(*tmpptr == 0x0409) {
                            pd->pd_CurrLangID = *tmpptr;
                            break;
                        }
                        tmpptr++;
                    }
                } else {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Reading language array descriptor (len %ld) failed: %s (%ld)",
                                   len, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    KPRINTF(15, ("Error reading lang array descriptor (%ld) failed %ld\n", len, ioerr));
                    *tmpptr = 0;
                }
            } else {
                KPRINTF(20, ("No langid array memory!\n"));
            }
        } else {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Reading language array descriptor (len %ld) failed: %s (%ld)",
                           2, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            KPRINTF(15, ("Error reading lang array descriptor (2) failed %ld\n", ioerr));
            /* Create empty array */
            if((pd->pd_LangIDArray = psdAllocVec(2))) {
                *pd->pd_LangIDArray = 0;
            }
        }
    }
    buf[0] = 0;
    psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE,
                 USR_GET_DESCRIPTOR, (UDT_STRING<<8)|idx, pd->pd_CurrLangID);
    ioerr = psdDoPipe(pp, buf, 2);
    if(ioerr == UHIOERR_OVERFLOW) {
        ioerr = 0;
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "String %ld overflow.", idx);
    }
    if(ioerr) {
        ioerr = psdDoPipe(pp, buf, 256);
    }
    if(!ioerr) {
        len = buf[0];
        if(len > 2) {
            tmpptr = (UWORD *) buf;
            KPRINTF(1, ("Getting String Descriptor %ld, length %ld\n", idx, len));
            ioerr = psdDoPipe(pp, tmpptr++, len);
            if(ioerr == UHIOERR_RUNTPACKET) {
                len = pp->pp_IOReq.iouh_Actual;
                if(len > 3) {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "String descriptor %ld truncated to %ld, requested %ld",
                                   idx, len, buf[0]);
                    ioerr = 0;
                }
            } else if(ioerr) {
                ioerr = psdDoPipe(pp, buf, 256);
            }
            if(!ioerr) {
                if((cbuf = rs = psdAllocVec(len>>1))) {
                    len >>= 1;
                    while(--len) {
                        widechar = *tmpptr++;
                        widechar = AROS_LE2WORD(widechar);
                        if(widechar == 0) {
                            /* buggy devices pad inside bLength with NULs -
                             * keep the remainder visible instead of truncating */
                            *cbuf++ = ' ';
                        } else if((widechar < 0x20) || (widechar > 255)) {
                            *cbuf++ = '?';
                        } else {
                            *cbuf++ = widechar;
                        }
                    }
                    *cbuf = 0;
                    KPRINTF(1, ("String \"%s\"\n", rs));
                    if(*rs) {
                        return(rs);
                    } else {
                        psdFreeVec(rs);
                        return(NULL);
                    }
                } else {
                    KPRINTF(20, ("No memory for string!\n"));
                }
            } else {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Reading string descriptor %ld (len %ld) failed: %s (%ld)",
                               idx, len, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                KPRINTF(15, ("Error reading string descriptor %ld (%ld) failed %ld\n",
                             idx, len, ioerr));
            }
        } else {
            KPRINTF(5, ("Empty string\n"));
            return(psdCopyStr(""));
        }
    } else {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Reading string descriptor %ld (len %ld) failed: %s (%ld)",
                       idx, 2, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(15, ("Error reading string descriptor %ld (2) failed %ld\n", idx, ioerr));
    }
    return(NULL);
}
/* \\\ */

/* /// "psdSetAltInterface()" */
BOOL (psdSetAltInterface)(struct PsdPipe * pp asm("a1"), struct PsdInterface * pif asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdConfig *pc = pif->pif_Config;
    struct PsdInterface *curif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
    struct PsdInterface *tmpif;
    struct PsdDevice *pd = pc->pc_Device;
    UBYTE buf[8]; // VIA babble bug safety buffer (8 instead of 2)
    LONG ioerr;
    UWORD ifnum = pif->pif_IfNum;
    UWORD altnum = pif->pif_Alternate;

    KPRINTF(2, ("Setting interface %ld to alt %ld...\n", ifnum, altnum));
    psdLockWriteDevice(pd);

    /* Find root config structure */
    while(curif->pif_Node.ln_Succ) {
        if(curif->pif_IfNum == ifnum) {
            break;
        }
        curif = (struct PsdInterface *) curif->pif_Node.ln_Succ;
    }
    if(!curif->pif_Node.ln_Succ) {
        KPRINTF(20, ("Interface %ld not found in this config!\n", ifnum));
        psdUnlockDevice(pd);
        return(FALSE);
    }
    if(curif == pif) { /* Is already the current alternate setting */
        psdUnlockDevice(pd);
        return(TRUE);
    }
    KPRINTF(1, ("really setting interface...\n"));
    if(pp) {
        /* backend adjusts endpoint contexts first (context HCDs: add/drop
           sets; legacy: no-op) - the wire SET_INTERFACE follows */
        ioerr = pd->pd_Hardware->phw_HCDOps->hop_SetInterface(ps, pp, curif, pif);
        if(ioerr) {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Endpoint reconfiguration (if %ld alt %ld) failed: %s (%ld)",
                           ifnum, altnum,
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            psdUnlockDevice(pd);
            return(FALSE);
        }
        psdPipeSetup(pp, URTF_STANDARD|URTF_INTERFACE,
                     USR_SET_INTERFACE, altnum, ifnum);
        ioerr = psdDoPipe(pp, NULL, 0);
    } else {
        ioerr = 0;
    }
    if((!ioerr) || (ioerr == UHIOERR_STALL)) {
        if(pp) {
            psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_INTERFACE,
                         USR_GET_INTERFACE, 0, ifnum);
            ioerr = psdDoPipe(pp, buf, 1);
        } else {
            buf[0] = altnum;
        }
        if(!ioerr) {
            if(altnum == buf[0]) {
                KPRINTF(1, ("resorting list..."));
                /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Changed to alt %ld",
                               altnum);*/
                Forbid();
                /* Remove pif from alt list */
                Remove(&pif->pif_Node);
                pif->pif_ParentIf = NULL;
                /* Now move bindings */
                pif->pif_ClsBinding = curif->pif_ClsBinding;
                pif->pif_IfBinding = curif->pif_IfBinding;
                curif->pif_IfBinding = NULL;
                curif->pif_ClsBinding = NULL;
                /* Insert it after root interface */
                Insert(&pc->pc_Interfaces, (struct Node *) &pif->pif_Node, (struct Node *) &curif->pif_Node);
                /* Unlink root interface */
                Remove(&curif->pif_Node);
                /* Now move all remaining alt interfaces to the new root interface */
                tmpif = (struct PsdInterface *) curif->pif_AlterIfs.lh_Head;
                while(tmpif->pif_Node.ln_Succ) {
                    Remove(&tmpif->pif_Node);
                    AddTail(&pif->pif_AlterIfs, &tmpif->pif_Node);
                    tmpif->pif_ParentIf = pif;
                    tmpif = (struct PsdInterface *) curif->pif_AlterIfs.lh_Head;
                }
                /* Add old root to the end of the alt list */
                AddTail(&pif->pif_AlterIfs, &curif->pif_Node);
                curif->pif_ParentIf = pif;
                Permit();
                psdUnlockDevice(pd);
                return(TRUE);
            } else {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Attempt to change interface %ld to alt %ld remained at alt %ld.",
                               ifnum, altnum, buf[0]);
            }
        } else {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "GET_INTERFACE(%ld) failed: %s (%ld)",
                           ifnum, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            KPRINTF(15, ("GET_INTERFACE failed %ld!\n", ioerr));
        }
    } else {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "SET_INTERFACE(%ld)=%ld failed: %s (%ld)",
                       ifnum, altnum,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(15, ("SET_INTERFACE failed %ld!\n", ioerr));
    }
    if(pp) {
        /* The switch did not take place, or cannot be told to have: the old
           alternate stays the current one here, so the controller takes its
           endpoints back - otherwise the two disagree on which endpoints
           exist, and the next call for the old alternate changes nothing. */
        ioerr = pd->pd_Hardware->phw_HCDOps->hop_SetInterface(ps, pp, pif, curif);
        if(ioerr) {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Endpoint reconfiguration back to (if %ld alt %ld) failed: %s (%ld)",
                           ifnum, (ULONG) curif->pif_Alternate,
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        }
    }
    psdUnlockDevice(pd);
    return(FALSE);
}
/* \\\ */

static void DumpPipe(struct PsdPipe *pp)
{
    struct PsdDevice   *pd  = pp ? pp->pp_Device   : NULL;
    struct PsdEndpoint *pep = pp ? pp->pp_Endpoint : NULL;

    Disable();
    KPRINTF(15, ("--- PIPE DUMP ---\n"));
    KPRINTF(15, ("Pipe:      0x%08lx\n", pp));
    KPRINTF(15, ("  MsgPort: 0x%08lx  Num=%lu  Flags=%04lx\n",
                 pp ? pp->pp_MsgPort : NULL,
                 pp ? (ULONG)pp->pp_Num : 0,
                 pp ? (ULONG)pp->pp_Flags : 0));

    if (pd) {
        const STRPTR hwName  = (pd->pd_Hardware && pd->pd_Hardware->phw_DevName)
                               ? pd->pd_Hardware->phw_DevName
                               : (STRPTR)"(n/a)";
        LONG hwUnit          = pd->pd_Hardware ? pd->pd_Hardware->phw_Unit : -1;
        const STRPTR prodStr = pd->pd_ProductStr ? pd->pd_ProductStr : (STRPTR)"(no product)";
        const STRPTR mnfStr  = pd->pd_MnfctrStr ? pd->pd_MnfctrStr  : (STRPTR)"(no mfg)";
        const STRPTR idStr   = pd->pd_IDString ? pd->pd_IDString    : (STRPTR)"(no ID string)";

        KPRINTF(15, ("Device:    0x%08lx  DevName=%s Unit=%ld\n",
                     pd, hwName, hwUnit));
        KPRINTF(15, ("  Addr=%lu  Hub=0x%08lx Port=%lu Flags=%04lx\n",
                     (ULONG)pd->pd_DevAddr,
                     pd->pd_Hub,
                     (unsigned)pd->pd_HubPort,
                     (unsigned)pd->pd_Flags));
        KPRINTF(15, ("  USBVers=%04lx  Class=%lu SubClass=%lu Proto=%lu\n",
                     (unsigned)pd->pd_USBVers,
                     (unsigned)pd->pd_DevClass,
                     (unsigned)pd->pd_DevSubClass,
                     (unsigned)pd->pd_DevProto));
        KPRINTF(15, ("  VID:PID=%04lx:%04lx  DevVers=%04lx  MaxPkt0=%lu\n",
                     (unsigned)pd->pd_VendorID,
                     (unsigned)pd->pd_ProductID,
                     (unsigned)pd->pd_DevVers,
                     (unsigned)pd->pd_MaxPktSize0));
        KPRINTF(15, ("  Product=\"%s\"  Manufacturer=\"%s\"\n",
                     prodStr, mnfStr));
        KPRINTF(15, ("  IDString=\"%s\"\n", idStr));

#ifdef HUB_CLASSCODE
        if (pd->pd_DevClass == HUB_CLASSCODE) {
            KPRINTF(15, ("  (This device is a HUB)\n"));
        }
#endif

        /* Walk upstream hub chain to show physical location */
        if (pd->pd_Hub) {
            struct PsdDevice *child = pd;
            struct PsdDevice *hub   = pd->pd_Hub;
            UWORD             level = 0;

            KPRINTF(15, ("Hub path (child -> root):\n"));
            while (hub) {
                const STRPTR hubName  = (hub->pd_Hardware && hub->pd_Hardware->phw_DevName)
                                        ? hub->pd_Hardware->phw_DevName
                                        : (STRPTR)"(n/a)";
                LONG hubUnit          = hub->pd_Hardware ? hub->pd_Hardware->phw_Unit : -1;
                const STRPTR hubProd  = hub->pd_ProductStr ? hub->pd_ProductStr : (STRPTR)"(no product)";

                KPRINTF(15, (
                            "  lvl %lu: HUB=0x%08lx Addr=%lu DevName=%s Unit=%ld "
                            "Port(child=%lu) Prod=\"%s\"\n",
                            (unsigned)level,
                            hub,
                            (ULONG)hub->pd_DevAddr,
                            hubName,
                            hubUnit,
                            (unsigned)child->pd_HubPort,
                            hubProd));

                /* Move one level up: this hub is now the child of the next hub */
                child = hub;
                hub   = hub->pd_Hub;
                level++;
            }
        } else {
            KPRINTF(15, ("Hub path: (device is directly attached to root)\n"));
        }

    } else {
        KPRINTF(15, ("Device:    (NULL)\n"));
    }

    KPRINTF(15, ("IOReq:\n"));
    if (pp) {
        KPRINTF(15, ("  DevAddr=%lu  MaxPktSize=%lu  Flags=%08lx  NakTimeout=%lu\n",
                     (ULONG)pp->pp_IOReq.iouh_DevAddr,
                     (ULONG)pp->pp_IOReq.iouh_MaxPktSize,
                     (ULONG)pp->pp_IOReq.iouh_Flags,
                     (ULONG)pp->pp_IOReq.iouh_NakTimeout));
        KPRINTF(15, ("  SplitHubAddr=%lu  SplitHubPort=%lu  Handle=%08lx  StreamID=%lu\n",
                     (unsigned)pp->pp_IOReq.iouh_SplitHubAddr,
                     (unsigned)pp->pp_IOReq.iouh_SplitHubPort,
                     (ULONG)pp->pp_Device->pd_Handle,
                     (ULONG)pp->pp_StreamID));
    } else {
        KPRINTF(15, ("  (no IOReq, pipe is NULL)\n"));
    }

    if (pep) {
        KPRINTF(15, ("Endpoint:\n"));
        KPRINTF(15, ("  EPNum=%lu  Dir=%lu  Type=%lu  MaxPktSize=%lu  Interval=%lu\n",
                     (unsigned)pep->pep_EPNum,
                     (unsigned)pep->pep_Direction,
                     (unsigned)pep->pep_TransType,
                     (unsigned)pep->pep_MaxPktSize,
                     (unsigned)pep->pep_Interval));
        KPRINTF(15, ("  NumTransMuFr=%lu  SyncType=%lu  UsageType=%lu\n",
                     (unsigned)pep->pep_NumTransMuFr,
                     (unsigned)pep->pep_SyncType,
                     (unsigned)pep->pep_UsageType));
        KPRINTF(15, ("  MaxBurst=%lu  CompAttr=%lu  BytesPerInterval=%lu\n",
                     (unsigned)pep->pep_MaxBurst,
                     (unsigned)pep->pep_CompAttributes,
                     (ULONG)pep->pep_BytesPerInterval));
    } else {
        KPRINTF(15, ("Endpoint:  default (EP0)\n"));
    }

    KPRINTF(15, ("--- END PIPE DUMP ---\n"));
    Enable();
}


static void pLogPipe(struct PsdPipe *pp)
{
    Disable();
    if (pp) {
        struct PsdDevice   *pd  = pp->pp_Device;
        struct PsdEndpoint *pep = pp->pp_Endpoint;

        struct PsdBase * ps = pd->pd_Hardware->phw_Base;

        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
	                   "Pipe %lu flags %04lx",
	                   (ULONG)pp->pp_Num,
	                   (ULONG)pp->pp_Flags);
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
	                   "  DevAddr=%lu  MaxPktSize=%lu  Flags=%08lx  NakTimeout=%lu\n",
                       (ULONG)pp->pp_IOReq.iouh_DevAddr,
                       (ULONG)pp->pp_IOReq.iouh_MaxPktSize,
                       (ULONG)pp->pp_IOReq.iouh_Flags,
                       (ULONG)pp->pp_IOReq.iouh_NakTimeout);
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
	                   "  SplitHubAddr=%lu  SplitHubPort=%lu  Handle=%08lx  StreamID=%lu\n",
                       (unsigned)pp->pp_IOReq.iouh_SplitHubAddr,
                       (unsigned)pp->pp_IOReq.iouh_SplitHubPort,
                       (ULONG)pp->pp_Device->pd_Handle,
                       (ULONG)pp->pp_StreamID);

        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Device MaxPktSize0=%lu",
	                   (unsigned)pd->pd_MaxPktSize0);

        if (pep) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
	                   "Endpoint %lu  Dir=%lu  Type=%lu  MaxPktSize=%lu  Interval=%lu\n",
                       (unsigned)pep->pep_EPNum,
                       (unsigned)pep->pep_Direction,
                       (unsigned)pep->pep_TransType,
                       (unsigned)pep->pep_MaxPktSize,
                       (unsigned)pep->pep_Interval);
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
	                   "  NumTransMuFr=%lu  SyncType=%lu  UsageType=%lu\n",
                       (unsigned)pep->pep_NumTransMuFr,
                       (unsigned)pep->pep_SyncType,
                       (unsigned)pep->pep_UsageType);
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
	                   "  MaxBurst=%lu  CompAttr=%lu  BytesPerInterval=%lu\n",
                       (unsigned)pep->pep_MaxBurst,
                       (unsigned)pep->pep_CompAttributes,
                       (ULONG)pep->pep_BytesPerInterval);
        }
    }
    Enable();
}

/*
 * Map BOS-derived capabilities into Neptune's device structure.
  */
static void
pApplyDeviceBosCapabilities(struct PsdDevice *pd, const struct PsdBosCaps *caps)
{
    if (!pd || !caps || !caps->hasBos)
        return;

    /* USB 2.0 LPM (L1) + BESL (USB2 LPM ECN) */
    if (caps->hasUsb20Ext) {
        pd->pd_Usb20LpmCapable        = caps->usb20LpmCapable;
        pd->pd_Usb20BeslCapable       = caps->usb20BeslCapable;
        pd->pd_Usb20BeslBaselineValid = caps->usb20BeslBaselineValid;
        pd->pd_Usb20BeslBaseline      = caps->usb20BeslBaseline;
        pd->pd_Usb20BeslDeepValid     = caps->usb20BeslDeepValid;
        pd->pd_Usb20BeslDeep          = caps->usb20BeslDeep;
    }

    /* SuperSpeed device capability */
    if (caps->hasSSCap) {
        pd->pd_Usb30LtmCapable   = (caps->ssBmAttributes & 0x02) ? TRUE : FALSE;
        pd->pd_SupportedSpeeds  |= caps->ssSpeedsSupported;
        pd->pd_Usb30U1ExitLat    = caps->ssU1DevExitLat;
        pd->pd_Usb30U2ExitLat    = caps->ssU2DevExitLat;

        /* Choose the highest supported speed */
        if (caps->ssSpeedsSupported & (1U << 3)) {          /* SuperSpeed */
            pd->pd_MaxUsbSpeed = 4;
        } else if (caps->ssSpeedsSupported & (1U << 2)) {   /* High-speed */
            pd->pd_MaxUsbSpeed = 3;
        } else if (caps->ssSpeedsSupported & (1U << 1)) {   /* Full-speed */
            pd->pd_MaxUsbSpeed = 2;
        } else if (caps->ssSpeedsSupported & (1U << 0)) {   /* Low-speed */
            pd->pd_MaxUsbSpeed = 1;
        }
    }

    /* Container ID */
    if (caps->hasContainerId) {
        pd->pd_HasContainerId = TRUE;
        CopyMem(caps->containerId, pd->pd_ContainerId, 16);
    }
}

static BOOL
pParseBosDescriptor(const UBYTE *bosbuf, UWORD bosbuflen,
                    struct PsdBosCaps *boscaps)
{
    const struct UsbStdBOSDesc *bosHdr;
    UWORD offset;

    if (!bosbuf || !boscaps || bosbuflen < sizeof(struct UsbStdBOSDesc)) {
        return FALSE;
    }

    memset(boscaps, 0, sizeof(*boscaps));
    boscaps->hasBos = TRUE;

    bosHdr = (const struct UsbStdBOSDesc *)bosbuf;
    offset = bosHdr->bLength;
    while (offset + 3 < bosbuflen) {
        UBYTE bLength         = bosbuf[offset + 0];
        UBYTE bDescriptorType = bosbuf[offset + 1];

        if (!bLength) {
            XPRINTF(1, ("BOS: zero-length descriptor at offset %lu, aborting\n",
                        (unsigned)offset));
            break;
        }
        if (offset + bLength > bosbuflen) {
            XPRINTF(1, ("BOS: descriptor len %lu overruns buffer (%lu), aborting\n",
                        (unsigned)bLength,
                        (unsigned)bosbuflen));
            break;
        }

        if (bDescriptorType == UDT_DEVICE_CAPABILITY) {
            UBYTE bDevCapType = bosbuf[offset + 2];
            const UBYTE *p = &bosbuf[offset];

            switch (bDevCapType) {

            case UDC_USB20_EXTENSION:
                if (bLength >= sizeof(struct Usb20ExtDesc)) {
                    struct Usb20ExtDesc *ext = (struct Usb20ExtDesc *)p;
                    ULONG attrs = AROS_LE2LONG(ext->bmAttributes);

                    boscaps->hasUsb20Ext           = TRUE;
                    boscaps->usb20LpmCapable        = (attrs & U20EA_LPM) ? TRUE : FALSE;
                    boscaps->usb20BeslCapable       = (attrs & U20EA_BESL) ? TRUE : FALSE;
                    boscaps->usb20BeslBaselineValid = (attrs & U20EA_BASELINE_VALID) ? TRUE : FALSE;
                    boscaps->usb20BeslBaseline      = (UBYTE)((attrs >> U20EA_BASELINE_SHIFT) & U20EA_BESL_MASK);
                    boscaps->usb20BeslDeepValid     = (attrs & U20EA_DEEP_VALID) ? TRUE : FALSE;
                    boscaps->usb20BeslDeep          = (UBYTE)((attrs >> U20EA_DEEP_SHIFT) & U20EA_BESL_MASK);

                    XPRINTF(2, ("BOS: USB2.0 ext: bmAttributes=0x%08lx LPM=%ld BESL=%ld "
                                "base=%ld/%ld deep=%ld/%ld\n",
                                attrs,
                                (LONG)boscaps->usb20LpmCapable,
                                (LONG)boscaps->usb20BeslCapable,
                                (LONG)boscaps->usb20BeslBaselineValid,
                                (LONG)boscaps->usb20BeslBaseline,
                                (LONG)boscaps->usb20BeslDeepValid,
                                (LONG)boscaps->usb20BeslDeep));
                } else {
                    XPRINTF(1, ("BOS: USB2.0 ext cap too small (len=%lu)\n",
                                (unsigned)bLength));
                }
                break;

            case UDC_SUPERSPEED_USB:
                if (bLength >= sizeof(struct UsbSSDevCapDesc)) {
                    struct UsbSSDevCapDesc *ss =
                        (struct UsbSSDevCapDesc *)p;

                    boscaps->hasSSCap              = TRUE;
                    boscaps->ssBmAttributes        = ss->bmAttributes;
                    boscaps->ssSpeedsSupported     =
                        AROS_LE2WORD(ss->wSpeedSupported);
                    boscaps->ssLowestFullFuncSpeed =
                        ss->bFunctionalitySupport;
                    boscaps->ssU1DevExitLat        = ss->bU1DevExitLat;
                    boscaps->ssU2DevExitLat        =
                        AROS_LE2WORD(ss->bU2DevExitLat);

                    XPRINTF(2, ("BOS: SS DevCap: speeds=0x%04lx, attr=0x%02lx, U1=%lu, U2=%lu\n",
                                (unsigned)boscaps->ssSpeedsSupported,
                                (unsigned)boscaps->ssBmAttributes,
                                (unsigned)boscaps->ssU1DevExitLat,
                                (unsigned)boscaps->ssU2DevExitLat));
                } else {
                    XPRINTF(1, ("BOS: SS DevCap too small (len=%lu)\n",
                                (unsigned)bLength));
                }
                break;

            case UDC_CONTAINER_ID:
                /* Container ID is 16 bytes, descriptor is 20 bytes total */
                if (bLength >= 20) {
                    boscaps->hasContainerId = TRUE;
                    CopyMem(p + 4, boscaps->containerId, 16);
                    XPRINTF(2, ("BOS: Container ID present\n"));
                } else {
                    XPRINTF(1, ("BOS: Container ID cap too small (len=%lu)\n",
                                (unsigned)bLength));
                }
                break;

            default:
                XPRINTF(2, ("BOS: ignoring devcap type %lu (len=%lu)\n",
                            (unsigned)bDevCapType,
                            (unsigned)bLength));
                break;
            }
        }

        offset += bLength;
    }

    return TRUE;
}

static BOOL
pFetchBosCaps(struct PsdPipe *pp, struct PsdBosCaps *boscaps)
{
    struct PsdBase * ps = pp->pp_Device->pd_Hardware->phw_Base;
    struct UsbStdBOSDesc bosHdr;
    LONG ioerr_bos;
    UWORD bosTotalLength;
    UWORD toRead;
    UBYTE bosbuf[256];

    /* First, read just the BOS header */
    psdPipeSetup(pp,
                 URTF_IN|URTF_STANDARD|URTF_DEVICE,
                 USR_GET_DESCRIPTOR,
                 UDT_BOS << 8,
                 0);
    ioerr_bos = psdDoPipe(pp, &bosHdr, sizeof(struct UsbStdBOSDesc));
    if (ioerr_bos) {
        XPRINTF(1, ("GET_DESCRIPTOR (BOS header) failed %ld\n", ioerr_bos));
        return FALSE;
    }

    bosTotalLength = AROS_LE2WORD(bosHdr.wTotalLength);

    XPRINTF(1, ("BOS header: len=%lu, numCaps=%lu, totalLen=%lu\n",
                (unsigned)bosHdr.bLength,
                (unsigned)bosHdr.bNumDeviceCaps,
                (unsigned)bosTotalLength));

    if (bosTotalLength < bosHdr.bLength) {
        XPRINTF(1, ("BOS wTotalLength=%lu smaller than header %lu, clamping\n",
                    (unsigned)bosTotalLength,
                    (unsigned)bosHdr.bLength));
        bosTotalLength = bosHdr.bLength;
    }

    /* Fetch the full BOS (bounded to bosbuf) */
    toRead = bosTotalLength;
    if (toRead > sizeof(bosbuf)) {
        XPRINTF(1, ("BOS wTotalLength=%lu too large, truncating to %lu bytes\n",
                    (unsigned)bosTotalLength,
                    (unsigned)sizeof(bosbuf)));
        toRead = sizeof(bosbuf);
    }

    psdPipeSetup(pp,
                 URTF_IN|URTF_STANDARD|URTF_DEVICE,
                 USR_GET_DESCRIPTOR,
                 UDT_BOS << 8,
                 0);
    ioerr_bos = psdDoPipe(pp, bosbuf, toRead);
    if (ioerr_bos) {
        XPRINTF(1, ("GET_DESCRIPTOR (BOS full, len=%lu) failed %ld\n",
                    (unsigned)toRead, ioerr_bos));
        return FALSE;
    }

    XPRINTF(1, ("Full BOS descriptor (%lu bytes) received\n",
                (unsigned)toRead));

    /* Ensure header is at the front of the buffer */
    memcpy(bosbuf, &bosHdr, sizeof(struct UsbStdBOSDesc));

    return pParseBosDescriptor(bosbuf, toRead, boscaps);
}

static UWORD
pGetMaxStreamsForEndpoint(const struct PsdEndpoint *pep)
{
    const struct PsdDevice *pd;
    UBYTE n;

    if (!pep) {
        return 0;
    }

    pd = pep->pep_Interface->pif_Config->pc_Device;
    if (!(pd->pd_Flags & PDFF_SUPERSPEED)) {
        return 0;
    }

    if (pep->pep_TransType != USEAF_BULK) {
        return 0;
    }

    n = (UBYTE)(pep->pep_CompAttributes & 0x1F);
    if (n == 0) {
        return 0;
    }

    if (n >= 16) {
        return 0xFFFF;
    }

    return (UWORD)(1U << n);
}

/* /// "Legacy HCD backend" */
/*
 * The legacy lower-edge backend: classic software-managed addressing
 * Endpoint configuration is implicit on this backend (the HCD learns
 * everything from the wire), so most hooks are no-ops. A context backend
 * (HCD-owned addressing via the context HCD ABI) will be bound for HCDs
 * advertising UHCF_CONTEXT instead. */

static LONG pLegacyAddressDevice(struct PsdBase *ps, struct PsdPipe *pp, struct UsbStdDevDesc *usdd)
{
    struct PsdDevice *pd = pp->pp_Device;
    LONG ioerr;
    IPTR islowspeed = 0;
    IPTR ishighspeed = 0;

    if(!pAllocDevAddr(pd)) {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR) libname,
                        "No free device address: more than 127 devices on the bus.");
        KPRINTF(20, ("out of addresses???\n"));
        return(UHIOERR_OUTOFMEMORY);
    }

    pp->pp_IOReq.iouh_DevAddr = 0;

    /*
     * Initial EP0 max packet size depends on device speed:
     *
     *  - Low-Speed (USB 1.x):        8 bytes (fixed)
     *  - Full-Speed (USB 1.x):       Start with 8 bytes; actual size
     *                                (8/16/32/64) is learned from
     *                                bMaxPacketSize0 after the first
     *                                GET_DESCRIPTOR(8)
     *  - High-Speed (USB 2.0):       64 bytes (fixed for EP0)
     *  - SuperSpeed / SuperSpeed+:   512 bytes (fixed for EP0)
     */
    psdGetAttrs(PGA_DEVICE, pd,
                DA_IsLowspeed,  &islowspeed,
                DA_IsHighspeed, &ishighspeed,
                TAG_END);

    if (ishighspeed) {
        pp->pp_IOReq.iouh_MaxPktSize = 64;
        pp->pp_IOReq.iouh_Flags |= UHFF_HIGHSPEED;
    } else {
        /* FS or LS initial */
        pp->pp_IOReq.iouh_MaxPktSize = 8; /* LS fixed 8, FS starts 8 */
        if(islowspeed) {
            pp->pp_IOReq.iouh_Flags |= UHFF_LOWSPEED;
        }
    }

    psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE, USR_GET_DESCRIPTOR, UDT_DEVICE<<8, 0);
    ioerr = psdDoPipe(pp, usdd, 8);
    if(ioerr && (ioerr != UHIOERR_RUNTPACKET)) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "%s/%ld GET_DESCRIPTOR (8) failed: %s (%ld)",
                       pd->pd_Hardware->phw_DevName, pd->pd_Hardware->phw_Unit,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(15, ("%s/%ld GET_DESCRIPTOR (8) failed %ld!\n",
                     pd->pd_Hardware->phw_DevName, pd->pd_Hardware->phw_Unit, ioerr));
        pLogPipe(pp);
        DumpPipe(pp);

        /*
         * Do not continue: usdd may be incomplete/invalid, and using it to
         * select MaxPktSize0 or class/hub behaviour can lock up the device.
         */
        return(ioerr);
    }

    KPRINTF(1, ("Setting DevAddr %ld...\n", pd->pd_DevAddr));
    psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE, USR_SET_ADDRESS, pd->pd_DevAddr, 0);
    ioerr = psdDoPipe(pp, NULL, 0);

    /*
        This is tricky: Maybe the device has accepted the command,
        but failed to send an ACK. Now, every resend trial will
        go to the wrong address!
    */
    if((ioerr == UHIOERR_TIMEOUT) || (ioerr == UHIOERR_STALL)) {
        KPRINTF(1, ("First attempt failed, retrying new address\n"));
        psdDelayMS(250);
        ioerr = psdDoPipe(pp, NULL, 0);
    }

    if(ioerr) {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "SET_ADDRESS(%ld) failed: %s (%ld)",
                       pd->pd_DevAddr, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(15, ("SET_ADDRESS(%ld) failed %ld!\n", pd->pd_DevAddr, ioerr));
        DumpPipe(pp);
        return(ioerr);
    }

    /* Address is now active */
    pd->pd_Flags |= PDFF_HASDEVADDR|PDFF_CONNECTED;
    pp->pp_IOReq.iouh_DevAddr = pd->pd_DevAddr;
    pd->pd_Handle = pd->pd_DevAddr;

    psdDelayMS(50); /* Allowed time to settle */

    return(0);
}

static LONG pLegacyUpdateEp0MaxPacket(struct PsdBase *ps, struct PsdPipe *pp)
{
    /* implicit on the legacy edge: the HCD picks it up from the pipe */
    return(0);
}

static LONG pLegacyConfigureEndpoints(struct PsdBase *ps, struct PsdPipe *pp, UWORD cfgnum)
{
    /* implicit on the legacy edge: the HCD infers endpoints per transfer */
    return(0);
}

static LONG pLegacySetInterface(struct PsdBase *ps, struct PsdPipe *pp, struct PsdInterface *curif, struct PsdInterface *pif)
{
    /* implicit on the legacy edge */
    return(0);
}

static void pLegacyUpdateHub(struct PsdBase *ps, struct PsdDevice *pd)
{
    /* implicit on the legacy edge: split/TT facts travel per transfer */
}

static void pLegacyDestroyDevice(struct PsdBase *ps, struct PsdDevice *pd)
{
    if(pd->pd_DevAddr) {
        KPRINTF(5,("Released DevAddr %ld\n", pd->pd_DevAddr));
        pd->pd_Hardware->phw_DevArray[pd->pd_DevAddr] = NULL;
    }
}

static const struct PsdHCDOps pLegacyHCDOps =
{
    pLegacyAddressDevice,
    pLegacyUpdateEp0MaxPacket,
    pLegacyConfigureEndpoints,
    pLegacySetInterface,
    pLegacyUpdateHub,
    pLegacyDestroyDevice,
};
/* \\\ */

/* /// "Context HCD backend" */
/*
 * The context lower-edge backend: the HCD owns addressing and endpoint contexts;
 * the stack drives them with explicit NSCMD_USB_* lifecycle ops.
 * No software-visible default-address phase exists - CREATE_DEVICE is atomic in
 * the driver's unit task - and transfers are keyed by an opaque device handle
 * instead of a bus address.
 *
 * The ops travel through the regular pipe machinery (pSubmitPipeReq/
 * psdWaitPipe) so they work from any task and honor quick-I/O.  They are
 * housekeeping, not device IO: they never call pActivityBegin(), so they can
 * never End - no pd_IOBusyCount, no pd_LastActivity stamp.
 */

static void pSubmitPipeReq(struct PsdPipe *pp, struct IORequest *ioreq, struct PsdBase *ps);
static void pCompletePipe(struct PsdPipe *pp);
static void pStallRecoverySweep(struct PsdBase *ps);

static LONG pCtxDoOp(struct PsdBase *ps, struct PsdPipe *pp, UWORD cmd, APTR op, ULONG len)
{
    struct IOStdReq *sio = &pp->pp_Ctx.ppc_Std;

    sio->io_Message = pp->pp_IOReq.iouh_Req.io_Message;
    sio->io_Message.mn_Node.ln_Name = (char *) pp; /* completion demux */
    sio->io_Message.mn_Length = sizeof(struct IOStdReq);
    sio->io_Device = pp->pp_IOReq.iouh_Req.io_Device;
    sio->io_Unit = pp->pp_IOReq.iouh_Req.io_Unit;
    sio->io_Command = cmd;
    sio->io_Flags = 0;
    sio->io_Error = 0;
    sio->io_Actual = 0;
    sio->io_Data = op;
    sio->io_Length = len;
    sio->io_Offset = 0;
    pSubmitPipeReq(pp, (struct IORequest *) sio, ps);
    return(psdWaitPipe(pp));
}

/* Lifecycle ops without a caller-supplied pipe (update-hub from psdSetAttrs,
   destroy from pFreeDevice) get a short-lived EP0 pipe of their own. */
static LONG pCtxDoOpOnDevice(struct PsdBase *ps, struct PsdDevice *pd, UWORD cmd, APTR op, ULONG len)
{
    struct MsgPort *mp;
    struct PsdPipe *pp;
    LONG ioerr = UHIOERR_OUTOFMEMORY;

    if((mp = CreateMsgPort())) {
        if((pp = psdAllocPipe(pd, mp, NULL))) {
            ioerr = pCtxDoOp(ps, pp, cmd, op, len);
            psdFreePipe(pp);
        }
        DeleteMsgPort(mp);
    }
    return(ioerr);
}

/* SetAddress recovery (USB 2.0 9.2.6.3: 2 ms). The HCD's Address Device puts
   SET_ADDRESS on the wire and completes right after its status stage; a device
   need not answer at the new address before the interval is over. */
#define CTX_SETADDRESS_RECOVERY_MS 10

static LONG pContextAddressDevice(struct PsdBase *ps, struct PsdPipe *pp, struct UsbStdDevDesc *usdd)
{
    struct PsdDevice *pd = pp->pp_Device;
    struct UhcdCreateDevice cdo;
    LONG ioerr;

    memset(&cdo, 0, sizeof(cdo));
    /* hub.class sets DA_HubDevice/DA_AtHubPortNumber/speed attrs before
       enumeration, so parent and port are already on the device */
    if(pd->pd_Hub) {
        cdo.cdo_ParentHandle = pd->pd_Hub->pd_Handle;
    } /* else 0 = this is the root hub itself */
    cdo.cdo_HubPort = pd->pd_HubPort;

    if(pd->pd_Flags & PDFF_SUPERSPEED) {
        cdo.cdo_Speed = UHCD_SPEED_SUPER;
    } else if(pd->pd_Flags & PDFF_HIGHSPEED) {
        cdo.cdo_Speed = UHCD_SPEED_HIGH;
    } else if(pd->pd_Flags & PDFF_LOWSPEED) {
        cdo.cdo_Speed = UHCD_SPEED_LOW;
    } else {
        cdo.cdo_Speed = UHCD_SPEED_FULL;
    }

    if(pd->pd_Flags & PDFF_NEEDSSPLIT) {
        UWORD ttport = 0;
        struct PsdDevice *tthub = pFindTTHub(pd, &ttport);
        if(!tthub) {
            psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname,
                            "Internal error obtaining split transaction hub!");
            return(UHIOERR_BADPARAMS);
        }
        cdo.cdo_TTHubHandle = tthub->pd_Handle;
        cdo.cdo_TTPort = ttport;
        cdo.cdo_TTThinkTime = tthub->pd_HubThinkTime;
    }

    ioerr = pCtxDoOp(ps, pp, NSCMD_USB_CREATE_DEVICE, &cdo, sizeof(cdo));
    if(ioerr) {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "CREATE_DEVICE (%s/%ld port %ld) failed: %s (%ld)",
                       pd->pd_Hardware->phw_DevName, pd->pd_Hardware->phw_Unit,
                       (ULONG) pd->pd_HubPort,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return(ioerr);
    }

    pd->pd_Handle = cdo.cdo_DeviceHandle;
    pd->pd_Ep0Token = cdo.cdo_Ep0Token;
    pd->pd_DevAddr = 0; /* no bus address on this backend */
    pd->pd_Flags |= PDFF_HASDEVADDR|PDFF_CONNECTED;

    if(pd->pd_Hub) { /* root hubs are emulated: no SET_ADDRESS on a wire */
        psdDelayMS(CTX_SETADDRESS_RECOVERY_MS);
    }

    KPRINTF(1, ("Created device handle 0x%08lx, reading first 8 descriptor bytes...\n", pd->pd_Handle));
    psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE, USR_GET_DESCRIPTOR, UDT_DEVICE<<8, 0);
    ioerr = psdDoPipe(pp, usdd, 8);
    if(ioerr && (ioerr != UHIOERR_RUNTPACKET)) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "%s/%ld GET_DESCRIPTOR (8) failed: %s (%ld)",
                       pd->pd_Hardware->phw_DevName, pd->pd_Hardware->phw_Unit,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        pLogPipe(pp);
        DumpPipe(pp);
        /* keep pd_Handle: hop_DestroyDevice releases the slot when the
           device is torn down */
        return(ioerr);
    }
    return(0);
}

static LONG pContextUpdateEp0MaxPacket(struct PsdBase *ps, struct PsdPipe *pp)
{
    struct PsdDevice *pd = pp->pp_Device;
    struct UhcdUpdateEp0 ueo;
    LONG ioerr;

    ueo.ueo_DeviceHandle = pd->pd_Handle;
    ueo.ueo_Ep0MaxPkt = pd->pd_MaxPktSize0; /* validated per speed by psdEnumerateDevice */
    ueo.ueo_Pad = 0;

    ioerr = pCtxDoOp(ps, pp, NSCMD_USB_UPDATE_EP0, &ueo, sizeof(ueo));
    if(ioerr) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "UPDATE_EP0 (%ld bytes) failed: %s (%ld)",
                       (ULONG) pd->pd_MaxPktSize0,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
    }
    return(ioerr);
}

static void pCtxFillEndpointDesc(struct UhcdEndpointDesc *ed, struct PsdInterface *pif,
                                 struct PsdEndpoint *pep, BOOL ishighspeed)
{
    UWORD mps = pep->pep_MaxPktSize;

    /* HS periodic endpoints carry the extra-transaction count in
       wMaxPacketSize bits 12:11; SS mult travels in the companion instead */
    if(ishighspeed && (pep->pep_NumTransMuFr > 1) &&
       ((pep->pep_TransType == USEAF_INTERRUPT) || (pep->pep_TransType == USEAF_ISOCHRONOUS))) {
        mps |= (pep->pep_NumTransMuFr - 1) << 11;
    }

    ed->ed_Address = pep->pep_EPNum | (pep->pep_Direction ? 0x80 : 0x00);
    ed->ed_Type = pep->pep_TransType;
    ed->ed_MaxPacket = mps;
    ed->ed_Interval = pep->pep_IntervalRaw;
    ed->ed_MaxBurst = pep->pep_MaxBurst ? pep->pep_MaxBurst - 1 : 0; /* raw bMaxBurst */
    ed->ed_Mult = (pep->pep_TransType == USEAF_ISOCHRONOUS) ? (pep->pep_CompAttributes & 0x03) : 0;
    ed->ed_IfClass = pif->pif_IfClass; /* REQUIRED when known: controller quirks key on it */
    ed->ed_BytesPerInterval = min(pep->pep_BytesPerInterval, 0xFFFF);
    ed->ed_MaxStreams = pep->pep_MaxStreams;
}

/* The transfer completion hook (usbhcd_context.h "The transfer path").
   Every context transfer is a direct submit() in the caller's context - no
   wire IORequest, no relay round trip; the HCD completes it by calling this
   hook from its unit task.  It writes the results into pp_IOReq and replies
   pp_Msg, so psdWaitPipe/psdCheckPipe and every consumer stay path-agnostic. */

static void pXferDoneHook(struct Hook *hook asm("a0"), APTR obj asm("a2"), struct UhcdXferDone *uxd asm("a1"))
{
    struct PsdPipe *pp = (struct PsdPipe *) uxd->uxd_Cookie;

    (void) hook;
    (void) obj;
    pp->pp_IOReq.iouh_Actual = uxd->uxd_Actual;
    pp->pp_IOReq.iouh_ExtError = uxd->uxd_ExtError;
    pp->pp_IOReq.iouh_Req.io_Error = (BYTE) uxd->uxd_Error;
    pCompletePipe(pp);
}

static LONG pContextConfigureEndpoints(struct PsdBase *ps, struct PsdPipe *pp, UWORD cfgnum)
{
    struct PsdDevice *pd = pp->pp_Device;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct PsdEndpoint *pep;
    struct UhcdEndpointDesc *eds;
    struct UhcdConfigureEndpoints ceo;
    UWORD cnt = 0;
    LONG ioerr;

    pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
    while(pc->pc_Node.ln_Succ && (pc->pc_CfgNum != cfgnum)) {
        pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
    }
    if(!pc->pc_Node.ln_Succ) {
        return(UHIOERR_BADPARAMS);
    }

    /* the currently selected alternate of each interface heads pc_Interfaces */
    for(pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
        pif->pif_Node.ln_Succ;
        pif = (struct PsdInterface *) pif->pif_Node.ln_Succ) {
        cnt += pif->pif_NumEPs;
    }

    eds = NULL;
    if(cnt) {
        if(!(eds = psdAllocVec(cnt * sizeof(struct UhcdEndpointDesc)))) {
            return(UHIOERR_OUTOFMEMORY);
        }
        cnt = 0;
        for(pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            pif->pif_Node.ln_Succ;
            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ) {
            for(pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
                pep->pep_Node.ln_Succ;
                pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ) {
                /* a config rebuild re-creates the HCD's endpoint contexts
                   without stream arrays; stale stream bookkeeping here would
                   make pCtxEnsureStreams skip the re-alloc and let stream
                   users run against phantom rings (mirror of the drop path
                   in pContextSetInterface). Endpoints of a previously active
                   *other* config are not walked here - nothing selects
                   between multi-config devices today. The token is rewritten
                   below only on success; pre-clearing covers the failure
                   path too. */
                pep->pep_StreamsAlloc = 0;
                pep->pep_Token = NULL;
                pCtxFillEndpointDesc(&eds[cnt++], pif, pep,
                                     (pd->pd_Flags & PDFF_HIGHSPEED) ? TRUE : FALSE);
            }
        }
    }

    memset(&ceo, 0, sizeof(ceo));
    ceo.ceo_DeviceHandle = pd->pd_Handle;
    ceo.ceo_ConfigValue = cfgnum;
    ceo.ceo_NumAdd = cnt;
    ceo.ceo_Add = eds;

    ioerr = pCtxDoOp(ps, pp, NSCMD_USB_CONFIGURE_ENDPOINTS, &ceo, sizeof(ceo));
    if(!ioerr && cnt) {
        /* the HCD wrote each added endpoint's submit token into eds[];
           same deterministic walk as the fill above */
        cnt = 0;
        for(pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            pif->pif_Node.ln_Succ;
            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ) {
            for(pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
                pep->pep_Node.ln_Succ;
                pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ) {
                pep->pep_Token = eds[cnt++].ed_Token;
            }
        }
    }
    psdFreeVec(eds);
    if(ioerr) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "CONFIGURE_ENDPOINTS (cfg %ld, %ld EPs) failed: %s (%ld)",
                       (ULONG) cfgnum, (ULONG) cnt,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
    }
    return(ioerr);
}

/* The controller drops the endpoints of curif and gets those of pif */
static LONG pContextSetInterface(struct PsdBase *ps, struct PsdPipe *pp, struct PsdInterface *curif, struct PsdInterface *pif)
{
    struct PsdConfig *pc = pif->pif_Config;
    struct PsdDevice *pd = pc->pc_Device;
    struct PsdEndpoint *pep;
    struct UhcdEndpointDesc *eds = NULL;
    UBYTE *drops = NULL;
    struct UhcdConfigureEndpoints ceo;
    UWORD nadd = pif->pif_NumEPs;
    UWORD ndrop = curif->pif_NumEPs;
    UWORD cnt;
    LONG ioerr;

    if(!(nadd || ndrop)) {
        return(0); /* both alternates are endpoint-less: nothing to reconfigure */
    }

    if(nadd) {
        if(!(eds = psdAllocVec(nadd * sizeof(struct UhcdEndpointDesc)))) {
            return(UHIOERR_OUTOFMEMORY);
        }
        cnt = 0;
        for(pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
            pep->pep_Node.ln_Succ;
            pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ) {
            pCtxFillEndpointDesc(&eds[cnt++], pif, pep,
                                 (pd->pd_Flags & PDFF_HIGHSPEED) ? TRUE : FALSE);
        }
        nadd = cnt;
    }
    if(ndrop) {
        if(!(drops = psdAllocVec(ndrop))) {
            psdFreeVec(eds);
            return(UHIOERR_OUTOFMEMORY);
        }
        cnt = 0;
        for(pep = (struct PsdEndpoint *) curif->pif_EPs.lh_Head;
            pep->pep_Node.ln_Succ;
            pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ) {
            drops[cnt++] = pep->pep_EPNum | (pep->pep_Direction ? 0x80 : 0x00);
            pep->pep_StreamsAlloc = 0; /* the HCD frees stream rings with the dropped endpoint */
            pep->pep_Token = NULL;     /* tokens don't survive the drop either */
        }
        ndrop = cnt;
    }

    memset(&ceo, 0, sizeof(ceo));
    ceo.ceo_DeviceHandle = pd->pd_Handle;
    ceo.ceo_ConfigValue = pd->pd_CurrCfg;
    ceo.ceo_NumAdd = nadd;
    ceo.ceo_Add = eds;
    ceo.ceo_NumDrop = ndrop;
    ceo.ceo_DropAddresses = drops;

    ioerr = pCtxDoOp(ps, pp, NSCMD_USB_CONFIGURE_ENDPOINTS, &ceo, sizeof(ceo));
    if(!ioerr && nadd) {
        /* the HCD wrote each added endpoint's submit token into eds[] */
        cnt = 0;
        for(pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
            pep->pep_Node.ln_Succ;
            pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ) {
            pep->pep_Token = eds[cnt++].ed_Token;
        }
    }
    psdFreeVec(eds);
    psdFreeVec(drops);
    return(ioerr);
}

static void pContextUpdateHub(struct PsdBase *ps, struct PsdDevice *pd)
{
    struct UhcdUpdateHub uho;
    LONG ioerr;

    if(!pd->pd_Handle) {
        return; /* hub facts arrive again once the device is created */
    }

    memset(&uho, 0, sizeof(uho));
    uho.uho_DeviceHandle = pd->pd_Handle;
    uho.uho_NumPorts = pd->pd_HubNumPorts;
    uho.uho_TTThinkTime = pd->pd_HubThinkTime;
    uho.uho_MultiTT = (pd->pd_Flags & PDFF_MULTITT) ? 1 : 0;
    uho.uho_HdrDecLat = (UBYTE) pd->pd_HubHdrDecLat; /* SS hubs (hubss.class); 0 otherwise */
    uho.uho_HubDelay = pd->pd_HubDelay;

    ioerr = pCtxDoOpOnDevice(ps, pd, NSCMD_USB_UPDATE_HUB, &uho, sizeof(uho));
    if(ioerr) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "UPDATE_HUB (%ld ports) for '%s' failed: %s (%ld)",
                       (ULONG) pd->pd_HubNumPorts, pd->pd_ProductStr,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
    }
}

static void pContextDestroyDevice(struct PsdBase *ps, struct PsdDevice *pd)
{
    /* latch and zero the handle first so a re-entrant call is a no-op */
    ULONG handle = pd->pd_Handle;

    if(!handle) {
        return;
    }
    pd->pd_Handle = 0;
    pd->pd_Ep0Token = NULL;
    /* during hardware teardown the relay task may already be gone; the
       driver releases all slots on CloseDevice anyway */
    if(pd->pd_Hardware->phw_Task) {
        struct UhcdDestroyDevice ddo;
        ddo.ddo_DeviceHandle = handle;
        pCtxDoOpOnDevice(ps, pd, NSCMD_USB_DESTROY_DEVICE, &ddo, sizeof(ddo));
    }
}

/* SS bulk streams (UAS).  Ensure the HCD holds stream rings for ids 1..maxid
   on this endpoint before stream-tagged transfers start; free them when the
   last stream user goes away.  Gated on the driver's NSD list - a driver
   without NSCMD_USB_ALLOC_STREAMS silently stays single-ring (it ignores the
   stream ids riding the transfers), which is the pre-streams behavior. */
static void pCtxFreeStreams(struct PsdBase *ps, struct PsdEndpoint *pep)
{
    struct PsdDevice *pd = pep->pep_Interface->pif_Config->pc_Device;
    struct UhcdStreams sto;

    if(!pep->pep_StreamsAlloc) {
        return;
    }
    pep->pep_StreamsAlloc = 0;
    /* device already destroyed (unplug) = the driver freed the rings with
       the slot; nothing to send */
    if(!pd->pd_Handle || !pd->pd_Hardware->phw_Task) {
        return;
    }
    sto.sto_DeviceHandle = pd->pd_Handle;
    sto.sto_EpAddress = (UBYTE) (pep->pep_EPNum | (pep->pep_Direction ? 0x80 : 0x00));
    sto.sto_Pad = 0;
    sto.sto_NumStreams = 0;
    pCtxDoOpOnDevice(ps, pd, NSCMD_USB_FREE_STREAMS, &sto, sizeof(sto));
}

static void pCtxEnsureStreams(struct PsdBase *ps, struct PsdEndpoint *pep, UWORD maxid)
{
    struct PsdDevice *pd = pep->pep_Interface->pif_Config->pc_Device;
    struct PsdHardware *phw = pd->pd_Hardware;
    struct UhcdStreams sto;
    LONG ioerr;

    if(!(phw->phw_CtxCmdMask & UHCD_CTXCMD_BIT(NSCMD_USB_ALLOC_STREAMS)) || !pd->pd_Handle) {
        return;
    }
    if(!pep->pep_MaxStreams) {
        return;
    }
    if(maxid > pep->pep_MaxStreams) {
        maxid = pep->pep_MaxStreams;
    }
    if(pep->pep_StreamsAlloc >= maxid) {
        return; /* the allocated rings already cover the id range */
    }
    if(pep->pep_StreamsAlloc) {
        /* growing = free + re-alloc; the stream users open before submitting,
           so the endpoint is idle at every trigger site */
        pCtxFreeStreams(ps, pep);
    }

    sto.sto_DeviceHandle = pd->pd_Handle;
    sto.sto_EpAddress = (UBYTE) (pep->pep_EPNum | (pep->pep_Direction ? 0x80 : 0x00));
    sto.sto_Pad = 0;
    sto.sto_NumStreams = maxid;
    ioerr = pCtxDoOpOnDevice(ps, pd, NSCMD_USB_ALLOC_STREAMS, &sto, sizeof(sto));
    if(ioerr) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "ALLOC_STREAMS (EP 0x%02lx, %ld streams) failed: %s (%ld), staying single-ring.",
                       (ULONG) sto.sto_EpAddress, (ULONG) maxid,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return;
    }
    pep->pep_StreamsAlloc = maxid;
    KPRINTF(5, ("ALLOC_STREAMS EP %02lx: %ld streams\n", sto.sto_EpAddress, maxid));
}

/* Program the device's parent-hub downstream-port U1/U2 inactivity timeout via
   a hub-class SetPortFeature.  For a device on a root port pd_Hub is the
   emulated root hub, whose SetPortFeature handler writes the controller PORTPMSC
   register; for an external hub it is a real EP0 wire transfer to the hub.
   wIndex = (timeout << 8) | port (USB 3.2 hub class).  Best-effort; returns the
   transfer's ioerr, 0 when there is no parent hub to talk to, and -1 when the
   port or pipe could not be allocated. */
static LONG pSetHubPortTimeout(struct PsdBase *ps, struct PsdDevice *pd,
                               UWORD feature, UWORD timeout)
{
    struct PsdDevice *hub = pd->pd_Hub;
    struct MsgPort *mp;
    struct PsdPipe *hpp;
    LONG ioerr = -1;

    if(!hub) {
        return 0; /* the device is itself the (emulated) root hub */
    }
    if((mp = CreateMsgPort())) {
        if((hpp = psdAllocPipe(hub, mp, NULL))) {
            psdPipeSetup(hpp, URTF_CLASS|URTF_OTHER, USR_SET_FEATURE, feature,
                         (UWORD)(((UWORD)timeout << 8) | (pd->pd_HubPort & 0xff)));
            ioerr = psdDoPipe(hpp, NULL, 0);
            if(ioerr) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "SetPortFeature(U%ld_TIMEOUT) to parent hub of '%s' failed: %s (%ld)",
                               (feature == UFS_PORT_U2_TIMEOUT) ? 2 : 1, pd->pd_ProductStr,
                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            }
            psdFreePipe(hpp);
        }
        DeleteMsgPort(mp);
    }
    return ioerr;
}

/* Compose the link power go/no-go for one device.  The per-device override wins
   outright over the global switch - the point of the override is to force a
   known bad device off (or a known good one on) whatever the global default is.
   Deliberately independent of pgc_PowerSaving: link power is a link level state
   the controller enters and leaves autonomously, while suspend is a device state
   the user drives, and conflating the two would hide one behind the other. */
static BOOL pLinkPowerWanted(struct PsdBase *ps, struct PsdDevice *pd)
{
    switch(pd->pd_NotifyCfg.ntc_LinkPowerOverride) {
        case POCL_DISABLE:
            return FALSE;

        case POCL_ENABLE:
            return TRUE;

        default:
            return ps->ps_GlobalCfg->pgc_LinkPowerMgmt ? TRUE : FALSE;
    }
}

/* Arm link power management on pd, using a caller supplied EP0 pipe: hand the
   BOS facts + the U1/U2 go-ahead to a context HCD (NSCMD_USB_SET_LINK_POWER).
   The HCD owns the exit-latency math and its controller-side state (MEL Evaluate
   Context, USB2 hardware-LPM registers) and returns the computed wire
   parameters; the stack issues the device/hub control transfers itself (SET_SEL,
   SET_FEATURE(U1/U2/LTM_ENABLE), the parent-hub port SetPortFeature).  The op
   replies only once MEL is latched, so it is safe to arm the port timeouts
   afterwards.  Everything that reaches the wire is recorded in pd_LpmArmed so a
   later policy change can take exactly that back off. */
static void pLinkPowerArm(struct PsdBase *ps, struct PsdDevice *pd, struct PsdPipe *pp)
{
    struct PsdHardware *phw = pd->pd_Hardware;
    struct UhcdSetLinkPower slo;
    LONG ioerr;

    /* Set first, and unconditionally: every early return below is still a
       policy that has been applied to this device, and the sweep keys its
       "needs work" test on this bit.  Without it a device the HCD or the BOS
       rules out would be revisited on every pass, forever. */
    pd->pd_LpmArmed |= PDLPMF_POLICY;

    if(!phw->phw_ContextBackend ||
       !(phw->phw_CtxCmdMask & UHCD_CTXCMD_BIT(NSCMD_USB_SET_LINK_POWER))) {
        return;
    }
    if(!(pd->pd_Usb30U1ExitLat || pd->pd_Usb30U2ExitLat ||
         pd->pd_Usb30LtmCapable || pd->pd_Usb20LpmCapable)) {
        return; /* the BOS advertises nothing to enable */
    }

    memset(&slo, 0, sizeof(slo));
    slo.slo_DeviceHandle = pd->pd_Handle;
    slo.slo_U1Enable = 1;
    slo.slo_U2Enable = 1;
    slo.slo_U1DevExitLat = pd->pd_Usb30U1ExitLat;
    slo.slo_U2DevExitLat = pd->pd_Usb30U2ExitLat;
    if(pd->pd_Usb20LpmCapable) {
        slo.slo_Flags |= UHCD_LPF_USB2_LPM;
    }
    if(pd->pd_Usb30LtmCapable) {
        slo.slo_Flags |= UHCD_LPF_LTM;
    }
    /* USB 2.0 extension BESL facts (USB2 LPM ECN), pre-decoded from the BOS. */
    if(pd->pd_Usb20BeslCapable) {
        slo.slo_Flags |= UHCD_LPF_BESL;
    }
    if(pd->pd_Usb20BeslBaselineValid) {
        slo.slo_Flags |= UHCD_LPF_BESL_BASELINE;
        slo.slo_BeslBaseline = pd->pd_Usb20BeslBaseline;
    }
    if(pd->pd_Usb20BeslDeepValid) {
        slo.slo_Flags |= UHCD_LPF_BESL_DEEP;
        slo.slo_BeslDeep = pd->pd_Usb20BeslDeep;
    }

    ioerr = pCtxDoOp(ps, pp, NSCMD_USB_SET_LINK_POWER, &slo, sizeof(slo));
    if(ioerr) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Link power setup for '%s' failed: %s (%ld)",
                       pd->pd_ProductStr,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return; /* MEL not latched: arm nothing on the wire */
    }
    /* The HCD took a non-empty policy, so it may now hold controller side state
       (MEL, root port PORTPMSC timeouts, USB2 hardware LPM) that only a withheld
       op can take back down again. */
    pd->pd_LpmArmed |= PDLPMF_CTXOP;

    /* The HCD programmed its controller-side state (MEL, USB2 hardware LPM) and
       returned the computed wire parameters; issue the device/hub control
       transfers it asks for.  Each is best-effort (LPM is advisory): a reject
       warns and the sequence continues. */

    /* (a) SET_SEL - inform the device of the system/path exit latencies. */
    if(slo.slo_OutFlags & UHCD_LPO_SET_SEL) {
        struct UsbSetSelData sel;
        sel.uss_U1Sel = (UBYTE) slo.slo_OutU1Sel;
        sel.uss_U1Pel = (UBYTE) slo.slo_OutU1Pel;
        sel.uss_U2Sel = AROS_WORD2LE(slo.slo_OutU2Sel);
        sel.uss_U2Pel = AROS_WORD2LE(slo.slo_OutU2Pel);
        psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE, USR_SET_SEL, 0, 0);
        ioerr = psdDoPipe(pp, &sel, sizeof(sel));
        if(ioerr) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "SET_SEL for '%s' failed: %s (%ld)", pd->pd_ProductStr,
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        }
    }

    /* (b) Arm the parent-hub downstream port U1/U2 inactivity timeouts (host-
       initiated LPM).  A root-port child routes to the emulated root hub. */
    if(slo.slo_OutU1Timeout) {
        if(!pSetHubPortTimeout(ps, pd, UFS_PORT_U1_TIMEOUT, slo.slo_OutU1Timeout)) {
            pd->pd_LpmArmed |= PDLPMF_PORTU1;
        }
    }
    if(slo.slo_OutU2Timeout) {
        if(!pSetHubPortTimeout(ps, pd, UFS_PORT_U2_TIMEOUT, slo.slo_OutU2Timeout)) {
            pd->pd_LpmArmed |= PDLPMF_PORTU2;
        }
    }

    /* (c) Enable device-initiated U1/U2 (needs SET_SEL sent + may-initiate). */
    if(slo.slo_OutFlags & UHCD_LPO_SET_SEL) {
        if((slo.slo_OutFlags & UHCD_LPO_U1_INIT) && slo.slo_OutU1Timeout) {
            psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE, USR_SET_FEATURE,
                         UFS_DEVICE_U1_ENABLE, 0);
            if(!psdDoPipe(pp, NULL, 0)) {
                pd->pd_LpmArmed |= PDLPMF_U1DEV;
            }
        }
        if((slo.slo_OutFlags & UHCD_LPO_U2_INIT) && slo.slo_OutU2Timeout) {
            psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE, USR_SET_FEATURE,
                         UFS_DEVICE_U2_ENABLE, 0);
            if(!psdDoPipe(pp, NULL, 0)) {
                pd->pd_LpmArmed |= PDLPMF_U2DEV;
            }
        }
    }

    /* (d) Enable Latency Tolerance Messaging. */
    if(slo.slo_OutFlags & UHCD_LPO_LTM) {
        psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE, USR_SET_FEATURE,
                     UFS_DEVICE_LTM_ENABLE, 0);
        if(!psdDoPipe(pp, NULL, 0)) {
            pd->pd_LpmArmed |= PDLPMF_LTM;
        }
    }
}

/* One device-recipient CLEAR_FEATURE of the disarm sequence.  The bit is dropped
   whatever the device answers: if it did not hear us the link is gone anyway,
   and retrying a CLEAR_FEATURE against a device that is not listening only
   burns NAK timeouts on every later sweep. */
static void pLinkPowerClearDevFeature(struct PsdBase *ps, struct PsdDevice *pd,
                                      struct PsdPipe *pp, UWORD feature, UWORD flag)
{
    LONG ioerr;

    if(!(pd->pd_LpmArmed & flag)) {
        return;
    }
    psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE, USR_CLEAR_FEATURE, feature, 0);
    ioerr = psdDoPipe(pp, NULL, 0);
    if(ioerr) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "CLEAR_FEATURE(%ld) for '%s' failed: %s (%ld)",
                       (LONG) feature, pd->pd_ProductStr,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
    }
    pd->pd_LpmArmed &= ~flag;
}

/* Take back exactly what pLinkPowerArm() put on the wire, in the reverse order:
   device-initiated states off first, so
   the device stops proposing U1/U2 before the port stops allowing it, then the
   host-initiated port timeouts, then the controller side state.

   Nothing here is fatal.  A half disarmed device is worse than a fully attempted
   one, so a device that rejects a CLEAR_FEATURE must still get its port timeouts
   zeroed and its controller state torn down.  The ctx op is the one step that
   keeps its bit on failure: a stale PORTPMSC.HLE pointing at a live slot is the
   only leftover with consequences, so the next sweep retries it. */
static void pLinkPowerDisarm(struct PsdBase *ps, struct PsdDevice *pd, struct PsdPipe *pp)
{
    pLinkPowerClearDevFeature(ps, pd, pp, UFS_DEVICE_U1_ENABLE, PDLPMF_U1DEV);
    pLinkPowerClearDevFeature(ps, pd, pp, UFS_DEVICE_U2_ENABLE, PDLPMF_U2DEV);
    pLinkPowerClearDevFeature(ps, pd, pp, UFS_DEVICE_LTM_ENABLE, PDLPMF_LTM);

    if(pd->pd_LpmArmed & PDLPMF_PORTU1) {
        pSetHubPortTimeout(ps, pd, UFS_PORT_U1_TIMEOUT, 0);
        pd->pd_LpmArmed &= ~PDLPMF_PORTU1;
    }
    if(pd->pd_LpmArmed & PDLPMF_PORTU2) {
        pSetHubPortTimeout(ps, pd, UFS_PORT_U2_TIMEOUT, 0);
        pd->pd_LpmArmed &= ~PDLPMF_PORTU2;
    }

    if(pd->pd_LpmArmed & PDLPMF_CTXOP) {
        struct UhcdSetLinkPower slo;
        /* A fully withheld block: no enables, no exit latencies and - just as
           important - none of the UHCD_LPF_* capability facts.  The HCD
           evaluates LTM and USB2 hardware LPM independently of the enable
           words, so zeroing the enables alone would leave L1 armed. */
        memset(&slo, 0, sizeof(slo));
        slo.slo_DeviceHandle = pd->pd_Handle;
        if(!pCtxDoOp(ps, pp, NSCMD_USB_SET_LINK_POWER, &slo, sizeof(slo))) {
            pd->pd_LpmArmed &= ~PDLPMF_CTXOP;
        } else {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Link power teardown for '%s' failed, will retry.",
                           pd->pd_ProductStr);
        }
    }
    pd->pd_LpmArmed &= ~PDLPMF_POLICY;
}

/* Bring one device into line with the current link power policy.  Unlike the
   enumeration path there is no pipe in hand here, so this owns its port and pipe
   (pArmRemoteWakeup() is the same shape).  Needs a live EP0: the caller must
   have ruled out suspended, dead and disconnected devices. */
static void pLinkPowerApply(struct PsdBase *ps, struct PsdDevice *pd)
{
    BOOL want = pLinkPowerWanted(ps, pd);
    struct MsgPort *mp;
    struct PsdPipe *pp;

    if(want == ((pd->pd_LpmArmed & PDLPMF_POLICY) ? TRUE : FALSE)) {
        return;
    }
    if((mp = CreateMsgPort())) {
        if((pp = psdAllocPipe(pd, mp, NULL))) {
            psdSetAttrs(PGA_PIPE, pp,
                        PPA_NakTimeout, TRUE,
                        PPA_NakTimeoutTime, 1000,
                        TAG_END);
            if(want) {
                pLinkPowerArm(ps, pd, pp);
            } else {
                pLinkPowerDisarm(ps, pd, pp);
            }
            psdFreePipe(pp);
        }
        DeleteMsgPort(mp);
    }
}

/* Bring every configured device into line with the current link power policy.
   Runs on the event handler task: each device costs a handful of blocking
   control transfers, so PBase is dropped around every one of them using the
   unlock/relock/restart idiom of psdRemClass().  Restarting from the head also
   keeps parents ahead of children, which the arm direction needs - the HCD only
   considers a child LPM capable once its parent hub is.

   Termination: every visit flips PDLPMF_POLICY, so no device can be picked up
   twice in one sweep. */
static void pLinkPowerSweep(struct PsdBase *ps)
{
    BOOL restart;

    /* cleared first, so a policy change landing mid-sweep requests another one
       instead of being swallowed by this one */
    ps->ps_LinkPowerReq = FALSE;

    psdLockReadPBase();
    do {
        struct PsdDevice *pd = NULL;
        restart = FALSE;
        while((pd = psdGetNextDevice(pd))) {
            if(!pd->pd_CurrentConfig) {
                continue; /* nothing to arm until it is configured */
            }
            /* PDFF_SUSPENDED above all: psdDoPipe() transparently resumes a
               suspended device, so touching one here would wake the bus for a
               policy change.  Left alone; psdResumeBindings() asks for a fresh
               sweep when it comes back. */
            if((pd->pd_Flags & (PDFF_CONNECTED|PDFF_SUSPENDED|PDFF_DEAD|PDFF_DELEXPUNGE))
               != PDFF_CONNECTED) {
                continue;
            }
            if(pLinkPowerWanted(ps, pd) == ((pd->pd_LpmArmed & PDLPMF_POLICY) ? TRUE : FALSE)) {
                continue;
            }
            psdUnlockPBase();
            pLinkPowerApply(ps, pd);
            psdLockReadPBase();
            restart = TRUE;
            break;
        }
    } while(restart);
    psdUnlockPBase();
}

/* Name the class whose interface owns this endpoint, for the recovery log:
   that class's error handling left the halt standing. */
static STRPTR pStallOwnerName(struct PsdDevice *pd, UWORD epnum, UWORD isin)
{
    struct PsdConfig *pc = pd->pd_CurrentConfig;

    if(pc) {
        struct PsdInterface *pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
        while(pif->pif_Node.ln_Succ) {
            struct PsdEndpoint *pep = (struct PsdEndpoint *) pif->pif_EPs.lh_Head;
            while(pep->pep_Node.ln_Succ) {
                if((pep->pep_EPNum == epnum) &&
                   ((pep->pep_Direction ? 1 : 0) == isin) &&
                   pif->pif_ClsBinding) {
                    return(pif->pif_ClsBinding->puc_ClassName);
                }
                pep = (struct PsdEndpoint *) pep->pep_Node.ln_Succ;
            }
            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
        }
    }
    if(pd->pd_ClsBinding) {
        return(pd->pd_ClsBinding->puc_ClassName);
    }
    return((STRPTR) "no class");
}

/* Test-and-clear one pd_EpHaltMask bit.  No retry on a failed clear: if the
   endpoint halts again, delivery re-marks it.  Bits 0/16 (EP0) are never set.
   One bit at a time, not the whole mask: a class clearing another endpoint
   itself meanwhile still cancels that one (pSubmitPipe snoop) instead of
   drawing a late duplicate under its resumed traffic.  The unlocked peek
   keeps the semaphore to the bits actually marked; a bit it misses re-raises
   ps_StallRecoveryReq.  Portability: one CPU (see pd_EpHaltMask). */
static BOOL pTakeEpHalt(struct PsdBase *ps, struct PsdDevice *pd, UWORD bit)
{
    if(!((pd->pd_EpHaltMask >> bit) & 1)) {
        return(FALSE);
    }
    ObtainSemaphore(&ps->ps_StallRecoverySem);
    BOOL marked = (pd->pd_EpHaltMask >> bit) & 1;
    pd->pd_EpHaltMask &= ~(1UL << bit);
    ReleaseSemaphore(&ps->ps_StallRecoverySem);
    return(marked);
}

/* Send one CLEAR_FEATURE(ENDPOINT_HALT) and log the outcome.  An ordinary
   control transfer on the normal pipe path: legacy HCDs snoop it to reset
   their data toggle (usbhardware.doc), context HCDs re-arm the endpoint
   host-side. */
static void pClearEpHalt(struct PsdBase *ps, struct PsdPipe *pp, UWORD bit)
{
    struct PsdDevice *pd = pp->pp_Device;
    LONG epnum = (LONG) PDEPHALT_EPNUM(bit);
    UWORD isin = (UWORD) PDEPHALT_ISIN(bit);

    psdPipeSetup(pp, URTF_STANDARD|URTF_ENDPOINT,
                 USR_CLEAR_FEATURE, UFS_ENDPOINT_HALT, PDEPHALT_ADDR(bit));
    LONG ioerr = psdDoPipe(pp, NULL, 0);

    if(!ioerr) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Endpoint %ld %s of %s (%s) was halted; cleared it.",
                       epnum, isin ? "in" : "out", pd->pd_ProductStr,
                       pStallOwnerName(pd, (UWORD) epnum, isin));
        return;
    }
    if(ioerr == UHIOERR_TIMEOUT) {
        /* device gone (a controller-raised halt is often the first sign of
           an unplug): nothing to report */
        return;
    }
    psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                   "Clearing halted endpoint %ld %s of %s failed: %s (%ld)",
                   epnum, isin ? "in" : "out", pd->pd_ProductStr,
                   psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
}

/* Clear every endpoint halt still owed on one device (its pd_EpHaltMask bits).
   pLinkPowerApply() shape: own port + transient EP0 pipe, no device lock -
   lifetime comes from psdAllocPipe()'s pd_UseCnt, so pd must not be touched
   after psdFreePipe(), which may run the deferred pFreeDevice(). */
static void pStallRecoverDevice(struct PsdBase *ps, struct PsdDevice *pd)
{
    struct MsgPort *mp = CreateMsgPort();
    struct PsdPipe *pp = mp ? psdAllocPipe(pd, mp, NULL) : NULL;

    if(!pp) {
        /* no port or pipe: drop the marks rather than retry in a tight loop;
           a halt that still stands re-marks on the class's next submit */
        ObtainSemaphore(&ps->ps_StallRecoverySem);
        pd->pd_EpHaltMask = 0;
        ReleaseSemaphore(&ps->ps_StallRecoverySem);
    } else {
        psdSetAttrs(PGA_PIPE, pp,
                    PPA_NakTimeout, TRUE,
                    PPA_NakTimeoutTime, 1000,
                    TAG_END);
        for(UWORD bit = 1; bit < 32; bit++) {
            if(pTakeEpHalt(ps, pd, bit)) {
                pClearEpHalt(ps, pp, bit);
            }
        }
        psdFreePipe(pp); /* may collect a DELEXPUNGE device: pd is dead to us now */
    }
    if(mp) {
        DeleteMsgPort(mp);
    }
}

/* Clear the endpoint halts of one marked device per call.  Runs on the event
   handler task (the one task that may block on the wire, like
   pLinkPowerSweep).  The request flag is cleared first, so a stall landing
   mid-sweep requests another sweep instead of being swallowed by this one.
   One device, then back to the event loop with the request re-raised: a
   device that re-stalls as fast as it is cleared (a class resubmitting onto a
   persistently halted endpoint) must not trap the task in here. */
static void pStallRecoverySweep(struct PsdBase *ps)
{
    struct PsdDevice *pd = NULL;

    ps->ps_StallRecoveryReq = FALSE;   /* SMP: full fence after this store */

    psdLockReadPBase();
    while((pd = psdGetNextDevice(pd))) {
        /* unlocked peek (TODO one CPU, see pd_EpHaltMask); a mark landing
           after it re-raises ps_StallRecoveryReq for the next sweep */
        if(!pd->pd_EpHaltMask) {
            continue;
        }
        /* PDFF_SUSPENDED above all (see pLinkPowerSweep): a suspended
           endpoint is quiesced, and psdDoPipe() would resume the device
           just to send a clear.  Drop the marks; if the halt still stands
           after resume, the class's resubmit re-stalls and re-marks. */
        if((pd->pd_Flags & (PDFF_CONNECTED|PDFF_SUSPENDED|PDFF_DEAD|PDFF_DELEXPUNGE))
           != PDFF_CONNECTED) {
            ObtainSemaphore(&ps->ps_StallRecoverySem);
            pd->pd_EpHaltMask = 0;
            ReleaseSemaphore(&ps->ps_StallRecoverySem);
            continue;
        }
        break;
    }
    psdUnlockPBase();

    if(!pd) {
        return;
    }
    /* pd outlives the unlock only because pFreeDevice() never frees a
       PsdDevice (a removed one fails the clear as not connected) */
    pStallRecoverDevice(ps, pd);

    /* more devices may be marked: come straight back after the loop has
       served its other work (an empty next pass simply ends it) */
    ps->ps_StallRecoveryReq = TRUE;
    SetSignal(1UL << ps->ps_EventHandler.ph_MsgPort->mp_SigBit,
              1UL << ps->ps_EventHandler.ph_MsgPort->mp_SigBit);
}

static const struct PsdHCDOps pContextHCDOps =
{
    pContextAddressDevice,
    pContextUpdateEp0MaxPacket,
    pContextConfigureEndpoints,
    pContextSetInterface,
    pContextUpdateHub,
    pContextDestroyDevice,
};
/* \\\ */

/* /// "pCollectEjectBindings()" */
/* The bindings on a device whose class can safe-eject, shared by
   psdGetAttrsA(DA_CanSafeEject) and psdSafeEjectDevice() - hence defined this
   early. */

#define NEJECT_MAXBIND 8

struct EjectBindings
{
    APTR                eb_Bindings[NEJECT_MAXBIND];
    struct PsdUsbClass *eb_Classes[NEJECT_MAXBIND];  /* parallel: for the dedupe + UsbClsBase */
    ULONG               eb_Count;
};

static void pAddEjectBinding(struct EjectBindings *eb, APTR binding, struct PsdUsbClass *puc)
{
    if((!binding) || (!puc) || (eb->eb_Count >= NEJECT_MAXBIND)) {
        return;
    }
    IPTR supports = 0;
    usbGetAttrs(UGA_CLASS, NULL, UCCA_SupportsSafeEject, &supports, TAG_END);
    if(!supports) {
        return;
    }
    /* Dedupe by class: UCM_SafeEject is device-scoped, so a class quiesces
       everything it holds on the device in one call - asking it again for a
       sibling interface would eject an already-ejected device. */
    ULONG idx;
    for(idx = 0; idx < eb->eb_Count; idx++) {
        if(eb->eb_Classes[idx] == puc) {
            return;
        }
    }
    eb->eb_Bindings[eb->eb_Count] = binding;
    eb->eb_Classes[eb->eb_Count] = puc;
    eb->eb_Count++;
}

/* A class can sit at either of Neptune's two binding levels
   (neptune.library-architecture.md section 5.5): one binding for the whole
   device, or one per interface of the current config -
   which is the level massstorage binds at.  Both are asked, exactly as
   psdSuspendBindings() does. */
static void pCollectEjectBindings(struct PsdDevice *pd, struct EjectBindings *eb)
{
    eb->eb_Count = 0;
    pAddEjectBinding(eb, pd->pd_DevBinding, pd->pd_ClsBinding);
    struct PsdConfig *pc;
    if((pc = pd->pd_CurrentConfig)) {
        struct PsdInterface *pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
        while(pif->pif_Node.ln_Succ) {
            pAddEjectBinding(eb, pif->pif_IfBinding, pif->pif_ClsBinding);
            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
        }
    }
}
/* \\\ */

/* /// "pSuspendRefusal()" */
/* Why psdSuspendDevice() would refuse this device before touching anything,
   or NULL.  Shared with psdGetAttrsA(DA_CanSuspend) so a GUI greys its button
   on exactly the conditions the call itself checks - hence defined this early.
   Only the static refusals: a class that is busy or declines, and the
   application-binding rule, depend on the moment and on pgc_ForceSuspend, and
   still surface from psdSuspendBindings(). */
static STRPTR pSuspendRefusal(struct PsdDevice *pd)
{
    if(pd->pd_Hardware->phw_ContextBackend &&
       !(pd->pd_Hardware->phw_CtxCmdMask & UHCD_CTXCMD_BIT(NSCMD_USB_SET_SUSPEND))) {
        /* on a context HCD, the endpoint rings must be quiesced before the
           hub port goes to U3/suspend - that is the SET_SUSPEND op */
        return "HCD does not support suspend";
    }
    struct PsdDevice *hubpd = pd->pd_Hub;
    if(hubpd && !(hubpd->pd_DevBinding && hubpd->pd_ClsBinding)) {
        /* only the parent hub class can park the port (UCM_HubSuspendDevice);
           a root device (no hub) is suspended by the core itself */
        return "its hub has no class binding";
    }
    return NULL;
}
/* \\\ */

/* /// "psdGetAttrsA()" */
LONG (psdGetAttrsA)(ULONG type asm("d0"), APTR psdstruct asm("a0"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct TagItem *ti;
    ULONG count = 0;
    ULONG *packtab = NULL;

    KPRINTF(1, ("psdGetAttrsA(%ld, 0x%08lx, 0x%08lx)\n", type, psdstruct, tags));

    if(type <= PGA_LAST) {
        packtab = (ULONG *) PsdPTArray[type];
    }

    switch(type) {
    case PGA_STACK:
        psdstruct = ps;
        if((ti = FindTagItem(PA_HardwareList, tags))) {
            *((struct List **) ti->ti_Data) = &ps->ps_Hardware;
            count++;
        }
        if((ti = FindTagItem(PA_ClassList, tags))) {
            *((struct List **) ti->ti_Data) = &ps->ps_Classes;
            count++;
        }
        if((ti = FindTagItem(PA_ErrorMsgList, tags))) {
            *((struct List **) ti->ti_Data) = &ps->ps_ErrorMsgs;
            count++;
        }
        break;

    case PGA_HARDWARE:
        if((ti = FindTagItem(HA_DeviceList, tags))) {
            *((struct List **) ti->ti_Data) = &(((struct PsdHardware *) psdstruct)->phw_Devices);
            count++;
        }
        break;

    case PGA_DEVICE:
        if((ti = FindTagItem(DA_ConfigList, tags))) {
            *((struct List **) ti->ti_Data) = &(((struct PsdDevice *) psdstruct)->pd_Configs);
            count++;
        }
        if((ti = FindTagItem(DA_DescriptorList, tags))) {
            *((struct List **) ti->ti_Data) = &(((struct PsdDevice *) psdstruct)->pd_Descriptors);
            count++;
        }
        if((ti = FindTagItem(DA_ContainerId, tags))) {
            /* interior pointer; PsdDevice structs are never freed */
            struct PsdDevice *pd = (struct PsdDevice *) psdstruct;
            *((UBYTE **) ti->ti_Data) = pd->pd_HasContainerId ? pd->pd_ContainerId : NULL;
            count++;
        }
        if((ti = FindTagItem(DA_CanSafeEject, tags))) {
            struct EjectBindings eb;
            pCollectEjectBindings((struct PsdDevice *) psdstruct, &eb);
            *((IPTR *) ti->ti_Data) = eb.eb_Count ? TRUE : FALSE;
            count++;
        }
        if((ti = FindTagItem(DA_CanSuspend, tags))) {
            *((IPTR *) ti->ti_Data) = pSuspendRefusal((struct PsdDevice *) psdstruct) ? FALSE : TRUE;
            count++;
        }
        break;

    case PGA_CONFIG:
        if((ti = FindTagItem(CA_InterfaceList, tags))) {
            *((struct List **) ti->ti_Data) = &(((struct PsdConfig *) psdstruct)->pc_Interfaces);
            count++;
        }
        break;

    case PGA_INTERFACE:
        if((ti = FindTagItem(IFA_EndpointList, tags))) {
            *((struct List **) ti->ti_Data) = &(((struct PsdInterface *) psdstruct)->pif_EPs);
            count++;
        }
        if((ti = FindTagItem(IFA_AlternateIfList, tags))) {
            *((struct List **) ti->ti_Data) = &(((struct PsdInterface *) psdstruct)->pif_AlterIfs);
            count++;
        }
        break;

    case PGA_ERRORMSG:
        if((ti = FindTagItem(EMA_DateStamp, tags))) {
            *((struct DateStamp **) ti->ti_Data) = &(((struct PsdErrorMsg *) psdstruct)->pem_DateStamp);
            count++;
        }
        break;

    case PGA_PIPE:
        if((ti = FindTagItem(PPA_IORequest, tags))) {
            *((struct IOUsbHWReq **) ti->ti_Data) = &(((struct PsdPipe *) psdstruct)->pp_IOReq);
            count++;
        }
        break;

    case PGA_STACKCFG:
        if((ti = FindTagItem(GCA_InsertionSound, tags))) {
            count++;
            *((STRPTR *) ti->ti_Data) = ps->ps_Notify.nt_InsertSndFile;
        }
        if((ti = FindTagItem(GCA_RemovalSound, tags))) {
            count++;
            *((STRPTR *) ti->ti_Data) = ps->ps_Notify.nt_RemoveSndFile;
        }
        break;
    }
    if(packtab) {
        return((LONG) (UnpackStructureTags(psdstruct, (ULONG *) packtab, tags)+count));
    } else {
        return(-1);
    }
}
/* \\\ */

/* /// "psdSetAttrsA()" */
LONG (psdSetAttrsA)(ULONG type asm("d0"), APTR psdstruct asm("a0"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct TagItem *ti;
    ULONG count = 0;
    ULONG *packtab = NULL;
    BOOL savenotifycfg = FALSE;
    BOOL checkcfgupdate = FALSE;
    BOOL powercalc = FALSE;
    BOOL updatehub = FALSE;
    BOOL checklinkpower = FALSE;
    UWORD oldlinkpower = 0;
    LONG res;

    KPRINTF(1, ("psdSetAttrsA(%ld, 0x%08lx, 0x%08lx)\n", type, psdstruct, tags));

    if(type <= PGA_LAST) {
        packtab = (ULONG *) PsdPTArray[type];
    }

    switch(type) {
    case PGA_DEVICE:
        if(FindTagItem(DA_InhibitPopup, tags) || FindTagItem(DA_InhibitClassBind, tags)) {
            savenotifycfg = TRUE;
        }
        if(FindTagItem(DA_OverridePowerInfo, tags)) {
            savenotifycfg = TRUE;
            powercalc = TRUE;
        }
        if(FindTagItem(DA_NoAutoSuspend, tags)) {
            savenotifycfg = TRUE;
        }
        if(FindTagItem(DA_LinkPowerOverride, tags)) {
            savenotifycfg = TRUE;
            /* snapshot before the pack: NeptuneUSB rewrites every per-device
               setting on each gadget click, and only a real change should
               cost a wire round trip */
            oldlinkpower = ((struct PsdDevice *) psdstruct)->pd_NotifyCfg.ntc_LinkPowerOverride;
            checklinkpower = TRUE;
        }
        if(FindTagItem(DA_HubNumPorts, tags)) {
            /* the hub classes announce hub facts (port count, think time,
               multi-TT) with this tag once the hub descriptor is read; the
               lifecycle backend forwards them to the HCD (update-hub op on
               context backends, no-op on legacy) */
            updatehub = TRUE;
        }
        break;

    case PGA_STACK:
        psdstruct = ps;
        break;

    case PGA_STACKCFG:
        if((ti = FindTagItem(GCA_InsertionSound, tags))) {
            count++;
            if(strcmp(ps->ps_Notify.nt_InsertSndFile, (STRPTR) ti->ti_Data)) {
                psdFreeVec(ps->ps_Notify.nt_InsertSndFile);
                ps->ps_Notify.nt_InsertSndFile = psdCopyStr((STRPTR) ti->ti_Data);
            }
        }
        if((ti = FindTagItem(GCA_RemovalSound, tags))) {
            count++;
            if(strcmp(ps->ps_Notify.nt_RemoveSndFile, (STRPTR) ti->ti_Data)) {
                psdFreeVec(ps->ps_Notify.nt_RemoveSndFile);
                ps->ps_Notify.nt_RemoveSndFile = psdCopyStr((STRPTR) ti->ti_Data);
            }
        }
        if(FindTagItem(GCA_LinkPowerMgmt, tags)) {
            oldlinkpower = ps->ps_GlobalCfg->pgc_LinkPowerMgmt;
            checklinkpower = TRUE;
        }
        checkcfgupdate = TRUE;
        break;

    case PGA_ENDPOINT: {
        struct PsdEndpoint *pep = (struct PsdEndpoint *) psdstruct;
        UWORD maxstreams;

        count += PackStructureTags(psdstruct, packtab, tags);
        maxstreams = pGetMaxStreamsForEndpoint(pep);
        pep->pep_MaxStreams = maxstreams;
        if (!maxstreams && pep->pep_StreamBase) {
            psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname,
                            "Stream base requested for endpoint without USB3 stream support.");
            pep->pep_StreamBase = 0;
        } else if (maxstreams && pep->pep_StreamBase > maxstreams) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Stream base %ld exceeds max streams %ld; disabling stream IDs.",
                           pep->pep_StreamBase, maxstreams);
            pep->pep_StreamBase = 0;
        }
        if(!pep->pep_StreamBase && pep->pep_StreamsAlloc) {
            /* stream ids switched off (e.g. UAS teardown): release the HCD's
               stream rings */
            pCtxFreeStreams(ps, pep);
        }

        return((LONG) count);
    }

    case PGA_PIPE: {
        struct PsdPipe *pp = (struct PsdPipe *) psdstruct;

        count += PackStructureTags(psdstruct, packtab, tags);
        if(FindTagItem(PPA_StreamID, tags) && pp->pp_Endpoint) {
            if(pp->pp_StreamID) {
                /* a plain pipe joins an endpoint's stream id space (UAS status
                   pipe): make sure the HCD has a ring for that id */
                pCtxEnsureStreams(ps, pp->pp_Endpoint, pp->pp_StreamID);
            } else if(pp->pp_Endpoint->pep_StreamsAlloc &&
                      !pp->pp_Endpoint->pep_StreamBase) {
                /* stream id cleared on a plain pipe: release the HCD's rings,
                   symmetric with EA_StreamBase -> 0. Guard on !pep_StreamBase
                   so a PsdPipeStream that owns the endpoint isn't torn down
                   underneath. */
                pCtxFreeStreams(ps, pp->pp_Endpoint);
            }
        }
        return((LONG) count);
    }

    case PGA_PIPESTREAM: {
        struct PsdPipeStream *pps = (struct PsdPipeStream *) psdstruct;
        struct PsdPipe *pp;
        ULONG oldbufsize = pps->pps_BufferSize;
        ULONG oldnumpipes = pps->pps_NumPipes;
        ULONG cnt;
        UWORD maxstreams;
        UWORD streambase;

        KPRINTF(1, ("SetAttrs PIPESTREAM\n"));
        ObtainSemaphore(&pps->pps_AccessLock);
        if((ti = FindTagItem(PSA_MessagePort, tags))) {
            count++;
            if((pps->pps_Flags & PSFF_OWNMSGPORT) && pps->pps_MsgPort) {
                KPRINTF(1, ("Deleting old MsgPort\n"));
                DeleteMsgPort(pps->pps_MsgPort);
                pps->pps_MsgPort = NULL;
            }
            pps->pps_Flags &= ~PSFF_OWNMSGPORT;
        }
        count += PackStructureTags(psdstruct, packtab, tags);
        KPRINTF(1, ("Pipes = %ld (old: %ld), BufferSize = %ld (old: %ld)\n",
                    pps->pps_NumPipes, oldnumpipes, pps->pps_BufferSize, oldbufsize));

        maxstreams = pGetMaxStreamsForEndpoint(pps->pps_Endpoint);
        streambase = pps->pps_Endpoint->pep_StreamBase;
        if (streambase) {
            if (!maxstreams) {
                psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname,
                                "Stream IDs requested but endpoint does not support USB3 streams.");
                streambase = 0;
            } else if (streambase > maxstreams) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Stream base %ld exceeds max streams %ld; disabling stream IDs.",
                               streambase, maxstreams);
                streambase = 0;
            } else {
                UWORD usable = (UWORD)(maxstreams - streambase + 1);
                if (pps->pps_NumPipes > usable) {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Stream pipe count %ld exceeds available streams %ld; capping.",
                                   pps->pps_NumPipes, usable);
                    pps->pps_NumPipes = usable;
                }
            }
        }
        if(pps->pps_NumPipes < 1) {
            pps->pps_NumPipes = 1; /* minimal */
        }
        if(pps->pps_BufferSize < pps->pps_Endpoint->pep_MaxPktSize) {
            pps->pps_BufferSize = pps->pps_Endpoint->pep_MaxPktSize; /* minimal */
        }
        if(!pps->pps_MsgPort) {
            if((pps->pps_MsgPort = CreateMsgPort())) {
                KPRINTF(1, ("Creating MsgPort\n"));
                pps->pps_Flags |= PSFF_OWNMSGPORT;
            }
        }
        /* do we need to reallocate? */
        if((oldbufsize != pps->pps_BufferSize) ||
                (oldnumpipes != pps->pps_NumPipes) ||
                (!pps->pps_Pipes) ||
                (!pps->pps_Buffer)) {
            if(pps->pps_Pipes) {
                KPRINTF(1, ("freeing %ld old pipes\n", oldnumpipes));
                for(cnt = 0; cnt < oldnumpipes; cnt++) {
                    pp = pps->pps_Pipes[cnt];
                    //if(pp->pp_IOReq.iouh_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE)
                    {
                        KPRINTF(1, ("Abort %ld\n", cnt));
                        psdAbortPipe(pp);
                        KPRINTF(1, ("Wait %ld\n", cnt));
                        psdWaitPipe(pp);
                    }
                    KPRINTF(1, ("Free %ld\n", cnt));
                    psdFreePipe(pp);
                }
                psdFreeVec(pps->pps_Pipes);
            }
            psdFreeVec(pps->pps_Buffer);
            /* reset stuff */
            NewList(&pps->pps_FreePipes);
            NewList(&pps->pps_ReadyPipes);
            pps->pps_Offset = 0;
            pps->pps_BytesPending = 0;
            pps->pps_ReqBytes = 0;
            pps->pps_ActivePipe = NULL;
            pps->pps_Buffer = psdAllocVec(pps->pps_NumPipes * pps->pps_BufferSize);
            pps->pps_Pipes = psdAllocVec(pps->pps_NumPipes * sizeof(struct PsdPipe *));
            if(pps->pps_Pipes && pps->pps_Buffer) {
                KPRINTF(1, ("allocating %ld new pipes\n", pps->pps_NumPipes));
                for(cnt = 0; cnt < pps->pps_NumPipes; cnt++) {
                    pp = psdAllocPipe(pps->pps_Device, pps->pps_MsgPort, pps->pps_Endpoint);
                    if((pps->pps_Pipes[cnt] = pp)) {
                        pp->pp_Num = cnt;
                        if (streambase && maxstreams) {
                            pp->pp_StreamID = (UWORD)(streambase + cnt);
                        } else {
                            pp->pp_StreamID = 0;
                        }
                        if(pps->pps_Flags & PSFF_NOSHORTPKT) pp->pp_IOReq.iouh_Flags |= UHFF_NOSHORTPKT;
                        if(pps->pps_Flags & PSFF_NAKTIMEOUT) pp->pp_IOReq.iouh_Flags |= UHFF_NAKTIMEOUT;
                        if(pps->pps_Flags & PSFF_ALLOWRUNT) pp->pp_IOReq.iouh_Flags |= UHFF_ALLOWRUNTPKTS;
                        pp->pp_IOReq.iouh_NakTimeout = pps->pps_NakTimeoutTime;
                        AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);
                    } else {
                        KPRINTF(1, ("Allocating Pipe %ld failed!\n", cnt));
                    }
                }
            } else {
                KPRINTF(1, ("Allocating Pipe array failed!\n"));
                psdFreeVec(pps->pps_Buffer);
                pps->pps_Buffer = NULL;
                psdFreeVec(pps->pps_Pipes);
                pps->pps_Pipes = NULL;
            }
        } else if (pps->pps_Pipes) {
            for(cnt = 0; cnt < pps->pps_NumPipes; cnt++) {
                pp = pps->pps_Pipes[cnt];
                if (streambase && maxstreams) {
                    pp->pp_StreamID = (UWORD)(streambase + cnt);
                } else {
                    pp->pp_StreamID = 0;
                }
            }
        }
        if(pps->pps_Pipes && streambase && maxstreams) {
            /* stream-tagged pipes exist: make sure the HCD has rings for the
               highest id in use (silent no-op on non-stream backends) */
            pCtxEnsureStreams(ps, pps->pps_Endpoint,
                              (UWORD)(streambase + pps->pps_NumPipes - 1));
        }
        ReleaseSemaphore(&pps->pps_AccessLock);
        return((LONG) count);
    }
    }

    if(packtab) {
        res = (LONG) PackStructureTags(psdstruct, packtab, tags);
    } else {
        res = -1;
    }
    if(savenotifycfg) {
        struct PsdDevice *pd = (struct PsdDevice *) psdstruct;
        struct PsdIFFContext *pic;

        pic = psdGetUsbDevCfg(NEP_PREFS_OWNER, pd->pd_IDString, NULL);
        if(!pic) {
            psdSetUsbDevCfg(NEP_PREFS_OWNER, pd->pd_IDString, NULL, NULL);
            pic = psdGetUsbDevCfg(NEP_PREFS_OWNER, pd->pd_IDString, NULL);
        }
        if(pic) {
            pAddCfgChunk(ps, pic, &pd->pd_NotifyCfg);
            checkcfgupdate = TRUE;
        }
    }
    if(checkcfgupdate) {
        pUpdateGlobalCfg(ps, (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head);
        pCheckCfgChanged(ps);
    }
    if(powercalc) {
        psdCalculatePower(((struct PsdDevice *) psdstruct)->pd_Hardware);
    }
    if(checklinkpower) {
        UWORD newlinkpower = (type == PGA_STACKCFG)
                             ? (UWORD) ps->ps_GlobalCfg->pgc_LinkPowerMgmt
                             : ((struct PsdDevice *) psdstruct)->pd_NotifyCfg.ntc_LinkPowerOverride;
        if(newlinkpower != oldlinkpower) {
            /* Applying the new policy is a series of blocking control transfers
               per device, and this call arrives on the caller's context - the
               MUI task, for NeptuneUSB.  Hand the work to the event handler task,
               which is built to block; it picks the request up on its next
               500ms tick. */
            ps->ps_LinkPowerReq = TRUE;
        }
    }
    if(updatehub) {
        struct PsdDevice *pd = (struct PsdDevice *) psdstruct;
        /* NULL = hardware has no backend bound yet. */
        if(pd->pd_Hardware->phw_HCDOps) {
            pd->pd_Hardware->phw_HCDOps->hop_UpdateHub(ps, pd);
        }
    }
    return(res);
}
/* \\\ */

/* /// "psdSetDeviceConfig()" */
BOOL (psdSetDeviceConfig)(struct PsdPipe * pp asm("a1"), UWORD cfgnum asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdConfig *pc;
    struct PsdDevice *pd = pp->pp_Device;
    LONG ioerr;
    BOOL res = FALSE;

    KPRINTF(2, ("Setting configuration to %ld...\n", cfgnum));

    /* backend builds the endpoint set first (context HCDs: Configure Endpoint;
       legacy: no-op) - the wire SET_CONFIGURATION follows */
    ioerr = pd->pd_Hardware->phw_HCDOps->hop_ConfigureEndpoints(ps, pp, cfgnum);
    if(ioerr) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Endpoint configuration (cfg %ld) for %s/%ld failed: %s (%ld)",
                       cfgnum, pd->pd_Hardware->phw_DevName, pd->pd_Hardware->phw_Unit,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return(FALSE);
    }

    psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE,
                 USR_SET_CONFIGURATION, cfgnum, 0);
    ioerr = psdDoPipe(pp, NULL, 0);
    if(!ioerr) {
        pd->pd_CurrCfg = cfgnum;
        res = TRUE;
    } else {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                "SET_CONFIGURATION for %s/%ld Addr=%lu failed: %s (%ld)",
                                pd->pd_Hardware->phw_DevName, pd->pd_Hardware->phw_Unit, (ULONG)pd->pd_DevAddr,
                                psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(15, ("SET_CONFIGURATION failed %ld!\n", ioerr));
    }
    // update direct link
    Forbid();
    pd->pd_CurrentConfig = NULL;
    pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
    while(pc->pc_Node.ln_Succ) {
        if(pc->pc_CfgNum == pd->pd_CurrCfg) {
            pd->pd_CurrentConfig = pc;
            break;
        }
        pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
    }
    Permit();
    if(!pd->pd_CurrentConfig) {
        psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname, "Device has no current configuration.");
    } else {
        UWORD status = 0;
        // power saving stuff
        if(ps->ps_GlobalCfg->pgc_PowerSaving && (pd->pd_CurrentConfig->pc_Attr & USCAF_REMOTE_WAKEUP)) {
            psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE,
                         USR_SET_FEATURE, UFS_DEVICE_REMOTE_WAKEUP, 0);
            ioerr = psdDoPipe(pp, NULL, 0);
            if(ioerr) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "SET_DEVICE_REMOTE_WAKEUP failed: %s (%ld)",
                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                KPRINTF(15, ("SET_DEVICE_REMOTE_WAKEUP failed %ld!\n", ioerr));
            }
            psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE, USR_GET_STATUS, 0, 0);
            ioerr = psdDoPipe(pp, &status, 2);
            if(!ioerr) {
                if(status & U_GSF_REMOTE_WAKEUP) {
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "Enabled remote wakeup feature for '%s'.",
                                   pd->pd_ProductStr);
                } else {
                    pd->pd_CurrentConfig->pc_Attr &= ~USCAF_REMOTE_WAKEUP;
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Remote wakeup feature for '%s' could not be enabled.",
                                   pd->pd_ProductStr);
                }
            } else {
                /*psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "GET_STATUS failed: %s (%ld)",
                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);*/
                KPRINTF(15, ("GET_STATUS failed %ld!\n", ioerr));
            }
        } else {
            psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE, USR_GET_STATUS, 0, 0);
            ioerr = psdDoPipe(pp, &status, 2);
        }
        if(!ioerr) {
            if((status & U_GSF_SELF_POWERED) && (!(pd->pd_CurrentConfig->pc_Attr & USCAF_SELF_POWERED))) {
                pd->pd_CurrentConfig->pc_Attr |= USCAF_SELF_POWERED;
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Device '%s' says it is currently self-powered. Fixing config.",
                               pd->pd_ProductStr);
            } else if((!(status & U_GSF_SELF_POWERED)) && (pd->pd_CurrentConfig->pc_Attr & USCAF_SELF_POWERED)) {
                pd->pd_CurrentConfig->pc_Attr &= ~USCAF_SELF_POWERED;
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Device '%s' says it is currently bus-powered. Fixing config.",
                               pd->pd_ProductStr);
            }
        } else {
            /*psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "GET_STATUS failed: %s (%ld)",
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);*/
            KPRINTF(15, ("GET_STATUS failed %ld!\n", ioerr));
        }
    }

    if(res && pLinkPowerWanted(ps, pd)) {
        /* reuses the enumeration pipe - no port/pipe allocation on this path.
           No-op on legacy / incapable HCDs. */
        pLinkPowerArm(ps, pd, pp);
    }

    return(res);
}
/* \\\ */

/* /// "psdEnumerateDevice()" */
struct PsdDevice * (psdEnumerateDevice)(struct PsdPipe * pp asm("a1"), struct PsdBase * ps asm("a6"))
{

    struct PsdDevice *pd = pp->pp_Device;
    struct PsdDevice *itpd = pp->pp_Device;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct UsbStdDevDesc usdd;

    UWORD oldflags = 0;
    ULONG oldnaktimeout = 0;

    LONG ioerr = 0;

    STRPTR classname;
    STRPTR vendorname;

    ULONG devclass;

    BOOL hasprodname;
    BOOL haspopupinhibit;

    UWORD cfgnum;

    struct PsdIFFContext *pic;

    ULONG *chnk;

    /* Track whether we successfully assigned an address, for cleanup. */
    BOOL addr_assigned = FALSE;

    KPRINTF(2, ("psdEnumerateDevice(0x%08lx)\n", pp));

    /* Ensure descriptor buffer is not used uninitialised */
    memset(&usdd, 0, sizeof(usdd));

    psdLockWriteDevice(pd);

    oldflags = pp->pp_IOReq.iouh_Flags;
    oldnaktimeout = pp->pp_IOReq.iouh_NakTimeout;

    pp->pp_IOReq.iouh_Flags |= UHFF_NAKTIMEOUT;
    pp->pp_IOReq.iouh_NakTimeout = 1000;

    /* Backend addresses the device (legacy: probe at address 0 + wire
       SET_ADDRESS; context: HCD-owned) and returns the first 8 descriptor
       bytes in usdd. */
    ioerr = pd->pd_Hardware->phw_HCDOps->hop_AddressDevice(ps, pp, &usdd);
    if(ioerr) {
        goto fail_restore;
    }
    addr_assigned = TRUE;

    /*
        We have already received at least the first 8 bytes from the descriptor.
        Validate bMaxPacketSize0 *now* and bail out if it is invalid to avoid
        wedging devices on subsequent control transfers.
    */
    KPRINTF(1, ("Getting MaxPktSize0...\n"));
    {
        /* EP0 max packet is validated per LINK SPEED, not per bcdUSB - LS,
           HS and SS have fixed values the descriptor byte cannot override.
           Only FS has a real choice. Same rule as the context HCD's UPDATE_EP0 validation. */
        BOOL maxpkt_ok = TRUE;
        UWORD maxpkt0;
        UWORD expect = 0;

        if(pd->pd_Flags & PDFF_SUPERSPEED) {
            maxpkt0 = 512;
            expect = 9; /* the SS descriptor byte is an exponent */
        } else if(pd->pd_Flags & PDFF_HIGHSPEED) {
            maxpkt0 = 64;
            expect = 64;
        } else if(pd->pd_Flags & PDFF_LOWSPEED) {
            maxpkt0 = 8;
            expect = 8;
        } else {
            /* FS: literal size, 8/16/32/64 */
            maxpkt0 = usdd.bMaxPacketSize0;
            switch(maxpkt0) {
            case 8:
            case 16:
            case 32:
            case 64:
                break;
            default:
                maxpkt_ok = FALSE;
                break;
            }
        }

        if(!maxpkt_ok) {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Illegal bMaxPacketSize0=%ld for endpoint 0 (bcdUSB=%04lx)",
                           (ULONG)usdd.bMaxPacketSize0, (ULONG)AROS_LE2WORD(usdd.bcdUSB));
            KPRINTF(2, ("Illegal bMaxPacketSize0=%ld (bcdUSB=%04lx)!\n",
                        (ULONG)usdd.bMaxPacketSize0, (ULONG)AROS_LE2WORD(usdd.bcdUSB)));
            ioerr = UHIOERR_CRCERROR;
            goto fail_restore;
        }

        if(expect && (usdd.bMaxPacketSize0 != expect)) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Device reports bMaxPacketSize0=%ld, but the link speed dictates %ld bytes for endpoint 0. Ignoring the descriptor.",
                           (ULONG)usdd.bMaxPacketSize0, (ULONG)maxpkt0);
        }

        pp->pp_IOReq.iouh_MaxPktSize = pd->pd_MaxPktSize0 = maxpkt0;
    }

    KPRINTF(1, ("  MaxPktSize0 = %ld\n", pd->pd_MaxPktSize0));

    /* let the backend apply the now-validated EP0 max packet (context HCDs
       patch the EP0 context here; legacy is implicit per transfer) */
    ioerr = pd->pd_Hardware->phw_HCDOps->hop_UpdateEp0MaxPacket(ps, pp);
    if(ioerr) {
        goto fail_restore;
    }

    KPRINTF(1, ("Getting full descriptor...\n"));
    /* We have set a new address for the device so we need to setup the pipe again */
    psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE, USR_GET_DESCRIPTOR, UDT_DEVICE<<8, 0);
    ioerr = psdDoPipe(pp, &usdd, sizeof(struct UsbStdDevDesc));
    if(ioerr) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "GET_DESCRIPTOR (len %lu) failed: %s (%ld)",
                       sizeof(struct UsbStdDevDesc), psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(15, ("GET_DESCRIPTOR (%lu) failed %ld!\n", sizeof(struct UsbStdDevDesc), ioerr));
        goto fail_restore;
    }

    pAllocDescriptor(pd, (UBYTE *) &usdd);
    pd->pd_Flags |= PDFF_HASDEVDESC;
    pd->pd_USBVers = AROS_LE2WORD(usdd.bcdUSB);
    /* A root hub's speed is its controller's: a USB 2.0 root hub is high
       speed (hub.class enforces split transactions below it), a full-speed
       controller's stays full speed. The SuperSpeed root hub is flagged
       before enumeration (psdEnumerateHardware). */
    if(!pd->pd_Hub && (pd->pd_USBVers >= 0x200) && !(pd->pd_Flags & PDFF_SUPERSPEED)) {
        pd->pd_Flags |= PDFF_HIGHSPEED;
    }
    pd->pd_DevClass = usdd.bDeviceClass;
    pd->pd_DevSubClass = usdd.bDeviceSubClass;
    pd->pd_DevProto = usdd.bDeviceProtocol;
    pd->pd_VendorID = AROS_LE2WORD(usdd.idVendor);
    pd->pd_ProductID = AROS_LE2WORD(usdd.idProduct);
    pd->pd_DevVers = AROS_LE2WORD(usdd.bcdDevice);
    vendorname = psdNumToStr(NTS_VENDORID, (LONG) pd->pd_VendorID, NULL);

    /*
        The USB 3.0 and USB 2.0 LPM specifications define a new USB descriptor called
        the Binary Device Object Store (BOS) for a USB device with bcdUSB > 0x0200.
    */
    if (pd->pd_USBVers > 0x0200) {
        struct PsdBosCaps boscaps;
        if (pFetchBosCaps(pp, &boscaps)) {
            pApplyDeviceBosCapabilities(pd, &boscaps);
        }
    }

    if(usdd.iManufacturer) {
        pd->pd_MnfctrStr = psdGetStringDescriptor(pp, usdd.iManufacturer);
    }

    if(usdd.iProduct) {
        pd->pd_ProductStr = psdGetStringDescriptor(pp, usdd.iProduct);
    }

    if(usdd.iSerialNumber) {
        pd->pd_SerNumStr = psdGetStringDescriptor(pp, usdd.iSerialNumber);
    }

    if(!pd->pd_MnfctrStr) {
        pd->pd_MnfctrStr = psdCopyStr(vendorname ? vendorname : (STRPTR) "n/a");
    }

    if(!pd->pd_ProductStr) {
        hasprodname = FALSE;
        classname = psdNumToStr(NTS_CLASSCODE, (LONG) pd->pd_DevClass, NULL);
        if(classname) {
            pd->pd_ProductStr = psdCopyStrFmt("%s: Vdr=%04lx/PID=%04lx", classname, pd->pd_VendorID, pd->pd_ProductID);
        } else {
            pd->pd_ProductStr = psdCopyStrFmt("Cls=%ld/Vdr=%04lx/PID=%04lx", pd->pd_DevClass, pd->pd_VendorID, pd->pd_ProductID);
        }
    } else {
        hasprodname = TRUE;
    }

    if(!pd->pd_SerNumStr) {
        pd->pd_SerNumStr = psdCopyStr("n/a");
    }

    KPRINTF(2, ("Product     : %s\n"
                "Manufacturer: %s\n"
                "SerialNumber: %s\n",
                pd->pd_ProductStr, pd->pd_MnfctrStr, pd->pd_SerNumStr));
    KPRINTF(2, ("USBVersion: %04lx\n"
                "Class     : %ld\n"
                "SubClass  : %ld\n"
                "DevProto  : %ld\n"
                "VendorID  : %ld\n"
                "ProductID : %ld\n"
                "DevVers   : %04lx\n",
                pd->pd_USBVers, pd->pd_DevClass, pd->pd_DevSubClass, pd->pd_DevProto,
                pd->pd_VendorID, pd->pd_ProductID, pd->pd_DevVers));

    /* check for clones */
    itpd = NULL;
    while((itpd = psdGetNextDevice(itpd))) {
        if(itpd != pd) {
            if((itpd->pd_ProductID == pd->pd_ProductID) &&
               (itpd->pd_VendorID == pd->pd_VendorID) &&
               (itpd->pd_SerNumStr && !strcmp(itpd->pd_SerNumStr, pd->pd_SerNumStr)) &&
               (itpd->pd_CloneCount == pd->pd_CloneCount)) {
                pd->pd_CloneCount++;
                itpd = NULL;
            }
        }
    }

    pd->pd_IDString = psdCopyStrFmt("%s-%04lx-%04lx-%s-%02lx",
                                    pd->pd_ProductStr, pd->pd_VendorID, pd->pd_ProductID,
                                    pd->pd_SerNumStr, pd->pd_CloneCount);

    pStripString(ps, pd->pd_MnfctrStr);
    pStripString(ps, pd->pd_ProductStr);
    pStripString(ps, pd->pd_SerNumStr);

    /* get custom name of device */
    pLockSemExcl(ps, &ps->ps_ConfigLock); // Exclusive lock to avoid deadlock situation when promoting read to write
    pd->pd_OldProductStr = pd->pd_ProductStr;
    pd->pd_ProductStr = NULL;
    haspopupinhibit = FALSE;
    pic = psdGetUsbDevCfg(NEP_PREFS_OWNER, pd->pd_IDString, NULL);
    if(pic) {
        pd->pd_IsNewToMe = FALSE;
        if((pd->pd_ProductStr = pGetStringChunk(ps, pic, IFFCHNK_NAME))) {
            hasprodname = TRUE;
        }
        if((chnk = pFindCfgChunk(ps, pic, IFFCHNK_POPUP))) {
            struct PsdNotifyCfg *poc = (struct PsdNotifyCfg *) chnk;
            CopyMem(((UBYTE *) poc) + 8, ((UBYTE *) &pd->pd_NotifyCfg) + 8,
                    min(AROS_LONG2BE(poc->ntc_Length), AROS_LONG2BE(pd->pd_NotifyCfg.ntc_Length)));
            haspopupinhibit = TRUE;
        }
    } else {
        pd->pd_IsNewToMe = TRUE;
        psdSetUsbDevCfg(NEP_PREFS_OWNER, pd->pd_IDString, NULL, NULL);
    }
    if(!pd->pd_ProductStr) {
        pd->pd_ProductStr = psdCopyStr(pd->pd_OldProductStr);
    }
    if(!haspopupinhibit) {
        if(pd->pd_DevClass == HUB_CLASSCODE) { // hubs default to true
            pd->pd_NotifyCfg.ntc_InhibitPopup = TRUE;
        }
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);

    pd->pd_NumCfgs = usdd.bNumConfigurations;
    KPRINTF(10, ("Device has %ld different configurations\n", pd->pd_NumCfgs));

    if(pGetDevConfig(pp)) {
        cfgnum = 1;
        if(pd->pd_Configs.lh_Head->ln_Succ) {
            cfgnum = ((struct PsdConfig *) pd->pd_Configs.lh_Head)->pc_CfgNum;
        }
        /* Configure the device already during enumeration (original-author quirk
           workaround, present since Poseidon 4.x: some devices misbehave when left
           unconfigured - and an unconfigured device is limited to 100mA anyway).
           The class scan re-selects configs as needed; its pd_CurrCfg check avoids
           a duplicate wire SET_CONFIGURATION for the common single-config case. */
        psdSetDeviceConfig(pp, cfgnum);
        {
            if(!hasprodname) {
                devclass = pd->pd_DevClass;
                pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
                while(pc->pc_Node.ln_Succ) {
                    pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                    while(pif->pif_Node.ln_Succ) {
                        if(pif->pif_IfClass) {
                            if(!devclass) {
                                devclass = pif->pif_IfClass;
                            } else {
                                if(devclass != pif->pif_IfClass) {
                                    devclass = 0;
                                    break;
                                }
                            }
                        }
                        pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                    }
                    pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
                }
                if(devclass) {
                    classname = psdNumToStr(NTS_CLASSCODE, (LONG) devclass, NULL);
                    if(classname) {
                        psdFreeVec(pd->pd_ProductStr);
                        if(vendorname) {
                            pd->pd_ProductStr = psdCopyStrFmt("%s (%s/%04lx)",
                                                             classname, vendorname, pd->pd_ProductID);
                        } else {
                            pd->pd_ProductStr = psdCopyStrFmt("%s (%04lx/%04lx)",
                                                             classname, pd->pd_VendorID, pd->pd_ProductID);
                        }
                    }
                }
            }
            pFixBrokenConfig(pp);
            pp->pp_IOReq.iouh_Flags = oldflags;
            pp->pp_IOReq.iouh_NakTimeout = oldnaktimeout;
            psdUnlockDevice(pd);
            psdCalculatePower(pd->pd_Hardware);
            return(pd);
        }
    } else {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Could not acquire device configuration for %s",
                       pd->pd_ProductStr ? pd->pd_ProductStr : (STRPTR) "new device");
        KPRINTF(15, ("GetDevConfig() failed\n"));
    }

    /* Although the device failed to configure fully, maybe some firmware will use it anyway */
    pp->pp_IOReq.iouh_Flags = oldflags;
    pp->pp_IOReq.iouh_NakTimeout = oldnaktimeout;
    psdUnlockDevice(pd);
    return(pd);

fail_restore:
    /* If we assigned an address but later fail, do not keep it active in the pipe state. */
    if(addr_assigned) {
        pp->pp_IOReq.iouh_DevAddr = 0;
        pd->pd_Flags &= ~(PDFF_HASDEVADDR | PDFF_CONNECTED);
    }
    pp->pp_IOReq.iouh_Flags = oldflags;
    pp->pp_IOReq.iouh_NakTimeout = oldnaktimeout;

    psdAddErrorMsg0(RETURN_FAIL, (STRPTR) libname,
                    "Device enumeration failed.");
    psdUnlockDevice(pd);
    return(NULL);

}
/* \\\ */

/* /// "psdGetNextDevice()" */
struct PsdDevice * (psdGetNextDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdHardware *phw;

    KPRINTF(1, ("psdGetNextDevice(0x%08lx)\n", pd));
    if(pd) {
        /* Is there another device node in the current hardware? */
        if(pd->pd_Node.ln_Succ->ln_Succ) {
            return((struct PsdDevice *) pd->pd_Node.ln_Succ);
        }
        /* No, then check if there's another hardware to scan */
        phw = (struct PsdHardware *) pd->pd_Hardware->phw_Node.ln_Succ;
    } else {
        /* No context, start with first hardware */
        phw = (struct PsdHardware *) ps->ps_Hardware.lh_Head;
    }
    while(phw->phw_Node.ln_Succ) {
        pd = (struct PsdDevice *) phw->phw_Devices.lh_Head;
        /* Is this an valid entry, or is the list empty? */
        if(pd->pd_Node.ln_Succ) {
            return(pd);
        }
        phw = (struct PsdHardware *) phw->phw_Node.ln_Succ;
    }
    /* End of list reached */
    return(NULL);
}
/* \\\ */

/* /// "psdSuspendBindings()" */
BOOL (psdSuspendBindings)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    BOOL res = FALSE;
    IPTR suspendable;
    BOOL force = FALSE;

    KPRINTF(5, ("psdSuspendBindings(0x%08lx)\n", pd));
    if(pd) {
        /* pd_CurrentConfig is NULL for an enumerated but unconfigured device */
        if(ps->ps_GlobalCfg->pgc_ForceSuspend && pd->pd_CurrentConfig &&
           (pd->pd_CurrentConfig->pc_Attr & USCAF_REMOTE_WAKEUP)) {
            force = TRUE;
        }
        // ask existing bindings to go to suspend first -- if they don't support it, break off
        if(pd->pd_DevBinding) {
            if(pd->pd_Flags & PDFF_APPBINDING) {
                if(!force) {
                    // can't suspend application binding devices
                    psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                   "Cannot suspend with application binding on '%s'.",
                                   pd->pd_ProductStr);
                    return FALSE;
                }
                psdReleaseDevBinding(pd);
            }
            if((puc = pd->pd_ClsBinding)) {
                suspendable = 0;
                usbGetAttrs(UGA_CLASS, NULL, UCCA_SupportsSuspend, &suspendable, TAG_END);
                if(suspendable) {
                    res = usbDoMethod(UCM_AttemptSuspendDevice, pd->pd_DevBinding);
                    if(!res) {
                        /* the class said no - it is using the device. That is
                           an answer, not a failure. */
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                       "Class '%s' declined to suspend device '%s'.",
                                       puc->puc_Node.ln_Name, pd->pd_ProductStr);
                        return FALSE;
                    }
                } else {
                    if(pd->pd_IOBusyCount) {
                        if(!force) {
                            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                           "Class '%s' does not support suspending.",
                                           puc->puc_Node.ln_Name);
                            return FALSE;
                        } else {
                            psdReleaseDevBinding(pd);
                        }
                    } else {
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Class '%s' does not support suspending, but has no active IO. Suspending anyway.",
                                       puc->puc_Node.ln_Name);
                    }
                }
            }
        }
        if((pc = pd->pd_CurrentConfig)) {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            while(pif->pif_Node.ln_Succ) {
                if(pif->pif_IfBinding) {
                    if((puc = pif->pif_ClsBinding)) {
                        suspendable = 0;
                        usbGetAttrs(UGA_CLASS, NULL, UCCA_SupportsSuspend, &suspendable, TAG_END);
                        if(suspendable) {
                            res = usbDoMethod(UCM_AttemptSuspendDevice, pif->pif_IfBinding);
                            if(!res) {
                                /* said no, as above: an answer, not a failure */
                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                               "%s declined to suspend device '%s'.",
                                               puc->puc_Node.ln_Name, pd->pd_ProductStr);
                                return FALSE;
                            }
                        } else {
                            if(pd->pd_IOBusyCount) {
                                if(!force) {

                                    psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                                   "%s does not support suspending.",
                                                   puc->puc_Node.ln_Name);
                                    return FALSE;
                                } else {
                                    psdReleaseIfBinding(pif);
                                }
                            } else {
                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                               "%s does not support suspending, but has no active IO. Suspending anyway.",
                                               puc->puc_Node.ln_Name);
                            }
                        }
                    }
                }
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
        }
        return TRUE;
    }
    return FALSE;
}
/* \\\ */

/* /// "pArmRemoteWakeup()" */
/* Arm DEVICE_REMOTE_WAKEUP right before the port parks.  Enumeration only
   arms when power saving was on at SET_CONFIGURATION time, so a device
   plugged in before the toggle would suspend fine but never wake.  Keyed on
   the config's wakeup capability, not the power-saving setting: an explicitly
   suspended device should be wakeable either way.  Failures only warn - the
   suspend itself proceeds.  Runs on the caller's context (own port + pipe)
   and must run while EP0 is still live, i.e. before SET_SUSPEND(1) quiesces
   the rings. */
static void pArmRemoteWakeup(struct PsdBase *ps, struct PsdDevice *pd)
{
    struct MsgPort *mp;
    struct PsdPipe *pp;
    UWORD status = 0;
    LONG ioerr;

    if(!(pd->pd_CurrentConfig && (pd->pd_CurrentConfig->pc_Attr & USCAF_REMOTE_WAKEUP))) {
        return;
    }
    if((mp = CreateMsgPort())) {
        if((pp = psdAllocPipe(pd, mp, NULL))) {
            psdSetAttrs(PGA_PIPE, pp,
                        PPA_NakTimeout, TRUE,
                        PPA_NakTimeoutTime, 1000,
                        TAG_END);
            psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE,
                         USR_SET_FEATURE, UFS_DEVICE_REMOTE_WAKEUP, 0);
            ioerr = psdDoPipe(pp, NULL, 0);
            if(ioerr) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "SET_DEVICE_REMOTE_WAKEUP failed: %s (%ld)",
                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            }
            psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE, USR_GET_STATUS, 0, 0);
            ioerr = psdDoPipe(pp, &status, 2);
            if((!ioerr) && !(status & U_GSF_REMOTE_WAKEUP)) {
                pd->pd_CurrentConfig->pc_Attr &= ~USCAF_REMOTE_WAKEUP;
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Remote wakeup feature for '%s' could not be enabled.",
                               pd->pd_ProductStr);
            }
            psdFreePipe(pp);
        }
        DeleteMsgPort(mp);
    }
}
/* \\\ */

/* /// "psdSuspendDevice()" */
BOOL (psdSuspendDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    struct PsdDevice *hubpd;
    APTR binding;
    BOOL res = FALSE;
    BOOL agreed = FALSE;

    KPRINTF(5, ("psdSuspendDevice(0x%08lx)\n", pd));
    if(pd) {
        if(pd->pd_Flags & PDFF_SUSPENDED) {
            return TRUE;
        }
        STRPTR refusal = pSuspendRefusal(pd);
        if(refusal) {
            /* refused before the bindings stop, so there is nothing to roll
               back: degrade by keeping the device awake */
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Cannot suspend '%s': %s.",
                           pd->pd_ProductStr, refusal);
            return FALSE;
        }
        hubpd = pd->pd_Hub;

        psdLockWriteDevice(pd);
        res = agreed = psdSuspendBindings(pd);
        psdUnlockDevice(pd);
        if(res && hubpd) {
            /* wake arming needs a live EP0 - before the ring quiesce below.
               Skipped for a root hub: its EP0 is emulated inside the HCD and
               there is no upstream link it could wake the host over. */
            pArmRemoteWakeup(ps, pd);
        }
        if(res && pd->pd_Hardware->phw_ContextBackend) {
            /* quiesce the endpoint rings before the hub class parks the port
               in U3 (xHCI 4.15.1) */
            struct UhcdSetSuspend sso;
            memset(&sso, 0, sizeof(sso));
            sso.sso_DeviceHandle = pd->pd_Handle;
            sso.sso_Suspend = 1;
            res = (pCtxDoOpOnDevice(ps, pd, NSCMD_USB_SET_SUSPEND, &sso, sizeof(sso)) == 0);
        }
        if(res) {
            /* Only the hub class writes DA_IsSuspended, so if it never runs the
               device is not suspended, however well the steps above went. */
            res = FALSE;
            if(hubpd) {
                psdLockReadDevice(pd);
                if((binding = hubpd->pd_DevBinding) && (puc = hubpd->pd_ClsBinding)) {
                    res = usbDoMethod(UCM_HubSuspendDevice, binding, pd);
                }
                psdUnlockDevice(pd);
            } else {
                /* Root hub: there is no parent hub to park a port on and no
                   upstream link to drive to U3, so "suspended" means the whole
                   subtree below it is suspended and its own class binding has
                   gone quiet - which is exactly what psdSuspendBindings() above
                   achieved: both hub classes implement UCM_AttemptSuspendDevice
                   as "psdSuspendDevice every downstream device, and only if all
                   of them succeed abort EP1 and clear nch_Running".

                   Documented exception to the sole-writer rule of
                   docs/hub.class-architecture.md section 6, rule 4: for a ROOT device the core
                   owns PDFF_SUSPENDED, because no hub class can - there is no
                   parent hub class to own it.  Written strictly AFTER the
                   bindings stopped and never before: the child suspends above
                   run control transfers on THIS device's EP0 pipe, and
                   psdDoPipe() transparently resumes a PDFF_SUSPENDED device.

                   Note a USB3 controller has two root devices (the SuperSpeed
                   and the USB2 root hub), so this suspends one root hub view,
                   not the whole controller. */
                psdSetAttrs(PGA_DEVICE, pd, DA_IsSuspended, TRUE, TAG_END);
                psdSendEvent(EHMB_DEVSUSPENDED, pd, NULL);
                res = TRUE;
            }
        }
        if(!res) {
            /* Roll back. Any failure above - a binding that refused halfway
               through psdSuspendBindings(), a failed ring quiesce, a parent hub
               with no class binding, a hub that could not park the port - leaves
               the device half suspended: bindings stopped and, on a context HCD,
               endpoint rings quiesced, while PDFF_SUSPENDED is still CLEAR
               because only the hub class sets it. Nothing would ever undo that:
               psdDoPipe()'s auto-resume keys off the flag so it never fires, and
               the idle sweep zeroes pd_LastActivity after every attempt while
               the suspended bindings issue no IO to re-stamp it, so the device
               is never revisited.
               This must run OUTSIDE the device lock: psdResumeBindings() can
               reach psdHubReleaseDevBinding(), which takes psdLockWriteDevice()
               on this same device, and a shared->exclusive promotion only
               succeeds for the sole reader. */
            if(hubpd && (hubpd->pd_Flags & PDFF_CONNECTED)) {
                /* the park may have been delivered even though the transfer
                   reported an error - unpark before resuming. Skipped for a hub
                   that is already gone: it would only cost another timeout, and
                   for a root hub, which has no parent to unpark. */
                psdLockReadDevice(pd);
                if((binding = hubpd->pd_DevBinding) && (puc = hubpd->pd_ClsBinding)) {
                    usbDoMethod(UCM_HubResumeDevice, binding, pd);
                }
                psdUnlockDevice(pd);
            }
            /* psdResumeBindings() re-issues SET_SUSPEND(0) itself and is
               idempotent, so it is the whole binding + ring rollback. */
            psdResumeBindings(pd);
        }
    }
    /* A binding that would not have it has said so itself
       (psdSuspendBindings); what failed after the bindings agreed has not. */
    if(pd && agreed && !res) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Suspending of device '%s' failed.",
                       pd->pd_ProductStr);
    }
    return(res);
}
/* \\\ */

/* /// "psdResumeBindings()" */
BOOL (psdResumeBindings)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    BOOL res = FALSE;
    BOOL rescan = FALSE;

    KPRINTF(5, ("psdResumeBindings(0x%08lx)\n", pd));
    if(pd) {
        if(pd->pd_Hardware->phw_ContextBackend &&
           (pd->pd_Hardware->phw_CtxCmdMask & UHCD_CTXCMD_BIT(NSCMD_USB_SET_SUSPEND)) &&
           pd->pd_Handle) {
            /* the link is back in U0 - software resume AND device remote wake
               both funnel through here (the hub classes call this directly on
               a detected wake) - so restart the endpoint rings quiesced by
               SET_SUSPEND(1) before the bindings start talking; idempotent if
               they never were quiesced */
            struct UhcdSetSuspend sso;
            memset(&sso, 0, sizeof(sso));
            sso.sso_DeviceHandle = pd->pd_Handle;
            pCtxDoOpOnDevice(ps, pd, NSCMD_USB_SET_SUSPEND, &sso, sizeof(sso));
        }
        /* The link power sweep skips suspended devices - it would wake them
           through psdDoPipe()'s auto-resume - so a policy change that landed
           while this device was parked has not reached it.  Ask for a fresh
           sweep; the event handler task runs it.  Non-blocking. */
        if(pd->pd_CurrentConfig &&
           (pLinkPowerWanted(ps, pd) != ((pd->pd_LpmArmed & PDLPMF_POLICY) ? TRUE : FALSE))) {
            ps->ps_LinkPowerReq = TRUE;
        }
        // ask existing bindings to resume -- if they don't support it, rebind
        if(pd->pd_DevBinding) {
            if(!(pd->pd_Flags & PDFF_APPBINDING)) {
                if((puc = pd->pd_ClsBinding)) {
                    res = usbDoMethod(UCM_AttemptResumeDevice, pd->pd_DevBinding);
                    if(!res) {
                        // if the device couldn't resume, better get rid of the binding
                        psdReleaseDevBinding(pd);
                        rescan = TRUE;
                    }
                }
            }
        }
        if((pc = pd->pd_CurrentConfig)) {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            while(pif->pif_Node.ln_Succ) {
                if(pif->pif_IfBinding) {
                    if((puc = pif->pif_ClsBinding)) {
                        res = usbDoMethod(UCM_AttemptResumeDevice, pif->pif_IfBinding);
                        if(!res) {
                            // didn't want to resume, so rebind
                            psdReleaseIfBinding(pif);
                            rescan = TRUE;
                        }
                    }
                    /* every bound interface, the same walk as psdSuspendBindings():
                       the AROS original broke out after the first one, which left
                       the other bindings of a composite device (two-interface HID
                       receivers, headsets) stopped for good after a resume */
                }
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
        }
        if(rescan) {
            psdClassScan();
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "psdResumeDevice()" */
BOOL (psdResumeDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    struct PsdDevice *hubpd;
    APTR binding;
    BOOL res = FALSE;

    KPRINTF(5, ("psdResumeDevice(0x%08lx)\n", pd));
    if(pd) {
        if(!(pd->pd_Flags & PDFF_SUSPENDED)) {
            return(TRUE);
        }
        hubpd = pd->pd_Hub;
        if(hubpd) {
            psdLockWriteDevice(pd);
            if((binding = hubpd->pd_DevBinding) && (puc = hubpd->pd_ClsBinding)) {
                res = usbDoMethod(UCM_HubResumeDevice, binding, pd);
            }
            psdUnlockDevice(pd);
        } else {
            /* Root hub: the mirror of the suspend branch, except that the flag
               is cleared FIRST.  psdResumeBindings() below reaches
               UCM_AttemptResumeDevice in the hub class, which unparks every
               child port with control transfers on THIS device's EP0 pipe - and
               psdDoPipe() transparently resumes a PDFF_SUSPENDED device, so with
               the flag still set that would recurse straight back in here. */
            psdSetAttrs(PGA_DEVICE, pd, DA_IsSuspended, FALSE, TAG_END);
            psdSendEvent(EHMB_DEVRESUMED, pd, NULL);
            res = TRUE;
        }

        if(res) {
            /* the ctx ring restart (SET_SUSPEND(0)) lives in psdResumeBindings,
               shared with the hub classes' remote-wake path */
            psdResumeBindings(pd);
        }
    }

    return(res);
}
/* \\\ */

/* /// "psdSafeEjectDevice()" */
/*
 * "Safely remove hardware": ask every class bound to the device that
 * advertises UCCA_SupportsSafeEject to quiesce what it holds (storage classes
 * flush and unmount their volumes, refusing while files are open), then take
 * the device off the bus by disabling its hub port, so the user can unplug it
 * without losing data.
 *
 * Returns SAFEEJECT_OK (safe to remove - the port is going down and
 * EHMB_REMDEVICE follows), SAFEEJECT_BUSY (something is still in use; busybuf
 * names it and nothing was changed), SAFEEJECT_FAIL, or
 * SAFEEJECT_NOT_SUPPORTED when no bound class can do this.  busybuf may be
 * NULL; 64 bytes is plenty.
 *
 * Must be called from a Process (the classes talk to filesystems) and never
 * with a device lock held: an eject blocks for as long as the filesystems and
 * the drive need.
 */
IPTR (psdSafeEjectDevice)(struct PsdDevice * pd asm("a0"), STRPTR busybuf asm("a1"),
                          ULONG busybuflen asm("d0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(5, ("psdSafeEjectDevice(0x%08lx)\n", pd));
    if(!pd) {
        return SAFEEJECT_FAIL;
    }
    if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Safe eject of '%s' needs a Process context.", pd->pd_ProductStr);
        return SAFEEJECT_FAIL;
    }

    /* Collect under the lock, call outside it: a class blocks on filesystem
       packets and on IO to its own exec device, and the task serving that IO
       must not be stuck behind our lock. */
    struct EjectBindings eb;
    psdLockReadDevice(pd);
    pCollectEjectBindings(pd, &eb);
    psdUnlockDevice(pd);
    if(!eb.eb_Count) {
        return SAFEEJECT_NOT_SUPPORTED;
    }

    IPTR res = SAFEEJECT_OK;
    ULONG idx;
    for(idx = 0; idx < eb.eb_Count; idx++) {
        struct PsdUsbClass *puc = eb.eb_Classes[idx];
        res = usbDoMethod(UCM_SafeEject, eb.eb_Bindings[idx], busybuf, busybuflen);
        if(res != SAFEEJECT_OK) {
            psdAddErrorMsg((res == SAFEEJECT_BUSY) ? RETURN_WARN : RETURN_ERROR, (STRPTR) libname,
                           "%s refused to eject '%s'.",
                           puc->puc_Node.ln_Name, pd->pd_ProductStr);
            return res;
        }
    }

    /* Off the bus.  Asynchronous by nature (the hub class does the work in its
       own task), so the caller watches for EHMB_REMDEVICE to know it landed. */
    if(pd->pd_Hub) {
        psdDoHubMethod(pd, UCM_HubDisablePort, pd->pd_Hub, pd->pd_HubPort);
    }
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "'%s' can now be removed safely.", pd->pd_ProductStr);
    return SAFEEJECT_OK;
}
/* \\\ */

/* /// "psdResetDevice()" */
/*
 * Full device reset without teardown:
 * hot-reset the port through the parent hub's class, re-address the preserved
 * HCD handle (NSCMD_USB_RESET_DEVICE:
 * xHCI Reset Device + BSR=0 Address Device - every endpoint context but EP0
 * is dropped and everything in flight fails IOERR_ABORTED), then restore the
 * configuration: endpoint contexts for the CURRENT alternates + wire
 * SET_CONFIGURATION (psdSetDeviceConfig), plus a wire SET_INTERFACE for each
 * non-default alternate (the contexts already match it).
 *
 * Contract: the CALLER owns quiescence of its own traffic before calling -
 * everything still in flight is failed, not replayed.  Bindings survive; the
 * caller re-establishes its endpoint state afterwards (pep_StreamsAlloc and
 * pep_Token are invalidated and re-minted by the configure step, so stream
 * users re-run their PPA_StreamID setup).
 *
 * FALSE = nothing happened or the device is lost: no context backend, the
 * HCD does not advertise NSCMD_USB_RESET_DEVICE, no handle, a root hub, or a
 * step failed.  The caller decides how to degrade.
 */
BOOL (psdResetDevice)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    struct PsdDevice *hubpd;
    struct UhcdResetDevice rdo;
    APTR binding;
    struct MsgPort *mp;
    struct PsdPipe *pp;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    LONG ioerr;
    BOOL res = FALSE;

    KPRINTF(5, ("psdResetDevice(0x%08lx)\n", pd));
    if(!pd) {
        return FALSE;
    }
    if(!(pd->pd_Hardware->phw_ContextBackend &&
         (pd->pd_Hardware->phw_CtxCmdMask & UHCD_CTXCMD_BIT(NSCMD_USB_RESET_DEVICE)) &&
         pd->pd_Handle && pd->pd_Hub)) {
        /* degrade contract: an old driver (or a root hub, which is not
           port-resettable) simply cannot do this */
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Cannot reset device '%s': no backend support.",
                       pd->pd_ProductStr);
        return FALSE;
    }
    hubpd = pd->pd_Hub;

    /* 1. hot-reset the port; the hub class owns the port and the method runs
       in the hub task, so it cannot race that task's change processing */
    psdLockReadDevice(pd);
    if((binding = hubpd->pd_DevBinding) && (puc = hubpd->pd_ClsBinding)) {
        res = usbDoMethod(UCM_HubResetPort, binding, pd);
    }
    psdUnlockDevice(pd);
    if(!res) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Port reset for '%s' failed.", pd->pd_ProductStr);
        return FALSE;
    }

    psdLockWriteDevice(pd);

    /* 2. re-address the preserved handle */
    memset(&rdo, 0, sizeof(rdo));
    rdo.rdo_DeviceHandle = pd->pd_Handle;
    ioerr = pCtxDoOpOnDevice(ps, pd, NSCMD_USB_RESET_DEVICE, &rdo, sizeof(rdo));
    if(ioerr) {
        psdUnlockDevice(pd);
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "RESET_DEVICE for '%s' failed: %s (%ld)",
                       pd->pd_ProductStr,
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return FALSE;
    }
    psdDelayMS(CTX_SETADDRESS_RECOVERY_MS); /* RESET_DEVICE re-addressed it on the wire */

    /* 3. restore the configuration */
    res = FALSE;
    if((mp = CreateMsgPort())) {
        if((pp = psdAllocPipe(pd, mp, NULL))) {
            res = psdSetDeviceConfig(pp, pd->pd_CurrCfg);
            if(res && (pc = pd->pd_CurrentConfig)) {
                /* re-assert every non-default alternate on the wire - the
                   configure step already built the contexts for the current
                   alternates, only the device fell back to alt 0 */
                for(pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                    pif->pif_Node.ln_Succ;
                    pif = (struct PsdInterface *) pif->pif_Node.ln_Succ) {
                    if(pif->pif_Alternate) {
                        psdPipeSetup(pp, URTF_STANDARD|URTF_INTERFACE,
                                     USR_SET_INTERFACE,
                                     pif->pif_Alternate, pif->pif_IfNum);
                        ioerr = psdDoPipe(pp, NULL, 0);
                        if(ioerr && (ioerr != UHIOERR_STALL)) {
                            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                           "SET_INTERFACE (if %ld alt %ld) after reset failed: %s (%ld)",
                                           (ULONG) pif->pif_IfNum, (ULONG) pif->pif_Alternate,
                                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                            res = FALSE;
                            break;
                        }
                    }
                }
            }
            psdFreePipe(pp);
        }
        DeleteMsgPort(mp);
    }
    psdUnlockDevice(pd);

    /* the U1/U2/LTM arming the reset cleared is restored by psdSetDeviceConfig()
       above (pLinkPowerArm) */
    if(!res) {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Restoring configuration of '%s' after reset failed.",
                       pd->pd_ProductStr);
    }
    return(res);
}
/* \\\ */

/* /// "psdFindDeviceA()" */
struct PsdDevice * (psdFindDeviceA)(struct PsdDevice * pd asm("a0"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct TagItem *ti;
    BOOL takeit;
    KPRINTF(2, ("psdFindDeviceA(0x%08lx, 0x%08lx)\n", pd, tags));
    while((pd = psdGetNextDevice(pd))) {
        takeit = TRUE;
        if((ti = FindTagItem(DA_ProductID, tags))) {
            if(ti->ti_Data != pd->pd_ProductID) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_VendorID, tags))) {
            if(ti->ti_Data != pd->pd_VendorID) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_Class, tags))) {
            if(ti->ti_Data != pd->pd_DevClass) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_SubClass, tags))) {
            if(ti->ti_Data != pd->pd_DevSubClass) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_Protocol, tags))) {
            if(ti->ti_Data != pd->pd_DevProto) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_Version, tags))) {
            if(ti->ti_Data != pd->pd_DevVers) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_SerialNumber, tags))) {
            if(!pd->pd_SerNumStr || strcmp((STRPTR) ti->ti_Data, pd->pd_SerNumStr)) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_ProductName, tags))) {
            if(!pd->pd_ProductStr || strcmp((STRPTR) ti->ti_Data, pd->pd_ProductStr)) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_Manufacturer, tags))) {
            if(!pd->pd_MnfctrStr || strcmp((STRPTR) ti->ti_Data, pd->pd_MnfctrStr)) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_IDString, tags))) {
            if(!pd->pd_IDString || strcmp((STRPTR) ti->ti_Data, pd->pd_IDString)) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_Binding, tags))) {
            if(ti->ti_Data != (IPTR) pd->pd_DevBinding) {
                takeit = FALSE;
            }
        }
        if((ti = FindTagItem(DA_HubDevice, tags))) {
            if(ti->ti_Data != (IPTR) pd->pd_Hub) {
                takeit = FALSE;
            }
        }

        if(takeit) {
            return(pd);
        }
    }
    return(NULL);
}
/* \\\ */

/* *** Hardware *** */

/* /// "pFindHardware()" */
/* The hardware entry for this name and unit, or NULL.
 *
 * Identity comes from <hwmatch.h>, shared with NeptuneUSB and the CLI tools: the
 * trailing path component, compared case-insensitively. This used to strip the
 * *query* progressively and compare each suffix against the unstripped stored
 * name, which matched "DEVS:USBHardware/xhci.device" against a stored bare
 * "xhci.device" but not the other way round -- so a prefs file holding the bare
 * spelling against a live entry that carried a path added a second controller. */
struct PsdHardware * pFindHardware(struct PsdBase * ps, STRPTR name, ULONG unit)
{
    struct PsdHardware *phw;
    Forbid();
    phw = (struct PsdHardware *) ps->ps_Hardware.lh_Head;
    while(phw->phw_Node.ln_Succ) {
        if(psdHwMatch(phw->phw_DevName, phw->phw_Unit, name, unit)) {
            Permit();
            return(phw);
        }
        phw = (struct PsdHardware *) phw->phw_Node.ln_Succ;
    }
    Permit();
    return(NULL);
}
/* \\\ */

static VOID pFreeDevAndBindings(struct PsdBase * ps, struct PsdDevice *pd)
{
    if (pd) {
        pFreeBindings(ps, pd);
        pFreeDevice(ps, pd);
    }
}

/* /// "psdEnumerateHardware()" */
struct PsdDevice * (psdEnumerateHardware)(struct PsdHardware * phw asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdEnumerateHardware(0x%08lx)\n", phw));

    struct MsgPort *mp = CreateMsgPort();
    if (!mp)
    {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR)libname,
                        "Could not create MsgPort for root hub enumeration.");
        return NULL;
    }

    /* ------------------------------------------------------------
     * 1) Create a device + pipe and run USBRESET once.
     * ------------------------------------------------------------ */
    Forbid();
    struct PsdDevice *probe_pd = psdAllocDevice(phw);
    Permit();

    if (!probe_pd)
    {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR)libname,
                        "Could not allocate probe device for root hub enumeration.");
        DeleteMsgPort(mp);
        return NULL;
    }

    struct PsdPipe *probe_pp = psdAllocPipe(probe_pd, mp, NULL);
    if (!probe_pp)
    {
        pFreeDevAndBindings(ps, probe_pd);
        DeleteMsgPort(mp);
        return NULL;
    }

    probe_pd->pd_Flags |= PDFF_CONNECTED;

    probe_pp->pp_IOReq.iouh_Req.io_Command = UHCMD_USBRESET;
    LONG ioerr = psdDoPipe(probe_pp, NULL, 0);
    if (ioerr == UHIOERR_HOSTERROR)
    {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR)libname,
                        "UHCMD_USBRESET reset failed.");
        psdFreePipe(probe_pp);
        pFreeDevAndBindings(ps, probe_pd);
        DeleteMsgPort(mp);
        return NULL;
    }

    psdDelayMS(100);

    /* ------------------------------------------------------------
     * 1) If the HCD is USB3-capable, try to enumerate the SuperSpeed root hub.
     * ------------------------------------------------------------ */
    phw->phw_RootDevice = NULL;
    if (phw->phw_ContextBackend && (phw->phw_Capabilities & UHCF_USB30))
    {
        probe_pd->pd_Flags &= ~(PDFF_HIGHSPEED | PDFF_LOWSPEED);
        probe_pd->pd_Flags |= PDFF_SUPERSPEED;

        /* switch the reset pipe over to control transfers, otherwise the
           first GET_DESCRIPTOR is dispatched as another UHCMD_USBRESET */
        probe_pp->pp_IOReq.iouh_Req.io_Command = UHCMD_CONTROLXFER;

        if (psdEnumerateDevice(probe_pp))
        {
            KPRINTF(1, ("SuperSpeed RootHub Enumeration finished!\n"));
            psdAddErrorMsg0(RETURN_OK, (STRPTR)libname,
                            "SuperSpeed root hub has been enumerated.");

            phw->phw_RootDevice = probe_pd;

            psdSendEvent(EHMB_ADDDEVICE, probe_pd, NULL);
        }
        else
        {
            KPRINTF(1, ("SuperSpeed RootHub enumeration failed; will try normal hub.\n"));
        }
    }

    /* The probe pipe is never needed past this point; the probe device
       survives only if it became the SuperSpeed root device above. */
    psdFreePipe(probe_pp);
    probe_pp = NULL;
    if (phw->phw_RootDevice == NULL)
    {
        pFreeDevAndBindings(ps, probe_pd);
        probe_pd = NULL;
    }

    /* ------------------------------------------------------------
     * 2) Enumerate the "normal" root hub if:
     *    (a) link is not SS, or SS hub not found, OR
     *    (b) SS hub found but driver reports >1 root hub.
     * ------------------------------------------------------------ */
    if (phw->phw_RootDevice == NULL || (phw->phw_NumRootHubs > 1))
    {
        BOOL hubconnect = FALSE;

        Forbid();
        struct PsdDevice *pd = psdAllocDevice(phw);
        Permit();

        if (!pd)
        {
            psdAddErrorMsg0(RETURN_FAIL, (STRPTR)libname,
                            "Could not allocate probe device for root hub enumeration.");
            DeleteMsgPort(mp);
            return phw->phw_RootDevice;
        }
        struct PsdPipe *pp = psdAllocPipe(pd, mp, NULL);
        if (!pp)
        {
            pFreeDevAndBindings(ps, pd);
            DeleteMsgPort(mp);
            return phw->phw_RootDevice;
        }

        /* No speed flag: the root hub is the driver's emulation, not a device
           on a link. It is enumerated as full speed - its own bMaxPacketSize0
           is taken - and promoted to high speed by its bcdUSB
           (psdEnumerateDevice). A full-speed controller's stays full speed. */
        pd->pd_Flags |= PDFF_CONNECTED;
        pp->pp_IOReq.iouh_Req.io_Command = UHCMD_CONTROLXFER;

        KPRINTF(1, ("Enumerating normal RootHub...\n"));
        if (psdEnumerateDevice(pp))
        {
            hubconnect = TRUE;

            KPRINTF(1, ("RootHub Enumeration finished!\n"));
            psdAddErrorMsg0(RETURN_OK, (STRPTR)libname,
                            "Root hub has been enumerated.");

            /* Preserve SS root device as phw_RootDevice if present; only set if none. */
            if (!phw->phw_RootDevice)
                phw->phw_RootDevice = pd;

            psdSendEvent(EHMB_ADDDEVICE, pd, NULL);
        }
        else
        {
            KPRINTF(1, ("Failed to enumerate normal RootHub\n"));
        }

        psdFreePipe(pp);

        if (!hubconnect)
        {
            pFreeDevAndBindings(ps, pd);
        }
    }

    DeleteMsgPort(mp);

    if (!phw->phw_RootDevice)
    {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR)libname,
                        "Root hub enumeration failed.");
        return NULL;
    }

    return phw->phw_RootDevice;
}
/* \\\ */

/* /// "psdRemHardware()" */
void (psdRemHardware)(struct PsdHardware * phw asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd;
    struct PsdHardware *cmphw;
    BOOL linked = FALSE;

    KPRINTF(5, ("FreeHardware(0x%08lx)\n", phw));

    /* A caller that cached a PsdHardware pointer can outlive it -- NeptuneUSB keeps
       one per GUI row -- and everything below dereferences it. Addresses only, so
       a stale pointer is never followed. Forbid() rather than the PBase semaphore,
       for the same reason pFindHardware() walks this list that way: it cannot
       deadlock against whatever the caller already holds, and the walk is short. */
    Forbid();
    cmphw = (struct PsdHardware *) ps->ps_Hardware.lh_Head;
    while(cmphw->phw_Node.ln_Succ) {
        if(cmphw == phw) {
            linked = TRUE;
            break;
        }
        cmphw = (struct PsdHardware *) cmphw->phw_Node.ln_Succ;
    }
    Permit();

    if(!linked) {
        /* Warn rather than stay silent: this stops being a crash, it must not
           become invisible. */
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "psdRemHardware: 0x%08lx is not on the hardware list.", phw);
        return;
    }

    pd = (struct PsdDevice *) phw->phw_Devices.lh_Head;
    while(pd->pd_Node.ln_Succ) {
        pFreeBindings(ps, pd);
        /* before the free; the note still arrives after it (identity only) */
        psdSendEvent(EHMB_REMDEVICE, pd, NULL);
        pFreeDevice(ps, pd);
        pd = (struct PsdDevice *) phw->phw_Devices.lh_Head;
    }
    pd = (struct PsdDevice *) phw->phw_DeadDevices.lh_Head;
    while(pd->pd_Node.ln_Succ) {
        ULONG cnt = 0;
        while(pd->pd_UseCnt && (++cnt < 30)) {
            KPRINTF(20, ("Can't remove device, usecnt %ld\n", pd->pd_UseCnt));
            if(cnt == 5) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Can't remove device '%s', there are still %ld pipes in use...",
                               pd->pd_ProductStr ? pd->pd_ProductStr : (STRPTR) "(no product)",
                               pd->pd_UseCnt);
            }
            psdDelayMS(1000);
        }
        if(pd->pd_UseCnt) {
            /* the class never let go: abandon the device rather than lie to the
               use counter. Configs/descriptors are deliberately leaked -- the
               outstanding pipes still reference the endpoint structures; HC
               state is reclaimed by CloseDevice in the device task. */
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Abandoning device '%s', %ld pipes were never released.",
                           pd->pd_ProductStr ? pd->pd_ProductStr : (STRPTR) "(no product)",
                           pd->pd_UseCnt);
            psdLockWriteDevice(pd);
            pd->pd_Flags &= ~(PDFF_CONNECTED|PDFF_DELEXPUNGE);
            psdUnlockDevice(pd);
            psdLockWritePBase();
            Remove(&pd->pd_Node);
            psdUnlockPBase();
            pDeleteSem(ps, &pd->pd_Lock);
        } else {
            pFreeDevice(ps, pd);
            //psdSendEvent(EHMB_REMDEVICE, pd, NULL);
        }
        pd = (struct PsdDevice *) phw->phw_DeadDevices.lh_Head;
    }

    Forbid();
    /* Note that the subtask unlinks the hardware! */
    phw->phw_ReadySignal = SIGB_SINGLE;
    phw->phw_ReadySigTask = FindTask(NULL);
    if(phw->phw_Task) {
        Signal(phw->phw_Task, SIGBREAKF_CTRL_C);
    }
    Permit();
    while(phw->phw_Task) {
        Wait(1L<<phw->phw_ReadySignal);
    }
    //FreeSignal(phw->phw_ReadySignal);
    KPRINTF(1, ("FreeHardware(0x%08lx) freevec name\n", phw));
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Removed hardware %s/%ld.",
                   phw->phw_DevName, phw->phw_Unit);
    /* before the free; the note still arrives after it (identity only) */
    psdSendEvent(EHMB_REMHARDWARE, phw, NULL);
    psdFreeVec(phw->phw_DevName);
    psdFreeVec(phw->phw_ProductName);
    psdFreeVec(phw->phw_Manufacturer);
    psdFreeVec(phw->phw_Description);
    psdFreeVec(phw->phw_Copyright);
    psdFreeVec(phw);
    KPRINTF(1, ("FreeHardware(0x%08lx) done\n", phw));
}
/* \\\ */

/* /// "psdAddHardware()" */
struct PsdHardware * (psdAddHardware)(STRPTR name asm("a0"), ULONG unit asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdHardware *phw;
    char buf[64];
    struct Task *tmptask;
    KPRINTF(5, ("psdAddHardware(%s, %ld)\n", name, unit));

    if(!name) {
        return(NULL);
    }
    /* Hardware is always known by its bare driver name (<hwmatch.h>); where the
       driver is loaded from is pDeviceTask()'s business. */
    name = (STRPTR) psdHwFilePart(name);

    /* Same guard psdAddClass() has always had. Without it a second add yields a
       second PsdHardware, a second device task and a second root-hub enumeration
       of the same controller -- which is what happens when a Kickstart-resident
       stack has already added the host controller and S:User-Startup's
       "AddUSBHardware xhci.device 0" line adds it again. */
    if(pFindHardware(ps, name, unit)) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Hardware %s/%ld is already installed.", name, unit);
        return(NULL);
    }

    if((phw = psdAllocVec(sizeof(struct PsdHardware)))) {
        NewList(&phw->phw_Devices);
        NewList(&phw->phw_DeadDevices);
        phw->phw_Unit = unit;
        phw->phw_Base = ps;
        phw->phw_NumRootHubs = 1;
        if((phw->phw_Node.ln_Name = phw->phw_DevName = psdCopyStr(name))) {
            psdSafeRawDoFmt(buf, 64, "usbhw<%s/%ld>", name, unit);
            phw->phw_ReadySignal = SIGB_SINGLE;
            phw->phw_ReadySigTask = FindTask(NULL);
            SetSignal(0, SIGF_SINGLE); // clear single bit
            if((tmptask = psdSpawnSubTask(buf, pDeviceTask, phw))) {
                psdBorrowLocksWait(tmptask, 1UL<<phw->phw_ReadySignal);
                if(phw->phw_Task) {
                    phw->phw_ReadySigTask = NULL;
                    //FreeSignal(phw->phw_ReadySignal);
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "New hardware %s/%ld added (%s).",
                                   phw->phw_DevName,
                                   phw->phw_Unit,
                                   phw->phw_ProductName);
                    psdSendEvent(EHMB_ADDHARDWARE, phw, NULL);
                    return(phw);
                }
            }
            phw->phw_ReadySigTask = NULL;
            //FreeSignal(phw->phw_ReadySignal);
            psdFreeVec(phw->phw_DevName);
        }
        psdFreeVec(phw);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdCalculatePower()" */
void (psdCalculatePower)(struct PsdHardware * phw asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *roothub = NULL;
    struct PsdDevice *pd;

    psdLockReadPBase();
    /* process each root device */
    pd = (struct PsdDevice *) phw->phw_Devices.lh_Head;
    while(pd->pd_Node.ln_Succ) {
        if(!pd->pd_Hub) {
            roothub = pd;
            roothub->pd_PowerDrain = 0;
            roothub->pd_PowerSupply = 500;

            /* calculate drain */
            pPowerRecurseDrain(ps, roothub);

            /* calculate supply */
            pPowerRecurseSupply(ps, roothub);
        }
        pd = (struct PsdDevice *) pd->pd_Node.ln_Succ;
    }
    psdUnlockPBase();
}
/* \\\ */

/* *** Pipes *** */

/* /// "psdAllocPipe()" */
struct PsdPipe * (psdAllocPipe)(struct PsdDevice * pd asm("a0"), struct MsgPort * mp asm("a1"), struct PsdEndpoint * pep asm("a2"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe   *pp;

    KPRINTF(2, ("psdAllocPipe(0x%08lx, 0x%08lx, 0x%08lx)\n", pd, mp, pep));
    if(!mp || !pd)
        return(NULL);

    if(pep &&
            (pep->pep_TransType == USEAF_ISOCHRONOUS) &&
            (!(pd->pd_Hardware->phw_Capabilities & UHCF_ISO))) {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR) libname,
                        "Controller driver does not support isochronous transfers.");
        return(NULL);
    }

    if((pp = psdAllocVec(sizeof(struct PsdPipe)))) {
        /* TT info (only used if PDFF_NEEDSSPLIT is set, legacy backend only:
           context HCDs learned the topology at CREATE_DEVICE time) */
        UWORD ttHubAddr  = 0;
        UWORD ttHubPort  = 0;
        UWORD ttThink    = 0;
        BOOL  ttIsMulti  = FALSE;

        if(!pd->pd_Hardware->phw_ContextBackend) {
            pGetTTInfo(pd, &ttHubAddr, &ttHubPort, &ttThink, &ttIsMulti);
        }

        pp->pp_Msg.mn_Node.ln_Type = NT_FREEMSG;
        pp->pp_MsgPort = pp->pp_Msg.mn_ReplyPort = mp;
        pp->pp_Msg.mn_Length = sizeof(struct PsdPipe);
        pp->pp_Device = pd;
        pp->pp_Endpoint = pep;
        pp->pp_StreamID = 0;
        pp->pp_WireReq = (struct IORequest *) &pp->pp_IOReq;

        /* Base template IOReq from HW driver */
        pp->pp_IOReq = *(pd->pd_Hardware->phw_RootIOReq);

        /* Common speed flags */
        if(pd->pd_Flags & PDFF_LOWSPEED)
            pp->pp_IOReq.iouh_Flags |= UHFF_LOWSPEED;

        if(pd->pd_Flags & PDFF_HIGHSPEED) {
            pp->pp_IOReq.iouh_Flags |= UHFF_HIGHSPEED;
            /* MULT for HS interrupt/isoch (transactions per microframe) */
            if(pep) {
                switch(pep->pep_NumTransMuFr) {
                case 2:
                    pp->pp_IOReq.iouh_Flags |= UHFF_MULTI_2;
                    break;
                case 3:
                    pp->pp_IOReq.iouh_Flags |= UHFF_MULTI_3;
                    break;
                default:
                    pp->pp_IOReq.iouh_Flags |= UHFF_MULTI_1;
                    break;
                }
            } else {
                pp->pp_IOReq.iouh_Flags |= UHFF_MULTI_1;
            }
        }

        if(pd->pd_Hardware->phw_ContextBackend) {
            /* Context backend: transfers are direct submits keyed by the
               endpoint token (pDirectSubmit, read per submit from pep/pd);
               the endpoint contexts already hold all topology/companion facts
               (set at CREATE_DEVICE and CONFIGURE_ENDPOINTS time), so no
               per-pipe topology is carried at all. */
            pp->pp_IOReq.iouh_DevAddr = 0;
        } else {
            pp->pp_IOReq.iouh_DevAddr               = pd->pd_DevAddr; /* Device address is per-pipe */

            /* Split transactions / TT info for FS/LS behind HS hubs */
            if(pd->pd_Flags & PDFF_NEEDSSPLIT) {
                /* USB1.1 device connected to a USB2.0 hub */
                pp->pp_IOReq.iouh_Flags        |= UHFF_SPLITTRANS;
                pp->pp_IOReq.iouh_SplitHubAddr  = ttHubAddr;
                pp->pp_IOReq.iouh_SplitHubPort  = ttHubPort;

                if(ttThink)
                    pp->pp_IOReq.iouh_Flags |= (ttThink << UHFS_THINKTIME);

                if(!ttHubAddr) {
                    psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname,
                                    "Internal error obtaining split transaction hub!");
                    psdFreeVec(pp);
                    return(NULL);
                }
            }
        }

        /* Endpoint / transfer type specific setup */
        pp->pp_BusyWeight = 1;
        if(pep) {
            switch(pep->pep_TransType) {
            case USEAF_CONTROL:
                pp->pp_IOReq.iouh_Req.io_Command = UHCMD_CONTROLXFER;
                break;
            case USEAF_ISOCHRONOUS:
                pp->pp_IOReq.iouh_Req.io_Command = UHCMD_ISOXFER;
                break;
            case USEAF_BULK:
                pp->pp_IOReq.iouh_Req.io_Command = UHCMD_BULKXFER;
                break;
            case USEAF_INTERRUPT:
                pp->pp_IOReq.iouh_Req.io_Command = UHCMD_INTXFER;
                /* a pending interrupt listener is not busy IO: pd_IOBusyCount
                   ignores this pipe */
                pp->pp_BusyWeight = 0;
                break;
            default:
                psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                               "AllocPipe(): Illegal transfer type %ld",
                               pep->pep_TransType);
                KPRINTF(20, ("Illegal transfer type for endpoint!"));
                psdFreeVec(pp);
                return(NULL);
            }

            pp->pp_IOReq.iouh_Dir        = (pep->pep_Direction ? UHDIR_IN : UHDIR_OUT);
            pp->pp_IOReq.iouh_Endpoint   = pep->pep_EPNum;
            pp->pp_IOReq.iouh_MaxPktSize = pep->pep_MaxPktSize;
            pp->pp_IOReq.iouh_Interval   = pep->pep_Interval;
        } else {
            /* Default pipe (EP0) */
            pp->pp_IOReq.iouh_Req.io_Command = UHCMD_CONTROLXFER;
            pp->pp_IOReq.iouh_Dir            = UHDIR_SETUP;
            pp->pp_IOReq.iouh_Endpoint       = 0;
            pp->pp_IOReq.iouh_MaxPktSize     = pd->pd_MaxPktSize0;
        }

        Forbid();
        pd->pd_UseCnt++;
        Permit();
        return(pp);
    }

    return(NULL);
}
/* \\\ */

/* /// "psdFreePipe()" */
void (psdFreePipe)(struct PsdPipe * pp asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd;
    if(!pp) {
        return;
    }
    KPRINTF(2, ("psdFreePipe(0x%08lx)\n", pp));
    pd = pp->pp_Device;

    if(pp->pp_Msg.mn_Node.ln_Type == NT_MESSAGE) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Tried to free pipe on %s that was still pending!",
                       pd->pd_ProductStr ? pd->pd_ProductStr : (STRPTR) "(no product)");
        psdAbortPipe(pp);
        psdWaitPipe(pp);
    }

    /* saturating: a late free on an abandoned device must not wrap the counter */
    Forbid();
    if(pd->pd_UseCnt) {
        pd->pd_UseCnt--;
    }
    BOOL collect = (!pd->pd_UseCnt) && (pd->pd_Flags & PDFF_DELEXPUNGE);
    Permit();

    if(collect) {
        KPRINTF(20, ("Finally getting rid of device %s\n", pd->pd_ProductStr));
        pFreeDevice(ps, pd);
        //psdSendEvent(EHMB_REMDEVICE, pd, NULL);
    }

    psdFreeVec(pp);
}
/* \\\ */

/* /// "psdPipeSetup()" */
void (psdPipeSetup)(struct PsdPipe * pp asm("a1"), UWORD rt asm("d0"), UWORD rq asm("d1"), UWORD val asm("d2"), UWORD idx asm("d3"), struct PsdBase * ps asm("a6"))
{
    struct UsbSetupData *usd = &pp->pp_IOReq.iouh_SetupData;

    KPRINTF(1, ("psdSetupPipe(0x%08lx, (%02lx %02lx %04lx %04lx))\n",
                pp, rt, rq, val, idx));
    usd->bmRequestType = rt;
    usd->bRequest = rq;
    usd->wValue = AROS_WORD2LE(val);
    usd->wIndex = AROS_WORD2LE(idx);
}
/* \\\ */

/* /// "pSubmitPipe()" */
/* The realtime-iso control commands (UHCMD_ADD/REMISOHANDLER, UHCMD_START/
 * STOPRTISO from psdAllocRTIsoHandlerA & co) are DEVICE-addressed, so on a
 * context backend they must not leave legacy-shaped: they go out as the
 * clock-driven iso-hook ops (NSCMD_USB_REGISTER/UNREGISTER_HOOKS,
 * START/STOP_STREAM - IOStdReq framing with {handle, endpoint} + a
 * struct USBIsoHooks, usbhcd_context.h).  The hook block lives in the
 * registration (prt_IsoHooks) and is refilled from the classic class-facing
 * IOUsbHWRTIso here at the submit boundary; uih_Object = the classic block,
 * so the HCD calls the class hooks with exactly the object they always got.
 * Same completion demux contract as every context request (pipe in ln_Name).
 */
static struct IORequest * pCtxMarshalIsoHooks(struct PsdPipe *pp)
{
    struct IOUsbHWReq *ior = &pp->pp_IOReq;
    struct IOStdReq *sio = &pp->pp_Ctx.ppc_RtIso.ppcr_Std;
    struct UhcdIsoHooks *op = &pp->pp_Ctx.ppc_RtIso.ppcr_Op;
    /* iouh_Data is the class's IOUsbHWRTIso embedded in the registration */
    struct PsdRTIsoHandler *prt = (struct PsdRTIsoHandler *)
        (((UBYTE *) ior->iouh_Data) - offsetof(struct PsdRTIsoHandler, prt_RTIso));
    struct USBIsoHooks *uih = &prt->prt_IsoHooks;
    UWORD cmd;

    switch(ior->iouh_Req.io_Command) {
    case UHCMD_ADDISOHANDLER:
        cmd = NSCMD_USB_REGISTER_HOOKS;
        break;
    case UHCMD_REMISOHANDLER:
        cmd = NSCMD_USB_UNREGISTER_HOOKS;
        break;
    case UHCMD_STARTRTISO:
        cmd = NSCMD_USB_START_STREAM;
        break;
    default: /* UHCMD_STOPRTISO */
        cmd = NSCMD_USB_STOP_STREAM;
        break;
    }

    /* refresh the wire hook block from the class-facing one (RTA_* attrs may
       have changed between alloc and start); release stays library-owned -
       the device-removal path calls prt_ReleaseHook itself (pFreeDevice) */
    uih->uih_OutRequestHook = prt->prt_RTIso.urti_OutReqHook;
    uih->uih_OutDoneHook = prt->prt_RTIso.urti_OutDoneHook;
    uih->uih_InRequestHook = prt->prt_RTIso.urti_InReqHook;
    uih->uih_InDoneHook = prt->prt_RTIso.urti_InDoneHook;
    uih->uih_ReleaseHook = NULL;
    uih->uih_MaxPrefetch = prt->prt_RTIso.urti_OutPrefetch;
    uih->uih_Flags = prt->prt_HookFlags;
    uih->uih_Pad = 0;
    uih->uih_Object = &prt->prt_RTIso; /* class hooks see their classic object */

    op->uio_DeviceHandle = pp->pp_Device->pd_Handle;
    op->uio_EpAddress = ior->iouh_Endpoint |
                        ((ior->iouh_Dir == UHDIR_IN) ? 0x80 : 0x00);
    op->uio_Pad = 0;
    op->uio_Pad2 = 0;
    op->uio_Hooks = uih;

    sio->io_Message = ior->iouh_Req.io_Message;
    sio->io_Message.mn_Node.ln_Name = (char *) pp; /* completion demux */
    sio->io_Message.mn_Length = sizeof(struct IOStdReq);
    sio->io_Device = ior->iouh_Req.io_Device;
    sio->io_Unit = ior->iouh_Req.io_Unit;
    sio->io_Command = cmd;
    sio->io_Flags = 0;
    sio->io_Error = 0;
    sio->io_Actual = 0;
    sio->io_Data = op;
    sio->io_Length = sizeof(struct UhcdIsoHooks);
    sio->io_Offset = 0;
    return((struct IORequest *) sio);
}

/* /// "Device IO activity accounting" */
/* Every pActivityBegin() on a pipe is balanced by the pActivityEnd() inside
   pCompletePipe() at completion delivery - NOT at collection: classes that reap
   their completions with GetMsg() never call psdWaitPipe(), and an End keyed to
   collection leaked one count per transfer for them (and drifted down on stale
   teardown waits).  PFF_ACTIVITY is the pairing token: Begin sets it, the one
   End that consumes it clears it.  pp_BusyWeight keeps interrupt listeners out
   of the count: a parked IN listener is not the device being used.  Library
   housekeeping (the context lifecycle ops, pCtxDoOp) never calls Begin, so it can
   never End: a SET_SUSPEND or SET_LINK_POWER never counts as activity.
    pd_LastActivity is what the idle sweep reads, and the rollback of a refused 
    uspend must not re-arm the next attempt. */
static inline void pActivityBegin(struct PsdPipe *pp, struct PsdBase *ps)
{
    struct PsdDevice *pd = pp->pp_Device;

    pd->pd_IOBusyCount += pp->pp_BusyWeight;
    pp->pp_Flags |= PFF_ACTIVITY;
    GetSysTime((APTR) &pd->pd_LastActivity);   /* TimerBase resolves through ps */
}

static inline void pActivityEnd(struct PsdPipe *pp, struct PsdBase *ps)
{
    struct PsdDevice *pd = pp->pp_Device;

    pd->pd_IOBusyCount -= pp->pp_BusyWeight;
    GetSysTime((APTR) &pd->pd_LastActivity);
}
/* \\\ */

/* Note a halted bulk/interrupt endpoint at completion delivery: most classes
 * never send the CLEAR_FEATURE(ENDPOINT_HALT) the device needs, so the
 * event handler task clears it on their behalf (pStallRecoverySweep).  The
 * device halted it (STALL), or the host controller did (BABBLE, XACTERROR,
 * SPLITERROR - usbhcd_common.h): then the device's endpoint runs, but its data
 * toggle or sequence number no longer matches the one the HCD just reset, and
 * the clear resyncs it.  A stream pipe is no exception: an error halts the
 * whole pipe and every stream on it, and the halt is removed the same way
 * (USB 3.2 4.4.6.4); the clear also puts the device's stream state machine
 * back to its start.  A context HCD sends nothing on the endpoint until the
 * clear has completed, so it never lands under traffic.  EP0 protocol stalls
 * clear themselves on the next SETUP and iso endpoints have no halt state, so
 * both stay out; so do root hubs (HCD-emulated endpoints).
 * Non-blocking: runs on whatever task delivers the completion. */
static void pMarkStalledPipe(struct PsdPipe *pp)
{
    struct PsdEndpoint *pep = pp->pp_Endpoint;
    struct PsdDevice *pd = pp->pp_Device;
    struct PsdBase *ps = pd->pd_Hardware->phw_Base;
    LONG ioerr = pp->pp_IOReq.iouh_Req.io_Error;
    BOOL halted = (ioerr == UHIOERR_STALL) || (ioerr == UHIOERR_BABBLE) ||
                  (ioerr == UHIOERR_XACTERROR) || (ioerr == UHIOERR_SPLITERROR);

    if(!halted || !pep || pp->pp_AbortPipe) {
        return;
    }
    if((pep->pep_TransType != USEAF_BULK) && (pep->pep_TransType != USEAF_INTERRUPT)) {
        return;
    }
    if(!pd->pd_Hub) {
        return;
    }
    if((pd->pd_Flags & (PDFF_CONNECTED|PDFF_DEAD|PDFF_DELEXPUNGE)) != PDFF_CONNECTED) {
        return;
    }

    ObtainSemaphore(&ps->ps_StallRecoverySem);
    pd->pd_EpHaltMask |= PDEPHALT_MASK(pep->pep_EPNum, pep->pep_Direction);
    ReleaseSemaphore(&ps->ps_StallRecoverySem);

    /* Order matters: mask, then flag (the sweep clears the flag, then reads
       masks).  The wake is latency only (the loop polls the flag anyway), but a class
       resubmitting onto the endpoint it left halted spins until the halt clears
       - too long to wait for the 500 ms tick. */
    ps->ps_StallRecoveryReq = TRUE;
    if(ps->ps_EventHandler.ph_Task) {
        Signal(ps->ps_EventHandler.ph_Task,
               1UL << ps->ps_EventHandler.ph_MsgPort->mp_SigBit);
    }
}

/* THE completion delivery funnel: every pipe finishes here, whatever path it
 * travelled (direct submit via pXferDoneHook, quick I/O, relay demux, a
 * synchronous rejection, psdSendPipe's not-connected fake completion).  Copies
 * a wire-framed result back into pp_IOReq (which psdWaitPipe()/psdCheckPipe(),
 * the DeadCount machinery and the pipe getters read), balances the activity
 * accounting begun at submission, notes a stalled endpoint for the recovery
 * sweep, and replies the message.  Runs on whatever task delivers the
 * completion (HCD unit task, relay task, or the caller): it must never block. */
static void pCompletePipe(struct PsdPipe *pp)
{
    struct IORequest *ioreq = pp->pp_WireReq;

    /* legacy-framed requests complete in place; direct submits carry NULL
       (the done hook writes pp_IOReq itself) */
    if(ioreq && (ioreq != (struct IORequest *) &pp->pp_IOReq)) {
        pp->pp_IOReq.iouh_Req.io_Error = ioreq->io_Error;
    }

    if(pp->pp_Flags & PFF_ACTIVITY) {
        pp->pp_Flags &= ~PFF_ACTIVITY;
        pActivityEnd(pp, pp->pp_Device->pd_Hardware->phw_Base);
    }

    pMarkStalledPipe(pp);

    ReplyMsg(&pp->pp_Msg);
}

/* Demux a wire request replied to phw_DevMsgPort back to its pipe: context
 * requests carry the pipe in their message ln_Name (set at marshal time),
 * legacy ones in the classic stack-owned iouh_UserData. */
static struct PsdPipe * pWireReqPipe(struct IOUsbHWReq *ioreq)
{
    if(UHCD_IS_CTXCMD(ioreq->iouh_Req.io_Command)) {
        return((struct PsdPipe *) ioreq->iouh_Req.io_Message.mn_Node.ln_Name);
    }
    return((struct PsdPipe *) ioreq->iouh_UserData);
}

/* Submit a specific wire request for this pipe; completion is always delivered
 * as pp_Msg on pp_MsgPort (the caller's port), so psdWaitPipe()/psdCheckPipe()
 * and the stream/CBI demux work uniformly.
 *
 * Quick HCD (UHCF_QUICKIO): traditional AmigaOS quick-I/O.  We mark pp_Msg
 *   pending, then BeginIO() with IOF_QUICK *in the caller's context*.
 *   - BeginIO leaves IOF_QUICK set -> completed synchronously, no reply was posted to
 *     phw_DevMsgPort, so WE ReplyMsg(pp_Msg) to the caller's port.  phw_MsgCount stays
 *     untouched (balanced).
 *   - BeginIO clears IOF_QUICK -> driver deferred; it will reply the wire request to
 *     phw_DevMsgPort and the relay task demuxes it (ReplyMsg pp_Msg, --phw_MsgCount),
 *     so we ++phw_MsgCount to match.  The ++ runs in the caller task vs the relay's --
 *     in the device task, so guard it (counter is volatile, RMW not atomic on m68k).
 *
 * Non-quick HCD: unchanged legacy path -- PutMsg to phw_TaskMsgPort;
 *   the relay task forwards via SendIO (++phw_MsgCount there) and demuxes the reply.
 */
static void pSubmitPipeReq(struct PsdPipe *pp, struct IORequest *ioreq, struct PsdBase *ps)
{
    struct PsdHardware *phw = pp->pp_Device->pd_Hardware;

    pp->pp_WireReq = ioreq;
    if(phw->phw_Capabilities & UHCF_QUICKIO) {
        pp->pp_Msg.mn_Node.ln_Type = NT_MESSAGE;
        ioreq->io_Flags |= IOF_QUICK;
        BeginIO(ioreq);
        if(ioreq->io_Flags & IOF_QUICK) {
            pCompletePipe(pp);                          /* synchronous completion */
        } else {
            Forbid();
            phw->phw_MsgCount++;                        /* deferred -> relay will reply */
            Permit();
        }
    } else {
        PutMsg(&phw->phw_TaskMsgPort, &pp->pp_Msg);
    }
}

/* Lower a transfer to the HCD's direct entries (usbhcd_context.h "The
   transfer path").  The submit runs synchronously in this task; completion
   arrives as pp_Msg from the library's done hook, exactly like every other
   path.  The endpoint token is re-read on every submit - the enumeration
   EP0 pipe exists before CREATE_DEVICE delivers pd_Ep0Token, and endpoint
   tokens change with every CONFIGURE_ENDPOINTS/SET_INTERFACE. */
static void pDirectSubmit(struct PsdPipe *pp)
{
    struct PsdHardware *phw = pp->pp_Device->pd_Hardware;
    struct IOUsbHWReq *ior = &pp->pp_IOReq;
    APTR token = pp->pp_Endpoint ? pp->pp_Endpoint->pep_Token
                                 : pp->pp_Device->pd_Ep0Token;
    ULONG naktimeout = (ior->iouh_Flags & UHFF_NAKTIMEOUT) ? ior->iouh_NakTimeout : 0;
    LONG ioerr;

    pp->pp_WireReq = NULL; /* nothing on the wire - abort goes through phw_CtxAbort */
    pp->pp_Msg.mn_Node.ln_Type = NT_MESSAGE; /* pending until the done hook replies */
    ior->iouh_Req.io_Error = 0;
    ior->iouh_Actual = 0;

    if(!token || !phw->phw_Task) {
        /* endpoint not configured / device gone - the stale-token semantics
           the driver applies wire-side */
        ioerr = UHIOERR_TIMEOUT;
    } else if(ior->iouh_Req.io_Command == UHCMD_CONTROLXFER) {
        /* struct UhcdSetupData mirrors the wire setup packet layout; the
           data-phase direction travels in the setup packet */
        ioerr = ((UhcdCtrlSubmitFunc) phw->phw_CtxCtrlSubmit)(phw->phw_CtxHcd,
                    token, (const struct UhcdSetupData *) &ior->iouh_SetupData,
                    ior->iouh_Data, ior->iouh_Length, naktimeout, pp);
    } else {
        /* UHCD_XFF_ bit positions equal their UHFF_ counterparts */
        ioerr = ((UhcdSubmitFunc) phw->phw_CtxSubmit)(phw->phw_CtxHcd, token,
                    ior->iouh_Data, ior->iouh_Length, naktimeout,
                    pp->pp_StreamID,
                    ior->iouh_Flags & (UHFF_ALLOWRUNTPKTS|UHFF_NOSHORTPKT), pp);
    }
    if(ioerr) {
        /* synchronous rejection: complete the pipe here (mirrors quick I/O) */
        ior->iouh_Req.io_Error = (BYTE) ioerr;
        pCompletePipe(pp);
    }
}

static void pSubmitPipe(struct PsdPipe *pp, struct PsdBase *ps)
{
    struct IORequest *ioreq;

    /* any clear-halt reaching the wire - psdClearEndpointHalt() or a
       raw request from an out-of-tree class - drops the library's own pending
       recovery of that endpoint (pStallRecoverySweep).  Without it every stall a
       class handles itself would draw a second clear and a WARN blaming it; a
       late duplicate would also reset the data toggle under resumed traffic. */
    if(!pp->pp_Endpoint) {
        struct UsbSetupData *usd = &pp->pp_IOReq.iouh_SetupData;
        if((usd->bmRequestType == (URTF_STANDARD|URTF_ENDPOINT)) &&
           (usd->bRequest == USR_CLEAR_FEATURE) &&
           (usd->wValue == AROS_WORD2LE(UFS_ENDPOINT_HALT))) {
            UWORD epaddr = AROS_WORD2LE(usd->wIndex);
            ObtainSemaphore(&ps->ps_StallRecoverySem);
            pp->pp_Device->pd_EpHaltMask &= ~PDEPHALT_MASKADDR(epaddr);
            ReleaseSemaphore(&ps->ps_StallRecoverySem);
        }
    }

    if(pp->pp_Device->pd_Hardware->phw_ContextBackend) {
        switch(pp->pp_IOReq.iouh_Req.io_Command) {
        case UHCMD_CONTROLXFER:
        case UHCMD_BULKXFER:
        case UHCMD_INTXFER:
        case UHCMD_ISOXFER:
            pDirectSubmit(pp);
            return;
        case UHCMD_ADDISOHANDLER:
        case UHCMD_REMISOHANDLER:
        case UHCMD_STARTRTISO:
        case UHCMD_STOPRTISO:
            /* device-addressed: context re-key, never legacy-shaped */
            ioreq = pCtxMarshalIsoHooks(pp);
            break;
        default:
            /* Bus-level command: legacy framing BY DESIGN. 
             * the only commands that reach a pipe submit on a
             * context backend besides the transfers and RT-ISO ops routed
             * above are UHCMD_USBRESET (root reset probes in
             * psdEnumerateHardware/pStartDevice) - bus-scoped, never
             * device-addressed.  Any new DEVICE-addressed command must get a
             * context framing here, never legacy passthrough. */
            ioreq = (struct IORequest *) &pp->pp_IOReq;
            break;
        }
    } else {
        ioreq = (struct IORequest *) &pp->pp_IOReq;
    }
    if(ioreq == (struct IORequest *) &pp->pp_IOReq) {
        pp->pp_IOReq.iouh_UserData = pp;                /* legacy completion demux */
    }
    pSubmitPipeReq(pp, ioreq, ps);
}
/* \\\ */

/* /// "psdDoPipe()" */
LONG (psdDoPipe)(struct PsdPipe * pp asm("a1"), APTR data asm("a0"), ULONG len asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd = pp->pp_Device;
    KPRINTF(2, ("psdDoPipe(0x%08lx, 0x%08lx, %ld)\n", pp, data, len));

    if(pd->pd_Flags & PDFF_CONNECTED) {
        if(pd->pd_Flags & PDFF_SUSPENDED) {
            // make sure the device is up and running before trying to send a new pipe
            psdResumeDevice(pd);
        }

        pp->pp_IOReq.iouh_Data = data;
        pp->pp_IOReq.iouh_Length = len;
        if(!pp->pp_Endpoint) {
            pp->pp_IOReq.iouh_SetupData.wLength = AROS_WORD2LE(len);
        }
        /* Begin before the submit: a synchronously completed (or rejected)
           transfer delivers its balancing pActivityEnd() from pCompletePipe() */
        pActivityBegin(pp, ps);
        pSubmitPipe(pp, ps);
        return(psdWaitPipe(pp));
    } else {
        psdDelayMS(50);
        pp->pp_IOReq.iouh_Actual = 0;
        pp->pp_Msg.mn_Node.ln_Type = NT_FREEMSG;
        return(pp->pp_IOReq.iouh_Req.io_Error = UHIOERR_TIMEOUT);
    }
}
/* \\\ */

/* /// "psdSendPipe()" */
void (psdSendPipe)(struct PsdPipe * pp asm("a1"), APTR data asm("a0"), ULONG len asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd = pp->pp_Device;
    KPRINTF(2, ("psdSendPipe(0x%08lx, 0x%08lx, %ld)\n", pp, data, len));
    if(pd->pd_Flags & PDFF_CONNECTED) {
        if(pd->pd_Flags & PDFF_SUSPENDED) {
            // make sure the device is up and running before trying to send a new pipe
            psdResumeDevice(pd);
        }

        pp->pp_IOReq.iouh_Data = data;
        pp->pp_IOReq.iouh_Length = len;
        if(!pp->pp_Endpoint) {
            pp->pp_IOReq.iouh_SetupData.wLength = AROS_WORD2LE(len);
        }
        pActivityBegin(pp, ps);  /* before the submit: see psdDoPipe() */
        pSubmitPipe(pp, ps);
    } else {
        psdDelayMS(50);
        pActivityBegin(pp, ps);
        pp->pp_IOReq.iouh_Actual = 0;
        pp->pp_IOReq.iouh_Req.io_Error = UHIOERR_TIMEOUT;
        pp->pp_WireReq = (struct IORequest *) &pp->pp_IOReq; /* nothing framed: complete in place */
        pCompletePipe(pp);
    }
}
/* \\\ */

/* /// "psdAbortPipe()" */
void (psdAbortPipe)(struct PsdPipe * pp asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *npp;

    KPRINTF(5, ("psdAbortPipe(0x%08lx)\n", pp));
    if(pp->pp_Msg.mn_Node.ln_Type != NT_MESSAGE) {
        KPRINTF(5, ("Nothing to abort %02lx\n", pp->pp_IOReq.iouh_Req.io_Message.mn_Node.ln_Type));
        return;
    }
    if(!pp->pp_WireReq) {
        /* direct submission: no wire request to AbortIO - the HCD's abort
           entry is callable from any task and completes through the done
           hook (an abort is a wish; psdWaitPipe collects the outcome) */
        struct PsdHardware *phw = pp->pp_Device->pd_Hardware;
        APTR token = pp->pp_Endpoint ? pp->pp_Endpoint->pep_Token
                                     : pp->pp_Device->pd_Ep0Token;
        if(token && phw->phw_Task) {
            ((UhcdAbortFunc) phw->phw_CtxAbort)(phw->phw_CtxHcd, token, pp);
        }
        return;
    }
    if((npp = psdAllocVec(sizeof(struct PsdPipe)))) {
        //npp->pp_Msg.mn_Node.ln_Type = NT_MESSAGE;
        npp->pp_Device = pp->pp_Device;
        npp->pp_MsgPort = npp->pp_Msg.mn_ReplyPort = pp->pp_MsgPort;
        npp->pp_Msg.mn_Length = sizeof(struct PsdPipe);

        npp->pp_AbortPipe = pp;
        PutMsg(&pp->pp_Device->pd_Hardware->phw_TaskMsgPort, &npp->pp_Msg);
        psdWaitPipe(npp);
        psdFreeVec(npp);
    }
}
/* \\\ */

/* /// "psdWaitPipe()" */
/* Wait for one pipe's reply, dequeue it and feed the device's dead counter.
   Activity accounting is balanced at completion delivery (pCompletePipe), so a
   stale or repeated wait cannot drift the busy count - and library housekeeping
   (pCtxDoOp), which never calls pActivityBegin(), waits here like anyone else. */
LONG (psdWaitPipe)(struct PsdPipe * pp asm("a1"), struct PsdBase * ps asm("a6"))
{
    ULONG sigs = 0;
    struct PsdDevice *pd = pp->pp_Device;
    LONG ioerr;
    KPRINTF(5, ("psdWaitPipe(0x%08lx)\n", pp));
    while(pp->pp_Msg.mn_Node.ln_Type == NT_MESSAGE) {
        KPRINTF(5, ("ln_Type = %02lx\n", pp->pp_Msg.mn_Node.ln_Type));
        sigs |= Wait(1L<<pp->pp_MsgPort->mp_SigBit);
        KPRINTF(5, ("sigs = 0x%08lx\n", sigs));
    }
    Forbid();
    if(pp->pp_Msg.mn_Node.ln_Type == NT_REPLYMSG) {
        /* NT_REPLYMSG does not mean "still queued here": ReplyMsg() sets it and
           GetMsg() is documented only to unlink, so a pipe a class
           reaped itself (bootkeyboard, lan78xx, camdusbmidi do) looks identical.
           A blind Remove() here would relink stale neighbours. */
        struct Node *n = pp->pp_MsgPort->mp_MsgList.lh_Head;
        while(n->ln_Succ) {
            if(n == &pp->pp_Msg.mn_Node) {
                Remove(n);
                break;
            }
            n = n->ln_Succ;
        }
        pp->pp_Msg.mn_Node.ln_Type = NT_FREEMSG;
    }
    /* avoid signals getting lost for other messages arriving */
    SetSignal(sigs, sigs);
    Permit();
    ioerr = pp->pp_IOReq.iouh_Req.io_Error;
    switch(ioerr) {
    case UHIOERR_TIMEOUT:
        pd->pd_DeadCount++;
    // fall through
    case UHIOERR_NAKTIMEOUT:
        pd->pd_DeadCount++;
    // fall through
    case UHIOERR_CRCERROR:
    case UHIOERR_XACTERROR:     /* context ABI: a failed transaction, like CRC */
    case UHIOERR_SPLITERROR:
        pd->pd_DeadCount++;
        break;
    case UHIOERR_RUNTPACKET:
    default:
        if(pd->pd_DeadCount) {
            pd->pd_DeadCount >>= 1;
            /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Device %s starts recovering with %s (%ld)!",
                           pd->pd_ProductStr,
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);*/
        }
    }
    KPRINTF(2, ("psdWaitPipe(0x%08lx)=%ld\n", pp, ioerr));

    if((pd->pd_DeadCount > 19) || ((pd->pd_DeadCount > 14) && (pd->pd_Flags & (PDFF_HASDEVADDR|PDFF_HASDEVDESC)))) {
        if(!(pd->pd_Flags & PDFF_DEAD)) {
            pd->pd_Flags |= PDFF_DEAD;
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Device %s stopped responding.", pd->pd_ProductStr);

            psdSendEvent(EHMB_DEVICEDEAD, pp->pp_Device, NULL);
        }
    } else {
        if((!pd->pd_DeadCount) && ((pd->pd_Flags & (PDFF_DEAD|PDFF_CONNECTED)) == (PDFF_DEAD|PDFF_CONNECTED))) {
            pd->pd_Flags &= ~PDFF_DEAD;
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Device %s is responding again.", pd->pd_ProductStr);
        }
    }
    return(ioerr);
}
/* \\\ */

/* /// "psdCheckPipe()" */
struct PsdPipe * (psdCheckPipe)(struct PsdPipe * pp asm("a1"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(5, ("psdCheckPipe(0x%08lx)\n", pp));
    if(pp->pp_Msg.mn_Node.ln_Type == NT_MESSAGE) {
        return(NULL);
    }
    return(pp);
}
/* \\\ */

/* /// "psdGetPipeActual()" */
ULONG (psdGetPipeActual)(struct PsdPipe * pp asm("a1"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(1, ("psdGetPipeActual(0x%08lx)\n", pp));
    return(pp->pp_IOReq.iouh_Actual);
}
/* \\\ */

/* /// "psdGetPipeError()" */
LONG (psdGetPipeError)(struct PsdPipe * pp asm("a1"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(1, ("psdGetPipeError(0x%08lx)\n", pp));
    return((LONG) pp->pp_IOReq.iouh_Req.io_Error);
}
/* \\\ */

/* /// "psdClearEndpointHalt()" */
LONG (psdClearEndpointHalt)(struct PsdPipe * pp asm("a1"), ULONG epaddr asm("d0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdClearEndpointHalt(0x%08lx, 0x%02lx)\n", pp, epaddr));
    if(pp->pp_Endpoint) {
        KPRINTF(20, ("psdClearEndpointHalt: not a default pipe!\n"));
        return(UHIOERR_BADPARAMS);
    }

    /* The pending recovery for this endpoint is cancelled by the pSubmitPipe
       snoop, on the very request below. */
    psdPipeSetup(pp, URTF_STANDARD|URTF_ENDPOINT,
                 USR_CLEAR_FEATURE, UFS_ENDPOINT_HALT, epaddr);
    return(psdDoPipe(pp, NULL, 0));
}
/* \\\ */

/* *** Streams *** */

/* /// "psdOpenStreamA()" */
struct PsdPipeStream * (psdOpenStreamA)(struct PsdEndpoint * pep asm("a0"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipeStream *pps;

    KPRINTF(2, ("psdOpenStream(0x%08lx, 0x%08lx)\n", pep, tags));
    if(!pep) {
        return(NULL);
    }
    if((pps = psdAllocVec(sizeof(struct PsdPipeStream)))) {
        pps->pps_Device = pep->pep_Interface->pif_Config->pc_Device;
        pps->pps_Endpoint = pep;
        NewList(&pps->pps_FreePipes);
        NewList(&pps->pps_ReadyPipes);
        InitSemaphore(&pps->pps_AccessLock);
        pps->pps_NakTimeoutTime = 5000;
        if(pep->pep_Direction) {
            /* Defaults for IN */
            pps->pps_NumPipes = 4;
            pps->pps_Flags = PSFF_READAHEAD|PSFF_BUFFERREAD|PSFF_ALLOWRUNT;
            pps->pps_BufferSize = 32*pps->pps_Endpoint->pep_MaxPktSize;
        } else {
            /* Defaults for OUT */
            pps->pps_NumPipes = 4;
            pps->pps_Flags = PSFF_NOSHORTPKT;
            pps->pps_BufferSize = 4*pps->pps_Endpoint->pep_MaxPktSize;
        }

        psdSetAttrsA(PGA_PIPESTREAM, pps, tags);
        if(!pps->pps_Pipes) {
            psdCloseStream(pps);
            pps = NULL;
        }
        return(pps);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdCloseStream()" */
void (psdCloseStream)(struct PsdPipeStream * pps asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *pp;
    ULONG cnt;

    KPRINTF(2, ("psdCloseStream(0x%08lx)\n", pps));
    if(!pps) {
        return;
    }
    psdStreamFlush(pps);
    ObtainSemaphore(&pps->pps_AccessLock);
    if(pps->pps_Pipes) {
        for(cnt = 0; cnt < pps->pps_NumPipes; cnt++) {
            pp = pps->pps_Pipes[cnt];
            //if(pp->pp_IOReq.iouh_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE)
            {
                KPRINTF(1, ("Abort %ld\n", cnt));
                psdAbortPipe(pp);
                KPRINTF(1, ("Wait %ld\n", cnt));
                psdWaitPipe(pp);
            }
            KPRINTF(1, ("Free %ld\n", cnt));
            psdFreePipe(pp);
        }
        psdFreeVec(pps->pps_Pipes);
        if((pps->pps_Flags & PSFF_OWNMSGPORT) && pps->pps_MsgPort) {
            DeleteMsgPort(pps->pps_MsgPort);
        }
    }
    if(pps->pps_Endpoint && pps->pps_Endpoint->pep_StreamsAlloc) {
        /* the pipe stream was the endpoint's stream user: release the HCD's
           stream rings (idempotent; no-op after unplug) */
        pCtxFreeStreams(ps, pps->pps_Endpoint);
    }
    psdFreeVec(pps->pps_Buffer);
    ReleaseSemaphore(&pps->pps_AccessLock);
    psdFreeVec(pps);
}
/* \\\ */

/* /// "psdStreamRead()" */
LONG (psdStreamRead)(struct PsdPipeStream * pps asm("a1"), UBYTE * buffer asm("a0"), LONG length asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *pp;
    ULONG cnt;
    LONG ioerr;
    LONG remlen;
    BOOL term = FALSE;
    LONG actual = 0;
    ULONG sigmask;

    UBYTE *bufptr;
    UBYTE *srcptr;
    UBYTE *tarrptr;
    ULONG tcnt;
    UBYTE cchar;

    KPRINTF(2, ("psdStreamRead(0x%08lx, 0x%08lx, %ld)\n", pps, buffer, length));
    if(!pps) {
        return(-1);
    }
    ObtainSemaphore(&pps->pps_AccessLock);
    KPRINTF(2, ("Sema\n"));
    pps->pps_Error = 0;
    if((!pps->pps_Pipes) || (!pps->pps_Endpoint->pep_Direction)) {
        KPRINTF(2, ("Wrong direction!\n"));
        pps->pps_Error = UHIOERR_BADPARAMS;
        ReleaseSemaphore(&pps->pps_AccessLock);
        return(-1);
    }
    if(!(pps->pps_Flags & PSFF_ASYNCIO)) {
        if(pps->pps_Flags & PSFF_BUFFERREAD) {
            /* buffered reading */
            do {
                /* check for incoming packets */
                while((pp = (struct PsdPipe *) GetMsg(pps->pps_MsgPort))) {
                    KPRINTF(1, ("PktBack(0x%08lx, 0x%08lx, %ld/%ld)=%ld\n",
                                pp, pp->pp_IOReq.iouh_Data, pp->pp_IOReq.iouh_Actual,
                                pp->pp_IOReq.iouh_Length, pp->pp_IOReq.iouh_Req.io_Error));

                    pps->pps_ReqBytes -= pp->pp_IOReq.iouh_Length;
                    ioerr = pp->pp_IOReq.iouh_Req.io_Error;
                    if((ioerr == UHIOERR_NAKTIMEOUT) && pp->pp_IOReq.iouh_Actual) {
                        ioerr = 0;
                    }

                    if(ioerr) {
                        pps->pps_Error = ioerr;
                        term = TRUE;
                        if(ioerr != UHIOERR_TIMEOUT) {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) "StreamRead",
                                           "Packet(%s) failed: %s (%ld)", (STRPTR) "b",
                                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                        }
                        /* stop automatic queueing */
                        pps->pps_Flags &= ~PSFF_READAHEAD;
                        AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);
                    } else {
                        /* successfully received packet */
                        pps->pps_BytesPending += pp->pp_IOReq.iouh_Actual;
                        AddTail(&pps->pps_ReadyPipes, &pp->pp_Msg.mn_Node);
                    }
                }
                if(length == -1) { /* get all that's there (STRONGLY DISCOURAGED! Might cause buffer overflows) */
                    length = pps->pps_BytesPending;
                }
                /* check for buffered data */
                while(length && pps->pps_BytesPending) {
                    pp = (struct PsdPipe *) pps->pps_ReadyPipes.lh_Head;
                    if(!pp->pp_Msg.mn_Node.ln_Succ) { /* debug */
                        psdAddErrorMsg0(RETURN_FAIL, (STRPTR) "StreamRead", "Readqueue empty!");
                        ReleaseSemaphore(&pps->pps_AccessLock);
                        return(-1);
                    }
                    if(pp->pp_IOReq.iouh_Actual < pps->pps_Offset) {
                        psdAddErrorMsg(RETURN_FAIL, (STRPTR) "StreamRead",
                                       "Actual %ld < offset %ld!", pp->pp_IOReq.iouh_Actual, pps->pps_Offset);
                        ReleaseSemaphore(&pps->pps_AccessLock);
                        return(-1);
                    }
                    remlen = pp->pp_IOReq.iouh_Actual - pps->pps_Offset;
                    if(length < remlen) {
                        KPRINTF(1, ("PktBit(0x%08lx, 0x%08lx, %ld)\n", pp, buffer, length));
                        remlen = length;
                    } else {
                        KPRINTF(1, ("PktRem(0x%08lx, 0x%08lx, %ld)\n", pp, buffer, remlen));
                    }
                    /* copy packet */
                    if(pp->pp_Flags & PFF_INPLACE) {
                        KPRINTF(1, ("PktRemIP(0x%08lx, 0x%08lx, %ld)\n", pp, buffer, remlen));
                    } else {
                        if(pps->pps_TermArray) {
                            /* EOF Mode */
                            KPRINTF(1, ("PktCpyEOF(0x%08lx, 0x%08lx, %ld)\n", pp, buffer, remlen));
                            bufptr = buffer;
                            srcptr = &(((UBYTE *) pp->pp_IOReq.iouh_Data)[pps->pps_Offset]);
                            tarrptr = pps->pps_TermArray;
                            cnt = remlen;
                            remlen = 0;
                            if(cnt) {
                                do {
                                    cchar = *bufptr++ = *srcptr++;
                                    remlen++;
                                    tcnt = 0;
                                    do {
                                        if(cchar < tarrptr[tcnt]) {
                                            break;
                                        } else if(cchar == tarrptr[tcnt]) {
                                            cnt = 1;
                                            term = TRUE;
                                            KPRINTF(2, ("EOF char %02lx found, length = %ld\n", cchar, remlen));
                                            break;
                                        }
                                        if(tcnt < 7) {
                                            if(tarrptr[tcnt] == tarrptr[tcnt+1]) {
                                                break;
                                            }
                                        }
                                    } while(++tcnt < 8);
                                } while(--cnt);
                            }
                        } else {
                            KPRINTF(1, ("PktCpy(0x%08lx, 0x%08lx, %ld)\n", pp, buffer, remlen));
                            /* quick non-eof mode */
                            CopyMem(&(((UBYTE *) pp->pp_IOReq.iouh_Data)[pps->pps_Offset]), buffer, remlen);
                        }
                    }
                    actual += remlen;
                    length -= remlen;
                    buffer += remlen;
                    pps->pps_BytesPending -= remlen;
                    pps->pps_Offset += remlen;
                    /* end of packet reached? */
                    if(pps->pps_Offset == pp->pp_IOReq.iouh_Actual) {
                        pps->pps_Offset = 0;
                        Remove(&pp->pp_Msg.mn_Node);
                        AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);
                        /* check for short packet */
                        if((pps->pps_Flags & PSFF_SHORTTERM) && (pp->pp_IOReq.iouh_Actual % pp->pp_IOReq.iouh_MaxPktSize)) {
                            term = TRUE;
                        }
                    }
                    if(term) {
                        break;
                    }
                }
                /* start sending out requests */
                remlen = length;
                pp = (struct PsdPipe *) pps->pps_FreePipes.lh_Head;
                if(!(pps->pps_BytesPending || pps->pps_ReqBytes || pps->pps_TermArray || (length < pps->pps_BufferSize))) {
                    /* faster non-buffered mode */
                    if(pp->pp_Msg.mn_Node.ln_Succ) {
                        pp->pp_Flags |= PFF_INPLACE;
                        Remove(&pp->pp_Msg.mn_Node);
                        remlen = length - (length % pp->pp_IOReq.iouh_MaxPktSize);
                        KPRINTF(1, ("OutFast(0x%08lx, 0x%08lx, %ld/%ld)\n",
                                    pp, buffer, remlen, length));
                        psdSendPipe(pp, buffer, remlen);
                        pps->pps_ReqBytes += remlen;
                        pp = (struct PsdPipe *) pps->pps_FreePipes.lh_Head;
                    }
                }
                /* slower buffered mode */
                while(pp->pp_Msg.mn_Node.ln_Succ && ((remlen > pps->pps_ReqBytes) || (pps->pps_Flags & PSFF_READAHEAD))) {
                    pp->pp_Flags &= ~PFF_INPLACE;
                    Remove(&pp->pp_Msg.mn_Node);
                    if((pps->pps_Flags & PSFF_READAHEAD) || (remlen % pp->pp_IOReq.iouh_MaxPktSize)) {
                        KPRINTF(1, ("OutSlow(0x%08lx, 0x%08lx, %ld)\n",
                                    pp, &pps->pps_Buffer[pp->pp_Num * pps->pps_BufferSize], pps->pps_BufferSize));
                        remlen = pps->pps_BufferSize;
                    } else {
                        KPRINTF(1, ("OutExact(0x%08lx, 0x%08lx, %ld)\n",
                                    pp, &pps->pps_Buffer[pp->pp_Num * pps->pps_BufferSize], remlen));
                    }
                    psdSendPipe(pp, &pps->pps_Buffer[pp->pp_Num * pps->pps_BufferSize], remlen);
                    pps->pps_ReqBytes += remlen;
                    pp = (struct PsdPipe *) pps->pps_FreePipes.lh_Head;
                }
                if((!length) || (pps->pps_Flags & PSFF_DONOTWAIT)) {
                    term = TRUE;
                }
                if(!term) {
                    sigmask = (1UL<<pps->pps_MsgPort->mp_SigBit)|pps->pps_AbortSigMask;
                    KPRINTF(1, ("WaitPort (0x%08lx)\n", sigmask));
                    sigmask = Wait(sigmask);
                    KPRINTF(1, ("Wait back (0x%08lx)\n", sigmask));
                    if(sigmask & pps->pps_AbortSigMask) {
                        KPRINTF(1, ("Aborted!\n"));
                        term = TRUE;
                        Signal(FindTask(NULL), pps->pps_AbortSigMask & sigmask);
                    }
                }
            } while(!term);
        } else {
            /* plain reading (might lose data) */
            if(pps->pps_TermArray || (pps->pps_Flags & PSFF_READAHEAD)) {
                psdAddErrorMsg0(RETURN_WARN, (STRPTR) "StreamRead", "This mode combination for the stream is not supported!");
            }
            /* start sending out requests */
            pp = (struct PsdPipe *) pps->pps_FreePipes.lh_Head;
            if(pp->pp_Msg.mn_Node.ln_Succ && length) {
                ioerr = psdDoPipe(pp, buffer, length);
                if(ioerr) {
                    pps->pps_Error = ioerr;
                    if(ioerr != UHIOERR_TIMEOUT) {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) "StreamRead",
                                       "Packet(%s) failed: %s (%ld)", (STRPTR) "u",
                                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    }
                }
                actual = pp->pp_IOReq.iouh_Actual;
            }
        }
    }
    ReleaseSemaphore(&pps->pps_AccessLock);
    return(actual);
}
/* \\\ */

/* /// "psdStreamWrite()" */
LONG (psdStreamWrite)(struct PsdPipeStream * pps asm("a1"), UBYTE * buffer asm("a0"), LONG length asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *pp;
    struct PsdPipe *newpp;
    ULONG cnt;
    LONG ioerr;
    LONG remlen;
    LONG actual = 0;
    ULONG sigmask;

    UBYTE *bufptr;
    UBYTE *srcptr;
    UBYTE *tarrptr;
    ULONG tcnt;
    UBYTE cchar;

    KPRINTF(2, ("psdStreamWrite(0x%08lx, 0x%08lx, %ld)\n", pps, buffer, length));
    if(!pps) {
        return(-1);
    }
    ObtainSemaphore(&pps->pps_AccessLock);
    pps->pps_Error = 0;
    if((!pps->pps_Pipes) || pps->pps_Endpoint->pep_Direction) {
        KPRINTF(2, ("Wrong direction!\n"));
        pps->pps_Error = UHIOERR_BADPARAMS;
        ReleaseSemaphore(&pps->pps_AccessLock);
        return(-1);
    }
    if(length == -1) { /* null terminated string mode */
        KPRINTF(2, ("EOL mode!\n"));
        length = strlen(buffer);
    }
    if((tarrptr = pps->pps_TermArray)) { /* EOF Mode */
        KPRINTF(1, ("EOFSearch(0x%08lx, %ld)\n", buffer, length));
        srcptr = buffer;
        cnt = length;
        length = 0;
        if(cnt) {
            do {
                cchar = *srcptr++;
                length++;
                tcnt = 0;
                do {
                    if(cchar < tarrptr[tcnt]) {
                        break;
                    } else if(cchar == tarrptr[tcnt]) {
                        cnt = 1;
                        KPRINTF(2, ("EOF char %02lx found, length = %ld\n", cchar, length));
                        break;
                    }
                    if(tcnt) {
                        if(tarrptr[tcnt] == tarrptr[tcnt+1]) {
                            break;
                        }
                    }
                } while(++tcnt < 8);
            } while(--cnt);
        }
    }
    if(!(pps->pps_Flags & PSFF_ASYNCIO)) {
        pp = (struct PsdPipe *) pps->pps_FreePipes.lh_Head;
        if(pp->pp_Msg.mn_Node.ln_Succ && length) {
            if(pps->pps_Flags & PSFF_BUFFERWRITE) {
                /* buffered writing */
                if(pps->pps_BytesPending) {
                    remlen = pps->pps_BytesPending % pp->pp_IOReq.iouh_MaxPktSize;
                    /* align to packet boundary */
                    if(remlen + length >= pp->pp_IOReq.iouh_MaxPktSize) {
                        /* new data crosses at least on packet size */
                        if(pps->pps_BytesPending + length <= pps->pps_BufferSize) {
                            /* copy everything up to the last (!) boundary */
                            remlen = pps->pps_BytesPending + length;
                            remlen = remlen - (remlen % pp->pp_IOReq.iouh_MaxPktSize);
                            remlen -= pps->pps_BytesPending;
                            KPRINTF(1, ("PendOptCpy(0x%08lx, %ld+%ld/%ld)\n", buffer, pps->pps_BytesPending, remlen, length));
                        } else {
                            /* just calculate amount to copy to the next boundary */
                            remlen = pp->pp_IOReq.iouh_MaxPktSize - remlen;
                            KPRINTF(1, ("PendOneCpy(0x%08lx, %ld+%ld/%ld)\n", buffer, pps->pps_BytesPending, remlen, length));
                        }
                        CopyMem(buffer, &pps->pps_Buffer[pps->pps_BytesPending], remlen);
                        pps->pps_BytesPending += remlen;
                        actual += remlen;
                        buffer += remlen;
                        length -= remlen;
                    } else {
                        KPRINTF(1, ("PendAdd(0x%08lx, %ld+%ld)\n", buffer, pps->pps_BytesPending, length));
                        /* only a few bytes, see if we can fit them */
                        CopyMem(buffer, &pps->pps_Buffer[pps->pps_BytesPending], length);
                        pps->pps_BytesPending += length;
                        actual += length;
                        //buffer += length; /* not needed */
                        length = 0;
                    }
                    /* flush some buffers */
                    if((length >= pp->pp_IOReq.iouh_MaxPktSize) ||
                            ((pps->pps_BytesPending >= (pps->pps_BufferSize>>1)) && (pps->pps_BytesPending >= pp->pp_IOReq.iouh_MaxPktSize))) {
                        remlen = pps->pps_BytesPending - (pps->pps_BytesPending % pp->pp_IOReq.iouh_MaxPktSize);
                        KPRINTF(1, ("PendFlush(%ld/%ld)\n", remlen, pps->pps_BytesPending));
                        Remove(&pp->pp_Msg.mn_Node);
                        psdSendPipe(pp, pps->pps_Buffer, remlen);
                        pps->pps_ActivePipe = pp;
                        while(!(newpp = (struct PsdPipe *) GetMsg(pps->pps_MsgPort))) {
                            sigmask = (1UL<<pps->pps_MsgPort->mp_SigBit)|pps->pps_AbortSigMask;
                            sigmask = Wait(sigmask);
                            if(sigmask & pps->pps_AbortSigMask) {
                                KPRINTF(1, ("Kill signal detected!\n"));
                                Signal(FindTask(NULL), pps->pps_AbortSigMask & sigmask);
                                break;
                            }
                        }
                        if(!newpp) {
                            psdAbortPipe(pp);
                        }
                        ioerr = psdWaitPipe(pp);
                        pps->pps_ActivePipe = NULL;
                        AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);

                        /* move end of buffer */
                        cnt = pps->pps_BytesPending;
                        tcnt = pp->pp_IOReq.iouh_Actual;
                        pps->pps_BytesPending -= tcnt;
                        bufptr = pps->pps_Buffer;
                        srcptr = bufptr + tcnt;
                        cnt -= tcnt;
                        if(cnt) {
                            do {
                                *bufptr++ = *srcptr++;
                            } while(--cnt);
                        }
                        if(ioerr) {
                            pps->pps_Error = ioerr;
                            if(ioerr != UHIOERR_TIMEOUT) {
                                psdAddErrorMsg(RETURN_WARN, (STRPTR) "StreamWrite",
                                               "Packet(%s) failed: %s (%ld)", (STRPTR) "b",
                                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                            }
                            ReleaseSemaphore(&pps->pps_AccessLock);
                            return(actual);
                        }
                    }
                }
                /* send out large chunk (avoid copying) */
                if(length >= pp->pp_IOReq.iouh_MaxPktSize) {
                    remlen = length - (length % pp->pp_IOReq.iouh_MaxPktSize);
                    KPRINTF(1, ("BulkFlush(0x%08lx, %ld/%ld)\n", buffer, remlen, length));
                    Remove(&pp->pp_Msg.mn_Node);
                    psdSendPipe(pp, buffer, remlen);
                    pps->pps_ActivePipe = pp;
                    while(!(newpp = (struct PsdPipe *) GetMsg(pps->pps_MsgPort))) {
                        sigmask = (1UL<<pps->pps_MsgPort->mp_SigBit)|pps->pps_AbortSigMask;
                        sigmask = Wait(sigmask);
                        if(sigmask & pps->pps_AbortSigMask) {
                            KPRINTF(1, ("Kill signal detected!\n"));
                            Signal(FindTask(NULL), pps->pps_AbortSigMask & sigmask);
                            break;
                        }
                    }
                    if(!newpp) {
                        psdAbortPipe(pp);
                    }
                    ioerr = psdWaitPipe(pp);
                    pps->pps_ActivePipe = NULL;
                    AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);

                    actual += pp->pp_IOReq.iouh_Actual;
                    buffer += pp->pp_IOReq.iouh_Actual;
                    length -= pp->pp_IOReq.iouh_Actual;
                    if(ioerr) {
                        pps->pps_Error = ioerr;
                        if(ioerr != UHIOERR_TIMEOUT) {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) "StreamWrite",
                                           "Packet(%s) failed: %s (%ld)", (STRPTR) "c",
                                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                        }
                        ReleaseSemaphore(&pps->pps_AccessLock);
                        return(actual);
                    }
                }
                /* buffer remaining bytes */
                if(length) {
                    KPRINTF(1, ("BufAdd(0x%08lx, %ld)\n", buffer, length));
                    /* only a few bytes left, so lets buffer them */
                    CopyMem(buffer, &pps->pps_Buffer[pps->pps_BytesPending], length);
                    pps->pps_BytesPending += length;
                    actual += length;
                }
            } else {
                /* plain writing */
                /* start sending out requests */
                KPRINTF(1, ("PlainWrite(0x%08lx, %ld)\n", buffer, length));
                Remove(&pp->pp_Msg.mn_Node);
                psdSendPipe(pp, buffer, length);
                pps->pps_ActivePipe = pp;
                while(!(newpp = (struct PsdPipe *) GetMsg(pps->pps_MsgPort))) {
                    sigmask = (1UL<<pps->pps_MsgPort->mp_SigBit)|pps->pps_AbortSigMask;
                    sigmask = Wait(sigmask);
                    if(sigmask & pps->pps_AbortSigMask) {
                        KPRINTF(1, ("Kill signal detected!\n"));
                        Signal(FindTask(NULL), pps->pps_AbortSigMask & sigmask);
                        break;
                    }
                }
                if(!newpp) {
                    psdAbortPipe(pp);
                }
                ioerr = psdWaitPipe(pp);
                pps->pps_ActivePipe = NULL;
                AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);
                if(ioerr) {
                    pps->pps_Error = ioerr;
                    if(ioerr != UHIOERR_TIMEOUT) {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) "StreamWrite",
                                       "Packet(%s) failed: %s (%ld)", (STRPTR) "u",
                                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    }
                }
                actual = pp->pp_IOReq.iouh_Actual;
            }
        } else {
            KPRINTF(2, ("No free pipe!\n"));
        }
    }
    ReleaseSemaphore(&pps->pps_AccessLock);
    return(actual);
}
/* \\\ */

/* /// "psdStreamFlush()" */
LONG (psdStreamFlush)(struct PsdPipeStream * pps asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *pp;
    ULONG cnt;
    LONG ioerr;
    LONG ret = FALSE;

    KPRINTF(2, ("psdStreamFlush(0x%08lx)\n", pps));
    if(!pps) {
        return(-1);
    }
    ObtainSemaphore(&pps->pps_AccessLock);
    pps->pps_Error = 0;
    if(pps->pps_Endpoint->pep_Direction) {
        /* IN */
        KPRINTF(2, ("Flushing in...\n"));
        for(cnt = 0; cnt < pps->pps_NumPipes; cnt++) {
            psdAbortPipe(pps->pps_Pipes[cnt]);
        }
        for(cnt = 0; cnt < pps->pps_NumPipes; cnt++) {
            psdWaitPipe(pps->pps_Pipes[cnt]);
        }
        pp = (struct PsdPipe *) pps->pps_ReadyPipes.lh_Head;
        while(pp->pp_Msg.mn_Node.ln_Succ) {
            Remove(&pp->pp_Msg.mn_Node);
            AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);
            pp = (struct PsdPipe *) pps->pps_ReadyPipes.lh_Head;
        }
        while((pp = (struct PsdPipe *) GetMsg(pps->pps_MsgPort))) {
            AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);
        }
        pps->pps_ReqBytes = 0;
        pps->pps_BytesPending = 0;
        pps->pps_Offset = 0;
        ret = TRUE;
    } else {
        /* OUT */
        pp = (struct PsdPipe *) pps->pps_FreePipes.lh_Head;
        if(pp->pp_Msg.mn_Node.ln_Succ) {
            ret = TRUE;
            if(pps->pps_BytesPending) {
                KPRINTF(2, ("Flushing out %ld...\n", pps->pps_BytesPending));
                Remove(&pp->pp_Msg.mn_Node);
                ioerr = psdDoPipe(pp, pps->pps_Buffer, pps->pps_BytesPending);
                AddTail(&pps->pps_FreePipes, &pp->pp_Msg.mn_Node);
                pps->pps_BytesPending = 0;
                if(ioerr) {
                    pps->pps_Error = ioerr;
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) "StreamFlush",
                                   "Packet(%s) failed: %s (%ld)", (STRPTR) "f",
                                   psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    ret = FALSE;
                }
            } else {
                KPRINTF(2, ("Nothing to flush\n"));
            }
        }
    }
    ReleaseSemaphore(&pps->pps_AccessLock);
    return(ret);
}
/* \\\ */

/* /// "psdGetStreamError()" */
LONG (psdGetStreamError)(struct PsdPipeStream * pps asm("a1"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(1, ("psdGetStreamError(0x%08lx)\n", pps));
    if(pps) {
        return((LONG) pps->pps_Error);
    } else {
        return(-1);
    }
}
/* \\\ */

/* *** Realtime Iso */

/* /// "pRtIsoForwardCmd()" */
/* Issue an RT-ISO control command (ADD/REM/START/STOP-ISOHANDLER) and wait for it,
 * submitting via pSubmitPipe() (quick BeginIO+IOF_QUICK on a quick HCD, else the relay).
 *
 * The original AROS code issued a direct DoIO() on pp_IOReq, which only works on HCDs
 * that complete *synchronously* inside BeginIO().  A deferring driver (e.g. emu68
 * xhci.device) instead replies pp_IOReq on phw_DevMsgPort, where the relay dereferences
 * iouh_UserData -- which the direct-DoIO path never set -> crash.  pSubmitPipe() always
 * delivers completion as pp_Msg on a port we own, so it works for both kinds of HCD.
 *
 * A fresh reply port is created in the *calling* task's context per command: psdWaitPipe()
 * Wait()s on the current task while ReplyMsg() signals the port's owner, and the four entry
 * points run in different tasks (Alloc/Start from the AHI task; Stop also fires from the
 * device-removal hub task via the RT-ISO release hook on unplug).  A running stream counts
 * as busy IO, so the command takes the normal pActivityBegin()/delivery-End pair, matching
 * the original DoIO-era accounting; the stamps are wanted too: a STOP marks the end of the
 * stream's activity, so the device gets its full idle timeout before the sweep looks at it. */
static LONG pRtIsoForwardCmd(struct PsdPipe *pp, struct PsdBase *ps)
{
    struct MsgPort *port = CreateMsgPort();
    LONG ioerr;
    if(!port) {
        return(UHIOERR_OUTOFMEMORY);
    }
    pp->pp_MsgPort = pp->pp_Msg.mn_ReplyPort = port;
    pActivityBegin(pp, ps);            /* balanced by pCompletePipe() at completion delivery */
    pSubmitPipe(pp, ps);               /* quick: BeginIO+IOF_QUICK; else relay PutMsg */
    ioerr = psdWaitPipe(pp);
    pp->pp_MsgPort = pp->pp_Msg.mn_ReplyPort = NULL;
    DeleteMsgPort(port);
    return(ioerr);
}
/* \\\ */

/* /// "psdAllocRTIsoHandler()" */
struct PsdRTIsoHandler * (psdAllocRTIsoHandlerA)(struct PsdEndpoint * pep asm("a0"), struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdRTIsoHandler *prt;
    struct PsdPipe *pp;
    LONG ioerr;

    KPRINTF(2, ("psdAllocRTIso(0x%08lx, 0x%08lx)\n", pep, tags));
    if(!pep) {
        return(NULL);
    }
    if(pep->pep_TransType != USEAF_ISOCHRONOUS) {
        return(NULL);
    }

    /* ctx backends carry RT-ISO via the clock-driven iso-hook ops
       (NSCMD_USB_REGISTER_HOOKS et al), so the coarse UHCF_RT_ISO
       bit must be backed by the op in the driver's NSD list */
    struct PsdHardware *phw = pep->pep_Interface->pif_Config->pc_Device->pd_Hardware;
    if (!(phw->phw_Capabilities & UHCF_RT_ISO) ||
        (phw->phw_ContextBackend &&
         !(phw->phw_CtxCmdMask & UHCD_CTXCMD_BIT(NSCMD_USB_REGISTER_HOOKS))))
    {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR)libname, "Controller driver does not support realtime isochronous transfers.");
        return (NULL);
    }

    if((prt = psdAllocVec(sizeof(struct PsdRTIsoHandler)))) {
        prt->prt_Device = pep->pep_Interface->pif_Config->pc_Device;
        prt->prt_Endpoint = pep;
        prt->prt_RTIso.urti_OutPrefetch = 2048;
        if((pp = prt->prt_Pipe = psdAllocPipe(prt->prt_Device, (struct MsgPort *) 0xffffffff, pep))) {
            pp->pp_MsgPort = pp->pp_Msg.mn_ReplyPort = NULL;
            psdSetAttrsA(PGA_RTISO, prt, tags);
            pp->pp_IOReq.iouh_Req.io_Command = UHCMD_ADDISOHANDLER;
            pp->pp_IOReq.iouh_Data = &prt->prt_RTIso;
            ioerr = pRtIsoForwardCmd(pp, ps);
            if(!ioerr) {
                Forbid();
                AddTail(&prt->prt_Device->pd_RTIsoHandlers, &prt->prt_Node);
                Permit();
                return(prt);
            } else {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Adding RT Iso Handler failed: %s (%ld)",
                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            }
            psdFreePipe(prt->prt_Pipe);
        }
        psdFreeVec(prt);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdFreeRTIsoHandler()" */
void (psdFreeRTIsoHandler)(struct PsdRTIsoHandler * prt asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *pp;

    if(!prt) {
        return;
    }
    Forbid();
    Remove(&prt->prt_Node);
    Permit();
    pp = prt->prt_Pipe;
    if(prt->prt_Running) {
        /* freed without psdStopRTIso(): the stream ends with the handler */
        --pp->pp_Device->pd_IOBusyCount;
        prt->prt_Running = FALSE;
    }
    pp->pp_IOReq.iouh_Req.io_Command = UHCMD_REMISOHANDLER;
    pRtIsoForwardCmd(pp, ps);
    psdFreePipe(pp);
    psdFreeVec(prt);
}
/* \\\ */

/* /// "psdStartRTIso()" */
LONG (psdStartRTIso)(struct PsdRTIsoHandler * prt asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *pp;
    LONG ioerr;

    if(!prt) {
        return UHIOERR_BADPARAMS;
    }
    pp = prt->prt_Pipe;
    if(pp->pp_Device->pd_Flags & PDFF_SUSPENDED) {
        // make sure the device is up and running before trying to send a new pipe
        psdResumeDevice(pp->pp_Device);
    }
    pp->pp_IOReq.iouh_Req.io_Command = UHCMD_STARTRTISO;
    ioerr = pRtIsoForwardCmd(pp, ps);
    if(!ioerr && !prt->prt_Running) {
        /* not pActivityBegin(): this is a hold for the whole lifetime of the
           running stream, released by psdStopRTIso() or psdFreeRTIsoHandler(),
           on top of the pair that pRtIsoForwardCmd() already balanced around
           the START command itself. One per stream, however often started. */
        ++pp->pp_Device->pd_IOBusyCount;
        prt->prt_Running = TRUE;
    }
    return(ioerr);
}
/* \\\ */

/* /// "psdStopRTIso()" */
LONG (psdStopRTIso)(struct PsdRTIsoHandler * prt asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdPipe *pp;
    LONG ioerr;

    if(!prt) {
        return UHIOERR_BADPARAMS;
    }
    pp = prt->prt_Pipe;
    pp->pp_IOReq.iouh_Req.io_Command = UHCMD_STOPRTISO;
    ioerr = pRtIsoForwardCmd(pp, ps);
    if(!ioerr && prt->prt_Running) {
        --pp->pp_Device->pd_IOBusyCount;   /* release psdStartRTIso()'s lifetime hold */
        prt->prt_Running = FALSE;
    }
    return(ioerr);
}
/* \\\ */

/* *** Classes *** */

/* /// "psdAddClass()" */
struct PsdUsbClass * (psdAddClass)(STRPTR name asm("a1"), ULONG vers asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct Library *cls = NULL;
    struct PsdUsbClass *puc;
    IPTR pri = 0;
    STRPTR desc;
    STRPTR origname = name;

    KPRINTF(5, ("psdAddClass(%s, %ld)\n", name, vers));

    while(*name) {
        if((cls = OpenLibrary(name, vers))) {
            break;
        }
        do {
            if((*name == '/') || (*name == ':')) {
                ++name;
                break;
            }
        } while(*(++name));
    }
    if(cls) {
        Forbid();
        if(FindName(&ps->ps_Classes, cls->lib_Node.ln_Name)) {
            Permit();
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Class %s is already installed.",
                           name);
            KPRINTF(20, ("attempt to add class twice!\n"));
            CloseLibrary(cls);
            return(NULL);
        }
        Permit();
        if((puc = psdAllocVec(sizeof(struct PsdUsbClass)))) {
            puc->puc_Base = ps;
            puc->puc_ClassBase = cls;
            puc->puc_Node.ln_Name = puc->puc_ClassName = psdCopyStr(cls->lib_Node.ln_Name);
            puc->puc_FullPath = psdCopyStr(origname);

            usbGetAttrs(UGA_CLASS, NULL,
                        UCCA_Priority, &pri,
                        UCCA_Description, &desc,
                        TAG_END);

            puc->puc_Node.ln_Pri = pri;
            psdLockWritePBase();
            Enqueue(&ps->ps_Classes, &puc->puc_Node);
            psdUnlockPBase();
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Added class %s V%ld.%ld (%s).",
                           cls->lib_Node.ln_Name, cls->lib_Version, cls->lib_Revision, desc);
            psdSendEvent(EHMB_ADDCLASS, puc, NULL);
            return(puc);
        }
        CloseLibrary(cls);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdRemClass()" */
void (psdRemClass)(struct PsdUsbClass * puc asm("a1"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(5, ("psdRemClass(0x%08lx)\n", puc));
    psdLockWritePBase();
    Remove(&puc->puc_Node);
    psdUnlockPBase();

    /* Release any bindings still held, best effort. Each restart pass clears
       the binding it targeted, so the sweep terminates. */
    BOOL restart;
    psdLockReadPBase();
    do {
        restart = FALSE;
        struct PsdDevice *pd = NULL;
        while((pd = psdGetNextDevice(pd))) {
            if(pd->pd_DevBinding && (pd->pd_ClsBinding == puc) && (!(pd->pd_Flags & PDFF_APPBINDING))) {
                psdUnlockPBase();
                psdReleaseDevBinding(pd);
                psdLockReadPBase();
                restart = TRUE;
                break;
            }
            struct PsdConfig *pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            while(pc->pc_Node.ln_Succ) {
                struct PsdInterface *pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                while(pif->pif_Node.ln_Succ) {
                    if(pif->pif_IfBinding && (pif->pif_ClsBinding == puc)) {
                        psdUnlockPBase();
                        psdReleaseIfBinding(pif);
                        psdLockReadPBase();
                        restart = TRUE;
                        break;
                    }
                    pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                }
                if(restart) {
                    break;
                }
                pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
            }
            if(restart) {
                break;
            }
        }
    } while(restart);
    psdUnlockPBase();

    if(puc->puc_UseCnt) {
        /* counter out of sync with the binding fields: leaking the class is
           safer than closing a library something still believes it holds */
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "Class %s still in use (cnt=%ld); not removed.",
                       puc->puc_ClassBase->lib_Node.ln_Name, puc->puc_UseCnt);
        return;
    }
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Removed class %s.",
                   puc->puc_ClassBase->lib_Node.ln_Name);
    /* before the free; the note still arrives after it (identity only) */
    psdSendEvent(EHMB_REMCLASS, puc, NULL);
    CloseLibrary(puc->puc_ClassBase);
    psdFreeVec(puc->puc_ClassName);
    psdFreeVec(puc->puc_FullPath);
    psdFreeVec(puc);
}
/* \\\ */

/* *** Error Msgs *** */

/* /// "psdAddErrorMsgA()" */
struct PsdErrorMsg * (psdAddErrorMsgA)(UWORD level asm("d0"), STRPTR origin asm("a0"), STRPTR fmtstr asm("a1"), RAWARG fmtdata asm("a2"), struct PsdBase * ps asm("a6"))
{
    struct PsdErrorMsg *pem;
    if(((!ps->ps_GlobalCfg->pgc_LogInfo) && (level < RETURN_WARN)) ||
            ((!ps->ps_GlobalCfg->pgc_LogWarning) && (level >= RETURN_WARN) && (level < RETURN_ERROR)) ||
            ((!ps->ps_GlobalCfg->pgc_LogError) && (level >= RETURN_ERROR) && (level < RETURN_FAIL)) ||
            ((!ps->ps_GlobalCfg->pgc_LogFailure) && (level >= RETURN_FAIL))) {
        return(NULL);
    }
    if((pem = psdAllocVec(sizeof(struct PsdErrorMsg)))) {
        pem->pem_Base = ps;
        pem->pem_Level = level;
        if((pem->pem_Origin = psdCopyStr(origin))) {
            if((pem->pem_Msg = psdCopyStrFmtA(fmtstr, fmtdata))) {
                /*
                 * Its possible that we get called in supervisor mode here,
                 * due to a race condition when booting - so dont try to open
                 * DOS directly since it will cause a crash/stack/corruption
                 */
                if(pHaveDOS(ps)) {
                    DateStamp(&pem->pem_DateStamp);
                } else {
                    struct timerequest tr = ps->ps_TimerIOReq;
                    tr.tr_node.io_Command = TR_GETSYSTIME;
                    DoIO((struct IORequest *) &tr);
                    pem->pem_DateStamp.ds_Days = tr.tr_time.tv_secs / (24*60*60);
                    pem->pem_DateStamp.ds_Minute = (tr.tr_time.tv_secs / 60) % 60;
                    pem->pem_DateStamp.ds_Tick = (tr.tr_time.tv_secs % 60) * 50;
                }
                Forbid();
                AddTail(&ps->ps_ErrorMsgs, &pem->pem_Node);
                Permit();
                psdSendEvent(EHMB_ADDERRORMSG, pem, NULL);
                return(pem);
            }
            psdFreeVec(pem->pem_Origin);
        }
        psdFreeVec(pem);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdRemErrorMsg()" */
void (psdRemErrorMsg)(struct PsdErrorMsg * pem asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(1, ("psdRemErrorMsg()\n"));
    Forbid();
    Remove(&pem->pem_Node);
    Permit();
    psdFreeVec(pem->pem_Origin);
    psdFreeVec(pem->pem_Msg);
    psdFreeVec(pem);
    psdSendEvent(EHMB_REMERRORMSG, pem, NULL);
}
/* \\\ */

/* *** Bindings *** */

/* /// "pFindIfBinding()" */
/* The first interface of any configuration of pd that puc is bound to, or
   NULL. The caller holds the PBase lock. */
static struct PsdInterface * pFindIfBinding(struct PsdDevice *pd, struct PsdUsbClass *puc)
{
    struct PsdConfig *pc;
    struct PsdInterface *pif;

    ForeachNode(&pd->pd_Configs, pc) {
        ForeachNode(&pc->pc_Interfaces, pif) {
            if(pif->pif_IfBinding && (pif->pif_ClsBinding == puc)) {
                return pif;
            }
        }
    }
    return NULL;
}
/* \\\ */

/* /// "pReleaseAfterDOSBindings()" */
/*
 * Called the first time a class scan happens from a process after the stack
 * was started as a task (i.e. before DOS was available). Classes that flag
 * themselves with UCCA_AfterDOSRestart (bootmouse.class, bootkeyboard.class)
 * are only meant to bridge the gap until the full classes (hid.class) can be
 * loaded, so their bindings are released here to let the subsequent scan
 * hand the devices over. All classes are told about DOS being available.
 */
static void pReleaseAfterDOSBindings(struct PsdBase *ps)
{
    struct PsdUsbClass *puc;
    IPTR restartme;

    psdLockReadPBase();
    psdAddErrorMsg0(RETURN_OK, (STRPTR) libname, "Checking AfterDOS...");
    puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
    while(puc->puc_Node.ln_Succ) {
        restartme = FALSE;
        usbGetAttrs(UGA_CLASS, NULL,
                    UCCA_AfterDOSRestart, &restartme,
                    TAG_END);

        if(restartme && puc->puc_UseCnt) {
            struct PsdDevice *pd;

            /* Well, try to release the open bindings in a best effort attempt.
               Each release drops the lock, so the walk starts over after it. */
            pd = NULL;
            while((pd = psdGetNextDevice(pd))) {
                struct PsdInterface *pif;

                if(pd->pd_DevBinding && (pd->pd_ClsBinding == puc) && (!(pd->pd_Flags & PDFF_APPBINDING))) {
                    psdUnlockPBase();
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "AfterDOS: Temporarily releasing %s %s binding to %s.",
                                   puc->puc_ClassName, "device", pd->pd_ProductStr);
                    psdReleaseDevBinding(pd);
                    psdLockReadPBase();
                    pd = NULL; /* restart */
                    continue;
                }
                if((pif = pFindIfBinding(pd, puc))) {
                    psdUnlockPBase();
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "AfterDOS: Temporarily releasing %s %s binding to %s.",
                                   puc->puc_ClassName, "interface", pd->pd_ProductStr);
                    psdReleaseIfBinding(pif);
                    psdLockReadPBase();
                    pd = NULL; /* restart */
                }
            }
        }
        usbDoMethodA(UCM_DOSAvailableEvent, NULL);
        puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
    }
    psdUnlockPBase();
}
/* \\\ */

/* /// "psdClassScan()" */
void (psdClassScan)(struct PsdBase * ps asm("a6"))
{
    struct PsdHardware *phw;
    struct PsdDevice *pd;
    struct PsdUsbClass *puc;
    BOOL nodos = (FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS);
    BOOL handover;

    if(nodos && (!ps->ps_ConfigRead)) {
        // it's the first time we were reading the config and DOS was not available
        ps->ps_StartedAsTask = TRUE;
    }

    /*
     * First scan from a process after the stack came up as a task before DOS:
     * the boot classes only bridged the gap, so release their bindings now and
     * let the classes that are available by now (hid.class) take the devices
     * over. This used to happen only in psdParseCfg(), i.e. only when a saved
     * prefs file existed, leaving bootmouse/bootkeyboard in charge forever
     * on systems without one.
     */
    Forbid();
    handover = (!nodos) && ps->ps_StartedAsTask;
    if(handover) {
        ps->ps_StartedAsTask = FALSE;
    }
    Permit();
    if(handover) {
        pReleaseAfterDOSBindings(ps);
    }

    psdLockReadPBase();

    puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
    if(!puc->puc_Node.ln_Succ) {
        psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname, "ClassScan attempted with no classes installed!");
        psdUnlockPBase();
        return;
    }

    phw = (struct PsdHardware *) ps->ps_Hardware.lh_Head;
    while(phw->phw_Node.ln_Succ) {
        pd = (struct PsdDevice *) phw->phw_Devices.lh_Head;
        while (pd->pd_Node.ln_Succ) {
            if (!pd->pd_Hub) {
                // for the root, do it ourselves, the rest is done by each hub task
                psdHubClassScan(pd);
            }
            pd = (struct PsdDevice *) pd->pd_Node.ln_Succ;
        }
        phw = (struct PsdHardware *) phw->phw_Node.ln_Succ;
    }
    psdUnlockPBase();
    //psdSendEvent(EHMB_CLSSCANRDY, NULL, NULL);
    KPRINTF(5, ("************ Scanning finished!\n"));
}
/* \\\ */

/* /// "psdDoHubMethodA()" */
LONG (psdDoHubMethodA)(struct PsdDevice * pd asm("a0"), ULONG methodid asm("d0"), APTR methoddata asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    KPRINTF(2, ("psdDoHubMethodA(0x%08lx)\n", pd));

    if(pd) {
        if(pd->pd_Hub) {
            if((pd->pd_Hub->pd_DevBinding) && (puc = pd->pd_Hub->pd_ClsBinding)) {
                return(usbDoMethodA(methodid, methoddata));
            }
        }
    }
    return 0;
}
/* \\\ */

/* /// "psdClaimAppBindingA()" */
struct PsdAppBinding * (psdClaimAppBindingA)(struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct PsdDevice *hubpd;
    struct PsdAppBinding tmppab;
    struct PsdAppBinding *pab = NULL;
    struct PsdUsbClass *puc;

    APTR binding;

    KPRINTF(2, ("psdClaimAppBindingA(0x%08lx)\n", tags));

    tmppab.pab_Device = NULL;
    tmppab.pab_ReleaseHook = NULL;
    tmppab.pab_Task = NULL;
    tmppab.pab_ForceRelease = FALSE;
    psdSetAttrsA(PGA_APPBINDING, &tmppab, tags);
    if(tmppab.pab_Device && tmppab.pab_ReleaseHook) {
        pd = tmppab.pab_Device;

        // force release of other bindings first
        if(tmppab.pab_ForceRelease) {
            /* If there are bindings, get rid of them. */
            if(pd->pd_DevBinding) {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Task %s claimed '%s'; the previous binding was released.",
                               FindTask(NULL)->tc_Node.ln_Name,
                               pd->pd_ProductStr);

                psdReleaseDevBinding(pd);
            } else {
                pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
                while(pc->pc_Node.ln_Succ) {
                    pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                    while(pif->pif_Node.ln_Succ) {
                        if(pif->pif_IfBinding) {
                            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                           "Task %s claimed '%s'; the previous binding was released.",
                                           FindTask(NULL)->tc_Node.ln_Name,
                                           pd->pd_ProductStr);
                            psdReleaseIfBinding(pif);
                        }
                        pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                    }
                    pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
                }
            }
        }
        hubpd = pd->pd_Hub;
        if(!hubpd) { // claim app binding at the root hub -- improbable, but possible.
            pab = psdHubClaimAppBindingA(tags);
        } else {
            if((binding = hubpd->pd_DevBinding) && (puc = hubpd->pd_ClsBinding)) {
                pab = (struct PsdAppBinding *) usbDoMethod(UCM_HubClaimAppBinding, binding, tags);
            }
        }
        if(pab) {
            // fill in task names
            pab->pab_Task = FindTask(NULL);
            pab->pab_Node.ln_Name = pab->pab_Task->tc_Node.ln_Name;
            psdSendEvent(EHMB_ADDBINDING, pd, NULL);
            return(pab);
        }
    }
    return(NULL);
}
/* \\\ */

/* Releases run directly against the target's own binding fields: the real
   serializer is the device write lock plus the NULL-before-invoke idempotency
   inside the psdHubRelease* primitives. Only claim/suspend/resume route
   through the parent hub's task (UCM_Hub*), as those drive the hub's EP0 pipe
   and port state. A release routed that way would be dropped whenever the hub
   itself is tearing down: its own binding fields -- the routing token -- are
   already NULL for the whole of the hub class's release. */

/* /// "psdReleaseAppBinding()" */
void (psdReleaseAppBinding)(struct PsdAppBinding * pab asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(2, ("psdReleaseAppBinding(0x%08lx)\n", pab));

    if(pab) {
        /* runs pab_ReleaseHook in this task's context */
        psdHubReleaseDevBinding(pab->pab_Device);
    }
}
/* \\\ */

/* /// "psdReleaseDevBinding()" */
void (psdReleaseDevBinding)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(5, ("psdReleaseDevBinding(0x%08lx)\n", pd));
    if(pd->pd_DevBinding) {
        psdHubReleaseDevBinding(pd);
    }
}
/* \\\ */

/* /// "psdReleaseIfBinding()" */
void (psdReleaseIfBinding)(struct PsdInterface * pif asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(5, ("psdReleaseIfBinding(0x%08lx)\n", pif));
    if(pif->pif_IfBinding && pif->pif_ClsBinding) {
        psdHubReleaseIfBinding(pif);
    }
}
/* \\\ */

/* /// "psdUnbindAll()" */
void (psdUnbindAll)(struct PsdBase * ps asm("a6"))
{
    struct PsdHardware *phw;
    struct PsdDevice *pd;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    BOOL restart;

    KPRINTF(10, ("pUnbindAll()\n"));
    /* FIXME What happens if devices or hardware gets removed during the process? Need notify semaphore */
    psdLockReadPBase();
    do {
        restart = FALSE;
        phw = (struct PsdHardware *) ps->ps_Hardware.lh_Head;
        while(phw->phw_Node.ln_Succ) {
            pd = (struct PsdDevice *) phw->phw_Devices.lh_Head;
            while(pd->pd_Node.ln_Succ) {
                /* If there are bindings, get rid of them. */
                if(pd->pd_DevBinding) {
                    psdUnlockPBase();
                    psdReleaseDevBinding(pd);
                    psdLockReadPBase();
                    restart = TRUE;
                    break;
                }
                pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
                while(pc->pc_Node.ln_Succ) {
                    pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                    while(pif->pif_Node.ln_Succ) {
                        if(pif->pif_IfBinding) {
                            psdUnlockPBase();
                            psdReleaseIfBinding(pif);
                            psdLockReadPBase();
                            restart = TRUE;
                            break;
                        }
                        pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                    }
                    if(restart) {
                        break;
                    }
                    pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
                }
                if(restart) {
                    break;
                }
                pd = (struct PsdDevice *) pd->pd_Node.ln_Succ;
            }
            if(restart) {
                break;
            }
            phw = (struct PsdHardware *) phw->phw_Node.ln_Succ;
        }
    } while(restart);
    psdUnlockPBase();
}
/* \\\ */

/* /// "psdHubClassScan()" */
void (psdHubClassScan)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct PsdInterface *firstpif;
    struct PsdPipe *pp = NULL;
    struct MsgPort *mp;
    APTR binding;
    UWORD hasifbinding;
    BOOL mainif;
    STRPTR owner;

    KPRINTF(5, ("psdClassScan()\n"));

    if(!(mp = CreateMsgPort())) {
        return;
    }
    psdLockReadPBase();
    psdLockWriteDevice(pd);
    while(!(pd->pd_NotifyCfg.ntc_NoClassBind || pd->pd_DevBinding)) {
        if(!(pp = psdAllocPipe(pd, mp, NULL))) {
            break;
        }
        KPRINTF(5, ("Doing ClassScan on Device: %s\n", pd->pd_ProductStr));
        hasifbinding = 0;
        /* First look if there is any interface binding. We may not change
           the current config in this case! */
        pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        while(pc->pc_Node.ln_Succ) {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;

            while(pif->pif_Node.ln_Succ) {
                if(pif->pif_IfBinding) {
                    hasifbinding = pc->pc_CfgNum;
                    break;
                }
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
        }

        owner = psdGetForcedBinding(pd->pd_IDString, NULL);
        if((!hasifbinding) && owner) {
            puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
            while(puc->puc_Node.ln_Succ) {
                if(!strcmp(owner, puc->puc_ClassName)) {
                    if((pd->pd_DevBinding = (APTR) usbDoMethod(UCM_ForceDeviceBinding, pd))) {
                        pd->pd_ClsBinding = puc;
                        puc->puc_UseCnt++;
                        psdSendEvent(EHMB_ADDBINDING, pd, NULL);
                    } else {
                        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                       "Forced device binding of %s to %s failed.", pd->pd_ProductStr, owner);
                    }
                    break;
                }
                puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
            }
            /* no more scanning required, abort here */
            break;
        }

        /* Second attempt */
        pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        while(pc->pc_Node.ln_Succ) {
            if((!hasifbinding) || (hasifbinding == pc->pc_CfgNum)) {
                /* If the current config is not the one selected, change it */
                if(pd->pd_CurrCfg != pc->pc_CfgNum) {
                    psdSetDeviceConfig(pp, pc->pc_CfgNum);
                }
                KPRINTF(5, ("  Config %ld\n", pc->pc_CfgNum));
                /* If something went wrong above, we must exclude this config */
                if(pd->pd_CurrCfg == pc->pc_CfgNum) {
                    pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                    while(pif->pif_Node.ln_Succ) {
                        KPRINTF(5, ("    Interface %ld\n", pif->pif_IfNum));
                        firstpif = pif;
                        mainif = TRUE;
                        if(!pif->pif_IfBinding) {
                            binding = NULL;
                            /* Offer the active alternate first, then the inactive ones, WITHOUT
                               switching the wire per probe (classes decide from the parsed
                               descriptors); the accepted alternate is switched to exactly once.
                               While probing, the interface tree stays untouched, so iteration is
                               simply firstpif followed by firstpif's pif_AlterIfs list. */
                            do {
                                owner = psdGetForcedBinding(pd->pd_IDString, pif->pif_IDString);
                                puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
                                while(puc->puc_Node.ln_Succ) {
                                    KPRINTF(5, (">>>PING %s!\n", puc->puc_ClassName));
                                    if(owner) {
                                        if(!strcmp(owner, puc->puc_ClassName)) {
                                            binding = (APTR) usbDoMethod(UCM_ForceInterfaceBinding, pif);
                                            if(!binding) {
                                                psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                                               "Forced interface binding of %s to %s failed.", pd->pd_ProductStr, owner);
                                            }
                                        }
                                        if(!binding) {
                                            puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
                                            continue;
                                        }
                                    } else {
                                        binding = (APTR) usbDoMethod(UCM_AttemptInterfaceBinding, pif);
                                    }
                                    KPRINTF(5, ("<<<PONG!!\n"));
                                    if(binding) {
                                        KPRINTF(5, ("Got binding!\n"));
                                        /* Make the accepted alternate the active one (wire
                                           SET_INTERFACE + tree resort; no-op if already active). */
                                        if(!psdSetAltInterface(pp, pif)) {
                                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                           "Could not switch %s to alt %ld for binding, skipping alternate.",
                                                           pd->pd_ProductStr, pif->pif_Alternate);
                                            usbDoMethod(UCM_ReleaseInterfaceBinding, binding);
                                            binding = NULL;
                                            break; /* alternate unusable, try the next one */
                                        }
                                        Forbid();
                                        /* Find root config structure (the accepted alternate,
                                           now in the main interface list) */
                                        pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                                        while(pif->pif_Node.ln_Succ) {
                                            if(pif->pif_IfNum == firstpif->pif_IfNum) {
                                                break;
                                            }
                                            pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                                        }
                                        if(!pif->pif_Node.ln_Succ) {
                                            KPRINTF(5, ("Interface list walk fell off the end!\n"));
                                            psdAddErrorMsg0(RETURN_FAIL, (STRPTR) libname, "Interface list walk failed; giving up.");
                                            Permit();
                                            break;
                                        }
                                        pif->pif_IfBinding = binding;
                                        pif->pif_ClsBinding = puc;
                                        hasifbinding = pc->pc_CfgNum;
                                        puc->puc_UseCnt++;
                                        psdSendEvent(EHMB_ADDBINDING, pd, NULL);
                                        Permit();
                                        break;
                                    }
                                    puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
                                }
                                if(binding) {
                                    break;
                                }
                                /* Advance to the next inactive alternate of the ORIGINAL main
                                   interface - the tree was not resorted while probing. */
                                if(mainif) {
                                    if(!firstpif->pif_AlterIfs.lh_Head->ln_Succ) {
                                        break; /* no alternates */
                                    }
                                    pif = (struct PsdInterface *) firstpif->pif_AlterIfs.lh_Head;
                                    mainif = FALSE;
                                } else {
                                    pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                                    if(!pif->pif_Node.ln_Succ) {
                                        break; /* alternates exhausted */
                                    }
                                }
                            } while(TRUE);
                            /* No restore needed: without a binding the wire was never switched. */
                            /* Hohum, search current main interface then */
                            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
                            while(pif->pif_Node.ln_Succ) {
                                if(pif->pif_IfNum == firstpif->pif_IfNum) {
                                    break;
                                }
                                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                            }
                        }
                        pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                    }
                }
            }
            KPRINTF(5, ("End, next ConfigCheck!\n"));
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
        }
        /* Could not establish interface binding, try device binding then */
        //psdUnlockPBase();
        if(!hasifbinding) {
            //pd->pd_DevBinding = (APTR) ~0UL;
            binding = NULL;
            owner = psdGetForcedBinding(pd->pd_IDString, NULL);
            puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
            while(puc->puc_Node.ln_Succ) {
                binding = NULL;
                if(owner) {
                    if(!strcmp(owner, puc->puc_ClassName)) {
                        binding = (APTR) usbDoMethod(UCM_ForceDeviceBinding, pd, TAG_END);
                        if(!binding) {
                            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                           "Forced device binding of %s to %s failed.", pd->pd_ProductStr, owner);
                        }
                    }
                    if(!binding) {
                        puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
                        continue;
                    }
                } else {
                    binding = (APTR) usbDoMethod(UCM_AttemptDeviceBinding, pd);
                }
                if(binding) {
                    pd->pd_DevBinding = binding;
                    pd->pd_ClsBinding = puc;
                    puc->puc_UseCnt++;
                    psdSendEvent(EHMB_ADDBINDING, pd, NULL);
                    break;
                }
                puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
            }
            pd->pd_DevBinding = binding;
        }
        break;
    }
    if(pp) {
        psdFreePipe(pp);
    }
    // call hub class scan code
    if((binding = pd->pd_DevBinding) && (puc = pd->pd_ClsBinding)) {
        usbDoMethod(UCM_HubClassScan, binding);
    }
    psdUnlockDevice(pd);
    psdUnlockPBase();
    DeleteMsgPort(mp);
}
/* \\\ */

/* /// "psdHubClaimAppBindingA()" */
struct PsdAppBinding * (psdHubClaimAppBindingA)(struct TagItem * tags asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdDevice *pd;
    struct PsdAppBinding *pab;
    struct PsdConfig *pc;
    struct PsdInterface *pif;

    BOOL hasbinding = FALSE;
    KPRINTF(2, ("psdHubClaimAppBindingA(0x%08lx)\n", tags));

    if((pab = psdAllocVec(sizeof(struct PsdAppBinding)))) {
        psdSetAttrsA(PGA_APPBINDING, pab, tags);
        if(pab->pab_Device && pab->pab_ReleaseHook) {
            pd = pab->pab_Device;
            if(pd->pd_DevBinding) {
                hasbinding = TRUE;
            } else {
                pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
                while(pc->pc_Node.ln_Succ) {
                    pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;

                    while(pif->pif_Node.ln_Succ) {
                        if(pif->pif_IfBinding) {
                            hasbinding = TRUE;
                            break;
                        }
                        pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                    }
                    pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
                }
            }
            if(!hasbinding) {
                pd->pd_Flags |= PDFF_APPBINDING;
                pd->pd_DevBinding = pab;
                pd->pd_ClsBinding = NULL;
                return(pab);
            }
        }
        psdFreeVec(pab);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdHubReleaseDevBinding()" */
void (psdHubReleaseDevBinding)(struct PsdDevice * pd asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    APTR binding;
    struct PsdAppBinding *pab;

    KPRINTF(5, ("psdHubReleaseDevBinding(0x%08lx)\n", pd));
    if(pd) {
        psdLockWriteDevice(pd);
        if((binding = pd->pd_DevBinding)) {
            pd->pd_DevBinding = NULL;
            if(pd->pd_Flags & PDFF_APPBINDING) {
                pab = (struct PsdAppBinding *) binding;
                CallHookPkt(pab->pab_ReleaseHook, pab, (APTR) pab->pab_UserData);
                pd->pd_ClsBinding = NULL;
                pd->pd_Flags &= ~PDFF_APPBINDING;
                psdFreeVec(pab);
                psdSendEvent(EHMB_REMBINDING, pd, NULL);
            } else {
                puc = pd->pd_ClsBinding;
                if(puc) {
                    pd->pd_ClsBinding = NULL;
                    usbDoMethod(UCM_ReleaseDeviceBinding, binding);
                    puc->puc_UseCnt--;
                    psdSendEvent(EHMB_REMBINDING, pd, NULL);
                }
            }
        }
        psdUnlockDevice(pd);
    }
}
/* \\\ */

/* /// "psdHubReleaseIfBinding()" */
void (psdHubReleaseIfBinding)(struct PsdInterface * pif asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdUsbClass *puc;
    struct PsdDevice *pd;
    APTR binding;

    KPRINTF(5, ("psdHubReleaseIfBinding(0x%08lx)\n", pif));

    if(pif) {
        pd = pif->pif_Config->pc_Device;
        psdLockWriteDevice(pd);
        if((binding = pif->pif_IfBinding)) {
            pif->pif_IfBinding = NULL;
            puc = pif->pif_ClsBinding;
            if(puc) {
                pif->pif_ClsBinding = NULL;
                usbDoMethod(UCM_ReleaseInterfaceBinding, binding);
                puc->puc_UseCnt--;
            }
            psdSendEvent(EHMB_REMBINDING, pd, NULL);
        }
        psdUnlockDevice(pd);
    }
}
/* \\\ */

/* *** Events *** */

/* /// "psdAddEventHandler()" */
struct PsdEventHook * (psdAddEventHandler)(struct MsgPort * mp asm("a1"), ULONG msgmask asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdEventHook *peh = NULL;

    KPRINTF(5, ("psdAddEventHandler(0x%08lx, 0x%08lx)\n", mp, msgmask));

    if(mp) {
        ObtainSemaphore(&ps->ps_ReentrantLock);
        if((peh = psdAllocVec(sizeof(struct PsdEventHook)))) {
            peh->peh_MsgPort = mp;
            peh->peh_MsgMask = msgmask;
            AddTail(&ps->ps_EventHooks, &peh->peh_Node);
        }
        ReleaseSemaphore(&ps->ps_ReentrantLock);
    }
    return(peh);
}
/* \\\ */

/* /// "psdRemEventHandler()" */
void (psdRemEventHandler)(struct PsdEventHook * peh asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct Message *msg;

    KPRINTF(5, ("psdRemEventHandler(0x%08lx)\n", peh));
    if(!peh) {
        return;
    }
    ObtainSemaphore(&ps->ps_ReentrantLock);
    Remove(&peh->peh_Node);
    while((msg = GetMsg(peh->peh_MsgPort))) {
        ReplyMsg(msg);
    }
    ReleaseSemaphore(&ps->ps_ReentrantLock);
    pGarbageCollectEvents(ps);
    psdFreeVec(peh);
}
/* \\\ */

/* /// "pSendEventWithData()" */
/* psdSendEvent(). With data, each note carries its own copy of len bytes of
   it, behind the note in the same allocation, and Param2 points at that
   copy: it lives exactly as long as the note. */
static void pSendEventWithData(struct PsdBase *ps, ULONG ehmt, APTR param1, APTR param2, CONST_APTR data, ULONG len)
{
    struct PsdEventNote *pen;
    struct PsdEventHook *peh;
    ULONG msgmask = (1L<<ehmt);

    KPRINTF(1, ("psdSendEvent(0x%08lx, 0x%08lx, 0x%08lx)\n", ehmt, param1, param2));

    pGarbageCollectEvents(ps);
    ObtainSemaphore(&ps->ps_ReentrantLock);
    peh = (struct PsdEventHook *) ps->ps_EventHooks.lh_Head;
    while(peh->peh_Node.ln_Succ) {
        if(peh->peh_MsgMask & msgmask) {
            if((pen = psdAllocVec(sizeof(struct PsdEventNote) + len))) {
                pen->pen_Msg.mn_ReplyPort = &ps->ps_EventReplyPort;
                pen->pen_Msg.mn_Length = sizeof(struct PsdEventNote);
                pen->pen_Event = ehmt;
                pen->pen_Param1 = param1;
                pen->pen_Param2 = param2;
                if(data) {
                    pen->pen_Param2 = pen + 1;
                    CopyMem((APTR) data, pen->pen_Param2, len);
                }
                PutMsg(peh->peh_MsgPort, &pen->pen_Msg);
            }
        }
        peh = (struct PsdEventHook *) peh->peh_Node.ln_Succ;
    }
    ReleaseSemaphore(&ps->ps_ReentrantLock);
}
/* \\\ */

/* /// "psdSendEvent()" */
void (psdSendEvent)(ULONG ehmt asm("d0"), APTR param1 asm("a0"), APTR param2 asm("a1"), struct PsdBase * ps asm("a6"))
{
    pSendEventWithData(ps, ehmt, param1, param2, NULL, 0);
}
/* \\\ */

/* *** Configuration *** */

/* /// "psdReadCfg()" */
BOOL (psdReadCfg)(struct PsdIFFContext * pic asm("a0"), APTR formdata asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *subpic;
    LONG len;
    ULONG chlen;
    ULONG *buf = formdata;
    BOOL res = TRUE;
    KPRINTF(10, ("psdReadCfg(0x%08lx, 0x%08lx)\n", pic, formdata));

    pLockSemExcl(ps, &ps->ps_ConfigLock);
    if(!pic) {
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
        if(!(pic->pic_Node.ln_Succ)) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return(FALSE);
        }
    }
    if((AROS_LONG2BE(*buf) != ID_FORM) || (AROS_LONG2BE(buf[2]) != pic->pic_FormID)) {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR) libname, "Tried to replace a cfg form with a chunk or with an alien form!");
        pUnlockSem(ps, &ps->ps_ConfigLock);
        return(FALSE);
    }
    subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    while(subpic->pic_Node.ln_Succ) {
        pFreeForm(ps, subpic);
        subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    }
    pic->pic_ChunksLen = 0;
    len = (AROS_LONG2BE(buf[1]) - 3) & ~1UL;
    buf += 3;
    while(len >= 8) {
        if(!(pAddCfgChunk(ps, pic, buf))) {
            break;
        }
        chlen = (AROS_LONG2BE(buf[1]) + 9) & ~1UL;
        len -= chlen;
        buf = (ULONG *) (((UBYTE *) buf) + chlen);
    }
    if(len) {
        psdAddErrorMsg0(RETURN_FAIL, (STRPTR) libname, "Corrupted FORM chunk; the configuration may be damaged.");
        res = 0;
    }

    pUnlockSem(ps, &ps->ps_ConfigLock);
    ps->ps_CheckConfigReq = TRUE;
    return(res);
}
/* \\\ */

/* /// "pMigrateCfgString()" */
/* Replace the leading oldp of the string held in chunk chnkid of pic with newp:
   a drawer prefix ("SYS:Classes/USB/hid.class" -> "DEVS:USBClasses/hid.class")
   or a whole value ("Trident" -> "NeptuneUSB"). oldp must match a whole path
   component (case-insensitive, as AmigaDOS is); a missing chunk or any other
   string is left alone. Returns TRUE if the chunk was rewritten. */
static BOOL pMigrateCfgString(struct PsdBase *ps, struct PsdIFFContext *pic, ULONG chnkid,
                              STRPTR oldp, STRPTR newp)
{
    ULONG *chnk = pFindCfgChunk(ps, pic, chnkid);
    if(!chnk) {
        return(FALSE);
    }
    /* chunk layout: ULONG id, ULONG length, then the NUL-terminated string */
    STRPTR str = (STRPTR) &chnk[2];
    ULONG oldlen = strlen(oldp);
    if(strnicmp((const char *) str, (const char *) oldp, oldlen)) {
        return(FALSE);                  /* different drawer or value */
    }
    STRPTR rest = &str[oldlen];         /* "/hid.class", or "" for an exact match */
    if(*rest && (*rest != '/')) {
        return(FALSE);                  /* only a partial name match, e.g. "SYS:Classes/USBx" */
    }
    /* pAddStringChunk() removes the old chunk by compacting (or reallocating)
       pic's chunk buffer, which overwrites `rest`, so build the new string first */
    STRPTR newstr = psdCopyStrFmt("%s%s", newp, rest);
    if(!newstr) {
        return(FALSE);
    }
    BOOL done = pAddStringChunk(ps, pic, chnkid, newstr);
    psdFreeVec(newstr);
    return(done);
}
/* \\\ */

/* /// "pMigrateCfg()" */
/* Rewrite what Poseidon prefs data carries over in Poseidon's names: the class
   paths of the UCLS forms and the attach/detach sound paths of the stack form,
   and the prefs editor's owner key on the per-device and per-interface data.
   Idempotent, so it runs wherever outside data enters the config. Returns TRUE
   if anything was rewritten. */
static BOOL pMigrateCfg(struct PsdBase *ps)
{
    BOOL changed = FALSE;

    pLockSemExcl(ps, &ps->ps_ConfigLock);
    struct PsdIFFContext *pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    if(pic) {
        for(struct PsdIFFContext *subpic = psdFindCfgForm(pic, IFFFORM_USBCLASS); subpic; subpic = psdNextCfgForm(subpic)) {
            changed |= pMigrateCfgString(ps, subpic, IFFCHNK_NAME, NEP_OLDCLASSDRAWER, NEP_CLASSDRAWER);
        }
        changed |= pMigrateCfgString(ps, pic, IFFCHNK_INSERTSND, NEP_OLDPRESETDRAWER, NEP_PRESETDRAWER);
        changed |= pMigrateCfgString(ps, pic, IFFCHNK_REMOVESND, NEP_OLDPRESETDRAWER, NEP_PRESETDRAWER);
    }
    for(struct PsdIFFContext *dev = psdFindCfgForm(NULL, IFFFORM_DEVICECFG); dev; dev = psdNextCfgForm(dev)) {
        for(struct PsdIFFContext *data = psdFindCfgForm(dev, IFFFORM_DEVCFGDATA); data; data = psdNextCfgForm(data)) {
            changed |= pMigrateCfgString(ps, data, IFFCHNK_OWNER, NEP_OLDPREFS_OWNER, NEP_PREFS_OWNER);
        }
        for(struct PsdIFFContext *data = psdFindCfgForm(dev, IFFFORM_IFCFGDATA); data; data = psdNextCfgForm(data)) {
            changed |= pMigrateCfgString(ps, data, IFFCHNK_OWNER, NEP_OLDPREFS_OWNER, NEP_PREFS_OWNER);
        }
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(changed);
}
/* \\\ */

/* /// "pLoadCfgFile()" */
/* Load and apply one prefs file, migrating Poseidon names first (pMigrateCfg()).
   Returns RETURN_OK, RETURN_WARN (no such file) or RETURN_ERROR (not a usable
   prefs file). */
static UWORD pLoadCfgFile(struct PsdBase *ps, STRPTR filename)
{
    XPRINTF(10, ("Loading config file: %s\n", filename));

    BPTR filehandle = Open(filename, MODE_OLDFILE);
    KPRINTF(1, ("File handle 0x%08lx\n", filehandle));
    if(!filehandle) {
        return(RETURN_WARN);
    }

    UWORD level = RETURN_ERROR;
    ULONG formhead[3];
    if((Read(filehandle, formhead, 12) == 12) &&
       (AROS_LONG2BE(formhead[0]) == ID_FORM) && (AROS_LONG2BE(formhead[2]) == IFFFORM_PSDCFG)) {
        ULONG formlen = AROS_LONG2BE(formhead[1]);
        KPRINTF(1, ("Header OK, %lu bytes\n", formlen));

        ULONG *buf = (ULONG *) psdAllocVec(formlen + 8);
        if(buf) {
            buf[0] = formhead[0];
            buf[1] = formhead[1];
            buf[2] = formhead[2];
            if(Read(filehandle, &buf[3], formlen - 4) == formlen - 4) {
                KPRINTF(1, ("Data read OK\n"));

                psdReadCfg(NULL, buf);
                BOOL migrated = pMigrateCfg(ps);
                psdParseCfg();      /* records the live config as saved */

                KPRINTF(1, ("All done\n"));
                level = RETURN_OK;
                /* a migrated config matches no file until it is saved */
                if(migrated) {
                    ps->ps_SavedConfigHash = ~ps->ps_ConfigHash;
                }
            }
            psdFreeVec(buf);
        }
    }
    Close(filehandle);
    return(level);
}
/* \\\ */

/* /// "psdLoadCfgFromDisk()" */
BOOL (psdLoadCfgFromDisk)(STRPTR filename asm("a1"), struct PsdBase * ps asm("a6"))
{
    if(!pOpenDOS(ps)) {
        KPRINTF(1, ("dos.library not available yet\n"));
        return(FALSE);
    }

    if(!filename) {
        /* Neptune's own prefs first; without them, a one-time import of
           Poseidon's, saved straight away as neptune.prefs so it is not
           repeated. A missing file is only reported if all are missing.
           Local array: a static pointer table would need a .data section. */
        STRPTR names[4] = { NEP_PREFS_ENV, NEP_PREFS_ENVARC, NEP_OLDPREFS_ENV, NEP_OLDPREFS_ENVARC };

        for(ULONG i = 0; i < 4; i++) {
            BOOL imported = (i >= 2);
            UWORD level = pLoadCfgFile(ps, names[i]);
            if(level == RETURN_OK) {
                if(imported && psdSaveCfgToDisk(NULL)) {
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "Settings imported from '%s' and saved to '%s'.",
                                   names[i], NEP_PREFS_ENVARC);
                }
                return(TRUE);
            }
            if(level == RETURN_ERROR) {
                psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname, "Failed to load config from '%s'!", names[i]);
            }
        }
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Failed to find config file '%s'!", NEP_PREFS_ENVARC);
        return(FALSE);
    }

    UWORD level = pLoadCfgFile(ps, filename);
    if(level != RETURN_OK) {
        psdAddErrorMsg(level, (STRPTR) libname,
                       "Failed to %s '%s'!", (level == RETURN_ERROR) ? "load config from" : "find config file",
                       filename);
    }
    return((BOOL) (level == RETURN_OK));
}
/* \\\ */

/* /// "psdSaveCfgToDisk()" */
BOOL (psdSaveCfgToDisk)(STRPTR filename asm("a1"), struct PsdBase * ps asm("a6"))
{
    ULONG *buf;
    BOOL saved = FALSE;
    BPTR filehandle;

    if(!filename) {
        saved = psdSaveCfgToDisk(NEP_PREFS_ENVARC);
        saved &= psdSaveCfgToDisk(NEP_PREFS_ENV);
        return(saved);
    }

    if(!pOpenDOS(ps)) {
        return(FALSE);
    }
    pLockSemShared(ps, &ps->ps_ConfigLock);

    buf = (ULONG *) psdWriteCfg(NULL);
    if(buf) {
        /* Write file */
        filehandle = Open(filename, MODE_NEWFILE);
        if(filehandle) {
            Write(filehandle, buf, (AROS_LONG2BE(buf[1])+9) & ~1UL);
            Close(filehandle);
            saved = TRUE;
        } else {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Failed to write config to '%s'!",
                           filename);
        }
        psdFreeVec(buf);
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    if(saved) {
        ps->ps_SavedConfigHash = ps->ps_ConfigHash;
    }
    return(saved);
}
/* \\\ */

/* /// "psdWriteCfg()" */
APTR (psdWriteCfg)(struct PsdIFFContext * pic asm("a0"), struct PsdBase * ps asm("a6"))
{
    ULONG len;
    APTR buf = NULL;

    KPRINTF(10, ("psdWriteCfg(0x%08lx)\n", pic));

    pLockSemShared(ps, &ps->ps_ConfigLock);
    if(!pic) {
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
        if(!(pic->pic_Node.ln_Succ)) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return(NULL);
        }
    }
    pUpdateGlobalCfg(ps, pic);
    ps->ps_CheckConfigReq = TRUE;
    len = pGetFormLength(pic);
    if((buf = psdAllocVec(len))) {
        pInternalWriteForm(pic, buf);
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(buf);
}
/* \\\ */

/* /// "psdFindCfgForm()" */
struct PsdIFFContext * (psdFindCfgForm)(struct PsdIFFContext * pic asm("a0"), ULONG formid asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *subpic;

    KPRINTF(1, ("psdFindCfgForm(0x%08lx, 0x%08lx)\n", pic, formid));
    pLockSemShared(ps, &ps->ps_ConfigLock);
    if(!pic) {
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
        if(!(pic->pic_Node.ln_Succ)) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return(NULL);
        }
    }
    subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    while(subpic->pic_Node.ln_Succ) {
        if(subpic->pic_FormID == formid) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return(subpic);
        }
        subpic = (struct PsdIFFContext *) subpic->pic_Node.ln_Succ;
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(NULL);
}
/* \\\ */

/* /// "psdNextCfgForm()" */
struct PsdIFFContext * (psdNextCfgForm)(struct PsdIFFContext * pic asm("a0"), struct PsdBase * ps asm("a6"))
{
    ULONG formid;
    KPRINTF(1, ("psdNextCfgForm(0x%08lx)\n", pic));

    if(!pic) {
        return(NULL);
    }
    pLockSemShared(ps, &ps->ps_ConfigLock);
    formid = pic->pic_FormID;
    pic = (struct PsdIFFContext *) pic->pic_Node.ln_Succ;
    while(pic->pic_Node.ln_Succ) {
        if(pic->pic_FormID == formid) {
            pUnlockSem(ps, &ps->ps_ConfigLock);

            KPRINTF(1, ("Found context 0x%08lx\n", pic));
            return(pic);
        }
        pic = (struct PsdIFFContext *) pic->pic_Node.ln_Succ;
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(NULL);
}
/* \\\ */

/* /// "psdAllocCfgForm()" */
struct PsdIFFContext * (psdAllocCfgForm)(ULONG formid asm("d0"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;
    KPRINTF(10, ("psdAllocCfgForm(0x%08lx)\n", formid));
    if((pic = psdAllocVec(sizeof(struct PsdIFFContext)))) {
        NewList(&pic->pic_SubForms);
        //pic->pic_Parent = parent;
        pic->pic_FormID = formid;
        pic->pic_FormLength = 4;
        pic->pic_Chunks = NULL;
        pic->pic_ChunksLen = 0;
        pic->pic_BufferLen = 0;
        Forbid();
        AddTail(&ps->ps_AlienConfigs, &pic->pic_Node);
        Permit();
    }
    return(pic);
}
/* \\\ */

/* /// "psdRemCfgForm()" */
/* The sfd declares BOOL: always TRUE, so a caller testing D0 reads a defined
   value */
BOOL (psdRemCfgForm)(struct PsdIFFContext * pic asm("a0"), struct PsdBase * ps asm("a6"))
{
    KPRINTF(10, ("psdRemCfgForm(0x%08lx)\n", pic));

    pLockSemExcl(ps, &ps->ps_ConfigLock);
    if(!pic) {
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
        if(!(pic->pic_Node.ln_Succ)) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return TRUE;
        }
    }
    pFreeForm(ps, pic);
    pUnlockSem(ps, &ps->ps_ConfigLock);
    ps->ps_CheckConfigReq = TRUE;
    return TRUE;
}
/* \\\ */

/* /// "psdAddCfgEntry()" */
struct PsdIFFContext * (psdAddCfgEntry)(struct PsdIFFContext * pic asm("a0"), APTR formdata asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *res;

    KPRINTF(10, ("psdAddCfgEntry(0x%08lx, 0x%08lx)\n", pic, formdata));
    pLockSemExcl(ps, &ps->ps_ConfigLock);
    if(!pic) {
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
        if(!(pic->pic_Node.ln_Succ)) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return(NULL);
        }
    }
    res = pAddCfgChunk(ps, pic, formdata);
    pUnlockSem(ps, &ps->ps_ConfigLock);
    pMigrateCfg(ps);    /* an imported entry may come from Poseidon prefs */
    ps->ps_CheckConfigReq = TRUE;
    return(res);
}
/* \\\ */

/* /// "psdRemCfgChunk()" */
BOOL (psdRemCfgChunk)(struct PsdIFFContext * pic asm("a0"), ULONG chnkid asm("d0"), struct PsdBase * ps asm("a6"))
{
    BOOL res = FALSE;

    KPRINTF(10, ("psdRemCfgChunk(0x%08lx, 0x%08lx)\n", pic, chnkid));
    pLockSemExcl(ps, &ps->ps_ConfigLock);
    if(!pic) {
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
        if(!(pic->pic_Node.ln_Succ)) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return(FALSE);
        }
    }
    if(chnkid) {
        res = pRemCfgChunk(ps, pic, chnkid);
    } else {
        struct PsdIFFContext *subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
        while(subpic->pic_Node.ln_Succ) {
            pFreeForm(ps, subpic);
            res = TRUE;
            subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
        }
        if(pic->pic_ChunksLen) {
            res = TRUE;
        }
        pic->pic_ChunksLen = 0;
        pic->pic_FormLength = 4;
    }

    pUnlockSem(ps, &ps->ps_ConfigLock);
    ps->ps_CheckConfigReq = TRUE;
    return(res);
}
/* \\\ */

/* /// "psdGetCfgChunk()" */
APTR (psdGetCfgChunk)(struct PsdIFFContext * pic asm("a0"), ULONG chnkid asm("d0"), struct PsdBase * ps asm("a6"))
{
    ULONG *chnk;
    ULONG *res = NULL;

    KPRINTF(1, ("psdGetCfgChunk(0x%08lx, 0x%08lx)\n", pic, chnkid));

    pLockSemShared(ps, &ps->ps_ConfigLock);
    if(!pic) {
        pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
        if(!(pic->pic_Node.ln_Succ)) {
            pUnlockSem(ps, &ps->ps_ConfigLock);
            return(NULL);
        }
    }
    pUpdateGlobalCfg(ps, pic);
    chnk = pFindCfgChunk(ps, pic, chnkid);
    if(chnk) {
        res = psdAllocVec(AROS_LONG2BE(chnk[1])+8);
        if(res) {
            memcpy(res, chnk, AROS_LONG2BE(chnk[1])+8);
        }
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(res);
}
/* \\\ */

/* /// "psdParseCfg()" */
void (psdParseCfg)(struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;
    struct PsdIFFContext *subpic;
    ULONG *chnk;
    STRPTR name;
    ULONG unit;
    struct PsdHardware *phw;
    struct PsdUsbClass *puc;
    BOOL removeall = TRUE;
    BOOL nodos = (FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS);

    XPRINTF(10, ("psdParseCfg()\n"));

    pLockSemShared(ps, &ps->ps_ConfigLock);
    pCheckCfgChanged(ps);
    pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    if(!pic) {
        pUnlockSem(ps, &ps->ps_ConfigLock);
        return;
    }

    // if no config for hardware is found, we don't remove the devices,
    // because this could render the system useless (no USB mice or
    // keyboards to configure the hardware!)
    if(!psdFindCfgForm(pic, IFFFORM_UHWDEVICE)) {
        XPRINTF(10, ("No hardware data present\n"));
        removeall = FALSE;
    }

    // the same goes for classes. A config without a class list says nothing
    // about which classes to run: either no prefs were ever saved, or they were
    // saved from a class settings window, which writes no stack lists. Removing
    // every class on that evidence would take the stack down.
    BOOL removeclasses = (psdFindCfgForm(pic, IFFFORM_USBCLASS) != NULL);

    psdLockReadPBase();

    /* select all hardware devices for removal */
    ForeachNode(&ps->ps_Hardware, phw) {
        phw->phw_RemoveMe = removeall;
    }

    /* select all classes for removal */
    ForeachNode(&ps->ps_Classes, puc) {
        /*
         * For kickstart-resident classes we check usage count, and
         * remove them only if it's zero.
         * These classes can be responsible for devices which we can use
         * at boot time. If we happen to remove them, we can end up with
         * no input or storage devices at all.
         */
        if (!removeclasses)
            puc->puc_RemoveMe = FALSE;
        else if (FindResident(puc->puc_ClassName))
            puc->puc_RemoveMe = (puc->puc_UseCnt == 0);
        else
            puc->puc_RemoveMe = TRUE;
    }

    psdUnlockPBase();

    /* Get Hardware config */
    subpic = psdFindCfgForm(pic, IFFFORM_UHWDEVICE);
    while(subpic) {
        chnk = pFindCfgChunk(ps, subpic, IFFCHNK_NAME);
        if(chnk) {
            name = (STRPTR) &chnk[2];
            unit = 0;
            chnk = pFindCfgChunk(ps, subpic, IFFCHNK_UNIT);
            if(chnk) {
                unit = chnk[2];
            }
            if(!pFindCfgChunk(ps, subpic, IFFCHNK_OFFLINE)) {
                phw = pFindHardware(ps, name, unit);
                XPRINTF(5, ("Have configuration for device 0x%08lx (%s unit %lu)\n", phw, name, unit));
                if(phw) {
                    phw->phw_RemoveMe = FALSE;
                }
            }
        }
        subpic = psdNextCfgForm(subpic);
    }

    /* Get Class config */
    subpic = psdFindCfgForm(pic, IFFFORM_USBCLASS);
    while(subpic) {
        chnk = pFindCfgChunk(ps, subpic, IFFCHNK_NAME);
        if(chnk) {
            name = (STRPTR) &chnk[2];
            puc = (struct PsdUsbClass *) pFindName(ps, &ps->ps_Classes, name);
            XPRINTF(5, ("Have configuration for class 0x%08lx (%s)\n", puc, name));
            if(puc) {
                puc->puc_RemoveMe = FALSE;
            }
        }
        subpic = psdNextCfgForm(subpic);
    }

    // unlock config while removing to avoid deadlocks.
    pUnlockSem(ps, &ps->ps_ConfigLock);

    struct Node *nodetmp;
    /* now remove remaining classes not found in the config */
    ForeachNodeSafe(&ps->ps_Classes, puc, nodetmp) {
        if(puc->puc_RemoveMe) {
            XPRINTF(5, ("Removing class %s\n", puc->puc_ClassName));
            psdRemClass(puc);
        }
    }

    /* now remove all remaining hardware not found in the config */
    ForeachNodeSafe(&ps->ps_Hardware, phw, nodetmp) {
        if(phw->phw_RemoveMe) {
            XPRINTF(5, ("Removing device %s unit %lu\n", phw->phw_DevName, phw->phw_Unit));
            psdRemHardware(phw);
        }
    }

    pLockSemShared(ps, &ps->ps_ConfigLock);
    pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    if(!pic) {
        pUnlockSem(ps, &ps->ps_ConfigLock);
        // oops!
        return;
    }

    /* Add missing Classes */
    subpic = psdFindCfgForm(pic, IFFFORM_USBCLASS);
    while(subpic) {
        chnk = pFindCfgChunk(ps, subpic, IFFCHNK_NAME);
        if(chnk) {
            /* *** FIXME *** POSSIBLE DEADLOCK WHEN CLASS TRIES TO DO CONFIG STUFF IN
               AN EXTERNAL TASK INSIDE LIBOPEN CODE */
            name = (STRPTR) &chnk[2];
            puc = (struct PsdUsbClass *) pFindName(ps, &ps->ps_Classes, name);
            if(!puc) {
                psdAddClass(name, 0);
            }
        }
        subpic = psdNextCfgForm(subpic);
    }

    /* Now really mount Hardware found in config */
    subpic = psdFindCfgForm(pic, IFFFORM_UHWDEVICE);
    while(subpic) {
        chnk = pFindCfgChunk(ps, subpic, IFFCHNK_NAME);
        if(chnk) {
            name = (STRPTR) &chnk[2];
            unit = 0;
            chnk = pFindCfgChunk(ps, subpic, IFFCHNK_UNIT);
            if(chnk) {
                unit = chnk[2];
            }
            if(!pFindCfgChunk(ps, subpic, IFFCHNK_OFFLINE)) {
                phw = pFindHardware(ps, name, unit);
                if(!phw) {
                    phw = psdAddHardware(name, unit);
                    if(phw) {
                        if(psdEnumerateHardware(phw) == NULL) {
                            psdRemHardware(phw);
                        }
                    }
                }
            }
        }
        subpic = psdNextCfgForm(subpic);
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);

    /* Devices claimed by the boot classes before DOS was available are handed
       over to the full classes by psdClassScan() below. */

    if(nodos && (!ps->ps_ConfigRead)) {
        // it's the first time we were reading the config and DOS was not available
        ps->ps_StartedAsTask = TRUE;
    }
    ps->ps_ConfigRead = TRUE;
    ps->ps_SavedConfigHash = ps->ps_ConfigHash; // update saved hash

    /* do a class scan */
    psdClassScan();

    if(nodos && ps->ps_GlobalCfg->pgc_BootDelay) {
        // wait for hubs to settle
        psdDelayMS(1000);
        puc = (struct PsdUsbClass *) FindName(&ps->ps_Classes, "massstorage.class");
        if(puc && puc->puc_UseCnt) {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Delaying further execution by %ld second(s) (boot delay).",
                           ps->ps_GlobalCfg->pgc_BootDelay);
            if(ps->ps_GlobalCfg->pgc_BootDelay >= 1) {
                psdDelayMS((ps->ps_GlobalCfg->pgc_BootDelay-1)*1000);
            }
        } else {
            psdAddErrorMsg0(RETURN_OK, (STRPTR) libname, "Boot delay skipped, no mass storage devices found.");
        }
    }
}
/* \\\ */

/* /// "psdSetClsCfg()" */
BOOL (psdSetClsCfg)(STRPTR owner asm("a0"), APTR form asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;
    BOOL result = FALSE;

    KPRINTF(10, ("psdSetClsCfg(%s, 0x%08lx)\n", owner, form));
    pLockSemExcl(ps, &ps->ps_ConfigLock);
    pic = psdFindCfgForm(NULL, IFFFORM_CLASSCFG);
    while(pic) {
        if(pMatchStringChunk(ps, pic, IFFCHNK_OWNER, owner)) {
            pic = psdFindCfgForm(pic, IFFFORM_CLASSDATA);
            if(pic) {
                if(form) {
                    result = psdReadCfg(pic, form);
                } else {
                    psdRemCfgChunk(pic, 0);
                    result = TRUE;
                }
                break;
            } else {
                break;
            }
        }
        pic = psdNextCfgForm(pic);
    }
    if(result) {
        pUnlockSem(ps, &ps->ps_ConfigLock);
        pCheckCfgChanged(ps);
        return(result);
    }
    pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
    if(pic->pic_Node.ln_Succ) {
        pic = pAllocForm(ps, pic, IFFFORM_CLASSCFG);
        if(pic) {
            if(pAddStringChunk(ps, pic, IFFCHNK_OWNER, owner)) {
                if(form) {
                    if(pAddCfgChunk(ps, pic, form)) {
                        pUnlockSem(ps, &ps->ps_ConfigLock);
                        pCheckCfgChanged(ps);
                        return(TRUE);
                    }
                } else {
                    ULONG buf[3];
                    buf[0] = AROS_LONG2BE(ID_FORM);
                    buf[1] = AROS_LONG2BE(4);
                    buf[2] = AROS_LONG2BE(IFFFORM_CLASSDATA);
                    if(pAddCfgChunk(ps, pic, buf)) {
                        pUnlockSem(ps, &ps->ps_ConfigLock);
                        pCheckCfgChanged(ps);
                        return(TRUE);
                    }
                }
            }
        }
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    pCheckCfgChanged(ps);
    return(FALSE);
}
/* \\\ */

/* /// "psdGetClsCfg()" */
struct PsdIFFContext * (psdGetClsCfg)(STRPTR owner asm("a0"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;

    KPRINTF(10, ("psdGetClsCfg(%s)\n", owner));
    pic = psdFindCfgForm(NULL, IFFFORM_CLASSCFG);
    while(pic) {
        if(pMatchStringChunk(ps, pic, IFFCHNK_OWNER, owner)) {
            return(psdFindCfgForm(pic, IFFFORM_CLASSDATA));
        }
        pic = psdNextCfgForm(pic);
    }
    return(NULL);
}
/* \\\ */

/* /// "psdSetUsbDevCfg()" */
BOOL (psdSetUsbDevCfg)(STRPTR owner asm("a0"), STRPTR devid asm("a2"), STRPTR ifid asm("a3"), APTR form asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;
    struct PsdIFFContext *cpic = NULL;
    struct PsdIFFContext *mpic = NULL;
    BOOL result = FALSE;

    KPRINTF(10, ("psdSetUsbDevCfg(%s, %s, %s, 0x%08lx)\n", owner, devid, ifid, form));
    pLockSemExcl(ps, &ps->ps_ConfigLock);
    /* Find device config form. It contains all device config data */
    pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
    while(pic) {
        /* Find DEVID-Chunk. Check if it matches our device id */
        if(pMatchStringChunk(ps, pic, IFFCHNK_DEVID, devid)) {
            cpic = NULL;
            /* We found the correct device. Now if we need to store interface data, find the interface first */
            if(ifid) {
                /* Search interface config form */
                mpic = psdFindCfgForm(pic, IFFFORM_IFCFGDATA);
                while(mpic) {
                    /* Found the form. Find the the ID String for the interface */
                    if(pMatchStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                        /* ID did match, now check for owner */
                        if(pMatchStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            /* found it! So there is already a config saved in there. Search for dev config data form */
                            cpic = psdFindCfgForm(mpic, IFFFORM_IFCLSDATA);
                            if(!cpic) {
                                /* not found, generate it */
                                cpic = pAllocForm(ps, mpic, IFFFORM_IFCLSDATA);
                            }
                            break;
                        }
                    }
                    mpic = psdNextCfgForm(mpic);
                }
                if(!cpic) {
                    if((mpic = pAllocForm(ps, pic, IFFFORM_IFCFGDATA))) {
                        if(pAddStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            if(pAddStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                                cpic = pAllocForm(ps, mpic, IFFFORM_IFCLSDATA);
                            }
                        }
                    }
                }
            } else {
                /* Search for device config */
                mpic = psdFindCfgForm(pic, IFFFORM_DEVCFGDATA);
                while(mpic) {
                    /* search for the right owner */
                    if(pMatchStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                        /* found it! So there is already a config saved in there. Search for dev config data form */
                        cpic = psdFindCfgForm(mpic, IFFFORM_DEVCLSDATA);
                        if(!cpic) {
                            /* not found, generate it */
                            cpic = pAllocForm(ps, mpic, IFFFORM_DEVCLSDATA);
                        }
                        break;
                    }
                    mpic = psdNextCfgForm(mpic);
                }
                if(!cpic) { /* no device config form */
                    if((mpic = pAllocForm(ps, pic, IFFFORM_DEVCFGDATA))) {
                        if(pAddStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            cpic = pAllocForm(ps, mpic, IFFFORM_DEVCLSDATA);
                        }
                    }
                }
            }
            if(cpic) {
                if(form) {
                    result = psdReadCfg(cpic, form);
                } else {
                    psdRemCfgChunk(cpic, 0);
                    result = TRUE;
                }
                break;
            }
        }
        pic = psdNextCfgForm(pic);
    }
    if(result) {
        pUnlockSem(ps, &ps->ps_ConfigLock);
        pCheckCfgChanged(ps);
        return(result);
    }
    cpic = NULL;
    pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
    if(pic->pic_Node.ln_Succ) {
        pic = pAllocForm(ps, pic, IFFFORM_DEVICECFG);
        if(pic) {
            if(pAddStringChunk(ps, pic, IFFCHNK_DEVID, devid)) {
                if(ifid) {
                    if((mpic = pAllocForm(ps, pic, IFFFORM_IFCFGDATA))) {
                        if(pAddStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            if(pAddStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                                cpic = pAllocForm(ps, mpic, IFFFORM_IFCLSDATA);
                            }
                        }
                    }
                } else {
                    if((mpic = pAllocForm(ps, pic, IFFFORM_DEVCFGDATA))) {
                        if(pAddStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            cpic = pAllocForm(ps, mpic, IFFFORM_DEVCLSDATA);
                        }
                    }
                }
                if(cpic) {
                    if(form) {
                        result = psdReadCfg(cpic, form);
                    } else {
                        psdRemCfgChunk(cpic, 0);
                        result = TRUE;
                    }
                }
            }
        }
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    pCheckCfgChanged(ps);
    return(result);
}
/* \\\ */

/* /// "psdGetUsbDevCfg()" */
struct PsdIFFContext * (psdGetUsbDevCfg)(STRPTR owner asm("a0"), STRPTR devid asm("a2"), STRPTR ifid asm("a3"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;
    struct PsdIFFContext *cpic = NULL;
    struct PsdIFFContext *mpic = NULL;

    KPRINTF(10, ("psdGetUsbDevCfg(%s, %s, %s)\n", owner, devid, ifid));
    pLockSemShared(ps, &ps->ps_ConfigLock);
    /* Find device config form. It contains all device config data */
    pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
    while(pic) {
        /* Find DEVID-Chunk. Check if it matches our device id */
        if(pMatchStringChunk(ps, pic, IFFCHNK_DEVID, devid)) {
            cpic = NULL;
            /* We found the correct device. Now if we need to store interface data, find the interface first */
            if(ifid) {
                /* Search interface config form */
                mpic = psdFindCfgForm(pic, IFFFORM_IFCFGDATA);
                while(mpic) {
                    /* Found the form. Find the the ID String for the interface */
                    if(pMatchStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                        /* ID did match, now check for owner */
                        if(pMatchStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            /* found it! So there is already a config saved in there. Search for dev config data form */
                            cpic = psdFindCfgForm(mpic, IFFFORM_IFCLSDATA);
                            break;
                        }
                    }
                    mpic = psdNextCfgForm(mpic);
                }
            } else {
                /* Search for device config */
                mpic = psdFindCfgForm(pic, IFFFORM_DEVCFGDATA);
                while(mpic) {
                    /* search for the right owner */
                    if(pMatchStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                        /* found it! So there is already a config saved in there. Search for dev config data form */
                        cpic = psdFindCfgForm(mpic, IFFFORM_DEVCLSDATA);
                        break;
                    }
                    mpic = psdNextCfgForm(mpic);
                }
            }
            break;
        }
        pic = psdNextCfgForm(pic);
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    KPRINTF(1, ("Result 0x%08lx\n", cpic));
    return(cpic);
}
/* \\\ */

/* /// "psdSetForcedBinding()" */
BOOL (psdSetForcedBinding)(STRPTR owner asm("a2"), STRPTR devid asm("a0"), STRPTR ifid asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;
    struct PsdIFFContext *mpic = NULL;
    ULONG olen = 0;
    BOOL result = FALSE;

    if(owner) {
        olen = strlen(owner);
    }
    pLockSemExcl(ps, &ps->ps_ConfigLock);
    /* Find device config form. It contains all device config data */
    pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
    while(pic) {
        /* Find DEVID-Chunk. Check if it matches our device id */
        if(pMatchStringChunk(ps, pic, IFFCHNK_DEVID, devid)) {
            /* We found the correct device. Now if we need to store interface data, find the interface first */
            if(ifid) {
                /* Search interface config form */
                mpic = psdFindCfgForm(pic, IFFFORM_IFCFGDATA);
                while(mpic) {
                    /* Found the form. Find the the ID String for the interface */
                    if(pMatchStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                        /* ID did match, insert/replace forced binding */
                        if(olen) {
                            if(pAddStringChunk(ps, mpic, IFFCHNK_FORCEDBIND, owner)) {
                                result = TRUE;
                            }
                        } else {
                            pRemCfgChunk(ps, mpic, IFFCHNK_FORCEDBIND);
                            result = TRUE;
                        }
                    }
                    mpic = psdNextCfgForm(mpic);
                }
                if(!olen) {
                    result = TRUE;
                }
                if((!result) && olen) {
                    if((mpic = pAllocForm(ps, pic, IFFFORM_IFCFGDATA))) {
                        if(pAddStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            if(pAddStringChunk(ps, mpic, IFFCHNK_FORCEDBIND, owner)) {
                                if(pAddStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                                    result = TRUE;
                                }
                            }
                        }
                    }
                }
            } else {
                /* Add FBND chunk */
                if(olen) {
                    if(pAddStringChunk(ps, pic, IFFCHNK_FORCEDBIND, owner)) {
                        result = TRUE;
                    }
                } else {
                    pRemCfgChunk(ps, pic, IFFCHNK_FORCEDBIND);
                    result = TRUE;
                }
            }
            break;
        }
        pic = psdNextCfgForm(pic);
    }
    if(!olen) {
        result = TRUE;
    }
    if(result) {
        pUnlockSem(ps, &ps->ps_ConfigLock);
        pCheckCfgChanged(ps);
        return(result);
    }
    pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
    if(pic->pic_Node.ln_Succ) {
        pic = pAllocForm(ps, pic, IFFFORM_DEVICECFG);
        if(pic) {
            if(pAddStringChunk(ps, pic, IFFCHNK_DEVID, devid)) {
                if(ifid) {
                    if((mpic = pAllocForm(ps, pic, IFFFORM_IFCFGDATA))) {
                        if(pAddStringChunk(ps, mpic, IFFCHNK_OWNER, owner)) {
                            if(pAddStringChunk(ps, mpic, IFFCHNK_FORCEDBIND, owner)) {
                                if(pAddStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                                    result = TRUE;
                                }
                            }
                        }
                    }
                } else {
                    /* Add FBND chunk */
                    if(pAddStringChunk(ps, pic, IFFCHNK_FORCEDBIND, owner)) {
                        result = TRUE;
                    }
                }
            }
        }
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    pCheckCfgChanged(ps);
    return(result);
}
/* \\\ */

/* /// "psdGetForcedBinding()" */
STRPTR (psdGetForcedBinding)(STRPTR devid asm("a0"), STRPTR ifid asm("a1"), struct PsdBase * ps asm("a6"))
{
    struct PsdIFFContext *pic;
    struct PsdIFFContext *mpic = NULL;
    ULONG *chunk;
    STRPTR owner = NULL;

    pLockSemShared(ps, &ps->ps_ConfigLock);
    /* Find device config form. It contains all device config data */
    pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
    while(pic) {
        /* Find DEVID-Chunk. Check if it matches our device id */
        if(pMatchStringChunk(ps, pic, IFFCHNK_DEVID, devid)) {
            /* We found the correct device. Now if we need to store interface data, find the interface first */
            if(ifid) {
                /* Search interface config form */
                mpic = psdFindCfgForm(pic, IFFFORM_IFCFGDATA);
                while(mpic) {
                    /* Found the form. Find the the ID String for the interface */
                    if(pMatchStringChunk(ps, mpic, IFFCHNK_IFID, ifid)) {
                        /* ID did match, now check for forced binding */
                        chunk = pFindCfgChunk(ps, mpic, IFFCHNK_FORCEDBIND);
                        if(chunk) {
                            owner = (STRPTR) &chunk[2];
                            break;
                        }
                    }
                    mpic = psdNextCfgForm(mpic);
                }
            } else {
                /* Search for device forced binding */
                chunk = pFindCfgChunk(ps, pic, IFFCHNK_FORCEDBIND);
                if(chunk) {
                    owner = (STRPTR) &chunk[2];
                    break;
                }
            }
            break;
        }
        pic = psdNextCfgForm(pic);
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(owner);
}
/* \\\ */

/* /// "psdAddStringChunk()" */
BOOL (psdAddStringChunk)(struct PsdIFFContext * pic asm("a0"), ULONG chunkid asm("d0"), CONST_STRPTR str asm("a1"), struct PsdBase * ps asm("a6"))
{
    BOOL res;
    KPRINTF(10, ("psdAddStringChunk(0x%08lx, 0x%08lx, %s)\n", pic, chunkid, str));
    pLockSemExcl(ps, &ps->ps_ConfigLock);
    res = pAddStringChunk(ps, pic, chunkid, str);
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(res);
}
/* \\\ */

/* /// "psdMatchStringChunk()" */
BOOL (psdMatchStringChunk)(struct PsdIFFContext * pic asm("a0"), ULONG chunkid asm("d0"), CONST_STRPTR str asm("a1"), struct PsdBase * ps asm("a6"))
{
    BOOL res;
    KPRINTF(10, ("psdMatchStringChunk(0x%08lx, 0x%08lx, %s)\n", pic, chunkid, str));
    pLockSemShared(ps, &ps->ps_ConfigLock);
    res = pMatchStringChunk(ps, pic, chunkid, str);
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(res);
}
/* \\\ */

/* /// "psdGetStringChunk()" */
STRPTR (psdGetStringChunk)(struct PsdIFFContext * pic asm("a0"), ULONG chunkid asm("d0"), struct PsdBase * ps asm("a6"))
{
    STRPTR str;
    KPRINTF(1, ("psdGetStringChunk(0x%08lx, 0x%08lx)\n", pic, chunkid));
    pLockSemShared(ps, &ps->ps_ConfigLock);
    str = pGetStringChunk(ps, pic, chunkid);
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(str);
}
/* \\\ */

/* *** (non-library subroutines) *** */

/* *** USB3 support functions *** */

/* Find the HS hub owning the TT for a FS/LS device (the first high-speed hub
 * walking upwards); *ttPort gets the TT hub port its FS/LS subtree hangs off.
 * Returns NULL when no TT is involved (HS/SS devices, FS/LS on a FS root).
 * Only FS/LS devices behind HS hubs need a TT; PDFF_NEEDSSPLIT already encodes
 * this in Neptune's logic, but this helper intentionally does not rely on
 * that flag. */
struct PsdDevice * pFindTTHub(struct PsdDevice *pd, UWORD *ttPort)
{
    struct PsdDevice *dev = pd;
    struct PsdDevice *hub = pd ? pd->pd_Hub : NULL;

    while (dev && hub) {
        if (hub->pd_Flags & PDFF_HIGHSPEED) {
            /* 'hub' is the HS hub owning the TT, 'dev' is the child of that hub */
            if (ttPort) *ttPort = dev->pd_HubPort;
            return hub;
        }

        dev = hub;
        hub = hub->pd_Hub;
    }
    return NULL;
}

void pGetTTInfo(struct PsdDevice *pd,
                UWORD *ttHubAddr,
                UWORD *ttHubPort,
                UWORD *thinkTime,
                BOOL  *isMultiTT)
{
    UWORD ttport = 0;
    struct PsdDevice *hub = pFindTTHub(pd, &ttport);

    if (ttHubAddr) *ttHubAddr = hub ? hub->pd_DevAddr : 0;
    if (ttHubPort) *ttHubPort = hub ? ttport : 0;
    if (thinkTime) *thinkTime = hub ? hub->pd_HubThinkTime : 0;
    if (isMultiTT) *isMultiTT = (hub && (hub->pd_Flags & PDFF_MULTITT)) ? TRUE : FALSE;
}

/* *** Configuration *** */

/* /// "pAllocForm()" */
struct PsdIFFContext * pAllocForm(struct PsdBase * ps, struct PsdIFFContext *parent, ULONG formid)
{
    struct PsdIFFContext *pic;
    KPRINTF(1, ("pAllocForm(0x%08lx, 0x%08lx)\n", parent, formid));
    if((pic = psdAllocVec(sizeof(struct PsdIFFContext)))) {
        NewList(&pic->pic_SubForms);
        //pic->pic_Parent = parent;
        pic->pic_FormID = formid;
        pic->pic_FormLength = 4;
        pic->pic_Chunks = NULL;
        pic->pic_ChunksLen = 0;
        pic->pic_BufferLen = 0;
        Forbid();
        if(parent) {
            AddTail(&parent->pic_SubForms, &pic->pic_Node);
        } else {
            AddTail(&ps->ps_ConfigRoot, &pic->pic_Node);
        }
        Permit();
    }
    return(pic);
}
/* \\\ */

/* /// "pFreeForm()" */
void pFreeForm(struct PsdBase * ps, struct PsdIFFContext *pic)
{
    struct PsdIFFContext *subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    KPRINTF(10, ("pFreeForm(0x%08lx)\n", pic));
    Remove(&pic->pic_Node);
    while(subpic->pic_Node.ln_Succ) {
        pFreeForm(ps, subpic);
        subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    }
    psdFreeVec(pic->pic_Chunks);
    psdFreeVec(pic);
}
/* \\\ */

/* /// "pGetFormLength()" */
ULONG pGetFormLength(struct PsdIFFContext *pic)
{
    ULONG len = (5 + pic->pic_ChunksLen) & ~1UL;
    struct PsdIFFContext *subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    //KPRINTF(10, ("pGetFormLength(0x%08lx)\n", pic));
    while(subpic->pic_Node.ln_Succ) {
        len += pGetFormLength(subpic);
        subpic = (struct PsdIFFContext *) subpic->pic_Node.ln_Succ;
    }
    pic->pic_FormLength = len;
    //KPRINTF(10, ("FormLen=%ld\n", len+8));
    return(len + 8);
}
/* \\\ */

/* /// "pFindCfgChunk()" */
APTR pFindCfgChunk(struct PsdBase * ps, struct PsdIFFContext *pic, ULONG chnkid)
{
    ULONG *buf = pic->pic_Chunks;
    ULONG len = pic->pic_ChunksLen;
    ULONG chlen;
    KPRINTF(1, ("pFindCfgChunk(0x%08lx, 0x%08lx)\n", pic, chnkid));

    while(len) {
        if(AROS_LONG2BE(*buf) == chnkid) {
            KPRINTF(1, ("Found at 0x%08lx\n", buf));
            return(buf);
        }
        chlen = (AROS_LONG2BE(buf[1]) + 9) & ~1UL;
        len -= chlen;
        buf = (ULONG *) (((UBYTE *) buf) + chlen);
    }
    KPRINTF(1, ("Not found!\n"));
    return(NULL);
}
/* \\\ */

/* /// "pRemCfgChunk()" */
BOOL pRemCfgChunk(struct PsdBase * ps, struct PsdIFFContext *pic, ULONG chnkid)
{
    ULONG *buf = pic->pic_Chunks;
    ULONG len = pic->pic_ChunksLen;
    ULONG chlen;
    KPRINTF(1, ("pRemCfgChunk(0x%08lx, 0x%08lx)\n", pic, chnkid));

    while(len) {
        chlen = ((AROS_LONG2BE(buf[1])) + 9) & ~1UL;
        if(AROS_LONG2BE(*buf) == chnkid) {
            len -= chlen;
            if(len) {
                memcpy(buf, &((UBYTE *) buf)[chlen], (size_t) len);
            }
            pic->pic_ChunksLen -= chlen;
            KPRINTF(10, ("Deleted %ld bytes to %ld chunk len\n", chlen, pic->pic_ChunksLen));
            return(TRUE);
        }
        len -= chlen;
        buf = (ULONG *) (((UBYTE *) buf) + chlen);
    }
    KPRINTF(1, ("Not found!\n"));
    return(FALSE);
}
/* \\\ */

/* /// "pAddCfgChunk()" */
struct PsdIFFContext * pAddCfgChunk(struct PsdBase * ps, struct PsdIFFContext *pic, APTR chunk)
{
    LONG len;
    LONG chlen;
    ULONG *buf = chunk;
    ULONG *newbuf;
    struct PsdIFFContext *subpic;
    KPRINTF(1, ("pAddCfgChunk(0x%08lx, 0x%08lx)\n", pic, chunk));
    if(AROS_LONG2BE(*buf) == ID_FORM) {
        buf++;
        len = ((AROS_LONG2BE(*buf)) - 3) & ~1UL;
        buf++;
        if((subpic = pAllocForm(ps, pic, AROS_LONG2BE(*buf)))) {
            buf++;
            while(len >= 8) {
                if(!(pAddCfgChunk(ps, subpic, buf))) {
                    break;
                }
                chlen = (AROS_LONG2BE(buf[1]) + 9) & ~1UL;
                len -= chlen;
                buf = (ULONG *) (((UBYTE *) buf) + chlen);
            }
            if(len) {
                psdAddErrorMsg0(RETURN_FAIL, (STRPTR) libname, "Corrupted FORM chunk; the configuration may be damaged.");
                return(NULL);
            }
        } else {
            return(NULL);
        }
        return(subpic);
    } else {
        pRemCfgChunk(ps, pic, AROS_LONG2BE(*buf));
        len = (AROS_LONG2BE(buf[1]) + 9) & ~1UL;
        if(pic->pic_ChunksLen+len > pic->pic_BufferLen) {
            KPRINTF(5, ("expanding buffer from %ld to %ld to fit %ld bytes\n", pic->pic_BufferLen, (pic->pic_ChunksLen+len)<<1, pic->pic_ChunksLen+len));

            /* Expand buffer */
            if((newbuf = psdAllocVec((pic->pic_ChunksLen+len)<<1))) {
                if(pic->pic_ChunksLen) {
                    memcpy(newbuf, pic->pic_Chunks, (size_t) pic->pic_ChunksLen);
                    psdFreeVec(pic->pic_Chunks);
                }
                pic->pic_Chunks = newbuf;
                pic->pic_BufferLen = (pic->pic_ChunksLen+len)<<1;
            } else {
                return(NULL);
            }
        }
        memcpy(&(((UBYTE *) pic->pic_Chunks)[pic->pic_ChunksLen]), chunk, (size_t) len);
        pic->pic_ChunksLen += len;
        return(pic);
    }
}
/* \\\ */

/* /// "pInternalWriteForm()" */
ULONG * pInternalWriteForm(struct PsdIFFContext *pic, ULONG *buf)
{
    struct PsdIFFContext *subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    //KPRINTF(10, ("pInternalWriteForm(0x%08lx, 0x%08lx)", pic, buf));
    *buf++ = AROS_LONG2BE(ID_FORM);
    *buf++ = AROS_LONG2BE(pic->pic_FormLength);
    *buf++ = AROS_LONG2BE(pic->pic_FormID);
    if(pic->pic_ChunksLen) {
        memcpy(buf, pic->pic_Chunks, (size_t) pic->pic_ChunksLen);
        buf = (ULONG *) (((UBYTE *) buf) + pic->pic_ChunksLen);
    }
    while(subpic->pic_Node.ln_Succ) {
        buf = pInternalWriteForm(subpic, buf);
        subpic = (struct PsdIFFContext *) subpic->pic_Node.ln_Succ;
    }
    return(buf);
}
/* \\\ */

/* /// "pCalcCfgCRC()" */
ULONG pCalcCfgCRC(struct PsdIFFContext *pic)
{
    struct PsdIFFContext *subpic = (struct PsdIFFContext *) pic->pic_SubForms.lh_Head;
    ULONG len;
    ULONG crc = pic->pic_FormID;
    UWORD *ptr;

    //KPRINTF(10, ("pInternalWriteForm(0x%08lx, 0x%08lx)", pic, buf));
    if(pic->pic_ChunksLen) {
        len = pic->pic_ChunksLen>>1;
        if(len) {
            ptr = (UWORD *) pic->pic_Chunks;
            do {
                crc = ((crc<<1)|(crc>>31))^(*ptr++);
            } while(--len);
        }
    }
    while(subpic->pic_Node.ln_Succ) {
        crc ^= pCalcCfgCRC(subpic);
        subpic = (struct PsdIFFContext *) subpic->pic_Node.ln_Succ;
    }
    return(crc);
}
/* \\\ */

/* /// "pCheckCfgChanged()" */
BOOL pCheckCfgChanged(struct PsdBase * ps)
{
    ULONG crc;
    struct PsdIFFContext *pic;
    struct PsdIFFContext *subpic;
    STRPTR tmpstr;

    pLockSemShared(ps, &ps->ps_ConfigLock);
    ps->ps_CheckConfigReq = FALSE;
    pic = (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head;
    if(!(pic->pic_Node.ln_Succ)) {
        pUnlockSem(ps, &ps->ps_ConfigLock);
        return(FALSE);
    }
    crc = pCalcCfgCRC(pic);
    if(crc != ps->ps_ConfigHash) {
        ULONG *chnk;
        ps->ps_ConfigHash = crc;
        /* Get Global config */
        if((subpic = psdFindCfgForm(pic, IFFFORM_STACKCFG))) {
            if((chnk = pFindCfgChunk(ps, subpic, IFFCHNK_GLOBALCFG))) {
                /* Loading prefs writes the live config behind psdSetAttrs()'s
                   back, so the link power policy has to be re-checked here too
                   - this is the "Load"/"Use" path in NeptuneUSB. */
                BOOL oldlinkpower = ps->ps_GlobalCfg->pgc_LinkPowerMgmt;
                CopyMem(&chnk[2], ((UBYTE *) ps->ps_GlobalCfg) + 8, min(AROS_LONG2BE(chnk[1]), AROS_LONG2BE(ps->ps_GlobalCfg->pgc_Length)));
                if(ps->ps_GlobalCfg->pgc_LinkPowerMgmt != oldlinkpower) {
                    ps->ps_LinkPowerReq = TRUE;
                }
            }
            if(!pMatchStringChunk(ps, subpic, IFFCHNK_INSERTSND, ps->ps_Notify.nt_InsertSndFile)) {
                if((tmpstr = pGetStringChunk(ps, subpic, IFFCHNK_INSERTSND))) {
                    psdFreeVec(ps->ps_Notify.nt_InsertSndFile);
                    ps->ps_Notify.nt_InsertSndFile = tmpstr;
                }
            }
            if(!pMatchStringChunk(ps, subpic, IFFCHNK_REMOVESND, ps->ps_Notify.nt_RemoveSndFile)) {
                if((tmpstr = pGetStringChunk(ps, subpic, IFFCHNK_REMOVESND))) {
                    psdFreeVec(ps->ps_Notify.nt_RemoveSndFile);
                    ps->ps_Notify.nt_RemoveSndFile = tmpstr;
                }
            }
        }
        pUnlockSem(ps, &ps->ps_ConfigLock);
        psdSendEvent(EHMB_CONFIGCHG, NULL, NULL);
        return(TRUE);
    }
    pUnlockSem(ps, &ps->ps_ConfigLock);
    return(FALSE);
}
/* \\\ */

/* /// "pAddStringChunk()" */
BOOL pAddStringChunk(struct PsdBase * ps, struct PsdIFFContext *pic, ULONG chunkid, CONST_STRPTR str)
{
    BOOL res = FALSE;
    ULONG len = strlen(str);
    ULONG *chnk = (ULONG *) psdAllocVec((ULONG) len+8+2);
    if(chnk) {
        chnk[0] = AROS_LONG2BE(chunkid);
        chnk[1] = AROS_LONG2BE(len+1);
        strcpy((STRPTR) &chnk[2], str);
        if(pAddCfgChunk(ps, pic, chnk)) {
            res = TRUE;
        }
        psdFreeVec(chnk);
    }
    return(res);
}
/* \\\ */

/* /// "pMatchStringChunk()" */
BOOL pMatchStringChunk(struct PsdBase * ps, struct PsdIFFContext *pic, ULONG chunkid, CONST_STRPTR str)
{
    ULONG *chunk;
    ULONG len;
    STRPTR srcptr;
    if((chunk = pFindCfgChunk(ps, pic, chunkid))) {
        srcptr = (STRPTR) &chunk[2];
        len = AROS_LONG2BE(chunk[1]);
        while(len-- && *srcptr) {
            if(*str++ != *srcptr++) {
                return(FALSE);
            }
        }
        if(!*str) {
            return(TRUE);
        }
    }
    return(FALSE);
}
/* \\\ */

/* /// "pGetStringChunk()" */
STRPTR pGetStringChunk(struct PsdBase * ps, struct PsdIFFContext *pic, ULONG chunkid)
{
    ULONG *chunk;
    STRPTR str;
    if((chunk = pFindCfgChunk(ps, pic, chunkid))) {
        if((str = (STRPTR) psdAllocVec(AROS_LONG2BE(chunk[1]) + 1))) {
            memcpy(str, &chunk[2], (size_t) AROS_LONG2BE(chunk[1]));
            return(str);
        }
    }
    return(NULL);
}
/* \\\ */

/* /// "pUpdateGlobalCfg()" */
void pUpdateGlobalCfg(struct PsdBase * ps, struct PsdIFFContext *pic)
{
    struct PsdIFFContext *tmppic;
    /* Set Global config */
    if(pic == (struct PsdIFFContext *) ps->ps_ConfigRoot.lh_Head) {
        if((tmppic = psdFindCfgForm(NULL, IFFFORM_STACKCFG))) {
            pAddCfgChunk(ps, tmppic, ps->ps_GlobalCfg);
            pAddStringChunk(ps, tmppic, IFFCHNK_INSERTSND, ps->ps_Notify.nt_InsertSndFile);
            pAddStringChunk(ps, tmppic, IFFCHNK_REMOVESND, ps->ps_Notify.nt_RemoveSndFile);
        }
    }
}
/* \\\ */

/* *** Misc (non library functions) ***/

/* /// "pGetDevConfig()" */
BOOL pGetDevConfig(struct PsdPipe *pp)
{
    struct PsdDevice *pd = pp->pp_Device;
    struct PsdBase * ps = pd->pd_Hardware->phw_Base;
    UBYTE *tempbuf;
    struct UsbStdCfgDesc uscd;
    ULONG len;
    LONG ioerr;
    STRPTR classname;
    UWORD curcfg = 0;

    KPRINTF(1, ("Getting configuration descriptor...\n"));
    psdLockWriteDevice(pd);
    while(curcfg < pd->pd_NumCfgs) {
        psdPipeSetup(pp, URTF_IN|URTF_STANDARD|URTF_DEVICE,
                     USR_GET_DESCRIPTOR, (UDT_CONFIGURATION<<8)|curcfg, 0);

        /*tempbuf = psdAllocVec(256);
        ioerr = psdDoPipe(pp, tempbuf, 34);
        if(ioerr == UHIOERR_RUNTPACKET)
        {
            ioerr = 0;
        }
        memcpy(&uscd, tempbuf, 9);*/
        ioerr = psdDoPipe(pp, &uscd, 9);//sizeof(struct UsbStdCfgDesc));
        if(!ioerr) {
            /* some firmware (Behringer UMC204HD) serves the same configuration for
               every index; a second copy would get bound twice by the class scan,
               on endpoints the HCD never configured */
            struct PsdConfig *parsedpc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            while(parsedpc->pc_Node.ln_Succ && (parsedpc->pc_CfgNum != uscd.bConfigurationValue)) {
                parsedpc = (struct PsdConfig *) parsedpc->pc_Node.ln_Succ;
            }
            if(parsedpc->pc_Node.ln_Succ) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "%s: configuration index %ld repeats configuration %ld, ignored.",
                               pd->pd_ProductStr ? pd->pd_ProductStr : (STRPTR) "new device",
                               (ULONG) curcfg, (ULONG) uscd.bConfigurationValue);
                curcfg++;
                continue;
            }
            KPRINTF(1, ("Config type: %ld\n", (ULONG) uscd.bDescriptorType));
            len = (ULONG) AROS_LE2WORD(uscd.wTotalLength);
            KPRINTF(1, ("Configsize %ld, total size %ld\n", (ULONG) uscd.bLength, len));
            if((tempbuf = psdAllocVec(len)))
                //if(1)
            {
                KPRINTF(1, ("Getting whole configuration descriptor...\n"));
                ioerr = psdDoPipe(pp, tempbuf, len);
                if(!ioerr) {
                    struct PsdConfig *pc = NULL;
                    struct PsdInterface *pif = NULL;
                    struct PsdInterface *altif = NULL;
                    struct PsdEndpoint *pep = NULL;
                    struct PsdDescriptor *pdd = NULL;
                    UBYTE *dbuf = tempbuf;
                    UBYTE *bufend;
                    ULONG dlen;
                    bufend = &dbuf[len];
                    while(dbuf < bufend) {
                        dlen = dbuf[0]; /* bLength */
                        if(dlen < 2) {
                            break;
                        }
                        if(&dbuf[dlen] > bufend) {
                            psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname, "End of descriptor past buffer!");
                        }
                        switch(dbuf[1]) { /* bDescriptorType */
                        case UDT_CONFIGURATION: {
                            struct UsbStdCfgDesc *usc = (struct UsbStdCfgDesc *) dbuf;
                            pif = NULL;
                            altif = NULL;
                            pep = NULL;
                            if((pc = pAllocConfig(pd))) {
                                pd->pd_Flags |= PDFF_CONFIGURED;
                                pc->pc_NumIfs = usc->bNumInterfaces;
                                pc->pc_CfgNum = usc->bConfigurationValue;
                                pc->pc_Attr   = usc->bmAttributes;
                                pc->pc_MaxPower = usc->bMaxPower<<1;
                                pc->pc_CfgStr = 0;
                                KPRINTF(1, ("  Config %ld\n", pc->pc_CfgNum));
                                if(usc->iConfiguration) {
                                    pc->pc_CfgStr = psdGetStringDescriptor(pp, usc->iConfiguration);
                                }
                                if(!pc->pc_CfgStr) {
                                    pc->pc_CfgStr = psdCopyStrFmt("Configuration %ld", pc->pc_CfgNum);
                                }
                            } else {
                                KPRINTF(20, ("  Config allocation failed\n"));
                            }
                            break;
                        }

                        case UDT_INTERFACE: {
                            struct UsbStdIfDesc *usif = (struct UsbStdIfDesc *) dbuf;
                            pep = NULL;
                            if(pc) {
                                if((altif = pAllocInterface(pc))) {
                                    altif->pif_IfNum = usif->bInterfaceNumber;
                                    altif->pif_Alternate = usif->bAlternateSetting;
                                    altif->pif_NumEPs = usif->bNumEndpoints;
                                    altif->pif_IfClass = usif->bInterfaceClass;
                                    altif->pif_IfSubClass = usif->bInterfaceSubClass;
                                    altif->pif_IfProto = usif->bInterfaceProtocol;
                                    KPRINTF(2, ("    Interface %ld\n", altif->pif_IfNum));
                                    if(usif->iInterface) {
                                        altif->pif_IfStr = psdGetStringDescriptor(pp, usif->iInterface);
                                    }
                                    if(!altif->pif_IfStr) {
                                        classname = psdNumToStr(NTS_CLASSCODE, (LONG) altif->pif_IfClass, NULL);
                                        if(classname) {
                                            altif->pif_IfStr = psdCopyStrFmt("%s interface (%ld)", classname, altif->pif_IfNum);
                                        } else {
                                            altif->pif_IfStr = psdCopyStrFmt("Interface %ld", altif->pif_IfNum);
                                        }
                                    }
                                    KPRINTF(2, ("      IfName    : %s\n"
                                                "      Alternate : %ld\n"
                                                "      NumEPs    : %ld\n"
                                                "      IfClass   : %ld\n"
                                                "      IfSubClass: %ld\n"
                                                "      IfProto   : %ld\n",
                                                altif->pif_IfStr, altif->pif_Alternate,
                                                altif->pif_NumEPs,
                                                altif->pif_IfClass,
                                                altif->pif_IfSubClass, altif->pif_IfProto));
                                    if(pc->pc_CfgNum == 1) {
                                        altif->pif_IDString = psdCopyStrFmt("%02lx-%02lx-%02lx-%02lx-%02lx",
                                                                            altif->pif_IfNum, altif->pif_Alternate,
                                                                            altif->pif_IfClass, altif->pif_IfSubClass,
                                                                            altif->pif_IfProto);
                                    } else {
                                        // for more than one config, add config number (retain backwards compatibility with most devices)
                                        altif->pif_IDString = psdCopyStrFmt("%02lx-%02lx-%02lx-%02lx-%02lx-%02lx",
                                                                            pc->pc_CfgNum,
                                                                            altif->pif_IfNum, altif->pif_Alternate,
                                                                            altif->pif_IfClass, altif->pif_IfSubClass,
                                                                            altif->pif_IfProto);
                                    }

                                    /* Move the interface to the alternatives if possible */
                                    if(altif->pif_Alternate) {
                                        if(!pif) {
                                            psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname, "Alternate interface without prior main interface!");
                                            KPRINTF(20, ("    Alternate interface without prior main interface\n"));
                                            pif = altif;
                                        } else {
                                            Remove(&altif->pif_Node);
                                            AddTail(&pif->pif_AlterIfs, &altif->pif_Node);
                                            altif->pif_ParentIf = pif;
                                        }
                                    } else {
                                        altif->pif_ParentIf = NULL;
                                        pif = altif;
                                    }
                                } else {
                                    KPRINTF(20, ("    Interface allocation failed\n"));
                                }
                            } else {
                                psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname, "Interface without prior config descriptor!");
                                KPRINTF(20, ("    Interface descriptor without Config\n"));
                            }
                            break;
                        }

                        case UDT_ENDPOINT: {
                            struct UsbStdEPDesc *usep = (struct UsbStdEPDesc *) dbuf;
                            if(altif) {
                                if((pep = pAllocEndpoint(altif))) {
                                    STRPTR eptype;
                                    pep->pep_EPNum = usep->bEndpointAddress & 0x0f;
                                    pep->pep_Direction = usep->bEndpointAddress>>7;
                                    pep->pep_TransType = usep->bmAttributes & 0x03;
                                    pep->pep_SyncType = (usep->bmAttributes>>2) & 0x03;
                                    pep->pep_UsageType = (usep->bmAttributes>>4) & 0x03;
                                    eptype = (pep->pep_TransType == USEAF_INTERRUPT) ? "int" : "iso";

                                    pep->pep_MaxPktSize = AROS_LE2WORD(usep->wMaxPacketSize) & 0x07ff;
                                    pep->pep_NumTransMuFr = ((AROS_LE2WORD(usep->wMaxPacketSize)>>11) & 3) + 1;
                                    if(pep->pep_NumTransMuFr == 4) {
                                        psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname, "Endpoint contains illegal Num Trans/Frame value!");
                                        pep->pep_NumTransMuFr = 1;
                                    }

                                    pep->pep_Interval = usep->bInterval;
                                    pep->pep_IntervalRaw = usep->bInterval;
                                    pep->pep_MaxBurst = 1;
                                    pep->pep_CompAttributes = 0;
                                    pep->pep_BytesPerInterval = pep->pep_MaxPktSize;
                                    if(pd->pd_Flags & PDFF_SUPERSPEED) {
                                        switch(pep->pep_TransType) {
                                        case USEAF_CONTROL:
                                        case USEAF_BULK:
                                            pep->pep_Interval = 0; // not used for superspeed control/bulk
                                            break;

                                        case USEAF_ISOCHRONOUS:
                                            if(pep->pep_MaxPktSize > 1024) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "Endpoint contains %s (%ld) MaxPktSize value!",
                                                               (STRPTR) "too high", pep->pep_MaxPktSize);
                                                pep->pep_MaxPktSize = 1024;
                                            }
                                            if(!pep->pep_Interval) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "Super", eptype, (STRPTR) "zero");
                                                pep->pep_Interval = 1;
                                            } else if(pep->pep_Interval > 16) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "Super", eptype, (STRPTR) "too high");
                                                pep->pep_Interval = 16;
                                            }
                                            pep->pep_Interval = 1<<(pep->pep_Interval-1);
                                            break;

                                        case USEAF_INTERRUPT:
                                            if(!pep->pep_Interval) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "Super", eptype, (STRPTR) "zero");
                                                pep->pep_Interval = 1;
                                            }
                                            pep->pep_Interval = 1<<(pep->pep_Interval-1);
                                            break;
                                        }
                                    } else if(pd->pd_Flags & PDFF_HIGHSPEED) {
                                        switch(pep->pep_TransType) {
                                        case USEAF_CONTROL:
                                        case USEAF_BULK:
                                            //pep->pep_Interval = 0; // no use here, NAK rate not of interest
                                            break;

                                        case USEAF_ISOCHRONOUS:
                                            if(pep->pep_MaxPktSize > 1024) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "Endpoint contains %s (%ld) MaxPktSize value!",
                                                               (STRPTR) "too high", pep->pep_MaxPktSize);
                                                pep->pep_MaxPktSize = 1024;
                                            }
                                        // fall through
                                        case USEAF_INTERRUPT:
                                            if(!pep->pep_Interval) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "High", eptype, (STRPTR) "zero");
                                                pep->pep_Interval = 1;
                                            } else if(pep->pep_Interval > 16) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "High", eptype, (STRPTR) "too high");
                                                pep->pep_Interval = 16;
                                            }
                                            pep->pep_Interval = 1<<(pep->pep_Interval-1);
                                            break;
                                        }
                                    } else if(pd->pd_Flags & PDFF_LOWSPEED) {
                                        switch(pep->pep_TransType) {
                                        case USEAF_INTERRUPT:
                                            if(pep->pep_Interval < 8) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "Low", eptype, (STRPTR) "too low");
                                                pep->pep_Interval = 8;
                                            }
                                            break;

                                        case USEAF_CONTROL:
                                        case USEAF_BULK:
                                            pep->pep_Interval = 0; // no use here
                                            break;

                                        case USEAF_ISOCHRONOUS:
                                            psdAddErrorMsg0(RETURN_ERROR, (STRPTR) libname, "Lowspeed devices cannot have isochronous endpoints!");
                                            break;
                                        }
                                    } else {
                                        switch(pep->pep_TransType) {
                                        case USEAF_INTERRUPT:
                                            if(!pep->pep_Interval) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "Full", eptype, (STRPTR) "zero");
                                                pep->pep_Interval = 1;
                                            }
                                            break;

                                        case USEAF_CONTROL:
                                        case USEAF_BULK:
                                            pep->pep_Interval = 0; // no use here
                                            break;

                                        case USEAF_ISOCHRONOUS:
                                            if(pep->pep_MaxPktSize > 1023) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Endpoint contains too high (%ld) MaxPktSize value! Fixing.", pep->pep_MaxPktSize);
                                                pep->pep_MaxPktSize = 1023;
                                            }
                                            if(!pep->pep_Interval) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "Full", eptype, (STRPTR) "zero");
                                                pep->pep_Interval = 1;
                                            } else if(pep->pep_Interval > 16) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "%sspeed %s endpoint contains %s interval value! Fixing.",
                                                               (STRPTR) "Full", eptype, (STRPTR) "too high");
                                                pep->pep_Interval = 16;
                                            }
                                            pep->pep_Interval = 1<<(pep->pep_Interval-1);
                                            break;
                                        }
                                    }

                                    KPRINTF(2, ("      Endpoint %ld\n", pep->pep_EPNum));
                                    KPRINTF(2, ("        Direction : %s\n"
                                                "        TransType : %ld\n"
                                                "        MaxPktSize: %ld\n"
                                                "        Interval  : %ld\n",
                                                (pep->pep_Direction ? "IN" : "OUT"),
                                                pep->pep_TransType, pep->pep_MaxPktSize,
                                                pep->pep_Interval));

                                } else {
                                    KPRINTF(20, ("      Endpoint allocation failed\n"));
                                }
                            } else {
                                psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname, "Endpoint without prior interface descriptor!");
                                KPRINTF(20, ("      Endpoint descriptor without Interface\n"));
                            }
                            break;
                        }

                        case UDT_SUPERSPEED_EP_COMP: {
                            struct UsbSSEndpointCompDesc *comp = (struct UsbSSEndpointCompDesc *) dbuf;
                            if(pep) {
                                pep->pep_MaxBurst = comp->bMaxBurst + 1;
                                pep->pep_CompAttributes = comp->bmAttributes;
                                pep->pep_BytesPerInterval = AROS_LE2WORD(comp->wBytesPerInterval);
                                pep->pep_MaxStreams = pGetMaxStreamsForEndpoint(pep);
                                if((pd->pd_Flags & PDFF_SUPERSPEED) && (pep->pep_TransType == USEAF_ISOCHRONOUS)) {
                                    pep->pep_NumTransMuFr = (comp->bmAttributes & 0x03) + 1;
                                }
                            } else {
                                psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname, "Superspeed companion without prior endpoint descriptor!");
                            }
                            break;
                        }

                        case UDT_SUPERSPEED_ISO_COMP: {
                            struct UsbSSPIsochEndpointCompDesc *comp = (struct UsbSSPIsochEndpointCompDesc *) dbuf;
                            if(pep) {
                                pep->pep_BytesPerInterval = AROS_LE2LONG(comp->dwBytesPerInterval);
                            } else {
                                psdAddErrorMsg0(RETURN_WARN, (STRPTR) libname, "Superspeed isoch companion without prior endpoint descriptor!");
                            }
                            break;
                        }

                        case UDT_DEVICE:
                        case UDT_SSHUB:
                        case UDT_HUB:
                        case UDT_HID:
                        case UDT_REPORT:
                        case UDT_PHYSICAL:
                        case UDT_CS_INTERFACE:
                        case UDT_CS_ENDPOINT:
                        case UDT_DEVICE_QUALIFIER:
                        case UDT_OTHERSPEED_QUALIFIER:
                        case UDT_INTERFACE_POWER:
                        case UDT_OTG:
                            //psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Skipping descriptor %02lx (pc=0x%08lx, pif=0x%08lx altpif=0x%08lx).", dbuf[1], pc, pif, altif);
                            KPRINTF(1, ("Skipping unknown descriptor %ld.\n", dbuf[1]));
                            break;

                        default:
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Skipping unknown descriptor %02lx.", dbuf[1]);
                            KPRINTF(1, ("Skipping unknown descriptor %ld.\n", dbuf[1]));
                            break;
                        }
                        // add descriptor to device
                        pdd = pAllocDescriptor(pd, dbuf);
                        if(pdd) {
                            STRPTR descname = NULL;

                            pdd->pdd_Config = pc;
                            pdd->pdd_Interface = altif;
                            pdd->pdd_Endpoint = pep;
                            if(pdd->pdd_Interface) {
                                if((pdd->pdd_Type >= UDT_CS_UNDEFINED) && (pdd->pdd_Type <= UDT_CS_ENDPOINT)) {
                                    descname = psdNumToStr(NTS_DESCRIPTOR, (LONG) (pdd->pdd_CSSubType<<24)|(pif->pif_IfSubClass<<16)|(pif->pif_IfClass<<8)|pdd->pdd_Type, NULL);
                                    if(!descname) {
                                        descname = psdNumToStr(NTS_DESCRIPTOR, (LONG) (pdd->pdd_CSSubType<<24)|(pif->pif_IfClass<<8)|pdd->pdd_Type, NULL);
                                    }
                                }
                                if(!descname) {
                                    descname = psdNumToStr(NTS_DESCRIPTOR, (LONG) (pif->pif_IfSubClass<<16)|(pif->pif_IfClass<<8)|pdd->pdd_Type, NULL);
                                }
                                if(!descname) {
                                    descname = psdNumToStr(NTS_DESCRIPTOR, (LONG) (pif->pif_IfClass<<8)|pdd->pdd_Type, NULL);
                                }
                            }
                            if(descname) {
                                pdd->pdd_Name = descname;
                            }
                        }
                        dbuf += dlen;
                    }
                    KPRINTF(1, ("Configuration acquired!\n"));
                    psdFreeVec(tempbuf);
                    curcfg++;
                    continue;
                    //psdUnlockDevice(pd);
                    //return(TRUE);
                } else {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "GET_DESCRIPTOR (len %ld) failed: %s (%ld)",
                                   len, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    KPRINTF(15, ("GET_DESCRIPTOR failed %ld!\n", ioerr));
                }
                psdFreeVec(tempbuf);
            } else {
                KPRINTF(20, ("No memory for %ld bytes config temp buffer!\n", len));
            }
        } else {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "GET_DESCRIPTOR (len %ld) failed: %s (%ld)",
                           9, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            KPRINTF(15, ("GET_DESCRIPTOR (9) failed %ld!\n", ioerr));
        }
        psdUnlockDevice(pd);
        return(FALSE);
    }
    psdUnlockDevice(pd);
    return(TRUE);
}
/* \\\ */

/* /// "pPowerRecurseDrain()" */
ULONG pPowerRecurseDrain(struct PsdBase * ps, struct PsdDevice *pd)
{
    struct PsdDevice *nextpd;
    struct PsdConfig *pc;
    UWORD maxdrain = 666;
    UWORD childdrain;
    BOOL selfpwd = TRUE;
    pd->pd_PowerDrain = 0;

    /* look at config */
    if((pc = pd->pd_CurrentConfig)) {

        /* if suspended, no more than 500 microamps are drained */
        if(pd->pd_Flags & PDFF_SUSPENDED) {
            pd->pd_PowerDrain = (pc->pc_MaxPower >= 100) ? 3 : 1;
            return(pd->pd_PowerDrain);
        }
        selfpwd = ((pc->pc_Attr & USCAF_SELF_POWERED) && (pd->pd_NotifyCfg.ntc_OverridePowerInfo != POCP_BUS_POWERED)) ||
                  (pd->pd_NotifyCfg.ntc_OverridePowerInfo == POCP_SELF_POWERED);
        maxdrain = selfpwd ? 500 : 100;
    }

    /* examine children */
    nextpd = (struct PsdDevice *) pd->pd_Hardware->phw_Devices.lh_Head;
    while(nextpd->pd_Node.ln_Succ) {
        if(nextpd->pd_Hub == pd) {
            childdrain = pPowerRecurseDrain(ps, nextpd);
            // limit the drain to the maximum power suckage
            pd->pd_PowerDrain += (childdrain > maxdrain) ? maxdrain : childdrain;
        }
        nextpd = (struct PsdDevice *) nextpd->pd_Node.ln_Succ;
    }

    /* look at config */
    if(selfpwd) {
        pd->pd_PowerDrain = 0;
    } else {
        pd->pd_PowerDrain += pc->pc_MaxPower;
    }
    return(pd->pd_PowerDrain);
}
/* \\\ */

/* /// "pPowerRecurseSupply()" */
void pPowerRecurseSupply(struct PsdBase * ps, struct PsdDevice *pd)
{
    struct PsdDevice *nextpd;
    struct PsdConfig *pc;
    UWORD ports = 0;
    UWORD supply = 666;
    BOOL selfpwd = TRUE;

    /* look at config */
    if((pc = pd->pd_CurrentConfig)) {
        selfpwd = ((pc->pc_Attr & USCAF_SELF_POWERED) && (pd->pd_NotifyCfg.ntc_OverridePowerInfo != POCP_BUS_POWERED)) ||
                  (pd->pd_NotifyCfg.ntc_OverridePowerInfo == POCP_SELF_POWERED);
    }

    /* count children */
    nextpd = (struct PsdDevice *) pd->pd_Hardware->phw_Devices.lh_Head;
    while(nextpd->pd_Node.ln_Succ) {
        if(nextpd->pd_Hub == pd) { // this device is a child of us (we're a hub!)
            ports++;
        }
        nextpd = (struct PsdDevice *) nextpd->pd_Node.ln_Succ;
    }

    /* look at config */
    if(selfpwd) {
        if(pc) {
            pd->pd_PowerSupply = ports ? 500*ports + pc->pc_MaxPower : pc->pc_MaxPower;
        }
        supply = 500; // each downstream port gets the full monty
    } else {
        // the parent hub has already set the amount of supply for this port
        if(pd->pd_PowerSupply >= pc->pc_MaxPower) {
            // the downstream ports get the remaining divided attention
            if(ports) {
                // avoid division by zero
                supply = (pd->pd_PowerSupply - pc->pc_MaxPower) / ports;
                if(supply > 100) {
                    // limit to 100 mA per port
                    supply = 100;
                }
            }
        } else {
            supply = 1; // bad luck, out of power
        }
    }

    /* set supply */
    if(ports) { /* needs to be a hub */
        // propagate supply down to the children
        nextpd = (struct PsdDevice *) pd->pd_Hardware->phw_Devices.lh_Head;
        while(nextpd->pd_Node.ln_Succ) {
            if(nextpd->pd_Hub == pd) {
                nextpd->pd_PowerSupply = supply;
                pPowerRecurseSupply(ps, nextpd);
            }
            nextpd = (struct PsdDevice *) nextpd->pd_Node.ln_Succ;
        }
    }
    if(pd->pd_PowerDrain > pd->pd_PowerSupply) {
        if(!(pd->pd_Flags & PDFF_LOWPOWER)) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Detected low power condition for '%s'.", pd->pd_ProductStr);
            pd->pd_Flags |= PDFF_LOWPOWER;
            psdSendEvent(EHMB_DEVICELOWPW, pd, NULL);
        }
    } else {
        if(pd->pd_Flags & PDFF_LOWPOWER) {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Low power condition resolved for '%s'.", pd->pd_ProductStr);
            pd->pd_Flags &= ~PDFF_LOWPOWER;
        }
    }
}
/* \\\ */

/* /// "pGarbageCollectEvents()" */
void pGarbageCollectEvents(struct PsdBase * ps)
{
    struct PsdEventNote *pen;
    while((pen = (struct PsdEventNote *) GetMsg(&ps->ps_EventReplyPort))) {
        psdFreeVec(pen);
    }
}
/* \\\ */

/* /// "pFindName()" */
struct Node * pFindName(struct PsdBase * ps, struct List *list, STRPTR name)
{
    struct Node *res = NULL;

    Forbid();
    while(*name) {
        res = FindName(list, name);
        if(res) {
            break;
        }
        do {
            if((*name == '/') || (*name == ':')) {
                ++name;
                break;
            }
        } while(*(++name));
    }
    Permit();
    return(res);
}
/* \\\ */

/* /// "pStripString()" */
void pStripString(struct PsdBase * ps, STRPTR str)
{
    STRPTR srcptr = str;
    STRPTR tarptr = str;
    STRPTR lastgoodchar = str;
    BOOL leadingspaces = TRUE;
    UBYTE ch;
    ULONG len = 0;

    while((ch = *srcptr++)) {
        len++;
        if(ch == ' ') {
            if(!leadingspaces) {
                *tarptr++ = ch;
            }
        } else {
            *tarptr++ = ch;
            lastgoodchar = tarptr;
            leadingspaces = FALSE;
        }
    }
    *lastgoodchar = 0;
    // empty string?
    if((str == lastgoodchar) && (len > 6)) {
        strcpy(str, "<empty>");
    }
}
/* \\\ */

/* /// "pFixBrokenConfig()" */
BOOL pFixBrokenConfig(struct PsdPipe *pp)
{
    struct PsdDevice *pd = pp->pp_Device;
    struct PsdBase * ps = pd->pd_Hardware->phw_Base;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    BOOL fixed = FALSE;

    switch(pd->pd_VendorID) {
    case 0x03eb: /* Atmel */
        if(pd->pd_ProductID == 0x3312) {
            psdFreeVec(pd->pd_ProductStr);
            pd->pd_ProductStr = psdCopyStr("Highway/Subway Root Hub");
        }
        break;

    case 0x04e6: /* E-Shuttle */
        if(pd->pd_ProductID == 0x0001) { /* LS120 */
            pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            /* Get msd interface and fix it */
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            if(pif->pif_IfClass != MASSSTORE_CLASSCODE) {
                fixed = TRUE;
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Fixing broken %s interface descriptor!", (STRPTR) "E-Shuttle LS120");
                pif->pif_IfClass = MASSSTORE_CLASSCODE;
                pif->pif_IfSubClass = MS_ATAPI_SUBCLASS;
                pif->pif_IfProto = MS_PROTO_CB;
            }
        }
        break;

    case 0x054C: /* Sony */
        if((pd->pd_ProductID == 0x002E) || (pd->pd_ProductID == 0x0010)) { /* Handycam */
            fixed = TRUE;
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Fixing broken %s interface descriptor!", (STRPTR) "Sony MSD");
            pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            /* Get msd interface and fix it */
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            pif->pif_IfSubClass = MS_RBC_SUBCLASS;
        }
        break;

    case 0x057b: /* Y-E Data */
        if(pd->pd_ProductID == 0x0000) { /* Flashbuster U */
            pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            /* Get msd interface and fix it */
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            if(pif->pif_IfClass != MASSSTORE_CLASSCODE) {
                fixed = TRUE;
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Fixing broken %s interface descriptor!", (STRPTR) "Y-E Data USB Floppy");
                pif->pif_IfClass = MASSSTORE_CLASSCODE;
                pif->pif_IfSubClass = MS_UFI_SUBCLASS;
                pif->pif_IfProto = (pd->pd_DevVers < 0x0300) ? MS_PROTO_CB : MS_PROTO_CBI;
            }
        }
        break;

    case 0x04ce: /* ScanLogic */
        if(pd->pd_ProductID == 0x0002) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Fixing broken %s interface descriptor!", (STRPTR) "ScanLogic");
            pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            /* Get msd interface and fix it */
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            fixed = TRUE;
            pif->pif_IfSubClass = MS_SCSI_SUBCLASS;
        }
        break;

    case 0x0584: /* Ratoc cardreader */
        if(pd->pd_ProductID == 0x0008) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Fixing broken %s interface descriptor!", (STRPTR) "RATOC");
            pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            /* Get msd interface and fix it */
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            fixed = TRUE;
            pif->pif_IfClass = MASSSTORE_CLASSCODE;
            pif->pif_IfSubClass = MS_SCSI_SUBCLASS;
            pif->pif_IfProto = MS_PROTO_BULK;
        }
        break;

    case 0x04b8: /* Epson */
        if(pd->pd_ProductID == 0x0602) { /* EPX Storage device (Card slot in Printer) */
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Fixing broken %s interface descriptor!", (STRPTR) "Epson storage");
            pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
            /* Get msd interface and fix it */
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            fixed = TRUE;
            pif->pif_IfClass = MASSSTORE_CLASSCODE;
            pif->pif_IfSubClass = MS_SCSI_SUBCLASS;
            pif->pif_IfProto = MS_PROTO_BULK;
        }
        break;

    default:
        break;
    }
    return(fixed);
}
/* \\\ */

/* /// "pHaveDOS()" */
BOOL pHaveDOS(struct PsdBase * ps)
{
    if(DOSBase) {
        return TRUE;
    }
    return FALSE;
}


/* /// "pOpenDOS()" */
BOOL pOpenDOS(struct PsdBase * ps)
{
    if(DOSBase) {
        return TRUE;
    }
    if((DOSBase = OpenLibrary("dos.library", 39))) {
        return TRUE;
    }
    return FALSE;
}
/* \\\ */

/* *** Class Scan Task *** */

/* /// "pStartEventHandler()" */
BOOL pStartEventHandler(struct PsdBase * ps)
{
    struct PsdHandlerTask *ph = &ps->ps_EventHandler;

    ObtainSemaphore(&ps->ps_NotifyLock);
    if(ph->ph_Task) {
        ReleaseSemaphore(&ps->ps_NotifyLock);
        return(TRUE);
    }
    ph->ph_ReadySignal = SIGB_SINGLE;
    ph->ph_ReadySigTask = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE); // clear single bit
    if(psdSpawnSubTask("Neptune Event Broadcast", pEventHandlerTask, ps)) {
        Wait(1UL<<ph->ph_ReadySignal);
    }
    ph->ph_ReadySigTask = NULL;
    //FreeSignal(ph->ph_ReadySignal);
    if(ph->ph_Task) {
        ReleaseSemaphore(&ps->ps_NotifyLock);
        psdAddErrorMsg0(RETURN_OK, (STRPTR) libname, "Event broadcaster started.");
        return(TRUE);
    }
    ReleaseSemaphore(&ps->ps_NotifyLock);
    return(FALSE);
}
/* \\\ */

/* *** Hardware Driver Task *** */

/* /// "pCtxQueryCmdMask()" */
/* NSCMD_DEVICEQUERY on the freshly opened HCD: bitmask of the context
   lifecycle ops in its NSD list. Runs in the device task while it still owns
   phw_RootIOReq exclusively; the query uses IOStdReq framing per the NSD
   spec (the request is large enough for either shape). */
static ULONG pCtxQueryCmdMask(struct PsdBase *ps, struct PsdHardware *phw)
{
    struct NSDeviceQueryResult nqr;
    struct IOStdReq *sio = (struct IOStdReq *) phw->phw_RootIOReq;
    ULONG mask = 0;

    memset(&nqr, 0, sizeof(nqr));
    sio->io_Command = NSCMD_DEVICEQUERY;
    sio->io_Data = &nqr;
    sio->io_Length = sizeof(nqr);
    if(!DoIO((struct IORequest *) sio)) {
        UWORD *cmdp = nqr.nsdqr_SupportedCommands;
        if(cmdp) {
            while(*cmdp) {
                if((*cmdp >= NSCMD_USBHCD_BASE) && (*cmdp < NSCMD_USBHCD_BASE + 32)) {
                    mask |= UHCD_CTXCMD_BIT(*cmdp);
                }
                cmdp++;
            }
        }
    }
    return(mask);
}
/* \\\ */

/* /// "pCtxAttach()" */
/* NSCMD_USB_ATTACH on the freshly opened HCD: exchange the library's
   transfer-completion hook for the driver's direct submit/abort entries
   (usbhcd_context.h "The transfer path"). Same exclusive phw_RootIOReq
   context as the NSD scan above. */
static LONG pCtxAttach(struct PsdBase *ps, struct PsdHardware *phw)
{
    struct UhcdAttach ato;
    struct IOStdReq *sio = (struct IOStdReq *) phw->phw_RootIOReq;
    LONG ioerr;

    phw->phw_XferDoneHook.h_Entry = (APTR) pXferDoneHook;
    phw->phw_XferDoneHook.h_Data = phw;

    memset(&ato, 0, sizeof(ato));
    ato.ato_DoneHook = &phw->phw_XferDoneHook;
    ato.ato_UserData = phw;
    sio->io_Command = NSCMD_USB_ATTACH;
    sio->io_Data = &ato;
    sio->io_Length = sizeof(ato);
    ioerr = DoIO((struct IORequest *) sio);
    if(!ioerr && (!ato.ato_Submit || !ato.ato_CtrlSubmit || !ato.ato_Abort)) {
        ioerr = UHIOERR_BADPARAMS;
    }
    if(!ioerr) {
        phw->phw_CtxHcd = ato.ato_HcdContext;
        phw->phw_CtxSubmit = ato.ato_Submit;
        phw->phw_CtxCtrlSubmit = ato.ato_CtrlSubmit;
        phw->phw_CtxAbort = ato.ato_Abort;
    }
    return(ioerr);
}
/* \\\ */

/* /// "pDeviceTask()" */
void pDeviceTask()
{
    struct PsdBase * ps;
    struct PsdHardware *phw;
    struct Task *thistask;
    ULONG sigs;
    ULONG sigmask;
    LONG ioerr;
    struct TagItem taglist[13];
    struct TagItem *tag;
    struct PsdPipe *pp;
    struct IOUsbHWReq *ioreq;

    STRPTR prodname = NULL;
    STRPTR manufacturer = NULL;
    STRPTR description = NULL;
    STRPTR copyright = NULL;
    ULONG  version = 0;
    ULONG  revision = 0;
    ULONG  driververs = 0x0100;
    ULONG  caps = UHCF_ISO;
    ULONG  numroothubs = 1;
    ULONG  dmaalign = 0;
    ULONG cnt;

    if(!(ps = (struct PsdBase *) OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION))) {
        Alert(AG_OpenLib);
        return;
    }

//    KPrintF("[neptune] %s: neptune.library @ 0x%08lx\n", __func__, ps);

    thistask = FindTask(NULL);
    SetTaskPri(thistask, 21);
    phw = thistask->tc_UserData;

    memset(&phw->phw_TaskMsgPort, 0, sizeof(phw->phw_TaskMsgPort));
    phw->phw_TaskMsgPort.mp_Node.ln_Type = NT_MSGPORT;
    phw->phw_TaskMsgPort.mp_Node.ln_Name = (APTR) phw;
    phw->phw_TaskMsgPort.mp_Flags = PA_SIGNAL;
    phw->phw_TaskMsgPort.mp_SigTask = thistask;
    phw->phw_TaskMsgPort.mp_SigBit = AllocSignal(-1L);
    NewList(&phw->phw_TaskMsgPort.mp_MsgList);

//    KPrintF("[neptune] %s: Task port @ 0x%08lx\n ", __func__, &phw->phw_TaskMsgPort);

    memset(&phw->phw_DevMsgPort, 0, sizeof(phw->phw_DevMsgPort));
    phw->phw_DevMsgPort.mp_Node.ln_Type = NT_MSGPORT;
    phw->phw_DevMsgPort.mp_Node.ln_Name = (APTR) phw;
    phw->phw_DevMsgPort.mp_Flags = PA_SIGNAL;
    phw->phw_DevMsgPort.mp_SigTask = thistask;
    phw->phw_DevMsgPort.mp_SigBit = AllocSignal(-1L);
    NewList(&phw->phw_DevMsgPort.mp_MsgList);

//    KPrintF("[neptune] %s: Task port @ 0x%08lx\n", __func__, &phw->phw_DevMsgPort);

    if((phw->phw_RootIOReq = (struct IOUsbHWReq *) CreateIORequest(&phw->phw_DevMsgPort, sizeof(struct IOUsbHWReq)))) {
//        KPrintF("[neptune] %s: ioreq @ 0x%08lx\n", __func__, phw->phw_RootIOReq);
        /* phw_DevName is always the bare driver name (psdAddHardware() strips
           paths). In memory -- Kickstart/Emu68 resident, or already open -- it is
           opened by that name; anything else is loaded from the one drawer HCDs
           live in, since a bare name alone is only looked for in DEVS: (exec
           OpenDevice autodoc). Forbid covers the lookup only: the driver's Open
           vector may Wait. */
        char path[108];
        STRPTR devname = phw->phw_DevName;
        Forbid();
        BOOL inmem = (FindName(&EXEC_BASE_NAME->DeviceList, devname) != NULL);
        Permit();
        if(!inmem) {
            psdSafeRawDoFmt(path, sizeof(path), NEP_HWDRAWER "/%s", devname);
            devname = path;
        }
        ioerr = OpenDevice(devname, phw->phw_Unit, (struct IORequest *) phw->phw_RootIOReq, 0);

//        KPrintF("[neptune] %s: device @ 0x%08lx\n", __func__, phw->phw_RootIOReq->iouh_Req.io_Device);

        if(!ioerr) {
            phw->phw_Node.ln_Name = phw->phw_RootIOReq->iouh_Req.io_Device->dd_Library.lib_Node.ln_Name;
            tag = taglist;
            tag->ti_Tag = UHA_ProductName;
            tag->ti_Data = (IPTR) &prodname;
            ++tag;
            tag->ti_Tag = UHA_Manufacturer;
            tag->ti_Data = (IPTR) &manufacturer;
            ++tag;
            tag->ti_Tag = UHA_Description;
            tag->ti_Data = (IPTR) &description;
            ++tag;
            tag->ti_Tag = UHA_Version;
            tag->ti_Data = (IPTR) &version;
            ++tag;
            tag->ti_Tag = UHA_Revision;
            tag->ti_Data = (IPTR) &revision;
            ++tag;
            tag->ti_Tag = UHA_Copyright;
            tag->ti_Data = (IPTR) &copyright;
            ++tag;
            tag->ti_Tag = UHA_DriverVersion;
            tag->ti_Data = (IPTR) &driververs;
            ++tag;
            tag->ti_Tag = UHA_Capabilities;
            tag->ti_Data = (IPTR) &caps;
            ++tag;
            tag->ti_Tag = UHA_NumRootHubs;
            tag->ti_Data = (IPTR) &numroothubs;
            ++tag;
            tag->ti_Tag = UHA_DMAAlignment;
            tag->ti_Data = (IPTR) &dmaalign;
            ++tag;
            tag->ti_Tag = TAG_END;
            phw->phw_RootIOReq->iouh_Data = taglist;
            phw->phw_RootIOReq->iouh_Req.io_Command = UHCMD_QUERYDEVICE;
            DoIO((struct IORequest *) phw->phw_RootIOReq);

//            KPrintF("[neptune] %s: device queried\n", __func__);

            phw->phw_ProductName = psdCopyStr(prodname ? prodname : (STRPTR) "n/a");
            phw->phw_Manufacturer = psdCopyStr(manufacturer ? manufacturer : (STRPTR) "n/a");
            phw->phw_Description = psdCopyStr(description ? description : (STRPTR) "n/a");
            phw->phw_Copyright = psdCopyStr(copyright ? copyright : (STRPTR) "n/a");
            phw->phw_Version = version;
            phw->phw_Revision = revision;
            phw->phw_DriverVers = driververs;
            phw->phw_Capabilities = caps;
            phw->phw_DMAAlignment = (UWORD) dmaalign;   /* 0 = HCD imposes no DMA alignment constraint */

            /* Lower-edge lifecycle backend: legacy software-managed addressing
               by default; a context HCD (UHCF_CONTEXT plus the mandatory op
               set in its NSD list) gets the context backend. */
            phw->phw_HCDOps = &pLegacyHCDOps;
            if(caps & UHCF_CONTEXT) {
                const ULONG mandatory = UHCD_MANDATORY_CMD_MASK;
                ULONG cmdmask = pCtxQueryCmdMask(ps, phw);
                if(((cmdmask & mandatory) == mandatory) && !pCtxAttach(ps, phw)) {
                    phw->phw_CtxCmdMask = cmdmask;
                    phw->phw_ContextBackend = TRUE;
                    phw->phw_StreamsSupported = (cmdmask & UHCD_CTXCMD_BIT(NSCMD_USB_ALLOC_STREAMS)) ? TRUE : FALSE;
                    phw->phw_HCDOps = &pContextHCDOps;
                    /* context drivers may split the root hub by protocol
                       (USB2 + USB3); the legacy view keeps the single
                       mixed root hub */
                    phw->phw_NumRootHubs = (numroothubs > 1) ? 2 : 1;
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "Using the context lifecycle ABI for %s.", prodname);
                } else {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "%s claims the context ABI but lacks mandatory ops or the attach failed (mask 0x%08lx), staying on the legacy backend.",
                                   prodname, cmdmask);
                }
            }

            /* Both ports stay PA_SIGNAL (set above) and are serviced by this relay
             * task.  Quick HCDs are handled per-request via traditional IOF_QUICK in
             * pSubmitPipe() (caller-context BeginIO), not via message-port callbacks. */
            sigmask = SIGBREAKF_CTRL_C
                    | (1UL<<phw->phw_DevMsgPort.mp_SigBit)
                    | (1UL<<phw->phw_TaskMsgPort.mp_SigBit);
            if(caps & UHCF_QUICKIO) {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Enabling QuickIO for %s.", prodname);
            }

            KPRINTF(10, ("%s ready!\n", thistask->tc_Node.ln_Name));
            phw->phw_Task = thistask;

//            KPrintF("[neptune] %s: registering in neptune.library..\n", __func__);

            psdLockWritePBase();
            AddTail(&ps->ps_Hardware, &phw->phw_Node);
            psdUnlockPBase();

//            KPrintF("[neptune] %s: sending ready signal\n", __func__);

            Forbid();
            if(phw->phw_ReadySigTask) {
                Signal(phw->phw_ReadySigTask, 1L<<phw->phw_ReadySignal);
            }
            Permit();
            do {
                KPRINTF(1, ("Main loop wait.\n"));
                while((pp = (struct PsdPipe *) GetMsg(&phw->phw_TaskMsgPort))) {
                    if(pp->pp_AbortPipe) {
                        KPRINTF(2, ("Abort pipe 0x%08lx\n", pp->pp_AbortPipe));
                        AbortIO(pp->pp_AbortPipe->pp_WireReq);
                        ReplyMsg(&pp->pp_Msg);
                        KPRINTF(2, ("Replying evil pipe 0x%08lx\n", pp));
                    } else {
                        KPRINTF(1, ("Forwarding pipe 0x%08lx\n", pp));
                        SendIO(pp->pp_WireReq);
                        ++phw->phw_MsgCount;
                    }
                }
                while((ioreq = (struct IOUsbHWReq *) GetMsg(&phw->phw_DevMsgPort))) {
                    struct PsdPipe *dpp = pWireReqPipe(ioreq);
                    KPRINTF(1, ("Replying pipe 0x%08lx\n", dpp));
                    pCompletePipe(dpp);
                    --phw->phw_MsgCount;
                }
                sigs = Wait(sigmask);
            } while(!(sigs & SIGBREAKF_CTRL_C));

            /* Flush all pending IO Requests */
            phw->phw_RootIOReq->iouh_Req.io_Command = CMD_FLUSH;
            DoIO((struct IORequest *) phw->phw_RootIOReq);
            cnt = 0;
            while(phw->phw_MsgCount) {
                KPRINTF(20, ("Still %ld iorequests pending!\n", phw->phw_MsgCount));
                psdDelayMS(100);
                if(++cnt == 50) {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Still %ld IORequests pending before the unit can go down.",
                                   phw->phw_MsgCount);
                }
                if(cnt == 300) {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Timed out waiting for %ld pending IORequests; abandoning them.",
                                   phw->phw_MsgCount);
                    phw->phw_MsgCount = 0;
                    break;
                }
                while((ioreq = (struct IOUsbHWReq *) GetMsg(&phw->phw_DevMsgPort))) {
                    struct PsdPipe *dpp = pWireReqPipe(ioreq);
                    KPRINTF(1, ("Replying pipe 0x%08lx\n", dpp));
                    pCompletePipe(dpp);
                    --phw->phw_MsgCount;
                }
            }
            psdLockWritePBase();
            Remove(&phw->phw_Node);
            psdUnlockPBase();
            CloseDevice((struct IORequest *) phw->phw_RootIOReq);
        } else {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                           "Opening %s unit %ld failed %s (%ld).",
                           devname, phw->phw_Unit, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        }
        DeleteIORequest((struct IORequest *) phw->phw_RootIOReq);
        phw->phw_RootIOReq = NULL;
    }
    FreeSignal((LONG) phw->phw_TaskMsgPort.mp_SigBit);
    FreeSignal((LONG) phw->phw_DevMsgPort.mp_SigBit);

    CloseLibrary((struct Library *) ps);
    phw->phw_Task = NULL;

    Forbid();
    if(phw->phw_ReadySigTask) {
        Signal(phw->phw_ReadySigTask, 1L<<phw->phw_ReadySignal);
    }
}
/* \\\ */

/* /// "pIdleSuspendSweep()" */
/* One pass of the idle auto-suspend sweep, run once a second by the event
   handler task while power saving is on: suspend every configured non-hub
   device that has no IO in flight (pd_IOBusyCount), has been idle for longer
   than pgc_SuspendTimeout and whose bound classes all say they can take it
   (pgc_ForceSuspend overrides that for a device that can remote-wake).

   Hubs stay excluded, deliberately.  A suspended hub cannot see its own
   disconnection - EP1 is aborted and re-armed from exactly one place gated on
   nch_Running - so detection is its parent's job, and a root hub has none.  A
   hub is also idle almost permanently, and suspending one suspends everything
   below it, including devices this very walk is iterating over.

   The walk holds PBase, because psdGetNextDevice() chases ln_Succ through lists
   that pFreeDevice() can unlink.  It is dropped around psdSuspendDevice(), which
   blocks on control transfers, and the walk restarts from the head afterwards
   (the psdRemClass() idiom).  Restarting cannot loop: pd_LastActivity is zeroed
   before the lock is dropped, and a zero stamp is never eligible again until
   fresh IO restamps it.  That also makes a refused attempt fire once: its
   rollback (SET_SUSPEND(0), the resume methods) is housekeeping that never
   calls pActivityBegin(), so it leaves the stamp alone. */
static void pIdleSuspendSweep(struct PsdBase *ps)
{
    struct timeval currtime;
    BOOL restart;

    GetSysTime((APTR) &currtime);
    psdLockReadPBase();
    do {
        struct PsdDevice *pd = NULL;
        restart = FALSE;
        while((pd = psdGetNextDevice(pd))) {
            struct PsdUsbClass *puc;
            struct PsdInterface *pif;
            BOOL doit = TRUE;
            IPTR suspendable;

            /* PDFF_CONFIGURED is set at descriptor parse; a failed
               SET_CONFIGURATION still leaves pd_CurrentConfig NULL */
            if((pd->pd_DevClass == HUB_CLASSCODE) || (!pd->pd_CurrentConfig) ||
               ((pd->pd_Flags & (PDFF_CONFIGURED|PDFF_DEAD|PDFF_SUSPENDED|PDFF_APPBINDING|PDFF_DELEXPUNGE)) != PDFF_CONFIGURED)) {
                continue;
            }
            if(pd->pd_NotifyCfg.ntc_NoAutoSuspend) {
                continue; /* the user pinned this one awake */
            }
            if(pd->pd_IOBusyCount) {
                continue;
            }
            if((!pd->pd_LastActivity.tv_secs) ||
               ((currtime.tv_secs - pd->pd_LastActivity.tv_secs) <= ps->ps_GlobalCfg->pgc_SuspendTimeout)) {
                continue;
            }
            if(!((pd->pd_CurrentConfig->pc_Attr & USCAF_REMOTE_WAKEUP) && ps->ps_GlobalCfg->pgc_ForceSuspend)) {
                if(pd->pd_DevBinding && ((puc = pd->pd_ClsBinding))) {
                    suspendable = 0;
                    usbGetAttrs(UGA_CLASS, NULL, UCCA_SupportsSuspend, &suspendable, TAG_END);
                    if(!suspendable) {
                        doit = FALSE;
                    }
                }
                pif = (struct PsdInterface *) pd->pd_CurrentConfig->pc_Interfaces.lh_Head;
                while(pif->pif_Node.ln_Succ) {
                    if(pif->pif_IfBinding && ((puc = pif->pif_ClsBinding))) {
                        suspendable = 0;
                        usbGetAttrs(UGA_CLASS, NULL, UCCA_SupportsSuspend, &suspendable, TAG_END);
                        if(!suspendable) {
                            doit = FALSE;
                            break;
                        }
                    }
                    pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
                }
            }
            /* fire once - stamped before the lock goes, so the restart below
               cannot pick this device up again */
            pd->pd_LastActivity.tv_secs = 0;
            if(!doit) {
                continue;
            }
            psdUnlockPBase();
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Suspending '%s'.", pd->pd_ProductStr);
            psdSuspendDevice(pd);
            psdLockReadPBase();
            restart = TRUE;
            break;
        }
    } while(restart);
    psdUnlockPBase();
}
/* \\\ */

/* /// "pIsDeviceStillValid()" */
/* Is pd still one of the stack's devices? For a pointer kept across a wait
   (an event note, a popup). */
BOOL pIsDeviceStillValid(struct PsdBase *ps, struct PsdDevice *pd)
{
    struct PsdDevice *tmppd = NULL;

    while((tmppd = psdGetNextDevice(tmppd))) {
        if(tmppd == pd) {
            return TRUE;
        }
    }
    return FALSE;
}
/* \\\ */

/* /// "pIsClassStillValid()" */
/* Is ucb still the base of a loaded class? */
BOOL pIsClassStillValid(struct PsdBase *ps, struct Library *ucb)
{
    struct PsdUsbClass *puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;

    while(puc->puc_Node.ln_Succ) {
        if(puc->puc_ClassBase == ucb) {
            return TRUE;
        }
        puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
    }
    return FALSE;
}
/* \\\ */

/* /// "pAutoRecoverAction()" */
/* What the stack settings ask for a dead or overdrawn device: EHRA_POWERCYCLE,
   EHRA_DISABLE or EHRA_NONE. EHRA_NONE also without a bound parent hub to do
   it - a root hub has no port. */
static ULONG pAutoRecoverAction(struct PsdBase *ps, struct PsdDevice *pd)
{
    struct PsdGlobalCfg *pgc = ps->ps_GlobalCfg;
    BOOL isdead = (pd->pd_Flags & PDFF_DEAD) ? TRUE : FALSE;
    BOOL lowpower = (pd->pd_Flags & PDFF_LOWPOWER) ? TRUE : FALSE;

    if(!(pd->pd_Hub && pd->pd_Hub->pd_DevBinding)) {
        return EHRA_NONE;
    }
    if(isdead && pgc->pgc_AutoRestartDead) {
        return EHRA_POWERCYCLE;
    }
    if((isdead && pgc->pgc_AutoDisableDead) || (lowpower && pgc->pgc_AutoDisableLP)) {
        return EHRA_DISABLE;
    }
    return EHRA_NONE;
}
/* \\\ */

/* /// "pAutoRecover()" */
/* Handle a device an EHMB_DEVICEDEAD or EHMB_DEVICELOWPW note names: apply
   pAutoRecoverAction() and report the outcome, EHRA_NONE included, as one
   EHMB_DEVRECOVERY note carrying everything a listener shows - the device
   may be gone by the time it is read. Runs in the event task, so recovery
   works whether or not the notifier does. Under the PBase read lock: the
   device cannot be unlinked meanwhile (psdFreeDevice needs the write lock),
   so it stays valid here, and the note is queued before the hub task can
   free it. */
static void pAutoRecover(struct PsdBase *ps, struct PsdDevice *pd)
{
    psdLockReadPBase();
    if(pIsDeviceStillValid(ps, pd) && (pd->pd_Flags & (PDFF_DEAD|PDFF_LOWPOWER))) {
        STRPTR name = pd->pd_ProductStr ? pd->pd_ProductStr : (STRPTR) "";
        ULONG len = sizeof(struct PsdDevRecovery) + strlen(name);
        struct PsdDevRecovery *pdr = psdAllocVec(len);
        ULONG action = pAutoRecoverAction(ps, pd);
        struct PsdUsbClass *puc = action ? pd->pd_Hub->pd_ClsBinding : NULL;

        if(action && !(puc && pIsClassStillValid(ps, puc->puc_ClassBase) &&
                       usbDoMethod((action == EHRA_POWERCYCLE) ? UCM_HubPowerCyclePort : UCM_HubDisablePort,
                                   pd->pd_Hub, pd->pd_HubPort))) {
            action = EHRA_NONE; /* the hub class did not take it */
        }
        if(action) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           (action == EHRA_POWERCYCLE)
                           ? "Automatically powercycling port for '%s' due to death event."
                           : "Automatically disabling port for '%s' due lowpower/death event.",
                           pd->pd_ProductStr);
        }
        if(pdr) {
            pdr->pdr_Action = action;
            pdr->pdr_Flags = ((pd->pd_Flags & PDFF_DEAD) ? PDRF_DEAD : 0) |
                             ((pd->pd_Flags & PDFF_LOWPOWER) ? PDRF_LOWPOWER : 0);
            strcpy((STRPTR) pdr->pdr_ProductName, name);
            pSendEventWithData(ps, EHMB_DEVRECOVERY, pd, NULL, pdr, len);
            psdFreeVec(pdr);
        }
    }
    psdUnlockPBase();
}
/* \\\ */

/* /// "pEventHandlerTask()" */
void pEventHandlerTask()
{
    struct PsdBase * ps;
    struct Task *thistask;
    ULONG sigs;
    ULONG sigmask;
    struct PsdUsbClass *puc;
    struct PsdHandlerTask *ph;
    struct PsdEventNote *pen;
    ULONG counter;
    ULONG cfgchanged;

    thistask = FindTask(NULL);
    ps = thistask->tc_UserData;
    ph = &ps->ps_EventHandler;
    SetTaskPri(thistask, 0);

    if((ph->ph_MsgPort = CreateMsgPort())) {
        if((ph->ph_TimerMsgPort = CreateMsgPort())) {
            if((ph->ph_TimerIOReq = (struct timerequest *) CreateIORequest(ph->ph_TimerMsgPort, sizeof(struct timerequest)))) {
                if(!(OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *) ph->ph_TimerIOReq, 0))) {
                    ph->ph_TimerIOReq->tr_node.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
                    ph->ph_TimerIOReq->tr_node.io_Message.mn_Node.ln_Name = "EventHandler";
                    ph->ph_TimerIOReq->tr_node.io_Command = TR_ADDREQUEST;

                    ph->ph_EventHandler = psdAddEventHandler(ph->ph_MsgPort, EHMF_CONFIGCHG|EHMF_DEVICEDEAD|EHMF_DEVICELOWPW);
                    if(ph->ph_EventHandler) {
                        ph->ph_Task = thistask;
                        Forbid();
                        if(ph->ph_ReadySigTask) {
                            Signal(ph->ph_ReadySigTask, 1L<<ph->ph_ReadySignal);
                        }
                        Permit();

                        ph->ph_TimerIOReq->tr_time.tv_micro = 500*1000;
                        SendIO(&ph->ph_TimerIOReq->tr_node);
                        sigmask = (1UL<<ph->ph_MsgPort->mp_SigBit)|(1UL<<ph->ph_TimerMsgPort->mp_SigBit)|SIGBREAKF_CTRL_C;
                        counter = 0;
                        cfgchanged = 0;     /* tick of the first pending change + 1; 0 = none */
                        do {
                            if(ps->ps_CheckConfigReq) {
                                pCheckCfgChanged(ps);
                            }
                            if(ps->ps_LinkPowerReq) {
                                /* someone changed the link power policy; this
                                   task is the one that may block on the wire */
                                pLinkPowerSweep(ps);
                            }
                            if(ps->ps_StallRecoveryReq) {
                                /* a stalled endpoint was delivered whose class
                                   may never clear the halt (pMarkStalledPipe) */
                                pStallRecoverySweep(ps);
                            }
                            while((pen = (struct PsdEventNote *) GetMsg(ph->ph_MsgPort))) {
                                switch(pen->pen_Event) {
                                case EHMB_CONFIGCHG:
                                    if(!cfgchanged) {
                                        cfgchanged = counter + 1;
                                    }
                                    break;

                                case EHMB_DEVICEDEAD:
                                case EHMB_DEVICELOWPW:
                                    pAutoRecover(ps, (struct PsdDevice *) pen->pen_Param1);
                                    break;
                                }
                                ReplyMsg(&pen->pen_Msg);
                            }
                            if(CheckIO(&ph->ph_TimerIOReq->tr_node)) {
                                BOOL startnotify;
                                WaitIO(&ph->ph_TimerIOReq->tr_node);
                                ph->ph_TimerIOReq->tr_time.tv_micro = 500*1000;
                                SendIO(&ph->ph_TimerIOReq->tr_node);
                                counter++;
                                startnotify = !((counter & 3) || ps->ps_Notify.nt_Task);
                                if((ps->ps_GlobalCfg->pgc_PopupDeviceNew == PGCP_NEVER) &&
                                        (!ps->ps_GlobalCfg->pgc_PopupDeviceDeath) &&
                                        (!ps->ps_GlobalCfg->pgc_PopupDeviceGone)) {
                                    startnotify = FALSE; // no windows wanted, so no notifier
                                }
                                if(startnotify) {
                                    struct PsdNotify *ntf = &ps->ps_Notify;

                                    ntf->nt_ReadySignal = SIGB_SINGLE;
                                    ntf->nt_ReadySigTask = FindTask(NULL);
                                    SetSignal(0, SIGF_SINGLE); // clear single bit
                                    if(psdSpawnSubTask("Neptune Notifier", pNotifyGUITask, ps)) {
                                        Wait(1UL<<ntf->nt_ReadySignal);
                                    }
                                    ntf->nt_ReadySigTask = NULL;
                                    //FreeSignal(ntf->nt_ReadySignal);
                                    if(ntf->nt_Task) {
                                        psdAddErrorMsg0(RETURN_OK, (STRPTR) libname, "Notifier started.");
                                    }
                                }
                                /* two ticks after the first change */
                                if(cfgchanged && ((cfgchanged + 1) == counter)) {
                                    KPRINTF(10, ("Sending information about config changed to all classes.\n"));
                                    /* Inform all classes */
                                    psdLockReadPBase();
                                    puc = (struct PsdUsbClass *) ps->ps_Classes.lh_Head;
                                    while(puc->puc_Node.ln_Succ) {
                                        usbDoMethod(UCM_ConfigChangedEvent);
                                        puc = (struct PsdUsbClass *) puc->puc_Node.ln_Succ;
                                    }
                                    psdUnlockPBase();
                                    cfgchanged = 0;
                                }
                                // power saving stuff, check every second
                                if((counter & 1) && ps->ps_GlobalCfg->pgc_PowerSaving) {
                                    pIdleSuspendSweep(ps);
                                }
                            }
                            sigs = Wait(sigmask);
                        } while(!(sigs & SIGBREAKF_CTRL_C));
                        psdRemEventHandler(ph->ph_EventHandler);
                        ph->ph_EventHandler = NULL;
                        AbortIO(&ph->ph_TimerIOReq->tr_node);
                        WaitIO(&ph->ph_TimerIOReq->tr_node);
                    }
                    CloseDevice((struct IORequest *) ph->ph_TimerIOReq);
                }
                DeleteIORequest((struct IORequest *) ph->ph_TimerIOReq);
            }
            DeleteMsgPort(ph->ph_TimerMsgPort);
        }
        DeleteMsgPort(ph->ph_MsgPort);
        ph->ph_MsgPort = NULL;
    }
    Forbid();
    ph->ph_Task = NULL;
    if(ph->ph_ReadySigTask) {
        Signal(ph->ph_ReadySigTask, 1L<<ph->ph_ReadySignal);
    }
}
/* \\\ */

/*****************************************************************/

/* /// "Packtables for psdGetAttrs() and psdSetAttrs() " */
/* Pack table for PsdBase */
static const ULONG PsdBasePT[] = {
    PACK_STARTTABLE(PA_Dummy),
    PACK_ENTRY(PA_Dummy, PA_ConfigRead, PsdBase, ps_ConfigRead, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PA_Dummy, PA_GlobalConfig, PsdBase, ps_GlobalCfg, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PA_Dummy, PA_MemPoolUsage, PsdBase, ps_MemAllocated, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PA_Dummy, PA_CurrConfigHash, PsdBase, ps_ConfigHash, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PA_Dummy, PA_SavedConfigHash, PsdBase, ps_SavedConfigHash, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PA_Dummy, PA_ReleaseVersion, PsdBase, ps_ReleaseVersion, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PA_Dummy, PA_OSVersion, PsdBase, ps_OSVersion, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENDTABLE
};

/* Pack table for PsdErrorMsg */
static const ULONG PsdErrorMsgPT[] = {
    PACK_STARTTABLE(EMA_Dummy),
    PACK_ENTRY(EMA_Dummy, EMA_Level, PsdErrorMsg, pem_Level, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EMA_Dummy, EMA_Origin, PsdErrorMsg, pem_Origin, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EMA_Dummy, EMA_Msg, PsdErrorMsg, pem_Msg, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENDTABLE
};

/* Pack table for PsdUsbClass */
static const ULONG PsdUsbClassPT[] = {
    PACK_STARTTABLE(UCA_Dummy),
    PACK_ENTRY(UCA_Dummy, UCA_ClassBase, PsdUsbClass, puc_ClassBase, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(UCA_Dummy, UCA_ClassName, PsdUsbClass, puc_ClassName, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(UCA_Dummy, UCA_FullPath, PsdUsbClass, puc_FullPath, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(UCA_Dummy, UCA_UseCount, PsdUsbClass, puc_UseCnt, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENDTABLE
};

/* Pack table for PsdHardware */
static const ULONG PsdHardwarePT[] = {
    PACK_STARTTABLE(HA_Dummy),
    PACK_ENTRY(HA_Dummy, HA_DeviceName, PsdHardware, phw_DevName, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_DeviceUnit, PsdHardware, phw_Unit, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_ProductName, PsdHardware, phw_ProductName, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_Manufacturer, PsdHardware, phw_Manufacturer, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_Description, PsdHardware, phw_Description, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_Copyright, PsdHardware, phw_Copyright, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_Version, PsdHardware, phw_Version, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_Revision, PsdHardware, phw_Revision, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_DriverVersion, PsdHardware, phw_DriverVers, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_NumRootHubs, PsdHardware, phw_NumRootHubs, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(HA_Dummy, HA_ContextBackend, PsdHardware, phw_ContextBackend, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_StreamsSupported, PsdHardware, phw_StreamsSupported, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(HA_Dummy, HA_DMAAlignment, PsdHardware, phw_DMAAlignment, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENDTABLE
};

/* Pack table for PsdDevice */
static const ULONG PsdDevicePT[] = {
    PACK_STARTTABLE(DA_Dummy),
    PACK_ENTRY(DA_Dummy, DA_Address, PsdDevice, pd_DevAddr, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_NumConfigs, PsdDevice, pd_NumCfgs, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_CurrConfig, PsdDevice, pd_CurrCfg, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_Config, PsdDevice, pd_CurrentConfig, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_HubDevice, PsdDevice, pd_Hub, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_UsbVersion, PsdDevice, pd_USBVers, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_Class, PsdDevice, pd_DevClass, PKCTRL_WORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_SubClass, PsdDevice, pd_DevSubClass, PKCTRL_WORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_Protocol, PsdDevice, pd_DevProto, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_VendorID, PsdDevice, pd_VendorID, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_MaxPktSize0, PsdDevice, pd_MaxPktSize0, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_ProductID, PsdDevice, pd_ProductID, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_Version, PsdDevice, pd_DevVers, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_Manufacturer, PsdDevice, pd_MnfctrStr, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_ProductName, PsdDevice, pd_ProductStr, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_OrigProductName, PsdDevice, pd_OldProductStr, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_SerialNumber, PsdDevice, pd_SerNumStr, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_Hardware, PsdDevice, pd_Hardware, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_Binding, PsdDevice, pd_DevBinding, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_BindingClass, PsdDevice, pd_ClsBinding, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_LangIDArray, PsdDevice, pd_LangIDArray, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_CurrLangID, PsdDevice, pd_CurrLangID, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_IDString, PsdDevice, pd_IDString, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_CloneCount, PsdDevice, pd_CloneCount, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_AtHubPortNumber, PsdDevice, pd_HubPort, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_PowerDrained, PsdDevice, pd_PowerDrain, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_PowerSupply, PsdDevice, pd_PowerSupply, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_IsNewToMe, PsdDevice, pd_IsNewToMe, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DA_Dummy, DA_InhibitPopup, PsdDevice, pd_NotifyCfg.ntc_InhibitPopup, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_InhibitClassBind, PsdDevice, pd_NotifyCfg.ntc_NoClassBind, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_OverridePowerInfo, PsdDevice, pd_NotifyCfg.ntc_OverridePowerInfo, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_LinkPowerOverride, PsdDevice, pd_NotifyCfg.ntc_LinkPowerOverride, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_NoAutoSuspend, PsdDevice, pd_NotifyCfg.ntc_NoAutoSuspend, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_HubThinkTime, PsdDevice, pd_HubThinkTime, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_HubNumPorts, PsdDevice, pd_HubNumPorts, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_HubHdrDecLat, PsdDevice, pd_HubHdrDecLat, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_HubDelay, PsdDevice, pd_HubDelay, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(DA_Dummy, DA_HasContainerId, PsdDevice, pd_HasContainerId, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_WORDBIT(DA_Dummy, DA_IsLowspeed, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_LOWSPEED),
    PACK_WORDBIT(DA_Dummy, DA_IsHighspeed, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_HIGHSPEED),
    PACK_WORDBIT(DA_Dummy, DA_IsConnected, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_CONNECTED),
    PACK_WORDBIT(DA_Dummy, DA_HasAddress, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_HASDEVADDR),
    PACK_WORDBIT(DA_Dummy, DA_HasDevDesc, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_HASDEVDESC),
    PACK_WORDBIT(DA_Dummy, DA_IsConfigured, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_CONFIGURED),
    PACK_WORDBIT(DA_Dummy, DA_IsDead, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_UNPACKONLY, PDFF_DEAD),
    PACK_WORDBIT(DA_Dummy, DA_IsSuspended, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_SUSPENDED),
    PACK_WORDBIT(DA_Dummy, DA_HasAppBinding, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_UNPACKONLY, PDFF_APPBINDING),
    PACK_WORDBIT(DA_Dummy, DA_NeedsSplitTrans, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_NEEDSSPLIT),
    PACK_WORDBIT(DA_Dummy, DA_LowPower, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_UNPACKONLY, PDFF_LOWPOWER),
    PACK_WORDBIT(DA_Dummy, DA_IsSuperspeed, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_SUPERSPEED),
    PACK_WORDBIT(DA_Dummy, DA_IsMultiTT, PsdDevice, pd_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PDFF_MULTITT),
    PACK_ENDTABLE
};

/* Pack table for PsdConfig */
static const ULONG PsdConfigPT[] = {
    PACK_STARTTABLE(CA_Dummy),
    PACK_ENTRY(CA_Dummy, CA_ConfigNum, PsdConfig, pc_CfgNum, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(CA_Dummy, CA_MaxPower, PsdConfig, pc_MaxPower, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(CA_Dummy, CA_ConfigName, PsdConfig, pc_CfgStr, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(CA_Dummy, CA_NumInterfaces, PsdConfig, pc_NumIfs, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(CA_Dummy, CA_Attrs, PsdConfig, pc_Attr, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(CA_Dummy, CA_Device, PsdConfig, pc_Device, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_WORDBIT(CA_Dummy, CA_SelfPowered, PsdConfig, pc_Attr, PKCTRL_BIT|PKCTRL_PACKUNPACK, USCAF_SELF_POWERED),
    PACK_WORDBIT(CA_Dummy, CA_RemoteWakeup, PsdConfig, pc_Attr, PKCTRL_BIT|PKCTRL_UNPACKONLY, USCAF_REMOTE_WAKEUP),
    PACK_ENDTABLE
};

/* Pack table for PsdDescriptor */
static const ULONG PsdDescriptorPT[] = {
    PACK_STARTTABLE(DDA_Dummy),
    PACK_ENTRY(DDA_Dummy, DDA_Device, PsdDescriptor, pdd_Device, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_Config, PsdDescriptor, pdd_Config, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_Interface, PsdDescriptor, pdd_Interface, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_Endpoint, PsdDescriptor, pdd_Endpoint, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_Name, PsdDescriptor, pdd_Name, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_DescriptorType, PsdDescriptor, pdd_Type, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_CS_SubType, PsdDescriptor, pdd_CSSubType, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_DescriptorData, PsdDescriptor, pdd_Data, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(DDA_Dummy, DDA_DescriptorLength, PsdDescriptor, pdd_Length, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENDTABLE
};

/* Pack table for PsdInterface */
static const ULONG PsdInterfacePT[] = {
    PACK_STARTTABLE(IFA_Dummy),
    PACK_ENTRY(IFA_Dummy, IFA_InterfaceNum, PsdInterface, pif_IfNum, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_AlternateNum, PsdInterface, pif_Alternate, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_NumEndpoints, PsdInterface, pif_NumEPs, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_Class, PsdInterface, pif_IfClass, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_SubClass, PsdInterface, pif_IfSubClass, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_Protocol, PsdInterface, pif_IfProto, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_InterfaceName, PsdInterface, pif_IfStr, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_Config, PsdInterface, pif_Config, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(IFA_Dummy, IFA_Binding, PsdInterface, pif_IfBinding, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(IFA_Dummy, IFA_BindingClass, PsdInterface, pif_ClsBinding, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(IFA_Dummy, IFA_IDString, PsdInterface, pif_IDString, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENDTABLE
};

/* Pack table for PsdEndpoint */
static const ULONG PsdEndpointPT[] = {
    PACK_STARTTABLE(EA_Dummy),
    PACK_ENTRY(EA_Dummy, EA_EndpointNum, PsdEndpoint, pep_EPNum, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_TransferType, PsdEndpoint, pep_TransType, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_MaxPktSize, PsdEndpoint, pep_MaxPktSize, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_Interval, PsdEndpoint, pep_Interval, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_NumTransMuFrame, PsdEndpoint, pep_NumTransMuFr, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_SyncType, PsdEndpoint, pep_SyncType, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_UsageType, PsdEndpoint, pep_UsageType, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_MaxBurst, PsdEndpoint, pep_MaxBurst, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_CompAttributes, PsdEndpoint, pep_CompAttributes, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_BytesPerInterval, PsdEndpoint, pep_BytesPerInterval, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_StreamBase, PsdEndpoint, pep_StreamBase, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(EA_Dummy, EA_MaxStreams, PsdEndpoint, pep_MaxStreams, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_StreamsAlloc, PsdEndpoint, pep_StreamsAlloc, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(EA_Dummy, EA_Interface, PsdEndpoint, pep_Interface, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_WORDBIT(EA_Dummy, EA_IsIn, PsdEndpoint, pep_Direction, PKCTRL_BIT|PKCTRL_UNPACKONLY, 1),
    PACK_ENDTABLE
};

/* Pack table for PsdPipe */
static const ULONG PsdPipePT[] = {
    PACK_STARTTABLE(PPA_Dummy),
    PACK_ENTRY(PPA_Dummy, PPA_Endpoint, PsdPipe, pp_Endpoint, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PPA_Dummy, PPA_Error, PsdPipe, pp_IOReq.iouh_Req.io_Error, PKCTRL_BYTE|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PPA_Dummy, PPA_Actual, PsdPipe, pp_IOReq.iouh_Actual, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PPA_Dummy, PPA_EndpointNum, PsdPipe, pp_IOReq.iouh_Endpoint, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PPA_Dummy, PPA_DeviceAddress, PsdPipe, pp_IOReq.iouh_DevAddr, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PPA_Dummy, PPA_MaxPktSize, PsdPipe, pp_IOReq.iouh_MaxPktSize, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PPA_Dummy, PPA_NakTimeoutTime, PsdPipe, pp_IOReq.iouh_NakTimeout, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PPA_Dummy, PPA_Interval, PsdPipe, pp_IOReq.iouh_Interval, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PPA_Dummy, PPA_StreamID, PsdPipe, pp_StreamID, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_WORDBIT(PPA_Dummy, PPA_NoShortPackets, PsdPipe, pp_IOReq.iouh_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, UHFF_NOSHORTPKT),
    PACK_WORDBIT(PPA_Dummy, PPA_NakTimeout, PsdPipe, pp_IOReq.iouh_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, UHFF_NAKTIMEOUT),
    PACK_WORDBIT(PPA_Dummy, PPA_AllowRuntPackets, PsdPipe, pp_IOReq.iouh_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, UHFF_ALLOWRUNTPKTS),
    PACK_ENDTABLE
};

/* Pack table for PsdAppBinding */
static const ULONG PsdAppBindingPT[] = {
    PACK_STARTTABLE(ABA_Dummy),
    PACK_ENTRY(ABA_Dummy, ABA_ReleaseHook, PsdAppBinding, pab_ReleaseHook, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(ABA_Dummy, ABA_Device, PsdAppBinding, pab_Device, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(ABA_Dummy, ABA_UserData, PsdAppBinding, pab_UserData, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(ABA_Dummy, ABA_Task, PsdAppBinding, pab_Task, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(ABA_Dummy, ABA_ForceRelease, PsdAppBinding, pab_ForceRelease, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENDTABLE
};

/* Pack table for PsdAppBinding */
static const ULONG PsdEventNotePT[] = {
    PACK_STARTTABLE(ENA_Dummy),
    PACK_ENTRY(ENA_Dummy, ENA_EventID, PsdEventNote, pen_Event, PKCTRL_UWORD|PKCTRL_UNPACKONLY),
    PACK_ENTRY(ENA_Dummy, ENA_Param1, PsdEventNote, pen_Param1, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENTRY(ENA_Dummy, ENA_Param2, PsdEventNote, pen_Param2, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_ENDTABLE
};

/* Pack table for PsdGlobalCfg */
static const ULONG PsdGlobalCfgPT[] = {
    PACK_STARTTABLE(GCA_Dummy),
    PACK_ENTRY(GCA_Dummy, GCA_LogInfo, PsdGlobalCfg, pgc_LogInfo, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_LogWarning, PsdGlobalCfg, pgc_LogWarning, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_LogError, PsdGlobalCfg, pgc_LogError, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_LogFailure, PsdGlobalCfg, pgc_LogFailure, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_BootDelay, PsdGlobalCfg, pgc_BootDelay, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_SubTaskPri, PsdGlobalCfg, pgc_SubTaskPri, PKCTRL_WORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PopupDeviceNew, PsdGlobalCfg, pgc_PopupDeviceNew, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PopupDeviceGone, PsdGlobalCfg, pgc_PopupDeviceGone, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PopupDeviceDeath, PsdGlobalCfg, pgc_PopupDeviceDeath, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PopupCloseDelay, PsdGlobalCfg, pgc_PopupCloseDelay, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PopupActivateWin, PsdGlobalCfg, pgc_PopupActivateWin, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PopupWinToFront, PsdGlobalCfg, pgc_PopupWinToFront, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_AutoDisableLP, PsdGlobalCfg, pgc_AutoDisableLP, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_AutoDisableDead, PsdGlobalCfg, pgc_AutoDisableDead, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_AutoRestartDead, PsdGlobalCfg, pgc_AutoRestartDead, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PowerSaving, PsdGlobalCfg, pgc_PowerSaving, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_ForceSuspend, PsdGlobalCfg, pgc_ForceSuspend, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_SuspendTimeout, PsdGlobalCfg, pgc_SuspendTimeout, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_LinkPowerMgmt, PsdGlobalCfg, pgc_LinkPowerMgmt, PKCTRL_UWORD|PKCTRL_PACKUNPACK),
    PACK_ENTRY(GCA_Dummy, GCA_PrefsVersion, PsdGlobalCfg, pgc_PrefsVersion, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENDTABLE
};

/* Pack table for PsdPipeStream */
static const ULONG PsdPipeStreamPT[] = {
    PACK_STARTTABLE(PSA_Dummy),
    PACK_ENTRY(PSA_Dummy, PSA_MessagePort, PsdPipeStream, pps_MsgPort, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PSA_Dummy, PSA_NumPipes, PsdPipeStream, pps_NumPipes, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PSA_Dummy, PSA_BufferSize, PsdPipeStream, pps_BufferSize, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PSA_Dummy, PSA_NakTimeoutTime, PsdPipeStream, pps_NakTimeoutTime, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PSA_Dummy, PSA_BytesPending, PsdPipeStream, pps_BytesPending, PKCTRL_ULONG|PKCTRL_UNPACKONLY),
    PACK_ENTRY(PSA_Dummy, PSA_Error, PsdPipeStream, pps_Error, PKCTRL_LONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PSA_Dummy, PSA_TermArray, PsdPipeStream, pps_TermArray, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PSA_Dummy, PSA_AbortSigMask, PsdPipeStream, pps_AbortSigMask, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_ENTRY(PSA_Dummy, PSA_ActivePipe, PsdPipeStream, pps_ActivePipe, PKCTRL_IPTR|PKCTRL_UNPACKONLY),
    PACK_WORDBIT(PSA_Dummy, PSA_AsyncIO, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_ASYNCIO),
    PACK_WORDBIT(PSA_Dummy, PSA_ShortPktTerm, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_SHORTTERM),
    PACK_WORDBIT(PSA_Dummy, PSA_ReadAhead, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_READAHEAD),
    PACK_WORDBIT(PSA_Dummy, PSA_BufferedRead, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_BUFFERREAD),
    PACK_WORDBIT(PSA_Dummy, PSA_BufferedWrite, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_BUFFERWRITE),
    PACK_WORDBIT(PSA_Dummy, PSA_NoZeroPktTerm, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_NOSHORTPKT),
    PACK_WORDBIT(PSA_Dummy, PSA_NakTimeout, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_NAKTIMEOUT),
    PACK_WORDBIT(PSA_Dummy, PSA_AllowRuntPackets, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_ALLOWRUNT),
    PACK_WORDBIT(PSA_Dummy, PSA_DoNotWait, PsdPipeStream, pps_Flags, PKCTRL_BIT|PKCTRL_PACKUNPACK, PSFF_DONOTWAIT),
    PACK_ENDTABLE
};

/* Pack table for PsdRTIsoHandler */
static const ULONG PsdRTIsoHandlerPT[] = {
    PACK_STARTTABLE(RTA_Dummy),
    PACK_ENTRY(RTA_Dummy, RTA_InRequestHook, PsdRTIsoHandler, prt_RTIso.urti_InReqHook, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(RTA_Dummy, RTA_OutRequestHook, PsdRTIsoHandler, prt_RTIso.urti_OutReqHook, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(RTA_Dummy, RTA_InDoneHook, PsdRTIsoHandler, prt_RTIso.urti_InDoneHook, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(RTA_Dummy, RTA_OutDoneHook, PsdRTIsoHandler, prt_RTIso.urti_OutDoneHook, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(RTA_Dummy, RTA_ReleaseHook, PsdRTIsoHandler, prt_ReleaseHook, PKCTRL_IPTR|PKCTRL_PACKUNPACK),
    PACK_ENTRY(RTA_Dummy, RTA_OutPrefetchSize, PsdRTIsoHandler, prt_RTIso.urti_OutPrefetch, PKCTRL_ULONG|PKCTRL_PACKUNPACK),
    PACK_WORDBIT(RTA_Dummy, RTA_ReportInErrors, PsdRTIsoHandler, prt_HookFlags, PKCTRL_BIT|PKCTRL_PACKUNPACK, UHCD_IHF_IN_ERRORS),
    PACK_ENDTABLE
};

/* PGA assignment table */
static const ULONG * const PsdPTArray[] = {
    NULL,
    PsdBasePT,
    PsdUsbClassPT,
    PsdHardwarePT,
    PsdDevicePT,
    PsdConfigPT,
    PsdInterfacePT,
    PsdEndpointPT,
    PsdErrorMsgPT,
    PsdPipePT,
    PsdAppBindingPT,
    PsdEventNotePT,
    PsdGlobalCfgPT,
    PsdPipeStreamPT,
    PsdDescriptorPT,
    PsdRTIsoHandlerPT
};
/* \\\ */

/* --- LVO jump table ------------------------------------------------------------------- *\
 *
 * Deliberately here, at the end of the TU that DEFINES every psd* function, rather than
 * next to the romtag in neptune_main.c: there it needed <clib/neptune_protos.h> for the
 * addresses, and those public prototypes omit the a6 libbase that every implementation
 * takes (APTR psdAllocVec(ULONG) vs APTR psdAllocVec(ULONG asm("d0"), struct PsdBase * asm("a6"))).
 * Pre-LTO the two never met; with LTO the compiler sees both TUs and it is
 * -Wlto-type-mismatch on every one of them -- the same conflict neptune.library.h warns
 * about for <proto/neptune.h>.  Below the definitions, no declarations are needed at all.
 *
 * Order IS the ABI: 4 std vectors, then neptune.sfd order (neptune_funcs.inc -- hand
 * maintained, so a new LVO must be appended to BOTH or clients' calls land on the
 * terminator), then the -1 terminator.
 */
extern struct PsdBase *LibOpen(struct PsdBase *base asm("a6"));
extern BPTR LibClose(struct PsdBase *base asm("a6"));
extern BPTR LibExpunge(struct PsdBase *base asm("a6"));
extern ULONG LibNull(void);

const APTR psdFuncTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
#include "neptune_funcs.inc"
    (APTR)-1
};
/* \\\ */
