# Copyright 2026 Citra Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.

# To get usable stack traces
-dontobfuscate

# Keep the entire NativeLibrary class (and its companion/inner classes).
# libcitra-android.so's JNI_OnLoad reads static fields on this class directly
# via GetStaticObjectField - not just its declared `native` methods - so the
# native-methods-only rule above doesn't cover it. Without this, R8 can shrink
# away a field only native code ever touches, and JNI_OnLoad aborts with
# "JNI DETECTED ERROR IN APPLICATION: fid == null" on app launch.
-keep class org.citra.citra_emu.NativeLibrary { *; }
-keep class org.citra.citra_emu.NativeLibrary$* { *; }
-keep class org.citra.citra_emu.utils.** { *; }

# Keep everything referenced by JNI (native <-> Java/Kotlin bridge).                                                    # R8 can't see into libcitra-android.so, so anything it calls back into
# by name (FindClass/GetMethodID) needs to survive shrinking explicitly.
# includedescriptorclasses also keeps the parameter/return types used in
# native method signatures, since those get passed across the JNI boundary too.
-keepclasseswithmembers,includedescriptorclasses class * {
    native <methods>;
}

# Keep WorkManager and Room's internal classes. WorkManager builds its
# WorkDatabase (a Room database) reflectively via androidx.startup at app
# launch, before any app code runs - R8 can't see that path statically,
# so without this it can strip/rename something Room's generated database
# implementation needs, crashing on launch with
# "Failed to create an instance of class androidx.work.impl.WorkDatabase".
-keep class androidx.work.** { *; }
-keep class * extends androidx.room.RoomDatabase
-dontwarn androidx.work.**

# Prevents crashing when using Wini
-keep class org.ini4j.spi.IniParser
-keep class org.ini4j.spi.IniBuilder
-keep class org.ini4j.spi.IniFormatter

# Suppress warnings for R8
-dontwarn org.bouncycastle.jsse.BCSSLParameters
-dontwarn org.bouncycastle.jsse.BCSSLSocket
-dontwarn org.bouncycastle.jsse.provider.BouncyCastleJsseProvider
-dontwarn org.conscrypt.Conscrypt$Version
-dontwarn org.conscrypt.Conscrypt
-dontwarn org.conscrypt.ConscryptHostnameVerifier
-dontwarn org.openjsse.javax.net.ssl.SSLParameters
-dontwarn org.openjsse.javax.net.ssl.SSLSocket
-dontwarn org.openjsse.net.ssl.OpenJSSE
-dontwarn java.beans.Introspector
-dontwarn java.beans.VetoableChangeListener
-dontwarn java.beans.VetoableChangeSupport

# Don't include VERBOSE log calls in release builds
-assumenosideeffects class android.util.Log {
    public static int v(...);
}
