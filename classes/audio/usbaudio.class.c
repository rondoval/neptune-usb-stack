/*
 *----------------------------------------------------------------------------
 *                         usbaudio class for Neptune
 *----------------------------------------------------------------------------
 * Based on the Poseidon usbaudio class by Chris Hodges <chrisly@platon42.de>.
 */

#include "debug.h"

#include "numtostr.h"
#include "usbaudio.class.h"

#include <proto/ahi.h>
#include <proto/timer.h>

#include <stdlib.h>

#define SUBLIBNAME          "usbaudio.audio"

/* /// "Lib Stuff" */
static const STRPTR libname = CLASS_NAME;

static
const APTR SubLibFuncTable[] =
{
    (APTR) subLibOpen,
    (APTR) subLibClose,
    (APTR) subLibExpunge,
    (APTR) subLibReserved,
    (APTR) subLibAllocAudio,
    (APTR) subLibFreeAudio,
    (APTR) subLibDisable,
    (APTR) subLibEnable,
    (APTR) subLibStart,
    (APTR) subLibUpdate,
    (APTR) subLibStop,
    (APTR) subLibSetVol,
    (APTR) subLibSetFreq,
    (APTR) subLibSetSound,
    (APTR) subLibSetEffect,
    (APTR) subLibLoadSound,
    (APTR) subLibUnloadSound,
    (APTR) subLibGetAttr,
    (APTR) subLibHardwareControl,
    (APTR) -1,
};

static int compare_frequencies(const void *left, const void *right)
{
    ULONG left_value = *(const ULONG *)left;
    ULONG right_value = *(const ULONG *)right;

    if (left_value < right_value)
        return -1;
    if (left_value > right_value)
        return 1;
    return 0;
}

int libInit(struct NepAudioBase * nh)
{
    struct NepAudioBase *ret = NULL;

    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", nh, EXEC_BASE_NAME));

    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);

#define	UtilityBase	nh->nh_UtilityBase

    if(UtilityBase)
    {
        /* Create default config */
        nh->nh_CurrentCGC.cgc_ChunkID = AROS_LONG2BE(MAKE_ID('U','A','U','D'));
        nh->nh_CurrentCGC.cgc_Length = AROS_LONG2BE(sizeof(struct ClsGlobalCfg)-8);

        NewList(&nh->nh_Units);

        KPRINTF(1, ("Before MakeLibrary\n"));
        if((nh->nh_SubLibBase = (struct NepAudioSubLibBase *) MakeLibrary((APTR) SubLibFuncTable,
                                                                         NULL, 
                                                                         (APTR) subLibInit,
                                                                         sizeof(struct NepAudioSubLibBase), NULL)))
        {
            KPRINTF(1, ("After MakeLibrary\n"));

            nh->nh_SubLibBase->nas_ClsBase = nh;
            Forbid();
            AddLibrary((struct Library *) nh->nh_SubLibBase);
            nh->nh_SubLibBase->nas_Library.lib_OpenCnt++;
            Permit();
            ret = nh;
        } else {
            KPRINTF(20, ("failed to create usbaudio.audio\n"));
        }
    } else {
        KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
    }

    KPRINTF(10, ("libInit: Ok\n"));
    return(ret ? TRUE : FALSE);
}

int libOpen(struct NepAudioBase * nh)
{
    KPRINTF(10, ("libOpen nh: 0x%08lx\n", nh));
    nLoadClassConfig(nh);
    return(TRUE);
}

/* /// "nFreeBinding()" */
/* A binding's memory: its modes, then itself. Unlinked, no task, no mode
   open. */
static void nFreeBinding(struct NepClassAudio *nch)
{
    struct NepAudioMode *nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;

    while(nam->nam_Node.ln_Succ)
    {
        struct NepAudioMode *next = (struct NepAudioMode *) nam->nam_Node.ln_Succ;

        FreeVec(nam);
        nam = next;
    }
    FreeVec(nch);
}
/* \\\ */

/* Only with AHI gone (the sub-driver's own open count left): a binding still
   listed here was kept by a release that found a mode open (usbReleaseInterfaceBinding) */
int libExpunge(struct NepAudioBase * nh)
{
    struct NepClassAudio *nch;

    KPRINTF(10, ("libExpunge nh: 0x%08lx\n", nh));

    if(nh->nh_SubLibBase->nas_Library.lib_OpenCnt == 1)
    {
        KPRINTF(1, ("libExpunge: closelibrary utilitybase 0x%08lx\n",
                    UtilityBase));
        CloseLibrary((struct Library *) UtilityBase);

        nch = (struct NepClassAudio *) nh->nh_Units.lh_Head;
        while(nch->nch_Node.ln_Succ)
        {
            Remove((struct Node *) nch);
            nFreeBinding(nch);
            nch = (struct NepClassAudio *) nh->nh_Units.lh_Head;
        }

        nh->nh_SubLibBase->nas_Library.lib_OpenCnt--;
        RemLibrary((struct Library *) nh->nh_SubLibBase);
        KPRINTF(5, ("libExpunge: Unloading done! usbaudio.class expunged!\n\n"));
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

#undef UtilityBase
#define	UtilityBase	nh->nh_UtilityBase

/* /// "nVerifyAudioStreamClass()" */
BOOL nVerifyAudioStreamDevice(struct Library *ps, struct PsdInterface *pif)
{
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    struct PsdInterface *subpif;
    struct UsbAudioHeaderDesc10 *header = NULL;
    IPTR ifclass;
    IPTR subclass;
    IPTR proto;
    UWORD bcdvers;
    UWORD cnt;
    APTR desc;

    psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Config, &pc,
                    IFA_Class, &ifclass,
                    IFA_SubClass, &subclass,
                    IFA_Protocol, &proto,
                    TAG_DONE);

    psdGetAttrs(PGA_CONFIG, pc, CA_Device, &pd, TAG_END);

    if((ifclass == AUDIO_CLASSCODE) && (subclass == AUDIO_CTRL_SUBCLASS))
    {
        desc = psdFindDescriptor(pd, NULL,
                                 DDA_Interface, pif,
                                 DDA_DescriptorType, UDT_CS_INTERFACE,
                                 DDA_CS_SubType, UDST_AUDIO_CTRL_HEADER,
                                 TAG_END);

        if(desc)
        {
            psdGetAttrs(PGA_DESCRIPTOR, desc, DDA_DescriptorData, &header, TAG_END);
            bcdvers = (header->bcdADC0|(header->bcdADC1<<8));
            if(bcdvers == 0x0100)
            {
                for(cnt = 0; cnt < header->bInCollection; cnt++)
                {
                    subpif = psdFindInterface(pd, NULL,
                                              IFA_InterfaceNum, header->baInterfaceNr[cnt],
                                              IFA_Class, AUDIO_CLASSCODE,
                                              IFA_SubClass, AUDIO_STREAM_SUBCLASS,
                                              IFA_Protocol, proto,
                                              TAG_END);
                    if(subpif)
                    {
                        return(TRUE);
                    }
                }
            } else {
                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Unsupported audio class spec version %lx.%2lx", bcdvers>>8, bcdvers & 0xff);
            }
        }
    }
    return(FALSE);
}
/* \\\ */

/* /// "usbAttemptInterfaceBinding()" */
struct NepClassAudio * usbAttemptInterfaceBinding(struct NepAudioBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    BOOL isaudio;

    KPRINTF(1, ("nepAudioAttemptInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        isaudio = nVerifyAudioStreamDevice(ps, pif);
        CloseLibrary(ps);
        if(isaudio)
        {
            return(usbForceInterfaceBinding(nh, pif));
        }
    }
    return(NULL);
}
/* \\\ */

/* /// "usbForceInterfaceBinding()" */
struct NepClassAudio * usbForceInterfaceBinding(struct NepAudioBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    struct NepClassAudio *nch;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    STRPTR devname;
    ULONG unitno;
    UBYTE buf[64];
    struct Task *tmptask;

    KPRINTF(1, ("nepAudioAttemptInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Config, &pc,
                    TAG_DONE);
        psdGetAttrs(PGA_CONFIG, pc,
                    CA_Device, &pd,
                    TAG_END);
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_ProductName, &devname,
                    TAG_END);
        /* AllocVec: a binding a release keeps is freed by libExpunge,
           outside the scope of neptune.library */
        if(!(nch = AllocVec(sizeof(struct NepClassAudio), MEMF_PUBLIC|MEMF_CLEAR)))
        {
            CloseLibrary(ps);
            return(NULL);
        }
        nch->nch_ClsBase = nh;
        nch->nch_Device = NULL;
        nch->nch_Interface = pif;
        NewList(&nch->nch_AudioModes);
        NewList(&nch->nch_AudioUnits);
        /* the lowest unit number not taken: a replugged device gets its old
           one back, and with it its AHI mode IDs */
        Forbid();
        unitno = 0;
        struct NepClassAudio *other = (struct NepClassAudio *) nh->nh_Units.lh_Head;
        while(other->nch_Node.ln_Succ)
        {
            if(other->nch_UnitNo == unitno)
            {
                unitno++;
                other = (struct NepClassAudio *) nh->nh_Units.lh_Head;
            } else {
                other = (struct NepClassAudio *) other->nch_Node.ln_Succ;
            }
        }
        nch->nch_UnitNo = unitno;
        AddTail(&nh->nh_Units, &nch->nch_Node);
        Permit();
        nch->nch_DenyRequests = TRUE;
 
        nLoadClassConfig(nh);

        psdSafeRawDoFmt(buf, 64, "usbaudio.class<%08lx>", nch);
        nch->nch_ReadySignal = SIGB_SINGLE;
        nch->nch_ReadySigTask = FindTask(NULL);
        SetSignal(0, SIGF_SINGLE);
        if((tmptask = psdSpawnSubTask(buf, nAudioTask, nch)))
        {
            psdBorrowLocksWait(tmptask, 1UL<<nch->nch_ReadySignal);
            if(nch->nch_Task)
            {
                nch->nch_ReadySigTask = NULL;
                //FreeSignal(nch->nch_ReadySignal);

                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               NEP_BOUND1_TXT,
                               devname);
                CloseLibrary(ps);
                return(nch);
            }
        }
        nch->nch_ReadySigTask = NULL;
        //FreeSignal(nch->nch_ReadySignal);
        /* the task never got going: no AHI mode was added */
        Forbid();
        Remove(&nch->nch_Node);
        Permit();
        nFreeBinding(nch);
        CloseLibrary(ps);
    }
    return(NULL);
}
/* \\\ */

/* /// "usbReleaseInterfaceBinding()" */
void usbReleaseInterfaceBinding(struct NepAudioBase *nh, struct NepClassAudio *nch)
{
    struct Library *ps;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    STRPTR devname;

    KPRINTF(1, ("nepAudioReleaseInterfaceBinding(%08lx)\n", nch));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        Forbid();
        nch->nch_DenyRequests = TRUE;
        nch->nch_ReadySignal = SIGB_SINGLE;
        nch->nch_ReadySigTask = FindTask(NULL);
        if(nch->nch_Task)
        {
            Signal(nch->nch_Task, SIGBREAKF_CTRL_C);
        }
        Permit();
        while(nch->nch_Task)
        {
            Wait(1L<<nch->nch_ReadySignal);
        }
        //FreeSignal(nch->nch_ReadySignal);
        psdGetAttrs(PGA_INTERFACE, nch->nch_Interface, IFA_Config, &pc, TAG_END);
        psdGetAttrs(PGA_CONFIG, pc, CA_Device, &pd, TAG_END);
        psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       NEP_RELEASED_TXT,
                       devname);
        /* The task left with no mode open and took the modes out of AHI.
           A mode AHI opened in between keeps the binding listed, for
           libExpunge. */
        Forbid();
        if(!nch->nch_CurrentMode)
        {
            Remove(&nch->nch_Node);
            Permit();
            nFreeBinding(nch);
        } else {
            Permit();
        }
        CloseLibrary(ps);
    }
}
/* \\\ */

