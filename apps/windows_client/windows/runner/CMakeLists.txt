cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# This no-network fixture uses real engines and HWNDs without elevation. It is
# never enabled for lib/main.dart or selected by ordinary product builds.
option(MULTITAP_SESSION_WINDOWS_QA "Build isolated session-window lifecycle fixture" OFF)
foreach(FLAGS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_PROFILE)
  if("${${FLAGS}}" MATCHES "MULTITAP_SESSION_WINDOWS_QA")
    message(FATAL_ERROR "Use the explicit MULTITAP_SESSION_WINDOWS_QA option, not compiler flags")
  endif()
endforeach()
get_directory_property(MULTITAP_FLUTTER_ENV DIRECTORY "${FLUTTER_MANAGED_DIR}" DEFINITION FLUTTER_TOOL_ENVIRONMENT)
set(MULTITAP_FLUTTER_TARGETS "${MULTITAP_FLUTTER_ENV}")
list(FILTER MULTITAP_FLUTTER_TARGETS INCLUDE REGEX "^FLUTTER_TARGET=")
if(MULTITAP_SESSION_WINDOWS_QA)
  if(NOT "${MULTITAP_FLUTTER_TARGETS}" STREQUAL "FLUTTER_TARGET=test_support/session_windows_qa.dart")
    message(FATAL_ERROR "Session-window QA requires generated FLUTTER_TARGET=test_support/session_windows_qa.dart")
  endif()
  set(MULTITAP_RUNNER_MANIFEST "session_windows_qa.exe.manifest")
else()
  # Flutter --config-only also configures its default build directory before
  # the script configures the separate QA directory. Keep that configuration
  # unelevated (asInvoker) and without QA APIs; only the explicit option below changes the fixture.
  set(MULTITAP_RUNNER_MANIFEST "runner.exe.manifest")
endif()

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "console_webview.cpp"
  "remote_decoder.cpp"
  "client_clipboard.cpp"
  "pending_clipboard.cpp"
  "client_print.cpp"
  "transfer_notice.cpp"
  "paste_target.cpp"
  "drive_channel.cpp"
  "drive_files_core.cpp"
  "../../../../native/windows_clipboard/clipboard.cpp"
  "remote_audio.cpp"
  "remote_input.cpp"
  "cursor_presenter.cpp"
  "remote_cursor.cpp"
  "remote_engine.cpp"
  "remote_track_engine.cpp"
  "remote_track_decoder.cpp"
  "remote_track_presenter.cpp"
  "remote_track_window.cpp"
  "udp_media_leg.cpp"
  "thumbnail_writer.cpp"
  "usb_capture.cpp"
  "usb_topology.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "${MULTITAP_RUNNER_MANIFEST}"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})
