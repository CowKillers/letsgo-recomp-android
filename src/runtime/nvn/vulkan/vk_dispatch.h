// Vulkan dispatch table: every Vulkan entry point the backend uses goes through g_vkd,
// loaded at startup from the Vulkan loader in use (the system loader, or a custom GPU driver
// via adrenotools). This is what lets a Turnip driver replace the system driver without
// touching the ~90 call sites. Generated from the call sites in this directory.
#pragma once
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#endif

typedef struct VkDispatch {
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkCmdBeginQuery vkCmdBeginQuery;
    PFN_vkCmdBeginRendering vkCmdBeginRendering;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
    PFN_vkCmdBlitImage vkCmdBlitImage;
    PFN_vkCmdClearAttachments vkCmdClearAttachments;
    PFN_vkCmdClearColorImage vkCmdClearColorImage;
    PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage;
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
    PFN_vkCmdCopyImage vkCmdCopyImage;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
    PFN_vkCmdDispatch vkCmdDispatch;
    PFN_vkCmdDraw vkCmdDraw;
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed;
    PFN_vkCmdEndQuery vkCmdEndQuery;
    PFN_vkCmdEndRendering vkCmdEndRendering;
    PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2;
    PFN_vkCmdPushConstants vkCmdPushConstants;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
    PFN_vkCmdResolveImage vkCmdResolveImage;
    PFN_vkCmdSetBlendConstants vkCmdSetBlendConstants;
    PFN_vkCmdSetCullMode vkCmdSetCullMode;
    PFN_vkCmdSetDepthBias vkCmdSetDepthBias;
    PFN_vkCmdSetDepthBiasEnable vkCmdSetDepthBiasEnable;
    PFN_vkCmdSetDepthCompareOp vkCmdSetDepthCompareOp;
    PFN_vkCmdSetDepthTestEnable vkCmdSetDepthTestEnable;
    PFN_vkCmdSetDepthWriteEnable vkCmdSetDepthWriteEnable;
    PFN_vkCmdSetFrontFace vkCmdSetFrontFace;
    PFN_vkCmdSetPrimitiveRestartEnable vkCmdSetPrimitiveRestartEnable;
    PFN_vkCmdSetPrimitiveTopology vkCmdSetPrimitiveTopology;
    PFN_vkCmdSetScissor vkCmdSetScissor;
    PFN_vkCmdSetStencilCompareMask vkCmdSetStencilCompareMask;
    PFN_vkCmdSetStencilOp vkCmdSetStencilOp;
    PFN_vkCmdSetStencilReference vkCmdSetStencilReference;
    PFN_vkCmdSetStencilTestEnable vkCmdSetStencilTestEnable;
    PFN_vkCmdSetStencilWriteMask vkCmdSetStencilWriteMask;
    PFN_vkCmdSetViewport vkCmdSetViewport;
    PFN_vkCmdWriteTimestamp2 vkCmdWriteTimestamp2;
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkCreateComputePipelines vkCreateComputePipelines;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkCreateDevice vkCreateDevice;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;
    PFN_vkCreatePipelineCache vkCreatePipelineCache;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkCreateQueryPool vkCreateQueryPool;
    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkCreateSemaphore vkCreateSemaphore;
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR;
    PFN_vkGetPipelineCacheData vkGetPipelineCacheData;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
    PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    PFN_vkQueuePresentKHR vkQueuePresentKHR;
    PFN_vkQueueSubmit2 vkQueueSubmit2;
    PFN_vkResetDescriptorPool vkResetDescriptorPool;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
    PFN_vkWaitSemaphores vkWaitSemaphores;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCmdDispatchIndirect vkCmdDispatchIndirect;
    PFN_vkCmdDrawIndirect vkCmdDrawIndirect;
    PFN_vkCmdExecuteCommands vkCmdExecuteCommands;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdSetDepthBounds vkCmdSetDepthBounds;
    PFN_vkCmdSetLineWidth vkCmdSetLineWidth;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;
    PFN_vkCreateFence vkCreateFence;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkDestroyFence vkDestroyFence;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkDestroyPipeline vkDestroyPipeline;
    PFN_vkDestroyPipelineCache vkDestroyPipelineCache;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkDestroyQueryPool vkDestroyQueryPool;
    PFN_vkDestroySampler vkDestroySampler;
    PFN_vkDestroySemaphore vkDestroySemaphore;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
    PFN_vkFreeDescriptorSets vkFreeDescriptorSets;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkQueueSubmit vkQueueSubmit;
    PFN_vkQueueWaitIdle vkQueueWaitIdle;
    PFN_vkResetCommandBuffer vkResetCommandBuffer;
    PFN_vkResetCommandPool vkResetCommandPool;
    PFN_vkResetFences vkResetFences;
    PFN_vkSignalSemaphore vkSignalSemaphore;
    PFN_vkUnmapMemory vkUnmapMemory;
    PFN_vkWaitForFences vkWaitForFences;
} VkDispatch;

