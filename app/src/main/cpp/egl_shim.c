// SPDX-License-Identifier: MIT
/*
 * FCL EGL presentation shim
 * -------------------------
 * Mesa (freedreno/kgsl) cannot own the Android window buffer on this device:
 * the window path goes through u_gralloc's fallback backend and the frame
 * never reaches the vendor compositor cleanly (eglMakeCurrent ends up with
 * EGL_BAD_SURFACE / heap corruption after ~60 s).  This shim splits the two
 * roles the way GameNative's BlitConverter/GPUImage do:
 *
 *   game GL calls -> Mesa context on a pbuffer (proven stable on KGSL)
 *   eglSwapBuffers -> Mesa glBlitFramebuffer into an AHardwareBuffer
 *                  -> EGL_ANDROID_native_fence_sync hand-off
 *                  -> vendor GLES samples the AHB via EGLImage and
 *                     glBlitFramebuffer's it to the vendor window surface
 *                  -> vendor eglSwapBuffers (vendor gralloc owns the window)
 *
 * The real Mesa EGL is shipped next to this library as libEGL_mesa_core.so
 * (renamed by scripts/build-mesa-android.sh); the vendor EGL/GLES come from
 * /system/lib64.  FCL resolves all EGL entry points through eglGetProcAddress,
 * so the shim also re-exports them for that path.
 */
#define LOG_TAG "EGLShim"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <libgen.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define SHIM_LOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define SHIM_ERR(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3143
#endif
#ifndef EGL_SYNC_FENCE_KHR
#define EGL_SYNC_FENCE_KHR 0x30F9
#endif
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3142
#endif

/* ------------------------------------------------------------------ */
/* backends                                                           */
/* ------------------------------------------------------------------ */

