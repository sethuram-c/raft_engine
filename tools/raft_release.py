#!/usr/bin/env python3
"""Create and verify a reproducible, source-only Raft Engine ZIP archive.

This is an integrity and packaging tool, not a signature or supply-chain
attestation. A SHA256SUMS manifest detects accidental modification after
creation, but does not establish publisher authenticity.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import zipfile

ROOT_DIR = "raft-engine"
MANIFEST = f"{ROOT_DIR}/SHA256SUMS"
ROOT_FILES = {"Makefile", "README.md", "progress.md", ".gitignore"}
ALLOWED = {
    "config": {".conf"},
    "docs": {".md"},
    "include": {".h"},
    "src": {".c"},
    "tests": {".c", ".py", ".sh"},
    "tools": {".c", ".py", ".sh"},
}
DEFAULT_EPOCH = 315532800  # 1980-01-01 UTC, earliest ZIP timestamp
MAX_INPUT_SIZE = 8 * 1024 * 1024  # project source file sanity cap
MAX_ARCHIVE_SIZE = 64 * 1024 * 1024  # total uncompressed source cap


class ReleaseError(ValueError):
    pass


def eligible(path: PurePosixPath) -> bool:
    parts = path.parts
    if len(parts) == 1:
        return parts[0] in ROOT_FILES
    if len(parts) < 2 or parts[0] not in ALLOWED:
        return False
    if any(part.startswith(".") for part in parts):
        return False
    if parts[0] == "config" and len(parts) != 2:
        return False
    return path.suffix in ALLOWED[parts[0]]


def inputs(root: Path) -> list[tuple[str, bytes]]:
    if not root.is_dir():
        raise ReleaseError(f"source directory not found: {root}")
    entries: list[tuple[str, bytes]] = []
    for item in root.rglob("*"):
        rel = PurePosixPath(item.relative_to(root).as_posix())
        if not eligible(rel):
            continue
        # lstat: disallow symlinks even if they point to legitimate files.
        mode = item.lstat().st_mode
        if not stat.S_ISREG(mode):
            raise ReleaseError(f"not a regular source file: {rel}")
        if item.stat().st_size > MAX_INPUT_SIZE:
            raise ReleaseError(f"source file too large: {rel}")
        entries.append((rel.as_posix(), item.read_bytes()))
    entries.sort(key=lambda row: row[0])
    names = {name for name, _ in entries}
    for name in ROOT_FILES:
        if name not in names:
            raise ReleaseError(f"required file missing: {name}")
    if "src/main.c" not in names or "tools/raft_release.py" not in names:
        raise ReleaseError("required application/release source missing")
    if sum(len(data) for _, data in entries) > MAX_ARCHIVE_SIZE:
        raise ReleaseError("total source size exceeds archive limit")
    return entries


def manifest_bytes(entries: list[tuple[str, bytes]]) -> bytes:
    return "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in entries
    ).encode("ascii")


def zip_datetime(epoch: int) -> tuple[int, int, int, int, int, int]:
    if epoch < DEFAULT_EPOCH:
        raise ReleaseError("SOURCE_DATE_EPOCH precedes the ZIP 1980 minimum")
    try:
        value = dt.datetime.fromtimestamp(epoch, dt.timezone.utc)
    except (OverflowError, OSError, ValueError) as exc:
        raise ReleaseError("SOURCE_DATE_EPOCH is invalid") from exc
    if value.year > 2107:
        raise ReleaseError("SOURCE_DATE_EPOCH exceeds ZIP maximum year 2107")
    # ZIP timestamps have two-second resolution.
    return (value.year, value.month, value.day, value.hour, value.minute,
            value.second - (value.second % 2))


def create(root: Path, output: Path, epoch: int) -> None:
    entries = inputs(root)
    entries.append(("SHA256SUMS", manifest_bytes(entries)))
    timestamp = zip_datetime(epoch)
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = output.with_name(output.name + ".tmp")
    try:
        with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=9, allowZip64=False) as archive:
            for name, data in entries:
                info = zipfile.ZipInfo(f"{ROOT_DIR}/{name}", timestamp)
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED,
                                 compresslevel=9)
        tmp.replace(output)
    finally:
        tmp.unlink(missing_ok=True)
    print(f"Created {output} ({len(entries) - 1} source files + SHA256SUMS)")
    print(f"Archive SHA256: {hashlib.sha256(output.read_bytes()).hexdigest()}")


def verify(filename: Path) -> None:
    if not filename.is_file():
        raise ReleaseError(f"archive not found: {filename}")
    with zipfile.ZipFile(filename) as archive:
        members = archive.infolist()
        names: set[str] = set()
        content: dict[str, bytes] = {}
        total = 0
        for info in members:
            name = info.filename
            if name in names:
                raise ReleaseError(f"duplicate archive member: {name}")
            names.add(name)
            parts = PurePosixPath(name).parts
            if (len(parts) < 2 or parts[0] != ROOT_DIR or
                name != PurePosixPath(name).as_posix() or
                any(part in (".", "..") for part in parts) or
                "\\" in name or name.startswith("/") or name.endswith("/")):
                raise ReleaseError(f"unsafe archive entry: {name}")
            mode = info.external_attr >> 16
            if stat.S_IFMT(mode) != stat.S_IFREG:
                raise ReleaseError(f"archive entry is not a regular file: {name}")
            rel = PurePosixPath(*parts[1:])
            if name != MANIFEST and not eligible(rel):
                raise ReleaseError(f"not an allowlisted source file: {name}")
            if info.file_size > MAX_INPUT_SIZE:
                raise ReleaseError(f"archive entry too large: {name}")
            total += info.file_size
            if total > MAX_ARCHIVE_SIZE:
                raise ReleaseError("archive uncompressed size exceeds limit")
            content[rel.as_posix()] = archive.read(info)
        if MANIFEST not in names:
            raise ReleaseError("SHA256SUMS manifest missing")
        actual_files = sorted(name for name in content if name != "SHA256SUMS")
        required_files = {"Makefile", "README.md", "progress.md", ".gitignore",
                          "src/main.c", "tools/raft_release.py"}
        if not required_files.issubset(actual_files):
            raise ReleaseError("required release source files missing")
        expected_manifest = manifest_bytes([(name, content[name]) for name in actual_files])
        if content["SHA256SUMS"] != expected_manifest:
            raise ReleaseError("SHA256SUMS mismatch: archive content modified or missing")
    print(f"PASS: verified {len(actual_files)} source files; SHA256SUMS and paths valid")
    print(f"Archive SHA256: {hashlib.sha256(filename.read_bytes()).hexdigest()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    make = sub.add_parser("create", help="create deterministic source-only ZIP")
    make.add_argument("--root", type=Path, default=Path("."))
    make.add_argument("--output", type=Path, required=True)
    make.add_argument("--epoch", type=int, default=None,
                      help="UTC epoch; default SOURCE_DATE_EPOCH or 1980-01-01")
    check = sub.add_parser("verify", help="check source paths and checksums")
    check.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.action == "create":
            epoch_string = (args.epoch if args.epoch is not None else
                            os.environ.get("SOURCE_DATE_EPOCH", DEFAULT_EPOCH))
            create(args.root, args.output, int(epoch_string))
        else:
            verify(args.archive)
    except (ReleaseError, OSError, ValueError, zipfile.BadZipFile, EOFError) as exc:
        print(f"Release error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
