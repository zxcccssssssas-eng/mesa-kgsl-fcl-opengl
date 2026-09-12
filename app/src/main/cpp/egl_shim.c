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

#include "vulkan_present.h"
#include <poll.h>
#include <time.h>
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
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
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
    void (*GetIntegerv)(GLenum, GLint *);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    GLboolean (*IsEnabled)(GLenum);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*PixelStorei)(GLenum, GLint);
    void (*BindBuffer)(GLenum, GLuint);
    void (*ReadBuffer)(GLenum);
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
    LOAD_GL(GetIntegerv, "glGetIntegerv");
    LOAD_GL(Viewport, "glViewport");
    LOAD_GL(Scissor, "glScissor");
    LOAD_GL(ClearColor, "glClearColor");
    LOAD_GL(Clear, "glClear");
    LOAD_GL(IsEnabled, "glIsEnabled");
    LOAD_GL(Enable, "glEnable");
    LOAD_GL(Disable, "glDisable");
    LOAD_GL(CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD_GL(ReadPixels, "glReadPixels");
    LOAD_GL(PixelStorei, "glPixelStorei");
    LOAD_GL(BindBuffer, "glBindBuffer");
    LOAD_GL(ReadBuffer, "glReadBuffer");
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
    int mesa_window;  /* zink mode: Mesa owns the window surface (no AHB/presenter) */
    struct vk_present *vk;
    int vk_ahb;       /* zero-copy AHB sampling available/selected */
    uint8_t *pixels;
    EGLConfig mesa_config;
    EGLSurface mesa_surface;
    EGLSurface vendor_window;
    int width, height;
    struct shim_surface *next;
    struct shim_context *ring_ctx;
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
    struct shim_surface *surfaces;
    int failed;
};

static struct shim_display display_singleton;
static int display_used;

static __thread struct shim_display *tls_dpy;
static __thread struct shim_context *tls_ctx;
static __thread struct shim_surface *tls_draw;
static __thread struct shim_surface *tls_read;

static __thread EGLint shim_error = EGL_SUCCESS;
static __thread EGLenum shim_api = EGL_OPENGL_ES_API;
/* FCL_SHIM_GALLIUM=zink: render through zink and let Mesa present the window
 * itself (the droid window path FCL's built-in Zink uses); the AHB ring and the
 * vendor/Vulkan presenters stay unused because zink cannot import our AHBs on
 * this driver (no VK_EXT_image_drm_format_modifier). */
static int shim_zink_mode;

static EGLint set_error(EGLint err)
{
    shim_error = err;
    return err;
}

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
struct saved_gl { GLint read, draw, tex; GLboolean scissor, srgb; };
static struct saved_gl save_gl(void)
{
    struct saved_gl st;
    mesa_gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &st.read);
    mesa_gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &st.draw);
    mesa_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &st.tex);
    st.scissor = mesa_gl.IsEnabled(GL_SCISSOR_TEST);
    st.srgb = shim_api == EGL_OPENGL_API ? mesa_gl.IsEnabled(GL_FRAMEBUFFER_SRGB) : GL_FALSE;
    mesa_gl.Disable(GL_SCISSOR_TEST);
    if (shim_api == EGL_OPENGL_API) mesa_gl.Disable(GL_FRAMEBUFFER_SRGB);
    return st;
}
static void restore_gl(struct saved_gl st)
{
    mesa_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, st.read);
    mesa_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, st.draw);
    mesa_gl.BindTexture(GL_TEXTURE_2D, st.tex);
    if (st.scissor) mesa_gl.Enable(GL_SCISSOR_TEST);
    if (st.srgb) mesa_gl.Enable(GL_FRAMEBUFFER_SRGB);
}
static int restore_mesa(struct shim_display *d)
{
    if (d->vendor_ok)
        vendor_egl.MakeCurrent(d->vendor_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return mesa_egl.MakeCurrent(d->mesa_dpy,
        tls_draw ? tls_draw->mesa_surface : EGL_NO_SURFACE,
        tls_read ? tls_read->mesa_surface : EGL_NO_SURFACE,
        tls_ctx ? tls_ctx->mesa_ctx : EGL_NO_CONTEXT);
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
        /* Finish outstanding reads before releasing imported storage. */
        struct pollfd pfd = { .fd = slot->vendor_fence_fd, .events = POLLIN };
        while (poll(&pfd, 1, -1) < 0 && errno == EINTR) {}
        close(slot->vendor_fence_fd);
        slot->vendor_fence_fd = -1;
    }
    if (slot->mesa_fbo && mesa_gl.DeleteFramebuffers)
        mesa_gl.DeleteFramebuffers(1, &slot->mesa_fbo);
    if (slot->mesa_tex && mesa_gl.DeleteTextures)
        mesa_gl.DeleteTextures(1, &slot->mesa_tex);
    if (slot->mesa_image && mesa_egl.DestroyImageKHR)
        mesa_egl.DestroyImageKHR(d->mesa_dpy, slot->mesa_image);
    if (slot->vendor_tex || slot->vendor_fbo) {
        if (vendor_egl.MakeCurrent(d->vendor_dpy, d->vendor_scratch, d->vendor_scratch, d->vendor_ctx)) {
            vendor_gl.Finish();
            if (slot->vendor_fbo) vendor_gl.DeleteFramebuffers(1, &slot->vendor_fbo);
            if (slot->vendor_tex) vendor_gl.DeleteTextures(1, &slot->vendor_tex);
        }
        restore_mesa(d);
    }
    if (slot->vendor_image && vendor_egl.DestroyImageKHR)
        vendor_egl.DestroyImageKHR(d->vendor_dpy, slot->vendor_image);
    if (slot->ahb)
        AHardwareBuffer_release(slot->ahb);
    memset(slot, 0, sizeof(*slot));
    slot->vendor_fence_fd = -1;
}

