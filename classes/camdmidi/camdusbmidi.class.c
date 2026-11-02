/*
 *----------------------------------------------------------------------------
 *                         camdusbmidi class for Neptune
 *----------------------------------------------------------------------------
 * Based on the Poseidon camdusbmidi class by Chris Hodges <chrisly@platon42.de>.
 */

#include "debug.h"

#include "camdusbmidi.class.h"

/* /// "Lib Stuff" */
static const STRPTR libname = CLASS_NAME;

int libInit(struct NepMidiBase * nh)
{
    KPRINTF(10, ("libInit nh: 0x%08lx SysBase: 0x%08lx\n", nh, EXEC_BASE_NAME));

    nh->nh_UtilityBase = OpenLibrary("utility.library", 39);

#define	UtilityBase	nh->nh_UtilityBase

    if(UtilityBase)
    {
        NewList(&nh->nh_Bindings);
        NewList(&nh->nh_Ports);
    } else {
        KPRINTF(20, ("libInit: OpenLibrary(\"utility.library\", 39) failed!\n"));
        nh = NULL;
    }

    KPRINTF(10, ("libInit: Ok\n"));
    return(nh ? TRUE : FALSE);
}

int libExpunge(struct NepMidiBase * nh)
{
    KPRINTF(10, ("libExpunge nh: 0x%08lx\n", nh));
    CloseLibrary((struct Library *) UtilityBase);
    return(TRUE);
}

/* \\\ */

/*
 * ***********************************************************************
 * * Library functions                                                   *
 * ***********************************************************************
 */

#include "CAMDDriver.c"     /* the embedded driver: CAMDDriver[], CAMDDriver_NAMEOFFSET/NAMESIZE/NPORTSOFFSET */

/* The CAMD side. The class subtask writes the embedded driver to DEVS:Midi/<ShortID>;
   camd.library loads that file, which opens this class and calls usbCAMDOpenPort /
   usbCAMDClosePort (LVOs -90/-96). An open port is a struct CAMDAdapter on nh_Ports shared
   with the driver: received event packets reach camd through the slot's RX hook, and when camd
   has output (ActivateXmit) the driver signals the subtask, which pulls the bytes through
   the TX hook and packetizes them (nTxByte). See camd/camd_driver.c for the camd
   calling conventions. */

#define TX_CHUNK    64      /* bytes pulled from camd per hook call */
#define TX_PULL_MAX ((TX_CHUNK + 1) * 4)    /* most bytes one pull appends: a packet per byte, plus one
                                               SysEx cut flushing bytes held from an earlier pull */
#define OUTBUF_SIZE 8192    /* bulk OUT buffer: 2048 event packets */
#define INBUF_SIZE  1024    /* bulk IN buffer */

/* MIDI bytes carried by an event packet, indexed by its Code Index Number (low nibble of
   byte 0; USB MIDI 1.0 Table 4-1). 0 = reserved code, the packet is skipped. */
