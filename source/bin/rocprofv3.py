#!/usr/bin/env python3

# MIT License
#
# Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

import os
import sys
import argparse
import subprocess
import re


class dotdict(dict):
    """dot.notation access to dictionary attributes"""

    __getattr__ = dict.get
    __setattr__ = dict.__setitem__
    __delattr__ = dict.__delitem__

    def __init__(self, d):
        super(dotdict, self).__init__(d)
        for k, v in self.items():
            if isinstance(v, dict):
                self.__setitem__(k, dotdict(v))
            elif isinstance(v, (list, tuple)):
                self.__setitem__(
                    k,
                    [dotdict(i) if isinstance(i, (list, tuple, dict)) else i for i in v],
                )


def fatal_error(msg, exit_code=1):
    sys.stderr.write(f"Fatal error: {msg}\n")
    sys.stderr.flush()
    sys.exit(exit_code)


def format_help(formatter, w=120, h=40):
    """Return a wider HelpFormatter, if possible."""
    try:
        kwargs = {"width": w, "max_help_position": h}
        formatter(None, **kwargs)
        return lambda prog: formatter(prog, **kwargs)
    except TypeError:
        return formatter


def strtobool(val):
    """Convert a string representation of truth to true or false.
    True values are 'y', 'yes', 't', 'true', 'on', and '1'; false values
    are 'n', 'no', 'f', 'false', 'off', and '0'.  Raises ValueError if
    'val' is anything else.
    """
    if isinstance(val, (list, tuple)):
        if len(val) > 1:
            val_type = type(val).__name__
            raise ValueError(f"invalid truth value {val} (type={val_type})")
        else:
            val = val[0]

    if isinstance(val, bool):
        return val
    elif isinstance(val, str) and val.lower() in ("y", "yes", "t", "true", "on", "1"):
        return True
    elif isinstance(val, str) and val.lower() in ("n", "no", "f", "false", "off", "0"):
        return False
    else:
        val_type = type(val).__name__
        raise ValueError(f"invalid truth value {val} (type={val_type})")


def search_path(path_list):
    supported_option = []
    lib_att_pattern = r"libatt_decoder_(trace|summary|debug|testing)\.so"
    file_list = []

    for path in path_list:
        for root, dirs, files in os.walk(path, topdown=True):
            file_list.extend(files)
            break
        for itr in file_list:
            _match = re.match(lib_att_pattern, itr)
            if _match:
                lst = re.findall("trace|debug|summary|testing", itr)
                supported_option.extend(lst)
    return set(supported_option)


def check_att_capability(args):

    path = []
    ROCPROFV3_DIR = os.path.dirname(os.path.realpath(__file__))
    ROCM_DIR = os.path.dirname(ROCPROFV3_DIR)
    support_input = {}
    tmp_parser = argparse.ArgumentParser(add_help=False)
    tmp_parser.add_argument(
        "--att-library-path",
        default=os.environ.get(
            "ATT_LIBRARY_PATH", os.environ.get("LD_LIBRARY_PATH", None)
        ),
        type=str,
        required=False,
    )

    tmp_parser.add_argument(
        "-i",
        "--input",
        default=None,
        type=str,
        required=False,
    )

    tmp_data = {}
    att_args, unparsed_args = tmp_parser.parse_known_args(args)
    tmp_keys = list(att_args.__dict__.keys())

    for itr in tmp_keys:
        if has_set_attr(att_args, itr):
            tmp_data[itr] = getattr(att_args, itr)

    data = dotdict(tmp_data)
    if data.input:
        # If index of a pass in input file is a key in the support_input dict, then that pass has att-library-path arg
        args_list = parse_input(data.input)
        for index, itr in enumerate(args_list):
            if itr.att_library_path:
                library_path = []
                if ":" in itr.att_library_path:
                    library_path.extend(itr.att_library_path.split(":"))
                else:
                    library_path.append(itr.att_library_path)
                support = search_path(library_path)
                # If the att-library-path in the input file for a pass is valid, then the value of index key in the dict, support_input, is updated to that valid path
                if support:
                    support_input[index] = set(support)
                else:
                    # If the att-library-path in the input file for a pass is invalid, then the value of index key in the dict, support_input, is empty
                    support_input[index] = []
    if data.att_library_path:
        if ":" in data.att_library_path:
            path.extend(data.att_library_path.split(":"))
        else:
            path.append(data.att_library_path)
    else:
        path.append(f"{ROCM_DIR}/lib")
        path.append(f"{ROCM_DIR}/lib64")

    support = search_path(set(path))
    if support:
        if len(path) == 1:
            os.environ["ATT_LIBRARY_PATH"] = path[0]
            os.environ["ROCPROF_ATT_LIBRARY_PATH"] = path[0]
        else:
            os.environ["ATT_LIBRARY_PATH"] = ":".join(path)
            os.environ["ROCPROF_ATT_LIBRARY_PATH"] = ":".join(path)
        return support, support_input

    return None, support_input


class booleanArgAction(argparse.Action):
    def __call__(self, parser, args, value, option_string=None):
        setattr(args, self.dest, strtobool(value))


