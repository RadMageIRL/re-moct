# Phase 0 pure-unit tests — link ONLY the pure units (no WinINet/ncurses/miniaudio),
# so this builds & runs on Linux CI as well as MSYS2/UCRT64.
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

# core::IHttp POST seam (group b) — Windows-only end-to-end request test. Links the
# real LastFm/ListenBrainz consumers + the WinINet impl and injects a FakeHttp to
# assert the request each consumer builds (verb, content-type, auth header, UTF-8
# body). Windows-only because it links the consumers; Linux CI keeps the 3 pure suites.
if(WIN32)
    add_executable(scrobble_request_test
        scrobble_request_test.cpp
        ${CMAKE_SOURCE_DIR}/src/LastFm.cpp
        ${CMAKE_SOURCE_DIR}/src/ListenBrainz.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp
        ${CMAKE_SOURCE_DIR}/src/Log.cpp
    )
    target_include_directories(scrobble_request_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    target_link_libraries(scrobble_request_test PRIVATE wininet advapi32)
    add_test(NAME scrobble_request_test COMMAND scrobble_request_test)

    # group (c) — Windows-only CoverArt request test. Links CoverArt (portable) + the
    # WinINet impl; injects a FakeHttp to assert per-call UA, reject_truncated, and the
    # FollowSameScheme redirect policy. CDRipper AR/CTDB are covered by the pure
    # urlIsSecureScheme test + the Joan Osborne rip gate (that TU is too heavy to link).
    add_executable(group_c_request_test
        group_c_request_test.cpp
        ${CMAKE_SOURCE_DIR}/src/CoverArt.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp
    )
    target_include_directories(group_c_request_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    target_link_libraries(group_c_request_test PRIVATE wininet)
    add_test(NAME group_c_request_test COMMAND group_c_request_test)

    # slice 4 — Windows-only REAL-socket test of the WinINet cancel + session
    # capabilities (localhost fixture; a fake cannot prove these): prompt mid-body
    # cancel (the "stop mid-segment must not hang 8s" gate), keep-alive connection
    # reuse across one session, and pre-cancelled fetch never touching the network.
    add_executable(http_cancel_test
        http_cancel_test.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp
    )
    target_include_directories(http_cancel_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    target_link_libraries(http_cancel_test PRIVATE wininet ws2_32)
    add_test(NAME http_cancel_test COMMAND http_cancel_test)

    # slice 4 — Windows-only IHeart metadata request test. Links the REAL IHeartRadio
    # consumer + the WinINet impl and injects a FakeHttp to assert the session config
    # (UA, 5s timeout) + per-request shape (Accept header, pragma, 4MB cap, no cancel
    # token) and the three verified endpoints. hlsHttpGet's request shape is covered
    # by http_cancel_test + the live gates (StreamSource's TU is too heavy to link).
    add_executable(iheart_request_test
        iheart_request_test.cpp
        ${CMAKE_SOURCE_DIR}/src/IHeartRadio.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win/HttpWinInet.cpp
    )
    target_include_directories(iheart_request_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    target_link_libraries(iheart_request_test PRIVATE wininet)
    add_test(NAME iheart_request_test COMMAND iheart_request_test)

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
endif()
