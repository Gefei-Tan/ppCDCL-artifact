#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import os
import random
import re
import subprocess
import tempfile
from collections import Counter
from concurrent.futures import Executor, Future, ProcessPoolExecutor, ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Mapping, Optional, Set, Tuple

from bench_utils import (
    PPCDCL_ROOT,
    log_config,
    print_ppcdcl_param_settings,
    print_preprocess_settings,
    resolve_optional_repo_path,
    resolve_repo_path,
    run_good_bench,
)
from preprocess_max_wl import SplitArtifacts, normalize_backbone_bin, preprocess_split


def _opt_path(text: Optional[str]) -> Optional[Path]:
    if text is None:
        return None
    t = str(text).strip()
    if t == "" or t.lower() in {"none", "off", "disable"}:
        return None
    return Path(t)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Compare ppSAT vs ppCDCL with preprocessing/max-wl pipeline.")
    ap.add_argument("--cnf", action="append", default=None, help="Specific CNF file (repeatable).")
    ap.add_argument("--dir", action="append", default=None, help="Directory to search recursively for CNFs (repeatable).")
    ap.add_argument("--glob", action="append", default=None, help="Glob pattern for CNFs (repeatable).")
    ap.add_argument("--ppsat-heuristic", choices=["rand", "det", "wrand"], default=PPSAT_HEURISTIC)
    ap.add_argument("--ppsat-time-budget", type=float, default=PPSAT_TIME_BUDGET_SEC)
    ap.add_argument("--ppsat-x-budget", type=float, default=PPSAT_X_BUDGET)
    ap.add_argument("--microtest-bin", type=Path, default=MICROTEST_BIN)
    ap.add_argument("--microtest-fixed-args", nargs=3, default=MICROTEST_FIXED_ARGS, help="Args for microtest (seed, literals, last).")
    ap.add_argument("--microtest-var-scale", type=float, default=MICROTEST_VAR_SCALE)
    ap.add_argument("--microtest-min-var", type=int, default=MICROTEST_MIN_VAR)
    ap.add_argument("--microtest-timeout", type=int, default=MICROTEST_TIMEOUT_SEC)
    ap.add_argument("--bin-test-big", type=Path, default=BIN_TEST_BIG)
    ap.add_argument("--bin-bench", type=Path, default=BIN_BENCH)
    ap.add_argument("--bench-port", type=str, default=GOOD_BENCH_PORT)
    ap.add_argument("--fixed-con-dly", type=int, default=FIXED_CON_DLY)
    ap.add_argument("--fixed-dec-dly", type=int, default=FIXED_DEC_DLY)
    ap.add_argument("--test-big-timeout", type=int, default=TEST_BIG_TIMEOUT)
    ap.add_argument("--conflict-factor", type=int, default=CONFLICT_FACTOR)
    ap.add_argument("--global-sample-factor", type=float, default=GLOBAL_SAMPLE_FACTOR)
    ap.add_argument("--conflict-sample-factor", type=int, default=CONFLICT_SAMPLE_FACTOR)
    ap.add_argument("--max-lit-in-conflict-clause", type=int, default=MAX_LIT_IN_CONFLICT_CLAUSE)
    ap.add_argument(
        "--fixed-max-conflict-clauses",
        type=int,
        default=FIXED_MAX_CONFLICT_CLAUSES,
        help=(
            "Public learned-clause count capacity used by both the plaintext trajectory "
            "and secure estimator. Use 10000 for the paper profile; <=0 preserves the "
            "legacy plaintext-derived capacity."
        ),
    )
    ap.add_argument("--uip-loop-cap", type=int, default=UIP_LOOP_CAP)
    ap.add_argument(
        "--max-lit-in-conflict-clause-sweep",
        type=int,
        action="append",
        default=MAX_LIT_IN_CONFLICT_CLAUSE_SWEEP,
        help="Optional sweep of max-lit-in-conflict-clause values (repeatable).",
    )
    ap.add_argument("--learned-cap-action", choices=["restart", "chrono"], default=LEARNED_CAP_ACTION)
    ap.add_argument("--preprocess-ratio", type=float, default=PREPROCESS_RATIO)
    ap.add_argument("--preprocess-seed", type=int, default=PREPROCESS_SEED)
    ap.add_argument("--preprocess-splits", type=int, default=PREPROCESS_SPLITS)
    ap.add_argument("--preprocess-max-steps", type=int, default=PREPROCESS_MAX_STEPS)
    ap.add_argument("--preprocess-learned-max-len", type=int, default=PREPROCESS_LEARNED_MAX_LEN)
    ap.add_argument("--preprocess-learned-max-lbd", type=int, default=PREPROCESS_LEARNED_MAX_LBD)
    ap.add_argument("--preprocess-backbone-bin", type=Path, default=PREPROCESS_BACKBONE_BIN)
    ap.add_argument("--preprocess-backbone-timeout", type=int, default=PREPROCESS_BACKBONE_TIMEOUT)
    ap.add_argument("--preprocess-out-dir", type=Path, default=PREPROCESS_OUT_DIR)
    ap.add_argument("--preprocess-test-big-timeout", type=int, default=PREPROCESS_TEST_BIG_TIMEOUT)
    ap.add_argument("--reuse-ppsat-csv", type=str, default=str(REUSE_PPSAT_CSV) if REUSE_PPSAT_CSV else "")
    ap.add_argument("--out-csv", type=Path, default=OUT_CSV)
    ap.add_argument("--skip-already-done", dest="skip_already_done", action="store_true", default=SKIP_ALREADY_DONE)
    ap.add_argument("--no-skip-already-done", dest="skip_already_done", action="store_false", help="Process all CNFs even if present in OUT_CSV.")
    ap.add_argument("--verbose", action="store_true", help="Print detailed configuration and commands.")
    ap.add_argument("--oramV", dest="oram_v", action="store_true", default=False)
    ap.add_argument("--no-oramV", dest="oram_v", action="store_false")
    ap.add_argument("--shortest-wl", dest="shortest_wl", action="store_true", default=False)
    ap.add_argument("--no-shortest-wl", dest="shortest_wl", action="store_false")
    ap.add_argument(
        "--watch-backend",
        choices=["auto", "block", "indexed"],
        default=WATCH_BACKEND,
        help=(
            "Watcher storage backend. Auto selects from public geometry only; "
            "the requested and effective choices are recorded in the output CSV."
        ),
    )
    ap.add_argument(
        "--wl-choice",
        choices=["occ", "solve", "combined", "real", "max-occ", "max-solve", "max-combined", "max-all", "derived", "max"],
        default=WL_CHOICE,
        help="Watchlist size selection: occurrence, split-solve, combined-occ (max literal occurrence on combined CNF), real, or max variants.",
    )
    ap.add_argument(
        "--best-params",
        dest="best_params",
        action="store_true",
        help=(
            "Use smallest viable plaintext-derived ppCDCL parameters: actual watchlist size, actual learned/conflict "
            "clause length, and actual combined UIP loop bound when available."
        ),
    )
    ap.add_argument(
        "--no-best-params",
        dest="best_params",
        action="store_false",
        help="Do not run the plaintext-derived smallest-parameter configuration.",
    )
    ap.set_defaults(best_params=BEST_PARAMS)
    ap.add_argument(
        "--baseline-params",
        dest="baseline_params",
        action="store_true",
        help=(
            "Run the old nvar-sized baseline for variable-bounded parameters such as UIP cap and learned/conflict "
            "clause length. This is conservative and not the smallest setting."
        ),
    )
    ap.add_argument(
        "--no-baseline-params",
        dest="baseline_params",
        action="store_false",
        help="Do not run the nvar-sized baseline configuration.",
    )
    ap.set_defaults(baseline_params=BASELINE_PARAMS)
    ap.add_argument(
        "--skip-preprocess-output",
        action="store_true",
        help="Skip keeping combined preprocessed CNF on disk (still runs preprocessing to derive params).",
    )
    ap.add_argument(
        "--no-ppsat",
        action="store_true",
        help="Skip running ppSAT/microtest; use cached ppSAT results when present.",
    )
    ap.add_argument(
        "--ppsat-only",
        action="store_true",
        default=PPSAT_ONLY,
        help="Compute ppSAT columns only; skip preprocessing and ppCDCL, leaving those columns blank.",
    )
    ap.add_argument(
        "--ppsat-preprocessing",
        action="store_true",
        default=PPSAT_PREPROCESSING,
        help="Run ppSAT on the preprocessed CNF (per split) instead of the original CNF.",
    )
    ap.add_argument(
        "--ppsat-jobs",
        type=int,
        default=PPSAT_JOBS,
        help="Parallelism for --ppsat-only across CNFs (1=serial).",
    )
    ap.add_argument(
        "--ppsat-parallel-backend",
        choices=["thread", "process"],
        default=PPSAT_PARALLEL_BACKEND,
        help="Parallel backend for --ppsat-only: threads (lower overhead) or processes (real CPU parallelism).",
    )
    return ap.parse_args()


# Default settings. main() overwrites most of them from the command-line
# options, which use them as defaults.

# CNF inputs.
CNF_FILES: List[str] = [
]

DIR_TO_SEARCH: List[str] = [

]

# ppSAT step counting.
PPSAT_HEURISTIC: str = "wrand"  # {"rand","det","wrand"}
PPSAT_TIME_BUDGET_SEC: float = 1e12
PPSAT_X_BUDGET: float = PPSAT_TIME_BUDGET_SEC

# microtest (ppSAT per-step cost).
MICROTEST_BIN = Path("./ppsat/Eval/microtest")
MICROTEST_FIXED_ARGS = ["16666", "3", "1"]  # seed, literals, last
MICROTEST_VAR_SCALE: float = 1  # factor applied to the variable count passed to microtest
MICROTEST_MIN_VAR: int = 200
MICROTEST_TIMEOUT_SEC: int = 120

# ppCDCL cost and time breakdown.
BIN_TEST_BIG = Path("./bin/test_big")
BIN_BENCH = Path("./bin/test_good_bench")
GOOD_BENCH_PORT = "16693"

FIXED_CON_DLY = 200
FIXED_DEC_DLY = 50
TEST_BIG_TIMEOUT = 15
TIMEOUT_PENALTY_TIME = 1e15

CONFLICT_FACTOR = 4
GLOBAL_SAMPLE_FACTOR = int(1e12)
CONFLICT_SAMPLE_FACTOR = 10

# Preprocessing and watchlist settings.
# Cap for learned/conflict clause length when running test_big during preprocessing;
# set <=0 to disable.
MAX_LIT_IN_CONFLICT_CLAUSE = 10
FIXED_MAX_CONFLICT_CLAUSES = 0
BEST_PARAMS = False
BASELINE_PARAMS = False
MAX_LIT_IN_CONFLICT_CLAUSE_SWEEP: List[int] = []
LEARNED_CAP_ACTION = "restart"
UIP_LOOP_CAP = 20