static void ring_destroy(struct shim_surface *s)
{
    if (s->vk) vk_present_idle(s->vk);
    for (int i = 0; i < RING_SIZE; i++) {
        if (s->ring_ctx) {
            if (!mesa_egl.MakeCurrent(s->dpy->mesa_dpy, s->mesa_surface, s->mesa_surface,
                                     s->ring_ctx->mesa_ctx)) {
                SHIM_ERR("cannot bind ring owner for cleanup");
                return;
            }
            mesa_gl.Finish();
        }
        ring_slot_destroy(s->dpy, &s->ring[i]);
    }
    if (s->ring_ctx) restore_mesa(s->dpy);
    s->ring_ctx = NULL;
    s->ring_ready = 0;
}

/* Creates the AHB ring; requires the Mesa context to be current (for the
 * Mesa-side texture/FBO) and temporarily makes the vendor context current. */
static int ring_create(struct shim_surface *s)
{
    struct shim_display *d = s->dpy;
    if (s->ring_ready && s->ring_ctx == tls_ctx) return 1;
    if (s->ring_ctx) ring_destroy(s);
    s->ring_ctx = tls_ctx;
    if (!mesa_gl.GenTextures || !mesa_gl.EGLImageTargetTexture2DOES ||
        !mesa_egl.CreateImageKHR || !vendor_egl.CreateImageKHR ||
        !vendor_egl.GetNativeClientBufferANDROID) {
        SHIM_ERR("ring: required AHB import entry points missing");
        return 0;
    }
    int want_vendor = (s->vk == NULL); /* Vulkan samples the AHB itself */
    if (want_vendor) {
        if (!vendor_gl.GenTextures && d->vendor_ok)
            load_gl_api(&vendor_gl, &vendor_egl, "vendor");
        if (!vendor_gl.GenTextures || !vendor_gl.EGLImageTargetTexture2DOES) {
            SHIM_ERR("ring: vendor GL entry points missing");
            return 0;
        }
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
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                     AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
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
        if (mesa_gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            SHIM_ERR("ring: incomplete Mesa framebuffer"); goto fail;
        }
        /* Vendor side: texture sampled by the presentation blit (EGL backend). */
        if (!want_vendor) goto check;
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
        if (vendor_gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            SHIM_ERR("ring: incomplete vendor framebuffer"); goto fail;
        }
        if (!restore_mesa(d)) goto fail;
        vendor_current = 0;
        continue;

check:
        /* One-time probe: is the driver writing the AHB with the layout the
         * platform reports?  A wrong row pitch shows up as stripe drift. */
        if (i == 0) {
            GLint vp[4] = {0, 0, 0, 0};
            mesa_gl.GetIntegerv(0x0BA2 /* GL_VIEWPORT */, vp);
            GLboolean scissor = mesa_gl.IsEnabled(0x0C11 /* GL_SCISSOR_TEST */);
            GLfloat old_clear[4];
            {
                typedef void (*glGetFloatv_t)(GLenum, GLfloat *);
                glGetFloatv_t glGetFloatv = (glGetFloatv_t)mesa_egl.GetProcAddress("glGetFloatv");
                if (glGetFloatv) glGetFloatv(0x0C22 /* GL_COLOR_CLEAR_VALUE */, old_clear);
            }
            mesa_gl.Enable(0x0C11);
            mesa_gl.Viewport(0, 0, s->width, s->height);
            int stripe = s->width / 8;
            for (int k = 0; k < 8; ++k) {
                int on = (k % 2) == 0;
                mesa_gl.ClearColor(on ? 1.0f : 0.0f, 0.3f, 0.0f, 1.0f);
                mesa_gl.Scissor(k * stripe, 0, k == 7 ? s->width - 7 * stripe : stripe, s->height);
                mesa_gl.Clear(GL_COLOR_BUFFER_BIT);
            }
            mesa_gl.Finish();
            if (!scissor) mesa_gl.Disable(0x0C11);
            mesa_gl.Viewport(vp[0], vp[1], vp[2], vp[3]);

            AHardwareBuffer_Desc desc;
            AHardwareBuffer_describe(slot->ahb, &desc);
            void *ptr = NULL;
            if (AHardwareBuffer_lock(slot->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL,
                                     &ptr) == 0 && ptr) {
                const uint8_t *px = ptr;
                int first = -1, drift = 0;
                for (int y = 0; y < s->height; y += s->height / 16) {
                    const uint8_t *row = px + (size_t)y * desc.stride * 4;
                    int edge = -1;
                    for (int x = 4; x < s->width && x < 1600; x += 4) {
                        int d = (int)row[x * 4] - (int)row[(x - 4) * 4];
                        if (d > 100 || d < -100) { edge = x; break; }
                    }
                    if (edge < 0) continue;
                    if (first < 0) first = edge;
                    else if (edge != first) drift++;
                }
                SHIM_LOG("AHB layout self-check: stride=%u px, first stripe edge=%d, drifting rows=%d",
                         desc.stride, first, drift);
                if (drift > 2)
                    SHIM_ERR("AHB layout mismatch: Mesa writes with a different row pitch than the "
                             "platform reports (image will be sheared)");
                AHardwareBuffer_unlock(slot->ahb, NULL);
            }
        }
        continue;
    }

    /* restore Mesa as current (caller had it current) */
    if (vendor_current && tls_ctx)
        restore_mesa(d);
    s->ring_ready = 1;
    SHIM_LOG("AHB ring ready: %dx%d", s->width, s->height);
    return 1;

fail:
    if (vendor_current && tls_ctx)
        restore_mesa(d);
    ring_destroy(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* presentation                                                       */
/* ------------------------------------------------------------------ */

static void frame_tick(void)
{
    static struct timespec last;
    static unsigned frames;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    frames++;
    if (last.tv_sec == 0) { last = now; return; }
    double dt = (double)(now.tv_sec - last.tv_sec) +
                (double)(now.tv_nsec - last.tv_nsec) / 1e9;
    if (dt >= 5.0) {
        SHIM_LOG("present fps: %.1f (%u frames / %.1fs)", (double)frames / dt, frames, dt);
        frames = 0;
        last = now;
    }
}

static int fence_wait(struct egl_api *api, EGLDisplay dpy, int fd)
{
    if (fd < 0) return 1;
    /* GPU-side wait when the display supports EGL_KHR_wait_sync: the consumer
     * queue waits on the fence in hardware instead of blocking the CPU. */
    if (api->CreateSyncKHR && api->DestroySyncKHR && api->WaitSyncKHR) {
        const EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE };
        EGLSyncKHR sync = api->CreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
        if (sync != EGL_NO_SYNC_KHR) {
            EGLBoolean ok = api->WaitSyncKHR(dpy, sync, 0);
            api->DestroySyncKHR(dpy, sync); /* consumes the fd */
            return ok == EGL_TRUE;
        }
        close(fd); /* not consumed on failure */
        return 0;
    }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int result;
    do { result = poll(&pfd, 1, -1); } while (result < 0 && errno == EINTR);
    close(fd);
    return result > 0 && (pfd.revents & POLLIN) && !(pfd.revents & (POLLERR | POLLNVAL));
}
static int fence_export(struct egl_api *api, struct gl_api *gl, EGLDisplay dpy)
{
    int fd = -1;
    if (api->CreateSyncKHR && api->DestroySyncKHR && api->DupNativeFenceFDANDROID) {
        EGLSyncKHR sync = api->CreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            gl->Flush(); /* Submit the fence before exporting it. */
            fd = api->DupNativeFenceFDANDROID(dpy, sync);
            api->DestroySyncKHR(dpy, sync);
        }
    }
    if (fd < 0) {
        if (api->GetError) api->GetError(); /* Do not expose an internal fallback error. */
        gl->Finish(); /* Completion of the blit, not just preceding draws. */
    }
    return fd;
}

