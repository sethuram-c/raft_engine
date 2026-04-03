#!/bin/sh
set -eu
binary=${1:-./build/raft-engine}
for i in 1 2 3; do
    output=$($binary --check-config "config/node$i.conf")
    printf '%s\n' "$output" | grep -qF "Config valid: node_id=$i peers=3"
    printf '%s\n' "$output" | grep -qF "peer $i 127.0.0.1:700$i (self)"
    printf '%s\n' "$output" | grep -qF 'no sockets opened'
    printf 'PASS: CLI config for node %s\n' "$i"
done
error_file=$(mktemp)
trap 'rm -f "$error_file"' EXIT HUP INT TERM
if "$binary" --check-config /tmp/no-such-raft-config-file 2>"$error_file"; then
    echo 'FAIL: nonexistent config must fail' >&2
    exit 1
fi
grep -qF 'Configuration I/O error:' "$error_file"
if "$binary" --check-config 2>"$error_file"; then
    echo 'FAIL: missing config path must fail' >&2
    exit 1
fi
grep -qF 'Usage: raft-engine' "$error_file"
printf 'PASS: CLI failure cases\n'
printf 'Config CLI tests passed.\n'
