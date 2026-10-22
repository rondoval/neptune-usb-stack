/*
 *----------------------------------------------------------------------------
 *                         massstorage class for Neptune
 *----------------------------------------------------------------------------
 * Based on the Poseidon massstorage class by Chris Hodges <chrisly@platon42.de>.
 */

#include "debug.h"

#include "massstorage.class.h"

#include "mounter/mounter.h"   /* MountDrive() - RDB/MBR/GPT/superfloppy/CD */

#include <stdarg.h>

#define DEF_NAKTIMEOUT  (300)
/* Optical drives NAK for many seconds while seeking or spinning up, so they get
   a floor under the configured value (deciseconds, like cdc_NakTimeout). */
#define MIN_CD_NAKTIMEOUT (150)
#define DEF_UASQD       (4)

/* MOUNTER_LOG sink: route the mounter's diagnostics to the debug backend. The
   mounter's format strings are %l-normalized for exec RawDoFmt. */
void mounter_log(const char *fmt, ...)
{
#ifdef DEBUG
    va_list args;
    va_start(args, fmt);
    RawDoFmt((CONST_STRPTR) fmt, (APTR) args, (APTR) nep_putch, NULL);
    va_end(args);
#else
    (void) fmt;
#endif
}

/* One row per mountable filesystem: where its handler/dostype/control live in
   the per-device chunk, where its DOS name and buffer count live in the per-LUN
   one, and what a fresh install starts from. This is the only place that knows
   the filesystems apart - the mount recipes, both GUI pages, the config
   defaults and the migration all loop over it, so adding a filesystem (exFAT is
   next) is one entry here plus the config fields it points at.

   Offsets rather than pointers so the table stays a single static const shared
   by every instance. */
struct MSFsDesc
{
    const char *fsd_Label;       /* GUI row label                             */
    const char *fsd_AslTitle;    /* file requester title on the device page   */
    const char *fsd_DefDOSName;  /* fresh-install DOS name...                 */
    ULONG       fsd_DefBuffers;  /* ...and buffer count                       */
    UWORD       fsd_HandlerOff;  /* offsetof(struct ClsDevCfg, cdc_*FSName)   */
    UWORD       fsd_DosTypeOff;  /* offsetof(struct ClsDevCfg, cdc_*DosType)  */
    UWORD       fsd_ControlOff;  /* offsetof(struct ClsDevCfg, cdc_*Control)  */
    UWORD       fsd_UnitOff;     /* offsetof(struct ClsUnitCfg, cuc_*FS)      */
    ULONG       fsd_FSFlags;     /* MOUNTFS_* for the recipe                  */
    UWORD       fsd_StackOff;    /* offsetof(struct ClsDevCfg, cdc_*Stack)    */
    ULONG       fsd_DefStack;    /* fresh-install handler stack in bytes      */
};

/* A handler is not started on less stack than this, whatever was typed */
#define MSFS_MIN_STACK 4096

/* Only the CD splits off by default: a stick keeps landing in one predictable
   UMSD0.. sequence whatever it is formatted with, while a disc gets its own
   UCD* pool and CD-sized buffering (2 KB blocks, read-only streaming). */
static const struct MSFsDesc MSFsTable[MSFS_COUNT] =
{
    [MSFS_FAT] =
    {
        "FAT:", "Select filesystem to use with FAT partitions...", "UMSD", 100,
        offsetof(struct ClsDevCfg, cdc_FATFSName), offsetof(struct ClsDevCfg, cdc_FATDosType),
        offsetof(struct ClsDevCfg, cdc_FATControl), offsetof(struct ClsUnitCfg, cuc_FatFS), 0,
        offsetof(struct ClsDevCfg, cdc_FATStack), 8192
    },
    /* NTFileSystem3G is ntfs-3g behind filesysbox.library; 64 KB is the stack
       recommended for it. With 8 KB the machine went down when an NTFS
       partition was mounted (2026-10-10). */
    [MSFS_NTFS] =
    {
        "NTFS:", "Select filesystem to use with NTFS partitions...", "UMSD", 100,
        offsetof(struct ClsDevCfg, cdc_NTFSName), offsetof(struct ClsDevCfg, cdc_NTFSDosType),
        offsetof(struct ClsDevCfg, cdc_NTFSControl), offsetof(struct ClsUnitCfg, cuc_NTFSFS), 0,
        offsetof(struct ClsDevCfg, cdc_NTFSStack), 65536
    },
    /* exFATFileSystem sizes its own cache and ignores de_NumBuffers (as it does
       de_MaxTransfer and de_Mask); the buffer count is kept for uniformity. It
       is built on filesysbox.library like the NTFS handler and starts from
       the same stack; it has run with 8 KB, and no figure for it is on
       record. */
    [MSFS_EXFAT] =
    {
        "exFAT:", "Select filesystem to use with exFAT partitions...", "UMSD", 100,
        offsetof(struct ClsDevCfg, cdc_ExFATName), offsetof(struct ClsDevCfg, cdc_ExFATDosType),
        offsetof(struct ClsDevCfg, cdc_ExFATControl), offsetof(struct ClsUnitCfg, cuc_ExFATFS), 0,
        offsetof(struct ClsDevCfg, cdc_ExFATStack), 65536
    },
    /* MOUNTFS_FORCELOAD: CD01 is the dostype every controller ROM with a CD
       filesystem in it has already claimed, and those are ISO9660-only. The
       handler the user picked has to win, or a Mac/UDF/High Sierra disc lands
       on a filesystem that cannot read it. A ROM entry stays the fallback for
       machines that carry ODFileSystem there and nowhere else. */
    [MSFS_CD] =
    {
        "CD/DVD:", "Select filesystem to use with CD/DVD media...", "UCD", 25,
        offsetof(struct ClsDevCfg, cdc_CDFSName), offsetof(struct ClsDevCfg, cdc_CDDosType),
        offsetof(struct ClsDevCfg, cdc_CDControl), offsetof(struct ClsUnitCfg, cuc_CDFS),
        MOUNTFS_FORCELOAD,
        offsetof(struct ClsDevCfg, cdc_CDStack), 8192
    },
};

static inline struct MSFsCfg *nUnitFs(struct ClsUnitCfg *cuc, ULONG fs)
{
    return (struct MSFsCfg *) (((UBYTE *) cuc) + MSFsTable[fs].fsd_UnitOff);
}

static inline char *nDevFsHandler(struct ClsDevCfg *cdc, ULONG fs)
{
    return (char *) (((UBYTE *) cdc) + MSFsTable[fs].fsd_HandlerOff);
}

static inline ULONG *nDevFsDosType(struct ClsDevCfg *cdc, ULONG fs)
{
    return (ULONG *) (((UBYTE *) cdc) + MSFsTable[fs].fsd_DosTypeOff);
}

static inline char *nDevFsControl(struct ClsDevCfg *cdc, ULONG fs)
{
    return (char *) (((UBYTE *) cdc) + MSFsTable[fs].fsd_ControlOff);
}

static inline IPTR *nDevFsStack(struct ClsDevCfg *cdc, ULONG fs)
{
    return (IPTR *) (((UBYTE *) cdc) + MSFsTable[fs].fsd_StackOff);
}

/* Seed the per-filesystem mount settings a stored config stopped short of from
   the FAT slot - which is where the single DOS name and buffer count older
   versions applied to every filesystem still lives. An upgrade therefore mounts
   exactly as it did before, and only a fresh install (no stored chunk at all)
   sees the table defaults. Per slot, not all-or-nothing: when exFAT appends a
   fourth slot, configs stored by this version get just that one seeded. */
static void nMigrateUnitFs(struct ClsUnitCfg *cuc, ULONG storedLen)
{
    for(ULONG fs = MSFS_FAT + 1; fs < MSFS_COUNT; fs++)
    {
        /* chunk lengths exclude the 8 byte header the overlay starts after */
        if(storedLen + 8 < MSFsTable[fs].fsd_UnitOff + sizeof(struct MSFsCfg))
        {
            *nUnitFs(cuc, fs) = cuc->cuc_FatFS;
        }
    }
}

/* Fill a mounter filesystem recipe; empty config strings mean "unset". */
static void nFillMountFS(struct MountFS *fs, ULONG dosType, const char *handler,
                         const char *dosName, const char *control,
                         ULONG buffers, ULONG maxTransfer, ULONG fsFlags,
                         ULONG stackSize)
{
    memset(fs, 0, sizeof(*fs));
    fs->dosType = dosType;
    fs->handler = (handler && *handler) ? (const UBYTE *) handler : NULL;
    fs->dosName = (dosName && *dosName) ? (const UBYTE *) dosName : NULL;
    fs->control = (control && *control) ? (const UBYTE *) control : NULL;
    fs->buffers = buffers;
    fs->maxTransfer = maxTransfer;
    fs->fsFlags = fsFlags;
    fs->stackSize = stackSize;
}

/* What the configured CD filesystem is able to cope with, as mounter flags.
   The only handler known to identify disc formats itself is ODFileSystem -
   ISO9660 with Joliet and Rock Ridge, plus High Sierra, UDF, HFS and HFS+ -
   and it is likewise the only one that exposes audio tracks (as virtual WAV
   files). Everything else, legacy CDFileSystem above all, reads ISO9660 and
   nothing more, so it keeps the conservative PVD gate and never sees an audio
   disc. Matched on the basename prefix, so a renamed or suffixed copy
   ("ODFileSystem_020") still counts. */
static ULONG nCDFSCaps(const char *path)
{
    const char *base = path;
    for(const char *p = path; *p; p++)
    {
        if((*p == '/') || (*p == ':'))
        {
            base = p + 1;
        }
    }
    static const char odfs[] = "odfilesystem";
    for(ULONG i = 0; i < sizeof(odfs) - 1; i++)   /* prefix: skip the NUL */
    {
        char c = base[i];
        if((c >= 'A') && (c <= 'Z'))
        {
            c += 'a' - 'A';
        }
        if(c != odfs[i])
        {
            return 0;
        }
    }
    return MOUNTFS_CD_AUDIO|MOUNTFS_CD_ANYFMT;
}

/* Mount one ready unit's media via the mounter: RDB, MBR/GPT and
   superfloppy FAT/NTFS/exFAT, CD/DVD media. What gets mounted and with which
   filesystem comes from the unit/device config; the medium is read through
   our own usbscsi.device (single explicit unit, array form so unit 0 is
   unambiguous). Returns TRUE if it mounted >= 1 partition. */
BOOL nMountDrive(struct NepClassMS *ncm, struct MountResult *stats)
{
    struct ClsDevCfg *cdc = ncm->ncm_CDC;
    struct ClsUnitCfg *cuc = ncm->ncm_CUC;
    struct MountFS fs[MSFS_COUNT];
    struct MountStruct ms;
    struct MountResult result;
    ULONG maxTransfer = (1UL << (cdc->cdc_MaxTransfer + 16)) - 1;
    ULONG unit = ncm->ncm_UnitNo;
    LONG n;

    /* Every filesystem brings its own DOS name and buffer count, so a disc
       lands in the UCD* pool while a stick keeps the UMSD* one. RDB partitions
       are not recipe-mounted at all (the mounter takes their name and
       environment from the RDB itself), so none of this reaches them. */
    for(ULONG i = 0; i < MSFS_COUNT; i++)
    {
        nFillMountFS(&fs[i], *nDevFsDosType(cdc, i), nDevFsHandler(cdc, i),
                     nUnitFs(cuc, i)->fsc_DOSName, nDevFsControl(cdc, i),
                     nUnitFs(cuc, i)->fsc_Buffers, maxTransfer,
                     MSFsTable[i].fsd_FSFlags, *nDevFsStack(cdc, i));
    }

    /* What the configured CD handler can cope with is a property of that handler,
       so it rides on the recipe rather than on the mount session. */
    fs[MSFS_CD].fsFlags |= nCDFSCaps(cdc->cdc_CDFSName);

    memset(&ms, 0, sizeof(ms));
    ms.deviceName  = (const UBYTE *) DEVNAME;          /* "usbscsi.device" */
    ms.units       = &unit;
    ms.unitCount   = 1;
    ms.creatorName = (const UBYTE *) CLASS_NAME;
    ms.SysBase     = EXEC_BASE_NAME;
    ms.fs[MOUNTFS_FAT]   = &fs[MSFS_FAT];
    ms.fs[MOUNTFS_NTFS]  = &fs[MSFS_NTFS];
    ms.fs[MOUNTFS_EXFAT] = &fs[MSFS_EXFAT];
    ms.fs[MOUNTFS_CD]    = &fs[MSFS_CD];
    ms.dmaAlign    = ncm->ncm_DmaAlign;
    if(!cuc->cuc_AutoMountRDB) ms.flags |= MSF_NO_RDB;
    if(!cuc->cuc_AutoMountLegacy) ms.flags |= MSF_NO_LEGACY;
    if(!cuc->cuc_MountAllLegacy)  ms.flags |= MSF_LEGACY_FIRST_ONLY;
    if(!cuc->cuc_AutoMountCD)  ms.flags |= MSF_NO_CD;
    if(!cuc->cuc_Boot)      ms.flags |= MSF_NO_BOOT;

    n = MountDrive(&ms, &result);

    /* Remember whether anything was left for a later pass: before DOS exists the
       mounter cannot LoadSeg() a handler out of L:, so those volumes are counted
       rather than mounted. Nothing deferred means a second MountDrive() has
       nothing to add. */
    ncm->ncm_MountDeferred = (result.deferred != 0);

    /* The caller logs the outcome: neither the neptune.library base nor libname is in
       scope this early in the file, and the removable task has both. */
    if(stats)
    {
        *stats = result;
    }
    KPRINTF(10, ("MountDrive(unit %ld) = %ld\n", ncm->ncm_UnitNo, n));
    return (n > 0);
}

static inline BOOL nIsBulkTransport(ULONG proto)
{
    return (proto == MS_PROTO_BULK) || (proto == MS_PROTO_UAS);
}

/* /// "Lib Stuff" */
const STRPTR libname = CLASS_NAME;

/* LVO table for the embedded usbscsi.device (MakeLibrary'd in libInit). */
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

int libInit(struct NepMSBase * nh)
{
    struct ClsDevCfg *cdc = NULL;
    struct ClsUnitCfg *cuc = NULL;
    struct NepClassMS *ncm;
    struct NepMSBase *ret = NULL;

    if(nDevNameTaken(DEVNAME))
    {
        return FALSE;
    }

    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", nh, EXEC_BASE_NAME));

#define	UtilityBase nh->nh_UtilityBase
    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);
    if(UtilityBase)
    {
        /* Initialize device node & library struct */
        KPRINTF(1, ("UtilityOkay\n"));
        NewList(&nh->nh_Units);
        InitSemaphore(&nh->nh_TaskLock);

        ncm = &nh->nh_DummyNCM;
        ncm->ncm_ClsBase = nh;
        ncm->ncm_Interface = NULL;
        strcpy(ncm->ncm_LUNIDStr, "Default");
        strcpy(ncm->ncm_LUNNumStr, "All");
        ncm->ncm_CDC = cdc = AllocVec(sizeof(struct ClsDevCfg), MEMF_PUBLIC|MEMF_CLEAR);
        ncm->ncm_CUC = cuc = AllocVec(sizeof(struct ClsUnitCfg), MEMF_PUBLIC|MEMF_CLEAR);
        if(cdc && cuc)
        {
            KPRINTF(1, ("MakeLibrary\n"));
            if((nh->nh_DevBase = (struct NepMSDevBase *) MakeLibrary((APTR) DevFuncTable, NULL, (APTR) devInit,
               sizeof(struct NepMSDevBase), NULL)))
            {
                KPRINTF(1,("AddDevice\n"));
                nh->nh_DevBase->np_ClsBase = nh;
                Forbid();
                AddDevice((struct Device *) nh->nh_DevBase);
                nh->nh_DevBase->np_Library.lib_OpenCnt++;
                Permit();
                ret = nh;
            } else {
                KPRINTF(20, ("failed to create usbscsi.device\n"));
            }
        }
    } else {
        KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
    }
    if(!ret)
    {
        FreeVec(cdc);
        FreeVec(cuc);
        CloseLibrary(UtilityBase);
    }
    KPRINTF(10, ("libInit: Ok\n"));
    return(ret ? TRUE : FALSE);
}

int libOpen(struct NepMSBase * nh)
{
    KPRINTF(10, ("libOpen nh: 0x%08lx\n", nh));
    nLoadClassConfig(nh);

    return(TRUE);
}

int libClose(struct NepMSBase * nh)
{
    /* 0, not 1: our own LibClose() in classes/class_main.c decrements
       lib_OpenCnt before calling this hook, so 0 means "that was the last
       close". */
    if(nh->nh_Library.lib_OpenCnt == 0)
    {
        ObtainSemaphore(&nh->nh_TaskLock);
        Forbid();
        nh->nh_ReadySignal = SIGB_SINGLE;
        nh->nh_ReadySigTask = FindTask(NULL);
        if(nh->nh_RemovableTask)
        {
            Signal(nh->nh_RemovableTask, SIGBREAKF_CTRL_C);
        }
        Permit();
        while(nh->nh_RemovableTask)
        {
            Wait(1L<<nh->nh_ReadySignal);
        }
        //FreeSignal(nh->nh_ReadySignal);
        ReleaseSemaphore(&nh->nh_TaskLock);
    }
    KPRINTF(5, ("libClose: lib_OpenCnt = %ld\n", nh->nh_Library.lib_OpenCnt));
    return(TRUE);
}

int libExpunge(struct NepMSBase * nh)
{
    struct NepClassMS *ncm;

    KPRINTF(10, ("libExpunge nh: 0x%08lx\n", nh));

    if((nh->nh_DevBase->np_Library.lib_OpenCnt == 1))
    {
        KPRINTF(1, ("libExpunge: closelibrary utilitybase 0x%08lx\n",
                    UtilityBase));

        ncm = (struct NepClassMS *) nh->nh_Units.lh_Head;
        while(ncm->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ)
        {
            Remove((struct Node *) ncm);
            FreeVec(ncm->ncm_CDC);
            FreeVec(ncm->ncm_CUC);
            FreeVec(ncm->ncm_DevIDString);
            FreeVec(ncm->ncm_IfIDString);
            FreeVec(ncm);
            ncm = (struct NepClassMS *) nh->nh_Units.lh_Head;
        }

        ncm = &nh->nh_DummyNCM;
        FreeVec(ncm->ncm_CDC);
        FreeVec(ncm->ncm_CUC);

        nh->nh_DevBase->np_Library.lib_OpenCnt--;
        RemDevice((struct Device *) nh->nh_DevBase);
        KPRINTF(5, ("libExpunge: Unloading done! massstorage.usbclass expunged!\n\n"));
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

/* /// "usbAttemptInterfaceBinding()" */
/* Prefer the UAS alternate over a BOT interface when it can actually run:
   SuperSpeed device on a hardware whose HCD does stream rings.  Declining the
   BOT offer makes psdHubClassScan offer the inactive alternates next - the
   UAS one is then accepted by the normal protocol. */
static BOOL nPreferUasAlternate(struct Library *ps, struct PsdInterface *pif, struct PsdDevice *pd)
{
    struct List *altlist = NULL;
    struct PsdInterface *altif;
    struct PsdHardware *hw = NULL;
    IPTR issuperspeed = 0;
    IPTR streams = 0;

    psdGetAttrs(PGA_DEVICE, pd,
                DA_IsSuperspeed, &issuperspeed,
                DA_Hardware, &hw,
                TAG_END);
    if(!issuperspeed || !hw)
        return(FALSE);

    psdGetAttrs(PGA_HARDWARE, hw,
                HA_StreamsSupported, &streams,
                TAG_END);
    if(!streams)
        return(FALSE);

    psdGetAttrs(PGA_INTERFACE, pif,
                IFA_AlternateIfList, &altlist,
                TAG_END);
    if(!altlist)
        return(FALSE);

    for (altif = (struct PsdInterface *)altlist->lh_Head;
         altif->pif_Node.ln_Succ;
         altif = (struct PsdInterface *)altif->pif_Node.ln_Succ)
    {
        IPTR aclass = 0;
        IPTR aproto = 0;
        psdGetAttrs(PGA_INTERFACE, altif,
                    IFA_Class, &aclass,
                    IFA_Protocol, &aproto,
                    TAG_END);
        if ((aclass == MASSSTORE_CLASSCODE) && (aproto == MS_PROTO_UAS))
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR)libname, "nPreferUasAlternate: UAS alternate found\n");
            return (TRUE);
        }
    }
    return(FALSE);
}

struct NepClassMS * usbAttemptInterfaceBinding(struct NepMSBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    IPTR ifclass;
    IPTR subclass;
    IPTR proto;
	struct PsdConfig *pc;
    struct PsdDevice *pd;
	IPTR prodid;
    IPTR vendid;
    BOOL preferuas = FALSE;

    KPRINTF(1, ("nepMSAttemptInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Class, &ifclass,
                    IFA_SubClass, &subclass,
                    IFA_Protocol, &proto,
					IFA_Config, &pc,
                    TAG_END);
		psdGetAttrs(PGA_CONFIG, pc,
                    CA_Device, &pd,
					TAG_END);
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ProductID, &prodid,
                    DA_VendorID, &vendid,
                    TAG_END);

        if((ifclass == MASSSTORE_CLASSCODE) && (proto == MS_PROTO_BULK)
           && !(nh->nh_DummyNCM.ncm_CDC->cdc_PatchFlags & PFF_NO_UAS))
        {
            preferuas = nPreferUasAlternate(ps, pif, pd);
        }

        CloseLibrary(ps);

		// Huawei modem, massstorage is useless.
		if( (vendid == 0x12d1 ) && (prodid == 0x1001 || prodid == 0x1003 ) ) return(NULL);

        if(preferuas)
        {
            /* decline BOT: the scan offers the UAS alternate next */
            KPRINTF(5, ("declining BOT interface, UAS alternate preferred\n"));
            return(NULL);
        }

        if((ifclass == MASSSTORE_CLASSCODE) &&
           ((subclass == MS_SCSI_SUBCLASS) ||
            (subclass == MS_RBC_SUBCLASS) ||
            (subclass == MS_ATAPI_SUBCLASS) ||
            (subclass == MS_FDDATAPI_SUBCLASS) ||
            (subclass == MS_UFI_SUBCLASS)) &&
           ((proto == MS_PROTO_BULK) ||
            (proto == MS_PROTO_CB) ||
            (proto == MS_PROTO_CBI) ||
            (proto == MS_PROTO_UAS)))
        {
            return(usbForceInterfaceBinding(nh, pif));
        }
    }
    return(NULL);
}
/* \\\ */

/* /// "nVendorQuirks()" */
/* The hard-coded per-model quirks, as a pure function of the binding's IDs so
   that a config reload re-applies exactly what bind did. The first set is only
   a default for the model: a saved per-device record replaces it, like every
   other default. The second set applies regardless. Neither is subject to
   PFF_NO_FALLBACK - these are known facts about the hardware, not guesses. */
static ULONG nVendorQuirks(struct Library *ps, struct NepClassMS *ncm, BOOL announce)
{
    ULONG vendid = ncm->ncm_UnitVendorID;
    ULONG prodid = ncm->ncm_UnitProdID;
    ULONG pf = 0;

    if(ncm->ncm_UsingDefaultCfg && nIsBulkTransport(ncm->ncm_TPType))
    {
        if(vendid == 0x05e3) /* 2.5 HD Wrapper by Eagle Tec */
        {
            pf |= PFF_FIX_INQ36|PFF_SIMPLE_SCSI|PFF_DELAY_DATA;
            if(announce)
            {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Broken Genesys firmware data phase delay activated. Performance loss!");
            }
        }
        if((vendid == 0x0d7d) && (prodid == 0x1600)) /* HAMA Memory stick */
        {
            pf |= PFF_SIMPLE_SCSI;
        }
        if(((vendid == 0x04cb) && (prodid == 0x0100)) || /* Fujifilm FinePix 1400Zoom */
           ((vendid == 0x0204) && (prodid == 0x6025)) || /* Brock's EXIGO Flashstick */
           ((vendid == 0x0aec) && (prodid == 0x5010))) /* SOYO Multislot Reader */
        {
            pf |= PFF_FIX_INQ36;
        }
        if(((vendid == 0x0c76) && (prodid == 0x0005)) || /* JetFlash */
           ((vendid == 0x066f) && (prodid == 0x8000))) /* Aiptek_mp3-310_128MB.txt */
        {
            pf |= PFF_NO_RESET;
        }

        if(((vendid == 0x059b) && (prodid == 0x0031)) || /* ZIP 100 */
           //((vendid == 0x0aec) && (prodid == 0x5010)) || /* Neodio CF-Reader */
           ((vendid == 0x058f) && (prodid == 0x9380)) || /* guido's stick */
           ((vendid == 0x3579) && (prodid == 0x6901)))
           //((vendid == 0x07c4) && (prodid == 0xb00b)))   /* USB Memory Stick */
        {
            pf |= PFF_SINGLE_LUN;
        }
        if(pf && announce)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Preconfig patchflags 0x%04lx", pf);
        }
    }

    if((vendid == 0x090a) && (prodid == 0x1100))
    {
        pf |= PFF_CLEAR_EP;
        if(announce)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Enabling clear endpoint halt mode for this device!");
        }
    }
    if(vendid == 0x07b4) /* Olympus C-xx */
    {
        pf |= PFF_CSS_BROKEN;
        if(announce)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Workaround for broken Olympus cameras enabled.");
        }
    }
    if(vendid == 0x067b) /* Prolific */
    {
        pf |= PFF_CSS_BROKEN;
        if(announce)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Workaround for broken Prolific signature enabled.");
        }
    }
    if((vendid == 0x0c76) && (prodid == 0x0005))
    {
        pf |= PFF_FIX_INQ36|PFF_FAKE_INQUIRY|PFF_MODE_XLATE;
    }
    return(pf);
}
/* \\\ */

