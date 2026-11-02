/*
 *----------------------------------------------------------------------------
 *                         cdceth class for Neptune
 *----------------------------------------------------------------------------
 * Based on the Poseidon cdceth class.
 * Copyright (C) 2025, The AROS Development Team.
 */

#include "debug.h"

#include "cdceth.class.h"
#include "cdceth_encap.h"

#include <devices/usb_cdc.h>

static void cdceth_seed_mac(struct NepClassEth *ncp);
static BOOL cdceth_mac_is_valid(const UBYTE *mac);
static BOOL cdceth_parse_mac_string(const STRPTR macstr, UBYTE *mac);
static BOOL cdceth_load_mac(struct NepClassEth *ncp);
static void cdceth_dump_interface(struct NepClassEth *ncp);
static void cdceth_log_pipe_attrs(struct PsdPipe *pipe, BOOL allow_short, BOOL nak_timeout, ULONG nak_time, BOOL allow_runt);
static UWORD cdceth_build_packet_filter(struct NepClassEth *ncp);
static BOOL cdceth_set_packet_filter(struct NepClassEth *ncp, UWORD filter);
static void cdceth_complete_write(struct NepClassEth *ncp, LONG ioerr, ULONG actual);

/* /// "Lib Stuff" */
const STRPTR libname = CLASS_NAME;

static
const APTR DevFuncTable[] =
{
    (APTR) devOpen,
    (APTR) devClose,
    (APTR) devExpunge,
    (APTR) devReserved,
    (APTR) devBeginIO,
    (APTR) devAbortIO,
    (APTR) -1,
};

int libInit(struct NepEthBase * nh)
{
    struct NepClassEth *ncp;
    struct NepEthBase *ret = NULL;

    if(nDevNameTaken(DEVNAME))
    {
        return FALSE;
    }

    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", (void *) nh, (void *) EXEC_BASE_NAME));

    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);

#define	UtilityBase	nh->nh_UtilityBase

    if(UtilityBase)
    {
        NewList(&nh->nh_Units);

        if((nh->nh_DevBase = (struct NepEthDevBase *) MakeLibrary((APTR) DevFuncTable, NULL, (APTR) devInit,
           sizeof(struct NepEthDevBase), NULL)))
        {
            ncp = &nh->nh_DummyNCP;
            ncp->ncp_ClsBase = nh;
            ncp->ncp_Interface = NULL;
            ncp->ncp_CDC = AllocVec(sizeof(struct ClsDevCfg), MEMF_PUBLIC|MEMF_CLEAR);
            if(ncp->ncp_CDC)
            {
                nh->nh_DevBase->np_ClsBase = nh;
                Forbid();
                AddDevice((struct Device *) nh->nh_DevBase);
                nh->nh_DevBase->np_Library.lib_OpenCnt++;
                Permit();
                ret = nh;
            }
        } else {
            KPRINTF(20, ("failed to create %s\n", DEVNAME));
        }
        if(!ret)
        {
            CloseLibrary(UtilityBase);
        }
    } else {
        KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
    }

    KPRINTF(10, ("libInit: Ok\n"));
    return(ret ? TRUE : FALSE);
}

int libOpen(struct NepEthBase * nh)
{
    KPRINTF(10, ("libOpen nh: 0x%08lx\n", (void *) nh));
    nLoadClassConfig(nh);
    return(TRUE);
}

int libExpunge(struct NepEthBase * nh)
{
    struct NepClassEth *ncp;

    KPRINTF(10, ("libExpunge nh: 0x%08lx\n", (void *) nh));

    if(nh->nh_DevBase->np_Library.lib_OpenCnt == 1)
    {
        KPRINTF(1, ("libExpunge: closelibrary utilitybase 0x%08lx\n",
                    (void *) UtilityBase));
        CloseLibrary((struct Library *) UtilityBase);

        ncp = (struct NepClassEth *) nh->nh_Units.lh_Head;
        while(ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ)
        {
            Remove((struct Node *) ncp);
            FreeVec(ncp->ncp_CDC);
            FreeVec(ncp);
            ncp = (struct NepClassEth *) nh->nh_Units.lh_Head;
        }

        nh->nh_DevBase->np_Library.lib_OpenCnt--;
        RemDevice((struct Device *) nh->nh_DevBase);

        KPRINTF(5, ("libExpunge: Unloading done! cdceth.usbclass expunged!\n\n"));
    } else {
        KPRINTF(5, ("libExpunge: Could not expunge, LIBF_DELEXP set!\n"));
        return(FALSE);
    }

    return(TRUE);
}

/* \\\ */

/*
 * ***********************************************************************
 * * Library functions                                                   *
 * ***********************************************************************
 */

/* /// "usbAttemptDeviceBinding()" */
struct NepClassEth * usbAttemptDeviceBinding(struct NepEthBase *nh, struct PsdDevice *pd)
{
    struct Library *ps;
    struct List *cfgs = NULL;
    struct PsdConfig *pc;
    struct List *ifs;
    struct PsdInterface *pif;
    IPTR prodid;
    IPTR vendid;

    KPRINTF(1, ("nepEthAttemptDeviceBinding(0x%08lx)\n", (void *) pd));

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_VendorID, &vendid,
                    DA_ProductID, &prodid,
                    TAG_END);

        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ConfigList, &cfgs,
                    TAG_END);

        /* Only bind to devices exposing a CDC Ethernet control interface on any config. */
        for(pc = (struct PsdConfig *) (cfgs ? cfgs->lh_Head : NULL);
            pc && pc->pc_Node.ln_Succ;
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ)
        {
            psdGetAttrs(PGA_CONFIG, pc,
                        CA_InterfaceList, &ifs,
                        TAG_END);

            for(pif = ifs ? (struct PsdInterface *) ifs->lh_Head : NULL;
                pif && pif->pif_Node.ln_Succ;
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ)
            {
                struct PsdInterface *altpif;
                struct List *altlist = NULL;
                IPTR ifcls = 0, ifsub = 0, ifproto = 0;

                psdGetAttrs(PGA_INTERFACE, pif,
                            IFA_Class, &ifcls,
                            IFA_SubClass, &ifsub,
                            IFA_Protocol, &ifproto,
                            IFA_AlternateIfList, &altlist,
                            TAG_DONE);

                if(cdceth_is_cdc_ethernet((uint8_t) ifcls, (uint8_t) ifsub, (uint8_t) ifproto))
                {
                    CloseLibrary(ps);
                    return(usbForceDeviceBinding(nh, pd));
                }

                for(altpif = altlist ? (struct PsdInterface *) altlist->lh_Head : NULL;
                    altpif && altpif->pif_Node.ln_Succ;
                    altpif = (struct PsdInterface *) altpif->pif_Node.ln_Succ)
                {
                    psdGetAttrs(PGA_INTERFACE, altpif,
                                IFA_Class, &ifcls,
                                IFA_SubClass, &ifsub,
                                IFA_Protocol, &ifproto,
                                TAG_DONE);

                    if(cdceth_is_cdc_ethernet((uint8_t) ifcls, (uint8_t) ifsub, (uint8_t) ifproto))
                    {
                        CloseLibrary(ps);
                        return(usbForceDeviceBinding(nh, pd));
                    }
                }
            }
        }
        CloseLibrary(ps);
    }
    return(NULL);
}
/* \\\ */

/* /// "usbForceDeviceBinding()" */
struct NepClassEth * usbForceDeviceBinding(struct NepEthBase *nh, struct PsdDevice *pd)

{
    struct Library *ps;
    struct NepClassEth *ncp;
    struct NepClassEth *tmpncp;
    struct ClsDevCfg *cdc;
    STRPTR devname;
    STRPTR devidstr;
    IPTR prodid;
    IPTR vendid;
    ULONG unitno;
    BOOL  unitfound;
    UBYTE buf[64];
    struct Task *tmptask;

    KPRINTF(1, ("nepEthForceDeviceBinding(%08lx)\n", pd));

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ProductID, &prodid,
                    DA_VendorID, &vendid,
                    DA_ProductName, &devname,
                    DA_IDString, &devidstr,
                    TAG_END);
        Forbid();

        unitfound = FALSE;
        unitno = (ULONG) -1;
        ncp = (struct NepClassEth *) nh->nh_Units.lh_Head;
        while(ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ)
        {
            if(!strcmp(ncp->ncp_DevIDString, devidstr))
            {
                unitno = ncp->ncp_UnitNo;
                unitfound = TRUE;
                break;
            }
            ncp = (struct NepClassEth *) ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ;
        }
        if(!unitfound)
        {
            /* as units are freed in the expunge-vector, the memory is
               outside the scope of neptune.library */
            if(!(ncp = AllocVec(sizeof(struct NepClassEth), MEMF_PUBLIC|MEMF_CLEAR)))
            {
                Permit();
                CloseLibrary(ps);
                return(NULL);
            }
            ncp->ncp_CDC = cdc = AllocVec(sizeof(struct ClsDevCfg), MEMF_PUBLIC|MEMF_CLEAR);
            if(!cdc)
            {
                Permit();
                FreeVec(ncp);
                CloseLibrary(ps);
                return(NULL);
            }
            /* IORequests may be queued even if the task is gone. */
            ncp->ncp_UnitNo = (ULONG) -1;
            NewList(&ncp->ncp_Unit.unit_MsgPort.mp_MsgList);
            NewList(&ncp->ncp_OrphanQueue);
            NewList(&ncp->ncp_WriteQueue);
            NewList(&ncp->ncp_BufManList);
            NewList(&ncp->ncp_EventList);
            NewList(&ncp->ncp_TrackList);
            NewList(&ncp->ncp_Multicasts);
            strncpy(ncp->ncp_DevIDString, devidstr, 127);
            AddTail(&nh->nh_Units, &ncp->ncp_Unit.unit_MsgPort.mp_Node);
        }
        ncp->ncp_ClsBase = nh;
        ncp->ncp_Device = pd;
        ncp->ncp_UnitProdID = prodid;
        ncp->ncp_UnitVendorID = vendid;

        nLoadBindingConfig(ncp);

        /* Find next free unit number */
        if(unitno == (ULONG) -1)
        {
            unitno = ncp->ncp_CDC->cdc_DefaultUnit;
            tmpncp = (struct NepClassEth *) nh->nh_Units.lh_Head;
            while(tmpncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ)
            {
                if(tmpncp->ncp_UnitNo == unitno)
                {
                    unitno++;
                    tmpncp = (struct NepClassEth *) nh->nh_Units.lh_Head;
                } else {
                    tmpncp = (struct NepClassEth *) tmpncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ;
                }
            }
        }
        ncp->ncp_UnitNo = unitno;
        Permit();

        psdSafeRawDoFmt(buf, 64, CLASS_NAME "<%08lx>", ncp);
        ncp->ncp_ReadySignal = SIGB_SINGLE;
        ncp->ncp_ReadySigTask = FindTask(NULL);
        SetSignal(0, SIGF_SINGLE);
        if((tmptask = psdSpawnSubTask(buf, nEthTask, ncp)))
        {
            psdBorrowLocksWait(tmptask, 1L<<ncp->ncp_ReadySignal);
            //Wait(1L<<ncp->ncp_ReadySignal);
            if(ncp->ncp_Task)
            {
                ncp->ncp_ReadySigTask = NULL;
                //FreeSignal(ncp->ncp_ReadySignal);
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               NEP_BOUND_TXT,
                               devname, nh->nh_DevBase->np_Library.lib_Node.ln_Name,
                               ncp->ncp_UnitNo);

                CloseLibrary(ps);
                return(ncp);
            }
        }
        ncp->ncp_ReadySigTask = NULL;
        //FreeSignal(ncp->ncp_ReadySignal);
        /* Get rid of unit structure */
        /*Forbid();
        Remove((struct Node *) ncp);
        FreeVec(ncp->ncp_CDC);
        FreeVec(ncp);
        Permit();*/
        CloseLibrary(ps);
    }
    return(NULL);
}
/* \\\ */

