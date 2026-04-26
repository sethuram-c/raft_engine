CC = gcc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g
LDFLAGS ?=
THREAD_FLAGS = -pthread
LDLIBS ?= -lssl -lcrypto
PYTHON ?= python3

APP = build/raft-engine
LIB_SOURCES = src/raft_log.c src/raft_node.c src/raft_config.c src/raft_tcp.c src/raft_wire.c src/raft_timer.c src/raft_vote.c src/raft_election.c src/raft_append.c src/raft_replication.c src/raft_commit.c src/raft_state_machine.c src/raft_replicate_client.c src/raft_storage.c src/raft_wal.c src/raft_snapshot.c src/raft_install.c src/raft_snapwal.c src/raft_client_write.c src/raft_dedup.c src/raft_client_auto.c src/raft_auth.c src/raft_tls.c
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

.PHONY: all run demo check-config test test-smoke test-unit test-network clean
all: $(APP)

$(APP): src/main.c $(LIB_SOURCES) $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) src/main.c $(LIB_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

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

test-unit: $(TEST_LOG) $(TEST_NODE) $(TEST_CONFIG) $(TEST_WIRE) $(TEST_TIMER) $(TEST_VOTE) $(TEST_ELECTION) $(TEST_APPEND) $(TEST_RECONCILE) $(TEST_REPLICATION) $(TEST_PROGRESS) $(TEST_COMMIT) $(TEST_APPLY) $(TEST_STORAGE) $(TEST_WAL) $(TEST_CHECKPOINT) $(TEST_SNAPSHOT) $(TEST_INSTALL) $(TEST_SNAPWAL) $(TEST_CLIENT_WRITE) $(TEST_DEDUP) $(TEST_AUTO) $(TEST_AUTH)
	./$(TEST_LOG)
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

test: test-smoke test-unit
	sh tests/test_config_cli.sh ./$(APP)
	sh tests/test_timer_cli.sh ./$(APP)
	sh tests/test_log_demo.sh ./$(APP)
	sh tests/test_snapshot_demo.sh ./$(APP)
	$(MAKE) test-network

clean:
	rm -rf build
