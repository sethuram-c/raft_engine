# Phase 34 — Release-readiness and Fedora deployment operations

> **Classification: educational release candidate, not production ready.**
> Phase 34 adds an optimized binary build, deterministic source package,
> integrity validation, release smoke tests, a repeatable acceptance gate and
> an operations checklist. It does **not** authorize public-network deployment.

## 1. Scope and supported environment

- **Platform:** Fedora Linux, C11 / GNU Make / Python 3, x86-64 or other Linux
  architectures compatible with the existing application; portable beyond the
  tested runner is not established.
- **Dependencies:** `gcc`, `make`, `python3`, `openssl-devel`, `openssl`, with
  `clang` recommended for separate sanitizer testing.
- **Cluster configuration:** exactly three voters, static IPv4 endpoints;
  example `config/node*.conf` files bind to loopback `127.0.0.1:7001-7003`.
- **Persistence:** opt-in modes from earlier phases. Keep each node's data files
  in a protected storage directory; do **not** mix snapshot, WAL and snapshot-WAL
  formats or clone a live node's state to a second voter.
- **Security:** Phase 26 mutual TLS plus Phase 25 HMAC write admission is the
  strongest available mode, but it still has important limitations described
  below. Plain development modes must stay on isolated localhost networks.

## 2. Reproducible source checkout and build

```sh
sudo dnf install gcc make python3 openssl openssl-devel
# Optional: sudo dnf install clang
make clean
make                     # debug application: build/raft-engine
make release             # separate optimized candidate: build/raft-engine-release
make test-release        # optimized CLI/config smoke; archive integrity tests
make test                # full debug C and network regression suite
# Equivalent all-in-one acceptance gate (takes longer):
make check-release
```

The candidate compiles with GCC `-O2 -g -D_FORTIFY_SOURCE=2
-fstack-protector-strong` plus the existing warning-as-error flags and OpenSSL
linkage. Test objects continue using the debug `-O0 -g` path. A compilation
flag is not proof that the code is memory-safe or production-hardened.

**Exact binary reproduction across different compilers or Fedora versions is
not promised.** Record compiler/OpenSSL/OS versions for a real release.

```sh
gcc --version | head -1
openssl version
uname -a
sha256sum build/raft-engine-release
```

## 3. Deterministic source package and integrity verification

```sh
make package-source
make verify-source
sha256sum dist/raft-engine-source.zip

# Or set a common reproducible ZIP timestamp (UTC epoch seconds):
SOURCE_DATE_EPOCH=1767225600 make package-source
make verify-source
```

`tools/raft_release.py` includes only allowlisted C source, headers, tests,
Python tools, sample `.conf`, Markdown documentation, the Makefile, and
`.gitignore`. It intentionally omits `build`, `dist`, `.git`, Python caches,
runtime data, PDFs, certificates and private keys. It rejects selected source
symlinks, duplicate/unsafe archive paths, overlarge source files and invalid
ZIP entries. Every source entry is listed in `SHA256SUMS` inside the ZIP;
`verify-source` checks that manifest against the actual archive contents.

