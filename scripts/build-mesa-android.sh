#!/usr/bin/env bash
# Cross-compile Mesa Gallium Freedreno (KGSL) + Android EGL/GLES for FCL.
#
# Mesa 25.x (Vera-Firefly/android-mesa-build, mesa-25.1.4) still has
# -Dosmesa=true. lfdevs/mesa-for-android-container adreno-main is Mesa 26
# and dropped the OSMesa frontend entirely (no osmesa / gallium-osmesa
# meson option, no src/gallium/targets/osmesa). FCL's custom_gallium path
# needed libOSMesa.so; this script therefore:
#   - probes meson.options like libdrm
#   - never passes unknown options (including -Dosmesa=true)
#   - builds Android EGL + GLES2 + desktop OpenGL + Freedreno KGSL
#   - packages libEGL_mesa.so / libGLESv2_mesa.so / libgallium_dri.so
#   - patches Mesa's android_stub so libcutils/libhardware are linked INTO the
#     Mesa DSOs instead of left as DT_NEEDED (they are private platform libs
#     and FCL's JVM classloader namespace cannot resolve them at runtime)
#   - patches Mesa's Android EGL platform to fall back to /dev/kgsl-3d0
#     (Qualcomm Android devices have no app-accessible DRM render node)
#   - patches Mesa's _eglIsApiValid() so EGL_OPENGL_API is accepted on Android
#     (Mesa gates desktop GL off for Android builds; FCL's
#     POJAV_RENDERER=opengles3_desktopgl needs it)
#   - patches Mesa's freedreno screen caps so the KGSL screen advertises
#     DRM_PRIME_CAP_IMPORT|EXPORT (drmGetCap() fails on a KGSL char device, so
#     caps.dmabuf stayed 0 and the DRI frontend disabled dma-buf import, which
#     broke EGL window surfaces and AHardwareBuffer/EGLImage imports)
#
# Proven flag sources for this Mesa generation:
#   - Mesa docs/android.rst (NDK: -Dplatforms=android -Dandroid-stub=true
#     -Dandroid-libbacktrace=disabled -Dfreedreno-kmds=kgsl)
#   - Vera-Firefly/android-mesa-build (NDK android-stub, platform-sdk 33,
#     gallium zink,freedreno, -Dfreedreno-kmds=kgsl,msm) minus -Dosmesa=true
#   - lfdevs adreno-main meson.options (egl/gles2 are feature options)
#   - FCL built-in Zink: libEGL_mesa.so + POJAV_RENDERER=opengles3_desktopgl*
#
# Usage:
#   ./scripts/build-mesa-android.sh
# Env:
#   MESA_SRC, DRM_SRC, NDK, SDK_VER, MESA_PREFIX, OUT_JNI, BUILD_VULKAN

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK_DIR:-$ROOT/.native-build}"
NDK="${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}}"
# NDK clang target. Must be >= 29: Mesa 26 Turnip's vk_android.c calls
# AHardwareBuffer_isSupported (API 29). Vera-Firefly/android-mesa-build uses 29.
# API 26 (plugin minSdk historically) fails with:
#   error: 'AHardwareBuffer_isSupported' is unavailable: introduced in Android 29
SDK_VER="${SDK_VER:-29}"
# 26.3.0-devel carries the KGSL dmabuf caps fix upstream; adreno-main was
# the 26.1.0-devel snapshot this plugin started from.
MESA_REF="${MESA_REF:-mesa-26.3.0-devel-20260824}"
MESA_REPO="${MESA_REPO:-https://github.com/lfdevs/mesa-for-android-container.git}"
DRM_REPO="${DRM_REPO:-https://gitlab.freedesktop.org/mesa/drm.git}"
MESA_PREFIX="${MESA_PREFIX:-$WORK/mesa-prefix}"
DRM_PREFIX="${DRM_PREFIX:-$WORK/drm-static}"
OUT_JNI="${OUT_JNI:-$ROOT/app/src/main/jniLibs/arm64-v8a}"
OUT_DIST="${OUT_DIST:-$ROOT/dist}"
BUILD_VULKAN="${BUILD_VULKAN:-1}"
JOBS="${JOBS:-$(nproc)}"

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

resolve_ndk() {
  if [[ -n "${NDK}" && -d "${NDK}" ]]; then
    return
  fi
  if [[ -n "${ANDROID_SDK_ROOT:-}" ]]; then
    local candidate
    candidate="$(ls -d "${ANDROID_SDK_ROOT}"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
    if [[ -n "${candidate}" ]]; then
      NDK="${candidate}"
      return
    fi
  fi
  die "set NDK or ANDROID_NDK_HOME to an Android NDK (r26+ recommended)"
}

ndk_prebuilt() {
  local host
  case "$(uname -m)" in
    aarch64|arm64) host="linux-aarch64" ;;
    *) host="linux-x86_64" ;;
  esac
  local p="${NDK}/toolchains/llvm/prebuilt/${host}"
  if [[ ! -d "${p}" ]]; then
    p="${NDK}/toolchains/llvm/prebuilt/linux-x86_64"
  fi
  [[ -d "${p}" ]] || die "NDK llvm toolchain not found under ${NDK}"
  printf '%s' "${p}"
}

