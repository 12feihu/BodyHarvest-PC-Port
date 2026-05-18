cmake_minimum_required(VERSION 3.20)
project(bh_app LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the CRT used by librecomp/ultramodern (/MD = MultiThreadedDLL).
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# ----------------------------------------------------------------------------
# Phase 1 target: bh_recomp
#
# A static library containing all 3915 functions emitted by N64Recomp from
# Body Harvest's ELF. This is just compile validation at this point — nothing
# is linked against it yet.
# ----------------------------------------------------------------------------

set(BH_RECOMP_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bh-recomp/RecompiledFuncs"
    CACHE PATH "Directory containing N64Recomp's generated .c files for BH")

# CONFIGURE_DEPENDS so re-running N64Recomp (e.g. after a TOML change)
# triggers a reconfigure on the next build.
file(GLOB BH_RECOMP_SOURCES CONFIGURE_DEPENDS "${BH_RECOMP_OUTPUT_DIR}/*.c")

if (BH_RECOMP_SOURCES STREQUAL "")
    message(FATAL_ERROR
        "No .c files found under ${BH_RECOMP_OUTPUT_DIR}. "
        "Run N64Recomp first: see ../RESUME.md.")
endif()

list(LENGTH BH_RECOMP_SOURCES BH_RECOMP_SOURCE_COUNT)
message(STATUS "bh_recomp: ${BH_RECOMP_SOURCE_COUNT} generated .c files")

# Generate forward declarations for `*_recomp` functions that the runtime
# implements but N64Recomp doesn't declare in funcs.h. We force-include the
# resulting header into every recompiled .c file (via /FI on MSVC-style
# drivers including clang-cl).
set(BH_RECOMP_EXTRAS_H "${CMAKE_CURRENT_BINARY_DIR}/generated/bh_recomp_extras.h")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python "${CMAKE_CURRENT_SOURCE_DIR}/gen_recomp_extras.py"
        "${BH_RECOMP_OUTPUT_DIR}" "${BH_RECOMP_EXTRAS_H}"
    RESULT_VARIABLE _gen_extras_rc
    OUTPUT_VARIABLE _gen_extras_out
    ERROR_VARIABLE  _gen_extras_err
)
if (NOT _gen_extras_rc EQUAL 0)
    message(FATAL_ERROR "gen_recomp_extras.py failed (${_gen_extras_rc}):\n${_gen_extras_out}\n${_gen_extras_err}")
endif()
message(STATUS "bh_recomp_extras.h: ${_gen_extras_out}")

add_library(bh_recomp STATIC ${BH_RECOMP_SOURCES})

