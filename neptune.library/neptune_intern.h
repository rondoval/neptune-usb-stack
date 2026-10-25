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
 *                  Internal includes for neptune.library
 *----------------------------------------------------------------------------
 * Based on Poseidon's poseidon_intern.h by Chris Hodges <chrisly@platon42.de>.
 *
 */

#ifndef _LIBRARIES_NEPTUNE_H
#define _LIBRARIES_NEPTUNE_H

#include <libraries/neptune.h>
#include <devices/usbhcd_context.h>    /* the context HCD ABI (lifecycle ops) */
#include <hwmatch.h>                   /* pFindHardware()'s notion of "same controller" */

/* The library version is the distribution version - NEPTUNE_VERSION/REVISION come
 * from project(VERSION) in the top-level CMakeLists.txt, the one place it is written. */
#include <neptune_version.h>
#ifndef VERSION_STRING
#define VERSION_STRING   NEP_VER("neptune.library")
#endif

/* Configuration stuff */

/* How this works:
   Configurations are stored in an IFF structure. It is managed by NeptuneUSB
   and the stack itself. Class drivers may only inject, delete or read out
   certain sections of the configuration data. They either get pointer to the
   FORM sections or return pointers to this FORM. The form data has to be
   copied in each case (the length is given in the form).

   The configuration normally looks like this:
   FORM PSDC (Poseidon Config file)

[1*] FORM STKC (Stack config)
[n*]   FORM UDEV (USB hardware device)
         CHNK NAME [varlen] (device name)
         CHNK UNIT [4 bytes]
         [...]
[n*]   FORM UCLS (USB class driver)
         CHNK NAME [varlen] (library name)
         [...]
       [Chunks/Forms] (chunks containing some more data for the main stack file)

[n*] FORM CLSC (Class config)
       CHNK OWNR [varlen] name of class which stores this data
       FORM GCPD (global class private data) <- that's the form the class can modify
         [...]

[n*] FORM DEVC (Device configurations)
       CHNK DVID [varlen] DeviceID-String
       CHNK FBND [varlen] <classname> (forced binding)
       CHNK NAME [varlen] Custom name
[n*]   FORM DCFG (Device configuration data)
         CHNK OWNR [varlen] name of class for binding (AppName?)
         FORM DCPD (device config private data) <- that's the form the class can modify
           [...]
[n*]   FORM ICFG (Interface configuration data)
         CHNK OWNR [varlen] name of class for binding
         CHNK IFID [varlen] InterfaceID-String
         CHNK FBND [varlen] <classname> (forced binding)
         FORM ICPD (interface config private data) <- that's the form the class can modify

    psdReadCfg(pic, formdata):
        pic == NULL; replace (!) root form with given formdata
        pic != NULL; replace given pic with new form data.
    psdWriteCfg(pic)
        pic == NULL; generate the whole configuration buffer to save
        pic != NULL; generate the form buffer of the pic and its subform.
    psdFindCfgForm(pic, formid)
        pic == NULL -> pic = root form;
        find and return pic with given form ID or NULL if no such form exists.
    psdNextCfgForm(pic)
        get next form pic with same ID or return NULL if this was the only form with this ID.

    psdRemCfgForm(pic)
        pic == NULL -> pic = root form;
        delete form pic (effectly calls pFreeForm()).

    psdAddCfgChunk(pic, formdata)
        pic == NULL -> pic = root form;
        add the form OR CHUNK to the pic context form. Do not replace existing forms.
        Replaces existing chunks.

    psdRemCfgChunk(pic, chnkid)
        pic == NULL -> pic = root form;
        delete the chunk from the pic context form.

*/

/* Private stuff starts here */

struct PsdLockSem
{
    struct Node	        pls_Node;         /* Linkage */
    BOOL                pls_Dead;         /* Has Semaphore been deactivated? */
    struct List         pls_WaitQueue;    /* List of waiting tasks (ReadLock structs) */
    struct List         pls_ReadLocks;    /* List of obtained shared locks */
    struct Task        *pls_Owner;        /* Current owner of exclusive lock */
    UWORD               pls_ExclLockCount; /* Exclusive lock count */
    UWORD               pls_SharedLockCount; /* Count of *different* shared lock owners */
};

struct PsdReadLock
{
    struct Node         prl_Node;         /* Linkage */
    BOOL                prl_IsExcl;       /* Is this lock exclusive? */
    struct Task        *prl_Task;         /* Task waiting for or obtaining this lock */
    UWORD               prl_Count;        /* Shared lock count */
};

struct PsdSemaInfo
{
    struct Node         psi_Node;         /* Linkage */
    struct PsdLockSem  *psi_LockSem;      /* Pointer to semaphore */
};

struct PsdBorrowLock
{
    struct Node         pbl_Node;         /* Linkage */
    UWORD               pbl_ExclLockCount; /* Was exclusive before */
    UWORD               pbl_Count;        /* Shared lock count */
    struct PsdLockSem  *pbl_LockSem;      /* Pointer to semaphore */
    struct PsdReadLock *pbl_ReadLock;     /* Readlock that was changed */
};

