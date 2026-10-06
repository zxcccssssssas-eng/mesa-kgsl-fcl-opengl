#!/usr/bin/env bash
# Fail if a jniLibs directory or plugin APK does not contain a real Mesa
# payload. Tiny CMake placeholders (~6-8KiB, no *_core.so) must not ship.
set -euo pipefail

if [[ "${1:-}" == "" ]]; then
  echo "usage: $0 <jniLibs-dir-or-apk>" >&2
  exit 2
fi

python3 - "$1" <<'PY'
import os
import sys
import zipfile

# Floor values sit between CMake stubs and stripped Mesa/shim DSOs.
# Real sizes from a main CI APK (Sept 2026):
#   libEGL_mesa_core.so      455784
#   libGLESv2_mesa_core.so    88176
#   libgallium_dri.so      24393952
#   libEGL_mesa.so (shim)    322032
#   libGLESv2_mesa.so (shim)  30408
# Stub APK (PR CI before this check):
#   libEGL_mesa.so             7536  (no _core)
#   libGLESv2_mesa.so          7144
#   libgallium_dri.so          6232
CORES = {
    "libEGL_mesa_core.so": 200_000,
    "libGLESv2_mesa_core.so": 40_000,
    "libgallium_dri.so": 1_000_000,
}
APK_EXTRAS = {
    "libEGL_mesa.so": 50_000,
    "libGLESv2_mesa.so": 20_000,
    "libfreedreno_kgsl_init.so": 8_000,
}


def fail(msg: str) -> None:
    print(f"FAIL {msg}", file=sys.stderr)
    sys.exit(1)


def ok(name: str, size: int) -> None:
    print(f"ok  {name} ({size} bytes)")


def check(name: str, size: int, minimum: int) -> None:
    if size < minimum:
        fail(
            f"{name} is {size} bytes (need >= {minimum}); "
            "this is a stub/empty payload, not Mesa"
        )
    ok(name, size)


target = sys.argv[1]
if os.path.isdir(target):
    for name, minimum in CORES.items():
        path = os.path.join(target, name)
        if not os.path.isfile(path):
            fail(f"missing {path}")
        check(name, os.path.getsize(path), minimum)
elif os.path.isfile(target):
    with zipfile.ZipFile(target) as archive:
        infos = {
            os.path.basename(info.filename): info
            for info in archive.infolist()
            if info.filename.startswith("lib/arm64-v8a/")
        }
    required = dict(CORES)
    required.update(APK_EXTRAS)
    for name, minimum in required.items():
        info = infos.get(name)
        if info is None:
            fail(f"APK missing lib/arm64-v8a/{name}")
        check(name, info.file_size, minimum)
else:
    fail(f"not a directory or APK: {target}")

print("real Mesa payload ok")
PY
