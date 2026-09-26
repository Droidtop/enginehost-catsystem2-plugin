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
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include "audio.h"
#include "broker.h"
#include "cs2.h"
#include "files.h"
#include "frametime.h"
#include "kcs.h"
#include "pictures.h"
#include "render.h"
#include "scene.h"
#include "startup.h"
#include "system.h"

#define TAG "catsystem2"
#define INSTRUCTION_BUDGET 2000000

/*
 * The isolated launch's audio ring (docs/engine-sandbox.md "Audio"): an
 * isolated process cannot reach AudioFlinger to open its own output at
 * all, so a game running isolated never opens AAudio here. Instead this
 * file renders into a plain shared-memory ring the host reads from and
 * plays on a real AudioTrack it owns -- the same layout as
 * dev.enginehost.EngineHost#isolatedAudioBuffer's own doc comment and
 * IsolatedRuntimeHost.kt's IsolatedAudioBridge on the host side: a
 * 16-byte header (write position, read position, capacity, reserved,
 * each little-endian uint32) followed by AUDIO_RING_CAPACITY bytes of
 * ring data. This side only ever advances the write position; the host
 * only ever advances the read position.
 */
#define AUDIO_HEADER_SIZE 16
#define AUDIO_RING_CAPACITY (32 * 1024)
/* One chunk is 1024 stereo 16-bit frames -- 4096 bytes, the same size the
   host's own consumer thread reads in one pass. */
#define AUDIO_CHUNK_FRAMES 1024

/* The engine writes whole lines; logcat takes whole lines. */
static void to_logcat(const char *line, void *context) {
    (void) context;
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", line);
}

/*
 * A dev.enginehost.api.EngineFileBroker, reached from native code
 * (docs/engine-sandbox.md "Host file service design"). Callbacks can fire
 * from whatever thread the engine is stepped on -- an isolated runtime's
 * step()/save calls arrive over Binder, which does not promise the same
 * pool thread twice -- so this holds a JavaVM and attaches per call rather
 * than assuming the JNIEnv that created it is still the right one.
 */
typedef struct {
    JavaVM *vm;
    jobject broker;       /* global ref */
    jclass broker_class;  /* global ref */
    jmethodID list_method;
    jmethodID open_read_method;
    jmethodID open_write_method;
    jmethodID commit_write_method;
    jmethodID delete_method;
} jni_broker;

typedef struct {
    cs2_files *files;
    cs2_startup *startup;
    cs2_system *system;
    cs2_kcs *boot;              /* the game's own boot script */
    cs2_pictures *pictures;     /* every picture of the game, decoded once */
    cs2_render *render;
    cs2_audio *audio;
    AAudioStream *sound;
    uint32_t *canvas;
    /*
     * The frame the screen is already showing. A reader looking at a line of
     * dialogue is looking at a picture that does not change, and handing Java
     * two and a half megabytes of pixels it already has, sixty times a second,
     * is work for nothing: what goes over is the rows that differ.
     */
    uint32_t *shown;
    int width, height;
    int running;                /* the boot script has not ended or faulted */
    /* Non-NULL only for an isolated launch (docs/engine-sandbox.md). */
    jni_broker *game_broker;
    jni_broker *save_broker;
    cs2_broker game_broker_ops;
    cs2_broker save_broker_ops;
    /*
     * The isolated launch's audio ring, and the thread that renders into
     * it. Non-NULL/running only when nativeOpenIsolated was handed a real
     * host-side buffer; an isolated launch with none simply plays
     * silently rather than touching AAudio, which would hang.
     */
    void *audio_ring;
    size_t audio_ring_size;
    int audio_ring_fd;
    pthread_t audio_thread;
    volatile int audio_thread_running;
} session;


/*
 * Creates the native side of one EngineFileBroker: a global ref (env's
 * caller frame ends before the engine is done with it) plus every method
 * ID this file calls, resolved once since a jmethodID, unlike a JNIEnv,
 * is valid on any thread for as long as the class is loaded.
 */
