/*
 * The desktop runner: opens a CatSystem2 game folder and plays a scene.
 *
 * With --shot it draws without opening a window and saves a PNG, which is how
 * the engine is checked in a container with no screen and in CI. Without it,
 * a window; a click or a key advances the script, as the reader does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "audio.h"
#include "files.h"
#include "frametime.h"
#include "kcs.h"
#include "plane.h"
#include "pictures.h"
#include "png.h"
#include "render.h"
#include "scene.h"
#include "startup.h"
#include "system.h"

/* Draws named images over one another, for working out how parts are named. */
int cs2_draw_images(cs2_files *files, const char *names, const char *path);

static void usage(void) {
    fprintf(stderr,
        "catsystem2 <game folder> [options]\n"
        "  --kcs [<name>]    run the game's own system script - its title screen and\n"
        "                    menus - instead of playing a scene, and say which of the\n"
        "                    engine's functions it asks for\n"
        "  --frames <n>      how many frames of it to run (default 60)\n"
        "  --boot <n>        what the system script boots into: -1 a new game, -2 a\n"
        "                    recollection, -10 scene select, -20 the ordinary boot\n"
        "                    through start.txt, anything else a saved game (default -20)\n"
        "  --boot-scene <s>  the scene script that boot begins on, for the ways in\n"
        "                    that name one\n"
        "  --script <name>   play this script instead of the game's own entry point,\n"
        "                    as \"ama_001.cst\" or \"scene.int/ama_001.cst\"\n"
        "  --steps <n>       advance n times before showing anything (default 1)\n"
        "  --tap <n>:<x>,<y> tap the front end at that place on frame n, as the reader\n"
        "                    does; may be given more than once\n"
        "  --press <n>:<key> press up, down, left, right, confirm or cancel on frame n;\n"
        "                    may be given more than once\n"
        "  --saves <folder>  where the game's own saves are kept (default ./saves).\n"
        "                    Never the game folder: that is the player's\n"
        "  --draw            compose every frame, the way a console that is showing\n"
        "                    the game has to, and say where the time went\n"
        "  --shot <file>     draw one frame into a PNG and exit, opening no window\n"
        "  --dump-scene      print the scenario the way the system script reads it:\n"
        "                    every line, its words and what kind each word is\n"
        "  --list <archive>  print an archive's entry names and exit\n"
        "  --image <names>   draw these images over one another and exit; a character\n"
        "                    is a body and a set of parts, and which suffix is which\n"
        "                    is learnt by looking\n"
        "  --silent          play the scene without opening a sound device\n"
        "  --quiet           say nothing but what was asked for\n");
}

/* What the front end has on the screen: every layout down to the innermost. */
static void say_the_screen(const cs2_system *system, const char *lead) {
    for (size_t i = 0; i < cs2_system_layout_count(system); i++) {
        const cs2_layout *layout = cs2_system_layout_at(system, i);
        const char *step = lead;
        for (; layout != NULL; layout = cs2_layout_child(layout)) {
            cs2_log("%s%s.fes, in #%s", step, cs2_layout_name(layout), cs2_layout_state(layout));
            step = "  under it, ";
        }
        lead = "and over it, ";
    }
}

/*
 * What the reader does, written down in advance.
 *
 * The front end is a screen that answers taps, and a run with no window has to
 * be able to answer it, so --tap and --press say what happens on which frame.
 * That is what makes "New Game starts the first scene" something a build can
 * be held to rather than something a person has to watch.
 */
#define READER_ACTIONS 32

typedef struct {
    int frame;
    int tap;                 /* a tap at x,y; otherwise a key */
    int x, y;
    cs2_layout_key key;
} reader_action;

static int reader_key(const char *name, cs2_layout_key *out) {
    static const struct { const char *name; cs2_layout_key key; } keys[] = {
        { "up", CS2_LAYOUT_UP }, { "down", CS2_LAYOUT_DOWN },
        { "left", CS2_LAYOUT_LEFT }, { "right", CS2_LAYOUT_RIGHT },
        { "confirm", CS2_LAYOUT_CONFIRM }, { "cancel", CS2_LAYOUT_CANCEL },
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (strcmp(name, keys[i].name) == 0) {
            *out = keys[i].key;
            return 0;
        }
    }
    return -1;
}

/*
 * SDL asks for samples on its own thread; the engine mixes into whatever buffer
 * it is handed, so this is the whole of the desktop sound output. The mixer is
 * reached through a file-scope pointer because the device has to be opened
 * before its real output rate is known, and the mixer is built from that rate.
 */
