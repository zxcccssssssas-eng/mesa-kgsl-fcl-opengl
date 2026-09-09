#!/usr/bin/env bash
# Pack a Turnip ICD as an AdrenoTools-style zip for FCL driver import.
# Format (K11MCH1 / whitebelyash / WinNative): meta.json + libvulkan_freedreno.so
# with a libraryName field.
set -euo pipefail

SO="${1:?usage: package-adrenotools-zip.sh libvulkan_freedreno.so [out.zip]}"
OUT="${2:-turnip-freedreno-kgsl-adrenotools.zip}"

[[ -f "${SO}" ]] || { echo "missing ${SO}" >&2; exit 1; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT
cp -L "${SO}" "${WORKDIR}/libvulkan_freedreno.so"

cat >"${WORKDIR}/meta.json" <<'EOF'
{
  "schemaVersion": 1,
  "name": "Mesa Turnip Freedreno KGSL",
  "description": "Mesa Vulkan Freedreno (Turnip) built with -Dfreedreno-kmds=kgsl for Adreno 6xx/7xx/8xx. Optional companion to the FCL Freedreno KGSL OpenGL renderer (Zink path).",
  "author": "mesa-kgsl-fcl-opengl",
  "packageVersion": "1",
  "vendor": "Mesa",
  "driverVersion": "Turnip (Freedreno KGSL)",
  "minApi": 26,
  "libraryName": "libvulkan_freedreno.so"
}
EOF

mkdir -p "$(dirname "${OUT}")"
(
  cd "${WORKDIR}"
  zip -9 -X "${OUT}" libvulkan_freedreno.so meta.json
)
echo "wrote ${OUT}"
