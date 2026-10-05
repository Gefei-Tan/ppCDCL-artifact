#!/usr/bin/env python3
from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import bench_utils  # noqa: E402
import microbench  # noqa: E402


class MicrobenchCliTest(unittest.TestCase):
    def test_shared_runner_forwards_and_returns_watch_backend_metadata(self) -> None:
        summary = (
            "BENCH_WATCH_BACKEND requested=block-local effective=block-local "
            "reason=explicit block-local request occ_bits=8 pair_bits=4\n"
            "SUMMARY 10 1 2 3 4 1 1 2 1 1 1 1\n"
        )
        commands = []

        class FakePopen:
            def __init__(self, command, **_kwargs):
                commands.append(command)
                self.returncode = 0

            def communicate(self, timeout=None):
                del timeout
                return summary, ""

            def poll(self):
                return self.returncode

            def kill(self):
                self.returncode = -9

        params = {
            "var_num": 8,
            "clause_num": 12,
            "max_num_of_con_clause": 8,
            "max_lit_in_clause": 4,
            "max_lit_in_con_clause": 4,
            "max_clause_in_wl": 8,
            "orange_total": 1,
            "yellow_total": 1,
            "blue_total": 1,
            "red_total": 1,
            "uip_cap": 4,
        }
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test_good_bench"
            binary.touch()
            with mock.patch.object(bench_utils.subprocess, "Popen", FakePopen):
                result = bench_utils.run_good_bench(
                    params,
                    bench_bin=binary,
                    watch_backend="block-local",
                    backend_threads=2,
                )

        self.assertEqual(len(commands), 2)
        for command in commands:
            backend_index = command.index("--watch-backend")
            self.assertEqual(command[backend_index + 1], "block")
        self.assertEqual(result["watch_backend_requested"], "block")
        self.assertEqual(result["watch_backend_effective"], "block")
        self.assertEqual(result["watch_backend_reason"], "explicit block-local request")
        self.assertEqual(result["watch_backend_occ_bits"], 8)
        self.assertEqual(result["watch_backend_pair_bits"], 4)

    def test_summary_parser_records_setup_and_conflict_gates(self) -> None:
        parsed = microbench._parse_good_bench_summary(
            "BENCH_CONFLICT_GATES per_block=1.25e+08\n"
            "BENCH_SETUP seconds=3.5 gates=4096 included_in_summary=0\n"
            "BENCH_WATCH_BACKEND requested=auto effective=indexed "
            "reason=auto: pair payload is smaller than one Data block "
            "occ_bits=34 pair_bits=8\n"
            "SUMMARY 10 1 2 3 4 1 1 2 5 6 7 8\n"
        )

        self.assertEqual(parsed["benchmark_setup_time"], 3.5)
        self.assertEqual(parsed["benchmark_setup_gates"], 4096.0)
        self.assertEqual(parsed["conflict_gates"], 1.25e8)
        self.assertEqual(parsed["total_time"], 10.0)
        self.assertEqual(parsed["watch_backend_requested"], "auto")
        self.assertEqual(parsed["watch_backend_effective"], "indexed")
        self.assertEqual(
            parsed["watch_backend_reason"],
            "auto: pair payload is smaller than one Data block",
        )
        self.assertEqual(parsed["watch_backend_occ_bits"], 34)
        self.assertEqual(parsed["watch_backend_pair_bits"], 8)

    def test_build_commands_describes_oram_shortest_configuration(self) -> None:
        args = microbench.parse_args(
            [
                "--binary", "/tmp/test_good_bench",
                "--oramV",
                "--threads", "8",
                "--uip-cap", "37",
                "--shortest-wl",
                "--base-port", "18086",
                "--global-sample-factor", "4",
                "--conflict-sample-factor", "9",
                "--watch-backend", "indexed",
            ]
        )

        alice, bob = microbench.build_good_bench_commands(5000, 10000, args)

        self.assertEqual(alice[:3], ["/tmp/test_good_bench", "1", "18086"])
        self.assertEqual(bob[:3], ["/tmp/test_good_bench", "2", "18086"])
        self.assertEqual(alice[3:5], ["5000", "10000"])
        self.assertEqual(alice[16], "37")
        self.assertEqual(
            alice[-6:],
            ["--oramV", "--shortest-wl", "--watch-backend", "indexed", "--threads", "8"],
        )
        self.assertEqual(
            bob[-6:],
            ["--oramV", "--shortest-wl", "--watch-backend", "indexed", "--threads", "8"],
        )

    def test_build_commands_explicitly_selects_linear_first_fit_defaults(self) -> None:
        args = microbench.parse_args(["--binary", "/tmp/test_good_bench"])

        alice, bob = microbench.build_good_bench_commands(100, 200, args)

        self.assertEqual(alice[2], str(microbench.DEFAULT_BASE_PORT))
        self.assertEqual(
            alice[-6:],
            ["--no-oramV", "--no-shortest-wl", "--watch-backend", "auto", "--threads", "4"],
        )
        self.assertEqual(
            bob[-6:],
            ["--no-oramV", "--no-shortest-wl", "--watch-backend", "auto", "--threads", "4"],
        )

    def test_csv_row_records_execution_configuration_without_mpc(self) -> None:
        args = microbench.parse_args(
            [
                "--oramV",
                "--threads", "6",
                "--uip-loop-cap", "31",
                "--no-shortest-wl",
                "--base-port", "19000",
                "--conflict-factor", "3",
                "--global-sample-factor", "7",
                "--conflict-sample-factor", "11",
                "--watch-backend", "block",
            ]
        )
        row = microbench.build_config_row(5000, 10000, args)

        self.assertEqual(row["variable_state_backend"], "oram")
        self.assertEqual(row["backend_threads"], 6)
        self.assertEqual(row["uip_loop_cap"], 31)
        self.assertEqual(row["watchlist_policy"], "first-fit")
        self.assertEqual(row["watch_backend_requested"], "block")
        self.assertEqual(row["watch_backend_effective"], "")
        self.assertEqual(row["base_port"], 19000)
        self.assertEqual(row["timeout_seconds"], microbench.DEFAULT_TIMEOUT_SECONDS)
        self.assertEqual(row["conflict_factor"], 3)
        self.assertEqual(row["global_sample_factor"], 7)
        self.assertEqual(row["conflict_sample_factor"], 11)

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "matched.csv"
            microbench.write_csv_header(output)
            microbench.append_row(output, row)
            with output.open(newline="") as handle:
                saved = next(csv.DictReader(handle))

        self.assertEqual(saved["variable_state_backend"], "oram")
        self.assertEqual(saved["backend_threads"], "6")
        self.assertEqual(saved["uip_loop_cap"], "31")
        self.assertEqual(saved["watchlist_policy"], "first-fit")
        self.assertEqual(saved["watch_backend_requested"], "block")
        self.assertEqual(saved["watch_backend_effective"], "")
        self.assertEqual(saved["base_port"], "19000")
        self.assertEqual(saved["timeout_seconds"], "600.0")
        self.assertEqual(saved["global_sample_factor"], "7")
        self.assertEqual(saved["conflict_sample_factor"], "11")

    def test_existing_legacy_csv_is_extended_without_losing_rows(self) -> None:
        legacy_columns = microbench.LEGACY_CSV_COLUMNS
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "legacy.csv"
            with output.open("w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=legacy_columns)
                writer.writeheader()
                writer.writerow({"var_num": 123, "clause_num": 456})

            microbench.write_csv_header(output)

            with output.open(newline="") as handle:
                reader = csv.DictReader(handle)
                saved = next(reader)
                self.assertEqual(reader.fieldnames, microbench.CSV_COLUMNS)

        self.assertEqual(saved["var_num"], "123")
        self.assertEqual(saved["clause_num"], "456")
        self.assertEqual(saved["variable_state_backend"], "")
        self.assertEqual(saved["watch_backend_requested"], "")

    def test_damaged_recognized_schema_is_rejected(self) -> None:
        damaged_columns = list(microbench.CSV_COLUMNS)
        damaged_columns.remove("decision_gates")
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "damaged.csv"
            with output.open("w", newline="") as handle:
                csv.writer(handle).writerow(damaged_columns)

            with self.assertRaisesRegex(ValueError, "unsupported or damaged"):
                microbench.write_csv_header(output)

    def test_run_applies_configured_hard_timeout_to_both_parties(self) -> None:
        args = microbench.parse_args(
            [
                "--binary", "/tmp/test_good_bench",
                "--timeout-seconds", "17.5",
            ]
        )
        summary = (
            "BENCH_CONFLICT_GATES per_block=12\n"
            "BENCH_SETUP seconds=0.1 gates=3 included_in_summary=0\n"
            "BENCH_WATCH_BACKEND requested=auto effective=block-local "
            "reason=auto: indexed occupancy bitmap exceeds 128 public bits "
            "occ_bits=256 pair_bits=8\n"
            "SUMMARY 10 1 2 3 4 1 1 2 100 100 2 100\n"
        )
        observed_timeouts = []

        def fake_run(command, **kwargs):
            observed_timeouts.append(kwargs.get("timeout"))
            return microbench.subprocess.CompletedProcess(command, 0, summary, "")

        with mock.patch.object(microbench.subprocess, "run", side_effect=fake_run):
            row = microbench.run_one_config(20, 91, args)

        self.assertEqual(sorted(observed_timeouts), [17.5, 17.5])
        self.assertEqual(row["timeout_seconds"], 17.5)
        self.assertEqual(row["watch_backend_requested"], "auto")
        self.assertEqual(row["watch_backend_effective"], "block")
        self.assertEqual(
            row["watch_backend_reason"],
            "auto: indexed occupancy bitmap exceeds 128 public bits",
        )
        self.assertEqual(row["watch_backend_occ_bits"], 256)
        self.assertEqual(row["watch_backend_pair_bits"], 8)

    def test_watch_backend_rejects_unknown_choice(self) -> None:
        with self.assertRaises(SystemExit):
            microbench.parse_args(["--watch-backend", "linear"])

    def test_missing_backend_metadata_is_explicit_for_legacy_output(self) -> None:
        parsed = microbench._parse_good_bench_summary(
            "SUMMARY 10 1 2 3 4 1 1 2 5 6 7 8\n",
            requested_watch_backend="block",
        )

        self.assertEqual(parsed["watch_backend_requested"], "block")
        self.assertEqual(parsed["watch_backend_effective"], "block")
        self.assertEqual(parsed["watch_backend_reason"], "not_reported")
        self.assertIsNone(parsed["watch_backend_occ_bits"])

    def test_unmeasured_metrics_remain_blank_when_runs_are_averaged(self) -> None:
        args = microbench.parse_args(["--runs", "1"])
        measured = microbench.build_config_row(20, 91, args)
        measured.update(
            {
                "total_time": 1.25,
                "conflict_gates": 1234.0,
                "decision_gates": None,
            }
        )

        original = microbench.run_one_config
        microbench.run_one_config = lambda *_args, **_kwargs: measured
        try:
            with tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "averaged.csv"
                microbench.write_csv_header(output)
                microbench.average_runs_for_config(20, 91, args, output)
                with output.open(newline="") as handle:
                    saved = next(csv.DictReader(handle))
        finally:
            microbench.run_one_config = original

        self.assertEqual(saved["total_time"], "1.25")
        self.assertEqual(saved["conflict_gates"], "1234.0")
        self.assertEqual(saved["decision_gates"], "")


if __name__ == "__main__":
    unittest.main()
