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
#include "startup.h"

#define DOCUMENT_LIMIT 8
#define VARIABLE_BYTES 0x10000u

struct cs2_system {
    cs2_files *files;
    cs2_planes *planes;
    int width, height;
    uint32_t event;              /* what the reader has done, nothing yet if zero */
    uint64_t frame;              /* the game's own sixty a second */
    uint8_t *variables;          /* the persistent block every script shares */
    cs2_startup *documents[DOCUMENT_LIMIT];
    int scenario;                /* 789 answers this; 209 makes its interpreter */
    int interpreter;
    cs2_kcs *flow;               /* the system script a started plane runs */
    cs2_plane flow_plane;
    int finished;
};

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
    free(system->variables);
    free(system);
}

void cs2_system_event(cs2_system *system, uint32_t event) {
    system->event = event;
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
static int wait_for_the_reader(cs2_system *system, cs2_kcs *script,
                               const uint8_t *arguments, uint32_t argument_size) {
    uint32_t where = arg(arguments, argument_size, 0);
    cs2_kcs_suspend(script, where);
    if (system->event != 0) {
        uint32_t *at = cs2_kcs_at(script, where, 4);
        if (at != NULL) {
            uint32_t event = system->event;
            memcpy(at, &event, 4);
            cs2_kcs_answer(script, 1);
        }
        system->event = 0;
    }
    return CS2_KCS_DONE_YIELD_AGAIN;
}

/*
 * Starting a plane (448) is what hands the game over to its own system script.
 * The boot script builds the screen and then starts the plane it gave the
 * layout "flow" to; in the game's engine that plane is the host of sscript.kcs,
 * the script that draws the title screen, begins a new game and does the saving
 * and loading. It is run here beside the boot script, one frame each, sharing
 * the same persistent variables, because they are two halves of one game.
 */
static void start_the_system_script(cs2_system *system, cs2_plane_state *plane) {
    plane->running = 1;
    plane->result = -1;
    system->flow_plane = plane->handle;
    if (system->flow != NULL) return;
    system->flow = cs2_kcs_load(system->files, "kcs.int/sscript.kcs");
    if (system->flow == NULL) {
        cs2_log("the plane %s was started but %s", plane->name, cs2_error());
        return;
    }
    cs2_kcs_set_variables(system->flow, system->variables, VARIABLE_BYTES);
    cs2_kcs_set_gcall(system->flow, cs2_system_gcall, system);
    cs2_kcs_trace(system->flow, 1);
    cs2_log("the plane %s runs kcs.int/sscript.kcs", plane->name);
}

void cs2_system_frame(cs2_system *system) {
    if (system == NULL) return;
    system->frame++;
    if (system->flow == NULL) return;
    int running = cs2_kcs_frame(system->flow, 2000000);
    if (running < 0) {
        cs2_log("kcs.int/sscript.kcs: %s", cs2_error());
    }
    if (running <= 0) {
        cs2_plane_state *plane = cs2_plane_get(system->planes, system->flow_plane);
        if (plane != NULL) {
            plane->running = 0;
            plane->result = running < 0 ? 1 : 0;
        }
        cs2_kcs_set_variables(system->flow, NULL, 0);
        cs2_kcs_free(system->flow);
        system->flow = NULL;
    }
}

/* ----------------------------------------------------------- the functions */

int cs2_system_gcall(void *context, cs2_kcs *script, uint32_t id,
                     const uint8_t *arguments, uint32_t argument_size, uint32_t *answer) {
    cs2_system *system = context;
    switch (id) {

    /* 1: say something. The script's own diagnostics, in its own words. */
    case 1: {
        const char *message = cs2_kcs_string(script, arg(arguments, argument_size, 0));
        if (message != NULL) cs2_log("%s: %s", cs2_kcs_name(script), message);
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
        const char *name = cs2_kcs_string(script, arg(arguments, argument_size, 2));
        *answer = cs2_plane_create(system->planes, type, parent, name);
        return CS2_KCS_DONE_VALUE;
    }

    /* 67: and take it away again. */
    case 67:
        cs2_plane_destroy(system->planes, arg(arguments, argument_size, 0));
        return CS2_KCS_DONE;

    /* 68: the screen's own plane, which everything else hangs under. */
    case 68:
        *answer = cs2_planes_root(system->planes);
        return CS2_KCS_DONE_VALUE;

    case 71:
        return wait_for_the_reader(system, script, arguments, argument_size);

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
     * 211: a system variable - its address in the persistent block and the
     * value it starts at. The engine's own copy is that block, so this is the
     * starting value and nothing else.
     */
    case 211: {
        uint32_t address = arg(arguments, argument_size, 0);
        uint32_t value = arg(arguments, argument_size, 1);
        uint32_t *at = cs2_kcs_at(script, address, 4);
        if (at != NULL) memcpy(at, &value, 4);
        return CS2_KCS_DONE;
    }

    /* 247: one of the game's own data files, or nothing when it is not there. */
    case 247: {
        const char *name = cs2_kcs_string(script, arg(arguments, argument_size, 0));
        cs2_bytes content = {0};
        if (name != NULL && cs2_files_read(system->files, name, &content) == 0) {
            cs2_bytes_free(&content);
            cs2_log("%s: %s exists but reading it is not written yet", cs2_kcs_name(script), name);
        }
        *answer = 0;
        return CS2_KCS_DONE_VALUE;
    }

    /* 276: hand a block of the script's memory to the engine, by number. */
    case 276:
        return CS2_KCS_DONE;

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
        const char *path = cs2_kcs_string(script, arg(arguments, argument_size, 1));
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
        const char *name = cs2_kcs_string(script, arg(arguments, argument_size, 1));
        if (plane != NULL && name != NULL) {
            snprintf(plane->layout, sizeof plane->layout, "%s", name);
        }
        return CS2_KCS_DONE;
    }

    /* 448: start it. This is where the game hands over to its system script. */
    case 448: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) start_the_system_script(system, plane);
        return CS2_KCS_DONE;
    }

    /* 449: the plane it works with - its image list. */
    case 449: {
        cs2_plane_state *plane = plane_of(system, arguments, argument_size, 0);
        if (plane != NULL) plane->attached = arg(arguments, argument_size, 1);
        return CS2_KCS_DONE;
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
        const char *path = cs2_kcs_string(script, arg(arguments, argument_size, 0));
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
    case 592: {
        uint32_t handle = arg(arguments, argument_size, 0);
        int fallback = (int) (int32_t) arg(arguments, argument_size, 1);
        const char *path = cs2_kcs_string(script, arg(arguments, argument_size, 2));
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

    /* 938: whether the game is being resumed rather than started. It is not. */
    case 938:
        *answer = 0;
        return CS2_KCS_DONE_VALUE;

    default:
        return CS2_KCS_UNWRITTEN;
    }
}
