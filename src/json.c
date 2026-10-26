#include "json.h"
#include "util.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t len;
    size_t pos;
    int depth;
    char **error;
} Parser;

static int parser_error(Parser *p, const char *message)
{
    if (p->error && !*p->error) {
        set_error(p->error, "JSON 错误（字节 %zu）: %s", p->pos, message);
    }
    return -1;
}

static void skip_ws(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
        } else {
            break;
        }
    }
}

static JsonValue *new_value(JsonType type)
{
    JsonValue *v = (JsonValue *)xcalloc(1, sizeof(JsonValue));
    v->type = type;
    return v;
}

static int append_utf8(StrBuf *sb, unsigned codepoint)
{
    char out[4];

    if (codepoint == 0 || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return -1;
    }
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return sb_append_n(sb, out, 1);
    }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return sb_append_n(sb, out, 2);
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return sb_append_n(sb, out, 3);
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return sb_append_n(sb, out, 4);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_hex4(Parser *p, unsigned *out)
{
    unsigned value = 0;
    int i;

    if (p->pos + 4 > p->len) {
        return parser_error(p, "\\u 转义不完整");
    }
    for (i = 0; i < 4; i++) {
        int d = hex_digit(p->text[p->pos + (size_t)i]);
        if (d < 0) {
            p->pos += (size_t)i;
            return parser_error(p, "\\u 转义包含非十六进制字符");
        }
        value = (value << 4) | (unsigned)d;
    }
    p->pos += 4;
    *out = value;
    return 0;
}

static char *parse_string_raw(Parser *p)
{
    StrBuf sb;

    if (p->pos >= p->len || p->text[p->pos] != '"') {
        parser_error(p, "期望字符串");
        return NULL;
    }
    p->pos++;
    sb_init(&sb);
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->text[p->pos++];
        if (c == '"') {
            return sb_take(&sb);
        }
        if (c < 0x20) {
            parser_error(p, "字符串中包含未转义的控制字符");
            sb_free(&sb);
            return NULL;
        }
        if (c != '\\') {
            if (sb_append_char(&sb, (char)c) != 0) {
                sb_free(&sb);
                parser_error(p, "内存不足");
                return NULL;
            }
            continue;
        }
        if (p->pos >= p->len) {
            sb_free(&sb);
            parser_error(p, "字符串转义不完整");
            return NULL;
        }
        c = (unsigned char)p->text[p->pos++];
        switch (c) {
        case '"': sb_append_char(&sb, '"'); break;
        case '\\': sb_append_char(&sb, '\\'); break;
        case '/': sb_append_char(&sb, '/'); break;
        case 'b': sb_append_char(&sb, '\b'); break;
        case 'f': sb_append_char(&sb, '\f'); break;
        case 'n': sb_append_char(&sb, '\n'); break;
        case 'r': sb_append_char(&sb, '\r'); break;
        case 't': sb_append_char(&sb, '\t'); break;
        case 'u': {
            unsigned cp;
            if (parse_hex4(p, &cp) != 0) {
                sb_free(&sb);
                return NULL;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned low;
                if (p->pos + 2 > p->len ||
                    p->text[p->pos] != '\\' || p->text[p->pos + 1] != 'u') {
                    sb_free(&sb);
                    parser_error(p, "高位代理项后缺少低位代理项");
                    return NULL;
                }
                p->pos += 2;
                if (parse_hex4(p, &low) != 0) {
                    sb_free(&sb);
                    return NULL;
                }
                if (low < 0xDC00 || low > 0xDFFF) {
                    sb_free(&sb);
                    parser_error(p, "无效的低位代理项");
                    return NULL;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                sb_free(&sb);
                parser_error(p, "孤立的低位代理项");
                return NULL;
            }
            if (append_utf8(&sb, cp) != 0) {
                sb_free(&sb);
                parser_error(p, "无效的 Unicode 码点");
                return NULL;
            }
            break;
        }
        default:
            sb_free(&sb);
            p->pos--;
            parser_error(p, "未知的字符串转义");
            return NULL;
        }
    }
    sb_free(&sb);
    parser_error(p, "字符串没有结束引号");
    return NULL;
}

static JsonValue *parse_value(Parser *p);

