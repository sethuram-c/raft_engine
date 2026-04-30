#!/usr/bin/env python3
"""Phase 31: deterministic planning, plan validation and safety-check tests."""
import importlib.util
import json
from pathlib import Path
import unittest

P = Path(__file__).resolve().parents[1] / 'tools' / 'raft_chaos.py'
spec = importlib.util.spec_from_file_location('raft_chaos', P)
assert spec and spec.loader
chaos = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chaos)


class ChaosPlanTests(unittest.TestCase):
    def test_reproducible(self):
        for seed in (0, 1, 31, 1729, 2**32-1):
            for count in (1, 2, 4, 8):
                one = chaos.create_plan(seed, count)
                self.assertEqual(one, chaos.create_plan(seed, count))
                self.assertEqual(one, chaos.validate_plan(json.loads(json.dumps(one))))
        self.assertNotEqual(chaos.create_plan(31, 2), chaos.create_plan(32, 2))

    def test_constraints(self):
        for seed, rounds in ((-1, 1), (2**32, 1), (0, 0), (0, 9)):
            with self.assertRaises(ValueError):
                chaos.create_plan(seed, rounds)
        plan = chaos.create_plan(31, 2)
        mutations = [
            dict(plan, version=2), dict(plan, seed=True),
            dict(plan, rounds=[]), dict(plan, rounds=[{'round': 0}]),
        ]
        for bad in mutations:
            with self.assertRaises(ValueError):
                chaos.validate_plan(bad)
        bad = json.loads(json.dumps(plan))
        bad['rounds'][0]['operations'][0]['value'] = 'unsafe injection'
        with self.assertRaises(ValueError):
            chaos.validate_plan(bad)
        bad = json.loads(json.dumps(plan))
        bad['rounds'][0]['operations'][0]['key'] = 'chaos1_0'
        with self.assertRaises(ValueError):
            chaos.validate_plan(bad)

    def test_observations(self):
        s = {1: (1, 4, 2, 1), 2: (2, 4, 0, 1), 3: (3, 4, 0, 1)}
        l = {1: (8, 4, 7), 2: (8, 4, 7), 3: (7, 3, 6)}
        chaos.check_observation(s, l)
        with self.assertRaises(AssertionError):
            chaos.check_observation({**s, 2: (2, 4, 2, 2)}, l)
        with self.assertRaises(AssertionError):
            chaos.check_observation(s, {**l, 1: (8, 4, 9)})
        # During a partition, leaders of DIFFERENT terms are possible.
        chaos.check_observation({1: (1, 4, 2, 1), 2: (2, 5, 2, 2)}, {})

    def test_binary_safe_expected_values(self):
        sample = 'Local key read: peer=2 key=a found=1 last_applied=2 value_hex=616263'
        self.assertTrue(chaos.matching_value(sample, 'a', 'abc'))
        self.assertFalse(chaos.matching_value(sample, 'a', 'abd'))
        self.assertFalse(chaos.matching_value(sample, 'b', 'abc'))
        self.assertTrue(chaos.matching_value('Local key read: peer=2 key=a found=0', 'a', None))


if __name__ == '__main__':
    unittest.main(verbosity=2)
