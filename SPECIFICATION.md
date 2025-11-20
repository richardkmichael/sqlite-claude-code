# JSONL Virtual Table Extension - Specification

## Project Goal

Create a SQLite virtual table extension that presents a collection of directories containing JSONL (JSON Lines) files as queryable SQL tables, enabling flexible analysis and correlation of chat session data with other data sources.

## Context and Problem Statement

Claude Code generates chat session logs stored as:
- **Projects**: Directories representing different coding projects
- **Sessions**: JSONL files (one per chat session) identified by UUID
- **Messages**: Individual JSON records within JSONL files

Current challenges:
- Data is scattered across multiple directories and files
- No unified query interface for analysis
- Difficult to correlate chat data with external sources
- File structure makes ad-hoc queries cumbersome
- New files are constantly being created (real-time requirement)

## Requirements

### Functional Requirements
1. Read project directories and JSONL files dynamically (no data copying)
2. Present data as three SQL tables: `projects`, `sessions`, `messages`
3. Support standard SQL queries (SELECT, WHERE, JOIN, GROUP BY, etc.)
4. Extract stable JSON fields to table columns
5. Preserve full JSON for flexible field access
6. Handle six message types: `user`, `assistant`, `system`, `file-history-snapshot`, `summary`, `queue-operation`
7. Provide SQL helper functions for common JSON extractions
8. Enforce read-only access (no INSERT/UPDATE/DELETE)

### Non-Functional Requirements
1. **Real-time visibility**: Queries see current state of filesystem
2. **No data duplication**: Virtual tables, not copied data
3. **Handle large files**: Some JSONL files exceed normal memory limits
4. **Performance**: Acceptable query response for interactive use
5. **Stability**: Don't crash on malformed JSON or missing files
6. **Portability**: Works with homebrew SQLite on macOS

## Architecture Decisions

### Decision 1: SQLite Virtual Tables vs. Alternative Approaches

**Chosen**: SQLite virtual table extension

**Rationale**:
- SQL provides flexible, ad-hoc query interface (use cases not fully known)
- Easy to JOIN with other data sources
- Standard tool (works with any SQLite client)
- Virtual table API designed exactly for this use case
- Better than custom Python API for exploratory queries

**Alternatives Considered**:
- **Python/TypeScript API**: More flexible implementation but non-standard interface, harder for third-party tools
- **Load into actual tables**: Better performance but requires sync logic, duplicates data, loses real-time visibility
- **File-based tools (grep/jq)**: No relational queries or JOINs

### Decision 2: Three Virtual Tables vs. Single Table

**Chosen**: Three separate virtual tables (`projects`, `sessions`, `messages`)

**Rationale**:
- Natural hierarchy matches data structure
- Better query ergonomics (filter by project name, not table name)
- Foreign key relationships express data model clearly
- Standard SQL patterns (JOIN across tables)
- Easier to understand and explain

**Alternatives Considered**:
- **Single unified table**: Requires repeating project/session data on every message row, denormalized, harder to query
- **Table-per-project**: Project names would need SQL escaping, meta-programming required, can't easily query across projects

### Decision 3: Schema Design - Extracted Fields vs. Pure JSON

**Chosen**: Hybrid approach - extract stable fields, keep full JSON

**Extracted fields**:
- `type`: Message type (always present)
- `timestamp`: Message timestamp (present in most types)
- `uuid`/`message_id`: Unique identifier
- `parent_id`: Thread structure (`parentUuid`)
- `user_type`: Message originator (`userType`)
- `session_id`: Links to session UUID
- `project_id`: Links to project directory name

**Stored as JSON**:
- `json_data` (TEXT column): Full JSON object for flexible access

**Rationale**:
- Different message types have vastly different schemas
- `message` object varies significantly (user vs. assistant)
- Nested objects (thinking blocks, usage stats, etc.)
- JSON schema may evolve over time
- Users can extract any field via `json_extract(json_data, '$.path')`
- Common queries benefit from indexed columns (type, timestamp, session_id)

**Alternatives Considered**:
- **Fully denormalized**: Extract all fields → many nullable columns, schema changes break extension
- **Pure JSON**: Everything in json_data → slow for common queries, no indexes

### Decision 4: Read-Only Access

**Chosen**: Virtual tables are strictly read-only

**Rationale**:
- Source of truth is JSONL files, not database
- Prevents accidental data corruption
- Simpler implementation (no xUpdate logic)
- Matches primary use case (analysis, not modification)

**Implementation**: Do not implement `xUpdate` function in virtual table module, or implement to return `SQLITE_READONLY`.

### Decision 5: JSON Storage Type

**Chosen**: `TEXT` column type for JSON data

