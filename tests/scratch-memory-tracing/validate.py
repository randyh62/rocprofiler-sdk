#!/usr/bin/env python3

import sys
import pytest
import json

from collections import defaultdict


# helper function
def node_exists(name, data, min_len=1):
    assert name in data
    assert data[name] is not None
    if isinstance(data[name], (list, tuple, dict, set)):
        assert len(data[name]) >= min_len


def test_data_structure(input_data):
    """verify minimum amount of expected data is present"""
    data = input_data
    sdk_data = input_data["rocprofiler-sdk-json-tool"]

    node_exists("rocprofiler-sdk-json-tool", data)

    sdk_data = data["rocprofiler-sdk-json-tool"]

    num_agents = len([agent for agent in sdk_data["agents"] if agent["type"] == 2])

    node_exists("metadata", sdk_data)
    node_exists("pid", sdk_data["metadata"])
    node_exists("main_tid", sdk_data["metadata"])
    node_exists("init_time", sdk_data["metadata"])
    node_exists("fini_time", sdk_data["metadata"])

    node_exists("agents", sdk_data)
    node_exists("call_stack", sdk_data)
    node_exists("callback_records", sdk_data)
    node_exists("buffer_records", sdk_data)

    node_exists("names", sdk_data["callback_records"])
    node_exists("code_objects", sdk_data["callback_records"])
    node_exists("kernel_symbols", sdk_data["callback_records"])
    node_exists("hsa_api_traces", sdk_data["callback_records"])
    node_exists("hip_api_traces", sdk_data["callback_records"], 0)
    node_exists("scratch_memory_traces", sdk_data["callback_records"], min_len=8)

    node_exists("names", sdk_data["buffer_records"])
    node_exists("kernel_dispatches", sdk_data["buffer_records"])
    node_exists("memory_copies", sdk_data["buffer_records"], num_agents)
    node_exists("hsa_api_traces", sdk_data["buffer_records"])
    node_exists("hip_api_traces", sdk_data["buffer_records"], 0)
    node_exists("retired_correlation_ids", sdk_data["buffer_records"])
    node_exists("scratch_memory_traces", sdk_data["buffer_records"], min_len=8)


def test_timestamps(input_data):
    data = input_data
    sdk_data = data["rocprofiler-sdk-json-tool"]

    cb_start = {}
    cb_end = {}
    for titr in ["hsa_api_traces", "hip_api_traces"]:
        for itr in sdk_data["callback_records"][titr]:
            cid = itr["record"]["correlation_id"]["internal"]
            phase = itr["record"]["phase"]
            if phase == 1:
                cb_start[cid] = itr["timestamp"]
            elif phase == 2:
                cb_end[cid] = itr["timestamp"]
                assert cb_start[cid] <= itr["timestamp"]
            else:
                assert phase == 1 or phase == 2

        for itr in sdk_data["buffer_records"][titr]:
            assert itr["start_timestamp"] <= itr["end_timestamp"]

    for titr in ["kernel_dispatches", "memory_copies"]:
        for itr in sdk_data["buffer_records"][titr]:
            assert itr["start_timestamp"] < itr["end_timestamp"]
            assert itr["correlation_id"]["internal"] > 0
            assert itr["correlation_id"]["external"] > 0
            assert sdk_data["metadata"]["init_time"] < itr["start_timestamp"]
            assert sdk_data["metadata"]["init_time"] < itr["end_timestamp"]
            assert sdk_data["metadata"]["fini_time"] > itr["start_timestamp"]
            assert sdk_data["metadata"]["fini_time"] > itr["end_timestamp"]

            # TODO(Is this check applicable for scratch, which doesn't use any correlation id?)
            # api_start = cb_start[itr["correlation_id"]["internal"]]
            # api_end = cb_end[itr["correlation_id"]["internal"]]
            # assert api_start < itr["start_timestamp"]
            # assert api_end <= itr["end_timestamp"]


def test_internal_correlation_ids(input_data):
    data = input_data
    sdk_data = data["rocprofiler-sdk-json-tool"]

    api_corr_ids = []
    for titr in ["hsa_api_traces", "hip_api_traces"]:
        for itr in sdk_data["callback_records"][titr]:
            api_corr_ids.append(itr["record"]["correlation_id"]["internal"])

        for itr in sdk_data["buffer_records"][titr]:
            api_corr_ids.append(itr["correlation_id"]["internal"])

    api_corr_ids_sorted = sorted(api_corr_ids)
    api_corr_ids_unique = list(set(api_corr_ids))

    for itr in sdk_data["buffer_records"]["kernel_dispatches"]:
        assert itr["correlation_id"]["internal"] in api_corr_ids_unique

    for itr in sdk_data["buffer_records"]["memory_copies"]:
        assert itr["correlation_id"]["internal"] in api_corr_ids_unique

    len_corr_id_unq = len(api_corr_ids_unique)
    assert len(api_corr_ids) != len_corr_id_unq
    assert max(api_corr_ids_sorted) == len_corr_id_unq


