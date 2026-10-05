// SPDX-License-Identifier: MIT
/* Exercise both production shims against real Mesa, using a pbuffer in place
 * of an Android window. FCL's SDL hooks force ES and normalize the config to
 * ES2 even for desktop GL. The resulting context must still be desktop GL. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    int desktop = !strcmp(argv[2], "desktop");
    CHECK(setenv("POJAV_RENDERER", "opengles3_desktopgl", 1) == 0);
    CHECK(setenv("SDL_OPENGL_LIBRARY", argv[1], 1) == 0);
    if (!strcmp(argv[2], "native-es")) {
        unsetenv("SDL_OPENGL_LIBRARY");
    } else if (!strcmp(argv[2], "other-renderer")) {
        CHECK(setenv("POJAV_RENDERER", "opengles3", 1) == 0);
    } else if (!strcmp(argv[2], "other-library")) {
        CHECK(setenv("SDL_OPENGL_LIBRARY", "/other/libGLESv2_mesa.so.backup", 1) == 0);
    } else {
        CHECK(desktop);
    }

    void *gl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!gl) fprintf(stderr, "%s\n", dlerror());
    CHECK(gl);
#define LOAD(result, name, args) \
    result (*name) args = (void *)dlsym(gl, #name); CHECK(name)
    LOAD(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType));
    LOAD(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *));
    LOAD(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *));
    LOAD(EGLBoolean, eglBindAPI, (EGLenum));
    LOAD(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *));
    LOAD(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *));
    LOAD(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
    LOAD(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext));
    LOAD(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface));
    LOAD(EGLBoolean, eglTerminate, (EGLDisplay));
    LOAD(__eglMustCastToProperFunctionPointerType, eglGetProcAddress, (const char *));
    LOAD(__eglMustCastToProperFunctionPointerType, glXGetProcAddress, (const unsigned char *));
#undef LOAD

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(display != EGL_NO_DISPLAY);
    CHECK(eglInitialize(display, NULL, NULL));
    /* Equivalent to FCL's normalized SDL request, except PBUFFER instead of
     * WINDOW so the same production libraries can run without Android UI. */
    const EGLint config_attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE };
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    CHECK(eglChooseConfig(display, config_attrs, &config, 1, &count) && count == 1);
    const EGLint surface_attrs[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    CHECK(surface != EGL_NO_SURFACE);
    /* FCL preserves CLIENT_VERSION (also the EGL major-version attribute).
     * Request desktop GL 4, never accept its fallback to an ES2 context. */
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, desktop ? 4 : 3, EGL_NONE };
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    CHECK(context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, surface, surface, context));

    const GLubyte *(*get_string)(GLenum) = (void *)eglGetProcAddress("glGetString");
    const GLubyte *(*glx_string)(GLenum) = (void *)glXGetProcAddress((const unsigned char *)"glGetString");
    void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat) = (void *)eglGetProcAddress("glClearColor");
    void (*clear)(GLbitfield) = (void *)eglGetProcAddress("glClear");
    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) =
        (void *)glXGetProcAddress((const unsigned char *)"glReadPixels");
    GLenum (*get_error)(void) = (void *)eglGetProcAddress("glGetError");
    CHECK(get_string && glx_string && clear_color && clear && read_pixels && get_error);
    const char *version = (const char *)get_string(GL_VERSION);
    CHECK(version && !strcmp(version, (const char *)glx_string(GL_VERSION)));
    if (desktop) {
        int major = 0;
        CHECK(strncmp(version, "OpenGL ES", 9) != 0);
        CHECK(sscanf(version, "%d", &major) == 1 && major >= 4);
    } else {
        CHECK(strncmp(version, "OpenGL ES 3", 11) == 0);
    }
    clear_color(1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    GLubyte pixel[4] = {0};
    read_pixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    CHECK(get_error() == 0);
    CHECK(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
    printf("PASS: %s: %s; EGL/glX dispatch and pixel readback agree\n", argv[2], version);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context));
    CHECK(eglDestroySurface(display, surface));
    CHECK(eglTerminate(display));
    return 0;
}
