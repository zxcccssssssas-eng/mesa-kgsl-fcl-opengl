// SPDX-License-Identifier: MIT
/*
 * FCL GL entry-point shim (libGLESv2_mesa.so)
 * -------------------------------------------
 * Mesa's Android build links the GL dispatch into libEGL and libGLESv2
 * separately (glvnd is disabled and there is no shared libglapi), so a context
 * made current through libEGL is invisible to the entry points exported by
 * libGLESv2.  LWJGL's GL class resolves functions with, in order:
 *
 *   1. glXGetProcAddress() exported by the GL library it loaded
 *   2. dlsym() on that library
 *
 * Mesa's libGLESv2 does not export glXGetProcAddress, so LWJGL ends up with
 * libGLESv2's own stubs, whose dispatch state the EGL shim never touches ->
 * every GL call throws "No context is current".
 *
 * This shim takes the libGLESv2_mesa.so slot and exports glXGetProcAddress /
 * glXGetProcAddressARB / OSMesaGetProcAddress, all forwarding to
 * libEGL_mesa_core.so's eglGetProcAddress().  That is the same library the EGL
 * shim uses for eglMakeCurrent, so the dispatch state matches, and the desktop
 * GL entry points (which the ES-only libGLESv2 does not export) become
 * available too.
 */
#define LOG_TAG "GLESShim"

#include <dlfcn.h>
#include <libgen.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#define SHIM_LOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define SHIM_ERR(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void *egl_handle;
static __eglMustCastToProperFunctionPointerType (*egl_get_proc_address)(const char *);

/* Flywheel's indirect backend uses gl_DrawID with a _flw_baseDraw uniform.
 * Mesa Freedreno on A840 can drop geometry from multi-draw indirect calls.
 * Submit those commands one at a time, matching Flywheel's own Intel fallback.
 * The commands stay GPU-resident, so this avoids a per-draw GPU readback. */
typedef void (*draw_elements_indirect_fn)(GLenum, GLenum, const void *);
typedef void (*multi_draw_elements_indirect_fn)(GLenum, GLenum, const void *, GLsizei, GLsizei);
typedef void (*get_uniform_uiv_fn)(GLuint, GLint, GLuint *);
typedef void (*uniform_1ui_fn)(GLint, GLuint);
typedef GLint (*get_uniform_location_fn)(GLuint, const GLchar *);
typedef void (*get_integer_v_fn)(GLenum, GLint *);

static void shim_multi_draw_elements_indirect(GLenum mode, GLenum type,
                                              const void *indirect, GLsizei drawcount,
                                              GLsizei stride)
{
    multi_draw_elements_indirect_fn multi_draw =
        (multi_draw_elements_indirect_fn)egl_get_proc_address("glMultiDrawElementsIndirect");
    draw_elements_indirect_fn draw =
        (draw_elements_indirect_fn)egl_get_proc_address("glDrawElementsIndirect");
    get_integer_v_fn get_integer =
        (get_integer_v_fn)egl_get_proc_address("glGetIntegerv");
    get_uniform_location_fn get_location =
        (get_uniform_location_fn)egl_get_proc_address("glGetUniformLocation");
    get_uniform_uiv_fn get_uniform =
        (get_uniform_uiv_fn)egl_get_proc_address("glGetUniformuiv");
    uniform_1ui_fn set_uniform =
        (uniform_1ui_fn)egl_get_proc_address("glUniform1ui");

    if (!multi_draw || !draw || !get_integer || !get_location ||
        !get_uniform || !set_uniform || drawcount <= 0) {
        if (multi_draw) multi_draw(mode, type, indirect, drawcount, stride);
        return;
    }

    GLint program = 0;
    get_integer(GL_CURRENT_PROGRAM, &program);
    GLint base_draw_location = program > 0 ?
        get_location((GLuint)program, "_flw_baseDraw") : -1;
    if (base_draw_location < 0) {
        multi_draw(mode, type, indirect, drawcount, stride);
        return;
    }

    GLuint base_draw = 0;
    get_uniform((GLuint)program, base_draw_location, &base_draw);
    size_t command_stride = stride ? (size_t)stride : 5 * sizeof(GLuint);
    uintptr_t command = (uintptr_t)indirect;
    for (GLsizei i = 0; i < drawcount; ++i) {
        set_uniform(base_draw_location, base_draw + (GLuint)i);
        draw(mode, type, (const void *)(command + (size_t)i * command_stride));
    }
    set_uniform(base_draw_location, base_draw);
    static int logged;
    if (!logged) {
        SHIM_LOG("Flywheel indirect compatibility active (%d draws)", drawcount);
        logged = 1;
    }
}

static void load_egl_get_proc_address(void)
{
    if (egl_get_proc_address)
        return;

    char path[4096];
    Dl_info info;
    if (dladdr((void *)&load_egl_get_proc_address, &info) && info.dli_fname) {
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s", info.dli_fname);
        snprintf(path, sizeof(path), "%s/libEGL_mesa_core.so", dirname(tmp));
        egl_handle = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
    }
    if (!egl_handle)
        egl_handle = dlopen("libEGL_mesa_core.so", RTLD_LOCAL | RTLD_LAZY);
    if (!egl_handle) {
        SHIM_ERR("cannot load libEGL_mesa_core.so: %s", dlerror());
        return;
    }
    egl_get_proc_address =
        (__eglMustCastToProperFunctionPointerType(*)(const char *))dlsym(egl_handle,
                                                                        "eglGetProcAddress");
    if (!egl_get_proc_address)
        SHIM_ERR("libEGL_mesa_core.so has no eglGetProcAddress");
    else
        SHIM_LOG("GL entry points now come from libEGL_mesa_core.so");
}

__eglMustCastToProperFunctionPointerType glXGetProcAddress(const unsigned char *procname)
{
    load_egl_get_proc_address();
    if (procname && strcmp((const char *)procname, "glMultiDrawElementsIndirect") == 0)
        return (__eglMustCastToProperFunctionPointerType)shim_multi_draw_elements_indirect;
    return egl_get_proc_address ? egl_get_proc_address((const char *)procname) : NULL;
}

__eglMustCastToProperFunctionPointerType glXGetProcAddressARB(const unsigned char *procname)
{
    return glXGetProcAddress(procname);
}

__eglMustCastToProperFunctionPointerType OSMesaGetProcAddress(const char *procname)
{
    return glXGetProcAddress((const unsigned char *)procname);
}
