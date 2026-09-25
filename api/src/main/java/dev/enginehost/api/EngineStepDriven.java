package dev.enginehost.api;
public interface EngineStepDriven {
    int pixelWidth();
    int pixelHeight();
    int step(int[] pixels);
    void onPointerMove(int x, int y);
    void onPointerUp(int x, int y);
}
