# JSONL Virtual Table Extension - Implementation Plan

## Overview

This document outlines the phased implementation plan for building a SQLite virtual table extension that presents Claude Code chat session JSONL files as queryable SQL tables.

**Timeline**: Multi-session development (exact duration TBD)
**Target**: Working extension with test coverage
**Success**: Can query real Claude Code projects via SQL

## Development Environment

### Required Tools
- **Compiler**: clang (via Xcode Command Line Tools)
- **Build system**: gmake (GNU Make)
- **SQLite**: Homebrew installation at `/opt/homebrew/Cellar/sqlite/3.51.0`
- **JSON library**: cJSON (vendored in project)
- **Version control**: git

### Compiler Flags
- `-Wall -Wextra`: Standard warnings (Phase 1)
- `clang-format`: Code formatting (Phase 6+)
- `clang-tidy`: Static analysis (Phase 6+)

### Directory Structure
```
development/
├── SPECIFICATION.md         # This spec document
├── IMPLEMENTATION_PLAN.md   # This implementation plan
├── Makefile                 # Build system
├── src/
│   ├── jsonl_projects.c     # Main extension implementation
│   ├── jsonl_projects.h     # Header file
│   ├── cJSON.c              # Vendored JSON library
│   └── cJSON.h              # JSON library header
├── test/
│   ├── basic.test           # SQLite test format
│   ├── integration.test     # End-to-end tests
│   └── run_tests.sh         # Test runner
├── test-projects/           # Sample data for testing
│   ├── claude-code-logger/  # Test project 1
│   └── test-project/        # Test project 2
├── sqlite-src-3510000/      # SQLite source (reference)
│   └── ext/misc/csv.c       # Reference implementation
├── build/                   # Build artifacts (gitignored)
└── README.md                # User documentation
```

## Phase 1: Project Scaffolding and Build System

### Goals
- Set up project structure
- Create working Makefile
- Verify SQLite headers/libraries accessible
- Vendor cJSON library
- Create minimal "hello world" extension

### Tasks

#### 1.1 Create Directory Structure
```bash
mkdir -p src test build
```

#### 1.2 Vendor cJSON
Download cJSON.c and cJSON.h from https://github.com/DaveGamble/cJSON into `src/`

#### 1.3 Create Makefile
```makefile
CC = clang
CFLAGS = -Wall -Wextra -fPIC -I/opt/homebrew/Cellar/sqlite/3.51.0/include
LDFLAGS = -L/opt/homebrew/Cellar/sqlite/3.51.0/lib -lsqlite3 -dynamiclib

SOURCES = src/jsonl_projects.c src/cJSON.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = build/jsonl_projects.dylib

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

test: $(TARGET)
	./test/run_tests.sh

.PHONY: all clean test
```

#### 1.4 Create Minimal Extension (src/jsonl_projects.c)
```c
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_jsonlprojects_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
) {
  SQLITE_EXTENSION_INIT2(pApi);
  // TODO: Register virtual tables
  return SQLITE_OK;
}
```

#### 1.5 Verify Build
```bash
gmake clean
gmake
sqlite3 :memory: ".load build/jsonl_projects.dylib"
```

### Success Criteria
- ✅ Makefile builds extension without errors
- ✅ Extension loads in sqlite3 without errors
- ✅ No warnings with -Wall -Wextra

### Deliverables
- Working Makefile
- Minimal extension that loads successfully
- cJSON vendored and compiling

---

## Phase 2: Implement `projects` Virtual Table

### Goals
- Implement first virtual table (simplest one)
- Directory scanning logic
- File metadata extraction (ctime, mtime)
- Read-only enforcement

### Tasks

#### 2.1 Define Virtual Table Module
Implement `sqlite3_module` structure for `projects`:
- `xCreate` / `xConnect`: Parse base_directory parameter
- `xBestIndex`: No-op (full table scan)
- `xDisconnect` / `xDestroy`: Cleanup
- `xOpen`: Create cursor
- `xClose`: Destroy cursor
- `xFilter`: Scan base_directory for subdirectories
- `xNext`: Advance to next project directory
- `xEof`: Check if scan complete
- `xColumn`: Return project_id, directory, created_at, updated_at
- `xRowid`: Return unique rowid

#### 2.2 Directory Scanning
Use `opendir()` / `readdir()` / `closedir()` from `<dirent.h>` to:
- List subdirectories under base_directory
- Skip `.` and `..`
- Get directory metadata via `stat()`

