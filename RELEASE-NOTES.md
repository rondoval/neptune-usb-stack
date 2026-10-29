# Release notes - Neptune 6.3

Everything in this archive - `neptune.library`, all 30 class drivers, Neptune USB, USBEject
and the command-line tools - reports version **6.3**, followed by the CPU it was built
for.

**Upgrading from 6.2:** run the installer; it replaces the old installation and your
settings are imported (see below).

## Poseidon for AmigaOS is now Neptune

At the request of the AROS Development Team, this project no longer presents itself
 as Poseidon. From 6.3 it is **Neptune - an incompatible fork of the Poseidon USB stack**.

**Neptune is not compatible with Poseidon:**

* Poseidon's classes and tools (the 4.x and AROS 5.x lines) do not run on Neptune, and
  Neptune's do not run on Poseidon.
* Host-controller drivers written for **Poseidon's V3 host-controller interface** are not
  supported. Neptune speaks the classic V1/V2 interface and its own context interface; the
  context interface is not part of Poseidon's API.

**What changed names:**

| | 6.0-6.2 | 6.3 |
|---|---|---|
| Library | `LIBS:poseidon.library` | `LIBS:neptune.library` |
| Class drivers | `SYS:Classes/USB/` | `DEVS:USBClasses/` |
| Control panel | `SYS:Prefs/Trident` | `SYS:Prefs/NeptuneUSB` ("Neptune USB") |
| Commands | `PsdStackLoader`, `PsdDevLister`, `PsdErrorlog` | `USBStart`, `USBList`, `USBErrorlog` |
| Settings | `ENV(ARC):Sys/poseidon.prefs` | `ENV(ARC):Sys/neptune.prefs` |
| Sounds | `SYS:Prefs/Presets/Poseidon/` | `SYS:Prefs/Presets/Neptune/` |
| Device popups | "Poseidon Popups" (PoPo) | "USB device notifications" |
| Settings-file datatype | `PSD` | `Neptune` |
| Archive | `Poseidon-<ver>-<cpu>.lha` | `Neptune-<ver>-<cpu>.lha` |

`AddUSBHardware`, `AddUSBClasses`, `USBEject` and the optional tools keep their names.

**What the installer does:**

* It disables the `Poseidon USB Stack` block in `S:User-Startup`, so the old stack is not
  started next to Neptune, and adds a `Neptune USB Stack` block if you ask for autostart.
* It offers to remove the 6.0-6.2 files listed above. Only files that report version 6 or
  newer are removed, so a classic Poseidon is never touched, and `poseidon.prefs` is kept.
* It copies sounds you added to `SYS:Prefs/Presets/Poseidon/` into the new drawer.

**Your settings:** on first start, if there is no `neptune.prefs`, Neptune imports
`poseidon.prefs`, moves the class and sound paths to the new drawers, and saves the result
as `neptune.prefs`. Loading an old settings file by hand converts it the same way.
Neptune USB's saved window positions start afresh, because the program's name changed.

**Programmers:** the library is opened as `NEPTUNENAME` (`"neptune.library"`), version
`NEPTUNE_LIB_MIN_VERSION`, with `<libraries/neptune.h>` and `<proto/neptune.h>`; the base is
`NeptuneBase`. The `psd*()` function names, structures and tags are unchanged.
Classes built for 6.0-6.2 open `poseidon.library` and must be rebuilt.

The notes for 6.0-6.2 below keep the names those releases used.

## USB cameras

The new `usbvideo.class` drives webcams that follow the USB Video Class standard (UVC 1.0,
1.1 and 1.5). It needs `xhci.device` 6.4 from the Emu68 driver stack on a Pi 4 or CM4.
Tested with a Logitech C930e, up to 1920x1080 at 30 frames a second.

* **Formats:** Motion-JPEG and uncompressed (YUY2, NV12 and others) in every size and rate
  the camera offers, and H.264 from cameras that carry it inside Motion-JPEG, such as the
  C930e. Other H.264 and VP8 cameras and bulk-transfer cameras should work but are untested.
* **cAMIra and other VHI programs:** the Installer offers to put `usbvideo.vhi` into
  `LIBS:VHI/`. Each camera is one input.
