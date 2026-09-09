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
check "OSMBridge source exists" test -f "${ROOT}/app/src/main/cpp/osmbridge.c"
check "Mesa NDK build script exists" test -x "${ROOT}/scripts/build-mesa-android.sh" -o -f "${ROOT}/scripts/build-mesa-android.sh"

manifest="${ROOT}/app/src/main/AndroidManifest.xml"
gradle="${ROOT}/app/build.gradle.kts"

check "manifest declares fclPlugin" grep -q 'android:name="fclPlugin"' "${manifest}"
check "manifest declares renderer meta-data" grep -q 'android:name="renderer"' "${manifest}"
check "manifest declares boatEnv" grep -q 'android:name="boatEnv"' "${manifest}"
check "manifest declares pojavEnv" grep -q 'android:name="pojavEnv"' "${manifest}"
check "extractNativeLibs is true" grep -q 'android:extractNativeLibs="true"' "${manifest}"

check "applicationIdSuffix is .freedreno.kgsl" grep -q 'applicationIdSuffix = ".freedreno.kgsl"' "${gradle}"
check "renderer id is FreedrenoKGSL" grep -q 'FreedrenoKGSL:libOSMBridge.so:libEGL.so' "${gradle}"
check "GALLIUM_DRIVER=freedreno" grep -q '"GALLIUM_DRIVER" to "freedreno"' "${gradle}"
check "MESA_LOADER_DRIVER_OVERRIDE=kgsl" grep -q '"MESA_LOADER_DRIVER_OVERRIDE" to "kgsl"' "${gradle}"
check "POJAV_RENDERER=custom_gallium" grep -q '"POJAV_RENDERER" to "custom_gallium"' "${gradle}"
check "legacy JNI packaging enabled" grep -q 'useLegacyPackaging = true' "${gradle}"

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
    "lib/arm64-v8a/libOSMBridge.so",
    "lib/arm64-v8a/libOSMesa.so",
]
missing = [n for n in need if n not in names]
if missing:
    print("missing native libs:", missing)
    sys.exit(1)
print("apk native libs ok:", [n for n in names if n.startswith("lib/")])
PY
    check "APK contains arm64 OSMBridge + OSMesa" true
  fi
fi

if [[ "${fail}" -ne 0 ]]; then
  echo "plugin config checks failed"
  exit 1
fi
echo "all plugin config checks passed"