**Rationale**:
- SQLite has no dedicated JSON or JSONB type
- JSON is stored as TEXT in SQLite
- SQLite's json1 extension provides functions (`json_extract()`, etc.)
- No performance difference vs. hypothetical JSON type

### Decision 6: SQL Helper Functions

**Chosen**: Provide custom SQL functions alongside virtual tables

**Functions to implement**:
- `get_message_content(json)`: Extract message content (handle arrays/text)
- `get_message_role(json)`: Extract message.role
- `get_message_model(json)`: Extract model for assistant messages
- `get_message_text(json)`: Get plain text (skip thinking blocks)
- `message_token_count(json)`: Extract token usage stats
- `is_thinking_message(json)`: Check if contains thinking block

**Rationale**:
- Common extractions are tedious with `json_extract()`
- Improves query readability
- Can evolve independently of schema
- Users can still use `json_extract()` for custom fields

**Documentation approach**: Create `_jsonl_functions` metadata table listing available functions with examples.

### Decision 7: JSON Parsing Library

**Chosen**: cJSON (https://github.com/DaveGamble/cJSON)

**Rationale**:
- Simple, single-file library (easy to vendor)
- MIT licensed
- Fast and lightweight
- Widely used
- Sufficient for our parsing needs

**Alternatives Considered**:
- **json-c**: More mature but heavier dependency
- **SQLite's built-in JSON**: Complex to use from C in virtual table context
- **Custom parser**: Reinventing the wheel, risk of bugs

### Decision 8: Performance Strategy

**Approach**: Lazy loading with optional caching

**Implementation**:
- Don't load all files into memory on `xFilter()`
- Stream records as SQLite calls `xNext()`
- Cache directory listing with configurable TTL (5 minutes default)
- Open/close JSONL files as needed
- Parse JSON on-demand per row

**Tradeoffs**:
- First query may be slower (directory scan)
- Subsequent queries faster (cached directory list)
- Memory-efficient (no bulk loading)
- `COUNT(*)` queries still require full scan

**Future optimization**:
- Index files by timestamp/type if performance issues emerge
- Background thread for directory watching (future)

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

**Source**: Subdirectories under `base_directory` parameter

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

**Source**: `*.jsonl` files in project directories (excluding `agent-*.jsonl` by default)

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

**Source**: Individual lines from JSONL files

## Extension Interface

### Loading the Extension

```sql
.load ./jsonl_projects.dylib
```

### Creating Virtual Tables

```sql
CREATE VIRTUAL TABLE my_projects USING jsonl_projects(
  base_directory='/Users/rmichael/.claude/projects'
);
```

This creates three virtual tables:
- `my_projects_projects`
- `my_projects_sessions`
- `my_projects_messages`

Or simpler registration (auto-creates `projects`, `sessions`, `messages`):

```sql
CREATE VIRTUAL TABLE chat USING jsonl_projects(
  base_directory='/Users/rmichael/.claude/projects'
);
-- Creates: chat_projects, chat_sessions, chat_messages
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
```

## Technical Constraints

1. **Homebrew SQLite**: `/opt/homebrew/Cellar/sqlite/3.51.0`
2. **macOS**: Darwin 23.6.0 (Apple Silicon assumed)
3. **Build tools**: clang, gmake
4. **File sizes**: Some JSONL files exceed 100MB
5. **Real-time data**: Files being created/modified during queries

## Future Considerations

### Session Naming
Sessions currently identified only by UUID. Future enhancement:
- Add `session_name` column to `sessions` table
- Extract from JSONL metadata or external mapping file
- Support human-readable session descriptions

### Extensibility
- Support additional message types as they're added
- Parameterize file exclusion patterns (currently `agent-*.jsonl` hardcoded)
- Support multiple base directories

### Performance Optimization
- File-based indexing for large projects
- Background directory watcher (eliminate scan latency)
- Query optimization hints for virtual table

### Multi-User Support
- Currently assumes single-user filesystem
- Could add user_id dimension in future

## Success Criteria

1. Extension loads successfully in SQLite
2. Three virtual tables are accessible via SQL
3. Queries return accurate data from test JSONL files
4. Large files (>100MB) handled without crashes
5. Real-time: new files appear in queries after creation
6. Read-only enforcement prevents modification
7. Helper functions work correctly
8. Documentation enables team members to use and extend the project

## References

- SQLite Virtual Table API: https://sqlite.org/vtab.html
- CSV Extension (reference): https://sqlite.org/csv.html
- SQLite C API: https://sqlite.org/c3ref/intro.html
- cJSON Library: https://github.com/DaveGamble/cJSON
- SQLite JSON Functions: https://sqlite.org/json1.html
