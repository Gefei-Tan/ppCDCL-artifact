#!/usr/bin/env python3
from __future__ import annotations

import re
import subprocess
import time
from pathlib import Path
from typing import Dict, Optional, Tuple, Union

DEFAULT_TEST_BIG_BIN = Path("./bin/test_big")
DEFAULT_BENCH_BIN = Path("./bin/test_good_bench")
PPCDCL_ROOT = Path(__file__).resolve().parents[1]
WATCH_BACKEND_CHOICES = ("auto", "block", "indexed")


def normalize_watch_backend(value: object) -> str:
    """Return the canonical watch backend name (auto, block or indexed), or raise ValueError."""
    normalized = str(value).strip().lower()
    if normalized in {"block-local", "block_local", "blocklocal", "packed"}:
        normalized = "block"
    if normalized not in WATCH_BACKEND_CHOICES:
        choices = "|".join(WATCH_BACKEND_CHOICES)
        raise ValueError(f"watch_backend must be one of {choices}, got {value!r}")
    return normalized


def parse_good_bench_watch_backend(
    stdout: str,
    expected_requested: str = "auto",
) -> Dict[str, object]:
    """Parse the optional BENCH_WATCH_BACKEND metadata line.

    If the binary does not emit the line, the requested backend is kept and the
    effective backend is set to the requested one, or left empty when the
    request is auto. The reason is then "not_reported".
    """
    expected = normalize_watch_backend(expected_requested)
    matches = [
        line.strip()
        for line in stdout.splitlines()
        if line.startswith("BENCH_WATCH_BACKEND")
    ]
    if not matches:
        return {
            "watch_backend_requested": expected,
            "watch_backend_effective": expected if expected != "auto" else "",
            "watch_backend_reason": "not_reported",
            "watch_backend_occ_bits": None,
            "watch_backend_pair_bits": None,
        }

    parsed_matches = []
    for line in matches:
        match = re.fullmatch(
            r"BENCH_WATCH_BACKEND\s+requested=(\S+)\s+effective=(\S+)"
            r"\s+reason=(.*?)\s+occ_bits=(-?\d+)\s+pair_bits=(-?\d+)\s*",
            line,
        )
        if match is None:
            raise RuntimeError(f"Malformed BENCH_WATCH_BACKEND line: {line}")

        requested = normalize_watch_backend(match.group(1))
        effective = normalize_watch_backend(match.group(2))
        if effective == "auto":
            raise RuntimeError(
                "Malformed BENCH_WATCH_BACKEND line; effective backend must "
                f"be block or indexed: {line}"
            )
        if requested != expected:
            raise RuntimeError(
                "test_good_bench reported a different requested watch backend: "
                f"sent {expected}, received {requested}"
            )
        try:
            occ_bits = int(match.group(4))
            pair_bits = int(match.group(5))
        except ValueError as exc:
            raise RuntimeError(
                f"Malformed BENCH_WATCH_BACKEND bit geometry: {line}"
            ) from exc
        if occ_bits < 0 or pair_bits < 0:
            raise RuntimeError(
                f"Malformed BENCH_WATCH_BACKEND bit geometry: {line}"
            )
        parsed_matches.append(
            {
                "watch_backend_requested": requested,
                "watch_backend_effective": effective,
                "watch_backend_reason": match.group(3),
                "watch_backend_occ_bits": occ_bits,
                "watch_backend_pair_bits": pair_bits,
            }
        )

    first = parsed_matches[0]
    if any(candidate != first for candidate in parsed_matches[1:]):
        raise RuntimeError(
            "Conflicting BENCH_WATCH_BACKEND lines in test_good_bench output"
        )
    return first


def resolve_repo_path(path: Union[Path, str]) -> Path:
    """Resolve relative paths against the ppCDCL repo root, not the caller's cwd."""
    p = Path(path).expanduser()
    if p.is_absolute():
        return p
    return (PPCDCL_ROOT / p).resolve(strict=False)


def resolve_optional_repo_path(path: Optional[Union[Path, str]]) -> Optional[Path]:
    if path is None:
        return None
    text = str(path).strip()
    if text == "" or text.lower() in {"none", "off", "disable"}:
        return None
    return resolve_repo_path(text)


