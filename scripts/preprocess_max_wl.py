#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import random
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

from bench_utils import PPCDCL_ROOT, log_config, resolve_optional_repo_path, resolve_repo_path, run_test_big

CDCL_DUMP_BIN = Path("./bin/test_cdcl_dump")
TEST_BIG_BIN = Path("./bin/test_big")
BACKBONE_BIN = Path("./cadiback")

DEFAULT_RATIO = 0.5
DEFAULT_SEED = 0
DEFAULT_MAX_STEPS = 10_000_000
DEFAULT_LEARNED_MAX_LEN = 50
DEFAULT_LEARNED_MAX_LBD = 30
DEFAULT_BACKBONE_TIMEOUT = 10
DEFAULT_TEST_BIG_TIMEOUT = 10
DEFAULT_CDCL_DUMP_TIMEOUT = 300
DEFAULT_CON_DLY = 200
DEFAULT_DEC_DLY = 50
DEFAULT_CAP_ACTION = "restart"
DEFAULT_SPLIT_ATTEMPT_MULTIPLIER = 10

WL_CHOICE_ALIASES = {"derived": "occ", "max": "max-all"}
WL_CHOICE_OPTIONS = ["occ", "solve", "combined", "real", "max-occ", "max-solve", "max-combined", "max-all"]


@dataclass
class SplitArtifacts:
    party_a_path: Path
    party_b_path: Path
    combined_path: Path
    clauses_before: int
    clauses_after: int
    nvar: int
    party_a_max_lit_occ: Optional[int]
    party_b_max_lit_occ: Optional[int]
    combined_max_lit_occ: Optional[int]
    derived_max_wl: Optional[int]
    derived_solve_wl: Optional[int]
    real_max_wl: Optional[int]
    combined_params: Dict[str, int]
    split_a_real_wl: Optional[int]
    split_b_real_wl: Optional[int]
    used_max_wl: Optional[int]
    derived_ge_real: Optional[bool]
    solve_ge_real: Optional[bool]
    combined_ge_real: Optional[bool]
    real_params: Dict[str, int]
    combined_max_uip_loop: Optional[int]
    timed_out: bool
    plaintext_time: Optional[float]
    split_idx: int
    used_wl_choice: str
    max_conflict_clauses: int = 0


class BackboneUnsatError(RuntimeError):
    """Raised when CadiBack reports UNSAT (return code 20)."""

    def __init__(self, cnf_path: Path, returncode: int, stdout: str, stderr: str):
        super().__init__(f"cadiback reported UNSAT (rc={returncode}) for {cnf_path}")
        self.cnf_path = cnf_path
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def parse_dimacs(path: Path) -> Tuple[int, List[List[int]]]:
    nvar = None
    clauses: List[List[int]] = []
    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("c"):
                continue
            if line.startswith("p"):
                parts = line.split()
                if len(parts) >= 4 and parts[1] == "cnf":
                    nvar = int(parts[2])
                continue
            lits = [int(tok) for tok in line.split() if tok != "0"]
            if lits:
                clauses.append(lits)
    if nvar is None:
        raise RuntimeError(f"Missing 'p cnf' header in {path}")
    return nvar, clauses


