# dist/icons - Workbench icon assets

Source art and generators for the `.info` icons (and the settings-file datatype) shipped by
`make package`. The produced `.info` / descriptor files are committed as **static
binary assets** (under `dist/`), so building the stack needs no Python/pypng/
icontool - the files here only matter when *regenerating* them.

## What gets generated

| Output (committed) | From | Kind |
|---|---|---|
| `dist/Install.info` | `installer.png` + `installer.info.src` | project, DefaultTool=`SYS:System/Installer`, ToolTypes `APPNAME` / `MINUSER` / `DEFUSER`, ColorIcon |
| `dist/NeptuneUSB.info` | `NeptuneUSB.png` + `NeptuneUSB.info.src` | tool (Stack 57344), ColorIcon |
| `dist/USBEject.info` | `USBEject.png` + `USBEject.info.src` | tool (Stack 16384, ToolType `DONOTWAIT`), ColorIcon |
| `dist/USBVideo.info` | `USBVideo.png` + `USBVideo.info.src` | tool (Stack 32768), ColorIcon |
| `dist/def_Neptune.info` | `def_Neptune.png` + `def_Neptune.info.src` | project deficon, ColorIcon |
| `dist/datatypes/Neptune` | `../datatypes/Neptune.dtd` | binary DataType descriptor |

All the `.info` files are built by the **same** generator (`make_icons.py`):
each gets a faithful OS3.5 ColorIcon plus a classic planar fallback, with
TYPE/STACK/DEFAULTTOOL/TOOLTYPES/TOOLTYPE taken from its `.info.src`. One
icontool invocation builds each icon - `--create` synthesises the DiskObject,
so no template file is written.

Descriptor keys:

| Key | Meaning |
|---|---|
| `TYPE` | `TOOL` \| `PROJECT` \| ... (default `TOOL`) |
| `STACK` | stack size in bytes (default 4096) |
| `DEFAULTTOOL` | the tool Workbench runs (projects) |
| `TOOLTYPES` | `FLAG[, FLAG...]` - boolean tooltypes |
| `TOOLTYPE` | `KEY=VALUE` - one value tooltype, repeatable |

## Files here

- `installer.png` + `installer.info.src` - installer icon art (a downward
  trident) and its descriptor (project, DefaultTool=`SYS:System/Installer`).
- `NeptuneUSB.png` + `NeptuneUSB.info.src` - Neptune USB program icon (AROS Gorilla USB-plug,
  the Trident icon of that set).
- `USBEject.png` + `USBEject.info.src` - USBEject daemon icon (same Gorilla USB-plug
  art as Neptune USB for now; `DONOTWAIT` so WBStartup does not stall on it).
- `def_Neptune.png` + `def_Neptune.info.src` - settings-file deficon art (the AROS Poseidon
  tree from the Poseidon sources).
- `USBVideo.png` + `USBVideo.info.src` - USBVideo program icon (a webcam, drawn for this
  project).
- `make_icons.py` - builds all five `.info` files from their PNG + `.info.src`.

The datatype source `../datatypes/Neptune.dtd` lives next to its generated binary.

## Requirements (host-side only)

- python3 with **pypng** (icontool reads the PNGs): `pip install pypng` (e.g. in a venv).
- **icontool** with `--create` / `--import-coloricon` / `--set-defaulttool` - from
  [rondoval/icontool](https://github.com/rondoval/icontool), branch `set-defaulttool`
  (adds the ColorIcon writer, DefaultTool set/clear, icon creation from scratch, and
  repeatable tooltype options). Path via `$ICONTOOL`, else `../../../icontool/icontool`.

## Regenerating

```sh
ICONTOOL=/path/to/icontool/icontool python3 make_icons.py   # all icons; name(s) as args = only those
ICONTOOL=/path/to/icontool/icontool python3 make_icons.py USBEject   # e.g. just dist/USBEject.info
```

The datatype descriptor is built from `../datatypes/Neptune.dtd` with AROS's
host tool `tools/dtdesc/createdtdesc` (it emits a correct big-endian Amiga
descriptor even on a little-endian host):

```sh
createdtdesc -o ../datatypes/Neptune ../datatypes/Neptune.dtd
```

## Licensing

`NeptuneUSB.png` and `USBEject.png` are from the AROS **Gorilla** icon set - **GPL** (see
top-level `LEGAL`). `def_Neptune.png` and `Neptune.dtd` derive from the Poseidon sources'
`def_PSD.png` and `PSD.dtd` (AROS Public License). The
installer art (`installer.png`) is original to this project.
