cmake_minimum_required(VERSION 3.22.1)
project(fcl_freedreno_kgsl C)

add_library(OSMBridge SHARED osmbridge.c)
target_link_libraries(OSMBridge PRIVATE ${CMAKE_DL_LIBS} log)
target_compile_options(OSMBridge PRIVATE -fvisibility=hidden -Wall -Wextra)

# Tiny OSMesa placeholder so the APK always contains libOSMesa.so.
# scripts/build-mesa-android.sh overwrites jniLibs with the real Mesa build;
# if that file is present, this stub is still compiled but the prebuilt
# jniLibs copy wins at package time when names collide — Gradle prefers
# cmake outputs. CI copies the real lib into jniLibs AND we only build the
# stub when FCL_STUB_OSMESA=ON.
option(FCL_STUB_OSMESA "Build a no-op libOSMesa.so for metadata-only APKs" ON)
if(FCL_STUB_OSMESA)
    add_library(OSMesa SHARED osmesa_stub.c)
    target_compile_options(OSMesa PRIVATE -fvisibility=default -Wall)
endif()
