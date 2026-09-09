#!/usr/bin/env bash
# Cross-compile Mesa Gallium Freedreno (KGSL) + OSMesa + Turnip for Android arm64.
#
# Proven flags come from:
#   - Vera-Firefly/android-mesa-build  (OSMesa + gallium freedreno/zink, Android NDK)
#   - lfdevs/mesa-for-android-container  (-Dfreedreno-kmds=kgsl)
#   - whitebelyash turnip_builder.sh     (Vulkan Turnip AdrenoTools zip)
#
# Usage:
#   ./scripts/build-mesa-android.sh
# Env:
#   MESA_SRC, DRM_SRC, NDK, SDK_VER, MESA_PREFIX, OUT_JNI, BUILD_VULKAN

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK_DIR:-$ROOT/.native-build}"
NDK="${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}}"
SDK_VER="${SDK_VER:-26}"
MESA_REF="${MESA_REF:-adreno-main}"
MESA_REPO="${MESA_REPO:-https://github.com/lfdevs/mesa-for-android-container.git}"
DRM_REPO="${DRM_REPO:-https://gitlab.freedesktop.org/mesa/drm.git}"
MESA_PREFIX="${MESA_PREFIX:-$WORK/mesa-prefix}"
DRM_PREFIX="${DRM_PREFIX:-$WORK/drm-static}"
OUT_JNI="${OUT_JNI:-$ROOT/app/src/main/jniLibs/arm64-v8a}"
OUT_DIST="${OUT_DIST:-$ROOT/dist}"
BUILD_VULKAN="${BUILD_VULKAN:-1}"
JOBS="${JOBS:-$(nproc)}"

log() { printf '==> %s\n' "$*"; }
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
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld', '-static-libstdc++']

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

build_libdrm() {
  local src="${DRM_SRC:-$WORK/drm}"
  clone_if_needed "${src}" "${DRM_REPO}" "${DRM_REF:-main}"
  write_cross_file "${WORK}/android-drm.ini" "${DRM_PREFIX}/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="${DRM_PREFIX}/lib/pkgconfig"
  local drm_reconf=()
  if [[ -d "${src}/build-android" ]]; then
    drm_reconf=(--reconfigure)
  fi
  meson setup "${src}/build-android" "${src}" \
    --prefix="${DRM_PREFIX}" \
    --cross-file "${WORK}/android-drm.ini" \
    -Ddefault_library=static \
    -Dintel=disabled \
    -Dradeon=disabled \
    -Damdgpu=disabled \
    -Dnouveau=disabled \
    -Dvmwgfx=disabled \
    -Dfreedreno=enabled \
    -Dvc4=disabled \
    -Detnaviv=disabled \
    -Dfreedreno-kgsl=true \
    "${drm_reconf[@]}"
  meson compile -C "${src}/build-android" -j "${JOBS}"
  meson install -C "${src}/build-android"
}

mesa_flags() {
  local vulkan_drivers=""
  if [[ "${BUILD_VULKAN}" == "1" ]]; then
    vulkan_drivers="freedreno"
  fi
  cat <<EOF
--prefix=${MESA_PREFIX}
--cross-file=${WORK}/android-mesa.ini
-Dbuildtype=release
-Dplatforms=android
-Dplatform-sdk-version=33
-Dandroid-stub=true
-Dandroid-libbacktrace=disabled
-Dandroid-strict=false
-Dxlib-lease=disabled
-Degl=disabled
-Dgbm=disabled
-Dglx=disabled
-Dllvm=disabled
-Dopengl=true
-Dosmesa=true
-Dgles1=disabled
-Dglvnd=disabled
-Dlibunwind=disabled
-Dmicrosoft-clc=disabled
-Dvalgrind=disabled
-Dintel-rt=disabled
-Dgallium-drivers=zink,freedreno
-Dfreedreno-kmds=kgsl
-Dvulkan-drivers=${vulkan_drivers}
-Dtools=
EOF
}

