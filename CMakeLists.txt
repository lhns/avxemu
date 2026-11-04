cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/UPSTREAM_VERSION" AVXEMU_UPSTREAM LIMIT_COUNT 1)
set(AVXEMU_VERSION "${AVXEMU_UPSTREAM}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AVXEMU_VERSION LIMIT_COUNT 1)
endif()

project(AVXEmu LANGUAGES C ASM)

find_package(MavericksShipyard REQUIRED)
if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "AVXEmu supports Darwin and Linux targets")
endif()
set(AVXEMU_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(AVXEMU_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(NOT AVXEMU_TARGET_ARCH MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "AVXEmu requires x86-64")
endif()
option(AVXEMU_BUILD_UPDATER "Build the Sparkle updater" OFF)
if(NOT APPLE AND AVXEMU_BUILD_UPDATER)
  message(FATAL_ERROR "The Sparkle updater requires Darwin")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_compile_definitions(_DEFAULT_SOURCE)
  find_package(Threads REQUIRED)
  link_libraries(Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
option(AVXEMU_BACKEND_TESTS_ONLY "Build isolated Linux backend contracts without the runtime" OFF)
if(AVXEMU_BACKEND_TESTS_ONLY AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Backend-only contracts require x86-64 Linux")
endif()

# The emulator's runtime is SSE4.2-only, so it runs on the CPUs it exists for; the flags and
# the source order are build.sh's, which is what lets this build reproduce the CDN's objects.
set(AVXEMU_CORE_FLAGS -O2 -Wall -std=c11 -msse4.2 -mno-avx -mno-fma)
set(AVXEMU_HOST_FLAGS -O2 -std=c11 -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -mf16c)

function(avxemu_objects name)
  add_library(${name} OBJECT ${ARGN})
  target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:C>:${AVXEMU_CORE_FLAGS}>")
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
endfunction()

set(AVXEMU_COMMON_SOURCES
  src/exec.c src/exec_bmi.c src/softfma.c src/names.c src/decode.c
  src/lde.c src/scan.c src/emulate.c src/image.c src/cpuid.c src/patch_mem.c
  src/tramp.c src/reloc.c src/handler.c src/lcache.c src/lcache.h)
set(AVXEMU_ASM_SOURCES src/selftest.S src/selftest.c src/tramp.S)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  enable_testing()
  avxemu_objects(avxemu_linux_memory src/platform/linux/memory.c src/platform/linux/maps.c)
  avxemu_objects(avxemu_linux_host src/platform/linux/host.c)
  add_executable(memory-contract test/memory_contract.c $<TARGET_OBJECTS:avxemu_linux_memory>)
  target_link_libraries(memory-contract PRIVATE Threads::Threads)
  add_executable(linux-memory-faults test/linux_memory_faults.c $<TARGET_OBJECTS:avxemu_linux_memory>)
  target_link_options(linux-memory-faults PRIVATE
    -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=mmap -Wl,--wrap=munmap -Wl,--wrap=syscall)
  add_executable(linux-host-contract test/linux_host_contract.c $<TARGET_OBJECTS:avxemu_linux_host>)
  foreach(contract memory-contract linux-memory-faults linux-host-contract)
    target_compile_options(${contract} PRIVATE ${AVXEMU_CORE_FLAGS} -Wextra)
    target_include_directories(${contract} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    add_test(NAME ${contract} COMMAND $<TARGET_FILE:${contract}>)
    set_tests_properties(${contract} PROPERTIES LABELS hermetic TIMEOUT 60)
  endforeach()
  target_compile_options(avxemu_linux_memory PRIVATE -Wextra)
  target_compile_options(avxemu_linux_host PRIVATE -Wextra)
endif()

if(APPLE)
  include(Mavericks)
  include(MavericksMode)
  include(MavericksCompatGuard)
  mavericks_build_mode(MAVERICKS_MODE)
  mavericks_require_expected_mode("${MAVERICKS_MODE}")
  message(STATUS "AVXEmu ${AVXEMU_VERSION}: build mode ${MAVERICKS_MODE}")
  set(AVXEMU_PLATFORM darwin)
  avxemu_objects(avxemu_core ${AVXEMU_COMMON_SOURCES}
    src/platform/darwin/image.c src/platform/darwin/host.c
    src/platform/darwin/memory.c src/platform/darwin/context.c
    src/platform/darwin/loader.c)
else()
  set(AVXEMU_PLATFORM linux)
  avxemu_objects(avxemu_linux_elf src/platform/linux/elf.c)
  avxemu_objects(avxemu_linux_image src/platform/linux/image.c src/image.c)
  avxemu_objects(avxemu_linux_context src/platform/linux/context.c src/platform/linux/xstate.c)
  list(REMOVE_ITEM AVXEMU_COMMON_SOURCES src/image.c)
  avxemu_objects(avxemu_core ${AVXEMU_COMMON_SOURCES} src/platform/linux/loader.c)
  set(AVXEMU_PLATFORM_OBJECTS $<TARGET_OBJECTS:avxemu_linux_elf>
    $<TARGET_OBJECTS:avxemu_linux_image> $<TARGET_OBJECTS:avxemu_linux_context>
    $<TARGET_OBJECTS:avxemu_linux_memory> $<TARGET_OBJECTS:avxemu_linux_host>)
endif()
avxemu_objects(avxemu_asm ${AVXEMU_ASM_SOURCES})

if(NOT APPLE)
  # libavxemu_helpers.a: the emulation core alone, for a client that runs without libc on an
  # AVX1 CPU (contract: src/avxemu_helpers.h). AVX1 is the ceiling; test/check-helpers-lib.sh
  # verifies the built archive, since the CI CPU would run anything above it.
  add_library(avxemu_helpers STATIC src/exec.c src/exec_bmi.c src/softfma.c)
  target_include_directories(avxemu_helpers PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_options(avxemu_helpers PRIVATE -O2 -Wall -std=c11
    -mavx -mno-avx2 -mno-fma -mno-f16c -mno-bmi -mno-bmi2 -mno-lzcnt -mno-movbe
    -mtune=ivybridge -fPIC -fno-stack-protector -D_FORTIFY_SOURCE=0)
  set_target_properties(avxemu_helpers PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  include(GNUInstallDirs)
  install(TARGETS avxemu_helpers ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(FILES src/avxemu_helpers.h src/vexops.h src/ymm.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/avxemu")

  # The runtime without those three sources: linked with the archive it must answer the same
  # (semtest-vex in test/spec/spec.cmake).
  set(AVXEMU_VEX_SOURCES ${AVXEMU_COMMON_SOURCES})
  list(REMOVE_ITEM AVXEMU_VEX_SOURCES src/exec.c src/exec_bmi.c src/softfma.c)
  avxemu_objects(avxemu_core_vex ${AVXEMU_VEX_SOURCES} src/platform/linux/loader.c)
endif()

if(NOT AVXEMU_BACKEND_TESTS_ONLY)
  add_library(avxemu SHARED $<TARGET_OBJECTS:avxemu_core> $<TARGET_OBJECTS:avxemu_asm> ${AVXEMU_PLATFORM_OBJECTS})
  set_target_properties(avxemu PROPERTIES
    LINKER_LANGUAGE C
    MACOSX_RPATH ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(APPLE)
    mavericks_assert_binary_compatible(avxemu)
  endif()
  if(APPLE)
    install(TARGETS avxemu LIBRARY DESTINATION lib)
  else()
    include(GNUInstallDirs)
    install(TARGETS avxemu LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(FILES README.md DEVELOPING.md INGREDIENTS.md LICENSE PROVENANCE.md
      DESTINATION "${CMAKE_INSTALL_DOCDIR}")
  endif()
endif()

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/objects/core.txt"
  CONTENT "$<JOIN:$<TARGET_OBJECTS:avxemu_core>;${AVXEMU_PLATFORM_OBJECTS},\n>\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/objects/asm.txt"
  CONTENT "$<JOIN:$<TARGET_OBJECTS:avxemu_asm>,\n>\n")

if(AVXEMU_BUILD_UPDATER)
  include(MavericksSparkle)
  mavericks_add_updater_app(
    PRODUCT avxemu
    PRODUCT_NAME "AVXEmu"
    ALLOW_GENERIC
    VERSION "${AVXEMU_VERSION}"
    CONFIRM_TITLE "AVXEmu updated"
    CONFIRM_BODY "AVXEmu was updated. Programs that load it use the new version the next time they start.")
endif()

avxemu_objects(vex_negative test/vex_negative.S)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/objects/negative.txt"
  CONTENT "$<JOIN:$<TARGET_OBJECTS:vex_negative>,\n>\n")
enable_testing()
# shipyard's run-repo-tests.sh drives ctest only when THIS file declares an add_test, so the
# first test lives here rather than in test/CMakeLists.txt: without it CI would run nothing.
add_test(NAME vex-leak-core
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/test/vex-leak.sh"
          "${CMAKE_BINARY_DIR}/objects/core.txt" "${CMAKE_BINARY_DIR}/objects/asm.txt" "${CMAKE_BINARY_DIR}/objects/negative.txt")
set_tests_properties(vex-leak-core PROPERTIES LABELS hermetic SKIP_RETURN_CODE 77)
add_subdirectory(test)
