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
#include "font.h"
#include "hg3.h"

/*
 * A layout's own numbered variables. Thirty-two was what the title screen
 * needed and it is not what the game needs: twelve of the thirty-five layouts
 * go past it, meswnd.fes as high as \500, and an assignment out of range was
 * simply dropped - which is why the message window read its own "\52 = \0"
 * as nothing and never drew the line it had been given.
 */
#define LOCALS 512
#define CALL_DEPTH 16
#define LINE_BUDGET 20000
#define COMMANDS_SAID 48
#define FONTS 4

typedef struct {
    int section;
    size_t line;
} call_frame;

/* Where a picture sits and how big it is, which is also where a button is. */
typedef struct {
    int known;
    int offset_x, offset_y;
    int width, height;
} object_box;

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
    int boxes_read;          /* its picture has been asked where it is */
    /*
     * A FILE column that is a reference rather than a name: the message window
     * draws every one of its pieces out of "$str900", the game's numbered
     * string for its own window theme. The reference is kept because the
     * string is set while the game runs and can be set again.
     */
    char file_reference[64];
    object_box boxes[CS2_FES_IDS];

    /*
     * A STRING object: the message window's own text. The script appends a
     * line to it and tells it to draw, and it comes out a character at a time
     * at the speed the reader has configured - that is what the window is
     * waiting for while sscript asks it MES_ISDRAW every frame.
     */
    char *text;
    size_t text_bytes;           /* how much of the buffer is used, with the 0 */
    size_t shown;                /* how many bytes of it have been revealed */
    int drawing;
    int frames_each;             /* frames a character takes; 0 is at once */
    int frames_left;
    int point_size;              /* the height its face is drawn at */
    uint32_t colour;

    /*
     * A save panel: `s_panel[n] setsave <slot> <mask> 0` binds the panel to a
     * slot, and everything the panel shows or answers afterwards - `getflag`
     * above all - is about that slot.
     */
    int save_bound;
    int save_slot;
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

    /*
     * The reader. A click is kept as the clicked object's identity, which is
     * its place in the layout plus one, because that is what the screens
     * compare it against; zero is "nothing", which is what the layouts read as
     * no click at all. It is cleared at the end of any frame whose script
     * actually ran, so a click made while the screen is waiting is still there
     * when the screen next looks.
     */
    int32_t click_left, click_right;
    int focus;               /* the object the pad is on, -1 for none */
    int said_buttons;        /* the screen has named its buttons in the log once */
    char said[COMMANDS_SAID][32];  /* the commands it has been sent, said once each */
    int said_count;
    int at_index;            /* what [@] means: the button whose event is running */
    int went;                /* the script has just changed state with next */

    /*
     * The game's own face, at whatever height the STRING objects on this
     * screen ask for. A screen draws its text in one or two sizes - the line
     * and the speaker's name - so a small set is enough and they are opened
     * as they are asked for.
     */
    struct {
        int point_size;
        cs2_font *font;
    } fonts[FONTS];
    int font_count;
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
static int subscript(cs2_layout *layout, const char *bracket);
static int reachable(const cs2_layout *layout, size_t index);
static void ensure_boxes(cs2_layout *layout);
static int object_rect(cs2_layout *layout, const object_state *object,
                       int *x, int *y, int *width, int *height);

/*
 * Which of ID.0.. an object is showing. setid picks one, and a button's two
 * ids are its plain and its lit picture, which is what focus switches between.
 */
