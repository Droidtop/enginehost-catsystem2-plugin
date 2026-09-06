/* Reading a .fes: the container, its sections, and its object tables. */
#include "fes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs2.h"

#define MAX_PLAIN (8u * 1024u * 1024u)
#define MAX_SECTIONS 512
#define MAX_OBJECTS 512
#define MAX_COLUMNS 24

typedef struct {
    char name[48];
    size_t first;                /* the first line of the section, in lines[] */
    size_t count;
} fes_section;

struct cs2_fes {
    char name[64];
    char *text;                  /* the whole file, cut into lines in place */
    char **lines;
    size_t line_count;
    fes_section sections[MAX_SECTIONS];
    size_t section_count;
    cs2_fes_object objects[MAX_OBJECTS];
    size_t object_count;
};

/* ------------------------------------------------------------ small helpers */

static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t'
                          || text[length - 1] == '\r')) {
        text[--length] = 0;
    }
    return text;
}

/*
 * A declared name and its index: "btn_d[0]" is btn_d element 0, "btn_d" is
 * every element, which the table rows and the script lines both rely on.
 */
static void split_name(const char *token, char *name, size_t name_size, int *index) {
    const char *bracket = strchr(token, '[');
    size_t length = bracket == NULL ? strlen(token) : (size_t) (bracket - token);
    if (length + 1 > name_size) length = name_size - 1;
    memcpy(name, token, length);
    name[length] = 0;
    *index = bracket == NULL ? -1 : atoi(bracket + 1);
}

static cs2_fes_kind kind_of(const char *word) {
    if (strcmp(word, "PLANE") == 0) return CS2_FES_PLANE;
    if (strcmp(word, "IMAGE") == 0 || strcmp(word, "CIMAGE") == 0
        || strcmp(word, "DIFIMAGE") == 0) return CS2_FES_IMAGE;
    if (strcmp(word, "BUTTON") == 0) return CS2_FES_BUTTON;
    if (strcmp(word, "SOUND") == 0) return CS2_FES_SOUND;
    if (strcmp(word, "STRING") == 0) return CS2_FES_STRING;
    return CS2_FES_OTHER;
}

static void pair(const char *value, int *x, int *y) {
    const char *comma = strchr(value, ',');
    *x = atoi(value);
    *y = comma == NULL ? 0 : atoi(comma + 1);
}

/* ------------------------------------------------------------ the container */

/*
 * A layout is named either way. The boot script gives its plane "flow", and
 * the window theme document gives the message window "meswnd.fes", so the
 * suffix is taken off before it is put back on.
 */
static char *inflate_layout(cs2_files *files, const char *name, char store[64]) {
    char path[160];
    size_t length = strlen(name);
    if (length > 4 && strcmp(name + length - 4, ".fes") == 0) length -= 4;
    if (length > 63) length = 63;
    memcpy(store, name, length);
    store[length] = 0;
    snprintf(path, sizeof path, "fes.int/%s.fes", store);
    cs2_bytes packed = {0};
    if (cs2_files_read(files, path, &packed) != 0) {
        cs2_set_error("there is no layout %s in this game", path);
        return NULL;
    }
    if (packed.size < 0x10 || memcmp(packed.data, "FES", 4) != 0) {
        cs2_bytes_free(&packed);
        cs2_set_error("%s is not a FES layout", path);
        return NULL;
    }
    uint32_t plain_size = cs2_u32(packed.data, packed.size, 8);
    if (plain_size == 0 || plain_size > MAX_PLAIN) {
        cs2_bytes_free(&packed);
        cs2_set_error("%s claims to unpack to %u bytes", path, plain_size);
        return NULL;
    }
    char *text = malloc((size_t) plain_size + 1);
    if (text == NULL) {
        cs2_bytes_free(&packed);
        cs2_set_error("out of memory for the layout %s", path);
        return NULL;
    }
    if (cs2_inflate(packed.data + 0x10, packed.size - 0x10, (uint8_t *) text, plain_size) != 0) {
        free(text);
        cs2_bytes_free(&packed);
        return NULL;
    }
    text[plain_size] = 0;
    cs2_bytes_free(&packed);
    return text;
}

/* ------------------------------------------------------------- the sections */

static int cut_into_lines(cs2_fes *fes, char *text, size_t size) {
    size_t capacity = 64;
    fes->lines = malloc(capacity * sizeof *fes->lines);
    if (fes->lines == NULL) return -1;
    char *at = text;
    char *end = text + size;
    while (at < end) {
        char *stop = memchr(at, '\n', (size_t) (end - at));
        if (stop != NULL) *stop = 0;
        if (fes->line_count == capacity) {
            capacity *= 2;
            char **grown = realloc(fes->lines, capacity * sizeof *fes->lines);
            if (grown == NULL) return -1;
            fes->lines = grown;
        }
        fes->lines[fes->line_count++] = trim(at);
        if (stop == NULL) break;
        at = stop + 1;
    }
    return 0;
}

