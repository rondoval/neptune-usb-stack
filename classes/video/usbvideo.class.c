/*
 *----------------------------------------------------------------------------
 *                        usbvideo class for Neptune
 *----------------------------------------------------------------------------
 * USB Video Class (UVC 1.0 / 1.1 / 1.5) cameras.
 *
 * Binds the VideoControl interface of a video function; the function's
 * VideoStreaming interfaces are reached through it and never claimed. Each
 * binding runs a subtask that owns everything done with the function.
 *
 * The subtask parses the function's descriptors into the model (uvc_desc.c),
 * gives every capture stream a unit of usbvideo.device (dev.c) and then
 * serves the commands that device passes on: mode negotiation (uvc_ctrl.c),
 * starting and stopping the stream (uvc_stream.c).
 */

#include "debug.h"
#include "usbvideo.class.h"

/* /// "Lib Stuff" */
static const STRPTR libname = CLASS_NAME;

/* LVO table for the embedded usbvideo.device (MakeLibrary'd in libInit). Order 1..6. */
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

int libInit(struct NepVideoBase * nh)
{
    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", nh, EXEC_BASE_NAME));

    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);

#define UtilityBase nh->nh_UtilityBase

    if(!UtilityBase)
    {
        KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
        return FALSE;
    }
    NewList(&nh->nh_Units);
    NewList(&nh->nh_Bindings);

    if(!(nh->nh_DevBase = (struct NepVideoDevBase *) MakeLibrary((APTR) DevFuncTable, NULL, (APTR) devInit,
       sizeof(struct NepVideoDevBase), NULL)))
    {
        KPRINTF(20, ("failed to create " DEVNAME "\n"));
        CloseLibrary(UtilityBase);
        return FALSE;
    }
    nh->nh_DevBase->np_ClsBase = nh;
    Forbid();
    AddDevice((struct Device *) nh->nh_DevBase);
    nh->nh_DevBase->np_Library.lib_OpenCnt++;
    Permit();

    KPRINTF(10, ("libInit: Ok\n"));
    return TRUE;
}

/* Runs with no opener left: an open unit of the device counts as one of the
   class (devOpen), so nobody holds a unit when the list goes. */
int libExpunge(struct NepVideoBase * nh)
{
    struct NepVideoStream *ns;

    KPRINTF(10, ("libExpunge nh: 0x%08lx\n", nh));

    CloseLibrary((struct Library *) UtilityBase);

    ns = (struct NepVideoStream *) nh->nh_Units.lh_Head;
    while(ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ)
    {
        Remove((struct Node *) ns);
        FreeVec(ns);
        ns = (struct NepVideoStream *) nh->nh_Units.lh_Head;
    }

    nh->nh_DevBase->np_Library.lib_OpenCnt--;
    RemDevice((struct Device *) nh->nh_DevBase);
    return TRUE;
}
/* \\\ */

/*
 * ***********************************************************************
 * * Library functions                                                   *
 * ***********************************************************************
 */

/* /// "usbAttemptInterfaceBinding()" */
/* Neptune offers an interface. Taken if it is the VideoControl interface
   of a video function; NULL = not ours. */
struct NepClassVideo * usbAttemptInterfaceBinding(struct NepVideoBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    IPTR ifclass;
    IPTR subclass;
    IPTR proto;
    BOOL take;

    KPRINTF(1, ("nepVideoAttemptInterfaceBinding(%08lx)\n", pif));
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return NULL;
    }
    psdGetAttrs(PGA_INTERFACE, pif,
                IFA_Class, &ifclass,
                IFA_SubClass, &subclass,
                IFA_Protocol, &proto,
                IFA_Config, &pc,
                TAG_END);
    psdGetAttrs(PGA_CONFIG, pc,
                CA_Device, &pd,
                TAG_END);

    /* The VideoControl interface stands for the whole function: its streaming
       interfaces are offered here too, and declined. Decided on descriptors
       alone - no wire traffic. */
    take = (ifclass == VIDEO_CLASSCODE) && (subclass == VIDEO_CTRL_SUBCLASS) &&
           (proto <= VIDEO_PROTOCOL_15) &&
           psdFindDescriptor(pd, NULL,
                             DDA_Interface, pif,
                             DDA_DescriptorType, UDT_CS_INTERFACE,
                             DDA_CS_SubType, UDST_VIDEO_CTRL_HEADER,
                             TAG_END);
    CloseLibrary(ps);
    return take ? usbForceInterfaceBinding(nh, pif) : NULL;
}
/* \\\ */

/* /// "usbForceInterfaceBinding()" */
/* Bind the interface: the binding is set up and its subtask started.
   NULL = that failed. */
struct NepClassVideo * usbForceInterfaceBinding(struct NepVideoBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    struct NepClassVideo *ncv;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    STRPTR devname;
    STRPTR devidstr;
    STRPTR ifidstr;
    UBYTE buf[64];
    struct Task *tmptask;

    KPRINTF(1, ("nepVideoForceInterfaceBinding(%08lx)\n", pif));
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return NULL;
    }
    psdGetAttrs(PGA_INTERFACE, pif,
                IFA_Config, &pc,
                IFA_IDString, &ifidstr,
                TAG_END);
    psdGetAttrs(PGA_CONFIG, pc,
                CA_Device, &pd,
                TAG_END);
    psdGetAttrs(PGA_DEVICE, pd,
                DA_ProductName, &devname,
                DA_IDString, &devidstr,
                TAG_END);
    if((ncv = psdAllocVec(sizeof(struct NepClassVideo))))
    {
        ncv->ncv_ClsBase = nh;
        ncv->ncv_Device = pd;
        ncv->ncv_Interface = pif;
        ncv->ncv_DevIDString = devidstr;
        ncv->ncv_IfIDString = ifidstr;
        ncv->ncv_UsingDefaultCfg = TRUE;

        psdSafeRawDoFmt(buf, 64, "usbvideo.class<%08lx>", ncv);
        ncv->ncv_ReadySignal = SIGB_SINGLE;
        ncv->ncv_ReadySigTask = FindTask(NULL);
        SetSignal(0, SIGF_SINGLE);
        if((tmptask = psdSpawnSubTask(buf, nVideoTask, ncv)))
        {
            psdBorrowLocksWait(tmptask, 1UL<<ncv->ncv_ReadySignal);
            if(ncv->ncv_Task)
            {
                ncv->ncv_ReadySigTask = NULL;
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               NEP_BOUND1_TXT,
                               devname);
                CloseLibrary(ps);
                return ncv;
            }
        }
        ncv->ncv_ReadySigTask = NULL;
        psdFreeVec(ncv);
    }
    CloseLibrary(ps);
    return NULL;
}
/* \\\ */

