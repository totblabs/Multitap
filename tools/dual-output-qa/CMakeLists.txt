cmake_minimum_required(VERSION 3.20)
project(MultitapDualOutputQa LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(../../apps/windows_host host EXCLUDE_FROM_ALL)
add_executable(MultitapDualOutputQa benchmark.cpp)
target_include_directories(MultitapDualOutputQa PRIVATE ../../apps/windows_host/src)
target_link_libraries(MultitapDualOutputQa PRIVATE host_engine)
target_compile_options(MultitapDualOutputQa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapIdleWatchdogQa idle_watchdog.cpp
  ../../apps/windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapIdleWatchdogQa PRIVATE ../../apps/windows_host/src
  ../../apps/windows_client/windows/runner)
target_link_libraries(MultitapIdleWatchdogQa PRIVATE host_engine)
target_compile_options(MultitapIdleWatchdogQa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapTrackMediaQa track_media.cpp
  ../../apps/windows_client/windows/runner/remote_track_decoder.cpp
  ../../apps/windows_client/windows/runner/remote_decoder.cpp)
target_include_directories(MultitapTrackMediaQa PRIVATE ../../apps/windows_host/src
  ../../apps/windows_client/windows/runner)
target_link_libraries(MultitapTrackMediaQa PRIVATE host_engine)
set_target_properties(MultitapTrackMediaQa PROPERTIES CXX_STANDARD 17)
target_compile_options(MultitapTrackMediaQa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapClientTrackEngineQa client_track_engine.cpp
  ../../apps/windows_client/windows/runner/remote_track_window.cpp
  ../../apps/windows_client/windows/runner/remote_track_presenter.cpp
  ../../apps/windows_client/windows/runner/remote_engine.cpp
  ../../apps/windows_client/windows/runner/remote_track_engine.cpp
  ../../apps/windows_client/windows/runner/remote_track_decoder.cpp
  ../../apps/windows_client/windows/runner/remote_decoder.cpp
  ../../apps/windows_client/windows/runner/remote_audio.cpp)
target_include_directories(MultitapClientTrackEngineQa PRIVATE ../../apps/windows_client/windows/runner
  ../../apps/windows_client/windows/flutter/ephemeral
  ../../apps/windows_client/windows/flutter/ephemeral/cpp_client_wrapper/include)
target_link_libraries(MultitapClientTrackEngineQa PRIVATE host_engine)
target_link_libraries(MultitapClientTrackEngineQa PRIVATE d3d11 dxgi user32 gdi32 pdh)
set_target_properties(MultitapClientTrackEngineQa PROPERTIES CXX_STANDARD 17)
target_compile_definitions(MultitapClientTrackEngineQa PRIVATE NOMINMAX UNICODE _UNICODE)
target_compile_options(MultitapClientTrackEngineQa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapClientTrackPresenterQa client_track_presenter.cpp
  ../../apps/windows_client/windows/runner/remote_track_presenter.cpp)
target_include_directories(MultitapClientTrackPresenterQa PRIVATE ../../apps/windows_client/windows/runner)
target_link_libraries(MultitapClientTrackPresenterQa PRIVATE d3d11 dxgi user32)
set_target_properties(MultitapClientTrackPresenterQa PROPERTIES CXX_STANDARD 17)
target_compile_definitions(MultitapClientTrackPresenterQa PRIVATE NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(MultitapClientTrackPresenterQa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
add_executable(MultitapClientTrackBridgeQa EXCLUDE_FROM_ALL client_track_bridge.cpp
  ../../apps/windows_client/windows/runner/remote_engine.cpp
  ../../apps/windows_client/windows/runner/remote_track_engine.cpp
  ../../apps/windows_client/windows/runner/remote_track_decoder.cpp
  ../../apps/windows_client/windows/runner/remote_decoder.cpp
  ../../apps/windows_client/windows/runner/remote_audio.cpp)
target_include_directories(MultitapClientTrackBridgeQa PRIVATE ../../apps/windows_host/src
  ../../apps/windows_client/windows/runner ../../apps/windows_client/windows/flutter/ephemeral
  ../../apps/windows_client/windows/flutter/ephemeral/cpp_client_wrapper/include)
target_link_libraries(MultitapClientTrackBridgeQa PRIVATE host_engine pdh)
set_target_properties(MultitapClientTrackBridgeQa PROPERTIES CXX_STANDARD 17)
target_compile_options(MultitapClientTrackBridgeQa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
