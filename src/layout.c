/*
 * The .fes layout runtime: the game's front end, one frame at a time.
 *
 * Read out of the game's own layouts rather than out of its engine: what a
 * statement means is what flow.fes, logo.fes and title.fes need it to mean for
 * the game to go logo, title, new game, which is the sequence they describe
 * between them. Anything not written here is read past and said once, which is
 * how a screen names what it still wants.
 */
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs2.h"
#include "hg3.h"

#define LOCALS 32
#define CALL_DEPTH 16
#define LINE_BUDGET 20000

typedef struct {
    int section;
    size_t line;
} call_frame;

/* An object as it stands this frame: the file's row, plus what has been done to it. */
typedef struct object_state object_state;
struct object_state {
    cs2_fes_object declared;
    int alpha;
    int current_id;          /* which of ID.0.. the object is showing */
    int deleted;
    int fade_frames;         /* how long the fade lasts, 0 when nothing is moving */
    int fade_left;
    int fade_from, fade_to;
};

struct cs2_layout {
    cs2_files *files;
    cs2_fes *fes;
    cs2_layout_host host;
    object_state *objects;
    size_t object_count;
    int32_t locals[LOCALS];
    int section;             /* the state: the section that runs every frame */
    size_t line;
    call_frame stack[CALL_DEPTH];
    int depth;
    int wait_frames;
    int wait_motion;
    int result;
    cs2_layout *child;
    char state[64];
    char missing[64];        /* the last image this copy of the game has not got */
};

/* --------------------------------------------------------------- the values */

/*
 * The little expression language the sections are written in: numbers, the
 * game's flags as $512, the layout's own locals as \0, _EXITCODE_, the usual
 * arithmetic and comparisons, && and ||, and parentheses. It is a plain
 * recursive descent because that is all the files need.
 */
typedef struct {
    cs2_layout *layout;
    const char *at;
} reader;

static object_state *object_of(cs2_layout *layout, const char *token, size_t *from);
static int32_t expression(reader *reader);

static void skip_space(reader *reader) {
    while (*reader->at == ' ' || *reader->at == '\t') reader->at++;
}

static int32_t primary(reader *r) {
    skip_space(r);
    char c = *r->at;
    if (c == '(') {
        r->at++;
        int32_t value = expression(r);
        skip_space(r);
        if (*r->at == ')') r->at++;
        return value;
    }
    if (c == '!') {
        r->at++;
        return primary(r) == 0;
    }
    if (c == '-') {
        r->at++;
        return -primary(r);
    }
    if (c == '$') {
        r->at++;
        int number = (int) strtol(r->at, (char **) &r->at, 10);
        return r->layout->host.flag == NULL ? 0
             : r->layout->host.flag(r->layout->host.context, number);
    }
    if (c == '\\') {
        r->at++;
        int number = (int) strtol(r->at, (char **) &r->at, 10);
        if (number < 0 || number >= LOCALS) return 0;
        return r->layout->locals[number];
    }
    if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        const char *word = r->at;
        while (*r->at == '_' || (*r->at >= 'A' && *r->at <= 'Z')
               || (*r->at >= 'a' && *r->at <= 'z')
               || (*r->at >= '0' && *r->at <= '9')) r->at++;
        size_t length = (size_t) (r->at - word);
        if (*r->at == '[') {                       /* an element of an array */
            while (*r->at != 0 && *r->at != ']') r->at++;
            if (*r->at == ']') r->at++;
        }
        if (strncmp(word, "_EXITCODE_", 10) == 0) {
            return r->layout->child == NULL ? -1 : cs2_layout_result(r->layout->child);
        }
        /*
         * _CLICK_L_ and _CLICK_R_ are what the reader has just clicked, as the
         * object itself: the title screen is written as "_CLICK_L_ ==
         * btn_j[0]". So an object's name has to answer something of its own,
         * and it is its place in the layout plus one, which is never zero -
         * zero being "nothing was clicked", which is what this engine answers
         * until the reader's clicks reach a layout.
         */
        if (word[0] == '_') return 0;
        char name[48];
        size_t keep = length + 1 > sizeof name ? sizeof name - 1 : length;
        memcpy(name, word, keep);
        name[keep] = 0;
        size_t from = 0;
        const object_state *object = object_of(r->layout, name, &from);
        return object == NULL ? 0 : (int32_t) from;
    }
    return (int32_t) strtol(r->at, (char **) &r->at, 10);
}

