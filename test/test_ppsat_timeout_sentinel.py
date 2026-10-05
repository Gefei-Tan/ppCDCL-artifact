#!/usr/bin/env python3
from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from compare import (  # noqa: E402
    MicrotestCost,
    _compute_ppsat_only_row,
    ensure_csv,
    get_ppsat_steps,
    ppsat_from_cache,
    ppsat_plus_ppcdcl_total,
    ppsat_row_timed_out,
    write_row,
)


CENSORED_DERIVED_FIELDS = (
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


def fixed_microtest_cost() -> MicrotestCost:
    times = {
        "total time": 2.0,
        "unit search": 0.2,
        "guess": 0.3,
        "mux": 0.4,
        "check": 0.5,
        "backtrack": 0.6,
        "propagation": 0.7,
    }
    return MicrotestCost(
        var_in=200,
        var_scale=1.0,
        ncls=1,
        times_raw=dict(times),
        times_scaled=dict(times),
    )


class PpSatProducerSentinelTest(unittest.TestCase):
    def test_timeout_preserves_only_returned_step_sentinel(self) -> None:
        with patch("compare.solve", return_value=-1):
            result = get_ppsat_steps(
                Path("/tmp/n4/formula.cnf"),
                "wrand",
                x_budget=100.0,
                per_step_seconds=2.0,
                parsed_formula=(2, 1, object()),
            )

        self.assertEqual(result.steps_returned, -1)
        self.assertIsNone(result.steps_used)
        self.assertIsNone(result.scaled_steps)
        self.assertIsNone(result.x_value)

    def test_unexpected_negative_step_count_is_rejected(self) -> None:
        with patch("compare.solve", return_value=-2):
            with self.assertRaisesRegex(RuntimeError, "invalid negative step count"):
                get_ppsat_steps(
                    Path("/tmp/n4/formula.cnf"),
                    "wrand",
                    x_budget=100.0,
                    per_step_seconds=2.0,
                    parsed_formula=(2, 1, object()),
                )

    def test_timeout_row_serializes_derived_values_as_empty_cells(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            cnf = tmp_path / "formula.cnf"
            cnf.write_text("p cnf 1 1\n1 0\n", encoding="utf-8")
            microtest = tmp_path / "microtest"
            microtest.touch()

            with (
                patch("compare.solve", return_value=-1),
                patch(
                    "compare.get_ppsat_step_cost_microtest_with_config",
                    return_value=fixed_microtest_cost(),
                ),
            ):
                row, total_est, summary = _compute_ppsat_only_row(
                    cnf_path=str(cnf),
                    family="fixture",
                    heuristic="wrand",
                    x_budget=100.0,
                    microtest_bin=str(microtest),
                    microtest_fixed_args=("1", "3", "1"),
                    microtest_var_scale=1.0,
                    microtest_min_var=200,
                    microtest_timeout_sec=1,
                )

            self.assertIsNone(total_est)
            self.assertIn("est_time=censored", summary)
            self.assertEqual(row["ppsat_steps_returned"], -1)
            for field in CENSORED_DERIVED_FIELDS:
                self.assertIsNone(row[field], field)

            output = tmp_path / "result.csv"
            ensure_csv(output)
            write_row(output, row)
            with output.open(newline="") as handle:
                saved = next(csv.DictReader(handle))

            self.assertEqual(saved["ppsat_steps_returned"], "-1")
            self.assertEqual(saved["micro_total_time_scaled"], "2.0")
            for field in CENSORED_DERIVED_FIELDS:
                self.assertEqual(saved[field], "", field)

    def test_completed_row_keeps_positive_estimates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            cnf = tmp_path / "formula.cnf"
            cnf.write_text("p cnf 1 1\n1 0\n", encoding="utf-8")
            microtest = tmp_path / "microtest"
            microtest.touch()

            with (
                patch("compare.solve", return_value=4),
                patch(
                    "compare.get_ppsat_step_cost_microtest_with_config",
                    return_value=fixed_microtest_cost(),
                ),
            ):
                row, total_est, summary = _compute_ppsat_only_row(
                    cnf_path=str(cnf),
                    family="fixture",
                    heuristic="wrand",
                    x_budget=100.0,
                    microtest_bin=str(microtest),
                    microtest_fixed_args=("1", "3", "1"),
                    microtest_var_scale=1.0,
                    microtest_min_var=200,
                    microtest_timeout_sec=1,
                )

        self.assertEqual(total_est, 8.0)
        self.assertIn("est_time=8.00s", summary)
        self.assertEqual(row["ppsat_steps_returned"], 4)
        self.assertEqual(row["ppsat_steps_used"], 4)
        self.assertEqual(row["ppsat_scaled_steps"], 8.0)
        self.assertEqual(row["ppsat_x_value"], 20.0)
        expected = {
            "ppsat_total_time_est": 8.0,
            "ppsat_unit_search_est": 0.8,
            "ppsat_guess_est": 1.2,
            "ppsat_mux_est": 1.6,
            "ppsat_check_est": 2.0,
            "ppsat_backtrack_est": 2.4,
            "ppsat_propagation_est": 2.8,
        }
        for field, value in expected.items():
            self.assertAlmostEqual(row[field], value, msg=field)


class PpSatCacheSentinelTest(unittest.TestCase):
    def test_timeout_identity_is_exact_numeric_minus_one(self) -> None:
        for sentinel in (-1, -1.0, "-1", "-1.0", "-1e0"):
            with self.subTest(sentinel=sentinel):
                self.assertTrue(ppsat_row_timed_out({"ppsat_steps_returned": sentinel}))

        for value in (-2, "-2", 0, "", None, "not-a-number"):
            with self.subTest(value=value):
                self.assertFalse(ppsat_row_timed_out({"ppsat_steps_returned": value}))

    def test_legacy_timeout_cache_row_is_normalized_without_mutation(self) -> None:
        cached = {
            "cnf_path": "/tmp/formula.cnf",
            "ppsat_steps_returned": "-1.0",
            "ppsat_steps_used": "-1",
            "ppsat_scaled_steps": "-1.5",
            "ppsat_x_value": "-3.75",
            "ppsat_total_time_est": "-2.0",
            "ppsat_unit_search_est": "-0.2",
            "ppsat_guess_est": "-0.3",
            "ppsat_mux_est": "-0.4",
            "ppsat_check_est": "-0.5",
            "ppsat_backtrack_est": "-0.6",
            "ppsat_propagation_est": "-0.7",
            "micro_total_time_scaled": "2.0",
        }
        original = dict(cached)

        normalized, total_est = ppsat_from_cache(cached)

        self.assertEqual(cached, original)
        self.assertIsNone(total_est)
        self.assertEqual(normalized["ppsat_steps_returned"], "-1.0")
        self.assertEqual(normalized["micro_total_time_scaled"], "2.0")
        for field in CENSORED_DERIVED_FIELDS:
            self.assertEqual(normalized[field], "", field)

    def test_completed_cache_row_remains_numeric(self) -> None:
        cached = {
            "cnf_path": "/tmp/formula.cnf",
            "ppsat_steps_returned": "4",
            "ppsat_steps_used": "4",
            "ppsat_scaled_steps": "8.0",
            "ppsat_x_value": "20.0",
            "ppsat_total_time_est": "8.0",
            "ppsat_unit_search_est": "0.8",
            "ppsat_guess_est": "1.2",
            "ppsat_mux_est": "1.6",
            "ppsat_check_est": "2.0",
            "ppsat_backtrack_est": "2.4",
            "ppsat_propagation_est": "2.8",
        }
        original = dict(cached)

        normalized, total_est = ppsat_from_cache(cached)

        self.assertEqual(cached, original)
        self.assertEqual(total_est, 8.0)
        for field, value in cached.items():
            self.assertEqual(normalized[field], value, field)


class CombinedTotalTest(unittest.TestCase):
    def test_combined_total_requires_two_observed_runtimes(self) -> None:
        self.assertEqual(
            ppsat_plus_ppcdcl_total(None, 5.0, ppcdcl_timed_out=False),
            "",
        )
        self.assertEqual(
            ppsat_plus_ppcdcl_total(3.0, None, ppcdcl_timed_out=False),
            "",
        )
        self.assertEqual(
            ppsat_plus_ppcdcl_total(3.0, 5.0, ppcdcl_timed_out=True),
            "",
        )

    def test_combined_total_adds_two_completed_runtimes(self) -> None:
        self.assertEqual(
            ppsat_plus_ppcdcl_total(3.0, 5.0, ppcdcl_timed_out=False),
            8.0,
        )


if __name__ == "__main__":
    unittest.main()
