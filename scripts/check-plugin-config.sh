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
check "renderer id is FreedrenoKGSL EGL/GLES mesa" grep -q 'FreedrenoKGSL:libGLESv2_mesa.so:/libEGL_mesa.so' "${gradle}"
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
check "build script enables EGL_OPENGL_API on Android" \
  grep -q 'patch_mesa_android_desktopgl' "${ROOT}/scripts/build-mesa-android.sh"
check "desktop GL patch touches _eglIsApiValid" \
  grep -q 'eglcurrent.h' "${ROOT}/scripts/build-mesa-android.sh"

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
  if command -v aapt >/dev/null 2>&1; then
    dump="$(aapt dump xmltree "${APK}" AndroidManifest.xml || true)"
    check "APK package is com.mio.plugin.renderer.freedreno.kgsl" \
      grep -q 'com.mio.plugin.renderer.freedreno.kgsl' <<<"${dump}"
    check "APK contains fclPlugin meta-data" grep -q 'fclPlugin' <<<"${dump}"
    check "APK contains FreedrenoKGSL renderer string" grep -q 'FreedrenoKGSL' <<<"${dump}"
  elif command -v python3 >/dev/null 2>&1; then
    python3 - "${APK}" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
names = z.namelist()
need = [
    "lib/arm64-v8a/libEGL_mesa.so",
    "lib/arm64-v8a/libGLESv2_mesa.so",
    "lib/arm64-v8a/libgallium_dri.so",
    "lib/arm64-v8a/libfreedreno_kgsl_init.so",
]
missing = [n for n in need if n not in names]
if missing:
    print("missing native libs:", missing)
    sys.exit(1)
print("apk native libs ok:", [n for n in names if n.startswith("lib/")])
PY
    check "APK contains arm64 EGL_mesa + GLESv2_mesa + gallium_dri" true
  fi
fi

if [[ "${fail}" -ne 0 ]]; then
  echo "plugin config checks failed"
  exit 1
fi
echo "all plugin config checks passed"
