// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "VulkanFunctions.h"

#include <cstring>

#include "Log.h"

// Mesa's RADV entry points (scripts/build-mesa.sh of DolphinPS4, linked in whole).
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                              const char* name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device,
                                                                           const char* name);

#define EMUSUNI_VK_DEFINE(name) PFN_##name name = nullptr;
EMUSUNI_VK_GLOBAL_FUNCTIONS(EMUSUNI_VK_DEFINE)
EMUSUNI_VK_INSTANCE_FUNCTIONS(EMUSUNI_VK_DEFINE)
EMUSUNI_VK_DEVICE_FUNCTIONS(EMUSUNI_VK_DEFINE)
#undef EMUSUNI_VK_DEFINE

namespace VulkanFunctions
{
bool LoadGlobal()
{
  bool ok = true;
#define EMUSUNI_VK_LOAD(name)                                                                     \
  name = reinterpret_cast<PFN_##name>(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, #name));          \
  if (!name)                                                                                      \
  {                                                                                               \
    LogF("vulkan: %s missing", #name);                                                            \
    ok = false;                                                                                   \
  }
  EMUSUNI_VK_GLOBAL_FUNCTIONS(EMUSUNI_VK_LOAD)
#undef EMUSUNI_VK_LOAD
  return ok;
}

bool LoadInstance(VkInstance instance)
{
  bool ok = true;
#define EMUSUNI_VK_LOAD(name)                                                                     \
  name = reinterpret_cast<PFN_##name>(vk_icdGetInstanceProcAddr(instance, #name));                \
  if (!name)                                                                                      \
  {                                                                                               \
    LogF("vulkan: %s missing", #name);                                                            \
    ok = false;                                                                                   \
  }
  EMUSUNI_VK_INSTANCE_FUNCTIONS(EMUSUNI_VK_LOAD)
#undef EMUSUNI_VK_LOAD
  return ok;
}

bool LoadDevice(VkDevice device)
{
  bool ok = true;
#define EMUSUNI_VK_LOAD(name)                                                                     \
  name = reinterpret_cast<PFN_##name>(vk_common_GetDeviceProcAddr(device, #name));                \
  if (!name)                                                                                      \
  {                                                                                               \
    LogF("vulkan: %s missing", #name);                                                            \
    ok = false;                                                                                   \
  }
  EMUSUNI_VK_DEVICE_FUNCTIONS(EMUSUNI_VK_LOAD)
#undef EMUSUNI_VK_LOAD
  return ok;
}

PFN_vkVoidFunction Lookup(VkInstance instance, VkDevice device, const char* name)
{
  PFN_vkVoidFunction function = nullptr;
  if (device != VK_NULL_HANDLE)
    function = vk_common_GetDeviceProcAddr(device, name);
  if (!function)
    function = vk_icdGetInstanceProcAddr(instance, name);
  return function;
}

#ifdef EMUSUNI_3DS
static PFN_vkCreateDevice s_create_device_hook = nullptr;  // SetCreateDeviceHook
#endif

namespace
{
// A loader's vkEnumerateInstanceVersion, should RADV's ICD entry point not offer it.
VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceVersion(uint32_t* version)
{
  *version = VK_API_VERSION_1_1;
  return VK_SUCCESS;
}
}  // namespace

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name)
{
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(GetInstanceProcAddr);
#ifdef EMUSUNI_3DS
  if (s_create_device_hook && std::strcmp(name, "vkCreateDevice") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(s_create_device_hook);
#endif
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
  PFN_vkVoidFunction function = vk_icdGetInstanceProcAddr(instance, name);
  if (!function && std::strcmp(name, "vkEnumerateInstanceVersion") == 0)
    function = reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceVersion);
  return function;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name)
{
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
  return vk_common_GetDeviceProcAddr(device, name);
}

#ifdef EMUSUNI_3DS
void SetCreateDeviceHook(PFN_vkCreateDevice hook)
{
  s_create_device_hook = hook;
}
#endif
}  // namespace VulkanFunctions