static int32_t product(reader *r) {
    int32_t value = primary(r);
    for (;;) {
        skip_space(r);
        char c = *r->at;
        if (c != '*' && c != '/' && c != '%') return value;
        r->at++;
        int32_t right = primary(r);
        if (c == '*') value *= right;
        else if (right == 0) value = 0;
        else if (c == '/') value /= right;
        else value %= right;
    }
}

static int32_t sum(reader *r) {
    int32_t value = product(r);
    for (;;) {
        skip_space(r);
        char c = *r->at;
        if (c != '+' && c != '-') return value;
        r->at++;
        int32_t right = product(r);
        value = c == '+' ? value + right : value - right;
    }
}

static int32_t comparison(reader *r) {
    int32_t value = sum(r);
    for (;;) {
        skip_space(r);
        const char *at = r->at;
        if (at[0] == '=' && at[1] == '=') { r->at += 2; value = value == sum(r); }
        else if (at[0] == '!' && at[1] == '=') { r->at += 2; value = value != sum(r); }
        else if (at[0] == '<' && at[1] == '=') { r->at += 2; value = value <= sum(r); }
        else if (at[0] == '>' && at[1] == '=') { r->at += 2; value = value >= sum(r); }
        else if (at[0] == '<') { r->at += 1; value = value < sum(r); }
        else if (at[0] == '>') { r->at += 1; value = value > sum(r); }
        else return value;
    }
}

static int32_t expression(reader *r) {
    int32_t value = comparison(r);
    for (;;) {
        skip_space(r);
        const char *at = r->at;
        if (at[0] == '&' && at[1] == '&') { r->at += 2; value = comparison(r) != 0 && value != 0; }
        else if (at[0] == '|' && at[1] == '|') { r->at += 2; value = comparison(r) != 0 || value != 0; }
        else return value;
    }
}

static int32_t evaluate(cs2_layout *layout, const char *text) {
    reader r = { layout, text };
    return expression(&r);
}

/* -------------------------------------------------------------- the objects */

static object_state *object_of(cs2_layout *layout, const char *token, size_t *from) {
    char name[40];
    int index = -1;
    const char *bracket = strchr(token, '[');
    size_t length = bracket == NULL ? strlen(token) : (size_t) (bracket - token);
    if (length + 1 > sizeof name) length = sizeof name - 1;
    memcpy(name, token, length);
    name[length] = 0;
    if (bracket != NULL) index = atoi(bracket + 1);
    for (size_t i = *from; i < layout->object_count; i++) {
        object_state *object = &layout->objects[i];
        if (strcmp(object->declared.name, name) != 0) continue;
        if (index >= 0 && object->declared.index != index) continue;
        *from = i + 1;
        return object;
    }
    return NULL;
}

static void fade_start(object_state *object, int frames, int from, int to) {
    object->alpha = from;
    object->fade_from = from;
    object->fade_to = to;
    object->fade_frames = frames < 1 ? 1 : frames;
    object->fade_left = object->fade_frames;
}

static void motions_step(cs2_layout *layout) {
    for (size_t i = 0; i < layout->object_count; i++) {
        object_state *object = &layout->objects[i];
        if (object->fade_left <= 0) continue;
        object->fade_left--;
        int done = object->fade_frames - object->fade_left;
        object->alpha = object->fade_from
            + (object->fade_to - object->fade_from) * done / object->fade_frames;
    }
}

