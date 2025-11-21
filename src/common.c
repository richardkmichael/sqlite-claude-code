/*
 * Common utilities for Claude Code virtual tables
 */

#include "common.h"

#ifdef _WIN32
/*
 * Windows Compatibility Layer Implementation
 */

DIR *opendir(const char *name) {
  DIR *dir = (DIR *)malloc(sizeof(DIR));
  if (!dir) {
    errno = ENOMEM;
    return NULL;
  }

  snprintf(dir->dir_path, PATH_MAX, "%s\\*", name);
  
  dir->hFind = FindFirstFileA(dir->dir_path, &dir->data);
  if (dir->hFind == INVALID_HANDLE_VALUE) {
    free(dir);
    return NULL;
  }
  
  /* Store original path for validation */
  strncpy(dir->dir_path, name, PATH_MAX - 1);
  dir->dir_path[PATH_MAX - 1] = '\0';
  
  dir->first_read = 1;
  return dir;
}

struct dirent *readdir(DIR *dir) {
  if (!dir) return NULL;

  if (dir->first_read) {
    dir->first_read = 0;
    /* First file is already found by FindFirstFile */
  } else {
    if (!FindNextFileA(dir->hFind, &dir->data)) {
      return NULL;
    }
  }

  strncpy(dir->ent.d_name, dir->data.cFileName, PATH_MAX - 1);
  dir->ent.d_name[PATH_MAX - 1] = '\0';
  return &dir->ent;
}

int closedir(DIR *dir) {
  if (!dir) return -1;
  if (dir->hFind != INVALID_HANDLE_VALUE) {
    FindClose(dir->hFind);
  }
  free(dir);
  return 0;
}

int win32_lstat(const char *path, struct stat *buf) {
  DWORD attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return -1;
  }

  /* Perform standard stat to get sizes/times */
  if (_stat(path, (struct _stat *)buf) != 0) {
    return -1;
  }

  /* If it's a reparse point, mark it as a link in our custom mode */
  if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
    /* Clear other type bits and set custom LINK flag */
    buf->st_mode &= ~S_IFMT;
    buf->st_mode |= 0xA000; /* S_IFLNK replacement */
  }

  return 0;
}

#endif /* _WIN32 */

/*
 * Validate a directory securely (or as securely as possible on Windows)
 */
int validate_directory(DIR *dir, const char *path UNUSED) {
  struct stat st;

#ifdef _WIN32
  /* Windows: Fallback to path-based check (TOCTOU risk accepted due to OS limits) */
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
    return 1;
  }
#else
  /* POSIX: Secure check using file descriptor */
  if (dir && fstat(dirfd(dir), &st) == 0 && S_ISDIR(st.st_mode)) {
    return 1;
  }
#endif

  return 0;
}

/*
 * Helper: Safely copy string with bounds checking
 */
void copy_config_string(char *dest, size_t dest_size,
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
int parse_kv_argument(const char *arg, vtab_config *config) {
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

  #define KEY_MATCHES(literal) \
    (key_len == (sizeof(literal) - 1) && strncmp(arg, literal, key_len) == 0)

  if (KEY_MATCHES("base_directory")) {
    copy_config_string(config->base_directory, sizeof(config->base_directory),
                       value, value_len);
    return 0;
  } else if (KEY_MATCHES("exclude_pattern")) {
    copy_config_string(config->exclude_pattern, sizeof(config->exclude_pattern),
                       value, value_len);
    return 0;
  }

  #undef KEY_MATCHES

  /* Unknown parameter - ignore for forward compatibility */
  return 0;
}

/*
 * Helper: Extract session ID from JSONL filename
 * Filename format: "<uuid>.jsonl"
 */
void extract_session_id(const char *filename, char *session_id, size_t size) {
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
