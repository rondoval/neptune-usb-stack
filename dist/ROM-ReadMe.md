# USB in ROM

For a **PiStorm with Emu68**. This puts the USB software into a custom Kickstart file, so that:

* a **USB mouse and keyboard work in the boot menu**
* the Amiga can **boot from a USB drive** (or from NVMe)

It is optional and easy to undo. Nothing on the Amiga's hard drive changes: you add one file to the
SD card and change one line in `config.txt`.

You do this on a **PC** (Linux, or Windows with WSL), not on the Amiga.

## 1. Get these ready

* **Python 3 and amitools.** Install amitools with `pipx install amitools`.
* **This archive, unpacked.** Keep its drawers together; the script takes files from `LIBS/` and
  `DEVS/` next to this one.
* **Your Kickstart.** The AmigaOS 3.2 ROM file that is on your SD card now (512 KB). Copy it to
  the PC. It must be the original file, not one that was already modified.
* **The Emu68 driver archive**, unpacked:
  [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack). You need two files from
  it: `LIBS/bcmpcie.library` and `DEVS/USBHardware/xhci.device`.

Optional extras:

* `DEVS/nvme.device` from the driver archive, to boot from NVMe.
* `ODFileSystem`, to boot from a CD.

## 2. Build the Kickstart

In the unpacked archive, run this as one command. Use your own file names and paths:

```
python3 ROM/build-kickstart.py  kick.rom \
     ../emu68-drivers/LIBS/bcmpcie.library \
     ../emu68-drivers/DEVS/USBHardware/xhci.device
```

To add `nvme.device` or `ODFileSystem`, put them at the end of the line.

The result is a new file, `kick-usb-2m.rom`, in the directory you ran the command from. Your own
Kickstart file is not changed.

The script lists what it put in, with version numbers:

```
   -41   NT_LIBRARY   bcmpcie.library          $VER: bcmpcie.library 2.5 ...
   -42   NT_DEVICE    xhci.device      $VER: emu68-xhci-driver 6.4 ...
   -44   NT_LIBRARY   neptune.library          ...
```

Check that `bcmpcie.library` and `xhci.device` are in that list.

## 3. Put it on the SD card

1. Copy `kick-usb-2m.rom` to the SD card's FAT partition, next to your Kickstart. Leave the old
   file there.
2. Open `config.txt` and change the `initramfs` line to the new file:

   ```
   initramfs kick-usb-2m.rom
   ```

3. Start the Amiga.

Nothing has to change on the Amiga itself. Your startup files stay as they are.

## To undo it

Put the old file name back in the `initramfs` line.

## When you update the drivers

Build the Kickstart again and copy it to the SD card. The Amiga uses the `xhci.device`,
`nvme.device` and `bcmpcie.library` that are inside the Kickstart, not the ones on the hard drive.
Installing newer ones on the Amiga alone changes nothing.

## If something goes wrong

**The Amiga does not start, or stops before the boot menu.** Undo it as described above.

**It starts, but USB does not work.** Look at the list the script printed: `bcmpcie.library` and
`xhci.device` must both be in it. If they are, open Neptune USB (the settings program in
`SYS:Prefs`) and read its log. `No xhci.device unit found` means the driver did not start. The
usual reason is a driver from the `-rangeops` archive on an Emu68 that does not support it; use
the plain driver archive.

**A USB drive works, but is not in the boot menu.** The drive must be partitioned the Amiga way
(RDB) and its partition marked bootable. For a CD, check that you added `ODFileSystem`.

**The script says `romtool not found`.** amitools is not installed, or its directory is not on
your PATH.

**The script says `scanBounds table not found`.** It does not recognise your Kickstart. Use the
original AmigaOS 3.2 Kickstart file, not one that was already modified.
