#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import random
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from bench_utils import (
    PPCDCL_ROOT,
    count_modulo_one_slots,
    log_config,
    print_ppcdcl_param_settings,
    print_preprocess_settings,
    resolve_optional_repo_path,
    resolve_repo_path,
    run_good_bench,
)
from preprocess_max_wl import SplitArtifacts, normalize_backbone_bin, preprocess_split


BIN_TEST_BIG = Path("./bin/test_big")
BIN_BENCH = Path("./bin/test_good_bench")
BIN_E2E = Path("./bin/test_ppcdcl_solver_e2e")
GOOD_BENCH_PORT = "16696"
E2E_PORT = 12480
MIN_EMP_WATCHLIST_CAP = 8

FIXED_CON_DLY = 200
FIXED_DEC_DLY = 50
CONFLICT_FACTOR = 1
GLOBAL_SAMPLE_FACTOR = 100000
CONFLICT_SAMPLE_FACTOR = 10
TEST_BIG_TIMEOUT = 60
TIMEOUT_PENALTY_TIME = 1e15

MAX_LIT_IN_CONFLICT_CLAUSE = 0
FIXED_MAX_CONFLICT_CLAUSES = 0
BEST_PARAMS = False
BASELINE_PARAMS = False
LEARNED_CAP_ACTION = "restart"
UIP_LOOP_CAP = 0

PREPROCESS_RATIO = 0.5
PREPROCESS_SEED = 0
PREPROCESS_SPLITS = 1
PREPROCESS_MAX_STEPS = 100_000_000
PREPROCESS_LEARNED_MAX_LEN = 10
PREPROCESS_LEARNED_MAX_LBD = 30
PREPROCESS_BACKBONE_BIN = Path("./cadiback")
PREPROCESS_BACKBONE_TIMEOUT = 60
PREPROCESS_OUT_DIR = Path("preprocess_out")
PREPROCESS_TEST_BIG_TIMEOUT = 60
WL_CHOICE = "combined"

OUT_CSV = Path("compare_single/e2e_comparison.csv")


CSV_HEADER = [
    "cnf_path",
    "family",
    "cnf_name",
    "nvar",
    "ncls",
    "config_name",
    "preprocessed_cnf_path",
    "preprocess_split_idx",
    "preprocess_clauses_before",
    "preprocess_clauses_after",
    "preprocess_party_a_max_lit_occ",
    "preprocess_party_b_max_lit_occ",
    "preprocess_combined_max_lit_occ",
    "preprocess_derived_max_clause_in_wl",
    "preprocess_derived_solve_max_clause_in_wl",
    "preprocess_derived_combined_max_clause_in_wl",
    "preprocess_real_max_clause_in_wl",
    "preprocess_used_max_clause_in_wl",
    "preprocess_ge_real_flag",
    "preprocess_ge_real_solve_flag",
    "preprocess_ge_real_combined_flag",
    "preprocess_plaintext_time",
    "preprocess_timed_out",
    "preprocess_learned_cap",
    "preprocess_max_conflict_clauses",
    "preprocess_cap_action",
    "ppcdcl_con_dly",
    "ppcdcl_dec_dly",
    "ppcdcl_max_clause_in_wl",
    "ppcdcl_max_lit_in_clause",
    "ppcdcl_max_lit_in_con_clause",
    "ppcdcl_max_num_of_con_clause",
    "ppcdcl_uip_cap",
    "ppcdcl_orange_total",
    "ppcdcl_yellow_total",
    "ppcdcl_blue_total",
    "ppcdcl_red_total",
    "ppcdcl_conflict_call_total",
    "ppcdcl_backend_threads",
    "ppcdcl_watch_backend_requested",
    "projected_timed_out",
    "projected_con_dly",
    "projected_dec_dly",
    "projected_watch_backend_requested",
    "projected_watch_backend_effective",
    "projected_watch_backend_reason",
    "projected_total_time",
    "projected_decision_time",
    "projected_from_u_time",
    "projected_conflict_time",
    "projected_regular_time",
    "projected_check_clause_time",
    "projected_update_wl_time",
    "projected_add_implication_time",
    "projected_error",
    "e2e_requested",
    "e2e_timed_out",
    "e2e_returncode_party1",
    "e2e_returncode_party2",
    "e2e_result_party1",
    "e2e_result_party2",
    "e2e_result",
    "e2e_result_text_party1",
    "e2e_result_text_party2",
    "e2e_result_text",
    "e2e_assignment_party1",
    "e2e_assignment_party2",
    "e2e_assignment",
    "e2e_failure_reason_party1",
    "e2e_failure_reason_party2",
    "e2e_failure_reason",
    "e2e_terminal_reason",
    "e2e_steps_budget",
    "e2e_con_dly",
    "e2e_dec_dly",
    "e2e_watch_backend_requested_party1",
    "e2e_watch_backend_requested_party2",
    "e2e_watch_backend_effective_party1",
    "e2e_watch_backend_effective_party2",
    "e2e_watch_backend_effective",
    "e2e_watch_backend_reason_party1",
    "e2e_watch_backend_reason_party2",
    "e2e_public_steps_executed_party1",
    "e2e_public_steps_executed_party2",
    "e2e_solver_seconds_party1",
    "e2e_solver_seconds_party2",
    "e2e_case_seconds_party1",
    "e2e_case_seconds_party2",
    "e2e_final_seconds_party1",
    "e2e_final_seconds_party2",
    "e2e_error",
]


WL_CHOICE_ALIASES = {"derived": "occ", "max": "max-all"}
WATCH_BACKEND_ALIASES = {
    "auto": "auto",
    "block": "block",
    "block-local": "block",
    "blocklocal": "block",
    "packed": "block",
    "indexed": "indexed",
}


@dataclass
class PpcdclConfigSpec:
    name: str
    best_params: bool
    baseline_params: bool
    wl_choice: str
    max_lit_in_conflict_clause: Optional[int]

    @property
    def param_mode(self) -> str:
        if self.best_params:
            return "best"
        if self.baseline_params:
            return "baseline"
        return "fixed"


def _opt_path(text: Optional[str]) -> Optional[Path]:
    if text is None:
        return None
    t = str(text).strip()
    if t == "" or t.lower() in {"none", "off", "disable"}:
        return None
    return Path(t)


def _sanitize_cap(max_lits: Optional[int]) -> Optional[int]:
    if max_lits is None:
        return None
    try:
        v = int(max_lits)
    except (TypeError, ValueError):
        return None
    return v if v > 0 else None


def _sanitize_count_capacity(max_clauses: Optional[int]) -> int:
    try:
        return max(0, int(max_clauses or 0))
    except (TypeError, ValueError):
        return 0


def normalize_wl_choice(choice: str) -> str:
    text = (choice or "").strip().lower()
    return WL_CHOICE_ALIASES.get(text, text)


def normalize_watch_backend(value: object) -> str:
    """Map the CLI and C++ diagnostic backend names to the canonical name used in the CSV."""
    text = str(value or "").strip().lower().replace("_", "-")
    return WATCH_BACKEND_ALIASES.get(text, text)


