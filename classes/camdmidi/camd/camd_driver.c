/* camd_driver.c - the CAMD MIDI driver camdusbmidi.usbclass writes to DEVS:Midi/<id>.
 *
 * camd.library LoadSeg()s this file and calls the MidiDeviceData entry points; the driver
 * opens camdusbmidi.usbclass and hands every port's traffic to the class through the two
 * hooks in struct CAMDAdapter. It follows Commodore's drivers.doc (the CAMD 37.14 release,
 * Aminet mus/midi/camd.lha) and also works with the camd40/AROS rewrite:
 *   Init       takes no parameters (A6 is not the exec base under V37; camd40 puts SysBase
 *              there): read $4. V37 reads NPorts after Init, Amiga Inc.'s V40 before it,
 *              so the table carries the maximum and the class patches the device's cable
 *              count into the file it writes (CAMDDriver_NPORTSOFFSET), as it patches name[].
 *   OpenPort   A3 MidiDeviceData, D0 port (V37 sets only the low byte), A0 transmit
 *              handler, A1 receive handler, A2 userdata; returns the MidiPortData.
 *   ClosePort  A3 MidiDeviceData, D0 port.
 *   transmit   A2 = userdata; D0 bits 0-7 = the byte. V37: D1 = 0 means more data, != 0
 *              means this was the last byte - send it, then do not call transmit again
 *              until ActivateXmit. camd40/V40: D0 = 0x100 = nothing left, calling it idle
 *              is harmless. Serial drivers call it from the transmit interrupt, and camd's
 *              senders are always tasks (camd.intro), so Forbid() around a pull gives the
 *              exclusion the interrupt model assumes.
 *   receive    D0 bits 0-7 = the byte, bit 15 = overrun (data lost), A2 = userdata.
 *   ActivateXmit  camd has output queued; V37 passes A0 = the MidiPortData OpenPort
 *              returned and may call it while the driver is still active, camd40 passes A2
 *              userdata + D0 port: one stub per port needs none of that.
 *   Flags      "currently none" in V37; bit 0 = 1 is camd40's "new format", which it requires.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#define CAMDUSBMIDI_BASE_NAME nh    /* the inline usbCAMD* stubs jump through our OpenLibrary'd base */
#include <proto/camdusbmidi.h>

#include <midi/camddevices.h>
#include "camdusbmidi.h"
#include <neptune_version.h>

extern BOOL Init(void);
extern VOID Expunge(VOID);
extern struct MidiPortData *OpenPort(struct MidiDeviceData *data asm("a3"), LONG portnum asm("d0"),
                                     CAMDXmitFunc transmitfunc asm("a0"), CAMDRecvFunc receivefunc asm("a1"),
                                     APTR userdata asm("a2"));
extern VOID ClosePort(struct MidiDeviceData *data asm("a3"), LONG portnum asm("d0"));

char name[], vers[];

/*** Identification data must follow directly *********************************/

#define CAMDPORTCOUNT   CAMD_MAX_PORTS

/* CAMD driver image layout, forced by the shared ldscripts/module.lds (which KEEPs
   .text.entry then .text.modhdr first in .text) - the same contract every .class and
   .library here uses, with a MidiDeviceData as the header instead of a Resident:
       offset 0:  this 4-byte entry stub          (driver fails gracefully if run as a program)
       offset 4:  the MidiDeviceData table below   (camd.library reads MDD_Magic here)
   At -O2 a `return -1` leaf compiles to exactly `moveq #-1,d0 ; rts` (4 bytes) - the same idiom
   as the class skeleton's doNotExecute. gen_camddriver.py asserts the stub + 'MDEV' land at
   offset 4, so any size drift fails the build instead of shipping a misaligned table. */
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void) { return -1; }

