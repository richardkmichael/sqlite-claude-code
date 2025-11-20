# JSONL Virtual Table Extension - Implementation Plan

## Overview

This document outlines the phased implementation plan for building a production-ready SQLite virtual table extension that presents Claude Code chat session JSONL files as queryable SQL tables.

Timeline: Multi-session development (exact duration TBD)
Target: Production-ready extension with zero-setup UX, thread safety, and performance optimization
Success: Can query real Claude Code projects via SQL in multi-threaded web server environments

## Development Environment

### Required Tools
- Compiler: clang (via Xcode Command Line Tools)
- Build system: gmake (GNU Make)
- SQLite: Homebrew installation at `/opt/homebrew/Cellar/sqlite/3.51.0`
- JSON library: cJSON (vendored in project)
- Version control: git

### Compiler Flags
- `-Wall -Wextra`: Standard warnings (all phases)
- `clang-format`: Code formatting (Phase 6)
- `clang-tidy`: Static analysis (Phase 6)

### Directory Structure
```
development/
├── SPECIFICATION.md         # Living specification document
├── IMPLEMENTATION_PLAN.md   # This living implementation plan
├── FEEDBACK.md              # Team feedback (incorporated into above docs)
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
│   ├── claude-code-logger/  # Test project 1 (6 sessions, all message types)
│   └── test-project/        # Test project 2 (1 session)
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
- Create minimal "hello world" extension that loads successfully

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
TARGET = build/claude_code.dylib

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
int sqlite3_claude_code_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
) {
  SQLITE_EXTENSION_INIT2(pApi);
  // TODO: Register virtual tables and functions
  return SQLITE_OK;
}
```

#### 1.5 Verify Build
```bash
gmake clean
gmake
sqlite3 :memory: ".load build/claude_code.dylib"
```

### Success Criteria
- Makefile builds extension without errors
- Extension loads in sqlite3 without errors
- No warnings with -Wall -Wextra

### Deliverables
- Working Makefile
- Minimal extension that loads successfully
- cJSON vendored and compiling
- Build directory in .gitignore

---

## Phase 2: Implement `projects` Virtual Table + Auto-Setup

### Goals
- Implement first virtual table (simplest one: projects)
- Add flexible key=value argument parser (foundation for future features)
- Implement zero-setup auto-table creation
- Directory scanning logic
- File metadata extraction (ctime, mtime)
- Basic xBestIndex (returns full scan cost)
- Read-only enforcement

This phase enables immediate experimentation: after Phase 2, users can `.load` the extension and immediately query projects.

### Tasks

#### 2.1 Flexible Key=Value Argument Parser

Implement generic parameter parsing in xConnect:

```c
typedef struct vtab_config {
  char base_directory[PATH_MAX];
  char exclude_pattern[256];
  char include_project[256];      // Future: for partitioning
  char include_projects[1024];    // Future: for partitioning
} vtab_config;

static int parse_kv_argument(const char *arg, vtab_config *config) {
  // Parse "key=value" format
  const char *eq = strchr(arg, '=');
  if (!eq) return -1;

  size_t key_len = eq - arg;
  const char *value = eq + 1;

  if (strncmp(arg, "base_directory", key_len) == 0) {
    strncpy(config->base_directory, value, sizeof(config->base_directory)-1);
  } else if (strncmp(arg, "exclude_pattern", key_len) == 0) {
    strncpy(config->exclude_pattern, value, sizeof(config->exclude_pattern)-1);
  }
  // Future parameters: include_project, include_projects, etc.

  return 0;
}
```

#### 2.2 Define Virtual Table Module for Projects

Implement `sqlite3_module` structure:
- `xCreate` / `xConnect`: Parse parameters using flexible parser
- `xBestIndex`: Return full scan cost (no optimization yet)
- `xDisconnect` / `xDestroy`: Cleanup
- `xOpen`: Create cursor
- `xClose`: Destroy cursor
- `xFilter`: Scan base_directory for subdirectories
- `xNext`: Advance to next project directory
- `xEof`: Check if scan complete
- `xColumn`: Return project_id, directory, created_at, updated_at
- `xRowid`: Return unique rowid

#### 2.3 Basic xBestIndex Implementation

