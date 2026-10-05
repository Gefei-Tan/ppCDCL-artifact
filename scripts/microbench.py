import argparse
import csv
import itertools
import math
import os
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from bench_utils import (
    PPCDCL_ROOT,
    WATCH_BACKEND_CHOICES,
    normalize_watch_backend,
    parse_good_bench_watch_backend,
    resolve_repo_path,
)

# Plotting dependencies are optional. Benchmark collection and CSV output do
# not need pandas or matplotlib; make_plots() imports them only when called.
plt = None
pd = None
LogFormatter = None
LogLocator = None
NullFormatter = None

# Columns of the earliest CSV schema. They are listed explicitly rather than
# sliced from CSV_COLUMNS, so that adding a column cannot make a damaged CSV
# look like a valid older one.
LEGACY_CSV_COLUMNS = [
    "var_num", "clause_num",
    "max_num_of_con_clause",
    "max_lit_in_clause",
    "max_lit_in_con_clause",
    "max_clause_in_wl",
    "conflict_factor",
    "orange_num", "yellow_num", "blue_num", "red_num",
    # times
    "decision_time",
    "from_u_time",
    "conflict_time",
    "check_clause_time",
    "update_wl_time",
    "add_implication_time",
    "regular_total_time",
    "total_time",
    # gates (per op)
    "decision_gates",
    "from_u_gates",
    "conflict_gates",
    "check_clause_gates",
    "update_wl_gates",
    "add_implication_gates",
    "regular_total_gates",
    "total_gates",
]

BENCHMARK_DIAGNOSTIC_COLUMNS = [
    "benchmark_setup_time",
    "benchmark_setup_gates",
    "benchmark_pair_wall_time",
]

EXECUTION_METADATA_V1_COLUMNS = [
    "global_sample_factor",
    "conflict_sample_factor",
    "uip_loop_cap",
    "variable_state_backend",
    "backend_threads",
    "watchlist_policy",
    "base_port",
]

EXECUTION_METADATA_V2_COLUMNS = EXECUTION_METADATA_V1_COLUMNS + [
    "timeout_seconds",
]

WATCH_BACKEND_METADATA_COLUMNS = [
    "watch_backend_requested",
    "watch_backend_effective",
    "watch_backend_reason",
    "watch_backend_occ_bits",
    "watch_backend_pair_bits",
]

EXECUTION_METADATA_COLUMNS = (
    EXECUTION_METADATA_V2_COLUMNS + WATCH_BACKEND_METADATA_COLUMNS
)

CSV_COLUMNS = (
    LEGACY_CSV_COLUMNS
    + BENCHMARK_DIAGNOSTIC_COLUMNS
    + EXECUTION_METADATA_COLUMNS
)

# CSV schemas that this script has written. A header with recognized column
# names in any other order or combination comes from a truncated or hand-edited
# file and is not upgraded.
KNOWN_CSV_SCHEMAS = (
    tuple(LEGACY_CSV_COLUMNS),
    tuple(LEGACY_CSV_COLUMNS + EXECUTION_METADATA_V1_COLUMNS),
    tuple(
        LEGACY_CSV_COLUMNS
        + BENCHMARK_DIAGNOSTIC_COLUMNS[:2]
        + EXECUTION_METADATA_V1_COLUMNS
    ),
    tuple(
        LEGACY_CSV_COLUMNS
        + BENCHMARK_DIAGNOSTIC_COLUMNS
        + EXECUTION_METADATA_V1_COLUMNS
    ),
    tuple(
        LEGACY_CSV_COLUMNS
        + BENCHMARK_DIAGNOSTIC_COLUMNS
        + EXECUTION_METADATA_V2_COLUMNS
    ),
    tuple(CSV_COLUMNS),
)

PARAM_COLUMNS = [
    "var_num", "clause_num",
    "max_num_of_con_clause",
    "max_lit_in_clause",
    "max_lit_in_con_clause",
    "max_clause_in_wl",
    "conflict_factor",
    "global_sample_factor",
    "conflict_sample_factor",
    "uip_loop_cap",
    "variable_state_backend",
    "backend_threads",
    "watchlist_policy",
    *WATCH_BACKEND_METADATA_COLUMNS,
    "base_port",
    "timeout_seconds",
    "orange_num", "yellow_num", "blue_num", "red_num",
]
METRIC_COLUMNS = [c for c in CSV_COLUMNS if c not in PARAM_COLUMNS]

DEFAULT_BIN = "./bin/test_good_bench"
DEFAULT_VAR_LIST = [100,1000,5000,10000,50000]
DEFAULT_CLAUSE_LIST = [100,5000, 10000, 50000, 100000]

DEFAULT_VAR_LIST = [100, 1000, 10000, 100000]
DEFAULT_CLAUSE_LIST = [100, 1000, 5000, 10000, 50000]

DEFAULT_VAR_LIST = [5000,10000,50000]
DEFAULT_CLAUSE_LIST = [10000,50000,100000]