/* Pinned to .text.modhdr so it lands right after the stub (offset 4). */
static struct MidiDeviceData MidiDeviceData __attribute__((used, section(".text.modhdr"))) =
{
  MDD_Magic,
  name,
  vers,
  1, 2,
  Init,
  Expunge,
  OpenPort,
  ClosePort,
  CAMDPORTCOUNT,    /* NPorts: patched per device by the class */
  1                 /* Flags: new format */
};

/* Patched by the class to the device's id (the file name camd matched us by); the room
   behind the initializer is what the class may use - gen_camddriver.py asserts it and
   finds it by this placeholder. */
char name[32] = "camdusbmidi";
char vers[] = NEP_VER("camdusbmidi") " CAMD MIDI driver";

struct ExecBase *SysBase = NULL;
struct Library *nh = NULL;
static BOOL CAMDv40 = FALSE;
static struct CAMDAdapter *CAMDPortBases[CAMDPORTCOUNT];

/* D0 bits of a transmit result that mean "no byte". A real byte has bits 8-15 clear
   (drivers.doc). 0x100 is camd40's "nothing queued" (Transmitter() returns it); 0x8000,
   D0.w negative, follows Blom's emu10kx driver, which suspects some V37 returned -1 when
   idle - harmless, since no real byte has it set. */
#define XMIT_NOBYTE 0x8100

BOOL Init(void)
{
    SysBase = *(struct ExecBase **)4;       /* V37 passes no exec base */

    /* camd40 (and Amiga Inc.'s V40) return 0x100 for "nothing left"; V37 flags the last
       byte in D1. camd.library is already open (it is loading us), so look, don't open. */
    Forbid();
    struct Library *camd = (struct Library *)FindName(&SysBase->LibList, "camd.library");
    CAMDv40 = camd && camd->lib_Version >= 40;
    Permit();

    nh = OpenLibrary("camdusbmidi.usbclass", 0);
    return nh != NULL;
}

VOID Expunge(VOID)
{
    if(nh)
        CloseLibrary(nh);
    nh = NULL;
}

/* RX hook (struct Hook h_Entry: hook A0, bytes A2, count A1): hand len received MIDI bytes
   to camd. Forbid() excludes a ClosePort from another task, so a closed port's userdata
   is never handed to camd; interrupts stay enabled as camd's receive function requires. */
static void SendToCAMD(struct Hook *hook asm("a0"), UBYTE *msg asm("a2"), ULONG len asm("a1"))
{
    struct CAMDAdapter *ca = (struct CAMDAdapter *)hook->h_Data;

    Forbid();
    if(ca->ca_IsOpen)
    {
        while(len--)
            ca->ca_RXFunc(*msg++, ca->ca_UserData);
    }
    Permit();
}

/* TX hook (hook A0, buffer A2, max A1): pull up to max bytes of camd's queued output into
   buf, return the count. Stops at the end marker and clears ca_TXActive there, so camd's
   transmit handler is never called while camd believes the driver idle (V37 rule).
   Forbid() keeps camd's senders - tasks, per camd.intro - from queueing while we pull, the
   exclusion a transmit interrupt would have. */
static ULONG PullFromCAMD(struct Hook *hook asm("a0"), UBYTE *buf asm("a2"), ULONG max asm("a1"))
{
    struct CAMDAdapter *ca = (struct CAMDAdapter *)hook->h_Data;
    ULONG n = 0;

    if(!ca->ca_IsOpen || !ca->ca_TXActive)
        return 0;
    Forbid();
    while(n < max && ca->ca_TXActive)
    {
        unsigned long long res = ca->ca_TXFunc(ca->ca_UserData);
        ULONG d0 = (ULONG)(res >> 32);
        ULONG d1 = (ULONG)res;

        if((d0 & XMIT_NOBYTE) == 0)
            buf[n++] = (UBYTE)d0;
        if((d0 & XMIT_NOBYTE) || (!CAMDv40 && (d1 & 0xff)))     /* V37: D1 != 0 = that was the last byte */
            ca->ca_TXActive = FALSE;
    }
    Permit();
    return n;
}

