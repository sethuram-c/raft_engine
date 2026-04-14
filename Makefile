CC = gcc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g
LDFLAGS ?=
THREAD_FLAGS = -pthread
LDLIBS ?=
PYTHON ?= python3

APP = build/raft-engine
LIB_SOURCES = src/raft_log.c src/raft_node.c src/raft_config.c src/raft_tcp.c src/raft_wire.c src/raft_timer.c src/raft_vote.c src/raft_election.c src/raft_append.c src/raft_replication.c src/raft_commit.c src/raft_replicate_client.c
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

$(TEST_WIRE): tests/test_raft_wire.c src/raft_wire.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_wire.c src/raft_wire.c $(LDFLAGS) $(LDLIBS) -o $@

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

$(TEST_REPLICATION): tests/test_raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c src/raft_wire.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c src/raft_wire.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_PROGRESS): tests/test_raft_progress.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_progress.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_COMMIT): tests/test_raft_commit.c src/raft_commit.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) tests/test_raft_commit.c src/raft_commit.c src/raft_replication.c src/raft_append.c src/raft_node.c src/raft_log.c src/raft_timer.c $(LDFLAGS) $(LDLIBS) -o $@

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

test-unit: $(TEST_LOG) $(TEST_NODE) $(TEST_CONFIG) $(TEST_WIRE) $(TEST_TIMER) $(TEST_VOTE) $(TEST_ELECTION) $(TEST_APPEND) $(TEST_RECONCILE) $(TEST_REPLICATION) $(TEST_PROGRESS) $(TEST_COMMIT)
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

test: test-smoke test-unit
	sh tests/test_config_cli.sh ./$(APP)
	sh tests/test_timer_cli.sh ./$(APP)
	sh tests/test_log_demo.sh ./$(APP)
	$(MAKE) test-network

clean:
	rm -rf build