DEFAULT_ORANGE_NUM = 100
DEFAULT_YELLOW_NUM = 100
DEFAULT_BLUE_NUM = 100
DEFAULT_RED_NUM = 2
DEFAULT_CONFLICT_FACTOR = 2

DEFAULT_MAX_NUM_OF_CON_CLAUSE = 10000
DEFAULT_MAX_LIT_IN_CLAUSE = 10
DEFAULT_MAX_LIT_IN_CON_CLAUSE = 10
DEFAULT_MAX_CLAUSE_IN_WL = 30

DEFAULT_RUNS = 2
DEFAULT_GLOBAL_SAMPLE_FACTOR = 1
DEFAULT_CONFLICT_SAMPLE_FACTOR = 1
DEFAULT_UIP_LOOP_CAP = 20
DEFAULT_BASE_PORT = 10086
DEFAULT_BACKEND_THREADS = 4
DEFAULT_TIMEOUT_SECONDS = 600.0
VAR_LABEL = "Number of Variables $(n)$"
CLAUSE_LABEL = "Number of Clauses ($m$)"
LEGEND_CLAUSE_LABEL = "$m$"
SWEEP_DIR_PATTERN = re.compile(
    r"^n_(?P<vars>[0-9-]+)"
    r"_m_(?P<clauses>[0-9-]+)"
    r"_orange_(?P<orange>\d+)"
    r"_yellow_(?P<yellow>\d+)"
    r"_blue_(?P<blue>\d+)"
    r"_red_(?P<red>\d+)"
    r"_conf_(?P<conflict>\d+)"
    r"_const_(?P<const_num>\d+)_(?P<const_lit>\d+)_(?P<const_lit_con>\d+)_(?P<wl>\d+)"
    r"_runs_(?P<runs>\d+)$"
)
FONT_FAMILY = "Times New Roman"
TICK_FONT_SIZE = 42
LABEL_FONT_SIZE = 50
LEGEND_FONT_SIZE = 45
TITLE_FONT_SIZE = 54
FIGSIZE_INDIV = (7,7*0.618)
FIGSIZE_COMBINED = (20, 22*0.618)
LEGEND_NCOL_INDIV = None
LEGEND_NROW_INDIV = None
LEGEND_NCOL_COMBINED = 6
LEGEND_NROW_COMBINED = None

LEGEND_PAD = 0.4
LEGEND_LABEL_SPACING = 0.1
LEGEND_HANDLE_TEXT_PAD = 0.2
LEGEND_COLUMN_SPACING = 0.8

SUBPLOT_WSPACE = 0.1
SUBPLOT_HSPACE = 0.03
Y_AXIS_SLACK = 0.3  # proportional slack for axis limits to leave headroom
X_AXIS_SLACK = 0.1
LINE_WIDTH = 5.0


def _safe_float(value) -> float:
    """Convert to float, treating None/NaN as 0.0."""
    if value is None:
        return 0.0
    try:
        result = float(value)
    except (TypeError, ValueError):
        return 0.0
    return 0.0 if math.isnan(result) else result


def _parse_good_bench_summary(
    stdout: str,
    requested_watch_backend: str = "auto",
) -> dict:
    """Extract the SUMMARY line emitted by test_good_bench (ALICE only)."""
    setup_match = re.search(
        r"^BENCH_SETUP seconds=([0-9eE+.\-]+) "
        r"gates=([0-9eE+.\-]+) included_in_summary=0$",
        stdout,
        re.MULTILINE,
    )
    conflict_gate_match = re.search(
        r"^BENCH_CONFLICT_GATES per_block=([0-9eE+.\-]+)$",
        stdout,
        re.MULTILINE,
    )
    for line in stdout.splitlines():
        if line.startswith("SUMMARY"):
            parts = line.strip().split()
            # SUMMARY total dec fromU conf regular check update imp yellow blue red orange
            if len(parts) != 13:
                raise ValueError(f"Malformed SUMMARY line (expected 13 tokens): {line}")
            time_keys = [
                "total_time",
                "decision_time",
                "from_u_time",
                "conflict_time",
                "regular_total_time",
                "check_clause_time",
                "update_wl_time",
                "add_implication_time",
            ]
            count_keys = ["yellow_total", "blue_total", "red_total", "orange_total"]
            values = parts[1:]
            summary = {}
            for key, val in zip(time_keys, values[: len(time_keys)]):
                summary[key] = float(val)
            for key, val in zip(count_keys, values[len(time_keys) :]):
                summary[key] = int(float(val))
            summary["benchmark_setup_time"] = (
                float(setup_match.group(1)) if setup_match else None
            )
            summary["benchmark_setup_gates"] = (
                float(setup_match.group(2)) if setup_match else None
            )
            summary["conflict_gates"] = (
                float(conflict_gate_match.group(1))
                if conflict_gate_match else None
            )
            summary.update(
                parse_good_bench_watch_backend(
                    stdout,
                    expected_requested=requested_watch_backend,
                )
            )
            return summary
    raise ValueError("SUMMARY line not found in ALICE stdout; cannot parse good_bench output")


