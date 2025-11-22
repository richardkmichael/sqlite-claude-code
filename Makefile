CC ?= clang

# SQLite flags: allow environment override for cross-platform builds
# E.g., on Windows: set SQLITE_CFLAGS=-I/path/to/sqlite and SQLITE_LIBS=-L/path -lsqlite3
ifndef SQLITE_CFLAGS
  BREW_SQLITE := $(shell brew --prefix sqlite3 2>/dev/null)
  ifneq ($(BREW_SQLITE),)
    SQLITE_CFLAGS := $(shell PKG_CONFIG_PATH=$(BREW_SQLITE)/lib/pkgconfig pkg-config --cflags sqlite3)
    SQLITE_LIBS := $(shell PKG_CONFIG_PATH=$(BREW_SQLITE)/lib/pkgconfig pkg-config --libs sqlite3)
  else
    SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
    SQLITE_LIBS := $(shell pkg-config --libs sqlite3 2>/dev/null)
  endif
  # Convert -I to -isystem to suppress warnings from SQLite headers
  SQLITE_CFLAGS := $(subst -I,-isystem ,$(SQLITE_CFLAGS))
endif

CFLAGS = -Wall -Wextra -Werror -fPIC $(SQLITE_CFLAGS) -isystem src/vendor
LDFLAGS = $(SQLITE_LIBS) -dynamiclib

# Our source files (for formatting/linting)
OUR_SOURCES = src/init.c src/common.c src/projects.c src/sessions.c src/messages.c
OUR_HEADERS = src/common.h

# All sources including vendored code
SOURCES = $(OUR_SOURCES) src/vendor/cJSON.c
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
	@mkdir -p $(dir $@)
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
	@mkdir -p $(dir $@)
	$(CC) $(ASAN_CFLAGS) -c -o $@ $<

# cJSON is third-party code - suppress deprecation warnings for sprintf
build/asan/vendor/cJSON.o: src/vendor/cJSON.c | build/asan
	@mkdir -p $(dir $@)
	$(CC) $(ASAN_CFLAGS) -Wno-deprecated-declarations -c -o $@ $<

build/asan:
	mkdir -p build/asan

# ASan test harness (avoids macOS SIP issues with DYLD_INSERT_LIBRARIES)
$(ASAN_HARNESS): test/asan_harness.c | build/asan
	$(CC) $(ASAN_CFLAGS) $(SQLITE_LIBS) -fsanitize=address -o $@ $<

# Run just ASan tests (builds ASan binaries first)
check: $(ASAN_TARGET) $(ASAN_HARNESS)
	@CLAUDE_PROJECTS_DIR=./test-projects $(ASAN_HARNESS) $(ASAN_TARGET)

# Code quality tools: allow environment override, prefer Homebrew LLVM, fall back to PATH
ifndef CLANG_FORMAT
  BREW_LLVM := $(shell brew --prefix llvm 2>/dev/null)
  ifneq ($(BREW_LLVM),)
    CLANG_FORMAT := $(BREW_LLVM)/bin/clang-format
    CLANG_TIDY := $(BREW_LLVM)/bin/clang-tidy
  else
    CLANG_FORMAT := clang-format
    CLANG_TIDY := clang-tidy
  endif
endif

format:
	$(CLANG_FORMAT) -i $(OUR_SOURCES) $(OUR_HEADERS)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(OUR_SOURCES) $(OUR_HEADERS)

lint:
	$(CLANG_TIDY) $(OUR_SOURCES) -- $(CFLAGS)

.PHONY: all clean test check format format-check lint