```c
static int projects_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  // Phase 2: Simple implementation - always full scan
  // Phase 5: Add constraint detection for optimization

  pIdxInfo->idxNum = 1; // Plan #1: Full scan
  pIdxInfo->estimatedCost = 100.0; // Cheap - few directories
  pIdxInfo->estimatedRows = 50;

  return SQLITE_OK;
}
```

#### 2.4 Directory Scanning

Use `opendir()` / `readdir()` / `closedir()` from `<dirent.h>`:
```c
typedef struct ProjectsCursor {
  sqlite3_vtab_cursor base;
  DIR *dir_handle;
  struct dirent *entry;
  char base_path[PATH_MAX];
  struct stat st;
  int eof;
  sqlite3_int64 rowid;
} ProjectsCursor;

static int projects_filter(sqlite3_vtab_cursor *cur, int idxNum,
                           const char *idxStr, int argc, sqlite3_value **argv) {
  ProjectsCursor *pCur = (ProjectsCursor*)cur;

  pCur->dir_handle = opendir(pCur->base_path);
  if (!pCur->dir_handle) {
    return SQLITE_ERROR;
  }

  pCur->rowid = 0;
  return projects_next(cur);
}

static int projects_next(sqlite3_vtab_cursor *cur) {
  ProjectsCursor *pCur = (ProjectsCursor*)cur;

  while ((pCur->entry = readdir(pCur->dir_handle)) != NULL) {
    if (strcmp(pCur->entry->d_name, ".") == 0 ||
        strcmp(pCur->entry->d_name, "..") == 0) {
      continue;
    }

    // Get full path and stat
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s",
             pCur->base_path, pCur->entry->d_name);

    if (stat(full_path, &pCur->st) == 0 && S_ISDIR(pCur->st.st_mode)) {
      pCur->rowid++;
      return SQLITE_OK;
    }
  }

  pCur->eof = 1;
  return SQLITE_OK;
}
```

#### 2.5 Zero-Setup Auto-Table Creation

Implement automatic table creation on extension load:

```c
int sqlite3_claude_code_init(sqlite3 *db, char **pzErrMsg,
                               const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  int rc;

  // Register virtual table module
  rc = sqlite3_create_module(db, "jsonl_projects", &projects_module, NULL);
  if (rc != SQLITE_OK) return rc;

  // Auto-create the projects table (zero-setup UX)
  const char *default_dir = getenv("CLAUDE_PROJECTS_DIR");
  if (!default_dir) {
    default_dir = "~/.claude/projects"; // Expand ~ to home directory
  }

  char sql[1024];
  snprintf(sql, sizeof(sql),
           "CREATE VIRTUAL TABLE IF NOT EXISTS projects "
           "USING jsonl_projects(base_directory='%s')",
           default_dir);

  rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  return SQLITE_OK;
}
```

#### 2.6 Read-Only Enforcement

Do not implement `xUpdate` in module structure (or implement to return SQLITE_READONLY).

### Testing

Create `test/projects.test`:
```sql
.load build/claude_code.dylib

-- Test zero-setup: table already exists
SELECT COUNT(*) FROM projects;

-- Test manual table creation with custom path
CREATE VIRTUAL TABLE test_projects USING jsonl_projects(
  base_directory='./test-projects'
);

SELECT project_id FROM test_projects;
-- Expected: claude-code-logger, test-project

-- Test metadata columns
SELECT project_id, directory, created_at, updated_at FROM test_projects;

-- Test read-only enforcement
INSERT INTO test_projects VALUES ('test', '/path', NULL, NULL);
-- Expected: Error (SQLITE_READONLY)
```

### Success Criteria
- `projects` virtual table registers successfully
- Zero-setup: `projects` table auto-created on extension load
- Flexible key=value parser handles base_directory parameter
- Can list test-projects directories
- Metadata (created_at, updated_at) populated correctly from stat()
- xBestIndex returns reasonable full scan cost
- Read-only: INSERT fails with SQLITE_READONLY

### Deliverables
- Working `projects` virtual table
- Flexible argument parser (foundation for partitioning)
- Zero-setup auto-table creation
- Basic xBestIndex implementation
- Test suite for projects table

---

## Phase 3: Implement `sessions` Virtual Table

### Goals
- Read JSONL files from project directories
- Parse filenames (UUIDs)
- Count records per file efficiently
- Link to projects via foreign key
- Basic xBestIndex (full scan cost)

After this phase: Users can query both projects and sessions.

### Tasks

#### 3.1 Define Virtual Table Module

