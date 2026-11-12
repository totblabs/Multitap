# libopus (BSD 3-clause; COPYING, upstream.json) for streaming audio
# (audio-opus-v1, apps/windows_host/src/opus_audio.hpp):
# the Host encodes with it, the Windows client decodes with it. The official
# release tarball is pinned by SHA-256; CMake checks it before unpacking and
# builds it unmodified with libopus' own CMake as a static library at its
# defaults (float, runtime-dispatched x86 SIMD), without its programs, tests,
# DNN features (DRED/OSCE/deep PLC) or install rules.
# MULTITAP_OPUS_ARCHIVE (cache or environment): a local copy of the same tarball
# for offline builds; otherwise it is downloaded from downloads.xiph.org.
include_guard(GLOBAL)
include(FetchContent)
set(MULTITAP_OPUS_VERSION "1.6.1")
set(MULTITAP_OPUS_SHA256 "6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1")
set(MULTITAP_OPUS_ARCHIVE "$ENV{MULTITAP_OPUS_ARCHIVE}" CACHE FILEPATH "Local opus-${MULTITAP_OPUS_VERSION}.tar.gz (optional; checked against the pinned SHA-256)")
function(multitap_add_opus)
  if(MULTITAP_OPUS_ARCHIVE)
    set(url "${MULTITAP_OPUS_ARCHIVE}")
  else()
    set(url "https://downloads.xiph.org/releases/opus/opus-${MULTITAP_OPUS_VERSION}.tar.gz")
  endif()
  # Scoped to this function: libopus' option() takes these (CMP0077), the
  # including project keeps its own values.
  set(BUILD_TESTING OFF)
  set(BUILD_SHARED_LIBS OFF)
  set(OPUS_BUILD_SHARED_LIBRARY OFF)
  set(OPUS_BUILD_TESTING OFF)
  set(OPUS_BUILD_PROGRAMS OFF)
  set(OPUS_CUSTOM_MODES OFF)
  set(OPUS_DRED OFF)
  set(OPUS_OSCE OFF)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  # EXCLUDE_FROM_ALL: built only as a dependency, and its install rules stay
  # out of the including project's install (the client's bundle).
  FetchContent_Declare(multitap_opus URL "${url}" URL_HASH SHA256=${MULTITAP_OPUS_SHA256} EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(multitap_opus)
  file(STRINGS "${multitap_opus_SOURCE_DIR}/package_version" version REGEX "^PACKAGE_VERSION=")
  if(NOT version STREQUAL "PACKAGE_VERSION=\"${MULTITAP_OPUS_VERSION}\"")
    message(FATAL_ERROR "libopus ${MULTITAP_OPUS_VERSION} is required, found: ${version}")
  endif()
  set(MULTITAP_OPUS_SOURCE_DIR "${multitap_opus_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
multitap_add_opus()