/* /// "usbReleaseDeviceBinding()" */
void usbReleaseDeviceBinding(struct NepEthBase *nh, struct NepClassEth *ncp)
{
    struct Library *ps;
    STRPTR devname;
    KPRINTF(1, ("nepEthReleaseDeviceBinding(%08lx)\n", ncp));

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        Forbid();
        ncp->ncp_ReadySignal = SIGB_SINGLE;
        ncp->ncp_ReadySigTask = FindTask(NULL);
        if(ncp->ncp_Task)
        {
            Signal(ncp->ncp_Task, SIGBREAKF_CTRL_C);
        }
        Permit();
        while(ncp->ncp_Task)
        {
            Wait(1L<<ncp->ncp_ReadySignal);
        }
        //FreeSignal(ncp->ncp_ReadySignal);
        psdGetAttrs(PGA_DEVICE, ncp->ncp_Device, DA_ProductName, &devname, TAG_END);
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       NEP_RELEASED_TXT,
                       devname);
        /*psdFreeVec(ncp);*/
        CloseLibrary(ps);
    }
}
/* \\\ */

/* /// "usbGetAttrsA()" */
LONG (usbGetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepEthBase * nh asm("a6"))
{

    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("nepEthGetAttrsA(%ld, %08lx, %08lx)\n", type, usbstruct, tags));
    switch(type)
    {
        case UGA_CLASS:
             if((ti = FindTagItem(UCCA_Priority, tags)))
             {
                 *((SIPTR *) ti->ti_Data) = 0;
                 count++;
             }
             if((ti = FindTagItem(UCCA_Description, tags)))
             {
                 *((STRPTR *) ti->ti_Data) = "Ethernet SANA driver for CDC Ethernet devices via usbcdceth.device";
                 count++;
             }
             if((ti = FindTagItem(UCCA_HasClassCfgGUI, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_HasBindingCfgGUI, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_AfterDOSRestart, tags)))
             {
                 *((IPTR *) ti->ti_Data) = FALSE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_UsingDefaultCfg, tags)))
             {
                 *((IPTR *) ti->ti_Data) = nh->nh_DummyNCP.ncp_UsingDefaultCfg;
                 count++;
             }
             break;

         case UGA_BINDING:
             if((ti = FindTagItem(UCBA_UsingDefaultCfg, tags)))
             {
                 *((IPTR *) ti->ti_Data) = ((struct NepClassEth *) usbstruct)->ncp_UsingDefaultCfg;
                 count++;
             }
             break;
    }
    return(count);
}
/* \\\ */

/* /// "usbSetAttrsA()" */
LONG (usbSetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepEthBase * nh asm("a6"))
{
    return(0);
}
/* \\\ */

/* /// "usbDoMethodA()" */
IPTR (usbDoMethodA)(ULONG methodid asm("d0"), IPTR * methoddata asm("a1"), struct NepEthBase * nh asm("a6"))
{

    struct NepClassEth *ncp;

    KPRINTF(10, ("Do Method %ld\n", methodid));
    switch(methodid)
    {
        case UCM_AttemptDeviceBinding:
            return((IPTR) usbAttemptDeviceBinding(nh, (struct PsdDevice *) methoddata[0]));

        case UCM_ForceDeviceBinding:
            return((IPTR) usbForceDeviceBinding(nh, (struct PsdDevice *) methoddata[0]));

        case UCM_ReleaseDeviceBinding:
            usbReleaseDeviceBinding(nh, (struct NepClassEth *) methoddata[0]);
            return(TRUE);

        case UCM_OpenCfgWindow:
            return(nOpenBindingCfgWindow(nh, &nh->nh_DummyNCP));

        case UCM_OpenBindingCfgWindow:
            return(nOpenBindingCfgWindow(nh, (struct NepClassEth *) methoddata[0]));

        case UCM_ConfigChangedEvent:
            nLoadClassConfig(nh);
            Forbid();
            ncp = (struct NepClassEth *) nh->nh_Units.lh_Head;
            while(ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ)
            {
                nLoadBindingConfig(ncp);
                ncp = (struct NepClassEth *) ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ;
            }
            Permit();
            return(TRUE);

        default:
            break;
    }
    return(0);
}
/* \\\ */

/* /// "nLoadClassConfig()" */
BOOL nLoadClassConfig(struct NepEthBase *nh)
{
    struct NepClassEth *ncp = &nh->nh_DummyNCP;
    struct Library *ps;
    struct ClsDevCfg *cdc;
    struct PsdIFFContext *pic;

    KPRINTF(10, ("Loading Class Config...\n"));
    if(ncp->ncp_GUITask)
    {
        return(FALSE);
    }
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }

    Forbid();
    /* Create default config */
    cdc = ncp->ncp_CDC;
    cdc->cdc_ChunkID = AROS_LONG2BE(MAKE_ID('C','D','C','E'));
    cdc->cdc_Length = AROS_LONG2BE(sizeof(struct ClsDevCfg)-8);
    cdc->cdc_DefaultUnit = 0;
    cdc->cdc_MediaType = 0;
    ncp->ncp_UsingDefaultCfg = TRUE;
    /* try to load default config */
    pic = psdGetClsCfg(libname);
    if(pic)
    {
        cdc = psdGetCfgChunk(pic, AROS_LONG2BE(ncp->ncp_CDC->cdc_ChunkID));
        if(cdc)
        {
            CopyMem(((UBYTE *) cdc) + 8, ((UBYTE *) ncp->ncp_CDC) + 8, min(AROS_LONG2BE(cdc->cdc_Length), AROS_LONG2BE(ncp->ncp_CDC->cdc_Length)));
            psdFreeVec(cdc);
            ncp->ncp_UsingDefaultCfg = FALSE;
        }
    }
    Permit();
    CloseLibrary(ps);
    return(FALSE);
}
/* \\\ */

/* /// "nLoadBindingConfig()" */
BOOL nLoadBindingConfig(struct NepClassEth *ncp)
{
    struct NepEthBase *nh = ncp->ncp_ClsBase;
    struct Library *ps;
    struct ClsDevCfg *cdc;
    struct PsdIFFContext *pic;

    KPRINTF(10, ("Loading Binding Config...\n"));
    if(ncp->ncp_GUITask)
    {
        return(FALSE);
    }
    //nLoadClassConfig(nh);
    *ncp->ncp_CDC = *nh->nh_DummyNCP.ncp_CDC;
    ncp->ncp_UsingDefaultCfg = TRUE;

    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }

    Forbid();
    /* Load config */
    pic = psdGetUsbDevCfg(libname, ncp->ncp_DevIDString, NULL);
    if(pic)
    {
        cdc = psdGetCfgChunk(pic, AROS_LONG2BE(ncp->ncp_CDC->cdc_ChunkID));
        if(cdc)
        {
            CopyMem(((UBYTE *) cdc) + 8, ((UBYTE *) ncp->ncp_CDC) + 8, min(AROS_LONG2BE(cdc->cdc_Length), AROS_LONG2BE(ncp->ncp_CDC->cdc_Length)));
            psdFreeVec(cdc);
            ncp->ncp_UsingDefaultCfg = FALSE;
        }
    }
    Permit();
    CloseLibrary(ps);
    return(FALSE);
}
/* \\\ */

/* /// "nOpenBindingCfgWindow()" */
LONG nOpenBindingCfgWindow(struct NepEthBase *nh, struct NepClassEth *ncp)
{
    struct Library *ps;
    KPRINTF(10, ("Opening GUI...\n"));
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }
    Forbid();
    if(!ncp->ncp_GUITask)
    {
        if((ncp->ncp_GUITask = psdSpawnSubTask(CLASS_NAME " GUI", nGUITask, ncp)))
        {
            Permit();
            CloseLibrary(ps);
            return(TRUE);
        }
    }
    Permit();
    CloseLibrary(ps);
    return(FALSE);
}
/* \\\ */

