// SPDX-License-Identifier: MIT
/* Vulkan presenter for the FCL EGL shim.
 *
 * Two presentation paths:
 *
 *  1. Zero-copy (preferred): the shim renders the Mesa frame into a shared
 *     AHardwareBuffer (the same ring the vendor-EGL path uses).  The AHB is
 *     imported as a VkImage (VK_ANDROID_external_memory_android_hardware_buffer),
 *     sampled by a fullscreen triangle into the swapchain image, and the
 *     producer's native fence is imported as a SYNC_FD semaphore so the GPU
 *     waits in hardware.  The vertical flip happens in the vertex shader.
 *
 *  2. CPU upload (fallback / comparison): glReadPixels pixels are copied into a
 *     host-visible staging buffer and vkCmdCopyBufferToImage'd to the image.
 *     Two frames in flight; the GL readback itself is done by the caller.
 *
 * Both paths use FIFO presentation.  The zero-copy path keeps frame pacing
 * (this is the Android presentation queue, not the render loop).
 */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vulkan_present.h"
#include "vk_shaders.h"

#define LOG(...) __android_log_print(ANDROID_LOG_ERROR, "VulkanShim", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VulkanShim", __VA_ARGS__)
#define TRY(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    LOG("%s: %d", #call, r_); goto fail; } } while (0)

#define MAX_FRAMES 2
#define MAX_AHB_SLOTS 4

struct vk_ahb_slot {
    AHardwareBuffer *ahb;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet ds;
    int layout_ready;   /* image has been transitioned to SHADER_READ_ONLY once */
    int last_frame;     /* frame slot of the last submission reading this AHB, -1 = never */
};

struct vk_present {
    ANativeWindow *window;

    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t count;
    VkImage *images;
    VkImageView *views;
    VkSemaphore acquired;      /* image acquired */
    VkSemaphore *complete;     /* per swapchain image, waits in present */
    int can_ahb;

    /* per frame in flight */
    VkCommandBuffer cmd[MAX_FRAMES];
    VkFence fences[MAX_FRAMES];
    VkBuffer upload[MAX_FRAMES];
    VkDeviceMemory upload_mem[MAX_FRAMES];
    void *mapped[MAX_FRAMES];
    VkSemaphore imported[MAX_FRAMES]; /* producer fence imported as SYNC_FD */
    int frame;

    VkCommandPool pool;

    /* zero-copy path */
    int ahb_ok;
    VkRenderPass rp;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkSampler sampler;
    VkFramebuffer *fbs;
    struct vk_ahb_slot slots[MAX_AHB_SLOTS];
    int slot_count;

    PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAHBProperties;

    int broken;
    int stale;      /* swapchain reported SUBOPTIMAL; rebuild once it really is */
};

/* ------------------------------------------------------------------ */
/* swapchain                                                          */
/* ------------------------------------------------------------------ */

static void destroy_framebuffers(struct vk_present *p)
{
    if (!p->fbs) return;
    for (uint32_t i = 0; i < p->count; ++i)
        if (p->fbs[i]) vkDestroyFramebuffer(p->device, p->fbs[i], NULL);
    free(p->fbs);
    p->fbs = NULL;
}

static void drop_swapchain(struct vk_present *p)
{
    if (!p->device) return;
    vkDeviceWaitIdle(p->device);

    destroy_framebuffers(p);

    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (p->imported[i]) { vkDestroySemaphore(p->device, p->imported[i], NULL); p->imported[i] = VK_NULL_HANDLE; }
        if (p->upload[i]) { vkDestroyBuffer(p->device, p->upload[i], NULL); p->upload[i] = VK_NULL_HANDLE; }
        if (p->upload_mem[i]) { vkFreeMemory(p->device, p->upload_mem[i], NULL); p->upload_mem[i] = VK_NULL_HANDLE; }
        p->mapped[i] = NULL;
    }
    if (p->complete) {
        for (uint32_t i = 0; i < p->count; ++i)
            if (p->complete[i]) vkDestroySemaphore(p->device, p->complete[i], NULL);
        free(p->complete);
        p->complete = NULL;
    }
    if (p->views) {
        for (uint32_t i = 0; i < p->count; ++i)
            if (p->views[i]) vkDestroyImageView(p->device, p->views[i], NULL);
        free(p->views);
        p->views = NULL;
    }
    if (p->swapchain) vkDestroySwapchainKHR(p->device, p->swapchain, NULL);
    free(p->images);
    p->images = NULL;
    p->swapchain = VK_NULL_HANDLE;
}

