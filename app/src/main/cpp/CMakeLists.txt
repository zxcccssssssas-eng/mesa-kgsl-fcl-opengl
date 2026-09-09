cmake_minimum_required(VERSION 3.22.1)
project(fcl_freedreno_kgsl C)

add_library(freedreno_kgsl_init SHARED kgsl_init.c)
target_compile_options(freedreno_kgsl_init PRIVATE -fvisibility=hidden -Wall -Wextra)

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
endif()
