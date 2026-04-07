#!/bin/sh
set -eu
APP="${1:-./build/raft-engine}"
output="$($APP --timer-demo config/node1.conf)"
printf '%s\n' "$output" | grep -q 'Follower timer: node=1 role=follower'
printf '%s\n' "$output" | grep -q 'At deadline-1: timeout_pending=0'
printf '%s\n' "$output" | grep -q 'At deadline: timeout_pending=1'
printf '%s\n' "$output" | grep -q 'After reset: timeout_pending=0'
printf '%s\n' "$output" | grep -q 'no candidate transition'
if "$APP" --timer-demo does-not-exist.conf > /dev/null 2>&1; then
    echo 'FAIL timer demo: nonexistent file accepted' >&2
    exit 1
fi
echo 'PASS timer CLI: deterministic deadline demonstration'
