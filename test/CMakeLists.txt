if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  foreach(obj avxemu_linux_elf avxemu_linux_image avxemu_linux_memory avxemu_linux_host)
    set_target_properties(${obj} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endforeach()
  add_library(image-backend SHARED $<TARGET_OBJECTS:avxemu_linux_elf>
    $<TARGET_OBJECTS:avxemu_linux_image> $<TARGET_OBJECTS:avxemu_linux_memory>
    $<TARGET_OBJECTS:avxemu_linux_host>)
  target_link_libraries(image-backend PRIVATE ${CMAKE_DL_LIBS})
  foreach(kind pie exec)
    add_executable(elf-probe-${kind} fixtures/elf_probe.c fixtures/elf_probe.S)
    if(kind STREQUAL "pie")
      target_compile_options(elf-probe-${kind} PRIVATE -fPIE)
      target_link_options(elf-probe-${kind} PRIVATE -pie)
    else()
      target_compile_options(elf-probe-${kind} PRIVATE -fno-pie)
      target_link_options(elf-probe-${kind} PRIVATE -no-pie)
    endif()
    add_custom_command(TARGET elf-probe-${kind} POST_BUILD
      COMMAND "${CMAKE_STRIP}" --strip-all -o "$<TARGET_FILE:elf-probe-${kind}>-stripped"
        "$<TARGET_FILE:elf-probe-${kind}>" VERBATIM)
  endforeach()
  add_executable(elf-contract elf_contract.c $<TARGET_OBJECTS:avxemu_linux_elf>)
  add_dependencies(elf-contract elf-probe-pie elf-probe-exec)
  add_test(NAME elf-contract COMMAND elf-contract
    $<TARGET_FILE:elf-probe-pie> "$<TARGET_FILE:elf-probe-pie>-stripped"
    $<TARGET_FILE:elf-probe-exec> "$<TARGET_FILE:elf-probe-exec>-stripped")
  add_executable(image-contract image_contract.c "${PROJECT_SOURCE_DIR}/src/image.c")
  add_test(NAME image-contract COMMAND image-contract)
  add_executable(linux-image-live linux_image_live.c fixtures/elf_probe.S
    $<TARGET_OBJECTS:avxemu_linux_elf> $<TARGET_OBJECTS:avxemu_linux_image>
    $<TARGET_OBJECTS:avxemu_linux_memory> $<TARGET_OBJECTS:avxemu_linux_host>)
  target_link_libraries(linux-image-live PRIVATE ${CMAKE_DL_LIBS})
  add_executable(linux-image-linked linux_image_live.c fixtures/elf_probe.S)
  target_link_libraries(linux-image-linked PRIVATE image-backend)
  add_executable(linux-image-exec linux_image_live.c fixtures/elf_probe.S)
  target_compile_options(linux-image-exec PRIVATE -fno-pie)
  target_link_options(linux-image-exec PRIVATE -no-pie)
  target_link_libraries(linux-image-exec PRIVATE image-backend)
  foreach(contract elf-contract image-contract linux-image-live linux-image-linked linux-image-exec)
    target_compile_options(${contract} PRIVATE ${AVXEMU_CORE_FLAGS} -Wextra -UNDEBUG)
    target_include_directories(${contract} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  endforeach()
  add_test(NAME linux-image-live COMMAND linux-image-live embedded)
  add_test(NAME linux-image-linked COMMAND linux-image-linked shared)
  add_test(NAME linux-image-exec COMMAND linux-image-exec shared)
  # glibc's explicit loader invocation changes /proc/self/exe to the loader.
  find_file(AVXEMU_TEST_LD_SO ld-linux-x86-64.so.2 PATHS /lib64 /lib/x86_64-linux-gnu NO_DEFAULT_PATH)
  if(AVXEMU_TEST_LD_SO)
    add_test(NAME linux-image-loader COMMAND "${AVXEMU_TEST_LD_SO}" $<TARGET_FILE:linux-image-linked> shared)
    add_test(NAME linux-image-loader-exec COMMAND "${AVXEMU_TEST_LD_SO}" $<TARGET_FILE:linux-image-exec> shared)
  endif()
  add_executable(linux-image-faults linux_image_faults.c $<TARGET_OBJECTS:avxemu_linux_elf>
    $<TARGET_OBJECTS:avxemu_linux_image> $<TARGET_OBJECTS:avxemu_linux_memory>)
  target_compile_options(linux-image-faults PRIVATE ${AVXEMU_CORE_FLAGS} -Wextra -UNDEBUG)
  target_include_directories(linux-image-faults PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(linux-image-faults PRIVATE ${CMAKE_DL_LIBS})
  target_link_options(linux-image-faults PRIVATE -Wl,--wrap=open -Wl,--wrap=mmap -Wl,--wrap=munmap)
  foreach(mode 1 2 3)
    add_test(NAME linux-image-faults-${mode} COMMAND linux-image-faults ${mode} $<TARGET_FILE:elf-probe-pie>)
  endforeach()
  add_library(image-backend-no-id SHARED $<TARGET_OBJECTS:avxemu_linux_elf>
    $<TARGET_OBJECTS:avxemu_linux_image> $<TARGET_OBJECTS:avxemu_linux_memory>
    $<TARGET_OBJECTS:avxemu_linux_host>)
  target_link_options(image-backend-no-id PRIVATE -Wl,--build-id=none)
  target_link_libraries(image-backend-no-id PRIVATE ${CMAKE_DL_LIBS})
  add_executable(linux-image-no-id linux_image_no_id.c)
  target_compile_options(linux-image-no-id PRIVATE ${AVXEMU_CORE_FLAGS} -Wextra -UNDEBUG)
  target_include_directories(linux-image-no-id PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(linux-image-no-id PRIVATE image-backend-no-id)
  add_test(NAME linux-image-no-id COMMAND linux-image-no-id)
  add_executable(xstate-contract linux_xstate_contract.c
    $<TARGET_OBJECTS:avxemu_linux_context> $<TARGET_OBJECTS:avxemu_linux_memory>)
  target_compile_options(xstate-contract PRIVATE ${AVXEMU_CORE_FLAGS} -Wextra -UNDEBUG)
  target_include_directories(xstate-contract PRIVATE "${PROJECT_SOURCE_DIR}/src")
  add_test(NAME xstate-contract COMMAND xstate-contract)

  foreach(contract context-contract loader-contract linux-loader-failure linux-signal-contract)
    string(REPLACE "-" "_" source "${contract}")
    add_executable(${contract} "${source}.c" $<TARGET_OBJECTS:avxemu_core> $<TARGET_OBJECTS:avxemu_asm> ${AVXEMU_PLATFORM_OBJECTS})
    target_compile_options(${contract} PRIVATE ${AVXEMU_CORE_FLAGS} -UNDEBUG)
    target_include_directories(${contract} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${contract} PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    add_test(NAME ${contract} COMMAND ${contract})
    set_tests_properties(${contract} PROPERTIES
      ENVIRONMENT "AVXEMU_ASSUME_CAPABLE=1;AVXEMU_NOCACHE=1")
  endforeach()
  target_compile_options(linux-signal-contract PRIVATE -U_DEFAULT_SOURCE)
  target_sources(context-contract PRIVATE context_contract.S)
  target_sources(loader-contract PRIVATE guard_operand.S)
  target_link_options(linux-loader-failure PRIVATE -Wl,--wrap=dlvsym)
  foreach(mode self recursive)
    add_test(NAME linux-loader-failure-${mode} COMMAND linux-loader-failure)
    set_tests_properties(linux-loader-failure-${mode} PROPERTIES
      ENVIRONMENT "AVXEMU_ASSUME_CAPABLE=1;AVXEMU_NOCACHE=1;AVXEMU_RESOLVE_FAILURE=${mode}")
  endforeach()
  get_property(backend_tests DIRECTORY PROPERTY TESTS)
  set_tests_properties(${backend_tests} PROPERTIES LABELS hermetic TIMEOUT 60)
  set_tests_properties(loader-contract PROPERTIES TIMEOUT 300)
  if(AVXEMU_BACKEND_TESTS_ONLY)
    return()
  endif()
endif()

# avxemu's tests. Each later task appends; the _replay_objs block added in Task 7, and then the
# AVXEMU_NOCACHE block, stay last.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(linux-signal-preload linux_signal_contract.c)
  target_compile_options(linux-signal-preload PRIVATE ${AVXEMU_CORE_FLAGS} -U_DEFAULT_SOURCE -UNDEBUG)
  target_compile_definitions(linux-signal-preload PRIVATE AVXEMU_PRELOAD_TEST)
  target_include_directories(linux-signal-preload PRIVATE "${PROJECT_SOURCE_DIR}/src")
  add_test(NAME linux-signal-preload COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh"
    $<TARGET_FILE:avxemu> $<TARGET_FILE:linux-signal-preload>)
  set_tests_properties(linux-signal-preload PROPERTIES LABELS hermetic
    ENVIRONMENT "AVXEMU_ASSUME_CAPABLE=1" PASS_REGULAR_EXPRESSION "linux-signal-contract: PASS")
  add_executable(external-entry fixtures/external_entry.c fixtures/external_entry.S)
  add_test(NAME required-external-entry COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/strict-cache.sh"
    $<TARGET_FILE:avxemu> $<TARGET_FILE:external-entry> "required prefix=f0")
  add_executable(external-overlap fixtures/external_entry.c fixtures/external_entry.S)
  target_compile_definitions(external-overlap PRIVATE AVXEMU_BAD_ENTRY)
  add_test(NAME required-reject-external-entry COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/required-reject.sh"
    $<TARGET_FILE:avxemu> $<TARGET_FILE:external-overlap>)
  add_executable(optional-data fixtures/optional_data.c fixtures/optional_data.S)
  add_test(NAME optional-data COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/strict-cache.sh"
    $<TARGET_FILE:avxemu> $<TARGET_FILE:optional-data> "optional data: PASS")
  add_executable(optional-replay optional_replay.c fixtures/optional_data.S)
  target_include_directories(optional-replay PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_options(optional-replay PRIVATE ${AVXEMU_CORE_FLAGS} -UNDEBUG)
  target_link_libraries(optional-replay PRIVATE avxemu)
  add_test(NAME optional-replay COMMAND optional-replay)
  set_tests_properties(optional-replay PROPERTIES LABELS hermetic TIMEOUT 30
    ENVIRONMENT "AVXEMU_DISABLE=1;AVXEMU_FORCETRAMP=1")
  add_executable(pending-signal pending_signal.c pending_signal.S)
  add_test(NAME pending-signal COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh"
    $<TARGET_FILE:avxemu> $<TARGET_FILE:pending-signal>)
  set_tests_properties(required-external-entry required-reject-external-entry optional-data pending-signal PROPERTIES
    LABELS hermetic TIMEOUT 30 ENVIRONMENT "AVXEMU_FORCEPATCH=1")
  add_executable(runtime-unknown runtime_load.c required_gap.S)
  target_include_directories(runtime-unknown PRIVATE "${PROJECT_SOURCE_DIR}/src")
  add_test(NAME required-reject-gap COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/required-reject.sh"
    $<TARGET_FILE:avxemu> $<TARGET_FILE:runtime-unknown>)
  set_tests_properties(required-reject-gap PROPERTIES LABELS hermetic)
  foreach(kind pie exec)
    foreach(mode linked preload)
      set(t runtime-${mode}-${kind})
      add_executable(${t} runtime_load.c)
      if(kind STREQUAL "pie")
        target_compile_options(${t} PRIVATE -fPIE)
        target_link_options(${t} PRIVATE -pie)
      else()
        target_compile_options(${t} PRIVATE -fno-pie)
        target_link_options(${t} PRIVATE -no-pie)
      endif()
      if(mode STREQUAL "linked")
        target_compile_definitions(${t} PRIVATE AVXEMU_LINKED)
        target_link_options(${t} PRIVATE -Wl,--as-needed)
        target_link_libraries(${t} PRIVATE avxemu)
        set(command "")
      else()
        set(command sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh" $<TARGET_FILE:avxemu>)
      endif()
      add_custom_command(TARGET ${t} POST_BUILD COMMAND "${CMAKE_STRIP}" --strip-all
        -o "$<TARGET_FILE:${t}>-stripped" "$<TARGET_FILE:${t}>" VERBATIM)
      foreach(suffix "" -stripped)
        add_test(NAME ${t}${suffix} COMMAND ${command} "$<TARGET_FILE:${t}>${suffix}")
        set_tests_properties(${t}${suffix} PROPERTIES LABELS hermetic
          ENVIRONMENT "AVXEMU_FORCEPATCH=1" PASS_REGULAR_EXPRESSION "runtime-load: OK")
      endforeach()
    endforeach()
  endforeach()
  set_property(TARGET runtime-preload-pie APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/no-functions.sh")
  add_custom_command(TARGET runtime-preload-pie POST_BUILD
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/no-functions.sh" "${CMAKE_OBJCOPY}"
      "$<TARGET_FILE:runtime-preload-pie>" "$<TARGET_FILE:runtime-preload-pie>-no-functions" VERBATIM)
  add_test(NAME required-reject-missing COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/required-reject.sh"
    $<TARGET_FILE:avxemu> "$<TARGET_FILE:runtime-preload-pie>-no-functions")
  set_tests_properties(required-reject-missing PROPERTIES LABELS hermetic)

endif()

avxemu_objects(t_cpu cpu.c)

add_executable(cpuprobe cpuprobe.c $<TARGET_OBJECTS:t_cpu>)
target_compile_options(cpuprobe PRIVATE "$<$<COMPILE_LANGUAGE:C>:${AVXEMU_CORE_FLAGS}>")
set(AVXEMU_RUN sh "${CMAKE_CURRENT_SOURCE_DIR}/run.sh" --cpuprobe $<TARGET_FILE:cpuprobe>)

function(avxemu_test_exe name)
  cmake_parse_arguments(A "" "" "FLAGS;SOURCES;LINK" ${ARGN})
  if(NOT A_FLAGS)
    set(A_FLAGS ${AVXEMU_CORE_FLAGS})
  endif()
  set(objs "")
  foreach(l IN LISTS A_LINK)
    list(APPEND objs $<TARGET_OBJECTS:${l}>)
  endforeach()
  if("avxemu_core" IN_LIST A_LINK)
    list(APPEND objs ${AVXEMU_PLATFORM_OBJECTS})
  endif()
  add_executable(${name} ${A_SOURCES} ${objs})
  target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:C>:${A_FLAGS}>")
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
endfunction()

add_test(NAME vex-leak-controls COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/vex-leak-test.sh"
  "${CMAKE_CURRENT_SOURCE_DIR}/vex-leak.sh" "${CMAKE_BINARY_DIR}/objects/asm.txt"
  "${CMAKE_BINARY_DIR}/objects/negative.txt")
set_tests_properties(vex-leak-controls PROPERTIES LABELS hermetic)

add_test(NAME run-wrapper COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/run-test.sh" "${CMAKE_CURRENT_SOURCE_DIR}/run.sh")

avxemu_test_exe(asm-contract SOURCES asm_contract.c "${PROJECT_SOURCE_DIR}/src/tramp.S")
add_test(NAME asm-contract COMMAND $<TARGET_FILE:asm-contract>)

if(APPLE)
avxemu_test_exe(image-contract SOURCES image_contract.c "${PROJECT_SOURCE_DIR}/src/image.c")
add_test(NAME image-contract COMMAND $<TARGET_FILE:image-contract>)

# Exercise metadata with emulator objects inside the executable and in a dylib.
avxemu_test_exe(darwin-data-contract SOURCES darwin_data_contract.c LINK avxemu_core avxemu_asm)
target_compile_options(darwin-data-contract PRIVATE -UNDEBUG)
add_test(NAME darwin-data-contract COMMAND darwin-data-contract)
set_tests_properties(darwin-data-contract PROPERTIES LABELS hermetic ENVIRONMENT "AVXEMU_DISABLE=1")
avxemu_test_exe(image-live SOURCES image_live.c LINK avxemu_core avxemu_asm)
add_test(NAME image-live COMMAND $<TARGET_FILE:image-live> embedded)
avxemu_test_exe(image-live-linked SOURCES image_live.c)
target_link_libraries(image-live-linked PRIVATE avxemu)
add_test(NAME image-live-linked COMMAND $<TARGET_FILE:image-live-linked> shared)

endif()
avxemu_test_exe(prefix-coverage SOURCES prefix_coverage.c)
target_link_libraries(prefix-coverage PRIVATE avxemu)
target_compile_options(prefix-coverage PRIVATE -UNDEBUG)
add_test(NAME prefix-coverage COMMAND prefix-coverage)
set_tests_properties(prefix-coverage PROPERTIES LABELS hermetic ENVIRONMENT "AVXEMU_DISABLE=1")
add_test(NAME prefix-handler COMMAND prefix-coverage handler)
avxemu_test_exe(crc32-decode SOURCES crc32_decode.c)
target_link_libraries(crc32-decode PRIVATE avxemu)
target_compile_options(crc32-decode PRIVATE -UNDEBUG)
add_test(NAME crc32-decode COMMAND crc32-decode)
set_tests_properties(crc32-decode PROPERTIES LABELS hermetic ENVIRONMENT "AVXEMU_DISABLE=1")
avxemu_test_exe(crc32-e2e SOURCES crc32_e2e.c)
add_test(NAME crc32-e2e COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh" $<TARGET_FILE:avxemu> $<TARGET_FILE:crc32-e2e>)
set_tests_properties(crc32-e2e PROPERTIES LABELS hermetic TIMEOUT 30
  ENVIRONMENT "AVXEMU_FORCEPATCH=1;AVXEMU_FORCETRAMP=1" PASS_REGULAR_EXPRESSION "crc32-e2e: OK")
avxemu_test_exe(mxcsr-fma SOURCES mxcsr_fma.c)
foreach(mode tramp handler)
  add_test(NAME mxcsr-fma-${mode} COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh" $<TARGET_FILE:avxemu> $<TARGET_FILE:mxcsr-fma> ${mode})
  set_tests_properties(mxcsr-fma-${mode} PROPERTIES LABELS hermetic TIMEOUT 30
    ENVIRONMENT "AVXEMU_FORCEPATCH=1;AVXEMU_NOCACHE=1" PASS_REGULAR_EXPRESSION "mxcsr-fma: OK")
endforeach()
set_property(TEST mxcsr-fma-tramp APPEND PROPERTY ENVIRONMENT "AVXEMU_FORCETRAMP=1")
avxemu_test_exe(addr32 SOURCES addr32.c)
foreach(mode tramp handler)
  add_test(NAME addr32-${mode} COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh" $<TARGET_FILE:avxemu> $<TARGET_FILE:addr32> ${mode})
  set_tests_properties(addr32-${mode} PROPERTIES LABELS hermetic TIMEOUT 30
    ENVIRONMENT "AVXEMU_FORCEPATCH=1;AVXEMU_NOCACHE=1" PASS_REGULAR_EXPRESSION "addr32: OK")
endforeach()
set_property(TEST addr32-tramp APPEND PROPERTY ENVIRONMENT "AVXEMU_FORCETRAMP=1")
avxemu_objects(t_softfma "${PROJECT_SOURCE_DIR}/src/softfma.c")
avxemu_test_exe(half-snan SOURCES half_snan.c LINK t_softfma)
target_compile_options(half-snan PRIVATE -UNDEBUG)
add_test(NAME half-snan COMMAND half-snan)
set_tests_properties(half-snan PROPERTIES LABELS hermetic)
avxemu_test_exe(shift-mem SOURCES shift_mem.c)
foreach(mode tramp handler)
  add_test(NAME shift-mem-${mode} COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh" $<TARGET_FILE:avxemu> $<TARGET_FILE:shift-mem> ${mode})
  set_tests_properties(shift-mem-${mode} PROPERTIES LABELS hermetic TIMEOUT 30
    ENVIRONMENT "AVXEMU_FORCEPATCH=1;AVXEMU_NOCACHE=1" PASS_REGULAR_EXPRESSION "shift-mem: OK")
endforeach()
set_property(TEST shift-mem-tramp APPEND PROPERTY ENVIRONMENT "AVXEMU_FORCETRAMP=1")
set_tests_properties(prefix-handler PROPERTIES LABELS hermetic TIMEOUT 20 ENVIRONMENT "AVXEMU_FORCEPATCH=1;AVXEMU_NOCACHE=1")

avxemu_test_exe(required-scan SOURCES required_scan.c LINK avxemu_core avxemu_asm)
target_compile_options(required-scan PRIVATE -UNDEBUG)
add_test(NAME required-scan COMMAND required-scan)
set_tests_properties(required-scan PROPERTIES LABELS hermetic ENVIRONMENT "AVXEMU_DISABLE=1")

avxemu_test_exe(scan-contract SOURCES scan_contract.c LINK avxemu_core avxemu_asm)
target_compile_options(scan-contract PRIVATE -UNDEBUG)
add_test(NAME scan-contract COMMAND $<TARGET_FILE:scan-contract>)
set_tests_properties(scan-contract PROPERTIES ENVIRONMENT "AVXEMU_DISABLE=1")

# Synthetic image and identities, with the real cache, scanner and memory backend.
avxemu_test_exe(cache-ranges SOURCES cache_ranges.c
  "${PROJECT_SOURCE_DIR}/src/lcache.c" "${PROJECT_SOURCE_DIR}/src/cpuid.c"
  "${PROJECT_SOURCE_DIR}/src/patch_mem.c" "${PROJECT_SOURCE_DIR}/src/scan.c" "${PROJECT_SOURCE_DIR}/src/lde.c" "${PROJECT_SOURCE_DIR}/src/decode.c"
  "${PROJECT_SOURCE_DIR}/src/image.c" "${PROJECT_SOURCE_DIR}/src/platform/${AVXEMU_PLATFORM}/memory.c"
  $<$<PLATFORM_ID:Linux>:${PROJECT_SOURCE_DIR}/src/platform/linux/maps.c>)
target_compile_options(cache-ranges PRIVATE -UNDEBUG)
add_test(NAME cache-ranges COMMAND $<TARGET_FILE:cache-ranges>)

if(APPLE)
avxemu_test_exe(memory-contract SOURCES memory_contract.c "${PROJECT_SOURCE_DIR}/src/platform/${AVXEMU_PLATFORM}/memory.c"
  $<$<PLATFORM_ID:Linux>:${PROJECT_SOURCE_DIR}/src/platform/linux/maps.c>)
find_package(Threads REQUIRED)
target_link_libraries(memory-contract PRIVATE Threads::Threads)
add_test(NAME memory-contract COMMAND $<TARGET_FILE:memory-contract>)

avxemu_test_exe(context-contract SOURCES context_contract.c context_contract.S LINK avxemu_core avxemu_asm)
add_test(NAME context-contract COMMAND $<TARGET_FILE:context-contract>)
avxemu_test_exe(loader-contract SOURCES loader_contract.c guard_operand.S LINK avxemu_core avxemu_asm)
add_test(NAME loader-contract COMMAND $<TARGET_FILE:loader-contract>)
set_tests_properties(loader-contract PROPERTIES ENVIRONMENT "AVXEMU_ASSUME_CAPABLE=1" TIMEOUT 300)

endif()
avxemu_test_exe(inject SOURCES inject.c stubs.S LINK avxemu_core avxemu_asm)
add_test(NAME inject COMMAND $<TARGET_FILE:inject>)

avxemu_test_exe(memtest SOURCES memtest.c memtest.S LINK avxemu_core avxemu_asm)
add_test(NAME memtest COMMAND $<TARGET_FILE:memtest>)

avxemu_test_exe(tramptest SOURCES tramptest.c tramp_harness.S LINK avxemu_core avxemu_asm)
add_test(NAME tramptest COMMAND $<TARGET_FILE:tramptest>)

avxemu_test_exe(operand-faults SOURCES operand_faults.c LINK avxemu_core avxemu_asm)
target_compile_options(operand-faults PRIVATE -UNDEBUG)
if(NOT APPLE)
  target_link_options(operand-faults PRIVATE -Wl,--wrap=avxemu_memory_read)
endif()
add_test(NAME operand-faults COMMAND operand-faults)
set_tests_properties(operand-faults PROPERTIES LABELS hermetic ENVIRONMENT "AVXEMU_DISABLE=1")

avxemu_test_exe(overread SOURCES overread.c LINK avxemu_core avxemu_asm)
add_test(NAME overread COMMAND $<TARGET_FILE:overread>)

avxemu_test_exe(reloctest SOURCES reloctest.c reloc_harness.S LINK avxemu_core avxemu_asm)
add_test(NAME reloctest COMMAND $<TARGET_FILE:reloctest>)
set_tests_properties(reloctest PROPERTIES ENVIRONMENT "AVXEMU_DISABLE=1;AVXEMU_FORCETRAMP=1")

avxemu_test_exe(nativetest SOURCES nativetest.c LINK avxemu_core avxemu_asm)
add_test(NAME nativetest COMMAND $<TARGET_FILE:nativetest>)
set_tests_properties(nativetest PROPERTIES ENVIRONMENT "AVXEMU_DISABLE=1")

# The live-register BMI thunks (AVXEMU_MINSPILL, on by default), each against bmi_exec, and
# which tier each setting selects. Hermetic: the emitted code is base-ISA only, and
# AVXEMU_DISABLE keeps the constructor out (the test sets up the pool itself).
avxemu_test_exe(minspilltest SOURCES minspilltest.c minspill_harness.S LINK avxemu_core avxemu_asm)
add_test(NAME minspilltest COMMAND $<TARGET_FILE:minspilltest> on)
add_test(NAME minspilltest-off COMMAND env AVXEMU_MINSPILL=0 $<TARGET_FILE:minspilltest> off)
set_tests_properties(minspilltest minspilltest-off PROPERTIES ENVIRONMENT "AVXEMU_DISABLE=1")

# Links avxemu into itself so the load-time passes act on its own cpuid, lzcnt and AVX2 run:
# the analysis cache records them, replays them identically, and refuses bad files.
avxemu_test_exe(cachetest SOURCES cachetest.c cachetest.S LINK avxemu_core avxemu_asm t_cpu)
add_test(NAME cachetest COMMAND ${AVXEMU_RUN} -- $<TARGET_FILE:cachetest>)

avxemu_objects(t_zcnt "${PROJECT_SOURCE_DIR}/src/decode.c" "${PROJECT_SOURCE_DIR}/src/exec_bmi.c" "${PROJECT_SOURCE_DIR}/src/names.c")
avxemu_test_exe(zcnt16 SOURCES zcnt16.c LINK t_zcnt)
add_test(NAME zcnt16 COMMAND $<TARGET_FILE:zcnt16>)

# Inserted into a program of our own, not /usr/bin/true: SIP strips DYLD_INSERT_LIBRARIES from
# system binaries on macOS newer than 10.9. The verdict is the self-test's own summary line, so a
# run where avxemu never loaded cannot pass.
avxemu_test_exe(selftest_host FLAGS -O0 SOURCES selftest_host.c)
add_test(NAME selftest COMMAND env AVXEMU_SELFTEST=1 sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh"
  $<TARGET_FILE:avxemu> $<TARGET_FILE:selftest_host>)
set_tests_properties(selftest PROPERTIES PASS_REGULAR_EXPRESSION "avxemu selftest: all shapes OK")

avxemu_test_exe(linkhook FLAGS -O0 SOURCES linkhook.c)
target_link_libraries(linkhook PRIVATE avxemu)
# AVXEMU_FORCEPATCH keeps avxemu installed on a CPU that has everything, where it would
# otherwise stay inert (inerttest) and there would be no rebind to test.
add_test(NAME linkhook COMMAND env AVXEMU_FORCEPATCH=1 $<TARGET_FILE:linkhook>)
if(APPLE)
  add_test(NAME linkhook-no-rebind COMMAND env AVXEMU_FORCEPATCH=1 AVXEMU_NO_REBIND=1 $<TARGET_FILE:linkhook>)
endif()

# Linked on a CPU that has everything, avxemu stays inert. The plain run checks the real early
# return on such a CPU, and on one without is the negative control: avxemu must be installed.
avxemu_test_exe(inerttest FLAGS -O0 SOURCES inerttest.c LINK t_cpu)
target_link_libraries(inerttest PRIVATE avxemu)
add_test(NAME inerttest COMMAND $<TARGET_FILE:inerttest>)
add_test(NAME inerttest-assume-capable COMMAND env AVXEMU_ASSUME_CAPABLE=1 $<TARGET_FILE:inerttest>)

# Linked against the dylib, like linkhook: the production configuration. N threads race each
# site's runtime relocation. It skips itself on a CPU with AVX2, where the loop never faults.
avxemu_test_exe(threadtest FLAGS -O2 -std=c11 SOURCES threadtest.c threadtest.S LINK t_cpu)
target_link_libraries(threadtest PRIVATE avxemu)
add_test(NAME threadtest COMMAND $<TARGET_FILE:threadtest> 20 12)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(lowcpu lowcpu.c lowcpu.S)
  target_compile_options(lowcpu PRIVATE ${AVXEMU_CORE_FLAGS} -Wextra -UNDEBUG)
  target_include_directories(lowcpu PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(lowcpu PRIVATE Threads::Threads)
  add_custom_command(TARGET lowcpu POST_BUILD COMMAND "${CMAKE_STRIP}" --strip-all
    -o "$<TARGET_FILE:lowcpu>-stripped" "$<TARGET_FILE:lowcpu>" VERBATIM)
  option(AVXEMU_TEST_LOWCPU "Require full-system QEMU reduced-CPU validation (see test/lowcpu.sh)" OFF)
  if(AVXEMU_TEST_LOWCPU)
    add_test(NAME lowcpu COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/lowcpu.sh" "${CMAKE_BINARY_DIR}")
    set_tests_properties(lowcpu PROPERTIES LABELS lowcpu TIMEOUT 900)
  endif()
endif()

avxemu_test_exe(overread_fault FLAGS -O0 SOURCES overread_fault.c)
add_test(NAME overread-fault COMMAND ${AVXEMU_RUN} -- env AVXEMU_FORCEPATCH=1 sh "${CMAKE_CURRENT_SOURCE_DIR}/preload.sh" $<TARGET_FILE:avxemu> $<TARGET_FILE:overread_fault>)
avxemu_test_exe(guard_page FLAGS -O0 SOURCES guard_page.c guard_operand.S)
add_test(NAME guard-page COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/guard-page.sh" $<TARGET_FILE:guard_page> $<TARGET_FILE:avxemu>)
set_tests_properties(guard-page PROPERTIES TIMEOUT 15)

set_tests_properties(asm-contract context-contract loader-contract image-contract scan-contract cache-ranges run-wrapper inject memtest tramptest overread reloctest nativetest
                     minspilltest minspilltest-off cachetest zcnt16 selftest linkhook
                     inerttest inerttest-assume-capable threadtest
                     overread-fault guard-page
  PROPERTIES LABELS hermetic SKIP_RETURN_CODE 77)

if(APPLE)
  set_tests_properties(image-live image-live-linked memory-contract linkhook-no-rebind
    PROPERTIES LABELS hermetic SKIP_RETURN_CODE 77)
endif()

avxemu_objects(t_refdigest refdigest.c cpu.c)
avxemu_test_exe(refdigest_test SOURCES refdigest_test.c LINK t_refdigest)
add_test(NAME refdigest COMMAND $<TARGET_FILE:refdigest_test>)
set_tests_properties(refdigest PROPERTIES LABELS hermetic SKIP_RETURN_CODE 77)

set(AVXEMU_REPLAY_PROGRAMS "")
function(avxemu_oracle name)
  cmake_parse_arguments(A "" "" "DRIVER;HW;ASM;LINK" ${ARGN})
  avxemu_objects(${name}_driver ${A_DRIVER})
  target_include_directories(${name}_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set(objs $<TARGET_OBJECTS:${name}_driver> $<TARGET_OBJECTS:t_refdigest>)
  if(A_HW)
    add_library(${name}_hw OBJECT ${A_HW})
    target_compile_options(${name}_hw PRIVATE ${AVXEMU_HOST_FLAGS})
    target_include_directories(${name}_hw PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND objs $<TARGET_OBJECTS:${name}_hw>)
  endif()
  foreach(l IN LISTS A_LINK)
    list(APPEND objs $<TARGET_OBJECTS:${l}>)
  endforeach()
  if("avxemu_core" IN_LIST A_LINK)
    list(APPEND objs ${AVXEMU_PLATFORM_OBJECTS})
  endif()
  add_executable(${name} ${A_ASM} ${objs})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
  set(AVXEMU_REPLAY_PROGRAMS ${AVXEMU_REPLAY_PROGRAMS} ${name} PARENT_SCOPE)
  add_test(NAME ${name}-replay COMMAND $<TARGET_FILE:${name}> check "${CMAKE_CURRENT_SOURCE_DIR}/reference/${name}.ref")
  add_test(NAME ${name}-record COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/record-matches.sh" $<TARGET_FILE:${name}>
           "${CMAKE_BINARY_DIR}/reference/${name}.ref" "${CMAKE_CURRENT_SOURCE_DIR}/reference/${name}.ref")
  set_tests_properties(${name}-replay PROPERTIES LABELS replay SKIP_RETURN_CODE 77)
  set_tests_properties(${name}-record PROPERTIES LABELS hardware SKIP_RETURN_CODE 77)
endfunction()

avxemu_objects(t_exec "${PROJECT_SOURCE_DIR}/src/exec.c" "${PROJECT_SOURCE_DIR}/src/exec_bmi.c"
                      "${PROJECT_SOURCE_DIR}/src/softfma.c" "${PROJECT_SOURCE_DIR}/src/names.c")
avxemu_oracle(oracle DRIVER oracle.c HW oracle_hw.c LINK t_exec)

avxemu_objects(t_bmi "${PROJECT_SOURCE_DIR}/src/exec_bmi.c" "${PROJECT_SOURCE_DIR}/src/names.c")
avxemu_oracle(bmi_oracle DRIVER bmi_oracle.c HW bmi_hw.c LINK t_bmi)

avxemu_oracle(patchtest DRIVER patchtest.c HW patchtest_hw.c ASM patch_stubs.S LINK avxemu_core avxemu_asm)

avxemu_oracle(bmimem DRIVER bmimem.c ASM bmimem.S LINK avxemu_core avxemu_asm)

if(APPLE)
avxemu_objects(t_decode "${PROJECT_SOURCE_DIR}/src/decode.c" "${PROJECT_SOURCE_DIR}/src/names.c")
avxemu_objects(t_lde "${PROJECT_SOURCE_DIR}/src/lde.c")
avxemu_test_exe(bintest SOURCES bintest.c LINK t_decode)
avxemu_test_exe(patchdiff SOURCES patchdiff.c LINK t_lde)
avxemu_test_exe(zdecode SOURCES zdecode.c LINK t_lde t_decode)
avxemu_test_exe(fuzz SOURCES fuzz.c fuzz.S LINK avxemu_core avxemu_asm)
add_test(NAME claude-decode COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/claude-decode.sh"
  $<TARGET_FILE:bintest> $<TARGET_FILE:patchdiff> $<TARGET_FILE:zdecode> "${CMAKE_BINARY_DIR}/claude-decode")
add_test(NAME claude-hardware COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/claude-hardware.sh"
  $<TARGET_FILE:cpuprobe> $<TARGET_FILE:fuzz> "${CMAKE_BINARY_DIR}/claude-hardware")
add_test(NAME claude-trampoline COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/claude-trampoline.sh"
  $<TARGET_FILE:cpuprobe> $<TARGET_FILE:avxemu> "${CMAKE_BINARY_DIR}/claude-trampoline")
set_tests_properties(claude-decode PROPERTIES LABELS claude-binary SKIP_RETURN_CODE 77)
set_tests_properties(claude-hardware claude-trampoline PROPERTIES LABELS "claude-binary;hardware" SKIP_RETURN_CODE 77)

endif()

set(_replay_objs $<TARGET_OBJECTS:t_refdigest>)
foreach(p IN LISTS AVXEMU_REPLAY_PROGRAMS)
  list(APPEND _replay_objs $<TARGET_OBJECTS:${p}_driver>)
endforeach()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/objects/replay.txt" CONTENT "$<JOIN:${_replay_objs},\n>\n")
add_test(NAME vex-leak-replay
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/vex-leak.sh"
          "${CMAKE_BINARY_DIR}/objects/replay.txt" "${CMAKE_BINARY_DIR}/objects/asm.txt" "${CMAKE_BINARY_DIR}/objects/negative.txt")
set_tests_properties(vex-leak-replay PROPERTIES LABELS hermetic SKIP_RETURN_CODE 77)

# Every test here loads avxemu, whose constructor would otherwise record its load-time analysis
# into ~/Library/Caches/avxemu (src/lcache.c). Only cachetest uses the cache, in a directory of
# its own; its own launch must not use one either.
get_property(_avxemu_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${_avxemu_tests} APPEND PROPERTY ENVIRONMENT AVXEMU_NOCACHE=1)