/* /// "usbReleaseInterfaceBinding()" */
/* The camera is gone, or the class is to let go of it: the settings window
   and the subtask end, the binding is freed */
void usbReleaseInterfaceBinding(struct NepVideoBase *nh, struct NepClassVideo *ncv)
{
    struct Library *ps;
    STRPTR devname;

    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return;
    }
    /* the settings window first: it sends its requests to the subtask */
    Forbid();
    ncv->ncv_ReadySignal = SIGB_SINGLE;
    ncv->ncv_ReadySigTask = FindTask(NULL);
    if(ncv->ncv_GUITask)
    {
        Signal(ncv->ncv_GUITask, SIGBREAKF_CTRL_C);
    }
    Permit();
    while(ncv->ncv_GUITask)
    {
        Wait(1UL<<ncv->ncv_ReadySignal);
    }
    Forbid();
    if(ncv->ncv_Task)
    {
        Signal(ncv->ncv_Task, SIGBREAKF_CTRL_C);
    }
    Permit();
    while(ncv->ncv_Task)
    {
        Wait(1UL<<ncv->ncv_ReadySignal);
    }
    psdGetAttrs(PGA_DEVICE, ncv->ncv_Device, DA_ProductName, &devname, TAG_END);
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   NEP_RELEASED_TXT,
                   devname);
    psdFreeVec(ncv);
    CloseLibrary(ps);
}
/* \\\ */

/* /// "usbGetAttrsA()" */
/* What Neptune and NeptuneUSB want to know of the class and of a binding.
   Returns the number of tags answered. */
LONG (usbGetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepVideoBase * nh asm("a6"))
{
    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("nepVideoGetAttrsA(%ld, %08lx, %08lx)\n", type, usbstruct, tags));
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
                 *((STRPTR *) ti->ti_Data) = "USB video class cameras";
                 count++;
             }
             if((ti = FindTagItem(UCCA_HasClassCfgGUI, tags)))
             {
                 *((IPTR *) ti->ti_Data) = FALSE;
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
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_SupportsSuspend, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             break;

         case UGA_BINDING:
             if((ti = FindTagItem(UCBA_UsingDefaultCfg, tags)))
             {
                 *((IPTR *) ti->ti_Data) = ((struct NepClassVideo *) usbstruct)->ncv_UsingDefaultCfg;
                 count++;
             }
             break;
    }
    return count;
}
/* \\\ */

/* /// "usbSetAttrsA()" */
/* Nothing of the class can be set */
LONG (usbSetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepVideoBase * nh asm("a6"))
{
    return 0;
}
/* \\\ */

static struct NepVideoStream * nNextUnit(struct NepClassVideo *ncv, struct NepVideoStream *ns, BOOL orphans);

/* /// "usbDoMethodA()" */
/* The methods of the class: binding and release, the settings window, a
   change of the stored settings, suspend and resume */
IPTR (usbDoMethodA)(ULONG methodid asm("d0"), IPTR * methoddata asm("a1"), struct NepVideoBase * nh asm("a6"))
{
    KPRINTF(10, ("Do Method %ld\n", methodid));
    switch(methodid)
    {
        case UCM_AttemptInterfaceBinding:
            return (IPTR) usbAttemptInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]);

        case UCM_ForceInterfaceBinding:
            return (IPTR) usbForceInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]);

        case UCM_ReleaseInterfaceBinding:
            usbReleaseInterfaceBinding(nh, (struct NepClassVideo *) methoddata[0]);
            return TRUE;

        case UCM_OpenBindingCfgWindow:
            return nOpenBindingCfgWindow(nh, (struct NepClassVideo *) methoddata[0]);

        case UCM_ConfigChangedEvent:
        {
            /* What is stored may be new - at boot it is, for a camera bound
               before the settings were loaded. Each camera's subtask applies
               its own, units or not; one whose window is open is being set
               by hand. */
            struct NepClassVideo *ncv;

            Forbid();
            ncv = (struct NepClassVideo *) nh->nh_Bindings.lh_Head;
            while(ncv->ncv_Node.ln_Succ)
            {
                if(!ncv->ncv_GUITask)
                {
                    Signal(ncv->ncv_Task, SIGBREAKF_CTRL_D);
                }
                ncv = (struct NepClassVideo *) ncv->ncv_Node.ln_Succ;
            }
            Permit();
            return TRUE;
        }

        case UCM_AttemptSuspendDevice:
        {
            /* An idle camera may sleep; one with a stream running is in use.
               Nothing is stopped or put away for it: the transfer waiting at
               the status endpoint stays queued in the controller driver and
               goes on at the resume, and the first request anyone sends -
               a mode, a stream start, the settings window - wakes the camera
               inside the library. */
            struct NepClassVideo *ncv = (struct NepClassVideo *) methoddata[0];
            struct NepVideoStream *ns = NULL;

            while((ns = nNextUnit(ncv, ns, FALSE)))
            {
                if(ns->ns_Streaming)
                {
                    return FALSE;
                }
            }
            return TRUE;
        }

        case UCM_AttemptResumeDevice:
            return TRUE;

        default:
            break;
    }
    return 0;
}
/* \\\ */

