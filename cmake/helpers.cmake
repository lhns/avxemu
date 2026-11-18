# libavxemu_helpers.a: the emulation core alone, for a client that runs without libc on an AVX1
# CPU (contract: src/avxemu_helpers.h). Included by this repo's CMakeLists.txt and by consumers
# such as lhns/v3compat, so both build the same archive. No project(), no global state, CMake 3.18.
#
# Defines AVXEMU_EXEC_SOURCES (absolute paths) and the STATIC target avxemu_helpers, whose public
# include directory is src/. AVX1 is the ceiling: test/check-helpers-lib.sh verifies the built
# archive, since the CI CPU would run anything above it. -Wall is on; a consumer may add -w.
get_filename_component(_avxemu_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(AVXEMU_EXEC_SOURCES "${_avxemu_root}/src/exec.c" "${_avxemu_root}/src/exec_bmi.c" "${_avxemu_root}/src/softfma.c")
add_library(avxemu_helpers STATIC ${AVXEMU_EXEC_SOURCES})
target_include_directories(avxemu_helpers PUBLIC "${_avxemu_root}/src")
target_compile_options(avxemu_helpers PRIVATE -O2 -Wall -std=c11
  -mavx -mno-avx2 -mno-fma -mno-f16c -mno-bmi -mno-bmi2 -mno-lzcnt -mno-movbe
  -mtune=ivybridge -fPIC -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
unset(_avxemu_root)