struct egl_api {
    void *handle;
    EGLDisplay (*GetDisplay)(EGLNativeDisplayType);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*Terminate)(EGLDisplay);
    const char *(*QueryString)(EGLDisplay, EGLint);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
    EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLBoolean (*ReleaseThread)(void);
    EGLint (*GetError)(void);
    __eglMustCastToProperFunctionPointerType (*GetProcAddress)(const char *);
    /* extensions */
    EGLSyncKHR (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean (*DestroySyncKHR)(EGLDisplay, EGLSyncKHR);
    EGLint (*ClientWaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
    EGLBoolean (*WaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint);
    EGLint (*DupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
    EGLClientBuffer (*GetNativeClientBufferANDROID)(AHardwareBuffer *);
    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
};

struct gl_api {
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                            GLbitfield, GLenum);
    void (*Finish)(void);
    void (*Flush)(void);
    void (*EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
};

static struct egl_api mesa_egl;
static struct egl_api vendor_egl;
static struct gl_api mesa_gl;
static struct gl_api vendor_gl;

static int load_egl_api(struct egl_api *api, const char *path, const char *fallback)
{
    memset(api, 0, sizeof(*api));
    api->handle = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
    if (!api->handle && fallback)
        api->handle = dlopen(fallback, RTLD_LOCAL | RTLD_LAZY);
    if (!api->handle) {
        SHIM_ERR("dlopen %s failed: %s", path, dlerror());
        return 0;
    }
#define LOAD(field, name) \
    do { \
        api->field = (void *)dlsym(api->handle, name); \
        if (!api->field) { SHIM_ERR("%s: missing %s", path, name); return 0; } \
    } while (0)
    LOAD(GetDisplay, "eglGetDisplay");
    LOAD(Initialize, "eglInitialize");
    LOAD(Terminate, "eglTerminate");
    LOAD(QueryString, "eglQueryString");
    LOAD(ChooseConfig, "eglChooseConfig");
    LOAD(GetConfigAttrib, "eglGetConfigAttrib");
    LOAD(CreateContext, "eglCreateContext");
    LOAD(DestroyContext, "eglDestroyContext");
    LOAD(CreateWindowSurface, "eglCreateWindowSurface");
    LOAD(CreatePbufferSurface, "eglCreatePbufferSurface");
    LOAD(DestroySurface, "eglDestroySurface");
    LOAD(MakeCurrent, "eglMakeCurrent");
    LOAD(SwapBuffers, "eglSwapBuffers");
    LOAD(SwapInterval, "eglSwapInterval");
    LOAD(QuerySurface, "eglQuerySurface");
    LOAD(BindAPI, "eglBindAPI");
    LOAD(GetError, "eglGetError");
    LOAD(GetProcAddress, "eglGetProcAddress");
#undef LOAD
    api->ReleaseThread = (void *)dlsym(api->handle, "eglReleaseThread");
    /* optional extensions */
#define LOAD_EXT(field, name) api->field = (void *)api->GetProcAddress(name)
    LOAD_EXT(CreateSyncKHR, "eglCreateSyncKHR");
    LOAD_EXT(DestroySyncKHR, "eglDestroySyncKHR");
    LOAD_EXT(ClientWaitSyncKHR, "eglClientWaitSyncKHR");
    LOAD_EXT(WaitSyncKHR, "eglWaitSyncKHR");
    LOAD_EXT(DupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID");
    LOAD_EXT(GetNativeClientBufferANDROID, "eglGetNativeClientBufferANDROID");
    LOAD_EXT(CreateImageKHR, "eglCreateImageKHR");
    LOAD_EXT(DestroyImageKHR, "eglDestroyImageKHR");
#undef LOAD_EXT
    return 1;
}

static void load_gl_api(struct gl_api *gl, struct egl_api *egl, const char *what)
{
    memset(gl, 0, sizeof(*gl));
#define LOAD_GL(field, name) \
    do { \
        gl->field = (void *)egl->GetProcAddress(name); \
        if (!gl->field) SHIM_ERR("%s: missing GL %s", what, name); \
    } while (0)
    LOAD_GL(GenTextures, "glGenTextures");
    LOAD_GL(DeleteTextures, "glDeleteTextures");
    LOAD_GL(BindTexture, "glBindTexture");
    LOAD_GL(GenFramebuffers, "glGenFramebuffers");
    LOAD_GL(DeleteFramebuffers, "glDeleteFramebuffers");
    LOAD_GL(BindFramebuffer, "glBindFramebuffer");
    LOAD_GL(FramebufferTexture2D, "glFramebufferTexture2D");
    LOAD_GL(BlitFramebuffer, "glBlitFramebuffer");
    LOAD_GL(Finish, "glFinish");
    LOAD_GL(Flush, "glFlush");
    LOAD_GL(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
#undef LOAD_GL
}

static const char *self_dir(void)
{
    static char dir[4096];
    if (dir[0])
        return dir;
    Dl_info info;
    if (!dladdr((void *)&self_dir, &info) || !info.dli_fname)
        return "";
    snprintf(dir, sizeof(dir), "%s", info.dli_fname);
    return dirname(dir);
}

/* ------------------------------------------------------------------ */
/* objects                                                            */
/* ------------------------------------------------------------------ */

#define RING_SIZE 2

struct shim_display;

struct shim_context {
    struct shim_display *dpy;
    EGLContext mesa_ctx;
};

struct shim_ring_slot {
    AHardwareBuffer *ahb;
    EGLImageKHR mesa_image;
    GLuint mesa_tex, mesa_fbo;
    EGLImageKHR vendor_image;
    GLuint vendor_tex, vendor_fbo;
    int vendor_fence_fd;
};

struct shim_surface {
    struct shim_display *dpy;
    int is_window;
    EGLConfig mesa_config;
    EGLSurface mesa_surface;
    EGLSurface vendor_window;
    int width, height;
    int ring_ready;
    struct shim_ring_slot ring[RING_SIZE];
    int cur;
};

struct shim_display {
    EGLDisplay mesa_dpy;
    EGLDisplay vendor_dpy;
    int mesa_ok, vendor_ok;
    EGLConfig vendor_cfg;
    EGLContext vendor_ctx;
    EGLSurface vendor_scratch; /* 1x1 pbuffer, keeps vendor ctx creatable */
    int failed;
};

static struct shim_display display_singleton;
static int display_used;

static __thread struct shim_display *tls_dpy;
static __thread struct shim_context *tls_ctx;
static __thread struct shim_surface *tls_draw;
static __thread struct shim_surface *tls_read;

static EGLint shim_error = EGL_SUCCESS;
static EGLenum shim_api = EGL_OPENGL_ES_API;

static EGLint set_error(EGLint err)
{
    shim_error = err;
    return err;
}

/* ------------------------------------------------------------------ */
/* vendor helpers                                                     */
/* ------------------------------------------------------------------ */

static int vendor_init(struct shim_display *d)
{
    if (d->vendor_ok)
        return 1;
    d->vendor_dpy = vendor_egl.GetDisplay(EGL_DEFAULT_DISPLAY);
    if (d->vendor_dpy == EGL_NO_DISPLAY) {
        SHIM_ERR("vendor eglGetDisplay failed");
        return 0;
    }
    if (!vendor_egl.Initialize(d->vendor_dpy, NULL, NULL)) {
        SHIM_ERR("vendor eglInitialize failed: 0x%x", vendor_egl.GetError());
        return 0;
    }
    vendor_egl.BindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLint n = 0;
    if (!vendor_egl.ChooseConfig(d->vendor_dpy, cfg_attribs, &d->vendor_cfg, 1, &n) || n == 0) {
        SHIM_ERR("vendor eglChooseConfig failed: 0x%x n=%d", vendor_egl.GetError(), n);
        return 0;
    }
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    d->vendor_ctx = vendor_egl.CreateContext(d->vendor_dpy, d->vendor_cfg, EGL_NO_CONTEXT,
                                             ctx_attribs);
    if (d->vendor_ctx == EGL_NO_CONTEXT) {
        SHIM_ERR("vendor eglCreateContext failed: 0x%x", vendor_egl.GetError());
        return 0;
    }
    const EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    d->vendor_scratch = vendor_egl.CreatePbufferSurface(d->vendor_dpy, d->vendor_cfg, pb);
    d->vendor_ok = 1;
    SHIM_LOG("vendor EGL ready (ES3 context %p)", (void *)d->vendor_ctx);
    return 1;
}

/* ------------------------------------------------------------------ */
/* ring (AHB interchange)                                             */
/* ------------------------------------------------------------------ */

static void ring_slot_destroy(struct shim_display *d, struct shim_ring_slot *slot)
{
    if (slot->vendor_fence_fd >= 0) {
        close(slot->vendor_fence_fd);
        slot->vendor_fence_fd = -1;
    }
    if (slot->mesa_fbo && mesa_gl.DeleteFramebuffers)
        mesa_gl.DeleteFramebuffers(1, &slot->mesa_fbo);
    if (slot->mesa_tex && mesa_gl.DeleteTextures)
        mesa_gl.DeleteTextures(1, &slot->mesa_tex);
    if (slot->mesa_image && mesa_egl.DestroyImageKHR)
        mesa_egl.DestroyImageKHR(d->mesa_dpy, slot->mesa_image);
    if (slot->vendor_fbo && vendor_gl.DeleteFramebuffers)
        vendor_gl.DeleteFramebuffers(1, &slot->vendor_fbo);
    if (slot->vendor_tex && vendor_gl.DeleteTextures)
        vendor_gl.DeleteTextures(1, &slot->vendor_tex);
    if (slot->vendor_image && vendor_egl.DestroyImageKHR)
        vendor_egl.DestroyImageKHR(d->vendor_dpy, slot->vendor_image);
    if (slot->ahb)
        AHardwareBuffer_release(slot->ahb);
    memset(slot, 0, sizeof(*slot));
    slot->vendor_fence_fd = -1;
}

static void ring_destroy(struct shim_surface *s)
{
    if (!s->ring_ready)
        return;
    for (int i = 0; i < RING_SIZE; i++)
        ring_slot_destroy(s->dpy, &s->ring[i]);
    s->ring_ready = 0;
}

/* Creates the AHB ring; requires the Mesa context to be current (for the
 * Mesa-side texture/FBO) and temporarily makes the vendor context current. */
static int ring_create(struct shim_surface *s)
{
    struct shim_display *d = s->dpy;
    if (s->ring_ready)
        return 1;
    if (!mesa_gl.GenTextures) {
        SHIM_ERR("ring: Mesa GL entry points missing");
        return 0;
    }
    if (!vendor_gl.GenTextures && d->vendor_ok)
        load_gl_api(&vendor_gl, &vendor_egl, "vendor");
    if (!vendor_gl.GenTextures) {
        SHIM_ERR("ring: vendor GL entry points missing");
        return 0;
    }
    mesa_egl.BindAPI(shim_api);

    int vendor_current = 0;
    for (int i = 0; i < RING_SIZE; i++) {
        struct shim_ring_slot *slot = &s->ring[i];
        slot->vendor_fence_fd = -1;

        AHardwareBuffer_Desc desc = {
            .width = (uint32_t)s->width,
            .height = (uint32_t)s->height,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            /* CPU_WRITE_OFTEN forces a linear layout; the interchange buffer
             * is only read/written by GPU blits, but linear keeps the u_gralloc
             * fallback metadata unambiguous. */
            .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
        };
        if (AHardwareBuffer_allocate(&desc, &slot->ahb) != 0 || !slot->ahb) {
            SHIM_ERR("ring: AHardwareBuffer_allocate %dx%d failed", s->width, s->height);
            goto fail;
        }

        /* Mesa side: render target for the frame blit. */
        EGLClientBuffer anwb = vendor_egl.GetNativeClientBufferANDROID
                                   ? vendor_egl.GetNativeClientBufferANDROID(slot->ahb)
                                   : NULL;
        if (!anwb) {
            SHIM_ERR("ring: no platform ANativeWindowBuffer");
            goto fail;
        }
        slot->mesa_image = mesa_egl.CreateImageKHR(d->mesa_dpy, EGL_NO_CONTEXT,
                                                   EGL_NATIVE_BUFFER_ANDROID, anwb, NULL);
        if (slot->mesa_image == EGL_NO_IMAGE_KHR) {
            SHIM_ERR("ring: Mesa eglCreateImageKHR(AHB) failed: 0x%x", mesa_egl.GetError());
            goto fail;
        }
        mesa_gl.GenTextures(1, &slot->mesa_tex);
        mesa_gl.BindTexture(GL_TEXTURE_2D, slot->mesa_tex);
        mesa_gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, slot->mesa_image);
        mesa_gl.GenFramebuffers(1, &slot->mesa_fbo);
        mesa_gl.BindFramebuffer(GL_FRAMEBUFFER, slot->mesa_fbo);
        mesa_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, slot->mesa_tex, 0);
        mesa_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

        /* Vendor side: texture sampled by the presentation blit. */
        if (!vendor_current) {
            if (!vendor_egl.MakeCurrent(d->vendor_dpy, d->vendor_scratch, d->vendor_scratch,
                                        d->vendor_ctx)) {
                SHIM_ERR("ring: vendor eglMakeCurrent failed: 0x%x", vendor_egl.GetError());
                goto fail;
            }
            vendor_current = 1;
        }
        EGLClientBuffer cb = vendor_egl.GetNativeClientBufferANDROID(slot->ahb);
        const EGLint attribs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        slot->vendor_image = vendor_egl.CreateImageKHR(d->vendor_dpy, EGL_NO_CONTEXT,
                                                       EGL_NATIVE_BUFFER_ANDROID, cb, attribs);
        if (slot->vendor_image == EGL_NO_IMAGE_KHR) {
            SHIM_ERR("ring: vendor eglCreateImageKHR(AHB) failed: 0x%x", vendor_egl.GetError());
            goto fail;
        }
        vendor_gl.GenTextures(1, &slot->vendor_tex);
        vendor_gl.BindTexture(GL_TEXTURE_2D, slot->vendor_tex);
        vendor_gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, slot->vendor_image);
        vendor_gl.GenFramebuffers(1, &slot->vendor_fbo);
        vendor_gl.BindFramebuffer(GL_FRAMEBUFFER, slot->vendor_fbo);
        vendor_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, slot->vendor_tex, 0);
        vendor_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /* restore Mesa as current (caller had it current) */
    if (vendor_current && tls_ctx)
        mesa_egl.MakeCurrent(d->mesa_dpy, s->mesa_surface, s->mesa_surface, tls_ctx->mesa_ctx);
    s->ring_ready = 1;
    SHIM_LOG("AHB ring ready: %dx%d", s->width, s->height);
    return 1;

fail:
    if (vendor_current && tls_ctx)
        mesa_egl.MakeCurrent(d->mesa_dpy, s->mesa_surface, s->mesa_surface, tls_ctx->mesa_ctx);
    ring_destroy(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* presentation                                                       */
/* ------------------------------------------------------------------ */

static int fence_wait(struct egl_api *api, EGLDisplay dpy, int fd)
{
    if (fd < 0)
        return 1;
    const EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE };
    EGLSyncKHR sync = api->CreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        SHIM_ERR("eglCreateSyncKHR(native fence fd=%d) failed: 0x%x", fd, api->GetError());
        close(fd);
        return 0;
    }
    if (api->WaitSyncKHR)
        api->WaitSyncKHR(dpy, sync, 0);
    else if (api->ClientWaitSyncKHR)
        api->ClientWaitSyncKHR(dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
    api->DestroySyncKHR(dpy, sync); /* consumes the fd */
    return 1;
}

static int fence_export(struct egl_api *api, EGLDisplay dpy)
{
    if (!api->CreateSyncKHR || !api->DupNativeFenceFDANDROID)
        return -1;
    EGLSyncKHR sync = api->CreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR, NULL);
    if (sync == EGL_NO_SYNC_KHR)
        return -1;
    int fd = api->DupNativeFenceFDANDROID(dpy, sync);
    api->DestroySyncKHR(dpy, sync);
    return fd;
}

static int shim_present(struct shim_surface *s, struct shim_context *ctx)
{
    struct shim_display *d = s->dpy;

    if (!ctx) {
        SHIM_ERR("eglSwapBuffers without a current context");
        return 0;
    }
    if (!d->vendor_ok && !vendor_init(d))
        return 0;
    mesa_egl.BindAPI(shim_api);

    /* Track window size changes (FCL recreates the surface, but be safe). */
    EGLint w = 0, h = 0;
    vendor_egl.QuerySurface(d->vendor_dpy, s->vendor_window, EGL_WIDTH, &w);
    vendor_egl.QuerySurface(d->vendor_dpy, s->vendor_window, EGL_HEIGHT, &h);
    if (w > 0 && h > 0 && (w != s->width || h != s->height)) {
        SHIM_LOG("window resized %dx%d -> %dx%d", s->width, s->height, w, h);
        ring_destroy(s);
        if (s->mesa_surface != EGL_NO_SURFACE)
            mesa_egl.DestroySurface(d->mesa_dpy, s->mesa_surface);
        s->width = w;
        s->height = h;
        const EGLint pb[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
        mesa_egl.BindAPI(shim_api);
        s->mesa_surface = mesa_egl.CreatePbufferSurface(d->mesa_dpy, s->mesa_config, pb);
        if (s->mesa_surface == EGL_NO_SURFACE) {
            SHIM_ERR("resize: Mesa pbuffer recreate failed: 0x%x", mesa_egl.GetError());
            return 0;
        }
        if (ctx && !mesa_egl.MakeCurrent(d->mesa_dpy, s->mesa_surface, s->mesa_surface,
                                         ctx->mesa_ctx)) {
            SHIM_ERR("resize: Mesa eglMakeCurrent failed: 0x%x", mesa_egl.GetError());
            return 0;
        }
    }

    if (!ring_create(s))
        return 0;

    struct shim_ring_slot *slot = &s->ring[s->cur];

    /* Wait until the vendor is done with this slot before overwriting it. */
    if (slot->vendor_fence_fd >= 0) {
        fence_wait(&mesa_egl, d->mesa_dpy, slot->vendor_fence_fd);
        slot->vendor_fence_fd = -1;
    }

    /* Mesa: pbuffer FBO 0 -> AHB FBO. */
    mesa_gl.Finish();
    mesa_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    mesa_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->mesa_fbo);
    mesa_gl.BlitFramebuffer(0, 0, s->width, s->height, 0, 0, s->width, s->height,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    mesa_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    mesa_gl.Flush();
    int fence_fd = fence_export(&mesa_egl, d->mesa_dpy);

    /* Vendor: wait for Mesa, blit AHB -> window, swap. */
    if (!vendor_egl.MakeCurrent(d->vendor_dpy, s->vendor_window, s->vendor_window,
                                d->vendor_ctx)) {
        SHIM_ERR("vendor eglMakeCurrent(window) failed: 0x%x", vendor_egl.GetError());
        if (fence_fd >= 0)
            close(fence_fd);
        if (ctx)
            mesa_egl.MakeCurrent(d->mesa_dpy, s->mesa_surface, s->mesa_surface, ctx->mesa_ctx);
        return 0;
    }
    if (fence_fd >= 0)
        fence_wait(&vendor_egl, d->vendor_dpy, fence_fd);

    vendor_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, slot->vendor_fbo);
    vendor_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    vendor_gl.BlitFramebuffer(0, 0, s->width, s->height, 0, 0, s->width, s->height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
    vendor_gl.Flush();

    slot->vendor_fence_fd = fence_export(&vendor_egl, d->vendor_dpy);

    EGLBoolean swapped = vendor_egl.SwapBuffers(d->vendor_dpy, s->vendor_window);

    /* Restore the game's Mesa context. */
    if (ctx && !mesa_egl.MakeCurrent(d->mesa_dpy, s->mesa_surface, s->mesa_surface,
                                     ctx->mesa_ctx)) {
        SHIM_ERR("restore Mesa eglMakeCurrent failed: 0x%x", mesa_egl.GetError());
    }

    s->cur ^= 1;
    if (!swapped) {
        SHIM_ERR("vendor eglSwapBuffers failed: 0x%x", vendor_egl.GetError());
        set_error(EGL_BAD_SURFACE);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* EGL API                                                            */
/* ------------------------------------------------------------------ */

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
    struct shim_display *d = &display_singleton;
    if (!display_used) {
        d->mesa_dpy = mesa_egl.GetDisplay(display_id);
        if (d->mesa_dpy == EGL_NO_DISPLAY) {
            SHIM_ERR("Mesa eglGetDisplay failed");
            return EGL_NO_DISPLAY;
        }
        display_used = 1;
        SHIM_LOG("display %p (mesa %p)", (void *)d, (void *)d->mesa_dpy);
    }
    return (EGLDisplay)d;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d || d != &display_singleton) {
        set_error(EGL_BAD_DISPLAY);
        return EGL_FALSE;
    }
    if (d->mesa_ok)
        return EGL_TRUE;
    if (!mesa_egl.Initialize(d->mesa_dpy, major, minor)) {
        SHIM_ERR("Mesa eglInitialize failed: 0x%x", mesa_egl.GetError());
        set_error(EGL_NOT_INITIALIZED);
        return EGL_FALSE;
    }
    d->mesa_ok = 1;
    vendor_init(d); /* best effort; window surfaces need it */
    if (!mesa_gl.GenTextures)
        load_gl_api(&mesa_gl, &mesa_egl, "mesa");
    if (!vendor_gl.GenTextures && d->vendor_ok)
        load_gl_api(&vendor_gl, &vendor_egl, "vendor");
    SHIM_LOG("initialized: Mesa ok, vendor %s", d->vendor_ok ? "ok" : "FAILED");
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d)
        return EGL_FALSE;
    if (d->mesa_ok)
        mesa_egl.Terminate(d->mesa_dpy);
    d->mesa_ok = 0;
    return EGL_TRUE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d)
        return NULL;
    return mesa_egl.QueryString(d->mesa_dpy, name);
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                           EGLint config_size, EGLint *num_config)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d) {
        set_error(EGL_BAD_DISPLAY);
        return EGL_FALSE;
    }
    return mesa_egl.ChooseConfig(d->mesa_dpy, attrib_list, configs, config_size, num_config);
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d) {
        set_error(EGL_BAD_DISPLAY);
        return EGL_FALSE;
    }
    return mesa_egl.GetConfigAttrib(d->mesa_dpy, config, attribute, value);
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                            const EGLint *attrib_list)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d) {
        set_error(EGL_BAD_DISPLAY);
        return EGL_NO_CONTEXT;
    }
    EGLContext share = share_context ? ((struct shim_context *)share_context)->mesa_ctx
                                     : EGL_NO_CONTEXT;
    EGLContext mc = mesa_egl.CreateContext(d->mesa_dpy, config, share, attrib_list);
    if (mc == EGL_NO_CONTEXT) {
        SHIM_ERR("Mesa eglCreateContext failed: 0x%x", mesa_egl.GetError());
        set_error(EGL_BAD_CONTEXT);
        return EGL_NO_CONTEXT;
    }
    struct shim_context *c = calloc(1, sizeof(*c));
    c->dpy = d;
    c->mesa_ctx = mc;
    SHIM_LOG("context %p (mesa %p)", (void *)c, (void *)mc);
    return (EGLContext)c;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    struct shim_display *d = (struct shim_display *)dpy;
    struct shim_context *c = (struct shim_context *)ctx;
    if (!d || !c)
        return EGL_FALSE;
    EGLBoolean ok = mesa_egl.DestroyContext(d->mesa_dpy, c->mesa_ctx);
    free(c);
    return ok;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win, const EGLint *attrib_list)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d || !win) {
        set_error(EGL_BAD_NATIVE_WINDOW);
        return EGL_NO_SURFACE;
    }
    if (!d->vendor_ok && !vendor_init(d)) {
        set_error(EGL_NOT_INITIALIZED);
        return EGL_NO_SURFACE;
    }

    EGLSurface vwin = vendor_egl.CreateWindowSurface(d->vendor_dpy, d->vendor_cfg, win,
                                                     attrib_list);
    if (vwin == EGL_NO_SURFACE) {
        SHIM_ERR("vendor eglCreateWindowSurface failed: 0x%x", vendor_egl.GetError());
        set_error(EGL_BAD_NATIVE_WINDOW);
        return EGL_NO_SURFACE;
    }
    EGLint w = 0, h = 0;
    vendor_egl.QuerySurface(d->vendor_dpy, vwin, EGL_WIDTH, &w);
    vendor_egl.QuerySurface(d->vendor_dpy, vwin, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) {
        w = 16;
        h = 16;
    }

    /* Mesa renders into a pbuffer of the same size.  Use the config the game
     * picked: FCL requests EGL_WINDOW_BIT|EGL_PBUFFER_BIT, so it works. */
    const EGLint pb[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface mpb = mesa_egl.CreatePbufferSurface(d->mesa_dpy, config, pb);
    if (mpb == EGL_NO_SURFACE) {
        SHIM_ERR("Mesa eglCreatePbufferSurface(%dx%d) failed: 0x%x", w, h,
                 mesa_egl.GetError());
        vendor_egl.DestroySurface(d->vendor_dpy, vwin);
        set_error(EGL_BAD_MATCH);
        return EGL_NO_SURFACE;
    }

    struct shim_surface *s = calloc(1, sizeof(*s));
    s->dpy = d;
    s->is_window = 1;
    s->mesa_config = config;
    s->mesa_surface = mpb;
    s->vendor_window = vwin;
    s->width = w;
    s->height = h;
    s->cur = 0;
    for (int i = 0; i < RING_SIZE; i++)
        s->ring[i].vendor_fence_fd = -1;
    SHIM_LOG("window surface %p (%dx%d, vendor %p, mesa pbuffer %p)",
             (void *)s, w, h, (void *)vwin, (void *)mpb);
    return (EGLSurface)s;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d) {
        set_error(EGL_BAD_DISPLAY);
        return EGL_NO_SURFACE;
    }
    EGLSurface mpb = mesa_egl.CreatePbufferSurface(d->mesa_dpy, config, attrib_list);
    if (mpb == EGL_NO_SURFACE) {
        SHIM_ERR("Mesa eglCreatePbufferSurface failed: 0x%x", mesa_egl.GetError());
        return EGL_NO_SURFACE;
    }
    struct shim_surface *s = calloc(1, sizeof(*s));
    s->dpy = d;
    s->mesa_config = config;
    s->mesa_surface = mpb;
    for (int i = 0; i < RING_SIZE; i++)
        s->ring[i].vendor_fence_fd = -1;
    return (EGLSurface)s;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    struct shim_display *d = (struct shim_display *)dpy;
    struct shim_surface *s = (struct shim_surface *)surface;
    if (!d || !s)
        return EGL_FALSE;
    ring_destroy(s);
    if (s->is_window && s->vendor_window != EGL_NO_SURFACE)
        vendor_egl.DestroySurface(d->vendor_dpy, s->vendor_window);
    if (s->mesa_surface != EGL_NO_SURFACE)
        mesa_egl.DestroySurface(d->mesa_dpy, s->mesa_surface);
    free(s);
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    struct shim_display *d = (struct shim_display *)dpy;
    struct shim_surface *sd = (struct shim_surface *)draw;
    struct shim_surface *sr = (struct shim_surface *)read;
    struct shim_context *c = (struct shim_context *)ctx;
    if (!d) {
        set_error(EGL_BAD_DISPLAY);
        return EGL_FALSE;
    }
    if (!ctx) {
        EGLBoolean ok = mesa_egl.MakeCurrent(d->mesa_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                             EGL_NO_CONTEXT);
        tls_ctx = NULL;
        tls_draw = tls_read = NULL;
        return ok;
    }
    EGLBoolean ok = mesa_egl.MakeCurrent(d->mesa_dpy,
                                         sd ? sd->mesa_surface : EGL_NO_SURFACE,
                                         sr ? sr->mesa_surface : EGL_NO_SURFACE,
                                         c->mesa_ctx);
    if (!ok) {
        SHIM_ERR("Mesa eglMakeCurrent failed: 0x%x (draw=%p read=%p ctx=%p)", mesa_egl.GetError(),
                 (void *)draw, (void *)read, (void *)ctx);
        set_error(EGL_BAD_SURFACE);
        return EGL_FALSE;
    }
    tls_dpy = d;
    tls_ctx = c;
    tls_draw = sd;
    tls_read = sr;
    static int logged_gl_info;
    if (!logged_gl_info) {
        logged_gl_info = 1;
        typedef const unsigned char *(*get_string_fn)(unsigned int);
        get_string_fn glGetString = (get_string_fn)mesa_egl.GetProcAddress("glGetString");
        if (glGetString) {
            SHIM_LOG("game context: GL_VERSION=%s GL_RENDERER=%s GL_VENDOR=%s",
                     glGetString(0x1F02) ? (const char *)glGetString(0x1F02) : "?",
                     glGetString(0x1F01) ? (const char *)glGetString(0x1F01) : "?",
                     glGetString(0x1F00) ? (const char *)glGetString(0x1F00) : "?");
        }
    }
    return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    struct shim_display *d = (struct shim_display *)dpy;
    struct shim_surface *s = (struct shim_surface *)surface;
    if (!d || !s) {
        set_error(EGL_BAD_SURFACE);
        return EGL_FALSE;
    }
    if (!s->is_window)
        return EGL_TRUE;
    if (!shim_present(s, tls_ctx)) {
        set_error(EGL_BAD_SURFACE);
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d)
        return EGL_FALSE;
    if (tls_draw && tls_draw->is_window)
        return vendor_egl.SwapInterval(d->vendor_dpy, interval);
    return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value)
{
    struct shim_display *d = (struct shim_display *)dpy;
    struct shim_surface *s = (struct shim_surface *)surface;
    if (!d || !s) {
        set_error(EGL_BAD_SURFACE);
        return EGL_FALSE;
    }
    if (s->is_window && (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)) {
        return vendor_egl.QuerySurface(d->vendor_dpy, s->vendor_window, attribute, value);
    }
    return mesa_egl.QuerySurface(d->mesa_dpy, s->mesa_surface, attribute, value);
}

