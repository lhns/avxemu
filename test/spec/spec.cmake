# Correctness suite: spec -> decoder -> semantics -> apps. Included from test/CMakeLists.txt.
set(SPEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spec")
set(KNOWN_FAILURES "${CMAKE_CURRENT_SOURCE_DIR}/known-failures.txt")
avxemu_objects(t_decode_spec "${PROJECT_SOURCE_DIR}/src/decode.c" "${PROJECT_SOURCE_DIR}/src/names.c")
avxemu_objects(t_spec "${SPEC_DIR}/spec.c" "${SPEC_DIR}/known.c")
target_include_directories(t_spec PRIVATE "${SPEC_DIR}")

# Stage 1: the manifest is complete and its statuses agree with decode.c.
avxemu_test_exe(spec-check SOURCES spec/spec_check.c LINK t_decode_spec t_spec)
target_include_directories(spec-check PRIVATE "${SPEC_DIR}")
add_test(NAME spec-complete COMMAND spec-check "${SPEC_DIR}/manifest.tsv" "${KNOWN_FAILURES}")
set_tests_properties(spec-complete PROPERTIES LABELS "spec;hermetic" ENVIRONMENT "AVXEMU_DISABLE=1")

# XED (test-only, Apache-2.0), fetched and built on request: AVXEMU_TEST_XED=ON.
option(AVXEMU_TEST_XED "Fetch Intel XED for the manifest generator and the decoder differential" OFF)
if(AVXEMU_TEST_XED)
  include(ExternalProject)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(XED_PREFIX "${CMAKE_BINARY_DIR}/xed")
  ExternalProject_Add(mbuild_src
    GIT_REPOSITORY https://github.com/intelxed/mbuild GIT_TAG 1b437e409221a2b5703b4d8896baa20d43e4ba1a
    SOURCE_DIR "${XED_PREFIX}/mbuild" CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "")
  ExternalProject_Add(xed_src
    GIT_REPOSITORY https://github.com/intelxed/xed GIT_TAG 0bcb6237345c5066726dcc08b3d87928df3b5b26 # v2026.08.23
    SOURCE_DIR "${XED_PREFIX}/xed" DEPENDS mbuild_src CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE 1 INSTALL_COMMAND ""
    BUILD_COMMAND "${Python3_EXECUTABLE}" mfile.py --install-dir=${XED_PREFIX}/inst --static -j8 install
    BUILD_BYPRODUCTS "${XED_PREFIX}/inst/lib/libxed.a")
  add_library(xed STATIC IMPORTED GLOBAL)
  set_target_properties(xed PROPERTIES IMPORTED_LOCATION "${XED_PREFIX}/inst/lib/libxed.a")
  add_dependencies(xed xed_src)
  set(XED_INCLUDE "${XED_PREFIX}/inst/include/xed")

  add_executable(xedgen spec/xedgen.c)
  target_include_directories(xedgen PRIVATE "${XED_INCLUDE}")
  target_link_libraries(xedgen PRIVATE xed)
  # The committed manifest is exactly what XED yields (forms and their order; statuses are ours).
  add_test(NAME spec-xed-fresh COMMAND sh "${SPEC_DIR}/fresh.sh" $<TARGET_FILE:xedgen>
    "${Python3_EXECUTABLE}" "${SPEC_DIR}")
  set_tests_properties(spec-xed-fresh PROPERTIES LABELS spec)
endif()

if(AVXEMU_TEST_XED)
  # Stage 2: decode.c and lde.c against XED, and the negative / prefix / random-bytes sweeps.
  avxemu_objects(t_lde_spec "${PROJECT_SOURCE_DIR}/src/lde.c")
  avxemu_test_exe(decoder-test SOURCES spec/decoder_test.c LINK t_decode_spec t_lde_spec t_spec)
  target_include_directories(decoder-test PRIVATE "${SPEC_DIR}" "${XED_INCLUDE}")
  target_link_libraries(decoder-test PRIVATE xed)
  add_dependencies(decoder-test xed_src)
  foreach(part positive negative fuzz)
    add_test(NAME decoder-${part} COMMAND decoder-test "${SPEC_DIR}/manifest.tsv" "${KNOWN_FAILURES}" ${part} 1000000)
    set_tests_properties(decoder-${part} PROPERTIES LABELS decoder TIMEOUT 600 ENVIRONMENT "AVXEMU_DISABLE=1")
  endforeach()
endif()

# Stage 3: semantics. The emulator side runs everywhere; the real-CPU side needs AVX2 & co.
add_executable(semtest spec/sem_test.c spec/sem_hw.S $<TARGET_OBJECTS:t_spec> $<TARGET_OBJECTS:t_refdigest>
  $<TARGET_OBJECTS:avxemu_core> $<TARGET_OBJECTS:avxemu_asm> ${AVXEMU_PLATFORM_OBJECTS})
target_compile_options(semtest PRIVATE "$<$<COMPILE_LANGUAGE:C>:${AVXEMU_CORE_FLAGS}>" -UNDEBUG)
target_compile_definitions(semtest PRIVATE SEM_MANIFEST="${SPEC_DIR}/manifest.tsv" SEM_KNOWN="${KNOWN_FAILURES}")
target_include_directories(semtest PRIVATE "${SPEC_DIR}" "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(semtest PROPERTIES LINKER_LANGUAGE C)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Static link: the interposed sigaction() has no libc original, so the fault handlers must not use it.
  add_executable(semtest-static spec/sem_test.c spec/sem_hw.S $<TARGET_OBJECTS:t_spec> $<TARGET_OBJECTS:t_refdigest>
    $<TARGET_OBJECTS:avxemu_core> $<TARGET_OBJECTS:avxemu_asm> ${AVXEMU_PLATFORM_OBJECTS})
  target_compile_options(semtest-static PRIVATE "$<$<COMPILE_LANGUAGE:C>:${AVXEMU_CORE_FLAGS}>" -UNDEBUG)
  target_compile_definitions(semtest-static PRIVATE SEM_MANIFEST="${SPEC_DIR}/manifest.tsv" SEM_KNOWN="${KNOWN_FAILURES}")
  target_include_directories(semtest-static PRIVATE "${SPEC_DIR}" "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_options(semtest-static PRIVATE -static)
  set_target_properties(semtest-static PROPERTIES LINKER_LANGUAGE C)
  add_test(NAME semantics-static-fault COMMAND semtest-static fault)
endif()
set(SEM_REF "${CMAKE_CURRENT_SOURCE_DIR}/reference/semantics.ref")
set(SEM_ARGS "${SPEC_DIR}/manifest.tsv" "${KNOWN_FAILURES}")
add_test(NAME semantics-self COMMAND semtest ${SEM_ARGS} self smoke)
add_test(NAME semantics-replay COMMAND semtest ${SEM_ARGS} check "${SEM_REF}" smoke)
add_test(NAME semantics-replay-full COMMAND semtest ${SEM_ARGS} check "${SEM_REF}" full)
add_test(NAME semantics-live COMMAND semtest ${SEM_ARGS} live smoke)
add_test(NAME semantics-live-full COMMAND semtest ${SEM_ARGS} live full)
if(EXISTS "${SEM_REF}")
  add_test(NAME semantics-record COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/record-matches.sh" $<TARGET_FILE:semtest>
    "${CMAKE_BINARY_DIR}/reference/semantics.ref" "${SEM_REF}")
  set_tests_properties(semantics-record PROPERTIES LABELS "silicon;semantics" ENVIRONMENT "AVXEMU_DISABLE=1" SKIP_RETURN_CODE 77)
endif()
set_tests_properties(semantics-self PROPERTIES LABELS "semantics;hermetic" TIMEOUT 300)
set_tests_properties(semantics-replay PROPERTIES LABELS "semantics;replay" TIMEOUT 300)
set_tests_properties(semantics-replay-full PROPERTIES LABELS "semantics;semantics-full" TIMEOUT 1800)
set_tests_properties(semantics-live PROPERTIES LABELS "semantics;silicon" TIMEOUT 600)
set_tests_properties(semantics-live-full PROPERTIES LABELS "semantics;silicon;semantics-full" TIMEOUT 3600)
set_tests_properties(semantics-self semantics-replay semantics-replay-full semantics-live semantics-live-full
  PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "AVXEMU_DISABLE=1")
# The same suite with the emulation core taken from libavxemu_helpers.a (AVX1 build) instead of
# the SSE-only objects: replay against the recorded references must give identical answers.
if(TARGET avxemu_core_vex)
  add_executable(semtest-vex spec/sem_test.c spec/sem_hw.S $<TARGET_OBJECTS:t_spec> $<TARGET_OBJECTS:t_refdigest>
    $<TARGET_OBJECTS:avxemu_core_vex> $<TARGET_OBJECTS:avxemu_asm> ${AVXEMU_PLATFORM_OBJECTS})
  target_compile_options(semtest-vex PRIVATE "$<$<COMPILE_LANGUAGE:C>:${AVXEMU_CORE_FLAGS}>" -UNDEBUG)
  target_compile_definitions(semtest-vex PRIVATE SEM_MANIFEST="${SPEC_DIR}/manifest.tsv" SEM_KNOWN="${KNOWN_FAILURES}")
  target_include_directories(semtest-vex PRIVATE "${SPEC_DIR}" "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(semtest-vex PRIVATE avxemu_helpers)
  set_target_properties(semtest-vex PROPERTIES LINKER_LANGUAGE C)
  add_test(NAME semantics-replay-vex COMMAND semtest-vex ${SEM_ARGS} check "${SEM_REF}" smoke)
  add_test(NAME semantics-replay-full-vex COMMAND semtest-vex ${SEM_ARGS} check "${SEM_REF}" full)
  set_tests_properties(semantics-replay-vex PROPERTIES LABELS "semantics;replay" TIMEOUT 300)
  set_tests_properties(semantics-replay-full-vex PROPERTIES LABELS "semantics;semantics-full" TIMEOUT 1800)
  set_tests_properties(semantics-replay-vex semantics-replay-full-vex
    PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "AVXEMU_DISABLE=1")
endif()
# QEMU TCG as a stand-in oracle when there is no AVX2 host: NOT silicon, so never in a default label.
find_program(QEMU_X86_64 qemu-x86_64)
if(QEMU_X86_64)
  add_test(NAME semantics-tcg COMMAND "${QEMU_X86_64}" -cpu max $<TARGET_FILE:semtest> ${SEM_ARGS} live smoke)
  set_tests_properties(semantics-tcg PROPERTIES LABELS tcg TIMEOUT 1800 SKIP_RETURN_CODE 77 ENVIRONMENT "AVXEMU_DISABLE=1")
endif()

# Stage 6: real programs natively and under the emulator (preload), outputs compared by sha256.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME apps COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/apps/run_apps.py"
    --lib $<TARGET_FILE:avxemu> --known "${KNOWN_FAILURES}" --expected "${CMAKE_CURRENT_SOURCE_DIR}/apps/expected.tsv" --work "${CMAKE_BINARY_DIR}/apps-work")
  set_tests_properties(apps PROPERTIES LABELS apps TIMEOUT 3600 SKIP_RETURN_CODE 77)
endif()