static jni_broker *jni_broker_create(JNIEnv *env, jobject broker) {
    if (broker == NULL) return NULL;
    jni_broker *jb = calloc(1, sizeof *jb);
    if (jb == NULL) return NULL;
    (*env)->GetJavaVM(env, &jb->vm);
    jb->broker = (*env)->NewGlobalRef(env, broker);
    jclass local_class = (*env)->GetObjectClass(env, broker);
    jb->broker_class = (jclass) (*env)->NewGlobalRef(env, local_class);
    (*env)->DeleteLocalRef(env, local_class);
    jb->list_method = (*env)->GetMethodID(
        env, jb->broker_class, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
    jb->open_read_method = (*env)->GetMethodID(
        env, jb->broker_class, "openRead", "(Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    jb->open_write_method = (*env)->GetMethodID(
        env, jb->broker_class, "openWrite", "(Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    jb->commit_write_method = (*env)->GetMethodID(env, jb->broker_class, "commitWrite", "(Ljava/lang/String;)V");
    jb->delete_method = (*env)->GetMethodID(env, jb->broker_class, "delete", "(Ljava/lang/String;)V");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, jb->broker);
        (*env)->DeleteGlobalRef(env, jb->broker_class);
        free(jb);
        return NULL;
    }
    return jb;
}

static void jni_broker_free(JNIEnv *env, jni_broker *jb) {
    if (jb == NULL) return;
    if (jb->broker != NULL) (*env)->DeleteGlobalRef(env, jb->broker);
    if (jb->broker_class != NULL) (*env)->DeleteGlobalRef(env, jb->broker_class);
    free(jb);
}

/* A JNIEnv valid on the calling thread, attaching it to the JVM if this is the first call on it. */
static JNIEnv *jni_broker_env(jni_broker *jb, int *attached) {
    JNIEnv *env = NULL;
    *attached = 0;
    if ((*jb->vm)->GetEnv(jb->vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*jb->vm)->AttachCurrentThread(jb->vm, &env, NULL) != 0) return NULL;
        *attached = 1;
    }
    return env;
}

/* Takes a ParcelFileDescriptor's underlying fd as this process's own (dup'd) and releases the Java object. */
static int jni_pfd_take(JNIEnv *env, jobject pfd) {
    if (pfd == NULL) return -1;
    jclass pfd_class = (*env)->GetObjectClass(env, pfd);
    jmethodID get_fd = (*env)->GetMethodID(env, pfd_class, "getFd", "()I");
    jint raw_fd = (*env)->CallIntMethod(env, pfd, get_fd);
    int native_fd = (*env)->ExceptionCheck(env) ? -1 : dup((int) raw_fd);
    jmethodID close_method = (*env)->GetMethodID(env, pfd_class, "close", "()V");
    (*env)->CallVoidMethod(env, pfd, close_method);
    (*env)->ExceptionClear(env); /* close() may throw; the dup above already has its own fd either way */
    (*env)->DeleteLocalRef(env, pfd_class);
    return native_fd;
}

static int broker_list(void *ctx, const char *relative_path, char names[][256], int max_names) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    jobjectArray result = (jobjectArray) (*env)->CallObjectMethod(env, jb->broker, jb->list_method, jpath);
    int count = -1;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    } else {
        count = 0;
        if (result != NULL) {
            jsize n = (*env)->GetArrayLength(env, result);
            for (jsize i = 0; i < n && count < max_names; i++) {
                jstring entry = (jstring) (*env)->GetObjectArrayElement(env, result, i);
                const char *bytes = (*env)->GetStringUTFChars(env, entry, NULL);
                snprintf(names[count], 256, "%s", bytes);
                (*env)->ReleaseStringUTFChars(env, entry, bytes);
                (*env)->DeleteLocalRef(env, entry);
                count++;
            }
        }
    }
    if (result != NULL) (*env)->DeleteLocalRef(env, result);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return count;
}

static int broker_open_read(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    jobject pfd = (*env)->CallObjectMethod(env, jb->broker, jb->open_read_method, jpath);
    int fd = -1;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    } else {
        fd = jni_pfd_take(env, pfd);
    }
    if (pfd != NULL) (*env)->DeleteLocalRef(env, pfd);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return fd;
}

static int broker_open_write(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    jobject pfd = (*env)->CallObjectMethod(env, jb->broker, jb->open_write_method, jpath);
    int fd = -1;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    } else {
        fd = jni_pfd_take(env, pfd);
    }
    if (pfd != NULL) (*env)->DeleteLocalRef(env, pfd);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return fd;
}

static int broker_commit_write(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    (*env)->CallVoidMethod(env, jb->broker, jb->commit_write_method, jpath);
    int ok = (*env)->ExceptionCheck(env) ? -1 : 0;
    if (ok != 0) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return ok;
}

