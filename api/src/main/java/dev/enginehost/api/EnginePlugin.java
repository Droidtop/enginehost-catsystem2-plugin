package dev.enginehost.api;
public interface EnginePlugin {
    void onCreate(EnginePluginSession session) throws Exception;
    default boolean onControllerEvent(EngineControllerEvent event) throws Exception { return false; }
    default void onStart() throws Exception {}
    default void onResume() throws Exception {}
    default void onPause() throws Exception {}
    default void onStop() throws Exception {}
    default void onDestroy() throws Exception {}
}
