/*
 * Common utilities for Claude Code virtual tables
 */

#include "common.h"

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
