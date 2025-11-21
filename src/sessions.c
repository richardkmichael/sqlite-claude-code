/*
 * Sessions Virtual Table
 *
 * Provides read-only access to Claude Code session files (.jsonl)
 * organized by project directories.
 */

#include <sqlite3ext.h>
extern const sqlite3_api_routines *sqlite3_api;  /* Defined in init.c */

#include "common.h"

/* Query plan constants */
#define PLAN_FULL_SCAN       1
#define PLAN_PROJECT_FILTER  2

/* Column indices (must match schema order) */
#define COL_SESSION_ID  0
#define COL_PROJECT_ID  1
#define COL_FILE_PATH   2
#define COL_CREATED_AT  3
#define COL_UPDATED_AT  4

/*
 * Sessions virtual table structure
 */
typedef struct SessionsVTab {
  sqlite3_vtab base;
  vtab_config config;
} SessionsVTab;

/*
 * Current session information
 * Organizes all data about the session we're currently pointing at
 */
typedef struct CurrentSession {
  char id[64];              /* Session UUID (from filename) */
  char path[PATH_MAX];      /* Full path to .jsonl file */
  time_t created_at;        /* File creation time */
  time_t updated_at;        /* File modification time */
} CurrentSession;

/*
 * Current project being scanned for sessions
 */
typedef struct CurrentProjectScan {
  char id[256];             /* Project ID (directory name) */
  char path[PATH_MAX];      /* Full path to project directory */
  DIR *sessions_dir;        /* Open directory handle for sessions */
} CurrentProjectScan;

/*
 * Sessions cursor structure for iteration
 * Iterates through projects -> JSONL files
 */
typedef struct SessionsCursor {
  sqlite3_vtab_cursor base;
  char base_path[PATH_MAX];
  DIR *projects_dir;
  CurrentProjectScan project;
  CurrentSession session;
  char filter_project_id[256];  /* Project ID filter (empty = no filter) */
  int eof;
  sqlite3_int64 rowid;
} SessionsCursor;

/*
 * Helper: Check if filename matches exclusion pattern
 * Simple prefix matching for now (e.g., "agent-" prefix)
 */
static int is_excluded_file(const char *filename, const char *pattern) {
  if (!pattern || pattern[0] == '\0') {
    return 0;  /* No exclusion pattern */
  }

  /* Simple prefix matching */
  size_t pattern_len = strlen(pattern);
  if (pattern[pattern_len - 1] == '*') {
    /* Prefix pattern like "agent-*" */
    return strncmp(filename, pattern, pattern_len - 1) == 0;
  }

  /* Exact match */
  return strcmp(filename, pattern) == 0;
}

/*
 * xConnect/xCreate - Create a new sessions virtual table instance
 */
