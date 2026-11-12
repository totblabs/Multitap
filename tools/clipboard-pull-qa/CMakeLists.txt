cmake_minimum_required(VERSION 3.20)
project(ClipboardPullQA LANGUAGES CXX)
add_executable(clipboard_pull_qa clipboard_pull_qa.cpp ../../native/windows_clipboard/clipboard.cpp)
target_compile_features(clipboard_pull_qa PRIVATE cxx_std_20)
target_compile_definitions(clipboard_pull_qa PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(clipboard_pull_qa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(clipboard_pull_qa PRIVATE ../../native/windows_clipboard)
target_link_libraries(clipboard_pull_qa PRIVATE user32 shell32 advapi32 bcrypt ole32 windowscodecs shlwapi)

# Narrow, non-interactive regression; the broader OLE/shell scenarios above are
# not needed to verify selection metadata admission.
add_executable(clipboard_offer_admission_qa offer_admission_qa.cpp ../../native/windows_clipboard/clipboard.cpp)
target_compile_features(clipboard_offer_admission_qa PRIVATE cxx_std_20)
target_compile_definitions(clipboard_offer_admission_qa PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(clipboard_offer_admission_qa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(clipboard_offer_admission_qa PRIVATE ../../native/windows_clipboard)
target_link_libraries(clipboard_offer_admission_qa PRIVATE user32 shell32 advapi32 bcrypt ole32 windowscodecs shlwapi)

# Narrow, non-interactive regression of the next-file prefetch (no clipboard, no shell).
add_executable(clipboard_prefetch_qa prefetch_qa.cpp ../../native/windows_clipboard/clipboard.cpp)
target_compile_features(clipboard_prefetch_qa PRIVATE cxx_std_20)
target_compile_definitions(clipboard_prefetch_qa PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(clipboard_prefetch_qa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(clipboard_prefetch_qa PRIVATE ../../native/windows_clipboard)
target_link_libraries(clipboard_prefetch_qa PRIVATE user32 shell32 advapi32 bcrypt ole32 windowscodecs shlwapi)

add_executable(clipboard_settled_selection_qa settled_selection_qa.cpp)
target_compile_features(clipboard_settled_selection_qa PRIVATE cxx_std_20)
target_compile_options(clipboard_settled_selection_qa PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(clipboard_settled_selection_qa PRIVATE ../../native/windows_clipboard)