static int vulkan_present(struct shim_surface *s)
{
    size_t row = (size_t)s->width * 4;
    if ((size_t)s->height > SIZE_MAX / row) return 0;
    if (!s->pixels) s->pixels = malloc(row * s->height);
    if (!s->pixels) return 0;
    struct saved_gl st = save_gl();
    GLint pack, align, length, rows, skip, read;
    mesa_gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    mesa_gl.GetIntegerv(GL_PACK_ALIGNMENT, &align);
    mesa_gl.GetIntegerv(GL_PACK_ROW_LENGTH, &length);
    mesa_gl.GetIntegerv(GL_PACK_SKIP_ROWS, &rows);
    mesa_gl.GetIntegerv(GL_PACK_SKIP_PIXELS, &skip);
    mesa_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    mesa_gl.GetIntegerv(GL_READ_BUFFER, &read);
    GLint doublebuffer = 1;
    if (shim_api == EGL_OPENGL_API) mesa_gl.GetIntegerv(0x0C32 /* GL_DOUBLEBUFFER */, &doublebuffer);
    mesa_gl.ReadBuffer(doublebuffer ? GL_BACK : GL_FRONT);
    mesa_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    mesa_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    mesa_gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    mesa_gl.PixelStorei(GL_PACK_SKIP_ROWS, 0);
    mesa_gl.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    /* Feed the swapchain its native channel order: desktop GL supports
     * GL_BGRA readback, which removes the per-frame CPU channel swap. */
    GLenum read_format = vk_present_prefers_bgra(s->vk) ? 0x80E1 /* GL_BGRA */ : GL_RGBA;
    mesa_gl.ReadPixels(0, 0, s->width, s->height, read_format, GL_UNSIGNED_BYTE, s->pixels);
    mesa_gl.ReadBuffer(read);
    mesa_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pack);
    mesa_gl.PixelStorei(GL_PACK_ALIGNMENT, align);
    mesa_gl.PixelStorei(GL_PACK_ROW_LENGTH, length);
    mesa_gl.PixelStorei(GL_PACK_SKIP_ROWS, rows);
    mesa_gl.PixelStorei(GL_PACK_SKIP_PIXELS, skip);
    restore_gl(st);
    /* GL starts at the bottom; Vulkan buffer-to-image copies start at the top. */
    for (int y = 0; y < s->height / 2; ++y) {
        uint8_t *a = s->pixels + y * row, *b = s->pixels + (s->height - 1 - y) * row;
        for (size_t x = 0; x < row; ++x) { uint8_t t = a[x]; a[x] = b[x]; b[x] = t; }
    }
    return vk_present_frame(s->vk, s->pixels, s->width, s->height);
}

