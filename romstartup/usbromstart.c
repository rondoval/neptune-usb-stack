/*
 * usbromstart.c - the Neptune Kickstart-ROM startup resident.
 *
 * A plain NT_TASK RTF_COLDSTART resident with no library of its own: exec calls
 * rt_Init once during the coldstart chain and that is the module's whole life.  It
 * exists so a Kickstart image can bring the USB stack up before strap picks a boot
 * volume; scripts/build-kickstart.py builds that image.  It brings up the whole stack:
 * hub/hubss/massstorage plus the input classes, a device unit, one class scan, and
 * then the boot gate below.
 *
 * Ported from the AROS originals (rom/usb/usbromearlystartup.c and
 * usbromlatestartup.c).
 *
 * The whole ROM set has to live in the -41..-49 window, because the Emu68 module
 * window - devicetree.resource, gic400.library, mailbox.resource, 68040.library
 * - is not initialised by "diag init" (105, which only relocates diag areas) but by
 * `romboot` at -40, which binds the Emu68 board's diag romtag.
 * Below us: the boot menu (-50), which lists the boot volumes this resident waits for,
 * and strap (-60). Within the window we are after the classes we ask for (-45)
 * and after the HCD (-42).
 */

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/resident.h>
#include <exec/execbase.h>

#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)

#include <proto/exec.h>

#include <dos/dos.h>                    /* RETURN_OK, for the psdAddErrorMsg log */
#include <utility/tagitem.h>
#include <libraries/expansionbase.h>
#include <libraries/usbclass.h>
#include <devices/timer.h>

#include <string.h>

#include <libraries/neptune.h>
#include <proto/neptune.h>

/* Inline LVO macros only, for both of these: the base is a local (TimerBase,
   UsbClsBase - the names the inlines default to), because a ROM module may not
   carry a writable global to hold one. */
#include <proto/timer.h>
#include <inline/usbclass.h>

#include <neptune_version.h>
#include "debug.h"

/* This is only the compile-time default (cmake -DROMSTART_HCD=<name>); the slot
   below is what actually gets asked for, and it is patchable per image. */
#ifndef ROMSTART_HCD_NAME
#define ROMSTART_HCD_NAME  "xhci.device"
#endif

/* Patchable host-controller slot.
 *
 * The ROM image is assembled from whatever modules the builder hands to
 * scripts/build-kickstart.py, so pointing the resident at a different host
 * controller must not need a recompile of the whole distribution.  The name
 * therefore lives in a fixed-size field carrying its own cookie:
 * `build-kickstart.py --hcd <name>` finds ROMSTART_HCD_COOKIE in the built module,
 * checks it occurs exactly once, and overwrites the 32 bytes that follow.  A byte
 * signature rather than a symbol because the module is linked -s.
 */
#define ROMSTART_HCD_COOKIE "NEPHCD\1"   /* 8 bytes with the NUL; \1 = layout version */

static const struct {
    char cookie[8];
    char name[32];
} romstartHcd __attribute__((used)) = { ROMSTART_HCD_COOKIE, ROMSTART_HCD_NAME };

_Static_assert(sizeof(ROMSTART_HCD_NAME) <= sizeof(romstartHcd.name),
               "ROMSTART_HCD name does not fit the patchable slot");

/* Linker entry point (-Wl,-e,_doNotExecute): a resident module is data to the
   loader, never a program. */
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void) { return -1; }

/* End-of-module marker for RT_ENDSKIP; defined by classes/class_end.c, which this
   target links last. */
extern const UBYTE endOfCode;

/* Host controller probing: how many unit numbers to try before a failed unit is
   taken as the end of the list.  The units of one HCD are not one contiguous run of
   like controllers - xhci.device's unit 0 is the SoC's own controller (devicetree
   /scb/xhci) while units 1..n are the PCIe xHCI cards in bus order - so a machine
   without the former fails unit 0 and still has everything on unit 1.  Past this
   floor the first failure ends the scan. */
#define HCD_PROBE_UNITS     2

/* Boot gate timings, in milliseconds.
   Enumeration runs asynchronously on the hub tasks, so this
   resident has to wait for it here.  See waitForBootDevices() for how they combine. */
