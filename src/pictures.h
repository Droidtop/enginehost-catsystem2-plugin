/*
 * The pictures a game is drawn out of, decoded once.
 *
 * A CatSystem2 screen is dozens of pieces of a few HG-3 files: the message
 * window's frame, its eight system buttons and the title screen's menu are all
 * frames of one picture. Nothing about a frame changes while the game runs, so
 * decoding one is something to do once - and until this existed the engine did
 * it again for every piece, every frame: the title screen read title.hg3 out of
 * image.int and decoded it sixty times a second, which is most of why the
 * console called the game incredibly slow.
 *
 * So there is one of these for a run, and everything that draws asks it rather
 * than reading and decoding for itself. It keeps what it has decoded up to a
 * budget and lets the least recently asked-for go first, because one image bank
 * is gigabytes and a game is free to use all of it.
 *
 * A picture a game does not have, or a frame a picture does not carry, is
 * remembered as well. The title screen asks for three frames of title.hg3 that
 * are not there on every frame it draws, and an answer of "no" that costs an
 * archive read is no cheaper than an answer of "yes".
 *
 * An answer is owned by this and is valid until the next call that asks for a
 * different frame. Everything that draws uses what it is handed at once, which
 * is what that is for: nothing here is reference-counted.
 */
#ifndef CS2_PICTURES_H
#define CS2_PICTURES_H

#include "files.h"
#include "hg3.h"

typedef struct cs2_pictures cs2_pictures;

/* The files must outlive this. A budget of 0 takes the engine's own default. */
cs2_pictures *cs2_pictures_new(cs2_files *files, size_t budget);
void cs2_pictures_free(cs2_pictures *pictures);

/*
 * A frame of a picture, by the id the .fes layouts name their pieces with, or
 * by its position in the file, which is how a scene script's layers name one.
 * The name carries no suffix: "sys_mwnd", not "sys_mwnd.hg3".
 */
const cs2_hg3_frame *cs2_pictures_id(cs2_pictures *pictures, const char *name, int id);
const cs2_hg3_frame *cs2_pictures_index(cs2_pictures *pictures, const char *name, int index);

/*
 * Whether the game has a picture of that name at all, which is a different
 * question from whether it carries a frame: a layout naming a picture this copy
 * of the game does not ship is worth one line in the log, and a layout naming a
 * frame that picture does not carry is ordinary - the title screen does it.
 */
int cs2_pictures_has(cs2_pictures *pictures, const char *name);

#endif
