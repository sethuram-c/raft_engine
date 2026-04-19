#!/bin/sh
set -eu
exe=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
output=$($exe --snapshot-demo "$work")
printf '%s\n' "$output" | grep -F 'boundary=2/1 retained=1 replay_applied=3 keys=1' >/dev/null
test -s "$work/phase19.snap"
if "$exe" --snapshot-demo "$work/nonexistent" >/dev/null 2>&1; then
  echo 'FAIL: demo unexpectedly wrote to a missing directory' >&2
  exit 1
fi
printf '%s\n' 'PASS Phase 19: atomic snapshot demo, reload, suffix replay and invalid directory'
