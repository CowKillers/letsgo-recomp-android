// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#ifdef LETSGO_ADRENOTOOLS
#include <android/native_window.h>
#endif
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "vk.h"
#include "../output_scale.h"
#include "../renderer.h"

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== dispatch / custom GPU drivers
// (PAUSADO: trabalho do adrenotools/Turnip interrompido em 20/10 ~00:05 a pedido da Emily.
//  Para retomar: definir LETSGO_ADRENOTOOLS no build, reativar vk_dispatch.h em vk.h, fiar
//  create_instance() no custom gipa e popular a tabela.)
#ifdef LETSGO_ADRENOTOOLS
// Every Vulkan entry point goes through g_vkd (vk_dispatch.h). At startup the table is filled from the
// loader in use: the system loader normally, or a custom GPU driver (Turnip) via adrenotools when the
// launcher selected one (LARECOMP_GPU_DRIVER=<dir>/<lib>.so, LARECOMP_NATIVE_LIB_DIR=<nativeLibraryDir>).
static PFN_vkGetInstanceProcAddr g_gipa;  // the loader's vkGetInstanceProcAddr behind g_vkd

#include <dlfcn.h>
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#endif

// Resolve the loader: adrenotools + the custom driver when configured, else the system loader.
static PFN_vkGetInstanceProcAddr load_gipa(void) {
#ifdef __ANDROID__
    const char *drv = getenv("LARECOMP_GPU_DRIVER");
    const char *libdir = getenv("LARECOMP_NATIVE_LIB_DIR");
    if (drv && *drv && libdir && *libdir) {
        // Wonder pattern: verify the driver .so is readable before calling adrenotools.
        if (access(drv, R_OK) != 0) {
            LOG("[vk] custom GPU driver %s not readable (%s), using system driver\n", drv, strerror(errno));
        } else {
        // drv = "<dir>/<lib>.so": adrenotools wants the dir and the bare name separately.
        const char *slash = strrchr(drv, '/');
        if (slash && slash[1]) {
            char dir[512];
            size_t dl = (size_t)(slash - drv);
            // BOTW pattern: dir with trailing slash (adrenotools expects "dir/" not "dir")
            if (dl + 1 < sizeof dir) {
                memcpy(dir, drv, dl);
                dir[dl] = '/';
                dir[dl + 1] = 0;
                void *h = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                                                     NULL, libdir, dir, slash + 1, NULL, NULL);
                if (h) {
                    void *g = dlsym(h, "vkGetInstanceProcAddr");
                    if (g) {
                        LOG("[vk] custom GPU driver via adrenotools: %s\n", drv);
                        return (PFN_vkGetInstanceProcAddr)g;
                    }
                    LOG("[vk] custom GPU driver %s loaded but vkGetInstanceProcAddr not found: %s\n", drv, dlerror());
                } else {
                    // v0079 Path 1: dlerror() is often NULL from adrenotools (its 5 nullptr
                    // returns don't set it). Log the parameters so the failure is diagnosable.
                    const char *e = dlerror();
                    LOG("[vk] custom GPU driver %s failed to load via adrenotools: %s (dir=%s name=%s hookLibDir=%s)\n",
                        drv, e ? e : "(adrenotools returned NULL without dlerror — likely linkernsbypass or hook install failure)",
                        dir, slash + 1, libdir);
                }
                LOG("[vk] using the system driver\n");
            }
        }
        } // end access check
    }
#endif
    // System loader: the linked libvulkan entry point. (The plain identifier is macro-redirected
    // to g_vkd by vk_dispatch.h, so resolve it explicitly from the loaded libvulkan.)
    void *g = NULL;
    void *libv = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (libv) g = dlsym(libv, "vkGetInstanceProcAddr");
    if (!g) FATAL("vkGetInstanceProcAddr not found in libvulkan.so\n");
    return (PFN_vkGetInstanceProcAddr)g;
}

// Fill one dispatch entry from the loader (fatal when a core entry point is missing).
static void load_one(const char *name, void **slot) {
    *slot = (void *)g_gipa(g_vk.inst, name);
    if (!*slot) FATAL("Vulkan entry point %s not found\n", name);
}

// The vk_* macros from vk_dispatch.h (vkCreateInstance -> g_vkd.vkCreateInstance) must NOT
// expand inside the fill code below, or VKFN(vkCreateInstance) becomes &g_vkd.g_vkd.vkCreateInstance.
// Undefine them only while defining and filling the table; restore them before any call sites.
#undef vkCreateInstance
#undef vkDestroyInstance
#undef vkEnumerateInstanceExtensionProperties
#undef vkEnumerateInstanceLayerProperties
#undef vkGetInstanceProcAddr
#undef vkEnumeratePhysicalDevices
#undef vkGetPhysicalDeviceProperties
#undef vkGetPhysicalDeviceQueueFamilyProperties
#undef vkGetPhysicalDeviceFeatures2
#undef vkEnumerateDeviceExtensionProperties
#undef vkGetPhysicalDeviceFormatProperties
#undef vkGetPhysicalDeviceSurfaceSupportKHR
#undef vkCreateAndroidSurfaceKHR
#undef vkDestroySurfaceKHR
#undef vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#undef vkGetPhysicalDeviceSurfaceFormatsKHR
#undef vkGetPhysicalDeviceSurfacePresentModesKHR
#undef vkCreateDevice
#undef vkGetDeviceProcAddr

