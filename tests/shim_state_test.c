// SPDX-License-Identifier: MIT
/* Run on Android: tests the presentation boundary without a GPU or a window. */
#define FCL_SHIM_TEST
#include "../app/src/main/cpp/egl_shim.c"
#include <assert.h>
static int flushed, finished, destroyed, exported;
static EGLenum sync_type;
static EGLSyncKHR create_sync(EGLDisplay d, EGLenum type, const EGLint *attrs)
{ (void)d; (void)attrs; sync_type = type; return (EGLSyncKHR)1; }
static EGLBoolean destroy_sync(EGLDisplay d, EGLSyncKHR sync)
{ (void)d; (void)sync; ++destroyed; return EGL_TRUE; }
static EGLint dup_fence(EGLDisplay d, EGLSyncKHR sync)
{ (void)d; (void)sync; assert(flushed); return exported; }
static void flush(void) { ++flushed; }
static void finish(void) { ++finished; }
static GLint read_fbo = 17, draw_fbo = 23, tex = 31;
static GLint pack = 41, alignment = 8, length = 7, rows = 2, skip = 3, read_buffer = GL_NONE;
static GLboolean scissor = GL_TRUE, srgb = GL_TRUE;
static void get_integer(GLenum name, GLint *value)
{
    switch (name) {
    case GL_READ_FRAMEBUFFER_BINDING: *value = read_fbo; break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *value = draw_fbo; break;
    case GL_TEXTURE_BINDING_2D: *value = tex; break;
    case GL_PIXEL_PACK_BUFFER_BINDING: *value = pack; break;
    case GL_PACK_ALIGNMENT: *value = alignment; break;
    case GL_PACK_ROW_LENGTH: *value = length; break;
    case GL_PACK_SKIP_ROWS: *value = rows; break;
    case GL_PACK_SKIP_PIXELS: *value = skip; break;
    case GL_READ_BUFFER: *value = read_buffer; break;
    case 0x0C32: *value = 0; break; /* Single-buffered desktop pbuffer. */
    default: assert(0);
    }
}
static GLboolean enabled(GLenum cap) { return cap == GL_SCISSOR_TEST ? scissor : srgb; }
static void enable(GLenum cap) { if (cap == GL_SCISSOR_TEST) scissor = GL_TRUE; else srgb = GL_TRUE; }
static void disable(GLenum cap) { if (cap == GL_SCISSOR_TEST) scissor = GL_FALSE; else srgb = GL_FALSE; }
static void bind_fbo(GLenum target, GLuint value)
{
    if (target != GL_DRAW_FRAMEBUFFER) read_fbo = value;
    if (target != GL_READ_FRAMEBUFFER) draw_fbo = value;
}
static void bind_tex(GLenum target, GLuint value) { (void)target; tex = value; }
static void bind_buffer(GLenum target, GLuint value) { assert(target == GL_PIXEL_PACK_BUFFER); pack = value; }
static void read_from(GLenum value) { read_buffer = value; }
static void pixel_store(GLenum name, GLint value)
{
    switch (name) {
    case GL_PACK_ALIGNMENT: alignment = value; break;
    case GL_PACK_ROW_LENGTH: length = value; break;
    case GL_PACK_SKIP_ROWS: rows = value; break;
    case GL_PACK_SKIP_PIXELS: skip = value; break;
    default: assert(0);
    }
}
static const uint8_t bottom_up[] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255 };
static void read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out)
{
    assert(x == 0 && y == 0 && w == 2 && h == 2 && format == GL_RGBA && type == GL_UNSIGNED_BYTE);
    assert(read_fbo == 0 && read_buffer == GL_FRONT && pack == 0);
    assert(alignment == 1 && length == 0 && rows == 0 && skip == 0);
    memcpy(out, bottom_up, sizeof(bottom_up));
}
struct vk_present *vk_present_create(ANativeWindow *w) { (void)w; return NULL; }
void vk_present_destroy(struct vk_present *p) { (void)p; }
int vk_present_size(struct vk_present *p, int *w, int *h) { (void)p; (void)w; (void)h; return 0; }
int vk_present_frame(struct vk_present *p, const uint8_t *rgba, int w, int h)
{
    (void)p; assert(w == 2 && h == 2);
    assert(!memcmp(rgba, bottom_up + 8, 8));
    assert(!memcmp(rgba + 8, bottom_up, 8));
    return 1;
}
int vk_present_prefers_bgra(struct vk_present *p) { (void)p; return 0; }
int vk_present_ahb_available(struct vk_present *p) { (void)p; return 0; }
int vk_present_ahb_slot_wait(struct vk_present *p, AHardwareBuffer *ahb)
{ (void)p; (void)ahb; return 1; }
int vk_present_frame_ahb(struct vk_present *p, AHardwareBuffer *ahb, int fence_fd, int w, int h)
{
    (void)p; (void)ahb; (void)w; (void)h;
    if (fence_fd >= 0) close(fence_fd);
    return 0;
}
void vk_present_idle(struct vk_present *p) { (void)p; }

