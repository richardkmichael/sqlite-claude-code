# JSONL Virtual Table Extension - Specification

## Project Goal

Create a production-ready SQLite virtual table extension that presents a collection of directories containing JSONL (JSON Lines) files as queryable SQL tables, enabling flexible analysis and correlation of chat session data with other data sources. The extension must provide a zero-setup user experience and support multi-threaded environments (web servers).

## Context and Problem Statement

Claude Code generates chat session logs stored as:
- Projects: Directories representing different coding projects
- Sessions: JSONL files (one per chat session) identified by UUID
- Messages: Individual JSON records within JSONL files

Current challenges:
- Data is scattered across multiple directories and files
- No unified query interface for analysis
- Difficult to correlate chat data with external sources
- File structure makes ad-hoc queries cumbersome
- New files are constantly being created (real-time requirement)
- Need production-ready solution for multi-user web server environments

## Requirements

### Functional Requirements
1. Zero-setup experience: Load extension and immediately query without manual table creation
2. Read project directories and JSONL files dynamically (no data copying)
3. Present data as three SQL tables: `projects`, `sessions`, `messages`
4. Support standard SQL queries (SELECT, WHERE, JOIN, GROUP BY, etc.)
5. Extract stable JSON fields to table columns
6. Preserve full JSON for flexible field access
7. Handle six message types: `user`, `assistant`, `system`, `file-history-snapshot`, `summary`, `queue-operation`
8. Provide SQL helper functions for common JSON extractions
9. Enforce read-only access (no INSERT/UPDATE/DELETE)
10. Support flexible configuration via key=value parameters

### Non-Functional Requirements
1. Real-time visibility: Queries see current state of filesystem
2. No data duplication: Virtual tables, not copied data
3. Handle large files: Some JSONL files exceed 100MB
4. Performance: Query optimization via intelligent xBestIndex implementation
5. Thread safety: Safe for multi-threaded web server environments
6. Stability: Don't crash on malformed JSON or missing files
7. Portability: Works with homebrew SQLite on macOS
8. Concurrency: Compatible with WAL mode for multi-user access

## Architecture Decisions

### Decision 1: SQLite Virtual Tables vs. Alternative Approaches

Chosen: SQLite virtual table extension

Rationale:
- SQL provides flexible, ad-hoc query interface (use cases not fully known)
- Easy to JOIN with other data sources
- Standard tool (works with any SQLite client)
- Virtual table API designed exactly for this use case
- Better than custom Python API for exploratory queries
- Production-ready with proper implementation

Alternatives Considered:
- Python/TypeScript API: More flexible implementation but non-standard interface, harder for third-party tools
- Load into actual tables: Better performance but requires sync logic, duplicates data, loses real-time visibility
- File-based tools (grep/jq): No relational queries or JOINs

### Decision 2: Three Virtual Tables vs. Single Table

Chosen: Three separate virtual tables (`projects`, `sessions`, `messages`)

Rationale:
- Natural hierarchy matches data structure
- Better query ergonomics (filter by project name, not table name)
- Foreign key relationships express data model clearly
- Standard SQL patterns (JOIN across tables)
- Easier to understand and explain
- Enables efficient query pruning in partitioned scenarios

Alternatives Considered:
- Single unified table: Requires repeating project/session data on every message row, denormalized, harder to query
- Table-per-project: Project names would need SQL escaping, meta-programming required, can't easily query across projects

### Decision 3: Zero-Setup Auto-Table Creation

Chosen: Automatic table creation on extension load

User Experience:
```bash
sqlite3 :memory: -init .claude_code_init
# Tables immediately available:
sqlite> SELECT p.project_id, COUNT(*)
   ...> FROM projects p
   ...> JOIN sessions s ON p.project_id = s.project_id
   ...> GROUP BY p.project_id;
```

Implementation:
- Extension performs one-time census on load
- Auto-creates three virtual tables: `projects`, `sessions`, `messages`
- User can immediately query without manual `CREATE VIRTUAL TABLE` statements

Rationale:
- Dramatically improves user experience
- Reduces setup friction for experimentation
- Consistent interface across all installations
- Enables configuration via init file instead of SQL commands

### Decision 4: xBestIndex Query Optimization

Chosen: Intelligent cost estimation based on WHERE constraints

