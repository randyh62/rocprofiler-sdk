// MIT License
//
// Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "lib/rocprofiler-sdk/hsa/scratch_memory.hpp"
#include "lib/common/defines.hpp"
#include "lib/common/utility.hpp"
#include "lib/rocprofiler-sdk/buffer.hpp"
#include "lib/rocprofiler-sdk/context/context.hpp"
#include "lib/rocprofiler-sdk/hsa/defines.hpp"
#include "lib/rocprofiler-sdk/hsa/hsa.hpp"
#include "lib/rocprofiler-sdk/hsa/queue_controller.hpp"

#include <rocprofiler-sdk/agent.h>
#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/hsa/api_id.h>
#include <rocprofiler-sdk/hsa/table_id.h>

#include <glog/logging.h>
#include <hsa/amd_hsa_signal.h>
#include <hsa/hsa.h>
#include <hsa/hsa_amd_tool.h>
#include <hsa/hsa_api_trace.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <tuple>
#include <type_traits>
#include <utility>

HSA_API_TABLE_LOOKUP_DEFINITION(ROCPROFILER_HSA_TABLE_ID_AmdTool, ::ToolsApiTable, amd_tool)

namespace rocprofiler
{
namespace hsa
{
namespace
{
template <typename T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

enum scratch_event_kind
{
    hsa_amd_tool_id_none = 0,
    // scratch reporting
    hsa_amd_tool_id_scratch_event_alloc_start,
    hsa_amd_tool_id_scratch_event_alloc_end,
    hsa_amd_tool_id_scratch_event_free_start,
    hsa_amd_tool_id_scratch_event_free_end,
    hsa_amd_tool_id_scratch_event_async_reclaim_start,
    hsa_amd_tool_id_scratch_event_async_reclaim_end,
    hsa_amd_tool_id_scratch_event_last,
};
}  // namespace
}  // namespace hsa
}  // namespace rocprofiler

HSA_API_META_DEFINITION_NP(ROCPROFILER_HSA_TABLE_ID_AmdTool,
                           hsa_amd_tool_id_scratch_event_alloc_start,
                           hsa_amd_tool_scratch_event_alloc_start,
                           hsa_amd_tool_scratch_event_alloc_start_fn);
HSA_API_META_DEFINITION_NP(ROCPROFILER_HSA_TABLE_ID_AmdTool,
                           hsa_amd_tool_id_scratch_event_alloc_end,
                           hsa_amd_tool_scratch_event_alloc_end,
                           hsa_amd_tool_scratch_event_alloc_end_fn);
HSA_API_META_DEFINITION_NP(ROCPROFILER_HSA_TABLE_ID_AmdTool,
                           hsa_amd_tool_id_scratch_event_free_start,
                           hsa_amd_tool_scratch_event_free_start,
                           hsa_amd_tool_scratch_event_free_start_fn);
HSA_API_META_DEFINITION_NP(ROCPROFILER_HSA_TABLE_ID_AmdTool,
                           hsa_amd_tool_id_scratch_event_free_end,
                           hsa_amd_tool_scratch_event_free_end,
                           hsa_amd_tool_scratch_event_free_end_fn);
HSA_API_META_DEFINITION_NP(ROCPROFILER_HSA_TABLE_ID_AmdTool,
                           hsa_amd_tool_id_scratch_event_async_reclaim_start,
                           hsa_amd_tool_scratch_event_async_reclaim_start,
                           hsa_amd_tool_scratch_event_async_reclaim_start_fn);
HSA_API_META_DEFINITION_NP(ROCPROFILER_HSA_TABLE_ID_AmdTool,
                           hsa_amd_tool_id_scratch_event_async_reclaim_end,
                           hsa_amd_tool_scratch_event_async_reclaim_end,
                           hsa_amd_tool_scratch_event_async_reclaim_end_fn);

