/*
 * The engine functions the game's own system script calls.
 *
 * Each one is written from its handler in the game's engine, and the numbering
 * is that engine's table at 0x0076D948. What each does, and how it was read, is
 * in /root/re/kcs/KCS-FORMAT.md. Anything not written here answers zero and
 * says so once in the log, which is how the boot path of a game names the
 * functions it needs.
 *
 * The return codes are the game's own and are not a matter of taste: they were
 * read off each handler, because a function that answers a value where the
 * original answered none leaves a word on the script's stack that the next
 * statement will take for its own.
 */
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs2.h"
#include "hg3.h"
#include "layout.h"
#include "scene.h"
#include "startup.h"

#define DOCUMENT_LIMIT 8
#define LAYOUT_LIMIT 8
#define VARIABLE_BYTES 0x10000u
#define SYSTEM_VARIABLES 256
#define SYSTEM_STRINGS 128
#define NAMED_NUMBERS 512
#define WAITING_LIMIT 4     /* the scripts that can be waiting for an event at once */
#define EVENT_LIMIT 16      /* how many events one of them can be behind by */
#define NAME_BYTES 32
#define VOICE_LIMIT 24     /* the sounds the script can be holding at once */

/*
 * A system variable, as 210 and 211 pass them about: the scripts hand the
 * engine a number and a value and read the value back later. The number looks
 * like an address into the persistent block, but the scripts take it out of a
 * table the engine fills in, so what it means is the engine's business and not
 * the script's - which is exactly why it is kept here rather than written into
 * the script's memory, where a number that is not an address would land on
 * whatever happens to be at that offset.
 */
typedef struct {
    uint32_t key;
    uint32_t value;
} system_variable;

/*
 * The game's other bank of variables: strings by number. adv.xml gives the
 * numbers names - the file a new game starts on is string 200, the window
 * theme 800 - and a script reaches one by writing "$str200" into a buffer and
 * asking function 278 to resolve it.
 */
typedef struct {
    uint32_t key;
    char *value;                 /* a line of dialogue is as long as it is */
} system_string;

/*
 * A name the system script has given a number to. 517 puts one in, 518 takes
 * it out, and the table it goes in is named by the script own object, so the
 * scene commands and the motion names do not share one. This is how a scene
 * script command is dispatched: sscript registers all 259 of its command
 * names against their own numbers, then looks each command up by name.
 */
typedef struct {
    uint32_t table;
    char name[NAME_BYTES];
    int32_t number;
} named_number;

struct cs2_system {
    cs2_files *files;
    cs2_audio *audio;            /* the mixer 936 plays through, or NULL */
    /*
     * The voices the script is holding. The game's own engine answers 936
     * with a pointer to the sound it started and the script keeps that in its
     * sound entry, so what a voice IS is the engine's business: here it is a
     * bank of the mixer, and the number the script keeps is this slot plus one
     * (the script writes -1 for "no voice", so zero is never handed out).
     */
    struct {
        int in_use;
        int kind;
        int bank;
    } voices[VOICE_LIMIT];
    cs2_planes *planes;
    int width, height;
    uint64_t frame;              /* the game's own sixty a second */
    uint8_t *variables;          /* the persistent block every script shares */
    cs2_startup *documents[DOCUMENT_LIMIT];
    int scenario;                /* 789 answers this; 209 makes its interpreter */
    int interpreter;
    cs2_startup *settings;       /* the game's startup.xml, for the message format */
    cs2_text *format;
    cs2_scene *scene;            /* what 325 has loaded, and 330 steps */
    int scene_loaded;
    int scene_step;              /* the scene has something to run this frame */
    int boot_type;
    char boot_scenario[128];
    /*
     * The layouts that are running. A started plane runs one, and more than
     * one plane is started at a time: the "main" plane runs flow.fes, the
     * whole shape of the game, and the message window plane runs meswnd.fes
     * over it for as long as a scene is being read.
     */
    struct {
        cs2_plane plane;
        cs2_layout *layout;
    } layouts[LAYOUT_LIMIT];
    size_t layout_count;
    cs2_kcs *flow;               /* the system script the layout has run */
    /*
     * The events each waiting script has still to be told about. A script
     * registers itself the first time it waits (71), and an event is posted to
     * every script that is waiting, not to the first one to ask: the game's own
     * engine keeps the block address on the script (Grisaia2.bin, [script+0x70])
     * rather than on the engine, so two scripts waiting at once each get it.
     */
    struct {
        cs2_kcs *script;
        uint32_t queue[EVENT_LIMIT][4];
        size_t count;
    } waiting[WAITING_LIMIT];
    size_t waiting_count;
    int boot_override;           /* the runner was told which boot to force */
    /*
     * The image list and the shelf of it the next picture goes on. main.kcs
     * makes one plane to hold every picture a scene puts on the screen and
     * keeps it in the persistent variables; before it makes or takes away a
     * picture the system script sends that plane 0x114 with the layer's own
     * depth, which says which shelf the work is on, and the pictures on a
     * lower shelf are drawn first.
     */
    cs2_plane image_list;
    int image_shelf;
    system_variable system_variables[SYSTEM_VARIABLES];
    size_t system_variable_count;
    system_string system_strings[SYSTEM_STRINGS];
    size_t system_string_count;
    named_number names[NAMED_NUMBERS];
    size_t name_count;
    int finished;
};

static uint32_t *system_variable_at(cs2_system *system, uint32_t key, int make) {
    for (size_t i = 0; i < system->system_variable_count; i++) {
        if (system->system_variables[i].key == key) return &system->system_variables[i].value;
    }
    if (!make || system->system_variable_count >= SYSTEM_VARIABLES) return NULL;
    system_variable *fresh = &system->system_variables[system->system_variable_count++];
    fresh->key = key;
    fresh->value = 0;
    return &fresh->value;
}

static system_string *system_string_at(cs2_system *system, uint32_t key, int make) {
    for (size_t i = 0; i < system->system_string_count; i++) {
        if (system->system_strings[i].key == key) return &system->system_strings[i];
    }
    if (!make || system->system_string_count >= SYSTEM_STRINGS) return NULL;
    system_string *fresh = &system->system_strings[system->system_string_count++];
    fresh->key = key;
    fresh->value = NULL;
    return fresh;
}

/*
 * These held 259 bytes each, and a line of Grisaia's dialogue is longer than
 * that: the message window was given its first line with the last third of it
 * cut off. A numbered string is as long as what is put in it.
 */
void cs2_system_set_string(cs2_system *system, uint32_t number, const char *text) {
    system_string *slot = system_string_at(system, number, 1);
    if (slot == NULL) return;
    if (text == NULL) text = "";
    size_t length = strlen(text) + 1;
    char *kept = realloc(slot->value, length);
    if (kept == NULL) return;
    memcpy(kept, text, length);
    slot->value = kept;
}

const char *cs2_system_string(cs2_system *system, uint32_t number) {
    const system_string *slot = system_string_at(system, number, 0);
    return slot == NULL ? NULL : slot->value;
}

void cs2_system_set_variable(cs2_system *system, uint32_t key, uint32_t value) {
    uint32_t *slot = system_variable_at(system, key, 1);
    if (slot != NULL) *slot = value;
}

/*
 * The words of one line of the scenario, as the system script counts them: the
 * command first and its arguments after, separated by spaces.
 */
/*
 * Where a word of the scenario is. The system script addresses a scene script
 * as (line, word): a line is a run of words, and the file says where each run
 * begins, so this is the one place the two are put together.
 */
static int scene_word(cs2_system *system, int32_t line, int32_t index, size_t *at) {
    if (!system->scene_loaded) return -1;
    size_t first = 0, count = 0;
    if (line < 0 || cs2_scene_line_words(system->scene, (size_t) line, &first, &count) != 0) {
        return -1;
    }
    if (index < 0 || (size_t) index >= count) return -1;
    *at = first + (size_t) index;
    return 0;
}

/*
 * What kind of word it is, as the engine answers it: the word's own type byte
 * out of the file - a piece of the message, a name, a pause, a page, a command
 * - and -1 where there is no such word. A line asked for with a negative word
 * answers how many words it holds, and a negative line answers how many lines
 * the script has, which is how the system script measures a scenario.
 */
static int32_t scene_kind(cs2_system *system, int32_t line, int32_t index) {
    if (!system->scene_loaded) return -1;
    size_t lines = cs2_scene_line_count(system->scene);
    if (line < 0) return (int32_t) lines;
    if ((size_t) line >= lines) return -1;
    size_t first = 0, count = 0;
    if (cs2_scene_line_words(system->scene, (size_t) line, &first, &count) != 0) return -1;
    if (index < 0) return (int32_t) count;
    if ((size_t) index >= count) return -1;
    unsigned type = 0;
    if (cs2_scene_word(system->scene, first + (size_t) index, &type) == NULL) return -1;
    return (int32_t) type;
}

