CC = clang
CFLAGS = -Wall -Wextra -Werror -fPIC -I/opt/homebrew/Cellar/sqlite/3.51.0/include
LDFLAGS = -L/opt/homebrew/Cellar/sqlite/3.51.0/lib -lsqlite3 -dynamiclib

SOURCES = src/init.c src/common.c src/projects.c src/sessions.c src/messages.c src/functions.c src/cJSON.c
OBJECTS = $(patsubst src/%.c,build/%.o,$(SOURCES))
TARGET = build/claude_code.dylib

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build

test: $(TARGET)
	./test/run_tests.sh

.PHONY: all clean test
