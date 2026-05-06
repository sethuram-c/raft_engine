# Building and running a local release candidate

This project can produce an optimized executable and a reproducible source archive. Neither is a production-certified release. Treat the example configs as local development settings.

## Fedora dependencies

```sh
sudo dnf install gcc make python3 openssl openssl-devel
# Optional: sudo dnf install clang
```

Build the normal debug program and run its tests first:

```sh
make clean
make
make test
```

Build the optimized executable and test it separately:

```sh
make release
make test-release
make capstone-report
```

`build/raft-engine-release` uses `-O2`, `_FORTIFY_SOURCE=2` and stack protection. Those flags help catch or mitigate some failures; they are not a substitute for security review.

## Source packages

```sh
make package-source
make verify-source
sha256sum dist/raft-engine-source.zip
```

`tools/raft_release.py` collects allowlisted source, headers, tests, tools and documentation. It excludes Git history, runtime state, compiled binaries, credentials and Python caches. The archive contains `SHA256SUMS`, and the verification command checks its members against that manifest.

Packaging is deterministic for unchanged inputs. `SOURCE_DATE_EPOCH` can be set to control the ZIP entry timestamp. A manifest inside an archive cannot authenticate the person who published it; compare the archive hash against a trusted external record and sign releases before public distribution.

## Local deployment rules

- Use the three distinct node configs under `config/`. They use a fixed membership of three voters.
- Use loopback or another isolated test network. Never expose a plain development listener to the internet.
- Keep per-node data in private directories. Do not clone a live node's WAL to create another voting identity.
- Do not mix full-state snapshot, ordinary WAL and snapshot-aware WAL formats in one participating data directory.
- For TLS mode, provision a separate private key for each node, check the configured CA and match the node ID in the certificate identity.
- Preserve client IDs and request IDs when retrying an uncertain write. A timeout does not imply rollback.
- Inspect `--health-tls`, `--ready-tls` and `--metrics-tls` during a secure experiment. Readiness is not a lease or a guarantee about the next write.
- Stop all nodes before making an offline storage copy; test recovery with disposable data first.

The integration tests generate temporary test certificates and credentials, so they are the easiest way to exercise the secure path without hand-maintaining a CA:

```sh
python3 tests/test_tls_integration.py ./build/raft-engine
python3 tests/test_capstone_integration.py ./build/raft-engine
```

## Checks before tagging a learning release

```sh
git diff --check
git status
make clean && make && make test
make release && make test-release
make test-capstone
make package-source && make verify-source
```

Record the Git commit, compiler and OpenSSL versions, platform and artifact SHA-256 alongside the test results. If a flaky integration fails, inspect the trace and run it again separately; do not silently turn a timing-dependent failure into a success claim.

Still missing: signed releases, a migration/rollback process, independent security audit, a linearizable read API, and real power-loss test coverage. For the full engineering review, see [final-capstone.md](final-capstone.md).
