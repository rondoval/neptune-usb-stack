# NeptuneUSB - the MUI prefs editor, based on Trident by Chris Hodges. A CRT *program*
# (not a library): default CRT link, client base-var (NEPTUNE_BASE_NAME=ps, no
# __NOLIBBASE__), plain global MUIMasterBase. 4 MUI custom classes + embedded icon data +
# localized catalogs.

find_program(FLEXCAT_EXECUTABLE NAMES flexcat HINTS $ENV{HOME}/.local/bin /opt/m68k-amigaos/bin REQUIRED)

# Generate strings.h (the CatCompArray + MSG_* enum) from neptuneusb.cd via flexcat +
# the AROS catcomp template, into <bindir>/generated/.
set(_strings ${CMAKE_CURRENT_BINARY_DIR}/generated/strings.h)
add_custom_command(OUTPUT ${_strings}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${FLEXCAT_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/neptuneusb.cd
            ${_strings}=${CMAKE_CURRENT_SOURCE_DIR}/catalogs/C_h_aros.sd
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/neptuneusb.cd
            ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/C_h_aros.sd
    VERBATIM)
add_custom_target(neptuneusb_strings DEPENDS ${_strings})

# Localized .catalog files (runtime assets - English defaults work without them).
# flexcat returns RETURN_WARN(5) on translation gaps but still writes the catalog,
# so tolerate a non-zero exit. Built into <bindir>/catalogs/ for manual install to
# LOCALE:Catalogs/<language>/System/Prefs/NeptuneUSB.catalog.
set(NEPTUNEUSB_LANGS czech french italian polish russian spanish)
set(_catalogs)
foreach(lang ${NEPTUNEUSB_LANGS})
    set(_cat ${CMAKE_CURRENT_BINARY_DIR}/catalogs/${lang}.catalog)
    add_custom_command(OUTPUT ${_cat}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/catalogs
        COMMAND sh -c "'${FLEXCAT_EXECUTABLE}' '${CMAKE_CURRENT_SOURCE_DIR}/catalogs/neptuneusb.cd' '${CMAKE_CURRENT_SOURCE_DIR}/catalogs/${lang}.ct' CATALOG '${_cat}' || true"
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/neptuneusb.cd
                ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/${lang}.ct
        VERBATIM)
    list(APPEND _catalogs ${_cat})
endforeach()
add_custom_target(neptuneusb_catalogs ALL DEPENDS ${_catalogs})

file(GLOB MASON_ICONS CONFIGURE_DEPENDS MasonIcons/*.c)

add_executable(NeptuneUSB
    NeptuneUSB.c
    ActionClass.c
    CfgListClass.c
    DevWinClass.c
    IconListClass.c
    locale.c
    ${MASON_ICONS})

add_dependencies(NeptuneUSB neptune_sfd_headers neptune_class_sfd_headers neptuneusb_strings)

target_compile_options(NeptuneUSB PRIVATE
    -O2 -DNEPTUNE_BASE_NAME=ps
    "SHELL:-include ${CMAKE_SOURCE_DIR}/include/mui_compat.h")  # broken SDK MUI_NewObject + the MUI 3.8 floor

target_include_directories(NeptuneUSB PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated)   # the flexcat-generated strings.h

target_link_libraries(NeptuneUSB PRIVATE
    neptune_headers
    neptune_class_headers
    mui_headers
    -lamiga)        # DoMethod / DoSuperMethodA / MUI vararg macros

nep_debug_finalize(NeptuneUSB)   # links -ldebug only for the serial backend (NeptuneUSB has no log sites)
