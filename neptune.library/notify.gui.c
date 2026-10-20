/*
 * GUI
 */

#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)

#include "debug.h"
#include "neptune.library.h"

#include <libraries/mui.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <libraries/gadtools.h>
#include <datatypes/soundclass.h>

#include <proto/exec.h>

#include <proto/alib.h>

/* ROM-safe per-instance MUI base. MUI's __inline object constructors (the `...End`
 * idiom) resolve MUIMASTER_BASE_NAME at *file* scope, so the per-call-site
 * `#define MUIMasterBase ntf->nt_MUIBase` below - fine for the OpenLibrary/CloseLibrary
 * sites that have `ntf` in scope - is invisible to them. Recover the base from the
 * running task instead: the notifier GUI subtask is spawned (psdSpawnSubTask) with the
 * libbase in tc_UserData. Read exec from $4 ($NOLIBBASE suppresses the SysBase global
 * here. */
#include <exec/execbase.h>
#include <exec/tasks.h>
static inline struct Library *_notify_mui_base(void)
{
    return ((struct PsdBase *) (*(struct ExecBase **)4UL)->ThisTask->tc_UserData)->ps_Notify.nt_MUIBase;
}
#define MUIMASTER_BASE_NAME (_notify_mui_base())

#include <proto/muimaster.h>
/* SDK's __inline MUI_NewObject is broken; shadow it with a va_list version.
 * Also carries the muimaster 19 (MUI 3.8) floor. */
#include "mui_compat.h"
#include <proto/dos.h>
#include <proto/datatypes.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/usbclass.h>

#include "numtostr.h"

#define NewList NEWLIST

#include "notify.gui.h"

#include <stdarg.h>

#define MUIMasterBase ntf->nt_MUIBase
#define DataTypesBase ntf->nt_DTBase
#define IntuitionBase ntf->nt_IntBase

