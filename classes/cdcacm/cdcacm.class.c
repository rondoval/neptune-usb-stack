/*
 *----------------------------------------------------------------------------
 *                         cdcacm class for Neptune
 *----------------------------------------------------------------------------
 * Based on the Poseidon cdcacm class by Chris Hodges <chrisly@platon42.de>.
 */

#include "debug.h"
#include "cdcacm.class.h"

/* /// "Lib Stuff" */
static const STRPTR libname = CLASS_NAME;

/* LVO table for the embedded usbmodem.device (MakeLibrary'd in libInit). Order 1..6. */
static const APTR DevFuncTable[] =
{
    (APTR) devOpen,
    (APTR) devClose,
    (APTR) devExpunge,
    (APTR) devReserved,
    (APTR) devBeginIO,
    (APTR) devAbortIO,
    (APTR) -1,
};

/* Address-only sentinel ("fake massstorage binding"); never dereferenced. Explicit
   initializer keeps this const in .rodata rather than a zero-init .bss common symbol. */
static const struct NepClassSerial fake_binding = {0};

/* Read-stream setup, see the psdOpenStreamA() call in nAllocSerial(). */
static const struct TagItem ReadStreamTags[] =
{
    { PSA_ReadAhead,        TRUE            },
    { PSA_BufferedRead,     TRUE            },
    { PSA_NumPipes,         NUMREADPIPES    },
    { PSA_BufferSize,       DEFREADBUFLEN   },
    { PSA_AllowRuntPackets, TRUE            },
    { PSA_DoNotWait,        TRUE            },
    { PSA_AbortSigMask,     SIGBREAKF_CTRL_C},
    { TAG_END,              0               }
};

int libInit(struct NepSerialBase * nh)
{
    struct NepSerialBase *ret = NULL;

    if(nDevNameTaken(DEVNAME))
    {
        return FALSE;
    }

    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", nh, EXEC_BASE_NAME));

    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);