static int shown_id(const object_state *object) {
    if (object->current_id > 0 && object->current_id < CS2_FES_IDS
        && object->declared.ids[object->current_id] >= 0) {
        return object->current_id;
    }
    return 0;
}

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
    if (c == '@') {
        /* Inside a button's own event section, the element it happened to. */
        r->at++;
        return r->layout->at_index;
    }
    if (c == '$' || c == '\\') {
        /*
         * Which variable is itself an expression when it is bracketed:
         * meswnd.fes walks its eight buttons with "\\(200+\\0)" over one local
         * rather than writing the eight lines out, the same way a subscript
         * does. Reading the digits gave variable zero for every one of them.
         */
        r->at++;
        int number;
        if (*r->at == '(') {
            r->at++;
            number = (int) expression(r);
            skip_space(r);
            if (*r->at == ')') r->at++;
        } else {
            number = (int) strtol(r->at, (char **) &r->at, 10);
        }
        if (c == '$') {
            return r->layout->host.flag == NULL ? 0
                 : r->layout->host.flag(r->layout->host.context, number);
        }
        if (number < 0 || number >= LOCALS) return 0;
        return r->layout->locals[number];
    }
    if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        const char *word = r->at;
        while (*r->at == '_' || (*r->at >= 'A' && *r->at <= 'Z')
               || (*r->at >= 'a' && *r->at <= 'z')
               || (*r->at >= '0' && *r->at <= '9')) r->at++;
        if (*r->at == '[') {                       /* an element of an array */
            while (*r->at != 0 && *r->at != ']') r->at++;
            if (*r->at == ']') r->at++;
        }
        size_t length = (size_t) (r->at - word);   /* the subscript belongs to it */
        if (strncmp(word, "_EXITCODE_", 10) == 0) {
            return r->layout->child == NULL ? -1 : cs2_layout_result(r->layout->child);
        }
        /*
         * _CLICK_L_ and _CLICK_R_ are what the reader has just clicked, as the
         * object itself: the title screen is written as "_CLICK_L_ ==
         * btn_j[0]". So an object's name answers something of its own - its
         * place in the layout plus one, which is never zero - and zero is
         * "nothing was clicked".
         */
        if (strncmp(word, "_CLICK_L_", 9) == 0) return r->layout->click_left;
        if (strncmp(word, "_CLICK_R_", 9) == 0) return r->layout->click_right;
        if (word[0] == '_') return 0;
        char name[64];
        size_t keep = length + 1 > sizeof name ? sizeof name - 1 : length;
        memcpy(name, word, keep);
        name[keep] = 0;
        size_t from = 0;
        const object_state *object = object_of(r->layout, name, &from);
        return object == NULL ? 0 : (int32_t) from;
    }
    /*
     * Base ten unless the file says otherwise. The save screen writes its
     * masks and its two string kinds in hex - "setsave <slot> 0x400 0",
     * "saveapend_str $805 0x80000000 $str155" - and reading those as decimal
     * gave zero and left the rest of the line unread. Only an explicit 0x is
     * treated as hex: a leading zero in these files is a decimal number with
     * a zero in front of it, not octal.
     */
    if (r->at[0] == '0' && (r->at[1] == 'x' || r->at[1] == 'X')) {
        return (int32_t) (uint32_t) strtoul(r->at, (char **) &r->at, 16);
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

/*
 * Which variable a line is writing. A variable's own number is an expression
 * like everything else in these files, and it is written in brackets when it
 * is one: the save screen fills a row of locals as it walks the slots, one
 * per panel. Reading that number as digits answered 0 every time, so the
 * screen wrote over the counter it was walking with and never came out of
 * its own loop.
 */
static int variable_number(cs2_layout *layout, const char *after_sigil) {
    reader r = { layout, after_sigil };
    skip_space(&r);
    if (*r.at == '(') {
        r.at++;
        return (int) expression(&r);
    }
    return (int) strtol(r.at, NULL, 10);
}

/* -------------------------------------------------------------- the objects */

/*
 * What is inside the brackets. A subscript is not always a number:
 * scenario_select polls its eight buttons as "btn_hit[\\0]" over one local,
 * and a button's own event section says "btn_d[@]" for the element the event
 * happened to. So it is read as an expression, like everything else.
 */
static int subscript(cs2_layout *layout, const char *bracket) {
    reader r = { layout, bracket + 1 };
    return (int) expression(&r);
}

static object_state *object_of(cs2_layout *layout, const char *token, size_t *from) {
    char name[40];
    int index = -1;
    const char *bracket = strchr(token, '[');
    size_t length = bracket == NULL ? strlen(token) : (size_t) (bracket - token);
    if (length + 1 > sizeof name) length = sizeof name - 1;
    memcpy(name, token, length);
    name[length] = 0;
    if (bracket != NULL) index = subscript(layout, bracket);
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
 * What a STRING object is told to hold. "$str1000" is one of the game's
 * numbered strings - the line sscript has just read - and anything else is
 * taken as itself, which is what the few literal appends in the layouts want.
 * The caller frees it.
 */
static char *string_argument(cs2_layout *layout, const char *token) {
    if (strncmp(token, "$str", 4) == 0 && layout->host.text_of != NULL) {
        return layout->host.text_of(layout->host.context, atoi(token + 4));
    }
    size_t length = strlen(token);
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, token, length + 1);
    return copy;
}

static void string_append(object_state *object, const char *added) {
    if (added == NULL || added[0] == 0) return;
    size_t already = object->text_bytes == 0 ? 0 : object->text_bytes - 1;
    size_t length = strlen(added);
    char *grown = realloc(object->text, already + length + 1);
    if (grown == NULL) return;
    memcpy(grown + already, added, length + 1);
    object->text = grown;
    object->text_bytes = already + length + 1;
}

static void string_clear(object_state *object) {
    free(object->text);
    object->text = NULL;
    object->text_bytes = 0;
    object->shown = 0;
    object->drawing = 0;
    object->frames_left = 0;
}

/* One character forward, in bytes, so a two-byte glyph appears whole. */
static size_t next_character(const char *text, size_t at) {
    if (text[at] == 0) return at;
    at++;
    while ((text[at] & 0xc0) == 0x80) at++;
    return at;
}

static void strings_step(cs2_layout *layout) {
    for (size_t i = 0; i < layout->object_count; i++) {
        object_state *object = &layout->objects[i];
        if (!object->drawing || object->text == NULL) continue;
        if (object->frames_each > 0 && --object->frames_left > 0) continue;
        object->frames_left = object->frames_each;
        object->shown = object->frames_each > 0
            ? next_character(object->text, object->shown)
            : object->text_bytes - 1;
        if (object->shown >= object->text_bytes - 1) {
            object->shown = object->text_bytes - 1;
            object->drawing = 0;
        }
    }
}

/*
 * The message window's own vocabulary. A STRING object is a box of text: the
 * script appends a line to it, tells it to draw, and asks it every frame
 * whether it has finished - which is what MES_ISDRAW is, and what the answer
 * goes into \0 for.
 */
static int string_command(cs2_layout *layout, object_state *object,
                          const char *command, const char *rest) {
    if (strcmp(command, "apend") == 0) {
        char token[128] = "";
        sscanf(rest, "%127s", token);
        char *added = string_argument(layout, token);
        string_append(object, added);
        free(added);
        return 1;
    }
    if (strcmp(command, "clear") == 0 || strcmp(command, "reset") == 0) {
        string_clear(object);
        return 1;
    }
    if (strcmp(command, "draw") == 0) {
        object->drawing = object->text != NULL
            && object->shown < object->text_bytes - 1;
        object->frames_left = object->frames_each;
        if (object->text != NULL && object->text[0] != 0) {
            cs2_log("%s.fes draws in %s: %s", cs2_fes_name(layout->fes),
                    object->declared.name, object->text);
        }
        return 1;
    }
    if (strcmp(command, "skip") == 0) {
        object->shown = object->text_bytes == 0 ? 0 : object->text_bytes - 1;
        object->drawing = 0;
        return 1;
    }
    if (strcmp(command, "isdraw") == 0) {
        layout->locals[0] = object->drawing;
        return 1;
    }
    if (strcmp(command, "layout") == 0) {
        char what[32] = "";
        int read = 0;
        if (sscanf(rest, "%31s%n", what, &read) < 1) return 1;
        const char *values = rest + read;
        int a = 0, b = 0, c = 0, d = 0, e = 0;
        int given = sscanf(values, "%d %d %d %d %d", &a, &b, &c, &d, &e);
        if (strcmp(what, "frame") == 0 && given >= 1) {
            object->frames_each = a < 0 ? 0 : a;
        } else if (strcmp(what, "size") == 0 && given >= 1) {
            /*
             * One number, or five. The five are a scale the reader can pick
             * from and the middle of them is the ordinary size: four of the
             * layouts write the same box both ways - "str layout size 24" and
             * "str layout size 18 22 24 36 42", "name layout size 22" and
             * "name layout size 11 11 22 22 22" - and in every one of them
             * the single number is the third of the five.
             */
            object->point_size = given >= 5 ? c : a;
        } else if (strcmp(what, "color") == 0 && given >= 3) {
            /* Nine numbers: three colours. The first is the text itself. */
            object->colour = 0xff000000u | ((uint32_t) (a & 0xff) << 16)
                           | ((uint32_t) (b & 0xff) << 8) | (uint32_t) (c & 0xff);
        }
        (void) d;
        (void) e;
        return 1;
    }
    /* apendmark, userfont, userfontobj, pos2, getpos2, the colours and the
       margins are how the window is dressed, not what it says. */
    return 1;
}

/* The game's own face at one height, opened the first time a screen wants it. */
static const cs2_font *font_at(cs2_layout *layout, int point_size) {
    if (point_size <= 0) return NULL;
    for (int i = 0; i < layout->font_count; i++) {
        if (layout->fonts[i].point_size == point_size) return layout->fonts[i].font;
    }
    if (layout->font_count >= FONTS) return NULL;
    cs2_font *font = cs2_font_open_beside(cs2_files_root(layout->files), point_size);
    if (font == NULL) {
        cs2_log("%s.fes has text to draw and this game ships no font beside its archives",
                cs2_fes_name(layout->fes));
    }
    layout->fonts[layout->font_count].point_size = point_size;
    layout->fonts[layout->font_count].font = font;
    layout->font_count++;
    return font;
}

/*
 * What a command was given. Not numbers: "pl_base disp \\52" is how the message
 * window is shown and "btn_sys[\\0] setid 2" is how its buttons are lit, so an
 * argument is an expression like everything else in these files. Reading them
 * as digits made every one of those zero, and a message window whose plane is
 * told "disp 0" draws nothing at all.
 *
 * One expression per word: the files write their arguments a word apart, and
 * taking them one at a time keeps "fade 16 -1 255" three arguments rather than
 * a subtraction.
 */
static int arguments_of(cs2_layout *layout, const char *rest, int *a, int *b, int *c) {
    int *into[3] = { a, b, c };
    int given = 0;
    while (given < 3) {
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest == 0) break;
        char word[64];
        size_t length = 0;
        while (rest[length] != 0 && rest[length] != ' ' && rest[length] != '\t'
               && length + 1 < sizeof word) {
            word[length] = rest[length];
            length++;
        }
        word[length] = 0;
        rest += length;
        reader r = { layout, word };
        *into[given] = (int) expression(&r);
        given++;
    }
    return given;
}