Similar to Phase 2:
- Use flexible key=value parser for parameters
- Scan project directories for `*.jsonl` files
- Exclude files matching exclusion pattern (default: `agent-*.jsonl`)
- Basic xBestIndex (no constraint optimization yet)

#### 3.2 File Scanning

For each project directory:
```c
typedef struct SessionsCursor {
  sqlite3_vtab_cursor base;
  char base_path[PATH_MAX];
  char **project_list;       // Array of project IDs
  int project_count;
  int current_project_idx;
  char **session_list;       // Array of session file paths
  int session_count;
  int current_session_idx;
  struct stat st;            // File metadata
  sqlite3_int64 rowid;
  int eof;
} SessionsCursor;
```

Use `glob()` or `readdir()` to find `*.jsonl` files and filter by pattern.

#### 3.3 Count Records Efficiently

```c
int count_jsonl_records(const char *filepath) {
  FILE *fp = fopen(filepath, "r");
  if (!fp) return -1;

  int count = 0;
  char buffer[8192];
  while (fgets(buffer, sizeof(buffer), fp)) {
    count++;
  }
  fclose(fp);
  return count;
}
```

#### 3.4 Basic xBestIndex for Sessions

```c
static int sessions_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  // Phase 3: Simple implementation - always full scan
  // Phase 5: Add project_id constraint optimization

  pIdxInfo->idxNum = 1;
  pIdxInfo->estimatedCost = 5000.0; // Moderate - scan all session files
  pIdxInfo->estimatedRows = 500;

  return SQLITE_OK;
}
```

#### 3.5 Auto-Table Creation

Add to init function:
```c
snprintf(sql, sizeof(sql),
         "CREATE VIRTUAL TABLE IF NOT EXISTS sessions "
         "USING jsonl_sessions(base_directory='%s')",
         default_dir);
sqlite3_exec(db, sql, NULL, NULL, NULL);
```

### Testing

Create `test/sessions.test`:
```sql
.load build/claude_code.dylib

SELECT session_id, project_id, record_count FROM sessions;
-- Expected: UUIDs from test-projects with correct counts

-- Test exclusion pattern
SELECT COUNT(*) FROM sessions WHERE session_id LIKE 'agent-%';
-- Expected: 0 (agent files excluded)

-- Test JOIN with projects
SELECT p.project_id, COUNT(s.session_id) as session_count
FROM projects p
LEFT JOIN sessions s ON p.project_id = s.project_id
GROUP BY p.project_id;
```

### Success Criteria
- `sessions` virtual table lists all non-agent JSONL files
- Exclusion pattern works correctly
- `record_count` matches actual line count
- Metadata (created_at, updated_at) from file stat()
- Foreign key relationship to projects works
- Auto-created on extension load (zero-setup)

### Deliverables
- Working `sessions` virtual table
- File scanning and counting logic
- Test suite for sessions table

---

## Phase 4: Implement `messages` Virtual Table

### Goals
- Parse JSONL files line-by-line
- Parse JSON records with cJSON
- Extract stable fields to columns
- Store full JSON in `json_data` column
- Handle large files (>100MB) without memory issues
- Basic xBestIndex (full scan cost)

After this phase: Full functionality - users can query all three tables and experiment with real data.

### Tasks

#### 4.1 Define Virtual Table Module

```c
typedef struct MessagesCursor {
  sqlite3_vtab_cursor base;
  // Nested iteration: projects → sessions → lines
  char base_path[PATH_MAX];
  char **project_list;
  int project_count;
  int current_project_idx;
  char **session_list;
  int session_count;
  int current_session_idx;
  FILE *current_file;
  char current_session_id[64];
  char line_buffer[1024*1024];  // 1MB buffer for large JSON
  // Parsed record
  char message_id[64];
  char session_id[64];
  char type[32];
  char timestamp[64];
  char parent_id[64];
  char user_type[32];
  char content_type[32];
  char *json_data;
  sqlite3_int64 rowid;
  int eof;
} MessagesCursor;
```

#### 4.2 JSON Parsing with cJSON

