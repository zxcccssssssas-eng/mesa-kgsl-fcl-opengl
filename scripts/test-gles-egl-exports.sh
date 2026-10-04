#!/usr/bin/env bash
# Host compile/run of the GLES EGL forwarders and the Vulkan dlopen split.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$(mktemp -d)"
trap 'rm -rf "${DIR}"' EXIT
CC="${CC:-gcc}"
INC="${ROOT}/tests/host"
CFLAGS=(-std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -I"${INC}")

"${CC}" -shared -fPIC -O2 "${CFLAGS[@]}" \
  -ffunction-sections -fdata-sections \
  "${ROOT}/app/src/main/cpp/gles_shim.c" "${ROOT}/tests/host/android_stubs.c" \
  -o "${DIR}/libGLESv2_mesa.so" -ldl \
  -Wl,--gc-sections -Wl,--no-undefined -Wl,-soname,libGLESv2_mesa.so
"${CC}" -shared -fPIC -O2 "${CFLAGS[@]}" -DFAKE_PRESENT \
  "${ROOT}/tests/gles_egl_exports_fake.c" \
  -o "${DIR}/libEGL_mesa.so" -Wl,--no-undefined -Wl,-soname,libEGL_mesa.so
"${CC}" -shared -fPIC -O2 "${CFLAGS[@]}" -DFAKE_CORE \
  "${ROOT}/tests/gles_egl_exports_fake.c" \
  -o "${DIR}/libEGL_mesa_core.so" -Wl,--no-undefined -Wl,-soname,libEGL_mesa_core.so
"${CC}" -O2 "${CFLAGS[@]}" \
  "${ROOT}/tests/gles_egl_exports_test.c" -o "${DIR}/gles_egl_exports_test" -ldl

if readelf -d "${DIR}/libGLESv2_mesa.so" | grep -E 'NEEDED.*(libEGL|libvulkan|libGLESv2)'; then
  echo "GLES shim has an unexpected DT_NEEDED"
  exit 1
fi
for sym in eglGetDisplay eglChooseConfig eglGetProcAddress eglWaitGL eglWaitNative \
           eglCreateImageKHR glXGetProcAddress; do
  if ! readelf -Ws "${DIR}/libGLESv2_mesa.so" | awk -v s="${sym}" \
      '$8 == s && $5 == "GLOBAL" && $7 != "UND" { found = 1 } END { exit !found }'; then
    echo "missing dynamic symbol ${sym}"
    readelf -Ws "${DIR}/libGLESv2_mesa.so" | grep "${sym}" || true
    exit 1
  fi
done

"${DIR}/gles_egl_exports_test" "${DIR}"

"${CC}" -O2 "${CFLAGS[@]}" \
  "${ROOT}/tests/shim_state_test.c" "${ROOT}/tests/host/android_stubs.c" \
  -o "${DIR}/shim_state_test" -ldl
"${DIR}/shim_state_test"

if [[ -f /usr/include/vulkan/vulkan.h ]]; then
  "${CC}" -c -O2 "${CFLAGS[@]}" -I"${ROOT}/app/src/main/cpp" \
    "${ROOT}/app/src/main/cpp/vulkan_present.c" -o "${DIR}/vulkan_present.o"
  if nm "${DIR}/vulkan_present.o" | awk '$1 == "U" && $2 ~ /^vk[A-Z]/ { found = 1 } END { exit !found }'; then
    echo "vulkan_present.o has undefined vk* symbols:"
    nm "${DIR}/vulkan_present.o" | awk '$1 == "U" && $2 ~ /^vk[A-Z]/'
    exit 1
  fi
  if nm "${DIR}/vulkan_present.o" | awk '($2 == "T" || $2 == "D") && $3 ~ /^vk[A-Z]/ { found = 1 } END { exit !found }'; then
    echo "vulkan_present.o defines a vk* symbol"
    nm "${DIR}/vulkan_present.o" | awk '($2 == "T" || $2 == "D") && $3 ~ /^vk[A-Z]/'
    exit 1
  fi
  "${CC}" -shared -fPIC -O2 "${CFLAGS[@]}" -I"${ROOT}/app/src/main/cpp" \
    "${ROOT}/app/src/main/cpp/vulkan_present.c" "${ROOT}/tests/host/android_stubs.c" \
    -o "${DIR}/libvulkan_present.so" -ldl -Wl,--no-undefined
  if readelf -d "${DIR}/libvulkan_present.so" | grep -E 'NEEDED.*libvulkan'; then
    echo "vulkan presenter still DT_NEEDs libvulkan"
    exit 1
  fi
  if readelf -Ws "${DIR}/libvulkan_present.so" | awk \
      '$8 ~ /^vk[A-Z]/ && $7 != "UND" { found = 1 } END { exit !found }'; then
    echo "vulkan presenter exports a vk* symbol"
    exit 1
  fi
  echo "PASS: vulkan presenter has no vk* imports or exports"
else
  echo "skip vulkan host compile (no /usr/include/vulkan/vulkan.h)"
fi
