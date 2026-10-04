#include "android/log.h"
#include "android/hardware_buffer.h"
#include "android/native_window.h"
#include <stdio.h>
#include <stdarg.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    (void)prio;
    fprintf(stderr, "%s: ", tag ? tag : "log");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 0;
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **outBuffer)
{
    (void)desc;
    if (outBuffer)
        *outBuffer = NULL;
    return -1;
}

void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *outDesc)
{
    (void)buffer;
    if (outDesc)
        outDesc->stride = 0;
}

int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                         const void *rect, void **outVirtualAddress)
{
    (void)buffer;
    (void)usage;
    (void)fence;
    (void)rect;
    if (outVirtualAddress)
        *outVirtualAddress = NULL;
    return -1;
}

int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence)
{
    (void)buffer;
    if (fence)
        *fence = -1;
    return -1;
}

void AHardwareBuffer_release(AHardwareBuffer *buffer)
{
    (void)buffer;
}

int ANativeWindow_getWidth(ANativeWindow *window)
{
    (void)window;
    return 0;
}

int ANativeWindow_getHeight(ANativeWindow *window)
{
    (void)window;
    return 0;
}

void ANativeWindow_acquire(ANativeWindow *window)
{
    (void)window;
}

void ANativeWindow_release(ANativeWindow *window)
{
    (void)window;
}