def write_csv_header(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        with open(path, "w", newline="") as f:
            csv.writer(f).writerow(CSV_COLUMNS)
        return

    with open(path, newline="") as source:
        reader = csv.DictReader(source)
        existing_columns = reader.fieldnames or []
        if existing_columns == CSV_COLUMNS:
            return
        if tuple(existing_columns) not in KNOWN_CSV_SCHEMAS:
            unknown = [
                column for column in existing_columns if column not in CSV_COLUMNS
            ]
            raise ValueError(
                f"Cannot upgrade {path}: unsupported or damaged CSV schema; "
                f"unrecognized columns={unknown}. "
                "Use a new --csv-path."
            )

        # Upgrade a CSV with an older schema: keep every existing cell, add the
        # new columns as blanks, and atomically replace the file before a new
        # row is appended.
        upgraded_path = path.with_name(f".{path.name}.schema-upgrade.tmp")
        with open(upgraded_path, "w", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=CSV_COLUMNS)
            writer.writeheader()
            for row in reader:
                writer.writerow(row)
    os.replace(upgraded_path, path)


def append_row(path: Path, row: dict):
    with open(path, "a", newline="") as f:
        csv.DictWriter(f, fieldnames=CSV_COLUMNS).writerow(row)


def build_config_row(var_num: int, clause_num: int, args) -> dict:
    """Return the public benchmark parameters recorded with every result row."""
    return {
        "var_num": var_num,
        "clause_num": clause_num,
        "max_num_of_con_clause": args.max_num_of_con_clause,
        "max_lit_in_clause": args.max_lit_in_clause,
        "max_lit_in_con_clause": args.max_lit_in_con_clause,
        "max_clause_in_wl": args.max_clause_in_wl,
        "conflict_factor": args.conflict_factor,
        "global_sample_factor": args.global_sample_factor,
        "conflict_sample_factor": args.conflict_sample_factor,
        "uip_loop_cap": args.uip_loop_cap,
        "variable_state_backend": "oram" if args.use_oram_v else "linear",
        "backend_threads": args.backend_threads,
        "watchlist_policy": "shortest" if args.use_shortest_wl else "first-fit",
        "watch_backend_requested": normalize_watch_backend(args.watch_backend),
        "watch_backend_effective": "",
        "watch_backend_reason": "",
        "watch_backend_occ_bits": "",
        "watch_backend_pair_bits": "",
        "base_port": args.base_port,
        "timeout_seconds": args.timeout_seconds,
        "orange_num": args.orange_num,
        "yellow_num": args.yellow_num,
        "blue_num": args.blue_num,
        "red_num": args.red_num,
    }


def build_good_bench_commands(var_num: int, clause_num: int, args) -> Tuple[List[str], List[str]]:
    """Build matched ALICE/BOB commands without starting either MPC party."""
    common_args = [
        str(var_num),
        str(clause_num),
        str(args.max_num_of_con_clause),
        str(args.max_lit_in_clause),
        str(args.max_lit_in_con_clause),
        str(args.max_clause_in_wl),
        str(args.conflict_factor),
        str(args.global_sample_factor),
        str(args.conflict_sample_factor),
        str(args.orange_num),
        str(args.yellow_num),
        str(args.blue_num),
        str(args.red_num),
        str(args.uip_loop_cap),
    ]
    policy_args = [
        "--oramV" if args.use_oram_v else "--no-oramV",
        "--shortest-wl" if args.use_shortest_wl else "--no-shortest-wl",
        "--watch-backend",
        normalize_watch_backend(args.watch_backend),
        "--threads",
        str(args.backend_threads),
    ]
    alice = [str(args.binary), "1", str(args.base_port), *common_args, *policy_args]
    bob = [str(args.binary), "2", str(args.base_port), *common_args, *policy_args]
    return alice, bob


def run_one_config(var_num: int, clause_num: int, args):
    """
    Run test_good_bench once for a (var_num, clause_num) pair and return a result row.
    Both parties run as subprocesses; the SUMMARY line is parsed from ALICE's stdout.
    """
    cmd1, cmd2 = build_good_bench_commands(var_num, clause_num, args)

    pair_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=2) as pool:
        f1 = pool.submit(
            subprocess.run, cmd1,
            capture_output=True, text=True, cwd=PPCDCL_ROOT,
            timeout=args.timeout_seconds,
        )
        f2 = pool.submit(
            subprocess.run, cmd2,
            capture_output=True, text=True, cwd=PPCDCL_ROOT,
            timeout=args.timeout_seconds,
        )
        res1 = f1.result()
        res2 = f2.result()
        print(" ".join(cmd1))
    pair_wall_time = time.perf_counter() - pair_start
        
    for label, res in (("ALICE", res1), ("BOB", res2)):
        if res.returncode != 0:
            raise RuntimeError(
                f"Benchmark failed for n={var_num}, m={clause_num} ({label}).\n"
                f"stdout:\n{res.stdout}\nstderr:\n{res.stderr}"
            )

    summary = _parse_good_bench_summary(
        res1.stdout,
        requested_watch_backend=args.watch_backend,
    )

    # Check that the binary echoed back the block counts that were sent.
    expected_counts = {
        "orange_total": args.orange_num,
        "yellow_total": args.yellow_num,
        "blue_total": args.blue_num,
        "red_total": args.red_num,
    }
    for field, expected in expected_counts.items():
        if summary[field] != expected:
            raise ValueError(
                f"Mismatch between input and good_bench output for {field}: "
                f"sent {expected}, received {summary[field]}"
            )

    row = {
        **build_config_row(var_num, clause_num, args),
        "decision_time": summary["decision_time"],
        "from_u_time": summary["from_u_time"],
        "conflict_time": summary["conflict_time"],
        "check_clause_time": summary["check_clause_time"],
        "update_wl_time": summary["update_wl_time"],
        "add_implication_time": summary["add_implication_time"],
        "regular_total_time": summary["regular_total_time"],
        "total_time": summary["total_time"],
        "benchmark_setup_time": summary["benchmark_setup_time"],
        "benchmark_setup_gates": summary["benchmark_setup_gates"],
        "watch_backend_requested": summary["watch_backend_requested"],
        "watch_backend_effective": summary["watch_backend_effective"],
        "watch_backend_reason": summary["watch_backend_reason"],
        "watch_backend_occ_bits": summary["watch_backend_occ_bits"],
        "watch_backend_pair_bits": summary["watch_backend_pair_bits"],
        # This includes process/network/backend startup, the sampled benchmark,
        # and teardown for the concurrently launched pair. Unlike SUMMARY it
        # is an observed invocation time, not an extrapolated solver total.
        "benchmark_pair_wall_time": pair_wall_time,
        # good_bench reports only the conflict gate count; the other gate
        # fields stay blank rather than being set to zero.
        "decision_gates": None,
        "from_u_gates": None,
        "conflict_gates": summary["conflict_gates"],
        "check_clause_gates": None,
        "update_wl_gates": None,
        "add_implication_gates": None,
        "regular_total_gates": None,
        "total_gates": None,
    }

    return row


