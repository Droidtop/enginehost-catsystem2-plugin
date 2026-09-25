package dev.enginehost.api;
import android.os.ParcelFileDescriptor;
import java.io.IOException;
public interface EngineFileBroker {
    String[] list(String relativePath) throws IOException;
    ParcelFileDescriptor openRead(String relativePath) throws IOException;
    ParcelFileDescriptor openWrite(String relativePath) throws IOException;
    void commitWrite(String relativePath) throws IOException;
    void delete(String relativePath) throws IOException;
}
