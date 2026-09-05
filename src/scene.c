#include "scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cst.h"

#define MAX_LAYERS 64
#define MAX_SCRIPTS 500
#define MAX_WORDS 16
#define SKIPPED_SIZE 1024

struct cs2_scene {
    cs2_files *files;
    const cs2_text *text;
    cs2_vars *vars;
    cs2_cst *script;
    char path[512];
    size_t cursor;
    int ended;
    int scripts_played;

    char speaker[256];
    char *message;

    cs2_layer layers[MAX_LAYERS];
    size_t layer_count;

    char skipped[SKIPPED_SIZE];
};

cs2_scene *cs2_scene_new(cs2_files *files, const cs2_text *text) {
    cs2_scene *scene = calloc(1, sizeof *scene);
    if (scene == NULL) return NULL;
    scene->files = files;
    scene->text = text;
    scene->vars = cs2_vars_new();
    if (scene->vars == NULL) {
        free(scene);
        return NULL;
    }
    return scene;
}

void cs2_scene_free(cs2_scene *scene) {
    if (scene == NULL) return;
    cs2_cst_free(scene->script);
    cs2_vars_free(scene->vars);
    free(scene->message);
    free(scene);
}

int cs2_scene_play(cs2_scene *scene, const char *script_path) {
    char normalised[512];
    snprintf(normalised, sizeof normalised, "%s", script_path);
    for (char *c = normalised; *c != '\0'; c++) {
        if (*c == '\\') *c = '/';
    }
    cs2_bytes data;
    if (cs2_files_read(scene->files, normalised, &data) != 0) return -1;
    cs2_cst *script = cs2_cst_parse(data.data, data.size, normalised);
    cs2_bytes_free(&data);
    if (script == NULL) return -1;
    cs2_cst_free(scene->script);
    scene->script = script;
    snprintf(scene->path, sizeof scene->path, "%s", normalised);
    scene->cursor = 0;
    scene->ended = 0;
    scene->scripts_played++;
    return 0;
}

static void note(cs2_scene *scene, const char *what) {
    if (what[0] == '\0') return;
    char padded[64];
    snprintf(padded, sizeof padded, " %s ", what);
    if (strstr(scene->skipped, padded) != NULL) return;
    size_t used = strlen(scene->skipped);
    if (used + strlen(padded) + 1 >= sizeof scene->skipped) return;
    if (used == 0) {
        snprintf(scene->skipped, sizeof scene->skipped, "%s", padded);
    } else {
        snprintf(scene->skipped + used - 1, sizeof scene->skipped - used + 1, "%s", padded);
    }
}

const char *cs2_scene_take_skipped(cs2_scene *scene) {
    static char out[SKIPPED_SIZE];
    snprintf(out, sizeof out, "%s", scene->skipped);
    scene->skipped[0] = '\0';
    /* Trim the padding the notes are stored with. */
    size_t length = strlen(out);
    while (length > 0 && out[length - 1] == ' ') out[--length] = '\0';
    return out[0] == ' ' ? out + 1 : out;
}

/* ---- the stage ---- */

static cs2_layer *find_layer(cs2_scene *scene, int kind, int slot) {
    for (size_t i = 0; i < scene->layer_count; i++) {
        if (scene->layers[i].kind == kind && scene->layers[i].slot == slot) return &scene->layers[i];
    }
    return NULL;
}

static void remove_at(cs2_scene *scene, size_t index) {
    for (size_t i = index; i + 1 < scene->layer_count; i++) scene->layers[i] = scene->layers[i + 1];
    scene->layer_count--;
}

static void clear_kind(cs2_scene *scene, int kind) {
    for (size_t i = scene->layer_count; i-- > 0; ) {
        if (scene->layers[i].kind == kind) remove_at(scene, i);
    }
}

static void clear_slot(cs2_scene *scene, int kind, int slot) {
    for (size_t i = scene->layer_count; i-- > 0; ) {
        if (scene->layers[i].kind == kind && scene->layers[i].slot == slot) remove_at(scene, i);
    }
}