static int motions_running(const cs2_layout *layout) {
    for (size_t i = 0; i < layout->object_count; i++) {
        if (layout->objects[i].fade_left > 0) return 1;
    }
    return 0;
}

/*
 * A command addressed to an object: "pl_bgi fade 60 0 255", "btn_d disp 1",
 * "btn_d[4] enable 0". A bare name is every element of the array, which is how
 * the files switch a whole row of buttons at once.
 */
static int object_command(cs2_layout *layout, const char *line) {
    char token[48], command[32];
    int read = 0;
    if (sscanf(line, "%47s %31s%n", token, command, &read) < 2) {
        if (sscanf(line, "%47s%n", token, &read) < 1) return 0;
        command[0] = 0;
    }
    size_t from = 0;
    if (object_of(layout, token, &from) == NULL) return 0;
    const char *rest = line + read;
    int a = 0, b = 0, c = 0;
    int given = sscanf(rest, "%d %d %d", &a, &b, &c);

    from = 0;
    object_state *object;
    while ((object = object_of(layout, token, &from)) != NULL) {
        if (strcmp(command, "disp") == 0) object->declared.disp = a;
        else if (strcmp(command, "enable") == 0) object->declared.enable = a;
        else if (strcmp(command, "noact") == 0) object->declared.enable = a == 0;
        else if (strcmp(command, "setid") == 0) object->current_id = a;
        else if (strcmp(command, "delete") == 0) object->deleted = 1;
        else if (strcmp(command, "pos") == 0 && given >= 2) {
            object->declared.x = a;
            object->declared.y = b;
        } else if (strcmp(command, "fade") == 0 && given >= 3) {
            fade_start(object, a, b, c);
        }
        /* load, play, stop, blend and the rest are the sound and motion
           vocabulary; they are not what puts a screen up and are read past. */
    }
    return 1;
}

/* -------------------------------------------------------------- the sections */

static const char *line_at(cs2_layout *layout, int section, size_t line) {
    return cs2_fes_line(layout->fes, section, line);
}

/* Past the matching #endif, counting the ifs in between. else stops the skip. */
static size_t skip_branch(cs2_layout *layout, int section, size_t line, int stop_at_else) {
    int depth = 0;
    size_t count = cs2_fes_section_lines(layout->fes, section);
    for (line++; line < count; line++) {
        const char *text = line_at(layout, section, line);
        if (text == NULL || text[0] == 0) continue;
        if (strncmp(text, "if", 2) == 0 && (text[2] == ' ' || text[2] == '(')) depth++;
        else if (strcmp(text, "endif") == 0) {
            if (depth == 0) return line + 1;
            depth--;
        } else if (stop_at_else && depth == 0 && strcmp(text, "else") == 0) {
            return line + 1;
        }
    }
    return count;
}

static void go_to(cs2_layout *layout, const char *name) {
    int section = cs2_fes_section(layout->fes, name);
    if (section < 0) {
        cs2_log("%s.fes has no section #%s", cs2_fes_name(layout->fes), name);
        return;
    }
    layout->section = section;
    layout->line = 0;
    layout->depth = 0;
}

