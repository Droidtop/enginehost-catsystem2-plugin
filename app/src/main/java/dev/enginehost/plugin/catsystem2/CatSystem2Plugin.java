package dev.enginehost.plugin.catsystem2;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.List;
import org.json.JSONObject;

/** Experimental in-process CST dialogue/input interpreter. */
public final class CatSystem2Plugin implements EnginePlugin {
    private EnginePluginSession session;
    private ScriptView view;

    @Override public void onCreate(EnginePluginSession session) throws Exception {
        this.session = session;
        if (!"catsystem2".equals(session.engine()) || !"cst".equals(session.engineContext())) {
            throw new IOException("Unsupported CatSystem2 context");
        }
        view = new ScriptView(CstScript.read(resolveScript()));
        session.display().addView(view, new android.view.ViewGroup.LayoutParams(-1, -1));
    }

    private File resolveScript() throws IOException {
        File root = new File(session.gamePath()).getCanonicalFile();
        if (!root.isDirectory()) throw new IOException("CatSystem2 game folder is unreadable");
        String exec = session.execFile();
        if (exec != null && !exec.isBlank()) return confined(root, exec);
        File scene = new File(root, "scene");
        File[] scripts = (scene.isDirectory() ? scene : root)
            .listFiles((dir, name) -> name.toLowerCase(java.util.Locale.ROOT).endsWith(".cst"));
        if (scripts == null || scripts.length == 0) throw new IOException("No extracted CST script found; set execFile") ;
        java.util.Arrays.sort(scripts);
        return scripts[0].getCanonicalFile();
    }

    private File confined(File root, String relative) throws IOException {
        if (new File(relative).isAbsolute()) throw new IOException("execFile must be relative");
        File selected = new File(root, relative).getCanonicalFile();
        if (!selected.isFile() || !selected.getPath().startsWith(root.getPath() + File.separator)) {
            throw new IOException("CatSystem2 execFile leaves the game folder");
        }
        return selected;
    }

    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        if (event.pressed() && ("confirm".equals(event.action()) || "page_next".equals(event.action()))) {
            view.advance();
            return true;
        }
        return false;
    }

    private final class ScriptView extends View {
        private final List<CstScript.Line> lines;
        private final Paint speakerPaint = paint(30, 0xff80cbc4);
        private final Paint textPaint = paint(34, Color.WHITE);
        private int cursor;
        private String speaker = "";
        private String text = "";

        ScriptView(List<CstScript.Line> lines) {
            super(session.host().context());
            this.lines = lines;
            setBackgroundColor(Color.BLACK);
            restore();
            advance();
        }

        private Paint paint(float size, int color) {
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG); paint.setTextSize(size); paint.setColor(color); return paint;
        }

        void advance() {
            StringBuilder message = new StringBuilder();
            while (cursor < lines.size()) {
                CstScript.Line line = lines.get(cursor++);
                if (line.type() == CstScript.NAME) speaker = line.content();
                else if (line.type() == CstScript.MESSAGE) {
                    if (message.length() > 0) message.append('\n'); message.append(line.content());
                } else if ((line.type() == CstScript.INPUT || line.type() == CstScript.PAGE) && message.length() > 0) break;
            }
            text = message.toString(); save(); invalidate();
        }

        private File stateFile() { return new File(session.host().saveDirectory(), "catsystem2-experimental-state.json"); }
        private void restore() {
            try {
                File file = stateFile(); if (!file.isFile() || file.length() > 1024 * 1024) return;
                byte[] bytes = new byte[(int) file.length()];
                try (java.io.FileInputStream input = new java.io.FileInputStream(file)) {
                    int offset = 0; for (int n; offset < bytes.length && (n = input.read(bytes, offset, bytes.length - offset)) > 0;) offset += n;
                }
                JSONObject json = new JSONObject(new String(bytes, java.nio.charset.StandardCharsets.UTF_8));
                cursor = Math.max(0, Math.min(lines.size(), json.optInt("cursor", 0)));
                speaker = json.optString("speaker", "");
            } catch (Exception error) { session.host().log(android.util.Log.WARN, "catsystem2", "Ignoring invalid save state", error); }
        }
        private void save() {
            try (FileOutputStream output = new FileOutputStream(stateFile(), false)) {
                JSONObject json = new JSONObject().put("cursor", cursor).put("speaker", speaker);
                output.write(json.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } catch (Exception error) { session.host().log(android.util.Log.ERROR, "catsystem2", "Could not save state", error); }
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas); float x = 48, y = getHeight() * 0.62f; canvas.drawText(speaker, x, y, speakerPaint); y += 55;
            for (String paragraph : text.split("\\n", -1)) for (String row : wrap(paragraph, getWidth() - 96)) {
                canvas.drawText(row, x, y, textPaint); y += 44;
            }
        }
        private java.util.List<String> wrap(String value, int width) {
            java.util.List<String> rows = new java.util.ArrayList<>(); int start = 0;
            while (start < value.length()) { int count = textPaint.breakText(value, start, value.length(), true, width, null); if (count <= 0) break; rows.add(value.substring(start, start + count)); start += count; }
            if (rows.isEmpty()) rows.add(""); return rows;
        }
        @Override public boolean onTouchEvent(MotionEvent event) { if (event.getAction() == MotionEvent.ACTION_UP) advance(); return true; }
    }
}