struct PsdNotify
{
    struct Task        *nt_Task;          /* Notifier task */
    struct MsgPort     *nt_MsgPort;       /* Port for EventNote messages */
    LONG                nt_ReadySignal;   /* Signal to respond to task changes */
    struct Task        *nt_ReadySigTask;  /* task to signal */
    struct Library     *nt_IntBase;       /* Intuition base for notifier task */
    struct Library     *nt_MUIBase;       /* MUI base for notifier task */
    struct Library     *nt_DTBase;        /* DataTypes base for notifier task */
    struct MsgPort     *nt_TimerMsgPort;  /* Standard timer MsgPort */
    struct timerequest *nt_TimerIOReq;    /* Standard timer request */
    struct MUI_CustomClass *nt_NotifyClass; /* Notifier action class */
    ULONG              *nt_NotifyObj;       /* Notifier action object */
    ULONG              *nt_AppObj;        /* App Object */
    ULONG              *nt_WindowObj;     /* Window Object */
    ULONG              *nt_GroupObj;      /* Group Object */
    ULONG              *nt_SaveObj;       /* Save Button Object */
    ULONG              *nt_CloseObj;      /* Close Button Object */
    ULONG              *nt_StickyObj;     /* Sticky Object */
    ULONG              *nt_AboutMI;       /* About MenuItem */
    ULONG              *nt_CloseMI;       /* Close MenuItem */
    ULONG              *nt_PrefsMI;     /* Prefs editor MenuItem */
    ULONG              *nt_MUIPrefsMI;    /* MUI Prefs MenuItem */
    struct PsdEventHook *nt_EventHandler; /* Event handler */
    struct List         nt_GadgetList;    /* List of gadgets for a device */
    BOOL                nt_OpenRequest;   /* open window requested */
    BOOL                nt_Sticky;        /* sticky entries */
    STRPTR              nt_InsertSndFile; /* Path to insertion sound file */
    STRPTR              nt_RemoveSndFile; /* Path to removal sound file */
    struct List         nt_Sounds;        /* List of loaded soundfiles */
};

struct PsdHandlerTask
{
    struct Task        *ph_Task;          /* Event Handler Task */
    struct MsgPort     *ph_MsgPort;       /* Port for EventNote messages */
    LONG                ph_ReadySignal;   /* Signal to respond to task changes */
    struct Task        *ph_ReadySigTask;  /* task to signal */
    struct PsdEventHook *ph_EventHandler; /* Event handler */
    struct MsgPort     *ph_TimerMsgPort;  /* Port for timer requests */
    struct timerequest *ph_TimerIOReq;    /* Standard timer request */
};

struct PsdWStringMap
{
    WORD   psm_ID;
    STRPTR psm_String;
};

struct PsdUWStringMap
{
    UWORD  psm_ID;
    STRPTR psm_String;
};

struct PsdULStringMap
{
    ULONG  psm_ID;
    STRPTR psm_String;
};

/* The library node - private
*/
struct PsdBase
{
    struct Library      ps_Library;       /* standard */
    BPTR                ps_SegList;       /* our load seglist (for expunge) */
    UWORD               ps_Flags;         /* various flags */
    struct UtilityBase *ps_UtilityBase;   /* for tags etc */
    struct Library     *ps_DosBase;       /* for dos stuff */
    BOOL                ps_StackInit;     /* Did we initialize the stack yet? */
    APTR                ps_MemPool;       /* Public Memory Pool */
    APTR                ps_SemaMemPool;   /* Memory Pool exclusively for Semaphore ReadLocks */
    struct List         ps_DeadlockDebug; /* linked list of semaphore allocations */
    struct PsdLockSem   ps_Lock;          /* PBase lock */
    struct PsdLockSem   ps_ConfigLock;    /* Config semaphore */
    struct timerequest  ps_TimerIOReq;    /* Standard timer request */
    struct List         ps_Hardware;      /* List of Hardware Interfaces in use */
    struct List         ps_Classes;       /* List of Classes loaded */
    struct List         ps_ErrorMsgs;     /* List of Error Msgs */
    struct List         ps_EventHooks;    /* List of EventHandlers */
    struct MsgPort      ps_EventReplyPort; /* Replyport for Events */
    struct List         ps_ConfigRoot;    /* Configuration FORMs */
    struct List         ps_AlienConfigs;  /* Configuration FORM from outer space */
    BOOL                ps_CfgChangeMute; /* Don't generate config changed events */
    struct SignalSemaphore ps_ReentrantLock; /* Lock for non-reentrant stuff */
    struct SignalSemaphore ps_NotifyLock;   /* Lock for non-reentrant stuff */
    struct SignalSemaphore ps_StallRecoverySem; /* Guards pd_EpHaltMask (every device).  Leaf-level:
                                             held only for the mask reads/writes themselves, never
                                             across a wire transfer or while taking another lock. */
    struct SignalSemaphore ps_MemPoolSem; /* Guards ps_MemPool and ps_MemAllocated: every task of
                                             the stack allocates there, and exec does not arbitrate
                                             a pool. Leaf-level. */
    ULONG               ps_MemAllocated;  /* Bytes of memory allocated by stack */
    BOOL                ps_ConfigRead;    /* Has a config been loaded? */
    BOOL                ps_CheckConfigReq; /* Set to true, to check if config changed */
    BOOL                ps_LinkPowerReq;  /* Set to true, to re-apply the link power policy */
    BOOL                ps_StallRecoveryReq; /* Set to true, to run the endpoint-stall recovery sweep.
                                             Plain flag, correct on one CPU; SMP needs a full fence
                                             between the sweep's clear and its mask reads. */
    ULONG               ps_ConfigHash;    /* Last config hash value */
    ULONG               ps_SavedConfigHash; /* Hash sum of last saved config */
    struct PsdGlobalCfg *ps_GlobalCfg;    /* Global Config structure */
    struct PsdNotify      ps_Notify;
    ULONG               ps_ReleaseVersion; /* Release Version for update info */
    ULONG               ps_OSVersion;     /* Internal OS Version descriptor */
    BOOL                ps_StartedAsTask; /* Did we start in Task Mode before DOS was available? */
    struct PsdHandlerTask ps_EventHandler; /* Event handler */
};