* **Camera settings** in Neptune USB: brightness, exposure, focus, white balance and the other
  standard controls, applied at once and saved per camera.
* **`UVCTool`** (optional tools) lists a camera's modes and saves pictures or H.264 video,
  e.g. `UVCTool TO=RAM:shot`.
* **The camera's microphone** shows up as an AHI recording mode, and an idle camera is
  suspended when power saving is on.

Not yet supported: choosing the frame rate from a VHI program, still images, and
maker-specific controls.

Programmers: see `<devices/usbvideodevice.h>`.

## USB audio

* **Record-only devices** such as webcam microphones now get their own AHI modes, named
  "Rec".
* **Devices with several outputs** (loudspeaker and headphones, say): AHI's output choice
  now works - "All outputs", or a single output by name if the device can mute each one.
* **Volume and gain sliders** now follow the device's range evenly instead of losing most of
  the level in the first steps, and the volume no longer lands on the wrong control.
* **Fixed:** 24 bit mono recording, 8 bit recordings that were far too quiet, timing on high
  speed devices, and the sample rate set on the device.

Only the USB Audio 1.0 standard is supported.

## USB MIDI

* **Plugging in a MIDI interface no longer crashes the machine.**
* **Sending and receiving MIDI now work**, including System Exclusive of any length.
* **One CAMD device per interface**, with as many ports as the interface really has, so the
  MIDI Ports editor shows the right number. Two identical interfaces each get their own name,
  and every port can be used at once.
* **USB MIDI 2.0 devices** are used in their MIDI 1.0 mode.
* The settings window is gone - it had nothing to set. Name ports in the MIDI Ports editor.

**Upgrading:** the new CAMD driver is written to `DEVS:Midi/` the first time you plug the
interface in. Quit your CAMD programs and reboot, then, with camd.library 37, add the
interface once in the MIDI Ports editor.

## USB in ROM

* The custom Kickstart holds 1.5 MB of modules instead of 1 MB.
* The build script is now `python3 ROM/build-kickstart.py`, with the same arguments.

## Bug fixes

* Rare crashes and freezes under heavy load.
* **Behringer UMC204HD** now works, and its MIDI port appears once instead of twice.
* **NTFS and exFAT:** The filesystem stack size is now a setting in the mass storage settings.
* A busy device is no longer logged as a failed suspend.
* No more "Adding RT Iso Handler failed" and silent audio after a device refused a setting
  change.
* A hub could stop seeing new devices after one was unplugged, until a reboot.
* Possible crash when a class driver was unloaded while still in use.
* Better recovery from errors on UAS disks.
* Possible crash on disk change with several partitions.
* **USB keyboards with `input.device` 50.x** (the 2003 68k one, version 50.26, found in some
  custom Kickstarts): a key pressed once no longer repeats for ever. The classes now use the
  3.2 `input.device` command only when the device reports that it has it.

# Release notes - Neptune 6.2 (released as Poseidon for AmigaOS 6.2)

Everything in this archive - `poseidon.library`, all 29 class drivers, Trident, USBEject
and the command-line tools - reports version **6.2**, followed by the CPU it was built
for.

**Upgrading from 6.1:** install the whole archive. Your settings are untouched. Every
binary in this release was rebuilt, so replace all of them rather than picking out
`poseidon.library`.

## One Suspend/Resume button

Trident's Devices page used to have separate **Suspend** and **Resume** buttons, one of them
always greyed out. It now has a single button that says **Suspend** or **Resume** depending on
the selected device's state, and follows it live, including when the device is put to sleep by
the power-saving timeout. The button is greyed out when suspending cannot work: the host
controller driver does not support it, or the hub the device is plugged into has no driver.

## Fewer stuck USB devices

When a USB device stalls, the library now recovers it, even for class drivers that never
did. You may see a line in the error log saying so.

## USB storage settings that stay as you set them

The compatibility switches of `massstorage.class` (Simple SCSI, Fix Capacity, Trim Inquiry and
so on) could change behind your back, and once they had, changing the class defaults no longer
reached that device. This release closes the gaps:

