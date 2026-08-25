# ---------------------------------------------------------------------------
# GoldenEye 007 PC port - layer 2.
#
# The decomp builds an N64 ROM (top-level Makefile, MIPS toolchain). This
# directory makes the game run on a PC. vr/ then puts it in a headset.
#
#   cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/port -j
#   ctest --test-dir build/port --output-on-failure
#
# See port/README.md for the bring-up notes, especially the header-shadowing
# rules and why a 32-bit target is preferred.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(ge007_port C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(GEPC_BUILD_TESTS "Build the platform layer tests" ON)
option(GEPC_WERROR      "Treat warnings as errors"       OFF)

set(GEPC_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(Threads REQUIRED)

add_library(ge007_platform STATIC
    src/system.c
    src/libultra.c
    src/romdata.c
    src/sha1.c
    src/gbi_walk.c
)

# port/include must come FIRST: it carries the strings.h shim that keeps
# glibc's bcopy/bcmp/bzero declarations from colliding with PR/os.h's.
target_include_directories(ge007_platform PUBLIC include)

# The repository's include/ and src/ go AFTER the system directories. They
# ship N64 replacements for seven libc headers (stdarg, stddef, string, stdlib,
# math, assert, limits); with a plain -I the host's stdio.h picks up the N64
# stdarg.h and the build collapses. -idirafter keeps libc first for those
# names while still finding PR/ and ultra64.h.
# The SHELL: prefix is load-bearing. Without it CMake sees two identical
# -idirafter flags, de-duplicates them, and leaves the second path stranded as
# a bare argument -- which silently drops src/ from the search path and lets
# glibc's strings.h collide with PR/os.h again.
target_compile_options(ge007_platform PUBLIC
    "SHELL:-idirafter ${GEPC_REPO_ROOT}/include"
    "SHELL:-idirafter ${GEPC_REPO_ROOT}/src"
)

# PR/ultratypes.h declares nothing at all without this.
target_compile_definitions(ge007_platform PUBLIC _LANGUAGE_C)

# glibc's fortified headers *define* bcopy and bzero as inline functions taking
# size_t, and PR/os.h declares the same names taking int. That is an outright
# conflict, and it arrives through pthread.h -> features.h ->
# bits/strings_fortified.h, so no include-path ordering can dodge it. It only
# triggers at -O2 and above, which is why a debug build can look fine and
# Release then fails.
#
# The strings.h shim in port/include handles the plain declarations; this
# handles the fortified definitions. Both are needed. Disabling fortification
# for the port is the trade: an N64 codebase cannot coexist with glibc's
# hardened string builtins, and the alternative is rewriting the game's calls.
if(NOT MSVC)
    target_compile_options(ge007_platform PUBLIC
        "SHELL:-U_FORTIFY_SOURCE" "SHELL:-D_FORTIFY_SOURCE=0")
endif()

target_link_libraries(ge007_platform PUBLIC Threads::Threads)

if(NOT MSVC)
    target_compile_options(ge007_platform PRIVATE -Wall -Wextra)
    if(GEPC_WERROR)
        target_compile_options(ge007_platform PRIVATE -Werror)
    endif()
endif()

if(CMAKE_SIZEOF_VOID_P GREATER 4)
    message(STATUS
        "ge007 port: building 64-bit. osVirtualToPhysical returns a u32 by "
        "declaration, so it panics rather than truncating a high pointer. "
        "A 32-bit target avoids the issue entirely - see port/README.md.")
endif()

if(GEPC_BUILD_TESTS)
    enable_testing()
    add_executable(ge007_port_tests tests/test_libultra.c)
    target_link_libraries(ge007_port_tests PRIVATE ge007_platform)
    add_test(NAME ge007_libultra COMMAND ge007_port_tests)
    # The blocking-queue and timer tests sleep on purpose.
    set_tests_properties(ge007_libultra PROPERTIES TIMEOUT 120)

    add_executable(ge007_gbi_tests tests/test_gbi_walk.c)
    target_link_libraries(ge007_gbi_tests PRIVATE ge007_platform)
    add_test(NAME ge007_gbi_walk COMMAND ge007_gbi_tests)
    set_tests_properties(ge007_gbi_walk PROPERTIES TIMEOUT 60)

    add_executable(ge007_romdata_tests tests/test_romdata.c)
    target_link_libraries(ge007_romdata_tests PRIVATE ge007_platform)
    # Writes a small synthetic ROM into the working directory, so run it
    # somewhere writable rather than the source tree.
    add_test(NAME ge007_romdata COMMAND ge007_romdata_tests
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(ge007_romdata PROPERTIES TIMEOUT 60)
endif()
