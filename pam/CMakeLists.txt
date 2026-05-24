# pam_chowdy.so — minimal C bridge between PAM and chowdyd.
# Real implementation lands on M6. For now this is a stub so the build
# graph is wired up.

add_library(pam_chowdy MODULE
    pam_chowdy.c
)
set_target_properties(pam_chowdy PROPERTIES
    PREFIX ""              # PAM expects "pam_chowdy.so", not "libpam_chowdy.so"
    OUTPUT_NAME "pam_chowdy"
)
target_link_libraries(pam_chowdy PRIVATE
    PkgConfig::LIBPAM
)
chowdy_apply_c_compile_options(pam_chowdy)
