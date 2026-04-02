#!/bin/sh
set -eu

binary=${1:-./build/raft-engine}

expected_version='raft-engine 0.2.0'
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

demo_output=$("$binary" --demo)
printf '%s\n' "$demo_output" | grep -qF 'Node: id=1 role=follower term=2'
printf '%s\n' "$demo_output" | grep -qF 'Log: entries=2 last_index=2'
printf '%s\n' "$demo_output" | grep -qF 'Progress: committed=1 applied=1'
printf 'PASS: in-memory node demonstration\n'

error_file=$(mktemp)
trap 'rm -f "$error_file"' EXIT HUP INT TERM
if "$binary" --unknown 2>"$error_file"; then
    printf 'FAIL: invalid argument should return an error\n' >&2
    exit 1
fi
if ! grep -qF 'Usage: raft-engine [--version|--demo]' "$error_file"; then
    printf 'FAIL: invalid argument did not print usage\n' >&2
    exit 1
fi
printf 'PASS: invalid argument handling\n'
printf 'All smoke tests passed.\n'