def parse_arguments(args=None):

    usage_examples = """

%(prog)s requires double-hyphen (--) before the application to be executed, e.g.

    $ rocprofv3 [<rocprofv3-option> ...] -- <application> [<application-arg> ...]
    $ rocprofv3 --hip-trace -- ./myapp -n 1

For MPI applications (or other job launchers such as SLURM), place rocprofv3 inside the job launcher:

    $ mpirun -n 4 rocprofv3 --hip-trace -- ./mympiapp

"""

    # Create the parser
    parser = argparse.ArgumentParser(
        description="ROCProfilerV3 Run Script",
        usage="%(prog)s [options] -- <application> [application options]",
        epilog=usage_examples,
        formatter_class=format_help(argparse.RawTextHelpFormatter),
    )

    def add_parser_bool_argument(gparser, *args, **kwargs):
        gparser.add_argument(
            *args,
            **kwargs,
            action=booleanArgAction,
            nargs="?",
            const=True,
            type=str,
            required=False,
            metavar="BOOL",
        )

    io_options = parser.add_argument_group("I/O options")

    io_options.add_argument(
        "-i",
        "--input",
        help="Input file for run configuration (JSON or YAML) or counter collection (TXT)",
        required=False,
    )
    io_options.add_argument(
        "-o",
        "--output-file",
        help="For the output file name. If nothing specified default path is `%%hostname%%/%%pid%%`",
        default=os.environ.get("ROCPROF_OUTPUT_FILE_NAME", None),
        type=str,
        required=False,
    )
    io_options.add_argument(
        "-d",
        "--output-directory",
        help="For adding output path where the output files will be saved. If nothing specified default path is `%%hostname%%/%%pid%%`",
        default=os.environ.get("ROCPROF_OUTPUT_PATH", None),
        type=str,
        required=False,
    )
    io_options.add_argument(
        "--output-format",
        help="For adding output format (supported formats: csv, json, pftrace, otf2)",
        nargs="+",
        default=None,
        choices=("csv", "json", "pftrace", "otf2"),
        type=str.lower,
    )
    io_options.add_argument(
        "--log-level",
        help="Set the desired log level",
        default=None,
        choices=("fatal", "error", "warning", "info", "trace", "env"),
        type=str.lower,
    )
    io_options.add_argument(
        "-E",
        "--extra-counters",
        help="Path to YAML file containing extra counter definitions",
        type=str,
        required=False,
    )

    aggregate_tracing_options = parser.add_argument_group("Aggregate tracing options")

    add_parser_bool_argument(
        aggregate_tracing_options,
        "-r",
        "--runtime-trace",
        help="Collect tracing data for HIP runtime API, Marker (ROCTx) API, RCCL API, ROCDecode API, Memory operations (copies, scratch, and allocation), and Kernel dispatches. Similar to --sys-trace but without tracing HIP compiler API and the underlying HSA API.",
    )
    add_parser_bool_argument(
        aggregate_tracing_options,
        "-s",
        "--sys-trace",
        help="Collect tracing data for HIP API, HSA API, Marker (ROCTx) API, RCCL API, ROCDecode API, Memory operations (copies, scratch, and allocations), and Kernel dispatches.",
    )

    pc_sampling_options = parser.add_argument_group("PC sampling options")

    add_parser_bool_argument(
        pc_sampling_options,
        "--pc-sampling-beta-enabled",
        help="enable pc sampling support; beta version",
    )

    pc_sampling_options.add_argument(
        "--pc-sampling-unit",
        help="",
        default=None,
        type=str.lower,
        choices=("instructions", "cycles", "time"),
    )

    pc_sampling_options.add_argument(
        "--pc-sampling-method",
        help="",
        default=None,
        type=str.lower,
        choices=("stochastic", "host_trap"),
    )

    pc_sampling_options.add_argument(
        "--pc-sampling-interval",
        help="",
        default=None,
        type=int,
    )
    basic_tracing_options = parser.add_argument_group("Basic tracing options")

    # Add the arguments
    add_parser_bool_argument(
        basic_tracing_options,
        "--hip-trace",
        help="Combination of --hip-runtime-trace and --hip-compiler-trace. This option only enables the tracing of the HIP API. Unlike previous iterations of rocprof, this does not enable kernel tracing, memory copy tracing, etc",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--marker-trace",
        help="For collecting Marker (ROCTx) Traces. Similar to --roctx-trace option in earlier rocprof versions but with improved ROCTx library with more features",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--kernel-trace",
        help="For collecting Kernel Dispatch Traces",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--memory-copy-trace",
        help="For collecting Memory Copy Traces. This was part of HIP and HSA traces in previous rocprof versions but is now a separate option",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--memory-allocation-trace",
        help="For collecting Memory Allocation Traces. Displays starting address, allocation size, and agent where allocation occurred.",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--scratch-memory-trace",
        help="For collecting Scratch Memory operations Traces. Helps in determining scratch allocations and manage them efficiently",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--hsa-trace",
        help="For collecting --hsa-core-trace, --hsa-amd-trace,--hsa-image-trace and --hsa-finalizer-trace. This option only enables the tracing of the HSA API. Unlike previous iterations of rocprof, this does not enable kernel tracing, memory copy tracing, etc",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--rccl-trace",
        help="For collecting RCCL(ROCm Communication Collectives Library. Also pronounced as 'Rickle' ) Traces",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--kokkos-trace",
        help="Enable built-in Kokkos Tools support (implies --marker-trace and --kernel-rename)",
    )
    add_parser_bool_argument(
        basic_tracing_options,
        "--rocdecode-trace",
        help="For collecting ROCDecode Traces",
    )

    extended_tracing_options = parser.add_argument_group("Granular tracing options")

    add_parser_bool_argument(
        extended_tracing_options,
        "--hip-runtime-trace",
        help="For collecting HIP Runtime API Traces, e.g. public HIP API functions starting with 'hip' (i.e. hipSetDevice).",
    )
    add_parser_bool_argument(
        extended_tracing_options,
        "--hip-compiler-trace",
        help="For collecting HIP Compiler generated code Traces, e.g. HIP API functions starting with '__hip' (i.e. __hipRegisterFatBinary).",
    )
    add_parser_bool_argument(
        extended_tracing_options,
        "--hsa-core-trace",
        help="For collecting HSA API Traces (core API), e.g. HSA functions prefixed with only 'hsa_' (i.e. hsa_init).",
    )
    add_parser_bool_argument(
        extended_tracing_options,
        "--hsa-amd-trace",
        help="For collecting HSA API Traces (AMD-extension API), e.g. HSA function prefixed with 'hsa_amd_' (i.e. hsa_amd_coherency_get_type).",
    )
    add_parser_bool_argument(
        extended_tracing_options,
        "--hsa-image-trace",
        help="For collecting HSA API Traces (Image-extenson API), e.g. HSA functions prefixed with only 'hsa_ext_image_' (i.e. hsa_ext_image_get_capability).",
    )
    add_parser_bool_argument(
        extended_tracing_options,
        "--hsa-finalizer-trace",
        help="For collecting HSA API Traces (Finalizer-extension API), e.g. HSA functions prefixed with only 'hsa_ext_program_' (i.e. hsa_ext_program_create).",
    )

    counter_collection_options = parser.add_argument_group("Counter collection options")

    counter_collection_options.add_argument(
        "--pmc",
        help=(
            "Specify Performance Monitoring Counters to collect(comma OR space separated in case of more than 1 counters). "
            "Note: job will fail if entire set of counters cannot be collected in single pass"
        ),
        default=None,
        nargs="*",
    )

    post_processing_options = parser.add_argument_group("Post-processing tracing options")

    add_parser_bool_argument(
        post_processing_options,
        "--stats",
        help="For collecting statistics of enabled tracing types. Must be combined with one or more tracing options. No default kernel stats unlike previous rocprof versions",
    )
    add_parser_bool_argument(
        post_processing_options,
        "-S",
        "--summary",
        help="Output single summary of tracing data at the conclusion of the profiling session",
    )
    add_parser_bool_argument(
        post_processing_options,
        "-D",
        "--summary-per-domain",
        help="Output summary for each tracing domain at the conclusion of the profiling session",
    )
    post_processing_options.add_argument(
        "--summary-groups",
        help="Output a summary for each set of domains matching the regular expression, e.g. 'KERNEL_DISPATCH|MEMORY_COPY' will generate a summary from all the tracing data in the KERNEL_DISPATCH and MEMORY_COPY domains; '*._API' will generate a summary from all the tracing data in the HIP_API, HSA_API, and MARKER_API domains",
        nargs="+",
        default=None,
        type=str,
        metavar="REGULAR_EXPRESSION",
    )

    summary_options = parser.add_argument_group("Summary options")

    summary_options.add_argument(
        "--summary-output-file",
        help="Output summary to a file, stdout, or stderr (default: stderr)",
        default=None,
        type=str,
    )
    summary_options.add_argument(
        "-u",
        "--summary-units",
        help="Timing units for output summary",
        default=None,
        type=str,
        choices=("sec", "msec", "usec", "nsec"),
    )

    kernel_naming_options = parser.add_argument_group("Kernel naming options")

    add_parser_bool_argument(
        kernel_naming_options,
        "-M",
        "--mangled-kernels",
        help="Do not demangle the kernel names",
    )
    add_parser_bool_argument(
        kernel_naming_options,
        "-T",
        "--truncate-kernels",
        help="Truncate the demangled kernel names. In earlier rocprof versions, this was known as --basenames [on/off]",
    )
    add_parser_bool_argument(
        kernel_naming_options,
        "--kernel-rename",
        help="Use region names defined by roctxRangePush/roctxRangePop regions to rename the kernels. Known as --roctx-rename in earlier rocprof versions",
    )

    filter_options = parser.add_argument_group("Filtering options")

    filter_options.add_argument(
        "--kernel-include-regex",
        help="Include the kernels matching this filter from counter-collection and thread-trace data (non-matching kernels will be excluded)",
        default=None,
        type=str,
        metavar="REGULAR_EXPRESSION",
    )
    filter_options.add_argument(
        "--kernel-exclude-regex",
        help="Exclude the kernels matching this filter from counter-collection and thread-trace data (applied after --kernel-include-regex option)",
        default=None,
        type=str,
        metavar="REGULAR_EXPRESSION",
    )
    filter_options.add_argument(
        "--kernel-iteration-range",
        help="Iteration range",
        nargs="+",
        default=None,
        type=str,
    )
    filter_options.add_argument(
        "-p",
        "--collection-period",
        help="The times are specified in seconds by default, but the unit can be changed using the `--collection-period-unit` option. Start Delay Time is the time in seconds before the collection begins, Collection Time is the duration in seconds for which data is collected, and Rate is the number of times the cycle is repeated. A repeat of 0 indicates that the cycle will repeat indefinitely. Users can specify multiple configurations, each defined by a triplet in the format `start_delay:collection_time:repeat`",
        nargs="+",
        default=None,
        type=str,
        metavar=("(START_DELAY_TIME):(COLLECTION_TIME):(REPEAT)"),
    )
    filter_options.add_argument(
        "--collection-period-unit",
        help="To change the unit used in `--collection-period` or `-p`, you can specify the desired unit using the `--collection-period-unit` option. The available units are `hour` for hours, `min` for minutes, `sec` for seconds, `msec` for milliseconds, `usec` for microseconds, and `nsec` for nanoseconds",
        nargs=1,
        default=["sec"],
        type=str,
        choices=("hour", "min", "sec", "msec", "usec", "nsec"),
    )

    perfetto_options = parser.add_argument_group("Perfetto-specific options")

    perfetto_options.add_argument(
        "--perfetto-backend",
        help="Perfetto data collection backend. 'system' mode requires starting traced and perfetto daemons",
        default=None,
        type=str,
        nargs=1,
        choices=("inprocess", "system"),
    )
    perfetto_options.add_argument(
        "--perfetto-buffer-size",
        help="Size of buffer for perfetto output in KB. default: 1 GB",
        default=None,
        type=int,
        metavar="KB",
    )
    perfetto_options.add_argument(
        "--perfetto-buffer-fill-policy",
        help="Policy for handling new records when perfetto has reached the buffer limit",
        default=None,
        type=str,
        choices=("discard", "ring_buffer"),
    )
    perfetto_options.add_argument(
        "--perfetto-shmem-size-hint",
        help="Perfetto shared memory size hint in KB. default: 64 KB",
        default=None,
        type=int,
        metavar="KB",
    )

    display_options = parser.add_argument_group("Display options")

    add_parser_bool_argument(
        display_options,
        "-L",
        "--list-avail",
        help="List available PC sampling configurations and metrics for counter collection. Backed by a valid YAML file. In earlier rocprof versions, this was known as --list-basic, --list-derived and --list-counters",
    )

    advanced_options = parser.add_argument_group("Advanced options")

    advanced_options.add_argument(
        "--preload",
        help="Libraries to prepend to LD_PRELOAD (useful for sanitizer libraries)",
        default=os.environ.get("ROCPROF_PRELOAD", "").split(":"),
        nargs="*",
    )

    advanced_options.add_argument(
        "--att-library-path",
        default=os.environ.get(
            "ATT_LIBRARY_PATH", os.environ.get("LD_LIBRARY_PATH", None)
        ),
        help="ATT library path to find decoder library",
    )
    # below is available for CI because LD_PRELOADing a library linked to a sanitizer library
    # causes issues in apps where HIP is part of shared library.
    add_parser_bool_argument(
        advanced_options,
        "--suppress-marker-preload",
        help=argparse.SUPPRESS,
    )

    if args is None:
        args = sys.argv[1:]

    rocp_args = args[:]

    app_args = []

    for idx, itr in enumerate(args):
        if itr == "--":
            rocp_args = args[0:idx]
            app_args = args[(idx + 1) :]
            break

    supported_list, is_support_input = check_att_capability(rocp_args)
    if supported_list or len(is_support_input) != 0:
        choice_list = []
        for keys, values in is_support_input.items():
            choice_list.extend(values)
        if supported_list:
            choice_list.extend(list(supported_list))

        att_options = parser.add_argument_group("Advanced Thread Trace")

        add_parser_bool_argument(
            att_options,
            "--advanced-thread-trace",
            help="Enable ATT",
        )

        att_options.add_argument(
            "--att-target-cu",
            help="ATT target compute unit",
            default=None,
            type=int,
        )

        att_options.add_argument(
            "--att-simd-select",
            help="Select ATT SIMD",
            default=None,
            type=str,
        )

        att_options.add_argument(
            "--att-buffer-size",
            help="Buffer Size",
            default=None,
            type=str,
        )

        att_options.add_argument(
            "--att-shader-engine-mask",
            help="att shader engine mask",
            default=None,
            type=str,
        )

        att_options.add_argument(
            "--att-parse",
            type=str.lower,
            default=None,
            help="Select ATT Parse method from the choices",
            choices=set(choice_list),
        )

        add_parser_bool_argument(
            att_options,
            "--att-serialize-all",
            default=False,
            help="Serialize all kernels",
        )

    return (parser.parse_args(rocp_args), app_args, supported_list, is_support_input)


