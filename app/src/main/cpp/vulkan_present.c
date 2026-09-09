// SPDX-License-Identifier: MIT
/* Conservative Vulkan presenter: CPU-visible upload, FIFO, one frame in flight.
 * No cross-driver external-memory ownership or implicit synchronization. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include "vulkan_present.h"
#define LOG(...) __android_log_print(ANDROID_LOG_ERROR, "VulkanShim", __VA_ARGS__)
#define TRY(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    LOG("%s: %d", #call, r_); goto fail; } } while (0)
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
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired;
    VkSemaphore *complete;
    VkBuffer upload;
    VkDeviceMemory memory;
    void *mapped;
    int broken;
};
static void drop_swapchain(struct vk_present *p)
{
    if (!p->device) return;
    vkDeviceWaitIdle(p->device);
    if (p->mapped) vkUnmapMemory(p->device, p->memory);
    if (p->upload) vkDestroyBuffer(p->device, p->upload, NULL);
    if (p->memory) vkFreeMemory(p->device, p->memory, NULL);
    if (p->complete) {
        for (uint32_t i = 0; i < p->count; ++i)
            if (p->complete[i]) vkDestroySemaphore(p->device, p->complete[i], NULL);
        free(p->complete); p->complete = NULL;
    }
    if (p->swapchain) vkDestroySwapchainKHR(p->device, p->swapchain, NULL);
    free(p->images);
    p->images = NULL; p->mapped = NULL; p->upload = VK_NULL_HANDLE;
    p->memory = VK_NULL_HANDLE; p->swapchain = VK_NULL_HANDLE;
}
static int make_swapchain(struct vk_present *p)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR *formats = NULL;
    uint32_t n = 0;
    TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->gpu, p->surface, &caps));
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
        p->extent.width = (uint32_t)w; p->extent.height = (uint32_t)h;
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
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    TRY(vkCreateSwapchainKHR(p->device, &sc, NULL, &p->swapchain));
    TRY(vkGetSwapchainImagesKHR(p->device, p->swapchain, &p->count, NULL));
    if (!(p->images = calloc(p->count, sizeof(*p->images)))) goto fail;
    TRY(vkGetSwapchainImagesKHR(p->device, p->swapchain, &p->count, p->images));
    if (!(p->complete = calloc(p->count, sizeof(*p->complete)))) goto fail;
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < p->count; ++i)
        TRY(vkCreateSemaphore(p->device, &sem, NULL, &p->complete[i]));
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)p->extent.width * p->extent.height * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    TRY(vkCreateBuffer(p->device, &bc, NULL, &p->upload));
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties props;
    vkGetBufferMemoryRequirements(p->device, p->upload, &req);
    vkGetPhysicalDeviceMemoryProperties(p->gpu, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
    if (type == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type };
    TRY(vkAllocateMemory(p->device, &ma, NULL, &p->memory));
    TRY(vkBindBufferMemory(p->device, p->upload, p->memory, 0));
    TRY(vkMapMemory(p->device, p->memory, 0, VK_WHOLE_SIZE, 0, &p->mapped));
    return 1;
fail:
    free(formats); drop_swapchain(p); return 0;
}
struct vk_present *vk_present_create(ANativeWindow *window)
{
    struct vk_present *p = calloc(1, sizeof(*p));
    VkPhysicalDevice *gpus = NULL;
    if (!p) return NULL;
    p->window = window; ANativeWindow_acquire(window);
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
                p->gpu = gpus[g]; p->family = q; break;
            }
        }
        free(qs);
    }
    free(gpus); gpus = NULL;
    if (!p->gpu) goto fail;
    float priority = 1;
    VkDeviceQueueCreateInfo qc = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = p->family, .queueCount = 1, .pQueuePriorities = &priority };
    const char *de = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &de };
    TRY(vkCreateDevice(p->gpu, &dc, NULL, &p->device));
    vkGetDeviceQueue(p->device, p->family, 0, &p->queue);
    VkCommandPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = p->family, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    TRY(vkCreateCommandPool(p->device, &pc, NULL, &p->pool));
    VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    TRY(vkAllocateCommandBuffers(p->device, &ca, &p->cmd));
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    TRY(vkCreateSemaphore(p->device, &sem, NULL, &p->acquired));
    if (!make_swapchain(p)) goto fail;
    __android_log_print(ANDROID_LOG_INFO, "VulkanShim", "ready: %ux%u format=%d (CPU upload)",
                        p->extent.width, p->extent.height, p->format);
    return p;
fail:
    free(gpus); vk_present_destroy(p); return NULL;
}
void vk_present_destroy(struct vk_present *p)
{
    if (!p) return;
    drop_swapchain(p);
    if (p->device) {
        if (p->acquired) vkDestroySemaphore(p->device, p->acquired, NULL);
        if (p->pool) vkDestroyCommandPool(p->device, p->pool, NULL);
        vkDestroyDevice(p->device, NULL);
    }
    if (p->surface) vkDestroySurfaceKHR(p->instance, p->surface, NULL);
    if (p->instance) vkDestroyInstance(p->instance, NULL);
    ANativeWindow_release(p->window); free(p);
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
    *w = (int)p->extent.width; *h = (int)p->extent.height; return 1;
}
int vk_present_frame(struct vk_present *p, const uint8_t *rgba, int w, int h)
{
    if (p->broken || w != (int)p->extent.width || h != (int)p->extent.height) return 0;
    size_t bytes = (size_t)w * h * 4;
    memcpy(p->mapped, rgba, bytes);
    if (p->format == VK_FORMAT_B8G8R8A8_UNORM) {
        uint8_t *dst = p->mapped;
        for (size_t i = 0; i < bytes; i += 4) {
            uint8_t r = dst[i]; dst[i] = dst[i + 2]; dst[i + 2] = r;
        }
    }
    uint32_t index;
    VkResult acquired = vkAcquireNextImageKHR(p->device, p->swapchain, UINT64_MAX,
                                              p->acquired, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        drop_swapchain(p);
        if (!make_swapchain(p)) goto fail;
        return 1; /* Drop this stale frame; next swap uses the new extent. */
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) goto fail;
    TRY(vkResetCommandBuffer(p->cmd, 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    TRY(vkBeginCommandBuffer(p->cmd, &bi));
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = p->images[index], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} };
    vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copy = { .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {(uint32_t)w, (uint32_t)h, 1} };
    vkCmdCopyBufferToImage(p->cmd, p->upload, p->images[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    TRY(vkEndCommandBuffer(p->cmd));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &p->acquired, .pWaitDstStageMask = &wait,
        .commandBufferCount = 1, .pCommandBuffers = &p->cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &p->complete[index] };
    TRY(vkQueueSubmit(p->queue, 1, &submit, VK_NULL_HANDLE));
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &p->complete[index],
        .swapchainCount = 1, .pSwapchains = &p->swapchain, .pImageIndices = &index };
    VkResult result = vkQueuePresentKHR(p->queue, &pi);
    TRY(vkQueueWaitIdle(p->queue)); /* Upload memory and acquire semaphore are now reusable.
                                 * Present semaphore is reused only on reacquiring its image. */
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        drop_swapchain(p);
        if (!make_swapchain(p)) goto fail;
    } else if (result != VK_SUCCESS) goto fail;
    return 1;
fail:
    p->broken = 1; LOG("presentation failed; recreate surface or restart with FCL_SHIM_RENDERER=egl");
    return 0;
}
