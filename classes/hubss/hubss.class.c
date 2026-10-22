/*
    Copyright (C) 2014-2025, The AROS Development Team. All rights reserved.

    Desc: SuperSpeed USB3.0 hub class for Neptune, based on the Poseidon hubss class
          (itself based upon hub.class.c by Chris Hodges <chrisly@platon42.de>)
*/

#include "debug.h"

#include <proto/neptune.h>

#include <devices/usb.h>
#include <devices/usb_hub.h>
#include <devices/usbhardware.h>
#include <libraries/neptune_class.h>

#include "common.h"
#include "hubss.class.h"

/* Cross-class dispatch (evicting the USB2 twin of an SS device via the peer
 * hub's class): only the inline LVO macros - the clib prototype (2 args) would
 * clash with our own 3-argument usbDoMethodA export below. NEPTUNE_CLASS_BASE_NAME
 * defaults to UsbClsBase, a local in nNotifyPeerTwinEvict(). */
#include <inline/neptune_class.h>

struct NepClassHubSS * usbAttemptDeviceBinding(struct NepHubSSBase *nh, struct PsdDevice *pd);
struct NepClassHubSS * usbForceDeviceBinding(struct NepHubSSBase * nh, struct PsdDevice *pd);
void usbReleaseDeviceBinding(struct NepHubSSBase *nh, struct NepClassHubSS *nch);

struct NepClassHubSS * nAllocHub(void);
void nFreeHub(struct NepClassHubSS *nch);
struct PsdDevice * nFindPeerHub(struct NepClassHubSS *nch);
void nNotifyPeerTwinEvict(struct NepClassHubSS *nch, UWORD port);
BOOL nPortShadowedByPeer(struct NepClassHubSS *nch, UWORD port);
BOOL nConnectShadowDebounce(struct NepClassHubSS *nch, UWORD port);
struct PsdDevice * nConfigurePort(struct NepClassHubSS *nch, UWORD port);
LONG nReadPortStatus(struct NepClassHubSS *nch, UWORD port, struct UsbPortStatus *uhps);
LONG nClearPortStatus(struct NepClassHubSS *nch, UWORD port);
LONG nWarmResetPort(struct NepClassHubSS *nch, UWORD port);
BOOL nHubSuspendDevice(struct NepClassHubSS *nch, struct PsdDevice *pd);
BOOL nHubResumeDevice(struct NepClassHubSS *nch, struct PsdDevice *pd);
BOOL nHubResetPort(struct NepClassHubSS *nch, struct PsdDevice *pd);
void nHandleHubMethod(struct NepClassHubSS *nch, struct NepHubSSMsg *nhm);
void nHubssTask();

/* /// "Lib Stuff" */
static const STRPTR libname = CLASS_NAME;
static const STRPTR hubunknown = "unknown hub";
static const STRPTR devunknown = "unknown device";

int libInit(struct NepHubSSBase * nh) {
    KPRINTF(1, ("%s()\n", __func__));
    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);
#define UtilityBase nh->nh_UtilityBase
    if(UtilityBase) {
        NEWLIST(&nh->nh_Bindings);
        return TRUE;
    }
    KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
    return FALSE;
}

int libExpunge(struct NepHubSSBase * nh) {
    KPRINTF(1, ("libExpunge nh: 0x%08lx\n", nh));
    CloseLibrary(UtilityBase);
    nh->nh_UtilityBase = NULL;
    return TRUE;
}


/* \\\ */

/*
 * ***********************************************************************
 * * Library functions                                                   *
 * ***********************************************************************
 */

/* /// "usbAttemptDeviceBinding()" */
struct NepClassHubSS * usbAttemptDeviceBinding(struct NepHubSSBase *nh, struct PsdDevice *pd) {
    struct Library *ps;
    IPTR devclass;
    IPTR issuperspeed = 0;

    KPRINTF(1, ("%s(0x%08lx)\n", __func__, pd));

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION))) {
        psdGetAttrs(PGA_DEVICE, pd, DA_Class, &devclass, DA_IsSuperspeed, &issuperspeed, TAG_DONE);
        CloseLibrary(ps);

        if((devclass == HUB_CLASSCODE) && (issuperspeed)) {
            return(usbForceDeviceBinding(nh, pd));
        }
    }
    return(NULL);
}

/* /// "usbForceDeviceBinding()" */
struct NepClassHubSS * usbForceDeviceBinding(struct NepHubSSBase * nh, struct PsdDevice *pd) {
    struct Library *ps;
    struct NepClassHubSS *nch;
    STRPTR devname;
    char buf[64];
    struct Task *tmptask;

    KPRINTF(1, ("%s(0x%08lx)\n", __func__, pd));

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION))) {
        psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_DONE);
        if(!devname) devname = hubunknown;

        if((nch = psdAllocVec(sizeof(struct NepClassHubSS)))) {
            nch->nch_HubBase = nh;
            nch->nch_Device = pd;
            nch->nch_Settling = TRUE;
            psdSafeRawDoFmt(buf, 64, CLASS_NAME "<0x%08lx>", nch);

            nch->nch_ReadySignal = SIGB_SINGLE;
            nch->nch_ReadySigTask = FindTask(NULL);
            SetSignal(0, SIGF_SINGLE);

            if((tmptask = psdSpawnSubTask(buf, nHubssTask, nch))) {
                psdBorrowLocksWait(tmptask, 1UL<<nch->nch_ReadySignal);

                if(nch->nch_Task) {
                    nch->nch_ReadySigTask = NULL;
                    //FreeSignal(nch->nch_ReadySignal);
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "SuperSpeed hub '%s' configured.", devname);

                    Forbid();
                    AddTail(&nh->nh_Bindings, &nch->nch_Node);
                    Permit();
                    CloseLibrary(ps);
                    return(nch);
                }
            }

            nch->nch_ReadySigTask = NULL;
            //FreeSignal(nch->nch_ReadySignal);
            psdFreeVec(nch);
        }

        CloseLibrary(ps);
    }

    return(NULL);
}

/* /// "usbReleaseDeviceBinding()" */
void usbReleaseDeviceBinding(struct NepHubSSBase *nh, struct NepClassHubSS *nch) {
    struct Library *ps;
    STRPTR devname;

    KPRINTF(1, ("%s(0x%08lx, 0x%08lx)\n", __func__, nh, nch));

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION))) {

        Forbid();
        nch->nch_ReadySignal = SIGB_SINGLE;
        nch->nch_ReadySigTask = FindTask(NULL);
        if(nch->nch_Task) {
            KPRINTF(1, ("Sending Break\n"));
            Signal(nch->nch_Task, SIGBREAKF_CTRL_C);
        }
        Permit();

        while(nch->nch_Task) {
            psdBorrowLocksWait(nch->nch_Task, 1UL<<nch->nch_ReadySignal);
        }
        KPRINTF(1, ("Task gone\n"));

        //FreeSignal(nch->nch_ReadySignal);
        psdGetAttrs(PGA_DEVICE, nch->nch_Device, DA_ProductName, &devname, TAG_END);
        if(!devname) devname = hubunknown;
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname, NEP_RELEASED_TXT, devname);

        Forbid();
        Remove(&nch->nch_Node);
        Permit();

        psdFreeVec(nch);
        CloseLibrary(ps);
    }
}

/* /// "usbGetAttrsA()" */
LONG (usbGetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * taglist asm("a1"), struct NepHubSSBase * nh asm("a6")) {

    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("%s(%ld, 0x%08lx, 0x%08lx)\n", __func__, type, usbstruct, taglist));

    switch(type) {
        case UGA_CLASS:
            while((ti = NextTagItem(&taglist)) != NULL) {
                switch (ti->ti_Tag) {
                    case UCCA_Priority:
                        *((SIPTR *) ti->ti_Data) = 1;
                        count++;
                        break;
                    case UCCA_Description:
                        *((STRPTR *) ti->ti_Data) = "Root/external superspeed hub base class";
                        count++;
                        break;
                    case UCCA_HasClassCfgGUI:
                        *((IPTR *) ti->ti_Data) = FALSE;
                        count++;
                        break;
                    case UCCA_HasBindingCfgGUI:
                        *((IPTR *) ti->ti_Data) = FALSE;
                        count++;
                        break;
                    case UCCA_AfterDOSRestart:
                        *((IPTR *) ti->ti_Data) = FALSE;
                        count++;
                        break;
                    case UCCA_UsingDefaultCfg:
                        *((IPTR *) ti->ti_Data) = TRUE;
                        count++;
                        break;
                    case UCCA_SupportsSuspend:
                        *((IPTR *) ti->ti_Data) = TRUE;
                        count++;
                        break;
                } /* switch (ti->ti_Tag) */
            }; /* while((ti = NextTagItem(&taglist)) != NULL) */
            break;

         case UGA_BINDING:
             if((ti = FindTagItem(UCBA_UsingDefaultCfg, taglist))) {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             break;
    }

    return(count);
}