struct PsdEventHook
{
    struct Node         peh_Node;         /* Node linkage */
    struct MsgPort     *peh_MsgPort;      /* Target message port */
    ULONG               peh_MsgMask;      /* Mask of messages to send */
};

struct PsdEventNote
{
    struct Message      pen_Msg;          /* Intertask communication message */
    UWORD               pen_Event;        /* Event number as specified above */
    APTR                pen_Param1;       /* Parameter 1 for event */
    APTR                pen_Param2;       /* Parameter 2 */
};

struct PsdEventNoteInternal
{
    struct Node         peni_Node;        /* Node linkage */
    struct PsdEventNote peni_EventNote;   /* Encapsulated PsdEventNote */
};

struct PsdErrorMsg
{
    struct Node         pem_Node;         /* Node linkage */
    struct PsdBase     *pem_Base;         /* Uplinking */
    UWORD               pem_Level;        /* RC: 0=Note, 5=Warn, 10=Error, 20=Fail */
    STRPTR              pem_Origin;       /* From whom? */
    STRPTR              pem_Msg;          /* Actual error message */
    struct DateStamp    pem_DateStamp;    /* Date Stamp (if DOS available) */
};

struct PsdIFFContext
{
    struct Node         pic_Node;         /* Node linkage */
    //struct PsdIFFContext *pic_Parent;     /* Uplinking */
    struct List         pic_SubForms;     /* All sub forms */
    ULONG               pic_FormID;       /* 4 bytes FORM ID */
    ULONG               pic_FormLength;   /* Length of form */
    ULONG              *pic_Chunks;       /* Chunks (no forms) */
    ULONG               pic_ChunksLen;    /* Total length of chunks */
    ULONG               pic_BufferLen;    /* size of buffer allocated */
};

struct PsdUsbClass
{
    struct Node         puc_Node;         /* Node linkage */
    struct PsdBase     *puc_Base;         /* Uplinking */
    struct Library     *puc_ClassBase;    /* Library pointer */
    STRPTR              puc_ClassName;    /* Name of class */
    STRPTR              puc_FullPath;     /* Full path and class name */
    UWORD               puc_UseCnt;       /* Number of bindings in use */
    BOOL                puc_RemoveMe;     /* Class scheduled for removal */
};

struct PsdAppBinding
{
    struct Node         pab_Node;         /* Node linkage */
    struct PsdDevice   *pab_Device;       /* Uplinking */
    struct Hook        *pab_ReleaseHook;  /* CallBackHook for releasing binding */
    IPTR                pab_UserData;     /* User Data */
    struct Task        *pab_Task;         /* Task bound to */
    BOOL                pab_ForceRelease; /* Force release of other app or class bindings */
};

/* Lower-edge lifecycle backend - one per PsdHardware, bound by the device task
   after UHCMD_QUERYDEVICE. The legacy backend is the classic software-managed
   behavior (stack picks the address, wire SET_ADDRESS, endpoints implicit); a
   context backend (HCD-owned addressing + explicit endpoint ops) will be selected
   by UHCF_CONTEXT.
   Hooks taking a pipe run in the calling task with the device write-locked;
   hop_UpdateHub runs in whichever task set DA_HubNumPorts (the hub facts it
   reads are only mutated by that same hub task). Hooks returning LONG use
   the UHIOERR_* convention (0 = success).

   The vtable is a total contract: no slot is ever NULL (a backend with nothing
   to do supplies a no-op op, not a NULL). Ops reached only mid-enumeration call
   through phw_HCDOps unguarded; only hop_UpdateHub and hop_DestroyDevice, which
   can fire on a PsdHardware whose backend was never bound, whole-table guard on
   phw_HCDOps != NULL. So NULL means "no backend bound", never "op is empty". */
struct PsdPipe;
struct PsdDevice;
struct PsdInterface;
struct UsbStdDevDesc;