/*
 * The nth space-separated word of a line, as it stands. Word 0 is the command.
 */
static int word_of(const char *line, int index, char *into, size_t size) {
    for (int i = 0; ; i++) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == 0) return 0;
        size_t length = 0;
        while (line[length] != 0 && line[length] != ' ' && line[length] != '\t') length++;
        if (i == index) {
            if (length + 1 > size) length = size - 1;
            memcpy(into, line, length);
            into[length] = 0;
            return 1;
        }
        line += length;
    }
}

/* And that word as a number, because every argument in these files is an
   expression: "saveexist ($802*\100)+\0 1026". */
static int32_t argument_of(cs2_layout *layout, const char *line, int index) {
    char word[128];
    if (!word_of(line, index, word, sizeof word)) return 0;
    return evaluate(layout, word);
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
    int given = arguments_of(layout, rest, &a, &b, &c);

    from = 0;
    object_state *object;
    while ((object = object_of(layout, token, &from)) != NULL) {
        if (object->declared.kind == CS2_FES_STRING
            && strcmp(command, "disp") != 0) {
            string_command(layout, object, command, rest);
        } else if (strcmp(command, "disp") == 0) object->declared.disp = a;
        else if (strcmp(command, "enable") == 0) object->declared.enable = a;
        else if (strcmp(command, "setid") == 0) object->current_id = a;
        else if (strcmp(command, "delete") == 0) object->deleted = 1;
        else if (strcmp(command, "pos") == 0 && given >= 2) {
            object->declared.x = a;
            object->declared.y = b;
        } else if (strcmp(command, "fade") == 0 && given >= 3) {
            fade_start(object, a, b, c);
        } else if (strcmp(command, "setsave") == 0 && given >= 1) {
            object->save_bound = 1;
            object->save_slot = a;
        } else if (strcmp(command, "getflag") == 0 && given >= 2) {
            /*
             * A flag out of the save the panel is bound to, into one of the
             * game's own flags. -1 where the save does not carry it, which is
             * the answer the load path tests for: "if ($541!=-1)".
             */
            int32_t value = -1;
            if (object->save_bound && layout->host.save_flag != NULL) {
                layout->host.save_flag(layout->host.context, object->save_slot, a, &value);
            }
            if (layout->host.set_flag != NULL) {
                layout->host.set_flag(layout->host.context, b, value);
            }
        }
        /*
         * load, play, stop, blend and the rest are the sound and motion
         * vocabulary; they are not what puts a screen up and are read past.
         *
         * noact is read past with them, and used not to be: it was taken for
         * "disable this button", which cost the console a whole screen. Every
         * screen in the game does it to one element of the row it has just
         * enabled - "btn_d[0] noact 1" on the title, "btn_hit[1] noact 1" on
         * the scenario list - and omake_sysvoice does it to all six of its
         * buttons in turn, each one overriding the last as its condition
         * allows, which no reading of "disable" survives. What it looks like
         * is where the pad starts, set without running the button's action;
         * the console's own screenshot of the scenario list shows the button
         * neither lit nor greyed, so it is left alone until that is settled.
         */
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

/*
 * The statements. One frame of the state a layout is in (one_frame), or one
 * command section run out to its end (not one_frame): a command is not a
 * state, so where a frame would stop and come back next time - the end of the
 * section, a break at the bottom, a wait - the command is simply over.
 */
static void run_lines(cs2_layout *layout, int one_frame) {
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
            return;                        /* and a command is finished */
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
            layout->went = 1;
            /*
             * next is not the end of a frame: the new state runs on in the
             * same pass. scenario_select says so plainly - it polls its eight
             * buttons one state at a time and puts an explicit "wait 1" before
             * going round again, which would be pointless if next itself
             * waited, and a click would have to survive eight frames to be
             * seen. A frame ends where the file says it ends: at a wait, or at
             * the end of a state that has none.
             */
            continue;
        }
        if (strcmp(word, "wait") == 0) {
            const char *rest = line + 4;
            while (*rest == ' ') rest++;
            layout->line++;
            if (one_frame) {
                if (*rest >= '0' && *rest <= '9') layout->wait_frames = atoi(rest);
                else layout->wait_motion = 1;
            }
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
        /*
         * The event a screen sends the system script. The message window's
         * own buttons are written entirely in these: Q.Save is
         * "send 0xffff0003 0 21" and nothing else.
         */
        if (strcmp(word, "send") == 0) {
            uint32_t class_ = (uint32_t) argument_of(layout, line, 1);
            uint32_t which = (uint32_t) argument_of(layout, line, 2);
            uint32_t code = (uint32_t) argument_of(layout, line, 3);
            cs2_log("%s.fes sends %#x %u %u", cs2_fes_name(layout->fes),
                    class_, which, code);
            if (layout->host.post != NULL) {
                layout->host.post(layout->host.context, class_, which, code);
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
        /*
         * The save vocabulary. These are the questions _saveload.fes and
         * flow.fes ask the engine about the game's own saves; loading is not
         * among them, because a load is done by booting the system script on
         * the slot number rather than by a call.
         */
        if (strcmp(word, "saveexist") == 0) {
            int slot = (int) argument_of(layout, line, 1);
            int into = (int) argument_of(layout, line, 2);
            int there = layout->host.save_exists != NULL
                     && layout->host.save_exists(layout->host.context, slot);
            if (layout->host.set_flag != NULL) {
                layout->host.set_flag(layout->host.context, into, there);
            }
            layout->line++;
            continue;
        }
        if (strcmp(word, "newsave") == 0) {
            int into = (int) argument_of(layout, line, 1);
            int from = (int) argument_of(layout, line, 2);
            int to = (int) argument_of(layout, line, 3);
            int newest = layout->host.save_newest == NULL ? -1
                       : layout->host.save_newest(layout->host.context, from, to);
            if (layout->host.set_flag != NULL) {
                layout->host.set_flag(layout->host.context, into, newest);
            }
            layout->line++;
            continue;
        }
        /*
         * "datasave $805 $10". The second argument is what the original hands
         * its own writer beside the slot; what a save holds is the engine's
         * business here, so the slot is the whole of it.
         */
        if (strcmp(word, "datasave") == 0) {
            if (layout->host.save_write != NULL) {
                layout->host.save_write(layout->host.context,
                                        (int) argument_of(layout, line, 1));
            }
            layout->line++;
            continue;
        }
        if (strcmp(word, "savedelete") == 0) {
            if (layout->host.save_delete != NULL) {
                layout->host.save_delete(layout->host.context,
                                         (int) argument_of(layout, line, 1));
            }
            layout->line++;
            continue;
        }
        if (strcmp(word, "saveexchg") == 0 || strcmp(word, "savecopy") == 0) {
            int a = (int) argument_of(layout, line, 1);
            int b = (int) argument_of(layout, line, 2);
            if (word[4] == 'e' && layout->host.save_exchange != NULL) {
                layout->host.save_exchange(layout->host.context, a, b);
            } else if (word[4] == 'c' && layout->host.save_copy != NULL) {
                layout->host.save_copy(layout->host.context, a, b);
            }
            layout->line++;
            continue;
        }
        if (strcmp(word, "saveget_str") == 0) {
            int slot = (int) argument_of(layout, line, 1);
            uint32_t what = (uint32_t) argument_of(layout, line, 2);
            int into = (int) argument_of(layout, line, 3);
            const char *text = layout->host.save_string == NULL ? NULL
                             : layout->host.save_string(layout->host.context, slot, what);
            if (layout->host.set_string != NULL) {
                layout->host.set_string(layout->host.context, into, text == NULL ? "" : text);
            }
            layout->line++;
            continue;
        }
        if (strcmp(word, "saveapend_str") == 0) {
            int slot = (int) argument_of(layout, line, 1);
            uint32_t what = (uint32_t) argument_of(layout, line, 2);
            char token[128] = "";
            word_of(line, 3, token, sizeof token);
            char *text = string_argument(layout, token);
            if (layout->host.save_set_string != NULL) {
                layout->host.save_set_string(layout->host.context, slot, what,
                                             text == NULL ? "" : text);
            }
            free(text);
            layout->line++;
            continue;
        }

        if (line[0] == '$' || line[0] == '\\') {
            const char *equals = strchr(line, '=');
            if (equals != NULL && equals[1] != '=') {
                int32_t value = evaluate(layout, equals + 1);
                int number = variable_number(layout, line + 1);
                if (line[0] == '$') {
                    if (layout->host.set_flag != NULL) {
                        layout->host.set_flag(layout->host.context, number, value);
                    }
                } else if (number >= 0 && number < LOCALS) {
                    layout->locals[number] = value;
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

/*
 * A command from outside. It runs on its own: the state the layout is in is
 * put aside and given back afterwards, so a command sent while the message
 * window is waiting for the reader does not lose its place in #WAIT_USER.
 *
 * A screen that has no section of that name simply does not answer it - a
 * layout implements the commands it is for and no more.
 */
void cs2_layout_send(cs2_layout *layout, const char *command) {
    if (layout == NULL || command == NULL) return;
    int section = cs2_fes_section(layout->fes, command);

    /*
     * Say each command once. The script sends MES_ISDRAW every frame it is
     * waiting, so saying them all would be the whole log; saying each the
     * first time is what tells a reader of the log what a screen was asked to
     * do, and which of the asks it has no answer for.
     */
    int said = 0;
    for (int i = 0; i < layout->said_count; i++) {
        if (strcmp(layout->said[i], command) == 0) said = 1;
    }
    if (!said) {
        if (layout->said_count < COMMANDS_SAID) {
            snprintf(layout->said[layout->said_count], 32, "%s", command);
            layout->said_count++;
        }
        cs2_log("%s.fes is sent %s%s", cs2_fes_name(layout->fes), command,
                section < 0 ? ", which it has no section for" : "");
    }
    if (section < 0) return;

    int was_section = layout->section;
    size_t was_line = layout->line;
    int was_depth = layout->depth;
    call_frame was_stack[CALL_DEPTH];
    memcpy(was_stack, layout->stack, sizeof was_stack);

    layout->section = section;
    layout->line = 0;
    layout->depth = 0;
    run_lines(layout, 0);

    layout->section = was_section;
    layout->line = was_line;
    layout->depth = was_depth;
    memcpy(layout->stack, was_stack, sizeof layout->stack);
}

void cs2_layout_set_local(cs2_layout *layout, int number, int32_t value) {
    if (layout == NULL || number < 0 || number >= LOCALS) return;
    layout->locals[number] = value;
}

int32_t cs2_layout_local(const cs2_layout *layout, int number) {
    if (layout == NULL || number < 0 || number >= LOCALS) return 0;
    return layout->locals[number];
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
    layout->focus = -1;
    layout->at_index = 0;
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
            cs2_set_error("out of memory for the objects of %s.fes", cs2_fes_name(layout->fes));
            return NULL;
        }
        for (size_t i = 0; i < layout->object_count; i++) {
            layout->objects[i].declared = *cs2_fes_object_at(layout->fes, i);
            if (strncmp(layout->objects[i].declared.file, "$str", 4) == 0) {
                snprintf(layout->objects[i].file_reference,
                         sizeof layout->objects[i].file_reference, "%s",
                         layout->objects[i].declared.file);
                layout->objects[i].declared.file[0] = 0;
            }
            layout->objects[i].alpha = 255;
            layout->objects[i].current_id = 0;
        }
    }
    layout->section = cs2_fes_section(layout->fes, "START");
    snprintf(layout->state, sizeof layout->state, "START");
    if (layout->section < 0) {
        cs2_log("%s.fes has no #START", cs2_fes_name(layout->fes));
        layout->section = 0;
    }
    cs2_log("the layout %s.fes is running, %zu objects", cs2_fes_name(layout->fes),
            layout->object_count);
    return layout;
}

void cs2_layout_free(cs2_layout *layout) {
    if (layout == NULL) return;
    cs2_layout_free(layout->child);
    cs2_fes_free(layout->fes);
    for (size_t i = 0; i < layout->object_count; i++) free(layout->objects[i].text);
    for (int i = 0; i < layout->font_count; i++) cs2_font_free(layout->fonts[i].font);
    free(layout->objects);
    free(layout);
}

const char *cs2_layout_name(const cs2_layout *layout) {
    return layout == NULL ? "" : cs2_fes_name(layout->fes);
}

const cs2_layout *cs2_layout_child(const cs2_layout *layout) {
    return layout == NULL ? NULL : layout->child;
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
    strings_step(layout);
    if (layout->wait_frames > 0) {
        layout->wait_frames--;
    } else if (layout->wait_motion && motions_running(layout)) {
        /* still moving */
    } else {
        layout->wait_motion = 0;
        run_lines(layout, 1);
        /* The script has had its look at the click, so it is spent. A click
           made while the screen was waiting is still there when it wakes. */
        layout->click_left = 0;
        layout->click_right = 0;
    }
    /*
     * A screen that has just put a different set of buttons up has taken the
     * focused one away with it - the title screen's BTN_SHOW disables one row
     * and enables another - so the pad is left on nothing rather than on a
     * button that is no longer there. Its lit picture goes back with the same
     * hand that took it, so there is no UNFOCUS to run.
     */
    if (layout->focus >= 0 && !reachable(layout, (size_t) layout->focus)) {
        layout->focus = -1;
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

static const object_state *plane_of(cs2_layout *layout, const char *name) {
    if (name == NULL || name[0] == 0) return NULL;
    char base[40];
    int index = -1;
    const char *bracket = strchr(name, '[');
    size_t length = bracket == NULL ? strlen(name) : (size_t) (bracket - name);
    if (length + 1 > sizeof base) length = sizeof base - 1;
    memcpy(base, name, length);
    base[length] = 0;
    if (bracket != NULL) index = subscript(layout, bracket);
    for (size_t i = 0; i < layout->object_count; i++) {
        const object_state *object = &layout->objects[i];
        if (strcmp(object->declared.name, base) != 0) continue;
        if (index >= 0 && object->declared.index != index) continue;
        return object;
    }
    return NULL;
}

/*
 * The text a STRING object is holding, as far as it has been revealed, inside
 * the box the window picture gives it. Lines are broken at spaces where the
 * next word would not fit, and at the "\n" the message format carries.
 */
static void draw_the_text(cs2_layout *layout, const object_state *object,
                          uint32_t *canvas, int width, int height) {
    if (object->text == NULL || object->shown == 0) return;
    const cs2_font *font = font_at(layout, object->point_size);
    if (font == NULL) return;

    ensure_boxes(layout);
    int at_x = 0, at_y = 0, box_width = 0, box_height = 0;
    if (object_rect(layout, object, &at_x, &at_y, &box_width, &box_height) != 0) {
        return;
    }
    (void) box_height;

    char shown[4096];
    size_t take = object->shown < sizeof shown - 1 ? object->shown : sizeof shown - 1;
    memcpy(shown, object->text, take);
    shown[take] = 0;

    uint32_t colour = object->colour == 0 ? 0xffffffffu : object->colour;
    int line_height = cs2_font_height(font);
    int y = at_y;
    char line[1024];
    size_t used = 0;
    size_t break_at = 0;                 /* the last space the line could end at */
    for (size_t i = 0; i <= take; i++) {
        char ch = shown[i];
        int end_of_line = ch == 0 || ch == '\n';
        if (!end_of_line) {
            if (used + 2 >= sizeof line) end_of_line = 1;
            else {
                line[used] = ch;
                line[used + 1] = 0;
                if (ch == ' ') break_at = used;
                if (box_width > 0 && cs2_font_measure(font, line) > box_width && break_at > 0) {
                    /* Put the word that did not fit on the next line. */
                    i -= used - break_at;
                    used = break_at;
                    line[used] = 0;
                    break_at = 0;
                    end_of_line = 1;
                } else {
                    used++;
                }
            }
        }
        if (!end_of_line) continue;
        line[used] = 0;
        if (used > 0) cs2_font_draw(font, canvas, width, height, at_x, y, line, colour);
        y += line_height;
        used = 0;
        break_at = 0;
        if (ch == 0) break;
    }
}

static void draw_one(cs2_layout *layout, const object_state *object,
                     uint32_t *canvas, int width, int height) {
    if (object->deleted || object->declared.disp == 0) return;
    if (object->declared.kind == CS2_FES_STRING) {
        draw_the_text(layout, object, canvas, width, height);
        return;
    }
    if (object->declared.kind != CS2_FES_IMAGE && object->declared.kind != CS2_FES_BUTTON) return;
    /*
     * A MASK is a shape, not a picture: the face plate's mask is the outline
     * the character's face is cut to, and the file behind it is a flat block
     * of colour. Drawing it as a picture puts that block on the screen, which
     * is what it did until this line; drawing through it is what the object
     * asks for and this engine has no masked blend yet, so it draws neither.
     */
    if (object->declared.mask != 0) return;
    if (object->declared.file[0] == 0) return;
    int id = object->declared.ids[shown_id(object)];
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
    /*
     * Before anything is drawn, because a picture named by a reference has no
     * file until it is resolved and the message window's whole frame is named
     * that way. The answers are kept, so this costs one pass over the objects.
     */
    ensure_boxes(layout);
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

/* ---------------------------------------------------------------- the reader */

/*
 * Where a button is. A .fes gives a button a picture and a place but no size:
 * the size and the offset are the picture's own, out of its HG-3 stdinfo, so
 * they are asked for once per layout and kept. Everything else - which of its
 * ids it is showing, where its plane has been moved to - can change every
 * frame and is worked out at the moment of the question.
 */
/*
 * "$str900:10" is not a place, it is a picture: frame 10 of whatever the
 * game's numbered string 900 holds, which for the message window is
 * sys_mwnd.hg3. So an object laid out that way is given that file and that
 * frame, and then it is an object with a picture like any other - its place
 * and its size are the frame's own offset and size, which is the same rule
 * every button on the title screen is found by.
 */
/*
 * A picture named by a reference rather than by a file: the message window's
 * whole theme is "$str900", the numbered string the system script writes the
 * window picture into (sys_mwnd for Grisaia's default theme), so until that
 * string is set the window has no frame to draw and after it is set every
 * piece of the window draws out of it. The name comes with its suffix and the
 * FILE column is written without one, the same as the box references.
 */
static void resolve_the_file(cs2_layout *layout, object_state *object) {
    if (object->file_reference[0] == 0 || layout->host.string == NULL) return;
    const char *name = layout->host.string(layout->host.context,
                                           atoi(object->file_reference + 4));
    if (name == NULL || name[0] == 0) return;
    char wanted[64];
    snprintf(wanted, sizeof wanted, "%s", name);
    size_t length = strlen(wanted);
    if (length > 4 && cs2_ieq(wanted + length - 4, ".hg3")) wanted[length - 4] = 0;
    if (strcmp(wanted, object->declared.file) == 0) return;
    snprintf(object->declared.file, sizeof object->declared.file, "%s", wanted);
    object->boxes_read = 0;
    cs2_log("%s.fes: %s is %s", cs2_fes_name(layout->fes), object->declared.name,
            object->declared.file);
}

static void resolve_the_box(cs2_layout *layout, object_state *object) {
    const char *from = object->declared.box_from;
    if (from[0] == 0 || object->declared.file[0] != 0) return;
    if (strncmp(from, "$str", 4) != 0) return;
    const char *colon = strchr(from, ':');
    if (colon == NULL || layout->host.string == NULL) return;
    const char *name = layout->host.string(layout->host.context, atoi(from + 4));
    if (name == NULL || name[0] == 0) return;
    snprintf(object->declared.file, sizeof object->declared.file, "%s", name);
    /* The tables name the file with its suffix and everything else without. */
    size_t length = strlen(object->declared.file);
    if (length > 4 && cs2_ieq(object->declared.file + length - 4, ".hg3")) {
        object->declared.file[length - 4] = 0;
    }
    object->declared.ids[0] = atoi(colon + 1);
    /*
     * The strings are the game's and they are set while the game runs, so a
     * reference can be nothing the first time it is looked at and a picture
     * the next. Whatever was read of this object before is out of date.
     */
    object->boxes_read = 0;
    cs2_log("%s.fes: %s is %s frame %d", cs2_fes_name(layout->fes),
            object->declared.name, object->declared.file, object->declared.ids[0]);
}

static void ensure_boxes(cs2_layout *layout) {
    for (size_t i = 0; i < layout->object_count; i++) {
        resolve_the_file(layout, &layout->objects[i]);
        resolve_the_box(layout, &layout->objects[i]);
    }
    for (size_t i = 0; i < layout->object_count; i++) {
        if (layout->objects[i].boxes_read) continue;
        char name[64];
        snprintf(name, sizeof name, "%s", layout->objects[i].declared.file);
        if (name[0] == 0) {
            layout->objects[i].boxes_read = 1;
            continue;
        }
        char path[160];
        snprintf(path, sizeof path, "image.int/%s.hg3", name);
        cs2_bytes file = {0};
        int opened = cs2_files_read(layout->files, path, &file) == 0;
        /* One read of the file answers for every object drawn out of it. */
        for (size_t j = i; j < layout->object_count; j++) {
            object_state *object = &layout->objects[j];
            if (object->boxes_read || strcmp(object->declared.file, name) != 0) continue;
            object->boxes_read = 1;
            if (!opened) continue;
            for (int k = 0; k < CS2_FES_IDS; k++) {
                if (object->declared.ids[k] < 0) continue;
                cs2_hg3_frame frame = {0};
                if (cs2_hg3_bounds_id(file.data, file.size,
                                      object->declared.ids[k], &frame) != 0) {
                    continue;
                }
                object->boxes[k].known = 1;
                object->boxes[k].offset_x = frame.offset_x;
                object->boxes[k].offset_y = frame.offset_y;
                object->boxes[k].width = frame.width;
                object->boxes[k].height = frame.height;
            }
        }
        if (opened) cs2_bytes_free(&file);
    }
}

/* The rectangle an object stands on this frame, on the screen. */
static int object_rect(cs2_layout *layout, const object_state *object,
                       int *x, int *y, int *width, int *height) {
    const object_box *box = &object->boxes[shown_id(object)];
    if (!box->known) box = &object->boxes[0];
    if (!box->known) return -1;
    int at_x = object->declared.x + box->offset_x;
    int at_y = object->declared.y + box->offset_y;
    const object_state *plane = plane_of(layout, object->declared.plane);
    if (plane != NULL) {
        if (plane->deleted || plane->declared.disp == 0) return -1;
        at_x += plane->declared.x + plane->declared.base_x;
        at_y += plane->declared.y + plane->declared.base_y;
    }
    *x = at_x;
    *y = at_y;
    *width = box->width;
    *height = box->height;
    return 0;
}

/*
 * Which button the reader is on. The topmost enabled one wins: the title
 * screen draws its menu as btn_d and lays an invisible btn_j over it at a
 * higher priority, and it is btn_j the screen's script compares against. A
 * button that is not displayed is still a button - that invisible row is the
 * whole point - so it is ENABLE, not DISP, that decides.
 */
static int hit(cs2_layout *layout, int x, int y) {
    ensure_boxes(layout);
    /*
     * The first time a reader touches a screen, the screen says what it is
     * offering and where. A tap that does nothing is otherwise silent, and on
     * a console that is the whole of what there is to go on.
     */
    if (!layout->said_buttons) {
        layout->said_buttons = 1;
        for (size_t i = 0; i < layout->object_count; i++) {
            const object_state *object = &layout->objects[i];
            if (object->declared.kind != CS2_FES_BUTTON) continue;
            if (object->deleted || object->declared.enable == 0) continue;
            int at_x, at_y, width, height;
            if (object_rect(layout, object, &at_x, &at_y, &width, &height) != 0) continue;
            cs2_log("%s.fes offers %s[%d] at %d,%d %dx%d", cs2_fes_name(layout->fes),
                    object->declared.name, object->declared.index,
                    at_x, at_y, width, height);
        }
    }
    int best = -1;
    int best_priority = 0;
    for (size_t i = 0; i < layout->object_count; i++) {
        const object_state *object = &layout->objects[i];
        if (object->declared.kind != CS2_FES_BUTTON) continue;
        if (object->deleted || object->declared.enable == 0) continue;
        int at_x, at_y, width, height;
        if (object_rect(layout, object, &at_x, &at_y, &width, &height) != 0) continue;
        if (x < at_x || y < at_y || x >= at_x + width || y >= at_y + height) continue;
        if (best < 0 || object->declared.priority >= best_priority) {
            best = (int) i;
            best_priority = object->declared.priority;
        }
    }
    return best;
}

/*
 * A button's own section, run where it stands. #btn_j.FOCUS and its like are
 * not states: they run through and the screen goes on being what it was, so
 * where the screen had got to is put back afterwards - unless the section
 * itself said next, which is a screen deciding to be somewhere else and is
 * left to stand.
 */
static void run_event(cs2_layout *layout, int section, int at_index) {
    int at_was = layout->at_index;
    int section_was = layout->section;
    size_t line_was = layout->line;
    int depth_was = layout->depth;
    int went_was = layout->went;
    call_frame stack_was[CALL_DEPTH];
    memcpy(stack_was, layout->stack, sizeof stack_was);

    layout->at_index = at_index;
    layout->section = section;
    layout->line = 0;
    layout->depth = 0;
    layout->went = 0;
    run_lines(layout, 1);
    int went = layout->went;

    layout->at_index = at_was;
    layout->went = went_was;
    if (went) return;
    layout->section = section_was;
    layout->line = line_was;
    layout->depth = depth_was;
    memcpy(layout->stack, stack_was, sizeof stack_was);
}

/* #<object>.<event>, if the layout has written one. */
static void fire(cs2_layout *layout, int index, const char *event) {
    if (index < 0) return;
    const object_state *object = &layout->objects[index];
    char name[80];
    snprintf(name, sizeof name, "%s.%s", object->declared.name, event);
    int section = cs2_fes_section(layout->fes, name);
    if (section < 0) return;
    run_event(layout, section, object->declared.index < 0 ? 0 : object->declared.index);
}

static void set_focus(cs2_layout *layout, int index) {
    if (index == layout->focus) return;
    int leaving = layout->focus;
    layout->focus = index;
    fire(layout, leaving, "UNFOCUS");
    fire(layout, index, "FOCUS");
}

/* A button the pad can be on: enabled, still there, and placed in a grid. */
static int reachable(const cs2_layout *layout, size_t index) {
    const object_state *object = &layout->objects[index];
    return object->declared.kind == CS2_FES_BUTTON && !object->deleted
        && object->declared.enable != 0 && object->declared.block >= 0;
}

/*
 * The pad, over the KEYBLOCK grids. Up and down walk a column, left and right
 * walk a row, and running off one end comes back at the other, which is what a
 * five-item menu on a console wants. With nothing focused yet the pad simply
 * lands on the first button of the lowest grid that has any.
 */
static void move_focus(cs2_layout *layout, int dx, int dy) {
    if (layout->focus < 0 || !reachable(layout, (size_t) layout->focus)) {
        int block = -1;
        int first = -1;
        for (size_t i = 0; i < layout->object_count; i++) {
            if (!reachable(layout, i)) continue;
            const cs2_fes_object *object = &layout->objects[i].declared;
            if (block < 0 || object->block < block) {
                block = object->block;
                first = -1;
            }
            if (object->block != block) continue;
            if (first < 0) {
                first = (int) i;
                continue;
            }
            const cs2_fes_object *best = &layout->objects[first].declared;
            if (object->column < best->column
                || (object->column == best->column && object->row < best->row)) {
                first = (int) i;
            }
        }
        if (first >= 0) set_focus(layout, first);
        return;
    }

    const cs2_fes_object *from = &layout->objects[layout->focus].declared;
    int step = dy != 0 ? dy : dx;
    if (step == 0) return;
    int fixed = dy != 0 ? from->column : from->row;
    int here = dy != 0 ? from->row : from->column;
    int next = -1, next_at = 0;
    int wrapped = -1, wrapped_at = 0;
    for (size_t i = 0; i < layout->object_count; i++) {
        if (!reachable(layout, i) || (int) i == layout->focus) continue;
        const cs2_fes_object *object = &layout->objects[i].declared;
        if (object->block != from->block) continue;
        int along = dy != 0 ? object->row : object->column;
        if ((dy != 0 ? object->column : object->row) != fixed) continue;
        if ((along - here) * step > 0
            && (next < 0 || (along - here) * step < (next_at - here) * step)) {
            next = (int) i;
            next_at = along;
        }
        if (wrapped < 0 || (along - wrapped_at) * step < 0) {
            wrapped = (int) i;
            wrapped_at = along;
        }
    }
    if (next < 0) next = wrapped;
    if (next >= 0) set_focus(layout, next);
}

/* The screen the reader is looking at: the innermost layout. */
static cs2_layout *active(cs2_layout *layout) {
    while (layout != NULL && layout->child != NULL) layout = layout->child;
    return layout;
}

void cs2_layout_pointer(cs2_layout *layout, int x, int y) {
    cs2_layout *screen = active(layout);
    if (screen == NULL) return;
    set_focus(screen, hit(screen, x, y));
}

int cs2_layout_click(cs2_layout *layout, int x, int y, int button) {
    cs2_layout *screen = active(layout);
    if (screen == NULL) return 0;
    int index = hit(screen, x, y);
    if (button == 2) {
        /* _CLICK_R_ is the layout language's "was there a right click", and a
           screen is entitled to ask only that: a right click on no button at
           all is still a right click, so it answers -1 rather than nothing. */
        screen->click_right = index >= 0 ? (int32_t) index + 1 : -1;
        fire(screen, index, "PUSH_R");
        return index >= 0;
    }
    set_focus(screen, index);
    screen->click_left = index >= 0 ? (int32_t) index + 1 : 0;
    if (index >= 0) {
        cs2_log("%s.fes: %d,%d is %s[%d]", cs2_fes_name(screen->fes), x, y,
                screen->objects[index].declared.name,
                screen->objects[index].declared.index);
    } else {
        cs2_log("%s.fes: %d,%d is not a button of this screen",
                cs2_fes_name(screen->fes), x, y);
    }
    fire(screen, index, "PUSH_L");
    return index >= 0;
}

int cs2_layout_press(cs2_layout *layout, cs2_layout_key key) {
    cs2_layout *screen = active(layout);
    if (screen == NULL) return 0;
    switch (key) {
    case CS2_LAYOUT_UP:      move_focus(screen, 0, -1); return 1;
    case CS2_LAYOUT_DOWN:    move_focus(screen, 0, 1); return 1;
    case CS2_LAYOUT_LEFT:    move_focus(screen, -1, 0); return 1;
    case CS2_LAYOUT_RIGHT:   move_focus(screen, 1, 0); return 1;
    case CS2_LAYOUT_CONFIRM:
        /*
         * Confirm with nothing focused is the pad arriving at the screen - and
         * a screen with nothing to arrive at, which is what the message window
         * is while a line is being read, has not taken the press at all.
         */
        if (screen->focus < 0 || !reachable(screen, (size_t) screen->focus)) {
            move_focus(screen, 0, 1);
            return screen->focus >= 0;
        }
        screen->click_left = (int32_t) screen->focus + 1;
        fire(screen, screen->focus, "PUSH_L");
        return 1;
    case CS2_LAYOUT_CANCEL:
        screen->click_right = -1;
        return 0;
    }
    return 0;
}
