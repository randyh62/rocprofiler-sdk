#
#
#   Installation of public headers
#
#
configure_file(${CMAKE_CURRENT_LIST_DIR}/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

set(ROCPROFILER_HEADER_FILES
    # core headers
    rocprofiler.h
    # secondary headers
    agent.h
    device_counting_service.h
    buffer.h
    buffer_tracing.h
    callback_tracing.h
    context.h
    counters.h
    defines.h
    dispatch_counting_service.h
    external_correlation.h
    fwd.h
    hip.h
    hsa.h
    intercept_table.h
    internal_threading.h
    marker.h
    ompt.h
    pc_sampling.h
    profile_config.h
    registration.h
    rccl.h
    rocdecode.h
    spm.h
    ${CMAKE_CURRENT_BINARY_DIR}/version.h)

install(
    FILES ${ROCPROFILER_HEADER_FILES}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rocprofiler-sdk
    COMPONENT development)

add_subdirectory(hip)
add_subdirectory(hsa)
add_subdirectory(marker)
add_subdirectory(ompt)
add_subdirectory(rccl)
add_subdirectory(rocdecode)
add_subdirectory(cxx)
add_subdirectory(kfd)
add_subdirectory(amd_detail)
add_subdirectory(experimental)
