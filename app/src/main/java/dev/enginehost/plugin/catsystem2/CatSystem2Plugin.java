package dev.enginehost.plugin.catsystem2;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import java.io.IOException;

/**
 * The Android wrapper around the CatSystem2 engine.
 *
 * <p>The engine is the C in this repository's {@code src}, the same code the
 * desktop runner builds. It runs the game's own boot script, which puts the
 * game's own front end on the screen - the logo, the title screen, and the new
 * game that starts the system script - and draws each frame into a buffer of
 * pixels. Everything this class does is hand it the game folder, drive its
 * frames off the display's own clock, show the buffer, and pass the reader's
 * taps and pad back - a tap put back into the game's own pixels first, since
 * the game's screen is drawn scaled onto the console's. No part of the engine
 * is repeated in Java, and nothing here decides what the game shows.
 */
public final class CatSystem2Plugin implements EnginePlugin {
    static {
        System.loadLibrary("catsystem2");
    }

    /* The pad, as the engine numbers it. */
    private static final int KEY_UP = 0;
    private static final int KEY_DOWN = 1;
    private static final int KEY_LEFT = 2;
    private static final int KEY_RIGHT = 3;
    private static final int KEY_CONFIRM = 4;
    private static final int KEY_CANCEL = 5;

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

    @Override public void onPause() {
        if (view != null) view.setRunning(false);
        if (engine != 0) nativeSetSounding(engine, false);
    }

    @Override public void onResume() {
        if (engine != 0) nativeSetSounding(engine, true);
        if (view != null) view.setRunning(true);
    }

    @Override public void onDestroy() {
        if (view != null) view.setRunning(false);
        if (engine != 0) {
            nativeClose(engine);
            engine = 0;
        }
    }

    /**
     * The pad. The front end - the title screen, the scenario list, the menus -
     * is a set of buttons the game lays out itself, and the d-pad walks them;
     * confirm presses the one it is on, and cancel is what the layouts read as
     * a right click. Nothing here decides what a button does.
     */
    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        if (engine == 0 || !event.pressed()) return false;
        int key;
        switch (event.action()) {
            case "up": key = KEY_UP; break;
            case "down": key = KEY_DOWN; break;
            case "left": key = KEY_LEFT; break;
            case "right": key = KEY_RIGHT; break;
            case "confirm": case "page_next": key = KEY_CONFIRM; break;
            case "cancel": key = KEY_CANCEL; break;
            default: return false;
        }
        nativeKey(engine, key);
        return true;
    }

    /**
     * Shows the engine's picture, a frame at a time.
     *
     * <p>The front end is written in frames - fades, waits, a logo that plays
     * itself out - so the game is stepped once per display frame rather than
     * once per tap. The game is authored for a fixed virtual screen, so the
     * frame arrives at that size and is scaled to the console's, centred, with
     * the aspect ratio kept.
     */
    private final class ScreenView extends View implements Choreographer.FrameCallback {
        private final int width;
        private final int height;
        private final int[] pixels;
        private final Bitmap frame;
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Rect source;
        private final RectF destination = new RectF();
        private boolean running;

        ScreenView() {
            super(session.host().context());
            width = nativeWidth(engine);
            height = nativeHeight(engine);
            pixels = new int[width * height];
            frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            source = new Rect(0, 0, width, height);
            setBackgroundColor(Color.BLACK);
            setRunning(true);
        }

        void setRunning(boolean wanted) {
            if (wanted == running) return;
            running = wanted;
            if (wanted) Choreographer.getInstance().postFrameCallback(this);
            else Choreographer.getInstance().removeFrameCallback(this);
        }

        @Override public void doFrame(long frameTimeNanos) {
            if (!running || engine == 0) return;
            boolean alive = nativeStep(engine);
            nativeFrame(engine, pixels);
            frame.setPixels(pixels, 0, width, 0, 0, width, height);
            invalidate();
            if (alive) Choreographer.getInstance().postFrameCallback(this);
            else running = false;
        }

        /** Where the game's fixed screen sits on the console's, letterboxed. */
        private float scale() {
            return Math.min(getWidth() / (float) width, getHeight() / (float) height);
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float scale = scale();
            float left = (getWidth() - width * scale) / 2;
            float top = (getHeight() - height * scale) / 2;
            destination.set(left, top, left + width * scale, top + height * scale);
            canvas.drawBitmap(frame, source, destination, paint);
        }

        /**
         * A tap, in the game's own pixels. The game is authored for a fixed
         * screen and drawn scaled and centred on the console's, so a tap has to
         * be put back where the game would have seen it before the engine can
         * say which of its buttons it landed on.
         */
        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event.getAction() != MotionEvent.ACTION_UP || engine == 0) return true;
            float scale = scale();
            if (scale <= 0) return true;
            int x = Math.round((event.getX() - (getWidth() - width * scale) / 2) / scale);
            int y = Math.round((event.getY() - (getHeight() - height * scale) / 2) / scale);
            nativeTouch(engine, x, y);
            return true;
        }
    }

    private static native long nativeOpen(String gamePath, String script);
    private static native void nativeClose(long engine);
    private static native void nativeSetSounding(long engine, boolean sounding);
    private static native String nativeError();
    private static native int nativeWidth(long engine);
    private static native int nativeHeight(long engine);
    private static native boolean nativeStep(long engine);
    private static native void nativeFrame(long engine, int[] pixels);
    private static native void nativeTouch(long engine, int x, int y);
    private static native void nativeKey(long engine, int key);
}