Critical Insight: Performance lives and dies by xBestIndex implementation. The extension must communicate accurate I/O costs to SQLite's query planner to avoid expensive full scans.

Implementation Strategy:

For messages table:
- Plan 1: Full Scan - No constraints → Must scan all files → Cost: ~10,000,000
- Plan 2: Session Filter - `session_id = ?` constraint → Read one file → Cost: ~5,100
- Plan 3: Project Filter - `project_id = ?` constraint → Scan project files → Cost: ~500,000

Example xBestIndex Implementation:
```c
static int messages_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  int session_idx = -1, project_idx = -1;

  // Find usable constraints from WHERE clause
  for (int i = 0; i < pIdxInfo->nConstraint; i++) {
    if (!pIdxInfo->aConstraint[i].usable) continue;

    if (pIdxInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ) {
      if (pIdxInfo->aConstraint[i].iColumn == COL_SESSION_ID) {
        session_idx = i;
      } else if (pIdxInfo->aConstraint[i].iColumn == COL_PROJECT_ID) {
        project_idx = i;
      }
    }
  }

  // Report cost based on best available constraint
  if (session_idx != -1) {
    // Best plan: session_id constraint
    pIdxInfo->idxNum = PLAN_SESSION_FILTER;
    pIdxInfo->aConstraintUsage[session_idx].argvIndex = 1;
    pIdxInfo->aConstraintUsage[session_idx].omit = 1;
    pIdxInfo->estimatedCost = 5100.0;
    pIdxInfo->estimatedRows = 1000;
  } else if (project_idx != -1) {
    // Good plan: project_id constraint
    pIdxInfo->idxNum = PLAN_PROJECT_FILTER;
    pIdxInfo->aConstraintUsage[project_idx].argvIndex = 1;
    pIdxInfo->aConstraintUsage[project_idx].omit = 1;
    pIdxInfo->estimatedCost = 500000.0;
    pIdxInfo->estimatedRows = 10000;
  } else {
    // Worst plan: full scan
    pIdxInfo->idxNum = PLAN_FULL_SCAN;
    pIdxInfo->estimatedCost = 10000000.0;
    pIdxInfo->estimatedRows = 1000000;
  }

  return SQLITE_OK;
}
```

Rationale:
- Accurate cost estimation enables optimal query plans
- Avoids scanning millions of records when filtering by session/project
- Critical for interactive query performance
- Foundation for future partitioning optimizations

### Decision 5: Thread Safety for Multi-User Access

Chosen: Mutex-protected one-time initialization

Target Environment: Multi-threaded web servers where multiple threads may load extension simultaneously.

Implementation:
```c
// Global state
static sqlite3_mutex *g_init_mutex = NULL;
static int g_initialized = 0;

int sqlite3_claude_code_init(sqlite3 *db, char **pzErrMsg,
                               const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);

  // Lazy-initialize mutex
  if (!g_init_mutex) {
    g_init_mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER);
  }

  sqlite3_mutex_enter(g_init_mutex);

  if (g_initialized) {
    sqlite3_mutex_leave(g_init_mutex);
    return SQLITE_OK; // Already initialized
  }

  // Perform census, create tables, register functions
  // ... initialization logic ...

  g_initialized = 1;
  sqlite3_mutex_leave(g_init_mutex);
  return SQLITE_OK;
}
```

Rationale:
- Prevents race conditions during initialization
- Essential for web server deployments
- Uses SQLite's built-in mutex primitives
- Low overhead after initial setup

Database Concurrency: Users should enable WAL mode for optimal multi-user concurrency:
```sql
PRAGMA journal_mode=WAL;
```

In WAL mode, readers do not block writers, enabling concurrent access essential for web applications.

### Decision 6: Schema Design - Extracted Fields vs. Pure JSON

Chosen: Hybrid approach - extract stable fields, keep full JSON

Extracted fields:
- `type`: Message type (always present)
- `timestamp`: Message timestamp (present in most types)
- `uuid`/`message_id`: Unique identifier
- `parent_id`: Thread structure (`parentUuid`)
- `user_type`: Message originator (`userType`)
- `session_id`: Links to session UUID
- `project_id`: Links to project directory name

Stored as JSON:
- `json_data` (TEXT column): Full JSON object for flexible access