#define UtilityBase nh->nh_UtilityBase

    if(UtilityBase)
    {
        NewList(&nh->nh_Units);

        if((nh->nh_DevBase = (struct NepSerDevBase *) MakeLibrary((APTR) DevFuncTable, NULL, (APTR) devInit,
           sizeof(struct NepSerDevBase), NULL)))
        {
            nh->nh_DevBase->np_ClsBase = nh;
            Forbid();
            AddDevice((struct Device *) nh->nh_DevBase);
            nh->nh_DevBase->np_Library.lib_OpenCnt++;
            Permit();
            ret = nh;
        } else {
            KPRINTF(20, ("failed to create cdcacm.device\n"));
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

int libExpunge(struct NepSerialBase * nh)
{
    struct NepClassSerial *ncp;

    KPRINTF(10, ("libExpunge nh: 0x%08lx\n", nh));

    if(nh->nh_DevBase->np_Library.lib_OpenCnt == 1)
    {
        KPRINTF(1, ("libExpunge: closelibrary utilitybase 0x%08lx\n",
                    UtilityBase));
        CloseLibrary((struct Library *) UtilityBase);

        ncp = (struct NepClassSerial *) nh->nh_Units.lh_Head;
        while(ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ)
        {
            Remove((struct Node *) ncp);
            FreeVec(ncp);
            ncp = (struct NepClassSerial *) nh->nh_Units.lh_Head;
        }

        nh->nh_DevBase->np_Library.lib_OpenCnt--;
        RemDevice((struct Device *) nh->nh_DevBase);

        KPRINTF(5, ("libExpunge: Unloading done! cdcacm.usbclass expunged!\n\n"));
    } else {
        KPRINTF(5, ("libExpunge: Could not expunge, LIBF_DELEXP set!\n"));
        return(FALSE);
    }

    return(TRUE);
}

/* \\\ */

void SendBulk( struct PsdDevice *pd,UBYTE *cmd,ULONG len)
{
    struct Library *ps;
    struct MsgPort *mp;
    struct PsdPipe *pp;
    struct PsdInterface *DataIf = 0;
    struct PsdEndpoint *EPOut;
    IPTR EPnum=0,IFnum=0,IFEPnum=0;

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        KPRINTF(10, ("SendBulk: FindInterface..."));
        if( ( DataIf = psdFindInterface( pd , DataIf , TAG_END ) ) ){

            psdGetAttrs(PGA_INTERFACE,DataIf,
                        IFA_InterfaceNum,&IFnum,
                        IFA_NumEndpoints,&IFEPnum,
                        TAG_END);
            KPRINTF(10, (" ...OK num:%ld  number of endpoints:%ld\n",IFnum,IFEPnum));

            KPRINTF(10, ("Find BULK OUT Endpoint..."));
            EPOut = psdFindEndpoint( DataIf, NULL,
                                    EA_IsIn, FALSE,
                                    EA_TransferType, USEAF_BULK,
                                    TAG_END);
            if( EPOut ){
                psdGetAttrs(PGA_ENDPOINT,EPOut,
                            EA_EndpointNum, &EPnum,
                            TAG_END);
                KPRINTF(10, (" ...OK address:%ld\n",EPnum));

                if((mp = CreateMsgPort())){
                    KPRINTF(10, ("OpenPipe...\n"));
                    if((pp = psdAllocPipe(pd, mp, EPOut))){
                        psdSetAttrs(PGA_PIPE, pp,
                                    PPA_NakTimeout,TRUE,
                                    PPA_NakTimeoutTime, 5000,
                                    TAG_END);
                        KPRINTF(10, ("Write %ld bytes ...\n",len));
                        KPRINTF(10, ("Error = %ld\n", psdDoPipe( pp , cmd , len )));
                        psdFreePipe(pp);
                    }
                    DeleteMsgPort(mp);
                }
            }
        }
        CloseLibrary(ps);
    }
}

/*
 * ***********************************************************************
 * * Library functions                                                   *
 * ***********************************************************************
 */

/* /// "usbAttemptInterfaceBinding()" */
struct NepClassSerial * usbAttemptInterfaceBinding(struct NepSerialBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    IPTR ifclass;
    IPTR subclass;
    IPTR proto;
    IPTR NumEndpoints;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    IPTR prodid;
    IPTR vendid;
    IPTR numintf;
    struct PsdPipe *pp;
    struct MsgPort *mp;

    KPRINTF(1, ("nepSerialAttemptInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {

        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Class, &ifclass,
                    IFA_SubClass, &subclass,
                    IFA_Protocol, &proto,
                    IFA_NumEndpoints, &NumEndpoints,
                    IFA_Config, &pc,
                    TAG_DONE);

        psdGetAttrs(PGA_CONFIG, pc,
                    CA_Device, &pd,
                    CA_NumInterfaces, &numintf,
                    TAG_END);

        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ProductID, &prodid,
                    DA_VendorID, &vendid,
                    TAG_END);

//      bug("cdcacm.usbclass:AttemptInterfaceBinding vendor id=%lx  product id=%lx\n",vendid,prodid);

        // ZTE ModeSwitch
        if( vendid == 0x19d2 && prodid == 0x2000 ){

            UBYTE cmd[31];
            // from BSD u3g.c
            memset(cmd, 0, sizeof(cmd));
            cmd[0] = 0x55;
            cmd[1] = 0x53;
            cmd[2] = 0x42;
            cmd[3] = 0x43;
            cmd[4] = 0x01;
            cmd[14] = 0x06;
            cmd[15] = 0x1b;
            cmd[19] = 0x02;

            SendBulk(pd,cmd,sizeof(cmd));

            CloseLibrary(ps);
            if( ifclass == MASSSTORE_CLASSCODE ){
                KPRINTF(10, ("cdcacm.usbclass: fake massstorage binding\n"));
                return (struct NepClassSerial *)&fake_binding;
            }
            return(NULL);
        }

        // huawei ModeSwitch
        if( (vendid == 0x12d1 ) && ( numintf < 4 ) && (
                                    prodid == 0x1001 ||    // e169
                                    prodid == 0x1003       // e220
            )){
                if((mp = CreateMsgPort()))
                {
                    if((pp = psdAllocPipe(pd, mp, NULL)))
                    {
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                        "Huawei ModeSwitch...");
                        psdPipeSetup(pp, URTF_STANDARD|URTF_DEVICE,
                                        USR_SET_FEATURE, UFS_DEVICE_REMOTE_WAKEUP, 0);
                        psdDoPipe(pp, NULL, 0);
                        psdFreePipe(pp);
                    }
                    DeleteMsgPort(mp);
                }
                CloseLibrary(ps);
                if( ifclass == MASSSTORE_CLASSCODE ){
                    KPRINTF(10, ("cdcacm.usbclass: fake massstorage binding\n"));
                    return (struct NepClassSerial *)&fake_binding;
                }
                return(NULL);
        }


        //  Huawei, e122  and many others.
        if( (vendid == 0x12d1 ) && ( prodid == 0x1446 )){

            UBYTE magic_cmd[31];
            //  magic command from BSD u3g driver sources.
            memset(magic_cmd, 0, sizeof(magic_cmd));
            magic_cmd[0] = 0x55;
            magic_cmd[1] = 0x53;
            magic_cmd[2] = 0x42;
            magic_cmd[3] = 0x43;
            magic_cmd[15]= 0x11;
            magic_cmd[16]= 0x06;

            SendBulk(pd,magic_cmd,sizeof(magic_cmd));

            CloseLibrary(ps);
            if( ifclass == MASSSTORE_CLASSCODE ){
                KPRINTF(10, ("cdcacm.usbclass: fake massstorage binding\n"));
                return (struct NepClassSerial *)&fake_binding;
            }
            return(NULL);
        }

        CloseLibrary(ps);

        // ZTE
        if( (vendid == 0x19d2) && (
            prodid == 0x0001 ||
            prodid == 0x0002 ||
            prodid == 0x0015 ||
            prodid == 0x0016 ||
            prodid == 0x0017 ||
            prodid == 0x0031 ||
            prodid == 0x0037 ||
            prodid == 0x0052 ||
            prodid == 0x0055 ||
            prodid == 0x0063 ||
            prodid == 0x0064 ||
            prodid == 0x0108 ||
            prodid == 0x0128
        )){
            if( ifclass == MASSSTORE_CLASSCODE ){
                KPRINTF(10, ("cdcacm.usbclass: fake massstorage binding\n"));
                return (struct NepClassSerial *)&fake_binding;
            }

            if((ifclass == 255) && (subclass == 255) && (proto == 255) && (NumEndpoints == 3)){
                return(usbForceInterfaceBinding(nh, pif));
            }
        }


        // Huawei
        if( (vendid == 0x12d1) && (
            prodid == 0x1001 ||    // e169
            prodid == 0x1003 ||    // e220
            prodid == 0x1406 ||
            prodid == 0x140b ||
            prodid == 0x140c ||
            prodid == 0x1412 ||
            prodid == 0x141b ||
            prodid == 0x14ac
        )){
            if( ifclass == MASSSTORE_CLASSCODE ){
                KPRINTF(10, ("cdcacm.usbclass: fake massstorage binding\n"));
                return (struct NepClassSerial *)&fake_binding;
            }

            if((ifclass == 255) && (subclass == 255) && (proto == 255) && (NumEndpoints == 3)){
                return(usbForceInterfaceBinding(nh, pif));
            }
        }

        // Novatel (Toshiba) wireless HSDPA modem
        if( (vendid == 0x0930) && (prodid == 0x1302) )
        {
            return(usbForceInterfaceBinding(nh, pif));
        }

        if((ifclass == CDCCTRL_CLASSCODE) &&
           (subclass == CDC_ACM_SUBCLASS) &&
           //(proto == CDC_PROTO_HAYES)
           ((proto >= 1) && (proto <= 6))) //found in some linux driver source
        {
            return(usbForceInterfaceBinding(nh, pif));
        }

        if((ifclass == CDCCTRL_CLASSCODE) &&
           (subclass == CDC_OBEX_SUBCLASS))
        {
            return(usbForceInterfaceBinding(nh, pif));
        }

    }

    return(NULL);
}

