#!/usr/bin/env bash
# Real Mesa context + pixel readback regression for FCL's forced SDL ES profile.
# Requires a Linux Mesa EGL runtime, its software driver and Vulkan headers.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$(mktemp -d)"
trap 'rm -rf "${DIR}"' EXIT
CC="${CC:-gcc}"
EGL_LIB="${HOST_EGL_LIBRARY:-$("${CC}" -print-file-name=libEGL.so.1)}"
if [[ ! -f "${EGL_LIB}" ]]; then
  echo "Mesa EGL runtime required; set HOST_EGL_LIBRARY to libEGL.so.1" >&2
  exit 1
fi
ln -s "$(realpath "${EGL_LIB}")" "${DIR}/libEGL_mesa_core.so"
CFLAGS=(-std=c11 -O2 -fPIC -Wall -Wextra -Werror -D_GNU_SOURCE -I"${ROOT}/tests/host")
"${CC}" -shared "${CFLAGS[@]}" \
  "${ROOT}/app/src/main/cpp/egl_shim.c" "${ROOT}/app/src/main/cpp/vulkan_present.c" \
  "${ROOT}/tests/host/android_stubs.c" -o "${DIR}/libEGL_mesa.so" \
  -ldl -Wl,--no-undefined -Wl,-soname,libEGL_mesa.so
"${CC}" -shared "${CFLAGS[@]}" \
  "${ROOT}/app/src/main/cpp/gles_shim.c" "${ROOT}/tests/host/android_stubs.c" \
  -o "${DIR}/libGLESv2_mesa.so" -ldl -Wl,--no-undefined
"${CC}" "${CFLAGS[@]}" "${ROOT}/tests/sdl_desktop_context_test.c" \
  -o "${DIR}/context_test" -ldl
# Do not let user overrides make a falsely advertised GL version pass.
unset MESA_GL_VERSION_OVERRIDE MESA_GLES_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
unset FCL_SHIM_GALLIUM FCL_SHIM_RENDERER GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
for mode in desktop native-es other-renderer other-library; do
  "${DIR}/context_test" "${DIR}/libGLESv2_mesa.so" "${mode}"
done