def parse_yaml(yaml_file):
    try:
        import yaml
    except ImportError as e:
        fatal_error(
            f"{e}\n\nYAML package is not installed. Run '{sys.executable} -m pip install pyyaml' or use JSON or text format"
        )
    try:
        lst = []
        with open(yaml_file, "r") as file:
            data = yaml.safe_load(file)
        for itr in data["jobs"]:
            # TODO: support naming jobs
            # if isinstance(itr, str):
            #     itr = data["jobs"][itr]
            itr["sub_directory"] = "pass_"
            lst.append(itr)

        return [dotdict(itr) for itr in lst]

    except yaml.YAMLError as exc:
        fatal_error(f"{exc}")

    return None


def parse_json(json_file):
    import json

    try:
        lst = []
        with open(json_file, "r") as file:
            data = json.load(file)
        for itr in data["jobs"]:
            itr["sub_directory"] = "pass_"
            lst.append(itr)

        return [dotdict(itr) for itr in lst]

    except Exception as e:
        fatal_error(f"{e}")

    return None


def parse_text(text_file):
    def process_line(line):
        if "pmc:" not in line:
            return ""
        line = line.strip()
        pos = line.find("#")
        if pos >= 0:
            line = line[0:pos]

        def _dedup(_line, _sep):
            for itr in _sep:
                _line = " ".join(_line.split(itr))
            return _line.strip()

        # remove tabs and duplicate spaces
        return _dedup(line.replace("pmc:", ""), ["\n", "\t", " "]).split(" ")

    try:
        with open(text_file, "r") as file:
            return [
                litr
                for litr in [process_line(itr) for itr in file.readlines()]
                if len(litr) > 0
            ]
    except Exception as e:
        fatal_error(f"{e}")

    return None