def average_runs_for_config(var_num: int, clause_num: int, args, final_csv: Path):
    rows = [run_one_config(var_num, clause_num, args) for _ in range(args.runs)]

    if not rows:
        raise RuntimeError(f"No data collected for n={var_num}, m={clause_num}")

    row = build_config_row(var_num, clause_num, args)
    for column in WATCH_BACKEND_METADATA_COLUMNS:
        values = {
            run.get(column)
            for run in rows
            if run.get(column) not in (None, "")
        }
        if len(values) > 1:
            raise ValueError(
                f"Watch backend metadata changed across repeated runs for {column}: "
                f"{sorted(str(value) for value in values)}"
            )
        if values:
            row[column] = values.pop()
    for col in METRIC_COLUMNS:
        values = [
            _safe_float(run[col])
            for run in rows
            if run.get(col) is not None
        ]
        row[col] = sum(values) / len(values) if values else None

    append_row(final_csv, row)


def run_all_configs(args, csv_path: Path):
    write_csv_header(csv_path)
    for clause_num in args.clauses:
        for var_num in args.vars:
            print(f"Running n={var_num}, m={clause_num} (runs={args.runs}) ...")
            average_runs_for_config(var_num, clause_num, args, csv_path)


def _load_plot_dependencies() -> bool:
    """Load optional plotting packages without making collection depend on them."""
    global plt, pd, LogFormatter, LogLocator, NullFormatter
    if plt is not None and pd is not None:
        return True
    try:
        import matplotlib.pyplot as matplotlib_pyplot
        import pandas as pandas_module
        from matplotlib.ticker import (
            LogFormatter as MatplotlibLogFormatter,
            LogLocator as MatplotlibLogLocator,
            NullFormatter as MatplotlibNullFormatter,
        )
    except ModuleNotFoundError as exc:
        print(
            f"[plot] optional dependency unavailable ({exc.name}); "
            "benchmark CSV was written, skipping plots"
        )
        return False
    plt = matplotlib_pyplot
    pd = pandas_module
    LogFormatter = MatplotlibLogFormatter
    LogLocator = MatplotlibLogLocator
    NullFormatter = MatplotlibNullFormatter
    return True


def _set_plot_style():
    plt.rcParams.update({
        "font.family": FONT_FAMILY,
        "font.serif": [FONT_FAMILY],
        "mathtext.fontset": "custom",
        "mathtext.rm": FONT_FAMILY,
        "mathtext.it": FONT_FAMILY,
        "mathtext.bf": FONT_FAMILY,
        "axes.labelsize": LABEL_FONT_SIZE,
        "axes.titlesize": TITLE_FONT_SIZE,
        "xtick.labelsize": TICK_FONT_SIZE,
        "ytick.labelsize": TICK_FONT_SIZE,
        "legend.fontsize": LEGEND_FONT_SIZE,
        "text.usetex": False,
    })


