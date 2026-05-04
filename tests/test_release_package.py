#!/usr/bin/env python3
"""Phase 34: release reproducibility, archive allowlisting, and integrity tests."""
from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import raft_release as release  # noqa: E402


class ReleasePackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="raft34-release-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "src"
        self.root.mkdir()
        for name in ["Makefile", "README.md", "progress.md", ".gitignore",
                     "src/main.c", "tools/raft_release.py", "tests/test_example.py",
                     "config/node1.conf", "docs/guide.md", "include/example.h"]:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"test content for {name}\n", encoding="utf8")
        # These are excluded regardless of size and mtime.
        for name in ["build/server", "dist/stale.zip", "data/node1.wal",
                     "config/node1.key", ".git/config", "tests/__pycache__/x.pyc",
                     "docs/handbook.pdf"]:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"DO NOT SHIP PRIVATE OR GENERATED DATA")
        self.first = Path(self.temp.name) / "one.zip"
        self.second = Path(self.temp.name) / "two.zip"

    def make(self, output):
        with contextlib.redirect_stdout(io.StringIO()):
            release.create(self.root, output, release.DEFAULT_EPOCH)

    def test_reproducible_archive_and_verified_manifest(self):
        self.make(self.first)
        for file in self.root.rglob("*"):
            if file.is_file():
                os.utime(file, (100000, 100000))
        self.make(self.second)
        self.assertEqual(self.first.read_bytes(), self.second.read_bytes())
        with contextlib.redirect_stdout(io.StringIO()):
            release.verify(self.first)
        with zipfile.ZipFile(self.first) as zipped:
            entries = zipped.namelist()
            self.assertEqual(entries[-1], release.MANIFEST)
            self.assertFalse(any("DO NOT SHIP" in zipped.read(n).decode("utf8") for n in entries))
            self.assertFalse(any(".key" in n or "build/" in n or "dist/" in n or "data/" in n for n in entries))

    def test_changed_content_breaks_checksum(self):
        self.make(self.first)
        with zipfile.ZipFile(self.first) as zipped:
            entries = [(info.filename, zipped.read(info)) for info in zipped.infolist()]
        with zipfile.ZipFile(self.second, "w") as zipped:
            for name, data in entries:
                if name.endswith("src/main.c"):
                    data += b"unexpected edit\n"
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                zipped.writestr(info, data)
        with self.assertRaisesRegex(release.ReleaseError, "SHA256SUMS mismatch"):
            release.verify(self.second)

    def test_symlink_source_is_rejected(self):
        (self.root / "src" / "proxy.c").symlink_to(self.root / "src" / "main.c")
        with self.assertRaisesRegex(release.ReleaseError, "not a regular source file"):
            self.make(self.first)

    def test_unsafe_archive_name_is_rejected(self):
        for filename in ("raft-engine/../escape.txt",
                         "raft-engine//src/main.c",
                         "raft-engine/src/./main.c"):
            info = zipfile.ZipInfo(filename, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            with zipfile.ZipFile(self.first, "w") as zipped:
                zipped.writestr(info, b"bad")
            with self.assertRaisesRegex(release.ReleaseError, "unsafe archive entry"):
                release.verify(self.first)

    def test_archive_symlink_is_rejected(self):
        info = zipfile.ZipInfo("raft-engine/src/main.c", (1980, 1, 1, 0, 0, 0))
        info.create_system = 3
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(self.first, "w") as zipped:
            zipped.writestr(info, b"/etc/passwd")
        with self.assertRaisesRegex(release.ReleaseError, "not a regular file"):
            release.verify(self.first)

    def test_missing_manifest_rejected(self):
        with zipfile.ZipFile(self.first, "w") as zipped:
            zipped.writestr("raft-engine/src/main.c", "int main(void) {return 0;}\n")
        with self.assertRaises(release.ReleaseError):
            release.verify(self.first)

    def test_timestamp_bounds_and_stable_rounding(self):
        self.assertEqual(release.zip_datetime(315532801), release.zip_datetime(315532800))
        with self.assertRaises(release.ReleaseError):
            release.zip_datetime(0)
        with self.assertRaises(release.ReleaseError):
            release.zip_datetime(999999999999999)


if __name__ == "__main__":
    unittest.main(verbosity=2)
