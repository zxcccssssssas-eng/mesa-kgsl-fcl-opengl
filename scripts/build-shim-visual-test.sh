#!/usr/bin/env bash
# Build a separate diagnostic APK; never replaces or installs the user's plugin.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${NDK:?Set NDK to an Android NDK directory}"
: "${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT}"
: "${JAVA_HOME:?Set JAVA_HOME to JDK 17 or 21}"
OUT="${FCL_TEST_OUT:-${ROOT}/app/build/shim-visual-test}"
LIBS="${FCL_TEST_LIBS:-${ROOT}/app/src/main/jniLibs/arm64-v8a}"
CC="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
BT="${ANDROID_SDK_ROOT}/build-tools/34.0.0"
JAR="${ANDROID_SDK_ROOT}/platforms/android-34/android.jar"
mkdir -p "${OUT}/lib/arm64-v8a" "${OUT}/classes" "${OUT}/dex"
for lib in libEGL_mesa_core.so libgallium_dri.so; do
  cp "${LIBS}/${lib}" "${OUT}/lib/arm64-v8a/${lib}"
done
"${CC}" -shared -fPIC -O2 -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror \
  "${ROOT}/app/src/main/cpp/egl_shim.c" "${ROOT}/app/src/main/cpp/vulkan_present.c" \
  -o "${OUT}/lib/arm64-v8a/libEGL_mesa.so" -llog -ldl -lnativewindow -lvulkan \
  -Wl,-z,max-page-size=16384 -Wl,--no-undefined -Wl,-soname,libEGL_mesa.so
"${CC}" -shared -fPIC -O2 -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror \
  "${ROOT}/tests/android/visual_test.c" -o "${OUT}/lib/arm64-v8a/libvisualtest.so" \
  -llog -ldl -lnativewindow -landroid -Wl,-z,max-page-size=16384 -Wl,--no-undefined
"${CC}" -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror "${ROOT}/tests/shim_state_test.c" \
  -o "${OUT}/shim-state-test" -llog -ldl -lnativewindow -Wl,-z,max-page-size=16384
"${JAVA_HOME}/bin/javac" --release 8 -classpath "${JAR}" -d "${OUT}/classes" \
  "${ROOT}/tests/android/VisualTest.java"
"${BT}/d8" --min-api 29 --lib "${JAR}" --output "${OUT}/dex" \
  "${OUT}/classes/com/mio/plugin/shimtest/VisualTest.class"
"${BT}/aapt2" link -I "${JAR}" --manifest "${ROOT}/tests/android/AndroidManifest.xml" \
  -o "${OUT}/test.apk"
python3 - "${OUT}" <<'PY'
import zipfile, pathlib, sys
out = pathlib.Path(sys.argv[1])
with zipfile.ZipFile(out/'test.apk', 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(out/'dex/classes.dex', 'classes.dex')
    for p in (out/'lib').rglob('*.so'):
        z.write(p, str(p.relative_to(out)))
PY
if [[ ! -f "${OUT}/test.jks" ]]; then
  "${JAVA_HOME}/bin/keytool" -genkeypair -keystore "${OUT}/test.jks" -storepass android \
    -keypass android -alias test -keyalg RSA -dname 'CN=FCL shim test' -validity 10000
fi
"${BT}/apksigner" sign --ks "${OUT}/test.jks" --ks-pass pass:android "${OUT}/test.apk"
printf 'Built diagnostic APK: %s/test.apk\n' "${OUT}"