def verbose_print(enabled: bool, message: str) -> None:
    """Print only when verbose mode is enabled."""
    if enabled:
        print(message)


def format_command(cmd: object) -> str:
    if isinstance(cmd, (list, tuple)):
        return " ".join(str(part) for part in cmd)
    return str(cmd)


def binary_failure_message(label: str, cmd: object, returncode: object, stdout: object, stderr: object) -> str:
    return (
        f"{label} failed unexpectedly (rc={returncode}).\n"
        f"CMD: {format_command(cmd)}\n"
        f"STDOUT:\n{stdout or ''}\n"
        f"STDERR:\n{stderr or ''}"
    )


def format_kv(data: Dict[str, object]) -> str:
    return ", ".join(f"{k}={v}" for k, v in sorted(data.items()))


def log_config(prefix: str, data: Dict[str, object], verbose: bool) -> None:
    verbose_print(verbose, f"[verbose] {prefix}: {format_kv(data)}")


def _source(params: Dict[str, object], key: str, default: str = "fixed") -> str:
    value = params.get(f"_source_{key}")
    return str(value) if value not in (None, "") else default


def print_ppcdcl_param_settings(prefix: str, params: Dict[str, object]) -> None:
    """Print the ppCDCL parameters that determine the projected and end-to-end runtime."""
    if not params:
        print(f"[params] {prefix}: no solver params available")
        return
    parts = [
        f"wl_size={params.get('max_clause_in_wl')} ({_source(params, 'max_clause_in_wl')})",
        f"uip_cap={params.get('uip_cap', 0)} ({_source(params, 'uip_cap')})",
        (
            f"max_lit_in_conflict_clause={params.get('max_lit_in_con_clause')} "
            f"({_source(params, 'max_lit_in_con_clause')})"
        ),
        (
            f"max_conflict_clauses={params.get('max_num_of_con_clause')} "
            f"({_source(params, 'max_num_of_con_clause')})"
        ),
        f"max_phi_literals={params.get('max_lit_in_clause')} ({_source(params, 'max_lit_in_clause', 'plaintext')})",
        f"vars={params.get('var_num')}",
        f"clauses={params.get('clause_num')}",
        f"con_dly={params.get('con_dly')}",
        f"dec_dly={params.get('dec_dly')}",
        (
            "blocks="
            f"{params.get('orange_total')}/{params.get('yellow_total')}/"
            f"{params.get('blue_total')}/{params.get('red_total')}"
        ),
    ]
    print(f"[params] {prefix}: " + "; ".join(parts))


def print_preprocess_settings(prefix: str, settings: Dict[str, object]) -> None:
    """Print the preprocessing settings used before a projected or end-to-end ppCDCL run."""
    print(f"[preprocess] {prefix}: {format_kv(settings)}")


def _sanitize_int(value: Optional[int], minimum: int = 1, maximum: Optional[int] = None) -> int:
    if value is None:
        return minimum
    try:
        v = int(value)
    except (TypeError, ValueError):
        return minimum
    v = max(minimum, v)
    if maximum is not None:
        v = min(maximum, v)
    return v


def _sanitize_count(value: Optional[int]) -> int:
    return _sanitize_int(value, minimum=0)


def count_modulo_one_slots(executed_steps: int, period: int) -> int:
    """Count the steps t in 1..executed_steps with t == 1 (mod period)."""
    steps = int(executed_steps)
    delay = int(period)
    if steps < 0:
        raise ValueError("executed_steps must be non-negative")
    if delay <= 0:
        raise ValueError("period must be positive")
    if steps == 0:
        return 0
    return 1 + (steps - 1) // delay


