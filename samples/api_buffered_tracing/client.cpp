// MIT License
//
// Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// undefine NDEBUG so asserts are implemented
#ifdef NDEBUG
#    undef NDEBUG
#endif

/**
 * @file samples/api_buffered_tracing/client.cpp
 *
 * @brief Example rocprofiler client (tool)
 */

#include "client.hpp"

#include <rocprofiler-sdk/buffer.h>
#include <rocprofiler-sdk/buffer_tracing.h>
#include <rocprofiler-sdk/callback_tracing.h>
#include <rocprofiler-sdk/external_correlation.h>
#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/internal_threading.h>
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/rocprofiler.h>

#include "common/call_stack.hpp"
#include "common/defines.hpp"
#include "common/filesystem.hpp"
#include "common/name_info.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace client
{
namespace
{
using common::buffer_name_info;
using common::call_stack_t;
using common::source_location;

using kernel_symbol_data_t = rocprofiler_callback_tracing_code_object_kernel_symbol_register_data_t;
using kernel_symbol_map_t  = std::unordered_map<rocprofiler_kernel_id_t, kernel_symbol_data_t>;

rocprofiler_client_id_t*      client_id        = nullptr;
rocprofiler_client_finalize_t client_fini_func = nullptr;
rocprofiler_context_id_t      client_ctx       = {0};
rocprofiler_buffer_id_t       client_buffer    = {};
buffer_name_info              client_name_info = {};
kernel_symbol_map_t           client_kernels   = {};

template <typename Tp>
std::string
as_hex(Tp _v, size_t _width = 16)
{
    uintptr_t _vp = 0;
    if constexpr(std::is_pointer<Tp>::value)
        _vp = reinterpret_cast<uintptr_t>(_v);
    else
        _vp = _v;

    auto _ss = std::stringstream{};
    _ss.fill('0');
    _ss << "0x" << std::hex << std::setw(_width) << _vp;
    return _ss.str();
}

void
print_call_stack(const call_stack_t& _call_stack)
{
    common::print_call_stack("api_buffered_trace.log", _call_stack);
}

void
tool_code_object_callback(rocprofiler_callback_tracing_record_t record,
                          rocprofiler_user_data_t*              user_data,
                          void*                                 callback_data)
{
    if(record.kind == ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT &&
       record.operation == ROCPROFILER_CODE_OBJECT_LOAD)
    {
        if(record.phase == ROCPROFILER_CALLBACK_PHASE_UNLOAD)
        {
            // flush the buffer to ensure that any lookups for the client kernel names for the code
            // object are completed
            auto flush_status = rocprofiler_flush_buffer(client_buffer);
            if(flush_status != ROCPROFILER_STATUS_ERROR_BUFFER_BUSY)
                ROCPROFILER_CALL(flush_status, "buffer flush");
        }
    }
    else if(record.kind == ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT &&
            record.operation == ROCPROFILER_CODE_OBJECT_DEVICE_KERNEL_SYMBOL_REGISTER)
    {
        auto* data = static_cast<kernel_symbol_data_t*>(record.payload);
        if(record.phase == ROCPROFILER_CALLBACK_PHASE_LOAD)
        {
            client_kernels.emplace(data->kernel_id, *data);
        }
        else if(record.phase == ROCPROFILER_CALLBACK_PHASE_UNLOAD)
        {
            client_kernels.erase(data->kernel_id);
        }
    }

    (void) user_data;
    (void) callback_data;
}

void
tool_tracing_callback(rocprofiler_context_id_t      context,
                      rocprofiler_buffer_id_t       buffer_id,
                      rocprofiler_record_header_t** headers,
                      size_t                        num_headers,
                      void*                         user_data,
                      uint64_t                      drop_count)
{
    assert(user_data != nullptr);
    assert(drop_count == 0 && "drop count should be zero for lossless policy");

    if(num_headers == 0)
        throw std::runtime_error{
            "rocprofiler invoked a buffer callback with no headers. this should never happen"};
    else if(headers == nullptr)
        throw std::runtime_error{"rocprofiler invoked a buffer callback with a null pointer to the "
                                 "array of headers. this should never happen"};

    for(size_t i = 0; i < num_headers; ++i)
    {
        auto* header = headers[i];

        auto kind_name = std::string{};
        if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING)
        {
            const char* _name = nullptr;
            auto        _kind = static_cast<rocprofiler_buffer_tracing_kind_t>(header->kind);
            ROCPROFILER_CALL(rocprofiler_query_buffer_tracing_kind_name(_kind, &_name, nullptr),
                             "query buffer tracing kind name");
            if(_name)
            {
                static size_t len = 15;

                kind_name = std::string{_name};
                len       = std::max(len, kind_name.length());
                kind_name.resize(len, ' ');
                kind_name += " :: ";
            }
        }

        if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING &&
           (header->kind == ROCPROFILER_BUFFER_TRACING_HSA_CORE_API ||
            header->kind == ROCPROFILER_BUFFER_TRACING_HSA_AMD_EXT_API ||
            header->kind == ROCPROFILER_BUFFER_TRACING_HSA_IMAGE_EXT_API ||
            header->kind == ROCPROFILER_BUFFER_TRACING_HSA_FINALIZE_EXT_API))
        {
            auto* record =
                static_cast<rocprofiler_buffer_tracing_hsa_api_record_t*>(header->payload);
            auto info = std::stringstream{};
            info << "tid=" << record->thread_id << ", context=" << context.handle
                 << ", buffer_id=" << buffer_id.handle
                 << ", cid=" << record->correlation_id.internal
                 << ", extern_cid=" << record->correlation_id.external.value
                 << ", kind=" << record->kind << ", operation=" << record->operation
                 << ", start=" << record->start_timestamp << ", stop=" << record->end_timestamp
                 << ", name=" << client_name_info.at(record->kind, record->operation);

            if(record->start_timestamp > record->end_timestamp)
            {
                auto msg = std::stringstream{};
                msg << "hsa api: start > end (" << record->start_timestamp << " > "
                    << record->end_timestamp
                    << "). diff = " << (record->start_timestamp - record->end_timestamp);
                std::cerr << "threw an exception " << msg.str() << "\n" << std::flush;
                // throw std::runtime_error{msg.str()};
            }

            static_cast<call_stack_t*>(user_data)->emplace_back(
                source_location{__FUNCTION__, __FILE__, __LINE__, kind_name + info.str()});
        }
        else if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING &&
                header->kind == ROCPROFILER_BUFFER_TRACING_HIP_RUNTIME_API)
        {
            auto* record =
                static_cast<rocprofiler_buffer_tracing_hip_api_record_t*>(header->payload);
            auto info = std::stringstream{};
            info << "tid=" << record->thread_id << ", context=" << context.handle
                 << ", buffer_id=" << buffer_id.handle
                 << ", cid=" << record->correlation_id.internal
                 << ", extern_cid=" << record->correlation_id.external.value
                 << ", kind=" << record->kind << ", operation=" << record->operation
                 << ", start=" << record->start_timestamp << ", stop=" << record->end_timestamp
                 << ", name=" << client_name_info[record->kind][record->operation];

            if(record->start_timestamp > record->end_timestamp)
            {
                auto msg = std::stringstream{};
                msg << "hip api: start > end (" << record->start_timestamp << " > "
                    << record->end_timestamp
                    << "). diff = " << (record->start_timestamp - record->end_timestamp);
                std::cerr << "threw an exception " << msg.str() << "\n" << std::flush;
                // throw std::runtime_error{msg.str()};
            }

            static_cast<call_stack_t*>(user_data)->emplace_back(
                source_location{__FUNCTION__, __FILE__, __LINE__, kind_name + info.str()});
        }
        else if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING &&
                header->kind == ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH)
        {
            auto* record =
                static_cast<rocprofiler_buffer_tracing_kernel_dispatch_record_t*>(header->payload);

            auto info = std::stringstream{};

            info << "tid=" << record->thread_id << ", context=" << context.handle
                 << ", buffer_id=" << buffer_id.handle
                 << ", cid=" << record->correlation_id.internal
                 << ", extern_cid=" << record->correlation_id.external.value
                 << ", kind=" << record->kind << ", operation=" << record->operation
                 << ", agent_id=" << record->dispatch_info.agent_id.handle
                 << ", queue_id=" << record->dispatch_info.queue_id.handle
                 << ", kernel_id=" << record->dispatch_info.kernel_id
                 << ", kernel=" << client_kernels.at(record->dispatch_info.kernel_id).kernel_name
                 << ", start=" << record->start_timestamp << ", stop=" << record->end_timestamp
                 << ", private_segment_size=" << record->dispatch_info.private_segment_size
                 << ", group_segment_size=" << record->dispatch_info.group_segment_size
                 << ", workgroup_size=(" << record->dispatch_info.workgroup_size.x << ","
                 << record->dispatch_info.workgroup_size.y << ","
                 << record->dispatch_info.workgroup_size.z << "), grid_size=("
                 << record->dispatch_info.grid_size.x << "," << record->dispatch_info.grid_size.y
                 << "," << record->dispatch_info.grid_size.z << ")";

            if(record->start_timestamp > record->end_timestamp)
                throw std::runtime_error("kernel dispatch: start > end");

            static_cast<call_stack_t*>(user_data)->emplace_back(
                source_location{__FUNCTION__, __FILE__, __LINE__, kind_name + info.str()});
        }
        else if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING &&
                header->kind == ROCPROFILER_BUFFER_TRACING_MEMORY_COPY)
        {
            auto* record =
                static_cast<rocprofiler_buffer_tracing_memory_copy_record_t*>(header->payload);

            auto info = std::stringstream{};

            info << "tid=" << record->thread_id << ", context=" << context.handle
                 << ", buffer_id=" << buffer_id.handle
                 << ", cid=" << record->correlation_id.internal
                 << ", extern_cid=" << record->correlation_id.external.value
                 << ", kind=" << record->kind << ", operation=" << record->operation
                 << ", src_agent_id=" << record->src_agent_id.handle
                 << ", dst_agent_id=" << record->dst_agent_id.handle
                 << ", direction=" << record->operation << ", start=" << record->start_timestamp
                 << ", stop=" << record->end_timestamp
                 << ", name=" << client_name_info.at(record->kind, record->operation);

            if(record->start_timestamp > record->end_timestamp)
                throw std::runtime_error("memory copy: start > end");

            static_cast<call_stack_t*>(user_data)->emplace_back(
                source_location{__FUNCTION__, __FILE__, __LINE__, kind_name + info.str()});
        }
        else if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING &&
                header->kind == ROCPROFILER_BUFFER_TRACING_PAGE_MIGRATION)
        {
            auto* record =
                static_cast<rocprofiler_buffer_tracing_page_migration_record_t*>(header->payload);

            auto info = std::stringstream{};

            info << "kind=" << record->kind << ", operation=" << record->operation
                 << ", pid=" << record->pid << ", timestamp=" << record->timestamp
                 << ", name=" << client_name_info.at(record->kind, record->operation)
                 << std::boolalpha;

            switch(record->operation)
            {
                case ROCPROFILER_PAGE_MIGRATION_PAGE_MIGRATE_START:
                {
                    const auto& arg = record->args;
                    info << ", page_migrate_start=(" << as_hex(arg.page_migrate_start.start_addr)
                         << ", " << as_hex(arg.page_migrate_start.end_addr) << ", "
                         << arg.page_migrate_start.from_agent.handle << ", "
                         << arg.page_migrate_start.to_agent.handle << ", "
                         << arg.page_migrate_start.prefetch_agent.handle << ", "
                         << arg.page_migrate_start.preferred_agent.handle << ", "
                         << arg.page_migrate_start.trigger << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_PAGE_MIGRATE_END:
                {
                    const auto& arg = record->args;
                    info << ", page_migrate_end=(" << as_hex(arg.page_migrate_end.start_addr)
                         << ", " << as_hex(arg.page_migrate_end.end_addr) << ", "
                         << arg.page_migrate_end.from_agent.handle << ", "
                         << arg.page_migrate_end.to_agent.handle << ", "
                         << arg.page_migrate_end.trigger << ", " << arg.page_migrate_end.error_code
                         << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_PAGE_FAULT_START:
                {
                    const auto& arg = record->args;
                    info << ", page_fault_start=(" << arg.page_fault_start.read_fault << ", "
                         << arg.page_fault_start.agent_id.handle << ", "
                         << as_hex(arg.page_fault_start.address) << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_PAGE_FAULT_END:
                {
                    const auto& arg = record->args;
                    info << ", page_fault_end=(" << arg.page_fault_end.migrated << ", "
                         << arg.page_fault_end.agent_id.handle << ", "
                         << as_hex(arg.page_fault_end.address) << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_QUEUE_EVICTION:
                {
                    const auto& arg = record->args;
                    info << ", queue_eviction=(" << arg.queue_eviction.agent_id.handle << ", "
                         << arg.queue_eviction.trigger << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_QUEUE_RESTORE:
                {
                    const auto& arg = record->args;
                    info << ", queue_restore=(" << arg.queue_restore.rescheduled << ", "
                         << arg.queue_restore.agent_id.handle << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_UNMAP_FROM_GPU:
                {
                    const auto& arg = record->args;
                    info << ", unmap_from_gpu=(" << as_hex(arg.unmap_from_gpu.start_addr) << ", "
                         << as_hex(arg.unmap_from_gpu.end_addr) << ", "
                         << arg.unmap_from_gpu.agent_id.handle << ", " << arg.unmap_from_gpu.trigger
                         << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_DROPPED_EVENT:
                {
                    const auto& arg = record->args;
                    info << ", dropped_event=(" << arg.dropped_event.dropped_events_count << ")";
                    break;
                }
                case ROCPROFILER_PAGE_MIGRATION_NONE:
                case ROCPROFILER_PAGE_MIGRATION_LAST:
                {
                    throw std::runtime_error{"unexpected page migration value"};
                    break;
                }
            }

            if(record->timestamp == 0) throw std::runtime_error("page migration: timestamp == 0");

            static_cast<call_stack_t*>(user_data)->emplace_back(
                source_location{__FUNCTION__, __FILE__, __LINE__, kind_name + info.str()});
        }
        else if(header->category == ROCPROFILER_BUFFER_CATEGORY_TRACING &&
                header->kind == ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY)
        {
            auto* record =
                static_cast<rocprofiler_buffer_tracing_scratch_memory_record_t*>(header->payload);

            auto info = std::stringstream{};

            auto _elapsed =
                std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                    std::chrono::nanoseconds{record->end_timestamp - record->start_timestamp})
                    .count();

            info << "tid=" << record->thread_id << ", context=" << context.handle
                 << ", buffer_id=" << buffer_id.handle
                 << ", cid=" << record->correlation_id.internal
                 << ", extern_cid=" << record->correlation_id.external.value
                 << ", kind=" << record->kind << ", operation=" << record->operation
                 << ", agent_id=" << record->agent_id.handle
                 << ", queue_id=" << record->queue_id.handle << ", thread_id=" << record->thread_id
                 << ", elapsed=" << std::setprecision(3) << std::fixed << _elapsed
                 << " usec, flags=" << record->flags
                 << ", name=" << client_name_info.at(record->kind, record->operation);

            static_cast<call_stack_t*>(user_data)->emplace_back(
                source_location{__FUNCTION__, __FILE__, __LINE__, kind_name + info.str()});
        }
        else
        {
            auto _msg = std::stringstream{};
            _msg << "unexpected rocprofiler_record_header_t category + kind: (" << header->category
                 << " + " << header->kind << ")";
            throw std::runtime_error{_msg.str()};
        }
    }
}

void
thread_precreate(rocprofiler_runtime_library_t lib, void* tool_data)
{
    static_cast<call_stack_t*>(tool_data)->emplace_back(
        source_location{__FUNCTION__,
                        __FILE__,
                        __LINE__,
                        std::string{"internal thread about to be created by rocprofiler (lib="} +
                            std::to_string(lib) + ")"});
}

void
thread_postcreate(rocprofiler_runtime_library_t lib, void* tool_data)
{
    static_cast<call_stack_t*>(tool_data)->emplace_back(
        source_location{__FUNCTION__,
                        __FILE__,
                        __LINE__,
                        std::string{"internal thread was created by rocprofiler (lib="} +
                            std::to_string(lib) + ")"});
}

int
tool_init(rocprofiler_client_finalize_t fini_func, void* tool_data)
{
    assert(tool_data != nullptr);

    auto* call_stack_v = static_cast<call_stack_t*>(tool_data);

    call_stack_v->emplace_back(source_location{__FUNCTION__, __FILE__, __LINE__, ""});

    client_name_info = common::get_buffer_tracing_names();

    for(const auto& itr : client_name_info)
    {
        auto name_idx = std::stringstream{};
        name_idx << " [" << std::setw(3) << itr.value << "]";
        call_stack_v->emplace_back(
            source_location{"rocprofiler_buffer_tracing_kind_names          " + name_idx.str(),
                            __FILE__,
                            __LINE__,
                            std::string{itr.name}});

        for(auto [didx, ditr] : itr.items())
        {
            auto operation_idx = std::stringstream{};
            operation_idx << " [" << std::setw(3) << didx << "]";
            call_stack_v->emplace_back(source_location{
                "rocprofiler_buffer_tracing_kind_operation_names" + operation_idx.str(),
                __FILE__,
                __LINE__,
                std::string{"- "} + std::string{*ditr}});
        }
    }

    client_fini_func = fini_func;

    ROCPROFILER_CALL(rocprofiler_create_context(&client_ctx), "context creation");

    auto code_object_ops = std::vector<rocprofiler_tracing_operation_t>{
        ROCPROFILER_CODE_OBJECT_DEVICE_KERNEL_SYMBOL_REGISTER};

    ROCPROFILER_CALL(
        rocprofiler_configure_callback_tracing_service(client_ctx,
                                                       ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT,
                                                       code_object_ops.data(),
                                                       code_object_ops.size(),
                                                       tool_code_object_callback,
                                                       nullptr),
        "code object tracing service configure");

    constexpr auto buffer_size_bytes      = 4096;
    constexpr auto buffer_watermark_bytes = buffer_size_bytes - (buffer_size_bytes / 8);

    ROCPROFILER_CALL(rocprofiler_create_buffer(client_ctx,
                                               buffer_size_bytes,
                                               buffer_watermark_bytes,
                                               ROCPROFILER_BUFFER_POLICY_LOSSLESS,
                                               tool_tracing_callback,
                                               tool_data,
                                               &client_buffer),
                     "buffer creation");

    for(auto itr :
        {ROCPROFILER_BUFFER_TRACING_HSA_CORE_API, ROCPROFILER_BUFFER_TRACING_HSA_AMD_EXT_API})
    {
        ROCPROFILER_CALL(rocprofiler_configure_buffer_tracing_service(
                             client_ctx, itr, nullptr, 0, client_buffer),
                         "buffer tracing service configure");
    }

    ROCPROFILER_CALL(
        rocprofiler_configure_buffer_tracing_service(
            client_ctx, ROCPROFILER_BUFFER_TRACING_HIP_RUNTIME_API, nullptr, 0, client_buffer),
        "buffer tracing service configure");

    ROCPROFILER_CALL(
        rocprofiler_configure_buffer_tracing_service(
            client_ctx, ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH, nullptr, 0, client_buffer),
        "buffer tracing service for kernel dispatch configure");

    ROCPROFILER_CALL(
        rocprofiler_configure_buffer_tracing_service(
            client_ctx, ROCPROFILER_BUFFER_TRACING_MEMORY_COPY, nullptr, 0, client_buffer),
        "buffer tracing service for memory copy configure");

    // May have incompatible kernel so only emit a warning here
    ROCPROFILER_WARN(rocprofiler_configure_buffer_tracing_service(
        client_ctx, ROCPROFILER_BUFFER_TRACING_PAGE_MIGRATION, nullptr, 0, client_buffer));

    ROCPROFILER_CALL(
        rocprofiler_configure_buffer_tracing_service(
            client_ctx, ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY, nullptr, 0, client_buffer),
        "buffer tracing service for page migration configure");

    auto client_thread = rocprofiler_callback_thread_t{};
    ROCPROFILER_CALL(rocprofiler_create_callback_thread(&client_thread),
                     "creating callback thread");

    ROCPROFILER_CALL(rocprofiler_assign_callback_thread(client_buffer, client_thread),
                     "assignment of thread for buffer");

    int valid_ctx = 0;
    ROCPROFILER_CALL(rocprofiler_context_is_valid(client_ctx, &valid_ctx),
                     "context validity check");
    if(valid_ctx == 0)
    {
        // notify rocprofiler that initialization failed
        // and all the contexts, buffers, etc. created
        // should be ignored
        return -1;
    }

    ROCPROFILER_CALL(rocprofiler_start_context(client_ctx), "rocprofiler context start");

    // no errors
    return 0;
}

void
tool_fini(void* tool_data)
{
    assert(tool_data != nullptr);

    auto* _call_stack = static_cast<call_stack_t*>(tool_data);
    _call_stack->emplace_back(source_location{__FUNCTION__, __FILE__, __LINE__, ""});

    print_call_stack(*_call_stack);

    delete _call_stack;
}
}  // namespace

void
setup()
{
    if(int status = 0;
       rocprofiler_is_initialized(&status) == ROCPROFILER_STATUS_SUCCESS && status == 0)
    {
        ROCPROFILER_CALL(rocprofiler_force_configure(&rocprofiler_configure),
                         "force configuration");
    }
}

void
shutdown()
{
    if(client_id)
    {
        ROCPROFILER_CALL(rocprofiler_flush_buffer(client_buffer), "buffer flush");
        client_fini_func(*client_id);
    }
}

void
start()
{
    ROCPROFILER_CALL(rocprofiler_start_context(client_ctx), "context start");
}

void
identify(uint64_t val)
{
    auto _tid = rocprofiler_thread_id_t{};
    rocprofiler_get_thread_id(&_tid);
    rocprofiler_user_data_t user_data = {};
    user_data.value                   = val;
    rocprofiler_push_external_correlation_id(client_ctx, _tid, user_data);
}

void
stop()
{
    ROCPROFILER_CALL(rocprofiler_stop_context(client_ctx), "context stop");
}
}  // namespace client

extern "C" rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t                 version,
                      const char*              runtime_version,
                      uint32_t                 priority,
                      rocprofiler_client_id_t* id)
{
    // set the client name
    id->name = "ExampleTool";

    // store client info
    client::client_id = id;

    // compute major/minor/patch version info
    uint32_t major = version / 10000;
    uint32_t minor = (version % 10000) / 100;
    uint32_t patch = version % 100;

    // generate info string
    auto info = std::stringstream{};
    info << id->name << " (priority=" << priority << ") is using rocprofiler-sdk v" << major << "."
         << minor << "." << patch << " (" << runtime_version << ")";

    std::clog << info.str() << std::endl;

    auto* client_tool_data = new std::vector<client::source_location>{};

    client_tool_data->emplace_back(
        client::source_location{__FUNCTION__, __FILE__, __LINE__, info.str()});

    ROCPROFILER_CALL(rocprofiler_at_internal_thread_create(
                         client::thread_precreate,
                         client::thread_postcreate,
                         ROCPROFILER_LIBRARY | ROCPROFILER_HSA_LIBRARY | ROCPROFILER_HIP_LIBRARY |
                             ROCPROFILER_MARKER_LIBRARY,
                         static_cast<void*>(client_tool_data)),
                     "registration for thread creation notifications");

    // create configure data
    static auto cfg =
        rocprofiler_tool_configure_result_t{sizeof(rocprofiler_tool_configure_result_t),
                                            &client::tool_init,
                                            &client::tool_fini,
                                            static_cast<void*>(client_tool_data)};

    // return pointer to configure data
    return &cfg;
}