/* Mesa: pbuffer FBO 0 -> the ring slot's AHB FBO.  Returns the native fence
 * fd for the blit (>= 0), or -1 when the export was unavailable and the blit
 * was completed with glFinish instead. */
static int blit_to_slot(struct shim_surface *s, struct shim_ring_slot *slot)
{
    struct saved_gl saved = save_gl();
    mesa_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    mesa_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->mesa_fbo);
    GLint read_buffer, doublebuffer = 1;
    mesa_gl.GetIntegerv(GL_READ_BUFFER, &read_buffer);
    if (shim_api == EGL_OPENGL_API) mesa_gl.GetIntegerv(0x0C32 /* GL_DOUBLEBUFFER */, &doublebuffer);
    mesa_gl.ReadBuffer(doublebuffer ? GL_BACK : GL_FRONT);
    mesa_gl.BlitFramebuffer(0, 0, s->width, s->height, 0, 0, s->width, s->height,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    mesa_gl.ReadBuffer(read_buffer);
    mesa_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    mesa_gl.Flush();
    int fd = fence_export(&mesa_egl, &mesa_gl, s->dpy->mesa_dpy);
    restore_gl(saved);
    return fd;
}

/* Zero-copy Vulkan presentation: Mesa renders into the ring AHB, the AHB is
 * imported as a VkImage and sampled by a fullscreen triangle.  Returns 1 on
 * success, -1 to fall back to the CPU upload path. */