struct PsdHCDOps
{
    /* Make the device addressed and EP0 usable: sets pd_Handle (+ pd_DevAddr on
       legacy), PDFF_HASDEVADDR|PDFF_CONNECTED, retargets pp to the device, and
       returns the first 8 device-descriptor bytes in *usdd. Rolls its own state
       back on failure. */
    LONG (*hop_AddressDevice)(struct PsdBase *ps, struct PsdPipe *pp, struct UsbStdDevDesc *usdd);
    /* Called once pd_MaxPktSize0 has been validated from the 8-byte descriptor. */
    LONG (*hop_UpdateEp0MaxPacket)(struct PsdBase *ps, struct PsdPipe *pp);
    /* Called before the wire SET_CONFIGURATION for cfgnum. */
    LONG (*hop_ConfigureEndpoints)(struct PsdBase *ps, struct PsdPipe *pp, UWORD cfgnum);
    /* Called before the wire SET_INTERFACE selecting pif in place of curif,
       the alternate of the same interface that is in force - and again with
       the two swapped when that request fails, to take the switch back. */
    LONG (*hop_SetInterface)(struct PsdBase *ps, struct PsdPipe *pp, struct PsdInterface *curif, struct PsdInterface *pif);
    /* Called when a device's hub facts (port count / TT think time / multi-TT)
       become known, i.e. when a hub class sets DA_HubNumPorts. */
    void (*hop_UpdateHub)(struct PsdBase *ps, struct PsdDevice *pd);
    /* Device teardown: release backend addressing state. */
    void (*hop_DestroyDevice)(struct PsdBase *ps, struct PsdDevice *pd);
};

struct PsdHardware
{
    struct Node         phw_Node;               /* Node linkage */
    struct PsdBase     *phw_Base;               /* Uplinking */
    struct Task        *phw_ReadySigTask;       /* Task to send ready signal to */
    LONG                phw_ReadySignal;        /* Signal to send when ready */
    struct Task        *phw_Task;               /* Device task */
    STRPTR              phw_DevName;            /* Device name */
    ULONG               phw_Unit;               /* Unit number */

    STRPTR              phw_ProductName;        /* Product name */
    STRPTR              phw_Manufacturer;       /* Manufacturer name */
    STRPTR              phw_Description;        /* Description string */
    STRPTR              phw_Copyright;          /* Copyright string */
    UWORD               phw_Version;            /* Version of device */
    UWORD               phw_Revision;           /* Device revision */
    UWORD               phw_DriverVers;         /* Driver version */
    ULONG               phw_Capabilities;       /* Driver/HW capabilities */
    UWORD               phw_NumRootHubs;        /* Number of root hubs */

    const struct PsdHCDOps *phw_HCDOps;         /* Lower-edge lifecycle backend (legacy, or context per UHCF_CONTEXT) */
    UWORD               phw_ContextBackend;     /* BOOL: context backend bound (exposed as HA_ContextBackend) */
    UWORD               phw_StreamsSupported;   /* BOOL: NSCMD_USB_ALLOC_STREAMS in the NSD list (exposed as HA_StreamsSupported) */
    UWORD               phw_DMAAlignment;       /* HCD-recommended DMA buffer alignment in bytes, 0 = none (exposed as HA_DMAAlignment) */
    ULONG               phw_CtxCmdMask;         /* Bitmask of context ops present in the driver's NSD list (test with UHCD_CTXCMD_BIT) */
    struct Hook         phw_XferDoneHook;       /* Transfer-completion hook (driver task -> ReplyMsg pp_Msg) */
    APTR                phw_CtxHcd;             /* Opaque HCD context from NSCMD_USB_ATTACH (first arg of every entry) */
    APTR                phw_CtxSubmit;          /* UhcdSubmitFunc (bulk/int/iso) from NSCMD_USB_ATTACH */
    APTR                phw_CtxCtrlSubmit;      /* UhcdCtrlSubmitFunc from NSCMD_USB_ATTACH */
    APTR                phw_CtxAbort;           /* UhcdAbortFunc from NSCMD_USB_ATTACH */

    struct IOUsbHWReq  *phw_RootIOReq;          /* First IO Request */

    struct PsdDevice   *phw_RootDevice;         /* Link to root hub of this hardware */
    struct PsdDevice   *phw_DevArray[128];      /* DevAddress->Device mapping (LEGACY backend only: pAllocDevAddr / pLegacyDestroyDevice; context HCDs own addressing) */
    struct List         phw_Devices;            /* List of devices */
    struct List         phw_DeadDevices;        /* List of disconnected devices */
    BOOL                phw_RemoveMe;           /* Hardware scheduled for removal */
    struct MsgPort      phw_DevMsgPort;         /* Quick device message port */
    struct MsgPort      phw_TaskMsgPort;        /* Quick task message port */
    volatile ULONG      phw_MsgCount;           /* Number of Messages pending */

};

/* Flags for pd_Flags */

