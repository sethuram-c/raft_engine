# Final integration test and project review

The capstone is an end-to-end test, not another Raft protocol feature. It runs the strongest available development mode: three nodes with mutual TLS, HMAC-authenticated client writes and snapshot-aware WAL persistence. Credentials and node data are created under temporary paths for the test.

## Run it

```sh
make
make test-capstone
make release
make capstone-report
```

The report is saved to `build/capstone-report.json`. To run it directly:

```sh
python3 tools/raft_capstone.py \
  --binary ./build/raft-engine-release \
  --json-out build/my-capstone-report.json
```

For a longer run, add `--extended`, or use `make capstone-extended`.

## What the scenario checks

1. Start three nodes with distinct node certificates and a test CA.
2. Wait for a leader and a positive operational readiness signal.
3. Reject an unauthorized client certificate.
4. Submit an authenticated write and retry the same identity without a second logical application.
5. Reject conflicting reuse of a request ID.
6. Confirm that log, commit and application indices progress in order.
7. Stop a follower and commit with the two remaining voters.
8. Restart the lagging follower and check that it catches up.
9. Kill the cluster, recover from the node-specific snapshot-WAL files and elect a new leader.
10. Commit an entry in the new leader's term, then verify that the original client's most recent request is recognized after restart.

That final current-term write matters: a new leader cannot infer commitment of an older-term entry from its replication count alone. The test also uses a separate client identity for the barrier because the session table only retains the latest request per client.

The JSON report records execution status, durations and the binary digest. It deliberately does not copy key material, HMAC secrets or raw node logs.

## What passing means—and what it does not

Passing shows that this executable completed these scenarios on the machine running the test. It is useful evidence for regression testing, but it is not formal proof of Raft safety, a security audit, or a simulation of a sudden power failure.

Before production use, the project would need at least:

- Linearizable reads and a defined consistency contract for clients.
- Dynamic membership changes and disk-format/rolling upgrade procedures.
- Certificate expiry/revocation, credential distribution and secret management.
- Crash testing that includes disk exhaustion, partial writes and power loss.
- Much broader fault schedules, protocol fuzzing and independent review.
- A better-defined retention/eviction policy for client request sessions.
- Resource isolation and a documented operational recovery procedure.

## Retrospective

Three choices paid off. First, every stage gained focused tests before the next layer was added. Second, request identities were replicated with data; a leader-local dedup cache would have broken after failover. Third, snapshot installation was kept separate from persistent live compaction until the two could be recovered as one consistent state.

The most useful bug-finding work was not the happy-path election demo. It was checking old-term commitment, correcting tests that assumed a particular leader schedule, covering aliased command buffers during log growth, and reproducing failure cases with seeded partitions. Those are the parts I would revisit first when extending this project.

See [architecture.md](architecture.md) for the code path and [release-operations.md](release-operations.md) for the Fedora build and packaging procedure.
