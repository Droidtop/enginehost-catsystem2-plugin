#include "volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHANNEL_LIMIT 64
#define GROUP_LIMIT   16
#define NAME_BYTES    64

typedef struct {
    char head[NAME_BYTES];      /* the leading letters of the names it owns */
    char archive[NAME_BYTES];   /* <int>: where those sounds are kept */
    int group;
    int master;                 /* <mastervol>: the game's own balance */
    int play;                   /* the player's own level for this channel */
} channel;

typedef struct {
    int id;
    char type[NAME_BYTES];      /* "bgm", "se", "voice" */
} group;

struct cs2_volumes {
    channel channels[CHANNEL_LIMIT];
    size_t channel_count;
    group groups[GROUP_LIMIT];
    size_t group_count;
    /* The player's own settings. */
    int bgm, se, voice;
    int mute_all, mute_bgm, mute_se, mute_voice;
};

static void copy(char *out, size_t out_size, const char *value) {
    if (value == NULL) {
        out[0] = 0;
        return;
    }
    snprintf(out, out_size, "%s", value);
}

/* Sound names are matched the way the archives are: without regard for case. */
static int starts_with(const char *name, const char *head) {
    for (size_t i = 0; head[i] != 0; i++) {
        char a = name[i], b = head[i];
        if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

cs2_volumes *cs2_volumes_read(const cs2_startup *startup) {
    cs2_volumes *volumes = calloc(1, sizeof *volumes);
    if (volumes == NULL) return NULL;

    volumes->bgm = cs2_startup_number(startup, "VOICE/vol/bgm", 100);
    volumes->se = cs2_startup_number(startup, "VOICE/vol/se", 100);
    volumes->voice = cs2_startup_number(startup, "VOICE/vol/voice", 100);
    int play = cs2_startup_number(startup, "VOICE/vol/play", 100);

    for (int i = 0; i < GROUP_LIMIT; i++) {
        char path[64];
        snprintf(path, sizeof path, "SOUNDGROUP/group%02d/type", i);
        const char *type = cs2_startup_value(startup, path);
        if (type == NULL) continue;
        group *entry = &volumes->groups[volumes->group_count++];
        copy(entry->type, sizeof entry->type, type);
        snprintf(path, sizeof path, "SOUNDGROUP/group%02d/id", i);
        entry->id = cs2_startup_number(startup, path, i);
    }

    for (int i = 0; i < CHANNEL_LIMIT; i++) {
        char path[64];
        snprintf(path, sizeof path, "VOICE/v%02d/group", i);
        const char *belongs = cs2_startup_value(startup, path);
        if (belongs == NULL) continue;
        channel *entry = &volumes->channels[volumes->channel_count++];
        entry->group = atoi(belongs);
        snprintf(path, sizeof path, "VOICE/v%02d/head", i);
        copy(entry->head, sizeof entry->head, cs2_startup_value(startup, path));
        snprintf(path, sizeof path, "VOICE/v%02d/int", i);
        copy(entry->archive, sizeof entry->archive, cs2_startup_value(startup, path));
        snprintf(path, sizeof path, "VOICE/v%02d/mastervol", i);
        entry->master = cs2_startup_number(startup, path, 100);
        /*
         * The player's per-channel level. The game keeps one of these for every
         * channel in its own config file ("play00".."play47" of section VOICE);
         * with no config file written yet every channel is where a fresh
         * install starts, which the document itself gives as <vol><play>.
         */
        entry->play = play;
    }
    return volumes;
}

void cs2_volumes_free(cs2_volumes *volumes) {
    free(volumes);
}

/*
 * The channel a sound name belongs to: the first whose <head> the name starts
 * with, which is how the original walks its own table. A channel with an empty
 * head owns nothing by name (Grisaia has three, the spare voice channels).
 */
static const channel *channel_of(const cs2_volumes *volumes, const char *sound_name) {
    if (volumes == NULL || sound_name == NULL || sound_name[0] == 0) return NULL;
    for (size_t i = 0; i < volumes->channel_count; i++) {
        const channel *entry = &volumes->channels[i];
        if (entry->head[0] != 0 && starts_with(sound_name, entry->head)) return entry;
    }
    return NULL;
}

int cs2_volumes_group(const cs2_volumes *volumes, const char *sound_name) {
    if (volumes == NULL) return 100;
    if (volumes->mute_all) return 0;
    const channel *entry = channel_of(volumes, sound_name);
    if (entry == NULL) return 100;
    const group *owner = NULL;
    for (size_t i = 0; i < volumes->group_count && owner == NULL; i++) {
        if (volumes->groups[i].id == entry->group) owner = &volumes->groups[i];
    }
    if (owner == NULL) return 100;
    if (strcmp(owner->type, "bgm") == 0) return volumes->mute_bgm ? 0 : volumes->bgm;
    if (strcmp(owner->type, "se") == 0) return volumes->mute_se ? 0 : volumes->se;
    if (strcmp(owner->type, "voice") == 0 || strcmp(owner->type, "sysvoice") == 0) {
        return volumes->mute_voice ? 0 : volumes->voice;
    }
    return 100;
}

int cs2_volumes_play(const cs2_volumes *volumes, const char *sound_name) {
    const channel *entry = channel_of(volumes, sound_name);
    return entry == NULL ? 0 : entry->play;
}

int cs2_volumes_master(const cs2_volumes *volumes, const char *sound_name) {
    const channel *entry = channel_of(volumes, sound_name);
    return entry == NULL ? 0 : entry->master;
}

const char *cs2_volumes_archive(const cs2_volumes *volumes, const char *sound_name) {
    const channel *entry = channel_of(volumes, sound_name);
    return entry == NULL || entry->archive[0] == 0 ? NULL : entry->archive;
}
