plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.jetbrains.kotlin.android)
}

android {
    namespace = "com.mio.plugin.renderer"
    compileSdk = 34
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.mio.plugin.renderer"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                val realMesa = file("src/main/jniLibs/arm64-v8a/libOSMesa.so")
                arguments += listOf(
                    "-DANDROID_STL=none",
                    "-DANDROID_PLATFORM=android-26",
                    "-DFCL_STUB_OSMESA=${if (realMesa.exists()) "OFF" else "ON"}",
                )
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
        configureEach {
            // App name shown in the Android launcher / package manager.
            resValue("string", "app_name", "FCL Freedreno KGSL")
            // Final applicationId = com.mio.plugin.renderer.freedreno.kgsl
            applicationIdSuffix = ".freedreno.kgsl"

            // Display name inside FoldCraftLauncher's renderer list.
            manifestPlaceholders["des"] = "Freedreno KGSL (Mesa Gallium, Adreno)"

            // FCL renderer string: Name:libGL.so:libEGL.so
            // Matches the proven FCL Mesa plugin ABI (OSMesa + OSMBridge).
            manifestPlaceholders["renderer"] = "FreedrenoKGSL:libOSMBridge.so:libEGL.so"

            // boatEnv / pojavEnv are KEY=val:KEY2=val2
            // DLOPEN=liba.so,libb.so loads extra native libs from this plugin APK.
            manifestPlaceholders["boatEnv"] = mutableMapOf(
                "LIBGL_STRING" to "custom_gallium",
                "LIBGL_NAME" to "libOSMesa.so",
                "LIB_MESA_NAME" to "libOSMesa.so",
                "MESA_LIBRARY" to "libOSMesa.so",
                "GALLIUM_DRIVER" to "freedreno",
                "MESA_LOADER_DRIVER_OVERRIDE" to "kgsl",
                "FD_FORCE_KGSL" to "1",
                "MESA_GL_VERSION_OVERRIDE" to "4.6",
                "MESA_GLSL_VERSION_OVERRIDE" to "460",
                "mesa_glthread" to "true",
                "DLOPEN" to "libOSMBridge.so,libOSMesa.so",
            ).entries.joinToString(":") { "${it.key}=${it.value}" }

            manifestPlaceholders["pojavEnv"] = mutableMapOf(
                "POJAV_RENDERER" to "custom_gallium",
                "LIB_MESA_NAME" to "libOSMBridge.so",
                "MESA_LIBRARY" to "libOSMesa.so",
                "GALLIUM_DRIVER" to "freedreno",
                "MESA_LOADER_DRIVER_OVERRIDE" to "kgsl",
                "FD_FORCE_KGSL" to "1",
                "MESA_GL_VERSION_OVERRIDE" to "4.6",
                "MESA_GLSL_VERSION_OVERRIDE" to "460",
                "mesa_glthread" to "true",
                "DLOPEN" to "libOSMesa.so",
            ).entries.joinToString(":") { "${it.key}=${it.value}" }

            manifestPlaceholders["minMCVer"] = ""
            manifestPlaceholders["maxMCVer"] = ""
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
    kotlinOptions {
        jvmTarget = "1.8"
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    packaging {
        jniLibs {
            // Extract .so into nativeLibraryDir so FCL/Boat/Pojav can dlopen them.
            useLegacyPackaging = true
            keepDebugSymbols += "**/*.so"
        }
    }
}

dependencies {
}
