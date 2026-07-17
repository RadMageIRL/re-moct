# Phase 0 pure-unit tests — link ONLY the pure units (no WinINet/ncurses/miniaudio),
# so this builds & runs on Linux CI as well as MSYS2/UCRT64.

# Per-platform HTTP transport for tests that link the REAL impl (slice 2): the
# same test drives WinINet on Windows and libcurl on Linux — identical scenarios
# and assertions against both transports, which IS the parity check.
if(WIN32)
    set(HTTP_IMPL ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp)
    set(HTTP_LIBS wininet)
else()
    set(HTTP_IMPL ${CMAKE_SOURCE_DIR}/src/platform/linux/HttpCurl.cpp)
    set(HTTP_LIBS CURL::libcurl pthread)
endif()

# Per-platform IPC transport (slice 4) — same idea: one test file drives the
# Windows named pipe AND the Linux Unix-domain socket.
if(WIN32)
    set(IPC_IMPL ${CMAKE_SOURCE_DIR}/src/platform/win/IpcWinPipe.cpp)
else()
    set(IPC_IMPL ${CMAKE_SOURCE_DIR}/src/platform/linux/IpcUnixSocket.cpp)
endif()

# Per-platform CD-I/O transport (slice 6) — cd_toc_test / cd_pipeline_test inject a
# FakeCdIo and link the real impl ONLY to satisfy core::cdio() (never invoked), so
# the same test files run on Windows (WinCdIo IOCTL) AND Linux (SgIoCdIo SG_IO).
# CDIO_LIBS carries the threading dep CDSource.cpp needs when linked on Linux.
if(WIN32)
    set(CDIO_IMPL ${CMAKE_SOURCE_DIR}/src/platform/win/CdIoWin.cpp)
    set(CDIO_LIBS "")
else()
    set(CDIO_IMPL ${CMAKE_SOURCE_DIR}/src/platform/linux/CdIoSgIo.cpp)
    set(CDIO_LIBS pthread)
endif()
# Per-platform plugin-loader transport (Phase 4 slice a) — the same idea as
# HTTP_IMPL/IPC_IMPL: one test file drives the Windows LoadLibrary loader AND the
# Linux dlopen loader through the identical core::loadPlugin policy.
if(WIN32)
    set(PLUGINLOADER_IMPL ${CMAKE_SOURCE_DIR}/src/platform/win/PluginLoaderWin.cpp)
    set(PLUGINLOADER_LIBS "")
else()
    set(PLUGINLOADER_IMPL ${CMAKE_SOURCE_DIR}/src/platform/linux/PluginLoaderPosix.cpp)
    set(PLUGINLOADER_LIBS dl)
endif()

add_executable(iheart_sm_test
    iheart_sm_test.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartNowPlayingSM.cpp   # moved in slice c
)
target_include_directories(iheart_sm_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/plugins/stream)
add_test(NAME iheart_sm_test COMMAND iheart_sm_test)

add_executable(ar_crc_test
    ar_crc_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ar_crc.cpp
)
target_include_directories(ar_crc_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ar_crc_test COMMAND ar_crc_test)

# core::INotify seam (slice 7) — pure FakeNotify-backed content test (no PowerShell,
# no process spawn, Linux-CI safe). Header-only consumer adapter (Toast.h), so no
# extra TUs; the transport (NotifyWinToast.cpp) is proven at the live toast gate.
add_executable(notify_toast_test notify_toast_test.cpp)
target_include_directories(notify_toast_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME notify_toast_test COMMAND notify_toast_test)

# notify-send argv builder (slice 5) — Linux-only, as the CD tests are Windows-
# only. Proves the injection-safety surface headless: buildNotifyArgv() puts
# title/body in argv slots verbatim (no shell) with a leading "--" so a
# dash-leading title can't be read as an option. The argv (NotifyArgv.h) lives
# under src/platform/linux/ — transport knowledge, not in the core headers.
if(UNIX)
    add_executable(notify_argv_test notify_argv_test.cpp)
    target_include_directories(notify_argv_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/platform/linux)
    add_test(NAME notify_argv_test COMMAND notify_argv_test)
endif()

