#!/usr/bin/env bash
#
# build.sh - build Neptune (in the shared GHCR toolchain container),
# package it as an installable .lha, and/or push the built binaries to a live Amiga
# over Cloanto Amiga Explorer (AE.exe).
#
# Pick any combination of --build / --package / --upload; with none of them given it
# does --build --upload (the usual edit-build-test loop).
#
# The upload pushes the freshly built binaries into the same locations the Installer
# uses; it does NOT install datatypes/icons/catalogs or touch S:User-Startup. For a
# first-time/full install use the .lha (--package) + Installer.
#
# Prerequisites
#   * --build / --package: docker (the toolchain image is public and auto-pulled).
#   * --upload: Amiga Explorer running on the Amiga (serial or TCP) with the matching
#     connection configured on the Windows side (this script drives AE.exe via WSL).
#
# Usage
#   ./build.sh [--build] [--package] [--upload] [--tools] [--all-cpus] [--dry-run]
#     --build     build the stack in the toolchain container (debug backend/level below)
#     --package   build, then create <build dir>/Neptune-<ver>-<cpu>[-<backend>]-<stamp>.lha
#     --upload    push the built binaries to the Amiga: the library, the classes, the
#                 commands, NeptuneUSB, USBEject and the VHI driver (to LIBS:VHI/)
#     (none of --build/--package/--upload => --build --upload)
#     --tools     also upload the optional per-gadget tools to SYS:Tools/, and the test
#                 tools that no archive has (USBVideoTest)
#     --all-cpus  build/package every released CPU (68020, 68040, 68060) in turn, each
#                 in its own build dir; cannot be combined with --upload
#     --dry-run   upload: show what would be copied; copy nothing
#     -h, --help  this help
#
# Env overrides:
#     BUILD_IMAGE=<image>  override the toolchain container (default lives in scripts/docker-build.sh)
#     AE=<path to AE.exe>
#     BUILD_DIR=<path>     build dir, must live under the repo (default <repo>/build for the
#                          default CPU, <repo>/build-<cpu tag> for any other)
#     BACKEND=<pistorm|serial|off>  debug sink (default pistorm; releases ship off)
#     DEBUG=<level>                 min KPRINTF level (default 1 = verbose; higher = quieter)
#     CPU=<68020|68040|68060>       target CPU (default 68040)
#     FPU=<soft|hard>               FPU code generation (default: soft for 68020, hard otherwise)
#     LTO=<on|off>                  link-time optimization (default on); off is for bisecting a
#                                   suspected miscompile, not for releases
#     NEPTUNE_CONFIGURE_ARGS=...   extra -D flags, appended last so they win
#
# Build stamp: every build made here is a local build and is marked as one. Each run
# computes one stamp, dev-<YYYYMMDD>-<HHMMSS>-g<hash>[-dirty] (-dirty = uncommitted
# changes to tracked files), and passes it as NEPTUNE_BUILD_STAMP; it ends up at the
# tail of every binary's $VER (`Version <file> full` shows it), in the .lha name and in
# the packaged ReadMe. Releases are built only by the tag-triggered CI workflow
# (.github/workflows/release.yml), which never sets it.
#
set -euo pipefail

# --- config ------------------------------------------------------------------
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_IMAGE="${BUILD_IMAGE:-}"             # empty => scripts/docker-build.sh owns the default tag
AE="${AE:-/mnt/c/Program Files/Cloanto/Amiga Explorer/Windows/AE.exe}"
DEBUG_LEVEL="${DEBUG:-1}"
DEBUG_BACKEND="${BACKEND:-pistorm}"
LTO="${LTO:-on}"
# The caller's extra -D flags, captured once: build_one exports NEPTUNE_CONFIGURE_ARGS, so
# reading it back there would carry one CPU's flags into the next --all-cpus pass.
USER_CONFIGURE_ARGS="${NEPTUNE_CONFIGURE_ARGS:-}"

# The local-build stamp (see the header). One per run, so an --all-cpus run and the
# separate package configure all carry the same one. Untracked files do not make a tree
# dirty; no git => date and time only.
build_stamp() {
    local stamp hash
    stamp="dev-$(date +%Y%m%d-%H%M%S)"
    if hash="$(git -C "$ROOT" rev-parse --short=8 HEAD 2>/dev/null)"; then
        stamp+="-g$hash"
        if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ]]; then
            stamp+="-dirty"
        fi
    fi
    printf '%s\n' "$stamp"
}
BUILD_STAMP="$(build_stamp)"

