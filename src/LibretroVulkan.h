// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// libretro's Vulkan rendering interface (libretro-common's libretro_vulkan.h, MIT license, the
// part a frontend needs): a core that draws with Vulkan (the N64 core's ParaLLEl-RDP) gets the
// frontend's device through it and hands over each frame as an image.

#pragma once

#include <cstdint>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <libretro.h>

#define RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION 5

struct retro_vulkan_image
{
  VkImageView image_view;
  VkImageLayout image_layout;
  VkImageViewCreateInfo create_info;
};

typedef void (*retro_vulkan_set_image_t)(void* handle, const struct retro_vulkan_image* image,
                                         uint32_t num_semaphores, const VkSemaphore* semaphores,
                                         uint32_t src_queue_family);
typedef uint32_t (*retro_vulkan_get_sync_index_t)(void* handle);
typedef uint32_t (*retro_vulkan_get_sync_index_mask_t)(void* handle);
typedef void (*retro_vulkan_set_command_buffers_t)(void* handle, uint32_t num_cmd, const VkCommandBuffer* cmd);
typedef void (*retro_vulkan_wait_sync_index_t)(void* handle);
typedef void (*retro_vulkan_lock_queue_t)(void* handle);
typedef void (*retro_vulkan_unlock_queue_t)(void* handle);
typedef void (*retro_vulkan_set_signal_semaphore_t)(void* handle, VkSemaphore semaphore);

struct retro_vulkan_context
{
  VkPhysicalDevice gpu;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family_index;
  VkQueue presentation_queue;
  uint32_t presentation_queue_family_index;
};

struct retro_hw_render_interface_vulkan
{
  enum retro_hw_render_interface_type interface_type;
  unsigned interface_version;
  void* handle;
  VkInstance instance;
  VkPhysicalDevice gpu;
  VkDevice device;
  PFN_vkGetDeviceProcAddr get_device_proc_addr;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
  VkQueue queue;
  unsigned queue_index;
  retro_vulkan_set_image_t set_image;
  retro_vulkan_get_sync_index_t get_sync_index;
  retro_vulkan_get_sync_index_mask_t get_sync_index_mask;
  retro_vulkan_set_command_buffers_t set_command_buffers;
  retro_vulkan_wait_sync_index_t wait_sync_index;
  retro_vulkan_lock_queue_t lock_queue;
  retro_vulkan_unlock_queue_t unlock_queue;
  retro_vulkan_set_signal_semaphore_t set_signal_semaphore;
};

#ifdef EMUSUNI_N64
// ParaLLEl-RDP's device creation (the N64 core's context negotiation, kept global by
// scripts/build-n64-core.sh): the display makes its device with it, so the core can draw on it.
extern "C" bool parallel_create_device(struct retro_vulkan_context* context, VkInstance instance,
                                       VkPhysicalDevice gpu, VkSurfaceKHR surface,
                                       PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                       const char** required_device_extensions,
                                       unsigned num_required_device_extensions,
                                       const char** required_device_layers, unsigned num_required_device_layers,
                                       const VkPhysicalDeviceFeatures* required_features);
#endif