/* /// "usbSetAttrsA()" */
LONG (usbSetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepHubSSBase * nh asm("a6")) {

    KPRINTF(1, ("%s(%ld, 0x%08lx, 0x%08lx)\n", __func__, type, usbstruct, tags));

    return(0);

}

/* /// "nHubMethodWait()" */
/*
 * Waiting for the hub task to answer a method (see usbDoMethodA()).
 *
 * The deadlock this guards against:
 *
 *   hub task:    sees a device unplugged, frees it  -> waits for the device's
 *                class task to end (the class's release does that)
 *   class task:  is recovering the same device and has just asked the hub
 *                for a port reset                   -> waits for the hub task
 *
 * Neither moves again, and the hub takes no further device.  The way out is
 * the class task's: its release sends it CTRL-C, so a request that is still
 * lying in the hub task's port when CTRL-C arrives is taken back, and the
 * method fails as if the hub had refused it.
 */

/* Take our request back out of the hub task's port.  FALSE = it is not there
   any more: the hub task is working on it and will answer. */
static BOOL nHubMethodTakeBack(struct NepClassHubSS *nch, struct NepHubSSMsg *nhm)
{
    struct Node *node;
    BOOL found = FALSE;

    Forbid();
    for(node = nch->nch_CtrlMsgPort->mp_MsgList.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        if(node == &nhm->nhm_Msg.mn_Node) {
            Remove(node);
            found = TRUE;
            break;
        }
    }
    Permit();
    return(found);
}

/* cancellable: the method may fail anyway, so CTRL-C may end the wait.  The
   result stays in nhm (0 if the request was taken back). */
static void nHubMethodWait(struct NepClassHubSS *nch, struct NepHubSSMsg *nhm, BOOL cancellable, struct Library *ps)
{
    struct MsgPort *replyport = nhm->nhm_Msg.mn_ReplyPort;
    ULONG replysig = 1UL<<replyport->mp_SigBit;

    if(cancellable) {
        /* first wait: for the answer, or for being told to quit */
        if(psdBorrowLocksWait(nch->nch_Task, replysig | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            /* Wait() consumed the signal; it is the caller's to act on */
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            if(nHubMethodTakeBack(nch, nhm)) {
                return;
            }
        }
    }
    /* answered already, or the hub task is working on it: wait it out */
    while(!GetMsg(replyport)) {
        psdBorrowLocksWait(nch->nch_Task, replysig);
    }
}
/* \\\ */

/* /// "usbDoMethodA()" */
IPTR (usbDoMethodA)(ULONG methodid asm("d0"), IPTR * methoddata asm("a1"), struct NepHubSSBase * nh asm("a6")) {

    struct NepClassHubSS *nch;

    KPRINTF(1, ("%s(%ld)\n", __func__, methodid));

    switch(methodid) {
        case UCM_AttemptDeviceBinding:
            return((IPTR) usbAttemptDeviceBinding(nh, (struct PsdDevice *) methoddata[0]));

        case UCM_ForceDeviceBinding:
            return((IPTR) usbForceDeviceBinding(nh, (struct PsdDevice *) methoddata[0]));

        case UCM_ReleaseDeviceBinding:
            usbReleaseDeviceBinding(nh, (struct NepClassHubSS *) methoddata[0]);
            return(TRUE);

        case UCM_HubPowerCyclePort:
        case UCM_HubDisablePort: {
            struct PsdDevice *pd = (struct PsdDevice *) methoddata[0];
            ULONG port = (ULONG) methoddata[1];

            if(!(pd && port)) {
                KPRINTF(20, ("HubPowerCycle/DisablePort Params Null!\n"));
                return(FALSE);
            }

            Forbid();
            nch = (struct NepClassHubSS *) nh->nh_Bindings.lh_Head;
            while(nch->nch_Node.ln_Succ) {
                if(nch->nch_Device == pd) {
                    KPRINTF(20, ("HubPowerCycle/DisablePort Dev found (port %ld)!\n", port));
                    if(port <= nch->nch_NumPorts) {
                        nch->nch_DisablePort |= 1UL<<port;
                        nch->nch_Settling = TRUE;
                        if(methodid == UCM_HubPowerCyclePort) {
                            nch->nch_PowerCycle |= 1UL<<port;
                        }
                        if(nch->nch_Task) {
                            Signal(nch->nch_Task, (1L<<nch->nch_TaskMsgPort->mp_SigBit));
                        }
                        Permit();
                        return(TRUE);
                    }
                    break;
                }
                nch = (struct NepClassHubSS *) nch->nch_Node.ln_Succ;
            }
            Permit();

            return(FALSE);
            } /* case UCM_HubDisablePort */

        case UCM_HubClassScan: {
            nch = (struct NepClassHubSS *) methoddata[0];

            Forbid();
            nch->nch_ClassScan = TRUE;
            nch->nch_Settling = TRUE;
            if(nch->nch_Task) {
                Signal(nch->nch_Task, (1L<<nch->nch_TaskMsgPort->mp_SigBit));
            }
            Permit();

            return(TRUE);
            } /* case UCM_HubClassScan */

        case UCM_AttemptSuspendDevice:
        case UCM_AttemptResumeDevice:
        case UCM_HubClaimAppBinding:
        case UCM_HubReleaseIfBinding:
        case UCM_HubReleaseDevBinding:
        case UCM_HubSuspendDevice:
        case UCM_HubResumeDevice:
        case UCM_HubResetPort: {
            struct NepHubSSMsg nhm;
            struct Library *ps;
            nch = (struct NepClassHubSS *) methoddata[0];
            nhm.nhm_Result = (IPTR) NULL;
            nhm.nhm_MethodID = methodid;
            nhm.nhm_Params = methoddata;

            if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION))) {
                if(nch->nch_Task == FindTask(NULL)) {
                    // if we would send the message to ourself, we would deadlock, so handle this directly
                    nHandleHubMethod(nch, &nhm);
                } else {
                    nhm.nhm_Msg.mn_ReplyPort = CreateMsgPort();
                    nhm.nhm_Msg.mn_Length = sizeof(struct NepHubSSMsg);

                    Forbid();
                    if(nch->nch_Task && nhm.nhm_Msg.mn_ReplyPort) {
                        PutMsg(nch->nch_CtrlMsgPort, &nhm.nhm_Msg);
                        Permit();

                        /* cancellable: the three a class task asks for about
                           its own device, all of which may fail anyway */
                        nHubMethodWait(nch, &nhm,
                                       (methodid == UCM_HubResetPort) ||
                                       (methodid == UCM_HubSuspendDevice) ||
                                       (methodid == UCM_HubResumeDevice), ps);
                    } else {
                        Permit();
                    }
                    if(nhm.nhm_Msg.mn_ReplyPort)
                    {
                        DeleteMsgPort(nhm.nhm_Msg.mn_ReplyPort);
                    }
                }
                CloseLibrary(ps);
            }

            return(nhm.nhm_Result);
            }/* case UCM_HubResumeDevice */

        /* Hubs still working a port pass, for the ROM boot gate. Forbid()
           rather than a lock: the caller polls from the coldstart chain and a
           hub task must never be made to wait on it. */
        case UCM_PortsPending: {
            IPTR pending = 0;

            Forbid();
            nch = (struct NepClassHubSS *) nh->nh_Bindings.lh_Head;
            while(nch->nch_Node.ln_Succ) {
                if(nch->nch_Settling) {
                    pending++;
                }
                nch = (struct NepClassHubSS *) nch->nch_Node.ln_Succ;
            }
            Permit();

            return(pending);
            } /* case UCM_PortsPending */

        default:
            break;
    }

    return(0);
}

#undef ps
#define ps nch->nch_Base

/* /// "nFindPeerHub()" */
/* Find the other half of the same physical USB3 hub: another bound hub device
 * carrying the identical BOS Container ID but the opposite half role (the spec
 * requires both halves to report the same Container ID, and identical port
 * numbering). VID/PID are deliberately not compared - the halves legitimately
 * differ.
 * Caller must hold psdLockReadPBase(). */
struct PsdDevice * nFindPeerHub(struct NepClassHubSS *nch)
{
    if(!nch->nch_ContainerId) {
        return(NULL);
    }
    struct PsdDevice *pd = NULL;
    while((pd = psdGetNextDevice(pd))) {
        if(pd == nch->nch_Device) {
            continue;
        }
        IPTR devclass = 0;
        IPTR isconnected = 0;
        IPTR isdead = 0;
        IPTR proto = 0;
        IPTR issuperspeed = 0;
        APTR binding = NULL;
        UBYTE *containerid = NULL;
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_Class, &devclass,
                    DA_IsConnected, &isconnected,
                    DA_IsDead, &isdead,
                    DA_Binding, &binding,
                    DA_Protocol, &proto,
                    DA_IsSuperspeed, &issuperspeed,
                    DA_ContainerId, &containerid,
                    TAG_END);
        if((devclass != HUB_CLASSCODE) || (!isconnected) || isdead || (!binding) || (!containerid)) {
            continue;
        }
        BOOL peerisss = (proto == 3) || issuperspeed;
        if((peerisss == nch->nch_IsSSHalf) || memcmp(containerid, nch->nch_ContainerId, 16)) {
            continue;
        }
        return(pd);
    }
    return(NULL);
}
/* \\\ */