# The released CPU variants, and the default.
# The FPU pairing is not a free choice: 68020 machines usually have no FPU and the
# soft-float 020 runtime is the one that matches (libm020 without libm881), while 68040
# and 68060 pair with the hard-float one. Nothing in the stack computes in floating point
# anyway - only the optional USBPencam/USBSonixcam gamma table does.
RELEASE_CPUS=(68020 68040 68060)
DEFAULT_CPU=68040
CPU="${CPU:-$DEFAULT_CPU}"

DO_BUILD=0 DO_PACKAGE=0 DO_UPLOAD=0 DO_TOOLS=0 DRY=0 ALL_CPUS=0 EXPLICIT=0
for a in "$@"; do
    case "$a" in
        --build)    DO_BUILD=1;   EXPLICIT=1 ;;
        --package)  DO_PACKAGE=1; EXPLICIT=1 ;;
        --upload)   DO_UPLOAD=1;  EXPLICIT=1 ;;
        --tools)    DO_TOOLS=1 ;;
        --all-cpus) ALL_CPUS=1 ;;
        --dry-run)  DRY=1 ;;
        -h|--help) sed -n '2,/^[^#]/{/^#/p}' "$0"; exit 0 ;;   # the leading comment block
        *) echo "unknown option: $a (try --help)" >&2; exit 2 ;;
    esac
done
# No action chosen => the usual build + upload loop.
if (( ! EXPLICIT )); then DO_BUILD=1; DO_UPLOAD=1; fi
# Packaging stages from a complete build, so it implies a build.
if (( DO_PACKAGE )); then DO_BUILD=1; fi

# --all-cpus produces several build trees; there is no single one to upload from.
if (( ALL_CPUS && DO_UPLOAD )); then
    echo "--all-cpus builds every CPU variant - pick one with CPU=<cpu> to upload." >&2
    exit 2
fi

# CPU/FPU pair -> the toolchain's M68K_CPU/M68K_FPU cache variables, and the default build
# dir. The default CPU keeps the plain build/ tree (the edit-build-test loop is unchanged);
# any other CPU gets its own, because a configured CMake cache is tied to one CPU and
# reusing it across variants would silently package the wrong binaries.
cpu_setup() {                              # cpu_setup <cpu> ; sets CPU_TAG, CPU_FPU, BUILD_DIR
    CPU="$1"
    case "$CPU" in
        68020|68040|68060) ;;
        *) echo "unsupported CPU: $CPU (want one of: ${RELEASE_CPUS[*]})" >&2; exit 2 ;;
    esac
    CPU_TAG="${CPU#68}"
    if [[ "$CPU" == 68020 ]]; then CPU_FPU="${FPU:-soft}"; else CPU_FPU="${FPU:-hard}"; fi
    if [[ -n "${BUILD_DIR_EXPLICIT:-}" ]]; then
        BUILD_DIR="$BUILD_DIR_EXPLICIT"
    elif [[ "$CPU" == "$DEFAULT_CPU" ]]; then
        BUILD_DIR="$ROOT/build"
    else
        BUILD_DIR="$ROOT/build-$CPU_TAG"
    fi
}

# An explicit BUILD_DIR= still wins, but only for a single-CPU run: --all-cpus needs one
# tree per variant.
BUILD_DIR_EXPLICIT="${BUILD_DIR:-}"
if (( ALL_CPUS )) && [[ -n "$BUILD_DIR_EXPLICIT" ]]; then
    echo "BUILD_DIR= cannot be combined with --all-cpus (each CPU needs its own tree)." >&2
    exit 2
fi
cpu_setup "$CPU"

