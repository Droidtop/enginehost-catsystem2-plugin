package dev.enginehost.plugin.catsystem2;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
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
import java.util.List;
import java.util.Locale;
import org.json.JSONObject;

/**
 * Experimental in-process CST dialogue/input interpreter.
 *
 * <p>It reads the game's own archives, so it works on an untouched install; it
 * does not yet run the engine. A CatSystem2 game boots into the file named by
 * {@code SCRIPT/start} in startup.xml, which on every retail release is a
 * compiled {@code .kcs} system script driving the title screen and the menus.
 * Nothing here executes KCS, so when the entry point is one, the plugin says so
 * and plays the first scene script instead.
 */
public final class CatSystem2Plugin implements EnginePlugin {
    private EnginePluginSession session;
    private GameFiles files;
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
        Script script = chooseScript(startup);
        log(Log.INFO, "Playing " + script.path() + " (" + script.lines().size() + " lines)"
            + (script.note() == null ? "" : "; " + script.note()), null);

        view = new ScriptView(script, startup, typeface());
        session.display().addView(view, new android.view.ViewGroup.LayoutParams(-1, -1));
    }

    @Override public void onDestroy() {
        if (files != null) files.close();
    }

    private void log(int priority, String message, Throwable error) {
        session.host().log(priority, "catsystem2", message, error);
    }

    /** What the plugin ended up playing, and why, so the screen can say so. */
    private record Script(String path, List<CstScript.Line> lines, String note) {}

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

    private Script chooseScript(Startup startup) throws IOException {
        String exec = session.execFile();
        if (exec != null && !exec.isBlank()) {
            byte[] data = files.read(exec.replace('\\', '/'));
            if (data == null) throw new IOException("The chosen script \"" + exec + "\" is not in this game");
            return new Script(exec, CstScript.read(data, exec), null);
        }

        String start = startup == null ? null : startup.value("SCRIPT/start");
        if (start != null && start.toLowerCase(Locale.ROOT).endsWith(".cst")) {
            byte[] data = files.read(start.replace('\\', '/'));
            if (data != null) return new Script(start, CstScript.read(data, start), null);
        }

        String note = start == null
            ? "startup.xml named no entry point"
            : "the game boots into " + start + ", which this plugin cannot run yet";
        String first = firstSceneScript();
        if (first == null) {
            throw new IOException("No CST script found in this game"
                + (files.gameKey() == null
                   ? "; its archives are encrypted and no key was found in the game folder"
                   : "; " + note));
        }
        byte[] data = files.read(first);
        if (data == null) throw new IOException("Could not read " + first);
        return new Script(first, CstScript.read(data, first), note);
    }

    /** The first script of scene.int, or of a loose scene folder, in name order. */
    private String firstSceneScript() throws IOException {
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

    private final class ScriptView extends View {
        private final Script script;
        private final CstText formatter;
        private final Paint statusPaint;
        private final Paint speakerPaint;
        private final Paint textPaint;
        private final String status;
        private int cursor;
        private String speaker = "";
        private String text = "";

        ScriptView(Script script, Startup startup, Typeface typeface) {
            super(session.host().context());
            this.script = script;
            this.formatter = CstText.from(startup);
            this.statusPaint = paint(22, 0xff6d7f8a, typeface);
            this.speakerPaint = paint(30, 0xff80cbc4, typeface);
            this.textPaint = paint(34, Color.WHITE, typeface);
            String title = startup == null ? null : startup.value("APP/title");
            this.status = (title == null ? "CatSystem2" : title) + "  -  " + script.path()
                + (script.note() == null ? "" : "  (" + script.note() + ")");
            setBackgroundColor(Color.BLACK);
            restore();
            advance();
        }

        private Paint paint(float size, int color, Typeface typeface) {
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
            paint.setTextSize(size);
            paint.setColor(color);
            if (typeface != null) paint.setTypeface(typeface);
            return paint;
        }

        void advance() {
            StringBuilder message = new StringBuilder();
            List<CstScript.Line> lines = script.lines();
            while (cursor < lines.size()) {
                CstScript.Line line = lines.get(cursor++);
                if (line.type() == CstScript.NAME) {
                    speaker = formatter.display(line.content());
                } else if (line.type() == CstScript.MESSAGE) {
                    if (message.length() > 0) message.append('\n');
                    message.append(formatter.display(line.content()));
                } else if ((line.type() == CstScript.INPUT || line.type() == CstScript.PAGE)
                           && message.length() > 0) {
                    break;
                }
            }
            text = message.toString();
            save();
            invalidate();
        }

        private File stateFile() { return new File(session.host().saveDirectory(), "catsystem2-experimental-state.json"); }

        private void restore() {
            try {
                File file = stateFile();
                if (!file.isFile() || file.length() > 1024 * 1024) return;
                JSONObject json = new JSONObject(new String(Bytes.readAll(file, 1024 * 1024),
                    java.nio.charset.StandardCharsets.UTF_8));
                if (!script.path().equals(json.optString("script", script.path()))) return;
                cursor = Math.max(0, Math.min(script.lines().size(), json.optInt("cursor", 0)));
                speaker = json.optString("speaker", "");
            } catch (Exception error) {
                log(Log.WARN, "Ignoring invalid save state", error);
            }
        }

        private void save() {
            try (FileOutputStream output = new FileOutputStream(stateFile(), false)) {
                JSONObject json = new JSONObject()
                    .put("script", script.path())
                    .put("cursor", cursor)
                    .put("speaker", speaker);
                output.write(json.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } catch (Exception error) {
                log(Log.ERROR, "Could not save state", error);
            }
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float x = 48;
            canvas.drawText(status, x, 40, statusPaint);
            float y = getHeight() * 0.62f;
            canvas.drawText(speaker, x, y, speakerPaint);
            y += 55;
            for (String paragraph : text.split("\\n", -1)) {
                for (String row : wrap(paragraph, getWidth() - 96)) {
                    canvas.drawText(row, x, y, textPaint);
                    y += 44;
                }
            }
        }

        private List<String> wrap(String value, int width) {
            List<String> rows = new ArrayList<>();
            int start = 0;
            while (start < value.length()) {
                int count = textPaint.breakText(value, start, value.length(), true, width, null);
                if (count <= 0) break;
                rows.add(value.substring(start, start + count));
                start += count;
            }
            if (rows.isEmpty()) rows.add("");
            return rows;
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event.getAction() == MotionEvent.ACTION_UP) advance();
            return true;
        }
    }
}