extern VkDispatch g_vkd;

#define vkAcquireNextImageKHR g_vkd.vkAcquireNextImageKHR
#define vkAllocateCommandBuffers g_vkd.vkAllocateCommandBuffers
#define vkAllocateDescriptorSets g_vkd.vkAllocateDescriptorSets
#define vkAllocateMemory g_vkd.vkAllocateMemory
#define vkBeginCommandBuffer g_vkd.vkBeginCommandBuffer
#define vkBindBufferMemory g_vkd.vkBindBufferMemory
#define vkCmdBeginQuery g_vkd.vkCmdBeginQuery
#define vkCmdBeginRendering g_vkd.vkCmdBeginRendering
#define vkCmdBindDescriptorSets g_vkd.vkCmdBindDescriptorSets
#define vkCmdBindIndexBuffer g_vkd.vkCmdBindIndexBuffer
#define vkCmdBindPipeline g_vkd.vkCmdBindPipeline
#define vkCmdBindVertexBuffers g_vkd.vkCmdBindVertexBuffers
#define vkCmdBlitImage g_vkd.vkCmdBlitImage
#define vkCmdClearAttachments g_vkd.vkCmdClearAttachments
#define vkCmdClearColorImage g_vkd.vkCmdClearColorImage
#define vkCmdClearDepthStencilImage g_vkd.vkCmdClearDepthStencilImage
#define vkCmdCopyBuffer g_vkd.vkCmdCopyBuffer
#define vkCmdCopyBufferToImage g_vkd.vkCmdCopyBufferToImage
#define vkCmdCopyImage g_vkd.vkCmdCopyImage
#define vkCmdCopyImageToBuffer g_vkd.vkCmdCopyImageToBuffer
#define vkCmdDispatch g_vkd.vkCmdDispatch
#define vkCmdDraw g_vkd.vkCmdDraw
#define vkCmdDrawIndexed g_vkd.vkCmdDrawIndexed
#define vkCmdEndQuery g_vkd.vkCmdEndQuery
#define vkCmdEndRendering g_vkd.vkCmdEndRendering
#define vkCmdPipelineBarrier2 g_vkd.vkCmdPipelineBarrier2
#define vkCmdPushConstants g_vkd.vkCmdPushConstants
#define vkCmdResetQueryPool g_vkd.vkCmdResetQueryPool
#define vkCmdResolveImage g_vkd.vkCmdResolveImage
#define vkCmdSetBlendConstants g_vkd.vkCmdSetBlendConstants
#define vkCmdSetCullMode g_vkd.vkCmdSetCullMode
#define vkCmdSetDepthBias g_vkd.vkCmdSetDepthBias
#define vkCmdSetDepthBiasEnable g_vkd.vkCmdSetDepthBiasEnable
#define vkCmdSetDepthCompareOp g_vkd.vkCmdSetDepthCompareOp
#define vkCmdSetDepthTestEnable g_vkd.vkCmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable g_vkd.vkCmdSetDepthWriteEnable
#define vkCmdSetFrontFace g_vkd.vkCmdSetFrontFace
#define vkCmdSetPrimitiveRestartEnable g_vkd.vkCmdSetPrimitiveRestartEnable
#define vkCmdSetPrimitiveTopology g_vkd.vkCmdSetPrimitiveTopology
#define vkCmdSetScissor g_vkd.vkCmdSetScissor
#define vkCmdSetStencilCompareMask g_vkd.vkCmdSetStencilCompareMask
#define vkCmdSetStencilOp g_vkd.vkCmdSetStencilOp
#define vkCmdSetStencilReference g_vkd.vkCmdSetStencilReference
#define vkCmdSetStencilTestEnable g_vkd.vkCmdSetStencilTestEnable
#define vkCmdSetStencilWriteMask g_vkd.vkCmdSetStencilWriteMask
#define vkCmdSetViewport g_vkd.vkCmdSetViewport
#define vkCmdWriteTimestamp2 g_vkd.vkCmdWriteTimestamp2
#define vkCreateBuffer g_vkd.vkCreateBuffer
#define vkCreateCommandPool g_vkd.vkCreateCommandPool
#define vkCreateComputePipelines g_vkd.vkCreateComputePipelines
#define vkCreateDescriptorPool g_vkd.vkCreateDescriptorPool
#define vkCreateDescriptorSetLayout g_vkd.vkCreateDescriptorSetLayout
#define vkCreateDevice g_vkd.vkCreateDevice
#define vkCreateGraphicsPipelines g_vkd.vkCreateGraphicsPipelines
#define vkCreateImageView g_vkd.vkCreateImageView
#define vkCreateInstance g_vkd.vkCreateInstance
#define vkDestroyInstance g_vkd.vkDestroyInstance
#define vkEnumerateInstanceExtensionProperties g_vkd.vkEnumerateInstanceExtensionProperties
#define vkEnumerateInstanceLayerProperties g_vkd.vkEnumerateInstanceLayerProperties
#define vkCreatePipelineCache g_vkd.vkCreatePipelineCache
#define vkCreatePipelineLayout g_vkd.vkCreatePipelineLayout
#define vkCreateQueryPool g_vkd.vkCreateQueryPool
#define vkCreateSampler g_vkd.vkCreateSampler
#define vkCreateSemaphore g_vkd.vkCreateSemaphore
#define vkCreateShaderModule g_vkd.vkCreateShaderModule
#define vkCreateSwapchainKHR g_vkd.vkCreateSwapchainKHR
#define vkDestroyBuffer g_vkd.vkDestroyBuffer
#define vkDestroyImageView g_vkd.vkDestroyImageView
#define vkDestroySurfaceKHR g_vkd.vkDestroySurfaceKHR
#define vkDestroySwapchainKHR g_vkd.vkDestroySwapchainKHR
#define vkDeviceWaitIdle g_vkd.vkDeviceWaitIdle
#define vkEndCommandBuffer g_vkd.vkEndCommandBuffer
#define vkEnumerateDeviceExtensionProperties g_vkd.vkEnumerateDeviceExtensionProperties
#define vkEnumeratePhysicalDevices g_vkd.vkEnumeratePhysicalDevices
#define vkFreeMemory g_vkd.vkFreeMemory
#define vkGetDeviceProcAddr g_vkd.vkGetDeviceProcAddr
#define vkGetDeviceQueue g_vkd.vkGetDeviceQueue
#define vkGetInstanceProcAddr g_vkd.vkGetInstanceProcAddr
#define vkGetPhysicalDeviceFeatures2 g_vkd.vkGetPhysicalDeviceFeatures2
#define vkGetPhysicalDeviceFormatProperties g_vkd.vkGetPhysicalDeviceFormatProperties
#define vkGetPhysicalDeviceProperties g_vkd.vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties g_vkd.vkGetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_vkd.vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR g_vkd.vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkGetPhysicalDeviceSurfacePresentModesKHR g_vkd.vkGetPhysicalDeviceSurfacePresentModesKHR
#define vkGetPhysicalDeviceSurfaceSupportKHR g_vkd.vkGetPhysicalDeviceSurfaceSupportKHR
#define vkCreateAndroidSurfaceKHR g_vkd.vkCreateAndroidSurfaceKHR
#define vkGetPipelineCacheData g_vkd.vkGetPipelineCacheData
#define vkGetQueryPoolResults g_vkd.vkGetQueryPoolResults
#define vkGetSemaphoreCounterValue g_vkd.vkGetSemaphoreCounterValue
#define vkGetSwapchainImagesKHR g_vkd.vkGetSwapchainImagesKHR
#define vkQueuePresentKHR g_vkd.vkQueuePresentKHR
#define vkQueueSubmit2 g_vkd.vkQueueSubmit2
#define vkResetDescriptorPool g_vkd.vkResetDescriptorPool
#define vkUpdateDescriptorSets g_vkd.vkUpdateDescriptorSets
#define vkWaitSemaphores g_vkd.vkWaitSemaphores
#define vkBindImageMemory g_vkd.vkBindImageMemory
#define vkCmdDispatchIndirect g_vkd.vkCmdDispatchIndirect
#define vkCmdDrawIndirect g_vkd.vkCmdDrawIndirect
#define vkCmdExecuteCommands g_vkd.vkCmdExecuteCommands
#define vkCmdPipelineBarrier g_vkd.vkCmdPipelineBarrier
#define vkCmdSetDepthBounds g_vkd.vkCmdSetDepthBounds
#define vkCmdSetLineWidth g_vkd.vkCmdSetLineWidth
#define vkCmdWriteTimestamp g_vkd.vkCmdWriteTimestamp
#define vkCreateFence g_vkd.vkCreateFence
#define vkCreateImage g_vkd.vkCreateImage
#define vkDestroyCommandPool g_vkd.vkDestroyCommandPool
#define vkDestroyDescriptorPool g_vkd.vkDestroyDescriptorPool
#define vkDestroyDescriptorSetLayout g_vkd.vkDestroyDescriptorSetLayout
#define vkDestroyDevice g_vkd.vkDestroyDevice
#define vkDestroyFence g_vkd.vkDestroyFence
#define vkDestroyImage g_vkd.vkDestroyImage
#define vkDestroyPipeline g_vkd.vkDestroyPipeline
#define vkDestroyPipelineCache g_vkd.vkDestroyPipelineCache
#define vkDestroyPipelineLayout g_vkd.vkDestroyPipelineLayout
#define vkDestroyQueryPool g_vkd.vkDestroyQueryPool
#define vkDestroySampler g_vkd.vkDestroySampler
#define vkDestroySemaphore g_vkd.vkDestroySemaphore
#define vkDestroyShaderModule g_vkd.vkDestroyShaderModule
#define vkFreeCommandBuffers g_vkd.vkFreeCommandBuffers
#define vkFreeDescriptorSets g_vkd.vkFreeDescriptorSets
#define vkGetBufferMemoryRequirements g_vkd.vkGetBufferMemoryRequirements
#define vkGetImageMemoryRequirements g_vkd.vkGetImageMemoryRequirements
#define vkMapMemory g_vkd.vkMapMemory
#define vkQueueSubmit g_vkd.vkQueueSubmit
#define vkQueueWaitIdle g_vkd.vkQueueWaitIdle
#define vkResetCommandBuffer g_vkd.vkResetCommandBuffer
#define vkResetCommandPool g_vkd.vkResetCommandPool
#define vkResetFences g_vkd.vkResetFences
#define vkSignalSemaphore g_vkd.vkSignalSemaphore
#define vkUnmapMemory g_vkd.vkUnmapMemory
#define vkWaitForFences g_vkd.vkWaitForFences