def parse_input(input_file):

    _, extension = os.path.splitext(input_file)
    if extension == ".txt":
        text_input = parse_text(input_file)
        text_input_lst = [{"pmc": itr, "sub_directory": "pmc_"} for itr in text_input]
        return [dotdict(itr) for itr in text_input_lst]
    elif extension in (".yaml", ".yml"):
        return parse_yaml(input_file)
    elif extension == ".json":
        return parse_json(input_file)
    else:
        fatal_error(
            f"Input file '{input_file}' does not have a recognized extension (.txt, .json, .yaml, .yml)\n"
        )

    return None


def has_set_attr(obj, key):
    if obj and hasattr(obj, key) and getattr(obj, key) is not None:
        return True
    else:
        return False


def patch_args(data):
    """Used to handle certain fields which might be specified as a string instead of an array or vice-versa"""

    if hasattr(data, "kernel_iteration_range") and isinstance(
        data.kernel_iteration_range, str
    ):
        data.kernel_iteration_range = [data.kernel_iteration_range]
    return data


def get_args(cmd_args, inp_args):
    def ensure_type(name, var, type_id):
        if not isinstance(var, type_id):
            raise TypeError(
                f"{name} is of type {type(var).__name__}, expected {type(type_id).__name__}"
            )

    ensure_type("cmd_args", cmd_args, argparse.Namespace)
    ensure_type("inp_args", inp_args, dotdict)

    cmd_keys = list(cmd_args.__dict__.keys())
    inp_keys = list(inp_args.keys())
    data = {}

    def get_attr(key):
        if has_set_attr(cmd_args, key):
            return getattr(cmd_args, key)
        elif has_set_attr(inp_args, key):
            return getattr(inp_args, key)
        return None

    for itr in set(cmd_keys + inp_keys):
        if (
            has_set_attr(cmd_args, itr)
            and has_set_attr(inp_args, itr)
            and getattr(cmd_args, itr) != getattr(inp_args, itr)
        ):
            raise RuntimeError(f"conflicting value for {itr}")
        else:
            data[itr] = get_attr(itr)

    return patch_args(dotdict(data))