#undef vkAcquireNextImageKHR
#undef vkAllocateCommandBuffers
#undef vkAllocateDescriptorSets
#undef vkAllocateMemory
#undef vkBeginCommandBuffer
#undef vkBindBufferMemory
#undef vkBindImageMemory
#undef vkCmdBeginQuery
#undef vkCmdBeginRendering
#undef vkCmdBindDescriptorSets
#undef vkCmdBindIndexBuffer
#undef vkCmdBindPipeline
#undef vkCmdBindVertexBuffers
#undef vkCmdBlitImage
#undef vkCmdClearAttachments
#undef vkCmdClearColorImage
#undef vkCmdClearDepthStencilImage
#undef vkCmdCopyBuffer
#undef vkCmdCopyBufferToImage
#undef vkCmdCopyImage
#undef vkCmdCopyImageToBuffer
#undef vkCmdDispatch
#undef vkCmdDispatchIndirect
#undef vkCmdDraw
#undef vkCmdDrawIndexed
#undef vkCmdDrawIndirect
#undef vkCmdEndQuery
#undef vkCmdEndRendering
#undef vkCmdExecuteCommands
#undef vkCmdPipelineBarrier
#undef vkCmdPipelineBarrier2
#undef vkCmdPushConstants
#undef vkCmdResetQueryPool
#undef vkCmdResolveImage
#undef vkCmdSetBlendConstants
#undef vkCmdSetCullMode
#undef vkCmdSetDepthBias
#undef vkCmdSetDepthBiasEnable
#undef vkCmdSetDepthBounds
#undef vkCmdSetDepthCompareOp
#undef vkCmdSetDepthTestEnable
#undef vkCmdSetDepthWriteEnable
#undef vkCmdSetFrontFace
#undef vkCmdSetLineWidth
#undef vkCmdSetPrimitiveRestartEnable
#undef vkCmdSetPrimitiveTopology
#undef vkCmdSetScissor
#undef vkCmdSetStencilCompareMask
#undef vkCmdSetStencilOp
#undef vkCmdSetStencilReference
#undef vkCmdSetStencilTestEnable
#undef vkCmdSetStencilWriteMask
#undef vkCmdSetViewport
#undef vkCmdWriteTimestamp
#undef vkCmdWriteTimestamp2
#undef vkCreateBuffer
#undef vkCreateCommandPool
#undef vkCreateComputePipelines
#undef vkCreateDescriptorPool
#undef vkCreateDescriptorSetLayout
#undef vkCreateFence
#undef vkCreateGraphicsPipelines
#undef vkCreateImage
#undef vkCreateImageView
#undef vkCreatePipelineCache
#undef vkCreatePipelineLayout
#undef vkCreateQueryPool
#undef vkCreateSampler
#undef vkCreateSemaphore
#undef vkCreateShaderModule
#undef vkCreateSwapchainKHR
#undef vkDestroyBuffer
#undef vkDestroyCommandPool
#undef vkDestroyDescriptorPool
#undef vkDestroyDescriptorSetLayout
#undef vkDestroyDevice
#undef vkDestroyFence
#undef vkDestroyImage
#undef vkDestroyImageView
#undef vkDestroyPipeline
#undef vkDestroyPipelineCache
#undef vkDestroyPipelineLayout
#undef vkDestroyQueryPool
#undef vkDestroySampler
#undef vkDestroySemaphore
#undef vkDestroyShaderModule
#undef vkDestroySwapchainKHR
#undef vkDeviceWaitIdle
#undef vkEndCommandBuffer
#undef vkFreeCommandBuffers
#undef vkFreeDescriptorSets
#undef vkFreeMemory
#undef vkGetBufferMemoryRequirements
#undef vkGetDeviceQueue
#undef vkGetImageMemoryRequirements
#undef vkGetPipelineCacheData
#undef vkGetQueryPoolResults
#undef vkGetSemaphoreCounterValue
#undef vkGetSwapchainImagesKHR
#undef vkMapMemory
#undef vkQueuePresentKHR
#undef vkQueueSubmit
#undef vkQueueSubmit2
#undef vkQueueWaitIdle
#undef vkResetCommandBuffer
#undef vkResetCommandPool
#undef vkResetDescriptorPool
#undef vkResetFences
#undef vkSignalSemaphore
#undef vkUnmapMemory
#undef vkUpdateDescriptorSets
#undef vkWaitForFences
#undef vkWaitSemaphores

#undef vkBindImageMemory
#undef vkCmdDispatchIndirect
#undef vkCmdDrawIndirect
#undef vkCmdExecuteCommands
#undef vkCmdPipelineBarrier
#undef vkCmdSetDepthBounds
#undef vkCmdSetLineWidth
#undef vkCmdWriteTimestamp
#undef vkCreateFence
#undef vkCreateImage
#undef vkDestroyCommandPool
#undef vkDestroyDescriptorPool
#undef vkDestroyDescriptorSetLayout
#undef vkDestroyDevice
#undef vkDestroyFence
#undef vkDestroyImage
#undef vkDestroyPipeline
#undef vkDestroyPipelineCache
#undef vkDestroyPipelineLayout
#undef vkDestroyQueryPool
#undef vkDestroySampler
#undef vkDestroySemaphore
#undef vkDestroyShaderModule
#undef vkFreeCommandBuffers
#undef vkFreeDescriptorSets
#undef vkGetBufferMemoryRequirements
#undef vkGetImageMemoryRequirements
#undef vkMapMemory
#undef vkQueueSubmit
#undef vkQueueWaitIdle
#undef vkResetCommandBuffer
#undef vkResetCommandPool
#undef vkResetFences
#undef vkSignalSemaphore
#undef vkUnmapMemory
#undef vkWaitForFences

#define VKFN(f) load_one(#f, (void **)&g_vkd.f)
VkDispatch g_vkd;