/**************************************************************************/

#undef  ps
#define ps ncp->ncp_Base

static const char *const MediaTypeStrings[] =
{
    "Auto negotiation",
    "10Base-T Half Duplex",
    "10Base-T Full Duplex",
    "100Base-TX Half Duplex",
    "100Base-TX Full Duplex",
    "1000Base-T Half Duplex",
    "1000Base-T Full Duplex",
    "2500Base-T Full Duplex",
    NULL
};

/* /// "nEthTask()" */
void nEthTask()
{

    struct NepClassEth *ncp;
    struct PsdPipe *pp;
    ULONG sigmask;
    ULONG sigs;
    LONG ioerr;
    UBYTE *pktptr;
    ULONG pktlen;
    UWORD cnt;
    LONG lastioerr = 0;
    ULONG errcount = 0;
    struct Sana2PacketTypeStats *stats = NULL;

    struct IOSana2Req *ioreq;

    if((ncp = nAllocEth()))
    {
        Forbid();
        if(ncp->ncp_ReadySigTask)
        {
            Signal(ncp->ncp_ReadySigTask, 1L<<ncp->ncp_ReadySignal);
        }
        Permit();

        cdceth_seed_mac(ncp);
        KPRINTF(10, ("MAC Address in EEPROM %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                     ncp->ncp_MacAddress[0],
                     ncp->ncp_MacAddress[1],
                     ncp->ncp_MacAddress[2],
                     ncp->ncp_MacAddress[3],
                     ncp->ncp_MacAddress[4],
                     ncp->ncp_MacAddress[5]));

        //if((!(ncp->ncp_StateFlags & DDF_OFFLINE)))// && (ncp->ncp_StateFlags & DDF_CONFIGURED))
        {
            /* Record start time_of_day */
            //GetSysTime(&ncp->ncp_DeviceStats.LastStart);
            nSetOnline(ncp);
        }

        /* Main task */
        sigmask = (1L<<ncp->ncp_Unit.unit_MsgPort.mp_SigBit)|(1L<<ncp->ncp_TaskMsgPort->mp_SigBit)|SIGBREAKF_CTRL_C;
        do
        {
            // start transmitting read request if online...
            if((ncp->ncp_StateFlags & DDF_ONLINE) && (ncp->ncp_ReadPending == NULL))
            {
                ncp->ncp_ReadPending = ncp->ncp_ReadBuffer[ncp->ncp_ReadBufNum];
                psdSendPipe(ncp->ncp_EPInPipe, ncp->ncp_ReadPending, ncp->ncp_ReadBufSize);
                ncp->ncp_ReadBufNum ^= 1;
            }
            while((pp = (struct PsdPipe *) GetMsg(ncp->ncp_TaskMsgPort)))
            {
                KPRINTF(1, ("Pipe back 0x%08lx\n", (void *) pp));
                if(pp == ncp->ncp_EPOutPipe)
                {
                    cdceth_complete_write(ncp, psdGetPipeError(pp), psdGetPipeActual(pp));
                }
                if(pp == ncp->ncp_EPInPipe)
                {
                    if((pktptr = ncp->ncp_ReadPending))
                    {
                        ioerr = psdGetPipeError(pp);
                        pktlen = psdGetPipeActual(pp);
                        KPRINTF(1, ("ReadBack with %ld bytes (%ld).\n", pktlen, ioerr));
                        // interleave next packet reading ASAP.
                        if(ncp->ncp_StateFlags & DDF_ONLINE)
                        {
                            ncp->ncp_ReadPending = ncp->ncp_ReadBuffer[ncp->ncp_ReadBufNum];
                            psdSendPipe(ncp->ncp_EPInPipe, ncp->ncp_ReadPending, ncp->ncp_ReadBufSize);
                            ncp->ncp_ReadBufNum ^= 1;
                        } else {
                            ncp->ncp_ReadPending = NULL;
                        }
                        if(ioerr)
                        {
                            if(lastioerr != ioerr)
                            {
                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                               "Eth receive failed: %s (%ld)",
                                               psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                                errcount = 0;
                            } else {
                                errcount++;
                                if(errcount > 20)
                                {
                                    psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                                                   NEP_GIVEUP_TXT);
                                    Signal(ncp->ncp_Task, SIGBREAKF_CTRL_C);
                                }
                            }
                            lastioerr = ioerr;
                            psdDelayMS(50);
                            break;
                        } else {
                            KPRINTF(1, ("Pkt %ld received\n", pktlen));
                            DB(dumpmem(pktptr, pktlen));
                            cdceth_handle_rx(ncp, pktptr, pktlen);
                        }
                    }
                }
            }

            while((ioreq = (struct IOSana2Req *) GetMsg(&ncp->ncp_Unit.unit_MsgPort)))
            {
                KPRINTF(5, ("command ioreq: 0x%08lx cmd: %lu len: %ld\n",
                        (void *) ioreq, ioreq->ios2_Req.io_Command, ioreq->ios2_DataLength));
                switch(ioreq->ios2_Req.io_Command)
                {
                    case S2_CONFIGINTERFACE:
                        //nWritePegRegs(ncp, ncp->ncp_MacAddress, ETHER_ADDR_SIZE, PEGREG_ETH_ID);
                        /* Now configured */
                        ncp->ncp_StateFlags |= DDF_CONFIGURED;
                        if(!(ncp->ncp_StateFlags & DDF_ONLINE))
                        {
                            nSetOnline(ncp);
                        }
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case S2_ADDMULTICASTADDRESS:
                    case S2_DELMULTICASTADDRESS:
                    case S2_ADDMULTICASTADDRESSES:
                    case S2_DELMULTICASTADDRESSES:
                        nUpdateRXMode(ncp);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case S2_ONLINE:
                        nSetOnline(ncp);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case S2_OFFLINE:
                        ncp->ncp_StateFlags &= ~DDF_ONLINE;
                        ncp->ncp_StateFlags |= DDF_OFFLINE;
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    default:
                        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
                        ReplyMsg((struct Message *) ioreq);
                        break;
                }
            }
            Forbid();
            while((!ncp->ncp_WritePending) && ncp->ncp_WriteQueue.lh_Head->ln_Succ)
            {
                ioreq = (struct IOSana2Req *) RemHead(&ncp->ncp_WriteQueue);
                Permit();
                nWritePacket(ncp, ioreq);
                Forbid();
            }
            Permit();
            sigs = Wait(sigmask);
        } while(!(sigs & SIGBREAKF_CTRL_C));

        Forbid();
        /* Now remove all requests still pending *anywhere* */
        //ncp->ncp_DenyRequests = TRUE;
        /* Current transfers */
        if((ioreq = ncp->ncp_WritePending))
        {
            KPRINTF(1, ("Aborting pending write...\n"));
            psdAbortPipe(ncp->ncp_EPOutPipe);
            psdWaitPipe(ncp->ncp_EPOutPipe);
            ioreq->ios2_Req.io_Error = IOERR_ABORTED;
            ReplyMsg((struct Message *) ioreq);
            ncp->ncp_WritePending = NULL;
        }
        if(ncp->ncp_ReadPending)
        {
            KPRINTF(1, ("Aborting pending read...\n"));
            psdAbortPipe(ncp->ncp_EPInPipe);
            psdWaitPipe(ncp->ncp_EPInPipe);
            ncp->ncp_ReadPending = NULL;
        }
        /* Command queue */
        /*while(ioreq = (struct IOSana2Req *) GetMsg(&ncp->ncp_Unit.unit_MsgPort))
        {
            KPRINTF(1, ("Aborting pending requests...\n"));
            ioreq->ios2_Req.io_Error = IOERR_ABORTED;
            ReplyMsg((struct Message *) ioreq);
        }*/
        Permit();

        nDoEvent(ncp, S2EVENT_OFFLINE);

        KPRINTF(20, ("Going down the river!\n"));
        nFreeEth(ncp);
    }
    
}
/* \\\ */

/* /// "cdceth_start_function()" */
/* Switches the device to the chosen configuration, configures the
   encapsulation, allocates the transfer buffers and selects the data
   interface's operating alternate. Needs ncp_EP0Pipe. */
static BOOL cdceth_start_function(struct NepClassEth *ncp, struct PsdConfig *cur_cfg)
{
    if(ncp->ncp_Config != cur_cfg)
    {
        IPTR cfgnum = 0;

        psdGetAttrs(PGA_CONFIG, ncp->ncp_Config,
                    CA_ConfigNum, &cfgnum,
                    TAG_END);
        KPRINTF(5, ("Switching device to configuration %ld\n", (LONG) cfgnum));
        if(!psdSetDeviceConfig(ncp->ncp_EP0Pipe, cfgnum))
        {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                           "Could not switch to configuration %ld!", (LONG) cfgnum);
            return(FALSE);
        }
    }

    if(!cdceth_encap_configure(ncp))
    {
        return(FALSE);
    }

    ULONG bufsize = max(ncp->ncp_ReadBufSize, ncp->ncp_WriteBufSize);
    if(!(ncp->ncp_ReadBuffer[0] = AllocVec(bufsize * 4, MEMF_PUBLIC|MEMF_CLEAR)))
    {
        KPRINTF(1, ("Out of memory for read buffer\n"));
        return(FALSE);
    }
    ncp->ncp_ReadBuffer[1] = ncp->ncp_ReadBuffer[0] + bufsize;
    ncp->ncp_WriteBuffer[0] = ncp->ncp_ReadBuffer[1] + bufsize;
    ncp->ncp_WriteBuffer[1] = ncp->ncp_WriteBuffer[0] + bufsize;

    if(!psdSetAltInterface(ncp->ncp_EP0Pipe, ncp->ncp_Interface))
    {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "Could not select data interface alternate setting!");
        return(FALSE);
    }
    return(TRUE);
}
/* \\\ */

/* /// "nAllocEth()" */
struct NepClassEth * nAllocEth(void)
{
    struct Task *thistask;
    struct NepClassEth *ncp;

