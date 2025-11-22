## Build System

- Use `make`, `make clean`, etc. -- not `gmake`

## Code Quality

- Do not silence warnings to resolve them; instead, change the code.
- Build with `-Wall -Wextra -Werror` - all warnings are errors

## Coding Conventions

### Unused Parameters

- Use `UNUSED` macro in parameter declarations (declaration attribute)
- Use `MARK_UNUSED(x)` in function body (statement)
- Both are always used together for uniform code across all compilers
- Never use bare `(void)param` casts - use the macros
- Document why parameters are required by interface but unused in implementation

### Compiler Portability

- Use `__has_attribute` for feature detection (preferred over compiler detection)
- Fallback to `__GNUC__` / `__clang__` detection for older compilers
- Support GCC, Clang, and MSVC
- Document compiler-specific code with comments explaining the approach

### String Operations

- Use helper functions (e.g., `copy_config_string`) for repetitive patterns
- Avoid duplicated strncpy/bounds-checking code

### Structure Organization

- Flatten structures - don't expose implementation details
- Extract needed fields rather than nesting entire structs (e.g., extract `time_t created_at` from `struct stat` instead of nesting `struct stat`)
- Keep access patterns shallow (prefer `obj.field` over `obj.nested.field.subfield`)
- Maximum 2 levels of member access preferred

### Documentation

- Document why unused parameters are required (interface compliance)
- Explain xBestIndex cost estimation and idxNum usage
- Document valid argument names in function comments
- Add TODOs for future work (e.g., regex filtering, file refactoring)

### Code Readability

- Prefer readability and maintainability over micro-optimizations
- Extract complex logic into helper functions
- Use descriptive names that indicate intent

## SQLite Extension Development

### Initialization Order

- `SQLITE_EXTENSION_INIT2(pApi)` must be called before any `sqlite3_*` API calls
- The macro just assigns a pointer - required to enable the API

### Thread Safety

- Use `SQLITE_MUTEX_STATIC_APP1` (or APP2/APP3) for extension mutexes
- Do NOT use `SQLITE_MUTEX_STATIC_MAIN` - it deadlocks during extension loading
- Per-connection setup (modules, functions): must run for every connection
- One-time setup (tables, metadata): protect with mutex, run once per process

### Virtual Table API

- `sqlite3_create_module()` registers with a specific db connection (per-connection)
- `CREATE VIRTUAL TABLE` creates in database (shared for file db, per-connection for :memory:)
- xBestIndex: use column index constants, set estimatedCost/estimatedRows, use aConstraintUsage

### Reference documentation

- [C-language Interface Specification for SQLite](https://sqlite.org/c3ref/intro.html)
- [Load An Extension](https://sqlite.org/c3ref/load_extension.html)
- [Virtual Table Interface Configuration](https://sqlite.org/c3ref/vtab_config.html)
- [Virtual Table Object](https://sqlite.org/c3ref/module.html)
- [Inspiration: CSV Virtual Table](https://sqlite.org/csv.html), [code](https://sqlite.org/src/artifact?ci=trunk&filename=ext/misc/csv.c)

## Project

- When future ideas and considerations arise, add them to @FUTURE.md or @FUTURE_USAGE.md