def write_dimacs(path: Path, nvar: int, clauses: Sequence[Sequence[int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        f.write(f"p cnf {nvar} {len(clauses)}\n")
        for cls in clauses:
            f.write(" ".join(str(l) for l in cls) + " 0\n")


def split_clauses(clauses: List[List[int]], ratio: float, rng: random.Random) -> Tuple[List[List[int]], List[List[int]]]:
    shuffled = clauses[:]
    rng.shuffle(shuffled)
    cut = int(len(shuffled) * ratio)
    return shuffled[:cut], shuffled[cut:]


def dedupe_clauses(clauses: List[List[int]], label: Optional[str] = None) -> List[List[int]]:
    seen = set()
    out: List[List[int]] = []
    removed = 0
    for cls in clauses:
        key = tuple(sorted(cls))
        if key in seen:
            removed += 1
            continue
        seen.add(key)
        out.append(cls)
    return out


def _max_ignore_none(values: List[Optional[int]]) -> Optional[int]:
    present = [v for v in values if v is not None]
    return max(present) if present else None


def normalize_wl_choice(choice: str) -> str:
    text = (choice or "").strip().lower()
    return WL_CHOICE_ALIASES.get(text, text)


def wl_choice_needs_solve_diagnostics(choice: str) -> bool:
    """Return whether the watchlist size choice can use the per-party solve-derived bounds."""
    return normalize_wl_choice(choice) in {"solve", "max-solve", "max-all"}


def choose_wl_value(
    real_wl: Optional[int],
    occ_wl: Optional[int],
    solve_wl: Optional[int],
    combined_wl: Optional[int],
    wl_choice: str,
) -> Optional[int]:
    choice = normalize_wl_choice(wl_choice)
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
    return _max_ignore_none([real_wl, occ_wl, combined_wl])


def simplify_with_assignments(clauses: List[List[int]], assignments: Sequence[int], label: str) -> List[List[int]]:
    """
    Apply unit assignments by removing falsified literals from each clause.
    Clauses satisfied by an assignment are kept.
    """
    if not assignments:
        return clauses[:]
    true_lits = set(assignments)
    false_lits = {-lit for lit in assignments}
    simplified: List[List[int]] = []
    for cls in clauses:
        sat_lit = next((lit for lit in cls if lit in true_lits), None)
        new_cls = [lit for lit in cls if lit not in false_lits]
        if len(new_cls) != len(cls):
            removed = [lit for lit in cls if lit in false_lits]
        simplified.append(new_cls)
    return simplified


def max_literal_occurrence(clauses: List[List[int]]) -> Optional[int]:
    counts: Dict[int, int] = {}
    for cls in clauses:
        for lit in cls:
            counts[lit] = counts.get(lit, 0) + 1
    if not counts:
        return None
    return max(counts.values())


def filter_learned_clauses(
    clauses: List[List[int]],
    lbds: Optional[List[int]],
    max_len: Optional[int],
    max_lbd: Optional[int],
    label: str,
) -> List[List[int]]:
    if not clauses:
        return []
    apply_len = max_len is not None and max_len > 0
    apply_lbd = max_lbd is not None and max_lbd > 0
    if apply_lbd and (lbds is None or len(lbds) != len(clauses)):
        print(f"[learned] {label}: missing or mismatched LBD data; skipping LBD filter")
        apply_lbd = False

    kept: List[List[int]] = []
    skipped_len = 0
    skipped_lbd = 0
    for idx, clause in enumerate(clauses):
        if apply_len and len(clause) > max_len:
            skipped_len += 1
            continue
        if apply_lbd:
            try:
                lbd_val = int(lbds[idx])
            except (TypeError, ValueError):
                lbd_val = None
            if lbd_val is not None and lbd_val > max_lbd:
                skipped_lbd += 1
                continue
        kept.append(clause)

    if apply_len or apply_lbd:
        print(
            f"[learned] {label}: kept {len(kept)}/{len(clauses)} "
            f"(len>{max_len}: {skipped_len}, lbd>{max_lbd}: {skipped_lbd})"
        )
    return kept


def run_cdcl_dump(
    cnf_path: Path,
    max_steps: int,
    timeout: float = DEFAULT_CDCL_DUMP_TIMEOUT,
) -> dict:
    cdcl_dump_bin = resolve_repo_path(CDCL_DUMP_BIN)
    if not cdcl_dump_bin.is_file():
        raise SystemExit(
            f"cdcl_dump binary not found at {cdcl_dump_bin}. Build it first (cmake --build <build_dir>)."
        )
    cnf_path = Path(cnf_path).resolve(strict=False)
    cmd = [str(cdcl_dump_bin), str(cnf_path), str(max_steps)]
    try:
        res = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=PPCDCL_ROOT,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"cdcl_dump timed out after {float(timeout):g}s for {cnf_path}"
        ) from exc
    
    if res.returncode != 0:
        raise RuntimeError(f"cdcl_dump failed for {cnf_path}:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")
    try:
        return json.loads(res.stdout)
    
    except json.JSONDecodeError as e:
        raise RuntimeError(f"Failed to parse cdcl_dump JSON output for {cnf_path}: {e}\nOutput:\n{res.stdout}")


def parse_backbone_output(output: str) -> List[int]:
    backbone: List[int] = []
    for raw in output.splitlines():
        line = raw.strip()
        if not line or line.startswith("c"):
            continue
        if line.startswith("b"):
            parts = line.split()
            for tok in parts[1:]:
                try:
                    val = int(tok)
                except ValueError:
                    continue
                if val == 0:
                    return backbone
                backbone.append(val)
    return backbone


def run_backbone(cnf_path: Path, backbone_bin: Optional[Path], timeout: int) -> List[int]:
    backbone_bin = resolve_optional_repo_path(backbone_bin)
    if backbone_bin is None:
        return []
    if not backbone_bin.is_file():
        raise RuntimeError(f"backbone binary not found at {backbone_bin}")
    cnf_path = Path(cnf_path).resolve(strict=False)
    cmd = [str(backbone_bin), "-q", str(cnf_path)]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=PPCDCL_ROOT)
    except subprocess.TimeoutExpired:
        print(f"[backbone] TIMEOUT after {timeout}s on {cnf_path}")
        return []
    if res.returncode == 20:
        print(f"[backbone] UNSAT reported by cadiback on {cnf_path} (rc=20); discarding this split.")
        raise BackboneUnsatError(cnf_path, res.returncode, res.stdout, res.stderr)
    if res.returncode != 10:
        raise RuntimeError(
            f"backbone failed unexpectedly for {cnf_path} (rc={res.returncode}).\n"
            f"CMD: {' '.join(cmd)}\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"
        )
    return parse_backbone_output(res.stdout)


def output_dir_for_cnf(cnf_path: Path, base_input: Path, out_dir: Path) -> Path:
    if base_input.is_dir():
        try:
            rel = cnf_path.relative_to(base_input)
            return out_dir / rel.parent
        except ValueError:
            return out_dir
    return out_dir


def preprocess_split(
    cnf_path: Path,
    ratio: float,
    rng: random.Random,
    max_steps: int,
    backbone_bin: Optional[Path],
    backbone_timeout: int,
    learned_max_len: Optional[int],
    learned_max_lbd: Optional[int],
    con_dly: int,
    dec_dly: int,
    max_confl_lits: Optional[int],
    cap_action: str,
    test_big_timeout: int,
    out_dir: Path,
    base_input: Path,
    split_idx: int,
    test_big_bin: Path,
    verbose: bool = False,
    cleanup_combined: bool = False,
    use_original_params_for_eval: bool = False,
    wl_choice: str = "combined",
    uip_loop_cap: int = 0,
    max_conflict_clauses: int = 0,
    min_wl_override: int = 0,
    derive_combined_uip: bool = False,
    use_shortest_wl: bool = False,
) -> SplitArtifacts:
    learned_count_capacity = max(0, int(max_conflict_clauses or 0))
    log_config(
        "preprocess_split",
        {
            "cnf": cnf_path,
            "split_idx": split_idx,
            "ratio": ratio,
            "max_steps": max_steps,
            "learned_max_len": learned_max_len,
            "learned_max_lbd": learned_max_lbd,
            "backbone_bin": backbone_bin,
            "backbone_timeout": backbone_timeout,
            "con_dly": con_dly,
            "dec_dly": dec_dly,
            "max_confl_lits": max_confl_lits,
            "max_conflict_clauses": learned_count_capacity,
            "cap_action": cap_action,
            "test_big_timeout": test_big_timeout,
            "test_big_bin": test_big_bin,
            "min_wl_override": min_wl_override,
            "derive_combined_uip": derive_combined_uip,
            "watchlist_strategy": "shortest" if use_shortest_wl else "first-fit",
        },
        verbose,
    )
    nvar, clauses = parse_dimacs(cnf_path)
    wl_choice_norm = normalize_wl_choice(wl_choice)
    party_a_clauses, party_b_clauses = split_clauses(clauses, ratio, rng)

    target_dir = output_dir_for_cnf(cnf_path, base_input, out_dir)
    cnf_dir = target_dir / "cnfs" / cnf_path.stem
    cnf_dir.mkdir(parents=True, exist_ok=True)

    suffix = f"_split{split_idx}"
    party_a_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyA.cnf"
    party_b_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyB.cnf"
    combined_path = cnf_dir / f"{cnf_path.stem}{suffix}_preprocessed.cnf"

    write_dimacs(party_a_path, nvar, party_a_clauses)
    write_dimacs(party_b_path, nvar, party_b_clauses)

    print(
        f"[+] Wrote splits: {party_a_path} ({len(party_a_clauses)} clauses), "
        f"{party_b_path} ({len(party_b_clauses)} clauses)"
    )

    print("[+] Running cdcl_dump on party A...")
    party_a = run_cdcl_dump(party_a_path, max_steps)
    print("[+] Running cdcl_dump on party B...")
    party_b = run_cdcl_dump(party_b_path, max_steps)

    backbone_a: List[int] = []
    backbone_b: List[int] = []
    if backbone_bin is not None:
        print("[+] Running backbone extraction on party A...")
        backbone_a = run_backbone(party_a_path, backbone_bin, backbone_timeout)
        print("[+] Running backbone extraction on party B...")
        backbone_b = run_backbone(party_b_path, backbone_bin, backbone_timeout)
        if backbone_a or backbone_b:
            print(f"[+] Backbone literals: party A={len(backbone_a)} party B={len(backbone_b)}")

    learned_a = party_a.get("learned") or []
    learned_b = party_b.get("learned") or []
    learned_a_lbd = party_a.get("learned_lbd")
    learned_b_lbd = party_b.get("learned_lbd")

    filtered_learned_a = filter_learned_clauses(
        learned_a,
        learned_a_lbd,
        learned_max_len,
        learned_max_lbd,
        "party A",
    )
    filtered_learned_b = filter_learned_clauses(
        learned_b,
        learned_b_lbd,
        learned_max_len,
        learned_max_lbd,
        "party B",
    )

    units_a = party_a.get("units") or []
    units_b = party_b.get("units") or []
    backbone_unit_clauses = [[u] for u in backbone_a + backbone_b]
    new_unit_clauses = [[u] for u in units_a + units_b] + backbone_unit_clauses

    simplified_a = simplify_with_assignments(party_a_clauses, backbone_a, "party A")
    simplified_b = simplify_with_assignments(party_b_clauses, backbone_b, "party B")

    party_a_augmented = dedupe_clauses(
        simplified_a + [[u] for u in units_a + backbone_a] + filtered_learned_a, label="party A"
    )
    party_b_augmented = dedupe_clauses(
        simplified_b + [[u] for u in units_b + backbone_b] + filtered_learned_b, label="party B"
    )
    party_a_processed_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyA_simplified.cnf"
    party_b_processed_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyB_simplified.cnf"
    write_dimacs(party_a_processed_path, nvar, party_a_augmented)
    write_dimacs(party_b_processed_path, nvar, party_b_augmented)

    party_a_max_lit_occ = max_literal_occurrence(party_a_augmented)
    party_b_max_lit_occ = max_literal_occurrence(party_b_augmented)

    augmented = dedupe_clauses(
        simplified_a + simplified_b + new_unit_clauses + filtered_learned_a + filtered_learned_b,
        label="combined",
    )
    combined_max_lit_occ = max_literal_occurrence(augmented)
    backbone_added = len(backbone_a) + len(backbone_b)
    added_learned = len(filtered_learned_a) + len(filtered_learned_b)
    print(
        f"[+] Backbone literals added: {backbone_added} (A={len(backbone_a)} B={len(backbone_b)}) | "
        f"learned kept: {added_learned}"
    )
    print(
        f"[+] Vars: before={nvar} after={nvar} | "
        f"Clauses: before={len(clauses)} after={len(augmented)} "
        f"(added units/backbone={len(new_unit_clauses)})"
    )
    write_dimacs(combined_path, nvar, augmented)
    print(f"[+] Wrote preprocessed CNF: {combined_path} (clauses: {len(augmented)})")

    split_a_params: Dict[str, int] = {}
    split_b_params: Dict[str, int] = {}
    if wl_choice_needs_solve_diagnostics(wl_choice_norm):
        split_a_params, _, _ = run_test_big(
            party_a_processed_path,
            con_dly,
            dec_dly,
            test_big_timeout,
            cap=max_confl_lits,
            max_conflict_clauses=learned_count_capacity,
            cap_action=cap_action,
            test_big_bin=test_big_bin,
            verbose=verbose,
            use_shortest_wl=use_shortest_wl,
        )
        split_b_params, _, _ = run_test_big(
            party_b_processed_path,
            con_dly,
            dec_dly,
            test_big_timeout,
            cap=max_confl_lits,
            max_conflict_clauses=learned_count_capacity,
            cap_action=cap_action,
            test_big_bin=test_big_bin,
            verbose=verbose,
            use_shortest_wl=use_shortest_wl,
        )
    else:
        print(
            f"[wl] Skipping per-party solve-derived diagnostics for choice={wl_choice_norm}; "
            "they cannot affect the selected W."
        )

    def wl_from_params(p: Dict[str, int]) -> Optional[int]:
        return p.get("max_clause_in_wl") if p else None

    combined_wl = combined_max_lit_occ
    derived_max_wl = None
    if party_a_max_lit_occ is not None and party_b_max_lit_occ is not None:
        derived_max_wl = party_a_max_lit_occ + party_b_max_lit_occ

    split_a_real_wl = wl_from_params(split_a_params)
    split_b_real_wl = wl_from_params(split_b_params)
    derived_solve_wl = None
    if split_a_real_wl is not None and split_b_real_wl is not None:
        derived_solve_wl = split_a_real_wl + split_b_real_wl

    wl_override = choose_wl_value(
        None, derived_max_wl, derived_solve_wl, combined_wl, wl_choice_norm
    )
    if wl_override is None:
        wl_override = _max_ignore_none([derived_max_wl, derived_solve_wl, combined_wl])
    min_wl_override = max(0, int(min_wl_override or 0))
    if min_wl_override > 0:
        wl_override = max(wl_override or min_wl_override, min_wl_override)

    combined_max_uip_loop: Optional[int] = None
    if derive_combined_uip:
        print("[+] Running cdcl_dump on combined CNF for actual UIP loop bound...")
        combined_dump = run_cdcl_dump(combined_path, max_steps)
        stats = combined_dump.get("stats") or {}
        raw_uip = stats.get("max_uip_loop")
        if raw_uip is not None:
            combined_max_uip_loop = int(raw_uip)
            print(f"[+] Actual combined UIP loop max: {combined_max_uip_loop}")

    combined_params, combined_timed_out, combined_plaintext_time = run_test_big(
        combined_path,
        con_dly,
        dec_dly,
        test_big_timeout,
        max_wl_size=wl_override,
        cap=max_confl_lits,
        max_conflict_clauses=learned_count_capacity,
        cap_action=cap_action,
        test_big_bin=test_big_bin,
        verbose=verbose,
        uip_loop_cap=uip_loop_cap,
        use_shortest_wl=use_shortest_wl,
    )
    params = combined_params
    timed_out = combined_timed_out
    plaintext_time = combined_plaintext_time
    if use_original_params_for_eval:
        orig_params, orig_timed_out, orig_plaintext = run_test_big(
            cnf_path,
            con_dly,
            dec_dly,
            test_big_timeout,
            max_wl_size=wl_override,
            cap=max_confl_lits,
            max_conflict_clauses=learned_count_capacity,
            cap_action=cap_action,
            test_big_bin=test_big_bin,
            verbose=verbose,
            use_shortest_wl=use_shortest_wl,
        )
        params = orig_params
        timed_out = orig_timed_out
        plaintext_time = orig_plaintext
    if cleanup_combined:
        try:
            combined_path.unlink(missing_ok=True)
            party_a_processed_path.unlink(missing_ok=True)
            party_b_processed_path.unlink(missing_ok=True)
        except OSError:
            pass
    log_config(
        "test_big combined",
        {
            "cnf": combined_path,
            "timed_out": combined_timed_out,
            "plaintext_time": combined_plaintext_time,
            "real_params": combined_params,
            "wl_override": wl_override,
        },
        verbose,
    )

    real_max_wl = wl_from_params(params)

    derived_ge_real: Optional[bool]
    solve_ge_real: Optional[bool]
    combined_ge_real: Optional[bool]
    if derived_max_wl is None or real_max_wl is None:
        derived_ge_real = None
    else:
        derived_ge_real = derived_max_wl >= real_max_wl

    if derived_solve_wl is None or real_max_wl is None:
        solve_ge_real = None
    else:
        solve_ge_real = derived_solve_wl >= real_max_wl

    if combined_wl is None or real_max_wl is None:
        combined_ge_real = None
    else:
        combined_ge_real = combined_wl >= real_max_wl

    wl_candidates = [v for v in (real_max_wl, derived_max_wl, derived_solve_wl, combined_wl) if v is not None]
    used_max_wl = wl_override if wl_override is not None else real_max_wl
    if used_max_wl is None and wl_candidates:
        used_max_wl = max(wl_candidates)

    def _status(flag: Optional[bool]) -> str:
        if flag is None:
            return "N/A"
        return "OK" if flag else "LOW"

    print(
        f"[wl] choice={wl_choice_norm} used={used_max_wl} "
        f"(derived-combined={combined_wl}, derived-occ={derived_max_wl}, derived-solve={derived_solve_wl}, real={real_max_wl}) "
        f"status_occ={_status(derived_ge_real)} status_solve={_status(solve_ge_real)} status_combined={_status(combined_ge_real)}"
    )

    return SplitArtifacts(
        party_a_path=party_a_path,
        party_b_path=party_b_path,
        combined_path=combined_path,
        clauses_before=len(clauses),
        clauses_after=len(augmented),
        nvar=nvar,
        party_a_max_lit_occ=party_a_max_lit_occ,
        party_b_max_lit_occ=party_b_max_lit_occ,
        combined_max_lit_occ=combined_max_lit_occ,
        derived_max_wl=derived_max_wl,
        derived_solve_wl=derived_solve_wl,
        real_max_wl=real_max_wl,
        combined_params=combined_params,
        split_a_real_wl=split_a_real_wl,
        split_b_real_wl=split_b_real_wl,
        used_max_wl=used_max_wl,
        derived_ge_real=derived_ge_real,
        solve_ge_real=solve_ge_real,
        combined_ge_real=combined_ge_real,
        used_wl_choice=wl_choice_norm,
        real_params=params,
        combined_max_uip_loop=combined_max_uip_loop,
        timed_out=timed_out,
        plaintext_time=plaintext_time,
        split_idx=split_idx,
        max_conflict_clauses=learned_count_capacity,
    )


def preprocess_split_uip_heuristics(
    cnf_path: Path,
    ratio: float,
    rng: random.Random,
    max_steps: int,
    backbone_bin: Optional[Path],
    backbone_timeout: int,
    learned_max_len: Optional[int],
    learned_max_lbd: Optional[int],
    con_dly: int,
    dec_dly: int,
    max_confl_lits: Optional[int],
    cap_action: str,
    test_big_timeout: int,
    out_dir: Path,
    base_input: Path,
    split_idx: int,
    test_big_bin: Path,
    verbose: bool = False,
    cleanup_combined: bool = False,
    use_original_params_for_eval: bool = False,
    wl_choice: str = "combined",
    uip_loop_cap: int = 0,
) -> List[int]:
    nvar, clauses = parse_dimacs(cnf_path)
    party_a_clauses, party_b_clauses = split_clauses(clauses, ratio, rng)

    target_dir = output_dir_for_cnf(cnf_path, base_input, out_dir)
    cnf_dir = target_dir / "cnfs" / cnf_path.stem
    cnf_dir.mkdir(parents=True, exist_ok=True)

    suffix = f"_split{split_idx}"
    party_a_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyA.cnf"
    party_b_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyB.cnf"
    combined_path = cnf_dir / f"{cnf_path.stem}{suffix}_preprocessed.cnf"

    write_dimacs(party_a_path, nvar, party_a_clauses)
    write_dimacs(party_b_path, nvar, party_b_clauses)

    print(
        f"[+] Wrote splits: {party_a_path} ({len(party_a_clauses)} clauses), "
        f"{party_b_path} ({len(party_b_clauses)} clauses)"
    )

    print("[+] Running cdcl_dump on party A...")
    party_a = run_cdcl_dump(party_a_path, max_steps)
    print("[+] Running cdcl_dump on party B...")
    party_b = run_cdcl_dump(party_b_path, max_steps)

    backbone_a: List[int] = []
    backbone_b: List[int] = []
    if backbone_bin is not None:
        print("[+] Running backbone extraction on party A...")
        backbone_a = run_backbone(party_a_path, backbone_bin, backbone_timeout)
        print("[+] Running backbone extraction on party B...")
        backbone_b = run_backbone(party_b_path, backbone_bin, backbone_timeout)
        if backbone_a or backbone_b:
            print(f"[+] Backbone literals: party A={len(backbone_a)} party B={len(backbone_b)}")

    learned_a = party_a.get("learned") or []
    learned_b = party_b.get("learned") or []
    learned_a_lbd = party_a.get("learned_lbd")
    learned_b_lbd = party_b.get("learned_lbd")

    filtered_learned_a = filter_learned_clauses(
        learned_a,
        learned_a_lbd,
        learned_max_len,
        learned_max_lbd,
        "party A",
    )
    filtered_learned_b = filter_learned_clauses(
        learned_b,
        learned_b_lbd,
        learned_max_len,
        learned_max_lbd,
        "party B",
    )

    units_a = party_a.get("units") or []
    units_b = party_b.get("units") or []
    backbone_unit_clauses = [[u] for u in backbone_a + backbone_b]
    new_unit_clauses = [[u] for u in units_a + units_b] + backbone_unit_clauses

    simplified_a = simplify_with_assignments(party_a_clauses, backbone_a, "party A")
    simplified_b = simplify_with_assignments(party_b_clauses, backbone_b, "party B")

    party_a_augmented = dedupe_clauses(
        simplified_a + [[u] for u in units_a + backbone_a] + filtered_learned_a, label="party A"
    )
    party_b_augmented = dedupe_clauses(
        simplified_b + [[u] for u in units_b + backbone_b] + filtered_learned_b, label="party B"
    )
    party_a_processed_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyA_simplified.cnf"
    party_b_processed_path = cnf_dir / f"{cnf_path.stem}{suffix}_partyB_simplified.cnf"
    write_dimacs(party_a_processed_path, nvar, party_a_augmented)
    write_dimacs(party_b_processed_path, nvar, party_b_augmented)

    augmented = dedupe_clauses(
        simplified_a + simplified_b + new_unit_clauses + filtered_learned_a + filtered_learned_b,
        label="combined",
    )
    combined_max_lit_occ = max_literal_occurrence(augmented)
    backbone_added = len(backbone_a) + len(backbone_b)
    added_learned = len(filtered_learned_a) + len(filtered_learned_b)
    print(
        f"[+] Backbone literals added: {backbone_added} (A={len(backbone_a)} B={len(backbone_b)}) | "
        f"learned kept: {added_learned}"
    )
    print(
        f"[+] Vars: before={nvar} after={nvar} | "
        f"Clauses: before={len(clauses)} after={len(augmented)} "
        f"(added units/backbone={len(new_unit_clauses)})"
    )
    write_dimacs(combined_path, nvar, augmented)
    print(f"[+] Wrote preprocessed CNF: {combined_path} (clauses: {len(augmented)})")
    print("[+] Getting split a uip loop size Running cdcl_dump on party A...")
    party_a = run_cdcl_dump(party_a_path, max_steps)
    uip_real_loop_size_a = party_a.get("stats").get("max_uip_loop") or -1
    print(f"[+] UIP loop size for party A: {uip_real_loop_size_a}")
    print("[+] Getting split b uip loop size")
    party_b = run_cdcl_dump(party_b_path, max_steps)
    uip_real_loop_size_b = party_b.get("stats").get("max_uip_loop") or -1
    print(f"[+] UIP loop size for party B: {uip_real_loop_size_b}")
    party_combined = run_cdcl_dump(combined_path, max_steps)
    var_num_combined = party_combined.get("stats").get("var_num") or -1
    clause_num_combined = party_combined.get("stats").get("clause_count") or -1
    uip_real_loop_size_combined = party_combined.get("stats").get("max_uip_loop") or -1
    print(f"[+] UIP loop size for combined: {uip_real_loop_size_combined}")
    return [var_num_combined, clause_num_combined, uip_real_loop_size_a, uip_real_loop_size_b, uip_real_loop_size_combined]
    

    


def collect_cnfs(target: Path) -> List[Path]:
    if target.is_file():
        if target.suffix != ".cnf":
            raise SystemExit(f"Expected a .cnf file, got {target}")
        return [target]
    if not target.is_dir():
        raise SystemExit(f"{target} is neither a file nor a directory")
    cnfs = sorted([p for p in target.rglob("*.cnf") if p.is_file()])
    if not cnfs:
        raise SystemExit(f"No .cnf files found under {target}")
    return cnfs


def load_existing_cnfs(csv_path: Path) -> Set[str]:
    """
    Return the cnf_path values of a CSV, both as written and as resolved absolute paths,
    so that already-processed CNFs can be skipped.
    """
    if not csv_path.is_file():
        print(f"[skip] CSV not found at {csv_path}; not skipping any CNFs.")
        return set()
    with csv_path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames and "cnf_path" not in reader.fieldnames:
            print(f"[skip] CSV at {csv_path} has no 'cnf_path' column; not skipping any CNFs.")
            return set()
        existing_raw: Set[str] = set()
        existing_keys: Set[str] = set()
        for row in reader:
            cnf_val = (row.get("cnf_path") or "").strip()
            if not cnf_val:
                continue
            existing_raw.add(cnf_val)
            existing_keys.add(cnf_val)
            try:
                existing_keys.add(str(Path(cnf_val).resolve(strict=False)))
            except Exception:
                pass
    print(f"[skip] Loaded {len(existing_raw)} cnf_path entr{'y' if len(existing_raw) == 1 else 'ies'} from {csv_path}.")
    return existing_keys


def normalize_backbone_bin(backbone_bin: Optional[Path]) -> Optional[Path]:
    backbone_bin = resolve_optional_repo_path(backbone_bin)
    if backbone_bin is None:
        return None
    if not backbone_bin.is_file():
        raise RuntimeError(
            f"backbone binary not found at {backbone_bin}. "
            "Pass the backbone option as 'none' to disable extraction explicitly."
        )
    return backbone_bin


def outcome_to_dict(outcome: SplitArtifacts) -> Dict[str, object]:
    data = asdict(outcome)
    # Convert Paths to strings for serialization
    for key, value in list(data.items()):
        if isinstance(value, Path):
            data[key] = str(value)
    return data


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Preprocess CNFs via random splits, backbone simplification, and max_wl heuristics."
    )
    ap.add_argument("path", type=Path, help="Path to a CNF file or directory containing CNFs.")
    ap.add_argument("--ratio", type=float, default=DEFAULT_RATIO, help="Fraction of clauses to assign to party A.")
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED, help="Random seed.")
    ap.add_argument("--splits", type=int, default=1, help="Number of random splits to run per CNF.")
    ap.add_argument(
        "--split-attempt-cap",
        type=int,
        default=None,
        help=f"Maximum split attempts per CNF; defaults to splits*{DEFAULT_SPLIT_ATTEMPT_MULTIPLIER}.",
    )
    ap.add_argument("--max-steps", type=int, default=DEFAULT_MAX_STEPS, help="Max steps for cdcl_dump.")
    ap.add_argument(
        "--learned-max-len",
        type=int,
        default=DEFAULT_LEARNED_MAX_LEN,
        help="Drop learned clauses longer than this (<=0 disables).",
    )
    ap.add_argument(
        "--learned-max-lbd",
        type=int,
        default=DEFAULT_LEARNED_MAX_LBD,
        help="Drop learned clauses with LBD above this (<=0 disables).",
    )
    ap.add_argument(
        "--backbone-bin",
        type=Path,
        default=BACKBONE_BIN,
        help=f"Path to cadiback binary (default: {BACKBONE_BIN}; set to 'none' to disable).",
    )
    ap.add_argument(
        "--backbone-timeout",
        type=int,
        default=DEFAULT_BACKBONE_TIMEOUT,
        help="Timeout (seconds) for backbone extraction.",
    )
    ap.add_argument("--con-dly", type=int, default=DEFAULT_CON_DLY, help="CON_DLY parameter for test_big.")
    ap.add_argument("--dec-dly", type=int, default=DEFAULT_DEC_DLY, help="DEC_DLY parameter for test_big.")
    ap.add_argument(
        "--max-lit-in-confl-clause",
        type=int,
        default=0,
        help="Cap on learned/conflict clause length passed to test_big (<=0 disables).",
    )
    ap.add_argument(
        "--max-num-of-con-clause",
        type=int,
        default=0,
        help=(
            "Public learned-clause count capacity passed to test_big; once full, "
            "new learned clauses are rejected and the plaintext solver restarts (<=0 disables)."
        ),
    )
    ap.add_argument(
        "--cap-action",
        choices=["restart", "chrono"],
        default=DEFAULT_CAP_ACTION,
        help="Action when the learned clause cap is hit (test_big).",
    )
    ap.add_argument(
        "--test-big-timeout",
        type=int,
        default=DEFAULT_TEST_BIG_TIMEOUT,
        help="Timeout (seconds) for test_big on the combined CNF.",
    )
    ap.add_argument("--out-dir", type=Path, default=Path("preprocess_out"), help="Output directory for CNFs.")
    ap.add_argument(
        "--test-big-binary",
        type=Path,
        default=TEST_BIG_BIN,
        help="Path to the test_big binary.",
    )
    ap.add_argument(
        "--wl-choice",
        choices=WL_CHOICE_OPTIONS + ["derived", "max"],
        default="combined",
        help="Watchlist heuristic to label as used: occ, solve, combined (max literal occurrence on combined CNF), real, or max- variants.",
    )
    ap.add_argument(
        "--json-out",
        type=Path,
        default=None,
        help="Optional path to write JSON results for all splits.",
    )
    ap.add_argument(
        "--skip-existing-csv",
        type=Path,
        default=None,
        help="Skip CNFs already present in this CSV (uses cnf_path column).",
    )
    ap.add_argument(
        "--verbose",
        action="store_true",
        help="Print detailed configuration and command lines.",
    )
    args = ap.parse_args()

    split_attempt_cap = args.split_attempt_cap
    if split_attempt_cap is None:
        split_attempt_cap = args.splits * DEFAULT_SPLIT_ATTEMPT_MULTIPLIER
    if split_attempt_cap <= 0:
        raise SystemExit("split-attempt-cap must be positive.")
    if split_attempt_cap < args.splits:
        raise SystemExit("split-attempt-cap must be at least as large as the requested number of splits.")

    log_config(
        "preprocess_max_wl config",
        {
            "path": args.path,
            "ratio": args.ratio,
            "seed": args.seed,
            "splits": args.splits,
            "max_steps": args.max_steps,
            "learned_max_len": args.learned_max_len,
            "learned_max_lbd": args.learned_max_lbd,
            "backbone_bin": args.backbone_bin,
            "backbone_timeout": args.backbone_timeout,
            "con_dly": args.con_dly,
            "dec_dly": args.dec_dly,
            "max_lit_in_confl_clause": args.max_lit_in_confl_clause,
            "max_num_of_con_clause": args.max_num_of_con_clause,
            "cap_action": args.cap_action,
            "test_big_timeout": args.test_big_timeout,
            "test_big_bin": args.test_big_binary,
            "out_dir": args.out_dir,
            "json_out": args.json_out,
            "wl_choice": args.wl_choice,
            "skip_existing_csv": args.skip_existing_csv,
            "split_attempt_cap": split_attempt_cap,
        },
        args.verbose,
    )

    backbone_bin = normalize_backbone_bin(args.backbone_bin)
    cnf_paths = collect_cnfs(args.path)
    rng = random.Random(args.seed)
    skip_cnfs: Set[str] = set()
    if args.skip_existing_csv is not None:
        skip_cnfs = load_existing_cnfs(args.skip_existing_csv)

    all_outcomes: List[SplitArtifacts] = []
    for cnf_path in cnf_paths:
        cnf_key = str(cnf_path)
        cnf_key_resolved = str(cnf_path.resolve())
        if skip_cnfs and (cnf_key in skip_cnfs or cnf_key_resolved in skip_cnfs):
            print(f"Skipping {cnf_path} (already present in {args.skip_existing_csv})")
            continue
        print(f"\n=== Processing {cnf_path} ===")
        successful_splits = 0
        attempts = 0
        while successful_splits < args.splits:
            if attempts >= split_attempt_cap:
                raise SystemExit(
                    f"Reached split attempt cap ({split_attempt_cap}) for {cnf_path} "
                    f"with only {successful_splits}/{args.splits} SAT splits found."
                )
            attempt_no = attempts + 1
            print(
                f"--- split {successful_splits + 1}/{args.splits} "
                f"(attempt {attempt_no}/{split_attempt_cap}) ---"
            )
            try:
                outcome = preprocess_split(
                    cnf_path=cnf_path,
                    ratio=args.ratio,
                    rng=rng,
                    max_steps=args.max_steps,
                    backbone_bin=backbone_bin,
                    backbone_timeout=args.backbone_timeout,
                    learned_max_len=args.learned_max_len,
                    learned_max_lbd=args.learned_max_lbd,
                    con_dly=args.con_dly,
                    dec_dly=args.dec_dly,
                    max_confl_lits=args.max_lit_in_confl_clause,
                    max_conflict_clauses=args.max_num_of_con_clause,
                    cap_action=args.cap_action,
                    test_big_timeout=args.test_big_timeout,
                    out_dir=args.out_dir,
                    base_input=args.path,
                    split_idx=successful_splits,
                    test_big_bin=args.test_big_binary,
                    verbose=args.verbose,
                    wl_choice=args.wl_choice,
                )
            except BackboneUnsatError:
                attempts += 1
                print(
                    f"[split] cadiback reported UNSAT on attempt {attempt_no} for {cnf_path}; "
                    "retrying with a new random split."
                )
                continue
            attempts += 1
            all_outcomes.append(outcome)

            status_occ = "N/A"
            status_solve = "N/A"
            if outcome.derived_ge_real is True:
                status_occ = "OK"
            elif outcome.derived_ge_real is False:
                status_occ = "LOW"
            if outcome.solve_ge_real is True:
                status_solve = "OK"
            elif outcome.solve_ge_real is False:
                status_solve = "LOW"
            print(
                f"[result] used_wl={outcome.used_max_wl} "
                f"(derived-occ={outcome.derived_max_wl}, derived-solve={outcome.derived_solve_wl}, real={outcome.real_max_wl}) "
                f"status_occ={status_occ} status_solve={status_solve} timed_out={outcome.timed_out}"
            )
            successful_splits += 1

    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        with args.json_out.open("w", encoding="utf-8") as f:
            json.dump([outcome_to_dict(o) for o in all_outcomes], f, indent=2)
        print(f"Wrote JSON results to {args.json_out}")


if __name__ == "__main__":
    main()