/**************************************************************************/

#undef  ps
#define ps ncv->ncv_Base

/* /// "nFindStreamIf()" */
/* Alternate 0 of a VideoStreaming interface: the one its format descriptors
   hang on, whatever alternate is selected. */
static struct PsdInterface * nFindStreamIf(struct NepClassVideo *ncv, ULONG ifnum)
{
    return psdFindInterface(ncv->ncv_Device, NULL,
                            IFA_InterfaceNum, ifnum,
                            IFA_AlternateNum, 0,
                            IFA_Class, VIDEO_CLASSCODE,
                            IFA_SubClass, VIDEO_STREAM_SUBCLASS,
                            TAG_END);
}
/* \\\ */

/* /// "nWalkDescs()" */
/* The class-specific descriptors of one interface, in descriptor order.
   Counts them; given a model, feeds them to it - as a stream's when us is
   set, as the VideoControl interface's otherwise. */
static ULONG nWalkDescs(struct NepClassVideo *ncv, struct PsdInterface *pif,
                        struct UvcFunction *uf, struct UvcStreamDesc *us)
{
    struct PsdDescriptor *pdd = NULL;
    ULONG count = 0;

    while((pdd = psdFindDescriptor(ncv->ncv_Device, pdd,
                                   DDA_Interface, pif,
                                   DDA_DescriptorType, UDT_CS_INTERFACE,
                                   TAG_END)))
    {
        count++;
        if(!uf)
        {
            continue; /* only counting */
        }
        UBYTE *desc = NULL;
        IPTR len = 0;

        psdGetAttrs(PGA_DESCRIPTOR, pdd,
                    DDA_DescriptorData, &desc,
                    DDA_DescriptorLength, &len,
                    TAG_END);
        if(us)
        {
            uvcAddStreamDesc(uf, us, desc, len);
        } else {
            uvcAddControlDesc(uf, desc, len);
        }
    }
    return count;
}
/* \\\ */

/* /// "nBuildFunction()" */
/* Descriptors to model: count, allocate once, fill. The interface numbers of
   the function's streams come from the VideoControl header. */
static struct UvcFunction * nBuildFunction(struct NepClassVideo *ncv)
{
    struct PsdDescriptor *pdd;
    UBYTE *header = NULL;
    IPTR headerlen = 0;

    pdd = psdFindDescriptor(ncv->ncv_Device, NULL,
                            DDA_Interface, ncv->ncv_Interface,
                            DDA_DescriptorType, UDT_CS_INTERFACE,
                            DDA_CS_SubType, UDST_VIDEO_CTRL_HEADER,
                            TAG_END);
    if(!pdd)
    {
        return NULL;
    }
    psdGetAttrs(PGA_DESCRIPTOR, pdd,
                DDA_DescriptorData, &header,
                DDA_DescriptorLength, &headerlen,
                TAG_END);
    const struct UsbVideoHeaderDesc *hd = (const struct UsbVideoHeaderDesc *) header;
    if(headerlen < sizeof(*hd))
    {
        return NULL;
    }
    ULONG numifs = min(hd->bInCollection, headerlen - sizeof(*hd));
    const UBYTE *ifnums = hd->baInterfaceNr;

    ULONG ctrldescs = nWalkDescs(ncv, ncv->ncv_Interface, NULL, NULL);
    ULONG streamdescs = 0;
    for(ULONG cnt = 0; cnt < numifs; cnt++)
    {
        struct PsdInterface *pif = nFindStreamIf(ncv, ifnums[cnt]);
        if(pif)
        {
            streamdescs += nWalkDescs(ncv, pif, NULL, NULL);
        }
    }

    APTR mem = psdAllocVec(uvcFunctionSize(ctrldescs, numifs, streamdescs));
    if(!mem)
    {
        return NULL;
    }
    struct UvcFunction *uf = uvcInitFunction(mem, ctrldescs, numifs, streamdescs);
    nWalkDescs(ncv, ncv->ncv_Interface, uf, NULL);
    for(ULONG cnt = 0; cnt < numifs; cnt++)
    {
        struct PsdInterface *pif = nFindStreamIf(ncv, ifnums[cnt]);
        if(pif)
        {
            struct UvcStreamDesc *us = uvcAddStream(uf, ifnums[cnt]);

            nWalkDescs(ncv, pif, uf, us);
            uvcAddEmbeddedFormat(uf, us);
        }
    }
    return uf;
}
/* \\\ */

/* /// "nEntityName()" */
/* Two-letter names of the entity types, by descriptor subtype */
static const char EntityNames[8][3] = { "??", "??", "IT", "OT", "SU", "PU", "XU", "EU" };

static STRPTR nEntityName(const struct UvcEntity *ue)
{
    return (STRPTR) ((ue->ue_TermType == UVITT_CAMERA) ? "CT" : EntityNames[ue->ue_Type & 7]);
}
/* \\\ */

/* /// "nLogStream()" */
/* One stream into the log: its endpoint and the path its video takes
   through the camera's units, then each format with its sizes */