static int create_per_frame(struct vk_present *p)
{
    VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES };
    TRY(vkAllocateCommandBuffers(p->device, &ca, p->cmd));

    VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (int i = 0; i < MAX_FRAMES; ++i)
        TRY(vkCreateFence(p->device, &fc, NULL, &p->fences[i]));

    return 1;
fail:
    return 0;
}

/* Only the CPU fallback needs 2 x frame-size host visible staging buffers, so
 * allocate them on first use instead of paying ~46 MB on every Vulkan surface. */
static int ensure_staging(struct vk_present *p)
{
    if (p->upload[0]) return 1;
    size_t bytes = (size_t)p->extent.width * p->extent.height * 4;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(p->gpu, &props);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        TRY(vkCreateBuffer(p->device, &bc, NULL, &p->upload[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(p->device, p->upload[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t j = 0; j < props.memoryTypeCount; ++j)
            if ((req.memoryTypeBits & (1u << j)) &&
                (props.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            { type = j; break; }
        if (type == UINT32_MAX) goto fail;
        VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = type };
        TRY(vkAllocateMemory(p->device, &ma, NULL, &p->upload_mem[i]));
        TRY(vkBindBufferMemory(p->device, p->upload[i], p->upload_mem[i], 0));
        TRY(vkMapMemory(p->device, p->upload_mem[i], 0, VK_WHOLE_SIZE, 0, &p->mapped[i]));
    }
    return 1;
fail:
    return 0;
}

static int create_framebuffers(struct vk_present *p)
{
    if (!p->ahb_ok || !p->rp) return 1;
    p->fbs = calloc(p->count, sizeof(*p->fbs));
    if (!p->fbs) return 0;
    for (uint32_t i = 0; i < p->count; ++i) {
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->rp, .attachmentCount = 1, .pAttachments = &p->views[i],
            .width = p->extent.width, .height = p->extent.height, .layers = 1 };
        if (vkCreateFramebuffer(p->device, &fi, NULL, &p->fbs[i]) != VK_SUCCESS) {
            LOG("vkCreateFramebuffer failed");
            return 0;
        }
    }
    return 1;
}

static int make_swapchain(struct vk_present *p)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR *formats = NULL;
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->gpu, p->surface, &caps) != VK_SUCCESS)
        return 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    p->can_ahb = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
                 (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
                 (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    if (p->can_ahb) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) goto fail;

    TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(p->gpu, p->surface, &n, NULL));
    if (!n || !(formats = calloc(n, sizeof(*formats)))) goto fail;
    TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(p->gpu, p->surface, &n, formats));
    p->format = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < n; ++i) {
        if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
            formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            formats[i].format == VK_FORMAT_UNDEFINED) {
            p->format = formats[i].format == VK_FORMAT_UNDEFINED ?
                VK_FORMAT_R8G8B8A8_UNORM : formats[i].format;
            break;
        }
    }
    free(formats); formats = NULL;
    if (p->format == VK_FORMAT_UNDEFINED) goto fail;

    p->extent = caps.currentExtent;
    if (p->extent.width == UINT32_MAX) {
        int w = ANativeWindow_getWidth(p->window), h = ANativeWindow_getHeight(p->window);
        if (w <= 0 || h <= 0) goto fail;
        p->extent.width = (uint32_t)w;
        p->extent.height = (uint32_t)h;
        if (p->extent.width < caps.minImageExtent.width) p->extent.width = caps.minImageExtent.width;
        if (p->extent.height < caps.minImageExtent.height) p->extent.height = caps.minImageExtent.height;
        if (p->extent.width > caps.maxImageExtent.width) p->extent.width = caps.maxImageExtent.width;
        if (p->extent.height > caps.maxImageExtent.height) p->extent.height = caps.maxImageExtent.height;
    }
    if (!p->extent.width || !p->extent.height) goto fail;

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    VkSwapchainCreateInfoKHR sc = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = p->surface, .minImageCount = count, .imageFormat = p->format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = p->extent,
        .imageArrayLayers = 1, .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    TRY(vkCreateSwapchainKHR(p->device, &sc, NULL, &p->swapchain));
    TRY(vkGetSwapchainImagesKHR(p->device, p->swapchain, &p->count, NULL));
    if (!(p->images = calloc(p->count, sizeof(*p->images)))) goto fail;
    TRY(vkGetSwapchainImagesKHR(p->device, p->swapchain, &p->count, p->images));
    if (!(p->views = calloc(p->count, sizeof(*p->views)))) goto fail;
    for (uint32_t i = 0; i < p->count; ++i) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = p->images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = p->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        TRY(vkCreateImageView(p->device, &vi, NULL, &p->views[i]));
    }
    if (!(p->complete = calloc(p->count, sizeof(*p->complete)))) goto fail;
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < p->count; ++i)
        TRY(vkCreateSemaphore(p->device, &sem, NULL, &p->complete[i]));
    if (!create_per_frame(p)) goto fail;
    if (!create_framebuffers(p)) goto fail;
    p->frame = 0;
    return 1;

