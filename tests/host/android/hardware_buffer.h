#ifndef ANDROID_HARDWARE_BUFFER_H
#define ANDROID_HARDWARE_BUFFER_H
#include <stdint.h>
typedef struct AHardwareBuffer AHardwareBuffer;
enum {
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1,
};
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ull << 8)
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT (1ull << 9)
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN (1ull << 1)
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN (1ull << 4)
typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **outBuffer);
void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *outDesc);
int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                         const void *rect, void **outVirtualAddress);
int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
#endif
