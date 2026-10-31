# CLAUDE.md

Guidance for coding agents working in this repository. `AGENTS.md` is a symlink to this file.

## What this repo is

**Neptune** - an incompatible fork of the Poseidon USB stack (released as "Poseidon for AmigaOS"
up to 6.2), started from the AROS 5.x line, for AmigaOS 3.x on m68k (GCC 16.2, NDK 3.2, cmake).
The version is `project(VERSION)` in the top-level `CMakeLists.txt` and nowhere else.

It is a **matched pair** with `xhci.device` in the `emu68-driver-stack` checkout
(`components/emu68-xhci-driver-context/`): cross-ABI changes land in both, and the vendored
`usbhcd_*.h` copies there stay byte-identical to `include/devices/`. Don't edit the frozen
`emu68-xhci-driver-legacy/` for stack work.

AROS baseline: `AROS-BASELINE`, extraction tagged `aros-extract-base`.

## Build

```sh
./build.sh --build                # container build (image auto-pulled)
./build.sh --package --all-cpus   # Neptune-<ver>-<cpu>.lha for 68020/68040/68060
./build.sh                        # --build --upload (to a live Amiga)
```

`BACKEND=pistorm|serial|off`, `DEBUG=<level>`, `CPU=68020|68040|68060` (non-default CPUs build in
`build-020/`, `build-060/`). `scripts/docker-build.sh` owns the image tag; CI uses it too.

After editing C, build and confirm **zero errors and zero warnings**. There is no test suite;
correctness is verified on the real Amiga.

Build traps:
- Common flags come from `cmake/toolchain.cmake` and the root `CMakeLists.txt`; per-target files
  set only the optimisation tier. Don't re-state the common ones.
- `-ffreestanding` must stay a **compile** option; link-only it is silently inert.
- LTO only via `nep_enable_lto(<t>)`, **never `-flto` by hand**. A TU whose payload is file-scope
  `asm()` must opt out with `nep_lto_keep_real_objects(<t> <src>)`.
- `nep_module_layout(<t>)` links through `ldscripts/module.lds`, which asserts the module has no
  writable sections (ROM-able).
- `scripts/check-mui38.py` fails the build if a GUI uses anything outside MUI 3.8.

## Layout

| Path | Contents |
|---|---|
| `neptune.library/` | The stack core |
| `classes/` | The `*.class` drivers (shared skeleton `class_main.c`, `common.h`) |
| `neptuneusb/` | Neptune USB, the MUI prefs editor |
| `c/`, `tools/`, `usbeject/`, `vhi/` | CLI commands, gadget tools, USBEject, `usbvideo.vhi` |
| `romstartup/` | Kickstart-ROM startup resident; `scripts/build-kickstart.py` builds the image |
| `include/` | Public headers; on-disk names and the Poseidon-prefs migration names in `nep_paths.h` |
| `dist/`, `presets/` | Installer, icons, ReadMe template, sounds |
| `docs/` | Architecture and ABI docs - start at `docs/neptune.library-architecture.md` |

Open work lives in `docs/implementation-plan.md` only.

## Hard constraints

- **Runtime floor is Kickstart/Workbench 3.1 (V40)** although the build uses NDK 3.2 headers: no
  OS call or `OpenLibrary` version above 40 without a deliberate runtime gate (the one existing
  case: `IND_ADDEVENT`, V47, gated by `nInputHasAddEvent()` in `classes/common.h` on the device's
  version and its `NSCMD_DEVICEQUERY` answer). See `docs/porting-playbook.md` §5.
- **The legacy HCD ABI is frozen.** `IOUsbHWReq` V1+V2 layout and all `UHCMD_*`/`UHIOERR_*`/
  `UHFB_*`/`UHCF_*` values are binary contract with classic HCDs. Neptune does not implement
  Poseidon's V3 interface; new HCD capabilities go into the context ABI, never into the request.
- **Settings file format is fixed:** the IFF IDs (`PSDC`, `STKC`, …) stay, and
  `struct PsdGlobalCfg` is the raw `GCFG` chunk - append-only, retired fields keep their slot.
- **Catalogs:** a `MSG_*` number is both the catalog id and the `CatCompArray` index; adding,
  removing or reordering messages needs a `CATALOG_VERSION` bump. `.ct` files are ISO-8859 or
  cp1251 - use `grep -a`.
- The class scan must not wire-switch alternates while probing, and endpoint-recipient class
  requests carry the full endpoint address in `wIndex`.
- The address-0 enumeration lock is `hub.class`-local (`nh_Adr0Sema`, held across all of
  `nConfigurePort`). Don't reintroduce a stack-wide one or narrow it.

## Source conventions

- **LVO functions:** plain C with register annotations and a **parenthesised name** at the
  definition (`APTR (psdAllocVec)(…)`). Self-calls go through the sfdc inlines with an in-scope
  `ps` (`NEPTUNE_BASE_NAME=ps`).
- **New public functions:** append to `neptune.sfd` *and* `neptune_funcs.inc`, same order.
- **RawDoFmt:** no `%p` (use `0x%08lx`); always `l`-size (`%ld`/`%lu`/`%lx`).
- **ROM-clean:** `__NOLIBBASE__`, `SysBase` from `$4`, no writable data (string tables `const`,
  pointer tables as locals).
- **MUI:** `include/mui_compat.h` is force-included; don't use the SDK's inline `MUI_NewObject`.
- Declare variables at first use; comments technical and terse.

## Docs

Read `docs/porting-playbook.md` before porting an AROS fix or adding a class. If a change
invalidates documented behaviour (locking, enumeration, scan, ABI), update that doc in the same
change; function names are the stable anchors, not line numbers.