EGLContext eglGetCurrentContext(void)
{
    return (EGLContext)tls_ctx;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw)
{
    return (EGLSurface)(readdraw == EGL_READ ? tls_read : tls_draw);
}

EGLDisplay eglGetCurrentDisplay(void)
{
    return (EGLDisplay)tls_dpy;
}

EGLBoolean eglBindAPI(EGLenum api)
{
    shim_api = api;
    return mesa_egl.BindAPI(api);
}

EGLBoolean eglReleaseThread(void)
{
    tls_ctx = NULL;
    tls_draw = tls_read = NULL;
    tls_dpy = NULL;
    return mesa_egl.ReleaseThread ? mesa_egl.ReleaseThread() : EGL_TRUE;
}

EGLint eglGetError(void)
{
    EGLint e = shim_error;
    shim_error = EGL_SUCCESS;
    if (e == EGL_SUCCESS)
        e = mesa_egl.GetError();
    return e;
}

EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                              EGLClientBuffer buffer, const EGLint *attrib_list)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d)
        return EGL_NO_IMAGE_KHR;
    return mesa_egl.CreateImageKHR(d->mesa_dpy, ctx ? ((struct shim_context *)ctx)->mesa_ctx
                                                    : EGL_NO_CONTEXT,
                                   target, buffer, attrib_list);
}

EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d)
        return EGL_FALSE;
    return mesa_egl.DestroyImageKHR(d->mesa_dpy, image);
}

/* ------------------------------------------------------------------ */
/* eglGetProcAddress dispatch                                         */
/* ------------------------------------------------------------------ */

struct proc_entry {
    const char *name;
    __eglMustCastToProperFunctionPointerType fn;
};

static const struct proc_entry shim_procs[] = {
    { "eglGetDisplay", (void *)eglGetDisplay },
    { "eglInitialize", (void *)eglInitialize },
    { "eglTerminate", (void *)eglTerminate },
    { "eglQueryString", (void *)eglQueryString },
    { "eglChooseConfig", (void *)eglChooseConfig },
    { "eglGetConfigAttrib", (void *)eglGetConfigAttrib },
    { "eglCreateContext", (void *)eglCreateContext },
    { "eglDestroyContext", (void *)eglDestroyContext },
    { "eglCreateWindowSurface", (void *)eglCreateWindowSurface },
    { "eglCreatePbufferSurface", (void *)eglCreatePbufferSurface },
    { "eglDestroySurface", (void *)eglDestroySurface },
    { "eglMakeCurrent", (void *)eglMakeCurrent },
    { "eglSwapBuffers", (void *)eglSwapBuffers },
    { "eglSwapInterval", (void *)eglSwapInterval },
    { "eglQuerySurface", (void *)eglQuerySurface },
    { "eglGetCurrentContext", (void *)eglGetCurrentContext },
    { "eglGetCurrentSurface", (void *)eglGetCurrentSurface },
    { "eglGetCurrentDisplay", (void *)eglGetCurrentDisplay },
    { "eglBindAPI", (void *)eglBindAPI },
    { "eglReleaseThread", (void *)eglReleaseThread },
    { "eglGetError", (void *)eglGetError },
    { "eglCreateImageKHR", (void *)eglCreateImageKHR },
    { "eglDestroyImageKHR", (void *)eglDestroyImageKHR },
};

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    if (!procname)
        return NULL;
    for (size_t i = 0; i < sizeof(shim_procs) / sizeof(shim_procs[0]); i++) {
        if (strcmp(shim_procs[i].name, procname) == 0)
            return shim_procs[i].fn;
    }
    __eglMustCastToProperFunctionPointerType fn =
        mesa_egl.GetProcAddress ? mesa_egl.GetProcAddress(procname) : NULL;
    if (!fn && procname[0] == 'g' && procname[1] == 'l')
        SHIM_ERR("eglGetProcAddress(%s) -> NULL", procname);
    return fn;
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void shim_init(void)
{
    char path[4096];
    const char *dir = self_dir();

    snprintf(path, sizeof(path), "%s/libEGL_mesa_core.so", dir);
    if (!load_egl_api(&mesa_egl, path, NULL)) {
        /* Fall back to a plain name lookup (e.g. system Mesa for debugging). */
        if (!load_egl_api(&mesa_egl, "libEGL_mesa.so", NULL)) {
            SHIM_ERR("cannot load Mesa EGL core; shim disabled");
            return;
        }
    }
    if (!load_egl_api(&vendor_egl, "/system/lib64/libEGL.so", "libEGL.so")) {
        SHIM_ERR("cannot load vendor EGL; window surfaces will fail");
        return;
    }
    SHIM_LOG("loaded: mesa=%s vendor=EGL", path);
}
