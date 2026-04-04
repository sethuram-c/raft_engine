CC = gcc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g
LDFLAGS ?=
LDLIBS ?=
PYTHON ?= python3

APP = build/raft-engine
LIB_SOURCES = src/raft_log.c src/raft_node.c src/raft_config.c src/raft_tcp.c
HEADERS = $(wildcard include/*.h)
TEST_LOG = build/test_raft_log
TEST_NODE = build/test_raft_node
TEST_CONFIG = build/test_raft_config

.PHONY: all run demo check-config test test-smoke test-unit test-network clean
all: $(APP)

$(APP): src/main.c $(LIB_SOURCES) $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c $(LIB_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_LOG): tests/test_raft_log.c src/raft_log.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_raft_log.c src/raft_log.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_NODE): tests/test_raft_node.c src/raft_node.c src/raft_log.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_raft_node.c src/raft_node.c src/raft_log.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_CONFIG): tests/test_raft_config.c src/raft_config.c $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_raft_config.c src/raft_config.c $(LDFLAGS) $(LDLIBS) -o $@

build:
	mkdir -p build

run: $(APP)
	./$(APP)

demo: $(APP)
	./$(APP) --demo

check-config: $(APP)
	./$(APP) --check-config config/node1.conf
	./$(APP) --check-config config/node2.conf
	./$(APP) --check-config config/node3.conf

test-smoke: $(APP)
	sh tests/test_smoke.sh ./$(APP)

test-unit: $(TEST_LOG) $(TEST_NODE) $(TEST_CONFIG)
	./$(TEST_LOG)
	./$(TEST_NODE)
	./$(TEST_CONFIG)

test-network: $(APP)
	$(PYTHON) tests/test_tcp_integration.py ./$(APP)

test: test-smoke test-unit
	sh tests/test_config_cli.sh ./$(APP)
	$(MAKE) test-network

clean:
	rm -rf build
