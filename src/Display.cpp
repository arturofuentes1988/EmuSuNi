// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4XMBDisplay.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "Display.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "GpuFilter.h"
#include "LibretroVulkan.h"
#include "Log.h"
#include "VulkanFunctions.h"

#include <imgui_impl_vulkan.h>

std::atomic<unsigned> g_display_frames{0};

// Mesa's GNM winsys (DolphinPS4 port/mesa/ac_ps4_drm.c): queues a flip behind the work submitted
// so far.
extern "C" int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);

// VideoOut's count of vertical blanks since sceVideoOutOpen (libSceVideoOut exports it; OpenOrbis
// declares it without parameters). The layout is the one shadPS4 and fpPS4 both found (count,
// process time, TSC, reserved, flags: 40 bytes); the buffer is bigger in case the real one is.
struct EmusuniVblankStatus
{
  uint64_t count, process_time, tsc, reserved;
  uint8_t flags, pad[63];
};
extern "C" int emusuni_get_vblank_status(int handle, EmusuniVblankStatus* status) __asm__("sceVideoOutGetVblankStatus");

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
  // A core's image as this frame draws it (sharp, smooth): one descriptor set each, rewritten
  // when this frame is recorded again.
  VkDescriptorSet external[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  // The core's image read with alpha 1 (OpaqueView), made for this frame.
  VkImageView opaque = VK_NULL_HANDLE;
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

// A core that draws with Vulkan: the device is ParaLLEl-RDP's, the queue shared under a lock; the
// semaphores the next frame waits for and signals.
bool s_shared = false;
std::mutex s_queue_mutex;
std::vector<VkSemaphore> s_wait_semaphores;
std::vector<VkPipelineStageFlags> s_wait_stages;
VkSemaphore s_signal_semaphore = VK_NULL_HANDLE;

int s_video = -1;
int s_buffer_set = -1;
// Time spent waiting (for the TV, for the GPU) since TakeWaitMs: the speed line's drawing time
// minus this is the work.
double s_wait_ms = 0.0;
// The TV's refresh rate (ReadRefreshRate).
double s_refresh_hz = 59.94;

double MsSince(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

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

// The TV's refresh rate from VideoOut (sceVideoOutGetResolutionStatus): refreshRate is the PS4
// SDK's SceVideoOutRefreshRate (shadPS4, fpPS4): 1 = 23.98 Hz, 2 = 50, 3 = 59.94, 6 = 29.97,
// 13 = 119.88, 35 = 89.91; 59.94 when it doesn't say. Logged at start and when it changes.
void ReadRefreshRate(const char* why)
{
  OrbisVideoOutResolutionStatus mode = {};
  const int result = sceVideoOutGetResolutionStatus(s_video, &mode);
  double hz = 59.94;
  if (result == 0 && mode.refreshRate == 2)
    hz = 50.0;
  else if (result == 0 && mode.refreshRate == 13)
    hz = 119.88;
  if (why || hz != s_refresh_hz)
  {
    LogF("display: VideoOut %ux%u, refresh rate %llu: %.2f Hz (%#x)%s%s", mode.width, mode.height,
         static_cast<unsigned long long>(mode.refreshRate), hz, static_cast<unsigned>(result), why ? ", " : "",
         why ? why : "");
  }
  s_refresh_hz = hz;
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

// ImGui's uses of the queue (texture uploads) under the queue's lock too, like every other.
VKAPI_ATTR VkResult VKAPI_CALL LockedQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits,
                                                 VkFence fence)
{
  std::lock_guard lock(s_queue_mutex);
  return vkQueueSubmit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL LockedQueueWaitIdle(VkQueue queue)
{
  std::lock_guard lock(s_queue_mutex);
  return vkQueueWaitIdle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL LockedDeviceWaitIdle(VkDevice device)
{
  std::lock_guard lock(s_queue_mutex);
  return vkDeviceWaitIdle(device);
}

PFN_vkVoidFunction LoadFunction(const char* name, void*)
{
  if (std::strcmp(name, "vkQueueSubmit") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(LockedQueueSubmit);
  if (std::strcmp(name, "vkQueueWaitIdle") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(LockedQueueWaitIdle);
  if (std::strcmp(name, "vkDeviceWaitIdle") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(LockedDeviceWaitIdle);
  PFN_vkVoidFunction function = VulkanFunctions::Lookup(s_instance, s_device, name);
  if (!function)
  {
    LogF("display: Vulkan function not available (stubbed): %s", name);
    function = UnusedVulkanFunction;
  }
  return function;
}

#ifdef EMUSUNI_3DS
// ---- The Nintendo 3DS core (Azahar) asks the device for more than the other cores do ----
// A few extensions with their features (no geometry shaders: the first geometry-shader draw hung the
// PS4's GPU in 00.62; Azahar no longer uses them there, scripts/build-3ds-core.sh). Granite (ParaLLEl-RDP)
// enables the timeline semaphores, the pipeline creation cache control and
// the image format list by itself when the GPU has them; extended dynamic state, custom border colours,
// 8-bit indices and the fragment shader interlock and barycentrics it knows nothing of: ExtraCreateDevice
// adds them to what Granite asks vkCreateDevice for (VulkanFunctions::SetCreateDeviceHook). The 3DS core is
// told (EMUSUNI_VK_DEVICE_FEATURES and _EXTENSIONS, PublishDeviceLists) only what the device really has, and
// the other cores what they always were.
struct Extras
{
  VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamic_state = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
  VkPhysicalDeviceCustomBorderColorFeaturesEXT border_color = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
  VkPhysicalDeviceIndexTypeUint8FeaturesEXT index_uint8 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
  VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
  VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
  std::vector<const char*> extensions;  // the ones ExtraCreateDevice adds
  // The names the 3DS core is told, besides the other cores': what Granite enables by itself, and what
  // ExtraCreateDevice adds (only told when the device was made with them).
  std::string granite_features, granite_extensions, hook_features, hook_extensions;
  bool added = false;  // the device was made with ExtraCreateDevice's additions
};
Extras s_extras;
std::string s_features_base, s_extensions_base, s_features_3ds, s_extensions_3ds;

void AddName(std::string* list, const char* name)
{
  *list += std::string(list->empty() ? "" : ",") + name;
}

// What the GPU can give the 3DS core: from its extensions and the features each reports.
void ProbeExtras(const std::vector<VkExtensionProperties>& all, [[maybe_unused]] const VkPhysicalDeviceFeatures& supported)
{
  auto has = [&all](const char* name) {
    return std::any_of(all.begin(), all.end(),
                       [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
  };
  s_extras = Extras();
  const auto get_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
      VulkanFunctions::Lookup(s_instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2"));
  if (has(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME))
    AddName(&s_extras.granite_extensions, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
  if (!get_features)
  {
    Log("3DS: no vkGetPhysicalDeviceFeatures2: no extra features asked for");
    return;
  }
  // The features of each extension the GPU lists, as the GPU reports them.
  VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
  VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT cache = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT};
  Extras probe;  // (its structs: the same chain types as the ones the device will be made with)
  const bool want_timeline = has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
  const bool want_cache = has(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
  const bool want_dynamic = has(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
  const bool want_border = has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
  const bool want_uint8 = has(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
  const bool want_interlock = has(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
  const bool want_barycentric = has(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
  void** next = &query.pNext;
  auto chain = [&next](auto& feature) {
    *next = &feature;
    next = &feature.pNext;
  };
  if (want_timeline)
    chain(timeline);
  if (want_cache)
    chain(cache);
  if (want_dynamic)
    chain(probe.dynamic_state);
  if (want_border)
    chain(probe.border_color);
  if (want_uint8)
    chain(probe.index_uint8);
  if (want_interlock)
    chain(probe.interlock);
  if (want_barycentric)
    chain(probe.barycentric);
  get_features(s_gpu, &query);
  if (want_timeline && timeline.timelineSemaphore)
  {
    AddName(&s_extras.granite_features, "timelineSemaphore");
    AddName(&s_extras.granite_extensions, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
  }
  if (want_cache && cache.pipelineCreationCacheControl)
  {
    AddName(&s_extras.granite_features, "pipelineCreationCacheControl");
    AddName(&s_extras.granite_extensions, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
  }
  if (want_dynamic && probe.dynamic_state.extendedDynamicState)
  {
    s_extras.dynamic_state.extendedDynamicState = VK_TRUE;
    s_extras.extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    AddName(&s_extras.hook_features, "extendedDynamicState");
    AddName(&s_extras.hook_extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
  }
  if (want_border && probe.border_color.customBorderColors)
  {
    s_extras.border_color.customBorderColors = VK_TRUE;
    s_extras.border_color.customBorderColorWithoutFormat = probe.border_color.customBorderColorWithoutFormat;
    s_extras.extensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    AddName(&s_extras.hook_features, "customBorderColors");
    if (probe.border_color.customBorderColorWithoutFormat)
      AddName(&s_extras.hook_features, "customBorderColorWithoutFormat");
    AddName(&s_extras.hook_extensions, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
  }
  if (want_uint8 && probe.index_uint8.indexTypeUint8)
  {
    s_extras.index_uint8.indexTypeUint8 = VK_TRUE;
    s_extras.extensions.push_back(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    AddName(&s_extras.hook_features, "indexTypeUint8");
    AddName(&s_extras.hook_extensions, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
  }
  if (want_interlock && probe.interlock.fragmentShaderPixelInterlock)
  {
    s_extras.interlock.fragmentShaderPixelInterlock = VK_TRUE;
    s_extras.extensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    AddName(&s_extras.hook_features, "fragmentShaderPixelInterlock");
    AddName(&s_extras.hook_extensions, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
  }
  if (want_barycentric && probe.barycentric.fragmentShaderBarycentric)
  {
    s_extras.barycentric.fragmentShaderBarycentric = VK_TRUE;
    s_extras.extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    AddName(&s_extras.hook_features, "fragmentShaderBarycentric");
    AddName(&s_extras.hook_extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
  }
}

// vkCreateDevice as Granite calls it: its device plus the 3DS core's extensions and their features. A device
// the driver refuses with them is made again without (the other cores must still be able to draw).
VKAPI_ATTR VkResult VKAPI_CALL ExtraCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkDevice* device)
{
  std::vector<const char*> names(info->ppEnabledExtensionNames,
                                 info->ppEnabledExtensionNames + info->enabledExtensionCount);
  for (const char* extra : s_extras.extensions)
  {
    if (std::none_of(names.begin(), names.end(), [extra](const char* name) { return std::strcmp(name, extra) == 0; }))
      names.push_back(extra);
  }
  VkDeviceCreateInfo created = *info;
  created.enabledExtensionCount = static_cast<uint32_t>(names.size());
  created.ppEnabledExtensionNames = names.data();
  // The feature structs first in the chain, then Granite's (VkPhysicalDeviceFeatures2 and its own).
  void* head = const_cast<void*>(info->pNext);
  auto chain = [&head](auto& feature) {
    feature.pNext = head;
    head = &feature;
  };
  if (s_extras.dynamic_state.extendedDynamicState)
    chain(s_extras.dynamic_state);
  if (s_extras.border_color.customBorderColors)
    chain(s_extras.border_color);
  if (s_extras.index_uint8.indexTypeUint8)
    chain(s_extras.index_uint8);
  if (s_extras.interlock.fragmentShaderPixelInterlock)
    chain(s_extras.interlock);
  if (s_extras.barycentric.fragmentShaderBarycentric)
    chain(s_extras.barycentric);
  created.pNext = head;
  const VkResult result = vkCreateDevice(gpu, &created, allocator, device);
  if (result == VK_SUCCESS)
  {
    s_extras.added = true;
    return result;
  }
  LogF("display: the device couldn't be made with the 3DS's extensions (%d): made without them",
       static_cast<int>(result));
  return vkCreateDevice(gpu, info, allocator, device);
}

// The lists the 3DS core is told: the other cores' and what the device has for it besides.
void Build3dsLists(const std::string& features, const std::string& extensions)
{
  s_features_base = features;
  s_extensions_base = extensions;
  s_features_3ds = features;
  s_extensions_3ds = extensions;
  for (const std::string& part : {s_extras.granite_features, s_extras.added ? s_extras.hook_features : std::string()})
  {
    if (!part.empty())
      s_features_3ds += (s_features_3ds.empty() ? "" : ",") + part;
  }
  for (const std::string& part :
       {s_extras.granite_extensions, s_extras.added ? s_extras.hook_extensions : std::string()})
  {
    if (!part.empty())
      s_extensions_3ds += (s_extensions_3ds.empty() ? "" : ",") + part;
  }
  LogF("display: told to the Nintendo 3DS core: features %s; extensions %s%s", s_features_3ds.c_str(),
       s_extensions_3ds.c_str(), s_extras.extensions.empty() ? "" : (s_extras.added ? " (added to the device)" : " (NOT added)"));
}
#endif

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
    LogF("display: queue family %u: %u queues, flags %#x", i, family_props[i].queueCount,
         static_cast<unsigned>(family_props[i].queueFlags));
  }
  for (uint32_t i = 0; i < families; i++)
  {
    if (family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
    {
      s_queue_family = i;
      break;
    }
  }
#ifdef EMUSUNI_N64
  // ParaLLEl-RDP (N64 games) makes the device, with the features it needs (8 and 16-bit storage,
  // subgroups...); the menus and every game draw on it. Its Vulkan context stays for the N64
  // games (scripts/build-n64-core.sh).
  {
    Log("creating the Vulkan device through ParaLLEl-RDP");
    retro_vulkan_context context = {};
    // Granite reads the features (a null pointer would crash). Besides its own: what the PlayStation
    // core (SwanStation: blending in one pass, sample shading) and the PSP core (PPSSPP: also logic
    // ops, depth clamp, clip and cull distances) use, when the GPU has them. The cores are told
    // every feature the device has (EMUSUNI_VK_DEVICE_FEATURES, scripts/build-ps1-core.sh and
    // build-psp-core.sh): ours and those of Granite's own list it enabled.
    VkPhysicalDeviceFeatures supported = {}, wanted = {};
    vkGetPhysicalDeviceFeatures(s_gpu, &supported);
    wanted.dualSrcBlend = supported.dualSrcBlend;
    wanted.sampleRateShading = supported.sampleRateShading;
    wanted.logicOp = supported.logicOp;
    wanted.depthClamp = supported.depthClamp;
    wanted.shaderClipDistance = supported.shaderClipDistance;
    wanted.shaderCullDistance = supported.shaderCullDistance;
    // PPSSPP's multisampling (render passes 2 and their depth resolve) and stencil from shaders,
    // when the GPU has them (Granite fails on a missing one). Granite doesn't enable them itself.
    std::vector<const char*> extensions;
    {
      uint32_t count = 0;
      vkEnumerateDeviceExtensionProperties(s_gpu, nullptr, &count, nullptr);
      std::vector<VkExtensionProperties> all(count);
      vkEnumerateDeviceExtensionProperties(s_gpu, nullptr, &count, all.data());
      auto has = [&all](const char* name) {
        return std::any_of(all.begin(), all.end(),
                           [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
      };
      if (has(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))
      {
        extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        if (has(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
          extensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
      }
      if (has(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME))
        extensions.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
#ifdef EMUSUNI_3DS
      ProbeExtras(all, supported);
#endif
    }
#ifdef EMUSUNI_3DS
    // Granite makes the device through vkCreateDevice as GetInstanceProcAddr hands it out: with the 3DS core's
    // extensions added (ExtraCreateDevice).
    VulkanFunctions::SetCreateDeviceHook(s_extras.extensions.empty() ? nullptr : ExtraCreateDevice);
#endif
    const bool made = parallel_create_device(&context, s_instance, s_gpu, VK_NULL_HANDLE,
                                             VulkanFunctions::GetInstanceProcAddr, extensions.data(),
                                             static_cast<unsigned>(extensions.size()), nullptr, 0, &wanted);
#ifdef EMUSUNI_3DS
    VulkanFunctions::SetCreateDeviceHook(nullptr);
#endif
    if (made && context.device != VK_NULL_HANDLE)
    {
      s_device = context.device;
      s_queue = context.queue;
      s_queue_family = context.queue_family_index;
      s_shared = true;
      // The features by name: ours, then Granite's (parallel-rdp/vulkan/context.cpp) the GPU has.
#define EMUSUNI_FEATURE(name) {#name, &VkPhysicalDeviceFeatures::name}
      static const std::pair<const char*, VkBool32 VkPhysicalDeviceFeatures::*> OURS[] = {
          EMUSUNI_FEATURE(sampleRateShading), EMUSUNI_FEATURE(dualSrcBlend), EMUSUNI_FEATURE(logicOp),
          EMUSUNI_FEATURE(depthClamp), EMUSUNI_FEATURE(shaderClipDistance), EMUSUNI_FEATURE(shaderCullDistance)};
      static const std::pair<const char*, VkBool32 VkPhysicalDeviceFeatures::*> GRANITE[] = {
          EMUSUNI_FEATURE(textureCompressionETC2), EMUSUNI_FEATURE(textureCompressionBC),
          EMUSUNI_FEATURE(textureCompressionASTC_LDR), EMUSUNI_FEATURE(fullDrawIndexUint32),
          EMUSUNI_FEATURE(imageCubeArray), EMUSUNI_FEATURE(fillModeNonSolid), EMUSUNI_FEATURE(independentBlend),
          EMUSUNI_FEATURE(fragmentStoresAndAtomics), EMUSUNI_FEATURE(shaderStorageImageExtendedFormats),
          EMUSUNI_FEATURE(shaderStorageImageMultisample), EMUSUNI_FEATURE(largePoints),
          EMUSUNI_FEATURE(shaderInt16), EMUSUNI_FEATURE(shaderInt64),
          EMUSUNI_FEATURE(shaderStorageImageWriteWithoutFormat),
          EMUSUNI_FEATURE(shaderStorageImageReadWithoutFormat),
          EMUSUNI_FEATURE(shaderSampledImageArrayDynamicIndexing),
          EMUSUNI_FEATURE(shaderUniformBufferArrayDynamicIndexing),
          EMUSUNI_FEATURE(shaderStorageBufferArrayDynamicIndexing),
          EMUSUNI_FEATURE(shaderStorageImageArrayDynamicIndexing), EMUSUNI_FEATURE(shaderImageGatherExtended),
          EMUSUNI_FEATURE(samplerAnisotropy)};
#undef EMUSUNI_FEATURE
      std::string features, ours;
      auto add = [](std::string* list, const char* name) { *list += std::string(list->empty() ? "" : ",") + name; };
      for (const auto& [name, member] : OURS)
      {
        if (wanted.*member)
        {
          add(&features, name);
          add(&ours, name);
        }
      }
      for (const auto& [name, member] : GRANITE)
      {
        if (supported.*member)
          add(&features, name);
      }
      std::string extension_list;
      for (const char* name : extensions)
        add(&extension_list, name);
      setenv("EMUSUNI_VK_DEVICE_FEATURES", features.c_str(), 1);
      setenv("EMUSUNI_VK_DEVICE_EXTENSIONS", extension_list.c_str(), 1);
#ifdef EMUSUNI_3DS
      Build3dsLists(features, extension_list);
#endif
      LogF("display: device made by ParaLLEl-RDP (queue family %u), with %s; extensions %s", s_queue_family,
           ours.empty() ? "no extra features" : ours.c_str(), extension_list.empty() ? "none" : extension_list.c_str());
    }
    else
    {
      Log("ParaLLEl-RDP couldn't make the device: N64 games can't be drawn");
    }
  }
  if (!s_shared)
#endif
  {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = s_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    Log("creating the Vulkan device");
    if (!Check(vkCreateDevice(s_gpu, &device_info, nullptr, &s_device), "vkCreateDevice"))
      return false;
  }
  if (!VulkanFunctions::LoadDevice(s_device))
    return false;
  if (!s_shared)
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
  ReadRefreshRate("at start");

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
  {
    std::lock_guard lock(s_queue_mutex);
    vkQueueSubmit(s_queue, 1, &submit, s_upload_fence);
  }
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

// Until the GPU has drawn every frame submitted.
void WaitAllFrames()
{
  for (Frame& frame : s_frames)
  {
    if (frame.fence != VK_NULL_HANDLE)
      vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  }
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

bool Display::SharesVulkan() const
{
  return s_shared;
}

#ifdef EMUSUNI_3DS
void Display::PublishDeviceLists(bool for_3ds)
{
  if (!s_shared)
    return;  // (no device of ParaLLEl-RDP's: nothing was published)
  setenv("EMUSUNI_VK_DEVICE_FEATURES", (for_3ds ? s_features_3ds : s_features_base).c_str(), 1);
  setenv("EMUSUNI_VK_DEVICE_EXTENSIONS", (for_3ds ? s_extensions_3ds : s_extensions_base).c_str(), 1);
}
#endif

VkInstance Display::Instance() const
{
  return s_instance;
}

VkPhysicalDevice Display::Gpu() const
{
  return s_gpu;
}

VkDevice Display::Device() const
{
  return s_device;
}

VkQueue Display::Queue() const
{
  return s_queue;
}

uint32_t Display::QueueFamily() const
{
  return s_queue_family;
}

void Display::LockQueue()
{
  s_queue_mutex.lock();
}

void Display::UnlockQueue()
{
  s_queue_mutex.unlock();
}

uint32_t Display::SyncIndex() const
{
  return (s_current + 1) % IMAGES;
}

uint32_t Display::SyncIndexMask() const
{
  return (1u << IMAGES) - 1;
}

void Display::WaitSyncIndex()
{
  Frame& frame = s_frames[(s_current + 1) % IMAGES];
  vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
}

void Display::WaitForSemaphores(uint32_t count, const VkSemaphore* semaphores)
{
  for (uint32_t i = 0; i < count; i++)
  {
    s_wait_semaphores.push_back(semaphores[i]);
    s_wait_stages.push_back(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  }
}

void Display::SignalSemaphore(VkSemaphore semaphore)
{
  s_signal_semaphore = semaphore;
}

ImTextureID Display::ExternalTexture(VkImageView view, VkImageLayout layout, bool smooth)
{
  if (view == VK_NULL_HANDLE)
    return ImTextureID{};
  // The set of the frame EndFrame records next: the GPU is done with its last use.
  Frame& frame = s_frames[(s_current + 1) % IMAGES];
  const auto wait = std::chrono::steady_clock::now();
  vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  s_wait_ms += MsSince(wait);
  VkSampler sampler = smooth ? s_sampler : s_sampler_nearest;
  VkDescriptorSet& set = frame.external[smooth ? 1 : 0];
  if (set == VK_NULL_HANDLE)
  {
    set = ImGui_ImplVulkan_AddTexture(sampler, view, layout);
  }
  else
  {
    VkDescriptorImageInfo image = {sampler, view, layout};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(s_device, 1, &write, 0, nullptr);
  }
  return reinterpret_cast<ImTextureID>(set);
}

VkImageView Display::OpaqueView(const VkImageViewCreateInfo& info)
{
  if (info.image == VK_NULL_HANDLE)
    return VK_NULL_HANDLE;
  // The frame EndFrame records next: the GPU is done with the view it had.
  Frame& frame = s_frames[(s_current + 1) % IMAGES];
  const auto wait = std::chrono::steady_clock::now();
  vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  s_wait_ms += MsSince(wait);
  if (frame.opaque != VK_NULL_HANDLE)
    vkDestroyImageView(s_device, frame.opaque, nullptr);
  frame.opaque = VK_NULL_HANDLE;
  VkImageViewCreateInfo opaque = info;
  opaque.pNext = nullptr;
  opaque.components.a = VK_COMPONENT_SWIZZLE_ONE;
  if (vkCreateImageView(s_device, &opaque, nullptr, &frame.opaque) != VK_SUCCESS)
    frame.opaque = VK_NULL_HANDLE;
  return frame.opaque;
}

void Display::ForgetOpaqueViews()
{
  WaitAllFrames();
  for (Frame& frame : s_frames)
  {
    if (frame.opaque != VK_NULL_HANDLE)
      vkDestroyImageView(s_device, frame.opaque, nullptr);
    frame.opaque = VK_NULL_HANDLE;
  }
}

void Display::WaitIdle()
{
  WaitAllFrames();
}

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
  // The GPU filters (Graphics > GPU Upscaler); without them pictures are drawn as before.
  GpuFilter::Context filters;
  filters.device = s_device;
  filters.gpu = s_gpu;
  filters.screen = s_render_pass;
  filters.width = WIDTH;
  filters.height = HEIGHT;
  filters.wait_idle = WaitAllFrames;
  filters.cache_path = std::string(App::DATA_DIR) + "/cache/gpu-filters.bin";
  filters.queue_family = s_queue_family;
  if (!GpuFilter::Init(filters))
    Log("GPU filters not available");
  Log("ready");
  return true;
}

void Display::Shutdown()
{
  if (s_device != VK_NULL_HANDLE)
  {
    std::lock_guard lock(s_queue_mutex);
    vkDeviceWaitIdle(s_device);
  }
  GpuFilter::Shutdown();
  for (Frame& frame : s_frames)
  {
    for (VkDescriptorSet& set : frame.external)
    {
      if (set != VK_NULL_HANDLE)
        ImGui_ImplVulkan_RemoveTexture(set);
      set = VK_NULL_HANDLE;
    }
  }
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

double Display::TakeWaitMs()
{
  const double ms = s_wait_ms;
  s_wait_ms = 0.0;
  return ms;
}

double Display::RefreshHz() const
{
  return s_refresh_hz;
}

Display::FlipState Display::GetFlipState() const
{
  FlipState state;
  if (s_video < 0)
    return state;
  OrbisVideoOutFlipStatus flip = {};
  if (sceVideoOutGetFlipStatus(s_video, &flip) >= 0)
  {
    state.shown = flip.flipArg;
    state.pending = static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(s_flips) - flip.flipArg, 0,
                                                         static_cast<int64_t>(IMAGES) - 1));
  }
  EmusuniVblankStatus vblank = {};
  if (emusuni_get_vblank_status(s_video, &vblank) == 0)
    state.vblanks = static_cast<int64_t>(vblank.count);
  return state;
}

int64_t Display::NextFlip() const
{
  return static_cast<int64_t>(s_flips) + 1;
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

VkImageView Display::GameView() const
{
  return s_game_view;
}

void Display::EndFrame()
{
  s_current = (s_current + 1) % IMAGES;
  const auto wait = std::chrono::steady_clock::now();
  WaitForImageOffScreen();
  Frame& frame = s_frames[s_current];
  vkWaitForFences(s_device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
  s_wait_ms += MsSince(wait);
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
  // The GPU filters' passes into their images (the last one draws inside the render pass).
  GpuFilter::Record(frame.commands, s_current);
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
  // A core's image: after the semaphores it gave (none for ParaLLEl-RDP, which draws on this
  // queue before).
  submit.waitSemaphoreCount = static_cast<uint32_t>(s_wait_semaphores.size());
  submit.pWaitSemaphores = s_wait_semaphores.empty() ? nullptr : s_wait_semaphores.data();
  submit.pWaitDstStageMask = s_wait_stages.empty() ? nullptr : s_wait_stages.data();
  if (s_signal_semaphore != VK_NULL_HANDLE)
  {
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &s_signal_semaphore;
  }
  const int64_t arg = static_cast<int64_t>(s_flips + 1);
  int result;
  {
    std::lock_guard lock(s_queue_mutex);
    vkQueueSubmit(s_queue, 1, &submit, frame.fence);
    result = ac_ps4_submit_flip(s_video, s_current, static_cast<unsigned>(s_flip_mode), arg);
  }
  s_wait_semaphores.clear();
  s_wait_stages.clear();
  s_signal_semaphore = VK_NULL_HANDLE;
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
    {
      std::lock_guard lock(s_queue_mutex);
      vkDeviceWaitIdle(s_device);
    }
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
