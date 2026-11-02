# USBEject - WBStartup daemon: Workbench "USB" menu with per-device safe eject.

find_program(FLEXCAT_EXECUTABLE NAMES flexcat HINTS $ENV{HOME}/.local/bin /opt/m68k-amigaos/bin REQUIRED)

# Generate strings.h (the CatCompArray + MSG_* enum) from USBEject.cd via flexcat +
# the shared AROS catcomp template (neptuneusb/catalogs/C_h_aros.sd), into <bindir>/generated/.
set(_strings ${CMAKE_CURRENT_BINARY_DIR}/generated/strings.h)
add_custom_command(OUTPUT ${_strings}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${FLEXCAT_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/USBEject.cd
            ${_strings}=${CMAKE_SOURCE_DIR}/neptuneusb/catalogs/C_h_aros.sd
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/USBEject.cd
            ${CMAKE_SOURCE_DIR}/neptuneusb/catalogs/C_h_aros.sd
    VERBATIM)
add_custom_target(usbeject_strings DEPENDS ${_strings})

# Localized .catalog files (runtime assets - English defaults work without them).
# flexcat returns RETURN_WARN(5) on translation gaps but still writes the catalog,
# so tolerate a non-zero exit. Built into <bindir>/catalogs/ and packaged to
# LOCALE:Catalogs/<language>/USBEject.catalog (plain name: the daemon lives in
# SYS:WBStartup, so OpenCatalog(NULL, "USBEject.catalog") finds it there).
set(USBEJECT_LANGS czech french italian polish russian spanish)
set(_catalogs)
foreach(lang ${USBEJECT_LANGS})
    set(_cat ${CMAKE_CURRENT_BINARY_DIR}/catalogs/${lang}.catalog)
    add_custom_command(OUTPUT ${_cat}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/catalogs
        COMMAND sh -c "'${FLEXCAT_EXECUTABLE}' '${CMAKE_CURRENT_SOURCE_DIR}/catalogs/USBEject.cd' '${CMAKE_CURRENT_SOURCE_DIR}/catalogs/${lang}.ct' CATALOG '${_cat}' || true"
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/USBEject.cd
                ${CMAKE_CURRENT_SOURCE_DIR}/catalogs/${lang}.ct
        VERBATIM)
    list(APPEND _catalogs ${_cat})
endforeach()
add_custom_target(usbeject_catalogs ALL DEPENDS ${_catalogs})

add_executable(USBEject USBEject.c locale.c)
add_dependencies(USBEject neptune_sfd_headers neptune_class_sfd_headers usbeject_strings)
target_compile_options(USBEject PRIVATE
    -Os                            # cold resident daemon: size over speed
    -DNEPTUNE_BASE_NAME=ps)       # client base-var = the program's own `ps`
target_include_directories(USBEject PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated)   # the flexcat-generated strings.h
target_link_libraries(USBEject PRIVATE neptune_headers neptune_class_headers -lamiga)
nep_debug_finalize(USBEject)
