#include "cst.h"

#include <stdlib.h>
#include <string.h>

#include "sjis.h"

#define MAX_SCRIPT (128u * 1024 * 1024)
#define MAX_LINES 1000000

typedef struct {
    unsigned type;
    char *text;
} line;

struct cs2_cst {
    line *lines;
    size_t count;
};

cs2_cst *cs2_cst_parse(const uint8_t *container, size_t size, const char *name) {
    if (size < 16 || memcmp(container, "CatScene", 8) != 0) {
        cs2_set_error("%s is not a CatSystem2 CatScene script", name);
        return NULL;
    }
    uint32_t packed = cs2_u32(container, size, 8);
    uint32_t plain = cs2_u32(container, size, 12);
    if (plain == 0 || plain > MAX_SCRIPT) {
        cs2_set_error("%s declares an impossible size", name);
        return NULL;
    }
    uint8_t *script = malloc(plain);
    if (script == NULL) {
        cs2_set_error("out of memory reading %s", name);
        return NULL;
    }
    if (packed == 0) {
        if (16 + (size_t) plain > size) {
            free(script);
            cs2_set_error("%s ends inside its own text", name);
            return NULL;
        }
        memcpy(script, container + 16, plain);
    } else {
        if (16 + (size_t) packed > size
            || cs2_inflate(container + 16, packed, script, plain) != 0) {
            free(script);
            cs2_set_error("%s could not be unpacked", name);
            return NULL;
        }
    }

    uint32_t offsets = cs2_u32(script, plain, 8);
    uint32_t strings = cs2_u32(script, plain, 12);
    size_t offsets_start = 16 + (size_t) offsets;
    size_t strings_start = 16 + (size_t) strings;
    if (plain < 16 || strings_start < offsets_start || strings_start > plain
        || (strings_start - offsets_start) % 4 != 0) {
        free(script);
        cs2_set_error("%s has an unreadable line table", name);
        return NULL;
    }
    size_t count = (strings_start - offsets_start) / 4;
    if (count > MAX_LINES) {
        free(script);
        cs2_set_error("%s claims %zu lines", name, count);
        return NULL;
    }

    cs2_cst *result = calloc(1, sizeof *result);
    if (result == NULL) {
        free(script);
        cs2_set_error("out of memory reading %s", name);
        return NULL;
    }
    result->lines = count == 0 ? NULL : calloc(count, sizeof *result->lines);
    if (count > 0 && result->lines == NULL) {
        free(script);
        free(result);
        cs2_set_error("out of memory reading %s", name);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        size_t at = strings_start + cs2_u32(script, plain, offsets_start + i * 4);
        if (at + 2 > plain || script[at] != 0x01) continue;
        size_t start = at + 2;
        size_t end = start;
        while (end < plain && script[end] != '\0') end++;
        char *text = cs2_sjis_to_utf8(script + start, end - start);
        if (text == NULL) continue;
        result->lines[result->count].type = script[at + 1];
        result->lines[result->count].text = text;
        result->count++;
    }
    free(script);
    return result;
}

size_t cs2_cst_count(const cs2_cst *script) {
    return script == NULL ? 0 : script->count;
}

cs2_cst_line cs2_cst_at(const cs2_cst *script, size_t index) {
    cs2_cst_line empty = { 0, "" };
    if (script == NULL || index >= script->count) return empty;
    cs2_cst_line result = { script->lines[index].type, script->lines[index].text };
    return result;
}

void cs2_cst_free(cs2_cst *script) {
    if (script == NULL) return;
    for (size_t i = 0; i < script->count; i++) free(script->lines[i].text);
    free(script->lines);
    free(script);
}
