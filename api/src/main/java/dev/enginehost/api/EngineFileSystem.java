package dev.enginehost.api;
import java.io.File;
import java.io.IOException;
public interface EngineFileSystem {
    File gameRoot(); File resolveGameFile(String relativePath) throws IOException;
    File saveRoot(); File resolveSaveFile(String relativePath) throws IOException;
}
