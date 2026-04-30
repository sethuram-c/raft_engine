#!/usr/bin/env python3
"""Deterministic Phase 32 tests for aggregation, validation and output."""
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('raft_bench', ROOT / 'tools' / 'raft_bench.py')
assert SPEC and SPEC.loader
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class BenchMath(unittest.TestCase):
    def test_percentiles_nearest_rank(self):
        self.assertEqual(BENCH.percentile_nearest_rank([8, 1, 5, 2, 3], 50), 3)
        self.assertEqual(BENCH.percentile_nearest_rank([8, 1, 5, 2, 3], 95), 8)
        self.assertEqual(BENCH.percentile_nearest_rank([8, 1, 5, 2, 3], 99), 8)
        self.assertEqual(BENCH.percentile_nearest_rank([1, 2, 3, 4], 25), 1)
        self.assertEqual(BENCH.percentile_nearest_rank([1, 2, 3, 4], 100), 4)

    def test_invalid_samples_and_ranges(self):
        for sample in ([], [-1, 4], [float('nan')], [float('inf')]):
            with self.assertRaises(ValueError):
                BENCH.percentile_nearest_rank(sample, 50)
        for p in (-1, 0, 101):
            with self.assertRaises(ValueError):
                BENCH.percentile_nearest_rank([1], p)
        for wall, attempts in ((-1, 2), (0, -1), (1, 1)):
            with self.assertRaises(ValueError):
                BENCH.summarize_latency([1, 2], wall, attempts)

    def test_throughput_and_failure_reporting(self):
        result = BENCH.summarize_latency([2, 3, 4], 0.5, 4)
        self.assertEqual((result['attempted'], result['committed'], result['failed']), (4, 3, 1))
        self.assertEqual(result['throughput_committed_ops_s'], 6)
        self.assertEqual(result['latency_ms']['p50'], 3)
        self.assertEqual(result['latency_ms']['p95'], 4)
        self.assertIsNone(BENCH.summarize_latency([], 1, 4)['latency_ms'])
        self.assertEqual(BENCH.summarize_latency([], 0, 0)['throughput_committed_ops_s'], 0)

    def test_proc_stat_field_position(self):
        # (pid (comm containing spaces) state ... utime stime ...)
        fields = ['S'] + ['0'] * 18
        fields[11], fields[12] = '12', '9'
        self.assertEqual(BENCH.parse_proc_stat('42 (worker a b) ' + ' '.join(fields), 100), 210)
        with self.assertRaises(ValueError):
            BENCH.parse_proc_stat('nonsense', 100)

    def test_csv_json_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            rows = [BENCH.make_row(0, 'measured', True, 4.2673, 0),
                    BENCH.make_row(1, 'recovery', False, 60.2, 3)]
            result = {'schema_version': 1, 'steady_state': {'attempted': 1}}
            BENCH.export_results(result, rows, p / 'sub' / 'summary.json', p / 'sub' / 'latency.csv')
            self.assertEqual(json.loads((p / 'sub' / 'summary.json').read_text()), result)
            with (p / 'sub' / 'latency.csv').open() as source:
                observed = list(csv.DictReader(source))
            self.assertEqual(len(observed), 2)
            self.assertEqual(observed[0]['latency_ms'], '4.267')
            self.assertEqual(observed[1]['committed'], '0')


if __name__ == '__main__':
    unittest.main(verbosity=2)
