cmake_minimum_required(VERSION 3.20)
# C is enabled alongside CXX for the one vendored C TU: lib/md5.c (the
# public-domain Openwall MD5 behind LastFm's api_sig — slice 2, both platforms).
project(remoct VERSION 1.1.0 LANGUAGES C CXX)

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

# ── Option C: PDCursesMod wingui (Windows single-exe truecolor) ─────────────
# Toggle with -DREMOCT_PDCURSES=ON (Windows only). Renders the TUI via the
# vendored PDCursesMod wingui/GDI port instead of ncursesw, guaranteeing
# truecolor for Awesome mode. OFF by default -> Windows keeps ncursesw. Linux
# is always ncursesw and ignores this. Defining it swaps the curses selection
# below AND makes Curses.h include <curses.h> (PDCurses) via the REMOCT_PDCURSES
# compile definition on the remoct target.
option(REMOCT_PDCURSES "Windows: render via vendored PDCursesMod wingui instead of ncursesw" OFF)

# ── Slice-1 instrumentation probe (TEMPORARY) ──────────────────────────────
# Toggle with -DREMOCT_PROBE=ON. Compiles the #ifdef REMOCT_PROBE colour-budget
# (Probe A1) and cover-art-render (Probe A2) logging. OFF by default -> the
# shipped path carries zero probe code. Removed when Slice 1 closes.
option(REMOCT_PROBE "Slice-1: compile temporary curses/cover-art probe logging" OFF)

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
    src/CoverArtRender.cpp        # decode + half-block render of cover art (Info pane)
    # Phase 4: the streaming source is driven through the plugin C ABI. Slice (c)
    # moved StreamSource + IHeartRadio + IHeartNowPlayingSM + IHeartDeepLog + the
    # adapter into plugins/stream/ (built as remoct_stream.{so,dll}); the host keeps
    # only the DRIVER side + the loader.
    src/PluginSource.cpp          # host-side driver over a RemoctPlugin table
    src/PluginHostServices.cpp    # RemoctHostServices HTTP shim over core::IHttp
    src/PluginHost.cpp            # loadPlugin / validatePlugin / LoadedPlugin (host is a loader now)
    src/ar_crc.cpp
    src/AacDecoder.cpp            # FDK-AAC file (.m4a/.m4b) backend — stays host (D5)
    src/OpusDecoder.cpp           # libopus/libopusfile (.opus) backend
    src/WavPackDecoder.cpp        # libwavpack (.wv) backend
    src/VorbisDecoder.cpp         # libvorbis/libvorbisfile (.ogg) backend
    src/CustomBackends.cpp        # the shared custom-backend array (playback + BPM)
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
    target_include_directories(remoct PRIVATE ${MSYS2_PREFIX}/include)
    if(NOT REMOCT_PDCURSES)
        target_include_directories(remoct PRIVATE ${MSYS2_PREFIX}/include/ncursesw)
    endif()
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

if(REMOCT_PROBE)
    target_compile_definitions(remoct PRIVATE REMOCT_PROBE)
    message(STATUS "REMOCT_PROBE: Slice-1 instrumentation ENABLED (temporary)")
endif()

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
if(WIN32 AND REMOCT_STATIC_PROBE)
    # fdk-aac stays DYNAMIC by design even under the static single-exe build:
    # the exe and the remoct_stream plugin share it, so one libfdk-aac.dll = one
    # codec state (static in the exe + a plugin copy would alias into two decoder
    # states). Force the import lib past the static-first suffix preference. This
    # normal-var set() shadows the .a the find_library above cached.
    set(FDKAAC_LIB "${MSYS2_PREFIX}/lib/libfdk-aac.dll.a")
endif()
message(STATUS "fdk-aac: ${FDKAAC_LIB}")

