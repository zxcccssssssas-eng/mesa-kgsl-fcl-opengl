/*
 * Placeholder libGLESv2_mesa.so for metadata-only APKs. Replaced in CI by
 * Mesa GLES built with -Dgles-lib-suffix=_mesa.
 */
#define EXPORT __attribute__((visibility("default"), used))

EXPORT const unsigned char *glGetString(unsigned name) {
    (void)name;
    return 0;
}

EXPORT void *glGetProcAddress(const char *name) {
    (void)name;
    return 0;
}

EXPORT void glFinish(void) {}
EXPORT void glFlush(void) {}
