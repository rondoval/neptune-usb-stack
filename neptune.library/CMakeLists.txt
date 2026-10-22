# neptune.library - the stack core (see docs/porting-playbook.md for the de-AROS recipe).

generate_sfd_headers(neptune NEPTUNE_GEN_DIR)

# Expose neptune.library's generated proto/inline/clib so the CLI tools and the
# class drivers can include <proto/neptune.h>.
add_library(neptune_headers INTERFACE)
target_include_directories(neptune_headers INTERFACE ${NEPTUNE_GEN_DIR})

add_executable(neptune_library
    neptune_main.c
    neptune.library.c
    numtostr.c
    notify.gui.c
)

# Headers must exist before we compile: our own neptune.sfd + neptune_class's (we call usbDoMethodA).
add_dependencies(neptune_library neptune_sfd_headers neptune_class_sfd_headers)

# The stack core: -O3. -ffreestanding must be a COMPILE option - as a link-only flag it is
# silently inert. Shared flags come from the toolchain + the root add_compile_options().
target_compile_options(neptune_library PRIVATE -O3 -ffreestanding)

target_compile_definitions(neptune_library PRIVATE __NOLIBBASE__)

target_include_directories(neptune_library PRIVATE
    ${CMAKE_SOURCE_DIR}/include          # public ABI headers (libraries/, devices/)
    ${CMAKE_CURRENT_SOURCE_DIR})         # neptune_intern.h, debug.h, ...

# neptune_headers + neptune_class_headers (INTERFACE) bring the generated-include dirs;
# mui_headers brings the MUI 5 SDK include dir (notify.gui.c). -lc (libnix string fns),
# -lamiga (BOOPSI DoMethod/set/get used by the notifier's MUI tree), -lgcc (intrinsics). The
# serial debug backend appends -ldebug (+ a weak __divsi3 glue object) via
# nep_debug_finalize() below; pistorm/off link neither.
target_link_libraries(neptune_library PRIVATE
    neptune_headers neptune_class_headers mui_headers
    -Wl,--start-group -lc -lamiga -lgcc -Wl,--end-group)

target_link_options(neptune_library PRIVATE
    -nostdlib -nostartfiles -s)

set_target_properties(neptune_library PROPERTIES
    OUTPUT_NAME neptune SUFFIX ".library")
nep_debug_finalize(neptune_library)   # links -ldebug only for the serial backend

# The core goes into the Kickstart ROM set, so hold it to the no-writable-data
# rule the code is already written for (asserted by ldscripts/module.lds).
nep_enable_lto(neptune_library)
nep_module_layout(neptune_library)
