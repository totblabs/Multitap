# x264 (GPLv2-or-later) for the Host's production H.264 encoder
# (docs/x264-encoder.md). Builds libx264 statically from the pinned, unmodified
# upstream checkout with MSVC + NASM, following the object lists and flags of
# x264's own Makefile/configure for x86_64, 8-bit, no CLI, no OpenCL. Only the
# hand-written equivalents of configure's config.h/x264_config.h (./config) are
# Multitap files; nothing inside the x264 tree is changed or generated.
set(MULTITAP_X264_COMMIT "b35605ace3ddf7c1a5d67a2eb553f034aef41d55")
if(NOT EXISTS "${MULTITAP_X264_SRC}/x264.h" OR NOT EXISTS "${MULTITAP_X264_SRC}/encoder/encoder.c")
  message(FATAL_ERROR "MULTITAP_X264_SRC must be the x264 source checkout (third_party/x264/README.md).")
endif()
if(NOT EXISTS "${MULTITAP_NASM}")
  message(FATAL_ERROR "MULTITAP_NASM must name nasm.exe (2.13 or newer) for the x264 assembly.")
endif()
file(STRINGS "${MULTITAP_X264_SRC}/x264.h" MULTITAP_X264_BUILD_LINE REGEX "^#define X264_BUILD ")
if(NOT MULTITAP_X264_BUILD_LINE MATCHES "X264_BUILD 165$")
  message(FATAL_ERROR "x264 API build 165 (commit ${MULTITAP_X264_COMMIT}) is required, found: ${MULTITAP_X264_BUILD_LINE}")
endif()
# Reproducible and unmodified: exactly the pinned commit, no local changes, no
# untracked or ignored files (an in-tree configure output would also be refused).
find_package(Git QUIET)
if(NOT GIT_FOUND)
  message(FATAL_ERROR "git is required to verify the pinned x264 checkout.")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${MULTITAP_X264_SRC}" rev-parse HEAD
  OUTPUT_VARIABLE MULTITAP_X264_HEAD RESULT_VARIABLE MULTITAP_X264_GIT_RESULT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(MULTITAP_X264_GIT_RESULT OR NOT MULTITAP_X264_HEAD STREQUAL MULTITAP_X264_COMMIT)
  message(FATAL_ERROR "x264 checkout must be commit ${MULTITAP_X264_COMMIT}, found '${MULTITAP_X264_HEAD}'.")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${MULTITAP_X264_SRC}" status --porcelain --ignored
  OUTPUT_VARIABLE MULTITAP_X264_DIRTY RESULT_VARIABLE MULTITAP_X264_GIT_RESULT ERROR_QUIET)
if(MULTITAP_X264_GIT_RESULT OR NOT MULTITAP_X264_DIRTY STREQUAL "")
  message(FATAL_ERROR "x264 checkout ${MULTITAP_X264_SRC} is modified or contains extra files:\n${MULTITAP_X264_DIRTY}")
endif()

enable_language(C)
set(MULTITAP_X264_CONFIG "${CMAKE_CURRENT_LIST_DIR}/config")
# Makefile SRCS (+ win32thread) and SRCS_X (+ threadpool, x86 C helpers).
set(MULTITAP_X264_C_ANY common/osdep.c common/base.c common/cpu.c common/tables.c encoder/api.c common/win32thread.c)
set(MULTITAP_X264_C_8 common/mc.c common/predict.c common/pixel.c common/macroblock.c
  common/frame.c common/dct.c common/cabac.c common/common.c common/rectangle.c
  common/set.c common/quant.c common/deblock.c common/vlc.c common/mvpred.c common/bitstream.c
  encoder/analyse.c encoder/me.c encoder/ratecontrol.c encoder/set.c encoder/macroblock.c
  encoder/cabac.c encoder/cavlc.c encoder/encoder.c encoder/lookahead.c
  common/threadpool.c common/x86/mc-c.c common/x86/predict-c.c)