static cs2_audio *desktop_audio;

static void feed_audio(void *user, Uint8 *stream, int bytes) {
    (void) user;
    cs2_audio_mix(desktop_audio, (int16_t *) stream, bytes / (int) (2 * sizeof(int16_t)));
}

/*
 * Opens the desktop's sound device and builds the mixer on the rate it really
 * got. Both ways of running a game want this - the scene player and the game's
 * own front end - and both want it the same way, so it is written once. NULL
 * means the game plays silently, which is not an error: a machine with no sound
 * card still runs the game.
 *
 * The device is left running rather than paused: nothing is playing yet, and
 * the mixer answers silence until something is.
 */
static cs2_audio *open_sound(cs2_files *files, SDL_AudioDeviceID *device) {
    SDL_AudioSpec wanted = {0}, got = {0};
    wanted.freq = 48000;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    wanted.callback = feed_audio;
    *device = SDL_OpenAudioDevice(NULL, 0, &wanted, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (*device == 0) {
        cs2_log("no sound device (%s); playing silently", SDL_GetError());
        return NULL;
    }
    cs2_audio *audio = cs2_audio_new(files, got.freq);
    if (audio == NULL) {
        cs2_log("%s", cs2_error());
        SDL_CloseAudioDevice(*device);
        *device = 0;
        return NULL;
    }
    desktop_audio = audio;
    SDL_PauseAudioDevice(*device, 0);
    return audio;
}

/* The first script of scene.int, in name order, for a game we cannot boot. */
static int first_scene_script(cs2_files *files, char *out, size_t out_size) {
    cs2_kif *archive = cs2_files_archive(files, "scene.int");
    if (archive == NULL) return -1;
    const char *best = NULL;
    for (size_t i = 0; i < cs2_kif_count(archive); i++) {
        const char *name = cs2_kif_name(archive, i);
        size_t length = strlen(name);
        if (length < 5 || !cs2_ieq(name + length - 4, ".cst")) continue;
        if (best == NULL || strcmp(name, best) < 0) best = name;
    }
    if (best == NULL) return -1;
    snprintf(out, out_size, "scene.int/%s", best);
    return 0;
}

/*
 * The screen the game is authored for, out of its own startup.xml.
 * cs2_startup_number reads any number the document holds, including the zeroes
 * and negatives a configuration file is entitled to; a screen is not one of
 * those, so a number that is not a screen counts as absent.
 *
 * There is no default. 1024x576 is Labyrinth of Grisaia's screen and no other
 * game's, and quietly composing another release at Grisaia's size would put
 * every picture in the wrong place while looking like an engine that worked.
 * A game that does not say is a game this runner refuses to draw.
 */
static int screen_size(const cs2_startup *startup, const char *path) {
    int value = cs2_startup_number(startup, path, 0);
    return value > 0 && value <= 8192 ? value : 0;
}

static int screen(const cs2_startup *startup, int *width, int *height) {
    *width = screen_size(startup, "SCREEN/width");
    *height = screen_size(startup, "SCREEN/height");
    if (*width > 0 && *height > 0) return 0;
    fprintf(stderr, "this game's startup.xml does not say what size screen it is"
                    " authored for, and there is no size to fall back on\n");
    return -1;
}

static int ends_with_cst(const char *name) {
    size_t length = strlen(name);
    return length > 4 && cs2_ieq(name + length - 4, ".cst");
}

/* What the command line asks for. */
typedef struct {
    const char *root;
    const char *wanted_script;
    const char *shot;
    const char *list;
    const char *images;
    const char *kcs_script;
    const char *saves;
    int draw_each;
    int kcs_frames;
    int boot_type;
    int boot_given;
    const char *boot_scene;
    int silent;
    int dump_scene;
    int steps;
    reader_action actions[READER_ACTIONS];
    int action_count;
} options;

/* 0, or the exit code of a command line that is not one this runner takes. */
static int parse_args(int argc, char **argv, options *opt) {
    if (argc < 2) {
        usage();
        return 2;
    }
    memset(opt, 0, sizeof *opt);
    opt->root = argv[1];
    opt->saves = "saves";
    opt->boot_type = -20;
    opt->steps = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--kcs") == 0) {
            if (opt->kcs_frames == 0) opt->kcs_frames = 60;
            if (i + 1 < argc && argv[i + 1][0] != '-') opt->kcs_script = argv[++i];
        }
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            opt->kcs_frames = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc) {
            opt->boot_type = atoi(argv[++i]);
            opt->boot_given = 1;
        }
        else if (strcmp(argv[i], "--boot-scene") == 0 && i + 1 < argc) {
            opt->boot_scene = argv[++i];
            opt->boot_given = 1;
        }
        else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) opt->wanted_script = argv[++i];
        else if (strcmp(argv[i], "--dump-scene") == 0) opt->dump_scene = 1;
        else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) opt->steps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--saves") == 0 && i + 1 < argc) opt->saves = argv[++i];
        else if (strcmp(argv[i], "--draw") == 0) opt->draw_each = 1;
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) opt->shot = argv[++i];
        else if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) opt->list = argv[++i];
        else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) opt->images = argv[++i];
        else if (strcmp(argv[i], "--tap") == 0 && i + 1 < argc) {
            reader_action action = {0};
            action.tap = 1;
            if (sscanf(argv[++i], "%d:%d,%d", &action.frame, &action.x, &action.y) != 3) {
                fprintf(stderr, "--tap wants <frame>:<x>,<y>\n");
                return 2;
            }
            if (opt->action_count < READER_ACTIONS) opt->actions[opt->action_count++] = action;
        }
        else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            reader_action action = {0};
            char name[32] = "";
            if (sscanf(argv[++i], "%d:%31s", &action.frame, name) != 2
                || reader_key(name, &action.key) != 0) {
                fprintf(stderr, "--press wants <frame>:<up|down|left|right|confirm|cancel>\n");
                return 2;
            }
            if (opt->action_count < READER_ACTIONS) opt->actions[opt->action_count++] = action;
        }
        else if (strcmp(argv[i], "--silent") == 0) opt->silent = 1;
        else if (strcmp(argv[i], "--quiet") == 0) cs2_log_quiet(1);
        else {
            usage();
            return 2;
        }
    }
    return 0;
}

