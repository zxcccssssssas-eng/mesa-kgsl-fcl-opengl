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
        minSdk = 29
        targetSdk = 34
        versionCode = 18
        versionName = "1.5.2"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                // Mesa 26 (lfdevs adreno-main) installs libEGL_mesa.so, not libOSMesa.so.
                // scripts/build-mesa-android.sh renames it to libEGL_mesa_core.so
                // so this shim can take the libEGL_mesa.so slot FCL dlopens.
                val realEgl = file("src/main/jniLibs/arm64-v8a/libEGL_mesa_core.so")
                arguments += listOf(
                    "-DANDROID_STL=none",
                    "-DANDROID_PLATFORM=android-29",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DFCL_STUB_MESA_EGL=${if (realEgl.exists()) "OFF" else "ON"}",
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
            manifestPlaceholders["des"] = "Freedreno KGSL (Mesa EGL, Adreno)"

            // FCL renderer string: Name:libGL.so:libEGL.so
            // Mesa 26 dropped OSMesa. Use Mesa Android EGL + GLES libs and
            // POJAV_RENDERER=opengles3_desktopgl so FCL binds EGL_OPENGL_API
            // (desktop GL) via the GL bridge — not OSMBridge.
            manifestPlaceholders["renderer"] = "FreedrenoKGSL:libGLESv2_mesa.so:/libEGL_mesa.so"

            // boatEnv / pojavEnv are KEY=val:KEY2=val2
            // DLOPEN=liba.so,libb.so loads extra native libs from this plugin APK.
            // Do NOT force MESA_GL_VERSION_OVERRIDE=4.6: NeoForge early display
            // probes GLFW core profiles and fails when the override lies.
            // pojavEnv MUST DLOPEN libEGL_mesa.so (was missing in 1.0.0).
            val dlopenLibs =
                "libfreedreno_kgsl_init.so,libgallium_dri.so,libEGL_mesa.so,libGLESv2_mesa.so"
            manifestPlaceholders["boatEnv"] = mutableMapOf(
                "LIBGL_STRING" to "opengles3_desktopgl",
                "LIBGL_NAME" to "libGLESv2_mesa.so",
                "LIBGL_ES" to "3",
                "GALLIUM_DRIVER" to "freedreno",
                "MESA_LOADER_DRIVER_OVERRIDE" to "kgsl",
                "FD_FORCE_KGSL" to "1",
                // Mesa 26.3 gates the KGSL dma-buf caps behind this option
                // (fd_kgsl_dmabuf_enabled()); without it the screen advertises
                // no DRM_PRIME_CAP_IMPORT and every AHB/window import fails.
                "FD_KGSL_ENABLE_DMABUF" to "1",
                "DLOPEN" to dlopenLibs,
            ).entries.joinToString(":") { "${it.key}=${it.value}" }

            manifestPlaceholders["pojavEnv"] = mutableMapOf(
                "POJAV_RENDERER" to "opengles3_desktopgl",
                "LIBGL_ES" to "3",
                "GALLIUM_DRIVER" to "freedreno",
                "MESA_LOADER_DRIVER_OVERRIDE" to "kgsl",
                "FD_FORCE_KGSL" to "1",
                // Mesa 26.3 gates the KGSL dma-buf caps behind this option
                // (fd_kgsl_dmabuf_enabled()); without it the screen advertises
                // no DRM_PRIME_CAP_IMPORT and every AHB/window import fails.
                "FD_KGSL_ENABLE_DMABUF" to "1",
                "DLOPEN" to dlopenLibs,
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
            // extractNativeLibs=true so FCL can dlopen real extracted files from the plugin APK;
            // keep 16KB ELF (max-page-size) + \$ORIGIN rpath for libgallium_dri.
            useLegacyPackaging = true
            keepDebugSymbols += "**/*.so"
        }
    }
}

dependencies {
}
