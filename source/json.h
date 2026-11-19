#ifndef NXLOAD_JSON_H
#define NXLOAD_JSON_H

#include <stdbool.h>
#include <ctype.h>
#include <string.h>

typedef struct {
    const char *begin, *end;
} Json;

static inline const char *whitespace(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) ++p;
    return p;
}

static inline const char *value_end(const char *p, const char *end, unsigned depth) {
    p = whitespace(p, end);
    if (p == end || depth > 50) return NULL;
    if (*p == '"') {
        while (++p < end) {
            if (*p == '"') return p + 1;
            if (*p == '\\' && ++p == end) break;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p++ == '{' ? '}' : ']';
        p = whitespace(p, end);
        while (p < end && *p != close) {
            p = value_end(p, end, depth + 1);
            if (!p) return NULL;
            p = whitespace(p, end);
            if (p < end && (*p == ':' || *p == ',')) {
                p = whitespace(p + 1, end);
                if (p == end || *p == close) return NULL;
            } else if (p == end || *p != close) return NULL;
        }
        return p < end ? p + 1 : NULL;
    }
    const char *start = p;
    while (p < end && !strchr(",]}:", *p) && !isspace((unsigned char)*p)) ++p;
    return p == start ? NULL : p;
}

static inline bool literal(Json part, const char *value) {
    return part.begin && (size_t)(part.end - part.begin) == strlen(value) &&
           !memcmp(part.begin, value, strlen(value));
}

static inline Json field(Json object, const char *key) {
    if (!object.begin || *object.begin != '{') return (Json){0};
    const char *p = whitespace(object.begin + 1, object.end);
    while (p < object.end && *p == '"') {
        const char *last = value_end(p, object.end, 0);
        if (!last) break;
        Json name = {p + 1, last - 1};
        p = whitespace(last, object.end);
        if (p == object.end || *p != ':') break;
        p = whitespace(p + 1, object.end);
        last = value_end(p, object.end, 0);
        if (!last) break;
        if (literal(name, key)) return (Json){p, last};
        p = whitespace(last, object.end);
        if (p == object.end || *p != ',') break;
        p = whitespace(p + 1, object.end);
    }
    return (Json){0};
}

static inline bool string_field(Json object, const char *key, char *out, size_t capacity) {
    Json value = field(object, key);
    if (!value.begin || *value.begin != '"' || (size_t)(value.end - value.begin) < 2)
        return false;
    size_t len = (size_t)(value.end - value.begin) - 2;
    if (len >= capacity || memchr(value.begin + 1, '\\', len)) return false;
    memcpy(out, value.begin + 1, len);
    out[len] = 0;
    return true;
}

static inline Json next_object(Json array, const char **cursor) {
    if (!array.begin || *array.begin != '[') return (Json){0};
    const char *p = whitespace(*cursor ? *cursor : array.begin + 1, array.end);
    if (p < array.end && *p == ',') p = whitespace(p + 1, array.end);
    if (p == array.end || *p != '{') return (Json){0};
    const char *end = value_end(p, array.end, 0);
    if (!end) return (Json){0};
    *cursor = end;
    return (Json){p, end};
}

static inline bool json_hex4(const char **s, unsigned *value) {
    const char *hex = "0123456789abcdef";
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int digit = tolower((unsigned char)**s);
        const char *p = digit ? strchr(hex, digit) : NULL;
        if (!p) return false;
        *value = 16 * *value + (unsigned)(p - hex);
        ++*s;
    }
    return true;
}

static inline bool read_json_string(const char **p, char *out, size_t capacity) {
    if (**p != '"') return false;
    const char *s = *p + 1;
    size_t n = 0;
    while (*s && *s != '"') {
        unsigned char ch = (unsigned char)*s++;
        if (ch < 0x20) return false;
        if (ch == '\\') {
            ch = (unsigned char)*s++;
            const char *simple = "\"\\/bfnrt";
            const char *escape = ch ? strchr(simple, ch) : NULL;
            if (escape) {
                ch = (unsigned char)"\"\\/\b\f\n\r\t"[escape - simple];
            } else if (ch == 'u') {
                unsigned code;
                if (!json_hex4(&s, &code) || !code) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (s[0] != '\\' || s[1] != 'u') return false;
                    s += 2;
                    unsigned low;
                    if (!json_hex4(&s, &low) || low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code <= 0xdfff) {
                    return false;
                }
                if (code > 0x7f) {
                    unsigned width = code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
                    if (n + width >= capacity) return false;
                    for (unsigned i = width - 1; i > 0; --i) {
                        out[n + i] = (char)(0x80 | (code & 0x3f));
                        code >>= 6;
                    }
                    const unsigned char prefix[] = {0, 0, 0xc0, 0xe0, 0xf0};
                    out[n] = (char)(prefix[width] | code);
                    n += width;
                    continue;
                }
                ch = (unsigned char)code;
            } else {
                return false;
            }
        }
        if (n + 1 >= capacity) return false;
        out[n++] = (char)ch;
    }
    if (*s != '"') return false;
    out[n] = 0;
    *p = s + 1;
    return true;
}

#endif
