/* Host check: libGLESv2_mesa.so exports SDL's EGL dlsym list and forwards
 * those calls to libEGL_mesa.so. glXGetProcAddress still uses Mesa core. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

struct fake_present_stats {
    int get_display;
    int initialize;
    int terminate;
    int get_proc;
    int choose_config;
    int create_context;
    int destroy_context;
    int create_pbuffer;
    int create_window;
    int destroy_surface;
    int make_current;
    int swap_buffers;
    int swap_interval;
    int query_string;
    int get_config_attrib;
    int wait_native;
    int wait_gl;
    int bind_api;
    int get_error;
    int destroy_image;
    int release_thread;
    int get_current_context;
    int get_current_surface;
    int get_current_display;
};

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s\n", #cond); \
            return 1; \
        } \
    } while (0)

static const char *required[] = {
    "eglGetDisplay", "eglInitialize", "eglTerminate", "eglGetProcAddress",
    "eglChooseConfig", "eglCreateContext", "eglDestroyContext",
    "eglCreatePbufferSurface", "eglCreateWindowSurface", "eglDestroySurface",
    "eglMakeCurrent", "eglSwapBuffers", "eglSwapInterval", "eglQueryString",
    "eglGetConfigAttrib", "eglWaitNative", "eglWaitGL", "eglBindAPI", "eglGetError",
    "glXGetProcAddress", "glXGetProcAddressARB", "OSMesaGetProcAddress",
};

static const char *optional[] = {
    "eglCreateImageKHR", "eglDestroyImageKHR", "eglReleaseThread",
    "eglGetCurrentContext", "eglGetCurrentSurface", "eglGetCurrentDisplay",
};

int main(int argc, char **argv)
{
    char path[4096];
    void *present, *core, *gles;
    struct fake_present_stats *stats;
    int *core_calls;
    void (*present_sentinel)(void);
    void (*core_sentinel)(void);
    EGLDisplay (*get_display)(EGLNativeDisplayType);
    EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*terminate)(EGLDisplay);
    __eglMustCastToProperFunctionPointerType (*egl_gpa)(const char *);
    __eglMustCastToProperFunctionPointerType (*glx_gpa)(const unsigned char *);
    __eglMustCastToProperFunctionPointerType (*glx_arb)(const unsigned char *);
    __eglMustCastToProperFunctionPointerType (*osmesa)(const char *);
    EGLBoolean (*choose)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*destroy_context)(EGLDisplay, EGLContext);
    EGLSurface (*create_pbuffer)(EGLDisplay, EGLConfig, const EGLint *);
    EGLSurface (*create_window)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    EGLBoolean (*destroy_surface)(EGLDisplay, EGLSurface);
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*swap_buffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*swap_interval)(EGLDisplay, EGLint);
    const char *(*query_string)(EGLDisplay, EGLint);
    EGLBoolean (*get_config_attrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    EGLBoolean (*wait_native)(EGLint);
    EGLBoolean (*wait_gl)(void);
    EGLBoolean (*bind_api)(EGLenum);
    EGLint (*get_error)(void);
    EGLImageKHR (*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (*destroy_image)(EGLDisplay, EGLImageKHR);
    EGLBoolean (*release_thread)(void);
    EGLContext (*get_current_context)(void);
    EGLSurface (*get_current_surface)(EGLint);
    EGLDisplay (*get_current_display)(void);
    EGLDisplay dpy;
    EGLint major = 0, minor = 0, n = -1, value = 0;
    void *via_egl, *via_glx, *compile, *multi;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s directory\n", argv[0]);
        return 2;
    }
#define OPEN(var, leaf) \
    do { \
        snprintf(path, sizeof(path), "%s/" leaf, argv[1]); \
        var = dlopen(path, RTLD_NOW | RTLD_LOCAL); \
        if (!var) { \
            fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); \
            return 1; \
        } \
    } while (0)
    OPEN(present, "libEGL_mesa.so");
    OPEN(core, "libEGL_mesa_core.so");
    OPEN(gles, "libGLESv2_mesa.so");
#undef OPEN

    stats = dlsym(present, "fake_present_stats");
    core_calls = dlsym(core, "fake_core_get_proc_calls");
    present_sentinel = dlsym(present, "fake_present_sentinel");
    core_sentinel = dlsym(core, "fake_core_sentinel");
    CHECK(stats && core_calls && present_sentinel && core_sentinel);

    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++)
        CHECK(dlsym(gles, required[i]) != NULL);
    for (i = 0; i < sizeof(optional) / sizeof(optional[0]); i++)
        CHECK(dlsym(gles, optional[i]) != NULL);

#define LOAD(var, name) \
    do { \
        var = dlsym(gles, name); \
        CHECK(var != NULL); \
    } while (0)
    LOAD(get_display, "eglGetDisplay");
    LOAD(initialize, "eglInitialize");
    LOAD(terminate, "eglTerminate");
    LOAD(egl_gpa, "eglGetProcAddress");
    LOAD(choose, "eglChooseConfig");
    LOAD(create_context, "eglCreateContext");
    LOAD(destroy_context, "eglDestroyContext");
    LOAD(create_pbuffer, "eglCreatePbufferSurface");
    LOAD(create_window, "eglCreateWindowSurface");
    LOAD(destroy_surface, "eglDestroySurface");
    LOAD(make_current, "eglMakeCurrent");
    LOAD(swap_buffers, "eglSwapBuffers");
    LOAD(swap_interval, "eglSwapInterval");
    LOAD(query_string, "eglQueryString");
    LOAD(get_config_attrib, "eglGetConfigAttrib");
    LOAD(wait_native, "eglWaitNative");
    LOAD(wait_gl, "eglWaitGL");
    LOAD(bind_api, "eglBindAPI");
    LOAD(get_error, "eglGetError");
    LOAD(create_image, "eglCreateImageKHR");
    LOAD(destroy_image, "eglDestroyImageKHR");
    LOAD(release_thread, "eglReleaseThread");
    LOAD(get_current_context, "eglGetCurrentContext");
    LOAD(get_current_surface, "eglGetCurrentSurface");
    LOAD(get_current_display, "eglGetCurrentDisplay");
    LOAD(glx_gpa, "glXGetProcAddress");
    LOAD(glx_arb, "glXGetProcAddressARB");
    LOAD(osmesa, "OSMesaGetProcAddress");
#undef LOAD

    dpy = get_display(EGL_DEFAULT_DISPLAY);
    CHECK(dpy == (EGLDisplay)(uintptr_t)0x42);
    CHECK(stats->get_display == 1);
    CHECK(initialize(dpy, &major, &minor) == EGL_TRUE && major == 1 && minor == 5);
    CHECK(terminate(dpy) == EGL_TRUE);
    CHECK(choose(dpy, NULL, NULL, 0, &n) == EGL_TRUE && n == 3);
    CHECK(create_context(dpy, NULL, EGL_NO_CONTEXT, NULL) == (EGLContext)(uintptr_t)0x43);
    CHECK(destroy_context(dpy, (EGLContext)(uintptr_t)0x43) == EGL_TRUE);
    CHECK(create_pbuffer(dpy, NULL, NULL) == (EGLSurface)(uintptr_t)0x44);
    CHECK(create_window(dpy, NULL, NULL, NULL) == (EGLSurface)(uintptr_t)0x45);
    CHECK(destroy_surface(dpy, (EGLSurface)(uintptr_t)0x45) == EGL_TRUE);
    CHECK(make_current(dpy, NULL, NULL, NULL) == EGL_TRUE);
    CHECK(swap_buffers(dpy, NULL) == EGL_TRUE);
    CHECK(swap_interval(dpy, 1) == EGL_TRUE);
    CHECK(strcmp(query_string(dpy, 0), "fake-egl") == 0);
    CHECK(get_config_attrib(dpy, NULL, 0, &value) == EGL_TRUE && value == 8);
    CHECK(wait_native(0) == EGL_TRUE);
    CHECK(wait_gl() == EGL_TRUE);
    CHECK(bind_api(EGL_OPENGL_API) == EGL_TRUE);
    CHECK(get_error() == EGL_SUCCESS);
    /* Optional and absent from the presentation shim: still exported, and safe. */
    CHECK(create_image(dpy, EGL_NO_CONTEXT, 0, NULL, NULL) == EGL_NO_IMAGE_KHR);
    CHECK(destroy_image(dpy, EGL_NO_IMAGE_KHR) == EGL_TRUE);
    CHECK(release_thread() == EGL_TRUE);
    CHECK(get_current_context() == (EGLContext)(uintptr_t)0x46);
    CHECK(get_current_surface(EGL_READ) == (EGLSurface)(uintptr_t)0x47);
    CHECK(get_current_display() == (EGLDisplay)(uintptr_t)0x48);
    CHECK(stats->choose_config == 1);
    CHECK(stats->wait_gl == 1 && stats->wait_native == 1);
    CHECK(stats->get_proc == 0);
    CHECK(*core_calls == 0);

    via_egl = (void *)egl_gpa("glGetString");
    CHECK(via_egl == (void *)present_sentinel);
    CHECK(stats->get_proc == 1);
    CHECK(*core_calls == 0);

    via_glx = (void *)glx_gpa((const unsigned char *)"glGetString");
    CHECK(via_glx == (void *)core_sentinel);
    CHECK(*core_calls == 1);
    CHECK(stats->get_proc == 1);
    CHECK(via_egl != via_glx);

    compile = (void *)glx_gpa((const unsigned char *)"glCompileShader");
    multi = (void *)glx_gpa((const unsigned char *)"glMultiDrawElementsIndirect");
    CHECK(compile != NULL && multi != NULL);
    CHECK(compile != (void *)core_sentinel && compile != (void *)present_sentinel);
    CHECK(multi != (void *)core_sentinel && multi != compile);
    CHECK(*core_calls == 1);

    CHECK((void *)glx_arb((const unsigned char *)"glGetString") == (void *)core_sentinel);
    CHECK((void *)osmesa("glGetString") == (void *)core_sentinel);
    CHECK(*core_calls == 3);

    puts("PASS: GLES shim EGL forwarders, presentation split, Flywheel glX wrappers");
    return 0;
}