/* One frame's worth of a layout's script. */
static void run(cs2_layout *layout) {
    for (int budget = 0; budget < LINE_BUDGET; budget++) {
        size_t count = cs2_fes_section_lines(layout->fes, layout->section);
        if (layout->line >= count) {
            if (layout->depth > 0) {
                layout->depth--;
                layout->section = layout->stack[layout->depth].section;
                layout->line = layout->stack[layout->depth].line;
                continue;
            }
            layout->line = 0;              /* the state runs again next frame */
            return;
        }
        const char *line = line_at(layout, layout->section, layout->line);
        if (line == NULL || line[0] == 0) {
            layout->line++;
            continue;
        }
        char word[48] = "";
        sscanf(line, "%47s", word);

        if (strncmp(line, "if", 2) == 0 && (line[2] == ' ' || line[2] == '(')) {
            const char *open = strchr(line, '(');
            if (open != NULL && evaluate(layout, open + 1) == 0) {
                layout->line = skip_branch(layout, layout->section, layout->line, 1);
            } else {
                layout->line++;
            }
            continue;
        }
        if (strcmp(word, "else") == 0) {
            layout->line = skip_branch(layout, layout->section, layout->line, 0);
            continue;
        }
        if (strcmp(word, "endif") == 0) {
            layout->line++;
            continue;
        }
        if (strcmp(word, "break") == 0) {
            if (layout->depth > 0) {
                layout->depth--;
                layout->section = layout->stack[layout->depth].section;
                layout->line = layout->stack[layout->depth].line;
                continue;
            }
            layout->line = 0;
            return;
        }
        if (strcmp(word, "call") == 0) {
            char name[48] = "";
            sscanf(line, "%*s %47s", name);
            int section = cs2_fes_section(layout->fes, name);
            if (section < 0 || layout->depth >= CALL_DEPTH) {
                layout->line++;
                continue;
            }
            layout->stack[layout->depth].section = layout->section;
            layout->stack[layout->depth].line = layout->line + 1;
            layout->depth++;
            layout->section = section;
            layout->line = 0;
            continue;
        }
        if (strcmp(word, "next") == 0) {
            char name[48] = "";
            sscanf(line, "%*s %47s", name);
            go_to(layout, name);
            snprintf(layout->state, sizeof layout->state, "%s", name);
            return;                        /* one state a frame, as the game runs it */
        }
        if (strcmp(word, "wait") == 0) {
            const char *rest = line + 4;
            while (*rest == ' ') rest++;
            layout->line++;
            if (*rest >= '0' && *rest <= '9') layout->wait_frames = atoi(rest);
            else layout->wait_motion = 1;
            return;
        }
        if (strcmp(word, "exit") == 0) {
            layout->result = (int) evaluate(layout, line + 4);
            layout->line++;
            continue;
        }
        if (strcmp(word, "execfes") == 0) {
            char name[48] = "";
            sscanf(line, "%*s %47s", name);
            cs2_layout_free(layout->child);
            layout->child = cs2_layout_start(layout->files, name, &layout->host);
            if (layout->child == NULL) cs2_log("%s", cs2_error());
            layout->line++;
            continue;
        }
        if (strcmp(word, "endfes") == 0) {
            cs2_layout_free(layout->child);
            layout->child = NULL;
            layout->line++;
            continue;
        }
        if (strcmp(word, "execkcs") == 0) {
            char name[48] = "";
            sscanf(line, "%*s %47s", name);
            if (layout->host.run_script != NULL) {
                layout->host.run_script(layout->host.context, name);
            }
            layout->line++;
            continue;
        }
        if (strcmp(word, "endkcs") == 0) {
            if (layout->host.stop_script != NULL) {
                layout->host.stop_script(layout->host.context);
            }
            layout->line++;
            continue;
        }
        if (strcmp(word, "strvar") == 0) {
            int number = 0;
            char value[128] = "";
            if (sscanf(line, "%*s %d %127s", &number, value) == 2
                && layout->host.set_string != NULL) {
                layout->host.set_string(layout->host.context, number, value);
            }
            layout->line++;
            continue;
        }
        if (line[0] == '$' || line[0] == '\\') {
            const char *equals = strchr(line, '=');
            if (equals != NULL && equals[1] != '=') {
                int32_t value = evaluate(layout, equals + 1);
                if (line[0] == '$') {
                    int number = atoi(line + 1);
                    if (layout->host.set_flag != NULL) {
                        layout->host.set_flag(layout->host.context, number, value);
                    }
                } else {
                    int number = atoi(line + 1);
                    if (number >= 0 && number < LOCALS) layout->locals[number] = value;
                }
            }
            layout->line++;
            continue;
        }
        if (!object_command(layout, line)) {
            /* Not an object and not a statement this engine knows: the screen's
               own vocabulary (setconf, newsave, baseimg, the fw_* disc check).
               Read past, so the screen keeps going and names what it wanted. */
        }
        layout->line++;
    }
    cs2_log("%s.fes ran %d lines in one frame of #%s and was stopped",
            cs2_fes_name(layout->fes), LINE_BUDGET,
            cs2_fes_section_name(layout->fes, layout->section));
    layout->line = 0;
}

