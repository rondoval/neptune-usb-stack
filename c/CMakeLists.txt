set(SHELLCMDS
    USBStart
    USBAddHardware
    USBAddClasses
    USBList
    USBErrorlog)

foreach(t ${SHELLCMDS})
    add_executable(${t} ${t}.c)
    add_dependencies(${t} neptune_sfd_headers)
    target_compile_options(${t} PRIVATE
        -Os                            # cold shell commands: size over speed
        -DNEPTUNE_BASE_NAME=ps)       # client base-var = the program's own `ps`
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(${t} PRIVATE neptune_headers)   # default CRT link
endforeach()