// C trampolines also serve consumers which load functions outside the dispatch table.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_loader_instance_proc(VkInstance instance, const char *name) {
    return g_gipa(instance, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_loader_device_proc(VkDevice device, const char *name) {
    return g_vkd.vkGetDeviceProcAddr(device, name);
}
static void load_dispatch_global(void) {   // before the instance exists (instance = VK_NULL_HANDLE)
    VKFN(vkCreateInstance);
    VKFN(vkEnumerateInstanceExtensionProperties);
    VKFN(vkEnumerateInstanceLayerProperties);
    VKFN(vkGetInstanceProcAddr);
}
static void load_dispatch_instance(void) { // after vkCreateInstance
    VKFN(vkDestroyInstance);
    VKFN(vkEnumeratePhysicalDevices);
    VKFN(vkGetPhysicalDeviceProperties);
    VKFN(vkGetPhysicalDeviceQueueFamilyProperties);
    VKFN(vkGetPhysicalDeviceFeatures2);
    VKFN(vkEnumerateDeviceExtensionProperties);
    VKFN(vkGetPhysicalDeviceFormatProperties);
    VKFN(vkGetPhysicalDeviceSurfaceSupportKHR);
    VKFN(vkCreateAndroidSurfaceKHR);
    VKFN(vkDestroySurfaceKHR);
    VKFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VKFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VKFN(vkGetPhysicalDeviceSurfacePresentModesKHR);
    VKFN(vkCreateDevice);
    VKFN(vkGetDeviceProcAddr);
}
static void load_dispatch_device(void) {   // after vkCreateDevice (device commands via gdpa)
    PFN_vkGetDeviceProcAddr gdpa = g_vkd.vkGetDeviceProcAddr;
#define VKFD(f) do { void *p = (void *)gdpa(g_vk.dev, #f); if (!p) p = (void *)g_gipa(g_vk.inst, #f); \
                     if (!p) FATAL("Vulkan entry point %s not found\n", #f); g_vkd.f = (PFN_##f)p; } while (0)
    VKFD(vkDestroyDevice);
    VKFD(vkGetDeviceQueue);
    VKFD(vkDeviceWaitIdle);
    VKFD(vkCreateBuffer);
    VKFD(vkDestroyBuffer);
    VKFD(vkGetBufferMemoryRequirements);
    VKFD(vkAllocateMemory);
    VKFD(vkFreeMemory);
    VKFD(vkMapMemory);
    VKFD(vkUnmapMemory);
    VKFD(vkBindBufferMemory);
    VKFD(vkBindImageMemory);
    VKFD(vkCreateImage);
    VKFD(vkDestroyImage);
    VKFD(vkGetImageMemoryRequirements);
    VKFD(vkCreateImageView);
    VKFD(vkDestroyImageView);
    VKFD(vkCreateSampler);
    VKFD(vkDestroySampler);
    VKFD(vkCreateShaderModule);
    VKFD(vkDestroyShaderModule);
    VKFD(vkCreatePipelineCache);
    VKFD(vkDestroyPipelineCache);
    VKFD(vkGetPipelineCacheData);
    VKFD(vkCreatePipelineLayout);
    VKFD(vkDestroyPipelineLayout);
    VKFD(vkCreateGraphicsPipelines);
    VKFD(vkCreateComputePipelines);
    VKFD(vkDestroyPipeline);
    VKFD(vkCreateDescriptorSetLayout);
    VKFD(vkDestroyDescriptorSetLayout);
    VKFD(vkCreateDescriptorPool);
    VKFD(vkDestroyDescriptorPool);
    VKFD(vkResetDescriptorPool);
    VKFD(vkAllocateDescriptorSets);
    VKFD(vkFreeDescriptorSets);
    VKFD(vkUpdateDescriptorSets);
    VKFD(vkCreateCommandPool);
    VKFD(vkDestroyCommandPool);
    VKFD(vkResetCommandPool);
    VKFD(vkAllocateCommandBuffers);
    VKFD(vkFreeCommandBuffers);
    VKFD(vkBeginCommandBuffer);
    VKFD(vkEndCommandBuffer);
    VKFD(vkResetCommandBuffer);
    VKFD(vkCreateSemaphore);
    VKFD(vkDestroySemaphore);
    VKFD(vkCreateFence);
    VKFD(vkDestroyFence);
    VKFD(vkWaitForFences);
    VKFD(vkResetFences);
    VKFD(vkGetSemaphoreCounterValue);
    VKFD(vkWaitSemaphores);
    VKFD(vkSignalSemaphore);
    VKFD(vkCreateQueryPool);
    VKFD(vkDestroyQueryPool);
    VKFD(vkGetQueryPoolResults);
    VKFD(vkQueueSubmit);
    VKFD(vkQueueSubmit2);
    VKFD(vkQueueWaitIdle);
    VKFD(vkQueuePresentKHR);
    VKFD(vkCreateSwapchainKHR);
    VKFD(vkDestroySwapchainKHR);
    VKFD(vkGetSwapchainImagesKHR);
    VKFD(vkAcquireNextImageKHR);
    VKFD(vkCmdBeginRendering);
    VKFD(vkCmdEndRendering);
    VKFD(vkCmdBindPipeline);
    VKFD(vkCmdBindDescriptorSets);
    VKFD(vkCmdBindVertexBuffers);
    VKFD(vkCmdBindIndexBuffer);
    VKFD(vkCmdDraw);
    VKFD(vkCmdDrawIndexed);
    VKFD(vkCmdDrawIndirect);
    VKFD(vkCmdDispatch);
    VKFD(vkCmdDispatchIndirect);
    VKFD(vkCmdSetViewport);
    VKFD(vkCmdSetScissor);
    VKFD(vkCmdSetDepthBias);
    VKFD(vkCmdSetDepthBiasEnable);
    VKFD(vkCmdSetLineWidth);
    VKFD(vkCmdSetBlendConstants);
    VKFD(vkCmdSetDepthBounds);
    VKFD(vkCmdSetStencilCompareMask);
    VKFD(vkCmdSetStencilWriteMask);
    VKFD(vkCmdSetStencilReference);
    VKFD(vkCmdSetCullMode);
    VKFD(vkCmdSetFrontFace);
    VKFD(vkCmdSetPrimitiveTopology);
    VKFD(vkCmdSetDepthTestEnable);
    VKFD(vkCmdSetDepthWriteEnable);
    VKFD(vkCmdSetDepthCompareOp);
    VKFD(vkCmdSetStencilTestEnable);
    VKFD(vkCmdSetStencilOp);
    VKFD(vkCmdSetPrimitiveRestartEnable);
    VKFD(vkCmdPushConstants);
    VKFD(vkCmdPipelineBarrier);
    VKFD(vkCmdPipelineBarrier2);
    VKFD(vkCmdCopyBuffer);
    VKFD(vkCmdCopyImage);
    VKFD(vkCmdCopyBufferToImage);
    VKFD(vkCmdCopyImageToBuffer);
    VKFD(vkCmdBlitImage);
    VKFD(vkCmdResolveImage);
    VKFD(vkCmdClearColorImage);
    VKFD(vkCmdClearDepthStencilImage);
    VKFD(vkCmdClearAttachments);
    VKFD(vkCmdBeginQuery);
    VKFD(vkCmdEndQuery);
    VKFD(vkCmdResetQueryPool);
    VKFD(vkCmdWriteTimestamp);
    VKFD(vkCmdWriteTimestamp2);
    VKFD(vkCmdExecuteCommands);
#undef VKFD
}
#undef VKFN
#include "vk_dispatch_defs.h"
#endif  // LETSGO_ADRENOTOOLS

// Accessor for surface recovery
SDL_Window *vk_sdl_window(void) { return window; }

// Use the selected loader for both initial creation and surface recovery.
static bool create_surface(void) {
#ifdef LETSGO_ADRENOTOOLS
    extern ANativeWindow *letsgo_get_native_window(void);
    ANativeWindow *nw = letsgo_get_native_window();
    if (!nw) return SDL_SetError("Android native window is not available");
    VkAndroidSurfaceCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = nw};
    VkResult r = vkCreateAndroidSurfaceKHR(g_vk.inst, &ci, NULL, &g_vk.surface);
    if (r != VK_SUCCESS) return SDL_SetError("vkCreateAndroidSurfaceKHR: %d", r);
    return true;
#else
    return SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface);
#endif
}

