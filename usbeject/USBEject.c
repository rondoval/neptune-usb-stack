/*
** USBEject -- Workbench "safely remove hardware" for Neptune
**
** Resident WBStartup daemon: publishes one "Eject <device>" menu item per
** attached device that can be ejected (DA_CanSafeEject), kept current via
** Neptune events. Selecting one runs psdSafeEjectDevice(), which has the
** bound classes flush and cleanly unmount their volumes - a volume with open
** files vetoes the whole eject - and then takes the device off the bus,
** whereupon we tell the user it is safe to unplug.
**
** Two presentations, decided at runtime (see MenuSync): our own "USB" menu in
** the menu bar where workbench.library V45 hierarchical AppMenus work, and
** plain items in the Tools menu everywhere else.  The fallback is what makes
** this work under a Workbench replacement: Directory Opus 5 patches
** AddAppMenuItemA and lists the items in its own Tools menu (source/Library/
** wb.c, source/Program/menus.c), but its emulation ignores the V45 title/key
** tags, so it hands back no key to hang sub-items on.  Note DOpus only shows
** them with "Show Tools menu" enabled in its Environment display options.
*/

#include <exec/exec.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <workbench/workbench.h>
#include <devices/timer.h>
#include <libraries/neptune.h>
#include <libraries/neptune_class.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/wb.h>
#include <proto/locale.h>
#include <proto/neptune.h>
#include <proto/neptune_class.h>

#include <neptune_version.h>

#include "locale.h"

static const char version[] __attribute__((used)) = NEP_VER("USBEject");

#define PORTNAME       "USBEject"
#define LOGNAME        ((STRPTR) "USBEject")   /* originator in the Neptune error log */
#define RETRY_SECS     5   /* AddAppMenuItem() fails while Workbench is not up yet */
#define DETACH_SECS    5   /* how long to wait for the device to drop off the bus */

/* CRT-provided; never define these ourselves (the definition would kill
   libnix's auto-open stub under -fno-common). */
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

struct IntuitionBase *IntuitionBase = NULL;
struct Library *WorkbenchBase = NULL;
struct LocaleBase *LocaleBase = NULL;   /* optional - English defaults if NULL */
struct Library *ps = NULL;

struct EjectDev
{
    struct Node         ed_Node;
    APTR                ed_Device;   /* Neptune pd - a key only, always revalidated */
    ULONG               ed_Id;       /* AppMenuItem id cookie, matched against am_ID */
    struct AppMenuItem *ed_Item;
    char                ed_Name[48];  /* the device, for the requesters */
    char                ed_Label[56]; /* "Eject <device>"; the host may keep the pointer,
                                         so it lives as long as the menu item does */
};

static struct MsgPort *appport = NULL;      /* AppMessages; public port doubling as the single-instance guard */
static BOOL portadded = FALSE;              /* ...but only once AddPort() ran */
static struct MsgPort *eventport = NULL;    /* Neptune event notes */
static struct MsgPort *timerport = NULL;
static struct timerequest *timerreq = NULL;
static BOOL timeropen = FALSE;
static BOOL timerpending = FALSE;
static APTR eventhandler = NULL;
static struct List devlist;
static struct AppMenuItem *titleitem = NULL;
static ULONG titlekey = 0;
static ULONG nextid = 0;
static BOOL dirty = FALSE;
static BOOL nomenuwarned = FALSE;   /* the "no AppMenu" warning is logged once, not every retry */

/* /// "Timer helpers" */
static void TimerStop(void)
{
    if(timerpending)
    {
        AbortIO((struct IORequest *) timerreq);
        WaitIO((struct IORequest *) timerreq);
        timerpending = FALSE;
    }
}

static void TimerStart(ULONG secs)
{
    TimerStop();
    timerreq->tr_node.io_Command = TR_ADDREQUEST;
    timerreq->tr_time.tv_secs = secs;
    timerreq->tr_time.tv_micro = 0;
    SendIO((struct IORequest *) timerreq);
    timerpending = TRUE;
}