/* Layers are kept in drawing order, back to front, so drawing is a plain walk. */
static void place(cs2_scene *scene, const cs2_layer *layer) {
    clear_slot(scene, layer->kind, layer->slot);
    if (scene->layer_count >= MAX_LAYERS) return;
    size_t at = scene->layer_count;
    while (at > 0) {
        const cs2_layer *before = &scene->layers[at - 1];
        if (before->kind < layer->kind
            || (before->kind == layer->kind && before->slot < layer->slot)) {
            break;
        }
        scene->layers[at] = scene->layers[at - 1];
        at--;
    }
    scene->layers[at] = *layer;
    scene->layer_count++;
}

size_t cs2_scene_layer_count(const cs2_scene *scene) {
    return scene->layer_count;
}

const cs2_layer *cs2_scene_layer_at(const cs2_scene *scene, size_t index) {
    return index >= scene->layer_count ? NULL : &scene->layers[index];
}

/* ---- commands ---- */

static int number(cs2_scene *scene, const char *word, int32_t *value) {
    return cs2_expr_eval(scene->vars, word, value);
}

/*
 * The layer commands. They share one shape: the kind, the slot, and then either
 * nothing (clear it), a named sub-command, an image name with its position, or a
 * "$"-prefixed colour with the rectangle to fill.
 */
static void layer_command(cs2_scene *scene, int kind, char words[MAX_WORDS][128], int count) {
    if (count == 1) {
        clear_kind(scene, kind);
        return;
    }
    int32_t slot = 0;
    if (number(scene, words[1], &slot) != 0) {
        note(scene, words[0]);
        return;
    }
    if (count == 2) {
        clear_slot(scene, kind, (int) slot);
        return;
    }
    const char *third = words[2];
    if (cs2_ieq(third, "blend")) {
        int32_t alpha = 255;
        if (count >= 4 && number(scene, words[3], &alpha) == 0) {
            cs2_layer *found = find_layer(scene, kind, (int) slot);
            if (found != NULL) found->alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : (int) alpha);
        }
        return;
    }
    if (cs2_ieq(third, "scale") || cs2_ieq(third, "move") || cs2_ieq(third, "rotate")) {
        char what[64];
        snprintf(what, sizeof what, "%s.%s", words[0], third);
        note(scene, what);
        return;
    }

    cs2_layer layer;
    memset(&layer, 0, sizeof layer);
    layer.kind = kind;
    layer.slot = (int) slot;
    layer.alpha = 255;

    if (third[0] == '$') {
        int32_t colour = 0;
        int32_t width = 0;
        int32_t height = 0;
        int32_t x = 0;
        int32_t y = 0;
        if (number(scene, third, &colour) != 0) return;
        if (count > 3) number(scene, words[3], &width);
        if (count > 4) number(scene, words[4], &height);
        if (count > 5) number(scene, words[5], &x);
        if (count > 6) number(scene, words[6], &y);
        layer.is_colour = 1;
        layer.colour = (uint32_t) colour;
        layer.width = width;
        layer.height = height;
        layer.x = x;
        layer.y = y;
        place(scene, &layer);
        return;
    }

    /* An image name can carry the engine's own pose and animation selectors
       after commas. Which frame each selector picks is not worked out yet, so
       the first frame is drawn and the selectors are noted. */
    size_t name_length = strcspn(third, ",");
    if (name_length >= sizeof layer.image) name_length = sizeof layer.image - 1;
    memcpy(layer.image, third, name_length);
    layer.image[name_length] = '\0';
    if (third[name_length] == ',') {
        char what[64];
        snprintf(what, sizeof what, "%s.parts", words[0]);
        note(scene, what);
    }
    int32_t x = 0;
    int32_t y = 0;
    if (count > 3) number(scene, words[3], &x);
    if (count > 4) number(scene, words[4], &y);
    layer.x = x;
    layer.y = y;
    place(scene, &layer);
}

