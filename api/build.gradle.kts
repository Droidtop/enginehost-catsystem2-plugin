plugins { `java-library` }
val androidSdk = System.getenv("ANDROID_HOME") ?: System.getenv("ANDROID_SDK_ROOT")
    ?: error("ANDROID_HOME or ANDROID_SDK_ROOT is required")
dependencies { compileOnly(files("$androidSdk/platforms/android-36/android.jar")) }
java { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
