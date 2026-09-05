#include "startup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sjis.h"

#define MAX_VALUES 4096
#define MAX_DEPTH 32

typedef struct {
    char *path;
    char *value;
} pair;

struct cs2_startup {
    pair *values;
    size_t count;
};

static void add(cs2_startup *startup, const char *path, const char *value, size_t value_length) {
    if (startup->count >= MAX_VALUES || path[0] == '\0') return;
    for (size_t i = 0; i < startup->count; i++) {
        if (strcmp(startup->values[i].path, path) == 0) return;  /* the first wins */
    }
    char *stored_path = malloc(strlen(path) + 1);
    char *stored_value = malloc(value_length + 1);
    if (stored_path == NULL || stored_value == NULL) {
        free(stored_path);
        free(stored_value);
        return;
    }
    memcpy(stored_path, path, strlen(path) + 1);
    memcpy(stored_value, value, value_length);
    stored_value[value_length] = '\0';
    startup->values[startup->count].path = stored_path;
    startup->values[startup->count].value = stored_value;
    startup->count++;
}

/* The element path below the document root, so a value reads as "SCRIPT/start". */
static void join(char names[MAX_DEPTH][64], int depth, char *out, size_t out_size) {
    out[0] = '\0';
    size_t at = 0;
    for (int i = 1; i < depth; i++) {
        size_t length = strlen(names[i]);
        if (at + length + 2 >= out_size) return;
        if (at > 0) out[at++] = '/';
        memcpy(out + at, names[i], length);
        at += length;
        out[at] = '\0';
    }
}

/*
 * All-whitespace text is layout when it sits between child elements or spans
 * lines, but a value when an element with no children holds it on one line:
 * Labyrinth of Grisaia replaces the escape "\_" with exactly one space, written
 * <rep> </rep>, and trimming that away deletes the spacing its text depends on.
 */
static size_t trim(const char *text, size_t length, int had_children, size_t *start) {
    size_t from = 0;
    size_t to = length;
    while (from < to && (unsigned char) text[from] <= ' ') from++;
    while (to > from && (unsigned char) text[to - 1] <= ' ') to--;
    if (to > from) {
        *start = from;
        return to - from;
    }
    int spans_lines = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\n' || text[i] == '\r') spans_lines = 1;
    }
    *start = 0;
    return (had_children || spans_lines) ? 0 : length;
}

cs2_startup *cs2_startup_parse(const uint8_t *document, size_t size) {
    char *xml = cs2_sjis_to_utf8(document, size);
    if (xml == NULL) {
        cs2_set_error("out of memory reading startup.xml");
        return NULL;
    }
    cs2_startup *startup = calloc(1, sizeof *startup);
    if (startup == NULL) {
        free(xml);
        cs2_set_error("out of memory reading startup.xml");
        return NULL;
    }
    startup->values = calloc(MAX_VALUES, sizeof *startup->values);
    if (startup->values == NULL) {
        free(xml);
        free(startup);
        cs2_set_error("out of memory reading startup.xml");
        return NULL;
    }

    char names[MAX_DEPTH][64];
    int had_children[MAX_DEPTH];
    int depth = 0;
    size_t text_start = 0;
    size_t text_length = 0;
    size_t at = 0;
    size_t length = strlen(xml);

    while (at < length) {
        char *open = strchr(xml + at, '<');
        if (open == NULL) break;
        size_t open_at = (size_t) (open - xml);
        if (depth > 0) text_length = open_at - text_start;
        char *close = strchr(open, '>');
        if (close == NULL) break;
        size_t close_at = (size_t) (close - xml);
        size_t tag_length = close_at - open_at - 1;
        char tag[256];
        if (tag_length >= sizeof tag) tag_length = sizeof tag - 1;
        memcpy(tag, xml + open_at + 1, tag_length);
        tag[tag_length] = '\0';
        at = close_at + 1;

        if (tag[0] == '?' || tag[0] == '!') {
            if (strncmp(tag, "!--", 3) == 0) {
                char *end = strstr(xml + open_at, "-->");
                at = end == NULL ? length : (size_t) (end - xml) + 3;
            }
            text_start = at;
            text_length = 0;
            continue;
        }
        if (tag[0] == '/') {
            char *name = tag + 1;
            while (*name == ' ') name++;
            char *tail = name + strlen(name);
            while (tail > name && tail[-1] == ' ') *--tail = '\0';
            if (depth > 0 && strcmp(names[depth - 1], name) == 0) {
                size_t value_start = 0;
                size_t value_length = trim(xml + text_start, text_length,
                                           had_children[depth - 1], &value_start);
                if (value_length > 0) {
                    char path[512];
                    join(names, depth, path, sizeof path);
                    add(startup, path, xml + text_start + value_start, value_length);
                }
                depth--;
            }
            text_start = at;
            text_length = 0;
            continue;
        }

        int self_closing = tag_length > 0 && tag[tag_length - 1] == '/';
        char name[64];
        size_t name_length = 0;
        while (name_length < tag_length && name_length + 1 < sizeof name
               && tag[name_length] != ' ' && tag[name_length] != '/') {
            name[name_length] = tag[name_length];
            name_length++;
        }
        name[name_length] = '\0';
        text_start = at;
        text_length = 0;
        if (name_length == 0) continue;
        if (depth > 0) had_children[depth - 1] = 1;
        if (!self_closing && depth < MAX_DEPTH) {
            snprintf(names[depth], sizeof names[depth], "%s", name);
            had_children[depth] = 0;
            depth++;
        }
    }

    free(xml);
    return startup;
}

void cs2_startup_free(cs2_startup *startup) {
    if (startup == NULL) return;
    for (size_t i = 0; i < startup->count; i++) {
        free(startup->values[i].path);
        free(startup->values[i].value);
    }
    free(startup->values);
    free(startup);
}

const char *cs2_startup_value(const cs2_startup *startup, const char *path) {
    if (startup == NULL) return NULL;
    for (size_t i = 0; i < startup->count; i++) {
        if (strcmp(startup->values[i].path, path) == 0) return startup->values[i].value;
    }
    return NULL;
}

size_t cs2_startup_count(const cs2_startup *startup) {
    return startup == NULL ? 0 : startup->count;
}

const char *cs2_startup_path_at(const cs2_startup *startup, size_t index) {
    return startup == NULL || index >= startup->count ? NULL : startup->values[index].path;
}

const char *cs2_startup_value_at(const cs2_startup *startup, size_t index) {
    return startup == NULL || index >= startup->count ? NULL : startup->values[index].value;
}

int cs2_startup_number(const cs2_startup *startup, const char *path, int fallback) {
    const char *value = cs2_startup_value(startup, path);
    if (value == NULL) return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > 8192) return fallback;
    return (int) parsed;
}
