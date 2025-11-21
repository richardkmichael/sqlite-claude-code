/*
 * Messages Virtual Table
 *
 * Provides read-only access to individual messages from Claude Code
 * session JSONL files. Each line in a .jsonl file becomes a row.
 */

#include <sqlite3ext.h>
extern const sqlite3_api_routines *sqlite3_api;  /* Defined in init.c */

#include "common.h"
#include "cJSON.h"

/* Maximum line buffer size (8MB for large JSON with tool outputs) */
#define MAX_LINE_BUFFER (8 * 1024 * 1024)

/*
 * Messages virtual table structure
 */
typedef struct MessagesVTab {
  sqlite3_vtab base;
  vtab_config config;
} MessagesVTab;

/*
 * Current message being returned
 */
typedef struct CurrentMessage {
  char uuid[64];           /* Message UUID */
  char session_id[64];     /* Session UUID */
  char type[32];           /* user, assistant, system */
  char timestamp[64];      /* ISO timestamp */
  char parent_uuid[64];    /* Parent message UUID (may be null) */
  char user_type[32];      /* external, etc. */
  char subtype[64];        /* For system messages: local_command, etc. */
  char *json_data;         /* Full JSON line (dynamically allocated) */
} CurrentMessage;

/*
 * Current session file being read
 */
typedef struct CurrentSessionFile {
  char id[64];             /* Session UUID (from filename) */
  char path[PATH_MAX];     /* Full path to .jsonl file */
  FILE *file;              /* Open file handle */
} CurrentSessionFile;

/*
 * Current project being scanned
 */
typedef struct CurrentProjectScan {
  char id[256];            /* Project ID (directory name) */
  char path[PATH_MAX];     /* Full path to project directory */
  DIR *sessions_dir;       /* Open directory handle */
} CurrentProjectScan;

/*
 * Messages cursor structure for iteration
 * Iterates through projects -> sessions -> lines
 */
typedef struct MessagesCursor {
  sqlite3_vtab_cursor base;
  char base_path[PATH_MAX];
  DIR *projects_dir;
  CurrentProjectScan project;
  CurrentSessionFile session;
  CurrentMessage message;
  char *line_buffer;       /* Dynamically allocated line buffer */
  int eof;
  sqlite3_int64 rowid;
} MessagesCursor;

/*
 * Helper: Check if filename matches exclusion pattern
 */
static int messages_is_excluded_file(const char *filename, const char *pattern) {
  if (!pattern || pattern[0] == '\0') {
    return 0;
  }

  size_t pattern_len = strlen(pattern);
  if (pattern[pattern_len - 1] == '*') {
    return strncmp(filename, pattern, pattern_len - 1) == 0;
  }

  return strcmp(filename, pattern) == 0;
}

/*
 * Helper: Parse a JSON line and extract message fields
 * Returns 0 on success, -1 on parse error
 */