static void nLogStream(struct NepClassVideo *ncv, struct UvcStreamDesc *us)
{
    struct UvcFunction *uf = ncv->ncv_Function;
    UBYTE path[160];
    UBYTE ids[16];
    ULONG depth = 0;
    ULONG pos = 0;
    ULONG others = 0;

    if(us->us_HeaderType != UDST_VIDEO_STREAM_INPUT_HEADER)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Interface %ld is not a capture stream; ignored.",
                       (ULONG) us->us_IfNum);
        return;
    }

    /* the chain of entities that ends in this stream's terminal, sink first */
    for(ULONG id = us->us_TerminalLink; depth < sizeof(ids); )
    {
        struct UvcEntity *ue = uvcFindEntity(uf, id);
        if(!ue)
        {
            break;
        }
        ids[depth++] = id;
        if(!ue->ue_NumSources)
        {
            break;
        }
        id = ue->ue_Sources[0];
    }
    path[0] = 0;
    while(depth--)
    {
        struct UvcEntity *ue = uvcFindEntity(uf, ids[depth]);
        psdSafeRawDoFmt(&path[pos], sizeof(path) - pos, "%s %ld%s",
                        nEntityName(ue), (ULONG) ue->ue_ID, depth ? " > " : "");
        pos += strlen(&path[pos]);
    }
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Interface %ld: %s endpoint %02lx, video path %s.",
                   (ULONG) us->us_IfNum,
                   uvcStreamIsBulk(ncv, us) ? "bulk" : "isochronous",
                   (ULONG) us->us_EPAddr, path);

    for(ULONG cnt = 0; cnt < us->us_NumFormats; cnt++)
    {
        struct UvcFormat *ufo = &us->us_Formats[cnt];
        const struct UvcPayload *up = ufo->ufo_Payload;
        if(!up || !ufo->ufo_NumFrames)
        {
            others++;
            continue;
        }
        struct UvcFrame *smallest = ufo->ufo_Frames;
        struct UvcFrame *largest = ufo->ufo_Frames;
        struct UvcFrame *deffrm = ufo->ufo_Frames;
        for(ULONG frm = 0; frm < ufo->ufo_NumFrames; frm++)
        {
            struct UvcFrame *ufr = &ufo->ufo_Frames[frm];
            ULONG pixels = (ULONG) ufr->ufr_Width * ufr->ufr_Height;
            if(pixels < (ULONG) smallest->ufr_Width * smallest->ufr_Height)
            {
                smallest = ufr;
            }
            if(pixels > (ULONG) largest->ufr_Width * largest->ufr_Height)
            {
                largest = ufr;
            }
            if(ufr->ufr_Index == ufo->ufo_DefFrame)
            {
                deffrm = ufr;
            }
        }
        ULONG name = uvcFourCC(ufo);
        UBYTE fourcc[5] = { name >> 24, name >> 16, name >> 8, name, 0 };
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Interface %ld: %s, %ld sizes from %ldx%ld to %ldx%ld, default %ldx%ld at %ld.%ld fps.",
                       (ULONG) us->us_IfNum, fourcc, (ULONG) ufo->ufo_NumFrames,
                       (ULONG) smallest->ufr_Width, (ULONG) smallest->ufr_Height,
                       (ULONG) largest->ufr_Width, (ULONG) largest->ufr_Height,
                       (ULONG) deffrm->ufr_Width, (ULONG) deffrm->ufr_Height,
                       10000000 / deffrm->ufr_DefInterval, (100000000 / deffrm->ufr_DefInterval) % 10);
    }
    if(others == us->us_NumFormats)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Interface %ld offers no format this class can stream yet.",
                       (ULONG) us->us_IfNum);
    }
    else if(others)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Interface %ld: %ld more format(s) not supported yet.",
                       (ULONG) us->us_IfNum, others);
    }
}
/* \\\ */

/* /// "nLogFunction()" */
/* The function as the class read it, into the log */
static void nLogFunction(struct NepClassVideo *ncv)
{
    struct UvcFunction *uf = ncv->ncv_Function;

    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "UVC %lx.%02lx function: %ld terminals and units, %ld video stream(s).",
                   (ULONG) uf->uf_Version >> 8, (ULONG) uf->uf_Version & 0xff,
                   (ULONG) uf->uf_NumEntities, (ULONG) uf->uf_NumStreams);
    if(uf->uf_Skipped)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "%ld malformed descriptor(s) ignored.", (ULONG) uf->uf_Skipped);
    }
    for(ULONG cnt = 0; cnt < uf->uf_NumStreams; cnt++)
    {
        nLogStream(ncv, &uf->uf_Streams[cnt]);
    }

#ifdef DEBUG
    /* the whole model, for the debug log */
    for(ULONG cnt = 0; cnt < uf->uf_NumEntities; cnt++)
    {
        struct UvcEntity *ue = &uf->uf_Entities[cnt];
        KPRINTF(10, ("Entity %ld: %s, terminal type %04lx, %ld source(s) (first %ld), %ld control byte(s) %02lx %02lx %02lx\n",
                     (ULONG) ue->ue_ID, nEntityName(ue), (ULONG) ue->ue_TermType,
                     (ULONG) ue->ue_NumSources, (ULONG) (ue->ue_NumSources ? ue->ue_Sources[0] : 0),
                     (ULONG) ue->ue_ControlSize,
                     (ULONG) (ue->ue_ControlSize > 0 ? ue->ue_Controls[0] : 0),
                     (ULONG) (ue->ue_ControlSize > 1 ? ue->ue_Controls[1] : 0),
                     (ULONG) (ue->ue_ControlSize > 2 ? ue->ue_Controls[2] : 0)));
    }
    for(ULONG cnt = 0; cnt < uf->uf_NumFormats; cnt++)
    {
        struct UvcFormat *ufo = &uf->uf_Formats[cnt];
        KPRINTF(10, ("Format %ld: subtype %02lx, %s, %ld frame(s), default frame %ld\n",
                     (ULONG) ufo->ufo_Index, (ULONG) ufo->ufo_Type,
                     ufo->ufo_Payload ? "streamable" : "not streamable",
                     (ULONG) ufo->ufo_NumFrames, (ULONG) ufo->ufo_DefFrame));
        for(ULONG frm = 0; frm < ufo->ufo_NumFrames; frm++)
        {
            struct UvcFrame *ufr = &ufo->ufo_Frames[frm];
            KPRINTF(10, ("  Frame %ld: %ldx%ld, default interval %ld, %ld interval(s) %ld..%ld\n",
                         (ULONG) ufr->ufr_Index, (ULONG) ufr->ufr_Width, (ULONG) ufr->ufr_Height,
                         ufr->ufr_DefInterval, (ULONG) ufr->ufr_NumIntervals,
                         uvcFrameMinInterval(ufr), uvcFrameMaxInterval(ufr)));
        }
    }
