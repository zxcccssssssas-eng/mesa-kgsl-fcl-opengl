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
#include <string.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define SHIM_LOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define SHIM_ERR(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void *egl_handle;
static __eglMustCastToProperFunctionPointerType (*egl_get_proc_address)(const char *);

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
