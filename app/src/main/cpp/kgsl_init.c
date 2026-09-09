/*
 * Loaded first via FCL DLOPEN so Gallium Freedreno sees KGSL before
 * libEGL_mesa / libgallium_dri create a device.
 *
 * It also carries a small one-shot diagnostic (FCLProbe) that checks whether
 * the vendor EGL can import an AHardwareBuffer as an EGLImage. That is the
 * zero-copy path the planned EGL presentation shim needs:
 *
 *   Mesa (freedreno/kgsl) renders into an AHB  ->  vendor GLES samples it via
 *   eglGetNativeClientBufferANDROID + eglCreateImageKHR and presents it with a
 *   fullscreen triangle (same idea as GameNative's BlitConverter).
 *
 * The probe never aborts: every step is checked and the result is logged with
 * the tag "FCLProbe" (visible in logcat / FCL logs).
 */
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define PROBE_TAG "FCLProbe"
#define PROBE_LOG(...) __android_log_print(ANDROID_LOG_INFO, PROBE_TAG, __VA_ARGS__)
#define PROBE_ERR(...) __android_log_print(ANDROID_LOG_ERROR, PROBE_TAG, __VA_ARGS__)

__attribute__((constructor))
static void pin_kgsl_env(void) {
    setenv("GALLIUM_DRIVER", "freedreno", 0);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "kgsl", 0);
    setenv("FD_FORCE_KGSL", "1", 0);
}

struct vendor_egl {
    void *handle;
    EGLDisplay (*get_display)(EGLNativeDisplayType);
    EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*bind_api)(EGLenum);
    EGLBoolean (*choose_config)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLSurface (*create_pbuffer_surface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*destroy_context)(EGLDisplay, EGLContext);
    EGLBoolean (*destroy_surface)(EGLDisplay, EGLSurface);
    EGLBoolean (*terminate)(EGLDisplay);
    __eglMustCastToProperFunctionPointerType (*get_proc_address)(const char *);
    EGLint (*get_error)(void);
    EGLImageKHR (*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLClientBuffer (*get_native_client_buffer)(AHardwareBuffer *);
    EGLBoolean (*destroy_image)(EGLDisplay, EGLImageKHR);
};

