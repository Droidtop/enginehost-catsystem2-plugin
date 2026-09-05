#include "audio.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kif.h"

/*
 * stb_vorbis is the whole decoder. It is included rather than compiled on its
 * own so both builds of the engine pick it up from the sources they already
 * glob, without either of them having to name a vendored file.
 */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#define MAX_CHANNELS 16
#define SOURCE_FRAMES 1024
#define MIX_BLOCK 512

typedef struct {
    int in_use;
    int kind;
    int bank;

    cs2_bytes file;         /* the Ogg stream, held while it plays */
    stb_vorbis *decoder;
    int source_rate;
    int source_channels;
    int loop;

    /* Decoded source frames, and where in them the resampler is reading. */
    short source[SOURCE_FRAMES * 2];
    int source_have;
    int source_at;
    int drained;            /* the decoder has no more to give */

    /* Linear resampling from the source rate to the mixer's. */
    short current[2];
    short next[2];
    double phase;
    double step;
    int primed;

    /* Volume, 0 to 1, and the slide towards it. */
    double volume;
    double target;
    long fade_left;         /* output frames still to slide over */
    long fade_total;
    double fade_from;
    int stop_when_faded;
} channel;

struct cs2_audio {
    cs2_files *files;
    int rate;
    pthread_mutex_t lock;
    channel channels[MAX_CHANNELS];
};

/* ---- finding the file ---- */

/*
 * CatSystem2 keeps each kind of sound in its own archives, so only those are
 * looked in: this game's image bank holds 94240 entries and reading its table
 * only to miss on a voice would cost a second of silence.
 */
static const char *const *archive_prefixes(int kind) {
    static const char *const bgm[] = { "bgm", NULL };
    static const char *const se[] = { "se", "hse", NULL };
    static const char *const pcm[] = { "pcm", NULL };
    switch (kind) {
        case CS2_SOUND_BGM: return bgm;
        case CS2_SOUND_PCM: return pcm;
        default: return se;
    }
}

static int read_sound(cs2_files *files, int kind, const char *name, cs2_bytes *out) {
    const char *const *prefixes = archive_prefixes(kind);
    char wanted[320];
    snprintf(wanted, sizeof wanted, "%s.ogg", name);
    for (size_t i = 0; i < cs2_files_archive_count(files); i++) {
        const char *archive = cs2_files_archive_name(files, i);
        int matches = 0;
        for (const char *const *prefix = prefixes; *prefix != NULL && !matches; prefix++) {
            matches = strncmp(archive, *prefix, strlen(*prefix)) == 0;
        }
        if (!matches) continue;
        /*
         * Through cs2_files_read, so a loose folder beside the archive shadows
         * it file by file, the way it does for every other kind of content.
         */
        char path[512];
        snprintf(path, sizeof path, "%s/%s", archive, wanted);
        if (cs2_files_read(files, path, out) == 0) return 0;
    }
    /*
     * A game may ship the folder without the archive - that is the same layering
     * every other kind of content has, and a patched or partly unpacked game
     * relies on it. "se.int/x.ogg" is looked for loose as "se/x.ogg" first, so
     * asking for it costs nothing when there is no such archive.
     */
    for (const char *const *prefix = prefixes; *prefix != NULL; prefix++) {
        char path[512];
        snprintf(path, sizeof path, "%s.int/%s", *prefix, wanted);
        if (cs2_files_read(files, path, out) == 0) return 0;
    }
    cs2_set_error("no sound named %s", wanted);
    return -1;
}

/* ---- channels ---- */

static void release(channel *slot) {
    if (slot->decoder != NULL) stb_vorbis_close(slot->decoder);
    slot->decoder = NULL;
    cs2_bytes_free(&slot->file);
    memset(slot, 0, sizeof *slot);
}

static channel *find(cs2_audio *audio, int kind, int bank) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        channel *slot = &audio->channels[i];
        if (slot->in_use && slot->kind == kind && slot->bank == bank) return slot;
    }
    return NULL;
}

static channel *claim(cs2_audio *audio, int kind, int bank) {
    channel *slot = find(audio, kind, bank);
    if (slot != NULL) {
        release(slot);
    } else {
        for (int i = 0; i < MAX_CHANNELS && slot == NULL; i++) {
            if (!audio->channels[i].in_use) slot = &audio->channels[i];
        }
    }
    /* Every bank in use means a script doing more than we model; take the first. */
    if (slot == NULL) {
        slot = &audio->channels[0];
        release(slot);
    }
    slot->in_use = 1;
    slot->kind = kind;
    slot->bank = bank;
    return slot;
}

