#include "cst.h"

#include <stdlib.h>
#include <string.h>

#include "sjis.h"

#define MAX_SCRIPT (128u * 1024 * 1024)
#define MAX_LINES 1000000

typedef struct {
    unsigned type;
    char *text;
} word;

/* A line is a run of the words: where it begins and how many it holds. */
typedef struct {
    size_t first;
    size_t count;
} line;

struct cs2_cst {
    word *words;
    size_t word_count;
    line *lines;
    size_t line_count;
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
    size_t lines_start = 16;
    size_t offsets_start = 16 + (size_t) offsets;
    size_t strings_start = 16 + (size_t) strings;
    if (plain < 16 || offsets_start < lines_start || strings_start < offsets_start
        || strings_start > plain || (offsets_start - lines_start) % 8 != 0
        || (strings_start - offsets_start) % 4 != 0) {
        free(script);
        cs2_set_error("%s has an unreadable line table", name);
        return NULL;
    }
    size_t line_count = (offsets_start - lines_start) / 8;
    size_t word_count = (strings_start - offsets_start) / 4;
    if (line_count > MAX_LINES || word_count > MAX_LINES) {
        free(script);
        cs2_set_error("%s claims %zu lines of %zu words", name, line_count, word_count);
        return NULL;
    }

    cs2_cst *result = calloc(1, sizeof *result);
    if (result == NULL) {
        free(script);
        cs2_set_error("out of memory reading %s", name);
        return NULL;
    }
    result->words = word_count == 0 ? NULL : calloc(word_count, sizeof *result->words);
    result->lines = line_count == 0 ? NULL : calloc(line_count, sizeof *result->lines);
    if ((word_count > 0 && result->words == NULL)
        || (line_count > 0 && result->lines == NULL)) {
        free(script);
        cs2_cst_free(result);
        cs2_set_error("out of memory reading %s", name);
        return NULL;
    }

    /*
     * Every word is kept at its own place in the table, empty where the file
     * has nothing readable, because a line names its words by that place: a
     * word skipped for being unreadable would shift every line after it.
     */
    for (size_t i = 0; i < word_count; i++) {
        size_t at = strings_start + cs2_u32(script, plain, offsets_start + i * 4);
        result->words[i].type = 0;
        result->words[i].text = NULL;
        if (at + 2 > plain || script[at] != 0x01) continue;
        size_t start = at + 2;
        size_t end = start;
        while (end < plain && script[end] != '\0') end++;
        char *text = cs2_sjis_to_utf8(script + start, end - start);
        if (text == NULL) continue;
        result->words[i].type = script[at + 1];
        result->words[i].text = text;
    }
    result->word_count = word_count;

    for (size_t i = 0; i < line_count; i++) {
        size_t count = cs2_u32(script, plain, lines_start + i * 8);
        size_t first = cs2_u32(script, plain, lines_start + i * 8 + 4);
        if (first > word_count) first = word_count;
        if (count > word_count - first) count = word_count - first;
        result->lines[i].first = first;
        result->lines[i].count = count;
    }
    result->line_count = line_count;

    free(script);
    return result;
}

size_t cs2_cst_word_count(const cs2_cst *script) {
    return script == NULL ? 0 : script->word_count;
}

cs2_cst_word cs2_cst_word_at(const cs2_cst *script, size_t index) {
    cs2_cst_word empty = { 0, "" };
    if (script == NULL || index >= script->word_count) return empty;
    const word *at = &script->words[index];
    cs2_cst_word result = { at->type, at->text == NULL ? "" : at->text };
    return result;
}

size_t cs2_cst_line_count(const cs2_cst *script) {
    return script == NULL ? 0 : script->line_count;
}

int cs2_cst_line_words(const cs2_cst *script, size_t line, size_t *first, size_t *count) {
    if (script == NULL || line >= script->line_count) return -1;
    if (first != NULL) *first = script->lines[line].first;
    if (count != NULL) *count = script->lines[line].count;
    return 0;
}

void cs2_cst_free(cs2_cst *script) {
    if (script == NULL) return;
    for (size_t i = 0; i < script->word_count; i++) free(script->words[i].text);
    free(script->words);
    free(script->lines);
    free(script);
}
