#!/usr/bin/env python3
from __future__ import annotations

import csv
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Optional
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from bench_utils import (  # noqa: E402
    _sanitize_count,
    count_modulo_one_slots,
    parse_test_big_output,
    run_good_bench,
    run_test_big,
)
import e2e_comparison  # noqa: E402
import compare as compare_script  # noqa: E402
from e2e_comparison import PpcdclConfigSpec, bench_params_from_preprocess, projection_error_percent  # noqa: E402
from preprocess_max_wl import wl_choice_needs_solve_diagnostics  # noqa: E402


def make_outcome(clause_num: int) -> SimpleNamespace:
    return SimpleNamespace(
        real_params={
            "var_num": 3,
            "clause_num": clause_num,
            "max_num_of_con_clause": 1,
            "max_lit_in_clause": 2,
            "max_lit_in_con_clause": 2,
            "max_clause_in_wl": 8,
            "orange_total": 3,
            "yellow_total": 3,
            "blue_total": 1,
            "red_total": 3,
        },
        real_max_wl=8,
        derived_max_wl=8,
        derived_solve_wl=8,
        combined_max_lit_occ=8,
        combined_max_uip_loop=None,
        nvar=3,
        clauses_after=2,
    )


class BenchmarkClauseCountTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.cnf = Path(self.temp_dir.name) / "two-clauses.cnf"
        self.cnf.write_text("c regression fixture\np cnf 3 2\n1 2 0\n-1 3 0\n", encoding="utf-8")
        self.cfg = PpcdclConfigSpec(
            name="fixed",
            best_params=False,
            baseline_params=False,
            wl_choice="combined",
            max_lit_in_conflict_clause=2,
        )

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def build_params(self, clause_num: int) -> dict[str, int]:
        return bench_params_from_preprocess(
            make_outcome(clause_num),
            self.cfg,
            max_lit_in_conflict_clause=2,
            con_dly=1,
            dec_dly=1,
            uip_loop_cap=3,
            evaluation_cnf=self.cnf,
        )

    def run_test_big_with_stdout(
        self,
        conflict_delay: int = 1,
        decision_delay: int = 1,
        cnf_path: Optional[Path] = None,
    ) -> tuple[dict[str, int], str]:
        if cnf_path is None:
            cnf_path = self.cnf
        completed = subprocess.run(
            [
                str(REPO_ROOT / "bin" / "test_big"),
                str(cnf_path),
                str(conflict_delay),
                str(decision_delay),
            ],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        params, _ = parse_test_big_output(completed.stdout, completed.args)
        return params, completed.stdout

    def run_test_big(self, conflict_delay: int = 1, decision_delay: int = 1) -> dict[str, int]:
        params, _ = self.run_test_big_with_stdout(conflict_delay, decision_delay)
        return params

    def test_estimator_clause_count_equals_dimacs_header(self) -> None:
        self.assertEqual(self.build_params(2)["clause_num"], 2)

    @unittest.skipUnless((REPO_ROOT / "bin" / "test_big").is_file(), "test_big has not been built")
    def test_test_big_excludes_internal_clause_sentinel(self) -> None:
        params = self.run_test_big()
        self.assertEqual(params["clause_num"], 2)

    @unittest.skipUnless((REPO_ROOT / "bin" / "test_big").is_file(), "test_big has not been built")
    def test_schedule_counts_use_paper_one_based_step_range(self) -> None:
        params = self.run_test_big(conflict_delay=10, decision_delay=10)
        steps = params["orange_total"]
        self.assertGreaterEqual(steps, 1)
        expected_slots = 1 + (steps - 1) // 10
        self.assertEqual(params["red_total"], expected_slots)
        self.assertEqual(params["yellow_total"], expected_slots)

    @unittest.skipUnless((REPO_ROOT / "bin" / "test_big").is_file(), "test_big has not been built")
    def test_orange_total_has_no_synthetic_step_zero(self) -> None:
        params, stdout = self.run_test_big_with_stdout(conflict_delay=10, decision_delay=10)
        internal_counts = re.findall(r"Orange blocks \(core\):\s*(\d+)", stdout)
        self.assertTrue(internal_counts)
        self.assertEqual(params["orange_total"], int(internal_counts[-1]))

    @unittest.skipUnless((REPO_ROOT / "bin" / "test_big").is_file(), "test_big has not been built")
    def test_root_unit_conflict_has_zero_public_steps_and_slots(self) -> None:
        params, _ = self.run_test_big_with_stdout(
            conflict_delay=10,
            decision_delay=10,
            cnf_path=REPO_ROOT / "test" / "cnfs" / "unit_conflict.cnf",
        )
        self.assertEqual(params["orange_total"], 0)
        self.assertEqual(params["yellow_total"], 0)
        self.assertEqual(params["red_total"], 0)

    @unittest.skipUnless((REPO_ROOT / "bin" / "test_big").is_file(), "test_big has not been built")
    def test_learned_capacity_is_derived_from_requested_solver_profile(self) -> None:
        completed = subprocess.run(
            [
                str(REPO_ROOT / "bin" / "test_big"),
                str(REPO_ROOT / "uf20-01.cnf"),
                "10",
                "10",
                "--max-wl-size",
                "14",
                "--learned-clause-cap",
                "10",
                "--cap-action",
                "restart",
                "--no-shortest-wl",
                "--uip-cap",
                "30",
            ],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        )
        params, _ = parse_test_big_output(completed.stdout, completed.args)
        # The solver's public default is positive polarity; capacity discovery
        # must follow that exact trajectory.
        self.assertEqual(params["max_num_of_con_clause"], 4)
        # Algorithm 1 checks SAT only after the final pending unit's complete
        # watchlist frame, so this profile includes that last 14-step frame.
        self.assertEqual(params["orange_total"], 765)

    @patch("bench_utils.subprocess.run")
    def test_run_test_big_forwards_public_learned_clause_count_capacity(self, run_mock) -> None:
        run_mock.return_value = SimpleNamespace(
            stdout="3 2 1 2 2 8 3 1 0 1 10 10\n",
        )

        params, timed_out, _ = run_test_big(
            self.cnf,
            10,
            10,
            1,
            max_conflict_clauses=10_000,
            test_big_bin=self.cnf,
        )

        self.assertFalse(timed_out)
        self.assertEqual(params["max_num_of_con_clause"], 1)
        command = run_mock.call_args.args[0]
        cap_index = command.index("--max-num-of-con-clause")
        self.assertEqual(command[cap_index + 1], "10000")

    def test_modulo_one_slot_count_boundaries(self) -> None:
        self.assertEqual(count_modulo_one_slots(0, 10), 0)
        self.assertEqual(count_modulo_one_slots(1, 10), 1)
        self.assertEqual(count_modulo_one_slots(10, 10), 1)
        self.assertEqual(count_modulo_one_slots(11, 10), 2)
        self.assertEqual(count_modulo_one_slots(21, 10), 3)

    def test_parser_rejects_legacy_modulo_zero_counts(self) -> None:
        legacy_output = "3 2 1 2 2 8 10 1 0 0 10 10\n"
        with self.assertRaisesRegex(RuntimeError, r"t == 1 \(mod delay\)"):
            parse_test_big_output(legacy_output)

    def test_bench_params_rederive_schedule_for_selected_delays(self) -> None:
        outcome = make_outcome(2)
        outcome.real_params.update(
            {
                "orange_total": 21,
                # Deliberately stale counts from a different delay.
                "yellow_total": 21,
                "red_total": 21,
                "blue_total": 0,
            }
        )
        params = bench_params_from_preprocess(
            outcome,
            self.cfg,
            max_lit_in_conflict_clause=2,
            con_dly=10,
            dec_dly=4,
            uip_loop_cap=3,
            evaluation_cnf=self.cnf,
        )
        self.assertEqual(params["orange_total"], 21)
        self.assertEqual(params["yellow_total"], 6)
        self.assertEqual(params["red_total"], 3)
        self.assertEqual(params["blue_total"], 0)

    def test_e2e_fixed_learned_count_capacity_overrides_plaintext_value(self) -> None:
        params = bench_params_from_preprocess(
            make_outcome(2),
            self.cfg,
            max_lit_in_conflict_clause=2,
            con_dly=10,
            dec_dly=10,
            uip_loop_cap=3,
            evaluation_cnf=self.cnf,
            fixed_max_conflict_clauses=10_000,
        )
        self.assertEqual(params["max_num_of_con_clause"], 10_000)
        self.assertEqual(params["_source_max_num_of_con_clause"], "fixed:public-capacity")

        legacy_params = self.build_params(2)
        self.assertEqual(legacy_params["max_num_of_con_clause"], 1)
        self.assertEqual(legacy_params["_source_max_num_of_con_clause"], "plaintext")

    def test_e2e_cli_learned_count_capacity_is_opt_in(self) -> None:
        with patch.object(sys, "argv", ["e2e_comparison.py"]):
            legacy_args = e2e_comparison.parse_args()
        self.assertEqual(legacy_args.fixed_max_conflict_clauses, 0)

        with patch.object(
            sys,
            "argv",
            ["e2e_comparison.py", "--fixed-max-conflict-clauses", "10000"],
        ):
            paper_args = e2e_comparison.parse_args()
        self.assertEqual(paper_args.fixed_max_conflict_clauses, 10_000)

    def test_e2e_cli_watch_backend_is_public_and_explicit(self) -> None:
        with patch.object(sys, "argv", ["e2e_comparison.py"]):
            default_args = e2e_comparison.parse_args()
        self.assertEqual(default_args.watch_backend, "auto")

        with patch.object(
            sys,
            "argv",
            ["e2e_comparison.py", "--watch-backend", "block"],
        ):
            block_args = e2e_comparison.parse_args()
        self.assertEqual(block_args.watch_backend, "block")

    def test_e2e_parses_watch_backend_config_with_multiword_reason(self) -> None:
        parsed = e2e_comparison._parse_e2e_output(
            "WATCH_BACKEND_CONFIG party=1 requested=auto effective=block-local "
            "reason=auto public cost: block-local=256, indexed=512, W=8 "
            "occ_bits=8 pair_bits=6\n"
        )
        self.assertEqual(parsed["e2e_watch_backend_requested_party1"], "auto")
        self.assertEqual(parsed["e2e_watch_backend_effective_party1"], "block")
        self.assertEqual(
            parsed["e2e_watch_backend_reason_party1"],
            "auto public cost: block-local=256, indexed=512, W=8",
        )

    def test_projection_forwards_and_records_watch_backend(self) -> None:
        args = SimpleNamespace(
            bin_bench=self.cnf,
            bench_port="1234",
            conflict_factor=1,
            global_sample_factor=1,
            conflict_sample_factor=1,
            verbose=False,
            oram_v=False,
            shortest_wl=False,
            backend_threads=2,
            watch_backend="block",
        )
        bench_result = {
            "watch_backend_requested": "block-local",
            "watch_backend_effective": "block-local",
            "watch_backend_reason": "explicit block-local request",
            "total_time": 1.0,
        }
        with patch("e2e_comparison.run_good_bench", return_value=bench_result) as bench_mock:
            projected = e2e_comparison.run_projected_bench(
                args, self.build_params(2), timed_out=False
            )

        self.assertEqual(bench_mock.call_args.kwargs["watch_backend"], "block")
        self.assertEqual(projected["projected_watch_backend_requested"], "block")
        self.assertEqual(projected["projected_watch_backend_effective"], "block")
        self.assertEqual(
            projected["projected_watch_backend_reason"],
            "explicit block-local request",
        )

    def test_e2e_forwards_learned_count_capacity_to_plaintext_preprocessing(self) -> None:
        args = SimpleNamespace(
            fixed_max_conflict_clauses=10_000,
            preprocess_learned_max_len=10,
            preprocess_splits=1,
            preprocess_seed=7,
            skip_preprocess_output=True,
            e2e=False,
            preprocess_ratio=0.5,
            preprocess_max_steps=10,
            preprocess_learned_max_lbd=30,
            learned_cap_action="restart",
            preprocess_backbone_timeout=1,
            preprocess_test_big_timeout=1,
            preprocess_out_dir=Path("preprocess_out"),
            bin_test_big=self.cnf,
            verbose=False,
            uip_loop_cap=30,
            shortest_wl=False,
            fixed_con_dly=10,
            fixed_dec_dly=10,
        )
        outcome = object()
        with (
            patch("e2e_comparison.print_preprocess_settings"),
            patch("e2e_comparison.preprocess_split", return_value=outcome) as preprocess_mock,
        ):
            outcomes = e2e_comparison.run_preprocessing_splits(
                args,
                self.cnf,
                None,
                self.cfg,
                2,
            )

        self.assertEqual(outcomes, [outcome])
        self.assertEqual(
            preprocess_mock.call_args.kwargs["max_conflict_clauses"],
            10_000,
        )

    def test_e2e_resume_identity_includes_learned_count_capacity(self) -> None:
        self.assertIn("preprocess_max_conflict_clauses", e2e_comparison.CSV_HEADER)
        path = Path(self.temp_dir.name) / "resume.csv"
        with path.open("w", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=[
                    "cnf_path",
                    "config_name",
                    "preprocess_split_idx",
                    "e2e_requested",
                    "preprocess_max_conflict_clauses",
                    "ppcdcl_watch_backend_requested",
                ],
            )
            writer.writeheader()
            writer.writerows(
                [
                    {
                        "cnf_path": str(self.cnf),
                        "config_name": "fixed",
                        "preprocess_split_idx": 0,
                        "e2e_requested": 1,
                        "preprocess_max_conflict_clauses": "",
                        "ppcdcl_watch_backend_requested": "auto",
                    },
                    {
                        "cnf_path": str(self.cnf),
                        "config_name": "fixed",
                        "preprocess_split_idx": 0,
                        "e2e_requested": 1,
                        "preprocess_max_conflict_clauses": 10_000,
                        "ppcdcl_watch_backend_requested": "auto",
                    },
                    {
                        "cnf_path": str(self.cnf),
                        "config_name": "fixed",
                        "preprocess_split_idx": 0,
                        "e2e_requested": 1,
                        "preprocess_max_conflict_clauses": 10_000,
                        "ppcdcl_watch_backend_requested": "indexed",
                    },
                ]
            )

        identities = e2e_comparison.existing_rows(path)
        self.assertIn((str(self.cnf), "fixed", "0", "1", "0", "auto"), identities)
        self.assertIn((str(self.cnf), "fixed", "0", "1", "10000", "auto"), identities)
        self.assertIn((str(self.cnf), "fixed", "0", "1", "10000", "indexed"), identities)

    def test_e2e_rejects_csv_with_unknown_schema_columns(self) -> None:
        path = Path(self.temp_dir.name) / "unknown-e2e-schema.csv"
        path.write_text("cnf_path,unknown_column\nfixture.cnf,value\n", encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "not a recognized subset"):
            e2e_comparison.ensure_csv(path)

    def test_release_estimator_records_and_uses_paper_count_capacity(self) -> None:
        with patch.object(
            sys,
            "argv",
            [
                "compare.py",
                "--fixed-max-conflict-clauses",
                "10000",
                "--watch-backend",
                "indexed",
            ],
        ):
            args = compare_script.parse_args()
        self.assertEqual(args.fixed_max_conflict_clauses, 10_000)
        self.assertEqual(args.watch_backend, "indexed")
        self.assertIn("preprocess_max_conflict_clauses", compare_script.CSV_HEADER)
        self.assertIn("ppcdcl_max_num_of_con_clause", compare_script.CSV_HEADER)
        self.assertIn("ppcdcl_watch_backend_requested", compare_script.CSV_HEADER)
        self.assertIn("ppcdcl_watch_backend_effective", compare_script.CSV_HEADER)

        with patch.object(compare_script, "FIXED_MAX_CONFLICT_CLAUSES", 10_000):
            params = compare_script.bench_params_from_preprocess(
                make_outcome(2),
                best_params=False,
                baseline_params=False,
                wl_choice="combined",
                max_lit_in_conflict_clause=2,
                con_dly=10,
                dec_dly=10,
            )
        self.assertEqual(params["max_num_of_con_clause"], 10_000)
        self.assertEqual(params["_source_max_num_of_con_clause"], "fixed:public-capacity")

        old_csv = Path(self.temp_dir.name) / "old-estimator.csv"
        old_csv.write_text("cnf_path,old_field\n", encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "header does not match"):
            compare_script.ensure_csv(old_csv)

    def test_release_estimator_forwards_requested_watch_backend(self) -> None:
        with (
            patch.object(compare_script, "WATCH_BACKEND", "indexed"),
            patch.object(
                compare_script,
                "run_good_bench",
                return_value={
                    "total_time": 1.0,
                    "watch_backend_requested": "indexed",
                    "watch_backend_effective": "indexed",
                },
            ) as bench_mock,
        ):
            result = compare_script.run_ppcdcl_bench(self.build_params(2))

        self.assertEqual(bench_mock.call_args.kwargs["watch_backend"], "indexed")
        self.assertEqual(result["watch_backend_effective"], "indexed")

    def test_release_estimator_resume_requires_every_split_exactly_once(self) -> None:
        path = Path(self.temp_dir.name) / "resume-estimator.csv"
        compare_script.ensure_csv(path)
        base_row = {
            "cnf_path": str(self.cnf),
            "preprocess_learned_cap": 10,
            "preprocess_max_conflict_clauses": 10_000,
            "ppcdcl_max_lit_in_con_clause": 10,
            "ppcdcl_total_time": 1.0,
            "ppcdcl_watch_backend_requested": "auto",
        }
        compare_script.write_row(path, {**base_row, "preprocess_split_idx": 0})
        self.assertEqual(compare_script.existing_conlit_configs(path, 2), {})

        compare_script.write_row(path, {**base_row, "preprocess_split_idx": 1})
        self.assertEqual(
            compare_script.existing_conlit_configs(path, 2),
            {str(self.cnf): {(10, 10_000)}},
        )
        self.assertEqual(
            compare_script.existing_conlit_configs(path, 2, "indexed"),
            {},
        )

    def test_release_estimator_resume_identity_separates_watch_backends(self) -> None:
        path = Path(self.temp_dir.name) / "backend-resume-estimator.csv"
        compare_script.ensure_csv(path)
        base_row = {
            "cnf_path": str(self.cnf),
            "preprocess_learned_cap": 10,
            "preprocess_max_conflict_clauses": 10_000,
            "ppcdcl_max_lit_in_con_clause": 10,
            "ppcdcl_total_time": 1.0,
        }
        for backend in ("auto", "indexed"):
            for split_idx in (0, 1):
                compare_script.write_row(
                    path,
                    {
                        **base_row,
                        "preprocess_split_idx": split_idx,
                        "ppcdcl_watch_backend_requested": backend,
                    },
                )

        expected = {str(self.cnf): {(10, 10_000)}}
        self.assertEqual(
            compare_script.existing_conlit_configs(path, 2, "auto"), expected
        )
        self.assertEqual(
            compare_script.existing_conlit_configs(path, 2, "indexed"), expected
        )

    def test_release_estimator_prunes_only_incomplete_backend_group(self) -> None:
        path = Path(self.temp_dir.name) / "backend-partial-estimator.csv"
        compare_script.ensure_csv(path)
        base_row = {
            "cnf_path": str(self.cnf),
            "preprocess_learned_cap": 10,
            "preprocess_max_conflict_clauses": 10_000,
            "ppcdcl_max_lit_in_con_clause": 10,
            "ppcdcl_total_time": 1.0,
        }
        for split_idx in (0, 1):
            compare_script.write_row(
                path,
                {
                    **base_row,
                    "preprocess_split_idx": split_idx,
                    "ppcdcl_watch_backend_requested": "auto",
                },
            )
        compare_script.write_row(
            path,
            {
                **base_row,
                "preprocess_split_idx": 0,
                "ppcdcl_watch_backend_requested": "indexed",
            },
        )

        self.assertEqual(compare_script.prune_incomplete_conlit_configs(path, 2), 1)
        with path.open("r", newline="") as handle:
            rows = list(csv.DictReader(handle))
        self.assertEqual(len(rows), 2)
        self.assertEqual(
            {row["ppcdcl_watch_backend_requested"] for row in rows}, {"auto"}
        )

    def test_release_estimator_prunes_partial_split_group_before_resume(self) -> None:
        path = Path(self.temp_dir.name) / "partial-estimator.csv"
        compare_script.ensure_csv(path)
        compare_script.write_row(
            path,
            {
                "cnf_path": str(self.cnf),
                "preprocess_split_idx": 0,
                "preprocess_learned_cap": 10,
                "preprocess_max_conflict_clauses": 10_000,
                "ppcdcl_max_lit_in_con_clause": 10,
                "ppcdcl_total_time": 1.0,
            },
        )

        self.assertEqual(compare_script.prune_incomplete_conlit_configs(path, 2), 1)
        with path.open("r", newline="") as handle:
            self.assertEqual(list(csv.DictReader(handle)), [])

    def test_release_estimator_prunes_duplicate_split_group_before_resume(self) -> None:
        path = Path(self.temp_dir.name) / "duplicate-estimator.csv"
        compare_script.ensure_csv(path)
        base_row = {
            "cnf_path": str(self.cnf),
            "preprocess_learned_cap": 10,
            "preprocess_max_conflict_clauses": 10_000,
            "ppcdcl_max_lit_in_con_clause": 10,
            "ppcdcl_total_time": 1.0,
        }
        compare_script.write_row(path, {**base_row, "preprocess_split_idx": 0})
        compare_script.write_row(path, {**base_row, "preprocess_split_idx": 0})
        compare_script.write_row(path, {**base_row, "preprocess_split_idx": 1})

        self.assertEqual(compare_script.existing_conlit_configs(path, 2), {})
        self.assertEqual(compare_script.prune_incomplete_conlit_configs(path, 2), 3)
        with path.open("r", newline="") as handle:
            self.assertEqual(list(csv.DictReader(handle)), [])

    def test_combined_watch_choice_skips_unused_party_solves(self) -> None:
        self.assertFalse(wl_choice_needs_solve_diagnostics("combined"))
        self.assertFalse(wl_choice_needs_solve_diagnostics("occ"))
        self.assertTrue(wl_choice_needs_solve_diagnostics("solve"))
        self.assertTrue(wl_choice_needs_solve_diagnostics("max-all"))

    def test_internal_sentinel_off_by_one_is_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, r"phi\[0\] sentinel"):
            self.build_params(3)

    def test_projection_error_uses_slower_solver_party(self) -> None:
        self.assertAlmostEqual(projection_error_percent(9.0, 8.0, 10.0), 10.0)

    def test_zero_event_counts_are_preserved(self) -> None:
        self.assertEqual(_sanitize_count(0), 0)

    def test_zero_step_projection_is_rejected_instead_of_fabricating_a_step(self) -> None:
        with self.assertRaisesRegex(ValueError, r"zero-step solve"):
            run_good_bench({"orange_total": 0}, bench_bin=self.cnf, backend_threads=2)


if __name__ == "__main__":
    unittest.main()