/* /// "nNotifyPeerTwinEvict()" */
/* SS half only: after enumerating a child on a port, evict any USB2 twin the
 * companion 2.0 half may have captured on the same physical connector. Sent
 * unconditionally (not only when a twin is visible) - the twin may still be
 * mid-enumeration; the persistent nch_DisablePort bit is what closes that
 * race. Deliberately does not touch PORT_POWER: Vbus is one shared rail per
 * connector, unpowering via the 2.0 half could brown-out the SS link. */
void nNotifyPeerTwinEvict(struct NepClassHubSS *nch, UWORD port)
{
    if((!nch->nch_IsSSHalf) || (!nch->nch_ContainerId)) {
        return;
    }
    psdLockReadPBase();
    struct PsdDevice *peerpd = nFindPeerHub(nch);
    if(peerpd) {
        APTR binding = NULL;
        APTR puc = NULL;
        struct Library *UsbClsBase = NULL;
        Forbid();
        psdGetAttrs(PGA_DEVICE, peerpd,
                    DA_Binding, &binding,
                    DA_BindingClass, &puc,
                    TAG_END);
        if(binding && puc) {
            psdGetAttrs(PGA_USBCLASS, puc, UCA_ClassBase, &UsbClsBase, TAG_END);
        }
        if(UsbClsBase) {
            KPRINTF(2, ("Evicting USB2 twin at peer hub 0x%08lx port %ld\n", peerpd, port));
            usbDoMethod(UCM_HubDisablePort, peerpd, (IPTR) port);
        }
        Permit();
    }
    psdUnlockPBase();
}
/* \\\ */

/* /// "nPortShadowedByPeer()" */
/* 2.0 half only: TRUE when the SS half of the same physical hub already has
 * (or is currently enumerating - hub/port attrs are set before reset) a child
 * on the same connector. The USB2 presence is then just the twin of the SS
 * device and must not be enumerated. (hubss.usbclass is always the SS half, so
 * this self-gates to FALSE; kept for clone parity with hub.class.) */
BOOL nPortShadowedByPeer(struct NepClassHubSS *nch, UWORD port)
{
    if(nch->nch_IsSSHalf || (!nch->nch_ContainerId)) {
        return(FALSE);
    }
    BOOL shadowed = FALSE;
    psdLockReadPBase();
    struct PsdDevice *peerpd = nFindPeerHub(nch);
    if(peerpd) {
        struct PsdDevice *pd = NULL;
        while((pd = psdGetNextDevice(pd))) {
            APTR hubpd = NULL;
            IPTR hubport = 0;
            psdGetAttrs(PGA_DEVICE, pd,
                        DA_HubDevice, &hubpd,
                        DA_AtHubPortNumber, &hubport,
                        TAG_END);
            if((hubpd == peerpd) && (hubport == port)) {
                shadowed = TRUE;
                break;
            }
        }
    }
    psdUnlockPBase();
    return(shadowed);
}
/* \\\ */

/* /// "nConnectShadowDebounce()" */
/* 2.0 half with a known SS peer: give the SS half time to win the connector
 * before acting on a fresh USB2 connect (SS devices present a transient D+
 * pullup until link training succeeds). Returns TRUE when the connect turned
 * out to be the twin of an SS device. (Self-gating here, see above.) */
BOOL nConnectShadowDebounce(struct NepClassHubSS *nch, UWORD port)
{
    if(nch->nch_IsSSHalf || (!nch->nch_ContainerId)) {
        return(FALSE);
    }
    if(nPortShadowedByPeer(nch, port)) {
        return(TRUE);
    }
    psdLockReadPBase();
    BOOL haspeer = (nFindPeerHub(nch) != NULL);
    psdUnlockPBase();
    if(!haspeer) {
        return(FALSE);
    }
    psdDelayMS(500);
    return(nPortShadowedByPeer(nch, port));
}
/* \\\ */

