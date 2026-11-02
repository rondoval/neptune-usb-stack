# neptune_class is a pure ABI base - nothing opens it at runtime (psdAddClass opens each
# class library by name, and every class implements the 3 LVO functions itself). So
# there is NO neptune_class.library binary; we only generate and expose its headers, which
# neptune.library and all 30 class drivers consume.

generate_sfd_headers(neptune_class NEPTUNE_CLASS_GEN_DIR)

add_library(neptune_class_headers INTERFACE)
target_include_directories(neptune_class_headers INTERFACE ${NEPTUNE_CLASS_GEN_DIR})
