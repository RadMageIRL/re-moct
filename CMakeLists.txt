cmake_minimum_required(VERSION 3.20)
project(remoct VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MSYS2_PREFIX "C:/msys64/ucrt64")
message(STATUS "MSYS2 prefix: ${MSYS2_PREFIX}")
list(APPEND CMAKE_PREFIX_PATH "${MSYS2_PREFIX}")

# ── Sources ────────────────────────────────────────────────────────────────
set(REMOCT_SOURCES
    src/main.cpp
    src/UIManager.cpp
    src/AudioManager.cpp
    src/PlaylistManager.cpp
    src/Config.cpp
    src/LrcData.cpp
    src/Toast.cpp
    src/CDSource.cpp
    src/MBLookup.cpp
    src/CDRipper.cpp
    src/StreamSource.cpp
    src/IHeartRadio.cpp
    src/IHeartDeepLog.cpp
    src/RadioBrowser.cpp
    src/LastFm.cpp
    src/Log.cpp
    src/ListenBrainz.cpp
    src/DiscordRP.cpp
)

add_executable(remoct ${REMOCT_SOURCES})

target_include_directories(remoct PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib
    ${MSYS2_PREFIX}/include
    ${MSYS2_PREFIX}/include/ncursesw
    ${FLAC_INCLUDE}
    ${LAME_INCLUDE}
    ${EBUR128_INCLUDE}
    ${FDKAAC_INCLUDE}
)

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
)

# ── Compiler warnings ──────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(remoct PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Wno-old-style-cast
    )
endif()

set_target_properties(remoct PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS remoct RUNTIME DESTINATION bin)