write_cross_file() {
  local out="$1"
  local pkgconfig_libdir="$2"
  local prebuilt
  prebuilt="$(ndk_prebuilt)"
  local clang="${prebuilt}/bin/aarch64-linux-android${SDK_VER}-clang"
  local clangxx="${prebuilt}/bin/aarch64-linux-android${SDK_VER}-clang++"
  local ar="${prebuilt}/bin/llvm-ar"
  local strip_bin="${prebuilt}/bin/llvm-strip"
  [[ -x "${clang}" ]] || die "missing ${clang}"
  [[ -x "${clangxx}" ]] || die "missing ${clangxx}"

  local ccache_c="[]"
  local ccache_cxx="[]"
  if command -v ccache >/dev/null 2>&1; then
    ccache_c="['ccache', '${clang}']"
    ccache_cxx="['ccache', '${clangxx}']"
  else
    ccache_c="'${clang}'"
    ccache_cxx="'${clangxx}'"
  fi

  cat >"${out}" <<EOF
[binaries]
c = ${ccache_c}
cpp = ${ccache_cxx}
ar = '${ar}'
strip = '${strip_bin}'
c_ld = 'lld'
cpp_ld = 'lld'
pkgconfig = ['env', 'PKG_CONFIG_LIBDIR=${pkgconfig_libdir}', '/usr/bin/pkg-config']

[built-in options]
c_args = ['-O3', '-fPIC', '-DVK_USE_PLATFORM_ANDROID_KHR', '-fno-strict-aliasing']
cpp_args = ['-O3', '-fPIC', '-DVK_USE_PLATFORM_ANDROID_KHR', '-fno-exceptions', '-fno-unwind-tables', '-Wno-c++11-narrowing']
c_link_args = ['-fuse-ld=lld', '-Wl,-z,max-page-size=16384', '-Wl,-rpath,$$ORIGIN']
cpp_link_args = ['-fuse-ld=lld', '-static-libstdc++', '-Wl,-z,max-page-size=16384', '-Wl,-rpath,$$ORIGIN']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '${pkgconfig_libdir}'
EOF
}

clone_if_needed() {
  local dest="$1" url="$2" ref="$3"
  if [[ -d "${dest}/.git" ]]; then
    log "updating $(basename "${dest}") to ${ref}"
    git -C "${dest}" fetch --depth 1 origin "${ref}" || git -C "${dest}" fetch --depth 1 origin
    git -C "${dest}" checkout --force FETCH_HEAD || git -C "${dest}" checkout --force "${ref}" || true
    return
  fi
  log "cloning ${url} (${ref})"
  git clone --depth 1 --branch "${ref}" "${url}" "${dest}" \
    || git clone --depth 1 "${url}" "${dest}"
}

# True if meson_options.txt / meson.options declares option('name').
meson_has_option() {
  local src="$1" name="$2"
  python3 - "$src" "$name" <<'PY'
import pathlib, re, sys
src, name = pathlib.Path(sys.argv[1]), sys.argv[2]
text = ""
for n in ("meson_options.txt", "meson.options"):
    p = src / n
    if p.is_file():
        text += p.read_text(encoding="utf-8", errors="replace")
sys.exit(0 if re.search(r"option\(\s*['\"]" + re.escape(name) + r"['\"]", text) else 1)
PY
}

drm_has_option() { meson_has_option "$@"; }

# Current libdrm (2.4.134+) dropped the 'freedreno' / 'freedreno-kgsl' meson
# options and the libdrm_freedreno backend. KGSL lives in Mesa
# (-Dfreedreno-kmds=kgsl). Older libdrm still has those options; probe so
# both trees configure.
libdrm_meson_flags() {
  local src="$1"
  local -a flags=(-Ddefault_library=static)
  local name
  # Disable unused KMS backends. Feature options take enabled/disabled/auto.
  for name in intel radeon amdgpu nouveau vmwgfx omap exynos tegra vc4 etnaviv \
              cairo-tests man-pages valgrind; do
    if drm_has_option "${src}" "${name}"; then
      flags+=("-D${name}=disabled")
    fi
  done
  for name in tests install-test-programs udev; do
    if drm_has_option "${src}" "${name}"; then
      flags+=("-D${name}=false")
    fi
  done
  if drm_has_option "${src}" "freedreno"; then
    flags+=("-Dfreedreno=enabled")
  else
    log "libdrm has no 'freedreno' option (current upstream); KGSL is Mesa-side"
  fi
  if drm_has_option "${src}" "freedreno-kgsl"; then
    flags+=("-Dfreedreno-kgsl=true")
  else
    log "libdrm has no 'freedreno-kgsl' option; skipping"
  fi
  printf '%s\n' "${flags[@]}"
}