def _max_ignore_none(values: List[Optional[int]]) -> Optional[int]:
    present = [v for v in values if v is not None]
    return max(present) if present else None


def choose_wl_value(
    real_wl: Optional[int],
    occ_wl: Optional[int],
    solve_wl: Optional[int],
    combined_wl: Optional[int],
    best_params: bool,
    wl_choice: str,
) -> Optional[int]:
    choice = normalize_wl_choice(wl_choice)
    if best_params:
        return real_wl or combined_wl or solve_wl or occ_wl
    if choice == "occ":
        return occ_wl or combined_wl or real_wl or solve_wl
    if choice == "solve":
        return solve_wl or combined_wl or real_wl or occ_wl
    if choice == "combined":
        return combined_wl or real_wl or solve_wl or occ_wl
    if choice == "real":
        return real_wl or combined_wl or solve_wl or occ_wl
    if choice == "max-solve":
        return _max_ignore_none([real_wl, solve_wl, combined_wl])
    if choice == "max-combined":
        return _max_ignore_none([real_wl, combined_wl])
    if choice == "max-all":
        return _max_ignore_none([real_wl, solve_wl, occ_wl, combined_wl])
    return _max_ignore_none([real_wl, occ_wl])


def build_ppcdcl_configurations(args: argparse.Namespace) -> List[PpcdclConfigSpec]:
    configs: List[PpcdclConfigSpec] = []
    wl_choice = normalize_wl_choice(args.wl_choice)
    if args.best_params:
        configs.append(PpcdclConfigSpec("best_params", True, False, wl_choice, None))
    if args.baseline_params:
        configs.append(PpcdclConfigSpec("baseline_params", False, True, wl_choice, None))

    sweep_caps = args.max_lit_in_conflict_clause_sweep
    if sweep_caps is None:
        sweep_caps = [args.max_lit_in_conflict_clause]
    for cap in sweep_caps:
        clean = _sanitize_cap(cap)
        if clean is not None:
            configs.append(PpcdclConfigSpec(f"wl_max_cap_{clean}", False, False, wl_choice, clean))
    return configs


def collect_cnf_files(args: argparse.Namespace) -> List[Path]:
    out: List[Path] = []
    for raw in args.inputs:
        p = Path(raw)
        if p.is_dir():
            out.extend(p.rglob("*.cnf"))
        else:
            out.append(p)
    for raw in args.cnf or []:
        out.append(Path(raw))
    for raw in args.dir or []:
        out.extend(Path(raw).rglob("*.cnf"))
    for raw in args.glob or []:
        out.extend(Path(".").glob(raw))

    return sorted({p.resolve() for p in out if p.suffix.lower() == ".cnf"})


def ensure_csv(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        with path.open("w", newline="") as f:
            csv.writer(f).writerow(CSV_HEADER)
        return

    with path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        existing_header = reader.fieldnames or []
        if existing_header == CSV_HEADER:
            return
        if any(field not in CSV_HEADER for field in existing_header):
            raise SystemExit(
                f"Existing CSV header is not a recognized subset of the current schema: {path}. "
                "Use a fresh --out-csv path or migrate the file explicitly."
            )
        rows = list(reader)

    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_HEADER)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in CSV_HEADER})
    tmp.replace(path)


def write_row(path: Path, row: Dict[str, object]) -> None:
    clean = {key: row.get(key, "") for key in CSV_HEADER}
    with path.open("a", newline="") as f:
        csv.DictWriter(f, fieldnames=CSV_HEADER).writerow(clean)


def existing_rows(path: Path) -> Set[Tuple[str, str, str, str, str, str]]:
    if not path.is_file():
        return set()
    with path.open("r", newline="") as f:
        return {
            (
                row.get("cnf_path", ""),
                row.get("config_name", ""),
                row.get("preprocess_split_idx", ""),
                row.get("e2e_requested", ""),
                str(_sanitize_count_capacity(row.get("preprocess_max_conflict_clauses", 0))),
                normalize_watch_backend(row.get("ppcdcl_watch_backend_requested", "")),
            )
            for row in csv.DictReader(f)
        }


def run_preprocessing_splits(
    args: argparse.Namespace,
    cnf_path: Path,
    backbone_bin: Optional[Path],
    cfg: PpcdclConfigSpec,
    cap_lits: Optional[int],
) -> List[SplitArtifacts]:
    outcomes: List[SplitArtifacts] = []
    max_conflict_clauses = _sanitize_count_capacity(args.fixed_max_conflict_clauses)
    learned_max_len = args.preprocess_learned_max_len
    if learned_max_len is None:
        learned_max_len = cap_lits if cap_lits is not None else 0
    for split_idx in range(args.preprocess_splits):
        split_rng = random.Random(args.preprocess_seed + split_idx)
        cleanup_combined = args.skip_preprocess_output and not args.e2e
        max_confl_lits = None if (cfg.best_params or cfg.baseline_params) else cap_lits
        print_preprocess_settings(
            f"{cnf_path.name} cfg={cfg.name} split={split_idx}",
            {
                "mode": cfg.param_mode,
                "ratio": args.preprocess_ratio,
                "seed": args.preprocess_seed + split_idx,
                "max_steps": args.preprocess_max_steps,
                "learned_max_len": learned_max_len,
                "learned_max_lbd": args.preprocess_learned_max_lbd,
                "max_confl_lits": max_confl_lits if max_confl_lits else "none",
                "max_conflict_clauses": max_conflict_clauses,
                "cap_action": args.learned_cap_action,
                "backbone_bin": backbone_bin,
                "backbone_timeout": args.preprocess_backbone_timeout,
                "test_big_timeout": args.preprocess_test_big_timeout,
                "wl_choice": cfg.wl_choice,
                "uip_loop_cap": args.uip_loop_cap,
                "min_wl_override": MIN_EMP_WATCHLIST_CAP,
                "derive_combined_uip": cfg.best_params,
            },
        )
        outcome = preprocess_split(
            cnf_path=cnf_path,
            ratio=args.preprocess_ratio,
            rng=split_rng,
            max_steps=args.preprocess_max_steps,
            backbone_bin=backbone_bin,
            backbone_timeout=args.preprocess_backbone_timeout,
            learned_max_len=learned_max_len,
            learned_max_lbd=args.preprocess_learned_max_lbd,
            con_dly=args.fixed_con_dly,
            dec_dly=args.fixed_dec_dly,
            cap_action=args.learned_cap_action,
            test_big_timeout=args.preprocess_test_big_timeout,
            out_dir=args.preprocess_out_dir,
            base_input=Path("."),
            split_idx=split_idx,
            test_big_bin=args.bin_test_big,
            verbose=args.verbose,
            max_confl_lits=max_confl_lits,
            max_conflict_clauses=max_conflict_clauses,
            min_wl_override=MIN_EMP_WATCHLIST_CAP,
            cleanup_combined=cleanup_combined,
            use_original_params_for_eval=args.skip_preprocess_output,
            wl_choice=cfg.wl_choice,
            uip_loop_cap=args.uip_loop_cap,
            derive_combined_uip=cfg.best_params,
            use_shortest_wl=args.shortest_wl,
        )
        outcomes.append(outcome)
    if not outcomes:
        raise RuntimeError("No preprocessing outcome produced.")
    return outcomes


