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
| `renderer` | `FreedrenoKGSL:libOSMBridge.so:libEGL.so` |
| `des` | `Freedreno KGSL (Mesa Gallium, Adreno)` |
| `boatEnv` / `pojavEnv` | KGSL + OSMesa env (see below) |

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

This plugin is **native Gallium Freedreno over KGSL**, not GL4ES, Holy-GL4ES, MobileGlues, or LTW. On paper that is the path that can actually implement desktop GL on Adreno instead of translating GLES.

## Environment injected into FCL

Boat (`boatEnv`) and Pojav (`pojavEnv`) both pin KGSL Freedreno and the proven FCL Mesa ABI (`custom_gallium` + OSMesa):

```text
GALLIUM_DRIVER=freedreno
MESA_LOADER_DRIVER_OVERRIDE=kgsl
FD_FORCE_KGSL=1
MESA_LIBRARY=libOSMesa.so
MESA_GL_VERSION_OVERRIDE=4.6
MESA_GLSL_VERSION_OVERRIDE=460
mesa_glthread=true
DLOPEN=libOSMBridge.so,libOSMesa.so   # boat
POJAV_RENDERER=custom_gallium         # pojav
LIB_MESA_NAME=libOSMBridge.so         # pojav
```

`libOSMBridge.so` is a small trampoline: it `dlopen`s `libOSMesa.so` and forwards OSMesa symbols, matching [Vera-Firefly/FCL-Mesa-Plugin](https://github.com/Vera-Firefly/FCL-Mesa-Plugin) / [ShirosakiMio/FCLRendererPlugin](https://github.com/ShirosakiMio/FCLRendererPlugin).

To try **Zink** (OpenGL on Vulkan) instead of native Freedreno, add FCL custom env `GALLIUM_DRIVER=zink` and import a Turnip driver (the AdrenoTools zip from this build, or any FCL driver plugin).

## Build

### CI (recommended)

Actions workflow `.github/workflows/build.yml`:

- `workflow_dispatch` is enabled. **Run workflow** with `build_mesa=true` (default) to compile Mesa with the NDK and produce a usable APK.
- Pull requests assemble a **stub** APK (plugin metadata + `libOSMBridge.so` + no-op `libOSMesa.so`) so Gradle stays green without a 1 hour Mesa compile. The stub is **not** a working renderer.

Dispatch inputs:

| input | default | meaning |
|---|---|---|
| `mesa_ref` | `adreno-main` | [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) branch |
| `build_mesa` | `true` | NDK Mesa + real `libOSMesa.so` |
| `build_vulkan` | `true` | Turnip ICD + AdrenoTools zip |

### Local

Need: Android SDK, NDK r27+, meson, ninja, pkg-config, python3-mako, git, zip.

```bash
export NDK=/path/to/android-ndk-r27c
export MESA_REF=adreno-main
./scripts/build-mesa-android.sh          # writes app/src/main/jniLibs/arm64-v8a/libOSMesa.so
./gradlew :app:assembleRelease           # APK under app/build/outputs/apk/release/
./scripts/check-plugin-config.sh app/build/outputs/apk/release/*.apk
```

Mesa meson flags (NDK, not the Linux container flags):

```text
-Dplatforms=android -Dandroid-stub=true -Dosmesa=true
-Dgallium-drivers=zink,freedreno -Dfreedreno-kmds=kgsl
-Dvulkan-drivers=freedreno -Degl=disabled -Dglx=disabled -Dllvm=disabled
```

That combination is [android-mesa-build](https://github.com/Vera-Firefly/android-mesa-build) plus lfdevs `-Dfreedreno-kmds=kgsl` and Turnip for the zip.

## Layout

```text
app/                         FCLRendererPlugin-style Android Gradle project
  src/main/cpp/              OSMBridge + optional OSMesa stub (CMake/NDK)
  src/main/jniLibs/arm64-v8a/  real libOSMesa.so from scripts/ (CI)
scripts/build-mesa-android.sh  libdrm (KGSL) + Mesa NDK cross compile
.github/workflows/build.yml    Mesa + release APK + AdrenoTools zip
```

## Why this is not the lfdevs tarball

[lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) builds Mesa for **Debian/Ubuntu/Fedora/… arm64 userspace** (`-Dplatforms=x11,wayland -Dglvnd=enabled -Dglx=dri`). FCL cannot dlopen those glibc binaries.

FCL loads `.so` files from a plugin APK’s `nativeLibraryDir` using the **Boat/Pojav** class loaders. The working format is the FCL renderer plugin APK ([FCLRendererPlugin](https://github.com/ShirosakiMio/FCLRendererPlugin)), with Mesa built by the **Android NDK** like [android-mesa-build](https://github.com/Vera-Firefly/android-mesa-build).

## Third party

- Mesa 3D — MIT, [lfdevs fork](https://github.com/lfdevs/mesa-for-android-container) / [upstream](https://gitlab.freedesktop.org/mesa/mesa)
- Plugin metadata layout — [ShirosakiMio/FCLRendererPlugin](https://github.com/ShirosakiMio/FCLRendererPlugin)
- OSMesa-on-FCL ABI — [Vera-Firefly/FCL-Mesa-Plugin](https://github.com/Vera-Firefly/FCL-Mesa-Plugin) / [android-mesa-build](https://github.com/Vera-Firefly/android-mesa-build)
- FoldCraftLauncher plugin scan — [FCL-Team/FoldCraftLauncher](https://github.com/FCL-Team/FoldCraftLauncher)
- Driver plugin / Turnip APK format — [FCL-Team/FCLDriverPlugin](https://github.com/FCL-Team/FCLDriverPlugin)
- AdrenoTools zip — `schemaVersion` / `libraryName` as used by K11MCH1 / whitebelyash Turnip packages
