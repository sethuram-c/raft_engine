#!/usr/bin/env python3
"""Non-mutating operational release smoke tests for an optimized executable."""
from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

APP = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path("build/raft-engine-release").resolve()
sys.argv = [sys.argv[0]]
ROOT = Path(__file__).resolve().parents[1]


class ReleaseSmoke(unittest.TestCase):
    def run_app(self, *args):
        return subprocess.run([str(APP), *map(str, args)], cwd=ROOT,
                              text=True, capture_output=True, timeout=10, check=False)

    def test_release_binary_is_executable_and_versioned(self):
        self.assertTrue(APP.is_file())
        response = self.run_app("--version")
        self.assertEqual(response.returncode, 0, response.stderr)
        self.assertEqual(response.stdout.strip(), "raft-engine 0.1.0")

    def test_valid_three_node_configs(self):
        for i in (1, 2, 3):
            response = self.run_app("--check-config", ROOT / "config" / f"node{i}.conf")
            self.assertEqual(response.returncode, 0, response.stderr)
            self.assertIn(f"Config valid: node_id={i} peers=3", response.stdout)

    def test_invalid_config_fails_closed_without_server(self):
        with tempfile.TemporaryDirectory() as tmp:
            invalid = Path(tmp) / "invalid.conf"
            invalid.write_text("node_id=7\npeer=1,127.0.0.1,7001\n"
                               "peer=2,127.0.0.1,7002\npeer=3,127.0.0.1,7003\n")
            response = self.run_app("--check-config", invalid)
            self.assertNotEqual(response.returncode, 0)
            self.assertIn("Configuration", response.stderr)

    def test_no_secret_or_network_required_for_smoke(self):
        self.assertEqual(self.run_app().returncode, 0)
        self.assertEqual(self.run_app("--demo").returncode, 0)
        bad = self.run_app("--unknown")
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("Usage: raft-engine", bad.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