static void say_the_key(cs2_files *files) {
    const cs2_game_key *key = cs2_files_key(files);
    if (key->found) {
        cs2_log("archive key from %s (passphrase %s), key %08x",
                key->source, key->passphrase, key->key);
    } else {
        cs2_log("no archive key found in the game's own binaries; only plain archives will open");
    }
}

/* --list: an archive's entry names. */
static int list_archive(cs2_files *files, const char *name) {
    cs2_kif *archive = cs2_files_archive(files, name);
    if (archive == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    for (size_t i = 0; i < cs2_kif_count(archive); i++) {
        printf("%s\n", cs2_kif_name(archive, i));
    }
    return 0;
}

/* The game's startup.xml, or NULL for a game that has none. */
static cs2_startup *read_startup(cs2_files *files) {
    cs2_bytes document;
    cs2_startup *startup = NULL;
    if (cs2_files_read(files, "config.int/startup.xml", &document) == 0
        || cs2_files_read(files, "startup.xml", &document) == 0) {
        startup = cs2_startup_parse(document.data, document.size);
        cs2_bytes_free(&document);
    } else {
        cs2_log("this game has no startup.xml");
    }
    return startup;
}

/* ------------------------------------------------------- the game's front end */

/* One run of the game's own front end, and the screen it is drawn at. */
typedef struct {
    cs2_files *files;
    cs2_pictures *pictures;
    cs2_system *system;
    cs2_kcs *script;
    int width, height;
} front_end;

/* The system script to run: --kcs names one, startup.xml another, or main.kcs. */
static void system_script_path(const cs2_startup *startup, const char *wanted,
                               char *path, size_t path_size) {
    const char *entry = cs2_startup_value(startup, "SCRIPT/start");
    if (wanted != NULL) entry = wanted;
    else if (entry == NULL || ends_with_cst(entry)) entry = "main.kcs";
    if (strchr(entry, '/') != NULL) snprintf(path, path_size, "%s", entry);
    else snprintf(path, path_size, "kcs.int/%s", entry);
}

/* What --tap and --press said the reader does on this frame. */
static void take_reader_actions(cs2_system *system, const options *opt, int frame) {
    for (int i = 0; i < opt->action_count; i++) {
        const reader_action *action = &opt->actions[i];
        if (action->frame != frame) continue;
        if (action->tap) {
            cs2_log("frame %d: a tap at %d,%d", frame, action->x, action->y);
            cs2_system_click(system, action->x, action->y, 1);
        } else {
            cs2_log("frame %d: the pad", frame);
            cs2_system_press(system, action->key);
        }
        say_the_screen(system, "  the front end is ");
    }
}

/* One frame composed as the console composes it, for --draw. */
static void compose_frame(front_end *fe, cs2_render *render, uint32_t *canvas) {
    uint64_t began = cs2_now();
    size_t pixels = (size_t) fe->width * fe->height;
    const cs2_scene *showing = cs2_system_scenario(fe->system);
    if (showing != NULL) {
        const uint32_t *drawn = cs2_render_frame(render, showing, NULL);
        memcpy(canvas, drawn, pixels * sizeof *canvas);
    } else {
        for (size_t i = 0; i < pixels; i++) {
            canvas[i] = 0xff000000u;
        }
    }
    cs2_system_draw(fe->system, canvas, fe->width, fe->height);
    cs2_span_add(CS2_SPAN_COMPOSE, began);
    cs2_frame_done();
}

/*
 * The boot script and whatever it starts are two halves of one game and
 * run a frame each, in that order, sharing one block of the game's own
 * persistent variables.
 *
 * Returns what the script's last frame answered: 1 still running, 0 ended,
 * negative stopped on an error. *frames is how many were run.
 */
static int run_frames(front_end *fe, const options *opt,
                      cs2_render *each_frame, uint32_t *each_canvas, int *frames) {
    int running = 1, frame = 0;
    for (; running == 1 && frame < opt->kcs_frames && !cs2_system_finished(fe->system); frame++) {
        take_reader_actions(fe->system, opt, frame);
        running = cs2_kcs_frame(fe->script, 2000000);
        cs2_system_frame(fe->system);
        if (opt->draw_each) compose_frame(fe, each_frame, each_canvas);
    }
    *frames = frame;
    return running;
}

/* Where the scripts stopped, and what they left on the screen. */
static void say_where_it_stopped(const front_end *fe, const char *path, int running, int frames) {
    if (running < 0) fprintf(stderr, "%s\n", cs2_error());
    cs2_log("%s: %d frames, %llu instructions, stopped at %#x%s", path, frames,
            (unsigned long long) cs2_kcs_instructions(fe->script),
            cs2_kcs_pc(fe->script), running == 0 ? ", the script ended" : "");

    say_the_screen(fe->system, "the front end is ");
    const cs2_kcs *flow = cs2_system_script(fe->system);
    if (flow != NULL) {
        cs2_log("%s: %llu instructions, stopped at %#x", cs2_kcs_name(flow),
                (unsigned long long) cs2_kcs_instructions(flow), cs2_kcs_pc(flow));
    }

    const cs2_planes *planes = cs2_system_planes(fe->system);
    for (size_t i = 0; i < cs2_planes_count(planes); i++) {
        const cs2_plane_state *plane = cs2_planes_at(planes, i);
        cs2_log("  plane %u %-14s kind %-3d at %g,%g size %gx%g pri %g%s%s%s",
                plane->handle, plane->name, plane->type, (double) plane->x,
                (double) plane->y, (double) plane->width, (double) plane->height,
                (double) plane->priority, plane->visible ? " shown" : " hidden",
                plane->filled ? " painted" : "", plane->layout[0] != 0 ? " laid out" : "");
    }
    const cs2_scene *scenario = cs2_system_scenario(fe->system);
    if (scenario != NULL) {
        cs2_log("the scenario is %s, %zu/%zu", cs2_scene_path(scenario),
                cs2_scene_cursor(scenario), cs2_scene_word_count(scenario));
    }
}

/* --shot for the front end: the screen as it stands, into a PNG. 0, or 1. */
static int shoot_front_end(front_end *fe, const char *shot) {
    int width = fe->width, height = fe->height;
    uint32_t *canvas = calloc((size_t) width * height, sizeof *canvas);
    if (canvas == NULL) {
        fprintf(stderr, "out of memory for a %dx%d frame\n", width, height);
        return 1;
    }
    int result = 0;
    /*
     * The screen is the scenario with the scripts' own planes over
     * it: the front end puts a scene script on the screen and hangs
     * its own furniture above it, so that is the order it is drawn.
     */
    const cs2_scene *scenario = cs2_system_scenario(fe->system);
    cs2_render *render = cs2_render_new(fe->files, fe->pictures, width, height);
    if (render != NULL && scenario != NULL) {
        const uint32_t *drawn = cs2_render_frame(render, scenario, NULL);
        memcpy(canvas, drawn, (size_t) width * height * sizeof *canvas);
    } else {
        for (size_t i = 0; i < (size_t) width * height; i++) canvas[i] = 0xff000000u;
    }
    cs2_system_draw(fe->system, canvas, width, height);
    if (cs2_png_write(shot, canvas, width, height) != 0) {
        fprintf(stderr, "%s\n", cs2_error());
        result = 1;
    }
    cs2_render_free(render);
    free(canvas);
    return result;
}

/*
 * The game's own front end: the title screen, its menus, its new game and
 * its saves are all one compiled system script. It is run on its own here
 * because the machine is new and what a run has to say first is which of
 * the engine's thousand functions the boot path asks for, and in what order.
 */
static int run_front_end(cs2_files *files, const cs2_startup *startup, const options *opt) {
    front_end fe = { .files = files };
    if (screen(startup, &fe.width, &fe.height) != 0) return 1;
    char path[512];
    system_script_path(startup, opt->kcs_script, path, sizeof path);

    fe.script = cs2_kcs_load(files, path);
    if (fe.script == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    /*
     * One store of decoded pictures for the whole run, shared by the front
     * end's layouts and the scene being composed: a screen is the same few
     * pictures over and over.
     */
    fe.pictures = cs2_pictures_new(files, 0);
    if (fe.pictures == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    fe.system = cs2_system_new(files, fe.pictures, fe.width, fe.height);
    if (fe.system == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    /*
     * The game's own front end plays the game's own sound, so this way of
     * running it opens a device too: hearing the game without Android is
     * the point of the desktop build. A saved frame is a still picture and
     * stays silent, deliberately - the script asks whether a voice is still
     * speaking (160) and waits on the answer, so a run that draws a frame
     * at a named frame number has to be one where nothing is sounding.
     */
    cs2_audio *audio = NULL;
    SDL_AudioDeviceID sound = 0;
    int sdl_started = 0;
    if (!opt->silent && opt->shot == NULL) {
        if (SDL_Init(SDL_INIT_AUDIO) != 0) {
            cs2_log("no sound (%s); playing silently", SDL_GetError());
        } else {
            sdl_started = 1;
            audio = open_sound(files, &sound);
            cs2_system_set_audio(fe.system, audio);
        }
    }
    cs2_system_set_save_folder(fe.system, opt->saves);
    /* The game's own layout sets the boot type; --boot overrides it. */
    if (opt->boot_given) cs2_system_set_boot(fe.system, opt->boot_type, opt->boot_scene);
    uint32_t variable_size = 0;
    void *variables = cs2_system_variables(fe.system, &variable_size);
    cs2_kcs_set_variables(fe.script, variables, variable_size);
    cs2_kcs_set_gcall(fe.script, cs2_system_gcall, fe.system);
    cs2_kcs_trace(fe.script, 1);
    cs2_log("running %s", path);
    /*
     * The console composes a picture every frame; a run that only draws the
     * last one measures everything except the part the reader complained
     * about. --draw does what the console does, so "where does the frame
     * go" can be asked here rather than on the device.
     */
    cs2_render *each_frame = NULL;
    uint32_t *each_canvas = NULL;
    if (opt->draw_each) {
        each_frame = cs2_render_new(files, fe.pictures, fe.width, fe.height);
        each_canvas = calloc((size_t) fe.width * fe.height, sizeof *each_canvas);
        if (each_frame == NULL || each_canvas == NULL) {
            fprintf(stderr, "no room to compose %dx%d frames\n", fe.width, fe.height);
            return 1;
        }
        cs2_frametime(1);
    }
    int frames = 0;
    int running = run_frames(&fe, opt, each_frame, each_canvas, &frames);
    cs2_render_free(each_frame);
    free(each_canvas);
    cs2_frametime(0);
    say_where_it_stopped(&fe, path, running, frames);

    int result = running < 0 ? 1 : 0;
    if (opt->shot != NULL && shoot_front_end(&fe, opt->shot) != 0) result = 1;
    if (sound != 0) SDL_CloseAudioDevice(sound);
    desktop_audio = NULL;
    cs2_audio_free(audio);
    if (sdl_started) SDL_Quit();
    cs2_kcs_set_variables(fe.script, NULL, 0);
    cs2_kcs_free(fe.script);
    cs2_system_free(fe.system);
    cs2_pictures_free(fe.pictures);
    return result;
}

/* ------------------------------------------------------------- one scene */

/*
 * The scene script to play: --script, else the game's own entry point when it
 * is a scene, else the first one there is. *note says why it was not the
 * entry point. -1 when the game holds none.
 */
static int scene_script(cs2_files *files, const cs2_startup *startup, const char *wanted,
                        char *script, size_t script_size, const char **note) {
    const char *start = cs2_startup_value(startup, "SCRIPT/start");
    *note = NULL;
    if (wanted != NULL) {
        if (strchr(wanted, '/') != NULL) {
            snprintf(script, script_size, "%s", wanted);
        } else {
            snprintf(script, script_size, "scene.int/%s", wanted);
        }
    } else if (start != NULL && ends_with_cst(start)) {
        snprintf(script, script_size, "%s", start);
    } else if (first_scene_script(files, script, script_size) == 0) {
        *note = start == NULL ? "startup.xml names no entry point"
                              : "the game boots into a KCS system script, which is not run yet";
    } else {
        return -1;
    }
    return 0;
}

/*
 * The scenario as the system script sees it. sscript walks a scene script
 * by (line, word) and switches on each word own type, so reading what it
 * reads is how a scene command that goes nowhere is found.
 */
static void dump_scenario(const cs2_scene *scene) {
    size_t lines = cs2_scene_line_count(scene);
    for (size_t line = 0; line < lines; line++) {
        size_t first = 0, count = 0;
        if (cs2_scene_line_words(scene, line, &first, &count) != 0) continue;
        for (size_t w = 0; w < count; w++) {
            unsigned type = 0;
            const char *word = cs2_scene_word(scene, first + w, &type);
            printf("%3zu.%-2zu  type %02x  %s\n", line, w, type,
                   word == NULL ? "" : word);
        }
    }
}

static void say_skipped(cs2_scene *scene) {
    const char *skipped = cs2_scene_take_skipped(scene);
    if (skipped[0] != '\0') cs2_log("commands not carried out yet: %s", skipped);
}

/* The line drawn under the scene: the game, the script and where in it. */
static void status_line(char *into, size_t into_size, const char *title, const cs2_scene *scene) {
    snprintf(into, into_size, "%s  -  %s  %zu/%zu", title == NULL ? "CatSystem2" : title,
             cs2_scene_path(scene), cs2_scene_cursor(scene), cs2_scene_word_count(scene));
}

/* --shot for a scene: the frame into a PNG, and its text on stdout. 0, or 1. */
static int shoot(cs2_render *render, const cs2_scene *scene, const char *status,
                 const char *path, int width, int height) {
    const uint32_t *canvas = cs2_render_frame(render, scene, status);
    if (cs2_png_write(path, canvas, width, height) != 0) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    printf("%s\n", cs2_scene_text(scene));
    return 0;
}

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int width, height;
} desktop_window;

/* -1 when there is no window to be had; close_window is wanted either way. */
static int open_window(desktop_window *w, const char *title, int width, int height) {
    w->width = width;
    w->height = height;
    w->window = SDL_CreateWindow(title == NULL ? "CatSystem2" : title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_RESIZABLE);
    w->renderer = w->window == NULL ? NULL : SDL_CreateRenderer(w->window, -1, 0);
    w->texture = w->renderer == NULL ? NULL : SDL_CreateTexture(w->renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (w->texture == NULL) fprintf(stderr, "no window: %s\n", SDL_GetError());
    SDL_RenderSetLogicalSize(w->renderer, width, height);
    return w->texture == NULL ? -1 : 0;
}

static void close_window(desktop_window *w) {
    if (w->texture != NULL) SDL_DestroyTexture(w->texture);
    if (w->renderer != NULL) SDL_DestroyRenderer(w->renderer);
    if (w->window != NULL) SDL_DestroyWindow(w->window);
}

/* Show the scene; a click or a key advances it, Escape or closing the window ends. */
static void run_loop(desktop_window *w, cs2_render *render, cs2_scene *scene, const char *title) {
    char status[512];
    for (int running = 1; running; ) {
        status_line(status, sizeof status, title, scene);
        const uint32_t *canvas = cs2_render_frame(render, scene, status);
        SDL_UpdateTexture(w->texture, NULL, canvas, w->width * (int) sizeof *canvas);
        SDL_RenderClear(w->renderer);
        SDL_RenderCopy(w->renderer, w->texture, NULL, NULL);
        SDL_RenderPresent(w->renderer);

        SDL_Event event;
        int advanced = 0;
        while (!advanced && running && SDL_WaitEvent(&event)) {
            if (event.type == SDL_QUIT) running = 0;
            else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = 0;
                else advanced = 1;
            } else if (event.type == SDL_MOUSEBUTTONDOWN
                       || event.type == SDL_CONTROLLERBUTTONDOWN) {
                advanced = 1;
            }
        }
        if (advanced) {
            cs2_scene_advance(scene);
            say_skipped(scene);
        }
    }
}

static int play_scene(cs2_files *files, const cs2_startup *startup, const options *opt) {
    cs2_text *text = cs2_text_from(startup);
    cs2_scene *scene = cs2_scene_new(files, text);
    if (text == NULL || scene == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    char script[512];
    const char *note = NULL;
    if (scene_script(files, startup, opt->wanted_script, script, sizeof script, &note) != 0) {
        fprintf(stderr, "this game holds no scene script to play\n");
        return 1;
    }
    if (cs2_scene_play(scene, script) != 0) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    cs2_log("playing %s (%zu lines)%s%s", cs2_scene_path(scene), cs2_scene_line_count(scene),
            note == NULL ? "" : "; ", note == NULL ? "" : note);

    if (opt->dump_scene) {
        dump_scenario(scene);
        cs2_scene_free(scene);
        cs2_text_free(text);
        return 0;
    }

    /*
     * A drawn-and-saved frame is a still picture, so it opens no sound device;
     * a window does, before the script is stepped, so that music the opening
     * asks for is already sounding when the first frame appears.
     */
    int silent = opt->silent || opt->shot != NULL;
    Uint32 systems = opt->shot != NULL ? 0 : SDL_INIT_VIDEO;
    if (!silent) systems |= SDL_INIT_AUDIO;
    if (SDL_Init(systems) != 0) {
        fprintf(stderr, "SDL will not start: %s\n", SDL_GetError());
        return 1;
    }

    cs2_audio *audio = NULL;
    SDL_AudioDeviceID sound = 0;
    if (!silent) {
        audio = open_sound(files, &sound);
        if (audio != NULL) cs2_scene_set_audio(scene, audio);
    }

    for (int i = 0; i < (opt->steps < 1 ? 1 : opt->steps); i++) cs2_scene_advance(scene);
    say_skipped(scene);

    int width, height;
    if (screen(startup, &width, &height) != 0) return 1;
    cs2_pictures *pictures = cs2_pictures_new(files, 0);
    cs2_render *render = pictures == NULL ? NULL : cs2_render_new(files, pictures, width, height);
    if (render == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }

    const char *title = cs2_startup_value(startup, "APP/title");
    int result = 0;
    if (opt->shot != NULL) {
        char status[512];
        status_line(status, sizeof status, title, scene);
        result = shoot(render, scene, status, opt->shot, width, height);
    } else {
        desktop_window window;
        if (open_window(&window, title, width, height) != 0) result = 1;
        else run_loop(&window, render, scene, title);
        close_window(&window);
    }

    if (sound != 0) {
        SDL_PauseAudioDevice(sound, 1);
        SDL_CloseAudioDevice(sound);
    }
    desktop_audio = NULL;
    cs2_render_free(render);
    cs2_pictures_free(pictures);
    cs2_scene_free(scene);
    cs2_audio_free(audio);
    cs2_text_free(text);
    SDL_Quit();
    return result;
}

int main(int argc, char **argv) {
    options opt;
    int bad = parse_args(argc, argv, &opt);
    if (bad != 0) return bad;

    cs2_files *files = cs2_files_open(opt.root);
    if (files == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    say_the_key(files);

    int result;
    if (opt.list != NULL) {
        result = list_archive(files, opt.list);
    } else if (opt.images != NULL) {
        if (opt.shot == NULL) {
            fprintf(stderr, "--image needs --shot to say where to draw\n");
            result = 2;
        } else {
            result = cs2_draw_images(files, opt.images, opt.shot);
        }
    } else {
        cs2_startup *startup = read_startup(files);
        result = opt.kcs_frames > 0 ? run_front_end(files, startup, &opt)
                                    : play_scene(files, startup, &opt);
        cs2_startup_free(startup);
    }
    cs2_files_close(files);
    return result;
}
