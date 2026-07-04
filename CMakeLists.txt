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
    src/StreamSource.cpp
    src/IHeartNowPlayingSM.cpp
    src/ar_crc.cpp
    src/AacDecoder.cpp
    src/Mp4Chapters.cpp
    src/IHeartRadio.cpp
    src/IHeartDeepLog.cpp
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
    )
endif()

# Phase 3: Linux platform sources. Slice 0 landed SeamStubs.cpp (inert seam
# placeholders + the core:: link-time bridges); slice 1 builds the full remoct
# target on UNIX against them. Each seam slice (2/4/5/6) replaces its stub with
# a real impl file appended here.
if(UNIX)
    list(APPEND REMOCT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/linux/SeamStubs.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/HttpCurl.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/IpcUnixSocket.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/NotifyNotifySend.cpp
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
        ${CMAKE_SOURCE_DIR}/src/platform/linux/SeamStubs.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/IpcUnixSocket.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/NotifyNotifySend.cpp
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

# ── Phase 0 pure-unit tests ───────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