namespace rocprofiler
{
namespace hsa
{
namespace scratch_memory
{
using context_t           = context::context;
using context_array_t     = common::container::small_vector<const context_t*>;
using correlation_service = context::correlation_tracing_service;

bool
context_filter(const context::context* ctx)
{
    const auto need_buffering =
        (ctx->buffered_tracer &&
         (ctx->buffered_tracer->domains(ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY)));
    const auto need_callbacks =
        (ctx->callback_tracer &&
         (ctx->callback_tracer->domains(ROCPROFILER_CALLBACK_TRACING_SCRATCH_MEMORY)));

    return need_buffering || need_callbacks;
}

bool
should_wrap_functor(const context::context_array_t& _contexts, int _operation)
{
    // we loop over all the *registered* contexts and see if any of them, at any point in time,
    // might require callback or buffered API tracing
    for(const auto& itr : _contexts)
    {
        if(!itr) continue;

        // if there is a callback tracer enabled for the given domain and op, we need to wrap
        if(itr->callback_tracer &&
           itr->callback_tracer->domains(ROCPROFILER_CALLBACK_TRACING_SCRATCH_MEMORY, _operation))
            return true;

        // if there is a buffered tracer enabled for the given domain and op, we need to wrap
        if(itr->buffered_tracer &&
           itr->buffered_tracer->domains(ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY, _operation))
            return true;
    }
    return false;
}

template <size_t TableIdx, size_t OpIdx>
auto&
get_next_dispatch()
{
    using function_t     = typename hsa_api_meta<TableIdx, OpIdx>::function_type;
    static function_t _v = nullptr;
    return _v;
}

template <typename FuncT, typename ArgsT, size_t... Idx>
decltype(auto)
invoke(FuncT&& _func, ArgsT&& _args, std::index_sequence<Idx...>)
{
    using RetT = decltype(std::forward<FuncT>(_func)(std::get<Idx>(_args)...));

    // Scratch function pointers that we saved could've been null
    if constexpr(std::is_same_v<RetT, hsa_status_t>)
    {
        if(_func != nullptr)
        {
            return std::forward<FuncT>(_func)(std::get<Idx>(_args)...);
        }
        else
        {
            return hsa_status_t::HSA_STATUS_SUCCESS;
        }
    }
    else
    {
        static_assert(sizeof(RetT) < 0, "Unexpected types for HSA tools table invoke");
    }
}

template <size_t N>
struct amd_tool_api_info;

template <size_t N>
struct scratch_op_info;

template <>
struct scratch_op_info<hsa_amd_tool_id_none>
{
    static constexpr auto operation = ROCPROFILER_SCRATCH_MEMORY_NONE;
    static constexpr auto phase     = ROCPROFILER_CALLBACK_PHASE_NONE;
};

#define SPECIALIZE_AMD_TOOL(TOOL_OP, STARTPHASE, ENDPHASE)                                          \
    template <>                                                                                     \
    struct scratch_op_info<hsa_amd_tool_id_##STARTPHASE>                                            \
    {                                                                                               \
        static constexpr auto                         operation = ROCPROFILER_##TOOL_OP;            \
        static constexpr rocprofiler_callback_phase_t phase     = ROCPROFILER_CALLBACK_PHASE_ENTER; \
        using function_t =                                                                          \
            decltype(ToolsApiTable::IMPL_DETAIL_CONCATENATE(hsa_amd_tool_##STARTPHASE, _fn));       \
    };                                                                                              \
    template <>                                                                                     \
    struct scratch_op_info<hsa_amd_tool_id_##ENDPHASE>                                              \
    {                                                                                               \
        static constexpr auto                         operation = ROCPROFILER_##TOOL_OP;            \
        static constexpr rocprofiler_callback_phase_t phase     = ROCPROFILER_CALLBACK_PHASE_EXIT;  \
        using function_t =                                                                          \
            decltype(ToolsApiTable::IMPL_DETAIL_CONCATENATE(hsa_amd_tool_##ENDPHASE, _fn));         \
    };                                                                                              \
    template <>                                                                                     \
    struct amd_tool_api_info<ROCPROFILER_##TOOL_OP>                                                 \
    {                                                                                               \
        using start_fn_t = scratch_op_info<hsa_amd_tool_id_##STARTPHASE>::function_t;               \
        using end_fn_t   = scratch_op_info<hsa_amd_tool_id_##ENDPHASE>::function_t;                 \
        static constexpr auto operation_idx = ROCPROFILER_##TOOL_OP;                                \
        static constexpr auto name          = #TOOL_OP;                                             \
    }

SPECIALIZE_AMD_TOOL(SCRATCH_MEMORY_ALLOC, scratch_event_alloc_start, scratch_event_alloc_end);
SPECIALIZE_AMD_TOOL(SCRATCH_MEMORY_FREE, scratch_event_free_start, scratch_event_free_end);
SPECIALIZE_AMD_TOOL(SCRATCH_MEMORY_ASYNC_RECLAIM,
                    scratch_event_async_reclaim_start,
                    scratch_event_async_reclaim_end);

template <>
struct amd_tool_api_info<ROCPROFILER_SCRATCH_MEMORY_NONE>
{
    using start_fn_t                    = std::nullptr_t;
    using end_fn_t                      = std::nullptr_t;
    static constexpr auto operation_idx = ROCPROFILER_SCRATCH_MEMORY_NONE;
    static constexpr auto name          = "SCRATCH_MEMORY_NONE";
    static constexpr auto start_phase   = nullptr;
    static constexpr auto end_phase     = nullptr;
};

#undef SPECIALIZE_AMD_TOOL
template <size_t S>
struct event_info_t;

template <size_t OpIdx, typename... Args>
hsa_status_t
impl(Args... args);

namespace
{
template <size_t Idx, size_t... IdxTail>
const char*
name_by_id(const uint32_t id, std::index_sequence<Idx, IdxTail...>)
{
    if(Idx == id) return amd_tool_api_info<Idx>::name;
    if constexpr(sizeof...(IdxTail) > 0)
        return name_by_id(id, std::index_sequence<IdxTail...>{});
    else
        return nullptr;
}

template <size_t Idx, size_t... IdxTail>
uint32_t
id_by_name(const char* name, std::index_sequence<Idx, IdxTail...>)
{
    if(std::string_view{amd_tool_api_info<Idx>::name} == std::string_view{name})
        return amd_tool_api_info<Idx>::operation_idx;
    if constexpr(sizeof...(IdxTail) > 0)
        return id_by_name(name, std::index_sequence<IdxTail...>{});
    else
        return ROCPROFILER_SCRATCH_MEMORY_NONE;
}

template <size_t... Idx>
void
get_ids(std::vector<uint32_t>& _id_list, std::index_sequence<Idx...>)
{
    auto _emplace = [](auto& _vec, uint32_t _v) {
        if(_v < static_cast<uint32_t>(ROCPROFILER_SCRATCH_MEMORY_LAST)) _vec.emplace_back(_v);
    };

    (_emplace(_id_list, amd_tool_api_info<Idx>::operation_idx), ...);
}

template <size_t... Idx>
void
get_names(std::vector<const char*>& _name_list, std::index_sequence<Idx...>)
{
    auto _emplace = [](auto& _vec, const char* _v) {
        if(_v != nullptr && strnlen(_v, 1) > 0) _vec.emplace_back(_v);
    };

    (_emplace(_name_list, amd_tool_api_info<Idx>::name), ...);
}

template <size_t TableIdx, typename LookupT = internal_table, size_t OpIdx>
void
copy_table(hsa_amd_tool_table_t* _orig, uint64_t _tbl_instance)
{
    using table_type = typename hsa_table_lookup<TableIdx>::type;

    static_assert(std::is_same<hsa_amd_tool_table_t, table_type>::value);

    if constexpr(OpIdx > hsa_amd_tool_id_none)
    {
        auto _info = hsa_api_meta<TableIdx, OpIdx>{};

        auto& _orig_table = _info.get_table(_orig);
        auto& _orig_func  = _info.get_table_func(_orig_table);
        // make sure we don't access a field that doesn't exist in input table
        if(_info.offset() >= _orig->version.minor_id) return;

        auto& _copy_table = _info.get_table(hsa_table_lookup<TableIdx>{}(LookupT{}));
        auto& _copy_func  = _info.get_table_func(_copy_table);

        LOG_IF(FATAL, _copy_func && _tbl_instance == 0)
            << _info.name << " has non-null function pointer " << _copy_func
            << " despite this being the first instance of the library being copies";

        if(!_copy_func)
        {
            LOG(INFO) << "copying table entry for " << _info.name;
            _copy_func = _orig_func;
        }
        else
        {
            LOG(INFO) << "skipping copying table entry for " << _info.name
                      << " from table instance " << _tbl_instance;
        }
    }
}

struct buffered_context_data
{
    const context::context* ctx = nullptr;
};

struct callback_context_data
{
    const context::context*               ctx       = nullptr;
    rocprofiler_callback_tracing_record_t record    = {};
    rocprofiler_user_data_t               user_data = {.value = 0};
};

void
populate_contexts(int                                 operation_idx,
                  std::vector<callback_context_data>& callback_contexts,
                  std::vector<buffered_context_data>& buffered_contexts)
{
    callback_contexts.clear();
    buffered_contexts.clear();

    auto active_contexts = context::context_array_t{};
    for(const auto* itr : context::get_active_contexts(active_contexts))
    {
        if(!itr) continue;

        if(itr->callback_tracer)
        {
            // if the given domain + op is not enabled, skip this context
            if(itr->callback_tracer->domains(ROCPROFILER_CALLBACK_TRACING_SCRATCH_MEMORY,
                                             operation_idx))
                callback_contexts.emplace_back(
                    callback_context_data{itr, rocprofiler_callback_tracing_record_t{}});
        }

        if(itr->buffered_tracer)
        {
            // if the given domain + op is not enabled, skip this context
            if(itr->buffered_tracer->domains(ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY,
                                             operation_idx))
                buffered_contexts.emplace_back(buffered_context_data{itr});
        }
    }
}

}  // namespace

static_assert(ROCPROFILER_SCRATCH_MEMORY_ALLOC ==
              scratch_op_info<hsa_amd_tool_id_scratch_event_alloc_end>::operation);
static_assert(ROCPROFILER_SCRATCH_MEMORY_FREE ==
              scratch_op_info<hsa_amd_tool_id_scratch_event_free_start>::operation);

static_assert(ROCPROFILER_CALLBACK_PHASE_EXIT ==
              scratch_op_info<hsa_amd_tool_id_scratch_event_alloc_end>::phase);
static_assert(ROCPROFILER_CALLBACK_PHASE_ENTER ==
              scratch_op_info<hsa_amd_tool_id_scratch_event_free_start>::phase);

#define ASSERT_SAME_OFFSET(S)                                                                      \
    static_assert(offsetof(hsa_amd_event_scratch_alloc_start_t, flags) ==                          \
                  offsetof(hsa_amd_event_scratch_##S, flags));

ASSERT_SAME_OFFSET(alloc_start_t);
ASSERT_SAME_OFFSET(alloc_end_t);
ASSERT_SAME_OFFSET(free_start_t);
ASSERT_SAME_OFFSET(free_end_t);
ASSERT_SAME_OFFSET(async_reclaim_start_t);
ASSERT_SAME_OFFSET(async_reclaim_end_t);

#undef ASSERT_SAME_OFFSET

template <typename T, typename... Ts>
constexpr bool have_same_offset(T /*m*/)
{
    return (offsetof(Ts, m) == ...);
}

template <typename T, typename... Ts>
struct same_flags_offset
{
    static constexpr auto value = ((offsetof(T, flags) == offsetof(Ts, flags)) && ...);
};

auto
get_flags(hsa_amd_tool_event_t event)
{
    static_assert(same_flags_offset<hsa_amd_event_scratch_alloc_start_t,
                                    hsa_amd_event_scratch_alloc_end_t,
                                    hsa_amd_event_scratch_free_start_t,
                                    hsa_amd_event_scratch_free_end_t,
                                    hsa_amd_event_scratch_async_reclaim_start_t,
                                    hsa_amd_event_scratch_async_reclaim_end_t>::value);
    return static_cast<rocprofiler_scratch_alloc_flag_t>(event.scratch_alloc_start->flags);
}

/*
Template instantiation per start/stop pairs to track event data through thread local storage
*/
template <size_t OpIdx>
auto&
get_tls_pair(rocprofiler_callback_phase_t phase)
{
    // Tony and Laurent's suggestion
    // To pair up a start event with an end event because we get them as separate callback
    // invocations, use thread local storage to track the item through a single callback
    // function for both start and end. Template on the buffer types instead of the callback
    // types
    // OpIdx = rocprofiler_callback_phase_t
    static_assert(
        (OpIdx > ROCPROFILER_SCRATCH_MEMORY_NONE) && (OpIdx < ROCPROFILER_SCRATCH_MEMORY_LAST),
        "Invalid event pair OpIdx");

    using callback_data_t = rocprofiler_callback_tracing_scratch_memory_data_t;
    using buffered_data_t = rocprofiler_buffer_tracing_scratch_memory_record_t;

    struct tls_data
    {
        callback_data_t callback_data = common::init_public_api_struct(callback_data_t{});
        buffered_data_t buffered_data = common::init_public_api_struct(buffered_data_t{});
        std::vector<callback_context_data> callback_ctxs = {};
        std::vector<buffered_context_data> buffered_ctxs = {};
    };

    static thread_local auto tls  = tls_data{};
    static thread_local auto held = false;

    if(phase == ROCPROFILER_CALLBACK_PHASE_ENTER)
    {
        LOG_IF(FATAL, held) << "Overwriting scratch memory TLS data";
        held = true;
        populate_contexts(OpIdx, tls.callback_ctxs, tls.buffered_ctxs);
    }
    else
    {
        held = false;
    }

    return tls;
}

template <size_t ScratchOpIdx, typename... Args>
hsa_status_t
impl(Args... args)
{
    using arg_event_t =
        common::mpl::unqualified_type_t<decltype(std::get<0>(std::make_tuple(args...)))>;
    static_assert(std::is_same_v<arg_event_t, hsa_amd_tool_event_t>, "unexpected type");

    constexpr auto OpIdx   = scratch_op_info<ScratchOpIdx>::operation;
    constexpr auto OpPhase = scratch_op_info<ScratchOpIdx>::phase;

    auto&& _tied_args = std::tie(args...);
    auto&  event_data = std::get<0>(_tied_args);

    // this lets start and end of the same type have the same thread local storage
    auto& tls = get_tls_pair<OpIdx>(OpPhase);

    if(tls.callback_ctxs.empty() && tls.buffered_ctxs.empty()) return HSA_STATUS_SUCCESS;

    const auto tid = common::get_tid();

    const auto get_agent_id = [](const hsa_queue_t* hsa_queue) -> rocprofiler_agent_id_t {
        rocprofiler_agent_id_t _agent_id{static_cast<uint64_t>(-1)};
        bool                   found_agent{false};

        rocprofiler::hsa::get_queue_controller()->iterate_queues(
            [&](const rocprofiler::hsa::Queue* queue_ptr) {
                if(queue_ptr->intercept_queue()->id == hsa_queue->id)
                {
                    _agent_id   = queue_ptr->get_agent().get_rocp_agent()->id;
                    found_agent = true;
                }
            });

        LOG_IF(FATAL, !found_agent) << fmt::format(
            "Scratch memory tracing: Could not find a valid agent for queue id {}", hsa_queue->id);
        return _agent_id;
    };

    auto*      corr_id          = context::get_latest_correlation_id();
    const auto invoke_callbacks = [&](const rocprofiler_callback_phase_t _phase) {
        for(auto& itr : tls.callback_ctxs)
        {
            auto corr_id_v = rocprofiler_correlation_id_t{};
            if(corr_id)
            {
                corr_id_v.internal = corr_id->internal;
                corr_id_v.external = itr.ctx->correlation_tracer.external_correlator.get(tid);
            }

            auto record = rocprofiler_callback_tracing_record_t{
                .context_id     = rocprofiler_context_id_t{itr.ctx->context_idx},
                .thread_id      = tid,
                .correlation_id = corr_id_v,
                .kind           = ROCPROFILER_CALLBACK_TRACING_SCRATCH_MEMORY,
                .operation      = OpIdx,
                .phase          = _phase,
                .payload        = static_cast<void*>(&tls.callback_data),
            };

            auto& cb_info = itr.ctx->callback_tracer->callback_data.at(
                ROCPROFILER_CALLBACK_TRACING_SCRATCH_MEMORY);

            cb_info.callback(record, &itr.user_data, &cb_info.data);
        }
    };

    if constexpr(OpPhase == ROCPROFILER_CALLBACK_PHASE_ENTER)
    {
        if(!tls.callback_ctxs.empty())
        {
            tls.callback_data.agent_id  = get_agent_id(event_data.scratch_alloc_start->queue);
            tls.callback_data.queue_id  = {event_data.scratch_alloc_start->queue->id};
            tls.callback_data.args_kind = event_data.none->kind;
            tls.callback_data.flags     = get_flags(event_data);

            if constexpr(OpIdx == ROCPROFILER_SCRATCH_MEMORY_ALLOC)
            {
                tls.callback_data.args.alloc_start.dispatch_id =
                    event_data.scratch_alloc_start->dispatch_id;
            }

            invoke_callbacks(OpPhase);  // NOLINT readability-misleading-indentation
        }

        if(!tls.buffered_ctxs.empty())
        {
            tls.buffered_data.kind            = ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY;
            tls.buffered_data.operation       = OpIdx;
            tls.buffered_data.agent_id        = get_agent_id(event_data.scratch_alloc_start->queue);
            tls.buffered_data.queue_id        = {event_data.scratch_alloc_start->queue->id};
            tls.buffered_data.thread_id       = tid;
            tls.buffered_data.start_timestamp = common::timestamp_ns();
        }
    }
    else if constexpr(OpPhase == ROCPROFILER_CALLBACK_PHASE_EXIT)
    {
        if(!tls.buffered_ctxs.empty())
        {
            tls.buffered_data.flags         = get_flags(event_data);
            tls.buffered_data.end_timestamp = common::timestamp_ns();
        }

        if(!tls.callback_ctxs.empty())
        {
            tls.callback_data.flags     = get_flags(event_data);
            tls.callback_data.args_kind = event_data.none->kind;
            if constexpr(OpIdx == ROCPROFILER_SCRATCH_MEMORY_ALLOC)
            {
                auto& data_args       = tls.callback_data.args.alloc_end;
                data_args.dispatch_id = event_data.scratch_alloc_end->dispatch_id;
                data_args.size        = event_data.scratch_alloc_end->size;
                data_args.num_slots   = event_data.scratch_alloc_end->num_slots;
            }

            invoke_callbacks(OpPhase);  // NOLINT readability-misleading-indentation
        }

        if(!tls.buffered_ctxs.empty())
        {
            for(const auto& itr : tls.buffered_ctxs)
            {
                auto* _buffer = buffer::get_buffer(itr.ctx->buffered_tracer->buffer_data.at(
                    ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY));

                auto corr_id_v = rocprofiler_correlation_id_t{};
                if(corr_id)
                {
                    // TODO(mkuriche) should the id be generated at entry?
                    corr_id_v.internal = corr_id->internal;
                    corr_id_v.external = itr.ctx->correlation_tracer.external_correlator.get(tid);
                }

                auto _record           = tls.buffered_data;
                _record.correlation_id = corr_id_v;

                CHECK_NOTNULL(_buffer)->emplace(ROCPROFILER_BUFFER_CATEGORY_TRACING,
                                                ROCPROFILER_BUFFER_TRACING_SCRATCH_MEMORY,
                                                _record);
            }
        }
    }

    return invoke(get_next_dispatch<ROCPROFILER_HSA_TABLE_ID_AmdTool, ScratchOpIdx>(),
                  std::move(_tied_args),
                  std::make_index_sequence<sizeof...(Args)>{});
}

template <size_t TableIdx, size_t OpIdx, typename RetT, typename... Args>
auto get_hsa_amd_tool_api_impl(RetT (*)(Args...))
{
    return &scratch_memory::impl<OpIdx, Args...>;
}

template <size_t TableIdx, size_t OpIdx>
void
update_table(const context_array_t& ctxs, hsa_amd_tool_table_t* _orig)
{
    if constexpr(OpIdx > hsa_amd_tool_id_none)
    {
        auto _info = hsa_api_meta<TableIdx, OpIdx>{};

        if(!should_wrap_functor(ctxs, OpIdx)) return;

        LOG(INFO) << "updating table entry for " << _info.name;

        auto  _meta  = hsa_api_meta<TableIdx, OpIdx>{};
        auto& _table = _meta.get_table(_orig);
        auto& _func  = _meta.get_table_func(_table);

        _func = get_hsa_amd_tool_api_impl<TableIdx, OpIdx>(_func);
    }
}

template <size_t TableIdx, size_t... OpIdx>
void
update_table(context_array_t ctxs, hsa_amd_tool_table_t* _orig, std::index_sequence<OpIdx...>)
{
    static_assert(
        std::is_same<hsa_amd_tool_table_t, typename hsa_table_lookup<TableIdx>::type>::value,
        "unexpected type");

    (update_table<TableIdx, OpIdx>(ctxs, _orig), ...);
}

template <size_t TableIdx, typename LookupT = internal_table, size_t... OpIdx>
void
copy_table(hsa_amd_tool_table_t* _orig, uint64_t _tbl_instance, std::index_sequence<OpIdx...>)
{
    static_assert(
        std::is_same<hsa_amd_tool_table_t, typename hsa_table_lookup<TableIdx>::type>::value,
        "unexpected type");

    (copy_table<TableIdx, LookupT, OpIdx>(_orig, _tbl_instance), ...);
}

void
copy_table(hsa_amd_tool_table_t* _orig, uint64_t _tbl_instance)
{
    if(_orig)
        copy_table<ROCPROFILER_HSA_TABLE_ID_AmdTool, internal_table>(
            _orig, _tbl_instance, std::make_index_sequence<hsa_amd_tool_id_scratch_event_last>{});
}

void
update_table(hsa_amd_tool_table_t* _orig, uint64_t _tbl_instance)
{
    if(_orig)
    {
        auto ctxs = context::get_registered_contexts(context_filter);
        if(!ctxs.empty())
        {
            copy_table<ROCPROFILER_HSA_TABLE_ID_AmdTool, tracing_table>(
                _orig,
                _tbl_instance,
                std::make_index_sequence<hsa_amd_tool_id_scratch_event_last>{});

            update_table<ROCPROFILER_HSA_TABLE_ID_AmdTool>(
                ctxs, _orig, std::make_index_sequence<hsa_amd_tool_id_scratch_event_last>{});
        }
    }
}

const char*
name_by_id(uint32_t id)
{
    return name_by_id(id, std::make_index_sequence<ROCPROFILER_SCRATCH_MEMORY_LAST>{});
}

uint32_t
id_by_name(const char* name)
{
    return id_by_name(name, std::make_index_sequence<ROCPROFILER_SCRATCH_MEMORY_LAST>{});
}

std::vector<uint32_t>
get_ids()
{
    auto _data = std::vector<uint32_t>{};
    _data.reserve(ROCPROFILER_SCRATCH_MEMORY_LAST);
    get_ids(_data, std::make_index_sequence<ROCPROFILER_SCRATCH_MEMORY_LAST>{});
    return _data;
}

std::vector<const char*>
get_names()
{
    auto _data = std::vector<const char*>{};
    _data.reserve(ROCPROFILER_SCRATCH_MEMORY_LAST);
    get_names(_data, std::make_index_sequence<ROCPROFILER_SCRATCH_MEMORY_LAST>{});
    return _data;
}
}  // namespace scratch_memory
}  // namespace hsa
}  // namespace rocprofiler
