/*
 * The Android side of the engine: nothing but a way in.
 *
 * The engine draws into a plain buffer of pixels and knows nothing about
 * Android; this file hands that buffer to Java and passes the reader's taps
 * back. Everything it calls lives in ../../../../src, which is also what the
 * desktop runner builds, so there is one engine and no second implementation of
 * anything.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include "audio.h"
#include "files.h"
#include "render.h"
#include "scene.h"
#include "startup.h"
#include "text.h"

#define TAG "catsystem2"

typedef struct {
    cs2_files *files;
    cs2_startup *startup;
    cs2_text *text;
    cs2_scene *scene;
    cs2_render *render;
    cs2_audio *audio;
    AAudioStream *sound;
    char note[256];
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

/* The first script of scene.int, in name order, for a game we cannot boot. */
static int first_scene_script(cs2_files *files, char *out, size_t out_size) {
    cs2_kif *archive = cs2_files_archive(files, "scene.int");
    if (archive == NULL) return -1;
    const char *best = NULL;
    for (size_t i = 0; i < cs2_kif_count(archive); i++) {
        const char *name = cs2_kif_name(archive, i);
        size_t length = strlen(name);
        if (length < 5 || !cs2_ieq(name + length - 4, ".cst")) continue;
        if (best == NULL || strcmp(name, best) < 0) best = name;
    }
    if (best == NULL) return -1;
    snprintf(out, out_size, "scene.int/%s", best);
    return 0;
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
    cs2_scene_set_audio(state->scene, state->audio);
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
    cs2_audio_free(state->audio);
    state->audio = NULL;
}

static void close_session(session *state) {
    if (state == NULL) return;
    close_sound(state);
    cs2_render_free(state->render);
    cs2_scene_free(state->scene);
    cs2_text_free(state->text);
    cs2_startup_free(state->startup);
    cs2_files_close(state->files);
    free(state);
}

JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeOpen(
        JNIEnv *env, jclass type, jstring game_path, jstring wanted_script) {
    (void) type;
    const char *root = (*env)->GetStringUTFChars(env, game_path, NULL);
    const char *wanted = wanted_script == NULL
        ? NULL : (*env)->GetStringUTFChars(env, wanted_script, NULL);

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
    state->text = cs2_text_from(state->startup);
    state->scene = cs2_scene_new(state->files, state->text);
    if (state->text == NULL || state->scene == NULL) {
        cs2_set_error("out of memory");
        goto failed;
    }

    char script[512];
    const char *start = cs2_startup_value(state->startup, "SCRIPT/start");
    if (wanted != NULL && wanted[0] != '\0') {
        if (strchr(wanted, '/') != NULL) snprintf(script, sizeof script, "%s", wanted);
        else snprintf(script, sizeof script, "scene.int/%s", wanted);
    } else if (start != NULL && ends_with_cst(start)) {
        snprintf(script, sizeof script, "%s", start);
    } else if (first_scene_script(state->files, script, sizeof script) == 0) {
        if (start == NULL) {
            snprintf(state->note, sizeof state->note, "startup.xml names no entry point");
        } else {
            snprintf(state->note, sizeof state->note,
                     "the game boots into %s, which this plugin cannot run yet", start);
        }
    } else {
        cs2_set_error("this game holds no scene script to play");
        goto failed;
    }
    if (cs2_scene_play(state->scene, script) != 0) goto failed;
    __android_log_print(ANDROID_LOG_INFO, TAG, "playing %s (%zu lines)%s%s",
        cs2_scene_path(state->scene), cs2_scene_line_count(state->scene),
        state->note[0] == '\0' ? "" : "; ", state->note);

    state->render = cs2_render_new(state->files,
        cs2_startup_number(state->startup, "SCREEN/width", 1024),
        cs2_startup_number(state->startup, "SCREEN/height", 576));
    if (state->render == NULL) goto failed;
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
    return cs2_render_width(from_handle(handle)->render);
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeHeight(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    return cs2_render_height(from_handle(handle)->render);
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeAdvance(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    session *state = from_handle(handle);
    cs2_scene_advance(state->scene);
    const char *skipped = cs2_scene_take_skipped(state->scene);
    if (skipped[0] != '\0') {
        __android_log_print(ANDROID_LOG_INFO, TAG, "commands not carried out yet: %s", skipped);
    }
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeSeek(
        JNIEnv *env, jclass type, jlong handle, jstring script, jint cursor) {
    (void) type;
    session *state = from_handle(handle);
    if (script != NULL) {
        const char *path = (*env)->GetStringUTFChars(env, script, NULL);
        if (path[0] != '\0' && strcmp(path, cs2_scene_path(state->scene)) != 0) {
            cs2_scene_play(state->scene, path);
        }
        (*env)->ReleaseStringUTFChars(env, script, path);
    }
    cs2_scene_seek(state->scene, (size_t) (cursor < 0 ? 0 : cursor));
}

/* Fills a width * height int array with the frame, in Android's ARGB_8888. */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeFrame(
        JNIEnv *env, jclass type, jlong handle, jintArray pixels, jstring status) {
    (void) type;
    session *state = from_handle(handle);
    const char *line = status == NULL ? NULL : (*env)->GetStringUTFChars(env, status, NULL);
    const uint32_t *canvas = cs2_render_frame(state->render, state->scene, line);
    if (line != NULL) (*env)->ReleaseStringUTFChars(env, status, line);

    jsize wanted = (jsize) (cs2_render_width(state->render) * cs2_render_height(state->render));
    if ((*env)->GetArrayLength(env, pixels) < wanted) return;
    (*env)->SetIntArrayRegion(env, pixels, 0, wanted, (const jint *) canvas);
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeText(
        JNIEnv *env, jclass type, jlong handle) {
    (void) type;
    return to_java(env, cs2_scene_text(from_handle(handle)->scene));
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativePath(
        JNIEnv *env, jclass type, jlong handle) {
    (void) type;
    return to_java(env, cs2_scene_path(from_handle(handle)->scene));
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeNote(
        JNIEnv *env, jclass type, jlong handle) {
    (void) type;
    return to_java(env, from_handle(handle)->note);
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeTitle(
        JNIEnv *env, jclass type, jlong handle) {
    (void) type;
    const char *title = cs2_startup_value(from_handle(handle)->startup, "APP/title");
    return to_java(env, title == NULL ? "CatSystem2" : title);
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeCursor(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    return (jint) cs2_scene_cursor(from_handle(handle)->scene);
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeLineCount(
        JNIEnv *env, jclass type, jlong handle) {
    (void) env;
    (void) type;
    return (jint) cs2_scene_line_count(from_handle(handle)->scene);
}
