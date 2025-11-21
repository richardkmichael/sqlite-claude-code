/*
 * Common definitions and utilities for Claude Code virtual tables
 *
 * Shared across projects, sessions, and messages tables.
 */

#ifndef CLAUDE_CODE_COMMON_H
#define CLAUDE_CODE_COMMON_H

#include <sqlite3ext.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

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

/* Function declarations (implementations in common.c) */

/*
 * Helper: Safely copy string with bounds checking
 */
void copy_config_string(char *dest, size_t dest_size,
                        const char *src, size_t src_len);

/*
 * Parse key=value argument
 * Returns 0 on success, -1 on error
 */
int parse_kv_argument(const char *arg, vtab_config *config);

/*
 * Helper: Extract session ID from JSONL filename
 * Filename format: "<uuid>.jsonl"
 */
void extract_session_id(const char *filename, char *session_id, size_t size);

/* External module declarations */
extern sqlite3_module projects_module;
extern sqlite3_module sessions_module;
extern sqlite3_module messages_module;

/* Table creation functions (defined in each domain file) */
int create_projects_table(sqlite3 *db, const char *base_dir, char **pzErrMsg);
int create_sessions_table(sqlite3 *db, const char *base_dir, char **pzErrMsg);
int create_messages_table(sqlite3 *db, const char *base_dir, char **pzErrMsg);

/* SQL helper functions (defined in functions.c) */
int register_helper_functions(sqlite3 *db);
int create_functions_metadata_table(sqlite3 *db, char **pzErrMsg);

#endif /* CLAUDE_CODE_COMMON_H */