def _legend_columns(num_series: int, override_ncol, override_nrow, default_ncol: int):
    if override_ncol is not None:
        return override_ncol
    if override_nrow is not None and override_nrow > 0:
        return max(1, math.ceil(num_series / override_nrow))
    return default_ncol


def make_plots(csv_path):
    if not _load_plot_dependencies():
        return
    df = pd.read_csv(csv_path)
    df = df.sort_values(["clause_num", "var_num"])
    for col in METRIC_COLUMNS:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    _set_plot_style()
    x_min_raw = df["clause_num"].min()
    x_max_raw = df["clause_num"].max()
    x_min = 10 ** int(math.floor(math.log10(x_min_raw)))
    x_max = x_max_raw

    # time plots
    time_columns = {
        "decision_time": "Decision",
        "from_u_time": "Get Pending Unit Literal",
        "conflict_time": "Conflict Analysis",
        "regular_total_time": "Unit Propagation",
        "check_clause_time": "Check Clause",
        "update_wl_time": "Update WL",
        "add_implication_time": "Add Implication",
    }

    # gate plots (per op)
    gate_columns = {
        "decision_gates": "Decision",
        "from_u_gates": "Get Pending Unit Literal",
        "conflict_gates": "Conflict",
        "regular_total_gates": "Unit Propagation",
        "check_clause_gates": "Check Clause",
        "update_wl_gates": "Update WL",
        "add_implication_gates": "Add Implication",
    }

    gate_out_dir = Path(f"{Path(csv_path).parent}/plots/gate")
    gate_out_dir.mkdir(parents=True, exist_ok=True)
    time_out_dir = Path(f"{Path(csv_path).parent}/plots/time")
    time_out_dir.mkdir(parents=True, exist_ok=True)

    MARKER_SIZE = 12
    MARKERS = ["o", "s", "^", "D", "v", "P", "*", "X"]

    def _limits_from_values(values):
        values = [v for v in values if v > 0]
        if not values:
            return (1e-6, 1.0)
        vmin = min(values)
        vmax = max(values)
        return (vmin, vmax)

    def _apply_slack(vmin, vmax, slack=0):
        if vmin <= 0:
            vmin = 1e-9
        upper = vmax * (1.0 + slack)
        lower = max(1e-9, vmin / (1.0 + slack))
        return lower, upper

    def _set_log_xticks(ax):
        max_exp = int(math.ceil(math.log10(x_max)))
        min_exp = int(math.floor(math.log10(max(x_min, 1e-9))))
        major_ticks = [10 ** e for e in range(min_exp, max_exp + 1)]
        ax.set_xticks(major_ticks)
        ax.xaxis.set_major_locator(LogLocator(base=10, subs=(1.0,)))
        ax.xaxis.set_major_formatter(LogFormatter(base=10, labelOnlyBase=True))
        ax.xaxis.set_minor_locator(LogLocator(base=10, subs=tuple(range(2, 10))))
        ax.xaxis.set_minor_formatter(NullFormatter())

    time_vals = []
    for col in time_columns.keys():
        time_vals.extend(df[col].tolist())
    time_ymin, time_ymax = _limits_from_values(time_vals)
    time_ymin, time_ymax = _apply_slack(time_ymin, time_ymax,Y_AXIS_SLACK)

    gate_vals = []
    has_gate_data = any(
        col in df.columns and pd.to_numeric(df[col], errors="coerce").gt(0).any()
        for col in gate_columns
    )
    if has_gate_data:
        for col in gate_columns.keys():
            if col in df.columns:
                gate_vals.extend(df[col].tolist())
        gate_ymin, gate_ymax = _limits_from_values(gate_vals)
        gate_ymin, gate_ymax = gate_ymin / 1e6, gate_ymax / 1e6
        gate_ymin, gate_ymax = _apply_slack(gate_ymin, gate_ymax,Y_AXIS_SLACK)

    # x_min is already a power of 10; only x_max gets headroom.
    _, x_max = _apply_slack(x_min, x_max, X_AXIS_SLACK)

    # Individual time plots.
    for col, label in time_columns.items():
        plt.figure(figsize=FIGSIZE_INDIV)
        marker_cycle = itertools.cycle(MARKERS)
        for n in sorted(df["var_num"].unique()):
            marker = next(marker_cycle)
            sub = df[df["var_num"] == n].sort_values("clause_num")
            plt.plot(
                sub["clause_num"],
                sub[col],
                marker=marker,
                markersize=MARKER_SIZE,
                linewidth=LINE_WIDTH,
                label=f"{n} Variables",
            )
        plt.xscale("log")
        plt.yscale("log")
        plt.xlim(x_min, x_max)
        plt.ylim(time_ymin, time_ymax)
        plt.xlabel(CLAUSE_LABEL)
        plt.ylabel("Time (s)")
        plt.title(label)
        ncol = _legend_columns(
            len(sorted(df["var_num"].unique())),
            LEGEND_NCOL_INDIV,
            LEGEND_NROW_INDIV,
            None,
        )
        if ncol is not None:
            plt.legend(ncol=ncol)
        else:
            plt.legend()
        fname = time_out_dir / f"{col}_time.pdf"
        plt.savefig(fname, bbox_inches="tight")
        plt.close()
        print(f"Saved {fname}")

    # Individual gate plots.
    if has_gate_data:
        for col, label in gate_columns.items():
            if col not in df.columns:
                continue
            plt.figure(figsize=FIGSIZE_INDIV)
            marker_cycle = itertools.cycle(MARKERS)
            for n in sorted(df["var_num"].unique()):
                marker = next(marker_cycle)
                sub = df[df["var_num"] == n].sort_values("clause_num")
                plt.plot(
                    sub["clause_num"],
                    sub[col]/1e6,  # convert to million gates
                    marker=marker,
                    markersize=MARKER_SIZE,
                    linewidth=LINE_WIDTH,
                    label=f"{n} Variables",
                )
            plt.xscale("log")
            plt.yscale("log")
            plt.xlim(x_min, x_max)
            plt.ylim(gate_ymin, gate_ymax)
            plt.xlabel(CLAUSE_LABEL)
            plt.ylabel("AND gates (million)")
            plt.title(label)
            ncol = _legend_columns(
                len(sorted(df["var_num"].unique())),
                LEGEND_NCOL_INDIV,
                LEGEND_NROW_INDIV,
                None,
            )
            if ncol is not None:
                plt.legend(ncol=ncol)
            else:
                plt.legend()
            fname = gate_out_dir / f"{col}_gates.pdf"
            plt.savefig(fname, bbox_inches="tight")
            plt.close()
            print(f"Saved {fname}")

    def plot_combined(columns, labels, ylabel, out_path, scale=1.0, ylim=None):
        fig, axes = plt.subplots(2, 2, figsize=FIGSIZE_COMBINED, sharex="col", sharey=True)
        axes = axes.flatten()
        for idx, (ax, col, label) in enumerate(zip(axes, columns, labels)):
            marker_cycle = itertools.cycle(MARKERS)
            for n in sorted(df["var_num"].unique()):
                marker = next(marker_cycle)
                sub = df[df["var_num"] == n].sort_values("clause_num")
                ax.plot(
                    sub["clause_num"],
                    sub[col] * scale,
                    marker=marker,
                    markersize=MARKER_SIZE,
                    linewidth=LINE_WIDTH,
                    label=f"{n} Variables",
                )
            ax.set_box_aspect(0.618)
            ax.set_xscale("log")
            ax.set_yscale("log")
            ax.set_xlim(x_min, x_max)
            if ylim:
                ax.set_ylim(ylim[0], ylim[1])
            if idx >= 2:
                ax.set_xlabel(CLAUSE_LABEL)
            else:
                ax.set_xlabel("")
            if idx % 2 == 0:
                ax.set_ylabel(ylabel)
            else:
                ax.set_ylabel("")
            ax.set_title(label)
        handles, labels_leg = axes[0].get_legend_handles_labels()
        ncol = _legend_columns(
            len(labels_leg),
            LEGEND_NCOL_COMBINED,
            LEGEND_NROW_COMBINED,
            len(labels_leg),
        )
        fig.legend(handles, labels_leg, loc="upper center", ncol=ncol,   borderpad= LEGEND_PAD, labelspacing= LEGEND_LABEL_SPACING,handletextpad=LEGEND_HANDLE_TEXT_PAD,        columnspacing=LEGEND_COLUMN_SPACING,
                   bbox_to_anchor=(0.5, 1.05))
        fig.tight_layout(rect=(0, 0, 1, 1))
        plt.subplots_adjust(wspace=SUBPLOT_WSPACE, hspace=SUBPLOT_HSPACE)
        plt.savefig(out_path, bbox_inches="tight")
        plt.close()
        print(f"Saved {out_path}")

    # Combined 2x2 plots for time and gates
    time_cols = [
        "regular_total_time",
        "conflict_time",
        "from_u_time",
        "decision_time",
    ]
    time_labels = [
        "Unit Propagation",
        "Conflict Analysis",
        "Get Pending Unit Literal",
        "Decision",
    ]
    plot_combined(
        time_cols,
        time_labels,
        "Time (s)",
        time_out_dir / "combined_time.pdf",
        scale=1.0,
        ylim=(time_ymin, time_ymax),
    )

    if has_gate_data:
        gate_cols = [
            "regular_total_gates",
            "conflict_gates",
            "from_u_gates",
            "decision_gates",
        ]
        gate_labels = [
            "Unit Propagation",
            "Conflict Analysis",
            "Get Pending Unit Literal",
            "Decision",
        ]
        plot_combined(
            gate_cols,
            gate_labels,
            "AND gates (million)",
            gate_out_dir / "combined_gates.pdf",
            scale=1e-6,
            ylim=(gate_ymin, gate_ymax),
        )