/* /// "usbGetAttrsA()" */
LONG (usbGetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepAudioBase * nh asm("a6"))
{

    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("nepAudioGetAttrsA(%ld, %08lx, %08lx)\n", type, usbstruct, tags));
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
                 *((STRPTR *) ti->ti_Data) = "USB Audio Streaming Interface class";
                 count++;
             }
             if((ti = FindTagItem(UCCA_HasClassCfgGUI, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
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
                 *((IPTR *) ti->ti_Data) = nh->nh_UsingDefaultCfg;
                 count++;
             }
             if((ti = FindTagItem(UCCA_SupportsSuspend, tags)))
             {
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             break;

         case UGA_BINDING:
             if((ti = FindTagItem(UCCA_UsingDefaultCfg, tags)))
             {
                 *((IPTR *) ti->ti_Data) = FALSE;
                 count++;
             }
             break;
    }
    return(count);
}
/* \\\ */

/* /// "usbSetAttrsA()" */
LONG (usbSetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepAudioBase * nh asm("a6"))
{
    return(0);
}
/* \\\ */

/* /// "usbDoMethodA()" */
IPTR (usbDoMethodA)(ULONG methodid asm("d0"), IPTR * methoddata asm("a1"), struct NepAudioBase * nh asm("a6"))
{

    struct NepClassAudio *nch;
    KPRINTF(10, ("Do Method %ld\n", methodid));
    switch(methodid)
    {
        case UCM_AttemptInterfaceBinding:
            return((IPTR) usbAttemptInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ForceInterfaceBinding:
            return((IPTR) usbForceInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ReleaseInterfaceBinding:
            usbReleaseInterfaceBinding(nh, (struct NepClassAudio *) methoddata[0]);
            return(TRUE);

        case UCM_OpenCfgWindow:
            return(nOpenCfgWindow(nh));

        case UCM_ConfigChangedEvent:
            nLoadClassConfig(nh);
            return(TRUE);

        case UCM_AttemptSuspendDevice:
            nch = (struct NepClassAudio *) methoddata[0];
            return(nch->nch_CurrentMode ? FALSE : TRUE);

        case UCM_AttemptResumeDevice:
            nch = (struct NepClassAudio *) methoddata[0];
            Signal(nch->nch_Task, (1L<<nch->nch_TaskMsgPort->mp_SigBit));
            return(TRUE);

        default:
            break;
    }
    return(0);
}
/* \\\ */

/**************************************************************************/

#undef  ps
#define ps nch->nch_Base
#undef AHIBase
#define AHIBase nch->nch_AHIBase

static const ULONG commonFreqs[] =
{
     4410,  4800,  5513,  6000,  7350,  8000,  9600,
    11025, 12000, 14700, 16000, 17640, 18900, 19200,
    22050, 24000, 29400, 32000, 33075, 37800, 44100,
    48000, 52000, 56000, 60000, 64000,
    // AHI doesn't support freqs > 65535 Hz
    /*72000, 80000, 88200, 96000, */
    0
};

/* A feature unit's Volume Control works in 1/256 dB, 0x8001 (-127.9961 dB)
   to 0x7FFF (+127.9961 dB) (UAC 1.0, 5.2.2.4.3.2); AHI's gains and volumes
   are linear factors (Fixed). Integer conversions, the stack has no floating
   point: */
#define DB256_PER_LOG2         1541   /* 20 * log10(2) dB = one doubling, in 1/256 dB */
#define LOG2_Q16_PER_DB256_Q10 43541  /* 1/256 dB in 16.16 doublings, times 1024 */
#define VOLUME_DB256_MIN       (-32767) /* 0x8001, the lowest setting a Volume Control has */

/* 2^(i/32) in 16.16, i = 0..32: the mantissa of a power of two, 1.0 to 2.0,
   in 32 steps; values between two entries are interpolated linearly */
static const ULONG Pow2Table[33] =
{
     65536,  66971,  68438,  69936,  71468,  73032,  74632,  76266,
     77936,  79642,  81386,  83169,  84990,  86851,  88752,  90696,
     92682,  94711,  96785,  98905, 101070, 103283, 105545, 107856,
    110218, 112631, 115098, 117618, 120194, 122825, 125515, 128263,
    131072
};
#define POW2_STEP_SHIFT 11      /* a 16.16 fraction >> 11 = its table step (65536 / 32) */
#define POW2_STEP_MASK  0x7ff   /* the part of a 16.16 fraction within its step */

/* /// "nDbToFixed()" */
/* A level in 1/256 dB as a linear factor, 10^(dB/20); Fixed holds -96 to
   +90 dB, a level beyond that is cut to it */
static Fixed nDbToFixed(LONG db256)
{
    /* dB to doublings (log2), 16.16; |db256| <= 32767 keeps the product in 32 bits */
    LONG log2q16 = (db256 * LOG2_Q16_PER_DB256_Q10) >> 10;
    /* whole doublings rounded down - also below zero - and a fraction 0..1 */
    LONG whole = log2q16 >> 16;
    ULONG frac = log2q16 & 0xffff;
    /* 2^frac from the table: its step, and how far into the step */
    ULONG idx = frac >> POW2_STEP_SHIFT;
    ULONG rem = frac & POW2_STEP_MASK;
    ULONG mant = Pow2Table[idx] + (((Pow2Table[idx + 1] - Pow2Table[idx]) * rem) >> POW2_STEP_SHIFT);

    /* times 2^whole; mant < 2.0, so << 15 would pass 2^31 */
    if(whole >= 15)
    {
        return 0x7fffffff;
    }
    if(whole >= 0)
    {
        return mant << whole;
    }
    /* below 2^-17 nothing of mant is left: the smallest factor, never 0 */
    if(whole < -17)
    {
        return 1;
    }
    mant >>= -whole;
    return mant ? mant : 1;
}
/* \\\ */

/* /// "nFixedToDb()" */
/* A linear factor as a level in 1/256 dB, 20 * log10(factor); 0 or less is
   the lowest level a Volume Control has */
static LONG nFixedToDb(Fixed gain)
{
    ULONG g = gain;
    ULONG idx;

    if(gain <= 0)
    {
        return VOLUME_DB256_MIN;
    }
    /* the highest set bit (bfffo from the 68020 on; g > 0 here): 16 is 1.0,
       so bit - 16 whole doublings */
    LONG bit = 31 - __builtin_clzl(g);
    /* the mantissa, scaled to 1.0 .. 2.0 in 16.16 */
    ULONG mant = (bit >= 16) ? (g >> (bit - 16)) : (g << (16 - bit));
    /* its table step, and the fraction of a doubling interpolated within it */
    for(idx = 0; mant >= Pow2Table[idx + 1]; idx++);

    ULONG frac = (idx << POW2_STEP_SHIFT) +
                 (((mant - Pow2Table[idx]) << POW2_STEP_SHIFT) / (Pow2Table[idx + 1] - Pow2Table[idx]));
    /* doublings in 16.16 (a multiplication: bit - 16 may be negative) */
    LONG log2q16 = (bit - 16) * 65536 + (LONG) frac;

    /* doublings to 1/256 dB; |log2q16| <= 16 doublings keeps the product in 32 bits */
    return (log2q16 * DB256_PER_LOG2) >> 16;
}
/* \\\ */

/* /// "nRecMode()" */
/* The recording half of an AHI mode, if it has one. A playback stream brings
   the recording stream of its shape along (nam_RecStream). A recording stream
   that no playback stream matches - a camera's microphone - is an AHI mode by
   itself: it records, the timer keeps AHI's player and mixer going, and what
   is mixed goes nowhere. */
static struct NepAudioMode * nRecMode(struct NepAudioMode *nam)
{
    return nam->nam_IsInput ? nam : nam->nam_RecStream;
}
/* \\\ */

/* /// "nAltForFreq()" */
/* The alternate setting of a mode that carries the rate. Only a mode that
   records and nothing else has more than one: a camera's microphone has an
   alternate setting per sample rate, and they are one AHI mode with all the
   rates (nExamineAudioUnits). */
static struct NepAudioMode * nAltForFreq(struct NepAudioMode *nam, ULONG freq)
{
    if(!nam->nam_IsInput)
    {
        return nam;   /* only a record-only mode has alternates */
    }
    for(struct NepAudioMode *alt = (struct NepAudioMode *) nam->nam_Unit->nch_AudioModes.lh_Head;
        alt->nam_Node.ln_Succ;
        alt = (struct NepAudioMode *) alt->nam_Node.ln_Succ)
    {
        if(alt->nam_PartOf != nam)
        {
            continue;
        }
        for(ULONG cnt = 0; cnt < alt->nam_NumFrequencies; cnt++)
        {
            if(alt->nam_FreqArray[cnt] == freq)
            {
                return alt;
            }
        }
    }
    return nam;   /* the rate is the mode's own */
}
/* \\\ */

/* /// "nGetOutputUnit()" */
/* The unit that holds the playback controls of a mode, if it plays */
static struct NepAudioUnit * nGetOutputUnit(struct NepAudioMode *nam)
{
    return nam->nam_IsInput ? NULL : nam->nam_RootUnit;
}
/* \\\ */

/* /// "nOutVolumeUnit()" */
/* The Feature Unit whose Volume Control range AHI is told for a mode's
   output volume: the one upstream of every output, else the first output's
   own; NULL if the mode has none (nMapOutputs) */
static struct NepAudioUnit * nOutVolumeUnit(struct NepAudioMode *nam)
{
    if(nam->nam_OutVolume)
    {
        return nam->nam_OutVolume;
    }
    for(ULONG cnt = 0; cnt < nam->nam_NumOutputs; cnt++)
    {
        if(nam->nam_Outputs[cnt].nao_Volume)
        {
            return nam->nam_Outputs[cnt].nao_Volume;
        }
    }
    return NULL;
}
/* \\\ */

/* /// "nModeTerminal()" */
/* TRUE if nau is a terminal of type (input or output terminal), not a USB
   streaming one, that a mode's stream reaches: an output terminal the
   playback stream flows to (nau_SourceUnit), an input terminal that flows to
   the recording stream (nau_SinkUnit). With all, any such terminal - the
   fallback when the tagging found none. */
static BOOL nModeTerminal(struct NepAudioMode *nam, struct NepAudioUnit *nau, UWORD type, BOOL all)
{
    if((nau->nau_Type != type) || (nau->nau_TermType == UAUTT_STREAMING))
    {
        return FALSE;
    }
    if(all)
    {
        return TRUE;
    }
    struct NepAudioUnit *stream = (type == UDST_AUDIO_CTRL_OUTPUT_TERMINAL) ? nau->nau_SourceUnit : nau->nau_SinkUnit;
    return nam->nam_RootUnit && (stream == nam->nam_RootUnit);
}
/* \\\ */

/* /// "nModeInput()" */
/* The num-th input terminal a recording mode's stream reaches (nModeTerminal),
   in the order of nInputNames; NULL if there are fewer */
static struct NepAudioUnit * nModeInput(struct NepClassAudio *nch, struct NepAudioMode *nam, ULONG num, BOOL all)
{
    for(struct NepAudioUnit *nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
        nau->nau_Node.ln_Succ;
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ)
    {
        if(nModeTerminal(nam, nau, UDST_AUDIO_CTRL_INPUT_TERMINAL, all) && !num--)
        {
            return nau;
        }
    }
    return NULL;
}
/* \\\ */

/* /// "nGetInputUnit()" */
struct NepAudioUnit * nGetInputUnit(struct NepAudioMode *nam)
{
    struct NepAudioMode *recnam = nRecMode(nam);
    struct NepAudioUnit *nau = NULL;
    if(recnam)
    {
        if(recnam->nam_RootUnit->nau_SelectorUnit)
        {
            nau = recnam->nam_RootUnit->nau_SelectorUnit->nau_InputUnit[nam->nam_CurrInput]->nau_RootUnit;
        } else {
            /* the input terminal AHI's input number names, counted as
               nInputNames named them: the gain and monitor of a path are kept
               on its input terminal, not on the stream's output terminal */
            nau = nModeInput(nam->nam_Unit, recnam, nam->nam_CurrInput, FALSE);
            if(!nau && !nModeInput(nam->nam_Unit, recnam, 0, FALSE))
            {
                nau = nModeInput(nam->nam_Unit, recnam, nam->nam_CurrInput, TRUE);
            }
        }
    }
    return(nau);
}
/* \\\ */

/* /// "nInputName()" */
/* The inputs AHI is offered for a mode, those of its recording half: a
   selector unit's, else the input terminals its stream comes from
   (nExamineAudioUnits; a webcam's one microphone). Returns how many there are, and
   the name of input num in *name (NULL beyond the count). Number 0 is the
   default (ahi_sub.doc). */
static ULONG nInputName(struct NepAudioMode *nam, LONG num, STRPTR *name)
{
    struct NepAudioMode *recnam = nRecMode(nam);
    struct NepAudioUnit *sel = recnam ? recnam->nam_RootUnit->nau_SelectorUnit : NULL;
    ULONG count = sel ? sel->nau_NumInputs : (recnam ? recnam->nam_NumInputs : 0);

    *name = NULL;
    if((num >= 0) && ((ULONG) num < count))
    {
        *name = sel ? sel->nau_InputUnit[num]->nau_RootUnit->nau_Name : recnam->nam_InputNames[num];
    }
    return count;
}
/* \\\ */

/* /// "nOutputName()" */
/* The outputs AHI is offered for a mode (nMapOutputs): one output terminal
   by its name; several as "All outputs" - what the device does by itself,
   so number 0 - followed by each of them if they can be switched; none for
   a mode that only records. As nInputName(). */
static ULONG nOutputName(struct NepAudioMode *nam, LONG num, STRPTR *name)
{
    ULONG outputs = nGetOutputUnit(nam) ? nam->nam_NumOutputs : 0;
    ULONG count = outputs;

    if(outputs > 1)
    {
        count = nam->nam_OutputSwitch ? outputs + 1 : 1;
    }
    *name = NULL;
    if((num < 0) || ((ULONG) num >= count))
    {
        return count;
    }
    if(outputs == 1)
    {
        *name = nam->nam_Outputs[0].nao_Name;
    } else {
        *name = num ? nam->nam_Outputs[num - 1].nao_Name : (STRPTR) "All outputs";
    }
    return count;
}
/* \\\ */

/* /// "nSetFeature()" */
/* SET_CUR one Feature Unit Control (UAC 1.0, 5.2.2.4.3) on one channel (FU_CHAN_*);
   a failure is logged as "Could not set <what>". */
static void nSetFeature(struct NepClassAudio *nch, struct NepAudioUnit *nau, UBYTE control,
                        UWORD chan, UBYTE *buf, ULONG len, STRPTR what)
{
    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_INTERFACE, UAUDR_SET_CUR,
                 ((ULONG) control << 8) | chan, (nau->nau_UnitID << 8) | nch->nch_IfNum);
    LONG ioerr = psdDoPipe(nch->nch_EP0Pipe, buf, len);
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Could not set %s (channel %ld): %s (%ld)!",
                       what, (ULONG) chan, psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
    }
}
/* \\\ */

/* /// "nSetVolume()" */
/* Set a feature unit's volume to AHI's gain, a linear factor: in the unit's
   1/256 dB, within its range. The master Control is cascaded after the
   channel Controls (UAC 1.0, 3.5.5): a unit that has one gets the level on
   it and its channels at 0 dB, so they add nothing; one without gets the
   level on each channel that has a Control. */
static void nSetVolume(struct NepClassAudio *nch, struct NepAudioUnit *nau, Fixed gain, STRPTR what)
{
    LONG db = nFixedToDb(gain);
    BOOL hasmaster = nau->nau_VolCtrlMask & FU_CHANBIT(FU_CHAN_MASTER);

    if(db < nau->nau_MinVolDb)
    {
        db = nau->nau_MinVolDb;
    }
    else if(db > nau->nau_MaxVolDb)
    {
        db = nau->nau_MaxVolDb;
    }
    KPRINTF(10, ("Setting %s %ld to %08lx (%04lx)\n", what, nau->nau_UnitID, gain, db));
    for(UWORD chan = FU_CHAN_MASTER; chan < FU_CHANNELS; chan++)
    {
        if(nau->nau_VolCtrlMask & FU_CHANBIT(chan))
        {
            LONG level = (hasmaster && (chan != FU_CHAN_MASTER)) ? 0 : db;
            UBYTE buf[2] = { level, level >> 8 };       /* wVolume, little-endian */

            nSetFeature(nch, nau, UAFUCS_VOLUME, chan, buf, 2, what);
        }
    }
}
/* \\\ */

/* /// "nSetMute()" */
/* Mute or unmute a Feature Unit. Its master Mute Control, if it has one, is
   cascaded after the channel Controls (UAC 1.0, 3.5.5) and silences them all
   by itself; without one each channel's Mute is set. bMute: 1 = muted
   (5.2.2.4.3.1). */
static void nSetMute(struct NepClassAudio *nch, struct NepAudioUnit *nau, BOOL mute)
{
    UBYTE buf[1] = { mute ? 1 : 0 };
    STRPTR what = mute ? (STRPTR) "mute" : (STRPTR) "unmute";

    KPRINTF(10, ("%s unit %ld\n", mute ? "Muting" : "Unmuting", nau->nau_UnitID));
    if(nau->nau_MuteCtrlMask & FU_CHANBIT(FU_CHAN_MASTER))
    {
        nSetFeature(nch, nau, UAFUCS_MUTE, FU_CHAN_MASTER, buf, 1, what);
        return;
    }
    for(UWORD chan = FU_CHAN_LEFT; chan < FU_CHANNELS; chan++)
    {
        if(nau->nau_MuteCtrlMask & FU_CHANBIT(chan))
        {
            nSetFeature(nch, nau, UAFUCS_MUTE, chan, buf, 1, what);
        }
    }
}
/* \\\ */

/* /// "nAudioTask()" */
void nAudioTask()
{

    struct NepClassAudio *nch;
    struct NepAudioMode *nam;
    struct NepAudioMode *recnam;
    struct NepAudioUnit *innau;
    ULONG sigmask;
    ULONG sigs;
    ULONG flags;
    UBYTE buf[2];
    LONG ioerr;

    if((nch = nAllocAudio()))
    {
        Forbid();
        if(nch->nch_ReadySigTask)
        {
            Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
        }
        Permit();
        sigmask = (1L<<nch->nch_TaskMsgPort->mp_SigBit)|SIGBREAKF_CTRL_C;
        do
        {
            //psdDoPipe(nch->nch_CurrentMode->nam_EPPipe, nch, 1024);
            sigs = Wait(sigmask);
            /* Taken with the mode they are for: AHI sets bits and closes the
               mode from its own task. A bit set from here on stays for the
               next wake. With no mode open there is nothing to set - the
               values stay in the mode, and AHI sets them again at the next
               open. */
            Forbid();
            flags = nch->nch_UpdateFlags;
            nch->nch_UpdateFlags = 0;
            nam = nch->nch_CurrentMode;
            Permit();
            if(flags && nam)
            {
                recnam = nRecMode(nam);
                if((flags &UAF_SELECT_INPUT) && recnam && recnam->nam_RootUnit->nau_SelectorUnit)
                {
                    struct NepAudioUnit *nau = recnam->nam_RootUnit->nau_SelectorUnit;
                    KPRINTF(10, ("Setting input %ld to %08lx\n", nau->nau_UnitID, nam->nam_CurrInput));
                    buf[0] = nam->nam_CurrInput + 1;
                    psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_INTERFACE, UAUDR_SET_CUR, 0, (nau->nau_UnitID<<8)|nch->nch_IfNum);
                    ioerr = psdDoPipe(nch->nch_EP0Pipe, buf, 1);
                    if(ioerr)
                    {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                       "Could not select input source: %s (%ld)!",
                                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    }
                }
                if(flags &UAF_SELECT_OUTPUT)
                {
                    /* AHI output 0 is all outputs, k the k-th alone (nam_Outputs[k - 1]) */
                    for(ULONG cnt = 0; cnt < nam->nam_NumOutputs; cnt++)
                    {
                        BOOL mute = nam->nam_CurrOutput && (nam->nam_CurrOutput != cnt + 1);

                        nSetMute(nch, nam->nam_Outputs[cnt].nao_Mute, mute);
                    }
                }
                if(flags &UAF_MASTER_VOLUME)
                {
                    /* the volume upstream of every output, else each output's own;
                       muted ones too, so switching needs no new volume */
                    if(nam->nam_OutVolume)
                    {
                        nSetVolume(nch, nam->nam_OutVolume, nam->nam_MasterVol, "master volume");
                    } else {
                        for(ULONG cnt = 0; cnt < nam->nam_NumOutputs; cnt++)
                        {
                            if(nam->nam_Outputs[cnt].nao_Volume)
                            {
                                nSetVolume(nch, nam->nam_Outputs[cnt].nao_Volume, nam->nam_MasterVol, "master volume");
                            }
                        }
                    }
                }
                innau = nGetInputUnit(nam);
                if((flags &UAF_INPUT_GAIN) && innau && innau->nau_VolumeControl)
                {
                    nSetVolume(nch, innau->nau_VolumeUnit, nam->nam_InputGain, "input gain");
                }
                if((flags &UAF_MONITOR_VOLUME) && innau && innau->nau_Monitor)
                {
                    nSetVolume(nch, innau->nau_MonitorUnit, nam->nam_MonitorVol, "monitor volume");
                }
            }
        } while(!(sigs & SIGBREAKF_CTRL_C));
        if(nch->nch_CurrentMode)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Cannot exit yet, audio playback still active!");
            while(nch->nch_CurrentMode)
            {
                psdDelayMS(500);
            }
        }
        KPRINTF(20, ("Going down the river!\n"));
        nFreeAudio(nch);
    }
    
}
/* \\\ */

/* /// "nFindAudioUnit()" */
struct NepAudioUnit * nFindAudioUnit(struct NepClassAudio *nch, ULONG unitid)
{
    struct NepAudioUnit *nau;

    nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    while(nau->nau_Node.ln_Succ)
    {
        if(nau->nau_UnitID == unitid)
        {
            return(nau);
        }
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ;
    }
    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Requested Unit %02lx not found.", unitid);
    return(NULL);
}
/* \\\ */

/* /// "nIsUpstream()" */
/* Does unit feed into of, directly or through other units? Descriptors may
   wire a loop: the search goes no deeper than UNIT_DEPTH_MAX units. */
static BOOL nIsUpstream(struct NepAudioUnit *unit, struct NepAudioUnit *of, UWORD depth)
{
    if(!depth)
    {
        return FALSE;
    }
    for(UWORD cnt = 0; cnt < of->nau_NumInputs; cnt++)
    {
        struct NepAudioUnit *in = of->nau_InputUnit[cnt];

        if((in == unit) || nIsUpstream(unit, in, depth - 1))
        {
            return TRUE;
        }
    }
    return FALSE;
}
/* \\\ */

/* /// "nFindAndConnectAudioUnit()" */
/* Link a unit to the one feeding it. An edge that would close a loop is
   refused, so the graph stays acyclic and every walk over it ends. */
void nFindAndConnectAudioUnit(struct NepClassAudio *nch, struct NepAudioUnit *targetnau, ULONG unitid)
{
    struct NepAudioUnit *sourcenau = nFindAudioUnit(nch, unitid);
    KPRINTF(10, ("Connecting unit %02lx to %02lx\n", unitid, targetnau->nau_UnitID));
    if(!sourcenau)
    {
        return;
    }
    if((sourcenau == targetnau) || nIsUpstream(targetnau, sourcenau, UNIT_DEPTH_MAX))
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Unit %02lx would feed itself; link ignored.", unitid);
        return;
    }
    if(sourcenau->nau_NumOutputs < 8)
    {
        sourcenau->nau_OutputUnit[sourcenau->nau_NumOutputs] = targetnau;
        sourcenau->nau_NumOutputs++;
    }
    if(targetnau->nau_NumInputs < 8)
    {
        targetnau->nau_InputUnit[targetnau->nau_NumInputs] = sourcenau;
        targetnau->nau_NumInputs++;
    }
}
/* \\\ */

/* /// "nFlowDown()" */
void nFlowDown(struct NepClassAudio *nch, struct NepAudioUnit *nau)
{
    UWORD cnt;

    KPRINTF(10, ("FlowDown %02lx\n", nau->nau_UnitID));

    if(nau->nau_InputUnit[0]->nau_RootUnit && ((nau->nau_Type != UDST_AUDIO_CTRL_OUTPUT_TERMINAL) || (nau->nau_TermType != UAUTT_STREAMING)))
    {
        // use parent audio mode for flow down
        nau->nau_RootUnit = nau->nau_InputUnit[0]->nau_RootUnit;
    }
    for(cnt = 0; cnt < nau->nau_NumOutputs; cnt++)
    {
        nFlowDown(nch, nau->nau_OutputUnit[cnt]);
    }
}
/* \\\ */

/* /// "nFlowDownToUSBSink()" */
struct NepAudioUnit * nFlowDownToUSBSink(struct NepClassAudio *nch, struct NepAudioUnit *nau)
{
    UWORD cnt;
    struct NepAudioUnit *resnau = NULL;
    KPRINTF(10, ("FlowDownToUSBSink %02lx\n", nau->nau_UnitID));

    if((nau->nau_Type == UDST_AUDIO_CTRL_OUTPUT_TERMINAL) && (nau->nau_TermType == UAUTT_STREAMING))
    {
        KPRINTF(10, ("USB Outterm found\n"));
        nau->nau_SinkUnit = nau;
        return nau;
    }
    for(cnt = 0; cnt < nau->nau_NumOutputs; cnt++)
    {
        nau->nau_SinkUnit = nFlowDownToUSBSink(nch, nau->nau_OutputUnit[cnt]);
        if((resnau = nau->nau_SinkUnit))
        {
            break;
        }
    }
    return(resnau);
}
/* \\\ */

/* /// "nFlowUpToUSBSource()" */
struct NepAudioUnit * nFlowUpToUSBSource(struct NepClassAudio *nch, struct NepAudioUnit *nau)
{
    UWORD cnt;
    struct NepAudioUnit *resnau = NULL;
    KPRINTF(10, ("FlowsDownToUSB %02lx\n", nau->nau_UnitID));

    if((nau->nau_Type == UDST_AUDIO_CTRL_INPUT_TERMINAL) && (nau->nau_TermType == UAUTT_STREAMING))
    {
        KPRINTF(10, ("USB interm found\n"));
        nau->nau_SourceUnit = nau;
        return nau;
    }
    for(cnt = 0; cnt < nau->nau_NumInputs; cnt++)
    {
        nau->nau_SourceUnit = nFlowUpToUSBSource(nch, nau->nau_InputUnit[cnt]);
        if((resnau = nau->nau_SourceUnit))
        {
            break;
        }
    }
    return(resnau);
}
/* \\\ */

