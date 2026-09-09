/*
 * OSMBridge — FCL/Pojav OSMesa trampoline for Mesa Gallium Freedreno/KGSL.
 *
 * FoldCraftLauncher loads this library as the renderer GL lib
 * (renderer = FreedrenoKGSL:libOSMBridge.so:libEGL.so) and then uses the
 * OSMesa entry points. We dlopen the real Mesa libOSMesa.so (MESA_LIBRARY)
 * and forward every symbol, after pinning KGSL Freedreno env vars.
 *
 * This is original code (not copied from FCL-Mesa-Plugin). The ABI it
 * implements matches Vera-Firefly's FCL Mesa plugin + FCLRendererPlugin.
 */

#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "FreedrenoKGSL"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EXPORT __attribute__((visibility("default"), used))

static void *mesa_handle;

static void setenv_default(const char *key, const char *value) {
    /* Do not clobber values FCL already injected from boatEnv/pojavEnv. */
    setenv(key, value, 0);
}

static void pin_kgsl_env(void) {
    setenv_default("GALLIUM_DRIVER", "freedreno");
    setenv_default("MESA_LOADER_DRIVER_OVERRIDE", "kgsl");
    setenv_default("FD_FORCE_KGSL", "1");
    setenv_default("MESA_GL_VERSION_OVERRIDE", "4.6");
    setenv_default("MESA_GLSL_VERSION_OVERRIDE", "460");
    setenv_default("mesa_glthread", "true");
}

static void *load_mesa(void) {
    if (mesa_handle) {
        return mesa_handle;
    }
    const char *lib = getenv("MESA_LIBRARY");
    if (!lib || !lib[0]) {
        lib = "libOSMesa.so";
    }
    mesa_handle = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!mesa_handle) {
        ALOGE("dlopen(%s) failed: %s", lib, dlerror());
    } else {
        ALOGI("loaded Mesa library %s", lib);
        const char *driver = getenv("GALLIUM_DRIVER");
        const char *override = getenv("MESA_LOADER_DRIVER_OVERRIDE");
        ALOGI("GALLIUM_DRIVER=%s MESA_LOADER_DRIVER_OVERRIDE=%s",
              driver ? driver : "(unset)",
              override ? override : "(unset)");
    }
    return mesa_handle;
}

__attribute__((constructor))
static void osmbridge_init(void) {
    pin_kgsl_env();
    load_mesa();
}

static void *mesa_sym(const char *name) {
    void *h = load_mesa();
    if (!h) {
        return NULL;
    }
    return dlsym(h, name);
}

EXPORT void *OSMesaGetProcAddress(const char *funcName) {
    void *(*fn)(const char *) = mesa_sym("OSMesaGetProcAddress");
    if (fn) {
        void *p = fn(funcName);
        if (p) {
            return p;
        }
    }
    return mesa_sym(funcName);
}

EXPORT void *OSMesaCreateContext(unsigned format, void *sharelist) {
    void *(*fn)(unsigned, void *) = mesa_sym("OSMesaCreateContext");
    return fn ? fn(format, sharelist) : NULL;
}

EXPORT void *OSMesaCreateContextExt(unsigned format, int depthBits, int stencilBits,
                                    int accumBits, void *sharelist) {
    void *(*fn)(unsigned, int, int, int, void *) = mesa_sym("OSMesaCreateContextExt");
    return fn ? fn(format, depthBits, stencilBits, accumBits, sharelist) : NULL;
}

EXPORT void *OSMesaCreateContextAttribs(const int *attribList, void *sharelist) {
    void *(*fn)(const int *, void *) = mesa_sym("OSMesaCreateContextAttribs");
    return fn ? fn(attribList, sharelist) : NULL;
}

EXPORT void OSMesaDestroyContext(void *ctx) {
    void (*fn)(void *) = mesa_sym("OSMesaDestroyContext");
    if (fn) {
        fn(ctx);
    }
}

EXPORT unsigned char OSMesaMakeCurrent(void *ctx, void *buffer, unsigned type,
                                       int width, int height) {
    unsigned char (*fn)(void *, void *, unsigned, int, int) =
        mesa_sym("OSMesaMakeCurrent");
    return fn ? fn(ctx, buffer, type, width, height) : 0;
}

EXPORT void *OSMesaGetCurrentContext(void) {
    void *(*fn)(void) = mesa_sym("OSMesaGetCurrentContext");
    return fn ? fn() : NULL;
}

EXPORT void OSMesaPixelStore(int pname, int value) {
    void (*fn)(int, int) = mesa_sym("OSMesaPixelStore");
    if (fn) {
        fn(pname, value);
    }
}

EXPORT void OSMesaGetIntegerv(int pname, int *value) {
    void (*fn)(int, int *) = mesa_sym("OSMesaGetIntegerv");
    if (fn) {
        fn(pname, value);
    }
}

EXPORT void OSMesaGetDepthBuffer(void *ctx, int *width, int *height, int *bytesPerValue,
                                 void **buffer) {
    void (*fn)(void *, int *, int *, int *, void **) = mesa_sym("OSMesaGetDepthBuffer");
    if (fn) {
        fn(ctx, width, height, bytesPerValue, buffer);
    }
}

EXPORT void OSMesaGetColorBuffer(void *ctx, int *width, int *height, int *format,
                                 void **buffer) {
    void (*fn)(void *, int *, int *, int *, void **) = mesa_sym("OSMesaGetColorBuffer");
    if (fn) {
        fn(ctx, width, height, format, buffer);
    }
}

EXPORT void OSMesaFlushFrontbuffer(void) {
    void (*fn)(void) = mesa_sym("OSMesaFlushFrontbuffer");
    if (fn) {
        fn();
    }
}

EXPORT const unsigned char *glGetString(unsigned name) {
    const unsigned char *(*fn)(unsigned) = mesa_sym("glGetString");
    return fn ? fn(name) : NULL;
}

EXPORT void glFinish(void) {
    void (*fn)(void) = mesa_sym("glFinish");
    if (fn) {
        fn();
    }
}

EXPORT void glFlush(void) {
    void (*fn)(void) = mesa_sym("glFlush");
    if (fn) {
        fn();
    }
}

EXPORT void glClearColor(float r, float g, float b, float a) {
    void (*fn)(float, float, float, float) = mesa_sym("glClearColor");
    if (fn) {
        fn(r, g, b, a);
    }
}

EXPORT void glClear(unsigned mask) {
    void (*fn)(unsigned) = mesa_sym("glClear");
    if (fn) {
        fn(mask);
    }
}

EXPORT void glReadPixels(int x, int y, int width, int height, unsigned format,
                         unsigned type, void *data) {
    void (*fn)(int, int, int, int, unsigned, unsigned, void *) = mesa_sym("glReadPixels");
    if (fn) {
        fn(x, y, width, height, format, type, data);
    }
}

EXPORT void glReadBuffer(unsigned mode) {
    void (*fn)(unsigned) = mesa_sym("glReadBuffer");
    if (fn) {
        fn(mode);
    }
}

EXPORT void glViewport(int x, int y, int width, int height) {
    void (*fn)(int, int, int, int) = mesa_sym("glViewport");
    if (fn) {
        fn(x, y, width, height);
    }
}
