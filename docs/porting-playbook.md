# Porting playbook (AROS sources → AmigaOS 3.x)

How to bring a component, or a fix, from the AROS Poseidon sources into this tree, which builds with
the NDK 3.2 headers, the m68k-amigaos GCC and cmake. The C logic carries over almost unchanged; the
work is replacing AROS's genmodule calling-convention glue (§1) and supplying the AROS definitions
the NDK does not have (§1.5). Two scripts in `scripts/` do the mechanical part: **`conf2sfd.py`**
and **`dearos_lh.py`**. `scripts/aros-port.sh` applies an upstream commit to this tree
(CONTRIBUTING.md, "Tracking AROS upstream").

The initial port is complete; this is a recipe, not a record. Use it to port an AROS fix into a
component, add a class driver, or touch the `.sfd`/`sfdc` flow.

---

## 0. Scope - what was taken and what was dropped

From the AROS `rom/usb/` tree (baseline SHA in `AROS-BASELINE`, extraction tagged
`aros-extract-base`): `poseidon.library`
(here `neptune.library`), `usbclass.library` (the base meta-class every class inherits - a
headers-only cmake target, since nothing opens it), all class drivers, Trident (here Neptune USB)
and the CLI tools. `scripts/aros-port.sh` maps the upstream paths and file names onto this tree.

**Not taken:** the AROS host-controller drivers `pciusb`/`pcixhci`/`vusbhc` - here the host
controller is a separate `.device` (legacy interface, or the
[context interface](neptune-context-hcd-abi.md)); `felsunxi` (Allwinner FEL) and `guiapps/ps3eye`,
which have no counterpart on this hardware; and the `trident/catalogs` *submodule link* (the
translations were copied into the tree).

---

## 1. Per-component conversion recipe

### The checklist

1. **`conf2sfd.py <c>.conf --modname <m> --base _<M>Base` → `<m>.sfd`** (§1.1).
2. **`dearos_lh.py <c>.c --inplace`** (and the internal `.h` if it has `AROS_UF*`/`AROS_LH`) (§1.2).
3. Write **`<m>_main.c`** (romtag and init table; the function table comes from
   `conf2sfd --mode functable`) (§1.3).
4. Remove genmodule/`<aros/*>` includes from the component header; set the base-var convention (§1.4).
5. Handle the component-specific AROS constructs (§2): stubs, OOP/HIDD, hooks, `ADD2INIT`, debug.
6. Write the component `CMakeLists.txt` (§1.6); **build clean - 0 warnings** before declaring done.

`aros_compat.h` (§1.5) is force-included into everything and grows as new AROS definitions are
needed.

### 1.1 genmodule `.conf` → `.sfd` + `sfdc`

`conf2sfd.py` emits the `.sfd`: config header (`==base`/`==basetype`/`==libname`/`==bias 30`), the
`functionlist` body **verbatim** (same `name(args)(REGS)` syntax), the cdef vararg stubs as
`==varargs` entries, and **drops the trailing `##begin class` (HIDD) blocks**. `sfdc --addvectors
none --target m68k-amigaos --mode {clib,macros,pragmas,proto}` then generates the headers.

**Key rule:** the component includes **`<inline/<m>.h>`**, *not* `<proto/<m>.h>` - the latter also
pulls the plain `clib` prototypes which **conflict** with our register-arg LVO definitions. External
libraries it merely *calls* (e.g. neptune→usbclass) use `<proto/…>` normally.

### 1.2 `AROS_LH` / `AROS_UFH` / `AROS_UFP` → plain C (`dearos_lh.py`)

