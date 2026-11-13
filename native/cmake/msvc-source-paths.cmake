# Keeps the build machine's folders out of the shipped programs (MSVC).
#
# The compiler writes the absolute path of a source or header into the program
# wherever code asks for its own location: __FILE__ (libopus' always-on
# hardening checks, celt_fatal in celt/arch.h; x264; our own code) and
# std::source_location (C++/WinRT's error reporting in the Windows SDK
# headers). A release build then carried the developer's profile folder.
#
# /pathmap:<prefix>=<replacement> rewrites those recorded paths; cl.exe accepts
# it only together with /experimental:deterministic (warning D9007 otherwise),
# which also replaces the PE time stamp with a content hash. The first
# matching prefix wins, so the specific folders come before the general ones.
# What remains is a short relative name such as opus\celt\bands.c.
# Only C and C++ get the options (rc.exe and NASM do not take them).
# Include it right after project(), before any target is defined.
# tools\release\scan-built-binaries.ps1 checks the result before publishing.
include_guard(DIRECTORY)
if(NOT MSVC)
  return()
endif()
get_filename_component(_multitap_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_multitap_maps
  "${CMAKE_BINARY_DIR}" build
  "${FETCHCONTENT_SOURCE_DIR_MULTITAP_OPUS}" opus
  "${MULTITAP_X264_SRC}" x264
  "$ENV{VCToolsInstallDir}" msvc
  "$ENV{UniversalCRTSdkDir}" winsdk
  "$ENV{WindowsSdkDir}" winsdk
  "${_multitap_repo}" multitap
  "$ENV{USERPROFILE}" home)
set(_multitap_seen "")
while(_multitap_maps)
  list(POP_FRONT _multitap_maps _multitap_prefix _multitap_name)
  if(NOT _multitap_prefix)
    continue()
  endif()
  get_filename_component(_multitap_prefix "${_multitap_prefix}" ABSOLUTE)
  string(REGEX REPLACE "[/\\\\]+$" "" _multitap_prefix "${_multitap_prefix}")
  string(TOLOWER "${_multitap_prefix}" _multitap_key)
  if(_multitap_prefix STREQUAL "" OR _multitap_key IN_LIST _multitap_seen)
    continue()
  endif()
  list(APPEND _multitap_seen "${_multitap_key}")
  # No trailing separator on either side: a quoted argument ending in a
  # backslash would escape its closing quote.
  file(TO_NATIVE_PATH "${_multitap_prefix}" _multitap_native)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/pathmap:${_multitap_native}=${_multitap_name}>")
endwhile()
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/experimental:deterministic>")
unset(_multitap_repo)
unset(_multitap_maps)
unset(_multitap_seen)