    thistask = FindTask(NULL);
    ncp = thistask->tc_UserData;
    if((ncp->ncp_Base = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)) == NULL)
    {
        Alert(AG_OpenLib);
        return NULL;
    }

    KPRINTF(5, ("ncp =  0x%08lx, ps = 0x%08lx\n", ncp, ps));

    do
    {
        struct PsdInterface *best_if = NULL;
        struct PsdInterface *ctrl_if = NULL;
        struct PsdConfig *pc;
        struct PsdConfig *cur_cfg = NULL;
        struct List *cfgs = NULL;
        IPTR ifcls = 0, ifsub = 0, ifproto = 0;
        IPTR altifnum = 0;
        IPTR dataifnum = -1;
        BOOL have_dataifnum = FALSE;
        struct PsdDescriptor *pdd;
        UBYTE *descdata = NULL;

        psdGetAttrs(PGA_DEVICE, ncp->ncp_Device,
                    DA_ConfigList, &cfgs,
                    DA_Config, &cur_cfg,
                    TAG_END);

        for(pc = (struct PsdConfig *) (cfgs ? cfgs->lh_Head : NULL);
            pc && pc->pc_Node.ln_Succ;
            pc = (struct PsdConfig *) pc->pc_Node.ln_Succ)
        {
            struct List *ifs = NULL;
            struct PsdInterface *pif;

            ctrl_if = NULL;
            dataifnum = -1;
            have_dataifnum = FALSE;

            psdGetAttrs(PGA_CONFIG, pc,
                        CA_InterfaceList, &ifs,
                        TAG_END);

            for(pif = ifs ? (struct PsdInterface *) ifs->lh_Head : NULL;
                pif && pif->pif_Node.ln_Succ;
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ)
            {
                struct PsdInterface *altpif;
                struct List *altlist = NULL;

                psdGetAttrs(PGA_INTERFACE, pif,
                            IFA_Class, &ifcls,
                            IFA_SubClass, &ifsub,
                            IFA_Protocol, &ifproto,
                            IFA_AlternateIfList, &altlist,
                            TAG_DONE);

                if(cdceth_is_cdc_ethernet((uint8_t) ifcls, (uint8_t) ifsub, (uint8_t) ifproto))
                {
                    ctrl_if = pif;
                    break;
                }

                for(altpif = altlist ? (struct PsdInterface *) altlist->lh_Head : NULL;
                    altpif && altpif->pif_Node.ln_Succ;
                    altpif = (struct PsdInterface *) altpif->pif_Node.ln_Succ)
                {
                    psdGetAttrs(PGA_INTERFACE, altpif,
                                IFA_Class, &ifcls,
                                IFA_SubClass, &ifsub,
                                IFA_Protocol, &ifproto,
                                TAG_DONE);

                    if(cdceth_is_cdc_ethernet((uint8_t) ifcls, (uint8_t) ifsub, (uint8_t) ifproto))
                    {
                        ctrl_if = altpif;
                        break;
                    }
                }

                if(ctrl_if)
                {
                    break;
                }
            }

            if(!ctrl_if)
            {
                continue;
            }

            pdd = psdFindDescriptor(ncp->ncp_Device, NULL,
                                    DDA_Interface, ctrl_if,
                                    DDA_DescriptorType, UDT_CS_INTERFACE,
                                    DDA_CS_SubType, UDST_CDC_UNION,
                                    TAG_END);
            if(pdd)
            {
                psdGetAttrs(PGA_DESCRIPTOR, pdd,
                            DDA_DescriptorData, &descdata,
                            TAG_END);
                dataifnum = ((struct UsbCDCUnionDesc *) descdata)->bSlaveInterface0;
                have_dataifnum = TRUE;
                KPRINTF(5, ("CDC union descriptor selects data interface %ld\n", (long) dataifnum));
            }

            for(pif = ifs ? (struct PsdInterface *) ifs->lh_Head : NULL;
                pif && pif->pif_Node.ln_Succ;
                pif = (struct PsdInterface *) pif->pif_Node.ln_Succ)
            {
                struct PsdInterface *altpif;
                struct List *altlist = NULL;
                IPTR ifnum = -1;

                psdGetAttrs(PGA_INTERFACE, pif,
                            IFA_Class, &ifcls,
                            IFA_SubClass, &ifsub,
                            IFA_Protocol, &ifproto,
                            IFA_AlternateNum, &altifnum,
                            IFA_InterfaceNum, &ifnum,
                            IFA_AlternateIfList, &altlist,
                            TAG_DONE);

                if((ifcls == CDC_CLASS_DATA) &&
                   (!have_dataifnum || ifnum == dataifnum) &&
                   (ifsub == 0) &&
                   ((ifproto == 0) || (ifproto == 1) || (ifproto == 7)))
                {
                    struct PsdEndpoint *in = psdFindEndpoint(pif, NULL,
                                                             EA_IsIn, TRUE,
                                                             EA_TransferType, USEAF_BULK,
                                                             TAG_END);
                    struct PsdEndpoint *out = psdFindEndpoint(pif, NULL,
                                                              EA_IsIn, FALSE,
                                                              EA_TransferType, USEAF_BULK,
                                                              TAG_END);

                    if(in && out)
                    {
                        best_if = pif;
                        ncp->ncp_Config = pc;
                        ncp->ncp_EPIn = in;
                        ncp->ncp_EPOut = out;
                        ncp->ncp_Interface = pif;
                        break;
                    }
                }

                for(altpif = altlist ? (struct PsdInterface *) altlist->lh_Head : NULL;
                    altpif && altpif->pif_Node.ln_Succ;
                    altpif = (struct PsdInterface *) altpif->pif_Node.ln_Succ)
                {
                    ifnum = -1;
                    psdGetAttrs(PGA_INTERFACE, altpif,
                                IFA_Class, &ifcls,
                                IFA_SubClass, &ifsub,
                                IFA_Protocol, &ifproto,
                                IFA_AlternateNum, &altifnum,
                                IFA_InterfaceNum, &ifnum,
                                TAG_DONE);

                    if((ifcls != CDC_CLASS_DATA) || (ifsub != 0) ||
                       (ifproto != 0 && ifproto != 1 && ifproto != 7) ||
                       (have_dataifnum && ifnum != dataifnum))
                    {
                        continue;
                    }

                    struct PsdEndpoint *in = psdFindEndpoint(altpif, NULL,
                                                             EA_IsIn, TRUE,
                                                             EA_TransferType, USEAF_BULK,
                                                             TAG_END);
                    struct PsdEndpoint *out = psdFindEndpoint(altpif, NULL,
                                                              EA_IsIn, FALSE,
                                                              EA_TransferType, USEAF_BULK,
                                                              TAG_END);

                    if(in && out)
                    {
                        best_if = altpif;
                        ncp->ncp_Config = pc;
                        ncp->ncp_EPIn = in;
                        ncp->ncp_EPOut = out;
                        ncp->ncp_Interface = altpif;
                        break;
                    }
                }

                if(best_if)
                {
                    break;
                }
            }

            if(best_if)
                break;
        }

        if(!best_if)
        {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "No CDC Ethernet data interface with bulk endpoints!");
            break;
        }
        KPRINTF(5, ("Selected CDC interface 0x%08lx in config 0x%08lx (alt %ld) class/sub/proto %ld/%ld/%ld\n",
                    (void *) best_if, (void *) ncp->ncp_Config, altifnum, ifcls, ifsub, ifproto));
        ncp->ncp_ControlInterface = ctrl_if;

        psdGetAttrs(PGA_ENDPOINT, ncp->ncp_EPOut,
                    EA_MaxPktSize, &ncp->ncp_EPOutMaxPktSize,
                    TAG_END);

        KPRINTF(5, ("Max packet size %ld\n", ncp->ncp_EPOutMaxPktSize));

        ncp->ncp_ReadPending = NULL;
        ncp->ncp_WritePending = NULL;
        ncp->ncp_Unit.unit_MsgPort.mp_SigBit = AllocSignal(-1);
        ncp->ncp_Unit.unit_MsgPort.mp_SigTask = thistask;
        ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        ncp->ncp_Unit.unit_MsgPort.mp_Flags = PA_SIGNAL;

        if((ncp->ncp_TaskMsgPort = CreateMsgPort()))
        {
            if((ncp->ncp_EP0Pipe = psdAllocPipe(ncp->ncp_Device, ncp->ncp_TaskMsgPort, NULL)))
            {
                if(cdceth_start_function(ncp, cur_cfg) &&
                   (ncp->ncp_EPOutPipe = psdAllocPipe(ncp->ncp_Device, ncp->ncp_TaskMsgPort, ncp->ncp_EPOut)))
                {
                    /* Allow continuous retries without aborting on NAK timeouts. */
                    psdSetAttrs(PGA_PIPE, ncp->ncp_EPOutPipe,
                                PPA_NoShortPackets, FALSE,
                                PPA_NakTimeout, FALSE,
                                PPA_NakTimeoutTime, 5000,
                                TAG_END);
                    cdceth_log_pipe_attrs(ncp->ncp_EPOutPipe, FALSE, FALSE, 5000, FALSE);
                    if((ncp->ncp_EPInPipe = psdAllocPipe(ncp->ncp_Device, ncp->ncp_TaskMsgPort, ncp->ncp_EPIn)))
                    {
                        /* Turn off short packets */
                        psdSetAttrs(PGA_PIPE, ncp->ncp_EPInPipe,
                                    PPA_NakTimeout, FALSE,
                                    PPA_NakTimeoutTime, 5000,
                                    PPA_AllowRuntPackets, TRUE,
                                    TAG_END);
                        cdceth_log_pipe_attrs(ncp->ncp_EPInPipe, FALSE, FALSE, 5000, TRUE);
                        ncp->ncp_Task = thistask;

                        if(nInitCDC(ncp))
                        {
                            cdceth_dump_interface(ncp);
                            cdceth_set_packet_filter(ncp, cdceth_build_packet_filter(ncp));
                            return(ncp);
                        }
                        psdFreePipe(ncp->ncp_EPInPipe);
                    }
                    psdFreePipe(ncp->ncp_EPOutPipe);
                }
                psdFreePipe(ncp->ncp_EP0Pipe);
            }
            DeleteMsgPort(ncp->ncp_TaskMsgPort);
        }
        FreeSignal((LONG) ncp->ncp_Unit.unit_MsgPort.mp_SigBit);
    } while(FALSE);

    if(ncp->ncp_ReadBuffer[0])
    {
        FreeVec(ncp->ncp_ReadBuffer[0]);
        ncp->ncp_ReadBuffer[0] = NULL;
    }
    if (ncp->ncp_Base) {
        CloseLibrary(ncp->ncp_Base);
        ncp->ncp_Base = NULL;
    }
    Forbid();
    ncp->ncp_Task = NULL;
    if(ncp->ncp_ReadySigTask)
    {
        Signal(ncp->ncp_ReadySigTask, 1L<<ncp->ncp_ReadySignal);
    }
    return(NULL);
}
/* \\\ */

