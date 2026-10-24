cmake_minimum_required(VERSION 3.14)
project(neptune VERSION 6.3 LANGUAGES C ASM)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install" CACHE PATH "" FORCE)
endif()

# --- Release identity ------------------------------------------------------------
# The project() version above is the ONE place the shipping version is written: the
# library, all 29 classes, NeptuneUSB and every tool report it, and Packaging names the
# archive after it. These six defines are global (they must reach every TU); the
# $VER cookie itself is assembled by NEP_VER() in include/neptune_version.h.
# The date is the configure timestamp.
#
# NEPTUNE_CPU is the target CPU (M68K_CPU, from cmake/toolchain.cmake) and is the only
# part of the cookie that differs between the released 68020/68040/68060 archives - the
# numbers are the library/romtag ABI version and are identical across all three.
#
# NEPTUNE_BUILD_STAMP marks a local (non-release) build: build.sh sets it, once per run,
# to dev-<YYYYMMDD>-<HHMMSS>-g<hash>[-dirty], and it rides at the end of every $VER cookie
# (NEPTUNE_BUILD_TAIL), in the .lha name and in the ReadMe. Release archives come from CI,
# which never sets it - empty means release, and leaves every output exactly as before.
set(NEPTUNE_BUILD_STAMP "" CACHE STRING "Local-build marker (build.sh sets it); empty = release")
set(_nep_build_tail "")
if(NEPTUNE_BUILD_STAMP)
    set(_nep_build_tail " ${NEPTUNE_BUILD_STAMP}")
endif()
string(TIMESTAMP CURRENT_DATE "%d.%m.%Y")
add_compile_definitions(
    NEPTUNE_VERSION=${PROJECT_VERSION_MAJOR}
    NEPTUNE_REVISION=${PROJECT_VERSION_MINOR}
    NEPTUNE_DATE="${CURRENT_DATE}"
    NEPTUNE_DIST_NAME="Neptune"
    NEPTUNE_CPU="${M68K_CPU}"
    NEPTUNE_BUILD_TAIL="${_nep_build_tail}")

find_program(SFDC_EXECUTABLE NAMES sfdc HINTS /opt/m68k-amigaos/bin REQUIRED)

# External AmigaOS SDKs not bundled with bebbo: the MUI 5 SDK (GUI classes + NeptuneUSB) and the NDK
# 3.2 SANA+Roadshow package (the ethernet classes' <devices/sana2.h>).
# Defaulted under one root - override AMIGA_SDK_ROOT, or either dir directly.
set(AMIGA_SDK_ROOT    "$ENV{HOME}/amiga" CACHE PATH "Root holding the external AmigaOS SDKs (MUI, NDK)")
set(MUI_INCLUDE_DIR   "${AMIGA_SDK_ROOT}/MUI5/SDK/MUI/C/include"               CACHE PATH "MUI 5 SDK include dir")
set(SANA2_INCLUDE_DIR "${AMIGA_SDK_ROOT}/NDK3.2R4/SANA+RoadshowTCP-IP/include" CACHE PATH "SANA-II SDK include dir (NDK SANA+Roadshow)")

# MUI: <inline/muimaster.h>'s __inline MUI_NewObject() is broken, so MUI-using
# sources force-include include/mui_compat.h (a va_list-based replacement, plus the MUI 3.8 floor).
add_library(mui_headers INTERFACE)
target_include_directories(mui_headers INTERFACE ${MUI_INCLUDE_DIR})
add_library(sana2_headers INTERFACE)
target_include_directories(sana2_headers INTERFACE ${SANA2_INCLUDE_DIR})

# Shared helper: generate clib/inline/pragmas/proto headers from <basename>.sfd
# (in the calling component's dir) into <bindir>/generated/include. Creates the
# custom target <basename>_sfd_headers and returns the generated-include dir.
function(generate_sfd_headers basename out_gen_dir)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
    set(sfd ${CMAKE_CURRENT_SOURCE_DIR}/${basename}.sfd)
    set(hdrs
        ${gen}/clib/${basename}_protos.h
        ${gen}/inline/${basename}.h
        ${gen}/pragmas/${basename}_pragmas.h
        ${gen}/proto/${basename}.h)
    add_custom_command(OUTPUT ${hdrs}
        COMMAND ${CMAKE_COMMAND}
            -DSFDC_EXECUTABLE=${SFDC_EXECUTABLE}
            -DSFD_FILE=${sfd}
            -DOUTPUT_DIR=${gen}
            -DOUTPUT_BASENAME=${basename}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateSfdHeaders.cmake
        DEPENDS ${sfd} ${CMAKE_SOURCE_DIR}/cmake/GenerateSfdHeaders.cmake
        VERBATIM)
    add_custom_target(${basename}_sfd_headers DEPENDS ${hdrs})
    set(${out_gen_dir} ${gen} PARENT_SCOPE)
