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

#include <stdint.h>

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
    /*
     * One of those strings as a reader should see it: the game's own
     * substitutions applied and the message markup taken off. The message
     * window appends its line this way - "str apend $str1000" - so the
     * conversion belongs where the rest of the game's text is done, not in a
     * layout. The caller frees what comes back.
     */
    char *(*text_of)(void *context, int number);
    /*
     * And one of them as it stands: a file name, a path, whatever the scripts
     * have put there. text_of is the same string turned into what a reader
     * should see; this is the string itself.
     */
    const char *(*string)(void *context, int number);
    void (*run_script)(void *context, const char *name);
    void (*stop_script)(void *context);
} cs2_layout_host;

/* Loads "fes.int/<name>.fes" and puts it on its first section, #START. */
cs2_layout *cs2_layout_start(cs2_files *files, const char *name, const cs2_layout_host *host);
void cs2_layout_free(cs2_layout *layout);

const char *cs2_layout_name(const cs2_layout *layout);

/* The screen this one has started under itself with execfes, or NULL. */
const cs2_layout *cs2_layout_child(const cs2_layout *layout);

/* One frame of this layout and of whatever it has started under itself. */
void cs2_layout_frame(cs2_layout *layout);

/* What it finished with, or -1 for as long as it runs. */
int cs2_layout_result(const cs2_layout *layout);

/*
 * The reader.
 *
 * A screen reads a click as the button itself - the title screen is written
 * "if (_CLICK_L_==btn_j[0])" - so what these do is decide which of the
 * layout's own buttons the reader has just named, and the layout's own script
 * does the rest. A button is a rectangle its picture's own size and offset,
 * enabled or not by the screen, and the topmost enabled one under the point
 * wins; the invisible btn_j row of the title screen sits over the visible
 * btn_d row for exactly that reason.
 *
 * The pad walks the KEYBLOCK grids instead: focus moves by column and row
 * inside the grid the enabled buttons are in, confirm clicks what is focused
 * and cancel is the right button. Moving focus runs the button's own
 * #<name>.FOCUS and #<name>.UNFOCUS sections, which is how the game lights a
 * button up, and a click runs its #<name>.PUSH_L.
 *
 * All of them go to the innermost layout - the screen the reader is looking
 * at, which is the one a parent layout started with execfes.
 */
typedef enum {
    CS2_LAYOUT_UP,
    CS2_LAYOUT_DOWN,
    CS2_LAYOUT_LEFT,
    CS2_LAYOUT_RIGHT,
    CS2_LAYOUT_CONFIRM,
    CS2_LAYOUT_CANCEL
} cs2_layout_key;

/* Where the reader is pointing, in the game's own screen pixels. */
void cs2_layout_pointer(cs2_layout *layout, int x, int y);

/* A click there: 1 the left button or a tap, 2 the right button. */
void cs2_layout_click(cs2_layout *layout, int x, int y, int button);

/* The pad. */
void cs2_layout_press(cs2_layout *layout, cs2_layout_key key);

/*
 * The command channel: how the system script drives a screen it has started.
 *
 * The message window is not run by the reader but by sscript, and the way it
 * reaches it is a named section. The script writes the arguments into the
 * layout's own locals (engine function 645), sends the command by name (630),
 * and reads whatever the section left behind back out of a local (646):
 *
 *     645(meswnd, 0, 1)  645(meswnd, 1, 0) ...   \0 = 1, \1 = 0
 *     630(meswnd, "MES_SETSTATE")               #MES_SETSTATE runs
 *     646(meswnd, 0)                            what it answered
 *
 * meswnd.fes has thirty of these - MES_SETSTRING, MES_DRAW, MES_SHOW,
 * MES_WAITINPUT and the rest - and they are the whole of reading a scene. A
 * command runs to its end there and then, not a frame at a time: the script
 * sends the next one on the line after.
 */
void cs2_layout_send(cs2_layout *layout, const char *command);
void cs2_layout_set_local(cs2_layout *layout, int number, int32_t value);
int32_t cs2_layout_local(const cs2_layout *layout, int number);

/*
 * Draws the layout and its children onto an ARGB canvas, in priority order.
 * An image whose file is not in this copy of the game is left out and said
 * once in the log, so a study copy without image.int still draws its planes.
 */
void cs2_layout_draw(cs2_layout *layout, uint32_t *canvas, int width, int height);

/* What the layout is doing, for the runner's log: its name and its section. */
const char *cs2_layout_state(const cs2_layout *layout);

#endif
