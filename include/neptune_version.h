/* neptune_version.h - the $VER cookie builder shared by every component of the
 * distribution (the library, the 29 classes, NeptuneUSB and the CLI tools).
 *
 * The numbers live in exactly one place: project(VERSION) in the top-level
 * CMakeLists.txt, which injects NEPTUNE_VERSION / NEPTUNE_REVISION / NEPTUNE_DATE /
 * NEPTUNE_DIST_NAME / NEPTUNE_CPU / NEPTUNE_BUILD_TAIL globally. Nothing here
 * hardcodes a version.
 *
 * NEP_VER("hub.usbclass")  ->  "$VER: hub.usbclass <ver> (<date>) Neptune 68040"
 *
 * A local (build.sh) build appends its stamp, " dev-<YYYYMMDD>-<HHMMSS>-g<hash>[-dirty]",
 * so every binary on the Amiga says which build produced it; NEPTUNE_BUILD_TAIL is ""
 * in a release.
 *
 * The date stays inside the parentheses the AmigaDOS `Version` command parses; the
 * distribution name follows it, the way the classic tool cookies carry their author.
 *
 * The CPU tail is the one thing that distinguishes the release archives from each other:
 * they all carry the same version numbers (those are the library/romtag ABI), so a
 * `Version LIBS:neptune.library` on the running machine would otherwise not say which
 * of the 68020/68040/68060 builds is installed.
 *
 * _NEP_STR is the two-level "stringify a macro's value" idiom: # does not expand its
 * operand, so _NEP_STR(NEPTUNE_VERSION) expands NEPTUNE_VERSION (e.g. 6) before
 * _NEP_STR2 stringifies it to "6" (vs _NEP_STR2(NEPTUNE_VERSION) -> "NEPTUNE_VERSION").
 */
#ifndef NEPTUNE_VERSION_H
#define NEPTUNE_VERSION_H

#if !defined(NEPTUNE_VERSION) || !defined(NEPTUNE_BUILD_TAIL)
#error "NEPTUNE_VERSION/REVISION/DATE/DIST_NAME/CPU/BUILD_TAIL come from the top-level CMakeLists.txt"
#endif

#define _NEP_STR2(x) #x
#define _NEP_STR(x)  _NEP_STR2(x)

#define NEP_VER(name) \
    "$VER: " name " " _NEP_STR(NEPTUNE_VERSION) "." _NEP_STR(NEPTUNE_REVISION) \
    " (" NEPTUNE_DATE ") " NEPTUNE_DIST_NAME " " NEPTUNE_CPU NEPTUNE_BUILD_TAIL

/* Short display form for window titles and the like: NEP_NAME_VER("Neptune USB") ->
 * "Neptune USB <ver>". Use this instead of putting a version in a locale catalog - a
 * translated version number can only ever drift. */
#define NEP_NAME_VER(name) \
    name " " _NEP_STR(NEPTUNE_VERSION) "." _NEP_STR(NEPTUNE_REVISION)

/* Fork statement shown in the About boxes. */
#define NEP_FORK_TXT "Part of Neptune, an incompatible fork\nof the Poseidon USB stack."

#endif /* NEPTUNE_VERSION_H */