# SG_IO CD-seam CDB builders (slice 6) — Linux-only, as the CD-IO transport is.
# Asserts the EXACT SCSI CDB bytes (CdbSgIo.h) headless: READ CD 0xBE byte-9
# 0x10/0x12 (the C2 request the non-C2 gate drive can't run live), LBA/transfer
# big-endian, READ TOC 0x43 MSF/format, SET CD SPEED 0xBB, INQUIRY, TUR. The
# CDBs live under src/platform/linux/ (transport knowledge), like NotifyArgv.h.
if(UNIX)
    add_executable(cdb_sgio_test cdb_sgio_test.cpp)
    target_include_directories(cdb_sgio_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/platform/linux)
    add_test(NAME cdb_sgio_test COMMAND cdb_sgio_test)
endif()

# core::IHttp seam — pure FakeHttp-backed parse test (no WinINet, Linux-CI safe).
add_executable(http_seam_test http_seam_test.cpp)
target_include_directories(http_seam_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
add_test(NAME http_seam_test COMMAND http_seam_test)

# Vendored MD5 (slice 2) — RFC 1321 reference vectors + padding-boundary lengths
# + a raw-UTF-8 api_sig-shaped vector, against lib/md5.c (the one signing code
# path behind LastFm's api_sig on BOTH platforms). Pure, both matrix jobs.
add_executable(md5_test
    md5_test.cpp
    ${CMAKE_SOURCE_DIR}/lib/md5.c
)
target_include_directories(md5_test PRIVATE ${CMAKE_SOURCE_DIR}/lib)
add_test(NAME md5_test COMMAND md5_test)

# Viz FFT (Slice C) - include/VizFFT.h proven in ISOLATION before it touched
# computeVizBins(): DC / bin-centred sines / 100 Hz-20 kHz sweep alias check
# (the old stride-4 DFT folds above ~sr/8) / Parseval / frame-cost gate vs the
# old per-band strided DFT. Pure, both matrix jobs.
add_executable(viz_fft_test viz_fft_test.cpp)
target_include_directories(viz_fft_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME viz_fft_test COMMAND viz_fft_test)

# core::IHttp POST seam (group b) — end-to-end request test. Links the real
# LastFm/ListenBrainz consumers + the REAL platform transport (satisfies
# core::http(); the test injects a FakeHttp via setHttp to assert the request
# each consumer builds: verb, content-type, auth header, UTF-8 body). Portable
# since slice 2 (both matrix jobs).
add_executable(scrobble_request_test
    scrobble_request_test.cpp
    ${CMAKE_SOURCE_DIR}/src/LastFm.cpp
    ${CMAKE_SOURCE_DIR}/src/ListenBrainz.cpp
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
    ${HTTP_IMPL}
    ${CMAKE_SOURCE_DIR}/lib/md5.c
)
target_include_directories(scrobble_request_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(scrobble_request_test PRIVATE ${HTTP_LIBS})
add_test(NAME scrobble_request_test COMMAND scrobble_request_test)

# group (c) — CoverArt request test. Links CoverArt (portable) + the real
# transport; injects a FakeHttp to assert per-call UA, reject_truncated, and the
# FollowSameScheme redirect policy. CDRipper AR/CTDB are covered by the pure
# urlIsSecureScheme test + the Joan Osborne rip gate (that TU is too heavy to
# link). Portable since slice 2.
add_executable(group_c_request_test
    group_c_request_test.cpp
    ${CMAKE_SOURCE_DIR}/src/CoverArt.cpp
    ${HTTP_IMPL}
)
target_include_directories(group_c_request_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(group_c_request_test PRIVATE ${HTTP_LIBS})
add_test(NAME group_c_request_test COMMAND group_c_request_test)

# slice 4 — REAL-socket test of the cancel + session capabilities (localhost
# fixture; a fake cannot prove these): prompt mid-body cancel (the "stop
# mid-segment must not hang 8s" gate), keep-alive connection reuse across one
# session (on Linux this is the live proof of the CURLSH share-handle pool),
# and pre-cancelled fetch never touching the network. Portable since slice 2 —
# one file, a platform shim, both transports.
add_executable(http_cancel_test
    http_cancel_test.cpp
    ${HTTP_IMPL}
)
target_include_directories(http_cancel_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(http_cancel_test PRIVATE ${HTTP_LIBS}
    $<$<PLATFORM_ID:Windows>:ws2_32>)
add_test(NAME http_cancel_test COMMAND http_cancel_test)
# Every wait in the fixture is bounded (~2s socket caps, 300ms cancel) — a run
# anywhere near this limit is a hang (the accept()-unblock class), not
# slowness. Fail fast in CI instead of eating ctest's 25-min default.
set_tests_properties(http_cancel_test PROPERTIES TIMEOUT 60)

# slice 4 — IHeart metadata request test. Links the REAL IHeartRadio consumer +
# the real transport and injects a FakeHttp to assert the session config (UA,
# 5s timeout) + per-request shape (Accept header, pragma, 4MB cap, no cancel
# token) and the three verified endpoints. Portable since slice 2.
add_executable(iheart_request_test
    iheart_request_test.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartRadio.cpp   # moved in slice c
    ${HTTP_IMPL}
)
target_include_directories(iheart_request_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib ${CMAKE_SOURCE_DIR}/plugins/stream)
target_link_libraries(iheart_request_test PRIVATE ${HTTP_LIBS})
add_test(NAME iheart_request_test COMMAND iheart_request_test)

# hls_pipeline_test: FakeHttp (core::setHttp) through the REAL StreamSource
# HLS/AAC pipeline — open -> hlsConnect -> producerWorkerAAC -> segment pump ->
# FDK decode -> ring -> readFrames. The Phase-0 Tier-2 replay idea realized on
# the slice-4 seam: synthetic manifests + real FDK-encoded ADTS. Prebuffer,
# underrun->rebuffer self-heal, timed-ID3 now-playing, and the prompt-close
# cancel gate (headless twin of the "mid-segment stop <1s" 7of9 gate). The
# ICY/continuous path (raw WinINet by design, Linux twin = slice 3) stays
# live-gate-only. Portable since slice 2 — the HLS pump runs on both platforms.
add_executable(hls_pipeline_test
    hls_pipeline_test.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/StreamSource.cpp        # moved in slice c
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartRadio.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartIdentity.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartNowPlayingSM.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartDeepLog.cpp
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
    ${HTTP_IMPL}
)
target_include_directories(hls_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib ${CMAKE_SOURCE_DIR}/plugins/stream ${FDKAAC_INCLUDE})
target_link_libraries(hls_pipeline_test PRIVATE ${FDKAAC_LIB} ${HTTP_LIBS}
    $<$<PLATFORM_ID:Linux>:dl> $<$<PLATFORM_ID:Linux>:m>)
add_test(NAME hls_pipeline_test COMMAND hls_pipeline_test)
set_tests_properties(hls_pipeline_test PROPERTIES TIMEOUT 120)

# icy_pipeline_test (slice 3): a REAL localhost ICY server fixture through the
# REAL StreamSource Continuous path — connect (raw WinINet / curl twin) ->
# readAudio de-interleave -> FDK -> ring -> readFrames. Closes the slice-0
# "ICY is live-gate-only" limit; run green on the Windows baseline BEFORE the
# Linux twin landed (baseline-first). Covers: Icy-MetaData request, metaint
# alignment across two title transitions (the offset-0 invariant), server-drop
# reconnect self-heal, bounded close on a silent stream, no-metaint passthrough.
add_executable(icy_pipeline_test
    icy_pipeline_test.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/StreamSource.cpp        # moved in slice c
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartRadio.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartIdentity.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartNowPlayingSM.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartDeepLog.cpp
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
    ${HTTP_IMPL}
)
target_include_directories(icy_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib ${CMAKE_SOURCE_DIR}/plugins/stream ${FDKAAC_INCLUDE})
target_link_libraries(icy_pipeline_test PRIVATE ${FDKAAC_LIB} ${HTTP_LIBS}
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Linux>:dl> $<$<PLATFORM_ID:Linux>:m>)
add_test(NAME icy_pipeline_test COMMAND icy_pipeline_test)
set_tests_properties(icy_pipeline_test PROPERTIES TIMEOUT 180)

# slice 6 — Discord IPC seam test. Links the REAL DiscordRP consumer + the
# platform impl TU (linked only to satisfy core::ipc(); never invoked — the
# test injects a FakeIpc via constructor injection) and asserts the protocol
# frames the consumer builds: handshake shape, endpoint probe order,
# SET_ACTIVITY framing + jsonEscape, activity-null clear, CLOSE rejection, and
# lazy reconnect after channel death. Windows-only until slice 4 delivered the
# Unix-socket impl; portable since — pure protocol, both matrix jobs.
add_executable(discord_ipc_test
    discord_ipc_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiscordRP.cpp
    ${IPC_IMPL}
)
target_include_directories(discord_ipc_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
add_test(NAME discord_ipc_test COMMAND discord_ipc_test)

# slice 4 — REAL-transport IPC test (the fixture twin of http_cancel_test): a
# real local server (Windows: CreateNamedPipe; Linux: Unix-socket listener in a
# sandboxed XDG_RUNTIME_DIR) through the REAL impl via core::ipc(). Proves the
# three primitives' semantics per platform: whole-buffer send, waitReadable
# min_bytes/timeout/broken-peer behavior, recvSome byte accounting + EOF, the
# dead-peer send (Linux: the MSG_NOSIGNAL regression case — a miss here is a
# SIGPIPE death, not an assert), and (Linux) the flatpak/snap/env discovery
# order. Run green on the untouched Windows baseline FIRST (slice-3 pattern).
add_executable(ipc_echo_test
    ipc_echo_test.cpp
    ${IPC_IMPL}
)
target_include_directories(ipc_echo_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(ipc_echo_test PRIVATE
    $<$<PLATFORM_ID:Linux>:pthread>)
add_test(NAME ipc_echo_test COMMAND ipc_echo_test)
# Every wait is bounded (2s waits, 300ms timeouts) — a run near this limit is a
# hang (a blocked ConnectNamedPipe/accept), not slowness. Fail fast in CI.
set_tests_properties(ipc_echo_test PROPERTIES TIMEOUT 60)

# slice 8 CD-I/O seam test — made portable at slice 6 (CDIO_IMPL). FakeCdIo via
# ctor injection through the REAL CDSource; the impl TU is linked only to satisfy
# core::cdio() (never invoked). Asserts the TOC parse against canned discs:
# Relish-shaped non-standard pregap (T1 @ LBA 182), Enhanced-CD data track +
# second-session leadout, malformed-TOC guards, drive-model → AR offset lookup,
# media checks. The rip path's readRaw shape is proven by the Joan Osborne gate
# (CDRipper's TU is too heavy to link). Runs on Windows AND Linux now.
add_executable(cd_toc_test
    cd_toc_test.cpp
    ${CMAKE_SOURCE_DIR}/src/CDSource.cpp
    ${CDIO_IMPL}
)
target_include_directories(cd_toc_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(cd_toc_test PRIVATE ${CDIO_LIBS})
add_test(NAME cd_toc_test COMMAND cd_toc_test)

# ── Slice 0 (Phase 2 prep) — the thin replay net ──────────────────────────────
# cd_pipeline_test: FakeCdIo through the REAL CDSource playback pipeline —
# playTrack -> readerWorker -> SPSC ring -> readFrames. Byte-exact PCM fidelity
# across the thread boundary, seek flush, pause continuity, backpressure,
# silence-fill on transient errors, media-removal hard stop, teardown states.
# Portable at slice 6 (timing via PortUtil: port::tickMs/port::sleepMs).
add_executable(cd_pipeline_test
    cd_pipeline_test.cpp
    ${CMAKE_SOURCE_DIR}/src/CDSource.cpp
    ${CDIO_IMPL}
)
target_include_directories(cd_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(cd_pipeline_test PRIVATE ${CDIO_LIBS})
add_test(NAME cd_pipeline_test COMMAND cd_pipeline_test)
set_tests_properties(cd_pipeline_test PROPERTIES TIMEOUT 120)

if(WIN32)
    # xfade_handoff_test: the REAL AudioManager on generated WAVs through a real
    # (muted) device — the swap-adjacent file path Slice B restructures: gapless
    # EOF swap + track_swap_flag_ install, crossfade trigger/mix/completion,
    # clearNext mid-crossfade (device-stop quiesce branch), end-without-preload.
    # Needs an audio device: SKIPs (exit 77) when none can be opened.
    # Slice c: AudioManager drives the stream through a LOADED plugin now, so it no
    # longer references any streaming-source symbol — this file crossfade test drops
    # the moved StreamSource/IHeart* stack and instead links the loader (PluginHost +
    # PluginLoaderWin). No .so sits beside the test binary, so loadPlugin returns null
    # and streaming is disabled (graceful) — irrelevant to the file crossfade path.
    add_executable(xfade_handoff_test
        xfade_handoff_test.cpp
        ${CMAKE_SOURCE_DIR}/src/AudioManager.cpp
        ${CMAKE_SOURCE_DIR}/src/LocalFileSource.cpp
        ${CMAKE_SOURCE_DIR}/src/PluginSource.cpp
        ${CMAKE_SOURCE_DIR}/src/PluginHostServices.cpp
        ${CMAKE_SOURCE_DIR}/src/PluginHost.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/PluginLoaderWin.cpp
        ${CMAKE_SOURCE_DIR}/src/CDSource.cpp
        ${CMAKE_SOURCE_DIR}/src/AacDecoder.cpp
        ${CMAKE_SOURCE_DIR}/src/OpusDecoder.cpp
        ${CMAKE_SOURCE_DIR}/src/WavPackDecoder.cpp
        ${CMAKE_SOURCE_DIR}/src/VorbisDecoder.cpp
        ${CMAKE_SOURCE_DIR}/src/CustomBackends.cpp
        ${CMAKE_SOURCE_DIR}/src/Mp4Chapters.cpp
        ${CMAKE_SOURCE_DIR}/src/Log.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/CdIoWin.cpp
    )
    target_include_directories(xfade_handoff_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib
        ${FDKAAC_INCLUDE} ${OPUS_INCLUDE} ${OPUS_INCLUDE}/opus ${WAVPACK_INCLUDE}
        ${TAGLIB_INCLUDE_DIRS})
    target_compile_definitions(xfade_handoff_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(xfade_handoff_test PRIVATE
        ${TAGLIB_LIBRARIES} ${FDKAAC_LIB} ${OPUSFILE_LIB} ${OPUS_LIB}
        ${VORBISFILE_LIB} ${VORBIS_LIB} ${OGG_LIB}
        ${WAVPACK_LIB} wininet winmm ole32)
    add_test(NAME xfade_handoff_test COMMAND xfade_handoff_test)
    set_tests_properties(xfade_handoff_test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()

# ── Phase 4 slice (a) — the plugin C-ABI: freeze the contract against a
# disposable plugin ──────────────────────────────────────────────────────────
# plugin_sine: a pure-C sine source built as a shared MODULE library. Compiled
# as C, so it also proves include/core/remoct_plugin.h is genuinely
# C-consumable (nothing C++ leaked across the ABI). PREFIX "" drops the "lib"
# so the loaded name is stable across platforms; the test loads it by absolute
# path, so no rpath/PATH juggling.
add_library(plugin_sine MODULE plugin_sine.c)
target_include_directories(plugin_sine PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(plugin_sine PROPERTIES PREFIX "")

# plugin_loader_test: loads plugin_sine from a REAL .dll/.so through the
# core::IPluginLoader seam + core::loadPlugin policy, drives the full lifecycle
# through the C table, and unit-tests the version gate's reject paths. Both jobs.
add_executable(plugin_loader_test
    plugin_loader_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginHost.cpp
    ${PLUGINLOADER_IMPL}
)
target_include_directories(plugin_loader_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(plugin_loader_test PRIVATE ${PLUGINLOADER_LIBS})
# The built plugin's absolute path, resolved at generate time.
target_compile_definitions(plugin_loader_test PRIVATE
    SINE_PLUGIN_PATH="$<TARGET_FILE:plugin_sine>")
add_dependencies(plugin_loader_test plugin_sine)
add_test(NAME plugin_loader_test COMMAND plugin_loader_test)
set_tests_properties(plugin_loader_test PROPERTIES TIMEOUT 60)

# plugin_http_shim_test: the host HTTP service shim (PluginHostServices) over a
# FakeHttp — proves the RemoctHostServices table translates requests/responses,
# passes the plugin's int32_t* cancel through with ZERO bridging, honors a set
# cancel, and owns/free's the response host-side. Deterministic; both jobs. (The
# real-transport int32 cancel granularity is locked by http_cancel_test.)
add_executable(plugin_http_shim_test
    plugin_http_shim_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginHostServices.cpp
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
)
target_include_directories(plugin_http_shim_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
add_test(NAME plugin_http_shim_test COMMAND plugin_http_shim_test)
set_tests_properties(plugin_http_shim_test PROPERTIES TIMEOUT 60)

# plugin_stream_test: the streaming source driven through the plugin C ABI — the
# in-process StreamSource adapter (remoct_stream_plugin_query) exercised via the
# raw C table AND via core::PluginSource (the driver AudioManager uses). Links the
# real StreamSource stack (like hls_pipeline_test) + the plugin host files; a
# dead-port open() fails fast so it's headless and quick. Both jobs.
# Slice c: the moved streaming stack lives in plugins/stream/; this test compiles it
# IN (the byte-identity reference D4 — the compiled-in adapter drives the SAME
# descriptor the .so exports). HostServiceHttp.cpp is added because the adapter now
# builds it from the host services (over core::http() here via HTTP_IMPL).
add_executable(plugin_stream_test
    plugin_stream_test.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/StreamPluginAdapter.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/HostServiceHttp.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginSource.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginHostServices.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/StreamSource.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartRadio.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartIdentity.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartNowPlayingSM.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartDeepLog.cpp
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
    ${HTTP_IMPL}
)
target_include_directories(plugin_stream_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib ${CMAKE_SOURCE_DIR}/plugins/stream ${FDKAAC_INCLUDE})
target_link_libraries(plugin_stream_test PRIVATE ${FDKAAC_LIB} ${HTTP_LIBS}
    $<$<PLATFORM_ID:Linux>:dl> $<$<PLATFORM_ID:Linux>:m>)
add_test(NAME plugin_stream_test COMMAND plugin_stream_test)
set_tests_properties(plugin_stream_test PROPERTIES TIMEOUT 60)

# plugin_hls_parity_test (Phase 4 slice d — the identical-to-compiled-in gate):
# runs identical synthetic HLS/ADTS through the REAL host HTTP service crossing
# (FakeHttp -> core::HostServices table -> plugin HostServiceHttp -> StreamSource)
# twice compiled-in (remoct_stream_plugin_query, D4) and once through the LOADED
# remoct_stream.{so,dll} (core::loadPlugin), asserting byte-identical PCM. It links
# the plugin stack compiled-in (the reference) + the plugin host + the loader, and
# depends on remoct_stream for the .so path. NO HTTP_IMPL: FakeHttp is the IHttp,
# so core::http() is never odr-used (proving the plugin needs no host transport).
add_executable(plugin_hls_parity_test
    plugin_hls_parity_test.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/StreamPluginAdapter.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/HostServiceHttp.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/StreamSource.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartRadio.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartIdentity.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartNowPlayingSM.cpp
    ${CMAKE_SOURCE_DIR}/plugins/stream/IHeartDeepLog.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginSource.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginHostServices.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginHost.cpp
    ${PLUGINLOADER_IMPL}
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
)
target_include_directories(plugin_hls_parity_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib ${CMAKE_SOURCE_DIR}/plugins/stream ${FDKAAC_INCLUDE})
# StreamSource's SACRED raw ICY loop (compiled-in here) needs its private raw
# transport — wininet / libcurl — NOT the core::IHttp seam (which stays out: HTTP
# crosses via the FakeHttp->HostServices table). Same libs the plugin target links.
target_link_libraries(plugin_hls_parity_test PRIVATE ${FDKAAC_LIB} ${PLUGINLOADER_LIBS}
    $<$<PLATFORM_ID:Windows>:wininet>
    $<$<PLATFORM_ID:Linux>:CURL::libcurl>
    $<$<PLATFORM_ID:Linux>:dl> $<$<PLATFORM_ID:Linux>:m> $<$<PLATFORM_ID:Linux>:pthread>)
target_compile_definitions(plugin_hls_parity_test PRIVATE
    REMOCT_STREAM_PATH="$<TARGET_FILE:remoct_stream>")
add_dependencies(plugin_hls_parity_test remoct_stream)
add_test(NAME plugin_hls_parity_test COMMAND plugin_hls_parity_test)
set_tests_properties(plugin_hls_parity_test PROPERTIES TIMEOUT 90)

