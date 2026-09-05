// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Top-level build file where you can add configuration options common to all sub-projects/modules.
plugins {
    // AGP 9.x has built-in Kotlin support, so org.jetbrains.kotlin.android is no
    // longer applied here (and isn't compatible with AGP's new DSL - see
    // https://developer.android.com/build/migrate-to-built-in-kotlin).
    id("com.android.application") version "9.4.0" apply false
    id("com.android.library") version "9.4.0" apply false
    id("org.jetbrains.kotlin.plugin.serialization") version "2.4.10"
}

tasks.register("clean").configure {
    delete(rootProject.layout.buildDirectory)
}

buildscript {
    repositories {
        google()
    }
    dependencies {
        classpath("androidx.navigation:navigation-safe-args-gradle-plugin:2.10.0")
    }
}
