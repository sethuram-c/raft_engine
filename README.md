# Raft Engine

A learning-oriented distributed key-value store under construction in **C11**
on **Fedora Linux**, using **GNU Make** and a fixed three-node configuration.

**Current milestone: Phase 5 / 35 - TCP message framing and serialization.**
The application exchanges bounded, binary-safe **PING/PONG** messages over
real TCP sockets. **This is not yet Raft RPC, leader election, replicated
consensus, an authenticated peer transport, or a working key-value store.**

## Implemented through Phase 5

- Raft term/node/index types, heap-owned in-memory log and node lifecycle.
- Strict three-node IPv4 configuration parser with uniqueness checks.
- TCP listen/accept, bounded nonblocking connect, clean SIGINT/SIGTERM shutdown.
- A 12-byte, network-byte-order header; exact-length send/receive helpers.
- Wire header validation: magic, protocol version, type, reserved bytes and
  maximum payload size (4096 bytes).
- Sequential one-request-per-connection PING/PONG echo demonstration.
- C unit tests and Python real-socket integration tests, including deliberately
  fragmented TCP writes, binary payloads and malformed/truncated frames.

## Prerequisites (Fedora)

```sh
sudo dnf install -y gcc make python3 git gdb valgrind
# Optional for inspection: sudo dnf install -y iproute strace tcpdump
```

## Build and test

```sh
make clean
make
make test
make check-config
make demo
```

For debug diagnostics (separate from a normal build):

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'
make test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'
make clean
make
```

`make test` runs Phase 1-5 suites. Loopback tests select temporary endpoints,
so example ports 7001-7003 do not have to be free.

## Run the Phase 5 PING/PONG demo

Terminal 1:

```sh
./build/raft-engine --serve config/node1.conf
```

Terminal 2:

```sh
./build/raft-engine --ping config/node2.conf 1 hello-raft
```

Expected client output:

```text
PONG: local_node=2 peer=1 bytes=10 message=hello-raft
Demo only: no Raft RPC or consensus performed.
```

The listener reports `Accepted:` and `Replied:` events. Press `Ctrl+C` in
Terminal 1 to stop it. `--serve` accepts one framed request per connection,
replies only to PING and closes the socket. It is sequential, not concurrent.

The earlier Phase 4 connection-only command still works:

```sh
./build/raft-engine --probe config/node2.conf 1
```

`--probe` does not send a frame. The listener closes it without sending a PONG.

## The Phase 5 wire format

Every frame is exactly **12-byte header + payload**:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 4 | Magic `0x52414654` (ASCII `RAFT`) |
| 4 | 1 | Version: `1` |
| 5 | 1 | Type: `1=PING`, `2=PONG` |
| 6 | 2 | Reserved: must be zero |
| 8 | 4 | Unsigned payload length, big-endian (0-4096) |
| 12 | N | Opaque binary payload, not NUL terminated |

Example `PING` for `abc` in hexadecimal:

```text
52 41 46 54  01 01  00 00  00 00 00 03  61 62 63
```

This wire encoding is a **teaching/demo transport**, not the final Raft RPC
schema. Future RPCs will add term, node ID, log indices and message variants.

## CLI

```text
raft-engine --version
raft-engine --demo
raft-engine --check-config PATH
raft-engine --serve PATH [--max-connections POSITIVE_INTEGER]
raft-engine --probe PATH PEER_ID
raft-engine --ping PATH PEER_ID MESSAGE
```

## Limitations and trust model

- IPv4 literals only; sample configs use `127.0.0.1`.
- Single-threaded TCP listener handles one connection at a time.
- 1-second socket **idle** timeouts are not total request deadlines.
- No peer authentication, TLS, persistent connections or replay protection.
- Protocol errors close only the affected connection; no error frame is sent.
- PING and PONG are demonstrations, not Raft RPCs or health guarantees.
- TCP packet boundaries do not equal message boundaries; reassembly is done
  by `raft_wire_recv`, and multiple frames can be decoded from a stream.
- Do not expose this learning server to untrusted networks.

The detailed Phase 5 learning PDF is a **separate local artifact** and is not
stored in this repository. `.gitignore` excludes PDF and `learning-guides/`.
