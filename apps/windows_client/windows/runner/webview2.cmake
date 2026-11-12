# Official Microsoft SDK, pinned bytes; no Visual Studio-private DLL copying.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(multitap_webview2
  URL "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg"
  URL_HASH SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
)
FetchContent_MakeAvailable(multitap_webview2)
target_include_directories(${BINARY_NAME} PRIVATE "${multitap_webview2_SOURCE_DIR}/build/native/include")
target_link_libraries(${BINARY_NAME} PRIVATE "${multitap_webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib" "shlwapi.lib" "version.lib" "bcrypt.lib")
add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${multitap_webview2_SOURCE_DIR}/LICENSE.txt" "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/WebView2-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${multitap_webview2_SOURCE_DIR}/NOTICE.txt" "$<TARGET_FILE_DIR:${BINARY_NAME}>/data/licenses/WebView2-NOTICE.txt"
)