build_libdrm() {
  local src="${DRM_SRC:-$WORK/drm}"
  clone_if_needed "${src}" "${DRM_REPO}" "${DRM_REF:-main}"
  write_cross_file "${WORK}/android-drm.ini" "${DRM_PREFIX}/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="${DRM_PREFIX}/lib/pkgconfig"
  local drm_reconf=()
  if [[ -d "${src}/build-android" ]]; then
    drm_reconf=(--reconfigure)
  fi
  local -a drm_flags=()
  mapfile -t drm_flags < <(libdrm_meson_flags "${src}")
  log "libdrm meson flags: ${drm_flags[*]}"
  meson setup "${src}/build-android" "${src}" \
    --prefix="${DRM_PREFIX}" \
    --cross-file "${WORK}/android-drm.ini" \
    "${drm_flags[@]}" \
    "${drm_reconf[@]}"
  meson compile -C "${src}/build-android" -j "${JOBS}"
  meson install -C "${src}/build-android"
}

# Emit -D flags that exist in this Mesa tree. Never emits -Dosmesa=* unless
# that option is still declared (Mesa 25). lfdevs adreno-main (26) does not.
mesa_flags() {
  local src="$1"
  python3 - "$src" "${BUILD_VULKAN}" <<'PY'
import pathlib, re, sys

src = pathlib.Path(sys.argv[1])
want_vulkan = sys.argv[2] == "1"
text = ""
for n in ("meson.options", "meson_options.txt"):
    p = src / n
    if p.is_file():
        text += p.read_text(encoding="utf-8", errors="replace")
opts = set(re.findall(r"option\(\s*['\"]([^'\"]+)['\"]", text))

def emit(name, value):
    if name in opts:
        print(f"-D{name}={value}")
    else:
        print(f"skip unknown meson option {name}", file=sys.stderr)

# Android NDK + Freedreno KGSL. Values match Vera-Firefly/android-mesa-build
# and Mesa docs/android.rst, except OSMesa (removed in Mesa 26) and EGL
# which must be enabled for FCL (egl + gles2 are feature options).
emit("platforms", "android")
emit("platform-sdk-version", "33")
emit("android-stub", "true")
emit("android-strict", "false")
emit("android-libbacktrace", "disabled")
emit("android-libperfetto", "disabled")
emit("xlib-lease", "disabled")
emit("egl", "enabled")
emit("egl-native-platform", "android")
emit("gles2", "enabled")
emit("gles1", "disabled")
emit("opengl", "true")
emit("gbm", "disabled")
emit("glx", "disabled")
emit("llvm", "disabled")
emit("glvnd", "disabled")
emit("libunwind", "disabled")
emit("microsoft-clc", "disabled")
emit("valgrind", "disabled")
emit("intel-rt", "disabled")
emit("lmsensors", "disabled")
emit("display-info", "disabled")
emit("gallium-va", "disabled")
emit("xmlconfig", "disabled")
emit("expat", "disabled")
emit("gallium-drivers", "zink,freedreno")
emit("freedreno-kmds", "kgsl")
emit("vulkan-drivers", "freedreno" if want_vulkan else "")
emit("tools", "")
# Avoid clashing with Android system libEGL.so / libGLESv2.so (FCL Zink
# already loads libEGL_mesa.so). Android also unversions libgallium_dri.
emit("egl-lib-suffix", "_mesa")
emit("gles-lib-suffix", "_mesa")
emit("unversion-libgallium", "true")
emit("allow-fallback-for", "libdrm")
emit("build-tests", "false")
emit("gallium-rusticl", "false")

# Mesa 25 still has these; Mesa 26 does not. Only pass if declared.
if "osmesa" in opts:
    print("Mesa still declares osmesa; enabling alongside EGL", file=sys.stderr)
    emit("osmesa", "true")
else:
    print("Mesa has no 'osmesa' option (26+); using Android EGL/GLES", file=sys.stderr)
if "gallium-osmesa" in opts:
    emit("gallium-osmesa", "true")
PY
}

meson_setup_mesa() {
  local src="$1"
  shift
  local -a args=("$@")
  local logf="${WORK}/meson-setup-mesa.log"
  local attempt unknown a
  for attempt in 1 2 3 4 5; do
    log "meson setup Mesa (attempt ${attempt}): ${args[*]}"
    if meson setup "${src}/build-android" "${src}" "${args[@]}" >"${logf}" 2>&1; then
      cat "${logf}" >&2
      return 0
    fi
    cat "${logf}" >&2
    unknown="$(grep -oE 'Unknown option: "[^"]+"' "${logf}" | head -n1 | sed -E 's/Unknown option: "([^"]+)"/\1/' || true)"
    if [[ -z "${unknown}" ]]; then
      return 1
    fi
    log "dropping unknown meson option '${unknown}' and retrying"
    local -a next=()
    for a in "${args[@]}"; do
      if [[ "${a}" == "-D${unknown}="* ]]; then
        continue
      fi
      next+=("${a}")
    done
    args=("${next[@]}")
    rm -rf "${src}/build-android"
  done
  return 1
}