```c
#include "cJSON.h"

static int parse_message_record(MessagesCursor *pCur, const char *json_line) {
  cJSON *root = cJSON_Parse(json_line);
  if (!root) {
    // Malformed JSON - log warning and skip
    fprintf(stderr, "Warning: malformed JSON in session %s\n", pCur->current_session_id);
    return -1;
  }

  // Extract fields
  cJSON *item;

  item = cJSON_GetObjectItem(root, "type");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->type, item->valuestring, sizeof(pCur->type)-1);
  }

  item = cJSON_GetObjectItem(root, "timestamp");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->timestamp, item->valuestring, sizeof(pCur->timestamp)-1);
  }

  item = cJSON_GetObjectItem(root, "uuid");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->message_id, item->valuestring, sizeof(pCur->message_id)-1);
  }

  item = cJSON_GetObjectItem(root, "parentUuid");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->parent_id, item->valuestring, sizeof(pCur->parent_id)-1);
  } else {
    pCur->parent_id[0] = '\0';
  }

  item = cJSON_GetObjectItem(root, "userType");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->user_type, item->valuestring, sizeof(pCur->user_type)-1);
  }

  item = cJSON_GetObjectItem(root, "subtype");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->content_type, item->valuestring, sizeof(pCur->content_type)-1);
  }

  // Store full JSON
  if (pCur->json_data) free(pCur->json_data);
  pCur->json_data = strdup(json_line);

  // Set session_id from current file
  strncpy(pCur->session_id, pCur->current_session_id, sizeof(pCur->session_id)-1);

  cJSON_Delete(root);
  return 0;
}
```

#### 4.3 Stream Reading for Large Files

```c
static int messages_next(sqlite3_vtab_cursor *cur) {
  MessagesCursor *pCur = (MessagesCursor*)cur;

  while (1) {
    // Try to read next line from current file
    if (pCur->current_file) {
      if (fgets(pCur->line_buffer, sizeof(pCur->line_buffer), pCur->current_file)) {
        if (parse_message_record(pCur, pCur->line_buffer) == 0) {
          pCur->rowid++;
          return SQLITE_OK;
        }
        // Parse failed, try next line
        continue;
      }

      // EOF on current file, close and move to next
      fclose(pCur->current_file);
      pCur->current_file = NULL;
      pCur->current_session_idx++;
    }

    // Open next session file
    if (pCur->current_session_idx < pCur->session_count) {
      const char *filepath = pCur->session_list[pCur->current_session_idx];
      pCur->current_file = fopen(filepath, "r");
      if (!pCur->current_file) {
        pCur->current_session_idx++;
        continue;
      }
      // Extract session_id from filename
      extract_session_id_from_path(filepath, pCur->current_session_id);
      continue;
    }

    // No more sessions in current project, move to next project
    pCur->current_project_idx++;
    if (pCur->current_project_idx < pCur->project_count) {
      // Load sessions for next project
      load_sessions_for_project(pCur, pCur->current_project_idx);
      pCur->current_session_idx = 0;
      continue;
    }

    // No more projects - EOF
    pCur->eof = 1;
    return SQLITE_OK;
  }
}
```

#### 4.4 Basic xBestIndex for Messages

```c
static int messages_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  // Phase 4: Simple implementation - always full scan
  // Phase 5: Add session_id and project_id constraint optimization

  pIdxInfo->idxNum = 1; // Plan #1: Full scan
  pIdxInfo->estimatedCost = 10000000.0; // High - many records
  pIdxInfo->estimatedRows = 100000;

  return SQLITE_OK;
}
```

#### 4.5 Error Handling

- Malformed JSON: Log warning, skip record, continue
- Missing files: Log error in xFilter, return SQLITE_ERROR
- Partial reads: Handle lines longer than buffer size gracefully

#### 4.6 Auto-Table Creation

Add to init function:
```c
snprintf(sql, sizeof(sql),
         "CREATE VIRTUAL TABLE IF NOT EXISTS messages "
         "USING jsonl_messages(base_directory='%s')",
         default_dir);
sqlite3_exec(db, sql, NULL, NULL, NULL);
```

### Testing

Create `test/messages.test`:
```sql
.load build/claude_code.dylib

-- Test basic query
SELECT type, COUNT(*) FROM messages GROUP BY type;
-- Expected: user: N, assistant: M, system: K, etc.

-- Test extracted fields
SELECT message_id, session_id, type, timestamp
FROM messages
WHERE type = 'user'
LIMIT 5;

-- Test JSON data column
SELECT json_extract(json_data, '$.message.role')
FROM messages
WHERE type = 'user'
LIMIT 5;
-- Expected: "user"

-- Test foreign key relationships
SELECT p.project_id, s.session_id, m.type, COUNT(*) as count
FROM projects p
JOIN sessions s ON p.project_id = s.project_id
JOIN messages m ON s.session_id = m.session_id
GROUP BY p.project_id, s.session_id, m.type
ORDER BY count DESC
LIMIT 10;
```