cs2_system *cs2_system_new(cs2_files *files, int width, int height) {
    cs2_system *system = calloc(1, sizeof *system);
    if (system == NULL) {
        cs2_set_error("out of memory for the engine's system functions");
        return NULL;
    }
    system->files = files;
    system->width = width;
    system->height = height;
    system->planes = cs2_planes_new(width, height);
    system->variables = calloc(1, VARIABLE_BYTES);
    if (system->planes == NULL || system->variables == NULL) {
        cs2_system_free(system);
        cs2_set_error("out of memory for the engine's system functions");
        return NULL;
    }
    system->scenario = 1;
    system->boot_type = -20;

    /*
     * The scene player the front end drives. It needs the game's own message
     * substitutions, which live in startup.xml beside the archives.
     */
    cs2_bytes document = {0};
    if (cs2_files_read(files, "config.int/startup.xml", &document) == 0
        || cs2_files_read(files, "startup.xml", &document) == 0) {
        system->settings = cs2_startup_parse(document.data, document.size);
        cs2_bytes_free(&document);
    }
    system->format = cs2_text_from(system->settings);
    system->scene = system->format == NULL ? NULL : cs2_scene_new(files, system->format);
    if (system->scene == NULL) {
        cs2_system_free(system);
        cs2_set_error("out of memory for the scenario the front end plays");
        return NULL;
    }
    return system;
}

void cs2_system_free(cs2_system *system) {
    if (system == NULL) return;
    for (int i = 0; i < DOCUMENT_LIMIT; i++) cs2_startup_free(system->documents[i]);
    if (system->flow != NULL) {
        /* The block is the engine's; take it back before the script frees it. */
        cs2_kcs_set_variables(system->flow, NULL, 0);
        cs2_kcs_free(system->flow);
    }
    cs2_planes_free(system->planes);
    cs2_scene_free(system->scene);
    cs2_text_free(system->format);
    cs2_startup_free(system->settings);
    for (size_t i = 0; i < system->system_string_count; i++) {
        free(system->system_strings[i].value);
    }
    free(system->variables);
    free(system);
}

/*
 * The reader has asked for the next thing. A scenario on the screen advances
 * on it, which is what reading the game is.
 *
 * The system script is NOT told here. It is told with an event, and an event
 * is a block the script reads a code out of rather than a number - see the
 * frame wait below - so a number put where a code belongs is read as one of
 * the game's own codes and does something else entirely.
 */
void cs2_system_event(cs2_system *system, uint32_t event) {
    (void) event;
    if (system->scene_loaded) system->scene_step = 1;
}

/*
 * An event, as the game's own scripts read one.
 *
 * It is a block of four words - a sender, a class, a word the class gives a
 * meaning to, and a code - and it is what a layout's `send` statement writes
 * ("send 0xffff0003 0 20" is meswnd.fes's Load button) and what the original
 * engine's window turns a message into. sscript's frame is a loop around 71:
 * every event is dispatched before the body of the frame runs.
 */
void cs2_system_post(cs2_system *system, uint32_t class_, uint32_t word, uint32_t code) {
    if (system == NULL) return;
    for (size_t i = 0; i < system->waiting_count; i++) {
        if (system->waiting[i].count >= EVENT_LIMIT) continue;
        uint32_t *block = system->waiting[i].queue[system->waiting[i].count++];
        block[0] = 0;
        block[1] = class_;
        block[2] = word;
        block[3] = code;
    }
}

const cs2_scene *cs2_system_scenario(const cs2_system *system) {
    if (system == NULL || !system->scene_loaded) return NULL;
    return system->scene;
}

/* The sound device, for the scenario the front end plays. */
void cs2_system_set_audio(cs2_system *system, cs2_audio *audio) {
    if (system == NULL) return;
    system->audio = audio;
    cs2_scene_set_audio(system->scene, audio);
}

void cs2_system_set_boot(cs2_system *system, int type, const char *scenario) {
    if (system == NULL) return;
    system->boot_override = 1;
    system->boot_type = type;
    snprintf(system->boot_scenario, sizeof system->boot_scenario, "%s",
             scenario == NULL ? "" : scenario);
}

const cs2_planes *cs2_system_planes(const cs2_system *system) {
    return system == NULL ? NULL : system->planes;
}

void *cs2_system_variables(cs2_system *system, uint32_t *size) {
    if (size != NULL) *size = VARIABLE_BYTES;
    return system == NULL ? NULL : system->variables;
}

int cs2_system_finished(const cs2_system *system) {
    return system == NULL ? 0 : system->finished;
}

const cs2_kcs *cs2_system_script(const cs2_system *system) {
    return system == NULL ? NULL : system->flow;
}

/*
 * The return codes of the engine functions the game asks for and this engine
 * has not written yet, read off each handler in Grisaia2.bin with
 * /root/re/kcs/gcallret.py. They are here rather than guessed because the code
 * is what keeps the script's stack straight: with them a run walks the whole
 * front end and names everything it wanted, and without them it stops at the
 * first function whose answer nobody takes.
 */
static const struct {
    uint16_t id;
    uint8_t code;
} unwritten_return_codes[] = {
    {35, 2},   {78, 2},   {84, 2},   {85, 2},   {101, 2},  {143, 0},  {156, 2},
    {207, 2},  {238, 1},  {244, 1},  {294, 1},  {301, 2},  {311, 1},  {315, 1},
    {316, 1},  {323, 1},  {328, 1},  {329, 1},  {342, 2},  {344, 2},  {345, 2},
    {389, 1},  {402, 2},  {404, 2},  {419, 1},  {434, 2},  {455, 2},  {458, 2},
    {466, 2},  {515, 1},  {542, 2},  {572, 2},  {586, 2},  {594, 1},
    {633, 1},  {696, 1},  {739, 2},  {741, 1},
    {785, 2},  {790, 1},  {863, 1},  {866, 1},  {925, 1}
};

/* ------------------------------------------------------- strings by hand */

/*
 * The three string helpers the scene-command dispatcher is built out of, each
 * written from its own handler. They are here rather than in a string module
 * because the shapes are the game own and not general: a token is what its
 * tokeniser calls a token, brackets and all.
 */
#define TEXT_BYTES 4096

/*
 * Splits a line the way the game does (0x00558DC0): a run of delimiters ends
 * one token, and a bracketed group is ONE token however many delimiters are
 * inside it, so "(1 + 2)" is a single argument. A group written "(:...)" is
 * handed on without its own brackets. Answers how many tokens were written,
 * and fills each of them into out, one after another.
 */
static size_t tokenise(const char *src, char *out, size_t out_size, size_t *starts,
                       size_t start_limit) {
    size_t count = 0, at = 0;
    int ended = 1;                 /* the last thing written closed a token */
    char previous = 0;
    for (size_t i = 0; src[i] != 0 && at + 2 < out_size;) {
        if (src[i] == 0x28) {      /* a bracketed group */
            size_t depth = 0, end = i;
            while (src[end] != 0) {
                if (src[end] == 0x28) depth++;
                else if (src[end] == 0x29) { end++; if (--depth == 0) break; continue; }
                end++;
            }
            /*
             * After a letter it is a fresh token - "if (a==1)" is two - but
             * after anything else it belongs to what is being written, which
             * is what makes "#194=(3+4)" one.
             */
            int letter = (previous >= 0x61 && previous <= 0x7a)
                      || (previous >= 0x41 && previous <= 0x5a);
            if (!ended && letter) { out[at++] = 0; ended = 1; }
            int bare = src[i + 1] == 0x3a;
            size_t from = bare ? i + 2 : i;
            size_t to = bare && end > i + 2 ? end - 1 : end;
            if (ended) {
                if (count < start_limit) starts[count] = at;
                count++;
                ended = 0;
            }
            for (size_t k = from; k < to && at + 2 < out_size; k++) previous = out[at++] = src[k];
            i = end;
            continue;
        }
        if (src[i] == 0x20) {      /* a run of these ends one token */
            if (!ended) { out[at++] = 0; ended = 1; }
            i++;
            continue;
        }
        if (ended) {
            if (count < start_limit) starts[count] = at;
            count++;
            ended = 0;
        }
        previous = out[at++] = src[i++];
    }
    if (!ended) out[at++] = 0;
    return count;
}

/* Every occurrence of one string swapped for another (0x00558C40). */
static void replace_all(const char *src, const char *find, const char *with,
                        char *out, size_t out_size) {
    size_t at = 0, find_length = strlen(find), with_length = strlen(with);
    if (find_length == 0) {
        snprintf(out, out_size, "%s", src);
        return;
    }
    for (size_t i = 0; src[i] != 0 && at + 1 < out_size;) {
        if (strncmp(src + i, find, find_length) == 0) {
            for (size_t k = 0; k < with_length && at + 1 < out_size; k++) out[at++] = with[k];
            i += find_length;
        } else {
            out[at++] = src[i++];
        }
    }
    out[at] = 0;
}

/* Puts a string where the script asked for it. */
/*
 * The game's string list, which is one buffer holding several strings one
 * after another (0x00558F00). A run of separators becomes ONE end mark however
 * long the run is, a separator at the front is passed over, and the whole ends
 * with two of them - so "a,,b," is the two strings "a" and "b" and nothing
 * else. Answers how many bytes were written, the two end marks included.
 */
static size_t list_build(const char *src, const char *separators, char *out, size_t out_size) {
    size_t at = 0;
    int ended = 1;                 /* the last thing written ended a string */
    for (; *src != 0 && at + 2 < out_size; src++) {
        if (strchr(separators, *src) == NULL) {
            out[at++] = *src;
            ended = 0;
        } else if (!ended) {
            out[at++] = 0;
            ended = 1;
        }
    }
    out[at++] = 0;
    out[at++] = 0;
    return at;
}

