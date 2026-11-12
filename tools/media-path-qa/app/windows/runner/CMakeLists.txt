cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)
# The client engine sources under test (apps/windows_client/windows/runner of
# the working tree); build.ps1 sets MQ_ENGINE_DIR.
if(NOT DEFINED ENV{MQ_ENGINE_DIR})
  message(FATAL_ERROR "Set MQ_ENGINE_DIR to a windows_client runner directory (tools/media-path-qa/build.ps1 does).")
endif()
set(MQ_ENGINE_DIR "$ENV{MQ_ENGINE_DIR}" CACHE STRING "" FORCE)
message(STATUS "Media QA engine: ${MQ_ENGINE_DIR}")

add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "media_qa.cpp"
  "${MQ_ENGINE_DIR}/remote_engine.cpp"
  "${MQ_ENGINE_DIR}/remote_decoder.cpp"
  "${MQ_ENGINE_DIR}/remote_audio.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)
apply_standard_settings(${BINARY_NAME})
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app flutter_wrapper_plugin)
target_link_libraries(${BINARY_NAME} PRIVATE dwmapi.lib mfplat.lib mf.lib mfuuid.lib wmcodecdspuuid.lib ole32.lib oleaut32.lib winmm.lib d3d11.lib dxgi.lib)
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}" "${MQ_ENGINE_DIR}")
target_compile_options(${BINARY_NAME} PRIVATE /utf-8)
add_dependencies(${BINARY_NAME} flutter_assemble)