def build_csv_path(args) -> Path:
    var_tag = "-".join(map(str, sorted(args.vars)))
    clause_tag = "-".join(map(str, sorted(args.clauses)))
    param_dir = Path(
        f"./microbench/n_{var_tag}"
        f"_m_{clause_tag}"
        f"_orange_{args.orange_num}"
        f"_yellow_{args.yellow_num}"
        f"_blue_{args.blue_num}"
        f"_red_{args.red_num}"
        f"_conf_{args.conflict_factor}"
        f"_const_{args.max_num_of_con_clause}_{args.max_lit_in_clause}_"
        f"{args.max_lit_in_con_clause}_{args.max_clause_in_wl}"
        f"_runs_{args.runs}"
    )
    param_dir.mkdir(parents=True, exist_ok=True)
    return param_dir / "ppcdcl_microbench.csv"


def _parse_int_list(tag: str, label: str) -> List[int]:
    try:
        parts = [int(p) for p in tag.split("-") if p]
    except ValueError:
        raise ValueError(f"Non-integer {label} component in directory tag: {tag}")
    if not parts:
        raise ValueError(f"No values for {label} in directory tag: {tag}")
    return sorted(set(parts))


def params_from_directory(dir_path: Path) -> Dict[str, object]:
    m = SWEEP_DIR_PATTERN.fullmatch(dir_path.name)
    if not m:
        raise ValueError(f"Could not parse sweep parameters from directory name: {dir_path.name}")

    vars_list = _parse_int_list(m.group("vars"), "vars")
    clauses_list = _parse_int_list(m.group("clauses"), "clauses")

    return {
        "vars": vars_list,
        "clauses": clauses_list,
        "orange_num": int(m.group("orange")),
        "yellow_num": int(m.group("yellow")),
        "blue_num": int(m.group("blue")),
        "red_num": int(m.group("red")),
        "conflict_factor": int(m.group("conflict")),
        "max_num_of_con_clause": int(m.group("const_num")),
        "max_lit_in_clause": int(m.group("const_lit")),
        "max_lit_in_con_clause": int(m.group("const_lit_con")),
        "max_clause_in_wl": int(m.group("wl")),
        "runs": int(m.group("runs")),
    }


