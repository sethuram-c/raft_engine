# Raft Engine

A learning-oriented distributed key-value store being implemented in **C11** on
**Fedora Linux** with **GNU Make** and a fixed three-node Raft cluster.

**Current milestone: Phase 3 of 35.** The in-memory Raft node/log model and the
static cluster configuration parser exist. **There is no network server, leader
election, distributed consensus, or working key-value store yet.**

## What works today

- In-memory Raft node state and dynamically owned binary-safe log entries.
- Log append, term lookup, suffix truncation, and local structural checks.
- Static three-node configuration parsing and validation.
- IPv4 addresses, strict TCP port checks, unique peer IDs and endpoints.
- Informative errors for malformed configuration files.
- CLI verification of three example configs without opening a socket.
- Smoke tests, unit tests, and an isolated CLI integration test.

## Fedora requirements

```sh
sudo dnf install -y gcc make git gdb valgrind
```

## Build and test

```sh
make clean
make
make test
make check-config
make demo
```

## Inspect cluster configuration

```sh
./build/raft-engine --check-config config/node1.conf
./build/raft-engine --check-config config/node2.conf
./build/raft-engine --check-config config/node3.conf
```

Example output for node 2:

```text
Config valid: node_id=2 peers=3
peer 1 127.0.0.1:7001
peer 2 127.0.0.1:7002 (self)
peer 3 127.0.0.1:7003
Config only: no sockets opened or nodes started.
```

## Configuration grammar (Phase 3)

```ini
# A full-line comment
node_id=1
peer=1,127.0.0.1,7001
peer=2,127.0.0.1,7002
peer=3,127.0.0.1,7003
```

- Exactly one `node_id` key and exactly **three** `peer` records are required.
- Node IDs are unique nonzero 32-bit unsigned integers, including the local ID.
- Each peer has `id,IPv4,port` where the decimal port is between 1 and 65535.
- No two peers may reuse the same IPv4 + port endpoint.
- Blank lines, leading/trailing spaces, CRLF, and full-line `#` comments work.
- Inline comments, hostnames, IPv6, environment interpolation, and dynamic
  membership are **not** part of the current format.
- Peers are sorted by ascending node ID after successful validation.
- The local node must appear exactly once; the reader never starts sockets.

All example nodes use `127.0.0.1`, so they are intended for **three processes
on one Linux machine** when TCP support is implemented in later phases.

## Project tree

```text
raft-engine/
  Makefile
  config/node{1,2,3}.conf
  include/{raft_types,raft_log,raft_node,raft_config}.h
  src/{main,raft_log,raft_node,raft_config}.c
  tests/{test_smoke,test_config_cli}.sh
  tests/{test_raft_log,test_raft_node,test_raft_config}.c
  docs/architecture.md
  README.md
  progress.md
```

## Diagnostics

```sh
# Run an individual unit test
./build/test_raft_config

# Trace configuration file system calls (optional)
sudo dnf install -y strace
strace -e trace=openat,read,close ./build/raft-engine --check-config config/node1.conf

# Debug the config loader in GDB
gdb --args ./build/raft-engine --check-config config/node1.conf
```

The comprehensive Phase 3 learning PDF is distributed **outside** this
repository. `.gitignore` excludes PDFs and `learning-guides/`.

## Roadmap

Next: Phase 4 - implement a TCP server that accepts peer connections.
See `progress.md` for milestones and acceptance criteria.
