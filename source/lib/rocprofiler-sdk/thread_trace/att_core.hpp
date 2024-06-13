// MIT License
//
// Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.
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

#pragma once

#include <rocprofiler-sdk/cxx/hash.hpp>
#include <rocprofiler-sdk/cxx/operators.hpp>
#include "lib/rocprofiler-sdk/hsa/agent_cache.hpp"

#include <rocprofiler-sdk/amd_detail/thread_trace.h>
#include <rocprofiler-sdk/intercept_table.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace rocprofiler
{
namespace hsa
{
class AQLPacket;
};

namespace thread_trace
{
struct thread_trace_parameter_pack
{
    rocprofiler_context_id_t               context_id{0};
    rocprofiler_att_dispatch_callback_t    dispatch_cb_fn{nullptr};
    rocprofiler_att_shader_data_callback_t shader_cb_fn{nullptr};
    void*                                  callback_userdata{nullptr};

    // Parameters
    uint8_t  target_cu          = 1;
    uint8_t  simd_select        = DEFAULT_SIMD;
    uint8_t  perfcounter_ctrl   = 0;
    uint64_t shader_engine_mask = DEFAULT_SE_MASK;
    uint64_t buffer_size        = DEFAULT_BUFFER_SIZE;

    // GFX9 Only
    std::vector<uint32_t> perfcounters;

    static constexpr size_t DEFAULT_SIMD                  = 0x7;
    static constexpr size_t DEFAULT_PERFCOUNTER_SIMD_MASK = 0xF;
    static constexpr size_t DEFAULT_SE_MASK               = 0x21;
    static constexpr size_t DEFAULT_BUFFER_SIZE           = 0x8000000;
    static constexpr size_t PERFCOUNTER_SIMD_MASK_SHIFT   = 28;
};

class ThreadTracerQueue
{
    using code_object_id_t = uint64_t;

public:
    ThreadTracerQueue(thread_trace_parameter_pack _params,
                      const hsa::AgentCache&,
                      const CoreApiTable&,
                      const AmdExtTable&);
    virtual ~ThreadTracerQueue();

    void load_codeobj(code_object_id_t id, uint64_t addr, uint64_t size);
    void unload_codeobj(code_object_id_t id);

    std::unique_ptr<hsa::TraceControlAQLPacket> get_control(bool bStart);
    void iterate_data(aqlprofile_handle_t handle, rocprofiler_user_data_t data);

    hsa_queue_t*                queue = nullptr;
    std::mutex                  trace_resources_mut;
    thread_trace_parameter_pack params;
    std::atomic<int>            active_traces{0};
    std::atomic<int>            active_queues{1};

    std::unique_ptr<hsa::TraceControlAQLPacket>       control_packet;
    std::unique_ptr<aql::ThreadTraceAQLPacketFactory> factory;

    bool Submit(hsa_ext_amd_aql_pm4_packet_t* packet);

private:
    decltype(hsa_queue_load_read_index_relaxed)* load_read_index_relaxed_fn{nullptr};
    decltype(hsa_queue_add_write_index_relaxed)* add_write_index_relaxed_fn{nullptr};
    decltype(hsa_signal_store_screlease)*        signal_store_screlease_fn{nullptr};
    decltype(hsa_queue_destroy)*                 queue_destroy_fn{nullptr};
};

class ThreadTracerInterface
{
public:
    ThreadTracerInterface()          = default;
    virtual ~ThreadTracerInterface() = default;

    virtual void start_context()                                                                = 0;
    virtual void stop_context()                                                                 = 0;
    virtual void resource_init(const hsa::AgentCache&, const CoreApiTable&, const AmdExtTable&) = 0;
    virtual void resource_deinit(const hsa::AgentCache&)                                        = 0;
};

class DispatchThreadTracer : public ThreadTracerInterface
{
    using code_object_id_t = uint64_t;
    using AQLPacketPtr     = std::unique_ptr<hsa::AQLPacket>;
    using inst_pkt_t       = common::container::small_vector<std::pair<AQLPacketPtr, int64_t>, 4>;

public:
    DispatchThreadTracer(thread_trace_parameter_pack _params)
    : params(std::move(_params))
    {}
    ~DispatchThreadTracer() override = default;

    void start_context() override;
    void stop_context() override;
    void resource_init(const hsa::AgentCache&, const CoreApiTable&, const AmdExtTable&) override;
    void resource_deinit(const hsa::AgentCache&) override;

    static void codeobj_tracing_callback(rocprofiler_callback_tracing_record_t record,
                                         rocprofiler_user_data_t*              user_data,
                                         void*                                 callback_data);

    std::unique_ptr<hsa::AQLPacket> pre_kernel_call(const hsa::Queue&              queue,
                                                    uint64_t                       kernel_id,
                                                    rocprofiler_dispatch_id_t      dispatch_id,
                                                    rocprofiler_user_data_t*       user_data,
                                                    const context::correlation_id* corr_id);

    void post_kernel_call(inst_pkt_t& aql, const hsa::Queue::queue_info_session_t& session);

    std::unordered_map<hsa_agent_t, std::unique_ptr<ThreadTracerQueue>> agents;

    std::shared_mutex agents_map_mut;
    std::atomic<int>  post_move_data{0};

    thread_trace_parameter_pack params;
    rocprofiler_context_id_t    codeobj_client_ctx{0};
};

class AgentThreadTracer : public ThreadTracerInterface
{
public:
    AgentThreadTracer(thread_trace_parameter_pack _params, rocprofiler_agent_id_t _id)
    : agent_id(_id)
    , params(std::move(_params))
    {}
    ~AgentThreadTracer() override = default;

    void start_context() override;
    void stop_context() override;
    void resource_init(const hsa::AgentCache&, const CoreApiTable&, const AmdExtTable&) override;
    void resource_deinit(const hsa::AgentCache&) override;

    static void codeobj_tracing_callback(rocprofiler_callback_tracing_record_t record,
                                         rocprofiler_user_data_t*              user_data,
                                         void*                                 callback_data);

    rocprofiler_agent_id_t             agent_id;
    std::mutex                         mut;
    std::unique_ptr<ThreadTracerQueue> tracer{nullptr};

    thread_trace_parameter_pack params;
    rocprofiler_context_id_t    codeobj_client_ctx{0};
};

};  // namespace thread_trace

}  // namespace rocprofiler