static const UBYTE cinlength[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

/* /// "usbAttemptInterfaceBinding()" */
struct NepClassMidi * usbAttemptInterfaceBinding(struct NepMidiBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    IPTR ifclass;
    IPTR subclass;
    IPTR proto;
    BOOL ismidi = FALSE;

    KPRINTF(1, ("nepMidiAttemptInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_Class, &ifclass,
                    IFA_SubClass, &subclass,
                    IFA_Protocol, &proto,
                    TAG_DONE);
        ismidi = (ifclass == AUDIO_CLASSCODE) && (subclass == AUDIO_MIDI_SUBCLASS) && nIsMidi10(ps, pif, (STRPTR) libname);
        CloseLibrary(ps);
    }
    return(ismidi ? usbForceInterfaceBinding(nh, pif) : NULL);
}
/* \\\ */

/* /// "nFindBinding()" */
/* Return the binding with this id (Forbid() held by the caller), or NULL. */
static struct NepClassMidi * nFindBinding(struct NepMidiBase *nh, STRPTR idstr)
{
    struct NepClassMidi *nch = (struct NepClassMidi *) nh->nh_Bindings.lh_Head;

    while(nch->nch_Node.ln_Succ)
    {
        if(!strcmp((char *) nch->nch_ShortID, (char *) idstr))
        {
            return nch;
        }
        nch = (struct NepClassMidi *) nch->nch_Node.ln_Succ;
    }
    return NULL;
}
/* \\\ */

/* /// "usbForceInterfaceBinding()" */
struct NepClassMidi * usbForceInterfaceBinding(struct NepMidiBase *nh, struct PsdInterface *pif)
{
    struct Library *ps;
    struct NepClassMidi *nch;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    STRPTR devname;
    UBYTE buf[64];
    struct Task *tmptask;

    KPRINTF(1, ("nepMidiForceInterfaceBinding(%08lx)\n", pif));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        if((nch = psdAllocVec(sizeof(struct NepClassMidi))))
        {
            STRPTR srcpos;
            STRPTR tarpos;
            UWORD cnt;

            psdGetAttrs(PGA_INTERFACE, pif, IFA_Config, &pc, TAG_END);
            psdGetAttrs(PGA_CONFIG, pc, CA_Device, &pd, TAG_END);
            psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);

            /* The id names the driver file and the CAMD device: the product name's
               alphanumerics, at most CAMDDriver_NAMESIZE - 1 of them. */
            srcpos = devname;
            tarpos = nch->nch_ShortID;
            cnt = CAMDDriver_NAMESIZE - 1;
            while(*srcpos && cnt)
            {
                if(((*srcpos >= 'A') && (*srcpos <= 'Z')) ||
                   ((*srcpos >= 'a') && (*srcpos <= 'z')) ||
                   ((*srcpos >= '0') && (*srcpos <= '9')))
                {
                    *tarpos++ = *srcpos;
                    cnt--;
                }
                srcpos++;
            }
            *tarpos = 0;
            if(!nch->nch_ShortID[0])
            {
                strcpy((char *) nch->nch_ShortID, "USBMIDI");
            }
            /* two identical devices would share the driver file: suffix a digit until unique */
            Forbid();
            if(nFindBinding(nh, nch->nch_ShortID))
            {
                UWORD len = strlen((char *) nch->nch_ShortID);
                UBYTE suffix = '2';

                if(len > CAMDDriver_NAMESIZE - 2)
                {
                    len = CAMDDriver_NAMESIZE - 2;
                }
                nch->nch_ShortID[len + 1] = 0;
                do
                {
                    nch->nch_ShortID[len] = suffix++;
                } while(nFindBinding(nh, nch->nch_ShortID) && (suffix <= '9'));
            }
            Permit();

            nch->nch_ClsBase = nh;
            nch->nch_Device = NULL;
            nch->nch_Interface = pif;

            psdSafeRawDoFmt(buf, 64, CLASS_NAME "<%08lx>", nch);
            nch->nch_ReadySignal = SIGB_SINGLE;
            nch->nch_ReadySigTask = FindTask(NULL);
            SetSignal(0, SIGF_SINGLE);
            if((tmptask = psdSpawnSubTask(buf, nMidiTask, nch)))
            {
                psdBorrowLocksWait(tmptask, 1UL<<nch->nch_ReadySignal);
                if(nch->nch_Task)
                {
                    nch->nch_ReadySigTask = NULL;
                    Forbid();
                    AddTail(&nh->nh_Bindings, &nch->nch_Node);
                    Permit();

                    psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                                   NEP_BOUND1_TXT,
                                   devname);
                    CloseLibrary(ps);
                    return(nch);
                }
            }
            nch->nch_ReadySigTask = NULL;
            psdFreeVec(nch);
        }
        CloseLibrary(ps);
    }
    return(NULL);
}
/* \\\ */

/* /// "usbReleaseInterfaceBinding()" */
void usbReleaseInterfaceBinding(struct NepMidiBase *nh, struct NepClassMidi *nch)
{
    struct Library *ps;
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    STRPTR devname;

    KPRINTF(1, ("nepMidiReleaseInterfaceBinding(%08lx)\n", nch));
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        Forbid();
        Remove(&nch->nch_Node);
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
        psdGetAttrs(PGA_INTERFACE, nch->nch_Interface, IFA_Config, &pc, TAG_END);
        psdGetAttrs(PGA_CONFIG, pc, CA_Device, &pd, TAG_END);
        psdGetAttrs(PGA_DEVICE, pd, DA_ProductName, &devname, TAG_END);
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                       NEP_RELEASED_TXT,
                       devname);
        psdFreeVec(nch);
        CloseLibrary(ps);
    }
}
/* \\\ */

/* /// "usbGetAttrsA()" */
LONG (usbGetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepMidiBase * nh asm("a6"))
{

    struct TagItem *ti;
    LONG count = 0;

    KPRINTF(1, ("nepMidiGetAttrsA(%ld, %08lx, %08lx)\n", type, usbstruct, tags));
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
                 *((STRPTR *) ti->ti_Data) = "USB MIDI CAMD Interface class";
                 count++;
             }
             if((ti = FindTagItem(UCCA_HasClassCfgGUI, tags)))
             {
                 /* nothing to configure: CAMD names its ports itself (MidiPorts) */
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
                 /* A binding made before DOS cannot write its driver file: the stack
                    releases and rebinds it once DOS is up, the subtask is a Process then. */
                 *((IPTR *) ti->ti_Data) = TRUE;
                 count++;
             }
             if((ti = FindTagItem(UCCA_UsingDefaultCfg, tags)))
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
LONG (usbSetAttrsA)(ULONG type asm("d0"), APTR usbstruct asm("a0"), struct TagItem * tags asm("a1"), struct NepMidiBase * nh asm("a6"))
{
    return(0);
}
/* \\\ */

