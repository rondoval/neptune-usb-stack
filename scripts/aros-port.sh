#!/usr/bin/env bash
# Stage one upstream AROS commit onto this tree - three-way, without committing.
#
# This tree was extracted from AROS rom/usb with rewritten history, so an upstream
# commit cannot be cherry-picked: it is not in this repository and its paths differ.
# This script does the equivalent across repositories.  It takes the commit's changes
# from an AROS clone, maps the paths, and applies them with a three-way merge onto our
# (de-AROS'd) files.  The result is left STAGED; nothing is committed.  The proposed
# commit message and the `git commit` line carrying the upstream author and date are
# written out for whoever commits.  CONTRIBUTING.md ("Tracking AROS upstream") has the
# conventions this implements.
#
# Usage:
#   scripts/aros-port.sh [--partial] <aros-clone> <sha> [<upstream-path> ...]
#
#   <aros-clone>     a clone of https://github.com/aros-development-team/AROS (a
#                    blobless/shallow one is fine as long as it holds <sha> and its parent)
#   <sha>            the upstream commit
#   <upstream-path>  restrict the port to these upstream paths (e.g.
#                    rom/usb/classes/hub); default is everything under rom/usb
#   --partial        mark the trailer "(partial)": only part of the upstream commit
#                    is being taken in this commit
#
# Exit status: 0 = applied cleanly, 1 = applied with conflicts or failures (conflict
# markers in the tree; resolve and `git add`), 2 = nothing applied / usage error.
set -euo pipefail

die() { echo "aros-port: $*" >&2; exit 2; }

# Single-quote a string for pasting into a shell.
quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }

partial=""
if [ "${1:-}" = "--partial" ]; then
    partial=" (partial)"
    shift