/* ---------------------------------------------------------- the object table */

static cs2_fes_object *object_make(cs2_fes *fes, const char *name, int index,
                                   cs2_fes_kind kind) {
    if (fes->object_count >= MAX_OBJECTS) return NULL;
    cs2_fes_object *object = &fes->objects[fes->object_count++];
    memset(object, 0, sizeof *object);
    snprintf(object->name, sizeof object->name, "%s", name);
    object->index = index;
    object->kind = kind;
    for (int i = 0; i < CS2_FES_IDS; i++) object->ids[i] = -1;
    object->disp = 0;
    object->enable = 1;
    object->block = -1;
    object->column = -1;
    object->row = -1;
    return object;
}

static void declare(cs2_fes *fes, const char *line) {
    char word[32], token[48];
    if (sscanf(line, "%31s %47s", word, token) != 2) return;
    cs2_fes_kind kind = kind_of(word);
    char name[40];
    int count;
    split_name(token, name, sizeof name, &count);
    if (count < 0) {
        object_make(fes, name, -1, kind);
        return;
    }
    for (int i = 0; i < count; i++) object_make(fes, name, i, kind);
}

/*
 * One column of one row. The header names it, so a table this reader has never
 * seen still lands its FILE, its ids and its place in the right fields and
 * leaves the rest alone.
 */
static void set_column(cs2_fes_object *object, const char *column, const char *value) {
    if (strcmp(column, "FILE") == 0) {
        snprintf(object->file, sizeof object->file, "%s", value);
    } else if (strcmp(column, "PL") == 0 || strcmp(column, "PARENT") == 0) {
        snprintf(object->plane, sizeof object->plane, "%s", value);
    } else if (strncmp(column, "ID.", 3) == 0) {
        int which = atoi(column + 3);
        if (which >= 0 && which < CS2_FES_IDS) object->ids[which] = atoi(value);
    } else if (strcmp(column, "IDN") == 0) {
        object->ids[0] = atoi(value);
    } else if (strcmp(column, "POS") == 0 || strcmp(column, "POS2") == 0) {
        pair(value, &object->x, &object->y);
    } else if (strcmp(column, "BASE") == 0) {
        pair(value, &object->base_x, &object->base_y);
    } else if (strcmp(column, "SIZE") == 0 || strcmp(column, "SIZE2") == 0) {
        pair(value, &object->width, &object->height);
    } else if (strcmp(column, "PRI") == 0) {
        object->priority = atoi(value);
    } else if (strcmp(column, "DISP") == 0) {
        object->disp = atoi(value);
    } else if (strcmp(column, "ENABLE") == 0) {
        object->enable = atoi(value);
    } else if (strcmp(column, "KEYBLOCK") == 0) {
        /* "grid,column,row" - which of the layout's button grids this one is
           in and where in it, which is the whole of how a pad moves. */
        int grid = -1, at_column = -1, at_row = -1;
        if (sscanf(value, "%d,%d,%d", &grid, &at_column, &at_row) == 3) {
            object->block = grid;
            object->column = at_column;
            object->row = at_row;
        }
    }
    /* VRAM is where the surface is put in the game's own texture sheet, and
       GROUP, STYPE, CHANNEL, COLOR and MASK belong to sound and to drawing
       modes this engine does not have yet; none of them changes where a thing
       is on the screen, so they are read past. */
}

static void table_row(cs2_fes *fes, const char *line,
                      char columns[MAX_COLUMNS][16], int column_count) {
    char row[512];
    snprintf(row, sizeof row, "%s", line);
    char *save = NULL;
    char *token = strtok_r(row, " \t", &save);
    if (token == NULL) return;
    char name[40];
    int index;
    split_name(token, name, sizeof name, &index);
    for (int i = 0; i < column_count; i++) {
        char *value = strtok_r(NULL, " \t", &save);
        if (value == NULL) break;
        for (size_t j = 0; j < fes->object_count; j++) {
            cs2_fes_object *object = &fes->objects[j];
            if (strcmp(object->name, name) != 0) continue;
            if (index >= 0 && object->index != index) continue;
            set_column(object, columns[i], value);
        }
    }
}

/* --------------------------------------------------------------- the layout */