### Success Criteria
- `messages` virtual table parses all JSONL records from test data
- Extracted fields match source JSON
- `json_data` contains full JSON text
- Large files handled without memory issues
- Malformed JSON handled gracefully (logged, skipped)
- All message types present in results
- Foreign key relationships work correctly
- Auto-created on extension load (zero-setup)

### Deliverables
- Working `messages` virtual table
- JSON parsing with cJSON
- Stream reading for large files
- Error handling for malformed data
- Test suite for messages table

---

## Phase 5: Performance Optimization (xBestIndex) + SQL Helper Functions

### Goals
- Implement sophisticated xBestIndex with constraint detection
- Optimize queries filtering by session_id or project_id
- Register custom SQL helper functions
- Create metadata table documenting functions
- No JSON caching (per team decision)

After this phase: Queries are fast when properly filtered.

### Tasks

#### 5.1 Sophisticated xBestIndex for Messages

```c
#define PLAN_FULL_SCAN       1
#define PLAN_SESSION_FILTER  2
#define PLAN_PROJECT_FILTER  3

#define COL_MESSAGE_ID   0
#define COL_SESSION_ID   1
#define COL_TYPE         2
// ... define column indices

static int messages_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  int session_idx = -1, project_idx = -1;

  // Scan constraints from WHERE clause
  for (int i = 0; i < pIdxInfo->nConstraint; i++) {
    if (!pIdxInfo->aConstraint[i].usable) continue;

    // Look for equality constraints
    if (pIdxInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ) {
      int col = pIdxInfo->aConstraint[i].iColumn;

      if (col == COL_SESSION_ID) {
        session_idx = i;
      } else if (col == COL_PROJECT_ID) {
        project_idx = i;
      }
    }
  }

  // Choose best plan based on available constraints
  if (session_idx != -1) {
    // Best plan: session_id = ? → Read one JSONL file
    pIdxInfo->idxNum = PLAN_SESSION_FILTER;
    pIdxInfo->aConstraintUsage[session_idx].argvIndex = 1;
    pIdxInfo->aConstraintUsage[session_idx].omit = 1;
    pIdxInfo->estimatedCost = 5100.0; // Find file (~100) + read file (~5000)
    pIdxInfo->estimatedRows = 1000;
  } else if (project_idx != -1) {
    // Good plan: project_id = ? → Read one project's files
    pIdxInfo->idxNum = PLAN_PROJECT_FILTER;
    pIdxInfo->aConstraintUsage[project_idx].argvIndex = 1;
    pIdxInfo->aConstraintUsage[project_idx].omit = 1;
    pIdxInfo->estimatedCost = 500000.0; // Read project's files
    pIdxInfo->estimatedRows = 10000;
  } else {
    // Worst plan: no constraints → Full scan of all files
    pIdxInfo->idxNum = PLAN_FULL_SCAN;
    pIdxInfo->estimatedCost = 10000000.0;
    pIdxInfo->estimatedRows = 1000000;
  }

  return SQLITE_OK;
}
```

#### 5.2 Implement Constraint-Aware xFilter

```c
static int messages_filter(sqlite3_vtab_cursor *cur, int idxNum,
                           const char *idxStr, int argc, sqlite3_value **argv) {
  MessagesCursor *pCur = (MessagesCursor*)cur;

  pCur->rowid = 0;
  pCur->eof = 0;

  switch (idxNum) {
    case PLAN_SESSION_FILTER:
      // argv[0] contains session_id value
      if (argc > 0) {
        const char *session_id = (const char*)sqlite3_value_text(argv[0]);
        // Only open the specific session file
        return filter_by_session(pCur, session_id);
      }
      break;

    case PLAN_PROJECT_FILTER:
      // argv[0] contains project_id value
      if (argc > 0) {
        const char *project_id = (const char*)sqlite3_value_text(argv[0]);
        // Only scan sessions in the specific project
        return filter_by_project(pCur, project_id);
      }
      break;

    case PLAN_FULL_SCAN:
    default:
      // Scan all projects and sessions
      return filter_full_scan(pCur);
  }

  return SQLITE_OK;
}
```