/* /// "usbForceInterfaceBinding()" */
struct NepClassMS * usbForceInterfaceBinding(struct NepMSBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    struct Task *subtask;
    struct NepClassMS *ncm;
    struct NepClassMS *firstncm = NULL;
    struct NepClassMS *tmpncm;
    struct MsgPort *mp;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    struct PsdPipe *pp;
    struct ClsDevCfg *cdc;
    struct ClsUnitCfg *cuc;
    STRPTR devname;
    IPTR ifclass;
    IPTR subclass;
    IPTR proto;
    IPTR ifnum;
    IPTR prodid;
    IPTR vendid;
    ULONG unitno;
    STRPTR devidstr;
    STRPTR ifidstr;
    BOOL  unitfound;
    UBYTE buf[64];
    UBYTE maxlun;
    UWORD lunnum;
    LONG ioerr;
    LONG retry;
    BOOL delayedstore = FALSE;

    KPRINTF(1, ("nepMSForceInterfaceBinding(%08lx)\n", pif));
    if(!(mp = CreateMsgPort()))
    {
        return(NULL);
    }
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Class, &ifclass,
                    IFA_SubClass, &subclass,
                    IFA_Protocol, &proto,
                    IFA_InterfaceNum, &ifnum,
                    IFA_Config, &pc,
                    IFA_IDString, &ifidstr,
                    TAG_DONE);
        psdGetAttrs(PGA_CONFIG, pc,
                    CA_Device, &pd,
                    TAG_END);
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ProductID, &prodid,
                    DA_VendorID, &vendid,
                    DA_ProductName, &devname,
                    DA_IDString, &devidstr,
                    TAG_END);
        maxlun = 0;
        /* Patches and fixes */
        if((proto != MS_PROTO_BULK) && (proto != MS_PROTO_CB) &&
           (proto != MS_PROTO_CBI) && (proto != MS_PROTO_UAS))
        {
            proto = MS_PROTO_BULK;
        }
        if((subclass != MS_SCSI_SUBCLASS) &&
           (subclass != MS_RBC_SUBCLASS) &&
           (subclass != MS_ATAPI_SUBCLASS) &&
           (subclass != MS_FDDATAPI_SUBCLASS) &&
           (subclass != MS_UFI_SUBCLASS))
        {
            subclass = MS_SCSI_SUBCLASS;
        }

        lunnum = 0;
        while(lunnum <= maxlun)
        {
            /*if(firstncm)
            {
                unitno = firstncm->ncm_UnitNo + lunnum;
            } else {
                unitno = lunnum;
            }*/
            Forbid();
            unitfound = FALSE;
            MS_FOREACH_UNIT(nh, ncm)
            {
                if((strcmp(devidstr, ncm->ncm_DevIDString) == 0) &&
                   (strcmp(ifidstr, ncm->ncm_IfIDString) == 0) &&
                   (ncm->ncm_UnitLUN == lunnum))
                {
                    unitno = ncm->ncm_UnitNo;
                    unitfound = TRUE;
                    break;
                }
            }
            if(!unitfound)
            {
                /* as units are freed in the expunge-vector, the memory is
                   outside the scope of neptune.library */
                if(!(ncm = AllocVec(sizeof(struct NepClassMS), MEMF_PUBLIC|MEMF_CLEAR)))
                {
                    Permit();
                    DeleteMsgPort(mp);
                    CloseLibrary(ps);
                    return(NULL);
                }
                ncm->ncm_ClsBase = nh;
                ncm->ncm_UnitNo = (ULONG) ~0;
                ncm->ncm_CDC = cdc = AllocVec(sizeof(struct ClsDevCfg), MEMF_PUBLIC|MEMF_CLEAR);
                ncm->ncm_CUC = cuc = AllocVec(sizeof(struct ClsUnitCfg), MEMF_PUBLIC|MEMF_CLEAR);
                ncm->ncm_DevIDString = AllocVec((ULONG) strlen(devidstr)+1, MEMF_PUBLIC|MEMF_CLEAR);
                ncm->ncm_IfIDString = AllocVec((ULONG) strlen(ifidstr)+1, MEMF_PUBLIC|MEMF_CLEAR);
                if(!(cdc && cuc && ncm->ncm_DevIDString && ncm->ncm_IfIDString))
                {
                    FreeVec(cdc);
                    FreeVec(cuc);
                    FreeVec(ncm->ncm_DevIDString);
                    FreeVec(ncm->ncm_IfIDString);
                    FreeVec(ncm);
                    Permit();
                    DeleteMsgPort(mp);
                    CloseLibrary(ps);
                    return(NULL);
                }
                /* IORequests may be queued even if the task is gone. */
                NewList(&ncm->ncm_Unit.unit_MsgPort.mp_MsgList);
                NewList(&ncm->ncm_XFerQueue);
                NewList(&ncm->ncm_DCInts);
                InitSemaphore(&ncm->ncm_XFerLock);
                /* devOpen can see the unit the moment it is in the list, but
                   nothing owns its message port until nMSTask has come up, and
                   the unit is never unlinked again (see below). So it starts
                   shut and only nMSTask opens it - a bind that dies on the way
                   up simply never clears this, and no zombie unit can be
                   opened. Not implied by MEMF_CLEAR: FALSE is "open". */
                ncm->ncm_DenyRequests = TRUE;
                AddTail(&nh->nh_Units, &ncm->ncm_Unit.unit_MsgPort.mp_Node);
                strcpy(ncm->ncm_DevIDString, devidstr);
                strcpy(ncm->ncm_IfIDString, ifidstr);

            }
            ncm->ncm_Interface = pif;
            ncm->ncm_Device = pd;
            ncm->ncm_Config = pc;
            /* Cache the host controller's DMA buffer alignment. It is a
               property of the HCD the device is attached to, stable for the
               lifetime of this binding. */
            struct PsdHardware *hw = NULL;
            IPTR dmaalign = 0;
            psdGetAttrs(PGA_DEVICE, pd, DA_Hardware, &hw, TAG_END);
            if(hw)
            {
                psdGetAttrs(PGA_HARDWARE, hw, HA_DMAAlignment, &dmaalign, TAG_END);
            }
            ncm->ncm_DmaAlign = (ULONG) dmaalign;
            ncm->ncm_UnitLUN = lunnum;
            ncm->ncm_UnitIfNum = ifnum;
            ncm->ncm_UnitProdID = prodid;
            ncm->ncm_UnitVendorID = vendid;
            ncm->ncm_TPType = proto;
            ncm->ncm_CSType = subclass;
            ncm->ncm_BlockSize = 0;
            ncm->ncm_BlockShift = 9;
            psdSafeRawDoFmt(ncm->ncm_LUNNumStr, 4, "%ld", ncm->ncm_UnitLUN);

            if(!firstncm)
            {
                firstncm = ncm;
            }
            ncm->ncm_UnitLUN0 = firstncm;

            /* class defaults, then the device's own record, then the
               per-model quirks - the same merge every later reload does */
            nLoadBindingConfig(ncm, lunnum == 0);
            ncm->ncm_FallbackAdvised = 0;

            // do this for the first LUN only
            if(lunnum == 0)
            {
                if((pp = psdAllocPipe(pd, mp, NULL)))
                {
                    psdSetAttrs(PGA_PIPE, pp,
                                PPA_NakTimeout, TRUE,
                                PPA_NakTimeoutTime, 1000,
                                TAG_END);
                    if(((vendid == 0x04e6) && ((prodid == 0x0002) || (prodid == 0x000b) || (prodid == 0x000c))) ||
                       ((vendid == 0x050d) && (prodid == 0x0115)) ||
                       ((vendid == 0x07af) && ((prodid == 0x0004) || (prodid == 0x0005))))
                    {
                        UBYTE databyte = 0x01;
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Attempting to do special eUSCSI init...");

                        psdPipeSetup(pp, URTF_VENDOR|URTF_INTERFACE, 0x0c, 0x01, ifnum);
                        ioerr = psdDoPipe(pp, &databyte, 1);
                        if(ioerr)
                        {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                           "eUSCSI_init failed: " MS_IOERR_FMT,
                                           MS_IOERR_ARGS(ioerr));
                        }
                    }
                    if((vendid == 0x1019) && (prodid == 0x0C55))
                    {
                        struct UsbMSCmdBlkWrapper umscbw;
                        struct UsbMSCmdStatusWrapper umscsw;
                        struct PsdPipe *outep, *inep;
                        struct PsdPipe *outpp, *inpp;

                        inep = psdFindEndpoint(pif, NULL,
                                               EA_IsIn, TRUE,
                                               EA_TransferType, USEAF_BULK,
                                               TAG_END);
                        outep = psdFindEndpoint(pif, NULL,
                                                EA_IsIn, FALSE,
                                                EA_TransferType, USEAF_BULK,
                                                TAG_END);
                        if(inep && outep)
                        {
                            inpp = psdAllocPipe(pd, mp, inep);
                            outpp = psdAllocPipe(pd, mp, outep);
                            if(inpp && outpp)
                            {
                                psdSetAttrs(PGA_PIPE, inpp,
                                            PPA_NakTimeout, TRUE,
                                            PPA_NakTimeoutTime, 5000,
                                            TAG_END);
                                psdSetAttrs(PGA_PIPE, outpp,
                                            PPA_NakTimeout, TRUE,
                                            PPA_NakTimeoutTime, 5000,
                                            PPA_NoShortPackets, TRUE,
                                            TAG_END);

                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                               "Attempting to do special UCR-61S2B init...");
                                umscbw.dCBWSignature = AROS_LONG2LE(0x43425355);
                                umscbw.dCBWTag = 0;
                                umscbw.dCBWDataTransferLength = 0;
                                umscbw.bmCBWFlags = 0;
                                umscbw.bCBWLUN = 0;
                                umscbw.bCBWCBLength = 12;
                                memset(umscbw.CBWCB, 0, 16);
                                CopyMem("\xec\x0a\x06\x00$PCCHIPS", umscbw.CBWCB, (ULONG) umscbw.bCBWCBLength);
                                ioerr = psdDoPipe(outpp, &umscbw, UMSCBW_SIZEOF);
                                if(ioerr)
                                {
                                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                   "UCR-61S2B init command failed: " MS_IOERR_FMT,
                                                   MS_IOERR_ARGS(ioerr));
                                }
                                ioerr = psdDoPipe(inpp, &umscsw, UMSCSW_SIZEOF);
                                if(ioerr)
                                {
                                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                   "UCR-61S2B init status failed: " MS_IOERR_FMT,
                                                   MS_IOERR_ARGS(ioerr));
                                }
                            }
                            if(inpp)
                            {
                                psdFreePipe(inpp);
                            }
                            if(outpp)
                            {
                                psdFreePipe(outpp);
                            }
                        }
                    }

                    if((!(ncm->ncm_CDC->cdc_PatchFlags & PFF_SINGLE_LUN)) && (proto != MS_PROTO_UAS))
                    {
                        retry = 3;
                        maxlun = 0;
                        do
                        {
                            psdPipeSetup(pp, URTF_IN|URTF_CLASS|URTF_INTERFACE,
                                         UMSR_GET_MAX_LUN, 0, ifnum);
                            ioerr = psdDoPipe(pp, &maxlun, 1);
                            if(ioerr)
                            {
                                maxlun = 0;
                                if((retry > 1) && (ioerr != UHIOERR_NAKTIMEOUT) && (ioerr != UHIOERR_TIMEOUT))
                                {
                                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                                   "GET_MAX_LUN failed: " MS_IOERR_FMT ", retrying in 0.5secs.",
                                                   MS_IOERR_ARGS(ioerr));
                                    psdDelayMS(500);
                                } else {
                                    psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                                   "GET_MAX_LUN failed: " MS_IOERR_FMT,
                                                   MS_IOERR_ARGS(ioerr));
                                }
                            } else {
                                /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                               "Number of logical units: %ld", (ULONG) maxlun+1);*/
                                break;
                            }
                        } while(--retry);
                        if(ioerr)
                        {
                            /* stored below, once out of Forbid */
                            delayedstore = nTryFallback(ps, ncm, PFF_SINGLE_LUN, "Single LUN", TRUE, RETURN_WARN);
                        }
                        if(maxlun > 7)
                        {
                             psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                                            "MaxLUN value %ld does not seem reasonable. Reducing to %ld.", maxlun, 3);
                             maxlun = 3;
                        }
                    } else {
                        maxlun = 0;
                    }
                    psdFreePipe(pp);
                }
            }
            ncm->ncm_MaxLUN = maxlun;

            /* Find next free unit number */
            if(ncm->ncm_UnitNo == (ULONG) ~0)
            {
                unitno = ncm->ncm_CUC->cuc_DefaultUnit;
                tmpncm = (struct NepClassMS *) nh->nh_Units.lh_Head;
                while(tmpncm->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ)
                {
                    if(tmpncm->ncm_UnitNo == unitno)
                    {
                        unitno++;
                        tmpncm = (struct NepClassMS *) nh->nh_Units.lh_Head;
                    } else {
                        tmpncm = (struct NepClassMS *) tmpncm->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ;
                    }
                }
                ncm->ncm_UnitNo = unitno;
            }
            Permit();

            if(delayedstore)
            {
                nStoreConfig(ps, ncm);
                delayedstore = FALSE;
            }
            psdSafeRawDoFmt(buf, 64, CLASS_NAME "<%08lx,%ld>", ncm, lunnum);
            ncm->ncm_ReadySignal = SIGB_SINGLE;
            ncm->ncm_ReadySigTask = FindTask(NULL);
            SetSignal(0, SIGF_SINGLE);
            subtask = psdSpawnSubTask(buf, (APTR) nMSTask, ncm);
            if(subtask)
            {
                psdBorrowLocksWait(subtask, 1UL<<ncm->ncm_ReadySignal);
                if(ncm->ncm_Task)
                {
                    //ncm->ncm_ReadySigTask = NULL;
                    //FreeSignal(ncm->ncm_ReadySignal);
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "Mounted '%s' LUN %ld as %s unit %ld.",
                                   devname, lunnum, nh->nh_DevBase->np_Library.lib_Node.ln_Name,
                                   ncm->ncm_UnitNo);
                    //lunnum++;
                    //continue;
                }
            }
            ncm->ncm_ReadySigTask = NULL;
            //FreeSignal(ncm->ncm_ReadySignal);
            /* Get rid of unit structure */
            /* Well, actually don't. maybe we will be able to get back to it later */
            /*Forbid();
            Remove(ncm);
            Permit();
            FreeVec(ncm);
            if(firstncm == ncm)
            {
                firstncm = NULL;
            }*/
            lunnum++;
        }
        CloseLibrary(ps);
    }
    DeleteMsgPort(mp);
    return(firstncm);
}
/* \\\ */

/* /// "usbReleaseInterfaceBinding()" */
void usbReleaseInterfaceBinding(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    struct Library *ps;
    STRPTR devname;
    struct NepClassMS *ncmhead;

    KPRINTF(1, ("nepMSReleaseInterfaceBinding(%08lx)\n", ncm));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        Forbid();
        if(ncm->ncm_GUITask)
        {
            Signal(ncm->ncm_GUITask, SIGBREAKF_CTRL_C);
        }
        Permit();
        psdGetAttrs(PGA_DEVICE, ncm->ncm_Device, DA_ProductName, &devname, TAG_END);
        ncmhead = ncm;
        MS_FOREACH_UNIT(nh, ncm)
        {
            KPRINTF(10, ("ncm = %08lx, ncmhead = %08lx, unit0 = %08lx\n", ncm, ncmhead, ncm->ncm_UnitLUN0));
            if((ncm->ncm_UnitLUN0 == ncmhead) && (ncm->ncm_Task))
            {
                /*ncm->ncm_UnitLUN0 = NULL;*/
                Forbid();
                ncm->ncm_ReadySignal = SIGB_SINGLE;
                ncm->ncm_ReadySigTask = FindTask(NULL);
                ncm->ncm_DenyRequests = TRUE;
                if(ncm->ncm_Task)
                {
                    Signal(ncm->ncm_Task, SIGBREAKF_CTRL_C);
                }
                Permit();
                while(ncm->ncm_Task)
                {
                    psdBorrowLocksWait(ncm->ncm_Task, 1UL<<ncm->ncm_ReadySignal);
                }
                //FreeSignal(ncm->ncm_ReadySignal);
            }
        }
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       NEP_RELEASED_TXT,
                       devname);
        CloseLibrary(ps);
    }
}
/* \\\ */

/* /// "usbGetAttrsA()" */
LONG (usbGetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepMSBase * nh asm("a6"))
{

    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("nepMSGetAttrsA(%ld, %08lx, %08lx)\n", type, usbstruct, tags));
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
                 *((STRPTR *) ti->ti_Data) = "Supports storage devices via usbscsi.device";
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
                 *((IPTR *) ti->ti_Data) = nh->nh_DummyNCM.ncm_UsingDefaultCfg;
                 count++;
             }
             if((ti = FindTagItem(UCCA_SupportsSuspend, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_SupportsSafeEject, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             break;

         case UGA_BINDING:
             if((ti = FindTagItem(UCBA_UsingDefaultCfg, tags)))
             {
                 *((IPTR *) ti->ti_Data) = ((struct NepClassMS *) usbstruct)->ncm_UsingDefaultCfg;
                 count++;
             }
             break;

    }
    return(count);
}
/* \\\ */

/* /// "usbSetAttrsA()" */
LONG (usbSetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepMSBase * nh asm("a6"))
{
    return(0);
}
/* \\\ */

/* /// "usbDoMethodA()" */
IPTR (usbDoMethodA)(ULONG methodid asm("d0"), IPTR * methoddata asm("a1"), struct NepMSBase * nh asm("a6"))
{

    struct NepClassMS *ncm;

    KPRINTF(10, ("Do Method %ld\n", methodid));
    switch(methodid)
    {
        case UCM_AttemptInterfaceBinding:
            return((IPTR) usbAttemptInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ForceInterfaceBinding:
            return((IPTR) usbForceInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ReleaseInterfaceBinding:
            usbReleaseInterfaceBinding(nh, (struct NepClassMS *) methoddata[0]);
            return(TRUE);

        case UCM_OpenCfgWindow:
            return(nOpenBindingCfgWindow(nh, &nh->nh_DummyNCM));

        case UCM_OpenBindingCfgWindow:
            return(nOpenBindingCfgWindow(nh, (struct NepClassMS *) methoddata[0]));

        case UCM_ConfigChangedEvent:
            nReapplyConfig(nh);
            return(TRUE);

        case UCM_AttemptSuspendDevice:
            ncm = (struct NepClassMS *) methoddata[0];
            /* Refuse while commands are on the wire (mirrors usbaudio's
               refuse-while-streaming): SET_SUSPEND(1) would park the rings
               under them.  ncm_CmdBusy = sync funnel depth, UAS tags = the
               async QD>1 engine, XFerLock = a sibling LUN mid-command.
               Queued-but-unstarted IO is fine - psdDoPipe auto-resumes.
               The tiny agree-to-park window that remains is bounded by the
               HCD (aborts complete on suspended endpoints). */
            if(ncm->ncm_CmdBusy || !nUasTagsIdle(ncm))
            {
                return(FALSE);
            }
            if(ncm->ncm_MaxLUN)
            {
                if(!AttemptSemaphore(&ncm->ncm_UnitLUN0->ncm_XFerLock))
                {
                    return(FALSE);
                }
                ReleaseSemaphore(&ncm->ncm_UnitLUN0->ncm_XFerLock);
            }
            ncm->ncm_Running = FALSE;
            return(TRUE);

        case UCM_AttemptResumeDevice:
            ncm = (struct NepClassMS *) methoddata[0];
            ncm->ncm_Running = TRUE;
            nWakeUnitTask(ncm);
            return(TRUE);

        case UCM_SafeEject:
            return(nSafeEjectDevice(nh, (struct NepClassMS *) methoddata[0],
                                    (STRPTR) methoddata[1], (ULONG) methoddata[2]));

        /* Units that have not settled yet, for the ROM boot gate. Forbid()
           rather than a semaphore: the caller is the coldstart chain polling us
           every fraction of a second, and the removable task must not be made to
           wait on it. Reading two words per unit under Forbid() is cheap enough
           that no lock is worth the coupling. */
        case UCM_MediaPending:
        {
            IPTR pending = 0;

            Forbid();
            MS_FOREACH_UNIT(nh, ncm)
            {
                if(ncm->ncm_MediaUnsettled && (!ncm->ncm_DenyRequests))
                {
                    pending++;
                }
            }
            Permit();
            return(pending);
        }

        default:
            break;
    }
    return(0);
}
/* \\\ */

/* /// "nLoadClassConfig()" */
BOOL nLoadClassConfig(struct NepMSBase *nh)
{
    struct NepClassMS *ncm = &nh->nh_DummyNCM;
    struct Library *ps;
    struct ClsDevCfg *cdc;
    struct ClsUnitCfg *cuc;
    struct PsdIFFContext *pic;

    KPRINTF(10, ("Loading Class Config...\n"));
    if(ncm->ncm_GUITask)
    {
        return(FALSE);
    }
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }

    Forbid();
    /* Create default config */
    cdc = ncm->ncm_CDC;
    cdc->cdc_ChunkID = AROS_LONG2BE(MAKE_ID('M','S','D','C'));
    cdc->cdc_Length = AROS_LONG2BE(sizeof(struct ClsDevCfg)-8);
    cdc->cdc_NakTimeout = DEF_NAKTIMEOUT;
    cdc->cdc_PatchFlags = PFF_MODE_XLATE|PFF_NO_RESET|PFF_FIX_INQ36|PFF_SIMPLE_SCSI;
    cdc->cdc_StartupDelay = 0;
    cdc->cdc_MaxTransfer = 5;
    cdc->cdc_UasQueueDepth = DEF_UASQD;
    /* filesystem defaults; all changeable in the GUI */
    cdc->cdc_FATDosType = 0x46415401;                  /* FAT\1 */
    strcpy(cdc->cdc_FATFSName, "L:fat95");
    cdc->cdc_CDDosType = 0x43443031;                   /* CD01 */
    strcpy(cdc->cdc_CDFSName, "L:ODFileSystem");
    cdc->cdc_NTFSDosType = 0x4e544653;                 /* NTFS */
    strcpy(cdc->cdc_NTFSName, "L:NTFileSystem3G");
    cdc->cdc_ExFATDosType = 0x46415458;                /* FATX */
    strcpy(cdc->cdc_ExFATName, "L:exFATFileSystem");
    for(ULONG fs = 0; fs < MSFS_COUNT; fs++)
    {
        *nDevFsStack(cdc, fs) = MSFsTable[fs].fsd_DefStack;
    }

    cuc = ncm->ncm_CUC;
    cuc->cuc_ChunkID = AROS_LONG2BE(MAKE_ID('L','U','N','0'));
    cuc->cuc_Length = AROS_LONG2BE(sizeof(struct ClsUnitCfg)-8);
    cuc->cuc_AutoMountLegacy = TRUE;
    for(ULONG fs = 0; fs < MSFS_COUNT; fs++)
    {
        strcpy(nUnitFs(cuc, fs)->fsc_DOSName, MSFsTable[fs].fsd_DefDOSName);
        nUnitFs(cuc, fs)->fsc_Buffers = MSFsTable[fs].fsd_DefBuffers;
    }
    cuc->cuc_AutoMountRDB = TRUE;
    cuc->cuc_Boot = TRUE;
    cuc->cuc_DefaultUnit = 0;
    cuc->cuc_AutoUnmount = TRUE;
    cuc->cuc_MountAllLegacy = TRUE;
    cuc->cuc_AutoMountCD = TRUE;

    ncm->ncm_UsingDefaultCfg = TRUE;
    /* try to load default config */
    pic = psdGetClsCfg(libname);
    if(pic)
    {
        cdc = psdGetCfgChunk(pic, AROS_LONG2BE(ncm->ncm_CDC->cdc_ChunkID));
        if(cdc)
        {
            CopyMem(((UBYTE *) cdc) + 8, ((UBYTE *) ncm->ncm_CDC) + 8, min(AROS_LONG2BE(cdc->cdc_Length), AROS_LONG2BE(ncm->ncm_CDC->cdc_Length)));
            psdFreeVec(cdc);
            ncm->ncm_UsingDefaultCfg = FALSE;
        }
        cuc = psdGetCfgChunk(pic, AROS_LONG2BE(ncm->ncm_CUC->cuc_ChunkID));
        if(cuc)
        {
            ULONG storedLen = AROS_LONG2BE(cuc->cuc_Length);
            CopyMem(((UBYTE *) cuc) + 8, ((UBYTE *) ncm->ncm_CUC) + 8, min(storedLen, AROS_LONG2BE(ncm->ncm_CUC->cuc_Length)));
            psdFreeVec(cuc);
            nMigrateUnitFs(ncm->ncm_CUC, storedLen);
            ncm->ncm_UsingDefaultCfg = FALSE;
        }
    }
    Permit();
    CloseLibrary(ps);
    return(FALSE);
}
/* \\\ */

/* /// "nLoadBindingConfig()" */
/* Class defaults, overlaid by the device's own record, plus the per-model
   quirks. Bind and every reload go through here, so the effective flag set is
   the same both times. announce logs the per-model quirks (bind only). */
BOOL nLoadBindingConfig(struct NepClassMS *ncm, BOOL announce)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    struct Library *ps;
    struct ClsDevCfg *cdc;
    struct ClsUnitCfg *cuc;
    struct PsdIFFContext *pic;

    KPRINTF(10, ("Loading Binding Config...\n"));
    if(ncm->ncm_GUITask)
    {
        return(FALSE);
    }
    //nLoadClassConfig(nh);
    *ncm->ncm_CDC = *nh->nh_DummyNCM.ncm_CDC;
    *ncm->ncm_CUC = *nh->nh_DummyNCM.ncm_CUC;
    ncm->ncm_CUC->cuc_ChunkID = AROS_LONG2BE(MAKE_ID('L','U','N','0')+ncm->ncm_UnitLUN);
    ncm->ncm_UsingDefaultCfg = TRUE;

    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }

    Forbid();
    /* Load config */
    pic = psdGetUsbDevCfg(libname, ncm->ncm_DevIDString, ncm->ncm_IfIDString);
    if(pic)
    {
        cdc = psdGetCfgChunk(pic, AROS_LONG2BE(ncm->ncm_CDC->cdc_ChunkID));
        if(cdc)
        {
            CopyMem(((UBYTE *) cdc) + 8, ((UBYTE *) ncm->ncm_CDC) + 8, min(AROS_LONG2BE(cdc->cdc_Length), AROS_LONG2BE(ncm->ncm_CDC->cdc_Length)));
            psdFreeVec(cdc);
            ncm->ncm_UsingDefaultCfg = FALSE;
        }
        cuc = psdGetCfgChunk(pic, AROS_LONG2BE(ncm->ncm_CUC->cuc_ChunkID));
        if(cuc)
        {
            /* the class defaults this overlays are already migrated, but a
               device-specific chunk of its own age seeds from ITS FAT name */
            ULONG storedLen = AROS_LONG2BE(cuc->cuc_Length);
            CopyMem(((UBYTE *) cuc) + 8, ((UBYTE *) ncm->ncm_CUC) + 8, min(storedLen, AROS_LONG2BE(ncm->ncm_CUC->cuc_Length)));
            psdFreeVec(cuc);
            nMigrateUnitFs(ncm->ncm_CUC, storedLen);
            ncm->ncm_UsingDefaultCfg = FALSE;
        }
    }
    Permit();
    ncm->ncm_CDC->cdc_PatchFlags |= nVendorQuirks(ps, ncm, announce);
    CloseLibrary(ps);
    return(FALSE);
}
/* \\\ */

/* /// "nReapplyConfig()" */
/* Re-read the prefs into every live unit after a config change. Each unit
   task re-arms its own pipes (see ncm_ApplyNak); touching them from here
   would race nFreeMS(). Whatever bind latched stays as it is: UAS queue depth,
   startup delay, unit number, and the mounts. */
void nReapplyConfig(struct NepMSBase *nh)
{
    struct NepClassMS *ncm;

    nLoadClassConfig(nh);
    Forbid();
    MS_FOREACH_UNIT(nh, ncm)
    {
        nLoadBindingConfig(ncm, FALSE);
        /* set even with no task up: the main loop looks before its first Wait */
        ncm->ncm_ApplyNak = TRUE;
        nWakeUnitTask(ncm);
    }
    Permit();
}
/* \\\ */

/* /// "nStoreConfig()" */
/* ps is the caller's own neptune.library base: bind, the unit task and the GUI task
   all store through here. */