#define PDFF_LOWSPEED       0x0001
#define PDFF_CONNECTED      0x0002
#define PDFF_HASDEVADDR     0x0004
#define PDFF_HASDEVDESC     0x0008
#define PDFF_CONFIGURED     0x0010
#define PDFF_HIGHSPEED      0x0020
#define PDFF_NEEDSSPLIT     0x0040
#define PDFF_LOWPOWER       0x0080
#define PDFF_DEAD           0x0100
#define PDFF_SUSPENDED      0x0200
#define PDFF_SUPERSPEED     0x0400
#define PDFF_MULTITT        0x0800   /* Hub supports multiple TTs */
#define PDFF_APPBINDING     0x4000
#define PDFF_DELEXPUNGE     0x8000

/* Flags for pd_LpmArmed -- what pLinkPowerArm() actually got onto the wire, so
   that pLinkPowerDisarm() can take exactly that back off again when the policy
   changes.  There is deliberately no "USB2 hardware LPM armed" bit: the HCD
   never reports whether it armed the L1 registers (it decides eligibility from a
   root port capability the stack cannot see), so PDLPMF_CTXOP stands in for the
   whole of the controller side state. */

#define PDLPMF_POLICY       0x0001   /* the last policy decision for this device was ON */
#define PDLPMF_U1DEV        0x0002   /* device accepted SET_FEATURE(DEVICE_U1_ENABLE) */
#define PDLPMF_U2DEV        0x0004   /* ...DEVICE_U2_ENABLE */
#define PDLPMF_LTM          0x0008   /* ...DEVICE_LTM_ENABLE */
#define PDLPMF_PORTU1       0x0010   /* parent hub port U1 inactivity timeout set non-zero */
#define PDLPMF_PORTU2       0x0020   /* ...U2 */
#define PDLPMF_CTXOP        0x0040   /* HCD accepted a non-empty SET_LINK_POWER, so it may
                                        hold controller side state: MEL, root port PORTPMSC
                                        timeouts, USB2 hardware LPM PORTPMSC.HLE */

/* pd_EpHaltMask bit encoding, in one place because five sites read or write it:
   bit n = OUT endpoint n, bit 16+n = IN endpoint n.  Bits 0 and 16 are EP0 and
   are never set - a protocol stall on EP0 self-clears.  MASKADDR takes a USB
   endpoint address (bEndpointAddress layout, URTF_IN = IN), ADDR is its inverse. */
#define PDEPHALT_BIT(epnum, isin)  (((epnum) & 15) + ((isin) ? 16 : 0))
#define PDEPHALT_MASK(epnum, isin) (1UL << PDEPHALT_BIT((epnum), (isin)))
#define PDEPHALT_MASKADDR(epaddr)  PDEPHALT_MASK((epaddr), ((epaddr) & URTF_IN))
#define PDEPHALT_EPNUM(bit)        ((bit) & 15)
#define PDEPHALT_ISIN(bit)         (((bit) & 16) != 0)
#define PDEPHALT_ADDR(bit)         (PDEPHALT_EPNUM(bit) | (PDEPHALT_ISIN(bit) ? URTF_IN : 0))

