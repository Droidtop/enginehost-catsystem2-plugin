/*
 * Running a .fes layout: the game's own front end.
 *
 * A plane is given a layout (447) and started (448), and from that moment the
 * layout is what the game is doing. Labyrinth of Grisaia's boot script starts
 * one plane with the layout "flow", and flow.fes is the whole shape of the
 * game: it shows the logo, then the title screen, and when the title answers
 * "new game" it sets the boot type in flag 512 and runs the system script.
 *
 * So this is not a decoration on top of the KCS interpreter - it is the layer
 * above it. A layout holds objects (planes, images, buttons, sounds) and named
 * sections of script; one section is the current state and runs every frame,
 * `next` moves to another, `call` runs one inline, `wait` suspends for frames
 * or until the motions finish, `execfes` starts a second layout under this one
 * and `_EXITCODE_` reads what it finished with.
 *
 * The variables the script writes with `$512` are the game's own numbered flags
 * - the same bank the KCS scripts read with 210 and write with 211 - so they
 * are not kept here: they go through the host below, which is what lets the
 * title screen tell the system script what kind of boot it is.
 */
#ifndef CS2_LAYOUT_H
#define CS2_LAYOUT_H

#include "fes.h"

typedef struct cs2_layout cs2_layout;

/*
 * What a layout reaches outside itself. The numbered flags and strings are the
 * game's, shared with every script; execkcs and endkcs start and stop the
 * system script, which is how a new game begins.
 */
typedef struct {
    void *context;
    int32_t (*flag)(void *context, int number);
    void (*set_flag)(void *context, int number, int32_t value);
    void (*set_string)(void *context, int number, const char *text);
    void (*run_script)(void *context, const char *name);
    void (*stop_script)(void *context);
} cs2_layout_host;

/* Loads "fes.int/<name>.fes" and puts it on its first section, #START. */
cs2_layout *cs2_layout_start(cs2_files *files, const char *name, const cs2_layout_host *host);
void cs2_layout_free(cs2_layout *layout);

const char *cs2_layout_name(const cs2_layout *layout);

/* One frame of this layout and of whatever it has started under itself. */
void cs2_layout_frame(cs2_layout *layout);

/* What it finished with, or -1 for as long as it runs. */
int cs2_layout_result(const cs2_layout *layout);

/*
 * Draws the layout and its children onto an ARGB canvas, in priority order.
 * An image whose file is not in this copy of the game is left out and said
 * once in the log, so a study copy without image.int still draws its planes.
 */
void cs2_layout_draw(cs2_layout *layout, uint32_t *canvas, int width, int height);

/* What the layout is doing, for the runner's log: its name and its section. */
const char *cs2_layout_state(const cs2_layout *layout);

#endif
