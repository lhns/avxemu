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
