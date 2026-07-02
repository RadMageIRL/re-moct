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
        ${CMAKE_SOURCE_DIR}/src/HttpWinInet.cpp
        ${CMAKE_SOURCE_DIR}/src/Log.cpp
    )
    target_include_directories(scrobble_request_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/lib)
    target_link_libraries(scrobble_request_test PRIVATE wininet advapi32)
    add_test(NAME scrobble_request_test COMMAND scrobble_request_test)
endif()
