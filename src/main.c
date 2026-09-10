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
#include "kcs.h"
#include "plane.h"
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

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *root = argv[1];
    const char *wanted_script = NULL;
    const char *shot = NULL;
    const char *list = NULL;
    const char *images = NULL;
    const char *kcs_script = NULL;
    const char *saves = "saves";
    int kcs_frames = 0;
    int boot_type = -20;
    int boot_given = 0;
    const char *boot_scene = NULL;
    int silent = 0;
    int dump_scene = 0;
    int steps = 1;
    reader_action actions[READER_ACTIONS];
    int action_count = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--kcs") == 0) {
            if (kcs_frames == 0) kcs_frames = 60;
            if (i + 1 < argc && argv[i + 1][0] != '-') kcs_script = argv[++i];
        }
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) kcs_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc) {
            boot_type = atoi(argv[++i]);
            boot_given = 1;
        }
        else if (strcmp(argv[i], "--boot-scene") == 0 && i + 1 < argc) {
            boot_scene = argv[++i];
            boot_given = 1;
        }
        else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) wanted_script = argv[++i];
        else if (strcmp(argv[i], "--dump-scene") == 0) dump_scene = 1;
        else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--saves") == 0 && i + 1 < argc) saves = argv[++i];
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) list = argv[++i];
        else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) images = argv[++i];
        else if (strcmp(argv[i], "--tap") == 0 && i + 1 < argc) {
            reader_action action = {0};
            action.tap = 1;
            if (sscanf(argv[++i], "%d:%d,%d", &action.frame, &action.x, &action.y) != 3) {
                fprintf(stderr, "--tap wants <frame>:<x>,<y>\n");
                return 2;
            }
            if (action_count < READER_ACTIONS) actions[action_count++] = action;
        }
        else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            reader_action action = {0};
            char name[32] = "";
            if (sscanf(argv[++i], "%d:%31s", &action.frame, name) != 2
                || reader_key(name, &action.key) != 0) {
                fprintf(stderr, "--press wants <frame>:<up|down|left|right|confirm|cancel>\n");
                return 2;
            }
            if (action_count < READER_ACTIONS) actions[action_count++] = action;
        }
        else if (strcmp(argv[i], "--silent") == 0) silent = 1;
        else if (strcmp(argv[i], "--quiet") == 0) cs2_log_quiet(1);
        else {
            usage();
            return 2;
        }
    }

    cs2_files *files = cs2_files_open(root);
    if (files == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    const cs2_game_key *key = cs2_files_key(files);
    if (key->found) {
        cs2_log("archive key from %s (passphrase %s), key %08x",
                key->source, key->passphrase, key->key);
    } else {
        cs2_log("no archive key found in the game's own binaries; only plain archives will open");
    }

    if (list != NULL) {
        cs2_kif *archive = cs2_files_archive(files, list);
        if (archive == NULL) {
            fprintf(stderr, "%s\n", cs2_error());
            cs2_files_close(files);
            return 1;
        }
        for (size_t i = 0; i < cs2_kif_count(archive); i++) {
            printf("%s\n", cs2_kif_name(archive, i));
        }
        cs2_files_close(files);
        return 0;
    }

    if (images != NULL) {
        if (shot == NULL) {
            fprintf(stderr, "--image needs --shot to say where to draw\n");
            cs2_files_close(files);
            return 2;
        }
        int result = cs2_draw_images(files, images, shot);
        cs2_files_close(files);
        return result;
    }

    cs2_bytes document;
    cs2_startup *startup = NULL;
    if (cs2_files_read(files, "config.int/startup.xml", &document) == 0
        || cs2_files_read(files, "startup.xml", &document) == 0) {
        startup = cs2_startup_parse(document.data, document.size);
        cs2_bytes_free(&document);
    } else {
        cs2_log("this game has no startup.xml");
    }

    /*
     * The game's own front end: the title screen, its menus, its new game and
     * its saves are all one compiled system script. It is run on its own here
     * because the machine is new and what a run has to say first is which of
     * the engine's thousand functions the boot path asks for, and in what order.
     */
    if (kcs_frames > 0) {
        int width, height;
        if (screen(startup, &width, &height) != 0) {
            cs2_startup_free(startup);
            cs2_files_close(files);
            return 1;
        }
        char path[512];
        const char *entry = cs2_startup_value(startup, "SCRIPT/start");
        if (kcs_script != NULL) entry = kcs_script;
        else if (entry == NULL || ends_with_cst(entry)) entry = "main.kcs";
        if (strchr(entry, '/') != NULL) snprintf(path, sizeof path, "%s", entry);
        else snprintf(path, sizeof path, "kcs.int/%s", entry);

        cs2_kcs *system_script = cs2_kcs_load(files, path);
        if (system_script == NULL) {
            fprintf(stderr, "%s\n", cs2_error());
            cs2_startup_free(startup);
            cs2_files_close(files);
            return 1;
        }
        cs2_system *system = cs2_system_new(files, width, height);
        if (system == NULL) {
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
        if (!silent && shot == NULL) {
            if (SDL_Init(SDL_INIT_AUDIO) != 0) {
                cs2_log("no sound (%s); playing silently", SDL_GetError());
            } else {
                sdl_started = 1;
                audio = open_sound(files, &sound);
                cs2_system_set_audio(system, audio);
            }
        }
        cs2_system_set_save_folder(system, saves);
        /* The game's own layout sets the boot type; --boot overrides it. */
        if (boot_given) cs2_system_set_boot(system, boot_type, boot_scene);
        uint32_t variable_size = 0;
        void *variables = cs2_system_variables(system, &variable_size);
        cs2_kcs_set_variables(system_script, variables, variable_size);
        cs2_kcs_set_gcall(system_script, cs2_system_gcall, system);
        cs2_kcs_trace(system_script, 1);
        cs2_log("running %s", path);
        /*
         * The boot script and whatever it starts are two halves of one game and
         * run a frame each, in that order, sharing one block of the game's own
         * persistent variables.
         */
        int running = 1, frame = 0;
        for (; running == 1 && frame < kcs_frames && !cs2_system_finished(system); frame++) {
            for (int i = 0; i < action_count; i++) {
                if (actions[i].frame != frame) continue;
                if (actions[i].tap) {
                    cs2_log("frame %d: a tap at %d,%d", frame, actions[i].x, actions[i].y);
                    cs2_system_click(system, actions[i].x, actions[i].y, 1);
                } else {
                    cs2_log("frame %d: the pad", frame);
                    cs2_system_press(system, actions[i].key);
                }
                say_the_screen(system, "  the front end is ");
            }
            running = cs2_kcs_frame(system_script, 2000000);
            cs2_system_frame(system);
        }
        if (running < 0) fprintf(stderr, "%s\n", cs2_error());
        cs2_log("%s: %d frames, %llu instructions, stopped at %#x%s", path, frame,
                (unsigned long long) cs2_kcs_instructions(system_script),
                cs2_kcs_pc(system_script), running == 0 ? ", the script ended" : "");

        say_the_screen(system, "the front end is ");
        const cs2_kcs *flow = cs2_system_script(system);
        if (flow != NULL) {
            cs2_log("%s: %llu instructions, stopped at %#x", cs2_kcs_name(flow),
                    (unsigned long long) cs2_kcs_instructions(flow), cs2_kcs_pc(flow));
        }

        int result = running < 0 ? 1 : 0;
        const cs2_planes *planes = cs2_system_planes(system);
        for (size_t i = 0; i < cs2_planes_count(planes); i++) {
            const cs2_plane_state *plane = cs2_planes_at(planes, i);
            cs2_log("  plane %u %-14s kind %-3d at %g,%g size %gx%g pri %g%s%s%s",
                    plane->handle, plane->name, plane->type, (double) plane->x,
                    (double) plane->y, (double) plane->width, (double) plane->height,
                    (double) plane->priority, plane->visible ? " shown" : " hidden",
                    plane->filled ? " painted" : "", plane->layout[0] != 0 ? " laid out" : "");
        }
        const cs2_scene *scenario = cs2_system_scenario(system);
        if (scenario != NULL) {
            cs2_log("the scenario is %s, %zu/%zu", cs2_scene_path(scenario),
                    cs2_scene_cursor(scenario), cs2_scene_word_count(scenario));
        }
        if (shot != NULL) {
            uint32_t *canvas = calloc((size_t) width * height, sizeof *canvas);
            if (canvas == NULL) {
                fprintf(stderr, "out of memory for a %dx%d frame\n", width, height);
                result = 1;
            } else {
                /*
                 * The screen is the scenario with the scripts' own planes over
                 * it: the front end puts a scene script on the screen and hangs
                 * its own furniture above it, so that is the order it is drawn.
                 */
                cs2_render *render = cs2_render_new(files, width, height);
                if (render != NULL && scenario != NULL) {
                    const uint32_t *drawn = cs2_render_frame(render, scenario, NULL);
                    memcpy(canvas, drawn, (size_t) width * height * sizeof *canvas);
                } else {
                    for (size_t i = 0; i < (size_t) width * height; i++) canvas[i] = 0xff000000u;
                }
                cs2_system_draw(system, canvas, width, height);
                if (cs2_png_write(shot, canvas, width, height) != 0) {
                    fprintf(stderr, "%s\n", cs2_error());
                    result = 1;
                }
                cs2_render_free(render);
                free(canvas);
            }
        }
        if (sound != 0) SDL_CloseAudioDevice(sound);
        desktop_audio = NULL;
        cs2_audio_free(audio);
        if (sdl_started) SDL_Quit();
        cs2_kcs_set_variables(system_script, NULL, 0);
        cs2_kcs_free(system_script);
        cs2_system_free(system);
        cs2_startup_free(startup);
        cs2_files_close(files);
        return result;
    }

    cs2_text *text = cs2_text_from(startup);
    cs2_scene *scene = cs2_scene_new(files, text);
    if (text == NULL || scene == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    char script[512];
    const char *note = NULL;
    const char *start = cs2_startup_value(startup, "SCRIPT/start");
    if (wanted_script != NULL) {
        if (strchr(wanted_script, '/') != NULL) {
            snprintf(script, sizeof script, "%s", wanted_script);
        } else {
            snprintf(script, sizeof script, "scene.int/%s", wanted_script);
        }
    } else if (start != NULL && ends_with_cst(start)) {
        snprintf(script, sizeof script, "%s", start);
    } else if (first_scene_script(files, script, sizeof script) == 0) {
        note = start == NULL ? "startup.xml names no entry point"
                             : "the game boots into a KCS system script, which is not run yet";
    } else {
        fprintf(stderr, "this game holds no scene script to play\n");
        return 1;
    }
    if (cs2_scene_play(scene, script) != 0) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }
    cs2_log("playing %s (%zu lines)%s%s", cs2_scene_path(scene), cs2_scene_line_count(scene),
            note == NULL ? "" : "; ", note == NULL ? "" : note);

    /*
     * The scenario as the system script sees it. sscript walks a scene script
     * by (line, word) and switches on each word own type, so reading what it
     * reads is how a scene command that goes nowhere is found.
     */
    if (dump_scene) {
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
        cs2_scene_free(scene);
        cs2_text_free(text);
        cs2_files_close(files);
        return 0;
    }

    /*
     * A drawn-and-saved frame is a still picture, so it opens no sound device;
     * a window does, before the script is stepped, so that music the opening
     * asks for is already sounding when the first frame appears.
     */
    if (shot != NULL) silent = 1;
    Uint32 systems = shot != NULL ? 0 : SDL_INIT_VIDEO;
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

    for (int i = 0; i < (steps < 1 ? 1 : steps); i++) cs2_scene_advance(scene);
    const char *skipped = cs2_scene_take_skipped(scene);
    if (skipped[0] != '\0') cs2_log("commands not carried out yet: %s", skipped);

    int width, height;
    if (screen(startup, &width, &height) != 0) return 1;
    cs2_render *render = cs2_render_new(files, width, height);
    if (render == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }

    char status[512];
    const char *title = cs2_startup_value(startup, "APP/title");
    snprintf(status, sizeof status, "%s  -  %s  %zu/%zu", title == NULL ? "CatSystem2" : title,
             cs2_scene_path(scene), cs2_scene_cursor(scene), cs2_scene_word_count(scene));

    int result = 0;
    if (shot != NULL) {
        const uint32_t *canvas = cs2_render_frame(render, scene, status);
        if (cs2_png_write(shot, canvas, width, height) != 0) {
            fprintf(stderr, "%s\n", cs2_error());
            result = 1;
        } else {
            printf("%s\n", cs2_scene_text(scene));
        }
    } else {
        SDL_Window *window = SDL_CreateWindow(title == NULL ? "CatSystem2" : title,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_RESIZABLE);
        SDL_Renderer *sdl = window == NULL ? NULL : SDL_CreateRenderer(window, -1, 0);
        SDL_Texture *texture = sdl == NULL ? NULL : SDL_CreateTexture(sdl,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (texture == NULL) {
            fprintf(stderr, "no window: %s\n", SDL_GetError());
            result = 1;
        }
        SDL_RenderSetLogicalSize(sdl, width, height);
        for (int running = texture != NULL; running; ) {
            snprintf(status, sizeof status, "%s  -  %s  %zu/%zu",
                     title == NULL ? "CatSystem2" : title, cs2_scene_path(scene),
                     cs2_scene_cursor(scene), cs2_scene_word_count(scene));
            const uint32_t *canvas = cs2_render_frame(render, scene, status);
            SDL_UpdateTexture(texture, NULL, canvas, width * (int) sizeof *canvas);
            SDL_RenderClear(sdl);
            SDL_RenderCopy(sdl, texture, NULL, NULL);
            SDL_RenderPresent(sdl);

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
                const char *more = cs2_scene_take_skipped(scene);
                if (more[0] != '\0') cs2_log("commands not carried out yet: %s", more);
            }
        }
        if (texture != NULL) SDL_DestroyTexture(texture);
        if (sdl != NULL) SDL_DestroyRenderer(sdl);
        if (window != NULL) SDL_DestroyWindow(window);
    }

    if (sound != 0) {
        SDL_PauseAudioDevice(sound, 1);
        SDL_CloseAudioDevice(sound);
    }
    desktop_audio = NULL;
    cs2_render_free(render);
    cs2_scene_free(scene);
    cs2_audio_free(audio);
    cs2_text_free(text);
    cs2_startup_free(startup);
    cs2_files_close(files);
    SDL_Quit();
    return result;
}