static JsonValue *parse_object(Parser *p)
{
    JsonValue *value;

    if (++p->depth > 128) {
        parser_error(p, "JSON 嵌套过深");
        return NULL;
    }
    p->pos++; /* '{' */
    value = new_value(JSON_OBJECT);
    skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == '}') {
        p->pos++;
        p->depth--;
        return value;
    }
    for (;;) {
        char *key;
        JsonValue *item;
        JsonMember *member;

        skip_ws(p);
        key = parse_string_raw(p);
        if (!key) {
            json_free(value);
            return NULL;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->text[p->pos] != ':') {
            free(key);
            json_free(value);
            parser_error(p, "对象键后缺少冒号");
            return NULL;
        }
        p->pos++;
        item = parse_value(p);
        if (!item) {
            free(key);
            json_free(value);
            return NULL;
        }
        member = (JsonMember *)xcalloc(1, sizeof(JsonMember));
        member->key = key;
        member->value = item;
        if (value->u.object.tail) {
            value->u.object.tail->next = member;
        } else {
            value->u.object.head = member;
        }
        value->u.object.tail = member;
        value->u.object.count++;
        skip_ws(p);
        if (p->pos >= p->len) {
            json_free(value);
            parser_error(p, "对象没有结束花括号");
            return NULL;
        }
        if (p->text[p->pos] == '}') {
            p->pos++;
            p->depth--;
            return value;
        }
        if (p->text[p->pos] != ',') {
            json_free(value);
            parser_error(p, "对象成员之间缺少逗号");
            return NULL;
        }
        p->pos++;
    }
}

static JsonValue *parse_array(Parser *p)
{
    JsonValue *value;

    if (++p->depth > 128) {
        parser_error(p, "JSON 嵌套过深");
        return NULL;
    }
    p->pos++; /* '[' */
    value = new_value(JSON_ARRAY);
    skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == ']') {
        p->pos++;
        p->depth--;
        return value;
    }
    for (;;) {
        JsonValue *item = parse_value(p);
        if (!item) {
            json_free(value);
            return NULL;
        }
        if (value->u.array.count == value->u.array.cap) {
            size_t cap = value->u.array.cap ? value->u.array.cap * 2 : 8;
            value->u.array.items = (JsonValue **)xrealloc(
                value->u.array.items, cap * sizeof(JsonValue *));
            value->u.array.cap = cap;
        }
        value->u.array.items[value->u.array.count++] = item;
        skip_ws(p);
        if (p->pos >= p->len) {
            json_free(value);
            parser_error(p, "数组没有结束方括号");
            return NULL;
        }
        if (p->text[p->pos] == ']') {
            p->pos++;
            p->depth--;
            return value;
        }
        if (p->text[p->pos] != ',') {
            json_free(value);
            parser_error(p, "数组元素之间缺少逗号");
            return NULL;
        }
        p->pos++;
    }
}