BOOL nStoreConfig(struct Library *ps, struct NepClassMS *ncm)
{
    APTR pic;
    struct NepClassMS *cncm;
    if(ncm->ncm_Interface)
    {
        /* The ROM stack binds before DOS exists, so no prefs have been read
           yet and cdc_* holds the hard-coded defaults. Storing now would
           freeze those into a per-device record that shadows the real class
           defaults from then on. psdReadCfg() usually discards it when the
           prefs arrive, but on a machine that never loads them the next
           NeptuneUSB Save would commit it for good. */
        IPTR cfgread = FALSE;
        psdGetAttrs(PGA_STACK, NULL, PA_ConfigRead, &cfgread, TAG_END);
        if(!cfgread)
        {
            return(FALSE);
        }
        pic = psdGetUsbDevCfg(libname, ncm->ncm_DevIDString, ncm->ncm_IfIDString);
        if(!pic)
        {
            psdSetUsbDevCfg(libname, ncm->ncm_DevIDString, ncm->ncm_IfIDString, NULL);
            pic = psdGetUsbDevCfg(libname, ncm->ncm_DevIDString, ncm->ncm_IfIDString);
        }
        if(pic)
        {
            psdAddCfgEntry(pic, ncm->ncm_CDC);
            cncm = ncm;
            while(((struct Node *) cncm)->ln_Succ)
            {
                if(cncm->ncm_UnitLUN0 != ncm)
                {
                    break;
                }
                psdAddCfgEntry(pic, cncm->ncm_CUC);
                cncm = (struct NepClassMS *) ((struct Node *) cncm)->ln_Succ;
            }
            return(TRUE);
        }
    } else {
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "nTryFallback()" */
/* The one gate for a learned quirk: set (or, with !enable, clear) flag in the
   binding's patch flags. Returns TRUE only if the flag actually changed, so
   the caller can do its own follow-up - persisting it, normally through
   nApplyFallback(). With PFF_NO_FALLBACK the change is refused and the log
   names the switch the device seems to want instead. name is the switch's GUI
   label. The hard-coded per-model quirks are not learned and do not come
   through here (see nVendorQuirks). ps is the caller's own neptune.library base. */
BOOL nTryFallback(struct Library *ps, struct NepClassMS *ncm, ULONG flag,
                  CONST_STRPTR name, BOOL enable, LONG rc)
{
    ULONG pf = ncm->ncm_CDC->cdc_PatchFlags;

    if(enable ? (pf & flag) : !(pf & flag))
    {
        return(FALSE);
    }
    if(pf & PFF_NO_FALLBACK)
    {
        /* once per binding: some callers sit on the per-IO path */
        if(!(ncm->ncm_FallbackAdvised & flag))
        {
            ncm->ncm_FallbackAdvised |= flag;
            psdAddErrorMsg(rc, (STRPTR) libname,
                           enable ? "\"%s\" is probably needed for this device. Please check this."
                                  : "\"%s\" is probably enabled incorrectly for this device. Please check this.",
                           (STRPTR) name);
        }
        return(FALSE);
    }
    ncm->ncm_CDC->cdc_PatchFlags = enable ? (pf | flag) : (pf & ~flag);
    psdAddErrorMsg(rc, (STRPTR) libname,
                   enable ? "Fallback: Enabling %s." : "Fallback: Disabling %s.",
                   (STRPTR) name);
    return(TRUE);
}
/* \\\ */

/* /// "nOpenBindingCfgWindow()" */
LONG nOpenBindingCfgWindow(struct NepMSBase *nh, struct NepClassMS *ncm)
{
    struct Library *ps;
    KPRINTF(10, ("Opening GUI...\n"));
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }
    Forbid();
    if(!ncm->ncm_GUITask)
    {
        if((ncm->ncm_GUITask = psdSpawnSubTask(CLASS_NAME " GUI", nGUITask, ncm)))
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
#define ps ncm->ncm_Base

const STRPTR DeviceTypeStrings[] =
{
    "Direct Access",
    "Sequential Access",
    "Printer",
    "Processor",
    "Worm",
    "CD/DVD ROM",
    "Scanner",
    "Optical",
    "Medium Changer",
    "Communications",
    "Arts 1",
    "Arts 2",
    "RAID",
    "Enclosure",
    "Simple Direct Access",
    "Optical Card",
    "Reserved",
    "Object Based"
};

const UWORD PrimeTable[] =
{
    5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
    73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149,
    151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223, 227,
    229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307,
    311, 313, 317, 331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389,
    397, 401, 409, 419, 421, 431, 433, 439, 443, 449, 457, 461, 463, 467,
    479, 487, 491, 499, 503, 509, 521, 523, 541, 547, 557, 563, 569, 571,
    577, 587, 593, 599, 601, 607, 613, 617, 619, 631, 641, 643, 647, 653,
    659, 661, 673, 677, 683, 691, 701, 709, 719, 727, 733, 739, 743, 751,
    757, 761, 769, 773, 787, 797, 809, 811, 821, 823, 827, 829, 839, 853,
    857, 859, 863, 877, 881, 883, 887, 907, 911, 919, 929, 937, 941, 947,
    953, 967, 971, 977, 983, 991, 997, 1009, 1013, 1019, 1021, 1031, 1033,
    1039, 1049, 1051, 1061, 1063, 1069, 1087, 1091, 1093, 1097, 1103, 1109,
    1117, 1123, 1129, 1151, 1153, 1163, 1171, 1181, 1187, 1193, 1201, 1213,
    1217, 1223, 1229, 1231, 1237, 1249, 1259, 1277, 1279, 1283, 1289, 1291,
    1297, 1301, 1303, 1307, 1319, 1321, 1327, 1361, 1367, 1373, 1381, 1399,
    1409, 1423, 1427, 1429, 1433, 1439, 1447, 1451, 1453, 1459, 1471, 1481,
    1483, 1487, 1489, 1493, 1499, 1511, 1523, 1531, 1543, 1549, 1553, 1559,
    1567, 1571, 1579, 1583, 1597, 1601, 1607, 1609, 1613, 1619, 1621, 1627,
    1637, 1657, 1663, 1667, 1669, 1693, 1697, 1699, 1709, 1721, 1723, 1733,
    1741, 1747, 1753, 1759, 1777, 1783, 1787, 1789, 1801, 1811, 1823, 1831,
    1847, 1861, 1867, 1871, 1873, 1877, 1879, 1889, 1901, 1907, 1913, 1931,
    1933, 1949, 1951, 1973, 1979, 1987, 1993, 1997, 1999, 0
};

/* /// "nHexString()" */
void nHexString(UBYTE *src, ULONG len, UBYTE *buf)
{
    static const char hexchars[] = "0123456789ABCDEF";
    UWORD cnt = 0;

    while(cnt < len)
    {
        if(cnt++)
        {
            *buf++ = ' ';
        }
        *buf++ = hexchars[*src>>4];
        *buf++ = hexchars[*src++ & 0xf];
    }
    *buf = 0;
}
/* \\\ */

/* /// "nMSTask()" */
void nMSTask()
{

    struct NepClassMS *ncm;
    ULONG sigmask;
    ULONG sigs;
    LONG ioerr;
    UWORD cnt;

    struct IOStdReq *ioreq;
    struct IOStdReq *ioreq2;

    struct SCSICmd scsicmd;
    UBYTE inquirydata[36];
    //UBYTE buf[256];
    UBYTE cmd6[6];
    UBYTE sensedata[18];

    if((ncm = nAllocMS()))
    {
        Forbid();
        /* The task is up and owns the unit's message port, so the unit is open
           for business - and it has to be open *here*, before the startup reset
           below: nBulkReset() bails out on ncm_DenyRequests, which is set both
           at unit creation and by the previous binding's teardown. Clearing it
           further down meant the reset was skipped on every rebind and the
           "No Reset" fallback got persisted to the stored config. */
        ncm->ncm_DenyRequests = FALSE;
        /* units are reused across replugs: a latch left over from a safe
           eject would silently veto every future mount of this unit */
        ncm->ncm_Ejected = FALSE;
        ncm->ncm_RemountPending = FALSE;
        if(ncm->ncm_ReadySigTask)
        {
            Signal(ncm->ncm_ReadySigTask, 1L<<ncm->ncm_ReadySignal);
        }
        Permit();

        if(ncm->ncm_CDC->cdc_PatchFlags)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Postconfig patchflags 0x%04lx%s%s%s%s%s%s%s%s%s%s%s%s%s%s.",
                           ncm->ncm_CDC->cdc_PatchFlags,
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_SINGLE_LUN) ? " SingleLun" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_MODE_XLATE) ? " ModeXLate" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_EMUL_LARGE_BLK) ? " EmulLarge" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_REM_SUPPORT) ? " RemSupport" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_INQ36) ? " FixInq36" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_DELAY_DATA) ? " DelayData" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI) ? " SimpleSCSI" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_NO_RESET) ? " NoReset" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_FAKE_INQUIRY) ? " FakeInq" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_CAPACITY) ? " FixCapacity" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_NO_FALLBACK) ? " NoFallback" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_CSS_BROKEN) ? " CSSBroken" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_CLEAR_EP) ? " ClearEP" : "",
                           (ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG) ? " Debug" : "");
        }

        if(ncm->ncm_CDC->cdc_StartupDelay)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Delaying init sequence by %ld00ms.",
                           ncm->ncm_CDC->cdc_StartupDelay);

            psdDelayMS(ncm->ncm_CDC->cdc_StartupDelay*100);
        }
        if(!(ncm->ncm_CDC->cdc_PatchFlags & PFF_NO_RESET))
        {
            nLockXFer(ncm);
            if(nBulkReset(ncm))
            {
                nApplyFallback(ncm, PFF_NO_RESET, "No Initial Reset", TRUE, RETURN_WARN);
            }
            nUnlockXFer(ncm);
        }

        ncm->ncm_UnitReady = FALSE;
        ncm->ncm_Removable = TRUE;

        scsicmd.scsi_Data = (UWORD *) inquirydata;
        scsicmd.scsi_Length = 36;
        scsicmd.scsi_Command = cmd6;
        scsicmd.scsi_CmdLength = 6;
        scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
        scsicmd.scsi_SenseData = sensedata;
        scsicmd.scsi_SenseLength = 18;
        cmd6[0] = SCSI_INQUIRY;
        cmd6[1] = 0;
        cmd6[2] = 0;
        cmd6[3] = 0;
        cmd6[4] = 36;
        cmd6[5] = 0;
        if((ioerr = nScsiDirect(ncm, &scsicmd)))
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "SCSI_INQUIRY failed: %ld",
                           ioerr);
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Try increasing the startup delay value or fake inquiry.");
        } else {
            CopyMem(&inquirydata[16], ncm->ncm_LUNIDStr, 16);
            ncm->ncm_LUNIDStr[15] = '\0';

            // trim spaces
            cnt = strlen(ncm->ncm_LUNIDStr);
            while(cnt--)
            {
                if(ncm->ncm_LUNIDStr[cnt] == ' ')
                {
                    ncm->ncm_LUNIDStr[cnt] = '\0';
                } else {
                    break;
                }
            }

            ncm->ncm_DeviceType = inquirydata[0] & PDT_MASK;
            if(ncm->ncm_DeviceType > 0x11)
            {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Illegal Device Type %02lx", ncm->ncm_DeviceType);
                ncm->ncm_DeviceType = 0;
            } else {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Device '%s' is of %s type.", ncm->ncm_LUNIDStr, DeviceTypeStrings[ncm->ncm_DeviceType]);
            }
            if((ncm->ncm_DeviceType == PDT_WORM) ||
               (ncm->ncm_DeviceType == PDT_CDROM))
            {
                // assume 2048 byte blocks
                ncm->ncm_BlockSize = 2048;
                ncm->ncm_BlockShift = 11;
                /* The stored config is deliberately not touched: the default
                   is already above the floor, so getting here means the value
                   was set on purpose, and nStoreConfig() would write back the
                   whole chunk. */
                ULONG nakms = nNakTimeoutMs(ncm);
                if(nakms != ncm->ncm_CDC->cdc_NakTimeout*100)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Raising NAK Timeout to %ld seconds for CD/DVD drives (configured %ld00ms).",
                                   (ULONG) (MIN_CD_NAKTIMEOUT/10),
                                   (ULONG) ncm->ncm_CDC->cdc_NakTimeout);
                    nApplyNakTimeout(ncm, nakms);
                }
            }

            /* Both budgets apply to every unit: a fixed disk skips the TEST UNIT READY
               question entirely, but its first mount can fail for the same reasons a
               removable one's can. */
            ncm->ncm_SenseRetries = RT_SENSE_RETRIES;
            ncm->ncm_MountRetries = RT_MOUNT_RETRIES;

            if(!(inquirydata[1] & 0x80))
            {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Device does not seem to use removable media.");
                ncm->ncm_Removable = FALSE;
                ncm->ncm_UnitReady = TRUE;
                ncm->ncm_ChangeCount++;
            } else {
                /* Unsettled until the removable task has actually mounted this unit
                   or established that it cannot. Without this the window between
                   binding and the first sweep reads as "nothing coming". */
                ncm->ncm_MediaUnsettled = TRUE;
            }
        }

        /* Main task */
        sigmask = (1L<<ncm->ncm_Unit.unit_MsgPort.mp_SigBit)|
                  (1L<<ncm->ncm_TaskMsgPort->mp_SigBit)|
                  SIGBREAKF_CTRL_C;
        if(ncm->ncm_CSType == MS_UFI_SUBCLASS)
        {
            struct IOStdReq tmpio;
            tmpio.io_Command = CMD_START;
            nStartStop(ncm, &tmpio);
        }

        // if not removable, call it once to trigger mounting...
        if(!ncm->ncm_Removable)
        {
            ncm->ncm_ForceRTCheck = TRUE;
        }
        ncm->ncm_Running = TRUE;

        do
        {
            if(ncm->ncm_ApplyNak)
            {
                /* nReapplyConfig() reloaded the config under us */
                ncm->ncm_ApplyNak = FALSE;
                nApplyNakTimeout(ncm, nNakTimeoutMs(ncm));
            }
            if(ncm->ncm_Removable || ncm->ncm_ForceRTCheck)
            {
                nStartRemovableTask(ps, ncm->ncm_ClsBase);
                ncm->ncm_ForceRTCheck = FALSE;
            }
            if(ncm->ncm_UasQueueDepth)
            {
                /* collect tag completions first: finishes ioreqs, frees tags */
                nUasProcessAborts(ncm);
                nUasReapTags(ncm);
                /* then the ABORT TASK ladder for any tag the device still
                   owns after a host-side kill */
                nUasProcessQuarantine(ncm);
            }
            /* unit port -> FIFO; Forbid because devAbortIO scans the queue */
            Forbid();
            while((ioreq = (struct IOStdReq *) GetMsg(&ncm->ncm_Unit.unit_MsgPort)))
            {
                AddTail(&ncm->ncm_XFerQueue, &ioreq->io_Message.mn_Node);
            }
            Permit();
            /* drain from the head: an eligible block-IO request rides a free
               tag; anything else is a barrier - every tag completes first,
               then the synchronous handler runs exactly as before */
            for(;;)
            {
                struct UasTag *ut = NULL;

                Forbid();
                ioreq = (struct IOStdReq *) ncm->ncm_XFerQueue.lh_Head;
                if(!ioreq->io_Message.mn_Node.ln_Succ)
                {
                    Permit();
                    break;
                }
                if(nUasEligible(ncm, ioreq))
                {
                    if(!(ut = nUasFreeTag(ncm)))
                    {
                        Permit();
                        break; /* every tag busy: wait for a completion */
                    }
                }
                Remove(&ioreq->io_Message.mn_Node);
                Permit();

                KPRINTF(5, ("command ioreq: 0x%08lx cmd: %lu len: %ld\n",
                        ioreq, ioreq->io_Command, ioreq->io_Length));

                if(ut)
                {
                    nUasSubmitTag(ncm, ut, ioreq);
                    continue;
                }

                /* No blanket barrier any more: every SCSI-carrying handler
                   below reaches the wire through nScsiDirect(), and on UAS
                   that takes a tag of its own (nUasClaimTag) instead of
                   draining the engine - sibling tags keep running underneath.
                   Only the cases that are barriers *by definition* drain, and
                   they do it in their own arm below. */
                switch(ioreq->io_Command)
                {
                    case TD_GETGEOMETRY:
                        nGetGeometry(ncm, ioreq);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case TD_EJECT:
                    case CMD_START:
                    case CMD_STOP:
                        /* medium-state changes must not overlap data IO:
                           spinning down under an in-flight write is exactly
                           the reordering SCSI SIMPLE tasks permit */
                        nUasDrainTags(ncm);
                        nStartStop(ncm, ioreq);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case CMD_READ:
                        ioreq->io_Actual = 0;
                    case NSCMD_TD_READ64:
                    case TD_READ64:
                        nRead64(ncm, ioreq);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case TD_SEEK:
                        ioreq->io_Actual = 0;
                    case NSCMD_TD_SEEK64:
                    case TD_SEEK64:
                        nSeek64(ncm, ioreq);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case TD_FORMAT:
                    case CMD_WRITE:
                        ioreq->io_Actual = 0;
                    case NSCMD_TD_FORMAT64:
                    case NSCMD_TD_WRITE64:
                    case TD_FORMAT64:
                    case TD_WRITE64:
                        nWrite64(ncm, ioreq);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case HD_SCSICMD:
                        ioreq->io_Error = nScsiDirect(ncm, ioreq->io_Data);
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    case CMD_RESET:
                        /* Reset does a flush too. Nothing else to undo here:
                           a request the task is executing right now owns the
                           CPU, and the drain below retires every in-flight
                           UAS tag first. */
                    case CMD_FLUSH:
                        /* the two commands that ARE barriers by definition */
                        nUasDrainTags(ncm);
                        Forbid(); /* devAbortIO scans the queue */
                        while((ioreq2 = (struct IOStdReq *) RemHead(&ncm->ncm_XFerQueue)))
                        {
                            ioreq2->io_Error = IOERR_ABORTED;
                            ReplyMsg((struct Message *) ioreq2);
                        }
                        Permit();
                        ReplyMsg((struct Message *) ioreq);
                        break;

                    default:
                        ioreq->io_Error = IOERR_NOCMD;
                        ReplyMsg((struct Message *) ioreq);
                        break;
                }
            }
            sigs = Wait(sigmask);
        } while(!(sigs & SIGBREAKF_CTRL_C));
        ncm->ncm_DenyRequests = TRUE;
        /* teardown drains first: in-flight tags complete or fail before the
           pipes go away (unplug makes the driver fail them immediately) */
        nUasDrainTags(ncm);
        /* Device ejected */
        ncm->ncm_UnitReady = FALSE;
        ncm->ncm_ChangeCount++;
        /* the filesystems add and take away their requests under Forbid()
           (devBeginIO), each from its own task */
        Forbid();
        ioreq = (struct IOStdReq *) ncm->ncm_DCInts.lh_Head;
        while(((struct Node *) ioreq)->ln_Succ)
        {
            Cause(ioreq->io_Data);
            ioreq = (struct IOStdReq *) ((struct Node *) ioreq)->ln_Succ;
        }
        Permit();
        if(!ncm->ncm_Removable)
        {
            nStartRemovableTask(ps, ncm->ncm_ClsBase);
        }
        KPRINTF(20, ("Going down the river!\n"));
        nFreeMS(ncm);
    }
}
/* \\\ */

/* /// "Unit task ports" */
/* The unit task has two ports, and each is the wake-up for one loop.
   Other tasks raise a port's signal only through nWakeUnitTask()/nWakeTransport();
   the task itself arms and retires the ports only through
   the two helpers below them. */

/* Raise a port's signal without sending it a message. Callers hold Forbid. */
static void nPokePort(struct MsgPort *mp)
{
    if(mp && mp->mp_SigTask)
    {
        Signal(mp->mp_SigTask, 1UL<<mp->mp_SigBit);
    }
}

/* A flag the main loop services has changed: ncm_ApplyNak, ncm_ForceRTCheck,
   ncm_Running. Only the main loop waits on the unit port, so this wake-up
   cannot be swallowed by a wait further down. */
void nWakeUnitTask(struct NepClassMS *ncm)
{
    Forbid();
    nPokePort(&ncm->ncm_Unit.unit_MsgPort);
    Permit();
}

/* The UAS engine pump has work that no pipe reply announces: ut_AbortReq.
   Everything that waits on the task port pumps the engine afterwards, or is
   psdWaitPipe(), which puts the signal back. */
void nWakeTransport(struct NepClassMS *ncm)
{
    Forbid();
    nPokePort(ncm->ncm_TaskMsgPort);
    Permit();
}

/* Shut the unit port: no more signals, and the bit goes back if this task
   armed it. Safe on a port that never was armed (a bind that failed early). */
static void nDisarmUnitPort(struct NepClassMS *ncm)
{
    struct MsgPort *mp = &ncm->ncm_Unit.unit_MsgPort;

    Forbid();
    mp->mp_Flags = PA_IGNORE;
    if(mp->mp_SigTask)
    {
        mp->mp_SigTask = NULL;
        FreeSignal((LONG) mp->mp_SigBit);
    }
    Permit();
}

/* The pointer is cleared before the port goes, so nWakeTransport() never
   sees a dead one. Every pipe on it must have been freed already. */
static void nDeleteTaskPort(struct NepClassMS *ncm)
{
    Forbid();
    struct MsgPort *mp = ncm->ncm_TaskMsgPort;
    ncm->ncm_TaskMsgPort = NULL;
    Permit();
    DeleteMsgPort(mp); /* NULL for no action */
}
/* \\\ */

/* /// "nAllocMS()" */
struct NepClassMS * nAllocMS(void)
{
    struct Task *thistask;
    struct NepClassMS *ncm;
    IPTR epnum;

    thistask = FindTask(NULL);
    ncm = thistask->tc_UserData;
    ncm->ncm_UasQueueDepth = 0;
    do
    {
        if(!(ncm->ncm_Base = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
        {
            Alert(AG_OpenLib | AO_Unknown);
            break;
        }
        if(ncm->ncm_TPType == MS_PROTO_UAS)
        {
            ncm->ncm_EPInt = NULL;
            if(!nUasCollectEndpoints(ncm))
            {
                psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "UAS endpoints missing or incomplete!");
                break;
            }
        } else {
            ncm->ncm_EPInt = psdFindEndpoint(ncm->ncm_Interface, NULL,
                                             EA_IsIn, TRUE,
                                             EA_TransferType, USEAF_INTERRUPT,
                                             TAG_END);
            ncm->ncm_EPIn = psdFindEndpoint(ncm->ncm_Interface, NULL,
                                            EA_IsIn, TRUE,
                                            EA_TransferType, USEAF_BULK,
                                            TAG_END);
            ncm->ncm_EPOut = psdFindEndpoint(ncm->ncm_Interface, NULL,
                                             EA_IsIn, FALSE,
                                             EA_TransferType, USEAF_BULK,
                                             TAG_END);
            if(!(ncm->ncm_EPIn && ncm->ncm_EPOut))
            {
                psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "IN or OUT endpoint missing!");
                break;
            }
        }
        if((!ncm->ncm_EPInt) && (ncm->ncm_TPType == MS_PROTO_CBI))
        {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "INT endpoint missing!");
            break;
        }
        if(ncm->ncm_EPInt)
        {
            psdGetAttrs(PGA_ENDPOINT, ncm->ncm_EPInt,
                        EA_EndpointNum, &epnum,
                        TAG_END);
            ncm->ncm_EPIntNum = epnum;
        }
        if(ncm->ncm_EPIn)
        {
            psdGetAttrs(PGA_ENDPOINT, ncm->ncm_EPIn,
                        EA_EndpointNum, &epnum,
                        TAG_END);
            ncm->ncm_EPInNum = epnum;
        }
        if(ncm->ncm_EPOut)
        {
            psdGetAttrs(PGA_ENDPOINT, ncm->ncm_EPOut,
                        EA_EndpointNum, &epnum,
                        TAG_END);
            ncm->ncm_EPOutNum = epnum;
        }
        if(ncm->ncm_TPType == MS_PROTO_UAS)
        {
            if(ncm->ncm_EPCmd)
            {
                psdGetAttrs(PGA_ENDPOINT, ncm->ncm_EPCmd,
                            EA_EndpointNum, &epnum,
                            TAG_END);
                ncm->ncm_EPCmdNum = epnum;
            }
            if(ncm->ncm_EPStatus)
            {
                psdGetAttrs(PGA_ENDPOINT, ncm->ncm_EPStatus,
                            EA_EndpointNum, &epnum,
                            TAG_END);
                ncm->ncm_EPStatusNum = epnum;
            }
        }

        ncm->ncm_BulkResetBorks = FALSE;
        ncm->ncm_GeoChangeCount = 0xffffffff;

        ncm->ncm_Unit.unit_MsgPort.mp_SigBit = AllocSignal(-1);
        ncm->ncm_Unit.unit_MsgPort.mp_SigTask = thistask;
        ncm->ncm_Unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        ncm->ncm_Unit.unit_MsgPort.mp_Flags = PA_SIGNAL;
        if((ncm->ncm_TaskMsgPort = CreateMsgPort()))
        {
            if((ncm->ncm_EP0Pipe = psdAllocPipe(ncm->ncm_Device, ncm->ncm_TaskMsgPort, NULL)))
            {
                /* UAS runs on a non-default interface alternate. Activate it
                   here - before the stream setup and INQUIRY below - so its SS
                   bulk endpoints (and their stream capability) are live in the
                   HCD. The enumerator switches the accepted alternate too, but
                   only after this bind task has run, which is too late. */
                if(ncm->ncm_TPType == MS_PROTO_UAS)
                {
                    KPRINTF(10, ("UAS alt switch begin\n"));
                    if(!psdSetAltInterface(ncm->ncm_EP0Pipe, ncm->ncm_Interface))
                    {
                        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                                       "Could not switch to the UAS interface alternate!");
                        psdFreePipe(ncm->ncm_EP0Pipe);
                        break;
                    }
                    KPRINTF(10, ("UAS alt switch done\n"));
                }
                if((ncm->ncm_EPOutPipe = psdAllocPipe(ncm->ncm_Device, ncm->ncm_TaskMsgPort, ncm->ncm_EPOut)))
                {
                    if((ncm->ncm_EPInPipe = psdAllocPipe(ncm->ncm_Device, ncm->ncm_TaskMsgPort, ncm->ncm_EPIn)))
                    {
                        if(ncm->ncm_TPType == MS_PROTO_UAS)
                        {
                            if(!(ncm->ncm_EPCmdPipe = psdAllocPipe(ncm->ncm_Device, ncm->ncm_TaskMsgPort, ncm->ncm_EPCmd)))
                            {
                                psdFreePipe(ncm->ncm_EPInPipe);
                                psdFreePipe(ncm->ncm_EPOutPipe);
                                psdFreePipe(ncm->ncm_EP0Pipe);
                                break;
                            }
                        }
                        nApplyNakTimeout(ncm, nNakTimeoutMs(ncm));
                        psdSetAttrs(PGA_PIPE, ncm->ncm_EPOutPipe,
                                    PPA_NoShortPackets, TRUE,
                                    TAG_END);
                        if(ncm->ncm_EPCmdPipe)
                        {
                            psdSetAttrs(PGA_PIPE, ncm->ncm_EPCmdPipe,
                                        PPA_NoShortPackets, TRUE,
                                        TAG_END);
                        }
                        if(ncm->ncm_TPType == MS_PROTO_UAS)
                        {
                            /* the tag engine is the only UAS transport; a
                               stream-capable HCD is guaranteed by the binding
                               (nPreferUasAlternate), so failure here is a
                               genuine fault - fail the bind loudly */
                            if(!nUasInitTags(ncm))
                            {
                                psdFreePipe(ncm->ncm_EPCmdPipe);
                                ncm->ncm_EPCmdPipe = NULL;
                                psdFreePipe(ncm->ncm_EPInPipe);
                                psdFreePipe(ncm->ncm_EPOutPipe);
                                psdFreePipe(ncm->ncm_EP0Pipe);
                                break;
                            }
                            KPRINTF(10, ("UAS tag engine up, QD %ld\n", (ULONG) ncm->ncm_UasQueueDepth));
                        }
                        if(ncm->ncm_EPInt)
                        {
                            if((ncm->ncm_EPIntPipe = psdAllocPipe(ncm->ncm_Device, ncm->ncm_TaskMsgPort, ncm->ncm_EPInt)))
                            {
                                nSetNakTimeout(ncm, ncm->ncm_EPIntPipe, nNakTimeoutMs(ncm));
                                ncm->ncm_Task = thistask;
                                return(ncm);
                            }
                        } else {
                            ncm->ncm_Task = thistask;
                            return(ncm);
                        }
                        psdFreePipe(ncm->ncm_EPCmdPipe);
                        psdFreePipe(ncm->ncm_EPInPipe);
                    }
                    psdFreePipe(ncm->ncm_EPOutPipe);
                }
                psdFreePipe(ncm->ncm_EP0Pipe);
            }
        }
    } while(FALSE);
    /* every way out of the loop has freed its pipes */
    nDeleteTaskPort(ncm);
    CloseLibrary(ncm->ncm_Base);
    Forbid();
    /* Failed bind: the unit stays linked in nh_Units for a later rebind, so
       leave it in the same shut state nFreeMS() leaves behind - deny requests
       and disarm the port, whose sig task is about to die. */
    ncm->ncm_DenyRequests = TRUE;
    nDisarmUnitPort(ncm);
    ncm->ncm_Task = NULL;
    if(ncm->ncm_ReadySigTask)
    {
        Signal(ncm->ncm_ReadySigTask, 1L<<ncm->ncm_ReadySignal);
    }
    return(NULL);
}
/* \\\ */