def parse_test_big_output(stdout: str, cmd: object = "test_big") -> Tuple[Dict[str, int], Optional[float]]:
    lines = stdout.strip().splitlines()
    if not lines:
        raise RuntimeError(f"test_big produced no output.\nCMD: {format_command(cmd)}")

    plaintext_time: Optional[float] = None
    for line in lines:
        if "total time" in line.lower():
            m = re.search(r"total time:\s*([0-9.+\-eE]+)", line, flags=re.IGNORECASE)
            if m:
                try:
                    plaintext_time = float(m.group(1))
                except ValueError:
                    pass

    last = lines[-1].strip().split()
    if len(last) != 12:
        raise RuntimeError(
            f"Expected 12 integers from test_big, got: {last}.\n"
            f"CMD: {format_command(cmd)}\nSTDOUT:\n{stdout}"
        )

    try:
        vals = list(map(int, last))
    except ValueError as ex:
        raise RuntimeError(
            f"test_big final line is not 12 integers: {last}.\n"
            f"CMD: {format_command(cmd)}\nSTDOUT:\n{stdout}"
        ) from ex
    keys = [
        "var_num",
        "clause_num",
        "max_num_of_con_clause",
        "max_lit_in_clause",
        "max_lit_in_con_clause",
        "max_clause_in_wl",
        "orange_total",
        "yellow_total",
        "blue_total",
        "red_total",
        "con_dly",
        "dec_dly",
    ]
    params = dict(zip(keys, vals))
    executed_steps = params["orange_total"]
    if executed_steps < 0:
        raise RuntimeError(
            f"test_big reported a negative executed-step count: {executed_steps}.\n"
            f"CMD: {format_command(cmd)}\nSTDOUT:\n{stdout}"
        )
    for count_key, delay_key in (("yellow_total", "dec_dly"), ("red_total", "con_dly")):
        try:
            expected = count_modulo_one_slots(executed_steps, params[delay_key])
        except ValueError as ex:
            raise RuntimeError(
                f"test_big reported an invalid public schedule: {delay_key}={params[delay_key]}.\n"
                f"CMD: {format_command(cmd)}\nSTDOUT:\n{stdout}"
            ) from ex
        if params[count_key] != expected:
            raise RuntimeError(
                "test_big schedule accounting does not match the paper policy "
                f"t == 1 (mod delay): T={executed_steps}, {delay_key}={params[delay_key]}, "
                f"{count_key}={params[count_key]}, expected={expected}. Rebuild test_big.\n"
                f"CMD: {format_command(cmd)}\nSTDOUT:\n{stdout}"
            )
    if params["blue_total"] < 0:
        raise RuntimeError(
            f"test_big reported a negative blue block count: {params['blue_total']}.\n"
            f"CMD: {format_command(cmd)}\nSTDOUT:\n{stdout}"
        )
    return params, plaintext_time


def run_test_big(
    cnf_path: Path,
    con_dly: int,
    dec_dly: int,
    timeout: int,
    max_wl_size: Optional[int] = None,
    uip_loop_cap: Optional[int] = 0,
    cap: Optional[int] = None,
    max_conflict_clauses: Optional[int] = None,
    cap_action: str = "restart",
    use_shortest_wl: bool = False,
    test_big_bin: Path = DEFAULT_TEST_BIG_BIN,
    verbose: bool = False,
) -> Tuple[Dict[str, int], bool, Optional[float]]:
    """
    Run test_big with optional watchlist, learned-length, and learned-count caps.

    Returns (params, timed_out, plaintext_time).
    """

    test_big_bin = resolve_repo_path(test_big_bin)
    if not test_big_bin.is_file():
        raise SystemExit(f"test_big binary not found at {test_big_bin}. Build it first.")

    cnf_path = Path(cnf_path).resolve(strict=False)
    cmd = [str(test_big_bin), str(cnf_path), str(con_dly), str(dec_dly)]
    if max_wl_size is not None and max_wl_size > 0:
        cmd.extend(["--max-wl-size", str(max_wl_size)])
    if cap is not None and cap > 0:
        cmd.extend(["--learned-clause-cap", str(cap), "--cap-action", cap_action])
    if max_conflict_clauses is not None and max_conflict_clauses > 0:
        cmd.extend(["--max-num-of-con-clause", str(max_conflict_clauses)])
    cmd.append("--shortest-wl" if use_shortest_wl else "--no-shortest-wl")
    
    cmd.extend(["--uip-cap", str(uip_loop_cap)])
    verbose_print(verbose, f"[test_big] command: {' '.join(cmd)} (timeout={timeout}s)")

    try:
        res = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=True,
            cwd=PPCDCL_ROOT,
        )
    except subprocess.TimeoutExpired:
        verbose_print(verbose, f"[test_big] TIMEOUT after {timeout}s on {cnf_path}")
        return {}, True, None
    except subprocess.CalledProcessError as e:
        raise RuntimeError(binary_failure_message("test_big", cmd, e.returncode, e.stdout, e.stderr)) from e

    params, plaintext_time = parse_test_big_output(res.stdout, cmd)
    if verbose:
        verbose_print(verbose, "[test_big] raw stdout:")
        verbose_print(verbose, res.stdout.strip())
        verbose_print(verbose, f"[test_big] parsed params: {format_kv(params)}")
        if plaintext_time is not None:
            verbose_print(verbose, f"[test_big] plaintext total time: {plaintext_time}")
    return params, False, plaintext_time


