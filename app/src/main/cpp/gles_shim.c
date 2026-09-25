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
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#define SHIM_LOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define SHIM_ERR(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void *egl_handle;
static __eglMustCastToProperFunctionPointerType (*egl_get_proc_address)(const char *);

/* Flywheel's indirect backend uses gl_DrawID with a _flw_baseDraw uniform.
 * Submit the commands one at a time, matching Flywheel's Intel fallback.
 * The commands stay GPU-resident, avoiding a per-draw GPU readback. */
typedef void (*draw_elements_indirect_fn)(GLenum, GLenum, const void *);
typedef void (*multi_draw_elements_indirect_fn)(GLenum, GLenum, const void *, GLsizei, GLsizei);
typedef void (*get_uniform_uiv_fn)(GLuint, GLint, GLuint *);
typedef void (*uniform_1ui_fn)(GLint, GLuint);
typedef GLint (*get_uniform_location_fn)(GLuint, const GLchar *);
typedef GLint (*get_attrib_location_fn)(GLuint, const GLchar *);
typedef void (*get_integer_v_fn)(GLenum, GLint *);
typedef void (*get_integer_i_v_fn)(GLenum, GLuint, GLint *);
typedef void (*get_integer_64_i_v_fn)(GLenum, GLuint, GLint64 *);
typedef void (*get_vertex_attrib_iv_fn)(GLuint, GLenum, GLint *);
typedef void (*get_vertex_attrib_pointer_v_fn)(GLuint, GLenum, void **);
typedef void (*bind_buffer_fn)(GLenum, GLuint);
typedef void (*vertex_attrib_i_pointer_fn)(GLuint, GLint, GLenum, GLsizei, const void *);
typedef void (*vertex_attrib_pointer_fn)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                         const void *);
typedef void (*vertex_attrib_divisor_fn)(GLuint, GLuint);
typedef void (*vertex_attrib_array_fn)(GLuint);
typedef void (*compile_shader_fn)(GLuint);
typedef void (*get_shader_iv_fn)(GLuint, GLenum, GLint *);
typedef void (*get_shader_source_fn)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*shader_source_fn)(GLuint, GLsizei, const GLchar *const *, const GLint *);

#define FLYWHEEL_INSTANCE_ATTRIBUTE 15

/* Reading Flywheel's instance-index SSBO in the vertex shader loses some
 * moving Create geometry on A840. Fetch the same GPU buffer through an
 * instanced integer vertex attribute instead. The indirect command's
 * baseInstance offsets instanced attributes as well as gl_BaseInstance. */
