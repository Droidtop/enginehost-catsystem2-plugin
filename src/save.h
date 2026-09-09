/*
 * The game's own saves.
 *
 * A CatSystem2 game saves and loads through its own screens: `_saveload.fes`
 * walks ten pages of eight panels and asks the engine `saveexist`, `newsave`,
 * `datasave`, `savedelete`, `saveget_str` and `saveapend_str` about them, and a
 * panel binds itself to a slot with `setsave` and reads a flag back out of it
 * with `getflag`. Loading is not a call at all: the screen writes the slot into
 * flag 512 and exits, flow.fes starts the system script again, and the system
 * script boots on that number - which is why the whole of load is one engine
 * call (70) made from sscript's own boot.
 *
 * So this file is only the store the screens ask about: what a slot holds, and
 * how it is written down. A slot number is `page * 8 + n`, pages 0..19 the
 * numbered saves, page 20 the quick saves and page 21 the auto saves, exactly
 * as the game's own layouts count them; the file for a slot is the name the
 * original uses, `save%04d.dat`, in the folder the host hands the engine.
 *
 * WHERE THE FOLDER COMES FROM. Never the game folder: that is the player's, it
 * holds no save of ours, and on the console it is a read-only card. The Android
 * wrapper passes Enginehost's own per-game save folder; the desktop runner
 * takes --saves, defaulting to a `saves` folder beside the binary. A game given
 * no folder can still be played and simply has no saves.
 *
 * WHAT IS IN A SAVE. The reading position (the scenario and how far into it),
 * the game's numbered flags and strings - the bank a script reads with 210 and
 * writes with 211, which is where every story flag in these games lives - and
 * the read-text high-water marks 328 and 329 keep. That is what the game's own
 * screens ask for and what its own load path needs; it is not the original
 * engine's file format, which nothing but the original reads, so the file says
 * plainly what it is in its first bytes and an original save is not mistaken
 * for one of ours.
 */
#ifndef CS2_SAVE_H
#define CS2_SAVE_H

#include <stddef.h>
#include <stdint.h>

/* The two `what` numbers the screens use with saveget_str/saveapend_str. */
#define CS2_SAVE_MESSAGE 0x200u        /* the player's own message */
#define CS2_SAVE_TITLE   0x80000000u   /* the scene title */

typedef struct {
    uint32_t key;
    int32_t value;
} cs2_save_flag;

typedef struct {
    uint32_t key;
    char *text;
} cs2_save_string;

/* How far the reader has read in one scenario: what 328 raises and 329 answers. */
typedef struct {
    char name[64];
    int32_t mark;
} cs2_save_mark;

/* One save, in memory. cs2_save_clear frees what it holds. */
typedef struct {
    char scenario[192];          /* the scene script being read */
    uint32_t cursor;             /* how far into it */
    int64_t when;                /* when it was written, in seconds */
    cs2_save_flag *flags;
    size_t flag_count;
    cs2_save_string *strings;
    size_t string_count;
    cs2_save_mark *marks;
    size_t mark_count;
} cs2_save;

void cs2_save_clear(cs2_save *save);

void cs2_save_set_flag(cs2_save *save, uint32_t key, int32_t value);
int cs2_save_flag_value(const cs2_save *save, uint32_t key, int32_t *value);
void cs2_save_set_string(cs2_save *save, uint32_t key, const char *text);
const char *cs2_save_string_value(const cs2_save *save, uint32_t key);
void cs2_save_set_mark(cs2_save *save, const char *name, int32_t mark);
int32_t cs2_save_mark_value(const cs2_save *save, const char *name);

/* The store: the folder, and the slots in it. */
typedef struct cs2_saves cs2_saves;

/* NULL when the folder cannot be made or written to; cs2_error says why. */
cs2_saves *cs2_saves_open(const char *folder);
void cs2_saves_free(cs2_saves *saves);
const char *cs2_saves_folder(const cs2_saves *saves);

int cs2_saves_exists(const cs2_saves *saves, int slot);

/*
 * The newest save in a range of slots, or -1 where the range holds none. The
 * screens open on the page this answers, and flow.fes finds the quick and auto
 * saves with it.
 */
int cs2_saves_newest(const cs2_saves *saves, int from, int to);

/* 0 on success. cs2_saves_read fills `into`, which the caller clears. */
int cs2_saves_read(const cs2_saves *saves, int slot, cs2_save *into);
int cs2_saves_write(const cs2_saves *saves, int slot, const cs2_save *from);
int cs2_saves_delete(const cs2_saves *saves, int slot);
int cs2_saves_exchange(const cs2_saves *saves, int a, int b);
int cs2_saves_copy(const cs2_saves *saves, int from, int to);

#endif