# Deploy table - "<source under build/>|<Amiga destination>".  Mirrors dist/Install:
CORE=(
    "neptune.library/neptune.library|LIBS:neptune.library"
    "c/USBStart|C:USBStart"
    "c/AddUSBHardware|C:AddUSBHardware"
    "c/AddUSBClasses|C:AddUSBClasses"
    "c/USBList|C:USBList"
    "c/USBErrorlog|C:USBErrorlog"
    "neptuneusb/NeptuneUSB|SYS:Prefs/NeptuneUSB"
    "usbeject/USBEject|SYS:WBStartup/USBEject"
    "vhi/usbvideo.vhi|LIBS:VHI/usbvideo.vhi"
)
# Optional per-gadget tools
GADGET_TOOLS=(
    "tools/USBRadio|SYS:Tools/USBRadio"
    "tools/USBPencam|SYS:Tools/USBPencam"
    "tools/USBPowMan|SYS:Tools/USBPowMan"
    "tools/USBRocket|SYS:Tools/USBRocket"
    "tools/USBSonixcam|SYS:Tools/USBSonixcam"
    "tools/USBUPS|SYS:Tools/USBUPS"
    "tools/USBVideo|SYS:Tools/USBVideo"
)
# Test tools: built and uploaded with the gadget tools, in no archive
TEST_TOOLS=(
    "tools/USBVideoTest|SYS:Tools/USBVideoTest"
)

# --- helpers -----------------------------------------------------------------
ae() { "$AE" "$@"; }                       # AE.exe always exits 0 - parse its output, not $?

# Create an Amiga dir if missing (MakeDir errors harmlessly when it already exists).
ensure_dir() { ae MakeDir "$1" >/dev/null 2>&1 || true; }

ok=0 fail=0
copy_one() {                               # copy_one <src-under-build> <amiga-dest>
    local src="$BUILD_DIR/$1" dest="$2" win out
    if [[ ! -f "$src" ]]; then
        printf '  \e[31mMISS\e[0m %-42s (not built - run --build?)\n' "$1" >&2
        fail=$((fail + 1)); return
    fi
    if (( DRY )); then
        printf '  would copy %-42s -> %s\n' "$1" "$dest"; return
    fi
    win="$(wslpath -w "$src")"
    out="$(ae Copy "$win" "$dest" /Y 2>&1 || true)"
    if printf '%s' "$out" | grep -qiE 'error|cannot|fail' || ! printf '%s' "$out" | grep -q '100%'; then
        printf '  \e[31mFAIL\e[0m %-42s -> %s\n' "$1" "$dest" >&2
        printf '       %s\n' "$(printf '%s' "$out" | tr -d '\r' | grep -iE 'error|cannot' | head -1)" >&2
        fail=$((fail + 1))
    else
        printf '  \e[32m OK \e[0m %-42s -> %s\n' "$1" "$dest"
        ok=$((ok + 1))
    fi
}

deploy_group() { local e; for e in "$@"; do copy_one "${e%%|*}" "${e#*|}"; done; }

