// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4XMBDisplay.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "Display.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "Log.h"
#include "VulkanFunctions.h"

#include <imgui_impl_vulkan.h>

std::atomic<unsigned> g_display_frames{0};

// Mesa's GNM winsys (DolphinPS4 port/mesa/ac_ps4_drm.c): queues a flip behind the work submitted
// so far.
extern "C" int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);

namespace
{
constexpr uint32_t IMAGES = 3;
constexpr VkDeviceSize FRAMEBUFFER_ALIGN = 2 << 20;  // VideoOut buffer alignment
constexpr VkFormat FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat GAME_FORMAT = VK_FORMAT_R5G6B5_UNORM_PACK16;  // Snes9x's RGB565
constexpr VkDeviceSize GAME_STAGING_SIZE =
    static_cast<VkDeviceSize>(Display::GAME_TEXTURE_W) * Display::GAME_TEXTURE_H * 2;

struct Frame
{
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  // The game picture to copy into the game texture before this frame draws (UploadGameFrame).
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  void* staging_data = nullptr;
  bool upload = false;
  uint32_t upload_w = 0, upload_h = 0, upload_row = 0;
  // The image's pixels as the CPU sees them (linear, host-visible), for screenshots.
  const uint8_t* pixels = nullptr;
  size_t row_bytes = 0;
};

struct Texture
{
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;  // only outside the arena
  VkImageView view = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDeviceSize offset = 0, size = 0;  // in the arena
  bool arena = false;
};

VkInstance s_instance = VK_NULL_HANDLE;
VkPhysicalDevice s_gpu = VK_NULL_HANDLE;
VkDevice s_device = VK_NULL_HANDLE;
uint32_t s_queue_family = 0;
VkQueue s_queue = VK_NULL_HANDLE;
VkCommandPool s_pool = VK_NULL_HANDLE;
VkRenderPass s_render_pass = VK_NULL_HANDLE;
VkSampler s_sampler = VK_NULL_HANDLE;          // linear (menus, smooth game picture)
VkSampler s_sampler_nearest = VK_NULL_HANDLE;  // sharp game picture
std::array<Frame, IMAGES> s_frames;
std::vector<Texture> s_textures;
uint32_t s_current = 0;
uint64_t s_flips = 0;

// The game texture and its two descriptor sets (smooth / sharp).
VkImage s_game_image = VK_NULL_HANDLE;
VkDeviceMemory s_game_memory = VK_NULL_HANDLE;
VkImageView s_game_view = VK_NULL_HANDLE;
VkDescriptorSet s_game_smooth = VK_NULL_HANDLE, s_game_sharp = VK_NULL_HANDLE;

// On the PS4 every allocation and release of GPU memory maps and unmaps memory, and each of those
// costs some of libkernel's small internal memory pool: creating and destroying textures (covers
// while scrolling, ImGui's font uploads) ran it out within minutes ("Internal Memory is running
// out"), and the next thread the app made killed it. So the menus' images live in one block of
// GPU memory allocated at start (the arena), uploaded through one staging buffer with one command
// buffer, all kept for the life of the app.
constexpr VkDeviceSize ARENA_SIZE = 192ull << 20;
constexpr VkDeviceSize UPLOAD_SIZE = 8ull << 20;
struct ArenaRange
{
  VkDeviceSize offset, size;
};
VkDeviceMemory s_arena = VK_NULL_HANDLE;
uint32_t s_arena_type = UINT32_MAX;
std::vector<ArenaRange> s_arena_free;  // sorted by offset
VkBuffer s_upload = VK_NULL_HANDLE;
VkDeviceMemory s_upload_memory = VK_NULL_HANDLE;
uint8_t* s_upload_map = nullptr;
VkCommandBuffer s_upload_commands = VK_NULL_HANDLE;
VkFence s_upload_fence = VK_NULL_HANDLE;

int s_video = -1;
int s_buffer_set = -1;
#ifndef ORBIS_VIDEO_OUT_TILING_MODE_LINEAR
#define ORBIS_VIDEO_OUT_TILING_MODE_LINEAR 1  // older OpenOrbis headers lack it
#endif
// VideoOut flip modes: 1 waits for the vertical blank, 2 (HSYNC) for the next line.
constexpr int FLIP_HSYNC = 2;
int s_flip_mode = ORBIS_VIDEO_OUT_FLIP_VSYNC;
OrbisKernelEqueue s_flip_queue;

void Log(const char* text)
{
  LogF("display: %s", text);
}

bool Check(VkResult result, const char* what)
{
  if (result == VK_SUCCESS)
    return true;
  char line[160];
  snprintf(line, sizeof(line), "%s failed: %d", what, static_cast<int>(result));
  Log(line);
  return false;
}

uint32_t MemoryType(uint32_t type_bits, VkMemoryPropertyFlags want)
{
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(s_gpu, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; i++)
  {
    if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
      return i;
  }
  return UINT32_MAX;
}

// Stands in for functions ImGui loads but this renderer never calls (its window/swap chain
// helpers need VK_KHR_surface/swapchain, which aren't enabled here).
VKAPI_ATTR void VKAPI_CALL UnusedVulkanFunction()
{
  Log("unused Vulkan function called");
}

PFN_vkVoidFunction LoadFunction(const char* name, void*)
{
  PFN_vkVoidFunction function = VulkanFunctions::Lookup(s_instance, s_device, name);
  if (!function)
  {
    LogF("display: Vulkan function not available (stubbed): %s", name);
    function = UnusedVulkanFunction;
  }
  return function;
}

bool CreateDevice()
{
  if (!VulkanFunctions::LoadGlobal())
    return false;
  Log("creating the Vulkan instance");
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "EmuSuNi";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  if (!Check(vkCreateInstance(&instance_info, nullptr, &s_instance), "vkCreateInstance") ||
      !VulkanFunctions::LoadInstance(s_instance))
  {
    return false;
  }
  uint32_t count = 1;
  if (vkEnumeratePhysicalDevices(s_instance, &count, &s_gpu) < 0 || count == 0)
  {
    Log("no Vulkan device");
    return false;
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(s_gpu, &props);
  LogF("display: GPU %s", props.deviceName);
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &families, nullptr);
  std::vector<VkQueueFamilyProperties> family_props(families);
  vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &families, family_props.data());
  for (uint32_t i = 0; i < families; i++)
  {
    if (family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
    {
      s_queue_family = i;
      break;
    }
  }
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = s_queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  Log("creating the Vulkan device");
  if (!Check(vkCreateDevice(s_gpu, &device_info, nullptr, &s_device), "vkCreateDevice") ||
      !VulkanFunctions::LoadDevice(s_device))
  {
    return false;
  }
  vkGetDeviceQueue(s_device, s_queue_family, 0, &s_queue);
  Log("device created");

  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = s_queue_family;
  return Check(vkCreateCommandPool(s_device, &pool_info, nullptr, &s_pool), "vkCreateCommandPool");
}

bool CreateRenderPass()
{
  VkAttachmentDescription color = {};
  color.format = FORMAT;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // scanned out by VideoOut (linear)
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &ref;
  VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &color;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  return Check(vkCreateRenderPass(s_device, &info, nullptr, &s_render_pass), "vkCreateRenderPass");
}

bool CreateStaging(Frame& frame)
{
  VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer.size = GAME_STAGING_SIZE;
  buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (!Check(vkCreateBuffer(s_device, &buffer, nullptr, &frame.staging), "vkCreateBuffer (staging)"))
    return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(s_device, frame.staging, &req);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!Check(vkAllocateMemory(s_device, &alloc, nullptr, &frame.staging_memory),
             "vkAllocateMemory (staging)"))
  {
    return false;
  }
  vkBindBufferMemory(s_device, frame.staging, frame.staging_memory, 0);
  return Check(vkMapMemory(s_device, frame.staging_memory, 0, VK_WHOLE_SIZE, 0, &frame.staging_data),
               "vkMapMemory (staging)");
}

