# Raft Engine

A distributed key-value store written in **C11**, built incrementally using the **Raft consensus algorithm**.

**Current status:** Phase 1 of 35 — project bootstrap only. Raft consensus and networking are not implemented yet.

## Planned features

- Leader election and heartbeats
- Replicated logs with majority-based commits
- TCP communication between a fixed three-node cluster
- Key-value state machine
- Persistent Raft state and crash recovery
- Fault-tolerance tests and performance benchmarks

## Development environment

- Fedora Linux
- GCC with C11 support
- GNU Make
- Git, GDB, and Valgrind

Install tools on Fedora:

```sh
sudo dnf install -y gcc make git gdb valgrind
```

## Build, run, and test

```sh
make          # Build build/raft-engine
make run      # Run the program
make test     # Run automated smoke tests
make clean    # Remove generated build files
```

Check the version:

```sh
./build/raft-engine --version
```

Expected output:

```text
raft-engine 0.1.0
```

## Structure

```text
raft-engine/
├── Makefile
├── src/
│   └── main.c
├── include/
│   └── .gitkeep
├── tests/
│   └── test_smoke.sh
├── docs/
│   └── architecture.md
├── README.md
├── progress.md
└── .gitignore
```

## Scope

This is an educational distributed-systems implementation, not production-ready infrastructure. Advanced features such as dynamic membership, snapshots, and linearizable read coordination are outside the initial 35-phase scope.