// Watch successful queue presentations, not physical scanout. Lifecycle callbacks also
// mark the target stale. Render-thread writes / Android UI reads use atomics.
static atomic_uint g_last_present_ms;
static atomic_uint g_stalled_since_ms;
// UI thread raises a flag; render thread owns the swapchain.
static atomic_bool g_resume_requested;
// How long the display may stay silent before the game says so (LARECOMP_BLACK_SCREEN_MS, 8 s).
static Uint32 black_screen_limit_ms(void) {
    static Uint32 v = 0;
    if (!v) {
        const char *e = getenv("LARECOMP_BLACK_SCREEN_MS");
        v = e ? (Uint32)atoi(e) : 8000;
    }
    return v;
}
// Called by the present path and by the state machine: the display works again.
void display_alive(void) {
    atomic_store(&g_last_present_ms, SDL_GetTicks());
    atomic_store(&g_stalled_since_ms, 0);
}
// Called when the surface goes away: the display is dead until a new one is built.
void display_dead(void) {
    if (!atomic_load(&g_stalled_since_ms)) atomic_store(&g_stalled_since_ms, SDL_GetTicks());
}
// Back in the foreground (LetsGoActivity.onResume): the time spent in the background, where the game waits and presents
// nothing (runtime/lifecycle.c), is not a silent display. The guard's limit counts again from now.
void display_foreground(void) {
    Uint32 now = SDL_GetTicks();
    if (atomic_load(&g_stalled_since_ms)) atomic_store(&g_stalled_since_ms, now);
    if (atomic_load(&g_last_present_ms)) atomic_store(&g_last_present_ms, now);
}
// 1 when the display has been silent for too long: the engine is alive but the screen shows nothing.
int display_stalled(void) {
    Uint32 limit = black_screen_limit_ms();
    if (!limit) return 0;                       // 0 = the guard is off
    Uint32 now = SDL_GetTicks();
    Uint32 stalled = atomic_load(&g_stalled_since_ms);
    if (stalled && (Uint32)(now - stalled) > limit) return 1;
    // Nothing reached the screen since the last good frame.
    Uint32 last = atomic_load(&g_last_present_ms);
    if (last && (Uint32)(now - last) > limit) return 1;
    return 0;
}
// How long the display has been silent, in ms (0 when it is fine).
int display_stalled_ms(void) {
    Uint32 now = SDL_GetTicks();
    Uint32 worst = 0;
    Uint32 stalled = atomic_load(&g_stalled_since_ms);
    if (stalled) { Uint32 d = now - stalled; if (d > worst) worst = d; }
    Uint32 last = atomic_load(&g_last_present_ms);
    if (last) { Uint32 d = now - last; if (d > worst) worst = d; }
    return (int)worst;
}
// Rebuild on request; the next accepted present clears the watchdog.
void display_resume_requested(void) {
    // Android lifecycle callbacks and the manual button both request a render-thread rebuild.
    LOG("[vk] display surface rebuild requested\n");
    atomic_store(&g_resume_requested, true);
}

// Diagnostic recipe only. Java exports the variable only when debug.larecomp.diag=1
// AND debug.larecomp.force_display_stalled=1. Hold before acquire (no semaphore/image
// left in flight), after the first good picture, until a rebuild request releases it.
static bool force_display_stalled(bool resume) {
    static int enabled = -1;
    static bool started, finished;
    if (enabled < 0) {
        const char *e = getenv("LARECOMP_FORCE_DISPLAY_STALLED");
        enabled = e && !strcmp(e, "1");
    }
    if (!enabled || finished) return false;
    if (resume && started) {
        finished = true;
        LOG("[display-guard] diagnostic hold released by rebuild request\n");
        return false;
    }
    if (!atomic_load(&g_last_present_ms)) return false;
    if (!started) LOG("[display-guard] diagnostic hold started at %u ms (engine continues)\n", (unsigned)SDL_GetTicks());
    started = true;
    display_dead();
    return true;
}

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

bool g_pipe_exec_stats;  // LARECOMP_PIPE_STATS and the driver has VK_KHR_pipeline_executable_properties

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    LOG("[vk] active GPU: %s (vendor 0x%04x, device 0x%04x, driver v%u.%u.%u)\n",
        g_vk.props.deviceName, g_vk.props.vendorID, g_vk.props.deviceID,
        VK_VERSION_MAJOR(g_vk.props.driverVersion), VK_VERSION_MINOR(g_vk.props.driverVersion), VK_VERSION_PATCH(g_vk.props.driverVersion));
#ifdef LETSGO_ADRENOTOOLS
    // Driver properties are a pNext query, not a standalone Vulkan entry point.
    PFN_vkGetPhysicalDeviceProperties2 props2 =
        (PFN_vkGetPhysicalDeviceProperties2)g_gipa(g_vk.inst, "vkGetPhysicalDeviceProperties2");
    if (!props2) FATAL("vkGetPhysicalDeviceProperties2 not found\n");
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    props2(g_vk.phys, &props);
    LOG("[vk] active driver: %s (driverID=%u, %s)\n", driver.driverName, driver.driverID, driver.driverInfo);