static int parse_message_json(MessagesCursor *pCur, const char *json_line) {
  cJSON *root = cJSON_Parse(json_line);
  if (!root) {
    /* Malformed JSON - skip this record */
    return -1;
  }

  /* Clear previous values */
  pCur->message.uuid[0] = '\0';
  pCur->message.type[0] = '\0';
  pCur->message.timestamp[0] = '\0';
  pCur->message.parent_uuid[0] = '\0';
  pCur->message.user_type[0] = '\0';
  pCur->message.subtype[0] = '\0';

  cJSON *item;

  /* Extract uuid */
  item = cJSON_GetObjectItem(root, "uuid");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->message.uuid, item->valuestring, sizeof(pCur->message.uuid) - 1);
    pCur->message.uuid[sizeof(pCur->message.uuid) - 1] = '\0';
  }

  /* Extract type */
  item = cJSON_GetObjectItem(root, "type");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->message.type, item->valuestring, sizeof(pCur->message.type) - 1);
    pCur->message.type[sizeof(pCur->message.type) - 1] = '\0';
  }

  /* Extract timestamp */
  item = cJSON_GetObjectItem(root, "timestamp");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->message.timestamp, item->valuestring, sizeof(pCur->message.timestamp) - 1);
    pCur->message.timestamp[sizeof(pCur->message.timestamp) - 1] = '\0';
  }

  /* Extract parentUuid (may be null) */
  item = cJSON_GetObjectItem(root, "parentUuid");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->message.parent_uuid, item->valuestring, sizeof(pCur->message.parent_uuid) - 1);
    pCur->message.parent_uuid[sizeof(pCur->message.parent_uuid) - 1] = '\0';
  }

  /* Extract userType */
  item = cJSON_GetObjectItem(root, "userType");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->message.user_type, item->valuestring, sizeof(pCur->message.user_type) - 1);
    pCur->message.user_type[sizeof(pCur->message.user_type) - 1] = '\0';
  }

  /* Extract subtype (for system messages) */
  item = cJSON_GetObjectItem(root, "subtype");
  if (item && cJSON_IsString(item)) {
    strncpy(pCur->message.subtype, item->valuestring, sizeof(pCur->message.subtype) - 1);
    pCur->message.subtype[sizeof(pCur->message.subtype) - 1] = '\0';
  }

  /* Copy session_id from current session */
  strncpy(pCur->message.session_id, pCur->session.id, sizeof(pCur->message.session_id) - 1);
  pCur->message.session_id[sizeof(pCur->message.session_id) - 1] = '\0';

  /* Store full JSON (strip trailing newline if present) */
  if (pCur->message.json_data) {
    sqlite3_free(pCur->message.json_data);
    pCur->message.json_data = NULL;
  }
  size_t len = strlen(json_line);
  while (len > 0 && (json_line[len - 1] == '\n' || json_line[len - 1] == '\r')) {
    len--;
  }
  pCur->message.json_data = sqlite3_malloc((int)len + 1);
  if (pCur->message.json_data) {
    memcpy(pCur->message.json_data, json_line, len);
    pCur->message.json_data[len] = '\0';
  }

  cJSON_Delete(root);
  return 0;
}

/*
 * xConnect/xCreate - Create a new messages virtual table instance
 */
