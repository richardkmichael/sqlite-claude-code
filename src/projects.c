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
 * Portable unused parameter handling
 *
 * We need TWO macros because they serve different syntactic purposes:
 *
 *   UNUSED - Declaration attribute (goes in parameter list)
 *            Example: int func(int param UNUSED)
 *
 *   MARK_UNUSED(x) - Statement (goes in function body)
 *                    Example: MARK_UNUSED(param);
 *
 * Why both are needed:
 *   - On GCC/Clang: UNUSED does the work, MARK_UNUSED is a no-op
 *   - On MSVC: UNUSED is empty, MARK_UNUSED does the work via (void) cast
 *   - We always use both to keep code uniform across compilers
 *   - Alternative would be #ifdef blocks in every function (messy!)
 *
 * This approach keeps function signatures and bodies identical
 * across all platforms while adapting to compiler capabilities.
 *
 * Feature detection (__has_attribute) is preferred over compiler detection
 * to handle newer compilers and compiler variants more robustly.
 */
#if defined(__has_attribute)
  #if __has_attribute(unused)
    #define UNUSED __attribute__((unused))
    #define MARK_UNUSED(x) /* Attribute handles it */
  #else
    #define UNUSED
    #define MARK_UNUSED(x) (void)(x)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  /* Older GCC/Clang without __has_attribute */
  #define UNUSED __attribute__((unused))
  #define MARK_UNUSED(x)
#else
  /* MSVC and unknown compilers */
  #define UNUSED
  #define MARK_UNUSED(x) (void)(x)
#endif

/*
 * Configuration structure for virtual table parameters
 * Supports flexible key=value arguments for future extensibility
 */
typedef struct vtab_config {
  char base_directory[PATH_MAX];
  char exclude_pattern[256];
} vtab_config;

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
  char name[256];           /* Directory name (project_id) */
  char path[PATH_MAX];      /* Full path to directory */
  time_t created_at;        /* Creation time */
  time_t updated_at;        /* Modification time */
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
 * Helper: Safely copy string with bounds checking
 */
static void copy_config_string(char *dest, size_t dest_size,
                                 const char *src, size_t src_len) {
  size_t copy_len = src_len < dest_size - 1 ? src_len : dest_size - 1;
  strncpy(dest, src, copy_len);
  dest[copy_len] = '\0';
}

/*
 * Parse key=value argument
 * Returns 0 on success, -1 on error
 *
 * Handles both quoted and unquoted values:
 *   base_directory='/path/to/dir'
 *   base_directory=/path/to/dir
 *
 * Valid argument names:
 *   base_directory - Root directory containing project subdirectories
 *   exclude_pattern - Pattern for excluding files (e.g., 'agent-*')
 */