/* /// "nFreeMS()" */
void nFreeMS(struct NepClassMS *ncm)
{
    struct IOStdReq *ioreq;
    /* Disable the message port, messages may still be queued */
    Forbid();
    nDisarmUnitPort(ncm);
    // get rid of all messages that still have appeared here
    while((ioreq = (struct IOStdReq *) GetMsg(&ncm->ncm_Unit.unit_MsgPort)))
    {
        ioreq->io_Error = IOERR_ABORTED;
        ReplyMsg((struct Message *) ioreq);
    }
    // and everything the dispatcher queued but never started
    while((ioreq = (struct IOStdReq *) RemHead(&ncm->ncm_XFerQueue)))
    {
        ioreq->io_Error = IOERR_ABORTED;
        ReplyMsg((struct Message *) ioreq);
    }
    Permit();

    nUasDisableTags(ncm);
    psdFreePipe(ncm->ncm_EPIntPipe);
    psdFreePipe(ncm->ncm_EPCmdPipe);
    psdFreePipe(ncm->ncm_EPInPipe);
    psdFreePipe(ncm->ncm_EPOutPipe);
    psdFreePipe(ncm->ncm_EP0Pipe);
    /* the unit struct is reused across replugs: a later rebind on another
       transport (UAS <-> BOT) must not see this binding's dead pipes */
    ncm->ncm_EPIntPipe = NULL;
    ncm->ncm_EPCmdPipe = NULL;
    ncm->ncm_EPInPipe = NULL;
    ncm->ncm_EPOutPipe = NULL;
    ncm->ncm_EP0Pipe = NULL;
    nDeleteTaskPort(ncm);

    psdFreeVec(ncm->ncm_OneBlock);
    ncm->ncm_OneBlock = NULL;
    ncm->ncm_OneBlockSize = 0;

    CloseLibrary(ncm->ncm_Base);
    Forbid();
    ncm->ncm_Task = NULL;
    if(ncm->ncm_ReadySigTask)
    {
        Signal(ncm->ncm_ReadySigTask, 1L<<ncm->ncm_ReadySignal);
    }
}
/* \\\ */

/* /// "nGetModePage()" */
UBYTE * nGetModePage(struct NepClassMS *ncm, UBYTE page)
{
    UBYTE cmd6[6];
    struct SCSICmd scsicmd;
    UBYTE sensedata[18];
    LONG ioerr;
    UBYTE *res;
    ULONG pf;

    KPRINTF(10, ("page %ld\n", page));

    memset(ncm->ncm_ModePageBuf, 0x00, 255);
    scsicmd.scsi_Data = (UWORD *) ncm->ncm_ModePageBuf;
    scsicmd.scsi_Length = 255;
    scsicmd.scsi_Command = cmd6;
    scsicmd.scsi_CmdLength = 6;
    scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE;
    scsicmd.scsi_SenseData = sensedata;
    scsicmd.scsi_SenseLength = 18;
    cmd6[0] = SCSI_MODE_SENSE_6;
    cmd6[1] = 0x00; /* no block descriptors */
    cmd6[2] = page;
    cmd6[3] = 0;
    cmd6[4] = 255;
    cmd6[5] = 0;
    if((ioerr = nScsiDirect(ncm, &scsicmd)))
    {
        KPRINTF(10, ("ioerr %ld\n", ioerr));
        pf = ncm->ncm_CDC->cdc_PatchFlags;
        if(pf & PFF_DEBUG)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "SCSI_MODE_SENSE(0x%02lx) failed: %ld",
                           page, ioerr);
        }
        if(ioerr == HFERR_Phase)
        {
            /* a ladder: 10-byte CDBs first, the command whitelist after */
            if(!(pf & PFF_MODE_XLATE))
            {
                nApplyFallback(ncm, PFF_MODE_XLATE, "Translate CMD6->CMD10", TRUE, RETURN_WARN);
            } else {
                nApplyFallback(ncm, PFF_SIMPLE_SCSI, "Simple SCSI", TRUE, RETURN_WARN);
            }
        }
        return(NULL);
    }
    pf = ncm->ncm_CDC->cdc_PatchFlags;
    res = ncm->ncm_ModePageBuf;
    if((scsicmd.scsi_Actual < 6) || (scsicmd.scsi_Actual < res[3]+6))
    {
        if(pf & PFF_DEBUG)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "SCSI_MODE_SENSE(0x%02lx) failed: only %ld returned",
                           page, scsicmd.scsi_Actual);
        }
        return(NULL);
    }
    if(pf & PFF_DEBUG)
    {
        UBYTE hexbuf[12*3+2];
        nHexString(res, 12, hexbuf);
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "ModePage (%ld) header: %s...", scsicmd.scsi_Actual, hexbuf);
    }
    if(res[3])
    {
        res += res[3];
        scsicmd.scsi_Actual -= res[3];
    }
    res += 4; // skip mode header */
    scsicmd.scsi_Actual -= 4;
    if((*res & 0x3f) != (page & 0x3f))
    {
        if(pf & PFF_DEBUG)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "SCSI_MODE_SENSE(0x%02lx) failed: wrong page 0x%02lx returned",
                           page, *res);
        }
        return(NULL);
    }
    if(scsicmd.scsi_Actual < res[1])
    {
        if(pf & PFF_DEBUG)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "SCSI_MODE_SENSE(0x%02lx) failed: page incomplete",
                           page);
        }
        return(NULL);
    }

#if 0
    if(*res & 0x40) /* subpage mode? */
    {
        res += 2;
    }
#endif
    return(res);
}
/* \\\ */

/* /// "nGetBlockSize()" */
LONG nGetBlockSize(struct NepClassMS *ncm)
{
    UBYTE cmd10[10];
    struct SCSICmd scsicmd;
    UBYTE sensedata[18];
    ULONG capacity[2];
    LONG ioerr;

    scsicmd.scsi_Data = (UWORD *) capacity;
    scsicmd.scsi_Length = 8;
    scsicmd.scsi_Command = cmd10;
    scsicmd.scsi_CmdLength = 10;
    /* Autoretry: a device coming out of reset answers the first command that
       reaches it with a unit attention, and the capacity can be that command.
       That is an "ask me again", not a failure. */
    scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
    scsicmd.scsi_SenseData = sensedata;
    scsicmd.scsi_SenseLength = 18;
    cmd10[0] = SCSI_DA_READ_CAPACITY;
    cmd10[1] = 0;
    cmd10[2] = 0;
    cmd10[3] = 0;
    cmd10[4] = 0;
    cmd10[5] = 0;
    cmd10[6] = 0;
    cmd10[7] = 0;
    cmd10[8] = 0;
    cmd10[9] = 0;
    if((ioerr = nScsiDirect(ncm, &scsicmd)))
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "SCSI_READ_CAPACITY failed: %ld",
                       ioerr);
        if(!ncm->ncm_BlockSize)
        {
            ncm->ncm_Geometry.dg_SectorSize = ncm->ncm_BlockSize = 512;
            ncm->ncm_BlockShift = 9;
        }
        /* Do not keep a stale sector count: nFakeGeometry() would build a
           volume out of it. Zero sectors means no usable medium. */
        ncm->ncm_Geometry.dg_TotalSectors = 0;
    } else {
        ncm->ncm_Geometry.dg_SectorSize = ncm->ncm_BlockSize = AROS_BE2LONG(capacity[1]);
        ncm->ncm_BlockShift = 0;
        while((1<<ncm->ncm_BlockShift) < ncm->ncm_BlockSize)
        {
            ncm->ncm_BlockShift++;
        }
        ncm->ncm_Geometry.dg_TotalSectors = AROS_BE2LONG(capacity[0])+1;
    }
    return(ioerr);
}
/* \\\ */

/* /// "nFakeGeometry()" */
/* Invent a cylinder/head/sector geometry for a device that has none worth
   using. The product has to be exactly dg_TotalSectors: partitioning works in
   whole cylinders, so a smaller product loses the tail of the disk and a
   larger one claims blocks that are not there. Hence the prime factorisation -
   the factors are dealt out to sectors per track and heads until those look
   conventional, and whatever is left becomes the cylinder count.

   A count that will not factor is taken as the off-by-one READ CAPACITY bug
   (total blocks reported instead of the last block): real media never has a
   prime number of blocks. Fix Capacity is flipped and the count retried. */
void nFakeGeometry(struct NepClassMS *ncm, struct DriveGeometry *tddg)
{
    UWORD cnt;
    ULONG primes[32];
    UWORD count[32];
    UWORD curpos;
    UWORD curprime;
    ULONG curprimesq;
    ULONG remblks = tddg->dg_TotalSectors;
    const UWORD *primetblptr = PrimeTable;
    UWORD totfactors;
    UWORD factor;
    ULONG remfact;
    UWORD tries;
    BOOL taken;

    if(!ncm->ncm_Geometry.dg_TotalSectors)
    {
        return;
    }
    KPRINTF(10, ("Faking geometry for %ld sectors\n", tddg->dg_TotalSectors));
    //psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Prime factorisation for %ld", remblks);
    tddg->dg_DeviceType = ncm->ncm_DeviceType;
    tddg->dg_Flags = ncm->ncm_Removable ? DGF_REMOVABLE : 0;
    tddg->dg_BufMemType = MEMF_PUBLIC;
    tddg->dg_Reserved = 0;
    do
    {
        KPRINTF(10, ("Loop\n"));
        // first prime is counted manually
        *primes = 2;
        *count = 0;
        curpos = 0;
        while(!(remblks & 1))
        {
            (*count)++;
            remblks >>= 1;
        }
        if(*count)
        {
            curpos++;
        }

        primes[curpos] = 3;
        count[curpos] = 0;
        primetblptr = PrimeTable;
        curprime = 3;
        curprimesq = 9;
        // abort if no primes can be found anymore
        do
        {
            KPRINTF(10, ("remblks = %ld, curprime=%ld\n", remblks, curprime));
            if(remblks % curprime)
            {
                /* NOTE: this test is inverted. "No divisor up to the square
                   root, so the rest is prime" is remblks < curprimesq; as
                   written the search gives up the first time a prime does not
                   divide a still-large remainder. In effect only 2s and 3s
                   are factored out, the rest is lumped into the cylinders, and
                   PrimeTable goes unused. It also makes the "prime number"
                   verdict below fire for any odd count that is not a multiple
                   of 3, so that heuristic is really "odd means off by one".
                   Left alone on purpose: correcting it changes the geometry
                   reported for disks whose count has factors such as 5 or 7,
                   and with it the cylinder boundaries of anything already
                   partitioned on the old one. */
                if(remblks >= curprimesq)
                {
                    // it's a prime!
                    break;
                }
                if(count[curpos]) // prime at least once
                {
                    // next prime
                    count[++curpos] = 0;
                    if(curpos == 31) // end of buffer reached
                    {
                        break;
                    }
                }
                // try next "prime"
                if(*primetblptr)
                {
                    // use lookup table for the first primes to 2000
                    curprime = *primetblptr++;
                } else {
                    // do it the hard way :-(
                    curprime += 2;
                }
                primes[curpos] = curprime;
                curprimesq = curprime * curprime;
            } else {
                // factor found
                count[curpos]++;
                remblks /= curprime;
            }
        } while((remblks > 1) && (remblks >= curprime));
        if(count[curpos])
        {
            curpos++;
        }
        if((remblks > 1) && curpos)
        {
            count[curpos] = 1;
            primes[curpos++] = remblks;
        }
        KPRINTF(10, ("Priming done, %ld different primes\n", curpos));
        if(curpos)
        {
            break;
        }
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "FakeGeometry: Total number of blocks is a prime number!");
        if(ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_CAPACITY)
        {
            if(!nApplyFallback(ncm, PFF_FIX_CAPACITY, "Fix Capacity", FALSE, RETURN_ERROR))
            {
                return;
            }
            remblks = ++tddg->dg_TotalSectors;
        } else {
            /* assume the off-by-one either way; No Fallback only stops it
               from being remembered */
            nApplyFallback(ncm, PFF_FIX_CAPACITY, "Fix Capacity", TRUE, RETURN_WARN);
            remblks = --tddg->dg_TotalSectors;
        }
    } while(TRUE);

    totfactors = 0;
    for(cnt = 0; cnt < curpos; cnt++)
    {
        totfactors += count[cnt];
    }

    // Rules:
    // - We want at least blocks per track * heads >= 64
    // - We want heads * heads < blocks per track
    // - We want blocks per track < cylinders
    // - We want heads < 8
    // - We want blocks per track < 256
    // - The rest goes into cylinders
    tddg->dg_TrackSectors = 1;
    tddg->dg_Heads = 1;
    remfact = tddg->dg_TotalSectors;
    tries = 0;
    while(totfactors)
    {
        KPRINTF(10, ("Totfactors %ld\n", totfactors));
        cnt = 0;
        while(!count[cnt])
        {
            cnt++;
            if(cnt >= curpos)
            {
                cnt = 0;
            }
        }
        taken = TRUE;
        factor = primes[cnt];
        if((tddg->dg_TrackSectors * factor < 256) &&
           (tddg->dg_TrackSectors * factor < remfact))
        {
            tddg->dg_TrackSectors *= factor;
            remfact /= factor;
        }
        else if((tddg->dg_Heads * factor <= 16) &&
                (tddg->dg_Heads * tddg->dg_Heads * factor * factor < tddg->dg_TrackSectors))
        {
            tddg->dg_Heads *= factor;
            remfact /= factor;
        }
        else if((tddg->dg_Heads * tddg->dg_TrackSectors >= 64) ||
                (tries > curpos))
        {
            // do nothing
        } else {
            taken = FALSE;
        }
        if(taken)
        {
            totfactors--;
            count[cnt]--;
            tries = 0;
        } else {
            tries++;
        }
        KPRINTF(10, ("Factor=%ld: Cylinders = %ld, Heads=%ld, TrackSectors=%ld, Blocks=%ld\n",
                     factor, remfact, tddg->dg_Heads, tddg->dg_TrackSectors,
                     tddg->dg_TotalSectors));

    }
    KPRINTF(10, ("Final: Cylinders = %ld, Heads=%ld, TrackSectors=%ld, Blocks=%ld\n",
                 remfact, tddg->dg_Heads, tddg->dg_TrackSectors,
                 tddg->dg_TotalSectors));

    tddg->dg_Cylinders = remfact;
    tddg->dg_CylSectors = tddg->dg_TrackSectors * tddg->dg_Heads;
}
/* \\\ */

struct	CapacityData
{
	ULONG	SectorCount;
	ULONG	SectorSize;
};

#define SERVICEACTION_CAPACITY16 0x10

/* /// "nGetGeometry()" */
LONG nGetGeometry(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    struct DriveGeometry *tddg;
    ULONG length;
    UBYTE cmd10[10];
    struct SCSICmd scsicmd;
    struct CapacityData capacitydata;
    UBYTE sensedata[18];
    LONG ioerr;
    ULONG tmpval;
    UBYTE *mpdata;
    BOOL gotblks = FALSE;
    BOOL gotcyl = FALSE;
    BOOL gotheads = FALSE;
    BOOL gotsect = FALSE;
    BOOL gotcylsect = FALSE;
    KPRINTF(10, ("\n"));

    ioreq->io_Error = 0;
    ioreq->io_Actual = 0;
    tddg = (struct DriveGeometry *) ioreq->io_Data;
    if(!tddg)
    {
        ioreq->io_Error = TDERR_NotSpecified;
        return(ioreq->io_Error);
    }
    length = min(ioreq->io_Length,sizeof(struct DriveGeometry));
    if((ncm->ncm_GeoChangeCount == ncm->ncm_ChangeCount) && ncm->ncm_Geometry.dg_TotalSectors)
    {
        // cached
        memcpy(tddg,&ncm->ncm_Geometry, (size_t) length);
        ioreq->io_Actual = length;
        return(0);
    }

    ncm->ncm_Geometry.dg_DeviceType = ncm->ncm_DeviceType;
    ncm->ncm_Geometry.dg_Flags = ncm->ncm_Removable ? DGF_REMOVABLE : 0;

    /*
     * Capacity10, 32bit Sectorcount
     */
    scsicmd.scsi_Data = (UWORD *) &capacitydata;
    scsicmd.scsi_Length = sizeof(capacitydata);
    scsicmd.scsi_Command = cmd10;
    scsicmd.scsi_CmdLength = 10;
    scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80; /* autoretry, as in nGetBlockSize() */
    scsicmd.scsi_SenseData = sensedata;
    scsicmd.scsi_SenseLength = 18;
    cmd10[0] = SCSI_DA_READ_CAPACITY;
    cmd10[1] = 0;
    cmd10[2] = 0;
    cmd10[3] = 0;
    cmd10[4] = 0;
    cmd10[5] = 0;
    cmd10[6] = 0;
    cmd10[7] = 0;
    cmd10[8] = 0;
    cmd10[9] = 0;

    if((ioerr = nScsiDirect(ncm, &scsicmd)))
    {
        KPRINTF(10, ("ioerr %ld\n",ioerr));
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "SCSI_READ_CAPACITY failed: %ld",
                       ioerr);
        /*
         * memset(tddg,0,Length);
         * The error should be enough
         */
        return(ioreq->io_Error = ioerr);
    } else {
        /*
         * 32Bit Totalsectors
         * R.S.
         */
        ncm->ncm_BlockSize = ncm->ncm_Geometry.dg_SectorSize = AROS_BE2LONG(capacitydata.SectorSize);
        ncm->ncm_Geometry.dg_TotalSectors = AROS_BE2LONG(capacitydata.SectorCount) + 1;
        if(capacitydata.SectorCount == 0xffffffff)
        {
            ncm->ncm_Geometry.dg_TotalSectors--; // set to maximum
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Capacity exceeds the maximum supported for 32 bit sector counts (usually >2TB)!");
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "I/O operations will still work, but geometry and partitioning will be unreliable!");
        }
        KPRINTF(10, ("blocksize %ld totalsectors %ld\n", ncm->ncm_BlockSize, ncm->ncm_Geometry.dg_TotalSectors));
        gotblks = TRUE;
        if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
        {
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Capacity: %ld blocks of %ld bytes", ncm->ncm_Geometry.dg_TotalSectors, ncm->ncm_Geometry.dg_SectorSize);
        }
        ncm->ncm_BlockShift = 0;
        while((1<<ncm->ncm_BlockShift) < ncm->ncm_BlockSize)
        {
            ncm->ncm_BlockShift++;
        }
    }
    KPRINTF(10, ("PatchFlags 0x%lx DeviceType %ld\n",ncm->ncm_CDC->cdc_PatchFlags,ncm->ncm_DeviceType));

    if(!((ncm->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI) ||
         (ncm->ncm_DeviceType == PDT_WORM) ||
         (ncm->ncm_DeviceType == PDT_CDROM)))
    {
        KPRINTF(10, ("reading mode page 3 (not SIMPLE_SCSI, not WORM/CDROM)\n"));
        // cd roms don't have valid or sensible capacity mode pages
        if((mpdata = nGetModePage(ncm, 0x03)))
        {
            if((tmpval = (mpdata[10]<<8)+mpdata[11]))
            {
                ncm->ncm_Geometry.dg_TrackSectors = tmpval;
                gotsect = TRUE;
                if(!ncm->ncm_Geometry.dg_Cylinders)
                {
                    ncm->ncm_Geometry.dg_Cylinders = ncm->ncm_Geometry.dg_TotalSectors;
                }
            }
            if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
            {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Capacity: TrackSectors=%ld",
                               ncm->ncm_Geometry.dg_TrackSectors);
            }
        }
    }
    /*
     * Cylinder and co are only defined for old 32Bit Totalsectors.
     * >2TB they have no meaning anyway, so they are calculated based
     * on 32Bit INT_MAX.
     */
    // recheck, could have simple scsi enabled in the meanwhile
    if(!((ncm->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI) ||
         (ncm->ncm_DeviceType == PDT_WORM) ||
         (ncm->ncm_DeviceType == PDT_CDROM)))
    {
        KPRINTF(10, ("recheck1 SIMPLE_SCSI or PDT_WORM/CDROM\n"));
        if((mpdata = nGetModePage(ncm, 0x04)))
        {
            if((tmpval = (mpdata[2]<<16)+(mpdata[3]<<8)+mpdata[4]))
            {
                ncm->ncm_Geometry.dg_Cylinders = tmpval;
                ncm->ncm_Geometry.dg_Heads = mpdata[5];
                gotcyl = gotheads = TRUE;
            }
            if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
            {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Capacity: Cylinders=%ld, Heads=%ld",
                               ncm->ncm_Geometry.dg_Cylinders, ncm->ncm_Geometry.dg_Heads);
            }
        }
    }
    // recheck, could have simple scsi enabled in the meanwhile
    if(!((ncm->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI) ||
         (ncm->ncm_DeviceType == PDT_WORM) ||
         (ncm->ncm_DeviceType == PDT_CDROM)))
    {
        KPRINTF(10, ("recheck2 SIMPLE_SCSI or PDT_WORM/CDROM\n"));
        if((mpdata = nGetModePage(ncm, 0x05)))
        {
            if((tmpval = (mpdata[8]<<8)+mpdata[9]))
            {
                ncm->ncm_Geometry.dg_Cylinders = tmpval;
                ncm->ncm_Geometry.dg_Heads = mpdata[4];
                ncm->ncm_Geometry.dg_TrackSectors = mpdata[5];

                gotcyl = gotheads = gotsect = TRUE;

                if(!gotblks)
                {
                    ncm->ncm_BlockSize = ncm->ncm_Geometry.dg_SectorSize = (mpdata[6]<<8)+mpdata[7];
                    ncm->ncm_BlockShift = 0;
                    while((1<<ncm->ncm_BlockShift) < ncm->ncm_BlockSize)
                    {
                        ncm->ncm_BlockShift++;
                    }
                    if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                    {
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Capacity: %ld blocks of %ld bytes", ncm->ncm_Geometry.dg_TotalSectors, ncm->ncm_BlockSize);
                    }
                }
                else if(ncm->ncm_BlockSize != (mpdata[6]<<8)+mpdata[7])
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Inconsistent block size information %ld != %ld!",
                                   ncm->ncm_BlockSize, (mpdata[6]<<8)+mpdata[7]);
                }
                if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                {
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "Capacity: Cylinders=%ld, Heads=%ld, TrackSectors=%ld",
                                   ncm->ncm_Geometry.dg_Cylinders, ncm->ncm_Geometry.dg_Heads, ncm->ncm_Geometry.dg_TrackSectors);
                }
            }
        }
    }
    // missing total blocks?
    if((!gotblks) && gotcyl && gotheads && gotsect)
    {
        ncm->ncm_Geometry.dg_TotalSectors = ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_Heads * ncm->ncm_Geometry.dg_TrackSectors;
        gotblks = TRUE;
    }
    // missing cylinders?
    if(gotblks && (!gotcyl) && gotheads && gotsect)
    {
        ncm->ncm_Geometry.dg_Cylinders = ncm->ncm_Geometry.dg_TotalSectors / (ncm->ncm_Geometry.dg_Heads * ncm->ncm_Geometry.dg_TrackSectors);
        gotcyl = TRUE;
    }
    // missing heads?
    if(gotblks && gotcyl && (!gotheads) && gotsect)
    {
        ncm->ncm_Geometry.dg_Heads = ncm->ncm_Geometry.dg_TotalSectors / (ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_TrackSectors);
        gotheads = TRUE;
    }
    // missing tracks per sector
    if(gotblks && gotcyl && gotheads && (!gotsect))
    {
        ncm->ncm_Geometry.dg_TrackSectors = ncm->ncm_Geometry.dg_TotalSectors / (ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_Heads);
        gotsect = TRUE;
    }

    if(gotblks && gotcyl && gotheads && gotsect &&
       (ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_TrackSectors * ncm->ncm_Geometry.dg_Heads == ncm->ncm_Geometry.dg_TotalSectors - 1) &&
       (!(ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_CAPACITY)))
    {
        if(nApplyFallback(ncm, PFF_FIX_CAPACITY, "Fix Capacity", TRUE, RETURN_WARN))
        {
            ncm->ncm_Geometry.dg_TotalSectors--;
        }
    }
    else if(gotblks && gotcyl && gotheads && gotsect &&
            (ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_TrackSectors * ncm->ncm_Geometry.dg_Heads == ncm->ncm_Geometry.dg_TotalSectors + 1) &&
            ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_CAPACITY)
    {
        if(nApplyFallback(ncm, PFF_FIX_CAPACITY, "Fix Capacity", FALSE, RETURN_ERROR))
        {
            ncm->ncm_Geometry.dg_TotalSectors++;
        }
    }

    // some devices report these bogus values regardless of actual device capacity, though the total number of blocks is correct.
    if(((ncm->ncm_Geometry.dg_Cylinders == 500) && (ncm->ncm_Geometry.dg_TrackSectors == 32) && (ncm->ncm_Geometry.dg_Heads == 8)) ||
       ((ncm->ncm_Geometry.dg_Cylinders == 16383) && (ncm->ncm_Geometry.dg_TrackSectors == 63) && (ncm->ncm_Geometry.dg_Heads == 16)))
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Firmware returns known bogus geometry, will fall back to faked geometry!");
        gotheads = gotcyl = gotsect = FALSE;
        nApplyFallback(ncm, PFF_SIMPLE_SCSI, "Simple SCSI", TRUE, RETURN_WARN);
    }

    // missing more than one?
    if(gotblks && (!(gotheads && gotcyl && gotsect)))
    {
        nFakeGeometry(ncm, &ncm->ncm_Geometry);
    }
    if(!gotcylsect)
    {
        ncm->ncm_Geometry.dg_CylSectors = ncm->ncm_Geometry.dg_TrackSectors * ncm->ncm_Geometry.dg_Heads;
    }

    if(ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_CylSectors != ncm->ncm_Geometry.dg_TotalSectors)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Estimated Geometry yields %ld less total blocks %ld: Cylinders=%ld, CylSectors=%ld, Heads=%ld, TrackSectors=%ld, Blocks=%ld",
                       ncm->ncm_Geometry.dg_TotalSectors - ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_CylSectors,
                       ncm->ncm_Geometry.dg_Cylinders * ncm->ncm_Geometry.dg_CylSectors,
                       ncm->ncm_Geometry.dg_Cylinders, ncm->ncm_Geometry.dg_CylSectors, ncm->ncm_Geometry.dg_Heads, ncm->ncm_Geometry.dg_TrackSectors,
                       ncm->ncm_Geometry.dg_TotalSectors);
    }
    else if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       "Capacity: Cylinders=%ld, CylSectors=%ld, Heads=%ld, TrackSectors=%ld, Blocks=%ld, SectorSize=%ld",
                       ncm->ncm_Geometry.dg_Cylinders, ncm->ncm_Geometry.dg_CylSectors, ncm->ncm_Geometry.dg_Heads, ncm->ncm_Geometry.dg_TrackSectors,
                       ncm->ncm_Geometry.dg_TotalSectors, ncm->ncm_Geometry.dg_SectorSize);
    }

    ncm->ncm_Geometry.dg_BufMemType = MEMF_PUBLIC;
    ncm->ncm_Geometry.dg_Reserved = 0;
    memcpy(tddg, &ncm->ncm_Geometry, (size_t) length);
    ioreq->io_Actual = length;
    ncm->ncm_GeoChangeCount = ncm->ncm_ChangeCount;
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nStartStop()" */
LONG nStartStop(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    UBYTE cmd6[6];
    struct SCSICmd scsicmd;
    UBYTE sensedata[18];
    LONG ioerr;

    if((ncm->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI)
        && ioreq->io_Command != TD_EJECT)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Simple SCSI: Ignoring START_STOP_UNIT command");
        return(0);
    }
    scsicmd.scsi_Data = NULL;
    scsicmd.scsi_Length = 0;
    scsicmd.scsi_Command = cmd6;
    scsicmd.scsi_CmdLength = 6;
    scsicmd.scsi_Flags = SCSIF_AUTOSENSE|0x80;
    scsicmd.scsi_SenseData = sensedata;
    scsicmd.scsi_SenseLength = 18;
    cmd6[0] = SCSI_DA_START_STOP_UNIT;
    cmd6[1] = 0;
    cmd6[2] = 0;
    cmd6[3] = 0;
    switch(ioreq->io_Command)
    {
        case CMD_START:
            cmd6[4] = 0x01;
            break;

        case CMD_STOP:
            cmd6[4] = 0x00;
            break;

        case TD_EJECT:
            cmd6[4] = ioreq->io_Length ? 0x02 : 0x03;
            break;
    }
    cmd6[5] = 0;
    if((ioerr = nScsiDirect(ncm, &scsicmd)))
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "START_STOP_UNIT failed: %ld",
                       ioerr);
        ioreq->io_Error = TDERR_NotSpecified;
    }
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nSetNakTimeout()" */
/* Set a pipe's NAK timeout; zero switches it off. No-op for a missing pipe,
   so callers can pass any of the unit's pipe pointers unguarded. The HCD
   applies it as a deadline on each transfer, counted from the submit - which
   makes this, in effect, the command timeout. */