static int broker_remove(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    (*env)->CallVoidMethod(env, jb->broker, jb->delete_method, jpath);
    int ok = (*env)->ExceptionCheck(env) ? -1 : 0;
    if (ok != 0) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return ok;
}

static void fill_broker(cs2_broker *out, jni_broker *jb) {
    out->ctx = jb;
    out->list = broker_list;
    out->open_read = broker_open_read;
    out->open_write = broker_open_write;
    out->commit_write = broker_commit_write;
    out->remove = broker_remove;
}

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

/*
 * The isolated launch's audio producer, running on its own thread: mixes
 * one chunk at a time and copies it into the shared ring, backing off
 * briefly when the host has not read enough of the last chunk yet. The
 * write position is the only thing this thread touches in the header;
 * the host's read position is only ever read here.
 */
static void *audio_produce(void *arg) {
    session *state = (session *) arg;
    uint8_t *ring = (uint8_t *) state->audio_ring;
    _Atomic uint32_t *write_pos = (_Atomic uint32_t *) (ring + 0);
    _Atomic uint32_t *read_pos = (_Atomic uint32_t *) (ring + 4);
    int16_t chunk[AUDIO_CHUNK_FRAMES * 2];
    size_t needed = sizeof chunk;
    while (state->audio_thread_running) {
        uint32_t w = atomic_load_explicit(write_pos, memory_order_relaxed);
        uint32_t r = atomic_load_explicit(read_pos, memory_order_acquire);
        uint32_t used = w - r;
        uint32_t free_bytes = (uint32_t) AUDIO_RING_CAPACITY - used;
        if (free_bytes < needed) {
            usleep(5000);
            continue;
        }
        cs2_audio_mix(state->audio, chunk, AUDIO_CHUNK_FRAMES);
        uint32_t start = w % (uint32_t) AUDIO_RING_CAPACITY;
        if ((size_t) start + needed <= (size_t) AUDIO_RING_CAPACITY) {
            memcpy(ring + AUDIO_HEADER_SIZE + start, chunk, needed);
        } else {
            size_t first = (size_t) AUDIO_RING_CAPACITY - start;
            memcpy(ring + AUDIO_HEADER_SIZE + start, chunk, first);
            memcpy(ring + AUDIO_HEADER_SIZE, (const uint8_t *) chunk + first, needed - first);
        }
        atomic_store_explicit(write_pos, w + (uint32_t) needed, memory_order_release);
    }
    return NULL;
}

/*
 * Sound for an isolated launch: no AAudio at all, since this process
 * cannot reach AudioFlinger to open it (dq-sandbox-03 found that hangs
 * forever rather than failing). audio_fd is a host-owned SharedMemory
 * region, already sized and zeroed by IsolatedRuntimeHost's
 * IsolatedAudioBridge; -1 means the host itself could not set one up, in
 * which case the game plays silently rather than touching AAudio.
 */
static void open_sound_bridged(session *state, int audio_fd, int sample_rate) {
    if (audio_fd < 0 || sample_rate <= 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "no sound: the host had no isolated audio buffer to hand over");
        return;
    }
    size_t map_size = AUDIO_HEADER_SIZE + AUDIO_RING_CAPACITY;
    void *ring = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, audio_fd, 0);
    if (ring == MAP_FAILED) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: mmap of the audio buffer failed (%s)", strerror(errno));
        close(audio_fd);
        return;
    }
    state->audio = cs2_audio_new(state->files, sample_rate);
    if (state->audio == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: %s", cs2_error());
        munmap(ring, map_size);
        close(audio_fd);
        return;
    }
    cs2_system_set_audio(state->system, state->audio);
    state->audio_ring = ring;
    state->audio_ring_size = map_size;
    state->audio_ring_fd = audio_fd;
    state->audio_thread_running = 1;
    if (pthread_create(&state->audio_thread, NULL, audio_produce, state) != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: could not start the audio thread");
        state->audio_thread_running = 0;
        cs2_system_set_audio(state->system, NULL);
        cs2_audio_free(state->audio);
        state->audio = NULL;
        munmap(ring, map_size);
        close(audio_fd);
        state->audio_ring = NULL;
        state->audio_ring_fd = -1;
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "sound at %d Hz, bridged to the host", sample_rate);
}

