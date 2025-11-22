/*
 * Projects Virtual Table
 *
 * Provides read-only access to Claude Code project directories.
 */

#include <sqlite3ext.h>
extern const sqlite3_api_routines *sqlite3_api; /* Defined in init.c */

#include "common.h"

/*
 * Projects virtual table structure
 */
typedef struct ProjectsVTab {
  sqlite3_vtab base;
  vtab_config config;
} ProjectsVTab;

/*
 * Current project information
 * Organizes all data about the project we're currently pointing at
 */
typedef struct CurrentProject {
  char name[256];      /* Directory name (project_id) */
  char path[PATH_MAX]; /* Full path to directory */
  time_t created_at;   /* Creation time */
  time_t updated_at;   /* Modification time */
} CurrentProject;

/*
 * Projects cursor structure for iteration
 */
typedef struct ProjectsCursor {
  sqlite3_vtab_cursor base;
  DIR *dir_handle;
  char base_path[PATH_MAX];
  CurrentProject current;
  int eof;
  sqlite3_int64 rowid;
} ProjectsCursor;

/*
 * xConnect/xCreate - Create a new virtual table instance
 */
static int projects_connect(sqlite3 *db, void *pAux UNUSED, int argc, const char *const *argv,
                            sqlite3_vtab **ppVTab, char **pzErr) {
  MARK_UNUSED(pAux); /* Required by interface, unused in our implementation */

  ProjectsVTab *pTab = sqlite3_malloc(sizeof(ProjectsVTab));
  if (!pTab) {
    return SQLITE_NOMEM;
  }
  memset(pTab, 0, sizeof(ProjectsVTab));

  /* Default configuration */
  const char *default_dir = getenv("CLAUDE_PROJECTS_DIR");
  if (!default_dir) {
    /* Default to $HOME/.claude/projects */
    const char *home_directory = getenv("HOME");
    snprintf(pTab->config.base_directory, sizeof(pTab->config.base_directory),
             "%s/.claude/projects", home_directory);
  } else {
    strncpy(pTab->config.base_directory, default_dir, sizeof(pTab->config.base_directory) - 1);
    pTab->config.base_directory[sizeof(pTab->config.base_directory) - 1] = '\0';
  }

  /* Parse arguments (argv[3..argc-1] are the parameters) */
  for (int i = 3; i < argc; i++) {
    if (parse_kv_argument(argv[i], &pTab->config) != 0) {
      *pzErr = sqlite3_mprintf("Invalid argument: %s", argv[i]);
      sqlite3_free(pTab);
      return SQLITE_ERROR;
    }
  }

  /* Declare table schema */
  int rc = sqlite3_declare_vtab(db, "CREATE TABLE projects("
                                    "  project_id TEXT,"    /* Directory name */
                                    "  directory TEXT,"     /* Full path */
                                    "  created_at INTEGER," /* ctime */
                                    "  updated_at INTEGER"  /* mtime */
                                    ")");

  if (rc != SQLITE_OK) {
    sqlite3_free(pTab);
    return rc;
  }

  *ppVTab = &pTab->base;
  return SQLITE_OK;
}

/*
 * xDisconnect/xDestroy - Destroy a virtual table instance
 */
static int projects_disconnect(sqlite3_vtab *pVTab) {
  ProjectsVTab *pTab = (ProjectsVTab *)pVTab;
  sqlite3_free(pTab);
  return SQLITE_OK;
}

/*
 * xBestIndex - Determine the best way to execute a query
 *
 * SQLite calls this to ask: "How would you execute this query?"
 * We respond with:
 *   idxNum - An arbitrary integer identifying our chosen query plan
 *            (we pick the value, SQLite just passes it back to xFilter)
 *   estimatedCost - Relative cost (SQLite compares across plans)
 *   estimatedRows - Expected number of rows returned
 *
 * Phase 2: Simple implementation - only one plan (full scan)
 * Phase 5: Will add optimized plans for filtered queries
 */
static int projects_best_index(sqlite3_vtab *tab UNUSED, sqlite3_index_info *pIdxInfo) {
  MARK_UNUSED(tab); /* Required by interface, unused for simple full scan */

  /* Plan ID 1: Full directory scan (our only plan for now) */
  pIdxInfo->idxNum = 1;

  /* Cost estimate: scanning ~50 directories is cheap */
  pIdxInfo->estimatedCost = 100.0;
  pIdxInfo->estimatedRows = 50;

  return SQLITE_OK;
}

/*
 * xOpen - Create a new cursor
 */