* **No Fallback is honoured everywhere.** With it ticked, the class no longer switches a
  workaround on by itself or saves one. When a device looks as if it needs one, the error log
  says which, once per plug-in - for example *"Simple SCSI" is probably needed for this device.
  Please check this.* The built-in workarounds for the few devices the class knows by name
  still apply.
* **Forget Device.** The settings window of a storage device has a new **Forget Device**
  button. It drops the settings saved for that one device, so the class defaults apply to it
  again. Trident's own data for the device, such as a custom name, is kept. (Trident's Config
  page could always remove such an entry; this is the short way.)
* **A changed NAK timeout reaches drives that are plugged in.** It used to wait for the next
  replug, and setting it to 0 did not switch it off on a running drive at all. (The UAS queue
  depth, the unit number and what is mounted still need a replug.)
* **USB in ROM:** the ROM stack starts before your settings can be read. Workarounds it
  picked up during that time could end up saved as if you had chosen them; they no longer do.
* **Built-in workarounds stay on.** The handful of devices the class knows by name (certain
  Genesys, Olympus and Prolific bridges, among others) lost their built-in workaround whenever
  settings were saved or loaded while they were attached. It now stays.

## One startup command

`PsdStackLoader <device> <unit>` now starts the whole stack, and the installer writes that one
line to `S:User-Startup`. `AddUSBHardware` and `AddUSBClasses` are no longer part of the
startup; an existing three-line startup keeps working.

## Bug fixes

* **USB Attached SCSI (UAS) drives** that reject a command no longer fail to mount with "NAK
  timeout" errors.
* **USB sticks and drives that stop answering** no longer hang their volume. The AROS line
  had added a "device is busy" retry for Bulk-Only devices: after a NAK timeout it raised the
  timeout to one or two minutes, tried again without resetting the device, and kept trying
  without limit. **That change is reverted.** A timeout is again an ordinary error: the device
  is reset, the command is tried once more, and then the error is reported. The NAK timeout
  you set is never changed for you, and its default for new installations is 30 seconds
  instead of 60.
* **Card readers and other multi-slot devices:** the compatibility switches of the first slot
  leaked into every other slot, and could be saved for them.
* **Drives that report a made-up geometry:** the automatic switch to Simple SCSI for them
  never took effect. It does now, unless No Fallback is ticked.
* **Trident, Config page:** long device entries were cut off at the end, which removed the
  serial number, the one part that tells two identical sticks apart. They are now shortened
  in the middle.
* **Serial adapters, printers and Palm devices:** a failed write could go unnoticed, because the
  wrong half of the connection was checked for errors. It is now reported.
* **Power saving:** many network and storage devices could never be put to sleep; they now
  can. A device that refuses to suspend is no longer asked again at every timeout.
* **USB audio:** a playing device could be put to sleep by the power-saving timeout. A running
  stream now keeps it awake.
* **French:** some Trident texts showed stray characters; fixed.
* **Composite devices** (two-interface keyboard/mouse receivers, headsets): after a resume,
  only the first interface came back to life; now all of them do.
* **Running a class driver as a program** (typing its name in the Shell) no longer crashes; it
  now returns an error.
* **ROM builds:** modules now report their true size, so Kickstart no longer rescans code it
  has already passed.

## Fixes ported from AROS

Poseidon is still developed in AROS, and fixes made there are brought over. The AROS commit
each one comes from is given in brackets.

* **USB audio:** a program that opened more AHI channels than the USB device has outputs - a
  four-channel module player on a stereo DAC, for instance - made AHI write past the end of its
  channel table. The class no longer replaces the channel count the program asked for.
  (`31794523`)
* **USB network adapters** (asixeth, cdceth, davicometh, ethwrap, moschipeth, pegasuseth,
  rndis): once a program had opened the device for exclusive use and closed it again, nothing
  else could open it - every later attempt failed with "unit busy" until the adapter was
  unplugged. Closing it now releases the exclusive claim. Not tested here. (`9a984adb`)
* **Trident in French:** the "Various Settings" heading on the Options page was shown in
  English although a translation existed. (translation repository, `1ac62187`)