PREPROCESS_RATIO = 0.5
PREPROCESS_SEED = 0
PREPROCESS_SPLITS = 1
PREPROCESS_MAX_STEPS = 100_000_000
PREPROCESS_LEARNED_MAX_LEN = MAX_LIT_IN_CONFLICT_CLAUSE
PREPROCESS_LEARNED_MAX_LBD = 30
PREPROCESS_BACKBONE_BIN = Path("./cadiback")
PREPROCESS_BACKBONE_TIMEOUT = 20
PREPROCESS_OUT_DIR = Path("preprocess_out")
PREPROCESS_TEST_BIG_TIMEOUT = 20

# Optional: reuse ppSAT results from an existing CSV instead of recomputing them.
REUSE_PPSAT_CSV: Optional[Path] = Path("compare_single/pre_big.csv")
OUT_CSV = Path("compare_single/pre_big_better.csv")

# Skip rows already present in OUT_CSV.
SKIP_ALREADY_DONE = True

# If CNF_FILES is empty, CNFs are collected from these glob patterns.
CNF_GLOBS: List[str] = [
]

VERBOSE = False
WL_CHOICE = "combined"
ORAM_V = False
SHORTEST_WL = False
WATCH_BACKEND = "auto"
NO_PPSAT = False
PPSAT_ONLY = False
PPSAT_PREPROCESSING = False
PPSAT_JOBS = 1
PPSAT_PARALLEL_BACKEND = "process"


# Python implementation of ppSAT, used to count solver steps.

re_Literal_capture = r"(-?)([0-9]+)"


class Literal:
    __slots__ = ("var", "is_neg")

    def __init__(self, var: int, is_neg: bool = False):
        self.var = var
        self.is_neg = is_neg

    @staticmethod
    def default() -> "Literal":
        return Literal(var=0, is_neg=False)

    def opp(self) -> "Literal":
        return Literal(self.var, not self.is_neg)

    def is_opp(self, other: "Literal") -> bool:
        return self.var == other.var and self.is_neg == (not other.is_neg)

    def __hash__(self) -> int:
        return hash((self.is_neg, self.var))

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Literal) and self.var == other.var and self.is_neg == other.is_neg

    def __lt__(self, other: "Literal") -> bool:
        return self.var < other.var or (self.var == other.var and self.is_neg and not other.is_neg)

    def __repr__(self) -> str:
        return ("-" if self.is_neg else "") + str(self.var)

    @staticmethod
    def parse(text: str) -> "Literal":
        m = re.match(re_Literal_capture, text)
        if m is None:
            raise ValueError(f"Bad literal text: {text!r}")
        sig, var = m.groups()
        return Literal(int(var), sig == "-")


class Clause:
    __slots__ = ("literals",)

    def __init__(self, literals: Set[Literal]):
        self.literals = literals

    def contains(self, literal: Literal) -> bool:
        return literal in self.literals

    def remove(self, literal: Literal) -> "Clause":
        if literal in self.literals:
            new_lits = self.literals.copy()
            new_lits.remove(literal)
            return Clause(new_lits)
        return self

    def unit(self) -> bool:
        return len(self.literals) == 1

    def get_unit_literal(self) -> Literal:
        if not self.unit():
            raise RuntimeError("not unit")
        return next(iter(self.literals))

    def __iter__(self) -> Iterator[Literal]:
        return iter(self.literals)


class Model:
    __slots__ = ("literals",)

    def __init__(self, lits: Optional[Set[Literal]] = None):
        self.literals = lits if lits is not None else set()

    @staticmethod
    def default() -> "Model":
        return Model(set())

    def add(self, literal: Literal) -> None:
        if literal.opp() in self.literals:
            raise RuntimeError("model contains negation")
        self.literals.add(literal)

    def __iter__(self) -> Iterator[Literal]:
        return iter(self.literals)


class State:
    __slots__ = ("ell", "model")

    def __init__(self, ell: Literal, model: Model):
        self.ell = ell
        self.model = model

    @staticmethod
    def default() -> "State":
        return State(Literal.default(), Model.default())


class ConditionalStack:
    __slots__ = ("stack",)

    def __init__(self):
        self.stack: List[Tuple[Literal, Set[Literal]]] = []

    def push(self, is_real: bool, ell: Literal, model: Model) -> None:
        if is_real:
            self.stack.append((ell, model.literals.copy()))

    def pop(self, is_real: bool) -> State:
        if is_real:
            ell, lits = self.stack.pop()
            return State(ell, Model(lits))
        return State.default()

    def empty(self) -> bool:
        return len(self.stack) == 0


class Formula:
    __slots__ = ("clauses",)

    def __init__(self, clauses: List[Clause]):
        self.clauses = clauses

    FORMULA_EMPTY = 0
    HAS_NEGATION = 1
    UNDETERMINED = 2

    def check(self, literal: Literal) -> int:
        if len(self.clauses) == 0:
            return Formula.FORMULA_EMPTY
        for clause in self.clauses:
            if clause.unit():
                l = clause.get_unit_literal()
                if literal.is_opp(l):
                    return Formula.HAS_NEGATION
        return Formula.UNDETERMINED

    def propagation(self, literal: Literal) -> None:
        literal_opp = literal.opp()
        new_clauses: List[Clause] = []
        for clause in self.clauses:
            if clause.contains(literal):
                continue
            if clause.contains(literal_opp):
                new_clauses.append(clause.remove(literal_opp))
            else:
                new_clauses.append(clause)
        self.clauses = new_clauses

    def simplify(self, model: Model) -> None:
        for literal in model:
            self.propagation(literal)

    def unit_search(self) -> Tuple[bool, Literal]:
        found = False
        literal = Literal.default()
        for clause in self.clauses:
            if clause.unit():
                found = True
                literal = clause.get_unit_literal()
        return found, literal

    def apply_literal(self, literal: Literal) -> Tuple[int, bool, Literal]:
        """
        Combine check(), propagation() and unit_search() in one pass over the
        clauses: detect a unit conflict, propagate the literal, and return the
        last unit literal of the updated formula.
        """
        if not self.clauses:
            return Formula.FORMULA_EMPTY, False, Literal.default()

        literal_opp = literal.opp()
        new_clauses: List[Clause] = []
        unit_found = False
        unit_literal = Literal.default()

        for clause in self.clauses:
            is_unit = clause.unit()
            has_literal = clause.contains(literal)
            has_opp = clause.contains(literal_opp)

            if is_unit and has_opp:
                return Formula.HAS_NEGATION, False, Literal.default()

            if has_literal:
                continue

            if has_opp:
                clause = clause.remove(literal_opp)

            new_clauses.append(clause)

            if clause.unit():
                unit_found = True
                unit_literal = clause.get_unit_literal()

        self.clauses = new_clauses
        return Formula.UNDETERMINED, unit_found, unit_literal


class Heuristic:
    @staticmethod
    def decision(formula: Formula) -> Literal:
        raise NotImplementedError


class DetHeuristic(Heuristic):
    @staticmethod
    def decision(formula: Formula) -> Literal:
        if len(formula.clauses) == 0:
            return Literal.default()
        counter: Dict[Literal, int] = {}
        for clause in formula.clauses:
            for lit in clause:
                counter[lit] = counter.get(lit, 0) + 1
        return max(counter, key=counter.get)


class RandHeuristic(Heuristic):
    @staticmethod
    def decision(formula: Formula) -> Literal:
        import random
        if len(formula.clauses) == 0:
            return Literal.default()
        lits: Set[Literal] = set()
        for clause in formula.clauses:
            lits.update(clause.literals)
        return random.choice(tuple(lits))


class WeightedRandHeuristic(Heuristic):
    @staticmethod
    def decision(formula: Formula) -> Literal:
        import random
        if len(formula.clauses) == 0:
            return Literal.default()
        lits: List[Literal] = []
        for clause in formula.clauses:
            lits.extend(clause.literals)
        return random.choice(lits)


def solve(starting_formula: Formula, h: Heuristic, max_steps: int) -> int:
    starting_clauses_lits = [c.literals.copy() for c in starting_formula.clauses]

    def fresh_formula() -> Formula:
        return Formula([Clause(lits.copy()) for lits in starting_clauses_lits])

    stack = ConditionalStack()
    i = 0
    current_literal = Literal.default()
    current_model = Model.default()
    current_formula = fresh_formula()
    conflict = False

    while True:
        if i != 0:
            check_result, b_unit, l0 = current_formula.apply_literal(current_literal)
            if check_result == Formula.FORMULA_EMPTY:
                return i
            elif check_result == Formula.HAS_NEGATION:
                conflict = True
                if stack.empty():
                    return i
                state = stack.pop(conflict)
                current_literal = state.ell.opp()
                current_model = state.model
                current_formula = fresh_formula()
                current_formula.simplify(current_model)
                b_unit, l0 = current_formula.unit_search()
            else:
                conflict = False
                current_model.add(current_literal)

        else:
            b_unit, l0 = current_formula.unit_search()

        l1 = h.decision(current_formula)
        stack.push(not b_unit and not conflict, l1, current_model)

        if not conflict:
            current_literal = l0 if b_unit else l1

        if i >= max_steps:
            return -1
        i += 1


def read_dimacs_formula(filename: str) -> Tuple[int, int, Formula]:
    """
    Read a DIMACS CNF file and return (nvar, ncls, formula).

    Comment lines are skipped, and literals are accumulated until a terminating
    0, so clauses wrapped over several lines are supported.
    """
    nvar = None
    ncls = None

    clauses: List[Clause] = []
    pending: List[int] = []

    with open(filename, "r", encoding="utf-8", errors="ignore") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("c"):
                continue
            if line.startswith("p"):
                parts = line.split()
                if len(parts) < 4 or parts[1] != "cnf":
                    raise RuntimeError(f"Bad DIMACS header line: {line}")
                nvar = int(parts[2])
                ncls = int(parts[3])
                continue

            if nvar is None or ncls is None:
                continue  # ignore lines before the header

            for tok in line.split():
                v = int(tok)
                if v == 0:
                    if pending:
                        lits: Set[Literal] = set()
                        for lit in pending:
                            lits.add(Literal(abs(lit), lit < 0))
                        clauses.append(Clause(lits))
                        pending = []
                else:
                    pending.append(v)

    if nvar is None or ncls is None:
        raise RuntimeError(f"No DIMACS 'p cnf' header found in {filename}")

    # Flush a final clause that has no terminating 0.
    if pending:
        lits: Set[Literal] = set(Literal(abs(l), l < 0) for l in pending)
        clauses.append(Clause(lits))

    return nvar, ncls, Formula(clauses)


def infer_genotype_and_case(cnf_path: Path) -> Tuple[str, str]:
    """
    Return (genotype, casenum) used to select alpha.

    The genotype is the parent folder name if it matches n<digits>, otherwise
    "generic". The case number is the file stem.
    """
    parent = cnf_path.parent.name
    if re.fullmatch(r"n\d+", parent):
        return parent, cnf_path.stem
    return "generic", cnf_path.stem