struct PsdDevice
{
    struct Node         pd_Node;          /* Node linkage */
    struct PsdHardware *pd_Hardware;      /* Interfacing hardware */
    struct PsdLockSem   pd_Lock;          /* Access locking */
    struct PsdDevice   *pd_Hub;           /* Hub for device */
    APTR                pd_DevBinding;    /* Device binding */
    struct PsdUsbClass *pd_ClsBinding;    /* Which class has the bond? */
    struct PsdConfig   *pd_CurrentConfig; /* Direct pointer to currently set config */
    UWORD               pd_UseCnt;        /* Usage counter */
    UWORD               pd_DevAddr;       /* Device address (legacy backend; 0 on context backends) */
    ULONG               pd_Handle;        /* Backend identity token (legacy: == pd_DevAddr; context: opaque HCD handle) */
    APTR                pd_Ep0Token;      /* Context backend: EP0 submit token from NSCMD_USB_CREATE_DEVICE (read per submit - assigned mid-enumeration) */
    UWORD               pd_CurrCfg;       /* Current Configuration Number */
    UWORD               pd_NumCfgs;       /* Number of configurations available */
    UWORD               pd_PowerDrain;    /* Current power usage */
    UWORD               pd_PowerSupply;   /* Power provided from parent */
    UWORD               pd_CurrLangID;    /* Current Language ID */
    UWORD              *pd_LangIDArray;   /* Array of supported languages */
    UWORD               pd_Flags;         /* Lowspeed? */
    UWORD               pd_HubPort;       /* Port number at parent hub */
    UWORD               pd_HubThinkTime;  /* Think time for TT inter-transaction gap */
    UWORD               pd_HubNumPorts;   /* Hub port count (set by the hub classes via DA_HubNumPorts) */
    UWORD               pd_HubHdrDecLat;  /* SS hubs: bHubHdrDecLat, 0.1 us units (DA_HubHdrDecLat) */
    UWORD               pd_HubDelay;      /* SS hubs: wHubDelay in ns (DA_HubDelay) */
    UWORD               pd_USBVers;       /* USB Version */
    UWORD               pd_DevClass;      /* Class code */
    UWORD               pd_DevSubClass;   /* Subclass code */
    UWORD               pd_DevProto;      /* Device protocol code */
    UWORD               pd_MaxPktSize0;   /* Packet size for EP0 */
    UWORD               pd_VendorID;      /* Vendor ID */
    UWORD               pd_ProductID;     /* Product ID */
    UWORD               pd_DevVers;       /* Device release version */
    UWORD               pd_CloneCount;    /* Running Number to distinguish same devices */
    UWORD               pd_DeadCount;     /* Number of timeouts on the device */
    UWORD               pd_IOBusyCount;   /* Busy IO: transfers in flight on pipes with pp_BusyWeight 1 (not interrupt listeners) + running RT-ISO streams */
    UBYTE               pd_Suspending;    /* psdSuspendDevice() is past its bindings' consent and not yet done (pTransferBegin) */
    UBYTE               pd_WakeWanted;    /* a transfer started while pd_Suspending: wake the device once it is parked */
    ULONG               pd_EpHaltMask;    /* Endpoints owed a CLEAR_FEATURE(ENDPOINT_HALT): bit 1-15 =
                                             OUT EPn, bit 17-31 = IN EPn (PDEPHALT_* above).  Set at completion delivery
                                             (pCompletePipe), consumed by the event task's stall
                                             recovery sweep, cancelled when someone else clears the
                                             halt (pSubmitPipe snoop / psdClearEndpointHalt).
                                             Writes under ps_StallRecoverySem; reads may peek
                                             unlocked.  Portability: that peek assumes one CPU
                                             (an aligned ULONG load cannot tear) - SMP wants an
                                             atomic load. */
    struct timeval      pd_LastActivity;  /* Timestamp of last IO access (start or end) */
    STRPTR              pd_MnfctrStr;     /* Manufacturer string */
    STRPTR              pd_ProductStr;    /* Product string (custom?) */
    STRPTR              pd_OldProductStr; /* Original product string */
    STRPTR              pd_SerNumStr;     /* Serial number string */
    STRPTR              pd_IDString;      /* Whole Device ID string */
    struct List         pd_Configs;       /* List of configurations */
    BOOL                pd_IsNewToMe;     /* Whether the device is connected the first time */
    struct PsdNotifyCfg   pd_NotifyCfg;       /* Inhibit PopUp and Class scan Config */
    struct List         pd_Descriptors;   /* Descriptors collected */
    struct List         pd_RTIsoHandlers; /* List of RTIsoHandlers */

    /* BOS-derived capability summary */
    BOOL                pd_Usb20LpmCapable;        /* Device supports USB 2.0 LPM (L1) */
    BOOL                pd_Usb20BeslCapable;       /* USB2-ext bit 2: BESL/alt-HIRD supported */
    BOOL                pd_Usb20BeslBaselineValid; /* USB2-ext bit 3: baseline BESL value valid */
    UBYTE               pd_Usb20BeslBaseline;      /* USB2-ext bits 11:8: baseline BESL (0..15) */
    BOOL                pd_Usb20BeslDeepValid;     /* USB2-ext bit 4: deep BESL value valid */
    UBYTE               pd_Usb20BeslDeep;          /* USB2-ext bits 15:12: deep BESL (0..15) */
    BOOL                pd_Usb30LtmCapable;        /* Device supports USB 3.0 Latency Tolerance Messaging (LTM) */
    UWORD               pd_SupportedSpeeds;        /* Bitmask of speeds (USB 3.0 wSpeedSupported) */
    UBYTE               pd_Usb30U1ExitLat;         /* Exit latency to U0 from U1 */
    UWORD               pd_Usb30U2ExitLat;         /* Exit latency to U0 from U2 */
    UBYTE               pd_MaxUsbSpeed;            /* store our chosen max speed */
                                                   /* e.g., 1=LS, 2=FS, 3=HS, 4=SS */
    BOOL                pd_HasContainerId;
    UBYTE               pd_ContainerId[16];
    UWORD               pd_LpmArmed;               /* PDLPMF_*: live link power state on the wire */
};

struct PsdDescriptor
{
    struct Node         pdd_Node;         /* Node linkage */
    struct PsdDevice   *pdd_Device;       /* Up linkage */
    struct PsdConfig   *pdd_Config;       /* Up linkage (optional, depending on type of desc) */
    struct PsdInterface *pdd_Interface;   /* Up linkage (optional, depending on type of desc) */
    struct PsdEndpoint *pdd_Endpoint;     /* Up linkage (optional, depending on type of desc) */
    STRPTR              pdd_Name;         /* Supposed Descriptor Type Name */
    UWORD               pdd_Type;         /* Descriptor Type */
    UWORD               pdd_Length;       /* Number of bytes in descriptor */
    UWORD               pdd_CSSubType;    /* Subtype of ClassSpecific Descriptor */
    UBYTE              *pdd_Data;         /* Pointer to data */
};