def parse_args(argv: Optional[Sequence[str]] = None):
    parser = argparse.ArgumentParser(
        description="Run ppCDCL microbenchmarks across var/clause sweeps and plot results."
    )
    parser.add_argument("--vars", nargs="+", type=int, default=DEFAULT_VAR_LIST,
                        help="List of n (var_num) values to sweep.")
    parser.add_argument("--clauses", nargs="+", type=int, default=DEFAULT_CLAUSE_LIST,
                        help="List of m (clause_num) values to sweep.")
    parser.add_argument("--orange-num", type=int, default=DEFAULT_ORANGE_NUM,
                        help="Number of times to run orange blocks.")
    parser.add_argument("--yellow-num", type=int, default=DEFAULT_YELLOW_NUM,
                        help="Number of times to run decision block.")
    parser.add_argument("--blue-num", type=int, default=DEFAULT_BLUE_NUM,
                        help="Number of times to run from_U block.")
    parser.add_argument("--red-num", type=int, default=DEFAULT_RED_NUM,
                        help="Number of conflicts to scale.")
    parser.add_argument("--conflict-factor", type=int, default=DEFAULT_CONFLICT_FACTOR,
                        help="Conflict down-sampling factor.")
    parser.add_argument("--max-num-of-con-clause", type=int,
                        default=DEFAULT_MAX_NUM_OF_CON_CLAUSE)
    parser.add_argument("--max-lit-in-clause", type=int,
                        default=DEFAULT_MAX_LIT_IN_CLAUSE)
    parser.add_argument("--max-lit-in-con-clause", type=int,
                        default=DEFAULT_MAX_LIT_IN_CON_CLAUSE)
    parser.add_argument("--max-clause-in-wl", type=int,
                        default=DEFAULT_MAX_CLAUSE_IN_WL)
    parser.add_argument("--binary", default=DEFAULT_BIN,
                        help="Path to test_good_bench binary.")
    parser.add_argument("--global-sample-factor", type=int,
                        default=DEFAULT_GLOBAL_SAMPLE_FACTOR,
                        help="Down-sampling factor for yellow/blue/orange blocks (good_bench).")
    parser.add_argument("--conflict-sample-factor", type=int,
                        default=DEFAULT_CONFLICT_SAMPLE_FACTOR,
                        help="Down-sampling factor for conflict block (good_bench).")
    parser.add_argument("--uip-loop-cap", "--uip-cap", dest="uip_loop_cap", type=int,
                        default=DEFAULT_UIP_LOOP_CAP,
                        help="Cap for UIP loop; 0 uses the default in good_bench.")
    backend = parser.add_mutually_exclusive_group()
    backend.add_argument("--oramV", "--oram-v", dest="use_oram_v", action="store_true",
                         help="Use the ORAM variable-state backend.")
    backend.add_argument("--no-oramV", "--no-oram-v", dest="use_oram_v", action="store_false",
                         help="Use the linear-scan variable-state backend (default).")
    parser.set_defaults(use_oram_v=False)
    watchlist = parser.add_mutually_exclusive_group()
    watchlist.add_argument("--shortest-wl", dest="use_shortest_wl", action="store_true",
                           help="Choose replacement watched literals by shortest watchlist.")
    watchlist.add_argument("--no-shortest-wl", dest="use_shortest_wl", action="store_false",
                           help="Use first-fit watchlist replacement (default).")
    parser.set_defaults(use_shortest_wl=False)
    parser.add_argument(
        "--watch-backend",
        type=normalize_watch_backend,
        choices=WATCH_BACKEND_CHOICES,
        default="auto",
        help="Watch storage backend: auto, block, or indexed (default auto).",
    )
    parser.add_argument("--threads", dest="backend_threads", type=int,
                        default=DEFAULT_BACKEND_THREADS,
                        help="Backend worker/NetIO channels (default 4; minimum 2).")
    parser.add_argument("--base-port", "--port", dest="base_port", type=int,
                        default=DEFAULT_BASE_PORT,
                        help="First NetIO port used by both benchmark parties.")
    parser.add_argument("--timeout-seconds", type=float,
                        default=DEFAULT_TIMEOUT_SECONDS,
                        help="Hard timeout for each benchmark party (default 600 seconds).")
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS,
                        help="Repeat each config this many times and average runtimes.")
    parser.add_argument("--csv-path", type=str,
                        help="Optional existing CSV path; otherwise derived from params.")
    parser.add_argument("--plot-only", action="store_true",
                        help="Skip running benchmarks and just plot the CSV.")
    return parser.parse_args(argv)


