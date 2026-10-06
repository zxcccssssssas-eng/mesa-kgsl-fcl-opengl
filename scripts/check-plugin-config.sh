#!/usr/bin/env bash
# Sanity-check that this tree is a FoldCraftLauncher renderer plugin, not a
# Linux container tarball. Used in CI before/after assemble.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0

check() {
  local desc="$1"
  shift
  if "$@"; then
    printf 'ok  %s\n' "${desc}"
  else
    printf 'FAIL %s\n' "${desc}"
    fail=1
  fi
}

check "Android Gradle project exists" test -f "${ROOT}/app/build.gradle.kts"
check "FCL renderer manifest exists" test -f "${ROOT}/app/src/main/AndroidManifest.xml"
check "KGSL env constructor source exists" test -f "${ROOT}/app/src/main/cpp/kgsl_init.c"
check "Mesa NDK build script exists" test -x "${ROOT}/scripts/build-mesa-android.sh" -o -f "${ROOT}/scripts/build-mesa-android.sh"
check "libdrm meson flags are probed from meson_options.txt" \
  grep -q 'drm_has_option' "${ROOT}/scripts/build-mesa-android.sh"
check "build script always resyncs the cached Mesa checkout" \
  grep -q 'always resync the checkout with MESA_REF' "${ROOT}/scripts/build-mesa-android.sh"
check "build script allows kopper on Android for zink" \
  grep -q 'patch_mesa_android_kopper' "${ROOT}/scripts/build-mesa-android.sh"
check "build script verifies the packaged Mesa version" \
  grep -q 'verify_mesa_version' "${ROOT}/scripts/build-mesa-android.sh"
check "default Mesa ref is 26.3.0-devel" \
  grep -q 'MESA_REF:-mesa-26.3.0-devel-20260824' "${ROOT}/scripts/build-mesa-android.sh"
check "Mesa meson flags are probed from meson.options" \
  grep -q 'mesa_flags' "${ROOT}/scripts/build-mesa-android.sh"
check "libdrm setup does not hardcode -Dfreedreno=enabled as a meson arg" \
  bash -c "! grep -qE '^[[:space:]]+-Dfreedreno=enabled(\\\\|$)' '${ROOT}/scripts/build-mesa-android.sh'"
check "Mesa setup does not hardcode -Dosmesa=true" \
  bash -c "! grep -qE '^[[:space:]]+-Dosmesa=true(\\\\|$)' '${ROOT}/scripts/build-mesa-android.sh'"
check "NDK clang target defaults to API 29" \
  grep -q 'SDK_VER="${SDK_VER:-29}"' "${ROOT}/scripts/build-mesa-android.sh"

manifest="${ROOT}/app/src/main/AndroidManifest.xml"
gradle="${ROOT}/app/build.gradle.kts"

check "manifest declares fclPlugin" grep -q 'android:name="fclPlugin"' "${manifest}"
check "manifest declares renderer meta-data" grep -q 'android:name="renderer"' "${manifest}"
check "manifest declares boatEnv" grep -q 'android:name="boatEnv"' "${manifest}"
check "manifest declares pojavEnv" grep -q 'android:name="pojavEnv"' "${manifest}"
check "extractNativeLibs is true" grep -q 'android:extractNativeLibs="true"' "${manifest}"

check "applicationIdSuffix is .freedreno.kgsl" grep -q 'applicationIdSuffix = ".freedreno.kgsl"' "${gradle}"
check "plugin minSdk is 29" grep -q 'minSdk = 29' "${gradle}"
check "renderer id is FreedrenoKGSL EGL/GLES mesa" grep -q 'FreedrenoKGSL:libGLESv2_mesa.so:libEGL_mesa.so' "${gradle}"
check "renderer EGL name has no leading slash" \
  bash -c "! grep -q 'libGLESv2_mesa.so:/libEGL_mesa.so' '${gradle}'"