# ── libopus + libopusfile (Opus decode: .opus) ─────────────────────────────
find_library(OPUS_LIB      NAMES opus     PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_library(OPUSFILE_LIB  NAMES opusfile PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
# Headers nest exactly like fdk-aac/, identically on MSYS2 and Debian. TWO
# include entries are needed: the root (so our code can write the explicit
# <opus/opusfile.h> — TagLib ships its own unrelated opusfile.h, so a bare
# <opusfile.h> in OUR code is banned) and the opus/ dir itself, because
# opusfile.h includes its libopus siblings (<opus_multistream.h>) unprefixed —
# upstream mandates -I<prefix>/include/opus via its pkg-config Cflags.
# The bare-include ban is ENFORCED by the CI grep gate (ci.yml "Include-guard
# lint"); adding the opus/ dir before TagLib's include dir below is
# defense-in-depth only — do not rely on -I order as the guard.
find_path(OPUS_INCLUDE NAMES opus/opusfile.h PATHS "${MSYS2_PREFIX}/include" REQUIRED)
# libogg is opusfile's container layer. The dynamic import lib carries it
# transitively, but under REMOCT_STATIC_PROBE the static libopusfile.a needs
# it spelled out — for every target that links opusfile, not just remoct
# (whose probe block already added libogg.a for static libFLAC).
find_library(OGG_LIB NAMES ogg PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
message(STATUS "opusfile: ${OPUSFILE_LIB}")

# ── libwavpack (WavPack decode: .wv) ───────────────────────────────────────
find_library(WAVPACK_LIB NAMES wavpack PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_path(WAVPACK_INCLUDE NAMES wavpack/wavpack.h PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "wavpack: ${WAVPACK_LIB}")

# ── libvorbis + libvorbisfile (Ogg Vorbis decode: .ogg) ────────────────────
find_library(VORBIS_LIB     NAMES vorbis     PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
find_library(VORBISFILE_LIB NAMES vorbisfile PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
# Root include only: vorbisfile.h pulls its siblings via QUOTED includes
# (probe-verified — unlike opusfile's unprefixed-angle siblings), so no
# <prefix>/include/vorbis entry exists to order. The bare-include ban is the
# CI grep gate, which already covers vorbisfile.
find_path(VORBIS_INCLUDE NAMES vorbis/vorbisfile.h PATHS "${MSYS2_PREFIX}/include" REQUIRED)
message(STATUS "vorbisfile: ${VORBISFILE_LIB}")

# Added HERE (after the find_path calls) rather than in the include block at
# the top: that block interpolates its variables before find_path has set them
# (the FDKAAC_INCLUDE entry there expands empty and only works because the
# whole MSYS2 include dir is added separately).
target_include_directories(remoct PRIVATE
    ${OPUS_INCLUDE} ${OPUS_INCLUDE}/opus ${WAVPACK_INCLUDE} ${VORBIS_INCLUDE})

# ── Curses: PDCursesMod wingui (Windows+REMOCT_PDCURSES) or ncursesw ────────
if(WIN32 AND REMOCT_PDCURSES)
    # Vendored PDCursesMod (pinned; see lib/pdcursesmod/VENDOR.md). Build the
    # wingui/GDI port as a static lib: portable core (pdcurses/*.c) + the 7
    # platform TUs (wingui/*.c, which #include the needed common/*.c helpers).
    set(PDC_DIR "${CMAKE_SOURCE_DIR}/lib/pdcursesmod")
    file(GLOB PDC_CORE_SRC "${PDC_DIR}/pdcurses/*.c")
    set(PDC_WINGUI_SRC
        "${PDC_DIR}/wingui/pdcclip.c"  "${PDC_DIR}/wingui/pdcdisp.c"
        "${PDC_DIR}/wingui/pdcgetsc.c" "${PDC_DIR}/wingui/pdckbd.c"
        "${PDC_DIR}/wingui/pdcscrn.c"  "${PDC_DIR}/wingui/pdcsetsc.c"
        "${PDC_DIR}/wingui/pdcutil.c")
    add_library(pdcurses_wingui STATIC ${PDC_CORE_SRC} ${PDC_WINGUI_SRC})
    target_include_directories(pdcurses_wingui PUBLIC "${PDC_DIR}")
    # PDC_WIDE + PDC_FORCE_UTF8: single-column wide glyphs + UTF-8 (box-drawing,
    # viz blocks). The wingui GDI port draws its own colours (truecolor).
    target_compile_definitions(pdcurses_wingui PRIVATE PDC_WIDE PDC_FORCE_UTF8)
    target_link_libraries(pdcurses_wingui PUBLIC gdi32 comdlg32 winmm)
    # Silence third-party warnings; this is vendored code, not ours to fix.
    target_compile_options(pdcurses_wingui PRIVATE -w)
    message(STATUS "curses: PDCursesMod wingui (vendored, static) - Option C")
else()
    find_library(NCURSESW_LIB NAMES ncursesw
        PATHS "${MSYS2_PREFIX}/lib" REQUIRED)
    find_library(PANELW_LIB NAMES panelw
        PATHS "${MSYS2_PREFIX}/lib")
    message(STATUS "ncursesw: ${NCURSESW_LIB}")
endif()

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

# ── Curses linkage (selected above) ────────────────────────────────────────
if(WIN32 AND REMOCT_PDCURSES)
    target_link_libraries(remoct PRIVATE pdcurses_wingui)
    # Curses.h -> <curses.h> (PDCursesMod) instead of ncursesw.
    target_compile_definitions(remoct PRIVATE REMOCT_PDCURSES)
else()
    target_link_libraries(remoct PRIVATE
        ${NCURSESW_LIB}
        $<$<BOOL:${PANELW_LIB}>:${PANELW_LIB}>
    )
endif()

# ── Link ───────────────────────────────────────────────────────────────────
target_link_libraries(remoct PRIVATE
    ${TAGLIB_LIBRARIES}
    ${FLAC_LIB}
    ${LAME_LIB}
    ${EBUR128_LIB}
    ${FDKAAC_LIB}
    ${OPUSFILE_LIB}   # dependency order: opusfile -> opus, vorbisfile -> vorbis, both -> ogg
    ${OPUS_LIB}
    ${VORBISFILE_LIB}
    ${VORBIS_LIB}
    ${OGG_LIB}
    ${WAVPACK_LIB}
    $<$<PLATFORM_ID:Windows>:winmm>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Windows>:ole32>
    $<$<PLATFORM_ID:Windows>:wininet>
    $<$<PLATFORM_ID:Windows>:shell32>
    $<$<PLATFORM_ID:Windows>:advapi32>
    $<$<PLATFORM_ID:Windows>:dwmapi>   # DwmSetWindowAttribute: OS dark title bar (wingui)
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

# ── App icon (Windows) ─────────────────────────────────────────────────────
# icon/remoct.ico is a LOCAL, gitignored asset. When present, embed it as the
# exe's first icon resource: Explorer/taskbar/shortcut use it, and PDCursesMod
# wingui uses the exe's first icon for its window class (titlebar top-left). A
# generated .rc (build tree) points windres at the absolute path. Conditional,
# so a checkout without the asset still builds.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/icon/remoct.ico")
    enable_language(RC)
    set(REMOCT_ICON_RC "${CMAKE_BINARY_DIR}/remoct_icon.rc")
    file(WRITE "${REMOCT_ICON_RC}" "1 ICON \"${CMAKE_SOURCE_DIR}/icon/remoct.ico\"\n")
    target_sources(remoct PRIVATE "${REMOCT_ICON_RC}")
    message(STATUS "app icon: embedding icon/remoct.ico")
endif()

# ── App logo (radio-art floor) ─────────────────────────────────────────────
# remoct_logo.jpg is the RE-MOCT logo, shipped with the app: the Info pane
# renders it as the half-block cover-art floor when a radio song has no confident
# cover match. Copy it beside the exe (bin/) so logoBytes() finds it at <exeDir>,
# and install it to bin/. Conditional so a checkout without the asset still builds.
if(EXISTS "${CMAKE_SOURCE_DIR}/icon/remoct_logo.jpg")
    add_custom_command(TARGET remoct POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/icon/remoct_logo.jpg"
                "$<TARGET_FILE_DIR:remoct>/remoct_logo.jpg"
        VERBATIM)
    install(FILES "${CMAKE_SOURCE_DIR}/icon/remoct_logo.jpg" DESTINATION bin)
endif()

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
