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

#include <rocprofiler-sdk/agent.h>
#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/registration.h>

#include "lib/rocprofiler-sdk/agent.hpp"
#include "lib/rocprofiler-sdk/registration.hpp"
#include "lib/rocprofiler-sdk/tests/details/agent.hpp"

#include <fmt/core.h>
#include <gtest/gtest.h>
#include <hsa/hsa.h>
#include <hsa/hsa_api_trace.h>

#include <pthread.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <type_traits>
#include <typeinfo>

TEST(rocprofiler_lib, agent_abi)
{
    constexpr auto msg = "ABI break. NEW FIELDS MAY ONLY BE ADDED AT END OF STRUCT";

    EXPECT_EQ(offsetof(rocprofiler_agent_t, size), 0) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, id), 8) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, type), 16) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, cpu_cores_count), 20) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, simd_count), 24) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, mem_banks_count), 28) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, caches_count), 32) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, io_links_count), 36) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, cpu_core_id_base), 40) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, simd_id_base), 44) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, max_waves_per_simd), 48) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, lds_size_in_kb), 52) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, gds_size_in_kb), 56) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, num_gws), 60) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, wave_front_size), 64) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, num_xcc), 68) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, cu_count), 72) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, array_count), 76) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, num_shader_banks), 80) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, simd_arrays_per_engine), 84) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, cu_per_simd_array), 88) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, simd_per_cu), 92) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, max_slots_scratch_cu), 96) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, gfx_target_version), 100) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, vendor_id), 104) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, device_id), 106) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, location_id), 108) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, domain), 112) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, drm_render_minor), 116) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, num_sdma_engines), 120) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, num_sdma_xgmi_engines), 124) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, num_sdma_queues_per_engine), 128) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, num_cp_queues), 132) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, max_engine_clk_ccompute), 136) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, max_engine_clk_fcompute), 140) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, sdma_fw_version), 144) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, fw_version), 148) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, capability), 152) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, cu_per_engine), 156) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, max_waves_per_cu), 160) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, family_id), 164) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, workgroup_max_size), 168) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, grid_max_size), 172) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, local_mem_size), 176) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, hive_id), 184) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, gpu_id), 192) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, workgroup_max_dim), 200) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, grid_max_dim), 212) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, mem_banks), 224) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, caches), 232) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, io_links), 240) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, name), 248) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, vendor_name), 256) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, product_name), 264) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, model_name), 272) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, node_id), 280) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, logical_node_id), 284) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, logical_node_type_id), 288) << msg;
    EXPECT_EQ(offsetof(rocprofiler_agent_t, reserved_padding0), 292) << msg;
    // Add test for offset of new field above this. Do NOT change any existing values!

    constexpr auto expected_rocp_agent_size = 296;
    // If a new field is added, increase this value by the size of the new field(s)
    EXPECT_EQ(sizeof(rocprofiler_agent_t), expected_rocp_agent_size)
        << "ABI break. If you added a new field, make sure that this is the only new check that "
           "failed. Please add a check for the new field at the offset and update this test to the "
           "new size";
    static_assert(sizeof(rocprofiler_agent_t) == expected_rocp_agent_size, "Update agent size!");
}

