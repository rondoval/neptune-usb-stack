#ifndef MIDI_CAMDDEVICES_H
#define MIDI_CAMDDEVICES_H
/*
 * midi/camddevices.h - native AmigaOS (m68k) CAMD MIDI device-driver interface.
 *
 * A CAMD MIDI driver is a LoadSeg'able file in DEVS:Midi/. Its first hunk begins with a
 * 4-byte `moveq #-1,d0; rts` stub (so it can't be run as a program) immediately followed
 * by a `struct MidiDeviceData` whose Magic == MDD_Magic. camd.library loads the file,
 * reads the table at that fixed offset, and calls the entry points using the register
 * convention noted on each field.
 *
 * Self-contained variant of the AROS midi/camddevices.h (Kjetil Matheussen / AROS team)
 * for the bebbo NDK toolchain - no <libcore/compiler.h>: the register convention is
 * written as asm("reg") parameters, so a driver's functions fill the table without casts
 * and the compiler checks them against it.
 */

#include <exec/types.h>

/* camd's transmit and receive functions, as OpenPort hands them to the driver.
 * Transmit: A2 = userdata; returns the next byte in D0 and, under V37, D1 != 0 when it was
 * the last one - a 64-bit return, which gcc passes in D0 (high half) and D1 (low half).
 * Receive: D0 = the received byte (bit 15 = overrun), A2 = userdata. camd is not gcc code and
 * may read all of D0, so the input is passed as a full register: gcc loads a narrower
 * asm("d0") argument with a .w move and leaves the upper half undefined. */
typedef unsigned long long (*CAMDXmitFunc)(APTR userdata asm("a2"));
typedef void (*CAMDRecvFunc)(ULONG input asm("d0"), APTR userdata asm("a2"));

struct MidiPortData {
    /* Called by camd when it has output queued. The camds disagree on the registers - V37:
       A0 = this MidiPortData only; camd40: also A2 userdata, D0 port; AROS: A2, D0 only - so
       it is declared without parameters: a driver should not rely on any of them. */
    void (*ActivateXmit)(void);
};

struct MidiDeviceData {
    ULONG  Magic;                 /* == MDD_Magic ('MDEV') */
    char  *Name;
    char  *IDString;
    UWORD  Version;
    UWORD  Revision;
    BOOL  (*Init)(void);          /* called right after LoadSeg(); no parameters (camd40 also sets A6 = SysBase) */
    void  (*Expunge)(void);       /* called right before UnLoadSeg() */
    struct MidiPortData *(*OpenPort)(struct MidiDeviceData *data asm("a3"),
                                     LONG portnum asm("d0"),        /* V37 sets only D0.b */
                                     CAMDXmitFunc transmitfunc asm("a0"),
                                     CAMDRecvFunc receivefunc asm("a1"),
                                     APTR userdata asm("a2"));
    void  (*ClosePort)(struct MidiDeviceData *data asm("a3"), LONG portnum asm("d0"));
    UBYTE  NPorts;                /* number of ports (may be set in Init) */
    UBYTE  Flags;                 /* bit 0: 1 = camd40 "new format" (0x100 end marker), 0 = old */
};

#define MDD_Magic ((ULONG)'M' << 24 | (ULONG)'D' << 16 | 'E' << 8 | 'V')

#endif /* MIDI_CAMDDEVICES_H */