#endif
}
/* \\\ */

/* /// "nFindUnit()" */
/* The unit with a number, or NULL. The caller holds Forbid(), as for the
   three functions that follow: the list of units is the class's. */
static struct NepVideoStream * nFindUnit(struct NepVideoBase *nh, ULONG unitno)
{
    struct NepVideoStream *ns = (struct NepVideoStream *) nh->nh_Units.lh_Head;

    while(ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ)
    {
        if(ns->ns_UnitNo == unitno)
        {
            return ns;
        }
        ns = (struct NepVideoStream *) ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ;
    }
    return NULL;
}
/* \\\ */

/* /// "nNewUnit()" */
/* A new unit with that number, in the class's list for good. NULL if there
   is no memory for it. */
static struct NepVideoStream * nNewUnit(struct NepVideoBase *nh, ULONG unitno)
{
    struct NepVideoStream *unit = AllocVec(sizeof(struct NepVideoStream), MEMF_PUBLIC|MEMF_CLEAR);

    if(unit)
    {
        unit->ns_UnitNo = unitno;
        NewList(&unit->ns_Unit.unit_MsgPort.mp_MsgList);
        NewList(&unit->ns_Reads.ur_Queue);
        AddTail(&nh->nh_Units, &unit->ns_Unit.unit_MsgPort.mp_Node);
    }
    return unit;
}
/* \\\ */

/* /// "nOwnUnit()" */
/* The unit that stood for this camera and stream before - the same vendor,
   product, serial number and interface - if no camera is behind it now.
   NULL if there is none. */
static struct NepVideoStream * nOwnUnit(struct NepVideoBase *nh, ULONG vendid, ULONG prodid, ULONG ifnum,
                                        CONST_STRPTR serial)
{
    struct NepVideoStream *ns = (struct NepVideoStream *) nh->nh_Units.lh_Head;

    while(ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ)
    {
        if(!ns->ns_Binding && (ns->ns_VendorID == vendid) && (ns->ns_ProductID == prodid) &&
           (ns->ns_IfNum == ifnum) && !strcmp(ns->ns_SerialNumber, serial))
        {
            return ns;
        }
        ns = (struct NepVideoStream *) ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ;
    }
    return NULL;
}
/* \\\ */

/* /// "nSpareUnit()" */
/* A unit for a stream that has none of its own: the lowest number that can
   be had. That is a number no unit has yet - then a new unit, or NULL if
   there is no memory for one - or the number of a unit whose camera is away
   and that nobody keeps open for it. Such a camera gets another number if it
   comes back. */
static struct NepVideoStream * nSpareUnit(struct NepVideoBase *nh)
{
    for(ULONG unitno = 0; ; unitno++)
    {
        struct NepVideoStream *ns = nFindUnit(nh, unitno);

        if(!ns)
        {
            return nNewUnit(nh, unitno);
        }
        if(!ns->ns_Binding && !ns->ns_Unit.unit_OpenCnt)
        {
            return ns;
        }
    }
}
/* \\\ */

/* /// "nAttachUnits()" */
/* Give every capture stream that has something to stream a unit of the
   device: the one this camera (by vendor, product and serial number) and
   stream had before if no camera is behind it now; else the lowest number
   that no unit has, or whose unit nobody holds open while its camera is
   away - that camera gets another number if it comes back. */
static void nAttachUnits(struct NepClassVideo *ncv)
{
    struct NepVideoBase *nh = ncv->ncv_ClsBase;
    struct UvcFunction *uf = ncv->ncv_Function;
    STRPTR devname = NULL;
    STRPTR devserial = NULL;
    STRPTR devmaker = NULL;
    IPTR vendid = 0;
    IPTR prodid = 0;
    UBYTE serial[sizeof(((struct NepVideoStream *) NULL)->ns_SerialNumber)];

    /* the stack has a name for the maker of every device: the device's own,
       else the one of its vendor ID, else "n/a" */
    psdGetAttrs(PGA_DEVICE, ncv->ncv_Device,
                DA_VendorID, &vendid,
                DA_ProductID, &prodid,
                DA_ProductName, &devname,
                DA_SerialNumber, &devserial,
                DA_Manufacturer, &devmaker,
                TAG_END);
    strncpy(serial, devserial ? devserial : (STRPTR) "", sizeof(serial) - 1);
    serial[sizeof(serial) - 1] = 0;
    for(ULONG cnt = 0; cnt < uf->uf_NumStreams; cnt++)
    {
        struct UvcStreamDesc *us = &uf->uf_Streams[cnt];

        if((us->us_HeaderType != UDST_VIDEO_STREAM_INPUT_HEADER) || !uvcFindFormat(us, 0))
        {
            continue;
        }
        BOOL bulk = uvcStreamIsBulk(ncv, us);

        Forbid();
        /* the unit it had before, if that can be had again; else the lowest number that can */
        struct NepVideoStream *unit = nOwnUnit(nh, vendid, prodid, us->us_IfNum, serial);

        if(!unit)
        {
            unit = nSpareUnit(nh);
        }
        if(unit)
        {
            unit->ns_VendorID = vendid;
            unit->ns_ProductID = prodid;
            unit->ns_IfNum = us->us_IfNum;
            unit->ns_IsBulk = bulk;
            unit->ns_Desc = us;
            unit->ns_Mode.um_Format = NULL;
            strncpy(unit->ns_ProductName, devname ? devname : (STRPTR) "", sizeof(unit->ns_ProductName) - 1);
            unit->ns_ProductName[sizeof(unit->ns_ProductName) - 1] = 0;
            strcpy(unit->ns_SerialNumber, serial);
            strncpy(unit->ns_Manufacturer, devmaker ? devmaker : (STRPTR) "n/a", sizeof(unit->ns_Manufacturer) - 1);
            unit->ns_Manufacturer[sizeof(unit->ns_Manufacturer) - 1] = 0;
            unit->ns_Binding = ncv;
        }
        Permit();

        if(unit)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Interface %ld is %s unit %ld.",
                           (ULONG) us->us_IfNum, DEVNAME, unit->ns_UnitNo);
        }
    }
}
/* \\\ */