/* /// "nFreeEth()" */
void nFreeEth(struct NepClassEth *ncp)
{
    struct IOSana2Req *ioreq;
    Forbid();
    /* Disable the message port, messages may still be queued */
    ncp->ncp_Unit.unit_MsgPort.mp_SigTask = NULL;
    ncp->ncp_Unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    FreeSignal((LONG) ncp->ncp_Unit.unit_MsgPort.mp_SigBit);
    // get rid of all messages that still have appeared here
    while((ioreq = (struct IOSana2Req *) GetMsg(&ncp->ncp_Unit.unit_MsgPort)))
    {
        ioreq->ios2_Req.io_Error = IOERR_ABORTED;
        ReplyMsg((struct Message *) ioreq);
    }
    Permit();

    psdFreePipe(ncp->ncp_EPInPipe);
    psdFreePipe(ncp->ncp_EPOutPipe);
    psdFreePipe(ncp->ncp_EP0Pipe);

    if(ncp->ncp_ReadBuffer[0])
    {
        FreeVec(ncp->ncp_ReadBuffer[0]);
        ncp->ncp_ReadBuffer[0] = NULL;
    }

    DeleteMsgPort(ncp->ncp_TaskMsgPort);
    CloseLibrary(ncp->ncp_Base);
    Forbid();
    ncp->ncp_Task = NULL;
    if(ncp->ncp_ReadySigTask)
    {
        Signal(ncp->ncp_ReadySigTask, 1L<<ncp->ncp_ReadySignal);
    }
}
/* \\\ */

static void cdceth_seed_mac(struct NepClassEth *ncp)
{
    UBYTE fallback[ETHER_ADDR_SIZE] = { 0x02, 0xcd, 0xce, 0x00, 0x00, 0x00 };

    fallback[3] = (UBYTE) (ncp->ncp_UnitVendorID & 0xff);
    fallback[4] = (UBYTE) (ncp->ncp_UnitProdID & 0xff);
    fallback[5] = (UBYTE) (ncp->ncp_UnitNo & 0xff);

    if(cdceth_mac_is_valid(ncp->ncp_ROMAddress))
    {
        if(!cdceth_mac_is_valid(ncp->ncp_MacAddress))
        {
            CopyMem(ncp->ncp_ROMAddress, ncp->ncp_MacAddress, ETHER_ADDR_SIZE);
        }
        return;
    }

    CopyMem(fallback, ncp->ncp_ROMAddress, ETHER_ADDR_SIZE);
    CopyMem(fallback, ncp->ncp_MacAddress, ETHER_ADDR_SIZE);
}
/* \\\ */

static BOOL cdceth_mac_is_valid(const UBYTE *mac)
{
    BOOL all_zero = TRUE;
    int idx;

    if(!mac)
    {
        return(FALSE);
    }

    for(idx = 0; idx < ETHER_ADDR_SIZE; idx++)
    {
        if(mac[idx] != 0)
        {
            all_zero = FALSE;
            break;
        }
    }

    if(all_zero)
    {
        return(FALSE);
    }

    if(mac[0] & 1)
    {
        return(FALSE);
    }

    return(TRUE);
}
/* \\\ */

static BOOL cdceth_parse_mac_string(const STRPTR macstr, UBYTE *mac)
{
    int hi = -1;
    int count = 0;
    int idx;
    char ch;

    if(!macstr || !mac)
    {
        return(FALSE);
    }

    for(idx = 0; (ch = macstr[idx]) != '\0'; idx++)
    {
        int val = -1;

        if((ch >= '0') && (ch <= '9'))
            val = ch - '0';
        else if((ch >= 'a') && (ch <= 'f'))
            val = 10 + (ch - 'a');
        else if((ch >= 'A') && (ch <= 'F'))
            val = 10 + (ch - 'A');
        else
            continue;

        if(hi < 0)
        {
            hi = val;
        } else {
            if(count >= ETHER_ADDR_SIZE)
            {
                break;
            }
            mac[count++] = (UBYTE) ((hi << 4) | val);
            hi = -1;
        }
    }
    
    if(count != ETHER_ADDR_SIZE)
    {
        return(FALSE);
    }

    return(cdceth_mac_is_valid(mac));
}
/* \\\ */

static BOOL cdceth_load_mac(struct NepClassEth *ncp)
{
    struct PsdDescriptor *pdd;
    struct UsbCDCEthernetDesc *ethdesc = NULL;
    STRPTR macstr = NULL;
    UBYTE mac[ETHER_ADDR_SIZE];

    if(!(ncp->ncp_ControlInterface && ncp->ncp_EP0Pipe))
    {
        return(FALSE);
    }

    pdd = psdFindDescriptor(ncp->ncp_Device, NULL,
                            DDA_Interface, ncp->ncp_ControlInterface,
                            DDA_DescriptorType, UDT_CS_INTERFACE,
                            DDA_CS_SubType, UDST_CDC_ETHERNET,
                            TAG_END);
    if(!pdd)
    {
        return(FALSE);
    }

    psdGetAttrs(PGA_DESCRIPTOR, pdd,
                DDA_DescriptorData, &ethdesc,
                TAG_END);

    if(!ethdesc || !ethdesc->iMACAddress)
    {
        return(FALSE);
    }

    macstr = psdGetStringDescriptor(ncp->ncp_EP0Pipe, ethdesc->iMACAddress);
    if(macstr)
    {
        if(cdceth_parse_mac_string(macstr, mac))
        {
            CopyMem(mac, ncp->ncp_ROMAddress, ETHER_ADDR_SIZE);
            CopyMem(mac, ncp->ncp_MacAddress, ETHER_ADDR_SIZE);
            psdFreeVec(macstr);
            return(TRUE);
        }
        psdFreeVec(macstr);
    }

    return(FALSE);
}
/* \\\ */

static void cdceth_log_pipe_attrs(struct PsdPipe *pipe, BOOL allow_short, BOOL nak_timeout, ULONG nak_time, BOOL allow_runt)
{
    KPRINTF(5, ("Configured pipe 0x%08lx (allow_short=%ld nak_timeout=%ld nak_time=%lu allow_runt=%ld)\n",
                (void *) pipe, (LONG) allow_short, (LONG) nak_timeout, nak_time, (LONG) allow_runt));
}
/* \\\ */

/* The device's own multicast list (SET_ETHERNET_MULTICAST_FILTERS) is never
   loaded: any wanted multicast address asks for all multicast, and the stack
   filters what arrives (ECM 1.2 6.2.4). */
static UWORD cdceth_build_packet_filter(struct NepClassEth *ncp)
{
    UWORD filter = USB_CDC_PACKET_TYPE_DIRECTED | USB_CDC_PACKET_TYPE_BROADCAST;

    if(ncp->ncp_OpenFlags & SANA2OPF_PROM)
    {
        filter |= USB_CDC_PACKET_TYPE_PROMISCUOUS | USB_CDC_PACKET_TYPE_ALLMULTI;
    } else if(ncp->ncp_Multicasts.lh_Head->ln_Succ) {
        filter |= USB_CDC_PACKET_TYPE_ALLMULTI;
    }

    return(filter);
}
/* \\\ */

/* /// "nCDCRequest()" */
/* Sends a class request to the CDC control interface on EP0 and returns the
   pipe error. dir is URTF_IN or URTF_OUT. */
LONG nCDCRequest(struct NepClassEth *ncp, UBYTE dir, UBYTE request, UWORD value, APTR data, ULONG len)
{
    IPTR ifnum = 0;

    psdGetAttrs(PGA_INTERFACE, ncp->ncp_ControlInterface,
                IFA_InterfaceNum, &ifnum,
                TAG_END);

    /* psdPipeSetup(pipe, bmRequestType, bRequest, wValue, wIndex) */
    psdPipeSetup(ncp->ncp_EP0Pipe, dir|URTF_CLASS|URTF_INTERFACE,
                 request, value, ifnum);
    return psdDoPipe(ncp->ncp_EP0Pipe, data, len);
}
/* \\\ */

static BOOL cdceth_set_packet_filter(struct NepClassEth *ncp, UWORD filter)
{
    LONG ioerr;

    if(!(ncp->ncp_EP0Pipe && ncp->ncp_ControlInterface && ncp->ncp_FilterSupported))
    {
        KPRINTF(5, ("No packet filter on this function, skipping.\n"));
        return(FALSE);
    }

    KPRINTF(5, ("SET_ETHERNET_PACKET_FILTER wValue=0x%04lx\n", (ULONG) filter));

    ioerr = nCDCRequest(ncp, URTF_OUT, UCDC_SET_ETHERNET_PACKET_FILTER, filter, NULL, 0);
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "SET_ETHERNET_PACKET_FILTER failed: %s (%ld)",
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);

        /* (an EP0 protocol stall clears itself on the next SETUP - no
           CLEAR_FEATURE needed or meaningful for the default pipe) */
        return(FALSE);
    }

    KPRINTF(5, ("Packet filter configured\n"));
    return(TRUE);
}
/* \\\ */

