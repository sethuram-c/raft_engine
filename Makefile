CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g
CPPFLAGS = -Iinclude
LDFLAGS =
LDLIBS =

TARGET = build/raft-engine
SOURCES = src/main.c

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(SOURCES) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build:
	mkdir -p build

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	sh tests/test_smoke.sh ./$(TARGET)

clean:
	rm -rf build