fail:
    free(formats);
    drop_swapchain(p);
    return 0;
}

int vk_present_size(struct vk_present *p, int *w, int *h)
{
    if (p->broken) return 0;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->gpu, p->surface, &caps) != VK_SUCCESS) return 0;
    if (caps.currentExtent.width != UINT32_MAX &&
        (caps.currentExtent.width != p->extent.width || caps.currentExtent.height != p->extent.height)) {
        drop_swapchain(p);
        if (!make_swapchain(p)) { p->broken = 1; return 0; }
    }
    *w = (int)p->extent.width;
    *h = (int)p->extent.height;
    return 1;
}

int vk_present_prefers_bgra(struct vk_present *p)
{
    return p && p->format == VK_FORMAT_B8G8R8A8_UNORM;
}

/* ------------------------------------------------------------------ */
/* zero-copy AHB path                                                 */
/* ------------------------------------------------------------------ */

int vk_present_ahb_available(struct vk_present *p)
{
    return p && p->ahb_ok && !p->broken;
}

void vk_present_idle(struct vk_present *p)
{
    if (p && p->device) vkDeviceWaitIdle(p->device);
}

static struct vk_ahb_slot *find_slot(struct vk_present *p, AHardwareBuffer *ahb)
{
    for (int i = 0; i < p->slot_count; ++i)
        if (p->slots[i].ahb == ahb) return &p->slots[i];
    return NULL;
}

int vk_present_ahb_slot_wait(struct vk_present *p, AHardwareBuffer *ahb)
{
    struct vk_ahb_slot *slot = find_slot(p, ahb);
    if (!slot || slot->last_frame < 0) return 1;
    VkResult r = vkWaitForFences(p->device, 1, &p->fences[slot->last_frame], VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        LOG("slot wait failed: %d", r);
        return 0;
    }
    return 1;
}

static struct vk_ahb_slot *import_slot(struct vk_present *p, AHardwareBuffer *ahb)
{
    if (p->slot_count >= MAX_AHB_SLOTS) return NULL;
    struct vk_ahb_slot *slot = &p->slots[p->slot_count];

    VkAndroidHardwareBufferPropertiesANDROID props = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    VkResult r = p->GetAHBProperties(p->device, ahb, &props);
    if (r != VK_SUCCESS) {
        LOG("vkGetAndroidHardwareBufferPropertiesANDROID: %d", r);
        return NULL;
    }

    VkExternalMemoryImageCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { p->extent.width, p->extent.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(p->device, &ici, NULL, &slot->image) != VK_SUCCESS) {
        LOG("vkCreateImage(AHB) failed");
        return NULL;
    }
    VkImportAndroidHardwareBufferInfoANDROID imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = ahb };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp, .image = slot->image };
    if (!props.memoryTypeBits) {
        LOG("AHB import: empty memory type bits");
        goto fail;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = (uint32_t)__builtin_ctz(props.memoryTypeBits) };
    if (vkAllocateMemory(p->device, &mai, NULL, &slot->memory) != VK_SUCCESS) {
        LOG("vkAllocateMemory(AHB import) failed");
        goto fail;
    }
    if (vkBindImageMemory(p->device, slot->image, slot->memory, 0) != VK_SUCCESS) {
        LOG("vkBindImageMemory(AHB) failed");
        goto fail;
    }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = slot->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(p->device, &vi, NULL, &slot->view) != VK_SUCCESS) {
        LOG("vkCreateImageView(AHB) failed");
        goto fail;
    }
    VkDescriptorSetAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->dpool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl };
    if (vkAllocateDescriptorSets(p->device, &da, &slot->ds) != VK_SUCCESS) {
        LOG("vkAllocateDescriptorSets failed");
        goto fail;
    }
    VkDescriptorImageInfo dii = { .sampler = p->sampler, .imageView = slot->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = slot->ds, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    vkUpdateDescriptorSets(p->device, 1, &wds, 0, NULL);

    slot->ahb = ahb;
    slot->layout_ready = 0;
    slot->last_frame = -1;
    p->slot_count++;
    LOGI("imported AHB %p -> VkImage (slot %d)", (void *)ahb, p->slot_count - 1);
    return slot;

fail:
    if (slot->view) vkDestroyImageView(p->device, slot->view, NULL);
    if (slot->memory) vkFreeMemory(p->device, slot->memory, NULL);
    if (slot->image) vkDestroyImage(p->device, slot->image, NULL);
    memset(slot, 0, sizeof(*slot));
    return NULL;
}

static void destroy_slots(struct vk_present *p)
{
    for (int i = 0; i < p->slot_count; ++i) {
        struct vk_ahb_slot *s = &p->slots[i];
        if (s->view) vkDestroyImageView(p->device, s->view, NULL);
        if (s->memory) vkFreeMemory(p->device, s->memory, NULL);
        if (s->image) vkDestroyImage(p->device, s->image, NULL);
    }
    p->slot_count = 0;
}

int vk_present_frame_ahb(struct vk_present *p, AHardwareBuffer *ahb, int fence_fd,
                         int width, int height)
{
    if (!p->ahb_ok || p->broken) goto fail_early;
    if ((uint32_t)width != p->extent.width || (uint32_t)height != p->extent.height) {
        LOG("AHB frame size mismatch (%dx%d vs %ux%u)", width, height,
            p->extent.width, p->extent.height);
        goto fail_early;
    }
    struct vk_ahb_slot *slot = find_slot(p, ahb);
    if (!slot) slot = import_slot(p, ahb);
    if (!slot) goto fail_early;

    int f = p->frame;
    if (vkWaitForFences(p->device, 1, &p->fences[f], VK_TRUE, UINT64_MAX) != VK_SUCCESS) goto fail;
    vkResetFences(p->device, 1, &p->fences[f]);
    if (p->imported[f]) {
        vkDestroySemaphore(p->device, p->imported[f], NULL);
        p->imported[f] = VK_NULL_HANDLE;
    }

    VkSemaphore waits[2];
    VkPipelineStageFlags wait_stages[2];
    uint32_t wait_count = 0;
    if (fence_fd >= 0) {
        VkSemaphore sem = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(p->device, &sci, NULL, &sem) == VK_SUCCESS) {
            VkImportSemaphoreFdInfoKHR isf = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                .semaphore = sem, .flags = 0,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fence_fd };
            if (p->ImportSemaphoreFdKHR(p->device, &isf) == VK_SUCCESS) {
                p->imported[f] = sem;
                fence_fd = -1; /* fd consumed by the semaphore */
                waits[wait_count] = sem;
                wait_stages[wait_count] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                wait_count++;
            } else {
                LOG("vkImportSemaphoreFdKHR failed; dropping fence");
                vkDestroySemaphore(p->device, sem, NULL);
                close(fence_fd);
            }
        } else {
            close(fence_fd);
        }
    }

    uint32_t index;
    VkResult acquired = vkAcquireNextImageKHR(p->device, p->swapchain, UINT64_MAX,
                                              p->acquired, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        drop_swapchain(p);
        if (!make_swapchain(p)) goto fail;
        return 1; /* stale frame dropped */
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) goto fail;
    waits[wait_count] = p->acquired;
    wait_stages[wait_count] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    wait_count++;

    TRY(vkResetCommandBuffer(p->cmd[f], 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    TRY(vkBeginCommandBuffer(p->cmd[f], &bi));

    VkImageMemoryBarrier src_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = slot->layout_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(p->cmd[f], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &src_barrier);
    slot->layout_ready = 1;

    VkViewport viewport = { 0, 0, (float)p->extent.width, (float)p->extent.height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, p->extent };
    vkCmdSetViewport(p->cmd[f], 0, 1, &viewport);
    vkCmdSetScissor(p->cmd[f], 0, 1, &scissor);

    VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->rp, .framebuffer = p->fbs[index], .renderArea = scissor };
    vkCmdBeginRenderPass(p->cmd[f], &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(p->cmd[f], VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    vkCmdBindDescriptorSets(p->cmd[f], VK_PIPELINE_BIND_POINT_GRAPHICS, p->pl, 0, 1, &slot->ds, 0, NULL);
    int swap = (p->format == VK_FORMAT_B8G8R8A8_UNORM) ? 1 : 0;
    vkCmdPushConstants(p->cmd[f], p->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(swap), &swap);
    vkCmdDraw(p->cmd[f], 3, 1, 0, 0);
    vkCmdEndRenderPass(p->cmd[f]);

    /* The render pass's finalLayout already moved the image to PRESENT_SRC_KHR. */
    TRY(vkEndCommandBuffer(p->cmd[f]));

    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = wait_count, .pWaitSemaphores = waits, .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1, .pCommandBuffers = &p->cmd[f],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &p->complete[index] };
    TRY(vkQueueSubmit(p->queue, 1, &submit, p->fences[f]));

    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &p->complete[index],
        .swapchainCount = 1, .pSwapchains = &p->swapchain, .pImageIndices = &index };
    VkResult result = vkQueuePresentKHR(p->queue, &pi);

    slot->last_frame = f;
    p->frame ^= 1;

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        drop_swapchain(p);
        if (!make_swapchain(p)) goto fail;
    } else if (result == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        /* Not an error: rebuild once when it persists, never per frame. */
        if (p->stale) {
            p->stale = 0;
            drop_swapchain(p);
            if (!make_swapchain(p)) goto fail;
        } else {
            p->stale = 1;
        }
    } else if (result != VK_SUCCESS) {
        goto fail;
    }
    return 1;

