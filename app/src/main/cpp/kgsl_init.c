/*
 * Loaded first via FCL DLOPEN so Gallium Freedreno sees KGSL before
 * libEGL_mesa / libgallium_dri create a device.
 *
 * It also carries a one-shot diagnostic (FCLProbe) for the planned EGL
 * presentation shim. Mesa (freedreno/kgsl) renders offscreen and the vendor
 * GLES presents the frame, so the shim needs two things:
 *
 *   1. the vendor EGL must import our AHardwareBuffer as an EGLImage
 *      (eglGetNativeClientBufferANDROID + eglCreateImageKHR);
 *   2. Mesa must be able to render INTO that AHB (fake ANativeWindowBuffer +
 *      eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) + FBO), otherwise the shim
 *      has to fall back to a PBO readback.
 *
 * Both checks are logged with the tag "FCLProbe" (logcat). The probe never
 * aborts; it only logs. Opt-out with FCL_PROBE=0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <dlfcn.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define PROBE_TAG "FCLProbe"
#define PROBE_LOG(...) __android_log_print(ANDROID_LOG_INFO, PROBE_TAG, __VA_ARGS__)
#define PROBE_ERR(...) __android_log_print(ANDROID_LOG_ERROR, PROBE_TAG, __VA_ARGS__)

/* Mesa was built against the Android stub headers, whose ANativeWindowBuffer is
 * 168 bytes (magic 0x5f626672). The NDK does not expose the struct, so mirror
 * the layout here (magic/version/handle offsets match the platform header). */
#define ANDROID_NATIVE_BUFFER_MAGIC 0x5f626672u

typedef struct {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(void *base);
    void (*decRef)(void *base);
} probe_native_base_t;

typedef struct {
    probe_native_base_t common;
    int width;
    int height;
    int stride;
    int format;
    int usage;
    void *reserved[2];
    const void *handle;
    void *reserved_proc[8];
} probe_anwb_t;

static void probe_noop_ref(void *base) { (void)base; }

