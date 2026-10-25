# Neptune - an incompatible fork of the Poseidon USB stack

A USB stack for **AmigaOS 3.1 and newer**. Plug in a keyboard, a mouse, a memory stick, a
USB sound card or a network adapter and use it from Workbench - hot-plug, unplug, no reboot.

Neptune is derived from **Poseidon** as maintained in the AROS tree: **Chris Hodges** wrote
Poseidon for AmigaOS (2002-2009) and placed it into AROS in 2009, where the **AROS
Development Team** has maintained and extended it since. Neptune is an independent fork, not
maintained by or affiliated with Chris Hodges or the AROS Development Team, and it is not
compatible with Poseidon - see [Compatibility with Poseidon](#compatibility-with-poseidon).
Releases 6.0 to 6.2 were published as "Poseidon for AmigaOS"; from 6.3 on the project is
Neptune.

## What you get

- **Plug and play** - devices appear and disappear as you connect them.
- **30 USB class drivers** - input, storage, networking, serial, printing, audio, MIDI and
  video ([list](#class-drivers)).
- **USB 3.0 SuperSpeed** - SuperSpeed hubs, and UAS mass storage with several commands in
  flight.
- **Power management** - idle devices and links drop into low-power states; a suspended
  keyboard or mouse wakes on use.
- **Neptune USB** - a control panel for every connected device and class.
- On PiStorm/Emu68, an optional **USB-in-ROM** kit: USB keyboard and mouse in the boot menu,
  booting from USB (see `ROM/ROM-ReadMe.md` in the archive).

The *host-controller driver* for your USB hardware is not included - see
[Talking to your USB hardware](#talking-to-your-usb-hardware).

## Requirements

- **Kickstart and Workbench 3.1 or newer**; a few details differ below 3.2 (see
  [Known limitations](#known-limitations)).
- A **68020 or better**; no FPU needed.
- A **USB host-controller driver**, such as `xhci.device` 6.x or `xhci-legacy.device` 5.x
  from the [Emu68 driver stack](https://github.com/rondoval/emu68-driver-stack) (PiStorm/Emu68
  on a Raspberry Pi 4 or CM4).
- **Installer 43.3 or newer** (OS 3.5 and later have it; Aminet `util/misc/Installer-43_3.lha`).
- **MUI 3.8 or newer** and a 640×480 screen, only for Neptune USB and the class settings
  windows.

## Talking to your USB hardware

A separate host-controller driver drives the USB hardware. You name it to the stack
(`USBStart xhci.device 1`, or in Neptune USB), and the stack asks it which interface it
speaks:

| | **Legacy interface** | **Context interface** |
|---|---|---|
| Spoken by | Classic Amiga USB cards (Deneb, Subway, …), `xhci-legacy.device` **5.x** | `xhci.device` **6.x** |
| SuperSpeed devices | seen as USB 2.0 | handled as SuperSpeed |
| SuperSpeed (USB 3) hubs | no | yes |
| UAS mass storage | one command at a time | bulk streams, several commands in flight |
| Link power management | up to the driver | the stack's policy, switchable in Neptune USB |

The legacy interface is the classic V1/V2 `IOUsbHWReq` contract, unchanged, so existing
drivers keep working. The context interface is Neptune's own and carries the USB 3.0 work:
use `xhci.device` 6.x where you can.

## Compatibility with Poseidon

Neptune is an incompatible fork:

- **Classes and tools.** Poseidon's classes and tools (the 4.x and AROS 5.x lines) do not
  run on Neptune, and Neptune's do not run on Poseidon: its library is `neptune.library`,
  and its class protocol has diverged from Poseidon's.
- **Host-controller drivers.** Neptune speaks the classic V1/V2 `IOUsbHWReq` interface and
  its own context interface. It does **not** implement Poseidon's **V3** host-controller
  interface, which the AROS Development Team designed with Chris Hodges, so a driver that
  depends on V3 does not work with Neptune. The context interface is not part of Poseidon's
  API; a driver written to it works with Neptune only.
- **Settings** live in `ENVARC:Sys/neptune.prefs`. If that file does not exist, Neptune
  imports `poseidon.prefs` once on first start; `poseidon.prefs` itself is never changed.
- **Names.** Commands and tools start with `USB` (`USBStart`, `USBRadio`, ...) and class
  drivers end in `.usbclass`, so installing Neptune replaces no Poseidon 4.x command, tool
  or class.
- **One stack at a time.** Neptune does not start its USB controllers while Poseidon is
  loaded, and says why in its log; reboot without Poseidon, or quit it and run `Avail FLUSH`.

## Download and install

Take the archive for your CPU from the [Releases](https://github.com/rondoval/neptune/releases)
page:

| Archive | For |
|---|---|
| `Neptune-<ver>-020.lha` | 68020 or 68030 (also runs on 040/060, untuned) |
| `Neptune-<ver>-040.lha` | 68040, and PiStorm / Emu68 |
| `Neptune-<ver>-060.lha` | 68060 |
| `Neptune-<ver>-<cpu>-serial.lha` | the same, plus diagnostics on the serial port |

Unpack it, run `Install` (double-click, or `Installer Install` from a Shell), answer the
questions and reboot. It installs `neptune.library` in `LIBS:`, the classes in
`DEVS:USBClasses/`, the commands in `C:` and Neptune USB in `SYS:Prefs/`, plus the optional
parts you tick: USBEject, the VHI driver, the settings-file icon and the gadget tools. It
never replaces a newer file without asking; to switch CPU variant, which has the same version
number, run it at the *Expert* level.

To start the stack at boot it adds one line to `S:User-Startup`, for the USB-C (OTG) port
(unit 0), the four USB-A ports of a Pi 4B (unit 1), or a device and unit you name:

```
C:USBStart >NIL: xhci.device 1
```

`USBStart` applies your saved settings; where they say nothing, it loads every class and
attaches the named controller.

**Upgrading from 6.0-6.2 ("Poseidon for AmigaOS").** The installer disables the old startup
block and offers to remove the old files - only those that report version 6 or newer, so a
classic Poseidon is never touched. Your settings are imported on first start, and sounds you
added are carried over.

## Settings worth knowing about

In **Neptune USB** (`SYS:Prefs/NeptuneUSB`):

- *Various Settings → Stack Settings*: **power-saving suspend** puts idle devices to sleep;
  **USB link power management** lets idle links drop into U1/U2 or L1 (context interface
  only; also per device). Turn either off if a device misbehaves when idle.
- **massstorage** (select the class, *Configure*): the handler, DOS name and buffers per
  filesystem (FAT, NTFS, exFAT, CD/DVD), what to automount, unmount on removal, UAS or BOT
  and the UAS queue depth. exFAT needs `L:exFATFileSystem` and `LIBS:filesysbox.library`,
  which are not in this archive.
- **The device list**: suspend, resume, power-cycle or safely eject a device by hand;
  power-cycling often revives a device that has stopped responding.

## Class drivers

Each is `DEVS:USBClasses/<name>.usbclass`.

| Category | Classes |
|---|---|
| **Hubs** | `hub`, `hubss` (USB 3 SuperSpeed) |
| **Human input (HID)** | `bootmouse`, `bootkeyboard`, `hid` (full HID parser), `egalaxtouch` (touchscreen) |
| **Mass storage & imaging** | `massstorage` (BOT / CBI / UAS), `ptp` (cameras), `dfu` (firmware upgrade), `rawwrap` |
| **Serial & modem** | `cdcacm`, `serialpl2303` (PL2303), `serialcp210x` (CP210x) |
| **Printer** | `printer` (provides `usbparallel.device`) |
| **Networking (SANA-II)** | `cdceth`, `asixeth` (ASIX), `pegasus` (Pegasus), `dm9601eth` (Davicom DM9601), `moschipeth` (MosChip), `ethwrap`, `rndis`, `lan78xx` (Microchip LAN78xx) |
| **MIDI & audio** | `simplemidi`, `camdusbmidi` (CAMD), `usbaudio` (USB audio → `ahi.device`) |
| **Video** | `usbvideo` (UVC webcams: Motion-JPEG, uncompressed, H.264; provides `usbvideo.device`) |
| **Other** | `bluetooth`, `stir4200` (IrDA), `palmpda`, `arosx` (Xbox gamepad) |

## Tools

- **USBStart** - starts the stack (the one startup command).
- **USBAddHardware**, **USBAddClasses** - attach or remove controllers and classes by hand.
- **USBList** - lists connected USB devices.
- **USBErrorlog** - prints the stack's error log.
- **USBEject** (optional, `SYS:WBStartup/`) - a Workbench **USB** menu with an *Eject* item
  per drive: it flushes and unmounts the drive's volumes (refusing while files are open), then
  stops the drive and says when it is safe to unplug.
- **Gadget tools** (optional, `SYS:Tools/`): `USBRadio`, `USBPencam`, `USBSonixcam`,
  `USBRocket`, `USBPowMan`, `USBUPS`, `USBVideo`.
- **`usbvideo.vhi`** (optional, `LIBS:VHI/`) lets VHI programs such as cAMIra use USB webcams.

## Known limitations

- **Not tested with classic Amiga USB cards.** The legacy interface works (`xhci-legacy.device`
  5.x runs on it), but no Deneb, Subway or similar card has been tried.
- **Below OS 3.2, held keys do not auto-repeat** - `input.device` gained the command that
  drives repeat (`IND_ADDEVENT`) in V47.
- **Below `workbench.library` 45, or under a Workbench replacement**, USBEject's items sit in
  the Tools menu.
- **On `lowlevel.library` older than 40.27**, gamepads have no analogue stick or rumble.
- **HID sound and key-string actions** need `datatypes.library` and `commodities.library`;
  without them those actions are skipped.

## Version numbers

`neptune.library` **6.x** continues the numbering of the releases published as "Poseidon for
AmigaOS", unrelated to Poseidon's own 4.x (Chris Hodges) and 5.x (AROS) lines. Every
component carries the same version - **6.3** here; the classes and tools need
`neptune.library` 6 or newer.

## Licence and credits

Neptune is distributed under the **AROS Public License (APL) Version 1.1** - see
[LICENSE](LICENSE). It is derived from the Poseidon sources in the AROS tree, maintained by the
**AROS Development Team** since 2009. Poseidon's original author and Initial Developer is
**Chris Hodges** (2002-2009), who placed his sources into AROS under the APL in 2009. Some
components carry their own licences, and MUI is a runtime dependency not included here; those
and the other third-party authors are named in [LEGAL](LEGAL).

---

## Building from source

With **docker**, `./build.sh` builds inside the shared toolchain container (pulled
automatically):

```sh
./build.sh --build                 # build everything
./build.sh --package               # ...and the Neptune-<ver>-<cpu>.lha archive
./build.sh --package --all-cpus    # one archive per CPU (020, 040, 060)
```

`CPU=68020|68040|68060` picks the CPU (default 68040; each gets its own `build-*/` tree),
`BACKEND=pistorm|serial|off` and `DEBUG=<level>` the debug output. With no flags
`./build.sh` also uploads to a live Amiga - see [CONTRIBUTING](CONTRIBUTING.md).

**Without the container** you need `m68k-amigaos` GCC 16.2 in `/opt/m68k-amigaos` with the
NDK 3.2 headers and `sfdc`, the MUI 5 SDK, the NDK 3.2 SANA+Roadshow package, `flexcat`,
`python3` and CMake 3.14+. The SDKs default to `$HOME/amiga/{MUI5,NDK3.2R4}`
(`AMIGA_SDK_ROOT`, `MUI_INCLUDE_DIR`, `SANA2_INCLUDE_DIR` override them):

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake   # add -DM68K_CPU=68020 -DM68K_FPU=soft etc.
cmake --build build -j"$(nproc)"
cmake --build build --target package
```

### Project layout

| Path | Contents |
|---|---|
| `neptune.library/` | The stack core. |
| `neptune_class/` | The base meta-class every class inherits (ABI headers). |
| `classes/` | The class drivers. |
| `neptuneusb/` | Neptune USB, the MUI control panel, and its translations. |
| `c/`, `tools/`, `usbeject/`, `vhi/` | Commands, gadget tools, USBEject, the VHI driver. |
| `romstartup/` | The Kickstart-ROM startup resident. |
| `include/` | Public headers. |
| `dist/`, `presets/` | Installer, icons, datatype, sounds. |
| `docs/` | Architecture and interface documents, the porting playbook, reference manuals. |
| `scripts/` | Container build wrapper and the scripts for porting from the AROS sources. |

The AROS Poseidon baseline Neptune was forked from is in [`AROS-BASELINE`](AROS-BASELINE),
tagged `aros-extract-base`.

### Documentation

In [`docs/`](docs/): the architecture documents - start with
[neptune.library-architecture.md](docs/neptune.library-architecture.md), then one per major
class (`hub`, `hubss`, `hid`, `massstorage`, `usbaudio`, `usbvideo`); the context interface
Neptune speaks to `xhci.device`, [neptune-context-hcd-abi.md](docs/neptune-context-hcd-abi.md);
the [porting playbook](docs/porting-playbook.md); and the reference manuals `neptune.doc`,
`neptune_class.doc` and `usbhardware.doc`.
