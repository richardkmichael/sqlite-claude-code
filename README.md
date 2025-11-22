# Get started

Install sqlite and a compiler, required for build.

1. Clone
2. Build: `make`
3. Query: `sqlite3 -cmd '.load build/claude_code' :memory: 'SELECT COUNT(*) FROM projects'`

## Complete example

Get the last message in a specific session:

```bash
# Claude Code: /status
$ SESSION_ID='aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'

# Get the last message in the session
$ SQL="SELECT json_data FROM messages WHERE session_id = '$SESSION_ID' ORDER BY timestamp DESC LIMIT 1"

$ sqlite3 -cmd '.load build/claude_code' :memory: "$SQL" | jq -S
{
  "content": "<command-name>/status</command-name>...",
  "cwd": "/path/to/project",
  "gitBranch": "main",
  "isMeta": false,
  "isSidechain": false,
  "level": "info",
  "parentUuid": "00000000-0000-0000-0000-000000000001",
  "sessionId": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
  "subtype": "local_command",
  "timestamp": "2025-01-01T12:00:00.000Z",
  "type": "system",
  "userType": "external",
  "uuid": "00000000-0000-0000-0000-000000000002",
  "version": "2.0.0"
}
```

# Claude Code SQLite extension

A SQLite extension providing read-only tables of Claude Code history from `~/.claude/projects`.

## Tables

- `projects` - Project directories in your Claude Code workspace
- `sessions` - Conversation sessions (JSONL files) within projects
- `messages` - Individual messages parsed from session files

## Building

### Requirements

- C compiler (clang or gcc)
- SQLite 3 development headers
- pkg-config

### macOS (Homebrew)

```bash
brew install sqlite3 llvm
make
```

### Linux

```bash
# Debian/Ubuntu
sudo apt install libsqlite3-dev pkg-config
make
```

### Windows

Set environment variables before building:

```cmd
set CC=cl
set SQLITE_CFLAGS=-I C:\path\to\sqlite\include
set SQLITE_LIBS=-L C:\path\to\sqlite\lib -lsqlite3
make
```

## Usage

Load the extension in SQLite:

```sql
.load build/claude_code.dylib
```

The extension auto-creates tables using `~/.claude/projects/` as the default directory.

### Examples

```sql
-- List all projects
SELECT  FROM projects ORDER BY updated_at DESC;

-- Count messages per session
SELECT session_id, COUNT() as msg_count
FROM messages
GROUP BY session_id;

-- Find sessions with assistant responses
SELECT DISTINCT session_id, project_id
FROM messages
WHERE type = 'assistant';

-- Extract data from json_data using SQLite JSON functions
SELECT json_extract(json_data, '$.message.model') as model
FROM messages
WHERE type = 'assistant' LIMIT 5;
```

### Custom directory

Set `CLAUDE_PROJECTS_DIR` environment variable, helpful for development and testing.

```bash
export CLAUDE_PROJECTS_DIR=/path/to/projects
sqlite3 -cmd '.load build/claude_code' :memory: 'SELECT  FROM projects'
```

## Testing

```bash
make test      # Run all tests (functional + ASan memory checks)
make check     # Run just ASan memory tests
make lint      # Static analysis with clang-tidy
make format    # Auto-format code
```

## Debugging

### macOS

If sqlite segfaults, use `Console > Crash Reports`; `Reveal in Finder`, likely
`~/Library/Logs/DiagnosticReports/`.

### AddressSanitizer

Run with ASan to detect memory errors:

```bash
# Run ASan test suite
make check

# Or use ASan harness directly
CLAUDE_PROJECTS_DIR=./test-projects ./build/asan/test_harness ./build/asan/claude_code.dylib
```

### Debug Build

```bash
# Run under lldb
lldb -- sqlite3 -cmd '.load build/claude_code'
(lldb) run
```
