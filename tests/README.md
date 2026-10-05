# Shim validation

## SDL desktop OpenGL startup

Run the loader/state tests and a real Mesa context test on Linux:

```sh
bash scripts/test-gles-egl-exports.sh
bash scripts/test-sdl-desktop-context.sh
```

The second test requires the Mesa EGL runtime and software driver, development
headers for Vulkan, and a C compiler. On Ubuntu these are provided by
`gcc libegl1-mesa-dev libgl1-mesa-dri libvulkan-dev`. Set `HOST_EGL_LIBRARY` to an
absolute `libEGL.so.1` path if the compiler cannot locate it.

The test compiles both production shims and loads real Mesa through them. It
replays FCL's forced ES API and ES2 config request, then requires an actual
desktop OpenGL 4 context and a correct pixel readback through EGL/glX dispatch.
Separate processes verify that native ES clients, another renderer, and a
different SDL GL library still receive ES contexts. API version overrides are
disabled. A pbuffer replaces the Android window; this tests context creation
and GL dispatch, not Android presentation or Minecraft gameplay.

The correction is enabled only when `POJAV_RENDERER=opengles3_desktopgl` and
`SDL_OPENGL_LIBRARY` names this plugin's `libGLESv2_mesa.so`. FCL forces an ES
profile even for this desktop renderer, so the shim restores desktop API and
config selection. Context version attributes continue through to Mesa.

Vulkan loader isolation is a separate check: the presenter has no static
Vulkan imports/dependency and loads the system loader only when explicitly
selected with `FCL_SHIM_RENDERER=vulkan`. This is not a fix for every
`vkGetInstanceProcAddr mismatch`: FCL must route SDL and LWJGL to the same
loader handle, including when using its private Turnip namespace.

The native regression test checks native-fence type and flushing, completion
fallback when exporting a fence fails, invalid fence handling, framebuffer and
pixel-pack state preservation, and GL-to-Vulkan vertical orientation.

`scripts/test-gles-egl-exports.sh` builds the GLES shim with the host compiler
and checks that the EGL entry points SDL dlsyms are real dynamic symbols
forwarded to `libEGL_mesa.so`, while `glXGetProcAddress` still uses
`libEGL_mesa_core.so` (including the Flywheel wrappers). It also compiles the
Vulkan presenter without linking `libvulkan` and runs the native regression
test.

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
