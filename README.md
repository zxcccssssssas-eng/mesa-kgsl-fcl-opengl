# Mesa Freedreno/KGSL renderer plugin for FoldCraftLauncher

This repository builds an **installable FoldCraftLauncher (FCL) renderer plugin APK**, not a Linux container Mesa tarball.

Primary deliverable:

- `FCL-FreedrenoKGSL-arm64.apk` — Android app plugin that FCL discovers via `meta-data fclPlugin=true`

Secondary:

- `turnip-freedreno-kgsl-adrenotools.zip` — AdrenoTools-style zip (`meta.json` + `libvulkan_freedreno.so`, `libraryName` field) for FCL **driver** import (Turnip / Zink companion)

## Install in FoldCraftLauncher

1. Get the APK
   - GitHub → **Actions** → **Build FCL Freedreno KGSL plugin** → **Run workflow**
   - Wait for Mesa NDK + APK jobs (the native compile is long)
   - Download artifact `fcl-freedreno-kgsl-apk`
   - Use `FCL-FreedrenoKGSL-arm64.apk` (not `*-stub.apk`)
2. Install the APK on the phone/tablet (sideload; allow unknown sources).
3. **Force-stop / restart FoldCraftLauncher** so it rescans installed packages.
   This is required after every plugin *update* too: FCL caches each plugin's
   `nativeLibraryDir` + renderer string at process start, and Android gives the
   APK a new `/data/app/~~.../lib/arm64` path on update. Updating the plugin
   while FCL is running makes it dlopen the deleted old path
   (`UnsatisfiedLinkError ... libGLESv2_mesa.so(error = null)`).
4. Open FCL → version / renderer settings and select **Freedreno KGSL**.
5. Launch the game.

FCL finds plugins by scanning installed apps for:

| meta-data | this plugin |
|---|---|
| `fclPlugin` | `true` |
| `renderer` | `FreedrenoKGSL:libGLESv2_mesa.so:libEGL_mesa.so` |
| `des` | `Freedreno KGSL (Mesa EGL, Adreno)` |
| `boatEnv` / `pojavEnv` | KGSL + Mesa EGL env (see below) |

Package id: `com.mio.plugin.renderer.freedreno.kgsl`

