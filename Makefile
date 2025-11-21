CC = clang
CFLAGS = -Wall -Wextra -Werror -fPIC -I/opt/homebrew/Cellar/sqlite/3.51.0/include
LDFLAGS = -L/opt/homebrew/Cellar/sqlite/3.51.0/lib -lsqlite3 -dynamiclib

SOURCES = src/common.c src/projects.c src/sessions.c src/cJSON.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = build/claude_code.dylib

all: $(TARGET)

$(TARGET): $(OBJECTS)
	mkdir -p build
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

test: $(TARGET)
	./test/run_tests.sh

.PHONY: all clean test