/* \\\ */

/* /// "usbForceInterfaceBinding()" */
struct NepClassSerial * usbForceInterfaceBinding(struct NepSerialBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    struct NepClassSerial *ncp;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    STRPTR devname;
    IPTR altifnum;
    IPTR ifnum;
    IPTR prodid;
    IPTR vendid;
    ULONG unitno;
    IPTR subclass;
    BOOL  unitfound;
    UBYTE buf[64];
    struct Task *tmptask;

    KPRINTF(1, ("nepSerialForceInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_InterfaceNum, &ifnum,
                    IFA_AlternateNum, &altifnum,
                    IFA_SubClass, &subclass,
                    IFA_Config, &pc,
                    TAG_DONE);
        psdGetAttrs(PGA_CONFIG, pc,
                    CA_Device, &pd,
                    TAG_END);
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ProductID, &prodid,
                    DA_VendorID, &vendid,
                    DA_ProductName, &devname,
                    TAG_END);
        Forbid();
        /* Find next free unit number */
        unitno = 0;
        ncp = (struct NepClassSerial *) nh->nh_Units.lh_Head;
        while(ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ)
        {
            if(ncp->ncp_UnitNo == unitno)
            {
                unitno++;
                ncp = (struct NepClassSerial *) nh->nh_Units.lh_Head;
            } else {
                ncp = (struct NepClassSerial *) ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ;
            }
        }
        unitfound = FALSE;
        ncp = (struct NepClassSerial *) nh->nh_Units.lh_Head;
        while(ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ)
        {
            if((ncp->ncp_UnitAltIfNum == altifnum) && (ncp->ncp_UnitIfNum == ifnum) &&
               (ncp->ncp_UnitProdID == prodid) && (ncp->ncp_UnitVendorID == vendid))
            {
                unitno = ncp->ncp_UnitNo;
                unitfound = TRUE;
                break;
            }
            ncp = (struct NepClassSerial *) ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Succ;
        }
        if(!unitfound)
        {
            /* as units are freed in the expunge-vector, the memory is
               outside the scope of neptune.library */
            if(!(ncp = AllocVec(sizeof(struct NepClassSerial), MEMF_PUBLIC|MEMF_CLEAR)))
            {
                Permit();
                CloseLibrary(ps);
                return(NULL);
            }
            /* IORequests may be queued even if the task is gone. */
            NewList(&ncp->ncp_Unit.unit_MsgPort.mp_MsgList);
            NewList(&ncp->ncp_ReadQueue);
            NewList(&ncp->ncp_WriteQueue);
            AddTail(&nh->nh_Units, &ncp->ncp_Unit.unit_MsgPort.mp_Node);
            ncp->ncp_DenyRequests = TRUE;
        }
        ncp->ncp_UnitNo = unitno;
        ncp->ncp_Interface = pif;
        ncp->ncp_Device = pd;
        ncp->ncp_UnitAltIfNum = altifnum;
        ncp->ncp_UnitIfNum = ifnum;
        ncp->ncp_UnitProdID = prodid;
        ncp->ncp_UnitVendorID = vendid;
        Permit();

        psdSafeRawDoFmt(buf, 64, CLASS_NAME "<%08lx>", ncp);
        ncp->ncp_ReadySignal = SIGB_SINGLE;
        ncp->ncp_ReadySigTask = FindTask(NULL);
        SetSignal(0, SIGF_SINGLE);
        if((tmptask = psdSpawnSubTask(buf, nSerialTask, ncp)))
        {
            psdBorrowLocksWait(tmptask, 1UL<<ncp->ncp_ReadySignal);
            //Wait(1UL<<ncp->ncp_ReadySignal);
            if(ncp->ncp_Task)
            {
                ncp->ncp_ReadySigTask = NULL;
                //FreeSignal(ncp->ncp_ReadySignal);
                if(subclass != CDC_OBEX_SUBCLASS)
                {
                    KPRINTF(10, ("Modem '%s' at %s unit %ld!\n",
                                   devname, nh->nh_DevBase->np_Library.lib_Node.ln_Name,
                                   ncp->ncp_UnitNo));
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   NEP_BOUND_TXT,
                                   devname, nh->nh_DevBase->np_Library.lib_Node.ln_Name,
                                   ncp->ncp_UnitNo);
                } else {
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   NEP_BOUND_TXT,
                                   devname, nh->nh_DevBase->np_Library.lib_Node.ln_Name,
                                   ncp->ncp_UnitNo);
                }
                CloseLibrary(ps);
                return(ncp);
            }
        }
        ncp->ncp_ReadySigTask = NULL;
        //FreeSignal(ncp->ncp_ReadySignal);
        /* Get rid of unit structure */
        /*Forbid();
        Remove((struct Node *) ncp);
        FreeVec(ncp);
        Permit();*/
        CloseLibrary(ps);
    }
    return(NULL);
}
/* \\\ */