fail_early:
    if (fence_fd >= 0) close(fence_fd);
    return 0;
fail:
    if (fence_fd >= 0) close(fence_fd);
    p->broken = 1;
    LOG("AHB presentation failed; restart with FCL_SHIM_RENDERER=egl");
    return 0;
}

/* ------------------------------------------------------------------ */
/* zero-copy pipeline setup                                           */
/* ------------------------------------------------------------------ */

static int ahb_pipeline_init(struct vk_present *p)
{
    VkAttachmentDescription att = { .format = p->format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT } };
    VkRenderPassCreateInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 2, .pDependencies = dep };
    TRY(vkCreateRenderPass(p->device, &rp, NULL, &p->rp));

    VkDescriptorSetLayoutBinding bind = { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo dsl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    TRY(vkCreateDescriptorSetLayout(p->device, &dsl, NULL, &p->dsl));

    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(int) };
    VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    TRY(vkCreatePipelineLayout(p->device, &pl, NULL, &p->pl));

    VkShaderModuleCreateInfo vs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vk_quad_vert_spv_size, .pCode = vk_quad_vert_spv };
    VkShaderModuleCreateInfo fs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vk_quad_frag_spv_size, .pCode = vk_quad_frag_spv };
    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    TRY(vkCreateShaderModule(p->device, &vs, NULL, &vsm));
    TRY(vkCreateShaderModule(p->device, &fs, NULL, &fsm));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &ds, .layout = p->pl, .renderPass = p->rp };
    VkResult pipe_r = vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &gp, NULL, &p->pipe);
    vkDestroyShaderModule(p->device, vsm, NULL);
    vkDestroyShaderModule(p->device, fsm, NULL);
    if (pipe_r != VK_SUCCESS) { LOG("vkCreateGraphicsPipelines: %d", pipe_r); goto fail; }

    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f };
    TRY(vkCreateSampler(p->device, &si, NULL, &p->sampler));

    VkDescriptorPoolSize psize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_AHB_SLOTS };
    VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_AHB_SLOTS, .poolSizeCount = 1, .pPoolSizes = &psize };
    TRY(vkCreateDescriptorPool(p->device, &dp, NULL, &p->dpool));

    p->ahb_ok = 1;
    return 1;