void nSetNakTimeout(struct NepClassMS *ncm, struct PsdPipe *pp, ULONG timeout_ms)
{
    if(pp)
    {
        psdSetAttrs(PGA_PIPE, pp,
                    PPA_NakTimeout, timeout_ms != 0,
                    PPA_NakTimeoutTime, timeout_ms,
                    TAG_END);
    }
}
/* \\\ */

/* /// "nNakTimeoutMs()" */
/* The configured NAK timeout in ms, with the optical-drive floor: CD/DVD
   drives NAK for seconds while seeking or spinning up, so a too-short value is
   raised to MIN_CD_NAKTIMEOUT. Never lowers a longer one, and a configured
   zero (NAK timeouts switched off) stays zero. */
ULONG nNakTimeoutMs(struct NepClassMS *ncm)
{
    ULONG nak = ncm->ncm_CDC->cdc_NakTimeout;

    if(nak && (nak < MIN_CD_NAKTIMEOUT) &&
       ((ncm->ncm_DeviceType == PDT_WORM) || (ncm->ncm_DeviceType == PDT_CDROM)))
    {
        nak = MIN_CD_NAKTIMEOUT;
    }
    return(nak*100);
}
/* \\\ */

/* /// "nApplyNakTimeout()" */
/* Set the timeout on every pipe the unit currently owns; zero switches it
   off on all of them. EP0 gets 100ms more than the data pipes on purpose: the
   control pipe carries the recovery traffic (CLEAR FEATURE, bulk-only reset)
   that has to survive a data pipe timing out, so it must outlive them. That
   offset is why EP0 cannot just be handed to nSetNakTimeout by the caller -
   with NAK timeouts switched off the sum would be a live 100ms window instead
   of "off". */
void nApplyNakTimeout(struct NepClassMS *ncm, ULONG timeout_ms)
{
    nSetNakTimeout(ncm, ncm->ncm_EP0Pipe, timeout_ms ? timeout_ms+100 : 0);
    nSetNakTimeout(ncm, ncm->ncm_EPInPipe, timeout_ms);
    nSetNakTimeout(ncm, ncm->ncm_EPOutPipe, timeout_ms);
    nSetNakTimeout(ncm, ncm->ncm_EPCmdPipe, timeout_ms);
    nSetNakTimeout(ncm, ncm->ncm_EPIntPipe, timeout_ms);
    /* the UAS engine's stream pipes, if it is up: task management, then
       the three of every tag */
    nSetNakTimeout(ncm, ncm->ncm_UasTMStatusPipe, timeout_ms);
    MS_FOREACH_TAG(ncm, ut)
    {
        nSetNakTimeout(ncm, ut->ut_StatusPipe, timeout_ms);
        nSetNakTimeout(ncm, ut->ut_DataInPipe, timeout_ms);
        nSetNakTimeout(ncm, ut->ut_DataOutPipe, timeout_ms);
    }
}
/* \\\ */

/* /// "nBuildRWCdb()" */
/* Shared READ/WRITE CDB builder: RW10 while the LBA fits 32 bits, RW16 above
   (>2 TB). cdb must hold 16 bytes; returns the CDB length actually used.
   datalen is in bytes and must be block-aligned. */
UWORD nBuildRWCdb(UBYTE *cdb, BOOL iswrite, ULONG startblockhigh, ULONG startblock,
                  ULONG datalen, UWORD blockshift)
{
    memset(cdb, 0, 16);
    if(startblockhigh)
    {
        ULONG *cdbsbh = (ULONG *) &cdb[2];
        ULONG *cdbsbl = (ULONG *) &cdb[6];

        cdb[0] = iswrite ? SCSI_DA_WRITE_16 : SCSI_DA_READ_16;
        *cdbsbh = AROS_LONG2BE(startblockhigh);
        *cdbsbl = AROS_LONG2BE(startblock);
        cdb[10] = datalen>>(blockshift+24);
        cdb[11] = datalen>>(blockshift+16);
        cdb[12] = datalen>>(blockshift+8);
        cdb[13] = datalen>>blockshift;
        return(16);
    }
    ULONG *cdbsb = (ULONG *) &cdb[2];

    cdb[0] = iswrite ? SCSI_DA_WRITE_10 : SCSI_DA_READ_10;
    *cdbsb = AROS_LONG2BE(startblock);
    cdb[7] = datalen>>(blockshift+8);
    cdb[8] = datalen>>blockshift;
    return(10);
}
/* \\\ */

/* /// "nBuildSenseCdb()" */
/* Shared REQUEST SENSE CDB: writes exactly cdb[0..5] and touches nothing else,
   so a caller with a longer buffer keeps whatever it put in the trailing
   bytes. Both of those matter: CBI sends a zero-padded 12-byte CDB to ATAPI
   devices, and the BOT path deliberately leaves the previous command's bytes
   in CBWCB[6..15]. Returns the CDB length. */
UWORD nBuildSenseCdb(UBYTE *cdb, ULONG senselen)
{
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[1] = 0;
    cdb[2] = 0;
    cdb[3] = 0;
    cdb[4] = (UBYTE) senselen;
    cdb[5] = 0;
    return(6);
}
/* \\\ */

