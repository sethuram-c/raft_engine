#!/bin/sh
set -eu
binary=${1:-./build/raft-engine}
[ "$($binary --version)" = 'raft-engine 0.1.0' ]
printf 'PASS: version output\n'
[ "$($binary)" = 'Raft Engine initialized successfully.' ]
printf 'PASS: startup output\n'
output=$($binary --demo)
printf '%s\n' "$output" | grep -qF 'Demo only: no elections'
printf 'PASS: Phase 2 Raft data demo\n'
error_file=$(mktemp)
trap 'rm -f "$error_file"' EXIT HUP INT TERM
if "$binary" --unknown 2>"$error_file"; then
    echo 'FAIL: invalid option should fail' >&2
    exit 1
fi
grep -qF 'Usage: raft-engine' "$error_file"
printf 'PASS: invalid argument handling\n'
printf 'Smoke tests passed.\n'
