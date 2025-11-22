/*
 * ASan test harness for Claude Code extension
 *
 * Loads the extension and runs basic queries to detect memory errors.
 * Must be compiled with -fsanitize=address and linked against the
 * ASan-instrumented extension.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static int run_query(sqlite3 *db, const char *sql, int expect_error) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);

    if (expect_error) {
        if (rc == SQLITE_OK) {
            fprintf(stderr, "  FAIL: Expected error but got success for: %s\n", sql);
            return 1;
        }
        printf("  PASS: Correctly failed: %s\n", sql);
        sqlite3_free(err);
        return 0;
    }

    if (rc != SQLITE_OK) {
        fprintf(stderr, "  FAIL: %s\n", err ? err : "unknown error");
        sqlite3_free(err);
        return 1;
    }
    printf("  PASS: %s\n", sql);
    return 0;
}

int main(int argc, char **argv) {
    sqlite3 *db;
    char *err = NULL;
    int failures = 0;
    const char *ext_path = "./build/asan/claude_code.dylib";
    const char *projects_dir = getenv("CLAUDE_PROJECTS_DIR");

    if (argc > 1) {
        ext_path = argv[1];
    }

    printf("ASan Test Harness for Claude Code Extension\n");
    printf("Extension: %s\n", ext_path);
    printf("CLAUDE_PROJECTS_DIR: %s\n\n", projects_dir ? projects_dir : "(not set)");

    /* Open database */
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database\n");
        return 1;
    }

    /* Enable extensions */
    if (sqlite3_enable_load_extension(db, 1) != SQLITE_OK) {
        fprintf(stderr, "Cannot enable extensions\n");
        sqlite3_close(db);
        return 1;
    }

    /* Load extension */
    printf("Loading extension...\n");
    if (sqlite3_load_extension(db, ext_path, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Load failed: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_close(db);
        return 1;
    }
    printf("  PASS: Extension loaded\n\n");

    /* Test virtual tables exist */
    printf("Testing virtual table schemas...\n");
    failures += run_query(db, "SELECT * FROM projects LIMIT 0", 0);
    failures += run_query(db, "SELECT * FROM sessions LIMIT 0", 0);
    failures += run_query(db, "SELECT * FROM messages LIMIT 0", 0);
    printf("\n");

    /* Test read-only enforcement */
    printf("Testing read-only enforcement...\n");
    failures += run_query(db, "INSERT INTO projects VALUES (1,2,3,4,5,6)", 1);
    failures += run_query(db, "INSERT INTO sessions VALUES (1,2,3,4,5,6,7,8,9)", 1);
    failures += run_query(db, "INSERT INTO messages VALUES (1,2,3,4,5,6,7)", 1);
    printf("\n");

    sqlite3_close(db);

    printf("=================\n");
    if (failures == 0) {
        printf("All ASan tests passed\n");
        return 0;
    } else {
        printf("Tests failed: %d\n", failures);
        return 1;
    }
}