```c
AROS_LH1(APTR, psdAllocVec, AROS_LHA(ULONG,size,D0), LIBBASETYPEPTR, ps, 5, psd) {INIT …EXIT}
 →  APTR (psdAllocVec)(ULONG size asm("d0"), struct PsdBase *ps asm("a6")) { … }
```
- **The LVO function name is parenthesised** `(psdAllocVec)(…)` so the same-named *inline call macro*
  (used for the library's own internal calls) does not expand at the definition.
- `AROS_UFH`/`AROS_UFP` (hook callbacks / their prototypes) → same, but **no `a6`**.
- Also drops `AROS_LIBFUNC_INIT/EXIT` and `ADD2{INIT,OPEN,EXPUNGE}LIB`, and rewrites
  `GM_UNIQUENAME(x)`→`x`, `LIBBASETYPEPTR`→`struct <Base> *`.
- **Internal calls are left untouched** - `psdAllocVec(size)` expands the inline macro, which jsr's
  the LVO with the libbase in `a6` (§1.4).
- **Gotcha:** the script reads/writes **latin-1** - some AROS sources are ISO-8859 (a UTF-8 read
  silently skips them, leaving the file un-transformed).
- The regex handles only LH/UFH/UFP. `AROS_LD`/`AROS_LDA` (library-descriptor protos), `AROS_LC*`
  (self-calls), `AROS_INTH*`/`AROS_INTFUNC_*` (soft-int handlers) must be hand-converted (§3.3).

### 1.3 Library skeleton - `*_main.c`

Hand-written (templates: `neptune.library/neptune_main.c`, and `classes/class_main.c` for classes).
**No trampolines.**
- `struct Resident` romtag (`RTC_MATCHWORD`, `RTF_AUTOINIT`, `NT_LIBRARY`, `residentpri`).
- The function table = `LibOpen, LibClose, LibExpunge, LibNull,` then **the LVO functions in `.sfd`
  order** (generated `*_funcs.inc`), then `(APTR)-1`. Order **is** the ABI. It may live with the
  functions (`psdFuncTable[]` is at the end of `neptune.library.c`).
- `initTable = { sizeof(struct <Base>), funcTable, 0, LibInit }`.
- `LibInit/Open/Close/Expunge` wrap the component's kept `libInit/libOpen/libExpunge` hooks
  (un-`static` them so `*_main.c` can call them). `doNotExecute` is the link entry point.
  In the class skeleton (`classes/class_main.c`) the `libExpunge` hook's result counts: TRUE =
  torn down, and the skeleton unlinks the library node and frees the base; FALSE = cannot go
  now and nothing was torn down, so the class stays and is asked again later. A class hook
  never `Remove()`s its own library node.
- **`SysBase`** defined here (the converted `.c` references it `extern`).
- The end of the module for `RT_ENDSKIP` (`_endOfCode`) comes from the linker script (§1.6); no
  end-marker source file is needed.
- **Seglist lives in the libbase struct** (e.g. add `BPTR ps_SegList;`), **never a `static`** - a
  writable static breaks ROM-ability.

### 1.4 Base-variable & `__NOLIBBASE__`

Compile every component `-D__NOLIBBASE__` so no proto auto-declares a base global; the component
supplies each base itself:
- **Its own API** (internal calls): `#define <M>_BASE_NAME ps` before `<inline/<m>.h>` - `ps` is the
  libbase parameter present in every LVO function. So `psdAllocVec(size)` → LVO via `a6 = ps`.
- **System libs** it opens: `#define DOSBase ps->ps_DosBase` etc.; `SysBase` is the real global.
- **Other stack libs** it calls (usbclass): the existing `#define UsbClsBase puc->puc_ClassBase`
  satisfies `USBCLASS_BASE_NAME`.
- **Clients** (classes, tools, Neptune USB) name their own base variable with
  `<M>_BASE_NAME` - here `NEPTUNE_BASE_NAME=ps`, set by their CMake target - instead of the `.sfd`
  `==base` name `_NeptuneBase`.

### 1.5 `aros_compat.h` - AROS definitions for the NDK

`include/aros_compat.h`, **force-included** (`-include`) into every TU, supplies what the NDK lacks
(so the converted sources need no per-file type edits). Read the header for the current set; the
entries that are not self-evident are `_sfdc_vararg` = `APTR` (so vararg string-literals are `void*`
rather than a wide-char error), a self-contained inline `stricmp` (libc `strcasecmp` drags in
`malloc.o` → unresolved `SysBase` in the freestanding link), the byte-order macros (`*2BE` identity
on m68k, `*2LE`/`LE2*` = `__builtin_bswap` since USB is LE - every GUI TU must see the identity or
config IFF silently corrupts), and `AROS_SLOWSTACKFORMAT_{PRE,ARG,POST}` (the m68k stack-varargs
pointer idiom, `ARG = &x+1`). Extend it (guarded with `#ifndef`) whenever a new component trips over
another AROS definition.

**Two differences need real code changes**, not a definition:
- **`NP_UserData`** is an AROS/OS4 `CreateNewProc` tag that **OS 3.x dos.library silently ignores**,
  so `tc_UserData` stayed unset and the spawned task read a garbage base → illegal-instruction crash.
  No shim; `psdSpawnSubTask` sets `tc_UserData` by hand under `Forbid()` after `CreateNewProcTags`.
  (A `#define NP_UserData …` would have *hidden* the bug - it compiles fine, just does nothing.)
- **`%p` in `RawDoFmt`** - classic OS 3.x exec `RawDoFmt` has no `%p` (AROS's has). Every
  library formatter routes through exec `RawDoFmt` (`psdSafeRawDoFmt`, `psdCopyStrFmt`,
  `psdAddErrorMsg`), so `%p` yields garbage/blank output. **Convert `%p` → `0x%08lx`**; for the **MUI
  text-engine image escape** use **`\33O[%08lx]`** (no `0x` prefix - MUI parses the bracket as hex).
  When porting a component, `grep` its `.c` for `%p` and convert the live (non-`//`) sites.

### 1.6 Build & link (cmake)

Flags live at exactly one of three levels - toolchain (`cmake/toolchain.cmake`), tree-wide (root
`add_compile_options()`), per target - and are **never repeated per target**. Every blanket flag
carries its rationale in the file that sets it, and the freestanding link line is defined once, in
the root `CMakeLists.txt`, and inherited.

A new component adds only its `-O` level, `-ffreestanding` if it is a library or class (a **COMPILE**
option - as a link-only flag it is silently inert), `-D__NOLIBBASE__`, its include dirs (`include/`,
the component, the generated sfd dir), `nep_debug_finalize(<target>)` (§6),
`nep_module_layout(<target>)` + `nep_enable_lto(<target>)` (below), and `OUTPUT_NAME <m>` +
`SUFFIX ".library"`; reuse `cmake/GenerateSfdHeaders.cmake`.

Two things that look like flags but are not, and so do not break the three-level rule:

- **Layout.** `nep_module_layout(<target>)` links the module through `ldscripts/module.lds`, which
  places `.text.entry` (the `doNotExecute` stub, which LoadSeg runs at offset 0) then `.text.modhdr`
  (the romtag) first, defines `_endOfCode` for `RT_ENDSKIP` at the true end of `.text`, and asserts
  the module has no writable sections. Pin the stub and the romtag with
  `__attribute__((used, section(".text.entry")))` / `(".text.modhdr")` - `used` is load-bearing, and
  `ENTRY()` is not a substitute for it. Pass `WRITABLE` only for a module that is never in ROM.
- **LTO** is a CMake *property*, not a flag: `nep_enable_lto(<target>)`. Never add `-flto` by hand.
  LTO's symbol table cannot see a definition that exists only inside an asm string, so the link
  fails with an undefined reference (or silently drops it). Write such code as a `.S` file, which
  never enters LTO (`classes/massstorage/mounter/bootpoint.S`); only if it needs the C compiler
  (`offsetof()` in `"i"` operands) keep it in C and opt the TU out with
  `nep_lto_keep_real_objects(<target> <src>)`. Nothing in the tree needs that today.

Two more traps: any *second* force-include beyond `aros_compat.h` (Neptune USB's `mui_compat.h`)
must use the `"SHELL:-include …"` form, since CMake de-dups a bare repeated `-include` flag; and
the release builds the same tree for 68020/68040/68060, so never assume a CPU in code.

---

## 2. Component-specific AROS constructs

| AROS construct | Where | Replacement |
|---|---|---|
| MUI GUI | boot-class config dialogs, `notify.gui.c`, Neptune USB | real MUI via **MUI 5.0** + a file-scope MUI-base accessor (no global) + `-lamiga` - see §4. |
| `debug.h` / `KPRINTF` / `XPRINTF` / `DB` | every component | the shared **`include/debug.h`** switchable backend (§6); call sites unchanged. AROS `bug()`/`D()` map to `KPRINTF` when ported. |
| OOP / HIDD (`<oop/oop.h>`, `<hidd/*>`) | **`hid` class only**, behind `#if __AROS__` | `__AROS__` is undefined here and the blocks have no `#else`, so they drop out; the class uses its `input.device` path (compare `bootmouse.class.c`). |
| `(HOOKFUNC)func` cast on `h_Entry` | hook assignments (shellapps, Neptune USB) | gcc errors `-Wincompatible-pointer-types` via the *typedef* even though `HOOKFUNC`≡`ULONG(*)()`; cast to the literal `(ULONG (*)(void))` (or `(APTR)`) instead. |
| NDK inlines typed `RET (*)()` (`SetFunction`, `RawDoFmt`, `Interrupt.is_Code`) | patches, formatters, interrupt servers | GCC 15+ defaults to **C23**, where `()` means `(void)` - a prototyped function (ours all carry `asm("dN")` register args) no longer converts implicitly and it is a hard **error**, not a warning. Cast explicitly, spelled `(ULONG (*)(void))` / `(void (*)(void))`: correct in both C11 and C23. An `APTR`-typed variable still passes silently (GCC's `void*`↔function-pointer extension) - that is why only *some* call sites break. |
| `ADD2INIT/EXIT` linker sets | Neptune USB `locale.c` | explicit init/cleanup in `main()`. |
| runtime debug knobs (`bootloader.resource` `usbdebug`, `PSF_KLOG`) | `libInit`, error-log path | dropped - the framework is compile-time (§6). |
| unchecked `psdAddHardware()` / `psdRemHardware()` | `neptune.library` hardware list | **deliberate behavioural difference**, not a port artefact: both validate, where upstream leaves it to the caller - a duplicate device+unit is refused, a `phw` that is not on the list is ignored rather than followed, each logged at `RETURN_WARN`, matching the guard `psdAddClass()` has always had. Identity lives in **`include/hwmatch.h`**, which `pFindHardware()` compares through too, so library and clients cannot drift. Don't re-implement the walk in a new caller. |

---

## 3. Class drivers

Each class is a full genmodule library, so §1 runs **per class**, but the shared skeleton + GUI/ROM
infrastructure make most of it mechanical. No per-class `.sfd` (the 3 usbclass ABI vectors come from
`usbclass_headers` + the shared `class_main.c` skeleton). The installer drops any new `.class` into
`DEVS:USBClasses/`, so there is **no packaging work per class**.

### 3.1 Per-class recipe (mechanical core)

1. **`dearos_lh.py --inplace`** the `<name>.class.c` *and* its `.h`; **`%p` sweep** (§1.5).
2. **CMakeLists** via `add_neptune_class()` (§3.2): `BASETYPE`, plus `GUI` or `AMIGALIB`,
   `SOURCES`, `DEFINES HAS_LIBOPEN`/`HAS_LIBCLOSE` as the source needs, and `DEPS` for family
   headers. The class name, version and `neptune_headers` come with the helper.
3. **GUI classes:** the class struct header ends with the `mui_base.h` block (`MUI_BASE_USERDATA` +
   `MUI_BASE_FIELD` + `#include "mui_base.h"`) - that include also pulls the `MUI_NewObject` fix (§4).
4. **Start ROM-clean** - the `$4` exec base and `const` tables come from `class_main.c`/`common.h`;
   don't add a writable global (§3.3). In ROM a write to one is lost silently, so
   `ldscripts/module.lds` fails the link on a non-empty `.data` or `.bss`.
5. **New `aros_compat.h` definitions / non-NDK headers** carried into `include/` as they are needed.
6. **Build 0/0**; deploy adds it to the install automatically.

### 3.2 `add_neptune_class()` CMake helper

Defined and documented at the top of the root `CMakeLists.txt` (next to `generate_sfd_headers`); it
collapses a class's ~40-line CMakeLists to ~3 lines. **Read the signature there, not here** -
`GUI` adds `mui_headers` + `-lamiga`, `AMIGALIB` adds `-lamiga` alone, `DEPS` carries family header
targets, `INCLUDE` overrides the default `<name>.h` struct header. The class version is global, so
there is nothing to state per class.

Header availability for a new class: the **AHI** sub-driver headers (`libraries/ahi_sub.h`,
`defines/`, `inline/`, `proto/`, `devices/ahi.h`), serial, `input.device` and `usbparallel` are
already in the toolchain / NDK. **SANA-II** is not in the toolchain's default include - it comes from
the `sana2_headers` INTERFACE target (`SANA2_INCLUDE_DIR`, defaulted under `AMIGA_SDK_ROOT` so no
literal home path lands in the build), as does the MUI 5 SDK via `mui_headers` (§4). **CAMD** is
absent; camdmidi vendors only `include/midi/camddevices.h`.

### 3.3 Class skeleton & embedded-device recipe

The shared `class_main.c` + `common.h` bake in the **`$4` exec base** and `const` tables. Romtag name
need not equal dir name - pass the *romtag* name to `add_neptune_class` so `<name>.class.c`/`.h`
resolve; the dir is only the `add_subdirectory` path.

**Embedded-device (dual-library) recipe** (massstorage / serial / eth / audio / arosx): the class
`MakeLibrary`'s a second library/device in `libInit`.
- `dearos_lh.py` with **two** `--libbasetype`s: the class base for `<name>.class.c`/`.h`, the device base for
  `dev.c` (its `a6` is the *device* base, not the class base). When both bases appear in one file,
  disambiguate by basevar and re-type only the funcs whose `a6` carries the second base. **Watch for
  a second basetype macro that is a *substring* of `LIBBASETYPEPTR`** (audio's `SUBLIBBASETYPEPTR`):
  resolve it and drop its `#define` *before* the transform, or a blind replace corrupts it.
- `dev.h` uses `AROS_LD`/`AROS_LDA` (library-descriptor protos) which `dearos_lh.py` does **not**
  handle → hand-convert to plain `asm()` protos (template `massstorage/dev.h`). The one `AROS_LC1`
  self-call → a direct call; `&AROS_SLIB_ENTRY(devX, dev, n)` in `DevFuncTable[]` → `(APTR) devX`.
  `AROS_INTH1` (soft-int handlers) → `ULONG name(type var asm("a1"))` - `is_Data` arrives in A1.
- un-`static` `libInit`/`libExpunge` (skeleton calls them via `extern`); add `BPTR nh_SegList;` to the
  class base; replace any global `SysBase` value-use → `EXEC_BASE_NAME`.
- **`bug()`** (AROS raw debug output) → `KPRINTF(10, (…))`; fix `%d`→`%ld` (classic `RawDoFmt` `%d` reads 16
  bits; args are 32-bit).
- A zero-init `const` sentinel lands in `.bss`; an explicit `= {0}` initializer keeps it in `.rodata`
  so `.bss`=0.

**Class extra vectors** (generic, gated): `class_main.c` supports `-DCLASS_VECTORS_HDR="…"` to pull a
class header that declares externs and `#define CLASS_EXTRA_VECTORS` (the funcTable tail: reserved
`LibNull`s + the extra vectors). Absent for every other class ⇒ their binaries stay byte-identical.
Used by camdmidi, whose `CMakeLists.txt` documents the whole embedded-CAMD-driver pipeline.

**Definition of done (per class):** builds 0/0; ROM-clean (`.bss`=0, no new named writable globals -
`nm` check); `%p`-free debug strings; GUI (if any) opens via Neptune USB's Classes panel; deployed
`.class` binds its device on plug.

---

## 4. MUI GUIs (build against MUI 5, run on MUI 3.8+)

Neptune USB (Trident upstream) and the class config GUIs were written for Zune, AROS's MUI
implementation (~MUI 3.x API). They
**compile** against the MUI 5 SDK (§4.1 - a toolchain choice only) and **run** on
`muimaster.library` **19+**, MUI 3.8/4.0/5 alike (§4.2 - a runtime floor). The two are
independent.

**Suspect the OS layer before the toolkit.** Of four showstoppers that looked like Zune↔MUI 5
incompatibilities, none was: `Scrollgroup`/`IconList` failure and "window won't open" were the
`MUI_NewObject` bug (§4.1), blank list rows were `RawDoFmt` `%p`, the event-broadcast crash was
`NP_UserData` (both §1.5).

### 4.1 SDK, the `MUI_NewObject` fix, and the base accessor

**Use the MUI 5.0 SDK** (`-DMUI_INCLUDE_DIR=…/MUI5/SDK/MUI/C/include`, via the `mui_headers`
INTERFACE target): it honours `__NOLIBBASE__` and its `inline/muimaster.h` parses under gcc (LPn
A-variants + real `__inline` vararg constructors, so `End`=`TAG_DONE)` works). Not the MUI 3.8 SDK
(gcc-2.x `a6@` asm), and don't regenerate it with `fd2sfd`+`sfdc` - sfdc emits the constructors as
function-like macros, which kills the `End` idiom.

**The SDK's `__inline MUI_NewObject` is broken and always will be**, so never use it: it passes
`&tags` - the address of the first *named* vararg - as the tag array, leaving the `...` args dead for
the inliner to drop. On **gcc 16.1** a 5-attribute `WindowObject` (11 tag words + terminator) becomes

```
subq.l #4,sp                 ; ONE longword reserved
move.l #-2143113923,4(sp)    ; only MUIA_Window_Title stored
lea (4,sp),a1                ; a1 = &tags, passed to MUI_NewObjectA
```

- no values, no `TAG_DONE`; MUI walks off the end. Surviving tag words by tier: `-O0` 9, then **1**
at `-O1`/`-O2`/`-O3`/`-Os` (10 with the fix), so every tier we ship is affected and `-O0` passing is
why it looks like an optimizer bug. `MUI_MakeObject` escapes it - `Label(x)` and friends are always
fully parenthesised. The replacement, and why include order picks the MUI base, is documented in the
force-included `include/mui_compat.h`.

**The MUI base is a file-scope accessor, never a writable global** (ROM-safe); `classes/mui_base.h`
carries it and the reasoning. A GUI class ends its struct header with:

```c
#define MUI_BASE_USERDATA struct NepClassHid   /* the struct in the GUI task's tc_UserData */
#define MUI_BASE_FIELD    nch_MUIBase          /* its *_MUIBase field */
#include "mui_base.h"                          /* accessor + base name + proto + the NewObject fix */
```

and links **`-lamiga`** (BOOPSI `DoMethod`/`DoMethodA` are amiga.lib stubs). Verify per class which
of the instance or the libbase its GUI subtask carries in `tc_UserData` - the existing classes show
both, each stated in its own `CMakeLists.txt` header. Dispatchers are either the raw `asm()` form
Neptune USB and the notifier use (`cl` in `a0`, `obj` in `a2`, `msg` in `a1`) or the SDK's SDI
`DISPATCHER()`, equivalent on m68k; per-class data goes in `cl->cl_UserData`, not the hook's
`h_Data`. `AROS_UFH3` dispatchers are converted to one of these forms.

### 4.2 The MUI 3.8 floor

The fleet runs on `muimaster.library` **19+**, and that was nearly free: the code, written for Zune,
uses no MUI 4/5 features. `include/mui_compat.h` shadows the SDK's `MUIMASTER_VMIN` (20, the MUI 4
baseline) with 19, so every `OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN)` picks the floor up
untouched and a class copied from an existing one cannot quietly regress it. The ceiling is **V14**
(`MUIM_Application_AboutMUI`, `NeptuneUSB.c`); keep it there.

`scripts/check-mui38.py` runs at the end of every container build and hard-fails on a symbol 3.8
lacks, anything V≥20, or an `OpenLibrary` bypassing the shadow; there is no escape hatch by design.
Its docstring is the full account, including how to regenerate the `scripts/mui38-symbols.tsv`
oracle. The one thing it merely *warns* about is access-flag narrowing: the `isg` flags narrow for 29
attributes between MUI 5 and 3.8, and exactly one is used in the lost direction
(`MUIA_Cycle_Entries`, `ActionClass.c`), where a comment rather than a workaround marks it.

---

## 5. The OS 3.1 floor

Same shape as §4.2, one layer down: we **compile** against the NDK 3.2 headers but **run** on
Kickstart/Workbench **3.1 (V40)** and up. The asymmetry is the whole hazard - a V47 call compiles
without a murmur and fails only on the user's machine, and it fails *silently* when it is a device
command rather than a library open.

**Re-run these two sweeps when porting an AROS fix or adding a class driver**; both must stay empty.
There is deliberately no `check-os31.py` - they are cheap by hand, and that is when to run them.

1. **Functions.** The NDK 3.2 `SFD/*.sfd` files carry `==version N` markers giving the library
   version each function was introduced in. Take every name under a marker above 40 - `IconControl`,
   `GetIconTags`, `WorkbenchControl`, `OpenWorkbenchObject`, `NewMinList`, `SNPrintf`, `Strncpy`,
   `IntuitionControl`, `ShowWindow`/`HideWindow`, `LayerOccluded`, `ScaleGadgetRect`, the V47
   outline-font calls - and grep for it. **Zero hits.**
2. **Constants and tags.** Every `#define` in `NDK3.2R4/Include_H` whose line carries a `V41`…`V59`
   annotation - 42 of them - grepped the same way. **One hit**: `IND_ADDEVENT`.

Plus the direct check: no `OpenLibrary("<os library>", N)` literal above 40. There is no carve-out
below it either - the vendored `mounter/` submodule is V40-only like the rest of the tree; its
`dos.library` open is still allowed to *fail*, but that is a pre-DOS detector, not a version
fallback.

Three exceptions stand, each runtime-gated rather than avoided, and each documented where it lives:
`IND_ADDEVENT` (`input.device` V47) in `hid.class`, `bootkeyboard.class` and `bootmouse.class`,
gated on the device's own `lib_Version >= 47` and its `NSCMD_DEVICEQUERY` answer listing the
command (`nInputHasAddEvent()`, `classes/common.h`), with an `IND_WRITEEVENT` fallback ([hid.class-architecture.md](hid.class-architecture.md) §5.6, §6 rule 2);
`WBAPPMENUA_GetTitleKey`/`WBAPPMENUA_UseKey` (`workbench.library` V45) in USBEject, *probed* rather
than version-tested, so an older library simply ignores the tag (`usbeject/USBEject.c`); and
`SetJoyPortAttrsA`, which is a *revision* - `lowlevel.library` **V40.27** - that `OpenLibrary()`
cannot ask for, so `nInstallLLPatch()` measures `lib_NegSize` before `SetFunction()`ing LVO −132
(`hid.class-architecture.md` §5.8, §6 rule 9). Prefer that last shape - ask the object what it has - wherever a revision, not a
version, is what actually differs. Anything newer than V40 needs a gate written on purpose, and a
line here saying which and where.

---

## 6. Debug backend

`include/debug.h` (the shared header-only formatter) and `cmake/NeptuneDebug.cmake` (the `-DNEPTUNE_DEBUG_BACKEND=pistorm|serial|off` and
`-DNEPTUNE_DEBUG_LEVEL=<n>` knobs) document themselves. What matters when porting:

- Every `KPRINTF(level,(fmt,…))` / `XPRINTF` / `DB` call site is **unchanged** by the port; AROS
  `bug()`/`D()` become `KPRINTF` (§3.3). Output goes through classic exec `RawDoFmt`, hence the
  `%p`/`%ld` rules of §1.5.
- **A new target must call `nep_debug_finalize(<target>)`** - it links `-ldebug` plus a weak
  `__divsi3` glue, and only for the `serial` backend.
- The framework is purely compile-time; there is no runtime verbosity knob. The error log itself
  (`USBErrorlog`) is untouched by any of it.
