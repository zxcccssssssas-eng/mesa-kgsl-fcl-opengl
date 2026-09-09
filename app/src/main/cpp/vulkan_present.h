// SPDX-License-Identifier: MIT
#pragma once
#include <android/native_window.h>
#include <stdint.h>
struct vk_present;
/* RGBA rows, top to bottom. Synchronous: pixels may be reused on return. */
struct vk_present *vk_present_create(ANativeWindow *window);
void vk_present_destroy(struct vk_present *p);
int vk_present_size(struct vk_present *p, int *width, int *height);
int vk_present_frame(struct vk_present *p, const uint8_t *rgba, int width, int height);