/* /// "nNextUnit()" */
/* The units of this binding, one after the other; with orphans only those
   whose stream outlived its opener. The list only ever grows. */
static struct NepVideoStream * nNextUnit(struct NepClassVideo *ncv, struct NepVideoStream *ns, BOOL orphans)
{
    Forbid();
    ns = (struct NepVideoStream *) (ns ? ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ
                                       : ncv->ncv_ClsBase->nh_Units.lh_Head);
    while(ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ)
    {
        if((ns->ns_Binding == ncv) && (!orphans || (ns->ns_Streaming && !ns->ns_Unit.unit_OpenCnt)))
        {
            Permit();
            return ns;
        }
        ns = (struct NepVideoStream *) ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ;
    }
    Permit();
    return NULL;
}
/* \\\ */

/* /// "nDetachUnits()" */
/* The camera is going: streams end, waiting reads fail, and the units stay
   with nothing behind them */
static void nDetachUnits(struct NepClassVideo *ncv)
{
    struct NepVideoStream *ns = NULL;

    while((ns = nNextUnit(ncv, ns, FALSE)))
    {
        uvcStop(ncv, ns);
        Forbid();
        uvcAbortReads(ns, UVERR_GONE);
        ns->ns_Binding = NULL;
        ns->ns_Desc = NULL;
        ns->ns_Mode.um_Format = NULL;
        ns->ns_Mode.um_Frame = NULL;
        Permit();
    }
}
/* \\\ */

/* /// "nCmdSetMode()" */
/* NSCMD_UV_SETMODE: negotiate the mode a request asks for. What it leaves
   open - format, size, interval - is the default. On success the request
   holds the mode the camera settled on. The structure has grown by what a
   request may ask of an encoder: one of the first form asks nothing of it
   and gets nothing back. Returns 0 or an error for io_Error. */
static LONG nCmdSetMode(struct NepClassVideo *ncv, struct IOStdReq *ioreq)
{
    struct NepVideoStream *ns = (struct NepVideoStream *) ioreq->io_Unit;
    struct UsbVideoModeReq *uvq = (struct UsbVideoModeReq *) ioreq->io_Data;

    if(!uvq || (ioreq->io_Length < offsetof(struct UsbVideoModeReq, uvq_BitRate)))
    {
        return IOERR_BADLENGTH;
    }
    if(ns->ns_Streaming)
    {
        return UVERR_BUSY;
    }
    BOOL hasencoder = (ioreq->io_Length >= sizeof(struct UsbVideoModeReq));
    ULONG bitrate = hasencoder ? uvq->uvq_BitRate : 0;
    ULONG keyinterval = hasencoder ? uvq->uvq_KeyInterval : 0;
    /* what the request leaves open is the default: the user's, if the
       settings stored for the camera name one, else the camera's own */
    const struct UvcDefault *ud = &ns->ns_Default;
    struct UvcFormat *ufo = uvcFindFormat(ns->ns_Desc, uvq->uvq_FourCC);

    if(!ufo)
    {
        return UVERR_BADMODE;
    }
    /* a size that is named has to be one the format has; none is the default */
    BOOL sized = uvq->uvq_Width || uvq->uvq_Height;
    struct UvcFrame *ufr = sized ? uvcFindFrame(ufo, uvq->uvq_Width, uvq->uvq_Height)
                                 : uvcDefaultFrame(ufo, ud->ud_Width, ud->ud_Height);

    if(!ufr)
    {
        return UVERR_BADMODE;
    }
    ULONG interval = uvq->uvq_Interval ? uvcSnapInterval(ufr, uvq->uvq_Interval)
                                       : uvcDefaultInterval(ufr, ud->ud_Width, ud->ud_Height, ud->ud_Interval);
    /* the one kind of mode whose encoder the class can ask anything of */
    BOOL embedded = (ufo->ufo_Payload->up_Flags & UPF_EMBEDDED) != 0;
    LONG err = embedded ? uvcNegotiateEmbedded(ncv, ns, ufo, ufr, interval, bitrate, keyinterval)
                        : uvcNegotiate(ncv, ns, ufo, ufr, interval);

    if(!err)
    {
        uvq->uvq_FourCC = uvcFourCC(ns->ns_Mode.um_Format);
        uvq->uvq_Width = ns->ns_Mode.um_Frame->ufr_Width;
        uvq->uvq_Height = ns->ns_Mode.um_Frame->ufr_Height;
        uvq->uvq_Interval = ns->ns_Mode.um_Interval;
        uvq->uvq_MaxFrameSize = ns->ns_Mode.um_MaxFrameSize;
        ioreq->io_Actual = offsetof(struct UsbVideoModeReq, uvq_BitRate);
        if(hasencoder)
        {
            uvq->uvq_BitRate = embedded ? AROS_LE2LONG(ns->ns_Mode.um_Embed.dwBitRate) : 0;
            uvq->uvq_KeyInterval = embedded ? AROS_LE2WORD(ns->ns_Mode.um_Embed.wIFramePeriod) : 0;
            ioreq->io_Actual = sizeof(struct UsbVideoModeReq);
        }
    }
    return err;
}
/* \\\ */

/* /// "nApplyControls()" */
/* Write the control values of a chunk to the camera, in their order. What
   the camera refuses is counted and left: a control that an automatic mode
   governs refuses while that mode is on, and that is as it should be. */
static void nApplyControls(struct NepClassVideo *ncv, const struct UvcCfgChunk *chunk, ULONG *written, ULONG *refused)
{
    ULONG len = AROS_LONG2BE(chunk->ucc_Length);

    *written = 0;
    *refused = 0;
    for(ULONG pos = 0; pos + sizeof(struct UvcCfgValue) <= len; pos += sizeof(struct UvcCfgValue))
    {
        const struct UvcCfgValue *ucv = (const struct UvcCfgValue *) (chunk->ucc_Data + pos);

        if(uvcControlWrite(ncv, ucv->ucv_ID, ucv->ucv_Entity, ucv->ucv_Value, NULL))
        {
            (*refused)++;
        } else {
            (*written)++;
        }
    }
}
/* \\\ */