static int parse(cs2_fes *fes) {
    char columns[MAX_COLUMNS][16];
    int column_count = 0;
    enum { NONE, DEFINE, TABLE, SCRIPT } mode = NONE;

    for (size_t i = 0; i < fes->line_count; i++) {
        const char *line = fes->lines[i];
        if (line[0] == '#') {
            char word[48];
            if (sscanf(line + 1, "%47s", word) != 1) continue;
            if (strcmp(word, "DEFINE") == 0) {
                mode = DEFINE;
                continue;
            }
            if (strcmp(word, "OBJECT") == 0) {
                mode = TABLE;
                column_count = 0;
                const char *at = line + 1;
                char header[512];
                snprintf(header, sizeof header, "%s", at);
                char *save = NULL;
                char *token = strtok_r(header, " \t", &save);   /* "OBJECT" */
                while ((token = strtok_r(NULL, " \t", &save)) != NULL
                       && column_count < MAX_COLUMNS) {
                    snprintf(columns[column_count++], 16, "%s", token);
                }
                continue;
            }
            if (strcmp(word, "SUBCOMMAND") == 0 || strcmp(word, "KEYBLOCK") == 0) {
                /*
                 * The motion table, and the header of the button grids - how
                 * many there are and which grid each edge leads to. Grisaia's
                 * grids all lead nowhere ("-1,-1,-1,-1"), and where a button
                 * sits in its grid is in the KEYBLOCK column of the object
                 * table, which is read, so this header is read past.
                 */
                mode = NONE;
                continue;
            }
            mode = SCRIPT;
            if (fes->section_count < MAX_SECTIONS) {
                fes_section *section = &fes->sections[fes->section_count++];
                snprintf(section->name, sizeof section->name, "%s", word);
                section->first = i + 1;
                section->count = 0;
            }
            continue;
        }
        if (line[0] == 0) {
            if (mode == SCRIPT && fes->section_count > 0) {
                fes->sections[fes->section_count - 1].count = i + 1
                    - fes->sections[fes->section_count - 1].first;
            }
            continue;
        }
        switch (mode) {
        case DEFINE: declare(fes, line); break;
        case TABLE:  table_row(fes, line, columns, column_count); break;
        case SCRIPT:
            if (fes->section_count > 0) {
                fes->sections[fes->section_count - 1].count = i + 1
                    - fes->sections[fes->section_count - 1].first;
            }
            break;
        case NONE: break;
        }
    }
    return 0;
}

cs2_fes *cs2_fes_load(cs2_files *files, const char *name) {
    cs2_fes *fes = calloc(1, sizeof *fes);
    if (fes == NULL) {
        cs2_set_error("out of memory for a layout");
        return NULL;
    }
    fes->text = inflate_layout(files, name, fes->name);
    if (fes->text == NULL) {
        free(fes);
        return NULL;
    }
    if (cut_into_lines(fes, fes->text, strlen(fes->text)) != 0
        || parse(fes) != 0) {
        cs2_fes_free(fes);
        cs2_set_error("out of memory for the layout %s", name);
        return NULL;
    }
    return fes;
}

void cs2_fes_free(cs2_fes *fes) {
    if (fes == NULL) return;
    free(fes->lines);
    free(fes->text);
    free(fes);
}

const char *cs2_fes_name(const cs2_fes *fes) {
    return fes == NULL ? "" : fes->name;
}

int cs2_fes_section(const cs2_fes *fes, const char *name) {
    if (fes == NULL || name == NULL) return -1;
    for (size_t i = 0; i < fes->section_count; i++) {
        if (strcmp(fes->sections[i].name, name) == 0) return (int) i;
    }
    return -1;
}

size_t cs2_fes_section_lines(const cs2_fes *fes, int section) {
    if (fes == NULL || section < 0 || (size_t) section >= fes->section_count) return 0;
    return fes->sections[section].count;
}

const char *cs2_fes_line(const cs2_fes *fes, int section, size_t line) {
    if (fes == NULL || section < 0 || (size_t) section >= fes->section_count) return NULL;
    const fes_section *at = &fes->sections[section];
    if (line >= at->count) return NULL;
    return fes->lines[at->first + line];
}

const char *cs2_fes_section_name(const cs2_fes *fes, int section) {
    if (fes == NULL || section < 0 || (size_t) section >= fes->section_count) return "";
    return fes->sections[section].name;
}

size_t cs2_fes_object_count(const cs2_fes *fes) {
    return fes == NULL ? 0 : fes->object_count;
}

const cs2_fes_object *cs2_fes_object_at(const cs2_fes *fes, size_t index) {
    if (fes == NULL || index >= fes->object_count) return NULL;
    return &fes->objects[index];
}