static int sessions_connect(
  sqlite3 *db,
  void *pAux UNUSED,
  int argc,
  const char *const *argv,
  sqlite3_vtab **ppVTab,
  char **pzErr
) {
  MARK_UNUSED(pAux);  /* Required by interface, unused in our implementation */

  SessionsVTab *pTab = sqlite3_malloc(sizeof(SessionsVTab));
  if (!pTab) {
    return SQLITE_NOMEM;
  }
  memset(pTab, 0, sizeof(SessionsVTab));

  /* Default configuration */
  const char *default_dir = getenv("CLAUDE_PROJECTS_DIR");
  if (!default_dir) {
    const char *home = getenv("HOME");
    snprintf(pTab->config.base_directory, sizeof(pTab->config.base_directory),
             "%s/.claude/projects", home);
  } else {
    strncpy(pTab->config.base_directory, default_dir, sizeof(pTab->config.base_directory) - 1);
    pTab->config.base_directory[sizeof(pTab->config.base_directory) - 1] = '\0';
  }

  /* Default exclusion pattern */
  copy_config_string(pTab->config.exclude_pattern, sizeof(pTab->config.exclude_pattern),
                     "agent-*", 7);

  /* Parse arguments */
  for (int i = 3; i < argc; i++) {
    if (parse_kv_argument(argv[i], &pTab->config) != 0) {
      *pzErr = sqlite3_mprintf("Invalid argument: %s", argv[i]);
      sqlite3_free(pTab);
      return SQLITE_ERROR;
    }
  }

  /* Declare table schema */
  int rc = sqlite3_declare_vtab(db,
    "CREATE TABLE sessions("
    "  session_id TEXT,"     /* UUID from filename */
    "  project_id TEXT,"     /* Foreign key to projects */
    "  file_path TEXT,"      /* Full path to .jsonl file */
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
 * xDisconnect/xDestroy
 */
static int sessions_disconnect(sqlite3_vtab *pVTab) {
  SessionsVTab *pTab = (SessionsVTab*)pVTab;
  sqlite3_free(pTab);
  return SQLITE_OK;
}

/*
 * xBestIndex - Determine the best way to execute a query
 *
 * Detects constraints in WHERE clause and chooses optimal query plan:
 *   PLAN_PROJECT_FILTER: project_id = ? -> scan single project directory (fast)
 *   PLAN_FULL_SCAN: no constraints -> scan all projects (expensive)
 */
static int sessions_best_index(sqlite3_vtab *tab UNUSED, sqlite3_index_info *pIdxInfo) {
  MARK_UNUSED(tab);

  int project_constraint_idx = -1;

  /* Scan constraints from WHERE clause */
  for (int i = 0; i < pIdxInfo->nConstraint; i++) {
    if (!pIdxInfo->aConstraint[i].usable) {
      continue;
    }

    /* Look for equality constraints on project_id */
    if (pIdxInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ) {
      int col = pIdxInfo->aConstraint[i].iColumn;

      if (col == COL_PROJECT_ID) {
        project_constraint_idx = i;
      }
    }
  }

  /* Choose best plan based on available constraints */
  if (project_constraint_idx != -1) {
    /* Filter by project_id - scan only one project directory */
    pIdxInfo->idxNum = PLAN_PROJECT_FILTER;
    pIdxInfo->aConstraintUsage[project_constraint_idx].argvIndex = 1;
    pIdxInfo->aConstraintUsage[project_constraint_idx].omit = 1;
    pIdxInfo->estimatedCost = 100.0;   /* Scan one directory */
    pIdxInfo->estimatedRows = 50;
  } else {
    /* Full scan of all session files */
    pIdxInfo->idxNum = PLAN_FULL_SCAN;
    pIdxInfo->estimatedCost = 5000.0;
    pIdxInfo->estimatedRows = 500;
  }

  return SQLITE_OK;
}

/*
 * xOpen - Create a new cursor
 */
static int sessions_open(sqlite3_vtab *pVTab UNUSED, sqlite3_vtab_cursor **ppCursor) {
  MARK_UNUSED(pVTab);

  SessionsCursor *pCur = sqlite3_malloc(sizeof(SessionsCursor));
  if (!pCur) {
    return SQLITE_NOMEM;
  }
  memset(pCur, 0, sizeof(SessionsCursor));

  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

/*
 * xClose - Close a cursor
 */
static int sessions_close(sqlite3_vtab_cursor *cur) {
  SessionsCursor *pCur = (SessionsCursor*)cur;

  if (pCur->project.sessions_dir) {
    closedir(pCur->project.sessions_dir);
  }
  if (pCur->projects_dir) {
    closedir(pCur->projects_dir);
  }

  sqlite3_free(pCur);
  return SQLITE_OK;
}

/*
 * xNext - Advance to next session file
 */
static int sessions_next(sqlite3_vtab_cursor *cur) {
  SessionsCursor *pCur = (SessionsCursor*)cur;
  SessionsVTab *pTab = (SessionsVTab*)cur->pVtab;
  struct dirent *entry;

  while (1) {
    /* Try to find next session file in current project */
    if (pCur->project.sessions_dir) {
      while ((entry = readdir(pCur->project.sessions_dir)) != NULL) {
        const char *name = entry->d_name;

        /* Skip . and .. */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
          continue;
        }

        /* Check if it's a .jsonl file */
        size_t name_len = strlen(name);
        if (name_len < 6 || strcmp(name + name_len - 6, ".jsonl") != 0) {
          continue;
        }

        /* Check exclusion pattern */
        if (is_excluded_file(name, pTab->config.exclude_pattern)) {
          continue;
        }

        /* Build full path */
        snprintf(pCur->session.path, sizeof(pCur->session.path),
                 "%s/%s", pCur->project.path, name);

        /* Get file stats (lstat to detect symlinks) */
        struct stat st;
        if (lstat(pCur->session.path, &st) != 0) {
          continue;  /* Skip if stat fails */
        }

        /* Security: Ignore symbolic links to prevent arbitrary file metadata leak */
        if (S_ISLNK(st.st_mode)) {
          continue;
        }

        /* Extract session ID from filename */
        extract_session_id(name, pCur->session.id, sizeof(pCur->session.id));

        /* Extract metadata we need */
        pCur->session.created_at = st.st_ctime;
        pCur->session.updated_at = st.st_mtime;

        pCur->rowid++;
        return SQLITE_OK;
      }

      /* No more sessions in this project, close directory */
      closedir(pCur->project.sessions_dir);
      pCur->project.sessions_dir = NULL;
    }

    /* Move to next project */
    while ((entry = readdir(pCur->projects_dir)) != NULL) {
      const char *name = entry->d_name;

      /* Skip . and .. */
      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        continue;
      }

      /* Build project path */
      snprintf(pCur->project.path, sizeof(pCur->project.path),
               "%s/%s", pCur->base_path, name);

      /* Check if it's a directory */
      struct stat st;
      if (stat(pCur->project.path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        continue;
      }

      /* Store project_id */
      copy_config_string(pCur->project.id, sizeof(pCur->project.id),
                         name, strlen(name));

      /* Open sessions directory securely */
      DIR *dir = opendir(pCur->project.path);
      if (dir) {
        /* Verify it's a real directory */
        if (validate_directory(dir, pCur->project.path)) {
          pCur->project.sessions_dir = dir;
          /* Found a valid project, restart loop to scan its sessions */
          break;
        }
        closedir(dir);
      }
    }

    /* If we didn't find another project, we're done */
    if (!pCur->project.sessions_dir) {
      pCur->eof = 1;
      return SQLITE_OK;
    }

    /* If filtering by project_id, we shouldn't reach here (only one project) */
    if (pCur->filter_project_id[0] != '\0') {
      pCur->eof = 1;
      return SQLITE_OK;
    }
  }
}

/*
 * Helper: Open a specific project directory by project_id
 * Returns 1 if found and opened, 0 if not found
 */
static int filter_by_project(SessionsCursor *pCur, const char *project_id) {
  struct dirent *entry;

  /* Store filter for later reference */
  strncpy(pCur->filter_project_id, project_id, sizeof(pCur->filter_project_id) - 1);
  pCur->filter_project_id[sizeof(pCur->filter_project_id) - 1] = '\0';

  /* Search for the target project directory */
  while ((entry = readdir(pCur->projects_dir)) != NULL) {
    const char *name = entry->d_name;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }

    /* Check if this is the project we want */
    if (strcmp(name, project_id) != 0) {
      continue;
    }

    /* Build project path */
    snprintf(pCur->project.path, sizeof(pCur->project.path),
             "%s/%s", pCur->base_path, name);

    /* Check if it's a directory */
    struct stat st;
    if (stat(pCur->project.path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }

    /* Store project_id */
    copy_config_string(pCur->project.id, sizeof(pCur->project.id),
                       name, strlen(name));

    /* Open sessions directory securely */
    DIR *dir = opendir(pCur->project.path);
    if (dir) {
      /* Verify it's a real directory */
      if (validate_directory(dir, pCur->project.path)) {
        pCur->project.sessions_dir = dir;
        return 1;  /* Success */
      }
      closedir(dir);
    }
  }

  /* Project not found */
  pCur->eof = 1;
  return 0;
}

/*
 * xFilter - Begin scanning sessions
 *
 * Parameters set by SQLite based on xBestIndex:
 *   idxNum - The plan ID we returned in xBestIndex
 *   idxStr - Optional string identifier (we don't use this)
 *   argc/argv - Constraint values from WHERE clause
 *
 * Supports two plans:
 *   PLAN_FULL_SCAN - Scan all project directories
 *   PLAN_PROJECT_FILTER - Scan only matching project_id directory
 */
static int sessions_filter(
  sqlite3_vtab_cursor *cur,
  int idxNum,
  const char *idxStr UNUSED,
  int argc,
  sqlite3_value **argv
) {
  MARK_UNUSED(idxStr);  /* We use idxNum, not idxStr for plan selection */

  SessionsCursor *pCur = (SessionsCursor*)cur;
  SessionsVTab *pTab = (SessionsVTab*)cur->pVtab;

  /* Copy base path */
  strncpy(pCur->base_path, pTab->config.base_directory, sizeof(pCur->base_path) - 1);
  pCur->base_path[sizeof(pCur->base_path) - 1] = '\0';

  /* Clear filter state */
  pCur->filter_project_id[0] = '\0';

  /* Open projects directory */
  pCur->projects_dir = opendir(pCur->base_path);
  if (!pCur->projects_dir) {
    pCur->base.pVtab->zErrMsg = sqlite3_mprintf("Cannot open directory: %s", pCur->base_path);
    return SQLITE_ERROR;
  }

  pCur->rowid = 0;
  pCur->eof = 0;

  /* Handle different query plans */
  if (idxNum == PLAN_PROJECT_FILTER && argc >= 1) {
    /* Filter by project_id - jump directly to target project */
    const char *project_id = (const char *)sqlite3_value_text(argv[0]);
    if (project_id && project_id[0] != '\0') {
      if (!filter_by_project(pCur, project_id)) {
        return SQLITE_OK;  /* Project not found, eof already set */
      }
    }
  }

  /* Advance to first session */
  return sessions_next(cur);
}

/*
 * xEof
 */
static int sessions_eof(sqlite3_vtab_cursor *cur) {
  SessionsCursor *pCur = (SessionsCursor*)cur;
  return pCur->eof;
}

/*
 * xColumn - Return column value
 */
static int sessions_column(
  sqlite3_vtab_cursor *cur,
  sqlite3_context *ctx,
  int col
) {
  SessionsCursor *pCur = (SessionsCursor*)cur;

  switch (col) {
    case 0:  /* session_id */
      sqlite3_result_text(ctx, pCur->session.id, -1, SQLITE_TRANSIENT);
      break;

    case 1:  /* project_id */
      sqlite3_result_text(ctx, pCur->project.id, -1, SQLITE_TRANSIENT);
      break;

    case 2:  /* file_path */
      sqlite3_result_text(ctx, pCur->session.path, -1, SQLITE_TRANSIENT);
      break;

    case 3:  /* created_at */
      sqlite3_result_int64(ctx, pCur->session.created_at);
      break;

    case 4:  /* updated_at */
      sqlite3_result_int64(ctx, pCur->session.updated_at);
      break;

    default:
      return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

/*
 * xRowid
 */
static int sessions_rowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid) {
  SessionsCursor *pCur = (SessionsCursor*)cur;
  *pRowid = pCur->rowid;
  return SQLITE_OK;
}

/*
 * Sessions virtual table module
 */
sqlite3_module sessions_module = {
  0,                      /* iVersion */
  sessions_connect,       /* xCreate */
  sessions_connect,       /* xConnect */
  sessions_best_index,    /* xBestIndex */
  sessions_disconnect,    /* xDisconnect */
  sessions_disconnect,    /* xDestroy */
  sessions_open,          /* xOpen */
  sessions_close,         /* xClose */
  sessions_filter,        /* xFilter */
  sessions_next,          /* xNext */
  sessions_eof,           /* xEof */
  sessions_column,        /* xColumn */
  sessions_rowid,         /* xRowid */
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

/*
 * Create the sessions virtual table with the given base directory
 */
int create_sessions_table(sqlite3 *db, const char *base_dir, char **pzErrMsg) {
  char sql[PATH_MAX + 256];

  snprintf(sql, sizeof(sql),
           "CREATE VIRTUAL TABLE IF NOT EXISTS sessions "
           "USING claudecode_sessions(base_directory='%s')",
           base_dir);

  return sqlite3_exec(db, sql, NULL, NULL, pzErrMsg);
}
