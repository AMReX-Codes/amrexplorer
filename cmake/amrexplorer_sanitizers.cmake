function(amrexplorer_enable_sanitizers)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "AMREXPLORER_ENABLE_SANITIZERS currently supports GNU, Clang, and AppleClang")
    endif()

    # float-cast-overflow is named explicitly because GCC leaves it out of the
    # undefined group, unlike Clang: without it both sanitizer jobs run straight
    # over an out-of-range floating-point to integer conversion, which is how one
    # reached review in a validator reading doubles off the wire. Naming it costs
    # nothing on either compiler (Clang already had it) and the check is what
    # catches the next one.
    add_compile_options(
        -fsanitize=address,undefined,float-cast-overflow
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    add_link_options(
        -fsanitize=address,undefined,float-cast-overflow
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
endfunction()

# ThreadSanitizer is incompatible with ASan/UBSan in one binary, so it is a
# separate option and preset (see AMREXPLORER_ENABLE_TSAN).
function(amrexplorer_enable_tsan)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "AMREXPLORER_ENABLE_TSAN currently supports GNU, Clang, and AppleClang")
    endif()

    add_compile_options(
        -fsanitize=thread
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    add_link_options(
        -fsanitize=thread
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
endfunction()
