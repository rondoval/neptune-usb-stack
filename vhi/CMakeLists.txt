# neptune.vhi - the VHI driver for USB video cameras. VHI applications (cAMIra, VHI Studio)
# list LIBS:VHI/ and open what they find there. This driver has no USB code: it is a client
# of usbvideo.device, the application interface of usbvideo.class.
#
# A freestanding library laid out like the classes (entry stub, romtag, no writable data),
# with its own five vectors instead of the class skeleton's.
add_executable(neptune_vhi neptune.vhi.c)

target_compile_options(neptune_vhi PRIVATE -Os -ffreestanding)
target_compile_definitions(neptune_vhi PRIVATE __NOLIBBASE__)
target_include_directories(neptune_vhi PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR})           # mjpegdht.h, which the UVC tools use as well
# -lnix: the C library's memmove() (the frame is slid down over itself) rests on a block
# copy that lives there. Nothing else is taken from it.
target_link_libraries(neptune_vhi PRIVATE
    -Wl,--start-group -lc -lnix -lgcc -Wl,--end-group)
target_link_options(neptune_vhi PRIVATE -nostdlib -nostartfiles -s)

set_target_properties(neptune_vhi PROPERTIES OUTPUT_NAME neptune SUFFIX ".vhi")
nep_enable_lto(neptune_vhi)
nep_module_layout(neptune_vhi)            # entry at 0, RT_ENDSKIP, no writable data
