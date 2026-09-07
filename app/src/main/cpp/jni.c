/*
 * The Android side of the engine: nothing but a way in.
 *
 * The engine draws into a plain buffer of pixels and knows nothing about
 * Android; this file hands that buffer to Java and passes the reader's taps
 * back. Everything it calls lives in ../../../../src, which is also what the
 * desktop runner builds, so there is one engine and no second implementation of
 * anything.
 *
 * What runs is the game itself, from its own boot script: kcs.int/main.kcs
 * builds the screen and starts the plane holding the layout "flow", and flow.fes
 * is Grisaia's front end - the logo, the title screen, and the new game that
 * starts the system script. So the console gets the game's own opening rather
 * than a scene script picked for it, and the frame loop is the game's own sixty
 * a second rather than one picture per tap.
 */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include "audio.h"
#include "cs2.h"
#include "files.h"
#include "kcs.h"
#include "render.h"
#include "scene.h"
#include "startup.h"
#include "system.h"

#define TAG "catsystem2"
#define INSTRUCTION_BUDGET 2000000

/* The engine writes whole lines; logcat takes whole lines. */
static void to_logcat(const char *line, void *context) {
    (void) context;
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", line);
}

typedef struct {
    cs2_files *files;
    cs2_startup *startup;
    cs2_system *system;
    cs2_kcs *boot;              /* the game's own boot script */
    cs2_render *render;
    cs2_audio *audio;
    AAudioStream *sound;
    uint32_t *canvas;
    int width, height;
    int running;                /* the boot script has not ended or faulted */
} session;

static session *from_handle(jlong handle) {
    return (session *) (intptr_t) handle;
}

static jstring to_java(JNIEnv *env, const char *text) {
    return (*env)->NewStringUTF(env, text == NULL ? "" : text);
}

static int ends_with_cst(const char *name) {
    size_t length = strlen(name);
    return length > 4 && cs2_ieq(name + length - 4, ".cst");
}

/*
 * Sound. AAudio asks for samples on its own thread and the engine mixes into
 * whatever buffer it is handed, so this is the whole of it; the stream is
 * opened before it is started, so the mixer is always in place by the time the
 * first callback runs. A game with no sound device still plays: the engine
 * reads the sound commands either way and nothing here is required.
 */
static aaudio_data_callback_result_t feed_audio(AAudioStream *stream, void *user,
                                                void *frames, int32_t count) {
    (void) stream;
    session *state = (session *) user;
    cs2_audio_mix(state->audio, (int16_t *) frames, count);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void open_sound(session *state) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: AAudio will not start");
        return;
    }
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setDataCallback(builder, feed_audio, state);
    AAudioStream *stream = NULL;
    aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK || stream == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: %s",
                            AAudio_convertResultToText(opened));
        return;
    }
    state->audio = cs2_audio_new(state->files, AAudioStream_getSampleRate(stream));
    if (state->audio == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: %s", cs2_error());
        AAudioStream_close(stream);
        return;
    }
    cs2_system_set_audio(state->system, state->audio);
    state->sound = stream;
    AAudioStream_requestStart(stream);
    __android_log_print(ANDROID_LOG_INFO, TAG, "sound at %d Hz",
                        cs2_audio_rate(state->audio));
}

static void close_sound(session *state) {
    if (state->sound != NULL) {
        AAudioStream_requestStop(state->sound);
        AAudioStream_close(state->sound);
        state->sound = NULL;
    }
    cs2_system_set_audio(state->system, NULL);
    cs2_audio_free(state->audio);
    state->audio = NULL;
}

static void close_session(session *state) {
    if (state == NULL) return;
    close_sound(state);
    if (state->boot != NULL) {
        /* The block of persistent variables belongs to the engine's system
           side; take it back before the script frees what it was lent. */
        cs2_kcs_set_variables(state->boot, NULL, 0);
        cs2_kcs_free(state->boot);
    }
    cs2_render_free(state->render);
    cs2_system_free(state->system);
    cs2_startup_free(state->startup);
    cs2_files_close(state->files);
    free(state->canvas);
    free(state);
}

static int screen_size(const cs2_startup *startup, const char *key, int fallback) {
    int value = cs2_startup_number(startup, key, fallback);
    return value > 0 ? value : fallback;
}

JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeOpen(
        JNIEnv *env, jclass type, jstring game_path, jstring wanted_script) {
    (void) type;
    const char *root = (*env)->GetStringUTFChars(env, game_path, NULL);
    const char *wanted = wanted_script == NULL
        ? NULL : (*env)->GetStringUTFChars(env, wanted_script, NULL);

    /*
     * Everything the engine says goes to logcat under this plugin's own tag.
     * Without this it goes to stderr, which Android throws away, and a run on
     * the console can say nothing about what the game asked for: which button
     * a tap landed on, which layout is up, which engine function came next.
     * dq-catsystem2-24 spent a whole run unable to say why a tap did nothing.
     */
    cs2_log_to(to_logcat, NULL);

    session *state = calloc(1, sizeof *state);
    if (state == NULL) {
        cs2_set_error("out of memory");
        goto failed;
    }
    state->files = cs2_files_open(root);
    if (state->files == NULL) goto failed;

    const cs2_game_key *key = cs2_files_key(state->files);
    if (key->found) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "archive key from %s (passphrase %s); %zu archives",
            key->source, key->passphrase, cs2_files_archive_count(state->files));
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "no archive key found in the game's own binaries; only plain archives will open");
    }

    cs2_bytes document;
    if (cs2_files_read(state->files, "config.int/startup.xml", &document) == 0
        || cs2_files_read(state->files, "startup.xml", &document) == 0) {
        state->startup = cs2_startup_parse(document.data, document.size);
        cs2_bytes_free(&document);
    }

    /*
     * The boot script startup.xml names. A game whose entry point is a scene
     * script rather than a system script is not one this plugin runs: the front
     * end is what the console needs, and it is the system script that has it.
     */
    char path[512];
    const char *entry = cs2_startup_value(state->startup, "SCRIPT/start");
    if (wanted != NULL && wanted[0] != '\0' && !ends_with_cst(wanted)) entry = wanted;
    if (entry == NULL || ends_with_cst(entry)) entry = "kcs.int/main.kcs";
    if (strchr(entry, '/') != NULL) snprintf(path, sizeof path, "%s", entry);
    else snprintf(path, sizeof path, "kcs.int/%s", entry);

    state->width = screen_size(state->startup, "SCREEN/width", 1024);
    state->height = screen_size(state->startup, "SCREEN/height", 576);
    state->canvas = calloc((size_t) state->width * state->height, sizeof *state->canvas);
    state->boot = cs2_kcs_load(state->files, path);
    state->system = cs2_system_new(state->files, state->width, state->height);
    state->render = cs2_render_new(state->files, state->width, state->height);
    if (state->canvas == NULL || state->boot == NULL || state->system == NULL
        || state->render == NULL) {
        goto failed;
    }
    uint32_t variable_size = 0;
    void *variables = cs2_system_variables(state->system, &variable_size);
    cs2_kcs_set_variables(state->boot, variables, variable_size);
    cs2_kcs_set_gcall(state->boot, cs2_system_gcall, state->system);
    state->running = 1;
    __android_log_print(ANDROID_LOG_INFO, TAG, "the game boots into %s", path);

    open_sound(state);
    (*env)->ReleaseStringUTFChars(env, game_path, root);
    if (wanted != NULL) (*env)->ReleaseStringUTFChars(env, wanted_script, wanted);
    return (jlong) (intptr_t) state;

failed:
    __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
    close_session(state);
    (*env)->ReleaseStringUTFChars(env, game_path, root);
    if (wanted != NULL) (*env)->ReleaseStringUTFChars(env, wanted_script, wanted);
    return 0;
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeClose(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    close_session(from_handle(handle));
}