#endif
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[8];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    // LARECOMP_PIPE_STATS: what the driver says about each compiled shader (registers, spills, ...), exec.c pipe_exec_report
    g_pipe_exec_stats = getenv("LARECOMP_PIPE_STATS") && has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (g_pipe_exec_stats) ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    else if (getenv("LARECOMP_PIPE_STATS")) LOG("[vk] LARECOMP_PIPE_STATS: the driver has no VK_KHR_pipeline_executable_properties\n");
    g_fault_ext = fault;
    free(ep);

    // features the backend relies on (all present on current desktop drivers).
    // On Android/mobile drivers some may be missing: query first, request only
    // what is supported, otherwise vkCreateDevice fails with
    // VK_ERROR_FEATURE_NOT_PRESENT.
    VkPhysicalDeviceVulkan11Features have11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &have11};
    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &have12};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have13};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = have13.dynamicRendering,
                                            .synchronization2 = have13.synchronization2,
                                            .shaderDemoteToHelperInvocation = have13.shaderDemoteToHelperInvocation,
                                            .shaderTerminateInvocation = have13.shaderTerminateInvocation,
                                            .maintenance4 = have13.maintenance4,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = have12.timelineSemaphore,
                                            .shaderInt8 = have12.shaderInt8,
                                            .samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge,
                                            .uniformAndStorageBuffer8BitAccess = have12.uniformAndStorageBuffer8BitAccess,
                                            .storageBuffer8BitAccess = have12.storageBuffer8BitAccess,
                                            .scalarBlockLayout = have12.scalarBlockLayout, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = have11.storageBuffer16BitAccess, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE,
        .pNext = f.pNext};
    if (g_pipe_exec_stats) f.pNext = &pe;
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->pipelineStatisticsQuery = h->pipelineStatisticsQuery;  // (LARECOMP_GPUPROF_STATS: invocations per draw)
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    if (getenv("LARECOMP_NO_ROBUST")) b->robustBufferAccess = VK_FALSE;  // (research: what the bounds checks cost)
    b->shaderClipDistance = h->shaderClipDistance;
    g_vk.shader_int64 = h->shaderInt64;  // (exec.c: without it, the translator emits 64-bit integer math on 32-bit words)
    if (!h->shaderInt64) LOG("[vk] no shaderInt64: shaders get 64-bit integer math (addresses, carries) as 32-bit pairs\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
#ifdef LETSGO_ADRENOTOOLS
    load_dispatch_device();  // before the first device command or allocator call
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
#ifdef LETSGO_ADRENOTOOLS
    // The first two fields are the loader callbacks; avoid macro-expanded field names.
    VmaVulkanFunctions functions = {vk_loader_instance_proc, vk_loader_device_proc};
    ai.pVulkanFunctions = &functions;
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

void vk_pipeline_cache_save(void) {  // (exit and exec.c's periodic save: one writer of the .tmp at a time)
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) { pthread_mutex_unlock(&lock); return; }
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    // MISSÃO A v0065: verifica estado antes de criar swapchain
    if (g_vk.present_state != PRESENT_READY && g_vk.present_state != PRESENT_RECREATING) {
        LOG("[vk] create_swapchain: skipped, present_state=%d (not READY/RECREATING)\n", g_vk.present_state);
        return;
    }
    if (g_vk.surface == VK_NULL_HANDLE) {
        LOG("[vk] create_swapchain: skipped, surface is NULL\n");
        g_vk.present_state = PRESENT_WAITING_FOR_WINDOW;
        return;
    }
    VkSurfaceCapabilitiesKHR caps;
    // MISSÃO A: não usar VK_CHECK aqui - falha de superfície não deve ser FATAL
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (cr != VK_SUCCESS) {
        LOG("[vk] create_swapchain: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %d, surface lost\n", cr);
        g_vk.present_state = PRESENT_SURFACE_LOST;
        return;
    }
    extern SDL_Window *vk_sdl_window(void);
    SDL_Window *window = vk_sdl_window();
    if (!window) {
        LOG("[vk] create_swapchain: no SDL window\n");
        g_vk.present_state = PRESENT_WAITING_FOR_WINDOW;
        return;
    }
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext;
    if (caps.currentExtent.width != UINT32_MAX) {
        // Surface defines the size: respect it.
        ext = caps.currentExtent;
    } else {
        // Variable size: use window size, clamped to supported range.
        ext.width = (uint32_t)w;
        ext.height = (uint32_t)h;
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) return;  // minimised
    // The present pass draws the picture pre-rotated: the swapchain images are in the display's own
    // orientation (portrait on a phone held sideways), like the Wonder port. Without this the
    // compositor has to rotate the landscape buffer itself and the image comes out stretched.
    bool sideways = (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                                              VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) != 0;
    if (sideways && ext.width > ext.height) { uint32_t t = ext.width; ext.width = ext.height; ext.height = t; }
    // Use the surface size directly (compatible with surface capabilities).
    // The 16:9 aspect ratio is handled via viewport letterboxing in the
    // present pass (output_scale), NOT by changing the swapchain size.
    // Log surface info for orientation debugging (limited: only on creation)
    LOG("[vk] surface: currentExtent %ux%u, currentTransform 0x%x, supportedTransforms 0x%x\n",
        caps.currentExtent.width, caps.currentExtent.height,
        caps.currentTransform, caps.supportedTransforms);
    LOG("[vk] window %dx%d -> swapchain %ux%u, preTransform 0x%x\n",
        w, h, ext.width, ext.height, caps.currentTransform);
    // Map the surface preTransform to a present-shader rotation compensation.
    // The swapchain uses preTransform = currentTransform; the present shader
    // rotates UVs to upright the image.
    switch (caps.currentTransform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:  g_vk.sc_rotation = 1.0f; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: g_vk.sc_rotation = 2.0f; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: g_vk.sc_rotation = 3.0f; break;
        default: g_vk.sc_rotation = 0.0f; break;  // IDENTITY, HORIZONTAL_MIRROR, etc.: no rotation
    }
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    // MISSÃO A v0065: verifica estado, não recria se aguardando janela
    if (g_vk.present_state == PRESENT_WAITING_FOR_WINDOW) {
        LOG("[vk] recreate_swapchain: skipped, waiting for window\n");
        return;
    }
    g_vk.present_state = PRESENT_RECREATING;
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
    // Se create_swapchain falhou, o estado já foi atualizado
    if (g_vk.present_state == PRESENT_RECREATING) {
        // Sucesso - volta para READY (ou mantém se create_swapchain mudou)
        // create_swapchain não muda para READY, então fazemos aqui se swapchain foi criado
        if (g_vk.swapchain != VK_NULL_HANDLE) {
            g_vk.present_state = PRESENT_READY;
        }
    }
}

