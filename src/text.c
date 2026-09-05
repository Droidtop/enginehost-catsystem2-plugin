#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RULES 64

typedef struct {
    char *find;
    char *replace;
} rule;

struct cs2_text {
    rule rules[MAX_RULES];
    size_t count;
};

static int ends_with(const char *text, const char *suffix) {
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length
        && strcmp(text + text_length - suffix_length, suffix) == 0;
}

cs2_text *cs2_text_from(const cs2_startup *startup) {
    cs2_text *text = calloc(1, sizeof *text);
    if (text == NULL) return NULL;
    for (size_t i = 0; i < cs2_startup_count(startup) && text->count < MAX_RULES; i++) {
        const char *path = cs2_startup_path_at(startup, i);
        if (strncmp(path, "SCRIPT/replace/", 15) != 0 || !ends_with(path, "/find")) continue;
        char replacement_path[512];
        snprintf(replacement_path, sizeof replacement_path, "%.*s%s",
                 (int) (strlen(path) - 4), path, "rep");
        const char *find = cs2_startup_value_at(startup, i);
        const char *replace = cs2_startup_value(startup, replacement_path);
        if (find == NULL || find[0] == '\0') continue;
        text->rules[text->count].find = strdup(find);
        text->rules[text->count].replace = strdup(replace == NULL ? "" : replace);
        if (text->rules[text->count].find == NULL || text->rules[text->count].replace == NULL) {
            free(text->rules[text->count].find);
            free(text->rules[text->count].replace);
            continue;
        }
        text->count++;
    }
    return text;
}

void cs2_text_free(cs2_text *text) {
    if (text == NULL) return;
    for (size_t i = 0; i < text->count; i++) {
        free(text->rules[i].find);
        free(text->rules[i].replace);
    }
    free(text);
}

/* Applies one substitution everywhere, returning a new string. */
static char *substitute(char *value, const char *find, const char *replace) {
    size_t find_length = strlen(find);
    if (find_length == 0) return value;
    size_t replace_length = strlen(replace);
    size_t occurrences = 0;
    for (const char *at = value; (at = strstr(at, find)) != NULL; at += find_length) occurrences++;
    if (occurrences == 0) return value;
    char *out = malloc(strlen(value) + occurrences * replace_length + 1);
    if (out == NULL) return value;
    size_t written = 0;
    for (const char *at = value; *at != '\0'; ) {
        if (strncmp(at, find, find_length) == 0) {
            memcpy(out + written, replace, replace_length);
            written += replace_length;
            at += find_length;
        } else {
            out[written++] = *at++;
        }
    }
    out[written] = '\0';
    free(value);
    return out;
}

char *cs2_text_display(const cs2_text *text, const char *raw) {
    char *value = strdup(raw);
    if (value == NULL) return NULL;
    for (size_t i = 0; i < text->count; i++) {
        value = substitute(value, text->rules[i].find, text->rules[i].replace);
    }
    char *out = malloc(strlen(value) + 1);
    if (out == NULL) return value;
    size_t written = 0;
    for (size_t i = 0; value[i] != '\0'; i++) {
        char c = value[i];
        if (c == '\\' && value[i + 1] != '\0') {
            /* A short, closed set: anything not listed drops the backslash and
               the single character naming the effect, and nothing else, so an
               unknown escape can never swallow the words after it. */
            if (strncmp(value + i, "\\fn", 3) == 0) {
                i += 2;
            } else if (value[i + 1] == 'n') {
                out[written++] = '\n';
                i++;
            } else {
                i++;
            }
        } else if (c != '[' && c != ']') {
            out[written++] = c;
        }
    }
    out[written] = '\0';
    free(value);
    return out;
}