/* /// "nHubssTask()" */
void nHubssTask() {

    struct NepClassHubSS *nch;
    struct PsdPipe *pp;
    ULONG sigmask;
    ULONG sigs;
    UWORD num;
    LONG ioerr;
    struct UsbPortStatus uhps;
    struct UsbHubStatus uhhs;
    ULONG count;
    struct PsdDevice *pd;
    STRPTR devname;
    struct NepHubSSMsg *nhm;

    KPRINTF(1, ("%s()\n", __func__));

    if((nch = nAllocHub())) {
        Forbid();
        if(nch->nch_ReadySigTask) {
            Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
        }
        Permit();
        count = 0;
        for(num = 1; num <= nch->nch_NumPorts; num++) {
            if(nPortShadowedByPeer(nch, num)) {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Port %ld is the USB 2.0 twin of a superspeed device, skipping.",
                               num);
                continue;
            }
            if(((nch->nch_Downstream)[num-1] = pd = nConfigurePort(nch, num))) {
                psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                if(!devname) devname = devunknown;
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Detected device '%s' at port %ld.",
                               devname, num);
                count++;
            }
        }
        if(count) {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Hub added %ld device(s).",
                           count);
        }
        // do a class scan
        for(num = 1; num <= nch->nch_NumPorts; num++) {
            if((pd = (nch->nch_Downstream)[num-1])) {
                psdHubClassScan(pd);
            }
        }
        sigmask = (1L<<nch->nch_TaskMsgPort->mp_SigBit)|(1L<<nch->nch_CtrlMsgPort->mp_SigBit)|SIGBREAKF_CTRL_C;
        nch->nch_Running = TRUE;
        nch->nch_IOStarted = FALSE;
        do {
            if(nch->nch_Running && (!nch->nch_IOStarted)) {
                psdSendPipe(nch->nch_EP1Pipe, nch->nch_PortChanges, (nch->nch_NumPorts+8)>>3);
                nch->nch_IOStarted = TRUE;
            }
            if(nch->nch_DisablePort || nch->nch_ClassScan) {
                /* Don't sleep on queued port work: the wake-up Signal may have
                   been consumed by a pipe wait inside a concurrent enumeration
                   (e.g. a twin-evict request arriving mid-nConfigurePort). */
                sigs = SetSignal(0, 0) & SIGBREAKF_CTRL_C;
            } else {
                /* Idle in Wait() with nothing queued is the one settled state;
                   the asynchronous arms in usbDoMethodA() raise the flag again
                   before they signal us. */
                nch->nch_Settling = FALSE;
                sigs = Wait(sigmask);
                nch->nch_Settling = TRUE;
            }

            while((nhm = (struct NepHubSSMsg *) GetMsg(nch->nch_CtrlMsgPort))) {
                nHandleHubMethod(nch, nhm);
                ReplyMsg((struct Message *) nhm);
            }

            if(nch->nch_DisablePort) {
                for(num = 1; num <= nch->nch_NumPorts; num++) {
                    if((nch->nch_DisablePort) & (1L<<num)) {
                        nch->nch_DisablePort &= ~(1L<<num);
                        /* Remove device */
                        if((pd = (nch->nch_Downstream)[num-1])) {
                            psdSetAttrs(PGA_DEVICE, pd, DA_IsConnected, FALSE, TAG_END);
                            psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                            if(!devname) devname = devunknown;
                            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                           "Removing dead device '%s' at port %ld.",
                                           devname, num);
                            psdFreeDevice(pd);
                            psdSendEvent(EHMB_REMDEVICE, pd, NULL);
                            (nch->nch_Downstream)[num-1] = NULL;
                            pd = NULL;
                            /* SS ports cannot be disabled, so nothing to do here. */
                        }
                        if(nch->nch_PowerCycle & (1<<num)) {
                            KPRINTF(2, ("Powercycle request for port %lu\n", num));
                            nch->nch_PowerCycle &= ~(1L<<num);

                            /* Wait for device to settle */
                            psdDelayMS(250);
                            if(nPortShadowedByPeer(nch, num)) {
                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                               "Port %ld is the USB 2.0 twin of a superspeed device, skipping.",
                                               num);
                            } else if(((nch->nch_Downstream)[num-1] = pd = nConfigurePort(nch, num))) {
                                psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                                if(!devname) devname = devunknown;
                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                               "Device '%s' returned.",
                                               devname);
                                psdHubClassScan(pd);
                            }
                        }
                    }
                }
            }

            if(nch->nch_ClassScan) {
                nch->nch_ClassScan = FALSE;
                for(num = 1; num <= nch->nch_NumPorts; num++)
                {
                    if((pd = (nch->nch_Downstream)[num-1]))
                    {
                        psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                        if(!devname) devname = devunknown;
                        psdHubClassScan(pd);
                    }
                }
            }
            while((pp = (struct PsdPipe *) GetMsg(nch->nch_TaskMsgPort))) {
                if(pp == nch->nch_EP1Pipe) {
                    nch->nch_IOStarted = FALSE;
                    ioerr = psdGetPipeError(nch->nch_EP1Pipe);
                    if(ioerr == UHIOERR_TIMEOUT) {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                       "Hub involuntarily gone! Disconnecting...");
                        psdSetAttrs(PGA_DEVICE, nch->nch_Device,
                                    DA_IsConnected, FALSE,
                                    TAG_END);
                        nch->nch_PortChanges[0] = 0xff;
                        nch->nch_PortChanges[1] = 0xff;
                        nch->nch_PortChanges[2] = 0xff;
                        nch->nch_PortChanges[3] = 0xff;
                        sigs |= SIGBREAKF_CTRL_C;
                    }
                    if((!ioerr) || (ioerr == UHIOERR_TIMEOUT)) {
                        KPRINTF(2, ("Port changed at 0x%08lx, Numports=%ld!\n", nch->nch_PortChanges[0], nch->nch_NumPorts));

                        if(nch->nch_PortChanges[0] & 1) {
                            psdPipeSetup(nch->nch_EP0Pipe, URTF_IN|URTF_CLASS|URTF_DEVICE,
                                         USR_GET_STATUS, 0, 0);
                            ioerr = psdDoPipe(nch->nch_EP0Pipe, &uhhs, sizeof(struct UsbHubStatus));
                            uhhs.wHubStatus = AROS_WORD2LE(uhhs.wHubStatus);
                            uhhs.wHubChange = AROS_WORD2LE(uhhs.wHubChange);
                            if(!ioerr)
                            {
                                if(uhhs.wHubStatus & UHSF_OVER_CURRENT)
                                {
                                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                   "Hub over-current situation detected! Unpowering ALL ports!");
                                    for(num = 1; num <= nch->nch_NumPorts; num++)
                                    {
                                        psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                                                     USR_CLEAR_FEATURE, UFS_PORT_POWER, (ULONG) num);
                                        ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
                                        if(ioerr)
                                        {
                                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                           "PORT_POWER for port %ld failed: %s (%ld)",
                                                           num, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                                            KPRINTF(1, ("PORT_POWER for port %ld failed %ld!\n", num, ioerr));
                                        }

                                        psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                                                     USR_CLEAR_FEATURE, UFS_C_PORT_OVER_CURRENT, (ULONG) num);
                                        psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
                                    }
                                    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_DEVICE,
                                                 USR_CLEAR_FEATURE, UFS_C_HUB_OVER_CURRENT, 0);
                                    psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
                                }
                                if(uhhs.wHubChange & UHSF_LOCAL_POWER_LOST)
                                {
                                    struct PsdConfig *pc = NULL;
                                    struct PsdHardware *phw = NULL;
                                    psdGetAttrs(PGA_DEVICE, nch->nch_Device,
                                                DA_Config, &pc,
                                                DA_Hardware, &phw,
                                                TAG_END);
                                    if(uhhs.wHubStatus & UHSF_LOCAL_POWER_LOST)
                                    {
                                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                       "Hub is no longer self-powered! Low power conditions may occur.");

                                        if(pc && phw)
                                        {
                                            psdSetAttrs(PGA_CONFIG, pc, CA_SelfPowered, FALSE, TAG_END);
                                            psdCalculatePower(phw);
                                        }
                                    } else {
                                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                                       "Hub is now self-powered.");
                                        if(pc && phw)
                                        {
                                            psdSetAttrs(PGA_CONFIG, pc, CA_SelfPowered, TRUE, TAG_END);
                                            psdCalculatePower(phw);
                                        }
                                    }
                                    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_DEVICE,
                                                 USR_CLEAR_FEATURE, UFS_C_HUB_LOCAL_POWER, 0);
                                    psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
                                }
                            }
                        }

                        for(num = 1; num <= nch->nch_NumPorts; num++)
                        {
                            if(nch->nch_PortChanges[num>>3] & (1L<<(num & 7)))
                            {
                                IPTR isconnected = 0;

                                psdGetAttrs(PGA_DEVICE, nch->nch_Device,
                                            DA_IsConnected, &isconnected, TAG_END);
                                if(!isconnected)
                                {
                                    /* The hub is gone, so asking it anything buys nothing:
                                       psdDoPipe() short-circuits every request on a device
                                       that is no longer connected and hands back a
                                       manufactured timeout 50ms later without touching
                                       hardware. The one thing we know is that the child is
                                       unreachable, which is exactly what the connection arm
                                       below concludes - synthesise that and nothing else.
                                       A blanket 0xffff carries C_PORT_CONFIG_ERROR, which
                                       sends every port down the warm-reset recovery arm:
                                       a full reset + clear + re-enumeration sequence
                                       against a hub that cannot answer, ~450ms and five
                                       misleading errors per port on every hub unplug. */
                                    uhps.wPortStatus = 0;
                                    uhps.wPortChange = UPCF_C_PORT_CONNECTION;
                                    ioerr = 0;
                                } else {
                                    ioerr = nReadPortStatus(nch, num, &uhps);
                                    if(ioerr == UHIOERR_TIMEOUT)
                                    {
                                        /* a live hub that did not answer within the pipe's
                                           1s NAK timeout is a real transfer failure, not a
                                           disconnect: assume every change is pending */
                                        uhps.wPortStatus = 0;
                                        uhps.wPortChange = 0xffff;
                                        ioerr = 0;
                                    } else {
                                        nClearPortStatus(nch, num);
                                    }
                                }
                                if(!ioerr)
                                {
                                    pd = (nch->nch_Downstream)[num-1];
                                    if(uhps.wPortStatus & UPSF_PORT_OVER_CURRENT)
                                    {
                                        if(pd)
                                        {
                                            psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                                            if(!devname) devname = devunknown;
                                        } else {
                                            devname = "a ghost";
                                        }
                                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                       "Over-current situation detected with %s at port %ld! Unpowering port!",
                                                       devname, num);
                                        psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                                                     USR_CLEAR_FEATURE, UFS_PORT_POWER, (ULONG) num);
                                        ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
                                        if(ioerr)
                                        {
                                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                           "PORT_POWER for port %ld failed: %s (%ld)",
                                                           num, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                                            KPRINTF(1, ("PORT_POWER for port %ld failed %ld!\n", num, ioerr));
                                        }

                                        psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                                                     USR_CLEAR_FEATURE, UFS_C_PORT_OVER_CURRENT, (ULONG) num);
                                        psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
                                    }
                                    if(uhps.wPortChange & (UPCF_C_PORT_LINK_STATE | UPCF_C_PORT_CONFIG_ERROR))
                                    {
                                        UWORD pls = (uhps.wPortStatus & UPSF_SS_PORT_LINK_STATE)
                                                    >> UPSS_SS_PORT_LINK_STATE;
                                        if((uhps.wPortChange & UPCF_C_PORT_CONFIG_ERROR) ||
                                           (pls == UPLS_SS_INACTIVE) || (pls == UPLS_COMPLIANCE))
                                        {
                                            /* The downstream link failed and cannot retrain itself;
                                               only a warm reset clears SS.Inactive/Compliance, and it
                                               returns the device to the default state, so tear the old
                                               device down and re-enumerate. */
                                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                           "Link error (state %ld) on port %ld, warm-resetting.",
                                                           (ULONG) pls, num);
                                            if(pd)
                                            {
                                                psdSetAttrs(PGA_DEVICE, pd, DA_IsConnected, FALSE, TAG_END);
                                                psdFreeDevice(pd);
                                                psdSendEvent(EHMB_REMDEVICE, pd, NULL);
                                                (nch->nch_Downstream)[num-1] = NULL;
                                                pd = NULL;
                                            }
                                            nWarmResetPort(nch, num);
                                            nClearPortStatus(nch, num);
                                            /* nConfigurePort re-reads status, no-ops if unplugged */
                                            if(((nch->nch_Downstream)[num-1] = pd = nConfigurePort(nch, num)))
                                            {
                                                psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                                                if(!devname) devname = devunknown;
                                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                                               "Device '%s' at port %ld recovered.",
                                                               devname, num);
                                                psdClassScan();
                                            }
                                        }
                                        else if((pls == UPLS_U0) && pd)
                                        {
                                            IPTR oldsusp = 0;
                                            psdGetAttrs(PGA_DEVICE, pd, DA_IsSuspended, &oldsusp, TAG_END);
                                            psdSetAttrs(PGA_DEVICE, pd, DA_IsSuspended, FALSE, TAG_END);
                                            psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                                            if(!devname) devname = devunknown;
                                            if(oldsusp)
                                            {
                                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                                               "Device '%s' at port %ld resumed from remote!",
                                                               devname, num);
                                                psdSendEvent(EHMB_DEVRESUMED, pd, NULL);
                                                psdResumeBindings(pd);
                                            }
                                        }
                                        else if((pls == UPLS_U3) && pd)
                                        {
                                            /* U3 is the parked state, never a wake . The device flag is
                                               the stack's mirror of that: psdDoPipe()/psdSendPipe()
                                               auto-resume off it and the idle sweep skips devices
                                               already suspended, so clearing it would strand the
                                               device on a U3 link. nHubSuspendDevice() has set it on
                                               our own suspend path - this only re-syncs a port that
                                               was parked behind the stack's back. */
                                            IPTR oldsusp = 0;
                                            psdGetAttrs(PGA_DEVICE, pd, DA_IsSuspended, &oldsusp, TAG_END);
                                            psdSetAttrs(PGA_DEVICE, pd, DA_IsSuspended, TRUE, TAG_END);
                                            psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                                            if(!devname) devname = devunknown;
                                            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                                           "Device '%s' at port %ld suspended!",
                                                           devname, num);
                                            if(!oldsusp)
                                            {
                                                psdSendEvent(EHMB_DEVSUSPENDED, pd, NULL);
                                            }
                                        } else {
                                            /* U1/U2/Recovery/Resume etc. are normal SuperSpeed link
                                               power management on a downstream port: no action. */
                                            KPRINTF(2, ("port %ld link state %ld, no action\n",
                                                        num, (ULONG) pls));
                                        }
                                    }
                                    if(uhps.wPortChange & UPCF_C_PORT_CONNECTION)
                                    {
                                        /* Remove device */
                                        if((!(uhps.wPortStatus & UPSF_PORT_CONNECTION)) && pd)
                                        {
                                            psdSetAttrs(PGA_DEVICE, pd, DA_IsConnected, FALSE, TAG_END);
                                            psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                                            if(!devname) devname = devunknown;
                                            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                                           "Device '%s' at port %ld is gone!",
                                                           devname, num);
                                            psdFreeDevice(pd);
                                            psdSendEvent(EHMB_REMDEVICE, pd, NULL);
                                            (nch->nch_Downstream)[num-1] = NULL;
                                            pd = NULL;
                                        }
                                        /* add new device */
                                        if((uhps.wPortStatus & UPSF_PORT_CONNECTION) && (!pd))
                                        {
                                            /* Wait for device to settle */
                                            psdDelayMS(100);
                                            if(nConnectShadowDebounce(nch, num))
                                            {
                                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                                               "Port %ld is the USB 2.0 twin of a superspeed device, skipping.",
                                                               num);
                                            }
                                            else if(((nch->nch_Downstream)[num-1] = pd = nConfigurePort(nch, num)))
                                            {
                                                psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
                                                if(!devname) devname = devunknown;
                                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                                               "New device '%s' at port %ld.",
                                                               devname, num);
                                                psdClassScan();
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        /* Bail out on time out. */
                        if(nch->nch_PortChanges[0] == 0xff)
                        {
                            break;
                        }
                        psdDelayMS(50);
                    } else {
                        if(ioerr != IOERR_ABORTED)
                        {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                           "Status packet failed: %s (%ld)",
                                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                            psdDelayMS(200);
                        }
                    }
                    break;
                } else {
                    KPRINTF(20, ("Bogus message received!\n"));
                }
            }
        } while(!(sigs & SIGBREAKF_CTRL_C));
        KPRINTF(20, ("Going down the river!\n"));
        if(nch->nch_IOStarted)
        {
            psdAbortPipe(nch->nch_EP1Pipe);
            psdWaitPipe(nch->nch_EP1Pipe);
        }
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Hub task terminated.");
        nFreeHub(nch);
    }
}

