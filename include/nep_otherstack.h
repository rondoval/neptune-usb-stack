#ifndef NEPTUNE_OTHERSTACK_H
#define NEPTUNE_OTHERSTACK_H
/*
 * One USB stack at a time. Neptune does not start while Poseidon is loaded: its
 * classes carry the same file names, so ramlib would hand Neptune Poseidon's
 * hid.class, and it may be driving the same host controller. Checked by
 * USBStart, USBAddHardware, USBAddClasses, the ROM startup and, for any other
 * caller, psdParseCfg(), psdAddClass() and psdAddHardware().
 */

#include <exec/execbase.h>
#include <proto/exec.h>
#include <nep_paths.h>      /* NEP_OLDLIBNAME */

/* Argument: the version nepPoseidonLoaded() returned. */
#define NEP_OTHERSTACK_TXT \
    "Poseidon (" NEP_OLDLIBNAME " V%ld) is loaded; Neptune does not start. Only one USB " \
    "stack can run at a time - reboot without Poseidon, or quit it and run Avail FLUSH."

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