* **Garbled copyright sign in About windows:** Trident, the device popup and the settings
  windows of the audio, mass-storage, HID control, boot keyboard, boot mouse, MIDI, Palm and
  raw-wrapper classes showed `ï¿½` where the © belongs, and the game-controller class showed
  `Â©`. The same goes for "Moiré" in the HID usage names. All non-ASCII characters in program
  text are now written so that no editor or tool can re-encode them, and the copyright notices
  use "(C)", which reads the same in every character set. (`717dd468`)
* **Devices missing behind a hub:** a hub that reports too short a power-on time was scanned
  too early, and a slow-starting device could be missed. External hubs now always get at least
  100 ms. (`16dc9e95`)
* **USB storage:** the capacity query is now retried once when a drive answers it with "unit
  attention" after a reset, like the class's other commands. (`3312e74c`)
* **Bluetooth dongles:** Broadcom dongles that use a vendor-specific interface class are now
  recognised; commands no longer block event reception; and event reception continues after a
  transfer error. Ported as they are; not tested here. (`ec7643e2`, `81516dcd`, `d02f90d5`)
* **USB in ROM without saved settings:** keyboard and mouse stayed on the boot classes, because
  the handover to `hid.class` only ran when a prefs file existed. It now runs on the first
  start from disk in every case. (`555fa3ae`)
* **Mouse wheel before `hid.class` takes over:** `bootmouse.class` now has wheel support on by
  default, and no longer reads a wheel byte the mouse did not send. A saved bootmouse setting
  is kept. (`555fa3ae`)
* **USB tethering (RNDIS):** `rndis.class` now binds the RNDIS interface instead of the whole
  device, and recognises the common 2/2/255 form as well as the wireless one. A composite
  device keeps its other interfaces for their own classes. (`4e1bd6ef`)
* **LAN78xx network adapters:** received data is now read through four requests at a time, and
  a busy receive path can no longer hold up sending. Ported without the upstream change to the
  chip's idle behaviour; not tested here. (`14ff79a6`)

# Release notes - Neptune 6.1 (released as Poseidon for AmigaOS 6.1)

Everything in this archive - `poseidon.library`, all 29 class drivers, Trident, USBEject
and the command-line tools - reports version **6.1**, followed by the CPU it was built
for.

**Upgrading from 6.0:** install the whole archive. Safe eject adds a `poseidon.library`
function that `massstorage.class`, Trident and USBEject call, and a new class against an
old library would guru. Your settings are untouched.

## An archive for your CPU

6.0 shipped one `-m68040 -mhard-float` build, which a 68020 or 68030 could not run at all.
6.1 ships **three archives - `-020`, `-040` and `-060`**, each with the usual `-serial`
diagnostic counterpart, and **none needs an FPU**. The `-020` archive runs on all three
CPUs, just untuned for the faster two.

All three carry the **same version number** - it is the library's ABI version, not the build
variant - so `Version LIBS:poseidon.library` is how you tell which one you installed:
`poseidon.library 6.1 (…) Poseidon for AmigaOS 68040`.

**Switching CPU variant needs the version requester.** Installing one variant over another
is a same-version copy, which the installer's version check skips. Run the Installer at the
*Average* or *Expert* user level, where it offers to overwrite, or delete the installed
files first.

## USB in ROM

On PiStorm/Emu68 the stack can now live inside a custom 2 MB Kickstart, which gets you a **USB
mouse and keyboard in the boot menu** and lets the machine **boot from a USB drive** - neither
of which is possible when the stack starts from `S:User-Startup`.

The kit is in the archive's **`ROM/` drawer**: the ROM startup resident, the build script and
`ROM-ReadMe.md`. You run it **on a PC** (Linux, or Windows with WSL) with Python and amitools,
feeding it your own Kickstart plus two files from the Emu68 driver archive. It is optional and reversible: nothing on the Amiga's hard drive changes, your normal
installation keeps working, and undoing it is one line in `config.txt`.

Because the drawer is for the PC, the Installer ignores it, and the `-serial` archives do not
carry it.

## Safely remove hardware

USB drives can now be ejected cleanly before you unplug them. **USBEject** (optional,
`SYS:WBStartup/`) keeps an *Eject* item per drive in its own **USB** menu on Workbench;
Workbench replacements emulate only the flat AppMenu, so the items land in **Tools**
instead (under **Directory Opus 5**, switch *Show Tools menu* on). Trident's Devices page
has an **Eject** button that needs no Workbench at all. Both are localized in the usual six
languages.