check "GALLIUM_DRIVER=freedreno" grep -q '"GALLIUM_DRIVER" to "freedreno"' "${gradle}"
check "MESA_LOADER_DRIVER_OVERRIDE=kgsl" grep -q '"MESA_LOADER_DRIVER_OVERRIDE" to "kgsl"' "${gradle}"
check "POJAV_RENDERER=opengles3_desktopgl" grep -q '"POJAV_RENDERER" to "opengles3_desktopgl"' "${gradle}"
check "pojavEnv DLOPEN includes libEGL_mesa.so" grep -q 'libEGL_mesa.so' "${gradle}"
check "does not force MESA_GL_VERSION_OVERRIDE=4.6" bash -c "! grep -qE '\"MESA_GL_VERSION_OVERRIDE\"[[:space:]]+to[[:space:]]+\"4\.6\"' '${gradle}'"
check "does not force mesa_glthread=true" bash -c "! grep -qE '\"mesa_glthread\"[[:space:]]+to[[:space:]]+\"true\"' '${gradle}'"
check "legacy JNI packaging enabled" grep -q 'useLegacyPackaging = true' "${gradle}"
check "NDK flexible page sizes enabled" grep -q 'ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON' "${gradle}"
check "Mesa linked with 16KB max-page-size" grep -q 'max-page-size=16384' "${ROOT}/scripts/build-mesa-android.sh"
check "Mesa link uses \$ORIGIN rpath" grep -q 'rpath' "${ROOT}/scripts/build-mesa-android.sh"

check "build script patches Mesa android_stub private libs" \
  grep -q 'patch_mesa_android_stub' "${ROOT}/scripts/build-mesa-android.sh"
check "build script links cutils/hardware stubs statically" \
  grep -q "foreach lib : \['cutils', 'hardware'\]" "${ROOT}/scripts/build-mesa-android.sh"
check "build script verifies no private platform deps" \
  grep -q 'verify_no_private_deps' "${ROOT}/scripts/build-mesa-android.sh"
check "hardware stub returns failure (no NULL deref in u_gralloc)" \
  grep -q 'return -1;' "${ROOT}/scripts/build-mesa-android.sh"
check "build script patches Mesa Android EGL for KGSL" \
  grep -q 'patch_mesa_android_kgsl' "${ROOT}/scripts/build-mesa-android.sh"
check "KGSL fallback opens /dev/kgsl-3d0" \
  grep -q '/dev/kgsl-3d0' "${ROOT}/scripts/build-mesa-android.sh"
check "build script fixes KGSL dma-buf caps" \
  grep -q 'patch_mesa_freedreno_kgsl_dmabuf' "${ROOT}/scripts/build-mesa-android.sh"
check "KGSL dmabuf patch uses FD_FEATURE_IMPORT_DMABUF" \
  grep -q 'FD_FEATURE_IMPORT_DMABUF' "${ROOT}/scripts/build-mesa-android.sh"
check "build script renames Mesa EGL for the shim" \
  grep -q 'libEGL_mesa_core.so' "${ROOT}/scripts/build-mesa-android.sh"
check "EGL shim source present" \
  test -f "${ROOT}/app/src/main/cpp/egl_shim.c"
check "EGL shim built as libEGL_mesa.so" \
  grep -q 'add_library(EGL_mesa SHARED egl_shim.c vulkan_present.c)' "${ROOT}/app/src/main/cpp/CMakeLists.txt"
check "GLES entry-point shim source present" \
  test -f "${ROOT}/app/src/main/cpp/gles_shim.c"
check "GLES shim exports glXGetProcAddress" \
  grep -q 'glXGetProcAddress' "${ROOT}/app/src/main/cpp/gles_shim.c"
check "GLES shim forwards EGL to libEGL_mesa.so" \
  grep -q 'libEGL_mesa.so' "${ROOT}/app/src/main/cpp/gles_shim.c"
check "GLES shim exports eglGetDisplay" \
  grep -q 'EGLDisplay eglGetDisplay' "${ROOT}/app/src/main/cpp/gles_shim.c"
check "GLES shim exports eglChooseConfig" \
  grep -q 'EGLBoolean eglChooseConfig' "${ROOT}/app/src/main/cpp/gles_shim.c"
check "libEGL_mesa is not linked against libvulkan" \
  bash -c "! grep -q 'target_link_libraries(EGL_mesa PRIVATE nativewindow log dl vulkan)' '${ROOT}/app/src/main/cpp/CMakeLists.txt'"
check "vulkan presenter dlopens libvulkan.so locally" \
  grep -Fq 'dlopen("libvulkan.so", RTLD_LOCAL | RTLD_NOW)' "${ROOT}/app/src/main/cpp/vulkan_present.c"
