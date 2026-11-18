cmake_minimum_required(VERSION 3.25)
project(AVXEmu LANGUAGES C ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "This fork builds for x86-64 Linux only")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_definitions(_DEFAULT_SOURCE)
find_package(Threads REQUIRED)
link_libraries(Threads::Threads m)
enable_testing()

# The emulator side of the test suite is SSE4.2-only, so it runs on CPUs without AVX.
set(AVXEMU_CORE_FLAGS -O2 -Wall -std=c11 -msse4.2 -mno-avx -mno-fma)
set(AVXEMU_HOST_FLAGS -O2 -std=c11 -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -mf16c)

function(avxemu_objects name)
  add_library(${name} OBJECT ${ARGN})
  target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:C>:${AVXEMU_CORE_FLAGS}>")
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
endfunction()

# The emulator the suite compares against hardware: decoder, emulation core and the fault-safe
# memory reads it needs.
avxemu_objects(avxemu_linux_memory src/platform/linux/memory.c src/platform/linux/maps.c)
target_compile_options(avxemu_linux_memory PRIVATE -Wextra)
set(AVXEMU_PLATFORM_OBJECTS $<TARGET_OBJECTS:avxemu_linux_memory>)
set(AVXEMU_EMULATOR_SOURCES src/names.c src/decode.c src/emulate.c)
include(cmake/helpers.cmake)
avxemu_objects(avxemu_core ${AVXEMU_EXEC_SOURCES} ${AVXEMU_EMULATOR_SOURCES})

# libavxemu_helpers.a is defined in cmake/helpers.cmake, which v3compat includes too.
set_target_properties(avxemu_helpers PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
include(GNUInstallDirs)
install(TARGETS avxemu_helpers ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES src/avxemu_helpers.h src/vexops.h src/ymm.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/avxemu")

# The emulator without those three sources: linked with the archive it must answer the same
# (semtest-vex in test/spec/spec.cmake).
avxemu_objects(avxemu_core_vex ${AVXEMU_EMULATOR_SOURCES})

add_subdirectory(test)
