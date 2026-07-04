# plugins/stream/ — the streaming source as a loadable plugin (Phase 4 slice c).
#
# Built as a MODULE library exporting the ONE ABI symbol remoct_plugin_query
# (StreamPluginAdapter.cpp; PREFIX "" -> remoct_stream.{so,dll}). The monorepo home
# while the ABI churns (architecture.md: atomic host+plugin commits, one CI).
#
# Links fdk-aac + its OWN miniaudio impl (miniaudio_impl.cpp, MA_NO_DEVICE_IO) + the
# raw ICY transport (wininet / libcurl) — the sacred StreamSource read loop's private
# transport, which moves with the plugin byte-verbatim. It does NOT link the
# core::IHttp seam impl (HttpWinInet/HttpCurl stay host-side); iHeart/HLS HTTP crosses
# via the injected RemoctHostServices table (HostServiceHttp.cpp).
#
# Support files: Log.cpp (D1: the [stream] operational log; independent g_enabled
# residual documented in lessons.md) — StringUtils is header-only (no .cpp).

add_library(remoct_stream MODULE
    StreamPluginAdapter.cpp        # RemoctPlugin table + exported remoct_plugin_query (+ in-proc remoct_stream_plugin_query, D4)
    HostServiceHttp.cpp            # core::IHttp over the injected host services (inverse of the slice-b shim)
    StreamSource.cpp               # the engine — moved byte-verbatim (only the core::http() rewire + ctor threading changed)
    IHeartRadio.cpp
    IHeartIdentity.cpp             # probe-only minted anonymous profileId (deep-log A/B handshake identity)
    IHeartNowPlayingSM.cpp
    IHeartDeepLog.cpp
    miniaudio_impl.cpp             # MINIAUDIO_IMPLEMENTATION + MA_NO_DEVICE_IO (decode/convert only)
    ${CMAKE_SOURCE_DIR}/src/Log.cpp
)

# No "lib" prefix (matches plugin_sine): remoct_stream.so / remoct_stream.dll. The
# module lives beside the binary in bin/plugins so port::exeDir()/plugins resolves it
# (LIBRARY_ = Linux .so, RUNTIME_ = Windows .dll for a MODULE target).
set_target_properties(remoct_stream PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")

target_include_directories(remoct_stream PRIVATE
    ${CMAKE_SOURCE_DIR}/include     # core/*.h, PortUtil.h, StringUtils.h, Log.h
    ${CMAKE_SOURCE_DIR}/lib         # miniaudio.h, json.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}     # the plugin's own headers
    ${FDKAAC_INCLUDE})

target_link_libraries(remoct_stream PRIVATE
    ${FDKAAC_LIB}
    # Sacred raw ICY transport — StreamSource's private read loop (NOT the seam).
    $<$<PLATFORM_ID:Windows>:wininet>
    $<$<PLATFORM_ID:Linux>:CURL::libcurl>
    # miniaudio decode + std::thread producer + libm.
    $<$<PLATFORM_ID:Linux>:pthread>
    $<$<PLATFORM_ID:Linux>:dl>
    $<$<PLATFORM_ID:Linux>:m>)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(remoct_stream PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic
        -Wno-unused-parameter -Wno-old-style-cast>)
endif()
