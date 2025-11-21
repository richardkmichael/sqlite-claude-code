CC = clang
CFLAGS = -Wall -Wextra -Werror -fPIC -I/opt/homebrew/Cellar/sqlite/3.51.0/include
LDFLAGS = -L/opt/homebrew/Cellar/sqlite/3.51.0/lib -lsqlite3 -dynamiclib

SOURCES = src/init.c src/common.c src/projects.c src/sessions.c src/messages.c src/functions.c src/cJSON.c
OBJECTS = $(patsubst src/%.c,build/%.o,$(SOURCES))
TARGET = build/claude_code.dylib

# ASan build
ASAN_CFLAGS = $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer
ASAN_LDFLAGS = $(LDFLAGS) -fsanitize=address
ASAN_OBJECTS = $(patsubst src/%.c,build/asan/%.o,$(SOURCES))
ASAN_TARGET = build/asan/claude_code.dylib
ASAN_HARNESS = build/asan/test_harness

# Default: build main extension only
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build

# Build everything and run all tests (regular + ASan)
test: $(TARGET) $(ASAN_TARGET) $(ASAN_HARNESS)
	./test/run_tests.sh

# ASan build rules
$(ASAN_TARGET): $(ASAN_OBJECTS)
	$(CC) $(ASAN_LDFLAGS) -o $@ $^

build/asan/%.o: src/%.c | build/asan
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

# cJSON is third-party code - suppress deprecation warnings for sprintf
build/asan/cJSON.o: src/cJSON.c | build/asan
	$(CC) $(ASAN_CFLAGS) -Wno-deprecated-declarations -c -o $@ $<

build/asan:
	mkdir -p build/asan

# ASan test harness (avoids macOS SIP issues with DYLD_INSERT_LIBRARIES)
$(ASAN_HARNESS): test/asan_harness.c | build/asan
	$(CC) $(ASAN_CFLAGS) -L/opt/homebrew/Cellar/sqlite/3.51.0/lib -lsqlite3 -fsanitize=address -o $@ $<

# Run just ASan tests (builds ASan binaries first)
check: $(ASAN_TARGET) $(ASAN_HARNESS)
	@CLAUDE_PROJECTS_DIR=./test-projects $(ASAN_HARNESS) $(ASAN_TARGET)

.PHONY: all clean test check