list(TRANSFORM MULTITAP_X264_C_ANY PREPEND "${MULTITAP_X264_SRC}/")
list(TRANSFORM MULTITAP_X264_C_8 PREPEND "${MULTITAP_X264_SRC}/")
add_library(multitap_x264_any OBJECT ${MULTITAP_X264_C_ANY})
add_library(multitap_x264_8 OBJECT ${MULTITAP_X264_C_8})
# Makefile: %-8.o: %.c ... -DHIGH_BIT_DEPTH=0 -DBIT_DEPTH=8
target_compile_definitions(multitap_x264_8 PRIVATE HIGH_BIT_DEPTH=0 BIT_DEPTH=8)
foreach(target multitap_x264_any multitap_x264_8)
  target_include_directories(${target} PRIVATE "${MULTITAP_X264_CONFIG}" "${MULTITAP_X264_SRC}" "${MULTITAP_X264_SRC}/extras")
  # configure (compiler=CL): -nologo -GS- -DHAVE_STRING_H, -O3 -> -O2, -ffast-math -> -fp:fast, -DNDEBUG
  target_compile_definitions(${target} PRIVATE HAVE_STRING_H NDEBUG)
  target_compile_options(${target} PRIVATE /GS- /fp:fast /O2 /W0)
endforeach()
# Makefile OBJASM for SYS_ARCH=X86_64 with HAVE_BITDEPTH8.
set(MULTITAP_X264_ASM_8 dct-64 trellis-64 bitstream-a const-a cabac-a dct-a deblock-a mc-a mc-a2 pixel-a predict-a quant-a sad-a)
set(MULTITAP_X264_ASM_OBJECTS)
set(MULTITAP_X264_NASM_FLAGS -f win64 -DARCH_X86_64=1 -DSTACK_ALIGNMENT=16 "-I${MULTITAP_X264_SRC}/common/x86/" "-I${MULTITAP_X264_SRC}/" "-I${MULTITAP_X264_CONFIG}/")
set(MULTITAP_X264_ASM_DEPENDS "${MULTITAP_X264_SRC}/common/x86/x86inc.asm" "${MULTITAP_X264_SRC}/common/x86/x86util.asm")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/x264-asm")
set(object "${CMAKE_CURRENT_BINARY_DIR}/x264-asm/cpu-a.obj")
add_custom_command(OUTPUT "${object}" COMMAND "${MULTITAP_NASM}" ${MULTITAP_X264_NASM_FLAGS} -o "${object}" "${MULTITAP_X264_SRC}/common/x86/cpu-a.asm"
  DEPENDS "${MULTITAP_X264_SRC}/common/x86/cpu-a.asm" ${MULTITAP_X264_ASM_DEPENDS} VERBATIM)
list(APPEND MULTITAP_X264_ASM_OBJECTS "${object}")
foreach(source ${MULTITAP_X264_ASM_8})
  set(object "${CMAKE_CURRENT_BINARY_DIR}/x264-asm/${source}-8.obj")
  # Makefile: %-8.o: %.asm ... -DBIT_DEPTH=8 -Dprivate_prefix=x264_8
  add_custom_command(OUTPUT "${object}" COMMAND "${MULTITAP_NASM}" ${MULTITAP_X264_NASM_FLAGS} -DBIT_DEPTH=8 -Dprivate_prefix=x264_8 -o "${object}" "${MULTITAP_X264_SRC}/common/x86/${source}.asm"
    DEPENDS "${MULTITAP_X264_SRC}/common/x86/${source}.asm" ${MULTITAP_X264_ASM_DEPENDS} VERBATIM)
  list(APPEND MULTITAP_X264_ASM_OBJECTS "${object}")
endforeach()
set_source_files_properties(${MULTITAP_X264_ASM_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(multitap_x264 STATIC $<TARGET_OBJECTS:multitap_x264_any> $<TARGET_OBJECTS:multitap_x264_8> ${MULTITAP_X264_ASM_OBJECTS})
set_target_properties(multitap_x264 PROPERTIES LINKER_LANGUAGE C)
target_include_directories(multitap_x264 INTERFACE "${MULTITAP_X264_CONFIG}" "${MULTITAP_X264_SRC}")
