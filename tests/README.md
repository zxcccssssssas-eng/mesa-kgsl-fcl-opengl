# Shim validation

The native regression test checks native-fence type and flushing, completion
fallback when exporting a fence fails, invalid fence handling, framebuffer and
pixel-pack state preservation, and GL-to-Vulkan vertical orientation.

The separate Android visual test renders four color quadrants for 600 frames,
with intentionally non-default framebuffer bindings, pixel-pack state, scissor,
and framebuffer sRGB enabled at the presentation boundary. It checks GL errors
and preserved state after each swap. Two delayed surface-size changes exercise
512×512 → 640×384 → 512×512 recreation. Its package is
`com.mio.plugin.shimtest`; it does not replace the FCL plugin.

Build with the real Mesa core and gallium libraries in `app/src/main/jniLibs/arm64-v8a`:

```sh
export NDK=/path/to/android-ndk-r27c
export ANDROID_SDK_ROOT=/path/to/sdk
export JAVA_HOME=/path/to/jdk-21
bash scripts/build-shim-visual-test.sh
adb push app/build/shim-visual-test/shim-state-test /data/local/tmp/fcl-shim-state-test
adb shell chmod 755 /data/local/tmp/fcl-shim-state-test
adb shell /data/local/tmp/fcl-shim-state-test
adb install -r app/build/shim-visual-test/test.apk
adb shell am start -n com.mio.plugin.shimtest/.VisualTest --es backend egl
# Wait for PASS, then restart the diagnostic process for Vulkan.
adb shell am force-stop com.mio.plugin.shimtest
adb shell am start -n com.mio.plugin.shimtest/.VisualTest --es backend vulkan
adb logcat -d -s EGLShim VulkanShim ShimVisualTest
adb uninstall com.mio.plugin.shimtest
```

Set `FCL_TEST_LIBS` to an alternative directory containing the Mesa libraries,
or `FCL_TEST_OUT` to an alternative build directory. SDK platform and build-tools
34 are required. The generated keystore is for diagnostics only.

## Device result, 2026-09-10

Lenovo TB323FU, Android API 36, Adreno 840. Reused the Mesa libraries from the
previously installed plugin: Mesa 26.1.0-devel, git `22dc45feab`. The updated
native code was built with Android NDK 27.2.12479018, API 29.

| Check | Result |
|---|---|
| Native synchronization/state regression test | PASS on device |
| EGL, 600 frames plus two resizes and swap interval | PASS; no GL errors or lost state |
| Vulkan, 600 frames plus two resizes and swap interval | PASS; no GL errors or lost state |
| Captured color quadrants | Matching orientation and colors in both modes |
| Real release APK, Gradle assembleRelease and release lint | PASS |
| Stub CMake build, including real-presenter compilation | PASS |
| Plugin configuration/APK library checks, APK signature verification | PASS |
| Install plugin version 1.4.0 / versionCode 16 | Success |

These are synthetic presentation tests, not a Minecraft/modpack gameplay test
or a performance benchmark. Vulkan's CPU readback/upload path is intentionally
conservative. Zero-copy Vulkan AHB imports, device-loss recovery, multi-threaded
EGL object lifetime, and exhaustive extension forwarding are outside this change.
