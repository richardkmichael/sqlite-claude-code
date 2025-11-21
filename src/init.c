/*
 * Extension Initialization
 *
 * Registers all virtual table modules and auto-creates tables
 * for zero-setup user experience.
 */

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "common.h"

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
  SQLITE_EXTENSION_INIT2(pApi);
  int rc;

  /* Defensive check */
  if (!db) {
    if (pzErrMsg) {
      *pzErrMsg = sqlite3_mprintf("Invalid database connection");
    }
    return SQLITE_ERROR;
  }

  /* Register virtual table modules */
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

  /* Register SQL helper functions */
  rc = register_helper_functions(db);
  if (rc != SQLITE_OK) {
    return rc;
  }

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
      if (pzErrMsg) {
        *pzErrMsg = sqlite3_mprintf(
          "Default directory %s does not exist. "
          "Set CLAUDE_PROJECTS_DIR environment variable to specify projects directory.",
          default_dir);
      }
      return SQLITE_ERROR;
    }
  }

  /* Auto-create tables (zero-setup UX) */
  rc = create_projects_table(db, default_dir, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = create_sessions_table(db, default_dir, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = create_messages_table(db, default_dir, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  /* Create functions metadata table */
  rc = create_functions_metadata_table(db, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  return SQLITE_OK;
}
