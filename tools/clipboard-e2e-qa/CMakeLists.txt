cmake_minimum_required(VERSION 3.20)
project(ClipboardE2EQA LANGUAGES CXX)
# The Flutter C++ client wrapper headers (header-only use: EncodableValue, MethodCall, MethodResult). A worktree without its own
# build passes -DMULTITAP_FLUTTER_EPHEMERAL=<apps/windows_client/windows/flutter/ephemeral of a checkout that was built once>.
set(MULTITAP_FLUTTER_EPHEMERAL "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/windows_client/windows/flutter/ephemeral" CACHE PATH "Flutter ephemeral directory")
add_subdirectory(../../apps/windows_host host EXCLUDE_FROM_ALL)

# The real Host ClipboardWorker on an in-memory clipboard.
add_executable(clipboard_e2e_host host_e2e.cpp)
target_compile_features(clipboard_e2e_host PRIVATE cxx_std_20)
target_compile_options(clipboard_e2e_host PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /wd4100)
target_include_directories(clipboard_e2e_host PRIVATE ../../apps/windows_host/src)
target_link_libraries(clipboard_e2e_host PRIVATE host_engine host_clipboard userenv wtsapi32 winhttp crypt32 bcrypt advapi32 shell32 user32 ole32 oleaut32)

# Per-chunk CPU of the Host's clipboard steps (README.md, "Throughput benchmark").
add_executable(clipboard_stage_bench stage_bench.cpp)
target_compile_features(clipboard_stage_bench PRIVATE cxx_std_20)
target_compile_options(clipboard_stage_bench PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /wd4100)
target_include_directories(clipboard_stage_bench PRIVATE ../../apps/windows_host/src)
target_link_libraries(clipboard_stage_bench PRIVATE host_engine host_clipboard userenv wtsapi32 winhttp crypt32 bcrypt advapi32 shell32 user32 ole32 oleaut32)

add_executable(clipboard_recovery_qa recovery_qa.cpp)
target_compile_features(clipboard_recovery_qa PRIVATE cxx_std_20)
target_compile_options(clipboard_recovery_qa PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /wd4100)
target_include_directories(clipboard_recovery_qa PRIVATE ../../apps/windows_host/src)
target_link_libraries(clipboard_recovery_qa PRIVATE host_engine host_clipboard userenv wtsapi32 winhttp crypt32 bcrypt advapi32 shell32 user32 ole32 oleaut32)

add_executable(clipboard_diagnostic_qa diagnostic_qa.cpp)
target_compile_features(clipboard_diagnostic_qa PRIVATE cxx_std_20)
target_compile_options(clipboard_diagnostic_qa PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /wd4100)
target_include_directories(clipboard_diagnostic_qa PRIVATE ../../apps/windows_host/src)
target_link_libraries(clipboard_diagnostic_qa PRIVATE host_engine host_clipboard userenv wtsapi32 winhttp crypt32 bcrypt advapi32 shell32 user32 ole32 oleaut32)

# The real native client adapter (ClientClipboardChannel) on an in-memory clipboard.
add_executable(clipboard_e2e_client client_e2e.cpp ../../apps/windows_client/windows/runner/client_clipboard.cpp ../../apps/windows_client/windows/runner/pending_clipboard.cpp)
target_compile_features(clipboard_e2e_client PRIVATE cxx_std_20)
target_compile_options(clipboard_e2e_client PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /wd4100)
target_include_directories(clipboard_e2e_client PRIVATE
  ../../apps/windows_host/src
  ../../apps/windows_client/windows/runner
  ${MULTITAP_FLUTTER_EPHEMERAL}
  ${MULTITAP_FLUTTER_EPHEMERAL}/cpp_client_wrapper/include)
target_link_libraries(clipboard_e2e_client PRIVATE host_clipboard userenv wtsapi32 winhttp crypt32 bcrypt advapi32 shell32 user32 ole32 oleaut32)

# The client adapter's own native smoke test, built from the same sources (run with MULTITAP_CLIPBOARD_QA_SUBSTITUTED_ONLY=1 as a
# normal user: it then touches no real clipboard and needs no window station).
add_executable(clipboard_client_smoke ../../apps/windows_client/test/native/client_clipboard_smoke.cpp ../../apps/windows_client/windows/runner/client_clipboard.cpp ../../apps/windows_client/windows/runner/pending_clipboard.cpp)
target_compile_features(clipboard_client_smoke PRIVATE cxx_std_20)
target_compile_options(clipboard_client_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /wd4100)
target_include_directories(clipboard_client_smoke PRIVATE
  ../../apps/windows_client/windows/runner
  ${MULTITAP_FLUTTER_EPHEMERAL}
  ${MULTITAP_FLUTTER_EPHEMERAL}/cpp_client_wrapper/include)
target_link_libraries(clipboard_client_smoke PRIVATE host_clipboard user32 shell32 advapi32 bcrypt ole32 oleaut32 uuid)
