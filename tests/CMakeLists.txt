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
add_executable(iheart_sm_test
    iheart_sm_test.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartNowPlayingSM.cpp
)
target_include_directories(iheart_sm_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
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
    ${CMAKE_SOURCE_DIR}/src/IHeartRadio.cpp
    ${HTTP_IMPL}
)
target_include_directories(iheart_request_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
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
    ${CMAKE_SOURCE_DIR}/src/StreamSource.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartRadio.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartNowPlayingSM.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartDeepLog.cpp
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
    ${HTTP_IMPL}
)
target_include_directories(hls_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib ${FDKAAC_INCLUDE})
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
    ${CMAKE_SOURCE_DIR}/src/StreamSource.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartRadio.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartNowPlayingSM.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartDeepLog.cpp
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
    ${HTTP_IMPL}
)
target_include_directories(icy_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib ${FDKAAC_INCLUDE})
target_link_libraries(icy_pipeline_test PRIVATE ${FDKAAC_LIB} ${HTTP_LIBS}
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Linux>:dl> $<$<PLATFORM_ID:Linux>:m>)
add_test(NAME icy_pipeline_test COMMAND icy_pipeline_test)
set_tests_properties(icy_pipeline_test PROPERTIES TIMEOUT 180)

if(WIN32)
    # slice 6 — Windows-only Discord IPC seam test. Links the REAL DiscordRP consumer
    # + the named-pipe impl TU (linked only to satisfy core::ipc(); never invoked —
    # the test injects a FakeIpc via constructor injection) and asserts the protocol
    # frames the consumer builds: handshake shape, endpoint probe order, SET_ACTIVITY
    # framing + jsonEscape, activity-null clear, CLOSE rejection, and lazy reconnect
    # after channel death. The live gate (real Discord shows RP) proves the transport.
    add_executable(discord_ipc_test
        discord_ipc_test.cpp
        ${CMAKE_SOURCE_DIR}/src/DiscordRP.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/IpcWinPipe.cpp
    )
    target_include_directories(discord_ipc_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    add_test(NAME discord_ipc_test COMMAND discord_ipc_test)

    # slice 8 — Windows-only CD-I/O seam test. Links the REAL CDSource consumer
    # + the IOCTL impl TU (linked only to satisfy core::cdio(); never invoked —
    # the test injects a FakeCdIo via constructor injection) and asserts the TOC
    # parse against canned discs: Relish-shaped non-standard pregap (T1 @ LBA 182),
    # Enhanced-CD data track + second-session leadout, malformed-TOC guards,
    # drive-model → AR offset lookup, media checks. The rip path's readRaw shape
    # is proven by the Joan Osborne gate (CDRipper's TU is too heavy to link).
    add_executable(cd_toc_test
        cd_toc_test.cpp
        ${CMAKE_SOURCE_DIR}/src/CDSource.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/CdIoWin.cpp
    )
    target_include_directories(cd_toc_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    add_test(NAME cd_toc_test COMMAND cd_toc_test)

    # ── Slice 0 (Phase 2 prep) — the thin replay net ──────────────────────────
    # Three tests that exercise the producer/consumer audio machinery the Source-
    # interface refactor restructures, headlessly, through the seams Phase 1 built.
    # A diff audit protects code that moves unopened; these protect the semantics
    # at the seams the refactor deliberately changes.

    # cd_pipeline_test: FakeCdIo through the REAL CDSource playback pipeline —
    # playTrack -> readerWorker -> SPSC ring -> readFrames. Byte-exact PCM fidelity
    # across the thread boundary, seek flush, pause continuity, backpressure,
    # silence-fill on transient errors, media-removal hard stop, teardown states.
    add_executable(cd_pipeline_test
        cd_pipeline_test.cpp
        ${CMAKE_SOURCE_DIR}/src/CDSource.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/CdIoWin.cpp
    )
    target_include_directories(cd_pipeline_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    add_test(NAME cd_pipeline_test COMMAND cd_pipeline_test)
    set_tests_properties(cd_pipeline_test PROPERTIES TIMEOUT 120)

    # xfade_handoff_test: the REAL AudioManager on generated WAVs through a real
    # (muted) device — the swap-adjacent file path Slice B restructures: gapless
    # EOF swap + track_swap_flag_ install, crossfade trigger/mix/completion,
    # clearNext mid-crossfade (device-stop quiesce branch), end-without-preload.
    # Needs an audio device: SKIPs (exit 77) when none can be opened.
    add_executable(xfade_handoff_test
        xfade_handoff_test.cpp
        ${CMAKE_SOURCE_DIR}/src/AudioManager.cpp
        ${CMAKE_SOURCE_DIR}/src/LocalFileSource.cpp
        ${CMAKE_SOURCE_DIR}/src/StreamSource.cpp
        ${CMAKE_SOURCE_DIR}/src/CDSource.cpp
        ${CMAKE_SOURCE_DIR}/src/IHeartRadio.cpp
        ${CMAKE_SOURCE_DIR}/src/IHeartNowPlayingSM.cpp
        ${CMAKE_SOURCE_DIR}/src/IHeartDeepLog.cpp
        ${CMAKE_SOURCE_DIR}/src/AacDecoder.cpp
        ${CMAKE_SOURCE_DIR}/src/Mp4Chapters.cpp
        ${CMAKE_SOURCE_DIR}/src/Log.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/CdIoWin.cpp
    )
    target_include_directories(xfade_handoff_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib
        ${FDKAAC_INCLUDE} ${TAGLIB_INCLUDE_DIRS})
    target_compile_definitions(xfade_handoff_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(xfade_handoff_test PRIVATE
        ${TAGLIB_LIBRARIES} ${FDKAAC_LIB} wininet winmm ole32)
    add_test(NAME xfade_handoff_test COMMAND xfade_handoff_test)
    set_tests_properties(xfade_handoff_test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()

