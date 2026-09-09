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
MESA_REF="${MESA_REF:-adreno-main}"
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
old = """  stub_libs = []
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
new = """  stub_libs = []

  # Private platform libs: link the stubs into the Mesa DSOs (no DT_NEEDED).
  foreach lib : ['cutils', 'hardware']
    stub_libs += static_library(
      lib,
      files(lib + '_stub.cpp'),
      include_directories : inc_include,
      install : false,
    )
  endforeach

  # Public platform libs: keep shared stubs; at runtime the real system
  # libraries (liblog/libnativewindow/libsync are in public.libraries.txt)
  # satisfy the DT_NEEDED entries.
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
elif old in text:
    meson.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("patched src/android_stub/meson.build (static cutils/hardware)", file=sys.stderr)
else:
    sys.exit("error: cannot patch src/android_stub/meson.build: unexpected upstream layout")

hw_text = hw.read_text(encoding="utf-8")
if "*module = NULL" in hw_text:
    print("hardware_stub.cpp already patched", file=sys.stderr)
else:
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
    if old_hw not in hw_text:
        sys.exit("error: cannot patch src/android_stub/hardware_stub.cpp: unexpected upstream layout")
    hw.write_text(hw_text.replace(old_hw, new_hw, 1), encoding="utf-8")
    print("patched src/android_stub/hardware_stub.cpp (hw_get_module returns -1)", file=sys.stderr)
PY
}

# Fail the build if a packaged DSO still has DT_NEEDED on a private platform
# library that Android app namespaces cannot resolve.
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

build_mesa() {
  local src="${MESA_SRC:-$WORK/mesa}"
  if [[ ! -d "${src}/.git" ]]; then
    clone_if_needed "${src}" "${MESA_REPO}" "${MESA_REF}"
  fi
  patch_mesa_android_stub "${src}"
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
