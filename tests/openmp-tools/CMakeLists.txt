#
#
#
cmake_minimum_required(VERSION 3.21.0 FATAL_ERROR)

project(
    rocprofiler-tests-openmp-tools
    LANGUAGES CXX
    VERSION 0.0.0)

find_package(rocprofiler-sdk REQUIRED)

set(PYTEST_ARGS)
if(ROCPROFILER_MEMCHECK MATCHES "(Address|Thread)Sanitizer" OR ROCPROFILER_BUILD_CODECOV)
    set(PYTEST_ARGS -k "not test_total_runtime")
endif()

if(ROCPROFILER_MEMCHECK_PRELOAD_ENV)
    set(PRELOAD_ENV
        "${ROCPROFILER_MEMCHECK_PRELOAD_ENV}:$<TARGET_FILE:rocprofiler-sdk-json-tool>")
else()
    set(PRELOAD_ENV "LD_PRELOAD=$<TARGET_FILE:rocprofiler-sdk-json-tool>")
endif()

if(ROCPROFILER_MEMCHECK STREQUAL "ThreadSanitizer")
    set(IS_THREAD_SANITIZER ON)
else()
    set(IS_THREAD_SANITIZER OFF)
endif()

add_test(NAME test-openmp-tools-execute COMMAND $<TARGET_FILE:openmp-target>)

set(openmp-tools-env
    "${PRELOAD_ENV}"
    "OMP_NUM_THREADS=2"
    "OMP_DISPLAY_ENV=1"
    "OMP_TARGET_OFFLOAD=mandatory"
    "ROCR_VISIBLE_DEVICES=0"
    "ROCPROFILER_TOOL_OUTPUT_FILE=openmp-tools-test.json"
    "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:rocprofiler-sdk::rocprofiler-sdk-shared-library>:$ENV{LD_LIBRARY_PATH}"
    )

set_tests_properties(
    test-openmp-tools-execute
    PROPERTIES TIMEOUT
               100
               LABELS
               "integration-tests;openmp-target"
               ENVIRONMENT
               "${openmp-tools-env}"
               FAIL_REGULAR_EXPRESSION
               "${ROCPROFILER_DEFAULT_FAIL_REGEX}"
               DISABLED
               "${IS_THREAD_SANITIZER}")

# copy to binary directory
rocprofiler_configure_pytest_files(COPY validate.py conftest.py CONFIG pytest.ini)

add_test(
    NAME test-openmp-tools-validate
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/validate.py ${PYTEST_ARGS}
            --input ${CMAKE_CURRENT_BINARY_DIR}/openmp-tools-test.json)

set_tests_properties(
    test-openmp-tools-validate
    PROPERTIES TIMEOUT
               45
               LABELS
               "integration-tests;openmp-target"
               DEPENDS
               test-openmp-tools-execute
               FAIL_REGULAR_EXPRESSION
               "${ROCPROFILER_DEFAULT_FAIL_REGEX}"
               DISABLED
               "${IS_THREAD_SANITIZER}")
