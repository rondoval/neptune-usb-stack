/*****************************************************************************
** This is the Action custom class, a sub class of Area.mui.
******************************************************************************/

#include "debug.h"

#define USE_INLINE_STDARG
#include <proto/muimaster.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/neptune.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/neptune_class.h>
#include <proto/icon.h>
#include <proto/utility.h>

#include <hwmatch.h>
#include <classdir.h>

#include "NeptuneUSB.h"
#include "ActionClass.h"
#include "IconListClass.h"
#include "DevWinClass.h"
#include "CfgListClass.h"
#include "locale.h"

extern struct ExecBase *SysBase;
extern struct Library *ps;
extern struct IntuitionBase *IntuitionBase;
extern struct DosLibrary *DOSBase;

#define NewList(list) NEWLIST(list)

/* /// "Some strings" */
static STRPTR mainpanels[] =
{
    NULL, //"General",
    NULL,//"Controllers",
    NULL, //"Devices",
    NULL, //"Classes",
    NULL, //"Options",
    NULL, //"Popups",
    NULL, //"Config",
    NULL
};

static STRPTR errlvlstrings[] =
{
    NULL, //"All messages",
    NULL, //"Warnings and worse",
    NULL, //"Errors and failures",
    NULL, //"Failures only",
    NULL
};

static STRPTR popupnewdevicestrings[] =
{
    NULL, //"Never open a popup window",
    NULL, //"Only popup on an error condition",
    NULL, //"Popup on new, unknown devices",
    NULL, //"Popup, if there is no binding",
    NULL, //"Popup, if there is no config yet",
    NULL, //"Popup on configurable class",
    NULL, //"Popup, regardless of binding",
    NULL, //"Always open a popup window",
    NULL
};
/* \\\ */

static inline ULONG GetLONGBE(void *address)
{
    UBYTE *ptr = (UBYTE*)address;
    ULONG retval = 0;

    retval = (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];

    return retval;
}

/* /// "HardwareListDisplayHook()" */
LONG HardwareListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), struct HWListEntry * hlnode asm("a1"))
{

    static char buf[16];
    static char buf2[32];
    struct IconListData *data = (struct IconListData *) INST_DATA(IconListClass->mcc_Class, ((struct ActionData *) hook->h_Data)->hwlistobj);

    if(hlnode)
    {
        psdSafeRawDoFmt(buf, 16, "%ld", hlnode->unit);
        psdSafeRawDoFmt(buf2, 32, "\33O[%08lx] %s", hlnode->phw ? data->mimainlist[18] : data->mimainlist[5], hlnode->phw ? _(MSG_PANEL_HARDWARE_ONLINE_YES) : _(MSG_PANEL_HARDWARE_ONLINE_NO));
        /* Same strip the matching uses, so the column shows exactly the part
           that decides whether two rows are the same controller. */
        *strarr++ = (STRPTR) psdHwFilePart(hlnode->devname);
        *strarr++ = buf;
        *strarr++ = buf2;
        *strarr   = hlnode->prodname ? hlnode->prodname : (STRPTR) _(MSG_PANEL_HARDWARE_UNKNOWN);
    } else {
        *strarr++ = _(MSG_PANEL_HARDWARE_COLS_NAME);
        *strarr++ = _(MSG_PANEL_HARDWARE_COLS_UNIT);
        *strarr++ = _(MSG_PANEL_HARDWARE_COLS_ONLINE);
        *strarr   = _(MSG_PANEL_HARDWARE_COLS_PRODUCT);
    }
    return(0);
}
/* \\\ */

/* /// "PrefsListDisplayHook()" */
LONG PrefsListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), struct PrefsListEntry * plnode asm("a1"))
{

    static char buf[16];

    if(plnode)
    {
        psdSafeRawDoFmt(buf, 16, "%ld", plnode->size);
        *strarr++ = plnode->type;
        *strarr++ = plnode->id;
        *strarr++ = plnode->owner;
        *strarr   = buf;
    } else {
        *strarr++ = _(MSG_PANEL_CONFIG_COLS_TYPE);
        *strarr++ = _(MSG_PANEL_CONFIG_COLS_DESC);
        *strarr++ = _(MSG_PANEL_CONFIG_COLS_OWNER);
        *strarr = _(MSG_PANEL_CONFIG_COLS_SIZE);
    }
    return(0);
}
/* \\\ */

/* /// "CheckDeviceValid()" */
BOOL CheckDeviceValid(struct DevListEntry *dlnode)
{
    struct Node *pd = NULL;
    if(dlnode)
    {
        if(dlnode->pd)
        {
            while((pd = psdGetNextDevice(pd)))
            {
                if(pd == dlnode->pd)
                {
                    return(TRUE);
                }
            }
        }
        dlnode->pd = NULL;
    }
    return(FALSE);
}
/* \\\ */

/* /// "DeviceListDisplayHook()" */
LONG DeviceListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), struct DevListEntry * dlnode asm("a1"))
{

    ULONG clsimg;
    ULONG stateimg;
    IPTR devclass;
    IPTR devsubclass;
    IPTR devproto;
    IPTR devislowspeed;
    IPTR devishighspeed;
    IPTR devissuperspeed;
    IPTR devisconnected;
    IPTR devhasaddress;
    IPTR devhasdevdesc;
    IPTR devhasappbinding;
    IPTR devisconfigured;
    IPTR devlowpower = 0;
    IPTR devisdead = 0;
    IPTR devissuspended = 0;
    APTR  devbinding;
    BOOL  goodbinding = FALSE;
    BOOL  hasmultiple = FALSE;
    IPTR ifclass;
    IPTR ifsubclass;
    IPTR ifproto;
    struct List *pclist;
    struct List *piflist;
    struct Node *pc;
    struct Node *pif;
    APTR  ifbinding;
    struct Library *bindingcls;
    struct Task *bindingtask;
    STRPTR statestr;
    STRPTR tmpcptr;
    static char buf[100];
    static char buf2[32];
    static char buf3[32];
    STRPTR bufptr;
    ULONG buflen = 99;
    struct IconListData *data = (struct IconListData *) INST_DATA(IconListClass->mcc_Class, ((struct ActionData *) hook->h_Data)->devlistobj);

    if(dlnode)
    {
        if(!CheckDeviceValid(dlnode))
        {
            psdSafeRawDoFmt(buf2, 32, "\33O[%08lx] %s",
                            data->mimainlist[0],
                            "Spiritual");
            psdSafeRawDoFmt(buf3, 32, "\33O[%08lx] %s",
                            data->mimainlist[5], "Ghost");
            *strarr++ = "<gone>";
            *strarr++ = "Zero";
            *strarr++ = buf3;
            *strarr++ = buf2;
            *strarr = "None";
            return(0);
        }

        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_Binding, &devbinding,
                    DA_BindingClass, &bindingcls,
                    DA_ProductName, strarr++,
                    DA_IsLowspeed, &devislowspeed,
                    DA_IsHighspeed, &devishighspeed,
                    DA_IsSuperspeed, &devissuperspeed,
                    DA_IsConnected, &devisconnected,
                    DA_HasAddress, &devhasaddress,
                    DA_HasDevDesc, &devhasdevdesc,
                    DA_IsConfigured, &devisconfigured,
                    DA_IsDead, &devisdead,
                    DA_IsSuspended, &devissuspended,
                    DA_LowPower, &devlowpower,
                    DA_HasAppBinding, &devhasappbinding,
                    DA_Class, &devclass,
                    DA_SubClass, &devsubclass,
                    DA_Protocol, &devproto,
                    DA_ConfigList, &pclist,
                    TAG_END);

        *strarr++ = (devislowspeed ? _(MSG_DEVICE_SPEED_LOW) : (devissuperspeed ? _(MSG_DEVICE_SPEED_SUPER) : (devishighspeed ? _(MSG_DEVICE_SPEED_HIGH) : _(MSG_DEVICE_SPEED_FULL))));

        if(devissuspended)
        {
            statestr = _(MSG_DEVICE_STATE_SUSPENDED);
            stateimg = 19;
        }
        else if(devisdead)
        {
            statestr = devlowpower ? _(MSG_DEVICE_STATE_DEADLP) : _(MSG_DEVICE_STATE_DEAD);
            stateimg = 5;
        }
        else if(devlowpower)
        {
            statestr = _(MSG_DEVICE_STATE_LOWPOWER);
            stateimg = 19;
        }
        else if(devisconfigured)
        {
            statestr = _(MSG_DEVICE_STATE_CONFIGURED);
            stateimg = 18;
        }
        else if(devhasdevdesc)
        {
            statestr = _(MSG_DEVICE_STATE_DEVDESC);
            stateimg = 5;
        }
        else if(devhasaddress)
        {
            statestr = _(MSG_DEVICE_STATE_HASADDRESS);
            stateimg = 5;
        }
        else if(devisconnected)
        {
            statestr = _(MSG_DEVICE_STATE_CONNECTED);
            stateimg = 5;
        } else {
            statestr = _(MSG_DEVICE_STATE_DEAD);
            stateimg = 5;
        }
        *strarr++ = buf3;
        if(!devclass)
        {
            ifclass = 0;
            pc = pclist->lh_Head;
            while(pc->ln_Succ)
            {
                psdGetAttrs(PGA_CONFIG, pc,
                            CA_InterfaceList, &piflist,
                            TAG_END);
                pif = piflist->lh_Head;
                while(pif->ln_Succ)
                {
                    psdGetAttrs(PGA_INTERFACE, pif,
                                IFA_Class, &ifclass,
                                IFA_SubClass, &ifsubclass,
                                IFA_Protocol, &ifproto,
                                TAG_END);
                    if(ifclass)
                    {
                        if(!devclass)
                        {
                            devclass = ifclass;
                            devsubclass = ifsubclass;
                            devproto = ifproto;
                        } else {
                            if(devclass != ifclass)
                            {
                                devclass = 0;
                                hasmultiple = TRUE;
                                break;
                            } else {
                                if(devsubclass != ifsubclass)
                                {
                                    devsubclass = 0;
                                    devproto = 0;
                                } else {
                                    if(devproto != ifproto)
                                    {
                                        devproto = 0;
                                    }
                                }
                            }
                        }
                    }
                    pif = pif->ln_Succ;
                }
                pc = pc->ln_Succ;
            }
        }
        clsimg = 5;
        switch(devclass)
        {
            case STILLIMG_CLASSCODE:
                clsimg = 22;
                break;
            case BLUETOOTH_CLASSCODE:
                clsimg = 21;
                break;
            case FWUPGRADE_CLASSCODE:
                clsimg = 1;
                break;
            case VENDOR_CLASSCODE:
                clsimg++;
            case SECURITY_CLASSCODE:
                clsimg++;
            case SMARTCARD_CLASSCODE:
                clsimg++;
            case CDCDATA_CLASSCODE:
                clsimg++;
            case HUB_CLASSCODE:
                clsimg++;
            case MASSSTORE_CLASSCODE:
                clsimg++;
            case PRINTER_CLASSCODE:
                clsimg++;
            case PHYSICAL_CLASSCODE:
                clsimg++;
            case HID_CLASSCODE:
                clsimg += 2;
            case CDCCTRL_CLASSCODE:
                clsimg++;
            case AUDIO_CLASSCODE:
                clsimg++;
                break;

            default:
                clsimg = 0;
        }
        if(!hasmultiple)
        {
            psdSafeRawDoFmt(buf2, 32, "\33O[%08lx] %s",
                            data->mimainlist[clsimg],
                            psdNumToStr(NTS_COMBOCLASS,
                                        (devclass<<NTSCCS_CLASS)|(devsubclass<<NTSCCS_SUBCLASS)|(devproto<<NTSCCS_PROTO)|
                                        NTSCCF_CLASS|NTSCCF_SUBCLASS|NTSCCF_PROTO,
                                        "None"));
        } else {
            psdSafeRawDoFmt(buf2, 32, "\33O[%08lx] %s",
                            data->mimainlist[0], "Multiple");
        }
        *strarr++ = buf2;
        if(devbinding)
        {
            if(devhasappbinding)
            {
                psdGetAttrs(PGA_APPBINDING, devbinding,
                            ABA_Task, &bindingtask,
                            TAG_END);
                *strarr = bindingtask->tc_Node.ln_Name;
            } else {
                *strarr = bindingcls->lib_Node.ln_Name;
            }
            goodbinding = TRUE;
        } else {
            *strarr = bufptr = buf;
            strcpy(buf, "None");
            pc = pclist->lh_Head;
            while(pc->ln_Succ)
            {
                psdGetAttrs(PGA_CONFIG, pc,
                            CA_InterfaceList, &piflist,
                            TAG_END);
                pif = piflist->lh_Head;
                while(pif->ln_Succ)
                {
                    psdGetAttrs(PGA_INTERFACE, pif,
                                IFA_Binding, &ifbinding,
                                IFA_BindingClass, &bindingcls,
                                TAG_END);
                    if(ifbinding)
                    {
                        goodbinding = TRUE;
                        if((buflen < 99) && ( buflen > 3))
                        {
                            *bufptr++ = ',';
                            *bufptr++ = ' ';
                            buflen -= 2;
                        }
                        tmpcptr = bindingcls->lib_Node.ln_Name;
                        while((*bufptr++ = *tmpcptr++))
                        {
                            if(!(--buflen))
                            {
                                *bufptr = 0;
                                break;
                            }
                        }
                        bufptr--;
                    }
                    pif = pif->ln_Succ;
                    if(!buflen)
                    {
                        break;
                    }
                }
                pc = pc->ln_Succ;
                if(!buflen)
                {
                    break;
                }
            }
        }
        if(!goodbinding && (stateimg == 18))
        {
            stateimg = 20;
        }
        psdSafeRawDoFmt(buf3, 32, "\33O[%08lx] %s",
                        data->mimainlist[stateimg], statestr);
    } else {
        *strarr++ = _(MSG_PANEL_DEVICES_COLS_NAME);
        *strarr++ = _(MSG_PANEL_DEVICES_COLS_SPEED);
        *strarr++ = _(MSG_PANEL_DEVICES_COLS_STATE);
        *strarr++ = _(MSG_PANEL_DEVICES_COLS_CLASS);
        *strarr   = _(MSG_PANEL_DEVICES_COLS_BINDINGS);
    }
    return(0);
}
/* \\\ */

/* /// "ClassListDisplayHook()" */
LONG ClassListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), struct ClsListEntry * clnode asm("a1"))
{

    static char buf[16];
    IPTR usecnt;
    struct Library *UsbClsBase;
    struct List *lst;
    struct Node *puc;

    if(clnode)
    {
        psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &lst, TAG_END);
        puc = lst->lh_Head;
        while(puc->ln_Succ)
        {
            if(clnode->puc == puc)
            {
                break;
            }
            puc = puc->ln_Succ;
        }
        if(!puc->ln_Succ)
        {
            clnode->puc = NULL;
            *strarr++ = "";
            *strarr++ = "";
            *strarr   = "";
            return(0);
        }
        psdGetAttrs(PGA_USBCLASS, clnode->puc,
                    UCA_ClassBase, &UsbClsBase,
                    UCA_ClassName, strarr++,
                    UCA_UseCount, &usecnt,
                    TAG_END);
        psdSafeRawDoFmt(buf, 16, "%ld", usecnt);
        *strarr++ = buf;
        usbGetAttrs(UGA_CLASS, NULL,
                    UCCA_Description, strarr,
                    TAG_END);

    } else {
        *strarr++ = _(MSG_PANEL_CLASSES_COLS_NAME);
        *strarr++ = _(MSG_PANEL_CLASSES_COLS_USE);
        *strarr   = _(MSG_PANEL_CLASSES_COLS_DESC);
    }
    return(0);
}
/* \\\ */

/* /// "ErrorListDisplayHook()" */
LONG ErrorListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), struct ErrListEntry * elnode asm("a1"))
{

    IPTR level;
    struct DateStamp *ds;
    struct DateTime dt;
    static char strtime[LEN_DATSTRING];

    if(elnode)
    {
        ds = NULL;
        psdGetAttrs(PGA_ERRORMSG, elnode->pem,
                    EMA_Level, &level,
                    EMA_Origin, &strarr[2],
                    EMA_Msg, &strarr[3],
                    EMA_DateStamp, &ds,
                    TAG_END);

        if(ds)
        {
            dt.dat_Stamp.ds_Days = ds->ds_Days;
            dt.dat_Stamp.ds_Minute = ds->ds_Minute;
            dt.dat_Stamp.ds_Tick = ds->ds_Tick;
            dt.dat_Format = FORMAT_DEF;
            dt.dat_Flags = 0;
            dt.dat_StrDay = NULL;
            dt.dat_StrDate = NULL;
            dt.dat_StrTime = strtime;
            DateToStr(&dt);
            strarr[0] = strtime;
        } else {
            strarr[0] = "";
        }
        strarr[1] = ((level == RETURN_OK) ? _(MSG_LOGLEVEL_OK) :
                    ((level == RETURN_WARN) ? _(MSG_LOGLEVEL_WARNING) :
                    ((level == RETURN_ERROR) ? _(MSG_LOGLEVEL_ERROR) :
                    ((level == RETURN_FAIL) ? _(MSG_LOGLEVEL_FAILURE) : _(MSG_LOGLEVEL_OTHER)))));
    }
    return(0);
}
/* \\\ */

/* /// "IconListDisplayHook()" */
LONG IconListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), STRPTR str asm("a1"))
{

    static char buf[32];
    struct IconListData *data = (struct IconListData *) INST_DATA(IconListClass->mcc_Class, ((struct ActionData *) hook->h_Data)->cfgpagelv);

    if(str)
    {
        LONG pos = ((IPTR *) strarr)[-1];

        if(pos == 5)
        {
            pos = 24; // fix for the notifier
        }
        else if(pos == 6)
        {
            pos = 16; // fix for Configure Management menu
        }
        else if(pos == 7)
        {
            pos = 23; // fix for Online menu
        }

        psdSafeRawDoFmt(buf, 32, "\33O[%08lx] %s",
                        data->mimainlist[pos],
                        str);
        *strarr = buf;
    }
    return(0);
}
/* \\\ */

/* /// "AllocHWEntry()" */
struct HWListEntry * AllocHWEntry(struct ActionData *data, struct Node *phw)
{
    struct HWListEntry *hlnode;
    STRPTR str;
    hlnode = psdAllocVec(sizeof(struct HWListEntry));
    if(hlnode)
    {
        if(phw)
        {
            psdGetAttrs(PGA_HARDWARE, phw,
                        HA_DeviceName, &str,
                        HA_DeviceUnit, &hlnode->unit,
                        HA_ProductName, &hlnode->prodname,
                        TAG_END);
            hlnode->devname = psdCopyStr(str);
            hlnode->phw = phw;
        }
        AddTail(&data->hwlist, &hlnode->node);
    }
    return(hlnode);
}
/* \\\ */

/* /// "MatchHWCfgForm()" */
/* TRUE if this UHWDEVICE config form describes the given device and unit.
 *
 * psdMatchStringChunk() compares the stored name byte for byte, but the same
 * controller is spelled differently depending on who wrote the entry down: a
 * Kickstart-resident stack adds a bare "xhci.device" (romstartup/), while a
 * saved neptune.prefs keeps whichever was current when it was written.
 * The library's pFindHardware() compares through <hwmatch.h> too, so every
 * place that asks "is this the same controller?" agrees by construction -
 * where it did not, a live entry and its own saved config read as two
 * controllers, and the list grew a phantom offline twin of every real one.
 */
static BOOL MatchHWCfgForm(APTR subpic, CONST_STRPTR name, ULONG unit)
{
    STRPTR cfgname = psdGetStringChunk(subpic, IFFCHNK_NAME);
    BOOL   match   = psdHwNameMatch(cfgname, name);

    psdFreeVec(cfgname);
    if(match)
    {
        ULONG *unitchk = psdGetCfgChunk(subpic, IFFCHNK_UNIT);

        match = (unitchk && (unitchk[2] == unit));
        psdFreeVec(unitchk);
    }
    return(match);
}
/* \\\ */

/* /// "FindHWCfgForm()" */
/* The first UHWDEVICE form describing this device and unit, or NULL. */
static APTR FindHWCfgForm(APTR pic, CONST_STRPTR name, ULONG unit)
{
    APTR subpic = psdFindCfgForm(pic, IFFFORM_UHWDEVICE);

    while(subpic && (!MatchHWCfgForm(subpic, name, unit)))
    {
        subpic = psdNextCfgForm(subpic);
    }
    return(subpic);
}
/* \\\ */