An eject flushes every volume and asks each filesystem to inhibit. **A filesystem that
reports a volume still in use vetoes the whole eject, and the requester names it** - nothing
is unmounted, so there is no half-ejected state. Otherwise the write cache is synced, the
drive stopped and its hub port switched off, and a requester confirms it is safe to pull.

Two things it cannot see. A filesystem that does not implement `Inhibit()` - some CD
handlers - cannot report itself busy, so the eject proceeds once its buffers are flushed;
the error log says so when this happens. And raw access below the filesystem, such as a
backup or imaging tool on `usbscsi.device`, is invisible to it, exactly as on other systems.
Close your files before ejecting a disc.

## MUI 3.8 is enough

6.0 required MUI 5 for Trident and the per-class settings dialogs. They now open on
**`muimaster.library` 19 and up** - MUI 3.8, MUI 4.0 and MUI 5 alike, with nothing given up
for it.

## Kickstart 3.1 is enough

The floor is now **Kickstart and Workbench 3.1**, whole distribution. The big one: `hid.class`
sent every keystroke and mouse movement with an `input.device` V47 command that older systems
rejected and dropped, unlogged - so below 3.2 **HID keyboards and mice did not work at all**. It
now falls back to the V40 command. Trident no longer requires `icon.library` 44 nor USBEject
`workbench.library` 45; a `lowlevel.library` older than 40.27 no longer risks memory corruption
when a gamepad binds; `datatypes.library` and `commodities.library` are optional; and `Install`
checks for Installer 43.3 up front.

Differences below 3.2: see **Known limitations** in `README.md`.

## A name and buffer count per filesystem

Mass storage used to apply one DOS name and one buffer count to everything it mounted, so a
CD came up as `UMSD3` among your USB sticks with hard-disk buffering. The *LUN Settings* page
now carries a *Mount name and buffers* row per filesystem - FAT, NTFS, exFAT, CD/DVD - and,
like everything there, **per LUN**, so each slot of a card reader can be named separately.
Out of the box discs mount as `UCD0` with 25 buffers and sticks stay in the `UMSD0…` sequence
with 100. RDB partitions are unaffected.

**Upgrading:** your existing name and buffer count carry over to *every* filesystem, so
mounts come up where they always did. Change the CD row if you want the new `UCD*` pool.

## exFAT sticks mount

Modern USB sticks - anything above 32 GB, and most of what you buy preformatted - are exFAT,
and mass storage previously recognized them only well enough to skip them. They now mount,
whether the stick is a superfloppy or carries an MBR or GPT partition, identified by its own
boot sector, so one mislabelled as NTFS still lands on the right handler.

**Two free downloads this archive does not contain are required:** `exFATFileSystem` in `L:`
(relan's libexfat, ported by Fredrik Wikström, 68k branch by Tobias Karlsson; read *and*
write) and `filesysbox.library` 53 or newer in `LIBS:`, which it runs on - a 68020 binary, so
exFAT needs an 020 or better. Both are configured out of the box; without them exFAT media
are skipped. Clearing a handler row is now the off switch for any filesystem.

**Eject exFAT drives before unplugging** - the handler writes a volume-clean flag on its way
out, and pulling the stick first leaves it marked dirty.

## Every disc ODFileSystem can read

A disc used to be refused unless it carried an ISO 9660 volume descriptor - all the old
`CDFileSystem` can read. Mass storage now recognises **ODFileSystem** by name and lets it
identify discs itself, so **High Sierra, UDF, HFS and HFS+** mount alongside ISO 9660 (Joliet
and Rock Ridge included), and an **audio CD** mounts with its tracks as playable WAV files. An
unreadable disc is still refused, and any other handler keeps the ISO-only check.

