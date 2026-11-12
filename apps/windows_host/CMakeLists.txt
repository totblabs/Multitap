cmake_minimum_required(VERSION 3.20)
project(MultitapHost VERSION 1.1.2 LANGUAGES CXX)
option(MULTITAP_SIGNIN_BACKEND "Compile the broker-admitted signin backend (machine preference defaults ON)" OFF)
option(MULTITAP_PRIVATE_SIGNIN_DIAGNOSTIC_TRIAL "Compile the local preparation diagnostic listener for private qualification only" OFF)
enable_language(RC)
# Explicit private policy/JSON memory tests. Neither opens a network connection
# nor reads credentials, Windows settings or native display/input state.
add_executable(MultitapHostTransportPolicyTest EXCLUDE_FROM_ALL ../../tools/host-transport-qa/fixture.cpp)
target_compile_features(MultitapHostTransportPolicyTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostTransportPolicyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostTransportJsonTest EXCLUDE_FROM_ALL src/broker_transport_test.cpp)
target_compile_features(MultitapHostTransportJsonTest PRIVATE cxx_std_20)
target_link_libraries(MultitapHostTransportJsonTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 normaliz)
target_compile_options(MultitapHostTransportJsonTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostStreamObservabilityTest EXCLUDE_FROM_ALL src/stream_observability_test.cpp)
target_compile_features(MultitapHostStreamObservabilityTest PRIVATE cxx_std_20)
target_link_libraries(MultitapHostStreamObservabilityTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 normaliz)
target_compile_options(MultitapHostStreamObservabilityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostObservationLogTest EXCLUDE_FROM_ALL src/host_observation_log_test.cpp)
target_compile_features(MultitapHostObservationLogTest PRIVATE cxx_std_20)
target_link_libraries(MultitapHostObservationLogTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 normaliz)
target_compile_options(MultitapHostObservationLogTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Tests that compile the whole service/agent exceed the default COFF section
# count (C1128) as the Host grows; every C++ target uses the big object format.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
# Product icons (tools/icons/build-icons.mjs): every product carries the amber
# tile with the dark power-strip mark as icon 1. MultitapHost.exe also carries
# the bare mark for its notification-area icon (2: amber for a dark taskbar,
# 3: dark for a light one; src/tray.hpp).
set(MULTITAP_CLIENT_PUBSPEC "${CMAKE_CURRENT_SOURCE_DIR}/../windows_client/pubspec.yaml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MULTITAP_CLIENT_PUBSPEC}")
file(STRINGS "${MULTITAP_CLIENT_PUBSPEC}" MULTITAP_CLIENT_VERSION_LINE REGEX "^version:")
if(NOT MULTITAP_CLIENT_VERSION_LINE MATCHES "^version: ([0-9]+\\.[0-9]+\\.[0-9]+)\\+[0-9]+$")
  message(FATAL_ERROR "Client installer requires a versioned Windows client.")
endif()
set(MULTITAP_CLIENT_VERSION "${CMAKE_MATCH_1}")
foreach(icon host client_setup)
  set(MULTITAP_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/${icon}.ico")
  set(MULTITAP_EXTRA_ICONS "")
  if(icon STREQUAL "host")
    set(MULTITAP_EXTRA_ICONS "2 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/resources/host_tray.ico\"\n3 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/resources/host_tray_light.ico\"")
  endif()
  set(MULTITAP_RESOURCE_PRODUCT "Multitap Host")
  set(MULTITAP_RESOURCE_DESCRIPTION "Multitap Host")
  set(MULTITAP_RESOURCE_VERSION "${PROJECT_VERSION}")
  if(icon STREQUAL "client_setup")
    set(MULTITAP_RESOURCE_PRODUCT "Multitap")
    set(MULTITAP_RESOURCE_DESCRIPTION "Multitap Installer")
    set(MULTITAP_RESOURCE_VERSION "${MULTITAP_CLIENT_VERSION}")
  endif()
  string(REPLACE "." "," MULTITAP_RESOURCE_VERSION_NUMERIC "${MULTITAP_RESOURCE_VERSION}.0")
  configure_file(src/product_icon.rc.in ${icon}_icon.rc @ONLY)
endforeach()
configure_file(src/product_versions.h.in product_versions.h @ONLY)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(MultitapHostStreamPrepareDiagnosticsTest EXCLUDE_FROM_ALL src/stream_prepare_diagnostics_test.cpp)
target_link_libraries(MultitapHostStreamPrepareDiagnosticsTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostStreamPrepareDiagnosticsTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# connect-timing-v1 (connect_stages.hpp): a stream start's stages.
add_executable(MultitapHostConnectStagesTest EXCLUDE_FROM_ALL src/connect_stages_test.cpp)
target_link_libraries(MultitapHostConnectStagesTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostConnectStagesTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostCaptureFollowTest EXCLUDE_FROM_ALL src/capture_follow_test.cpp)
target_link_libraries(MultitapHostCaptureFollowTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostCaptureFollowTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostStreamGpuPriorityTest EXCLUDE_FROM_ALL src/stream_gpu_priority_test.cpp)
target_link_libraries(MultitapHostStreamGpuPriorityTest PRIVATE advapi32 gdi32)
target_compile_options(MultitapHostStreamGpuPriorityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_library(host_engine STATIC src/media.cpp src/gpu_capture.cpp src/gpu_h264.cpp src/gpu_h264_nvenc.cpp src/gpu_h264_amf.cpp src/gpu_h264_qsv_mft.cpp src/drive_winfsp_helper_main.cpp)
# Distinct Winlogon path. The ordinary worker's role/desktop admission remains
# unchanged. Entry is dormant until the service explicitly authorizes this role.
target_sources(host_engine PRIVATE src/signin_worker_native.cpp src/signin_capture_dxgi.cpp
  src/signin_worker_channel.cpp src/signin_worker_entry.cpp src/signin_worker_command.cpp
  src/signin_preparation.cpp src/signin_restoration.cpp src/signin_diagnostic_inlet.cpp
  src/signin_diagnostic_client.cpp src/signin_diagnostic_coordinator.cpp src/signin_diagnostic_handoff.cpp src/signin_bridge.cpp)
target_link_libraries(host_engine PUBLIC winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 version)
if(MULTITAP_PRIVATE_SIGNIN_DIAGNOSTIC_TRIAL)
  target_compile_definitions(host_engine PUBLIC MULTITAP_PRIVATE_SIGNIN_DIAGNOSTIC_TRIAL=1)
endif()
# gpu-capture-v1 (gpu_capture.hpp): GPU NV12 conversion and cursor composition
# against the CPU path's bytes on WARP, the per-tick decisions, the encode
# policy, texture input/fallback of the encoder facade and the NVENC
# registration cache. Synthetic textures only; --benchmark measures per frame.
add_executable(MultitapHostGpuCaptureTest EXCLUDE_FROM_ALL src/gpu_capture_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostGpuCaptureTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostGpuCaptureTest PRIVATE host_engine)
target_compile_options(MultitapHostGpuCaptureTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Hardware MFT texture input (Intel QSV's path) on this PC's own GPU: texture vs byte path through the client decoder.
add_executable(MultitapHostMftTextureTest EXCLUDE_FROM_ALL src/mft_texture_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostMftTextureTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostMftTextureTest PRIVATE host_engine)
target_compile_options(MultitapHostMftTextureTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostGpuH264PolicyTest EXCLUDE_FROM_ALL src/gpu_h264_policy_test.cpp)
target_compile_options(MultitapHostGpuH264PolicyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostEncoderDiagnosticsTest EXCLUDE_FROM_ALL src/encoder_diagnostics_test.cpp)
target_compile_features(MultitapHostEncoderDiagnosticsTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostEncoderDiagnosticsTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNvencOutputPolicyTest EXCLUDE_FROM_ALL src/gpu_h264_nvenc_policy_test.cpp)
target_compile_features(MultitapHostNvencOutputPolicyTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostNvencOutputPolicyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostGpuH264InteropTest EXCLUDE_FROM_ALL src/gpu_h264_interop_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostGpuH264InteropTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostGpuH264InteropTest PRIVATE host_engine)
target_compile_options(MultitapHostGpuH264InteropTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set(MULTITAP_NVENC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc/nvEncodeAPI.h")
file(SHA256 "${MULTITAP_NVENC_HEADER}" MULTITAP_NVENC_HEADER_SHA256)
if(NOT MULTITAP_NVENC_HEADER_SHA256 STREQUAL "067c51597e586b72ced8127b2456f48dfe3a9f7d7e0d00dcbb26f9535c05b4ad")
  message(FATAL_ERROR "NVIDIA's pinned unmodified MIT API header is required; see third_party/nvenc/upstream.json.")
endif()
target_include_directories(host_engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc")
target_include_directories(MultitapHostNvencOutputPolicyTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc")
target_compile_definitions(MultitapHostNvencOutputPolicyTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(host_engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf/include")
target_compile_definitions(host_engine PRIVATE MULTITAP_NVENC=1)
set(MULTITAP_AMF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf")
file(SHA256 "${MULTITAP_AMF_ROOT}/upstream.json" MULTITAP_AMF_MANIFEST_SHA256)
if(NOT MULTITAP_AMF_MANIFEST_SHA256 STREQUAL "7132aa69d80f4f30ca1c5c601ddf22d4fb61c98dc1b4d5f7971aa92d3386cd6c")
  message(FATAL_ERROR "AMD's pinned public API source manifest is required; see third_party/amf/upstream.json.")
endif()
file(READ "${MULTITAP_AMF_ROOT}/upstream.json" MULTITAP_AMF_MANIFEST)
string(JSON MULTITAP_AMF_FILE_COUNT LENGTH "${MULTITAP_AMF_MANIFEST}" files)
math(EXPR MULTITAP_AMF_FILE_LAST "${MULTITAP_AMF_FILE_COUNT} - 1")
foreach(index RANGE 0 ${MULTITAP_AMF_FILE_LAST})
  string(JSON amf_path GET "${MULTITAP_AMF_MANIFEST}" files ${index} path)
  string(JSON amf_expected_sha256 GET "${MULTITAP_AMF_MANIFEST}" files ${index} sha256)
  string(JSON amf_expected_bytes GET "${MULTITAP_AMF_MANIFEST}" files ${index} bytes)
  if(NOT amf_path MATCHES "^(include/(core|components)/[A-Za-z0-9_]+\\.h|LICENSE\\.txt)$")
    message(FATAL_ERROR "AMD API source manifest contains an unexpected path.")
  endif()
  file(SHA256 "${MULTITAP_AMF_ROOT}/${amf_path}" amf_actual_sha256)
  file(SIZE "${MULTITAP_AMF_ROOT}/${amf_path}" amf_actual_bytes)
  if(NOT amf_actual_sha256 STREQUAL amf_expected_sha256 OR NOT amf_actual_bytes EQUAL amf_expected_bytes)
    message(FATAL_ERROR "AMD API source input differs from its pinned unmodified upstream bytes: ${amf_path}")
  endif()
endforeach()
target_include_directories(host_engine PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
# WinFsp SDK headers (third_party/winfsp/upstream.json): the DLL is never linked, the Host resolves its exports at run time.
target_include_directories(host_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/winfsp/inc")
add_library(host_clipboard STATIC ../../native/windows_clipboard/clipboard.cpp)
target_include_directories(host_clipboard PUBLIC ../../native/windows_clipboard)
target_compile_definitions(host_clipboard PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(host_clipboard PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(host_clipboard PUBLIC bcrypt advapi32 shell32 user32)
target_compile_definitions(host_engine PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(host_engine PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(host_engine PUBLIC mfplat mf mfuuid wmcodecdspuuid ole32 oleaut32 gdi32 user32 uuid d3d11 dxgi windowsapp cfgmgr32 secur32 netapi32 normaliz)
# libopus (BSD 3-clause; native/third_party/opus) encodes the stream's audio
# where the broker granted audio-opus-v1 (src/opus_audio.hpp).
include(../../native/third_party/opus/opus.cmake)
target_link_libraries(host_engine PUBLIC opus)
# x264 (GPLv2-or-later) as the production H.264 encoder, with automatic
# fallback to the Microsoft MFT (docs/x264-encoder.md). Release builds pass
# both paths (tools/host-installer/build-native.ps1); without them the Host
# is built MFT-only.
set(MULTITAP_X264_SRC "" CACHE PATH "Pinned unmodified x264 checkout (third_party/x264/README.md)")
set(MULTITAP_NASM "" CACHE FILEPATH "nasm.exe 2.13 or newer for the x264 assembly")
if(MULTITAP_X264_SRC OR MULTITAP_NASM)
  include(third_party/x264/x264.cmake)
  target_sources(host_engine PRIVATE src/x264_encoder.cpp)
  target_link_libraries(host_engine PUBLIC multitap_x264 advapi32 shell32)
  target_compile_definitions(host_engine PUBLIC MULTITAP_X264=1)
  # Real x264 + client decoder contract, live bitrate, epochs, VBV bound, fallback.
  add_executable(MultitapHostX264EncoderTest EXCLUDE_FROM_ALL src/x264_encoder_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
  target_include_directories(MultitapHostX264EncoderTest PRIVATE ../windows_client/windows/runner)
  target_link_libraries(MultitapHostX264EncoderTest PRIVATE host_engine)
  target_compile_options(MultitapHostX264EncoderTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
  add_executable(MultitapHostX264PolicyTest EXCLUDE_FROM_ALL src/x264_policy_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
  target_include_directories(MultitapHostX264PolicyTest PRIVATE ../windows_client/windows/runner)
  target_link_libraries(MultitapHostX264PolicyTest PRIVATE host_engine)
  target_compile_options(MultitapHostX264PolicyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
  add_executable(MultitapHostGpuH264FallbackTest EXCLUDE_FROM_ALL src/gpu_h264_fallback_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
  target_include_directories(MultitapHostGpuH264FallbackTest PRIVATE ../windows_client/windows/runner)
  target_link_libraries(MultitapHostGpuH264FallbackTest PRIVATE host_engine)
  target_compile_options(MultitapHostGpuH264FallbackTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
  # encoder_gpu_retry.hpp: the bounded way back to the GPU encoder (injected backend, clock and factory).
  add_executable(MultitapHostGpuRetryTest EXCLUDE_FROM_ALL src/gpu_retry_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
  target_include_directories(MultitapHostGpuRetryTest PRIVATE ../windows_client/windows/runner)
  target_link_libraries(MultitapHostGpuRetryTest PRIVATE host_engine)
  target_compile_options(MultitapHostGpuRetryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
# Production-path encoder benchmark (x264 and MFT behind host::Encoder with the
# production change-only/refinement policy); measurement only, not a test.
add_executable(MultitapHostEncoderBenchmark EXCLUDE_FROM_ALL src/encoder_benchmark.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostEncoderBenchmark PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostEncoderBenchmark PRIVATE host_engine)
target_compile_options(MultitapHostEncoderBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# connect-timing-v1: one-time stream setup costs (encoder, client decoder, process launch).
add_executable(MultitapHostConnectStartBenchmark EXCLUDE_FROM_ALL src/connect_start_benchmark.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostConnectStartBenchmark PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostConnectStartBenchmark PRIVATE host_engine)
target_compile_options(MultitapHostConnectStartBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set(MULTITAP_NATIVE_DISPLAY_SDK "" CACHE PATH "Inspected pinned libvirtualdisplay v1.6.5 source for native provider (stable control ABI)")
if(MULTITAP_NATIVE_DISPLAY_SDK)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER OFF CACHE BOOL "" FORCE)
  set(BUILD_VIRTUALDISPLAY_TOOLS OFF CACHE BOOL "" FORCE)
  set(BUILD_VIRTUALDISPLAY_PROBE OFF CACHE BOOL "" FORCE)
  set(BUILD_VIRTUALDISPLAY_VULKAN_LAYER OFF CACHE BOOL "" FORCE)
  add_subdirectory("${MULTITAP_NATIVE_DISPLAY_SDK}" native-display-sdk EXCLUDE_FROM_ALL)
  set_property(TARGET libvirtualdisplay_driver PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  target_link_libraries(host_engine PUBLIC libvirtualdisplay::driver)
  target_include_directories(host_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/display_bridge")
  target_compile_definitions(host_engine PUBLIC MULTITAP_NATIVE_DISPLAY=1)
endif()
add_executable(MultitapHost WIN32 src/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/host_icon.rc")
add_executable(MultitapHostTool src/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/host_icon.rc")
add_executable(MultitapHostInputTest src/input_test.cpp)
add_executable(MultitapHostPointerReliabilityTest EXCLUDE_FROM_ALL src/pointer_reliability_test.cpp)
add_executable(MultitapHostWorkerServiceBindingTest EXCLUDE_FROM_ALL src/worker_service_binding_test.cpp)
target_compile_options(MultitapHostWorkerServiceBindingTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostWorkerServiceBindingTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostPointerReliabilityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostPointerReliabilityTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
add_executable(MultitapWindowCloseTest src/window_close_test.cpp)
target_compile_definitions(MultitapWindowCloseTest PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(MultitapWindowCloseTest PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(MultitapWindowCloseTest PRIVATE user32)
add_executable(MultitapHostServiceTest src/service_test.cpp)
add_executable(MultitapHostSessionStopTest EXCLUDE_FROM_ALL src/session_stop_test.cpp)
target_compile_definitions(MultitapHostSessionStopTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST MULTITAP_SESSION_STOP_TEST)
target_compile_options(MultitapHostSessionStopTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSessionStopTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
add_executable(MultitapHostWorkerExitFenceTest EXCLUDE_FROM_ALL src/worker_exit_fence_test.cpp)
target_compile_options(MultitapHostWorkerExitFenceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostTrackSourceTest EXCLUDE_FROM_ALL src/track_source_test.cpp
  ../windows_client/windows/runner/remote_track_decoder.cpp
  ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostTrackSourceTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostTrackSourceTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostTrackSourceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostTrackTopologyTest EXCLUDE_FROM_ALL src/track_source_topology_test.cpp)
target_link_libraries(MultitapHostTrackTopologyTest PRIVATE host_engine)
target_compile_options(MultitapHostTrackTopologyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostTrackAgentTest EXCLUDE_FROM_ALL src/track_agent_test.cpp
  ../windows_client/windows/runner/remote_track_decoder.cpp
  ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostTrackAgentTest PRIVATE ../windows_client/windows/runner)
target_compile_definitions(MultitapHostTrackAgentTest PRIVATE MULTITAP_TRACK_SOURCE_TEST)
target_link_libraries(MultitapHostTrackAgentTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostTrackAgentTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostTrackSecureDesktopTest EXCLUDE_FROM_ALL src/track_secure_desktop_test.cpp)
target_compile_definitions(MultitapHostTrackSecureDesktopTest PRIVATE MULTITAP_TRACK_SOURCE_TEST)
target_link_libraries(MultitapHostTrackSecureDesktopTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostTrackSecureDesktopTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostAgentPublicationTest EXCLUDE_FROM_ALL src/agent_publication_test.cpp)
target_compile_definitions(MultitapHostAgentPublicationTest PRIVATE MULTITAP_AGENT_PUBLICATION_TEST MULTITAP_INPUT_LATENCY_TEST)
target_link_libraries(MultitapHostAgentPublicationTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostAgentPublicationTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Broker channel robustness under load: the send queue degrades a media
# backlog (drops video until an IDR), and every WebSocket message leaves in
# 8 KiB fragments (actual WinHTTP against a pinging loopback server).
# thumbnail-capture-v1: launcher pictures on request (substituted session,
# helper and privacy; synthetic pixels through the real WIC encoder; the
# actual Agent request/queue path). Never reads this PC's screen.
add_executable(MultitapHostThumbnailCaptureTest EXCLUDE_FROM_ALL src/thumbnail_capture_test.cpp)
target_compile_definitions(MultitapHostThumbnailCaptureTest PRIVATE MULTITAP_AGENT_PUBLICATION_TEST MULTITAP_INPUT_LATENCY_TEST)
target_link_libraries(MultitapHostThumbnailCaptureTest PRIVATE host_engine winhttp crypt32 windowscodecs)
target_compile_options(MultitapHostThumbnailCaptureTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostSendQueueTest EXCLUDE_FROM_ALL src/send_queue_test.cpp)
target_compile_options(MultitapHostSendQueueTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostWebSocketSendTest EXCLUDE_FROM_ALL src/websocket_send_test.cpp)
target_link_libraries(MultitapHostWebSocketSendTest PRIVATE winhttp crypt32 bcrypt ws2_32)
target_compile_options(MultitapHostWebSocketSendTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# host-resume-v1: which lost channels keep the stream and what the next channel offers.
add_executable(MultitapHostResumeTest EXCLUDE_FROM_ALL src/host_resume_test.cpp)
target_compile_options(MultitapHostResumeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# The clipboard channel's retry bound, and what a held and resumed stream changes about it.
add_executable(MultitapHostClipboardReconnectTest EXCLUDE_FROM_ALL src/clipboard_reconnect_test.cpp)
target_compile_options(MultitapHostClipboardReconnectTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# channel-resume-v1: which ends of a print, drive or USB channel are dialled again, whether the stream is away,
# and the bounded redial after a resume.
add_executable(MultitapHostChannelRedialTest EXCLUDE_FROM_ALL src/channel_redial_test.cpp)
target_compile_options(MultitapHostChannelRedialTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# TCP_NODELAY of the Host's WinHTTP WebSocket, read from its own socket (WinHTTP has no option for it).
add_executable(MultitapHostWebSocketNoDelayProbe EXCLUDE_FROM_ALL src/websocket_nodelay_probe.cpp)
target_link_libraries(MultitapHostWebSocketNoDelayProbe PRIVATE winhttp crypt32 bcrypt ws2_32)
target_compile_options(MultitapHostWebSocketNoDelayProbe PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# input-latency-v1: tagger/session math, the actual worker injection path and
# the actual Session0 publication path (substituted injector and bridge).
add_executable(MultitapHostInputLatencyTest EXCLUDE_FROM_ALL src/input_latency_test.cpp)
target_compile_definitions(MultitapHostInputLatencyTest PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostInputLatencyTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostInputLatencyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# cursor-v1: native cursor conversion, records, channel rules, composition
# switch, the worker input path and a synthetic encode CPU measurement.
add_executable(MultitapHostCursorTest EXCLUDE_FROM_ALL src/cursor_test.cpp)
target_compile_definitions(MultitapHostCursorTest PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostCursorTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostCursorTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# cursor-v1: the shape sent has the size Windows shows at the output's scale
# (cursor_channel.hpp shownFactor/extractScaled). Cursors are read and copied only.
add_executable(MultitapHostCursorShownSizeTest EXCLUDE_FROM_ALL src/cursor_shown_size_test.cpp)
target_compile_definitions(MultitapHostCursorShownSizeTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostCursorShownSizeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# text-input-v1: validation/bounds, Unicode INPUT sequence, session gating on
# the worker Agent path and Session0 forwarding (substituted injector).
add_executable(MultitapHostTextInputTest EXCLUDE_FROM_ALL src/text_input_test.cpp)
target_compile_definitions(MultitapHostTextInputTest PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostTextInputTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostTextInputTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# display-scale-v1: request validation, step math, keeper apply/restore and
# crash recovery (substituted DisplayConfig/record) and Session0 forwarding.
# stream-size-v1: decoder limits, full-size encoding, measured-load scale controller.
add_executable(MultitapHostStreamSizeTest EXCLUDE_FROM_ALL src/stream_size_test.cpp)
target_link_libraries(MultitapHostStreamSizeTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostStreamSizeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Secure desktop notice: UAC of an unlocked session keeps the stream with a notice picture.
add_executable(MultitapHostSecureDesktopNoticeTest EXCLUDE_FROM_ALL src/secure_desktop_notice_test.cpp)
target_link_libraries(MultitapHostSecureDesktopNoticeTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 gdi32 user32)
target_compile_options(MultitapHostSecureDesktopNoticeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Secure desktop scope: Role::secure scope matrix, input allowlist, helper source switch (pure).
add_executable(MultitapHostSecureDesktopScopeTest EXCLUDE_FROM_ALL src/secure_desktop_scope_test.cpp)
target_compile_options(MultitapHostSecureDesktopScopeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Secure desktop bridge with a fake helper: the frame section, BGRA scaling, the worker's source switch, the
# service's dispatch and input routing, the helper's control loop, and all of them end to end over a real unnamed
# section and the service's real input pipe (no SYSTEM process, no secure desktop).
add_executable(MultitapHostSecureDesktopBridgeTest EXCLUDE_FROM_ALL src/secure_desktop_bridge_test.cpp)
target_link_libraries(MultitapHostSecureDesktopBridgeTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 gdi32 user32)
target_compile_options(MultitapHostSecureDesktopBridgeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# System input (system_input.hpp): which stream gets the SYSTEM injector, the injector's loop, the service's routing
# and marker, pointer geometry, the pipe's reach, and the time of one input over the real pipe. Fakes only.
add_executable(MultitapHostSystemInputTest EXCLUDE_FROM_ALL src/system_input_test.cpp)
target_link_libraries(MultitapHostSystemInputTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 gdi32 user32)
target_compile_options(MultitapHostSystemInputTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# System input on independent outputs: the service keeps the client's event, the worker's real track plan places it,
# the injector gets it at that desktop pixel (two and three outputs, plan changes, answers a worker may not give).
add_executable(MultitapHostSecureDesktopTracksTest EXCLUDE_FROM_ALL src/secure_desktop_tracks_test.cpp)
target_link_libraries(MultitapHostSecureDesktopTracksTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 gdi32 user32)
target_compile_options(MultitapHostSecureDesktopTracksTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostSystemInputTracksTest EXCLUDE_FROM_ALL src/system_input_tracks_test.cpp)
target_link_libraries(MultitapHostSystemInputTracksTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 gdi32 user32)
target_compile_options(MultitapHostSystemInputTracksTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostResolutionV2Test EXCLUDE_FROM_ALL src/resolution_v2_test.cpp)
target_link_libraries(MultitapHostResolutionV2Test PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostResolutionV2Test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostDisplayScaleTest EXCLUDE_FROM_ALL src/display_scale_test.cpp)
target_compile_definitions(MultitapHostDisplayScaleTest PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostDisplayScaleTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostDisplayScaleTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# touch-input-v1: frame validation, coordinate mapping, injected touch frames,
# keep-alive/release/failure rules, worker Agent gating and Session0
# forwarding (substituted touch injection backend; no OS input).
# caret-v1: request gating, caret mapping, throttle, tracker smoke run.
add_executable(MultitapHostCaretFollowTest EXCLUDE_FROM_ALL src/caret_follow_test.cpp)
target_compile_definitions(MultitapHostCaretFollowTest PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostCaretFollowTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostCaretFollowTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostTouchInputTest EXCLUDE_FROM_ALL src/touch_input_test.cpp)
target_compile_definitions(MultitapHostTouchInputTest PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostTouchInputTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostTouchInputTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# relative-mouse-v1: the INPUT of a movement, refusals, worker Agent gating and
# Session0 forwarding (substituted injector; no OS input).
add_executable(MultitapHostRelativeMouseTest EXCLUDE_FROM_ALL src/relative_mouse_test.cpp)
target_compile_definitions(MultitapHostRelativeMouseTest PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostRelativeMouseTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostRelativeMouseTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# print-v1: grant, titles, printer security, printer/port lifecycle and
# leftovers (substituted spooler), session window/digest/cancel/revocation,
# protected job files and the loopback listener. No printer is created.
add_executable(MultitapHostPrintRedirectionTest EXCLUDE_FROM_ALL src/print_redirection_test.cpp)
target_link_libraries(MultitapHostPrintRedirectionTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi winspool)
target_compile_options(MultitapHostPrintRedirectionTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# print-v1 default printer: save / set / restore, user choice, crash recovery
# and the link-hardened record (substituted printer API; nothing is changed).
add_executable(MultitapHostPrintDefaultTest EXCLUDE_FROM_ALL src/print_default_test.cpp)
target_link_libraries(MultitapHostPrintDefaultTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi winspool)
target_compile_options(MultitapHostPrintDefaultTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# A standard (non-administrator) Windows user and the device features: every
# security descriptor the Host builds for the clipboard store, shared folders,
# the printer, the USB controller, its data folders and the worker pipe, asked
# of Windows with AccessCheck for a standard user and for another account
# (docs/standard-user-devices.md). Nothing is installed, mounted or changed.
add_executable(MultitapHostStandardUserAccessTest EXCLUDE_FROM_ALL src/standard_user_access_test.cpp)
target_link_libraries(MultitapHostStandardUserAccessTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi winspool setupapi newdev cfgmgr32 wintrust)
target_compile_options(MultitapHostStandardUserAccessTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# usb-v1: grant, descriptor policy (shared vectors with the broker), USB/IP and
# relay codecs, session logic with a fake kernel and client, controller
# preparation with a substituted Windows, the loopback server over sockets and
# the vhci ioctl ABI. No driver is installed and no USB device is touched.
add_executable(MultitapHostUsbRedirectionTest EXCLUDE_FROM_ALL src/usb_redirection_test.cpp)
target_compile_definitions(MultitapHostUsbRedirectionTest PRIVATE "MULTITAP_USB_VECTORS=\"${CMAKE_CURRENT_SOURCE_DIR}/../../server/test/fixtures/usb-descriptor-vectors.json\"")
target_link_libraries(MultitapHostUsbRedirectionTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi newdev cfgmgr32 wintrust)
target_compile_options(MultitapHostUsbRedirectionTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# 게임 마우스: the Host's own share of input latency on the virtual USB mouse path
# (inbox, core, real loopback USB/IP to a fake kernel) and on the SendInput path
# (worker Agent, substituted injector). Measures; injects and installs nothing.
add_executable(MultitapHostGameMouseLatency EXCLUDE_FROM_ALL src/game_mouse_latency_benchmark.cpp)
target_compile_definitions(MultitapHostGameMouseLatency PRIVATE MULTITAP_INPUT_LATENCY_TEST MULTITAP_AGENT_PUBLICATION_TEST)
target_link_libraries(MultitapHostGameMouseLatency PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi newdev cfgmgr32 wintrust)
target_compile_options(MultitapHostGameMouseLatency PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Explicit probe only (moves this PC's real cursor, then restores it).
add_executable(MultitapHostCursorCpuProbe EXCLUDE_FROM_ALL src/cursor_cpu_probe.cpp)
target_link_libraries(MultitapHostCursorCpuProbe PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostCursorCpuProbe PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostPendingConnectionTest EXCLUDE_FROM_ALL src/pending_connection_test.cpp)
# host-machine-v1: a registration is used only on the Windows it was approved on (memory only).
add_executable(MultitapHostCredentialBindingTest EXCLUDE_FROM_ALL src/credential_binding_test.cpp)
# The stored Windows account of the desktop (windows_account_record.hpp): parsing, validation, save order.
add_executable(MultitapHostWindowsAccountRecordTest EXCLUDE_FROM_ALL src/windows_account_record_test.cpp)
# drive-v1 Host side: name/path rules, the loopback WebDAV endpoint over real
# sockets (in-memory client), request multiplexer wire format and bounds,
# drive-letter helper names, and the session orchestrator (fake relay/mounter).
add_executable(MultitapHostDriveTest EXCLUDE_FROM_ALL src/drive_test.cpp)
target_link_libraries(MultitapHostDriveTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi mpr)
target_compile_options(MultitapHostDriveTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# WinFsp backend: callbacks through the real WinFsp interface table against a fake WinFsp, the service<->helper
# wire, the bridge and helper end to end over in-process pipes, the orchestrator with a fake WinFsp mounter,
# and the WinFsp install logic (setup_winfsp.hpp) with fake msiexec/registry. Nothing is installed or mounted.
add_executable(MultitapHostDriveFsTest EXCLUDE_FROM_ALL src/drive_winfsp_test.cpp)
target_link_libraries(MultitapHostDriveFsTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi mpr msi version)
target_compile_definitions(MultitapHostDriveFsTest PRIVATE "MULTITAP_WINFSP_PIN=\"${CMAKE_CURRENT_SOURCE_DIR}/third_party/winfsp/upstream.json\"")
target_compile_options(MultitapHostDriveFsTest PRIVATE /W4 /permissive- /EHsc /utf-8)
# WinFsp drive latency and throughput on the same fakes, behind a link with a fixed round trip, eight requests in flight and
# one shared bandwidth (drive_winfsp_bench.cpp). Nothing is mounted.
add_executable(MultitapHostDriveFsBench EXCLUDE_FROM_ALL src/drive_winfsp_bench.cpp)
target_link_libraries(MultitapHostDriveFsBench PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi mpr msi version)
target_compile_definitions(MultitapHostDriveFsBench PRIVATE "MULTITAP_WINFSP_PIN=\"${CMAKE_CURRENT_SOURCE_DIR}/third_party/winfsp/upstream.json\"")
target_compile_options(MultitapHostDriveFsBench PRIVATE /W4 /permissive- /EHsc /utf-8 /wd4505)
add_executable(MultitapHostSettingsRpcTest EXCLUDE_FROM_ALL src/settings_rpc_test.cpp)
add_executable(MultitapHostConsoleTransferTest EXCLUDE_FROM_ALL src/console_transfer_test.cpp)
add_executable(MultitapHostStreamConsoleRecoveryTest EXCLUDE_FROM_ALL src/stream_console_recovery_test.cpp)
add_executable(MultitapHostStreamConsoleWorkerTest EXCLUDE_FROM_ALL src/stream_console_worker_test.cpp)
target_link_libraries(MultitapHostStreamConsoleWorkerTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostStreamConsoleWorkerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostStreamConsoleRecoveryTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostStreamConsoleRecoveryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeDisplayContractTest EXCLUDE_FROM_ALL src/native_display_contract_test.cpp)
add_executable(MultitapHostNativeDisplayTopologyTest EXCLUDE_FROM_ALL src/native_display_topology_test.cpp)
# Memory-only coherent CCD query/merge cases; no device, desktop or file access.
add_executable(MultitapHostNativeActivationCcdTest EXCLUDE_FROM_ALL src/native_activation_ccd_test.cpp)
target_compile_features(MultitapHostNativeActivationCcdTest PRIVATE cxx_std_20)
target_include_directories(MultitapHostNativeActivationCcdTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostNativeActivationCcdTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostNativeActivationCcdTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostNativeActivationCcdTest PRIVATE user32)
add_executable(MultitapHostNativeCcdModeReferenceTest EXCLUDE_FROM_ALL src/native_ccd_mode_reference_test.cpp)
target_compile_features(MultitapHostNativeCcdModeReferenceTest PRIVATE cxx_std_20)
target_include_directories(MultitapHostNativeCcdModeReferenceTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostNativeCcdModeReferenceTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostNativeCcdModeReferenceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostNativeCcdModeReferenceTest PRIVATE user32)
add_executable(MultitapHostNativeReadinessModeReferenceTest EXCLUDE_FROM_ALL src/native_readiness_mode_reference_test.cpp)
target_compile_features(MultitapHostNativeReadinessModeReferenceTest PRIVATE cxx_std_20)
target_include_directories(MultitapHostNativeReadinessModeReferenceTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostNativeReadinessModeReferenceTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostNativeReadinessModeReferenceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostNativeReadinessModeReferenceTest PRIVATE user32)
add_executable(MultitapHostDisplayV2CaptureTest EXCLUDE_FROM_ALL src/display_v2_capture_test.cpp)
target_compile_features(MultitapHostDisplayV2CaptureTest PRIVATE cxx_std_20)
target_include_directories(MultitapHostDisplayV2CaptureTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostDisplayV2CaptureTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostDisplayV2CaptureTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostDisplayV2CaptureTest PRIVATE user32)
add_executable(MultitapHostNativeStreamPrimaryProfileTest EXCLUDE_FROM_ALL src/native_stream_primary_profile_test.cpp)
add_executable(MultitapHostNativeStreamExpiredConsoleTest EXCLUDE_FROM_ALL src/native_stream_expired_console_test.cpp)
target_link_libraries(MultitapHostNativeStreamExpiredConsoleTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostNativeStreamExpiredConsoleTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamExpiredConsoleTransactionTest EXCLUDE_FROM_ALL src/native_stream_expired_console_transaction_test.cpp)
target_link_libraries(MultitapHostNativeStreamExpiredConsoleTransactionTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamExpiredConsoleTransactionTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamExpiredRpcTest EXCLUDE_FROM_ALL src/native_stream_expired_rpc_test.cpp)
target_link_libraries(MultitapHostNativeStreamExpiredRpcTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamExpiredRpcTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamExpiredLedgerTest EXCLUDE_FROM_ALL src/native_stream_expired_ledger_test.cpp)
target_link_libraries(MultitapHostNativeStreamExpiredLedgerTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamExpiredLedgerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamCancelRecoveryTest EXCLUDE_FROM_ALL src/native_stream_cancel_recovery_test.cpp)
target_link_libraries(MultitapHostNativeStreamCancelRecoveryTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamCancelRecoveryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamCancelServiceTest EXCLUDE_FROM_ALL src/native_stream_cancel_service_test.cpp)
target_compile_definitions(MultitapHostNativeStreamCancelServiceTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostNativeStreamCancelServiceTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostNativeStreamCancelServiceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostInitialViewportTest EXCLUDE_FROM_ALL src/initial_viewport_test.cpp)
target_compile_definitions(MultitapHostInitialViewportTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostInitialViewportTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostInitialViewportTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostViewportEncoderLimitsTest EXCLUDE_FROM_ALL src/viewport_encoder_limits_test.cpp)
target_compile_definitions(MultitapHostViewportEncoderLimitsTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostViewportEncoderLimitsTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostViewportEncoderLimitsTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamTrackCommitTest EXCLUDE_FROM_ALL src/native_stream_track_commit_test.cpp)
target_link_libraries(MultitapHostNativeStreamTrackCommitTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostNativeStreamTrackCommitTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamTrackServiceTest EXCLUDE_FROM_ALL src/native_stream_track_service_test.cpp)
target_compile_definitions(MultitapHostNativeStreamTrackServiceTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostNativeStreamTrackServiceTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostNativeStreamTrackServiceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamTrackLedgerTest EXCLUDE_FROM_ALL src/native_stream_track_ledger_test.cpp)
target_link_libraries(MultitapHostNativeStreamTrackLedgerTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamTrackLedgerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamGroupExpiredTransactionTest EXCLUDE_FROM_ALL src/native_stream_group_expired_transaction_test.cpp)
target_link_libraries(MultitapHostNativeStreamGroupExpiredTransactionTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamGroupExpiredTransactionTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamPendingReservationTest EXCLUDE_FROM_ALL src/native_stream_pending_reservation_test.cpp)
target_link_libraries(MultitapHostNativeStreamPendingReservationTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamPendingReservationTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostInitialProfileTracksTest EXCLUDE_FROM_ALL src/initial_profile_tracks_test.cpp)
add_executable(MultitapHostProfileTrackResizeTest EXCLUDE_FROM_ALL src/profile_track_resize_test.cpp)
add_executable(MultitapHostProfileTrackCapabilityTest EXCLUDE_FROM_ALL src/profile_track_capability_test.cpp)
target_compile_definitions(MultitapHostProfileTrackCapabilityTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostProfileTrackCapabilityTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostProfileTrackCapabilityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_compile_definitions(MultitapHostProfileTrackResizeTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostProfileTrackResizeTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostProfileTrackResizeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_compile_definitions(MultitapHostInitialProfileTracksTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostInitialProfileTracksTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostInitialProfileTracksTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeCountObservedLayoutTest EXCLUDE_FROM_ALL src/native_count_observed_layout_test.cpp)
target_compile_definitions(MultitapHostNativeCountObservedLayoutTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostNativeCountObservedLayoutTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostNativeCountObservedLayoutTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostNativeStreamPrimaryProfileTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostNativeStreamPrimaryProfileTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamConsoleIdentityTest EXCLUDE_FROM_ALL src/native_stream_console_identity_test.cpp)
add_executable(MultitapHostNativeStreamConsoleLiveProbe EXCLUDE_FROM_ALL src/native_stream_console_live_probe.cpp)
target_link_libraries(MultitapHostNativeStreamConsoleLiveProbe PRIVATE host_engine setupapi cfgmgr32 winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostNativeStreamConsoleLiveProbe PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostNativeStreamConsoleIdentityTest PRIVATE host_engine setupapi cfgmgr32 winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostNativeStreamConsoleIdentityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamProfilePayloadTest EXCLUDE_FROM_ALL src/native_stream_profile_payload_test.cpp)
target_link_libraries(MultitapHostNativeStreamProfilePayloadTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostNativeStreamProfilePayloadTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamProfileJournalTest EXCLUDE_FROM_ALL src/native_stream_profile_journal_test.cpp)
target_link_libraries(MultitapHostNativeStreamProfileJournalTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamProfileJournalTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamProfileWorkerTest EXCLUDE_FROM_ALL src/native_stream_profile_worker_test.cpp)
target_link_libraries(MultitapHostNativeStreamProfileWorkerTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamProfileWorkerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamLaunchTest EXCLUDE_FROM_ALL src/native_stream_launch_test.cpp)
target_link_libraries(MultitapHostNativeStreamLaunchTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamLaunchTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeStreamReclaimTest EXCLUDE_FROM_ALL src/native_stream_reclaim_test.cpp)
target_link_libraries(MultitapHostNativeStreamReclaimTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeStreamReclaimTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeRecoveryResetTest EXCLUDE_FROM_ALL src/native_recovery_reset_test.cpp)
target_link_libraries(MultitapHostNativeRecoveryResetTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeRecoveryResetTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeRecoveryResetServiceTest EXCLUDE_FROM_ALL src/native_recovery_reset_service_test.cpp)
target_compile_definitions(MultitapHostNativeRecoveryResetServiceTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostNativeRecoveryResetServiceTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostNativeRecoveryResetServiceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeDisplayIdentityTest EXCLUDE_FROM_ALL src/native_display_identity_test.cpp)
target_link_libraries(MultitapHostNativeDisplayIdentityTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeDisplayIdentityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostNativeDisplayServiceChangeTest EXCLUDE_FROM_ALL src/native_display_service_change_test.cpp)
if(MULTITAP_NATIVE_DISPLAY_SDK)
  add_executable(MultitapHostNativeRenderPreferenceTest EXCLUDE_FROM_ALL src/native_render_adapter_test.cpp)
  target_link_libraries(MultitapHostNativeRenderPreferenceTest PRIVATE host_engine)
  target_compile_options(MultitapHostNativeRenderPreferenceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
target_compile_options(MultitapHostNativeDisplayServiceChangeTest PRIVATE /WX)
add_executable(MultitapHostNativeDisplayJournalTest EXCLUDE_FROM_ALL src/native_display_journal_test.cpp)
target_compile_definitions(MultitapHostNativeDisplayServiceChangeTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
add_executable(MultitapHostNativeProfileViewportRecoveryTest EXCLUDE_FROM_ALL src/native_display_service_change_test.cpp)
target_compile_definitions(MultitapHostNativeProfileViewportRecoveryTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST MULTITAP_PROFILE_VIEWPORT_RECOVERY_ONLY)
target_link_libraries(MultitapHostNativeProfileViewportRecoveryTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostNativeProfileViewportRecoveryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Display manager v2: pure CCD/SDK/state fixtures, and the native SDK service glue.
add_executable(MultitapHostDisplayV2Test EXCLUDE_FROM_ALL src/display_v2_test.cpp)
# Resize stage fields (resize_stages.hpp); pure checks.
add_executable(MultitapHostResizeStagesTest EXCLUDE_FROM_ALL src/resize_stages_test.cpp)
target_include_directories(MultitapHostResizeStagesTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostResizeStagesTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostResizeStagesTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# What a resize restart costs after the display change: encoder creation, its
# first IDR, the client's decoder start and first picture, on generated frames
# at the field's sizes. Measurement only, not a test; no display is touched.
add_executable(MultitapHostResizeRestartBenchmark EXCLUDE_FROM_ALL src/resize_restart_benchmark.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostResizeRestartBenchmark PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostResizeRestartBenchmark PRIVATE host_engine)
target_compile_options(MultitapHostResizeRestartBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Faster stream start (media_warmup.hpp): the kept/warmed capture device and
# its hand-over rules with substituted devices, the cached WGC support answer,
# a real hardware D3D11 device (no capture, display, audio stream or input),
# and the worker's call sites.
add_executable(MultitapHostFastStartTest EXCLUDE_FROM_ALL src/fast_start_test.cpp)
target_link_libraries(MultitapHostFastStartTest PRIVATE host_engine)
target_compile_definitions(MultitapHostFastStartTest PRIVATE "MULTITAP_AGENT_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/agent.hpp\"" "MULTITAP_SERVICE_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/service.hpp\"" "MULTITAP_MEDIA_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/media.cpp\"")
target_compile_options(MultitapHostFastStartTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostRenderObservationTest EXCLUDE_FROM_ALL src/render_observation_test.cpp)
target_compile_options(MultitapHostRenderObservationTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# udp-media-v1 Phase 2: the probe's rules without sockets, then its real socket leg against a local peer.
add_executable(MultitapHostUdpProbeTest EXCLUDE_FROM_ALL src/udp_probe_test.cpp)
target_link_libraries(MultitapHostUdpProbeTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostUdpProbeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostUdpProbeLoopbackTest EXCLUDE_FROM_ALL src/udp_probe_loopback_test.cpp)
target_link_libraries(MultitapHostUdpProbeLoopbackTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostUdpProbeLoopbackTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# host-route-v1: the Broker's in-network address offer, parsing and the fallback to DNS.
add_executable(MultitapHostBrokerRouteTest EXCLUDE_FROM_ALL src/broker_route_test.cpp)
target_compile_options(MultitapHostBrokerRouteTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# UDP media core (native/windows_video/udp_media*.hpp): vectors, framing, repair logic and a simulated link. No sockets.
add_executable(MultitapHostUdpMediaTest EXCLUDE_FROM_ALL src/udp_media_test.cpp)
add_executable(MultitapHostUdpDirectTest EXCLUDE_FROM_ALL src/udp_direct_test.cpp)
add_executable(MultitapHostUdpTrackMediaTest EXCLUDE_FROM_ALL ../../native/windows_video/udp_track_media_test.cpp)
target_compile_options(MultitapHostUdpTrackMediaTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostUdpTracksSessionTest EXCLUDE_FROM_ALL src/udp_tracks_session_test.cpp)
target_compile_definitions(MultitapHostUdpTracksSessionTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostUdpTracksSessionTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostUdpTracksSessionTest PRIVATE bcrypt ws2_32 crypt32)
target_include_directories(MultitapHostUdpTracksSessionTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(MultitapHostUdpTracksLoopbackTest EXCLUDE_FROM_ALL src/udp_tracks_loopback_test.cpp ../windows_client/windows/runner/udp_media_leg.cpp)

target_compile_definitions(MultitapHostUdpTracksLoopbackTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

target_compile_options(MultitapHostUdpTracksLoopbackTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

target_link_libraries(MultitapHostUdpTracksLoopbackTest PRIVATE bcrypt ws2_32 crypt32)

target_include_directories(MultitapHostUdpTracksLoopbackTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(MultitapHostDirectInputTest EXCLUDE_FROM_ALL src/direct_input_test.cpp)
target_compile_options(MultitapHostDirectInputTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(MultitapHostDirectInputTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostDirectInputTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(MultitapHostUdpDirectTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostUdpDirectTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostUdpDirectTest PRIVATE bcrypt)
add_library(MultitapClientUdpDirectCompile OBJECT EXCLUDE_FROM_ALL ../windows_client/windows/runner/udp_media_leg.cpp)
target_compile_definitions(MultitapClientUdpDirectCompile PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapClientUdpDirectCompile PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set_target_properties(MultitapClientUdpDirectCompile PROPERTIES CXX_STANDARD 17)
add_executable(MultitapHostUdpDirectLoopbackTest EXCLUDE_FROM_ALL src/udp_direct_loopback_test.cpp ../windows_client/windows/runner/udp_media_leg.cpp)
target_compile_definitions(MultitapHostUdpDirectLoopbackTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostUdpDirectLoopbackTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostUdpDirectLoopbackTest PRIVATE bcrypt ws2_32 crypt32)
target_include_directories(MultitapHostUdpDirectLoopbackTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostUdpMediaTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostUdpMediaTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostUdpMediaTest PRIVATE bcrypt)
# udp-media-v1 Phase 3: the path's rules with injected time, then the real Agent and leg against a fake relay and the core Receiver.
add_executable(MultitapHostUdpMediaPathTest EXCLUDE_FROM_ALL src/udp_media_path_test.cpp)
target_link_libraries(MultitapHostUdpMediaPathTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostUdpMediaPathTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostUdpMediaLoopbackTest EXCLUDE_FROM_ALL src/udp_media_loopback_test.cpp)
target_compile_definitions(MultitapHostUdpMediaLoopbackTest PRIVATE MULTITAP_AGENT_PUBLICATION_TEST MULTITAP_INPUT_LATENCY_TEST)
target_link_libraries(MultitapHostUdpMediaLoopbackTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostUdpMediaLoopbackTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# audio-opus-v1 (opus_audio.hpp): negotiation, stream.info shape, framing and
# sequence, one datagram per packet, round-trip quality and encode cost. Synthetic PCM only.
add_executable(MultitapHostOpusAudioTest EXCLUDE_FROM_ALL src/opus_audio_test.cpp)
target_link_libraries(MultitapHostOpusAudioTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostOpusAudioTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostQemuConsoleTest EXCLUDE_FROM_ALL src/qemu_console_test.cpp)
# ime-state-v1: the console session's 한/영 mode for the browser Console (memory only).
add_executable(MultitapHostImeStateTest EXCLUDE_FROM_ALL src/ime_state_test.cpp)
target_link_libraries(MultitapHostImeStateTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostImeStateTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostQemuConsoleTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostQemuConsoleTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostDisplayV2Test PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostDisplayV2Test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostDisplayV2TraceTest EXCLUDE_FROM_ALL src/display_v2_trace_test.cpp)
target_link_libraries(MultitapHostDisplayV2TraceTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostDisplayV2TraceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostDisplayV2CountTest EXCLUDE_FROM_ALL src/display_v2_count_test.cpp)
target_link_libraries(MultitapHostDisplayV2CountTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostDisplayV2CountTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostDisplayV2ServiceTest EXCLUDE_FROM_ALL src/display_v2_service_test.cpp)
target_compile_definitions(MultitapHostDisplayV2ServiceTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_link_libraries(MultitapHostDisplayV2ServiceTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostDisplayV2ServiceTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostBrokerStatusTest EXCLUDE_FROM_ALL src/broker_status_test.cpp)
target_compile_options(MultitapHostBrokerStatusTest PRIVATE /WX)
# host-resources-v1: in-guest CPU/memory sampler math (fake sources) and report policy.
add_executable(MultitapHostResourceSamplerTest EXCLUDE_FROM_ALL src/resource_sampler_test.cpp)
target_compile_options(MultitapHostResourceSamplerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostViewportDiagnosticsTest EXCLUDE_FROM_ALL src/viewport_diagnostics_test.cpp)
add_executable(MultitapHostUpdateTest EXCLUDE_FROM_ALL src/host_update_test.cpp)
target_link_libraries(MultitapHostUpdateTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 ole32 bcrypt wtsapi32 userenv ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostUpdateTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# pc-power-v1: hello, request decisions and refusal codes with a substituted
# Windows call; the real shutdown path is not compiled into this fixture.
add_executable(MultitapHostPowerTest EXCLUDE_FROM_ALL src/host_power_test.cpp)
target_compile_definitions(MultitapHostPowerTest PRIVATE MULTITAP_HOST_POWER_TEST "MULTITAP_HOST_POWER_SOURCE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/host_power.hpp\"")
target_link_libraries(MultitapHostPowerTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 powrprof)
target_compile_options(MultitapHostPowerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostDiagnosticsTest EXCLUDE_FROM_ALL src/host_diagnostics_test.cpp)
target_link_libraries(MultitapHostDiagnosticsTest PRIVATE host_engine host_clipboard)
target_compile_options(MultitapHostDiagnosticsTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# boot-ready-v1 (boot_timeline.hpp): stage marks, the one record, log fields, loop wait and wake.
add_executable(MultitapHostBootTimelineTest EXCLUDE_FROM_ALL src/boot_timeline_test.cpp)
target_link_libraries(MultitapHostBootTimelineTest PRIVATE host_engine)
target_compile_options(MultitapHostBootTimelineTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Measurement tool (wake_counter.hpp): wakeups, CPU, handles and private bytes of
# a running process over a window, from a read-only process snapshot.
add_executable(MultitapHostWakeProbe EXCLUDE_FROM_ALL src/wake_probe.cpp)
target_compile_options(MultitapHostWakeProbe PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Idle wakeups (#70): the always-on service threads, measured in this process
# with QA pipe names, a temporary folder and a substituted clock.
add_executable(MultitapHostIdleWakeTest EXCLUDE_FROM_ALL src/idle_wake_test.cpp)
target_link_libraries(MultitapHostIdleWakeTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32)
target_compile_options(MultitapHostIdleWakeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# hostAction 275 (#69): the service run marker, owned worker exits (own child
# processes) and the record's place in the diagnostics report.
add_executable(MultitapHostProcessExitTest EXCLUDE_FROM_ALL src/process_exit_test.cpp)
target_link_libraries(MultitapHostProcessExitTest PRIVATE host_engine host_clipboard)
target_compile_options(MultitapHostProcessExitTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Memory over days (#71): long-lived service/stream state driven through many
# sessions and requests with fakes (in-memory helper pipes) stays bounded.
add_executable(MultitapHostBoundedStateTest EXCLUDE_FROM_ALL src/bounded_state_test.cpp)
target_link_libraries(MultitapHostBoundedStateTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi mpr setupapi cfgmgr32)
target_compile_options(MultitapHostBoundedStateTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_compile_options(MultitapHostViewportDiagnosticsTest PRIVATE /WX)
add_executable(MultitapHostCatalogMaintenanceTest EXCLUDE_FROM_ALL src/catalog_maintenance_test.cpp)
target_compile_options(MultitapHostCatalogMaintenanceTest PRIVATE /WX)
add_executable(MultitapHostFrameEncodePolicyTest EXCLUDE_FROM_ALL src/frame_encode_policy_test.cpp)
target_compile_options(MultitapHostFrameEncodePolicyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostStreamWorkerWatchdogTest EXCLUDE_FROM_ALL src/stream_worker_watchdog_test.cpp)
target_compile_features(MultitapHostStreamWorkerWatchdogTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostStreamWorkerWatchdogTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostFrameRefinementTest EXCLUDE_FROM_ALL src/frame_refinement_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostFrameRefinementTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostFrameRefinementTest PRIVATE host_engine)
target_compile_options(MultitapHostFrameRefinementTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Adaptive bitrate (docs/adaptive-bitrate.md): pure controller on a simulated
# link, and the actual MFT/decoder loop for a live mean-bitrate change.
add_executable(MultitapHostBitrateControllerTest EXCLUDE_FROM_ALL src/bitrate_controller_test.cpp)
target_compile_options(MultitapHostBitrateControllerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostEncoderBitrateTest EXCLUDE_FROM_ALL src/encoder_bitrate_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostEncoderBitrateTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostEncoderBitrateTest PRIVATE host_engine)
target_compile_options(MultitapHostEncoderBitrateTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostLosslessTilesTest EXCLUDE_FROM_ALL src/lossless_tiles_test.cpp)
target_link_libraries(MultitapHostLosslessTilesTest PRIVATE host_engine)
target_compile_options(MultitapHostLosslessTilesTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostLosslessTilesEncoderTest EXCLUDE_FROM_ALL src/lossless_tiles_encoder_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostLosslessTilesEncoderTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostLosslessTilesEncoderTest PRIVATE host_engine)
target_compile_options(MultitapHostLosslessTilesEncoderTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostLosslessTilesScaledTest EXCLUDE_FROM_ALL src/lossless_tiles_scaled_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostLosslessTilesScaledTest PRIVATE ../windows_client/windows/runner)
target_link_libraries(MultitapHostLosslessTilesScaledTest PRIVATE host_engine)
target_compile_options(MultitapHostLosslessTilesScaledTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostLosslessTrackAgentTest EXCLUDE_FROM_ALL src/lossless_track_agent_test.cpp
  ../windows_client/windows/runner/remote_track_decoder.cpp
  ../windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapHostLosslessTrackAgentTest PRIVATE ../windows_client/windows/runner)
target_compile_definitions(MultitapHostLosslessTrackAgentTest PRIVATE MULTITAP_TRACK_SOURCE_TEST)
target_link_libraries(MultitapHostLosslessTrackAgentTest PRIVATE host_engine winhttp crypt32)
target_compile_options(MultitapHostLosslessTrackAgentTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostCaptureCacheTest EXCLUDE_FROM_ALL ../../tools/capture-cache-qa/benchmark.cpp)
target_include_directories(MultitapHostCaptureCacheTest PRIVATE src)
target_link_libraries(MultitapHostCaptureCacheTest PRIVATE host_engine)
target_compile_options(MultitapHostCaptureCacheTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostCapturePatchTest EXCLUDE_FROM_ALL src/capture_patch_test.cpp)
target_link_libraries(MultitapHostCapturePatchTest PRIVATE host_engine)
target_compile_options(MultitapHostCapturePatchTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Scaled capture path: area-average resampler (SIMD == scalar statement, exact-average bound).
add_executable(MultitapHostCaptureScaleTest EXCLUDE_FROM_ALL src/capture_scale_test.cpp)
target_compile_options(MultitapHostCaptureScaleTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Change -> capture latency of the loop pacing (fixed tick vs WGC frame event),
# optionally with CPU contention and StreamPriority; captures only its own window.
add_executable(MultitapHostCapturePacingTest EXCLUDE_FROM_ALL src/capture_pacing_test.cpp)
target_compile_definitions(MultitapHostCapturePacingTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(MultitapHostCapturePacingTest PRIVATE d3d11 dxgi windowsapp user32 gdi32)
target_compile_options(MultitapHostCapturePacingTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostCapturePacingBenchmark EXCLUDE_FROM_ALL src/capture_pacing_benchmark.cpp)
target_link_libraries(MultitapHostCapturePacingBenchmark PRIVATE host_engine)
target_compile_options(MultitapHostCapturePacingBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# capture-dirty-v1 (capture_dirty.hpp): readback of the whole frame against the
# dirty rectangles, with a real WGC of the benchmark's own window; byte-equal check.
add_executable(MultitapHostCaptureReadbackBenchmark EXCLUDE_FROM_ALL src/capture_readback_benchmark.cpp)
target_link_libraries(MultitapHostCaptureReadbackBenchmark PRIVATE host_engine)
target_compile_options(MultitapHostCaptureReadbackBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostCaptureDirtyTest EXCLUDE_FROM_ALL src/capture_dirty_test.cpp)
target_compile_options(MultitapHostCaptureDirtyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# The real Capture on a WARP device against a whole-frame capture of the same monitor (reads the desktop into memory).
add_executable(MultitapHostCaptureDirtyLiveTest EXCLUDE_FROM_ALL src/capture_dirty_live_test.cpp)
target_link_libraries(MultitapHostCaptureDirtyLiveTest PRIVATE host_engine)
target_compile_options(MultitapHostCaptureDirtyLiveTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostInputAttributionBenchmark EXCLUDE_FROM_ALL src/input_attribution_benchmark.cpp)
target_link_libraries(MultitapHostInputAttributionBenchmark PRIVATE host_engine)
target_compile_options(MultitapHostInputAttributionBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostPipelineBenchmark EXCLUDE_FROM_ALL src/pipeline_benchmark.cpp)
target_link_libraries(MultitapHostPipelineBenchmark PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostPipelineBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Worker -> service pipe write time per frame size (pipe_packet.hpp), legacy vs current packet code and buffer.
add_executable(MultitapHostPipeWriteBenchmark EXCLUDE_FROM_ALL src/pipe_write_benchmark.cpp)
target_link_libraries(MultitapHostPipeWriteBenchmark PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostPipeWriteBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# Probe-only build of media.cpp with a switch that forces the previous DIB path.
add_executable(MultitapHostCapturePathProbe EXCLUDE_FROM_ALL src/capture_path_probe.cpp src/media.cpp)
target_include_directories(MultitapHostCapturePathProbe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapHostCapturePathProbe PRIVATE MULTITAP_CAPTURE_PATH_PROBE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(MultitapHostCapturePathProbe PRIVATE mfplat mf mfuuid wmcodecdspuuid ole32 oleaut32 gdi32 user32 uuid d3d11 dxgi windowsapp cfgmgr32 secur32 winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi)
target_compile_options(MultitapHostCapturePathProbe PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostClipboardTest src/clipboard_test.cpp)
add_executable(MultitapHostClipboardPullCancelTest src/clipboard_pull_cancel_test.cpp)
# Received clipboard files on disk: relative-to-handle creation and removal, links refused.
add_executable(MultitapHostClipboardFilesTest EXCLUDE_FROM_ALL src/clipboard_files_test.cpp)
# The service's own list: controls 128/129 for SYSTEM and Administrators only (AccessCheck on descriptors; no service touched).
add_executable(MultitapHostServiceControlSecurityTest EXCLUDE_FROM_ALL src/service_control_security_test.cpp)
target_compile_definitions(MultitapHostServiceControlSecurityTest PRIVATE UNICODE _UNICODE)
target_compile_options(MultitapHostServiceControlSecurityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# The Host installer's own checks (installer/host_installer.cpp compiled in): squatted lock, untrusted cache folder, held package.
add_executable(MultitapHostInstallerTest EXCLUDE_FROM_ALL src/host_installer_test.cpp)
target_compile_definitions(MultitapHostInstallerTest PRIVATE UNICODE _UNICODE "MULTITAP_HOST_VERSION=\"9.9.9\"")
target_link_libraries(MultitapHostInstallerTest PRIVATE shell32 ole32 advapi32 user32 wtsapi32 userenv)
target_compile_options(MultitapHostInstallerTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostResizeTest src/resize_test.cpp ../windows_client/windows/runner/remote_decoder.cpp)
add_executable(MultitapCodecBenchmark EXCLUDE_FROM_ALL src/codec_benchmark.cpp ../windows_client/windows/runner/remote_decoder.cpp)
target_link_libraries(MultitapCodecBenchmark PRIVATE host_engine windowscodecs)
target_compile_options(MultitapCodecBenchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapStreamMetricsTest EXCLUDE_FROM_ALL src/stream_metrics_test.cpp
  ../windows_client/windows/runner/remote_engine.cpp
  ../windows_client/windows/runner/remote_decoder.cpp
  ../windows_client/windows/runner/remote_audio.cpp)
target_include_directories(MultitapStreamMetricsTest PRIVATE
  ../windows_client/windows/runner
  ../windows_client/windows/flutter/ephemeral
  ../windows_client/windows/flutter/ephemeral/cpp_client_wrapper/include)
target_link_libraries(MultitapStreamMetricsTest PRIVATE host_engine pdh)
target_compile_options(MultitapStreamMetricsTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(MultitapHostResizeTest PRIVATE ../windows_client/windows/runner)
# The actual legacy client engine with lossless tiles (needs the client's generated Flutter wrapper headers).
add_executable(MultitapClientLosslessEngineTest EXCLUDE_FROM_ALL src/client_lossless_engine_test.cpp
  ../windows_client/windows/runner/remote_engine.cpp
  ../windows_client/windows/runner/remote_decoder.cpp
  ../windows_client/windows/runner/remote_audio.cpp)
target_include_directories(MultitapClientLosslessEngineTest PRIVATE
  ../windows_client/windows/runner
  ../windows_client/windows/flutter/ephemeral
  ../windows_client/windows/flutter/ephemeral/cpp_client_wrapper/include)
target_link_libraries(MultitapClientLosslessEngineTest PRIVATE host_engine pdh)
target_compile_options(MultitapClientLosslessEngineTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# texture-pacing-v1: the actual legacy client engine's frame queue, remarks,
# backlog/stale trims, deferred/replaced lossless republishes, and the client's
# render-adapter classification (WARP), with a Flutter registrar fixture.
add_executable(MultitapClientTexturePacingTest EXCLUDE_FROM_ALL src/client_texture_pacing_test.cpp
  ../windows_client/windows/runner/remote_engine.cpp
  ../windows_client/windows/runner/remote_decoder.cpp
  ../windows_client/windows/runner/remote_audio.cpp)
target_include_directories(MultitapClientTexturePacingTest PRIVATE
  ../windows_client/windows/runner
  ../windows_client/windows/flutter/ephemeral
  ../windows_client/windows/flutter/ephemeral/cpp_client_wrapper/include)
target_link_libraries(MultitapClientTexturePacingTest PRIVATE host_engine d3d11 dxgi dwmapi pdh)
target_compile_options(MultitapClientTexturePacingTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# input-latency-v1 frame tag -> production encoder -> actual client engine watch.
add_executable(MultitapClientInputFrameTest EXCLUDE_FROM_ALL src/client_input_frame_test.cpp
  ../windows_client/windows/runner/remote_engine.cpp
  ../windows_client/windows/runner/remote_decoder.cpp
  ../windows_client/windows/runner/remote_audio.cpp)
target_include_directories(MultitapClientInputFrameTest PRIVATE
  ../windows_client/windows/runner
  ../windows_client/windows/flutter/ephemeral
  ../windows_client/windows/flutter/ephemeral/cpp_client_wrapper/include)
target_link_libraries(MultitapClientInputFrameTest PRIVATE host_engine pdh)
target_compile_options(MultitapClientInputFrameTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# cursor-v1 client presenter: strict records, shape cache, the WM_SETCURSOR rule
# on a hidden window with faked cursor calls, scaling, warps and restoration.
add_executable(MultitapClientCursorPresenterTest EXCLUDE_FROM_ALL src/client_cursor_presenter_test.cpp
  ../windows_client/windows/runner/cursor_presenter.cpp
  ../windows_client/windows/runner/remote_cursor.cpp)
target_include_directories(MultitapClientCursorPresenterTest PRIVATE ../windows_client/windows/runner)
target_compile_definitions(MultitapClientCursorPresenterTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(MultitapClientCursorPresenterTest PRIVATE user32 gdi32 comctl32)
target_compile_options(MultitapClientCursorPresenterTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
# The setup windows draw with the client's Pretendard (OFL 1.1), embedded with its licence.
set(MULTITAP_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../windows_client/assets/fonts")
configure_file(src/setup_fonts.rc.in setup_fonts.rc @ONLY)
set(MULTITAP_SETUP_FONTS "${CMAKE_CURRENT_BINARY_DIR}/setup_fonts.rc")
add_executable(MultitapClientSetup WIN32 src/setup_main.cpp "${CMAKE_CURRENT_BINARY_DIR}/client_setup_icon.rc" ${MULTITAP_SETUP_FONTS})
add_executable(MultitapClientSetupQA WIN32 src/setup_main.cpp "${CMAKE_CURRENT_BINARY_DIR}/client_setup_icon.rc" ${MULTITAP_SETUP_FONTS})
target_include_directories(MultitapClientSetupQA PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(MultitapClientSetupQA PRIVATE MULTITAP_SETUP_QA UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(MultitapClientSetupQA PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(MultitapClientSetupQA PRIVATE comctl32 shell32 advapi32 ole32 user32 gdi32 gdiplus uxtheme crypt32)
set_property(TARGET MultitapClientSetupQA PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(MultitapClientSetup PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_options(MultitapClientSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
# DLL planting: a setup runs elevated from Downloads. Every import that is
# not a KnownDLL is delay-loaded, so it is resolved only after wWinMain's SetDefaultDllDirectories
# (System32 only), and the loader takes the static imports from System32 only.
target_link_libraries(MultitapClientSetup PRIVATE delayimp)
target_link_options(MultitapClientSetup PRIVATE /DEPENDENTLOADFLAG:0x800
  /DELAYLOAD:comctl32.dll /DELAYLOAD:uxtheme.dll /DELAYLOAD:crypt32.dll /DELAYLOAD:version.dll /DELAYLOAD:bcrypt.dll
  /DELAYLOAD:newdev.dll /DELAYLOAD:msi.dll /DELAYLOAD:wintrust.dll /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll)
target_compile_definitions(MultitapClientSetup PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(MultitapClientSetup PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(MultitapClientSetup PRIVATE comctl32 shell32 advapi32 ole32 user32 gdi32 gdiplus uxtheme crypt32)
set_property(TARGET MultitapClientSetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(MultitapHostTool PRIVATE HOST_CONSOLE_ENTRY)
# main.cpp includes the whole service (adaptive bitrate + remote update made it
# exceed the default COFF section limit, C1128).
target_compile_options(MultitapHost PRIVATE /bigobj)
target_compile_options(MultitapHostTool PRIVATE /bigobj)
foreach(target MultitapHost MultitapHostTool MultitapHostInputTest MultitapHostServiceTest MultitapHostPendingConnectionTest MultitapHostCredentialBindingTest MultitapHostWindowsAccountRecordTest MultitapHostSettingsRpcTest MultitapHostConsoleTransferTest MultitapHostNativeDisplayContractTest MultitapHostNativeDisplayTopologyTest MultitapHostNativeDisplayServiceChangeTest MultitapHostNativeDisplayJournalTest MultitapHostBrokerStatusTest MultitapHostViewportDiagnosticsTest MultitapHostCatalogMaintenanceTest MultitapHostClipboardTest MultitapHostClipboardPullCancelTest MultitapHostClipboardFilesTest MultitapHostResizeTest)
  target_link_libraries(${target} PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
  target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
endforeach()

# drive-v1 PC side: the Windows client runner's drive file core (path rules,
# link refusal, every operation, read-only shares) in a temporary folder.
add_executable(MultitapClientDriveFilesTest EXCLUDE_FROM_ALL ../windows_client/test/native/drive_files_core_test.cpp
  ../windows_client/windows/runner/drive_files_core.cpp)
target_include_directories(MultitapClientDriveFilesTest PRIVATE ../windows_client/windows/runner)
target_compile_definitions(MultitapClientDriveFilesTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapClientDriveFilesTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

# Header-only resize decoder-box candidate/metadata regression; memory only.
# The fixture invokes no service, driver, capture, encoder, file or network API.
add_executable(MultitapHostResizeEncoderLimitsTest EXCLUDE_FROM_ALL src/resize_encoder_limits_test.cpp)
target_link_libraries(MultitapHostResizeEncoderLimitsTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostResizeEncoderLimitsTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

# Autologon hooks: explicit private QA targets, never packaged or ALL targets.
# The link fixture retains the real adapter vtable/imports but main performs no
# Windows account, registry, LSA, service, process or credential operation.
add_executable(MultitapHostAutologonLinkTest EXCLUDE_FROM_ALL ../../tools/windows-autologon-hooks-qa/native_link_test.cpp)
target_link_libraries(MultitapHostAutologonLinkTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi netapi32)
target_compile_options(MultitapHostAutologonLinkTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapHostAutologonHooksMemoryTest EXCLUDE_FROM_ALL ../../tools/windows-autologon-hooks-qa/memory_test.cpp)
target_compile_options(MultitapHostAutologonHooksMemoryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

# Actual production encodeRecord/Decoder on synthetic records only. These
# explicit targets never construct NativeAccess or run the product CLI.
foreach(target MultitapHostAutologonCodecMemoryTest MultitapHostAutologonLegacyCodecOracle)
  add_executable(${target} EXCLUDE_FROM_ALL ../../tools/windows-autologon-hooks-qa/native_codec_test.cpp)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(${target} PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi netapi32)
  target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endforeach()
target_compile_definitions(MultitapHostAutologonLegacyCodecOracle PRIVATE MULTITAP_AUTOLOGON_CODEC_SCHEMA1_ONLY=1)


# host-prerequisites-v1: the native display finish of a Host install (trust,
# device node, provider selection, service restart, rollback) on fakes only.
# A release gate (tools/host-installer/build-native.ps1): a first install that
# leaves the display provider unhandled must not ship.
add_executable(MultitapHostPrerequisitesTest EXCLUDE_FROM_ALL src/host_prerequisites_test.cpp)
target_compile_definitions(MultitapHostPrerequisitesTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapHostPrerequisitesTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostPrerequisitesTest PRIVATE bcrypt ole32 shell32 shlwapi advapi32)

# host-removal-v1: what removing the Host takes away besides its files (device
# nodes, driver package, the certificate entries Multitap added, display
# records), and that an update removes none of it. Fakes only: no device,
# driver store, certificate store, registry value, file or service is touched.
add_executable(MultitapHostRemovalTest EXCLUDE_FROM_ALL src/host_removal_test.cpp)
target_link_libraries(MultitapHostRemovalTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32)
target_compile_options(MultitapHostRemovalTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

# drive.open-v1: fake WinFsp table/pipe, injected shell action; no legacy main.
add_executable(MultitapHostDriveOpenTest EXCLUDE_FROM_ALL src/drive_open_test.cpp)
target_link_libraries(MultitapHostDriveOpenTest PRIVATE host_engine winhttp crypt32 advapi32 shell32 wtsapi32 userenv bcrypt ws2_32 iphlpapi mpr msi version)
target_compile_definitions(MultitapHostDriveOpenTest PRIVATE "MULTITAP_WINFSP_PIN=\"${CMAKE_CURRENT_SOURCE_DIR}/third_party/winfsp/upstream.json\"")
target_compile_options(MultitapHostDriveOpenTest PRIVATE /W4 /permissive- /EHsc /utf-8)

# Korean and English text tables of the installers, tray, service and settings helper (docs/i18n.md).
add_executable(MultitapHostUiTextTest EXCLUDE_FROM_ALL src/ui_text_test.cpp)
target_compile_features(MultitapHostUiTextTest PRIVATE cxx_std_20)
target_compile_definitions(MultitapHostUiTextTest PRIVATE UNICODE _UNICODE)
target_compile_options(MultitapHostUiTextTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostUiTextTest PRIVATE advapi32 wtsapi32)
# Which sentence the removal window shows for a failure code (setup_uninstall_reason.hpp); memory only.
add_executable(MultitapSetupUninstallReasonTest EXCLUDE_FROM_ALL src/setup_uninstall_reason_test.cpp)
target_compile_features(MultitapSetupUninstallReasonTest PRIVATE cxx_std_20)
target_compile_options(MultitapSetupUninstallReasonTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapClientInstallationTest EXCLUDE_FROM_ALL src/setup_client_installation_test.cpp)
target_compile_definitions(MultitapClientInstallationTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(MultitapClientInstallationTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapClientInstallationTest PRIVATE bcrypt ole32 shell32 shlwapi advapi32 version user32)

# Shared production policy/metadata decisions, synthetic data only.
# Explicit qualification targets: never linked into Host or packaged.

# Source-only sign-in qualification. These tests use injected facts/local IPC;
# they do not attach a desktop, launch a SYSTEM worker or advertise readiness.
add_executable(MultitapHostSigninNativeTest EXCLUDE_FROM_ALL
  src/signin_worker_native.cpp src/signin_capture_dxgi.cpp src/signin_native_test.cpp)
target_compile_features(MultitapHostSigninNativeTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninNativeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_compile_definitions(MultitapHostSigninNativeTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(MultitapHostSigninNativeTest PRIVATE advapi32 bcrypt wtsapi32 userenv user32 gdi32 d3d11 dxgi)
add_executable(MultitapHostSigninChannelTest EXCLUDE_FROM_ALL
  src/signin_worker_native.cpp src/signin_worker_channel.cpp src/signin_worker_channel_test.cpp)
target_compile_features(MultitapHostSigninChannelTest PRIVATE cxx_std_20)
target_include_directories(MultitapHostSigninChannelTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(MultitapHostSigninChannelTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_compile_definitions(MultitapHostSigninChannelTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(MultitapHostSigninChannelTest PRIVATE advapi32 bcrypt wtsapi32 userenv user32 gdi32 winhttp crypt32 shell32 normaliz ole32 ws2_32)

add_executable(MultitapHostSigninServiceAuthorityTest EXCLUDE_FROM_ALL
  src/signin_service_authority_test.cpp src/signin_worker_native.cpp src/signin_worker_channel.cpp)
target_compile_features(MultitapHostSigninServiceAuthorityTest PRIVATE cxx_std_20)
target_compile_definitions(MultitapHostSigninServiceAuthorityTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_compile_options(MultitapHostSigninServiceAuthorityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninServiceAuthorityTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32 user32)

# Same orchestration with injected facts only. This definition is target-local:
# the production entry object in host_engine is always compiled in full.
add_executable(MultitapHostSigninEntryTest EXCLUDE_FROM_ALL src/signin_worker_entry.cpp src/signin_worker_entry_test.cpp)
target_compile_definitions(MultitapHostSigninEntryTest PRIVATE MULTITAP_SIGNIN_ENTRY_CORE_ONLY)
target_compile_features(MultitapHostSigninEntryTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninEntryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninEntryTest PRIVATE host_engine)

# Force the real entry/command implementation and all native/Encoder imports
# out of the static library, without invoking any of those implementations.
add_executable(MultitapHostSigninEntryLinkTest EXCLUDE_FROM_ALL src/signin_worker_entry_link_test.cpp)
target_compile_features(MultitapHostSigninEntryLinkTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninEntryLinkTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninEntryLinkTest PRIVATE host_engine)

add_executable(MultitapHostSigninCommandTest EXCLUDE_FROM_ALL src/signin_worker_command_test.cpp src/signin_worker_command.hpp)
target_compile_features(MultitapHostSigninCommandTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninCommandTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

# Autonomous restart state machine uses injected SCM/clock observations only.
# Link the real native endpoint without invoking it in the fixture process.
add_executable(MultitapHostServiceRestartTest EXCLUDE_FROM_ALL src/host_service_restart_test.cpp)
target_compile_features(MultitapHostServiceRestartTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostServiceRestartTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostServiceRestartTest PRIVATE host_engine version)

add_executable(MultitapHostSigninOriginalFileTest EXCLUDE_FROM_ALL src/signin_original_file_test.cpp)
target_compile_features(MultitapHostSigninOriginalFileTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninOriginalFileTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninOriginalFileTest PRIVATE host_engine)

add_executable(MultitapHostSigninPreparationTest EXCLUDE_FROM_ALL
  src/signin_preparation_test.cpp src/signin_preparation.cpp src/signin_worker_native.cpp)
target_compile_features(MultitapHostSigninPreparationTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninPreparationTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninPreparationTest PRIVATE host_engine)

add_executable(MultitapHostSigninPreparationAuthorityTest EXCLUDE_FROM_ALL src/signin_preparation_authority_test.cpp)
target_compile_features(MultitapHostSigninPreparationAuthorityTest PRIVATE cxx_std_20)
target_compile_definitions(MultitapHostSigninPreparationAuthorityTest PRIVATE MULTITAP_NATIVE_DISPLAY_TEST)
target_compile_options(MultitapHostSigninPreparationAuthorityTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninPreparationAuthorityTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32 user32)

add_executable(MultitapHostSigninRestorationTest EXCLUDE_FROM_ALL
  src/signin_restoration_test.cpp src/signin_restoration.cpp src/signin_worker_native.cpp)
target_compile_features(MultitapHostSigninRestorationTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninRestorationTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninRestorationTest PRIVATE host_engine)

# Test TU includes the actual implementation with a private synthetic-peer
# fixture; the production host_engine TU has no test adapter compiled into it.
add_executable(MultitapHostSigninDiagnosticInletTest EXCLUDE_FROM_ALL src/signin_diagnostic_inlet_test.cpp)
target_compile_features(MultitapHostSigninDiagnosticInletTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninDiagnosticInletTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninDiagnosticInletTest PRIVATE host_engine)

add_executable(MultitapHostSigninDiagnosticClientTest EXCLUDE_FROM_ALL src/signin_diagnostic_client_test.cpp)
target_compile_features(MultitapHostSigninDiagnosticClientTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninDiagnosticClientTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninDiagnosticClientTest PRIVATE host_engine)

add_executable(MultitapHostSigninDiagnosticCoordinatorTest EXCLUDE_FROM_ALL src/signin_diagnostic_coordinator_test.cpp)
target_compile_features(MultitapHostSigninDiagnosticCoordinatorTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninDiagnosticCoordinatorTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninDiagnosticCoordinatorTest PRIVATE host_engine)

add_executable(MultitapHostSigninDiagnosticBridgeTest EXCLUDE_FROM_ALL src/signin_diagnostic_bridge_test.cpp)
target_compile_features(MultitapHostSigninDiagnosticBridgeTest PRIVATE cxx_std_20)
target_compile_definitions(MultitapHostSigninDiagnosticBridgeTest PRIVATE MULTITAP_SESSION_STOP_TEST)
target_compile_options(MultitapHostSigninDiagnosticBridgeTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninDiagnosticBridgeTest PRIVATE host_engine host_clipboard winhttp crypt32 advapi32 shell32 imm32 wtsapi32 userenv bcrypt ws2_32 iphlpapi setupapi cfgmgr32 secur32 user32)

add_executable(MultitapHostSigninDiagnosticHandoffTest EXCLUDE_FROM_ALL src/signin_diagnostic_handoff_test.cpp)
target_compile_features(MultitapHostSigninDiagnosticHandoffTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninDiagnosticHandoffTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninDiagnosticHandoffTest PRIVATE host_engine)

# Explicit standalone MSI handoff policy/own-child RM fixtures only.
# EXCLUDE_FROM_ALL: no Host/Tool/Setup runtime enablement or default build.
# Private candidate production-bridge pure tests. No Host/service/capture is run.
add_executable(MultitapHostSigninBridgePolicyTest EXCLUDE_FROM_ALL src/signin_bridge_policy_test.cpp)
target_compile_features(MultitapHostSigninBridgePolicyTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninBridgePolicyTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

add_executable(MultitapHostSigninOwnedReleaseTest EXCLUDE_FROM_ALL src/signin_owned_release_test.cpp)
target_compile_features(MultitapHostSigninOwnedReleaseTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninOwnedReleaseTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
foreach(signin_component BrokerPreparation BrokerUnknown ReleaseLease)
  if(signin_component STREQUAL "BrokerPreparation")
    set(signin_test_source src/signin_broker_preparation_test.cpp)
  elseif(signin_component STREQUAL "BrokerUnknown")
    set(signin_test_source src/signin_broker_unknown_account_test.cpp)
  else()
    set(signin_test_source src/signin_release_lease_test.cpp)
  endif()
  add_executable(MultitapHostSignin${signin_component}Test EXCLUDE_FROM_ALL ${signin_test_source})
  target_compile_features(MultitapHostSignin${signin_component}Test PRIVATE cxx_std_20)
  target_compile_options(MultitapHostSignin${signin_component}Test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
  target_link_libraries(MultitapHostSignin${signin_component}Test PRIVATE host_engine)
endforeach()

add_executable(MultitapHostSigninAtomicEnterTest EXCLUDE_FROM_ALL src/signin_atomic_enter_test.cpp src/signin_worker_entry.cpp)
target_compile_definitions(MultitapHostSigninAtomicEnterTest PRIVATE MULTITAP_SIGNIN_ENTRY_CORE_ONLY)
target_compile_features(MultitapHostSigninAtomicEnterTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninAtomicEnterTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninAtomicEnterTest PRIVATE host_engine host_clipboard)

add_executable(MultitapHostSigninWirePackets EXCLUDE_FROM_ALL src/signin_wire_packet_fixture.cpp src/signin_worker_entry.cpp)
target_compile_definitions(MultitapHostSigninWirePackets PRIVATE MULTITAP_SIGNIN_ENTRY_CORE_ONLY)
target_compile_features(MultitapHostSigninWirePackets PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninWirePackets PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(MultitapHostSigninWirePackets PRIVATE host_engine)

add_executable(MultitapHostSigninEligibilityHistoryTest EXCLUDE_FROM_ALL src/signin_eligibility_history_test.cpp)
target_compile_features(MultitapHostSigninEligibilityHistoryTest PRIVATE cxx_std_20)
target_compile_options(MultitapHostSigninEligibilityHistoryTest PRIVATE /W4 /WX /permissive- /EHsc /utf-8)

target_link_libraries(host_engine PUBLIC wintrust iphlpapi)

if(MULTITAP_SIGNIN_BACKEND)
  if(NOT MULTITAP_NATIVE_DISPLAY_SDK OR MULTITAP_PRIVATE_SIGNIN_DIAGNOSTIC_TRIAL)
    message(FATAL_ERROR "Production signin backend requires the original paired native SDK and a separate non-diagnostic candidate")
  endif()
  target_compile_definitions(host_engine PUBLIC MULTITAP_SIGNIN_BACKEND=1)
endif()
foreach(signin_backend_mode Off On)
  add_executable(MultitapHostSigninBackend${signin_backend_mode}Test EXCLUDE_FROM_ALL src/signin_backend_policy_test.cpp)
  target_compile_features(MultitapHostSigninBackend${signin_backend_mode}Test PRIVATE cxx_std_20)
  target_compile_options(MultitapHostSigninBackend${signin_backend_mode}Test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
  if(signin_backend_mode STREQUAL "On")
    target_compile_definitions(MultitapHostSigninBackend${signin_backend_mode}Test PRIVATE MULTITAP_SIGNIN_BACKEND=1)
  endif()
endforeach()