/* /// "FreeHWEntry()" */
void FreeHWEntry(struct ActionData *data, struct HWListEntry *hlnode)
{
    Remove(&hlnode->node);
    if(hlnode->phw)
    {
        /* psdRemHardware() checks the pointer is still on the hardware list
           itself, so a row whose controller went away underneath us is a no-op. */
        psdRemHardware(hlnode->phw);
        hlnode->phw = NULL;
    }
    if(hlnode->infowindow)
    {
        set(hlnode->infowindow, MUIA_Window_Open, FALSE);
        if(data->appobj)
        {
            DoMethod(data->appobj, OM_REMMEMBER, hlnode->infowindow);
            DoMethod(hlnode->infowindow, OM_DISPOSE);
        }
        hlnode->infowindow = NULL;
    }
    psdFreeVec(hlnode->devname);
    psdFreeVec(hlnode);
}
/* \\\ */

/* /// "AllocDevEntry()" */
struct DevListEntry * AllocDevEntry(struct ActionData *data, struct Node *pd)
{
    struct DevListEntry *dlnode;

    dlnode = psdAllocVec(sizeof(struct DevListEntry));
    if(dlnode)
    {
        dlnode->pd = pd;
        dlnode->adata = data;
        AddTail(&data->devlist, &dlnode->node);
    }
    return(dlnode);
}
/* \\\ */

/* /// "FreeDevEntry()" */
void FreeDevEntry(struct ActionData *data, struct DevListEntry *dlnode)
{
    if(dlnode->infowindow)
    {
        set(dlnode->infowindow, MUIA_Window_Open, FALSE);
        if(data->appobj)
        {
            DoMethod(data->appobj, OM_REMMEMBER, dlnode->infowindow);
            DoMethod(dlnode->infowindow, OM_DISPOSE);
        }
        dlnode->infowindow = NULL;
        if(dlnode->devdata)
        {
            dlnode->devdata->dlnode = NULL;
        }
    }
    Remove(&dlnode->node);
    psdFreeVec(dlnode);
}
/* \\\ */

/* /// "AllocClsEntry()" */
struct ClsListEntry * AllocClsEntry(struct ActionData *data, struct Node *puc)
{
    struct ClsListEntry *clnode;
    clnode = psdAllocVec(sizeof(struct ClsListEntry));
    if(clnode)
    {
        clnode->puc = puc;

        AddTail(&data->clslist, &clnode->node);
    }
    return(clnode);
}
/* \\\ */

/* /// "FreeClsEntry()" */
void FreeClsEntry(struct ActionData *data, struct ClsListEntry *clnode)
{
    Remove(&clnode->node);
    psdFreeVec(clnode);
}
/* \\\ */

/* /// "FreeErrorList()" */
void FreeErrorList(struct ActionData *data)
{
    struct Node *node;
    node = data->errlist.lh_Head;
    while(node->ln_Succ)
    {
        Remove(node);
        psdFreeVec(node);
        node = data->errlist.lh_Head;
    }
}
/* \\\ */

/* /// "CreateErrorList()" */
void CreateErrorList(struct ActionData *data)
{
    struct Node *pem;
    struct List *lst;
    struct ErrListEntry *elnode;
    IPTR level;

    set(data->errlistobj, MUIA_List_Quiet, TRUE);
    DoMethod(data->errlistobj, MUIM_List_Clear);
    FreeErrorList(data);
    psdGetAttrs(PGA_STACK, NULL, PA_ErrorMsgList, &lst, TAG_END);
    Forbid();
    pem = lst->lh_Head;
    while(pem->ln_Succ)
    {
        psdGetAttrs(PGA_ERRORMSG, pem,
                    EMA_Level, &level,
                    TAG_END);
        if(level >= data->errorlevel)
        {
            if((elnode = psdAllocVec(sizeof(struct ErrListEntry))))
            {
                elnode->pem = pem;
                AddTail(&data->errlist, &elnode->node);
                DoMethod(data->errlistobj, MUIM_List_InsertSingle, elnode, MUIV_List_Insert_Bottom);
            }
        }
        pem = pem->ln_Succ;
    }
    Permit();
    set(data->errlistobj, MUIA_List_Quiet, FALSE);
    set(data->errlistobj, MUIA_List_Active, MUIV_List_Active_Bottom);
}
/* \\\ */

/* /// "FreePrefsList()" */
void FreePrefsList(struct ActionData *data)
{
    struct PrefsListEntry *plnode;
    plnode = (struct PrefsListEntry *) data->prefslist.lh_Head;
    while(plnode->node.ln_Succ)
    {
        Remove(&plnode->node);
        psdFreeVec(plnode->id);
        psdFreeVec(plnode->owner);
        psdFreeVec(plnode->devid);
        psdFreeVec(plnode->ifid);
        psdFreeVec(plnode);
        plnode = (struct PrefsListEntry *) data->prefslist.lh_Head;
    }
}
/* \\\ */

/* /// "AllocPrefsEntry()" */
struct PrefsListEntry * AllocPrefsEntry(struct ActionData *data, ULONG formid, ULONG size, STRPTR id, STRPTR owner)
{
    struct PrefsListEntry *plnode = NULL;

    if((plnode = psdAllocVec(sizeof(struct PrefsListEntry))))
    {
        plnode->id = id;
        /* Shorten long ids in the middle: a device id is
           product-VID-PID-serial-clone, and its tail is the part that tells
           two sticks of the same model apart. */
        ULONG idlen = strlen(id);
        if(idlen > 39)
        {
            id[18] = '.';
            id[19] = '.';
            id[20] = '.';
            memmove(&id[21], &id[idlen - 18], 18 + 1);
        }
        plnode->chunkid = formid;
        plnode->size = size;
        switch(formid)
        {
            case IFFFORM_STACKCFG:
                plnode->type = "Core Cfg";
                plnode->node.ln_Pri = 100;
                break;

            case IFFFORM_DEVICECFG:
                plnode->type = "USB Device";
                break;

            case IFFFORM_CLASSCFG:
                plnode->type = "Class Cfg";
                plnode->node.ln_Pri = 20;
                break;

            case IFFFORM_DEVCFGDATA:
                plnode->type = "  Device Cfg";
                break;

            case IFFFORM_IFCFGDATA:
                plnode->type = "  Interface Cfg";
                break;

            case MAKE_ID('P','S','D','L'):
                plnode->type = "Licence";
                plnode->node.ln_Pri = 50;
                break;

            case 0:
                break;

            default:
                plnode->type = "Unknown";
                plnode->node.ln_Pri = -100;
                break;
        }
        if(data->devidstr)
        {
            plnode->devid = psdCopyStr(data->devidstr);
            if(data->ifidstr)
            {
                plnode->ifid = psdCopyStr(data->ifidstr);
            }
        }
        plnode->owner = owner;
        Enqueue(&data->prefslist, &plnode->node);
    }
    return(plnode);
}
/* \\\ */

/* /// "FindCfgChunk()" */
ULONG * FindCfgChunk(ULONG *form, ULONG chnkid)
{
    ULONG *buf = form + 3;
    ULONG len = (AROS_LONG2BE(form[1]) - 3) & ~1UL;
    ULONG chlen;

    while(len)
    {
        if(AROS_LONG2BE(*buf) == chnkid)
        {
            return(buf);
        }
        chlen = (AROS_LONG2BE(buf[1]) + 9) & ~1UL;
        len -= chlen;
        buf = (ULONG *) (((UBYTE *) buf) + chlen);
    }
    return(NULL);
}
/* \\\ */

/* /// "GetStringChunk()" */
STRPTR GetStringChunk(ULONG *form, ULONG chnkid, STRPTR defstr)
{
    ULONG *chunkptr;
    STRPTR str = NULL;
    if((chunkptr = FindCfgChunk(form, chnkid)))
    {
        if((str = (STRPTR) psdAllocVec(AROS_LONG2BE(chunkptr[1]) + 1)))
        {
            memcpy(str, &chunkptr[2], (size_t) AROS_LONG2BE(chunkptr[1]));
            return(str);
        }
    }
    if(defstr)
    {
        str = psdCopyStr(defstr);
    }
    return(str);
}
/* \\\ */

/* /// "RecursePrefsForm()" */
void RecursePrefsForm(struct ActionData *data, ULONG *form, ULONG depth, STRPTR stack)
{
    struct PrefsListEntry *plnode;
    ULONG *endptr;
    ULONG *currptr;
    ULONG chunklen;
    ULONG chunkid;
    ULONG formid = AROS_LONG2BE(form[2]);
    STRPTR newstack;
    ULONG *chunkptr;
    STRPTR owner;
    STRPTR id;
    ULONG formsize = AROS_LONG2BE(form[1]) + 8;
    BOOL allocdevid = FALSE;
    BOOL allocifid = FALSE;

    newstack = (STRPTR) psdAllocVec((ULONG) strlen(stack) + 5);
    if(!newstack)
    {
        return;
    }
    strcpy(newstack, stack);
    *((ULONG *) &newstack[strlen(stack)]) = AROS_LONG2BE(formid);

    if(AROS_LONG2BE(*((ULONG *) newstack)) != IFFFORM_NEPCFG)
    {
        owner = GetStringChunk(form, IFFCHNK_OWNER, "Unknown");
        id = psdCopyStr("This is not a Neptune prefs file!");
        plnode = AllocPrefsEntry(data, formid, formsize, id, owner);
    }
    else if(!strcmp(newstack + 4, "STKC"))
    {
        plnode = AllocPrefsEntry(data, formid, formsize, psdCopyStr("Global Stack Configuration"), psdCopyStr(NEP_PREFS_OWNER));
    }
    else if(!strcmp(newstack + 4, "DEVC"))
    {
        id = GetStringChunk(form, IFFCHNK_DEVID, "Unknown");
        data->devidstr = psdCopyStr(id);
        allocdevid = TRUE;
        plnode = AllocPrefsEntry(data, formid, formsize, id, psdCopyStr(NEP_PREFS_OWNER));
    }
    else if(!strcmp(newstack + 4, "DEVCDCFG"))
    {
        owner = GetStringChunk(form, IFFCHNK_OWNER, "Unknown");
        if(!strcmp(owner, NEP_PREFS_OWNER))
        {
            id = psdCopyStr("Generic Prefs");
        } else {
            id = psdCopyStrFmt("%s Prefs", owner);
        }
        plnode = AllocPrefsEntry(data, formid, formsize, id, owner);
    }
    else if(!strcmp(newstack + 4, "DEVCICFG"))
    {
        if(formsize > 4)
        {
            data->ifidstr = GetStringChunk(form, IFFCHNK_IFID, "Unknown");
            allocifid = TRUE;
            owner = GetStringChunk(form, IFFCHNK_OWNER, "Unknown");
            plnode = AllocPrefsEntry(data, formid, formsize, psdCopyStrFmt("%s Prefs for %s", owner, data->ifidstr), owner);
        }
    }
    else if(!strcmp(newstack + 4, "CLSC"))
    {
        if(formsize > 4)
        {
            owner = GetStringChunk(form, IFFCHNK_OWNER, "Unknown");
            id = psdCopyStrFmt("(Default) Prefs for %s", owner);
            plnode = AllocPrefsEntry(data, formid, formsize, id, owner);
        }
    }
    else if(!strcmp(newstack + 4, "PSDL"))
    {
        owner = GetStringChunk(form, IFFCHNK_OWNER, "Unknown");
        chunkptr = FindCfgChunk(form, MAKE_ID('S','K','E','Y'));
        if(chunkptr)
        {
            id = psdCopyStrFmt("Keyfile Serial #%04ld", chunkptr[3] & 0xffff);
        } else {
            id = psdCopyStr("Corrupted Keyfile?");
        }
        plnode = AllocPrefsEntry(data, formid, formsize, id, psdCopyStr("Secret"));
    } else {
        if(depth == 1)
        {
            owner = GetStringChunk(form, IFFCHNK_OWNER, "Unknown");
            id = psdCopyStr("Unknown prefs data (XXXX)");
            if(id)
            {
                *((ULONG *) &id[20]) = AROS_LONG2BE(formid);
            }
            plnode = AllocPrefsEntry(data, formid, formsize, id, owner);
        }
    }
    depth++;
    currptr = &form[3];
    endptr = (ULONG *) (((UBYTE *) form) + ((GetLONGBE(&form[1]) + 9) & ~1UL));
    while(currptr < endptr)
    {
        chunkid = GetLONGBE(&currptr[0]);
        chunklen = (GetLONGBE(&currptr[1]) + 9) & ~1UL;
        if((chunkid == ID_FORM) && (depth < 3))
        {
            RecursePrefsForm(data, currptr, depth, newstack);
        } else {
            switch(chunkid)
            {
                case IFFCHNK_FORCEDBIND:
                    if(!strcmp(newstack + 4, "DEVC"))
                    {
                        owner = GetStringChunk(form, chunkid, "Unknown");
                        plnode = AllocPrefsEntry(data, chunkid, chunklen, psdCopyStrFmt("Forced Device Binding to %s", owner), owner);
                        if(plnode)
                        {
                            plnode->type = "  Binding";
                        }
                    }
                    else if(!strcmp(newstack + 4, "DEVCICFG"))
                    {
                        owner = GetStringChunk(form, IFFCHNK_OWNER, "Unknown");
                        plnode = AllocPrefsEntry(data, chunkid, chunklen, psdCopyStrFmt("Forced IFace Binding to %s", owner), owner);
                        if(plnode)
                        {
                            plnode->type = "    Binding";
                        }
                    }
                    break;
            }
        }
        currptr = (ULONG *) (((UBYTE *) currptr) + chunklen);
    }
    if(allocdevid)
    {
        psdFreeVec(data->devidstr);
        data->devidstr = NULL;
    }
    if(allocifid)
    {
        psdFreeVec(data->ifidstr);
        data->ifidstr = NULL;
    }
    psdFreeVec(newstack);
}
/* \\\ */

/* /// "CreatePrefsList()" */
void CreatePrefsList(struct ActionData *data)
{
    struct PrefsListEntry *plnode;
    IPTR oldpos = 0;
    IPTR oldhash = 0;
    IPTR currhash = 0;

    set(data->prefslistobj, MUIA_List_Quiet, TRUE);
    get(data->prefslistobj, MUIA_List_Active, &oldpos);
    DoMethod(data->prefslistobj, MUIM_List_Clear);
    FreePrefsList(data);
    data->configroot = psdWriteCfg(NULL);
    if(data->configroot)
    {
        RecursePrefsForm(data, data->configroot, 0, "");
        psdFreeVec(data->configroot);
        data->configroot = NULL;
    }
    plnode = (struct PrefsListEntry *) data->prefslist.lh_Head;
    while(plnode->node.ln_Succ)
    {
        DoMethod(data->prefslistobj, MUIM_List_InsertSingle, plnode, MUIV_List_Insert_Bottom);
        plnode = (struct PrefsListEntry *) plnode->node.ln_Succ;
    }
    set(data->prefslistobj, MUIA_List_Active, oldpos);
    set(data->prefslistobj, MUIA_List_Quiet, FALSE);
    psdGetAttrs(PGA_STACK, NULL,
                PA_CurrConfigHash, &currhash,
                PA_SavedConfigHash, &oldhash,
                TAG_END);
    set(data->saveobj, MUIA_Disabled, (oldhash == currhash));
}
/* \\\ */

/* /// "FreeGUILists()" */
void FreeGUILists(struct ActionData *data)
{
    struct HWListEntry  *hlnode;
    struct DevListEntry *dlnode;
    struct ClsListEntry *clnode;

    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    while(hlnode->node.ln_Succ)
    {
        //hlnode->infowindow = NULL;
        hlnode->phw = NULL;
        FreeHWEntry(data, hlnode);
        hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    }
    dlnode = (struct DevListEntry *) data->devlist.lh_Head;
    while(dlnode->node.ln_Succ)
    {
        //dlnode->infowindow = NULL;
        FreeDevEntry(data, dlnode);
        dlnode = (struct DevListEntry *) data->devlist.lh_Head;
    }
    clnode = (struct ClsListEntry *) data->clslist.lh_Head;
    while(clnode->node.ln_Succ)
    {
        FreeClsEntry(data, clnode);
        clnode = (struct ClsListEntry *) data->clslist.lh_Head;
    }
}
/* \\\ */

/* /// "CreateClassPopup()" */
Object * CreateClassPopup(void)
{
    Object *mi_root;
    Object *mi_top;
    Object *mi_sub;
    struct List *lst;
    struct Node *puc;
    STRPTR clname;

    mi_root = MenustripObject,
        Child, mi_top = MenuObjectT("Forced binding"),
            Child, MenuitemObject,
                MUIA_Menuitem_Title, "None",
                End,
            End,
        End;

    if(mi_root)
    {
        psdLockReadPBase();
        psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &lst, TAG_END);
        puc = lst->lh_Head;
        while(puc->ln_Succ)
        {
            psdGetAttrs(PGA_USBCLASS, puc,
                        UCA_ClassName, &clname,
                        TAG_END);

            mi_sub = MenuitemObject,
                MUIA_Menuitem_Title, clname,
                End;
            if(mi_sub)
            {
                DoMethod(mi_top, OM_ADDMEMBER, mi_sub);
            }
            puc = puc->ln_Succ;
        }
        psdUnlockPBase();
    }
    return(mi_root);
}
/* \\\ */

/* /// "InternalCreateConfig()" */
BOOL InternalCreateConfig(void)
{
    ULONG tmpform[3];
    APTR pic;
    APTR subpic;
    STRPTR name;
    IPTR unit;
    struct Node *phw;
    struct Node *puc;
    struct List *lst;

    pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    if(!pic)
    {
        tmpform[0] = AROS_LONG2BE(ID_FORM);
        tmpform[1] = AROS_LONG2BE(4);
        tmpform[2] = AROS_LONG2BE(IFFFORM_STACKCFG);
        psdAddCfgEntry(NULL, tmpform);
        pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    }
    if(!pic)
    {
        return(FALSE);
    }

    /* First mark all old hardware entries as offline */
    subpic = psdFindCfgForm(pic, IFFFORM_UHWDEVICE);
    while(subpic)
    {
        tmpform[0] = AROS_LONG2BE(IFFCHNK_OFFLINE);
        tmpform[1] = AROS_LONG2BE(4);
        tmpform[2] = TRUE;
        psdAddCfgEntry(subpic, tmpform);
        subpic = psdNextCfgForm(subpic);
    }

    /* Add existing hardware entries */
    psdLockReadPBase();
    psdGetAttrs(PGA_STACK, NULL, PA_HardwareList, &lst, TAG_END);
    phw = lst->lh_Head;
    while(phw->ln_Succ)
    {
        psdGetAttrs(PGA_HARDWARE, phw,
                    HA_DeviceName, &name,
                    HA_DeviceUnit, &unit,
                    TAG_END);

        // find corresponding form in config
        subpic = FindHWCfgForm(pic, name, unit);

        if(!subpic)
        {
            // not found, add it
            tmpform[0] = AROS_LONG2BE(ID_FORM);
            tmpform[1] = AROS_LONG2BE(4);
            tmpform[2] = AROS_LONG2BE(IFFFORM_UHWDEVICE);
            subpic = psdAddCfgEntry(pic, tmpform);
        }

        if(subpic)
        {
            psdAddStringChunk(subpic, IFFCHNK_NAME, name);
            tmpform[0] = AROS_LONG2BE(IFFCHNK_UNIT);
            tmpform[1] = AROS_LONG2BE(4);
            tmpform[2] = unit;
            psdAddCfgEntry(subpic, tmpform);
            // remove offline chunk to make sure the thing is going online next time
            psdRemCfgChunk(subpic, IFFCHNK_OFFLINE);
        }
        phw = phw->ln_Succ;
    }
    psdUnlockPBase();

    /* Add existing class entries */
    psdLockReadPBase();
    psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &lst, TAG_END);
    puc = lst->lh_Head;
    while(puc->ln_Succ)
    {
        tmpform[0] = AROS_LONG2BE(ID_FORM);
        tmpform[1] = AROS_LONG2BE(4);
        tmpform[2] = AROS_LONG2BE(IFFFORM_USBCLASS);
        subpic = psdAddCfgEntry(pic, tmpform);
        if(subpic)
        {
            name = NULL;
            psdGetAttrs(PGA_USBCLASS, puc,
                        UCA_FullPath, &name,
                        TAG_END);
            if(!name)
            {
                psdGetAttrs(PGA_USBCLASS, puc,
                            UCA_ClassName, &name,
                            TAG_END);
                name = psdCopyStrFmt("USB/%s", name);
                if(name)
                {
                    psdAddStringChunk(subpic, IFFCHNK_NAME, name);
                    psdFreeVec(name);
                }
            } else {
                psdAddStringChunk(subpic, IFFCHNK_NAME, name);
            }
        }
        puc = puc->ln_Succ;
    }
    psdUnlockPBase();
    return(TRUE);
}
/* \\\ */

