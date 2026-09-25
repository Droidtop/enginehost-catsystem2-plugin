package dev.enginehost.api;
import android.content.Context;
import java.io.File;
public interface EngineHost {
    Context context(); File saveDirectory(); File cacheDirectory(); EngineFileSystem fileSystem();
    default EngineFileBroker gameBroker() { return null; }
    default EngineFileBroker saveBroker() { return null; }
    void log(int priority, String tag, String message, Throwable error);
    boolean rumbleController(int deviceId, long durationMs, int amplitude); void finish();
}
