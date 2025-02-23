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

#include "metrics_test.h"

#include <gtest/gtest.h>

#include <algorithm>

#include <rocprofiler-sdk/rocprofiler.h>

#include "lib/common/logging.hpp"
#include "lib/rocprofiler-sdk/agent.hpp"
#include "lib/rocprofiler-sdk/counters/metrics.hpp"

namespace
{
namespace counters = ::rocprofiler::counters;

auto
loadTestData(const std::unordered_map<std::string, std::vector<std::vector<std::string>>>& map)
{
    std::unordered_map<std::string, std::vector<counters::Metric>> ret;
    for(const auto& [gfx, dataMap] : map)
    {
        auto& metric_vec = ret.emplace(gfx, std::vector<counters::Metric>{}).first->second;
        for(const auto& data_vec : dataMap)
        {
            metric_vec.emplace_back("gfx9",
                                    data_vec.at(0),
                                    data_vec.at(1),
                                    data_vec.at(2),
                                    data_vec.at(4),
                                    data_vec.at(3),
                                    "",
                                    0);
        }
    }
    return ret;
}
}  // namespace

TEST(metrics, base_load)
{
    auto rocp_data = counters::getBaseHardwareMetrics();
    auto test_data = loadTestData(basic_gfx908);

    ASSERT_EQ(rocp_data.count("gfx908"), 1);
    ASSERT_EQ(test_data.count("gfx908"), 1);
    auto rocp_data_v = rocp_data.at("gfx908");
    auto test_data_v = test_data.at("gfx908");
    // get_agent_available_properties() is the metrics added for fields in agent.hpp
    EXPECT_EQ(rocp_data_v.size(),
              test_data_v.size() + rocprofiler::agent::get_agent_available_properties().size());
    auto find = [&rocp_data_v](const auto& v) -> std::optional<counters::Metric> {
        for(const auto& ditr : rocp_data_v)
        {
            ROCP_INFO << fmt::format("{}", ditr);
            if(ditr.name() == v.name()) return ditr;
        }
        return std::nullopt;
    };
    auto equal = [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.name(), lhs.block(), lhs.event(), lhs.description()) ==
               std::tie(rhs.name(), rhs.block(), rhs.event(), rhs.description());
    };
    for(const auto& itr : test_data_v)
    {
        auto val = find(itr);
        if(!val)
        {
            EXPECT_TRUE(val) << "failed to find " << fmt::format("{}", itr);
            continue;
        }
        EXPECT_TRUE(equal(itr, *val)) << fmt::format("\n\t{} \n\t\t!= \n\t{}", itr, *val);
    }
}

TEST(metrics, derived_load)
{
    auto rocp_data = counters::getDerivedHardwareMetrics();
    auto test_data = loadTestData(derived_gfx908);
    ASSERT_EQ(rocp_data.count("gfx908"), 1);
    ASSERT_EQ(test_data.count("gfx908"), 1);
    auto rocp_data_v = rocp_data.at("gfx908");
    auto test_data_v = test_data.at("gfx908");
    EXPECT_EQ(rocp_data_v.size(), test_data_v.size());
    auto find = [&rocp_data_v](const auto& v) -> std::optional<counters::Metric> {
        for(const auto& ditr : rocp_data_v)
            if(ditr.name() == v.name()) return ditr;
        return std::nullopt;
    };
    auto equal = [](const auto& lhs, const auto& rhs) {
        return std::tie(
                   lhs.name(), lhs.block(), lhs.event(), lhs.description(), lhs.expression()) ==
               std::tie(rhs.name(), rhs.block(), rhs.event(), rhs.description(), rhs.expression());
    };
    for(const auto& itr : test_data_v)
    {
        auto val = find(itr);
        if(!val)
        {
            EXPECT_TRUE(val) << "failed to find " << fmt::format("{}", itr);
            continue;
        }
        EXPECT_TRUE(equal(itr, *val)) << fmt::format("\n\t{} \n\t\t!= \n\t{}", itr, *val);
    }
}

TEST(metrics, check_agent_valid)
{
    const auto& rocp_data      = *counters::getMetricMap();
    auto        common_metrics = [&]() -> std::set<uint64_t> {
        std::set<uint64_t> ret;
        for(const auto& [gfx, counters] : rocp_data)
        {
            std::set<uint64_t> counter_ids;
            for(const auto& metric : counters)
            {
                counter_ids.insert(metric.id());
            }

            if(ret.empty())
            {
                ret = counter_ids;
            }
            else
            {
                std::set<uint64_t> out_intersection;
                std::set_intersection(ret.begin(),
                                      ret.end(),
                                      counter_ids.begin(),
                                      counter_ids.end(),
                                      std::inserter(out_intersection, out_intersection.begin()));
            }

            if(ret.empty()) return ret;
        }
        return ret;
    }();

    for(const auto& [gfx, counters] : rocp_data)
    {
        for(const auto& metric : counters)
        {
            ASSERT_EQ(counters::checkValidMetric(gfx, metric), true)
                << gfx << " " << fmt::format("{}", metric);
        }

        for(const auto& [other_gfx, other_counters] : rocp_data)
        {
            if(other_gfx == gfx) continue;
            for(const auto& metric : other_counters)
            {
                if(common_metrics.count(metric.id()) || !metric.special().empty()) continue;
                EXPECT_EQ(counters::checkValidMetric(gfx, metric), false)
                    << fmt::format("GFX {} has Metric {} but shouldn't", gfx, metric);
            }
        }
    }
}

TEST(metrics, check_public_api_query)
{
    const auto* id_map = counters::getMetricIdMap();
    for(const auto& [id, metric] : *id_map)
    {
        rocprofiler_counter_info_v0_t version;

        ASSERT_EQ(
            rocprofiler_query_counter_info(
                {.handle = id}, ROCPROFILER_COUNTER_INFO_VERSION_0, static_cast<void*>(&version)),
            ROCPROFILER_STATUS_SUCCESS);
        EXPECT_EQ(version.name, metric.name().c_str());
        EXPECT_EQ(version.block, metric.block().c_str());
        EXPECT_EQ(version.expression, metric.expression().c_str());
        EXPECT_EQ(version.is_derived, !metric.expression().empty());
        EXPECT_EQ(version.description, metric.description().c_str());
    }
}