Rationale:
- Different message types have vastly different schemas
- `message` object varies significantly (user vs. assistant)
- Nested objects (thinking blocks, usage stats, etc.)
- JSON schema may evolve over time
- Users can extract any field via `json_extract(json_data, '$.path')`
- Common queries benefit from indexed columns (type, timestamp, session_id)
- xBestIndex can optimize queries on extracted columns

Alternatives Considered:
- Fully denormalized: Extract all fields → many nullable columns, schema changes break extension
- Pure JSON: Everything in json_data → slow for common queries, no indexes

### Decision 7: Read-Only Access

Chosen: Virtual tables are strictly read-only

Rationale:
- Source of truth is JSONL files, not database
- Prevents accidental data corruption
- Simpler implementation (no xUpdate logic)
- Matches primary use case (analysis, not modification)

Implementation: Do not implement `xUpdate` function in virtual table module, or implement to return `SQLITE_READONLY`.

### Decision 8: JSON Storage Type

Chosen: `TEXT` column type for JSON data

Rationale:
- SQLite has no dedicated JSON or JSONB type
- JSON is stored as TEXT in SQLite
- SQLite's json1 extension provides functions (`json_extract()`, etc.)
- No performance difference vs. hypothetical JSON type

### Decision 9: SQL Helper Functions

Chosen: Provide custom SQL functions alongside virtual tables

Functions to implement:
- `get_message_content(json)`: Extract message content (handle arrays/text)
- `get_message_role(json)`: Extract message.role
- `get_message_model(json)`: Extract model for assistant messages
- `get_message_text(json)`: Get plain text (skip thinking blocks)
- `message_token_count(json)`: Extract token usage stats
- `is_thinking_message(json)`: Check if contains thinking block

Rationale:
- Common extractions are tedious with `json_extract()`
- Improves query readability
- Can evolve independently of schema
- Users can still use `json_extract()` for custom fields

Documentation approach: Create `_jsonl_functions` metadata table listing available functions with examples.

### Decision 10: No JSON Caching Policy

Chosen: Helper functions parse JSON on every call (no caching)

Rationale:
- Real-time requirement: Caching would serve stale data if files change
- Correctness over performance: Always reflect current file state
- Simpler implementation: No cache invalidation logic
- Memory efficiency: No cache memory overhead
- Acceptable performance: Modern JSON parsing is fast enough for interactive queries

If performance becomes an issue: Users can materialize results into actual tables for repeated access.

Alternatives Considered:
- cJSON object caching: Violates real-time visibility, complex invalidation, memory overhead

### Decision 11: JSON Parsing Library