/* /// "nFlowUp()" */
void nFlowUp(struct NepClassAudio *nch, struct NepAudioUnit *nau)
{
    UWORD cnt;
    UBYTE *descdata = nau->nau_Descriptor;

    KPRINTF(10, ("FlowUp %02lx\n", nau->nau_UnitID));

    if(nau->nau_Name)
    {
        return;
    }

    // use downlink audio mode for flow up
    for(cnt = 0; cnt < nau->nau_NumInputs; cnt++)
    {
        if(nau->nau_RootUnit)
        {
            if(!nau->nau_InputUnit[cnt]->nau_RootUnit)
            {
                nau->nau_InputUnit[cnt]->nau_RootUnit = nau->nau_RootUnit;
            }
        }
        nFlowUp(nch, nau->nau_InputUnit[cnt]);
    }

    switch(nau->nau_Type)
    {

        case UDST_AUDIO_CTRL_INPUT_TERMINAL:
        {
            struct UsbAudioInputTermDesc10 *uait = (struct UsbAudioInputTermDesc10 *) descdata;
            ULONG termtype = AROS_WORD2LE(uait->wTerminalType);
            UWORD channelcfg = AROS_WORD2LE(uait->wChannelConfig);
            STRPTR spacial = nConcatBitsStr(nch, NTS_SPATIALLOCATION, (ULONG) channelcfg);
            nau->nau_Name = psdCopyStrFmt("%s (%s)", nNumToStr(nch, NTS_TERMINALTYPE, termtype, "Unknown"), spacial);
            psdFreeVec(spacial);
            break;
        }

        case UDST_AUDIO_CTRL_OUTPUT_TERMINAL:
            nau->nau_Name = psdCopyStrFmt("%s-Output from %s",
                                          nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_TermType, "Unknown"),
                                          nau->nau_InputUnit[0]->nau_Name);
            nau->nau_OutChannels = nau->nau_InputUnit[0]->nau_OutChannels;
            nau->nau_ChannelCfg = nau->nau_InputUnit[0]->nau_ChannelCfg;
            break;

        case UDST_AUDIO_CTRL_FEATURE_UNIT:
        {
            UWORD features = 0;
            UWORD featsize = descdata[5];
            UWORD feat;
            UBYTE *featptr = descdata + 6;
            UWORD featsizecnt;
            STRPTR featstr;

            nau->nau_TermType = nau->nau_InputUnit[0]->nau_TermType;
            nau->nau_OutChannels = nau->nau_InputUnit[0]->nau_OutChannels;
            nau->nau_ChannelCfg = nau->nau_InputUnit[0]->nau_ChannelCfg;

            for(cnt = 0; cnt <= nau->nau_InputUnit[0]->nau_OutChannels; cnt++)
            {
                feat = 0;
                for(featsizecnt = 0; featsizecnt < featsize; featsizecnt++)
                {
                    feat |= (*featptr++)<<(featsizecnt<<3);
                }
                KPRINTF(10, ("Feature Unit %02lx, Channel %ld: %04lx\n", nau->nau_UnitID, cnt, feat));
                features |= feat;
            }
            featstr = nConcatBitsStr(nch, NTS_FEATURE, (ULONG) features);
            nau->nau_Name = psdCopyStrFmt("%s for %s", featstr, nau->nau_InputUnit[0]->nau_Name);
            psdFreeVec(featstr);
            break;
        }

        case UDST_AUDIO_CTRL_SELECTOR_UNIT:
            for(cnt = 0; cnt < nau->nau_NumInputs; cnt++)
            {
                UWORD taroutch = nau->nau_InputUnit[cnt]->nau_OutChannels;
                if(cnt)
                {
                    if(nau->nau_OutChannels != taroutch)
                    {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Selector channels %ld != %ld conflict.",
                                       nau->nau_OutChannels, taroutch);
                    }
                }
                nau->nau_OutChannels = taroutch;
                nau->nau_ChannelCfg = nau->nau_InputUnit[cnt]->nau_ChannelCfg;
            }
            // fall through

        case UDST_AUDIO_CTRL_MIXER_UNIT:
        case UDST_AUDIO_CTRL_PROCESSING_UNIT:
        case UDST_AUDIO_CTRL_EXTENSION_UNIT:
        {
            UBYTE buf[64];
            UBYTE *bufptr = buf;
            ULONG buflen = 63;
            UBYTE *srcptr;

            if(nau->nau_NumInputs == 1)
            {
                // not ambigious, so lets copy the input.
                nau->nau_TermType = nau->nau_InputUnit[0]->nau_TermType;
            }
            for(cnt = 0; buflen && (cnt < nau->nau_NumInputs); cnt++)
            {
                if(cnt && buflen)
                {
                    if(nau->nau_Type == UDST_AUDIO_CTRL_SELECTOR_UNIT)
                    {
                        *bufptr++ = '/';
                    } else {
                        *bufptr++ = '+';
                    }
                    if(!(--buflen))
                    {
                        break;
                    }
                }
                /*if(nau->nau_InputUnit[cnt]->nau_TermType == UAUTT_STREAMING)
                {
                    srcptr = "Loopback";
                } else {*/
                    srcptr = nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_InputUnit[cnt]->nau_TermType, "Unknown");
                //}
                while((*bufptr++ = *srcptr++))
                {
                    if(!(--buflen))
                    {
                        break;
                    }
                }
                bufptr--;
            }
            *bufptr = 0;
            nau->nau_Name = psdCopyStrFmt("%s (%s)", nNumToStr(nch, NTS_UNITTYPE, (ULONG) nau->nau_Type, "Unknown"), buf);
            break;
        }
    }
}
/* \\\ */

/* /// "nExamineAudioDescriptors()" */
void nExamineAudioDescriptors(struct NepClassAudio *nch)
{
    APTR desc;
    UBYTE *descdata;
    IPTR ifnum;
    struct NepAudioMode *nam;
    struct PsdInterface *streampif;
    APTR subdesc;
    UWORD cnt;
    struct UsbAudioHeaderDesc10 *header;
    struct UsbAudioGeneralIFDesc *uagi;
    struct UsbAudioType1FormatDesc *uat1f;
    struct UsbAudioGeneralEPDesc *uage;
    ULONG audioformat;
    BOOL freqctrl = FALSE;
    BOOL pitchctrl = FALSE;
    ULONG ahimodeid = AHI_USB_MODE_BASE + (nch->nch_UnitNo<<12);

    KPRINTF(10, ("ExamineAudioDesc Stage 1\n"));

    desc = psdFindDescriptor(nch->nch_Device, NULL,
                                   DDA_Interface, nch->nch_Interface,
                                   DDA_DescriptorType, UDT_CS_INTERFACE,
                                   DDA_CS_SubType, UDST_AUDIO_CTRL_HEADER,
                                   TAG_END);
    if(!desc)
    {
        return;
    }
    psdGetAttrs(PGA_DESCRIPTOR, desc,
                DDA_DescriptorData, &descdata,
                TAG_END);
    header = (struct UsbAudioHeaderDesc10 *) descdata;

    for(cnt = 0; cnt < header->bInCollection; cnt++)
    {
        streampif = NULL;
        while((streampif = psdFindInterface(nch->nch_Device, streampif,
                                            IFA_InterfaceNum, header->baInterfaceNr[cnt],
                                            IFA_AlternateNum, 0xffffffff,
                                            IFA_Class, AUDIO_CLASSCODE,
                                            IFA_SubClass, AUDIO_STREAM_SUBCLASS,
                                            TAG_END)))
        {
            psdGetAttrs(PGA_INTERFACE, streampif,
                        IFA_InterfaceNum, &ifnum,
                        TAG_END);
            subdesc = psdFindDescriptor(nch->nch_Device, NULL,
                                        DDA_Interface, streampif,
                                        DDA_DescriptorType, UDT_CS_INTERFACE,
                                        DDA_CS_SubType, UDST_AUDIO_STREAM_GENERAL,
                                        TAG_END);
            if(subdesc)
            {
                psdGetAttrs(PGA_DESCRIPTOR, subdesc,
                            DDA_DescriptorData, &uagi,
                            TAG_END);
                audioformat = uagi->wFormatTag0|(uagi->wFormatTag1<<8);
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Terminal %02lx (%ld delay) of Format %s",
                               uagi->bTerminalLink,
                               uagi->bDelay,
                               nNumToStr(nch, NTS_AUDIOFORMAT, audioformat, "Unknown"));
                if(audioformat == UAADF_PCM)
                {
                    subdesc = psdFindDescriptor(nch->nch_Device, NULL,
                                                DDA_Interface, streampif,
                                                DDA_DescriptorType, UDT_CS_INTERFACE,
                                                DDA_CS_SubType, UDST_AUDIO_STREAM_FMT_TYPE,
                                                TAG_END);
                    if(subdesc)
                    {
                        psdGetAttrs(PGA_DESCRIPTOR, subdesc,
                                    DDA_DescriptorData, &uat1f,
                                    TAG_END);

                        subdesc = psdFindDescriptor(nch->nch_Device, NULL,
                                                    DDA_Interface, streampif,
                                                    DDA_DescriptorType, UDT_CS_ENDPOINT,
                                                    TAG_END);
                        pitchctrl = freqctrl = FALSE;
                        if(subdesc)
                        {
                            psdGetAttrs(PGA_DESCRIPTOR, subdesc,
                                        DDA_DescriptorData, &uage,
                                        TAG_END);

                            if(uage->bmAttributes & UAECS_SAMPLE_FREQ) // actually, should check the bit, but the control selector has the right value by incident ;)
                            {
                                freqctrl = TRUE;
                                /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Terminal %02lx has frequency control.",
                                               uagi->bTerminalLink);*/
                            }
                            if(uage->bmAttributes & UAECS_PITCH)  // actually, should check the bit, but the control selector has the right value by incident ;)
                            {
                                pitchctrl = TRUE;
                                /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Terminal %02lx has pitch control.",
                                               uagi->bTerminalLink);*/
                            }
                        }

                        /* mono or stereo, of 8, 16 or 17 to 32 bits */
                        BOOL typegood = ((uat1f->bNrChannels == 1) || (uat1f->bNrChannels == 2)) &&
                                        ((uat1f->bBitResolution == 8) || (uat1f->bBitResolution == 16) ||
                                         ((uat1f->bBitResolution > 16) && (uat1f->bBitResolution <= 32)));

                        if(!typegood)
                        {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Unsupported mode '%s' with %ld bits x %ld channels ignored.",
                                           nNumToStr(nch, NTS_AUDIOFORMAT, audioformat, "Unknown"),
                                           uat1f->bBitResolution,
                                           uat1f->bNrChannels);
                        } else {
                            if((nam = AllocVec(sizeof(struct NepAudioMode), MEMF_CLEAR|MEMF_PUBLIC)))
                            {
                                nam->nam_SubLibBase = nch->nch_ClsBase->nh_SubLibBase;
                                nam->nam_Unit = nch;
                                nam->nam_Interface = streampif;
                                nam->nam_ZeroBWIF = psdFindInterface(nch->nch_Device, NULL,
                                                                     IFA_InterfaceNum, ifnum,
                                                                     IFA_AlternateNum, 0xffffffff,
                                                                     IFA_Class, AUDIO_CLASSCODE,
                                                                     IFA_SubClass, AUDIO_STREAM_SUBCLASS,
                                                                     IFA_NumEndpoints, 0,
                                                                     TAG_END);
                                if(!nam->nam_ZeroBWIF)
                                {
                                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Zero Bandwidth endpoint not found!");
                                }

                                nam->nam_IfNum = ifnum;
                                nam->nam_NumChannels = uat1f->bNrChannels;
                                nam->nam_FrameSize = uat1f->bSubframeSize * nam->nam_NumChannels;
                                nam->nam_SampleSize = uat1f->bSubframeSize;
                                nam->nam_Resolution = uat1f->bBitResolution;
                                nam->nam_TerminalID = uagi->bTerminalLink;
                                nam->nam_HasFreqCtrl = freqctrl;
                                nam->nam_HasPitchCtrl = pitchctrl;

                                AddTail(&nch->nch_AudioModes, &nam->nam_Node);
                                if(uat1f->bSamFreqType)
                                {
                                    UWORD cnt;
                                    ULONG freq;
                                    UBYTE *fptr = uat1f->tSamFreq0;
                                    nam->nam_MinFreq = 100000;
                                    nam->nam_MaxFreq = 1;
                                    nam->nam_NumFrequencies = (uat1f->bSamFreqType < 64) ? uat1f->bSamFreqType : 64;
                                    for(cnt = 0; cnt < nam->nam_NumFrequencies; cnt++)
                                    {
                                        freq = fptr[0]|(fptr[1]<<8)|(fptr[2]<<16);
                                        if(freq < nam->nam_MinFreq)
                                        {
                                            nam->nam_MinFreq = freq;
                                        }
                                        if(freq > 64000)
                                        {
                                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "AHI does not support %ld Hz (above 65 KHz).", freq);
                                            nam->nam_NumFrequencies--;
                                            cnt--;
                                            fptr += 3;
                                            continue;
                                        }
                                        if(freq > nam->nam_MaxFreq)
                                        {
                                            nam->nam_MaxFreq = freq;
                                        }
                                        nam->nam_FreqArray[cnt] = freq;
                                        fptr += 3;
                                    }
                                    qsort(nam->nam_FreqArray,
                                          nam->nam_NumFrequencies,
                                          sizeof(nam->nam_FreqArray[0]),
                                          compare_frequencies);
                                } else {
                                    const ULONG *freqtab = commonFreqs;
                                    /* Continuous range: tSamFreq0 holds the lower
                                       bound, the following 3 descriptor bytes the
                                       upper bound. */
                                    const UBYTE *fptr = uat1f->tSamFreq0;
                                    nam->nam_MinFreq = fptr[0]|(fptr[1]<<8)|(fptr[2]<<16);
                                    nam->nam_MaxFreq = fptr[3]|(fptr[4]<<8)|(fptr[5]<<16);
                                    if(nam->nam_MaxFreq > 64000)
                                    {
                                        nam->nam_MaxFreq = 64000;
                                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "AHI does not support frequencies above 65 KHz.");
                                    }
                                    nam->nam_NumFrequencies = 0;
                                    while(*freqtab && (nam->nam_NumFrequencies < 64))
                                    {
                                        if((*freqtab >= nam->nam_MinFreq) && (*freqtab <= nam->nam_MaxFreq))
                                        {
                                            nam->nam_FreqArray[nam->nam_NumFrequencies++] = *freqtab;
                                        }
                                        freqtab++;
                                    }
                                }
                                nam->nam_AHIModeID = ++ahimodeid;
                                psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Found mode with %ld channel(s) (%ld byte(s) per frame) and %ld bits",
                                               uat1f->bNrChannels,
                                               uat1f->bSubframeSize,
                                               uat1f->bBitResolution);

                            }
                        }

                        /*if(!uat1f->bSamFreqType)
                        {
                            ULONG lowfreq = uat1f->tSamFreq0[0]|(uat1f->tSamFreq0[1]<<8)|(uat1f->tSamFreq0[2]<<16);
                            ULONG highfreq = uat1f->tSamFreq0[3]|(uat1f->tSamFreq0[4]<<8)|(uat1f->tSamFreq0[5]<<16);
                            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Continuous frequency from %ld Hz to %ld Hz",
                                           lowfreq, highfreq);
                        } else {
                            ULONG freq = uat1f->tSamFreq0[0]|(uat1f->tSamFreq0[1]<<8)|(uat1f->tSamFreq0[2]<<16);
                            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "%ld frequencies supported (first one being %ld Hz)",
                                           uat1f->bSamFreqType, freq);

                        }*/
                    } else {
                        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname, "Interface %ld is missing type 1 format descriptor!", header->baInterfaceNr[cnt]);
                    }
                } else {
                    psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname, "Terminal %02lx with Format '%s' is not supported!",
                                   uagi->bTerminalLink,
                                   nNumToStr(nch, NTS_AUDIOFORMAT, audioformat, "Unknown"));
                }
            }
        }
    }
}
/* \\\ */

/* /// "nFindLogVolume()" */
/* The unit's volume range as AHI takes it, linear factors from the unit's
   1/256 dB. A range reported upside down is turned round. */
void nFindLogVolume(struct NepClassAudio *nch, struct NepAudioUnit *nau)
{
    if(nau->nau_MinVolDb > nau->nau_MaxVolDb)
    {
        WORD lowest = nau->nau_MaxVolDb;

        nau->nau_MaxVolDb = nau->nau_MinVolDb;
        nau->nau_MinVolDb = lowest;
    }
    nau->nau_MinVolume = nDbToFixed(nau->nau_MinVolDb);
    nau->nau_MaxVolume = nDbToFixed(nau->nau_MaxVolDb);
}
/* \\\ */

/* /// "nInputNames()" */
/* Name the input terminals a recording mode's stream reaches (nModeTerminal;
   with all, every input terminal) into nam_InputNames, at most
   MAX_PORT_NAMES. Returns how many. */
static ULONG nInputNames(struct NepClassAudio *nch, struct NepAudioMode *nam, BOOL all)
{
    ULONG count = 0;

    for(struct NepAudioUnit *nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
        nau->nau_Node.ln_Succ && (count < MAX_PORT_NAMES);
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ)
    {
        if(nModeTerminal(nam, nau, UDST_AUDIO_CTRL_INPUT_TERMINAL, all))
        {
            nam->nam_InputNames[count++] = nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_TermType, "Unknown");
        }
    }
    return count;
}
/* \\\ */

/* /// "nOutputTerminals()" */
/* Fill nam_Outputs with the output terminals a playback mode's stream flows
   to (nModeTerminal; with all, every output terminal), without controls yet,
   at most MAX_PORT_NAMES. Returns how many. */
static ULONG nOutputTerminals(struct NepClassAudio *nch, struct NepAudioMode *nam, BOOL all)
{
    struct NepAudioOutput *out = nam->nam_Outputs;
    ULONG count = 0;

    for(struct NepAudioUnit *nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
        nau->nau_Node.ln_Succ && (count < MAX_PORT_NAMES);
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ)
    {
        if(!nModeTerminal(nam, nau, UDST_AUDIO_CTRL_OUTPUT_TERMINAL, all))
        {
            continue;
        }
        out[count].nao_Name = nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_TermType, "Unknown");
        out[count].nao_Terminal = nau;
        out[count].nao_Mute = NULL;
        out[count].nao_Volume = NULL;
        count++;
    }
    return count;
}
/* \\\ */

/* /// "nStreamInput()" */
/* The input of unit the stream from root comes through; NULL if none */
static struct NepAudioUnit * nStreamInput(struct NepAudioUnit *unit, struct NepAudioUnit *root)
{
    for(UWORD in = 0; in < unit->nau_NumInputs; in++)
    {
        struct NepAudioUnit *cand = unit->nau_InputUnit[in];

        if((cand == root) || nIsUpstream(root, cand, UNIT_DEPTH_MAX))
        {
            return cand;
        }
    }
    return NULL;
}
/* \\\ */

/* /// "nFeedsOutputs()" */
/* Does unit feed the outputs of a mode: every one of them (all), or any
   but number self (all = FALSE)? */
static BOOL nFeedsOutputs(struct NepAudioMode *nam, struct NepAudioUnit *unit, ULONG self, BOOL all)
{
    for(ULONG cnt = 0; cnt < nam->nam_NumOutputs; cnt++)
    {
        BOOL feeds = nIsUpstream(unit, nam->nam_Outputs[cnt].nao_Terminal, UNIT_DEPTH_MAX);

        if(all && !feeds)
        {
            return FALSE;
        }
        if(!all && feeds && (cnt != self))
        {
            return TRUE;
        }
    }
    return all;
}
/* \\\ */

/* /// "nOutputBranch()" */
/* The Feature Units on output number self's own branch - from its terminal
   up the way the stream comes, to the first unit that also feeds another
   output: the first with a Mute Control switches the output (nao_Mute),
   the first with a Volume Control sets its level (nao_Volume) */
static void nOutputBranch(struct NepAudioMode *nam, ULONG self)
{
    struct NepAudioOutput *out = &nam->nam_Outputs[self];
    struct NepAudioUnit *unit = out->nao_Terminal;

    for(UWORD steps = 0; steps < UNIT_DEPTH_MAX; steps++)
    {
        struct NepAudioUnit *up = nStreamInput(unit, nam->nam_RootUnit);

        if(!up || (up == nam->nam_RootUnit) || nFeedsOutputs(nam, up, self, FALSE))
        {
            return;
        }
        if(up->nau_Type == UDST_AUDIO_CTRL_FEATURE_UNIT)
        {
            if(!out->nao_Mute && up->nau_MuteCtrlMask)
            {
                out->nao_Mute = up;
            }
            if(!out->nao_Volume && up->nau_VolCtrlMask)
            {
                out->nao_Volume = up;
            }
        }
        unit = up;
    }
}
/* \\\ */

/* /// "nSharedVolume()" */
/* The last Feature Unit with a Volume Control the stream feeds that is
   upstream of every output - the one the class always took on a single
   path; with reached = FALSE (the outputs are not the stream's) the last
   the stream feeds at all. NULL if there is none. */
static struct NepAudioUnit * nSharedVolume(struct NepClassAudio *nch, struct NepAudioMode *nam, BOOL reached)
{
    struct NepAudioUnit *found = NULL;

    for(struct NepAudioUnit *nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
        nau->nau_Node.ln_Succ;
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ)
    {
        if((nau->nau_Type != UDST_AUDIO_CTRL_FEATURE_UNIT) || !nau->nau_VolCtrlMask ||
           !nIsUpstream(nam->nam_RootUnit, nau, UNIT_DEPTH_MAX))
        {
            continue;
        }
        if(!reached || nFeedsOutputs(nam, nau, 0, TRUE))
        {
            found = nau;
        }
    }
    return found;
}
/* \\\ */

/* /// "nMapOutputs()" */
/* The outputs of a playback mode as AHI sees them: the output terminals its
   stream flows to (nOutputTerminals), each with its own Mute and Volume
   (nOutputBranch), the volume upstream of all of them (nSharedVolume), and
   whether AHI may choose one - several, each with its own Mute
   (nam_OutputSwitch). If the tagging found no output, every output terminal
   is named, without controls. */
