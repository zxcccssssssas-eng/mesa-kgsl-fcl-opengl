# FCL Freedreno KGSL

An **installable FoldCraftLauncher (FCL) renderer plugin** for Minecraft Java on **Qualcomm Adreno** phones and tablets.

Install the APK like a normal Android app. FoldCraftLauncher finds it automatically and lists **Freedreno KGSL** in its renderer menu. You do **not** unpack Mesa libraries by hand.

Current plugin version: **1.5.4**. Mesa inside the plugin is **26.3.0-devel**.

> This is an Android plugin, not a Linux container Mesa tarball. For Debian/Ubuntu/Fedora-style arm64 userspace, use [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container) instead.

The previous technical README (build flags, Mesa patches, internals) is saved as [`README.md.bak`](README.md.bak).

---

## Will this work on my device?

| You need | It will not work on |
|---|---|
| Qualcomm **Adreno 6xx / 7xx / 8xx** | Mali, PowerVR, Samsung Xclipse, desktop GPUs |
| Android **10+** (API 29+) | Older Android |
| **64-bit ARM** (`arm64-v8a`) | 32-bit devices |
| A device that exposes KGSL (`/dev/kgsl-3d0`) | Non-Android Linux / containers |
| A current [FoldCraftLauncher](https://github.com/FCL-Team/FoldCraftLauncher) | FCL builds that predate renderer plugins |

This driver reports the **real** OpenGL version your Adreno supports. It does **not** pretend to be desktop GL 4.6. That is safer for NeoForge, but some PC-only shader packs or mods may still fail.

It is **native Mesa Freedreno over KGSL**, not GL4ES, Holy-GL4ES, MobileGlues, or LTW.

---

## Install

You need two apps on the **same user profile**: FoldCraftLauncher, and this plugin.

### 1. Get the APK

There is no GitHub Release yet. Download a CI artifact:

1. Open this repo on GitHub → **Actions**
2. Open the workflow **Build FCL Freedreno KGSL plugin**
3. Use a successful run on `main`, or click **Run workflow** (leave Mesa build enabled) and wait — the native compile is long
4. Download the artifact **`fcl-freedreno-kgsl-apk`**
5. Use **`FCL-FreedrenoKGSL-arm64.apk`**

Skip `FCL-FreedrenoKGSL-stub.apk`. The stub only exists so pull-request CI can assemble an APK without compiling Mesa. It is **not** a working renderer.

### 2. Install it on the device

1. Copy the APK to the phone or tablet
2. Open it and install (allow **Install unknown apps** for your file manager if Android asks)
3. You should see **FCL Freedreno KGSL** in the app drawer. Opening it is optional; it only shows help and copy buttons for extra settings

### 3. Restart FoldCraftLauncher

**Force-stop FoldCraftLauncher, then open it again.** Do this after every install **and every plugin update**.

FCL remembers the plugin’s library path when it starts. Android gives the app a new path on update. If FCL stays running, it tries to load the old (deleted) files and the game fails with `UnsatisfiedLinkError` / `libGLESv2_mesa.so`.

Quick way with a computer:

```bash
adb shell am force-stop com.tungsten.fcl
```

Or use Android: Settings → Apps → FoldCraftLauncher → Force stop.

### 4. Pick the renderer and play

1. In FCL, open the instance → renderer / version settings
2. Select **Freedreno KGSL**
3. Launch the game

If the name never appears: the APK is not installed for the same Android user/work profile as FCL, or FCL is too old. Update FCL from [FCL-Team/FoldCraftLauncher](https://github.com/FCL-Team/FoldCraftLauncher).

Package id: `com.mio.plugin.renderer.freedreno.kgsl`

---

## Extra settings (optional)

You do not need these for a first launch. Defaults already use Freedreno/KGSL OpenGL.

The plugin app has buttons that **copy** the lines below. Copying does nothing by itself: paste them into FCL’s **custom environment**, then fully restart the game.

### Presentation (how frames reach the screen)

The game still renders OpenGL through Freedreno/KGSL. This only changes how the finished frame is shown.

| Paste into FCL custom environment | What it does |
|---|---|
| `FCL_SHIM_RENDERER=egl` | **Default.** Usually the right choice. |
| `FCL_SHIM_RENDERER=vulkan` | Alternative presentation path. Useful to compare if EGL looks wrong. Falls back to EGL if Vulkan cannot start. |

Unset or unrecognized values use EGL.

### Visual glitches on Adreno 8xx (including Adreno 840)

If chunks look cut open, the sky stripes through world geometry, or the image flickers, try **Zink** (OpenGL on top of the device’s Vulkan driver):

```text
FCL_SHIM_GALLIUM=zink
```

Leave this unset to stay on Freedreno GL.

**Caveat:** on a 1.21.1 NeoForge profile with a large mod list, Zink has crashed while resources load. Vendor GLES renderers (MobileGlues / ANGLE) can look correct on the same device. Try Zink if Freedreno GL is unusable; go back if it crashes.

### Create / Flywheel missing or flashing blocks

On Adreno 840, some Create machinery can vanish or flash with Flywheel. Plugin **1.5.4+** includes a workaround for Flywheel’s **indirect** backend.

In the instance file `config/flywheel-client.toml`, set:

```text
backend = "flywheel:indirect"
```

Then restart the game. If problems remain, `flywheel:instancing` is a working fallback. `flywheel:off` also restores blocks but turns Flywheel off.

### NeoForge: “Failed to find a valid GLFW profile”

1. Use plugin **1.1.0 or newer**
2. In the instance `config/fml.toml`, set `earlyWindowControl=false`
3. Force-stop FCL, relaunch, and select **Freedreno KGSL** again
4. If it still fails, grab `latest.log` from the FCL instance

### Advertise a higher OpenGL version (modpacks)

Only if a pack **requires** it. In FCL custom environment, for example:

```text
MESA_GL_VERSION_OVERRIDE=4.5
```

Expect missing features versus a real desktop GPU. Do not set this unless you need it; a fake 4.6 is a common NeoForge crash.

---

## Optional: Turnip Vulkan zip

The same workflow may also produce `turnip-freedreno-kgsl-adrenotools.zip`. **Most people can ignore it.**

Import it in FCL only if you switched Gallium to Zink (`GALLIUM_DRIVER=zink` / `FCL_SHIM_GALLIUM=zink`) **and** you want Mesa’s Turnip Vulkan driver instead of the phone’s system Vulkan driver.

1. In FCL, open **driver import** (AdrenoTools-compatible Vulkan driver)
2. Import the zip (`libraryName` is `libvulkan_freedreno.so`)
3. Keep the **Freedreno KGSL** renderer selected for OpenGL

---

## Troubleshooting

Always **force-stop FCL** after installing or updating this plugin, then try again.

| What you see | What to try |
|---|---|
| Renderer is missing from FCL | Confirm the APK is installed on the same user as FCL. Update FCL. Do not use the stub APK. |
| Game fails right after a plugin update | Force-stop FCL so it picks up the new library path. |
| `Failed to dynamically load library` / `libGLESv2_mesa.so` | Same as above, or you installed the stub APK. Need a full Mesa build (plugin **1.2.3+**). |
| `eglInitialize` failed / GLFW cannot create a window | Use plugin **1.2.4+**. Confirm the device is Adreno + KGSL. |
| `EGL_BAD_PARAMETER` / no desktop OpenGL | Use a current plugin (desktop GL on Android is patched in). |
| NeoForge GLFW profile error | `earlyWindowControl=false` in `config/fml.toml`; see above. |
| Cut-open chunks / sky stripes on Adreno 8xx | Try `FCL_SHIM_GALLIUM=zink`, or a vendor GLES renderer. |
| Create blocks missing or flashing | Plugin **1.5.4+** and `backend = "flywheel:indirect"` (see above). |

If you need logs from a computer:

```bash
adb logcat -s FCL EGLShim VulkanShim FCLProbe
```

---

## What FCL actually loads

FCL scans installed apps for plugin metadata. This one advertises:

| Field | Value |
|---|---|
| `fclPlugin` | `true` |
| Display name | `Freedreno KGSL (Mesa EGL, Adreno)` |
| Renderer | `FreedrenoKGSL:libGLESv2_mesa.so:libEGL_mesa.so` |

Boat and Pojav both pin KGSL Freedreno and Mesa’s Android EGL libraries. You do not need to set these yourself:

```text
GALLIUM_DRIVER=freedreno
MESA_LOADER_DRIVER_OVERRIDE=kgsl
FD_FORCE_KGSL=1
FD_KGSL_ENABLE_DMABUF=1
LIBGL_ES=3
DLOPEN=libfreedreno_kgsl_init.so,libgallium_dri.so,libEGL_mesa.so,libGLESv2_mesa.so
POJAV_RENDERER=opengles3_desktopgl
```

---

## For developers

Build, Mesa flags, patch notes, and architecture live in the original README: **[`README.md.bak`](README.md.bak)**.

**CI (recommended):** GitHub Actions → **Build FCL Freedreno KGSL plugin** → **Run workflow** with `build_mesa=true`. Pull requests assemble a **stub** APK on purpose.

**Local (needs Android SDK, NDK r27+, meson, ninja, pkg-config, python3-mako, git, zip):**

```bash
export NDK=/path/to/android-ndk-r27c
export MESA_REF=adreno-main
./scripts/build-mesa-android.sh
./gradlew :app:assembleRelease
./scripts/check-plugin-config.sh app/build/outputs/apk/release/*.apk
```

Shim tests: [`tests/README.md`](tests/README.md).

### Credits

- Mesa 3D — MIT, [lfdevs fork](https://github.com/lfdevs/mesa-for-android-container) / [upstream](https://gitlab.freedesktop.org/mesa/mesa)
- Plugin layout — [ShirosakiMio/FCLRendererPlugin](https://github.com/ShirosakiMio/FCLRendererPlugin)
- Android NDK Mesa flags — [Vera-Firefly/android-mesa-build](https://github.com/Vera-Firefly/android-mesa-build)
- FoldCraftLauncher — [FCL-Team/FoldCraftLauncher](https://github.com/FCL-Team/FoldCraftLauncher)
- Driver / Turnip packaging — [FCL-Team/FCLDriverPlugin](https://github.com/FCL-Team/FCLDriverPlugin), AdrenoTools zip conventions
- EGL presentation design — [utkarshdalal/GameNative](https://github.com/utkarshdalal/GameNative)
