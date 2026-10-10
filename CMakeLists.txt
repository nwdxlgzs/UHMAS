cmake_minimum_required(VERSION 3.20)

project(UHMAS LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(uhmas_detect_address_sanitizer out_var)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _uhmas_build_type)
    set(_uhmas_flags
        "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_uhmas_build_type}} "
        "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${_uhmas_build_type}} "
        "${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS_${_uhmas_build_type}}"
    )
    if(_uhmas_flags MATCHES "(^|[ ,])[-/]fsanitize=([^ ]*,)?address(,| |$)" OR
       _uhmas_flags MATCHES "(^|[ ,])-fsanitize=([^ ]*,)?address(,| |$)")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(uhmas_detect_wsl out_var)
    set(_uhmas_is_wsl FALSE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(DEFINED ENV{WSL_INTEROP} OR DEFINED ENV{WSL_DISTRO_NAME})
            set(_uhmas_is_wsl TRUE)
        elseif(EXISTS "/proc/sys/kernel/osrelease")
            file(READ "/proc/sys/kernel/osrelease" _uhmas_osrelease LIMIT 256)
            string(TOLOWER "${_uhmas_osrelease}" _uhmas_osrelease_lower)
            if(_uhmas_osrelease_lower MATCHES "microsoft|wsl")
                set(_uhmas_is_wsl TRUE)
            endif()
        endif()
    endif()
    set(${out_var} ${_uhmas_is_wsl} PARENT_SCOPE)
endfunction()

option(UHMAS_ASAN_NO_PIE
       "Build Linux AddressSanitizer executables as non-PIE to avoid WSL ASAN shadow clashes"
       ON)

uhmas_detect_address_sanitizer(UHMAS_ADDRESS_SANITIZER_ENABLED)
uhmas_detect_wsl(UHMAS_RUNNING_ON_WSL)
if(UHMAS_ADDRESS_SANITIZER_ENABLED AND UHMAS_RUNNING_ON_WSL AND NOT UHMAS_ASAN_NO_PIE)
    message(WARNING
        "Forcing UHMAS_ASAN_NO_PIE=ON for WSL AddressSanitizer builds. "
        "PIE ASAN executables can collide with GCC ASAN shadow reservations on WSL "
        "and spin in AddressSanitizer:DEADLYSIGNAL until the outer command times out."
    )
    set(UHMAS_ASAN_NO_PIE ON CACHE BOOL
        "Build Linux AddressSanitizer executables as non-PIE to avoid WSL ASAN shadow clashes"
        FORCE)
endif()

set(UHMAS_USE_ASAN_NO_PIE OFF)
if(UHMAS_ASAN_NO_PIE AND UHMAS_ADDRESS_SANITIZER_ENABLED AND
   CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(UHMAS_USE_ASAN_NO_PIE ON)
endif()

option(UHMAS_EXPECT_32BIT "Fail configure unless this build targets a 32-bit pointer ABI" OFF)
if(UHMAS_EXPECT_32BIT AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "UHMAS_EXPECT_32BIT is ON, but CMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P}")
endif()

set(UHMAS_BUILD_PROFILE "dev" CACHE STRING
    "Build profile: dev, stress, or release")
set_property(CACHE UHMAS_BUILD_PROFILE PROPERTY STRINGS dev stress release)
if(NOT UHMAS_BUILD_PROFILE MATCHES "^(dev|stress|release)$")
    message(FATAL_ERROR
        "UHMAS_BUILD_PROFILE must be one of: dev, stress, release")
endif()

set(_uhmas_default_build_tests ON)
set(_uhmas_default_build_benchmarks ON)
set(_uhmas_default_test_hooks ON)
set(_uhmas_default_diagnostics ON)
if(UHMAS_BUILD_PROFILE STREQUAL "release")
    set(_uhmas_default_build_tests OFF)
    set(_uhmas_default_build_benchmarks OFF)
    set(_uhmas_default_test_hooks OFF)
    set(_uhmas_default_diagnostics OFF)
endif()

option(UHMAS_BUILD_TESTS "Build UHMAS tests" ${_uhmas_default_build_tests})
option(UHMAS_BUILD_BENCHMARKS "Build UHMAS benchmarks" ${_uhmas_default_build_benchmarks})
option(UHMAS_BUILD_EXAMPLES "Build the public API example" OFF)
if(NOT DEFINED UHMAS_ENABLE_TEST_HOOKS)
    set(UHMAS_ENABLE_TEST_HOOKS ${_uhmas_default_test_hooks} CACHE BOOL
        "Compile internal fault-injection and diagnostic test hooks")
endif()
if(UHMAS_BUILD_TESTS AND NOT UHMAS_ENABLE_TEST_HOOKS)
    message(FATAL_ERROR
        "UHMAS_BUILD_TESTS=ON requires UHMAS_ENABLE_TEST_HOOKS=ON because "
        "the white-box basic tests exercise internal fault-injection hooks. "
        "Use -DUHMAS_BUILD_TESTS=OFF for a no-hooks library build."
    )
endif()
if(NOT DEFINED UHMAS_ENABLE_DIAGNOSTICS)
    set(UHMAS_ENABLE_DIAGNOSTICS ${_uhmas_default_diagnostics} CACHE BOOL
        "Compile internal diagnostics counters and timing probes")
endif()
option(UHMAS_STRICT_SLOT_OWNER_GEOMETRY
       "Use strict slab slot geometry validation for remote-batch owner checks"
       OFF)
if(UHMAS_BUILD_TESTS AND NOT UHMAS_ENABLE_DIAGNOSTICS)
    message(FATAL_ERROR
        "UHMAS_BUILD_TESTS=ON requires UHMAS_ENABLE_DIAGNOSTICS=ON because "
        "the white-box tests validate internal diagnostics counters. "
        "Use -DUHMAS_BUILD_TESTS=OFF for a no-diagnostics release library build."
    )
endif()

set(UHMAS_UNITY_INCLUDE_FILES
    src/uhm_core_pages.inc
    src/uhm_core_base.inc
    src/uhm_core_vm_ops.inc
    src/uhm_core_control.inc
    src/uhm_core_metadata.inc
    src/uhm_core_page_map.inc
    src/uhm_slab_tls.inc
    src/uhm_slab_core.inc
    src/uhm_slab_remote.inc
    src/uhm_slab_pages.inc
    src/uhm_slab_alloc_free.inc
    src/uhm_slab_raw_batch.inc
    src/uhm_thread_tls_cache.inc
    src/uhm_malloc.inc
    src/uhm_malloc_trees.inc
    src/uhm_malloc_decommit.inc
    src/uhm_malloc_blocks.inc
    src/uhm_malloc_api.inc
    src/uhm_mmap_vm.inc
    src/uhm_mmap_free_vma.inc
    src/uhm_mmap_ops.inc
    src/uhm_munmap_mremap.inc
    src/uhm_mprotect.inc
    src/uhm_api_lifecycle.inc
    src/uhm_api_thread_ctx.inc
    src/uhm_api_check.inc
    src/uhm_api_stats.inc
    src/uhm_api_check_ctx.inc
    src/uhm_api_check_metadata.inc
    src/uhm_api_check_page_map.inc
    src/uhm_api_check_slab_helpers.inc
    src/uhm_api_check_heap.inc
    src/uhm_api_open_close.inc
)
set_source_files_properties(${UHMAS_UNITY_INCLUDE_FILES}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

set(UHMAS_BASIC_TEST_UNITY_INCLUDE_FILES
    tests/test_uhm_common.inc
    tests/test_uhm_common_defs.inc
    tests/test_uhm_common_helpers.inc
    tests/test_uhm_common_helpers_storage_abi.inc
    tests/test_uhm_common_helpers_layout_config.inc
    tests/test_uhm_common_helpers_slab_shadow.inc
    tests/test_uhm_common_helpers_threads.inc
    tests/test_uhm_common_workers.inc
    tests/test_uhm_common_vm_callbacks.inc
    tests/test_uhm_vm_metadata.inc
    tests/test_uhm_vm_metadata_open_tls_numa.inc
    tests/test_uhm_vm_metadata_faults.inc
    tests/test_uhm_vm_metadata_collision_control.inc
    tests/test_uhm_vm_metadata_quarantine.inc
    tests/test_uhm_slab_ctx.inc
    tests/test_uhm_slab_core.inc
    tests/test_uhm_slab_remote_accounted.inc
    tests/test_uhm_slab_remote_accounted_drain.inc
    tests/test_uhm_slab_remote_accounted_partial.inc
    tests/test_uhm_slab_remote_accounted_transitions.inc
    tests/test_uhm_slab_remote_drain.inc
    tests/test_uhm_slab_lockless_remote.inc
    tests/test_uhm_slab_lockless_remote_basic.inc
    tests/test_uhm_slab_raw_ready.inc
    tests/test_uhm_slab_raw_batch.inc
    tests/test_uhm_slab_lockless_remote_class_queue.inc
    tests/test_uhm_slab_lockless_remote_reader_gates.inc
    tests/test_uhm_slab_lockless_remote_rollback.inc
    tests/test_uhm_thread_tls_lifecycle.inc
    tests/test_uhm_malloc.inc
    tests/test_uhm_malloc_decommit.inc
    tests/test_uhm_malloc_retention.inc
    tests/test_uhm_malloc_lookup_tree.inc
    tests/test_uhm_vm_ranges_runner.inc
    tests/test_uhm_vm_ranges_core.inc
    tests/test_uhm_vm_ranges_core_threads.inc
    tests/test_uhm_vm_ranges_core_protect_decommit.inc
    tests/test_uhm_vm_ranges_core_tree_pressure.inc
    tests/test_uhm_vm_ranges_core_recent_reuse.inc
    tests/test_uhm_vm_ranges_diagnostics.inc
    tests/test_uhm_vm_ranges_mremap.inc
    tests/test_uhm_vm_ranges_mprotect.inc
    tests/test_uhm_vm_ranges_munmap.inc
    tests/test_uhm_vm_ranges_mprotect_rollback.inc
    tests/test_uhm_vm_ranges_main.inc
)
set_source_files_properties(${UHMAS_BASIC_TEST_UNITY_INCLUDE_FILES}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

set(UHMAS_BENCHMARK_UNITY_INCLUDE_FILES
    tests/bench_uhm_common.inc
    tests/bench_uhm_common_defs.inc
    tests/bench_uhm_common_utils.inc
    tests/bench_uhm_common_diag_capture.inc
    tests/bench_uhm_common_diag_print.inc
    tests/bench_uhm_env.inc
    tests/bench_uhm_core_cases.inc
    tests/bench_uhm_raw_remote.inc
    tests/bench_uhm_heap_close.inc
    tests/bench_uhm_heap_close_tls.inc
    tests/bench_uhm_heap_close_mixed.inc
    tests/bench_uhm_pressure_cases.inc
    tests/bench_uhm_main.inc
)
set_source_files_properties(${UHMAS_BENCHMARK_UNITY_INCLUDE_FILES}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

add_library(uhmas
    src/uhm.c
    ${UHMAS_UNITY_INCLUDE_FILES}
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(uhmas
        PRIVATE
            UHMAS_BUILD_SHARED
            UHMAS_BUILDING_LIBRARY
        INTERFACE
            UHMAS_BUILD_SHARED
    )
endif()

target_include_directories(uhmas
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(UHMAS_ENABLE_TEST_HOOKS)
    target_compile_definitions(uhmas PUBLIC UHMAS_ENABLE_TEST_HOOKS=1)
else()
    target_compile_definitions(uhmas PUBLIC UHMAS_ENABLE_TEST_HOOKS=0)
endif()
if(UHMAS_ENABLE_DIAGNOSTICS)
    target_compile_definitions(uhmas PUBLIC UHMAS_ENABLE_DIAGNOSTICS=1)
else()
    target_compile_definitions(uhmas PUBLIC UHMAS_ENABLE_DIAGNOSTICS=0)
endif()
if(UHMAS_STRICT_SLOT_OWNER_GEOMETRY)
    target_compile_definitions(uhmas PRIVATE UHMAS_STRICT_SLOT_OWNER_GEOMETRY=1)
else()
    target_compile_definitions(uhmas PRIVATE UHMAS_STRICT_SLOT_OWNER_GEOMETRY=0)
endif()

function(uhmas_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
    endif()
endfunction()

uhmas_target_warnings(uhmas)

if(UHMAS_USE_ASAN_NO_PIE AND NOT BUILD_SHARED_LIBS)
    target_compile_options(uhmas PRIVATE -fno-pie)
endif()

function(uhmas_target_runtime_quirks target)
    if(UHMAS_USE_ASAN_NO_PIE)
        target_compile_options(${target} PRIVATE -fno-pie)
        target_link_options(${target} PRIVATE -no-pie)
    endif()
endfunction()

function(uhmas_target_test_runtime_options target)
    if(UHMAS_ADDRESS_SANITIZER_ENABLED)
        target_sources(${target} PRIVATE
            tests/test_sanitizer_options.c
        )
        target_compile_definitions(${target} PRIVATE
            UHMAS_TEST_WITH_ADDRESS_SANITIZER
        )
    endif()
endfunction()

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(uhmas PUBLIC Threads::Threads)
endif()

find_program(UHMAS_POWERSHELL powershell pwsh)
if(UHMAS_POWERSHELL)
    add_custom_target(uhmas_verify_32bit
        COMMAND ${UHMAS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_32bit_gate.ps1
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Run Win32 and WSL i386 32-bit compatibility gate"
        VERBATIM
    )
endif()

if((UHMAS_BUILD_TESTS OR UHMAS_BUILD_BENCHMARKS) AND WIN32)
    find_package(Threads REQUIRED)
endif()

if(UHMAS_BUILD_TESTS)
    enable_testing()

    add_executable(uhmas_basic_tests
        tests/test_uhm.c
        ${UHMAS_BASIC_TEST_UNITY_INCLUDE_FILES}
    )
    target_include_directories(uhmas_basic_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(uhmas_basic_tests PRIVATE uhmas Threads::Threads)
    uhmas_target_warnings(uhmas_basic_tests)
    uhmas_target_runtime_quirks(uhmas_basic_tests)
    uhmas_target_test_runtime_options(uhmas_basic_tests)

    add_test(NAME uhmas_basic_tests COMMAND uhmas_basic_tests)

    add_executable(uhmas_platform_vm_tests
        tests/test_platform_vm.c
    )
    target_link_libraries(uhmas_platform_vm_tests PRIVATE uhmas)
    uhmas_target_warnings(uhmas_platform_vm_tests)
    uhmas_target_runtime_quirks(uhmas_platform_vm_tests)
    uhmas_target_test_runtime_options(uhmas_platform_vm_tests)

    add_test(NAME uhmas_platform_vm_tests COMMAND uhmas_platform_vm_tests)
endif()

if(UHMAS_BUILD_BENCHMARKS)
    add_executable(uhmas_benchmarks
        tests/bench_uhm.c
        ${UHMAS_BENCHMARK_UNITY_INCLUDE_FILES}
    )
    target_include_directories(uhmas_benchmarks PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(uhmas_benchmarks PRIVATE uhmas Threads::Threads)
    uhmas_target_warnings(uhmas_benchmarks)
    uhmas_target_runtime_quirks(uhmas_benchmarks)
    uhmas_target_test_runtime_options(uhmas_benchmarks)
endif()

if(UHMAS_BUILD_EXAMPLES)
    add_executable(uhmas_example examples/basic.c)
    target_link_libraries(uhmas_example PRIVATE uhmas)
    uhmas_target_warnings(uhmas_example)
    uhmas_target_runtime_quirks(uhmas_example)
endif()

if(UHMAS_BUILD_TESTS AND UHMAS_BUILD_BENCHMARKS)
    add_custom_target(uhmas_verify_stress
        COMMAND $<TARGET_FILE:uhmas_basic_tests>
        COMMAND $<TARGET_FILE:uhmas_platform_vm_tests>
        COMMAND ${CMAKE_COMMAND} -E env UHMAS_BENCH_STRESS=1
                $<TARGET_FILE:uhmas_benchmarks>
        DEPENDS uhmas_basic_tests uhmas_platform_vm_tests uhmas_benchmarks
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Run UHMAS basic/platform tests and stress benchmarks"
        VERBATIM
    )
endif()
