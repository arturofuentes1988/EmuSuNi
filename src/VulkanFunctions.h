// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Vulkan functions the display uses, looked up in Mesa's RADV, which is linked into the
// executable (the PS4 has no Vulkan loader): global and instance functions through RADV's ICD entry
// point, device functions through Mesa's common vkGetDeviceProcAddr (as DolphinPS4 does).

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define EMUSUNI_VK_GLOBAL_FUNCTIONS(X) X(vkCreateInstance)

#define EMUSUNI_VK_INSTANCE_FUNCTIONS(X)                                                          \
  X(vkDestroyInstance)                                                                            \
  X(vkEnumeratePhysicalDevices)                                                                   \
  X(vkGetPhysicalDeviceQueueFamilyProperties)                                                     \
  X(vkGetPhysicalDeviceMemoryProperties)                                                          \
  X(vkGetPhysicalDeviceProperties)                                                                \
  X(vkGetPhysicalDeviceFormatProperties)                                                          \
  X(vkGetPhysicalDeviceFeatures)                                                                  \
  X(vkEnumerateDeviceExtensionProperties)                                                         \
  X(vkCreateDevice)

#define EMUSUNI_VK_DEVICE_FUNCTIONS(X)                                                            \
  X(vkDestroyDevice)                                                                              \
  X(vkGetDeviceQueue)                                                                             \
  X(vkDeviceWaitIdle)                                                                             \
  X(vkCreateCommandPool)                                                                          \
  X(vkDestroyCommandPool)                                                                         \
  X(vkAllocateCommandBuffers)                                                                     \
  X(vkFreeCommandBuffers)                                                                         \
  X(vkBeginCommandBuffer)                                                                         \
  X(vkEndCommandBuffer)                                                                           \
  X(vkResetCommandBuffer)                                                                         \
  X(vkQueueSubmit)                                                                                \
  X(vkQueueWaitIdle)                                                                              \
  X(vkCreateFence)                                                                                \
  X(vkDestroyFence)                                                                               \
  X(vkWaitForFences)                                                                              \
  X(vkResetFences)                                                                                \
  X(vkCreateRenderPass)                                                                           \
  X(vkDestroyRenderPass)                                                                          \
  X(vkCreateFramebuffer)                                                                          \
  X(vkDestroyFramebuffer)                                                                         \
  X(vkCreateImage)                                                                                \
  X(vkDestroyImage)                                                                               \
  X(vkGetImageMemoryRequirements)                                                                 \
  X(vkGetImageSubresourceLayout)                                                                  \
  X(vkBindImageMemory)                                                                            \
  X(vkCreateImageView)                                                                            \
  X(vkDestroyImageView)                                                                           \
  X(vkCreateBuffer)                                                                               \
  X(vkDestroyBuffer)                                                                              \
  X(vkGetBufferMemoryRequirements)                                                                \
  X(vkBindBufferMemory)                                                                           \
  X(vkAllocateMemory)                                                                             \
  X(vkFreeMemory)                                                                                 \
  X(vkMapMemory)                                                                                  \
  X(vkUnmapMemory)                                                                                \
  X(vkCreateSampler)                                                                              \
  X(vkDestroySampler)                                                                             \
  X(vkCmdBeginRenderPass)                                                                         \
  X(vkCmdEndRenderPass)                                                                           \
  X(vkCmdPipelineBarrier)                                                                         \
  X(vkCmdCopyBufferToImage)                                                                       \
  X(vkCmdClearColorImage)                                                                         \
  X(vkUpdateDescriptorSets)                                                                       \
  X(vkCreateShaderModule)                                                                         \
  X(vkDestroyShaderModule)                                                                        \
  X(vkCreatePipelineLayout)                                                                       \
  X(vkDestroyPipelineLayout)                                                                      \
  X(vkCreateGraphicsPipelines)                                                                    \
  X(vkDestroyPipeline)                                                                            \
  X(vkCreatePipelineCache)                                                                        \
  X(vkGetPipelineCacheData)                                                                       \
  X(vkDestroyPipelineCache)                                                                       \
  X(vkCreateDescriptorSetLayout)                                                                  \
  X(vkDestroyDescriptorSetLayout)                                                                 \
  X(vkCreateDescriptorPool)                                                                       \
  X(vkDestroyDescriptorPool)                                                                      \
  X(vkAllocateDescriptorSets)                                                                     \
  X(vkCmdBindPipeline)                                                                            \
  X(vkCmdBindDescriptorSets)                                                                      \
  X(vkCmdPushConstants)                                                                           \
  X(vkCmdSetViewport)                                                                             \
  X(vkCmdSetScissor)                                                                              \
  X(vkCmdDraw)                                                                                    \
  X(vkCreateQueryPool)                                                                            \
  X(vkDestroyQueryPool)                                                                           \
  X(vkCmdResetQueryPool)                                                                          \
  X(vkCmdWriteTimestamp)                                                                          \
  X(vkGetQueryPoolResults)

#define EMUSUNI_VK_DECLARE(name) extern PFN_##name name;
EMUSUNI_VK_GLOBAL_FUNCTIONS(EMUSUNI_VK_DECLARE)
EMUSUNI_VK_INSTANCE_FUNCTIONS(EMUSUNI_VK_DECLARE)
EMUSUNI_VK_DEVICE_FUNCTIONS(EMUSUNI_VK_DECLARE)
#undef EMUSUNI_VK_DECLARE

namespace VulkanFunctions
{
// Each returns false (and logs the name) when a function is missing.
bool LoadGlobal();
bool LoadInstance(VkInstance instance);
bool LoadDevice(VkDevice device);
// Any function, for ImGui's Vulkan renderer.
PFN_vkVoidFunction Lookup(VkInstance instance, VkDevice device, const char* name);
// vkGetInstanceProcAddr and vkGetDeviceProcAddr as a Vulkan loader would offer them, for a core
// that draws with Vulkan (libretro's interface).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name);
}  // namespace VulkanFunctions