def test_external_correlation_ids(input_data):
    data = input_data
    sdk_data = data["rocprofiler-sdk-json-tool"]

    extern_corr_ids = []
    for titr in ["hsa_api_traces", "hip_api_traces"]:
        for itr in sdk_data["callback_records"][titr]:
            assert itr["record"]["correlation_id"]["external"] > 0
            assert (
                itr["record"]["thread_id"] == itr["record"]["correlation_id"]["external"]
            )
            extern_corr_ids.append(itr["record"]["correlation_id"]["external"])

    extern_corr_ids = list(set(sorted(extern_corr_ids)))
    for titr in ["hsa_api_traces", "hip_api_traces"]:
        for itr in sdk_data["buffer_records"][titr]:
            assert itr["correlation_id"]["external"] > 0
            assert itr["thread_id"] == itr["correlation_id"]["external"]
            assert itr["thread_id"] in extern_corr_ids
            assert itr["correlation_id"]["external"] in extern_corr_ids

    for itr in sdk_data["buffer_records"]["kernel_dispatches"]:
        assert itr["correlation_id"]["external"] > 0
        assert itr["correlation_id"]["external"] in extern_corr_ids

    for itr in sdk_data["buffer_records"]["memory_copies"]:
        assert itr["correlation_id"]["external"] > 0
        assert itr["correlation_id"]["external"] in extern_corr_ids


def op_name(op_name, record):
    found_op = False
    op_key = None

    for kind_node in record["names"]["kind_names"]:
        if kind_node["value"] == op_name:
            op_key = kind_node["key"]

    for op_node in record["names"]["operation_names"]:
        if op_node["key"] == op_key:
            return op_node


# Tests above are identical to async-copy. Update as needed


def test_scratch_memory_tracking(input_data):
    sdk_data = input_data["rocprofiler-sdk-json-tool"]
    callback_records = sdk_data["callback_records"]
    buffer_records = sdk_data["buffer_records"]

    scratch_callback_data = sdk_data["callback_records"]["scratch_memory_traces"]
    scratch_buffer_data = sdk_data["buffer_records"]["scratch_memory_traces"]

    cb_op_names = op_name("SCRATCH_MEMORY", callback_records)["value"]
    bf_op_names = op_name("SCRATCH_MEMORY", buffer_records)["value"]

    assert len(cb_op_names) == 4
    assert len(bf_op_names) == 4

    # op name -> enum value
    scratch_cb_op_map = {node["value"]: node["key"] for node in cb_op_names}
    scratch_bf_op_map = {node["value"]: node["key"] for node in bf_op_names}
    assert scratch_cb_op_map == scratch_bf_op_map

    scratch_reported_agent_ids = set()
    detected_agents_ids = set(
        agent["id"]["handle"] for agent in sdk_data["agents"] if agent["type"] == 2
    )
    # check buffering data
    for node in scratch_buffer_data:
        assert "size" in node
        assert "kind" in node
        assert "flags" in node
        assert "thread_id" in node
        assert "end_timestamp" in node
        assert "start_timestamp" in node

        assert "queue_id" in node
        assert "agent_id" in node
        assert "operation" in node
        assert "handle" in node["queue_id"]

        assert node["start_timestamp"] > 0
        assert node["start_timestamp"] < node["end_timestamp"]

        scratch_reported_agent_ids.add(node["agent_id"]["handle"])

    assert 2**64 - 1 not in scratch_reported_agent_ids
    assert scratch_reported_agent_ids == detected_agents_ids

    # { thread-id -> [ events ],  ... }
    cb_threads = defaultdict(list)
    bf_threads = defaultdict(list)

    # fetch node["payload"]
    pl = lambda x: x["payload"]
    # fetch node["record"]
    rc = lambda x: x["record"]

    for node in scratch_callback_data:
        cb_threads[rc(node)["thread_id"]].append(node)

    for node in scratch_buffer_data:
        bf_threads[node["thread_id"]].append(node)

    for thread_id, nodes in cb_threads.items():
        assert thread_id > 0

        # start must be followed by end
        for inx in range(0, len(nodes), 2):
            this_node = nodes[inx]
            next_node = nodes[inx + 1]

            assert rc(this_node)["phase"] + 1 == rc(next_node)["phase"]
            assert rc(this_node)["thread_id"] == rc(next_node)["thread_id"]
            assert this_node["timestamp"] < next_node["timestamp"]

            # alloc has more data vs free and async reclaim
            scratch_alloc_node = (
                this_node["record"]["operation"]
                == scratch_cb_op_map["SCRATCH_MEMORY_ALLOC"]
            )
            if scratch_alloc_node:
                assert (
                    pl(this_node)["queue_id"]["handle"]
                    == pl(next_node)["queue_id"]["handle"]
                )
                assert (
                    this_node["args"]["dispatch_id"] == next_node["args"]["dispatch_id"]
                )
                assert "size" in pl(next_node) and pl(next_node)["size"] > 0
                assert (
                    "num_slots" in next_node["args"]
                    and next_node["args"]["num_slots"] > 0
                )
                assert "flags" in pl(next_node)

    # callback data and buffer data must agree with each other
    for bf_thr, bf_nodes in bf_threads.items():
        cb_nodes = cb_threads[bf_thr]

        for bf_node_inx in range(len(bf_nodes)):
            # All these 3 should have same data
            # timestamps are not same as callback records them at
            # a different instant in time. Callback timestamp
            # should be more than buffer timestamp
            bf_node = bf_nodes[bf_node_inx]
            cb_enter = cb_nodes[bf_node_inx * 2]
            cb_exit = cb_nodes[bf_node_inx * 2 + 1]

            assert (
                bf_node["operation"]
                == rc(cb_enter)["operation"]
                == rc(cb_exit)["operation"]
            )
            assert (
                bf_op_names[bf_node["operation"]]
                == cb_op_names[rc(cb_enter)["operation"]]
                == cb_op_names[rc(cb_exit)["operation"]]
            )

            assert bf_node["flags"] == pl(cb_exit)["flags"]

            assert (
                bf_node["thread_id"]
                == rc(cb_enter)["thread_id"]
                == rc(cb_exit)["thread_id"]
            )


if __name__ == "__main__":
    exit_code = pytest.main(["-x", __file__] + sys.argv[1:])
    sys.exit(exit_code)
