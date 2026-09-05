package dev.enginehost.plugin.catsystem2;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import org.json.JSONObject;

/**
 * A scene player for CatSystem2 games.
 *
 * <p>It reads the game's own archives, so it works on an untouched install, and
 * it plays a scene script: the background, the characters standing on it and the
 * dialogue, drawn on the virtual screen the game was authored for. It is not the
 * engine. A CatSystem2 game boots into the file named by {@code SCRIPT/start} in
 * startup.xml, which on every retail release is a compiled {@code .kcs} system
 * script driving the title screen, the menus and saving. Nothing here executes
 * KCS, so when the entry point is one, the plugin says so and plays the first
 * scene script instead.
 */
public final class CatSystem2Plugin implements EnginePlugin {
    private EnginePluginSession session;
    private GameFiles files;
    private SceneRunner runner;
    private ScriptView view;

    @Override public void onCreate(EnginePluginSession session) throws Exception {
        this.session = session;
        if (!"catsystem2".equals(session.engine()) || !"cst".equals(session.engineContext())) {
            throw new IOException("Unsupported CatSystem2 context");
        }
        files = GameFiles.open(new File(session.gamePath()));
        GameKey key = files.gameKey();
        log(Log.INFO, (key == null
            ? "No CatSystem2 archive key found in the game folder; only plain archives will open"
            : "Archive key from " + key.source().getName() + " (passphrase " + key.passphrase() + ")")
            + "; archives " + files.archiveNames(), null);

        Startup startup = readStartup();
        runner = new SceneRunner(files, CstText.from(startup));
        String note = choose(startup);
        log(Log.INFO, "Playing " + runner.path() + " (" + runner.lineCount() + " lines)"
            + (note == null ? "" : "; " + note), null);

        view = new ScriptView(startup, note, typeface());
        session.display().addView(view, new android.view.ViewGroup.LayoutParams(-1, -1));
    }

    @Override public void onDestroy() {
        if (files != null) files.close();
    }

    private void log(int priority, String message, Throwable error) {
        session.host().log(priority, "catsystem2", message, error);
    }

    private Startup readStartup() {
        try {
            byte[] document = files.read("config.int/startup.xml");
            if (document == null) document = files.read("startup.xml");
            return document == null ? null : Startup.parse(document);
        } catch (IOException error) {
            log(Log.WARN, "Could not read startup.xml", error);
            return null;
        }
    }

    /**
     * Loads the script to play and returns what the screen should say about that
     * choice, or null when the game's own entry point was played as it is.
     */
    private String choose(Startup startup) throws IOException {
        String exec = session.execFile();
        if (exec != null && !exec.isBlank()) {
            runner.play(exec);
            return null;
        }
        String start = startup == null ? null : startup.value("SCRIPT/start");
        if (start != null && start.toLowerCase(Locale.ROOT).endsWith(".cst")) {
            runner.play(start);
            return null;
        }
        String first = firstSceneScript();
        if (first == null) {
            throw new IOException("No CST script found in this game"
                + (files.gameKey() == null
                   ? "; its archives are encrypted and no key was found in the game folder"
                   : ""));
        }
        runner.play(first);
        return start == null
            ? "startup.xml named no entry point"
            : "the game boots into " + start + ", which this plugin cannot run yet";
    }

    /** The first script of scene.int, or of a loose scene folder, in name order. */
    private String firstSceneScript() {
        List<String> candidates = new ArrayList<>(files.namesIn("scene.int"));
        java.util.Collections.sort(candidates);
        for (String name : candidates) {
            if (name.toLowerCase(Locale.ROOT).endsWith(".cst")) return "scene.int/" + name;
        }
        File scene = new File(files.root(), "scene");
        File[] loose = (scene.isDirectory() ? scene : files.root())
            .listFiles((dir, name) -> name.toLowerCase(Locale.ROOT).endsWith(".cst"));
        if (loose == null || loose.length == 0) return null;
        java.util.Arrays.sort(loose);
        return (scene.isDirectory() ? "scene/" : "") + loose[0].getName();
    }