Chosen: cJSON (https://github.com/DaveGamble/cJSON)

Rationale:
- Simple, single-file library (easy to vendor)
- MIT licensed
- Fast and lightweight
- Widely used
- Sufficient for our parsing needs

Alternatives Considered:
- json-c: More mature but heavier dependency
- SQLite's built-in JSON: Complex to use from C in virtual table context
- Custom parser: Reinventing the wheel, risk of bugs

### Decision 12: Flexible Configuration Architecture

Chosen: Generic key=value argument parser in xConnect

Implementation:
```c
static int jsonl_connect(sqlite3 *db, void *pAux, int argc,
                         const char *const *argv, ...) {
  // argv[0] = module name, argv[1] = db name, argv[2] = table name
  // argv[3+] are key=value arguments

  for (int i = 3; i < argc; i++) {
    parse_kv_argument(argv[i], &vtab->config);
  }
  // ... rest of connect logic
}
```

Supported Parameters (current and future):
- `base_directory=/path` - Root directory for scanning
- `exclude_pattern=agent-*` - File exclusion pattern
- `include_project=proj_name` - Limit to specific project (for partitioning)
- `include_projects=p1,p2,p3` - Limit to project list (for partitioning)

Rationale:
- Low-cost upfront design enables future features
- Enables partitioning without API changes
- Standard SQLite virtual table pattern
- Easy to extend with new parameters

### Decision 13: Performance Strategy

Approach: Lazy loading with no caching

Implementation:
- Don't load all files into memory on `xFilter()`
- Stream records as SQLite calls `xNext()`
- Open/close JSONL files as needed
- Parse JSON on-demand per row
- Use xBestIndex to avoid unnecessary file reads

Tradeoffs:
- First query may be slower (directory scan)
- Memory-efficient (no bulk loading)
- Always sees current filesystem state
- `COUNT(*)` queries still require full scan (acceptable for current scale)

Optimization Strategy:
- Phase 1-4: Basic lazy loading
- Phase 5: Intelligent xBestIndex with constraint detection
- Phase 7+: Partitioning for large-scale data

## Data Model

### Table: `projects`

```sql
CREATE TABLE projects (
  project_id   TEXT PRIMARY KEY,  -- Directory name (e.g., "-Users-rmichael-Documents-...")
  directory    TEXT NOT NULL,     -- Full path to directory
  created_at   DATETIME,          -- Directory creation time (ctime)
  updated_at   DATETIME           -- Directory modification time (mtime)
);
```

Source: Subdirectories under `base_directory` parameter

### Table: `sessions`

```sql
CREATE TABLE sessions (
  session_id   TEXT PRIMARY KEY,  -- UUID from JSONL filename
  project_id   TEXT NOT NULL,     -- Foreign key to projects
  file_path    TEXT NOT NULL,     -- Full path to .jsonl file
  record_count INT,               -- Number of messages in session
  created_at   DATETIME,          -- File creation time (ctime)
  updated_at   DATETIME,          -- File modification time (mtime)
  FOREIGN KEY (project_id) REFERENCES projects(project_id)
);
```

Source: `*.jsonl` files in project directories (excluding `agent-*.jsonl` by default)

### Table: `messages`

```sql
CREATE TABLE messages (
  message_id   TEXT PRIMARY KEY,  -- uuid field from JSON record
  session_id   TEXT NOT NULL,     -- Foreign key to sessions
  type         TEXT NOT NULL,     -- Message type (user, assistant, system, etc.)
  timestamp    DATETIME,          -- From record.timestamp
  parent_id    TEXT,              -- parentUuid (NULL for root messages)
  user_type    TEXT,              -- userType field
  content_type TEXT,              -- subtype for system messages
  json_data    TEXT NOT NULL,     -- Full JSON object as text
  FOREIGN KEY (session_id) REFERENCES sessions(session_id)
);
```

Source: Individual lines from JSONL files

## Extension Interface

### Zero-Setup Usage

The extension automatically creates tables on load:

```bash
# Create init file
echo ".load ./build/claude_code.dylib" > .claude_code_init

# Start SQLite - tables are immediately available
sqlite3 :memory: -init .claude_code_init

sqlite> SELECT project_id, COUNT(*) AS msg_count
   ...> FROM projects p
   ...> JOIN sessions s ON p.project_id = s.project_id
   ...> JOIN messages m ON s.session_id = m.session_id
   ...> GROUP BY p.project_id
   ...> ORDER BY msg_count DESC;
```

### Configuration

The extension scans a default directory (configurable at compile time or via environment variable):

```c
// Default: ~/.claude/projects
// Or set via: export CLAUDE_PROJECTS_DIR=/custom/path
```

### Manual Table Creation (Advanced)

For custom configurations:

```sql
.load ./claude_code.dylib

CREATE VIRTUAL TABLE custom_projects USING jsonl_projects(
  base_directory='/custom/path',
  exclude_pattern='agent-*'
);
-- Creates: custom_projects_projects, custom_projects_sessions, custom_projects_messages
```

### Example Queries

```sql
-- Count messages per project
SELECT p.project_id, COUNT(*) as msg_count
FROM projects p
JOIN sessions s ON p.project_id = s.project_id
JOIN messages m ON s.session_id = m.session_id
GROUP BY p.project_id
ORDER BY msg_count DESC;

-- Find all assistant messages in a specific project
SELECT m.timestamp, get_message_text(m.json_data) as content
FROM messages m
JOIN sessions s ON m.session_id = s.session_id
WHERE s.project_id = 'claude-code-logger'
  AND m.type = 'assistant'
ORDER BY m.timestamp;

-- Analyze token usage across sessions
SELECT s.session_id, SUM(message_token_count(m.json_data)) as total_tokens
FROM sessions s
JOIN messages m ON s.session_id = m.session_id
WHERE m.type = 'assistant'
GROUP BY s.session_id;

-- Efficient query using session_id constraint (low cost via xBestIndex)
SELECT type, COUNT(*)
FROM messages
WHERE session_id = '136a28d8-a077-4e92-a098-db544000f4cd'
GROUP BY type;
```

## Technical Constraints

1. Homebrew SQLite: `/opt/homebrew/Cellar/sqlite/3.51.0`
2. macOS: Darwin 23.6.0 (Apple Silicon assumed)
3. Build tools: clang, gmake
4. File sizes: Some JSONL files exceed 100MB
5. Real-time data: Files being created/modified during queries
6. Multi-threaded: Must support concurrent access from web servers

## Production Deployment Recommendations

### Database Configuration

For multi-user web applications, enable WAL mode:

```sql
PRAGMA journal_mode=WAL;
```

This allows readers and writers to work concurrently without blocking each other.

### Performance Tuning

- Use WHERE clauses with `session_id` or `project_id` when possible (leverages xBestIndex optimization)
- For repeated queries on static data, materialize results into actual tables
- Monitor query performance with `.timer on`

## Future Considerations

### Session Naming
Sessions currently identified only by UUID. Future enhancement:
- Add `session_name` column to `sessions` table
- Extract from JSONL metadata or external mapping file
- Support human-readable session descriptions

### Large-Scale Data Partitioning

As data grows, automatic partitioning will become necessary. The flexible configuration architecture supports this.

Strategy: Census → Partition → Unify

1. Census Phase (in init function):
   - One-time scan of base_directory
   - Build in-memory list of all projects and their sizes
   - Classify projects as "large" (>250MB) or "small"

2. Partition Phase:
   - Create dedicated virtual tables for large projects:
     ```sql
     CREATE VIRTUAL TABLE _partition_projA_messages USING jsonl_messages(
       include_project='project_A'
     );
     ```
   - Create grouped virtual tables for small projects:
     ```sql
     CREATE VIRTUAL TABLE _partition_small_messages USING jsonl_messages(
       include_projects='proj_B,proj_C,proj_D'
     );
     ```

3. Unify Phase:
   - Create user-facing VIEW combining all partitions:
     ```sql
     CREATE VIEW messages AS
       SELECT * FROM _partition_projA_messages
       UNION ALL
       SELECT * FROM _partition_projB_messages
       UNION ALL
       SELECT * FROM _partition_small_messages;
     ```

Query Pruning Mechanism:

When users query `WHERE project_id = 'project_A'`, SQLite pushes this constraint down to all UNION ALL branches. Partition-aware xBestIndex implementations detect when a query is impossible for a partition and report zero cost/rows:

```c
static int messages_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  jsonl_vtab *pVtab = (jsonl_vtab*)tab;

  const char *requested_project = find_project_constraint(pIdxInfo);

  if (requested_project && !is_project_in_partition(pVtab, requested_project)) {
    // This partition cannot satisfy this query - signal pruning
    pIdxInfo->estimatedCost = 0.0;
    pIdxInfo->estimatedRows = 0;
    return SQLITE_OK;
  }

  // ... normal cost calculation for valid queries
}
```

The 0/0 signal tells SQLite to completely skip that partition, making queries highly efficient even with many partitions.

### Multi-User Support
- Currently assumes single-user filesystem
- Could add user_id dimension in future
- Would require changes to directory structure and scanning logic

## Success Criteria

1. Zero-setup: Extension loads and tables are immediately queryable
2. Performance: xBestIndex optimization makes filtered queries fast
3. Thread safety: Safe for multi-threaded web server use
4. Accuracy: Queries return correct data from test and real JSONL files
5. Large files: Files >100MB handled without crashes
6. Real-time: New files appear in queries after creation
7. Read-only: Modification attempts fail gracefully
8. Helper functions: Work correctly without caching issues
9. Documentation: Enables team members to use and extend the project
10. Production-ready: Passes all tests, no memory leaks, clean code

## References

- SQLite Virtual Table API: https://sqlite.org/vtab.html
- SQLite xBestIndex: https://sqlite.org/vtab.html#xbestindex
- CSV Extension (reference): https://sqlite.org/csv.html
- SQLite C API: https://sqlite.org/c3ref/intro.html
- SQLite WAL Mode: https://sqlite.org/wal.html
- cJSON Library: https://github.com/DaveGamble/cJSON
- SQLite JSON Functions: https://sqlite.org/json1.html
- SQLite Thread Safety: https://sqlite.org/threadsafe.html

---

Document Status: Living document - updated with team feedback
Last Updated: 2025-11-19
Authors: Team + Claude Code