/* TRUE exactly once per expiry, so the caller can just ask. */
static BOOL TimerFired(void)
{
    if(timerpending && CheckIO((struct IORequest *) timerreq))
    {
        WaitIO((struct IORequest *) timerreq);
        timerpending = FALSE;
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "DrainEvents()" */
/* Reply every queued event note. Relevant events mark the menu dirty.
   Returns TRUE if an EHMB_REMDEVICE for watchpd (if any) came through. */
static BOOL DrainEvents(APTR watchpd)
{
    BOOL removed = FALSE;
    struct Message *pen;
    while((pen = GetMsg(eventport)))
    {
        IPTR eid = 0;
        IPTR param1 = 0;
        psdGetAttrs(PGA_EVENTNOTE, pen,
                    ENA_EventID, &eid,
                    ENA_Param1, &param1,
                    TAG_END);
        ReplyMsg(pen);
        switch(eid)
        {
            case EHMB_REMDEVICE:
                if(watchpd && ((APTR) param1 == watchpd))
                {
                    removed = TRUE;
                }
                /* fall through */
            case EHMB_ADDBINDING:
            case EHMB_REMBINDING:
                dirty = TRUE;
                break;
        }
    }
    return(removed);
}
/* \\\ */

/* /// "MenuTeardown()" */
static void MenuTeardown(void)
{
    struct EjectDev *ed;
    while((ed = (struct EjectDev *) RemHead(&devlist)))
    {
        if(ed->ed_Item)
        {
            RemoveAppMenuItem(ed->ed_Item);
        }
        FreeVec(ed);
    }
    if(titleitem)
    {
        RemoveAppMenuItem(titleitem);
        titleitem = NULL;
    }
    titlekey = 0;
}
/* \\\ */

/* /// "MenuSync()" */
/* Full rebuild from the current device list. Simple and self-healing: a
   couple of AddAppMenuItem() calls per attached drive, on rare events only.
   Stale ids in AppMessages already in flight simply match nothing. */
static void MenuSync(void)
{
    MenuTeardown();

    psdLockReadPBase();
    APTR pd = NULL;
    while((pd = psdGetNextDevice(pd)))
    {
        IPTR caneject = FALSE;
        STRPTR prodname = NULL;
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_CanSafeEject, &caneject,
                    DA_ProductName, &prodname,
                    TAG_END);
        if(caneject)
        {
            struct EjectDev *ed;
            if((ed = (struct EjectDev *) AllocVec(sizeof(struct EjectDev), MEMF_CLEAR)))
            {
                ed->ed_Device = pd;
                psdSafeRawDoFmt(ed->ed_Name, sizeof(ed->ed_Name), "%s",
                                (prodname && *prodname) ? prodname : (STRPTR) _(MSG_FALLBACK_NAME));
                AddTail(&devlist, &ed->ed_Node);
            }
        }
    }
    psdUnlockPBase();

    struct EjectDev *ed = (struct EjectDev *) devlist.lh_Head;
    if(!ed->ed_Node.ln_Succ)
    {
        return;   /* nothing to offer - no "USB" title either */
    }

    /* Probe for hierarchical AppMenus by asking for a title key: a V45
       workbench.library fills one in, a host that only emulates the flat
       AppMenu (Directory Opus) leaves it at zero.  A NULL item, on the other
       hand, means there is no AppMenu to add to at all - Workbench has not
       opened yet - so try again later.  The title stays invisible until its
       first child is added, and is removed again if it turns out we cannot
       hang anything under it. */
    titlekey = 0;
    titleitem = AddAppMenuItem(0, 0, (STRPTR) _(MSG_MENU_TITLE), appport,
                               WBAPPMENUA_GetTitleKey, (IPTR) &titlekey,
                               TAG_DONE);
    if(!titleitem)
    {
        if(!nomenuwarned)
        {
            nomenuwarned = TRUE;
            psdAddErrorMsg(RETURN_WARN, LOGNAME,
                           "Workbench is not open - no USB eject menu yet (retrying every %ld seconds). "
                           "Neptune USB's Eject button works regardless.",
                           (ULONG) RETRY_SECS);
        }
        MenuTeardown();   /* nothing published yet, so this just drops the list */
        TimerStart(RETRY_SECS);
        return;
    }
    nomenuwarned = FALSE;
    if(!titlekey)
    {
        /* flat host: the items go into the Tools menu, so the title would
           only sit there doing nothing */
        RemoveAppMenuItem(titleitem);
        titleitem = NULL;
    }

    struct TagItem undertitle[2] = { { WBAPPMENUA_UseKey, titlekey }, { TAG_DONE, 0 } };
    ULONG count = 0;
    while(ed->ed_Node.ln_Succ)
    {
        ed->ed_Id = ++nextid;
        psdSafeRawDoFmt(ed->ed_Label, sizeof(ed->ed_Label), (STRPTR) _(MSG_MENU_EJECT), ed->ed_Name);
        ed->ed_Item = AddAppMenuItemA(ed->ed_Id, 0, ed->ed_Label, appport,
                                      titlekey ? undertitle : NULL);
        if(ed->ed_Item)
        {
            count++;
        }
        ed = (struct EjectDev *) ed->ed_Node.ln_Succ;
    }
    psdAddErrorMsg(RETURN_OK, LOGNAME,
                   "%ld drive(s) offered in the %s menu.",
                   count, titlekey ? (STRPTR) _(MSG_MENU_TITLE) : (STRPTR) "Tools");
}
/* \\\ */

/* /// "ShowReq()" */
static void ShowReq(CONST_STRPTR body, CONST_STRPTR arg1, CONST_STRPTR arg2)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0,
                             (STRPTR) "USBEject", (STRPTR) body, (STRPTR) _(MSG_REQ_OK) };
    IPTR args[2];
    args[0] = (IPTR) arg1;
    args[1] = (IPTR) arg2;
    EasyRequestArgs(NULL, &es, NULL, (APTR) args);
}
/* \\\ */