static int vulkan_present_ahb(struct shim_surface *s)
{
    if (!ring_create(s)) return -1;
    struct shim_ring_slot *slot = &s->ring[s->cur];
    if (!vk_present_ahb_slot_wait(s->vk, slot->ahb)) return -1;
    int fd = blit_to_slot(s, slot);
    /* vk_present_frame_ahb takes ownership of fd even on failure. */
    if (!vk_present_frame_ahb(s->vk, slot->ahb, fd, s->width, s->height))
        return -1;
    s->cur ^= 1;
    return 1;
}

static int shim_present(struct shim_surface *s, struct shim_context *ctx)
{
    struct shim_display *d = s->dpy;

    if (!ctx) {
        SHIM_ERR("eglSwapBuffers without a current context");
        return 0;
    }
    if (!s->vk && !d->vendor_ok && !vendor_init(d))
        return 0;
    mesa_egl.BindAPI(shim_api);

    /* Track window size changes (FCL recreates the surface, but be safe). */
    EGLint w = 0, h = 0;
    if (s->mesa_window) {
        mesa_egl.QuerySurface(d->mesa_dpy, s->mesa_surface, EGL_WIDTH, &w);
        mesa_egl.QuerySurface(d->mesa_dpy, s->mesa_surface, EGL_HEIGHT, &h);
        s->width = w > 0 ? w : s->width;
        s->height = h > 0 ? h : s->height;
        return 1; /* nothing else to do for the Mesa window surface */
    } else if (s->vk) {
        if (!vk_present_size(s->vk, &w, &h)) return 0;
    } else {
        vendor_egl.QuerySurface(d->vendor_dpy, s->vendor_window, EGL_WIDTH, &w);
        vendor_egl.QuerySurface(d->vendor_dpy, s->vendor_window, EGL_HEIGHT, &h);
    }
    if (w > 0 && h > 0 && (w != s->width || h != s->height)) {
        SHIM_LOG("window resized %dx%d -> %dx%d", s->width, s->height, w, h);
        const EGLint pb[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
        EGLSurface next = mesa_egl.CreatePbufferSurface(d->mesa_dpy, s->mesa_config, pb);
        if (next == EGL_NO_SURFACE) return 0;
        EGLSurface old = s->mesa_surface;
        s->mesa_surface = next;
        if (!restore_mesa(d)) {
            s->mesa_surface = old;
            mesa_egl.DestroySurface(d->mesa_dpy, next);
            return 0;
        }
        ring_destroy(s);
        mesa_egl.DestroySurface(d->mesa_dpy, old);
        free(s->pixels); s->pixels = NULL;
        s->width = w; s->height = h;
        return 1; /* New pbuffer has no rendered content until the next frame. */
    }
    if (s->mesa_window) {
        /* zink mode: the Mesa window surface is already current; let Mesa/zink
         * present it (no readback, no AHB, no vendor GLES). */
        EGLBoolean ok = mesa_egl.SwapBuffers(d->mesa_dpy, s->mesa_surface);
        if (!ok) {
            SHIM_ERR("Mesa eglSwapBuffers failed: 0x%x", mesa_egl.GetError());
            set_error(EGL_BAD_SURFACE);
            return 0;
        }
        return 1;
    }
    if (s->vk) {
        if (s->vk_ahb && vk_present_ahb_available(s->vk)) {
            int r = vulkan_present_ahb(s);
            if (r == 1) return 1;
            s->vk_ahb = 0;
            SHIM_LOG("zero-copy AHB presentation failed; using CPU upload fallback");
        }
        return vulkan_present(s);
    }
    struct saved_gl saved = save_gl();

    if (!ring_create(s)) { restore_gl(saved); return 0; }

    struct shim_ring_slot *slot = &s->ring[s->cur];

    /* Wait until the vendor is done with this slot before overwriting it. */
    if (slot->vendor_fence_fd >= 0) {
        int ok = fence_wait(&mesa_egl, d->mesa_dpy, slot->vendor_fence_fd);
        slot->vendor_fence_fd = -1;
        if (!ok) { restore_gl(saved); return 0; }
    }

    int fence_fd = blit_to_slot(s, slot);
    restore_gl(saved);

    /* Vendor: wait for Mesa, blit AHB -> window, swap. */
    if (!vendor_egl.MakeCurrent(d->vendor_dpy, s->vendor_window, s->vendor_window,
                                d->vendor_ctx)) {
        SHIM_ERR("vendor eglMakeCurrent(window) failed: 0x%x", vendor_egl.GetError());
        if (fence_fd >= 0)
            close(fence_fd);
        restore_mesa(d);
        return 0;
    }
    if (!fence_wait(&vendor_egl, d->vendor_dpy, fence_fd)) {
        restore_mesa(d); return 0;
    }

    vendor_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, slot->vendor_fbo);
    vendor_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    vendor_gl.BlitFramebuffer(0, 0, s->width, s->height, 0, 0, s->width, s->height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
    vendor_gl.Flush();

    slot->vendor_fence_fd = fence_export(&vendor_egl, &vendor_gl, d->vendor_dpy);

    EGLBoolean swapped = vendor_egl.SwapBuffers(d->vendor_dpy, s->vendor_window);

    /* Restore the game's Mesa context. */
    if (!restore_mesa(d)) {
        SHIM_ERR("restore Mesa eglMakeCurrent failed: 0x%x", mesa_egl.GetError());
        return 0;
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
    if (!mesa_egl.GetDisplay) { set_error(EGL_NOT_INITIALIZED); return EGL_NO_DISPLAY; }
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
    for (struct shim_surface *s = d->surfaces; s; s = s->next)
        if (s->ring_ctx == c) ring_destroy(s);
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
    if (shim_zink_mode) {
        EGLSurface mwin = mesa_egl.CreateWindowSurface(d->mesa_dpy, config, win, attrib_list);
        if (mwin == EGL_NO_SURFACE) {
            SHIM_ERR("zink mode: Mesa eglCreateWindowSurface failed: 0x%x", mesa_egl.GetError());
            set_error(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }
        struct shim_surface *zs = calloc(1, sizeof(*zs));
        zs->dpy = d;
        zs->is_window = 1;
        zs->mesa_window = 1;
        zs->mesa_config = config;
        zs->mesa_surface = mwin;
        for (int i = 0; i < RING_SIZE; i++)
            zs->ring[i].vendor_fence_fd = -1;
        EGLint zw = 0, zh = 0;
        mesa_egl.QuerySurface(d->mesa_dpy, mwin, EGL_WIDTH, &zw);
        mesa_egl.QuerySurface(d->mesa_dpy, mwin, EGL_HEIGHT, &zh);
        zs->width = zw > 0 ? zw : 16;
        zs->height = zh > 0 ? zh : 16;
        SHIM_LOG("window surface %p (%dx%d, Mesa window, zink present)",
                 (void *)zs, zs->width, zs->height);
        return (EGLSurface)zs;
    }
    const char *backend = getenv("FCL_SHIM_RENDERER");
    struct vk_present *vk = NULL;
    if (backend && !strcmp(backend, "vulkan")) {
        vk = vk_present_create(win);
        if (!vk) SHIM_ERR("Vulkan initialization failed; falling back to EGL");
    } else if (backend && strcmp(backend, "egl")) {
        SHIM_ERR("unknown FCL_SHIM_RENDERER=%s; using EGL", backend);
    }
    if (!vk && !d->vendor_ok && !vendor_init(d)) {
        set_error(EGL_NOT_INITIALIZED);
        return EGL_NO_SURFACE;
    }
    EGLSurface vwin = vk ? EGL_NO_SURFACE : vendor_egl.CreateWindowSurface(
        d->vendor_dpy, d->vendor_cfg, win, attrib_list);
    if (!vk && vwin == EGL_NO_SURFACE) {
        SHIM_ERR("vendor eglCreateWindowSurface failed: 0x%x", vendor_egl.GetError());
        set_error(EGL_BAD_NATIVE_WINDOW);
        return EGL_NO_SURFACE;
    }
    EGLint w = 0, h = 0;
    if (vk) vk_present_size(vk, &w, &h);
    else {
        vendor_egl.QuerySurface(d->vendor_dpy, vwin, EGL_WIDTH, &w);
        vendor_egl.QuerySurface(d->vendor_dpy, vwin, EGL_HEIGHT, &h);
    }
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
        if (vk) vk_present_destroy(vk);
        else vendor_egl.DestroySurface(d->vendor_dpy, vwin);
        set_error(EGL_BAD_MATCH);
        return EGL_NO_SURFACE;
    }

    struct shim_surface *s = calloc(1, sizeof(*s));
    s->dpy = d;
    s->next = d->surfaces;
    d->surfaces = s;
    s->is_window = 1;
    s->vk = vk;
    s->vk_ahb = vk ? vk_present_ahb_available(vk) : 0;
    {
        const char *ahb_env = getenv("FCL_SHIM_VK_AHB");
        if (s->vk_ahb && ahb_env && ahb_env[0] == '0') {
            s->vk_ahb = 0;
            SHIM_LOG("FCL_SHIM_VK_AHB=0: forcing CPU upload presentation");
        }
    }
    SHIM_LOG("presentation backend: %s", vk
                 ? (s->vk_ahb ? "vulkan (zero-copy AHB)" : "vulkan (CPU upload)")
                 : "egl (AHB)");
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
    struct shim_surface **link = &d->surfaces;
    while (*link && *link != s) link = &(*link)->next;
    if (*link) *link = s->next;
    vk_present_destroy(s->vk);
    free(s->pixels);
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
    frame_tick();
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    struct shim_display *d = (struct shim_display *)dpy;
    if (!d)
        return EGL_FALSE;
    if (tls_draw && tls_draw->mesa_window)
        return mesa_egl.SwapInterval(d->mesa_dpy, interval);
    if (tls_draw && tls_draw->vk) return EGL_TRUE; /* Vulkan uses FIFO. */
    if (tls_draw && tls_draw->is_window) {
        if (!vendor_egl.MakeCurrent(d->vendor_dpy, tls_draw->vendor_window,
                                   tls_draw->vendor_window, d->vendor_ctx)) return EGL_FALSE;
        EGLBoolean ok = vendor_egl.SwapInterval(d->vendor_dpy, interval);
        if (!restore_mesa(d)) return EGL_FALSE;
        return ok;
    }
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
    if (s->is_window && s->mesa_window)
        return mesa_egl.QuerySurface(d->mesa_dpy, s->mesa_surface, attribute, value);
    if (s->is_window && (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)) {
        if (s->vk) {
            int w, h;
            if (!vk_present_size(s->vk, &w, &h)) return EGL_FALSE;
            *value = attribute == EGL_WIDTH ? w : h;
            return EGL_TRUE;
        }
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
    EGLBoolean ok = mesa_egl.BindAPI(api);
    if (ok) shim_api = api;
    return ok;
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

#ifndef FCL_SHIM_TEST
__attribute__((constructor))
#endif
static void shim_init(void)
{
    char path[4096];
    const char *dir = self_dir();
    const char *gallium = getenv("FCL_SHIM_GALLIUM");
    shim_zink_mode = gallium && strcmp(gallium, "zink") == 0;

    snprintf(path, sizeof(path), "%s/libEGL_mesa_core.so", dir);
    if (!load_egl_api(&mesa_egl, path, NULL)) {
        if (!load_egl_api(&mesa_egl, "libEGL_mesa_core.so", NULL)) {
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