struct PsdConfig
{
    struct Node         pc_Node;          /* Node linkage */
    struct PsdDevice   *pc_Device;        /* Up linkage */
    UWORD               pc_CfgNum;        /* Config number */
    UWORD               pc_Attr;          /* Attributes */
    UWORD               pc_MaxPower;      /* MaxPower (in mA) */
    UWORD               pc_NumIfs;        /* Number of interfaces */
    STRPTR              pc_CfgStr;        /* Name of config */
    struct List         pc_Interfaces;    /* List of interfaces */
};

struct PsdInterface
{
    struct Node         pif_Node;         /* Node linkage */
    struct PsdConfig   *pif_Config;       /* Up linkage */
    APTR                pif_IfBinding;    /* Interface Binding */
    struct PsdUsbClass *pif_ClsBinding;   /* Which class has the bond? */
    UWORD               pif_IfNum;        /* Interface number */
    UWORD               pif_Alternate;    /* Alternate setting */
    UWORD               pif_NumEPs;       /* Number of Endpoints */
    UWORD               pif_IfClass;      /* Interface Class */
    UWORD               pif_IfSubClass;   /* Interface Subclass */
    UWORD               pif_IfProto;      /* Interface Protocol */
    STRPTR              pif_IfStr;        /* Interface String */
    STRPTR              pif_IDString;     /* Interface ID String */
    struct PsdInterface *pif_ParentIf;    /* If interface is an alternate, uplink */
    struct List         pif_AlterIfs;     /* List of alternate interface not in use */
    struct List         pif_EPs;          /* List of endpoints */
};

struct PsdEndpoint
{
    struct Node         pep_Node;         /* Node linkage */
    struct PsdInterface *pep_Interface;   /* Up linkage */
    UWORD               pep_EPNum;        /* Endpoint address */
    UWORD               pep_Direction;    /* Direction (0=OUT, 1=IN) */
    UWORD               pep_TransType;    /* TransferType, see USEA-Flags */
    UWORD               pep_MaxPktSize;   /* Maximum packet size for EP */
    UWORD               pep_NumTransMuFr; /* Number of transactions per microframe */
    UWORD               pep_Interval;     /* Interval for polling in ms */
    UWORD               pep_IntervalRaw;  /* bInterval exactly as in the descriptor (context HCDs re-encode per xHCI rules) */
    UWORD               pep_SyncType;     /* Iso Synchronization Type */
    UWORD               pep_UsageType;    /* Iso Usage Type */
    UWORD               pep_MaxBurst;     /* Superspeed companion: bursts per service interval */
    UWORD               pep_CompAttributes; /* Superspeed companion: bmAttributes */
    ULONG               pep_BytesPerInterval; /* Superspeed companion: bytes per service interval */
    UWORD               pep_StreamBase;   /* USB3 stream base (0 = default stream) */
    UWORD               pep_MaxStreams;   /* USB3 stream count (0 = not supported) */
    UWORD               pep_StreamsAlloc; /* Context backend: stream ids 1..n the HCD holds rings for (NSCMD_USB_ALLOC_STREAMS); 0 = single-ring */
    APTR                pep_Token;        /* Context backend: HCD submit token (NSCMD_USB_CONFIGURE_ENDPOINTS ed_Token); NULL until configured */
};

/* Flags for pp_Flags */
#define PFF_INPLACE     0x0001            /* streams: buffer is in place, needs no copying */
#define PFF_ACTIVITY    0x0002            /* pActivityBegin() counted this transfer; consumed by the
                                             pActivityEnd() at completion delivery (pCompletePipe) */

struct PsdPipe
{
    struct Message      pp_Msg;           /* Intertask communication message */
    struct PsdDevice   *pp_Device;        /* Up linkage */
    struct PsdEndpoint *pp_Endpoint;      /* Endpoint linkage or NULL for default pipe */
    struct MsgPort     *pp_MsgPort;       /* Msg Port of task allocated pipe */
    struct PsdPipe     *pp_AbortPipe;     /* Pipe to abort */
    ULONG               pp_Num;           /* internal pipe number (used for streams) */
    UWORD               pp_StreamID;      /* USB3 StreamID (0 = default) */
    UWORD               pp_Flags;         /* internal flags (used for streams) */
    UWORD               pp_BusyWeight;    /* 1: transfers on this pipe count in pd_IOBusyCount; 0: interrupt endpoint (a parked listener is not busy IO) */
    UWORD               pp_Pad0;          /* keeps pp_WireReq longword-aligned */
    struct IORequest   *pp_WireReq;       /* the message request in flight (legacy: &pp_IOReq; context ops: one of pp_Ctx; NULL: direct submit) */
    struct IOUsbHWReq   pp_IOReq;         /* the library's pipe state + the legacy wire request */
    union
    {                                     /* context-backend message framings (ops only - transfers are direct calls) */
        struct IOStdReq ppc_Std;          /* lifecycle ops (io_Data -> Uhcd* op block) */
        struct
        {                                 /* clock-driven iso-hook ops (ABI doc 10.3) */
            struct IOStdReq     ppcr_Std; /* wire request */
            struct UhcdIsoHooks ppcr_Op;  /* io_Data payload - must outlive the submit */
        }               ppc_RtIso;
    }                   pp_Ctx;
};

