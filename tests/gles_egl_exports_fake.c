/* Stand-in for libEGL_mesa.so or libEGL_mesa_core.so in the host export test. */
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifdef FAKE_CORE

int fake_core_get_proc_calls;
void fake_core_sentinel(void) {}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    (void)procname;
    fake_core_get_proc_calls++;
    return fake_core_sentinel;
}

#else

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
struct fake_present_stats fake_present_stats;
void fake_present_sentinel(void) {}

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
    (void)display_id;
    fake_present_stats.get_display++;
    return (EGLDisplay)(uintptr_t)0x42;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 5;
    fake_present_stats.initialize++;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    (void)dpy;
    fake_present_stats.terminate++;
    return EGL_TRUE;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    (void)procname;
    fake_present_stats.get_proc++;
    return fake_present_sentinel;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                           EGLint config_size, EGLint *num_config)
{
    (void)dpy;
    (void)attrib_list;
    (void)configs;
    (void)config_size;
    if (num_config) *num_config = 3;
    fake_present_stats.choose_config++;
    return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                            const EGLint *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)share_context;
    (void)attrib_list;
    fake_present_stats.create_context++;
    return (EGLContext)(uintptr_t)0x43;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    (void)dpy;
    (void)ctx;
    fake_present_stats.destroy_context++;
    return EGL_TRUE;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)attrib_list;
    fake_present_stats.create_pbuffer++;
    return (EGLSurface)(uintptr_t)0x44;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                  const EGLint *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)win;
    (void)attrib_list;
    fake_present_stats.create_window++;
    return (EGLSurface)(uintptr_t)0x45;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy;
    (void)surface;
    fake_present_stats.destroy_surface++;
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    (void)dpy;
    (void)draw;
    (void)read;
    (void)ctx;
    fake_present_stats.make_current++;
    return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy;
    (void)surface;
    fake_present_stats.swap_buffers++;
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    (void)dpy;
    (void)interval;
    fake_present_stats.swap_interval++;
    return EGL_TRUE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    (void)dpy;
    (void)name;
    fake_present_stats.query_string++;
    return "fake-egl";
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
    (void)dpy;
    (void)config;
    (void)attribute;
    if (value) *value = 8;
    fake_present_stats.get_config_attrib++;
    return EGL_TRUE;
}

EGLBoolean eglWaitNative(EGLint engine)
{
    (void)engine;
    fake_present_stats.wait_native++;
    return EGL_TRUE;
}

EGLBoolean eglWaitGL(void)
{
    fake_present_stats.wait_gl++;
    return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api)
{
    (void)api;
    fake_present_stats.bind_api++;
    return EGL_TRUE;
}

EGLint eglGetError(void)
{
    fake_present_stats.get_error++;
    return EGL_SUCCESS;
}

EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image)
{
    (void)dpy;
    (void)image;
    fake_present_stats.destroy_image++;
    return EGL_TRUE;
}

EGLBoolean eglReleaseThread(void)
{
    fake_present_stats.release_thread++;
    return EGL_TRUE;
}

EGLContext eglGetCurrentContext(void)
{
    fake_present_stats.get_current_context++;
    return (EGLContext)(uintptr_t)0x46;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw)
{
    (void)readdraw;
    fake_present_stats.get_current_surface++;
    return (EGLSurface)(uintptr_t)0x47;
}

EGLDisplay eglGetCurrentDisplay(void)
{
    fake_present_stats.get_current_display++;
    return (EGLDisplay)(uintptr_t)0x48;
}

#endif