static void close_sound(session *state) {
    if (state->sound != NULL) {
        AAudioStream_requestStop(state->sound);
        AAudioStream_close(state->sound);
        state->sound = NULL;
    }
    if (state->audio_thread_running) {
        state->audio_thread_running = 0;
        pthread_join(state->audio_thread, NULL);
    }
    if (state->audio_ring != NULL) {
        munmap(state->audio_ring, state->audio_ring_size);
        state->audio_ring = NULL;
    }
    if (state->audio_ring_fd >= 0) {
        close(state->audio_ring_fd);
        state->audio_ring_fd = -1;
    }
    cs2_system_set_audio(state->system, NULL);
    cs2_audio_free(state->audio);
    state->audio = NULL;
}

static void close_session(JNIEnv *env, session *state) {
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
    cs2_pictures_free(state->pictures);
    cs2_startup_free(state->startup);
    cs2_files_close(state->files);
    free(state->canvas);
    free(state->shown);
    /* Freed last: nothing above calls back into a broker, only closes
       fds/FILE*s that already came from one. */
    jni_broker_free(env, state->game_broker);
    jni_broker_free(env, state->save_broker);
    free(state);
}

static int screen_size(const cs2_startup *startup, const char *key, int fallback) {
    int value = cs2_startup_number(startup, key, fallback);
    return value > 0 ? value : fallback;
}

/*
 * Everything past having a cs2_files open: reads startup.xml, resolves the
 * boot script, sizes the canvas, brings up pictures/system/render, wires
 * saves (a real folder or a broker, whichever the caller gave), and starts
 * sound. nativeOpen and nativeOpenIsolated differ only in how cs2_files and
 * the save store are obtained; everything downstream of that is this one
 * mechanism, not two.
 */
static jlong finish_open(JNIEnv *env, session *state, const char *wanted,
                         const char *save_folder, const cs2_broker *save_broker,
                         int isolated, int audio_fd, int audio_rate) {
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
    state->shown = calloc((size_t) state->width * state->height, sizeof *state->shown);
    state->boot = cs2_kcs_load(state->files, path);
    state->pictures = cs2_pictures_new(state->files, 0);
    state->system = cs2_system_new(state->files, state->pictures, state->width, state->height);
    state->render = cs2_render_new(state->files, state->pictures, state->width, state->height);
    if (state->canvas == NULL || state->shown == NULL || state->boot == NULL
        || state->pictures == NULL || state->system == NULL || state->render == NULL) {
        goto failed;
    }
    /*
     * Where the game's own saves go. The game folder is the reader's and on this
     * console it is a card that is not written to, so the host hands the plugin
     * a folder of its own and the engine is told it. A game given none plays and
     * simply cannot save.
     */
    if (save_broker != NULL) {
        cs2_system_set_save_broker(state->system, save_broker);
    } else if (save_folder != NULL && save_folder[0] != 0) {
        cs2_system_set_save_folder(state->system, save_folder);
    }
    /*
     * And the frame-time line, which is how the console answers "why is this
     * slow": one line a second in logcat saying where the frame went.
     */
    cs2_frametime(1);
    uint32_t variable_size = 0;
    void *variables = cs2_system_variables(state->system, &variable_size);
    cs2_kcs_set_variables(state->boot, variables, variable_size);
    cs2_kcs_set_gcall(state->boot, cs2_system_gcall, state->system);
    state->running = 1;
    __android_log_print(ANDROID_LOG_INFO, TAG, "the game boots into %s", path);

    if (isolated) open_sound_bridged(state, audio_fd, audio_rate);
    else open_sound(state);
    return (jlong) (intptr_t) state;

failed:
    __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
    close_session(env, state);
    return 0;
}

JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeOpen(
        JNIEnv *env, jclass type, jstring game_path, jstring wanted_script,
        jstring save_folder) {
    (void) type;
    const char *root = (*env)->GetStringUTFChars(env, game_path, NULL);
    const char *wanted = wanted_script == NULL
        ? NULL : (*env)->GetStringUTFChars(env, wanted_script, NULL);
    const char *saves = save_folder == NULL
        ? NULL : (*env)->GetStringUTFChars(env, save_folder, NULL);

    /*
     * Everything the engine says goes to logcat under this plugin's own tag.
     * Without this it goes to stderr, which Android throws away, and a run on
     * the console can say nothing about what the game asked for: which button
     * a tap landed on, which layout is up, which engine function came next.
     * dq-catsystem2-24 spent a whole run unable to say why a tap did nothing.
     */
    cs2_log_to(to_logcat, NULL);

    jlong handle = 0;
    session *state = calloc(1, sizeof *state);
    if (state == NULL) {
        cs2_set_error("out of memory");
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
    } else {
        state->audio_ring_fd = -1; /* calloc leaves 0, which is a real fd (stdin) */
        state->files = cs2_files_open(root);
        if (state->files == NULL) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
            close_session(env, state);
        } else {
            handle = finish_open(env, state, wanted, saves, NULL, /*isolated=*/0, /*audio_fd=*/-1, /*audio_rate=*/0);
        }
    }
    (*env)->ReleaseStringUTFChars(env, game_path, root);
    if (wanted != NULL) (*env)->ReleaseStringUTFChars(env, wanted_script, wanted);
    if (saves != NULL) (*env)->ReleaseStringUTFChars(env, save_folder, saves);
    return handle;
}

/*
 * Sandbox layer 2 (docs/engine-sandbox.md), first milestone: the same open,
 * with the game folder and the save folder each a host-brokered
 * EngineFileBroker instead of a real path -- this process, under
 * android:isolatedProcess, cannot resolve either path itself. audio_buffer
 * is the host's shared ring (docs/engine-sandbox.md "Audio"), or null when
 * the host could not make one; either way this never opens AAudio itself.
 */
JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeOpenIsolated(
        JNIEnv *env, jclass type, jobject game_broker, jstring wanted_script,
        jobject save_broker, jobject audio_buffer, jint audio_sample_rate) {
    (void) type;
    const char *wanted = wanted_script == NULL
        ? NULL : (*env)->GetStringUTFChars(env, wanted_script, NULL);

    cs2_log_to(to_logcat, NULL);

    jlong handle = 0;
    session *state = calloc(1, sizeof *state);
    if (state == NULL) {
        cs2_set_error("out of memory");
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
    } else {
        state->audio_ring_fd = -1; /* calloc leaves 0, which is a real fd (stdin) */
        state->game_broker = jni_broker_create(env, game_broker);
        if (state->game_broker == NULL) {
            cs2_set_error("no host file broker for the game folder");
            __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
            close_session(env, state);
        } else {
            fill_broker(&state->game_broker_ops, state->game_broker);
            state->files = cs2_files_open_via_broker(&state->game_broker_ops);
            if (state->files == NULL) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", cs2_error());
                close_session(env, state);
            } else {
                const cs2_broker *save_ops = NULL;
                if (save_broker != NULL) {
                    state->save_broker = jni_broker_create(env, save_broker);
                    if (state->save_broker != NULL) {
                        fill_broker(&state->save_broker_ops, state->save_broker);
                        save_ops = &state->save_broker_ops;
                    }
                }
                /* Takes and closes the Java-side ParcelFileDescriptor; -1 when there was none. */
                int audio_fd = jni_pfd_take(env, audio_buffer);
                handle = finish_open(env, state, wanted, NULL, save_ops,
                                     /*isolated=*/1, audio_fd, (int) audio_sample_rate);
            }
        }
    }
    if (wanted != NULL) (*env)->ReleaseStringUTFChars(env, wanted_script, wanted);
    return handle;
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeClose(
        JNIEnv *env, jclass type, jlong handle) {
    (void) type;
    close_session(env, from_handle(handle));
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
JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeFrame(
        JNIEnv *env, jclass type, jlong handle, jintArray pixels) {
    (void) type;
    session *state = from_handle(handle);
    if (state == NULL) return 0;
    size_t count = (size_t) state->width * state->height;
    uint64_t composing = cs2_now();
    const cs2_scene *scenario = cs2_system_scenario(state->system);
    if (scenario != NULL) {
        const uint32_t *drawn = cs2_render_frame(state->render, scenario, NULL);
        memcpy(state->canvas, drawn, count * sizeof *state->canvas);
    } else {
        for (size_t i = 0; i < count; i++) state->canvas[i] = 0xff000000u;
    }
    cs2_system_draw(state->system, state->canvas, state->width, state->height);
    cs2_span_add(CS2_SPAN_COMPOSE, composing);

    /*
     * What is new about this picture, as whole rows: the first row that differs
     * from the one on the screen and the last. A reader reading is looking at a
     * picture where the only thing moving is the line of text appearing, so most
     * frames are a band a tenth of the screen high and many are nothing at all -
     * and a row of pixels not handed over is a row Java does not copy into its
     * bitmap and the display does not redraw.
     */
    uint64_t handing = cs2_now();
    int first = -1, last = -1;
    size_t row_bytes = (size_t) state->width * sizeof *state->canvas;
    for (int row = 0; row < state->height; row++) {
        const uint32_t *made = state->canvas + (size_t) row * state->width;
        uint32_t *showing = state->shown + (size_t) row * state->width;
        if (memcmp(made, showing, row_bytes) == 0) continue;
        memcpy(showing, made, row_bytes);
        if (first < 0) first = row;
        last = row;
    }
    if (first >= 0) {
        (*env)->SetIntArrayRegion(env, pixels, (jsize) ((size_t) first * state->width),
                                  (jsize) ((size_t) (last - first + 1) * state->width),
                                  (const jint *) (state->canvas + (size_t) first * state->width));
    }
    cs2_span_add(CS2_SPAN_UPLOAD, handing);
    cs2_frame_done();
    if (first < 0) return 0;
    return (jint) (((uint32_t) first << 16) | (uint32_t) (last - first + 1));
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
 * advances the scenario when no screen takes it, as Enter does for the
 * original, where Confirm and Advance Text share the key. Advance Text goes
 * on whatever the screen has up.
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
    case 6:
        cs2_system_advance(state->system);
        cs2_system_event(state->system, 1);
        break;
    default: break;
    }
}

