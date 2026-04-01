#!/bin/sh
set -eu

binary=${1:-./build/raft-engine}

expected_version='raft-engine 0.1.0'
actual_version=$("$binary" --version)
if [ "$actual_version" != "$expected_version" ]; then
    printf 'FAIL: version output: %s\n' "$actual_version" >&2
    exit 1
fi
printf 'PASS: version output\n'

expected_startup='Raft Engine initialized successfully.'
actual_startup=$("$binary")
if [ "$actual_startup" != "$expected_startup" ]; then
    printf 'FAIL: startup output: %s\n' "$actual_startup" >&2
    exit 1
fi
printf 'PASS: startup output\n'

error_file=$(mktemp)
trap 'rm -f "$error_file"' EXIT HUP INT TERM
if "$binary" --unknown 2>"$error_file"; then
    printf 'FAIL: invalid argument should return an error\n' >&2
    exit 1
fi
if ! grep -qF 'Usage: raft-engine [--version]' "$error_file"; then
    printf 'FAIL: invalid argument did not print usage\n' >&2
    exit 1
fi
printf 'PASS: invalid argument handling\n'
printf 'All smoke tests passed.\n'
