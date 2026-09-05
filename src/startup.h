/*
 * startup.xml: the window title, the virtual screen the game is drawn on, the
 * text substitutions it defines for itself, and above all SCRIPT/start, the
 * file the engine boots into.
 *
 * The document is CP932 and is walked with a small element scanner rather than
 * an XML parser, which keeps the engine free of one more dependency. Only leaf
 * text is collected, addressed by its element path, so a value reads as
 * "SCRIPT/start".
 */
#ifndef CS2_STARTUP_H
#define CS2_STARTUP_H

#include "cs2.h"

typedef struct cs2_startup cs2_startup;

cs2_startup *cs2_startup_parse(const uint8_t *document, size_t size);
void cs2_startup_free(cs2_startup *startup);

/* Leaf text at an element path, or NULL. Both are UTF-8. */
const char *cs2_startup_value(const cs2_startup *startup, const char *path);

/* The whole set, for walking the substitution rules. */
size_t cs2_startup_count(const cs2_startup *startup);
const char *cs2_startup_path_at(const cs2_startup *startup, size_t index);
const char *cs2_startup_value_at(const cs2_startup *startup, size_t index);

/* A value read as a number, or the fallback when it is missing or not one. */
int cs2_startup_number(const cs2_startup *startup, const char *path, int fallback);

#endif
