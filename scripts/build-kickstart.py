#!/usr/bin/env python3
"""Link the ROM-resident part of the USB stack into a custom 2 MB Kickstart image, so
USB comes up before strap picks a boot volume: mouse and keyboard in the early boot
menu, and booting from a USB or NVMe drive.

This is an advanced, entirely optional path. It does not replace, and does not touch,
the normal filesystem installation - see ROM-ReadMe.md.

The seven Neptune ROM modules are found automatically - from the archive this script
ships in (the ROM/ drawer of Neptune-<ver>-<cpu>.lha, beside LIBS/ and DEVS/), or
from the build tree when run out of a source checkout. Anything else is passed on the
command line, so this stays driver-agnostic: on PiStorm/Emu68 that is normally
bcmpcie.library + xhci.device, plus nvme.device if you want to boot from NVMe.

Every module has to be ROM-clean - no writable data at all, because the image is mapped
read-only and writes to it vanish silently.

Prerequisites: romtool from amitools (pipx install amitools), and your own stock
512 KiB AmigaOS 3.2 Kickstart.
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# --- the image -----------------------------------------------------------------------
# Emu68 maps a 2 MB Kickstart in four 512 KiB chunks: chunk 0 -> $E00000, chunk 1 ->
# $A80000, chunk 2 -> $B00000 (1 and 2 being one contiguous megabyte), and chunk 3 ->
# $F80000. $F00000 is not mapped. So the image is:
#
#     [ 512 KiB bank @ $E00000 ][ 1 MB bank @ $A80000 ][ Kickstart @ $F80000 ]
#
# Both banks hold modules. The megabyte is filled first; the 512 KiB bank takes what
# does not fit there.
#
# Chunk 0 has one more job. Emu68 starts the m68k from the SSP and PC at addresses 0
# and 4, and while OVL is set - which it is at reset - it answers the first 512 KiB of
# the address space from chunk 0, not from $F80000. So chunk 0 has to begin with a reset
# vector, and the header romtool puts on an extension ROM is one: its second long is
# $F80002, the Kickstart's own JMP to its entry point. Exec switches OVL off before it
# reads anything in low memory, so those eight bytes are all that is ever fetched this
# way and the rest of the chunk is free. The header is why that bank is built even when
# no module lands in it.
CHUNK = 512 * 1024
ROM_BASE = 0xF80000
ROM_END = ROM_BASE + CHUNK
ROM_HEADER = 0x11144EF9  # BIG_ROMS + JMP.  NOT $1111, which exec treats as a diag cart.
RESET_VECTOR = struct.pack(">II", ROM_HEADER, ROM_BASE + 2)

BANK_OVERHEAD = 16 + 24  # romtool's extension ROM header and footer

# The window every ROM module has to land in. Above BAND_HI the Emu68 module window
# does not exist yet; below BAND_LO the boot menu has already listed the volumes. The
# header of romstartup/usbromstart.c has the reasoning.
BAND_HI = -41  # first free slot under romboot (-40)
BAND_LO = -49  # last free slot above bootmenu (-50)

# The priorities everything here depends on, checked against the real ROM rather than
# assumed. romboot (-40) is the one that matters most: it walks the ConfigDev chain,
# finds the romtag in the Emu68 board's diag area and SetCurrentBinding+InitResident's
# it, which is where the Emu68 module window (devicetree.resource, gic400.library,
# mailbox.resource, 68040.library) actually comes up. "diag init" (105) only relocates
# diag areas - it initialises nothing - so the whole ROM set has to sit *below* -40.
# bootmenu (-50) closes the window: the volumes we mount have to be listed by then.
KICK_ANCHORS = {
    "expansion.library": 110,
    "diag init": 105,
    "FileSystem.resource": 80,
    "timer.device": 50,
    "input.device": 40,
    "romboot": -40,
    "bootmenu": -50,
    "strap": -60,
}

ROM_CLASSES = ("hubss", "hub", "massstorage", "bootmouse", "bootkeyboard")


def die(msg):
    sys.exit(f"build-kickstart: {msg}")


def rl(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def wl(buf, off, val):
    struct.pack_into(">I", buf, off, val)


# --- the Kickstart's scan table --------------------------------------------------------
# Exec only looks for romtags where its scanBounds table tells it to. The table is a
# list of (lower, upper) pairs of longs closed by a long of -1, and a stock ROM has
# three:
#
#     $F80000..end of exec    exec on its own
#     $F80000..$1000000       the whole Kickstart, exec included
#     $F00000..$F80000        not mapped in a 2 MB image
#
# Neither bank is in it, so modules linked there would simply never be found. But the
# first range finds nothing the second does not, and the third finds nothing at all, so
# the table is rewritten in place:
#
#     $F80000..$1000000       the Kickstart
#     <first bank>
#     <second bank>
#
# Same size, same address, nothing re-pointed. It could not be grown anyway: the
# "chip memory" string follows it directly.
#
# The Kickstart stays first so that it is scanned first. Among residents of equal
# priority exec initialises in scan order, and a module in a bank should come after the
# Kickstart's own whichever bank it landed in.
KICK_RANGE = (ROM_BASE, ROM_END)
F0_RANGE = (0x00F00000, ROM_BASE)
TABLE_END = 0xFFFFFFFF
SEARCH_LIMIT = 0x600  # the table lives just past the coldstart code; 0x3f8 in 47.115


def read_table(kick, off):
    """The three ranges at file offset `off`, and the long that follows them."""
    longs = struct.unpack_from(">7I", kick, off)
    return [longs[0:2], longs[2:4], longs[4:6]], longs[6]


def write_table(kick, off, ranges):
    for i, (lower, upper) in enumerate(ranges):
        wl(kick, off + 8 * i, lower)
        wl(kick, off + 8 * i + 4, upper)


def is_stock_table(kick, off):
    """True if what is at file offset `off` reads as a stock scanBounds table."""
    (exec_range, kick_range, f0_range), end = read_table(kick, off)
    exec_lower, exec_upper = exec_range
    return (exec_lower == ROM_BASE and ROM_BASE < exec_upper <= ROM_END
            and kick_range == KICK_RANGE
            and f0_range == F0_RANGE
            and end == TABLE_END)


def exec_points_at(kick, off):
    """True if exec holds a pointer to file offset `off`.

    The exec range is only recognised by its shape, so this is the proof that the
    table starts with it. If exec's pointer named the Kickstart range instead, the
    eight bytes in front would be something else - and the rewritten table would be
    read from its second range on: both banks, and no Kickstart.
    """
    exec_upper = rl(kick, off + 4)
    exec_code = kick[:exec_upper - ROM_BASE]
    return struct.pack(">I", ROM_BASE + off) in exec_code


def find_table(kick):
    """File offset of the stock scanBounds table, or None."""
    for off in range(0, SEARCH_LIMIT, 2):
        if is_stock_table(kick, off) and exec_points_at(kick, off):
            return off
    return None


def checksum(kick):
    """Sum of all longs with end-around carry. $FFFFFFFF for a valid Kickstart."""
    total = 0
    for (long,) in struct.iter_unpack(">I", kick):
        total += long
        if total > 0xFFFFFFFF:
            total = (total & 0xFFFFFFFF) + 1
    return total


def show(ranges):
    return "  ".join(f"${lower:06X}..${upper:06X}" for lower, upper in ranges)


def patch_kickstart(stock, banks):
    """The Kickstart with its scan table rewritten to cover `banks`, as bytes."""
    kick = bytearray(stock)
    off = find_table(kick)
    if off is None:
        die(f"scanBounds table not found in the Kickstart. Expected the ranges "
            f"$F80000..<end of exec>, {show([KICK_RANGE, F0_RANGE])} within the first "
            f"{SEARCH_LIMIT:#x} bytes, and exec pointing at them; this ROM is either "
            f"already patched - start from the stock Kickstart - or not a layout we know.")

    stock_ranges, _ = read_table(kick, off)
    patched_ranges = [KICK_RANGE] + [(bank.base, bank.base + bank.size) for bank in banks]
    print(f"   scanBounds  ${ROM_BASE + off:06X}")
    print(f"   stock       {show(stock_ranges)}")
    print(f"   patched     {show(patched_ranges)}")
    write_table(kick, off, patched_ranges)

    # The checksum long lives 0x18 from the end and counts as zero while it is computed.
    wl(kick, CHUNK - 0x18, 0)
    wl(kick, CHUNK - 0x18, 0xFFFFFFFF - checksum(kick))
    if checksum(kick) != 0xFFFFFFFF:
        die("internal error: the patched Kickstart's checksum does not come out")
    return bytes(kick)


# --- the startup resident's host controller --------------------------------------------
# romstartup/usbromstart.c carries the HCD name in a fixed-size field tagged with its
# own cookie, so an image can embed a controller other than the one the distribution
# was compiled for. Both values must match ROMSTART_HCD_COOKIE and
# sizeof(romstartHcd.name) there. The trailing \x01 is the layout version: bump it there
# and here together if the field ever moves or changes size, so an old script cannot
# silently mis-patch a new module.
HCD_COOKIE = b"NEPHCD\x01\x00"
HCD_NAME_LEN = 32


def patch_hcd(romstart, name):
    """The startup resident with `name` as its host controller, as bytes."""
    try:
        encoded = name.encode("latin-1")
    except UnicodeEncodeError:
        die(f"--hcd: {name!r} has characters an Amiga device name cannot hold")
    if not encoded:
        die("--hcd: the device name is empty")
    if len(encoded) + 1 > HCD_NAME_LEN:
        die(f"--hcd: {name!r} is {len(encoded)} bytes; the slot holds "
            f"{HCD_NAME_LEN - 1} plus a NUL")
    if romstart.count(HCD_COOKIE) != 1:
        die(f"--hcd: cookie {HCD_COOKIE!r} not found exactly once in the ROM startup "
            f"resident - was it built from a source carrying the patchable slot?")

    # Whole field, NUL-padded: never leave a tail of the previous name behind.
    field_off = romstart.index(HCD_COOKIE) + len(HCD_COOKIE)
    patched = bytearray(romstart)
    patched[field_off:field_off + HCD_NAME_LEN] = encoded.ljust(HCD_NAME_LEN, b"\0")
    print(f"   patched HCD name -> {name} (slot at {field_off:#x})")
    return bytes(patched)


# --- residents -------------------------------------------------------------------------
NODE_TYPES = {0: "NT_UNKNOWN", 1: "NT_TASK", 3: "NT_DEVICE", 8: "NT_RESOURCE", 9: "NT_LIBRARY"}


@dataclass
class Resident:
    pri: int
    node_type: int
    name: str
    id_string: str


def scan_residents(image, base):
    """The residents exec finds in `image` mapped at `base`, in scan order.

    A romtag is the word $4AFC followed by its own address, and like exec this resumes
    at the tag's RT_ENDSKIP, so a tag inside another module's extent does not count.
    """
    def string_at(addr):
        off = addr - base
        if not 0 <= off < len(image):
            return "?"
        return image[off:image.find(b"\0", off)].decode("latin-1").strip()

    residents = []
    off = 0
    while off + 26 <= len(image):
        if image[off:off + 2] != b"\x4a\xfc" or rl(image, off + 2) != base + off:
            off += 2
            continue
        end_skip, _flags, _version, node_type, pri, name, id_string = \
            struct.unpack_from(">IBBBbII", image, off + 6)
        residents.append(Resident(pri, node_type, string_at(name), string_at(id_string)))
        off = max(end_skip - base, off + 2)
    return residents


# --- the banks -------------------------------------------------------------------------
@dataclass
class Bank:
    base: int
    size: int
    modules: list = field(default_factory=list)
    used: int = BANK_OVERHEAD

    @property
    def free(self):
        return self.size - self.used

    def take(self, module):
        """Add `module` if it fits. The file size stands in for the size in ROM: it
        errs high, by the hunk headers and relocation tables romtool strips, so a bank
        planned this way is never over-full when it is built."""
        need = (module.stat().st_size + 3) & ~3  # romtool pads every module to a long
        if need > self.free:
            return False
        self.modules.append(module)
        self.used += need
        return True

    def build(self, romtool, tmp):
        """The bank as an extension ROM, as bytes."""
        index = tmp / f"bank-{self.base:06x}.txt"  # .txt: romtool reads it as a module list
        rom = index.with_suffix(".rom")
        index.write_text("".join(f"{module.resolve()}\n" for module in self.modules))
        result = subprocess.run(
            [romtool, "build", "-o", rom, "-t", "ext", "-s", str(self.size // 1024),
             "-e", f"{self.base:x}", "-f", index],
            capture_output=True, text=True)
        if result.returncode != 0:
            die(f"romtool could not build the ${self.base:06X} bank:\n{result.stderr.strip()}")
        return rom.read_bytes()


def neptune_modules(build_dir):
    """Where the Neptune ROM modules are: (library and classes, startup resident,
    output directory, what to do if one is missing).

    The resident sitting next to this script is what tells the two layouts apart: the
    archive ships it in ROM/, the source tree only ever has it in the build tree.
    """
    here = Path(__file__).resolve().parent
    if (here / "usbromstart").is_file():
        kit = here.parent
        library = kit / "LIBS" / "neptune.library"
        classes = [kit / "DEVS" / "USBClasses" / f"{c}.usbclass" for c in ROM_CLASSES]
        romstart = here / "usbromstart"
        out_dir = Path.cwd()
        hint = "the archive is incomplete; unpack Neptune-<ver>-<cpu>.lha again"
    else:
        build = Path(build_dir) if build_dir else here.parent / "build"
        library = build / "neptune.library" / "neptune.library"
        classes = [build / "classes" / c / f"{c}.usbclass" for c in ROM_CLASSES]
        romstart = build / "romstartup" / "usbromstart"
        out_dir = build
        hint = f"Neptune modules come from {build} - run ./build.sh --build"

    return [library, *classes], romstart, out_dir, hint


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kick", type=Path, metavar="kick.rom",
                        help="stock 512 KiB Kickstart")
    parser.add_argument("extra", type=Path, nargs="*", metavar="module",
                        help="extra ROM-able binaries to embed, e.g. your bcmpcie.library, "
                             "xhci.device and nvme.device")
    parser.add_argument("--hcd", metavar="NAME",
                        help="host controller the startup resident should add, e.g. "
                             "--hcd myhci.device. Patches the name into a copy of the "
                             "resident, so a different controller needs no rebuild of the "
                             "distribution; without it the compiled-in default (cmake "
                             "-DROMSTART_HCD) is used. It must name a real host "
                             "controller that is in the image: the resident opens it "
                             "before DOS exists and waits on it with no timeout, so a "
                             "name that opens something which is *not* a host controller "
                             "hangs the boot rather than reporting anything.")
    parser.add_argument("-o", "--output", type=Path, metavar="FILE",
                        help="output image (default ./kick-usb-2m.rom from the archive, "
                             "<build dir>/kick-usb-2m.rom from a source tree)")
    parser.add_argument("--build-dir", metavar="DIR",
                        help="source tree only: build tree to take the Neptune modules "
                             "from (default <repo>/build)")
    return parser.parse_args()


def main():
    args = parse_args()
    romtool = shutil.which("romtool")
    if romtool is None:
        die("romtool not found. Install amitools: pipx install amitools")

    neptune, romstart, out_dir, missing_hint = neptune_modules(args.build_dir)
    # In descending romtag priority, so the list at the end reads like the init
    # sequence. Priorities live in the romtags, not in this order.
    #   -44 neptune.library   -45 the classes   -46 ROM startup
    modules = [*neptune, romstart, *args.extra]
    out = args.output or out_dir / "kick-usb-2m.rom"

    megabyte = Bank(0xA80000, 2 * CHUNK)  # Emu68 chunks 1+2
    chunk0 = Bank(0xE00000, CHUNK)
    banks = [megabyte, chunk0]            # the order they are filled and exec scans them

    # --- 0. patch the Kickstart ------------------------------------------------------
    # First, because it doubles as the validation of the input.
    if not args.kick.is_file():
        die(f"no such Kickstart: {args.kick}")
    stock_kick = args.kick.read_bytes()
    if len(stock_kick) != CHUNK:
        die(f"{args.kick} is {len(stock_kick)} bytes, expected exactly {CHUNK} (512 KiB)")
    if rl(stock_kick, 0) != ROM_HEADER:
        die(f"{args.kick} does not start with {ROM_HEADER:08x} - not a 512 KiB Kickstart image")
    print("== Patching Kickstart to scan the module banks")
    kick = patch_kickstart(stock_kick, banks)

    for module in modules:
        if not module.is_file():
            die(f"missing module: {module}\n"
                f"   ({missing_hint}; extra modules are the paths you passed.)")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        # --- 1. point the startup resident at a different host controller ------------
        # Patched into a copy, so the shipped resident stays what it was and a later
        # run without --hcd still produces the stock image.
        if args.hcd is not None:
            # The resident opens this device pre-DOS and waits on it with no timeout, so
            # a name that resolves to something which is not a host controller hangs the
            # boot instead of failing. A name that is in the image is not proof it is an
            # HCD, but a name that is *not* is nearly always a typo or a forgotten module.
            if args.hcd not in [module.name for module in modules]:
                print(f"   WARNING: --hcd {args.hcd} names no module being embedded - the resident will\n"
                      f"            look for it at boot and find nothing. Fine only if it comes from your\n"
                      f"            Kickstart or the board ROM; otherwise you forgot to pass the driver.")
            patched_romstart = tmp / romstart.name
            patched_romstart.write_bytes(patch_hcd(romstart.read_bytes(), args.hcd))
            modules[modules.index(romstart)] = patched_romstart

        # --- 2. the modules, and the bank each one goes in ---------------------------
        print("== Modules")
        for module in modules:
            bank = next((bank for bank in banks if bank.take(module)), None)
            if bank is None:
                die(f"no room left for {module.name} ({module.stat().st_size} bytes).\n"
                    f"   Both banks are full - leave a module out.")
            print(f"   {module.name:<28} {module.stat().st_size:>7} bytes   ${bank.base:06x}")

        # --- 3. what is already in the Kickstart -------------------------------------
        print(f"== Kickstart: {args.kick}")
        kick_pri = {resident.name: resident.pri
                    for resident in scan_residents(stock_kick, ROM_BASE)}
        for name, expected in KICK_ANCHORS.items():
            if name not in kick_pri:
                print(f"   WARNING: {name} not found in the Kickstart resident list")
            elif kick_pri[name] != expected:
                print(f"   WARNING: {name} is priority {kick_pri[name]}, expected {expected} "
                      f"- check the ROM module priorities")
            else:
                print(f"   {name:<22} pri {expected:<5} ok")

        # --- 4. build the banks ------------------------------------------------------
        roms = {}
        for bank in banks:
            print(f"== Building bank ({bank.size // 1024} KiB @ ${bank.base:06x})")
            roms[bank.base] = bank.build(romtool, tmp)

    # --- 5. assemble ------------------------------------------------------------------
    # In Emu68's chunk order.
    print(f"== Assembling {out}")
    image = roms[chunk0.base] + roms[megabyte.base] + kick
    if len(image) != 4 * CHUNK:
        die(f"the image comes to {len(image)} bytes, expected {4 * CHUNK} (2 MB)")
    # What Emu68 starts the m68k from. That $F80002 is the Kickstart's JMP was checked
    # with its header.
    if image[:8] != RESET_VECTOR:
        die("the image does not begin with a reset vector into the Kickstart")

    # --- 6. verify and report ---------------------------------------------------------
    residents = [resident for bank in banks
                 for resident in scan_residents(roms[bank.base], bank.base)]
    if len(residents) != len(modules):
        die(f"found {len(residents)} residents in the banks, expected {len(modules)} "
            f"- a module with no romtag?")

    print("== Residents in the banks (exec initialises these high priority first)")
    for r in residents:
        print(f"   {r.pri:<5} {NODE_TYPES.get(r.node_type, r.node_type):<12} {r.name:<24} {r.id_string}")

    # Below the band is always wrong: bootmenu (-50) has already listed the boot volumes
    # by then, so a module that mounts one down there has missed it, and strap is right
    # behind.
    under = [r for r in residents if r.pri < BAND_LO]
    if under:
        die(f"these modules sit below the ROM priority band (under {BAND_LO}):\n"
            + "".join(f"   {r.name:<24} priority {r.pri}\n" for r in under)
            + "   The boot menu (-50) lists the boot volumes before they run, and strap (-60) picks\n"
              "   one straight after.")

    # Above the band is a judgement call the script cannot make. Anything that touches
    # the device tree, PCIe, MSI or the 68040 cache patches has to be under romboot
    # (-40), which is where Emu68's own m68k modules are bound; a module that needs none
    # of that - a filesystem registering itself in FileSystem.resource, say - is
    # perfectly happy up there, and often has to be, so that it exists before anything
    # mounts.
    over = [r for r in residents if r.pri > BAND_HI]
    if over:
        print("   NOTE: these run before romboot (-40) opens the Emu68 module window:")
        for r in over:
            print(f"   {r.name:<24} priority {r.pri}")
        print("         Fine for a filesystem or anything else that only needs the Kickstart's\n"
              "         own resources; fatal for anything wanting devicetree.resource,\n"
              "         gic400.library, mailbox.resource or 68040.library's DMA cache patches.")

    out.write_bytes(image)

    # How much of each bank is spoken for, as it was planned - so by file size, which
    # errs high.
    print()
    for bank in banks:
        print(f"   bank ${bank.base:06x}  ~{bank.used} bytes of {bank.size} ({bank.free // 1024} KiB free)")
    print(f"   image      {out} ({len(image)} bytes)")
    print("Copy it to the SD FAT partition and point the initramfs line in config.txt at it.")
    print("Keep the stock ROM beside it - rollback is a one-line edit. See ROM-ReadMe.md.")


if __name__ == "__main__":
    main()