if __name__ == "__main__":
    args = parse_args()

    csv_path_arg = Path(args.csv_path) if args.csv_path else None
    if args.plot_only and csv_path_arg:
        try:
            derived = params_from_directory(csv_path_arg.parent)
        except ValueError as exc:
            print(f"[warn] {exc}")
        else:
            for key, value in derived.items():
                setattr(args, key, value)
            print(f"[plot-only] Parsed sweep params from directory: {csv_path_arg.parent.name}")

    if args.runs < 1:
        raise ValueError("--runs must be >= 1")
    if args.global_sample_factor < 1:
        raise ValueError("--global-sample-factor must be >= 1")
    if args.conflict_sample_factor < 1:
        raise ValueError("--conflict-sample-factor must be >= 1")
    if args.uip_loop_cap < 0:
        raise ValueError("--uip-loop-cap must be >= 0")
    if args.backend_threads < 2:
        raise ValueError("--threads must be >= 2")
    if args.base_port < 1 or args.base_port + args.backend_threads - 1 > 65535:
        raise ValueError("--base-port and --threads must fit in the TCP port range")
    if args.timeout_seconds <= 0:
        raise ValueError("--timeout-seconds must be > 0")
    args.binary = str(resolve_repo_path(args.binary))

    args.vars = sorted(set(args.vars))
    args.clauses = sorted(set(args.clauses))

    csv_path = csv_path_arg if csv_path_arg else build_csv_path(args)

    if args.plot_only and not csv_path.is_file():
        raise FileNotFoundError(f"--plot-only set but CSV not found at {csv_path}")

    if not args.plot_only:
        print(
            "[params] microbench: "
            f"max_num_of_con_clause={args.max_num_of_con_clause} (fixed); "
            f"max_lit_in_clause={args.max_lit_in_clause} (fixed); "
            f"max_lit_in_con_clause={args.max_lit_in_con_clause} (fixed); "
            f"max_clause_in_wl={args.max_clause_in_wl} (fixed); "
            f"uip_cap={args.uip_loop_cap} (fixed:uip-loop-cap); "
            f"conflict_factor={args.conflict_factor}; "
            f"global_sample_factor={args.global_sample_factor}; "
            f"conflict_sample_factor={args.conflict_sample_factor}; "
            f"variable_state_backend={'oram' if args.use_oram_v else 'linear'}; "
            f"watchlist_policy={'shortest' if args.use_shortest_wl else 'first-fit'}; "
            f"watch_backend_requested={args.watch_backend}; "
            f"backend_threads={args.backend_threads}; "
            f"base_port={args.base_port}; "
            f"timeout_seconds={args.timeout_seconds}; "
            f"blocks={args.orange_num}/{args.yellow_num}/{args.blue_num}/{args.red_num}"
        )
        run_all_configs(args, csv_path)

    make_plots(csv_path)