/* /// "nStoredChunk()" */
/* One chunk of what is stored for this camera: a copy, to be freed, or NULL */
static struct UvcCfgChunk * nStoredChunk(struct NepClassVideo *ncv, ULONG id)
{
    struct UvcCfgChunk *chunk = NULL;
    struct PsdIFFContext *pic;

    /* the stored form is the library's own, and stays where it is only this long */
    Forbid();
    if((pic = psdGetUsbDevCfg((STRPTR) libname, ncv->ncv_DevIDString, ncv->ncv_IfIDString)))
    {
        chunk = psdGetCfgChunk(pic, id);
    }
    Permit();
    return chunk;
}
/* \\\ */

/* /// "nApplyStoredControls()" */
/* The control values stored for this camera go to it. Returns whether there
   are any. The settings window wrote them. */
static BOOL nApplyStoredControls(struct NepClassVideo *ncv, ULONG *written, ULONG *refused)
{
    struct UvcCfgChunk *controls = nStoredChunk(ncv, UVCFG_VALUES);

    if(!controls)
    {
        return FALSE;
    }
    nApplyControls(ncv, controls, written, refused);
    psdFreeVec(controls);
    return TRUE;
}
/* \\\ */

/* /// "nApplyConfig()" */
/* What is stored for this camera, put in force: the default mode of each of
   its streams goes to the units, the control values to the camera. */
static void nApplyConfig(struct NepClassVideo *ncv)
{
    struct UvcCfgChunk *modes = nStoredChunk(ncv, UVCFG_MODES);
    struct NepVideoStream *ns = NULL;

    while((ns = nNextUnit(ncv, ns, FALSE)))
    {
        struct UvcCfgMode none = { 0, 0, 0, 0 };
        const struct UvcCfgMode *ucm = &none;
        ULONG len = modes ? AROS_LONG2BE(modes->ucc_Length) : 0;

        for(ULONG pos = 0; pos + sizeof(struct UvcCfgMode) <= len; pos += sizeof(struct UvcCfgMode))
        {
            const struct UvcCfgMode *cand = (const struct UvcCfgMode *) (modes->ucc_Data + pos);

            if(cand->ucm_IfNum == ns->ns_IfNum)
            {
                ucm = cand;
                break;
            }
        }
        /* readers of the unit (NSCMD_UV_GETMODES) look under Forbid() */
        Forbid();
        ns->ns_Default.ud_Width = ucm->ucm_Width;
        ns->ns_Default.ud_Height = ucm->ucm_Height;
        ns->ns_Default.ud_Interval = ucm->ucm_Interval;
        Permit();
    }
    ULONG written;
    ULONG refused;
    BOOL controls = nApplyStoredControls(ncv, &written, &refused);
    if(controls)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Stored settings applied: %ld controls set, %ld left as the camera has them.",
                       written, refused);
    }
    ncv->ncv_UsingDefaultCfg = !(controls || modes);
    psdFreeVec(modes);
}
/* \\\ */

/* /// "nVideoTask()" */
/* The binding's subtask: everything that talks to the camera happens here */
void nVideoTask()
{
    struct NepClassVideo *ncv;
    struct IOStdReq *ioreq;
    ULONG sigmask;
    ULONG sigs;

    if((ncv = nAllocVideo()))
    {
        Forbid();
        if(ncv->ncv_ReadySigTask)
        {
            Signal(ncv->ncv_ReadySigTask, 1UL<<ncv->ncv_ReadySignal);
        }
        Permit();

        /* Main task: the commands usbvideo.device and the settings window
           pass on and what the camera reports by itself, until the binding
           is released. Ctrl-D says that the settings stored for the camera
           may be others now. */
        sigmask = (1UL<<ncv->ncv_CmdPort->mp_SigBit)|(1UL<<ncv->ncv_TaskMsgPort->mp_SigBit)|
                  SIGBREAKF_CTRL_C|SIGBREAKF_CTRL_D;
        sigs = 0;
        do
        {
            if(sigs & SIGBREAKF_CTRL_D)
            {
                nApplyConfig(ncv);
            }
            /* a unit closed with its stream running (devClose signals) */
            struct NepVideoStream *ns = NULL;
            while((ns = nNextUnit(ncv, ns, TRUE)))
            {
                uvcStop(ncv, ns);
            }
            while((ioreq = (struct IOStdReq *) GetMsg(ncv->ncv_CmdPort)))
            {
                switch(ioreq->io_Command)
                {
                    case NSCMD_UV_SETMODE:
                        ioreq->io_Error = nCmdSetMode(ncv, ioreq);
                        break;

                    case NSCMD_UV_REQUEST:
                        ioreq->io_Error = uvcPassRequest(ncv, ioreq);
                        break;

                    case NSCMD_UV_GETCONTROL:
                    case NSCMD_UV_NEXTCONTROL:
                    case NSCMD_UV_SETCONTROL:
                        ioreq->io_Error = uvcControlCommand(ncv, ioreq);
                        break;

                    case UVCMD_APPLY:
                    {
                        /* the settings window takes back what was done in it */
                        ULONG written;
                        ULONG refused;

                        nApplyControls(ncv, (struct UvcCfgChunk *) ioreq->io_Data, &written, &refused);
                        ioreq->io_Error = 0;
                        break;
                    }

                    case CMD_START:
                        /* A camera may let go of control values when a stream
                           starts: the C930e had another exposure time after a
                           stream than the manual one set before it.
                           So what is stored is put in force once more - unless
                           the settings window is open: then the camera is
                           being set by hand. */
                        if(!(ioreq->io_Error = uvcStart(ncv, (struct NepVideoStream *) ioreq->io_Unit)) &&
                           !ncv->ncv_GUITask)
                        {
                            ULONG written;
                            ULONG refused;

                            nApplyStoredControls(ncv, &written, &refused);
                        }
                        break;

                    case CMD_STOP:
                        uvcStop(ncv, (struct NepVideoStream *) ioreq->io_Unit);
                        ioreq->io_Error = 0;
                        break;

                    default:
                        ioreq->io_Error = IOERR_NOCMD;
                        break;
                }
                ReplyMsg(&ioreq->io_Message);
            }
            /* the transfers that come back here by themselves: the one
               waiting at the status endpoint and the one of each bulk stream
               that runs; requests are waited for where they are sent */
            struct PsdPipe *pp;
            while((pp = (struct PsdPipe *) GetMsg(ncv->ncv_TaskMsgPort)))
            {
                if(pp == ncv->ncv_Status.ust_Pipe)
                {
                    uvcStatusDone(ncv);
                    continue;
                }
                ns = NULL;
                while((ns = nNextUnit(ncv, ns, FALSE)))
                {
                    if(pp == ns->ns_Bulk.ub_Pipe)
                    {
                        uvcBulkDone(ncv, ns);
                        break;
                    }
                }
            }
            sigs = Wait(sigmask);
        } while(!(sigs & SIGBREAKF_CTRL_C));

        KPRINTF(20, ("Going down the river!\n"));
        nFreeVideo(ncv);
    }
}
/* \\\ */