**ODFileSystem is not in this archive** - it is a free download (Stefan Reinauer's, BSD).
Fresh installs set the CD/DVD *DosType* to `CD01`, its own, and the configured handler now
takes precedence over an older CD filesystem in a controller ROM. Existing settings keep
working.

---

## Bug fixes

* **USB keyboard:** a key pressed for a command that then scrolls output for a while - Enter on
  a long `List`, say - could come back as several extra key presses once the command finished.
  The USB input tasks now run at a higher priority, so a busy console (or a task scheduler such
  as Executive) can no longer delay the key release.
* **USB mouse wheel:** scrolling under the same kind of load can no longer add extra steps.
* **USB keyboard:** Print Screen, Scroll Lock and Num Lock no longer act as a key that is never
  released, and pressing more keys than the keyboard can report no longer releases and re-presses
  the others.
* **USB keyboard + mouse combos:** Ctrl-Alt-Del is detected reliably.

# Release notes - Neptune 6.0 (released as Poseidon for AmigaOS 6.0)

The first release of this fork, published as **Poseidon for AmigaOS**: the AROS Poseidon
sources rebuilt for AmigaOS, and taken further.

Poseidon was Chris Hodges' USB stack for AmigaOS (2002-2009). In 2009 he placed the
sources into AROS, and the AROS Development Team has carried them forward since - gaining
a SuperSpeed hub class, more class drivers and a long tail of fixes. This fork starts from
that version (AROS commit `c01498ab`), rebuilt for **AmigaOS 3.2** on 68k.

**The port is only half of it.** Roughly as much of this release is new work that has no
counterpart in the AROS line: a second, purpose-built interface to the host
controller and the reworked SuperSpeed handling that rides on it, UAS mass storage over
bulk streams with several commands in flight and real error recovery, USB link power
management under your control, a rebuilt mounting layer, and the isochronous-audio and
suspend fixes. Everything below is the delta against that AROS baseline, and each section
says plainly whether it is new here or a port of something that was already there.

Everything in this archive - `poseidon.library`, all 29 class drivers, Trident and the
command-line tools - reports version **6.0**.

---

## What's new since the AROS 5.x line

### It runs on AmigaOS 3.2

**This is the port half.** The whole distribution was rebuilt as a native AmigaOS program
set: the AROS build system is gone, and with it the parts that only make sense on AROS -
AROS's own USB host controllers, the hosted virtual controller, and the Allwinner-specific
`felsunxi` class. What remains is 29 class drivers, the library, Trident and five shell
commands, plus an Installer script, icons, a datatype so Poseidon preset files show their
own icon, and Trident's translations (Czech, French, Italian, Polish, Russian, Spanish).

### Two host-controller interfaces

**New here.** Poseidon has always talked to a host-controller driver by filling in a
`struct IOUsbHWReq` and sending it - the classic V1/V2 request. It describes a transfer
the way the USB 1.1 and 2.0 controllers of its day wanted one: an address, an endpoint, a
buffer. A USB 3.0 controller wants something quite different. It keeps *contexts* - durable
per-device and per-endpoint state it is told once - and a transfer is then a tiny reference
to those contexts. Handed only classic requests, an xHCI driver has to reconstruct the
topology by watching the traffic go past and guessing what the stack is doing.

Poseidon addresses this with its **V3 interface**, designed by the AROS Development Team
with Chris Hodges, which carries the topology and SuperSpeed parameters with each transfer.
This fork took a different approach and **does not implement V3**: its own **context
interface** establishes a device and its endpoints once, up front, so the transfers stay
small. It is not part of Poseidon's API, and a V3 driver does not work with this stack.
It is what `xhci.device` 6.x speaks, and where everything below in this section comes from.

**The legacy interface stays, and stays frozen.** The classic V1+V2 `IOUsbHWReq` layout
and every `UHCMD_*`/`UHIOERR_*` value are untouched binary contract, because classic Amiga
USB cards - Deneb, Subway and friends - depend on them. The stack keeps a complete second
backend for that path; it is not a compatibility stub.

Both are confirmed working: **`xhci.device` 6.x** on the context interface, and
**`xhci.device` 5.x** on the legacy one. You tell the stack which driver to open, and it
asks that driver which interface it implements. On the legacy interface you get the
classic feature set - USB 2.0-style device handling, no `hubss.class` binding (so no
SuperSpeed hubs), no UAS bulk streams, and whatever link power management the driver
chooses to do on its own, with no control from Trident.

### USB 3.0 that behaves like USB 3.0

**New here**, on the context interface. A SuperSpeed device now reaches the stack *as* a
SuperSpeed device instead of being disguised as USB 2.0 on the way in, and the SuperSpeed
hub driver was largely rewritten around that: real SuperSpeed port status handling, warm
reset, and no leftover low/full-speed machinery that a USB 3 hub never needs.

**A USB 3 drive no longer shows up twice.** A USB 3 hub presents itself as two hubs - a
USB 2.0 half and a SuperSpeed half - and a SuperSpeed device appears on both. The two
halves are now recognised as one hub (they share a Container ID) and the duplicate is
dropped. The same race could also hang the machine while a device was being plugged in;
that is fixed too.

### Mass storage: UAS, and much faster

**New here.** The AROS line had the outline of a UAS transport; it is now a working one.
Several commands run on the drive at once over USB 3.0 bulk streams, with a **queue
depth** you can set in the settings, and UAS is preferred over the older BOT transport
when a drive offers both (the preference is a switch, so you can force BOT). Bulk streams
need the context interface; on the legacy one UAS falls back to one command at a time.

The class also stopped getting in its own way: reading geometry, mode pages, `HD_SCSICMD`
passthrough and seeks used to stall all block I/O while they ran, and no longer do. NAK
timeout handling was reworked, which mostly matters to CD and DVD drives that take their
time.

**Recovery instead of a hang.** When a command wedges, the class now aborts just that
command with a UAS task-management request; if the drive ignores that, it resets the
device. Previously a stuck command took the drive - and sometimes the machine - with it.

### Mounting, rewritten

**New here.** Every kind of medium is now mounted through the A4091 mounter (the partition
parser from `a4091.device`), which handles **RDB, MBR, GPT and superfloppy** disks from
one place. In the mass-storage settings you choose which handler to use for **FAT**,
**NTFS** and **CD/DVD** partitions, and audio CDs are recognised as such. The automount
switches - RDB, MBR/GPT, CD/DVD, and unmounting on removal - are all per-category.

### USB power management

**New here**, on the context interface: a **link power management** switch that lets an
idle link drop into a low-power state between transfers (U1/U2 on SuperSpeed, L1 on
High-Speed) and enables Latency Tolerance Messaging. Entry and exit are handled by the
controller and are invisible to transfers; turn it off if a device misbehaves when idle.
It applies immediately, both ways, and can be overridden per device. (A legacy-interface
driver may do link power management of its own accord - `xhci.device` 5.x does - but the
stack has no say in it and Trident's switch does nothing there.)

Poseidon's older power-saving suspend got a working-over alongside it: root hubs can now
be suspended, and remote wakeup is always armed before a device is put to sleep, so a
suspended keyboard or mouse brings its own link back up the moment you use it instead of
needing a replug. Devices that should not sleep - a mounted drive, an active HID device -
no longer do. Unplugging a device *while it was suspended* used to leave it lingering in
the stack; that is fixed.

### USB audio keeps playing

Rescanning the class drivers used to tear down isochronous transfers, which meant audio
stopped whenever the stack rescanned. The scan was reworked so it does not. A wrong
endpoint address the audio class sent on the wire - which `xhci.device` used to carry a
workaround for - was fixed at the source.

### "Use plain, factual messages"

*(Removed in 6.3 - the plain wording is now the only one.)*

**New here.** Poseidon traditionally reports itself with a certain amount of humour. In Trident,
*Various Settings → Logging Options → **Use plain, factual messages*** swaps every log
entry, popup and requester for a short factual one. It is **off** by default, so the
traditional wording is what you get unless you ask for otherwise, and it takes effect
immediately for new messages.

### Other fixes

- The HID class settings window no longer crashes when opened.
- Hubs cope with a device disappearing mid-operation instead of reporting a string of
  spurious errors.
- After a resume, hubs no longer submitted the same status-change request twice.
- Unknown hubs and devices get a sensible name in Trident instead of a blank one.
- A `TD_SEEK`/`TD_SEEK64` on a mass-storage unit used to send an uninitialised command to
  the drive.