@dataclass
class PpSatStepsResult:
    nvar: int
    ncls: int
    genotype: str
    casenum: str
    htype: str
    steps_length: float
    alpha: float
    x_budget: float
    max_steps: int
    steps_returned: int  # -1 if hit max_steps
    steps_used: Optional[int]  # absent when steps_returned is the timeout sentinel
    scaled_steps: Optional[float]  # absent when the search is censored
    x_value: Optional[float]  # absent when the search is censored


def get_ppsat_steps(
        cnf_path: Path,
        htype: str,
        x_budget: float,
        per_step_seconds: float,
        parsed_formula: Optional[Tuple[int, int, Formula]] = None,
) -> PpSatStepsResult:
    genotype, casenum = infer_genotype_and_case(cnf_path)
    if parsed_formula is None:
        nvar, ncls, formula = read_dimacs_formula(str(cnf_path))
    else:
        nvar, ncls, formula = parsed_formula

    if htype == "rand":
        h = RandHeuristic
        alpha_base = 0.4
    elif htype == "det":
        h = DetHeuristic
        alpha_base = 0.5
    elif htype == "wrand":
        h = WeightedRandHeuristic
        alpha_base = 1.0
    else:
        raise RuntimeError(f"Unknown heuristic type {htype}")

    if genotype in ("n3", "n6"):
        factor = 3 if genotype == "n3" else 36
        alpha = alpha_base * factor
    else:
        alpha = 2.5 if htype == "wrand" else (0.48 if htype == "det" else 1.0)

    steps_length = per_step_seconds

    if steps_length <= 0:
        max_steps = 1
    else:
        max_steps = max(1, int(x_budget / steps_length))

    steps = solve(formula, h, max_steps)
    if steps < -1:
        raise RuntimeError(f"ppSAT returned invalid negative step count: {steps}")
    if steps == -1:
        steps_used = None
        scaled_steps = None
        x_value = None
    else:
        steps_used = steps
        scaled_steps = steps_used * steps_length
        x_value = scaled_steps * alpha
    return PpSatStepsResult(
        nvar=nvar,
        ncls=ncls,
        genotype=genotype,
        casenum=casenum,
        htype=htype,
        steps_length=steps_length,
        alpha=alpha,
        x_budget=x_budget,
        max_steps=max_steps,
        steps_returned=steps,
        steps_used=steps_used,
        scaled_steps=scaled_steps,
        x_value=x_value,
    )


# microtest: per-step cost of ppSAT.

_MICRO_KEYS = ["unit search", "guess", "mux", "check", "backtrack", "propagation", "total time"]


@dataclass
class MicrotestCost:
    var_in: int
    var_scale: float
    ncls: int
    times_raw: Dict[str, float]  # per-step times for the scaled var_in
    times_scaled: Dict[str, float]  # per-step times scaled linearly to the original nvar


def _parse_microtest_party_output(text: str) -> Dict[str, float]:
    """Extract per-step times (seconds) from one party's stdout; the last occurrence of each key wins."""
    out: Dict[str, float] = {}

    # Lines look like "unit search: 1.85392 seconds" and "total time: 24.9062".
    for key in _MICRO_KEYS:
        if key == "total time":
            pat = re.compile(rf"^{re.escape(key)}:\s*([0-9]*\.?[0-9]+)\s*$", re.MULTILINE)
        else:
            pat = re.compile(rf"^{re.escape(key)}:\s*([0-9]*\.?[0-9]+)\s*seconds\s*$", re.MULTILINE)

        matches = pat.findall(text)
        if matches:
            out[key] = float(matches[-1])

    if "total time" not in out:
        raise RuntimeError(f"Could not find 'total time:' in microtest output.\n--- stdout ---\n{text}")

    return out


def get_ppsat_step_cost_microtest(nvar: int, ncls: int, var_scale: float, heuristic_name="wrand") -> MicrotestCost:
    return get_ppsat_step_cost_microtest_with_config(
        nvar=nvar,
        ncls=ncls,
        var_scale=var_scale,
        heuristic_name=heuristic_name,
        microtest_bin=MICROTEST_BIN,
        microtest_fixed_args=MICROTEST_FIXED_ARGS,
        microtest_min_var=MICROTEST_MIN_VAR,
        microtest_timeout_sec=MICROTEST_TIMEOUT_SEC,
    )


def get_ppsat_step_cost_microtest_with_config(
        *,
        nvar: int,
        ncls: int,
        var_scale: float,
        heuristic_name: str = "wrand",
        microtest_bin: Path,
        microtest_fixed_args: List[str],
        microtest_min_var: int,
        microtest_timeout_sec: int,
) -> MicrotestCost:
    microtest_bin = resolve_repo_path(microtest_bin)
    if var_scale <= 0:
        raise ValueError("var_scale must be > 0")

    if microtest_min_var <= 0:
        raise ValueError("microtest_min_var must be > 0")

    var_in = max(microtest_min_var, int(round(nvar * var_scale)))
    var_in = max(var_in, 1)

    if len(microtest_fixed_args) != 3:
        raise ValueError("microtest_fixed_args must have exactly 3 items (seed, literals, last)")

    cmd1 = [str(microtest_bin), "1", microtest_fixed_args[0], str(var_in), str(ncls), microtest_fixed_args[1],
            microtest_fixed_args[2], str(heuristic_name)]
    cmd2 = [str(microtest_bin), "2", microtest_fixed_args[0], str(var_in), str(ncls), microtest_fixed_args[1],
            microtest_fixed_args[2], str(heuristic_name)]

    p1 = subprocess.Popen(cmd1, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=PPCDCL_ROOT)
    p2 = subprocess.Popen(cmd2, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=PPCDCL_ROOT)

    try:
        out1, err1 = p1.communicate(timeout=microtest_timeout_sec)
        out2, err2 = p2.communicate(timeout=microtest_timeout_sec)
    except subprocess.TimeoutExpired:
        p1.kill()
        p2.kill()
        raise RuntimeError(f"microtest timed out after {microtest_timeout_sec}s (var_in={var_in}, ncls={ncls})")

    if p1.returncode != 0:
        raise RuntimeError(f"microtest party1 failed:\nCMD: {' '.join(cmd1)}\nSTDERR:\n{err1}\nSTDOUT:\n{out1}")
    if p2.returncode != 0:
        raise RuntimeError(f"microtest party2 failed:\nCMD: {' '.join(cmd2)}\nSTDERR:\n{err2}\nSTDOUT:\n{out2}")

    times_raw = _parse_microtest_party_output(out1)

    # Scale the per-step times linearly back to the original variable count.
    scale_up = (float(nvar) / float(var_in)) if var_in > 0 else 1.0
    times_scaled = {k: v * scale_up for k, v in times_raw.items()}

    return MicrotestCost(
        var_in=var_in,
        var_scale=var_scale,
        ncls=ncls,
        times_raw=times_raw,
        times_scaled=times_scaled,
    )


def run_ppcdcl_bench(params: Dict[str, int], verbose: bool = False) -> Dict[str, object]:
    return run_good_bench(
        params,
        bench_bin=BIN_BENCH,
        bench_port=GOOD_BENCH_PORT,
        conflict_factor=CONFLICT_FACTOR,
        global_sample_factor=GLOBAL_SAMPLE_FACTOR,
        conflict_sample_factor=CONFLICT_SAMPLE_FACTOR,
        verbose=verbose,
        use_oram_v=ORAM_V,
        use_shortest_wl=SHORTEST_WL,
        watch_backend=WATCH_BACKEND,
    )


@dataclass
class PpCdclCost:
    timed_out: bool
    params: Dict[str, int]
    times: Dict[str, object]


def get_ppcdcl_cost_from_params(params: Dict[str, int], timed_out: bool) -> PpCdclCost:
    if timed_out or not params:
        return PpCdclCost(timed_out=True, params=params, times={"total_time": TIMEOUT_PENALTY_TIME})
    times = run_ppcdcl_bench(params, verbose=VERBOSE)
    return PpCdclCost(timed_out=False, params=params, times=times)


# CSV output.

CSV_HEADER = [
    "cnf_path",
    "family",
    "cnf_name",
    "nvar",
    "ncls",
    # ppSAT steps
    "ppsat_htype",
    "ppsat_genotype",
    "ppsat_casenum",
    "ppsat_x_budget",
    "ppsat_steps_length",
    "ppsat_alpha",
    "ppsat_max_steps",
    "ppsat_steps_returned",
    "ppsat_steps_used",
    "ppsat_scaled_steps",
    "ppsat_x_value",
    # microtest (per-step)
    "micro_var_scale",
    "micro_var_in",
    "micro_total_time_raw",
    "micro_unit_search_raw",
    "micro_guess_raw",
    "micro_mux_raw",
    "micro_check_raw",
    "micro_backtrack_raw",
    "micro_propagation_raw",
    "micro_total_time_scaled",
    "micro_unit_search_scaled",
    "micro_guess_scaled",
    "micro_mux_scaled",
    "micro_check_scaled",
    "micro_backtrack_scaled",
    "micro_propagation_scaled",
    # ppSAT total estimated runtime (steps_used * per-step)
    "ppsat_total_time_est",
    "ppsat_unit_search_est",
    "ppsat_guess_est",
    "ppsat_mux_est",
    "ppsat_check_est",
    "ppsat_backtrack_est",
    "ppsat_propagation_est",
    # preprocessing / max_wl derivation
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
    # ppCDCL
    "ppcdcl_con_dly",
    "ppcdcl_dec_dly",
    "ppcdcl_watch_backend_requested",
    "ppcdcl_watch_backend_effective",
    "ppcdcl_watch_backend_reason",
    "ppcdcl_watch_occ_bits",
    "ppcdcl_watch_pair_bits",
    "ppcdcl_max_clause_in_wl",
    "ppcdcl_max_lit_in_con_clause",
    "ppcdcl_max_num_of_con_clause",
    "ppcdcl_timed_out",
    "ppcdcl_total_time",
    "ppcdcl_decision_time",
    "ppcdcl_from_u_time",
    "ppcdcl_conflict_time",
    "ppcdcl_regular_time",
    "ppcdcl_check_clause_time",
    "ppcdcl_update_wl_time",
    "ppcdcl_add_implication_time",
    # sum of the ppSAT estimate and the ppCDCL time
    "total_time_ppsat_plus_ppcdcl",
]

PPSAT_FIELDS = CSV_HEADER[: CSV_HEADER.index("preprocessed_cnf_path")]
PPSAT_CENSORED_DERIVED_FIELDS = (
    "ppsat_steps_used",
    "ppsat_scaled_steps",
    "ppsat_x_value",
    "ppsat_total_time_est",
    "ppsat_unit_search_est",
    "ppsat_guess_est",
    "ppsat_mux_est",
    "ppsat_check_est",
    "ppsat_backtrack_est",
    "ppsat_propagation_est",
)


