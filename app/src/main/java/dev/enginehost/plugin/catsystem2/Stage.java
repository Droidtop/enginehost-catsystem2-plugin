package dev.enginehost.plugin.catsystem2;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * What is on screen: a handful of numbered slots per kind of layer, holding
 * either an image out of the game's archives or a flat colour.
 *
 * <p>A CatSystem2 script addresses layers the way the engine does, by kind and
 * slot: {@code bg 0} is the background, {@code cg 1} the second character
 * standing on it, {@code eg 5} an effect over everything, and the bare command
 * with no slot clears every layer of that kind. This holds only what is placed
 * where; decoding and drawing happen elsewhere, so the whole interpreter can be
 * run and checked without a screen.
 */
final class Stage {
    /** Layer kinds, in the order they are drawn, back to front. */
    static final int BACKGROUND = 0;
    static final int CHARACTER = 1;
    static final int FOREGROUND = 2;
    static final int FACE = 3;
    static final int EFFECT = 4;

    /** One placed layer. Exactly one of {@code image} and {@code colour} applies. */
    record Layer(int kind, int slot, String image, int frame, int colour,
                 int x, int y, int width, int height, int alpha) {
        boolean isColour() { return image == null; }
    }

    private final Map<Integer, Layer> layers = new LinkedHashMap<>();

    private static int key(int kind, int slot) { return kind * 1000 + slot; }

    void place(Layer layer) { layers.put(key(layer.kind(), layer.slot()), layer); }

    void clear(int kind) { layers.keySet().removeIf(key -> key / 1000 == kind); }

    void clear(int kind, int slot) { layers.remove(key(kind, slot)); }

    void clearAll() { layers.clear(); }

    /** Changes one layer's opacity, if that layer is on screen. */
    void blend(int kind, int slot, int alpha) {
        Layer layer = layers.get(key(kind, slot));
        if (layer == null) return;
        layers.put(key(kind, slot), new Layer(layer.kind(), layer.slot(), layer.image(), layer.frame(),
            layer.colour(), layer.x(), layer.y(), layer.width(), layer.height(),
            Math.max(0, Math.min(255, alpha))));
    }

    /** Everything on screen, back to front. */
    List<Layer> layers() {
        List<Layer> ordered = new ArrayList<>(layers.values());
        ordered.sort(Comparator.comparingInt((Layer layer) -> layer.kind())
            .thenComparingInt(Layer::slot));
        return ordered;
    }
}