fail:
    return 0;
}

/* ------------------------------------------------------------------ */
/* CPU upload path                                                    */
/* ------------------------------------------------------------------ */

int vk_present_frame(struct vk_present *p, const uint8_t *rgba, int w, int h)
{
    if (p->broken || w != (int)p->extent.width || h != (int)p->extent.height) return 0;
    if (!ensure_staging(p)) goto fail;
    size_t bytes = (size_t)w * h * 4;
    int f = p->frame;

    /* Wait until this frame's staging buffer is free before writing it. */
    if (vkWaitForFences(p->device, 1, &p->fences[f], VK_TRUE, UINT64_MAX) != VK_SUCCESS) goto fail;
    vkResetFences(p->device, 1, &p->fences[f]);

    /* The caller reads the frame back in the swapchain's channel order (see
     * vk_present_prefers_bgra()), so this is a plain staging copy. */
    memcpy(p->mapped[f], rgba, bytes);

    uint32_t index;
    VkResult acquired = vkAcquireNextImageKHR(p->device, p->swapchain, UINT64_MAX,
                                              p->acquired, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        drop_swapchain(p);
        if (!make_swapchain(p)) goto fail;
        return 1;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) goto fail;

    TRY(vkResetCommandBuffer(p->cmd[f], 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    TRY(vkBeginCommandBuffer(p->cmd[f], &bi));
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = p->images[index], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(p->cmd[f], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyBufferToImage(p->cmd[f], p->upload[f], p->images[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(p->cmd[f], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    TRY(vkEndCommandBuffer(p->cmd[f]));

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &p->acquired, .pWaitDstStageMask = &wait,
        .commandBufferCount = 1, .pCommandBuffers = &p->cmd[f],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &p->complete[index] };
    TRY(vkQueueSubmit(p->queue, 1, &submit, p->fences[f]));

    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &p->complete[index],
        .swapchainCount = 1, .pSwapchains = &p->swapchain, .pImageIndices = &index };
    VkResult result = vkQueuePresentKHR(p->queue, &pi);

    p->frame ^= 1;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        drop_swapchain(p);
        if (!make_swapchain(p)) goto fail;
    } else if (result == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        if (p->stale) {
            p->stale = 0;
            drop_swapchain(p);
            if (!make_swapchain(p)) goto fail;
        } else {
            p->stale = 1;
        }
    } else if (result != VK_SUCCESS) {
        goto fail;
    }
    return 1;
fail:
    p->broken = 1;
    LOG("CPU presentation failed; restart with FCL_SHIM_RENDERER=egl");
    return 0;
}

/* ------------------------------------------------------------------ */
/* setup / teardown                                                   */
/* ------------------------------------------------------------------ */

struct vk_present *vk_present_create(ANativeWindow *window)
{
    struct vk_present *p = calloc(1, sizeof(*p));
    VkPhysicalDevice *gpus = NULL;
    if (!p) return NULL;
    p->window = window;
    ANativeWindow_acquire(window);
    for (int i = 0; i < MAX_AHB_SLOTS; ++i) p->slots[i].last_frame = -1;

    const char *ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "FCL Vulkan Shim", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext };
    TRY(vkCreateInstance(&ic, NULL, &p->instance));
    VkAndroidSurfaceCreateInfoKHR ac = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = window };
    TRY(vkCreateAndroidSurfaceKHR(p->instance, &ac, NULL, &p->surface));

    uint32_t n = 0;
    TRY(vkEnumeratePhysicalDevices(p->instance, &n, NULL));
    if (!n || !(gpus = calloc(n, sizeof(*gpus)))) goto fail;
    TRY(vkEnumeratePhysicalDevices(p->instance, &n, gpus));
    for (uint32_t g = 0; g < n && !p->gpu; ++g) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[g], &qn, NULL);
        VkQueueFamilyProperties *qs = calloc(qn, sizeof(*qs));
        if (!qs) goto fail;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[g], &qn, qs);
        for (uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(gpus[g], q, p->surface, &present);
            if (r == VK_SUCCESS && present && (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                p->gpu = gpus[g];
                p->family = q;
                break;
            }
        }
        free(qs);
    }
    free(gpus);
    gpus = NULL;
    if (!p->gpu) goto fail;

    float priority = 1;
    VkDeviceQueueCreateInfo qc = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = p->family, .queueCount = 1, .pQueuePriorities = &priority };
    /* Enable only what this device actually exposes. */
    const char *de[4];
    uint32_t de_count = 0;
    de[de_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    uint32_t den = 0;
    vkEnumerateDeviceExtensionProperties(p->gpu, NULL, &den, NULL);
    VkExtensionProperties *deps = calloc(den ? den : 1, sizeof(*deps));
    int has_sem_fd = 0, has_ahb = 0;
    if (deps && vkEnumerateDeviceExtensionProperties(p->gpu, NULL, &den, deps) == VK_SUCCESS) {
        for (uint32_t i = 0; i < den; ++i) {
            if (!strcmp(deps[i].extensionName, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) has_sem_fd = 1;
            if (!strcmp(deps[i].extensionName,
                        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) has_ahb = 1;
        }
    }
    free(deps);
    if (has_sem_fd) de[de_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    if (has_ahb) de[de_count++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
    VkDeviceCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc,
        .enabledExtensionCount = de_count, .ppEnabledExtensionNames = de };
    TRY(vkCreateDevice(p->gpu, &dc, NULL, &p->device));
    vkGetDeviceQueue(p->device, p->family, 0, &p->queue);

    p->ImportSemaphoreFdKHR =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(p->device, "vkImportSemaphoreFdKHR");
    p->GetAHBProperties = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(p->device, "vkGetAndroidHardwareBufferPropertiesANDROID");

    VkCommandPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = p->family, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    TRY(vkCreateCommandPool(p->device, &pc, NULL, &p->pool));
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    TRY(vkCreateSemaphore(p->device, &sem, NULL, &p->acquired));
    if (!make_swapchain(p)) goto fail;

    if (p->can_ahb && p->ImportSemaphoreFdKHR && p->GetAHBProperties) {
        if (!ahb_pipeline_init(p) || !create_framebuffers(p)) {
            LOG("zero-copy pipeline unavailable; using CPU upload");
            destroy_framebuffers(p);
            p->ahb_ok = 0;
        } else {
            LOGI("ready: %ux%u format=%d (zero-copy AHB + CPU upload fallback)",
                 p->extent.width, p->extent.height, p->format);
            return p;
        }
    }
    LOGI("ready: %ux%u format=%d (CPU upload)", p->extent.width, p->extent.height, p->format);
    return p;

fail:
    free(gpus);
    vk_present_destroy(p);
    return NULL;
}

void vk_present_destroy(struct vk_present *p)
{
    if (!p) return;
    if (p->device) {
        vkDeviceWaitIdle(p->device);
        destroy_framebuffers(p);
        destroy_slots(p);
        for (int i = 0; i < MAX_FRAMES; ++i) {
            if (p->imported[i]) vkDestroySemaphore(p->device, p->imported[i], NULL);
            if (p->fences[i]) vkDestroyFence(p->device, p->fences[i], NULL);
            if (p->upload[i]) vkDestroyBuffer(p->device, p->upload[i], NULL);
            if (p->upload_mem[i]) vkFreeMemory(p->device, p->upload_mem[i], NULL);
        }
        if (p->sampler) vkDestroySampler(p->device, p->sampler, NULL);
        if (p->dpool) vkDestroyDescriptorPool(p->device, p->dpool, NULL);
        if (p->pipe) vkDestroyPipeline(p->device, p->pipe, NULL);
        if (p->pl) vkDestroyPipelineLayout(p->device, p->pl, NULL);
        if (p->dsl) vkDestroyDescriptorSetLayout(p->device, p->dsl, NULL);
        if (p->rp) vkDestroyRenderPass(p->device, p->rp, NULL);
        if (p->pool) vkDestroyCommandPool(p->device, p->pool, NULL);
    }
    drop_swapchain(p);
    if (p->device) {
        if (p->acquired) vkDestroySemaphore(p->device, p->acquired, NULL);
        vkDestroyDevice(p->device, NULL);
    }
    if (p->surface) vkDestroySurfaceKHR(p->instance, p->surface, NULL);
    if (p->instance) vkDestroyInstance(p->instance, NULL);
    ANativeWindow_release(p->window);
    free(p);
}