#define GATE_TICK_MS      100   /* poll granularity */
#define GATE_FLOOR_MS     500   /* never believe "bus is empty" before this */
#define GATE_QUIET_MS     750   /* mount list unchanged this long => mounts done */
#define GATE_MOUNT_MS    2000   /* minimum wait after mass storage binds */
#define GATE_MAX_MS      8000   /* hard cap: a wedged device must not hang the boot */
#define GATE_MEDIA_MS   20000   /* the cap once a medium has been seen coming up */

static ULONG initUsbRom(ULONG            dummy   asm("d0"),
                        BPTR             seglist asm("a0"),
                        struct ExecBase *sysbase asm("a6"));

/* `residentId` feeds the shared $VER cookie (include/neptune_version.h), so this
   module carries the same version identity as the rest of the distribution. */
static const char residentName[] = "Neptune ROM Init";
static const char residentId[]   = NEP_VER("Neptune ROM Startup");

const struct Resident romTag __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&romTag,
    (APTR)&endOfCode,
    RTF_COLDSTART,
    NEPTUNE_VERSION,
    NT_TASK,
    -46,
    (char *)residentName,
    (char *)residentId,
    (APTR)initUsbRom
};

/* Keyboard and mouse.
 *
 * hid.class is tried first and is expected to fail in the shipping ROM.
 * The two boot-protocol classes cover keyboard and mouse until USBStart
 * runs from the startup-sequence. Its class scan is the first one from a
 * process, and that is what runs the AfterDOS pass: it releases the boot
 * classes' bindings and hands the devices to the disk-loaded hid.class.
 *
 * Called before the bus is enumerated, so the hub tasks bind these in the same pass
 * that brings storage up: the keyboard is live *during* the boot gate below.
 */
static void addInputClasses(struct Library *ps, CONST_STRPTR origin)
{
    if(psdAddClass("hid.class", 0))
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR)origin, "Added hid.class from ROM.");
    } else {
        psdAddClass("bootmouse.class", 0);
        psdAddClass("bootkeyboard.class", 0);
        psdAddErrorMsg(RETURN_OK, (STRPTR)origin,
                       "Added boot mouse/keyboard classes from ROM.");
    }
}

/* Every unit of the host controller, enumerated.  Returns the number found; 0 means
   there is no USB at all and the caller has nothing to wait for. */
static ULONG addHardware(struct Library *ps, CONST_STRPTR origin)
{
    ULONG units = 0;

    for(ULONG unit = 0; ; unit++)
    {
        APTR phw = psdAddHardware((STRPTR)romstartHcd.name, unit);

        if(!phw)
        {
            if((unit + 1) >= HCD_PROBE_UNITS)
            {
                break;
            }
            continue;
        }

        KPRINTF(10, ("usbromstart: added %s unit %ld\n", (STRPTR)romstartHcd.name, unit));
        psdEnumerateHardware(phw);
        units++;
    }

    if(units)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR)origin,
                       "Started %ld %s unit(s) from ROM.", units, (STRPTR)romstartHcd.name);
    } else {
        psdAddErrorMsg(RETURN_WARN, (STRPTR)origin,
                       "No %s unit found; USB is not available at boot.",
                       (STRPTR)romstartHcd.name);
    }

    return units;
}

/* TRUE once every device on the bus has finished enumerating - each one is
   either configured, dead, or gone. */
static BOOL busSettled(struct Library *ps)
{
    APTR pd = NULL;
    BOOL settled = TRUE;

    psdLockReadPBase();
    while((pd = psdGetNextDevice(pd)))
    {
        IPTR connected = 0, configured = 0, dead = 0;

        psdGetAttrs(PGA_DEVICE, pd,
                    DA_IsConnected,  &connected,
                    DA_IsConfigured, &configured,
                    DA_IsDead,       &dead,
                    TAG_END);

        if(connected && !configured && !dead)
        {
            settled = FALSE;
            break;
        }
    }
    psdUnlockPBase();

    return settled;
}

/* Elapsed milliseconds, from the E-clock.
 *
 * Only ev_lo is used.  The difference of two unsigned reads is correct across the
 * one wrap that can happen, and at ~709 kHz that wrap is 6000 seconds apart.
 */
static ULONG elapsedMS(struct Device *TimerBase, ULONG start, ULONG ticksPerMS)
{
    struct EClockVal now;

    ReadEClock(&now);
    return (now.ev_lo - start) / ticksPerMS;
}