/* The reader left the game; the music should not follow them out of it. */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeSetSounding(
        JNIEnv *env, jclass type, jlong handle, jboolean sounding) {
    (void) env;
    (void) type;
    session *state = from_handle(handle);
    if (state == NULL || state->sound == NULL) return;
    if (sounding) AAudioStream_requestStart(state->sound);
    else AAudioStream_requestPause(state->sound);
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeError(JNIEnv *env, jclass type) {
    (void) type;
    return to_java(env, cs2_error());
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeWidth(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    return (jint) from_handle(handle)->width;
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeHeight(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    return (jint) from_handle(handle)->height;
}

/*
 * One frame of the game: the boot script, then the engine's own side - the
 * layout that is the front end, and the system script the layout has started.
 * Answers false once the game has ended or the script has faulted.
 */
JNIEXPORT jboolean JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeStep(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    session *state = from_handle(handle);
    if (state == NULL || !state->running) return JNI_FALSE;
    int running = cs2_kcs_frame(state->boot, INSTRUCTION_BUDGET);
    if (running < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
        state->running = 0;
        return JNI_FALSE;
    }
    cs2_system_frame(state->system);
    if (running == 0 || cs2_system_finished(state->system)) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "the game has ended");
        state->running = 0;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

/*
 * The picture: the scenario the front end has on the screen, with the planes
 * and their layouts over it. That is the order the game builds its screen -
 * a scene script underneath, the front end's own furniture above it.
 */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeFrame(
        JNIEnv *env, jclass type, jlong handle, jintArray pixels) {
    (void) type;
    session *state = from_handle(handle);
    if (state == NULL) return;
    size_t count = (size_t) state->width * state->height;
    const cs2_scene *scenario = cs2_system_scenario(state->system);
    if (scenario != NULL) {
        const uint32_t *drawn = cs2_render_frame(state->render, scenario, NULL);
        memcpy(state->canvas, drawn, count * sizeof *state->canvas);
    } else {
        for (size_t i = 0; i < count; i++) state->canvas[i] = 0xff000000u;
    }
    cs2_system_draw(state->system, state->canvas, state->width, state->height);
    (*env)->SetIntArrayRegion(env, pixels, 0, (jsize) count, (const jint *) state->canvas);
}

/*
 * A tap, where it landed. The front end reads it as a click on one of its own
 * buttons - the title screen's New Game is a button like any other - and the
 * scenario reads it as "the next line, please". Both are true of one tap, and
 * which of them means anything depends on what is on the screen, so both are
 * sent and the engine decides. The coordinates are the game's own screen, so
 * Java has already undone the scaling the console's display put on it.
 */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeTouch(
        JNIEnv *env, jclass type, jlong handle, jint x, jint y) {
    (void) env;
    (void) type;
    session *state = from_handle(handle);
    if (state == NULL) return;
    __android_log_print(ANDROID_LOG_INFO, TAG, "a tap at %d,%d in the game's own picture",
                        (int) x, (int) y);
    cs2_system_click(state->system, x, y, 1);
    cs2_system_event(state->system, 1);
}

/*
 * Where the reader is pointing, without pressing anything. The screen the game
 * has up keeps ONE selection: the button the pointer is over IS the button the
 * pad is on, so a finger dragged across the title screen lights the button it
 * passes over and the pad carries on from there. Java has already undone the
 * scaling, so these are the game's own pixels.
 */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativePointer(
        JNIEnv *env, jclass type, jlong handle, jint x, jint y) {
    (void) env;
    (void) type;
    session *state = from_handle(handle);
    if (state == NULL) return;
    cs2_system_pointer(state->system, x, y);
}

/*
 * The pad. The d-pad walks the buttons of whatever screen is up, over the
 * layout's own KEYBLOCK grids; confirm presses the one it is on and also
 * advances the scenario, because on a console it is the same button for both.
 */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeKey(
        JNIEnv *env, jclass type, jlong handle, jint key) {
    (void) env;
    (void) type;
    session *state = from_handle(handle);
    if (state == NULL) return;
    /*
     * Said whatever the key turns out to be, and said before it is turned into
     * anything, because the question a run has to answer first is whether the
     * pad reached this file at all. Twice now a device report has had to say
     * "the pad did nothing" without being able to say where it stopped.
     */
    __android_log_print(ANDROID_LOG_INFO, TAG, "the pad: key %d", (int) key);
    switch (key) {
    case 0: cs2_system_press(state->system, CS2_LAYOUT_UP); break;
    case 1: cs2_system_press(state->system, CS2_LAYOUT_DOWN); break;
    case 2: cs2_system_press(state->system, CS2_LAYOUT_LEFT); break;
    case 3: cs2_system_press(state->system, CS2_LAYOUT_RIGHT); break;
    case 4:
        cs2_system_press(state->system, CS2_LAYOUT_CONFIRM);
        cs2_system_event(state->system, 1);
        break;
    case 5: cs2_system_press(state->system, CS2_LAYOUT_CANCEL); break;
    default: break;
    }
}
