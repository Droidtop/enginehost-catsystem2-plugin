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
#include "png.h"
#include "render.h"
#include "scene.h"
#include "startup.h"

/* Draws named images over one another, for working out how parts are named. */
int cs2_draw_images(cs2_files *files, const char *names, const char *path);

static void usage(void) {
    fprintf(stderr,
        "catsystem2 <game folder> [options]\n"
        "  --script <name>   play this script instead of the game's own entry point,\n"
        "                    as \"ama_001.cst\" or \"scene.int/ama_001.cst\"\n"
        "  --steps <n>       advance n times before showing anything (default 1)\n"
        "  --shot <file>     draw one frame into a PNG and exit, opening no window\n"
        "  --list <archive>  print an archive's entry names and exit\n"
        "  --image <names>   draw these images over one another and exit; a character\n"
        "                    is a body and a set of parts, and which suffix is which\n"
        "                    is learnt by looking\n"
        "  --silent          play the scene without opening a sound device\n"
        "  --quiet           say nothing but what was asked for\n");
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
    int silent = 0;
    int steps = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) wanted_script = argv[++i];
        else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) list = argv[++i];
        else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) images = argv[++i];
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
        cs2_log("this game has no startup.xml; falling back to a 1024x576 screen");
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
        SDL_AudioSpec wanted = {0}, got = {0};
        wanted.freq = 48000;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = 1024;
        wanted.callback = feed_audio;
        sound = SDL_OpenAudioDevice(NULL, 0, &wanted, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (sound == 0) {
            cs2_log("no sound device (%s); playing silently", SDL_GetError());
        } else {
            /* The device opens paused, so the mixer is in place before it runs. */
            audio = cs2_audio_new(files, got.freq);
            if (audio == NULL) {
                cs2_log("%s", cs2_error());
                SDL_CloseAudioDevice(sound);
                sound = 0;
            } else {
                desktop_audio = audio;
                cs2_scene_set_audio(scene, audio);
                SDL_PauseAudioDevice(sound, 0);
            }
        }
    }

    for (int i = 0; i < (steps < 1 ? 1 : steps); i++) cs2_scene_advance(scene);
    const char *skipped = cs2_scene_take_skipped(scene);
    if (skipped[0] != '\0') cs2_log("commands not carried out yet: %s", skipped);

    int width = cs2_startup_number(startup, "SCREEN/width", 1024);
    int height = cs2_startup_number(startup, "SCREEN/height", 576);
    cs2_render *render = cs2_render_new(files, width, height);
    if (render == NULL) {
        fprintf(stderr, "%s\n", cs2_error());
        return 1;
    }

    char status[512];
    const char *title = cs2_startup_value(startup, "APP/title");
    snprintf(status, sizeof status, "%s  -  %s  %zu/%zu", title == NULL ? "CatSystem2" : title,
             cs2_scene_path(scene), cs2_scene_cursor(scene), cs2_scene_line_count(scene));

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
                     cs2_scene_cursor(scene), cs2_scene_line_count(scene));
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