#### 5.3 Update Sessions and Projects xBestIndex

Similarly add constraint detection for `project_id` in sessions table.

#### 5.4 SQL Helper Functions (No Caching)

```c
// get_message_content(json_text) -> TEXT
static void get_message_content_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  const char *json = (const char*)sqlite3_value_text(argv[0]);

  // Parse JSON on every call (no caching per team decision)
  cJSON *root = cJSON_Parse(json);
  if (!root) {
    sqlite3_result_null(ctx);
    return;
  }

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (message) {
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content) {
      if (cJSON_IsString(content)) {
        sqlite3_result_text(ctx, content->valuestring, -1, SQLITE_TRANSIENT);
      } else if (cJSON_IsArray(content)) {
        // Extract text from array elements
        int array_size = cJSON_GetArraySize(content);
        for (int i = 0; i < array_size; i++) {
          cJSON *item = cJSON_GetArrayItem(content, i);
          cJSON *type = cJSON_GetObjectItem(item, "type");
          if (type && strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (text && cJSON_IsString(text)) {
              sqlite3_result_text(ctx, text->valuestring, -1, SQLITE_TRANSIENT);
              break;
            }
          }
        }
      }
    }
  }

  cJSON_Delete(root);
}

// Additional helper functions:
// - get_message_role(json)
// - get_message_model(json)
// - get_message_text(json) - extract plain text, skip thinking blocks
// - message_token_count(json)
// - is_thinking_message(json)
```

#### 5.5 Register Functions

```c
int sqlite3_claude_code_init(sqlite3 *db, ...) {
  // ... after virtual table registration

  sqlite3_create_function(db, "get_message_content", 1, SQLITE_UTF8,
                          NULL, get_message_content_func, NULL, NULL);
  sqlite3_create_function(db, "get_message_role", 1, SQLITE_UTF8,
                          NULL, get_message_role_func, NULL, NULL);
  sqlite3_create_function(db, "get_message_model", 1, SQLITE_UTF8,
                          NULL, get_message_model_func, NULL, NULL);
  sqlite3_create_function(db, "get_message_text", 1, SQLITE_UTF8,
                          NULL, get_message_text_func, NULL, NULL);
  sqlite3_create_function(db, "message_token_count", 1, SQLITE_UTF8,
                          NULL, message_token_count_func, NULL, NULL);
  sqlite3_create_function(db, "is_thinking_message", 1, SQLITE_UTF8,
                          NULL, is_thinking_message_func, NULL, NULL);

  // ... continue
}
```

#### 5.6 Create Metadata Table

```c
const char *create_meta =
  "CREATE TABLE IF NOT EXISTS _jsonl_functions ("
  "  name TEXT PRIMARY KEY,"
  "  signature TEXT,"
  "  description TEXT,"
  "  example TEXT"
  ")";
sqlite3_exec(db, create_meta, NULL, NULL, NULL);

const char *docs[] = {
  "INSERT OR REPLACE INTO _jsonl_functions VALUES("
  "'get_message_content', 'get_message_content(json_text)', "
  "'Extract message.content, handling array/text formats', "
  "'SELECT get_message_content(json_data) FROM messages WHERE type=\"user\"')",

  "INSERT OR REPLACE INTO _jsonl_functions VALUES("
  "'get_message_role', 'get_message_role(json_text)', "
  "'Extract role from message (user, assistant, system)', "
  "'SELECT get_message_role(json_data) FROM messages WHERE type=\"user\"')",

  // ... more functions
  NULL
};

for (int i = 0; docs[i]; i++) {
  sqlite3_exec(db, docs[i], NULL, NULL, NULL);
}
```

### Testing

Create `test/performance.test`:
```sql
.load build/claude_code.dylib

.timer on

-- Test: Query with session_id constraint (should be fast)
EXPLAIN QUERY PLAN
SELECT * FROM messages WHERE session_id = '136a28d8-a077-4e92-a098-db544000f4cd';
-- Check that cost is low (~5100)

SELECT COUNT(*) FROM messages WHERE session_id = '136a28d8-a077-4e92-a098-db544000f4cd';

-- Test: Query with project_id constraint (should be faster than full scan)
SELECT COUNT(*) FROM messages
JOIN sessions ON messages.session_id = sessions.session_id
WHERE sessions.project_id = 'claude-code-logger';

.timer off
```