TEST(rocprofiler_lib, agent)
{
    rocprofiler::registration::init_logging();

    auto info_ret = std::system("/usr/bin/rocminfo");
    if(info_ret != 0) info_ret = std::system("rocminfo");
    if(info_ret != 0) info_ret = std::system("/opt/rocm/bin/rocminfo");

    std::cout << "# Data from '/sys/class/kfd/kfd/topology/nodes': \n" << std::flush;
    auto sys_ret_kfd = std::system(
        "/bin/bash -c 'for i in $(find /sys/class/kfd/kfd/topology/nodes -maxdepth 2 -type f | "
        "grep properties | sort); do echo -e \"\n##### ${i} #####\n\"; cat ${i}; echo \"\"; done'");
    EXPECT_EQ(sys_ret_kfd, 0);

    std::cout << "# Data from '/sys/devices/virtual/kfd/kfd/topology/nodes': \n" << std::flush;
    auto sys_ret_virt =
        std::system("/bin/bash -c 'for i in $(find /sys/devices/virtual/kfd/kfd/topology/nodes "
                    "-maxdepth 2 -type f | grep properties | sort); do echo -e \"\n##### ${i} "
                    "#####\n\"; cat ${i}; echo \"\"; done'");
    EXPECT_EQ(sys_ret_virt, 0);

    static_assert(std::is_same<rocprofiler_agent_t, rocprofiler_agent_v0_t>::value,
                  "update test to support new agent struct version");

    auto                                    agents     = std::vector<const rocprofiler_agent_t*>{};
    rocprofiler_query_available_agents_cb_t iterate_cb = [](rocprofiler_agent_version_t agents_ver,
                                                            const void**                agents_arr,
                                                            size_t                      num_agents,
                                                            void*                       user_data) {
        EXPECT_EQ(agents_ver, ROCPROFILER_AGENT_INFO_VERSION_0);
        if(agents_ver != ROCPROFILER_AGENT_INFO_VERSION_0) return ROCPROFILER_STATUS_ERROR;

        auto* agents_v = static_cast<std::vector<const rocprofiler_agent_t*>*>(user_data);
        for(size_t i = 0; i < num_agents; ++i)
        {
            const auto* agent = static_cast<const rocprofiler_agent_t*>(agents_arr[i]);
            agents_v->emplace_back(agent);
        }
        return ROCPROFILER_STATUS_SUCCESS;
    };

    hsa_init();
    {
        auto table         = ::HsaApiTable{};
        auto core_table    = ::CoreApiTable{};
        auto amd_ext_table = ::AmdExtTable{};

        memset(&table, 0, sizeof(table));
        memset(&core_table, 0, sizeof(core_table));
        memset(&amd_ext_table, 0, sizeof(amd_ext_table));

        core_table.hsa_iterate_agents_fn                    = &hsa_iterate_agents;
        core_table.hsa_status_string_fn                     = &hsa_status_string;
        core_table.hsa_agent_get_info_fn                    = &hsa_agent_get_info;
        amd_ext_table.hsa_amd_agent_iterate_memory_pools_fn = &hsa_amd_agent_iterate_memory_pools;
        amd_ext_table.hsa_amd_memory_pool_get_info_fn       = &hsa_amd_memory_pool_get_info;
        table.core_                                         = &core_table;
        table.amd_ext_                                      = &amd_ext_table;
        rocprofiler::agent::construct_agent_cache(&table);
    }

    std::cout << "# querying available agents...\n" << std::flush;
    auto status =
        rocprofiler_query_available_agents(ROCPROFILER_AGENT_INFO_VERSION_0,
                                           iterate_cb,
                                           sizeof(rocprofiler_agent_t),
                                           const_cast<void*>(static_cast<const void*>(&agents)));

    EXPECT_EQ(status, ROCPROFILER_STATUS_SUCCESS);

    auto _rocm_info = rocprofiler::test::rocm_info{};
    EXPECT_EQ(rocprofiler::test::get_info(_rocm_info), 0);

    auto& hsa_agents_v = _rocm_info.agents;

    EXPECT_GE(agents.size(), hsa_agents_v.size());

    uint64_t skipped = 0;
    for(const auto* agent : agents)
    {
        ASSERT_NE(agent, nullptr);

        auto msg = fmt::format("name={}, model={}, gfx version={}, id={}, type={}",
                               agent->name,
                               agent->model_name,
                               agent->gfx_target_version,
                               agent->node_id,
                               agent->type == ROCPROFILER_AGENT_TYPE_CPU ? "CPU" : "GPU");

        rocprofiler::test::agent_info_t* hsa_agent = nullptr;
        {
            auto _hsa_agent = rocprofiler::agent::get_hsa_agent(agent);

            if(!_hsa_agent)
            {
                ++skipped;
                continue;
            }

            for(auto& hitr : hsa_agents_v)
            {
                if(_hsa_agent && _hsa_agent->handle == hitr.hsa_agent.handle)
                {
                    hsa_agent = &hitr;
                    break;
                }
            }
            ASSERT_NE(hsa_agent, nullptr) << msg;
        }

        if(agent->type == ROCPROFILER_AGENT_TYPE_CPU)
        {
            EXPECT_EQ(hsa_agent->device_type, HSA_DEVICE_CPU) << msg;
        }
        else if(agent->type == ROCPROFILER_AGENT_TYPE_GPU)
        {
            EXPECT_EQ(hsa_agent->device_type, HSA_DEVICE_GPU) << msg;
        }
        else
        {
            EXPECT_TRUE(false) << msg << " :: agent-type != CPU|GPU :: " << agent->type;
        }

        EXPECT_EQ(std::string_view{agent->name}, std::string_view{hsa_agent->name}) << msg;
        EXPECT_EQ(std::string_view{agent->vendor_name}, std::string_view{hsa_agent->vendor_name})
            << msg;
        EXPECT_EQ(std::string_view{agent->product_name},
                  std::string_view{hsa_agent->device_mkt_name})
            << msg;
        // TODO(aelwazir): To be changed back to use node id once ROCR fixes the hsa_agents to use
        // the real node id
        EXPECT_EQ(agent->logical_node_id, hsa_agent->internal_node_id) << msg;
        EXPECT_EQ(agent->location_id, hsa_agent->bdf_id) << msg;
        EXPECT_EQ(agent->device_id, hsa_agent->chip_id) << msg;
        EXPECT_EQ(agent->simd_count, hsa_agent->compute_unit * hsa_agent->simds_per_cu) << msg;
        EXPECT_EQ(agent->cu_count, hsa_agent->compute_unit) << msg;
        EXPECT_EQ(agent->simd_per_cu, hsa_agent->simds_per_cu) << msg;
        EXPECT_EQ(agent->wave_front_size, hsa_agent->wavefront_size) << msg;
        EXPECT_EQ(agent->simd_arrays_per_engine, hsa_agent->shader_arrs_per_sh_eng) << msg;
        EXPECT_EQ(agent->max_waves_per_cu, hsa_agent->max_waves_per_cu) << msg;
        EXPECT_EQ(agent->num_shader_banks, hsa_agent->shader_engs) << msg;
        EXPECT_EQ(agent->workgroup_max_size, hsa_agent->workgroup_max_size) << msg;
        EXPECT_EQ(agent->workgroup_max_dim.x, hsa_agent->workgroup_max_dim[0]) << msg;
        EXPECT_EQ(agent->workgroup_max_dim.y, hsa_agent->workgroup_max_dim[1]) << msg;
        EXPECT_EQ(agent->workgroup_max_dim.z, hsa_agent->workgroup_max_dim[2]) << msg;
        EXPECT_EQ(agent->grid_max_size, hsa_agent->grid_max_size) << msg;
        EXPECT_EQ(agent->grid_max_dim.x, hsa_agent->grid_max_dim.x) << msg;
        EXPECT_EQ(agent->grid_max_dim.y, hsa_agent->grid_max_dim.y) << msg;
        EXPECT_EQ(agent->grid_max_dim.z, hsa_agent->grid_max_dim.z) << msg;
        if(agent->type == ROCPROFILER_AGENT_TYPE_GPU)
        {
            // HSA lib doesn't set family ID for CPU-only but we do
            EXPECT_EQ(agent->family_id, hsa_agent->family_id) << msg;
        }
        EXPECT_EQ(agent->fw_version.ui32.uCode, hsa_agent->ucode_version) << msg;
        EXPECT_EQ(agent->sdma_fw_version.uCodeSDMA, hsa_agent->sdma_ucode_version) << msg;

        if(hsa_agent->shader_engs > 0)
        {
            EXPECT_EQ(agent->cu_per_engine, hsa_agent->compute_unit / hsa_agent->shader_engs)
                << msg;
        }
    }

    EXPECT_EQ(skipped, (agents.size() - hsa_agents_v.size()));

    // clean up memory leak
    for(auto& itr : _rocm_info.isas)
        delete[] itr.name_str;
}
