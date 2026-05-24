# pam_fastauth.so — minimal C bridge between PAM and fastauthd.
# Real implementation lands on M6. For now this is a stub so the build
# graph is wired up.

add_library(pam_fastauth MODULE
    pam_fastauth.c
)
set_target_properties(pam_fastauth PROPERTIES
    PREFIX ""              # PAM expects "pam_fastauth.so", not "libpam_fastauth.so"
    OUTPUT_NAME "pam_fastauth"
)
target_link_libraries(pam_fastauth PRIVATE
    PkgConfig::LIBPAM
)
fastauth_apply_c_compile_options(pam_fastauth)