If the renderer does not appear, the APK is not installed for the same user/profile as FCL, or FCL is older than the renderer-plugin mechanism. Update FCL from [FCL-Team/FoldCraftLauncher](https://github.com/FCL-Team/FoldCraftLauncher).

### Optional Turnip driver zip

If the workflow produced `turnip-freedreno-kgsl-adrenotools.zip`:

1. In FCL, open **driver import** (AdrenoTools-compatible Vulkan driver).
2. Import the zip. `libraryName` is `libvulkan_freedreno.so`.
3. Keep the **renderer** plugin selected for OpenGL. The zip is only needed if you switch Gallium to Zink (`GALLIUM_DRIVER=zink`) and want Turnip instead of the system Vulkan driver.

## Expected GPUs

| Works | Does not work |
|---|---|
| Qualcomm **Adreno 6xx / 7xx / 8xx** (including A840-class chips that lfdevs Mesa supports) | Mali, PowerVR, Xclipse, desktop GPUs |
| Android devices that expose **KGSL** (`/dev/kgsl-3d0`) | Non-Android Linux containers (use [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) for those) |

Architecture: **arm64-v8a only**. Mesa 26 Turnip needs Android API **29+** (the NDK compile uses `aarch64-linux-android29-clang`, matching Vera-Firefly). The plugin `minSdk` is 29.

## OpenGL version (honest)

This plugin **does not** force `MESA_GL_VERSION_OVERRIDE`. Mesa reports whatever Freedreno/KGSL actually exposes on your Adreno. That is safer for NeoForge / GLFW early display, which probes core profiles and fails hard if an override claims 4.6 while context creation cannot deliver it.

If you still want an advertised version for a modpack, set FCL custom env yourself (`MESA_GL_VERSION_OVERRIDE=4.5` etc.). Expect gaps vs desktop GL 4.x on mobile GPUs.

This plugin is **native Gallium Freedreno over KGSL**, not GL4ES / Holy-GL4ES / MobileGlues / LTW. Mesa 26 (lfdevs `adreno-main`) **removed OSMesa**, so the plugin talks to FCL the same way FCL's Mesa EGL path does: `libEGL_mesa.so` + `POJAV_RENDERER=opengles3_desktopgl` (desktop OpenGL via `EGL_OPENGL_API`).


### `libcutils.so` / `libhardware.so` not found (fixed in 1.2.3)

Symptom (FCL log + `logcat -s FCL`):

```text
GLFW: Failed to create window context!
java.lang.UnsatisfiedLinkError: Failed to dynamically load library:
  /data/app/~~.../com.mio.plugin.renderer.freedreno.kgsl-.../lib/arm64/libGLESv2_mesa.so(error = null)
E/FCL: DLOPEN: loading .../libgallium_dri.so
  (error = dlopen failed: library "libcutils.so" not found: needed by .../libgallium_dri.so in namespace clns-9)
```

Root cause: Mesa built with `-Dandroid-stub=true` links its stub DSOs as
`DT_NEEDED`. `libcutils.so` and `libhardware.so` are **private platform
libraries**, and Android app processes (the JVM classloader namespace `clns-N`
LWJGL dlopens the renderer from) can only resolve libraries listed in
`/system/etc/public.libraries.txt`. `liblog`/`libnativewindow`/`libsync` are
public, so they were fine; `libcutils`/`libhardware` are not, so the whole
dlopen chain failed before Mesa could even initialize.

Fix: `scripts/build-mesa-android.sh` now patches Mesa's
`src/android_stub/meson.build` so the `cutils` and `hardware` stubs are linked
**statically into** `libgallium_dri.so` / `libEGL_mesa.so` / `libvulkan_freedreno.so`
(no `DT_NEEDED` left). The public stubs stay shared and resolve to the real
system libraries. Mesa's `hw_get_module` stub now returns failure instead of
`0` with an unset `*module`, so `u_gralloc` cleanly falls back to its generic
gralloc implementation (RGB window buffers keep working; `lock_ycbcr`/YUV video
paths are unavailable in the fallback).

The build also hard-fails if any packaged DSO still has a `DT_NEEDED` on
`libcutils`/`libhardware`/`libutils`/`libbinder`/`libgui`, so this regression
cannot ship again silently.

### `eglInitialize_p() failed: 3001` (fixed in 1.2.4)

After the `libcutils`/`libhardware` fix the libraries load, but Mesa still
cannot create a display on Qualcomm Android devices:

```text
E/GLBridge: eglInitialize_p() failed: 3001        (EGL_NOT_INITIALIZED)
E/GLBridge: eglChooseConfig_p() failed: 3001
GLFW: Failed to create window context!
```

Root cause: Mesa 26's **Android EGL platform only probes DRM render nodes**
(`droid_open_device()` → `_eglDeviceDrm`). Qualcomm Android kernels expose the
GPU exclusively through KGSL (`/dev/kgsl-3d0`); the only DRM node present is the
display's `msm_drm` node, which SELinux does not let app processes open. The
wayland and surfaceless EGL platforms already have a KGSL fallback gated on
`MESA_LOADER_DRIVER_OVERRIDE=kgsl`, but the Android platform does not.

Fix: `scripts/build-mesa-android.sh` patches
`src/egl/drivers/dri2/platform_android.c` to add the same fallback — when
`droid_open_device()` finds nothing and `disp->Options.Kgsl` is set, it opens
`/dev/kgsl-3d0`, selects the `kgsl` DRI driver and creates the screen, so
`eglGetDisplay(EGL_DEFAULT_DISPLAY)` + Android window surfaces keep working.

### Desktop OpenGL API missing on Android (`EGL_BAD_PARAMETER`)

After the KGSL fallback the display initializes, but Mesa still refuses desktop
OpenGL on Android builds:

```text
CLIENT_APIS: OpenGL_ES          (no OpenGL)
eglBindAPI(EGL_OPENGL_API) -> EGL_FALSE, error 0x300C (EGL_BAD_PARAMETER)
eglChooseConfig(EGL_OPENGL_BIT) -> 0 configs
```

Root cause: `src/egl/main/eglcurrent.h` gates the API on
`#if HAVE_OPENGL && !DETECT_OS_ANDROID` — "OpenGL is not a valid/supported API
on Android". Container Mesa builds (lfdevs) get desktop GL because they are
compiled with a Linux toolchain (`DETECT_OS_ANDROID=0`); this NDK build targets
Android, so the guard applies even though Mesa was configured with
`-Dopengl=true` and `libGLESv2_mesa.so` exports the desktop GL entry points.

Fix: the build script patches `_eglIsApiValid()` to accept `EGL_OPENGL_API` on
Android too. `st_api_query_versions()` then reports the desktop GL versions the
Freedreno/KGSL screen supports, `disp->ClientAPIs` gains `EGL_OPENGL_BIT`, and
FCL's `POJAV_RENDERER=opengles3_desktopgl` path can bind desktop GL and create
a context (the requested `EGL_CONTEXT_CLIENT_VERSION=3` is only a minimum —
Mesa creates the highest version the driver supports).

### Renderer does not appear / old renderer after updating the plugin

FCL scans plugin packages once per process. After installing or updating this
APK, force-stop FCL (`am force-stop com.tungsten.fcl`) before launching a game.
Otherwise FCL keeps the previous plugin's `nativeLibraryDir` in memory and
dlopens a directory Android already deleted.

### NeoForge: "Failed to find a valid GLFW profile"

1. Install plugin **1.1.0+** (pojavEnv now DLOPENs `libEGL_mesa.so`; no forced GL 4.6).
2. In the instance `config/fml.toml`, set `earlyWindowControl=false` (NeoForged guidance for Early Lifecycle / GLFW).
3. Force-stop FCL, relaunch, pick **Freedreno KGSL** again.
4. If it still fails, paste the full FCL/latest.log (the earlier message had no log attached).

## Environment injected into FCL

Boat (`boatEnv`) and Pojav (`pojavEnv`) both pin KGSL Freedreno and Mesa 26's Android EGL/GLES libraries:

```text
GALLIUM_DRIVER=freedreno
MESA_LOADER_DRIVER_OVERRIDE=kgsl
FD_FORCE_KGSL=1
LIBGL_ES=3
DLOPEN=libfreedreno_kgsl_init.so,libgallium_dri.so,libEGL_mesa.so,libGLESv2_mesa.so
POJAV_RENDERER=opengles3_desktopgl   # pojav; FCL binds EGL_OPENGL_API
```

Default path is **pure Gallium Freedreno over KGSL** (not Zink). `libfreedreno_kgsl_init.so` pins KGSL before Mesa creates a device. The renderer string is `FreedrenoKGSL:libGLESv2_mesa.so:libEGL_mesa.so`. FCL sets `POJAVEXEC_EGL` from the EGL library name.

The packaged DSOs only depend on public Android libraries
(`liblog`, `libnativewindow`, `libsync`, `libz`, `libm`, `libdl`, `libc`) plus
each other via `DT_RUNPATH=$ORIGIN`; the private `libcutils`/`libhardware`
stubs are linked in statically (see the troubleshooting section above).

The Turnip AdrenoTools zip is an optional separate artifact for people who want Vulkan/Zink later; this plugin itself does not switch you to Zink.

## Build

### CI (recommended)

Actions workflow `.github/workflows/build.yml`:

- `workflow_dispatch` is enabled. **Run workflow** with `build_mesa=true` (default) to compile Mesa with the NDK and produce a usable APK.
- Pull requests assemble a **stub** APK (plugin metadata + placeholder `libEGL_mesa.so` / `libGLESv2_mesa.so` / `libgallium_dri.so`) so Gradle stays green without a 1 hour Mesa compile. The stub is **not** a working renderer.

Dispatch inputs:

| input | default | meaning |
|---|---|---|
| `mesa_ref` | `adreno-main` | [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) branch |
| `build_mesa` | `true` | NDK Mesa + real `libEGL_mesa.so` (set `false` to reuse last `mesa-jniLibs` artifact for a fast APK-only rebuild) |
| `build_vulkan` | `true` | Turnip ICD + AdrenoTools zip |

### Local

Need: Android SDK, NDK r27+, meson, ninja, pkg-config, python3-mako, git, zip.
The Mesa NDK clang target is API **29** (`SDK_VER=29`).

```bash
export NDK=/path/to/android-ndk-r27c
export MESA_REF=adreno-main
./scripts/build-mesa-android.sh          # writes app/src/main/jniLibs/arm64-v8a/libEGL_mesa.so …
./gradlew :app:assembleRelease           # APK under app/build/outputs/apk/release/
./scripts/check-plugin-config.sh app/build/outputs/apk/release/*.apk
```

libdrm (generic static, NDK) meson flags — current upstream **removed**
`freedreno` / `freedreno-kgsl`. The script probes `meson_options.txt` and only
passes those if present. On libdrm 2.4.134 that is:

```text
-Ddefault_library=static
-Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled
-Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dtegra=disabled
-Dvc4=disabled -Detnaviv=disabled
-Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled
-Dtests=false -Dinstall-test-programs=false -Dudev=false
```

KGSL is enabled in **Mesa**, not libdrm.

Mesa meson flags for **lfdevs `adreno-main` / Mesa 26** (NDK, not the Linux
container flags). The script probes `meson.options` and skips unknown names.
`-Dosmesa=true` is **not** passed: that option does not exist on this tree
(same error as GitHub Actions run 34319938380). Working `-D` set:

```text
-Dbuildtype=release
-Dplatforms=android
-Dplatform-sdk-version=33
-Dandroid-stub=true
-Dandroid-strict=false
-Dandroid-libbacktrace=disabled
-Dandroid-libperfetto=disabled
-Dxlib-lease=disabled
-Degl=enabled
-Degl-native-platform=android
-Dgles2=enabled
-Dgles1=disabled
-Dopengl=true
-Dgbm=disabled
-Dglx=disabled
-Dllvm=disabled
-Dglvnd=disabled
-Dlibunwind=disabled
-Dmicrosoft-clc=disabled
-Dvalgrind=disabled
-Dintel-rt=disabled
-Dlmsensors=disabled
-Ddisplay-info=disabled
-Dgallium-va=disabled
-Dxmlconfig=disabled
-Dexpat=disabled
-Dgallium-drivers=zink,freedreno
-Dfreedreno-kmds=kgsl
-Dvulkan-drivers=freedreno
-Dtools=
-Degl-lib-suffix=_mesa
-Dgles-lib-suffix=_mesa
-Dunversion-libgallium=true
-Dallow-fallback-for=libdrm
-Dbuild-tests=false
-Dgallium-rusticl=false
```

That is [android-mesa-build](https://github.com/Vera-Firefly/android-mesa-build) (NDK android-stub, sdk 33, gallium zink+freedreno, kgsl) plus Mesa 26 Android EGL/GLES instead of OSMesa, lfdevs `-Dfreedreno-kmds=kgsl`, `_mesa` library suffixes (FCL Zink), and Turnip for the zip. `egl` / `gles2` are **feature** options (`enabled` / `disabled`), not booleans.

If meson still reports `Unknown option`, the script drops that `-D` and retries.

Libraries this Mesa generation installs (and the plugin packages):

| DSO | role |
|---|---|
| `libEGL_mesa.so` | Mesa EGL (`egl-lib-suffix=_mesa`) |
| `libGLESv2_mesa.so` | GLES2 entry library (`gles-lib-suffix=_mesa`) |
| `libgallium_dri.so` | Gallium dri megadriver (Android unversions this) |
| `libvulkan_freedreno.so` | Turnip (optional, AdrenoTools zip) |

## Layout

```text
app/                         FCLRendererPlugin-style Android Gradle project
  src/main/cpp/              KGSL env constructor + optional EGL/GLES stubs
  src/main/jniLibs/arm64-v8a/  real Mesa .so from scripts/ (CI)
scripts/build-mesa-android.sh  libdrm + Mesa NDK cross compile
.github/workflows/build.yml    Mesa + release APK + AdrenoTools zip
```

## Why this is not the lfdevs tarball

[lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) builds Mesa for **Debian/Ubuntu/Fedora/… arm64 userspace** (`-Dplatforms=x11,wayland -Dglvnd=enabled -Dglx=dri`). FCL cannot dlopen those glibc binaries.

FCL loads `.so` files from a plugin APK’s `nativeLibraryDir` using the **Boat/Pojav** class loaders. The working format is the FCL renderer plugin APK ([FCLRendererPlugin](https://github.com/ShirosakiMio/FCLRendererPlugin)), with Mesa built by the **Android NDK** like [android-mesa-build](https://github.com/Vera-Firefly/android-mesa-build).

## Third party

- Mesa 3D — MIT, [lfdevs fork](https://github.com/lfdevs/mesa-for-android-container) / [upstream](https://gitlab.freedesktop.org/mesa/mesa)
- Plugin metadata layout — [ShirosakiMio/FCLRendererPlugin](https://github.com/ShirosakiMio/FCLRendererPlugin)
- Android NDK Mesa flags — [Vera-Firefly/android-mesa-build](https://github.com/Vera-Firefly/android-mesa-build) (OSMesa era) + Mesa `docs/android.rst`
- FCL EGL desktop-GL renderer ABI — built-in Zink (`libEGL_mesa.so`, `opengles3_desktopgl*`) in [FCL-Team/FoldCraftLauncher](https://github.com/FCL-Team/FoldCraftLauncher)
- FoldCraftLauncher plugin scan — [FCL-Team/FoldCraftLauncher](https://github.com/FCL-Team/FoldCraftLauncher)
- Driver plugin / Turnip APK format — [FCL-Team/FCLDriverPlugin](https://github.com/FCL-Team/FCLDriverPlugin)
- AdrenoTools zip — `schemaVersion` / `libraryName` as used by K11MCH1 / whitebelyash Turnip packages