/* /// "nRead64Emul()" */
LONG nRead64Emul(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    UBYTE cmd10[10];
    ULONG *cmd10sb = (ULONG *)&cmd10[2];
    UBYTE sensedata[18];
    ULONG dataoffset = 0;
    ULONG dataremain = ioreq->io_Length;
    ULONG datalen;
    ULONG startblock;
    ULONG maxtrans = 1UL<<(ncm->ncm_CDC->cdc_MaxTransfer+16);
    ULONG insideblockoffset;
    struct SCSICmd scsicmd;

    if(dataremain & 511)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Attempt to read partial block (%ld %% %ld != 0)!",
                       dataremain, ncm->ncm_BlockSize);
        ioreq->io_Actual = 0;
        return(ioreq->io_Error = IOERR_BADLENGTH);
    }
    if(ioreq->io_Offset & 511)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Attempt to read unaligned block (%ld %% %ld != 0)!",
                       ioreq->io_Offset, ncm->ncm_BlockSize);
        ioreq->io_Actual = 0;
        return(ioreq->io_Error = IOERR_BADADDRESS);
    }

    if(!ncm->ncm_OneBlock || (ncm->ncm_OneBlockSize < ncm->ncm_BlockSize))
    {
        psdFreeVec(ncm->ncm_OneBlock);
        if(!(ncm->ncm_OneBlock = psdAllocVec(ncm->ncm_BlockSize)))
        {
            return(IOERR_BADADDRESS);
        }
        ncm->ncm_OneBlockSize = ncm->ncm_BlockSize;
    }

    startblock = (ioreq->io_Offset>>ncm->ncm_BlockShift)|(ioreq->io_Actual<<(32-ncm->ncm_BlockShift));
    insideblockoffset = (ioreq->io_Offset & ((1<<ncm->ncm_BlockShift)-1));
    while(dataremain)
    {
        KPRINTF(10, ("Reading from block %ld, %ld bytes left...\n", startblock, dataremain));
        datalen = dataremain;
        if(datalen > maxtrans)
        {
            datalen = maxtrans;
        }
        if(insideblockoffset || (datalen < ncm->ncm_BlockSize))
        {
            if(datalen > ncm->ncm_BlockSize-insideblockoffset)
            {
                datalen = ncm->ncm_BlockSize-insideblockoffset;
            }
            scsicmd.scsi_Data = (UWORD *) ncm->ncm_OneBlock;
            scsicmd.scsi_Length = ncm->ncm_BlockSize;
            scsicmd.scsi_Command = cmd10;
            scsicmd.scsi_CmdLength = 10;
            scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
            scsicmd.scsi_SenseData = sensedata;
            scsicmd.scsi_SenseLength = 18;
            cmd10[0] = SCSI_DA_READ_10;
            cmd10[1] = 0;
            *cmd10sb = AROS_LONG2BE(startblock);
            cmd10[6] = 0;
            cmd10[7] = 0;
            cmd10[8] = 1;
            cmd10[9] = 0;
            if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
            {
                KPRINTF(10, ("Read error!\n"));
                break;
            }
            CopyMemQuick(&ncm->ncm_OneBlock[insideblockoffset], &(((UBYTE *) ioreq->io_Data)[dataoffset]), datalen);
            insideblockoffset = 0;
            startblock++;
        } else {
            scsicmd.scsi_Data = (UWORD *) &(((UBYTE *) ioreq->io_Data)[dataoffset]);
            scsicmd.scsi_Length = datalen;
            scsicmd.scsi_Command = cmd10;
            scsicmd.scsi_CmdLength = 10;
            scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
            scsicmd.scsi_SenseData = sensedata;
            scsicmd.scsi_SenseLength = 18;
            cmd10[0] = SCSI_DA_READ_10;
            cmd10[1] = 0;
            *cmd10sb = AROS_LONG2BE(startblock);
            cmd10[6] = 0;
            cmd10[7] = datalen>>(ncm->ncm_BlockShift+8);
            cmd10[8] = datalen>>ncm->ncm_BlockShift;
            cmd10[9] = 0;
            if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
            {
                KPRINTF(10, ("Read error!\n"));
                break;
            }
            startblock += (insideblockoffset+datalen)>>ncm->ncm_BlockShift;
        }
        dataoffset += datalen;
        dataremain -= datalen;
    }
    ioreq->io_Actual = dataoffset;
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nWrite64Emul()" */
LONG nWrite64Emul(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    UBYTE cmd10[10];
    ULONG *cmd10sb = (ULONG *)&cmd10[2];
    UBYTE sensedata[18];
    ULONG dataoffset = 0;
    ULONG dataremain = ioreq->io_Length;
    ULONG datalen;
    ULONG startblock;
    ULONG maxtrans = 1UL<<(ncm->ncm_CDC->cdc_MaxTransfer+16);
    ULONG insideblockoffset;
    struct SCSICmd scsicmd;

    if(dataremain & 511)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Attempt to write partial block (%ld %% %ld != 0)!",
                       dataremain, ncm->ncm_BlockSize);
        ioreq->io_Actual = 0;
        return(ioreq->io_Error = IOERR_BADLENGTH);
    }
    if(ioreq->io_Offset & 511)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Attempt to write unaligned block (%ld %% %ld != 0)!",
                       ioreq->io_Offset, ncm->ncm_BlockSize);
        ioreq->io_Actual = 0;
        return(ioreq->io_Error = IOERR_BADADDRESS);
    }

    if(!ncm->ncm_OneBlock || (ncm->ncm_OneBlockSize < ncm->ncm_BlockSize))
    {
        psdFreeVec(ncm->ncm_OneBlock);
        if(!(ncm->ncm_OneBlock = psdAllocVec(ncm->ncm_BlockSize)))
        {
            return(IOERR_BADADDRESS);
        }
        ncm->ncm_OneBlockSize = ncm->ncm_BlockSize;
    }

    startblock = (ioreq->io_Offset>>ncm->ncm_BlockShift)|(ioreq->io_Actual<<(32-ncm->ncm_BlockShift));
    insideblockoffset = (ioreq->io_Offset & ((1<<ncm->ncm_BlockShift)-1));
    while(dataremain)
    {
        KPRINTF(10, ("Writing from block %ld, %ld bytes left...\n", startblock, dataremain));
        datalen = dataremain;
        if(datalen > maxtrans)
        {
            datalen = maxtrans;
        }
        if(insideblockoffset || (datalen < ncm->ncm_BlockSize))
        {
            if(datalen > ncm->ncm_BlockSize-insideblockoffset)
            {
                datalen = ncm->ncm_BlockSize-insideblockoffset;
            }
            scsicmd.scsi_Data = (UWORD *) ncm->ncm_OneBlock;
            scsicmd.scsi_Length = ncm->ncm_BlockSize;
            scsicmd.scsi_Command = cmd10;
            scsicmd.scsi_CmdLength = 10;
            scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
            scsicmd.scsi_SenseData = sensedata;
            scsicmd.scsi_SenseLength = 18;
            cmd10[0] = SCSI_DA_READ_10;
            cmd10[1] = 0;
            *cmd10sb = AROS_LONG2BE(startblock);
            cmd10[6] = 0;
            cmd10[7] = 0;
            cmd10[8] = 1;
            cmd10[9] = 0;
            if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
            {
                KPRINTF(10, ("Read error!\n"));
                break;
            }
            CopyMemQuick(&(((UBYTE *) ioreq->io_Data)[dataoffset]), &ncm->ncm_OneBlock[insideblockoffset], datalen);

            //scsicmd.scsi_Data = (UWORD *) ncm->ncm_OneBlock;
            //scsicmd.scsi_Length = ncm->ncm_BlockSize;
            //scsicmd.scsi_Command = cmd10;
            //scsicmd.scsi_CmdLength = 10;
            scsicmd.scsi_Flags = SCSIF_WRITE|SCSIF_AUTOSENSE|0x80;
            //scsicmd.scsi_SenseData = sensedata;
            //scsicmd.scsi_SenseLength = 18;
            cmd10[0] = SCSI_DA_WRITE_10;
            //cmd10[1] = 0;
            //*((ULONG *) (&cmd10[2])) = AROS_LONG2BE(startblock);
            //cmd10[6] = 0;
            //cmd10[7] = 0;
            //cmd10[8] = 1;
            //cmd10[9] = 0;
            if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
            {
                KPRINTF(10, ("Write error!\n"));
                break;
            }
            insideblockoffset = 0;
            startblock++;
        } else {
            scsicmd.scsi_Data = (UWORD *) &(((UBYTE *) ioreq->io_Data)[dataoffset]);
            scsicmd.scsi_Length = datalen;
            scsicmd.scsi_Command = cmd10;
            scsicmd.scsi_CmdLength = 10;
            scsicmd.scsi_Flags = SCSIF_WRITE|SCSIF_AUTOSENSE|0x80;
            scsicmd.scsi_SenseData = sensedata;
            scsicmd.scsi_SenseLength = 18;
            cmd10[0] = SCSI_DA_WRITE_10;
            cmd10[1] = 0;
            *cmd10sb = AROS_LONG2BE(startblock);
            cmd10[6] = 0;
            cmd10[7] = datalen>>(ncm->ncm_BlockShift+8);
            cmd10[8] = datalen>>ncm->ncm_BlockShift;
            cmd10[9] = 0;
            if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
            {
                KPRINTF(10, ("Write error!\n"));
                break;
            }
            startblock += (insideblockoffset+datalen)>>ncm->ncm_BlockShift;
        }
        dataoffset += datalen;
        dataremain -= datalen;
    }
    ioreq->io_Actual = dataoffset;
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nRead64()" */
LONG nRead64(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    UBYTE cmd16[16];
    UBYTE sensedata[18];
    ULONG dataoffset = 0;
    ULONG dataremain = ioreq->io_Length;
    ULONG datalen;
    ULONG startblockhigh;
    ULONG startblock;
    ULONG oldstartblock;
    ULONG maxtrans = 1UL<<(ncm->ncm_CDC->cdc_MaxTransfer+16);
    struct SCSICmd scsicmd;

    if(!ncm->ncm_BlockSize)
    {
        nGetBlockSize(ncm);
    }
    if((((dataremain >> ncm->ncm_BlockShift)<<ncm->ncm_BlockShift) != dataremain) ||
       (((ioreq->io_Offset >> ncm->ncm_BlockShift)<<ncm->ncm_BlockShift) != ioreq->io_Offset))
    {
        KPRINTF(20, ("unaligned read access offset %ld, length %ld...\n", ioreq->io_Offset, dataremain));
        nApplyFallback(ncm, PFF_EMUL_LARGE_BLK, "Emulate on larger block sizes", TRUE, RETURN_ERROR);
        if(ncm->ncm_CDC->cdc_PatchFlags & PFF_EMUL_LARGE_BLK)
        {
            return(nRead64Emul(ncm, ioreq));
        }
        if((dataremain >> ncm->ncm_BlockShift)<<ncm->ncm_BlockShift != dataremain)
        {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Attempt to read partial block (%ld %% %ld != 0)!",
                           dataremain, ncm->ncm_BlockSize);
            ioreq->io_Error = IOERR_BADLENGTH;
        } else {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Attempt to read unaligned block (%ld %% %ld != 0)!",
                           ioreq->io_Offset, ncm->ncm_BlockSize);
            ioreq->io_Error = IOERR_BADADDRESS;
        }
        if(ncm->ncm_BlockSize != 512)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "The used FileSystem or other software must support %ld byte blocks!",
                           ncm->ncm_BlockSize);
        }
        ioreq->io_Actual = 0;
        return(ioreq->io_Error);
    }

    startblockhigh = ioreq->io_Actual>>ncm->ncm_BlockShift;
    startblock = (ioreq->io_Offset>>ncm->ncm_BlockShift)|(ioreq->io_Actual<<(32-ncm->ncm_BlockShift));
    while(dataremain)
    {
        KPRINTF(10, ("Reading from block %ld, %ld bytes left...\n", startblock, dataremain));
        datalen = dataremain;
        if(datalen > maxtrans)
        {
            datalen = maxtrans;
        }
        scsicmd.scsi_Data = (UWORD *) &(((UBYTE *) ioreq->io_Data)[dataoffset]);
        scsicmd.scsi_Length = datalen;
        scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
        scsicmd.scsi_SenseData = sensedata;
        scsicmd.scsi_SenseLength = 18;
        scsicmd.scsi_Command = cmd16;
        scsicmd.scsi_CmdLength = nBuildRWCdb(cmd16, FALSE, startblockhigh, startblock,
                                             datalen, ncm->ncm_BlockShift);
        if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
        {
            KPRINTF(10, ("Read error!\n"));
            break;
        }
        dataoffset += datalen;
        dataremain -= datalen;
        oldstartblock = startblock;
        startblock += datalen>>ncm->ncm_BlockShift;
        if(startblock < oldstartblock)
        {
            // wrap around occurred
            startblockhigh++;
        }
    }
    ioreq->io_Actual = dataoffset;
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nSeek64()" */
LONG nSeek64(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    UBYTE cmd10[10];
    ULONG *cmd10sb = (ULONG *)&cmd10[2];
    UBYTE sensedata[18];
    struct SCSICmd scsicmd;
    ULONG startblock;

    if(!ncm->ncm_BlockSize)
    {
        nGetBlockSize(ncm);
    }
    if(((ioreq->io_Offset >> ncm->ncm_BlockShift)<<ncm->ncm_BlockShift) != ioreq->io_Offset)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Attempt to to seek to unaligned block (%ld %% %ld != 0)!",
                       ioreq->io_Offset, ncm->ncm_BlockSize);
        if(ncm->ncm_BlockSize != 512)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "The used FileSystem or other software must support %ld byte blocks!",
                           ncm->ncm_BlockSize);
        }
        ioreq->io_Actual = 0;
        return(ioreq->io_Error = IOERR_BADADDRESS);
    }

    startblock = (ioreq->io_Offset>>ncm->ncm_BlockShift)|(ioreq->io_Actual<<(32-ncm->ncm_BlockShift));
    scsicmd.scsi_Data = NULL;
    scsicmd.scsi_Length = 0;
    scsicmd.scsi_Command = cmd10;
    scsicmd.scsi_CmdLength = 10;
    scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
    scsicmd.scsi_SenseData = sensedata;
    scsicmd.scsi_SenseLength = 18;
    cmd10[0] = SCSI_DA_SEEK_10;
    cmd10[1] = 0;
    *cmd10sb = AROS_LONG2BE(startblock);
    cmd10[6] = 0;
    cmd10[7] = 0;
    cmd10[8] = 0;
    cmd10[9] = 0;
    if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
    {
        KPRINTF(10, ("Seek error!\n"));
    }
    ioreq->io_Actual = 0;
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nWrite64()" */
LONG nWrite64(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    UBYTE cmd16[16];
    UBYTE sensedata[18];
    ULONG dataoffset = 0;
    ULONG dataremain = ioreq->io_Length;
    ULONG datalen;
    ULONG startblockhigh;
    ULONG startblock;
    ULONG oldstartblock;
    ULONG maxtrans = 1UL<<(ncm->ncm_CDC->cdc_MaxTransfer+16);
    struct SCSICmd scsicmd;

    if(!ncm->ncm_BlockSize)
    {
        nGetBlockSize(ncm);
    }
    if(((dataremain >> ncm->ncm_BlockShift)<<ncm->ncm_BlockShift != dataremain) ||
       ((ioreq->io_Offset >> ncm->ncm_BlockShift)<<ncm->ncm_BlockShift != ioreq->io_Offset))
    {
        KPRINTF(20, ("unaligned write access offset %ld, length %ld...\n", ioreq->io_Offset, dataremain));
        nApplyFallback(ncm, PFF_EMUL_LARGE_BLK, "Emulate on larger block sizes", TRUE, RETURN_ERROR);
        if(ncm->ncm_CDC->cdc_PatchFlags & PFF_EMUL_LARGE_BLK)
        {
            return(nWrite64Emul(ncm, ioreq));
        }
        if((dataremain >> ncm->ncm_BlockShift)<<ncm->ncm_BlockShift != dataremain)
        {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Attempt to write partial block (%ld %% %ld != 0)!",
                           dataremain, ncm->ncm_BlockSize);
            ioreq->io_Error = IOERR_BADLENGTH;
        } else {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                           "Attempt to write unaligned block (%ld %% %ld != 0)!",
                           ioreq->io_Offset, ncm->ncm_BlockSize);
            ioreq->io_Error = IOERR_BADADDRESS;
        }
        if(ncm->ncm_BlockSize != 512)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "The used FileSystem or other software must support %ld byte blocks!",
                           ncm->ncm_BlockSize);
        }
        ioreq->io_Actual = 0;
        return(ioreq->io_Error);
    }

    startblockhigh = ioreq->io_Actual>>ncm->ncm_BlockShift;
    startblock = (ioreq->io_Offset>>ncm->ncm_BlockShift)|(ioreq->io_Actual<<(32-ncm->ncm_BlockShift));
    while(dataremain)
    {
        datalen = dataremain;
        if(datalen > maxtrans)
        {
            datalen = maxtrans;
        }
        scsicmd.scsi_Data = (UWORD *) &(((UBYTE *) ioreq->io_Data)[dataoffset]);
        scsicmd.scsi_Length = datalen;
        scsicmd.scsi_Flags = SCSIF_WRITE|SCSIF_AUTOSENSE|0x80;
        scsicmd.scsi_SenseData = sensedata;
        scsicmd.scsi_SenseLength = 18;
        scsicmd.scsi_Command = cmd16;
        scsicmd.scsi_CmdLength = nBuildRWCdb(cmd16, TRUE, startblockhigh, startblock,
                                             datalen, ncm->ncm_BlockShift);
        if((ioreq->io_Error = nScsiDirect(ncm, &scsicmd)))
        {
            break;
        }
        dataoffset += datalen;
        dataremain -= datalen;
        oldstartblock = startblock;
        startblock += datalen>>ncm->ncm_BlockShift;
        if(startblock < oldstartblock)
        {
            // wrap around occurred
            startblockhigh++;
        }
    }
    ioreq->io_Actual = dataoffset;
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nScsiDirect()" */
static LONG nScsiDirectInner(struct NepClassMS *ncm, struct SCSICmd *scsicmd)
{
    STRPTR prodname;
    STRPTR vendname;
    UBYTE cmd12[12];
    struct SCSICmd scsicmd10;
    BOOL usecmd10 = FALSE;
    LONG res;
    UBYTE *sensedata = NULL;
    UBYTE *buf;
    BOOL xlate = FALSE;
    ULONG pf = ncm->ncm_CDC->cdc_PatchFlags;

    scsicmd->scsi_Actual = 0;
    scsicmd->scsi_CmdActual = 0;
    scsicmd->scsi_SenseActual = 0;

    if(((pf & PFF_MODE_XLATE) && (scsicmd->scsi_CmdLength == 6)) ||
       (ncm->ncm_CSType == MS_FDDATAPI_SUBCLASS) ||
       (ncm->ncm_CSType == MS_ATAPI_SUBCLASS) ||
       (ncm->ncm_CSType == MS_UFI_SUBCLASS))
    {
        xlate = TRUE;
    }

    if(pf & PFF_SIMPLE_SCSI)
    {
        if(scsicmd->scsi_Flags & SCSIF_AUTOSENSE)
        {
            if(scsicmd->scsi_SenseLength > 18)
            {
                KPRINTF(10, ("Fixing Sense Length to 18!\n"));
                scsicmd->scsi_SenseLength = 18;
            }
        }
        switch(scsicmd->scsi_Command[0])
        {
            case SCSI_TEST_UNIT_READY:
            case SCSI_INQUIRY:
            //case SCSI_LOG_SELECT:
            //case SCSI_LOG_SENSE:
            case SCSI_REQUEST_SENSE:
            //case SCSI_MODE_SELECT_6:
            //case SCSI_MODE_SELECT_10:
            //case SCSI_MODE_SENSE_6:
            //case SCSI_MODE_SENSE_10:
            case SCSI_DA_READ_6:
            case SCSI_DA_READ_10:
            case SCSI_DA_READ_CAPACITY:
            case SCSI_DA_SEEK_6:
            case SCSI_DA_SEEK_10:
            case SCSI_DA_WRITE_6:
            case SCSI_DA_WRITE_10:
                break;

            case SCSI_MODE_SENSE_6:
            {
                UWORD modepage = scsicmd->scsi_Command[2] & 0x3f;
                UBYTE *data = (UBYTE *) scsicmd->scsi_Data;
                if((modepage == 0x3f) ||
                   (modepage == 0x03) ||
                   (modepage == 0x04) ||
                   (modepage == 0x05))
                {
                    if(!(ncm->ncm_BlockSize && ncm->ncm_Geometry.dg_TotalSectors))
                    {
                        nGetBlockSize(ncm);
                    }
                    if(ncm->ncm_Geometry.dg_TotalSectors)
                    {
                        nFakeGeometry(ncm, &ncm->ncm_Geometry);
                        ncm->ncm_GeoChangeCount = ncm->ncm_ChangeCount;
                    }
                    memset(data, 0, (size_t) scsicmd->scsi_Length);
                    scsicmd->scsi_Status = SCSI_GOOD;
                }
                if(modepage == 0x3f) // all available mode pages
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Simple SCSI: Faking All Mode Pages 0x03-0x05");
                    data[0] = 3+2+22+2+22+2+30;
                    data += 4;
                    scsicmd->scsi_Actual = 4;
                    if(scsicmd->scsi_Length >= 4+2+22)
                    {
                        data[0] = 0x03; // mode page
                        data[1] = 22; // page length
                        data[10] = ncm->ncm_Geometry.dg_TrackSectors>>8;
                        data[11] = ncm->ncm_Geometry.dg_TrackSectors;
                        data[12] = ncm->ncm_Geometry.dg_SectorSize>>8;
                        data[13] = ncm->ncm_Geometry.dg_SectorSize;
                        data += 2+22;
                        scsicmd->scsi_Actual += 2+22;
                    }
                    if(scsicmd->scsi_Length >= 4+2+22+2+22)
                    {
                        data[0] = 0x04; // mode page
                        data[1] = 22; // page length
                        data[2] = ncm->ncm_Geometry.dg_Cylinders>>16;
                        data[3] = ncm->ncm_Geometry.dg_Cylinders>>8;
                        data[4] = ncm->ncm_Geometry.dg_Cylinders;
                        data[5] = ncm->ncm_Geometry.dg_Heads;
                        data += 2+22;
                        scsicmd->scsi_Actual += 2+22;
                    }
                    if(scsicmd->scsi_Length >= 4+2+22+2+22+2+30)
                    {
                        data[0] = 0x05; // mode page
                        data[1] = 30; // page length
                        data[4] = ncm->ncm_Geometry.dg_Heads;
                        data[5] = ncm->ncm_Geometry.dg_TrackSectors;
                        data[6] = ncm->ncm_Geometry.dg_SectorSize>>8;
                        data[7] = ncm->ncm_Geometry.dg_SectorSize;
                        data[8] = ncm->ncm_Geometry.dg_Cylinders>>8;
                        data[9] = ncm->ncm_Geometry.dg_Cylinders;
                        //data += 2+30;
                        scsicmd->scsi_Actual += 2+30;
                    }
                    return(0);
                }
                else if((modepage == 0x03) && (scsicmd->scsi_Length >= 6+22)) // Format Device mode page
                {
                    // fake geometry request
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Simple SCSI: Faking Mode Page 0x03 (Format Device)");

                    data[0] = 5+22; // length
                    data += 4;
                    data[0] = 0x03; // mode page
                    data[1] = 22; // page length
                    data[10] = ncm->ncm_Geometry.dg_TrackSectors>>8;
                    data[11] = ncm->ncm_Geometry.dg_TrackSectors;
                    data[12] = ncm->ncm_Geometry.dg_SectorSize>>8;
                    data[13] = ncm->ncm_Geometry.dg_SectorSize;
                    scsicmd->scsi_Actual = 6+22;
                    return(0);
                }
                else if((modepage == 0x04) && (scsicmd->scsi_Length >= 6+22)) //  Rigid Disk Geometry mode page
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Simple SCSI: Faking Mode Page 0x04 (Rigid Disk Geometry)");
                    data[0] = 5+22; // length
                    data += 4;
                    data[0] = 0x04; // mode page
                    data[1] = 22; // page length
                    data[2] = ncm->ncm_Geometry.dg_Cylinders>>16;
                    data[3] = ncm->ncm_Geometry.dg_Cylinders>>8;
                    data[4] = ncm->ncm_Geometry.dg_Cylinders;
                    data[5] = ncm->ncm_Geometry.dg_Heads;
                    scsicmd->scsi_Actual = 6+22;
                    return(0);
                }
                else if((modepage == 0x05) && (scsicmd->scsi_Length >= 6+30)) // Flexible Disk mode page
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Simple SCSI: Faking Mode Page 0x05 (Flexible Disk)");
                    data[0] = 5+30; // length
                    data += 4;
                    data[0] = 0x05; // mode page
                    data[1] = 30; // page length
                    data[4] = ncm->ncm_Geometry.dg_Heads;
                    data[5] = ncm->ncm_Geometry.dg_TrackSectors;
                    data[6] = ncm->ncm_Geometry.dg_SectorSize>>8;
                    data[7] = ncm->ncm_Geometry.dg_SectorSize;
                    data[8] = ncm->ncm_Geometry.dg_Cylinders>>8;
                    data[9] = ncm->ncm_Geometry.dg_Cylinders;
                    scsicmd->scsi_Actual = 6+30;
                    return(0);
                }
                // fall through
            }

            case 0x51: // drive ready
            case SCSI_CD_READ_TOC:
            case SCSI_CD_PAUSE_RESUME:
            case SCSI_CD_PLAY_AUDIO_10:
            case SCSI_CD_PLAY_AUDIO_12:
            case SCSI_CD_PLAY_AUDIO_MSF:
            case SCSI_CD_PLAY_AUDIO_TRACK_INDEX:
            case SCSI_CD_PLAY_TRACK_RELATIVE_10:
            case SCSI_CD_PLAY_TRACK_RELATIVE_12:
            case SCSI_CD_BLANK:
            case SCSI_CD_CLOSE_TRACK:
            case SCSI_CD_GET_CONFIGURATION:
            case SCSI_CD_GET_EVENT_STATUS_NOTIFICATION:
            case SCSI_CD_GET_PERFORMANCE:
            case SCSI_CD_LOAD_UNLOAD_MEDIUM:
            case SCSI_DA_START_STOP_UNIT:
            case SCSI_CD_MECHANISM_STATUS:
            case SCSI_CD_PRE_FETCH:
            case SCSI_CD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            case SCSI_CD_READ_HEADER:
            case SCSI_CD_READ_SUB_CHANNEL:
                if((scsicmd->scsi_Command[0] != SCSI_MODE_SENSE_6) &&
                   ((ncm->ncm_DeviceType == PDT_WORM) || (ncm->ncm_DeviceType == PDT_CDROM)))
                {
                    // allows these CD rom commands even with SimpleSCSI enabled.
                    break;
                }

            default:
            {
                UBYTE cmdstrbuf[16*3+2];

                nHexString(scsicmd->scsi_Command, (ULONG) (scsicmd->scsi_CmdLength < 16 ? scsicmd->scsi_CmdLength : 16), cmdstrbuf);
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Simple SCSI: Filtering SCSI command %s", cmdstrbuf);

                scsicmd->scsi_Status = SCSI_CHECK_CONDITION;
                scsicmd->scsi_SenseActual = 0;
                if(scsicmd->scsi_Flags & SCSIF_AUTOSENSE)
                {
                    if(scsicmd->scsi_SenseLength >= 18)
                    {
                        memset(scsicmd->scsi_SenseData, 0x00, 18);
                        scsicmd->scsi_SenseData[0] = 0x80|0x70;
                        scsicmd->scsi_SenseData[2] = SK_ILLEGAL_REQUEST;
                        scsicmd->scsi_SenseData[12] = 0x20; // unsupported command
                        scsicmd->scsi_SenseActual = 18;
                    }
                }
                return(-1);
            }
        }
    }

    if((scsicmd->scsi_Command[0] == SCSI_TEST_UNIT_READY) && scsicmd->scsi_Length)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Bogus TEST_UNIT_READY data length set to 0.");
        scsicmd->scsi_Length = 0;
    }

    if(pf & PFF_FAKE_INQUIRY)
    {
        if(scsicmd->scsi_Command[0] == SCSI_INQUIRY)
        {
            if(scsicmd->scsi_Length >= 36)
            {
                UBYTE *data = (UBYTE *) scsicmd->scsi_Data;
                if(pf & PFF_DEBUG)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Faking Inquiry.");
                }
                KPRINTF(10, ("Faking INQUIRY!\n"));
                psdGetAttrs(PGA_DEVICE, ncm->ncm_Device,
                            DA_ProductName, &prodname,
                            DA_Manufacturer, &vendname,
                            TAG_END);
                memset(data, 0x00, (size_t) scsicmd->scsi_Length);
                data[1] = 0x80; // removable media
                data[2] = 0x02; // 0x03 would be ANSI X3.301:1997 (SPC).
                data[3] = 0x02; // Response data format = 2
                data[4] = 32; // additional length n-4
                CopyMem(vendname, &data[8], (ULONG) ((strlen(vendname) < 8) ? strlen(vendname) : 8));
                CopyMem(prodname, &data[16], (ULONG) ((strlen(prodname) < 16) ? strlen(prodname) : 16));
                scsicmd->scsi_CmdActual = scsicmd->scsi_CmdLength;
                scsicmd->scsi_Status = SCSI_GOOD;
                scsicmd->scsi_SenseActual = 0;
                return(0);
            }
            return(-1);
        }
    }

    if(pf & PFF_FIX_INQ36)
    {
        if((scsicmd->scsi_Command)[0] == SCSI_INQUIRY)
        {
            if(scsicmd->scsi_Length > 36)
            {
                if(pf & PFF_DEBUG)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Fixing Inquiry.");
                }

                KPRINTF(10, ("Fixing INQUIRY!\n"));
                scsicmd->scsi_Length = 36;
            }
            else if(scsicmd->scsi_Length < 36)
            {
                if(pf & PFF_DEBUG)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Couldn't fix Inquiry < %ld.",
                                   scsicmd->scsi_Length);
                }
                KPRINTF(10, ("Couldn't fix INQUIRY!\n"));
            }
        }
    }

    if(xlate)
    {
        scsicmd10 = *scsicmd;
        scsicmd10.scsi_Command = cmd12;
        scsicmd10.scsi_CmdLength = 10;
        memset(cmd12, 0x00, 12);
        switch(scsicmd->scsi_Command[0])
        {
            case SCSI_DA_READ_6:
            case SCSI_DA_WRITE_6:
            case SCSI_DA_SEEK_6:
                cmd12[0] = scsicmd->scsi_Command[0] | 0x20;
                cmd12[1] = scsicmd->scsi_Command[1] & 0xe0;
                cmd12[3] = scsicmd->scsi_Command[1] & 0x1f;
                cmd12[4] = scsicmd->scsi_Command[2];
                cmd12[5] = scsicmd->scsi_Command[3];
                cmd12[8] = scsicmd->scsi_Command[4];
                cmd12[9] = scsicmd->scsi_Command[5];
                usecmd10 = TRUE;
                break;

            case SCSI_MODE_SELECT_6:
                cmd12[0] = SCSI_MODE_SELECT_10;
                cmd12[1] = scsicmd->scsi_Command[1];
                //cmd12[2] = scsicmd->scsi_Command[2]; // reserved
                //cmd12[3] = scsicmd->scsi_Command[3]; // reserved
                cmd12[7] = (scsicmd->scsi_Command[4]+4)>>8;
                cmd12[8] = scsicmd->scsi_Command[4]+4;
                cmd12[9] = scsicmd->scsi_Command[5];

                sensedata = psdAllocVec(scsicmd->scsi_Length+4);
                if(sensedata && (scsicmd->scsi_Length >= 4))
                {
                    buf = (UBYTE *) scsicmd->scsi_Data;
                    sensedata[1] = *buf++;
                    sensedata[2] = *buf++;
                    sensedata[3] = *buf++;
                    sensedata[7] = *buf++;

                    scsicmd10.scsi_Length = scsicmd->scsi_Length+4;
                    scsicmd10.scsi_Data = (UWORD *) sensedata;

                    CopyMem(buf, &sensedata[8], scsicmd->scsi_Length-4);
                }
                usecmd10 = TRUE;
                break;

            case SCSI_MODE_SENSE_6:
                cmd12[0] = SCSI_MODE_SENSE_10;
                cmd12[1] = scsicmd->scsi_Command[1] & 0xf7;
                cmd12[2] = scsicmd->scsi_Command[2];
                cmd12[3] = scsicmd->scsi_Command[3];
                // Workaround: Some devices are seriously broken and do not interpret
                // the upper byte of the allocation length field.
                // Hence they return 3 bytes instead of 259 bytes. For this special case,
                // we will simply truncate the size by four, to get back to a 255 byte
                // buffer.
                if((scsicmd->scsi_Command[4] > 251) && (scsicmd->scsi_Length == scsicmd->scsi_Command[4]))
                {
                    scsicmd->scsi_Command[4] -= 4;
                    scsicmd->scsi_Length -= 4;
                }
                cmd12[7] = (scsicmd->scsi_Command[4]+4)>>8;
                cmd12[8] = scsicmd->scsi_Command[4]+4;
                cmd12[9] = scsicmd->scsi_Command[5];
                sensedata = psdAllocVec(scsicmd->scsi_Length+4);
                if(sensedata)
                {
                    scsicmd10.scsi_Length = scsicmd->scsi_Length+4;
                    scsicmd10.scsi_Data = (UWORD *) sensedata;
                }
                usecmd10 = TRUE;
                break;
        }
    }

    if((ncm->ncm_CSType == MS_ATAPI_SUBCLASS) ||
       (ncm->ncm_CSType == MS_FDDATAPI_SUBCLASS) ||
       (ncm->ncm_CSType == MS_UFI_SUBCLASS))
    {
        if(!usecmd10)
        {
            scsicmd10 = *scsicmd;
            scsicmd10.scsi_Command = cmd12;
            memset(cmd12, 0x00, 12);
            CopyMem(scsicmd->scsi_Command, cmd12, (ULONG) scsicmd->scsi_CmdLength);
            usecmd10 = TRUE;
        }
        scsicmd10.scsi_CmdLength = 12;
        if(ncm->ncm_CSType == MS_UFI_SUBCLASS)
        {
            switch(cmd12[0])
            {
                case SCSI_REQUEST_SENSE:
                    cmd12[4] = 18; /* restrict UFI response to 18 bytes */
                    scsicmd10.scsi_Flags &= ~SCSIF_AUTOSENSE;
                    break;

                case SCSI_INQUIRY:
                    KPRINTF(10, ("Disabling autosense for UFI!\n"));
                    cmd12[4] = 36; /* restrict UFI response to 36 bytes */
                    scsicmd10.scsi_Flags &= ~SCSIF_AUTOSENSE;
                    break;

                case SCSI_MODE_SELECT_10:
                    cmd12[7] = 0;
                    cmd12[8] = 8;
                    break;

            }
        }
    }

    if(usecmd10)
    {
        if(pf & PFF_DEBUG)
        {
            UBYTE cmd1strbuf[16*3+2];
            UBYTE cmd2strbuf[16*3+2];

            nHexString(scsicmd->scsi_Command, (ULONG) (scsicmd->scsi_CmdLength < 16 ? scsicmd->scsi_CmdLength : 16), cmd1strbuf);
            nHexString(scsicmd10.scsi_Command, (ULONG) (scsicmd10.scsi_CmdLength < 16 ? scsicmd10.scsi_CmdLength : 16), cmd2strbuf);
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Mode XLATE for %s -> %s", cmd1strbuf, cmd2strbuf);
            if(scsicmd->scsi_Length && (scsicmd10.scsi_Command[0] == SCSI_MODE_SELECT_10))
            {
                nHexString((UBYTE *) scsicmd->scsi_Data, (ULONG) (scsicmd->scsi_Length < 16 ? scsicmd->scsi_Length : 16), cmd1strbuf);
                nHexString((UBYTE *) scsicmd10.scsi_Data, (ULONG) (scsicmd10.scsi_Length < 16 ? scsicmd10.scsi_Length : 16), cmd2strbuf);
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                               "Request: %s (%ld) -> %s (%ld)",
                               cmd1strbuf, scsicmd->scsi_Length,
                               cmd2strbuf, scsicmd10.scsi_Length);
            }
        }
        KPRINTF(20, ("Mode XLATE for cmd %lx\n", scsicmd->scsi_Command[0]));
    }

    switch(ncm->ncm_TPType)
    {
        case MS_PROTO_BULK:
            res = nScsiDirectBulk(ncm, usecmd10 ? &scsicmd10 : scsicmd);
            break;

        case MS_PROTO_UAS:
            res = nScsiDirectUAS(ncm, usecmd10 ? &scsicmd10 : scsicmd);
            break;

        case MS_PROTO_CB:
        case MS_PROTO_CBI:
            res = nScsiDirectCBI(ncm, usecmd10 ? &scsicmd10 : scsicmd);
            break;

        default:
            return(-1);
    }
    KPRINTF(20, ("Cmd done %ld\n", res));
    if(usecmd10)
    {
        if(sensedata)
        {
            scsicmd->scsi_Actual = 0;
            if(scsicmd10.scsi_Command[0] == SCSI_MODE_SENSE_10)
            {
                UBYTE cmd1strbuf[16*3+2];
                UBYTE cmd2strbuf[16*3+2];

                if(scsicmd10.scsi_Actual >= 8)
                {
                    scsicmd->scsi_Actual = scsicmd10.scsi_Actual - 4;
                    buf = (UBYTE *) scsicmd->scsi_Data;
                    *buf++ = sensedata[1];
                    *buf++ = sensedata[2];
                    *buf++ = sensedata[3];
                    *buf++ = sensedata[7];
                    CopyMem(&sensedata[8], buf, (ULONG) scsicmd10.scsi_Actual - 8);
                    if(pf & PFF_DEBUG)
                    {
                        nHexString((UBYTE *) scsicmd10.scsi_Data, scsicmd10.scsi_Actual < 16 ? scsicmd10.scsi_Actual : 16, cmd1strbuf);
                        nHexString((UBYTE *) scsicmd->scsi_Data, scsicmd->scsi_Actual < 16 ? scsicmd->scsi_Actual : 16, cmd2strbuf);
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                       "Response: %s (%ld) -> %s (%ld)",
                                       cmd1strbuf, scsicmd10.scsi_Actual,
                                       cmd2strbuf, scsicmd->scsi_Actual);
                    }
                }
            }
            psdFreeVec(sensedata);
        } else {
            scsicmd->scsi_Actual = scsicmd10.scsi_Actual;
        }
        scsicmd->scsi_CmdActual = scsicmd10.scsi_CmdActual;
        scsicmd->scsi_Status = scsicmd10.scsi_Status;
        scsicmd->scsi_SenseActual = scsicmd10.scsi_SenseActual;
    }

    pf = ncm->ncm_CDC->cdc_PatchFlags;
    if((res == HFERR_Phase) && (!(pf & PFF_SIMPLE_SCSI)))
    {
        switch(scsicmd->scsi_Command[0])
        {
            case SCSI_TEST_UNIT_READY:
            case SCSI_INQUIRY:
            //case SCSI_LOG_SELECT:
            //case SCSI_LOG_SENSE:
            case SCSI_REQUEST_SENSE:
            //case SCSI_MODE_SELECT_6:
            //case SCSI_MODE_SELECT_10:
            //case SCSI_MODE_SENSE_6:
            //case SCSI_MODE_SENSE_10:
            case SCSI_DA_READ_6:
            case SCSI_DA_READ_10:
            case SCSI_DA_READ_CAPACITY:
            case SCSI_DA_SEEK_6:
            case SCSI_DA_SEEK_10:
            case SCSI_DA_WRITE_6:
            case SCSI_DA_WRITE_10:
                break;

            default:
                nApplyFallback(ncm, PFF_SIMPLE_SCSI, "Simple SCSI", TRUE, RETURN_WARN);
                break;
        }
    }

    if((!res) && ((scsicmd->scsi_Command)[0] == SCSI_DA_READ_CAPACITY) && (pf & PFF_FIX_CAPACITY) && (scsicmd->scsi_Length >= 8))
    {
        ULONG *capacity = ((ULONG *) scsicmd->scsi_Data);
        *capacity = AROS_LONG2BE(AROS_BE2LONG(*capacity)-1);
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Fix Capacity: Correcting number of blocks.");
    }

    if(res && (scsicmd->scsi_Command[0] == SCSI_INQUIRY))
    {
        /* a ladder: clamp the allocation length first, synthesize after */
        if(!(ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_INQ36))
        {
            nApplyFallback(ncm, PFF_FIX_INQ36, "Trim Inquiry", TRUE, RETURN_WARN);
        } else {
            nApplyFallback(ncm, PFF_FAKE_INQUIRY, "Fake Inquiry", TRUE, RETURN_WARN);
        }
    }

    if((ncm->ncm_DeviceType == PDT_WORM) ||
       (ncm->ncm_DeviceType == PDT_CDROM))
    {
        // cd roms are always write protected
        ncm->ncm_WriteProtect = TRUE;
        return(res);
    }
    // don't try to obtain write protection, when there's no media inserted.
    if(!ncm->ncm_UnitReady)
    {
        return(res);
    }
    if((!res) && (scsicmd->scsi_Command[0] == SCSI_MODE_SENSE_6))
    {
        if(((UBYTE *) scsicmd->scsi_Data)[2] & 0x80)
        {
            if(!ncm->ncm_WriteProtect)
            {
                if(pf & PFF_REM_SUPPORT)
                {
                    ncm->ncm_ChangeCount++;
                    if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                    {
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Diskchange: Sense6WriteProtect On (count = %ld)",
                                       ncm->ncm_ChangeCount);
                    }
                }
                ncm->ncm_WriteProtect = TRUE;
            }
        } else {
            if(ncm->ncm_WriteProtect)
            {
                if(pf & PFF_REM_SUPPORT)
                {
                    ncm->ncm_ChangeCount++;
                    if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                    {
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Diskchange: Sense6WriteProtect Off (count = %ld)",
                                       ncm->ncm_ChangeCount);
                    }
                }
                ncm->ncm_WriteProtect = FALSE;
            }
        }
    }
    if((!res) && (scsicmd->scsi_Command[0] == SCSI_MODE_SENSE_10))
    {
        if(((UBYTE *) scsicmd->scsi_Data)[3] & 0x80)
        {
            if(!ncm->ncm_WriteProtect)
            {
                if(pf & PFF_REM_SUPPORT)
                {
                    ncm->ncm_ChangeCount++;
                    if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                    {
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Diskchange: Sense10WriteProtect On (count = %ld)",
                                       ncm->ncm_ChangeCount);
                    }
                }
                ncm->ncm_WriteProtect = TRUE;
            }
        } else {
            if(ncm->ncm_WriteProtect)
            {
                if(pf & PFF_REM_SUPPORT)
                {
                    ncm->ncm_ChangeCount++;
                    if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                    {
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Diskchange: Sense10WriteProtect Off (count = %ld)",
                                       ncm->ncm_ChangeCount);
                    }
                }
                ncm->ncm_WriteProtect = FALSE;
            }
        }
    }
    return(res);
}

LONG nScsiDirect(struct NepClassMS *ncm, struct SCSICmd *scsicmd)
{
    /* Busy bracket for the suspend probe (UCM_AttemptSuspendDevice runs in
       the caller's task): counts wire-command nesting so suspend is refused
       mid-command. */
    ncm->ncm_CmdBusy++;
    LONG res = nScsiDirectInner(ncm, scsicmd);
    ncm->ncm_CmdBusy--;
    return(res);
}
/* \\\ */

/* /// "nLockXFer()" */
void nLockXFer(struct NepClassMS *ncm)
{
    if(ncm->ncm_MaxLUN)
    {
        KPRINTF(1, ("PING(lock)\n"));
        ObtainSemaphore(&ncm->ncm_UnitLUN0->ncm_XFerLock);
        KPRINTF(1, ("PONG(lock)\n"));
    }
}
/* \\\ */

/* /// "nUnlockXFer()" */
void nUnlockXFer(struct NepClassMS *ncm)
{
    if(ncm->ncm_MaxLUN)
    {
        KPRINTF(1, ("PING(unlock)\n"));
        ReleaseSemaphore(&ncm->ncm_UnitLUN0->ncm_XFerLock);
        KPRINTF(1, ("PONG(unlock)\n"));
    }
}
/* \\\ */

/* /// "nApplyFallback()" */
/* nTryFallback(), and remember the change in the device's record. For the
   unit task: it goes through the task's own neptune.library base. */
BOOL nApplyFallback(struct NepClassMS *ncm, ULONG flag, CONST_STRPTR name,
                    BOOL enable, LONG rc)
{
    if(!nTryFallback(ps, ncm, flag, name, enable, rc))
    {
        return(FALSE);
    }
    nStoreConfig(ps, ncm);
    return(TRUE);
}
/* \\\ */

/* ********************************************************************* */

/* /// "BSTR Macros" */
/* BSTR = classic AmigaOS BCPL string (length byte + chars). */

#define b2cstr(bstr, cstr) { ULONG i; for (i = 0; i < bstr[0]; i++) cstr[i] = bstr[i + 1]; cstr[i] = 0x00; }
/* \\\ */

#undef  ps
#define ps nh->nh_PsdBase
/* Expands against each function's own nh local (see nRemoveDosNode); the
   library itself is opened and owned by the removable task (nAllocRT). */
#undef  ExpansionBase
#define ExpansionBase nh->nh_ExpansionBase

/* /// "nOpenDOSLib()" */
/* Bare dos.library open/cache. Callers must ensure their own task may talk
   DOS (the safe-eject path verifies the calling task is a Process itself). */
BOOL nOpenDOSLib(struct NepMSBase *nh)
{
    if(nh->nh_DOSBase)
    {
        return(TRUE);
    }
    if((nh->nh_DOSBase = OpenLibrary("dos.library", 39)))
    {
        KPRINTF(10, ("Opened dos.library!\n"));
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "nOpenDOS()" */
/* Removable-task flavour: proxies "is DOS usable" through the removable
   task's node type, because that is the task doing the DoPkt()s here. */
BOOL nOpenDOS(struct NepMSBase *nh)
{
    if(nh->nh_DOSBase)
    {
        return(TRUE);
    }
    if(nh->nh_RemovableTask->tc_Node.ln_Type != NT_PROCESS)
    {
        return(FALSE);
    }
    return(nOpenDOSLib(nh));
}
/* \\\ */

#undef DOSBase
#define	DOSBase	nh->nh_DOSBase

/* /// "nRemoveDosNode()" */
/* Rip one DeviceNode out of the DOS list (log, RemDosEntry, stale-BootNode
   unlink). The handler packets are the caller's business: the legacy path
   fires INHIBIT+DIE blind, the safe-eject path has already negotiated them. */
void nRemoveDosNode(struct NepClassMS *ncm, struct DeviceNode *node)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;

    /* A BSTR length byte reaches 255; nDosEntryMatches() also matches
       user-authored DOSDrivers whose names are longer than the mounter's own
       (<= 30), so size for the worst case (matches device[256] there). */
    char partname[256];
    UBYTE *bstr = (UBYTE *) BADDR(node->dn_Name);
    b2cstr(bstr, partname);
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "Unmounting partition %s...",
                   partname);
    if(LockDosList(LDF_DEVICES | LDF_WRITE))
    {
        RemDosEntry((struct DosList *) node);
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
    }
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

/* /// "nUnmountPartition()" */
/* Legacy reactive teardown (device already gone): packets fired blind,
   results deliberately ignored - there is nobody left to veto for. */
void nUnmountPartition(struct NepClassMS *ncm)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    struct DeviceNode *node;
    struct DeviceNode *oldnode = NULL;

    if(!nOpenDOS(nh))
    {
        return;
    }
    while((node = FindMatchingDevice(ncm)))
    {
        if(oldnode == node)
        {
            break;
        }
        if(node->dn_Task)   /* a never-accessed handler has no process yet */
        {
            DoPkt(node->dn_Task, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0);
            DoPkt(node->dn_Task, ACTION_DIE, 0, 0, 0, 0, 0);
        }
        nRemoveDosNode(ncm, node);
        oldnode = node;
    }
}
/* \\\ */

/* /// "nIOCmdTunnel()" */
LONG nIOCmdTunnel(struct NepClassMS *ncm, struct IOStdReq *ioreq)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    Forbid();
    if(ncm->ncm_DenyRequests)
    {
        Permit();
        return(ioreq->io_Error = IOERR_ABORTED);
    }
    PutMsg(&ncm->ncm_Unit.unit_MsgPort, (struct Message *) ioreq);
    Permit();
    while(!GetMsg(nh->nh_IOMsgPort))
    {
        WaitPort(nh->nh_IOMsgPort);
    }
    return(ioreq->io_Error);
}
/* \\\ */

