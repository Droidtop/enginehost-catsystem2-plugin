package dev.enginehost.plugin.catsystem2;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import org.json.JSONObject;

/**
 * The Android wrapper around the CatSystem2 engine.
 *
 * <p>The engine is the C in this repository's {@code src}, the same code the
 * desktop runner builds. It reads the game's archives, plays a scene script and
 * draws the result into a buffer of pixels; everything here does is hand it the
 * game folder, show that buffer, and pass the reader's taps back. No part of the
 * engine is repeated in Java.
 */
public final class CatSystem2Plugin implements EnginePlugin {
    static {
        System.loadLibrary("catsystem2");
    }

    private EnginePluginSession session;
    private long engine;
    private ScreenView view;

    @Override public void onCreate(EnginePluginSession session) throws Exception {
        this.session = session;
        if (!"catsystem2".equals(session.engine()) || !"cst".equals(session.engineContext())) {
            throw new IOException("Unsupported CatSystem2 context");
        }
        engine = nativeOpen(session.gamePath(), session.execFile());
        if (engine == 0) throw new IOException(nativeError());
        view = new ScreenView();
        session.display().addView(view, new android.view.ViewGroup.LayoutParams(-1, -1));
    }

    @Override public void onDestroy() {
        if (view != null) view.save();
        if (engine != 0) {
            nativeClose(engine);
            engine = 0;
        }
    }

    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        if (event.pressed() && ("confirm".equals(event.action()) || "page_next".equals(event.action()))) {
            view.advance();
            return true;
        }
        return false;
    }

    private void log(int priority, String message, Throwable error) {
        session.host().log(priority, "catsystem2", message, error);
    }

    /**
     * Shows the engine's picture. The game is authored for a fixed virtual
     * screen, so the frame arrives at that size and is scaled to the console's,
     * centred, with the aspect ratio kept.
     */
    private final class ScreenView extends View {
        private final int width;
        private final int height;
        private final int[] pixels;
        private final Bitmap frame;
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Rect source;
        private final RectF destination = new RectF();
        private final String title;

        ScreenView() {
            super(session.host().context());
            width = nativeWidth(engine);
            height = nativeHeight(engine);
            pixels = new int[width * height];
            frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            source = new Rect(0, 0, width, height);
            title = nativeTitle(engine);
            setBackgroundColor(Color.BLACK);
            restore();
            draw();
        }

        void advance() {
            nativeAdvance(engine);
            draw();
            save();
        }

        private void draw() {
            nativeFrame(engine, pixels, status());
            frame.setPixels(pixels, 0, width, 0, 0, width, height);
            invalidate();
        }

        private String status() {
            String note = nativeNote(engine);
            return title + "  -  " + nativePath(engine) + "  " + nativeCursor(engine)
                + "/" + nativeLineCount(engine) + (note.isEmpty() ? "" : "  (" + note + ")");
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float scale = Math.min(getWidth() / (float) width, getHeight() / (float) height);
            float left = (getWidth() - width * scale) / 2;
            float top = (getHeight() - height * scale) / 2;
            destination.set(left, top, left + width * scale, top + height * scale);
            canvas.drawBitmap(frame, source, destination, paint);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event.getAction() == MotionEvent.ACTION_UP) advance();
            return true;
        }

        private File stateFile() {
            return new File(session.host().saveDirectory(), "catsystem2-scene-state.json");
        }

        /**
         * Puts the reader back where they left off. Only the position is kept;
         * the engine replays the script to it with nothing drawn, which rebuilds
         * the backgrounds and characters exactly as the script put them.
         */
        private void restore() {
            try {
                File file = stateFile();
                if (!file.isFile() || file.length() > 1024 * 1024) return;
                byte[] stored = new byte[(int) file.length()];
                try (java.io.FileInputStream input = new java.io.FileInputStream(file)) {
                    int read = 0;
                    while (read < stored.length) {
                        int count = input.read(stored, read, stored.length - read);
                        if (count < 0) break;
                        read += count;
                    }
                }
                JSONObject json = new JSONObject(new String(stored,
                    java.nio.charset.StandardCharsets.UTF_8));
                String script = json.optString("script", "");
                int cursor = json.optInt("cursor", 0);
                if (!script.isEmpty() && cursor > 0) nativeSeek(engine, script, cursor);
            } catch (Exception error) {
                log(Log.WARN, "Ignoring invalid save state", error);
            }
        }

        void save() {
            try (FileOutputStream output = new FileOutputStream(stateFile(), false)) {
                JSONObject json = new JSONObject()
                    .put("script", nativePath(engine))
                    .put("cursor", nativeCursor(engine));
                output.write(json.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } catch (Exception error) {
                log(Log.ERROR, "Could not save state", error);
            }
        }
    }

    private static native long nativeOpen(String gamePath, String script);
    private static native void nativeClose(long engine);
    private static native String nativeError();
    private static native int nativeWidth(long engine);
    private static native int nativeHeight(long engine);
    private static native void nativeAdvance(long engine);
    private static native void nativeSeek(long engine, String script, int cursor);
    private static native void nativeFrame(long engine, int[] pixels, String status);
    private static native String nativeText(long engine);
    private static native String nativePath(long engine);
    private static native String nativeNote(long engine);
    private static native String nativeTitle(long engine);
    private static native int nativeCursor(long engine);
    private static native int nativeLineCount(long engine);
}
