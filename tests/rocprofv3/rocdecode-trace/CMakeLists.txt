#
#
#
cmake_minimum_required(VERSION 3.21.0 FATAL_ERROR)

project(
    rocprofiler-tests-rocprofv3-rocdecode-tracing
    LANGUAGES CXX
    VERSION 0.0.0)

find_package(rocprofiler-sdk REQUIRED)

rocprofiler_configure_pytest_files(CONFIG pytest.ini COPY validate.py conftest.py)

string(REPLACE "LD_PRELOAD=" "ROCPROF_PRELOAD=" PRELOAD_ENV
               "${ROCPROFILER_MEMCHECK_PRELOAD_ENV}")

set(rocdecode-tracing-env "${PRELOAD_ENV}")

set(ROCDECODE_VIDEO_FILE
    "${ROCM_PATH}/share/rocdecode/video/AMD_driving_virtual_20-H265.265")
if(NOT EXISTS "${ROCDECODE_VIDEO_FILE}")
    message(
        FATAL_ERROR
            "Unable to find video file for rocdecode tests: ${ROCDECODE_VIDEO_FILE}")
endif()
add_test(
    NAME rocprofv3-test-rocdecode-tracing-execute
    COMMAND
        $<TARGET_FILE:rocprofiler-sdk::rocprofv3> --rocdecode-trace -d
        ${CMAKE_CURRENT_BINARY_DIR}/%tag%-trace -o out --output-format json otf2 pftrace
        csv --log-level env -- $<TARGET_FILE:rocdecode> -i ${ROCDECODE_VIDEO_FILE})

set_tests_properties(
    rocprofv3-test-rocdecode-tracing-execute
    PROPERTIES TIMEOUT 45 LABELS "integration-tests" ENVIRONMENT
               "${rocdecode-tracing-env}" FAIL_REGULAR_EXPRESSION "threw an exception")

add_test(
    NAME rocprofv3-test-rocdecode-tracing-validate
    COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/validate.py --json-input
        ${CMAKE_CURRENT_BINARY_DIR}/rocdecode-trace/out_results.json --otf2-input
        ${CMAKE_CURRENT_BINARY_DIR}/rocdecode-trace/out_results.otf2 --pftrace-input
        ${CMAKE_CURRENT_BINARY_DIR}/rocdecode-trace/out_results.pftrace --csv-input
        ${CMAKE_CURRENT_BINARY_DIR}/rocdecode-trace/out_rocdecode_api_trace.csv)

set_tests_properties(
    rocprofv3-test-rocdecode-tracing-validate
    PROPERTIES TIMEOUT 45 LABELS "integration-tests" DEPENDS
               rocprofv3-test-rocdecode-tracing-execute FAIL_REGULAR_EXPRESSION
               "AssertionError")
