#ifndef NEPTUNE_OTHERSTACK_H
#define NEPTUNE_OTHERSTACK_H
/*
 * One USB stack at a time. Neptune does not start while Poseidon is loaded: two
 * stacks must never drive the same host controller (both would open the same
 * host-controller driver, e.g. xhci.device, for it). Checked by USBStart, USBAddHardware, the ROM
 * startup and, for any other caller, psdParseCfg() and psdAddHardware(). Class
 * drivers are not refused: their file names differ from Poseidon's (.usbclass).
 */

#include <exec/execbase.h>
#include <proto/exec.h>
#include <nep_paths.h>      /* NEP_OLDLIBNAME */

/* Argument: the version nepPoseidonLoaded() returned. */
#define NEP_OTHERSTACK_TXT \
    "Poseidon (" NEP_OLDLIBNAME " V%ld) is loaded; Neptune does not start. Only one USB " \
    "stack can run at a time - reboot without Poseidon."

/* Version of a loaded Poseidon (poseidon.library in exec's library list), 0 if none.
   Any version counts: classic 4.x and the 6.0-6.2 fork alike. */
static inline UWORD nepPoseidonLoaded(void)
{
    Forbid();
    struct Library *lib = (struct Library *) FindName(&EXEC_BASE_NAME->LibList, NEP_OLDLIBNAME);
    UWORD version = lib ? lib->lib_Version : 0;
    Permit();
    return version;
}

#endif /* NEPTUNE_OTHERSTACK_H */