/* /// "usbReleaseInterfaceBinding()" */
void usbReleaseInterfaceBinding(struct NepSerialBase *nh, struct NepClassSerial *ncp)
{
    struct Library *ps;
    STRPTR devname;

    KPRINTF(1, ("nepSerialReleaseInterfaceBinding(%08lx)\n", ncp));

    if( ncp == &fake_binding ){
        KPRINTF(10, ("cdcacm.usbclass: fake massstorage binding released\n"));
        return;
    }

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
            Wait(1UL<<ncp->ncp_ReadySignal);
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
LONG (usbGetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepSerialBase * nh asm("a6"))
{

    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("nepHidGetAttrsA(%ld, %08lx, %08lx)\n", type, usbstruct, tags));
    switch(type)
    {
        case UGA_CLASS:
             if((ti = FindTagItem(UCCA_Priority, tags)))
             {
                 *((SIPTR *) ti->ti_Data) = 1;
                 count++;
             }
             if((ti = FindTagItem(UCCA_Description, tags)))
             {
                 *((STRPTR *) ti->ti_Data) = "USB modems and OBEX via usbmodem.device";
                 count++;
             }
             if((ti = FindTagItem(UCCA_HasClassCfgGUI, tags)))
             {
                 *((IPTR *) ti->ti_Data) = FALSE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_HasBindingCfgGUI, tags)))
             {
                 *((IPTR *) ti->ti_Data) = FALSE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_AfterDOSRestart, tags)))
             {
                 *((IPTR *) ti->ti_Data) = FALSE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_UsingDefaultCfg, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             break;

         case UGA_BINDING:
             if((ti = FindTagItem(UCBA_UsingDefaultCfg, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             break;
    }
    return(count);
}
/* \\\ */

/* /// "usbSetAttrsA()" */
LONG (usbSetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepSerialBase * nh asm("a6"))
{
    return(0);
}
/* \\\ */

/* /// "usbDoMethodA()" */
IPTR (usbDoMethodA)(ULONG methodid asm("d0"), IPTR * methoddata asm("a1"), struct NepSerialBase * nh asm("a6"))
{

    KPRINTF(10, ("Do Method %ld\n", methodid));
    switch(methodid)
    {
        case UCM_AttemptInterfaceBinding:
            return((IPTR) usbAttemptInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ForceInterfaceBinding:
            return((IPTR) usbForceInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ReleaseInterfaceBinding:
            usbReleaseInterfaceBinding(nh, (struct NepClassSerial *) methoddata[0]);
            return(TRUE);

        default:
            break;
    }
    return(0);
}
/* \\\ */

/**************************************************************************/

#undef  ps
#define ps ncp->ncp_Base

/* /// "nSetSerialMode()" */
void nSetSerialMode(struct NepClassSerial *ncp, struct IOExtSer *ioreq)
{
    struct UsbCDCLineCoding ulc;
    LONG ioerr;
    ULONG bufsize = ioreq->io_RBufLen / NUMREADPIPES;

    if(bufsize < 1024)
    {
        bufsize = 1024;
    }
    bufsize = (bufsize + 127) & (~127);

    psdSetAttrs(PGA_PIPESTREAM, ncp->ncp_EPInStream,
                PSA_BufferSize, bufsize,
                TAG_END);
    if(ioreq->io_SerFlags & SERF_EOFMODE)
    {
        psdSetAttrs(PGA_PIPESTREAM, ncp->ncp_EPInStream,
                    PSA_TermArray, &ioreq->io_TermArray,
                    TAG_END);
        psdSetAttrs(PGA_PIPESTREAM, ncp->ncp_EPOutStream,
                    PSA_TermArray, &ioreq->io_TermArray,
                    TAG_END);
    } else {
        psdSetAttrs(PGA_PIPESTREAM, ncp->ncp_EPInStream,
                    PSA_TermArray, NULL,
                    TAG_END);
        psdSetAttrs(PGA_PIPESTREAM, ncp->ncp_EPOutStream,
                    PSA_TermArray, NULL,
                    TAG_END);
    }
    ulc.dwDTERate = AROS_LONG2LE(ioreq->io_Baud);
    if(ioreq->io_StopBits == 1)
    {
        ulc.bCharFormat = 0;
    }
    else if(ioreq->io_StopBits == 2)
    {
        ulc.bCharFormat = 2;
    } else {
        ioreq->IOSer.io_Error = SerErr_InvParam;
        ulc.bCharFormat = 0;
    }
    ulc.bParityType = 0; /* no parity */
    if(ioreq->io_SerFlags & SERF_PARTY_ON)
    {
        if(ioreq->io_SerFlags & SERF_PARTY_ODD)
        {
            ulc.bParityType = 1; /* odd parity */
        } else {
            ulc.bParityType = 2; /* even parity */
        }
    }
    if(ioreq->io_ExtFlags & SEXTF_MSPON)
    {
        ioreq->io_SerFlags |= SERF_PARTY_ON;
        if(ioreq->io_ExtFlags & SEXTF_MARK)
        {
            ulc.bParityType = 3; /* mark parity */
        } else {
            ulc.bParityType = 4; /* space parity */
        }
    }
    ulc.bDataBits = ioreq->io_ReadLen;
    if(ioreq->io_SerFlags & SERF_RAD_BOOGIE)
    {
        ulc.bCharFormat = 0; /* 1 stop bit */
        ulc.bParityType = 0; /* no parity */
        ulc.bDataBits = 8; /* 8 data bits */
    }

    psdPipeSetup(ncp->ncp_EP0Pipe, URTF_CLASS|URTF_INTERFACE,
                 UCDCR_SET_LINE_CODING, 0, (ULONG) ncp->ncp_UnitIfNum);
    ioerr = psdDoPipe(ncp->ncp_EP0Pipe, &ulc, 7);
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "SET_LINE_CODING failed: %s (%ld)",
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        ioreq->IOSer.io_Error = SerErr_LineErr;
    }
    /* Activate RTS/CTS */
    psdPipeSetup(ncp->ncp_EP0Pipe, URTF_CLASS|URTF_INTERFACE,
                 UCDCR_SET_CONTROL_LINE_STATE, (ULONG) ((ioreq->io_SerFlags & SERF_7WIRE) ? 0 : 3), (ULONG) ncp->ncp_UnitIfNum);
    ioerr = psdDoPipe(ncp->ncp_EP0Pipe, NULL, 0);
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "SET_CONTROL_LINE_STATE failed: %s (%ld)",
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        ioreq->IOSer.io_Error = SerErr_LineErr;
    }
}
/* \\\ */

/* /// "nSerialTask()" */
void nSerialTask()
{

    struct NepClassSerial *ncp;
    struct PsdPipe *pp;
    ULONG sigmask;
    ULONG sigs;
    LONG ioerr;
    LONG len;
    IPTR pending;
    struct IOExtSer *ioreq;
    struct IOExtSer *ioreq2;
    UWORD serstate;

    if((ncp = nAllocSerial()))
    {
        Forbid();
        if(ncp->ncp_ReadySigTask)
        {
            Signal(ncp->ncp_ReadySigTask, 1UL<<ncp->ncp_ReadySignal);
        }
        Permit();

        /* Main task */
        if(ncp->ncp_EPIntPipe)
        {
            psdPipeSetup(ncp->ncp_EPIntPipe, URTF_IN|URTF_CLASS|URTF_INTERFACE,
                         UCDCR_SERIAL_STATE, 0, (ULONG) ncp->ncp_UnitIfNum);
            psdSendPipe(ncp->ncp_EPIntPipe, &ncp->ncp_SerialStateReq, sizeof(ncp->ncp_SerialStateReq));
        }
        /* start readahead */

        sigmask = (1UL<<ncp->ncp_Unit.unit_MsgPort.mp_SigBit)|(1UL<<ncp->ncp_TaskMsgPort->mp_SigBit)|SIGBREAKF_CTRL_C;
        {
            struct MsgPort *tmpmp = NULL;
            psdGetAttrs(PGA_PIPESTREAM, ncp->ncp_EPInStream, PSA_MessagePort, &tmpmp, TAG_END);
            if(tmpmp)
            {
                sigmask |= (1UL<<tmpmp->mp_SigBit);
            }
        }
        do
        {
            psdStreamRead(ncp->ncp_EPInStream, NULL, 0);
            while((pp = (struct PsdPipe *) GetMsg(ncp->ncp_TaskMsgPort)))
            {
                KPRINTF(1, ("Pipe back %08lx\n", pp));
                if(pp == ncp->ncp_EPIntPipe)
                {
                    KPRINTF(5, ("Int serial state back %04lx\n", ncp->ncp_SerialStateReq.uss_State));
                    ioerr = psdGetPipeError(pp);
                    len = psdGetPipeActual(pp);
                    if(ioerr && (ioerr != UHIOERR_RUNTPACKET))
                    {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                       "Serial state failed: %s (%ld), len = %ld",
                                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr, (int)len);
                        psdDelayMS(100);
                    }
                    psdSendPipe(ncp->ncp_EPIntPipe, &ncp->ncp_SerialStateReq, sizeof(ncp->ncp_SerialStateReq));
                    break;
                }
            }
            while((ioreq = (struct IOExtSer *) GetMsg(&ncp->ncp_Unit.unit_MsgPort)))
            {
                KPRINTF(5, ("command ioreq: 0x%08lx cmd: %lu len: %ld\n",
                        ioreq, ioreq->IOSer.io_Command, ioreq->IOSer.io_Length));
                switch(ioreq->IOSer.io_Command)
                {
                    case CMD_CLEAR:
                        psdStreamFlush(ncp->ncp_EPOutStream);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case CMD_RESET:
                        /* Reset does a flush too */
                    case CMD_FLUSH:
                        Forbid();
                        ioreq2 = (struct IOExtSer *) ncp->ncp_WriteQueue.lh_Head;
                        while(ioreq2->IOSer.io_Message.mn_Node.ln_Succ)
                        {
                            Remove((struct Node *) ioreq2);
                            ioreq2->IOSer.io_Error = IOERR_ABORTED;
                            ReplyMsg((struct Message *) ioreq2);
                            ioreq2 = (struct IOExtSer *) ncp->ncp_WriteQueue.lh_Head;
                        }
                        ioreq2 = (struct IOExtSer *) ncp->ncp_ReadQueue.lh_Head;
                        while(ioreq2->IOSer.io_Message.mn_Node.ln_Succ)
                        {
                            Remove((struct Node *) ioreq2);
                            ioreq2->IOSer.io_Error = IOERR_ABORTED;
                            ReplyMsg((struct Message *) ioreq2);
                            ioreq2 = (struct IOExtSer *) ncp->ncp_ReadQueue.lh_Head;
                        }
                        ReplyMsg((struct Message *) ioreq);
                        Permit();
                        break;

                    case SDCMD_QUERY:
                        serstate = 0;
                        if(!(ncp->ncp_SerialStateReq.uss_State & 0x0100)) /* Carrier Detect */
                            serstate |= (1<<5);
                        if(!(ncp->ncp_SerialStateReq.uss_State & 0x0200)) /* DSR */
                            serstate |= (1<<3);
                        if(ncp->ncp_SerialStateReq.uss_State & 0x0400) /* Break */
                            serstate |= (1<<10);
                        if(ncp->ncp_SerialStateReq.uss_State & 0x0800) /* Ring */
                            serstate |= (1<<2);
                        if(ncp->ncp_SerialStateReq.uss_State & 0x7000) /* Framing, Parity, Hardware error */
                            serstate |= (1<<8);

                        ioreq->io_Status = serstate;
                        pending = 0;
                        psdGetAttrs(PGA_PIPESTREAM, ncp->ncp_EPInStream,
                                    PSA_BytesPending, &pending,
                                    TAG_END);
                        ioreq->IOSer.io_Actual = pending;
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case SDCMD_SETPARAMS:
                        nSetSerialMode(ncp, ioreq);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case SDCMD_BREAK:
                        psdPipeSetup(ncp->ncp_EP0Pipe, URTF_CLASS|URTF_INTERFACE,
                                     UCDCR_SEND_BREAK, ioreq->io_BrkTime, (ULONG) ncp->ncp_UnitIfNum);
                        ioerr = psdDoPipe(ncp->ncp_EP0Pipe, NULL, 0);
                        if(ioerr)
                        {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                           "SEND_BREAK failed: %s (%ld)",
                                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                            ioreq->IOSer.io_Error = SerErr_LineErr;
                        }
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    default:
                        ioreq->IOSer.io_Error = IOERR_NOCMD;
                        ReplyMsg((struct Message *) ioreq);
                        break;
                }
            }
            if(!ncp->ncp_DevSuspend)
            {
                pending = 0;
                psdGetAttrs(PGA_PIPESTREAM, ncp->ncp_EPInStream,
                            PSA_BytesPending, &pending,
                            TAG_END);
                Forbid();
                ioreq = (struct IOExtSer *) ncp->ncp_ReadQueue.lh_Head;
                while(ioreq->IOSer.io_Message.mn_Node.ln_Succ && pending)
                {
                    Remove((struct Node *) ioreq);
                    Permit();
                    ioreq->IOSer.io_Actual = psdStreamRead(ncp->ncp_EPInStream, ioreq->IOSer.io_Data, ioreq->IOSer.io_Length);
                    ioerr = psdGetStreamError(ncp->ncp_EPInStream);
                    if(ioerr == UHIOERR_NAKTIMEOUT)
                    {
                        Forbid();
                        AddHead(&ncp->ncp_ReadQueue, &ioreq->IOSer.io_Message.mn_Node);
                        break;
                    } else {
                        if(ioerr > 0)
                        {
                            ioreq->IOSer.io_Error = SerErr_LineErr;
                        } else {
                            ioreq->IOSer.io_Error = ioerr;
                        }
                        ReplyMsg((struct Message *) ioreq);
                    }
                    psdGetAttrs(PGA_PIPESTREAM, ncp->ncp_EPInStream,
                                PSA_BytesPending, &pending,
                                TAG_END);
                    Forbid();
                    ioreq = (struct IOExtSer *) ncp->ncp_ReadQueue.lh_Head;
                }
                ioreq = (struct IOExtSer *) ncp->ncp_WriteQueue.lh_Head;
                while(ioreq->IOSer.io_Message.mn_Node.ln_Succ)
                {
                    Remove((struct Node *) ioreq);
                    ncp->ncp_WritePending = ioreq;
                    Permit();
                    ioreq->IOSer.io_Actual = psdStreamWrite(ncp->ncp_EPOutStream, ioreq->IOSer.io_Data, ioreq->IOSer.io_Length);
                    ncp->ncp_WritePending = NULL;
                    ioerr = psdGetStreamError(ncp->ncp_EPOutStream);
                    if(ioerr > 0)
                    {
                        ioreq->IOSer.io_Error = SerErr_LineErr;
                    } else {
                        ioreq->IOSer.io_Error = ioerr;
                    }
                    ReplyMsg((struct Message *) ioreq);
                    Forbid();
                    ioreq = (struct IOExtSer *) ncp->ncp_WriteQueue.lh_Head;
                }
                Permit();
            }
            sigs = Wait(sigmask);
        } while(!(sigs & SIGBREAKF_CTRL_C));
        KPRINTF(1, ("Going down...\n"));
        /* Now remove all requests still pending *anywhere* */
        ncp->ncp_DenyRequests = TRUE;

        if(ncp->ncp_EPIntPipe)
        {
            psdAbortPipe(ncp->ncp_EPIntPipe);
            psdWaitPipe(ncp->ncp_EPIntPipe);
        }

        /* Read/Write queues */
        Forbid();
        ioreq = (struct IOExtSer *) ncp->ncp_WriteQueue.lh_Head;
        while(ioreq->IOSer.io_Message.mn_Node.ln_Succ)
        {
            Remove((struct Node *) ioreq);
            ioreq->IOSer.io_Error = IOERR_ABORTED;
            ReplyMsg((struct Message *) ioreq);
            ioreq = (struct IOExtSer *) ncp->ncp_WriteQueue.lh_Head;
        }
        ioreq = (struct IOExtSer *) ncp->ncp_ReadQueue.lh_Head;
        while(ioreq->IOSer.io_Message.mn_Node.ln_Succ)
        {
            Remove((struct Node *) ioreq);
            ioreq->IOSer.io_Error = IOERR_ABORTED;
            ReplyMsg((struct Message *) ioreq);
            ioreq = (struct IOExtSer *) ncp->ncp_ReadQueue.lh_Head;
        }
        /* Command queue */
        while((ioreq = (struct IOExtSer *) GetMsg(&ncp->ncp_Unit.unit_MsgPort)))
        {
            ioreq->IOSer.io_Error = IOERR_ABORTED;
            ReplyMsg((struct Message *) ioreq);
        }
        Permit();
        KPRINTF(20, ("Going down the river!\n"));
        nFreeSerial(ncp);
    }

}
/* \\\ */

/* /// "nAllocSerial()" */
struct NepClassSerial * nAllocSerial(void)
{
    struct Task *thistask;
    struct NepClassSerial *ncp;
    struct PsdDescriptor *pdd;
    UBYTE *descdata = NULL;
    ULONG dataifnum = 0;

    thistask = FindTask(NULL);
    ncp = thistask->tc_UserData;
    do
    {
        if(!(ncp->ncp_Base = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
        {
            Alert(AG_OpenLib);
            break;
        }

        ncp->ncp_EPInt = psdFindEndpoint(ncp->ncp_Interface, NULL,
                                         EA_IsIn, TRUE,
                                         EA_TransferType, USEAF_INTERRUPT,
                                         TAG_END);

        // try to obtain correct data interface using the class specific interface descriptor
        ncp->ncp_DataIf = NULL;
        pdd = psdFindDescriptor(ncp->ncp_Device, NULL,
                                DDA_Interface, ncp->ncp_Interface,
                                DDA_DescriptorType, UDT_CS_INTERFACE,
                                DDA_CS_SubType, UDST_CDC_CALLMGM,
                                TAG_END);
        if(pdd)
        {
            KPRINTF(1, ("Descriptor %08lx\n", pdd));
            psdGetAttrs(PGA_DESCRIPTOR, pdd,
                        DDA_DescriptorData, &descdata,
                        TAG_END);
            dataifnum = ((struct UsbCDCCallMgmDesc *) descdata)->bDataInterface;
            KPRINTF(1, ("Using Data Interface %ld\n", dataifnum));
            if(!ncp->ncp_EPInt)
            {
                psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "No notify endpoint?!?");
                KPRINTF(1, ("Ooops!?! No notify endpoint defined?\n"));
                break;
            }
        } else {
            pdd = psdFindDescriptor(ncp->ncp_Device, NULL,
                                    DDA_Interface, ncp->ncp_Interface,
                                    DDA_DescriptorType, UDT_CS_INTERFACE,
                                    DDA_CS_SubType, UDST_CDC_UNION,
                                    TAG_END);
            if(pdd)
            {
                KPRINTF(1, ("Descriptor %08lx\n", pdd));
                psdGetAttrs(PGA_DESCRIPTOR, pdd,
                            DDA_DescriptorData, &descdata,
                            TAG_END);
                dataifnum = ((struct UsbCDCUnionDesc *) descdata)->bSlaveInterface0;
                KPRINTF(1, ("Using Data Interface %ld\n", dataifnum));
            } else {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "No descriptor pointing to the data interface. Hm.");
            }
        }

        do
        {
            ncp->ncp_DataIf = psdFindInterface(ncp->ncp_Device, ncp->ncp_DataIf,
                                               IFA_Class, CDCDATA_CLASSCODE,
                                               IFA_InterfaceNum, dataifnum,
                                               IFA_AlternateNum, 0xffffffff,
                                               TAG_END);
            if(!ncp->ncp_DataIf)
            {
                break;
            }
            ncp->ncp_EPIn = psdFindEndpoint(ncp->ncp_DataIf, NULL,
                                            EA_IsIn, TRUE,
                                            EA_TransferType, USEAF_BULK,
                                            TAG_END);
            ncp->ncp_EPOut = psdFindEndpoint(ncp->ncp_DataIf, NULL,
                                             EA_IsIn, FALSE,
                                             EA_TransferType, USEAF_BULK,
                                             TAG_END);
        } while(!(ncp->ncp_EPOut && ncp->ncp_EPIn));

        if(!ncp->ncp_DataIf)// Huawei ? (this interface => interrupt + bulks in & out)
        {
            ncp->ncp_EPIn = psdFindEndpoint(ncp->ncp_Interface, NULL,
                                            EA_IsIn, TRUE,
                                            EA_TransferType, USEAF_BULK,
                                            TAG_END);
            ncp->ncp_EPOut = psdFindEndpoint(ncp->ncp_Interface, NULL,
                                            EA_IsIn, FALSE,
                                            EA_TransferType, USEAF_BULK,
                                            TAG_END);
            if( ncp->ncp_EPInt && ncp->ncp_EPIn && ncp->ncp_EPOut ){
                 ncp->ncp_DataIf = ncp->ncp_Interface;
            }
        }

        if(!ncp->ncp_DataIf)
        {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "Data interface with IN and OUT interface missing?!?");
            KPRINTF(1, ("Ooops!?! No data interface defined?\n"));
            break;
        }
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Using data interface %ld.", dataifnum);
        ncp->ncp_Unit.unit_MsgPort.mp_SigBit = AllocSignal(-1);
        ncp->ncp_Unit.unit_MsgPort.mp_SigTask = thistask;
        ncp->ncp_Unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        ncp->ncp_Unit.unit_MsgPort.mp_Flags = PA_SIGNAL;
        ncp->ncp_WritePending = NULL;

        if((ncp->ncp_TaskMsgPort = CreateMsgPort()))
        {
            if((ncp->ncp_EP0Pipe = psdAllocPipe(ncp->ncp_Device, ncp->ncp_TaskMsgPort, NULL)))
            {
                psdSetAltInterface(ncp->ncp_EP0Pipe, ncp->ncp_DataIf);
                ncp->ncp_AbortSignal = AllocSignal(-1);
                if((ncp->ncp_EPOutStream = psdOpenStream(ncp->ncp_EPOut,
                                                        PSA_BufferedWrite, FALSE,
                                                        PSA_NoZeroPktTerm, FALSE,
                                                        PSA_NakTimeout, TRUE,
                                                        PSA_NakTimeoutTime, 5000,
                                                        PSA_AbortSigMask, (1UL<<ncp->ncp_AbortSignal)|SIGBREAKF_CTRL_C,
                                                        TAG_END)))
                {
                    /* Static taglist, not the vararg form: every value here is a
                       compile-time constant, and the sfdc vararg inline builds a
                       *local* array, which gcc materialises as a writable template in
                       .data. */
                    if((ncp->ncp_EPInStream = psdOpenStreamA(ncp->ncp_EPIn,
                                                             (struct TagItem *)ReadStreamTags)))
                    {
                        if(ncp->ncp_EPInt)
                        {
                            if((ncp->ncp_EPIntPipe = psdAllocPipe(ncp->ncp_Device, ncp->ncp_TaskMsgPort, ncp->ncp_EPInt)))
                            {
                                ncp->ncp_Task = thistask;
                                return(ncp);
                            }
                        } else {
                            ncp->ncp_Task = thistask;
                            return(ncp);
                        }
                        psdCloseStream(ncp->ncp_EPInStream);
                    }
                    psdCloseStream(ncp->ncp_EPOutStream);
                }
                FreeSignal(ncp->ncp_AbortSignal);
                psdFreePipe(ncp->ncp_EP0Pipe);
            }
            DeleteMsgPort(ncp->ncp_TaskMsgPort);
        }
        FreeSignal((LONG) ncp->ncp_Unit.unit_MsgPort.mp_SigBit);
    } while(FALSE);
    CloseLibrary(ncp->ncp_Base);
    Forbid();
    ncp->ncp_Task = NULL;
    if(ncp->ncp_ReadySigTask)
    {
        Signal(ncp->ncp_ReadySigTask, 1UL<<ncp->ncp_ReadySignal);
    }
    return(NULL);
}
/* \\\ */