static void nMapOutputs(struct NepClassAudio *nch, struct NepAudioMode *nam)
{
    BOOL reached = TRUE;
    ULONG count = nOutputTerminals(nch, nam, FALSE);

    if(!count)
    {
        reached = FALSE;
        count = nOutputTerminals(nch, nam, TRUE);
    }
    nam->nam_NumOutputs = count;
    nam->nam_OutVolume = NULL;
    nam->nam_OutputSwitch = FALSE;
    if(!nam->nam_RootUnit)
    {
        return;
    }
    nam->nam_OutVolume = nSharedVolume(nch, nam, reached);
    if(!reached)
    {
        return;
    }
    nam->nam_OutputSwitch = (count > 1);
    for(ULONG cnt = 0; cnt < count; cnt++)
    {
        nOutputBranch(nam, cnt);
        nam->nam_OutputSwitch &= (nam->nam_Outputs[cnt].nao_Mute != NULL);
    }
}
/* \\\ */

/* /// "nExamineAudioUnits()" */
void nExamineAudioUnits(struct NepClassAudio *nch)
{
    APTR desc;
    UBYTE *descdata;
    IPTR subtype;
    struct NepAudioMode *nam;
    struct NepAudioUnit *nau;
    KPRINTF(10, ("ExamineAudioDesc Stage 2\n"));

    desc = NULL;
    while((desc = psdFindDescriptor(nch->nch_Device, desc,
                                   DDA_Interface, nch->nch_Interface,
                                   DDA_DescriptorType, UDT_CS_INTERFACE,
                                   TAG_END)))
    {
        psdGetAttrs(PGA_DESCRIPTOR, desc,
                    DDA_DescriptorData, &descdata,
                    DDA_CS_SubType, &subtype,
                    TAG_END);
        switch(subtype)
        {
            case UDST_AUDIO_CTRL_INPUT_TERMINAL:
            case UDST_AUDIO_CTRL_OUTPUT_TERMINAL:
            case UDST_AUDIO_CTRL_MIXER_UNIT:
            case UDST_AUDIO_CTRL_SELECTOR_UNIT:
            case UDST_AUDIO_CTRL_FEATURE_UNIT:
            case UDST_AUDIO_CTRL_PROCESSING_UNIT:
            case UDST_AUDIO_CTRL_EXTENSION_UNIT:
                nau = psdAllocVec(sizeof(struct NepAudioUnit));
                if(!nau)
                {
                    break;
                }
                nau->nau_Type = subtype;
                nau->nau_UnitID = descdata[3];
                nau->nau_Descriptor = descdata;
                AddTail(&nch->nch_AudioUnits, &nau->nau_Node);
                break;
        }
    }

    // connect units together
    nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    while(nau->nau_Node.ln_Succ)
    {
        descdata = nau->nau_Descriptor;
        switch(nau->nau_Type)
        {
            case UDST_AUDIO_CTRL_OUTPUT_TERMINAL:
                nFindAndConnectAudioUnit(nch, nau, (ULONG) ((struct UsbAudioOutputTermDesc10 *) descdata)->bSourceID);
                break;

            case UDST_AUDIO_CTRL_MIXER_UNIT:
            case UDST_AUDIO_CTRL_SELECTOR_UNIT:
            {
                struct UsbAudioMixerUnitDesc10 *uamu = (struct UsbAudioMixerUnitDesc10 *) descdata;
                UWORD inputpins = uamu->bNrInPins;
                UWORD cnt;

                if(inputpins > 8)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "On Mixer / Selector Unit %02lx with %ld input pins only 8 are supported.",
                                   uamu->bUnitID,
                                   inputpins);
                    inputpins = 8;
                }

                for(cnt = 0; cnt < inputpins; cnt++)
                {
                    nFindAndConnectAudioUnit(nch, nau, (ULONG) descdata[5+cnt]);
                }
                break;
            }

            case UDST_AUDIO_CTRL_FEATURE_UNIT:
                nFindAndConnectAudioUnit(nch, nau, (ULONG) ((struct UsbAudioFeatureUnitDesc10 *) descdata)->bSourceID);
                break;

            case UDST_AUDIO_CTRL_PROCESSING_UNIT:
            case UDST_AUDIO_CTRL_EXTENSION_UNIT:
            {
                struct UsbAudioProcessingUnitDesc10 *uapu = (struct UsbAudioProcessingUnitDesc10 *) descdata;
                UWORD inputpins = uapu->bNrInPins;
                UWORD cnt;

                if(inputpins > 8)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "On Processing / Extension Unit %02lx with %ld input pins only 8 are supported.",
                                   uapu->bUnitID,
                                   inputpins);
                    inputpins = 8;
                }

                for(cnt = 0; cnt < inputpins; cnt++)
                {
                    nFindAndConnectAudioUnit(nch, nau, (ULONG) descdata[7+cnt]);
                }
                break;
            }

        }
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ;
    }

    KPRINTF(10, ("ExamineAudioDesc Stage 3\n"));

    // obtain output channels and configs where known (the rest is done on flow recursion next step)
    nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    while(nau->nau_Node.ln_Succ)
    {
        descdata = nau->nau_Descriptor;
        switch(nau->nau_Type)
        {
            case UDST_AUDIO_CTRL_OUTPUT_TERMINAL:
            {
                struct UsbAudioOutputTermDesc10 *uaot = (struct UsbAudioOutputTermDesc10 *) descdata;
                ULONG termtype = AROS_WORD2LE(uaot->wTerminalType);
                nau->nau_TermType = termtype;
                if(termtype == UAUTT_STREAMING)
                {
                    nau->nau_RootUnit = nau;
                    // try to locate audio mode, which is obviously an input
                    nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
                    while(nam->nam_Node.ln_Succ)
                    {
                        if(nam->nam_TerminalID == uaot->bTerminalID)
                        {
                            nam->nam_IsInput = TRUE;
                            nam->nam_RootUnit = nau;
                            //psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Terminal %02lx assigned to IN audio mode!", nam->nam_TerminalID);
                        }
                        nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
                    }
                }
                break;
            }

            case UDST_AUDIO_CTRL_INPUT_TERMINAL:
            {
                struct UsbAudioInputTermDesc10 *uait = (struct UsbAudioInputTermDesc10 *) descdata;
                UWORD channelcfg = AROS_WORD2LE(uait->wChannelConfig);
                ULONG termtype = AROS_WORD2LE(uait->wTerminalType);

                nau->nau_OutChannels = uait->bNrChannels;
                nau->nau_ChannelCfg = channelcfg;
                nau->nau_TermType = termtype;
                nau->nau_RootUnit = nau;
                if(termtype == UAUTT_STREAMING)
                {
                    // try to locate audio mode, which is obviously an output
                    nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
                    while(nam->nam_Node.ln_Succ)
                    {
                        if(nam->nam_TerminalID == uait->bTerminalID)
                        {
                            nam->nam_IsInput = FALSE;
                            nam->nam_RootUnit = nau;
                            //psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Terminal %02lx assigned to OUT audio mode!", nam->nam_TerminalID);
                        }
                        nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
                    }
                }
                break;
            }

            case UDST_AUDIO_CTRL_MIXER_UNIT:
            {
                struct UsbAudioMixerUnitDesc10 *uamu = (struct UsbAudioMixerUnitDesc10 *) descdata;
                UWORD inputpins = uamu->bNrInPins;
                UWORD outchannels = descdata[5+inputpins];
                UWORD channelcfg = descdata[6+inputpins]|(descdata[7+inputpins]<<8);

                nau->nau_OutChannels = outchannels;
                nau->nau_ChannelCfg = channelcfg;
                break;
            }

            case UDST_AUDIO_CTRL_PROCESSING_UNIT:
            case UDST_AUDIO_CTRL_EXTENSION_UNIT:
            {
                struct UsbAudioProcessingUnitDesc10 *uapu = (struct UsbAudioProcessingUnitDesc10 *) descdata;
                UWORD inputpins = uapu->bNrInPins;
                UWORD outchannels = descdata[7+inputpins];
                UWORD channelcfg = descdata[8+inputpins]|(descdata[9+inputpins]<<8);

                nau->nau_OutChannels = outchannels;
                nau->nau_ChannelCfg = channelcfg;
                break;
            }
        }
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ;
    }

    KPRINTF(10, ("ExamineAudioDesc Stage 4\n"));

    // recurse over output terminals, flow up the chain
    nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    while(nau->nau_Node.ln_Succ)
    {
        descdata = nau->nau_Descriptor;
        if(nau->nau_Type == UDST_AUDIO_CTRL_OUTPUT_TERMINAL)
        {
            nFlowUp(nch, nau);
            nau->nau_SourceUnit = nFlowUpToUSBSource(nch, nau);
        }
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ;
    }

    // recurse over input terminals, flow down the chain
    nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    while(nau->nau_Node.ln_Succ)
    {
        descdata = nau->nau_Descriptor;
        if(nau->nau_Type == UDST_AUDIO_CTRL_INPUT_TERMINAL)
        {
            UWORD cnt;
            for(cnt = 0; cnt < nau->nau_NumOutputs; cnt++)
            {
                nFlowDown(nch, nau->nau_OutputUnit[cnt]);
            }
            nau->nau_SinkUnit = nFlowDownToUSBSink(nch, nau);
        }
        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ;
    }

    KPRINTF(10, ("ExamineAudioDesc Stage 5\n"));

    // do the some more magic
    nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    while(nau->nau_Node.ln_Succ)
    {
        descdata = nau->nau_Descriptor;
        switch(nau->nau_Type)
        {
            case UDST_AUDIO_CTRL_MIXER_UNIT:
            {
                struct UsbAudioMixerUnitDesc10 *uamu = (struct UsbAudioMixerUnitDesc10 *) descdata;
                UWORD inputpins = uamu->bNrInPins;
                //UWORD outchannels = descdata[5+inputpins];
                UWORD channelcfg = descdata[6+inputpins]|(descdata[7+inputpins]<<8);
                STRPTR spacial = nConcatBitsStr(nch, NTS_SPATIALLOCATION, (ULONG) channelcfg);

                /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Mixer Unit %02lx with %ld input pins and %ld output channels (%s).",
                               uamu->bUnitID,
                               inputpins,
                               outchannels,
                               spacial);*/
                psdFreeVec(spacial);
                break;
            }

            case UDST_AUDIO_CTRL_SELECTOR_UNIT:
            {
                //struct UsbAudioSelectorUnitDesc10 *uasu = (struct UsbAudioSelectorUnitDesc10 *) descdata;
                //UWORD inputpins = uasu->bNrInPins;
                //UWORD cnt;

                /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Selector Unit %02lx with %ld input pins.",
                               uasu->bUnitID,
                               inputpins);*/

                if(nau->nau_SinkUnit)
                {
                    KPRINTF(10, ("Assigning Selector to Unit %02lx", nau->nau_SinkUnit->nau_UnitID));
                    nau->nau_SinkUnit->nau_SelectorUnit = nau;
                } else {
                    KPRINTF(10, ("Error: No Sink unit!\n"));
                }

                /*nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
                while(nam->nam_Node.ln_Succ)
                {
                    if(nam->nam_TerminalID == nau->nau_RootUnit->nau_UnitID)
                    {
                        KPRINTF(10, ("Terminal ID %02lx matched\n", nam->nam_TerminalID));
                        if(nau->nau_RootUnit->nau_Type == UDST_AUDIO_CTRL_INPUT_TERMINAL)
                        {
                            for(cnt = 0; cnt < nau->nau_NumInputs; cnt++)
                            {
                                if(nam->nam_NumOutputs < 8)
                                {
                                    KPRINTF(10, ("Adding output selector %s\n", nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_InputUnit[cnt]->nau_TermType, "Unknown")));
                                    nam->nam_OutputNames[nam->nam_NumOutputs++] = nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_InputUnit[cnt]->nau_TermType, "Unknown");
                                }
                            }
                        } else {
                            for(cnt = 0; cnt < nau->nau_NumInputs; cnt++)
                            {
                                if(nam->nam_NumInputs < 8)
                                {
                                    KPRINTF(10, ("Adding input selector %s\n", nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_InputUnit[cnt]->nau_TermType, "Unknown")));
                                    nam->nam_InputNames[nam->nam_NumInputs++] = nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_InputUnit[cnt]->nau_TermType, "Unknown");
                                }
                            }
                        }
                    }
                    nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
                }*/
                break;
            }

            case UDST_AUDIO_CTRL_FEATURE_UNIT:
            {
                UWORD featsize = descdata[5];
                UWORD feat;
                UWORD masterfeat = 0;
                UWORD leftfeat = 0;
                UWORD rightfeat = 0;
                UBYTE *featptr = descdata + 6;
                UWORD featsizecnt;
                UWORD cnt;
                UWORD firstchan = 0;

                nau->nau_TermType = nau->nau_InputUnit[0]->nau_TermType;
                nau->nau_OutChannels = nau->nau_InputUnit[0]->nau_OutChannels;
                nau->nau_ChannelCfg = nau->nau_InputUnit[0]->nau_ChannelCfg;

                KPRINTF(10, ("Feature Unit %ld, Root unit %ld\n", nau->nau_UnitID, nau->nau_RootUnit->nau_UnitID));

                for(cnt = 0; cnt <= nau->nau_InputUnit[0]->nau_OutChannels; cnt++)
                {
                    feat = 0;
                    for(featsizecnt = 0; featsizecnt < featsize; featsizecnt++)
                    {
                        feat |= (*featptr++)<<(featsizecnt<<3);
                    }
                    if(cnt == 0)
                    {
                        masterfeat = feat;
                    }
                    else if(cnt == 1)
                    {
                        leftfeat = feat;
                    }
                    else if(cnt == 2)
                    {
                        rightfeat = feat;
                    }
                }

                if((masterfeat & UAFUF_VOLUME) ||
                   ((nau->nau_OutChannels == 1) && (leftfeat & UAFUF_VOLUME)) ||
                   (leftfeat & rightfeat & UAFUF_VOLUME))
                {
                    KPRINTF(10, ("Master volume control supported\n"));
                    if(nau->nau_SourceUnit)
                    {
                        /* on a playback path: which Feature Units set the output
                           volume is decided per mode, from all of them (nMapOutputs) */
                        KPRINTF(10, ("Volume roots from USB input streaming\n"));
                    }
                    else if(nau->nau_SinkUnit)
                    {
                        KPRINTF(10, ("Flows to USB stream, assuming input gain!\n"));
                        nau->nau_RootUnit->nau_VolumeControl = TRUE;
                        nau->nau_RootUnit->nau_VolumeUnit = nau;
                        if(leftfeat & rightfeat & UAFUF_VOLUME)
                        {
                            KPRINTF(10, ("Input Balance control supported\n"));
                            nau->nau_RootUnit->nau_Balance = TRUE;
                        }
                    } else {
                        KPRINTF(10, ("Flows to something else, assuming monitor!\n"));
                        nau->nau_RootUnit->nau_Monitor = TRUE;
                        nau->nau_RootUnit->nau_MonitorUnit = nau;
                    }
                }
                if(rightfeat & UAFUF_VOLUME)
                {
                    firstchan = FU_CHAN_RIGHT;
                    nau->nau_VolCtrlMask |= FU_CHANBIT(FU_CHAN_RIGHT);
                    nau->nau_VolCtrlCount++;
                }
                if(leftfeat & UAFUF_VOLUME)
                {
                    firstchan = FU_CHAN_LEFT;
                    nau->nau_VolCtrlMask |= FU_CHANBIT(FU_CHAN_LEFT);
                    nau->nau_VolCtrlCount++;
                }
                if((masterfeat & UAFUF_VOLUME))
                {
                    firstchan = FU_CHAN_MASTER;
                    nau->nau_VolCtrlMask |= FU_CHANBIT(FU_CHAN_MASTER);
                    nau->nau_VolCtrlCount++;
                }
                /* Mute Controls, the same bit per channel */
                nau->nau_MuteCtrlMask = ((masterfeat & UAFUF_MUTE) ? FU_CHANBIT(FU_CHAN_MASTER) : 0) |
                                        ((leftfeat & UAFUF_MUTE) ? FU_CHANBIT(FU_CHAN_LEFT) : 0) |
                                        ((rightfeat & UAFUF_MUTE) ? FU_CHANBIT(FU_CHAN_RIGHT) : 0);
                KPRINTF(10, ("Volume Feature Mask %04lx\n", nau->nau_VolCtrlMask));

                if(nau->nau_VolCtrlCount)
                {
                    UBYTE buf[2];
                    LONG ioerr;
                    psdPipeSetup(nch->nch_EP0Pipe, URTF_IN|URTF_CLASS|URTF_INTERFACE, UAUDR_GET_MIN, (ULONG) (UAFUCS_VOLUME<<8)|firstchan, (nau->nau_UnitID<<8)|nch->nch_IfNum);
                    ioerr = psdDoPipe(nch->nch_EP0Pipe, buf, 2);
                    if(ioerr)
                    {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                       "Could not get min volume: %s (%ld)!",
                                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    } else {
                        KPRINTF(10, ("Minimum volume: %04lx\n", buf[0]|(buf[1]<<8)));
                        nau->nau_MinVolDb = (WORD) (buf[0]|(buf[1]<<8));
                    }
                    psdPipeSetup(nch->nch_EP0Pipe, URTF_IN|URTF_CLASS|URTF_INTERFACE, UAUDR_GET_MAX, (ULONG) (UAFUCS_VOLUME<<8)|firstchan, (nau->nau_UnitID<<8)|nch->nch_IfNum);
                    ioerr = psdDoPipe(nch->nch_EP0Pipe, buf, 2);
                    if(ioerr)
                    {
                        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                                       "Could not get max volume: %s (%ld)!",
                                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
                    } else {
                        KPRINTF(10, ("Maximum volume: %04lx\n", buf[0]|(buf[1]<<8)));
                        nau->nau_MaxVolDb = (WORD) (buf[0]|(buf[1]<<8));
                    }
                    nFindLogVolume(nch, nau);
                }
                break;
            }
        }
        /*psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "%s Unit (%s) ID=%02lx, In/Out=%ld/%ld, Chans=%ld, Cfg=%ld, TermType=%s, Root=%02lx, Source=%02lx, Sink=%02lx",
                       nNumToStr(nch, NTS_UNITTYPE, (ULONG) nau->nau_Type, "Unknown"),
                       nau->nau_Name,
                       nau->nau_UnitID,
                       nau->nau_NumInputs,
                       nau->nau_NumOutputs,
                       nau->nau_OutChannels,
                       nau->nau_ChannelCfg,
                       nNumToStr(nch, NTS_TERMINALTYPE, (ULONG) nau->nau_TermType, "Unknown"),
                       nau->nau_RootUnit->nau_UnitID,
                       nau->nau_SourceUnit ? nau->nau_SourceUnit->nau_UnitID : 0,
                       nau->nau_SinkUnit ? nau->nau_SinkUnit->nau_UnitID : 0);*/

        nau = (struct NepAudioUnit *) nau->nau_Node.ln_Succ;
    }

    /* the terminals each mode's stream reaches, by name, for AHI (nInputName, nOutputName) */
    nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
    while(nam->nam_Node.ln_Succ)
    {
        if(nam->nam_IsInput)
        {
            /* if the tagging found none, every input terminal - a mode keeps names either way */
            nam->nam_NumInputs = nInputNames(nch, nam, FALSE);
            if(!nam->nam_NumInputs)
            {
                nam->nam_NumInputs = nInputNames(nch, nam, TRUE);
            }
        } else {
            nMapOutputs(nch, nam);
        }
        nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
    }

    // pair recording streams with playback modes (the modes outlive a binding: what is paired stays paired)
    nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
    while(nam->nam_Node.ln_Succ)
    {
        if(nam->nam_IsInput && !nam->nam_PartOf)
        {
            KPRINTF(10, ("Trying to find matching record mode for %ld channel(s) and %ld bits.\n", nam->nam_NumChannels, nam->nam_Resolution));
            struct NepAudioMode *playmode = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;

            while(playmode->nam_Node.ln_Succ)
            {
                if(!playmode->nam_IsInput)
                {
                    if((playmode->nam_Resolution == nam->nam_Resolution) &&
                       (playmode->nam_NumChannels == nam->nam_NumChannels) &&
                       (playmode->nam_FrameSize == nam->nam_FrameSize) &&
                       (playmode->nam_NumFrequencies == nam->nam_NumFrequencies) &&
                       (playmode->nam_MinFreq == nam->nam_MinFreq) &&
                       (playmode->nam_MaxFreq == nam->nam_MaxFreq) &&
                       (playmode->nam_FreqArray[0] == nam->nam_FreqArray[0]))
                    {
                        KPRINTF(10, ("Recording mode with %ld channel(s) and %ld bits matched.\n", nam->nam_NumChannels, nam->nam_Resolution));
                        if(playmode->nam_RecStream)
                        {
                            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Matching playback mode with %ld channel(s) and %ld bits is already occupied.",
                                           playmode->nam_NumChannels,
                                           playmode->nam_Resolution);
                        } else {
                            nam->nam_PartOf = playmode;
                            playmode->nam_RecStream = nam;
                            break;
                        }
                    }
                }
                playmode = (struct NepAudioMode *) playmode->nam_Node.ln_Succ;
            }
            if(!playmode->nam_Node.ln_Succ)
            {
                KPRINTF(10, ("Recording mode with %ld channel(s) and %ld bits could not be matched to an input mode.\n", nam->nam_NumChannels, nam->nam_Resolution));
                playmode = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
                while(playmode->nam_Node.ln_Succ)
                {
                    if(!playmode->nam_IsInput)
                    {
                        if((playmode->nam_NumFrequencies == nam->nam_NumFrequencies) &&
                           (playmode->nam_MinFreq == nam->nam_MinFreq) &&
                           (playmode->nam_MaxFreq == nam->nam_MaxFreq) &&
                           (playmode->nam_FreqArray[0] == nam->nam_FreqArray[0]))
                        {
                            KPRINTF(10, ("Recording mode partially matched.\n"));
                            if(playmode->nam_RecStream)
                            {
                                psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Partially matching playback mode with %ld channel(s) and %ld bits is already occupied.",
                                               playmode->nam_NumChannels,
                                               playmode->nam_Resolution);
                            } else {
                                nam->nam_PartOf = playmode;
                                playmode->nam_RecStream = nam;
                                break;
                            }
                        }
                    }
                    playmode = (struct NepAudioMode *) playmode->nam_Node.ln_Succ;
                }
                if(playmode->nam_Node.ln_Succ)
                {
                    psdAddErrorMsg(RETURN_WARN, (STRPTR) libname, "Recording mode with %ld ch@%ldb was assigned to %ld ch@%ldb output mode.",
                                   nam->nam_NumChannels,
                                   nam->nam_Resolution,
                                   playmode->nam_NumChannels,
                                   playmode->nam_Resolution);
                }
            }
        }
        nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
    }

    /* Recording modes that are left over go with no playback mode: each is an
       AHI mode of its own. Those of one interface that differ in nothing but
       the alternate setting - a camera has one per sample rate - are one mode,
       the first of them, with the rates of all; the others hang on to it as
       its alternates (nAltForFreq). */
    nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
    while(nam->nam_Node.ln_Succ)
    {
        if(nam->nam_IsInput && !nam->nam_PartOf)
        {
            ULONG had = nam->nam_NumFrequencies;
            struct NepAudioMode *alt = (struct NepAudioMode *) nam->nam_Node.ln_Succ;

            while(alt->nam_Node.ln_Succ)
            {
                if(alt->nam_IsInput && !alt->nam_PartOf &&
                   (alt->nam_IfNum == nam->nam_IfNum) &&
                   (alt->nam_NumChannels == nam->nam_NumChannels) &&
                   (alt->nam_SampleSize == nam->nam_SampleSize) &&
                   (alt->nam_Resolution == nam->nam_Resolution))
                {
                    for(ULONG cnt = 0; cnt < alt->nam_NumFrequencies; cnt++)
                    {
                        ULONG freq = alt->nam_FreqArray[cnt];
                        ULONG have;

                        /* a rate that is there already is listed once */
                        for(have = 0; (have < nam->nam_NumFrequencies) && (nam->nam_FreqArray[have] != freq); have++);
                        if((have == nam->nam_NumFrequencies) && (have < 64))
                        {
                            nam->nam_FreqArray[nam->nam_NumFrequencies++] = freq;
                            nam->nam_MinFreq = (freq < nam->nam_MinFreq) ? freq : nam->nam_MinFreq;
                            nam->nam_MaxFreq = (freq > nam->nam_MaxFreq) ? freq : nam->nam_MaxFreq;
                        }
                    }
                    alt->nam_PartOf = nam;
                }
                alt = (struct NepAudioMode *) alt->nam_Node.ln_Succ;
            }
            if(nam->nam_NumFrequencies != had)
            {
                qsort(nam->nam_FreqArray,
                      nam->nam_NumFrequencies,
                      sizeof(nam->nam_FreqArray[0]),
                      compare_frequencies);
            }
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Recording with %ld channel(s) and %ld bits goes with no output mode: an AHI mode of its own, %ld rate(s).",
                           nam->nam_NumChannels,
                           nam->nam_Resolution,
                           nam->nam_NumFrequencies);
        }
        nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
    }

    KPRINTF(10, ("ExamineAudioUnits finished\n"));
}
/* \\\ */

