cmake_minimum_required(VERSION 3.20)
# Keep in step with the changelog in README.md. This sat at 0.3.0 until August
# 2026 while the project shipped 0.6.x through 0.8.0, so every build printed
# "ps3recomp runtime v0.3.0" and was believed. v0.9.0 is the first TAGGED one.
project(ps3recomp_runtime
    VERSION 0.18.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PS3RecompSanitizers.cmake")

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
    "runtime/diag/*.c"      # yz_frontier_trace, used by the live draw engine
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# The Vulkan RSX backend is opt-in while it grows (stage V0: clear + present).
# It loads libvulkan at run time, so building needs only the Vulkan headers --
# no target-architecture libvulkan, which keeps cross-compiling simple.
option(PS3RECOMP_RSX_VULKAN "Build the Vulkan RSX backend (Linux)" OFF)
list(FILTER HLE_SOURCES EXCLUDE REGEX "rsx_vulkan_backend\\.c$")
if(PS3RECOMP_RSX_VULKAN)
    if(WIN32 OR APPLE)
        message(FATAL_ERROR "PS3RECOMP_RSX_VULKAN is Linux-only for now")
    endif()
    find_path(PS3RECOMP_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if(NOT PS3RECOMP_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Vulkan headers not found. Debian/Ubuntu: "
            "libvulkan-dev, Fedora: vulkan-headers -- or pass "
            "-DPS3RECOMP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>")
    endif()
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_vulkan_backend.c")
    # HLSL -> SPIR-V for guest programs: a stub unless glslang is found below.
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp")
endif()

# The Metal backend is Objective-C and Apple-only, so it is added explicitly
# rather than globbed. It needs no platform #ifdef of its own as a result.
if(APPLE)
    enable_language(OBJC)
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m")
endif()
# The HLSL -> MSL translator is C++ (it drives glslang's C++ API) and compiles
# to a stub wherever the shader tools are absent, so it is part of the
# library everywhere. See the "Guest shaders on Metal" section below.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_msl.cpp")
# The RSX capture writer (rsx_capture.h): C++ for the JSON and file handling. The walker and the
# engine reach it only through a registered sink, so it is optional for any link that skips it.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_capture.cpp")
# cellAtrac decodes with FFmpeg's ATRAC3plus decoder as extracted by PPSSPP
# (LGPL-2.1, kept separate in third_party/at3_standalone -- see its README).
file(GLOB AT3_SOURCES "third_party/at3_standalone/*.cpp")
set_source_files_properties(${AT3_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")  # upstream code
list(APPEND HLE_SOURCES ${AT3_SOURCES})

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h (and the game's RSX/D3D12
#    backend), not as part of the game-agnostic runtime library. See
#    docs/PPU_RECOMP.md.
#  - spu_dispatch_mt.c: the musttail SPU indirect-branch fast path. It uses
#    __attribute__((musttail)), which MSVC cannot parse, and it must be compiled
#    *inside the game build* by clang so the tail call is guaranteed -- a plain
#    call leaks one resolver frame per guest loop iteration. A port adds it to
#    its own target; see templates/project/CMakeLists.txt.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/|/spu_dispatch_mt\.c$")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
# The Jq2 command-ring codec and the reference scheduler are pure C++20, so
# they are a target of their own (ps3recomp_spurs_jq2_model): their T0 tests
# link them alone, and a title can use them without the HLE runtime.
#
# ps3recomp_spurs_jq2 is the guest-facing half: the port line (port.cpp, the
# guest memory base and the reservation model) and the HLE executor
# (executor.cpp: the guest queue, the SPU workers, the LV2 event flags, the
# SPURS job loader, named stops). The runtime needs it (cellSpursJq.c calls
# jq2/jq2.h) and it needs the runtime.
#
# The sources are compiled once, as OBJECT libraries, and their objects go into
# BOTH the standalone static libraries (the T0 tests link those) and the
# runtime archive itself. The installed toolkit ships one archive,
# libps3recomp_runtime.a, and a title links only that (docs/BUILDING.md,
# "Linking Against the Runtime"): it must carry every jq2_* definition its HLE
# objects call. The standalone libraries come first on an in-tree link line, so
# the duplicate members in the runtime archive are never pulled.
add_library(ps3recomp_spurs_jq2_model_objs OBJECT
    libs/spurs/jq2/ring.cpp
    libs/spurs/jq2/sched.cpp
)
target_include_directories(ps3recomp_spurs_jq2_model_objs PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
)
add_library(ps3recomp_spurs_jq2_model STATIC
    $<TARGET_OBJECTS:ps3recomp_spurs_jq2_model_objs>
)
target_include_directories(ps3recomp_spurs_jq2_model PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
)

add_library(ps3recomp_spurs_jq2_objs OBJECT
    libs/spurs/jq2/port.cpp
    libs/spurs/jq2/pool.cpp
    libs/spurs/jq2/executor.cpp
)
target_include_directories(ps3recomp_spurs_jq2_objs PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
)
target_include_directories(ps3recomp_spurs_jq2_objs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
add_library(ps3recomp_spurs_jq2 STATIC
    $<TARGET_OBJECTS:ps3recomp_spurs_jq2_objs>
)
target_include_directories(ps3recomp_spurs_jq2 PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
)
find_package(Threads REQUIRED)
target_link_libraries(ps3recomp_spurs_jq2 PUBLIC ps3recomp_spurs_jq2_model Threads::Threads)
target_link_libraries(ps3recomp_spurs_jq2 PRIVATE ps3recomp_runtime)

add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
    $<TARGET_OBJECTS:ps3recomp_spurs_jq2_model_objs>
    $<TARGET_OBJECTS:ps3recomp_spurs_jq2_objs>
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# What a capture's environment record says about this build. Fixed when CMake configures: a commit
# made afterwards is not in it, so a baseline is only comparable to a build configured at the same
# state. "-dirty" marks tracked changes in the work tree at that moment.
set(PS3RECOMP_TOOLKIT_SHA "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _ps3_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _ps3_sha_rc)
    if(_ps3_sha_rc EQUAL 0 AND _ps3_sha)
        set(PS3RECOMP_TOOLKIT_SHA "${_ps3_sha}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _ps3_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_ps3_dirty)
            set(PS3RECOMP_TOOLKIT_SHA "${_ps3_sha}-dirty")
        endif()
    endif()
endif()
if(CMAKE_BUILD_TYPE)
    set(PS3RECOMP_BUILD_TYPE_NAME "${CMAKE_BUILD_TYPE}")
else()
    set(PS3RECOMP_BUILD_TYPE_NAME "unspecified")
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_capture.cpp" PROPERTIES
    COMPILE_DEFINITIONS "PS3RECOMP_TOOLKIT_SHA=\"${PS3RECOMP_TOOLKIT_SHA}\";PS3RECOMP_BUILD_TYPE=\"${PS3RECOMP_BUILD_TYPE_NAME}\"")

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
    )
else()
    # cellPad and cellAudio select their SDL2 backends unconditionally off
    # Windows (see PAD_BACKEND_SDL2 / AUDIO_BACKEND_SDL2), so SDL2 is required
    # here -- docs/BUILDING.md already lists it as the Linux/macOS dependency.
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        if(TARGET SDL2::SDL2)
            target_link_libraries(ps3recomp_runtime PRIVATE SDL2::SDL2)
        else()
            target_include_directories(ps3recomp_runtime PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(ps3recomp_runtime PRIVATE ${SDL2_LIBRARIES})
        endif()
        message(STATUS "  SDL2:         found (input + audio backends enabled)")
    else()
        message(FATAL_ERROR
            "SDL2 not found. It provides the cellPad and cellAudio backends on "
            "non-Windows hosts.\n"
            "  macOS:  brew install sdl2\n"
            "  Debian: apt install libsdl2-dev\n"
            "If SDL2 is installed in a non-standard prefix, pass "
            "-DCMAKE_PREFIX_PATH=<prefix>.")
    endif()

    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)
endif()

if(APPLE)
    target_link_libraries(ps3recomp_runtime PUBLIC
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework AppKit"
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    message(STATUS "  Metal:        RSX backend enabled (rsx_metal_backend.m)")
endif()

if(PS3RECOMP_RSX_VULKAN)
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_VULKAN_INCLUDE_DIR}")
    # PUBLIC: ps3recomp_host selects its backend on this definition.
    target_compile_definitions(ps3recomp_runtime PUBLIC PS3RECOMP_RSX_VULKAN)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  Vulkan:       RSX backend enabled (rsx_vulkan_backend.c, headers in ${PS3RECOMP_VULKAN_INCLUDE_DIR})")
endif()

# cellVdec decodes H.264 / MPEG-2 through FFmpeg when this is on. libavcodec
# is loaded at run time (dlopen / LoadLibrary), never linked, so building needs
# only the FFmpeg headers, and a binary run where FFmpeg is absent -- or with
# PS3RECOMP_FFMPEG=0 -- keeps the callback-only decoders. FFmpeg stays a
# separate, user-supplied LGPL library. See libs/codec/ffmpeg_runtime.h.
option(PS3RECOMP_FFMPEG_RUNTIME "Decode media through FFmpeg loaded at run time (needs the FFmpeg headers)" OFF)
if(PS3RECOMP_FFMPEG_RUNTIME)
    find_path(PS3RECOMP_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
    if(NOT PS3RECOMP_FFMPEG_INCLUDE_DIR)
        message(FATAL_ERROR "FFmpeg headers not found. Debian/Ubuntu: "
            "libavcodec-dev, Fedora: ffmpeg-free-devel, MSYS2: mingw-w64-x86_64-ffmpeg "
            "-- or pass -DPS3RECOMP_FFMPEG_INCLUDE_DIR=<dir containing libavcodec/avcodec.h>")
    endif()
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_FFMPEG_INCLUDE_DIR}")
    target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_FFMPEG_RUNTIME)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  FFmpeg:       run-time loaded (headers in ${PS3RECOMP_FFMPEG_INCLUDE_DIR})")
endif()

# ---------------------------------------------------------------------------
# Guest shaders on Metal: HLSL -> SPIR-V -> MSL
# ---------------------------------------------------------------------------
# The RSX decompilers emit HLSL for the D3D12 backend. Metal cannot compile
# that, so libs/video/rsx_shader_msl.cpp lowers it with glslang (HLSL ->
# SPIR-V) and spirv-cross (SPIR-V -> MSL). Both are optional: without them the
# module compiles to a stub, rsx_hlsl_to_msl_available() says so, and the
# Metal backend stays on its fixed-function path. The translation has no GPU
# in it, so it is built and tested on Linux as well -- that is where the
# decompilers' HLSL meets glslang's front end on every push.
#
#   macOS:  brew install glslang spirv-cross
#   Debian: apt install glslang-dev glslang-tools spirv-tools \
#                       libspirv-cross-c-shared-dev
#           (glslang-tools only because the packaged CMake config declares
#           the standalone binary as an imported target and fails to load
#           without it)
set(PS3RECOMP_MSL_TRANSLATION OFF)
if(UNIX)
    find_package(glslang CONFIG QUIET)

    # spirv-cross's C API. Debian ships it as a shared library and Homebrew as
    # a static one, under different pkg-config names -- and pkg-config itself
    # is not a given on a Mac, so fall back to a plain header + library search.
    set(SPVC_OK OFF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SPVC QUIET spirv-cross-c-shared)
        if(NOT SPVC_FOUND)
            pkg_check_modules(SPVC QUIET spirv-cross-c)
        endif()
        if(SPVC_FOUND)
            set(SPVC_INC     ${SPVC_INCLUDE_DIRS})
            set(SPVC_LIBS    ${SPVC_LINK_LIBRARIES})
            set(SPVC_LIBDIRS ${SPVC_LIBRARY_DIRS})
            set(SPVC_OK ON)
        endif()
    endif()
    if(NOT SPVC_OK)
        find_path(SPVC_INC_DIR spirv_cross_c.h PATH_SUFFIXES spirv_cross)
        find_library(SPVC_LIB NAMES spirv-cross-c-shared spirv-cross-c)
        if(SPVC_INC_DIR AND SPVC_LIB)
            set(SPVC_INC  ${SPVC_INC_DIR})
            set(SPVC_LIBS ${SPVC_LIB})
            get_filename_component(SPVC_LIBDIRS "${SPVC_LIB}" DIRECTORY)
            set(SPVC_OK ON)
        endif()
    endif()

    if(glslang_FOUND AND SPVC_OK
       AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        add_library(ps3recomp_msl_translation INTERFACE)
        target_compile_definitions(ps3recomp_msl_translation INTERFACE
            PS3RECOMP_HAVE_MSL_TRANSLATION=1)
        target_include_directories(ps3recomp_msl_translation INTERFACE ${SPVC_INC})
        target_link_libraries(ps3recomp_msl_translation INTERFACE
            glslang::glslang glslang::SPIRV ${SPVC_LIBS})
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_msl_translation INTERFACE
                glslang::glslang-default-resource-limits)
        endif()
        # A static spirv-cross C API (Homebrew's) needs the backends it was
        # built from, and neither pkg-config nor the library name says so:
        # pick up the static siblings next to it. The shared one carries
        # its own.
        if(SPVC_LIBS MATCHES "\\.a$")
            foreach(part msl glsl hlsl cpp reflect util core)
                find_library(SPVC_PART_${part} NAMES spirv-cross-${part}
                    HINTS ${SPVC_LIBDIRS} NO_DEFAULT_PATH)
                if(SPVC_PART_${part})
                    target_link_libraries(ps3recomp_msl_translation INTERFACE
                        ${SPVC_PART_${part}})
                endif()
            endforeach()
        endif()
        target_link_libraries(ps3recomp_runtime PRIVATE ps3recomp_msl_translation)
        set(PS3RECOMP_MSL_TRANSLATION ON)
        message(STATUS "  Shaders:      guest programs translated to MSL (glslang ${glslang_VERSION}, ${SPVC_LIBS})")
    else()
        message(STATUS "  Shaders:      glslang/spirv-cross not found; Metal backend stays fixed-function")
    endif()
endif()

# Guest shaders on Vulkan: HLSL -> SPIR-V. glslang only -- Vulkan consumes
# SPIR-V, so spirv-cross is not needed. Without glslang
# libs/video/rsx_shader_spirv.cpp is a stub and the Vulkan backend stays on
# its fixed-function path.
if(PS3RECOMP_RSX_VULKAN)
    if(glslang_FOUND AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp"
            PROPERTIES COMPILE_DEFINITIONS PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
        target_link_libraries(ps3recomp_runtime PRIVATE glslang::glslang glslang::SPIRV)
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_runtime PRIVATE
                glslang::glslang-default-resource-limits)
        endif()
        set(PS3RECOMP_SPIRV_TRANSLATION ON)
        message(STATUS "  Vulkan:       guest programs available (glslang ${glslang_VERSION}); "
                       "opt in with PS3RECOMP_VK_GUEST_PROGRAMS=1")
    else()
        message(STATUS "  Vulkan:       glslang not found; guest programs unavailable")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # No spontaneous fused multiply-add. MSVC never contracts, and x86-64
        # without -mfma has nothing to contract into, so every validated
        # result came from separate multiplies and adds; arm64 has FMA in
        # the base ISA and Clang fuses by default there. A PPC fmadd is
        # spelt as one where the lifter means one.
        -ffp-contract=off
        # Explicit, because the per-config defaults cannot be trusted: this
        # tree's caches carry CMAKE_C_FLAGS_RELEASE="" and CMAKE_CXX_FLAGS_RELEASE="",
        # and clang-cl with no /O flag compiles at /Od -- a "Release" runtime
        # was unoptimised without a word. Duplicating a default -O2 is harmless.
        # Every configuration but Debug, including none: a port configured
        # without CMAKE_BUILD_TYPE matched neither name and compiled the whole
        # runtime -- the FIFO walker, the draw engine, the MFC -- at -O0.
        $<$<NOT:$<CONFIG:Debug>>:-O2>
    )
    # SSE4.1 (any x86-64 since ~2008): pshufb/blendv for spu_shufb, the
    # hottest SPU op in lifted audio code. Probed rather than assumed: recent
    # Apple clang rejects the flag outright for arm64 ("unsupported option
    # '-msse4.1'"), which stops the whole runtime from building there.
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-msse4.1 PS3RECOMP_HAS_MSSE41)
    if(PS3RECOMP_HAS_MSSE41)
        target_compile_options(ps3recomp_runtime PRIVATE -msse4.1)
    endif()
elseif(MSVC)
    # Real cl.exe -- clang-cl reports its id as Clang and is handled above.
    #
    # This does not currently build. The runtime and the lifted output use GCC/
    # Clang builtins that cl has no equivalent for, and the failure lands as a
    # wall of errors in whichever file the build happens to reach first rather
    # than as anything that names the cause. Say it here instead.
    message(WARNING
        "Building with cl.exe (MSVC). This is not currently supported.\n"
        "  The runtime uses __atomic_* builtins, __int128 and __builtin_bswap*, "
        "and the lifted output uses __int128 for the PPC 64x64 multiplies. "
        "cl has none of them.\n"
        "  Use clang-cl instead -- it consumes the MSVC headers and libraries, "
        "so nothing else about a Visual Studio build changes:\n"
        "    cmake -S . -B build -G Ninja "
        "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl\n"
        "  See docs/BUILDING.md. Porting the intrinsics to cl is a welcome "
        "contribution; nothing else stands in the way.")
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(NOT MSVC AND NOT WIN32)
    # dladdr/Dl_info (runtime/platform/win32_backtrace.h) are GNU extensions
    # that glibc hides unless _GNU_SOURCE is set. g++ defines it implicitly,
    # so the C++ boot scaffold never noticed; a C file including the same
    # shim does not get it for free.
    target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    # Every test carries an area label and a tier label:
    #   t0      fast and deterministic: the default set (ctest -L t0)
    #   stress  long-running or iteration-heavy
    #   tsan    worth a run under PS3RECOMP_SANITIZE_THREAD
    # Subdirectories label the tests they add.
    # sync_stress is written directly against the Win32 threading API
    # (WaitForMultipleObjects, joinable HANDLEs, Interlocked*, CRITICAL_SECTION).
    # Off Windows all of that comes from runtime/platform/win32_compat.h, so
    # the suite runs everywhere -- and is the arm64 memory-model regression
    # net for the lv2 sync primitives.
    add_subdirectory(tests/sync_stress)
    # cellFiber switches contexts with Windows fibers on Windows and ucontext
    # everywhere else. Apple deprecated the ucontext routines and gates both
    # them and the shape of ucontext_t on _XOPEN_SOURCE, so whether that path
    # works on arm64 is a question to answer rather than assume.
    add_subdirectory(tests/fiber_switch)
    add_subdirectory(tests/sync2_semaphore)
    # Differential harness (HLE vs lifted firmware); inert unless PS3RECOMP_DIFF_FW_DIR names a
    # lifted module kept outside the repository (tests/diff_harness/CMakeLists.txt).
    add_subdirectory(tests/diff_harness)
    if(UNIX)
        add_subdirectory(tests/jq2_fiber)
    endif()
    # Syscall 14's address/flags classification as a plain unit test: the
    # check is pure, so the test links one runtime translation unit
    # (lv2_lock_line.c) and nothing else.
    add_subdirectory(tests/syscalls)
    # The JobQueue2 ring-entry codec: synthetic bytes only, linked without the
    # runtime so the target's standalone contract is exercised too.
    add_subdirectory(tests/spurs_jq2)
    # cellVdec through its guest API: the callback-only decoder always, and
    # real H.264 / MPEG-2 decode when FFmpeg is on (libs/codec/tests).
    add_subdirectory(libs/codec/tests)
    # cellGcm's FIFO walker without a backend: where GET_REPORT writes for
    # each report DMA context. Links the runtime like the POSIX host below.
    if(UNIX)
        add_subdirectory(tests/gcm_reports)
        # _cellGcmInitBody's command-buffer layout, segment callback, NV406E acquire, ordered
        # user commands.
        add_subdirectory(tests/gcm_initbody)
        # cellResc error numbering and the VideoOut resolution-id mapper.
        add_subdirectory(tests/resc)
        # cellGcmGetTiledPitchSize against libgcm_sys's pitch table.
        add_subdirectory(tests/gcm_pitch)
        # video.resolutions and the cellVideoOut answers that follow it.
        add_subdirectory(tests/video_resolutions)
    endif()
    # The startup gates (typed config, identity, VSCR policy) and the named stop
    # they end in: probe executables run per scenario by Python runners.
    add_subdirectory(tests/gates)
    # sys_lwmutex_* / sys_lwcond_* through ps3_hle_call: the guest-visible
    # contract, and the message-passing and lost-wakeup litmus tests. They link
    # the runtime library, so they run the handlers a title links.
    if(UNIX)
        add_subdirectory(tests/lwsync)
        # The HLE call observer ps3_hle_call reports to (hle_observer.h).
        add_subdirectory(tests/hle_observer)
        # The reservation model: PPU lwarx/stwcx. against SPU GETLLAR/PUTLLC,
        # both directions, and the JQ2 port line under load.
        add_subdirectory(tests/reservation)
    endif()
    # PS3_LOG_FIRST, the saturating counter behind the "first n times" log lines.
    add_executable(test_log_first tests/test_log_first.c)
    target_include_directories(test_log_first PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME log_first COMMAND test_log_first)
    set_tests_properties(log_first PROPERTIES LABELS "runtime;t0")

    # cellSync.h from C++, with <stdatomic.h> included first: the generated NID
    # table includes the header that way and no other in-tree TU did, so this
    # keeps the C++ spelling -- and the field layout both languages share --
    # compiling (and it links nothing; the C file is the implementation).
    add_executable(test_cell_sync_header tests/test_cell_sync_header.cpp)
    target_include_directories(test_cell_sync_header PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync")
    add_test(NAME cell_sync_header COMMAND test_cell_sync_header)
    set_tests_properties(cell_sync_header PROPERTIES LABELS "hle;t0")

    # The sysPrxForUser CRT shims bound the guest spans they copy: a span that
    # leaves the arena is a named stop (hle-guest-range) raised before the copy,
    # not a write past guest memory. The fixture maps an arena with a PROT_NONE
    # guard page after it, so a copy that gets through faults instead. Compiled
    # against the shim's own source (the rest of the runtime asks for a whole
    # host layer) with the real stop implementation.
    add_executable(test_sysprx_guest_range
        tests/test_sysprx_guest_range.c
        libs/system/sysPrxForUser.c
        runtime/diag/stop.cpp)
    target_include_directories(test_sysprx_guest_range PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
    set_target_properties(test_sysprx_guest_range PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME sysprx_guest_range COMMAND test_sysprx_guest_range)
    set_tests_properties(sysprx_guest_range PROPERTIES LABELS "runtime;t0" TIMEOUT 60)

    # sys_fs_read/sys_fs_write hand the host a guest span and the 8-byte
    # nread/nwrite out-pointers. A span that leaves guest memory, or a size
    # whose u64 bound the old (uint32_t) truncation lost, is a named stop
    # (hle-guest-range) raised before the transfer; a bad fd is CELL_EBADF
    # before any pointer is looked at. The fixture maps an arena with a
    # PROT_NONE guard page after it, so a check that misses faults instead,
    # and it links the real stop implementation.
    add_executable(test_sys_fs_guest_range
        tests/test_sys_fs_guest_range.c
        runtime/syscalls/sys_fs.c
        libs/filesystem/edat.c
        runtime/diag/stop.cpp)
    target_include_directories(test_sys_fs_guest_range PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/syscalls")
    set_target_properties(test_sys_fs_guest_range PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME sys_fs_guest_range COMMAND test_sys_fs_guest_range)
    set_tests_properties(sys_fs_guest_range PROPERTIES LABELS "runtime;t0" TIMEOUT 60)

    # A guest exit ends the host process while the PPU, SPU, RSX and audio host
    # threads are still running, so the runtime exits with quick_exit(): no
    # C++ static destructor runs under a live thread. The fixture pairs such a
    # destructor with a host thread that keeps writing a global, and checks
    # that its quick-exit diagnostic reaches a pipe -- quick_exit flushes
    # nothing, so the handler flushes its own output. See ps3emu/host_exit.h.
    add_executable(test_quick_exit
        tests/test_quick_exit.c
        tests/test_quick_exit_static.cpp
        runtime/host/host_stubs.c)
    target_include_directories(test_quick_exit PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform")
    target_link_libraries(test_quick_exit PRIVATE ps3recomp_runtime)
    set_target_properties(test_quick_exit PROPERTIES LINKER_LANGUAGE CXX)

    if(NOT WIN32)
        # The vector single-precision ops against the exact per-lane references.
        add_executable(test_spu_float_exact
            runtime/spu/tests/test_spu_float_exact.c)
        target_include_directories(test_spu_float_exact PRIVATE include runtime/spu)
        target_compile_options(test_spu_float_exact PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_float_exact PRIVATE m)
        add_test(NAME spu_float_exact COMMAND test_spu_float_exact)
        set_tests_properties(spu_float_exact PROPERTIES TIMEOUT 300)

        add_executable(test_spu_lockline_wait
            runtime/spu/tests/test_spu_lockline_wait.c)
        target_include_directories(test_spu_lockline_wait PRIVATE include)
        target_compile_options(test_spu_lockline_wait PRIVATE -UNDEBUG)
        target_link_libraries(test_spu_lockline_wait PRIVATE Threads::Threads)
        add_test(NAME spu_lockline_wait COMMAND test_spu_lockline_wait)
        set_tests_properties(spu_lockline_wait PROPERTIES TIMEOUT 15)

    endif()
    add_executable(test_rsx_present_policy tests/test_rsx_present_policy.c)
    target_include_directories(test_rsx_present_policy PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
    add_test(NAME rsx_present_policy COMMAND test_rsx_present_policy)


    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        # The guest exit path itself: status, the flush-through-a-pipe and the
        # static destructor that must stay unrun.
        add_test(NAME quick_exit
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_quick_exit.py"
                $<TARGET_FILE:test_quick_exit>)
        set_tests_properties(quick_exit PROPERTIES LABELS "runtime;t0" TIMEOUT 120)
        add_test(NAME spinlock_word_width
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spinlock_word_width.py")
        add_test(NAME spurs_task_exit_code_initialize
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_task_exit_code_initialize.py")
        add_test(NAME spurs_taskset_size
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_taskset_size.py")
        # The return codes the guest sees: task/core literals through the real HLE, and the
        # policy-module/job families the firmware and RPCS3 define.
        add_test(NAME spurs_error_codes
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_error_codes.py")
        # A task or job the SPURS HLE cannot dispatch (an image that is not an SPU ELF, a job with no
        # binary or one outside guest memory) ends in a named stop; a null or unaligned task ELF is the
        # firmware's error.
        add_test(NAME spurs_dispatch_miss
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_dispatch_miss.py")
        add_test(NAME spu_taskset_elf_flags
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_taskset_elf_flags.py")
        add_test(NAME spu_sdcrz
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_sdcrz.py")
        add_test(NAME spu_observe
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_observe.py")
        # The identical-DMA guard: timing-only back-off, opt-in named stop.
        add_test(NAME spu_dma_repeat
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_dma_repeat.py")
        # Malformed MFC transfers (size, alignment, LS overrun, EA range, the
        # null page, storage control, a resumed list) are named stops; a
        # zero-size transfer and an ack for an unparked tag are no-ops.
        add_test(NAME spu_mfc_errors
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_mfc_errors.py")
        # An SPU PUT/PUTL into the registered guest store window (the RSX label
        # window) is counted and stops by name; a PUT outside it is untouched.
        add_test(NAME spu_label_window
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_label_window.py")
        # The SPURS job loader: JOBCRT Ver13/Ver14 gate, Ver14 jm interface,
        # kernel view and bss, each byte-checked on synthetic jobs.
        add_test(NAME spurs_job_crt
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_job_crt.py")
        set_tests_properties(spinlock_word_width PROPERTIES LABELS "sync;t0")
        set_tests_properties(spurs_task_exit_code_initialize spurs_taskset_size spurs_error_codes
            spurs_dispatch_miss spu_taskset_elf_flags PROPERTIES LABELS "spurs;t0")
        set_tests_properties(spu_sdcrz spu_observe spu_dma_repeat spu_mfc_errors
            spu_label_window spurs_job_crt PROPERTIES LABELS "spu;t0")
        # Message passing between a PPU thread and an SPU through the real MFC
        # engine, both directions: acquire before a GET, release after a PUT.
        # The same program runs short here and at 10^8 in the stress tier.
        add_test(NAME spu_dma_litmus
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_dma_litmus.py" 5000000)
        add_test(NAME spu_dma_litmus_stress
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_dma_litmus.py" 100000000)
        set_tests_properties(spu_dma_litmus PROPERTIES LABELS "spu;t0" TIMEOUT 300)
        set_tests_properties(spu_dma_litmus_stress PROPERTIES LABELS "spu;stress" TIMEOUT 7200)
    endif()
    if(NOT WIN32)
        find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME spu_fnms_zero_extended
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spu_fnms_zero_extended.py")
            set_tests_properties(spu_fnms_zero_extended PROPERTIES LABELS "spu;t0")
            # The per-fixture integration harnesses: real runtime, hand-encoded
            # SPU programs, channel-overriding stubs (runtime/spu/tests/run_tests.sh).
            add_test(NAME spu_runtime_tests
                COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu/tests/run_tests.sh")
            set_tests_properties(spu_runtime_tests PROPERTIES
                LABELS "spu;t0" TIMEOUT 300
                ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
        endif()
    endif()
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME loaded_image_memory
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_loaded_image_memory.py")
        add_test(NAME loader_memory_domain
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_loader_memory_domain.py")
        add_test(NAME mmapper_hle_heap
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_mmapper_hle_heap.py")
        add_test(NAME user_memory_domain
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_user_memory_domain.py")
        set_tests_properties(loaded_image_memory loader_memory_domain
            mmapper_hle_heap user_memory_domain PROPERTIES LABELS "memory;t0")
        add_test(NAME check_hygiene
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_hygiene.py")
        add_test(NAME check_commit_msg
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_commit_msg.py")
        set_tests_properties(check_hygiene check_commit_msg PROPERTIES LABELS "tools;t0")
        # The lifter records the SHA-256 of the file it lifts; the runtime's
        # identity gate compares the loaded executable against it.
        add_test(NAME lifter_identity
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_lifter_identity.py")
        set_tests_properties(lifter_identity PROPERTIES LABELS "tools;t0")
        add_test(NAME lifter_code_window
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_lifter_code_window.py")
        set_tests_properties(lifter_code_window PROPERTIES LABELS "tools;t0")
        # #22: every store form into the RSX label window runs the hook before
        # its bytes land (the forms are generated from the real lifter), and the
        # GCM label-store path goes through the real module. The lifter's own
        # conformance suite runs every lowered instruction against the runtime.
        add_test(NAME ppu_store_hook
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_store_hook.py")
        add_test(NAME gcm_label_store
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gcm_label_store.py")
        set_tests_properties(ppu_store_hook gcm_label_store
            PROPERTIES LABELS "ppu;t0" TIMEOUT 300)
        add_test(NAME ppu_lift_conformance
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_lift.py")
        set_tests_properties(ppu_lift_conformance PROPERTIES LABELS "ppu;t0" TIMEOUT 600)
        # The trace schema contract: event/1 converts losslessly, validation is
        # strict, and diff reports the first divergence per execution context.
        add_test(NAME ps3diff
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ps3diff.py")
        set_tests_properties(ps3diff PROPERTIES LABELS "tools;t0")
        # Every gap the runtime used to paper over is a named stop (docs/STOPS.md).
        add_test(NAME hle_unresolved
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_hle_unresolved.py")
        add_test(NAME loader_gaps
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_loader_gaps.py")
        set_tests_properties(hle_unresolved loader_gaps PROPERTIES LABELS "ppu;t0" TIMEOUT 300)
        # #70: the HLE's flip/vblank handler delivery against the firmware's OPD contract (dossier T1-T3), through
        # cellGcmSys.c's pump and the real ppu_guest_call: a good descriptor runs, a zeroed or swapped one is the
        # named stop unresolved-guest-callback, never a skip or a call.
        add_test(NAME gcm_handler_contract
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gcm_handler_contract.py")
        set_tests_properties(gcm_handler_contract PROPERTIES LABELS "gcm;t0" TIMEOUT 300)
        # ppu_load_elf sets the SDK version syscall 25 reports from the ELF's PROC_PARAM.
        add_test(NAME proc_param_sdk_version
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_proc_param.py")
        set_tests_properties(proc_param_sdk_version PROPERTIES LABELS "ppu;t0" TIMEOUT 300)
        add_test(NAME game_patchcheck
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_game_patchcheck.py")
        set_tests_properties(game_patchcheck PROPERTIES LABELS "runtime;t0")
        add_test(NAME sys_spu_printf
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_sys_spu_printf.py")
        set_tests_properties(sys_spu_printf PROPERTIES LABELS "runtime;t0")
        add_test(NAME sys_crashdump
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_sys_crashdump.py")
        set_tests_properties(sys_crashdump PROPERTIES LABELS "runtime;t0")
        # The typed HLE call adapter: every return class, the positional argument
        # mapping and the guest parameter save area through the real
        # ps3_hle_call (runtime/ppu/tests/test_hle.cpp).
        add_test(NAME hle_abi
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_hle_abi.py")
        set_tests_properties(hle_abi PROPERTIES LABELS "ppu;t0" TIMEOUT 300)
        # The registration table is fail-closed: every scanned definition is a
        # firmware export by evidence or a host-side helper by rule (never a
        # NID registered by guess), and the helper names the review caught must
        # stay out of the table.
        add_test(NAME gen_hle_nids
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gen_hle_nids.py")
        set_tests_properties(gen_hle_nids PROPERTIES LABELS "tools;t0" TIMEOUT 300)
        # cellSaveDataEnableOverlay (cellSysutil 0xE7FA820B): registered in the table and returns CELL_OK.
        add_test(NAME savedata_enable_overlay
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_savedata_enable_overlay.py")
        set_tests_properties(savedata_enable_overlay PROPERTIES LABELS "ppu;t0" TIMEOUT 300)
        # PPU thread ids: counted up, never reused (a stale id is ESRCH), distinct under concurrent creates.
        add_test(NAME ppu_thread_ids
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_thread_ids.py")
        set_tests_properties(ppu_thread_ids PROPERTIES LABELS "runtime;t0;tsan" TIMEOUT 300)
        add_test(NAME ppu_thread_slots
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_thread_slots.py")
        set_tests_properties(ppu_thread_slots PROPERTIES LABELS "runtime;t0" TIMEOUT 300)
        add_test(NAME ppu_thread_stack_recycle
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_thread_stack_recycle.py")
        set_tests_properties(ppu_thread_stack_recycle PROPERTIES LABELS "runtime;t0;tsan" TIMEOUT 300)
        # The lv2 join/detach state machine and the worker epilogue's last access to its slot, ordered by a hook.
        add_test(NAME ppu_thread_join_detach
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_thread_join_detach.py")
        set_tests_properties(ppu_thread_join_detach PROPERTIES LABELS "runtime;t0;tsan" TIMEOUT 300)
        # docs/DIAGNOSTICS.md is hand-maintained prose; the checker requires the
        # knob set (and its summary counts) to match the sources, not the whole
        # generated text.
        add_test(NAME diagnostics_index
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_diagnostics_index.py" --check)
        add_test(NAME diagnostics_index_check
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gen_diagnostics_index.py")
        set_tests_properties(diagnostics_index diagnostics_index_check
            PROPERTIES LABELS "tools;t0")
    endif()

    # Module-level HLE argument/state checks (Phase 2a).  These are the same
    # standalone fixtures the CI workflows build; they include the module
    # source and define the host stubs themselves.
    if(UNIX)
        function(ps3recomp_add_hle_check_test name source)
            add_executable(${name} ${source})
            target_include_directories(${name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include")
            # The fixtures compile HLE sources that reach win32_backtrace.h
            # (dladdr/Dl_info); glibc hides those without _GNU_SOURCE.
            target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
            if(APPLE)
                target_link_options(${name} PRIVATE "-Wl,-dead_strip")
            else()
                target_compile_options(${name} PRIVATE
                    -ffunction-sections -fdata-sections)
                target_link_options(${name} PRIVATE "-Wl,--gc-sections")
            endif()
            add_test(NAME ${name} COMMAND ${name})
            set_tests_properties(${name} PROPERTIES LABELS "hle;t0" TIMEOUT 60)
        endfunction()

        ps3recomp_add_hle_check_test(hle_netctl_checks
            libs/network/tests/test_netctl_checks.c)
        target_include_directories(hle_netctl_checks PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/system")
        ps3recomp_add_hle_check_test(hle_cellfs_checks
            libs/filesystem/tests/test_cellfs_checks.c)
        ps3recomp_add_hle_check_test(hle_avconf_checks
            libs/misc/tests/test_avconf_gamma.c)
        ps3recomp_add_hle_check_test(hle_game_paths
            libs/system/tests/test_game_paths.c)
        # cellGame host paths: the refused calls end in named stops, so this
        # fixture links the real stop implementation and runs them in a child.
        add_executable(test_game_host_paths
            libs/system/tests/test_game_host_paths.c
            runtime/diag/stop.cpp)
        target_include_directories(test_game_host_paths PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu")
        target_compile_definitions(test_game_host_paths PRIVATE _GNU_SOURCE)
        set_target_properties(test_game_host_paths PROPERTIES LINKER_LANGUAGE CXX)
        add_test(NAME game_host_paths COMMAND test_game_host_paths)
        set_tests_properties(game_host_paths PROPERTIES LABELS "hle;t0" TIMEOUT 60)
        ps3recomp_add_hle_check_test(hle_savedata_io
            libs/system/tests/test_savedata_io.c)
        # cellSaveData turns the funcFile callback's fileBuf EA into a host
        # pointer. A span that leaves guest memory is a named stop
        # (hle-guest-range) before the pointer is formed, so this fixture
        # links the real stop implementation instead of the abort stub the
        # other savedata fixtures define, and runs the refused call in a child.
        add_executable(test_savedata_filebuf_range
            libs/system/tests/test_savedata_filebuf_range.c
            runtime/diag/stop.cpp)
        target_include_directories(test_savedata_filebuf_range PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu")
        target_compile_definitions(test_savedata_filebuf_range PRIVATE _GNU_SOURCE)
        set_target_properties(test_savedata_filebuf_range PROPERTIES LINKER_LANGUAGE CXX)
        add_test(NAME savedata_filebuf_range COMMAND test_savedata_filebuf_range)
        set_tests_properties(savedata_filebuf_range PROPERTIES LABELS "hle;t0" TIMEOUT 60)
        # funcStat/funcFile result mapping, version/errDialog PARAM, and the
        # statGet.fileList contract (#48). The refused setBuf->buf span is a
        # named stop, so it links the real stop implementation like the fixture
        # above and runs that case in a forked child.
        add_executable(test_savedata_cbresult
            libs/system/tests/test_savedata_cbresult.c
            runtime/diag/stop.cpp)
        target_include_directories(test_savedata_cbresult PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu")
        target_compile_definitions(test_savedata_cbresult PRIVATE _GNU_SOURCE)
        set_target_properties(test_savedata_cbresult PROPERTIES LINKER_LANGUAGE CXX)
        add_test(NAME savedata_cbresult COMMAND test_savedata_cbresult)
        set_tests_properties(savedata_cbresult PROPERTIES LABELS "hle;t0" TIMEOUT 60)
        ps3recomp_add_hle_check_test(hle_trophy_start
            libs/network/tests/test_trophy_start.c)
        ps3recomp_add_hle_check_test(hle_trophy_registration
            libs/network/tests/test_trophy_registration.c)
        target_sources(hle_trophy_registration PRIVATE libs/system/cellSysutil.c)
        target_link_libraries(hle_trophy_registration PRIVATE Threads::Threads)
        set_tests_properties(hle_trophy_registration PROPERTIES LABELS "hle;t0;tsan")
        ps3recomp_add_hle_check_test(hle_trophy_errors
            libs/network/tests/test_trophy_errors.c)
        target_sources(hle_trophy_errors PRIVATE libs/system/cellSysutil.c)
        target_link_libraries(hle_trophy_errors PRIVATE Threads::Threads)
        ps3recomp_add_hle_check_test(hle_dialog_callbacks
            libs/system/tests/test_dialog_callbacks.c)
        target_sources(hle_dialog_callbacks PRIVATE libs/system/cellSysutil.c)
        target_link_libraries(hle_dialog_callbacks PRIVATE Threads::Threads)

        if(SDL2_FOUND)
            ps3recomp_add_hle_check_test(hle_audio_ring
                libs/audio/tests/test_audio_ring.c)
            ps3recomp_add_hle_check_test(hle_pad_rumble
                libs/input/tests/test_pad_rumble.c)
            if(TARGET SDL2::SDL2)
                target_link_libraries(hle_audio_ring PRIVATE SDL2::SDL2)
                target_link_libraries(hle_pad_rumble PRIVATE SDL2::SDL2)
            else()
                target_include_directories(hle_audio_ring PRIVATE ${SDL2_INCLUDE_DIRS})
                target_include_directories(hle_pad_rumble PRIVATE ${SDL2_INCLUDE_DIRS})
                target_link_libraries(hle_audio_ring PRIVATE ${SDL2_LIBRARIES})
                target_link_libraries(hle_pad_rumble PRIVATE ${SDL2_LIBRARIES})
            endif()
            target_link_libraries(hle_pad_rumble PRIVATE m)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# POSIX host harness
# ---------------------------------------------------------------------------
# Drives the cellGcm -> RSX -> backend bridge with no lifted game, so a backend
# can be brought up and regression-tested without a game binary or a recompiler
# run. Builds on any POSIX host: it picks Metal on Apple and the null backend's
# headless software path everywhere else, so Linux -- which has no real backend
# yet -- can still run the whole guest command stream end to end in CI.
if(UNIX)
    add_executable(ps3recomp_host
        runtime/host/host_posix.c
        runtime/host/host_stubs.c
    )
    target_link_libraries(ps3recomp_host PRIVATE ps3recomp_runtime)
    if(NOT WIN32)
        # glibc keeps the math functions in a separate library; MSVC does not.
        target_link_libraries(ps3recomp_host PRIVATE m)
    endif()
    target_include_directories(ps3recomp_host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    # The runtime carries C++ (and, with translation on, spirv-cross), so the
    # C++ driver must do the final link or the standard library goes missing.
    set_target_properties(ps3recomp_host PROPERTIES LINKER_LANGUAGE CXX)

    # Guest shader translation, decompilers -> HLSL -> MSL, with no GPU: the
    # regression net for the Metal backend's shader path on every platform.
    if(PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_shader_msl
            libs/video/tests/test_shader_msl.c
            libs/video/rsx_shader_msl.cpp
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
        )
        target_include_directories(test_shader_msl PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(test_shader_msl PRIVATE ps3recomp_msl_translation)

        # The polygon offset through the engine and the Metal backend on the
        # GPU; the guest programs need the translation above.
        if(APPLE)
            add_executable(test_metal_poly_offset
                libs/video/tests/test_metal_poly_offset.m
                libs/video/rsx_shader_msl.cpp
                libs/video/rsx_dispatch.c
                libs/video/rsx_vertex_compact.c
                libs/video/rsx_texture_layout.c
                libs/video/rsx_vp_decompiler.c
                libs/video/rsx_fp_decompiler.c
            )
            set_source_files_properties(
                "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/tests/test_metal_poly_offset.m"
                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
            target_include_directories(test_metal_poly_offset PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
            )
            target_link_libraries(test_metal_poly_offset PRIVATE
                ps3recomp_msl_translation
                "-framework Metal" "-framework QuartzCore" "-framework AppKit")
            # Only what the engine and the backend reach; the rest of the
            # runtime is not linked in.
            target_link_options(test_metal_poly_offset PRIVATE "-Wl,-dead_strip")
        endif()
    endif()

    # The RESC output path on a real Metal device: the library's convert-and-flip stream, run by
    # the real draw engine, must put the composed surface into the display buffer. Links the
    # library's own cellResc.c/resc_convert.c and the engine through the test, not ps3recomp_runtime
    # (the test supplies the libgcm side). Skips (77) without a Metal device.
    if(APPLE AND PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_resc_present_metal
            libs/video/tests/test_resc_present_metal.m
            libs/video/cellResc.c
            libs/video/resc_convert.c
            libs/video/rsx_shader_msl.cpp
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
            libs/video/rsx_texture_layout.c
            libs/video/rsx_dispatch.c
            libs/video/rsx_vertex_compact.c
        )
        set_source_files_properties(libs/video/tests/test_resc_present_metal.m
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_include_directories(test_resc_present_metal PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/tests"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        )
        target_link_libraries(test_resc_present_metal PRIVATE ps3recomp_msl_translation
            "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit")
        set_target_properties(test_resc_present_metal PROPERTIES LINKER_LANGUAGE CXX)
        # The included backend also carries its legacy fixed-function entry points, which pull in
        # the RSX state machine; the engine path under test never reaches them.
        target_link_options(test_resc_present_metal PRIVATE "-Wl,-dead_strip")
        add_test(NAME resc_present_metal COMMAND test_resc_present_metal)
        set_tests_properties(resc_present_metal PROPERTIES
            LABELS "resc;video;gpu;t0" SKIP_RETURN_CODE 77 TIMEOUT 120
            ENVIRONMENT "PS3RECOMP_METAL_HEADLESS=1")
    endif()

    # NV4097 method contracts read back from a real Metal device: each test
    # includes the engine and the backend and drives them with synthetic
    # method words (#62). Skips (77) without a Metal device.
    if(APPLE AND PS3RECOMP_MSL_TRANSLATION)
        foreach(gpu_test back_stencil window)
            add_executable(test_metal_${gpu_test}
                libs/video/tests/test_metal_${gpu_test}.m
                libs/video/rsx_shader_msl.cpp
                libs/video/rsx_dispatch.c
                libs/video/rsx_vertex_compact.c
                libs/video/rsx_texture_layout.c
                libs/video/rsx_vp_decompiler.c
                libs/video/rsx_fp_decompiler.c
            )
            set_source_files_properties(libs/video/tests/test_metal_${gpu_test}.m
                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
            target_include_directories(test_metal_${gpu_test} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
            )
            target_link_libraries(test_metal_${gpu_test} PRIVATE ps3recomp_msl_translation
                "-framework Metal" "-framework QuartzCore" "-framework AppKit")
            target_link_options(test_metal_${gpu_test} PRIVATE "-Wl,-dead_strip")
            add_test(NAME metal_${gpu_test} COMMAND test_metal_${gpu_test})
            set_tests_properties(metal_${gpu_test} PROPERTIES
                LABELS "video;gpu;t0" SKIP_RETURN_CODE 77 TIMEOUT 120
                ENVIRONMENT "PS3RECOMP_METAL_HEADLESS=1")
        endforeach()
    endif()

    # The RSX capture replay runner (docs/RSX_GRAPHICS.md, "Replay"): a ps3diff.rsx/1 capture through the
    # real FIFO walker, draw engine and headless Metal backend, with no title. It links the whole runtime
    # library, like ps3recomp_host, so it runs the code the title runs, not a copy of it.
    if(APPLE AND PS3RECOMP_MSL_TRANSLATION)
        add_executable(rsx_replay
            tools/rsx_replay/rsx_replay.cpp
            runtime/host/host_stubs.c
        )
        target_include_directories(rsx_replay PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        )
        target_link_libraries(rsx_replay PRIVATE ps3recomp_runtime)
        set_target_properties(rsx_replay PROPERTIES LINKER_LANGUAGE CXX)
    endif()

    # The HLSL -> SPIR-V translation cache, memory and disk. Needs glslang
    # only, no GPU.
    if(PS3RECOMP_SPIRV_TRANSLATION)
        add_executable(test_shader_spirv
            libs/video/tests/test_shader_spirv.c
            libs/video/rsx_shader_spirv.cpp
        )
        target_include_directories(test_shader_spirv PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(test_shader_spirv PRIVATE glslang::glslang glslang::SPIRV)
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(test_shader_spirv PRIVATE
                glslang::glslang-default-resource-limits)
        endif()
        set_target_properties(test_shader_spirv PROPERTIES LINKER_LANGUAGE CXX)
    endif()

    # The register-file draw engine's accounting and dump naming (M3 gaps
    # 3a-3e, 3g). Driven through rsx_draw_engine_method() against a recording
    # stub backend: no GPU, no host API, no game content. Same source set the
    # CI workflows compile ad hoc.
    foreach(engine_test test_rsx_draw_accounting test_rsx_draw_dump_names)
        add_executable(${engine_test}
            libs/video/tests/${engine_test}.c
            libs/video/rsx_draw_engine.c
            libs/video/rsx_dispatch.c
            libs/video/rsx_vertex_compact.c
            libs/video/rsx_texture_layout.c
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
        )
        target_include_directories(${engine_test} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/tests"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        add_test(NAME ${engine_test} COMMAND ${engine_test})
        set_tests_properties(${engine_test} PROPERTIES LABELS "gcm;t0" TIMEOUT 120)
    endforeach()

    # The RSX capture (ps3diff.rsx/1), producer side. Both fixtures leave a capture in a directory and
    # tools/test_rsx_capture.py checks it: the walker's (executed order, binds, labels, a wait that is
    # not an event) and the engine's (draws, the buffer as each draw consumed it, drops, frames).
    # No GPU, no game content. They link only what they test, like the other single-TU fixtures.
    set(RSX_CAPTURE_FIXTURE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
    add_executable(test_rsx_capture_walker
        libs/video/tests/test_rsx_capture_walker.c
        libs/video/rsx_capture.cpp
        libs/video/gcm_default_state.c
        runtime/memory/label_window.c
        runtime/diag/stop.cpp
        runtime/diag/sha256.cpp)
    add_executable(test_rsx_capture_engine
        libs/video/tests/test_rsx_capture_engine.c
        libs/video/rsx_capture.cpp
        libs/video/rsx_draw_engine.c
        libs/video/rsx_dispatch.c
        libs/video/rsx_vertex_compact.c
        libs/video/rsx_texture_layout.c
        libs/video/rsx_vp_decompiler.c
        libs/video/rsx_fp_decompiler.c
        runtime/diag/stop.cpp
        runtime/diag/sha256.cpp)
    foreach(t test_rsx_capture_walker test_rsx_capture_engine)
        target_include_directories(${t} PRIVATE ${RSX_CAPTURE_FIXTURE_INCLUDES})
        target_compile_definitions(${t} PRIVATE _GNU_SOURCE)
        target_link_libraries(${t} PRIVATE Threads::Threads)
        set_target_properties(${t} PROPERTIES LINKER_LANGUAGE CXX C_EXTENSIONS ON)
        if(APPLE)
            target_link_options(${t} PRIVATE "-Wl,-dead_strip")
        else()
            target_compile_options(${t} PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffunction-sections -fdata-sections>)
            target_link_options(${t} PRIVATE "-Wl,--gc-sections")
            target_link_libraries(${t} PRIVATE m)
        endif()
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME rsx_capture
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_rsx_capture.py"
                $<TARGET_FILE:test_rsx_capture_walker> $<TARGET_FILE:test_rsx_capture_engine>)
        set_tests_properties(rsx_capture PROPERTIES LABELS "gcm;t0" TIMEOUT 120)
    endif()

    # NV0039 (M2MF) and the walker's method verdicts: the real walker over synthetic rings, destination
    # bytes observed. tools/test_rsx_methods.py runs the fixture's modes and checks the captures it leaves.
    add_executable(test_gcm_nv0039
        libs/video/tests/test_gcm_nv0039.c
        libs/video/rsx_dispatch.c
        libs/video/rsx_capture.cpp
        libs/video/gcm_default_state.c
        runtime/memory/label_window.c
        runtime/diag/stop.cpp
        runtime/diag/sha256.cpp)
    target_include_directories(test_gcm_nv0039 PRIVATE ${RSX_CAPTURE_FIXTURE_INCLUDES})
    target_compile_definitions(test_gcm_nv0039 PRIVATE _GNU_SOURCE)
    target_link_libraries(test_gcm_nv0039 PRIVATE Threads::Threads)
    set_target_properties(test_gcm_nv0039 PROPERTIES LINKER_LANGUAGE CXX C_EXTENSIONS ON)
    if(APPLE)
        target_link_options(test_gcm_nv0039 PRIVATE "-Wl,-dead_strip")
    else()
        target_compile_options(test_gcm_nv0039 PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffunction-sections -fdata-sections>)
        target_link_options(test_gcm_nv0039 PRIVATE "-Wl,--gc-sections")
        target_link_libraries(test_gcm_nv0039 PRIVATE m)
    endif()
    # The method classification against the census it must explain (issue #47): one synthetic FIFO with
    # every observed (class, method) pair, counted, plus the unclassified pair's named stop. The real
    # register-file class table is linked in, because that is the engine's consumer list.
    add_executable(test_rsx_method_census
        libs/video/tests/test_rsx_method_census.c
        libs/video/rsx_dispatch.c
        libs/video/gcm_default_state.c
        runtime/memory/label_window.c
        runtime/diag/stop.cpp)
    target_include_directories(test_rsx_method_census PRIVATE ${RSX_CAPTURE_FIXTURE_INCLUDES})
    target_compile_definitions(test_rsx_method_census PRIVATE _GNU_SOURCE)
    target_link_libraries(test_rsx_method_census PRIVATE Threads::Threads)
    set_target_properties(test_rsx_method_census PROPERTIES LINKER_LANGUAGE CXX C_EXTENSIONS ON)
    if(APPLE)
        target_link_options(test_rsx_method_census PRIVATE "-Wl,-dead_strip")
    else()
        target_compile_options(test_rsx_method_census PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffunction-sections -fdata-sections>)
        target_link_options(test_rsx_method_census PRIVATE "-Wl,--gc-sections")
        target_link_libraries(test_rsx_method_census PRIVATE m)
    endif()
    add_test(NAME rsx_method_census COMMAND $<TARGET_FILE:test_rsx_method_census>)
    set_tests_properties(rsx_method_census PROPERTIES LABELS "rsx;t0" TIMEOUT 120)

    # The depth range (SET_CLIP_MIN/MAX) through the register-file engine, host only: the guest's
    # 0.25/0.75 must reach the backend per draw. The real Metal readback is test_depth_range_metal.m.
    add_executable(test_rsx_depth_range
        libs/video/tests/test_rsx_depth_range.c
        libs/video/rsx_vertex_compact.c
        libs/video/rsx_texture_layout.c
        libs/video/rsx_vp_decompiler.c
        libs/video/rsx_fp_decompiler.c)
    target_include_directories(test_rsx_depth_range PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )
    set_target_properties(test_rsx_depth_range PROPERTIES LINKER_LANGUAGE CXX C_EXTENSIONS ON)
    target_link_libraries(test_rsx_depth_range PRIVATE m)
    add_test(NAME rsx_depth_range COMMAND $<TARGET_FILE:test_rsx_depth_range>)
    set_tests_properties(rsx_depth_range PROPERTIES LABELS "rsx;t0" TIMEOUT 120)

    # The same contract on a real device: [0.25, 0.75] must land in MTLViewport.znear/zfar, read back
    # from the depth buffer. Labelled gpu so a host without Metal skips it (exit 77), never passes.
    if(APPLE AND PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_depth_range_metal
            libs/video/tests/test_depth_range_metal.m
            libs/video/rsx_dispatch.c
            libs/video/rsx_vertex_compact.c
            libs/video/rsx_texture_layout.c
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
            libs/video/rsx_capture.cpp
            libs/video/rsx_shader_msl.cpp
            runtime/diag/sha256.cpp)
        set_source_files_properties(libs/video/tests/test_depth_range_metal.m
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_include_directories(test_depth_range_metal PRIVATE ${RSX_CAPTURE_FIXTURE_INCLUDES})
        target_link_libraries(test_depth_range_metal PRIVATE ps3recomp_msl_translation Threads::Threads
            "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit")
        target_link_options(test_depth_range_metal PRIVATE "-Wl,-dead_strip")
        set_target_properties(test_depth_range_metal PROPERTIES LINKER_LANGUAGE CXX)
        add_test(NAME depth_range_metal COMMAND $<TARGET_FILE:test_depth_range_metal>)
        set_tests_properties(depth_range_metal PROPERTIES LABELS "rsx;gpu;t0" SKIP_RETURN_CODE 77 TIMEOUT 300)
        if(PS3RECOMP_SANITIZE)
            # Same false positive as `rsx_replay` above (#52): spirv-cross reports a container-overflow
            # reading the SPIR-V vector glslang filled, with a correct data()/size() handoff
            # (rsx_shader_msl.cpp:121). Homebrew's glslang is uninstrumented; dyld coalescing its weak
            # std::vector code with our instrumented copies leaves the vector's annotations stale.
            # dev-ci run 40366340676 hit it here, inside eng_function, before any readback ran. It does
            # not reproduce on the local Xcode toolchain (the test passes with the check on there), so
            # this rests on the CI evidence and the identical, already-accepted #52 case. Only that
            # check is off, only for this test; the rest repeats the preset's ASAN_OPTIONS, which
            # ENVIRONMENT replaces.
            set_tests_properties(depth_range_metal PROPERTIES ENVIRONMENT
                "ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:detect_container_overflow=0")
        endif()
    endif()

    set(RSX_METHODS_TEST_ARGS $<TARGET_FILE:test_gcm_nv0039>)
    # The backend's dump-list parser, no GPU: the .m includes the backend and reaches only fdump_parse.
    if(APPLE)
        add_executable(test_metal_frame_dump_list
            libs/video/tests/test_metal_frame_dump_list.m
            runtime/diag/stop.cpp)
        set_source_files_properties(libs/video/tests/test_metal_frame_dump_list.m
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_include_directories(test_metal_frame_dump_list PRIVATE ${RSX_CAPTURE_FIXTURE_INCLUDES})
        target_link_libraries(test_metal_frame_dump_list PRIVATE Threads::Threads
            "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit")
        set_target_properties(test_metal_frame_dump_list PROPERTIES LINKER_LANGUAGE CXX)
        target_link_options(test_metal_frame_dump_list PRIVATE "-Wl,-dead_strip")
        list(APPEND RSX_METHODS_TEST_ARGS $<TARGET_FILE:test_metal_frame_dump_list>)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME rsx_methods
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_rsx_methods.py"
                ${RSX_METHODS_TEST_ARGS})
        set_tests_properties(rsx_methods PROPERTIES LABELS "gcm;t0" TIMEOUT 120)
        # The park heuristic in both modes (Reference stop, Compatibility resync) on one synthetic parked
        # FIFO; the fixture compiles cellGcmSys.c into itself (tools/test_gcm_park_modes.py).
        add_test(NAME gcm_park_modes
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gcm_park_modes.py")
        set_tests_properties(gcm_park_modes PROPERTIES LABELS "gcm;t0" TIMEOUT 120)
        # The walker with the real draw engine: a method is interpreted in the class bound on its
        # subchannel (a 2D class never reaches the NV4097 register file), puller words act on every slot.
        add_test(NAME gcm_class_routing
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gcm_class_routing.py")
        set_tests_properties(gcm_class_routing PROPERTIES LABELS "gcm;t0" TIMEOUT 120)
        # #68: the handler mask word at driver_info+0x12C0 as libgcm_sys keeps it (dossier T6): start value 4 after
        # init, a per-bit read-modify-write in each setter, foreign bits surviving. `gcm_handler_mask_race` is the same
        # fixture with two threads registering at once, built with -fsanitize=thread where the compiler has it.
        add_test(NAME gcm_handler_mask
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gcm_handler_mask.py" mask)
        set_tests_properties(gcm_handler_mask PROPERTIES LABELS "gcm;t0" TIMEOUT 120)
        add_test(NAME gcm_handler_mask_race
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gcm_handler_mask.py" race)
        set_tests_properties(gcm_handler_mask_race PROPERTIES LABELS "gcm;t0;tsan" TIMEOUT 300)
    endif()

    # Capture -> replay on a real Metal device (tools/test_rsx_replay.py): the scene fixture is a synthetic
    # live run through the real walker, engine and backend; rsx_replay replays what it captured. Labelled
    # `gpu` so a host without Metal (every Linux job) lists the test as skipped, never as passed.
    if(APPLE AND PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_rsx_replay_scene
            libs/video/tests/test_rsx_replay_scene.c
            runtime/host/host_stubs.c
        )
        target_include_directories(test_rsx_replay_scene PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        )
        target_link_libraries(test_rsx_replay_scene PRIVATE ps3recomp_runtime)
        set_target_properties(test_rsx_replay_scene PROPERTIES LINKER_LANGUAGE CXX)
        if(Python3_Interpreter_FOUND)
            add_test(NAME rsx_replay
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_rsx_replay.py"
                    $<TARGET_FILE:rsx_replay> $<TARGET_FILE:test_rsx_replay_scene>)
            set_tests_properties(rsx_replay PROPERTIES LABELS "gcm;video;gpu;t0" SKIP_RETURN_CODE 77 TIMEOUT 300)
            if(PS3RECOMP_SANITIZE)
                # On macos-15 / Xcode 16.4 (dev-ci run 40307082211) spirv-cross reports a
                # container-overflow reading the SPIR-V vector that glslang filled, with a correct
                # data()/size() call. glslang is Homebrew's prebuilt, uninstrumented dylib; the likely
                # cause is that dyld coalesces its weak std::vector code with our instrumented copies,
                # which leaves the vector's container annotations stale (the documented false-positive
                # class for mixed builds). Only that check is off, only for this test; the rest repeats
                # the asan test preset's ASAN_OPTIONS, which ENVIRONMENT replaces.
                set_tests_properties(rsx_replay PROPERTIES ENVIRONMENT
                    "ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:detect_container_overflow=0")
            endif()
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# PPU boot smoke title
# ---------------------------------------------------------------------------
# The one target that LINKS AND RUNS the PPU boot scaffold. runtime/ppu/ is the
# game-agnostic boot path every port is built on, and nothing here has ever
# built it: it #includes the lifter's per-game ppu_recomp.h, so
# tools/check_ppu_scaffold.py compiles it against a stub header and stops
# there. Compiling is not running, and on POSIX the loader, the NID bridge, the
# lv2 syscalls, guest threads, the frame clock and the RSX backend had never
# once run together.
#
# runtime/ppu/tests/smoke/ supplies what was missing: a hand-written program in
# the lifter's ABI, and tools/make_smoke_elf.py writes both the PPU ELF it is
# loaded from and the ppu_recomp.h it compiles against -- the header from
# ppu_lifter.py's own HEADER_PREAMBLE, so the synthetic title cannot drift away
# from the ABI a real one gets.
#
# The HLE NID table is generated exactly as a port generates it, so the smoke
# title's firmware imports resolve through the same bridge a game's do.
if(UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "  Boot smoke:   no python3; ps3recomp_boot_smoke not built")
    else()
        set(SMOKE_GEN "${CMAKE_CURRENT_BINARY_DIR}/smoke")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/smoke.elf" "${SMOKE_GEN}/ppu_recomp.h"
                   "${SMOKE_GEN}/smoke_identity.c"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    --out      "${SMOKE_GEN}/smoke.elf"
                    --header   "${SMOKE_GEN}/ppu_recomp.h"
                    --identity "${SMOKE_GEN}/smoke_identity.c"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppu_lifter.py"
            COMMENT "Generating the smoke title's image, ppu_recomp.h and identity")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/ppu_hle_nids.cpp"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    --out "${SMOKE_GEN}/ppu_hle_nids.cpp" --all
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nid_database.py"
                    ${HLE_SOURCES}
            COMMENT "Generating the HLE NID -> handler table")
        # The generated unit includes libs/... by repository-relative path and
        # ppu_hle_adapt.hpp from runtime/ppu; both are already on this target's
        # include path, the repository root is not. Keep it private to the
        # generated file rather than on every source of the smoke title.
        set_source_files_properties("${SMOKE_GEN}/ppu_hle_nids.cpp"
            PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")

        add_executable(ps3recomp_boot_smoke
            runtime/ppu/tests/boot_main.cpp
            runtime/ppu/tests/smoke/smoke_recomp.c
            runtime/ppu/ppu_loader.cpp
            runtime/ppu/ppu_hle.cpp
            runtime/ppu/ppu_fs.cpp
            runtime/ppu/ppu_sysprx.cpp
            "${SMOKE_GEN}/ppu_hle_nids.cpp"
            "${SMOKE_GEN}/smoke_identity.c"
            "${SMOKE_GEN}/ppu_recomp.h"
        )
        target_include_directories(ps3recomp_boot_smoke PRIVATE
            "${SMOKE_GEN}"                                  # the generated ppu_recomp.h
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/memory"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/syscalls"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_link_libraries(ps3recomp_boot_smoke PRIVATE ps3recomp_runtime)
        set_target_properties(ps3recomp_boot_smoke PROPERTIES LINKER_LANGUAGE CXX)

        # The smoke guest decides its own verdict and the exit code carries it
        # (see smoke_recomp.c). Run under both values of ppu.vscr_nj: the guest
        # checks the VSCR of its main thread and of a thread it spawns against
        # whatever the config says. The Metal backend needs its headless mode
        # off a display; other hosts take the null backend regardless.
        if(PS3RECOMP_BUILD_TESTS)
            add_test(NAME boot_smoke COMMAND ps3recomp_boot_smoke "${SMOKE_GEN}/smoke.elf")
            add_test(NAME boot_smoke_vscr_nj_off COMMAND ps3recomp_boot_smoke "${SMOKE_GEN}/smoke.elf")
            set_tests_properties(boot_smoke PROPERTIES
                ENVIRONMENT "PS3RECOMP_METAL_HEADLESS=1"
                LABELS "ppu;t0" TIMEOUT 120)
            set_tests_properties(boot_smoke_vscr_nj_off PROPERTIES
                ENVIRONMENT "PS3RECOMP_METAL_HEADLESS=1;PS3RECOMP_PPU_VSCR_NJ=0"
                LABELS "ppu;t0" TIMEOUT 120)
            # The gate itself, end to end: the title boots from its own image,
            # and from nothing that is not byte-identical to it.
            add_test(NAME identity_gate
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gates/test_identity.py"
                        $<TARGET_FILE:ps3recomp_boot_smoke> "${SMOKE_GEN}/smoke.elf")
            set_tests_properties(identity_gate PROPERTIES LABELS "ppu;t0" TIMEOUT 120)
        endif()

        # Where the runnable image lands, so CI and a developer both say
        #   ./build/ps3recomp_boot_smoke build/smoke/smoke.elf
        message(STATUS "  Boot smoke:   ps3recomp_boot_smoke + smoke/smoke.elf")

        # The same lift again, arranged the way a port's own tree arranges one:
        # the generated ppu_recomp.h and the lifted source in one directory,
        # which is what templates/project wants for -DRECOMP_DIR. The header is
        # generated into the build tree and the source lives in the repository,
        # so a directory holding both has to be made rather than pointed at.
        #
        # This is what lets CI build the project template against a real lift
        # and run it -- the check that a port started from templates/project
        # still boots, on a platform where it can be run.
        set(SMOKE_LIFT "${SMOKE_GEN}/lift")
        set(SMOKE_LIFT_SRC
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu/tests/smoke/smoke_recomp.c")
        add_custom_command(
            OUTPUT "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c"
                   "${SMOKE_LIFT}/smoke_identity.c"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SMOKE_LIFT}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT}/ppu_recomp.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_GEN}/smoke_identity.c" "${SMOKE_LIFT}/smoke_identity.c"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_LIFT_SRC}" "${SMOKE_LIFT}/smoke_recomp.c"
            DEPENDS "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_GEN}/smoke_identity.c" "${SMOKE_LIFT_SRC}"
            COMMENT "Staging the smoke title's lift as a RECOMP_DIR")
        add_custom_target(ps3recomp_smoke_lift ALL
            DEPENDS "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c"
                    "${SMOKE_LIFT}/smoke_identity.c")
        message(STATUS "  Smoke lift:   smoke/lift (a RECOMP_DIR for templates/project)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