/* /// "nAllocHub()" */
struct NepClassHubSS * nAllocHub(void) {
    struct UsbSSHubDesc *usshd;
    //struct UsbStdBOSDesc *usbosd;
    struct Task *thistask;
    struct NepClassHubSS *nch;
    struct UsbHubStatus uhhs;
    APTR parenthub;
    LONG ioerr;
    ULONG len;
    UWORD num;
    UBYTE buf[2];
    IPTR issuperspeed = 0;
    IPTR prodid;
    IPTR vendid;
    IPTR proto = 0;
    UBYTE *containerid = NULL;
    BOOL overcurrent = FALSE;
    IPTR ctxhw = 0;

    thistask = FindTask(NULL);
    nch = thistask->tc_UserData;

    KPRINTF(1, ("%s()\n", __func__));

    do {
        if(!(nch->nch_Base = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION))) {
            Alert(AG_OpenLib);
            break;
        }

        psdGetAttrs(PGA_DEVICE, nch->nch_Device,
                    DA_Hardware, &nch->nch_Hardware,
                    DA_IsSuperspeed, &issuperspeed,
                    DA_ProductID, &prodid,
                    DA_VendorID, &vendid,
                    DA_HubDevice, &parenthub,
                    DA_Protocol, &proto,
                    DA_ContainerId, &containerid,
                    TAG_END);