bool CreateFrames()
{
  s_video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
  if (s_video < 0)
  {
    Log("sceVideoOutOpen failed");
    return false;
  }
  sceKernelCreateEqueue(&s_flip_queue, "emusuni flip");
  sceVideoOutAddFlipEvent(s_flip_queue, s_video, nullptr);

  std::array<void*, IMAGES> addresses{};
  uint32_t pitch = Display::WIDTH;
  for (uint32_t i = 0; i < IMAGES; i++)
  {
    Frame& frame = s_frames[i];
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = FORMAT;
    info.extent = {Display::WIDTH, Display::HEIGHT, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_LINEAR;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(s_device, &info, nullptr, &frame.image), "vkCreateImage (frame)"))
      return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s_device, frame.image, &req);
    // VideoOut wants 2 MiB aligned buffers: over-allocate, bind at an aligned offset.
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size + FRAMEBUFFER_ALIGN;
    alloc.memoryTypeIndex =
        MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    uint8_t* base = nullptr;
    if (!Check(vkAllocateMemory(s_device, &alloc, nullptr, &frame.memory), "vkAllocateMemory") ||
        !Check(vkMapMemory(s_device, frame.memory, 0, VK_WHOLE_SIZE, 0,
                           reinterpret_cast<void**>(&base)),
               "vkMapMemory"))
    {
      return false;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    VkDeviceSize offset = ((address + FRAMEBUFFER_ALIGN - 1) & ~(FRAMEBUFFER_ALIGN - 1)) - address;
    offset = (offset + req.alignment - 1) & ~(req.alignment - 1);
    vkBindImageMemory(s_device, frame.image, frame.memory, offset);
    VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(s_device, frame.image, &subresource, &layout);
    addresses[i] = base + offset + layout.offset;
    pitch = static_cast<uint32_t>(layout.rowPitch / 4);
    frame.pixels = base + offset + layout.offset;
    frame.row_bytes = static_cast<size_t>(layout.rowPitch);

    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = frame.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = FORMAT;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!Check(vkCreateImageView(s_device, &view, nullptr, &frame.view), "vkCreateImageView"))
      return false;
    VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = s_render_pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &frame.view;
    fb.width = Display::WIDTH;
    fb.height = Display::HEIGHT;
    fb.layers = 1;
    if (!Check(vkCreateFramebuffer(s_device, &fb, nullptr, &frame.framebuffer),
               "vkCreateFramebuffer"))
    {
      return false;
    }
    VkCommandBufferAllocateInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = s_pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!Check(vkAllocateCommandBuffers(s_device, &cb, &frame.commands), "command buffer") ||
        !Check(vkCreateFence(s_device, &fence, nullptr, &frame.fence), "vkCreateFence") ||
        !CreateStaging(frame))
    {
      return false;
    }
  }

  OrbisVideoOutBufferAttribute attribute;
  sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, Display::WIDTH,
                                Display::HEIGHT, pitch);
  s_buffer_set = sceVideoOutRegisterBuffers(s_video, 0, addresses.data(), IMAGES, &attribute);
  if (s_buffer_set < 0)
  {
    Log("sceVideoOutRegisterBuffers failed");
    return false;
  }
  return true;
}