/*
 * One string out of such a list (0x00558C00): walking past `index` end marks,
 * answering nothing when the walk runs off the end or lands on an empty
 * string. `size` bounds the walk, because a list is read out of the script's
 * own memory and a script is not to be trusted to have ended one.
 */
static const char *list_field(const char *list, size_t size, uint32_t index) {
    size_t at = 0;
    for (uint32_t seen = 0; seen < index; seen++) {
        while (at < size && list[at] != 0) at++;
        if (at >= size) return NULL;
        at++;                      /* over the end mark */
    }
    return (at < size && list[at] != 0) ? list + at : NULL;
}

/*
 * The object-message protocol.
 *
 * A CatSystem2 object is not called, it is sent a numbered message: every one
 * of these engine functions ends in one virtual method, the object system's
 * slot at +0x10, given the owner, the object, a message number and up to three
 * things to carry. 258 is 0x114, 520 asks 0x174, 853 is 0x1EA, 854 0x1EB, 855
 * 0x1EC, 936 0x21. One shape, a table of numbers on top of it.
 *
 * The objects are the planes. main.kcs makes the image list with function 66
 * and stores that handle in the persistent variables at offset 8, which is the
 * [0x27918] sscript sends its per-frame 0x114 to; so a message's target is a
 * handle out of the same table this engine already keeps its planes in, and
 * this is where the two meet.
 *
 * What each message MEANS is not written yet, and it is deliberately not
 * guessed: this says what was sent and to what, so the next reading of the
 * original starts from the traffic the game actually makes rather than from a
 * list of function numbers. An answer of 0 is what every one of these
 * functions gave before, so nothing above it changes behaviour today.
 */
static uint32_t object_message(cs2_system *system, cs2_kcs *script, uint32_t target,
                               uint32_t message, const uint32_t *values, size_t count) {
    char carried[128];
    size_t at = 0;
    for (size_t i = 0; i < count && at + 24 < sizeof carried; i++) {
        at += (size_t) snprintf(carried + at, sizeof carried - at, i == 0 ? "%d" : ", %d",
                                (int) (int32_t) values[i]);
    }
    const cs2_plane_state *plane = cs2_plane_get(system->planes, target);
    cs2_log("%s: object %u (%s) is sent %#x (%s)", cs2_kcs_name(script), target,
            plane == NULL ? "no plane of this screen" : plane->name, message,
            at == 0 ? "" : carried);
    return 0;
}


/* ------------------------------------------------- the pictures a scene draws */

/*
 * A scene's picture is an object of the game's own class "image": the system
 * script asks for one by name (710), is given a handle, asks it how big it is
 * (714), puts it where the layer says (726) and throws it away again (711).
 * Grisaia2.bin's 0x0051D620 builds it through the class factory at 0x008A8158
 * under the name "image" and hangs it on the owner's list; here it is a plane
 * carrying the picture's pixels, on the shelf of the image list the last 0x114
 * named, which is the same order on the screen.
 *
 * The name is a file and nothing else - no folder, no extension and no frame
 * id, unlike a .fes, which names a file AND the frame in it - so the picture is
 * the file's first frame.
 */
static cs2_plane scene_image_load(cs2_system *system, const char *name) {
    if (name == NULL || name[0] == 0) return 0;
    char path[192];
    snprintf(path, sizeof path, "image.int/%s.hg3", name);
    cs2_bytes file = {0};
    if (cs2_files_read(system->files, path, &file) != 0) {
        cs2_log("the scene wants %s, which is not in this copy of the game", path);
        return 0;
    }
    cs2_hg3_frame image = {0};
    int decoded = cs2_hg3_decode(file.data, file.size, 0, &image);
    cs2_bytes_free(&file);
    if (decoded != 0) {
        cs2_log("%s: %s", path, cs2_error());
        return 0;
    }
    cs2_plane handle = cs2_plane_create(system->planes, 0, system->image_list, name);
    cs2_plane_state *plane = cs2_plane_get(system->planes, handle);
    if (plane == NULL) {
        cs2_hg3_frame_free(&image);
        return 0;
    }
    const cs2_plane_state *list = cs2_plane_get(system->planes, system->image_list);
    plane->priority = (list == NULL ? 0.0f : list->priority) + (float) system->image_shelf;
    plane->visible = 1;
    plane->width = (float) image.width;
    plane->height = (float) image.height;
    cs2_log("the scene draws %s, %dx%d at %d,%d on shelf %d", path, image.width, image.height,
            image.offset_x, image.offset_y, system->image_shelf);
    cs2_plane_set_picture(system->planes, handle, image.pixels, image.width, image.height,
                          image.offset_x, image.offset_y);
    image.pixels = NULL;
    cs2_hg3_frame_free(&image);
    return handle;
}

static void write_word(cs2_kcs *script, uint32_t address, uint32_t value) {
    void *out = cs2_kcs_at(script, address, (uint32_t) sizeof value);
    if (out != NULL) memcpy(out, &value, sizeof value);
}

static void write_text(cs2_kcs *script, uint32_t address, const char *text) {
    size_t length = strlen(text) + 1;
    char *out = cs2_kcs_at(script, address, (uint32_t) length);
    if (out != NULL) memcpy(out, text, length);
}

static named_number *named_number_at(cs2_system *system, uint32_t table,
                                     const char *name, int make) {
    for (size_t i = 0; i < system->name_count; i++) {
        named_number *entry = &system->names[i];
        if (entry->table == table && strcmp(entry->name, name) == 0) return entry;
    }
    if (!make || system->name_count >= NAMED_NUMBERS) return NULL;
    named_number *fresh = &system->names[system->name_count++];
    fresh->table = table;
    snprintf(fresh->name, sizeof fresh->name, "%s", name);
    fresh->number = 0;
    return fresh;
}

/* ------------------------------------------------------- reading arguments */

static uint32_t arg(const uint8_t *arguments, uint32_t size, uint32_t index) {
    return cs2_kcs_argument(arguments, size, index);
}

