plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// The versionCode comes from versionName and stays below 1000:
//   x.y.z-preview.N  ->  N        (the 0.1.0 previews)
//   1.0.z            ->  100 + z  (z = 0..199; the versions before the public release)
//   1.1.z            ->  300 + z  (z = 0..699; the public versions from 1.1.0)
// Flutter's split APKs add 1000 * ABI number, and every released in-app
// updater (0.1.0-preview.5 on) installs only a strictly higher code in the
// same ABI thousand, so a code of 1000 or more (e.g. 10000 for 1.0.0) would
// never install over a preview, and a hand-kept code that was not raised made
// every newer APK "not_newer". tools/build-android.ps1 derives the same code
// (android-workspace.ps1), checks that it exceeds every earlier build and that
// each APK carries exactly 1000 * ABI + code. A 1.2.0 needs a new rule here first.
fun androidVersionCode(name: String): Int {
    Regex("""^\d+\.\d+\.\d+-preview\.([1-9]\d{0,2})$""").matchEntire(name)?.let { return it.groupValues[1].toInt() }
    Regex("""^1\.0\.(0|[1-9]\d{0,2})$""").matchEntire(name)?.let { match ->
        val patch = match.groupValues[1].toInt()
        if (patch <= 199) return 100 + patch
    }
    Regex("""^1\.1\.(0|[1-9]\d{0,2})$""").matchEntire(name)?.let { match ->
        val patch = match.groupValues[1].toInt()
        if (patch <= 699) return 300 + patch
    }
    throw GradleException("Android versionName must be x.y.z-preview.N (N = 1..999), 1.0.z (z = 0..199, versionCode 100 + z) or 1.1.z (z = 0..699, versionCode 300 + z): $name")
}

android {
    namespace = "me.totb.multitap_client"
    compileSdk = flutter.compileSdkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    signingConfigs {
        create("preview") {
            val previewKey = System.getenv("MULTITAP_ANDROID_KEYSTORE")
            if (previewKey != null) {
                storeFile = file(previewKey)
                storePassword = System.getenv("MULTITAP_ANDROID_STOREPASS")
                keyAlias = System.getenv("MULTITAP_ANDROID_KEY_ALIAS") ?: "multitap-preview"
                keyPassword = System.getenv("MULTITAP_ANDROID_STOREPASS")
            }
        }
    }

    defaultConfig {
        applicationId = "me.totb.multitap_client"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = 26
        targetSdk = flutter.targetSdkVersion
        // When using split APKs, 1000 * ABI_VERSION is added automatically by Flutter.
        // (https://developer.android.com/studio/build/configure-apk-splits#configure-APK-versions)
        versionName = "1.1.5"
        versionCode = androidVersionCode(versionName!!)
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("preview")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}

dependencies {
    // JVM tests of the pure udp-media-v1 port (src/test); nothing ships in the APK.
    testImplementation("junit:junit:4.12")
}
