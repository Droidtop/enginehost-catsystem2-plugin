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

#include "files.h"
#include "kcs.h"
#include "plane.h"

typedef struct cs2_system cs2_system;

cs2_system *cs2_system_new(cs2_files *files, int width, int height);
void cs2_system_free(cs2_system *system);

/* Hands the script to the engine: cs2_kcs_set_gcall(script, cs2_system_gcall, system). */
int cs2_system_gcall(void *context, cs2_kcs *script, uint32_t id,
                     const uint8_t *arguments, uint32_t argument_size, uint32_t *answer);

/* What the reader has done since the last frame, for the script to be told about. */
void cs2_system_event(cs2_system *system, uint32_t event);

/*
 * One frame of the engine's own side: the clock the scripts read, and the
 * system script a started plane runs. Call it once a frame, after the boot
 * script has had its turn.
 */
void cs2_system_frame(cs2_system *system);

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
 * The system script a started plane is running, or NULL. The runner prints
 * where it has got to beside the boot script, because between the two of them
 * they are the whole of the game's front end.
 */
const cs2_kcs *cs2_system_script(const cs2_system *system);

#endif