#### 2.3 Cursor Structure
```c
typedef struct ProjectsCursor {
  sqlite3_vtab_cursor base;
  DIR *dir_handle;           // Directory handle
  struct dirent *entry;      // Current entry
  char base_path[PATH_MAX];  // Base directory path
  int eof;                   // End of scan flag
  sqlite3_int64 rowid;       // Current row ID
} ProjectsCursor;
```

#### 2.4 Register Module
In `sqlite3_jsonlprojects_init()`:
```c
rc = sqlite3_create_module(db, "jsonl_projects", &projects_module, NULL);
```

### Testing
Create `test/projects.test`:
```sql
.load build/jsonl_projects.dylib

CREATE VIRTUAL TABLE p USING jsonl_projects(
  base_directory='./test-projects'
);

SELECT project_id FROM p;
-- Expected: claude-code-logger, test-project
```

### Success Criteria
- ✅ `projects` virtual table registers successfully
- ✅ Can list test-projects directories
- ✅ Metadata (created_at, updated_at) populated correctly
- ✅ Read-only: INSERT fails with SQLITE_READONLY

### Deliverables
- Working `projects` virtual table
- Test suite for projects table
- Documentation of API usage

---

## Phase 3: Implement `sessions` Virtual Table

### Goals
- Read JSONL files from project directories
- Parse filenames (UUIDs)
- Count records per file
- Link to projects via foreign key

### Tasks

#### 3.1 Define Virtual Table Module
Similar to Phase 2, implement `sqlite3_module` for `sessions`:
- Parse parameters (base_directory, exclude_pattern)
- Scan project directories for `*.jsonl` files
- Exclude `agent-*.jsonl` by default

#### 3.2 File Scanning
For each project directory:
- Use `glob()` or `readdir()` to find `*.jsonl` files
- Filter by exclusion pattern
- Get file metadata (`stat()`)

#### 3.3 Count Records
Efficiently count lines in JSONL file:
```c
int count_jsonl_records(const char *filepath) {
  FILE *fp = fopen(filepath, "r");
  int count = 0;
  char buffer[8192];
  while (fgets(buffer, sizeof(buffer), fp)) {
    count++;
  }
  fclose(fp);
  return count;
}
```

#### 3.4 Cursor Structure
```c
typedef struct SessionsCursor {
  sqlite3_vtab_cursor base;
  char base_path[PATH_MAX];
  char **project_list;       // Array of project IDs
  int project_count;
  int current_project_idx;
  char **session_list;       // Array of session UUIDs in current project
  int session_count;
  int current_session_idx;
  sqlite3_int64 rowid;
  int eof;
} SessionsCursor;
```

### Testing
Create `test/sessions.test`:
```sql
.load build/jsonl_projects.dylib

CREATE VIRTUAL TABLE s USING jsonl_sessions(
  base_directory='./test-projects'
);

SELECT session_id, project_id, record_count FROM s;
-- Expected: UUIDs from test-projects with correct counts
```

### Success Criteria
- ✅ `sessions` virtual table lists all JSONL files
- ✅ Excludes `agent-*.jsonl` files
- ✅ `record_count` matches actual line count
- ✅ Foreign key to projects works correctly

### Deliverables
- Working `sessions` virtual table
- Test suite for sessions table
- File scanning and counting logic

---

## Phase 4: Implement `messages` Virtual Table

### Goals
- Parse JSONL files line-by-line
- Parse JSON records with cJSON
- Extract stable fields to columns
- Store full JSON in `json_data` column

### Tasks

#### 4.1 Define Virtual Table Module
Implement `sqlite3_module` for `messages`:
- Scan all JSONL files (nested iteration: projects → sessions → lines)
- Parse each line as JSON
- Extract fields: type, timestamp, uuid, parentUuid, userType, subtype

#### 4.2 JSON Parsing with cJSON
```c
#include "cJSON.h"

void parse_message(const char *json_line, MessageRecord *record) {
  cJSON *root = cJSON_Parse(json_line);
  if (!root) {
    // Handle parse error
    return;
  }

  // Extract fields
  cJSON *type = cJSON_GetObjectItem(root, "type");
  if (type) {
    strncpy(record->type, type->valuestring, sizeof(record->type));
  }

  cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
  if (timestamp) {
    strncpy(record->timestamp, timestamp->valuestring, sizeof(record->timestamp));
  }

  // ... extract other fields

  // Store full JSON
  record->json_data = strdup(json_line);

  cJSON_Delete(root);
}
```