static int projects_open(sqlite3_vtab *pVTab UNUSED, sqlite3_vtab_cursor **ppCursor) {
  MARK_UNUSED(pVTab);

  ProjectsCursor *pCur = sqlite3_malloc(sizeof(ProjectsCursor));
  if (!pCur) {
    return SQLITE_NOMEM;
  }
  memset(pCur, 0, sizeof(ProjectsCursor));

  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

/*
 * xClose - Close a cursor
 */
static int projects_close(sqlite3_vtab_cursor *cur) {
  ProjectsCursor *pCur = (ProjectsCursor *)cur;

  if (pCur->dir_handle) {
    closedir(pCur->dir_handle);
  }

  sqlite3_free(pCur);
  return SQLITE_OK;
}

/*
 * xNext - Advance to the next row
 *
 * Each row represents one project (subdirectory in base_directory).
 * Iterates through directory entries until finding the next valid directory.
 *
 * TODO: Add regex pattern to filter project directories (e.g., exclude
 * non-project dirs)
 */
static int projects_next(sqlite3_vtab_cursor *cur) {
  ProjectsCursor *pCur = (ProjectsCursor *)cur;
  struct dirent *entry;

  while ((entry = readdir(pCur->dir_handle)) != NULL) {
    /* Skip . and .. */
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    /* Build full path */
    snprintf(pCur->current.path, sizeof(pCur->current.path), "%s/%s", pCur->base_path,
             entry->d_name);

    /* Check if it's a directory securely (ignore symlinks) */
    struct stat st;
    if (lstat(pCur->current.path, &st) == 0 && S_ISDIR(st.st_mode)) {
      /* Store project name */
      strncpy(pCur->current.name, entry->d_name, sizeof(pCur->current.name) - 1);
      pCur->current.name[sizeof(pCur->current.name) - 1] = '\0';

      /* Extract metadata we need */
      pCur->current.created_at = st.st_ctime;
      pCur->current.updated_at = st.st_mtime;

      pCur->rowid++;
      return SQLITE_OK;
    }
  }

  /* No more project directories */
  pCur->eof = 1;
  return SQLITE_OK;
}

/*
 * xFilter - Begin a search of the virtual table
 *
 * Parameters set by SQLite based on xBestIndex:
 *   idxNum - The plan ID we returned in xBestIndex
 *   idxStr - Optional string identifier (we don't use this)
 *   argc/argv - Constraint values from WHERE clause
 *
 * Phase 2: No constraints supported yet, parameters unused.
 * Phase 5: Will use idxNum to select optimized query plans.
 */
static int projects_filter(sqlite3_vtab_cursor *cur, int idxNum UNUSED, const char *idxStr UNUSED,
                           int argc UNUSED, sqlite3_value **argv UNUSED) {
  /* Phase 2: All parameters unused (no query optimization yet) */
  MARK_UNUSED(idxNum);
  MARK_UNUSED(idxStr);
  MARK_UNUSED(argc);
  MARK_UNUSED(argv);

  ProjectsCursor *pCur = (ProjectsCursor *)cur;
  ProjectsVTab *pTab = (ProjectsVTab *)cur->pVtab;

  /* Copy base path from vtab config */
  strncpy(pCur->base_path, pTab->config.base_directory, sizeof(pCur->base_path) - 1);
  pCur->base_path[sizeof(pCur->base_path) - 1] = '\0';

  /* Open directory */
  pCur->dir_handle = opendir(pCur->base_path);
  if (!pCur->dir_handle) {
    pCur->base.pVtab->zErrMsg = sqlite3_mprintf("Cannot open directory: %s (errno=%d, %s)",
                                                pCur->base_path, errno, strerror(errno));
    return SQLITE_ERROR;
  }

  pCur->rowid = 0;
  pCur->eof = 0;

  /* Advance to first valid entry */
  return projects_next(cur);
}

/*
 * xEof - Check if the cursor has reached the end
 */
static int projects_eof(sqlite3_vtab_cursor *cur) {
  ProjectsCursor *pCur = (ProjectsCursor *)cur;
  return pCur->eof;
}

/*
 * xColumn - Return a column value
 */
static int projects_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int col) {
  ProjectsCursor *pCur = (ProjectsCursor *)cur;

  switch (col) {
  case 0: /* project_id */
    sqlite3_result_text(ctx, pCur->current.name, -1, SQLITE_TRANSIENT);
    break;

  case 1: /* directory */
    sqlite3_result_text(ctx, pCur->current.path, -1, SQLITE_TRANSIENT);
    break;

  case 2: /* created_at */
    sqlite3_result_int64(ctx, pCur->current.created_at);
    break;

  case 3: /* updated_at */
    sqlite3_result_int64(ctx, pCur->current.updated_at);
    break;

  default:
    return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

/*
 * xRowid - Return the rowid
 */
static int projects_rowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid) {
  ProjectsCursor *pCur = (ProjectsCursor *)cur;
  *pRowid = pCur->rowid;
  return SQLITE_OK;
}

/*
 * Virtual table module definition
 */
sqlite3_module projects_module = {
    0,                   /* iVersion */
    projects_connect,    /* xCreate */
    projects_connect,    /* xConnect */
    projects_best_index, /* xBestIndex */
    projects_disconnect, /* xDisconnect */
    projects_disconnect, /* xDestroy */
    projects_open,       /* xOpen */
    projects_close,      /* xClose */
    projects_filter,     /* xFilter */
    projects_next,       /* xNext */
    projects_eof,        /* xEof */
    projects_column,     /* xColumn */
    projects_rowid,      /* xRowid */
    NULL,                /* xUpdate */
    NULL,                /* xBegin */
    NULL,                /* xSync */
    NULL,                /* xCommit */
    NULL,                /* xRollback */
    NULL,                /* xFindFunction */
    NULL,                /* xRename */
    NULL,                /* xSavepoint */
    NULL,                /* xRelease */
    NULL,                /* xRollbackTo */
    NULL,                /* xShadowName */
    NULL                 /* xIntegrity */
};

/*
 * Create the projects virtual table with the given base directory
 */
int create_projects_table(sqlite3 *db, const char *base_dir, char **pzErrMsg) {
  char sql[PATH_MAX + 256];

  snprintf(sql, sizeof(sql),
           "CREATE VIRTUAL TABLE IF NOT EXISTS projects "
           "USING claudecode_projects(base_directory='%s')",
           base_dir);

  return sqlite3_exec(db, sql, NULL, NULL, pzErrMsg);
}