/* /// "InternalCreateConfigGUI()" */
BOOL InternalCreateConfigGUI(struct ActionData *data)
{
    ULONG tmpform[3];
    APTR pic;
    APTR subpic;
    STRPTR name;
    IPTR unit;
    struct Node *phw;
    struct HWListEntry  *hlnode;
    struct ClsListEntry *clnode;
    struct List *lst;

    pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    if(!pic)
    {
        tmpform[0] = AROS_LONG2BE(ID_FORM);
        tmpform[1] = AROS_LONG2BE(4);
        tmpform[2] = AROS_LONG2BE(IFFFORM_STACKCFG);
        psdAddCfgEntry(NULL, tmpform);
        pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    }
    if(!pic)
    {
        return(FALSE);
    }

    psdLockReadPBase();


    /* First remove all old hardware entries */
    while((subpic = psdFindCfgForm(pic, IFFFORM_UHWDEVICE)))
    {
        psdRemCfgForm(subpic);
    }

    /* Add hardware entries from GUI, but mark them as offline */
    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    while(hlnode->node.ln_Succ)
    {
        tmpform[0] = AROS_LONG2BE(ID_FORM);
        tmpform[1] = AROS_LONG2BE(4);
        tmpform[2] = AROS_LONG2BE(IFFFORM_UHWDEVICE);
        subpic = psdAddCfgEntry(pic, tmpform);
        if(subpic)
        {
            psdAddStringChunk(subpic, IFFCHNK_NAME, hlnode->devname);
            tmpform[0] = AROS_LONG2BE(IFFCHNK_UNIT);
            tmpform[1] = AROS_LONG2BE(4);
            tmpform[2] = hlnode->unit;
            psdAddCfgEntry(subpic, tmpform);
            tmpform[0] = AROS_LONG2BE(IFFCHNK_OFFLINE);
            tmpform[1] = AROS_LONG2BE(4);
            tmpform[2] = TRUE;
            psdAddCfgEntry(subpic, tmpform);
        }
        hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
    }

    /* Now find all hardware entries that are online and mark them as such */
    psdGetAttrs(PGA_STACK, NULL, PA_HardwareList, &lst, TAG_END);
    phw = lst->lh_Head;
    while(phw->ln_Succ)
    {
        psdGetAttrs(PGA_HARDWARE, phw,
                    HA_DeviceName, &name,
                    HA_DeviceUnit, &unit,
                    TAG_END);

        // find corresponding form in config
        if((subpic = FindHWCfgForm(pic, name, unit)))
        {
            psdRemCfgChunk(subpic, IFFCHNK_OFFLINE);
        }
        phw = phw->ln_Succ;
    }

    /* Delete all old class entries */
    while((subpic = psdFindCfgForm(pic, IFFFORM_USBCLASS)))
    {
        psdRemCfgForm(subpic);
    }
    /* Add existing class entries */
    clnode = (struct ClsListEntry *) data->clslist.lh_Head;
    while(clnode->node.ln_Succ)
    {
        tmpform[0] = AROS_LONG2BE(ID_FORM);
        tmpform[1] = AROS_LONG2BE(4);
        tmpform[2] = AROS_LONG2BE(IFFFORM_USBCLASS);
        subpic = psdAddCfgEntry(pic, tmpform);
        if(subpic)
        {
            name = NULL;
            psdGetAttrs(PGA_USBCLASS, clnode->puc,
                        UCA_FullPath, &name,
                        TAG_END);
            if(!name)
            {
                psdGetAttrs(PGA_USBCLASS, clnode->puc,
                            UCA_ClassName, &name,
                            TAG_END);
                name = psdCopyStrFmt("USB/%s", name);
                if(name)
                {
                    psdAddStringChunk(subpic, IFFCHNK_NAME, name);
                    psdFreeVec(name);
                }
            } else {
                psdAddStringChunk(subpic, IFFCHNK_NAME, name);
            }
        }
        clnode = (struct ClsListEntry *) clnode->node.ln_Succ;
    }
    psdUnlockPBase();
    data->swallowconfigevent = TRUE;
    return(TRUE);
}
/* \\\ */

/* /// "CleanupEventHandler()" */
void CleanupEventHandler(struct ActionData *data)
{
    if(data->eventhandler)
    {
        psdRemEventHandler(data->eventhandler);
        data->eventhandler = NULL;
    }
    if(data->eventmsgport)
    {
        DeleteMsgPort(data->eventmsgport);
        data->eventmsgport = NULL;
    }
}
/* \\\ */

/* /// "SetupEventHandler()" */
BOOL SetupEventHandler(struct ActionData *data)
{
    if((data->eventmsgport = CreateMsgPort()))
    {
        data->eventhandler = psdAddEventHandler(data->eventmsgport, ~0);
        if(data->eventhandler)
        {
            return(TRUE);
        }
    } else {
        data->eventhandler = NULL;
    }
    return(FALSE);
}
/* \\\ */