#### 4.3 Cursor Structure
```c
typedef struct MessagesCursor {
  sqlite3_vtab_cursor base;
  FILE *current_file;
  char current_session_id[64];
  char line_buffer[1024*1024];  // 1MB buffer for large JSON
  MessageRecord current_record;
  sqlite3_int64 rowid;
  int eof;
  // ... project/session iteration state
} MessagesCursor;
```

#### 4.4 Handle Large Files
Stream reading (don't load entire file):
```c
// In xNext()
while (fgets(cursor->line_buffer, sizeof(cursor->line_buffer), cursor->current_file)) {
  parse_message(cursor->line_buffer, &cursor->current_record);
  return SQLITE_OK;
}
// EOF reached, move to next file
```

#### 4.5 Error Handling
- Malformed JSON: Log warning, skip record, continue
- Missing files: Return error in xFilter
- Partial reads: Handle incomplete lines gracefully

### Testing
Create `test/messages.test`:
```sql
.load build/jsonl_projects.dylib

CREATE VIRTUAL TABLE m USING jsonl_messages(
  base_directory='./test-projects'
);

SELECT type, COUNT(*) FROM m GROUP BY type;
-- Expected: user: N, assistant: M, system: K, etc.

SELECT json_extract(json_data, '$.message.role')
FROM m
WHERE type = 'user'
LIMIT 5;
-- Expected: "user"
```

### Success Criteria
- ✅ `messages` virtual table parses all JSONL records
- ✅ Extracted fields match source JSON
- ✅ `json_data` contains full JSON text
- ✅ Large files (>100MB) handled without crash
- ✅ Malformed JSON handled gracefully

### Deliverables
- Working `messages` virtual table
- JSON parsing with cJSON
- Error handling for malformed data
- Test suite for messages table

---

## Phase 5: SQL Helper Functions

### Goals
- Register custom SQL functions
- Implement common JSON extractions
- Create documentation/metadata table

### Tasks

#### 5.1 Implement Helper Functions
```c
// get_message_content(json_text) -> TEXT
static void get_message_content_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  const char *json = (const char*)sqlite3_value_text(argv[0]);
  cJSON *root = cJSON_Parse(json);

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (message) {
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content) {
      if (cJSON_IsString(content)) {
        sqlite3_result_text(ctx, content->valuestring, -1, SQLITE_TRANSIENT);
      } else if (cJSON_IsArray(content)) {
        // Extract text from array elements
        // ...
      }
    }
  }

  cJSON_Delete(root);
}
```

#### 5.2 Register Functions
In `sqlite3_jsonlprojects_init()`:
```c
sqlite3_create_function(db, "get_message_content", 1, SQLITE_UTF8,
                        NULL, get_message_content_func, NULL, NULL);
sqlite3_create_function(db, "get_message_role", 1, SQLITE_UTF8,
                        NULL, get_message_role_func, NULL, NULL);
sqlite3_create_function(db, "get_message_model", 1, SQLITE_UTF8,
                        NULL, get_message_model_func, NULL, NULL);
// ... other functions
```

#### 5.3 Create Metadata Table
```c
// Create _jsonl_functions table documenting available functions
const char *create_meta =
  "CREATE TABLE IF NOT EXISTS _jsonl_functions ("
  "  name TEXT PRIMARY KEY,"
  "  signature TEXT,"
  "  description TEXT,"
  "  example TEXT"
  ")";
sqlite3_exec(db, create_meta, NULL, NULL, NULL);

// Insert function documentation
const char *insert_docs =
  "INSERT OR REPLACE INTO _jsonl_functions VALUES"
  "('get_message_content', 'get_message_content(json_text)', "
  " 'Extract message.content, handling array/text formats', "
  " 'SELECT get_message_content(json_data) FROM messages WHERE type=\"user\"')";
sqlite3_exec(db, insert_docs, NULL, NULL, NULL);
```

### Testing
Create `test/functions.test`:
```sql
.load build/jsonl_projects.dylib

CREATE VIRTUAL TABLE m USING jsonl_messages(base_directory='./test-projects');

SELECT get_message_role(json_data) FROM m WHERE type = 'user' LIMIT 1;
-- Expected: "user"

SELECT name, signature FROM _jsonl_functions;
-- Expected: List of all functions
```

### Success Criteria
- ✅ All helper functions registered
- ✅ Functions return correct values
- ✅ Metadata table documents functions
- ✅ Functions handle NULL/invalid input gracefully

### Deliverables
- 5+ helper functions implemented
- Metadata table with documentation
- Test suite for functions

---

## Phase 6: Testing and Refinement

### Goals
- Comprehensive test coverage
- Test against real data (carefully)
- Performance profiling
- Documentation
- Code cleanup (clang-format, clang-tidy)

### Tasks

#### 6.1 Integration Tests
Create end-to-end tests:
```sql
-- test/integration.test
.load build/jsonl_projects.dylib

CREATE VIRTUAL TABLE chat USING jsonl_projects(
  base_directory='./test-projects'
);

-- Test: Count messages per project
SELECT p.project_id, COUNT(*) as msg_count
FROM chat_projects p
JOIN chat_sessions s ON p.project_id = s.project_id
JOIN chat_messages m ON s.session_id = m.session_id
GROUP BY p.project_id;

-- Test: Find assistant messages
SELECT m.type, COUNT(*)
FROM chat_messages m
WHERE m.type = 'assistant';

-- Test: JSON extraction
SELECT json_extract(m.json_data, '$.message.model')
FROM chat_messages m
WHERE m.type = 'assistant'
LIMIT 5;
```

#### 6.2 Real Data Testing (Read-Only)
Carefully test against real projects:
```bash
sqlite3 :memory: << EOF
.load build/jsonl_projects.dylib
CREATE VIRTUAL TABLE real USING jsonl_projects(
  base_directory='/Users/rmichael/.claude/projects'
);
SELECT COUNT(*) FROM real_messages;
.quit
EOF
```

#### 6.3 Performance Testing
```sql
-- Measure query performance
.timer on
SELECT type, COUNT(*) FROM messages GROUP BY type;
.timer off
```

#### 6.4 Code Quality
```bash
# Format code
clang-format -i src/*.c src/*.h

# Static analysis
clang-tidy src/jsonl_projects.c -- -I/opt/homebrew/Cellar/sqlite/3.51.0/include
```

#### 6.5 Documentation
Create `README.md`:
- Installation instructions
- Usage examples
- API reference
- Troubleshooting

### Success Criteria
- ✅ All tests pass
- ✅ Works with real Claude Code projects
- ✅ No memory leaks (valgrind on Linux, or leaks on macOS)
- ✅ Query performance acceptable (<5s for typical queries)
- ✅ Code formatted and passes static analysis
- ✅ Documentation complete

### Deliverables
- Comprehensive test suite
- Performance benchmarks
- User documentation (README.md)
- Clean, formatted code

---

## Milestones and Checkpoints

| Phase | Deliverable | Checkpoint |
|-------|-------------|------------|
| 1 | Build system + minimal extension | Extension loads in sqlite3 |
| 2 | `projects` virtual table | Can list test projects |
| 3 | `sessions` virtual table | Can list test sessions |
| 4 | `messages` virtual table | Can query test messages |
| 5 | SQL helper functions | Functions work correctly |
| 6 | Testing + documentation | Production-ready extension |

## Risks and Mitigations

### Risk: Large JSONL files cause memory issues
**Mitigation**: Stream parsing, don't load entire file into memory

### Risk: Malformed JSON crashes extension
**Mitigation**: Robust error handling, skip bad records, log warnings

### Risk: Performance too slow for interactive use
**Mitigation**: Profile and optimize, add caching, index hints

### Risk: Directory structure changes break extension
**Mitigation**: Parameterize patterns, version the extension

### Risk: Multi-session development loses context
**Mitigation**: Comprehensive documentation (this file + SPECIFICATION.md)

## Git Commit Strategy

Commit after each major task completion:
- Phase 1: "Add build system and minimal extension"
- Phase 2: "Implement projects virtual table"
- Phase 3: "Implement sessions virtual table"
- Phase 4: "Implement messages virtual table"
- Phase 5: "Add SQL helper functions"
- Phase 6: "Add tests and documentation"

Keep commits focused and atomic. Use descriptive messages.

## Next Steps

1. Review SPECIFICATION.md and IMPLEMENTATION_PLAN.md with team
2. Start Phase 1: Project scaffolding
3. Iterate through phases, committing work as we go
4. Test against real data carefully
5. Deploy and use for actual analysis

---

**Document Status**: Draft for team review
**Last Updated**: 2025-11-19
**Authors**: Team + Claude Code