static JsonValue *parse_number(Parser *p)
{
    size_t start = p->pos;
    char *token;
    char *end = NULL;
    double number;
    JsonValue *value;

    if (p->text[p->pos] == '-') {
        p->pos++;
    }
    if (p->pos >= p->len) {
        parser_error(p, "数字不完整");
        return NULL;
    }
    if (p->text[p->pos] == '0') {
        p->pos++;
    } else if (p->text[p->pos] >= '1' && p->text[p->pos] <= '9') {
        while (p->pos < p->len && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9') {
            p->pos++;
        }
    } else {
        parser_error(p, "无效的数字");
        return NULL;
    }
    if (p->pos < p->len && p->text[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || p->text[p->pos] < '0' ||
            p->text[p->pos] > '9') {
            parser_error(p, "小数点后缺少数字");
            return NULL;
        }
        while (p->pos < p->len && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9') {
            p->pos++;
        }
    }
    if (p->pos < p->len &&
        (p->text[p->pos] == 'e' || p->text[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len &&
            (p->text[p->pos] == '+' || p->text[p->pos] == '-')) {
            p->pos++;
        }
        if (p->pos >= p->len || p->text[p->pos] < '0' ||
            p->text[p->pos] > '9') {
            parser_error(p, "指数部分缺少数字");
            return NULL;
        }
        while (p->pos < p->len && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9') {
            p->pos++;
        }
    }
    token = xstrndup(p->text + start, p->pos - start);
    number = strtod(token, &end);
    if (!end || *end != '\0' || number != number ||
        number > DBL_MAX || number < -DBL_MAX) {
        free(token);
        parser_error(p, "数字超出范围");
        return NULL;
    }
    free(token);
    value = new_value(JSON_NUMBER);
    value->u.number = number;
    return value;
}

static JsonValue *parse_value(Parser *p)
{
    JsonValue *value;

    skip_ws(p);
    if (p->pos >= p->len) {
        parser_error(p, "期望一个 JSON 值");
        return NULL;
    }
    switch (p->text[p->pos]) {
    case '{':
        return parse_object(p);
    case '[':
        return parse_array(p);
    case '"': {
        char *s = parse_string_raw(p);
        if (!s) {
            return NULL;
        }
        value = new_value(JSON_STRING);
        value->u.string = s;
        return value;
    }
    case 't':
        if (p->pos + 4 <= p->len &&
            memcmp(p->text + p->pos, "true", 4) == 0) {
            p->pos += 4;
            value = new_value(JSON_BOOL);
            value->u.boolean = 1;
            return value;
        }
        break;
    case 'f':
        if (p->pos + 5 <= p->len &&
            memcmp(p->text + p->pos, "false", 5) == 0) {
            p->pos += 5;
            value = new_value(JSON_BOOL);
            value->u.boolean = 0;
            return value;
        }
        break;
    case 'n':
        if (p->pos + 4 <= p->len &&
            memcmp(p->text + p->pos, "null", 4) == 0) {
            p->pos += 4;
            return new_value(JSON_NULL);
        }
        break;
    default:
        break;
    }
    if (p->text[p->pos] == '-' ||
        (p->text[p->pos] >= '0' && p->text[p->pos] <= '9')) {
        return parse_number(p);
    }
    parser_error(p, "未知的 JSON 值");
    return NULL;
}

JsonValue *json_parse(const char *text, size_t len, char **error)
{
    Parser p;
    JsonValue *value;

    if (error) {
        free(*error);
        *error = NULL;
    }
    if (!text) {
        set_error(error, "JSON 输入为空");
        return NULL;
    }
    p.text = text;
    p.len = len;
    p.pos = 0;
    p.depth = 0;
    p.error = error;
    if (len >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        p.pos = 3;
    }
    value = parse_value(&p);
    if (!value) {
        return NULL;
    }
    skip_ws(&p);
    if (p.pos != p.len) {
        json_free(value);
        parser_error(&p, "JSON 值之后存在多余内容");
        return NULL;
    }
    return value;
}

JsonValue *json_parse_file(const wchar_t *path, char **error)
{
    char *text;
    size_t len;
    JsonValue *value;

    text = read_text_file(path, &len, error);
    if (!text) {
        return NULL;
    }
    value = json_parse(text, len, error);
    free(text);
    return value;
}

void json_free(JsonValue *value)
{
    if (!value) {
        return;
    }
    switch (value->type) {
    case JSON_STRING:
        free(value->u.string);
        break;
    case JSON_ARRAY: {
        size_t i;
        for (i = 0; i < value->u.array.count; i++) {
            json_free(value->u.array.items[i]);
        }
        free(value->u.array.items);
        break;
    }
    case JSON_OBJECT: {
        JsonMember *m = value->u.object.head;
        while (m) {
            JsonMember *next = m->next;
            free(m->key);
            json_free(m->value);
            free(m);
            m = next;
        }
        break;
    }
    default:
        break;
    }
    free(value);
}

JsonValue *json_get(const JsonValue *object, const char *key)
{
    JsonMember *m;

    if (!object || object->type != JSON_OBJECT || !key) {
        return NULL;
    }
    for (m = object->u.object.head; m; m = m->next) {
        if (strcmp(m->key, key) == 0) {
            return m->value;
        }
    }
    return NULL;
}

JsonValue *json_array_at(const JsonValue *array, size_t index)
{
    if (!array || array->type != JSON_ARRAY || index >= array->u.array.count) {
        return NULL;
    }
    return array->u.array.items[index];
}

size_t json_array_size(const JsonValue *array)
{
    return (array && array->type == JSON_ARRAY) ? array->u.array.count : 0;
}

size_t json_object_size(const JsonValue *object)
{
    return (object && object->type == JSON_OBJECT) ? object->u.object.count : 0;
}

const char *json_string(const JsonValue *value)
{
    return (value && value->type == JSON_STRING) ? value->u.string : NULL;
}

const char *json_get_string(const JsonValue *object, const char *key)
{
    return json_string(json_get(object, key));
}

int json_get_bool(const JsonValue *object, const char *key, int fallback)
{
    JsonValue *v = json_get(object, key);
    return (v && v->type == JSON_BOOL) ? v->u.boolean : fallback;
}

double json_get_number(const JsonValue *object, const char *key, double fallback)
{
    JsonValue *v = json_get(object, key);
    return (v && v->type == JSON_NUMBER) ? v->u.number : fallback;
}

long long json_get_i64(const JsonValue *object, const char *key,
                       long long fallback)
{
    JsonValue *v = json_get(object, key);
    if (!v || v->type != JSON_NUMBER) {
        return fallback;
    }
    if (v->u.number > 9.0e18 || v->u.number < -9.0e18) {
        return fallback;
    }
    return (long long)v->u.number;
}
