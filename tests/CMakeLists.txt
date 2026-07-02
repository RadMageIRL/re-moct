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