def dimacs_header_size(cnf_path: Path) -> Tuple[int, int]:
    """Return the variable/clause dimensions declared by a DIMACS header."""
    with Path(cnf_path).open("r", encoding="utf-8", errors="ignore") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("c"):
                continue
            parts = line.split()
            if len(parts) == 4 and parts[0] == "p" and parts[1] == "cnf":
                try:
                    nvar, ncls = int(parts[2]), int(parts[3])
                except ValueError as ex:
                    raise RuntimeError(f"invalid DIMACS header in {cnf_path}: {line}") from ex
                if nvar <= 0 or ncls <= 0:
                    raise RuntimeError(f"DIMACS dimensions must be positive in {cnf_path}: {line}")
                return nvar, ncls
    raise RuntimeError(f"missing 'p cnf' header in {cnf_path}")


def bench_params_from_preprocess(
    outcome: SplitArtifacts,
    cfg: PpcdclConfigSpec,
    max_lit_in_conflict_clause: Optional[int],
    con_dly: int,
    dec_dly: int,
    uip_loop_cap: int,
    evaluation_cnf: Path,
    fixed_max_conflict_clauses: int = 0,
) -> Dict[str, int]:
    raw = dict(outcome.real_params)

    def to_int_opt(val: object) -> Optional[int]:
        try:
            return int(val)
        except (TypeError, ValueError):
            return None

    real_wl = outcome.real_max_wl
    if real_wl is None:
        real_wl = to_int_opt(raw.get("max_clause_in_wl"))
    occ_wl = to_int_opt(outcome.derived_max_wl)
    solve_wl = to_int_opt(outcome.derived_solve_wl)
    combined_wl = to_int_opt(outcome.combined_max_lit_occ)

    chosen_wl = choose_wl_value(real_wl, occ_wl, solve_wl, combined_wl, cfg.best_params, cfg.wl_choice)
    if chosen_wl is not None:
        raw["max_clause_in_wl"] = int(chosen_wl)

    def to_int(val: object, default: int) -> int:
        try:
            return int(val)
        except (TypeError, ValueError):
            return default

    header_nvar, header_clause_num = dimacs_header_size(evaluation_cnf)
    nvar = max(1, to_int(raw.get("var_num"), header_nvar))
    raw_clause_num = to_int_opt(raw.get("clause_num"))
    if raw_clause_num is not None and raw_clause_num != header_clause_num:
        raise RuntimeError(
            "test_big clause count does not match the DIMACS header for "
            f"{evaluation_cnf}: estimator={raw_clause_num}, header={header_clause_num}. "
            "Rebuild test_big; older binaries counted the internal phi[0] sentinel."
        )
    clause_num = header_clause_num

    max_lit_in_clause = max(1, to_int(raw.get("max_lit_in_clause"), nvar))
    max_lit_in_clause = min(max_lit_in_clause, nvar)

    max_lit_in_con_clause = raw.get("max_lit_in_con_clause", max_lit_in_clause)
    max_lit_in_con_clause = max(1, to_int(max_lit_in_con_clause, max_lit_in_clause))
    max_lit_in_con_clause = min(max_lit_in_con_clause, nvar)
    cap_lits = _sanitize_cap(max_lit_in_conflict_clause)
    if cfg.baseline_params:
        max_lit_in_con_clause = nvar
        max_lit_source = "baseline:nvar"
    elif not cfg.best_params and cap_lits is not None:
        max_lit_in_con_clause = max(1, min(int(cap_lits), nvar))
        max_lit_source = "fixed:cap"
    else:
        max_lit_source = "best:plaintext" if cfg.best_params else "plaintext"
    if cfg.best_params:
        raw_uip = getattr(outcome, "combined_max_uip_loop", None)
        if raw_uip is not None and int(raw_uip) > 0:
            uip_cap = max(1, min(int(raw_uip), nvar))
            uip_source = "best:plaintext-uip"
        else:
            uip_cap = nvar
            uip_source = "best:fallback-nvar"
    elif cfg.baseline_params:
        uip_cap = nvar
        uip_source = "baseline:nvar"
    else:
        uip_cap = uip_loop_cap
        uip_source = "fixed:uip-loop-cap"

    wl_source = "best:plaintext-real" if cfg.best_params else f"{cfg.param_mode}:wl-choice:{cfg.wl_choice}"

    orange_total = max(0, to_int(raw.get("orange_total"), 0))
    yellow_total = count_modulo_one_slots(orange_total, dec_dly)
    red_total = count_modulo_one_slots(orange_total, con_dly)
    count_capacity = _sanitize_count_capacity(fixed_max_conflict_clauses)
    max_num_of_con_clause = (
        count_capacity
        if count_capacity > 0
        else max(1, to_int(raw.get("max_num_of_con_clause"), 1))
    )

    return {
        "var_num": nvar,
        "clause_num": clause_num,
        "max_num_of_con_clause": max_num_of_con_clause,
        "max_lit_in_clause": max_lit_in_clause,
        "max_lit_in_con_clause": max_lit_in_con_clause,
        "max_clause_in_wl": max(MIN_EMP_WATCHLIST_CAP, to_int(raw.get("max_clause_in_wl"), 1)),
        "orange_total": orange_total,
        "yellow_total": yellow_total,
        "blue_total": max(0, to_int(raw.get("blue_total"), 0)),
        "red_total": red_total,
        "con_dly": con_dly,
        "dec_dly": dec_dly,
        "uip_cap": uip_cap,
        "_source_max_clause_in_wl": wl_source,
        "_source_max_lit_in_con_clause": max_lit_source,
        "_source_max_num_of_con_clause": (
            "fixed:public-capacity" if count_capacity > 0 else "plaintext"
        ),
        "_source_uip_cap": uip_source,
        "_source_max_lit_in_clause": "plaintext",
    }