static EGLenum bound_api;
static int fail_bind, config_calls;
static EGLint requested_type, requested_conformant, requested_depth;
static EGLBoolean bind_api(EGLenum api)
{
    bound_api = api;
    return fail_bind ? EGL_FALSE : EGL_TRUE;
}
static EGLBoolean choose_config(EGLDisplay d, const EGLint *attrs, EGLConfig *configs,
                               EGLint size, EGLint *count)
{
    (void)d; (void)configs; (void)size; (void)count;
    ++config_calls;
    requested_type = EGL_OPENGL_ES_BIT; /* EGL default when omitted. */
    requested_conformant = requested_depth = 0;
    for (int i = 0; attrs && attrs[i] != EGL_NONE; i += 2) {
        if (attrs[i] == EGL_RENDERABLE_TYPE) requested_type = attrs[i + 1];
        if (attrs[i] == EGL_CONFORMANT) requested_conformant = attrs[i + 1];
        if (attrs[i] == EGL_DEPTH_SIZE) requested_depth = attrs[i + 1];
    }
    return EGL_TRUE;
}

static void test_sdl_desktop_gl(void)
{
    unsetenv("POJAV_RENDERER");
    unsetenv("SDL_OPENGL_LIBRARY");
    assert(!uses_sdl_desktop_gl());
    setenv("POJAV_RENDERER", "opengles3_desktopgl", 1);
    assert(!uses_sdl_desktop_gl());
    setenv("SDL_OPENGL_LIBRARY", "/plugin/libGLESv2_mesa.so.other", 1);
    assert(!uses_sdl_desktop_gl());
    setenv("SDL_OPENGL_LIBRARY", "/plugin/libGLESv2_mesa.so", 1);
    assert(uses_sdl_desktop_gl());
    setenv("SDL_OPENGL_LIBRARY", "libGLESv2_mesa.so", 1);
    assert(uses_sdl_desktop_gl());
    setenv("POJAV_RENDERER", "opengles3", 1);
    assert(!uses_sdl_desktop_gl());

    mesa_egl.BindAPI = bind_api;
    mesa_egl.ChooseConfig = choose_config;
    struct shim_display display = {0};
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_CONFORMANT, EGL_OPENGL_ES3_BIT_KHR, EGL_DEPTH_SIZE, 24, EGL_NONE};
    shim_sdl_desktop_gl = 0;
    assert(eglBindAPI(EGL_OPENGL_ES_API) && bound_api == EGL_OPENGL_ES_API);
    assert(eglChooseConfig(&display, attrs, NULL, 0, NULL));
    assert(requested_type == EGL_OPENGL_ES2_BIT && requested_conformant == EGL_OPENGL_ES3_BIT_KHR);
    shim_sdl_desktop_gl = 1;
    assert(eglBindAPI(EGL_OPENGL_ES_API) && bound_api == EGL_OPENGL_API);
    assert(shim_api == EGL_OPENGL_API);
    assert(eglChooseConfig(&display, attrs, NULL, 0, NULL));
    assert(requested_type == EGL_OPENGL_BIT && requested_conformant == EGL_OPENGL_BIT);
    assert(requested_depth == 24 && attrs[1] == EGL_OPENGL_ES2_BIT);
    const EGLint unrestricted[] = {EGL_RENDERABLE_TYPE, EGL_DONT_CARE,
        EGL_CONFORMANT, 0, EGL_NONE};
    assert(eglChooseConfig(&display, unrestricted, NULL, 0, NULL));
    assert(requested_type == EGL_DONT_CARE && requested_conformant == 0);
    const EGLint desktop[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_CONFORMANT, EGL_DONT_CARE, EGL_NONE};
    assert(eglChooseConfig(&display, desktop, NULL, 0, NULL));
    assert(requested_type == EGL_OPENGL_BIT && requested_conformant == EGL_DONT_CARE);
    assert(eglChooseConfig(&display, NULL, NULL, 0, NULL));
    assert(requested_type == EGL_OPENGL_BIT);
    const EGLint depth_only[] = {EGL_DEPTH_SIZE, 16, EGL_NONE};
    assert(eglChooseConfig(&display, depth_only, NULL, 0, NULL));
    assert(requested_type == EGL_OPENGL_BIT && requested_depth == 16);
    EGLint too_long[129];
    for (int i = 0; i < 128; i += 2) {
        too_long[i] = EGL_DEPTH_SIZE;
        too_long[i + 1] = 24;
    }
    too_long[128] = EGL_NONE;
    int previous_calls = config_calls;
    assert(!eglChooseConfig(&display, too_long, NULL, 0, NULL));
    assert(shim_error == EGL_BAD_ATTRIBUTE && config_calls == previous_calls);
    assert(eglBindAPI(EGL_OPENGL_API) && bound_api == EGL_OPENGL_API);
    fail_bind = 1;
    assert(!eglBindAPI(0));
    assert(bound_api == 0 && shim_api == EGL_OPENGL_API);
}