static void cdceth_dump_interface(struct NepClassEth *ncp)
{
    IPTR ifcls = 0, ifsub = 0, ifproto = 0, ifalt = 0;
    IPTR ctrl_ifnum = -1;
    IPTR outmps = 0, inmps = 0;

    if(ncp->ncp_Interface)
    {
        psdGetAttrs(PGA_INTERFACE, ncp->ncp_Interface,
                    IFA_Class, &ifcls,
                    IFA_SubClass, &ifsub,
                    IFA_Protocol, &ifproto,
                    IFA_AlternateNum, &ifalt,
                    TAG_END);
    }

    if(ncp->ncp_ControlInterface)
    {
        psdGetAttrs(PGA_INTERFACE, ncp->ncp_ControlInterface,
                    IFA_InterfaceNum, &ctrl_ifnum,
                    TAG_END);
    }

    if(ncp->ncp_EPOut)
    {
        psdGetAttrs(PGA_ENDPOINT, ncp->ncp_EPOut,
                    EA_MaxPktSize, &outmps,
                    TAG_END);
    }

    if(ncp->ncp_EPIn)
    {
        psdGetAttrs(PGA_ENDPOINT, ncp->ncp_EPIn,
                    EA_MaxPktSize, &inmps,
                    TAG_END);
    }

    KPRINTF(5, ("CDC device 0x%08lx configured: iface 0x%08lx (cls/sub/proto %ld/%ld/%ld alt %ld) epIN 0x%08lx mps %ld epOUT 0x%08lx mps %ld\n",
                (void *) ncp->ncp_Device, (void *) ncp->ncp_Interface,
                ifcls, ifsub, ifproto, ifalt,
                (void *) ncp->ncp_EPIn, inmps,
                (void *) ncp->ncp_EPOut, outmps));

    KPRINTF(5, ("CDC MAC %02lx:%02lx:%02lx:%02lx:%02lx:%02lx flags 0x%08lx\n",
                (ULONG) ncp->ncp_MacAddress[0], (ULONG) ncp->ncp_MacAddress[1],
                (ULONG) ncp->ncp_MacAddress[2], (ULONG) ncp->ncp_MacAddress[3],
                (ULONG) ncp->ncp_MacAddress[4], (ULONG) ncp->ncp_MacAddress[5],
                (ULONG) ncp->ncp_StateFlags));
    KPRINTF(5, ("CDC control interface number %ld\n", (long) ctrl_ifnum));
}
/* \\\ */

BOOL nInitCDC(struct NepClassEth *ncp)
{
    if(!cdceth_load_mac(ncp))
    {
        cdceth_seed_mac(ncp);
    }
    return(TRUE);
}
/* \\\ */

void nUpdateRXMode(struct NepClassEth *ncp)
{
    cdceth_set_packet_filter(ncp, cdceth_build_packet_filter(ncp));
}
/* \\\ */

void nSetOnline(struct NepClassEth *ncp)
{
    ncp->ncp_StateFlags |= DDF_ONLINE;
    ncp->ncp_StateFlags &= ~DDF_OFFLINE;
    nDoEvent(ncp, S2EVENT_ONLINE);
}
/* \\\ */

static void cdceth_complete_write(struct NepClassEth *ncp, LONG ioerr, ULONG actual)
{
    struct IOSana2Req *ioreq;

    if((ioreq = ncp->ncp_WritePending))
    {
        KPRINTF(5, ("TX complete ioreq 0x%08lx err %ld len %ld actual %lu\n", (void *) ioreq, ioerr, (long) ioreq->ios2_DataLength, actual));
        if(ioerr)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Eth transmit failed: %s (%ld)",
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);

            /* Trigger any tx or generic error events */
            nDoEvent(ncp, S2EVENT_ERROR|S2EVENT_TX);

            /* Set error code and terminate the iorequest.
               NOTE: Can't use RC_* or deverror() this is not
               called from devBeginIO()!
            */
            ioreq->ios2_DataLength   = 0;
            ioreq->ios2_Req.io_Error = S2ERR_TX_FAILURE;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            psdDelayMS(50);
        } else {
            struct Sana2PacketTypeStats *stats = FindPacketTypeStats(ncp, ioreq->ios2_PacketType);
            if(stats)
            {
                stats->PacketsSent++;
                stats->BytesSent += (ULONG) ioreq->ios2_DataLength;
            }
            ncp->ncp_DeviceStats.PacketsSent++;
            ioreq->ios2_Req.io_Error = 0;
        }
        ReplyMsg((struct Message *) ioreq);
        ncp->ncp_WritePending = NULL;
    }
}
/* \\\ */

/* /// "nDoEvent()" */
void nDoEvent(struct NepClassEth *ncp, ULONG events)
{
    struct IOSana2Req *worknode, *nextnode;

    KPRINTF(1, ("DoEvent events: 0x%08lx\n", events));

    Forbid();
    /* Process pending S2_ONEVENT requests */
    worknode = (struct IOSana2Req *) ncp->ncp_EventList.lh_Head;
    while((nextnode = (struct IOSana2Req *) (((struct Node *) worknode)->ln_Succ)))
    {
        if(worknode->ios2_WireError & events)
        {
            Remove(&worknode->ios2_Req.io_Message.mn_Node);
            worknode->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
            KPRINTF(1, ("DoEvent: returned eventreq 0x%08lx\n", (void *) worknode));
            ReplyMsg(&worknode->ios2_Req.io_Message);
        }
        worknode = nextnode;
    }
    Permit();
}
/* \\\ */

/* /// "support routines" */
#define callfilter CallHookPkt
/* \\\ */

/* /// "nWritePacket()" */
BOOL nWritePacket(struct NepClassEth *ncp, struct IOSana2Req *ioreq)
{
    ULONG packettype;
    struct EtherPacketHeader *eph;
    UBYTE *copydest;
    UWORD writelen;
    ULONG payload_len;
    ULONG payload_offset = 0;
    ULONG total_len = 0;
    ULONG frame_len;
    struct BufMan *bufman;
    UBYTE *buf = ncp->ncp_WriteBuffer[ncp->ncp_WriteBufNum];

    packettype = ioreq->ios2_PacketType;
    writelen   = ioreq->ios2_DataLength;
    bufman     = ioreq->ios2_BufferManagement;

    frame_len = writelen;
    if(!(ioreq->ios2_Req.io_Flags & SANA2IOF_RAW))
    {
        frame_len += sizeof(struct EtherPacketHeader);
    }
    payload_len = frame_len < ETHER_MIN_LEN ? ETHER_MIN_LEN : frame_len;

    if(!cdceth_prepare_tx(ncp, payload_len, ncp->ncp_WriteBufSize,
                          &payload_offset, &total_len))
    {
        KPRINTF(10, ("writepacket: encapsulation setup failed!\n"));

        /* Trigger any tx, buff or generic error events */
        nDoEvent(ncp, S2EVENT_ERROR|S2EVENT_TX|S2EVENT_BUFF);

        ioreq->ios2_DataLength   = 0;
        ioreq->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
        ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
        ReplyMsg((struct Message *) ioreq);
        return FALSE;
    }

    eph = (struct EtherPacketHeader *) (buf + payload_offset);
    copydest = buf + payload_offset;

    if(!(ioreq->ios2_Req.io_Flags & SANA2IOF_RAW))
    {
        UWORD cnt;
        for(cnt = 0; cnt < ETHER_ADDR_SIZE; cnt++)
        {
            eph->eph_Dest[cnt] = ioreq->ios2_DstAddr[cnt];
            eph->eph_Src[cnt]  = ncp->ncp_MacAddress[cnt];
        }
        eph->eph_Type = AROS_WORD2BE(packettype);
        copydest += sizeof(struct EtherPacketHeader);
    }

    /* Dma not available, fallback to regular copy */
    if(!nCallS2Copy(bufman->bm_CopyFromBuf, copydest, ioreq->ios2_Data, ioreq->ios2_DataLength))
    {
        KPRINTF(10, ("writepacket: copyfrom returned failure!\n"));

        /* Trigger any tx, buff or generic error events */
        nDoEvent(ncp, S2EVENT_ERROR|S2EVENT_TX|S2EVENT_BUFF);

        /* Set error code and terminate the iorequest.
           NOTE: Can't use RC_* or deverror() this is not
           called from devBeginIO()! */
        ioreq->ios2_DataLength   = 0;
        ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
        ReplyMsg((struct Message *) ioreq);
        return FALSE;
    }

    if(payload_len > frame_len)
    {
        memset(buf + payload_offset + frame_len, 0, payload_len - frame_len);
    }

    cdceth_finalize_tx(ncp, buf, payload_offset, payload_len, total_len);

    KPRINTF(20, ("PktOut[%ld] %ld\n", ncp->ncp_WriteBufNum, (long) payload_len));

    /* Track the padded length so completion can account for bytes sent. */
    ioreq->ios2_DataLength = payload_len;
    ncp->ncp_WritePending = ioreq;

    /*
     * Submit the transfer asynchronously and let the pipe callback clear the
     * pending write so the queue keeps flowing even if the controller delays
     * completions.  If the pipe is already halted, the pending request will be
     * completed with the error when the pipe callback reports it.
     */
    psdSendPipe(ncp->ncp_EPOutPipe, buf, (ULONG) total_len);

    ncp->ncp_WriteBufNum ^= 1;

    DB(
        if(AROS_BE2WORD(eph->eph_Type) < ETHERPKT_SIZE)
        {
            KPRINTF(5, ("writepacket: %04lx%08lx > %04lx%08lx (IEEE802.3) len %lu, %lu bytes\n",
                        *((UWORD *) eph->eph_Src), *((ULONG *) (eph->eph_Src + 2)),
                        *((UWORD *) eph->eph_Dest), *((ULONG *) (eph->eph_Dest + 2)),
                        AROS_BE2WORD(eph->eph_Type), writelen));
        } else {
            KPRINTF(5, ("writepacket: %04lx%08lx > %04lx%08lx type %lu, %lu bytes\n",
                        *((UWORD *) eph->eph_Src), *((ULONG *) (eph->eph_Src + 2)),
                        *((UWORD *) eph->eph_Dest), *((ULONG *) (eph->eph_Dest + 2)),
                        AROS_BE2WORD(eph->eph_Type), writelen));
        }
        //dumpmem(buf, (ULONG) writelen);
    )

    return TRUE;
}
/* \\\ */