cs2_audio *cs2_audio_new(cs2_files *files, int rate) {
    cs2_audio *audio = calloc(1, sizeof *audio);
    if (audio == NULL) {
        cs2_set_error("out of memory");
        return NULL;
    }
    audio->files = files;
    audio->rate = rate > 0 ? rate : 48000;
    if (pthread_mutex_init(&audio->lock, NULL) != 0) {
        free(audio);
        cs2_set_error("the mixer cannot be locked");
        return NULL;
    }
    return audio;
}

void cs2_audio_free(cs2_audio *audio) {
    if (audio == NULL) return;
    for (int i = 0; i < MAX_CHANNELS; i++) release(&audio->channels[i]);
    pthread_mutex_destroy(&audio->lock);
    free(audio);
}

int cs2_audio_rate(const cs2_audio *audio) {
    return audio == NULL ? 0 : audio->rate;
}

int cs2_audio_play(cs2_audio *audio, int kind, int bank, const char *name, int loop) {
    if (audio == NULL || name == NULL || name[0] == '\0') return -1;

    /* Read and decode outside the lock: this touches the card and takes a while. */
    cs2_bytes file;
    if (read_sound(audio->files, kind, name, &file) != 0) return -1;
    int error = 0;
    stb_vorbis *decoder = stb_vorbis_open_memory(file.data, (int) file.size, &error, NULL);
    if (decoder == NULL) {
        cs2_bytes_free(&file);
        cs2_set_error("%s is not Ogg Vorbis this engine can read (%d)", name, error);
        return -1;
    }
    stb_vorbis_info info = stb_vorbis_get_info(decoder);

    pthread_mutex_lock(&audio->lock);
    channel *slot = claim(audio, kind, bank);
    slot->file = file;
    slot->decoder = decoder;
    slot->source_rate = (int) info.sample_rate;
    slot->source_channels = info.channels < 1 ? 1 : info.channels;
    if (slot->source_channels > 2) slot->source_channels = 2;
    slot->loop = loop;
    slot->step = (double) slot->source_rate / (double) audio->rate;
    slot->volume = 1.0;
    slot->target = 1.0;
    pthread_mutex_unlock(&audio->lock);
    return 0;
}

static long fade_frames_to_samples(const cs2_audio *audio, int frames) {
    if (frames <= 0) return 0;
    return (long) frames * audio->rate / CS2_SOUND_FPS;
}

void cs2_audio_stop(cs2_audio *audio, int kind, int bank, int fade_frames) {
    if (audio == NULL) return;
    pthread_mutex_lock(&audio->lock);
    channel *slot = find(audio, kind, bank);
    if (slot != NULL) {
        long samples = fade_frames_to_samples(audio, fade_frames);
        if (samples <= 0) {
            release(slot);
        } else {
            slot->fade_from = slot->volume;
            slot->target = 0.0;
            slot->fade_total = samples;
            slot->fade_left = samples;
            slot->stop_when_faded = 1;
        }
    }
    pthread_mutex_unlock(&audio->lock);
}

void cs2_audio_stop_kind(cs2_audio *audio, int kind, int fade_frames) {
    if (audio == NULL) return;
    for (int bank = 0; bank < MAX_CHANNELS; bank++) {
        cs2_audio_stop(audio, kind, bank, fade_frames);
    }
}

void cs2_audio_stop_all(cs2_audio *audio) {
    if (audio == NULL) return;
    pthread_mutex_lock(&audio->lock);
    for (int i = 0; i < MAX_CHANNELS; i++) release(&audio->channels[i]);
    pthread_mutex_unlock(&audio->lock);
}

