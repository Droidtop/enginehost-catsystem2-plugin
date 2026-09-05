/*
 * The game folder as the engine sees it: loose files layered over the ".int"
 * archives beside them.
 *
 * CatSystem2 addresses content as "archive.int/name", and a folder named after
 * the archive minus its extension shadows it file by file. Every shipping game
 * relies on that: Labyrinth of Grisaia carries a loose config/startup.xml that
 * must win over the one inside config.int.
 *
 * Archives are opened when something first asks them for a file. One of them is
 * a 2.8 GB image bank, so no entry table is read until it is needed, and no
 * archive is ever unpacked.
 */
#ifndef CS2_FILES_H
#define CS2_FILES_H

#include "kif.h"

typedef struct cs2_files cs2_files;

cs2_files *cs2_files_open(const char *game_root);
void cs2_files_close(cs2_files *files);

const char *cs2_files_root(const cs2_files *files);
const cs2_game_key *cs2_files_key(const cs2_files *files);

/* The archive file names in the folder, lower-cased and in name order. */
size_t cs2_files_archive_count(const cs2_files *files);
const char *cs2_files_archive_name(const cs2_files *files, size_t index);

/*
 * Reads "archive.int/name", or a bare name looked for loose and then in every
 * archive. Returns 0, or -1 when nothing has that name.
 */
int cs2_files_read(cs2_files *files, const char *path, cs2_bytes *out);

/* Opens one archive by file name ("scene.int"), keeping it. NULL when absent. */
cs2_kif *cs2_files_archive(cs2_files *files, const char *archive_name);

#endif
