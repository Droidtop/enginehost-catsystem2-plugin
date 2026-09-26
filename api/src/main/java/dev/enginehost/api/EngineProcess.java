package dev.enginehost.api;
/** Whether the calling code is running in Enginehost's own isolated runtime process (docs/engine-sandbox.md "Layer 2"). */
public final class EngineProcess {
    private EngineProcess() {}
    public static native boolean isIsolated();
}