def ensure_csv(path: Path) -> None:
    if not path.exists():
        with path.open("w", newline="") as f:
            csv.writer(f).writerow(CSV_HEADER)
        return
    with path.open("r", newline="") as f:
        existing_header = next(csv.reader(f), [])
    if existing_header != CSV_HEADER:
        raise SystemExit(
            f"Existing CSV header does not match the current schema: {path}. "
            "Use a fresh --out-csv path for this configuration."
        )


def write_row(path: Path, row: Dict[str, object]) -> None:
    with path.open("a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_HEADER)
        w.writerow(row)


def load_ppsat_cache(path: Optional[Path]) -> Dict[str, Dict[str, str]]:
    if path is None:
        return {}
    if not path.is_file():
        print(f"[cache] Reuse CSV not found at {path}, ignoring reuse option.")
        return {}
    with path.open("r", newline="") as f:
        return {row["cnf_path"]: row for row in csv.DictReader(f) if row.get("cnf_path")}


def parse_float(value: object) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        try:
            text = str(value).strip()
            if not text:
                return None
            return float(text)
        except (TypeError, ValueError):
            return None


def ppsat_row_timed_out(row: Mapping[str, object]) -> bool:
    """Return whether a row carries the ppSAT timeout sentinel (ppsat_steps_returned == -1)."""
    return parse_float(row.get("ppsat_steps_returned")) == -1.0


def ppsat_plus_ppcdcl_total(
    ppsat_total: Optional[float],
    ppcdcl_total: Optional[float],
    ppcdcl_timed_out: bool,
) -> float | str:
    """Return the sum of the two solver runtimes, or "" if either is missing or ppCDCL timed out."""
    if ppcdcl_timed_out:
        return ""
    if ppsat_total is None or ppcdcl_total is None:
        return ""
    return float(ppsat_total) + float(ppcdcl_total)


def existing_cnfs(path: Path) -> Set[str]:
    if not path.is_file():
        return set()
    with path.open("r", newline="") as f:
        return {row["cnf_path"] for row in csv.DictReader(f) if row.get("cnf_path")}


def _parse_cap_field(value: object) -> Optional[int]:
    try:
        text = str(value).strip()
    except Exception:
        return None
    if not text:
        return None
    try:
        return _sanitize_cap(int(text))
    except (TypeError, ValueError):
        try:
            return _sanitize_cap(int(float(text)))
        except (TypeError, ValueError):
            return None


def _parse_count_capacity_field(value: object) -> int:
    try:
        text = str(value).strip()
    except Exception:
        return 0
    if not text:
        return 0
    try:
        return max(0, int(float(text)))
    except (TypeError, ValueError):
        return 0


def _parse_watch_backend_field(value: object) -> str:
    """Return the canonical watch backend name (block, indexed or auto) for a CSV field."""
    text = str(value or "").strip().lower().replace("_", "-")
    if text in {"block", "block-local", "blocklocal", "packed"}:
        return "block"
    if text == "indexed":
        return "indexed"
    # A blank or unrecognized value, such as in a CSV without the backend
    # columns, is treated as auto.
    return "auto"


def existing_conlit_configs(
    path: Path,
    expected_splits: int,
    watch_backend: str = "auto",
) -> Dict[str, Set[Tuple[Optional[int], int]]]:
    """Return, per CNF, the (cap, count capacity) pairs with every split present for one watch backend."""
    if not path.is_file():
        return {}
    requested_backend = _parse_watch_backend_field(watch_backend)
    split_ids: Dict[Tuple[str, Optional[int], int, str], Counter[int]] = {}
    with path.open("r", newline="") as f:
        for row in csv.DictReader(f):
            cnf = row.get("cnf_path")
            if not cnf:
                continue
            has_ppcdcl = any(row.get(key) not in ("", None) for key in ("ppcdcl_total_time", "ppcdcl_max_lit_in_con_clause"))
            if not has_ppcdcl:
                continue
            raw_cap = row.get("preprocess_learned_cap")
            cap = _parse_cap_field(raw_cap)
            if cap is None and raw_cap is None:
                cap = _parse_cap_field(row.get("ppcdcl_max_lit_in_con_clause"))
            count_capacity = _parse_count_capacity_field(
                row.get("preprocess_max_conflict_clauses")
            )
            row_backend = _parse_watch_backend_field(
                row.get("ppcdcl_watch_backend_requested")
            )
            try:
                split_idx = int(str(row.get("preprocess_split_idx", "")).strip())
            except (TypeError, ValueError):
                continue
            split_ids.setdefault(
                (cnf, cap, count_capacity, row_backend), Counter()
            )[split_idx] += 1
    expected = Counter({idx: 1 for idx in range(expected_splits)})
    conlits: Dict[str, Set[Tuple[Optional[int], int]]] = {}
    for (cnf, cap, count_capacity, row_backend), observed in split_ids.items():
        if row_backend == requested_backend and observed == expected:
            conlits.setdefault(cnf, set()).add((cap, count_capacity))
    return conlits


def prune_incomplete_conlit_configs(path: Path, expected_splits: int) -> int:
    """Atomically remove partial or duplicate ppCDCL groups before a resume."""
    if not path.is_file():
        return 0
    with path.open("r", newline="") as f:
        rows = list(csv.DictReader(f))
    grouped: Dict[Tuple[str, Optional[int], int, str], Counter[int]] = {}
    for row in rows:
        cnf = row.get("cnf_path")
        has_ppcdcl = any(
            row.get(key) not in ("", None)
            for key in ("ppcdcl_total_time", "ppcdcl_max_lit_in_con_clause")
        )
        if not cnf or not has_ppcdcl:
            continue
        raw_cap = row.get("preprocess_learned_cap")
        cap = _parse_cap_field(raw_cap)
        if cap is None and raw_cap is None:
            cap = _parse_cap_field(row.get("ppcdcl_max_lit_in_con_clause"))
        count_capacity = _parse_count_capacity_field(
            row.get("preprocess_max_conflict_clauses")
        )
        row_backend = _parse_watch_backend_field(
            row.get("ppcdcl_watch_backend_requested")
        )
        try:
            split_idx = int(str(row.get("preprocess_split_idx", "")).strip())
        except (TypeError, ValueError):
            split_idx = -1
        grouped.setdefault(
            (cnf, cap, count_capacity, row_backend), Counter()
        )[split_idx] += 1
    expected = Counter({idx: 1 for idx in range(expected_splits)})
    incomplete = {key for key, observed in grouped.items() if observed != expected}
    if not incomplete:
        return 0

    kept: List[Dict[str, str]] = []
    removed = 0
    for row in rows:
        cnf = row.get("cnf_path")
        raw_cap = row.get("preprocess_learned_cap")
        cap = _parse_cap_field(raw_cap)
        if cap is None and raw_cap is None:
            cap = _parse_cap_field(row.get("ppcdcl_max_lit_in_con_clause"))
        count_capacity = _parse_count_capacity_field(
            row.get("preprocess_max_conflict_clauses")
        )
        row_backend = _parse_watch_backend_field(
            row.get("ppcdcl_watch_backend_requested")
        )
        if cnf and (cnf, cap, count_capacity, row_backend) in incomplete:
            removed += 1
        else:
            kept.append(row)

    temp_name: Optional[str] = None
    try:
        with tempfile.NamedTemporaryFile(
            "w",
            newline="",
            delete=False,
            dir=path.parent,
            prefix=f".{path.name}.resume-",
        ) as f:
            temp_name = f.name
            writer = csv.DictWriter(f, fieldnames=CSV_HEADER)
            writer.writeheader()
            writer.writerows(kept)
        os.replace(temp_name, path)
    finally:
        if temp_name is not None:
            Path(temp_name).unlink(missing_ok=True)
    return removed


def ppsat_from_cache(row: Dict[str, str]) -> Tuple[Dict[str, object], Optional[float]]:
    data = {key: row.get(key, "") for key in PPSAT_FIELDS}
    if ppsat_row_timed_out(row):
        for key in PPSAT_CENSORED_DERIVED_FIELDS:
            data[key] = ""
        return data, None
    return data, parse_float(row.get("ppsat_total_time_est"))


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


def blank_ppsat_data() -> Dict[str, object]:
    """Return empty ppSAT/micro fields for the CSV schema."""
    return {key: "" for key in PPSAT_FIELDS}


def _ppsat_only_executor(backend: str, jobs: int) -> Executor:
    if jobs <= 1:
        raise ValueError("jobs must be > 1 for parallel execution")
    text = (backend or "").strip().lower()
    if text == "thread":
        return ThreadPoolExecutor(max_workers=jobs)
    if text == "process":
        return ProcessPoolExecutor(max_workers=jobs)
    raise ValueError(f"Unknown parallel backend: {backend!r}")


def _compute_ppsat_only_row(
        *,
        cnf_path: str,
        family: str,
        heuristic: str,
        x_budget: float,
        microtest_bin: str,
        microtest_fixed_args: Tuple[str, str, str],
        microtest_var_scale: float,
        microtest_min_var: int,
        microtest_timeout_sec: int,
        formula_path: Optional[str] = None,
) -> Tuple[Dict[str, object], Optional[float], str]:
    cnf = Path(cnf_path)
    if not cnf.is_file():
        raise RuntimeError(f"CNF file not found: {cnf}")
    formula_src = Path(formula_path) if formula_path is not None else cnf
    if not formula_src.is_file():
        raise RuntimeError(f"CNF file not found: {formula_src}")

    row_base: Dict[str, object] = {
        "cnf_path": str(cnf.resolve()),
        "family": family,
        "cnf_name": cnf.name,
    }

    microtest_path = resolve_repo_path(microtest_bin)
    if not microtest_path.is_file():
        raise RuntimeError(f"microtest binary not found: {microtest_path}")

    nvar, ncls, formula = read_dimacs_formula(str(formula_src))

    micro = get_ppsat_step_cost_microtest_with_config(
        nvar=nvar,
        ncls=ncls,
        var_scale=microtest_var_scale,
        heuristic_name=heuristic,
        microtest_bin=microtest_path,
        microtest_fixed_args=list(microtest_fixed_args),
        microtest_min_var=microtest_min_var,
        microtest_timeout_sec=microtest_timeout_sec,
    )

    per_step_total = micro.times_scaled.get("total time", 0.0)
    if per_step_total <= 0:
        raise RuntimeError(f"microtest returned non-positive per-step total time for {cnf}")

    ppsat = get_ppsat_steps(
        cnf,
        heuristic,
        x_budget,
        per_step_total,
        parsed_formula=(nvar, ncls, formula),
    )

    steps_used = ppsat.steps_used
    per_step_scaled = micro.times_scaled

    def t(key: str) -> float:
        return per_step_scaled.get(key, 0.0)

    if ppsat.steps_returned == -1:
        ppsat_total_est = None
        ppsat_unit_est = None
        ppsat_guess_est = None
        ppsat_mux_est = None
        ppsat_check_est = None
        ppsat_bt_est = None
        ppsat_prop_est = None
    else:
        if steps_used is None:
            raise RuntimeError("Completed ppSAT search is missing its step count")
        ppsat_total_est = steps_used * t("total time")
        ppsat_unit_est = steps_used * t("unit search")
        ppsat_guess_est = steps_used * t("guess")
        ppsat_mux_est = steps_used * t("mux")
        ppsat_check_est = steps_used * t("check")
        ppsat_bt_est = steps_used * t("backtrack")
        ppsat_prop_est = steps_used * t("propagation")

    row: Dict[str, object] = {
        **row_base,
        "nvar": ppsat.nvar,
        "ncls": ppsat.ncls,
        "ppsat_htype": ppsat.htype,
        "ppsat_genotype": ppsat.genotype,
        "ppsat_casenum": ppsat.casenum,
        "ppsat_x_budget": ppsat.x_budget,
        "ppsat_steps_length": ppsat.steps_length,
        "ppsat_alpha": ppsat.alpha,
        "ppsat_max_steps": ppsat.max_steps,
        "ppsat_steps_returned": ppsat.steps_returned,
        "ppsat_steps_used": ppsat.steps_used,
        "ppsat_scaled_steps": ppsat.scaled_steps,
        "ppsat_x_value": ppsat.x_value,
        # micro raw
        "micro_var_scale": micro.var_scale,
        "micro_var_in": micro.var_in,
        "micro_total_time_raw": micro.times_raw.get("total time", ""),
        "micro_unit_search_raw": micro.times_raw.get("unit search", ""),
        "micro_guess_raw": micro.times_raw.get("guess", ""),
        "micro_mux_raw": micro.times_raw.get("mux", ""),
        "micro_check_raw": micro.times_raw.get("check", ""),
        "micro_backtrack_raw": micro.times_raw.get("backtrack", ""),
        "micro_propagation_raw": micro.times_raw.get("propagation", ""),
        # micro scaled
        "micro_total_time_scaled": micro.times_scaled.get("total time", ""),
        "micro_unit_search_scaled": micro.times_scaled.get("unit search", ""),
        "micro_guess_scaled": micro.times_scaled.get("guess", ""),
        "micro_mux_scaled": micro.times_scaled.get("mux", ""),
        "micro_check_scaled": micro.times_scaled.get("check", ""),
        "micro_backtrack_scaled": micro.times_scaled.get("backtrack", ""),
        "micro_propagation_scaled": micro.times_scaled.get("propagation", ""),
        # ppSAT totals
        "ppsat_total_time_est": ppsat_total_est,
        "ppsat_unit_search_est": ppsat_unit_est,
        "ppsat_guess_est": ppsat_guess_est,
        "ppsat_mux_est": ppsat_mux_est,
        "ppsat_check_est": ppsat_check_est,
        "ppsat_backtrack_est": ppsat_bt_est,
        "ppsat_propagation_est": ppsat_prop_est,
    }

    est_time_text = "censored" if ppsat_total_est is None else f"{ppsat_total_est:.2f}s"
    summary = (
        f"microtest: var_in={micro.var_in} (scale={micro.var_scale}), "
        f"raw={micro.times_raw.get('total time', 0.0):.6f}s, scaled={micro.times_scaled.get('total time', 0.0):.6f}s | "
        f"ppSAT: nvar={ppsat.nvar}, ncls={ppsat.ncls}, steps={ppsat.steps_returned}, "
        f"max_steps={ppsat.max_steps}, est_time={est_time_text}"
    )
    if formula_src != cnf:
        summary += f" (formula={formula_src})"
    return row, ppsat_total_est, summary


def _sanitize_cap(max_lits: Optional[int]) -> Optional[int]:
    if max_lits is None:
        return None
    try:
        v = int(max_lits)
    except (TypeError, ValueError):
        return None
    return v if v > 0 else None


WL_CHOICE_ALIASES = {"derived": "occ", "max": "max-all"}


def normalize_wl_choice(choice: str) -> str:
    text = (choice or "").strip().lower()
    return WL_CHOICE_ALIASES.get(text, text)


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
    # default: max-occ policy
    return _max_ignore_none([real_wl, occ_wl])


def run_preprocessing_splits(
        cnf_path: Path,
        backbone_bin: Optional[Path],
        *,
        best_params: bool,
        baseline_params: bool,
        max_conflict_lits: Optional[int],
        skip_preprocess_output: bool,
        rng_seed: int,
        splits: int,
        keep_outputs: bool = False,
) -> List[SplitArtifacts]:
    """Run preprocessing for each split and return the outcome of every split."""
    outcomes: List[SplitArtifacts] = []
    cap_lits = None if (best_params or baseline_params) else _sanitize_cap(max_conflict_lits)
    for split_idx in range(splits):
        split_rng = random.Random(rng_seed + split_idx)
        cleanup_combined = skip_preprocess_output and not keep_outputs
        print_preprocess_settings(
            f"{cnf_path.name} split={split_idx}",
            {
                "mode": "best" if best_params else ("baseline" if baseline_params else "fixed"),
                "ratio": PREPROCESS_RATIO,
                "seed": rng_seed + split_idx,
                "max_steps": PREPROCESS_MAX_STEPS,
                "learned_max_len": PREPROCESS_LEARNED_MAX_LEN,
                "learned_max_lbd": PREPROCESS_LEARNED_MAX_LBD,
                "max_confl_lits": cap_lits if cap_lits else "none",
                "max_conflict_clauses": FIXED_MAX_CONFLICT_CLAUSES,
                "cap_action": LEARNED_CAP_ACTION,
                "backbone_bin": backbone_bin,
                "backbone_timeout": PREPROCESS_BACKBONE_TIMEOUT,
                "test_big_timeout": PREPROCESS_TEST_BIG_TIMEOUT,
                "wl_choice": WL_CHOICE,
                "uip_loop_cap": UIP_LOOP_CAP,
                "derive_combined_uip": best_params,
            },
        )
        outcome = preprocess_split(
            cnf_path=cnf_path,
            ratio=PREPROCESS_RATIO,
            rng=split_rng,
            max_steps=PREPROCESS_MAX_STEPS,
            backbone_bin=backbone_bin,
            backbone_timeout=PREPROCESS_BACKBONE_TIMEOUT,
            learned_max_len=PREPROCESS_LEARNED_MAX_LEN,
            learned_max_lbd=PREPROCESS_LEARNED_MAX_LBD,
            con_dly=FIXED_CON_DLY,
            dec_dly=FIXED_DEC_DLY,
            cap_action=LEARNED_CAP_ACTION,
            test_big_timeout=PREPROCESS_TEST_BIG_TIMEOUT,
            out_dir=PREPROCESS_OUT_DIR,
            # Mirror the CNF's path relative to the repository root in the
            # output directory. Haplotype families reuse file names, so flat
            # output paths would make different CNFs share one file.
            base_input=PPCDCL_ROOT,
            split_idx=split_idx,
            test_big_bin=BIN_TEST_BIG,
            verbose=VERBOSE,
            max_confl_lits=cap_lits,
            max_conflict_clauses=FIXED_MAX_CONFLICT_CLAUSES,
            cleanup_combined=cleanup_combined,
            use_original_params_for_eval=skip_preprocess_output,
            wl_choice=WL_CHOICE,
            uip_loop_cap=UIP_LOOP_CAP,
            derive_combined_uip=best_params,
        )
        outcomes.append(outcome)
    if not outcomes:
        raise RuntimeError("No preprocessing outcome produced.")
    return outcomes


def bench_params_from_preprocess(
        outcome: SplitArtifacts,
        best_params: bool,
        baseline_params: bool,
        wl_choice: str,
        max_lit_in_conflict_clause: Optional[int],
        con_dly: int,
        dec_dly: int,
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

    chosen_wl = choose_wl_value(real_wl, occ_wl, solve_wl, combined_wl, best_params, wl_choice)
    if chosen_wl is not None:
        raw["max_clause_in_wl"] = int(chosen_wl)

    def to_int(val: object, default: int) -> int:
        try:
            return int(val)
        except (TypeError, ValueError):
            return default

    nvar = max(1, to_int(raw.get("var_num"), outcome.nvar))
    clause_num = max(1, to_int(raw.get("clause_num"), outcome.clauses_after))

    max_lit_in_clause = max(1, to_int(raw.get("max_lit_in_clause"), nvar))
    max_lit_in_clause = min(max_lit_in_clause, nvar)

    max_lit_in_con_clause = raw.get("max_lit_in_con_clause", max_lit_in_clause)
    max_lit_in_con_clause = max(1, to_int(max_lit_in_con_clause, max_lit_in_clause))
    max_lit_in_con_clause = min(max_lit_in_con_clause, nvar)
    cap_lits = _sanitize_cap(max_lit_in_conflict_clause)
    if baseline_params:
        max_lit_in_con_clause = nvar
        max_lit_source = "baseline:nvar"
    elif not best_params and cap_lits is not None:
        max_lit_in_con_clause = max(1, min(int(cap_lits), nvar))
        max_lit_source = "fixed:cap"
    else:
        max_lit_source = "best:plaintext" if best_params else "plaintext"
    if best_params:
        raw_uip = getattr(outcome, "combined_max_uip_loop", None)
        if raw_uip is not None and int(raw_uip) > 0:
            uip_cap = max(1, min(int(raw_uip), nvar))
            uip_source = "best:plaintext-uip"
        else:
            uip_cap = nvar
            uip_source = "best:fallback-nvar"
    elif baseline_params:
        uip_cap = nvar
        uip_source = "baseline:nvar"
    else:
        uip_cap = UIP_LOOP_CAP
        uip_source = "fixed:uip-loop-cap"

    wl_source = "best:plaintext-real" if best_params else (
        f"baseline:wl-choice:{wl_choice}" if baseline_params else f"fixed:wl-choice:{wl_choice}"
    )
    params: Dict[str, int] = {
        "var_num": nvar,
        "clause_num": clause_num,
        "max_num_of_con_clause": (
            FIXED_MAX_CONFLICT_CLAUSES
            if FIXED_MAX_CONFLICT_CLAUSES > 0
            else max(1, to_int(raw.get("max_num_of_con_clause"), 1))
        ),
        "max_lit_in_clause": max_lit_in_clause,
        "max_lit_in_con_clause": max_lit_in_con_clause,
        "max_clause_in_wl": max(1, to_int(raw.get("max_clause_in_wl"), 1)),
        "orange_total": max(1, to_int(raw.get("orange_total"), 1)),
        "yellow_total": max(1, to_int(raw.get("yellow_total"), 1)),
        "blue_total": max(1, to_int(raw.get("blue_total"), 1)),
        "red_total": max(1, to_int(raw.get("red_total"), 1)),
        "con_dly": con_dly,
        "dec_dly": dec_dly,
        "uip_cap": uip_cap,
        "_source_max_clause_in_wl": wl_source,
        "_source_max_lit_in_con_clause": max_lit_source,
        "_source_max_num_of_con_clause": (
            "fixed:public-capacity" if FIXED_MAX_CONFLICT_CLAUSES > 0 else "plaintext"
        ),
        "_source_uip_cap": uip_source,
        "_source_max_lit_in_clause": "plaintext",
    }

    # Report when clause lengths were capped at nvar.
    if max_lit_in_clause != to_int(raw.get("max_lit_in_clause"), nvar) or max_lit_in_con_clause != to_int(
            raw.get("max_lit_in_con_clause"), max_lit_in_clause
    ):
        print(
            f"[sanitize] Capped clause sizes to nvar={nvar}: "
            f"max_lit_in_clause={max_lit_in_clause}, max_lit_in_con_clause={max_lit_in_con_clause}"
        )

    return params


def preprocess_fields_dict(outcome: SplitArtifacts, cap_lits: Optional[int]) -> Dict[str, object]:
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
        "preprocess_max_conflict_clauses": int(outcome.max_conflict_clauses),
        "preprocess_cap_action": LEARNED_CAP_ACTION,
    }


def build_ppcdcl_configurations(include_best: bool, include_baseline: bool, sweep_caps: List[int]) -> List[PpcdclConfigSpec]:
    """Prepare the ppCDCL configurations to evaluate for each CNF."""
    configs: List[PpcdclConfigSpec] = []
    if include_best:
        configs.append(
            PpcdclConfigSpec(
                name="best_params",
                best_params=True,
                baseline_params=False,
                wl_choice=WL_CHOICE,
                max_lit_in_conflict_clause=None,
            )
        )
    if include_baseline:
        configs.append(
            PpcdclConfigSpec(
                name="baseline_params",
                best_params=False,
                baseline_params=True,
                wl_choice=WL_CHOICE,
                max_lit_in_conflict_clause=None,
            )
        )
    for cap in sweep_caps:
        clean = _sanitize_cap(cap)
        if clean is not None:
            configs.append(
                PpcdclConfigSpec(
                    name=f"wl_max_cap_{clean}",
                    best_params=False,
                    baseline_params=False,
                    wl_choice=WL_CHOICE,
                    max_lit_in_conflict_clause=clean,
                )
            )
    return configs


def collect_cnf_files(cnf_files: List[str], cnf_globs: List[str]) -> List[Path]:
    if cnf_files:
        return [Path(p) for p in cnf_files]
    out: List[Path] = []
    for g in cnf_globs:
        out.extend(Path(".").glob(g))
    uniq = sorted({p.resolve() for p in out if p.suffix == ".cnf"})
    return uniq


def main() -> None:
    args = parse_args()

    global CNF_FILES, DIR_TO_SEARCH, CNF_GLOBS, PPSAT_HEURISTIC, PPSAT_TIME_BUDGET_SEC, PPSAT_X_BUDGET
    global MICROTEST_BIN, MICROTEST_FIXED_ARGS, MICROTEST_VAR_SCALE, MICROTEST_MIN_VAR, MICROTEST_TIMEOUT_SEC
    global BIN_TEST_BIG, BIN_BENCH, GOOD_BENCH_PORT, FIXED_CON_DLY, FIXED_DEC_DLY, TEST_BIG_TIMEOUT
    global CONFLICT_FACTOR, GLOBAL_SAMPLE_FACTOR, CONFLICT_SAMPLE_FACTOR
    global MAX_LIT_IN_CONFLICT_CLAUSE, FIXED_MAX_CONFLICT_CLAUSES, MAX_LIT_IN_CONFLICT_CLAUSE_SWEEP, LEARNED_CAP_ACTION
    global PREPROCESS_RATIO, PREPROCESS_SEED, PREPROCESS_SPLITS, PREPROCESS_MAX_STEPS
    global PREPROCESS_LEARNED_MAX_LEN, PREPROCESS_LEARNED_MAX_LBD, PREPROCESS_BACKBONE_BIN
    global PREPROCESS_BACKBONE_TIMEOUT, PREPROCESS_OUT_DIR, PREPROCESS_TEST_BIG_TIMEOUT
    global REUSE_PPSAT_CSV, OUT_CSV, SKIP_ALREADY_DONE, VERBOSE, WL_CHOICE, ORAM_V, SHORTEST_WL, WATCH_BACKEND, BEST_PARAMS, BASELINE_PARAMS, SKIP_PREPROCESS_OUTPUT, NO_PPSAT
    global PPSAT_ONLY, PPSAT_PREPROCESSING, PPSAT_JOBS, PPSAT_PARALLEL_BACKEND
    global UIP_LOOP_CAP
    # CNF sources
    base_dirs = args.dir if args.dir else DIR_TO_SEARCH
    CNF_FILES = [str(Path(p)) for p in (args.cnf or [])]
    DIR_TO_SEARCH = list(base_dirs)
    for d in base_dirs:
        p = Path(d)
        if p.is_dir():
            CNF_FILES.extend(str(f) for f in p.rglob("*.cnf"))
    CNF_GLOBS = args.glob or CNF_GLOBS

    # ppSAT settings
    PPSAT_HEURISTIC = args.ppsat_heuristic
    PPSAT_TIME_BUDGET_SEC = args.ppsat_time_budget
    PPSAT_X_BUDGET = args.ppsat_x_budget

    # microtest settings
    MICROTEST_BIN = resolve_repo_path(args.microtest_bin)
    MICROTEST_FIXED_ARGS = args.microtest_fixed_args
    MICROTEST_VAR_SCALE = args.microtest_var_scale
    MICROTEST_MIN_VAR = args.microtest_min_var
    MICROTEST_TIMEOUT_SEC = args.microtest_timeout

    # bench binaries and timing
    BIN_TEST_BIG = resolve_repo_path(args.bin_test_big)
    BIN_BENCH = resolve_repo_path(args.bin_bench)
    GOOD_BENCH_PORT = args.bench_port
    FIXED_CON_DLY = args.fixed_con_dly
    FIXED_DEC_DLY = args.fixed_dec_dly
    TEST_BIG_TIMEOUT = args.test_big_timeout
    CONFLICT_FACTOR = args.conflict_factor
    GLOBAL_SAMPLE_FACTOR = int(args.global_sample_factor)
    CONFLICT_SAMPLE_FACTOR = args.conflict_sample_factor

    # preprocessing caps/heuristics
    MAX_LIT_IN_CONFLICT_CLAUSE = args.max_lit_in_conflict_clause
    FIXED_MAX_CONFLICT_CLAUSES = max(0, int(args.fixed_max_conflict_clauses))
    MAX_LIT_IN_CONFLICT_CLAUSE_SWEEP = args.max_lit_in_conflict_clause_sweep
    LEARNED_CAP_ACTION = args.learned_cap_action
    PREPROCESS_RATIO = args.preprocess_ratio
    PREPROCESS_SEED = args.preprocess_seed
    PREPROCESS_SPLITS = args.preprocess_splits
    PREPROCESS_MAX_STEPS = args.preprocess_max_steps
    PREPROCESS_LEARNED_MAX_LEN = args.preprocess_learned_max_len
    PREPROCESS_LEARNED_MAX_LBD = args.preprocess_learned_max_lbd
    PREPROCESS_BACKBONE_BIN = resolve_optional_repo_path(args.preprocess_backbone_bin)
    PREPROCESS_BACKBONE_TIMEOUT = args.preprocess_backbone_timeout
    PREPROCESS_OUT_DIR = args.preprocess_out_dir
    PREPROCESS_TEST_BIG_TIMEOUT = args.preprocess_test_big_timeout
    UIP_LOOP_CAP = args.uip_loop_cap

    REUSE_PPSAT_CSV = _opt_path(args.reuse_ppsat_csv)
    OUT_CSV = args.out_csv
    SKIP_ALREADY_DONE = args.skip_already_done
    VERBOSE = args.verbose
    ORAM_V = args.oram_v
    SHORTEST_WL = args.shortest_wl
    WATCH_BACKEND = args.watch_backend
    WL_CHOICE = normalize_wl_choice(args.wl_choice)
    args.wl_choice = WL_CHOICE
    BEST_PARAMS = args.best_params
    BASELINE_PARAMS = args.baseline_params
    if BEST_PARAMS and BASELINE_PARAMS:
        raise SystemExit("--best-params and --baseline-params are mutually exclusive.")
    SKIP_PREPROCESS_OUTPUT = args.skip_preprocess_output
    NO_PPSAT = args.no_ppsat
    PPSAT_ONLY = bool(args.ppsat_only)
    PPSAT_PREPROCESSING = bool(args.ppsat_preprocessing)
    PPSAT_JOBS = max(1, int(args.ppsat_jobs))
    PPSAT_PARALLEL_BACKEND = str(args.ppsat_parallel_backend)

    sweep_caps = list(MAX_LIT_IN_CONFLICT_CLAUSE_SWEEP) if MAX_LIT_IN_CONFLICT_CLAUSE_SWEEP else [MAX_LIT_IN_CONFLICT_CLAUSE]
    ppcdcl_configs: List[PpcdclConfigSpec] = []
    if not PPSAT_ONLY:
        ppcdcl_configs = build_ppcdcl_configurations(BEST_PARAMS, BASELINE_PARAMS, sweep_caps)
        if not ppcdcl_configs:
            raise SystemExit(
                "No ppCDCL configurations requested. Use --best-params, --baseline-params, "
                "or pass a positive --max-lit-in-conflict-clause/--max-lit-in-conflict-clause-sweep."
            )

    log_config(
        "compare config",
        {
            "cnf_inputs": CNF_FILES or CNF_GLOBS or DIR_TO_SEARCH,
            "bench_bin": BIN_BENCH,
            "bench_port": GOOD_BENCH_PORT,
            "test_big_bin": BIN_TEST_BIG,
            "fixed_con_dly": FIXED_CON_DLY,
            "fixed_dec_dly": FIXED_DEC_DLY,
            "conflict_factor": CONFLICT_FACTOR,
            "global_sample_factor": GLOBAL_SAMPLE_FACTOR,
            "conflict_sample_factor": CONFLICT_SAMPLE_FACTOR,
            "max_lit_in_conflict_clause": MAX_LIT_IN_CONFLICT_CLAUSE,
            "fixed_max_conflict_clauses": FIXED_MAX_CONFLICT_CLAUSES,
            "max_lit_in_conflict_clause_sweep": sweep_caps,
            "learned_cap_action": LEARNED_CAP_ACTION,
            "preprocess_ratio": PREPROCESS_RATIO,
            "preprocess_seed": PREPROCESS_SEED,
            "preprocess_splits": PREPROCESS_SPLITS,
            "preprocess_max_steps": PREPROCESS_MAX_STEPS,
            "preprocess_learned_max_len": PREPROCESS_LEARNED_MAX_LEN,
            "preprocess_learned_max_lbd": PREPROCESS_LEARNED_MAX_LBD,
            "preprocess_backbone_bin": PREPROCESS_BACKBONE_BIN,
            "preprocess_backbone_timeout": PREPROCESS_BACKBONE_TIMEOUT,
            "preprocess_out_dir": PREPROCESS_OUT_DIR,
            "preprocess_test_big_timeout": PREPROCESS_TEST_BIG_TIMEOUT,
            "reuse_ppsat_csv": REUSE_PPSAT_CSV,
            "out_csv": OUT_CSV,
            "skip_already_done": SKIP_ALREADY_DONE,
            "wl_choice": WL_CHOICE,
            "oram_v": ORAM_V,
            "watchlist_strategy": "shortest" if SHORTEST_WL else "first-fit",
            "watch_backend": WATCH_BACKEND,
            "best_params": BEST_PARAMS,
            "baseline_params": BASELINE_PARAMS,
            "no_ppsat": NO_PPSAT,
            "ppsat_only": PPSAT_ONLY,
            "ppsat_preprocessing": PPSAT_PREPROCESSING,
            "uip_loop_cap": UIP_LOOP_CAP,
            "ppsat_jobs": PPSAT_JOBS,
            "ppsat_parallel_backend": PPSAT_PARALLEL_BACKEND,
            "skip_preprocess_output": SKIP_PREPROCESS_OUTPUT,
            "ppcdcl_configs": [cfg.name for cfg in ppcdcl_configs],
        },
        VERBOSE,
    )

    cnfs = collect_cnf_files(CNF_FILES, CNF_GLOBS)
    print(f"Total CNF files collected: {len(cnfs)}")
    if not cnfs:
        raise SystemExit("No CNFs found. Fill CNF_FILES or CNF_GLOBS in the config section.")

    if PPSAT_PREPROCESSING and REUSE_PPSAT_CSV:
        print("[cache] Ignoring reuse_ppsat_csv because --ppsat-preprocessing needs split-specific ppSAT data.")
    ppsat_cache = {} if PPSAT_PREPROCESSING else load_ppsat_cache(REUSE_PPSAT_CSV)
    if not OUT_CSV.parent.exists():
        OUT_CSV.parent.mkdir(parents=True, exist_ok=True)
    ensure_csv(OUT_CSV)
    if SKIP_ALREADY_DONE:
        removed_partial_rows = prune_incomplete_conlit_configs(OUT_CSV, PREPROCESS_SPLITS)
        if removed_partial_rows:
            print(f"[resume] Removed {removed_partial_rows} incomplete ppCDCL row(s) from {OUT_CSV}.")
        done_cnfs = existing_cnfs(OUT_CSV)
        done_conlits = existing_conlit_configs(
            OUT_CSV, PREPROCESS_SPLITS, WATCH_BACKEND
        )
    else:
        done_cnfs = set()
        done_conlits = {}

    if PPSAT_ONLY:
        print(f"[+] CNFs: {len(cnfs)}")
        print(f"[+] Writing CSV: {OUT_CSV}")
        print(f"[+] ppSAT-only mode enabled (jobs={PPSAT_JOBS}, backend={PPSAT_PARALLEL_BACKEND})")
        print(f"[+] ppSAT heuristic={PPSAT_HEURISTIC}, x_budget={PPSAT_X_BUDGET}")
        if PPSAT_PREPROCESSING and SKIP_PREPROCESS_OUTPUT:
            print("[warn] Keeping preprocess outputs so ppSAT can read the preprocessed CNFs (--ppsat-preprocessing).")
        if REUSE_PPSAT_CSV:
            reuse_msg = f"[+] Reusing ppSAT data from {REUSE_PPSAT_CSV} when available"
            if NO_PPSAT:
                reuse_msg += " (ppSAT skipped when missing)"
            if PPSAT_PREPROCESSING:
                reuse_msg += " (ignored with --ppsat-preprocessing)"
            print(reuse_msg)
        elif NO_PPSAT:
            print("[+] ppSAT computation disabled (--no-ppsat); ppSAT columns will stay empty when not cached.")
        else:
            print(f"[+] microtest var_scale={MICROTEST_VAR_SCALE}")
        if PPSAT_PREPROCESSING:
            print(f"[+] ppSAT will use preprocessed CNFs for each split (splits={PREPROCESS_SPLITS}).")

        if PPSAT_PREPROCESSING:
            backbone_bin = normalize_backbone_bin(PREPROCESS_BACKBONE_BIN)
            cap_lits = None if (BEST_PARAMS or BASELINE_PARAMS) else _sanitize_cap(MAX_LIT_IN_CONFLICT_CLAUSE)
            if not BIN_TEST_BIG.is_file():
                raise SystemExit(f"test_big binary not found: {BIN_TEST_BIG}")
            for cnf in cnfs:
                cnf = cnf.resolve()
                if SKIP_ALREADY_DONE and str(cnf) in done_cnfs:
                    print(f"[+] Skipping already done: {cnf}")
                    continue

                family = cnf.parent.name
                if NO_PPSAT:
                    print(f"\n=== {cnf} ===")
                    print("[ppsat] Skipping ppSAT computation and leaving ppSAT columns blank (--no-ppsat).")
                    row = blank_ppsat_data()
                    row.update({"cnf_path": str(cnf), "family": family, "cnf_name": cnf.name})
                    write_row(OUT_CSV, row)
                    done_cnfs.add(str(cnf))
                    continue

                outcomes = run_preprocessing_splits(
                    cnf_path=cnf,
                    backbone_bin=backbone_bin,
                    best_params=BEST_PARAMS,
                    baseline_params=BASELINE_PARAMS,
                    max_conflict_lits=MAX_LIT_IN_CONFLICT_CLAUSE,
                    skip_preprocess_output=SKIP_PREPROCESS_OUTPUT,
                    rng_seed=PREPROCESS_SEED,
                    splits=PREPROCESS_SPLITS,
                    keep_outputs=True,
                )

                for outcome in outcomes:
                    print(f"\n=== {cnf} (split {outcome.split_idx}) ===")
                    row, _, summary = _compute_ppsat_only_row(
                        cnf_path=str(cnf),
                        family=family,
                        heuristic=PPSAT_HEURISTIC,
                        x_budget=PPSAT_X_BUDGET,
                        microtest_bin=str(MICROTEST_BIN),
                        microtest_fixed_args=tuple(MICROTEST_FIXED_ARGS),
                        microtest_var_scale=MICROTEST_VAR_SCALE,
                        microtest_min_var=MICROTEST_MIN_VAR,
                        microtest_timeout_sec=MICROTEST_TIMEOUT_SEC,
                        formula_path=str(outcome.combined_path),
                    )
                    print(summary)
                    row["nvar"] = outcome.nvar
                    row["ncls"] = outcome.clauses_after
                    row.update(preprocess_fields_dict(outcome, cap_lits))
                    write_row(OUT_CSV, row)
                done_cnfs.add(str(cnf))
        else:
            remaining: List[Tuple[Path, str]] = []
            for cnf in cnfs:
                cnf = cnf.resolve()
                if SKIP_ALREADY_DONE and str(cnf) in done_cnfs:
                    print(f"[+] Skipping already done: {cnf}")
                    continue

                family = cnf.parent.name
                cached_ppsat = ppsat_cache.get(str(cnf))
                if cached_ppsat:
                    print(f"\n=== {cnf} ===")
                    print("[ppsat] Using cached ppSAT/micro results.")
                    cached_data, _ = ppsat_from_cache(cached_ppsat)
                    row = dict(cached_data)
                    row.update({"cnf_path": str(cnf), "family": family, "cnf_name": cnf.name})
                    write_row(OUT_CSV, row)
                    done_cnfs.add(str(cnf))
                    continue

                if NO_PPSAT:
                    print(f"\n=== {cnf} ===")
                    print("[ppsat] Skipping ppSAT computation and leaving ppSAT columns blank (--no-ppsat).")
                    row = blank_ppsat_data()
                    row.update({"cnf_path": str(cnf), "family": family, "cnf_name": cnf.name})
                    write_row(OUT_CSV, row)
                    done_cnfs.add(str(cnf))
                    continue

                remaining.append((cnf, family))

            if remaining and not MICROTEST_BIN.is_file():
                raise SystemExit(f"microtest binary not found: {MICROTEST_BIN}")

            if remaining and PPSAT_JOBS > 1 and len(remaining) > 1:
                with _ppsat_only_executor(PPSAT_PARALLEL_BACKEND, PPSAT_JOBS) as ex:
                    future_to_cnf: Dict[Future[Tuple[Dict[str, object], Optional[float], str]], Path] = {}
                    for cnf, family in remaining:
                        fut = ex.submit(
                            _compute_ppsat_only_row,
                            cnf_path=str(cnf),
                            family=family,
                            heuristic=PPSAT_HEURISTIC,
                            x_budget=PPSAT_X_BUDGET,
                            microtest_bin=str(MICROTEST_BIN),
                            microtest_fixed_args=tuple(MICROTEST_FIXED_ARGS),
                            microtest_var_scale=MICROTEST_VAR_SCALE,
                            microtest_min_var=MICROTEST_MIN_VAR,
                            microtest_timeout_sec=MICROTEST_TIMEOUT_SEC,
                        )
                        future_to_cnf[fut] = cnf

                    for fut in as_completed(future_to_cnf):
                        cnf = future_to_cnf[fut]
                        row, _, summary = fut.result()
                        print(f"\n=== {cnf} ===")
                        print(summary)
                        write_row(OUT_CSV, row)
                        done_cnfs.add(str(cnf))
            else:
                for cnf, family in remaining:
                    print(f"\n=== {cnf} ===")
                    row, _, summary = _compute_ppsat_only_row(
                        cnf_path=str(cnf),
                        family=family,
                        heuristic=PPSAT_HEURISTIC,
                        x_budget=PPSAT_X_BUDGET,
                        microtest_bin=str(MICROTEST_BIN),
                        microtest_fixed_args=tuple(MICROTEST_FIXED_ARGS),
                        microtest_var_scale=MICROTEST_VAR_SCALE,
                        microtest_min_var=MICROTEST_MIN_VAR,
                        microtest_timeout_sec=MICROTEST_TIMEOUT_SEC,
                    )
                    print(summary)
                    write_row(OUT_CSV, row)
                    done_cnfs.add(str(cnf))

        print(f"\n[done] wrote {OUT_CSV}")
        return

    if not BIN_TEST_BIG.is_file():
        raise SystemExit(f"test_big binary not found: {BIN_TEST_BIG}")
    if not BIN_BENCH.is_file():
        raise SystemExit(f"ppcdcl bench binary not found: {BIN_BENCH}")

    backbone_bin = normalize_backbone_bin(PREPROCESS_BACKBONE_BIN)

    con_dly = FIXED_CON_DLY
    dec_dly = FIXED_DEC_DLY

    print(f"[+] CNFs: {len(cnfs)}")
    print(f"[+] Writing CSV: {OUT_CSV}")
    print(f"[+] ppSAT heuristic={PPSAT_HEURISTIC}, x_budget={PPSAT_X_BUDGET}")
    if REUSE_PPSAT_CSV:
        reuse_msg = f"[+] Reusing ppSAT data from {REUSE_PPSAT_CSV} when available"
        if NO_PPSAT:
            reuse_msg += " (ppSAT skipped when missing)"
        if PPSAT_PREPROCESSING:
            reuse_msg += " (ignored with --ppsat-preprocessing)"
        print(reuse_msg)
    elif NO_PPSAT:
        print("[+] ppSAT computation disabled (--no-ppsat); ppSAT columns will stay empty when not cached.")
    else:
        print(f"[+] microtest var_scale={MICROTEST_VAR_SCALE}")
    if PPSAT_PREPROCESSING:
        if SKIP_PREPROCESS_OUTPUT:
            print("[warn] Keeping preprocess outputs so ppSAT can read the preprocessed CNFs (--ppsat-preprocessing).")
        print(f"[+] ppSAT will use preprocessed CNFs for each split (splits={PREPROCESS_SPLITS}).")
    print(
        f"[+] preprocessing ratio={PREPROCESS_RATIO}, splits={PREPROCESS_SPLITS}, "
        f"learned_max_len={PREPROCESS_LEARNED_MAX_LEN}, learned_max_lbd={PREPROCESS_LEARNED_MAX_LBD}, "
        f"cap={MAX_LIT_IN_CONFLICT_CLAUSE if MAX_LIT_IN_CONFLICT_CLAUSE > 0 else 'none'}"
    )
    print(f"[+] ppCDCL con_dly={con_dly}, dec_dly={dec_dly}")
    print(
        f"[+] ppCDCL variable_state={'oram' if ORAM_V else 'linear'} "
        f"watchlist_strategy={'shortest' if SHORTEST_WL else 'first-fit'} "
        f"watch_backend={WATCH_BACKEND}"
    )
    print(f"[+] ppCDCL configs: {[cfg.name for cfg in ppcdcl_configs]} (sweep caps={sweep_caps})")

    for cnf in cnfs:
        cnf = cnf.resolve()
        cnf_key = str(cnf)
        existing_configs = done_conlits.get(cnf_key, set())
        configs_to_run = ppcdcl_configs
        if SKIP_ALREADY_DONE:
            configs_to_run = [
                cfg
                for cfg in ppcdcl_configs
                if cfg.best_params
                or cfg.baseline_params
                or (
                    _sanitize_cap(cfg.max_lit_in_conflict_clause),
                    FIXED_MAX_CONFLICT_CLAUSES,
                )
                not in existing_configs
            ]
            if not configs_to_run:
                print(f"[+] Skipping already done: {cnf}")
                continue
        family = cnf.parent.name

        print(f"\n=== {cnf} ===")

        cached_ppsat = ppsat_cache.get(str(cnf)) if not PPSAT_PREPROCESSING else None
        ppsat_total_est_base: Optional[float] = None
        ppsat_data_base: Optional[Dict[str, object]] = None
        ppsat_data_by_split: Dict[int, Tuple[Dict[str, object], Optional[float]]] = {}

        if cached_ppsat:
            print("[ppsat] Using cached ppSAT/micro results.")
            ppsat_data_base, ppsat_total_est_base = ppsat_from_cache(cached_ppsat)
        elif NO_PPSAT:
            print("[ppsat] Skipping ppSAT computation and leaving ppSAT columns blank (--no-ppsat).")
            ppsat_data_base = blank_ppsat_data()
        elif not PPSAT_PREPROCESSING:
            row, ppsat_total_est_base, summary = _compute_ppsat_only_row(
                cnf_path=str(cnf),
                family=family,
                heuristic=PPSAT_HEURISTIC,
                x_budget=PPSAT_X_BUDGET,
                microtest_bin=str(MICROTEST_BIN),
                microtest_fixed_args=tuple(MICROTEST_FIXED_ARGS),
                microtest_var_scale=MICROTEST_VAR_SCALE,
                microtest_min_var=MICROTEST_MIN_VAR,
                microtest_timeout_sec=MICROTEST_TIMEOUT_SEC,
            )
            print(summary)
            ppsat_data_base = row

        for cfg in configs_to_run:
            cap_lits = _sanitize_cap(cfg.max_lit_in_conflict_clause)
            outcomes = run_preprocessing_splits(
                cnf_path=cnf,
                backbone_bin=backbone_bin,
                best_params=cfg.best_params,
                baseline_params=cfg.baseline_params,
                max_conflict_lits=cap_lits,
                skip_preprocess_output=SKIP_PREPROCESS_OUTPUT,
                rng_seed=PREPROCESS_SEED,
                splits=PREPROCESS_SPLITS,
                keep_outputs=PPSAT_PREPROCESSING,
            )
            for outcome in outcomes:
                if PPSAT_PREPROCESSING:
                    if NO_PPSAT:
                        ppsat_data = blank_ppsat_data()
                        ppsat_total_est = None
                    else:
                        cached_split = ppsat_data_by_split.get(outcome.split_idx)
                        if cached_split is None:
                            row_split, split_total, summary = _compute_ppsat_only_row(
                                cnf_path=str(cnf),
                                family=family,
                                heuristic=PPSAT_HEURISTIC,
                                x_budget=PPSAT_X_BUDGET,
                                microtest_bin=str(MICROTEST_BIN),
                                microtest_fixed_args=tuple(MICROTEST_FIXED_ARGS),
                                microtest_var_scale=MICROTEST_VAR_SCALE,
                                microtest_min_var=MICROTEST_MIN_VAR,
                                microtest_timeout_sec=MICROTEST_TIMEOUT_SEC,
                                formula_path=str(outcome.combined_path),
                            )
                            print(f"[ppsat split={outcome.split_idx}] {summary}")
                            cached_split = (row_split, split_total)
                            ppsat_data_by_split[outcome.split_idx] = cached_split
                        row_split, ppsat_total_est = cached_split
                        ppsat_data = dict(row_split)
                    base_ncls = outcome.clauses_after
                else:
                    ppsat_data = dict(ppsat_data_base) if ppsat_data_base is not None else blank_ppsat_data()
                    ppsat_total_est = ppsat_total_est_base
                    base_ncls = outcome.clauses_before

                # Use the base fields set below, not the copies in the ppSAT row.
                for base_key in ["cnf_path", "family", "cnf_name", "nvar", "ncls"]:
                    ppsat_data.pop(base_key, None)

                bench_params = bench_params_from_preprocess(
                    outcome,
                    cfg.best_params,
                    cfg.baseline_params,
                    cfg.wl_choice,
                    cap_lits,
                    con_dly,
                    dec_dly,
                )
                print_ppcdcl_param_settings(f"{cnf.name} cfg={cfg.name} split={outcome.split_idx}", bench_params)
                ppcdcl_cost = get_ppcdcl_cost_from_params(bench_params, outcome.timed_out)
                cdcl_total = ppcdcl_cost.times.get("total_time", TIMEOUT_PENALTY_TIME)

                total_sum = ppsat_plus_ppcdcl_total(
                    ppsat_total_est,
                    cdcl_total,
                    ppcdcl_timed_out=ppcdcl_cost.timed_out,
                )

                row: Dict[str, object] = {
                    "cnf_path": str(cnf),
                    "family": family,
                    "cnf_name": cnf.name,
                    "nvar": outcome.nvar,
                    "ncls": base_ncls,
                }

                row.update(ppsat_data)
                row["nvar"] = outcome.nvar
                row["ncls"] = base_ncls

                row.update(preprocess_fields_dict(outcome, cap_lits))

                row.update(
                    {
                        "ppcdcl_con_dly": con_dly,
                        "ppcdcl_dec_dly": dec_dly,
                        "ppcdcl_watch_backend_requested": ppcdcl_cost.times.get(
                            "watch_backend_requested", WATCH_BACKEND
                        ),
                        "ppcdcl_watch_backend_effective": ppcdcl_cost.times.get(
                            "watch_backend_effective", ""
                        ),
                        "ppcdcl_watch_backend_reason": ppcdcl_cost.times.get(
                            "watch_backend_reason", ""
                        ),
                        "ppcdcl_watch_occ_bits": ppcdcl_cost.times.get(
                            "watch_backend_occ_bits", ppcdcl_cost.times.get("occ_bits", "")
                        ),
                        "ppcdcl_watch_pair_bits": ppcdcl_cost.times.get(
                            "watch_backend_pair_bits", ppcdcl_cost.times.get("pair_bits", "")
                        ),
                        "ppcdcl_max_clause_in_wl": bench_params.get("max_clause_in_wl", ""),
                        "ppcdcl_max_lit_in_con_clause": bench_params.get("max_lit_in_con_clause", ""),
                        "ppcdcl_max_num_of_con_clause": bench_params.get("max_num_of_con_clause", ""),
                        "ppcdcl_timed_out": int(ppcdcl_cost.timed_out),
                        "ppcdcl_total_time": cdcl_total,
                        "ppcdcl_decision_time": ppcdcl_cost.times.get("decision_time", ""),
                        "ppcdcl_from_u_time": ppcdcl_cost.times.get("from_u_time", ""),
                        "ppcdcl_conflict_time": ppcdcl_cost.times.get("conflict_time", ""),
                        "ppcdcl_regular_time": ppcdcl_cost.times.get("regular_time", ""),
                        "ppcdcl_check_clause_time": ppcdcl_cost.times.get("check_clause_time", ""),
                        "ppcdcl_update_wl_time": ppcdcl_cost.times.get("update_wl_time", ""),
                        "ppcdcl_add_implication_time": ppcdcl_cost.times.get("add_implication_time", ""),
                        "total_time_ppsat_plus_ppcdcl": total_sum,
                    }
                )

                print(
                    f"[{cfg.name} split={outcome.split_idx}] Summary: ppsat_total_est={ppsat_total_est} | preprocess_wl={outcome.used_max_wl} "
                    f"(derived-occ={outcome.derived_max_wl}, derived-solve={outcome.derived_solve_wl}, real={outcome.real_max_wl}) "
                    f"| ppcdcl_total={cdcl_total}"
                )

                write_row(OUT_CSV, row)
                if not (cfg.best_params or cfg.baseline_params):
                    done_conlits.setdefault(cnf_key, set()).add(
                        (cap_lits, FIXED_MAX_CONFLICT_CLAUSES)
                    )
        done_cnfs.add(str(cnf))

    print(f"\n[done] wrote {OUT_CSV}")


if __name__ == "__main__":
    main()
