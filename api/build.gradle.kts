// Enginehost's plugin API, compiled against and never shipped: at run time the host supplies these
// classes. The source is Enginehost's own plugin-api/, checked out into build/enginehost by the
// Droidtop/enginehost .github/actions/plugin-api action at the commit the workflow pins
// (Droidtop/tracker#768: this repository used to commit a copy, which had fallen behind).
plugins { `java-library` }
val androidSdk = System.getenv("ANDROID_HOME") ?: System.getenv("ANDROID_SDK_ROOT")
    ?: error("ANDROID_HOME or ANDROID_SDK_ROOT is required")
val enginehostApi = rootProject.file("build/enginehost/plugin-api/src/main/java")
check(enginehostApi.isDirectory) {
    "Enginehost's plugin API is not checked out: clone Droidtop/enginehost into build/enginehost at the " +
        "commit .github/workflows/android.yml pins its plugin-api action to"
}
sourceSets.named("main") { java.srcDir(enginehostApi) }
dependencies { compileOnly(files("$androidSdk/platforms/android-36/android.jar")) }
java { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