def preprocess_fields_dict(outcome: SplitArtifacts, cap_lits: Optional[int], cap_action: str) -> Dict[str, object]:
    wl_flag_occ = "" if outcome.derived_ge_real is None else int(outcome.derived_ge_real)
    wl_flag_solve = "" if outcome.solve_ge_real is None else int(outcome.solve_ge_real)
    wl_flag_combined = "" if outcome.combined_ge_real is None else int(outcome.combined_ge_real)
    return {
        "preprocessed_cnf_path": str(outcome.combined_path),
        "preprocess_split_idx": outcome.split_idx,
        "preprocess_clauses_before": outcome.clauses_before,
        "preprocess_clauses_after": outcome.clauses_after,
        "preprocess_party_a_max_lit_occ": outcome.party_a_max_lit_occ if outcome.party_a_max_lit_occ is not None else "",
        "preprocess_party_b_max_lit_occ": outcome.party_b_max_lit_occ if outcome.party_b_max_lit_occ is not None else "",
        "preprocess_combined_max_lit_occ": outcome.combined_max_lit_occ if outcome.combined_max_lit_occ is not None else "",
        "preprocess_derived_max_clause_in_wl": outcome.derived_max_wl if outcome.derived_max_wl is not None else "",
        "preprocess_derived_solve_max_clause_in_wl": outcome.derived_solve_wl if outcome.derived_solve_wl is not None else "",
        "preprocess_derived_combined_max_clause_in_wl": outcome.combined_max_lit_occ if outcome.combined_max_lit_occ is not None else "",
        "preprocess_real_max_clause_in_wl": outcome.real_max_wl if outcome.real_max_wl is not None else "",
        "preprocess_used_max_clause_in_wl": outcome.used_max_wl if outcome.used_max_wl is not None else "",
        "preprocess_ge_real_flag": wl_flag_occ,
        "preprocess_ge_real_solve_flag": wl_flag_solve,
        "preprocess_ge_real_combined_flag": wl_flag_combined,
        "preprocess_plaintext_time": outcome.plaintext_time if outcome.plaintext_time is not None else "",
        "preprocess_timed_out": int(outcome.timed_out),
        "preprocess_learned_cap": cap_lits if cap_lits is not None else "",
        "preprocess_max_conflict_clauses": int(getattr(outcome, "max_conflict_clauses", 0)),
        "preprocess_cap_action": cap_action,
    }


def _required_positive_int(params: Dict[str, int], key: str) -> int:
    if key not in params:
        raise RuntimeError(f"missing required solver parameter: {key}")
    try:
        value = int(params[key])
    except (TypeError, ValueError) as ex:
        raise RuntimeError(f"solver parameter {key} must be an integer, got {params[key]!r}") from ex
    if value <= 0:
        raise RuntimeError(f"solver parameter {key} must be positive, got {value}")
    return value


def solver_delay_params(params: Dict[str, int]) -> Tuple[int, int]:
    return _required_positive_int(params, "con_dly"), _required_positive_int(params, "dec_dly")


def solver_size_params(params: Dict[str, int]) -> Dict[str, int]:
    return {
        "var_num": _required_positive_int(params, "var_num"),
        "clause_num": _required_positive_int(params, "clause_num"),
        "max_num_of_con_clause": _required_positive_int(params, "max_num_of_con_clause"),
        "max_lit_in_clause": _required_positive_int(params, "max_lit_in_clause"),
        "max_lit_in_con_clause": _required_positive_int(params, "max_lit_in_con_clause"),
        "max_clause_in_wl": _required_positive_int(params, "max_clause_in_wl"),
    }


def run_projected_bench(args: argparse.Namespace, params: Dict[str, int], timed_out: bool) -> Dict[str, object]:
    con_dly, dec_dly = solver_delay_params(params)
    solver_size_params(params)
    delay_fields = {
        "projected_con_dly": con_dly,
        "projected_dec_dly": dec_dly,
        "projected_watch_backend_requested": args.watch_backend,
    }
    if timed_out or not params:
        return {"projected_timed_out": 1, "projected_total_time": TIMEOUT_PENALTY_TIME, **delay_fields}
    times = run_good_bench(
        params,
        bench_bin=args.bin_bench,
        bench_port=args.bench_port,
        conflict_factor=args.conflict_factor,
        global_sample_factor=int(args.global_sample_factor),
        conflict_sample_factor=args.conflict_sample_factor,
        verbose=args.verbose,
        use_oram_v=args.oram_v,
        use_shortest_wl=args.shortest_wl,
        backend_threads=args.backend_threads,
        watch_backend=args.watch_backend,
    )
    reported_request = normalize_watch_backend(times.get("watch_backend_requested", ""))
    if reported_request and reported_request != args.watch_backend:
        raise RuntimeError(
            "projected benchmark used a different public watch backend: "
            f"requested={args.watch_backend} reported={reported_request}"
        )
    return {
        "projected_timed_out": 0,
        **delay_fields,
        "projected_watch_backend_requested": reported_request or args.watch_backend,
        "projected_watch_backend_effective": normalize_watch_backend(
            times.get("watch_backend_effective", "")
        ),
        "projected_watch_backend_reason": times.get("watch_backend_reason", ""),
        "projected_total_time": times.get("total_time", ""),
        "projected_decision_time": times.get("decision_time", ""),
        "projected_from_u_time": times.get("from_u_time", ""),
        "projected_conflict_time": times.get("conflict_time", ""),
        "projected_regular_time": times.get("regular_time", ""),
        "projected_check_clause_time": times.get("check_clause_time", ""),
        "projected_update_wl_time": times.get("update_wl_time", ""),
        "projected_add_implication_time": times.get("add_implication_time", ""),
    }