/* ActivateXmit for port: wake the class task, which drains camd through PullFromCAMD. A
   port whose device is gone is drained here and the bytes dropped, so camd's queue never
   waits on a driver that no longer transmits. All under Forbid(): it pairs with the class
   clearing ca_SigTask when the device goes, and keeps ClosePort on another task from
   freeing the port meanwhile (the drain cannot Wait: camd's transmit function is
   interrupt-safe). */
static void nActivate(UWORD port)
{
    Forbid();
    struct CAMDAdapter *ca = CAMDPortBases[port];
    if(ca && ca->ca_IsOpen)
    {
        ca->ca_TXActive = TRUE;
        if(ca->ca_SigTask)
        {
            Signal(ca->ca_SigTask, ca->ca_SigMask);
        }
        else
        {
            /* The device is gone (the class cleared ca_SigTask on unplug), so no class
               task will pull this output. Take camd's queue here on the sender's task and
               drop the bytes: left there, camd would still count the driver as
               transmitting, never call ActivateXmit again (V37) and a program closing the
               port could wait forever. Ends at camd's end marker (PullFromCAMD returns 0);
               camd's senders are tasks, so the queue only shrinks while we hold Forbid(). */
            UBYTE junk[16];
            while(PullFromCAMD(&ca->ca_CAMDTXFunc, junk, sizeof(junk)) != 0)
                ;
        }
    }
    Permit();
}

/* One ActivateXmit per port, so the port needs no register: V37 passes only A0 = the
   MidiPortData (drivers.doc), AROS's camd only A2 = userdata and D0 = port, camd40 all
   three. PORTS() lists the port numbers once and generates the stubs and the table. */
#define PORTS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) \
                 X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define ACTIVATE_STUB(n)  static void ActivateXmit##n(void) { nActivate(n); }
#define ACTIVATE_ENTRY(n) { ActivateXmit##n },
PORTS(ACTIVATE_STUB)
static struct MidiPortData MidiPortData[CAMDPORTCOUNT] = { PORTS(ACTIVATE_ENTRY) };

struct MidiPortData *OpenPort(struct MidiDeviceData *data asm("a3"), LONG portnum asm("d0"),
                              CAMDXmitFunc transmitfunc asm("a0"), CAMDRecvFunc receivefunc asm("a1"),
                              APTR userdata asm("a2"))
{
    UWORD port = portnum & 0xff;    /* V37 sets only D0.b */
    struct CAMDAdapter *ca;

    (void)data;
    if(port >= CAMDPORTCOUNT || CAMDPortBases[port])
        return NULL;
    ca = (struct CAMDAdapter *)usbCAMDOpenPort((APTR)transmitfunc, (APTR)receivefunc, userdata, name, port);
    if(!ca)
        return NULL;
    ca->ca_CAMDRXFunc.h_Entry = (APTR)SendToCAMD;
    ca->ca_CAMDRXFunc.h_Data = ca;
    ca->ca_CAMDTXFunc.h_Entry = (APTR)PullFromCAMD;
    ca->ca_CAMDTXFunc.h_Data = ca;
    CAMDPortBases[port] = ca;
    ca->ca_IsOpen = TRUE;           /* last: the class delivers and pulls only on an open slot */
    return &MidiPortData[port];
}

VOID ClosePort(struct MidiDeviceData *data asm("a3"), LONG portnum asm("d0"))
{
    UWORD port = portnum & 0xff;
    struct CAMDAdapter *ca;

    (void)data;
    if(port >= CAMDPORTCOUNT)
        return;
    ca = CAMDPortBases[port];
    if(!ca)
        return;
    Forbid();
    ca->ca_IsOpen = FALSE;
    ca->ca_TXActive = FALSE;
    CAMDPortBases[port] = NULL;     /* with the flags: nActivate looks the port up under Forbid() */
    Permit();
    usbCAMDClosePort(ca);
}
