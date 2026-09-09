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
    assert(fence_wait(NULL, EGL_NO_DISPLAY, fds[0])); close(fds[1]);
    assert(!fence_wait(NULL, EGL_NO_DISPLAY, fds[0])); /* Closed FD must fail. */
    mesa_gl = (struct gl_api){.GetIntegerv = get_integer, .IsEnabled = enabled,
        .Enable = enable, .Disable = disable, .BindFramebuffer = bind_fbo, .BindTexture = bind_tex,
        .BindBuffer = bind_buffer, .ReadBuffer = read_from, .PixelStorei = pixel_store, .ReadPixels = read_pixels};
    shim_api = EGL_OPENGL_API;
    struct shim_surface surface = {.width = 2, .height = 2};
    assert(vulkan_present(&surface));
    assert(read_fbo == 17 && draw_fbo == 23 && tex == 31 && scissor && srgb);
    assert(pack == 41 && alignment == 8 && length == 7 && rows == 2 && skip == 3 && read_buffer == GL_NONE);
    free(surface.pixels);
    puts("PASS: native fences, completion fallback, invalid fences, GL state, pixel-pack state, vertical orientation");
    return 0;
}