check "GLES shim EGL forwarders" \
  bash "${ROOT}/scripts/test-gles-egl-exports.sh"
check "build script renames Mesa GLES for the shim" \
  grep -q 'libGLESv2_mesa_core.so' "${ROOT}/scripts/build-mesa-android.sh"
check "build script enables EGL_OPENGL_API on Android" \
  grep -q 'patch_mesa_android_desktopgl' "${ROOT}/scripts/build-mesa-android.sh"
check "desktop GL patch touches _eglIsApiValid" \
  grep -q 'eglcurrent.h' "${ROOT}/scripts/build-mesa-android.sh"
check "CI reuses Mesa jniLibs when the NDK job is skipped" \
  grep -q 'Reuse Mesa native libs from last successful build' "${ROOT}/.github/workflows/build.yml"
check "CI verifies real Mesa payload before publishing APK" \
  grep -q 'require-mesa-payload.sh' "${ROOT}/.github/workflows/build.yml"
check "CI does not name the plugin artifact as a stub APK" \
  bash -c "! grep -q 'NAME=\"FCL-FreedrenoKGSL-stub.apk\"' '${ROOT}/.github/workflows/build.yml'"
check "require-mesa-payload script is executable" \
  test -x "${ROOT}/scripts/require-mesa-payload.sh"

# Probe Mesa 26-style meson.options: osmesa must not be emitted.
mesa_probe_dir="$(mktemp -d)"
trap 'rm -rf "${mesa_probe_dir}"' EXIT
cat >"${mesa_probe_dir}/meson.options" <<'EOF'
option('egl', type : 'feature')
option('gles2', type : 'feature')
option('opengl', type : 'boolean', value : true)
option('platforms', type : 'array', value : ['auto'])
option('gallium-drivers', type : 'array', value : ['auto'])
option('freedreno-kmds', type : 'array', value : ['msm'])
option('vulkan-drivers', type : 'array', value : ['auto'])
option('egl-lib-suffix', type : 'string', value : '')
option('gles-lib-suffix', type : 'string', value : '')
option('android-stub', type : 'boolean', value : false)
option('glx', type : 'combo', value : 'auto', choices : ['auto', 'disabled', 'dri', 'xlib'])
option('llvm', type : 'feature')
EOF
# shellcheck disable=SC1091
source "${ROOT}/scripts/build-mesa-android.sh"
BUILD_VULKAN=1
probe_flags="$(mesa_flags "${mesa_probe_dir}" | tr '\n' ' ')"
check "probed flags enable EGL" bash -c "[[ '${probe_flags}' == *'-Degl=enabled'* ]]"
check "probed flags enable GLES2" bash -c "[[ '${probe_flags}' == *'-Dgles2=enabled'* ]]"
check "probed flags set freedreno-kmds=kgsl" bash -c "[[ '${probe_flags}' == *'-Dfreedreno-kmds=kgsl'* ]]"
check "probed flags set egl-lib-suffix=_mesa" bash -c "[[ '${probe_flags}' == *'-Degl-lib-suffix=_mesa'* ]]"
check "probed flags do not include osmesa" bash -c "[[ '${probe_flags}' != *osmesa* ]]"

# If an APK was passed, inspect it.
if [[ "${1:-}" != "" ]]; then
  APK="$1"
  check "APK file exists" test -f "${APK}"
  check "APK contains a real Mesa payload (not CMake stubs)" \
    "${ROOT}/scripts/require-mesa-payload.sh" "${APK}"
  if command -v aapt >/dev/null 2>&1; then
    dump="$(aapt dump xmltree "${APK}" AndroidManifest.xml || true)"
    check "APK package is com.mio.plugin.renderer.freedreno.kgsl" \
      grep -q 'com.mio.plugin.renderer.freedreno.kgsl' <<<"${dump}"
    check "APK contains fclPlugin meta-data" grep -q 'fclPlugin' <<<"${dump}"
    check "APK contains FreedrenoKGSL renderer string" grep -q 'FreedrenoKGSL' <<<"${dump}"
  fi
fi

if [[ "${fail}" -ne 0 ]]; then
  echo "plugin config checks failed"
  exit 1
fi
echo "all plugin config checks passed"