if(MULTITAP_SESSION_WINDOWS_QA)
  target_compile_definitions(${BINARY_NAME} PRIVATE MULTITAP_SESSION_WINDOWS_QA=1 MULTITAP_SESSION_WINDOWS_QA_VALIDATED=1)
  target_link_options(${BINARY_NAME} PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
  set_target_properties(${BINARY_NAME} PROPERTIES OUTPUT_NAME "MultitapSessionWindowsQA")
else()
  target_link_options(${BINARY_NAME} PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
endif()
target_compile_options(${BINARY_NAME} PRIVATE /utf-8)
# The Host settings UI uses this same runner target. The generated
# resource-build range says which of the two this build is; it must agree with
# the variant the build was asked for.
if(NOT DEFINED FLUTTER_VERSION_BUILD OR NOT "${FLUTTER_VERSION_BUILD}" MATCHES "^[0-9]+$")
  message(FATAL_ERROR "Generated Flutter resource-build identity required")
endif()
if(FLUTTER_VERSION_BUILD LESS 1 OR FLUTTER_VERSION_BUILD GREATER 65535)
  message(FATAL_ERROR "Invalid generated Windows resource-build identity")
endif()
if(FLUTTER_VERSION_BUILD LESS 20000)
  set(MULTITAP_RESOURCE_VARIANT "Client")
else()
  set(MULTITAP_RESOURCE_VARIANT "HostSettings")
endif()
if(NOT "$ENV{MULTITAP_WINDOWS_VARIANT}" STREQUAL "")
  if(NOT "$ENV{MULTITAP_WINDOWS_VARIANT}" STREQUAL MULTITAP_RESOURCE_VARIANT)
    message(FATAL_ERROR "Portable variant differs from generated resource identity")
  endif()
elseif(DEFINED MULTITAP_WINDOWS_VARIANT AND NOT "${MULTITAP_WINDOWS_VARIANT}" STREQUAL "")
  if(NOT MULTITAP_WINDOWS_VARIANT STREQUAL MULTITAP_RESOURCE_VARIANT)
    message(FATAL_ERROR "CMake variant differs from generated resource identity")
  endif()
endif()

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app flutter_wrapper_plugin)
include("webview2.cmake")
add_executable(multitap_console_webview_policy_test EXCLUDE_FROM_ALL "tests/console_webview_policy_test.cpp" "console_webview.cpp")
target_include_directories(multitap_console_webview_policy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${multitap_webview2_SOURCE_DIR}/build/native/include")
target_compile_features(multitap_console_webview_policy_test PRIVATE cxx_std_17)
target_compile_definitions(multitap_console_webview_policy_test PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(multitap_console_webview_policy_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_console_webview_policy_test PRIVATE "${multitap_webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib" "dwmapi.lib" "shlwapi.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "gdi32.lib" "version.lib" "bcrypt.lib")
add_executable(multitap_console_webview_probe EXCLUDE_FROM_ALL "tests/console_webview_probe.cpp" "console_webview.cpp")
target_include_directories(multitap_console_webview_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${multitap_webview2_SOURCE_DIR}/build/native/include")
target_compile_features(multitap_console_webview_probe PRIVATE cxx_std_17)
target_compile_definitions(multitap_console_webview_probe PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(multitap_console_webview_probe PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_console_webview_probe PRIVATE "${multitap_webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib" "dwmapi.lib" "shlwapi.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "gdi32.lib" "version.lib" "bcrypt.lib")
# Private QA binaries must never enter runner/Release, which is packaged.
set_target_properties(multitap_console_webview_policy_test multitap_console_webview_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qa/console-webview"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/qa/console-webview/debug"
  RUNTIME_OUTPUT_DIRECTORY_PROFILE "${CMAKE_BINARY_DIR}/qa/console-webview/profile"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa/console-webview"
)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib" "advapi32.lib" "imm32.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "shell32.lib" "bcrypt.lib" "setupapi.lib" "wintrust.lib" "crypt32.lib" "cfgmgr32.lib" "cabinet.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "d3d11.lib" "dxgi.lib")
target_include_directories(${BINARY_NAME} PRIVATE "../../../../native/windows_clipboard")
target_link_libraries(${BINARY_NAME} PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "windowscodecs.lib" "pdh.lib" "gdiplus.lib" "shcore.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# usb-v1 capture on a substituted UsbDk: listing, capture, request translation,
# cancellation, release and unplugging. No driver, device or installation.
add_executable(multitap_usb_capture_smoke EXCLUDE_FROM_ALL "../../test/native/usb_capture_core_test.cpp")
target_include_directories(multitap_usb_capture_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_usb_capture_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_usb_capture_smoke PRIVATE NOMINMAX)
target_compile_options(multitap_usb_capture_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_usb_capture_smoke PRIVATE "user32.lib" "advapi32.lib" "shell32.lib" "ole32.lib" "wintrust.lib" "crypt32.lib")
set_target_properties(multitap_usb_capture_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# USB picker naming/classification, the vendor table and the hand-over check over
# fake device trees (usb_describe_core.h). No device or disk is touched.
add_executable(multitap_usb_describe_smoke EXCLUDE_FROM_ALL "../../test/native/usb_describe_core_test.cpp")
target_include_directories(multitap_usb_describe_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_usb_describe_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_usb_describe_smoke PRIVATE NOMINMAX)
target_compile_options(multitap_usb_describe_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_usb_describe_smoke PRIVATE "user32.lib" "cabinet.lib")
set_target_properties(multitap_usb_describe_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_instance_smoke EXCLUDE_FROM_ALL "../../test/native/single_instance_smoke.cpp")
target_include_directories(multitap_instance_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_instance_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_instance_smoke PRIVATE NOMINMAX)
target_compile_options(multitap_instance_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_instance_smoke PRIVATE "user32.lib" "advapi32.lib" "shell32.lib" "ole32.lib")
set_target_properties(multitap_instance_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_geometry_smoke EXCLUDE_FROM_ALL "../../test/native/window_geometry_smoke.cpp" "win32_window.cpp")
target_include_directories(multitap_geometry_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_geometry_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_geometry_smoke PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(multitap_geometry_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_geometry_smoke PRIVATE flutter "dwmapi.lib" "user32.lib")
set_target_properties(multitap_geometry_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
add_custom_command(TARGET multitap_geometry_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FLUTTER_LIBRARY}" "$<TARGET_FILE_DIR:multitap_geometry_smoke>/flutter_windows.dll")

# The mouse lock's picture rectangle (pointer_confinement.h): pure arithmetic;
# no window, clip or input.
add_executable(multitap_pointer_smoke EXCLUDE_FROM_ALL "../../test/native/pointer_confinement_smoke.cpp")
target_include_directories(multitap_pointer_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_pointer_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_pointer_smoke PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(multitap_pointer_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_pointer_smoke PRIVATE "user32.lib")
set_target_properties(multitap_pointer_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# relative-mouse-v1 delta summing, and capture on a hidden HWND with recorded
# fakes; never clips the real cursor, reads raw input or shows a window.
add_executable(multitap_relative_mouse_smoke EXCLUDE_FROM_ALL "../../test/native/relative_mouse_smoke.cpp")
target_include_directories(multitap_relative_mouse_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_relative_mouse_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_relative_mouse_smoke PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(multitap_relative_mouse_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_relative_mouse_smoke PRIVATE "user32.lib")
set_target_properties(multitap_relative_mouse_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# 게임 마우스 over USB: relay frames, Raw Input as reports, the device's answers,
# pacing, the relative mouse's routing on a hidden HWND with fakes, and a model
# of the hold time per path. No device, VM, clip or physical input.
add_executable(multitap_game_mouse_usb_test EXCLUDE_FROM_ALL "../../test/native/game_mouse_usb_test.cpp")
target_include_directories(multitap_game_mouse_usb_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_game_mouse_usb_test PRIVATE cxx_std_17)
target_compile_definitions(multitap_game_mouse_usb_test PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(multitap_game_mouse_usb_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_game_mouse_usb_test PRIVATE "user32.lib")
set_target_properties(multitap_game_mouse_usb_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_ime_smoke EXCLUDE_FROM_ALL "../../test/native/password_ime_smoke.cpp")
target_include_directories(multitap_ime_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(multitap_ime_smoke PRIVATE "imm32.lib" "user32.lib")
set_target_properties(multitap_ime_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_decoder_smoke EXCLUDE_FROM_ALL "../../test/native/remote_decode_smoke.cpp" "remote_decoder.cpp")
target_include_directories(multitap_decoder_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_decoder_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_decoder_smoke PRIVATE cxx_std_17)
target_link_libraries(multitap_decoder_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib")
set_target_properties(multitap_decoder_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# GPU presentation plumbing on WARP: the D3D11 NV12 -> BGRA converter against the
# CPU reference (pixel-exact), pooled shared textures opened by a second device,
# scaling, the lossless overlay copy and the adapter selection fallbacks.
add_executable(multitap_gpu_video_smoke EXCLUDE_FROM_ALL "../../test/native/gpu_video_smoke.cpp")
target_include_directories(multitap_gpu_video_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_gpu_video_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_gpu_video_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_gpu_video_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_gpu_video_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "ole32.lib" "user32.lib")
set_target_properties(multitap_gpu_video_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# Synthetic frames through the real scaler, WIC JPEG encoder and atomic file
# replacement in a temporary folder; it measures cost, not VM capture.
add_executable(multitap_thumbnail_smoke EXCLUDE_FROM_ALL "../../test/native/thumbnail_writer_smoke.cpp" "thumbnail_writer.cpp")
target_include_directories(multitap_thumbnail_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_thumbnail_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_thumbnail_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_thumbnail_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_thumbnail_smoke PRIVATE "windowscodecs.lib" "ole32.lib" "oleaut32.lib" "shell32.lib" "user32.lib" "advapi32.lib")
set_target_properties(multitap_thumbnail_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# Real engines and CPU decoder on the recorded fixtures (argument: fixture
# folder), checking that each stream end hands its last frame to the writer.
add_executable(multitap_thumbnail_engine_smoke EXCLUDE_FROM_ALL "../../test/native/thumbnail_engine_smoke.cpp"
  "thumbnail_writer.cpp" "remote_engine.cpp" "remote_track_engine.cpp" "remote_track_decoder.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_thumbnail_engine_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_thumbnail_engine_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_thumbnail_engine_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_thumbnail_engine_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_thumbnail_engine_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "windowscodecs.lib" "ole32.lib" "oleaut32.lib" "shell32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_thumbnail_engine_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# A viewport resize's held picture (RemoteMediaSession::Start hold) on the real
# engines and CPU decoder with the recorded fixtures (argument: fixture folder).
add_executable(multitap_resize_hold_smoke EXCLUDE_FROM_ALL "../../test/native/resize_hold_smoke.cpp"
  "remote_engine.cpp" "remote_track_engine.cpp" "remote_track_decoder.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_resize_hold_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_resize_hold_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_resize_hold_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_resize_hold_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_resize_hold_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_resize_hold_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# decoder-prestart-v1: prestarted CPU decoders give a new decoder's pictures
# (decoder, legacy/held/track sessions, WARP GPU textures); `--benchmark`
# times stream info -> first picture with and without one (Idle priority).
add_executable(multitap_decoder_prestart_smoke EXCLUDE_FROM_ALL "../../test/native/decoder_prestart_smoke.cpp"
  "remote_engine.cpp" "remote_track_engine.cpp" "remote_track_decoder.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_decoder_prestart_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_decoder_prestart_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_decoder_prestart_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_decoder_prestart_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_decoder_prestart_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_decoder_prestart_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# Long-session bounds (backlog #60): handles, threads, GDI/USER objects and private
# bytes across many reconnects, held-picture resizes (CPU and WARP), two-output
# starts and unused prestarts on the real engines; `cycles=N` per phase.
add_executable(multitap_media_cycles_smoke EXCLUDE_FROM_ALL "../../test/native/media_cycles_smoke.cpp"
  "remote_engine.cpp" "remote_track_engine.cpp" "remote_track_decoder.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_media_cycles_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_media_cycles_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_media_cycles_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_media_cycles_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_media_cycles_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_media_cycles_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# Several open remote windows (backlog #62): what a hidden window still does and
# how it comes back (default), and `bench`: CPU, private bytes and threads for
# 1-3 windows at 1080p60 on the real engines and CPU decoder, Flutter modelled.
add_executable(multitap_multi_session_smoke EXCLUDE_FROM_ALL "../../test/native/multi_session_smoke.cpp"
  "remote_engine.cpp" "remote_track_engine.cpp" "remote_track_decoder.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_multi_session_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_multi_session_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_multi_session_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_multi_session_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_multi_session_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_multi_session_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# When a session window shows none of its picture (session_visibility.h): the
# rule on rectangles and the z-order walk on two tool windows far off-screen.
add_executable(multitap_session_visibility_test EXCLUDE_FROM_ALL "../../test/native/session_visibility_test.cpp")
target_include_directories(multitap_session_visibility_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_session_visibility_test PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_features(multitap_session_visibility_test PRIVATE cxx_std_17)
target_compile_options(multitap_session_visibility_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_session_visibility_test PRIVATE "dwmapi.lib" "user32.lib")
set_target_properties(multitap_session_visibility_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# The audio jitter buffer against simulated arrival patterns (no device, no network).
add_executable(multitap_audio_jitter_test EXCLUDE_FROM_ALL "../../test/native/audio_jitter_test.cpp")
target_include_directories(multitap_audio_jitter_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_audio_jitter_test PRIVATE cxx_std_17)
target_compile_options(multitap_audio_jitter_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set_target_properties(multitap_audio_jitter_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# Bounded audio buffer and media push outcomes (full / stopped / invalid) without
# a VM, relay or audio device.
add_executable(multitap_media_backpressure_smoke EXCLUDE_FROM_ALL "../../test/native/media_backpressure_smoke.cpp"
  "remote_engine.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_media_backpressure_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_media_backpressure_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_media_backpressure_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_media_backpressure_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_media_backpressure_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_media_backpressure_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# udp-media-v1: the probe schedule and the path rule with injected time, then the leg
# over real loopback sockets against a fake relay and Host. No network, no decoder.
add_executable(multitap_udp_media_leg_test EXCLUDE_FROM_ALL "../../test/native/udp_media_leg_test.cpp" "udp_media_leg.cpp")
target_include_directories(multitap_udp_media_leg_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_udp_media_leg_test PRIVATE NOMINMAX)
target_compile_features(multitap_udp_media_leg_test PRIVATE cxx_std_17)
target_compile_options(multitap_udp_media_leg_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_udp_media_leg_test PRIVATE "ws2_32.lib" "bcrypt.lib")
set_target_properties(multitap_udp_media_leg_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# The real dispatcher and engines against the gate: an engine that stops is never
# reached by the receive thread again.
add_executable(multitap_udp_session_smoke EXCLUDE_FROM_ALL "../../test/native/udp_session_smoke.cpp"
  "remote_engine.cpp" "remote_track_engine.cpp" "remote_track_decoder.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_udp_session_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_udp_session_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_udp_session_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_udp_session_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_udp_session_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_udp_session_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# tracks-v1 decode lanes on real Microsoft-encoded H.264 (CPU decoder) and two
# converters of one WARP device at once; `--benchmark` prints the 1080p rate.
add_executable(multitap_track_lanes_smoke EXCLUDE_FROM_ALL "../../test/native/track_lanes_smoke.cpp"
  "remote_engine.cpp" "remote_track_engine.cpp" "remote_track_decoder.cpp" "remote_decoder.cpp" "remote_audio.cpp")
target_include_directories(multitap_track_lanes_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_track_lanes_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_track_lanes_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_track_lanes_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_track_lanes_smoke PRIVATE "mfplat.lib" "mf.lib" "mfuuid.lib" "wmcodecdspuuid.lib" "ole32.lib" "oleaut32.lib" "user32.lib" "pdh.lib")
set_target_properties(multitap_track_lanes_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# audio-opus-tracks-v1: the independent-output start arguments and the shared
# TVT1 audio wire/gate (no audio device).
add_executable(multitap_track_configuration_test EXCLUDE_FROM_ALL "../../test/native/track_configuration_test.cpp")
target_include_directories(multitap_track_configuration_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_track_configuration_test PRIVATE NOMINMAX)
target_compile_features(multitap_track_configuration_test PRIVATE cxx_std_17)
target_compile_options(multitap_track_configuration_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set_target_properties(multitap_track_configuration_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_client_clipboard_smoke EXCLUDE_FROM_ALL "../../test/native/client_clipboard_smoke.cpp" "client_clipboard.cpp" "pending_clipboard.cpp" "../../../../native/windows_clipboard/clipboard.cpp")
target_include_directories(multitap_client_clipboard_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "../../../../native/windows_clipboard" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_features(multitap_client_clipboard_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_client_clipboard_smoke PRIVATE /utf-8 /W4 /WX /wd4100)
target_link_libraries(multitap_client_clipboard_smoke PRIVATE "user32.lib" "shell32.lib" "advapi32.lib" "bcrypt.lib" "ole32.lib" "oleaut32.lib" "uuid.lib")
set_target_properties(multitap_client_clipboard_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# print-v1 local printing: a generated PDF through Windows.Data.Pdf and the
# Direct2D print control to "Microsoft Print to PDF", written to a file.
add_executable(multitap_client_print_smoke EXCLUDE_FROM_ALL "../../test/native/client_print_smoke.cpp" "client_print.cpp")
target_include_directories(multitap_client_print_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_client_print_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_client_print_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_client_print_smoke PRIVATE /W4 /WX /EHsc /utf-8)
target_link_libraries(multitap_client_print_smoke PRIVATE "ole32.lib" "oleaut32.lib" "shell32.lib" "user32.lib")
set_target_properties(multitap_client_print_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# Mark-of-the-Web (zone_identifier.h) for shared-folder writes and PDF로 저장,
# in a private temporary folder only.
add_executable(multitap_zone_identifier_test EXCLUDE_FROM_ALL "../../test/native/zone_identifier_test.cpp")
target_include_directories(multitap_zone_identifier_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_zone_identifier_test PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_features(multitap_zone_identifier_test PRIVATE cxx_std_17)
target_compile_options(multitap_zone_identifier_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set_target_properties(multitap_zone_identifier_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# Where a paste on this PC goes: only a File Explorer window or the desktop, only the tab on screen, only names that
# stay inside the folder (paste_target_policy.h). No window, COM or file system call.
add_executable(multitap_paste_target_test EXCLUDE_FROM_ALL "../../test/native/paste_target_test.cpp")
target_include_directories(multitap_paste_target_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_paste_target_test PRIVATE cxx_std_17)
target_compile_options(multitap_paste_target_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set_target_properties(multitap_paste_target_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# The transfer notice card and taskbar progress: the real window on a private (invisible) desktop with a
# recording taskbar; never touches the user's desktop, focus or taskbar. `--preview <folder>` draws every state
# without any window and writes PNGs there.
add_executable(multitap_transfer_notice_smoke EXCLUDE_FROM_ALL "../../test/native/transfer_notice_smoke.cpp" "transfer_notice.cpp")
target_include_directories(multitap_transfer_notice_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_transfer_notice_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_transfer_notice_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_transfer_notice_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_transfer_notice_smoke PRIVATE "gdiplus.lib" "shcore.lib" "ole32.lib" "user32.lib" "gdi32.lib" "uuid.lib")
set_target_properties(multitap_transfer_notice_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_window_close_guard_smoke EXCLUDE_FROM_ALL "../../test/native/window_close_guard_smoke.cpp")
target_include_directories(multitap_window_close_guard_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_window_close_guard_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_window_close_guard_smoke PRIVATE /W4 /WX /utf-8)
set_target_properties(multitap_window_close_guard_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# What the main window does for X, a quit request and logoff (main_window_close.h).
add_executable(multitap_main_window_close_smoke EXCLUDE_FROM_ALL "../../test/native/main_window_close_smoke.cpp")
target_include_directories(multitap_main_window_close_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_main_window_close_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_main_window_close_smoke PRIVATE NOMINMAX)
target_compile_options(multitap_main_window_close_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_main_window_close_smoke PRIVATE "user32.lib")
set_target_properties(multitap_main_window_close_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")
# The notification-area icon against a fake shell, on a private desktop (client_tray.h).
add_executable(multitap_tray_smoke EXCLUDE_FROM_ALL "../../test/native/tray_smoke.cpp")
target_include_directories(multitap_tray_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_tray_smoke PRIVATE cxx_std_17)
target_compile_definitions(multitap_tray_smoke PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(multitap_tray_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_link_libraries(multitap_tray_smoke PRIVATE "user32.lib" "shell32.lib" "gdi32.lib")
set_target_properties(multitap_tray_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_session_power_smoke EXCLUDE_FROM_ALL "../../test/native/session_power_smoke.cpp")
target_include_directories(multitap_session_power_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_session_power_smoke PRIVATE cxx_std_17)
set_target_properties(multitap_session_power_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

add_executable(multitap_input_smoke EXCLUDE_FROM_ALL "../../test/native/remote_input_smoke.cpp" "remote_input.cpp")
target_include_directories(multitap_input_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_input_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_input_smoke PRIVATE cxx_std_17)
target_link_libraries(multitap_input_smoke PRIVATE "user32.lib")
set_target_properties(multitap_input_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# Substituted keyboard dispatch only: no hook, HWND, focus or SendInput calls.
add_executable(multitap_remote_keyboard_test EXCLUDE_FROM_ALL "../../test/native/remote_keyboard_test.cpp" "remote_input.cpp")
target_include_directories(multitap_remote_keyboard_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FLUTTER_MANAGED_DIR}/ephemeral" "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper/include")
target_compile_definitions(multitap_remote_keyboard_test PRIVATE NOMINMAX)
target_compile_features(multitap_remote_keyboard_test PRIVATE cxx_std_17)
target_compile_options(multitap_remote_keyboard_test PRIVATE /W4 /WX /permissive- /EHsc)
target_link_libraries(multitap_remote_keyboard_test PRIVATE "user32.lib")
set_target_properties(multitap_remote_keyboard_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

target_link_libraries(${BINARY_NAME} PRIVATE "uuid.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "gdi32.lib" "comctl32.lib" "d2d1.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "ws2_32.lib")
add_executable(multitap_window_smoke EXCLUDE_FROM_ALL "../../test/native/window_presentation_smoke.cpp")
target_include_directories(multitap_window_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_window_smoke PRIVATE cxx_std_17)
target_link_libraries(multitap_window_smoke PRIVATE "user32.lib")
set_target_properties(multitap_window_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# Same runner and Flutter AOT assets, separate short-lived noninteractive visual
# QA executable. This target is excluded from builds and product installation.
get_target_property(MULTITAP_LAYOUT_QA_SOURCES ${BINARY_NAME} SOURCES)
add_executable(multitap_layout_qa WIN32 EXCLUDE_FROM_ALL ${MULTITAP_LAYOUT_QA_SOURCES})
foreach(PROPERTY INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS LINK_OPTIONS LINK_LIBRARIES)
  get_target_property(VALUE ${BINARY_NAME} ${PROPERTY})
  if(VALUE)
    set_property(TARGET multitap_layout_qa PROPERTY ${PROPERTY} "${VALUE}")
  endif()
endforeach()
target_compile_definitions(multitap_layout_qa PRIVATE MULTITAP_WINDOW_LAYOUT_QA=1)
add_dependencies(multitap_layout_qa flutter_assemble)
set_target_properties(multitap_layout_qa PROPERTIES OUTPUT_NAME "MultitapWindowLayoutQA" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/layout-qa")

add_executable(multitap_process_load_smoke EXCLUDE_FROM_ALL "../../test/native/process_load_smoke.cpp")
target_include_directories(multitap_process_load_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(multitap_process_load_smoke PRIVATE NOMINMAX)
target_compile_features(multitap_process_load_smoke PRIVATE cxx_std_17)
target_compile_options(multitap_process_load_smoke PRIVATE /W4 /WX /utf-8)
target_link_libraries(multitap_process_load_smoke PRIVATE "pdh.lib")
set_target_properties(multitap_process_load_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# audio-opus-v1 (opus_audio_decoder.h): decode against the real libopus with
# synthetic audio: round trip, loss concealment into the jitter buffer, late
# and restarted sequences, malformed packets and decode cost. No device, no network.
add_executable(multitap_opus_audio_test EXCLUDE_FROM_ALL "../../test/native/opus_audio_test.cpp")
target_include_directories(multitap_opus_audio_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(multitap_opus_audio_test PRIVATE cxx_std_17)
target_compile_definitions(multitap_opus_audio_test PRIVATE NOMINMAX)
target_compile_options(multitap_opus_audio_test PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
set_target_properties(multitap_opus_audio_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/qa")

# libopus (BSD 3-clause; native/third_party/opus) decodes audio-opus-v1 in every
# target that builds remote_audio.cpp, and in the codec test. Its licence goes
# with the application, like WebView2's.
include("../../../../native/third_party/opus/opus.cmake")
get_property(MULTITAP_RUNNER_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(TARGET_NAME IN LISTS MULTITAP_RUNNER_TARGETS)
  get_target_property(TARGET_SOURCES ${TARGET_NAME} SOURCES)
  if(TARGET_SOURCES MATCHES "remote_audio\\.cpp" OR TARGET_NAME STREQUAL "multitap_opus_audio_test")
    target_link_libraries(${TARGET_NAME} PRIVATE opus)
  endif()
endforeach()
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/third_party/opus/COPYING" "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/libopus-COPYING.txt"
)
