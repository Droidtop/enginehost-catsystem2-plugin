/*
 * The game's own sound levels.
 *
 * A CatSystem2 game does not mix at one volume: the system script works out a
 * level for every sound it is playing, once a frame, as a chain of percentages,
 * and sends the answer to the sound with engine function 161. Three of the
 * numbers in that chain are lookups BY THE SOUND'S OWN NAME - engine functions
 * 605, 606 and 607 - and every one of them is answered out of the game's own
 * startup.xml, which is what this reads:
 *
 *   <SOUNDGROUP>  one entry per group: an id, a type ("bgm", "se", "voice")
 *                 and the range of banks the group owns.
 *   <VOICE>       one entry per channel: <head>, the leading letters of every
 *                 sound name that belongs to it ("se", "yum"); <group>, which
 *                 group it plays in; <int>, the archive its files live in; and
 *                 <mastervol>, the level the game itself balances that channel
 *                 at (Grisaia holds its music at 70 and everything else at 100).
 *   <VOICE><vol>  the player's own settings as a fresh install finds them:
 *                 bgm, se, voice, play and master.
 *
 * The player's own settings live in the game's config file once it has been
 * written; until this engine has a Configuration screen to write one, they are
 * the defaults the game's own document gives, which is exactly what the
 * original reads them as on a first run.
 *
 * Nothing here is keyed to a game: every name, group and level is the game's
 * own file talking. A release with no <VOICE> section has no channels, and then
 * every lookup answers the way the original's does with an unknown name.
 */
#ifndef CS2_VOLUME_H
#define CS2_VOLUME_H

#include "startup.h"

typedef struct cs2_volumes cs2_volumes;

/* Reads the tables out of a parsed startup.xml. Never NULL unless out of memory. */
cs2_volumes *cs2_volumes_read(const cs2_startup *startup);
void cs2_volumes_free(cs2_volumes *volumes);

/*
 * Engine function 605: the player's setting for the KIND of sound this name is
 * - music, effect or voice - found through the channel's group. 100 for a name
 * no channel claims, 0 while that kind is muted, as the original answers.
 */
int cs2_volumes_group(const cs2_volumes *volumes, const char *sound_name);

/*
 * Engine function 606: the player's own level for this one channel, which the
 * game keeps per channel under "play00".."play47". 0 for an unclaimed name.
 */
int cs2_volumes_play(const cs2_volumes *volumes, const char *sound_name);

/*
 * Engine function 607: the level the GAME balances this channel at, its
 * <mastervol>. 0 for an unclaimed name.
 */
int cs2_volumes_master(const cs2_volumes *volumes, const char *sound_name);

/*
 * The archive the channel's own sounds live in (<int>, so "pcm_a" for Yumiko's
 * voice), or NULL when no channel claims the name. Nothing uses this yet; it is
 * read because it is the game's own answer to a question the sound loader
 * currently guesses at from archive names.
 */
const char *cs2_volumes_archive(const cs2_volumes *volumes, const char *sound_name);

#endif
