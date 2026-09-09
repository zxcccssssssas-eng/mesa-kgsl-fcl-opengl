/*
 * Loaded first via FCL DLOPEN so Gallium Freedreno sees KGSL before
 * libEGL_mesa / libgallium_dri create a device.
 */
#include <stdlib.h>

__attribute__((constructor))
static void pin_kgsl_env(void) {
    setenv("GALLIUM_DRIVER", "freedreno", 0);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "kgsl", 0);
    setenv("FD_FORCE_KGSL", "1", 0);
}