fi
[ $# -ge 2 ] || die "usage: $0 [--partial] <aros-clone> <sha> [<upstream-path> ...]"

aros=$1
rev=$2
shift 2
[ $# -gt 0 ] || set -- rom/usb

cd "$(git rev-parse --show-toplevel)"

git -C "$aros" rev-parse --git-dir >/dev/null 2>&1 || die "$aros is not a git repository"
sha=$(git -C "$aros" rev-parse --verify --quiet "$rev^{commit}") || die "no commit $rev in $aros"
[ "$(git -C "$aros" rev-list --parents -n 1 "$sha" | wc -w)" -le 2 ] || die "$rev is a merge commit"
parent=$(git -C "$aros" rev-parse --verify --quiet "$sha^") ||
    die "$rev has no parent in $aros (shallow clone boundary?) - deepen the clone"

# A port is staged on its own, so the staged diff is exactly what gets committed.
git diff --cached --quiet || die "the index already has staged changes; commit or unstage them first"

# Upstream directory -> our directory.  Most specific first: a file is handled by the
# first entry it falls under.
map=(
    "rom/usb/poseidon/shellcommands  c"
    "rom/usb/poseidon                neptune.library"
    "rom/usb/shellapps               tools"
    "rom/usb/classes                 classes"
    "rom/usb/trident                 neptuneusb"
)

# Upstream basename -> ours, for files renamed here (applied after the directory map).
renamed=(
    "poseidon.library.c  neptune.library.c"
    "poseidon.library.h  neptune.library.h"
    "poseidon_intern.h   neptune_intern.h"
    "popo.gui.c          notify.gui.c"
    "popo.gui.h          notify.gui.h"
    "Trident.c           NeptuneUSB.c"
    "Trident.h           NeptuneUSB.h"
    "PsdStackLoader.c    USBStart.c"
    "PsdDevLister.c      USBList.c"
    "PsdErrorlog.c       USBErrorlog.c"
    "AddUSBHardware.c    USBAddHardware.c"
    "AddUSBClasses.c     USBAddClasses.c"
    "DRadioTool.c        USBRadio.c"
    "DRadioTool.h        USBRadio.h"
    "PencamTool.c        USBPencam.c"
    "PencamTool.h        USBPencam.h"
    "PowManTool.c        USBPowMan.c"
    "PowManTool.h        USBPowMan.h"
    "RocketTool.c        USBRocket.c"
    "RocketTool.h        USBRocket.h"
    "SonixcamTool.c      USBSonixcam.c"
    "SonixcamTool.h      USBSonixcam.h"
    "UPSTool.c           USBUPS.c"
    "UPSTool.h           USBUPS.h"
)

# Our path for an upstream file, or nothing if we have no place for it.
ours_for() {
    local entry prefix dir path base from to
    for entry in "${map[@]}"; do
        read -r prefix dir <<<"$entry"
        case "$1" in
            "$prefix"/*) path="$dir/${1#"$prefix"/}"; break ;;
        esac
    done
    [ -n "${path:-}" ] || return 0
    base=${path##*/}
    for entry in "${renamed[@]}"; do
        read -r from to <<<"$entry"
        [ "$base" = "$from" ] && { path="${path%/*}/$to"; break; }
    done
    echo "$path"
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
: > "$tmp/build" ; : > "$tmp/unmapped" ; : > "$tmp/absent" ; : > "$tmp/failed"

applied=0

# One file at a time: `git apply` is all-or-nothing per patch, and a commit that also
# touches a file we no longer have must not take the rest down with it.  A plain
# parent..commit diff, NOT `format-patch -1 -- <path>` - with a pathspec that silently
# picks the last commit that touched the path instead of this one.
while IFS=$'\t' read -r status file; do
    case "$file" in
        *.conf|*mmakefile.src|rom/usb/trident/catalogs*)   # AROS build files; a submodule upstream
            echo "$file" >> "$tmp/build"; continue ;;
    esac

    target=$(ours_for "$file")
    if [ -z "$target" ]; then
        echo "$file" >> "$tmp/unmapped"; continue
    fi
    if [ "$status" != "A" ] && [ ! -e "$target" ]; then
        echo "$file -> $target" >> "$tmp/absent"; continue
    fi

    patch="$tmp/patch"
    git -C "$aros" diff --full-index --no-renames "$parent" "$sha" -- "$file" > "$patch"
    # A file renamed here: point the patch headers at our basename.
    if [ "${file##*/}" != "${target##*/}" ]; then
        sed -i -E "/^(diff --git|---|\+\+\+) /s#/${file##*/}( |\$)#/${target##*/}\1#g" "$patch"
    fi

    # The three-way merge needs the pre-image blob.  Import it if we lack it; nothing
    # else of AROS's history enters this repository.
    blob=$(sed -n -E 's/^index ([0-9a-f]{40})\.\.[0-9a-f]{40}.*/\1/p' "$patch" | head -n 1)
    if [ -n "$blob" ] && [ "$blob" != 0000000000000000000000000000000000000000 ] &&
       ! git cat-file -e "$blob" 2>/dev/null; then
        got=$(git -C "$aros" cat-file blob "$blob" | git hash-object -w --stdin)
        [ "$got" = "$blob" ] || die "could not import pre-image blob $blob"
    fi

    # Re-root the upstream path at ours: strip a/ and every directory, then prepend
    # our directory.  (A renamed basename was rewritten in the headers above.)
    strip=$(( $(printf '%s' "$file" | tr -cd / | wc -c) + 1 ))
    args=(--3way --index "-p$strip" "--directory=$(dirname "$target")")

    if git apply "${args[@]}" "$patch" 2>"$tmp/err"; then
        applied=$((applied + 1))
    elif [ -n "$(git diff --name-only --diff-filter=U -- "$target")" ]; then
        applied=$((applied + 1))        # --3way left conflict markers: resolve by hand
    else
        echo "$file -> $target: $(tail -n 1 "$tmp/err")" >> "$tmp/failed"
    fi
done < <(git -C "$aros" diff --name-status --no-renames "$parent" "$sha" -- "$@")

if [ "$applied" -eq 0 ]; then
    # Say why before giving up: a renamed file is a port by hand, not a no-op.
    cat "$tmp/failed" "$tmp/absent" >&2
    die "nothing in ${sha:0:8} applies to this tree for: $*"
fi

# The proposed message: upstream's, plus the trailer that records where it came from.
# It goes in the repository root (git-ignored), where it is easy to open and edit.
msg=AROS_PORT_MSG
git -C "$aros" log -1 --format=%B "$sha" |
    git interpret-trailers --trailer "AROS-commit: $sha$partial" > "$msg"

author=$(git -C "$aros" log -1 --format='%an <%ae>' "$sha")
date=$(git -C "$aros" log -1 --format=%aI "$sha")
git diff --name-only --diff-filter=U > "$tmp/conflicts"
git diff --cached --name-only --diff-filter=AMD > "$tmp/staged"

list() {    # <heading> <file>: print the heading and the indented list, if any
    [ -s "$2" ] || return 0
    echo
    echo "$1"
    sed 's/^/  /' "$2"
}

echo "Upstream ${sha:0:8}: $(git -C "$aros" log -1 --format=%s "$sha")"
list "Staged:" "$tmp/staged"
list "CONFLICTS - resolve, then git add:" "$tmp/conflicts"
list "FAILED to apply (port by hand):" "$tmp/failed"
list "Not in this tree (renamed or removed here - port by hand if it matters):" "$tmp/absent"
list "No place in this tree (host-controller drivers etc.):" "$tmp/unmapped"
list "AROS build files, skipped:" "$tmp/build"

echo
echo "Proposed message: $msg"
echo "Commit with:"
echo "  git commit --author=$(quote "$author") --date=$(quote "$date") -F $(quote "$msg")"

[ ! -s "$tmp/conflicts" ] && [ ! -s "$tmp/failed" ]