def _parse_kv_line(line: str) -> Dict[str, str]:
    data: Dict[str, str] = {}
    for part in line.strip().split()[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        data[key] = value
    return data


def _watch_backend_reason(line: str) -> str:
    marker = " reason="
    if marker not in line:
        return ""
    reason = line.split(marker, 1)[1]
    # WATCH_BACKEND_CONFIG appends occ_bits and pair_bits after the reason,
    # which can itself contain spaces and '=' signs.
    for suffix in (" occ_bits=", " pair_bits="):
        if suffix in reason:
            reason = reason.split(suffix, 1)[0]
    return reason.strip()


def _parse_e2e_output(stdout: str) -> Dict[str, object]:
    out: Dict[str, object] = {}
    for line in stdout.splitlines():
        if line.startswith("E2E_SUMMARY"):
            data = _parse_kv_line(line)
            party = data.get("party", "")
            suffix = f"_party{party}" if party in {"1", "2"} else ""
            out[f"e2e_result{suffix}"] = data.get("result", "")
            out[f"e2e_public_steps_executed{suffix}"] = data.get("public_steps_executed", "")
            out[f"e2e_solver_seconds{suffix}"] = data.get("solver_seconds", "")
            out[f"e2e_case_seconds{suffix}"] = data.get("case_e2e_seconds", "")
            out["e2e_steps_budget"] = data.get("steps_budget", out.get("e2e_steps_budget", ""))
        elif line.startswith("E2E_FINAL"):
            data = _parse_kv_line(line)
            party = data.get("party", "")
            if party in {"1", "2"}:
                out[f"e2e_final_seconds_party{party}"] = data.get("e2e_seconds", "")
        elif line.startswith("E2E_RESULT"):
            data = _parse_kv_line(line)
            party = data.get("party", "")
            if party in {"1", "2"}:
                out[f"e2e_result_text_party{party}"] = _result_name(data.get("result", "")).upper()
                out[f"e2e_failure_reason_party{party}"] = data.get("reason", "")
                marker = " assignment="
                if marker in line:
                    out[f"e2e_assignment_party{party}"] = line.split(marker, 1)[1].strip()
        elif line.startswith("WATCH_BACKEND_CONFIG"):
            data = _parse_kv_line(line)
            party = data.get("party", "")
            if party in {"1", "2"}:
                out[f"e2e_watch_backend_requested_party{party}"] = (
                    normalize_watch_backend(data.get("requested", ""))
                )
                out[f"e2e_watch_backend_effective_party{party}"] = (
                    normalize_watch_backend(data.get("effective", ""))
                )
                out[f"e2e_watch_backend_reason_party{party}"] = _watch_backend_reason(line)
    return out


def _result_name(value: object) -> str:
    text = str(value).strip()
    upper = text.upper()
    if text == "1" or upper in {"SAT", "SATISFIABLE"}:
        return "sat"
    if text == "0" or upper in {"UNSAT", "UNSATISFIABLE"}:
        return "unsat"
    if text == "-1" or upper == "TIMEOUT":
        return "timeout"
    if text == "-2" or upper in {"FAILURE", "FAILED"}:
        return "failure"
    return text or "unknown"


def _dimacs_result_label(result: object) -> str:
    name = _result_name(result)
    if name == "sat":
        return "SATISFIABLE"
    if name == "unsat":
        return "UNSATISFIABLE"
    if name == "timeout":
        return "TIMEOUT"
    if name == "failure":
        return "FAILURE"
    return str(result).strip().upper() or "UNKNOWN"


def _int_or_none(value: object) -> Optional[int]:
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return None


def projection_error_percent(projected_seconds: object, party1_seconds: object, party2_seconds: object) -> Optional[float]:
    """Return the percent error of the projected time against the slower party's solver time."""
    try:
        projected = float(str(projected_seconds).strip())
        measured = max(float(str(party1_seconds).strip()), float(str(party2_seconds).strip()))
    except (TypeError, ValueError):
        return None
    if measured <= 0:
        return None
    return abs(projected - measured) / measured * 100.0


def _e2e_terminal_reason(
    *,
    timed_out: bool,
    process_error_before_timeout: bool,
    returncode1: Optional[int],
    returncode2: Optional[int],
    result1: object,
    result2: object,
    result: str,
    steps: int,
    parsed: Dict[str, object],
) -> str:
    if process_error_before_timeout:
        return "process_error"
    if timed_out:
        return "wall_clock_timeout"
    if returncode1 not in (0, None) or returncode2 not in (0, None):
        return "process_error"

    result1_text = str(result1).strip()
    result2_text = str(result2).strip()
    if not result1_text and not result2_text:
        return "missing_summary"
    if not result1_text or not result2_text:
        return "missing_party_summary"
    if result1_text != result2_text:
        return "party_result_mismatch"

    if result in {"sat", "unsat"}:
        return "result_obtained"
    if result == "timeout":
        budget = _int_or_none(parsed.get("e2e_steps_budget", steps))
        executed = [
            _int_or_none(parsed.get("e2e_public_steps_executed_party1")),
            _int_or_none(parsed.get("e2e_public_steps_executed_party2")),
        ]
        if budget is not None and any(value is not None and value >= budget for value in executed):
            return "step_budget_reached"
        return "solver_timeout"
    if result in {"-2", "failure", "failed"}:
        return "solver_failure"
    return "unknown"


def _e2e_steps(args: argparse.Namespace, params: Dict[str, int]) -> int:
    if args.e2e_steps and args.e2e_steps > 0:
        return int(args.e2e_steps)
    base = int(params.get("orange_total", 1))
    return max(1, int(base * float(args.e2e_step_multiplier)))


def run_e2e(args: argparse.Namespace, cnf_path: Path, params: Dict[str, int]) -> Dict[str, object]:
    steps = _e2e_steps(args, params)
    expected = str(args.e2e_expected).strip().lower()
    con_dly, dec_dly = solver_delay_params(params)
    sizes = solver_size_params(params)
    cnf_path = Path(cnf_path).resolve(strict=False)
    common_args = [
        str(cnf_path),
        expected,
        str(steps),
        "--max-watchlist",
        str(sizes["max_clause_in_wl"]),
        "--max-phi-literals",
        str(sizes["max_lit_in_clause"]),
        "--max-conflict-literals",
        str(sizes["max_lit_in_con_clause"]),
        "--max-conflict-clauses",
        str(sizes["max_num_of_con_clause"]),
        "--decision-delay",
        str(dec_dly),
        "--conflict-delay",
        str(con_dly),
        "--threads",
        str(args.backend_threads),
        "--watch-backend",
        args.watch_backend,
    ]
    if int(params.get("uip_cap", 0)) > 0:
        common_args.extend(["--uip-cap", str(params.get("uip_cap", 0))])
    common_args.append("--oramV" if args.oram_v else "--no-oramV")
    if args.shortest_wl:
        common_args.append("--shortest-wl")
    else:
        common_args.append("--no-shortest-wl")
    common_args.extend(["--port", str(args.e2e_port)])
    if args.e2e_public_early_exit:
        common_args.append("--public-early-exit")
    if args.e2e_verbose:
        common_args.append("--verbose")
    if args.e2e_progress_interval and args.e2e_progress_interval > 0:
        common_args.extend(["--progress-interval", str(args.e2e_progress_interval)])

    cmd1 = [str(args.bin_e2e), "1", *common_args]
    cmd2 = [str(args.bin_e2e), "2", *common_args]
    if args.verbose:
        print(f"[e2e] party1 cmd: {' '.join(cmd1)}")
        print(f"[e2e] party2 cmd: {' '.join(cmd2)}")

    p1 = subprocess.Popen(cmd1, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=PPCDCL_ROOT)
    p2 = subprocess.Popen(cmd2, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=PPCDCL_ROOT)

    timed_out = False
    process_error_before_timeout = False
    out1 = err1 = out2 = err2 = ""
    deadline = time.monotonic() + float(args.e2e_timeout)
    try:
        out1, err1 = p1.communicate(timeout=max(0.1, deadline - time.monotonic()))
        out2, err2 = p2.communicate(timeout=max(0.1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        timed_out = True
        pre_timeout_returncodes = [p1.poll(), p2.poll()]
        process_error_before_timeout = any(rc not in (0, None) for rc in pre_timeout_returncodes)
        for proc in (p1, p2):
            if proc.poll() is None:
                proc.kill()
        out1, err1 = p1.communicate()
        out2, err2 = p2.communicate()

    def raise_process_failure(reason: str) -> None:
        raise RuntimeError(
            "e2e solver process failed unexpectedly"
            f" ({reason}).\n"
            f"Party1 CMD: {' '.join(cmd1)}\n"
            f"Party1 rc: {p1.returncode}\n"
            f"Party1 STDOUT:\n{out1}\n"
            f"Party1 STDERR:\n{err1}\n"
            f"Party2 CMD: {' '.join(cmd2)}\n"
            f"Party2 rc: {p2.returncode}\n"
            f"Party2 STDOUT:\n{out2}\n"
            f"Party2 STDERR:\n{err2}"
        )

    if process_error_before_timeout:
        raise_process_failure("process exited before timeout")
    if not timed_out and (p1.returncode != 0 or p2.returncode != 0):
        raise_process_failure("nonzero return code")

    parsed = {}
    parsed.update(_parse_e2e_output(out1))
    parsed.update(_parse_e2e_output(out2))
    requested_backends = [
        normalize_watch_backend(parsed.get("e2e_watch_backend_requested_party1", "")),
        normalize_watch_backend(parsed.get("e2e_watch_backend_requested_party2", "")),
    ]
    effective_backends = [
        normalize_watch_backend(parsed.get("e2e_watch_backend_effective_party1", "")),
        normalize_watch_backend(parsed.get("e2e_watch_backend_effective_party2", "")),
    ]
    if any(value and value != args.watch_backend for value in requested_backends):
        raise_process_failure(
            "watch backend request mismatch: "
            f"expected={args.watch_backend} party1={requested_backends[0] or 'missing'} "
            f"party2={requested_backends[1] or 'missing'}"
        )
    if all(requested_backends) and requested_backends[0] != requested_backends[1]:
        raise_process_failure("party watch backend request mismatch")
    if all(effective_backends) and effective_backends[0] != effective_backends[1]:
        raise_process_failure("party effective watch backend mismatch")
    if not timed_out and (not all(requested_backends) or not all(effective_backends)):
        raise_process_failure("missing WATCH_BACKEND_CONFIG")
    parsed["e2e_watch_backend_effective"] = effective_backends[0] or effective_backends[1]
    result1 = parsed.get("e2e_result_party1", "")
    result2 = parsed.get("e2e_result_party2", "")
    if result1 != "" and result2 != "" and result1 != result2:
        result = "mismatch"
    else:
        result = _result_name(result1 if result1 != "" else result2)
    if timed_out:
        result = "timeout"
    if not timed_out and (result1 == "" or result2 == ""):
        raise_process_failure("missing E2E_SUMMARY")
    if not timed_out and result1 != "" and result2 != "" and result1 != result2:
        raise_process_failure("party result mismatch")
    result_text1 = _result_name(parsed.get("e2e_result_text_party1", result1))
    result_text2 = _result_name(parsed.get("e2e_result_text_party2", result2))
    if timed_out:
        result_text = "timeout"
    elif result_text1 != "unknown" and result_text2 != "unknown" and result_text1 != result_text2:
        raise_process_failure("party result text mismatch")
    else:
        result_text = result_text1 if result_text1 != "unknown" else result_text2
        if result_text == "unknown":
            result_text = _result_name(result)
    assignment1 = str(parsed.get("e2e_assignment_party1", "")).strip()
    assignment2 = str(parsed.get("e2e_assignment_party2", "")).strip()
    failure_reason1 = str(parsed.get("e2e_failure_reason_party1", "")).strip()
    failure_reason2 = str(parsed.get("e2e_failure_reason_party2", "")).strip()
    if not timed_out and result_text == "sat":
        if not assignment1 and not assignment2:
            raise_process_failure("missing SAT assignment")
        if assignment1 and assignment2 and assignment1 != assignment2:
            raise_process_failure("party assignment mismatch")
    if not timed_out and result_text == "failure":
        if failure_reason1 and failure_reason2 and failure_reason1 != failure_reason2:
            raise_process_failure("party failure reason mismatch")
    assignment = assignment1 or assignment2
    failure_reason = failure_reason1 or failure_reason2
    print(_dimacs_result_label(result_text))
    if result_text == "sat" and assignment:
        print(assignment)
    if result_text == "failure" and failure_reason:
        print(failure_reason)
    error_parts: List[str] = []
    if p1.returncode not in (0, None):
        error_parts.append(f"party1 rc={p1.returncode}: {err1.strip()[-500:]}")
    if p2.returncode not in (0, None):
        error_parts.append(f"party2 rc={p2.returncode}: {err2.strip()[-500:]}")
    if timed_out:
        error_parts.append(f"timeout after {args.e2e_timeout}s")
    terminal_reason = _e2e_terminal_reason(
        timed_out=timed_out,
        process_error_before_timeout=process_error_before_timeout,
        returncode1=p1.returncode,
        returncode2=p2.returncode,
        result1=result1,
        result2=result2,
        result=result,
        steps=steps,
        parsed=parsed,
    )

    return {
        "e2e_requested": 1,
        "e2e_timed_out": int(timed_out),
        "e2e_returncode_party1": p1.returncode if p1.returncode is not None else "",
        "e2e_returncode_party2": p2.returncode if p2.returncode is not None else "",
        "e2e_result": result,
        "e2e_result_text": result_text.upper(),
        "e2e_assignment": assignment,
        "e2e_failure_reason": failure_reason,
        "e2e_terminal_reason": terminal_reason,
        "e2e_steps_budget": steps,
        "e2e_con_dly": con_dly,
        "e2e_dec_dly": dec_dly,
        "e2e_error": " | ".join(part for part in error_parts if part),
        **parsed,
    }


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Compare ppCDCL projected benchmark cost with optional two-party e2e runtime."
    )
    ap.add_argument("inputs", nargs="*", help="CNF files or directories. Directories are searched recursively.")
    ap.add_argument("--cnf", action="append", default=None, help="Specific CNF file (repeatable).")
    ap.add_argument("--dir", action="append", default=None, help="Directory to search recursively for CNFs (repeatable).")
    ap.add_argument("--glob", action="append", default=None, help="Glob pattern for CNFs (repeatable).")

    ap.add_argument("--bin-test-big", type=Path, default=BIN_TEST_BIG)
    ap.add_argument("--bin-bench", type=Path, default=BIN_BENCH)
    ap.add_argument("--bin-e2e", type=Path, default=BIN_E2E)
    ap.add_argument("--bench-port", type=str, default=GOOD_BENCH_PORT)
    ap.add_argument("--e2e-port", type=int, default=E2E_PORT)
    ap.add_argument("--fixed-con-dly", type=int, default=FIXED_CON_DLY)
    ap.add_argument("--fixed-dec-dly", type=int, default=FIXED_DEC_DLY)
    ap.add_argument("--conflict-factor", type=int, default=CONFLICT_FACTOR)
    ap.add_argument("--global-sample-factor", type=float, default=GLOBAL_SAMPLE_FACTOR)
    ap.add_argument("--conflict-sample-factor", type=int, default=CONFLICT_SAMPLE_FACTOR)

    ap.add_argument("--max-lit-in-conflict-clause", type=int, default=MAX_LIT_IN_CONFLICT_CLAUSE)
    ap.add_argument(
        "--fixed-max-conflict-clauses",
        type=int,
        default=FIXED_MAX_CONFLICT_CLAUSES,
        help=(
            "Optional public learned-clause count capacity shared by plaintext preprocessing "
            "and the secure solver. Use 10000 for the paper profile; <=0 preserves the "
            "legacy plaintext-derived capacity."
        ),
    )
    ap.add_argument(
        "--max-lit-in-conflict-clause-sweep",
        type=int,
        action="append",
        default=None,
        help="Optional sweep of learned/conflict clause caps. Repeat for multiple configs.",
    )
    ap.add_argument("--uip-loop-cap", type=int, default=UIP_LOOP_CAP)
    ap.add_argument("--learned-cap-action", choices=["restart", "chrono"], default=LEARNED_CAP_ACTION)
    ap.add_argument(
        "--best-params",
        dest="best_params",
        action="store_true",
        default=BEST_PARAMS,
        help=(
            "Use smallest viable plaintext-derived ppCDCL parameters: actual watchlist size, actual learned/conflict "
            "clause length, and actual combined UIP loop bound when available."
        ),
    )
    ap.add_argument("--no-best-params", dest="best_params", action="store_false")
    ap.add_argument(
        "--baseline-params",
        dest="baseline_params",
        action="store_true",
        default=BASELINE_PARAMS,
        help=(
            "Use the old nvar-sized baseline for variable-bounded parameters such as UIP cap and learned/conflict "
            "clause length. This is conservative and not the smallest setting."
        ),
    )
    ap.add_argument("--no-baseline-params", dest="baseline_params", action="store_false")
    ap.add_argument(
        "--wl-choice",
        choices=["occ", "solve", "combined", "real", "max-occ", "max-solve", "max-combined", "max-all", "derived", "max"],
        default=WL_CHOICE,
    )

    ap.add_argument("--preprocess-ratio", type=float, default=PREPROCESS_RATIO)
    ap.add_argument("--preprocess-seed", type=int, default=PREPROCESS_SEED)
    ap.add_argument("--preprocess-splits", type=int, default=PREPROCESS_SPLITS)
    ap.add_argument("--preprocess-max-steps", type=int, default=PREPROCESS_MAX_STEPS)
    ap.add_argument(
        "--preprocess-learned-max-len",
        type=int,
        default=None,
        help="Learned clause length filter for preprocessing. Defaults to the active conflict-clause cap, or 0 in best/baseline modes.",
    )
    ap.add_argument("--preprocess-learned-max-lbd", type=int, default=PREPROCESS_LEARNED_MAX_LBD)
    ap.add_argument("--preprocess-backbone-bin", type=Path, default=PREPROCESS_BACKBONE_BIN)
    ap.add_argument("--preprocess-backbone-timeout", type=int, default=PREPROCESS_BACKBONE_TIMEOUT)
    ap.add_argument("--preprocess-out-dir", type=Path, default=PREPROCESS_OUT_DIR)
    ap.add_argument("--preprocess-test-big-timeout", type=int, default=PREPROCESS_TEST_BIG_TIMEOUT)
    ap.add_argument("--skip-preprocess-output", action="store_true")

    ap.add_argument("--out-csv", type=Path, default=OUT_CSV)
    ap.add_argument("--skip-already-done", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--oramV", dest="oram_v", action="store_true", default=False)
    ap.add_argument("--no-oramV", dest="oram_v", action="store_false")
    ap.add_argument("--shortest-wl", dest="shortest_wl", action="store_true", default=False)
    ap.add_argument("--no-shortest-wl", dest="shortest_wl", action="store_false")
    ap.add_argument(
        "--watch-backend",
        choices=["auto", "block", "indexed"],
        default="auto",
        help=(
            "Public watcher storage backend shared by projection and both e2e parties "
            "(default: auto)."
        ),
    )
    ap.add_argument("--backend-threads", type=int, default=4, help="Shared E2E and microbenchmark worker count (default 4).")
    ap.add_argument("--projection", dest="projection", action="store_true", default=True)
    ap.add_argument("--no-projection", dest="projection", action="store_false")

    ap.add_argument("--e2e", default=True, action="store_true", help="Also run the two-party e2e solver for each queued formula.")
    ap.add_argument("--e2e-timeout", type=float, default=100000.0, help="Wall-clock timeout per two-party e2e run.")
    ap.add_argument("--e2e-steps", type=int, default=1e6, help="Override e2e public giant-step budget.")
    ap.add_argument(
        "--e2e-step-multiplier",
        type=float,
        default=1.0,
        help="Multiplier for the default e2e budget, which is ppcdcl orange_total.",
    )
    ap.add_argument(
        "--e2e-expected",
        choices=["any", "sat", "unsat", "timeout", "failure", "1", "0", "-1", "-2"],
        default="any",
        help="Expected e2e result. Default any records the result without failing on SAT vs UNSAT.",
    )
    ap.add_argument("--e2e-public-early-exit", dest="e2e_public_early_exit", action="store_true", default=True)
    ap.add_argument("--no-e2e-public-early-exit", dest="e2e_public_early_exit", action="store_false")
    ap.add_argument("--e2e-verbose", action="store_true")
    ap.add_argument("--e2e-progress-interval", type=int, default=0)
    return ap.parse_args()


def main() -> None:
    args = parse_args()
    if args.best_params and args.baseline_params:
        raise SystemExit("--best-params and --baseline-params are mutually exclusive.")
    if args.backend_threads < 2:
        raise SystemExit("--backend-threads must be at least 2.")
    args.wl_choice = normalize_wl_choice(args.wl_choice)
    args.fixed_max_conflict_clauses = _sanitize_count_capacity(args.fixed_max_conflict_clauses)
    args.bin_test_big = resolve_repo_path(args.bin_test_big)
    args.bin_bench = resolve_repo_path(args.bin_bench)
    args.bin_e2e = resolve_repo_path(args.bin_e2e)
    configs = build_ppcdcl_configurations(args)
    if not configs:
        raise SystemExit(
            "No ppCDCL configs requested. Use --best-params, --baseline-params, "
            "or pass a positive --max-lit-in-conflict-clause."
        )

    cnfs = collect_cnf_files(args)
    if not cnfs:
        raise SystemExit("No CNFs found.")
    if not args.bin_test_big.is_file():
        raise SystemExit(f"test_big binary not found: {args.bin_test_big}")
    if args.projection and not args.bin_bench.is_file():
        raise SystemExit(f"test_good_bench binary not found: {args.bin_bench}")
    if args.e2e and not args.bin_e2e.is_file():
        raise SystemExit(f"e2e binary not found: {args.bin_e2e}")
    if args.e2e and args.skip_preprocess_output:
        print("[warn] Keeping preprocessed CNFs so the e2e binary can read them.")
        args.skip_preprocess_output = False

    args.preprocess_backbone_bin = resolve_optional_repo_path(args.preprocess_backbone_bin)
    backbone_bin = normalize_backbone_bin(args.preprocess_backbone_bin)
    ensure_csv(args.out_csv)
    done = existing_rows(args.out_csv) if args.skip_already_done else set()

    log_config(
        "e2e_comparison config",
        {
            "cnfs": len(cnfs),
            "configs": [cfg.name for cfg in configs],
            "out_csv": args.out_csv,
            "wl_choice": args.wl_choice,
            "fixed_con_dly": args.fixed_con_dly,
            "fixed_dec_dly": args.fixed_dec_dly,
            "uip_loop_cap": args.uip_loop_cap,
            "fixed_max_conflict_clauses": args.fixed_max_conflict_clauses,
            "e2e": args.e2e,
            "projection": args.projection,
            "oram_v": args.oram_v,
            "watchlist_strategy": "shortest" if args.shortest_wl else "first-fit",
            "watch_backend": args.watch_backend,
            "backend_threads": args.backend_threads,
            "best_params": args.best_params,
            "baseline_params": args.baseline_params,
            "e2e_timeout": args.e2e_timeout,
            "e2e_steps": args.e2e_steps,
            "preprocess_out_dir": args.preprocess_out_dir,
        },
        args.verbose,
    )

    print(f"[+] CNFs: {len(cnfs)}")
    print(f"[+] ppCDCL configs: {[cfg.name for cfg in configs]}")
    print(f"[+] Writing CSV: {args.out_csv}")
    print(
        f"[+] projection={'enabled' if args.projection else 'disabled'} "
        f"variable_state={'oram' if args.oram_v else 'linear'} "
        f"watchlist_strategy={'shortest' if args.shortest_wl else 'first-fit'}"
        f" watch_backend={args.watch_backend}"
        f" backend_threads={args.backend_threads}"
    )
    if args.e2e:
        print(f"[+] e2e enabled: timeout={args.e2e_timeout}s expected={args.e2e_expected}")
        if args.e2e_public_early_exit:
            print("[!] e2e public early exit is enabled; this is useful for timing experiments but leaks solve length.")

    for cnf in cnfs:
        cnf = cnf.resolve()
        family = cnf.parent.name
        print(f"\n=== {cnf} ===")
        for cfg in configs:
            cap_lits = _sanitize_cap(cfg.max_lit_in_conflict_clause)
            outcomes = run_preprocessing_splits(args, cnf, backbone_bin, cfg, cap_lits)
            for outcome in outcomes:
                done_key = (
                    str(cnf),
                    cfg.name,
                    str(outcome.split_idx),
                    str(int(args.e2e)),
                    str(args.fixed_max_conflict_clauses),
                    args.watch_backend,
                )
                if done_key in done:
                    print(f"[+] Skipping already done: {cnf} cfg={cfg.name} split={outcome.split_idx} e2e={int(args.e2e)}")
                    continue

                bench_params = bench_params_from_preprocess(
                    outcome,
                    cfg,
                    cap_lits,
                    args.fixed_con_dly,
                    args.fixed_dec_dly,
                    args.uip_loop_cap,
                    cnf if args.skip_preprocess_output else outcome.combined_path,
                    args.fixed_max_conflict_clauses,
                )
                print_ppcdcl_param_settings(
                    f"{cnf.name} cfg={cfg.name} split={outcome.split_idx}",
                    bench_params,
                )
                con_dly, dec_dly = solver_delay_params(bench_params)
                log_config(
                    "shared ppCDCL solver params",
                    {
                        "cnf": outcome.combined_path,
                        "split_idx": outcome.split_idx,
                        "config": cfg.name,
                        "con_dly": con_dly,
                        "dec_dly": dec_dly,
                        "max_clause_in_wl": bench_params.get("max_clause_in_wl", ""),
                        "max_lit_in_clause": bench_params.get("max_lit_in_clause", ""),
                        "max_lit_in_con_clause": bench_params.get("max_lit_in_con_clause", ""),
                        "max_num_of_con_clause": bench_params.get("max_num_of_con_clause", ""),
                        "uip_cap": bench_params.get("uip_cap", ""),
                        "watch_backend": args.watch_backend,
                        "backend_threads": args.backend_threads,
                    },
                    args.verbose,
                )
                if args.projection:
                    projected = run_projected_bench(args, bench_params, outcome.timed_out)
                    print(
                        f"[{cfg.name} split={outcome.split_idx}] projected={projected.get('projected_total_time', '')} "
                        f"wl={bench_params.get('max_clause_in_wl')} cap={bench_params.get('max_lit_in_con_clause')} "
                        f"uip={bench_params.get('uip_cap')}"
                    )
                else:
                    projected = {
                        "projected_timed_out": "",
                        "projected_error": "projection_skipped",
                        "projected_con_dly": con_dly,
                        "projected_dec_dly": dec_dly,
                        "projected_watch_backend_requested": args.watch_backend,
                    }
                e2e_data: Dict[str, object] = {
                    "e2e_requested": int(args.e2e),
                    "e2e_terminal_reason": "not_requested" if not args.e2e else "",
                }
                if args.e2e:
                    e2e_data.update(
                        {
                            "e2e_con_dly": con_dly,
                            "e2e_dec_dly": dec_dly,
                            "e2e_terminal_reason": "preprocess_timed_out" if outcome.timed_out else "",
                        }
                    )
                if args.e2e and not outcome.timed_out:
                    e2e_data.update(run_e2e(args, outcome.combined_path, bench_params))
                if args.projection and args.e2e and not outcome.timed_out:
                    error_pct = projection_error_percent(
                        projected.get("projected_total_time", ""),
                        e2e_data.get("e2e_solver_seconds_party1", ""),
                        e2e_data.get("e2e_solver_seconds_party2", ""),
                    )
                    projected["projected_error"] = "" if error_pct is None else error_pct

                row: Dict[str, object] = {
                    "cnf_path": str(cnf),
                    "family": family,
                    "cnf_name": cnf.name,
                    "nvar": outcome.nvar,
                    "ncls": outcome.clauses_after,
                    "ppcdcl_backend_threads": args.backend_threads,
                    "ppcdcl_watch_backend_requested": args.watch_backend,
                    "config_name": cfg.name,
                    "ppcdcl_con_dly": con_dly,
                    "ppcdcl_dec_dly": dec_dly,
                    "ppcdcl_max_clause_in_wl": bench_params.get("max_clause_in_wl", ""),
                    "ppcdcl_max_lit_in_clause": bench_params.get("max_lit_in_clause", ""),
                    "ppcdcl_max_lit_in_con_clause": bench_params.get("max_lit_in_con_clause", ""),
                    "ppcdcl_max_num_of_con_clause": bench_params.get("max_num_of_con_clause", ""),
                    "ppcdcl_uip_cap": bench_params.get("uip_cap", ""),
                    "ppcdcl_orange_total": bench_params.get("orange_total", ""),
                    "ppcdcl_yellow_total": bench_params.get("yellow_total", ""),
                    "ppcdcl_blue_total": bench_params.get("blue_total", ""),
                    "ppcdcl_red_total": bench_params.get("red_total", ""),
                    "ppcdcl_conflict_call_total": bench_params.get("red_total", ""),
                }
                row.update(preprocess_fields_dict(outcome, cap_lits, args.learned_cap_action))
                row.update(projected)
                row.update(e2e_data)
                write_row(args.out_csv, row)
                done.add(done_key)

                print(
                    f"[{cfg.name} split={outcome.split_idx}] projected={row.get('projected_total_time', '')} "
                    f"solver={row.get('e2e_solver_seconds_party1', '')}/{row.get('e2e_solver_seconds_party2', '')} "
                    f"projection_error_pct={row.get('projected_error', '')} "
                    f"reason={row.get('e2e_terminal_reason', '')} "
                    f"wl={bench_params.get('max_clause_in_wl')} cap={bench_params.get('max_lit_in_con_clause')} "
                    f"uip={bench_params.get('uip_cap')}"
                )

    print(f"\n[done] wrote {args.out_csv}")


if __name__ == "__main__":
    main()