static void shim_compile_shader(GLuint shader)
{
    compile_shader_fn compile = (compile_shader_fn)egl_get_proc_address("glCompileShader");
    get_shader_iv_fn get_iv = (get_shader_iv_fn)egl_get_proc_address("glGetShaderiv");
    get_shader_source_fn get_source =
        (get_shader_source_fn)egl_get_proc_address("glGetShaderSource");
    shader_source_fn set_source = (shader_source_fn)egl_get_proc_address("glShaderSource");
    get_integer_v_fn get_integer = (get_integer_v_fn)egl_get_proc_address("glGetIntegerv");
    if (!compile) return;
    if (!get_iv || !get_source || !set_source || !get_integer) {
        compile(shader);
        return;
    }

    GLint max_attributes = 0;
    get_integer(GL_MAX_VERTEX_ATTRIBS, &max_attributes);
    if (max_attributes <= FLYWHEEL_INSTANCE_ATTRIBUTE) {
        compile(shader);
        return;
    }

    GLint length = 0;
    get_iv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    if (length <= 0 || length > 4 * 1024 * 1024) { compile(shader); return; }
    char *source = malloc((size_t)length + 1);
    if (!source) { compile(shader); return; }
    GLsizei actual = 0;
    get_source(shader, length + 1, &actual, source);
    source[actual] = '\0';

    const char *marker = "uniform uint _flw_baseDraw;";
    const char *old_index =
        "uint instanceIndex = _flw_instanceIndices[flw_baseInstance + gl_InstanceID];";
    char *at = strstr(source, marker);
    char *index = strstr(source, old_index);
    if (at && index && !strstr(source, "layout(location = 15)") &&
        strstr(source, "MeshDrawCommand draw = _flw_drawCommands[drawIndex]")) {
        const char *extra = "\nlayout(location = 15) in uint _flw_instanceIndexAttrib;";
        const char *new_index = "uint instanceIndex = _flw_instanceIndexAttrib;";
        size_t offset = (size_t)(at - source) + strlen(marker);
        size_t extra_len = strlen(extra);
        size_t total = (size_t)actual + extra_len;
        char *patched = malloc(total + 1);
        if (patched) {
            memcpy(patched, source, offset);
            memcpy(patched + offset, extra, extra_len);
            memcpy(patched + offset + extra_len, source + offset,
                   (size_t)actual - offset + 1);
            char *match = strstr(patched, old_index);
            memset(match, ' ', strlen(old_index));
            memcpy(match, new_index, strlen(new_index));
            const GLchar *new_source = patched;
            GLint new_length = (GLint)total;
            set_source(shader, 1, &new_source, &new_length);
            SHIM_LOG("Flywheel instance-index vertex attribute active");
            free(patched);
        }
    }
    free(source);
    compile(shader);
}

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
    get_attrib_location_fn get_attrib_location =
        (get_attrib_location_fn)egl_get_proc_address("glGetAttribLocation");
    get_uniform_uiv_fn get_uniform =
        (get_uniform_uiv_fn)egl_get_proc_address("glGetUniformuiv");
    uniform_1ui_fn set_uniform =
        (uniform_1ui_fn)egl_get_proc_address("glUniform1ui");

    if (!multi_draw || !draw || !get_integer || !get_location ||
        !get_uniform || !set_uniform || drawcount <= 0 || stride < 0) {
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
    get_integer_i_v_fn get_integer_i =
        (get_integer_i_v_fn)egl_get_proc_address("glGetIntegeri_v");
    get_integer_64_i_v_fn get_integer_64_i =
        (get_integer_64_i_v_fn)egl_get_proc_address("glGetInteger64i_v");
    get_vertex_attrib_iv_fn get_attrib =
        (get_vertex_attrib_iv_fn)egl_get_proc_address("glGetVertexAttribiv");
    get_vertex_attrib_pointer_v_fn get_attrib_pointer =
        (get_vertex_attrib_pointer_v_fn)egl_get_proc_address("glGetVertexAttribPointerv");
    bind_buffer_fn bind_buffer = (bind_buffer_fn)egl_get_proc_address("glBindBuffer");
    vertex_attrib_i_pointer_fn attrib_i_pointer =
        (vertex_attrib_i_pointer_fn)egl_get_proc_address("glVertexAttribIPointer");
    vertex_attrib_pointer_fn attrib_pointer =
        (vertex_attrib_pointer_fn)egl_get_proc_address("glVertexAttribPointer");
    vertex_attrib_divisor_fn attrib_divisor =
        (vertex_attrib_divisor_fn)egl_get_proc_address("glVertexAttribDivisor");
    vertex_attrib_array_fn enable_attrib =
        (vertex_attrib_array_fn)egl_get_proc_address("glEnableVertexAttribArray");
    vertex_attrib_array_fn disable_attrib =
        (vertex_attrib_array_fn)egl_get_proc_address("glDisableVertexAttribArray");
    GLint max_attributes = 0, index_buffer = 0, saved_array_buffer = 0;
    GLint64 index_start = 0, index_range = 0;
    GLint old_enabled = 0, old_divisor = 0, old_buffer = 0;
    GLint old_size = 0, old_type = 0, old_integer = 0, old_normalized = 0;
    GLint old_stride = 0;
    void *old_pointer = NULL;
    int use_attribute = 0;
    if (get_attrib_location && get_integer_i && get_integer_64_i && get_attrib && get_attrib_pointer &&
        bind_buffer && attrib_i_pointer && attrib_pointer && attrib_divisor &&
        enable_attrib && disable_attrib &&
        get_attrib_location((GLuint)program, "_flw_instanceIndexAttrib") ==
            FLYWHEEL_INSTANCE_ATTRIBUTE) {
        get_integer(GL_MAX_VERTEX_ATTRIBS, &max_attributes);
        get_integer_i(GL_SHADER_STORAGE_BUFFER_BINDING, 2, &index_buffer);
        get_integer_64_i(GL_SHADER_STORAGE_BUFFER_START, 2, &index_start);
        get_integer_64_i(GL_SHADER_STORAGE_BUFFER_SIZE, 2, &index_range);
        if (max_attributes > FLYWHEEL_INSTANCE_ATTRIBUTE && index_buffer > 0 &&
            index_start >= 0 && index_range >= (GLint64)sizeof(GLuint)) {
            get_integer(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &old_enabled);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &old_divisor);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &old_buffer);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_SIZE, &old_size);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_TYPE, &old_type);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &old_integer);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &old_normalized);
            get_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &old_stride);
            get_attrib_pointer(FLYWHEEL_INSTANCE_ATTRIBUTE, GL_VERTEX_ATTRIB_ARRAY_POINTER, &old_pointer);
            bind_buffer(GL_ARRAY_BUFFER, (GLuint)index_buffer);
            attrib_i_pointer(FLYWHEEL_INSTANCE_ATTRIBUTE, 1, GL_UNSIGNED_INT,
                             sizeof(GLuint), (const void *)(uintptr_t)index_start);
            attrib_divisor(FLYWHEEL_INSTANCE_ATTRIBUTE, 1);
            enable_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE);
            use_attribute = 1;
        }
    }
    static int attribute_logged;
    if (!attribute_logged) {
        SHIM_LOG("Flywheel attribute use=%d max=%d indexBuffer=%d start=%lld range=%lld oldEnabled=%d",
                 use_attribute, max_attributes, index_buffer,
                 (long long)index_start, (long long)index_range, old_enabled);
        attribute_logged = 1;
    }
    size_t command_stride = stride ? (size_t)stride : 5 * sizeof(GLuint);
    uintptr_t command = (uintptr_t)indirect;
    for (GLsizei i = 0; i < drawcount; ++i) {
        set_uniform(base_draw_location, base_draw + (GLuint)i);
        draw(mode, type, (const void *)(command + (size_t)i * command_stride));
    }
    set_uniform(base_draw_location, base_draw);
    if (use_attribute) {
        if (!old_enabled) disable_attrib(FLYWHEEL_INSTANCE_ATTRIBUTE);
        if (old_buffer > 0) {
            bind_buffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
            if (old_integer)
                attrib_i_pointer(FLYWHEEL_INSTANCE_ATTRIBUTE, old_size, (GLenum)old_type,
                                 old_stride, old_pointer);
            else
                attrib_pointer(FLYWHEEL_INSTANCE_ATTRIBUTE, old_size, (GLenum)old_type,
                               (GLboolean)old_normalized, old_stride, old_pointer);
        }
        attrib_divisor(FLYWHEEL_INSTANCE_ATTRIBUTE, (GLuint)old_divisor);
        bind_buffer(GL_ARRAY_BUFFER, (GLuint)saved_array_buffer);
    }
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
    if (!egl_get_proc_address)
        return NULL;
    if (procname && strcmp((const char *)procname, "glMultiDrawElementsIndirect") == 0)
        return (__eglMustCastToProperFunctionPointerType)shim_multi_draw_elements_indirect;
    if (procname && strcmp((const char *)procname, "glCompileShader") == 0)
        return (__eglMustCastToProperFunctionPointerType)shim_compile_shader;
    return egl_get_proc_address((const char *)procname);
}

__eglMustCastToProperFunctionPointerType glXGetProcAddressARB(const unsigned char *procname)
{
    return glXGetProcAddress(procname);
}

__eglMustCastToProperFunctionPointerType OSMesaGetProcAddress(const char *procname)
{
    return glXGetProcAddress((const unsigned char *)procname);
}
