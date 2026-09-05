// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

import de.undercouch.gradle.tasks.download.Download
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.util.Locale

plugins {
    id("com.android.application")
    id("de.undercouch.download") version "5.7.0"
    id("kotlin-parcelize")
    kotlin("plugin.serialization") version "2.4.10"
    id("androidx.navigation.safeargs.kotlin")
}

/**
 * Use the number of seconds/10 since Jan 1 2016 as the versionCode.
 * This lets us upload a new build at most every 10 seconds for the
 * next 680 years.
 */
val autoVersion = (((System.currentTimeMillis() / 1000) - 1451606400) / 10).toInt()
val abiFilter = listOf("arm64-v8a")

val downloadedJniLibsDir = layout.buildDirectory.dir("downloadedJniLibs")

android {
    namespace = "org.citra.citra_emu"

    compileSdk = 36
    ndkVersion = "29.0.14206865"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    androidResources {
        generateLocaleConfig = true
    }

    packaging {
        // Modern packaging options for native libraries
        jniLibs {
            useLegacyPackaging = true
        }
    }

    buildFeatures {
        viewBinding = true
        buildConfig = true
    }

    lint {
        // Run lint but do not abort on error
        abortOnError = false
    }

    defaultConfig {
        // The application ID refers to Lime3DS to allow for
        // the Play Store listing, which was originally set up for Lime3DS, to still be used.
        // applicationId = "org.azahar_emu.azahar"
        applicationId = "io.github.lime3ds.android"

        minSdk = 35 
        targetSdk = 35
        versionCode = autoVersion
        versionName = getGitVersion()

        ndk {
            //noinspection ChromeOsAbiSupport
            abiFilters += abiFilter
        }

        externalNativeBuild {
            cmake {
                arguments(
                    "-DENABLE_QT=0", // Don't use QT
                    "-DENABLE_SDL2=0", // Don't use SDL
                    "-DCMAKE_C_FLAGS=-O3 -march=armv8.2-a+crypto+dotprod+fp16+rcpc",
                    "-DANDROID_ARM_NEON=true", // cryptopp requires Neon to work
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON", // Support Android 15 16KiB page
                    // sizes
                    "-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja",
                    "-DENABLE_GDBSTUB=OFF" // Disable GDB stub
                )
            }
        }

        buildConfigField("String", "GIT_VERSION", "\"${getGitVersion()}\"")
        // ^ Has no suffix, unlike VERSION_NAME
        buildConfigField("String", "GIT_HASH", "\"${getGitHash()}\"")
        buildConfigField("String", "BRANCH", "\"${getBranch()}\"")
    }

    val keystoreFile = System.getenv("ANDROID_KEYSTORE_FILE")
    if (keystoreFile != null) {
        signingConfigs {
            create("release") {
                storeFile = file(keystoreFile)
                storePassword = System.getenv("ANDROID_KEYSTORE_PASS")
                keyAlias = System.getenv("ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("ANDROID_KEYSTORE_PASS")
            }
        }
    }

    // Define build types, which are orthogonal to product flavors.
    buildTypes {
        // Signed by release key, allowing for upload to Play Store.
        release {
            signingConfig = if (keystoreFile != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }

        // builds a release build that doesn't need signing
        // Attaches 'debug' suffix to version and package name, allowing installation alongside the release build.
        register("relWithDebInfo") {
            initWith(getByName("release"))
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
            signingConfig = signingConfigs.getByName("debug")
            isShrinkResources = true
            isDebuggable = true
            isJniDebuggable = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            isDefault = true
        }

        // Same as above, but with isDebuggable disabled.
        register("relWithDebInfoLite") {
            initWith(getByName("relWithDebInfo"))
            signingConfig = signingConfigs.getByName("debug")
            isDebuggable = false
            installation {
                enableBaselineProfile = false // Disabled by default when isDebuggable is true
            }
            lint {
                checkReleaseBuilds = false
            }
        }

        // Signed by debug key disallowing distribution on Play Store.
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
            isDebuggable = true
            isJniDebuggable = true
        }
    }

    flavorDimensions.add("version")

    productFlavors {
        register("vanilla") {
            isDefault = true
            dimension = "version"
            versionNameSuffix = "-vanilla"
        }
        register("googlePlay") {
            dimension = "version"
            versionNameSuffix = "-googleplay"
        }
    }

    externalNativeBuild {
        cmake {
            version = "3.25.0+"
            path = file("../../../CMakeLists.txt")
        }
    }

    sourceSets {
        named("main") {
            // Set up path for downloaded native libraries
            jniLibs.srcDir(downloadedJniLibsDir)
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_21)
    }
}

dependencies {
    implementation("androidx.activity:activity-ktx:1.13.0")
    implementation("androidx.appcompat:appcompat:1.8.0")
    implementation("androidx.core:core-splashscreen:1.2.0")
    implementation("androidx.documentfile:documentfile:1.1.0")
    implementation("androidx.fragment:fragment-ktx:1.9.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.11.0")
    implementation("androidx.navigation:navigation-fragment-ktx:2.10.0")
    implementation("androidx.navigation:navigation-ui-ktx:2.10.0")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
    implementation("androidx.slidingpanelayout:slidingpanelayout:1.2.0")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.2.0")
    implementation("androidx.work:work-runtime:2.9.1")
    implementation("com.google.android.material:material:1.12.0")
    implementation("info.debatty:java-string-similarity:2.0.0")
    implementation("io.coil-kt:coil:2.7.0")
    implementation("org.ini4j:ini4j:0.5.4")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.11.0")
}

// Download Vulkan Validation Layers from the KhronosGroup GitHub.
val downloadVulkanValidationLayers = tasks.register<Download>("downloadVulkanValidationLayers") {
    src(
        "https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.357.0/android-binaries-1.4.357.0.zip"
    )
    dest(layout.buildDirectory.file("tmp/Vulkan-ValidationLayers.zip"))
    onlyIfModified(true)
}

// Extract Vulkan Validation Layers into the downloaded native libraries directory.
val unzipVulkanValidationLayers = tasks.register<Copy>("unzipVulkanValidationLayers") {
    dependsOn(downloadVulkanValidationLayers)
    from(zipTree(downloadVulkanValidationLayers.get().dest)) {
        eachFile {
            relativePath = RelativePath(true, *relativePath.segments.drop(1).toTypedArray())
        }
        includeEmptyDirs = false
    }
    into(downloadedJniLibsDir)
}

tasks.named("preBuild") {
    dependsOn(unzipVulkanValidationLayers)
}

fun getGitVersion(): String {
    var versionName = "0.0"

    try {
        versionName = ProcessBuilder("git", "describe", "--always", "--long")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start().inputStream.bufferedReader().use { it.readText() }
            .trim()
            .replace(Regex("(-0)?-[^-]+$"), "")
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy version number")
    }

    if (System.getenv("GITHUB_ACTIONS") != null) {
        val gitTag = System.getenv("GIT_TAG_NAME")
        versionName = gitTag ?: versionName
    }

    return versionName
}

fun getGitHash(): String =
    runGitCommand(ProcessBuilder("git", "rev-parse", "--short", "HEAD")) ?: "dummy-hash"

fun getBranch(): String =
    runGitCommand(ProcessBuilder("git", "rev-parse", "--abbrev-ref", "HEAD")) ?: "dummy-branch"

fun runGitCommand(command: ProcessBuilder): String? {
    try {
        command.directory(project.rootDir)
        val process = command.start()
        val inputStream = process.inputStream
        val errorStream = process.errorStream
        process.waitFor()

        return if (process.exitValue() == 0) {
            inputStream.bufferedReader()
                .use { it.readText().trim() }
        } else {
            val errorMessage = errorStream.bufferedReader().use { it.readText().trim() }
            logger.error("Error running git command: $errorMessage")
            return null
        }
    } catch (e: Exception) {
        logger.error("$e: Cannot find git")
        return null
    }
}

// Rewritten using modern AndroidComponents API compatible with AGP 9.0+
androidComponents.onVariants { variant ->
    val capitalizedName = variant.name.replaceFirstChar { 
        if (it.isLowerCase()) it.titlecase(Locale.US) else it.toString() 
    }

    val copyBundleTask = tasks.register("copyBundle$capitalizedName") {
        doLast {
            project.copy {
                from(layout.buildDirectory.dir("outputs/bundle/${variant.name}"))
                include("*.aab")
                into(layout.buildDirectory.dir("bundle"))
            }
        }
    }
    tasks.matching { it.name == "bundle$capitalizedName" }.configureEach {
        finalizedBy(copyBundleTask)
    }

    val copyApkTask = tasks.register("copyApk$capitalizedName") {                                                       doLast {
	    project.copy {
                from(layout.buildDirectory.dir("outputs/apk/${variant.name}"))
		include("*.apk")
		into(layout.buildDirectory.dir("bundle"))
	    }
        }
    }
    tasks.matching { it.name == "assemble$capitalizedName" }.configureEach {
	finalizedBy(copyApkTask)
    }
}

