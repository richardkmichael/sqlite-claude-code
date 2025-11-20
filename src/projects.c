/*
 * Claude Code Virtual Table Extension
 *
 * Provides virtual tables for querying Claude Code chat sessions:
 * - projects - directories containing chat sessions
 * - sessions - files (future)
 * - messages - records (future)
 */

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>

/*
 * Configuration structure for virtual table parameters
 * Supports flexible key=value arguments for future extensibility
 */
typedef struct vtab_config {
  char base_directory[PATH_MAX];
} vtab_config;

/*
 * Projects virtual table structure
 */
typedef struct ProjectsVTab {
  sqlite3_vtab base;
  vtab_config config;
} ProjectsVTab;

/*
 * Projects cursor structure for iteration
 */
typedef struct ProjectsCursor {
  sqlite3_vtab_cursor base;
  DIR *dir_handle;
  struct dirent *entry;
  char base_path[PATH_MAX];
  char current_path[PATH_MAX];
  struct stat st;
  int eof;
  sqlite3_int64 rowid;
} ProjectsCursor;

/*
 * Parse key=value argument
 * Returns 0 on success, -1 on error
 *
 * Handles both quoted and unquoted values:
 *   base_directory='/path/to/dir'
 *   base_directory=/path/to/dir
 */
static int parse_kv_argument(const char *arg, vtab_config *config) {
  const char *eq = strchr(arg, '=');
  if (!eq) {
    return -1;
  }

  size_t key_len = eq - arg;
  const char *value = eq + 1;

  /* Strip surrounding quotes if present */
  size_t value_len = strlen(value);
  if (value_len >= 2 && value[0] == '\'' && value[value_len - 1] == '\'') {
    value++;  /* Skip opening quote */
    value_len -= 2;  /* Remove both quotes from length */
  }

  if (strncmp(arg, "base_directory", key_len) == 0 && key_len == 14) {
    size_t copy_len = value_len < sizeof(config->base_directory) - 1 ?
                      value_len : sizeof(config->base_directory) - 1;
    strncpy(config->base_directory, value, copy_len);
    config->base_directory[copy_len] = '\0';
    return 0;
  }

  /* Unknown parameter - ignore for forward compatibility */
  return 0;
}

/*
 * xConnect/xCreate - Create a new virtual table instance
 */