        /* hubss.usbclass drives the raw USB3 hub protocol and relies on the HCD
           owning device addressing (the context lifecycle ABI).  A SuperSpeed
           hub only ever enumerates on a context HCD - on the legacy ABI xhci
           presents a USB 2.0 hub, bound by hub.usbclass - so a non-context HCD
           here is a misconfiguration: refuse the binding rather than run blind. */
        psdGetAttrs(PGA_HARDWARE, nch->nch_Hardware,
                    HA_ContextBackend, &ctxhw,
                    TAG_END);
        if(!ctxhw) {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                           CLASS_NAME " requires a context-ABI HCD; ignoring SuperSpeed hub.");
            break;
        }

        nch->nch_IsRootHub = (parenthub ? FALSE : TRUE);
        /* USB3 hub pairing: hubss.usbclass only binds superspeed hubs, so this is
           always the SS half. An all-zero Container ID (counterfeit hubs) keeps
           pairing disabled for this hub. */
        nch->nch_IsSSHalf = (proto == 3) || issuperspeed;
        nch->nch_ContainerId = containerid;
        if(containerid) {
            UWORD cnt;
            for(cnt = 0; (cnt < 16) && (!containerid[cnt]); cnt++);
            if(cnt == 16) {
                nch->nch_ContainerId = NULL;
            }
        }

        if(!nch->nch_Interface) {
            nch->nch_Interface = psdFindInterface(nch->nch_Device, NULL, IFA_Class, HUB_CLASSCODE, TAG_END);
        }

        if(!nch->nch_Interface) {
            KPRINTF(1, ("Ooops!?! No interfaces defined?\n"));
            break;
        }

        nch->nch_EP1 = psdFindEndpoint(nch->nch_Interface, NULL, EA_IsIn, TRUE, EA_TransferType, USEAF_INTERRUPT, TAG_END);

        if(!nch->nch_EP1) {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "No endpoints defined.");
            KPRINTF(1, ("Ooops!?! No Endpoints defined?\n"));
            break;
        }

        /*
            Device descriptor tree:

            Device descriptor
            Config descriptor
            ...String descriptor
            BOS descriptor
            ...SS capability descriptor
            ......USB2.0 LPM descriptor
            Interface descriptor
            Endpoint descriptor 0
            ...Endpoint descriptor 1
            ......Endpoint descriptor n

            USB 3.0 enumeration:

            Set address
            Get device descriptor
            Get BOS descriptor
            Get config descriptor
                .
                .
                .

            see: http://youtu.be/5ChWxMLKzOs
        */


        if((nch->nch_CtrlMsgPort = CreateMsgPort())) {
            if((nch->nch_TaskMsgPort = CreateMsgPort())) {
                KPRINTF(2, ("Allocating EP0 pipe..\n"));
                if((nch->nch_EP0Pipe = psdAllocPipe(nch->nch_Device, nch->nch_TaskMsgPort, NULL))) {
                    KPRINTF(2, ("EP0 pipe @ 0x%08lx\n", nch->nch_EP0Pipe));
                    psdSetAttrs(PGA_PIPE, nch->nch_EP0Pipe, PPA_NakTimeout, TRUE, PPA_NakTimeoutTime, 1000, TAG_END);
                    psdSetAltInterface(nch->nch_EP0Pipe, nch->nch_Interface);

                    if((nch->nch_EP1Pipe = psdAllocPipe(nch->nch_Device, nch->nch_TaskMsgPort, nch->nch_EP1))) {

                        psdSetAttrs(PGA_PIPE, nch->nch_EP1Pipe, PPA_AllowRuntPackets, TRUE, TAG_END);
                        psdPipeSetup(nch->nch_EP0Pipe, URTF_IN|URTF_CLASS|URTF_DEVICE, USR_GET_DESCRIPTOR, UDT_SSHUB<<8, 0);

                        ioerr = psdDoPipe(nch->nch_EP0Pipe, &buf, 2);

                        if(buf[1] == UDT_SSHUB) {

                            if((!ioerr) || (ioerr == UHIOERR_OVERFLOW)) {
                                len = buf[0];

                                if((usshd = psdAllocVec(len))) {
                                    ioerr = psdDoPipe(nch->nch_EP0Pipe, usshd, len);

                                    if(!ioerr) {
                                        nch->nch_NumPorts     = (UWORD)usshd->bNbrPorts;
                                        nch->nch_HubAttr      = (UWORD)AROS_WORD2LE(usshd->wHubCharacteristics);
                                        nch->nch_PwrGoodTime  = (UWORD)usshd->bPwrOn2PwrGood<<1;
                                        /* An external hub owes a device at least 100ms
                                           after port power, whatever its descriptor says;
                                           a root hub is the host controller driver and is
                                           taken at its word (see hub.usbclass). */
                                        if(!nch->nch_IsRootHub && (nch->nch_PwrGoodTime < 100)) {
                                            nch->nch_PwrGoodTime = 100;
                                        }
                                        nch->nch_HubCurrent   = (UWORD)usshd->bHubContrCurrent;
                                        nch->nch_HubHdrDecLat = (UWORD)usshd->bHubHdrDecLat;
                                        nch->nch_HubDelay     = (UWORD)AROS_WORD2LE(usshd->wHubDelay);
                                        nch->nch_Removable    = (UWORD)usshd->DeviceRemovable;

                                        /* publish the hub facts; DA_HubNumPorts triggers the
                                           HCD update-hub op on context backends (SS hubs have
                                           no TT, the think-time bits are reserved).  The two
                                           latency facts feed the HCD's U1/U2 exit-latency math
                                           for devices downstream of this hub. */
                                        psdSetAttrs(PGA_DEVICE, nch->nch_Device,
                                                    DA_HubHdrDecLat, nch->nch_HubHdrDecLat,
                                                    DA_HubDelay, nch->nch_HubDelay,
                                                    DA_HubNumPorts, nch->nch_NumPorts,
                                                    TAG_END);

                                        if(!nch->nch_IsRootHub) {
                                            /* USB 3 section 10.14.2.9: an SS hub routes nothing downstream
                                               until told its tier (the root hub needs no depth). */
                                            UWORD depth = 0;
                                            APTR up = NULL;
                                            APTR hub = NULL;
                                            psdGetAttrs(PGA_DEVICE, nch->nch_Device, DA_HubDevice, &hub, TAG_END);
                                            while(hub) {
                                                up = NULL;
                                                psdGetAttrs(PGA_DEVICE, hub, DA_HubDevice, &up, TAG_END);
                                                if(!up) {
                                                    break; /* 'hub' is the root hub: don't count it */
                                                }
                                                depth++;
                                                hub = up;
                                            }
                                            psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_DEVICE,
                                                         UHR_SET_HUB_DEPTH, depth, 0);
                                            ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
                                            if(ioerr) {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "SET_HUB_DEPTH(%ld) failed: %s (%ld)",
                                                               (ULONG) depth,
                                                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                                            }
                                        }

                                        KPRINTF(2, ("Parsed SSHub descriptor\n"
                                                    "  nch_NumPorts     = %ld\n"
                                                    "  nch_HubAttr      = 0x%04lx\n"
                                                    "  nch_PwrGoodTime  = %ld\n"
                                                    "  nch_HubCurrent   = %ld\n"
                                                    "  nch_HubHdrDecLat = %ld\n"
                                                    "  nch_HubDelay     = %ld\n"
                                                    "  nch_Removable    = 0x%04lx\n\n",
                                                    (ULONG)nch->nch_NumPorts,
                                                    (ULONG)nch->nch_HubAttr,
                                                    (ULONG)nch->nch_PwrGoodTime,
                                                    (ULONG)nch->nch_HubCurrent,
                                                    (ULONG)nch->nch_HubHdrDecLat,
                                                    (ULONG)nch->nch_HubDelay,
                                                    (ULONG)nch->nch_Removable));

                                        psdFreeVec(usshd);

                                        psdPipeSetup(nch->nch_EP0Pipe, URTF_IN|URTF_CLASS|URTF_DEVICE, USR_GET_STATUS, 0, 0);
                                        ioerr = psdDoPipe(nch->nch_EP0Pipe, &uhhs, sizeof(struct UsbHubStatus));

                                        uhhs.wHubStatus = AROS_WORD2LE(uhhs.wHubStatus);
                                        uhhs.wHubChange = AROS_WORD2LE(uhhs.wHubChange);
                                        if(!ioerr)
                                        {
                                            struct PsdConfig *pc = NULL;
                                            struct PsdHardware *phw = NULL;
                                            if(uhhs.wHubStatus & UHSF_OVER_CURRENT)
                                            {
                                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                               "Hub over-current situation detected! Resolve this first!");
                                                //overcurrent = TRUE;
                                            }

                                            psdGetAttrs(PGA_DEVICE, nch->nch_Device,
                                                        DA_Config, &pc,
                                                        DA_Hardware, &phw,
                                                        TAG_END);
                                            if(uhhs.wHubStatus & UHSF_LOCAL_POWER_LOST)
                                            {
                                                if(pc && phw)
                                                {
                                                    psdSetAttrs(PGA_CONFIG, pc, CA_SelfPowered, FALSE, TAG_END);
                                                    psdCalculatePower(phw);
                                                }
                                            } else {
                                                if(pc && phw)
                                                {
                                                    psdSetAttrs(PGA_CONFIG, pc, CA_SelfPowered, TRUE, TAG_END);
                                                    psdCalculatePower(phw);
                                                }
                                            }
                                        }
                                        if(!overcurrent) {
                                            if((nch->nch_Downstream = psdAllocVec((ULONG) nch->nch_NumPorts*sizeof(APTR)))) {
                                                /*for(num = 1; num <= nch->nch_NumPorts; num++)
                                                {
                                                    nClearPortStatus(nch, num);
                                                }
                                                psdDelayMS(20);*/

                                                KPRINTF(2, ("Powering up ports...\n\n"));

                                                for(num = 1; num <= nch->nch_NumPorts; num++) {
                                                    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER, USR_SET_FEATURE, UFS_PORT_POWER, (ULONG) num);
                                                    ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);

                                                    if(ioerr) {
                                                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "PORT_POWER for port %ld failed: %s (%ld)", num, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                                                        KPRINTF(1, ("PORT_POWER for port %ld failed %ld!\n", num, ioerr));
                                                    }
                                                }
                                                psdDelayMS((ULONG) nch->nch_PwrGoodTime + 15);

                                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Hub with %ld ports successfully configured.", nch->nch_NumPorts);

                                                KPRINTF(10, ("%s ready!\n", thistask->tc_Node.ln_Name));
                                                nch->nch_Task = thistask;

                                                return(nch);
                                            } else {
                                                KPRINTF(1, ("No downstream port array memory!\n"));
                                            }
                                        }
                                    } else {
                                        psdFreeVec(usshd);
                                        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "GET_HUB_DESCRIPTOR (%ld) failed: %s (%ld)", len, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                                        KPRINTF(1, ("GET_HUB_DESCRIPTOR (%ld) failed %ld!\n", len, ioerr));
                                    }

                                } else {
                                    KPRINTF(1, ("No Hub Descriptor memory!\n"));
                                }
                            } else {
                                psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "GET_HUB_DESCRIPTOR (%ld) failed: %s (%ld)", 1, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                                KPRINTF(1, ("GET_HUB_DESCRIPTOR (1) failed %ld!\n", ioerr));
                            }

                        }

                        psdFreePipe(nch->nch_EP1Pipe);
                    }
                    psdFreePipe(nch->nch_EP0Pipe);
                }
                DeleteMsgPort(nch->nch_TaskMsgPort);
            }
            DeleteMsgPort(nch->nch_CtrlMsgPort);
        }
    } while(FALSE);

    CloseLibrary(nch->nch_Base);

    Forbid();
    nch->nch_Task = NULL;

    if(nch->nch_ReadySigTask) {
        Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
    }

    return(NULL);
}

/* /// "nFreeHub()" */
void nFreeHub(struct NepClassHubSS *nch) {
    UWORD num;
    LONG ioerr;
    struct PsdDevice *pd;
    STRPTR devname;
    IPTR isconnected;
    struct Message *msg;

    KPRINTF(1, ("%s(0x%08lx)\n", __func__, nch));

    psdGetAttrs(PGA_DEVICE, nch->nch_Device, DA_IsConnected, &isconnected, TAG_END);
    for(num = 1; num <= nch->nch_NumPorts; num++) {
        KPRINTF(1, ("Iterating Port %ld\n", num));
        /* Remove downstream device */
        pd = (nch->nch_Downstream)[num-1];
        if(pd) {
            if(!isconnected) {
                psdSetAttrs(PGA_DEVICE, pd, DA_IsConnected, FALSE, TAG_END);
            }
            psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
            if(!devname) devname = devunknown;
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Hub removal disconnected device '%s' at port %ld.", devname, num);
            KPRINTF(1, ("FreeDevice 0x%08lx\n", pd));
            psdFreeDevice(pd);
            psdSendEvent(EHMB_REMDEVICE, pd, NULL);
            (nch->nch_Downstream)[num-1] = NULL;
        }
        /* There's no sense trying to send out commands if the hub is already gone! */
        if(isconnected) {
             /* power down for port */
             psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER, USR_CLEAR_FEATURE, UFS_PORT_POWER, (ULONG) num);
             ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
             if(ioerr) {
                 psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "PORT_POWER for port %ld failed: %s (%ld)", num, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                 KPRINTF(1, ("PORT_POWER for port %ld failed %ld!\n", num, ioerr));
             }
        }
    }

    KPRINTF(1, ("FreePipes\n"));
    psdFreePipe(nch->nch_EP1Pipe);
    psdFreePipe(nch->nch_EP0Pipe);
    psdFreeVec(nch->nch_Downstream);

    KPRINTF(1, ("Entering Forbid\n"));
    Forbid();
    // clear queue
    while((msg = GetMsg(nch->nch_CtrlMsgPort))) {
        ReplyMsg(msg);
    }

    DeleteMsgPort(nch->nch_TaskMsgPort);
    DeleteMsgPort(nch->nch_CtrlMsgPort);
    CloseLibrary(nch->nch_Base);
    nch->nch_Task = NULL;

    if(nch->nch_ReadySigTask) {
        Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
    }

    KPRINTF(1, ("Really gone now!\n"));
}

