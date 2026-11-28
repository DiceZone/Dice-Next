plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "zone.dice.dicenext"
    compileSdk = 35
    ndkVersion = "27.2.12479018"
    defaultConfig {
        applicationId = "zone.dice.dicenext"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "3.0.0-android"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }
    buildTypes {
        getByName("debug") {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { viewBinding = false }
    packaging {
        jniLibs { useLegacyPackaging = true }
    }
    sourceSets.getByName("main") {
        jniLibs.srcDirs("src/main/jniLibs")
        assets.srcDirs("src/main/assets")
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.webkit:webkit:1.12.1")
    implementation("com.google.android.material:material:1.12.0")
}
