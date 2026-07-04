cmake_minimum_required(VERSION 3.20)
# C is enabled alongside CXX for the one vendored C TU: lib/md5.c (the
# public-domain Openwall MD5 behind LastFm's api_sig — slice 2, both platforms).
project(remoct VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows toolchain prefix. On Linux the find_library/find_path calls below fall
# through these (empty/nonexistent) hints to the system paths — verified on Debian
# Trixie: configure passes unchanged (Phase 3 readiness survey, docs/phase3-readiness.md).
if(WIN32)
    set(MSYS2_PREFIX "C:/msys64/ucrt64")
    message(STATUS "MSYS2 prefix: ${MSYS2_PREFIX}")
    list(APPEND CMAKE_PREFIX_PATH "${MSYS2_PREFIX}")
endif()

# ── Phase 0 static-link probe (experimental/win-pdcurses) ──────────────────
# Toggle with -DREMOCT_STATIC_PROBE=ON. Prefers static .a archives over the
# MSYS2 import stubs (.dll.a) so curses + codecs + GCC runtime fold into the
# exe. OFF by default -> the normal dynamic build is completely unchanged.
option(REMOCT_STATIC_PROBE "Prefer static archives + static libgcc/stdc++ (Windows single-exe probe)" OFF)
if(WIN32 AND REMOCT_STATIC_PROBE)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a;.dll.a")
    message(STATUS "STATIC PROBE: preferring .a archives over .dll.a import stubs")
endif()

# ── Sources ────────────────────────────────────────────────────────────────
set(REMOCT_SOURCES
    src/main.cpp
    src/UIManager.cpp
    src/AudioManager.cpp
    src/PlaylistManager.cpp
    src/Config.cpp
    src/LrcData.cpp
    src/CDSource.cpp
    src/LocalFileSource.cpp
    src/MBLookup.cpp
    src/CDRipper.cpp
    src/CoverArt.cpp
    # Phase 4: the streaming source is driven through the plugin C ABI. Slice (c)
    # moved StreamSource + IHeartRadio + IHeartNowPlayingSM + IHeartDeepLog + the
    # adapter into plugins/stream/ (built as remoct_stream.{so,dll}); the host keeps
    # only the DRIVER side + the loader.
    src/PluginSource.cpp          # host-side driver over a RemoctPlugin table
    src/PluginHostServices.cpp    # RemoctHostServices HTTP shim over core::IHttp
    src/PluginHost.cpp            # loadPlugin / validatePlugin / LoadedPlugin (host is a loader now)
    src/ar_crc.cpp
    src/AacDecoder.cpp            # FDK-AAC file (.m4a/.m4b) backend — stays host (D5)
    src/Mp4Chapters.cpp
    src/RadioBrowser.cpp
    src/LastFm.cpp
    src/Log.cpp
    src/ListenBrainz.cpp
    src/DiscordRP.cpp
    lib/md5.c
)

# Platform seams (Phase 1): Windows impls of the core interfaces, in src/platform/win/
# — the core/platform boundary (win/ compiled only on Windows; linux/ siblings =
# libcurl / Unix socket, Phase 3).
if(WIN32)
    list(APPEND REMOCT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/IpcWinPipe.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/NotifyWinToast.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/CdIoWin.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/PluginLoaderWin.cpp  # slice c: LoadLibrary seam in the host
    )
endif()

# Phase 3: Linux platform sources. Slice 0 landed SeamStubs.cpp (inert seam
# placeholders + the core:: link-time bridges); slice 1 builds the full remoct
# target on UNIX against them. Each seam slice (2/4/5/6) replaced its stub with a
# real impl file — slice 6 (CdIoSgIo.cpp) is the last, so SeamStubs.cpp is gone
# and every seam now has its real Linux transport.
if(UNIX)
    list(APPEND REMOCT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/linux/HttpCurl.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/IpcUnixSocket.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/NotifyNotifySend.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/CdIoSgIo.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/PluginLoaderPosix.cpp  # slice c: dlopen seam in the host
    )
    # Module-mode search only: under WSL2 the appended Windows PATH would let
    # config-mode find MSYS2's CURLConfig.cmake at /mnt/c (CMake treats PATH
    # entries as prefixes) and demand its Windows-side deps. The classic module
    # finds the system libcurl and still defines CURL::libcurl.
    set(CURL_NO_CURL_CMAKE ON)
    find_package(CURL REQUIRED)
    # Standalone compile check for the seam/stub layer (CI builds it by name —
    # proves src/platform/linux/ + the core interface headers stay
    # platform-clean even if the main target is mid-port).
    add_library(remoct_linux_seams OBJECT
        ${CMAKE_SOURCE_DIR}/src/platform/linux/IpcUnixSocket.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/NotifyNotifySend.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/CdIoSgIo.cpp
        # Phase 4 plugin-loader seam (dlopen). Now also a real host source (slice c
        # made the host a loader); kept here too so CI proves src/platform/linux/
        # stays platform-clean, and it's exercised by plugin_loader_test on both jobs.
        ${CMAKE_SOURCE_DIR}/src/platform/linux/PluginLoaderPosix.cpp
    )
    target_include_directories(remoct_linux_seams PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(remoct_linux_seams PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

add_executable(remoct ${REMOCT_SOURCES})

target_include_directories(remoct PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib
    ${FLAC_INCLUDE}
    ${LAME_INCLUDE}
    ${EBUR128_INCLUDE}
    ${FDKAAC_INCLUDE}
)
if(WIN32)
    target_include_directories(remoct PRIVATE
        ${MSYS2_PREFIX}/include
        ${MSYS2_PREFIX}/include/ncursesw
    )
else()
    # Debian keeps the wide-char build's headers under /usr/include/ncursesw —
    # putting it on the path makes <ncurses.h> resolve to the ncursesw variant
    # (NCURSES_WIDECHAR-ready), mirroring the MSYS2 layout above.
    target_include_directories(remoct PRIVATE /usr/include/ncursesw)
endif()

# ── Compile definitions ────────────────────────────────────────────────────
target_compile_definitions(remoct PRIVATE
    NCURSES_STATIC
    UNICODE
    _UNICODE
)

# ── libFLAC ────────────────────────────────────────────────────────────────
find_library(FLAC_LIB NAMES FLAC
    PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_path(FLAC_INCLUDE NAMES FLAC/stream_encoder.h
    PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "libFLAC: ${FLAC_LIB}")

# ── libmp3lame ─────────────────────────────────────────────────────────────
find_library(LAME_LIB NAMES mp3lame
    PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_path(LAME_INCLUDE NAMES lame/lame.h
    PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "lame: ${LAME_LIB}")

# ── libebur128 (ReplayGain EBU R128) ──────────────────────────────────────
find_library(EBUR128_LIB NAMES ebur128
    PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_path(EBUR128_INCLUDE NAMES ebur128.h
    PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "libebur128: ${EBUR128_LIB}")

# ── FDK-AAC (HE-AAC/AAC+ decode for internet radio) ───────────────────────
find_library(FDKAAC_LIB NAMES fdk-aac
    PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_path(FDKAAC_INCLUDE NAMES fdk-aac/aacdecoder_lib.h
    PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "fdk-aac: ${FDKAAC_LIB}")
find_library(NCURSESW_LIB NAMES ncursesw
    PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_library(PANELW_LIB NAMES panelw
    PATHS "${MSYS2_PREFIX}/lib")
message(STATUS "ncursesw: ${NCURSESW_LIB}")

# ── TagLib ─────────────────────────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(TAGLIB taglib)
endif()
if(NOT TAGLIB_FOUND)
    find_library(TAGLIB_LIB NAMES tag taglib
        PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
    set(TAGLIB_INCLUDE_DIRS "${MSYS2_PREFIX}/include/taglib")
    set(TAGLIB_LIBRARIES    "${TAGLIB_LIB}")
    message(STATUS "TagLib (manual): ${TAGLIB_LIB}")
else()
    message(STATUS "TagLib (pkg-config): ${TAGLIB_LIBRARIES}")
endif()
target_include_directories(remoct PRIVATE ${TAGLIB_INCLUDE_DIRS})

# ── Link ───────────────────────────────────────────────────────────────────
target_link_libraries(remoct PRIVATE
    ${NCURSESW_LIB}
    $<$<BOOL:${PANELW_LIB}>:${PANELW_LIB}>
    ${TAGLIB_LIBRARIES}
    ${FLAC_LIB}
    ${LAME_LIB}
    ${EBUR128_LIB}
    ${FDKAAC_LIB}
    $<$<PLATFORM_ID:Windows>:winmm>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Windows>:ole32>
    $<$<PLATFORM_ID:Windows>:wininet>
    $<$<PLATFORM_ID:Windows>:shell32>
    $<$<PLATFORM_ID:Windows>:advapi32>
    # Linux: miniaudio's runtime backend loading (dlopen) + threads + libm;
    # libcurl backs the core::IHttp seam (slice 2).
    $<$<PLATFORM_ID:Linux>:pthread>
    $<$<PLATFORM_ID:Linux>:dl>
    $<$<PLATFORM_ID:Linux>:m>
)
if(UNIX)
    target_link_libraries(remoct PRIVATE CURL::libcurl)
endif()

# ── Phase 0 static-link probe: static runtime + explicit transitive deps ────
if(WIN32 AND REMOCT_STATIC_PROBE)
    target_link_options(remoct PRIVATE -static -static-libgcc -static-libstdc++)
    # TagLib's headers default TAGLIB_EXPORT to __declspec(dllimport); without
    # this define the objects reference __imp_ symbols the static libtag.a lacks
    # (the C++ analog of the NCURSES_STATIC define already set above).
    target_compile_definitions(remoct PRIVATE TAGLIB_STATIC)
    # FLAC_API defaults to __declspec(dllimport) on Windows unless FLAC__NO_DLL
    # is defined -> same __imp_ mismatch against the static libFLAC.a.
    target_compile_definitions(remoct PRIVATE FLAC__NO_DLL)
    # Static libFLAC needs libogg explicitly (the dynamic build pulled ogg in
    # transitively via the FLAC DLL; a static .a exposes the dependency).
    target_link_libraries(remoct PRIVATE "${MSYS2_PREFIX}/lib/libogg.a")
endif()

# ── Compiler warnings ──────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # C++-only: lib/md5.c (C) must not get C++-only warning flags.
    target_compile_options(remoct PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic
        -Wno-unused-parameter -Wno-old-style-cast>
    )
endif()

set_target_properties(remoct PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS remoct RUNTIME DESTINATION bin)

# ── Phase 4 slice (c): the streaming source as a loadable plugin ────────────
# Built AFTER the host target so FDKAAC_LIB/INCLUDE + CURL::libcurl (found above)
# are in scope. add_dependencies makes `ninja remoct` also build remoct_stream so
# the .so/.dll is always present beside the binary (bin/plugins) for loadPlugin.
add_subdirectory(plugins/stream)
add_dependencies(remoct remoct_stream)
install(TARGETS remoct_stream LIBRARY DESTINATION bin/plugins RUNTIME DESTINATION bin/plugins)

# ── Phase 0 pure-unit tests ───────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