/* One class-scoped count, straight from the class that owns the state. 0 for a
   class that is absent or does not implement the method, which is the right
   answer: nothing to wait for. */
static ULONG classCount(struct Library *UsbClsBase, ULONG method)
{
    if(!UsbClsBase)
    {
        return 0;
    }
    return (ULONG) usbDoMethodA(method, NULL);
}

/* The class base is what carries usbDoMethodA(); psdAddClass() gave us the
   Neptune-side object. */
static struct Library *classBase(struct Library *ps, APTR puc)
{
    struct Library *base = NULL;

    if(puc)
    {
        psdGetAttrs(PGA_USBCLASS, puc, UCA_ClassBase, &base, TAG_END);
    }
    return base;
}

/* Length of expansion's mount list - the BootNodes the mounter enqueues for us
   pre-DOS.  Forbid() because the mounter may be adding to it right now. */
static LONG countMountNodes(struct ExpansionBase *eb)
{
    LONG count = 0;

    Forbid();
    for(struct Node *n = eb->MountList.lh_Head; n->ln_Succ; n = n->ln_Succ)
    {
        count++;
    }
    Permit();

    return count;
}

/* Hold the coldstart chain until the USB boot devices are on the mount list.
 *
 * Four conditions have to hold together before we let strap run, and each one
 * is there for a case the others miss:
 *   - a floor, so "no devices" is never concluded before the root hub has even
 *     scanned its ports;
 *   - the mount list unchanged for a while, so a drive that produces several
 *     partitions is not cut off halfway;
 *   - a minimum wait after mass storage binds, because between binding and the
 *     first AddBootNode the mount list is legitimately still empty and would
 *     otherwise read as "quiet";
 *   - the bus settled, so a device still enumerating keeps us here;
 *   - every hub done with its port pass, because a hub in its power-good wait,
 *     or between seeing a connection and enumerating it, has nothing in the
 *     device list yet and would otherwise read as settled - behind a chain of
 *     hubs the keyboard, mouse and disk are all still to come;
 *   - no unit reporting a medium on its way up, so an optical drive gets the
 *     seconds it needs to spin up and be mounted, and only then.
 * The caps bound the whole thing: a wedged device costs a slow boot, never a
 * hung one. GATE_MAX_MS ordinarily; GATE_MEDIA_MS while media are pending,
 * which is the only case that can legitimately take that long.
 */