// The image rendered next must be off screen: with 3 in rotation it was shown by flip n-2 and
// is replaced on screen by flip n-1 (n = flips submitted).
void WaitForImageOffScreen()
{
  if (s_flips < 2)
    return;
  const int64_t needed = static_cast<int64_t>(s_flips - 1);
  for (int tries = 0; tries < 40; tries++)
  {
    OrbisVideoOutFlipStatus status = {};
    if (sceVideoOutGetFlipStatus(s_video, &status) < 0 || status.flipArg >= needed)
      return;
    OrbisKernelEvent event;
    int count = 0;
    OrbisKernelUseconds timeout = 5000;
    sceKernelWaitEqueue(s_flip_queue, &event, 1, &count, &timeout);
  }
}

// The upload command buffer (one, reused: a new one each time would allocate and free its
// command memory).
VkCommandBuffer BeginOneTimeCommands()
{
  if (s_upload_commands == VK_NULL_HANDLE)
  {
    VkCommandBufferAllocateInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = s_pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    vkAllocateCommandBuffers(s_device, &cb, &s_upload_commands);
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(s_device, &fence, nullptr, &s_upload_fence);
  }
  vkResetCommandBuffer(s_upload_commands, 0);
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(s_upload_commands, &begin);
  return s_upload_commands;
}