static float argf(const uint8_t *arguments, uint32_t size, uint32_t index) {
    uint32_t word = cs2_kcs_argument(arguments, size, index);
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

static uint32_t from_float(float value) {
    uint32_t word;
    memcpy(&word, &value, sizeof word);
    return word;
}

static cs2_plane_state *plane_of(cs2_system *system, const uint8_t *arguments,
                                 uint32_t size, uint32_t index) {
    return cs2_plane_get(system->planes, arg(arguments, size, index));
}

/* --------------------------------------------------------------- the frame */

/*
 * 71: wait for the reader, and put what they did at the address given.
 *
 * This is the game's frame boundary. Its handler hands the address to the
 * script's own suspend fields and answers "no value, end the frame"; the next
 * frame begins by telling the script whether anything happened. The main menu
 * is a loop around this call: ask, and if nothing happened run one step of the
 * screen it is showing.
 */
static int wait_for_the_reader(cs2_system *system, cs2_kcs *script, uint32_t *answer,
                               const uint8_t *arguments, uint32_t argument_size) {
    uint32_t where = arg(arguments, argument_size, 0);

    /* A script asking is a script waiting: this is where it says so. */
    size_t which = 0;
    while (which < system->waiting_count && system->waiting[which].script != script) which++;
    if (which == system->waiting_count && which < WAITING_LIMIT) {
        system->waiting[system->waiting_count].script = script;
        system->waiting[system->waiting_count].count = 0;
        system->waiting_count++;
    }

    /*
     * An event first, and without ending the frame. The game's frame is
     *
     *     while (1) { if (71(&block)) dispatch(&block); else { body(); } }
     *
     * so answering 1 drains one event and comes straight back here, and
     * answering 0 is the frame boundary itself.
     */
    if (which < system->waiting_count && system->waiting[which].count > 0) {
        uint32_t *block = cs2_kcs_at(script, where, 16);
        if (block != NULL) memcpy(block, system->waiting[which].queue[0], 16);
        system->waiting[which].count--;
        memmove(system->waiting[which].queue, system->waiting[which].queue + 1,
                system->waiting[which].count * sizeof system->waiting[which].queue[0]);
        *answer = 1;
        return CS2_KCS_DONE_VALUE;
    }
    cs2_kcs_suspend(script, where);
    return CS2_KCS_DONE_YIELD_AGAIN;
}

/*
 * Starting a plane (448) starts its layout, and the layout is the game.
 *
 * The boot script gives its "main" plane the layout "flow" and starts it, and
 * flow.fes is the whole shape of Labyrinth of Grisaia: it shows the logo, then
 * the title screen, and when the title answers "new game" it writes -1 into
 * flag 512 - the flag adv.xml calls sysflag/boot - and runs kcs.int/sscript.kcs.
 * So the boot type is not set by the engine at all: the game's own layout sets
 * it, which is why nothing in either script and no string in Grisaia2.bin ever
 * named it.
 */
static int32_t host_flag(void *context, int number) {
    cs2_system *system = context;
    uint32_t *at = system_variable_at(system, (uint32_t) number, 0);
    return at == NULL ? 0 : (int32_t) *at;
}

static void host_set_flag(void *context, int number, int32_t value) {
    cs2_system_set_variable(context, (uint32_t) number, (uint32_t) value);
}

static void host_set_string(void *context, int number, const char *text) {
    cs2_system_set_string(context, (uint32_t) number, text);
}

/*
 * One of those strings as a reader should see it. The message window is given
 * its line as a number - sscript writes the line with 276 and then sends
 * MES_SETSTRING, and meswnd.fes says "str apend $str1000" - so what the window
 * appends has still to go through the game's own substitutions and have the
 * message markup taken off it, which is what everything else drawn from a
 * script goes through too.
 */
static const char *host_string(void *context, int number) {
    return cs2_system_string(context, (uint32_t) number);
}

static char *host_text_of(void *context, int number) {
    cs2_system *system = context;
    const char *raw = cs2_system_string(system, (uint32_t) number);
    if (raw == NULL) return NULL;
    return cs2_text_display(system->format, raw);
}

/* execkcs: the layout starts one of the game's system scripts beside itself. */
static void host_run_script(void *context, const char *name) {
    cs2_system *system = context;
    if (system->flow != NULL) return;
    char path[160];
    snprintf(path, sizeof path, "kcs.int/%s.kcs", name);
    system->flow = cs2_kcs_load(system->files, path);
    if (system->flow == NULL) {
        cs2_log("the layout asked for %s but %s", path, cs2_error());
        return;
    }
    cs2_kcs_set_variables(system->flow, system->variables, VARIABLE_BYTES);
    cs2_kcs_set_gcall(system->flow, cs2_system_gcall, system);
    cs2_kcs_trace(system->flow, 1);

    /*
     * What it boots into. The numbers adv.xml keeps them under are in the
     * persistent block by now, put there by the boot script; the layout has
     * already written the boot type into the flag, so the engine only writes
     * one when the runner was told to force it.
     */
    uint32_t boot_flag = 0, boot_string = 0;
    memcpy(&boot_flag, system->variables + 0x4884u, 4);
    memcpy(&boot_string, system->variables + 0x48D8u, 4);
    if (system->boot_override) {
        cs2_system_set_variable(system, boot_flag, (uint32_t) (int32_t) system->boot_type);
        if (system->boot_scenario[0] != 0) {
            cs2_system_set_string(system, boot_string, system->boot_scenario);
        }
    }
    cs2_log("%s starts, boot type %d", path, host_flag(system, (int) boot_flag));
}

static void host_stop_script(void *context) {
    cs2_system *system = context;
    if (system->flow == NULL) return;
    cs2_kcs_set_variables(system->flow, NULL, 0);
    cs2_kcs_free(system->flow);
    system->flow = NULL;
}

static void forget_the_layout(cs2_system *system, cs2_plane handle) {
    for (size_t i = 0; i < system->layout_count; i++) {
        if (system->layouts[i].plane != handle) continue;
        cs2_layout_free(system->layouts[i].layout);
        for (size_t j = i + 1; j < system->layout_count; j++) system->layouts[j - 1] = system->layouts[j];
        system->layout_count--;
        return;
    }
}

static cs2_layout *the_layout_of(cs2_system *system, cs2_plane handle) {
    for (size_t i = 0; i < system->layout_count; i++) {
        if (system->layouts[i].plane == handle) return system->layouts[i].layout;
    }
    return NULL;
}

static void start_the_layout(cs2_system *system, cs2_plane_state *plane) {
    plane->running = 1;
    plane->result = -1;
    if (plane->layout[0] == 0) {
        cs2_log("the plane %s was started with no layout", plane->name);
        return;
    }
    forget_the_layout(system, plane->handle);
    if (system->layout_count >= LAYOUT_LIMIT) {
        cs2_log("the plane %s wanted %s and there is no room for another layout",
                plane->name, plane->layout);
        return;
    }
    cs2_layout_host host = {
        system, host_flag, host_set_flag, host_set_string, host_text_of,
        host_string, host_run_script, host_stop_script
    };
    cs2_layout *started = cs2_layout_start(system->files, plane->layout, &host);
    if (started == NULL) {
        cs2_log("%s", cs2_error());
        return;
    }
    system->layouts[system->layout_count].plane = plane->handle;
    system->layouts[system->layout_count].layout = started;
    system->layout_count++;
}

/*
 * The reader reaches every screen that is up, newest first: the message window
 * is started after the front end and lies over it, and a screen that has no
 * button where the reader pointed does nothing with it.
 */
void cs2_system_pointer(cs2_system *system, int x, int y) {
    if (system == NULL) return;
    for (size_t i = system->layout_count; i-- > 0; ) cs2_layout_pointer(system->layouts[i].layout, x, y);
}

/*
 * The reader's click, and what it means when no screen wanted it.
 *
 * A button of a running layout takes it first: that is how the title screen
 * and the message window's own system buttons work. A click that lands on no
 * button is the reader asking to go on, and the game's own scripts are told so
 * with the class 0xFFFF0003 and the code 2 - the handler for it, at 0xD4C9D of
 * sscript, is the one that takes skip and auto off and sets the flag the
 * reading wait then consumes, and it is reached only while the reader is in a
 * line (sub-states 4, 5 and 6 of [0x28234]).
 */
#define EVENT_SYSTEM 0xFFFF0003u   /* what the layouts' own `send` posts too */
#define EVENT_ADVANCE 2u           /* go on: the reader has read this much */

void cs2_system_click(cs2_system *system, int x, int y, int button) {
    if (system == NULL) return;
    int taken = 0;
    for (size_t i = system->layout_count; i-- > 0; ) {
        if (cs2_layout_click(system->layouts[i].layout, x, y, button)) taken = 1;
    }
    if (!taken && button != 2) cs2_system_post(system, EVENT_SYSTEM, 0, EVENT_ADVANCE);
}

void cs2_system_press(cs2_system *system, cs2_layout_key key) {
    if (system == NULL) return;
    int taken = 0;
    for (size_t i = system->layout_count; i-- > 0; ) {
        if (cs2_layout_press(system->layouts[i].layout, key)) taken = 1;
    }
    if (!taken && key == CS2_LAYOUT_CONFIRM) {
        cs2_system_post(system, EVENT_SYSTEM, 0, EVENT_ADVANCE);
    }
}

size_t cs2_system_layout_count(const cs2_system *system) {
    return system == NULL ? 0 : system->layout_count;
}

const cs2_layout *cs2_system_layout_at(const cs2_system *system, size_t index) {
    if (system == NULL || index >= system->layout_count) return NULL;
    return system->layouts[index].layout;
}

const cs2_layout *cs2_system_layout(const cs2_system *system) {
    return cs2_system_layout_at(system, 0);
}

void cs2_system_draw(cs2_system *system, uint32_t *canvas, int width, int height) {
    if (system == NULL) return;
    cs2_planes_draw(system->planes, canvas, width, height);
    for (size_t i = 0; i < system->layout_count; i++) {
        cs2_layout_draw(system->layouts[i].layout, canvas, width, height);
    }
}

void cs2_system_frame(cs2_system *system) {
    if (system == NULL) return;
    system->frame++;
    /*
     * The layout first: it is the front end, and the system script it runs is
     * something it starts part way through. A layout that has finished leaves
     * its answer on the plane, which is what 452 reads back.
     */
    for (size_t i = 0; i < system->layout_count; i++) {
        cs2_layout_frame(system->layouts[i].layout);
        cs2_plane_state *plane = cs2_plane_get(system->planes, system->layouts[i].plane);
        int result = cs2_layout_result(system->layouts[i].layout);
        if (plane != NULL && result >= 0) {
            plane->running = 0;
            plane->result = result;
        }
    }
    if (system->flow == NULL) return;
    int running = cs2_kcs_frame(system->flow, 2000000);
    if (running < 0) {
        cs2_log("%s: %s", cs2_kcs_name(system->flow), cs2_error());
    }
    if (running <= 0) host_stop_script(system);
}

/* ------------------------------------------------------------ the voices */

/*
 * A voice is a bank of the mixer, and the number the script keeps is the slot
 * plus one. A sound that has finished on its own leaves its slot behind, so the
 * slots are swept before one is handed out; a voice the script still holds is
 * only ever dropped by the script, through 156.
 */
static void sweep_voices(cs2_system *system) {
    for (int i = 0; i < VOICE_LIMIT; i++) {
        if (!system->voices[i].in_use) continue;
        if (!cs2_audio_playing(system->audio, system->voices[i].kind,
                               system->voices[i].bank)) {
            system->voices[i].in_use = 0;
        }
    }
}

static int voice_of(cs2_system *system, uint32_t handle, int *kind, int *bank) {
    int32_t slot = (int32_t) handle - 1;
    if (system->audio == NULL || slot < 0 || slot >= VOICE_LIMIT) return 0;
    if (!system->voices[slot].in_use) return 0;
    *kind = system->voices[slot].kind;
    *bank = system->voices[slot].bank;
    return 1;
}

/* ----------------------------------------------------------- the functions */

int cs2_system_gcall(void *context, cs2_kcs *script, uint32_t id,
                     const uint8_t *arguments, uint32_t argument_size, uint32_t *answer) {
    cs2_system *system = context;
    switch (id) {

    /*
     * 1: say something. The script's own diagnostics, written in the game's own
     * format notation - "sscript loaded > %s[L20]" - so they go through the
     * same formatting as 14.
     */
    case 1: {
        const char *message = cs2_kcs_text(script, arg(arguments, argument_size, 0));
        if (message != NULL) {
            cs2_log("%s: %s", cs2_kcs_name(script), message);
        }
        return CS2_KCS_DONE;
    }

    /*
     * 14: write a string with something in it, into the script's own memory.
     * Until this was written the game's plane names read "plane %d[L12]".
     */
    case 14: {
        uint32_t destination = arg(arguments, argument_size, 0);
        const char *written = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        if (written == NULL) written = "";
        size_t length = strlen(written) + 1;
        char *at = cs2_kcs_at(script, destination, (uint32_t) length);
        if (at != NULL) memcpy(at, written, length);
        *answer = destination;
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 277: read one of those back into the script's memory. Its handler
     * answers whether the number held anything - not the address written to,
     * which is what this used to say - and empties the destination when it
     * did not.
     */
    case 277: {
        uint32_t number = arg(arguments, argument_size, 0);
        uint32_t destination = arg(arguments, argument_size, 1);
        const char *value = cs2_system_string(system, number);
        *answer = value != NULL;
        if (value == NULL) value = "";
        size_t length = strlen(value) + 1;
        char *at = cs2_kcs_at(script, destination, (uint32_t) length);
        if (at != NULL) memcpy(at, value, length);
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 278: resolve a reference to one of those strings, in place. The script
     * writes "$str200" into a buffer with 14 and hands the buffer here; what
     * comes back is the string that number holds - for 200, the file a new
     * game starts on.
     */
    case 278: {
        uint32_t address = arg(arguments, argument_size, 0);
        const char *reference = cs2_kcs_text(script, address);
        if (reference == NULL || strncmp(reference, "$str", 4) != 0) return CS2_KCS_DONE;
        char *end = NULL;
        unsigned long number = strtoul(reference + 4, &end, 10);
        if (end == reference + 4) return CS2_KCS_DONE;
        const char *value = cs2_system_string(system, (uint32_t) number);
        if (value == NULL) {
            cs2_log("%s: the game has no string %lu to put in %s", cs2_kcs_name(script),
                    number, reference);
            value = "";
        }
        size_t length = strlen(value) + 1;
        char *at = cs2_kcs_at(script, address, (uint32_t) length);
        if (at != NULL) memcpy(at, value, length);
        return CS2_KCS_DONE;
    }

    /* 5: the clock, in milliseconds, which is what the fades are counted in. */
    case 5:
        *answer = (uint32_t) (system->frame * 1000u / 60u);
        return CS2_KCS_DONE_VALUE;

    /* 8: the size of the screen, written through the two addresses given. */
    case 8: {
        uint32_t *width = cs2_kcs_at(script, arg(arguments, argument_size, 0), 4);
        uint32_t *height = cs2_kcs_at(script, arg(arguments, argument_size, 1), 4);
        uint32_t w = (uint32_t) system->width, h = (uint32_t) system->height;
        if (width != NULL) memcpy(width, &w, 4);
        if (height != NULL) memcpy(height, &h, 4);
        return CS2_KCS_DONE;
    }

    /* 9: the game has asked to stop. */
    case 9:
        system->finished = 1;
        cs2_log("%s: the game has asked to stop", cs2_kcs_name(script));
        return CS2_KCS_DONE;

    /* 13: copy a string from one place in the script's memory to another. */
    case 13: {
        uint32_t destination = arg(arguments, argument_size, 0);
        const char *source = cs2_kcs_string(script, arg(arguments, argument_size, 1));
        if (source != NULL) {
            size_t length = strlen(source) + 1;
            char *at = cs2_kcs_at(script, destination, (uint32_t) length);
            if (at != NULL) memcpy(at, source, length);
        }
        *answer = destination;
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 15: add a string to the end of one already in the script's memory.
     *
     * This is how every path the game looks a value up by is built: "default",
     * then "/mes/", then "script", and the theme document is asked for
     * "default/mes/script".
     */
    case 15: {
        uint32_t destination = arg(arguments, argument_size, 0);
        const char *added = cs2_kcs_string(script, arg(arguments, argument_size, 1));
        char *at = cs2_kcs_at(script, destination, 1);
        *answer = destination;
        if (at == NULL || added == NULL) return CS2_KCS_DONE_VALUE;
        size_t already = strlen(at);
        size_t length = strlen(added) + 1;
        char *end = cs2_kcs_at(script, destination + (uint32_t) already, (uint32_t) length);
        if (end != NULL) memcpy(end, added, length);
        return CS2_KCS_DONE_VALUE;
    }

    /* 36: fill a run of the script's memory with one byte. */
    case 36: {
        uint32_t address = arg(arguments, argument_size, 0);
        uint32_t value = arg(arguments, argument_size, 1);
        uint32_t count = arg(arguments, argument_size, 2);
        uint8_t *at = cs2_kcs_at(script, address, count);
        if (at != NULL) memset(at, (int) (value & 0xffu), count);
        return CS2_KCS_DONE;
    }

    /* 66: make a plane of a kind, under another, with a name. */
    case 66: {
        int type = (int) arg(arguments, argument_size, 0);
        cs2_plane parent = arg(arguments, argument_size, 1);
        const char *name = cs2_kcs_text(script, arg(arguments, argument_size, 2));
        *answer = cs2_plane_create(system->planes, type, parent, name);
        return CS2_KCS_DONE_VALUE;
    }

    /* 67: and take it away again, with whatever layout it was running. */
    case 67:
        forget_the_layout(system, arg(arguments, argument_size, 0));
        cs2_plane_destroy(system->planes, arg(arguments, argument_size, 0));
        return CS2_KCS_DONE;

    /* 68: the screen's own plane, which everything else hangs under. */
    case 68:
        *answer = cs2_planes_root(system->planes);
        return CS2_KCS_DONE_VALUE;

    case 71:
        return wait_for_the_reader(system, script, answer, arguments, argument_size);

    /* 79: how big a plane is, as the floats the script works in. */
    case 79: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) {
            plane->width = argf(arguments, argument_size, 1);
            plane->height = argf(arguments, argument_size, 2);
        }
        return CS2_KCS_DONE;
    }

    /* 88: what it is drawn in front of. */
    case 88: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) plane->priority = argf(arguments, argument_size, 1);
        return CS2_KCS_DONE;
    }

    /* 92: show it or hide it. */
    case 92: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) plane->visible = arg(arguments, argument_size, 1) != 0;
        return CS2_KCS_DONE;
    }

    /* 100: paint it a colour, which is how the game fades to and from black. */
    case 100: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) {
            plane->colour = arg(arguments, argument_size, 1);
            plane->filled = arg(arguments, argument_size, 2) != 0;
        }
        return CS2_KCS_DONE;
    }

    /* 104: place, size and priority in one call, for a plane being set up. */
    case 104: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) {
            plane->x = (float) (int32_t) arg(arguments, argument_size, 1);
            plane->y = (float) (int32_t) arg(arguments, argument_size, 2);
            plane->width = (float) (int32_t) arg(arguments, argument_size, 3);
            plane->height = (float) (int32_t) arg(arguments, argument_size, 4);
            plane->priority = (float) (int32_t) arg(arguments, argument_size, 5);
        }
        return CS2_KCS_DONE;
    }

    /* 105, 262: take a plane into use, or put it away. */
    case 105:
    case 262: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) plane->visible = arg(arguments, argument_size, 1) != 0;
        return CS2_KCS_DONE;
    }

    /* 256, 260: size and place as whole numbers. */
    case 256: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) {
            plane->width = (float) (int32_t) arg(arguments, argument_size, 1);
            plane->height = (float) (int32_t) arg(arguments, argument_size, 2);
        }
        return CS2_KCS_DONE;
    }
    case 260: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) {
            plane->x = (float) (int32_t) arg(arguments, argument_size, 1);
            plane->y = (float) (int32_t) arg(arguments, argument_size, 2);
        }
        return CS2_KCS_DONE;
    }

    /*
     * 117: fill a rectangle straight onto a surface, 0 being the screen. The
     * boot path calls it five times over a table it has just zeroed, so nothing
     * is drawn until something fills that table in; the call is written all the
     * same because a debug build fills it.
     */
    case 117: {
        uint32_t surface = arg(arguments, argument_size, 0);
        int width = (int) (int32_t) arg(arguments, argument_size, 3);
        int height = (int) (int32_t) arg(arguments, argument_size, 4);
        if (surface != 0 && width > 0 && height > 0) {
            cs2_log("%s: a rectangle fill on surface %u, which is not the screen",
                    cs2_kcs_name(script), surface);
        }
        return CS2_KCS_DONE;
    }

    /* 209: the interpreter that runs a scenario, and 789 the scenario itself. */
    case 209:
        system->interpreter = 1;
        return CS2_KCS_DONE;
    case 789:
        *answer = (uint32_t) system->scenario;
        return CS2_KCS_DONE_VALUE;

    /*
     * 210, 211: read and write one of the system variables the interpreter
     * keeps. The boot script sets them up and the system script reads them
     * back - the boot type it starts on (a new game, a recollection, a saved
     * game) is one of these.
     */
    case 210: {
        uint32_t *value = system_variable_at(system, arg(arguments, argument_size, 0), 0);
        *answer = value == NULL ? 0 : *value;
        return CS2_KCS_DONE_VALUE;
    }
    case 211: {
        uint32_t *value = system_variable_at(system, arg(arguments, argument_size, 0), 1);
        if (value != NULL) *value = arg(arguments, argument_size, 1);
        return CS2_KCS_DONE;
    }

    /* 247: one of the game's own data files, or nothing when it is not there. */
    case 247: {
        const char *name = cs2_kcs_text(script, arg(arguments, argument_size, 0));
        cs2_bytes content = {0};
        if (name != NULL && cs2_files_read(system->files, name, &content) == 0) {
            cs2_bytes_free(&content);
            cs2_log("%s: %s exists but reading it is not written yet", cs2_kcs_name(script), name);
        }
        *answer = 0;
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 276: keep a string the script has built, under a number, for every
     * script to read back with 277.
     *
     * These are adv.xml's "sysstr" - the file a new game starts on (200), the
     * window theme document (800), the name of the theme inside it (801). The
     * boot script writes them and the system script reads them, so they cannot
     * belong to either: they are the same bank a layout writes with $str.
     */
    case 276: {
        uint32_t number = arg(arguments, argument_size, 0);
        const char *value = cs2_kcs_string(script, arg(arguments, argument_size, 1));
        cs2_system_set_string(system, number, value == NULL ? "" : value);
        return CS2_KCS_DONE;
    }

    /*
     * 318, 319: where a run has got to. The arguments are the value it starts
     * at, the value it ends at, which shape the run has, how long it lasts and
     * how far in it is; 319 rounds that to a whole number.
     */
    case 318: {
        float from = argf(arguments, argument_size, 0);
        float to = argf(arguments, argument_size, 1);
        float span = argf(arguments, argument_size, 3);
        float at = argf(arguments, argument_size, 4);
        float part = span > 0 ? at / span : 1.0f;
        if (part < 0) part = 0;
        if (part > 1) part = 1;
        *answer = from_float(from + (to - from) * part);
        return CS2_KCS_DONE_VALUE;
    }
    case 319: {
        float value = argf(arguments, argument_size, 0);
        *answer = (uint32_t) (int32_t) (value < 0 ? value - 0.5f : value + 0.5f);
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 325: put a scene script on the screen. This is the bridge between the
     * game's front end and its scene player: every way into the game - a new
     * game, a recollection, a saved game, the title screen itself - ends in
     * this call, and everything the scene player draws is on the far side of
     * it. The name is the scenario's, without a folder or an extension.
     */
    case 325: {
        const char *name = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        char path[256];
        *answer = 0;
        if (name == NULL || name[0] == 0) return CS2_KCS_DONE_VALUE;
        if (strchr(name, '/') != NULL) {
            snprintf(path, sizeof path, "%s", name);
        } else {
            size_t length = strlen(name);
            int has_suffix = length > 4 && cs2_ieq(name + length - 4, ".cst");
            snprintf(path, sizeof path, "scene.int/%s%s", name, has_suffix ? "" : ".cst");
        }
        if (cs2_scene_play(system->scene, path) != 0) {
            cs2_log("%s: %s", cs2_kcs_name(script), cs2_error());
            return CS2_KCS_DONE_VALUE;
        }
        system->scene_loaded = 1;
        system->scene_step = 1;
        *answer = 1;
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * The message sends themselves. Each names its object in argument 0 and
     * carries what the original pushes after the message number; 853 also
     * carries a small block built out of arguments 1 to 5, which is not
     * unpacked here because nothing reads it yet.
     */
    case 258: {
        uint32_t values[] = { arg(arguments, argument_size, 1) };
        object_message(system, script, arg(arguments, argument_size, 0), 0x114, values, 1);
        system->image_list = arg(arguments, argument_size, 0);
        system->image_shelf = (int) (int32_t) values[0];
        return CS2_KCS_DONE;
    }
    /*
     * 710, 711, 714 and 726: the picture on a layer, made, thrown away, asked
     * how big it is and put where the layer says. This is what turns `bg 0
     * bg15t 0 0 0 0` into pixels: the command only writes "bg15t" into the
     * layer table, and the redraw that follows asks for the picture by that
     * name (Grisaia2.bin 0x0008E10A) and copies the answer into the layer.
     */
    case 710: {
        *answer = scene_image_load(system,
                                   cs2_kcs_text(script, arg(arguments, argument_size, 0)));
        return CS2_KCS_DONE_VALUE;
    }
    case 711:
        cs2_plane_destroy(system->planes, arg(arguments, argument_size, 0));
        return CS2_KCS_DONE;
    case 714: {
        const cs2_plane_state *plane =
            cs2_plane_get(system->planes, arg(arguments, argument_size, 0));
        write_word(script, arg(arguments, argument_size, 1),
                   plane == NULL ? 0 : (uint32_t) plane->pixel_width);
        write_word(script, arg(arguments, argument_size, 2),
                   plane == NULL ? 0 : (uint32_t) plane->pixel_height);
        return CS2_KCS_DONE;
    }
    case 726: {
        cs2_plane_state *plane =
            cs2_plane_get(system->planes, arg(arguments, argument_size, 0));
        if (plane != NULL) {
            plane->x = (float) (int32_t) arg(arguments, argument_size, 1);
            plane->y = (float) (int32_t) arg(arguments, argument_size, 2);
        }
        return CS2_KCS_DONE;
    }

    /*
     * 936 is the call that PLAYS a sound; 156, 157, 160 and 161 are what the
     * script does to it afterwards. The shape of this was read out of the
     * system script and the game's engine together.
     *
     * sscript keeps its sounds in a table of 268-byte entries in the persistent
     * bank, and an entry's own index says what kind of sound it is
     * (0x0009C3C4): under 2 is music, under 12 an effect, under 22 a voice,
     * which is where this engine's own 0/1/2 come from. Once an entry has a
     * name in it the per-frame sound pass calls 936(kind, 0, name, 1, 1, fade)
     * and keeps the answer in the entry as its voice (0x0009C842). The engine
     * is never told which bank to use - the script keeps nothing but that
     * voice - so the engine picks one and the voice is how the bank is named
     * back.
     *
     * WHETHER IT LOOPS is in none of 936's arguments, and that is what held
     * this back. It is in the entry, at +184, and 157 carries it over right
     * after 936 in the same pass (0x0009F0B0 reads it back: an entry whose +184
     * is not 1 is left alone when the sound ends, and one whose +184 is not 0
     * is freed). So +184 is PLAY ONCE, not "loop":
     *   - the sound command handler writes it 1 for se and pcm and 0 for bgm
     *     (sscript 0x0005224B), which is why music repeats and an effect or a
     *     line of dialogue does not;
     *   - the "loop" option - keyword 53 of the option table [0x43388],
     *     dispatched at 0x000526B5 to its handler at 0x00056103 - writes it 0,
     *     so "se 0 loop se571" is an effect that keeps going.
     * 157 is message 0xB4 to the sound (Grisaia2.bin 0x0050BAF0), 156 is 0xB3
     * (stop), 160 is 0xB7 (is it still sounding) and 161 is 0xB8 (volume).
     *
     * 936 therefore starts the sound the way the command's own default would
     * have it, which is what 157 then confirms or changes, so a script that
     * never sends 157 still gets what its command asked for.
     */
    case 936: {
        int kind = (int) arg(arguments, argument_size, 0);
        const char *name = cs2_kcs_text(script, arg(arguments, argument_size, 2));
        int fade = (int) arg(arguments, argument_size, 5);
        if (kind < CS2_SOUND_BGM || kind > CS2_SOUND_PCM) kind = CS2_SOUND_SE;
        *answer = (uint32_t) -1;
        if (system->audio == NULL || name == NULL || name[0] == 0) {
            return CS2_KCS_DONE_VALUE;
        }
        sweep_voices(system);
        int slot = -1;
        for (int i = 0; i < VOICE_LIMIT && slot < 0; i++) {
            if (!system->voices[i].in_use) slot = i;
        }
        int bank = cs2_audio_free_bank(system->audio, kind);
        if (slot < 0 || bank < 0) {
            cs2_log("%s: no free bank to play %s on", cs2_kcs_name(script), name);
            return CS2_KCS_DONE_VALUE;
        }
        if (cs2_audio_play(system->audio, kind, bank, name,
                           kind == CS2_SOUND_BGM) != 0) {
            cs2_log("%s: %s", cs2_kcs_name(script), cs2_error());
            return CS2_KCS_DONE_VALUE;
        }
        /* The last argument is a fade-in, in the game's own frames. */
        if (fade > 0) cs2_audio_volume(system->audio, kind, bank, 0, 100, fade);
        system->voices[slot].in_use = 1;
        system->voices[slot].kind = kind;
        system->voices[slot].bank = bank;
        cs2_log("%s: playing %s (%s) on bank %d", cs2_kcs_name(script), name,
                kind == CS2_SOUND_BGM ? "music" : kind == CS2_SOUND_PCM ? "voice" : "effect",
                bank);
        *answer = (uint32_t) (slot + 1);
        return CS2_KCS_DONE_VALUE;
    }

    /* 156: stop this voice. The script forgets it straight afterwards. */
    case 156: {
        int kind, bank;
        uint32_t handle = arg(arguments, argument_size, 0);
        if (voice_of(system, handle, &kind, &bank)) {
            cs2_audio_stop(system->audio, kind, bank, 0);
            system->voices[handle - 1].in_use = 0;
        }
        return CS2_KCS_DONE;
    }

    /*
     * 157: play once, or keep going. The argument is the sound entry's +184,
     * which is 1 for "play it once", so looping is that argument being zero.
     */
    case 157: {
        int kind, bank;
        if (voice_of(system, arg(arguments, argument_size, 0), &kind, &bank)) {
            cs2_audio_set_loop(system->audio, kind, bank,
                               arg(arguments, argument_size, 1) == 0);
        }
        return CS2_KCS_DONE;
    }

    /*
     * 160: is this voice still sounding. It is how the script asks whether a
     * line of dialogue has finished being spoken, so it is read every frame.
     */
    case 160: {
        int kind, bank;
        *answer = voice_of(system, arg(arguments, argument_size, 0), &kind, &bank)
            && cs2_audio_playing(system->audio, kind, bank);
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 161: this voice's volume, and it is NOT applied yet, on purpose.
     *
     * The script works the level out per frame (sscript 0x0009F781) as a chain
     * of percentages: the player's setting for the kind, the per-sound levels
     * that 605, 606, 607, 964 and 995 look up BY NAME in a table of the game's
     * own, and a duck while a voice is speaking (it walks the voice entries
     * with 160). Every one of those lookups is a table this engine does not
     * read yet, so they all answer zero and the product is zero: applying it
     * today would silence every sound the moment it started. The level is a
     * percentage - the reading is not in doubt, only the numbers going into it
     * - so this waits for the volume tables rather than for more reading.
     */
    case 161:
        return CS2_KCS_UNWRITTEN_WITH(CS2_KCS_DONE);

    case 520: {
        uint32_t values[] = { 0 };
        *answer = object_message(system, script, arg(arguments, argument_size, 0),
                                 0x174, values, 0);
        return CS2_KCS_DONE_VALUE;
    }
    case 853: {
        uint32_t values[] = {
            arg(arguments, argument_size, 1), arg(arguments, argument_size, 4),
            arg(arguments, argument_size, 5),
        };
        object_message(system, script, arg(arguments, argument_size, 0), 0x1EA, values, 3);
        return CS2_KCS_DONE;
    }
    case 854: {
        object_message(system, script, arg(arguments, argument_size, 0), 0x1EB, NULL, 0);
        return CS2_KCS_DONE;
    }
    case 855: {
        uint32_t values[] = { arg(arguments, argument_size, 1) };
        object_message(system, script, arg(arguments, argument_size, 0), 0x1EC, values, 1);
        return CS2_KCS_DONE;
    }

    /*
     * 41 and 42: the game's string list, and 47, which counts what a reader
     * would call a character. sscript splits an argument with 41 - `pcm` and
     * the layer commands hand it a comma - and takes the pieces back out with
     * 42 one at a time.
     *
     * 41(list, text, separators) builds the list; 42(destination, list, index)
     * copies one string out of it and answers whether that string had anything
     * in it. A list is left in the script's own memory, so 42 reads it back
     * from there rather than from anything this engine remembered.
     */
    case 41: {
        const char *src = cs2_kcs_string(script, arg(arguments, argument_size, 1));
        const char *separators = cs2_kcs_string(script, arg(arguments, argument_size, 2));
        char built[TEXT_BYTES];
        size_t written = list_build(src == NULL ? "" : src,
                                    separators == NULL ? "" : separators,
                                    built, sizeof built);
        uint32_t destination = arg(arguments, argument_size, 0);
        char *out = cs2_kcs_at(script, destination, (uint32_t) written);
        if (out != NULL) memcpy(out, built, written);
        return CS2_KCS_DONE;
    }
    case 42: {
        uint32_t list_address = arg(arguments, argument_size, 1);
        const char *list = cs2_kcs_at(script, list_address, 1);
        size_t room = cs2_kcs_room(script, list_address);
        const char *field = list == NULL ? NULL
            : list_field(list, room, arg(arguments, argument_size, 2));
        write_text(script, arg(arguments, argument_size, 0), field == NULL ? "" : field);
        *answer = (uint32_t) (field != NULL);
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 47: how many characters a string holds, not how many bytes. The original
     * walks CP932, where a lead byte and the byte after it are one character
     * (0x005095C0 over 0x00558260); this engine holds the same text as UTF-8,
     * so the same count is the bytes that are not continuation bytes. It is
     * the reader's count either way, which is what the caller wanted.
     */
    case 47: {
        const char *text = cs2_kcs_string(script, arg(arguments, argument_size, 0));
        uint32_t count = 0;
        for (const unsigned char *at = (const unsigned char *) (text == NULL ? "" : text);
             *at != 0; at++) {
            if ((*at & 0xC0u) != 0x80u) count++;
        }
        *answer = count;
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 598: the first field of a comma-separated string, into the address the
     * second argument names, answering whether there was one (0x00518D60).
     *
     * It is NOT the image resolve this engine had it down as. sscript uses it
     * on a layer's picture name before asking 597 for that background's tone
     * out of config.int/bgtone.tbl, which is a table Labyrinth of Grisaia does
     * not ship - so 594 hands back no table, 597 answers 0 for every name, and
     * the tone block sscript skips is a block the original skips too. Nothing
     * about a background's picture passes through here.
     */
    case 598: {
        const char *text = cs2_kcs_text(script, arg(arguments, argument_size, 0));
        char built[TEXT_BYTES];
        size_t written = list_build(text == NULL ? "" : text, ",", built, sizeof built);
        const char *first = list_field(built, written, 0);
        if (first != NULL) write_text(script, arg(arguments, argument_size, 1), first);
        *answer = (uint32_t) (first != NULL);
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 45: every occurrence of one string swapped for another, into a buffer
     * the caller names. sscript runs a scene command through this before it
     * reads it, turning the game own "$" hex mark into "0x" - and until it
     * was written the buffer it writes into kept the LAST line put there, so
     * every command of a scenario was read as the message before it.
     */
    case 45: {
        const char *src = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        const char *find = cs2_kcs_text(script, arg(arguments, argument_size, 2));
        const char *with = cs2_kcs_text(script, arg(arguments, argument_size, 3));
        char written[TEXT_BYTES];
        replace_all(src == NULL ? "" : src, find == NULL ? "" : find,
                    with == NULL ? "" : with, written, sizeof written);
        write_text(script, arg(arguments, argument_size, 0), written);
        return CS2_KCS_DONE;
    }

    /*
     * 517, 518 and 519: how a scene script command is carried out. sscript
     * registers all 259 command names against its own numbers with 517 - "bg"
     * is 9, "se" 18, "title" 33 - splits a command line with 519 and looks the
     * first token up with 518, then switches on the number. So this small
     * table is the whole of the dispatch, and without it not one scene command
     * ran: no background, no sound, no wait.
     */
    case 517: {
        const char *name = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        if (name != NULL) {
            named_number *entry = named_number_at(system, arg(arguments, argument_size, 0),
                                                  name, 1);
            if (entry != NULL) entry->number = (int32_t) arg(arguments, argument_size, 2);
        }
        return CS2_KCS_DONE;
    }
    case 518: {
        const char *name = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        const named_number *entry = name == NULL ? NULL
            : named_number_at(system, arg(arguments, argument_size, 0), name, 0);
        *answer = (uint32_t) (entry == NULL ? -1 : entry->number);
        return CS2_KCS_DONE_VALUE;
    }
    case 519: {
        const char *src = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        uint32_t wanted = arg(arguments, argument_size, 2);
        char written[TEXT_BYTES];
        size_t starts[64];
        size_t count = tokenise(src == NULL ? "" : src, written, sizeof written,
                                starts, sizeof starts / sizeof *starts);
        int found = wanted < count && wanted < sizeof starts / sizeof *starts;
        write_text(script, arg(arguments, argument_size, 0),
                   found ? written + starts[wanted] : "");
        *answer = (uint32_t) found;
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 326 and 327: how the system script reads the scene script it put on the
     * screen. 326 says what kind the word at (line, word) is and 327 hands
     * back its text. Between them the script walks the scenario a word at a
     * time, and -1 from 326 is what tells it there is no such word.
     */
    case 326:
        *answer = (uint32_t) scene_kind(system,
                                        (int32_t) arg(arguments, argument_size, 1),
                                        (int32_t) arg(arguments, argument_size, 2));
        return CS2_KCS_DONE_VALUE;
    case 327: {
        uint32_t destination = arg(arguments, argument_size, 1);
        const char *text = "";
        size_t at = 0;
        if (scene_word(system, (int32_t) arg(arguments, argument_size, 2),
                       (int32_t) arg(arguments, argument_size, 3), &at) == 0) {
            const char *word = cs2_scene_word(system->scene, at, NULL);
            if (word != NULL) text = word;
        }
        size_t length = strlen(text) + 1;
        char *out = cs2_kcs_at(script, destination, (uint32_t) length);
        if (out != NULL) memcpy(out, text, length);
        *answer = destination;
        return CS2_KCS_DONE_VALUE;
    }

    /*
     * 330 and 331: the scenario's turn, once a frame. In the game these are two
     * halves of the same thing - run what the script has to run, then draw what
     * it left - and the scene player is written the same way round: it runs
     * commands until the script wants the reader, and what it leaves behind is
     * the frame. So the running is here and the drawing is whoever composes the
     * screen.
     */
    case 330:
        if (system->scene_loaded && system->scene_step) {
            system->scene_step = 0;
            cs2_scene_advance(system->scene);
            const char *skipped = cs2_scene_take_skipped(system->scene);
            if (skipped[0] != 0) {
                cs2_log("%s: commands not carried out yet: %s", cs2_kcs_name(script), skipped);
            }
        }
        return CS2_KCS_DONE;
    case 331:
        return CS2_KCS_DONE;

    /* 332: how many lines a scene script holds, which is how far it can be
     * wound on. Lines, not words: it is the same number a scenario's line
     * numbers are counted against. */
    case 332:
        *answer = (uint32_t) cs2_scene_line_count(system->scene);
        return CS2_KCS_DONE_VALUE;

    /*
     * 359: the game's engine copies four blocks of its own state into the
     * script's persistent variables. Here those variables ARE the state - there
     * is no second copy to fetch from - so the blocks are already where the
     * script is about to read them.
     */
    case 359:
        return CS2_KCS_DONE;

    /* 393: a setting on the window itself, which a drawn frame does not have. */
    case 393:
        return CS2_KCS_DONE;

    /* 400: where a piece of music loops, out of the game's own list. */
    case 400: {
        const char *path = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        cs2_bytes content = {0};
        if (path == NULL || cs2_files_read(system->files, path, &content) != 0) {
            cs2_log("%s: the music loop points (%s) are not in this game",
                    cs2_kcs_name(script), path == NULL ? "?" : path);
            return CS2_KCS_DONE;
        }
        cs2_bytes_free(&content);
        return CS2_KCS_DONE;
    }

    /* 447: the layout a plane draws itself from, a .fes in the game's own set. */
    case 447: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        const char *name = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        if (plane != NULL && name != NULL) {
            snprintf(plane->layout, sizeof plane->layout, "%s", name);
        }
        return CS2_KCS_DONE;
    }

    /* 448: start it. This is where the game hands over to its own front end. */
    case 448: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) start_the_layout(system, plane);
        return CS2_KCS_DONE;
    }

    /* 449: the plane it works with - its image list. */
    case 449: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) plane->attached = arg(arguments, argument_size, 1);
        return CS2_KCS_DONE;
    }

    /*
     * 630, 645, 646: the system script talking to a screen it has started.
     *
     * The message window is not the reader's; sscript drives it, and the way
     * it does that is a named section of meswnd.fes. 645 writes an argument
     * into the layout's own locals, 630 runs the section by name, and 646
     * reads a local back - so "show the window" is
     *
     *     645(meswnd, 0, 1); 630(meswnd, "MES_SHOW")
     *
     * and #MES_SHOW reads that 1 as \0. 630's third argument is a second
     * string and its fourth the value that travels with it; the handler only
     * passes them on when that string is not empty, and in all 104 of
     * sscript's calls it is, so what they mean is not written here. If a game
     * ever sends one, the log says so rather than this engine guessing.
     */
    case 630: {
        cs2_layout *layout = the_layout_of(system, arg(arguments, argument_size, 0));
        const char *command = cs2_kcs_text(script, arg(arguments, argument_size, 1));
        const char *extra = cs2_kcs_text(script, arg(arguments, argument_size, 2));
        if (extra != NULL && extra[0] != 0) {
            cs2_log("%s: %s carries the string \"%s\", which is not read yet",
                    cs2_kcs_name(script), command == NULL ? "?" : command, extra);
        }
        if (layout != NULL && command != NULL) cs2_layout_send(layout, command);
        return CS2_KCS_DONE;
    }

    case 645: {
        cs2_layout *layout = the_layout_of(system, arg(arguments, argument_size, 0));
        int number = (int) (int32_t) arg(arguments, argument_size, 1);
        int32_t value = (int32_t) arg(arguments, argument_size, 2);
        cs2_layout_set_local(layout, number, value);
        return CS2_KCS_DONE;
    }

    case 646: {
        cs2_layout *layout = the_layout_of(system, arg(arguments, argument_size, 0));
        int number = (int) (int32_t) arg(arguments, argument_size, 1);
        *answer = (uint32_t) cs2_layout_local(layout, number);
        return CS2_KCS_DONE_VALUE;
    }

    /* 452: what a started plane finished with; -1 for as long as it runs. */
    case 452: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        *answer = (uint32_t) (int32_t) (plane == NULL ? -1 : plane->result);
        return CS2_KCS_DONE_VALUE;
    }

    /* 465: whether this is one of the developer's own builds. It is not. */
    case 465:
        *answer = 0;
        return CS2_KCS_DONE_VALUE;

    /* 590, 591, 592: one of the game's configuration documents. */
    case 590: {
        const char *path = cs2_kcs_text(script, arg(arguments, argument_size, 0));
        cs2_bytes document = {0};
        *answer = 0;
        if (path == NULL || cs2_files_read(system->files, path, &document) != 0) {
            cs2_log("%s: no configuration document %s", cs2_kcs_name(script),
                    path == NULL ? "?" : path);
            return CS2_KCS_DONE_VALUE;
        }
        for (int i = 0; i < DOCUMENT_LIMIT; i++) {
            if (system->documents[i] != NULL) continue;
            system->documents[i] = cs2_startup_parse(document.data, document.size);
            if (system->documents[i] != NULL) *answer = (uint32_t) (i + 1);
            break;
        }
        cs2_bytes_free(&document);
        if (*answer == 0) cs2_log("%s: no room to keep %s open", cs2_kcs_name(script), path);
        return CS2_KCS_DONE_VALUE;
    }
    case 591: {
        uint32_t handle = arg(arguments, argument_size, 0);
        if (handle >= 1 && handle <= DOCUMENT_LIMIT) {
            cs2_startup_free(system->documents[handle - 1]);
            system->documents[handle - 1] = NULL;
        }
        return CS2_KCS_DONE;
    }
    /*
     * 593: a value out of one of those documents, written into the script's
     * own memory as a string.
     *
     * This is how the game finds the layout for its message window. main.kcs
     * reads etc/defwnd out of adv.xml - "meswnd01.xml" - and sscript opens
     * that document and asks it for "<theme>/mes/script", which is
     * "meswnd.fes". Without it the theme table stayed empty and the meswnd
     * plane was started with no layout at all.
     */
    case 593: {
        uint32_t handle = arg(arguments, argument_size, 0);
        uint32_t destination = arg(arguments, argument_size, 1);
        const char *path = cs2_kcs_text(script, arg(arguments, argument_size, 2));
        const cs2_startup *document = handle >= 1 && handle <= DOCUMENT_LIMIT
            ? system->documents[handle - 1] : NULL;
        const char *value = path == NULL ? NULL : cs2_startup_value(document, path);
        if (value == NULL) value = "";
        size_t length = strlen(value) + 1;
        char *at = cs2_kcs_at(script, destination, (uint32_t) length);
        if (at != NULL) memcpy(at, value, length);
        return CS2_KCS_DONE;
    }

    case 592: {
        uint32_t handle = arg(arguments, argument_size, 0);
        int fallback = (int) (int32_t) arg(arguments, argument_size, 1);
        const char *path = cs2_kcs_text(script, arg(arguments, argument_size, 2));
        const cs2_startup *document = handle >= 1 && handle <= DOCUMENT_LIMIT
            ? system->documents[handle - 1] : NULL;
        *answer = (uint32_t) (int32_t) (path == NULL ? fallback
                                        : cs2_startup_number(document, path, fallback));
        return CS2_KCS_DONE_VALUE;
    }

    /* 848, 849: two settings on a plane the drawn screen does not read yet. */
    case 848:
    case 849:
        return CS2_KCS_DONE;

    /*
     * 437: may the reading go on - has the model layer at least this much left?
     *
     * 434, 436, 437 and 438 are one manager, Grisaia2.bin [0x8A8110], and what
     * it manages is the game's model layer: the loader beside it (81) takes
     * kx2, kx3 and veff, the formats the .kx2 archive holds. 434 gives it an
     * unbounded budget (0x7FFFFFFF), 436 takes it to nothing, 438 spends one,
     * and 437 (0x004A6070) asks whether the budget covers n.
     *
     * Its first line is the one that matters. The manager is built with a gate
     * word (+0x1351C, out of the settings at +0xC9A0), and when that word is
     * zero none of it - no worker, no list - is made and every ask answers 1.
     * This engine draws no models, so the original's own answer for "it is not
     * there" is the answer here, and it is the one the reading state machine
     * waits on: sub-state 7 asks it before it goes on to the next line.
     */
    case 437:
        *answer = 1;
        return CS2_KCS_DONE_VALUE;

    /* 938: whether the game is being resumed rather than started. It is not. */
    case 938:
        *answer = 0;
        return CS2_KCS_DONE_VALUE;

    default:
        for (size_t i = 0; i < sizeof unwritten_return_codes / sizeof *unwritten_return_codes; i++) {
            if (unwritten_return_codes[i].id == id) {
                return CS2_KCS_UNWRITTEN_WITH(unwritten_return_codes[i].code);
            }
        }
        return CS2_KCS_UNWRITTEN;
    }
}