static int vendor_egl_load(struct vendor_egl *v) {
    memset(v, 0, sizeof(*v));
    v->handle = dlopen("/system/lib64/libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    if (!v->handle) {
        v->handle = dlopen("libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    }
    if (!v->handle) {
        PROBE_ERR("vendor libEGL dlopen failed: %s", dlerror());
        return 0;
    }
    v->get_display = (void *)dlsym(v->handle, "eglGetDisplay");
    v->initialize = (void *)dlsym(v->handle, "eglInitialize");
    v->bind_api = (void *)dlsym(v->handle, "eglBindAPI");
    v->choose_config = (void *)dlsym(v->handle, "eglChooseConfig");
    v->create_context = (void *)dlsym(v->handle, "eglCreateContext");
    v->create_pbuffer_surface = (void *)dlsym(v->handle, "eglCreatePbufferSurface");
    v->make_current = (void *)dlsym(v->handle, "eglMakeCurrent");
    v->destroy_context = (void *)dlsym(v->handle, "eglDestroyContext");
    v->destroy_surface = (void *)dlsym(v->handle, "eglDestroySurface");
    v->terminate = (void *)dlsym(v->handle, "eglTerminate");
    v->get_proc_address = (void *)dlsym(v->handle, "eglGetProcAddress");
    v->get_error = (void *)dlsym(v->handle, "eglGetError");
    if (!v->get_display || !v->initialize || !v->get_proc_address || !v->get_error) {
        PROBE_ERR("vendor libEGL is missing core entry points");
        return 0;
    }
    v->create_image = (void *)v->get_proc_address("eglCreateImageKHR");
    v->get_native_client_buffer = (void *)v->get_proc_address("eglGetNativeClientBufferANDROID");
    v->destroy_image = (void *)v->get_proc_address("eglDestroyImageKHR");
    PROBE_LOG("vendor EGL loaded: createImageKHR=%p getNativeClientBufferANDROID=%p",
              (void *)v->create_image, (void *)v->get_native_client_buffer);
    return 1;
}

static void probe_ahb_import(void) {
    struct vendor_egl v;
    AHardwareBuffer *ahb = NULL;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    int ok = 0;

    if (!vendor_egl_load(&v)) return;
    if (!v.create_image || !v.get_native_client_buffer) {
        PROBE_ERR("vendor EGL lacks EGL_ANDROID_get_native_client_buffer / EGL_KHR_image_base");
        return;
    }

    AHardwareBuffer_Desc desc = {
        .width = 64,
        .height = 64,
        .layers = 1,
        /* NDK public header exposes R8G8B8A8_UNORM; B8G8R8A8 (5) is HAL-only. */
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
    };
    int rc = AHardwareBuffer_allocate(&desc, &ahb);
    if (rc != 0 || !ahb) {
        PROBE_ERR("AHardwareBuffer_allocate failed: %d", rc);
        return;
    }
    AHardwareBuffer_Desc out;
    AHardwareBuffer_describe(ahb, &out);
    PROBE_LOG("AHB allocated: %ux%u stride=%u format=%u usage=0x%llx",
              out.width, out.height, out.stride, out.format,
              (unsigned long long)out.usage);

    dpy = v.get_display(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || v.initialize(dpy, NULL, NULL) != EGL_TRUE) {
        PROBE_ERR("vendor eglInitialize failed: display=%p err=0x%x",
                  (void *)dpy, v.get_error());
        goto out;
    }

    EGLClientBuffer client = v.get_native_client_buffer(ahb);
    EGLint attribs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };

    /* First attempt: no current context (the EGL_ANDROID_image_native_buffer
     * contract wants EGL_NO_CONTEXT). */
    img = v.create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attribs);
    PROBE_LOG("import attempt 1 (no context): client=%p image=%p err=0x%x",
              (void *)client, (void *)img, v.get_error());
    if (img != EGL_NO_IMAGE_KHR) { ok = 1; goto out; }

    /* Second attempt: with a throw-away ES2 pbuffer context current. */
    if (!v.bind_api || !v.choose_config || !v.create_context ||
        !v.create_pbuffer_surface || !v.make_current) {
        PROBE_ERR("vendor EGL lacks context entry points");
        goto out;
    }
    v.bind_api(EGL_OPENGL_ES_API);
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    if (v.choose_config(dpy, cfg_attribs, &cfg, 1, &ncfg) != EGL_TRUE || ncfg == 0) {
        PROBE_ERR("vendor eglChooseConfig failed: err=0x%x ncfg=%d", v.get_error(), ncfg);
        goto out;
    }
    EGLint pb_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    surf = v.create_pbuffer_surface(dpy, cfg, pb_attribs);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx = v.create_context(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        v.make_current(dpy, surf, surf, ctx) != EGL_TRUE) {
        PROBE_ERR("vendor context setup failed: surf=%p ctx=%p err=0x%x",
                  (void *)surf, (void *)ctx, v.get_error());
        goto out;
    }
    img = v.create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attribs);
    PROBE_LOG("import attempt 2 (ES2 context current): image=%p err=0x%x",
              (void *)img, v.get_error());
    if (img != EGL_NO_IMAGE_KHR) ok = 1;

out:
    PROBE_LOG("RESULT: vendor AHB->EGLImage %s", ok ? "OK (zero-copy possible)" : "FAILED (use PBO fallback)");
    if (img != EGL_NO_IMAGE_KHR && v.destroy_image) v.destroy_image(dpy, img);
    if (ctx != EGL_NO_CONTEXT && v.destroy_context) {
        v.make_current(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        v.destroy_context(dpy, ctx);
    }
    if (surf != EGL_NO_SURFACE && v.destroy_surface) v.destroy_surface(dpy, surf);
    /* Leave the display initialized: eglGetDisplay() returns a process-wide
     * singleton, and FCL initializes it again for the real renderer. */
    if (ahb) AHardwareBuffer_release(ahb);
}

__attribute__((constructor))
static void run_probe_once(void) {
    /* Opt-out for release builds: FCL_PROBE=0 */
    const char *enabled = getenv("FCL_PROBE");
    if (enabled && enabled[0] == '0') return;
    probe_ahb_import();
}
