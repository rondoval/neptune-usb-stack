#ifndef NEPTUNE_PATHS_H
#define NEPTUNE_PATHS_H
/*
 * On-disk locations and keys, one definition each, shared by the library, NeptuneUSB,
 * the CLI tools and the ROM startup.
 */

#define NEP_HWDRAWER      "DEVS:USBHardware"            /* host-controller drivers */
#define NEP_CLASSDRAWER   "DEVS:USBClasses"             /* class drivers */
#define NEP_CLASSSUFFIX   ".usbclass"                   /* file = library name suffix */
#define NEP_PRESETDRAWER  "SYS:Prefs/Presets/Neptune"   /* attach/detach sounds */
#define NEP_PREFS_ENV     "ENV:Sys/neptune.prefs"
#define NEP_PREFS_ENVARC  "ENVARC:Sys/neptune.prefs"

/* Owner key of the prefs editor's own per-device data (custom name, popup
   inhibit, forced binding) in the prefs file. */
#define NEP_PREFS_OWNER   "NeptuneUSB"

/* Poseidon names. When no neptune.prefs exists, poseidon.prefs is loaded
   and saved as neptune.prefs - a 6.0-6.2 one only: a classic Poseidon's file is
   that stack's own (pReadCfgFile()). Wherever prefs data enters the config (any
   load, any imported entry), the drawer prefixes, the class suffix and the owner
   key below are rewritten to the ones above (see pMigrateCfg()). */
#define NEP_OLDPREFS_ENV      "ENV:Sys/poseidon.prefs"
#define NEP_OLDPREFS_ENVARC   "ENVARC:Sys/poseidon.prefs"
#define NEP_OLDPREFSFORM      MAKE_ID('P','S','D','C')  /* 6.0-6.2's and 4.x's outer FORM */
#define NEP_OLDGLOBALCFGLEN   50    /* GCFG length 6.0-6.2 wrote: pgc_LogInfo to pgc_Reserved0 */
#define NEP_OLDCLASSDRAWER    "SYS:Classes/USB"
#define NEP_OLDPRESETDRAWER   "SYS:Prefs/Presets/Poseidon"
#define NEP_OLDCLASSSUFFIX    ".class"
#define NEP_OLDPREFS_OWNER    "Trident"
#define NEP_OLDLIBNAME        "poseidon.library"   /* see <nep_otherstack.h> */

#endif /* NEPTUNE_PATHS_H */
