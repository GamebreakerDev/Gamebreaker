cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Gamebreaker - Static Recompilation
#
# Based on xboxrecomp's templates/new-game (MIT, sp00nznet and contributors).
# Expects the xboxrecomp clone next to this folder (../xboxrecomp) and the
# generated code in src/recomp/gen/ (not in git; produced by tools.recomp).
# ============================================================================

# RC: res/gamebreaker_game.rc (the program's icon and version information).
# MSVC compiles it with rc.exe; llvm-mingw with its windres, found beside clang.
project(gamebreaker C RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Clang or GCC (MinGW-w64, e.g. llvm-mingw): code-generation rules the lifted
# code and the toolkit depend on. Set here, before add_subdirectory, so the
# toolkit's libraries get them too without editing the toolkit.
#   -fwrapv               signed arithmetic wraps like the Xbox CPU (lifted
#                         code has ~1,100 signed multiplies that may overflow)
#   -fno-strict-aliasing  guest memory is read through type-punned pointers
#   -ffp-contract=off     no fused multiply-add: keep each rounding step
# Never add -ffast-math or -march=native (the x87 emulation needs exact
# double arithmetic). -g -gcodeview with --pdb (below) lets the fault handler
# name functions through dbghelp, as with MSVC's PDB.
if(NOT MSVC)
    add_compile_options(-fwrapv -fno-strict-aliasing -ffp-contract=off
                        -gline-tables-only -gcodeview)
endif()

# GB_QUIET_BUILD (the player's setup turns it on): no warnings from the toolkit
# or the lifted code, thousands of harmless lines in the player's log. The
# port's own files keep theirs, and errors are never silenced.
option(GB_QUIET_BUILD "No compiler warnings from the toolkit and the lifted code" OFF)
if(GB_QUIET_BUILD AND NOT MSVC)
    add_compile_options(-w)
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# The toolkit took its copy of the options above; take -w back for this folder.
if(GB_QUIET_BUILD AND NOT MSVC)
    get_directory_property(_gb_dir_options COMPILE_OPTIONS)
    list(REMOVE_ITEM _gb_dir_options -w)
    set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_gb_dir_options}")
endif()

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
#
# GB_GEN_DIR: where the lifted code is. The developer layout keeps it in
# src/recomp/gen; the player's setup (setup/setup.ps1) keeps it in its own
# work folder, so this source tree is only read.
set(GB_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "Folder holding the lifted code (recomp_*.c, recomp_funcs.h)")
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${GB_GEN_DIR}/*.c")
if(GB_QUIET_BUILD AND NOT MSVC)
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
endif()

# The run switches (config/run_env.txt) as a C table, so the program sets
# them itself when started without a script (main.c,
# gb_apply_default_switches). Lines look like  NAME=value [cpu]  # comment.
# file(STRINGS) splits at ';', so only the part before the first ';' of a line
# is used, which always holds NAME=value and the [cpu] mark.
set(_gb_env_file "${CMAKE_CURRENT_SOURCE_DIR}/config/run_env.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gb_env_file}")
file(STRINGS "${_gb_env_file}" _gb_env_lines)
set(GB_RUN_ENV_ENTRIES "")
foreach(_l IN LISTS _gb_env_lines)
    if(_l MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=([^ \t#]+)")
        set(_n "${CMAKE_MATCH_1}")
        set(_v "${CMAKE_MATCH_2}")
        if(_l MATCHES "\\[cpu\\]")
            set(_cpu 1)
        else()
            set(_cpu 0)
        endif()
        string(APPEND GB_RUN_ENV_ENTRIES "    { \"${_n}\", \"${_v}\", ${_cpu} },\n")
    endif()
endforeach()
if(GB_RUN_ENV_ENTRIES STREQUAL "")
    message(FATAL_ERROR "no switches found in ${_gb_env_file}")
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gb_run_env.h" CONTENT [=[
/* Generated by port/CMakeLists.txt from port/config/run_env.txt. Do not edit. */
#pragma once
static const struct { const char *name, *value; int cpu_only; } gb_run_env[] = {
@GB_RUN_ENV_ENTRIES@};
]=] @ONLY)

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/gb_d3d11.c          # Direct3D 11 render back end (off unless GB_D3D11 is set)
    src/gb_overlay.c        # start screen, corner mark, frame counter and screenshots over the Direct3D 11 picture
    src/gb_save.c           # save folder, backups, save-write guard, log file, fatal-error box
    src/gb_pad.c            # host controllers behind the emulated pads; replaces the toolkit's xinput_device.c at link time (GB_PADS)
    src/gb_pad_logic.c      # its rules (players, mapping, settings file); tested by tests/pad_logic_test.c
    src/gb_cpu.c            # thread placement on processors with performance and efficiency cores
    src/gb_cpu_logic.c      # its decision; tested by tests/cpu_logic_test.c
    src/gb_bgsound.c        # the game's sound while its window is not in front (GB_BG_SOUND)
    src/gb_sticky.c         # Windows' Sticky Keys shortcut off while the window is in front
    src/gb_mods.c           # the player's replacement files from the mods folder (GB_MODS, GB_MODS_DIR)
    src/gb_mouse.c          # the mouse in menus (GB_MOUSE_MENUS)
    src/gb_textures.c       # mods' pictures drawn in place of game textures by fingerprint; the maker's texture dump
    res/gamebreaker_game.rc # the program's icon and version information
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