static int projects_connect(
  sqlite3 *db,
  void *pAux,
  int argc,
  const char *const *argv,
  sqlite3_vtab **ppVTab,
  char **pzErr
) {
  (void)pAux;

  ProjectsVTab *pTab = sqlite3_malloc(sizeof(ProjectsVTab));
  if (!pTab) {
    return SQLITE_NOMEM;
  }
  memset(pTab, 0, sizeof(ProjectsVTab));

  /* Default configuration */
  const char *default_dir = getenv("CLAUDE_PROJECTS_DIR");
  if (!default_dir) {
    /* Default to $HOME/.claude/projects */
    const char *home = getenv("HOME");
    snprintf(pTab->config.base_directory, sizeof(pTab->config.base_directory),
             "%s/.claude/projects", home);
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
  int rc = sqlite3_declare_vtab(db,
    "CREATE TABLE projects("
    "  project_id TEXT,"     /* Directory name */
    "  directory TEXT,"      /* Full path */
    "  created_at INTEGER,"  /* ctime */
    "  updated_at INTEGER"   /* mtime */
    ")"
  );

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
  ProjectsVTab *pTab = (ProjectsVTab*)pVTab;
  sqlite3_free(pTab);
  return SQLITE_OK;
}

/*
 * xBestIndex - Determine the best way to execute a query
 * Phase 2: Simple implementation - always full scan
 */
static int projects_best_index(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
  (void)tab;

  pIdxInfo->idxNum = 1;  /* Plan #1: Full scan */
  pIdxInfo->estimatedCost = 100.0;  /* Cheap - few directories */
  pIdxInfo->estimatedRows = 50;

  return SQLITE_OK;
}

/*
 * xOpen - Create a new cursor
 */
static int projects_open(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor) {
  (void)pVTab;

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
  ProjectsCursor *pCur = (ProjectsCursor*)cur;

  if (pCur->dir_handle) {
    closedir(pCur->dir_handle);
  }

  sqlite3_free(pCur);
  return SQLITE_OK;
}

/*
 * xNext - Advance to the next row
 */
static int projects_next(sqlite3_vtab_cursor *cur) {
  ProjectsCursor *pCur = (ProjectsCursor*)cur;

  while ((pCur->entry = readdir(pCur->dir_handle)) != NULL) {
    /* Skip . and .. */
    if (strcmp(pCur->entry->d_name, ".") == 0 ||
        strcmp(pCur->entry->d_name, "..") == 0) {
      continue;
    }

    /* Build full path */
    snprintf(pCur->current_path, sizeof(pCur->current_path), "%s/%s",
             pCur->base_path, pCur->entry->d_name);

    /* Check if it's a directory */
    if (stat(pCur->current_path, &pCur->st) == 0 && S_ISDIR(pCur->st.st_mode)) {
      pCur->rowid++;
      return SQLITE_OK;
    }
  }

  /* No more directories */
  pCur->eof = 1;
  return SQLITE_OK;
}

/*
 * xFilter - Begin a search of the virtual table
 */
static int projects_filter(
  sqlite3_vtab_cursor *cur,
  int idxNum,
  const char *idxStr,
  int argc,
  sqlite3_value **argv
) {
  (void)idxNum;
  (void)idxStr;
  (void)argc;
  (void)argv;

  ProjectsCursor *pCur = (ProjectsCursor*)cur;
  ProjectsVTab *pTab = (ProjectsVTab*)cur->pVtab;

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
  ProjectsCursor *pCur = (ProjectsCursor*)cur;
  return pCur->eof;
}

/*
 * xColumn - Return a column value
 */
static int projects_column(
  sqlite3_vtab_cursor *cur,
  sqlite3_context *ctx,
  int col
) {
  ProjectsCursor *pCur = (ProjectsCursor*)cur;

  switch (col) {
    case 0:  /* project_id */
      sqlite3_result_text(ctx, pCur->entry->d_name, -1, SQLITE_TRANSIENT);
      break;

    case 1:  /* directory */
      sqlite3_result_text(ctx, pCur->current_path, -1, SQLITE_TRANSIENT);
      break;

    case 2:  /* created_at */
      sqlite3_result_int64(ctx, pCur->st.st_ctime);
      break;

    case 3:  /* updated_at */
      sqlite3_result_int64(ctx, pCur->st.st_mtime);
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
  ProjectsCursor *pCur = (ProjectsCursor*)cur;
  *pRowid = pCur->rowid;
  return SQLITE_OK;
}

/*
 * Virtual table module definition
 */
static sqlite3_module projects_module = {
  0,                      /* iVersion */
  projects_connect,       /* xCreate */
  projects_connect,       /* xConnect */
  projects_best_index,    /* xBestIndex */
  projects_disconnect,    /* xDisconnect */
  projects_disconnect,    /* xDestroy */
  projects_open,          /* xOpen */
  projects_close,         /* xClose */
  projects_filter,        /* xFilter */
  projects_next,          /* xNext */
  projects_eof,           /* xEof */
  projects_column,        /* xColumn */
  projects_rowid,         /* xRowid */
  NULL,                   /* xUpdate */
  NULL,                   /* xBegin */
  NULL,                   /* xSync */
  NULL,                   /* xCommit */
  NULL,                   /* xRollback */
  NULL,                   /* xFindFunction */
  NULL,                   /* xRename */
  NULL,                   /* xSavepoint */
  NULL,                   /* xRelease */
  NULL,                   /* xRollbackTo */
  NULL,                   /* xShadowName */
  NULL                    /* xIntegrity */
};

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
 *
 * Phase 2: Register projects virtual table module and auto-create table
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

  /* Register projects virtual table module */
  rc = sqlite3_create_module(db, "claudecode_projects", &projects_module, NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }

  /* Zero-setup: Auto-create projects table */
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
          "Default directory %s does not exist. Set CLAUDE_PROJECTS_DIR environment variable to specify projects directory.",
          default_dir);
      }
      return SQLITE_ERROR;
    }
  }

  char sql[PATH_MAX + 256];
  snprintf(sql, sizeof(sql),
           "CREATE VIRTUAL TABLE IF NOT EXISTS projects "
           "USING claudecode_projects(base_directory='%s')",
           default_dir);

  rc = sqlite3_exec(db, sql, NULL, NULL, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  return SQLITE_OK;
}
