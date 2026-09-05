/*
 * The engine behind the game's own system script: the functions a KCS script
 * reaches through GCALL.
 *
 * A KCS script is only arithmetic and jumps until it calls one of these; every
 * picture it draws, every wait, every save is a numbered call out of the script
 * into the engine. The numbers are the game engine's own, taken from the table
 * at Grisaia2.bin:0x0076D948, and they are written here as the running game
 * asks for them, each one named from what its handler does rather than from a
 * name list, because the game ships none.
 */
#ifndef CS2_SYSTEM_H
#define CS2_SYSTEM_H

#include "files.h"
#include "kcs.h"

typedef struct cs2_system cs2_system;

cs2_system *cs2_system_new(cs2_files *files);
void cs2_system_free(cs2_system *system);

/* Hands the script to the engine: cs2_kcs_set_gcall(script, cs2_system_gcall, system). */
int cs2_system_gcall(void *context, cs2_kcs *script, uint32_t id,
                     const uint8_t *arguments, uint32_t argument_size, uint32_t *answer);

/* What the reader has done since the last frame, for the script to be told about. */
void cs2_system_event(cs2_system *system, uint32_t event);

#endif