/* /// "nAddAudioModes()" */
void nAddAudioModes(struct NepClassAudio *nch)
{
    struct NepAudioMode *nam;
    STRPTR devname;
    STRPTR ahimodename;

    psdGetAttrs(PGA_DEVICE, nch->nch_Device, DA_ProductName, &devname, TAG_END);
    nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
    while(nam->nam_Node.ln_Succ)
    {
        /* every playback stream, and every recording stream that goes with none */
        if(!nam->nam_IsInput || !nam->nam_PartOf)
        {
            struct TagItem *tags = nam->nam_Tags;
            if(nam->nam_IsInput)
            {
                ahimodename = psdCopyStrFmt("%.20s: Rec %ld bit %s (%ld bpf)",
                                            devname,
                                            nam->nam_Resolution,
                                            (nam->nam_NumChannels > 1) ? "stereo" : "mono",
                                            nam->nam_FrameSize);
            } else {
                ahimodename = psdCopyStrFmt("%.20s: %s%ld bit %s (%ld bpf)",
                                            devname,
                                            (nam->nam_Resolution > 16) ? "HiFi " : "",
                                            nam->nam_Resolution,
                                            (nam->nam_NumChannels > 1) ? "stereo++" : "mono",
                                            nam->nam_FrameSize);
            }

            KPRINTF(10, ("AHI Mode %s NumInput=%ld, NumOutput=%ld, Rec=%08lx\n",
                         ahimodename, nam->nam_NumInputs, nam->nam_NumOutputs, nam->nam_RecStream));

            // AHI samples are always 16 bit, except for HiFi mode, which returns 32 bit
            nam->nam_AHIFrameSize = nam->nam_NumChannels * ((nam->nam_Resolution > 16) ? 4 : 2);

            tags->ti_Tag = AHIDB_AudioID;
            tags->ti_Data = nam->nam_AHIModeID;
            tags++;
            tags->ti_Tag = AHIDB_Name;
            tags->ti_Data = (IPTR) ahimodename;
            tags++;
            tags->ti_Tag = AHIDB_Driver;
            tags->ti_Data = (IPTR) "usbaudio";
            tags++;
            /*tags->ti_Tag = AHIDB_DriverBaseName;
            tags->ti_Data = (ULONG) "";
            tags++;*/
            tags->ti_Tag = AHIDB_Volume;
            tags->ti_Data = nOutVolumeUnit(nam) ? TRUE : FALSE;
            tags++;
            tags->ti_Tag = AHIDB_Panning;
            tags->ti_Data = (nam->nam_NumChannels > 1) ? TRUE : FALSE;
            tags++;
            tags->ti_Tag = AHIDB_Stereo;
            tags->ti_Data = (nam->nam_NumChannels > 1) ? TRUE : FALSE;
            tags++;
            tags->ti_Tag = AHIDB_HiFi;
            tags->ti_Data = (nam->nam_Resolution > 16) ? TRUE : FALSE;
            tags++;
            tags->ti_Tag = AHIDB_MultTable;
            tags->ti_Data = FALSE;
            tags++;
            tags->ti_Tag = AHIDB_NepAudioMode;
            tags->ti_Data = (IPTR) nam;
            tags++;
            tags->ti_Tag = TAG_END;
            AHI_AddAudioMode(nam->nam_Tags);
            psdFreeVec(ahimodename);
        }
        nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
    }
}
/* \\\ */

/* /// "nAllocAudio()" */
struct NepClassAudio * nAllocAudio(void)
{
    struct Task *thistask;
    struct NepClassAudio *nch;

    thistask = FindTask(NULL);
    nch = thistask->tc_UserData;
    do
    {
        if(!(nch->nch_Base = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
        {
            Alert(AG_OpenLib);
            break;
        }

        if(!(nch->nch_AHIMsgPort = CreateMsgPort()))
        {
            break;
        }

        if(!(nch->nch_AHIReq = (struct AHIRequest *) CreateIORequest(nch->nch_AHIMsgPort, sizeof(struct AHIRequest))))
        {
            break;
        }

        //if(OpenDevice("ahi.device", AHI_NO_UNIT, (struct IORequest *) nch->nch_AHIReq, AHIDF_NOMODESCAN))
        if(OpenDevice("ahi.device", AHI_NO_UNIT, (struct IORequest *) nch->nch_AHIReq, 0))
        {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "Unable to open AHI device");
            break;
        }

        AHIBase = (struct Library *) nch->nch_AHIReq->ahir_Std.io_Device;

        psdGetAttrs(PGA_INTERFACE, nch->nch_Interface,
                    IFA_Config, &nch->nch_Config,
                    IFA_InterfaceNum, &nch->nch_IfNum,
                    TAG_END);
        psdGetAttrs(PGA_CONFIG, nch->nch_Config,
                    CA_Device, &nch->nch_Device,
                    TAG_END);

        if(!nch->nch_AudioModes.lh_Head->ln_Succ)
        {
            nExamineAudioDescriptors(nch);
        }
        if(nch->nch_AudioModes.lh_Head->ln_Succ)
        {
            if((nch->nch_TaskMsgPort = CreateMsgPort()))
            {
                if((nch->nch_EP0Pipe = psdAllocPipe(nch->nch_Device, nch->nch_TaskMsgPort, NULL)))
                {
                    nExamineAudioUnits(nch);
                    nAddAudioModes(nch);

                    nch->nch_Task = thistask;
                    return(nch);
                }
                DeleteMsgPort(nch->nch_TaskMsgPort);
                nch->nch_TaskMsgPort = NULL;
            }
        } else {
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "No suitable audio modes found for this device.");
        }
    } while(FALSE);
    if(AHIBase)
    {
        CloseDevice((struct IORequest *) nch->nch_AHIReq);
        AHIBase = NULL;
    }
    DeleteIORequest((struct IORequest *) nch->nch_AHIReq);
    nch->nch_AHIReq = NULL;
    DeleteMsgPort(nch->nch_AHIMsgPort);
    nch->nch_AHIMsgPort = NULL;
    CloseLibrary(nch->nch_Base);
    nch->nch_Base = NULL;
    Forbid();
    nch->nch_Task = NULL;
    if(nch->nch_ReadySigTask)
    {
        Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
    }
    return(NULL);
}
/* \\\ */

/* /// "nFreeAudio()" */
void nFreeAudio(struct NepClassAudio *nch)
{
    struct NepAudioMode *nam;
    struct NepAudioUnit *nau;

    nam = (struct NepAudioMode *) nch->nch_AudioModes.lh_Head;
    while(nam->nam_Node.ln_Succ)
    {
        if(nam->nam_AHIModeID)
        {
            AHI_RemoveAudioMode(nam->nam_AHIModeID);
        }
        nam = (struct NepAudioMode *) nam->nam_Node.ln_Succ;
    }
    nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    while(nau->nau_Node.ln_Succ)
    {
        Remove((struct Node *) nau);
        psdFreeVec(nau->nau_Name);
        psdFreeVec(nau);
        nau = (struct NepAudioUnit *) nch->nch_AudioUnits.lh_Head;
    }
    psdFreePipe(nch->nch_EP0Pipe);
    DeleteMsgPort(nch->nch_TaskMsgPort);
    nch->nch_TaskMsgPort = NULL;

    CloseDevice((struct IORequest *) nch->nch_AHIReq);
    AHIBase = NULL;
    DeleteIORequest((struct IORequest *) nch->nch_AHIReq);
    nch->nch_AHIReq = NULL;
    DeleteMsgPort(nch->nch_AHIMsgPort);
    nch->nch_AHIMsgPort = NULL;
    CloseLibrary(nch->nch_Base);
    nch->nch_Base = NULL;
    Forbid();
    nch->nch_Task = NULL;
    if(nch->nch_ReadySigTask)
    {
        Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
    }
}
/* \\\ */

/**************************************************************************/

#undef  ps

/* /// "nLoadClassConfig()" */
BOOL nLoadClassConfig(struct NepAudioBase *nh)
{
    struct Library *ps;
    struct ClsGlobalCfg *cgc;
    struct PsdIFFContext *pic;

    KPRINTF(10, ("Loading Class Config...\n"));
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }
    Forbid();
    nh->nh_UsingDefaultCfg = TRUE;
    pic = psdGetClsCfg(libname);
    if(pic)
    {
        if((cgc = psdGetCfgChunk(pic, AROS_LONG2BE(nh->nh_CurrentCGC.cgc_ChunkID))))
        {
            CopyMem(((UBYTE *) cgc) + 8, ((UBYTE *) &nh->nh_CurrentCGC) + 8, min(AROS_LONG2BE(cgc->cgc_Length), AROS_LONG2BE(nh->nh_CurrentCGC.cgc_Length)));
            psdFreeVec(cgc);
            nh->nh_UsingDefaultCfg = FALSE;
        }
    }
    Permit();
    CloseLibrary(ps);
    return(FALSE);
}
/* \\\ */