/* /// "nScsiDirectTunnel()" */
LONG nScsiDirectTunnel(struct NepClassMS *ncm, struct SCSICmd *scsicmd)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    struct IOStdReq *ioreq = &nh->nh_IOReq;
    ioreq->io_Command = HD_SCSICMD;
    ioreq->io_Data = scsicmd;
    ioreq->io_Length = sizeof(*scsicmd);
    return(nIOCmdTunnel(ncm, ioreq));
}
/* \\\ */

/* /// "nGetWriteProtect()" */
LONG nGetWriteProtect(struct NepClassMS *ncm)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    UBYTE cmd10[10];
    struct SCSICmd scsicmd;
    UBYTE inquirydata[256];
    UBYTE sensedata[18];
    LONG ioerr;

    if((ncm->ncm_DeviceType == PDT_WORM) ||
       (ncm->ncm_DeviceType == PDT_CDROM))
    {
        // cd roms are always write protected
        return(ncm->ncm_WriteProtect = TRUE);
    }
    if(ncm->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI)
    {
        return(ncm->ncm_WriteProtect = FALSE);
    }
    scsicmd.scsi_Data = (UWORD *) inquirydata;
    scsicmd.scsi_Length = 8;
    scsicmd.scsi_Command = cmd10;
    scsicmd.scsi_CmdLength = 10;
    scsicmd.scsi_Flags = SCSIF_READ|SCSIF_AUTOSENSE|0x80;
    scsicmd.scsi_SenseData = sensedata;
    scsicmd.scsi_SenseLength = 18;
    cmd10[0] = SCSI_MODE_SENSE_10;
    cmd10[1] = 0x00; /* no block descriptors */
    cmd10[2] = 0x3f; /* no page, only header */
    cmd10[3] = 0;
    cmd10[4] = 0;
    cmd10[5] = 0;
    cmd10[6] = 0;
    cmd10[7] = 0;
    cmd10[8] = 8;
    cmd10[9] = 0;
    if((ioerr = nScsiDirectTunnel(ncm, &scsicmd)))
    {
        cmd10[2] = 0x00; /* try again with vendor page, only header */
        if((ioerr = nScsiDirectTunnel(ncm, &scsicmd)))
        {
            cmd10[2] = 0x3f; /* try again with 255 length */
            cmd10[8] = 0xff;
            if((ioerr = nScsiDirectTunnel(ncm, &scsicmd)))
            {
                if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                   "Failed to get write protection state: %ld",
                                   ioerr);
                }
                nApplyFallback(ncm, PFF_SIMPLE_SCSI, "Simple SCSI", TRUE, RETURN_WARN);
                return(0);
            }
        }
    }
    if(inquirydata[3] & 0x80)
    {
        if(!ncm->ncm_WriteProtect)
        {
            if(ncm->ncm_CDC->cdc_PatchFlags & PFF_REM_SUPPORT)
            {
                ncm->ncm_ChangeCount++;
                if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                {
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "Diskchange: GetWriteProtect On (count = %ld)",
                                   ncm->ncm_ChangeCount);
                }
            }
            ncm->ncm_WriteProtect = TRUE;
        }
    } else {
        if(ncm->ncm_WriteProtect)
        {
            if(ncm->ncm_CDC->cdc_PatchFlags & PFF_REM_SUPPORT)
            {
                ncm->ncm_ChangeCount++;
                if(ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG)
                {
                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   "Diskchange: GetWriteProtect Off (count = %ld)",
                                   ncm->ncm_ChangeCount);
                }
            }
            ncm->ncm_WriteProtect = FALSE;
        }
    }
    return(ncm->ncm_WriteProtect);
}
/* \\\ */

/* /// "nDosEntryMatches()" */
/* TRUE if this DOS device entry's startup points at our usbscsi.device unit
   (i.e. anything the mounter - or a user-authored DOSDriver - created for
   this medium). Caller holds a DOS device list lock. */
BOOL nDosEntryMatches(struct NepClassMS *ncm, struct DosList *list)
{
    struct FileSysStartupMsg *fssm = BADDR(list->dol_misc.dol_handler.dol_Startup);

    if(fssm > (struct FileSysStartupMsg *) 0x1000)
    {
        struct DosEnvec *de = BADDR(fssm->fssm_Environ);
        UBYTE *devname = BADDR(fssm->fssm_Device);

        if(TypeOfMem(de) && TypeOfMem(devname) && (de->de_TableSize > 0) && (de->de_TableSize < 32))
        {
            char device[256];
            b2cstr(devname, device);
            if((ncm->ncm_UnitNo == fssm->fssm_Unit) && (strcmp(DEVNAME, device) == 0))
            {
                return(TRUE);
            }
        }
    }
    return(FALSE);
}
/* \\\ */

/* /// "FindMatchingDevice()" */
struct DeviceNode * FindMatchingDevice(struct NepClassMS *ncm)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    struct DeviceNode *node = NULL;
    struct DosList *list;

    if(!nOpenDOS(nh))
    {
        return(NULL);
    }

    if((list = LockDosList(LDF_DEVICES | LDF_READ)))
    {
        while((list = NextDosEntry(list, LDF_DEVICES | LDF_READ)))
        {
            if(nDosEntryMatches(ncm, list))
            {
                node = (struct DeviceNode *) list;
                break;
            }
        }
        UnLockDosList(LDF_DEVICES | LDF_READ);
    }
    return(node);
}
/* \\\ */

/* /// "nBusyName()" */
/* Human-readable name of the thing vetoing an eject: the mounted volume if
   the handler has one ("PenDrive (UMSD0:)"), else the bare DOS device name.
   Locks only - never sends packets (the handler just vetoed one). */
void nBusyName(struct NepClassMS *ncm, struct DeviceNode *node, STRPTR buf, ULONG len)
{
    struct NepMSBase *nh = ncm->ncm_ClsBase;
    char devname[256];
    char volname[256];
    UBYTE *bstr = (UBYTE *) BADDR(node->dn_Name);
    b2cstr(bstr, devname);
    volname[0] = 0;
    struct DosList *list;
    if((list = LockDosList(LDF_VOLUMES | LDF_READ)))
    {
        while((list = NextDosEntry(list, LDF_VOLUMES | LDF_READ)))
        {
            if(list->dol_Task == node->dn_Task)
            {
                UBYTE *vstr = (UBYTE *) BADDR(list->dol_Name);
                b2cstr(vstr, volname);
                break;
            }
        }
        UnLockDosList(LDF_VOLUMES | LDF_READ);
    }
    if(volname[0])
    {
        psdSafeRawDoFmt(buf, len, "%s (%s:)", volname, devname);
    } else {
        psdSafeRawDoFmt(buf, len, "%s:", devname);
    }
}
/* \\\ */

/* Safe eject (UCM_SafeEject). The unit the caller happened to name is only a
   handle on its device: everything below works on all of that device's units,
   because one USB device is what the user unplugs. */

#define NEJECT_MAXNODES 32

struct SafeEjectNode
{
    struct DeviceNode *sen_Node;
    struct NepClassMS *sen_LUN;
    BOOL               sen_Inhibited;
};

/* /// "nNextLUN()" */
/* Iterator over the units belonging to one device: pass NULL to start, the
   previous unit to continue, NULL comes back at the end. */
static struct NepClassMS * nNextLUN(struct NepMSBase *nh, struct PsdDevice *pd,
                                   struct NepClassMS *lun)
{
    lun = lun ? (struct NepClassMS *) lun->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ
              : (struct NepClassMS *) nh->nh_Units.lh_Head;
    while(lun->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ)
    {
        if(lun->ncm_Device == pd)
        {
            return(lun);
        }
        lun = (struct NepClassMS *) lun->ncm_Unit.unit_MsgPort.mp_Node.ln_Succ;
    }
    return(NULL);
}
/* \\\ */

/* /// "nSetEjectLatch()" */
/* Arm/disarm the re-mount block on every unit of the device at once, so the
   removable task's 3s tick cannot mount behind an eject in progress. */
static void nSetEjectLatch(struct NepMSBase *nh, struct PsdDevice *pd, BOOL on)
{
    struct NepClassMS *lun;

    Forbid();
    for(lun = nNextLUN(nh, pd, NULL); lun; lun = nNextLUN(nh, pd, lun))
    {
        lun->ncm_Ejected = on;
    }
    Permit();
}
/* \\\ */

/* /// "nLUNForDosEntry()" */
/* Which of the device's units, if any, does this DOS device entry ride on?
   Caller holds a DOS device list lock. */
static struct NepClassMS * nLUNForDosEntry(struct NepMSBase *nh, struct PsdDevice *pd,
                                          struct DosList *list)
{
    struct NepClassMS *lun;

    for(lun = nNextLUN(nh, pd, NULL); lun; lun = nNextLUN(nh, pd, lun))
    {
        if(nDosEntryMatches(lun, list))
        {
            return(lun);
        }
    }
    return(NULL);
}
/* \\\ */

/* /// "nCollectEjectNodes()" */
/* Gather every DOS device node riding one of the device's units under ONE read
   lock - and never send a packet while holding it (an inhibit may need the very
   list we would be locking). Returns the number collected, or -1 if there were
   more than max: all-or-nothing cannot be promised beyond the array. */
static LONG nCollectEjectNodes(struct NepMSBase *nh, struct PsdDevice *pd,
                               struct SafeEjectNode *nodes, LONG max)
{
    LONG count = 0;
    struct DosList *list;

    if((list = LockDosList(LDF_DEVICES | LDF_READ)))
    {
        while((list = NextDosEntry(list, LDF_DEVICES | LDF_READ)))
        {
            struct NepClassMS *lun = nLUNForDosEntry(nh, pd, list);
            if(!lun)
            {
                continue;
            }
            if(count == max)
            {
                count = -1;
                break;
            }
            nodes[count].sen_Node = (struct DeviceNode *) list;
            nodes[count].sen_LUN = lun;
            nodes[count].sen_Inhibited = FALSE;
            count++;
        }
        UnLockDosList(LDF_DEVICES | LDF_READ);
    }
    if(count < 0)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Eject: more than %ld partitions on one device?!",
                       (ULONG) max);
    }
    return(count);
}
/* \\\ */

/* /// "nInhibitAll()" */
/* Flush and inhibit every collected handler, all-or-nothing: on a veto the
   ones already inhibited are released again and busybuf names the offender.
   The caller must be a Process - these are DoPkt()s. */
static IPTR nInhibitAll(struct NepMSBase *nh, struct SafeEjectNode *nodes, LONG count,
                        STRPTR busybuf, ULONG busybuflen)
{
    LONG idx;

    for(idx = 0; idx < count; idx++)
    {
        struct DeviceNode *node = nodes[idx].sen_Node;
        if(!node->dn_Task)   /* never-accessed handler: no process, nothing to flush */
        {
            continue;
        }
        DoPkt(node->dn_Task, ACTION_FLUSH, 0, 0, 0, 0, 0);
        if(DoPkt(node->dn_Task, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0) != DOSFALSE)
        {
            nodes[idx].sen_Inhibited = TRUE;
            continue;
        }
        LONG err = IoErr();
        if(err == ERROR_DEVICE_NOT_MOUNTED)   /* handler never mounted a volume */
        {
            continue;
        }
        if(err == ERROR_ACTION_NOT_KNOWN)
        {
            /* handler cannot inhibit (some CDFS-class ones); the flush above
               already ran, treating this as a veto would make such volumes
               permanently un-ejectable */
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Eject: a filesystem does not support Inhibit(), proceeding after flush.");
            continue;
        }
        /* the normal veto (ERROR_OBJECT_IN_USE): report, then release the
           handlers before this one (this one never got inhibited) */
        char busyname[64];
        nBusyName(nodes[idx].sen_LUN, node, busyname, sizeof(busyname));
        if(busybuf && busybuflen)
        {
            strncpy(busybuf, busyname, busybuflen - 1);
            busybuf[busybuflen - 1] = 0;
        }
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Eject vetoed: %s is still in use (error %ld).",
                       busyname, err);
        while(idx--)
        {
            if(nodes[idx].sen_Inhibited)
            {
                DoPkt(nodes[idx].sen_Node->dn_Task, ACTION_INHIBIT, DOSFALSE, 0, 0, 0, 0);
            }
        }
        return(SAFEEJECT_BUSY);
    }
    return(SAFEEJECT_OK);
}
/* \\\ */

/* /// "nQuiesceLUN()" */
/* Post-unmount drive hygiene on one unit: cache sync, allow removal, spindown.
   Best effort throughout - the data-safety part is already done. */
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
/* Quiesce all units of the device over an IO channel of our own: nh_IOReq and
   nIOCmdTunnel belong to the removable task's tick and must not be raced
   (precedent for foreign-context IO: AutoDetectMaxTransfer). */