static const char *run_command(cs2_scene *scene, const char *line, char *next_script, size_t next_size);

/* "if (condition) command", with or without a space after "if". */
static const char *conditional(cs2_scene *scene, const char *line,
                               char *next_script, size_t next_size) {
    const char *open = strchr(line, '(');
    if (open == NULL) {
        note(scene, "if");
        return NULL;
    }
    int depth = 0;
    const char *close = NULL;
    for (const char *at = open; *at != '\0'; at++) {
        if (*at == '(') depth++;
        else if (*at == ')' && --depth == 0) {
            close = at;
            break;
        }
    }
    if (close == NULL) {
        note(scene, "if");
        return NULL;
    }
    char condition[512];
    size_t length = (size_t) (close - open - 1);
    if (length >= sizeof condition) length = sizeof condition - 1;
    memcpy(condition, open + 1, length);
    condition[length] = '\0';
    int32_t value = 0;
    if (cs2_expr_eval(scene->vars, condition, &value) != 0) {
        note(scene, "if");
        return NULL;
    }
    if (value == 0) return NULL;
    return run_command(scene, close + 1, next_script, next_size);
}

/* "#slot = expression", where the slot may itself be computed. */
static void assign(cs2_scene *scene, const char *line) {
    int depth = 0;
    const char *equals = NULL;
    for (const char *at = line; *at != '\0'; at++) {
        if (*at == '(') depth++;
        else if (*at == ')') depth--;
        else if (*at == '=' && depth == 0 && at[1] != '='
                 && (at == line || strchr("!<>=", at[-1]) == NULL)) {
            equals = at;
            break;
        }
    }
    if (equals == NULL) {
        note(scene, "assignment");
        return;
    }
    char slot_text[256];
    size_t length = (size_t) (equals - line - 1);
    if (length >= sizeof slot_text) length = sizeof slot_text - 1;
    memcpy(slot_text, line + 1, length);
    slot_text[length] = '\0';
    int32_t slot = 0;
    int32_t value = 0;
    if (cs2_expr_eval(scene->vars, slot_text, &slot) != 0
        || cs2_expr_eval(scene->vars, equals + 1, &value) != 0) {
        note(scene, "assignment");
        return;
    }
    cs2_vars_set_number(scene->vars, slot, value);
}

/*
 * Carries out one command line. Returns the script to continue with, copied
 * into next_script, or NULL when the command was not a jump.
 */
static const char *run_command(cs2_scene *scene, const char *line,
                               char *next_script, size_t next_size) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return NULL;
    if (strncmp(line, "if", 2) == 0) return conditional(scene, line, next_script, next_size);
    if (line[0] == '#') {
        assign(scene, line);
        return NULL;
    }

    char words[MAX_WORDS][128];
    int count = 0;
    for (const char *at = line; *at != '\0' && count < MAX_WORDS; ) {
        while (*at == ' ' || *at == '\t') at++;
        if (*at == '\0') break;
        size_t length = strcspn(at, " \t");
        size_t kept = length >= sizeof words[0] ? sizeof words[0] - 1 : length;
        memcpy(words[count], at, kept);
        words[count][kept] = '\0';
        count++;
        at += length;
    }
    if (count == 0) return NULL;

    static const char *ignored[] = {
        "wait", "rdraw", "frameoff", "frameon", "wipe", "wipe2", "rwipe", "wipedef",
        "draw", "drawdef", "auto", "autoface", "mesdraw", "view", "scene", "jumpstop",
        "undo", "cgflag", "cgreg", "place", "title", "end_of_kaisou"
    };

    if (cs2_ieq(words[0], "bg")) layer_command(scene, CS2_LAYER_BACKGROUND, words, count);
    else if (cs2_ieq(words[0], "cg")) layer_command(scene, CS2_LAYER_CHARACTER, words, count);
    else if (cs2_ieq(words[0], "fg")) layer_command(scene, CS2_LAYER_FOREGROUND, words, count);
    else if (cs2_ieq(words[0], "fw")) layer_command(scene, CS2_LAYER_FACE, words, count);
    else if (cs2_ieq(words[0], "eg")) layer_command(scene, CS2_LAYER_EFFECT, words, count);
    else if (cs2_ieq(words[0], "str")) {
        int32_t slot = 0;
        if (count >= 3 && number(scene, words[1], &slot) == 0) {
            char *shown = cs2_text_display(scene->text, words[2]);
            if (shown != NULL) {
                cs2_vars_set_string(scene->vars, slot, shown);
                free(shown);
            }
        }
    } else if (cs2_ieq(words[0], "next")) {
        if (count >= 2) {
            snprintf(next_script, next_size, "scene.int/%s.cst", words[1]);
            return next_script;
        }
    } else {
        for (size_t i = 0; i < sizeof ignored / sizeof ignored[0]; i++) {
            if (cs2_ieq(words[0], ignored[i])) return NULL;
        }
        note(scene, words[0]);
    }
    return NULL;
}

