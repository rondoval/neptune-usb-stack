# Neptune - an incompatible fork of the Poseidon USB stack

A USB stack for **AmigaOS 3.1 and newer**. Plug in a keyboard, a mouse, a memory stick, a
USB sound card or a network adapter and use it from Workbench - hot-plug, unplug, no reboot.

Neptune is derived from **Poseidon** as maintained in the AROS tree. **Chris Hodges** wrote
Poseidon for AmigaOS (2002-2009) and placed it into AROS in 2009, where the **AROS
Development Team** has maintained and extended it since. Neptune started from those sources
and has gone its own way: a
host-controller interface of its own design, rewritten SuperSpeed hub handling, UAS mass
storage over bulk streams, USB link power management and a rebuilt mounting layer
([`RELEASE-NOTES.md`](RELEASE-NOTES.md) has the details).

**Neptune is not Poseidon.** It is an independent fork, not maintained by or affiliated with
Chris Hodges or the AROS Development Team, and it is not compatible with Poseidon - see
[Compatibility with Poseidon](#compatibility-with-poseidon). Releases 6.0 to 6.2 were
published as "Poseidon for AmigaOS"; from 6.3 on the project is Neptune.

## What you get

- **Plug and play** - devices appear and disappear as you connect them.
- **30 USB class drivers** - input, storage, networking, serial, printing, audio, MIDI and
  video ([list](#class-drivers)).
- **USB 3.0 SuperSpeed** - SuperSpeed hubs and devices handled as such; UAS mass storage
  with several commands in flight.
- **Power management** - idle devices and links drop into low-power states; a suspended
  keyboard or mouse wakes on use.
- **Neptune USB** - a control panel for every connected device and class.
- **Shell commands** and, on PiStorm/Emu68, an optional **USB-in-ROM** kit (USB keyboard
  and mouse in the boot menu, booting from USB; see `ROM/ROM-ReadMe.md` in the archive).

Not included: the *host-controller driver* for your USB hardware - see
[Talking to your USB hardware](#talking-to-your-usb-hardware).

## Requirements

- **Kickstart and Workbench 3.1 or newer** - a few details differ below 3.2, see
  [Known limitations](#known-limitations).
- A **68020 or better**; no FPU needed.
- A **USB host-controller driver**. `xhci.device` 6.x and 5.x from the
  [Emu68 driver stack](https://github.com/rondoval/emu68-driver-stack) (PiStorm/Emu68 on a
  Raspberry Pi 4 or CM4) are confirmed working.
- **Installer 43.3 or newer** for the `Install` script (Aminet `util/misc/Installer-43_3.lha`;
  OS 3.5 and later have one).
- **MUI 3.8 or newer** and a **640×480 or larger screen**, for Neptune USB and the class
  settings windows only; the stack itself needs neither.

## Talking to your USB hardware

A separate host-controller driver drives the USB hardware; you name it to the stack
(`USBStart xhci.device 0`, or in Neptune USB), and the stack asks it which of two
interfaces it speaks:

| | **Legacy interface** | **Context interface** |
|---|---|---|
| Spoken by | Classic Amiga USB cards (Deneb, Subway, …), `xhci.device` **5.x** | `xhci.device` **6.x** |
| SuperSpeed devices | seen as USB 2.0 | handled as SuperSpeed |
| SuperSpeed (USB 3) hubs | no | yes |
| UAS mass storage | one command at a time | bulk streams, several commands in flight |
| Link power management | up to the driver | the stack's policy, switchable in Neptune USB |

The legacy interface is the classic V1/V2 `IOUsbHWReq` contract, unchanged, so existing
drivers written to it keep working. The context interface is Neptune's own and carries the
USB 3.0 work; take `xhci.device` 6.x if your hardware has one.

## Compatibility with Poseidon

Neptune is an incompatible fork:

- **Classes and tools.** Poseidon's classes and tools (the 4.x and AROS 5.x lines) do not
  run on Neptune, and Neptune's do not run on Poseidon. Neptune's library is
  `neptune.library`, and its class protocol has diverged from Poseidon's.
- **Host-controller drivers.** Neptune speaks the classic V1/V2 `IOUsbHWReq` interface and
  its own context interface. It does **not** implement Poseidon's **V3** host-controller
  interface, which the AROS Development Team designed with Chris Hodges, so a driver that
  depends on V3 does not work with Neptune.
- **The context interface is not Poseidon's.** It is Neptune's own design and is not part
  of Poseidon's API; a driver written to it works with Neptune only.
- **Settings.** Neptune keeps its settings in `ENVARC:Sys/neptune.prefs`. On first start,
  if that file does not exist, it imports `poseidon.prefs` once; `poseidon.prefs` itself is
  never changed.

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
questions and reboot. The installer puts `neptune.library` in `LIBS:`, the classes in
`DEVS:USBClasses/`, the commands in `C:` and Neptune USB in `SYS:Prefs/`; USBEject, the
optional tools and the settings-file icon are separate questions. It never replaces a newer
file without asking - to switch CPU variant, which has the same version number, run it at
the *Average* or *Expert* level.

It can also add the one startup command to `S:User-Startup`:

```
C:USBStart >NIL: xhci.device 0     ; your host-controller device and unit
```

`USBStart` applies your saved settings; where they say nothing it loads every class and
attaches the named controller.

**Upgrading from 6.0-6.2 ("Poseidon for AmigaOS").** The installer disables the old
`Poseidon USB Stack` startup block and offers to remove the old files (`poseidon.library`,
`SYS:Classes/USB/`, `C:Psd*`, `SYS:Prefs/Trident` and friends) - only files that report
version 6 or newer, so a classic Poseidon is never touched. Neptune imports your settings on
first start and moves any sounds you added to `SYS:Prefs/Presets/Neptune/`.

## Settings worth knowing about

In **Neptune USB** (`SYS:Prefs/NeptuneUSB`):

- *Various Settings → Stack Settings*: **power-saving suspend** puts idle devices to sleep;
  **USB link power management** lets idle links drop into U1/U2 or L1 (context interface
  only; also per device). Turn either off if a device misbehaves when idle.
- **massstorage** (select the class, *Configure*): the handler per filesystem (FAT, NTFS,
  exFAT, CD/DVD) with its DOS name and buffers, what to automount, unmount on removal, UAS vs
  BOT and the UAS queue depth. exFAT needs `L:exFATFileSystem` and `LIBS:filesysbox.library`,
  which are not in this archive.
- **The device list**: suspend, resume, power-cycle or safely eject a device by hand;
  power-cycling often revives a device that has stopped responding.

## Class drivers

| Category | Classes |
|---|---|
| **Hubs** | `hub`, `hubss` (USB 3 SuperSpeed) |
| **Human input (HID)** | `bootmouse`, `bootkeyboard`, `hid` (full HID parser), `egalaxtouch` (touchscreen) |
| **Mass storage & imaging** | `massstorage` (BOT / CBI / UAS), `ptp` (cameras), `dfu` (firmware upgrade), `rawwrap` |
| **Serial & modem** | `cdcacm`, `serialpl2303` (PL2303), `serialcp210x` (CP210x) |
| **Printer** | `printer` (provides `usbparallel.device`) |
| **Networking (SANA-II)** | `cdceth`, `asixeth` (ASIX), `pegasuseth` (Pegasus), `davicometh` (DM9601), `moschipeth` (MosChip), `ethwrap`, `rndis`, `lan78xx` (Microchip LAN78xx) |
| **MIDI & audio** | `simplemidi`, `camdmidi` (CAMD), `audio` (USB audio → `ahi.device`) |
| **Video** | `video` (UVC webcams: Motion-JPEG, uncompressed, H.264; provides `usbvideo.device`) |
| **Other** | `bluetooth`, `stir4200` (IrDA), `palmpda`, `arosx` (Xbox gamepad) |

## Tools

- **USBStart** - starts the stack (the one startup command).
- **AddUSBHardware**, **AddUSBClasses** - attach or remove controllers and classes by hand.
- **USBList** - lists connected USB devices.
- **USBErrorlog** - prints the stack's error log.
- **USBEject** (optional, `SYS:WBStartup/`) - a Workbench **USB** menu with an *Eject* item
  per drive. It flushes and unmounts every volume (refusing while files are open), then
  stops the drive and confirms it is safe to unplug. Under a Workbench replacement the items
  appear in the **Tools** menu instead.
- Optional per-gadget tools in `SYS:Tools/`: `DRadioTool`, `PencamTool`, `SonixcamTool`,
  `RocketTool`, `PowManTool`, `UPSTool`, `UVCTool`.
- `usbvideo.vhi` in `LIBS:VHI/` lets VHI programs such as cAMIra use USB webcams; camera
  controls are in the camera's settings window in Neptune USB.

## Known limitations

- **BOT read throughput** is below what the drive can do (unaligned buffers copied on the
  driver side); a fix is in progress. UAS drives are not affected.
- **Not tested with classic Amiga USB cards.** The legacy interface works (`xhci.device` 5.x
  runs on it), but no Deneb, Subway or similar card has been tried.
- **Below OS 3.2, held keys do not auto-repeat** - `input.device` gained the command that
  drives repeat (`IND_ADDEVENT`) in V47. The same holds for the 68k `input.device` 50.x, which
  does not have it either.
- **Below `workbench.library` 45**, USBEject's items sit flat in the Tools menu.
- **On `lowlevel.library` older than 40.27**, gamepads have no analogue stick or rumble.
- **HID sound and key-string actions** need `datatypes.library` and `commodities.library`;
  without them those actions are skipped.

## Version numbers

`neptune.library` **6.x** continues the numbering of the releases published as "Poseidon for
AmigaOS" and is unrelated to Poseidon's own 4.x (Chris Hodges) and 5.x (AROS) lines. Every
component carries the same version - **6.3** here - and the classes and tools require
`neptune.library` 6 or newer.

## Licence and credits

Neptune is distributed under the **AROS Public License (APL) Version 1.1** - see
[LICENSE](LICENSE). It is derived from the Poseidon sources in the AROS tree, maintained by the
**AROS Development Team**. Poseidon's original author and Initial Developer is **Chris
Hodges**, who placed his sources into AROS under the APL in 2009. Some components carry their
own licences, and MUI is a runtime dependency not included here - see [LEGAL](LEGAL).

- **Chris Hodges** - original author of Poseidon (2002-2009).
- **The AROS Development Team** - maintainers of Poseidon in the AROS tree since 2009, whose
  sources Neptune is derived from.
- The third-party authors named in [LEGAL](LEGAL).

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
| `usbclass.library/` | The base meta-class every class inherits (ABI headers). |
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
`usbclass.doc` and `usbhardware.doc`.
