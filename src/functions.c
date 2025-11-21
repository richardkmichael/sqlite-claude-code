/*
 * SQL Helper Functions
 *
 * Convenience functions for extracting data from message JSON.
 * These parse JSON on every call (no caching, per design decision).
 */

#include <sqlite3ext.h>
extern const sqlite3_api_routines *sqlite3_api;  /* Defined in init.c */

#include "common.h"
#include "cJSON.h"

/*
 * get_message_content(json_text) -> TEXT
 *
 * Extract message.content, handling both string and array formats.
 * For arrays, returns the first text block's content.
 */
static void get_message_content_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  const char *json = (const char *)sqlite3_value_text(argv[0]);

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    sqlite3_result_null(ctx);
    return;
  }

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (message) {
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content) {
      if (cJSON_IsString(content)) {
        sqlite3_result_text(ctx, content->valuestring, -1, SQLITE_TRANSIENT);
        cJSON_Delete(root);
        return;
      } else if (cJSON_IsArray(content)) {
        /* Extract text from array elements */
        int array_size = cJSON_GetArraySize(content);
        for (int i = 0; i < array_size; i++) {
          cJSON *item = cJSON_GetArrayItem(content, i);
          cJSON *type = cJSON_GetObjectItem(item, "type");
          if (type && cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (text && cJSON_IsString(text)) {
              sqlite3_result_text(ctx, text->valuestring, -1, SQLITE_TRANSIENT);
              cJSON_Delete(root);
              return;
            }
          }
        }
      }
    }
  }

  sqlite3_result_null(ctx);
  cJSON_Delete(root);
}

/*
 * get_message_role(json_text) -> TEXT
 *
 * Extract message.role (user, assistant, system).
 */
static void get_message_role_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  const char *json = (const char *)sqlite3_value_text(argv[0]);

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    sqlite3_result_null(ctx);
    return;
  }

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (message) {
    cJSON *role = cJSON_GetObjectItem(message, "role");
    if (role && cJSON_IsString(role)) {
      sqlite3_result_text(ctx, role->valuestring, -1, SQLITE_TRANSIENT);
      cJSON_Delete(root);
      return;
    }
  }

  sqlite3_result_null(ctx);
  cJSON_Delete(root);
}

/*
 * get_message_model(json_text) -> TEXT
 *
 * Extract message.model (e.g., "claude-sonnet-4-20250514").
 */
static void get_message_model_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  const char *json = (const char *)sqlite3_value_text(argv[0]);

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    sqlite3_result_null(ctx);
    return;
  }

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (message) {
    cJSON *model = cJSON_GetObjectItem(message, "model");
    if (model && cJSON_IsString(model)) {
      sqlite3_result_text(ctx, model->valuestring, -1, SQLITE_TRANSIENT);
      cJSON_Delete(root);
      return;
    }
  }

  sqlite3_result_null(ctx);
  cJSON_Delete(root);
}

/*
 * get_message_text(json_text) -> TEXT
 *
 * Extract plain text from message content, skipping thinking blocks.
 * Concatenates all non-thinking text content.
 */
static void get_message_text_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  const char *json = (const char *)sqlite3_value_text(argv[0]);

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    sqlite3_result_null(ctx);
    return;
  }

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (!message) {
    sqlite3_result_null(ctx);
    cJSON_Delete(root);
    return;
  }

  cJSON *content = cJSON_GetObjectItem(message, "content");
  if (!content) {
    sqlite3_result_null(ctx);
    cJSON_Delete(root);
    return;
  }

  /* Handle string content directly */
  if (cJSON_IsString(content)) {
    sqlite3_result_text(ctx, content->valuestring, -1, SQLITE_TRANSIENT);
    cJSON_Delete(root);
    return;
  }

  /* Handle array content - concatenate non-thinking text */
  if (cJSON_IsArray(content)) {
    /* First pass: calculate total length */
    size_t total_len = 0;
    int array_size = cJSON_GetArraySize(content);

    for (int i = 0; i < array_size; i++) {
      cJSON *item = cJSON_GetArrayItem(content, i);
      cJSON *type = cJSON_GetObjectItem(item, "type");

      if (!type || !cJSON_IsString(type)) continue;

      /* Skip thinking blocks */
      if (strcmp(type->valuestring, "thinking") == 0) continue;

      if (strcmp(type->valuestring, "text") == 0) {
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (text && cJSON_IsString(text)) {
          total_len += strlen(text->valuestring);
          if (total_len > 0) total_len += 1;  /* For newline separator */
        }
      }
    }

    if (total_len == 0) {
      sqlite3_result_null(ctx);
      cJSON_Delete(root);
      return;
    }

    /* Second pass: build result */
    char *result = sqlite3_malloc((int)(total_len + 1));
    if (!result) {
      sqlite3_result_error_nomem(ctx);
      cJSON_Delete(root);
      return;
    }

    result[0] = '\0';
    int first = 1;

    for (int i = 0; i < array_size; i++) {
      cJSON *item = cJSON_GetArrayItem(content, i);
      cJSON *type = cJSON_GetObjectItem(item, "type");

      if (!type || !cJSON_IsString(type)) continue;
      if (strcmp(type->valuestring, "thinking") == 0) continue;

      if (strcmp(type->valuestring, "text") == 0) {
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (text && cJSON_IsString(text)) {
          if (!first) {
            strcat(result, "\n");
          }
          strcat(result, text->valuestring);
          first = 0;
        }
      }
    }

    sqlite3_result_text(ctx, result, -1, sqlite3_free);
    cJSON_Delete(root);
    return;
  }

  sqlite3_result_null(ctx);
  cJSON_Delete(root);
}

