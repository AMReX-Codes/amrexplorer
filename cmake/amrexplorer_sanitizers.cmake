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

# libFuzzer coverage instrumentation, project-wide. The harness alone being
# instrumented would leave the fuzzer blind to the branches that matter -- the
# ones inside the parsers and validators it drives -- so every target is
# compiled with the coverage hooks. fuzzer-no-link at link time supplies the
# hooks' runtime without libFuzzer's main, so ordinary executables still link
# and run as usual; only the fuzz drivers add -fsanitize=fuzzer for the driver
# main (see amrexplorer_add_fuzz_target in tests/CMakeLists.txt).
function(amrexplorer_enable_libfuzzer_instrumentation)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "AMREXPLORER_LIBFUZZER requires Clang (libFuzzer runtime).")
    endif()
    add_compile_options(-fsanitize=fuzzer-no-link)
    add_link_options(-fsanitize=fuzzer-no-link)
endfunction()
