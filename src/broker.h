/*
 * The host-brokered file root a game folder or save folder becomes under
 * an isolated runtime (docs/engine-sandbox.md "Host file service design").
 * The isolated process holds no storage permission of its own, so every
 * open crosses back into Java, which resolves and opens the real file
 * with the host process's own permission and hands back a descriptor
 * (app/src/main/cpp/jni.c bridges this to
 * dev.enginehost.api.EngineFileBroker). ctx is opaque to this engine: the
 * JNI layer owns it, and it carries whatever the callback needs to reach
 * back into Java from any thread the engine steps on.
 *
 * Every function here reports failure as -1 (or NULL/false as its return
 * type allows); none of them ever partially fill an output on failure.
 */
#ifndef CS2_BROKER_H
#define CS2_BROKER_H

/* A generous, one-time cap on how many names cs2_broker_list callers ask
   for: game roots hold a handful of archives, not thousands of entries,
   and this is never called per frame. */
#define CS2_BROKER_LIST_MAX 1024

typedef struct {
    void *ctx;

    /*
     * Entries directly inside relative_path ("" for the root itself),
     * into names (max_names entries of up to 255 bytes plus a NUL each).
     * Returns the count actually written, or -1.
     */
    int (*list)(void *ctx, const char *relative_path, char names[][256], int max_names);

    /* A descriptor open for reading relative_path, caller-owned (the
       caller closes it, directly or via fclose on an fdopen'd stream), or -1. */
    int (*open_read)(void *ctx, const char *relative_path);

    /* A descriptor to a fresh, not-yet-visible location for relative_path's
       new content, caller-owned, or -1. Write-root brokers only. */
    int (*open_write)(void *ctx, const char *relative_path);

    /* Makes the last open_write(relative_path) visible under that name.
       Returns 0 or -1. Write-root brokers only. */
    int (*commit_write)(void *ctx, const char *relative_path);

    /* Removes relative_path if present. Returns 0 or -1, never a distinct
       code for "was already absent". Write-root brokers only. */
    int (*remove)(void *ctx, const char *relative_path);
} cs2_broker;

#endif