void cs2_audio_volume(cs2_audio *audio, int kind, int bank, int from, int to, int fade_frames) {
    if (audio == NULL) return;
    pthread_mutex_lock(&audio->lock);
    channel *slot = find(audio, kind, bank);
    if (slot != NULL) {
        double wanted = (to < 0 ? 0 : to > 100 ? 100 : to) / 100.0;
        long samples = fade_frames_to_samples(audio, fade_frames);
        if (from >= 0) slot->volume = (from > 100 ? 100 : from) / 100.0;
        slot->fade_from = slot->volume;
        slot->target = wanted;
        slot->fade_total = samples;
        slot->fade_left = samples;
        slot->stop_when_faded = 0;
        if (samples <= 0) slot->volume = wanted;
    }
    pthread_mutex_unlock(&audio->lock);
}

int cs2_audio_playing(cs2_audio *audio, int kind, int bank) {
    if (audio == NULL) return 0;
    pthread_mutex_lock(&audio->lock);
    int playing = find(audio, kind, bank) != NULL;
    pthread_mutex_unlock(&audio->lock);
    return playing;
}

/* ---- mixing ---- */

/* Reads one source frame into out as stereo. Returns 0 when the sound is over. */
static int source_frame(channel *slot, short out[2]) {
    while (slot->source_at >= slot->source_have) {
        if (slot->drained) return 0;
        int frames = stb_vorbis_get_samples_short_interleaved(
            slot->decoder, slot->source_channels, slot->source,
            SOURCE_FRAMES * slot->source_channels);
        if (frames <= 0 && slot->loop) {
            stb_vorbis_seek_start(slot->decoder);
            frames = stb_vorbis_get_samples_short_interleaved(
                slot->decoder, slot->source_channels, slot->source,
                SOURCE_FRAMES * slot->source_channels);
        }
        if (frames <= 0) {
            slot->drained = 1;
            return 0;
        }
        slot->source_have = frames;
        slot->source_at = 0;
    }
    const short *frame = slot->source + (size_t) slot->source_at * slot->source_channels;
    out[0] = frame[0];
    out[1] = slot->source_channels >= 2 ? frame[1] : frame[0];
    slot->source_at++;
    return 1;
}

/*
 * Mixes one channel into a block of accumulated samples. Returns 0 when the
 * channel is finished and its slot should be freed, 1 while it plays on.
 */
static int mix_channel(channel *slot, int32_t *sum, int frames) {
    if (!slot->primed) {
        if (!source_frame(slot, slot->current)) return 0;
        if (!source_frame(slot, slot->next)) {
            slot->next[0] = slot->current[0];
            slot->next[1] = slot->current[1];
        }
        slot->phase = 0.0;
        slot->primed = 1;
    }
    for (int i = 0; i < frames; i++) {
        while (slot->phase >= 1.0) {
            slot->current[0] = slot->next[0];
            slot->current[1] = slot->next[1];
            if (!source_frame(slot, slot->next)) return 0;
            slot->phase -= 1.0;
        }
        if (slot->fade_left > 0 && slot->fade_total > 0) {
            double done = (double) (slot->fade_total - slot->fade_left) / (double) slot->fade_total;
            slot->volume = slot->fade_from + (slot->target - slot->fade_from) * done;
            slot->fade_left--;
            if (slot->fade_left == 0) {
                slot->volume = slot->target;
                if (slot->stop_when_faded) return 0;
            }
        }
        for (int c = 0; c < 2; c++) {
            double value = slot->current[c] + (slot->next[c] - slot->current[c]) * slot->phase;
            sum[i * 2 + c] += (int32_t) (value * slot->volume);
        }
        slot->phase += slot->step;
    }
    return 1;
}

void cs2_audio_mix(cs2_audio *audio, int16_t *out, int frames) {
    if (frames <= 0) return;
    memset(out, 0, (size_t) frames * 2 * sizeof *out);
    if (audio == NULL) return;

    int32_t sum[MIX_BLOCK * 2];
    pthread_mutex_lock(&audio->lock);
    for (int done = 0; done < frames; ) {
        int block = frames - done;
        if (block > MIX_BLOCK) block = MIX_BLOCK;
        memset(sum, 0, (size_t) block * 2 * sizeof *sum);
        for (int i = 0; i < MAX_CHANNELS; i++) {
            channel *slot = &audio->channels[i];
            if (!slot->in_use) continue;
            if (mix_channel(slot, sum, block) == 0) release(slot);
        }
        for (int i = 0; i < block * 2; i++) {
            int32_t value = sum[i];
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            out[(done * 2) + i] = (int16_t) value;
        }
        done += block;
    }
    pthread_mutex_unlock(&audio->lock);
}
