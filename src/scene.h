/*
 * Plays a CatSystem2 scene script.
 *
 * A scene script is a flat list of lines: message text, a speaker's name, a
 * pause for the reader, and commands. cs2_scene_advance runs commands until the
 * script wants the reader, so when it returns the stage and the message hold
 * what the screen should show. This is deliberately a scene player and not the
 * whole engine: it keeps no history, runs no system menu, and a command with no
 * meaning here yet is noted and skipped rather than guessed at.
 *
 * The stage is what is on screen: numbered slots per kind of layer, the way the
 * script addresses them. "bg 0" is the background, "cg 1" the second character
 * standing on it, "eg 5" an effect over everything, and the bare command with no
 * slot clears every layer of that kind.
 */
#ifndef CS2_SCENE_H
#define CS2_SCENE_H

#include "audio.h"
#include "expr.h"
#include "files.h"
#include "text.h"

/* Layer kinds, in the order they are drawn, back to front. */
#define CS2_LAYER_BACKGROUND 0
#define CS2_LAYER_CHARACTER  1
#define CS2_LAYER_FOREGROUND 2
#define CS2_LAYER_FACE       3
#define CS2_LAYER_EFFECT     4

typedef struct {
    int kind;
    int slot;
    int is_colour;
    char image[160];
    /*
     * A character is drawn as a body with face parts over it. The script names
     * them together, "Ttas02l,1,1,d,d": the name, the body, and then the parts,
     * which are separate images in the same archive whose own offsets say where
     * on the body they belong.
     */
    char parts[4][160];
    int part_count;
    int frame;
    uint32_t colour;
    int x;
    int y;
    int width;
    int height;
    int alpha;
} cs2_layer;

typedef struct cs2_scene cs2_scene;

/* The files and the formatter must outlive the scene. */
cs2_scene *cs2_scene_new(cs2_files *files, const cs2_text *text);
void cs2_scene_free(cs2_scene *scene);

/*
 * Hands the player a mixer to sound the script's bgm, se and pcm commands on.
 * Without one those commands are still read and understood, and heard by nobody:
 * the desktop runner drawing a single frame opens no sound device.
 */
void cs2_scene_set_audio(cs2_scene *scene, cs2_audio *audio);

/* Loads a script by archive path, such as "scene.int/ama_001.cst". */
int cs2_scene_play(cs2_scene *scene, const char *script_path);

/* Runs until the script asks for the reader, or until it runs out. */
void cs2_scene_advance(cs2_scene *scene);

/* Replays silently from the start of the current script up to a line. */
void cs2_scene_seek(cs2_scene *scene, size_t cursor);

const char *cs2_scene_path(const cs2_scene *scene);
const char *cs2_scene_speaker(const cs2_scene *scene);
const char *cs2_scene_text(const cs2_scene *scene);
size_t cs2_scene_cursor(const cs2_scene *scene);
size_t cs2_scene_line_count(const cs2_scene *scene);
int cs2_scene_ended(const cs2_scene *scene);

size_t cs2_scene_layer_count(const cs2_scene *scene);
const cs2_layer *cs2_scene_layer_at(const cs2_scene *scene, size_t index);

/* The commands met but not carried out, space separated. Emptied as it is read. */
const char *cs2_scene_take_skipped(cs2_scene *scene);

#endif
