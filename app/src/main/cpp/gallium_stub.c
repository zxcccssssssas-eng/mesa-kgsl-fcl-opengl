/*
 * Placeholder libgallium_dri.so so stub APKs contain the DSO name Mesa 26
 * installs on Android (with_platform_android → unversioned gallium_dri).
 */
#define EXPORT __attribute__((visibility("default"), used))

EXPORT void fcl_gallium_dri_stub(void) {}