endfunction()

# Shared helper: define a USB class driver - a freestanding .class library built from the
# shared classes/class_main.c skeleton + <name>.class.c. Call from the class's dir:
#
#   add_neptune_class(bootmouse BASETYPE NepHidBase GUI DEFINES HAS_LIBOPEN)
#
# The class version is the distribution version (NEPTUNE_VERSION/REVISION, global) -
# the fleet ships in lockstep, so there is nothing to state per class.
#
# Every class is held to the ROM-cleanliness rule - no writable data at all - by
# and a writable section that reaches a ROM faults at boot with no diagnostic.
#
# The romtag priority follows from the same rule, so it is a default rather than a
# per-class decision: NEP_CLASS_PRI (-45) is the one slot in the Kickstart ROM band
# that works for a class - under neptune.library (-44) and above both startup
# residents (-46/-47), which psdAddClass() us and must find us already initialised.
# Pass PRI only to opt a class *out* of ever being ROM-able - see classes/audio.
#
# Options:  GUI       links mui_headers + -lamiga (config dialog: MUI + BOOPSI DoMethod)
#           AMIGALIB  links -lamiga without MUI (a non-GUI class that still uses amiga.lib stubs)
# One-val:  BASETYPE (required); INCLUDE (default "<name>.h"); PRI (default NEP_CLASS_PRI)
# Multi:    SOURCES   extra .c beyond <name>.class.c + the skeleton
#           DEFINES   extra -D (HAS_LIBOPEN, HAS_LIBCLOSE, DISKLABELS, ...)
#           DEPS      extra link targets (e.g. sana2_headers for the ethernet family)
set(NEP_CLASS_PRI -45)

function(add_neptune_class name)
    cmake_parse_arguments(PC "GUI;AMIGALIB" "BASETYPE;PRI;INCLUDE" "SOURCES;DEFINES;DEPS" ${ARGN})
    if(NOT PC_INCLUDE)
        set(PC_INCLUDE "${name}.h")
    endif()
    if(NOT DEFINED PC_PRI)
        set(PC_PRI ${NEP_CLASS_PRI})
    endif()
    set(tgt ${name}_class)

    # Register every class target in one global list so Packaging (and anything else)
    # installs the whole fleet without a hand-maintained roster. See cmake/Packaging.cmake.
    set_property(GLOBAL APPEND PROPERTY NEPTUNE_CLASS_TARGETS ${tgt})

    add_executable(${tgt}
        ${name}.class.c ${PC_SOURCES}
        ${CMAKE_SOURCE_DIR}/classes/class_main.c
        )
    add_dependencies(${tgt} neptune_sfd_headers)

    # Hot path (the whole USB datapath runs through these): -O3. -ffreestanding must be a
    # COMPILE option - as a link-only flag it is silently inert. The shared flags come from
    # the toolchain (-m${M68K_CPU} -m${M68K_FPU}-float ...) and the root add_compile_options() below.
    target_compile_options(${tgt} PRIVATE -O3 -ffreestanding)

    target_compile_definitions(${tgt} PRIVATE
        __NOLIBBASE__
        NEPTUNE_BASE_NAME=ps
        CLASS_NAME="${name}.usbclass"          # file = romtag = library name; VERSION_STRING etc. derive from it
        CLASS_INCLUDE="${PC_INCLUDE}"
        CLASS_BASETYPE_NAME=${PC_BASETYPE}
        CLASS_PRI=${PC_PRI}
        ${PC_DEFINES})

    target_include_directories(${tgt} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/classes            # shared common.h / debug.h
        ${CMAKE_CURRENT_SOURCE_DIR})

    set(_amiga "")
    if(PC_GUI OR PC_AMIGALIB)
        set(_amiga -lamiga)                    # BOOPSI DoMethod / NewObject stubs
    endif()
    set(_libs neptune_headers ${PC_DEPS})
    if(PC_GUI)
        list(APPEND _libs mui_headers)
    endif()
    target_link_libraries(${tgt} PRIVATE ${_libs}
        -Wl,--start-group -lc ${_amiga} -lgcc -Wl,--end-group)

    target_link_options(${tgt} PRIVATE
        -nostdlib -nostartfiles -s)

    set_target_properties(${tgt} PROPERTIES OUTPUT_NAME ${name} SUFFIX ".usbclass")
    nep_debug_finalize(${tgt})                 # links -ldebug only for the serial backend
    nep_enable_lto(${tgt})
    nep_module_layout(${tgt})                  # entry at 0, RT_ENDSKIP, no writable data
