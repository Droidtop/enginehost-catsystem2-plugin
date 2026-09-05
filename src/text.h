/*
 * Turns a script's message line into the text a reader should see.
 *
 * Two things happen. First the game's own substitutions from SCRIPT/replace in
 * startup.xml, which every release defines for itself: Labyrinth of Grisaia
 * maps "\_" to a space. Then the markup the message format itself carries:
 * "[word]" groups, which mark where a line may break and are not meant to be
 * seen, and backslash escapes such as "\fn", which switch fonts and waits this
 * renderer has no equivalent for. "\n" is the one escape with a plain meaning
 * here.
 */
#ifndef CS2_TEXT_H
#define CS2_TEXT_H

#include "startup.h"

typedef struct cs2_text cs2_text;

cs2_text *cs2_text_from(const cs2_startup *startup);
void cs2_text_free(cs2_text *text);

/* The displayable form of one line. The caller frees it. */
char *cs2_text_display(const cs2_text *text, const char *raw);

#endif
