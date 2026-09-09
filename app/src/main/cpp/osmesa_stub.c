/*
 * No-op libOSMesa.so so metadata-only APKs still contain the library name
 * FoldCraftLauncher expects (MESA_LIBRARY=libOSMesa.so). Replaced in CI by
 * a real Mesa Gallium OSMesa built with -Dfreedreno-kmds=kgsl.
 */

#define EXPORT __attribute__((visibility("default"), used))

EXPORT void *OSMesaGetProcAddress(const char *funcName) {
    (void)funcName;
    return 0;
}

EXPORT void *OSMesaCreateContext(unsigned format, void *sharelist) {
    (void)format;
    (void)sharelist;
    return 0;
}

EXPORT void *OSMesaCreateContextExt(unsigned format, int depthBits, int stencilBits,
                                    int accumBits, void *sharelist) {
    (void)format;
    (void)depthBits;
    (void)stencilBits;
    (void)accumBits;
    (void)sharelist;
    return 0;
}

EXPORT void *OSMesaCreateContextAttribs(const int *attribList, void *sharelist) {
    (void)attribList;
    (void)sharelist;
    return 0;
}

EXPORT void OSMesaDestroyContext(void *ctx) { (void)ctx; }

EXPORT unsigned char OSMesaMakeCurrent(void *ctx, void *buffer, unsigned type,
                                       int width, int height) {
    (void)ctx;
    (void)buffer;
    (void)type;
    (void)width;
    (void)height;
    return 0;
}

EXPORT void *OSMesaGetCurrentContext(void) { return 0; }

EXPORT void OSMesaPixelStore(int pname, int value) {
    (void)pname;
    (void)value;
}

EXPORT void OSMesaGetIntegerv(int pname, int *value) {
    (void)pname;
    (void)value;
}

EXPORT void OSMesaFlushFrontbuffer(void) {}

EXPORT const unsigned char *glGetString(unsigned name) {
    (void)name;
    return 0;
}

EXPORT void glFinish(void) {}
EXPORT void glFlush(void) {}

EXPORT void glClearColor(float r, float g, float b, float a) {
    (void)r;
    (void)g;
    (void)b;
    (void)a;
}

EXPORT void glClear(unsigned mask) { (void)mask; }

EXPORT void glReadPixels(int x, int y, int width, int height, unsigned format,
                         unsigned type, void *data) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    (void)data;
}

EXPORT void glReadBuffer(unsigned mode) { (void)mode; }

EXPORT void glViewport(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}