/* /// "nOpenCfgWindow()" */
LONG nOpenCfgWindow(struct NepAudioBase *nh)
{
    struct Library *ps;
    KPRINTF(10, ("Opening GUI...\n"));
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return(FALSE);
    }
    Forbid();
    if(!nh->nh_GUITask)
    {
        if((nh->nh_GUITask = psdSpawnSubTask(CLASS_NAME " GUI", nGUITask, nh)))
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

/* /// "nGUITask()" */
void nGUITask()
{
    struct Task *thistask;
    struct NepAudioBase *nh;
    APTR pic;

    thistask = FindTask(NULL);

#define ps nh->nh_PsdBase
#undef IntuitionBase
#define IntuitionBase nh->nh_IntBase
#undef MUIMasterBase
#define MUIMasterBase nh->nh_MUIBase

    nh = thistask->tc_UserData;
    ++nh->nh_Library.lib_OpenCnt;
    if(!(MUIMasterBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN)))
    {
        KPRINTF(10, ("Couldn't open muimaster.library.\n"));
        nGUITaskCleanup(nh);
        return;
    }

    if(!(IntuitionBase = OpenLibrary("intuition.library", 39)))
    {
        KPRINTF(10, ("Couldn't open intuition.library.\n"));
        nGUITaskCleanup(nh);
        return;
    }
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        KPRINTF(10, ("Couldn't open neptune.library.\n"));
        nGUITaskCleanup(nh);
        return;
    }

    nh->nh_App = ApplicationObject,
        MUIA_Application_Title      , (IPTR)libname,
        MUIA_Application_Version    , (IPTR)VERSION_STRING,
        MUIA_Application_Copyright  , (IPTR)"(C) 2008-2009 Chris Hodges",
        MUIA_Application_Author     , (IPTR)"Chris Hodges <chrisly@platon42.de>",
        MUIA_Application_Description, (IPTR)"Settings for the usbaudio.class",
        MUIA_Application_Base       , (IPTR)"USBAUDIO",
        MUIA_Application_HelpFile   , (IPTR)"HELP:Neptune.guide",
        MUIA_Application_Menustrip  , (IPTR)MenustripObject,
            Child, (IPTR)MenuObjectT((IPTR)"Project"),
                Child, (IPTR)(nh->nh_AboutMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"About...",
                    MUIA_Menuitem_Shortcut, (IPTR)"?",
                    End),
                End,
            Child, (IPTR)MenuObjectT((IPTR)"Settings"),
                Child, (IPTR)(nh->nh_UseMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"Save",
                    MUIA_Menuitem_Shortcut, (IPTR)"S",
                    End),
                Child, (IPTR)MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)NM_BARLABEL,
                    End,
                Child, (IPTR)(nh->nh_MUIPrefsMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"MUI Settings",
                    MUIA_Menuitem_Shortcut, (IPTR)"M",
                    End),
                End,
            End,

        SubWindow, (IPTR)(nh->nh_MainWindow = WindowObject,
            MUIA_Window_ID   , MAKE_ID('M','A','I','N'),
            MUIA_Window_Title, (IPTR)libname,
            MUIA_HelpNode, (IPTR)libname,

            WindowContents, (IPTR)VGroup,
                Child, (IPTR)ColGroup(2), (IPTR)GroupFrameT((IPTR)"Global Settings"),
                    Child, (IPTR)Label((IPTR) "None"),
                    Child, (IPTR)HSpace(0),
                    End,
                Child, (IPTR)VSpace(0),
                Child, (IPTR)HGroup,
                    MUIA_Group_SameWidth, TRUE,
                    Child, (IPTR)(nh->nh_UseObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Save ",
                        End),
                    Child, (IPTR)(nh->nh_CloseObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Use ",
                        End),
                    End,
                End,
            End),
        End;

    if(!nh->nh_App)
    {
        KPRINTF(10, ("Couldn't create application\n"));
        nGUITaskCleanup(nh);
        return;
    }
    DoMethod(nh->nh_MainWindow, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             nh->nh_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(nh->nh_UseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             nh->nh_App, 2, MUIM_Application_ReturnID, ID_STORE_CONFIG);
    DoMethod(nh->nh_CloseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             nh->nh_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

    DoMethod(nh->nh_AboutMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             nh->nh_App, 2, MUIM_Application_ReturnID, ID_ABOUT);
    DoMethod(nh->nh_UseMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             nh->nh_App, 2, MUIM_Application_ReturnID, ID_STORE_CONFIG);
    DoMethod(nh->nh_MUIPrefsMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             nh->nh_App, 2, MUIM_Application_OpenConfigWindow, 0);

    {
        IPTR  isopen = 0;
        IPTR  iconify = 0;
        ULONG sigs;
        ULONG sigmask;
        LONG retid;

        get(nh->nh_App, MUIA_Application_Iconified, &iconify);
        set(nh->nh_MainWindow, MUIA_Window_Open, TRUE);
        get(nh->nh_MainWindow, MUIA_Window_Open, &isopen);
        if(!(isopen || iconify))
        {
            nGUITaskCleanup(nh);
            return;
        }
        sigmask = 0;
        do
        {
            retid = DoMethod(nh->nh_App, MUIM_Application_NewInput, &sigs);
            switch(retid)
            {
                case ID_STORE_CONFIG:
                case MUIV_Application_ReturnID_Quit:
                    pic = psdGetClsCfg(libname);
                    if(!pic)
                    {
                        psdSetClsCfg(libname, NULL);
                        pic = psdGetClsCfg(libname);
                    }
                    if(pic)
                    {
                        if(psdAddCfgEntry(pic, &nh->nh_CurrentCGC))
                        {
                            if(retid != MUIV_Application_ReturnID_Quit)
                            {
                                psdSaveCfgToDisk(NULL);
                            }
                            retid = MUIV_Application_ReturnID_Quit;
                        }
                    }
                    break;

               case ID_ABOUT:
                    NEP_CLASS_ABOUT(nh->nh_App, nh->nh_MainWindow);
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
        set(nh->nh_MainWindow, MUIA_Window_Open, FALSE);
    }
    nGUITaskCleanup(nh);
    
}
/* \\\ */

/* /// "nGUITaskCleanup()" */
void nGUITaskCleanup(struct NepAudioBase *nh)
{
    if(nh->nh_App)
    {
        MUI_DisposeObject(nh->nh_App);
        nh->nh_App = NULL;
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
    nh->nh_GUITask = NULL;
    --nh->nh_Library.lib_OpenCnt;
}
/* \\\ */

/**************************************************************************/

#undef UtilityBase
#undef ps
#define ps nam->nam_PsdBase

/* /// "subLib stuff" */
struct NepAudioSubLibBase * subLibInit(struct NepAudioSubLibBase * nas asm("d0"), BPTR seglist asm("a0"), struct ExecBase * SysBase asm("a6"))
{

    KPRINTF(10, ("subLibInit base: 0x%08lx seglist: 0x%08lx SysBase: 0x%08lx\n",
                 nas, seglist, SysBase));

    nas->nas_Library.lib_Node.ln_Type = NT_LIBRARY;
    nas->nas_Library.lib_Node.ln_Name = SUBLIBNAME;
    nas->nas_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    nas->nas_Library.lib_Version      = AHI_SUB_LIB_VERSION;
    nas->nas_Library.lib_Revision     = NEPTUNE_REVISION;
    nas->nas_Library.lib_IdString     = VERSION_STRING;

    /* Store segment */
    nas->nas_SegList = seglist;

#define	UtilityBase	nas->nas_UtilityBase
    if((nas->nas_UtilityBase = OpenLibrary("utility.library", 0)))
    {
        KPRINTF(10, ("subLibInit: Ok\n"));
        KPRINTF(10, ("subLibInit: openCnt = %ld\n", nas->nas_Library.lib_OpenCnt));
        return(nas);
    }
    else
    {
        return(NULL);
    }

    return(nas);
    
}

struct NepAudioSubLibBase * (subLibOpen)(ULONG version asm("d0"), struct NepAudioSubLibBase * nas asm("a6"))
{

    //struct NepAudioBase *nh = nas->nas_ClsBase;

    KPRINTF(10, ("subLibOpen base: 0x%08lx\n", nas));

    ++nas->nas_Library.lib_OpenCnt;
    nas->nas_Library.lib_Flags &= ~LIBF_DELEXP;

    KPRINTF(10, ("subLibOpen: openCnt = %ld\n", nas->nas_Library.lib_OpenCnt));

    return(nas);
    
}

BPTR (subLibClose)(struct NepAudioSubLibBase * nas asm("a6"))
{

    BPTR ret;

    KPRINTF(10, ("subLibClose base: 0x%08lx\n", nas));

    ret = BNULL;

    if(--nas->nas_Library.lib_OpenCnt == 0)
    {
        if(nas->nas_Library.lib_Flags & LIBF_DELEXP)
        {
            KPRINTF(5, ("subLibClose: calling expunge...\n"));
            ret = subLibExpunge(nas, nas);
        }
    }

    KPRINTF(5, ("subLibClose: lib_OpenCnt = %ld\n", nas->nas_Library.lib_OpenCnt));

    return(ret);
    
}

BPTR (subLibExpunge)(struct NepAudioSubLibBase * extralh asm("d0"), struct NepAudioSubLibBase * nas asm("a6"))
{
    
    BPTR ret;

    KPRINTF(10, ("subLibExpunge base: 0x%08lx\n", nas));

    ret = BNULL;

    if(nas->nas_Library.lib_OpenCnt == 0)
    {
        KPRINTF(5, ("subLibExpunge: Unloading...\n"));

        CloseLibrary(nas->nas_UtilityBase);

        ret = nas->nas_SegList;

        KPRINTF(5, ("subLibExpunge: removing library node 0x%08lx\n",
                    &nas->nas_Library.lib_Node));
        Remove(&nas->nas_Library.lib_Node);

        KPRINTF(5, ("subLibExpunge: FreeMem()...\n"));
        FreeMem((char *) nas - nas->nas_Library.lib_NegSize,
                (ULONG) (nas->nas_Library.lib_NegSize + nas->nas_Library.lib_PosSize));

        KPRINTF(5, ("subLibExpunge: Unloading done! usbaudio.audio expunged!\n\n"));

        return(ret);
    }
    else
    {
        KPRINTF(5, ("subLibExpunge: Could not expunge, LIBF_DELEXP set!\n"));
        nas->nas_Library.lib_Flags |= LIBF_DELEXP;
    }

    return(BNULL);
    
}

struct NepAudioSubLibBase * (subLibReserved)(struct NepAudioSubLibBase * nas asm("a6"))
{
    return NULL;
}
/* \\\ */

/* /// "nMixAndConvert()" */
/* One player interval: AHI mixes into nam_MixBuffer, the conversion hook
   writes it into the fill buffer in the device's format, and that buffer is
   the next to go to the device (nOutReqHook). */
static void nMixAndConvert(struct NepAudioMode *nam)
{
    struct NepAudioSubLibBase *nas = nam->nam_SubLibBase;
    struct AHIAudioCtrlDrv *audioctrl = nam->nam_AudioCtrl;
    UWORD fill = nam->nam_FillBuf;

    CallHookPkt(audioctrl->ahiac_MixerFunc, audioctrl, nam->nam_MixBuffer);
    CallHookPkt(&nam->nam_SamConvHook, nam->nam_PlayBuffer[fill], (APTR) (IPTR) audioctrl->ahiac_BuffSamples);
    nam->nam_SendBuf = fill;
    nam->nam_FillBuf = 1 - fill;
}
/* \\\ */

/* /// "subLibPlayerIntV4()" */
ULONG subLibPlayerIntV4(struct NepAudioMode * nam asm("a1"))
{
    
    struct NepAudioSubLibBase *nas = nam->nam_SubLibBase;
    struct AHIAudioCtrlDrv *audioctrl = nam->nam_AudioCtrl;

    CallHookPkt(audioctrl->ahiac_PlayerFunc, nam->nam_AudioCtrl, NULL);
    if(!nam->nam_Unit->nch_DenyRequests)
    {
        nMixAndConvert(nam);
    }
    
    return FALSE;
    
}
/* \\\ */

/* /// "subLibPlayerIntV6()" */
ULONG subLibPlayerIntV6(struct NepAudioMode * nam asm("a1"))
{
    
    struct NepAudioSubLibBase *nas = nam->nam_SubLibBase;
    struct AHIAudioCtrlDrv *audioctrl = nam->nam_AudioCtrl;

    CallHookPkt(audioctrl->ahiac_PlayerFunc, nam->nam_AudioCtrl, NULL);
    BOOL skipit = CallHookPkt(audioctrl->ahiac_PreTimerFunc, nam->nam_AudioCtrl, NULL);
    if(!(skipit || nam->nam_Unit->nch_DenyRequests))
    {
        nMixAndConvert(nam);
    }
    CallHookPkt(audioctrl->ahiac_PostTimerFunc, nam->nam_AudioCtrl, NULL);

    return FALSE;
    
}
/* \\\ */

/* /// "nPlayerTick()" */
/* The player is called: the milliseconds until its next call
   (nam_PlayerMS), and the frames AHI mixes for them, at most what the
   buffers hold. The parts of a millisecond and of a frame carry over: carry
   is the part frame this pass starts with, *nextcarry gets the one the next
   pass starts with. */
static ULONG nPlayerTick(struct NepAudioMode *nam, Fixed carry, Fixed *nextcarry)
{
    nam->nam_PlayerInterval &= 0xffff;
    nam->nam_PlayerInterval += nam->nam_PlayerFrac;
    nam->nam_PlayerMS = nam->nam_PlayerInterval >> 16;

    /* whole and part frames apart: a slow player at a high rate would
       overflow 16.16 */
    ULONG ms = nam->nam_PlayerMS;
    ULONG part = (carry & 0xffff) + (nam->nam_FramesPerMS & 0xffff) * ms;
    ULONG frames = (nam->nam_FramesPerMS >> 16) * ms + (part >> 16);

    *nextcarry = part & 0xffff;
    return (frames > nam->nam_MaxMixFrames) ? nam->nam_MaxMixFrames : frames;
}
/* \\\ */

/* /// "nAbortTimer()" */
/* The timer stops calling the player. Returns whether its request is back
   in the port, to be collected with WaitIO() (or GetMsg() by the player).
   AbortIO() is for a request that was sent at least once (exec.doc); a mode
   that only records, or one unplugged while stopped, has the timer for a
   clock without having sent it, and the player gives the request its
   command when it first sends it. */
static BOOL nAbortTimer(struct NepAudioMode *nam)
{
    nam->nam_TimerMsgPort.mp_Flags = PA_IGNORE;
    if(nam->nam_TimerIOReq->tr_node.io_Command != TR_ADDREQUEST)
    {
        return FALSE;
    }
    AbortIO((struct IORequest *) nam->nam_TimerIOReq);
    return TRUE;
}
/* \\\ */

/* /// "subLibPlayerIntDummy()" */
ULONG subLibPlayerIntDummy(struct NepAudioMode * nam asm("a1"))
{

    struct NepAudioSubLibBase *nas = nam->nam_SubLibBase;
    struct AHIAudioCtrlDrv *audioctrl = nam->nam_AudioCtrl;

    /* the timer request, if it is what called */
    GetMsg(&nam->nam_TimerMsgPort);

    /* The next call is due nam_PlayerMS from now; the request goes out
       before the mixing, which would otherwise stretch every interval.
       nOutReqHook does not run on the timer clock: its send carry keeps
       the part frame of this mix. */
    ULONG frames = nPlayerTick(nam, nam->nam_SendCarry, &nam->nam_SendCarry);

    nam->nam_TimerIOReq->tr_node.io_Command = TR_ADDREQUEST;
    nam->nam_TimerIOReq->tr_time.tv_secs = nam->nam_PlayerMS / 1000;
    nam->nam_TimerIOReq->tr_time.tv_micro = (nam->nam_PlayerMS % 1000) * 1000;
    SendIO((struct IORequest *) nam->nam_TimerIOReq);

    /* what plays until then; nobody hears it */
    audioctrl->ahiac_BuffSamples = frames;
    CallHookPkt(audioctrl->ahiac_PlayerFunc, nam->nam_AudioCtrl, NULL);
    CallHookPkt(audioctrl->ahiac_MixerFunc, nam->nam_AudioCtrl, nam->nam_MixBuffer);

    return FALSE;
    
}
/* \\\ */

/* /// "nOutReqHook()" */

void nOutReqHook(struct Hook * hook asm("a0"), struct IOUsbHWRTIso * urti asm("a2"), struct IOUsbHWBufferReq * ubr asm("a1"))
{

    struct NepAudioMode *nam = (struct NepAudioMode *) hook->h_Data;

    nam->nam_PlayerTimer -= nam->nam_Interval;
    if(nam->nam_PlayerTimer < 0)
    {
        UWORD bufnum = nam->nam_FillBuf;
        /* the other buffer starts with the part frame this one leaves */
        ULONG cnt = nPlayerTick(nam, nam->nam_PlayCarry[bufnum], &nam->nam_PlayCarry[1 - bufnum]);

        nam->nam_PlayerTimer += nam->nam_PlayerMS;
        nam->nam_AudioCtrl->ahiac_BuffSamples = cnt;
        nam->nam_PlayLen[bufnum] = cnt * nam->nam_FrameSize;

        Cause(&nam->nam_PlayerInt);
        /*if(nam->nam_SendLeft)
        {
            KPRINTF(200, ("narf %ld - %ld, %lx, %lx %ld!\n", nam->nam_SendLeft, ubr->ubr_Length, nam->nam_PlayerInterval, nam->nam_SendCarry, nam->nam_PlayerTimer));
        }*/
        nam->nam_SendLeft = 0;
        //KPRINTF(1, ("NT %ld\n", nam->nam_PlayerTimer));
    }
    if(nam->nam_Unit->nch_DenyRequests)
    {
        ubr->ubr_Length = 0;
        return;
    }
 
    if(nam->nam_SendLeft <= 0)
    {
        // switching is done via Mixer Int
        // ubr_Buffer automatically increases
        ubr->ubr_Buffer = nam->nam_PlayBuffer[nam->nam_SendBuf];
        nam->nam_SendCarry = nam->nam_PlayCarry[nam->nam_SendBuf];
        nam->nam_SendLeft = nam->nam_PlayLen[nam->nam_SendBuf];
    }
    nam->nam_SendCarry &= 0xffff;
    nam->nam_SendCarry += nam->nam_FramesPerMS * nam->nam_Interval;
    ubr->ubr_Length = (nam->nam_SendCarry>>16) * nam->nam_FrameSize;
    nam->nam_SendLeft -= ubr->ubr_Length;
    
}
/* \\\ */

/* /// "nRecBlockFrames()" */
/* Frames each record block has room for: the block, and the service
   interval that fills it up arrives whole (nInDoneHook) */
static ULONG nRecBlockFrames(struct NepAudioMode *nam)
{
    return REC_BLOCK_FRAMES + nam->nam_IntervalBytes / nam->nam_FrameSize;
}
/* \\\ */

/* /// "nInReqHook()" */
void nInReqHook(struct Hook * hook asm("a0"), struct IOUsbHWRTIso * urti asm("a2"), struct IOUsbHWBufferReq * ubr asm("a1"))
{
    
    struct NepAudioMode *nam = (struct NepAudioMode *) hook->h_Data;

    /* the packet buffer holds one service interval: more is cut off */
    ubr->ubr_Buffer = nam->nam_PacketBuffer;
    if(ubr->ubr_Length > nam->nam_IntervalBytes)
    {
        ubr->ubr_Length = nam->nam_IntervalBytes;
    }
}
/* \\\ */

/* /// "nInDoneHook()" */
void nInDoneHook(struct Hook * hook asm("a0"), struct IOUsbHWRTIso * urti asm("a2"), struct IOUsbHWBufferReq * ubr asm("a1"))
{
    
    struct NepAudioMode *nam = (struct NepAudioMode *) hook->h_Data;
    struct NepAudioSubLibBase *nas = nam->nam_SubLibBase;

    ULONG cnt = ubr->ubr_Length / nam->nam_FrameSize;

    if(!cnt)
    {
        return;
    }
    /* AHI gets what was recorded in blocks, not interval by interval */
    WORD *block = nam->nam_RecMsg.ahirm_Buffer;
    ULONG have = nam->nam_RecMsg.ahirm_Length;

    /* have < REC_BLOCK_FRAMES, and nInReqHook let in one service interval
       at most: the block has room for it (nRecBlockFrames) */
    CallHookPkt(&nam->nam_SamConvHook, block + have * REC_FRAME_WORDS, (APTR) (IPTR) cnt);
    nam->nam_RecFrames += cnt;
    nam->nam_RecMsg.ahirm_Length = have + cnt;
    if(nam->nam_RecMsg.ahirm_Length >= REC_BLOCK_FRAMES)
    {
        CallHookPkt(nam->nam_AudioCtrl->ahiac_SamplerFunc, nam->nam_AudioCtrl, &nam->nam_RecMsg);
        /* on with the other block: the one handed over stays as it is until the next one goes */
        nam->nam_RecMsg.ahirm_Buffer = (block == nam->nam_RecBuffer)
                                       ? nam->nam_RecBuffer + nRecBlockFrames(nam) * REC_FRAME_WORDS
                                       : nam->nam_RecBuffer;
        nam->nam_RecMsg.ahirm_Length = 0;
    }

}
/* \\\ */

/* /// "nReleaseHook()" */
void nReleaseHook(struct Hook * hook asm("a0"), APTR prt asm("a2"), APTR unused asm("a1"))
{

    struct NepAudioMode *nam = (struct NepAudioMode *) hook->h_Data;
    struct NepClassAudio *nch = nam->nam_Unit;

    psdStopRTIso(nam->nam_RTIso);
    if(nam->nam_IsInput)
    {
        // we can only stop recording, we still need to call the player func until audio is done
        psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname, "Recording stopped.");
    } else {
        /* AHI's player goes on, on the timer clock, until AHI stops it;
           if it is not playing, subLibStart() starts the timer */
        nam->nam_PlayerInt.is_Code = (VOID_FUNC) subLibPlayerIntDummy;
        nam->nam_FallbackTimer = TRUE;
        if(nam->nam_Playing)
        {
            Cause(&nam->nam_PlayerInt);
        }
    }
    nch->nch_DenyRequests = TRUE;
    psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                   "Audio device removed while playing.");
    

}
/* \\\ */

/* /// "nConv8BitMono()" */
void nConv8BitMono(struct Hook * hook asm("a0"), BYTE * btarptr asm("a2"), ULONG cnt asm("a1"))
{

    WORD *srcptr = hook->h_Data;
    do
    {
        *btarptr++ = *((BYTE *) srcptr);
        srcptr++;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nConv8BitStereo()" */
void nConv8BitStereo(struct Hook * hook asm("a0"), BYTE * btarptr asm("a2"), ULONG cnt asm("a1"))
{

    WORD *srcptr = hook->h_Data;
    do
    {
        *btarptr++ = *((BYTE *) srcptr);
        srcptr++;
        *btarptr++ = *((BYTE *) srcptr);
        srcptr++;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nConv16BitMono()" */
void nConv16BitMono(struct Hook * hook asm("a0"), UWORD * wtarptr asm("a2"), ULONG cnt asm("a1"))
{

    UWORD *srcptr = hook->h_Data;
    do
    {
        *wtarptr++ = AROS_WORD2LE(*srcptr);
        srcptr++;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nConv16BitStereo()" */
void nConv16BitStereo(struct Hook * hook asm("a0"), ULONG * ltarptr asm("a2"), ULONG cnt asm("a1"))
{
    
    ULONG *lsrcptr = hook->h_Data;
    do
    {
        *ltarptr++ = ((*lsrcptr>>8) & 0x00ff00ff)|((*lsrcptr<<8) & 0xff00ff00);
        lsrcptr++;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nConv24BitMono()" */
void nConv24BitMono(struct Hook * hook asm("a0"), UBYTE * btarptr asm("a2"), ULONG cnt asm("a1"))
{

    ULONG *lsrcptr = hook->h_Data;
    ULONG tmp;
    do
    {
        tmp = *lsrcptr++;
        tmp >>= 8;
        *btarptr++ = tmp;
        tmp >>= 8;
        *btarptr++ = tmp;
        tmp >>= 8;
        *btarptr++ = tmp;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nConv24BitStereo()" */
void nConv24BitStereo(struct Hook * hook asm("a0"), UBYTE * btarptr asm("a2"), ULONG cnt asm("a1"))
{

    ULONG *lsrcptr = hook->h_Data;
    ULONG tmp;
    do
    {
        tmp = *lsrcptr++;
        tmp >>= 8;
        *btarptr++ = tmp;
        tmp >>= 8;
        *btarptr++ = tmp;
        tmp >>= 8;
        *btarptr++ = tmp;
        tmp = *lsrcptr++;
        tmp >>= 8;
        *btarptr++ = tmp;
        tmp >>= 8;
        *btarptr++ = tmp;
        tmp >>= 8;
        *btarptr++ = tmp;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nConv32BitMono()" */
void nConv32BitMono(struct Hook * hook asm("a0"), ULONG * ltarptr asm("a2"), ULONG cnt asm("a1"))
{

    UBYTE *bsrcptr = hook->h_Data;
    register ULONG tmp;
    do
    {
        tmp = *bsrcptr++;
        tmp |= (*bsrcptr++)<<8;
        tmp |= (*bsrcptr++)<<16;
        tmp |= (*bsrcptr++)<<24;
        *ltarptr++ = tmp;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nConv32BitStereo()" */
void nConv32BitStereo(struct Hook * hook asm("a0"), ULONG * ltarptr asm("a2"), ULONG cnt asm("a1"))
{

    UBYTE *bsrcptr = hook->h_Data;
    register ULONG tmp;
    do
    {
        tmp = *bsrcptr++;
        tmp |= (*bsrcptr++)<<8;
        tmp |= (*bsrcptr++)<<16;
        tmp |= (*bsrcptr++)<<24;
        *ltarptr++ = tmp;
        tmp = *bsrcptr++;
        tmp |= (*bsrcptr++)<<8;
        tmp |= (*bsrcptr++)<<16;
        tmp |= (*bsrcptr++)<<24;
        *ltarptr++ = tmp;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec8BitMono()" */
void nRec8BitMono(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    BYTE *srcptr = hook->h_Data;
    do
    {
        WORD src = *srcptr++ * 256; /* the byte is the high half of AHI's 16 bits */
        *tarptr++ = src;
        *tarptr++ = src;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec8BitStereo()" */
void nRec8BitStereo(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    BYTE *srcptr = hook->h_Data;
    do
    {
        *tarptr++ = *srcptr++ * 256; /* the byte is the high half of AHI's 16 bits */
        *tarptr++ = *srcptr++ * 256;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec16BitMono()" */
void nRec16BitMono(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    UWORD *srcptr = hook->h_Data;
    do
    {
        WORD src = AROS_WORD2LE(*srcptr);
        *tarptr++ = src;
        *tarptr++ = src;
        srcptr++;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec16BitStereo()" */
void nRec16BitStereo(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    UWORD *srcptr = hook->h_Data;
    do
    {
        *tarptr++ = AROS_WORD2LE(*srcptr);
        srcptr++;
        *tarptr++ = AROS_WORD2LE(*srcptr);
        srcptr++;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec24BitMono()" */
void nRec24BitMono(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    UBYTE *srcptr = hook->h_Data;
    srcptr++;
    do
    {
        UWORD src = *srcptr|(srcptr[1]<<8);
        *tarptr++ = src;
        *tarptr++ = src;
        srcptr += 3;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec24BitStereo()" */
void nRec24BitStereo(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    UBYTE *srcptr = hook->h_Data;
    srcptr++;
    do
    {
        *tarptr++ = *srcptr|(srcptr[1]<<8);
        *tarptr++ = srcptr[3]|(srcptr[4]<<8);
        srcptr += 6;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec32BitMono()" */
void nRec32BitMono(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    UWORD *srcptr = hook->h_Data;
    srcptr++;
    do
    {
        UWORD src = AROS_WORD2LE(*srcptr);
        *tarptr++ = src;
        *tarptr++ = src;
        srcptr += 2;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nRec32BitStereo()" */
void nRec32BitStereo(struct Hook * hook asm("a0"), WORD * tarptr asm("a2"), ULONG cnt asm("a1"))
{

    UWORD *srcptr = hook->h_Data;
    srcptr++;
    do
    {
        *tarptr++ = AROS_WORD2LE(*srcptr);
        srcptr += 2;
        *tarptr++ = AROS_WORD2LE(*srcptr);
        srcptr += 2;
    } while(--cnt);
    
}
/* \\\ */

/* /// "nSelectAudioMode()" */
BOOL nSelectAudioMode(struct NepAudioMode *nam)
{
    struct NepClassAudio *nch = nam->nam_Unit;
    UBYTE freqbuf[3];
    ULONG freq = nam->nam_AudioCtrl->ahiac_MixFreq;
    struct NepAudioMode *alt = nAltForFreq(nam, freq);
    IPTR highspeed = FALSE;
    IPTR superspeed = FALSE;
    IPTR pktsize = 0;
    IPTR mult = 1;
    IPTR bytesperinterval = 0;
    LONG ioerr;
    ULONG maxfreq;

    KPRINTF(1, ("SelectingAltInterface\n"));
    if(!(psdSetAltInterface(nam->nam_EP0Pipe, alt->nam_Interface)))
    {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "Cannot select alternate interface!");
        return(FALSE);
    }
    nam->nam_EP = psdFindEndpoint(alt->nam_Interface, NULL,
                                  EA_IsIn, nam->nam_IsInput,
                                  EA_TransferType, USEAF_ISOCHRONOUS,
                                  TAG_END);

    if(!nam->nam_EP)
    {
        KPRINTF(1, ("Ooops!?! No Endpoints defined?\n"));
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "No Iso Endpoint!");
        return(FALSE);
    }
    psdGetAttrs(PGA_ENDPOINT, nam->nam_EP,
                EA_EndpointNum, &nam->nam_EPNum,
                EA_MaxPktSize, &pktsize,
                EA_NumTransMuFrame, &mult,
                EA_BytesPerInterval, &bytesperinterval,
                EA_Interval, &nam->nam_Interval,
                TAG_END);
    /* endpoint-recipient requests carry the full endpoint address (incl. direction bit) in wIndex */
    nam->nam_EPAddr = nam->nam_EPNum | (nam->nam_IsInput ? URTF_IN : 0);

    /* A high speed or SuperSpeed endpoint has its interval in microframes;
       everything here counts milliseconds. (One that is served more often
       than once a millisecond is taken for one that is not: it records, and
       it would play at the wrong pace.) */
    psdGetAttrs(PGA_DEVICE, nch->nch_Device,
                DA_IsHighspeed, &highspeed,
                DA_IsSuperspeed, &superspeed,
                TAG_END);
    if(highspeed || superspeed)
    {
        nam->nam_Interval = (nam->nam_Interval > 8) ? (nam->nam_Interval >> 3) : 1;
    }

    /* What the endpoint of this alternate setting can carry: a high speed
       endpoint may make up to three transactions a microframe, a SuperSpeed
       one says it in its companion descriptor */
    nam->nam_IntervalBytes = superspeed ? bytesperinterval : pktsize * mult;
    maxfreq = (1000 * nam->nam_IntervalBytes) / (nam->nam_FrameSize * nam->nam_Interval);
    if(maxfreq < freq)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "%ld Hz was asked for, but bandwidth-wise, only %ld Hz is possible.",
                       freq, maxfreq);
    }

    if(alt->nam_HasFreqCtrl)
    {
        KPRINTF(1, ("Setting frequency\n"));
        freqbuf[0] = freq;
        freqbuf[1] = (freq>>8);
        freqbuf[2] = (freq>>16);
        psdPipeSetup(nam->nam_EP0Pipe, URTF_CLASS|URTF_ENDPOINT, UAUDR_SET_CUR, UAECS_SAMPLE_FREQ<<8, nam->nam_EPAddr);
        ioerr = psdDoPipe(nam->nam_EP0Pipe, freqbuf, 3);
        if(ioerr)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Could not set current frequency for EP %ld: %s (%ld)!",
                           nam->nam_EPNum,
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        }
    }
    if(alt->nam_HasPitchCtrl)
    {
        KPRINTF(1, ("Setting pitch control\n"));
        freqbuf[0] = 1;
        psdPipeSetup(nam->nam_EP0Pipe, URTF_CLASS|URTF_ENDPOINT, UAUDR_SET_CUR, UAECS_PITCH<<8, nam->nam_EPAddr);
        ioerr = psdDoPipe(nam->nam_EP0Pipe, freqbuf, 1);
        if(ioerr)
        {
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "Could not enable pitch control: %s (%ld)!",
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        }
    }
    nam->nam_ReleaseHook.h_Entry = (APTR) nReleaseHook;
    nam->nam_ReleaseHook.h_Data = nam;

    if(nam->nam_IsInput)
    {
        KPRINTF(1, ("Input hook\n"));
        nch->nch_InReqHook.h_Entry = (APTR) nInReqHook;
        nch->nch_InReqHook.h_Data = nam;
        nch->nch_InDoneHook.h_Entry = (APTR) nInDoneHook;
        nch->nch_InDoneHook.h_Data = nam;

        nam->nam_RTIso = psdAllocRTIsoHandler(nam->nam_EP,
                                              RTA_InRequestHook, &nch->nch_InReqHook,
                                              RTA_InDoneHook, &nch->nch_InDoneHook,
                                              RTA_ReleaseHook, &nam->nam_ReleaseHook,
                                              TAG_END);
    } else {
        KPRINTF(1, ("Output hook\n"));

        nch->nch_OutReqHook.h_Entry = (APTR) nOutReqHook;
        nch->nch_OutReqHook.h_Data = nam;

        nam->nam_RTIso = psdAllocRTIsoHandler(nam->nam_EP,
                                              RTA_OutRequestHook, &nch->nch_OutReqHook,
                                              RTA_ReleaseHook, &nam->nam_ReleaseHook,
                                              TAG_END);
    }
    if(!nam->nam_RTIso)
    {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname, "Could not allocate RT Iso Handler!");
        return(FALSE);
    }
    /*psdPipeSetup(nch->nch_EP0Pipe, URTF_CLASS|URTF_ENDPOINT, UAUDR_SET_CUR, UAECS_SAMPLE_FREQ, nam->nam_EPNum);
    freq[0] = 44100 & 0xff;
    freq[1] = (44100>>8);
    freq[2] = (44100>>16);
    ioerr = psdDoPipe(nch->nch_EP0Pipe, freq, 3);
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Could not set current frequency!");
    }
    psdPipeSetup(nch->nch_EP0Pipe, URTF_IN|URTF_CLASS|URTF_ENDPOINT, UAUDR_GET_CUR, UAECS_SAMPLE_FREQ, nam->nam_EPNum);
    ioerr = psdDoPipe(nch->nch_EP0Pipe, freq, 3);
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Could not get current frequency!");
    } else {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "Current frequency: %ld Hz",
                       freq[0]|(freq[1]<<8)|(freq[2]<<16));
    }*/

    nch->nch_DenyRequests = FALSE;
    return(TRUE);
}
/* \\\ */

/* /// "nConverter()" */
/* Sample conversion hooks by bytes per sample (1 to 4) and channels (1, 2):
   playback converts AHI's mix into the device's format, recording the
   device's format into 16 bit stereo for AHI. */
static const APTR PlayConverters[4][2] =
{
    { (APTR) nConv8BitMono,  (APTR) nConv8BitStereo },
    { (APTR) nConv16BitMono, (APTR) nConv16BitStereo },
    { (APTR) nConv24BitMono, (APTR) nConv24BitStereo },
    { (APTR) nConv32BitMono, (APTR) nConv32BitStereo }
};
static const APTR RecConverters[4][2] =
{
    { (APTR) nRec8BitMono,  (APTR) nRec8BitStereo },
    { (APTR) nRec16BitMono, (APTR) nRec16BitStereo },
    { (APTR) nRec24BitMono, (APTR) nRec24BitStereo },
    { (APTR) nRec32BitMono, (APTR) nRec32BitStereo }
};

/* Set the mode's conversion hook from table; FALSE for a sample format
   it has none for */
static BOOL nConverter(struct NepAudioMode *nam, const APTR table[4][2])
{
    UWORD size = nam->nam_SampleSize;
    UWORD channels = nam->nam_NumChannels;

    if((size < 1) || (size > 4) || (channels < 1) || (channels > 2))
    {
        KPRINTF(10, ("Unknown sample format\n"));
        return FALSE;
    }
    nam->nam_SamConvHook.h_Entry = table[size - 1][channels - 1];
    return TRUE;
}
/* \\\ */

/* /// "nMaxMixFrames()" */
/* The most frames one player pass mixes (nPlayerTick): the time between two
   player calls at the slowest rate the program gave AHI - its player rate or
   its minimum, whichever is lower - and one millisecond more for the part
   carried over, at the mix rate. At least 100 ms per millisecond of USB
   interval, what the buffers always had. Both rates normalised to 16.16. */
static ULONG nMaxMixFrames(struct NepAudioMode *nam, struct AHIAudioCtrlDrv *audioctrl)
{
    Fixed slowest = audioctrl->ahiac_PlayerFreq;
    ULONG frames = (audioctrl->ahiac_MixFreq * nam->nam_Interval) / 10;

    if(audioctrl->ahiac_MinPlayerFreq && (audioctrl->ahiac_MinPlayerFreq < slowest))
    {
        slowest = audioctrl->ahiac_MinPlayerFreq;
    }
    if(slowest >= 0x10000)
    {
        ULONG ms = (((1000UL<<20) / (slowest>>12)) >> 16) + 1;
        ULONG need = (audioctrl->ahiac_MixFreq * ms) / 1000 + 1;

        frames = (need > frames) ? need : frames;
    }
    return frames;
}
/* \\\ */

/* /// "subLibAllocAudio()" */
ULONG (subLibAllocAudio)(struct TagItem * tags asm("a1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{
    
    struct NepAudioMode *nam = (struct NepAudioMode *) GetTagData(AHIDB_NepAudioMode, (IPTR) NULL, tags);
    struct NepAudioMode *recnam;
    struct NepClassAudio *nch;
    ULONG flags;
    UWORD cnt;
    STRPTR audiomode;

    KPRINTF(10, ("subLibAllocAudio(%08lx)\n", audioctrl));
    if(!nam)
    {
        KPRINTF(20, ("Could not find NepAudioMode in GetAttr call!\n"));
        audioctrl->ahiac_DriverData = NULL;
        return AHISF_ERROR;
    }
    /* one open mode per binding: the hooks, the task's current mode and the
       interface's alternate setting are the binding's. The claim is given
       back by subLibFreeAudio, which AHI calls even when this fails. */
    nch = nam->nam_Unit;
    Forbid();
    if(nch->nch_CurrentMode)
    {
        Permit();
        KPRINTF(20, ("Already in use!\n"));
        audioctrl->ahiac_DriverData = NULL;
        return AHISF_ERROR;
    }
    nch->nch_CurrentMode = nam;
    Permit();
    audioctrl->ahiac_DriverData = nam;
    nam->nam_AudioCtrl = audioctrl;
    if(!(nam->nam_PsdBase = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return AHISF_ERROR;
    }

    nam->nam_FallbackTimer = FALSE;
    nam->nam_Playing = FALSE;
    nam->nam_TimerMsgPort.mp_Node.ln_Type = NT_MSGPORT;
    nam->nam_TimerMsgPort.mp_Flags = PA_SOFTINT;
    nam->nam_TimerMsgPort.mp_SigTask = &nam->nam_PlayerInt;
    NewList(&nam->nam_TimerMsgPort.mp_MsgList);
    if((nam->nam_TimerIOReq = (struct timerequest *) CreateIORequest(&nam->nam_TimerMsgPort, sizeof(struct timerequest))))
    {
        if(!OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *) nam->nam_TimerIOReq, 0))
        {
            nam->nam_TimerIOReq->tr_node.io_Message.mn_Node.ln_Type = NT_FREEMSG;
        } else {
            DeleteIORequest((struct IORequest *) nam->nam_TimerIOReq);
            nam->nam_TimerIOReq = NULL;
            return AHISF_ERROR;
        }
    } else {
        return AHISF_ERROR;
    }

    /* the rate first: which alternate setting is selected can depend on it */
    for(cnt = 0; cnt < nam->nam_NumFrequencies; cnt++)
    {
        if(audioctrl->ahiac_MixFreq == nam->nam_FreqArray[cnt])
        {
            break;
        }
    }
    if(cnt == nam->nam_NumFrequencies)
    {
        // need rate adaption
        ULONG target = audioctrl->ahiac_MixFreq;
        ULONG diff = 100000;
        for(cnt = 0; cnt < nam->nam_NumFrequencies; cnt++)
        {
            if(nam->nam_FreqArray[cnt] > target)
            {
                if(nam->nam_FreqArray[cnt] - target < diff)
                {
                    diff = nam->nam_FreqArray[cnt] - target;
                    audioctrl->ahiac_MixFreq = nam->nam_FreqArray[cnt];
                }
            } else {
                if(target - nam->nam_FreqArray[cnt] < diff)
                {
                    diff = target - nam->nam_FreqArray[cnt];
                    audioctrl->ahiac_MixFreq = nam->nam_FreqArray[cnt];
                }
            }
        }
    }

    KPRINTF(10, ("CreateMsgPort\n"));
    if(!(nam->nam_TaskMsgPort = CreateMsgPort()))
    {
        return AHISF_ERROR;
    }
    KPRINTF(10, ("AllocPipe\n"));
    if(!(nam->nam_EP0Pipe = psdAllocPipe(nch->nch_Device, nam->nam_TaskMsgPort, NULL)))
    {
        return AHISF_ERROR;
    }
    KPRINTF(10, ("SelectAudioMode\n"));
    if(!(nSelectAudioMode(nam)))
    {
        return AHISF_ERROR;
    }

    flags = AHISF_MIXING;
    if(nam->nam_NumChannels > 1)
    {
        flags |= AHISF_KNOWSTEREO;
    }
    if(nam->nam_Resolution > 16)
    {
        flags |= AHISF_KNOWHIFI;
    }
    KPRINTF(10, ("Freq=%ld, BuffSamples = %ld, BuffSize = %ld\n",
                 audioctrl->ahiac_MixFreq, audioctrl->ahiac_BuffSamples, audioctrl->ahiac_BuffSize));
    KPRINTF(10, ("NumChan=%ld, Sounds=%ld, BuffType=%04lx\n",
                 audioctrl->ahiac_Channels, audioctrl->ahiac_Sounds, audioctrl->ahiac_BuffType));
    KPRINTF(10, ("MinPlayerFreq=%08lx, MaxPlayerFreq=%08lx\n",
                 audioctrl->ahiac_MinPlayerFreq, audioctrl->ahiac_MaxPlayerFreq));

    if(audioctrl->ahiac_PlayerFreq < 0x10000)
    {
        audioctrl->ahiac_PlayerFreq <<= 16;
    }

    if(audioctrl->ahiac_MinPlayerFreq < 0x10000)
    {
        audioctrl->ahiac_MinPlayerFreq <<= 16;
    }
    nam->nam_MaxMixFrames = nMaxMixFrames(nam, audioctrl);
    /* AHI is told 10 Hz at least, as it always was; the buffers are sized
       above, from what the program gave */
    if(audioctrl->ahiac_MinPlayerFreq < 10<<16)
    {
        audioctrl->ahiac_MinPlayerFreq = 10<<16;
    }

    if(audioctrl->ahiac_MaxPlayerFreq < 0x10000)
    {
        audioctrl->ahiac_MaxPlayerFreq <<= 16;
    }

    /* ahiac_Channels is the number of mixer channels the application asked
       for, not the device's output channel count - AHI sizes its per-channel
       data by it. The device's layout is reported via AHISF_KNOWSTEREO. */

    audioctrl->ahiac_BuffSamples = nam->nam_MaxMixFrames;
    audioctrl->ahiac_BuffSize = audioctrl->ahiac_BuffSamples * nam->nam_AHIFrameSize;

    /* what AHI mixes into */
    nam->nam_MixBuffer = psdAllocVec(audioctrl->ahiac_BuffSize);
    if(!nam->nam_MixBuffer)
    {
        return AHISF_ERROR;
    }

    nam->nam_MasterVol = 0x00010000;
    nam->nam_ChannelVol[0] = 0x00010000;
    nam->nam_ChannelVol[1] = 0x00010000;
    nam->nam_InputGain = 0x00010000;
    nam->nam_MonitorVol = 0x00010000;

    KPRINTF(10, ("Freq=%ld, BuffSamples = %ld, BuffSize = %ld\n",
                 audioctrl->ahiac_MixFreq, audioctrl->ahiac_BuffSamples, audioctrl->ahiac_BuffSize));

    nam->nam_PlayerInt.is_Node.ln_Type = NT_INTERRUPT;
    nam->nam_PlayerInt.is_Node.ln_Name = "Player Interrupt";
    nam->nam_PlayerInt.is_Node.ln_Pri  = 0;
    nam->nam_PlayerInt.is_Data = nam;
    if(nam->nam_IsInput)
    {
        /* nothing to play to: the timer is the clock from the start, as it
           is for a sound card that was unplugged while it played */
        nam->nam_FallbackTimer = TRUE;
        nam->nam_PlayerInt.is_Code = (VOID_FUNC) subLibPlayerIntDummy;
        audiomode = "Input-only";
    } else {
        nam->nam_PlayBuffer[0] = psdAllocVec(nam->nam_MaxMixFrames * nam->nam_FrameSize);
        nam->nam_PlayBuffer[1] = psdAllocVec(nam->nam_MaxMixFrames * nam->nam_FrameSize);
        if(!(nam->nam_PlayBuffer[0] && nam->nam_PlayBuffer[1]))
        {
            return AHISF_ERROR;
        }

        // make sure to play silence, if CPU is too slow on first frame
        nam->nam_SendBuf = 1;
        nam->nam_FillBuf = 0;
        nam->nam_SendCarry = 0;
        nam->nam_PlayerTimer = 0;
        nam->nam_SendLeft = 0;
        nam->nam_PlayLen[0] = 0;
        nam->nam_PlayLen[1] = 0;

        if(nam->nam_Unit->nch_AHIBase->lib_Version < 6)
        {
            KPRINTF(10, ("Using V4 code\n"));
            nam->nam_PlayerInt.is_Code = (VOID_FUNC) subLibPlayerIntV4;
        } else {
            KPRINTF(10, ("Using V6 code\n"));
            nam->nam_PlayerInt.is_Code = (VOID_FUNC) subLibPlayerIntV6;
        }
        nam->nam_SamConvHook.h_Data = nam->nam_MixBuffer;

        if(!nConverter(nam, PlayConverters))
        {
            return AHISF_ERROR;
        }
        audiomode = nam->nam_RecStream ? "Full-Duplex" : "Output-only";
    }

    if((recnam = nRecMode(nam)))
    {
        if(recnam != nam)
        {
            recnam->nam_PsdBase = nam->nam_PsdBase;
            recnam->nam_AudioCtrl = audioctrl;
            if(!(recnam->nam_TaskMsgPort = CreateMsgPort()))
            {
                return AHISF_ERROR;
            }
            if(!(recnam->nam_EP0Pipe = psdAllocPipe(nch->nch_Device, recnam->nam_TaskMsgPort, NULL)))
            {
                return AHISF_ERROR;
            }
            if(!(nSelectAudioMode(recnam)))
            {
                return AHISF_ERROR;
            }
        }
        /* two record blocks for AHI, and one service interval as it arrives */
        recnam->nam_RecBuffer = psdAllocVec(2 * nRecBlockFrames(recnam) * REC_FRAME_WORDS * sizeof(WORD));
        recnam->nam_PacketBuffer = psdAllocVec(recnam->nam_IntervalBytes);
        if(!(recnam->nam_RecBuffer && recnam->nam_PacketBuffer))
        {
            return AHISF_ERROR;
        }
        recnam->nam_SamConvHook.h_Data = recnam->nam_PacketBuffer;

        if(!nConverter(recnam, RecConverters))
        {
            return AHISF_ERROR;
        }

        flags |= AHISF_CANRECORD;
    }
    psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Opening %s audio mode at %ld Hz with %ld channel%s and %ld bits each.",
                   audiomode,
                   audioctrl->ahiac_MixFreq,
                   nam->nam_NumChannels,
                   (nam->nam_NumChannels > 1) ? "s" : "",
                   nam->nam_Resolution);

    return flags;
    
}
/* \\\ */

/* /// "subLibFreeAudio()" */
void (subLibFreeAudio)(struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{

    struct NepAudioMode *nam = (struct NepAudioMode *) audioctrl->ahiac_DriverData;

    KPRINTF(10, ("subLibFreeAudio(%08lx)\n", audioctrl));
    /* no mode: the open was refused before claiming one */
    if(!nam)
    {
        return;
    }
    if(!nam->nam_PsdBase)
    {
        nam->nam_Unit->nch_CurrentMode = NULL;
        return;
    }
    struct NepAudioMode *recnam = nam->nam_RecStream;

    if(recnam)
    {
        if(recnam->nam_RTIso)
        {
            psdStopRTIso(recnam->nam_RTIso);
            psdFreeRTIsoHandler(recnam->nam_RTIso);
            recnam->nam_RTIso = NULL;
        }

        psdFreeVec(recnam->nam_RecBuffer);
        recnam->nam_RecBuffer = NULL;
        psdFreeVec(recnam->nam_PacketBuffer);
        recnam->nam_PacketBuffer = NULL;

        if(recnam->nam_ZeroBWIF)
        {
            if(!(psdSetAltInterface(recnam->nam_EP0Pipe, recnam->nam_ZeroBWIF)))
            {
                psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname, "Cannot restore zero bandwidth interface!");
            }
        }
        if(recnam->nam_EP0Pipe)
        {
            psdFreePipe(recnam->nam_EP0Pipe);
            recnam->nam_EP0Pipe = NULL;
        }
        DeleteMsgPort(recnam->nam_TaskMsgPort);
        recnam->nam_TaskMsgPort = NULL;
        recnam->nam_PsdBase = NULL;
    }
    if(nam->nam_FallbackTimer && nAbortTimer(nam))
    {
        WaitIO((struct IORequest *) nam->nam_TimerIOReq);
    }
    if(nam->nam_RTIso)
    {
        psdStopRTIso(nam->nam_RTIso);
        psdFreeRTIsoHandler(nam->nam_RTIso);
        nam->nam_RTIso = NULL;
    }

    psdFreeVec(nam->nam_MixBuffer);
    nam->nam_MixBuffer = NULL;
    psdFreeVec(nam->nam_RecBuffer);
    nam->nam_RecBuffer = NULL;
    psdFreeVec(nam->nam_PlayBuffer[0]);
    nam->nam_PlayBuffer[0] = NULL;
    psdFreeVec(nam->nam_PlayBuffer[1]);
    nam->nam_PlayBuffer[1] = NULL;
    psdFreeVec(nam->nam_PacketBuffer);
    nam->nam_PacketBuffer = NULL;

    if(nam->nam_ZeroBWIF)
    {
        if(!(psdSetAltInterface(nam->nam_EP0Pipe, nam->nam_ZeroBWIF)))
        {
            psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname, "Cannot restore zero bandwidth interface!");
        }
    }

    if(nam->nam_EP0Pipe)
    {
        psdFreePipe(nam->nam_EP0Pipe);
        nam->nam_EP0Pipe = NULL;
    }
    DeleteMsgPort(nam->nam_TaskMsgPort);
    nam->nam_TaskMsgPort = NULL;

    if(nam->nam_TimerIOReq)
    {
        CloseDevice((struct IORequest *) nam->nam_TimerIOReq);
        DeleteIORequest((struct IORequest *) nam->nam_TimerIOReq);
        nam->nam_TimerIOReq = NULL;
    }

    CloseLibrary(nam->nam_PsdBase);
    nam->nam_PsdBase = NULL;
    /* last: once it is NULL the binding task may end and a release free
       the mode */
    nam->nam_Unit->nch_CurrentMode = NULL;
}
/* \\\ */

/* /// "subLibDisable()" */
void (subLibDisable)(struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{
    //KPRINTF(10, ("subLibDisable(%08lx)\n", audioctrl));
}
/* \\\ */

/* /// "subLibEnable()" */
void (subLibEnable)(struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{
    //KPRINTF(10, ("subLibEnable(%08lx)\n", audioctrl));
}
/* \\\ */

/* the timer of the mode: opened when AHI allocated it */
#undef  TimerBase
#define TimerBase nam->nam_TimerIOReq->tr_node.io_Device

/* /// "subLibStart()" */
ULONG (subLibStart)(ULONG flags asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{

    struct NepAudioMode *nam = (struct NepAudioMode *) audioctrl->ahiac_DriverData;
    ULONG res = AHIE_UNKNOWN;

    KPRINTF(10, ("subLibStart(%08lx, %lx)\n", audioctrl, flags));
    if(!nam)
    {
        return res;
    }
    if(flags & AHISF_PLAY)
    {
        KPRINTF(10, ("Freq=%ld, BuffSamples = %ld, BuffSize = %ld, PlayerFreq = %08lx\n",
                     audioctrl->ahiac_MixFreq, audioctrl->ahiac_BuffSamples, audioctrl->ahiac_BuffSize,
                     audioctrl->ahiac_PlayerFreq));
        KPRINTF(10, ("NumChan=%ld, Sounds=%ld, BuffType=%04lx\n",
                     audioctrl->ahiac_Channels, audioctrl->ahiac_Sounds, audioctrl->ahiac_BuffType));
        KPRINTF(10, ("MinPlayerFreq=%08lx, MaxPlayerFreq=%08lx\n",
                     audioctrl->ahiac_MinPlayerFreq, audioctrl->ahiac_MaxPlayerFreq));
        if(nam->nam_AudioCtrl->ahiac_PlayerFreq < 0x10000)
        {
            nam->nam_AudioCtrl->ahiac_PlayerFreq <<= 16;
        }
        nam->nam_PlayerFrac = (1000UL<<20) / (nam->nam_AudioCtrl->ahiac_PlayerFreq>>12);
        KPRINTF(10, ("Player Frac = %08lx\n", nam->nam_PlayerFrac));
        nam->nam_FramesPerMS = (audioctrl->ahiac_MixFreq<<16) / 1000;
        nam->nam_Playing = TRUE;
        if(nam->nam_FallbackTimer)
        {
            /* AHI may start what runs already: the request is not sent twice */
            nAbortTimer(nam);
            nam->nam_TimerMsgPort.mp_Flags = PA_SOFTINT;
            Cause(&nam->nam_PlayerInt);
        } else {
            psdStartRTIso(nam->nam_RTIso);
        }
        res = AHIE_OK;
    }
    if(flags & AHISF_RECORD)
    {
        struct NepAudioMode *recnam = nRecMode(nam);
        if(recnam)
        {
            // target format is always 16 bit stereo; the first block is empty
            recnam->nam_RecMsg.ahirm_Type = AHIST_S16S;
            recnam->nam_RecMsg.ahirm_Buffer = recnam->nam_RecBuffer;
            recnam->nam_RecMsg.ahirm_Length = 0;
            recnam->nam_RecFrames = 0;
            GetSysTime(&recnam->nam_RecStart);
            psdStartRTIso(recnam->nam_RTIso);
            res = AHIE_OK;
        } else {
            res = AHIE_UNKNOWN;
        }
    }
    return res;
    
}
/* \\\ */

/* /// "subLibUpdate()" */
ULONG (subLibUpdate)(ULONG flags asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{

    struct NepAudioMode *nam = (struct NepAudioMode *) audioctrl->ahiac_DriverData;
    KPRINTF(10, ("subLibUpdate(%08lx, %lx)\n", audioctrl, flags));

    if(!nam)
    {
        return AHIE_UNKNOWN;
    }
    if(nam->nam_AudioCtrl->ahiac_PlayerFreq < 0x10000)
    {
        nam->nam_AudioCtrl->ahiac_PlayerFreq <<= 16;
    }
    nam->nam_PlayerFrac = (1000UL<<20) / (nam->nam_AudioCtrl->ahiac_PlayerFreq>>12);
    return AHIE_OK;
    
}
/* \\\ */

/* /// "subLibStop()" */
ULONG (subLibStop)(ULONG flags asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{

    struct NepAudioMode *nam = (struct NepAudioMode *) audioctrl->ahiac_DriverData;

    KPRINTF(10, ("subLibStop(%08lx, %lx)\n", audioctrl, flags));
    if(!nam)
    {
        return AHIE_UNKNOWN;
    }
    if(flags & AHISF_PLAY)
    {
        nam->nam_Playing = FALSE;
        if(nam->nam_FallbackTimer)
        {
            nAbortTimer(nam);
        } else {
            psdStopRTIso(nam->nam_RTIso);
        }
    }
    struct NepAudioMode *recnam = nRecMode(nam);
    if((flags & AHISF_RECORD) && recnam)
    {
        psdStopRTIso(recnam->nam_RTIso);
        if(recnam->nam_RecStart.tv_secs)
        {
            /* measurement of the recording path: what arrived
               against what the rate should have brought in that time. Lost
               intervals show here, as too few frames. */
            struct timeval now;

            GetSysTime(&now);
            ULONG ms = (now.tv_secs - recnam->nam_RecStart.tv_secs) * 1000 +
                       ((LONG) now.tv_micro - (LONG) recnam->nam_RecStart.tv_micro) / 1000;
            ULONG freq = audioctrl->ahiac_MixFreq;
            psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                           "Recording ended: %ld frames in %ld ms; %ld Hz would make %ld.",
                           recnam->nam_RecFrames, ms, freq,
                           (ms / 1000) * freq + ((ms % 1000) * freq) / 1000);
            recnam->nam_RecStart.tv_secs = 0;
        }
    }
    return AHIE_OK;
    
}
/* \\\ */

/* /// "subLibSetVol()" */
ULONG (subLibSetVol)(UWORD channel asm("d0"), Fixed volume asm("d1"), sposition pan asm("d2"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), ULONG flags asm("d3"), struct NepAudioSubLibBase * nas asm("a6"))
{
    return AHIS_UNKNOWN;
}
/* \\\ */

/* /// "subLibSetFreq()" */
ULONG (subLibSetFreq)(UWORD channel asm("d0"), ULONG freq asm("d1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), ULONG flags asm("d2"), struct NepAudioSubLibBase * nas asm("a6"))
{
    return AHIS_UNKNOWN;
}
/* \\\ */

/* /// "subLibSetSound()" */
ULONG (subLibSetSound)(UWORD channel asm("d0"), UWORD sound asm("d1"), ULONG offset asm("d2"), LONG length asm("d3"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), ULONG flags asm("d4"), struct NepAudioSubLibBase * nas asm("a6"))
{
    return AHIS_UNKNOWN;
}
/* \\\ */

/* /// "subLibSetEffect()" */
ULONG (subLibSetEffect)(ULONG * effect asm("a0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{
    return AHIS_UNKNOWN;
}
/* \\\ */

/* /// "subLibLoadSound()" */
ULONG (subLibLoadSound)(UWORD sound asm("d0"), ULONG type asm("d1"), APTR info asm("a0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{
    return AHIS_UNKNOWN;
}
/* \\\ */

/* /// "subLibUnloadSound()" */
ULONG (subLibUnloadSound)(UWORD sound asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{
    return AHIS_UNKNOWN;
}
/* \\\ */

/* /// "subLibGetAttr()" */
IPTR (subLibGetAttr)(ULONG attr asm("d0"), LONG arg asm("d1"), LONG defvalue asm("d2"), struct TagItem * tags asm("a1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{

    UWORD cnt;
    struct NepAudioMode *nam = (struct NepAudioMode *) GetTagData(AHIDB_NepAudioMode, (IPTR) NULL, tags);
    struct NepAudioUnit *nau;
    STRPTR name;

    KPRINTF(10, ("subLibGetAttr(%08lx, %08lx, %ld, %ld)\n", audioctrl, attr, arg, defvalue));

    if(!nam)
    {
        KPRINTF(20, ("Could not find NepAudioMode in GetAttr call!\n"));
        return defvalue;
    }

    switch(attr)
    {
        case AHIDB_Bits:
            return (IPTR) nam->nam_Resolution;

        /*case AHIDB_MaxChannels:
            return (IPTR) nam->nam_NumChannels;

        case AHIDB_MinMixFreq:
            return (IPTR) nam->nam_MinFreq;

        case AHIDB_MaxMixFreq:
            return (IPTR) nam->nam_MaxFreq;*/

        case AHIDB_Frequencies:
            return (IPTR) nam->nam_NumFrequencies;

        case AHIDB_Frequency: // Index->Frequency
            return (IPTR) nam->nam_FreqArray[arg];

        case AHIDB_Index: // Frequency->Index
        {
            ULONG diff = 100000;
            LONG bestfit = 0;
            for(cnt = 0; cnt < nam->nam_NumFrequencies; cnt++)
            {
                if(nam->nam_FreqArray[cnt] > arg)
                {
                    if(nam->nam_FreqArray[cnt] - arg < diff)
                    {
                        diff = nam->nam_FreqArray[cnt] - arg;
                        bestfit = cnt;
                    }
                } else {
                    if(arg - nam->nam_FreqArray[cnt] < diff)
                    {
                        diff = arg - nam->nam_FreqArray[cnt];
                        bestfit = cnt;
                    }
                }
            }
            return bestfit;
        }

        case AHIDB_Author:
            return (IPTR) "Chris Hodges";

        case AHIDB_Copyright:
            return (IPTR) "2008-2009 Chris Hodges";

        case AHIDB_Version:
            return (IPTR) VERSION_STRING;

        case AHIDB_Annotation:
            return (IPTR) "Bye...";

        case AHIDB_Record:
        case AHIDB_FullDuplex: /* what plays on a mode that only records disturbs nothing */
            return nRecMode(nam) ? TRUE : FALSE;

        case AHIDB_Realtime:
            return TRUE;

        case AHIDB_MinOutputVolume:
            nau = nOutVolumeUnit(nam);
            return nau ? nau->nau_MinVolume : 0;

        case AHIDB_MaxOutputVolume:
            nau = nOutVolumeUnit(nam);
            return nau ? nau->nau_MaxVolume : 0;

        case AHIDB_MinInputGain:
            nau = nGetInputUnit(nam);
            if(nau)
            {
                if(nau->nau_VolumeControl)
                {
                    KPRINTF(10, ("MinInputGain %08lx\n", nau->nau_VolumeUnit->nau_MinVolume));
                    return nau->nau_VolumeUnit->nau_MinVolume;
                }
            }
            return 0;

        case AHIDB_MaxInputGain:
            nau = nGetInputUnit(nam);
            if(nau)
            {
                if(nau->nau_VolumeControl)
                {
                    KPRINTF(10, ("MaxInputGain %08lx\n", nau->nau_VolumeUnit->nau_MaxVolume));
                    return nau->nau_VolumeUnit->nau_MaxVolume;
                }
            }
            return 0;

        case AHIDB_MinMonitorVolume:
            nau = nGetInputUnit(nam);
            if(nau)
            {
                if(nau->nau_Monitor)
                {
                    KPRINTF(10, ("MinMonitorVol %08lx\n", nau->nau_MonitorUnit->nau_MinVolume));
                    return nau->nau_MonitorUnit->nau_MinVolume;
                }
            }
            return 0;

        case AHIDB_MaxMonitorVolume:
            nau = nGetInputUnit(nam);
            if(nau)
            {
                if(nau->nau_Monitor)
                {
                    KPRINTF(10, ("MaxMonitorVol %08lx\n", nau->nau_MonitorUnit->nau_MaxVolume));
                    return nau->nau_MonitorUnit->nau_MaxVolume;
                }
            }
            return 0;

        case AHIDB_Inputs:
            return nInputName(nam, -1, &name);

        case AHIDB_Input:
            nInputName(nam, arg, &name);
            return (IPTR) name;

        case AHIDB_Outputs:
            return nOutputName(nam, -1, &name);

        case AHIDB_Output:
            nOutputName(nam, arg, &name);
            return (IPTR) name;
    }
    return defvalue;
    
}
/* \\\ */

/* /// "subLibHardwareControl()" */
ULONG (subLibHardwareControl)(ULONG attr asm("d0"), LONG arg asm("d1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"))
{

    struct NepAudioMode *nam = (struct NepAudioMode *) audioctrl->ahiac_DriverData;
    struct NepClassAudio *nch;
    KPRINTF(10, ("subLibHardwareControl(%08lx, %08lx, %ld)\n", audioctrl, attr, arg));

    if(!nam)
    {
        return FALSE;
    }
    nch = nam->nam_Unit;

    switch(attr)
    {
        case AHIC_MixFreq_Query:
            return audioctrl->ahiac_MixFreq;

        case AHIC_InputGain:
            nam->nam_InputGain = arg;
            nch->nch_UpdateFlags |= UAF_INPUT_GAIN;
            Signal(nch->nch_Task, 1UL<<nch->nch_TaskMsgPort->mp_SigBit);
            return TRUE;

        case AHIC_InputGain_Query:
            return nam->nam_InputGain;

        case AHIC_MonitorVolume:
            nam->nam_MonitorVol = arg;
            nch->nch_UpdateFlags |= UAF_MONITOR_VOLUME;
            Signal(nch->nch_Task, 1UL<<nch->nch_TaskMsgPort->mp_SigBit);
            return TRUE;

        case AHIC_MonitorVolume_Query:
            return nam->nam_MonitorVol;

        case AHIC_OutputVolume:
            nam->nam_MasterVol = arg;
            nch->nch_UpdateFlags |= UAF_MASTER_VOLUME;
            Signal(nch->nch_Task, 1UL<<nch->nch_TaskMsgPort->mp_SigBit);
            return TRUE;

        case AHIC_OutputVolume_Query:
            return nam->nam_MasterVol;

        case AHIC_Input:
        {
            /* only an input AHI was offered; one alone needs no switching */
            STRPTR name;
            ULONG count = nInputName(nam, arg, &name);

            if(!name)
            {
                return FALSE;
            }
            if(count < 2)
            {
                return TRUE;
            }
            nam->nam_CurrInput = arg;
            nch->nch_UpdateFlags |= UAF_SELECT_INPUT;
            Signal(nch->nch_Task, 1UL<<nch->nch_TaskMsgPort->mp_SigBit);
            return TRUE;
        }

        case AHIC_Input_Query:
            return nam->nam_CurrInput;

        case AHIC_Output:
        {
            /* Only an output AHI was offered. Outputs are switched by muting
               the others (nMapOutputs), not by a selector unit: that only
               ever chooses what is recorded. */
            STRPTR name;

            nOutputName(nam, arg, &name);
            if(!name)
            {
                return FALSE;
            }
            nam->nam_CurrOutput = arg;
            if(nam->nam_OutputSwitch)
            {
                nch->nch_UpdateFlags |= UAF_SELECT_OUTPUT;
                Signal(nch->nch_Task, 1UL<<nch->nch_TaskMsgPort->mp_SigBit);
            }
            return TRUE;
        }

        case AHIC_Output_Query:
            return nam->nam_CurrOutput;
    }
    return FALSE;
    
}
/* \\\ */