/* /// "nFreeSerial()" */
void nFreeSerial(struct NepClassSerial *ncp)
{
    struct IOStdReq *ioreq;
    Forbid();
    FreeSignal(ncp->ncp_AbortSignal);
    ncp->ncp_AbortSignal = -1;
    /* Disable the message port, messages may still be queued */
    ncp->ncp_Unit.unit_MsgPort.mp_SigTask = NULL;
    ncp->ncp_Unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    FreeSignal((LONG) ncp->ncp_Unit.unit_MsgPort.mp_SigBit);
    // get rid of all messages that still have appeared here
    while((ioreq = (struct IOStdReq *) GetMsg(&ncp->ncp_Unit.unit_MsgPort)))
    {
        ioreq->io_Error = IOERR_ABORTED;
        ReplyMsg((struct Message *) ioreq);
    }
    Permit();

    psdCloseStream(ncp->ncp_EPInStream);
    psdCloseStream(ncp->ncp_EPOutStream);
    psdFreePipe(ncp->ncp_EPIntPipe);
    psdFreePipe(ncp->ncp_EP0Pipe);

    DeleteMsgPort(ncp->ncp_TaskMsgPort);
    CloseLibrary(ncp->ncp_Base);
    Forbid();
    ncp->ncp_Task = NULL;
    if(ncp->ncp_ReadySigTask)
    {
        Signal(ncp->ncp_ReadySigTask, 1UL<<ncp->ncp_ReadySignal);
    }
}
/* \\\ */