/* /// "nReadIOReq()" */
UWORD nReadIOReq(struct NepClassEth *ncp, struct EtherPacketHeader *eph, UWORD datasize, struct IOSana2Req *ioreq, UWORD flags)
{
    struct NepEthBase * nh = ncp->ncp_ClsBase;
    UBYTE *copyfrom;
    UWORD cnt;

    /* Handle RAW read */
    if(ioreq->ios2_Req.io_Flags & SANA2IOF_RAW)
    {
        /* ShapeShifter won't work with `sizeof(struct etherpacket_hdr)'
           here. This is most likely because it want the RAW ethernet
           packet checksum size (4) added to the packet size. */
        copyfrom = (UBYTE *) eph;
        datasize += sizeof(struct EtherPacketHeader) + 4;
    } else {
        copyfrom = (UBYTE *) (eph + 1);
    }

    /* Build up the ios2 structure enough so we can call the packet filter. */
    ioreq->ios2_PacketType = AROS_BE2WORD(eph->eph_Type);
    for(cnt = 0; cnt < ETHER_ADDR_SIZE; cnt++)
    {
        ioreq->ios2_SrcAddr[cnt] = eph->eph_Src[cnt];
        ioreq->ios2_DstAddr[cnt] = eph->eph_Dest[cnt];
    }
    ioreq->ios2_DataLength = datasize;
    /* Call the packet filter, if available. */
    if((flags & PACKETFILTER) &&
       (((struct BufMan *) ioreq->ios2_BufferManagement)->bm_PacketFilter) &&
       (!callfilter(((struct BufMan *) ioreq->ios2_BufferManagement)->bm_PacketFilter,
                   ioreq, copyfrom)))
    {
        /* This packet got dropped! */
        KPRINTF(7, ("readioreq: packet type %lu for ioreq 0x%08lx dropped\n",
                AROS_BE2WORD(eph->eph_Type), (void *) ioreq));
        return flags;
    }


    /* Ok, the packet didn't get dropped, set the BCAST and MCAST
       flags according to dstaddr. */

    /* Address == Multicast? */
    if(ioreq->ios2_DstAddr[0] & 1)
    {
        /* Address == Broadcast? */
        static const UBYTE bcast[ETHER_ADDR_SIZE] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        if(memcmp(bcast, ioreq->ios2_DstAddr, ETHER_ADDR_SIZE) == 0)
        {
            ioreq->ios2_Req.io_Flags |= SANA2IOF_BCAST;
        } else {
            ioreq->ios2_Req.io_Flags |= SANA2IOF_MCAST;
        }
    }

    /* Finally copy the packet data! */
    if(nCallS2Copy(((struct BufMan *) ioreq->ios2_BufferManagement)->bm_CopyToBuf,
                   ioreq->ios2_Data, copyfrom, ioreq->ios2_DataLength))
    {
        DB(
        KPRINTF(5, ("readioreq: copytobuffed packet ior 0x%08lx, %04lx%08lx < %04lx%08lx, type %lu, %lu bytes, %s%s%s\n",
                    (void *) ioreq,
                    *((UWORD *) ioreq->ios2_DstAddr), *((ULONG *) (ioreq->ios2_DstAddr + 2)),
                    *((UWORD *) ioreq->ios2_SrcAddr), *((ULONG *) (ioreq->ios2_SrcAddr + 2)),
                    ioreq->ios2_PacketType, ioreq->ios2_DataLength,
                    (ioreq->ios2_Req.io_Flags & SANA2IOF_RAW) ? "RAW " : "",
                    (ioreq->ios2_Req.io_Flags & SANA2IOF_BCAST) ? "BCAST " : "",
                    (ioreq->ios2_Req.io_Flags & SANA2IOF_MCAST) ? "MCAST " : ""));
        //dumpmem(copyfrom, ioreq->ios2_DataLength);
        )

        /* Clear the dropped flag */
        flags &= ~DROPPED;
    } else {
        KPRINTF(10, ("readioreq: copyto returned failure!\n"));

        /* Trigger any rx, buff or generic error events */
        nDoEvent(ncp, S2EVENT_ERROR|S2EVENT_RX|S2EVENT_BUFF);

        /* Set error code.
           NOTE: Can't use RC_* or deverror() this is not called from devBeginIO()!
        */
        ioreq->ios2_DataLength   = 0;
        ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
    }

    /* Pull the ioreq off the list & terminate it */
    Forbid();
    Remove((struct Node *) ioreq);
    Permit();
    ReplyMsg((struct Message *) ioreq);
    return flags;
}
/* \\\ */

/* /// "nReadPacket()" */
BOOL nReadPacket(struct NepClassEth *ncp, UBYTE *pktptr, ULONG pktlen)
{
    struct EtherPacketHeader *eph;
    struct BufMan *bufman;
    struct IOSana2Req *worknode, *nextnode;
    struct Sana2PacketTypeStats *stats;
    UWORD flags;
    UWORD datasize;
    BOOL ret = FALSE;

    KPRINTF(20, ("PktIn [%ld] %ld\n", ncp->ncp_ReadBufNum, pktlen));

    if(pktlen < sizeof(struct EtherPacketHeader))
    {
        ncp->ncp_DeviceStats.BadData++;
        return FALSE;
    }

    ncp->ncp_DeviceStats.PacketsReceived++;
    eph = (struct EtherPacketHeader *) pktptr;
    stats = FindPacketTypeStats(ncp, (ULONG) AROS_BE2WORD(eph->eph_Type));
    flags = DROPPED|PACKETFILTER;

    /* Calculate size of the actual data */
    datasize = pktlen - sizeof(struct EtherPacketHeader);

    /* Is the packet datasize valid? */
    if(pktlen <= ETHER_MAX_LEN)
    {
        /* Update the packet statistics */
        if(stats)
        {
            stats->PacketsReceived++;
            stats->BytesReceived += datasize;  /* NOTE: don't include headers */
        }
        /* For each device user (bufman)
           NOTE: We absolutely *MUST* try to offer the packet to *all*
           different device users (SANA-II V2 spec requirement). */
        Forbid();
        bufman = (struct BufMan *) ncp->ncp_BufManList.lh_Head;
        while(((struct Node *) bufman)->ln_Succ)
        {
            /* For each queued read request (ioreq) */
            worknode = (struct IOSana2Req *) bufman->bm_RXQueue.lh_Head;
            while((nextnode = (struct IOSana2Req *) (((struct Node *) worknode)->ln_Succ)))
            {
                /* Check the packet type. Also handles 802.3 packets. */
                if((worknode->ios2_PacketType == AROS_BE2WORD(eph->eph_Type)) ||
                   ((AROS_BE2WORD(eph->eph_Type) < ETHERPKT_SIZE) && (worknode->ios2_PacketType < ETHERPKT_SIZE)))
                {
                    flags = nReadIOReq(ncp, eph, datasize, worknode, flags);
                    /* Break out - let other callers get the packet too */
                    break;
                }
                worknode = nextnode;
            }
            bufman = (struct BufMan *) (((struct Node *) bufman)->ln_Succ);
        }
        Permit();
        /* Now we've tried to give the packet to every CMD_READ caller.
           If DROPPED is set at this point no-one wanted this packet. */
        if(flags & DROPPED)
        {
            /* So there were no outstanding CMD_READs or the packet wasn't
               accepted by any of them. Okay, check if we have any pending
               S2_READORPHAN ioreq in list and if we have return this packet
               with it. Note that packet filter must not be used for this
               time!

               NOTE: orphanlist is global, ie. only one caller will get the
               packet if multiple users have pending S2_READORPHANs.
            */

            /* Process pending orphanread iorequs */
            Forbid();
            worknode = (struct IOSana2Req *) ncp->ncp_OrphanQueue.lh_Head;
            while((nextnode = (struct IOSana2Req *) (((struct Node *) worknode)->ln_Succ)))
            {
                nReadIOReq(ncp, eph, datasize, worknode, 0);
                worknode = nextnode;
            }
            Permit();
        } else {
            /* Packet not dropped - return ok */
            ret = TRUE;
        }
    } else {
        KPRINTF(20, ("Pktlen %ld invalid!\n", pktlen));
        ncp->ncp_DeviceStats.BadData++;
    }
    if(!ret)
    {
        /* Update global dropped packet counter. */
        ncp->ncp_DeviceStats.UnknownTypesReceived++;

        /* Update dropped packet statistics. */
        if(stats)
        {
            stats->PacketsDropped++;
        }
        KPRINTF(9, ("readpacket: packet type %lu dropped\n", AROS_BE2WORD(eph->eph_Type)));

        /* Trigger any rx or generic error events */
        nDoEvent(ncp, S2EVENT_ERROR|S2EVENT_RX);
    }
    return ret;
}
/* \\\ */

/**************************************************************************/