/* /// "EventHandler()" */
void EventHandler(struct ActionData *data)
{
    APTR pen;
    ULONG eventmask;
    IPTR penid;
    IPTR penparam1;
    IPTR penparam2;
    BOOL cfgchanged = FALSE;
    /* Balance MUIA_List_Quiet across the whole drain: one TRUE, one FALSE.
       One hub unplug delivers a removal per child plus one for the hub, and
       the autodoc does not say whether repeated TRUEs nest - if the List class
       counts them (Zune uses a plain flag, MUI is unspecified), setting it per
       message while clearing it once per batch leaves the list quiet, and
       never repainting, for good. */
    BOOL devquiet = FALSE;
    BOOL clsquiet = FALSE;

    eventmask = 0;
    while((pen = GetMsg(data->eventmsgport)))
    {
        psdGetAttrs(PGA_EVENTNOTE, pen,
                    ENA_EventID, &penid,
                    ENA_Param1, &penparam1,
                    ENA_Param2, &penparam2,
                    TAG_END);
        eventmask |= (1L<<penid);
        /*printf("Event %ld, Param1 %ld\n", pen->pen_Event, pen->pen_Param1);*/
        switch(penid)
        {
            case EHMB_ADDHARDWARE:
            {
                struct Node *phw;
                struct List *lst;
                struct HWListEntry *hlnode;
                STRPTR devname;
                IPTR unit;
                STRPTR prodname;

                psdLockReadPBase();
                psdGetAttrs(PGA_STACK, NULL, PA_HardwareList, &lst, TAG_END);
                phw = lst->lh_Head;
                while(phw->ln_Succ)
                {
                    if(phw == (struct Node *) penparam1)
                    {
                        break;
                    }
                    phw = phw->ln_Succ;
                }
                if(phw->ln_Succ)
                {
                    psdGetAttrs(PGA_HARDWARE, phw,
                                HA_DeviceName, &devname,
                                HA_DeviceUnit, &unit,
                                HA_ProductName, &prodname,
                                TAG_END);

                    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
                    while(hlnode->node.ln_Succ)
                    {
                        if(psdHwMatch(devname, unit, hlnode->devname, hlnode->unit))
                        {
                            if(!hlnode->phw)
                            {
                                hlnode->phw = phw;
                                hlnode->prodname = prodname;
                            }
                            break;
                        }
                        hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
                    }
                    if(!hlnode->node.ln_Succ)
                    {
                        if((hlnode = AllocHWEntry(data, phw)))
                        {
                            DoMethod(data->hwlistobj, MUIM_List_InsertSingle, hlnode, MUIV_List_Insert_Bottom);
                        }
                    } else {
                        DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
                    }
                }
                psdUnlockPBase();
                break;
            }

            case EHMB_REMHARDWARE:
            {
                struct HWListEntry *hlnode;
                psdLockReadPBase();
                hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
                while(hlnode->node.ln_Succ)
                {
                    if(hlnode->phw == (struct Node *) penparam1)
                    {
                        hlnode->phw = NULL;
                        hlnode->prodname = NULL;
                        break;
                    }
                    hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
                }
                if(hlnode->node.ln_Succ)
                {
                    DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
                }
                psdUnlockPBase();
                break;
            }

            case EHMB_ADDDEVICE:
            {
                struct Node *pd = NULL;
                struct DevListEntry *dlnode;
                psdLockReadPBase();
                while((pd = psdGetNextDevice(pd)))
                {
                    if(pd == (struct Node *) penparam1)
                    {
                        dlnode = (struct DevListEntry *) data->devlist.lh_Head;
                        while(dlnode->node.ln_Succ)
                        {
                            if(dlnode->pd == pd)
                            {
                                // ignore existing entry!
                                break;
                            }
                            dlnode = (struct DevListEntry *) dlnode->node.ln_Succ;
                        }
                        if(!dlnode->node.ln_Succ)
                        {
                            dlnode = AllocDevEntry(data, pd);
                            if(dlnode)
                            {
                                DoMethod(data->devlistobj, MUIM_List_InsertSingle, dlnode, MUIV_List_Insert_Bottom);
                            }
                        }
                        break;
                    }
                }
                psdUnlockPBase();
                break;
            }

            case EHMB_REMDEVICE:
            {
                struct DevListEntry *dlnode;
                struct DevListEntry *tmpnode;
                ULONG pos = 0;
                if(!devquiet)
                {
                    set(data->devlistobj, MUIA_List_Quiet, TRUE);
                    devquiet = TRUE;
                }
                dlnode = (struct DevListEntry *) data->devlist.lh_Head;
                while(dlnode->node.ln_Succ)
                {
                    if((dlnode->pd == (struct Node *) penparam1) || (!dlnode->pd))
                    {
                        dlnode->pd = NULL;
                        do
                        {
                            DoMethod(data->devlistobj, MUIM_List_GetEntry, pos, &tmpnode);
                            if(!tmpnode)
                                break;
                            if(tmpnode == dlnode)
                            {
                                DoMethod(data->devlistobj, MUIM_List_Remove, pos);
                                break;
                            }
                            pos++;
                        } while(TRUE);
                        /* Keep scanning, as the class list does below: CheckDeviceValid()
                           NULLs dlnode->pd on every repaint of a row whose device is gone,
                           so stopping at the first hit lets such a ghost swallow the
                           removal of the device this event is actually about, leaving the
                           real row behind for good. */
                        tmpnode = (struct DevListEntry *) dlnode->node.ln_Succ;
                        FreeDevEntry(data, dlnode);
                        dlnode = tmpnode;
                    } else {
                        dlnode = (struct DevListEntry *) dlnode->node.ln_Succ;
                    }
                }
                break;
            }

            case EHMB_ADDCLASS:
            {
                struct ClsListEntry *clnode;
                clnode = (struct ClsListEntry *) data->clslist.lh_Head;
                while(clnode->node.ln_Succ)
                {
                    if(clnode->puc == (struct Node *) penparam1)
                    {
                        break;
                    }
                    clnode = (struct ClsListEntry *) clnode->node.ln_Succ;
                }
                if(!clnode->node.ln_Succ)
                {
                    if((clnode = AllocClsEntry(data, (struct Node *) penparam1)))
                    {
                        DoMethod(data->clslistobj, MUIM_List_InsertSingle, clnode, MUIV_List_Insert_Bottom);
                    }
                }
                break;
            }

            case EHMB_REMCLASS:
            {
                struct ClsListEntry *clnode;
                struct ClsListEntry *tmpnode;
                ULONG pos = 0;
                if(!clsquiet)
                {
                    set(data->clslistobj, MUIA_List_Quiet, TRUE);
                    clsquiet = TRUE;
                }
                clnode = (struct ClsListEntry *) data->clslist.lh_Head;
                while(clnode->node.ln_Succ)
                {
                    if((clnode->puc == (struct Node *) penparam1) || (!clnode->puc))
                    {
                        clnode->puc = NULL;
                        do
                        {
                            DoMethod(data->clslistobj, MUIM_List_GetEntry, pos, &tmpnode);
                            if(!tmpnode)
                                break;
                            if(tmpnode == clnode)
                            {
                                DoMethod(data->clslistobj, MUIM_List_Remove, pos);
                                break;
                            }
                            pos++;
                        } while(TRUE);
                        tmpnode = (struct ClsListEntry *) clnode->node.ln_Succ;
                        FreeClsEntry(data, clnode);
                        clnode = tmpnode;
                    } else {
                        clnode = (struct ClsListEntry *) clnode->node.ln_Succ;
                    }
                }
                break;
            }

            case EHMB_ADDERRORMSG:
            {
                struct ErrListEntry *elnode;
                IPTR level;

                psdGetAttrs(PGA_ERRORMSG, (APTR) penparam1,
                            EMA_Level, &level,
                            TAG_END);
                if(level >= data->errorlevel)
                {
                    if((elnode = psdAllocVec(sizeof(struct ErrListEntry))))
                    {
                        elnode->pem = (struct Node *) penparam1;
                        AddTail(&data->errlist, &elnode->node);
                        DoMethod(data->errlistobj, MUIM_List_InsertSingle, elnode, MUIV_List_Insert_Bottom);
                    }
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
        ReplyMsg(pen);
    }
    if(eventmask & EHMF_REMERRORMSG)
    {
        CreateErrorList(data);
    }
    if(eventmask & EHMF_ADDERRORMSG)
    {
        set(data->errlistobj, MUIA_List_Active, MUIV_List_Active_Bottom);
    }
    if(eventmask & (EHMF_ADDBINDING|EHMF_REMBINDING|EHMF_DEVICEDEAD|EHMF_DEVICELOWPW|EHMF_DEVSUSPENDED|EHMF_DEVRESUMED))
    {
        DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
        DoMethod(data->clslistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
        DoMethod(data->selfobj, MUIM_Action_Dev_Activate);
    }
    if(devquiet)
    {
        set(data->devlistobj, MUIA_List_Quiet, FALSE);
    }
    if(clsquiet)
    {
        set(data->clslistobj, MUIA_List_Quiet, FALSE);
    }
    if(eventmask & (EHMF_ADDCLASS|EHMF_REMCLASS))
    {
        set(data->devlistobj, MUIA_ContextMenu, NULL);
        MUI_DisposeObject(data->mi_classpopup);
        data->mi_classpopup = CreateClassPopup();
        set(data->devlistobj, MUIA_ContextMenu, data->mi_classpopup);
    }
    if(cfgchanged)
    {
        if(!data->swallowconfigevent)
        {
            UpdateConfigToGUI(data);
        } else {
            IPTR oldhash = 0;
            IPTR currhash = 0;
            psdGetAttrs(PGA_STACK, NULL,
                        PA_CurrConfigHash, &currhash,
                        PA_SavedConfigHash, &oldhash,
                        TAG_END);
            set(data->saveobj, MUIA_Disabled, (oldhash == currhash));
        }
        data->swallowconfigevent = FALSE;
    }
}
/* \\\ */

/* /// "UpdateConfigToGUI()" */
void UpdateConfigToGUI(struct ActionData *data)
{
    struct Node *phw;
    struct List *lst;
    struct HWListEntry *hlnode;
    struct Node *puc;
    struct ClsListEntry *clnode;
    APTR stackcfg;
    IPTR bootdelay;
    IPTR subtaskpri;
    IPTR loginfo;
    IPTR logwarn;
    IPTR logerr;
    IPTR logfail;
    IPTR popupnew = 0;
    IPTR popupgone = FALSE;
    IPTR popupdeath = FALSE;
    IPTR popupdelay = 0;
    IPTR popupactivate = FALSE;
    IPTR popuptofront = TRUE;
    IPTR autodisablelp = FALSE;
    IPTR autodisabledead = TRUE;
    IPTR autorestartdead = TRUE;
    IPTR powersaving = FALSE;
    IPTR forcesuspend = FALSE;
    IPTR suspendtimeout = 30;
    IPTR linkpower = TRUE;
    STRPTR devdtxsoundfile = "";
    STRPTR devremsoundfile = "";
    IPTR prefsversion = 0;
    IPTR relversion = 0;

    ULONG numclasses = 0;
    APTR pic;
    APTR subpic;
    APTR oldphw;
    ULONG curpos = 0;
    IPTR selpos = MUIV_List_Active_Off;
    IPTR clsselpos = MUIV_List_Active_Off;

    get(data->hwlistobj, MUIA_List_Active, &selpos);
    get(data->clslistobj, MUIA_List_Active, &clsselpos);

    set(data->hwlistobj, MUIA_List_Quiet, TRUE);
    DoMethod(data->hwlistobj, MUIM_List_Clear);
    set(data->clslistobj, MUIA_List_Quiet, TRUE);
    DoMethod(data->clslistobj, MUIM_List_Clear);

    oldphw = data->acthlnode ? data->acthlnode->phw : NULL;

    // remove all GUI entries first
    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    while(hlnode->node.ln_Succ)
    {
        hlnode->phw = NULL;
        FreeHWEntry(data, hlnode);
        hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    }

    clnode = (struct ClsListEntry *) data->clslist.lh_Head;
    while(clnode->node.ln_Succ)
    {
        FreeClsEntry(data, clnode);
        clnode = (struct ClsListEntry *) data->clslist.lh_Head;
    }

    /* get stuff that's online first */
    psdGetAttrs(PGA_STACK, NULL,
                PA_HardwareList, &lst,
                PA_ReleaseVersion, &relversion,
                PA_GlobalConfig, &stackcfg,
                TAG_END);
    phw = lst->lh_Head;
    while(phw->ln_Succ)
    {
        if((hlnode = AllocHWEntry(data, phw)))
        {
            if(phw == oldphw)
            {
                selpos = curpos;
            }
            DoMethod(data->hwlistobj, MUIM_List_InsertSingle, hlnode, MUIV_List_Insert_Bottom);
            curpos++;
        }
        phw = phw->ln_Succ;
    }

    psdLockReadPBase();
    pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);
    /* now check for additional entries that are offline */
    subpic = pic ? psdFindCfgForm(pic, IFFFORM_UHWDEVICE) : NULL;
    while(subpic)
    {
        hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
        while(hlnode->node.ln_Succ)
        {
            if(MatchHWCfgForm(subpic, hlnode->devname, hlnode->unit))
            {
                break;
            }

            hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
        }
        if(!hlnode->node.ln_Succ)
        {
            if((hlnode = AllocHWEntry(data, NULL)))
            {
                ULONG *unitchk = psdGetCfgChunk(subpic, IFFCHNK_UNIT);
                if(unitchk)
                {
                    hlnode->unit = unitchk[2];
                    psdFreeVec(unitchk);
                }
                /* Older prefs may hold a path; entries keep the bare name. */
                STRPTR cfgname = psdGetStringChunk(subpic, IFFCHNK_NAME);
                hlnode->devname = cfgname ? psdCopyStr((STRPTR) psdHwFilePart(cfgname)) : NULL;
                psdFreeVec(cfgname);
                DoMethod(data->hwlistobj, MUIM_List_InsertSingle, hlnode, MUIV_List_Insert_Bottom);
                curpos++;
            }
        }
        subpic = psdNextCfgForm(subpic);
    }

    psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &lst, TAG_END);
    puc = lst->lh_Head;
    while(puc->ln_Succ)
    {
        if((clnode = AllocClsEntry(data, puc)))
        {
            numclasses++;
            DoMethod(data->clslistobj, MUIM_List_InsertSingle, clnode, MUIV_List_Insert_Bottom);
        }
        puc = puc->ln_Succ;
    }

    if(stackcfg)
    {
        psdGetAttrs(PGA_STACKCFG, stackcfg, GCA_PrefsVersion, &prefsversion, TAG_END);
    }
    psdUnlockPBase();

    if(relversion > prefsversion)
    {
        psdAddErrorMsg(RETURN_WARN, "NeptuneUSB", "Forcing a DirScan for (new) classes, as you probably updated to a newer version.");
        psdSetAttrs(PGA_STACKCFG, stackcfg, GCA_PrefsVersion, relversion, TAG_END);
        DoMethod(data->selfobj, MUIM_Action_Cls_Scan);
    }
    else if(!(numclasses || data->autoclassesadded))
    {
        psdAddErrorMsg(RETURN_WARN, "NeptuneUSB", "Doing a DirScan for classes, because none were in config.");
        DoMethod(data->selfobj, MUIM_Action_Cls_Scan);
    }
    data->autoclassesadded = TRUE;

    set(data->hwlistobj, MUIA_List_Active, selpos);
    set(data->clslistobj, MUIA_List_Active, clsselpos);
    /* only the hardware and class lists were silenced above - clearing the
       device list here too is an unpaired FALSE (see EventHandler) */
    set(data->hwlistobj, MUIA_List_Quiet, FALSE);
    set(data->clslistobj, MUIA_List_Quiet, FALSE);
    CreatePrefsList(data);

    if(stackcfg)
    {
        psdGetAttrs(PGA_STACKCFG, stackcfg,
                    GCA_SubTaskPri, &subtaskpri,
                    GCA_BootDelay, &bootdelay,
                    GCA_LogInfo, &loginfo,
                    GCA_LogWarning, &logwarn,
                    GCA_LogError, &logerr,
                    GCA_LogFailure, &logfail,
                    GCA_PopupDeviceNew, &popupnew,
                    GCA_PopupDeviceGone, &popupgone,
                    GCA_PopupDeviceDeath, &popupdeath,
                    GCA_PopupCloseDelay, &popupdelay,
                    GCA_PopupActivateWin, &popupactivate,
                    GCA_PopupWinToFront, &popuptofront,
                    GCA_InsertionSound, &devdtxsoundfile,
                    GCA_RemovalSound, &devremsoundfile,
                    GCA_AutoDisableLP, &autodisablelp,
                    GCA_AutoDisableDead, &autodisabledead,
                    GCA_AutoRestartDead, &autorestartdead,
                    GCA_PowerSaving, &powersaving,
                    GCA_ForceSuspend, &forcesuspend,
                    GCA_SuspendTimeout, &suspendtimeout,
                    GCA_LinkPowerMgmt, &linkpower,
                    TAG_END);
        nnset(data->cfgtaskpriobj, MUIA_Numeric_Value, subtaskpri);
        nnset(data->cfgbootdelayobj, MUIA_Numeric_Value, bootdelay);
        nnset(data->cfgloginfoobj, MUIA_Selected, loginfo);
        nnset(data->cfglogwarnobj, MUIA_Selected, logwarn);
        nnset(data->cfglogerrobj, MUIA_Selected, logerr);
        nnset(data->cfglogfailobj, MUIA_Selected, logfail);
        nnset(data->cfgpopupnewobj, MUIA_Cycle_Active, popupnew);
        nnset(data->cfgpopupgoneobj, MUIA_Selected, popupgone);
        nnset(data->cfgpopupdeathobj, MUIA_Selected, popupdeath);
        nnset(data->cfgpopupdelayobj, MUIA_Numeric_Value, popupdelay);
        nnset(data->cfgpopupactivateobj, MUIA_Selected, popupactivate);
        nnset(data->cfgpopuptofrontobj, MUIA_Selected, popuptofront);
        nnset(data->cfgdevdtxsoundobj, MUIA_String_Contents, devdtxsoundfile);
        nnset(data->cfgdevremsoundobj, MUIA_String_Contents, devremsoundfile);
        nnset(data->cfgautolpobj, MUIA_Selected, autodisablelp);
        nnset(data->cfgautodeadobj, MUIA_Selected, autodisabledead);
        nnset(data->cfgautopcobj, MUIA_Selected, autorestartdead);
        /* same interlock Action_Cfg_Changed() applies: auto-restart owns the
           dead device, so auto-disable cannot also act on it */
        nnset(data->cfgautodeadobj, MUIA_Disabled, autorestartdead);
        nnset(data->cfgpowersavingobj, MUIA_Selected, powersaving);
        nnset(data->cfgforcesuspendobj, MUIA_Selected, forcesuspend);
        nnset(data->cfgsuspendtimeoutobj, MUIA_Numeric_Value, suspendtimeout);
        nnset(data->cfglinkpowerobj, MUIA_Selected, linkpower);
    }
}
/* \\\ */

Object *MyTextObject(char *Text,char *Help)
{
    Object	*obj;
    obj = TextObject,
                MUIA_ShortHelp, Help,
                ButtonFrame,
                MUIA_Background, MUII_ButtonBack,
                MUIA_CycleChain, 1,
                MUIA_InputMode, MUIV_InputMode_RelVerify,
                MUIA_Text_Contents, Text,
                End;

    return(obj);
}

Object *MyTextObjectDisabled(char *Text,char *Help)
{
    Object	*obj;
    obj = TextObject,
                MUIA_ShortHelp, Help,
                ButtonFrame,
                MUIA_Background, MUII_ButtonBack,
                MUIA_CycleChain, 1,
                MUIA_InputMode, MUIV_InputMode_RelVerify,
                MUIA_Text_Contents, Text,
                MUIA_Disabled, TRUE,
                End;

    return(obj);
}

/* /// "Action_OM_NEW()" */
Object * Action_OM_NEW(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data;

    IPTR bootdelay = 0;
    IPTR subtaskpri = 5;
    IPTR loginfo = TRUE;
    IPTR logwarn = TRUE;
    IPTR logerr = TRUE;
    IPTR logfail = TRUE;
    IPTR popupnew = 0;
    IPTR popupgone = FALSE;
    IPTR popupdeath = FALSE;
    IPTR popupdelay = 0;
    IPTR popupactivate = FALSE;
    IPTR popuptofront = TRUE;
    IPTR autodisablelp = FALSE;
    IPTR autodisabledead = FALSE;
    IPTR autorestartdead = TRUE;
    IPTR powersaving = FALSE;
    IPTR forcesuspend = FALSE;
    IPTR suspendtimeout = 30;
    IPTR linkpower = TRUE;
    STRPTR devdtxsoundfile = "";
    STRPTR devremsoundfile = "";
    APTR stackcfg = NULL;
    APTR pd = NULL;
    struct DevListEntry *dlnode;

    mainpanels[0] = _(MSG_MAINPANEL_GENERAL);
    mainpanels[1] = _(MSG_MAINPANEL_CONTROLLERS);
    mainpanels[2] = _(MSG_MAINPANEL_DEVICES);
    mainpanels[3] = _(MSG_MAINPANEL_CLASSES);
    mainpanels[4] = _(MSG_MAINPANEL_OPTIONS);
    mainpanels[5] = _(MSG_MAINPANEL_POPUPS);
    mainpanels[6] = _(MSG_MAINPANEL_CONFIG);

    errlvlstrings[0] = _(MSG_ERRLVL_ALL);
    errlvlstrings[1] = _(MSG_ERRLVL_WARNINGS);
    errlvlstrings[2] = _(MSG_ERRLVL_ERRORS);
    errlvlstrings[3] = _(MSG_ERRLVL_FAILURES);

    popupnewdevicestrings[0] = _(MSG_POPUP_NEVER);
    popupnewdevicestrings[1] = _(MSG_POPUP_ERROR);
    popupnewdevicestrings[2] = _(MSG_POPUP_UNKNOWN);
    popupnewdevicestrings[3] = _(MSG_POPUP_NOBINDING);
    popupnewdevicestrings[4] = _(MSG_POPUP_NOCONFIG);
    popupnewdevicestrings[5] = _(MSG_POPUP_CONFIGCLASS);
    popupnewdevicestrings[6] = _(MSG_POPUP_BINDING);
    popupnewdevicestrings[7] = _(MSG_POPUP_ALWAYS);

    if(!(obj = (Object *) DoSuperMethodA(cl, obj, msg)))
        return(0);

    data = INST_DATA(cl, obj);
    data->selfobj = obj;
    NewList(&data->hwlist);
    NewList(&data->devlist);
    NewList(&data->clslist);
    NewList(&data->errlist);
    NewList(&data->prefslist);

    data->acthlnode = NULL;
    data->errorlevel = 0;
    data->HardwareDisplayHook.h_Data = data;
    data->DeviceDisplayHook.h_Data = data;
    data->ClassDisplayHook.h_Data = data;
    data->ErrorDisplayHook.h_Data = data;
    data->IconDisplayHook.h_Data = data;
    data->PrefsDisplayHook.h_Data = data;

    data->HardwareDisplayHook.h_Entry = (APTR) HardwareListDisplayHook;
    data->DeviceDisplayHook.h_Entry = (APTR) DeviceListDisplayHook;
    data->ClassDisplayHook.h_Entry = (APTR) ClassListDisplayHook;
    data->ErrorDisplayHook.h_Entry = (APTR) ErrorListDisplayHook;
    data->IconDisplayHook.h_Entry = (APTR) IconListDisplayHook;
    data->PrefsDisplayHook.h_Entry = (APTR) PrefsListDisplayHook;

    /* get current global config */
    psdGetAttrs(PGA_STACK, NULL, PA_GlobalConfig, &stackcfg, TAG_END);
    if(stackcfg)
    {
        psdGetAttrs(PGA_STACKCFG, stackcfg,
                    GCA_SubTaskPri, &subtaskpri,
                    GCA_BootDelay, &bootdelay,
                    GCA_LogInfo, &loginfo,
                    GCA_LogWarning, &logwarn,
                    GCA_LogError, &logerr,
                    GCA_LogFailure, &logfail,
                    GCA_PopupDeviceNew, &popupnew,
                    GCA_PopupDeviceGone, &popupgone,
                    GCA_PopupDeviceDeath, &popupdeath,
                    GCA_PopupCloseDelay, &popupdelay,
                    GCA_PopupActivateWin, &popupactivate,
                    GCA_PopupWinToFront, &popuptofront,
                    GCA_InsertionSound, &devdtxsoundfile,
                    GCA_RemovalSound, &devremsoundfile,
                    GCA_AutoDisableLP, &autodisablelp,
                    GCA_AutoDisableDead, &autodisabledead,
                    GCA_AutoRestartDead, &autorestartdead,
                    GCA_PowerSaving, &powersaving,
                    GCA_ForceSuspend, &forcesuspend,
                    GCA_SuspendTimeout, &suspendtimeout,
                    GCA_LinkPowerMgmt, &linkpower,
                    TAG_END);
    }

    /* General panel */
    data->cfgcntobj[0] = ScrollgroupObject,
        MUIA_HelpNode, "neptuneusbgeneral",
        MUIA_Scrollgroup_Contents, VirtgroupObject,
            Child, VGroup,
                Child, VSpace(0),
                Child, HGroup,
                    Child, HSpace(0),
                    Child, VGroup,
                        Child, VSpace(0),
                        Child, Label(__(MSG_PANEL_GENERAL_LABEL1)),
                        Child, Label(__(MSG_PANEL_GENERAL_LABEL2)),
                        Child, VSpace(20),
                        Child, Label(__(MSG_PANEL_GENERAL_LABEL3)),
                        Child, Label(__(MSG_PANEL_GENERAL_LABEL4)),
                        Child, VSpace(20),
                        Child, Label(__(MSG_PANEL_GENERAL_LABEL5)),
                        Child, VSpace(0),
                        End,
                    Child, HSpace(0),
                    End,
                Child, VSpace(0),
                End,
            End,
        End;

    /* Hardware panel */
    data->cfgcntobj[1] = VGroup,
        MUIA_HelpNode, "neptuneusbhardware",
        Child, Label(__(MSG_PANEL_HARDWARE_LABEL1)),
        Child, ListviewObject,
            MUIA_CycleChain, 1,
            MUIA_ShortHelp, __(MSG_PANEL_HARDWARE_CONTROLLER_HELP),
            MUIA_Listview_List, data->hwlistobj =
    NewObject(IconListClass->mcc_Class, 0, InputListFrame, MUIA_List_MinLineHeight, 16, MUIA_List_Format, "BAR,BAR,BAR,", MUIA_List_Title, TRUE, MUIA_List_DisplayHook, &data->HardwareDisplayHook, TAG_END),
            End,
        Child, data->hwdevgrpobj = HGroup,
            MUIA_Disabled, TRUE,
            Child, Label(__(MSG_PANEL_HARDWARE_LABEL2)),
            Child, data->hwdevaslobj = PopaslObject,
                MUIA_CycleChain, 1,
                MUIA_ShortHelp, __(MSG_PANEL_HARDWARE_DEVICE_HELP),
                MUIA_Popstring_String, data->hwdevobj = StringObject,
                    StringFrame,
                    MUIA_CycleChain, 1,
                    MUIA_String_AdvanceOnCR, TRUE,
                    End,
                MUIA_Popstring_Button, PopButton(MUII_PopFile),
                ASLFR_TitleText, __(MSG_PANEL_HARDWARE_SELECT),
                End,
            Child, Label(__(MSG_PANEL_HARDWARE_UNIT)),
            Child, data->hwunitobj = StringObject,
                MUIA_ShortHelp, __(MSG_PANEL_HARDWARE_UNIT_HELP),
                StringFrame,
                MUIA_HorizWeight, 10,
                MUIA_CycleChain, 1,
                MUIA_String_AdvanceOnCR, TRUE,
                MUIA_String_Integer, 0,
                MUIA_String_Accept, "0123456789",
                End,
            End,
        Child, ColGroup(3),
            MUIA_Group_SameWidth, TRUE,
            Child, data->hwnewobj = MyTextObject(_(MSG_PANEL_HARDWARE_DRIVER_NEW),_(MSG_PANEL_HARDWARE_DRIVER_NEW_HELP)),
            Child, data->hwcopyobj = MyTextObject(_(MSG_PANEL_HARDWARE_DRIVER_COPY),_(MSG_PANEL_HARDWARE_DRIVER_COPY_HELP)),
            Child, data->hwdelobj = MyTextObject(_(MSG_PANEL_HARDWARE_DRIVER_DELETE),_(MSG_PANEL_HARDWARE_DRIVER_DELETE_HELP)),
            Child, data->hwonlineobj = MyTextObject(_(MSG_PANEL_HARDWARE_DRIVER_ONLINE),_(MSG_PANEL_HARDWARE_DRIVER_ONLINE_HELP)),
            Child, data->hwofflineobj = MyTextObject(_(MSG_PANEL_HARDWARE_DRIVER_OFFLINE),_(MSG_PANEL_HARDWARE_DRIVER_OFFLINE_HELP)),
            Child, data->hwinfoobj = MyTextObject(_(MSG_PANEL_HARDWARE_DRIVER_INFO),_(MSG_PANEL_HARDWARE_DRIVER_INFO_HELP)),
            End,
        End;

    /* Devices panel */
    data->cfgcntobj[2] = VGroup,
        MUIA_HelpNode, "neptuneusbdevices",
        Child, Label(__(MSG_PANEL_DEVICES_LABEL)),
        Child, ListviewObject,
            MUIA_CycleChain, 1,
            MUIA_Listview_List, data->devlistobj =
    NewObject(IconListClass->mcc_Class, 0, MUIA_ShortHelp, __(MSG_PANEL_DEVICES_HELP), InputListFrame, MUIA_List_MinLineHeight, 16, MUIA_List_Format, "BAR,BAR,BAR,BAR,", MUIA_List_Title, TRUE, MUIA_List_DisplayHook, &data->DeviceDisplayHook, TAG_END),
            End,
        Child, ColGroup(4),
            MUIA_Group_SameWidth, TRUE,
            Child, data->devbindobj = MyTextObject(_(MSG_PANEL_DEVICES_SCAN),_(MSG_PANEL_DEVICES_SCAN_HELP)),
            Child, data->devunbindobj = MyTextObjectDisabled(_(MSG_PANEL_DEVICES_UNBIND),_(MSG_PANEL_DEVICES_UNBIND_HELP)),
            Child, data->devinfoobj = MyTextObject(_(MSG_PANEL_DEVICES_INFO),_(MSG_PANEL_DEVICES_INFO_HELP)),
            Child, data->devcfgobj = MyTextObjectDisabled(_(MSG_PANEL_DEVICES_SETTINGS),_(MSG_PANEL_DEVICES_SETTINGS_HELP)),
            /* one grid cell that shows whichever of the two the device state
               calls for; a page group is as wide as its widest page, so the
               switch never clips a translated label (MUI 3.8 would not
               re-layout a changed MUIA_Text_Contents) */
            Child, data->devsuspendpageobj = VGroup,
                MUIA_Group_PageMode, TRUE,
                MUIA_Group_ActivePage, MUIV_Group_ActivePage_First,
                Child, data->devsuspendobj = MyTextObjectDisabled(_(MSG_PANEL_DEVICES_SUSPEND),_(MSG_PANEL_DEVICES_SUSPEND_HELP)),
                Child, data->devresumeobj = MyTextObjectDisabled(_(MSG_PANEL_DEVICES_RESUME),_(MSG_PANEL_DEVICES_RESUME_HELP)),
                End,
            Child, data->devpowercycleobj = MyTextObjectDisabled(_(MSG_PANEL_DEVICES_POWERCYCLE),_(MSG_PANEL_DEVICES_POWERCYCLE_HELP)),
            Child, data->devdisableobj = MyTextObjectDisabled(_(MSG_PANEL_DEVICES_DISABLE),_(MSG_PANEL_DEVICES_DISABLE_HELP)),
            Child, data->devejectobj = MyTextObjectDisabled(_(MSG_PANEL_DEVICES_EJECT),_(MSG_PANEL_DEVICES_EJECT_HELP)),
            End,
        End;

    /* Classes panel */
    data->cfgcntobj[3] = VGroup,
        MUIA_HelpNode, "neptuneusbclasses",
        Child, Label(__(MSG_PANEL_CLASSES)),
        Child, data->clslistobj = ListviewObject,
            MUIA_CycleChain, 1,
            MUIA_Listview_List, ListObject,
                MUIA_ShortHelp, __(MSG_PANEL_CLASSES_HELP),
                InputListFrame,
                MUIA_List_Format, "BAR,BAR,",
                MUIA_List_Title, TRUE,
                MUIA_List_DisplayHook, &data->ClassDisplayHook,
                End,
            End,
        Child, HGroup,
            Child, Label(__(MSG_PANEL_CLASSES_LABEL)),
            Child, PopaslObject,
                MUIA_CycleChain, 1,
                MUIA_ShortHelp, __(MSG_PANEL_CLASSES_LABEL_HELP),
                MUIA_Popstring_String, data->clsnameobj = StringObject,
                    StringFrame,
                    MUIA_CycleChain, 1,
                    MUIA_String_AdvanceOnCR, TRUE,
                    MUIA_String_Contents, NEP_CLASSDRAWER "/",
                    End,
                MUIA_Popstring_Button, PopButton(MUII_PopFile),
                ASLFR_TitleText, __(MSG_PANEL_CLASSES_SELECT),
                End,
            End,
        Child, HGroup,
            MUIA_Group_SameWidth, TRUE,
            Child, data->clsaddobj = MyTextObject(_(MSG_PANEL_CLASSES_ADD),_(MSG_PANEL_CLASSES_ADD_HELP)),
            Child, data->clsremobj = MyTextObjectDisabled(_(MSG_PANEL_CLASSES_REMOVE),_(MSG_PANEL_CLASSES_REMOVE_HELP)),
            Child, data->clscfgobj = MyTextObjectDisabled(_(MSG_PANEL_CLASSES_CONFIG),_(MSG_PANEL_CLASSES_CONFIG_HELP)),
            Child, data->clsscanobj = MyTextObject(_(MSG_PANEL_CLASSES_SCAN),_(MSG_PANEL_CLASSES_SCAN_HELP)),
            End,
        End;

    /* Options panel */
    data->cfgcntobj[4] = VGroup,
        MUIA_HelpNode, "neptuneusboptions",
        Child, Label(__(MSG_PANEL_OPTIONS)),
        Child, VSpace(0),
        Child, VGroup, GroupFrameT(__(MSG_PANEL_OPTIONS_STACK)),
            Child, VSpace(0),
            Child, HGroup,
                Child, Label(__(MSG_PANEL_OPTIONS_PRIORITY)),
                Child, data->cfgtaskpriobj = SliderObject, SliderFrame,
                    MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_PRIORITY_HELP),
                    MUIA_CycleChain, 1,
                    MUIA_Numeric_Min, 0,
                    MUIA_Numeric_Max, 127,
                    MUIA_Numeric_Value, subtaskpri,
                    End,
                    Child, Label(__(MSG_PANEL_OPTIONS_BOOT)),
                    Child, data->cfgbootdelayobj = SliderObject, SliderFrame,
                        MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_BOOT_HELP),
                        MUIA_CycleChain, 1,
                        MUIA_Numeric_Min, 0,
                        MUIA_Numeric_Max, 15,
                        MUIA_Numeric_Value, bootdelay,
                        MUIA_Numeric_Format,__(MSG_PANEL_OPTIONS_BOOT_FORMAT),
                        End,
                End,
            Child, ColGroup(2),
                Child, HGroup,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_OPTIONS_DISABLELOW)),
                    End,
                Child, data->cfgautolpobj = ImageObject, ImageButtonFrame,
                    MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_DISABLELOW_HELP),
                    MUIA_Background, MUII_ButtonBack,
                    MUIA_CycleChain, 1,
                    MUIA_InputMode, MUIV_InputMode_Toggle,
                    MUIA_Image_Spec, MUII_CheckMark,
                    MUIA_Image_FreeVert, TRUE,
                    MUIA_Selected, autodisablelp,
                    MUIA_ShowSelState, FALSE,
                    End,
                Child, HGroup,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_OPTIONS_DISABLEDEAD)),
                    End,
                Child, data->cfgautodeadobj = ImageObject, ImageButtonFrame,
                    MUIA_ShortHelp,__(MSG_PANEL_OPTIONS_DISABLEDEAD_HELP),
                    MUIA_Background, MUII_ButtonBack,
                    MUIA_CycleChain, 1,
                    MUIA_InputMode, MUIV_InputMode_Toggle,
                    MUIA_Image_Spec, MUII_CheckMark,
                    MUIA_Image_FreeVert, TRUE,
                    MUIA_Selected, autodisabledead,
                    MUIA_Disabled, autorestartdead,
                    MUIA_ShowSelState, FALSE,
                    End,
                Child, HGroup,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_OPTIONS_POWERCYCLE)),
                    End,
                Child, data->cfgautopcobj = ImageObject, ImageButtonFrame,
                    MUIA_ShortHelp,__(MSG_PANEL_OPTIONS_POWERCYCLE_HELP),
                    MUIA_Background, MUII_ButtonBack,
                    MUIA_CycleChain, 1,
                    MUIA_InputMode, MUIV_InputMode_Toggle,
                    MUIA_Image_Spec, MUII_CheckMark,
                    MUIA_Image_FreeVert, TRUE,
                    MUIA_Selected, autorestartdead,
                    MUIA_ShowSelState, FALSE,
                    End,
                Child, HGroup,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_OPTIONS_POWERSAVE)),
                    End,
                Child, data->cfgpowersavingobj = ImageObject, ImageButtonFrame,
                    MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_POWERSAVE_HELP),
                    MUIA_Background, MUII_ButtonBack,
                    MUIA_CycleChain, 1,
                    MUIA_InputMode, MUIV_InputMode_Toggle,
                    MUIA_Image_Spec, MUII_CheckMark,
                    MUIA_Image_FreeVert, TRUE,
                    MUIA_Selected, powersaving,
                    MUIA_ShowSelState, FALSE,
                    End,
                Child, HGroup,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_OPTIONS_FORCESUSPEND)),
                    End,
                Child, data->cfgforcesuspendobj = ImageObject, ImageButtonFrame,
                    MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_FORCESUSPEND_HELP),
                    MUIA_Background, MUII_ButtonBack,
                    MUIA_CycleChain, 1,
                    MUIA_InputMode, MUIV_InputMode_Toggle,
                    MUIA_Image_Spec, MUII_CheckMark,
                    MUIA_Image_FreeVert, TRUE,
                    MUIA_Selected, forcesuspend,
                    MUIA_ShowSelState, FALSE,
                    End,
                Child, HGroup,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_OPTIONS_LINKPOWER)),
                    End,
                Child, data->cfglinkpowerobj = ImageObject, ImageButtonFrame,
                    MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_LINKPOWER_HELP),
                    MUIA_Background, MUII_ButtonBack,
                    MUIA_CycleChain, 1,
                    MUIA_InputMode, MUIV_InputMode_Toggle,
                    MUIA_Image_Spec, MUII_CheckMark,
                    MUIA_Image_FreeVert, TRUE,
                    MUIA_Selected, linkpower,
                    MUIA_ShowSelState, FALSE,
                    End,
                End,
            Child, HGroup,
                Child, Label(__(MSG_PANEL_OPTIONS_INACTIVITY)),
                Child, data->cfgsuspendtimeoutobj = SliderObject, SliderFrame,
                    MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_INACTIVITY_HELP),
                    MUIA_CycleChain, 1,
                    MUIA_Numeric_Min, 5,
                    MUIA_Numeric_Max, 600,
                    MUIA_Numeric_Value, suspendtimeout,
                    MUIA_Numeric_Format, __(MSG_PANEL_OPTIONS_INACTIVITY_FORMAT),
                    End,
                End,
            Child, VSpace(0),
            End,
        Child, VSpace(0),
        Child, VGroup, GroupFrameT(__(MSG_PANEL_OPTIONS_LOGGING)),
            Child, VSpace(0),
            Child, HGroup,
                Child, HSpace(0),
                Child, ColGroup(4),
                    Child, HGroup,
                        Child, HSpace(0),
                        Child, Label(__(MSG_PANEL_OPTIONS_LOGGING_LABEL)),
                        End,
                    Child, data->cfgloginfoobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp,__(MSG_PANEL_OPTIONS_LOGGING_LABEL_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, loginfo,
                        MUIA_ShowSelState, FALSE,
                        End,
                    Child, HGroup,
                        Child, HSpace(0),
                        Child, Label(__(MSG_PANEL_OPTIONS_LOGGING_WARN)),
                        End,
                    Child, data->cfglogwarnobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_LOGGING_WARN_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, logwarn,
                        MUIA_ShowSelState, FALSE,
                        End,
                    Child, HGroup,
                        Child, HSpace(0),
                        Child, Label(__(MSG_PANEL_OPTIONS_LOGGING_ERR)),
                        End,
                    Child, data->cfglogerrobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_LOGGING_ERR_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, logerr,
                        MUIA_ShowSelState, FALSE,
                        End,
                    Child, HGroup,
                        Child, HSpace(0),
                        Child, Label(__(MSG_PANEL_OPTIONS_LOGGING_FAIL)),
                        End,
                    Child, data->cfglogfailobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp, __(MSG_PANEL_OPTIONS_LOGGING_FAIL_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, logfail,
                        MUIA_ShowSelState, FALSE,
                        End,
                    End,
                Child, HSpace(0),
                End,
            Child, VSpace(0),
            End,
        Child, VSpace(0),
        Child, HGroup,
            Child, HSpace(0),
            Child, Label(__(MSG_PANEL_OPTIONS_MEMORY)),
            Child, data->mempoolobj = TextObject,
                MUIA_Text_Contents, "?",
                End,
            Child, HSpace(0),
            End,
        End;

    /* Notifier panel */
    data->cfgcntobj[5] = VGroup,
        MUIA_HelpNode, "neptuneusbpopups",
        Child, Label(__(MSG_PANEL_NOTIFY)),
        Child, VSpace(0),
        Child, VGroup, GroupFrameT(__(MSG_PANEL_NOTIFY_LABEL)),
            Child, VSpace(0),
            Child, ColGroup(2),
                Child, Label(__(MSG_PANEL_NOTIFY_CONNECT)),
                Child, HGroup,
                    Child, data->cfgpopupnewobj = CycleObject,
                        MUIA_ShortHelp, __(MSG_PANEL_NOTIFY_CONNECT_HELP),
                        MUIA_CycleChain, 1,
                        MUIA_Cycle_Entries, popupnewdevicestrings,
                        MUIA_Cycle_Active, popupnew,
                        End,
                    End,
                Child, Label(__(MSG_PANEL_NOTIFY_DISCONNECT)),
                Child, HGroup,
                    Child, data->cfgpopupgoneobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp, _(MSG_PANEL_NOTIFY_DISCONNECT_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, popupgone,
                        MUIA_ShowSelState, FALSE,
                        End,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_NOTIFY_DEATH)),
                    Child, data->cfgpopupdeathobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp, __(MSG_PANEL_NOTIFY_DEATH_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, popupdeath,
                        MUIA_ShowSelState, FALSE,
                        End,
                    End,
                Child, Label(__(MSG_PANEL_NOTIFY_DELAY)),
                Child, data->cfgpopupdelayobj = SliderObject, SliderFrame,
                    MUIA_ShortHelp, __(MSG_PANEL_NOTIFY_DELAY_HELP),
                    MUIA_CycleChain, 1,
                    MUIA_Numeric_Min, 0,
                    MUIA_Numeric_Max, 25,
                    MUIA_Numeric_Value, popupdelay,
                    MUIA_Numeric_Format, __(MSG_PANEL_NOTIFY_DELAY_FORMAT),
                    End,
                Child, Label(__(MSG_PANEL_NOTIFY_ACTIVATE)),
                Child, HGroup,
                    Child, data->cfgpopupactivateobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp, __(MSG_PANEL_NOTIFY_ACTIVATE_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, popupactivate,
                        MUIA_ShowSelState, FALSE,
                        End,
                    Child, HSpace(0),
                    Child, Label(__(MSG_PANEL_NOTIFY_TOFRONT)),
                    Child, data->cfgpopuptofrontobj = ImageObject, ImageButtonFrame,
                        MUIA_ShortHelp, __(MSG_PANEL_NOTIFY_TOFRONT_HELP),
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, popuptofront,
                        MUIA_ShowSelState, FALSE,
                        End,
                    End,
                Child, Label(__(MSG_PANEL_NOTIFY_CONSOUND)),
                Child, PopaslObject,
                    MUIA_CycleChain, 1,
                    MUIA_ShortHelp, __(MSG_PANEL_NOTIFY_CONSOUND_HELP),
                    MUIA_Popstring_String, data->cfgdevdtxsoundobj = StringObject,
                        StringFrame,
                        MUIA_CycleChain, 1,
                        MUIA_String_AdvanceOnCR, TRUE,
                        MUIA_String_Contents, devdtxsoundfile,
                        End,
                    MUIA_Popstring_Button, PopButton(MUII_PopFile),
                    ASLFR_TitleText, __(MSG_PANEL_NOTIFY_CONSOUND_SELECT),
                    End,
                Child, Label(__(MSG_PANEL_NOTIFY_DISCONSOUND)),
                Child, PopaslObject,
                    MUIA_CycleChain, 1,
                    MUIA_ShortHelp, __(MSG_PANEL_NOTIFY_DISCONSOUND_HELP),
                    MUIA_Popstring_String, data->cfgdevremsoundobj = StringObject,
                        StringFrame,
                        MUIA_CycleChain, 1,
                        MUIA_String_AdvanceOnCR, TRUE,
                        MUIA_String_Contents, devremsoundfile,
                        End,
                    MUIA_Popstring_Button, PopButton(MUII_PopFile),
                    ASLFR_TitleText, __(MSG_PANEL_NOTIFY_DISCONSOUND_SELECT),
                    End,
                End,
            Child, VSpace(0),
            End,
            Child, VSpace(0),
        End;

    /* Configuration panel */
    data->cfgcntobj[6] = VGroup,
        MUIA_HelpNode, "neptuneusbconfig",
        Child, Label(__(MSG_PANEL_CONFIG)),
        Child, data->prefslistobj = ListviewObject,
            MUIA_CycleChain, 1,
            MUIA_ShortHelp, __(MSG_PANEL_CONFIG_HELP),
            MUIA_Listview_DragType, MUIV_Listview_DragType_Immediate,
            MUIA_Listview_List,
    NewObject(CfgListClass->mcc_Class, 0, InputListFrame, MUIA_List_Format, "BAR,BAR,BAR,", MUIA_List_Title, TRUE, MUIA_List_DisplayHook, &data->PrefsDisplayHook, MUIA_List_ShowDropMarks, TRUE, MUIA_List_AutoVisible, TRUE, TAG_END),
            End,
        Child, HGroup,
            MUIA_Group_SameWidth, TRUE,
            Child, data->prefssaveasobj = MyTextObject(_(MSG_PANEL_CONFIG_SAVEAS),_(MSG_PANEL_CONFIG_SAVEAS_HELP)),
            Child, data->prefsexportobj = MyTextObjectDisabled(_(MSG_PANEL_CONFIG_EXPORT),_(MSG_PANEL_CONFIG_EXPORT_HELP)),
            Child, data->prefsimportobj = MyTextObject(_(MSG_PANEL_CONFIG_IMPORT),_(MSG_PANEL_CONFIG_IMPORT_HELP)),
            Child, data->prefsremoveobj = MyTextObjectDisabled(_(MSG_PANEL_CONFIG_REMOVE),_(MSG_PANEL_CONFIG_REMOVE_HELP)),
            End,
        End;

    data->mainobj = VGroup,
        Child, HGroup,
            Child, ListviewObject,
                MUIA_CycleChain, 1,
                MUIA_ShowMe, !registermode,
                MUIA_Listview_MultiSelect, MUIV_Listview_MultiSelect_None,
                MUIA_Listview_List, data->cfgpagelv =
    NewObject(IconListClass->mcc_Class, 0, InputListFrame, MUIA_List_MinLineHeight, 16, MUIA_List_SourceArray, mainpanels, MUIA_List_AdjustWidth, TRUE, MUIA_List_Active, 0, MUIA_List_DisplayHook, &data->IconDisplayHook, TAG_END),
                End,
            Child, VGroup,
                /*ReadListFrame,*/
				/*MUIA_Background, MUII_GroupBack,*/
                Child, data->cfgpagegrp = (registermode ?
                    (RegisterGroup(mainpanels),
                    MUIA_CycleChain, 1,
                    MUIA_Register_Frame, TRUE,
                    Child, data->cfgcntobj[0],
                    Child, data->cfgcntobj[1],
                    Child, data->cfgcntobj[2],
                    Child, data->cfgcntobj[3],
                    Child, data->cfgcntobj[4],
                    Child, data->cfgcntobj[5],
                    Child, data->cfgcntobj[6],
                    End) :
                    (VGroup,
                    MUIA_Group_PageMode, TRUE,
                    MUIA_Group_ActivePage, MUIV_Group_ActivePage_First,
                    Child, data->cfgcntobj[0],
                    Child, data->cfgcntobj[1],
                    Child, data->cfgcntobj[2],
                    Child, data->cfgcntobj[3],
                    Child, data->cfgcntobj[4],
                    Child, data->cfgcntobj[5],
                    Child, data->cfgcntobj[6],
                    End)),
                End,
            End,
        Child, BalanceObject,
            MUIA_ObjectID, MAKE_ID('I','L','B','P'),
            End,
        Child, VGroup, GroupFrameT(__(MSG_PANEL_CONFIG_MSGLOG)),
            MUIA_VertWeight, 20,
            Child, HGroup,
                Child, Label(__(MSG_PANEL_CONFIG_INFLVL)),
                Child, data->errlvlobj = CycleObject,
                    MUIA_CycleChain, 1,
                    MUIA_ShortHelp, __(MSG_PANEL_CONFIG_INFLVL_HELP),
                    MUIA_Cycle_Entries, errlvlstrings,
                    End,
                Child, HSpace(0),
                Child, HGroup,
                    MUIA_Group_SameWidth, TRUE,
                    Child, data->errsaveobj = MyTextObject(_(MSG_PANEL_CONFIG_LOGSAVE),_(MSG_PANEL_CONFIG_LOGSAVE_HELP)),
                    Child, data->errflushobj = MyTextObject(_(MSG_PANEL_CONFIG_LOGFLUSH),_(MSG_PANEL_CONFIG_LOGFLUSH_HELP)),
                    End,
                End,
            Child, data->errlistobj = ListviewObject,
                MUIA_CycleChain, 1,
                MUIA_Listview_Input, FALSE,
                MUIA_Listview_List, ListObject,
                    MUIA_ShortHelp, __(MSG_PANEL_CONFIG_LOGHELP),
                    ReadListFrame,
                    MUIA_List_Format, "BAR,BAR,BAR,",
                    MUIA_List_DisplayHook, &data->ErrorDisplayHook,
                    End,
                End,
            End,
        Child, HGroup,
            MUIA_Group_SameWidth, TRUE,
            Child, data->onlineobj = MyTextObject( _(MSG_PANEL_CONFIG_ALLONLINE),_(MSG_PANEL_CONFIG_ALLONLINE_HELP)),
            Child, data->offlineobj = MyTextObject(_(MSG_PANEL_CONFIG_ALLOFFLINE),_(MSG_PANEL_CONFIG_ALLOFFLINE_HELP)),
            Child, data->restartobj = MyTextObject(_(MSG_PANEL_CONFIG_RESTART),_(MSG_PANEL_CONFIG_RESTART_HELP)),
            Child, HSpace(0),
            Child, data->saveobj = MyTextObject(_(MSG_PANEL_CONFIG_SAVE),_(MSG_PANEL_CONFIG_SAVE_HELP)),
            Child, data->useobj = MyTextObject(_(MSG_PANEL_CONFIG_USE),_(MSG_PANEL_CONFIG_USE_HELP)),
            End,
        End;

    DoMethod(obj, OM_ADDMEMBER, data->mainobj);

    DoMethod(data->cfgpagelv, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             data->cfgpagegrp, 3, MUIM_Set, MUIA_Group_ActivePage, MUIV_TriggerValue);
    DoMethod(data->cfgpagelv, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             obj, 1, MUIM_Action_Info_MemPool);

    DoMethod(data->hwnewobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_HW_New);
    DoMethod(data->hwcopyobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_HW_Copy);
    DoMethod(data->hwdelobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_HW_Del);
    DoMethod(data->hwinfoobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_HW_Info);
    DoMethod(data->hwonlineobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_HW_Online);
    DoMethod(data->hwofflineobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_HW_Offline);

    DoMethod(data->hwlistobj, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE,
             obj, 1, MUIM_Action_HW_Info);
    DoMethod(data->hwlistobj, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             obj, 1, MUIM_Action_HW_Activate);
    DoMethod(data->hwdevobj, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             obj, 2, MUIM_Action_HW_Update, NULL);
    DoMethod(data->hwunitobj, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             obj, 2, MUIM_Action_HW_Update, NULL);

    DoMethod(data->errsaveobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_SaveErrors);
    DoMethod(data->errflushobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_FlushErrors);
    DoMethod(data->errlvlobj, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_Action_ChgErrLevel);

    DoMethod(data->onlineobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Online);
    DoMethod(data->offlineobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Offline);
    DoMethod(data->restartobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Restart);

    DoMethod(data->saveobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_SaveQuit);
    DoMethod(data->useobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_UseQuit);

    DoMethod(data->devlistobj, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             obj, 1, MUIM_Action_Dev_Activate);
    DoMethod(data->devbindobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Bind);
    DoMethod(data->devinfoobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Info);
    DoMethod(data->devunbindobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Unbind);
    DoMethod(data->devsuspendobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Suspend);
    DoMethod(data->devresumeobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Resume);
    DoMethod(data->devpowercycleobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_PowerCycle);
    DoMethod(data->devdisableobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Disable);
    DoMethod(data->devejectobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Eject);
    DoMethod(data->devlistobj, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE,
             obj, 1, MUIM_Action_Dev_Info);
    DoMethod(data->devlistobj, MUIM_Notify, MUIA_ContextMenuTrigger, MUIV_EveryTime,
             obj, 2, MUIM_Action_Dev_ForceBind, MUIV_TriggerValue);

    DoMethod(data->devcfgobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Dev_Configure);

    DoMethod(data->clslistobj, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cls_Activate);
    DoMethod(data->clsaddobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Cls_Add);
    DoMethod(data->clsnameobj, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cls_Add);
    DoMethod(data->clsremobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Cls_Remove);
    DoMethod(data->clscfgobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Cls_Configure);
    DoMethod(data->clsscanobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Cls_Scan);
    DoMethod(data->clslistobj, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE,
             obj, 1, MUIM_Action_Cls_Configure);

    DoMethod(data->cfgtaskpriobj, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgbootdelayobj, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgloginfoobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfglogwarnobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfglogerrobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfglogfailobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);

    DoMethod(data->cfgpopupnewobj, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgpopupgoneobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgpopupdeathobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgpopupdelayobj, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgpopupactivateobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgpopuptofrontobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgdevdtxsoundobj, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Snd_Changed);
    DoMethod(data->cfgdevremsoundobj, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Snd_Changed);

    DoMethod(data->cfgautolpobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgautodeadobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgautopcobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);

    DoMethod(data->cfgpowersavingobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgforcesuspendobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfgsuspendtimeoutobj, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);
    DoMethod(data->cfglinkpowerobj, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Changed);

    DoMethod(data->prefslistobj, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             obj, 1, MUIM_Action_Cfg_Activate);
    DoMethod(data->prefssaveasobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_SavePrefsAs);
    DoMethod(data->prefsexportobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Cfg_Export);
    DoMethod(data->prefsimportobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Cfg_Import);
    DoMethod(data->prefsremoveobj, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_Action_Cfg_Remove);

    data->mi_classpopup = CreateClassPopup();
    set(data->devlistobj, MUIA_ContextMenu, data->mi_classpopup);

    Action_HW_Activate(cl, obj, msg);

    CreateErrorList(data);
    SetupEventHandler(data);
    while((pd = psdGetNextDevice(pd)))
    {
        dlnode = AllocDevEntry(data, pd);
        if(dlnode)
        {
            DoMethod(data->devlistobj, MUIM_List_InsertSingle, dlnode, MUIV_List_Insert_Bottom);
        }
    }
    UpdateConfigToGUI(data);
    return(obj);
}
/* \\\ */

/* /// "Action_OM_DISPOSE()" */
IPTR Action_OM_DISPOSE(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    FreeErrorList(data);
    FreePrefsList(data);
    FreeGUILists(data);
    if(data->mi_classpopup)
    {
        MUI_DisposeObject(data->mi_classpopup);
        data->mi_classpopup = NULL;
    }
    CleanupEventHandler(data);
    return(FALSE);
}
/* \\\ */

/* /// "Action_Setup()" */
IPTR Action_Setup(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);

    data->appobj = _app(obj);
    data->winobj = _win(obj);
    if(!DoSuperMethodA(cl,obj,msg)) return(FALSE);

    data->eventihn.ihn_Object = obj;
    data->eventihn.ihn_Signals = 1UL<<data->eventmsgport->mp_SigBit;
    data->eventihn.ihn_Flags = 0;
    data->eventihn.ihn_Method = MUIM_Action_HandlePsdEvents;
    DoMethod(data->appobj, MUIM_Application_AddInputHandler, &data->eventihn);
    //MUI_RequestIDCMP(obj, IDCMP_INTUITICKS);
    return(TRUE);
}
/* \\\ */

/* /// "Action_HW_New()" */
IPTR Action_HW_New(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    hlnode = AllocHWEntry(data, NULL);
    if(hlnode)
    {
        hlnode->devname = psdCopyStr("");
        DoMethod(data->hwlistobj, MUIM_List_InsertSingle, hlnode, MUIV_List_Insert_Bottom);
        set(data->hwlistobj, MUIA_List_Active, MUIV_List_Active_Bottom);
        DoMethod(data->hwdevaslobj, MUIM_Popstring_Open);
        InternalCreateConfigGUI(data);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_HW_Copy()" */
IPTR Action_HW_Copy(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    if(data->acthlnode)
    {
        hlnode = AllocHWEntry(data, NULL);
        if(hlnode)
        {
            hlnode->devname = psdCopyStr(data->acthlnode->devname);
            hlnode->unit = data->acthlnode->unit + 1;
            DoMethod(data->hwlistobj, MUIM_List_InsertSingle, hlnode, MUIV_List_Insert_Bottom);
            set(data->hwlistobj, MUIA_List_Active, MUIV_List_Active_Bottom);
            InternalCreateConfigGUI(data);
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_HW_Del()" */
IPTR Action_HW_Del(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    if(data->acthlnode)
    {
        hlnode = data->acthlnode;
        DoMethod(data->hwlistobj, MUIM_List_Remove, MUIV_List_Remove_Active);
        FreeHWEntry(data, hlnode);
        InternalCreateConfigGUI(data);
    }
    return(TRUE);
}
/* \\\ */

/* /// "SetHWDevGadget()" */
/* Entries hold the bare driver name (<hwmatch.h>); the Device field shows it in
   NEP_HWDRAWER, where the library loads it from. That is also all Popasl's
   default path/file split needs to open the requester in the right drawer.
   Action_HW_Update() strips the path again. */
static void SetHWDevGadget(struct ActionData *data, struct HWListEntry *hlnode)
{
    char path[108];

    psdSafeRawDoFmt(path, sizeof(path), NEP_HWDRAWER "/%s",
                    hlnode->devname ? hlnode->devname : (STRPTR) "");
    nnset(data->hwdevobj, MUIA_String_Contents, path);
}
/* \\\ */

/* /// "Action_HW_Update()" */
IPTR Action_HW_Update(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    hlnode = (struct HWListEntry *) ((struct opSet *) msg)->ops_AttrList;
    if(!hlnode)
    {
        hlnode = data->acthlnode;
    }
    if(hlnode)
    {
        STRPTR str = "";
        get(data->hwdevobj, MUIA_String_Contents, &str);
        /* An ASL pick or a typed name, with or without a path, keeps only its
           trailing component; the field is redrawn in its canonical form. */
        psdFreeVec(hlnode->devname);
        hlnode->devname = psdCopyStr((STRPTR) psdHwFilePart(str));
        SetHWDevGadget(data, hlnode);
        get(data->hwunitobj, MUIA_String_Integer, &hlnode->unit);
        DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
        InternalCreateConfigGUI(data);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_HW_Activate()" */
IPTR Action_HW_Activate(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    if(data->acthlnode)
    {
        DoMethod(obj, MUIM_Action_HW_Update, data->acthlnode);
    }
    DoMethod(data->hwlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &hlnode);
    if((data->acthlnode = hlnode))
    {
        set(data->hwdevgrpobj, MUIA_Disabled, FALSE);
        set(data->hwcopyobj, MUIA_Disabled, FALSE);
        set(data->hwdelobj, MUIA_Disabled, FALSE);
        set(data->hwinfoobj, MUIA_Disabled, !hlnode->phw);
        set(data->hwofflineobj, MUIA_Disabled, !hlnode->phw);
        set(data->hwonlineobj, MUIA_Disabled, hlnode->phw);
        SetHWDevGadget(data, hlnode);
        set(data->hwunitobj, MUIA_String_Integer, hlnode->unit);
    } else {
        set(data->hwdevgrpobj, MUIA_Disabled, TRUE);
        set(data->hwcopyobj, MUIA_Disabled, TRUE);
        set(data->hwdelobj, MUIA_Disabled, TRUE);
        set(data->hwinfoobj, MUIA_Disabled, TRUE);
        set(data->hwofflineobj, MUIA_Disabled, TRUE);
        set(data->hwonlineobj, MUIA_Disabled, TRUE);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_HW_Info()" */
IPTR Action_HW_Info(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    IPTR revision = 0;
    IPTR version = 0;
    IPTR unitnr = -1;
    STRPTR devname = _(MSG_UNKNOWN);
    STRPTR manufacturer = _(MSG_UNKNOWN);
    STRPTR prodname = _(MSG_UNKNOWN);
    STRPTR description = _(MSG_UNKNOWN);
    STRPTR copyright = _(MSG_UNKNOWN);
    STRPTR textbuf1;

    DoMethod(data->hwlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &hlnode);
    if(hlnode)
    {
        if(hlnode->infowindow || (!hlnode->phw))
        {
            return(TRUE);
        }
        psdGetAttrs(PGA_HARDWARE, hlnode->phw,
                    HA_DeviceName, &devname,
                    HA_DeviceUnit, &unitnr,
                    HA_Version, &version,
                    HA_Revision, &revision,
                    HA_ProductName, &prodname,
                    HA_Manufacturer, &manufacturer,
                    HA_Description, &description,
                    HA_Copyright, &copyright,
                    TAG_END);

        textbuf1 = psdCopyStrFmt("%s\n%ld\nV%ld.%ld\n%s\n%s\n%s\n%s",
                        devname, unitnr, version, revision, prodname,
                        manufacturer, description, copyright);
        hlnode->infowindow = WindowObject,
            MUIA_Window_ID   , MAKE_ID('H','I','N','F'),
            MUIA_Window_Title, __(MSG_HARDWARE_WINDOW),
            MUIA_Window_IsSubWindow, FALSE,
            WindowContents, HGroup,
                MUIA_ShortHelp, __(MSG_HARDWARE_WINDOW_HELP),
                MUIA_FrameTitle, __(MSG_HARDWARE_WINDOW_FRAME),
                Child, LabelB(__(MSG_HARDWARE_WINDOW_LABEL)),
                Child, TextObject,
                    TextFrame,
                    MUIA_Background, MUII_TextBack,
                    MUIA_Text_Contents, textbuf1,
                    End,
                End,
            End;
        if(hlnode->infowindow)
        {
            DoMethod(data->appobj, OM_ADDMEMBER, hlnode->infowindow);
            DoMethod(hlnode->infowindow, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
                     data->appobj, 5, MUIM_Application_PushMethod, obj, 2, MUIM_Action_CloseSubWinReq, hlnode);

            set(hlnode->infowindow, MUIA_Window_Open, TRUE);
        }
        psdFreeVec(textbuf1);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_HW_Online()" */
IPTR Action_HW_Online(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode = data->acthlnode;
    if((!hlnode) || hlnode->phw)
    {
        return(FALSE);
    }

    set(data->appobj, MUIA_Application_Sleep, TRUE);
    if((hlnode->phw = psdAddHardware(hlnode->devname, hlnode->unit)))
    {
        psdGetAttrs(PGA_HARDWARE, hlnode->phw,
                    HA_ProductName, &hlnode->prodname,
                    TAG_END);
        if(psdEnumerateHardware(hlnode->phw) == NULL) {
            psdRemHardware(hlnode->phw);
            hlnode->phw = NULL;
            hlnode->prodname = NULL;
        }
    }

    DoMethod(obj, MUIM_Action_HW_Activate);
    DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    psdClassScan();
    set(data->appobj, MUIA_Application_Sleep, FALSE);
    // update the online status to the config
    InternalCreateConfigGUI(data);
    return(TRUE);
}
/* \\\ */

/* /// "Action_HW_Offline()" */
IPTR Action_HW_Offline(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode = data->acthlnode;

    if((!hlnode) || (!hlnode->phw))
    {
        return(FALSE);
    }

    set(data->appobj, MUIA_Application_Sleep, TRUE);

    psdRemHardware(hlnode->phw);
    hlnode->phw = NULL;
    hlnode->prodname = NULL;

    DoMethod(obj, MUIM_Action_HW_Activate);
    DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    set(data->appobj, MUIA_Application_Sleep, FALSE);
    // update the online status to the config
    // NOTE! This means that the hardware will not come online on next loading.
    InternalCreateConfigGUI(data);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Online()" */
IPTR Action_Online(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    set(data->appobj, MUIA_Application_Sleep, TRUE);
    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    while(hlnode->node.ln_Succ)
    {
        if(!hlnode->phw)
        {
            if((hlnode->phw = psdAddHardware(hlnode->devname, hlnode->unit)))
            {
                psdGetAttrs(PGA_HARDWARE, hlnode->phw,
                            HA_ProductName, &hlnode->prodname,
                            TAG_END);
                psdEnumerateHardware(hlnode->phw);
            }
        }
        hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
    }
    DoMethod(obj, MUIM_Action_HW_Activate);
    DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    psdClassScan();
    set(data->appobj, MUIA_Application_Sleep, FALSE);
    // update the online status to the config
    InternalCreateConfigGUI(data);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Offline()" */
IPTR Action_Offline(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    set(data->appobj, MUIA_Application_Sleep, TRUE);
    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    while(hlnode->node.ln_Succ)
    {
        if(hlnode->phw)
        {
            psdRemHardware(hlnode->phw);
            hlnode->phw = NULL;
            hlnode->prodname = NULL;
        }
        hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
    }
    DoMethod(obj, MUIM_Action_HW_Activate);
    DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    set(data->appobj, MUIA_Application_Sleep, FALSE);
    // update the online status to the config
    // NOTE! This means that the hardware will not come online on next loading.
    InternalCreateConfigGUI(data);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Restart()" */
IPTR Action_Restart(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct HWListEntry  *hlnode;
    set(data->appobj, MUIA_Application_Sleep, TRUE);
    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    while(hlnode->node.ln_Succ)
    {
        if(hlnode->phw)
        {
            psdRemHardware(hlnode->phw);
            hlnode->phw = NULL;
            hlnode->prodname = NULL;
        }
        hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
    }
    hlnode = (struct HWListEntry *) data->hwlist.lh_Head;
    while(hlnode->node.ln_Succ)
    {
        if(!hlnode->phw)
        {
            if((hlnode->phw = psdAddHardware(hlnode->devname, hlnode->unit)))
            {
                psdGetAttrs(PGA_HARDWARE, hlnode->phw,
                            HA_ProductName, &hlnode->prodname,
                            TAG_END);
                psdEnumerateHardware(hlnode->phw);
            }
        }
        hlnode = (struct HWListEntry *) hlnode->node.ln_Succ;
    }
    DoMethod(obj, MUIM_Action_HW_Activate);
    DoMethod(data->hwlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    psdClassScan();
    set(data->appobj, MUIA_Application_Sleep, FALSE);
    return(TRUE);
}
/* \\\ */

/* /// "Action_ChgErrLevel()" */
IPTR Action_ChgErrLevel(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    IPTR lev = 0;
    get(data->errlvlobj, MUIA_Cycle_Active, &lev);
    switch(lev)
    {
        case 0:
            data->errorlevel = RETURN_OK;
            break;
        case 1:
            data->errorlevel = RETURN_WARN;
            break;
        case 2:
            data->errorlevel = RETURN_ERROR;
            break;
        case 3:
            data->errorlevel = RETURN_FAIL;
            break;
    }
    CreateErrorList(data);
    return(TRUE);
}
/* \\\ */

/* /// "WriteErrorLogFile()" */
void WriteErrorLogFile(BPTR fh)
{
    struct List *lst;
    struct Node *pem;
    IPTR level;
    STRPTR origin;
    STRPTR errstr;
    struct DateStamp *ds;
    struct DateTime dt;
    struct DateStamp currdate;
    UBYTE strdate[LEN_DATSTRING];
    UBYTE strtime[LEN_DATSTRING];

    DateStamp(&currdate);
    psdLockReadPBase();
    psdGetAttrs(PGA_STACK, NULL, PA_ErrorMsgList, &lst, TAG_END);
    pem = lst->lh_Head;
    while(pem->ln_Succ)
    {
        ds = NULL;
        psdGetAttrs(PGA_ERRORMSG, pem,
                    EMA_Level, &level,
                    EMA_Origin, &origin,
                    EMA_Msg, &errstr,
                    EMA_DateStamp, &ds,
                    TAG_END);
        if(ds)
        {
            dt.dat_Stamp.ds_Days = ds->ds_Days;
            dt.dat_Stamp.ds_Minute = ds->ds_Minute;
            dt.dat_Stamp.ds_Tick = ds->ds_Tick;
            dt.dat_Format = FORMAT_DEF;
            dt.dat_Flags = 0;
            dt.dat_StrDay = NULL;
            dt.dat_StrDate = strdate;
            dt.dat_StrTime = strtime;
            DateToStr(&dt);
            if(currdate.ds_Days == ds->ds_Days)
            {
                FPrintf(fh, "%s| %2ld-%s: %s\n", strtime, level, origin, errstr);
            } else {
                FPrintf(fh, "%s %s| %2ld-%s: %s\n", strdate, strtime, level, origin, errstr);
            }
        } else {
            FPrintf(fh, "%2ld-%s: %s\n", level, origin, errstr);
        }
        pem = pem->ln_Succ;
    }
    psdUnlockPBase();
}
/* \\\ */

/* /// "Action_SaveErrors()" */
IPTR Action_SaveErrors(struct IClass *cl, Object *obj, Msg msg)
{
    struct FileRequester *aslreq;
    char   path[256];
    struct TagItem asltags[] = { { ASLFR_InitialDrawer, (IPTR) "RAM:" },
                                 { ASLFR_InitialFile, __(MSG_ACTION_SAVE_ERR_FILE) },
                                 { ASLFR_DoSaveMode, (IPTR) TRUE },
                                 { ASLFR_TitleText, __(MSG_ACTION_SAVE_ERR) },
                                 { TAG_END, (IPTR) NULL } };
    BPTR fh;

    if((aslreq = MUI_AllocAslRequest(ASL_FileRequest, asltags)))
    {
        if(MUI_AslRequest(aslreq, TAG_END))
        {
            strncpy(path, aslreq->fr_Drawer, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            AddPart(path, aslreq->fr_File, 256);
            if((fh = Open(path, MODE_NEWFILE)))
            {
                WriteErrorLogFile(fh);
                Close(fh);
                psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_ERR_SAVED), path);
            } else {
                psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_ERR_SAVED_ERRWRITE), path);
            }
            MUI_FreeAslRequest(aslreq);
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_FlushErrors()" */
IPTR Action_FlushErrors(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct List *lst;
    psdGetAttrs(PGA_STACK, NULL, PA_ErrorMsgList, &lst, TAG_END);
    Forbid();
    while(lst->lh_Head->ln_Succ)
    {
        psdRemErrorMsg(lst->lh_Head);
    }
    Permit();
    CreateErrorList(data);
    return(TRUE);
}
/* \\\ */

/* /// "Action_SaveDeviceList()" */
IPTR Action_SaveDeviceList(struct IClass *cl, Object *obj, Msg msg)
{
    struct FileRequester *aslreq;
    char   path[256];
    struct TagItem asltags[] = { { ASLFR_InitialDrawer, (IPTR) "RAM:" },
                                 { ASLFR_InitialFile, __(MSG_ACTION_SAVE_DEV_FILE) },
                                 { ASLFR_DoSaveMode, (IPTR) TRUE },
                                 { ASLFR_TitleText, __(MSG_ACTION_SAVE_DEV) },
                                 { TAG_END, (IPTR) NULL } };
    BPTR fh;

    if((aslreq = MUI_AllocAslRequest(ASL_FileRequest, asltags)))
    {
        if(MUI_AslRequest(aslreq, TAG_END))
        {
            strncpy(path, aslreq->fr_Drawer, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            AddPart(path, aslreq->fr_File, 256);
            if((fh = Open(path, MODE_NEWFILE)))
            {
                if(SystemTags("USBList", SYS_Output, fh, TAG_END))
                {
                    psdAddErrorMsg(RETURN_ERROR,_(MSG_APP_TITLE), _(MSG_ACTION_SAVE_DEV_ERREXE));
                } else {
                    psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_DEV_SAVED), path);
                }
                Close(fh);
            } else {
                psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_ERR_SAVED_ERRWRITE), path);
            }
            MUI_FreeAslRequest(aslreq);
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_LoadPrefs()" */
IPTR Action_LoadPrefs(struct IClass *cl, Object *obj, Msg msg)
{
    //struct ActionData *data = INST_DATA(cl, obj);
    struct FileRequester *aslreq;
    char   path[256];
    struct TagItem asltags[] = { { ASLFR_InitialDrawer, (IPTR) "ENVARC:Sys" },
                                 { ASLFR_InitialFile, (IPTR) "Neptune.prefs" },
                                 { ASLFR_TitleText, __(MSG_ACTION_PREFS_LOAD) },
                                 { TAG_END, (IPTR) NULL } };

    if((aslreq = MUI_AllocAslRequest(ASL_FileRequest, asltags)))
    {
        if(MUI_AslRequest(aslreq, TAG_END))
        {
            strncpy(path, aslreq->fr_Drawer, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            AddPart(path, aslreq->fr_File, 256);
            if(psdLoadCfgFromDisk(path))
            {
                psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_PREFS_LOADED), path);
                //UpdateConfigToGUI(data);
            }
        }
        MUI_FreeAslRequest(aslreq);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_SavePrefsAs()" */
IPTR Action_SavePrefsAs(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct FileRequester *aslreq;
    char   path[256];
    struct TagItem asltags[] = { { ASLFR_InitialDrawer, (IPTR) "ENVARC:Sys" },
                                 { ASLFR_InitialFile, (IPTR) "Neptune.prefs" },
                                 { ASLFR_TitleText, __(MSG_ACTION_PREFS_SAVEAS) },
                                 { ASLFR_DoSaveMode, (IPTR) TRUE },
                                 { TAG_END, (IPTR) NULL } };

    if((aslreq = MUI_AllocAslRequest(ASL_FileRequest, asltags)))
    {
        if(MUI_AslRequest(aslreq, TAG_END))
        {
            strncpy(path, aslreq->fr_Drawer, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            AddPart(path, aslreq->fr_File, 256);
            InternalCreateConfigGUI(data);
            if(psdSaveCfgToDisk(path))
            {
                psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_PREFS_SAVED), path);
                {
                    IPTR hash = 0;
                    psdGetAttrs(PGA_STACK, NULL, PA_CurrConfigHash, &hash, TAG_END);
                    psdSetAttrs(PGA_STACK, NULL, PA_SavedConfigHash, hash, TAG_END);
                }
            } else {
                psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_PREFS_SAVEDERR), path);
            }
            MUI_FreeAslRequest(aslreq);
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_SavePrefs()" */
IPTR Action_SavePrefs(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);

    DoMethod(obj, MUIM_Action_Cfg_Snd_Changed);
    InternalCreateConfigGUI(data);

    if(!psdSaveCfgToDisk(NULL))
    {
        psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_PREFS_NOTSAVED));
    } else {
        psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_PREFS_SAVEDSUCCESS));
    }

    return(TRUE);
}
/* \\\ */

/* /// "Action_Prefs_Changed()" */
IPTR Action_Prefs_Changed(struct IClass *cl, Object *obj, Msg msg)
{
    IPTR oldhash = 0;
    IPTR currhash = 0;
    DoMethod(obj, MUIM_Action_Cfg_Snd_Changed);
    psdGetAttrs(PGA_STACK, NULL,
                PA_CurrConfigHash, &currhash,
                PA_SavedConfigHash, &oldhash,
                TAG_END);
    return((IPTR) (currhash != oldhash));
}
/* \\\ */

/* /// "Action_UseQuit()" */
IPTR Action_UseQuit(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    LONG res;
    //DoMethod(obj, MUIM_Action_Cfg_Snd_Changed);
    DoMethod(obj, MUIM_Action_Use);
    if(DoMethod(obj, MUIM_Action_Prefs_Changed))
    {
        res = MUI_RequestA(data->appobj, data->winobj, 0, NULL, _(MSG_ACTION_USEQUIT), _(MSG_ACTION_CONFIGCHANGED), NULL);
        if(res == 0)
        {
            return(FALSE);
        }
        if(res == 2)
        {
            DoMethod(obj, MUIM_Action_SavePrefs);
        }
    }
    DoMethod(data->appobj, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    return(TRUE);
}
/* \\\ */

/* /// "Action_SaveQuit()" */
IPTR Action_SaveQuit(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    DoMethod(obj, MUIM_Action_Cfg_Snd_Changed);
    DoMethod(obj, MUIM_Action_Use);
    if(!(psdSaveCfgToDisk(NULL)))
    {
        psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_SAVEQUIT));
    } else {
        DoMethod(data->appobj, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_LoadPrefsFrom()" */
IPTR Action_LoadPrefsFrom(struct IClass *cl, Object *obj, Msg msg)
{
    //struct ActionData *data = INST_DATA(cl, obj);
    STRPTR path = (STRPTR) ((struct opSet *) msg)->ops_AttrList;
    if(psdLoadCfgFromDisk(path))
    {
        psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_PREFS_LOADED), path);
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "Action_SavePrefsTo()" */
IPTR Action_SavePrefsTo(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    STRPTR path = (STRPTR) ((struct opSet *) msg)->ops_AttrList;
    InternalCreateConfigGUI(data);
    if(psdSaveCfgToDisk(path))
    {
        psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_PREFS_SAVED), path);
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "Action_Dev_Activate()" */
IPTR Action_Dev_Activate(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;
    APTR binding;
    APTR cbind;
    struct List *pclist;
    struct List *piflist;
    struct Node *pc;
    struct Node *pif;
    struct Node *puc;
    IPTR hascfggui = FALSE;
    IPTR issuspended = FALSE;
    IPTR caneject = FALSE;
    struct Library *UsbClsBase;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        psdLockReadDevice(dlnode->pd);
        set(data->devinfoobj, MUIA_Disabled, FALSE);
        IPTR cansuspend = TRUE;     /* kept by a library that predates DA_CanSuspend */
        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_Binding, &binding,
                    DA_BindingClass, &puc,
                    DA_ConfigList, &pclist,
                    DA_IsSuspended, &issuspended,
                    DA_CanSafeEject, &caneject,
                    DA_CanSuspend, &cansuspend,
                    TAG_END);
        if(binding && puc)
        {
            psdGetAttrs(PGA_USBCLASS, puc,
                UCA_ClassBase, &UsbClsBase,
                TAG_END);
            usbGetAttrs(UGA_CLASS, NULL,
                UCCA_HasBindingCfgGUI, &hascfggui,
                TAG_END);
        }
        pc = pclist->lh_Head;
        while((!binding) && pc->ln_Succ)
        {
            psdGetAttrs(PGA_CONFIG, pc,
                        CA_InterfaceList, &piflist,
                        TAG_END);
            pif = piflist->lh_Head;
            while(pif->ln_Succ)
            {
                psdGetAttrs(PGA_INTERFACE, pif,
                            IFA_Binding, &cbind,
                            IFA_BindingClass, &puc,
                            TAG_END);
                if(cbind)
                {
                    binding = cbind;
                }
                if(cbind && puc && !hascfggui)
                {
                    psdGetAttrs(PGA_USBCLASS, puc,
                        UCA_ClassBase, &UsbClsBase,
                        TAG_END);
                    usbGetAttrs(UGA_CLASS, NULL,
                        UCCA_HasBindingCfgGUI, &hascfggui,
                        TAG_END);
                }
                pif = pif->ln_Succ;
            }
            pc = pc->ln_Succ;
        }
        psdUnlockDevice(dlnode->pd);
        set(data->devunbindobj, MUIA_Disabled, !binding);
        set(data->devcfgobj, MUIA_Disabled, !hascfggui);
        /*if(dlnode->infowindow) FIXME
        {
             DoMethod(obj, MUIM_Action_Dev_If_Activate, dlnode);
        }*/
        set(data->devsuspendpageobj, MUIA_Group_ActivePage, issuspended ? 1 : 0);
        set(data->devsuspendobj, MUIA_Disabled, !cansuspend);
        set(data->devresumeobj, MUIA_Disabled, FALSE);
        set(data->devpowercycleobj, MUIA_Disabled, FALSE);
        set(data->devdisableobj, MUIA_Disabled, FALSE);
        set(data->devejectobj, MUIA_Disabled, !(caneject && !issuspended));
        set(data->devlistobj, MUIA_ContextMenu, data->mi_classpopup);
    } else {
        set(data->devunbindobj, MUIA_Disabled, TRUE);
        set(data->devinfoobj, MUIA_Disabled, TRUE);
        set(data->devcfgobj, MUIA_Disabled, TRUE);
        set(data->devsuspendobj, MUIA_Disabled, TRUE);
        set(data->devresumeobj, MUIA_Disabled, TRUE);
        set(data->devpowercycleobj, MUIA_Disabled, TRUE);
        set(data->devdisableobj, MUIA_Disabled, TRUE);
        set(data->devejectobj, MUIA_Disabled, TRUE);
        set(data->devlistobj, MUIA_ContextMenu, NULL);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Info()" */
IPTR Action_Dev_Info(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(dlnode)
    {
        if(dlnode->infowindow)
        {
            return(TRUE);
        }
        dlnode->infowindow = NewObject(DevWinClass->mcc_Class, 0, MUIA_DevWin_DevEntry, dlnode, WindowContents, VGroup, End, TAG_END);
        if(dlnode->infowindow)
        {
            DoMethod(data->appobj, OM_ADDMEMBER, dlnode->infowindow);
            DoMethod(dlnode->infowindow, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
                     data->appobj, 5, MUIM_Application_PushMethod, obj, 2, MUIM_Action_CloseSubWinReq, dlnode);
            set(dlnode->infowindow, MUIA_Window_Open, TRUE);
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Bind()" */
IPTR Action_Dev_Bind(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    psdClassScan();
    DoMethod(obj, MUIM_Action_Dev_Activate);
    DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Unbind()" */
IPTR Action_Dev_Unbind(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;
    APTR binding;
    struct List *pclist;
    struct List *piflist;
    struct Node *pc;
    struct Node *pif;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_Binding, &binding,
                    DA_ConfigList, &pclist,
                    TAG_END);
        if(binding)
        {
            psdReleaseDevBinding(dlnode->pd);
        } else {
            pc = pclist->lh_Head;
            while(pc->ln_Succ)
            {
                psdGetAttrs(PGA_CONFIG, pc,
                            CA_InterfaceList, &piflist,
                            TAG_END);
                pif = piflist->lh_Head;
                while(pif->ln_Succ)
                {
                    psdGetAttrs(PGA_INTERFACE, pif,
                                IFA_Binding, &binding,
                                TAG_END);
                    if(binding)
                    {
                        psdReleaseIfBinding(pif);
                    }
                    pif = pif->ln_Succ;
                }
                pc = pc->ln_Succ;
            }
        }
        set(data->devunbindobj, MUIA_Disabled, TRUE);
        set(data->devcfgobj, MUIA_Disabled, TRUE);
        /*if(dlnode->infowindow) FIXME
        {
            DoMethod(obj, MUIM_Action_Dev_If_Activate, dlnode);
            DoMethod(dlnode->iflvobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
        }*/
    }
    DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Suspend()" */
IPTR Action_Dev_Suspend(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        /* Blocking control transfers, on the MUI task - and for a hub, one set
           per downstream device.  Sleep the application so the user gets a busy
           pointer instead of an apparently live but frozen window. */
        set(data->appobj, MUIA_Application_Sleep, TRUE);
        psdSuspendDevice(dlnode->pd);
        set(data->appobj, MUIA_Application_Sleep, FALSE);
        /* the library logs why a refusal happened; re-reading DA_IsSuspended is
           what keeps the Suspend/Resume page from lying about it afterwards */
        DoMethod(obj, MUIM_Action_Dev_Activate);
    }
    DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Resume()" */
IPTR Action_Dev_Resume(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        /* see Action_Dev_Suspend() on why this sleeps the application */
        set(data->appobj, MUIA_Application_Sleep, TRUE);
        psdResumeDevice(dlnode->pd);
        set(data->appobj, MUIA_Application_Sleep, FALSE);
        DoMethod(obj, MUIM_Action_Dev_Activate);
    }
    DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_PowerCycle()" */
IPTR Action_Dev_PowerCycle(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        /* psdDoHubMethod resolves the parent hub's class base itself */
        struct Node *hubpd = NULL;
        IPTR hubport = 0;
        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_HubDevice, &hubpd,
                    DA_AtHubPortNumber, &hubport,
                    TAG_END);
        if(hubpd)
        {
            psdDoHubMethod(dlnode->pd, UCM_HubPowerCyclePort, hubpd, hubport);
        }
    }
    DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Disable()" */
IPTR Action_Dev_Disable(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        /* psdDoHubMethod resolves the parent hub's class base itself */
        struct Node *hubpd = NULL;
        IPTR hubport = 0;
        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_HubDevice, &hubpd,
                    DA_AtHubPortNumber, &hubport,
                    TAG_END);
        if(hubpd)
        {
            psdDoHubMethod(dlnode->pd, UCM_HubDisablePort, hubpd, hubport);
        }
    }
    DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Eject()" */
IPTR Action_Dev_Eject(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        STRPTR prodname = NULL;
        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_ProductName, &prodname,
                    TAG_END);
        char devname[64];
        psdSafeRawDoFmt(devname, sizeof(devname), "%s",
                        (prodname && *prodname) ? prodname : (STRPTR) "USB device");
        char busyname[64];
        busyname[0] = 0;
        /* see Action_Dev_Suspend() on why this sleeps the application */
        set(data->appobj, MUIA_Application_Sleep, TRUE);
        IPTR res = psdSafeEjectDevice(dlnode->pd, busyname, sizeof(busyname));
        set(data->appobj, MUIA_Application_Sleep, FALSE);
        if(res == SAFEEJECT_BUSY)
        {
            MUI_Request(data->appobj, data->winobj, 0, NULL,
                        _(MSG_ACTION_DEV_EJECT_OK),
                        _(MSG_ACTION_DEV_EJECT_BUSY_TXT),
                        devname, busyname);
        }
        else if(res != SAFEEJECT_OK)
        {
            MUI_Request(data->appobj, data->winobj, 0, NULL,
                        _(MSG_ACTION_DEV_EJECT_OK),
                        _(MSG_ACTION_DEV_EJECT_FAILED_TXT),
                        devname);
        }
        /* on success the port is going down; the device vanishing from the
           list (EHMB_REMDEVICE refresh) is the feedback */
    }
    DoMethod(data->devlistobj, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Dev_Configure()" */
IPTR Action_Dev_Configure(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;
    APTR binding;
    struct List *pclist;
    struct List *piflist;
    struct Node *pc;
    struct Node *pif;
    struct Node *puc;
    struct Library *UsbClsBase;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        psdLockReadDevice(dlnode->pd);
        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_Binding, &binding,
                    DA_BindingClass, &puc,
                    DA_ConfigList, &pclist,
                    TAG_END);
        UsbClsBase = NULL;
        if(puc)
        {
             psdGetAttrs(PGA_USBCLASS, puc,
                         UCA_ClassBase, &UsbClsBase,
                         TAG_END);
        }
        if(binding && UsbClsBase)
        {
            usbDoMethod(UCM_OpenBindingCfgWindow, binding);
        } else {
            pc = pclist->lh_Head;
            while(pc->ln_Succ)
            {
                psdGetAttrs(PGA_CONFIG, pc,
                            CA_InterfaceList, &piflist,
                            TAG_END);
                pif = piflist->lh_Head;
                while(pif->ln_Succ)
                {
                    psdGetAttrs(PGA_INTERFACE, pif,
                                IFA_Binding, &binding,
                                IFA_BindingClass, &puc,
                                TAG_END);
                    UsbClsBase = NULL;
                    if(puc)
                    {
                        psdGetAttrs(PGA_USBCLASS, puc,
                                    UCA_ClassBase, &UsbClsBase,
                                    TAG_END);
                    }
                    if(binding && UsbClsBase)
                    {
                        usbDoMethod(UCM_OpenBindingCfgWindow, binding);
                    }
                    pif = pif->ln_Succ;
                }
                pc = pc->ln_Succ;
            }
        }
        psdUnlockDevice(dlnode->pd);
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "Action_Dev_ForceBind()" */
IPTR Action_Dev_ForceBind(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct DevListEntry *dlnode;
    Object *mi = (Object *) ((IPTR *) msg)[1];
    STRPTR name = NULL;
    STRPTR devid = NULL;
    STRPTR devname = NULL;
    LONG clever;

    DoMethod(data->devlistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &dlnode);
    if(CheckDeviceValid(dlnode))
    {
        get(mi, MUIA_Menuitem_Title, &name);
        if(!strcmp(name, _(MSG_ACTION_DEV_FORCEBIND_NONE)))
        {
            name = NULL;
        }
        psdGetAttrs(PGA_DEVICE, dlnode->pd,
                    DA_ProductName, &devname,
                    DA_IDString, &devid,
                    TAG_END);
        if(name)
        {
            clever = MUI_RequestA(data->appobj, data->winobj, 0, NULL, _(MSG_ACTION_DEV_FORCE_REQ),
                                  _(MSG_ACTION_DEV_FORCE_REQ_TXT), NULL);
            if(!clever)
            {
                return(FALSE);
            }
        }
        if(psdSetForcedBinding(name, devid, NULL))
        {
            if(name)
            {
                psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_DEV_FORCE), devname, name);
            } else {
                psdAddErrorMsg(RETURN_OK, _(MSG_APP_TITLE), _(MSG_ACTION_DEV_FORCE_REMOVE), devname);
            }
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cfg_Changed()" */
IPTR Action_Cfg_Changed(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    IPTR bootdelay = 0;
    IPTR subtaskpri = 0;
    IPTR loginfo = 0;
    IPTR logwarn = 0;
    IPTR logerr = 0;
    IPTR logfail = 0;
    IPTR popupnew = 0;
    IPTR popupgone = 0;
    IPTR popupdeath = 0;
    IPTR popupdelay = 0;
    IPTR popupactivate = 0;
    IPTR popuptofront = 0;
    IPTR autodisablelp = 0;
    IPTR autodisabledead = 0;
    IPTR autorestartdead = 0;
    IPTR powersaving = 0;
    IPTR forcesuspend = 0;
    IPTR suspendtimeout = 0;
    IPTR linkpower = 0;
    APTR stackcfg = NULL;

    psdGetAttrs(PGA_STACK, NULL, PA_GlobalConfig, &stackcfg, TAG_END);
    get(data->cfgtaskpriobj, MUIA_Numeric_Value, &subtaskpri);
    get(data->cfgbootdelayobj, MUIA_Numeric_Value, &bootdelay);
    get(data->cfgloginfoobj, MUIA_Selected, &loginfo);
    get(data->cfglogwarnobj, MUIA_Selected, &logwarn);
    get(data->cfglogerrobj, MUIA_Selected, &logerr);
    get(data->cfglogfailobj, MUIA_Selected, &logfail);
    get(data->cfgpopupnewobj, MUIA_Cycle_Active, &popupnew);
    get(data->cfgpopupgoneobj, MUIA_Selected, &popupgone);
    get(data->cfgpopupdeathobj, MUIA_Selected, &popupdeath);
    get(data->cfgpopupdelayobj, MUIA_Numeric_Value, &popupdelay);
    get(data->cfgpopupactivateobj, MUIA_Selected, &popupactivate);
    get(data->cfgpopuptofrontobj, MUIA_Selected, &popuptofront);
    get(data->cfgautolpobj, MUIA_Selected, &autodisablelp);
    get(data->cfgautodeadobj, MUIA_Selected, &autodisabledead);
    get(data->cfgautopcobj, MUIA_Selected, &autorestartdead);
    get(data->cfgpowersavingobj, MUIA_Selected, &powersaving);
    get(data->cfgforcesuspendobj, MUIA_Selected, &forcesuspend);
    get(data->cfgsuspendtimeoutobj, MUIA_Numeric_Value, &suspendtimeout);
    get(data->cfglinkpowerobj, MUIA_Selected, &linkpower);

    if(autorestartdead && autodisabledead)
    {
        autodisabledead = FALSE;
        nnset(data->cfgautodeadobj, MUIA_Selected, FALSE);
    }
    if(autorestartdead)
    {
        nnset(data->cfgautodeadobj, MUIA_Disabled, TRUE);
    } else {
        nnset(data->cfgautodeadobj, MUIA_Disabled, FALSE);
    }
    if(stackcfg)
    {
        psdSetAttrs(PGA_STACKCFG, stackcfg,
                    GCA_SubTaskPri, subtaskpri,
                    GCA_BootDelay, bootdelay,
                    GCA_LogInfo, loginfo,
                    GCA_LogWarning, logwarn,
                    GCA_LogError, logerr,
                    GCA_LogFailure, logfail,
                    GCA_PopupDeviceNew, popupnew,
                    GCA_PopupDeviceGone, popupgone,
                    GCA_PopupDeviceDeath, popupdeath,
                    GCA_PopupCloseDelay, popupdelay,
                    GCA_PopupActivateWin, popupactivate,
                    GCA_PopupWinToFront, popuptofront,
                    GCA_AutoDisableLP, autodisablelp,
                    GCA_AutoDisableDead, autodisabledead,
                    GCA_AutoRestartDead, autorestartdead,
                    GCA_PowerSaving, powersaving,
                    GCA_ForceSuspend, forcesuspend,
                    GCA_SuspendTimeout, suspendtimeout,
                    GCA_LinkPowerMgmt, linkpower,
                    TAG_END);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cfg_Snd_Changed()" */
IPTR Action_Cfg_Snd_Changed(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    STRPTR dtxsndfile = "";
    STRPTR remsndfile = "";
    APTR stackcfg = NULL;

    psdGetAttrs(PGA_STACK, NULL, PA_GlobalConfig, &stackcfg, TAG_END);
    get(data->cfgdevdtxsoundobj, MUIA_String_Contents, &dtxsndfile);
    get(data->cfgdevremsoundobj, MUIA_String_Contents, &remsndfile);
    if(stackcfg)
    {
        psdSetAttrs(PGA_STACKCFG, stackcfg,
                    GCA_InsertionSound, dtxsndfile,
                    GCA_RemovalSound, remsndfile,
                    TAG_END);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cls_Activate()" */
IPTR Action_Cls_Activate(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct ClsListEntry *clnode;
    DoMethod(data->clslistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &clnode);
    if(clnode)
    {
        struct Node *puc;
        struct List *lst;
        IPTR hascfggui = FALSE;
        struct Library *UsbClsBase;

        set(data->clsremobj, MUIA_Disabled, FALSE);

        psdLockReadPBase();
        psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &lst, TAG_END);
        puc = lst->lh_Head;
        while(puc->ln_Succ)
        {
            if(puc == clnode->puc)
            {
                psdGetAttrs(PGA_USBCLASS, clnode->puc,
                            UCA_ClassBase, &UsbClsBase,
                            TAG_END);
                usbGetAttrs(UGA_CLASS, NULL,
                            UCCA_HasClassCfgGUI, &hascfggui,
                            TAG_END);
                set(data->clscfgobj, MUIA_Disabled, !hascfggui);
                break;
            }
            puc = puc->ln_Succ;
        }
        psdUnlockPBase();
    } else {
        set(data->clsremobj, MUIA_Disabled, TRUE);
        set(data->clscfgobj, MUIA_Disabled, TRUE);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cls_Add()" */
IPTR Action_Cls_Add(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    STRPTR clsname = "";
    get(data->clsnameobj, MUIA_String_Contents, &clsname);
    psdAddClass(clsname, 0);
    InternalCreateConfigGUI(data);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cls_Remove()" */
IPTR Action_Cls_Remove(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct ClsListEntry *clnode;
    DoMethod(data->clslistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &clnode);
    if(clnode)
    {
        struct Node *puc;
        struct List *lst;
        psdLockReadPBase();
        psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &lst, TAG_END);
        puc = lst->lh_Head;
        while(puc->ln_Succ)
        {
            if(puc == clnode->puc)
            {
                clnode->puc = NULL;
                DoMethod(data->clslistobj, MUIM_List_Remove, MUIV_List_Remove_Active);
                FreeClsEntry(data, clnode);
                psdUnlockPBase();
                psdRemClass(puc);
                puc = NULL;
                break;
            }
            puc = puc->ln_Succ;
        }
        if(puc)
        {
            psdUnlockPBase();
        }
    }
    InternalCreateConfigGUI(data);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cls_Scan()" */
IPTR Action_Cls_Scan(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);

    if(psdAddClassDir(ps, FALSE))
    {
        InternalCreateConfigGUI(data);
        psdClassScan();
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cls_Configure()" */
IPTR Action_Cls_Configure(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct ClsListEntry *clnode;
    DoMethod(data->clslistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &clnode);
    if(clnode)
    {
        struct Node *puc;
        struct List *lst;
        struct Library *UsbClsBase;

        psdLockReadPBase();
        psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &lst, TAG_END);
        puc = lst->lh_Head;
        while(puc->ln_Succ)
        {
            if(puc == clnode->puc)
            {
                psdGetAttrs(PGA_USBCLASS, clnode->puc,
                            UCA_ClassBase, &UsbClsBase,
                            TAG_END);
                usbDoMethod(UCM_OpenCfgWindow);
                break;
            }
            puc = puc->ln_Succ;
        }
        psdUnlockPBase();

        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "Action_Info_MemPool()" */
IPTR Action_Info_MemPool(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    char buf[32];
    IPTR mem;
    psdGetAttrs(PGA_STACK, NULL, PA_MemPoolUsage, &mem, TAG_END);
    psdSafeRawDoFmt(buf, 32, _(MSG_ACTION_MEMPOOL_FORMAT), (mem+512)>>10);
    set(data->mempoolobj, MUIA_Text_Contents, buf);
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cfg_Activate()" */
IPTR Action_Cfg_Activate(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct PrefsListEntry *plnode;
    DoMethod(data->prefslistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &plnode);
    if(plnode)
    {
        BOOL noexport = FALSE;
        switch(plnode->chunkid)
        {
            case IFFCHNK_FORCEDBIND:
            case MAKE_ID('P','S','D','L'):
                noexport = TRUE;
        }

        set(data->prefsremoveobj, MUIA_Disabled, plnode->chunkid == IFFFORM_STACKCFG);
        set(data->prefsexportobj, MUIA_Disabled, noexport);
    } else {
        set(data->prefsremoveobj, MUIA_Disabled, TRUE);
        set(data->prefsexportobj, MUIA_Disabled, TRUE);
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cfg_Remove()" */
IPTR Action_Cfg_Remove(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct PrefsListEntry *plnode;
    DoMethod(data->prefslistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &plnode);
    if(plnode)
    {
        LONG result;
        APTR pic;
        switch(plnode->chunkid)
        {
            case IFFFORM_STACKCFG:
                break;

            case IFFFORM_DEVICECFG:
                result = MUI_RequestA(data->appobj, data->winobj, 0, NULL, _(MSG_ACTION_CFG_REMOVE),_(MSG_ACTION_CFG_REMOVE_HELP), &plnode->devid);
                if(result)
                {
                    pic = psdFindCfgForm(NULL, plnode->chunkid);
                    while(pic)
                    {
                        if(psdMatchStringChunk(pic, IFFCHNK_DEVID, plnode->devid))
                        {
                            psdRemCfgForm(pic);
                            break;
                        }
                        pic = psdNextCfgForm(pic);
                    }
                }
                break;

            case IFFFORM_CLASSCFG:
                result = MUI_RequestA(data->appobj, data->winobj, 0, NULL, _(MSG_ACTION_CFG_REMOVE),_(MSG_ACTION_CFG_REMOVE_DEFAULT), &plnode->owner);
                if(result)
                {
                    pic = psdFindCfgForm(NULL, IFFFORM_CLASSCFG);
                    while(pic)
                    {
                        if(psdMatchStringChunk(pic, IFFCHNK_OWNER, plnode->owner))
                        {
                            psdRemCfgForm(pic);
                            break;
                        }
                        pic = psdNextCfgForm(pic);
                    }
                }
                break;

            case IFFFORM_DEVCFGDATA:
                result = MUI_Request(data->appobj, data->winobj, 0, NULL, _(MSG_ACTION_CFG_REMOVE),_(MSG_ACTION_CFG_REMOVE_CLASS), plnode->owner, plnode->devid);
                if(result)
                {
                    pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
                    while(pic)
                    {
                        if(psdMatchStringChunk(pic, IFFCHNK_DEVID, plnode->devid))
                        {
                            pic = psdFindCfgForm(pic, plnode->chunkid);
                            while(pic)
                            {
                                if(psdMatchStringChunk(pic, IFFCHNK_OWNER, plnode->owner))
                                {
                                    psdRemCfgForm(pic);
                                    break;
                                }
                                pic = psdNextCfgForm(pic);
                            }
                            break;
                        }
                        pic = psdNextCfgForm(pic);
                    }
                }
                break;

            case IFFFORM_IFCFGDATA:
                result = MUI_Request(data->appobj, data->winobj, 0, NULL, _(MSG_ACTION_CFG_REMOVE),_(MSG_ACTION_CFG_REMOVE_CLASS_DEF),
                                     plnode->owner, plnode->ifid, plnode->devid);
                if(result)
                {
                    pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
                    while(pic)
                    {
                        if(psdMatchStringChunk(pic, IFFCHNK_DEVID, plnode->devid))
                        {
                            pic = psdFindCfgForm(pic, plnode->chunkid);
                            while(pic)
                            {
                                if(psdMatchStringChunk(pic, IFFCHNK_IFID, plnode->ifid))
                                {
                                    if(psdMatchStringChunk(pic, IFFCHNK_OWNER, plnode->owner))
                                    {
                                        psdRemCfgForm(pic);
                                        break;
                                    }
                                }
                                pic = psdNextCfgForm(pic);
                            }
                            break;
                        }
                        pic = psdNextCfgForm(pic);
                    }
                }
                break;

            case IFFCHNK_FORCEDBIND:
                psdSetForcedBinding(NULL, plnode->devid, plnode->ifid);
                break;

            default:
                if(plnode->chunkid)
                {
                    result = MUI_RequestA(data->appobj, data->winobj, 0, NULL, _(MSG_ACTION_CFG_REMOVE),_(MSG_ACTION_CFG_REMOVE_UNKNOWN), NULL);
                    if(result)
                    {
                        pic = psdFindCfgForm(NULL, plnode->chunkid);
                        if(pic)
                        {
                            psdRemCfgForm(pic);
                        }
                    }
                }
                break;
        }
    }
    return(TRUE);
}
/* \\\ */

/* /// "Action_Cfg_Export()" */
IPTR Action_Cfg_Export(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct PrefsListEntry *plnode;
    struct FileRequester *aslreq;
    char   path[256];
    struct TagItem asltags[] = { { ASLFR_InitialFile, __(MSG_ACTION_CFG_PREFS_FILE) },
                                 { ASLFR_InitialDrawer, (IPTR) NEP_PRESETDRAWER },
                                 { ASLFR_TitleText, __(MSG_ACTION_CFG_EXPORT) },
                                 { ASLFR_DoSaveMode, (IPTR) TRUE },
                                 { TAG_END, (IPTR) NULL } };
    BPTR fh;
    ULONG *form;

    DoMethod(data->prefslistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &plnode);
    if(plnode)
    {
        APTR pic = NULL;
        switch(plnode->chunkid)
        {
            case IFFFORM_STACKCFG:
                asltags[0].ti_Data = __(MSG_ACTION_STACKCFG_FILE);
                pic = psdFindCfgForm(NULL, plnode->chunkid);
                break;

            case IFFFORM_DEVICECFG:
                asltags[0].ti_Data = __(MSG_ACTION_DEVICECFG_FILE);
                pic = psdFindCfgForm(NULL, plnode->chunkid);
                while(pic)
                {
                    if(psdMatchStringChunk(pic, IFFCHNK_DEVID, plnode->devid))
                    {
                        break;
                    }
                    pic = psdNextCfgForm(pic);
                }
                break;

            case IFFFORM_CLASSCFG:
                asltags[0].ti_Data = __(MSG_ACTION_CLASSCFG_FILE);
                pic = psdFindCfgForm(NULL, plnode->chunkid);
                while(pic)
                {
                    if(psdMatchStringChunk(pic, IFFCHNK_OWNER, plnode->owner))
                    {
                        break;
                    }
                    pic = psdNextCfgForm(pic);
                }
                break;

            case IFFFORM_DEVCFGDATA:
                asltags[0].ti_Data = __(MSG_ACTION_DEVCFGDATA_FILE);
                pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
                while(pic)
                {
                    if(psdMatchStringChunk(pic, IFFCHNK_DEVID, plnode->devid))
                    {
                        pic = psdFindCfgForm(pic, plnode->chunkid);
                        while(pic)
                        {
                            if(psdMatchStringChunk(pic, IFFCHNK_OWNER, plnode->owner))
                            {
                                break;
                            }
                            pic = psdNextCfgForm(pic);
                        }
                        break;
                    }
                    pic = psdNextCfgForm(pic);
                }
                break;

            case IFFFORM_IFCFGDATA:
                asltags[0].ti_Data = __(MSG_ACTION_IFCFGDATA_FILE);
                pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
                while(pic)
                {
                    if(psdMatchStringChunk(pic, IFFCHNK_DEVID, plnode->devid))
                    {
                        pic = psdFindCfgForm(pic, plnode->chunkid);
                        while(pic)
                        {
                            if(psdMatchStringChunk(pic, IFFCHNK_IFID, plnode->ifid))
                            {
                                if(psdMatchStringChunk(pic, IFFCHNK_OWNER, plnode->owner))
                                {
                                    break;
                                }
                            }
                            pic = psdNextCfgForm(pic);
                        }
                        break;
                    }
                    pic = psdNextCfgForm(pic);
                }
                break;

            case IFFCHNK_FORCEDBIND:
                break;

            default:
                if(plnode->chunkid)
                {
                    pic = psdFindCfgForm(NULL, plnode->chunkid);
                }
                break;
        }
        if(!pic)
        {
            return(FALSE);
        }
        if((aslreq = MUI_AllocAslRequest(ASL_FileRequest, asltags)))
        {
            if(MUI_AslRequest(aslreq, TAG_END))
            {
                strncpy(path, aslreq->fr_Drawer, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
                AddPart(path, aslreq->fr_File, 256);
                fh = Open(path, MODE_NEWFILE);
                if(fh)
                {
                    form = psdWriteCfg(pic);
                    if(form)
                    {
                        Write(fh, form, form[1]+8);
                        psdFreeVec(form);
                    }
                    Close(fh);
                } else {
                    psdAddErrorMsg(RETURN_FAIL, _(MSG_APP_TITLE), _(MSG_ACTION_CFG_FAIL), path);
                }
            }
            MUI_FreeAslRequest(aslreq);
        }
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "Action_Cfg_Import()" */
IPTR Action_Cfg_Import(struct IClass *cl, Object *obj, Msg msg)
{
    struct ActionData *data = INST_DATA(cl, obj);
    struct PrefsListEntry *plnode;
    struct FileRequester *aslreq;
    char path[256];
    BPTR fh;
    ULONG iffhead[3];
    ULONG *buff;
    APTR pic;

    struct TagItem asltags[] = { { ASLFR_InitialDrawer, (IPTR) NEP_PRESETDRAWER },
                                 { ASLFR_TitleText, __(MSG_ACTION_CFG_IMPORT) },
                                 { TAG_END, (IPTR) NULL } };

    DoMethod(data->prefslistobj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &plnode);

    if((aslreq = MUI_AllocAslRequest(ASL_FileRequest, asltags)))
    {
        if(MUI_AslRequest(aslreq, TAG_END))
        {
            strncpy(path, aslreq->fr_Drawer, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            AddPart(path, aslreq->fr_File, 256);
            fh = Open(path, MODE_OLDFILE);
            if(fh)
            {
                Read(fh, iffhead, 12);
                if(AROS_LONG2BE(iffhead[0]) == ID_FORM)
                {
                    buff = psdAllocVec(AROS_LONG2BE(iffhead[1])+8);
                    if(buff)
                    {
                        buff[0] = iffhead[0];
                        buff[1] = iffhead[1];
                        buff[2] = iffhead[2];
                        if(Read(fh, &buff[3], AROS_LONG2BE(iffhead[1])-4) == AROS_LONG2BE(iffhead[1])-4)
                        {
                            switch(buff[2])
                            {
                                case IFFFORM_STACKCFG:
                                    pic = psdFindCfgForm(NULL, AROS_LONG2BE(buff[2]));
                                    if(pic)
                                    {
                                        psdReadCfg(pic, buff);
                                    } else {
                                        psdAddCfgEntry(NULL, buff);
                                    }
                                    break;

                                case IFFFORM_DEVICECFG:
                                case IFFFORM_CLASSCFG:
                                    psdAddCfgEntry(NULL, buff);
                                    break;

                                case IFFFORM_DEVCFGDATA:
                                case IFFFORM_IFCFGDATA:
                                    if(plnode)
                                    {
                                        if((plnode->chunkid == AROS_LONG2BE(buff[2])) ||
                                           (plnode->chunkid == IFFFORM_DEVICECFG) ||
                                           (plnode->chunkid == IFFFORM_IFCFGDATA))
                                        {
                                            pic = psdFindCfgForm(NULL, IFFFORM_DEVICECFG);
                                            while(pic)
                                            {
                                                if(psdMatchStringChunk(pic, IFFCHNK_DEVID, plnode->devid))
                                                {
                                                    psdAddCfgEntry(pic, buff);
                                                    break;
                                                }
                                                pic = psdNextCfgForm(pic);
                                            }
                                            break;
                                        }
                                    }
                                    MUI_RequestA(data->appobj, data->winobj, 0, NULL, _(MSG_REQ_OK), _(MSG_ACTION_CFGDATA_OOPS_HELP), NULL);
                                    break;

                                case IFFFORM_NEPCFG:
                                case NEP_OLDPREFSFORM:      /* psdLoadCfgFromDisk() tells 6.x from 4.x */
                                    psdLoadCfgFromDisk(path);
                                    break;

                                default:
                                    MUI_RequestA(data->appobj, data->winobj, 0, NULL, _(MSG_REQ_OK), _(MSG_ACTION_CFGDATA_ERR), NULL);
                                    break;
                            }
                        } else {
                            psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_CFGDATA_ERR_READ), path);
                        }
                        psdFreeVec(buff);
                   } else {
                       psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_CFGDATA_ERR_ALLOC), path);
                   }
                } else {
                    psdAddErrorMsg(RETURN_ERROR, _(MSG_APP_TITLE), _(MSG_ACTION_CFGDATA_ERR_IFF), path);
                }
                Close(fh);
            } else {
                psdAddErrorMsg(RETURN_FAIL, _(MSG_APP_TITLE), _(MSG_ACTION_CFGDATA_ERR_OPEN), path);
            }
        }
        MUI_FreeAslRequest(aslreq);
    }
    return(TRUE);
}
/* \\\ */

/* /// "ActionDispatcher()" */
IPTR ActionDispatcher(struct IClass * cl asm("a0"), Object * obj asm("a2"), Msg msg asm("a1"))
{
    // There should never be an uninitialized pointer, but just in case, try to get an mungwall hit if so.
    struct ActionData *data = (struct ActionData *) 0xABADCAFE;

    // on OM_NEW the obj pointer will be void, so don't try to get the data base in this case.
    if(msg->MethodID != OM_NEW) data = INST_DATA(cl, obj);

    switch(msg->MethodID)
    {
        case OM_NEW:
            return((IPTR) Action_OM_NEW(cl, obj, msg));

        case MUIM_Setup:
            return(Action_Setup(cl, obj, msg));

        case MUIM_Cleanup:
            //MUI_RejectIDCMP(obj, IDCMP_INTUITICKS);
            while(data->OnlUpdTask)
            {
                Delay(50);
            }
            DoMethod(data->appobj, MUIM_Application_RemInputHandler, &data->eventihn);
            data->appobj = NULL;
            data->winobj = NULL;
            return(DoSuperMethodA(cl,obj,msg));

        case OM_DISPOSE:
            Action_OM_DISPOSE(cl, obj, msg);
            break;

        case MUIM_Action_HandlePsdEvents:
            EventHandler(data);
            return(TRUE);

        case MUIM_Action_CloseSubWinReq:
        {
            struct DefListEntry *winnode;
            winnode = (struct DefListEntry *) ((struct opSet *) msg)->ops_AttrList;
            if(winnode->infowindow)
            {
                set(winnode->infowindow, MUIA_Window_Open, FALSE);
                DoMethod(data->appobj, OM_REMMEMBER, winnode->infowindow);
                DoMethod(winnode->infowindow, OM_DISPOSE);
                winnode->infowindow = NULL;
            }
            return(TRUE);
        }

        case MUIM_Action_Cfg_Changed:
            return(Action_Cfg_Changed(cl, obj, msg));

        case MUIM_Action_Cfg_Snd_Changed:
            return(Action_Cfg_Snd_Changed(cl, obj, msg));

        case MUIM_Action_HW_New:
            return(Action_HW_New(cl, obj, msg));

        case MUIM_Action_HW_Copy:
            return(Action_HW_Copy(cl, obj, msg));

        case MUIM_Action_HW_Del:
            return(Action_HW_Del(cl, obj, msg));

        case MUIM_Action_HW_Update:
            return(Action_HW_Update(cl, obj, msg));

        case MUIM_Action_HW_Activate:
            return(Action_HW_Activate(cl, obj, msg));

        case MUIM_Action_HW_Info:
            return(Action_HW_Info(cl, obj, msg));

        case MUIM_Action_HW_Online:
            return(Action_HW_Online(cl, obj, msg));

        case MUIM_Action_HW_Offline:
            return(Action_HW_Offline(cl, obj, msg));

        case MUIM_Action_Online:
            return(Action_Online(cl, obj, msg));

        case MUIM_Action_Offline:
            return(Action_Offline(cl, obj, msg));

        case MUIM_Action_Restart:
            return(Action_Restart(cl, obj, msg));

        case MUIM_Action_ChgErrLevel:
            return(Action_ChgErrLevel(cl, obj, msg));

        case MUIM_Action_SaveErrors:
            return(Action_SaveErrors(cl, obj, msg));

        case MUIM_Action_FlushErrors:
            return(Action_FlushErrors(cl, obj, msg));

        case MUIM_Action_SaveDeviceList:
            return(Action_SaveDeviceList(cl, obj, msg));

        case MUIM_Action_Use:
            InternalCreateConfigGUI(data);
            psdSaveCfgToDisk(NEP_PREFS_ENV);
            return(TRUE);

        case MUIM_Action_LoadPrefs:
            return(Action_LoadPrefs(cl, obj, msg));

        case MUIM_Action_SavePrefsAs:
            return(Action_SavePrefsAs(cl, obj, msg));

        case MUIM_Action_SavePrefs:
            return(Action_SavePrefs(cl, obj, msg));

        case MUIM_Action_Prefs_Changed:
            return(Action_Prefs_Changed(cl, obj, msg));

        case MUIM_Action_UseQuit:
            return(Action_UseQuit(cl, obj, msg));

        case MUIM_Action_SaveQuit:
            return(Action_SaveQuit(cl, obj, msg));

        case MUIM_Action_LoadPrefsFrom:
            return(Action_LoadPrefsFrom(cl, obj, msg));

        case MUIM_Action_SavePrefsTo:
            return(Action_SavePrefsTo(cl, obj, msg));

        case MUIM_Action_Dev_Activate:
            return(Action_Dev_Activate(cl, obj, msg));

        case MUIM_Action_Dev_Bind:
            return(Action_Dev_Bind(cl, obj, msg));

        case MUIM_Action_Dev_Unbind:
            return(Action_Dev_Unbind(cl, obj, msg));

        case MUIM_Action_Dev_Suspend:
            return(Action_Dev_Suspend(cl, obj, msg));

        case MUIM_Action_Dev_Resume:
            return(Action_Dev_Resume(cl, obj, msg));

        case MUIM_Action_Dev_PowerCycle:
            return(Action_Dev_PowerCycle(cl, obj, msg));

        case MUIM_Action_Dev_Disable:
            return(Action_Dev_Disable(cl, obj, msg));

        case MUIM_Action_Dev_Eject:
            return(Action_Dev_Eject(cl, obj, msg));

        case MUIM_Action_Dev_ForceBind:
            return(Action_Dev_ForceBind(cl, obj, msg));

        case MUIM_Action_Dev_Info:
            return(Action_Dev_Info(cl, obj, msg));

        case MUIM_Action_Dev_Configure:
            return(Action_Dev_Configure(cl, obj, msg));

        case MUIM_Action_Cls_Activate:
            return(Action_Cls_Activate(cl, obj, msg));

        case MUIM_Action_Cls_Add:
            return(Action_Cls_Add(cl, obj, msg));

        case MUIM_Action_Cls_Remove:
            return(Action_Cls_Remove(cl, obj, msg));

        case MUIM_Action_Cls_Scan:
            return(Action_Cls_Scan(cl, obj, msg));

        case MUIM_Action_Cls_Configure:
            return(Action_Cls_Configure(cl, obj, msg));

        case MUIM_Action_Info_MemPool:
            return(Action_Info_MemPool(cl, obj, msg));

        case MUIM_Action_About:
            DoMethod(data->cfgpagelv, MUIM_Set, MUIA_List_Active, 0);
            return(TRUE);

        case MUIM_Action_WakeUp:
            set(data->appobj, MUIA_Application_Iconified, FALSE);
            DoMethod(data->winobj, MUIM_Window_ToFront);
            set(data->winobj, MUIA_Window_Activate, TRUE);
            return(TRUE);

        case MUIM_Action_Cfg_Activate:
            return(Action_Cfg_Activate(cl, obj, msg));

        case MUIM_Action_Cfg_Remove:
            return(Action_Cfg_Remove(cl, obj, msg));

        case MUIM_Action_Cfg_Export:
            return(Action_Cfg_Export(cl, obj, msg));

        case MUIM_Action_Cfg_Import:
            return(Action_Cfg_Import(cl, obj, msg));

    }
    return(DoSuperMethodA(cl,obj,msg));
}
/* \\\ */