The default timestamp is the earliest ZIP-compatible date, 1980-01-01 UTC.
For an explicit timestamp, `SOURCE_DATE_EPOCH` is interpreted as UTC and rounded
down to two seconds (ZIP's timestamp resolution). Identical source and toolchain
produce identical source ZIP bytes; file mtimes do not affect the result.

**A checksum is not a signature.** A malicious actor who changes both source
and the internal manifest can still produce a passing archive. Compare the
external ZIP SHA256 to a value obtained via a trusted, separately authenticated
channel. Signing and formal provenance attestations are future work.

## 4. Archive and build acceptance checklist

- [ ] Verify archive SHA256 against a trusted release record.
- [ ] Extract only after verifying with `tools/raft_release.py verify`.
- [ ] Check that no `*.key`, `*.pem`, runtime WAL/snapshot files, or binaries
      appear in the unpacked source.
- [ ] Record `gcc --version`, `openssl version`, OS/kernel and source archive hash.
- [ ] Run `make clean && make && make test` on a Fedora host.
- [ ] Run `make release && make test-release` on the same host.
- [ ] Run `make test-chaos` and `make test-bench` separately for acceptance of
      the failure and performance harnesses if not already covered by `make test`.
- [ ] Run a repeat of the snapshot-WAL SIGKILL/restart tests in an **isolated**
      temporary directory; never on valuable cluster data.
- [ ] Obtain a maintainer review of the Phase 34 diff and open issues.

These boxes are a manual operations checklist. Automated checks only cover what
is explicitly invoked and must not be read as a deployment certification.

## 5. Safe local cluster operation

1. Verify that the three node IDs and IPv4 endpoints are distinct and match
   each node's configuration using `--check-config`.
2. Use loopback for experiments. Do not forward port 7001-7003 or expose an
   unauthenticated development mode beyond the host.
3. For persistent mTLS trials, provision an isolated private directory, a
   trusted CA and three different `raft-node-ID` certificates. Supply one
   correctly permissioned node private key per process. Use a `raft-client`
   certificate plus an HMAC client key to submit writes.
4. Start nodes with the Phase 26 `--elect-tls-auth-wal` command in separate
   terminals, with **distinct local node configuration files and the same
   secure storage parent directory**. The server names WAL files per node.
5. Inspect `--health-tls` / `--ready-tls` and `--metrics-tls` before and after
   writes. Health is operational only; it is not a quorum-lease proof.
6. Use `--write-tls-auth-auto` with stable client/request IDs. Do not change
   IDs after an `UNKNOWN` result until the original operation is resolved.
7. Test graceful SIGTERM then a controlled restart with the same node-specific
   storage. Make a separately reviewed, offline backup only with all nodes
   stopped, and never duplicate a node state into another voting identity.

Example (replace the absolute credential paths and node config for each node):

```sh
./build/raft-engine-release --check-config config/node1.conf
./build/raft-engine-release --elect-tls-auth-wal \
  config/node1.conf data-secure \
  /secure/raft/client.keys /secure/raft/node1.crt \
  /secure/raft/node1.key /secure/raft/ca-bundle.crt
```

The sample config's fixed loopback ports may already be in use. Do not change
peer membership dynamically: cluster reconfiguration is not implemented.

## 6. Failure and rollback runbook

| Symptom | Safe first response | Avoid |
|---|---|---|
| Startup rejects configuration | Run `--check-config`, inspect endpoint/ID uniqueness | Forcing a conflicting duplicate identity |
| TLS handshake or peer identity fails | Check CA overlap, exact SAN `raft-node-ID`, key permissions, clock validity | Disabling authentication on the live secure cluster |
| Readiness expires | Observe leader term, quorum communication and metrics | Assuming `ready=0` means prior writes rolled back |
| Write returns `UNKNOWN` | Retry identical client ID, request ID and command | Blindly incrementing IDs or changing command bytes |
| WAL/snapshot corruption | Stop affected node, preserve forensic copy, investigate checksums | Overwriting or deleting durable state to force startup |
| Disk is full or `fsync` fails | Stop writes, preserve files, remediate storage offline | Treating failure as a successful commit |
| Split-brain concern | Halt experiments, preserve per-node logs, check terms and commit state | Trusting a single status sample as proof of global safety |
| Cert rotation fails | Keep last valid TLS context; inspect logs and corrected key pair | Simultaneously removing old CA trust from all nodes |

Safe rollback is limited to **source and binary rollback while respecting disk
format compatibility**. Do not use earlier versions on files they cannot parse
or mix running executables with undocumented protocol/format assumptions.

## 7. Explicit known limitations

This repository is an educational prototype. Open operational gaps include:

- Exactly three static voters; no membership changes, joint consensus or
  rolling upgrade compatibility framework.
- No production-level secret storage, key revocation automation, certificate
  renewal orchestration, or full adversarial threat-model audit.
- No guaranteed linearizable GET; diagnostic reads are local.
- Write timeouts can be indeterminate. Phase 23 deduplication has bounded
  client-session capacity and sequential-request requirements.
- No publicly reviewed protocol implementation, formal model checking, or
  verified crash consistency under arbitrary power-loss, bit rot and storage
  hardware behavior.
- No safe online migration across historical WAL/snapshot modes, no external
  backup/restore automation, and no production operations control plane.
- Metrics and operational health checks are advisory; they are not safety
  certificates or leadership leases.
- Performance results from a local host or benchmark suite are not a
  capacity-planning guarantee.
- SHA256SUMS gives package-integrity checking but not software authenticity;
  signed releases, SBOM, CVE tracking and CI/CD policy enforcement are absent.
- **No license grant is included in the project source.** Establish project
  licensing explicitly before public redistribution.

**Recommended decision for Phase 34:** the source and educational release
candidate can be shared for local study; deployment to untrusted or public
networks remains **NO-GO** pending hardening and review.