static int messages_connect(
  sqlite3 *db,
  void *pAux UNUSED,
  int argc,
  const char *const *argv,
  sqlite3_vtab **ppVTab,
  char **pzErr
) {
  MARK_UNUSED(pAux);

  MessagesVTab *pTab = sqlite3_malloc(sizeof(MessagesVTab));
  if (!pTab) {
    return SQLITE_NOMEM;
  }
  memset(pTab, 0, sizeof(MessagesVTab));

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
    "CREATE TABLE messages("
    "  message_id TEXT,"      /* UUID of this message */
    "  session_id TEXT,"      /* Foreign key to sessions */
    "  type TEXT,"            /* user, assistant, system */
    "  timestamp TEXT,"       /* ISO timestamp */
    "  parent_id TEXT,"       /* Parent message UUID */
    "  user_type TEXT,"       /* external, etc. */
    "  subtype TEXT,"         /* For system messages */
    "  json_data TEXT"        /* Full JSON record */
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
static int messages_disconnect(sqlite3_vtab *pVTab) {
  MessagesVTab *pTab = (MessagesVTab*)pVTab;
  sqlite3_free(pTab);
  return SQLITE_OK;
}

/*
 * xBestIndex - Determine the best way to execute a query
 *
 * Phase 4: Simple implementation - only full scan
 * Phase 5: Will add session_id and project_id constraint optimization
 */
static int messages_best_index(sqlite3_vtab *tab UNUSED, sqlite3_index_info *pIdxInfo) {
  MARK_UNUSED(tab);

  /* Plan ID 1: Full scan of all messages */
  pIdxInfo->idxNum = 1;

  /* Cost estimate: scanning all messages is expensive */
  pIdxInfo->estimatedCost = 10000000.0;
  pIdxInfo->estimatedRows = 100000;

  return SQLITE_OK;
}

/*
 * xOpen - Create a new cursor
 */
static int messages_open(sqlite3_vtab *pVTab UNUSED, sqlite3_vtab_cursor **ppCursor) {
  MARK_UNUSED(pVTab);

  MessagesCursor *pCur = sqlite3_malloc(sizeof(MessagesCursor));
  if (!pCur) {
    return SQLITE_NOMEM;
  }
  memset(pCur, 0, sizeof(MessagesCursor));

  /* Allocate line buffer */
  pCur->line_buffer = sqlite3_malloc(MAX_LINE_BUFFER);
  if (!pCur->line_buffer) {
    sqlite3_free(pCur);
    return SQLITE_NOMEM;
  }

  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

/*
 * xClose - Close a cursor
 */
static int messages_close(sqlite3_vtab_cursor *cur) {
  MessagesCursor *pCur = (MessagesCursor*)cur;

  if (pCur->session.file) {
    fclose(pCur->session.file);
  }
  if (pCur->project.sessions_dir) {
    closedir(pCur->project.sessions_dir);
  }
  if (pCur->projects_dir) {
    closedir(pCur->projects_dir);
  }
  if (pCur->line_buffer) {
    sqlite3_free(pCur->line_buffer);
  }
  if (pCur->message.json_data) {
    sqlite3_free(pCur->message.json_data);
  }

  sqlite3_free(pCur);
  return SQLITE_OK;
}

/*
 * Helper: Open the next session file in current project
 * Returns 1 if a file was opened, 0 if no more files
 */
static int open_next_session_file(MessagesCursor *pCur, MessagesVTab *pTab) {
  struct dirent *entry;

  while (pCur->project.sessions_dir &&
         (entry = readdir(pCur->project.sessions_dir)) != NULL) {
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
    if (messages_is_excluded_file(name, pTab->config.exclude_pattern)) {
      continue;
    }

    /* Build full path */
    snprintf(pCur->session.path, sizeof(pCur->session.path),
             "%s/%s", pCur->project.path, name);

    /* Open the file */
    pCur->session.file = fopen(pCur->session.path, "r");
    if (!pCur->session.file) {
      continue;  /* Skip if can't open */
    }

    /* Extract session ID from filename */
    extract_session_id(name, pCur->session.id, sizeof(pCur->session.id));

    return 1;
  }

  return 0;
}

/*
 * Helper: Open the next project directory
 * Returns 1 if a project was opened, 0 if no more projects
 */
static int open_next_project(MessagesCursor *pCur) {
  struct dirent *entry;

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

    /* Open sessions directory */
    pCur->project.sessions_dir = opendir(pCur->project.path);
    if (pCur->project.sessions_dir) {
      return 1;
    }
  }

  return 0;
}

/*
 * xNext - Advance to next message
 */
static int messages_next(sqlite3_vtab_cursor *cur) {
  MessagesCursor *pCur = (MessagesCursor*)cur;
  MessagesVTab *pTab = (MessagesVTab*)cur->pVtab;

  while (1) {
    /* Try to read next line from current file */
    if (pCur->session.file) {
      while (fgets(pCur->line_buffer, MAX_LINE_BUFFER, pCur->session.file)) {
        /* Skip empty lines */
        if (pCur->line_buffer[0] == '\n' || pCur->line_buffer[0] == '\0') {
          continue;
        }

        /* Parse the JSON */
        if (parse_message_json(pCur, pCur->line_buffer) == 0) {
          pCur->rowid++;
          return SQLITE_OK;
        }
        /* Parse failed, try next line */
      }

      /* EOF on current file */
      fclose(pCur->session.file);
      pCur->session.file = NULL;
    }

    /* Try to open next session file in current project */
    if (open_next_session_file(pCur, pTab)) {
      continue;
    }

    /* Close current project's sessions directory */
    if (pCur->project.sessions_dir) {
      closedir(pCur->project.sessions_dir);
      pCur->project.sessions_dir = NULL;
    }

    /* Try to open next project */
    if (open_next_project(pCur)) {
      continue;
    }

    /* No more projects - EOF */
    pCur->eof = 1;
    return SQLITE_OK;
  }
}

/*
 * xFilter - Begin scanning messages
 */
static int messages_filter(
  sqlite3_vtab_cursor *cur,
  int idxNum UNUSED,
  const char *idxStr UNUSED,
  int argc UNUSED,
  sqlite3_value **argv UNUSED
) {
  MARK_UNUSED(idxNum);
  MARK_UNUSED(idxStr);
  MARK_UNUSED(argc);
  MARK_UNUSED(argv);

  MessagesCursor *pCur = (MessagesCursor*)cur;
  MessagesVTab *pTab = (MessagesVTab*)cur->pVtab;

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

  /* Find first project */
  if (!open_next_project(pCur)) {
    pCur->eof = 1;
    return SQLITE_OK;
  }

  /* Find first session file */
  if (!open_next_session_file(pCur, pTab)) {
    /* No session files in first project, try advancing */
    closedir(pCur->project.sessions_dir);
    pCur->project.sessions_dir = NULL;
  }

  /* Advance to first message */
  return messages_next(cur);
}

/*
 * xEof
 */
static int messages_eof(sqlite3_vtab_cursor *cur) {
  MessagesCursor *pCur = (MessagesCursor*)cur;
  return pCur->eof;
}

/*
 * xColumn - Return column value
 */
static int messages_column(
  sqlite3_vtab_cursor *cur,
  sqlite3_context *ctx,
  int col
) {
  MessagesCursor *pCur = (MessagesCursor*)cur;

  switch (col) {
    case 0:  /* message_id */
      sqlite3_result_text(ctx, pCur->message.uuid, -1, SQLITE_TRANSIENT);
      break;

    case 1:  /* session_id */
      sqlite3_result_text(ctx, pCur->message.session_id, -1, SQLITE_TRANSIENT);
      break;

    case 2:  /* type */
      sqlite3_result_text(ctx, pCur->message.type, -1, SQLITE_TRANSIENT);
      break;

    case 3:  /* timestamp */
      sqlite3_result_text(ctx, pCur->message.timestamp, -1, SQLITE_TRANSIENT);
      break;

    case 4:  /* parent_id */
      if (pCur->message.parent_uuid[0] != '\0') {
        sqlite3_result_text(ctx, pCur->message.parent_uuid, -1, SQLITE_TRANSIENT);
      } else {
        sqlite3_result_null(ctx);
      }
      break;

    case 5:  /* user_type */
      sqlite3_result_text(ctx, pCur->message.user_type, -1, SQLITE_TRANSIENT);
      break;

    case 6:  /* subtype */
      if (pCur->message.subtype[0] != '\0') {
        sqlite3_result_text(ctx, pCur->message.subtype, -1, SQLITE_TRANSIENT);
      } else {
        sqlite3_result_null(ctx);
      }
      break;

    case 7:  /* json_data */
      if (pCur->message.json_data) {
        sqlite3_result_text(ctx, pCur->message.json_data, -1, SQLITE_TRANSIENT);
      } else {
        sqlite3_result_null(ctx);
      }
      break;

    default:
      return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

/*
 * xRowid
 */
static int messages_rowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid) {
  MessagesCursor *pCur = (MessagesCursor*)cur;
  *pRowid = pCur->rowid;
  return SQLITE_OK;
}

/*
 * Messages virtual table module
 */
sqlite3_module messages_module = {
  0,                      /* iVersion */
  messages_connect,       /* xCreate */
  messages_connect,       /* xConnect */
  messages_best_index,    /* xBestIndex */
  messages_disconnect,    /* xDisconnect */
  messages_disconnect,    /* xDestroy */
  messages_open,          /* xOpen */
  messages_close,         /* xClose */
  messages_filter,        /* xFilter */
  messages_next,          /* xNext */
  messages_eof,           /* xEof */
  messages_column,        /* xColumn */
  messages_rowid,         /* xRowid */
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
 * Create the messages virtual table with the given base directory
 */
int create_messages_table(sqlite3 *db, const char *base_dir, char **pzErrMsg) {
  char sql[PATH_MAX + 256];

  snprintf(sql, sizeof(sql),
           "CREATE VIRTUAL TABLE IF NOT EXISTS messages "
           "USING claudecode_messages(base_directory='%s')",
           base_dir);

  return sqlite3_exec(db, sql, NULL, NULL, pzErrMsg);
}
