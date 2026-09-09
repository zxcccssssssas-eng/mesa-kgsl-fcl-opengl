// SPDX-License-Identifier: MIT
#include <jni.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define LOAD_EGL(name) __typeof__(&name) p_##name = dlsym(lib, #name); if (!p_##name) FAIL("missing " #name)
#define LOAD_GL(name) __typeof__(&name) p_##name = (void *)p_eglGetProcAddress(#name); if (!p_##name) FAIL("missing " #name)
#define FAIL(msg) return (*env)->NewStringUTF(env, "FAIL: " msg)
#define CHECK_GL() do { GLenum e = p_glGetError(); if (e) { char b[100]; snprintf(b,sizeof(b),"FAIL: GL 0x%x at line %d",e,__LINE__); return (*env)->NewStringUTF(env,b); } } while(0)
JNIEXPORT jstring JNICALL Java_com_mio_plugin_shimtest_VisualTest_run(
    JNIEnv *env, jclass cls, jobject surface, jstring backend, jstring dir)
{
    (void)cls;
    const char *mode = (*env)->GetStringUTFChars(env, backend, NULL);
    setenv("FCL_SHIM_RENDERER", mode, 1);
    (*env)->ReleaseStringUTFChars(env, backend, mode);
    setenv("GALLIUM_DRIVER", "freedreno", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "kgsl", 1);
    setenv("FD_FORCE_KGSL", "1", 1);
    setenv("FCL_PROBE", "0", 1);
    const char *folder = (*env)->GetStringUTFChars(env, dir, NULL);
    char path[4096]; snprintf(path, sizeof(path), "%s/libEGL_mesa.so", folder);
    (*env)->ReleaseStringUTFChars(env, dir, folder);
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) return (*env)->NewStringUTF(env, dlerror());
    LOAD_EGL(eglGetDisplay); LOAD_EGL(eglInitialize); LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglBindAPI); LOAD_EGL(eglCreateContext); LOAD_EGL(eglCreateWindowSurface);
    LOAD_EGL(eglMakeCurrent); LOAD_EGL(eglSwapBuffers); LOAD_EGL(eglGetProcAddress);
    LOAD_EGL(eglDestroySurface); LOAD_EGL(eglDestroyContext); LOAD_EGL(eglGetError);
    LOAD_EGL(eglQuerySurface); LOAD_EGL(eglSwapInterval);
    EGLDisplay d = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!p_eglInitialize(d, NULL, NULL)) FAIL("eglInitialize");
    if (!p_eglBindAPI(EGL_OPENGL_API)) FAIL("eglBindAPI");
    EGLConfig cfg; EGLint count;
    EGLint attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    if (!p_eglChooseConfig(d, attr, &cfg, 1, &count) || !count) FAIL("eglChooseConfig");
    EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,EGL_NONE};
    EGLContext c = p_eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);
    if (!c) FAIL("eglCreateContext");
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    EGLSurface s = p_eglCreateWindowSurface(d,cfg,window,NULL);
    if (!s) FAIL("eglCreateWindowSurface");
    if (!p_eglMakeCurrent(d,s,s,c)) FAIL("eglMakeCurrent");
    if (!p_eglSwapInterval(d, 1)) FAIL("eglSwapInterval");
    LOAD_GL(glClearColor); LOAD_GL(glClear); LOAD_GL(glScissor); LOAD_GL(glEnable);
    LOAD_GL(glDisable); LOAD_GL(glViewport); LOAD_GL(glGetError); LOAD_GL(glGetIntegerv);
    LOAD_GL(glGenFramebuffers); LOAD_GL(glBindFramebuffer); LOAD_GL(glDeleteFramebuffers);
    LOAD_GL(glGenBuffers); LOAD_GL(glBindBuffer); LOAD_GL(glDeleteBuffers); LOAD_GL(glPixelStorei);
    LOAD_GL(glIsEnabled);
    GLuint fbos[2], pack;
    p_glGenFramebuffers(2,fbos); p_glGenBuffers(1,&pack);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER,pack);
    p_glPixelStorei(GL_PACK_ALIGNMENT,8); p_glPixelStorei(GL_PACK_ROW_LENGTH,17);
    p_glPixelStorei(GL_PACK_SKIP_ROWS,2); p_glPixelStorei(GL_PACK_SKIP_PIXELS,3);
    CHECK_GL();
    for (int frame = 0; frame < 600; ++frame) {
        EGLint w,h; p_eglQuerySurface(d,s,EGL_WIDTH,&w); p_eglQuerySurface(d,s,EGL_HEIGHT,&h);
        p_glBindFramebuffer(GL_FRAMEBUFFER,0);
        p_glViewport(0,0,w,h); p_glEnable(GL_SCISSOR_TEST);
        const GLfloat colors[4][3]={{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
        for (int i=0;i<4;++i) {
            p_glScissor((i%2)*w/2,(i/2)*h/2,w/2,h/2);
            p_glClearColor(colors[i][0],colors[i][1],colors[i][2],1);
            p_glClear(GL_COLOR_BUFFER_BIT);
        }
        p_glScissor(0,0,1,1);
        p_glEnable(0x8DB9); // GL_FRAMEBUFFER_SRGB: shim must preserve and bypass it.
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER,fbos[0]);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbos[1]);
        CHECK_GL();
        if (!p_eglSwapBuffers(d,s)) {
            char b[100]; snprintf(b,sizeof(b),"FAIL: swap frame %d EGL 0x%x",frame,p_eglGetError());
            return (*env)->NewStringUTF(env,b);
        }
        CHECK_GL();
        GLint r,dr,pa,le;
        p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&r);
        p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&dr);
        p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pa);
        p_glGetIntegerv(GL_PACK_ROW_LENGTH,&le);
        if (r!=(GLint)fbos[0] || dr!=(GLint)fbos[1] || pa!=(GLint)pack || le!=17 ||
            !p_glIsEnabled(GL_SCISSOR_TEST) || !p_glIsEnabled(0x8DB9)) FAIL("game state changed");
        p_glDisable(0x8DB9);
        usleep(16667);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER,0); p_glDeleteFramebuffers(2,fbos);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER,0); p_glDeleteBuffers(1,&pack);
    p_eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    p_eglDestroySurface(d,s); p_eglDestroyContext(d,c); ANativeWindow_release(window);
    return (*env)->NewStringUTF(env,"PASS: 600 frames; GL state preserved; no GL errors");
}
