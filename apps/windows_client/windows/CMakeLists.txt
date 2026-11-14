# Project-level configuration.
cmake_minimum_required(VERSION 3.14)
project(multitap_client LANGUAGES CXX)
# No build-machine folder in the programs (__FILE__ in libopus, std::source_location); before any target.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../native/cmake/msvc-source-paths.cmake")

# The name of the executable created for the application. Change this to change
# the on-disk name of your application.
set(BINARY_NAME "Multitap")

# Explicitly opt in to modern CMake behaviors to avoid warnings with recent
# versions of CMake.
cmake_policy(VERSION 3.14...3.25)

# Define build configuration option.
get_property(IS_MULTICONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(IS_MULTICONFIG)
  set(CMAKE_CONFIGURATION_TYPES "Debug;Profile;Release"
    CACHE STRING "" FORCE)
else()
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE
      STRING "Flutter build mode" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
      "Debug" "Profile" "Release")
  endif()
endif()
# Define settings for the Profile build mode.
set(CMAKE_EXE_LINKER_FLAGS_PROFILE "${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
set(CMAKE_SHARED_LINKER_FLAGS_PROFILE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_PROFILE "${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_PROFILE "${CMAKE_CXX_FLAGS_RELEASE}")

# Use Unicode for all projects.
add_definitions(-DUNICODE -D_UNICODE)

# Compilation settings that should be applied to most targets.
#
# Be cautious about adding new options here, as plugins use this function by
# default. In most cases, you should add new options to specific targets instead
# of modifying this function.
function(APPLY_STANDARD_SETTINGS TARGET)
  target_compile_features(${TARGET} PUBLIC cxx_std_17)
  target_compile_options(${TARGET} PRIVATE /W4 /WX /wd4100)
  target_compile_options(${TARGET} PRIVATE /EHsc)
  target_compile_definitions(${TARGET} PRIVATE "_HAS_EXCEPTIONS=0")
  target_compile_definitions(${TARGET} PRIVATE "$<$<CONFIG:Debug>:_DEBUG>")
endfunction()

# Flutter library and tool build rules.
set(FLUTTER_MANAGED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/flutter")
add_subdirectory(${FLUTTER_MANAGED_DIR})

# Application build; see runner/CMakeLists.txt.
add_subdirectory("runner")


# Generated plugin build rules, which manage building the plugins and adding
# them to the application.
include(flutter/generated_plugins.cmake)


# === Installation ===
# Support files are copied into place next to the executable, so that it can
# run in place. This is done instead of making a separate bundle (as on Linux)
# so that building and running from within Visual Studio will work.
set(BUILD_BUNDLE_DIR "$<TARGET_FILE_DIR:${BINARY_NAME}>")
# Make the "install" step default, as it's required to run.
set(CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD 1)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${BUILD_BUNDLE_DIR}" CACHE PATH "..." FORCE)
endif()

set(INSTALL_BUNDLE_DATA_DIR "${CMAKE_INSTALL_PREFIX}/data")
set(INSTALL_BUNDLE_LIB_DIR "${CMAKE_INSTALL_PREFIX}")

install(TARGETS ${BINARY_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
  COMPONENT Runtime)

install(FILES "${FLUTTER_ICU_DATA_FILE}" DESTINATION "${INSTALL_BUNDLE_DATA_DIR}"
  COMPONENT Runtime)

install(FILES "${FLUTTER_LIBRARY}" DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
  COMPONENT Runtime)

# The runner's post-build rules stage third-party notices next to the EXE.
# Preserve them when the install bundle uses a separate output directory.
install(DIRECTORY "${BUILD_BUNDLE_DIR}/data/licenses/"
  DESTINATION "${INSTALL_BUNDLE_DATA_DIR}/licenses" COMPONENT Runtime)

if(PLUGIN_BUNDLED_LIBRARIES)
  install(FILES "${PLUGIN_BUNDLED_LIBRARIES}"
    DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
    COMPONENT Runtime)
endif()

# Copy the native assets provided by the build.dart from all packages.
set(NATIVE_ASSETS_DIR "${PROJECT_BUILD_DIR}native_assets/windows/")
install(DIRECTORY "${NATIVE_ASSETS_DIR}"
   DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
   COMPONENT Runtime)

# Fully re-copy the assets directory on each build to avoid having stale files
# from a previous install.
set(FLUTTER_ASSET_DIR_NAME "flutter_assets")
install(CODE "
  file(REMOVE_RECURSE \"${INSTALL_BUNDLE_DATA_DIR}/${FLUTTER_ASSET_DIR_NAME}\")
  " COMPONENT Runtime)
install(DIRECTORY "${PROJECT_BUILD_DIR}/${FLUTTER_ASSET_DIR_NAME}"
  DESTINATION "${INSTALL_BUNDLE_DATA_DIR}" COMPONENT Runtime)

# Install the AOT library on non-Debug builds only.
install(FILES "${AOT_LIBRARY}" DESTINATION "${INSTALL_BUNDLE_DATA_DIR}"
  CONFIGURATIONS Profile;Release
  COMPONENT Runtime)
# usb-v1: every client carries the pinned UsbDk installer (Red Hat, Apache-2.0)
# in usbdk/. The client verifies its size, SHA-256 and signature and installs it
# only from the user's explicit action (usb_capture.cpp); without it the USB
# panel can only say the installer is missing. The MSI is not in the repository:
# tools/fetch-usb-drivers.ps1 -UsbDk puts it in .tools. Its name, size and
# SHA-256 come from the pin (third_party/usbdk/upstream.json). Checked when the
# bundle is installed, so the smoke and QA targets still configure without it:
# a Release or Profile build stops without that exact file, a Debug build only
# warns. Host settings (resource build 20000 or more) never carry it.
if(FLUTTER_VERSION_BUILD LESS 20000)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/usbdk/upstream.json" MULTITAP_USBDK_PIN)
  string(JSON MULTITAP_USBDK_RELEASE GET "${MULTITAP_USBDK_PIN}" release)
  string(JSON MULTITAP_USBDK_NAME GET "${MULTITAP_USBDK_PIN}" file name)
  string(JSON MULTITAP_USBDK_SIZE GET "${MULTITAP_USBDK_PIN}" file size)
  string(JSON MULTITAP_USBDK_SHA256 GET "${MULTITAP_USBDK_PIN}" file sha256)
  set(MULTITAP_USBDK_MSI "${CMAKE_CURRENT_SOURCE_DIR}/../../../.tools/usb-drivers/usbdk-${MULTITAP_USBDK_RELEASE}/${MULTITAP_USBDK_NAME}")
  install(CODE "
    if(\"\${CMAKE_INSTALL_CONFIG_NAME}\" MATCHES \"^([Rr][Ee][Ll][Ee][Aa][Ss][Ee]|[Pp][Rr][Oo][Ff][Ii][Ll][Ee])\$\")
      set(MULTITAP_USBDK_LEVEL FATAL_ERROR)
    else()
      set(MULTITAP_USBDK_LEVEL WARNING)
    endif()
    if(NOT EXISTS \"${MULTITAP_USBDK_MSI}\")
      message(\${MULTITAP_USBDK_LEVEL} \"usbdk_installer_missing: run tools/fetch-usb-drivers.ps1 -UsbDk\")
    else()
      file(SIZE \"${MULTITAP_USBDK_MSI}\" MULTITAP_USBDK_BYTES)
      file(SHA256 \"${MULTITAP_USBDK_MSI}\" MULTITAP_USBDK_HASH)
      if(NOT MULTITAP_USBDK_BYTES EQUAL ${MULTITAP_USBDK_SIZE} OR NOT MULTITAP_USBDK_HASH STREQUAL \"${MULTITAP_USBDK_SHA256}\")
        message(\${MULTITAP_USBDK_LEVEL} \"usbdk_installer_changed: ${MULTITAP_USBDK_MSI} is not the pinned installer; run tools/fetch-usb-drivers.ps1 -UsbDk\")
      endif()
    endif()
    " COMPONENT Runtime)
  install(FILES "${MULTITAP_USBDK_MSI}" DESTINATION "${CMAKE_INSTALL_PREFIX}/usbdk"
    OPTIONAL COMPONENT Runtime)
endif()
