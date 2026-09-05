plugins { id("com.android.library") }

android {
    namespace = "dev.enginehost.plugin.catsystem2"
    compileSdk = 36
    defaultConfig {
        minSdk = 26
        // The console is arm64. Adding an ABI is a line here and a longer build,
        // not a change to anything in the engine.
        ndk { abiFilters += listOf("arm64-v8a") }
    }
    // The engine is compiled from the branch this wrapper is merged onto. On the
    // wrapper's own branch there is no src/ and CMake says so plainly.
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies { compileOnly(project(":api")) }
