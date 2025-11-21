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
#include <limits.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <sys/stat.h>
  #include <fcntl.h>

  /* Constants and Types */
  #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
  #endif
  #ifndef S_ISDIR
    #define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
  #endif
  #ifndef S_ISREG
    #define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
  #endif
  #ifndef S_ISLNK
    /* Windows _stat doesn't report S_IFLNK, we handle this via lstat wrapper */
    #define S_ISLNK(mode) (((mode) & S_IFMT) == 0xA000) /* Custom flag for our shim */
  #endif

  #define O_NOFOLLOW 0 /* Not supported in _open, handled differently or ignored */
  
  /* dirent.h Shim for MSVC */
  typedef struct dirent {
    char d_name[PATH_MAX];
  } dirent;

  typedef struct DIR {
    HANDLE hFind;
    WIN32_FIND_DATAA data;
    struct dirent ent;
    int first_read;
    char dir_path[PATH_MAX]; /* stored for stat checks since no dirfd */
  } DIR;

  DIR *opendir(const char *name);
  struct dirent *readdir(DIR *dir);
  int closedir(DIR *dir);
  
  /* 
   * Windows lacks dirfd. We'll define a macro that returns -1 
   * and handle it in the calling code.
   */
  #define dirfd(d) (-1)

  /* Function Mappings */
  #define open _open
  #define close _close
  #define fdopen _fdopen
  #define fstat _fstat
  #define stat _stat
  
  /* lstat Shim Prototype */
  int win32_lstat(const char *path, struct stat *buf);
  #define lstat win32_lstat

#else
  /* POSIX Headers */
  #include <dirent.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* 
 * Validate a directory securely.
 * On POSIX: Uses fstat(dirfd(dir)) to ensure the open directory handle matches.
 * On Windows: Fallback to stat(path) as dirfd isn't available.
 */
int validate_directory(DIR *dir, const char *path);


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