def run(app_args, args, **kwargs):

    app_env = dict(os.environ)
    use_execv = kwargs.get("use_execv", True)
    app_pass = kwargs.get("pass_id", None)

    def update_env(env_var, env_val, **kwargs):
        """Local function for updating application environment which supports
        various options for dealing with existing environment variables
        """
        _overwrite = kwargs.get("overwrite", True)
        _prepend = kwargs.get("prepend", False)
        _append = kwargs.get("append", False)
        _join_char = kwargs.get("join_char", ":")

        # only overwrite if env_val evaluates as true
        _overwrite_if_true = kwargs.get("overwrite_if_true", False)
        # only overwrite if env_val evaluates as false
        _overwrite_if_false = kwargs.get("overwrite_if_false", False)

        _formatter = kwargs.get(
            "formatter",
            lambda x: f"{x}" if not isinstance(x, bool) else "1" if x else "0",
        )

        for itr in kwargs.keys():
            if itr not in (
                "overwrite",
                "prepend",
                "append",
                "join_char",
                "overwrite_if_true",
                "overwrite_if_false",
                "formatter",
            ):
                fatal_error(
                    f"Internal error in update_env('{env_var}', {env_val}, {itr}={kwargs[itr]}). Invalid key: {itr}"
                )

        if env_val is None:
            return app_env.get(env_var, None)

        _val = _formatter(env_val)
        _curr_val = app_env.get(env_var, None)

        def _write_env_value():
            if _overwrite_if_true:
                if bool(env_val) is True:
                    app_env[env_var] = _val
            elif _overwrite_if_false:
                if bool(env_val) is False:
                    app_env[env_var] = _val
            else:
                app_env[env_var] = _val

        if _curr_val is not None:
            if not _overwrite:
                pass
            elif _prepend:
                app_env[env_var] = (
                    "{}{}{}".format(_val, _join_char, _curr_val) if _val else _curr_val
                ).strip(":")
            elif _append:
                app_env[env_var] = (
                    "{}{}{}".format(_curr_val, _join_char, _val) if _val else _curr_val
                ).strip(":")
            elif _overwrite:
                _write_env_value()
        else:
            _write_env_value()

        return app_env.get(env_var, None)

    update_env("ROCPROFILER_LIBRARY_CTOR", True)

    ROCPROFV3_DIR = os.path.dirname(os.path.realpath(__file__))
    ROCM_DIR = os.path.dirname(ROCPROFV3_DIR)
    ROCPROF_TOOL_LIBRARY = f"{ROCM_DIR}/lib/rocprofiler-sdk/librocprofiler-sdk-tool.so"
    ROCPROF_SDK_LIBRARY = f"{ROCM_DIR}/lib/librocprofiler-sdk.so"
    ROCPROF_ROCTX_LIBRARY = f"{ROCM_DIR}/lib/librocprofiler-sdk-roctx.so"
    ROCPROF_KOKKOSP_LIBRARY = (
        f"{ROCM_DIR}/lib/rocprofiler-sdk/librocprofiler-sdk-tool-kokkosp.so"
    )
    ROCPROF_LIST_AVAIL_TOOL_LIBRARY = (
        f"{ROCM_DIR}/libexec/rocprofiler-sdk/librocprofv3-list-avail.so"
    )

    prepend_preload = [itr for itr in args.preload if itr]
    append_preload = [
        ROCPROF_TOOL_LIBRARY,
        ROCPROF_LIST_AVAIL_TOOL_LIBRARY,
        ROCPROF_SDK_LIBRARY,
    ]

    update_env("LD_PRELOAD", ":".join(prepend_preload), prepend=True)
    update_env("LD_PRELOAD", ":".join(append_preload), append=True)

    update_env(
        "ROCP_TOOL_LIBRARIES",
        f"{ROCPROF_TOOL_LIBRARY}",
        append=True,
    )
    update_env(
        "LD_LIBRARY_PATH",
        f"{ROCM_DIR}/lib",
        append=True,
    )

    _output_file = args.output_file
    _output_path = (
        args.output_directory if args.output_directory is not None else os.getcwd()
    )

    update_env("ROCPROF_OUTPUT_FILE_NAME", _output_file)
    update_env("ROCPROF_OUTPUT_PATH", _output_path)
    if app_pass is not None and args.sub_directory is not None:
        app_env["ROCPROF_OUTPUT_PATH"] = os.path.join(
            f"{_output_path}", f"{args.sub_directory}{app_pass}"
        )

    if args.output_file is not None or args.output_directory is not None:
        update_env("ROCPROF_OUTPUT_LIST_AVAIL_FILE", True)

    if not args.output_format:
        args.output_format = ["csv"]

    update_env(
        "ROCPROF_OUTPUT_FORMAT", ",".join(args.output_format), append=True, join_char=","
    )

    if args.kokkos_trace:
        update_env("KOKKOS_TOOLS_LIBS", ROCPROF_KOKKOSP_LIBRARY, append=True)
        for itr in (
            "marker_trace",
            "kernel_rename",
        ):
            setattr(args, itr, True)

    if args.sys_trace:
        for itr in (
            "hip_trace",
            "hsa_trace",
            "marker_trace",
            "kernel_trace",
            "memory_copy_trace",
            "memory_allocation_trace",
            "scratch_memory_trace",
            "rccl_trace",
            "rocdecode_trace",
        ):
            setattr(args, itr, True)

    if args.runtime_trace:
        for itr in (
            "hip_runtime_trace",
            "marker_trace",
            "kernel_trace",
            "memory_copy_trace",
            "memory_allocation_trace",
            "scratch_memory_trace",
            "rccl_trace",
            "rocdecode_trace",
        ):
            setattr(args, itr, True)

    if args.hip_trace:
        for itr in ("compiler", "runtime"):
            setattr(args, f"hip_{itr}_trace", True)

    if args.hsa_trace:
        for itr in ("core", "amd", "image", "finalizer"):
            setattr(args, f"hsa_{itr}_trace", True)

    trace_count = 0
    trace_opts = ["--hip-trace", "--hsa-trace"]
    for opt, env_val in dict(
        [
            ["hip_compiler_trace", "HIP_COMPILER_API_TRACE"],
            ["hip_runtime_trace", "HIP_RUNTIME_API_TRACE"],
            ["hsa_core_trace", "HSA_CORE_API_TRACE"],
            ["hsa_amd_trace", "HSA_AMD_EXT_API_TRACE"],
            ["hsa_image_trace", "HSA_IMAGE_EXT_API_TRACE"],
            ["hsa_finalizer_trace", "HSA_FINALIZER_EXT_API_TRACE"],
            ["marker_trace", "MARKER_API_TRACE"],
            ["rccl_trace", "RCCL_API_TRACE"],
            ["rocdecode_trace", "ROCDECODE_API_TRACE"],
            ["kernel_trace", "KERNEL_TRACE"],
            ["memory_copy_trace", "MEMORY_COPY_TRACE"],
            ["memory_allocation_trace", "MEMORY_ALLOCATION_TRACE"],
            ["scratch_memory_trace", "SCRATCH_MEMORY_TRACE"],
        ]
    ).items():
        val = getattr(args, f"{opt}")
        update_env(f"ROCPROF_{env_val}", val, overwrite_if_true=True)
        trace_count += 1 if val else 0
        trace_opts += ["--{}".format(opt.replace("_", "-"))]

    # if marker tracing was requested, LD_PRELOAD the rocprofiler-sdk-roctx library
    # to override the roctx symbols of an app linked to the old roctracer roctx
    if args.marker_trace and not args.suppress_marker_preload:
        update_env("LD_PRELOAD", ROCPROF_ROCTX_LIBRARY, append=True)

    if trace_count == 0:
        # if no tracing was enabled but the options below were enabled, raise an error
        for oitr in [
            "stats",
            "summary",
            "summary-per-domain",
            "summary-groups",
            "summary-output-file",
            "summary-units",
        ]:
            _attr = oitr.replace("-", "_")
            if not hasattr(args, _attr):
                fatal_error(
                    f"Internal error. parser does not support --{oitr} argument (i.e. args.{_attr})"
                )
            elif getattr(args, _attr):
                _len = max([len(f"{key}") for key in args.keys()])
                _args = "\n\t".join(
                    sorted([f"{key:<{_len}} = {val}" for key, val in args.items()])
                )
                fatal_error(
                    "No tracing options were enabled for --{} option.\nConfiguration argument values:\n\t{}\nTracing options:\n\t{}".format(
                        oitr, f"{_args}", "\n\t".join(trace_opts)
                    )
                )

    _summary_groups = "##@@##".join(args.summary_groups) if args.summary_groups else None
    _summary_output_fname = args.summary_output_file
    if _summary_output_fname is None:
        _summary_output_fname = "stderr"
    elif _summary_output_fname.lower() in ("stdout", "stderr"):
        _summary_output_fname = _summary_output_fname.lower()

    update_env("ROCPROF_STATS", args.stats, overwrite_if_true=True)
    update_env("ROCPROF_STATS_SUMMARY", args.summary, overwrite_if_true=True)
    update_env("ROCPROF_STATS_SUMMARY_UNITS", args.summary_units, overwrite=True)
    update_env("ROCPROF_STATS_SUMMARY_OUTPUT", _summary_output_fname, overwrite=True)
    update_env("ROCPROF_STATS_SUMMARY_GROUPS", _summary_groups, overwrite=True)
    update_env(
        "ROCPROF_STATS_SUMMARY_PER_DOMAIN",
        args.summary_per_domain,
        overwrite_if_true=True,
    )
    update_env(
        "ROCPROF_DEMANGLE_KERNELS",
        not args.mangled_kernels,
        overwrite_if_false=True,
    )
    update_env(
        "ROCPROF_TRUNCATE_KERNELS",
        args.truncate_kernels,
        overwrite_if_true=True,
    )
    update_env(
        "ROCPROF_LIST_AVAIL",
        args.list_avail,
        overwrite_if_true=True,
    )
    if args.collection_period:
        factors = {
            "hour": 60 * 60 * 1e9,
            "min": 60 * 1e9,
            "sec": 1e9,
            "msec": 1e6,
            "usec": 1e3,
            "nsec": 1,
        }

        def to_nanosec(val):
            return int(float(val) * factors[args.collection_period_unit[0]])

        def convert_triplet(delay, duration, repeat):
            return ":".join(
                [
                    f"{itr}"
                    for itr in [to_nanosec(delay), to_nanosec(duration), int(repeat)]
                ]
            )

        periods = [convert_triplet(*itr.split(":")) for itr in args.collection_period]
        update_env(
            "ROCPROF_COLLECTION_PERIOD",
            ";".join(periods),
            overwrite_if_true=True,
        )

    if args.log_level and args.log_level not in ("env"):
        for itr in ("ROCPROF", "ROCPROFILER", "ROCTX"):
            update_env(
                f"{itr}_LOG_LEVEL",
                args.log_level,
            )

    for opt, env_val in dict(
        [
            ["kernel_rename", "KERNEL_RENAME"],
        ]
    ).items():
        val = getattr(args, f"{opt}")
        if val is not None:
            update_env(f"ROCPROF_{env_val}", val, overwrite_if_true=True)

    for opt, env_val in dict(
        [
            ["perfetto_buffer_size", "PERFETTO_BUFFER_SIZE_KB"],
            ["perfetto_shmem_size_hint", "PERFETTO_SHMEM_SIZE_HINT_KB"],
            ["perfetto_fill_policy", "PERFETTO_BUFFER_FILL_POLICY"],
            ["perfetto_backend", "PERFETTO_BACKEND"],
        ]
    ).items():
        val = getattr(args, f"{opt}")
        if val is not None:
            if isinstance(val, (list, tuple, set)):
                val = ", ".join(val)
            update_env(f"ROCPROF_{env_val}", val, overwrite=True)

    def log_config(_env):
        existing_env = dict(os.environ)
        init_message = "\n- rocprofv3 configuration{}:\n".format(
            "" if app_pass is None else f" (pass {app_pass})"
        )
        for key, itr in _env.items():
            if key not in existing_env.keys():
                if init_message:
                    sys.stderr.write(init_message)
                    init_message = None
                sys.stderr.write(f"\t- {key}={itr}\n")
        if init_message is None:
            sys.stderr.write("\n")
        sys.stderr.flush()

    if args.list_avail:
        update_env("ROCPROFILER_PC_SAMPLING_BETA_ENABLED", "on")
        path = os.path.join(f"{ROCM_DIR}", "bin/rocprofv3_avail")
        if app_args:
            exit_code = subprocess.check_call(["python3", path], env=app_env)
        else:
            app_args = ["python3", path]

    elif not app_args:
        log_config(app_env)
        fatal_error("No application provided")

    if args.kernel_include_regex:
        update_env(
            "ROCPROF_KERNEL_FILTER_INCLUDE_REGEX",
            args.kernel_include_regex,
        )

    if args.kernel_exclude_regex:
        update_env(
            "ROCPROF_KERNEL_FILTER_EXCLUDE_REGEX",
            args.kernel_exclude_regex,
        )

    if args.kernel_iteration_range:
        update_env("ROCPROF_KERNEL_FILTER_RANGE", ", ".join(args.kernel_iteration_range))

    if args.extra_counters is not None:
        with open(args.extra_counters, "r") as e_file:
            e_file_contents = e_file.read()
            update_env("ROCPROF_EXTRA_COUNTERS_CONTENTS", e_file_contents, overwrite=True)

    if args.pmc:
        update_env("ROCPROF_COUNTER_COLLECTION", True, overwrite=True)
        update_env(
            "ROCPROF_COUNTERS", "pmc: {}".format(" ".join(args.pmc)), overwrite=True
        )
    else:
        update_env("ROCPROF_COUNTER_COLLECTION", False, overwrite=True)

    if args.log_level in ("info", "trace", "env"):
        log_config(app_env)

    if args.pc_sampling_unit or args.pc_sampling_method or args.pc_sampling_interval:

        if (
            not args.pc_sampling_beta_enabled
            and os.environ.get("ROCPROFILER_PC_SAMPLING_BETA_ENABLED", None) is None
        ):
            fatal_error(
                "PC sampling unavailable. The feature is implicitly disabled. To enable it, use --pc-sampling-beta-enable option or set ROCPROFILER_PC_SAMPLING_BETA_ENABLED=ON in the environment"
            )

        update_env(
            "ROCPROFILER_PC_SAMPLING_BETA_ENABLED",
            args.pc_sampling_beta_enabled,
            overwrite_if_true=True,
        )

        if not (
            args.pc_sampling_unit
            and args.pc_sampling_method
            and args.pc_sampling_interval
        ):
            fatal_error("All three PC sampling configurations need to be set")

        if args.pc_sampling_interval <= 0:
            fatal_error("PC sampling interval must be a positive number.")

        update_env("ROCPROF_PC_SAMPLING_UNIT", args.pc_sampling_unit)
        update_env("ROCPROF_PC_SAMPLING_METHOD", args.pc_sampling_method)
        update_env("ROCPROF_PC_SAMPLING_INTERVAL", args.pc_sampling_interval)

    if args.advanced_thread_trace:

        def int_auto(num_str):
            if "0x" in num_str:
                return int(num_str, 16)
            else:
                return int(num_str, 10)

        if args.pmc or (
            args.pc_sampling_beta_enabled
            or args.pc_sampling_unit
            or args.pc_sampling_method
            or args.pc_sampling_interval
        ):
            fatal_error(
                "Advanced thread trace cannot be enabled with counter collection or pc sampling"
            )

        if not args.att_parse:
            fatal_error("provide the parser choice")

        update_env("ROCPROF_ADVANCED_THREAD_TRACE", True, overwrite=True)
        update_env("ROCPROF_ATT_CAPABILITY", args.att_parse, overwrite=True)

        if args.att_target_cu:
            update_env("ROCPROF_ATT_PARAM_TARGET_CU", args.att_target_cu, overwrite=True)

        if args.att_shader_engine_mask:
            update_env(
                "ROCPROF_ATT_PARAM_SHADER_ENGINE_MASK",
                int_auto(args.att_shader_engine_mask),
                overwrite=True,
            )
        if args.att_buffer_size:
            update_env(
                "ROCPROF_ATT_PARAM_BUFFER_SIZE",
                int_auto(args.att_buffer_size),
                overwrite=True,
            )
        if args.att_simd_select:
            update_env(
                "ROCPROF_ATT_PARAM_SIMD_SELECT",
                int_auto(args.att_simd_select),
                overwrite=True,
            )
        if args.att_serialize_all:
            update_env(
                "ROCPROF_ATT_PARAM_SERIALIZE_ALL",
                args.att_serialize_all,
                overwrite=True,
            )

        if args.att_library_path:

            update_env(
                "ROCPROF_ATT_LIBRARY_PATH",
                args.att_library_path,
                overwrite=True,
            )
            update_env(
                "ATT_LIBRARY_PATH",
                args.att_library_path,
                overwrite=True,
            )
        if args.att_percounters:
            update_env(
                "ROCPROF_ATT_PARAM_PERFCOUNTERS",
                " ".join(args.att_perfcounters),
                overwrite=True,
            )

    if use_execv:
        # does not return
        os.execvpe(app_args[0], app_args, env=app_env)
    else:
        try:
            exit_code = subprocess.check_call(app_args, env=app_env)
            if exit_code != 0:
                fatal_error("Application exited with non-zero exit code", exit_code)
        except Exception as e:
            fatal_error(f"{e}\n")
        return exit_code


