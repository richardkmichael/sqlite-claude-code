/*
 * Extension Initialization
 *
 * Registers all virtual table modules and auto-creates tables
 * for zero-setup user experience.
 *
 * Thread Safety Model:
 *   - One-time setup (tables, metadata) runs once per process, protected by mutex
 *   - Per-connection setup (modules, functions) runs for each db connection
 *   - All module structs are read-only after compilation
 *
 * Note: For file databases, tables are shared across connections.
 * For :memory: databases, only the first connection gets auto-created tables.
 */

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "common.h"

/*
 * Thread-safe initialization state.
 * g_schema_initialized: tracks whether tables/metadata have been created
 * The mutex prevents redundant schema setup when multiple threads
 * load the extension simultaneously into a shared database.
 */
static volatile int g_schema_initialized = 0;

#ifdef _WIN32
__declspec(dllexport)
#endif
/*
 * Extension initialization function.
 * This is called when the extension is loaded via .load command.
 *
 * Note: Function name must match the library filename convention.
 * For claude_code.dylib, SQLite expects sqlite3_claudecode_init
 * (underscores in filename are removed from the entry point name).
 */
int sqlite3_claudecode_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
) {
  int rc;

  /* Initialize SQLite API pointer - required before any sqlite3_* calls */
  SQLITE_EXTENSION_INIT2(pApi);

  /* Defensive check */
  if (!db) {
    if (pzErrMsg) {
      *pzErrMsg = sqlite3_mprintf("Invalid database connection");
    }
    return SQLITE_ERROR;
  }

  /*
   * Per-connection setup: module and function registration.
   * These must run for every connection that loads the extension.
   */
  rc = sqlite3_create_module(db, "claudecode_projects", &projects_module, NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = sqlite3_create_module(db, "claudecode_sessions", &sessions_module, NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = sqlite3_create_module(db, "claudecode_messages", &messages_module, NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = register_helper_functions(db);
  if (rc != SQLITE_OK) {
    return rc;
  }

  /*
   * One-time setup: schema creation.
   * Protected by mutex to prevent redundant work when multiple threads
   * load the extension into connections to the same database file.
   * Uses SQLITE_MUTEX_STATIC_APP1 (reserved for application use).
   */
  sqlite3_mutex *mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP1);
  sqlite3_mutex_enter(mutex);

  if (!g_schema_initialized) {
    /* Determine base directory for auto-created tables */
    const char *default_dir = getenv("CLAUDE_PROJECTS_DIR");
    char expanded_path[PATH_MAX];

    if (!default_dir) {
      /* Default to $HOME/.claude/projects */
      const char *home = getenv("HOME");
      snprintf(expanded_path, sizeof(expanded_path), "%s/.claude/projects", home);
      default_dir = expanded_path;

      /* Verify the default directory exists */
      struct stat st;
      if (stat(default_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        sqlite3_mutex_leave(mutex);
        if (pzErrMsg) {
          *pzErrMsg = sqlite3_mprintf(
            "Default directory %s does not exist. "
            "Set CLAUDE_PROJECTS_DIR environment variable to specify projects directory.",
            default_dir);
        }
        return SQLITE_ERROR;
      }
    }

    /* Auto-create virtual tables (zero-setup UX) */
    rc = create_projects_table(db, default_dir, pzErrMsg);
    if (rc != SQLITE_OK) {
      sqlite3_mutex_leave(mutex);
      return rc;
    }

    rc = create_sessions_table(db, default_dir, pzErrMsg);
    if (rc != SQLITE_OK) {
      sqlite3_mutex_leave(mutex);
      return rc;
    }

    rc = create_messages_table(db, default_dir, pzErrMsg);
    if (rc != SQLITE_OK) {
      sqlite3_mutex_leave(mutex);
      return rc;
    }

    /* Create functions metadata table */
    rc = create_functions_metadata_table(db, pzErrMsg);
    if (rc != SQLITE_OK) {
      sqlite3_mutex_leave(mutex);
      return rc;
    }

    g_schema_initialized = 1;
  }

  sqlite3_mutex_leave(mutex);
  return SQLITE_OK;
}
