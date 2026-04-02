CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g
CPPFLAGS = -Iinclude
LDFLAGS =
LDLIBS =

APP = build/raft-engine
TEST_LOG = build/test_raft_log
TEST_NODE = build/test_raft_node
LIB_SOURCES = src/raft_log.c src/raft_node.c
PUBLIC_HEADERS = include/raft_types.h include/raft_log.h include/raft_node.h

.PHONY: all run demo test test-smoke test-unit clean

all: $(APP)

$(APP): src/main.c $(LIB_SOURCES) $(PUBLIC_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c $(LIB_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_LOG): tests/test_raft_log.c src/raft_log.c $(PUBLIC_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_raft_log.c src/raft_log.c $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_NODE): tests/test_raft_node.c $(LIB_SOURCES) $(PUBLIC_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_raft_node.c $(LIB_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build:
	mkdir -p build

run: $(APP)
	./$(APP)

demo: $(APP)
	./$(APP) --demo

test: test-smoke test-unit

test-smoke: $(APP)
	sh tests/test_smoke.sh ./$(APP)

test-unit: $(TEST_LOG) $(TEST_NODE)
	./$(TEST_LOG)
	./$(TEST_NODE)

clean:
	rm -rf build