/* /// "nOpenVideo()" */
/* What the subtask needs before it can do anything: the library, its two
   ports, the control pipe and the model of the camera. FALSE = one of them
   could not be had; what was taken until then stays in ncv, for
   nCloseVideo(). */
static BOOL nOpenVideo(struct NepClassVideo *ncv)
{
    if(!(ncv->ncv_Base = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        Alert(AG_OpenLib);
        return FALSE;
    }
    psdGetAttrs(PGA_INTERFACE, ncv->ncv_Interface,
                IFA_InterfaceNum, &ncv->ncv_IfNum,
                TAG_END);
    if(!(ncv->ncv_TaskMsgPort = CreateMsgPort()) ||
       !(ncv->ncv_CmdPort = CreateMsgPort()) ||
       !(ncv->ncv_EP0Pipe = psdAllocPipe(ncv->ncv_Device, ncv->ncv_TaskMsgPort, NULL)))
    {
        return FALSE;
    }
    /* a camera that stops answering must not hold the subtask for ever;
       one that answers with fewer bytes than asked is normal (uvcRequest) */
    psdSetAttrs(PGA_PIPE, ncv->ncv_EP0Pipe,
                PPA_NakTimeout, TRUE,
                PPA_NakTimeoutTime, 5000,
                PPA_AllowRuntPackets, TRUE,
                TAG_END);
    if(!(ncv->ncv_Function = nBuildFunction(ncv)))
    {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "Could not read the video function's descriptors.");
        return FALSE;
    }
    return TRUE;
}
/* \\\ */

/* /// "nCloseVideo()" */
/* What nOpenVideo() took goes back - all of it, or as much as there is:
   every call here passes over what was not taken, and the binding's memory
   starts out cleared. Then whoever waits for the subtask - the binding, the
   release - is told that it is ending. Returns under Forbid(), which lasts
   until the task is gone. */
static void nCloseVideo(struct NepClassVideo *ncv)
{
    /* without the library there is nothing of its to give back */
    if(ncv->ncv_Base)
    {
        psdFreePipe(ncv->ncv_EP0Pipe);
        psdFreeVec(ncv->ncv_Function);
        ncv->ncv_Function = NULL;
    }
    DeleteMsgPort(ncv->ncv_CmdPort);
    DeleteMsgPort(ncv->ncv_TaskMsgPort);
    CloseLibrary(ncv->ncv_Base);
    Forbid();
    /* not listed if the set-up failed */
    if(ncv->ncv_Task)
    {
        Remove(&ncv->ncv_Node);
    }
    ncv->ncv_Task = NULL;
    if(ncv->ncv_ReadySigTask)
    {
        Signal(ncv->ncv_ReadySigTask, 1UL<<ncv->ncv_ReadySignal);
    }
}
/* \\\ */

/* /// "nAllocVideo()" */
/* The subtask sets itself up: the library, pipe and ports, the model, the
   units. NULL = it could not, and everything is given back. */
struct NepClassVideo * nAllocVideo(void)
{
    struct Task *thistask = FindTask(NULL);
    struct NepClassVideo *ncv = thistask->tc_UserData;

    if(!nOpenVideo(ncv))
    {
        nCloseVideo(ncv);
        return NULL;
    }
    nLogFunction(ncv);
    /* before the units appear: from then on the device signals this task
       (devClose) and queues at the binding (NSCMD_UV_WAITEVENT). Listed
       before the settings are applied: a settings change from now on leaves
       Ctrl-D pending for the main loop. */
    Forbid();
    ncv->ncv_Task = thistask;
    AddTail(&ncv->ncv_ClsBase->nh_Bindings, &ncv->ncv_Node);
    Permit();
    uvcStatusStart(ncv);
    nAttachUnits(ncv);
    nApplyConfig(ncv);
    return ncv;
}
/* \\\ */

/* /// "nFreeVideo()" */
/* ...and takes it all down again, the units first */
void nFreeVideo(struct NepClassVideo *ncv)
{
    struct IOStdReq *ioreq;

    /* from here on the device sends this binding nothing new, and nobody
       looks at its model */
    nDetachUnits(ncv);
    while((ioreq = (struct IOStdReq *) GetMsg(ncv->ncv_CmdPort)))
    {
        ioreq->io_Error = UVERR_GONE;
        ReplyMsg(&ioreq->io_Message);
    }
    uvcStatusStop(ncv);
    nCloseVideo(ncv);
}
/* \\\ */
