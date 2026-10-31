# Neptune shellapps - per-gadget CLI tools (USB radio, cameras, UPS, rocket launcher)

# USBVideo is the usbvideo.device client for users; USBVideoTest is the test bench of the class,
# built here but not shipped (cmake/Packaging.cmake leaves it out).
set(SHELLAPPS USBRadio USBPencam USBPowMan USBRocket USBSonixcam USBUPS USBVideo USBVideoTest)

foreach(t ${SHELLAPPS})
    add_executable(${t} ${t}.c)
    add_dependencies(${t} neptune_sfd_headers)
    target_compile_options(${t} PRIVATE
        -Os                            # cold shell commands: size over speed
        -DNEPTUNE_BASE_NAME=ps)
    target_include_directories(${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR})        # local <name>.h
    target_link_libraries(${t} PRIVATE neptune_headers -lm)  # -lm: pow/atof (camera tools)
    nep_debug_finalize(${t})   # links -ldebug only for the serial backend (tools have no log sites)
endforeach()

# uvc_client.c: what a client of usbvideo.device does (commands, mode list, reads, files).
# uvc_preview.c: a camera run by a process of its own, with a preview window.
# mjpegdht.h is the VHI driver's; the JPEG files saved are completed with it.
foreach(t USBVideo USBVideoTest)
    target_sources(${t} PRIVATE uvc_client.c uvc_preview.c)
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/vhi)
endforeach()

# The window. MUI is opened at run time: USBVideo in the Shell runs without it.
target_sources(USBVideo PRIVATE usbvideo_gui.c)
set_source_files_properties(usbvideo_gui.c PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/include/mui_compat.h")  # as NeptuneUSB
target_link_libraries(USBVideo PRIVATE mui_headers -lamiga)  # DoMethod and the MUI vararg macros
