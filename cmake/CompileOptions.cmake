# Common compile options shared by every fastauth target.
# Keep this header-light so it can be safely included multiple times.

function(fastauth_apply_compile_options target)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        target_compile_features(${target} INTERFACE cxx_std_20)
        # INTERFACE libraries can't carry warning flags; consumers do that.
    else()
        target_compile_features(${target} PUBLIC cxx_std_20)
        set_target_properties(${target} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual
            -Wconversion -Wsign-conversion
            -fstack-protector-strong
        )
    endif()
endfunction()

# C variant for the PAM module.
function(fastauth_apply_c_compile_options target)
    target_compile_features(${target} PUBLIC c_std_17)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wcast-align -Wunused
        -Wconversion -Wsign-conversion
        -fstack-protector-strong
        -fPIC
    )
endfunction()
