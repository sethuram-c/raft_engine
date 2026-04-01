# Development progress

## Phase 1 / 35 — Fedora setup and Make-based C project

**Time budget:** approximately 2 hours

### Deliverable

A version-controlled C11 application that compiles with GNU Make and passes a runnable smoke test.

### Acceptance criteria

- [ ] Fedora development tools installed
- [ ] `make` succeeds without warnings
- [ ] `make run` prints startup message
- [ ] `./build/raft-engine --version` prints `raft-engine 0.1.0`
- [ ] `make test` passes all smoke checks
- [ ] Git repository initialized and phase committed
- [ ] Phase 1 pushed to GitHub

### Verification commands

```sh
make clean
make
make test
git status
```

### Commit suggestion

```text
chore: initialize C11 raft engine with Make and smoke tests
```

Keep actual commit timestamps unless they reflect work genuinely completed at a different recorded time.