# Mesa's -Dandroid-stub=true builds stub DSOs named libcutils.so /
# libhardware.so / liblog.so / libnativewindow.so / libsync.so and links them
# as DT_NEEDED entries. libcutils and libhardware are PRIVATE platform
# libraries: Android app processes -- including the JVM classloader namespace
# ("clns-N") that LWJGL uses to dlopen the renderer -- cannot resolve them.
# The result on device is:
#   GLFW: Failed to create window context!
#   UnsatisfiedLinkError: Failed to dynamically load library:
#     .../libGLESv2_mesa.so(error = null)
#   dlopen failed: library "libcutils.so" not found: needed by
#     .../libgallium_dri.so in namespace clns-N
# Link the two private stubs statically into the Mesa DSOs instead (no
# DT_NEEDED left). The public stubs (liblog/libnativewindow/libsync) stay
# shared; the real system libraries satisfy those NEEDED entries because they
# are listed in /system/etc/public.libraries.txt.
#
# The upstream hardware stub returns 0 without writing *module, which makes
# u_gralloc's fallback dereference NULL. Return -1 so Mesa falls back cleanly
# (RGB window buffers still work; only lock_ycbcr / YUV paths are lost).
patch_mesa_android_stub() {
  local src="$1"
  python3 - "$src" <<'PY'
import pathlib, sys

src = pathlib.Path(sys.argv[1])
meson = src / "src" / "android_stub" / "meson.build"
hw = src / "src" / "android_stub" / "hardware_stub.cpp"

text = meson.read_text(encoding="utf-8")
marker = "# Private platform libs: link the stubs into the Mesa DSOs"

# Mesa 26.1/26.2 layout (libcutils stub still present)
old_v1 = """  stub_libs = []
  lib_names = ['cutils', 'hardware', 'log', 'nativewindow', 'sync']

  if with_libbacktrace
    lib_names += ['backtrace']
  endif

  foreach lib : lib_names
    stub_libs += shared_library(
      lib,
      files(lib + '_stub.cpp'),
      include_directories : inc_include,
      install : false,
    )
  endforeach"""

# Mesa 26.3 layout (libcutils stub removed upstream)
old_v2 = """  stub_libs = []
  lib_names = ['hardware', 'log', 'nativewindow', 'sync']

  if with_libbacktrace
    lib_names += ['backtrace']
  endif

  foreach lib : lib_names
    stub_libs += shared_library(
      lib,
      files(lib + '_stub.cpp'),
      include_directories : inc_include,
      install : false,
    )
  endforeach"""

new_v1 = """  stub_libs = []

  # Private platform libs: link the stubs into the Mesa DSOs (no DT_NEEDED).
  foreach lib : ['cutils', 'hardware']
    stub_libs += static_library(
      lib,
      files(lib + '_stub.cpp'),
      include_directories : inc_include,
      install : false,
    )
  endforeach

  # Public platform libs: keep shared stubs; the real system libraries
  # (in public.libraries.txt) satisfy those DT_NEEDED entries at runtime.
  shared_lib_names = ['log', 'nativewindow', 'sync']

  if with_libbacktrace
    shared_lib_names += ['backtrace']
  endif

  foreach lib : shared_lib_names
    stub_libs += shared_library(
      lib,
      files(lib + '_stub.cpp'),
      include_directories : inc_include,
      install : false,
    )
  endforeach"""

new_v2 = """  stub_libs = []

  # Private platform lib: link the stub into the Mesa DSOs (no DT_NEEDED).
  # Mesa 26.3 dropped the libcutils stub; only libhardware is private.
  foreach lib : ['hardware']
    stub_libs += static_library(
      lib,
      files(lib + '_stub.cpp'),
      include_directories : inc_include,
      install : false,
    )
  endforeach

  # Public platform libs: keep shared stubs; the real system libraries
  # (in public.libraries.txt) satisfy those DT_NEEDED entries at runtime.
  shared_lib_names = ['log', 'nativewindow', 'sync']

  if with_libbacktrace
    shared_lib_names += ['backtrace']
  endif

  foreach lib : shared_lib_names
    stub_libs += shared_library(
      lib,
      files(lib + '_stub.cpp'),
      include_directories : inc_include,
      install : false,
    )
  endforeach"""

if marker in text:
    print("android_stub meson.build already patched", file=sys.stderr)
elif old_v1 in text:
    meson.write_text(text.replace(old_v1, new_v1, 1), encoding="utf-8")
    print("patched src/android_stub/meson.build (static cutils/hardware)", file=sys.stderr)
elif old_v2 in text:
    meson.write_text(text.replace(old_v2, new_v2, 1), encoding="utf-8")
    print("patched src/android_stub/meson.build (static hardware; Mesa 26.3 layout)", file=sys.stderr)
else:
    sys.exit("error: cannot patch src/android_stub/meson.build: unexpected upstream layout")

hw_text = hw.read_text(encoding="utf-8")
old_hw = """int hw_get_module(const char *id, const struct hw_module_t **module)
{
   return 0;
}"""
new_hw = """int hw_get_module(const char *id, const struct hw_module_t **module)
{
   if (module)
      *module = NULL;
   /* Report failure so u_gralloc falls back instead of dereferencing NULL. */
   return -1;
}"""
if "*module = NULL" in hw_text:
    print("hardware_stub.cpp already patched", file=sys.stderr)
elif old_hw in hw_text:
    hw.write_text(hw_text.replace(old_hw, new_hw, 1), encoding="utf-8")
    print("patched src/android_stub/hardware_stub.cpp (hw_get_module returns -1)", file=sys.stderr)
else:
    sys.exit("error: cannot patch src/android_stub/hardware_stub.cpp: unexpected upstream layout")
PY
}