/*
 * message_token_count(json_text) -> INTEGER
 *
 * Extract token count from message.usage.output_tokens (for assistant)
 * or message.usage.input_tokens (for user).
 * Returns NULL if token info not available.
 */
static void message_token_count_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  const char *json = (const char *)sqlite3_value_text(argv[0]);

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    sqlite3_result_null(ctx);
    return;
  }

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (message) {
    cJSON *usage = cJSON_GetObjectItem(message, "usage");
    if (usage) {
      /* Try output_tokens first (assistant messages) */
      cJSON *output = cJSON_GetObjectItem(usage, "output_tokens");
      if (output && cJSON_IsNumber(output)) {
        sqlite3_result_int(ctx, output->valueint);
        cJSON_Delete(root);
        return;
      }

      /* Try input_tokens (user messages) */
      cJSON *input = cJSON_GetObjectItem(usage, "input_tokens");
      if (input && cJSON_IsNumber(input)) {
        sqlite3_result_int(ctx, input->valueint);
        cJSON_Delete(root);
        return;
      }
    }
  }

  sqlite3_result_null(ctx);
  cJSON_Delete(root);
}

/*
 * is_thinking_message(json_text) -> INTEGER (0 or 1)
 *
 * Check if message content contains a thinking block.
 */
static void is_thinking_message_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
) {
  if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_int(ctx, 0);
    return;
  }

  const char *json = (const char *)sqlite3_value_text(argv[0]);

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    sqlite3_result_int(ctx, 0);
    return;
  }

  cJSON *message = cJSON_GetObjectItem(root, "message");
  if (message) {
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsArray(content)) {
      int array_size = cJSON_GetArraySize(content);
      for (int i = 0; i < array_size; i++) {
        cJSON *item = cJSON_GetArrayItem(content, i);
        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (type && cJSON_IsString(type) &&
            strcmp(type->valuestring, "thinking") == 0) {
          sqlite3_result_int(ctx, 1);
          cJSON_Delete(root);
          return;
        }
      }
    }
  }

  sqlite3_result_int(ctx, 0);
  cJSON_Delete(root);
}

/*
 * Register all helper functions with the database
 */
int register_helper_functions(sqlite3 *db) {
  int rc;

  rc = sqlite3_create_function(db, "get_message_content", 1, SQLITE_UTF8,
                               NULL, get_message_content_func, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function(db, "get_message_role", 1, SQLITE_UTF8,
                               NULL, get_message_role_func, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function(db, "get_message_model", 1, SQLITE_UTF8,
                               NULL, get_message_model_func, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function(db, "get_message_text", 1, SQLITE_UTF8,
                               NULL, get_message_text_func, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function(db, "message_token_count", 1, SQLITE_UTF8,
                               NULL, message_token_count_func, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  rc = sqlite3_create_function(db, "is_thinking_message", 1, SQLITE_UTF8,
                               NULL, is_thinking_message_func, NULL, NULL);
  if (rc != SQLITE_OK) return rc;

  return SQLITE_OK;
}

/*
 * Create metadata table documenting all helper functions
 */
int create_functions_metadata_table(sqlite3 *db, char **pzErrMsg) {
  int rc;

  /* Create the metadata table */
  const char *create_table =
    "CREATE TABLE IF NOT EXISTS _claude_code_functions ("
    "  name TEXT PRIMARY KEY,"
    "  signature TEXT,"
    "  description TEXT,"
    "  example TEXT"
    ")";

  rc = sqlite3_exec(db, create_table, NULL, NULL, pzErrMsg);
  if (rc != SQLITE_OK) return rc;

  /* Insert function documentation */
  const char *docs[] = {
    "INSERT OR REPLACE INTO _claude_code_functions VALUES("
    "'get_message_content', 'get_message_content(json_text) -> TEXT', "
    "'Extract message.content, handling array/text formats. Returns first text block for arrays.', "
    "'SELECT get_message_content(json_data) FROM messages WHERE type=\"user\" LIMIT 1')",

    "INSERT OR REPLACE INTO _claude_code_functions VALUES("
    "'get_message_role', 'get_message_role(json_text) -> TEXT', "
    "'Extract role from message (user, assistant, system).', "
    "'SELECT get_message_role(json_data) FROM messages LIMIT 1')",

    "INSERT OR REPLACE INTO _claude_code_functions VALUES("
    "'get_message_model', 'get_message_model(json_text) -> TEXT', "
    "'Extract model name from assistant message.', "
    "'SELECT get_message_model(json_data) FROM messages WHERE type=\"assistant\" LIMIT 1')",

    "INSERT OR REPLACE INTO _claude_code_functions VALUES("
    "'get_message_text', 'get_message_text(json_text) -> TEXT', "
    "'Extract plain text from content, skipping thinking blocks. Concatenates multiple text blocks.', "
    "'SELECT get_message_text(json_data) FROM messages WHERE type=\"assistant\" LIMIT 1')",

    "INSERT OR REPLACE INTO _claude_code_functions VALUES("
    "'message_token_count', 'message_token_count(json_text) -> INTEGER', "
    "'Extract token count from usage (output_tokens for assistant, input_tokens for user).', "
    "'SELECT SUM(message_token_count(json_data)) FROM messages WHERE type=\"assistant\"')",

    "INSERT OR REPLACE INTO _claude_code_functions VALUES("
    "'is_thinking_message', 'is_thinking_message(json_text) -> INTEGER', "
    "'Check if message contains a thinking block (returns 0 or 1).', "
    "'SELECT COUNT(*) FROM messages WHERE is_thinking_message(json_data) = 1')",

    NULL
  };

  for (int i = 0; docs[i] != NULL; i++) {
    rc = sqlite3_exec(db, docs[i], NULL, NULL, pzErrMsg);
    if (rc != SQLITE_OK) return rc;
  }

  return SQLITE_OK;
}