/* /// "DoEject()" */
static void DoEject(ULONG id)
{
    struct EjectDev *ed = (struct EjectDev *) devlist.lh_Head;
    while(ed->ed_Node.ln_Succ)
    {
        if(ed->ed_Id == id)
        {
            break;
        }
        ed = (struct EjectDev *) ed->ed_Node.ln_Succ;
    }
    if(!ed->ed_Node.ln_Succ)
    {
        return;   /* stale menu message - device already gone */
    }

    APTR pd = ed->ed_Device;
    STRPTR devname = ed->ed_Name;   /* the node outlives us: MenuSync runs in the main loop */

    /* The pd is only a key: make sure it is still on the bus before using it
       (menu messages can outlive an unplug). */
    BOOL valid = FALSE;
    psdLockReadPBase();
    APTR chk = NULL;
    while((chk = psdGetNextDevice(chk)) && !valid)
    {
        valid = (chk == pd);
    }
    psdUnlockPBase();
    if(!valid)
    {
        dirty = TRUE;
        return;
    }

    char busyname[64];
    busyname[0] = 0;
    IPTR res = psdSafeEjectDevice(pd, busyname, sizeof(busyname));

    switch(res)
    {
        case SAFEEJECT_OK:
        {
            /* the port disable runs in the hub task, so EHMB_REMDEVICE is the
               confirmation that the device is really gone */
            BOOL removed = FALSE;
            TimerStart(DETACH_SECS);
            while(!removed && !TimerFired())
            {
                Wait((1UL << eventport->mp_SigBit) | (1UL << timerport->mp_SigBit));
                removed = DrainEvents(pd);
            }
            TimerStop();
            ShowReq(removed ? _(MSG_REQ_SAFE) : _(MSG_REQ_NODETACH), devname, NULL);
            break;
        }

        case SAFEEJECT_BUSY:
            ShowReq(_(MSG_REQ_BUSY), devname, busyname);
            break;

        case SAFEEJECT_NOT_SUPPORTED:
            ShowReq(_(MSG_REQ_TOOOLD), devname, NULL);
            break;

        default:
            ShowReq(_(MSG_REQ_FAILED), devname, NULL);
            break;
    }
    dirty = TRUE;
}
/* \\\ */

