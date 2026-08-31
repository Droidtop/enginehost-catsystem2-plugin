plugins { id("com.android.library") }
android {
    namespace = "dev.enginehost.plugin.catsystem2"
    compileSdk = 36
    defaultConfig {
        minSdk = 26
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
dependencies { compileOnly(project(":api")) }