/* *** HUBSS Class *** */

/* /// "nReadPortStatus()" */
/*
 * GET_PORT_STATUS, returning the native USB 3 wire status/change words.
 *
 * Every hub this class binds speaks the USB 3 spec port-status format
 * (UPSF_SS_ / UPCF_ flags): power in bit 9, link state in bits 8:5, a zero
 * speed field (5 Gbps).
 */
LONG nReadPortStatus(struct NepClassHubSS *nch, UWORD port, struct UsbPortStatus *uhps)
{
    LONG ioerr;

    psdPipeSetup(nch->nch_EP0Pipe, URTF_IN|URTF_CLASS|URTF_OTHER,
                 USR_GET_STATUS, 0, (ULONG)port);
    ioerr = psdDoPipe(nch->nch_EP0Pipe, uhps, sizeof(struct UsbPortStatus));

    uhps->wPortStatus = AROS_WORD2LE(uhps->wPortStatus);
    uhps->wPortChange = AROS_WORD2LE(uhps->wPortChange);

    KPRINTF(2, ("port %ld status/change %04lx/%04lx\n",
                port, (ULONG)uhps->wPortStatus, (ULONG)uhps->wPortChange));
    return ioerr;
}


/* /// "nClearPortStatus()" */
LONG nClearPortStatus(struct NepClassHubSS *nch, UWORD port)
{
    LONG ioerr;
    LONG firsterr = 0;

    KPRINTF(1, ("%s(0x%08lx, %ld)\n", __func__, nch, port));

    /* Best-effort: try to clear all relevant change bits. Do not abort early,
       otherwise we may leave sticky change flags behind and re-trigger events. */

    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                 USR_CLEAR_FEATURE, UFS_C_PORT_CONNECTION, (ULONG)port);
    if((ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0))) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                       "CLEAR_PORT_FEATURE (C_PORT_CONNECTION) failed: %s (%ld)",
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(10, ("error occurred clearing UFS_C_PORT_CONNECTION!\n"));
        if(!firsterr) firsterr = ioerr;
    }

    /* raw USB3 hub protocol: the SS change selectors */
    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                 USR_CLEAR_FEATURE, UFS_C_PORT_LINK_STATE, (ULONG)port);
    if((ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0))) {
        KPRINTF(10, ("error occurred clearing UFS_C_PORT_LINK_STATE!\n"));
        if(!firsterr) firsterr = ioerr;
    }

    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                 USR_CLEAR_FEATURE, UFS_C_PORT_CONFIG_ERROR, (ULONG)port);
    if((ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0))) {
        KPRINTF(10, ("error occurred clearing UFS_C_PORT_CONFIG_ERROR!\n"));
        if(!firsterr) firsterr = ioerr;
    }

    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                 USR_CLEAR_FEATURE, UFS_C_BH_PORT_RESET, (ULONG)port);
    if((ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0))) {
        KPRINTF(10, ("error occurred clearing UFS_C_BH_PORT_RESET!\n"));
        if(!firsterr) firsterr = ioerr;
    }

    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                 USR_CLEAR_FEATURE, UFS_C_PORT_OVER_CURRENT, (ULONG)port);
    if((ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0))) {
        KPRINTF(10, ("error occurred clearing UFS_C_PORT_OVER_CURRENT!\n"));
        if(!firsterr) firsterr = ioerr;
    }

    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                 USR_CLEAR_FEATURE, UFS_C_PORT_RESET, (ULONG)port);
    if((ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0))) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                       "CLEAR_PORT_FEATURE (C_PORT_RESET) failed: %s (%ld)",
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(10, ("error occurred clearing UFS_C_PORT_RESET!\n"));
        if(!firsterr) firsterr = ioerr;
    }

    return firsterr;
}


/* /// "nWarmResetPort()" */
/*
 * Warm-reset (BH_PORT_RESET) a downstream port and wait for it to complete.
 * A warm reset is the only way to clear an SS.Inactive/Compliance link, and it
 * returns the device to the default state, so the caller must re-enumerate.
 */
LONG nWarmResetPort(struct NepClassHubSS *nch, UWORD port)
{
    struct UsbPortStatus uhps;
    LONG ioerr;
    LONG retries;

    KPRINTF(1, ("%s(0x%08lx, %ld)\n", __func__, nch, port));

    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                 USR_SET_FEATURE, UFS_BH_PORT_RESET, (ULONG)port);
    ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
    if(ioerr) {
        psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                       "BH_PORT_RESET for port %ld failed: %s (%ld)",
                       port, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return ioerr;
    }

    /* wait for the warm reset to finish (C_BH_PORT_RESET) or the port to drop */
    for(retries = 0; retries < 500; retries += 20) {
        psdDelayMS(20);
        if((ioerr = nReadPortStatus(nch, port, &uhps))) break;
        if(uhps.wPortChange & UPCF_C_BH_PORT_RESET) break;
        if(!(uhps.wPortStatus & UPSF_PORT_CONNECTION)) break;
    }
    return ioerr;
}


/* /// "nConfigurePort()" */
struct PsdDevice * nConfigurePort(struct NepClassHubSS *nch, UWORD port)
{
    LONG ioerr;
    LONG delayretries;
    LONG resetretries;
    ULONG delaytime = 10;
    struct UsbPortStatus uhps;
    struct PsdDevice *pd;
    struct PsdPipe *pp;

    KPRINTF(1, ("%s(0x%08lx, %ld)\n", __func__, nch, port));

    uhps.wPortStatus = 0xDEAD;
    uhps.wPortChange = 0xDA1A;

    ioerr = nReadPortStatus(nch, port, &uhps);

    if(!ioerr) {
        KPRINTF(2, ("Status 0x%04lx, change 0x%04lx\n", uhps.wPortStatus, uhps.wPortChange));

        if(uhps.wPortStatus & UPSF_PORT_CONNECTION) {
            KPRINTF(2, ("There's something at port %ld!\n", port));

            Forbid();
            if((pd = psdAllocDevice(nch->nch_Hardware))) {
                psdLockWriteDevice(pd);
                Permit();

                /* Hub reference */
                psdSetAttrs(PGA_DEVICE, pd,
                            DA_HubDevice, nch->nch_Device,
                            DA_IsConnected, TRUE,
                            DA_AtHubPortNumber, port,
                            TAG_END);

                /* every device on a SuperSpeed hub is SuperSpeed */
                psdSetAttrs(PGA_DEVICE, pd, DA_IsSuperspeed, TRUE, TAG_END);
                KPRINTF(2, ("    It's a superspeed device!\n"));

                for(resetretries = 0; resetretries < 3; resetretries++) {
                    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                                 USR_SET_FEATURE, UFS_PORT_RESET, (ULONG)port);
                    ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);

                    if(ioerr) {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                                       "PORT_RESET for port %ld failed: %s (%ld)",
                                       port, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                        KPRINTF(1, ("PORT_RESET failed %ld.\n", ioerr));
                        break;
                    }

                    if(nch->nch_IsRootHub) {
                        /* Root hubs need 50ms minimum delay */
                        psdDelayMS(50);
                    }

                    for(delayretries = 0; delayretries < 500; delayretries += delaytime) {
                        psdDelayMS(delaytime);

                        ioerr = nReadPortStatus(nch, port, &uhps);

                        if(ioerr) {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                                           "GET_PORT_STATUS for port %ld failed: %s (%ld)",
                                           port, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                            KPRINTF(1, ("GET_PORT_STATUS failed %ld.\n", ioerr));
                            break;
                        }

                        KPRINTF(2, ("After reset: status 0x%04lx, change 0x%04lx\n",
                                    uhps.wPortStatus, uhps.wPortChange));

                        if(!(uhps.wPortStatus & UPSF_PORT_CONNECTION)) {
                            break;
                        }

                        if((uhps.wPortStatus &
                            (UPSF_PORT_RESET|UPSF_PORT_CONNECTION|UPSF_PORT_ENABLE|
                             UPSF_SS_PORT_POWER|UPSF_PORT_OVER_CURRENT))
                           == (UPSF_PORT_CONNECTION|UPSF_PORT_ENABLE|UPSF_SS_PORT_POWER))
                        {
                            psdSetAttrs(PGA_DEVICE, pd, DA_IsSuperspeed, TRUE, TAG_END);
                            KPRINTF(2, ("    It's a superspeed device!\n"));

                            nClearPortStatus(nch, port);
                            psdDelayMS(100);

                            if((pp = psdAllocPipe(pd, nch->nch_TaskMsgPort, NULL))) {
                                if(psdEnumerateDevice(pp)) {
                                    KPRINTF(2, ("  Device successfully added!\n"));
                                    psdFreePipe(pp);
                                    psdUnlockDevice(pd);
                                    psdSendEvent(EHMB_ADDDEVICE, pd, NULL);
                                    nNotifyPeerTwinEvict(nch, port);
                                    return pd;
                                }
                                psdFreePipe(pp);
                            }
                            break;
                        } else {
                            if(!(uhps.wPortStatus & UPSF_PORT_RESET)) {
                                psdAddErrorMsg(RETURN_ERROR, (STRPTR)libname,
                                               "Wrong port status %04lx for port %ld!",
                                               uhps.wPortStatus, port);
                                KPRINTF(2, ("Wrong port status %04lx for port %ld.\n",
                                            uhps.wPortStatus, port));
                            }
                        }

                        if(delayretries > 20) {
                            delaytime = 300;
                        }
                    }

                    delaytime = 200;
                }

                psdUnlockDevice(pd);
                psdFreeDevice(pd);

                nClearPortStatus(nch, port);
            } else {
                Permit();
                KPRINTF(1, ("AllocDevice() failed.\n"));
            }
        }
    } else {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR)libname,
                       "GET_PORT_STATUS failed: %s (%ld)",
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        KPRINTF(1, ("GET_PORT_STATUS for port %ld failed %ld.\n", port, ioerr));
    }

    return NULL;
}