/* /// "Cleanup()" */
static void Cleanup(void)
{
    if(WorkbenchBase)
    {
        MenuTeardown();
    }
    if(appport)
    {
        /* drain AppMessages that raced the removal (wb.doc requirement) */
        struct Message *msg;
        while((msg = GetMsg(appport)))
        {
            ReplyMsg(msg);
        }
        if(portadded)
        {
            RemPort(appport);
            portadded = FALSE;
        }
        DeleteMsgPort(appport);
        appport = NULL;
    }
    if(eventhandler)
    {
        psdRemEventHandler(eventhandler);   /* drains + replies queued notes */
        eventhandler = NULL;
    }
    if(timeropen)
    {
        TimerStop();
        CloseDevice((struct IORequest *) timerreq);
        timeropen = FALSE;
    }
    if(timerreq)
    {
        DeleteIORequest((struct IORequest *) timerreq);
        timerreq = NULL;
    }
    if(timerport)
    {
        DeleteMsgPort(timerport);
        timerport = NULL;
    }
    if(eventport)
    {
        DeleteMsgPort(eventport);
        eventport = NULL;
    }
    Locale_Deinitialize();
    if(LocaleBase)     { CloseLibrary((struct Library *) LocaleBase);    LocaleBase = NULL; }
    if(ps)             { CloseLibrary(ps);                              ps = NULL; }
    if(WorkbenchBase)  { CloseLibrary(WorkbenchBase);                   WorkbenchBase = NULL; }
    if(IntuitionBase)  { CloseLibrary((struct Library *) IntuitionBase); IntuitionBase = NULL; }
}
/* \\\ */

/* /// "main()" */
int main(int argc, char *argv[])
{
    NewList(&devlist);

    IntuitionBase = (struct IntuitionBase *) OpenLibrary("intuition.library", 39);
    /* AddAppMenuItemA() itself is V36. WBAPPMENUA_GetTitleKey is V45, but MenuSetup() probes for
       it rather than assuming it: a V40 workbench.library ignores the tag, leaves the key at zero,
       and we hang the eject items flat in the Tools menu -- so 36 is the honest floor. */
    WorkbenchBase = OpenLibrary("workbench.library", 36);
    LocaleBase    = (struct LocaleBase *) OpenLibrary("locale.library", 38);
    ps            = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION);
    if((!IntuitionBase) || (!WorkbenchBase) || (!ps))
    {
        Cleanup();
        return(RETURN_FAIL);
    }
    Locale_Initialize();

    if((!(appport = CreateMsgPort())) ||
       (!(eventport = CreateMsgPort())) ||
       (!(timerport = CreateMsgPort())) ||
       (!(timerreq = (struct timerequest *) CreateIORequest(timerport, sizeof(struct timerequest)))))
    {
        Cleanup();
        return(RETURN_FAIL);
    }
    if(OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *) timerreq, 0))
    {
        Cleanup();
        return(RETURN_FAIL);
    }
    timeropen = TRUE;

    /* Single instance: the named AppMessage port is the guard, so claiming the
       name and publishing it have to be one atomic step. */
    appport->mp_Node.ln_Name = (STRPTR) PORTNAME;
    Forbid();
    if(FindPort((STRPTR) PORTNAME))
    {
        Permit();
        Cleanup();
        return(RETURN_WARN);
    }
    AddPort(appport);
    portadded = TRUE;
    Permit();

    eventhandler = psdAddEventHandler(eventport, EHMF_ADDBINDING|EHMF_REMBINDING|EHMF_REMDEVICE);

    psdAddErrorMsg(RETURN_OK, LOGNAME, "Safe-eject menu handler started.");
    MenuSync();

    for(;;)
    {
        ULONG sigs = Wait((1UL << appport->mp_SigBit) |
                          (1UL << eventport->mp_SigBit) |
                          (1UL << timerport->mp_SigBit) |
                          SIGBREAKF_CTRL_C);
        if(sigs & SIGBREAKF_CTRL_C)
        {
            break;
        }
        DrainEvents(NULL);
        if(TimerFired())
        {
            dirty = TRUE;   /* time to retry the menu */
        }
        struct AppMessage *amsg;
        while((amsg = (struct AppMessage *) GetMsg(appport)))
        {
            /* our ids start at 1, so 0 means "not one of our menu items" */
            ULONG id = (amsg->am_Type == AMTYPE_APPMENUITEM) ? amsg->am_ID : 0;
            ReplyMsg((struct Message *) amsg);   /* reply BEFORE the long eject */
            if(id)
            {
                DoEject(id);
            }
        }
        if(dirty)
        {
            dirty = FALSE;
            MenuSync();
        }
    }

    psdAddErrorMsg(RETURN_OK, LOGNAME, "Safe-eject menu handler stopped.");
    Cleanup();
    return(RETURN_OK);
}
/* \\\ */