endfunction()

# --- Compile flags ---------------------------------------------------------------
# The CPU/FPU/CRT flags (-m${M68K_CPU} -m${M68K_FPU}-float -fomit-frame-pointer -mcrt=nix20)
# come from cmake/toolchain.cmake - the release builds this tree once
# per CPU. These two apply to every TU in the tree; each target adds only its own -O level
# (library + classes -O3, NeptuneUSB -O2, the CLI programs -Os) and, where it is freestanding,
# -ffreestanding.
#
# -Wno-int-conversion: m68k tag/vararg calls inherently mix pointers and ULONGs (sfdc's
# vararg inlines marshal them through one array); pointers are ULONG-sized here. GCC 14+
# makes int<->pointer conversion an error by default, so this is load-bearing.
# SHELL: keeps the -include pair intact - CMake de-dups a bare repeated -include flag,
# which would otherwise collapse NeptuneUSB's second force-include into this one.
add_compile_options(-Wno-int-conversion
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/aros_compat.h")

# --- Debug logging -------------------------------------------------------------
# Switchable output backend (pistorm | serial | off) + verbosity level, shared by every
# component via include/debug.h. Compile defs are applied globally here (before the
# subdirs); per-target -ldebug linking (serial only) is done by nep_debug_finalize() in
# each component. See cmake/NeptuneDebug.cmake.
include(cmake/NeptuneDebug.cmake)
nep_debug_definitions()
message(STATUS "Neptune: debug backend = ${NEPTUNE_DEBUG_BACKEND}, level = ${NEPTUNE_DEBUG_LEVEL}")

# The ROM-ability guard (no writable sections) used to be a POST_BUILD objdump pass here;
# it is now an ASSERT in ldscripts/module.lds, applied by nep_module_layout() below, so it
# fails at link time and cannot be skipped.

# Module layout contract (entry stub at offset 0, RT_ENDSKIP, no writable sections).
include(cmake/NeptuneModuleLayout.cmake)

# Link-time optimization, opted in per target.
include(cmake/NeptuneLto.cmake)

# --- Components (neptune_class first: neptune.library + the classes consume its headers) ---
add_subdirectory(neptune_class)
add_subdirectory(neptune.library)
add_subdirectory(romstartup)
add_subdirectory(c)
add_subdirectory(tools)
add_subdirectory(vhi)
add_subdirectory(classes/hub)
add_subdirectory(classes/bootmouse)
add_subdirectory(classes/bootkeyboard)
add_subdirectory(classes/massstorage)
add_subdirectory(classes/cdcacm)
add_subdirectory(classes/serialpl2303)
add_subdirectory(classes/serialcp210x)
add_subdirectory(classes/printer)
add_subdirectory(classes/cdceth)
add_subdirectory(classes/asixeth)
add_subdirectory(classes/pegasuseth)
add_subdirectory(classes/davicometh)
add_subdirectory(classes/moschipeth)
add_subdirectory(classes/ethwrap)
add_subdirectory(classes/rndis)
add_subdirectory(classes/lan78xx)
add_subdirectory(classes/egalaxtouch)
add_subdirectory(classes/simplemidi)
add_subdirectory(classes/camdmidi)
add_subdirectory(classes/audio)
add_subdirectory(classes/video)
add_subdirectory(classes/dfu)
add_subdirectory(classes/ptp)
add_subdirectory(classes/rawwrap)
add_subdirectory(classes/bluetooth)
add_subdirectory(classes/stir4200)
add_subdirectory(classes/palmpda)
add_subdirectory(classes/hubss)
add_subdirectory(classes/arosx)
add_subdirectory(classes/hid)
add_subdirectory(neptuneusb)
add_subdirectory(usbeject)

include(cmake/Packaging.cmake)