struct egl_api {
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
    __eglMustCastToProperFunctionPointerType (*get_proc_address)(const char *);
    EGLint (*get_error)(void);
    EGLImageKHR (*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLClientBuffer (*get_native_client_buffer)(AHardwareBuffer *);
    EGLBoolean (*destroy_image)(EGLDisplay, EGLImageKHR);
};

static int egl_api_load(struct egl_api *v, const char *path) {
    memset(v, 0, sizeof(*v));
    v->handle = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
    if (!v->handle) {
        PROBE_ERR("dlopen %s failed: %s", path, dlerror());
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
    v->get_proc_address = (void *)dlsym(v->handle, "eglGetProcAddress");
    v->get_error = (void *)dlsym(v->handle, "eglGetError");
    if (!v->get_display || !v->initialize || !v->get_proc_address || !v->get_error) {
        PROBE_ERR("%s is missing core EGL entry points", path);
        return 0;
    }
    v->create_image = (void *)v->get_proc_address("eglCreateImageKHR");
    v->get_native_client_buffer = (void *)v->get_proc_address("eglGetNativeClientBufferANDROID");
    v->destroy_image = (void *)v->get_proc_address("eglDestroyImageKHR");
    return 1;
}

static AHardwareBuffer *probe_alloc_ahb(AHardwareBuffer_Desc *out) {
    AHardwareBuffer_Desc desc = {
        .width = 64,
        .height = 64,
        .layers = 1,
        /* NDK public header exposes R8G8B8A8_UNORM; B8G8R8A8 (5) is HAL-only. */
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
    };
    AHardwareBuffer *ahb = NULL;
    int rc = AHardwareBuffer_allocate(&desc, &ahb);
    if (rc != 0 || !ahb) {
        PROBE_ERR("AHardwareBuffer_allocate failed: %d", rc);
        return NULL;
    }
    AHardwareBuffer_describe(ahb, out);
    return ahb;
}

/* 1. vendor EGL: AHB -> EGLImage (zero-copy presentation) */
static void probe_vendor_import(void) {
    struct egl_api v;
    AHardwareBuffer *ahb = NULL;
    AHardwareBuffer_Desc desc;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    int ok = 0;

    if (!egl_api_load(&v, "/system/lib64/libEGL.so")) {
        egl_api_load(&v, "libEGL.so");
    }
    if (!v.handle) return;
    if (!v.create_image || !v.get_native_client_buffer) {
        PROBE_ERR("vendor EGL lacks EGL_ANDROID_get_native_client_buffer / EGL_KHR_image_base");
        return;
    }

    ahb = probe_alloc_ahb(&desc);
    if (!ahb) return;
    PROBE_LOG("vendor: AHB %ux%u stride=%u format=%u usage=0x%llx",
              desc.width, desc.height, desc.stride, desc.format,
              (unsigned long long)desc.usage);

    dpy = v.get_display(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || v.initialize(dpy, NULL, NULL) != EGL_TRUE) {
        PROBE_ERR("vendor eglInitialize failed: display=%p err=0x%x", (void *)dpy, v.get_error());
        goto out;
    }

    EGLClientBuffer client = v.get_native_client_buffer(ahb);
    EGLint attribs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    img = v.create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attribs);
    PROBE_LOG("vendor: import(no ctx) client=%p image=%p err=0x%x",
              (void *)client, (void *)img, v.get_error());
    if (img != EGL_NO_IMAGE_KHR) { ok = 1; goto out; }

    if (!v.bind_api || !v.choose_config || !v.create_context ||
        !v.create_pbuffer_surface || !v.make_current) goto out;
    v.bind_api(EGL_OPENGL_ES_API);
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    if (v.choose_config(dpy, cfg_attribs, &cfg, 1, &ncfg) != EGL_TRUE || ncfg == 0) goto out;
    EGLint pb_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    surf = v.create_pbuffer_surface(dpy, cfg, pb_attribs);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx = v.create_context(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        v.make_current(dpy, surf, surf, ctx) != EGL_TRUE) goto out;
    img = v.create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attribs);
    PROBE_LOG("vendor: import(ES2 ctx) image=%p err=0x%x", (void *)img, v.get_error());
    if (img != EGL_NO_IMAGE_KHR) ok = 1;

out:
    PROBE_LOG("vendor AHB->EGLImage: %s", ok ? "OK (zero-copy)" : "FAILED (PBO fallback)");
    if (img != EGL_NO_IMAGE_KHR && v.destroy_image) v.destroy_image(dpy, img);
    if (ctx != EGL_NO_CONTEXT && v.destroy_context) {
        v.make_current(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        v.destroy_context(dpy, ctx);
    }
    if (surf != EGL_NO_SURFACE && v.destroy_surface) v.destroy_surface(dpy, surf);
    /* Leave the process-wide EGL display initialized for FCL. */
    if (ahb) AHardwareBuffer_release(ahb);
}

/* AHardwareBuffer_getNativeHandle() is a libnativewindow symbol but is not
 * declared in the NDK r27 headers, so resolve it at runtime. */
static const void *probe_ahb_handle(AHardwareBuffer *ahb) {
    typedef const void *(*get_native_handle_fn)(const AHardwareBuffer *);
    static get_native_handle_fn fn;
    if (!fn) fn = (get_native_handle_fn)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
    return fn ? fn(ahb) : NULL;
}

/* 2. Mesa EGL: AHB -> FBO render target (offscreen render -> shared AHB) */
static void probe_mesa_render_into_ahb(void) {
    struct egl_api m;
    AHardwareBuffer *ahb = NULL;
    AHardwareBuffer_Desc desc;
    probe_anwb_t anwb;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    unsigned int tex = 0, fbo = 0;
    int ok = 0;

    /* Locate the plugin's own directory from this library's path. */
    Dl_info info;
    char egl_path[4096] = {0};
    if (dladdr((void *)&probe_mesa_render_into_ahb, &info) && info.dli_fname) {
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s", info.dli_fname);
        snprintf(egl_path, sizeof(egl_path), "%s/libEGL_mesa.so", dirname(tmp));
    }
    if (egl_path[0] == '\0') {
        PROBE_ERR("cannot locate plugin directory for libEGL_mesa.so");
        return;
    }
    if (!egl_api_load(&m, egl_path)) return;

    typedef void (*GL_glEGLImageTargetTexture2DOES_t)(GLenum, GLeglImageOES);
    typedef void (*GL_glGenTextures_t)(GLsizei, GLuint *);
    typedef void (*GL_glBindTexture_t)(GLenum, GLuint);
    typedef void (*GL_glGenFramebuffers_t)(GLsizei, GLuint *);
    typedef void (*GL_glBindFramebuffer_t)(GLenum, GLuint);
    typedef void (*GL_glFramebufferTexture2D_t)(GLenum, GLenum, GLenum, GLuint, GLint);
    typedef void (*GL_glViewport_t)(GLint, GLint, GLsizei, GLsizei);
    typedef void (*GL_glClearColor_t)(GLfloat, GLfloat, GLfloat, GLfloat);
    typedef void (*GL_glClear_t)(GLbitfield);
    typedef void (*GL_glFinish_t)(void);

    GL_glEGLImageTargetTexture2DOES_t fnImageTarget = (void *)m.get_proc_address("glEGLImageTargetTexture2DOES");
    GL_glGenTextures_t fnGenTex = (void *)m.get_proc_address("glGenTextures");
    GL_glBindTexture_t fnBindTex = (void *)m.get_proc_address("glBindTexture");
    GL_glGenFramebuffers_t fnGenFbo = (void *)m.get_proc_address("glGenFramebuffers");
    GL_glBindFramebuffer_t fnBindFbo = (void *)m.get_proc_address("glBindFramebuffer");
    GL_glFramebufferTexture2D_t fnFboTex = (void *)m.get_proc_address("glFramebufferTexture2D");
    GL_glViewport_t fnViewport = (void *)m.get_proc_address("glViewport");
    GL_glClearColor_t fnClearColor = (void *)m.get_proc_address("glClearColor");
    GL_glClear_t fnClear = (void *)m.get_proc_address("glClear");
    GL_glFinish_t fnFinish = (void *)m.get_proc_address("glFinish");
    if (!fnImageTarget || !fnGenTex || !fnGenFbo || !fnClear) {
        PROBE_ERR("mesa GL entry points unavailable");
        return;
    }

    ahb = probe_alloc_ahb(&desc);
    if (!ahb) return;
    const void *handle = probe_ahb_handle(ahb);
    if (!handle) {
        PROBE_ERR("mesa: cannot resolve AHardwareBuffer_getNativeHandle");
        goto out;
    }

    dpy = m.get_display(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || m.initialize(dpy, NULL, NULL) != EGL_TRUE) {
        PROBE_ERR("mesa eglInitialize failed: err=0x%x", m.get_error());
        goto out;
    }
    m.bind_api(EGL_OPENGL_API);
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    if (m.choose_config(dpy, cfg_attribs, &cfg, 1, &ncfg) != EGL_TRUE || ncfg == 0) {
        PROBE_ERR("mesa eglChooseConfig failed: err=0x%x ncfg=%d", m.get_error(), ncfg);
        goto out;
    }
    EGLint pb_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    surf = m.create_pbuffer_surface(dpy, cfg, pb_attribs);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx = m.create_context(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        m.make_current(dpy, surf, surf, ctx) != EGL_TRUE) {
        PROBE_ERR("mesa context setup failed: surf=%p ctx=%p err=0x%x",
                  (void *)surf, (void *)ctx, m.get_error());
        goto out;
    }

    memset(&anwb, 0, sizeof(anwb));
    anwb.common.magic = (int)ANDROID_NATIVE_BUFFER_MAGIC;
    anwb.common.version = (int)sizeof(anwb);
    anwb.common.incRef = probe_noop_ref;
    anwb.common.decRef = probe_noop_ref;
    anwb.width = (int)desc.width;
    anwb.height = (int)desc.height;
    anwb.stride = (int)desc.stride;
    anwb.format = (int)desc.format;
    anwb.usage = (int)desc.usage;
    anwb.handle = handle;

    img = m.create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)&anwb, NULL);
    PROBE_LOG("mesa: AHB import image=%p err=0x%x", (void *)img, m.get_error());
    if (img == EGL_NO_IMAGE_KHR) goto out;

    fnGenTex(1, &tex);
    fnBindTex(GL_TEXTURE_2D, tex);
    fnImageTarget(GL_TEXTURE_2D, (GLeglImageOES)img);
    fnGenFbo(1, &fbo);
    fnBindFbo(GL_FRAMEBUFFER, fbo);
    fnFboTex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    fnViewport(0, 0, (GLsizei)desc.width, (GLsizei)desc.height);
    fnClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    fnClear(GL_COLOR_BUFFER_BIT);
    fnFinish();

    void *ptr = NULL;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &ptr) == 0 && ptr) {
        const unsigned char *px = (const unsigned char *)ptr;
        PROBE_LOG("mesa: rendered pixel B=%u G=%u R=%u A=%u", px[0], px[1], px[2], px[3]);
        ok = (px[0] > 200 && px[1] < 50 && px[2] < 50);
        AHardwareBuffer_unlock(ahb, NULL);
    } else {
        PROBE_ERR("mesa: AHardwareBuffer_lock failed");
    }

out:
    PROBE_LOG("mesa AHB render target: %s", ok ? "OK (offscreen render -> shared AHB)" : "FAILED");
    if (img != EGL_NO_IMAGE_KHR && m.destroy_image) m.destroy_image(dpy, img);
    if (ctx != EGL_NO_CONTEXT && m.destroy_context) {
        m.make_current(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        m.destroy_context(dpy, ctx);
    }
    if (surf != EGL_NO_SURFACE && m.destroy_surface) m.destroy_surface(dpy, surf);
    if (ahb) AHardwareBuffer_release(ahb);
}

__attribute__((constructor))
static void fcl_probe_init(void) {
    setenv("GALLIUM_DRIVER", "freedreno", 0);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "kgsl", 0);
    setenv("FD_FORCE_KGSL", "1", 0);

    const char *enabled = getenv("FCL_PROBE");
    if (enabled && enabled[0] == '0') return;
    probe_vendor_import();
    probe_mesa_render_into_ahb();
}
