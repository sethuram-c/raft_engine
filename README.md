# Raft Engine

A learning-oriented distributed key-value store under construction in **C11**
on **Fedora Linux**, using **GNU Make** and a fixed three-node Raft cluster.

**Current milestone: Phase 4 of 35.** Nodes now support TCP listeners and
connection-only peer probes. **There is still no message framing, Raft RPC,
leader election, replicated consensus, or working key-value store.**

## Features implemented so far

- Binary-safe, dynamically owned in-memory Raft log and basic node state.
- Static three-node configuration parser with strict IPv4/port validation.
- POSIX TCP listener: `socket` -> `setsockopt` -> `bind` -> `listen` -> `accept`.
- CLI connection probe using nonblocking `connect`, `poll` and `SO_ERROR`.
- Explicit descriptor cleanup and SIGINT/SIGTERM-controlled listener shutdown.
- Integration tests using real loopback TCP connections and temporary ports.

The Phase 4 listener **immediately closes each accepted connection**. This is
intentional: message framing and RPC handling arrive in later phases. A
successful `--probe` only confirms a TCP handshake, not Raft health or an
authenticated peer.

## Fedora requirements

```sh
sudo dnf install -y gcc make python3 git gdb valgrind
# Optional diagnostic utilities:
sudo dnf install -y iproute netcat-openbsd strace tcpdump
```

## Build and test

```sh
make clean
make
make test
make check-config
make demo
```

`make test` covers the prior log/node/config tests plus Phase 4 loopback TCP
integration tests. The test creates temporary config files and chooses available
local ports; it does not require the example ports 7001-7003 to be free.

## Run a node's TCP listener

Terminal A:

```sh
./build/raft-engine --serve config/node1.conf
```

Expected initial line:

```text
Listening: node=1 127.0.0.1:7001 (max_connections=0)
```

Terminal B (a separate process acting as node 2):

```sh
./build/raft-engine --probe config/node2.conf 1
```

Expected:

```text
Connected: local_node=2 peer=1 127.0.0.1:7001
Probe only: no Raft messages sent.
```

Terminal A reports an accepted connection. Press `Ctrl+C` in Terminal A to
stop it. `0` in the server status is the unlimited connection limit, not a
maximum of zero connections.

Run a bounded listener that exits after two connections:

```sh
./build/raft-engine --serve config/node1.conf --max-connections 2
```

Run three listeners in three terminals (they listen independently; no Raft
traffic flows between them yet):

```sh
./build/raft-engine --serve config/node1.conf
./build/raft-engine --serve config/node2.conf
./build/raft-engine --serve config/node3.conf
```

Use **one command per terminal** or launch them as separate background jobs.

## Inspect listening sockets (Linux)

```sh
ss -ltnp | grep -E ':7001|:7002|:7003'
```

Inspect configured peers without opening a socket:

```sh
./build/raft-engine --check-config config/node1.conf
```

## CLI syntax

```text
raft-engine --version
raft-engine --demo
raft-engine --check-config PATH
raft-engine --serve PATH [--max-connections POSITIVE_INTEGER]
raft-engine --probe PATH PEER_ID
```

`--serve` uses the local node ID from its config, while `--probe` connects to
the selected peer ID in the configuration and immediately closes the socket.
`--probe` applies a 1000 ms connection timeout.

## Current limitations

- IPv4 literals only; sample configs use loopback `127.0.0.1`.
- No authentication, encryption, wire framing, heartbeat, or election logic.
- Sequential, single-process accept loop; each connection is immediately closed.
- No payload exchange, and no peer identity verification at the TCP layer.
- Fixed membership of exactly three nodes; no cluster formation yet.
- Manual demonstration ports can already be occupied by other applications.

## Project structure

```text
raft-engine/
  Makefile
  config/node{1,2,3}.conf
  include/{raft_types,raft_log,raft_node,raft_config,raft_tcp}.h
  src/{main,raft_log,raft_node,raft_config,raft_tcp}.c
  tests/{test_smoke,test_config_cli}.sh
  tests/{test_raft_log,test_raft_node,test_raft_config}.c
  tests/test_tcp_integration.py
  docs/architecture.md
  README.md
  progress.md
```

The separate Phase 4 learning PDF is deliberately **not** in this repository.
`.gitignore` excludes PDF files and `learning-guides/`.

**Next:** Phase 5 - design and implement a framed Raft message protocol.