/* /// "nGUITask()" */
void nGUITask()
{

    struct Task *thistask;
    struct NepEthBase *nh;
    struct NepClassEth *ncp;
    struct PsdIFFContext *pic;

    thistask = FindTask(NULL);
#undef ps
#define ps ncp->ncp_PsdBase
#undef IntuitionBase
#define IntuitionBase ncp->ncp_IntBase
#undef MUIMasterBase
#define MUIMasterBase ncp->ncp_MUIBase

    ncp = thistask->tc_UserData;
    nh = ncp->ncp_ClsBase;

    ++nh->nh_Library.lib_OpenCnt;
    if(!(MUIMasterBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN)))
    {
        KPRINTF(10, ("Couldn't open muimaster.library.\n"));
        nGUITaskCleanup(ncp);
        return;
    }

    if(!(IntuitionBase = OpenLibrary("intuition.library", 39)))
    {
        KPRINTF(10, ("Couldn't open intuition.library.\n"));
        nGUITaskCleanup(ncp);
        return;
    }
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        KPRINTF(10, ("Couldn't open neptune.library.\n"));
        nGUITaskCleanup(ncp);
        return;
    }

    ncp->ncp_App = ApplicationObject,
        MUIA_Application_Title      , (IPTR)libname,
        MUIA_Application_Version    , (IPTR)VERSION_STRING,
        MUIA_Application_Copyright  , (IPTR)"2025 The AROS Development Team.",
        MUIA_Application_Author     , (IPTR)"The AROS Development Team",
        MUIA_Application_Description, (IPTR)"Settings for the " CLASS_NAME,
        MUIA_Application_Base       , (IPTR)"CDCETH",
        MUIA_Application_HelpFile   , (IPTR)"HELP:Neptune.guide",
        MUIA_Application_Menustrip  , (IPTR)MenustripObject,
            Child, (IPTR)MenuObjectT((IPTR)"Project"),
                Child, (IPTR)(ncp->ncp_AboutMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"About...",
                    MUIA_Menuitem_Shortcut, (IPTR)"?",
                    End),
                End,
            Child, (IPTR)MenuObjectT((IPTR)"Settings"),
                Child, (IPTR)(ncp->ncp_UseMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"Save",
                    MUIA_Menuitem_Shortcut, (IPTR)"S",
                    End),
                Child, (IPTR)(ncp->ncp_SetDefaultMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"Set as Default",
                    MUIA_Menuitem_Shortcut, (IPTR)"D",
                    End),
                Child, (IPTR)MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)NM_BARLABEL,
                    End,
                Child, (IPTR)(ncp->ncp_MUIPrefsMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"MUI Settings",
                    MUIA_Menuitem_Shortcut, (IPTR)"M",
                    End),
                End,
            End,

        SubWindow, (IPTR)(ncp->ncp_MainWindow = WindowObject,
            MUIA_Window_ID   , MAKE_ID('M','A','I','N'),
            MUIA_Window_Title, (IPTR)libname,
            MUIA_HelpNode, (IPTR)libname,

            WindowContents, (IPTR)VGroup,
                Child, (IPTR)ColGroup(2), GroupFrameT((IPTR)(ncp->ncp_Interface ? "Device Settings" : "Default Device Settings")),
                    //Child, HSpace(0),
                    Child, (IPTR)Label((IPTR) "Media Type:"),
                    Child, (IPTR)(ncp->ncp_MediaTypeObj = CycleObject,
                        MUIA_CycleChain, 1,
                        MUIA_Cycle_Entries, (IPTR)MediaTypeStrings,
                        MUIA_Cycle_Active, ncp->ncp_CDC->cdc_MediaType,
                        End),
                    Child, (IPTR)Label((IPTR) "Default " DEVNAME " Unit:"),
                    Child, (IPTR)(ncp->ncp_UnitObj = StringObject,
                        StringFrame,
                        MUIA_CycleChain, 1,
                        MUIA_String_AdvanceOnCR, TRUE,
                        MUIA_String_Integer, ncp->ncp_CDC->cdc_DefaultUnit,
                        MUIA_String_Accept, (IPTR)"0123456789",
                        End),
                    End,
                Child, (IPTR)VSpace(0),
                Child, (IPTR)HGroup,
                    MUIA_Group_SameWidth, TRUE,
                    Child, (IPTR)(ncp->ncp_UseObj = TextObject, ButtonFrame,
                        MUIA_ShowMe, (IPTR)ncp->ncp_Interface,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Save ",
                        End),
                    Child, (IPTR)(ncp->ncp_SetDefaultObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)(ncp->ncp_Interface ? "\33c Save as Default " : "\33c Save Defaults "),
                        End),
                    Child, (IPTR)(ncp->ncp_CloseObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Use ",
                        End),
                    End,
                End,
            End),
        End;

    if(!ncp->ncp_App)
    {
        KPRINTF(10, ("Couldn't create application\n"));
        nGUITaskCleanup(ncp);
        return;
    }

    DoMethod(ncp->ncp_MainWindow, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             ncp->ncp_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(ncp->ncp_UseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncp->ncp_App, 2, MUIM_Application_ReturnID, ID_STORE_CONFIG);
    DoMethod(ncp->ncp_SetDefaultObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncp->ncp_App, 2, MUIM_Application_ReturnID, ID_DEF_CONFIG);
    DoMethod(ncp->ncp_CloseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncp->ncp_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

    DoMethod(ncp->ncp_AboutMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncp->ncp_App, 2, MUIM_Application_ReturnID, ID_ABOUT);
    DoMethod(ncp->ncp_UseMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncp->ncp_App, 2, MUIM_Application_ReturnID, ID_STORE_CONFIG);
    DoMethod(ncp->ncp_SetDefaultMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncp->ncp_App, 2, MUIM_Application_ReturnID, ID_DEF_CONFIG);
    DoMethod(ncp->ncp_MUIPrefsMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncp->ncp_App, 2, MUIM_Application_OpenConfigWindow, 0);
    {
        IPTR  isopen = 0;
        IPTR  iconify = 0;
        ULONG sigs;
        ULONG sigmask;
        LONG retid;

        get(ncp->ncp_App, MUIA_Application_Iconified, &iconify);
        set(ncp->ncp_MainWindow, MUIA_Window_Open, TRUE);
        get(ncp->ncp_MainWindow, MUIA_Window_Open, &isopen);
        if(!(isopen || iconify))
        {
            nGUITaskCleanup(ncp);
            return;
        }
        sigmask = 0;
        do
        {
            retid = DoMethod(ncp->ncp_App, MUIM_Application_NewInput, &sigs);
            switch(retid)
            {
                case ID_DEF_CONFIG:
                case ID_STORE_CONFIG:
                case MUIV_Application_ReturnID_Quit:
                {
                    get(ncp->ncp_UnitObj, MUIA_String_Integer, &ncp->ncp_CDC->cdc_DefaultUnit);
                    get(ncp->ncp_MediaTypeObj, MUIA_Cycle_Active, &ncp->ncp_CDC->cdc_MediaType);

                    if(retid == ID_DEF_CONFIG)
                    {
                        pic = psdGetClsCfg(libname);
                        if(!pic)
                        {
                            psdSetClsCfg(libname, NULL);
                            pic = psdGetClsCfg(libname);
                        }
                        if(pic)
                        {
                            psdAddCfgEntry(pic, ncp->ncp_CDC);
                            psdSaveCfgToDisk(NULL);
                        }
                    }
                    if(ncp->ncp_Interface)
                    {
                        pic = psdGetUsbDevCfg(libname, ncp->ncp_DevIDString, NULL);
                        if(!pic)
                        {
                            psdSetUsbDevCfg(libname, ncp->ncp_DevIDString, NULL, NULL);
                            pic = psdGetUsbDevCfg(libname, ncp->ncp_DevIDString, NULL);
                        }
                        if(pic)
                        {
                            if(psdAddCfgEntry(pic, ncp->ncp_CDC))
                            {
                                if(retid != MUIV_Application_ReturnID_Quit)
                                {
                                    psdSaveCfgToDisk(NULL);
                                }
                                retid = MUIV_Application_ReturnID_Quit;
                            }
                        }
                    } else {
                        retid = MUIV_Application_ReturnID_Quit;
                    }
                    break;
                }

                case ID_ABOUT:
                    NEP_CLASS_ABOUT(ncp->ncp_App, ncp->ncp_MainWindow);
                    break;
            }
            if(retid == MUIV_Application_ReturnID_Quit)
            {
                break;
            }
            if(sigs)
            {
                sigs = Wait(sigs|sigmask|SIGBREAKF_CTRL_C);
                if(sigs & SIGBREAKF_CTRL_C)
                {
                    break;
                }
            }
        } while(TRUE);
        set(ncp->ncp_MainWindow, MUIA_Window_Open, FALSE);
    }
    nGUITaskCleanup(ncp);
    
}
/* \\\ */

/* /// "nGUITaskCleanup()" */
void nGUITaskCleanup(struct NepClassEth *ncp)
{
    if(ncp->ncp_App)
    {
        MUI_DisposeObject(ncp->ncp_App);
        ncp->ncp_App = NULL;
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
    if(ps)
    {
        CloseLibrary(ps);
        ps = NULL;
    }
    Forbid();
    ncp->ncp_GUIBinding = NULL;
    ncp->ncp_GUITask = NULL;
    if(ncp->ncp_ReadySigTask)
    {
        Signal(ncp->ncp_ReadySigTask, 1L<<ncp->ncp_ReadySignal);
    }
    --ncp->ncp_ClsBase->nh_Library.lib_OpenCnt;
}
/* \\\ */

/* /// "CDC Ethernet helpers" */

BOOL cdceth_is_cdc_ethernet(uint8_t cls, uint8_t subcls, uint8_t proto)
{
    switch(subcls)
    {
        case CDC_SUBCLASS_ETHERNET:   /* ECM */
        case CDC_SUBCLASS_EEM:        /* EEM */
        case CDC_SUBCLASS_NCM:        /* NCM */
            break;
        default:
            return(FALSE);
    }

    if(cls != CDC_CLASS_COMMUNICATION)
        return(FALSE);

    /* Protocol byte varies between ECM (0), NCM (0/1), and EEM (7). */
    if(proto != 0 && proto != 1 && proto != 7)
        return(FALSE);

    return(TRUE);
}

/* \\\ */