Create `test/functions.test`:
```sql
.load build/claude_code.dylib

-- Test helper functions
SELECT get_message_role(json_data) FROM messages WHERE type = 'user' LIMIT 1;
-- Expected: "user"

SELECT get_message_model(json_data) FROM messages WHERE type = 'assistant' LIMIT 1;
-- Expected: model name

-- Test function documentation
SELECT name, signature FROM _jsonl_functions;
-- Expected: List of all functions
```

### Success Criteria
- xBestIndex detects session_id and project_id constraints
- Queries with constraints are significantly faster
- Helper functions work correctly
- Helper functions parse JSON on every call (no caching)
- Metadata table documents all functions
- Functions handle NULL/invalid input gracefully

### Deliverables
- Sophisticated xBestIndex for all three tables
- 5+ helper SQL functions
- Metadata table with function documentation
- Performance test suite

---

## Phase 6: Production Hardening

### Goals
- Thread safety (mutex-protected initialization)
- Comprehensive test coverage
- Test against real data (carefully, read-only)
- Performance profiling
- Code quality (clang-format, clang-tidy)
- Documentation (README with WAL mode recommendation)
- Memory leak checking

After this phase: Production-ready extension.

### Tasks

#### 6.1 Thread Safety Implementation

```c
// Global state for thread-safe initialization
static sqlite3_mutex *g_init_mutex = NULL;
static int g_initialized = 0;

int sqlite3_claude_code_init(sqlite3 *db, char **pzErrMsg,
                               const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  int rc;

  // Lazy-initialize mutex
  if (!g_init_mutex) {
    g_init_mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER);
  }

  sqlite3_mutex_enter(g_init_mutex);

  if (g_initialized) {
    // Already initialized by another thread
    sqlite3_mutex_leave(g_init_mutex);
    return SQLITE_OK;
  }

  // Perform one-time initialization
  // - Register virtual table modules
  // - Auto-create tables
  // - Register helper functions
  // - Create metadata tables

  // ... (all Phase 2-5 initialization code here)

  g_initialized = 1;
  sqlite3_mutex_leave(g_init_mutex);
  return SQLITE_OK;
}
```

#### 6.2 Integration Tests

Create `test/integration.test`:
```sql
.load build/claude_code.dylib

-- Test: Zero-setup - tables exist immediately
.tables
-- Expected: projects, sessions, messages, _jsonl_functions

-- Test: Count messages per project
SELECT p.project_id, COUNT(*) as msg_count
FROM projects p
JOIN sessions s ON p.project_id = s.project_id
JOIN messages m ON s.session_id = m.session_id
GROUP BY p.project_id
ORDER BY msg_count DESC;

-- Test: Find assistant messages
SELECT m.type, COUNT(*)
FROM messages m
WHERE m.type = 'assistant';

-- Test: Helper functions
SELECT s.session_id, get_message_text(m.json_data)
FROM sessions s
JOIN messages m ON s.session_id = m.session_id
WHERE m.type = 'user'
LIMIT 5;

-- Test: Performance with constraints
.timer on
SELECT COUNT(*) FROM messages
WHERE session_id = (SELECT session_id FROM sessions LIMIT 1);
.timer off
```

#### 6.3 Real Data Testing (Read-Only)

Create test script:
```bash
#!/bin/bash
# test/real_data_test.sh

sqlite3 :memory: << EOF
.load build/claude_code.dylib

-- Verify extension loaded
.tables

-- Count projects
SELECT COUNT(*) as project_count FROM projects;

-- Count sessions
SELECT COUNT(*) as session_count FROM sessions;

-- Count messages by type
SELECT type, COUNT(*) as count
FROM messages
GROUP BY type
ORDER BY count DESC;

-- Test helper function
SELECT get_message_role(json_data)
FROM messages
WHERE type = 'user'
LIMIT 1;

.quit
EOF
```

#### 6.4 Code Quality

```bash
# Format all source files
clang-format -i src/*.c src/*.h

# Run static analysis
clang-tidy src/jsonl_projects.c -- \
  -I/opt/homebrew/Cellar/sqlite/3.51.0/include \
  -Wall -Wextra
```

#### 6.5 Memory Leak Checking

```bash
# On macOS, use leaks command
MallocStackLogging=1 sqlite3 :memory: << EOF
.load build/claude_code.dylib
SELECT COUNT(*) FROM messages;
.quit
EOF

leaks sqlite3
```

