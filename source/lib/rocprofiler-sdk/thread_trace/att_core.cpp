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

#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/intercept_table.h>
#include <rocprofiler-sdk/rocprofiler.h>

#include "lib/common/container/stable_vector.hpp"
#include "lib/common/utility.hpp"
#include "lib/rocprofiler-sdk/buffer.hpp"
#include "lib/rocprofiler-sdk/context/context.hpp"
#include "lib/rocprofiler-sdk/hsa/queue_controller.hpp"
#include "lib/rocprofiler-sdk/internal_threading.hpp"
#include "lib/rocprofiler-sdk/registration.hpp"

#include <hsa/hsa_api_trace.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK_HSA(fn, message)                                                                     \
    {                                                                                              \
        auto _status = (fn);                                                                       \
        if(_status != HSA_STATUS_SUCCESS)                                                          \
        {                                                                                          \
            ROCP_ERROR << "HSA Err: " << _status << '\n';                                          \
            throw std::runtime_error(message);                                                     \
        }                                                                                          \
    }

constexpr size_t ROCPROFILER_QUEUE_SIZE = 64;

namespace rocprofiler
{
namespace thread_trace
{
struct cbdata_t
{
    rocprofiler_att_shader_data_callback_t cb_fn;
    const rocprofiler_user_data_t*         dispatch_userdata;
};

common::Synchronized<std::optional<int64_t>> client;

bool
ThreadTracerQueue::Submit(hsa_ext_amd_aql_pm4_packet_t* packet)
{
    const uint64_t write_idx = add_write_index_relaxed_fn(queue, 1);

    size_t index      = (write_idx % queue->size) * sizeof(hsa_ext_amd_aql_pm4_packet_t);
    auto*  queue_slot = reinterpret_cast<uint32_t*>(size_t(queue->base_address) + index);  // NOLINT

    const auto* slot_data = reinterpret_cast<const uint32_t*>(packet);

    memcpy(&queue_slot[1], &slot_data[1], sizeof(hsa_ext_amd_aql_pm4_packet_t) - sizeof(uint32_t));
    auto* header = reinterpret_cast<std::atomic<uint32_t>*>(queue_slot);

    header->store(slot_data[0], std::memory_order_release);
    signal_store_screlease_fn(queue->doorbell_signal, write_idx);

    int loops = 0;
    while(load_read_index_relaxed_fn(queue) <= write_idx)
    {
        loops++;
        usleep(1);
        if(loops > 10000)  // Add loop limit to prevent hang. TODO: Remove once stability proven
        {
            ROCP_ERROR << "Codeobj packet submission failed!";
            return false;
        }
    }
    return true;
}

ThreadTracerQueue::ThreadTracerQueue(thread_trace_parameter_pack _params,
                                     const hsa::AgentCache&      cache,
                                     const CoreApiTable&         coreapi,
                                     const AmdExtTable&          ext)
: params(std::move(_params))
{
    factory = std::make_unique<aql::ThreadTraceAQLPacketFactory>(cache, this->params, coreapi, ext);
    control_packet = factory->construct_control_packet();

    auto status = coreapi.hsa_queue_create_fn(cache.get_hsa_agent(),
                                              ROCPROFILER_QUEUE_SIZE,
                                              HSA_QUEUE_TYPE_SINGLE,
                                              nullptr,
                                              nullptr,
                                              UINT32_MAX,
                                              UINT32_MAX,
                                              &this->queue);
    if(status != HSA_STATUS_SUCCESS)
    {
        ROCP_ERROR << "Failed to create thread trace async queue";
        this->queue = nullptr;
    }

    queue_destroy_fn           = coreapi.hsa_queue_destroy_fn;
    signal_store_screlease_fn  = coreapi.hsa_signal_store_screlease_fn;
    add_write_index_relaxed_fn = coreapi.hsa_queue_add_write_index_relaxed_fn;
    load_read_index_relaxed_fn = coreapi.hsa_queue_load_read_index_relaxed_fn;
}

ThreadTracerQueue::~ThreadTracerQueue()
{
    std::unique_lock<std::mutex> lk(trace_resources_mut);
    if(active_traces.load() < 1)
    {
        if(queue_destroy_fn) queue_destroy_fn(this->queue);
        return;
    }

    ROCP_WARNING << "Thread tracer being destroyed with thread trace active";

    control_packet->clear();
    control_packet->populate_after();

    for(auto& after_packet : control_packet->after_krn_pkt)
        Submit(&after_packet);
}

/**
 * Callback we get from HSA interceptor when a kernel packet is being enqueued.
 * We return an AQLPacket containing the start/stop/read packets for injection.
 */
std::unique_ptr<hsa::TraceControlAQLPacket>
ThreadTracerQueue::get_control(bool bStart)
{
    std::unique_lock<std::mutex> lk(trace_resources_mut);

    auto active_resources = std::make_unique<hsa::TraceControlAQLPacket>(*control_packet);
    active_resources->clear();

    if(bStart) active_traces.fetch_add(1);

    return active_resources;
}

hsa_status_t
thread_trace_callback(uint32_t shader, void* buffer, uint64_t size, void* callback_data)
{
    auto& cb_data = *static_cast<cbdata_t*>(callback_data);

    cb_data.cb_fn(shader, buffer, size, *cb_data.dispatch_userdata);
    return HSA_STATUS_SUCCESS;
}

void
ThreadTracerQueue::iterate_data(aqlprofile_handle_t handle, rocprofiler_user_data_t data)
{
    cbdata_t cb_dt{};

    cb_dt.cb_fn             = params.shader_cb_fn;
    cb_dt.dispatch_userdata = &data;

    auto status = aqlprofile_att_iterate_data(handle, thread_trace_callback, &cb_dt);
    CHECK_HSA(status, "Failed to iterate ATT data");

    active_traces.fetch_sub(1);
}

void
ThreadTracerQueue::load_codeobj(code_object_id_t id, uint64_t addr, uint64_t size)
{
    std::unique_lock<std::mutex> lk(trace_resources_mut);

    control_packet->add_codeobj(id, addr, size);

    if(!queue || active_traces.load() < 1) return;

    auto packet   = factory->construct_load_marker_packet(id, addr, size);
    bool bSuccess = Submit(&packet->packet);

    if(!bSuccess)  // If something went wrong, don't delete packet to avoid CP memory access fault
        packet.release();
}

void
ThreadTracerQueue::unload_codeobj(code_object_id_t id)
{
    std::unique_lock<std::mutex> lk(trace_resources_mut);

    control_packet->remove_codeobj(id);

    if(!queue || active_traces.load() < 1) return;

    auto packet   = factory->construct_unload_marker_packet(id);
    bool bSuccess = Submit(&packet->packet);

    if(!bSuccess)  // If something went wrong, don't delete packet to avoid CP memory access fault
        packet.release();
}

// TODO: make this a wrapper on HSA load instead of registering
void
DispatchThreadTracer::codeobj_tracing_callback(rocprofiler_callback_tracing_record_t record,
                                               rocprofiler_user_data_t* /* user_data */,
                                               void* callback_data)
{
    if(!callback_data) return;
    if(record.kind != ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT) return;
    if(record.operation != ROCPROFILER_CODE_OBJECT_LOAD) return;

    auto* rec = static_cast<rocprofiler_callback_tracing_code_object_load_data_t*>(record.payload);
    assert(rec);

    DispatchThreadTracer& tracer = *static_cast<DispatchThreadTracer*>(callback_data);
    auto                  agent  = rec->hsa_agent;

    std::shared_lock<std::shared_mutex> lk(tracer.agents_map_mut);

    auto tracer_it = tracer.agents.find(agent);
    if(tracer_it == tracer.agents.end()) return;

    if(record.phase == ROCPROFILER_CALLBACK_PHASE_LOAD)
        tracer_it->second->load_codeobj(rec->code_object_id, rec->load_delta, rec->load_size);
    else if(record.phase == ROCPROFILER_CALLBACK_PHASE_UNLOAD)
        tracer_it->second->unload_codeobj(rec->code_object_id);
}

void
DispatchThreadTracer::resource_init(const hsa::AgentCache& cache,
                                    const CoreApiTable&    coreapi,
                                    const AmdExtTable&     ext)
{
    auto                                agent = cache.get_hsa_agent();
    std::unique_lock<std::shared_mutex> lk(agents_map_mut);

    auto agent_it = agents.find(agent);
    if(agent_it != agents.end())
    {
        agent_it->second->active_queues.fetch_add(1);
        return;
    }

    auto new_tracer = std::make_unique<ThreadTracerQueue>(this->params, cache, coreapi, ext);
    agents.emplace(agent, std::move(new_tracer));
}

void
DispatchThreadTracer::resource_deinit(const hsa::AgentCache& cache)
{
    std::unique_lock<std::shared_mutex> lk(agents_map_mut);

    auto agent_it = agents.find(cache.get_hsa_agent());
    if(agent_it == agents.end()) return;

    if(agent_it->second->active_queues.fetch_sub(1) > 1) return;

    agents.erase(cache.get_hsa_agent());
}

/**
 * Callback we get from HSA interceptor when a kernel packet is being enqueued.
 * We return an AQLPacket containing the start/stop/read packets for injection.
 */
std::unique_ptr<hsa::AQLPacket>
DispatchThreadTracer::pre_kernel_call(const hsa::Queue&              queue,
                                      rocprofiler_kernel_id_t        kernel_id,
                                      rocprofiler_dispatch_id_t      dispatch_id,
                                      rocprofiler_user_data_t*       user_data,
                                      const context::correlation_id* corr_id)
{
    rocprofiler_correlation_id_t rocprof_corr_id =
        rocprofiler_correlation_id_t{.internal = 0, .external = context::null_user_data};

    if(corr_id) rocprof_corr_id.internal = corr_id->internal;
    // TODO: Get external

    // Maybe adds serialization packets to the AQLPacket (if serializer is enabled)
    // and maybe adds barrier packets if the state is transitioning from serialized <->
    // unserialized
    auto maybe_add_serialization = [&](auto& gen_pkt) {
        CHECK_NOTNULL(hsa::get_queue_controller())->serializer().rlock([&](const auto& serializer) {
            for(auto& s_pkt : serializer.kernel_dispatch(queue))
                gen_pkt->before_krn_pkt.push_back(s_pkt.ext_amd_aql_pm4);
        });
    };

    auto control_flags = params.dispatch_cb_fn(queue.get_id(),
                                               queue.get_agent().get_rocp_agent(),
                                               rocprof_corr_id,
                                               kernel_id,
                                               dispatch_id,
                                               user_data,
                                               params.callback_userdata);

    if(control_flags == ROCPROFILER_ATT_CONTROL_NONE)
    {
        auto empty = std::make_unique<hsa::EmptyAQLPacket>();
        maybe_add_serialization(empty);
        return empty;
    }

    std::shared_lock<std::shared_mutex> lk(agents_map_mut);

    auto it = agents.find(queue.get_agent().get_hsa_agent());
    assert(it != agents.end() && it->second != nullptr);

    auto packet = it->second->get_control(bool(control_flags & ROCPROFILER_ATT_CONTROL_START));

    post_move_data.fetch_add(1);
    maybe_add_serialization(packet);

    if((control_flags & ROCPROFILER_ATT_CONTROL_START) != 0) packet->populate_before();

    if((control_flags & ROCPROFILER_ATT_CONTROL_STOP) != 0) packet->populate_after();

    return packet;
}

class SignalSerializerExit
{
public:
    SignalSerializerExit(const hsa::Queue::queue_info_session_t& _session)
    : session(_session)
    {}
    ~SignalSerializerExit()
    {
        auto* controller = hsa::get_queue_controller();
        if(!controller) return;

        controller->serializer().wlock(
            [&](auto& serializer) { serializer.kernel_completion_signal(session.queue); });
    }
    const hsa::Queue::queue_info_session_t& session;
};

void
DispatchThreadTracer::post_kernel_call(DispatchThreadTracer::inst_pkt_t&       aql,
                                       const hsa::Queue::queue_info_session_t& session)
{
    SignalSerializerExit signal(session);

    if(post_move_data.load() < 1) return;

    for(auto& aql_pkt : aql)
    {
        auto* pkt = dynamic_cast<hsa::TraceControlAQLPacket*>(aql_pkt.first.get());
        if(!pkt) continue;

        std::shared_lock<std::shared_mutex> lk(agents_map_mut);
        post_move_data.fetch_sub(1);

        if(pkt->after_krn_pkt.empty()) continue;

        auto it = agents.find(pkt->GetAgent());
        if(it != agents.end() && it->second != nullptr)
            it->second->iterate_data(pkt->GetHandle(), session.user_data);
    }
}

void
DispatchThreadTracer::start_context()
{
    using corr_id_map_t = hsa::Queue::queue_info_session_t::external_corr_id_map_t;
    if(codeobj_client_ctx.handle != 0)
    {
        auto status = rocprofiler_start_context(codeobj_client_ctx);
        if(status != ROCPROFILER_STATUS_SUCCESS) throw std::exception();
    }

    CHECK_NOTNULL(hsa::get_queue_controller())->enable_serialization();

    // Only one thread should be attempting to enable/disable this context
    client.wlock([&](auto& client_id) {
        if(client_id) return;

        client_id = hsa::get_queue_controller()->add_callback(
            std::nullopt,
            [=](const hsa::Queue& q,
                const hsa::rocprofiler_packet& /* kern_pkt */,
                rocprofiler_kernel_id_t   kernel_id,
                rocprofiler_dispatch_id_t dispatch_id,
                rocprofiler_user_data_t*  user_data,
                const corr_id_map_t& /* extern_corr_ids */,
                const context::correlation_id* corr_id) {
                return this->pre_kernel_call(q, kernel_id, dispatch_id, user_data, corr_id);
            },
            [=](const hsa::Queue& /* q */,
                hsa::rocprofiler_packet /* kern_pkt */,
                const hsa::Queue::queue_info_session_t& session,
                inst_pkt_t& aql) { this->post_kernel_call(aql, session); });
    });
}

void
DispatchThreadTracer::stop_context()
{
    client.wlock([&](auto& client_id) {
        if(!client_id) return;

        // Remove our callbacks from HSA's queue controller
        hsa::get_queue_controller()->remove_callback(*client_id);
        client_id = std::nullopt;
    });

    auto* controller = hsa::get_queue_controller();
    if(controller) controller->disable_serialization();
}

void
AgentThreadTracer::resource_init(const hsa::AgentCache& cache,
                                 const CoreApiTable&    coreapi,
                                 const AmdExtTable&     ext)
{
    if(cache.get_rocp_agent()->id != this->agent_id) return;

    std::unique_lock<std::mutex> lk(mut);

    if(tracer != nullptr)
    {
        tracer->active_queues.fetch_add(1);
        return;
    }

    tracer = std::make_unique<ThreadTracerQueue>(this->params, cache, coreapi, ext);
}

void
AgentThreadTracer::resource_deinit(const hsa::AgentCache& cache)
{
    if(cache.get_rocp_agent()->id != this->agent_id) return;

    std::unique_lock<std::mutex> lk(mut);
    if(tracer == nullptr) return;

    if(tracer->active_queues.fetch_sub(1) == 1) tracer.reset();
}

void
AgentThreadTracer::start_context()
{
    std::unique_lock<std::mutex> lk(mut);

    if(tracer == nullptr)
    {
        ROCP_FATAL << "Thread trace context not present for agent!";
        return;
    }

    auto packet = tracer->get_control(true);
    packet->populate_before();

    for(auto& start : packet->before_krn_pkt)
        tracer->Submit(&start);
}

void
AgentThreadTracer::stop_context()
{
    std::unique_lock<std::mutex> lk(mut);

    auto packet = tracer->get_control(false);
    packet->populate_after();

    for(auto& stop : packet->after_krn_pkt)
        tracer->Submit(&stop);

    rocprofiler_user_data_t userdata{.ptr = params.callback_userdata};
    tracer->iterate_data(packet->GetHandle(), userdata);
}

void
AgentThreadTracer::codeobj_tracing_callback(rocprofiler_callback_tracing_record_t record,
                                            rocprofiler_user_data_t* /* user_data */,
                                            void* callback_data)
{
    if(!callback_data) return;
    if(record.kind != ROCPROFILER_CALLBACK_TRACING_CODE_OBJECT) return;
    if(record.operation != ROCPROFILER_CODE_OBJECT_LOAD) return;

    auto* rec = static_cast<rocprofiler_callback_tracing_code_object_load_data_t*>(record.payload);
    assert(rec);

    AgentThreadTracer&           tracer = *static_cast<AgentThreadTracer*>(callback_data);
    std::unique_lock<std::mutex> lk(tracer.mut);

    if(record.phase == ROCPROFILER_CALLBACK_PHASE_LOAD)
        tracer.tracer->load_codeobj(rec->code_object_id, rec->load_delta, rec->load_size);
    else if(record.phase == ROCPROFILER_CALLBACK_PHASE_UNLOAD)
        tracer.tracer->unload_codeobj(rec->code_object_id);
}

}  // namespace thread_trace

}  // namespace rocprofiler
