// SPDX-License-Identifier: MIT
#pragma once
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <stdint.h>

struct vk_present;

struct vk_present *vk_present_create(ANativeWindow *window);
void vk_present_destroy(struct vk_present *p);

int vk_present_size(struct vk_present *p, int *width, int *height);

/* Zero-copy path: sample a shared AHardwareBuffer (R8G8B8A8_UNORM) with a
 * fullscreen triangle and present the swapchain image.  fence_fd is the
 * producer's native fence (EGL_SYNC_NATIVE_FENCE_ANDROID dup); it is consumed
 * (imported as a SYNC_FD semaphore) when >= 0.  Ownership of fence_fd is
 * transferred either way: it is consumed or closed before returning. */
int vk_present_ahb_available(struct vk_present *p);
int vk_present_frame_ahb(struct vk_present *p, AHardwareBuffer *ahb, int fence_fd,
                         int width, int height);
/* Blocks until the GPU is done reading this AHB (safe to overwrite it). */
int vk_present_ahb_slot_wait(struct vk_present *p, AHardwareBuffer *ahb);

/* CPU upload fallback: rgba rows, top to bottom. */
int vk_present_frame(struct vk_present *p, const uint8_t *rgba, int width, int height);
/* True when the swapchain wants B8G8R8A8, so the caller can read GL_BGRA
 * directly instead of paying for a CPU channel swap. */
int vk_present_prefers_bgra(struct vk_present *p);

/* Waits for all submitted frames (surface destroy / resize / backend switch). */
void vk_present_idle(struct vk_present *p);