#### 6.6 Documentation (README.md)

Create comprehensive README:
```markdown
# Claude Code JSONL Virtual Table Extension

Zero-setup SQLite extension for querying Claude Code chat session logs.

## Installation

```bash
gmake
```

## Quick Start

```bash
sqlite3 :memory: -init .claude_code_init
sqlite> SELECT * FROM projects;
```

## Usage

The extension automatically creates three tables:
- `projects` - Chat projects
- `sessions` - Chat sessions (JSONL files)
- `messages` - Individual messages

### Configuration

Set custom projects directory:
```bash
export CLAUDE_PROJECTS_DIR=/path/to/projects
```

### Production Deployment

For multi-user web applications, enable WAL mode:
```sql
PRAGMA journal_mode=WAL;
```

WAL mode allows concurrent readers and writers without blocking.

### Query Performance

Use WHERE clauses with `session_id` or `project_id` for optimal performance:
```sql
-- Fast: uses xBestIndex optimization
SELECT * FROM messages WHERE session_id = '...';

-- Slow: full scan
SELECT * FROM messages;
```

### Helper Functions

- `get_message_content(json)` - Extract message content
- `get_message_role(json)` - Extract role
- `get_message_model(json)` - Extract model name
- `get_message_text(json)` - Get plain text (skip thinking)
- `message_token_count(json)` - Extract token count
- `is_thinking_message(json)` - Check for thinking block

List all functions:
```sql
SELECT * FROM _jsonl_functions;
```

## Example Queries

... (include examples from spec)

## Troubleshooting

... (common issues and solutions)
```

### Success Criteria
- Thread safety: Multiple threads can load extension safely
- All tests pass
- Works with real Claude Code projects
- No memory leaks detected
- Code formatted and passes static analysis
- Query performance acceptable (<5s for typical queries)
- Documentation complete and clear

### Deliverables
- Thread-safe initialization
- Comprehensive test suite
- Real data test script
- Clean, formatted code (clang-format, clang-tidy)
- User documentation (README.md)
- Performance benchmarks

---

## Milestones and Checkpoints

| Phase | Deliverable | Checkpoint |
|-------|-------------|------------|
| 1 | Build system + minimal extension | Extension loads in sqlite3 |
| 2 | `projects` table + zero-setup + flexible args | Can query projects immediately after load |
| 3 | `sessions` table | Can query projects + sessions |
| 4 | `messages` table | Full functionality, can experiment with real queries |
| 5 | xBestIndex optimization + helper functions | Queries are fast when filtered properly |
| 6 | Thread safety + testing + docs | Production-ready extension |

## Risks and Mitigations

### Risk: Large JSONL files cause memory issues
Mitigation: Stream parsing, don't load entire file into memory (Phase 4)

### Risk: Malformed JSON crashes extension
Mitigation: Robust error handling, skip bad records, log warnings (Phase 4)

### Risk: Performance too slow for interactive use
Mitigation: Sophisticated xBestIndex with constraint detection (Phase 5)

### Risk: Race conditions in multi-threaded environment
Mitigation: Mutex-protected initialization (Phase 6)

### Risk: Directory structure changes break extension
Mitigation: Flexible key=value parameter system (Phase 2)

### Risk: Multi-session development loses context
Mitigation: Comprehensive documentation (SPECIFICATION.md + IMPLEMENTATION_PLAN.md)

## Git Commit Strategy

Commit after completing each phase:
- Phase 1: "Add build system and minimal extension"
- Phase 2: "Implement projects table with zero-setup and flexible arguments"
- Phase 3: "Implement sessions table"
- Phase 4: "Implement messages table with JSON parsing"
- Phase 5: "Add xBestIndex optimization and helper functions"
- Phase 6: "Add thread safety, tests, and documentation"

Keep commits focused and atomic. Include the Claude Code footer:
```
🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude <noreply@anthropic.com>
```

## Next Steps

1. Review updated SPECIFICATION.md and IMPLEMENTATION_PLAN.md
2. Start Phase 1: Project scaffolding
3. Iterate through phases, testing and committing as we go
4. Experiment with queries after Phase 4
5. Optimize performance in Phase 5
6. Harden for production in Phase 6
7. Deploy and use for actual analysis

---

Document Status: Living document - incorporates team feedback
Last Updated: 2025-11-19
Authors: Team + Claude Code