/* --------------------------------------------------------------- the layout */

cs2_layout *cs2_layout_start(cs2_files *files, const char *name, const cs2_layout_host *host) {
    cs2_layout *layout = calloc(1, sizeof *layout);
    if (layout == NULL) {
        cs2_set_error("out of memory for a layout");
        return NULL;
    }
    layout->files = files;
    layout->host = *host;
    layout->result = -1;
    layout->fes = cs2_fes_load(files, name);
    if (layout->fes == NULL) {
        free(layout);
        return NULL;
    }
    layout->object_count = cs2_fes_object_count(layout->fes);
    if (layout->object_count > 0) {
        layout->objects = calloc(layout->object_count, sizeof *layout->objects);
        if (layout->objects == NULL) {
            cs2_layout_free(layout);
            cs2_set_error("out of memory for the objects of %s.fes", name);
            return NULL;
        }
        for (size_t i = 0; i < layout->object_count; i++) {
            layout->objects[i].declared = *cs2_fes_object_at(layout->fes, i);
            layout->objects[i].alpha = 255;
            layout->objects[i].current_id = 0;
        }
    }
    layout->section = cs2_fes_section(layout->fes, "START");
    snprintf(layout->state, sizeof layout->state, "START");
    if (layout->section < 0) {
        cs2_log("%s.fes has no #START", name);
        layout->section = 0;
    }
    cs2_log("the layout %s.fes is running, %zu objects", name, layout->object_count);
    return layout;
}

void cs2_layout_free(cs2_layout *layout) {
    if (layout == NULL) return;
    cs2_layout_free(layout->child);
    cs2_fes_free(layout->fes);
    free(layout->objects);
    free(layout);
}

const char *cs2_layout_name(const cs2_layout *layout) {
    return layout == NULL ? "" : cs2_fes_name(layout->fes);
}

int cs2_layout_result(const cs2_layout *layout) {
    return layout == NULL ? -1 : layout->result;
}

const char *cs2_layout_state(const cs2_layout *layout) {
    return layout == NULL ? "" : layout->state;
}

void cs2_layout_frame(cs2_layout *layout) {
    if (layout == NULL) return;
    motions_step(layout);
    if (layout->wait_frames > 0) {
        layout->wait_frames--;
    } else if (layout->wait_motion && motions_running(layout)) {
        /* still moving */
    } else {
        layout->wait_motion = 0;
        run(layout);
    }
    cs2_layout_frame(layout->child);
}

/* --------------------------------------------------------------- the drawing */

static void blend(uint32_t *canvas, int width, int height, int x, int y,
                  const cs2_hg3_frame *image, int alpha) {
    for (int row = 0; row < image->height; row++) {
        int to_y = y + row;
        if (to_y < 0 || to_y >= height) continue;
        for (int column = 0; column < image->width; column++) {
            int to_x = x + column;
            if (to_x < 0 || to_x >= width) continue;
            uint32_t pixel = image->pixels[(size_t) row * image->width + column];
            uint32_t a = ((pixel >> 24) & 0xffu) * (uint32_t) alpha / 255u;
            if (a == 0) continue;
            uint32_t under = canvas[(size_t) to_y * width + to_x];
            uint32_t out = 0xff000000u;
            for (int shift = 0; shift <= 16; shift += 8) {
                uint32_t over = (pixel >> shift) & 0xffu;
                uint32_t below = (under >> shift) & 0xffu;
                out |= ((over * a + below * (255u - a)) / 255u) << shift;
            }
            canvas[(size_t) to_y * width + to_x] = out;
        }
    }
}