def check_att_path_parse_method(args, index, support_att_input, att_parse_supported):

    if not att_parse_supported:
        if index not in support_att_input.keys():
            fatal_error(
                f"Advanced_thread_trace enabled but no decoder library found in cmdline/env paths and att_library_path not set for pass-{index + 1}"
            )
        elif not support_att_input[index]:
            fatal_error(
                f"Advanced_thread_trace enabled but no decoder library found in att_library_path for pass-{index + 1}"
            )
        else:
            if args.att_parse and args.att_parse not in support_att_input[index]:
                fatal_error(
                    f"Advanced_thread_trace enabled but decoder library for requested parse method not found in att_library_path for pass-{index + 1}"
                )
    else:
        if index in support_att_input.keys() and not support_att_input[index]:
            fatal_error(
                f"Advanced_thread_trace enabled but no decoder library found in att_library_path for pass-{index + 1}"
            )

        elif index not in support_att_input.keys():
            if args.att_parse and args.att_parse not in att_parse_supported:
                fatal_error(
                    "Advanced_thread_trace enabled but decoder library for requested parse method not found"
                )
        else:
            if args.att_parse and args.att_parse not in support_att_input[index]:
                fatal_error(
                    f"Advanced_thread_trace enabled but decoder library for requested parse method not found for pass-{index + 1}"
                )


def main(argv=None):

    # att_parse_supported is valid path for decoder in env or commandline arg
    # support_att_input is a dict, where key is a pass index with value being a valid decoder path
    cmd_args, app_args, att_parse_supported, support_att_input = parse_arguments(argv)
    inp_args = (
        parse_input(cmd_args.input) if getattr(cmd_args, "input") else [dotdict({})]
    )

    if len(inp_args) == 1:
        args = get_args(cmd_args, inp_args[0])
        pass_idx = None
        if has_set_attr(args, "pmc") and len(args.pmc) > 0:
            pass_idx = 1
        if args.advanced_thread_trace:
            check_att_path_parse_method(args, 0, support_att_input, att_parse_supported)
        run(app_args, args, pass_id=pass_idx)
    else:
        for idx, itr in enumerate(inp_args):
            args = get_args(cmd_args, itr)
            if args.advanced_thread_trace:
                check_att_path_parse_method(
                    args, idx, support_att_input, att_parse_supported
                )
            run(
                app_args,
                args,
                pass_id=(idx + 1),
                use_execv=False,
            )


if __name__ == "__main__":
    ec = main(sys.argv[1:])
    sys.exit(ec)