/* /// "usbDoMethodA()" */
IPTR (usbDoMethodA)(ULONG methodid asm("d0"), IPTR * methoddata asm("a1"), struct NepMidiBase * nh asm("a6"))
{

    KPRINTF(10, ("Do Method %ld\n", methodid));
    switch(methodid)
    {
        case UCM_AttemptInterfaceBinding:
            return((IPTR) usbAttemptInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ForceInterfaceBinding:
            return((IPTR) usbForceInterfaceBinding(nh, (struct PsdInterface *) methoddata[0]));

        case UCM_ReleaseInterfaceBinding:
            usbReleaseInterfaceBinding(nh, (struct NepClassMidi *) methoddata[0]);
            return(TRUE);

        default:
            break;
    }
    return(0);
}
/* \\\ */

/* /// "nFindPort()" */
/* Return this binding's CAMD port `port` (= USB-MIDI cable) from nh_Ports, or NULL.
   Call under Forbid(): usbCAMDClosePort frees ports. */
static struct CAMDAdapter * nFindPort(struct NepClassMidi *nch, UWORD port)
{
    for(struct Node *node = nch->nch_ClsBase->nh_Ports.lh_Head; node->ln_Succ; node = node->ln_Succ)
    {
        struct CAMDAdapter *ca = (struct CAMDAdapter *) node;

        if((ca->ca_Binding == nch) && (ca->ca_PortNum == port))
        {
            return ca;
        }
    }
    return NULL;
}
/* \\\ */

/* /// "usbCAMDOpenPort()" */
/* Called by the driver's OpenPort: give CAMD port `port` of the device the driver file
   `idstr` was written for a new struct CAMDAdapter on nh_Ports. NULL if the device is gone,
   the port is beyond its cables or already open, or there is no memory. */
APTR (usbCAMDOpenPort)(APTR xmitfct asm("a0"), APTR recvfct asm("a1"), APTR userdata asm("a2"), STRPTR idstr asm("a3"), ULONG port asm("d0"), struct NepMidiBase * nh asm("a6"))
{
    struct CAMDAdapter *slot;
    BOOL ok = FALSE;

    KPRINTF(10, ("Open Port %ld, ID=%s\n", port, idstr));
    if((port >= CAMD_MAX_PORTS) || !(slot = AllocVec(sizeof(*slot), MEMF_PUBLIC|MEMF_CLEAR)))
    {
        return NULL;
    }
    Forbid();
    struct NepClassMidi *nch = nFindBinding(nh, idstr);
    if(nch && nch->nch_Task && (port < nch->nch_NumCables) && !nFindPort(nch, port))
    {
        slot->ca_Binding = nch;
        slot->ca_PortNum = port;
        slot->ca_SigTask = nch->nch_Task;
        slot->ca_SigMask = 1UL << nch->nch_TaskMsgPort->mp_SigBit;
        slot->ca_TXFunc = (CAMDXmitFunc) xmitfct;
        slot->ca_RXFunc = (CAMDRecvFunc) recvfct;
        slot->ca_UserData = userdata;
        /* ca_IsOpen stays FALSE: the driver sets it once its hooks are in */
        AddTail(&nh->nh_Ports, &slot->ca_Node);
        ok = TRUE;
    }
    Permit();
    if(!ok)
    {
        KPRINTF(20, ("No CAMD port %ld for '%s'\n", port, idstr));
        FreeVec(slot);
        return NULL;
    }
    return slot;
}
/* \\\ */

/* /// "usbCAMDClosePort()" */
/* Called by the driver's ClosePort with the port usbCAMDOpenPort returned: unlink and free
   it. Every party here is a task (camd's senders included, camd.intro), so Forbid() is the
   exclusion the driver's ActivateXmit and the class task pair with. */
void (usbCAMDClosePort)(APTR adapter asm("a0"), struct NepMidiBase * nh asm("a6"))
{
    BOOL found = FALSE;

    KPRINTF(10, ("Close Port %08lx\n", adapter));
    Forbid();
    for(struct Node *node = nh->nh_Ports.lh_Head; node->ln_Succ; node = node->ln_Succ)
    {
        if(node == adapter)
        {
            Remove(node);
            found = TRUE;
            break;
        }
    }
    Permit();
    if(found)
    {
        FreeVec(adapter);
    }
}
/* \\\ */

/**************************************************************************/

#undef  ps
#define ps nch->nch_Base

/* /// "nMidiTask()" */
void nMidiTask()
{
    struct NepClassMidi *nch;
    struct PsdPipe *pp;
    ULONG sigmask;
    ULONG sigs;
    UBYTE *buf;
    LONG ioerr;

    if((nch = nAllocMidi()))
    {
        Forbid();
        if(nch->nch_ReadySigTask)
        {
            Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
        }
        Permit();
        nWriteDriver(nch);
        sigmask = (1L<<nch->nch_TaskMsgPort->mp_SigBit)|SIGBREAKF_CTRL_C;
        buf = nch->nch_EPInBuf;
        psdSendPipe(nch->nch_EPInPipe, buf, INBUF_SIZE);
        do
        {
            sigs = Wait(sigmask);
            while((pp = (struct PsdPipe *) GetMsg(nch->nch_TaskMsgPort)))
            {
                if(pp == nch->nch_EPInPipe)
                {
                    if(!(ioerr = psdGetPipeError(pp)))
                    {
                        nParseMidi(nch, buf, psdGetPipeActual(pp));
                    } else {
                        KPRINTF(10, ("Bulk IN failed %ld\n", ioerr));
                        psdDelayMS(200);    /* don't spin on a persistent error */
                    }
                    psdSendPipe(nch->nch_EPInPipe, buf, INBUF_SIZE);
                }
            }
            nTransmit(nch);
        } while(!(sigs & SIGBREAKF_CTRL_C));
        KPRINTF(20, ("Going down the river!\n"));
        psdAbortPipe(nch->nch_EPInPipe);
        psdWaitPipe(nch->nch_EPInPipe);
        nFreeMidi(nch);
    }
}
/* \\\ */

/* TX path: camd hands us a plain MIDI 1.0 byte stream; USB-MIDI wants event packets.
 *
 * MIDI byte stream: a status byte (80-FF) starts a message, data bytes (00-7F) follow.
 *  - Channel messages 8x-Ex carry 2 data bytes, except Cx (program change) and Dx (channel
 *    pressure) with 1. "Running status": a sender may omit a repeated status byte, so
 *    90 3C 7F 3E 7F is two note-ons.
 *  - System common F1-F7: F1 (MTC quarter frame) and F3 (song select) take 1 data byte,
 *    F2 (song position) 2, F6 (tune request) none; F4/F5 are undefined. No running status.
 *  - SysEx: F0, any number of data bytes, F7.
 *  - Realtime F8-FF (clock, start, stop, ...): one byte, may appear anywhere, even between
 *    the bytes of another message, which it does not interrupt.
 *
 * USB-MIDI event packet (USB MIDI 1.0 section 4): 4 bytes, one complete message each.
 *  - byte 0: cable number (high nibble, = CAMD port) | Code Index Number (low nibble, the
 *    kind and length of the message: 8-E = channel message of that status, 2/3 = 2/3-byte
 *    system common, 5 = 1-byte system common or SysEx end, 4 = SysEx start or middle,
 *    6/7 = SysEx end with 2/3 bytes, F = single byte);
 *  - bytes 1-3: the MIDI bytes, status first, unused ones zero.
 *  There is no running status: every packet carries its status byte. A SysEx goes out
 *  as 3-byte CIN 4 packets, the last 1-3 bytes (ending in F7) as CIN 5/6/7.
 *
 * nTxByte() takes one stream byte at a time and keeps per-port state in struct UsbMidiTx
 * (the message being assembled). */

/* /// "nTxPacket()" */
/* Append one event packet at out (byte 0 = cable << 4 | cin, then the three MIDI bytes,
   unused ones passed as 0); return out + 4. */
static UBYTE * nTxPacket(UBYTE *out, UBYTE cable, UBYTE cin, UBYTE b0, UBYTE b1, UBYTE b2)
{
    out[0] = USBMIDI_HEADER(cable, cin);
    out[1] = b0;
    out[2] = b1;
    out[3] = b2;
    return out + 4;
}
/* \\\ */

/* /// "nTxMessage()" */
/* The last data byte of a message arrived: emit tx_Status + d0 (+ d1) as one packet; len =
   message length including the status (2 or 3). A channel message's CIN is its status's
   high nibble, and the state goes back to "waiting for data" with the status kept, so the
   next data bytes (running status) form another message of the same kind. System common
   uses CIN 2/3 = its length and is not repeated by running status: state is cleared. */
static UBYTE * nTxMessage(struct UsbMidiTx *tx, UBYTE cable, UBYTE len, UBYTE d0, UBYTE d1, UBYTE *out)
{
    UBYTE cin;

    if(tx->tx_Status < MS_System)
    {
        /* channel message 8x-Ex: CIN 8-E = the status's high nibble; keep the status and
           wait for the same number of data bytes again (running status) */
        cin = tx->tx_Status >> 4;
        tx->tx_State = (len == 2) ? USBMIDI_TX_1PARAM : USBMIDI_TX_2PARAM_1;
    } else {
        /* system common F1/F2/F3: CIN 2/3 = the message length; no running status, so
           further data bytes are dropped until the next status byte */
        cin = len;
        tx->tx_State = USBMIDI_TX_UNKNOWN;
    }
    return nTxPacket(out, cable, cin, tx->tx_Status, d0, d1);
}
/* \\\ */

/* /// "nTxData()" */
/* A data byte (00-7F): what it means depends on the state. In a message it is either held
   (first of two data bytes) or completes the message. In SysEx it is collected; every third
   one fills a CIN 4 packet. With no status seen yet (USBMIDI_TX_UNKNOWN) it belongs to
   nothing we can encode and is dropped. */
static UBYTE * nTxData(struct UsbMidiTx *tx, UBYTE cable, UBYTE b, UBYTE *out)
{
    switch(tx->tx_State)
    {
    case USBMIDI_TX_1PARAM:         /* the only data byte (e.g. C0 05): message complete */
        return nTxMessage(tx, cable, 2, b, 0, out);
    case USBMIDI_TX_2PARAM_1:       /* first of two (e.g. the note of 90 3C 7F): hold it */
        tx->tx_Data[0] = b;
        tx->tx_State = USBMIDI_TX_2PARAM_2;
        return out;
    case USBMIDI_TX_2PARAM_2:       /* second of two (the velocity): message complete */
        return nTxMessage(tx, cable, 3, tx->tx_Data[0], b, out);
    case USBMIDI_TX_SYSEX_0:        /* SysEx: first byte of the next 3-byte packet */
        tx->tx_Data[0] = b;
        tx->tx_State = USBMIDI_TX_SYSEX_1;
        return out;
    case USBMIDI_TX_SYSEX_1:        /* SysEx: second byte */
        tx->tx_Data[1] = b;
        tx->tx_State = USBMIDI_TX_SYSEX_2;
        return out;
    case USBMIDI_TX_SYSEX_2:        /* SysEx: third byte fills the packet; more may follow */
        tx->tx_State = USBMIDI_TX_SYSEX_0;
        return nTxPacket(out, cable, USBMIDI_CIN_SYSEX, tx->tx_Data[0], tx->tx_Data[1], b);
    }
    return out;                     /* USBMIDI_TX_UNKNOWN: no status to attach it to */
}
/* \\\ */

/* /// "nTxSysExEnd()" */
/* F7 ends a SysEx: send the 0-2 bytes still held plus F7 as the closing packet, whose CIN
   says how many bytes it has (5 = F7 alone, 6 = one held byte + F7, 7 = two + F7). An F7
   outside SysEx has nothing to close and is dropped. */
static UBYTE * nTxSysExEnd(struct UsbMidiTx *tx, UBYTE cable, UBYTE *out)
{
    UBYTE state = tx->tx_State;

    tx->tx_State = USBMIDI_TX_UNKNOWN;      /* SysEx over (or there was none) */
    switch(state)
    {
    case USBMIDI_TX_SYSEX_0:        /* last packet was full: F7 goes alone */
        return nTxPacket(out, cable, USBMIDI_CIN_SYSEX_END1, MS_EOX, 0, 0);
    case USBMIDI_TX_SYSEX_1:        /* one byte held (F0 itself for F0 F7): byte + F7 */
        return nTxPacket(out, cable, USBMIDI_CIN_SYSEX_END2, tx->tx_Data[0], MS_EOX, 0);
    case USBMIDI_TX_SYSEX_2:        /* two bytes held: both + F7 */
        return nTxPacket(out, cable, USBMIDI_CIN_SYSEX_END3, tx->tx_Data[0], tx->tx_Data[1], MS_EOX);
    }
    return out;                     /* F7 outside SysEx: nothing to end, dropped */
}
/* \\\ */

/* /// "nTxSysExCut()" */
/* A status byte other than F7 or realtime arrived inside a SysEx. MIDI 1.0 counts that as
   the end of the SysEx (the sender should have sent F7 first): send the 0-2 bytes still held
   as the closing packet (CIN 5 = one byte, 6 = two; no F7), so they are not lost and the
   device sees the SysEx closed. The caller sets the new state. */
static UBYTE * nTxSysExCut(struct UsbMidiTx *tx, UBYTE cable, UBYTE *out)
{
    switch(tx->tx_State)
    {
    case USBMIDI_TX_SYSEX_1:
        return nTxPacket(out, cable, USBMIDI_CIN_SYSEX_END1, tx->tx_Data[0], 0, 0);
    case USBMIDI_TX_SYSEX_2:
        return nTxPacket(out, cable, USBMIDI_CIN_SYSEX_END2, tx->tx_Data[0], tx->tx_Data[1], 0);
    }
    return out;                     /* not in SysEx, or the last packet was full */
}
/* \\\ */

/* /// "nTxSystem()" */
/* A system byte F0-F7. F0 starts a SysEx (held as its first byte); F1/F3 wait for one data
   byte, F2 for two; F6 has no data and goes out at once as CIN 5; F7 ends a SysEx. F4/F5 are
   undefined in MIDI 1.0 and have no CIN, so they are dropped. Any of these ends a channel
   message in progress, as on a MIDI cable; a SysEx they end was flushed by nTxByte. */
static UBYTE * nTxSystem(struct UsbMidiTx *tx, UBYTE cable, UBYTE b, UBYTE *out)
{
    switch(b)
    {
    case MS_SysEx:      /* SysEx start: F0 is the first byte of the first CIN 4 packet */
        tx->tx_Data[0] = b;
        tx->tx_State = USBMIDI_TX_SYSEX_1;
        return out;
    case MS_QtrFrame:   /* MTC quarter frame, song select: wait for the one data byte */
    case MS_SongSelect:
        tx->tx_Status = b;
        tx->tx_State = USBMIDI_TX_1PARAM;
        return out;
    case MS_SongPos:    /* song position: wait for the two data bytes */
        tx->tx_Status = b;
        tx->tx_State = USBMIDI_TX_2PARAM_1;
        return out;
    case MS_TuneReq:    /* tune request: complete by itself */
        tx->tx_State = USBMIDI_TX_UNKNOWN;
        return nTxPacket(out, cable, USBMIDI_CIN_SYSEX_END1, b, 0, 0);
    case MS_EOX:        /* SysEx end */
        return nTxSysExEnd(tx, cable, out);
    }
    tx->tx_State = USBMIDI_TX_UNKNOWN;          /* F4, F5: undefined, no CIN; dropped */
    return out;
}
/* \\\ */

/* /// "nTxByte()" */
/* Feed one stream byte for cable (see the TX path note above nTxPacket); append the packets
   it completes, if any, at out and return out advanced by 0, 4 or 8 bytes (8: a status byte
   that ends an unterminated SysEx and is itself complete, F6). Realtime bytes go out at once
   as CIN F and leave the state alone, so a message or SysEx they interrupt continues; a
   channel status byte starts a message and waits for its data. */
static UBYTE * nTxByte(struct UsbMidiTx *tx, UBYTE cable, UBYTE b, UBYTE *out)
{
    if(b >= MS_RealTime)
    {
        /* realtime (clock, start, stop, ...): one-byte packet now; state untouched */
        return nTxPacket(out, cable, USBMIDI_CIN_SINGLEBYTE, b, 0, 0);
    }
    if((b & MIDI_STATUSBIT) && (b != MS_EOX))
    {
        /* any other status byte ends a SysEx in progress: flush its held bytes first */
        out = nTxSysExCut(tx, cable, out);
    }
    if(b >= MS_System)
    {
        /* system common or SysEx framing */
        return nTxSystem(tx, cable, b, out);
    }
    if(b & MIDI_STATUSBIT)
    {
        /* channel status 8x-Ex: start a message. Program change (Cx) and channel pressure
           (Dx) take one data byte, the others two. An incomplete message in progress is
           dropped, as a MIDI receiver would discard it. */
        tx->tx_Status = b;
        tx->tx_State = (((b & MS_StatBits) == MS_Prog) || ((b & MS_StatBits) == MS_ChanPress))
                       ? USBMIDI_TX_1PARAM : USBMIDI_TX_2PARAM_1;
        return out;
    }
    /* data byte 00-7F: meaning depends on the state */
    return nTxData(tx, cable, b, out);
}
/* \\\ */

/* /// "nParseMidi()" */
/* Deliver the event packets of one bulk IN transfer to camd: the Code Index Number gives
   the byte count (Table 4-1; 0 for the reserved codes = skip), the cable number the port. */
void nParseMidi(struct NepClassMidi *nch, UBYTE *buf, ULONG len)
{
    while(len >= 4)
    {
        UWORD cable = USBMIDI_CABLE(buf[0]);
        UWORD cnt = cinlength[USBMIDI_CIN_OF(buf[0])];

        KPRINTF(1, ("Msg: %02lx %02lx %02lx %02lx\n", buf[0], buf[1], buf[2], buf[3]));
        if(cnt && (cable < nch->nch_InCables))
        {
            Forbid();
            struct CAMDAdapter *ca = nFindPort(nch, cable);
            if(ca && ca->ca_IsOpen)
            {
                CallHookA(&ca->ca_CAMDRXFunc, (Object *) &buf[1], (APTR) (ULONG) cnt);
            }
            Permit();
        }
        buf += 4;
        len -= 4;
    }
}
/* \\\ */

/* /// "nAllocMidi()" */
struct NepClassMidi * nAllocMidi(void)
{
    struct Task *thistask;
    struct NepClassMidi *nch;

    thistask = FindTask(NULL);
    nch = thistask->tc_UserData;
    do
    {
        if(!(nch->nch_Base = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
        {
            Alert(AG_OpenLib);
            break;
        }
        psdGetAttrs(PGA_INTERFACE, nch->nch_Interface,
                    IFA_Config, &nch->nch_Config,
                    IFA_InterfaceNum, &nch->nch_IfNum,
                    TAG_END);
        psdGetAttrs(PGA_CONFIG, nch->nch_Config,
                    CA_Device, &nch->nch_Device,
                    TAG_END);

        nch->nch_EPIn = psdFindEndpoint(nch->nch_Interface, NULL,
                                        EA_IsIn, TRUE,
                                        EA_TransferType, USEAF_BULK,
                                        TAG_END);

        nch->nch_EPOut = psdFindEndpoint(nch->nch_Interface, NULL,
                                         EA_IsIn, FALSE,
                                         EA_TransferType, USEAF_BULK,
                                         TAG_END);
        if(!(nch->nch_EPIn && nch->nch_EPOut))
        {
            KPRINTF(1, ("Ooops!?! No Endpoints defined?\n"));
            psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                           "No Bulk-In or Bulk-Out Endpoint!");
            break;
        }
        nch->nch_InCables = nCableCount(nch, nch->nch_EPIn);
        nch->nch_OutCables = nCableCount(nch, nch->nch_EPOut);
        nch->nch_NumCables = max(nch->nch_InCables, nch->nch_OutCables);
        if((nch->nch_TaskMsgPort = CreateMsgPort()))
        {
            if((nch->nch_EPInPipe = psdAllocPipe(nch->nch_Device, nch->nch_TaskMsgPort, nch->nch_EPIn)))
            {
                psdSetAttrs(PGA_PIPE, nch->nch_EPInPipe,
                            PPA_NakTimeout, FALSE,
                            PPA_AllowRuntPackets, TRUE,
                            TAG_END);

                if((nch->nch_EPOutPipe = psdAllocPipe(nch->nch_Device, nch->nch_TaskMsgPort, nch->nch_EPOut)))
                {
                    psdSetAttrs(PGA_PIPE, nch->nch_EPOutPipe,
                                PPA_NakTimeout, TRUE,
                                PPA_AllowRuntPackets, TRUE,
                                PPA_NakTimeoutTime, 5000,
                                TAG_END);
                    if((nch->nch_EPInBuf = psdAllocVec(INBUF_SIZE)))
                    {
                        if((nch->nch_EPOutBuf = psdAllocVec(OUTBUF_SIZE)))
                        {
                            nch->nch_Task = thistask;
                            return(nch);
                        }
                        psdFreeVec(nch->nch_EPInBuf);
                    }
                    psdFreePipe(nch->nch_EPOutPipe);
                }
                psdFreePipe(nch->nch_EPInPipe);
            }
            DeleteMsgPort(nch->nch_TaskMsgPort);
        }
    } while(FALSE);
    CloseLibrary(nch->nch_Base);
    Forbid();
    nch->nch_Task = NULL;
    if(nch->nch_ReadySigTask)
    {
        Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
    }
    return(NULL);
}
/* \\\ */

/* /// "nFreeMidi()" */
/* Detach the binding's CAMD ports (the driver reads ca_SigTask under Forbid), pull what
   camd still has queued for them so camd never waits on a driver that no longer transmits
   (the driver drains later ActivateXmits of a detached port itself), then free the USB side. */
void nFreeMidi(struct NepClassMidi *nch)
{
    UBYTE junk[TX_CHUNK];

    Forbid();
    for(struct Node *node = nch->nch_ClsBase->nh_Ports.lh_Head; node->ln_Succ; node = node->ln_Succ)
    {
        struct CAMDAdapter *ca = (struct CAMDAdapter *) node;

        if(ca->ca_Binding == nch)
        {
            ca->ca_Binding = NULL;
            ca->ca_SigTask = NULL;
            /* drop what camd still has queued so it sees the port idle again; terminates:
               camd's senders are tasks, so the queue only shrinks while we hold Forbid(),
               which also keeps usbCAMDClosePort from freeing the port meanwhile */
            while(ca->ca_IsOpen && CallHookA(&ca->ca_CAMDTXFunc, (Object *) junk, (APTR) sizeof(junk)))
                ;
        }
    }
    Permit();
    psdFreeVec(nch->nch_EPOutBuf);
    psdFreeVec(nch->nch_EPInBuf);
    psdFreePipe(nch->nch_EPOutPipe);
    psdFreePipe(nch->nch_EPInPipe);
    DeleteMsgPort(nch->nch_TaskMsgPort);
    CloseLibrary(nch->nch_Base);
    Forbid();
    nch->nch_Task = NULL;
    if(nch->nch_ReadySigTask)
    {
        Signal(nch->nch_ReadySigTask, 1L<<nch->nch_ReadySignal);
    }
}
/* \\\ */

/* /// "nPullPort()" */
/* Pull up to TX_CHUNK bytes of one port's queued output from camd through the driver's TX
   hook and append their event packets at *out. A port beyond the device's OUT cables is
   drained the same way, but nothing is sent for it. Return the bytes pulled, 0 once camd's
   queue for the port is empty. Call under Forbid(): usbCAMDClosePort frees ports. */
static ULONG nPullPort(struct NepClassMidi *nch, struct CAMDAdapter *ca, UBYTE **out)
{
    UBYTE tmp[TX_CHUNK];
    ULONG num = CallHookA(&ca->ca_CAMDTXFunc, (Object *) tmp, (APTR) sizeof(tmp));

    if(ca->ca_PortNum < nch->nch_OutCables)
    {
        for(ULONG i = 0; i < num; i++)
        {
            *out = nTxByte(&ca->ca_TX, ca->ca_PortNum, tmp[i], *out);
        }
    }
    return num;
}
/* \\\ */

/* /// "nPullPorts()" */
/* One pass over the open ports: pull once from each of this device's ports with output
   pending (ActivateXmit set ca_TXActive), stopping early when *out passes last (the buffer
   could not take another pull). Return TRUE if any port gave bytes, i.e. another pass may
   find more. */
static BOOL nPullPorts(struct NepClassMidi *nch, UBYTE **out, UBYTE *last)
{
    BOOL got = FALSE;

    Forbid();
    for(struct Node *node = nch->nch_ClsBase->nh_Ports.lh_Head; node->ln_Succ && (*out <= last); node = node->ln_Succ)
    {
        struct CAMDAdapter *ca = (struct CAMDAdapter *) node;

        if((ca->ca_Binding == nch) && ca->ca_TXActive && nPullPort(nch, ca, out))
        {
            got = TRUE;
        }
    }
    Permit();
    return got;
}
/* \\\ */

/* /// "nTransmit()" */
/* Send what CAMD programs have queued for this device's ports. camd keeps the bytes; the
   driver's ActivateXmit only wakes this task, which pulls them (nPullPorts) and sends the
   packets of all ports together in one bulk OUT transfer, outside Forbid(). A transfer goes
   out when the buffer fills or when no port has more. A wake-up with nothing queued costs
   one hook call per pending port. */
void nTransmit(struct NepClassMidi *nch)
{
    UBYTE *buf = nch->nch_EPOutBuf;
    UBYTE *last = buf + OUTBUF_SIZE - TX_PULL_MAX;  /* last position one more pull still fits from */
    UBYTE *out = buf;
    BOOL again;

    do
    {
        again = nPullPorts(nch, &out, last);
        if((out > last) || (!again && (out > buf)))
        {
            psdDoPipe(nch->nch_EPOutPipe, buf, out - buf);
            out = buf;
        }
    } while(again);
}
/* \\\ */

/* /// "nCableCount()" */
/* Return the embedded MIDI jacks of a bulk endpoint from its class-specific descriptor
   (USB MIDI 1.0 6.2.2, bNumEmbMIDIJack): the cable numbers that endpoint carries. 1 when the
   descriptor is missing or out of range. */
UWORD nCableCount(struct NepClassMidi *nch, struct PsdEndpoint *pep)
{
    struct UsbMIDIStreamingEndpointDesc *msd = NULL;
    APTR desc = psdFindDescriptor(nch->nch_Device, NULL,
                                  DDA_Endpoint, pep,
                                  DDA_DescriptorType, UDT_CS_ENDPOINT,
                                  DDA_CS_SubType, UDST_AUDIO_MIDI_EP_GENERAL,
                                  TAG_END);

    if(desc)
    {
        psdGetAttrs(PGA_DESCRIPTOR, desc, DDA_DescriptorData, &msd, TAG_END);
    }
    if(msd && (msd->bNumEmbMIDIJack >= 1) && (msd->bNumEmbMIDIJack <= CAMD_MAX_PORTS))
    {
        return msd->bNumEmbMIDIJack;
    }
    return 1;
}
/* \\\ */

/* /// "nBuildDriver()" */
/* Copy the embedded CAMD driver to image, its name[] set to the id camd matches against the
   file name and its NPorts to the device's cable count (camd reads NPorts before Init). */
static void nBuildDriver(struct NepClassMidi *nch, UBYTE *image)
{
    CopyMem((APTR) CAMDDriver, image, sizeof(CAMDDriver));
    memset(&image[CAMDDriver_NAMEOFFSET], 0, CAMDDriver_NAMESIZE);
    strcpy((char *) &image[CAMDDriver_NAMEOFFSET], (char *) nch->nch_ShortID);
    image[CAMDDriver_NPORTSOFFSET] = nch->nch_NumCables;
}
/* \\\ */

/* /// "nDriverIsCurrent()" */
/* Return TRUE if the file at path holds exactly the size bytes of image; scratch takes
   size + 1 bytes, so a longer file reads short of equal. */
static BOOL nDriverIsCurrent(struct Library *DOSBase, STRPTR path, UBYTE *image, ULONG size, UBYTE *scratch)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    BOOL same;

    if(!fh)
    {
        return FALSE;
    }
    same = (Read(fh, scratch, size + 1) == (LONG) size) && !memcmp(scratch, image, size);
    Close(fh);
    return same;
}
/* \\\ */

/* /// "nSaveDriver()" */
/* Write size bytes of image to path, creating DEVS:Midi if the first open fails. */
static BOOL nSaveDriver(struct NepClassMidi *nch, struct Library *DOSBase, STRPTR path, UBYTE *image, ULONG size)
{
    BPTR fh = Open(path, MODE_NEWFILE);
    BOOL ok;

    if(!fh)
    {
        BPTR lock = CreateDir("DEVS:Midi");

        if(!lock)
        {
            return FALSE;
        }
        UnLock(lock);
        psdAddErrorMsg(RETURN_OK, (STRPTR) libname, "Created directory '%s'!", (STRPTR) "DEVS:Midi");
        if(!(fh = Open(path, MODE_NEWFILE)))
        {
            return FALSE;
        }
    }
    ok = (Write(fh, image, size) == (LONG) size);
    Close(fh);
    return ok;
}
/* \\\ */

/* /// "nWriteDriver()" */
/* Write the device's CAMD driver to DEVS:Midi/<ShortID> unless the file already holds that
   image, so a driver fix reaches an installed system and an unchanged file is left alone.
   Runs in the class subtask, a Process whenever DOS is up; before DOS there is nothing to
   write to and the UCCA_AfterDOSRestart rebind comes back here. */
void nWriteDriver(struct NepClassMidi *nch)
{
    ULONG size = sizeof(CAMDDriver);
    UBYTE path[64];

    if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
    {
        psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                       "No DOS yet, CAMD MIDI driver for '%s' not written.", nch->nch_ShortID);
        return;
    }
    struct Library *DOSBase = OpenLibrary("dos.library", 37);
    UBYTE *image = psdAllocVec(size * 2 + 1);   /* the image, then the file read back */

    psdSafeRawDoFmt(path, sizeof(path), "DEVS:Midi/%s", nch->nch_ShortID);
    if(DOSBase && image)
    {
        nBuildDriver(nch, image);
        if(!nDriverIsCurrent(DOSBase, path, image, size, image + size))
        {
            if(nSaveDriver(nch, DOSBase, path, image, size))
            {
                psdAddErrorMsg(RETURN_OK, (STRPTR) libname,
                               "Generated CAMD MIDI driver '%s' (%ld in, %ld out cable(s)).",
                               path, (ULONG) nch->nch_InCables, (ULONG) nch->nch_OutCables);
            } else {
                psdAddErrorMsg(RETURN_ERROR, (STRPTR) libname,
                               "Couldn't generate CAMD MIDI driver '%s'!", path);
            }
        }
    }
    psdFreeVec(image);
    CloseLibrary(DOSBase);  /* NULL is ignored */
}
/* \\\ */
