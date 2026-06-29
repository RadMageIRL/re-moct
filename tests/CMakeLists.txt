# Phase 0 pure-unit tests — link ONLY the pure units (no WinINet/ncurses/miniaudio),
# so this builds & runs on Linux CI as well as MSYS2/UCRT64.
add_executable(iheart_sm_test
    iheart_sm_test.cpp
    ${CMAKE_SOURCE_DIR}/src/IHeartNowPlayingSM.cpp
)
target_include_directories(iheart_sm_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME iheart_sm_test COMMAND iheart_sm_test)