int main(void)
{
    (void)shim_init; /* Constructor deliberately disabled in this test. */
    struct egl_api api = {.CreateSyncKHR = create_sync, .DestroySyncKHR = destroy_sync,
                          .DupNativeFenceFDANDROID = dup_fence};
    struct gl_api gl = {.Flush = flush, .Finish = finish};
    exported = 77;
    assert(fence_export(&api, &gl, EGL_NO_DISPLAY) == 77);
    assert(sync_type == EGL_SYNC_NATIVE_FENCE_ANDROID && flushed == 1 && destroyed == 1 && !finished);
    exported = -1;
    assert(fence_export(&api, &gl, EGL_NO_DISPLAY) == -1 && finished == 1);
    api.CreateSyncKHR = NULL;
    assert(fence_export(&api, &gl, EGL_NO_DISPLAY) == -1 && finished == 2);
    int fds[2]; assert(pipe(fds) == 0); assert(write(fds[1], "x", 1) == 1);
    struct egl_api no_sync = {0}; /* No EGL sync entry points: poll the fence fd. */
    assert(fence_wait(&no_sync, EGL_NO_DISPLAY, fds[0])); close(fds[1]);
    assert(!fence_wait(&no_sync, EGL_NO_DISPLAY, fds[0])); /* Closed FD must fail. */
    mesa_gl = (struct gl_api){.GetIntegerv = get_integer, .IsEnabled = enabled,
        .Enable = enable, .Disable = disable, .BindFramebuffer = bind_fbo, .BindTexture = bind_tex,
        .BindBuffer = bind_buffer, .ReadBuffer = read_from, .PixelStorei = pixel_store, .ReadPixels = read_pixels};
    shim_api = EGL_OPENGL_API;
    struct shim_surface surface = {.width = 2, .height = 2};
    assert(vulkan_present(&surface));
    assert(read_fbo == 17 && draw_fbo == 23 && tex == 31 && scissor && srgb);
    assert(pack == 41 && alignment == 8 && length == 7 && rows == 2 && skip == 3 && read_buffer == GL_NONE);
    free(surface.pixels);
    test_sdl_desktop_gl();
    puts("PASS: native fences, completion fallback, invalid fences, GL state, pixel-pack state, vertical orientation, SDL desktop API/config");
    return 0;
}
