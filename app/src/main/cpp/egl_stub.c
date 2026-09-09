/*
 * Placeholder libEGL_mesa.so for metadata-only APKs. Replaced in CI by
 * Mesa EGL built with -Degl-lib-suffix=_mesa -Dfreedreno-kmds=kgsl.
 */
#define EXPORT __attribute__((visibility("default"), used))

typedef void *EGLDisplay;
typedef void *EGLBoolean;
typedef int EGLint;

EXPORT EGLDisplay eglGetDisplay(void *native_display) {
    (void)native_display;
    return 0;
}

EXPORT void *eglGetProcAddress(const char *procname) {
    (void)procname;
    return 0;
}

EXPORT EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    (void)dpy;
    (void)major;
    (void)minor;
    return 0;
}

EXPORT EGLBoolean eglBindAPI(unsigned api) {
    (void)api;
    return 0;
}

EXPORT EGLBoolean eglTerminate(EGLDisplay dpy) {
    (void)dpy;
    return 0;
}