static int parse_kv_argument(const char *arg, vtab_config *config) {
  const char *eq = strchr(arg, '=');
  if (!eq) {
    return -1;
  }

  size_t key_len = eq - arg;
  const char *value = eq + 1;

  /* Strip surrounding single quotes from value if present */
  size_t value_len = strlen(value);
  if (value_len >= 2 && value[0] == '\'' && value[value_len - 1] == '\'') {
    value++;  /* Skip opening quote */
    value_len -= 2;  /* Remove both quotes from length */
  }

  if (strncmp(arg, "base_directory", key_len) == 0 && key_len == 14) {
    copy_config_string(config->base_directory, sizeof(config->base_directory),
                       value, value_len);
    return 0;
  } else if (strncmp(arg, "exclude_pattern", key_len) == 0 && key_len == 15) {
    copy_config_string(config->exclude_pattern, sizeof(config->exclude_pattern),
                       value, value_len);
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
  void *pAux UNUSED,
  int argc,
  const char *const *argv,
  sqlite3_vtab **ppVTab,
  char **pzErr
) {
  MARK_UNUSED(pAux);  /* Required by interface, unused in our implementation */

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
  MARK_UNUSED(tab);  /* Required by interface, unused for simple full scan */

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
  ProjectsCursor *pCur = (ProjectsCursor*)cur;

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
 * TODO: Add regex pattern to filter project directories (e.g., exclude non-project dirs)
 */
static int projects_next(sqlite3_vtab_cursor *cur) {
  ProjectsCursor *pCur = (ProjectsCursor*)cur;
  struct dirent *entry;

  while ((entry = readdir(pCur->dir_handle)) != NULL) {
    /* Skip . and .. */
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    /* Build full path */
    snprintf(pCur->current.path, sizeof(pCur->current.path), "%s/%s",
             pCur->base_path, entry->d_name);

    /* Check if it's a directory and get metadata */
    struct stat st;
    if (stat(pCur->current.path, &st) == 0 && S_ISDIR(st.st_mode)) {
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
static int projects_filter(
  sqlite3_vtab_cursor *cur,
  int idxNum UNUSED,
  const char *idxStr UNUSED,
  int argc UNUSED,
  sqlite3_value **argv UNUSED
) {
  /* Phase 2: All parameters unused (no query optimization yet) */
  MARK_UNUSED(idxNum);
  MARK_UNUSED(idxStr);
  MARK_UNUSED(argc);
  MARK_UNUSED(argv);

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
      sqlite3_result_text(ctx, pCur->current.name, -1, SQLITE_TRANSIENT);
      break;

    case 1:  /* directory */
      sqlite3_result_text(ctx, pCur->current.path, -1, SQLITE_TRANSIENT);
      break;

    case 2:  /* created_at */
      sqlite3_result_int64(ctx, pCur->current.created_at);
      break;

    case 3:  /* updated_at */
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

/* ========================================================================
 * SESSIONS VIRTUAL TABLE
 * TODO: Refactor into separate files after Phase 4 (common.c, projects.c, sessions.c, messages.c)
 * ======================================================================== */

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
 * Helper: Extract session ID from JSONL filename
 * Filename format: "<uuid>.jsonl"
 */
static void extract_session_id(const char *filename, char *session_id, size_t size) {
  const char *dot = strrchr(filename, '.');
  if (dot && strcmp(dot, ".jsonl") == 0) {
    size_t id_len = dot - filename;
    if (id_len < size) {
      strncpy(session_id, filename, id_len);
      session_id[id_len] = '\0';
      return;
    }
  }
  session_id[0] = '\0';
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
 * SQLite calls this to ask: "How would you execute this query?"
 * We respond with:
 *   idxNum - An arbitrary integer identifying our chosen query plan
 *            (we pick the value, SQLite just passes it back to xFilter)
 *   estimatedCost - Relative cost (SQLite compares across plans)
 *   estimatedRows - Expected number of rows returned
 *
 * Phase 3: Simple implementation - only one plan (full scan)
 * Phase 5: Will add optimized plans for session_id/project_id filters
 */
static int sessions_best_index(sqlite3_vtab *tab UNUSED, sqlite3_index_info *pIdxInfo) {
  MARK_UNUSED(tab);  /* Required by interface, unused for simple full scan */

  /* Plan ID 1: Full scan of all session files (our only plan for now) */
  pIdxInfo->idxNum = 1;

  /* Cost estimate: scanning ~500 session files is moderate work */
  pIdxInfo->estimatedCost = 5000.0;
  pIdxInfo->estimatedRows = 500;

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

        /* Get file stats */
        struct stat st;
        if (stat(pCur->session.path, &st) != 0) {
          continue;  /* Skip if stat fails */
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

      /* Open sessions directory for this project */
      pCur->project.sessions_dir = opendir(pCur->project.path);
      if (pCur->project.sessions_dir) {
        /* Found a valid project, restart loop to scan its sessions */
        break;
      }
    }

    /* If we didn't find another project, we're done */
    if (!pCur->project.sessions_dir) {
      pCur->eof = 1;
      return SQLITE_OK;
    }
  }
}

/*
 * xFilter - Begin scanning sessions
 *
 * Parameters set by SQLite based on xBestIndex:
 *   idxNum - The plan ID we returned in xBestIndex
 *   idxStr - Optional string identifier (we don't use this)
 *   argc/argv - Constraint values from WHERE clause
 *
 * Phase 3: No constraints supported yet, parameters unused.
 * Phase 5: Will use idxNum to select optimized query plans.
 */
static int sessions_filter(
  sqlite3_vtab_cursor *cur,
  int idxNum UNUSED,
  const char *idxStr UNUSED,
  int argc UNUSED,
  sqlite3_value **argv UNUSED
) {
  /* Phase 3: All parameters unused (no query optimization yet) */
  MARK_UNUSED(idxNum);
  MARK_UNUSED(idxStr);
  MARK_UNUSED(argc);
  MARK_UNUSED(argv);

  SessionsCursor *pCur = (SessionsCursor*)cur;
  SessionsVTab *pTab = (SessionsVTab*)cur->pVtab;

  /* Copy base path */
  strncpy(pCur->base_path, pTab->config.base_directory, sizeof(pCur->base_path) - 1);
  pCur->base_path[sizeof(pCur->base_path) - 1] = '\0';

  /* Open projects directory */
  pCur->projects_dir = opendir(pCur->base_path);
  if (!pCur->projects_dir) {
    pCur->base.pVtab->zErrMsg = sqlite3_mprintf("Cannot open directory: %s", pCur->base_path);
    return SQLITE_ERROR;
  }

  pCur->rowid = 0;
  pCur->eof = 0;

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
static sqlite3_module sessions_module = {
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

  /* Register sessions virtual table module */
  rc = sqlite3_create_module(db, "claudecode_sessions", &sessions_module, NULL);
  if (rc != SQLITE_OK) {
    return rc;
  }

  /* Zero-setup: Auto-create projects and sessions tables */
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

  /* Create projects table */
  snprintf(sql, sizeof(sql),
           "CREATE VIRTUAL TABLE IF NOT EXISTS projects "
           "USING claudecode_projects(base_directory='%s')",
           default_dir);

  rc = sqlite3_exec(db, sql, NULL, NULL, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  /* Create sessions table */
  snprintf(sql, sizeof(sql),
           "CREATE VIRTUAL TABLE IF NOT EXISTS sessions "
           "USING claudecode_sessions(base_directory='%s')",
           default_dir);

  rc = sqlite3_exec(db, sql, NULL, NULL, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  return SQLITE_OK;
}
