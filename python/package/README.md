# sqlite-claude-code

SQLite extension for querying Claude Code conversation history.

## Installation

```bash
pip install sqlite-claude-code
# or
uv pip install sqlite-claude-code
```

## Quick Start

```python
import sqlite_claude_code

# Easiest way: use connect() context manager (auto-closes)
with sqlite_claude_code.connect() as conn:
    cursor = conn.execute("SELECT COUNT(*) FROM sessions")
    count = cursor.fetchone()[0]
    print(f"Found {count} Claude Code sessions")
```

## Usage

### Automatic Connection (Recommended)

The `connect()` function returns a context manager that automatically closes the connection:

```python
import sqlite_claude_code

# With context manager (automatically closes)
with sqlite_claude_code.connect() as conn:
    cursor = conn.execute("SELECT * FROM projects LIMIT 5")
    for row in cursor:
        print(row)

    cursor = conn.execute("SELECT * FROM sessions LIMIT 5")
    for row in cursor:
        print(row)

    cursor = conn.execute("SELECT type, message_id FROM messages LIMIT 10")
    for row in cursor:
        print(row)

# Connection is automatically closed after the with block
```

### Loading Into Existing Connection

If you already have a database connection, use `load()`:

```python
import sqlite3
import sqlite_claude_code

# Create connection
conn = sqlite3.connect(':memory:')

# Enable and load extension
conn.enable_load_extension(True)
sqlite_claude_code.load(conn)
conn.enable_load_extension(False)

# Query virtual tables
cursor = conn.execute("SELECT * FROM projects LIMIT 5")
for row in cursor:
    print(row)

conn.close()
```

Note: Python's sqlite3 module must be compiled with extension loading support (`enable_load_extension` must be available).

## Virtual Tables

The extension provides three virtual tables:

- `projects` - Project directories in `~/.claude/projects/`
- `sessions` - JSONL session files within projects
- `messages` - Parsed messages from session JSONL files

## Development

### Building from Source

The package includes a C extension that is automatically compiled during build:

```bash
# Clone repository
git clone https://github.com/yourusername/sqlite-vfs-claude-code
cd sqlite-vfs-claude-code/development/python/package

# Build wheel (automatically compiles extension)
uv build

# Or install locally in editable mode (also compiles extension)
uv pip install -e .
```

The extension is automatically compiled by `setup.py` during the build process.

### Running Tests

```bash
uv run pytest tests/
```

## Build Requirements

- C compiler (clang on macOS, gcc on Linux)
- SQLite development headers (usually from Homebrew on macOS: `brew install sqlite3`)

## License

BSD-3-Clause