/* /// "pNotifyGUITask()" */
void pNotifyGUITask()
{
    struct Task *thistask;
    struct PsdBase * ps;
    struct PsdNotify *ntf;
    struct List *scrlist;
    struct PubScreenNode *pubscr;
    BOOL hasworkbench = FALSE;

    thistask = FindTask(NULL);

    ps = thistask->tc_UserData;
    SetTaskPri(thistask, 0);
    ntf = &ps->ps_Notify;

    NewList(&ntf->nt_GadgetList);
    NewList(&ntf->nt_Sounds);

    if(!(IntuitionBase = OpenLibrary("intuition.library", 39)))
    {
        KPRINTF(10, ("Couldn't open intuition.library.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    scrlist = LockPubScreenList();
    pubscr = (struct PubScreenNode *) scrlist->lh_Head;
    while(pubscr->psn_Node.ln_Succ)
    {
        if(strcmp(pubscr->psn_Node.ln_Name, "Workbench") == 0)
        {
            if(pubscr->psn_Screen)
            {
                hasworkbench = TRUE;
            }
            break;
        }
        pubscr = (struct PubScreenNode *) pubscr->psn_Node.ln_Succ;
    }
    UnlockPubScreenList();

    if(!hasworkbench)
    {
        KPRINTF(10, ("No screen open yet.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    if(!(MUIMasterBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN)))
    {
        KPRINTF(10, ("Couldn't open muimaster.library.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }
    /* 19 = MUI 3.8, 20 = MUI 4.0, 21 = MUI 5.x. The fleet runs on all three; this is
       the only place that says which one is actually underneath. */
    KPRINTF(10, ("muimaster.library is version %ld.\n", (ULONG) nep_MUIVersion()));

    if(!(DataTypesBase = OpenLibrary("datatypes.library", 39)))
    {
        KPRINTF(10, ("Couldn't open datatypes.library.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    if(!(ntf->nt_MsgPort = CreateMsgPort()))
    {
        KPRINTF(10, ("Couldn't create MsgPort.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    ntf->nt_EventHandler = psdAddEventHandler(ntf->nt_MsgPort, EHMF_ADDDEVICE|EHMF_REMDEVICE|EHMF_ADDBINDING|EHMF_CONFIGCHG|EHMF_DEVRECOVERY);
    if(!ntf->nt_EventHandler)
    {
        KPRINTF(10, ("Couldn't add EventHandler.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    if(!(ntf->nt_TimerMsgPort = CreateMsgPort()))
    {
        KPRINTF(10, ("Couldn't create MsgPort.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    if(!(ntf->nt_TimerIOReq = (struct timerequest *) CreateIORequest(ntf->nt_TimerMsgPort, sizeof(struct timerequest))))
    {
        KPRINTF(10, ("Couldn't create TimerIOReq.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    if(OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *) ntf->nt_TimerIOReq, 0))
    {
        KPRINTF(10, ("Couldn't open timer.device.\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }
    ntf->nt_TimerIOReq->tr_node.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    ntf->nt_TimerIOReq->tr_node.io_Message.mn_Node.ln_Name = "Neptune Notifier";
    ntf->nt_TimerIOReq->tr_node.io_Command = TR_ADDREQUEST;

    if(!(ntf->nt_NotifyClass = MUI_CreateCustomClass(NULL, MUIC_Area, NULL, sizeof(struct DevNotifyData), DevNotifyDispatcher)))
    {
        KPRINTF(10, ("Couldn't create notifier class\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }
    ntf->nt_NotifyClass->mcc_Class->cl_UserData = (IPTR) ps;

    ntf->nt_AppObj = ApplicationObject,
        MUIA_Application_Title      , (IPTR)"Neptune USB device notifications",
        MUIA_Application_Version    , (IPTR)VERSION_STRING,
        MUIA_Application_Copyright  , (IPTR)"(C) 2004-2009 Chris Hodges",
        MUIA_Application_Author     , (IPTR)"Chris Hodges <chrisly@platon42.de>",
        MUIA_Application_Description, (IPTR)"Shows USB device notifications",
        MUIA_Application_Base       , (IPTR)"NEPTUNENOTIFY",
        MUIA_Application_HelpFile   , (IPTR)"HELP:Neptune.guide",
        MUIA_Application_Menustrip  , (IPTR)MenustripObject,
            Child, (IPTR)MenuObjectT((IPTR)"Project"),
                Child, (IPTR)(ntf->nt_AboutMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"About...",
                    MUIA_Menuitem_Shortcut, (IPTR)"?",
                    End),
                Child, (IPTR)MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)NM_BARLABEL,
                    End,
                Child, (IPTR)(ntf->nt_CloseMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"Close",
                    MUIA_Menuitem_Shortcut, (IPTR)"Q",
                    End),
                End,
            Child, (IPTR)MenuObjectT((IPTR)"Settings"),
                Child, (IPTR)(ntf->nt_PrefsMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"Open Neptune USB",
                    MUIA_Menuitem_Shortcut, (IPTR)"O",
                    End),
                Child, (IPTR)MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)NM_BARLABEL,
                    End,
                Child, (IPTR)(ntf->nt_MUIPrefsMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"MUI Settings",
                    MUIA_Menuitem_Shortcut, (IPTR)"M",
                    End),
                End,
            End,

        SubWindow, (IPTR)(ntf->nt_WindowObj = WindowObject,
            MUIA_Window_ID   , MAKE_ID('P','O','P','O'),
            MUIA_Window_Title, (IPTR)"USB device notifications",
            MUIA_HelpNode, (IPTR)"main",

            WindowContents, (IPTR)VGroup,
                Child, (IPTR)(ntf->nt_NotifyObj = NewObject(ntf->nt_NotifyClass->mcc_Class, 0, MUIA_ShowMe, FALSE, TAG_END)),
                Child, (IPTR)(ntf->nt_GroupObj = VGroup,
                    Child, (IPTR)HGroup,
                        Child, (IPTR)HSpace(0),
                        Child, (IPTR)Label("Messages"),
                        Child, (IPTR)HSpace(0),
                        End,
                    End),
                Child, (IPTR)HGroup,
                    Child, (IPTR)(ntf->nt_StickyObj = ImageObject, ImageButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, FALSE,
                        MUIA_ShowSelState, FALSE,
                        End),
                    Child, (IPTR)Label("Hold on"),
                    Child, (IPTR)HGroup,
                        MUIA_Group_SameWidth, TRUE,
                        Child, (IPTR)(ntf->nt_SaveObj = TextObject, ButtonFrame,
                            MUIA_Disabled, (ps->ps_SavedConfigHash == ps->ps_ConfigHash),
                            MUIA_Background, MUII_ButtonBack,
                            MUIA_CycleChain, 1,
                            MUIA_InputMode, MUIV_InputMode_RelVerify,
                            MUIA_Text_Contents, (IPTR)"\33c Save Prefs ",
                            End),
                        Child, (IPTR)(ntf->nt_CloseObj = TextObject, ButtonFrame,
                            MUIA_Background, MUII_ButtonBack,
                            MUIA_CycleChain, 1,
                            MUIA_InputMode, MUIV_InputMode_RelVerify,
                            MUIA_Text_Contents, (IPTR)"\33c Close ",
                            End),
                        End,
                    End,
                End,
            End),
        End;

    if(!ntf->nt_AppObj)
    {
        KPRINTF(10, ("Couldn't create application\n"));
        pNotifyGUITaskCleanup(ps);
        return;
    }

    DoMethod(ntf->nt_WindowObj, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             ntf->nt_AppObj, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(ntf->nt_SaveObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ntf->nt_NotifyObj, 1, MUIM_DevNotify_SavePrefs);
    DoMethod(ntf->nt_CloseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ntf->nt_AppObj, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(ntf->nt_StickyObj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             ntf->nt_NotifyObj, 1, MUIM_DevNotify_Sticky);

    DoMethod(ntf->nt_AboutMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ntf->nt_NotifyObj, 1, MUIM_DevNotify_About);
    DoMethod(ntf->nt_CloseMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ntf->nt_AppObj, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(ntf->nt_PrefsMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ntf->nt_NotifyObj, 1, MUIM_DevNotify_OpenPrefs);
    DoMethod(ntf->nt_MUIPrefsMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ntf->nt_AppObj, 2, MUIM_Application_OpenConfigWindow, 0);


    {
        BOOL isopen = FALSE;
        ULONG sigs;
        ULONG sigmask;
        LONG retid;
        struct PsdNotifyGadgets *pog;
        struct timeval currtime;

        ntf->nt_Task = thistask;
        Forbid();
        if(ntf->nt_ReadySigTask)
        {
            Signal(ntf->nt_ReadySigTask, 1L<<ntf->nt_ReadySignal);
        }
        Permit();

        //set(ntf->nt_WindowObj, MUIA_Window_Open, TRUE);
        ntf->nt_TimerIOReq->tr_time.tv_micro = 500*1000;
        SendIO(&ntf->nt_TimerIOReq->tr_node);
        sigmask = (1UL<<ntf->nt_MsgPort->mp_SigBit)|(1UL<<ntf->nt_TimerMsgPort->mp_SigBit);
        do
        {
            retid = DoMethod(ntf->nt_AppObj, MUIM_Application_NewInput, &sigs);
            if(sigs)
            {
                pEventHandler(ps);
                if(CheckIO(&ntf->nt_TimerIOReq->tr_node))
                {
                    WaitIO(&ntf->nt_TimerIOReq->tr_node);
                    ntf->nt_TimerIOReq->tr_time.tv_micro = 500*1000;
                    SendIO(&ntf->nt_TimerIOReq->tr_node);
                }
                CurrentTime((ULONG *)&currtime.tv_secs, (ULONG *)&currtime.tv_micro);
                pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
                while(pog->pog_Node.ln_Succ)
                {
                    if((currtime.tv_secs > pog->pog_TimeoutTime.tv_secs) ||
                       ((currtime.tv_secs == pog->pog_TimeoutTime.tv_secs) &&
                        (currtime.tv_micro > pog->pog_TimeoutTime.tv_micro)))
                    {
                        pFreeNotifyGadgets(ps, pog);
                        pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
                    } else {
                        pog = (struct PsdNotifyGadgets *) pog->pog_Node.ln_Succ;
                    }
                }

                if(ntf->nt_GadgetList.lh_Head->ln_Succ)
                {
                    if(ntf->nt_OpenRequest)
                    {
                        if(!isopen)
                        {
                            if(ps->ps_GlobalCfg->pgc_PopupActivateWin)
                            {
                                set(ntf->nt_WindowObj, MUIA_Window_Activate, TRUE);
                            } else {
                                set(ntf->nt_WindowObj, MUIA_Window_Activate, FALSE);
                            }
                            set(ntf->nt_StickyObj, MUIA_Selected, FALSE);
                            set(ntf->nt_WindowObj, MUIA_Window_Open, TRUE);
                            isopen = TRUE;
                        } else {
                            if(ps->ps_GlobalCfg->pgc_PopupWinToFront)
                            {
                                DoMethod(ntf->nt_WindowObj, MUIM_Window_ToFront);
                            }
                        }
                        ntf->nt_OpenRequest = FALSE;
                    }
                } else {
                    if(isopen)
                    {
                        set(ntf->nt_WindowObj, MUIA_Window_Open, FALSE);
                        isopen = FALSE;
                    }
                }

                sigs = Wait(sigs | sigmask | SIGBREAKF_CTRL_C);
                if(sigs & SIGBREAKF_CTRL_C)
                {
                    break;
                }
            }
            if(retid == MUIV_Application_ReturnID_Quit)
            {
                set(ntf->nt_WindowObj, MUIA_Window_Open, FALSE);
                /* remove all thingies */
                pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
                while(pog->pog_Node.ln_Succ)
                {
                    pFreeNotifyGadgets(ps, pog);
                    pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
                }
            }
            if((ps->ps_GlobalCfg->pgc_PopupDeviceNew == PGCP_NEVER) &&
               (!ps->ps_GlobalCfg->pgc_PopupDeviceDeath) &&
               (!ps->ps_GlobalCfg->pgc_PopupDeviceGone))
            {
                psdAddErrorMsg0(RETURN_OK, (STRPTR) "Neptune Notifier", "No notifications left to show; exiting.");
                break;
            }

        } while(TRUE);
        set(ntf->nt_WindowObj, MUIA_Window_Open, FALSE);
    }
    pNotifyGUITaskCleanup(ps);
}
/* \\\ */

/* /// "pNotifyGUITaskCleanup()" */
void pNotifyGUITaskCleanup(struct PsdBase * ps)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    struct PsdNotifyGadgets *pog;
    struct PsdNotifySound *pps;

    pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
    while(pog->pog_Node.ln_Succ)
    {
        pFreeNotifyGadgets(ps, pog);
        pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
    }

    pps = (struct PsdNotifySound *) ntf->nt_Sounds.lh_Head;
    while(pps->pps_Node.ln_Succ)
    {
        pNotifyFreeSound(ps, pps);
        pps = (struct PsdNotifySound *) ntf->nt_Sounds.lh_Head;
    }

    if(ntf->nt_TimerIOReq)
    {
        if(ntf->nt_TimerIOReq->tr_node.io_Device)
        {
            AbortIO(&ntf->nt_TimerIOReq->tr_node);
            WaitIO(&ntf->nt_TimerIOReq->tr_node);
            CloseDevice((struct IORequest *) ntf->nt_TimerIOReq);
        }
        DeleteIORequest((struct IORequest *) ntf->nt_TimerIOReq);
        ntf->nt_TimerIOReq = NULL;
    }
    if(ntf->nt_TimerMsgPort)
    {
        DeleteMsgPort(ntf->nt_TimerMsgPort);
        ntf->nt_TimerMsgPort = NULL;
    }

    if(ntf->nt_EventHandler)
    {
        psdRemEventHandler(ntf->nt_EventHandler);
        ntf->nt_EventHandler = NULL;
    }
    if(ntf->nt_MsgPort)
    {
        DeleteMsgPort(ntf->nt_MsgPort);
        ntf->nt_MsgPort = NULL;
    }
    if(ntf->nt_AppObj)
    {
        MUI_DisposeObject(ntf->nt_AppObj);
        ntf->nt_AppObj = NULL;
    }
    if(ntf->nt_NotifyClass)
    {
        MUI_DeleteCustomClass(ntf->nt_NotifyClass);
        ntf->nt_NotifyClass = NULL;
    }
    if(DataTypesBase)
    {
        CloseLibrary(DataTypesBase);
        DataTypesBase = NULL;
    }
    if(MUIMasterBase)
    {
        CloseLibrary(MUIMasterBase);
        MUIMasterBase = NULL;
    }
    if(IntuitionBase)
    {
        CloseLibrary(IntuitionBase);
        IntuitionBase = NULL;
    }
    Forbid();
    ntf->nt_Task = NULL;
    if(ntf->nt_ReadySigTask)
    {
        Signal(ntf->nt_ReadySigTask, 1L<<ntf->nt_ReadySignal);
    }
}
/* \\\ */

/* /// "pNotifyLoadSound()" */
struct PsdNotifySound * pNotifyLoadSound(struct PsdBase * ps, STRPTR name)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    struct PsdNotifySound *pps;
    if((pps = psdAllocVec(sizeof(struct PsdNotifySound))))
    {
        if((pps->pps_Node.ln_Name = psdCopyStr(name)))
        {
            AddTail(&ntf->nt_Sounds, &pps->pps_Node);
            pps->pps_DTHandle = NewDTObject(name,
                                        DTA_SourceType, DTST_FILE,
                                        DTA_GroupID, GID_SOUND,
                                        SDTA_Cycles, 1L,
                                        TAG_END);
            if(!pps->pps_DTHandle)
            {
                psdAddErrorMsg(RETURN_ERROR, "Neptune Notifier", "Could not load soundfile '%s'.", name);
            }
            return(pps);
        }
        psdFreeVec(pps);
    }
    return(NULL);
}
/* \\\ */

/* /// "pNotifyPlaySound()" */
BOOL pNotifyPlaySound(struct PsdBase * ps, STRPTR name)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    struct PsdNotifySound *pps;
    struct dtTrigger playmsg;

    pps = (struct PsdNotifySound *) FindName(&ntf->nt_Sounds, name);
    if(!pps)
    {
        pps = pNotifyLoadSound(ps, name);
    }
    if(!pps)
    {
        return(FALSE);
    }
    if(!pps->pps_DTHandle)
    {
        return(FALSE);
    }

    SetAttrs(pps->pps_DTHandle,
             SDTA_Volume, 64,
             TAG_END);
    playmsg.MethodID     = DTM_TRIGGER;
    playmsg.dtt_GInfo    = NULL;
    playmsg.dtt_Function = STM_PLAY;
    playmsg.dtt_Data     = NULL;
    DoMethodA(pps->pps_DTHandle, (Msg) &playmsg);
    return(TRUE);
}
/* \\\ */

/* /// "pNotifyFreeSound()" */
void pNotifyFreeSound(struct PsdBase * ps, struct PsdNotifySound *pps)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    Remove(&pps->pps_Node);
    if(pps->pps_DTHandle)
    {
        DisposeDTObject(pps->pps_DTHandle);
    }
    psdFreeVec(pps->pps_Node.ln_Name);
    psdFreeVec(pps);
}
/* \\\ */

/* /// "pRemoveOldBox()" */
void pRemoveOldBox(struct PsdBase * ps, struct PsdDevice *pd)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    struct PsdNotifyGadgets *pog;
    // try to remove adding message, if it still existing
    pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
    while(pog->pog_Node.ln_Succ)
    {
        if(pog->pog_Device == pd)
        {
            pFreeNotifyGadgets(ps, pog);
            pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
        } else {
            pog = (struct PsdNotifyGadgets *) pog->pog_Node.ln_Succ;
        }
    }
}
/* \\\ */

/* /// "pGenerateAddBox()" */
struct PsdNotifyGadgets * pGenerateAddBox(struct PsdBase * ps, struct PsdDevice *pd, struct PsdNotifyGadgets *pog)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    STRPTR body = NULL;
    STRPTR bindingsstr;
    STRPTR bindinginfo;
    STRPTR bodyinfo;
    STRPTR wholebodyinfo;
    STRPTR gad[4] = { NULL, NULL, NULL, NULL };
    STRPTR lpwarn;
    STRPTR deadwarn;
    ULONG  configstate;
    BOOL   lowpower = FALSE;
    BOOL   isdead = FALSE;
    BOOL   updatetime = FALSE;
    STRPTR oldbody = NULL;
    UWORD  cnt;
    ULONG  methods[4] = { MUIM_DevNotify_ConfigureBinding, MUIM_DevNotify_ConfigureClass, MUIM_DevNotify_ShutUp, MUIM_DevNotify_RemInfo };

    bindingsstr = pBindingsString(ps, pd);
    configstate = pCheckConfigurable(ps, pd);

    lowpower = pd->pd_Flags & PDFF_LOWPOWER;
    isdead = pd->pd_Flags & PDFF_DEAD;

    body = "A new USB device was detected:\n\n\33b%s\33n\n\n";
    if(lowpower)
    {
        lpwarn = "\n\n\33bWARNING: Detected low power situation!";
    } else {
        lpwarn = "";
    }
    if(isdead)
    {
        deadwarn = "\n\n\33bWARNING: Device seems to be dead!";
    } else {
        deadwarn = "";
    }

    if(bindingsstr)
    {
        bindinginfo = psdCopyStrFmt("It is bound to \33b%s\33n", bindingsstr);
    } else {
        bindinginfo = psdCopyStr("No class has claimed it yet.");
    }
    if(!bindinginfo)
    {
        psdFreeVec(bindingsstr);
        return(NULL);
    }
    if(!(bodyinfo = psdCopyStrFmt(body, pd->pd_ProductStr)))
    {
        psdFreeVec(bindingsstr);
        psdFreeVec(bindinginfo);
        return(NULL);
    }
    if(!(wholebodyinfo = psdCopyStrFmt("%s%s%s%s", bodyinfo, bindinginfo, lpwarn, deadwarn)))
    {
        psdFreeVec(bindingsstr);
        psdFreeVec(bindinginfo);
        psdFreeVec(bodyinfo);
        return(NULL);
    }
    if(configstate & PPF_HasBindingGUI)
    {
        gad[0] = "Set Device Prefs";
    }
    if(configstate & PPF_HasBindConfig)
    {
        gad[0] = "Change Device Prefs";
    }
    if(configstate & PPF_HasClassGUI)
    {
        gad[1] = "Set Class Prefs";
    }
    if(configstate & PPF_HasClsConfig)
    {
        gad[1] = "Change Class Prefs";
    }
    /* port actions only with a port: a root hub has none */
    if((isdead || lowpower) && pd->pd_Hub)
    {
        gad[0] = "Disable Port";
        methods[0] = MUIM_DevNotify_DisablePort;
        gad[1] = NULL;
    }
    if(isdead && (!lowpower) && pd->pd_Hub)
    {
        gad[1] = "Powercycle Port";
        methods[1] = MUIM_DevNotify_PowerCyclePort;
    }
    gad[2] = "No PopUps!";
    gad[3] = "Continue";
    if(!pog)
    {
        pog = pAllocNotifyGadgets(ps, wholebodyinfo, gad);
        if(pog)
        {
            pog->pog_Device = pd;
        }
    } else {
        get(pog->pog_BodyObj, MUIA_Text_Contents, &oldbody);
        if(strcmp(oldbody, wholebodyinfo))
        {
            set(pog->pog_BodyObj, MUIA_Text_Contents, wholebodyinfo);
            updatetime = TRUE;
        }
        for(cnt = 0; cnt < 4; cnt++)
        {
            if(gad[cnt])
            {
                set(pog->pog_GadgetObj[cnt], MUIA_Text_Contents, gad[cnt]);
                set(pog->pog_GadgetObj[cnt], MUIA_ShowMe, TRUE);
            } else {
                set(pog->pog_GadgetObj[cnt], MUIA_ShowMe, FALSE);
            }
        }
    }
    if(pog)
    {
        for(cnt = 0; cnt < 4; cnt++)
        {
            DoMethod(pog->pog_GadgetObj[cnt], MUIM_KillNotify, MUIA_Pressed);
            if(gad[cnt])
            {
                DoMethod(pog->pog_GadgetObj[cnt], MUIM_Notify, MUIA_Pressed,
                    FALSE, ntf->nt_AppObj, 5, MUIM_Application_PushMethod,
                    ntf->nt_NotifyObj, 2, methods[cnt], pog);
       	    }
        }
    }

    psdFreeVec(bindingsstr);
    psdFreeVec(bindinginfo);
    psdFreeVec(bodyinfo);
    psdFreeVec(wholebodyinfo);

    /* Everything after this depends on having gadgets */
    if(!pog)
        return NULL;

    /* update delay */
    if(updatetime && pog)
    {
        CurrentTime((ULONG *)&pog->pog_TimeoutTime.tv_secs, (ULONG *)&pog->pog_TimeoutTime.tv_micro);
        if(ps->ps_GlobalCfg->pgc_PopupCloseDelay)
        {
            pog->pog_TimeoutTime.tv_secs += ps->ps_GlobalCfg->pgc_PopupCloseDelay;
        } else {
            pog->pog_TimeoutTime.tv_secs += 60*60*24;
        }
    }

    if((ps->ps_GlobalCfg->pgc_PopupDeviceNew >= PGCP_ISNEW) && pd->pd_IsNewToMe)
    {
        pog->pog_ShowMe = TRUE;
    }
    if((ps->ps_GlobalCfg->pgc_PopupDeviceNew >= PGCP_ERROR) && (isdead || lowpower))
    {
        pog->pog_ShowMe = TRUE;
    }
    switch(ps->ps_GlobalCfg->pgc_PopupDeviceNew)
    {
        case PGCP_NEVER:
        case PGCP_ERROR:
        case PGCP_ISNEW:
            break;

        case PGCP_NOBINDING:
            if(configstate & PPF_HasBinding)
            {
                break;
            }
            // break missing on purpose
        case PGCP_HASBINDING:
            if(configstate & PPF_HasBinding)
            {
                pog->pog_ShowMe = TRUE;
            }
            break;

        case PGCP_ASKCONFIG:
            if(configstate & PPF_HasBindingGUI)
            {
                if(!(configstate & PPF_HasBindConfig))
                {
                    pog->pog_ShowMe = TRUE;
                }
                break; // don't ask, if there is an unset default class config
            }
            if(configstate & PPF_HasClassGUI)
            {
                if(!(configstate & PPF_HasClsConfig))
                {
                    pog->pog_ShowMe = TRUE;
                }
            }
            break;

        case PGCP_CANCONFIG:
            if(configstate & (PPF_HasClassGUI|PPF_HasBindingGUI))
            {
                pog->pog_ShowMe = TRUE;
            }
            break;

        case PGCP_ALWAYS:
            pog->pog_ShowMe = TRUE;
            break;

        default:
            break;
    }
    return(pog);
}
/* \\\ */

/* /// "pEventHandler()" */
void pEventHandler(struct PsdBase * ps)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    struct PsdDevice *pd;
    struct PsdEventNote *pen;
    struct PsdNotifyGadgets *pog;
    ULONG  eventmask;
    STRPTR body = NULL;
    BOOL   addsound = FALSE;
    BOOL   remsound = FALSE;
    BOOL   cfgchanged = FALSE;

    /* delete removed devices first */
    psdLockReadPBase();

    pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
    while(pog->pog_Node.ln_Succ)
    {
        if(pog->pog_Device)
        {
            /* verify that device is still there */
            if(!pIsDeviceStillValid(ps, pog->pog_Device))
            {
                /* huh, gone! */
                pFreeNotifyGadgets(ps, pog);
                pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
                continue;
            }
        }
        pog = (struct PsdNotifyGadgets *) pog->pog_Node.ln_Succ;
    }

    eventmask = 0;
    while((pen = (struct PsdEventNote *) GetMsg(ntf->nt_MsgPort)))
    {
        eventmask |= (1L<<pen->pen_Event);
        switch(pen->pen_Event)
        {
            case EHMB_ADDDEVICE:
            {
                pd = (struct PsdDevice *) pen->pen_Param1;
                if(pIsDeviceStillValid(ps, pd))
                {
                    pRemoveOldBox(ps, pd);
                    if(pd->pd_NotifyCfg.ntc_InhibitPopup)
                    {
                        break;
                    }
                    addsound = TRUE;
                    pog = pGenerateAddBox(ps, pd, NULL);
                    if(!pog)
                    {
                        break;
                    }
                    pog->pog_Device = pd;
                    if(pog->pog_ShowMe && (!pog->pog_WaitBinding))
                    {
                        ntf->nt_OpenRequest = TRUE;
                    }
                }
                break;
            }

            case EHMB_DEVRECOVERY:
            {
                /* One note per dead or overdrawn device, after the stack has
                   handled it (pAutoRecover). It carries what the popup shows:
                   the device may be gone already, so pd is used for identity,
                   and for the port buttons only while it is still there. */
                struct PsdDevRecovery *pdr = (struct PsdDevRecovery *) pen->pen_Param2;
                BOOL lowpower = (pdr->pdr_Flags & PDRF_LOWPOWER) ? TRUE : FALSE;
                BOOL isdead = (pdr->pdr_Flags & PDRF_DEAD) ? TRUE : FALSE;
                BOOL present;
                STRPTR body;
                STRPTR devname;
                STRPTR state;
                STRPTR done = "";
                STRPTR gads[4] = { NULL, NULL, NULL, "Ignore" };

                pd = (struct PsdDevice *) pen->pen_Param1;
                present = pIsDeviceStillValid(ps, pd);
                if(present && pd->pd_NotifyCfg.ntc_InhibitPopup)
                {
                    break;
                }

                remsound = TRUE;
                if((ps->ps_GlobalCfg->pgc_PopupDeviceNew < PGCP_ERROR) && (!ps->ps_GlobalCfg->pgc_PopupDeviceDeath))
                {
                    break;
                }

                pRemoveOldBox(ps, pd);
                devname = pdr->pdr_ProductName[0] ? (STRPTR) pdr->pdr_ProductName : (STRPTR) "(unnamed device)";
                if(lowpower && isdead)
                {
                    state = "\33bWARNING: Detected LOW POWER situation\33n\n\nfor USB device\n\n\33b%s\33n\n\nand it has \33bdropped dead already\33n!%s";
                }
                else if(lowpower)
                {
                    state = "\33bWARNING: Detected LOW POWER situation\33n\n\nfor USB device\n\n\33b%s\33n\n\nand this might cause failures!%s";
                } else {
                    state = "\33bWARNING: DEAD DEVICE!\33n\n\nUSB device\n\n\33b%s\33n\n\ndropped dead for no apparent reason!%s";
                }
                switch(pdr->pdr_Action)
                {
                    case EHRA_POWERCYCLE:
                        done = "\n\n\33bAutomatically powercycling port!\33n";
                        break;

                    case EHRA_DISABLE:
                        done = "\n\n\33bAutomatically disabling port!\33n";
                        break;

                    default:
                        /* nothing done: offer it by hand, if there is a port */
                        if(present && pd->pd_Hub)
                        {
                            gads[0] = "Disable Port";
                            if(isdead && !lowpower)
                            {
                                gads[1] = "Powercycle Port";
                            }
                        }
                        break;
                }
                body = psdCopyStrFmt(state, devname, done);
                pog = pAllocNotifyGadgets(ps, body, gads);
                psdFreeVec(body);
                if(!pog)
                {
                    break;
                }

                /* a recovered device's popup outlives the device */
                pog->pog_Device = (pdr->pdr_Action == EHRA_NONE) ? pd : NULL;
                if(gads[0])
                {
                    DoMethod(pog->pog_GadgetObj[0], MUIM_Notify, MUIA_Pressed, FALSE,
                             ntf->nt_AppObj, 5, MUIM_Application_PushMethod, ntf->nt_NotifyObj, 2, MUIM_DevNotify_DisablePort, pog);
                }
                if(gads[1])
                {
                    DoMethod(pog->pog_GadgetObj[1], MUIM_Notify, MUIA_Pressed, FALSE,
                             ntf->nt_AppObj, 5, MUIM_Application_PushMethod, ntf->nt_NotifyObj, 2, MUIM_DevNotify_PowerCyclePort, pog);
                }
                DoMethod(pog->pog_GadgetObj[3], MUIM_Notify, MUIA_Pressed, FALSE,
                         ntf->nt_AppObj, 5, MUIM_Application_PushMethod, ntf->nt_NotifyObj, 2, MUIM_DevNotify_RemInfo, pog);
                ntf->nt_OpenRequest = TRUE;
                break;
            }

            case EHMB_REMDEVICE:
            {
                pd = (struct PsdDevice *) pen->pen_Param1;
                pRemoveOldBox(ps, pd);
                if(pd->pd_NotifyCfg.ntc_InhibitPopup)
                {
                    break;
                }
                remsound = TRUE;
                if(!ps->ps_GlobalCfg->pgc_PopupDeviceGone)
                {
                    break;
                }
                if(pd->pd_ProductStr)
                {
                    body = psdCopyStrFmt("The USB device\n\n\33b%s\33n\n\nhas been removed!", pd->pd_ProductStr);
                    /* late free */
                    //psdFreeVec(pd->pd_ProductStr);
                    //pd->pd_ProductStr = NULL;
                } else {
                    body = psdCopyStr("An unnamed USB device has been removed.");
                }
                if(body)
                {
                    STRPTR gads[4] = { NULL, NULL, NULL, "OK" };
                    pog = pAllocNotifyGadgets(ps, body, gads);
                    psdFreeVec(body);
                    if(!pog)
                    {
                        break;
                    }
                    DoMethod(pog->pog_GadgetObj[3], MUIM_Notify, MUIA_Pressed, FALSE,
                             ntf->nt_AppObj, 5, MUIM_Application_PushMethod, ntf->nt_NotifyObj, 2, MUIM_DevNotify_RemInfo, pog);
                    ntf->nt_OpenRequest = TRUE;
                }
                break;
            }

            case EHMB_ADDBINDING:
            case EHMB_REMBINDING:
            {
                pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
                while(pog->pog_Node.ln_Succ)
                {
                    if(pog->pog_Device == (struct PsdDevice *) pen->pen_Param1)
                    {
                        if(!pIsDeviceStillValid(ps, pog->pog_Device))
                        {
                            break;
                        }
                        if(pog->pog_Device->pd_NotifyCfg.ntc_InhibitPopup)
                        {
                            break;
                        }
                        pGenerateAddBox(ps, pog->pog_Device, pog);
                        if(pog->pog_ShowMe)
                        {
                            ntf->nt_OpenRequest = TRUE;
                        }
                        break;
                    }
                    pog = (struct PsdNotifyGadgets *) pog->pog_Node.ln_Succ;
                }
                break;
            }

            case EHMB_CONFIGCHG:
                if(!cfgchanged)
                {
                    psdDelayMS(100);
                    cfgchanged = TRUE;
                }
                break;

        }
        ReplyMsg(&pen->pen_Msg);
    }
    psdUnlockPBase();
    if(addsound)
    {
        if(ntf->nt_InsertSndFile)
        {
            if(*ntf->nt_InsertSndFile)
            {
                pNotifyPlaySound(ps, ntf->nt_InsertSndFile);
            }
        }
    }
    if(remsound)
    {
        if(ntf->nt_RemoveSndFile)
        {
            if(*ntf->nt_RemoveSndFile)
            {
                pNotifyPlaySound(ps, ntf->nt_RemoveSndFile);
            }
        }
    }
    if(cfgchanged)
    {
        set(ntf->nt_SaveObj, MUIA_Disabled, (ps->ps_SavedConfigHash == ps->ps_ConfigHash));
    }
}
/* \\\ */

/* /// "pBindingsString()" */
STRPTR pBindingsString(struct PsdBase * ps, struct PsdDevice *pd)
{
    STRPTR oldstr = NULL;
    STRPTR newstr = NULL;
    struct PsdConfig *pc;
    struct PsdInterface *pif;

    if(pd->pd_DevBinding)
    {
        if(pd->pd_Flags & PDFF_APPBINDING)
        {
            return(psdCopyStr(((struct PsdAppBinding *) pd->pd_DevBinding)->pab_Task->tc_Node.ln_Name));
        } else {
            return(psdCopyStr(pd->pd_ClsBinding->puc_ClassBase->lib_Node.ln_Name));
        }
    } else {
        pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        while(pc->pc_Node.ln_Succ)
        {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            while(pif->pif_Node.ln_Succ)
            {
                if(pif->pif_IfBinding)
                {
                    if(oldstr)
                    {
                        newstr = psdCopyStrFmt("%s, %s", oldstr, pif->pif_ClsBinding->puc_ClassBase->lib_Node.ln_Name);
                        psdFreeVec(oldstr),
                        oldstr = newstr;
                    } else {
                        oldstr = psdCopyStr(pif->pif_ClsBinding->puc_ClassBase->lib_Node.ln_Name);
                    }
                }
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
        }
    }
    return(oldstr);
}
/* \\\ */

/* /// "pCheckConfigurable()" */
ULONG pCheckConfigurable(struct PsdBase * ps, struct PsdDevice *pd)
{
    ULONG  hasclassgui;
    ULONG  hasbindinggui;
    ULONG  noconfig;
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct Library *UsbClsBase;
    ULONG result = 0;

    psdLockReadPBase();
    if(pd->pd_DevBinding)
    {
        if(!(pd->pd_Flags & PDFF_APPBINDING))
        {
            UsbClsBase = pd->pd_ClsBinding->puc_ClassBase;
            noconfig = FALSE;
            hasclassgui = FALSE;
            hasbindinggui = FALSE;
            if(pIsClassStillValid(ps, UsbClsBase))
            {
                usbGetAttrs(UGA_CLASS, NULL,
                            UCCA_HasClassCfgGUI, &hasclassgui,
                            UCCA_HasBindingCfgGUI, &hasbindinggui,
                            UCCA_UsingDefaultCfg, &noconfig,
                            TAG_END);
                result |= PPF_HasBinding;
                if(hasclassgui)
                {
                    result |= PPF_HasClassGUI;
                    if(!noconfig)
                    {
                        result |= PPF_HasClsConfig;
                    }
                }
                if(hasbindinggui)
                {
                    result |= PPF_HasBindingGUI;
                    usbGetAttrs(UGA_BINDING, pd->pd_DevBinding,
                                UCBA_UsingDefaultCfg, &noconfig,
                                TAG_END);
                    if(!noconfig)
                    {
                        result |= PPF_HasBindConfig;
                    }
                }
            }
        }
    } else {
        pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        while(pc->pc_Node.ln_Succ)
        {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            while(pif->pif_Node.ln_Succ)
            {
                if(pif->pif_IfBinding)
                {
                    UsbClsBase = pif->pif_ClsBinding->puc_ClassBase;
                    noconfig = FALSE;
                    hasclassgui = FALSE;
                    hasbindinggui = FALSE;
                    if(pIsClassStillValid(ps, UsbClsBase))
                    {
                        usbGetAttrs(UGA_CLASS, NULL,
                                    UCCA_HasClassCfgGUI, &hasclassgui,
                                    UCCA_HasBindingCfgGUI, &hasbindinggui,
                                    UCCA_UsingDefaultCfg, &noconfig,
                                    TAG_END);
                        result |= PPF_HasBinding;
                        if(hasclassgui)
                        {
                            result |= PPF_HasClassGUI;
                            if(!noconfig)
                            {
                                result |= PPF_HasClsConfig;
                            }
                        }
                        if(hasbindinggui)
                        {
                            result |= PPF_HasBindingGUI;
                            usbGetAttrs(UGA_BINDING, pif->pif_IfBinding,
                                        UCBA_UsingDefaultCfg, &noconfig,
                                        TAG_END);
                            if(!noconfig)
                            {
                                result |= PPF_HasBindConfig;
                            }
                        }
                    }
                }
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
        }
    }
    psdUnlockPBase();
    return(result);
}
/* \\\ */

/* /// "pOpenBindingsConfigGUI()" */
void pOpenBindingsConfigGUI(struct PsdBase * ps, struct PsdDevice *pd)
{
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct Library *UsbClsBase;
    ULONG hascfggui;
    psdLockReadPBase();
    if(!pIsDeviceStillValid(ps, pd))
    {
        psdUnlockPBase();
        return;
    }
    if(pd->pd_DevBinding)
    {
        if(!(pd->pd_Flags & PDFF_APPBINDING))
        {
            UsbClsBase = pd->pd_ClsBinding->puc_ClassBase;
            if(pIsClassStillValid(ps, UsbClsBase))
            {
                usbGetAttrs(UGA_CLASS, NULL,
                            UCCA_HasBindingCfgGUI, &hascfggui,
                            TAG_END);
                if(hascfggui)
                {
                    usbDoMethod(UCM_OpenBindingCfgWindow, pd->pd_DevBinding);
                }
            }
        }
    } else {
        pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        while(pc->pc_Node.ln_Succ)
        {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            while(pif->pif_Node.ln_Succ)
            {
                if(pif->pif_IfBinding)
                {
                    UsbClsBase =  pif->pif_ClsBinding->puc_ClassBase;
                    if(pIsClassStillValid(ps, UsbClsBase))
                    {
                        usbGetAttrs(UGA_CLASS, NULL,
                                    UCCA_HasBindingCfgGUI, &hascfggui,
                                    TAG_END);
                        if(hascfggui)
                        {
                            usbDoMethod(UCM_OpenBindingCfgWindow, pif->pif_IfBinding);
                        }
                    }
                }
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
        }
    }
    psdUnlockPBase();
}
/* \\\ */

/* /// "pOpenClassesConfigGUI()" */
void pOpenClassesConfigGUI(struct PsdBase * ps, struct PsdDevice *pd)
{
    struct PsdConfig *pc;
    struct PsdInterface *pif;
    struct Library *UsbClsBase;
    ULONG hascfggui;

    psdLockReadPBase();
    if(!pIsDeviceStillValid(ps, pd))
    {
        psdUnlockPBase();
        return;
    }

    if(pd->pd_DevBinding)
    {
        if(!(pd->pd_Flags & PDFF_APPBINDING))
        {
            UsbClsBase = pd->pd_ClsBinding->puc_ClassBase;
            if(pIsClassStillValid(ps, UsbClsBase))
            {
                usbGetAttrs(UGA_CLASS, NULL,
                            UCCA_HasClassCfgGUI, &hascfggui,
                            TAG_END);
                if(hascfggui)
                {
                    usbDoMethod(UCM_OpenCfgWindow);
                }
            }
        }
    } else {
        pc = (struct PsdConfig *) pd->pd_Configs.lh_Head;
        while(pc->pc_Node.ln_Succ)
        {
            pif = (struct PsdInterface *) pc->pc_Interfaces.lh_Head;
            while(pif->pif_Node.ln_Succ)
            {
                if(pif->pif_IfBinding)
                {
                    UsbClsBase =  pif->pif_ClsBinding->puc_ClassBase;
                    if(pIsClassStillValid(ps, UsbClsBase))
                    {
                        usbGetAttrs(UGA_CLASS, NULL,
                                    UCCA_HasClassCfgGUI, &hascfggui,
                                    TAG_END);
                        if(hascfggui)
                        {
                            usbDoMethod(UCM_OpenCfgWindow);
                        }
                    }
                }
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ;
            }
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ;
        }
    }
    psdUnlockPBase();
}
/* \\\ */

/* /// "pDisableDevicePopup()" */
void pDisableDevicePopup(struct PsdBase * ps, struct PsdDevice *pd)
{
    if(!pIsDeviceStillValid(ps, pd))
    {
        return;
    }
    psdSetAttrs(PGA_DEVICE, pd, DA_InhibitPopup, TRUE, TAG_END);
}
/* \\\ */

/* /// "pAllocNotifyGadgets()" */
struct PsdNotifyGadgets * pAllocNotifyGadgets(struct PsdBase * ps, STRPTR body, STRPTR *gad)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    struct PsdNotifyGadgets *pog;
    if((pog = (struct PsdNotifyGadgets *) psdAllocVec(sizeof(struct PsdNotifyGadgets))))
    {
        pog->pog_GroupObj =
            VGroup,
                Child, (IPTR)(pog->pog_BodyObj = TextObject,
                    MUIA_Frame, MUIV_Frame_Text,
                    MUIA_Background, MUII_TextBack,
                    MUIA_Text_PreParse, (IPTR)"\33c",
                    MUIA_Text_Contents, (IPTR)body,
                    End),
                Child, (IPTR)(HGroup,
                    MUIA_Group_SameWidth, TRUE,
                    Child, (IPTR)(pog->pog_GadgetObj[0] = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_PreParse, (IPTR)"\33c",
                        MUIA_Text_Contents, (IPTR)(gad[0] ? gad[0] : (STRPTR) ""),
                        MUIA_ShowMe, (IPTR)gad[0],
                        End),
                    Child, (IPTR)(pog->pog_GadgetObj[1] = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_PreParse, (IPTR)"\33c",
                        MUIA_Text_Contents, (IPTR)(gad[1] ? gad[1] : (STRPTR) ""),
                        MUIA_ShowMe, (IPTR)gad[1],
                        End),
                    Child, (IPTR)(pog->pog_GadgetObj[2] = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_PreParse, (IPTR)"\33c",
                        MUIA_Text_Contents, (IPTR)(gad[2] ? gad[2] : (STRPTR) ""),
                        MUIA_ShowMe, (IPTR)gad[2],
                        End),
                    Child, (IPTR)(pog->pog_GadgetObj[3] = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_PreParse, (IPTR)"\33c",
                        MUIA_Text_Contents, (IPTR)(gad[3] ? gad[3] : (STRPTR) ""),
                        MUIA_ShowMe, (IPTR)gad[3],
                        End),
                    End),
                //Child, VSpace(0),
                Child, (IPTR)(BalanceObject,
                    End),
                //Child, VSpace(0),
                End;

        if(!pog->pog_GroupObj)
        {
            psdFreeVec(pog);
            return(NULL);
        }
        DoMethod(ntf->nt_GroupObj, MUIM_Group_InitChange);
        DoMethod(ntf->nt_GroupObj, OM_ADDMEMBER, pog->pog_GroupObj);
        DoMethod(ntf->nt_GroupObj, MUIM_Group_ExitChange);
        AddTail(&ntf->nt_GadgetList, &pog->pog_Node);
        CurrentTime((ULONG *)&pog->pog_TimeoutTime.tv_secs, (ULONG *)&pog->pog_TimeoutTime.tv_micro);
        if(ps->ps_GlobalCfg->pgc_PopupCloseDelay && (!ntf->nt_Sticky))
        {
            pog->pog_TimeoutTime.tv_secs += ps->ps_GlobalCfg->pgc_PopupCloseDelay;
        } else {
            pog->pog_TimeoutTime.tv_secs += 60*60*24;
        }
    }
    return(pog);
}
/* \\\ */

/* /// "pFreeNotifyGadgets()" */
void pFreeNotifyGadgets(struct PsdBase * ps, struct PsdNotifyGadgets *pog)
{
    struct PsdNotify *ntf = &ps->ps_Notify;
    Remove(&pog->pog_Node);
    DoMethod(ntf->nt_GroupObj, MUIM_Group_InitChange);
    DoMethod(ntf->nt_GroupObj, OM_REMMEMBER, pog->pog_GroupObj);
    DoMethod(ntf->nt_GroupObj, MUIM_Group_ExitChange);
    MUI_DisposeObject(pog->pog_GroupObj);
    psdFreeVec(pog);
}
/* \\\ */

/* /// "DevNotifyDispatcher()" */
IPTR DevNotifyDispatcher(struct IClass * cl asm("a0"), Object * obj asm("a2"), Msg msg asm("a1"))
{
    struct PsdNotifyGadgets *pog;
    struct PsdNotifyGadgets *tmppog;
    struct PsdBase * ps = (struct PsdBase *) cl->cl_UserData;
    struct PsdNotify *ntf = &ps->ps_Notify;
    ULONG sticky = 0;

    switch(msg->MethodID)
    {
        case OM_NEW:
            if(!(obj = (Object *)DoSuperMethodA(cl,obj,msg)))
                return(0);
            return((IPTR)obj);

        case MUIM_DevNotify_SavePrefs:
            psdSaveCfgToDisk(NULL);
            set(ntf->nt_SaveObj, MUIA_Disabled, (ps->ps_SavedConfigHash == ps->ps_ConfigHash));
            return(0);

        case MUIM_DevNotify_Sticky:
            get(ntf->nt_StickyObj, MUIA_Selected, &sticky);
            ntf->nt_Sticky = sticky;
            pog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
            while(pog->pog_Node.ln_Succ)
            {
                CurrentTime((ULONG *)&pog->pog_TimeoutTime.tv_secs, (ULONG *)&pog->pog_TimeoutTime.tv_micro);
                if(ps->ps_GlobalCfg->pgc_PopupCloseDelay && (!ntf->nt_Sticky))
                {
                    pog->pog_TimeoutTime.tv_secs += ps->ps_GlobalCfg->pgc_PopupCloseDelay;
                } else {
                    pog->pog_TimeoutTime.tv_secs += 60*60*24;
                }
                pog = (struct PsdNotifyGadgets *) pog->pog_Node.ln_Succ;
            }
            return(0);

        case MUIM_DevNotify_ConfigureBinding:
            pog = (struct PsdNotifyGadgets *) ((IPTR *) msg)[1];
            //KPRINTF(20, ("Config ntf=%08lx, pog=%08lx, ps=%08lx\n", ntf, pog, ps));
            pog->pog_TimeoutTime.tv_secs += 60*60*24;
            pOpenBindingsConfigGUI(ps, pog->pog_Device);
            return(0);

        case MUIM_DevNotify_ConfigureClass:
            pog = (struct PsdNotifyGadgets *) ((IPTR *) msg)[1];
            //KPRINTF(20, ("Config ntf=%08lx, pog=%08lx, ps=%08lx\n", ntf, pog, ps));
            pog->pog_TimeoutTime.tv_secs += 60*60*24;
            pOpenClassesConfigGUI(ps, pog->pog_Device);
            return(0);

        case MUIM_DevNotify_ShutUp:
            pog = (struct PsdNotifyGadgets *) ((IPTR *) msg)[1];
            pDisableDevicePopup(ps, pog->pog_Device);
            pRemoveOldBox(ps, pog->pog_Device);
            return(0);

        case MUIM_DevNotify_RemInfo:
            pog = (struct PsdNotifyGadgets *) ((IPTR *) msg)[1];
            //KPRINTF(20, ("RemInfo ntf=%08lx, pog=%08lx, ps=%08lx\n", ntf, pog, ps));
            tmppog = (struct PsdNotifyGadgets *) ntf->nt_GadgetList.lh_Head;
            while(tmppog->pog_Node.ln_Succ)
            {
                if(tmppog == pog)
                {
                    pFreeNotifyGadgets(ps, pog);
                    break;
                }
                tmppog = (struct PsdNotifyGadgets *) tmppog->pog_Node.ln_Succ;
            }
            return(0);

        case MUIM_DevNotify_DisablePort:
        case MUIM_DevNotify_PowerCyclePort:
        {
            struct PsdDevice *pd;
            struct Library *UsbClsBase;
            BOOL disable = (msg->MethodID == MUIM_DevNotify_DisablePort);

            pog = (struct PsdNotifyGadgets *) ((IPTR *) msg)[1];
            pd = pog->pog_Device;
            set(pog->pog_GadgetObj[0], MUIA_Disabled, TRUE);
            if(pog->pog_GadgetObj[1] && (!disable))
            {
                set(pog->pog_GadgetObj[1], MUIA_Disabled, TRUE);
            }
            Forbid();
            if(pIsDeviceStillValid(ps, pd))
            {
                struct PsdDevice *hubpd = pd->pd_Hub;
                if(hubpd && hubpd->pd_DevBinding) /* a root hub has no parent port */
                {
                    UsbClsBase = hubpd->pd_ClsBinding->puc_ClassBase;
                    if(pIsClassStillValid(ps, UsbClsBase))
                    {
                        usbDoMethod((ULONG) (disable ? UCM_HubDisablePort : UCM_HubPowerCyclePort), hubpd, pd->pd_HubPort);
                    }
                }
            }
            Permit();
            pog->pog_TimeoutTime.tv_secs += 60*60*24;

            return(0);
        }

        case MUIM_DevNotify_About:
            MUI_RequestA(ntf->nt_AppObj, ntf->nt_WindowObj, 0, NULL, "OK",
                         "Neptune USB device notifications\n\n" NEP_FORK_TXT "\n\nBased on PoPo by Chris Hodges.", NULL);
            return(0);

        case MUIM_DevNotify_OpenPrefs:
        {
            struct Library *DOSBase;
            if((DOSBase = OpenLibrary("dos.library", 39)))
            {
                BPTR fhandle;
                if((fhandle = Open("NIL:", MODE_READWRITE)))
                {
                    /* Same stack the NeptuneUSB icon asks for (dist/icons/NeptuneUSB.info.src). */
                    if(SystemTags("NeptuneUSB",
                                  NP_StackSize, 57344,
                                  SYS_Input, fhandle,
                                  SYS_Output, NULL,
                                  SYS_Asynch, TRUE,
                                  TAG_END))
                    {
                        Close(fhandle);
                        MUI_RequestA(ntf->nt_AppObj, ntf->nt_WindowObj, 0, NULL, "OK",
                                     "Could not start Neptune USB.", NULL);
                    }
                }
                CloseLibrary(DOSBase);
            }
            return(0);
        }
    }
    return(DoSuperMethodA(cl,obj,msg));
}
/* \\\ */
