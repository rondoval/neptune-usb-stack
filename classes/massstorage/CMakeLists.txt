# massstorage.class - USB mass-storage -> AmigaDOS volumes. The biggest class: the class library
# PLUS an embedded usbscsi.device (dev.c, MakeLibrary'd at run time) PLUS three USB transports
# (BBB/CBI/UAS) PLUS the mounter (RDB/MBR/GPT/superfloppy/CD, sources listed by its
# mounter.cmake; plus its vdrive units, usbscsivd.device for the virtual drives; -DMOUNTER_LOG
# routes its diagnostics to our mounter_log() -> debug backend, -DMOUNTER_TRACE=1
# keeps its per-step dbg() tracing on -- the submodule used to hardcode that, and
# now leaves the choice to the host). MUI config GUI.
# See add_neptune_class() in the top-level CMakeLists.txt.
include(${CMAKE_CURRENT_SOURCE_DIR}/mounter/mounter.cmake)

add_neptune_class(massstorage BASETYPE NepMSBase GUI
    SOURCES massstorage_bulk.c massstorage_cbi.c massstorage_uas.c
            massstorage_removable.c dev.c
            ${MOUNTER_SOURCES} ${VDRIVE_SOURCES}
    DEFINES HAS_LIBOPEN HAS_LIBCLOSE MOUNTER_LOG MOUNTER_TRACE=1)

set_source_files_properties(
    ${MOUNTER_C_SOURCES} ${VDRIVE_SOURCES}
    PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wsign-conversion;-Wshadow;-Wmissing-prototypes;-Wstrict-prototypes")