build_mesa() {
  local src="${MESA_SRC:-$WORK/mesa}"
  if [[ ! -d "${src}/.git" ]]; then
    clone_if_needed "${src}" "${MESA_REPO}" "${MESA_REF}"
  fi
  write_cross_file "${WORK}/android-mesa.ini" "${DRM_PREFIX}/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="${DRM_PREFIX}/lib/pkgconfig"
  export PKG_CONFIG_PATH="${DRM_PREFIX}/lib/pkgconfig"

  local args=()
  mapfile -t args < <(mesa_flags)
  log "meson setup Mesa with Freedreno KGSL + OSMesa"
  local reconf=()
  if [[ -d "${src}/build-android" ]]; then
    reconf=(--reconfigure)
  fi
  if ! meson setup "${src}/build-android" "${src}" "${args[@]}" "${reconf[@]}"; then
    if [[ "${BUILD_VULKAN}" == "1" ]]; then
      log "meson setup with vulkan failed; retrying gallium-only"
      BUILD_VULKAN=0
      rm -rf "${src}/build-android"
      args=()
      mapfile -t args < <(mesa_flags)
      meson setup "${src}/build-android" "${src}" "${args[@]}"
    else
      die "meson setup failed"
    fi
  fi
  meson compile -C "${src}/build-android" -j "${JOBS}"
  meson install -C "${src}/build-android"
}

package_libs() {
  mkdir -p "${OUT_JNI}" "${OUT_DIST}"
  local libdir="${MESA_PREFIX}/lib"
  [[ -d "${libdir}" ]] || libdir="${MESA_PREFIX}/lib64"
  [[ -d "${libdir}" ]] || die "Mesa libdir missing under ${MESA_PREFIX}"

  log "Mesa installed libraries:"
  find "${MESA_PREFIX}" -name '*.so*' | sort

  local osmesa=""
  for cand in "${libdir}/libOSMesa.so" "${libdir}/libOSMesa.so.8"; do
    if [[ -f "${cand}" ]]; then
      osmesa="${cand}"
      break
    fi
  done
  [[ -n "${osmesa}" ]] || die "libOSMesa.so not produced (OSMesa build failed)"
  cp -L "${osmesa}" "${OUT_JNI}/libOSMesa.so"

  if [[ -f "${libdir}/libglapi.so" ]]; then
    cp -L "${libdir}/libglapi.so" "${OUT_JNI}/libglapi.so"
  fi

  local turnip=""
  for cand in \
      "${libdir}/libvulkan_freedreno.so" \
      "${MESA_PREFIX}/lib/libvulkan_freedreno.so" \
      "${MESA_PREFIX}/share/vulkan/icd.d/"*; do
    if [[ -f "${cand}" && "${cand}" == *.so ]]; then
      turnip="${cand}"
      break
    fi
  done
  # ICD json often points at libvulkan_freedreno.so next to it or in lib/
  if [[ -z "${turnip}" ]]; then
    turnip="$(find "${MESA_PREFIX}" -name 'libvulkan_freedreno.so' | head -n1 || true)"
  fi
  if [[ -n "${turnip}" && -f "${turnip}" ]]; then
    cp -L "${turnip}" "${OUT_JNI}/libvulkan_freedreno.so"
    "${ROOT}/scripts/package-adrenotools-zip.sh" \
      "${OUT_JNI}/libvulkan_freedreno.so" \
      "${OUT_DIST}/turnip-freedreno-kgsl-adrenotools.zip"
  else
    log "Turnip libvulkan_freedreno.so not built; skipping AdrenoTools zip"
  fi

  if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip -S "${OUT_JNI}"/*.so || true
  elif [[ -x "$(ndk_prebuilt)/bin/llvm-strip" ]]; then
    "$(ndk_prebuilt)/bin/llvm-strip" -S "${OUT_JNI}"/*.so || true
  fi

  log "jniLibs payload:"
  ls -lh "${OUT_JNI}"
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

main "$@"