// Recover from VK_ERROR_SURFACE_LOST_KHR: destroy and recreate the
// Vulkan surface from the SDL window, then recreate the swapchain.
// Called when vkAcquireNextImageKHR or vkQueuePresentKHR reports
// the surface as lost (e.g., after onPause/onResume on Android).
// MISSÃO A v0065: usa máquina de estados explícita
static void recover_lost_surface(void) {
    LOG("[vk] surface lost: recreating surface and swapchain\n");
    g_vk.present_state = PRESENT_SURFACE_LOST;
    vkDeviceWaitIdle(g_vk.dev);
    // Destroy swapchain first (depends on surface)
    if (g_vk.swapchain) {
        vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
        g_vk.swapchain = VK_NULL_HANDLE;
    }
    // Destroy the lost surface
    if (g_vk.surface) {
        vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
        g_vk.surface = VK_NULL_HANDLE;
    }
    // Recreate surface from SDL window
    extern SDL_Window *vk_sdl_window(void);
    SDL_Window *window = vk_sdl_window();
    if (window) {
        if (!create_surface()) {
            LOG("[vk] surface recovery FAILED: %s (waiting for window)\n", SDL_GetError());
            g_vk.present_state = PRESENT_WAITING_FOR_WINDOW;
            return;
        }
        LOG("[vk] surface recreated\n");
        g_vk.present_state = PRESENT_RECREATING;
    } else {
        LOG("[vk] surface recovery FAILED: no SDL window (waiting for window)\n");
        g_vk.present_state = PRESENT_WAITING_FOR_WINDOW;
        return;
    }
    // Só recria a swapchain se a superfície for válida
    if (g_vk.surface != VK_NULL_HANDLE) {
        create_swapchain();
        // create_swapchain atualiza o estado em caso de falha
        // Se ainda estamos em RECREATING e temos swapchain, vamos para READY
        if (g_vk.present_state == PRESENT_RECREATING && g_vk.swapchain != VK_NULL_HANDLE) {
            g_vk.present_state = PRESENT_READY;
            LOG("[vk] surface recovery: READY\n");
        }
    } else {
        LOG("[vk] surface is null, waiting for window\n");
        g_vk.present_state = PRESENT_WAITING_FOR_WINDOW;
    }
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    // MISSÃO A v0065: não executa acquire/present se não estiver READY
    if (g_vk.present_state != PRESENT_READY) {
        // Tenta recuperar se estiver em SURFACE_LOST (mas não em loop apertado)
        static uint64_t last_retry = 0;
        extern uint64_t vk_frame_counter(void);
        uint64_t now = vk_frame_counter();
        if (g_vk.present_state == PRESENT_SURFACE_LOST && now - last_retry > 60) {
            last_retry = now;
            LOG("[vk] frame_begin: attempting recovery from SURFACE_LOST\n");
            recover_lost_surface();
        }
        // Se ainda não está READY, pula o frame
        if (g_vk.present_state != PRESENT_READY) {
            return false;
        }
    }
    int w, h;
    extern SDL_Window *vk_sdl_window(void);
    SDL_Window *window = vk_sdl_window();
    if (!window) return false;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    // Only recreate if window size actually changed (not every frame).
    // The swapchain follows the surface (currentExtent); window size changes
    // indicate a real resize that needs a new swapchain.
    static int last_w = 0, last_h = 0;
    static unsigned recreate_count = 0;
    if (!g_vk.swapchain || w != last_w || h != last_h) {
        if (g_vk.swapchain) {
            LOG("[vk] swapchain recreate #%u: window %dx%d -> %dx%d\n", ++recreate_count, last_w, last_h, w, h);
        }
        recreate_swapchain();
        last_w = w; last_h = h;
    }
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    } else if (r == VK_ERROR_SURFACE_LOST_KHR) {
        // Surface lost (e.g., after onPause/onResume): recreate surface + swapchain
        recover_lost_surface();
        if (g_vk.swapchain) {
            r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
        }
    }
    static unsigned acquire_fails;  // (a window the driver keeps reporting out of date: logged once per 600, not every frame)
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        if (acquire_fails++ % 600 == 0) LOG("[vk] acquire failed: %d (%u times)\n", r, acquire_fails);
        return false;
    }
    acquire_fails = 0;
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    // P1 v0066: captura de frame quando houve draw de texto com gatilho
    extern _Atomic int g_font_capture_frame;
    const char *effective_shot = shot_path;
    char font_shot_path[1024] = {0};
    if (atomic_load(&g_font_capture_frame)) {
        atomic_store(&g_font_capture_frame, 0);
        const char *root = getenv("LARECOMP_DATA_ROOT");
        extern uint64_t vk_frame_counter(void);
        uint64_t fc = vk_frame_counter();
        // DEFEITO A v0070: nome único com número do frame (preserva capturas anteriores)
        if (root && root[0]) snprintf(font_shot_path, sizeof(font_shot_path), "%s/font_frame_%llu.png", root, (unsigned long long)fc);
        else snprintf(font_shot_path, sizeof(font_shot_path), "font_frame_%llu.png", (unsigned long long)fc);
        effective_shot = font_shot_path;
        // Log para correlação
        LOG("[font-capture] Saving frame screenshot to %s (frame %llu)\n", font_shot_path, (unsigned long long)fc);
    }
    if (effective_shot) {
        save_window_image(cb, &shot);
        // DEFEITO A v0070: verifica se o arquivo foi realmente escrito
        // (save_window_image é assíncrono; o log de confirmação vem do vk_save_png)
    }
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    else if (r == VK_ERROR_SURFACE_LOST_KHR) recover_lost_surface();
    cur_frame.ok = false;
    if (effective_shot) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(effective_shot, px, w, h);
    }
}

