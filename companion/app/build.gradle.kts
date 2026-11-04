plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.weltenaichaos.d2rcompanion"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.weltenaichaos.d2rcompanion"
        minSdk = 29
        targetSdk = 35
        versionCode = (System.getenv("COMPANION_BUILD") ?: "1").toInt()
        versionName = "0.1." + (System.getenv("COMPANION_BUILD") ?: "1")
    }

    // Fixed test key so a new build installs over the old one.
    signingConfigs {
        getByName("debug") {
            storeFile = rootProject.file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    // On-device text recognition, model bundled in the APK (works offline).
    implementation("com.google.mlkit:text-recognition:16.0.1")
}
