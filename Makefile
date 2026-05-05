CC = gcc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g
LDFLAGS ?=
THREAD_FLAGS = -pthread
LDLIBS ?= -lssl -lcrypto
PYTHON ?= python3

# Phase 34: hardened, separately built release candidate (tests still use the
# debug build, preserving assert-based test coverage).
RELEASE_APP = build/raft-engine-release
RELEASE_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
                  -D_FORTIFY_SOURCE=2 -fstack-protector-strong
SOURCE_PACKAGE ?= dist/raft-engine-source.zip

APP = build/raft-engine
LIB_SOURCES = src/raft_log.c src/raft_node.c src/raft_config.c src/raft_tcp.c src/raft_wire.c src/raft_timer.c src/raft_vote.c src/raft_election.c src/raft_append.c src/raft_replication.c src/raft_commit.c src/raft_state_machine.c src/raft_replicate_client.c src/raft_storage.c src/raft_wal.c src/raft_snapshot.c src/raft_install.c src/raft_snapwal.c src/raft_client_write.c src/raft_dedup.c src/raft_client_auto.c src/raft_auth.c src/raft_tls.c src/raft_metrics.c src/raft_health.c src/raft_fault.c
HEADERS = $(wildcard include/*.h)
TEST_LOG = build/test_raft_log
TEST_NODE = build/test_raft_node
TEST_CONFIG = build/test_raft_config
TEST_WIRE = build/test_raft_wire
TEST_TIMER = build/test_raft_timer
TEST_VOTE = build/test_raft_vote
TEST_ELECTION = build/test_raft_election
TEST_APPEND = build/test_raft_append
TEST_RECONCILE = build/test_raft_reconcile
TEST_REPLICATION = build/test_raft_replication
TEST_PROGRESS = build/test_raft_progress
TEST_COMMIT = build/test_raft_commit
TEST_APPLY = build/test_raft_state_machine
TEST_STORAGE = build/test_raft_storage
TEST_WAL = build/test_raft_wal
TEST_CHECKPOINT = build/test_raft_checkpoint
TEST_SNAPSHOT = build/test_raft_snapshot
TEST_INSTALL = build/test_raft_install
TEST_SNAPWAL = build/test_raft_snapwal
TEST_CLIENT_WRITE = build/test_raft_client_write
TEST_DEDUP = build/test_raft_dedup
TEST_AUTO = build/test_raft_client_auto
TEST_AUTH = build/test_raft_auth
TEST_METRICS = build/test_raft_metrics
TEST_HEALTH = build/test_raft_health
TEST_FAULT = build/test_raft_fault
TEST_RECONCILE_PERF = build/test_raft_reconcile_perf
RECONCILE_BENCH = build/raft-reconcile-bench

.PHONY: all run demo check-config test test-smoke test-unit test-network test-chaos test-bench bench bench-reconcile release package-source verify-source test-release check-release clean
all: $(APP)

$(APP): src/main.c $(LIB_SOURCES) $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) src/main.c $(LIB_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

# Release candidate build keeps debug binaries available for regressions.
$(RELEASE_APP): src/main.c $(LIB_SOURCES) $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) $(THREAD_FLAGS) src/main.c $(LIB_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

release: $(RELEASE_APP)

package-source:
	$(PYTHON) tools/raft_release.py create --root . --output $(SOURCE_PACKAGE)

verify-source:
	$(PYTHON) tools/raft_release.py verify --archive $(SOURCE_PACKAGE)

capstone-report: release
	$(PYTHON) tools/raft_capstone.py --binary ./$(RELEASE_APP) --json-out build/capstone-report.json

capstone-extended: release
	$(PYTHON) tools/raft_capstone.py --binary ./$(RELEASE_APP) --json-out build/capstone-extended.json --extended

test-release: release
	$(PYTHON) tests/test_release_smoke.py ./$(RELEASE_APP)
	$(PYTHON) tests/test_release_package.py
	# Exercise the optimized executable over real TCP and mutual TLS as well.
	$(PYTHON) tests/test_client_write_integration.py ./$(RELEASE_APP)
	$(PYTHON) tests/test_tls_integration.py ./$(RELEASE_APP)
	$(MAKE) package-source
	$(MAKE) verify-source

# This is the slower complete acceptance gate for release candidates.
check-release: test-release test capstone-report

$(TEST_LOG): tests/test_raft_log.c src/raft_log.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_log.c src/raft_log.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_NODE): tests/test_raft_node.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_node.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_CONFIG): tests/test_raft_config.c src/raft_config.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_config.c src/raft_config.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_WIRE): tests/test_raft_wire.c src/raft_wire.c src/raft_tls.c src/raft_config.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_wire.c src/raft_wire.c src/raft_tls.c src/raft_config.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_TIMER): tests/test_raft_timer.c src/raft_timer.c src/raft_node.c src/raft_log.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_timer.c src/raft_timer.c src/raft_node.c src/raft_log.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_VOTE): tests/test_raft_vote.c src/raft_vote.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_vote.c src/raft_vote.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_ELECTION): tests/test_raft_election.c src/raft_election.c src/raft_vote.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_election.c src/raft_election.c src/raft_vote.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_APPEND): tests/test_raft_append.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_append.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_RECONCILE): tests/test_raft_reconcile.c src/raft_log.c src/raft_node.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_reconcile.c src/raft_log.c src/raft_node.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_REPLICATION): tests/test_raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c src/raft_wire.c src/raft_tls.c src/raft_config.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c src/raft_wire.c src/raft_tls.c src/raft_config.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_PROGRESS): tests/test_raft_progress.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_progress.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_COMMIT): tests/test_raft_commit.c src/raft_commit.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_commit.c src/raft_commit.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_APPLY): tests/test_raft_state_machine.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_state_machine.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_STORAGE): tests/test_raft_storage.c src/raft_storage.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_storage.c src/raft_storage.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_WAL): tests/test_raft_wal.c src/raft_wal.c src/raft_storage.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_wal.c src/raft_wal.c src/raft_storage.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_CHECKPOINT): tests/test_raft_checkpoint.c src/raft_wal.c src/raft_storage.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_checkpoint.c src/raft_wal.c src/raft_storage.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_SNAPSHOT): tests/test_raft_snapshot.c src/raft_snapshot.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_log.c src/raft_node.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_snapshot.c src/raft_snapshot.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_log.c src/raft_node.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_INSTALL): tests/test_raft_install.c src/raft_install.c src/raft_snapshot.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_log.c src/raft_node.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_install.c src/raft_install.c src/raft_snapshot.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_log.c src/raft_node.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_SNAPWAL): tests/test_raft_snapwal.c src/raft_snapwal.c src/raft_storage.c src/raft_snapshot.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_snapwal.c src/raft_snapwal.c src/raft_storage.c src/raft_snapshot.c src/raft_state_machine.c src/raft_dedup.c src/raft_client_write.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_DEDUP): tests/test_raft_dedup.c src/raft_dedup.c src/raft_client_write.c src/raft_snapshot.c src/raft_state_machine.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_dedup.c src/raft_dedup.c src/raft_client_write.c src/raft_snapshot.c src/raft_state_machine.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_CLIENT_WRITE): tests/test_raft_client_write.c src/raft_client_write.c src/raft_state_machine.c src/raft_dedup.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_client_write.c src/raft_client_write.c src/raft_state_machine.c src/raft_dedup.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_AUTO): tests/test_raft_client_auto.c src/raft_client_auto.c src/raft_dedup.c src/raft_client_write.c src/raft_config.c src/raft_tcp.c src/raft_wire.c src/raft_node.c src/raft_log.c src/raft_timer.c src/raft_vote.c src/raft_election.c src/raft_append.c src/raft_replication.c src/raft_commit.c src/raft_state_machine.c src/raft_replicate_client.c src/raft_storage.c src/raft_wal.c src/raft_snapshot.c src/raft_install.c src/raft_snapwal.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_client_auto.c $(LIB_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_AUTH): tests/test_raft_auth.c src/raft_auth.c src/raft_dedup.c src/raft_client_write.c src/raft_state_machine.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_auth.c src/raft_auth.c src/raft_dedup.c src/raft_client_write.c src/raft_state_machine.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_METRICS): tests/test_raft_metrics.c src/raft_metrics.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_metrics.c src/raft_metrics.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_HEALTH): tests/test_raft_health.c src/raft_health.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_health.c src/raft_health.c $(LDFLAGS) $(LDLIBS) -o $@

build:
	mkdir -p build

run: $(APP)
	./$(APP)

demo: $(APP)
	./$(APP) --demo
	./$(APP) --timer-demo config/node1.conf
	./$(APP) --log-demo

check-config: $(APP)
	./$(APP) --check-config config/node1.conf
	./$(APP) --check-config config/node2.conf
	./$(APP) --check-config config/node3.conf

test-smoke: $(APP)
	sh tests/test_smoke.sh ./$(APP)

$(TEST_FAULT): tests/test_raft_fault.c src/raft_fault.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_fault.c src/raft_fault.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_RECONCILE_PERF): tests/test_raft_reconcile_perf.c src/raft_log.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_reconcile_perf.c src/raft_log.c $(LDFLAGS) $(LDLIBS) -o $@

$(RECONCILE_BENCH): tools/raft_reconcile_bench.c src/raft_log.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -O2 tools/raft_reconcile_bench.c src/raft_log.c $(LDFLAGS) $(LDLIBS) -o $@

bench-reconcile: $(RECONCILE_BENCH)
	./$(RECONCILE_BENCH) 8192

test-unit: $(TEST_RECONCILE_PERF) $(TEST_LOG) $(TEST_NODE) $(TEST_CONFIG) $(TEST_WIRE) $(TEST_TIMER) $(TEST_VOTE) $(TEST_ELECTION) $(TEST_APPEND) $(TEST_RECONCILE) $(TEST_REPLICATION) $(TEST_PROGRESS) $(TEST_COMMIT) $(TEST_APPLY) $(TEST_STORAGE) $(TEST_WAL) $(TEST_CHECKPOINT) $(TEST_SNAPSHOT) $(TEST_INSTALL) $(TEST_SNAPWAL) $(TEST_CLIENT_WRITE) $(TEST_DEDUP) $(TEST_AUTO) $(TEST_AUTH) $(TEST_METRICS) $(TEST_HEALTH) $(TEST_FAULT)
	./$(TEST_LOG)
	./$(TEST_RECONCILE_PERF)
	./$(TEST_NODE)
	./$(TEST_CONFIG)
	./$(TEST_WIRE)
	./$(TEST_TIMER)
	./$(TEST_VOTE)
	./$(TEST_ELECTION)
	./$(TEST_APPEND)
	./$(TEST_RECONCILE)
	./$(TEST_REPLICATION)
	./$(TEST_PROGRESS)
	./$(TEST_COMMIT)
	./$(TEST_APPLY)
	./$(TEST_STORAGE)
	./$(TEST_WAL)
	./$(TEST_CHECKPOINT)
	./$(TEST_SNAPSHOT)
	./$(TEST_INSTALL)
	./$(TEST_SNAPWAL)
	./$(TEST_CLIENT_WRITE)
	./$(TEST_DEDUP)
	./$(TEST_AUTO)
	./$(TEST_AUTH)
	./$(TEST_METRICS)
	./$(TEST_HEALTH)
	./$(TEST_FAULT)
	$(PYTHON) tests/test_chaos_plan.py
	$(PYTHON) tests/test_bench_metrics.py
	$(PYTHON) tests/test_release_package.py
	$(PYTHON) tests/test_capstone_report.py

test-chaos: $(APP)
	$(PYTHON) tests/test_chaos_plan.py
	$(PYTHON) tests/test_chaos_integration.py ./$(APP)

test-bench: $(APP)
	$(PYTHON) tests/test_bench_metrics.py
	$(PYTHON) tests/test_benchmark_integration.py ./$(APP)

bench: $(APP)
	$(PYTHON) tools/raft_bench.py --binary ./$(APP) --writes 24 --concurrency 4 --warmup 4 --json-out build/bench-results.json --csv-out build/bench-samples.csv

test-network: $(APP)
	$(PYTHON) tests/test_tcp_integration.py ./$(APP)
	$(PYTHON) tests/test_wire_integration.py ./$(APP)
	$(PYTHON) tests/test_concurrent_tcp.py ./$(APP)
	$(PYTHON) tests/test_vote_integration.py ./$(APP)
	$(PYTHON) tests/test_election_integration.py ./$(APP)
	$(PYTHON) tests/test_heartbeat_integration.py ./$(APP)
	$(PYTHON) tests/test_replication_integration.py ./$(APP)
	$(PYTHON) tests/test_progress_integration.py ./$(APP)
	$(PYTHON) tests/test_commit_integration.py ./$(APP)
	$(PYTHON) tests/test_apply_integration.py ./$(APP)
	$(PYTHON) tests/test_storage_integration.py ./$(APP)
	$(PYTHON) tests/test_wal_integration.py ./$(APP)
	$(PYTHON) tests/test_checkpoint_integration.py ./$(APP)
	$(PYTHON) tests/test_install_integration.py ./$(APP)
	$(PYTHON) tests/test_snapwal_integration.py ./$(APP)
	$(PYTHON) tests/test_client_write_integration.py ./$(APP)
	$(PYTHON) tests/test_client_write_durable.py ./$(APP)
	$(PYTHON) tests/test_dedup_integration.py ./$(APP)
	$(PYTHON) tests/test_auto_client_integration.py ./$(APP)
	$(PYTHON) tests/test_auth_integration.py ./$(APP)
	$(PYTHON) tests/test_tls_integration.py ./$(APP)
	$(PYTHON) tests/test_tls_lifecycle.py ./$(APP)
	$(PYTHON) tests/test_metrics_integration.py ./$(APP)
	$(PYTHON) tests/test_health_integration.py ./$(APP)
	$(PYTHON) tests/test_fault_integration.py ./$(APP)
	$(PYTHON) tests/test_chaos_integration.py ./$(APP)
	$(PYTHON) tests/test_benchmark_integration.py ./$(APP)
	$(PYTHON) tests/test_capstone_integration.py ./$(APP)

test-capstone: $(APP)
	$(PYTHON) tests/test_capstone_integration.py ./$(APP)

test: test-smoke test-unit
	sh tests/test_config_cli.sh ./$(APP)
	sh tests/test_timer_cli.sh ./$(APP)
	sh tests/test_log_demo.sh ./$(APP)
	sh tests/test_snapshot_demo.sh ./$(APP)
	$(MAKE) test-network

clean:
	rm -rf build