void EndOneTimeCommands(VkCommandBuffer commands)
{
  vkEndCommandBuffer(commands);
  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commands;
  vkQueueSubmit(s_queue, 1, &submit, s_upload_fence);
  vkWaitForFences(s_device, 1, &s_upload_fence, VK_TRUE, UINT64_MAX);
  vkResetFences(s_device, 1, &s_upload_fence);
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize align)
{
  return align > 1 ? (value + align - 1) / align * align : value;
}

// The arena and the staging buffer (Display::Initialize).
bool CreateArena()
{
  // The memory type of a sampled RGBA8 image, from a probe image.
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = FORMAT;
  info.extent = {64, 64, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage probe = VK_NULL_HANDLE;
  if (!Check(vkCreateImage(s_device, &info, nullptr, &probe), "vkCreateImage (probe)"))
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(s_device, probe, &req);
  vkDestroyImage(s_device, probe, nullptr);
  s_arena_type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = ARENA_SIZE;
  alloc.memoryTypeIndex = s_arena_type;
  if (!Check(vkAllocateMemory(s_device, &alloc, nullptr, &s_arena), "vkAllocateMemory (arena)"))
    return false;
  s_arena_free = {{0, ARENA_SIZE}};

  VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer.size = UPLOAD_SIZE;
  buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (!Check(vkCreateBuffer(s_device, &buffer, nullptr, &s_upload), "vkCreateBuffer (upload)"))
    return false;
  vkGetBufferMemoryRequirements(s_device, s_upload, &req);
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!Check(vkAllocateMemory(s_device, &alloc, nullptr, &s_upload_memory), "vkAllocateMemory (upload)"))
    return false;
  vkBindBufferMemory(s_device, s_upload, s_upload_memory, 0);
  if (!Check(vkMapMemory(s_device, s_upload_memory, 0, VK_WHOLE_SIZE, 0,
                         reinterpret_cast<void**>(&s_upload_map)),
             "vkMapMemory (upload)"))
    return false;
  LogF("display: texture arena %llu MiB, upload buffer %llu MiB",
       static_cast<unsigned long long>(ARENA_SIZE >> 20), static_cast<unsigned long long>(UPLOAD_SIZE >> 20));
  return true;
}

// First fit in the free list; UINT64_MAX when nothing fits.
VkDeviceSize ArenaAlloc(VkDeviceSize size, VkDeviceSize align)
{
  for (size_t i = 0; i < s_arena_free.size(); i++)
  {
    const ArenaRange range = s_arena_free[i];
    const VkDeviceSize start = AlignUp(range.offset, align);
    if (start + size > range.offset + range.size)
      continue;
    std::vector<ArenaRange> pieces;
    if (start > range.offset)
      pieces.push_back({range.offset, start - range.offset});
    if (start + size < range.offset + range.size)
      pieces.push_back({start + size, range.offset + range.size - start - size});
    s_arena_free.erase(s_arena_free.begin() + static_cast<long>(i));
    s_arena_free.insert(s_arena_free.begin() + static_cast<long>(i), pieces.begin(), pieces.end());
    return start;
  }
  return UINT64_MAX;
}

void ArenaFree(VkDeviceSize offset, VkDeviceSize size)
{
  auto at = std::lower_bound(s_arena_free.begin(), s_arena_free.end(), offset,
                             [](const ArenaRange& r, VkDeviceSize o) { return r.offset < o; });
  at = s_arena_free.insert(at, {offset, size});
  // Merge with the neighbours.
  if (at + 1 != s_arena_free.end() && at->offset + at->size == (at + 1)->offset)
  {
    at->size += (at + 1)->size;
    s_arena_free.erase(at + 1);
  }
  if (at != s_arena_free.begin() && (at - 1)->offset + (at - 1)->size == at->offset)
  {
    (at - 1)->size += at->size;
    s_arena_free.erase(at);
  }
}

void Barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
             VkPipelineStageFlags dst_stage)
{
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(commands, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool CreateGameTexture()
{
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = GAME_FORMAT;
  info.extent = {Display::GAME_TEXTURE_W, Display::GAME_TEXTURE_H, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!Check(vkCreateImage(s_device, &info, nullptr, &s_game_image), "vkCreateImage (game)"))
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(s_device, s_game_image, &req);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!Check(vkAllocateMemory(s_device, &alloc, nullptr, &s_game_memory), "vkAllocateMemory (game)"))
    return false;
  vkBindImageMemory(s_device, s_game_image, s_game_memory, 0);

  // Black until the first frame arrives.
  VkCommandBuffer commands = BeginOneTimeCommands();
  Barrier(commands, s_game_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkClearColorValue black = {};
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(commands, s_game_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                       &range);
  Barrier(commands, s_game_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  EndOneTimeCommands(commands);

  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = s_game_image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = GAME_FORMAT;
  view.subresourceRange = range;
  if (!Check(vkCreateImageView(s_device, &view, nullptr, &s_game_view), "vkCreateImageView (game)"))
    return false;
  s_game_smooth =
      ImGui_ImplVulkan_AddTexture(s_sampler, s_game_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  s_game_sharp = ImGui_ImplVulkan_AddTexture(s_sampler_nearest, s_game_view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return s_game_smooth != VK_NULL_HANDLE && s_game_sharp != VK_NULL_HANDLE;
}

VkSampler CreateSampler(VkFilter filter)
{
  VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = filter;
  sampler.minFilter = filter;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = 1.0f;
  VkSampler result = VK_NULL_HANDLE;
  Check(vkCreateSampler(s_device, &sampler, nullptr, &result), "vkCreateSampler");
  return result;
}
}  // namespace

bool Display::Initialize()
{
  if (!CreateDevice() || !CreateRenderPass())
    return false;
  Log("creating the VideoOut buffers");
  if (!CreateFrames())
    return false;
  Log("VideoOut buffers registered");
  s_sampler = CreateSampler(VK_FILTER_LINEAR);
  s_sampler_nearest = CreateSampler(VK_FILTER_NEAREST);
  if (s_sampler == VK_NULL_HANDLE || s_sampler_nearest == VK_NULL_HANDLE)
    return false;

  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, LoadFunction))
  {
    Log("ImGui_ImplVulkan_LoadFunctions failed");
    return false;
  }
  ImGui_ImplVulkan_InitInfo info = {};
  info.ApiVersion = VK_API_VERSION_1_1;
  info.Instance = s_instance;
  info.PhysicalDevice = s_gpu;
  info.Device = s_device;
  info.QueueFamily = s_queue_family;
  info.Queue = s_queue;
  info.DescriptorPoolSize = 512;  // the font atlas, covers (cached), wallpapers, the game
  info.RenderPass = s_render_pass;
  info.MinImageCount = IMAGES;
  info.ImageCount = IMAGES;
  info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&info))
  {
    Log("ImGui_ImplVulkan_Init failed");
    return false;
  }
  Log("ImGui renderer ready");
  if (!CreateArena() || !CreateGameTexture())
    return false;
  Log("ready");
  return true;
}