static void waitForBootDevices(struct Library *ps, APTR msdclass, APTR hubclass, APTR hubssclass,
                               CONST_STRPTR origin)
{
    struct ExpansionBase *eb = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);

    if(!eb)
    {
        /* Cannot observe mounts; fall back to the AROS behaviour of a flat wait. */
        KPRINTF(20, ("usbromstart: no expansion.library, flat boot delay\n"));
        psdDelayMS(GATE_MOUNT_MS);
        return;
    }

    struct timerequest tr;
    memset(&tr, 0, sizeof(tr));
    tr.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    if(OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)&tr, 0))
    {
        KPRINTF(20, ("usbromstart: no timer.device, flat boot delay\n"));
        psdDelayMS(GATE_MOUNT_MS);
        CloseLibrary((struct Library *)eb);
        return;
    }

    struct Device    *TimerBase = tr.tr_node.io_Device;
    struct EClockVal  startEV;
    ULONG             ticksPerMS = ReadEClock(&startEV) / 1000;

    if(!ticksPerMS)
    {
        ticksPerMS = 1;                  /* absurd clock; keep the arithmetic sane */
    }

    /* Whom to ask about work still in flight: media coming up, hubs still scanning their ports. */
    struct Library *msdBase   = classBase(ps, msdclass);
    struct Library *hubBase   = classBase(ps, hubclass);
    struct Library *hubssBase = classBase(ps, hubssclass);

    /* Gate state: carried across iterations, and read again by the report below. */
    ULONG elapsed      = 0;
    ULONG lastChangeAt = 0;   /* when the mount list last moved */
    ULONG boundAt      = 0;
    ULONG lastPending  = 0;
    ULONG lastSettling = 0;
    LONG  lastCount    = -1;
    BOOL  sawStorage   = FALSE;
    BOOL  sawPending   = FALSE;

    for(;;)
    {
        psdDelayMS(GATE_TICK_MS);
        elapsed = elapsedMS(TimerBase, startEV.ev_lo, ticksPerMS);

        ULONG pending  = classCount(msdBase, UCM_MediaPending);
        ULONG settling = classCount(hubBase, UCM_PortsPending) +
                         classCount(hubssBase, UCM_PortsPending);

        if(pending)
        {
            sawPending = TRUE;
        }

        if(elapsed >= (sawPending ? GATE_MEDIA_MS : GATE_MAX_MS))
        {
            break;
        }

        if(pending != lastPending)
        {
            KPRINTF(10, ("usbromstart: %ld medium/media coming up at %ld ms\n",
                         pending, elapsed));
            lastPending = pending;
        }
        if(settling != lastSettling)
        {
            KPRINTF(10, ("usbromstart: %ld hub(s) still scanning ports at %ld ms\n",
                         settling, elapsed));
            lastSettling = settling;
        }

        if(!sawStorage && msdclass)
        {
            IPTR usecount = 0;

            psdGetAttrs(PGA_USBCLASS, msdclass, UCA_UseCount, &usecount, TAG_END);
            if(usecount)
            {
                sawStorage = TRUE;
                boundAt    = elapsed;
                psdAddErrorMsg(RETURN_OK, (STRPTR)origin,
                               "Mass storage bound after %ld ms; waiting for mounts.",
                               elapsed);
            }
        }

        LONG count = countMountNodes(eb);

        if(count != lastCount)
        {
            lastCount = count;
            lastChangeAt = elapsed;
        }

        if(elapsed < GATE_FLOOR_MS)                           continue;
        if((elapsed - lastChangeAt) < GATE_QUIET_MS)          continue;
        if(sawStorage && (elapsed - boundAt) < GATE_MOUNT_MS) continue;
        if(pending)                                           continue;
        if(settling)                                          continue;
        if(!busSettled(ps))                                   continue;

        break;
    }

    CloseDevice((struct IORequest *)&tr);

    if(sawStorage)
    {
        psdAddErrorMsg(RETURN_OK, (STRPTR)origin,
                       "Boot gate released after %ld ms, %ld mount list entr%s.",
                       elapsed, lastCount, (lastCount == 1) ? "y" : "ies");
    } else {
        psdAddErrorMsg(RETURN_OK, (STRPTR)origin,
                       "Boot gate skipped after %ld ms, no USB mass storage found.",
                       elapsed);
    }
    KPRINTF(10, ("usbromstart: gate released, %ld ms, %ld nodes\n",
                 elapsed, lastCount));

    CloseLibrary((struct Library *)eb);
}

static ULONG initUsbRom(ULONG            dummy   asm("d0"),
                        BPTR             seglist asm("a0"),
                        struct ExecBase *sysbase asm("a6"))
{
    CONST_STRPTR origin = (CONST_STRPTR)residentName;

    (void)dummy; (void)seglist; (void)sysbase;

    KPRINTF(10, ("usbromstart: opening neptune.library\n"));

    struct Library *ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION);
    if(!ps)
    {
        KPRINTF(20, ("usbromstart: neptune.library not available\n"));
        return 0;
    }

    APTR hubclass   = psdAddClass("hub.class", 0);
    APTR hubssclass = psdAddClass("hubss.class", 0);
    APTR msdclass   = psdAddClass("massstorage.class", 0);
    addInputClasses(ps, origin);

    ULONG units = addHardware(ps, origin);

    /* Unconditional, including on the no-HCD path: besides binding the root device's
       classes, this is what sets ps_StartedAsTask, which is how the first class
       scan from a process later knows to run the AfterDOS pass that hands keyboard
       and mouse to hid.class. */
    psdClassScan();

    if(units)
    {
        waitForBootDevices(ps, msdclass, hubclass, hubssclass, origin);
    }

    /* No CloseLibrary(ps): the ROM-resident stack keeps this reference for good.
       psdClassScan() has started the event handler and notification tasks, and with
       no device bound nothing else holds the library open; at lib_OpenCnt 0 a memory
       flush (Avail FLUSH) runs LibExpunge, which Wait()s for those tasks inside
       Expunge (breaking the Forbid() exec holds there) and then frees the base. */
    return 0;
}