# The resource compiler does not report the icon as a dependency of the .rc,
# so without this a changed icon alone rebuilds nothing.
set_source_files_properties(res/gamebreaker_game.rc PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/gamebreaker_game.ico)

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# The version shown on the start screen. A release can set its own.
# Defined only for the files that use it, so a new version recompiles those
# files and not the whole game: an update then compiles only what changed.
set(GB_VERSION "0.1.0" CACHE STRING "Version shown on the start screen")
set_property(SOURCE src/gb_overlay.c src/gb_save.c APPEND PROPERTY
    COMPILE_DEFINITIONS "GB_VERSION_STR=\"${GB_VERSION}\"")
# The program's version information (Properties, Details) follows it too. The
# resource compiler drops quotes given on its command line, so the values go
# through a small generated header (rewritten only when they change).
set(GB_VER_NUM "0,0,0,0")
if(GB_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(GB_VER_NUM "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gb_version_rc.h
     CONTENT "#define GB_VERSION_STR \"@GB_VERSION@\"\n#define GB_VER_NUM @GB_VER_NUM@\n" @ONLY)
set_property(SOURCE res/gamebreaker_game.rc APPEND PROPERTY
    OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gb_version_rc.h)


# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${GB_GEN_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}     # gb_run_env.h
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    ole32                   # the audio session API (gb_bgsound.c)
    d3d11
    d3dcompiler             # gb_d3d11.c compiles its HLSL at start-up
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# The controller layer uses the toolkit's input headers (input_map.h,
# keyboard.h, xinput_xbox.h and the platform/ header they include). Only these
# two files get the paths, so nothing else can pick up a toolkit header by name.
set_source_files_properties(src/gb_pad.c src/gb_pad_logic.c PROPERTIES
    INCLUDE_DIRECTORIES "${XBOXRECOMP_DIR}/src/input;${XBOXRECOMP_DIR}/src")

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # Symbol map next to the executable (gamebreaker.map). Costs nothing at
        # run time; port/tools/perf/rip_sample.ps1 uses it to turn sampled
        # instruction addresses into function names.
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    # /MP: compile this target's files in parallel, one compiler per core.
    # Without it Visual Studio compiles the ~70 generated files one at a time.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP)
else()
    # Clang or GCC targeting Windows (MinGW-w64): llvm-mingw is the default
    # compiler (the one setup downloads) and builds the same program as MSVC.
    # -O2 matches MSVC's /O2 (CMake's Release default here would be -O3).
    target_compile_options(${PROJECT_NAME} PRIVATE -O2)
    target_link_options(${PROJECT_NAME} PRIVATE
        # Keep the program itself above 4 GB, as MSVC's linker does, so the
        # Xbox address space can be mapped at its usual low addresses.
        -Wl,--image-base,0x140000000 -Wl,--dynamicbase -Wl,--high-entropy-va
        # Microsoft-format symbols (gamebreaker.pdb) for the fault handler.
        -Wl,--pdb=
        # Symbol map next to the executable, as /MAP does for MSVC; the
        # profiling and stall-dump tools turn addresses into names with it.
        -Wl,--Map=gamebreaker.map)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
