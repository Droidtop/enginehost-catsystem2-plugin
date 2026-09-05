/*
 * The game's sound: music, effects and voices.
 *
 * A scene script asks for sound by name - "bgm 0 bgm18", "se 0 se0007",
 * "pcm AMA_ama_001_001" - and the named file is an Ogg Vorbis stream inside one
 * of the game's archives (bgm*.int, se*.int, hse*.int, pcm_?.int). Each kind of
 * sound has numbered banks, the way the script addresses them: a bank holds one
 * sound, and starting another on the same bank replaces it.
 *
 * The engine mixes; it does not open a sound device, because the two frontends
 * open different ones (SDL on the desktop, AAudio on Android). The frontend
 * asks for the next block of samples from its own audio thread, which is why
 * every function here is safe to call while cs2_audio_mix is running.
 */
#ifndef CS2_AUDIO_H
#define CS2_AUDIO_H

#include "files.h"

#define CS2_SOUND_BGM 0
#define CS2_SOUND_SE  1
#define CS2_SOUND_PCM 2

/* Fades are written in the game's own frames, and the game runs at 60 of them. */
#define CS2_SOUND_FPS 60

typedef struct cs2_audio cs2_audio;

/*
 * rate is the frontend's output rate in samples per second; sounds recorded at
 * another rate are resampled to it. Output is always two channels, interleaved
 * 16-bit. The files must outlive the mixer.
 */
cs2_audio *cs2_audio_new(cs2_files *files, int rate);
void cs2_audio_free(cs2_audio *audio);

int cs2_audio_rate(const cs2_audio *audio);

/*
 * Starts <name>.ogg on a bank, replacing what it held. Music loops unless told
 * otherwise; an effect or a voice plays once. Returns 0, or -1 when no archive
 * holds that name, which is worth logging: it means a name the script built.
 */
int cs2_audio_play(cs2_audio *audio, int kind, int bank, const char *name, int loop);

/* Stops a bank, over fade_frames of the game's frames (0 stops at once). */
void cs2_audio_stop(cs2_audio *audio, int kind, int bank, int fade_frames);

/* Every bank of one kind, which is what a bare "bgm" or "se" in a script means. */
void cs2_audio_stop_kind(cs2_audio *audio, int kind, int fade_frames);

/* Everything, for leaving a scene. */
void cs2_audio_stop_all(cs2_audio *audio);

/*
 * Sets a bank's volume, 0 to 100. With fade_frames above zero it slides there
 * from `from` (or from where it is, when from is negative).
 */
void cs2_audio_volume(cs2_audio *audio, int kind, int bank, int from, int to, int fade_frames);

/* Whether a bank is sounding, which is how "is the voice still talking" is asked. */
int cs2_audio_playing(cs2_audio *audio, int kind, int bank);

/*
 * Mixes the next `frames` stereo frames into out, which holds frames * 2
 * samples. Silence is written when nothing is playing, so the frontend can call
 * this unconditionally. Called from the frontend's audio thread.
 */
void cs2_audio_mix(cs2_audio *audio, int16_t *out, int frames);

#endif