/* Flags for pps_Flags */
#define PSFF_ASYNCIO     0x0001           /* async task (r/w) */
#define PSFF_SHORTTERM   0x0002           /* terminate read on short packet (r) */
#define PSFF_READAHEAD   0x0004           /* read from usb device until buffer full (r) */
#define PSFF_BUFFERREAD  0x0008           /* reads smaller than maxpktsize are buffered (r) */
#define PSFF_BUFFERWRITE 0x0010           /* writes smaller than maxpktsize are buffered -- flush required (w) */
#define PSFF_NOSHORTPKT  0x0020           /* don't terminate writes with a short packet (w) */
#define PSFF_NAKTIMEOUT  0x0040           /* enable nak timeout (r/w) */
#define PSFF_ALLOWRUNT   0x0080           /* allow reading of runt packets (r) */
#define PSFF_DONOTWAIT   0x0100           /* non blocking IO (r) */
#define PSFF_OWNMSGPORT  0x8000           /* internal flag */

struct PsdPipeStream
{
    struct Node         pps_Node;         /* Node linkage */
    struct PsdDevice   *pps_Device;       /* Up linkage */
    struct PsdEndpoint *pps_Endpoint;     /* Endpoint linkage */
    struct MsgPort     *pps_MsgPort;      /* Msg Port of task allocated pipe */
    struct PsdPipe    **pps_Pipes;        /* Array of pipes */
    struct List         pps_FreePipes;    /* Inactive pipes */
    struct List         pps_ReadyPipes;   /* Ready pipes */
    struct Task        *pps_AsyncTask;    /* Task used for asynchroneous transfers */
    struct SignalSemaphore pps_AccessLock; /* Semaphore for reading & writing */
    struct PsdPipe     *pps_ActivePipe;   /* Pipe currently active (w) */
    ULONG               pps_NumPipes;     /* Number of pipes */
    ULONG               pps_NakTimeoutTime; /* Nak Timeout time for pipe */
    ULONG               pps_AbortSigMask; /* Signal mask for waking up read or write */
    UWORD               pps_Flags;        /* transfer flags */

    UBYTE              *pps_Buffer;       /* globally allocated buffer for all pipes */
    ULONG               pps_BufferSize;   /* Size of each pipe buffer */
    ULONG               pps_Offset;       /* read or write offset into the first returned buffer */
    ULONG               pps_BytesPending; /* bytes in read or write buffer */
    ULONG               pps_ReqBytes;     /* number of bytes currently requested */
    LONG                pps_Error;        /* last error occurred */
    UBYTE              *pps_TermArray;    /* termination char array */
};

struct PsdRTIsoHandler
{
    struct Node         prt_Node;         /* Node linkage */
    struct PsdDevice   *prt_Device;       /* Up linkage */
    struct PsdEndpoint *prt_Endpoint;     /* Endpoint linkage */
    struct PsdPipe     *prt_Pipe;         /* Pipe */
    struct Hook        *prt_ReleaseHook;  /* Hook to be called when device gets removed */
    struct IOUsbHWRTIso prt_RTIso;        /* RT Iso structure (the classic class-facing block) */
    struct USBIsoHooks  prt_IsoHooks;     /* Context backend: the wire hook block (filled from prt_RTIso at marshal; uih_Object = &prt_RTIso keeps class hooks unchanged) */
    UWORD               prt_HookFlags;    /* UHCD_IHF_* for that block (RTA_ReportInErrors) */
    BOOL                prt_Running;      /* Started: holds one pd_IOBusyCount until stopped or freed */
};

/* Summary of BOS capabilities for one device */
struct PsdBosCaps
{
    BOOL   hasBos;

    BOOL   hasUsb20Ext;
    BOOL   usb20LpmCapable;        /* bit 1: LPM (L1) capable */
    BOOL   usb20BeslCapable;       /* bit 2: BESL/alt-HIRD supported */
    BOOL   usb20BeslBaselineValid; /* bit 3: baseline BESL value valid */
    UBYTE  usb20BeslBaseline;      /* bits 11:8: baseline BESL (0..15) */
    BOOL   usb20BeslDeepValid;     /* bit 4: deep BESL value valid */
    UBYTE  usb20BeslDeep;          /* bits 15:12: deep BESL (0..15) */

    BOOL   hasSSCap;
    UBYTE  ssBmAttributes;         /* bmAttributes from UsbSSDevCapDesc (LTM etc.) */
    UWORD  ssSpeedsSupported;      /* wSpeedSupported bitmask */
    UBYTE  ssLowestFullFuncSpeed;  /* bFunctionalitySupport */
    UBYTE  ssU1DevExitLat;         /* raw U1 exit latency */
    UWORD  ssU2DevExitLat;         /* raw U2 exit latency */

    BOOL   hasContainerId;
    UBYTE  containerId[16];        /* BOS Container ID GUID */
};

#endif /* _LIBRARIES_NEPTUNE_H */
