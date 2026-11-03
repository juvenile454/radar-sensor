import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}

// Persönliche Einstellungen aus android/local.properties (nicht im Repo), z. B. radar.bridge=<host>:7777
val localProps =
    Properties().apply {
        rootProject.file("local.properties").takeIf { it.isFile }?.inputStream()?.use { load(it) }
    }

android {
    namespace = "de.timo.radarsensor"
    compileSdk =
        libs.versions.compileSdk
            .get()
            .toInt()
    // A5: DSP-Header der Firmware per NDK (nur Nachrechnen von Aufnahmen); Standard-NDK von AGP 9.4
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "de.timo.radarsensor"
        minSdk =
            libs.versions.minSdk
                .get()
                .toInt()
        targetSdk =
            libs.versions.targetSdk
                .get()
                .toInt()
        versionCode = 1
        versionName = "0.1.0"
        // Vorbelegung der Brücke (host/esp_tcp_bridge.py); leer = in der App eintragen
        val bridge = localProps.getProperty("radar.bridge", "").replace("\"", "")
        buildConfigField("String", "DEFAULT_BRIDGE", "\"$bridge\"")
        ndk {
            abiFilters += "arm64-v8a" // nur das eigene Gerät
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        buildConfig = true
        compose = true
    }

    testOptions {
        unitTests {
            isReturnDefaultValues = true
        }
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

kotlin {
    jvmToolchain(17)
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.activity.compose)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.usb.serial)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.compose.material3)
    debugImplementation(libs.androidx.compose.ui.tooling)

    testImplementation(libs.junit)
}