static void nQuiesceDevice(struct NepMSBase *nh, struct PsdDevice *pd)
{
    struct MsgPort *mp;

    if((mp = CreateMsgPort()))
    {
        struct IOStdReq *ioreq;
        if((ioreq = (struct IOStdReq *) CreateIORequest(mp, sizeof(struct IOStdReq))))
        {
            struct NepClassMS *lun;
            for(lun = nNextLUN(nh, pd, NULL); lun; lun = nNextLUN(nh, pd, lun))
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
/* UCM_SafeEject: flush + VERIFIED unmount of every volume on all of this
   device's LUNs (all-or-nothing - one busy volume rolls everything back),
   then drive cache sync and spindown. Runs on the caller's task by design;
   the caller must be a Process (DoPkt). Taking the device off the bus
   afterwards is psdSafeEjectDevice()'s half of the job. */

IPTR nSafeEjectDevice(struct NepMSBase *nh, struct NepClassMS *ncm, STRPTR busybuf, ULONG busybuflen)
{
    struct PsdDevice *pd = ncm->ncm_Device;

    if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
    {
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                       "Safe eject needs a Process context.");
        return(SAFEEJECT_FAIL);
    }
    if(ncm->ncm_Ejected)
    {
        return(SAFEEJECT_OK);   /* idempotent: all of a device's units latch together */
    }
    if(!nOpenDOSLib(nh))
    {
        return(SAFEEJECT_FAIL);
    }

    /* Latch first: the removable task's 3s tick must not (re)mount anything
       between the collection below and the port disable at the end. Rolled
       back on any failure. */
    nSetEjectLatch(nh, pd, TRUE);

    struct SafeEjectNode nodes[NEJECT_MAXNODES];
    LONG count = nCollectEjectNodes(nh, pd, nodes, NEJECT_MAXNODES);
    IPTR res = (count < 0) ? SAFEEJECT_FAIL
                           : nInhibitAll(nh, nodes, count, busybuf, busybuflen);
    if(res != SAFEEJECT_OK)
    {
        nSetEjectLatch(nh, pd, FALSE);
        return(res);
    }

    /* Point of no return: the data is safe, so shut the handlers down and drop
       the DOS nodes, results ignored from here on. */
    LONG idx;
    for(idx = 0; idx < count; idx++)
    {
        struct DeviceNode *node = nodes[idx].sen_Node;
        if(node->dn_Task)
        {
            DoPkt(node->dn_Task, ACTION_DIE, 0, 0, 0, 0, 0);
        }
        nRemoveDosNode(nodes[idx].sen_LUN, node);
        nodes[idx].sen_LUN->ncm_HasMounted = FALSE;
    }

    nQuiesceDevice(nh, pd);

    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                   "'%s' safely ejected.", ncm->ncm_LUNIDStr);
    /* taking the device off the bus is psdSafeEjectDevice()'s job */
    return(SAFEEJECT_OK);
}
/* \\\ */

/**************************************************************************/

static const char * const MaxTransferStrings[] =
{
    " 64 KB",
    "128 KB",
    "256 KB",
    "512 KB",
    "  1 MB",
    "  2 MB",
    "  4 MB",
    "  8 MB",
    NULL
};

static const char * const MainGUIPages[] = { "Device Settings", "LUN Settings", NULL };
static const char * const MainGUIPagesDefault[] = { "Device Defaults", "LUN Defaults", NULL };

/* /// "nGetDosType()" */
ULONG nGetDosType(STRPTR tmpstr)
{
    ULONG dostype = 0;
    UBYTE ch;

    while((ch = *tmpstr++))
    {
        dostype <<= 4;
        if((ch >= '0') && (ch <= '9'))
        {
            dostype += ch - '0';
        }
        else if((ch >= 'a') && (ch <= 'f'))
        {
            dostype += ch - 'a' + 10;
        }
        else if((ch >= 'A') && (ch <= 'F'))
        {
            dostype += ch - 'A' + 10;
        }
    }
    return(dostype);
}
/* \\\ */

/* Add one row of cells to a group the object tree left empty. All or nothing:
   a half-added row would leave dangling gadget pointers behind. */
static BOOL nAddRow(Object *group, Object **cells, ULONG count)
{
    for(ULONG i = 0; i < count; i++)
    {
        if(!cells[i])
        {
            while(i--)
            {
                MUI_DisposeObject(cells[i]);
            }
            return(FALSE);
        }
    }
    for(ULONG i = 0; i < count; i++)
    {
        DoMethod(group, OM_ADDMEMBER, cells[i]);
    }
    return(TRUE);
}

/* MUI's spacer/label macros reach for IntuitionBase at file scope; like the GUI
   task below, the functions from here on carry it in the instance they are
   handed. */
#undef IntuitionBase
#define IntuitionBase ncm->ncm_IntBase

/* Fill the two filesystem groups: handler, DOS type, control string and stack
   on the device page, DOS name and buffer count on the LUN page - one row each per
   MSFsTable entry. Built by loop rather than spelled out in the object tree,
   so a new filesystem is a table entry and nothing else; MUI accepts runtime
   children through OM_ADDMEMBER as long as the group has not been set up yet
   (the same pattern as classes/hid/hidctrl.gui.c). */
static BOOL nAddFsRows(struct NepClassMS *ncm, char dostypebuf[][10])
{
    struct ClsDevCfg *cdc = ncm->ncm_CDC;
    Object *cells[8];

    cells[0] = HSpace(0);
    cells[1] = Label("DOSName");
    cells[2] = Label("Buffers");
    if(!nAddRow(ncm->ncm_FsUnitGroupObj, cells, 3))
    {
        return(FALSE);
    }

    for(ULONG fs = 0; fs < MSFS_COUNT; fs++)
    {
        const struct MSFsDesc *fsd = &MSFsTable[fs];

        cells[0] = Label(fsd->fsd_Label);
        cells[1] = PopaslObject,
            MUIA_Popstring_String, (IPTR) (ncm->ncm_FsHandlerObj[fs] = (APTR) StringObject,
                StringFrame,
                MUIA_CycleChain, 1,
                MUIA_String_AdvanceOnCR, TRUE,
                MUIA_String_Contents, (IPTR) nDevFsHandler(cdc, fs),
                MUIA_String_MaxLen, 63,
                End),
            MUIA_Popstring_Button, (IPTR) PopButton(MUII_PopFile),
            ASLFR_TitleText, (IPTR) fsd->fsd_AslTitle,
            End;
        cells[2] = Label("DosType:");
        cells[3] = ncm->ncm_FsDosTypeObj[fs] = (APTR) StringObject,
            StringFrame,
            MUIA_HorizWeight, 50,
            MUIA_CycleChain, 1,
            MUIA_String_AdvanceOnCR, TRUE,
            MUIA_String_Contents, (IPTR) dostypebuf[fs],
            MUIA_String_Accept, (IPTR) "0123456789abcdefABCDEF",
            MUIA_String_MaxLen, 9,
            End;
        cells[4] = Label("Ctrl:");
        cells[5] = ncm->ncm_FsControlObj[fs] = (APTR) StringObject,
            StringFrame,
            MUIA_HorizWeight, 50,
            MUIA_CycleChain, 1,
            MUIA_String_AdvanceOnCR, TRUE,
            MUIA_String_Contents, (IPTR) nDevFsControl(cdc, fs),
            MUIA_String_MaxLen, 63,
            End;
        /* in bytes, as a mountlist has it; for a handler DOS loads from its
           file - one that FileSystem.resource holds brings its own */
        cells[6] = Label("Stack:");
        cells[7] = ncm->ncm_FsStackObj[fs] = (APTR) StringObject,
            StringFrame,
            MUIA_HorizWeight, 25,
            MUIA_CycleChain, 1,
            MUIA_String_AdvanceOnCR, TRUE,
            MUIA_String_Integer, *nDevFsStack(cdc, fs),
            MUIA_String_Accept, (IPTR) "0123456789",
            MUIA_String_MaxLen, 8,
            End;
        if(!nAddRow(ncm->ncm_FsDevGroupObj, cells, 8))
        {
            return(FALSE);
        }

        /* the LUN page shows the defaults until a LUN is picked; ID_SELECT_LUN
           fills in the values of whichever one that is */
        cells[0] = Label(fsd->fsd_Label);
        cells[1] = ncm->ncm_FsDOSNameObj[fs] = (APTR) StringObject,
            StringFrame,
            MUIA_CycleChain, 1,
            MUIA_String_AdvanceOnCR, TRUE,
            MUIA_String_Contents, (IPTR) fsd->fsd_DefDOSName,
            MUIA_String_Reject, (IPTR) "/ :?#*",
            MUIA_String_MaxLen, 31,
            End;
        cells[2] = ncm->ncm_FsBuffersObj[fs] = (APTR) StringObject,
            StringFrame,
            MUIA_CycleChain, 1,
            MUIA_String_AdvanceOnCR, TRUE,
            MUIA_String_Integer, fsd->fsd_DefBuffers,
            MUIA_String_Accept, (IPTR) "0123456789",
            End;
        if(!nAddRow(ncm->ncm_FsUnitGroupObj, cells, 3))
        {
            return(FALSE);
        }
    }
    return(TRUE);
}

/* The LUN page shows one LUN at a time: harvest its per-filesystem gadgets
   before switching away from it (and before saving), fill them in after. */
static void nGetLunFsGadgets(struct NepClassMS *ncm, struct NepClassMS *curncm)
{
    for(ULONG fs = 0; fs < MSFS_COUNT; fs++)
    {
        struct MSFsCfg *fsc = nUnitFs(curncm->ncm_CUC, fs);
        STRPTR tmpstr = "";

        get(ncm->ncm_FsDOSNameObj[fs], MUIA_String_Contents, &tmpstr);
        strncpy(fsc->fsc_DOSName, tmpstr, sizeof(fsc->fsc_DOSName)-1);
        get(ncm->ncm_FsBuffersObj[fs], MUIA_String_Integer, &fsc->fsc_Buffers);
    }
}

static void nSetLunFsGadgets(struct NepClassMS *ncm, struct NepClassMS *curncm)
{
    for(ULONG fs = 0; fs < MSFS_COUNT; fs++)
    {
        struct MSFsCfg *fsc = nUnitFs(curncm->ncm_CUC, fs);

        set(ncm->ncm_FsDOSNameObj[fs], MUIA_String_Contents, fsc->fsc_DOSName);
        set(ncm->ncm_FsBuffersObj[fs], MUIA_String_Integer, fsc->fsc_Buffers);
    }
}

/* /// "nGUITask()" */
void nGUITask()
{

    struct Task *thistask;
    struct NepMSBase *nh;
    struct NepClassMS *ncm;
    struct NepClassMS *cncm;
    struct NepClassMS *curncm = NULL;
    APTR pic;
    char dostypebuf[MSFS_COUNT][10];
    char bar[] = "BAR,";

    thistask = FindTask(NULL);
#undef ps
#define ps ncm->ncm_PsdBase
#undef IntuitionBase
#define IntuitionBase ncm->ncm_IntBase

    ncm = thistask->tc_UserData;
    nh = ncm->ncm_ClsBase;

    ++nh->nh_Library.lib_OpenCnt;
    if(!(ncm->ncm_MUIBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN)))
    {
        KPRINTF(10, ("Couldn't open muimaster.library.\n"));
        nGUITaskCleanup(ncm);
        return;
    }

    if(!(IntuitionBase = OpenLibrary("intuition.library", 39)))
    {
        KPRINTF(10, ("Couldn't open intuition.library.\n"));
        nGUITaskCleanup(ncm);
        return;
    }
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        KPRINTF(10, ("Couldn't open neptune.library.\n"));
        nGUITaskCleanup(ncm);
        return;
    }

    ncm->ncm_LUNListDisplayHook.h_Data = NULL;
    ncm->ncm_LUNListDisplayHook.h_Entry = (APTR) LUNListDisplayHook;

    for(ULONG fs = 0; fs < MSFS_COUNT; fs++)
    {
        psdSafeRawDoFmt(dostypebuf[fs], 10, "%08lx", *nDevFsDosType(ncm->ncm_CDC, fs));
    }

    ncm->ncm_App = (APTR) ApplicationObject,
        MUIA_Application_Title      , (IPTR) libname,
        MUIA_Application_Version    , (IPTR) VERSION_STRING,
        MUIA_Application_Copyright  , (IPTR) "(C) 2002-2009 Chris Hodges",
        MUIA_Application_Author     , (IPTR) "Chris Hodges <chrisly@platon42.de>",
        MUIA_Application_Description, (IPTR)"Settings for the " CLASS_NAME,
        MUIA_Application_Base       , (IPTR) "MASSSTORAGE",
        MUIA_Application_HelpFile   , (IPTR) "HELP:Neptune.guide",
        MUIA_Application_Menustrip  , (IPTR) MenustripObject,
            Child, (IPTR) MenuObjectT((IPTR) "Project"),
                Child, (IPTR) (ncm->ncm_AboutMI = (APTR) MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR) "About...",
                    MUIA_Menuitem_Shortcut, (IPTR) "?",
                    End),
                End,
            Child, (IPTR) MenuObjectT((IPTR) "Settings"),
                Child, (IPTR) (ncm->ncm_UseMI = (APTR) MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR) "Save",
                    MUIA_Menuitem_Shortcut, (IPTR) "S",
                    End),
                Child, (IPTR) (ncm->ncm_SetDefaultMI = (APTR) MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR) "Save as Default",
                    MUIA_Menuitem_Shortcut, (IPTR) "D",
                    End),
                Child, (IPTR) MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR) NM_BARLABEL,
                    End,
                Child, (IPTR) (ncm->ncm_MUIPrefsMI = (APTR) MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR) "MUI Settings",
                    MUIA_Menuitem_Shortcut, (IPTR) "M",
                    End),
                End,
            End,

        SubWindow, (IPTR) (ncm->ncm_MainWindow = (APTR) WindowObject,
            MUIA_Window_ID   , MAKE_ID('M','A','I','N'),
            MUIA_Window_Title, (IPTR) libname,
            MUIA_HelpNode, (IPTR) libname,

            WindowContents, (IPTR) VGroup,
                Child, (IPTR) RegisterGroup(ncm->ncm_Interface ? MainGUIPages : MainGUIPagesDefault),
                    MUIA_CycleChain, 1,
                    MUIA_Register_Frame, TRUE,
                    Child, (IPTR) VGroup,
                        Child, (IPTR) VSpace(0),
                        Child, (IPTR) ColGroup(2),
                            Child, (IPTR) Label("NAK Timeout:"),
                            Child, (IPTR) (ncm->ncm_NakTimeoutObj = (APTR) SliderObject, SliderFrame,
                                MUIA_CycleChain, 1,
                                MUIA_Numeric_Min, 0,
                                MUIA_Numeric_Max, 600,
                                MUIA_Numeric_Value, ncm->ncm_CDC->cdc_NakTimeout,
                                MUIA_Numeric_Format, (IPTR) "%ld00ms",
                                End),
                            Child, (IPTR) Label("UAS queue depth:"),
                            Child, (IPTR) (ncm->ncm_UasQDObj = (APTR) SliderObject, SliderFrame,
                                MUIA_CycleChain, 1,
                                MUIA_Numeric_Min, 1,
                                MUIA_Numeric_Max, NCM_MAXTAGS,
                                MUIA_Numeric_Value, ncm->ncm_CDC->cdc_UasQueueDepth,
                                MUIA_Numeric_Format, (IPTR) "%ld",
                                End),
                            Child, (IPTR) Label("Startup delay:"),
                            Child, (IPTR) (ncm->ncm_StartupDelayObj = (APTR) SliderObject, SliderFrame,
                                MUIA_CycleChain, 1,
                                MUIA_Numeric_Min, 0,
                                MUIA_Numeric_Max, 100,
                                MUIA_Numeric_Value, ncm->ncm_CDC->cdc_StartupDelay,
                                MUIA_Numeric_Format, (IPTR) "%ld00ms",
                                End),
                            Child, (IPTR) Label("Single LUN:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_SingleLunObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_SINGLE_LUN,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) HSpace(0),
                                Child, (IPTR) Label("No Initial Reset:"),
                                Child, (IPTR) (ncm->ncm_InitialResetObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_NO_RESET,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                End,
                            Child, (IPTR) Label("Simple SCSI:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_SimpleSCSIObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_SIMPLE_SCSI,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) HSpace(0),
                                Child, (IPTR) Label("Translate CMD6->CMD10:"),
                                Child, (IPTR) (ncm->ncm_XLate610Obj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_MODE_XLATE,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                End,
                            Child, (IPTR) Label("Fake Inquiry:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_FakeInquiryObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_FAKE_INQUIRY,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) HSpace(0),
                                Child, (IPTR) Label("Better Removable Support:"),
                                Child, (IPTR) (ncm->ncm_RemSupportObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_REM_SUPPORT,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                End,
                            Child, (IPTR) Label("Trim Inquiry:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_FixInquiryObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_INQ36,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) HSpace(0),
                                Child, (IPTR) Label("Ignore broken CSS-ID:"),
                                Child, (IPTR) (ncm->ncm_CSSBrokenObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_CSS_BROKEN,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                End,
                            Child, (IPTR) Label("Fix Capacity:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_FixCapacityObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_FIX_CAPACITY,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) HSpace(0),
                                Child, (IPTR) Label("Emulate on larger block sizes:"),
                                Child, (IPTR) (ncm->ncm_EmulLargeBlkObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_EMUL_LARGE_BLK,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                End,
                            Child, (IPTR) Label("No Fallback:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_NoFallbackObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_NO_FALLBACK,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) HSpace(0),
                                Child, (IPTR) Label("Debug:"),
                                Child, (IPTR) (ncm->ncm_DebugObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, ncm->ncm_CDC->cdc_PatchFlags & PFF_DEBUG,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                End,
                            Child, (IPTR) Label("Prefer UAS:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_PreferUasObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Disabled, ncm->ncm_Interface ? TRUE : FALSE, /* global setting: edit only in class-default window */
                                    MUIA_Selected, !(ncm->ncm_CDC->cdc_PatchFlags & PFF_NO_UAS), /* checked = prefer UAS */
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) HSpace(0),
                                End,
                            Child, (IPTR) Label("Max Transfer:"),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_MaxTransferObj = (APTR) CycleObject,
                                    MUIA_Cycle_Entries, (IPTR) MaxTransferStrings,
                                    MUIA_Cycle_Active, ncm->ncm_CDC->cdc_MaxTransfer,
                                    End),
                                //Child, HSpace(0),
                                Child, (IPTR) (ncm->ncm_AutoDtxMaxTransObj = (APTR) TextObject, ButtonFrame,
                                    MUIA_Disabled, !ncm->ncm_Interface,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                    MUIA_Text_Contents, (IPTR) "\33c Auto-detect ",
                                    End),
                                End,
                            End,
                        Child, (IPTR) VSpace(0),

                        /* One row per mountable filesystem, added by nAddFsRows() once the tree exists */
                        Child, (IPTR) (ncm->ncm_FsDevGroupObj = (APTR) ColGroup(8),
                            End),
                        Child, (IPTR) VSpace(0),
                        End,
                    Child, (IPTR) VGroup,
                        Child, (IPTR) ListviewObject,
                            MUIA_CycleChain, 1,
                            MUIA_Listview_List, (IPTR) (ncm->ncm_LunLVObj = (APTR) ListObject,
                                ReadListFrame,
                                MUIA_List_Format, (IPTR) bar,
                                MUIA_List_Title, TRUE,
                                MUIA_List_DisplayHook, (IPTR) &ncm->ncm_LUNListDisplayHook,
                                End),
                            End,
                        Child, (IPTR) (ncm->ncm_LunGroupObj = (APTR) VGroup,
                            MUIA_Disabled, TRUE,
                            Child, (IPTR) VSpace(0),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_AutoMountRDBObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, TRUE,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) Label("AutoMount RDB partitions"),
                                Child, (IPTR) HSpace(0),
                                End,
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_AutoMountLegacyObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, TRUE,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) Label("AutoMount MBR/GPT partitions"),
                                Child, (IPTR) HSpace(0),
                                End,
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_MountAllLegacyObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, TRUE,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) Label("Mount all MBR/GPT partitions"),
                                Child, (IPTR) HSpace(0),
                                End,
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_AutoMountCDObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, TRUE,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) Label("AutoMount CD/DVD"),
                                Child, (IPTR) HSpace(0),
                                End,
                            Child, (IPTR) VSpace(0),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_BootObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, FALSE,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) Label("Allow booting"),
                                Child, (IPTR) HSpace(0),
                                End,
                            Child, (IPTR) HGroup,
                                Child, (IPTR) (ncm->ncm_UnmountObj = (APTR) ImageObject, ImageButtonFrame,
                                    MUIA_Background, MUII_ButtonBack,
                                    MUIA_CycleChain, 1,
                                    MUIA_InputMode, MUIV_InputMode_Toggle,
                                    MUIA_Image_Spec, MUII_CheckMark,
                                    MUIA_Image_FreeVert, TRUE,
                                    MUIA_Selected, FALSE,
                                    MUIA_ShowSelState, FALSE,
                                    End),
                                Child, (IPTR) Label("Unmount partitions after removal"),
                                Child, (IPTR) HSpace(0),
                                End,
                            Child, (IPTR) VSpace(0),
                            /* One row per mountable filesystem, added by nAddFsRows() once the tree exists */
                            Child, (IPTR) (ncm->ncm_FsUnitGroupObj = (APTR) ColGroup(3),
                                GroupFrameT("Mount name and buffers"),
                                End),
                            Child, (IPTR) VSpace(0),
                            Child, (IPTR) HGroup,
                                Child, (IPTR) Label("Default " DEVNAME " unit:"),
                                Child, (IPTR) (ncm->ncm_UnitObj = (APTR) StringObject,
                                    StringFrame,
                                    MUIA_CycleChain, 1,
                                    MUIA_String_AdvanceOnCR, TRUE,
                                    MUIA_String_Integer, 0,
                                    MUIA_String_Accept, (IPTR) "0123456789",
                                    End),
                                End,
                            End),
                        End,
                    End,
                Child, (IPTR) HGroup,
                    MUIA_Group_SameWidth, TRUE,
                    Child, (IPTR) (ncm->ncm_UseObj = (APTR) TextObject, ButtonFrame,
                        MUIA_ShowMe, (IPTR) ncm->ncm_Interface,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR) "\33c Save ",
                        End),
                    Child, (IPTR) (ncm->ncm_SetDefaultObj = (APTR) TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, ncm->ncm_Interface ? (IPTR) "\33c Save as Default " : (IPTR) "\33c Save Defaults ",
                        End),
                    Child, (IPTR) (ncm->ncm_ForgetObj = (APTR) TextObject, ButtonFrame,
                        MUIA_ShowMe, (IPTR) ncm->ncm_Interface,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR) "\33c Forget Device ",
                        End),
                    Child, (IPTR) (ncm->ncm_CloseObj = (APTR) TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR) "\33c Use ",
                        End),
                    End,
                End,
            End),
        End;

    if(!ncm->ncm_App)
    {
        KPRINTF(10, ("Couldn't create application\n"));
        nGUITaskCleanup(ncm);
        return;
    }

    if(!nAddFsRows(ncm, dostypebuf))
    {
        KPRINTF(10, ("Couldn't create filesystem rows\n"));
        nGUITaskCleanup(ncm);
        return;
    }

    if(ncm->ncm_Interface)
    {
        cncm = ncm;
        while(((struct Node *) cncm)->ln_Succ)
        {
            if(cncm->ncm_UnitLUN0 != ncm)
            {
                break;
            }
            DoMethod(ncm->ncm_LunLVObj, MUIM_List_InsertSingle, cncm, MUIV_List_Insert_Bottom);
            cncm = (struct NepClassMS *) ((struct Node *) cncm)->ln_Succ;
        }
    } else {
        DoMethod(ncm->ncm_LunLVObj, MUIM_List_InsertSingle, ncm, MUIV_List_Insert_Bottom);
    }
    DoMethod(ncm->ncm_MainWindow, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(ncm->ncm_UseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_STORE_CONFIG);
    DoMethod(ncm->ncm_SetDefaultObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_DEF_CONFIG);
    DoMethod(ncm->ncm_ForgetObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_FORGET_CONFIG);
    DoMethod(ncm->ncm_CloseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

    DoMethod(ncm->ncm_AutoDtxMaxTransObj, MUIM_Notify, MUIA_Pressed, FALSE,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_AUTODTXMAXTX);
    DoMethod(ncm->ncm_LunLVObj, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_SELECT_LUN);

    DoMethod(ncm->ncm_AboutMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_ABOUT);
    DoMethod(ncm->ncm_UseMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_STORE_CONFIG);
    DoMethod(ncm->ncm_SetDefaultMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncm->ncm_App, 2, MUIM_Application_ReturnID, ID_DEF_CONFIG);
    DoMethod(ncm->ncm_MUIPrefsMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             ncm->ncm_App, 2, MUIM_Application_OpenConfigWindow, 0);

    set(ncm->ncm_LunLVObj, MUIA_List_Active, MUIV_List_Active_Top);
    {
        IPTR  isopen = 0;
        IPTR  iconify = 0;
        ULONG sigs;
        ULONG sigmask;
        LONG retid;

        get(ncm->ncm_App, MUIA_Application_Iconified, &iconify);
        set(ncm->ncm_MainWindow, MUIA_Window_Open, TRUE);
        get(ncm->ncm_MainWindow, MUIA_Window_Open, &isopen);
        if(!(isopen || iconify))
        {
            nGUITaskCleanup(ncm);
            return;
        }
        sigmask = 0;
        do
        {
            retid = DoMethod(ncm->ncm_App, MUIM_Application_NewInput, &sigs);
            switch(retid)
            {
                case ID_DEF_CONFIG:
                case ID_STORE_CONFIG:
                case MUIV_Application_ReturnID_Quit:
                {
                    ULONG tmpflags;
                    ULONG patchflags;
                    STRPTR tmpstr;

                    get(ncm->ncm_NakTimeoutObj, MUIA_Numeric_Value, &ncm->ncm_CDC->cdc_NakTimeout);
                    get(ncm->ncm_UasQDObj, MUIA_Numeric_Value, &ncm->ncm_CDC->cdc_UasQueueDepth);
                    get(ncm->ncm_StartupDelayObj, MUIA_Numeric_Value, &ncm->ncm_CDC->cdc_StartupDelay);
                    patchflags = ncm->ncm_CDC->cdc_PatchFlags & ~(PFF_SINGLE_LUN|PFF_FAKE_INQUIRY|PFF_SIMPLE_SCSI|PFF_NO_RESET|PFF_MODE_XLATE|PFF_DEBUG|PFF_NO_FALLBACK|PFF_REM_SUPPORT|PFF_FIX_INQ36|PFF_CSS_BROKEN|PFF_FIX_CAPACITY|PFF_EMUL_LARGE_BLK|PFF_NO_UAS);
                    tmpflags = 0;
                    get(ncm->ncm_SingleLunObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_SINGLE_LUN;
                    tmpflags = 0;
                    get(ncm->ncm_FakeInquiryObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_FAKE_INQUIRY;
                    tmpflags = 0;
                    get(ncm->ncm_SimpleSCSIObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_SIMPLE_SCSI;
                    tmpflags = 0;
                    get(ncm->ncm_InitialResetObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_NO_RESET;
                    tmpflags = 0;
                    get(ncm->ncm_XLate610Obj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_MODE_XLATE;
                    tmpflags = 0;
                    get(ncm->ncm_RemSupportObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_REM_SUPPORT;
                    tmpflags = 0;
                    get(ncm->ncm_FixInquiryObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_FIX_INQ36;
                    tmpflags = 0;
                    get(ncm->ncm_CSSBrokenObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_CSS_BROKEN;
                    tmpflags = 0;
                    get(ncm->ncm_FixCapacityObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_FIX_CAPACITY;
                    tmpflags = 0;
                    get(ncm->ncm_EmulLargeBlkObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_EMUL_LARGE_BLK;
                    tmpflags = 0;
                    get(ncm->ncm_NoFallbackObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_NO_FALLBACK;
                    tmpflags = 0;
                    get(ncm->ncm_DebugObj, MUIA_Selected, &tmpflags);
                    if(tmpflags) patchflags |= PFF_DEBUG;
                    tmpflags = 0;
                    get(ncm->ncm_PreferUasObj, MUIA_Selected, &tmpflags);
                    if(!tmpflags) patchflags |= PFF_NO_UAS; /* "Prefer UAS" unchecked => disable UAS */
                    ncm->ncm_CDC->cdc_PatchFlags = patchflags;

                    get(ncm->ncm_MaxTransferObj, MUIA_Cycle_Active, &ncm->ncm_CDC->cdc_MaxTransfer);

                    for(ULONG fs = 0; fs < MSFS_COUNT; fs++)
                    {
                        tmpstr = "";
                        get(ncm->ncm_FsHandlerObj[fs], MUIA_String_Contents, &tmpstr);
                        strncpy(nDevFsHandler(ncm->ncm_CDC, fs), tmpstr, 63);
                        tmpstr = "";
                        get(ncm->ncm_FsControlObj[fs], MUIA_String_Contents, &tmpstr);
                        strncpy(nDevFsControl(ncm->ncm_CDC, fs), tmpstr, 63);
                        tmpstr = "";
                        get(ncm->ncm_FsDosTypeObj[fs], MUIA_String_Contents, &tmpstr);
                        *nDevFsDosType(ncm->ncm_CDC, fs) = nGetDosType(tmpstr);
                        IPTR stack = 0;
                        get(ncm->ncm_FsStackObj[fs], MUIA_String_Integer, &stack);
                        *nDevFsStack(ncm->ncm_CDC, fs) = (stack < MSFS_MIN_STACK) ? MSFS_MIN_STACK : stack;
                    }

                    if(ncm->ncm_Interface)
                    {
                        /* copy device config to all luns */
                        cncm = (struct NepClassMS *) ((struct Node *) ncm)->ln_Succ;
                        while(((struct Node *) cncm)->ln_Succ)
                        {
                            if(cncm->ncm_UnitLUN0 != ncm)
                            {
                                break;
                            }
                            *(cncm->ncm_CDC) = *(ncm->ncm_CDC);
                            cncm = (struct NepClassMS *) ((struct Node *) cncm)->ln_Succ;
                        }
                    }
                    if(curncm)
                    {
                        get(ncm->ncm_AutoMountLegacyObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoMountLegacy);
                        get(ncm->ncm_MountAllLegacyObj, MUIA_Selected, &curncm->ncm_CUC->cuc_MountAllLegacy);
                        get(ncm->ncm_AutoMountCDObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoMountCD);
                        nGetLunFsGadgets(ncm, curncm);
                        get(ncm->ncm_AutoMountRDBObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoMountRDB);
                        get(ncm->ncm_BootObj, MUIA_Selected, &curncm->ncm_CUC->cuc_Boot);
                        get(ncm->ncm_UnitObj, MUIA_String_Integer, &curncm->ncm_CUC->cuc_DefaultUnit);
                        get(ncm->ncm_UnmountObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoUnmount);
                    }

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
                            psdAddCfgEntry(pic, ncm->ncm_CDC);
                            psdAddCfgEntry(pic, ncm->ncm_CUC);
                            psdSaveCfgToDisk(NULL);
                        }
                    }
                    if(nStoreConfig(ps, ncm))
                    {
                        if(retid != MUIV_Application_ReturnID_Quit)
                        {
                            psdSaveCfgToDisk(NULL);
                        }
                        retid = MUIV_Application_ReturnID_Quit;
                    }
                    break;
                }

                case ID_SELECT_LUN:
                {
                    DoMethod(ncm->ncm_LunLVObj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &cncm);
                    if(curncm != cncm)
                    {
                        if(curncm)
                        {
                            get(ncm->ncm_AutoMountLegacyObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoMountLegacy);
                            get(ncm->ncm_MountAllLegacyObj, MUIA_Selected, &curncm->ncm_CUC->cuc_MountAllLegacy);
                            get(ncm->ncm_AutoMountCDObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoMountCD);
                            nGetLunFsGadgets(ncm, curncm);
                            get(ncm->ncm_AutoMountRDBObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoMountRDB);
                            get(ncm->ncm_BootObj, MUIA_Selected, &curncm->ncm_CUC->cuc_Boot);
                            get(ncm->ncm_UnitObj, MUIA_String_Integer, &curncm->ncm_CUC->cuc_DefaultUnit);
                            get(ncm->ncm_UnmountObj, MUIA_Selected, &curncm->ncm_CUC->cuc_AutoUnmount);
                        }
                    }
                    if((curncm = cncm))
                    {
                        set(ncm->ncm_AutoMountLegacyObj, MUIA_Selected, curncm->ncm_CUC->cuc_AutoMountLegacy);
                        set(ncm->ncm_MountAllLegacyObj, MUIA_Selected, curncm->ncm_CUC->cuc_MountAllLegacy);
                        set(ncm->ncm_AutoMountCDObj, MUIA_Selected, curncm->ncm_CUC->cuc_AutoMountCD);
                        nSetLunFsGadgets(ncm, curncm);
                        set(ncm->ncm_AutoMountRDBObj, MUIA_Selected, curncm->ncm_CUC->cuc_AutoMountRDB);
                        set(ncm->ncm_BootObj, MUIA_Selected, curncm->ncm_CUC->cuc_Boot);
                        set(ncm->ncm_UnitObj, MUIA_String_Integer, curncm->ncm_CUC->cuc_DefaultUnit);
                        set(ncm->ncm_UnmountObj, MUIA_Selected, curncm->ncm_CUC->cuc_AutoUnmount);
                        set(ncm->ncm_LunGroupObj, MUIA_Disabled, FALSE);
                    } else {
                        set(ncm->ncm_LunGroupObj, MUIA_Disabled, TRUE);
                    }
                    break;
                }
                case ID_AUTODTXMAXTX:
                {
                    DoMethod(ncm->ncm_LunLVObj, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &cncm);
                    set(ncm->ncm_App, MUIA_Application_Sleep, TRUE);
                    AutoDetectMaxTransfer(cncm);
                    set(ncm->ncm_App, MUIA_Application_Sleep, FALSE);
                    set(ncm->ncm_MaxTransferObj, MUIA_Cycle_Active, ncm->ncm_CDC->cdc_MaxTransfer);
                    break;
                }

                case ID_ABOUT:
                    NEP_CLASS_ABOUT_EXTRA(ncm->ncm_App, ncm->ncm_MainWindow,
                                          "\n\nAutomounting by a fork of the a4091.device mounter,\n(c) Toni Wilen and contributors.\nSee LEGAL for full attribution.");
                    break;

                case ID_FORGET_CONFIG:
                    /* Drop this class's record for the device so the class
                       defaults govern it again. Only our own payload goes:
                       NeptuneUSB's data for the device (custom name, forced
                       binding) stays. The live units follow through
                       ConfigChangedEvent -> nReapplyConfig(), which runs once
                       this window is gone (it skips units with a window open). */
                    if(MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "Forget|Cancel",
                                    "Forget the saved settings of this device?\n"
                                    "The class defaults will apply to it again.", NULL))
                    {
                        psdSetUsbDevCfg(libname, ncm->ncm_DevIDString, ncm->ncm_IfIDString, NULL);
                        psdSaveCfgToDisk(NULL);
                        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                       "Forgot the saved settings of %s.", ncm->ncm_DevIDString);
                        retid = MUIV_Application_ReturnID_Quit;
                    }
                    break;
            }
            if(retid == MUIV_Application_ReturnID_Quit)
            {
                break;
            }
            if(sigs)
            {
                sigs = Wait(sigs | sigmask | SIGBREAKF_CTRL_C);
                if(sigs & SIGBREAKF_CTRL_C)
                {
                    break;
                }
            }
        } while(TRUE);
        set(ncm->ncm_MainWindow, MUIA_Window_Open, FALSE);
    }
    nGUITaskCleanup(ncm);

}
/* \\\ */

/* /// "AutoDetectMaxTransfer()" */
void AutoDetectMaxTransfer(struct NepClassMS *cncm)
{
    LONG res;
    UBYTE *memory;
    UBYTE *orgbuffer;
    UBYTE *cmpbuffer;
    ULONG *lbufptr;
    struct MsgPort *mp;
    struct IOStdReq *ioreq;
    struct NepClassMS *ncm = cncm->ncm_UnitLUN0;
    ULONG numblocks;
    /* Two halves of memsize/2 each: a block-by-block reference read and a
       single maxtrans-sized chunk read to compare. Each half must be >= the
       largest MaxTransfer tried, so 16 MB covers up to the 8 MB setting. */
    ULONG memsize = 16<<20;
    ULONG block;
    ULONG maxtrans;
    LONG ioerr;
    BOOL bail = FALSE;
    ULONG cnt;

    res = MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "Continue|Cancel",
        "Auto-detection of the maximum transfer rate\n"
        "will need some media inserted in the selected\n"
        "LUN of the drive. Moreover, the contents of the\n"
        "media may not be empty.\n"
        "The test will need about 16 MB of temporary memory!\n"
        "No data is written to the disk!", NULL);
    if(!res)
    {
        return;
    }
    if(!cncm->ncm_UnitReady)
    {
        MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "No disk/media in drive!", NULL);
        return;
    }
    memory = (UBYTE *) AllocVec(memsize, MEMF_CLEAR);
    if(!memory)
    {
        MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "Sorry, out of memory!", NULL);
        return;
    }
    mp = CreateMsgPort();
    if(mp)
    {
        ioreq = (struct IOStdReq *) CreateIORequest(mp, sizeof(struct IOStdReq));
        if(ioreq)
        {
            if(!OpenDevice(DEVNAME, cncm->ncm_UnitNo, (struct IORequest *) ioreq, 0))
            {
                ncm->ncm_CDC->cdc_MaxTransfer = 0;
                if(!cncm->ncm_BlockSize)
                {
                    ioreq->io_Command = TD_GETGEOMETRY;
                    ioreq->io_Data = &cncm->ncm_Geometry;
                    ioreq->io_Length = sizeof(cncm->ncm_Geometry);
                    DoIO((struct IORequest *) ioreq);
                }

                numblocks = (memsize>>1) / cncm->ncm_BlockSize;
                orgbuffer = memory;
                cmpbuffer = &memory[memsize>>1];

                do
                {
                    for(block = 0; block < numblocks; block++)
                    {
                        ioreq->io_Command = TD_READ64;
                        ioreq->io_Actual = 0;
                        ioreq->io_Offset = block*cncm->ncm_BlockSize;
                        ioreq->io_Length = cncm->ncm_BlockSize;
                        ioreq->io_Data = &orgbuffer[ioreq->io_Offset];
                        ioerr = DoIO((struct IORequest *) ioreq);
                        if(ioerr)
                        {
                            MUI_Request(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "Error %ld while reading block %ld!\nAborting process...", ioerr, block);
                            bail = TRUE;
                            break;
                        }
                    }
                    if(bail) break;

                    do
                    {
                        maxtrans = (1UL<<(ncm->ncm_CDC->cdc_MaxTransfer+16));
                        // do a quick check on block contents
                        numblocks = (memsize>>1) / maxtrans;
                        if(numblocks)
                        {
                            for(block = 0; block < numblocks-1; block++)
                            {
                                if(!memcmp(&orgbuffer[block * maxtrans], &orgbuffer[(block + 1) * maxtrans], (size_t) maxtrans))
                                {
                                    res = MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "Continue|Abort", "Medium contents are too uniform\nfor a reliable test.\nResults might be wrong.", NULL);
                                    if(!res)
                                    {
                                        bail = TRUE;
                                    }
                                    break;
                                }
                            }
                        }
                        if(bail) break;

                        // fill compare buffer with garbage
                        cnt = (memsize>>3);
                        lbufptr = (ULONG *) cmpbuffer;
                        do
                        {
                            *lbufptr = (IPTR) lbufptr;
                            lbufptr++;
                        }
                        while(--cnt);

                        // start reading the chunks
                        numblocks = (memsize>>1) / maxtrans;
                        for(block = 0; block < numblocks; block++)
                        {
                            ioreq->io_Command = TD_READ64;
                            ioreq->io_Actual = 0;
                            ioreq->io_Offset = block*maxtrans;
                            ioreq->io_Length = maxtrans;
                            ioreq->io_Data = &cmpbuffer[ioreq->io_Offset];
                            ioerr = DoIO((struct IORequest *) ioreq);
                            if(ioerr)
                            {
                                MUI_Request(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "Error %ld while reading at offset %ld!\nAborting process...", ioerr, &ioreq->io_Offset);
                                bail = TRUE;
                                break;
                            }
                            if(memcmp(&orgbuffer[ioreq->io_Offset], &cmpbuffer[ioreq->io_Offset], (size_t) maxtrans))
                            {
                                MUI_Request(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "Reading failed at MaxTrans = %08lx", maxtrans-1);
                                if(ncm->ncm_CDC->cdc_MaxTransfer)
                                {
                                    ncm->ncm_CDC->cdc_MaxTransfer--;
                                }
                                bail = TRUE;
                                break;
                            }
                        }
                        if(bail) break;
                        //MUI_Request(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "Test with %ld (%ld) succeeded!", maxtrans, numblocks);
                        if(ncm->ncm_CDC->cdc_MaxTransfer < 7)
                        {
                            ncm->ncm_CDC->cdc_MaxTransfer++;
                        } else {
                            break;
                        }
                    } while(TRUE);
                    if(!bail)
                    {
                        MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "Test succeeded, setting MaxTrans to maximum value!", NULL);
                    }
                } while(FALSE);
                CloseDevice((struct IORequest *) ioreq);
            } else {
                MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "Couldn't open device!", NULL);
            }
            DeleteIORequest((struct IORequest *) ioreq);
        } else {
            MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "No IOReq!", NULL);
        }
        DeleteMsgPort(mp);
    } else {
        MUI_RequestA(ncm->ncm_App, ncm->ncm_MainWindow, 0, NULL, "OK", "No MsgPort!", NULL);
    }
    FreeVec(memory);
}
/* \\\ */

/* /// "nGUITaskCleanup()" */
void nGUITaskCleanup(struct NepClassMS *ncm)
{
    if(ncm->ncm_App)
    {
        MUI_DisposeObject(ncm->ncm_App);
        ncm->ncm_App = NULL;
    }
    if(ncm->ncm_MUIBase)
    {
        CloseLibrary(ncm->ncm_MUIBase);
        ncm->ncm_MUIBase = NULL;
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
    ncm->ncm_GUIBinding = NULL;
    ncm->ncm_GUITask = NULL;
    --ncm->ncm_ClsBase->nh_Library.lib_OpenCnt;
}
/* \\\ */

/* /// "LUNListDisplayHook()" */
LONG LUNListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), struct NepClassMS * ncm asm("a1"))
{

    //struct NepMSBase *nh = ncm->ncm_ClsBase;
    if(ncm)
    {
        *strarr++ = ncm->ncm_LUNNumStr;
        *strarr = ncm->ncm_LUNIDStr;
    } else {
        *strarr++ = "\33l\33uLUN";
        *strarr = "\33l\33uID";
    }
    return(0);

}
/* \\\ */
