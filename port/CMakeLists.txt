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

# The ROM layout table, generated from the repository's own extraction
# manifest. scripts/filelist.u.csv records where each file begins in the ROM
# and how long it is -- layout, not content -- which is what the port needs to
# give the game's file table its addresses. See port/include/obseg.h.
set(GEPC_ROM_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/generated/gepc_rom_files.c")
add_custom_command(
    OUTPUT  "${GEPC_ROM_MANIFEST}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_rom_manifest.py"
            "${GEPC_REPO_ROOT}/scripts/filelist.u.csv" "${GEPC_ROM_MANIFEST}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_rom_manifest.py"
            "${GEPC_REPO_ROOT}/scripts/filelist.u.csv"
    COMMENT "Generating the ROM file manifest"
    VERBATIM)

add_library(ge007_platform STATIC
    src/system.c
    src/libultra.c
    src/romdata.c
    src/sha1.c
    src/gbi_walk.c
    src/gfx_state.c
    src/gfx_rdp.c
    src/rdram.c
    src/libultra_stubs.c
    src/gfx_texture.c
    src/audio.c
    src/audio_abi.c
    src/obseg.c
    src/game_random.c
    src/game_symbols.c
    ${GEPC_ROM_MANIFEST}
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
    "SHELL:-idirafter ${GEPC_REPO_ROOT}"
    "SHELL:-idirafter ${GEPC_REPO_ROOT}/include"
    "SHELL:-idirafter ${GEPC_REPO_ROOT}/include/PR"
    "SHELL:-idirafter ${GEPC_REPO_ROOT}/src"
    "SHELL:-idirafter ${GEPC_REPO_ROOT}/src/game"
    "SHELL:-idirafter ${GEPC_REPO_ROOT}/src/libultra"
)

# PR/ultratypes.h declares nothing at all without this.
#
# The rest of this list is the game's own build configuration, and it has to
# match what port/tools/compile-survey.sh measures with -- otherwise the survey
# and the real build are compiling different code. They had in fact drifted:
# GEPC was never defined here at all, so every guarded fix in the port was
# inactive in this build and Gfx came out 16 bytes wide. The accessor test's
# sizeof(Gfx) == 8 check is what caught it, which is exactly what that check is
# for. Keep the two lists in step.
#
# GEPC marks the PC build wherever the game's sources need to differ from what
# IDO accepted. The version defines mirror the Makefile's US target (see its
# LCDEFS): without them bg.c pairs a four-argument call site against a
# five-argument definition and several tables look absent. TARGET_N64 is
# deliberately *not* set -- that would flip platform_info.h into N64 mode.
target_compile_definitions(ge007_platform PUBLIC
    _LANGUAGE_C
    GEPC
    VERSION_US LANG_US REFRESH_NTSC
    LEFTOVERDEBUG LEFTOVERSPECTRUM BUGFIX_R0 BYTEMATCH
)

# gepc_prelude.h supplies what the shadowed repository headers used to carry --
# the float constants, M_PI_F and friends, the generated BITFLAG arities. It is
# force-included rather than added to each source, because the game's files
# include it transitively through headers that cannot be edited freely.
target_compile_options(ge007_platform PUBLIC
    "SHELL:-include gepc_prelude.h")

# The game builds its type hierarchy on `#define inherits struct`, so a child
# record starts with an unnamed member carrying a tag:
#
#     typedef struct WeaponObjRecord { inherits ObjectRecord; ... }
#
# That is the anonymous-member extension IDO had, and it is what makes
# wep->type reach ObjectRecord's field. Standard C only allows an anonymous
# member when the struct is defined inline without a tag, so plain GCC treats
# the line as a stray declaration, silently contributes no members, and every
# inherited field then fails with "has no member named". -fms-extensions
# restores IDO's behaviour. It affects name lookup only -- an unnamed member
# is laid out exactly as a named one of the same type would be -- so record
# offsets are unchanged.
if(NOT MSVC)
    target_compile_options(ge007_platform PUBLIC -fms-extensions)
endif()

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

# A display list command word is 32 bits (see the GEPC branch of Gwords in
# include/PR/gbi.h), so every address the game can put in one has to fit. The
# RDRAM arena handles everything the game allocates, but statically initialised
# Gfx arrays and the vertex and texture data they point at live in the
# executable's own .data and .rodata. A PIE build loads those far above 4 GB
# and each pointer would truncate in silence -- the list still walks, the
# counts still look right, the geometry is garbage.
#
# -no-pie puts the image at 0x400000 on x86-64 ELF. gepcAssertLowMemory()
# verifies it at startup so a toolchain that ignores the flag (some
# distributions force PIE through their spec files) fails loudly instead.
if(NOT MSVC AND NOT APPLE)
    include(CheckPIESupported OPTIONAL RESULT_VARIABLE _gepc_have_pie_module)
    set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
    target_link_options(ge007_platform PUBLIC -no-pie)
    target_compile_options(ge007_platform PUBLIC -fno-pie)
endif()

if(NOT MSVC)
    target_compile_options(ge007_platform PRIVATE -Wall -Wextra)
    if(GEPC_WERROR)
        target_compile_options(ge007_platform PRIVATE -Werror)
    endif()
endif()

# 64-bit is the supported configuration. The game's pointers still have to fit
# in the u32 osVirtualToPhysical returns, which is handled by confining all
# game-visible memory to the RDRAM arena in src/rdram.c rather than by building
# 32-bit. See port/README.md.
if(CMAKE_SIZEOF_VOID_P GREATER 4)
    message(STATUS
        "ge007 port: 64-bit build; game memory is confined to the low RDRAM "
        "arena so pointers survive osVirtualToPhysical.")
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

    add_executable(ge007_gfx_tests tests/test_gfx_state.c)
    target_link_libraries(ge007_gfx_tests PRIVATE ge007_platform)
    add_test(NAME ge007_gfx_state COMMAND ge007_gfx_tests)
    set_tests_properties(ge007_gfx_state PROPERTIES TIMEOUT 60)

    add_executable(ge007_audio_tests tests/test_audio.c)
    target_link_libraries(ge007_audio_tests PRIVATE ge007_platform)
    add_test(NAME ge007_audio COMMAND ge007_audio_tests)
    set_tests_properties(ge007_audio PROPERTIES TIMEOUT 60)

    add_executable(ge007_hook_tests tests/test_gfxhook.c)
    target_link_libraries(ge007_hook_tests PRIVATE ge007_platform)
    add_test(NAME ge007_gfxhook COMMAND ge007_hook_tests)
    set_tests_properties(ge007_gfxhook PROPERTIES TIMEOUT 60)

    add_executable(ge007_texture_tests tests/test_gfx_texture.c)
    target_link_libraries(ge007_texture_tests PRIVATE ge007_platform)
    add_test(NAME ge007_texture COMMAND ge007_texture_tests)
    set_tests_properties(ge007_texture PROPERTIES TIMEOUT 60)

    add_executable(ge007_rdram_tests tests/test_rdram.c)
    target_link_libraries(ge007_rdram_tests PRIVATE ge007_platform)
    add_test(NAME ge007_rdram COMMAND ge007_rdram_tests)
    set_tests_properties(ge007_rdram PROPERTIES TIMEOUT 60)

    add_executable(ge007_random_tests tests/test_game_random.c)
    target_link_libraries(ge007_random_tests PRIVATE ge007_platform)
    add_test(NAME ge007_game_random COMMAND ge007_random_tests)
    set_tests_properties(ge007_game_random PROPERTIES TIMEOUT 60)

    add_executable(ge007_rdp_tests tests/test_gfx_rdp.c)
    target_link_libraries(ge007_rdp_tests PRIVATE ge007_platform)
    add_test(NAME ge007_gfx_rdp COMMAND ge007_rdp_tests)
    set_tests_properties(ge007_gfx_rdp PROPERTIES TIMEOUT 60)

    add_executable(ge007_accessor_tests tests/test_gbi_accessors.c)
    target_link_libraries(ge007_accessor_tests PRIVATE ge007_platform)
    add_test(NAME ge007_gbi_accessors COMMAND ge007_accessor_tests)
    set_tests_properties(ge007_gbi_accessors PROPERTIES TIMEOUT 60)

    add_executable(ge007_romdata_tests tests/test_romdata.c)
    target_link_libraries(ge007_romdata_tests PRIVATE ge007_platform)
    # Writes a small synthetic ROM into the working directory, so run it
    # somewhere writable rather than the source tree.
    add_test(NAME ge007_romdata COMMAND ge007_romdata_tests
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(ge007_romdata PROPERTIES TIMEOUT 60)
endif()

# --------------------------------------------------------------- GL backend

# ------------------------------------------------------------------ the VR layer
#
# vr/ was built as its own project while the port did not exist yet. It does
# now, so it comes in here -- but on its own terms: gevr_core is the control
# mapping and the maths, with no windowing or headset dependency at all, and
# gevr_runtime is the OpenXR half that only appears when a runtime is
# installed.
#
# GE_VR is what activates the hooks in the game (joy.c injects the mapped pads,
# fr.c takes the per-eye projection), and it is only defined when the runtime
# is actually there. Without it the port builds and runs flat, which is what
# the shim's own contract asks for: a missing or broken headset should not stop
# the game starting. The N64 ROM build never defines it under any
# circumstances, which is what keeps that build byte-for-byte unchanged.
add_subdirectory("${GEPC_REPO_ROOT}/vr" "${CMAKE_CURRENT_BINARY_DIR}/vr")

if(TARGET gevr_runtime)
    set(GEPC_VR_ENABLED TRUE)
    message(STATUS "ge007: VR enabled (OpenXR runtime found)")
else()
    set(GEPC_VR_ENABLED FALSE)
    message(STATUS
        "ge007: VR disabled -- no OpenXR runtime found. The game builds and "
        "runs flat; install libopenxr-dev for the headset path.")
endif()

# ---------------------------------------------------------------- the game
#
# Until now nothing here compiled the game's own sources. The survey in
# port/tools/compile-survey.sh proves each translation unit compiles on its
# own, which is a weaker claim than "these link together": it says nothing
# about duplicate definitions, symbols nobody provides, or whether there is an
# entry point at all.
#
# The exclusions match the survey's exactly. src/libultra/os, src/libultra/io
# and the matching src/libultrare directories are the N64 hardware layers that
# port/src/libultra.c replaces. Only os/ and io/ though -- libultrare/audio and
# libultrare/libc are ordinary library code the game calls (the synthesis
# driver and _Printf), and excluding the whole tree, as this first did, loses
# them. Same mistake as excluding libultra/gu and libultra/audio would be.
# src/libultra/libc/string.c defines strchr, strlen and memcpy over
# `const unsigned char *`, which on a hosted build are glibc's. Keep this list
# and the survey's in step -- they are measuring the same thing.
file(GLOB_RECURSE GEPC_GAME_SOURCES CONFIGURE_DEPENDS
     "${GEPC_REPO_ROOT}/src/*.c")
list(FILTER GEPC_GAME_SOURCES EXCLUDE REGEX "/src/libultra/os/")
list(FILTER GEPC_GAME_SOURCES EXCLUDE REGEX "/src/libultra/io/")
list(FILTER GEPC_GAME_SOURCES EXCLUDE REGEX "/src/libultrare/os/")
list(FILTER GEPC_GAME_SOURCES EXCLUDE REGEX "/src/libultrare/io/")
list(FILTER GEPC_GAME_SOURCES EXCLUDE REGEX "/src/libultra/libc/string\\.c$")

# The Makefile splits the assets in two, and so does this.
#
# ASSET_DATAFILES and the fonts are ordinary data linked into the code
# segment -- the animation table, the font glyphs, the Rareware logo -- so
# they compile like any other source. Everything under assets/obseg is not:
# ob_seg.s .incbins compressed files that the Makefile extracts from the
# player's ROM into build/u/assets/obseg, and a fresh checkout has none of
# them. Those become ROM segments the game loads on demand, which is what
# port/src/romdata.c exists for; they must not be linked here.
#
# The 1080 .inc.c files under assets/ are #included by the sources above and
# must never be compiled on their own.
list(APPEND GEPC_GAME_SOURCES
     "${GEPC_REPO_ROOT}/assets/animationtable_data.c"
     "${GEPC_REPO_ROOT}/assets/animationtable_entries.c"
     "${GEPC_REPO_ROOT}/assets/font_dl.c"
     "${GEPC_REPO_ROOT}/assets/font_chardatae.c"
     "${GEPC_REPO_ROOT}/assets/font_chardataj.c"
     "${GEPC_REPO_ROOT}/assets/oddtextures.c"
     "${GEPC_REPO_ROOT}/assets/rarewarelogo.c"
     "${GEPC_REPO_ROOT}/assets/font/fontBankGothic.c"
     "${GEPC_REPO_ROOT}/assets/font/fontZurichBold.c")

list(LENGTH GEPC_GAME_SOURCES GEPC_GAME_COUNT)
message(STATUS "ge007: ${GEPC_GAME_COUNT} game translation units")

if(GEPC_VR_ENABLED)
    # Compiled here rather than in vr/, because it includes bondtypes.h and
    # game/bondview.h alongside the VR headers -- it is the seam, so it needs
    # both sides.
    list(APPEND GEPC_GAME_SOURCES "${GEPC_REPO_ROOT}/vr/shim/gevr_shim.c")
endif()

add_library(ge007_game STATIC ${GEPC_GAME_SOURCES})
target_link_libraries(ge007_game PUBLIC ge007_platform)

if(GEPC_VR_ENABLED)
    target_compile_definitions(ge007_game PUBLIC GE_VR)
    target_include_directories(ge007_game PUBLIC
        "${GEPC_REPO_ROOT}/vr/include"
        "${GEPC_REPO_ROOT}/vr/shim")
    target_link_libraries(ge007_game PUBLIC gevr_runtime)
endif()

# The game's sources are a 1997 codebase built by a compiler that warned about
# far less. Their warnings are not actionable here and would bury the port's
# own, so they are quietened for this target only; ge007_platform keeps
# -Wall -Wextra.
if(NOT MSVC)
    target_compile_options(ge007_game PRIVATE -w)
endif()

# The only part of the renderer that needs a GPU. Everything with a right
# answer -- display-list decoding, the matrix stack, texture conversion --
# lives in ge007_platform and is tested headlessly, so this staying optional
# costs no coverage.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GEPC_SDL2 IMPORTED_TARGET sdl2)
endif()

if(GEPC_SDL2_FOUND)
    add_library(ge007_gfx_gl STATIC src/gfx_gl.c src/video.c src/audio_sdl.c)
    target_link_libraries(ge007_gfx_gl PUBLIC ge007_platform PkgConfig::GEPC_SDL2)
    if(NOT MSVC)
        target_compile_options(ge007_gfx_gl PRIVATE -Wall -Wextra)
        if(GEPC_WERROR)
            target_compile_options(ge007_gfx_gl PRIVATE -Werror)
        endif()
    endif()
    # End-to-end harness. Drives the real stack -- RDRAM, task interception,
    # decoder, GL backend, audio -- and reads the framebuffer back, which is
    # the only way to tell a working renderer from one whose counters merely
    # look healthy. Needs a display; under CI run it with
    #   LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./ge007-selftest
    add_executable(ge007-selftest tools/ge007_selftest.c)
    target_link_libraries(ge007-selftest PRIVATE ge007_gfx_gl)

# ge007.ld gives every ROM segment a symbol for where it sits in the
    # cartridge, and the game takes those symbols' *addresses* -- it DMAs from
    # &_xSegmentRomStart and sizes the transfer from the gap to the next one.
    # Nothing reads one as a value, so --defsym can place them at literal
    # cartridge addresses: no source change, no runtime lookup, and the
    # difference between two of them is the real size because the addresses
    # are real. Offsets come from the same manifest as the file table.
    set(GEPC_DEFSYMS "${CMAKE_CURRENT_BINARY_DIR}/generated/segment_defsyms.txt")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/generated")
    execute_process(
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_segment_defsyms.py"
                "${GEPC_REPO_ROOT}/scripts/filelist.u.csv" "${GEPC_DEFSYMS}"
        RESULT_VARIABLE GEPC_DEFSYM_RC)
    if(NOT GEPC_DEFSYM_RC EQUAL 0)
        message(FATAL_ERROR "could not generate the ROM segment symbols")
    endif()
    file(STRINGS "${GEPC_DEFSYMS}" GEPC_DEFSYM_LIST)

    # The playable executable. src/main.c stands in for boot.s: it brings the
    # machine up and then calls the game's own init(), exactly as the
    # cartridge did.
    add_executable(ge007 src/main.c)
    target_link_libraries(ge007 PRIVATE ge007_game ge007_gfx_gl)
    foreach(_sym IN LISTS GEPC_DEFSYM_LIST)
        target_link_options(ge007 PRIVATE "LINKER:${_sym}")
    endforeach()
    if(NOT MSVC)
        target_link_libraries(ge007 PRIVATE m)
    endif()
else()
    message(STATUS
        "ge007 port: SDL2 not found; skipping the OpenGL backend. "
        "The decoder and its tests build regardless.")
endif()