/*
 * Sandbox layer 2, isolated launches only (docs/engine-sandbox.md
 * "Audio", the InMemoryDexClassLoader pivot): under Android 14's
 * "safer dynamic code loading", ART refuses a path-based dex file this
 * process itself made even sealed non-writable (dq-sandbox-09 confirmed
 * this directly -- the seal is not what the check inspects). The host
 * loads this plugin's dex via InMemoryDexClassLoader instead, which is
 * `final` and cannot override findLibrary the way the in-process path's
 * loader does, so this library's own native methods are never
 * auto-bound there and the static initialiser's own
 * System.loadLibrary("catsystem2") always fails under isolation
 * (caught and ignored, CatSystem2Plugin.java).
 *
 * IsolatedRuntimeService dlopens this library directly (a /proc/self/fd
 * path to the same host-verified .so every other launch shape uses) and
 * dlsyms this EXACT, fixed, non-JNI symbol name -- not
 * Java_..._nativeOpen-style, so the JVM never tries to auto-bind it --
 * calling it once with a JNIEnv and this plugin's own Class object,
 * already correctly resolved by ordinary Java reflection on the host's
 * side. No FindClass, no classloader ambiguity: RegisterNatives binds
 * these exact, already-linked function pointers to that jclass directly,
 * regardless of which classloader loaded this library or defined that
 * class. Every one of this plugin's own native methods is listed here,
 * once, so nothing needs a second declaration to stay in sync as they
 * change -- the addresses are the same functions this file already
 * defines for the ordinary JNI auto-binding path.
 */
JNIEXPORT void JNICALL
enginehost_register_natives(JNIEnv *env, jclass clazz) {
    static const JNINativeMethod methods[] = {
        {"nativeOpen", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J",
         (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeOpen},
        {"nativeOpenIsolated",
         "(Ldev/enginehost/api/EngineFileBroker;Ljava/lang/String;Ldev/enginehost/api/EngineFileBroker;"
         "Landroid/os/ParcelFileDescriptor;I)J",
         (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeOpenIsolated},
        {"nativeClose", "(J)V", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeClose},
        {"nativeSetSounding", "(JZ)V", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeSetSounding},
        {"nativeError", "()Ljava/lang/String;", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeError},
        {"nativeWidth", "(J)I", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeWidth},
        {"nativeHeight", "(J)I", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeHeight},
        {"nativeStep", "(J)Z", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeStep},
        {"nativeFrame", "(J[I)I", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeFrame},
        {"nativeTouch", "(JII)V", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeTouch},
        {"nativePointer", "(JII)V", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativePointer},
        {"nativeKey", "(JI)V", (void *) Java_dev_enginehost_plugin_catsystem2_CatSystem2Plugin_nativeKey},
    };
    (*env)->RegisterNatives(env, clazz, methods, (jint) (sizeof(methods) / sizeof(methods[0])));
}