/* /// "nHandleHubMethod()" */
void nHandleHubMethod(struct NepClassHubSS *nch, struct NepHubSSMsg *nhm)
{
    ULONG num;
    struct PsdDevice *pd;

    nhm->nhm_Result = 0;

    switch(nhm->nhm_MethodID) {
        case UCM_HubClaimAppBinding:
            nhm->nhm_Result = (IPTR)psdHubClaimAppBindingA((struct TagItem *)nhm->nhm_Params[1]);
            break;

        case UCM_HubReleaseIfBinding:
            psdHubReleaseIfBinding((struct PsdInterface *)nhm->nhm_Params[1]);
            break;

        case UCM_HubReleaseDevBinding:
            psdHubReleaseDevBinding((struct PsdDevice *)nhm->nhm_Params[1]);
            break;

        case UCM_AttemptSuspendDevice: {
            BOOL res = TRUE;

            for(num = 1; num <= nch->nch_NumPorts; num++) {
                if((pd = (nch->nch_Downstream)[num-1])) {
                    res &= psdSuspendDevice(pd);
                }
            }

            if(res) {
                /* Suspending all downstream devices succeeded; stop hub activity too. */
                psdAbortPipe(nch->nch_EP1Pipe);
                nch->nch_Running = FALSE;
                nhm->nhm_Result = TRUE;
            }
            break;
        }

        case UCM_AttemptResumeDevice:
            /* The main loop owns the EP1 pipe: with nch_Running set it will
             * resubmit once the aborted request has drained (nch_IOStarted). */
            nch->nch_Running = TRUE;
            nhm->nhm_Result = TRUE;

            for(num = 1; num <= nch->nch_NumPorts; num++) {
                if((pd = (nch->nch_Downstream)[num-1])) {
                    psdResumeDevice(pd);
                }
            }
            break;

        case UCM_HubSuspendDevice:
            nhm->nhm_Result = nHubSuspendDevice(nch, (struct PsdDevice *)nhm->nhm_Params[1]);
            break;

        case UCM_HubResumeDevice:
            nhm->nhm_Result = nHubResumeDevice(nch, (struct PsdDevice *)nhm->nhm_Params[1]);
            break;

        case UCM_HubResetPort:
            nhm->nhm_Result = nHubResetPort(nch, (struct PsdDevice *)nhm->nhm_Params[1]);
            break;

        default:
            /* Unknown/unsupported method */
            nhm->nhm_Result = 0;
            break;
    }
}


/* /// "nHubSuspendDevice()" */
BOOL nHubSuspendDevice(struct NepClassHubSS *nch, struct PsdDevice *pd)
{
    ULONG num;
    BOOL result = FALSE;
    LONG ioerr;

    /* Binding info is not used here; avoid unused-variable warnings. */
    {
        APTR binding = NULL;
        APTR puc = NULL;
        psdGetAttrs(PGA_DEVICE, pd, DA_Binding, &binding, DA_BindingClass, &puc, TAG_END);
        (void)binding;
        (void)puc;
    }

    for(num = 1; num <= nch->nch_NumPorts; num++) {
        if(pd == (nch->nch_Downstream)[num-1]) {
            /* raw USB3 hub protocol: suspend = set link state U3 */
            psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                         USR_SET_FEATURE, UFS_PORT_LINK_STATE,
                         (ULONG)(num | (UPLS_U3 << 8)));
            ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);

            if(ioerr) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                               "SET_PORT_SUSPEND failed: %s (%ld)",
                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                KPRINTF(1, ("SET_PORT_SUSPEND failed %ld.\n", ioerr));
            } else {
                result = TRUE;
                psdSetAttrs(PGA_DEVICE, pd, DA_IsSuspended, TRUE, TAG_END);
                psdSendEvent(EHMB_DEVSUSPENDED, pd, NULL);
            }
            break; /* done */
        }
    }

    return result;
}


/* /// "nHubResetPort()" */
/*
 * Hot-reset the port a bound downstream device sits on (UCM_HubResetPort,
 * the psdResetDevice() port-reset half).  Unlike nConfigurePort's reset this
 * neither allocates nor enumerates anything: the caller keeps the device and
 * re-addresses it through the HCD (NSCMD_USB_RESET_DEVICE).  Runs in the hub
 * task, so it cannot race this hub's own port-change processing; the change
 * bits are cleared before returning so the change loop never sees an
 * unexplained C_PORT_RESET.
 */
BOOL nHubResetPort(struct NepClassHubSS *nch, struct PsdDevice *pd)
{
    struct UsbPortStatus uhps;
    ULONG num;
    LONG ioerr;
    LONG delayretries;

    for(num = 1; num <= nch->nch_NumPorts; num++) {
        if(pd != (nch->nch_Downstream)[num-1]) {
            continue;
        }

        psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                     USR_SET_FEATURE, UFS_PORT_RESET, num);
        ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);
        if(ioerr) {
            psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                           "PORT_RESET for port %ld failed: %s (%ld)",
                           num, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
            return FALSE;
        }

        if(nch->nch_IsRootHub) {
            /* Root hubs need 50ms minimum delay */
            psdDelayMS(50);
        }

        for(delayretries = 0; delayretries < 500; delayretries += 10) {
            psdDelayMS(10);

            ioerr = nReadPortStatus(nch, num, &uhps);
            if(ioerr) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                               "GET_PORT_STATUS for port %ld failed: %s (%ld)",
                               num, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                return FALSE;
            }

            if(!(uhps.wPortStatus & UPSF_PORT_CONNECTION)) {
                /* the reset dislodged the device */
                return FALSE;
            }

            if(!(uhps.wPortStatus & UPSF_PORT_RESET) &&
               (uhps.wPortStatus & UPSF_PORT_ENABLE)) {
                nClearPortStatus(nch, num);
                psdDelayMS(100); /* post-reset recovery interval */
                return TRUE;
            }
        }

        psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                       "PORT_RESET for port %ld never completed.", num);
        return FALSE;
    }

    return FALSE; /* not one of this hub's downstream devices */
}


/* /// "nHubResumeDevice()" */
BOOL nHubResumeDevice(struct NepClassHubSS *nch, struct PsdDevice *pd)
{
    ULONG num;
    BOOL result = FALSE;
    LONG ioerr;

    for(num = 1; num <= nch->nch_NumPorts; num++) {
        if(pd == (nch->nch_Downstream)[num-1]) {
            /* raw USB3 hub protocol: resume = set link state U0 */
            psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_OTHER,
                         USR_SET_FEATURE, UFS_PORT_LINK_STATE,
                         (ULONG)(num | (UPLS_U0 << 8)));
            ioerr = psdDoPipe(nch->nch_EP0Pipe, NULL, 0);

            if(ioerr) {
                psdAddErrorMsg(RETURN_WARN, (STRPTR)libname,
                               "CLEAR_PORT_SUSPEND failed: %s (%ld)",
                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                KPRINTF(1, ("CLEAR_PORT_SUSPEND failed %ld.\n", ioerr));
            } else {
                psdSetAttrs(PGA_DEVICE, pd, DA_IsSuspended, FALSE, TAG_END);
                psdSendEvent(EHMB_DEVRESUMED, pd, NULL);
                result = TRUE;
                psdDelayMS(30);
            }
            break; /* done */
        }
    }

    return result;
}