# Fail the build if a packaged DSO still has DT_NEEDED on a private platform
# library that Android app namespaces cannot resolve.
# Fail if the packaged Mesa DSOs do not carry the version of the source tree we
# just built (guards against stale caches / mismatched refs).
verify_mesa_version() {
  local src="${MESA_SRC:-$WORK/mesa}"
  local want=""
  [[ -f "${src}/VERSION" ]] && want="$(head -n1 "${src}/VERSION" | tr -d '[:space:]')"
  [[ -n "${want}" ]] || { log "WARN: no VERSION in ${src}; skipping Mesa version check"; return 0; }
  local found=0 so
  for so in "${OUT_JNI}"/libgallium*.so "${OUT_JNI}"/libEGL*.so "${OUT_JNI}"/libGLESv2*.so; do
    [[ -f "${so}" ]] || continue
    if grep -aqF "${want}" "${so}"; then
      found=1
      log "$(basename "${so}") carries Mesa ${want}"
    fi
  done
  if [[ "${found}" -eq 0 ]]; then
    die "packaged Mesa DSOs do not contain '${want}' - stale cache or wrong MESA_REF?"
  fi
}

verify_no_private_deps() {
  local readelf_bin=""
  if command -v llvm-readelf >/dev/null 2>&1; then
    readelf_bin="$(command -v llvm-readelf)"
  elif [[ -x "$(ndk_prebuilt)/bin/llvm-readelf" ]]; then
    readelf_bin="$(ndk_prebuilt)/bin/llvm-readelf"
  elif command -v readelf >/dev/null 2>&1; then
    readelf_bin="$(command -v readelf)"
  fi
  if [[ -z "${readelf_bin}" ]]; then
    log "WARN: no readelf found; skipping private-library dependency check"
    return 0
  fi
  local bad=0 so dep
  for so in "${OUT_JNI}"/*.so; do
    [[ -f "${so}" ]] || continue
    while IFS= read -r dep; do
      [[ -n "${dep}" ]] || continue
      case "${dep}" in
        libcutils.so|libhardware.so|libutils.so|libbinder.so|libgui.so)
          log "ERROR: $(basename "${so}") still has DT_NEEDED ${dep} (private platform lib; app namespace cannot load it)"
          bad=1
          ;;
      esac
    done < <("${readelf_bin}" -d "${so}" 2>/dev/null | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p')
  done
  [[ "${bad}" -eq 0 ]] || die "private platform library dependencies remain; FCL's app namespace cannot dlopen these DSOs"
  log "no private platform library dependencies in packaged DSOs"
}

# Mesa 26's Android EGL platform only probes DRM render nodes
# (droid_open_device -> _eglDeviceDrm). Qualcomm Android kernels expose the
# GPU exclusively through the KGSL kernel driver (/dev/kgsl-3d0) and the only
# DRM node (msm_drm display) is not accessible to app processes (SELinux), so
# eglInitialize() fails with EGL_NOT_INITIALIZED:
#   E/GLBridge: eglInitialize_p() failed: 3001
#   E/GLBridge: eglChooseConfig_p() failed: 3001
# The wayland and surfaceless platforms already have a KGSL fallback gated on
# MESA_LOADER_DRIVER_OVERRIDE=kgsl (disp->Options.Kgsl); add the same fallback
# to the Android platform so FCL's EGL window surfaces keep working.
patch_mesa_android_kgsl() {
  local src="$1"
  python3 - "$src" <<'PY'
import pathlib, sys

src = pathlib.Path(sys.argv[1])
path = src / "src" / "egl" / "drivers" / "dri2" / "platform_android.c"
text = path.read_text(encoding="utf-8")
marker = "Qualcomm Android devices expose the GPU only through the KGSL"
old = """   if (!force_pure_swrast)
      device_opened = droid_open_device(disp, disp->Options.ForceSoftware);

   if ((!device_opened && disp->Options.ForceSoftware) ||
       force_pure_swrast) {"""
new = """   if (!force_pure_swrast)
      device_opened = droid_open_device(disp, disp->Options.ForceSoftware);

   /* Qualcomm Android devices expose the GPU only through the KGSL kernel
    * driver (/dev/kgsl-3d0) and have no DRM render node an app process may
    * open, so droid_open_device() above fails.  Mirror the wayland and
    * surfaceless KGSL fallback: use the kgsl KMD directly when
    * MESA_LOADER_DRIVER_OVERRIDE=kgsl (disp->Options.Kgsl).
    */
   if (!device_opened && !disp->Options.ForceSoftware && disp->Options.Kgsl) {
      dri2_dpy->fd_render_gpu = loader_open_device("/dev/kgsl-3d0");
      if (dri2_dpy->fd_render_gpu >= 0) {
         dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;
         dri2_dpy->driver_name = strdup("kgsl");
         dri2_dpy->loader_extensions = droid_image_loader_extensions;
         dri2_detect_swrast_kopper(disp);
         if (dri2_create_screen(disp)) {
            device_opened = EGL_TRUE;
         } else {
            _eglLog(_EGL_WARNING, "DRI2: failed to create KGSL screen");
            free(dri2_dpy->driver_name);
            dri2_dpy->driver_name = NULL;
            close(dri2_dpy->fd_render_gpu);
            dri2_dpy->fd_render_gpu = -1;
            dri2_dpy->fd_display_gpu = -1;
         }
      } else {
         _eglLog(_EGL_WARNING, "DRI2: failed to open /dev/kgsl-3d0");
      }
   }

   if ((!device_opened && disp->Options.ForceSoftware) ||
       force_pure_swrast) {"""

if marker in text:
    print("platform_android.c already patched (kgsl fallback)", file=sys.stderr)
elif old in text:
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("patched src/egl/drivers/dri2/platform_android.c (kgsl fallback)", file=sys.stderr)
else:
    sys.exit("error: cannot patch platform_android.c: unexpected upstream layout")
PY
}

# Mesa gates EGL_OPENGL_API off on Android builds:
#   src/egl/main/eglcurrent.h: #if HAVE_OPENGL && !DETECT_OS_ANDROID
# so eglBindAPI(EGL_OPENGL_API) fails with EGL_BAD_PARAMETER (0x300C) and
# disp->ClientAPIs only advertises EGL_OPENGL_ES_*, which breaks FCL's
# POJAV_RENDERER=opengles3_desktopgl path:
#   EGLBridge: bind failed: 0x3000 / eglChooseConfig -> 0 configs
# The lfdevs "mesa-for-android-container" builds get desktop GL because they
# are compiled with a Linux toolchain (DETECT_OS_ANDROID=0); this NDK build
# targets Android, so drop the guard. The GLESv2 DSO already exports the
# desktop GL entry points (Mesa built with -Dopengl=true).
patch_mesa_android_desktopgl() {
  local src="$1"
  python3 - "$src" <<'PY'
import pathlib, sys

src = pathlib.Path(sys.argv[1])
path = src / "src" / "egl" / "main" / "eglcurrent.h"
text = path.read_text(encoding="utf-8")
marker = "OpenGL is accepted on Android for this NDK build"
old = """#if HAVE_OPENGL && !DETECT_OS_ANDROID
   /* OpenGL is not a valid/supported API on Android */
   if (api == EGL_OPENGL_API)
      return true;
#endif"""
new = """#if HAVE_OPENGL
   /* OpenGL is accepted on Android for this NDK build: the renderer plugin
    * is used by FCL's opengles3_desktopgl path, which needs EGL_OPENGL_API.
    * Container Mesa builds get this because they are compiled with a Linux
    * toolchain (DETECT_OS_ANDROID=0).
    */
   if (api == EGL_OPENGL_API)
      return true;
#endif"""

if marker in text:
    print("eglcurrent.h already patched (android desktop GL)", file=sys.stderr)
elif old in text:
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("patched src/egl/main/eglcurrent.h (EGL_OPENGL_API on Android)", file=sys.stderr)
else:
    sys.exit("error: cannot patch eglcurrent.h: unexpected upstream layout")
PY
}

# Mesa's u_init_pipe_screen_caps() derives caps.dmabuf from
# drmGetCap(fd, DRM_CAP_PRIME), but the freedreno KGSL backend opens a KGSL
# character device (/dev/kgsl-3d0), not a DRM node, so drmGetCap() fails and
# caps.dmabuf stays 0. dri_screen.c then leaves dmabuf_import/has_dmabuf false
# and every dma-buf import goes through dri2_from_dma_bufs()'s early
# "if (!screen->dmabuf_import) return NULL" (silent EGL_BAD_PARAMETER), which
# breaks:
#   - EGL window surfaces on Android (droid_create_image_from_native_buffer)
#   - AHardwareBuffer / EGL_NATIVE_BUFFER_ANDROID imports (EGLImage)
# KGSL advertises FD_FEATURE_IMPORT_DMABUF and implements both directions
# (kgsl_bo_from_dmabuf / kgsl_bo_dmabuf), so set the caps explicitly.
patch_mesa_freedreno_kgsl_dmabuf() {
  local src="$1"
  python3 - "$src" <<'PYEOF'
import pathlib, sys

src = pathlib.Path(sys.argv[1])
path = src / "src" / "gallium" / "drivers" / "freedreno" / "freedreno_screen.c"
text = path.read_text(encoding="utf-8")
marker = "KGSL advertises FD_FEATURE_IMPORT_DMABUF"
upstream_marker = "screen->kgsl_dmabuf"  # Mesa 26.3 already advertises KGSL dmabuf caps
old = """   u_init_pipe_screen_caps(&screen->base, 1);

   /* this is probably not totally correct.. but it's a start: */
"""
new = """   u_init_pipe_screen_caps(&screen->base, 1);

   /* KGSL is a character device, not a DRM device, so
    * u_init_pipe_screen_caps()'s drmGetCap(DRM_CAP_PRIME) fails and
    * caps->dmabuf stays 0. The DRI frontend then clears
    * dri_screen::dmabuf_import and every EGL window surface /
    * AHardwareBuffer (EGL_NATIVE_BUFFER_ANDROID) import fails with a silent
    * EGL_BAD_PARAMETER. KGSL advertises FD_FEATURE_IMPORT_DMABUF and
    * implements both directions (kgsl_bo_from_dmabuf / kgsl_bo_dmabuf).
    */
   if (fd_get_features(screen->dev) & FD_FEATURE_IMPORT_DMABUF)
      caps->dmabuf = 0x1 | 0x2; /* DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT */

   /* this is probably not totally correct.. but it's a start: */
"""
if marker in text:
    print("freedreno_screen.c already patched (kgsl dmabuf caps)", file=sys.stderr)
elif upstream_marker in text:
    print("freedreno_screen.c advertises KGSL dmabuf caps upstream; skipping patch",
          file=sys.stderr)
elif old in text:
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("patched freedreno_screen.c (KGSL dmabuf caps)", file=sys.stderr)
else:
    sys.exit("error: cannot patch freedreno_screen.c: unexpected upstream layout")
PYEOF
}

build_mesa() {
  local src="${MESA_SRC:-$WORK/mesa}"
  # CI caches $WORK (.native-build): always resync the checkout with MESA_REF so
  # a cached tree cannot silently keep building the previous Mesa release.
  clone_if_needed "${src}" "${MESA_REPO}" "${MESA_REF}"
  patch_mesa_android_stub "${src}"
  patch_mesa_android_kgsl "${src}"
  patch_mesa_android_desktopgl "${src}"
  patch_mesa_freedreno_kgsl_dmabuf "${src}"
  write_cross_file "${WORK}/android-mesa.ini" "${DRM_PREFIX}/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="${DRM_PREFIX}/lib/pkgconfig"
  export PKG_CONFIG_PATH="${DRM_PREFIX}/lib/pkgconfig"

  local -a dflags=()
  mapfile -t dflags < <(mesa_flags "${src}")
  log "Mesa -D flags: ${dflags[*]}"

  # Drop stale meson build dirs (CI cache may still have -Dosmesa=true).
  rm -rf "${src}/build-android"

  local -a args=(
    --prefix="${MESA_PREFIX}"
    --cross-file="${WORK}/android-mesa.ini"
    -Dbuildtype=release
    "${dflags[@]}"
  )
  if ! meson_setup_mesa "${src}" "${args[@]}"; then
    if [[ "${BUILD_VULKAN}" == "1" ]]; then
      log "meson setup with vulkan failed; retrying gallium-only"
      BUILD_VULKAN=0
      rm -rf "${src}/build-android"
      dflags=()
      mapfile -t dflags < <(mesa_flags "${src}")
      args=(
        --prefix="${MESA_PREFIX}"
        --cross-file="${WORK}/android-mesa.ini"
        -Dbuildtype=release
        "${dflags[@]}"
      )
      meson_setup_mesa "${src}" "${args[@]}" || die "meson setup failed"
    else
      die "meson setup failed"
    fi
  fi
  meson compile -C "${src}/build-android" -j "${JOBS}"
  meson install -C "${src}/build-android"
}

# libEGL_mesa.so.1.0.0 → libEGL_mesa.so
unversioned_so_name() {
  local base
  base="$(basename "$1")"
  printf '%s' "${base%%.so*}.so"
}

copy_so() {
  local src="$1"
  local dest="${OUT_JNI}/$(unversioned_so_name "${src}")"
  cp -L "${src}" "${dest}"
  log "packaged $(basename "${dest}")"
}

package_libs() {
  mkdir -p "${OUT_JNI}" "${OUT_DIST}"
  log "Mesa installed libraries:"
  find "${MESA_PREFIX}" \( -name '*.so' -o -name '*.so.*' \) | sort >&2

  local so
  while IFS= read -r so; do
    local base
    base="$(basename "${so}")"
    case "${base}" in
      libEGL*.so|libEGL*.so.*) copy_so "${so}" ;;
      libGLESv2*.so|libGLESv2*.so.*) copy_so "${so}" ;;
      libgallium*.so|libgallium*.so.*) copy_so "${so}" ;;
      libglapi*.so|libglapi*.so.*) copy_so "${so}" ;;
      libvulkan_freedreno.so|libvulkan_freedreno.so.*) copy_so "${so}" ;;
      *_dri.so|*_dri.so.*) copy_so "${so}" ;;
    esac
  done < <(find "${MESA_PREFIX}" \( -name '*.so' -o -name '*.so.*' \) | sort)

  local egl="" gles="" gallium="" osmesa=""
  egl="$(ls "${OUT_JNI}"/libEGL*.so 2>/dev/null | head -n1 || true)"
  gles="$(ls "${OUT_JNI}"/libGLESv2*.so 2>/dev/null | head -n1 || true)"
  gallium="$(ls "${OUT_JNI}"/libgallium*.so 2>/dev/null | head -n1 || true)"
  osmesa="$(ls "${OUT_JNI}"/libOSMesa*.so 2>/dev/null | head -n1 || true)"

  if [[ -z "${egl}" || -z "${gles}" ]]; then
    if [[ -n "${osmesa}" ]]; then
      log "EGL/GLES not built; OSMesa is present (Mesa 25 tree)"
    else
      die "Mesa did not install libEGL*.so + libGLESv2*.so (or libOSMesa.so)"
    fi
  fi
  if [[ -z "${gallium}" ]]; then
    log "warning: libgallium*.so not found; EGL may fail to load the dri driver"
  fi

  # The plugin ships libEGL_mesa.so / libGLESv2_mesa.so as shims (built by
  # CMake). Rename the real Mesa libs so the shims can dlopen them next to
  # themselves:
  #   libEGL_mesa.so      -> libEGL_mesa_core.so     (presentation shim)
  #   libGLESv2_mesa.so   -> libGLESv2_mesa_core.so  (GL entry-point shim)
  if [[ -f "${OUT_JNI}/libEGL_mesa.so" ]]; then
    mv -f "${OUT_JNI}/libEGL_mesa.so" "${OUT_JNI}/libEGL_mesa_core.so"
    log "renamed libEGL_mesa.so -> libEGL_mesa_core.so (EGL shim slot)"
  fi
  if [[ -f "${OUT_JNI}/libGLESv2_mesa.so" ]]; then
    mv -f "${OUT_JNI}/libGLESv2_mesa.so" "${OUT_JNI}/libGLESv2_mesa_core.so"
    log "renamed libGLESv2_mesa.so -> libGLESv2_mesa_core.so (GLES shim slot)"
  fi

  local turnip=""
  turnip="$(find "${OUT_JNI}" "${MESA_PREFIX}" -name 'libvulkan_freedreno.so' 2>/dev/null | head -n1 || true)"
  if [[ -n "${turnip}" && -f "${turnip}" ]]; then
    if [[ "${turnip}" != "${OUT_JNI}/libvulkan_freedreno.so" ]]; then
      cp -L "${turnip}" "${OUT_JNI}/libvulkan_freedreno.so"
    fi
    "${ROOT}/scripts/package-adrenotools-zip.sh" \
      "${OUT_JNI}/libvulkan_freedreno.so" \
      "${OUT_DIST}/turnip-freedreno-kgsl-adrenotools.zip"
  else
    log "Turnip libvulkan_freedreno.so not built; skipping AdrenoTools zip"
  fi

  # Ensure libGLESv2_mesa can resolve libgallium_dri.so next to it when
  # LWJGL dlopens the absolute plugin path (no implicit $ORIGIN otherwise).
  if ! command -v patchelf >/dev/null 2>&1; then
    sudo apt-get install -y patchelf >/dev/null 2>&1 || true
  fi
  if command -v patchelf >/dev/null 2>&1; then
    for so in "${OUT_JNI}"/*.so; do
      [[ -f "${so}" ]] || continue
      patchelf --set-rpath '$ORIGIN' "${so}" 2>/dev/null || true
    done
    log "patchelf DT_RUNPATH=\$ORIGIN on ${OUT_JNI}"
  else
    log "WARN: patchelf unavailable; DT_RUNPATH may be missing"
  fi

  if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip -S "${OUT_JNI}"/*.so || true
  elif [[ -x "$(ndk_prebuilt)/bin/llvm-strip" ]]; then
    "$(ndk_prebuilt)/bin/llvm-strip" -S "${OUT_JNI}"/*.so || true
  fi

  verify_mesa_version
  verify_no_private_deps

  log "jniLibs payload:"
  ls -lh "${OUT_JNI}" >&2
}

main() {
  need_cmd git
  need_cmd meson
  need_cmd ninja
  need_cmd pkg-config
  resolve_ndk
  mkdir -p "${WORK}" "${OUT_JNI}" "${OUT_DIST}"
  log "NDK=${NDK}"
  log "MESA_REF=${MESA_REF}"
  build_libdrm
  build_mesa
  package_libs
  log "Mesa Android NDK build complete"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