def run_good_bench(
        params: Dict[str, int],
        *,
        bench_bin: Path = DEFAULT_BENCH_BIN,
        bench_port: str = "12498",
        conflict_factor: int = 4,
        global_sample_factor: int = int(1e12),
        conflict_sample_factor: int = 10,
        verbose: bool = False,
        use_oram_v: bool = False,
        use_shortest_wl: bool = False,
        watch_backend: str = "auto",
        backend_threads: int = 4,
        timeout_seconds: float = 600.0,
) -> Dict[str, object]:
    """Run test_good_bench for both parties and parse SUMMARY."""
    bench_bin = resolve_repo_path(bench_bin)
    if not bench_bin.is_file():
        raise SystemExit(f"test_good_bench binary not found at {bench_bin}. Build it first.")
    if int(backend_threads) < 2:
        raise ValueError("backend_threads must be at least 2")
    if float(timeout_seconds) <= 0:
        raise ValueError("timeout_seconds must be positive")
    watch_backend = normalize_watch_backend(watch_backend)
    orange_total = _sanitize_count(params.get("orange_total"))
    if orange_total == 0:
        raise ValueError(
            "cannot project a zero-step solve: test_good_bench does not model the "
            "pre-loop unit-clause initialization cost"
        )

    common_args = [
        str(_sanitize_int(params.get("var_num"))),
        str(_sanitize_int(params.get("clause_num"))),
        str(_sanitize_int(params.get("max_num_of_con_clause"))),
        str(_sanitize_int(params.get("max_lit_in_clause"))),
        str(_sanitize_int(params.get("max_lit_in_con_clause"))),
        str(_sanitize_int(params.get("max_clause_in_wl"))),
        str(conflict_factor),
        str(global_sample_factor),
        str(conflict_sample_factor),
        str(orange_total),
        str(_sanitize_count(params.get("yellow_total"))),
        str(_sanitize_count(params.get("blue_total"))),
        str(_sanitize_count(params.get("red_total"))),
        str((params.get("uip_cap",0))),
    ]

    cmd1 = [str(bench_bin), "1", str(bench_port), *common_args]
    cmd2 = [str(bench_bin), "2", str(bench_port), *common_args]
    oram_v_flag = "--oramV" if use_oram_v else "--no-oramV"
    cmd1.append(oram_v_flag)
    cmd2.append(oram_v_flag)
    if use_shortest_wl:
        cmd1.append("--shortest-wl")
        cmd2.append("--shortest-wl")
    else:
        cmd1.append("--no-shortest-wl")
        cmd2.append("--no-shortest-wl")
    cmd1.extend(["--watch-backend", watch_backend])
    cmd2.extend(["--watch-backend", watch_backend])
    cmd1.extend(["--threads", str(int(backend_threads))])
    cmd2.extend(["--threads", str(int(backend_threads))])

    print(
        "[bench] good_bench: "
        f"conflict_factor={conflict_factor}; "
        f"global_sample_factor={global_sample_factor}; "
        f"conflict_sample_factor={conflict_sample_factor}; "
        f"variable_state={'oram' if use_oram_v else 'linear'}; "
        f"watchlist_strategy={'shortest' if use_shortest_wl else 'first-fit'}; "
        f"watch_backend_requested={watch_backend}; "
        f"backend_threads={int(backend_threads)}; "
        f"bench_port={bench_port}"
    )
    verbose_print(verbose, f"[good_bench] party1 cmd: {' '.join(cmd1)}")
    verbose_print(verbose, f"[good_bench] party2 cmd: {' '.join(cmd2)}")

    p1 = subprocess.Popen(cmd1, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=PPCDCL_ROOT)
    p2 = subprocess.Popen(cmd2, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=PPCDCL_ROOT)
    deadline = time.monotonic() + float(timeout_seconds)
    try:
        out1, err1 = p1.communicate(timeout=max(0.1, deadline - time.monotonic()))
        out2, err2 = p2.communicate(timeout=max(0.1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired as exc:
        for proc in (p1, p2):
            if proc.poll() is None:
                proc.kill()
        out1, err1 = p1.communicate()
        out2, err2 = p2.communicate()
        raise RuntimeError(
            f"ppcdcl_bench timed out after {float(timeout_seconds):g}s\n"
            f"Party1 CMD: {format_command(cmd1)}\nParty1 STDOUT:\n{out1}\nParty1 STDERR:\n{err1}\n"
            f"Party2 CMD: {format_command(cmd2)}\nParty2 STDOUT:\n{out2}\nParty2 STDERR:\n{err2}"
        ) from exc

    if p1.returncode != 0:
        raise RuntimeError(binary_failure_message("ppcdcl_bench party1", cmd1, p1.returncode, out1, err1))
    if p2.returncode != 0:
        raise RuntimeError(binary_failure_message("ppcdcl_bench party2", cmd2, p2.returncode, out2, err2))

    summary_line = next((ln for ln in reversed(out1.strip().splitlines()) if ln.startswith("SUMMARY")), None)
    if summary_line is None:
        raise RuntimeError(
            "No SUMMARY line found in ppcdcl_bench party1 output.\n"
            f"Party1 CMD: {format_command(cmd1)}\n"
            f"Party1 STDOUT:\n{out1}\n"
            f"Party1 STDERR:\n{err1}\n"
            f"Party2 CMD: {format_command(cmd2)}\n"
            f"Party2 STDOUT:\n{out2}\n"
            f"Party2 STDERR:\n{err2}"
        )

    parts = summary_line.split()
    _, total_t, dec_t, fromu_t, conf_t, reg_t, chk_t, wl_t, imp_t, *_ = parts

    if len(parts) != 13:
        raise RuntimeError(
            f"Malformed SUMMARY line from ppcdcl_bench: {summary_line}\n"
            f"Party1 CMD: {format_command(cmd1)}\nParty1 STDOUT:\n{out1}"
        )
    verbose_print(verbose,
                  f"[good_bench] Parsed SUMMARY times: total={total_t}, decision={dec_t}, from_u={fromu_t}, conflict={conf_t}, regular={reg_t}, check_clause={chk_t}, update_wl={wl_t}, add_implication={imp_t}")

    watch_backend_metadata = parse_good_bench_watch_backend(
        out1,
        expected_requested=watch_backend,
    )
    if "BENCH_WATCH_BACKEND" in out2:
        party2_metadata = parse_good_bench_watch_backend(
            out2,
            expected_requested=watch_backend,
        )
        if party2_metadata != watch_backend_metadata:
            raise RuntimeError(
                "test_good_bench parties reported different watch backend metadata: "
                f"party1={watch_backend_metadata}, party2={party2_metadata}"
            )
    verbose_print(
        verbose,
        f"[good_bench] watch backend: {format_kv(watch_backend_metadata)}",
    )

    return {
        "total_time": float(total_t),
        "decision_time": float(dec_t),
        "from_u_time": float(fromu_t),
        "conflict_time": float(conf_t),
        "regular_time": float(reg_t),
        "check_clause_time": float(chk_t),
        "update_wl_time": float(wl_t),
        "add_implication_time": float(imp_t),
        **watch_backend_metadata,
    }
