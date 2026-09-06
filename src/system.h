/*
 * The engine behind the game's own system script: the functions a KCS script
 * reaches through GCALL.
 *
 * A KCS script is only arithmetic and jumps until it calls one of these; every
 * plane it makes, every wait, every save is a numbered call out of the script
 * into the engine. The numbers are the game engine's own, taken from the table
 * at Grisaia2.bin:0x0076D948, and they are written here as the running game
 * asks for them, each one named from what its handler does rather than from a
 * name list, because the game ships none.
 *
 * The game's own engine reaches a plane by sending it a numbered message; this
 * one keeps a plane table (plane.h) and writes each function as what it does to
 * that table, which is the same work without the object graph.
 */
#ifndef CS2_SYSTEM_H
#define CS2_SYSTEM_H

#include <stddef.h>

#include "files.h"
#include "kcs.h"
#include "layout.h"
#include "plane.h"
#include "scene.h"

typedef struct cs2_system cs2_system;

cs2_system *cs2_system_new(cs2_files *files, int width, int height);
void cs2_system_free(cs2_system *system);

/* Hands the script to the engine: cs2_kcs_set_gcall(script, cs2_system_gcall, system). */
int cs2_system_gcall(void *context, cs2_kcs *script, uint32_t id,
                     const uint8_t *arguments, uint32_t argument_size, uint32_t *answer);

/* What the reader has done since the last frame, for the script to be told about. */
void cs2_system_event(cs2_system *system, uint32_t event);

/*
 * One frame of the engine's own side: the clock the scripts read, the layout a
 * started plane runs, and the system script that layout has started. Call it
 * once a frame, after the boot script has had its turn.
 */
void cs2_system_frame(cs2_system *system);

/*
 * The scenario: the scene script the system script has put on the screen. The
 * title screen and the menus are the game's own .fes layouts, but a line of
 * dialogue is not: the system script loads a scene script (325) and steps it
 * (330, 331), and everything the scene player does happens on the far side of
 * that. NULL until one is loaded.
 */
const cs2_scene *cs2_system_scenario(const cs2_system *system);

/*
 * Forces what the system script boots into, which the game itself does not need:
 * flow.fes writes the boot type into flag 512 before it runs the script, so a
 * game left to itself boots the way its own layout says. This is the runner's
 * override, for opening a game straight on one of the other ways in: -1 a new
 * game, -2 a recollection, -10 scene select, -20 the boot through start.txt,
 * anything else a saved game to load, with the scene script the branches that
 * need one begin on. adv.xml names the numbers both are kept under.
 */
void cs2_system_set_boot(cs2_system *system, int type, const char *scenario);

/* The sound device the scenario plays through, or NULL for a game with none. */
void cs2_system_set_audio(cs2_system *system, cs2_audio *audio);

/*
 * The layouts the started planes are running: the game's own front end, the
 * logo and the title screen and what they lead to, and over them the message
 * window a scene is read in. They are in the order they were started, which is
 * the order they are drawn in; cs2_system_layout is the first, the front end
 * itself, and is NULL until a plane is started.
 */
size_t cs2_system_layout_count(const cs2_system *system);
const cs2_layout *cs2_system_layout_at(const cs2_system *system, size_t index);
const cs2_layout *cs2_system_layout(const cs2_system *system);

/*
 * The reader, at the front end. A tap or a click goes to the screen the
 * layouts have up, in the game's own screen pixels, and the pad walks that
 * screen's buttons; what a click means is entirely the layout's business.
 */
void cs2_system_pointer(cs2_system *system, int x, int y);
void cs2_system_click(cs2_system *system, int x, int y, int button);
void cs2_system_press(cs2_system *system, cs2_layout_key key);

/* The whole screen the front end has built: the planes and their layouts. */
void cs2_system_draw(cs2_system *system, uint32_t *canvas, int width, int height);

/* The screen as the scripts have built it, for drawing and for logs. */
const cs2_planes *cs2_system_planes(const cs2_system *system);

/*
 * The game's two banks of variables by number, which adv.xml gives names to:
 * the numbered flags a script reads with 210 and writes with 211, and the
 * numbered strings it resolves with 278. Whatever starts the game sets the ones
 * the system script boots on - the boot type, and the file it starts from.
 */
void cs2_system_set_variable(cs2_system *system, uint32_t key, uint32_t value);
void cs2_system_set_string(cs2_system *system, uint32_t number, const char *text);
const char *cs2_system_string(cs2_system *system, uint32_t number);

/* The persistent variables every script in the game shares. */
void *cs2_system_variables(cs2_system *system, uint32_t *size);

/* Set when the game itself has asked to stop. */
int cs2_system_finished(const cs2_system *system);

/*
 * The system script the front end has started with execkcs, or NULL. The runner
 * prints where it has got to beside the boot script and the layout.
 */
const cs2_kcs *cs2_system_script(const cs2_system *system);

#endif