// LARECOMP_SHOT_RAW=<file>: the numbered series (".../frame_NNNNN.png") goes to one raw file instead, no PNG encoding:
// per frame a record of u32 frame number + 480x270 RGB (box-filtered), read by tools/bateria_scan.py. Several times faster
// than PNGs (and ffmpeg) for a 900-frame route.
#define RAW_W 480
#define RAW_H 270
static bool shot_raw(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    static FILE *f; static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("LARECOMP_SHOT_RAW");
        if (e && *e) { f = fopen(e, "wb"); if (f) setvbuf(f, NULL, _IOFBF, 1 << 20); else LOG("[vk] cannot open %s\n", e); }
    }
    const char *n = strstr(path, "frame_");
    if (!f || !n) return false;
    static uint8_t out[RAW_W * RAW_H * 3];
    for (uint32_t y = 0; y < RAW_H; y++) {
        uint32_t y0 = (uint64_t)y * h / RAW_H, y1 = (uint64_t)(y + 1) * h / RAW_H; if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < RAW_W; x++) {
            uint32_t x0 = (uint64_t)x * w / RAW_W, x1 = (uint64_t)(x + 1) * w / RAW_W; if (x1 <= x0) x1 = x0 + 1;
            uint32_t s[3] = {0, 0, 0};
            for (uint32_t yy = y0; yy < y1; yy++)
                for (uint32_t xx = x0; xx < x1; xx++) { const uint8_t *p = rgba + ((uint64_t)yy * w + xx) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; }
            uint32_t c = (y1 - y0) * (x1 - x0);
            uint8_t *o = out + ((uint64_t)y * RAW_W + x) * 3;
            o[0] = (uint8_t)(s[0] / c); o[1] = (uint8_t)(s[1] / c); o[2] = (uint8_t)(s[2] / c);
        }
    }
    uint32_t fn = (uint32_t)atol(n + 6);
    fwrite(&fn, 4, 1, f); fwrite(out, sizeof out, 1, f); fflush(f);  // (the game leaves with _exit)
    if (getenv("LARECOMP_SHOT_RAW_LAST") && (long)fn >= atol(getenv("LARECOMP_SHOT_RAW_LAST"))) { LOG("[vk] raw frames done at %u\n", fn); fflush(stderr); _exit(0); }
    return true;
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (shot_raw(path, rgba, w, h)) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    bool is_font_capture = path && strstr(path, "font_frame") != NULL;
    if (!s || !SDL_SavePNG(s, path)) {
        LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
        if (is_font_capture) LOG("[font-capture] CAP_WRITE_FAILED: %s (%s)\n", path, SDL_GetError());
    } else {
        LOG("[vk] saved %s (%ux%u)\n", path, w, h);
        if (is_font_capture) LOG("[font-capture] CAP_WRITE_OK: %s (%ux%u)\n", path, w, h);
    }
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float bx, float by, float bw, float bh, float pw, float ph, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
double g_current_fps = 0;  // for Android FPS overlay
double g_frame_ms = 0;     // max frame time in ms
static NvnTexture *g_last_presented;

