/*
 * Minimal JSON parser used for Mojang metadata.
 * Supports the complete JSON value grammar plus UTF-8 and \u escapes.
 */
#ifndef MCLC_JSON_H
#define MCLC_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct JsonMember {
    char *key;
    JsonValue *value;
    struct JsonMember *next;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        int boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
            size_t cap;
        } array;
        struct {
            JsonMember *head;
            JsonMember *tail;
            size_t count;
        } object;
    } u;
};

JsonValue *json_parse(const char *text, size_t len, char **error);
JsonValue *json_parse_file(const wchar_t *path, char **error);
void json_free(JsonValue *value);

JsonValue *json_get(const JsonValue *object, const char *key);
JsonValue *json_array_at(const JsonValue *array, size_t index);
size_t json_array_size(const JsonValue *array);
size_t json_object_size(const JsonValue *object);

const char *json_string(const JsonValue *value);
const char *json_get_string(const JsonValue *object, const char *key);
int json_get_bool(const JsonValue *object, const char *key, int fallback);
double json_get_number(const JsonValue *object, const char *key, double fallback);
long long json_get_i64(const JsonValue *object, const char *key, long long fallback);

#endif /* MCLC_JSON_H */
