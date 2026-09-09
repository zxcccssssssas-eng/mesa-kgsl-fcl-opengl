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
4. Open FCL → version / renderer settings and select **Freedreno KGSL**.
5. Launch the game.

FCL finds plugins by scanning installed apps for:

| meta-data | this plugin |
|---|---|
| `fclPlugin` | `true` |
| `renderer` | `FreedrenoKGSL:libGLESv2_mesa.so:libEGL_mesa.so` |
| `des` | `Freedreno KGSL (Mesa Gallium, Adreno)` |
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

Architecture: **arm64-v8a only**.

## OpenGL version (honest)

Minecraft / F3 may show **OpenGL 4.6** because this plugin sets:

```text
MESA_GL_VERSION_OVERRIDE=4.6
MESA_GLSL_VERSION_OVERRIDE=460
```

That is the **advertised** desktop GL version Mesa is configured to report. It is **not** a guarantee that every OpenGL 4.6 feature is complete on your Adreno:

- Freedreno hardware support varies by generation (a6xx vs a7xx vs a8xx).
- Some 4.x features are missing, lowered, or buggy. Shader packs and Sodium/Iris can expose that.
- If a version is too new for the GPU, unset the override in FCL custom env, or try `4.5` / `4.4`.

This plugin is **native Gallium Freedreno over KGSL**, not GL4ES, Holy-GL4ES, MobileGlues, or LTW. Mesa 26 (lfdevs `adreno-main`) **removed OSMesa**, so the plugin talks to FCL the same way FCL's built-in Zink renderer does: Mesa EGL (`libEGL_mesa.so`) with `POJAV_RENDERER=opengles3_desktopgl` so FCL binds `EGL_OPENGL_API` (desktop OpenGL), not the old `custom_gallium` + `libOSMesa.so` path.

## Environment injected into FCL

Boat (`boatEnv`) and Pojav (`pojavEnv`) both pin KGSL Freedreno and Mesa 26's Android EGL/GLES libraries:

```text
GALLIUM_DRIVER=freedreno
MESA_LOADER_DRIVER_OVERRIDE=kgsl
FD_FORCE_KGSL=1
LIBGL_ES=3
MESA_GL_VERSION_OVERRIDE=4.6
MESA_GLSL_VERSION_OVERRIDE=460
mesa_glthread=true
DLOPEN=libfreedreno_kgsl_init.so,libgallium_dri.so,libEGL_mesa.so,libGLESv2_mesa.so
POJAV_RENDERER=opengles3_desktopgl   # pojav; FCL binds EGL_OPENGL_API
```

`libfreedreno_kgsl_init.so` only pins those env vars before Mesa creates a device. The renderer string is `FreedrenoKGSL:libGLESv2_mesa.so:libEGL_mesa.so`. FCL sets `POJAVEXEC_EGL` from the EGL library name (same pattern as built-in Zink's `libEGL_mesa.so`).

To try **Zink** (OpenGL on Vulkan) instead of native Freedreno, add FCL custom env `GALLIUM_DRIVER=zink` and import a Turnip driver (the AdrenoTools zip from this build, or any FCL driver plugin).

## Build

### CI (recommended)

Actions workflow `.github/workflows/build.yml`:

- `workflow_dispatch` is enabled. **Run workflow** with `build_mesa=true` (default) to compile Mesa with the NDK and produce a usable APK.
- Pull requests assemble a **stub** APK (plugin metadata + placeholder `libEGL_mesa.so` / `libGLESv2_mesa.so` / `libgallium_dri.so`) so Gradle stays green without a 1 hour Mesa compile. The stub is **not** a working renderer.

Dispatch inputs:

| input | default | meaning |
|---|---|---|
| `mesa_ref` | `adreno-main` | [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) branch |
| `build_mesa` | `true` | NDK Mesa + real `libEGL_mesa.so` |
| `build_vulkan` | `true` | Turnip ICD + AdrenoTools zip |

### Local

Need: Android SDK, NDK r27+, meson, ninja, pkg-config, python3-mako, git, zip.

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