target_include_directories(bh_recomp PUBLIC
    # For funcs.h (sibling of the funcs_*.c files)
    "${BH_RECOMP_OUTPUT_DIR}"
    # For recomp.h (defines RECOMP_FUNC, recomp_context, the int128 helpers,
    # the LOOKUP_FUNC macro signature, etc.)
    "${CMAKE_CURRENT_SOURCE_DIR}/../N64ModernRuntime/N64Recomp/include"
    # For the generated bh_recomp_extras.h (force-included below)
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# Force-include the extras header in every .c so the missing `*_recomp`
# prototypes are visible before the call sites. clang-cl uses /FI just like
# MSVC; pure MSVC would too if we ever fall back.
if (MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
    target_compile_options(bh_recomp PRIVATE "/FI${BH_RECOMP_EXTRAS_H}")
else()
    target_compile_options(bh_recomp PRIVATE "-include" "${BH_RECOMP_EXTRAS_H}")
endif()

# ----------------------------------------------------------------------------
# Phase 2 target: bh_app_glue
#
# C++ glue that wraps the auto-generated section/overlay tables and registers
# them with librecomp. Also picks up the auto-generated lookup.cpp
# (get_entrypoint_address / get_rom_name).
# ----------------------------------------------------------------------------

set(BH_GLUE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/register_overlays.cpp"
    "${BH_RECOMP_OUTPUT_DIR}/lookup.cpp"
)

add_library(bh_app_glue STATIC ${BH_GLUE_SOURCES})

target_include_directories(bh_app_glue PUBLIC
    # Our own headers
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # recomp.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../N64ModernRuntime/N64Recomp/include"
    # recomp_overlays.inl + funcs.h (so the .inl's `#include "funcs.h"` resolves)
    "${BH_RECOMP_OUTPUT_DIR}"
    # librecomp public headers (librecomp/overlays.hpp, sections.h, etc)
    "${CMAKE_CURRENT_SOURCE_DIR}/../N64ModernRuntime/librecomp/include"
)

target_link_libraries(bh_app_glue PUBLIC bh_recomp)

# bh_app.hpp pulls in ultramodern's renderer_context.hpp which transitively
# needs json.hpp (under thirdparty/json/). Add both.
target_include_directories(bh_app_glue PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../N64ModernRuntime/ultramodern/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../N64ModernRuntime/thirdparty"
)

# ----------------------------------------------------------------------------
# Import the prebuilt N64ModernRuntime libraries (built standalone via
# build-n64modernruntime.bat). We avoid add_subdirectory to keep bh-app's
# configure/build fast.
# ----------------------------------------------------------------------------

set(NMR_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../N64ModernRuntime/build")
set(NMR_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}/../N64ModernRuntime")

function(import_static_lib name lib_path include_dirs)
    add_library(${name} STATIC IMPORTED GLOBAL)
    set_target_properties(${name} PROPERTIES
        IMPORTED_LOCATION "${lib_path}"
        INTERFACE_INCLUDE_DIRECTORIES "${include_dirs}"
    )
endfunction()

import_static_lib(nmr_librecomp   "${NMR_BUILD}/librecomp/librecomp.lib"
    "${NMR_ROOT}/librecomp/include;${NMR_ROOT}/ultramodern/include;${NMR_ROOT}/N64Recomp/include;${NMR_ROOT}/thirdparty;${NMR_ROOT}/thirdparty/concurrentqueue;${NMR_ROOT}/thirdparty/o1heap;${NMR_ROOT}/thirdparty/xxHash;${NMR_ROOT}/thirdparty/miniz")
import_static_lib(nmr_ultramodern "${NMR_BUILD}/ultramodern/ultramodern.lib"
    "${NMR_ROOT}/ultramodern/include;${NMR_ROOT}/thirdparty;${NMR_ROOT}/thirdparty/concurrentqueue;${NMR_ROOT}/thirdparty/sse2neon")
import_static_lib(nmr_n64recomp   "${NMR_BUILD}/librecomp/N64Recomp/N64Recomp.lib"      "${NMR_ROOT}/N64Recomp/include")
import_static_lib(nmr_liverecomp  "${NMR_BUILD}/librecomp/N64Recomp/LiveRecomp.lib"     "${NMR_ROOT}/N64Recomp/include")
import_static_lib(nmr_symlists    "${NMR_BUILD}/librecomp/N64Recomp/SymbolLists.lib"    "${NMR_ROOT}/N64Recomp/include")
import_static_lib(nmr_rabbitizer  "${NMR_BUILD}/librecomp/N64Recomp/rabbitizer.lib"     "${NMR_ROOT}/N64Recomp/lib/rabbitizer/include")
import_static_lib(nmr_fmt         "${NMR_BUILD}/librecomp/N64Recomp/lib/fmt/fmt.lib"    "${NMR_ROOT}/N64Recomp/lib/fmt/include")
import_static_lib(nmr_miniz       "${NMR_BUILD}/librecomp/miniz/miniz.lib"              "${NMR_ROOT}/thirdparty/miniz")

# ----------------------------------------------------------------------------
# Import the prebuilt RT64 libraries.
# ----------------------------------------------------------------------------

set(RT64_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../rt64/build")
set(RT64_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}/../rt64")

# rt64.lib gathers everything in rt64/src (hle/, render/, common/, gui/, ...).
# Include both src/ and src/contrib so internal headers resolve.
import_static_lib(rt64_main "${RT64_BUILD}/rt64.lib"
    "${RT64_ROOT}/src;${RT64_ROOT}/src/contrib;${RT64_ROOT}/src/contrib/plume;${RT64_ROOT}/src/contrib/hlslpp/include;${RT64_ROOT}/src/contrib/dxc/inc")
import_static_lib(rt64_plume     "${RT64_BUILD}/src/contrib/plume/plume.lib"
    "${RT64_ROOT}/src/contrib/plume;${RT64_ROOT}/src/contrib/plume/contrib/volk;${RT64_ROOT}/src/contrib/plume/contrib/Vulkan-Headers/include;${RT64_ROOT}/src/contrib/plume/contrib/VulkanMemoryAllocator/include;${RT64_ROOT}/src/contrib/plume/contrib/D3D12MemoryAllocator/include")
import_static_lib(rt64_respirv   "${RT64_BUILD}/src/contrib/re-spirv/re-spirv.lib"
    "${RT64_ROOT}/src/contrib/re-spirv;${RT64_ROOT}/src/contrib/re-spirv/external/SPIRV-Headers/include")
import_static_lib(rt64_zstd      "${RT64_BUILD}/src/contrib/zstd/build/cmake/lib/zstd_static.lib"
    "${RT64_ROOT}/src/contrib/zstd/lib")
import_static_lib(rt64_nfd       "${RT64_BUILD}/src/contrib/nativefiledialog-extended/src/nfd.lib"
    "${RT64_ROOT}/src/contrib/nativefiledialog-extended/src/include")

# SDL2: RT64's hle/rt64_application_window.h transitively includes <SDL.h>,
# so we need SDL2 headers on the include path AND the import library linked
# even though we don't (yet) use SDL ourselves.
set(SDL2_DIR "${RT64_ROOT}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
add_library(sdl2 SHARED IMPORTED GLOBAL)
set_target_properties(sdl2 PROPERTIES
    IMPORTED_LOCATION             "${SDL2_DIR}/lib/x64/SDL2.dll"
    IMPORTED_IMPLIB               "${SDL2_DIR}/lib/x64/SDL2.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_DIR}/include"
)

# ----------------------------------------------------------------------------
# Phase 3a target: bh_app.exe
#
# A console app whose only job is to LINK. It calls bh::register_overlays
# and then recomp::start with stub callbacks. At runtime it will probably
# crash on the first ROM-load attempt; that's fine for this phase.
# ----------------------------------------------------------------------------

set(BH_ASPMAIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../bh-rsp/aspMain.cpp")

add_executable(bh_app
    src/main.cpp
    src/bh_stubs.cpp
    src/bh_renderer.cpp
    src/bh_cheats.cpp
    src/bh_net.cpp
    # Phase 5: recompiled aspMain audio RSP microcode (output of RSPRecomp on
    # bh-rsp/aspMain.toml). Lives outside src/ so it isn't conflated with
    # hand-written code.
    ${BH_ASPMAIN_SRC}
)

# librecomp's rsp_vu_impl.hpp uses SSE4.1 intrinsics (_mm_blendv_epi8 etc.)
# unconditionally. clang-cl defaults to no SSE4.1 baseline — opt aspMain.cpp
# (the only file that pulls rsp_vu_impl.hpp in) up to a Nehalem ISA. Same
# trick Zelda64Recomp uses; -march works under clang-cl despite its MSVC
# driver persona.
if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(${BH_ASPMAIN_SRC} PROPERTIES
        COMPILE_OPTIONS "-march=nehalem")
endif()

# RT64 includes <intrin.h> et al that want NOMINMAX and we pull <windows.h>
# in main.cpp anyway.
target_compile_definitions(bh_app PRIVATE
    NOMINMAX
    HLSL_CPU
)

# Debug info even in Release builds — we want windbg/VS to resolve our
# stack frames to function names. /Z7 embeds debug info in .obj (works
# better with parallel builds than /Zi); /DEBUG tells lld-link to emit
# a .pdb and write the debug-directory entry into the exe so debuggers
# can find it.
if (MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
    target_compile_options(bh_app PRIVATE /Z7)
    target_link_options(bh_app PRIVATE /DEBUG)
endif()

# Prepend the Agility SDK headers so RT64's plume sees D3D12_HEAP_TYPE_GPU_UPLOAD.
target_include_directories(bh_app PRIVATE
    "C:/Tools/AgilitySDK/build/native/include"
)

target_link_libraries(bh_app PRIVATE
    bh_app_glue
    bh_recomp
    # SDL2 (RT64 headers transitively include SDL.h)
    sdl2
    # RT64 + transitive deps (order matters for static-only link; the
    # plume/respirv/zstd/nfd come after rt64_main since they satisfy
    # references rt64_main pulls in.)
    rt64_main
    rt64_plume
    rt64_respirv
    rt64_zstd
    rt64_nfd
    # N64ModernRuntime
    nmr_librecomp
    nmr_ultramodern
    nmr_liverecomp
    nmr_n64recomp
    nmr_symlists
    nmr_rabbitizer
    nmr_fmt
    nmr_miniz
    # Windows graphics + system
    d3d12
    dxgi
    dxguid
    "${RT64_ROOT}/src/contrib/dxc/lib/x64/dxcompiler.lib"
    winmm
    ws2_32
    # XInput for controller input (Phase 9). Using xinput9_1_0 instead of
    # xinput1_4 because the Windows 10 SDK 10.0.19041 we link against
    # doesn't ship the 1_4 import lib by default, and 9_1_0 covers every
    # gamepad we care about (Xbox 360/One/Series, DS4 via DS4Windows, etc.).
    xinput9_1_0
    # XAudio2 for audio output (Phase 11). The static import lib lives in
    # the Windows 10 SDK alongside the rest; ole32 is required because
    # XAudio2 is a COM object and needs CoInitializeEx + IID handling.
    xaudio2
    ole32
    advapi32
    user32
    gdi32
    dbghelp
    shcore   # SetProcessDpiAwareness (RT64 calls this in ApplicationWindow::setup)
    comctl32 # Phase 14: InitCommonControlsEx for the cheats-overlay window
    ws2_32   # Phase 16: Winsock for the multiplayer POC (bh_net.cpp)
)

# RT64 ships DXC at runtime via dxcompiler.dll + dxil.dll. Drop them next to
# the exe along with SDL2.dll so the loader finds everything on launch.
add_custom_command(TARGET bh_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RT64_ROOT}/src/contrib/dxc/bin/x64/dxcompiler.dll"
        "${RT64_ROOT}/src/contrib/dxc/bin/x64/dxil.dll"
        "${SDL2_DIR}/lib/x64/SDL2.dll"
        $<TARGET_FILE_DIR:bh_app>
)

# The generated code is verbose by design and trips a lot of clang's "you
# wrote `lo`/`hi`/`c1cs` but never read it" diagnostics. Silence them so the
# build output stays readable.
if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(bh_recomp PRIVATE
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-function
        -Wno-unused-label
        -Wno-unused-parameter
        -Wno-tautological-compare
        -Wno-parentheses-equality
        -Wno-shift-count-overflow
        -Wno-constant-conversion
        -Wno-implicit-int-conversion
        -Wno-shorten-64-to-32
    )
endif()