    /**
     * The game's own font. Retail CatSystem2 releases ship the face named in
     * startup.xml beside the archives, and the English ones map punctuation
     * through it, so text is only right when that font is the one drawn with.
     */
    private Typeface typeface() {
        File[] fonts = files.root().listFiles((dir, name) -> {
            String lower = name.toLowerCase(Locale.ROOT);
            return lower.endsWith(".ttf") || lower.endsWith(".otf") || lower.endsWith(".ttc");
        });
        if (fonts == null || fonts.length == 0) return null;
        java.util.Arrays.sort(fonts);
        try {
            return Typeface.createFromFile(fonts[0]);
        } catch (RuntimeException error) {
            log(Log.WARN, "Could not load " + fonts[0].getName(), error);
            return null;
        }
    }

    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        if (event.pressed() && ("confirm".equals(event.action()) || "page_next".equals(event.action()))) {
            view.advance();
            return true;
        }
        return false;
    }

    /**
     * Draws the stage the runner leaves behind. The game is authored for a fixed
     * virtual screen, so everything is laid out in those coordinates and the
     * whole picture is scaled to fit the console's, centred, with the aspect
     * ratio kept.
     */
    private final class ScriptView extends View {
        private static final int MAX_CACHED_IMAGES = 12;

        private final int screenWidth;
        private final int screenHeight;
        private final String title;
        private final String note;
        private final Paint statusPaint;
        private final Paint speakerPaint;
        private final Paint textPaint;
        private final Paint boxPaint;
        private final Paint layerPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Paint fillPaint = new Paint();
        private final Map<String, Bitmap> images = new LinkedHashMap<>();

        ScriptView(Startup startup, String note, Typeface typeface) {
            super(session.host().context());
            this.screenWidth = size(startup, "SCREEN/width", 1024);
            this.screenHeight = size(startup, "SCREEN/height", 576);
            String named = startup == null ? null : startup.value("APP/title");
            this.title = named == null ? "CatSystem2" : named;
            this.note = note;
            this.statusPaint = paint(22, 0xff6d7f8a, typeface);
            this.speakerPaint = paint(30, 0xff80cbc4, typeface);
            this.textPaint = paint(34, Color.WHITE, typeface);
            this.boxPaint = new Paint();
            this.boxPaint.setColor(0xcc000c10);
            setBackgroundColor(Color.BLACK);
            restore();
            invalidate();
        }

        private int size(Startup startup, String path, int fallback) {
            String value = startup == null ? null : startup.value(path);
            try {
                int parsed = value == null ? 0 : Integer.parseInt(value.trim());
                return parsed > 0 && parsed <= 8192 ? parsed : fallback;
            } catch (NumberFormatException error) {
                return fallback;
            }
        }

        private Paint paint(float size, int colour, Typeface typeface) {
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
            paint.setTextSize(size);
            paint.setColor(colour);
            if (typeface != null) paint.setTypeface(typeface);
            return paint;
        }

        void advance() {
            runner.advance();
            report();
            save();
            invalidate();
        }

        private void report() {
            java.util.Set<String> unhandled = runner.takeSkippedCommands();
            if (!unhandled.isEmpty()) {
                log(Log.INFO, "Commands not carried out yet: " + unhandled, null);
            }
        }

        /** Decodes an image on demand, keeping the last few in memory. */
        private Bitmap image(String name) {
            Bitmap cached = images.get(name);
            if (cached != null) return cached;
            try {
                byte[] data = files.read("image.int/" + name + ".hg3");
                if (data == null) data = files.read(name + ".hg3");
                if (data == null) {
                    log(Log.WARN, "No image named " + name, null);
                    return null;
                }
                Hg3Image.Frame frame = Hg3Image.decode(data, 0);
                Bitmap bitmap = Bitmap.createBitmap(frame.pixels(), frame.width(), frame.height(),
                    Bitmap.Config.ARGB_8888);
                if (images.size() >= MAX_CACHED_IMAGES) {
                    String oldest = images.keySet().iterator().next();
                    Bitmap dropped = images.remove(oldest);
                    if (dropped != null) dropped.recycle();
                }
                images.put(name, bitmap);
                return bitmap;
            } catch (IOException | RuntimeException error) {
                log(Log.WARN, "Could not decode " + name, error);
                return null;
            }
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float scale = Math.min(getWidth() / (float) screenWidth, getHeight() / (float) screenHeight);
            float left = (getWidth() - screenWidth * scale) / 2;
            float top = (getHeight() - screenHeight * scale) / 2;

            int saved = canvas.save();
            canvas.translate(left, top);
            canvas.scale(scale, scale);
            canvas.clipRect(0, 0, screenWidth, screenHeight);
            drawStage(canvas);
            drawMessage(canvas);
            canvas.restoreToCount(saved);

            canvas.drawText(status(), 16, 26, statusPaint);
        }

        private String status() {
            return title + "  -  " + runner.path() + "  " + runner.cursor() + "/" + runner.lineCount()
                + (note == null ? "" : "  (" + note + ")");
        }

        private void drawStage(Canvas canvas) {
            for (Stage.Layer layer : runner.stage().layers()) {
                if (layer.isColour()) {
                    fillPaint.setColor(layer.colour());
                    int width = layer.width() > 0 ? layer.width() : screenWidth;
                    int height = layer.height() > 0 ? layer.height() : screenHeight;
                    canvas.drawRect(layer.x(), layer.y(), layer.x() + width, layer.y() + height, fillPaint);
                    continue;
                }
                Bitmap bitmap = image(layer.image());
                if (bitmap == null) continue;
                layerPaint.setAlpha(layer.alpha());
                canvas.drawBitmap(bitmap, layer.x(), layer.y(), layerPaint);
            }
        }

        private void drawMessage(Canvas canvas) {
            String text = runner.text();
            String speaker = runner.speaker();
            if (text.isEmpty() && speaker.isEmpty()) return;
            float margin = screenWidth * 0.05f;
            float lineHeight = textPaint.getTextSize() * 1.3f;
            List<String> rows = new ArrayList<>();
            for (String paragraph : text.split("\n", -1)) {
                rows.addAll(wrap(paragraph, (int) (screenWidth - margin * 2)));
            }
            float boxHeight = lineHeight * (rows.size() + 1) + margin;
            float boxTop = screenHeight - boxHeight - margin / 2;
            canvas.drawRoundRect(new RectF(margin / 2, boxTop, screenWidth - margin / 2,
                screenHeight - margin / 4), 12, 12, boxPaint);

            float y = boxTop + margin / 2 + speakerPaint.getTextSize();
            if (!speaker.isEmpty()) canvas.drawText(speaker, margin, y, speakerPaint);
            y += lineHeight;
            for (String row : rows) {
                canvas.drawText(row, margin, y, textPaint);
                y += lineHeight;
            }
        }

        private List<String> wrap(String value, int width) {
            List<String> rows = new ArrayList<>();
            int start = 0;
            while (start < value.length()) {
                int count = textPaint.breakText(value, start, value.length(), true, width, null);
                if (count <= 0) break;
                int end = start + count;
                if (end < value.length()) {
                    int space = value.lastIndexOf(' ', end);
                    if (space > start) end = space + 1;
                }
                rows.add(value.substring(start, end));
                start = end;
            }
            if (rows.isEmpty()) rows.add("");
            return rows;
        }

        private File stateFile() {
            return new File(session.host().saveDirectory(), "catsystem2-scene-state.json");
        }

        /**
         * Puts the reader back where they left off. The stage is not saved, only
         * the position, so the script is replayed from its start with nothing
         * drawn until it reaches that position again; that rebuilds the
         * backgrounds and characters exactly as they were.
         */
        private void restore() {
            int position = 0;
            try {
                File file = stateFile();
                if (file.isFile() && file.length() <= 1024 * 1024) {
                    JSONObject json = new JSONObject(new String(Bytes.readAll(file, 1024 * 1024),
                        java.nio.charset.StandardCharsets.UTF_8));
                    String saved = json.optString("script", "");
                    if (!saved.isEmpty()) {
                        // The reader may have been in a script the first one
                        // chained to, so resume in that one rather than in the
                        // entry point.
                        if (!saved.equals(runner.path())) runner.play(saved);
                        position = Math.max(0, json.optInt("cursor", 0));
                    }
                }
            } catch (Exception error) {
                log(Log.WARN, "Ignoring invalid save state", error);
            }
            int guard = 0;
            do {
                runner.advance();
            } while (runner.cursor() < position && runner.stop() != SceneRunner.Stop.END
                     && ++guard < 100_000);
            report();
        }

        private void save() {
            try (FileOutputStream output = new FileOutputStream(stateFile(), false)) {
                JSONObject json = new JSONObject()
                    .put("script", runner.path())
                    .put("cursor", runner.cursor());
                output.write(json.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } catch (Exception error) {
                log(Log.ERROR, "Could not save state", error);
            }
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event.getAction() == MotionEvent.ACTION_UP) advance();
            return true;
        }
    }
}
