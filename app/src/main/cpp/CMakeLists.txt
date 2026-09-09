cmake_minimum_required(VERSION 3.22.1)
project(fcl_freedreno_kgsl C)

add_library(freedreno_kgsl_init SHARED kgsl_init.c)
target_compile_options(freedreno_kgsl_init PRIVATE -fvisibility=hidden -Wall -Wextra)
# FCLProbe: AHardwareBuffer (libnativewindow) + __android_log_print (liblog) +
# dlopen of the vendor EGL (libdl).
target_link_libraries(freedreno_kgsl_init PRIVATE nativewindow log dl)

# Tiny EGL/GLES/gallium placeholders so metadata-only APKs still contain the
# library names FoldCraftLauncher will dlopen. scripts/build-mesa-android.sh
# writes real Mesa DSOs into jniLibs; Gradle prefers CMake outputs on name
# collision, so stubs are only built when those files are absent.
option(FCL_STUB_MESA_EGL "Build no-op Mesa EGL/GLES/gallium DSOs" ON)
if(FCL_STUB_MESA_EGL)
    add_library(EGL_mesa SHARED egl_stub.c)
    target_compile_options(EGL_mesa PRIVATE -fvisibility=default -Wall)

    add_library(GLESv2_mesa SHARED glesv2_stub.c)
    target_compile_options(GLESv2_mesa PRIVATE -fvisibility=default -Wall)

    add_library(gallium_dri SHARED gallium_stub.c)
    target_compile_options(gallium_dri PRIVATE -fvisibility=default -Wall)
else()
    # Real Mesa DSOs are in jniLibs (libEGL_mesa_core.so holds the renamed
    # Mesa EGL). Ship the presentation shim under the name FCL dlopens.
    add_library(EGL_mesa SHARED egl_shim.c)
    target_compile_options(EGL_mesa PRIVATE -fvisibility=default -Wall -Wextra)
    target_link_libraries(EGL_mesa PRIVATE nativewindow log dl)
endif()