void Display::Shutdown()
{
  if (s_device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(s_device);
  for (Texture& texture : s_textures)
  {
    if (texture.set != VK_NULL_HANDLE)
      ImGui_ImplVulkan_RemoveTexture(texture.set);
    vkDestroyImageView(s_device, texture.view, nullptr);
    vkDestroyImage(s_device, texture.image, nullptr);
    if (!texture.arena)
      vkFreeMemory(s_device, texture.memory, nullptr);
  }
  s_textures.clear();
  vkFreeMemory(s_device, s_arena, nullptr);
  vkDestroyBuffer(s_device, s_upload, nullptr);
  vkFreeMemory(s_device, s_upload_memory, nullptr);
  vkDestroyFence(s_device, s_upload_fence, nullptr);
  if (s_game_smooth != VK_NULL_HANDLE)
    ImGui_ImplVulkan_RemoveTexture(s_game_smooth);
  if (s_game_sharp != VK_NULL_HANDLE)
    ImGui_ImplVulkan_RemoveTexture(s_game_sharp);
  vkDestroyImageView(s_device, s_game_view, nullptr);
  vkDestroyImage(s_device, s_game_image, nullptr);
  vkFreeMemory(s_device, s_game_memory, nullptr);
  ImGui_ImplVulkan_Shutdown();
  if (s_buffer_set >= 0)
    sceVideoOutUnregisterBuffers(s_video, s_buffer_set);
  s_buffer_set = -1;
  for (Frame& frame : s_frames)
  {
    vkDestroyFence(s_device, frame.fence, nullptr);
    vkDestroyFramebuffer(s_device, frame.framebuffer, nullptr);
    vkDestroyImageView(s_device, frame.view, nullptr);
    vkDestroyImage(s_device, frame.image, nullptr);
    vkFreeMemory(s_device, frame.memory, nullptr);
    vkDestroyBuffer(s_device, frame.staging, nullptr);
    vkFreeMemory(s_device, frame.staging_memory, nullptr);
    frame = {};
  }
  vkDestroySampler(s_device, s_sampler, nullptr);
  vkDestroySampler(s_device, s_sampler_nearest, nullptr);
  vkDestroyRenderPass(s_device, s_render_pass, nullptr);
  vkDestroyCommandPool(s_device, s_pool, nullptr);
  vkDestroyDevice(s_device, nullptr);
  vkDestroyInstance(s_instance, nullptr);
  s_device = VK_NULL_HANDLE;
  s_instance = VK_NULL_HANDLE;
  if (s_video >= 0)
    sceVideoOutClose(s_video);
  s_video = -1;
}

void Display::BeginFrame()
{
  ImGui_ImplVulkan_NewFrame();
}

void Display::UploadGameFrame(const void* pixels, uint32_t width, uint32_t height, size_t pitch)
{
  if (!pixels || width == 0 || height == 0)
    return;
  width = std::min(width, GAME_TEXTURE_W);
  height = std::min(height, GAME_TEXTURE_H);
  const uint32_t row = static_cast<uint32_t>(pitch / 2);  // pixels per source row
  if (row < width || static_cast<VkDeviceSize>(row) * height * 2 > GAME_STAGING_SIZE)
    return;
  // The frame EndFrame renders next; its staging buffer may still be read by the GPU.
  Frame& frame = s_frames[(s_current + 1) % IMAGES];
  vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  std::memcpy(frame.staging_data, pixels, static_cast<size_t>(row) * height * 2);
  frame.upload = true;
  frame.upload_w = width;
  frame.upload_h = height;
  frame.upload_row = row;
}

void Display::SetVsync(bool on)
{
  s_flip_mode = on ? ORBIS_VIDEO_OUT_FLIP_VSYNC : FLIP_HSYNC;
}

bool Display::Capture(std::vector<uint8_t>* rgba)
{
  const Frame& frame = s_frames[s_current];
  if (!frame.pixels)
    return false;
  vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  rgba->resize(static_cast<size_t>(WIDTH) * HEIGHT * 4);
  for (uint32_t y = 0; y < HEIGHT; y++)
  {
    uint8_t* to = rgba->data() + static_cast<size_t>(y) * WIDTH * 4;
    std::memcpy(to, frame.pixels + static_cast<size_t>(y) * frame.row_bytes, static_cast<size_t>(WIDTH) * 4);
    for (uint32_t x = 0; x < WIDTH; x++)
      to[x * 4 + 3] = 255;  // ImGui's blending leaves the alpha anywhere
  }
  return true;
}

ImTextureID Display::GameTexture(bool smooth) const
{
  return reinterpret_cast<ImTextureID>(smooth ? s_game_smooth : s_game_sharp);
}

void Display::EndFrame()
{
  s_current = (s_current + 1) % IMAGES;
  WaitForImageOffScreen();
  Frame& frame = s_frames[s_current];
  vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  vkResetFences(s_device, 1, &frame.fence);

  vkResetCommandBuffer(frame.commands, 0);
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame.commands, &begin);
  if (frame.upload)
  {
    // The game picture of this frame into the game texture (after the earlier frames' draws,
    // which ran before on the same queue).
    frame.upload = false;
    Barrier(frame.commands, s_game_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {};
    copy.bufferRowLength = frame.upload_row;
    copy.bufferImageHeight = frame.upload_h;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {frame.upload_w, frame.upload_h, 1};
    vkCmdCopyBufferToImage(frame.commands, frame.staging, s_game_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(frame.commands, s_game_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  }
  VkClearValue clear = {};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = s_render_pass;
  pass.framebuffer = frame.framebuffer;
  pass.renderArea.extent = {WIDTH, HEIGHT};
  pass.clearValueCount = 1;
  pass.pClearValues = &clear;
  vkCmdBeginRenderPass(frame.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commands);
  vkCmdEndRenderPass(frame.commands);
  vkEndCommandBuffer(frame.commands);

  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame.commands;
  vkQueueSubmit(s_queue, 1, &submit, frame.fence);

  const int64_t arg = static_cast<int64_t>(s_flips + 1);
  int result = ac_ps4_submit_flip(s_video, s_current, static_cast<unsigned>(s_flip_mode), arg);
  if (result != 0)
  {
    vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    result = sceVideoOutSubmitFlip(s_video, static_cast<int>(s_current), s_flip_mode, arg);
  }
  if (result >= 0)
    s_flips++;
  if (s_flips <= 3)
    LogF("display: frame %llu shown (flip %#x)", static_cast<unsigned long long>(s_flips),
         static_cast<unsigned>(result));
  g_display_frames.fetch_add(1, std::memory_order_relaxed);
}

ImTextureID Display::CreateTexture(const void* rgba, uint32_t width, uint32_t height)
{
  if (!rgba || width == 0 || height == 0 || static_cast<VkDeviceSize>(width) * 4 > UPLOAD_SIZE)
    return ImTextureID{};
  Texture texture;
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = FORMAT;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!Check(vkCreateImage(s_device, &info, nullptr, &texture.image), "vkCreateImage (texture)"))
    return ImTextureID{};
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(s_device, texture.image, &req);
  const VkDeviceSize offset = s_arena != VK_NULL_HANDLE && (req.memoryTypeBits & (1u << s_arena_type)) ?
                                  ArenaAlloc(req.size, req.alignment) :
                                  UINT64_MAX;
  if (offset != UINT64_MAX)
  {
    texture.arena = true;
    texture.offset = offset;
    texture.size = req.size;
    vkBindImageMemory(s_device, texture.image, s_arena, offset);
  }
  else
  {
    // The arena is full: memory of its own (costs a mapping, logged).
    static unsigned logged;
    if (logged++ < 20)
      LogF("display: texture %ux%u outside the arena", width, height);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!Check(vkAllocateMemory(s_device, &alloc, nullptr, &texture.memory), "vkAllocateMemory (texture)"))
    {
      vkDestroyImage(s_device, texture.image, nullptr);
      return ImTextureID{};
    }
    vkBindImageMemory(s_device, texture.image, texture.memory, 0);
  }

  // The pixels through the staging buffer, in bands of rows that fit it.
  const VkDeviceSize row_bytes = static_cast<VkDeviceSize>(width) * 4;
  const uint32_t band = static_cast<uint32_t>(std::max<VkDeviceSize>(1, UPLOAD_SIZE / row_bytes));
  for (uint32_t y = 0; y < height; y += band)
  {
    const uint32_t rows = std::min(band, height - y);
    std::memcpy(s_upload_map, static_cast<const uint8_t*>(rgba) + y * row_bytes, rows * row_bytes);
    VkCommandBuffer commands = BeginOneTimeCommands();
    if (y == 0)
    {
      Barrier(commands, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {0, static_cast<int32_t>(y), 0};
    copy.imageExtent = {width, rows, 1};
    vkCmdCopyBufferToImage(commands, s_upload, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    if (y + rows >= height)
    {
      Barrier(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    EndOneTimeCommands(commands);
  }

  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = texture.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = FORMAT;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(s_device, &view, nullptr, &texture.view);
  texture.set = ImGui_ImplVulkan_AddTexture(s_sampler, texture.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  s_textures.push_back(texture);
  return reinterpret_cast<ImTextureID>(texture.set);
}

void Display::DestroyTexture(ImTextureID id)
{
  for (auto it = s_textures.begin(); it != s_textures.end(); ++it)
  {
    if (reinterpret_cast<ImTextureID>(it->set) != id)
      continue;
    vkDeviceWaitIdle(s_device);
    ImGui_ImplVulkan_RemoveTexture(it->set);
    vkDestroyImageView(s_device, it->view, nullptr);
    vkDestroyImage(s_device, it->image, nullptr);
    if (it->arena)
      ArenaFree(it->offset, it->size);
    else
      vkFreeMemory(s_device, it->memory, nullptr);
    s_textures.erase(it);
    return;
  }
}