/* ---- playing ---- */

static void set_message(cs2_scene *scene, const char *value) {
    free(scene->message);
    scene->message = strdup(value == NULL ? "" : value);
}

static char *append_line(char *message, const char *addition) {
    size_t used = message == NULL ? 0 : strlen(message);
    size_t extra = strlen(addition);
    char *grown = realloc(message, used + extra + 2);
    if (grown == NULL) return message;
    if (used > 0) grown[used++] = '\n';
    memcpy(grown + used, addition, extra + 1);
    return grown;
}

void cs2_scene_advance(cs2_scene *scene) {
    char *message = NULL;
    while (scene->cursor < cs2_cst_count(scene->script)) {
        cs2_cst_line line = cs2_cst_at(scene->script, scene->cursor++);
        if (line.type == CS2_CST_NAME) {
            char *shown = cs2_text_display(scene->text, line.text);
            if (shown != NULL) {
                snprintf(scene->speaker, sizeof scene->speaker, "%s", shown);
                free(shown);
            }
        } else if (line.type == CS2_CST_MESSAGE) {
            char *shown = cs2_text_display(scene->text, line.text);
            if (shown != NULL) {
                message = append_line(message, shown);
                free(shown);
            }
        } else if (line.type == CS2_CST_INPUT || line.type == CS2_CST_PAGE) {
            if (message != NULL && message[0] != '\0') {
                set_message(scene, message);
                free(message);
                scene->ended = 0;
                return;
            }
        } else if (line.type == CS2_CST_COMMAND) {
            char next[512];
            const char *jump = run_command(scene, line.text, next, sizeof next);
            if (jump == NULL) continue;
            if (scene->scripts_played >= MAX_SCRIPTS) break;
            if (cs2_scene_play(scene, jump) != 0) {
                note(scene, "next");
                break;
            }
        }
    }
    set_message(scene, message == NULL ? "" : message);
    free(message);
    scene->ended = 1;
}

void cs2_scene_seek(cs2_scene *scene, size_t cursor) {
    scene->cursor = 0;
    scene->ended = 0;
    size_t guard = 0;
    do {
        cs2_scene_advance(scene);
    } while (scene->cursor < cursor && !scene->ended && ++guard < 1000000);
}

const char *cs2_scene_path(const cs2_scene *scene) { return scene->path; }
const char *cs2_scene_speaker(const cs2_scene *scene) { return scene->speaker; }
const char *cs2_scene_text(const cs2_scene *scene) {
    return scene->message == NULL ? "" : scene->message;
}
size_t cs2_scene_cursor(const cs2_scene *scene) { return scene->cursor; }
size_t cs2_scene_line_count(const cs2_scene *scene) { return cs2_cst_count(scene->script); }
int cs2_scene_ended(const cs2_scene *scene) { return scene->ended; }
