#!/bin/sh
set -eu
binary=${1:-./build/raft-engine}
output=$($binary --log-demo)
printf '%s\n' "$output" | grep -qF 'Log repair: matched_through=3 total_entries=3'
printf '%s\n' "$output" | grep -qF 'entry[1]: term=1 command=SET x 1'
printf '%s\n' "$output" | grep -qF 'entry[2]: term=3 command=SET y 2'
printf '%s\n' "$output" | grep -qF 'entry[3]: term=3 command=SET z 3'
printf '%s\n' "$output" | grep -qF 'In-memory demonstration only'
printf 'PASS: Phase 11 CLI log reconciliation demo\n'