// The game image is computed in upright (viewed) space; the swapchain images are in the display's
// own orientation (portrait on a phone held sideways, when the surface transform is ROTATE_90/270).
// Maps the upright letterbox into the swapchain image's coordinates (like the Wonder port).
static void present_box(double dw, double dh, double vx, double vy, double vw, double vh, float *out) {
    // (dw,dh): upright (viewed) target size; (vx,vy,vw,vh): the letterbox in it.
    double W = g_vk.sc_extent.width, H = g_vk.sc_extent.height;
    if (g_vk.sc_rotation == 1.0f) {        // ROTATE_90: the viewer sees the buffer rotated 90° CCW
        out[0] = (float)(W * (1.0 - (vy + vh) / dh));
        out[1] = (float)(H * vx / dw);
        out[2] = (float)(W * vh / dh);
        out[3] = (float)(H * vw / dw);
    } else if (g_vk.sc_rotation == 3.0f) { // ROTATE_270: the viewer sees the buffer rotated 90° CW
        out[0] = (float)(W * vy / dh);
        out[1] = (float)(H * (1.0 - (vx + vw) / dw));
        out[2] = (float)(W * vh / dh);
        out[3] = (float)(H * vw / dw);
    } else if (g_vk.sc_rotation == 2.0f) { // ROTATE_180
        out[0] = (float)(W * (1.0 - (vx + vw) / dw));
        out[1] = (float)(H * (1.0 - (vy + vh) / dh));
        out[2] = (float)(W * vw / dw);
        out[3] = (float)(H * vh / dh);
    } else {                              // IDENTITY
        out[0] = (float)vx; out[1] = (float)vy;
        out[2] = (float)vw; out[3] = (float)vh;
    }
}

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void backend_represent(void);
static void represent_job(void *unused) { (void)unused; backend_represent(); }
void backend_represent(void) {
    if (rt_active() && !rt_on_render_thread()) { rt_call(represent_job, NULL); return; }  // (the keyboard dialog: game thread)
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

void backend_present(NvnTexture *t) {
    {   // (LARECOMP_HEAL_KEYS research key F5: every texture re-checks its memory)
        extern volatile int g_heal_tex;
        extern void backend_pool_written(uint64_t addr, uint64_t size);
        if (g_heal_tex) { g_heal_tex = 0; backend_pool_written(0x7000000000ull, 0x320000000ull); }
    }
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    // Frame interpolation (60 fps, exec.c vk_interp_render): the in-between image now, this frame's own half a frame
    // later (the period: the game's present rhythm, smoothed).
    TexBackend *hold = NULL;
    extern bool vk_interp_render(TexBackend *b, TexBackend **hold);
    bool two = vk_interp_render(b, &hold);
    static uint64_t last_present, period = 33333333;
    if (last_present && t0 - last_present > 5000000 && t0 - last_present < 100000000) period = (period * 7 + (t0 - last_present)) / 8;
    last_present = t0;
    {   // 60 fps mode: a menu's frames come every 33.3 ms, but the smoothed period still holds the world's 16.7 ms for its
        // first frames - the second image would come too early
        extern bool fps60_menu(void);
        if (fps60_menu()) period = 33333333;
    }
    for (int image = two ? 0 : 1; image < 2; image++) {
    bool final = image == 1;
    if (final && two) {
        uint64_t due = t0 + period / 2, now = SDL_GetTicksNS();
        if (due > now) SDL_DelayPrecise(due - now);
        b = hold;
    }
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        // The present is computed in upright (viewed) space: when the surface is pre-rotated (ROTATE_90/270)
        // the swapchain images are in the panel's orientation (portrait) and the upright target swaps
        // width/height, like the Wonder port.
        bool sideways = g_vk.sc_rotation == 1.0f || g_vk.sc_rotation == 3.0f;
        double dw = sideways ? g_vk.sc_extent.height : g_vk.sc_extent.width;
        double dh = sideways ? g_vk.sc_extent.width : g_vk.sc_extent.height;
        // render scale + placement in the window (the game's 16:9 image, fitted): runtime/nvn/output_scale.c;
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        OutScale os;
        output_scale(dw, dh, false, SDL_GetTicksNS() / 1e9, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh;
        float box[4];  // the letterbox in swapchain-image coordinates
        present_box(dw, dh, vx, vy, vw, vh, box);
        float crop[4] = {0, 0, 1, 1};  // uv origin and size: the whole image
        {   // where the game image is on the screen (pixels): the mouse as a finger on the touch screen (hle/input.c)
            // (upright/window coordinates - the touch layer sees the window, not the swapchain image)
            extern double g_present_rect[4];
            g_present_rect[0] = vx, g_present_rect[1] = vy, g_present_rect[2] = vw, g_present_rect[3] = vh;
        }
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, g_vk.sc_rotation, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        // The game's first two presents are a blue full-screen 2D quad drawn while it loads (then black, then the
        // controller screen): a flash on every start. Keep the window black for them. LARECOMP_HIDE_FIRST_FRAMES=<n>
        // (0: show everything).
        static long hide = -1;
        if (hide < 0) hide = getenv("LARECOMP_HIDE_FIRST_FRAMES") ? atol(getenv("LARECOMP_HIDE_FIRST_FRAMES")) : 2;
        if ((long)total_frames >= hide) vk_present_draw(cb, b, box[0], box[1], box[2], box[3], (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (final && menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        // (the in-between image: build/window_mid.png, with interpolation on)
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        // LARECOMP_WINDOW_SHOTS=<n>,<n>,...: the in-between image and the frame of each, build/frames/{mid,fin}_<n>.png
        static long list[512]; static int nlist = -1;
        if (nlist < 0) {
            nlist = 0;
            const char *e = getenv("LARECOMP_WINDOW_SHOTS");
            for (; e && *e && nlist < 512; nlist++) { list[nlist] = atol(e); e = strchr(e, ','); if (!e) { nlist++; break; } e++; }
        }
        static char lpath[96];
        extern long fps60_scene_start(void);
        static bool rel_list = false;
        { static bool init; if (!init) { init = true; rel_list = getenv("LARECOMP_WINDOW_SHOTS_REL") != NULL; } }
        const char *path = shot ? (final ? "build/window.png" : "build/window_mid.png") : NULL;
        for (int i = 0; i < nlist; i++)
            if (rel_list ? (fps60_scene_start() >= 0 && (long)total_frames + 1 - fps60_scene_start() == list[i])
                         : (long)total_frames + 1 == list[i]) {
                mkdir("build/frames", 0755);
                snprintf(lpath, sizeof lpath, "build/frames/%s_%ld.png", final ? "fin" : "mid", list[i]);
                path = lpath;
            }
        vk_frame_end(path);
        if (final && shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
    {   // Opt-in game-present intervals (not the extra menu image shown by the 60 fps mode).
        static FILE *csv;
        static uint64_t previous;
        static bool initialized;
        if (!initialized) {
            initialized = true;
            const char *path = getenv("LARECOMP_FRAMETIME_CSV");
            if (path && *path) {
                csv = fopen(path, "w");
                if (csv) fputs("present,dt_ms\n", csv);
                else LOG("[vk] cannot open frametime CSV: %s\n", path);
            }
        }
        if (csv) {
            uint64_t now = SDL_GetTicksNS();
            if (previous) fprintf(csv, "%llu,%.6f\n", (unsigned long long)total_frames, (now - previous) / 1e6);
            previous = now;
            if (total_frames % 60 == 0) fflush(csv);
            static long exit_at = -1;
            if (exit_at < 0) exit_at = getenv("LARECOMP_EXIT_AT_PRESENT") ? atol(getenv("LARECOMP_EXIT_AT_PRESENT")) : 0;
            if (exit_at && (long)total_frames >= exit_at) { LOG("[vk] exit at present %llu\n", (unsigned long long)total_frames); fflush(csv); fflush(stderr); _exit(0); }
        }
    }
    {   // LARECOMP_RENDERDOC_AT=<frame>: under RenderDoc (renderdoccmd capture), capture the frame after that one
        extern void renderdoc_frame(uint64_t frame);
        renderdoc_frame(total_frames);
    }
    // (with the render thread the game thread runs these three when it queues the present: rt_present)
    extern void input_frame(uint64_t frame);
    if (!rt_on_render_thread()) input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0 && total_frames > 2600) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    if (!rt_on_render_thread()) fnprof_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    if (!rt_on_render_thread()) reboot_test_frame(total_frames);
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    {   // LARECOMP_SHOT_REL=<n>,<n>,...: the game image n presents after a capture began (fps60.c marks it)
        extern long fps60_scene_start(void);
        static const char *rel = (const char *)1;
        if (rel == (const char *)1) { rel = getenv("LARECOMP_SHOT_REL"); if (rel) mkdir("build/frames", 0755); }
        long st = rel ? fps60_scene_start() : -1;
        for (const char *p = rel; st >= 0 && p && *p;) {
            long n = strtol(p, (char **)&p, 10);
            if ((long)total_frames == st + n) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/rel_%04ld.png", n);
                renderer_save_texture_tex(b, path);
            }
            if (*p == ',') p++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        if (last_fps_time > 0) {
            g_current_fps = frames / (now - last_fps_time);
            g_frame_ms = max_gap * 1000.0;
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms (%s)\n", now, (unsigned long long)total_frames,
                g_current_fps, g_frame_ms, renderer_stats());
        }
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Force landscape orientation: the game must render 1280x720, not 720x1280.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    settings_load();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    input_load_user_mappings();
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow(RT_APP_NAME, win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    // On Android, g_settings.fullscreen is forced true in settings_load() (like the Zelda port).
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) {
        bool ok = SDL_SetWindowFullscreen(window, true);
        LOG("[vk] SDL_SetWindowFullscreen(true) -> %s%s%s\n", ok ? "ok" : "FAIL", ok ? "" : ": ", ok ? "" : SDL_GetError());
    }
#ifdef LETSGO_ADRENOTOOLS
    g_gipa = load_gipa();
    load_dispatch_global();
#endif
    create_instance();
#ifdef LETSGO_ADRENOTOOLS
    load_dispatch_instance();
#endif
    if (!create_surface()) FATAL("Vulkan surface creation: %s\n", SDL_GetError());
    // MISSÃO A v0065: inicializa máquina de estados
    g_vk.present_state = PRESENT_READY;
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    rt_reset();  // (the queued work belongs to the game that is going away)
    if (!g_vk.dev) return;
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                exit(0);
            }
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
    }
}