static const object_state *plane_of(const cs2_layout *layout, const char *name) {
    if (name == NULL || name[0] == 0) return NULL;
    char base[40];
    int index = -1;
    const char *bracket = strchr(name, '[');
    size_t length = bracket == NULL ? strlen(name) : (size_t) (bracket - name);
    if (length + 1 > sizeof base) length = sizeof base - 1;
    memcpy(base, name, length);
    base[length] = 0;
    if (bracket != NULL) index = atoi(bracket + 1);
    for (size_t i = 0; i < layout->object_count; i++) {
        const object_state *object = &layout->objects[i];
        if (strcmp(object->declared.name, base) != 0) continue;
        if (index >= 0 && object->declared.index != index) continue;
        return object;
    }
    return NULL;
}

static void draw_one(cs2_layout *layout, const object_state *object,
                     uint32_t *canvas, int width, int height) {
    if (object->deleted || object->declared.disp == 0) return;
    if (object->declared.kind != CS2_FES_IMAGE && object->declared.kind != CS2_FES_BUTTON) return;
    if (object->declared.file[0] == 0) return;
    int id = object->declared.ids[0];
    if (object->current_id > 0 && object->current_id < CS2_FES_IDS
        && object->declared.ids[object->current_id] >= 0) {
        id = object->declared.ids[object->current_id];
    }
    if (id < 0) return;

    const object_state *plane = plane_of(layout, object->declared.plane);
    if (plane != NULL && (plane->deleted || plane->declared.disp == 0)) return;
    int alpha = object->alpha;
    if (plane != NULL) alpha = alpha * plane->alpha / 255;
    if (alpha <= 0) return;

    char path[160];
    snprintf(path, sizeof path, "image.int/%s.hg3", object->declared.file);
    cs2_bytes file = {0};
    if (cs2_files_read(layout->files, path, &file) != 0) {
        if (strcmp(layout->missing, object->declared.file) != 0) {
            snprintf(layout->missing, sizeof layout->missing, "%s", object->declared.file);
            cs2_log("%s.fes wants %s, which is not in this copy of the game",
                    cs2_fes_name(layout->fes), path);
        }
        return;
    }
    cs2_hg3_frame image = {0};
    if (cs2_hg3_decode_id(file.data, file.size, id, &image) != 0) {
        cs2_log("%s: %s", path, cs2_error());
        cs2_bytes_free(&file);
        return;
    }
    int x = object->declared.x + image.offset_x;
    int y = object->declared.y + image.offset_y;
    if (plane != NULL) {
        x += plane->declared.x + plane->declared.base_x;
        y += plane->declared.y + plane->declared.base_y;
    }
    blend(canvas, width, height, x, y, &image, alpha);
    cs2_hg3_frame_free(&image);
    cs2_bytes_free(&file);
}

void cs2_layout_draw(cs2_layout *layout, uint32_t *canvas, int width, int height) {
    if (layout == NULL) return;
    size_t count = layout->object_count;
    if (count == 0) {
        cs2_layout_draw(layout->child, canvas, width, height);
        return;
    }
    /*
     * Priority order, lowest first, which is the order the game draws them in;
     * a stable insertion sort, so objects sharing a priority keep the order the
     * layout declared them in.
     */
    size_t *order = malloc(count * sizeof *order);
    if (order == NULL) return;
    for (size_t i = 0; i < count; i++) order[i] = i;
    for (size_t i = 1; i < count; i++) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0 && layout->objects[order[j - 1]].declared.priority
                        > layout->objects[key].declared.priority) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    for (size_t i = 0; i < count; i++) draw_one(layout, &layout->objects[order[i]], canvas, width, height);
    free(order);
    cs2_layout_draw(layout->child, canvas, width, height);
}