# Every built class under build/classes/*/*.class -> DEVS:USBClasses/<file> (output names
# differ from dir names - pegasus.class, dm9601eth.class, usbaudio.class, ... - so glob the
# actual artifacts rather than the source-dir names).
deploy_classes() {
    local f rel; local -a list
    mapfile -t list < <(find "$BUILD_DIR/classes" -name '*.class' -type f 2>/dev/null | sort)
    if (( ${#list[@]} == 0 )); then
        printf '  \e[31mMISS\e[0m %-42s (no classes built - run --build?)\n' "classes/*/*.class" >&2
        fail=$((fail + 1)); return
    fi
    for f in "${list[@]}"; do
        rel="${f#"$BUILD_DIR"/}"
        copy_one "$rel" "DEVS:USBClasses/$(basename "$f")"
    done
}

# --- build / package (delegated to the CI-coupled container wrapper) ----------
# One CPU variant, into the build dir cpu_setup() picked for it.
build_one() {
    # scripts/docker-build.sh owns the docker invocation, image tag and configure
    # incantation (toolchain file, MUI/SANA SDKs); we just feed it the variant knobs.
    local build_rel
    case "$BUILD_DIR" in
        "$ROOT"/*) build_rel="${BUILD_DIR#"$ROOT"/}" ;;
        *) echo "BUILD_DIR must live under the repo ($ROOT) for the container build." >&2; exit 1 ;;
    esac
    local lto_arg; case "${LTO,,}" in
        on|yes|1)  lto_arg="-DNEPTUNE_LTO=ON" ;;
        off|no|0)  lto_arg="-DNEPTUNE_LTO=OFF" ;;
        *) echo "LTO must be on or off (got '$LTO')" >&2; exit 2 ;;
    esac
    # The caller's own -D flags go last so they override ours.
    export NEPTUNE_CONFIGURE_ARGS="-DNEPTUNE_DEBUG_BACKEND=$DEBUG_BACKEND -DNEPTUNE_DEBUG_LEVEL=$DEBUG_LEVEL -DM68K_CPU=$CPU -DM68K_FPU=$CPU_FPU $lto_arg -DNEPTUNE_BUILD_STAMP=$BUILD_STAMP $USER_CONFIGURE_ARGS"
    export NEPTUNE_BUILD_DIR="$build_rel"
    [[ -n "$BUILD_IMAGE" ]] && export NEPTUNE_BUILD_IMAGE="$BUILD_IMAGE"

    local what="building"; (( DO_PACKAGE )) && what="building + packaging"
    echo ">> $what via scripts/docker-build.sh (cpu=$CPU, fpu=$CPU_FPU, debug backend=$DEBUG_BACKEND, level=$DEBUG_LEVEL, lto=${LTO,,}, stamp=$BUILD_STAMP) ..."
    "$ROOT/scripts/docker-build.sh"
    if (( DO_PACKAGE )); then
        # package has no build dependency - stage the freshly built tree into the .lha.
        "$ROOT/scripts/docker-build.sh" --target package
        local lha
        lha="$(ls -t "$BUILD_DIR"/Neptune-*.lha 2>/dev/null | head -1 || true)"
        if [[ -z "$lha" ]]; then
            echo "package produced no .lha in $BUILD_DIR" >&2
            return 1
        fi
        echo ">> package: $lha"
        PACKAGES+=("$lha")
    fi
}

PACKAGES=()
if (( DO_BUILD )); then
    if (( ALL_CPUS )); then
        for c in "${RELEASE_CPUS[@]}"; do cpu_setup "$c"; build_one; done
    else
        build_one
    fi
    if (( DO_PACKAGE && ${#PACKAGES[@]} > 1 )); then
        echo; echo ">> packages:"; printf '   %s\n' "${PACKAGES[@]}"
    fi
fi

# --- upload ------------------------------------------------------------------
if (( DO_UPLOAD )); then
    # --dry-run touches nothing, so it does not need AE.exe - it just previews the plan.
    if (( ! DRY )); then
        [[ -x "$AE" || -f "$AE" ]] || { echo "AE.exe not found at: $AE  (set AE=...)" >&2; exit 1; }

        # preflight: is the Amiga reachable? (retry - AE drops the odd request)
        echo ">> checking Amiga Explorer connection ..."
        # NB: do NOT wrap AE.exe in `timeout` - under WSL Win32 interop that severs its
        # connection and it reports no volumes. AE has its own serial/TCP timeout anyway.
        connected=0
        for attempt in 1 2 3; do
            if ae Info 2>/dev/null | grep -q 'Volume:'; then connected=1; break; fi
            sleep 2
        done
        if (( ! connected )); then
            echo "No Amiga connection (AE Info returned no volumes after 3 tries)." >&2
            echo "Start Amiga Explorer on the Amiga and check the Windows-side connection." >&2
            exit 1
        fi
    fi

    echo ">> deploying core to LIBS: / LIBS:VHI / DEVS:USBClasses / C: / SYS:Prefs ..."
    (( DRY )) || ensure_dir "DEVS:USBClasses"
    (( DRY )) || ensure_dir "LIBS:VHI"
    deploy_classes
    deploy_group "${CORE[@]}"

    if (( DO_TOOLS )); then
        echo ">> deploying per-gadget and test tools to SYS:Tools ..."
        (( DRY )) || ensure_dir "SYS:Tools"
        deploy_group "${GADGET_TOOLS[@]}" "${TEST_TOOLS[@]}"
    fi
fi

# --- summary -----------------------------------------------------------------
if (( DO_UPLOAD )); then
    echo
    if (( DRY )); then
        echo "dry run - nothing copied."
    else
        echo "done: $ok copied, $fail failed."
        echo "Note: neptune.library / *.class are loaded into memory at boot - reboot"
        echo "      (or unload the stack and re-run USBStart / AddUSBClasses) to pick up changes."
    fi
fi
(( fail == 0 ))
