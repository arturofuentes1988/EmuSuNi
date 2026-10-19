#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds SwanStation (libretro/swanstation, ~/src/swanstation, GPL-3.0: the DuckStation-based PS1
# core) for the PS4 into one object that scripts/build.sh links into EmuSuNi: the PlayStation. Its
# own CMake makes the core with the PS4 toolchain (toolchain/ps4-love-modern.cmake): the x86-64
# recompiler, the Vulkan renderer (internal resolution upscaling, PGXP; its shaders are SPIR-V
# built into it, nothing is compiled from GLSL at run time), the software renderer, CHD/CUE/PBP/
# M3U/ISO/ECM/MDS images read by the core itself (stdio) and OpenBIOS (built in: used when no
# BIOS file is found). Its libretro API becomes swanstation_retro_* and everything else in it
# private, as for FCEUmm (CMakeLists.txt). The PS4 changes to its sources (applied each time to
# the files as git has them, checked; all under EMUSUNI_PORT, which the CMake option of that name
# defines):
#   - The Vulkan renderer adopts the frontend's device: EmuSuNi makes its only VkDevice at start
#     (src/Display.cpp, through ParaLLEl-RDP's Granite) and never calls the context negotiation's
#     create_device, so the renderer takes the instance, GPU, device and queue of
#     GET_HW_RENDER_INTERFACE. The device's enabled features (it may use dualSrcBlend and
#     sampleRateShading) come from EMUSUNI_VK_DEVICE_FEATURES (comma-separated
#     VkPhysicalDeviceFeatures names), else what Granite enables of them: sampleRateShading when the
#     GPU has it, not dualSrcBlend.
#   - Every vkQueueSubmit and vkDeviceWaitIdle of the core under the frontend's lock_queue /
#     unlock_queue (the queue is the menus' one too); upstream locked only its end-of-frame submit,
#     not the ones of its VRAM read-backs.
#   - The recompiler's code buffer (an array in the core's data, within 2 GiB of its code: 16 MiB
#     of code and 8 of slow paths, as upstream's 32-bit ARM build, not 32 + 16: the app's data holds
#     the N64 core's buffers too; a full buffer is flushed) is made executable through EmuSuNi
#     (port/ps4_n64.c: emusuni_make_executable, the ways the PS4 allows), 16 KiB aligned (the PS4's
#     pages: nothing else shares them), without guard pages. If that fails the core runs the cached
#     interpreter instead of crashing.
#   - No fastmem: it relies on page faults (the app's fault handler is its crash log) and, in its
#     mmap form, on shared memory objects, which the PS4 doesn't have the normal way. The RAM comes
#     from the heap; the recompiler calls the memory handlers.
#   - No dlopen of a Vulkan library (the frontend's vkGetInstanceProcAddr is all it needs).
#   - Built as a static library, without LTO (ld -r of LLVM bitcode); the files are read with
#     plain reads (libretro-common without mmap: no mapping of disc images on the USB drive).
# The object is checked at the end: only swanstation_retro_* defined globally, the C++ operator
# new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and none defined in
# it, and every other symbol it needs found in what the app links (OpenOrbis' libc, libkernel,
# libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-ps1-core.sh <out dir> [clean]
#   PS1_CORE_HOST=1 build-ps1-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                 the PC tests: no PS4 checks; the test frontend
#                                                 provides emusuni_make_executable
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${SWANSTATION_DIR:-$HOME/src/swanstation}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/ps1-core.log"
HOST="${PS1_CORE_HOST:-0}"

python3 -I - "$SRC" <<'PATCH'
import subprocess
import sys
root = sys.argv[1]
patched = {}
def patch(path, old, new):
    # On the file as git has it (the core's commit), so a changed patch replaces the old one.
    if path not in patched:
        blob = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + path], check=True,
                              capture_output=True).stdout
        patched[path] = blob.decode('utf-8').replace('\r\n', '\n')
    text = patched[path]
    assert text.count(old) == 1, (path, text.count(old), old[:60])
    patched[path] = text.replace(old, new)

# --- The build: the EMUSUNI_PORT option, no LTO, a static core, no mmap fastmem.
patch('CMakeLists.txt',
      'if(CMAKE_BUILD_TYPE MATCHES "Release")\n  cmake_policy(SET CMP0069 NEW)\n',
      '# EmuSuNi (scripts/build-ps1-core.sh): the core as one object of the PS4 app (static, no LTO:\n'
      '# ld -r would get LLVM bitcode), with the changes under EMUSUNI_PORT.\n'
      'option(EMUSUNI_PORT "Build the core for EmuSuNi (PS4)" OFF)\n'
      'if(EMUSUNI_PORT)\n  add_definitions("-DEMUSUNI_PORT=1")\nendif()\n\n'
      'if(CMAKE_BUILD_TYPE MATCHES "Release" AND NOT EMUSUNI_PORT)\n  cmake_policy(SET CMP0069 NEW)\n')
patch('src/libretro/CMakeLists.txt',
      'add_library(swanstation_libretro SHARED\n',
      'if(EMUSUNI_PORT)\n  set(SWANSTATION_LIBRETRO_TYPE STATIC)\nelse()\n  set(SWANSTATION_LIBRETRO_TYPE SHARED)\nendif()\n'
      'add_library(swanstation_libretro ${SWANSTATION_LIBRETRO_TYPE}\n')
patch('src/core/CMakeLists.txt',
      '  target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/xbyak/xbyak")\n'
      '  target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1" "WITH_MMAP_FASTMEM=1")\n',
      '  target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/xbyak/xbyak")\n'
      '  if(EMUSUNI_PORT)\n'
      '    # EmuSuNi: no mmap fastmem (shared memory views and page faults).\n'
      '    target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1")\n'
      '  else()\n'
      '    target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1" "WITH_MMAP_FASTMEM=1")\n'
      '  endif()\n')

# --- No fastmem: the recompiler calls the memory handlers. The RAM from the heap.
patch('src/core/settings.cpp',
      '  cpu_fastmem_rewrite = si.GetBoolValue("CPU", "FastmemRewrite", false);\n',
      '  cpu_fastmem_rewrite = si.GetBoolValue("CPU", "FastmemRewrite", false);\n'
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: no fastmem (it needs page faults; the PS4 app keeps those for its crash log).\n'
      '  cpu_fastmem_mode = CPUFastmemMode::Disabled;\n'
      '#endif\n')
patch('src/common/memory_arena.cpp',
      '#elif defined(__APPLE__) || defined(__FreeBSD__)\n#if defined(__APPLE__)\n  m_shmem_fd = shm_open(',
      '#elif defined(EMUSUNI_PORT)\n'
      '  // EmuSuNi: no shared memory objects (and nothing that needs them: no mmap fastmem).\n'
      '  return false;\n'
      '#elif defined(__APPLE__) || defined(__FreeBSD__)\n#if defined(__APPLE__)\n  m_shmem_fd = shm_open(')
patch('src/core/bus.cpp',
      'bool AllocateMemory(bool enable_8mb_ram)\n{\n'
      '  if (!m_memory_arena.Create(MEMORY_ARENA_SIZE, true, false))\n    return false;\n\n'
      '  // Create the base views.\n'
      '  const uint32_t ram_size = enable_8mb_ram ? RAM_8MB_SIZE : RAM_2MB_SIZE;\n'
      '  const uint32_t ram_mask = enable_8mb_ram ? RAM_8MB_MASK : RAM_2MB_MASK;\n'
      '  g_ram = static_cast<uint8_t*>(m_memory_arena.CreateViewPtr(MEMORY_ARENA_RAM_OFFSET, ram_size, true, false));\n',
      'bool AllocateMemory(bool enable_8mb_ram)\n{\n'
      '  const uint32_t ram_size = enable_8mb_ram ? RAM_8MB_SIZE : RAM_2MB_SIZE;\n'
      '  const uint32_t ram_mask = enable_8mb_ram ? RAM_8MB_MASK : RAM_2MB_MASK;\n'
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: no shared memory object (the PS4 has none the normal way; only mmap fastmem needs\n'
      '  // views of it): the RAM from the heap.\n'
      '  g_ram = static_cast<uint8_t*>(std::calloc(1, ram_size));\n'
      '#else\n'
      '  if (!m_memory_arena.Create(MEMORY_ARENA_SIZE, true, false))\n    return false;\n\n'
      '  // Create the base views.\n'
      '  g_ram = static_cast<uint8_t*>(m_memory_arena.CreateViewPtr(MEMORY_ARENA_RAM_OFFSET, ram_size, true, false));\n'
      '#endif\n')
patch('src/core/bus.cpp',
      '  if (g_ram)\n  {\n    m_memory_arena.ReleaseViewPtr(g_ram, g_ram_size);\n',
      '  if (g_ram)\n  {\n'
      '#ifdef EMUSUNI_PORT\n    std::free(g_ram);\n#else\n'
      '    m_memory_arena.ReleaseViewPtr(g_ram, g_ram_size);\n#endif\n')
patch('src/core/bus.cpp',
      '    g_ram_size = 0;\n  }\n\n  m_memory_arena.Destroy();\n',
      '    g_ram_size = 0;\n  }\n\n#ifndef EMUSUNI_PORT\n  m_memory_arena.Destroy();\n#endif\n')

# --- The recompiler's code buffer: executable through EmuSuNi, on its own 16 KiB pages.
patch('src/common/jit_code_buffer.cpp',
      'JitCodeBuffer::JitCodeBuffer() = default;\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (port/ps4_n64.c): makes [address, address + size) executable the ways the PS4 allows,\n'
      '// each checked by running a small function there; the way that worked (0-3) or -1.\n'
      'extern "C" int emusuni_make_executable(void* address, size_t size);\n'
      '#endif\n\n'
      'JitCodeBuffer::JitCodeBuffer() = default;\n')
patch('src/common/jit_code_buffer.cpp',
      '  m_code_ptr = static_cast<uint8_t*>(mmap(nullptr, m_total_size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0));\n'
      '  if (!m_code_ptr)\n    return false;\n',
      '  m_code_ptr = static_cast<uint8_t*>(mmap(nullptr, m_total_size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0));\n'
      '  if (!m_code_ptr || m_code_ptr == MAP_FAILED)\n  {\n    m_code_ptr = nullptr;\n    return false;\n  }\n')
patch('src/common/jit_code_buffer.cpp',
      '  m_code_ptr = static_cast<uint8_t*>(buffer);\n  m_old_protection = static_cast<uint32_t>(old_protect);\n'
      '#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)\n'
      '  if (mprotect(buffer, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)\n',
      '  m_code_ptr = static_cast<uint8_t*>(buffer);\n  m_old_protection = static_cast<uint32_t>(old_protect);\n'
      '#elif defined(EMUSUNI_PORT)\n'
      '  // EmuSuNi: no guard pages (their protection could undo a new mapping of the buffer); it stays\n'
      '  // executable after Destroy (the next game uses it again).\n'
      '  if (emusuni_make_executable(buffer, size) < 0)\n    return false;\n'
      '  m_code_ptr = static_cast<uint8_t*>(buffer);\n  m_old_protection = 0;\n'
      '#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)\n'
      '  if (mprotect(buffer, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)\n')
patch('src/common/jit_code_buffer.cpp',
      '#else\n    mprotect(m_code_ptr, m_total_size, m_old_protection);\n#endif\n',
      '#elif !defined(EMUSUNI_PORT)\n    mprotect(m_code_ptr, m_total_size, m_old_protection);\n#endif\n')
patch('src/core/cpu_code_cache.cpp',
      '#if defined(CPU_AARCH32)\n// Use a smaller code buffer size on AArch32 to have a better chance of being in range.\n',
      '#if defined(CPU_AARCH32) || defined(EMUSUNI_PORT)\n'
      '// Use a smaller code buffer size on AArch32 to have a better chance of being in range.\n'
      '// EmuSuNi: and in the PS4 app\'s data (24 MiB, not 48: the app links two other cores\' buffers too);\n'
      '// a full buffer is flushed ("Out of code space" in the log) and refilled.\n')
patch('src/core/cpu_code_cache.cpp',
      'alignas(Recompiler::CODE_STORAGE_ALIGNMENT) static uint8_t\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: on pages of its own (the PS4\'s are 16 KiB), as it may be mapped anew to be executable.\n'
      'alignas(16384)\n#else\nalignas(Recompiler::CODE_STORAGE_ALIGNMENT)\n#endif\nstatic uint8_t\n')
patch('src/core/cpu_code_cache.cpp',
      '    if (!has_buffer && !s_code_buffer.Allocate(RECOMPILER_CODE_CACHE_SIZE, RECOMPILER_FAR_CODE_CACHE_SIZE)) { }\n',
      '    if (!has_buffer && !s_code_buffer.Allocate(RECOMPILER_CODE_CACHE_SIZE, RECOMPILER_FAR_CODE_CACHE_SIZE))\n'
      '    {\n'
      '#ifdef EMUSUNI_PORT\n'
      '      // EmuSuNi: no executable memory on this console: the cached interpreter, not a crash.\n'
      '      Log_ErrorPrintf("No executable memory for the recompiler: the cached interpreter runs instead");\n'
      '      g_settings.cpu_execution_mode = CPUExecutionMode::CachedInterpreter;\n'
      '      return;\n'
      '#endif\n'
      '    }\n')
patch('src/core/cpu_code_cache.cpp',
      '#ifdef USE_STATIC_CODE_BUFFER\n'
      '    s_code_buffer.Initialize(s_code_storage, sizeof(s_code_storage), RECOMPILER_FAR_CODE_CACHE_SIZE,RECOMPILER_GUARD_SIZE);\n'
      '#else\n',
      '#ifdef USE_STATIC_CODE_BUFFER\n'
      '    if (!s_code_buffer.Initialize(s_code_storage, sizeof(s_code_storage), RECOMPILER_FAR_CODE_CACHE_SIZE,RECOMPILER_GUARD_SIZE))\n'
      '    {\n'
      '#ifdef EMUSUNI_PORT\n'
      '      Log_ErrorPrintf("No executable memory for the recompiler: the cached interpreter runs instead");\n'
      '      g_settings.cpu_execution_mode = CPUExecutionMode::CachedInterpreter;\n'
      '      return;\n'
      '#endif\n'
      '    }\n'
      '#else\n')

# --- Vulkan: no library to open; the frontend's device adopted; the shared queue locked.
patch('dep/vulkan-loader/src/vulkan_loader.cpp',
      '#ifndef _WIN32\n#include <dlfcn.h>\n#endif\n',
      '#if !defined(_WIN32) && !defined(EMUSUNI_PORT)\n#include <dlfcn.h>\n#endif\n')
patch('dep/vulkan-loader/src/vulkan_loader.cpp',
      '#if defined(_WIN32)\n\nstatic HMODULE vulkan_module;\n',
      '#if defined(EMUSUNI_PORT)\n\n'
      '// EmuSuNi: no Vulkan library to open: the frontend hands over its vkGetInstanceProcAddr.\n'
      'bool LoadVulkanLibrary()\n{\n  return false;\n}\n\n'
      'void UnloadVulkanLibrary()\n{\n  ResetVulkanLibraryFunctionPointers();\n}\n\n'
      '#elif defined(_WIN32)\n\nstatic HMODULE vulkan_module;\n')
patch('src/common/vulkan/context.h',
      '  // Destroys context.\n  static void Destroy();\n',
      '  // Destroys context.\n  static void Destroy();\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: a context on the frontend\'s own device and queue (neither is destroyed with it);\n'
      '  // enabled_features: the features that device was made with.\n'
      '  static bool CreateFromFrontendDevice(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, VkQueue queue,\n'
      '                                       uint32_t queue_family_index,\n'
      '                                       const VkPhysicalDeviceFeatures& enabled_features);\n\n'
      '  // EmuSuNi: the frontend\'s queue lock, taken around every submission and device wait.\n'
      '  void SetQueueLock(void (*lock)(void*), void (*unlock)(void*), void* handle)\n'
      '  {\n'
      '    m_queue_lock = lock;\n'
      '    m_queue_unlock = unlock;\n'
      '    m_queue_lock_handle = handle;\n'
      '  }\n'
      '#endif\n')
patch('src/common/vulkan/context.h',
      '  bool m_owns_device = false;\n',
      '  bool m_owns_device = false;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '  void (*m_queue_lock)(void*) = nullptr;\n'
      '  void (*m_queue_unlock)(void*) = nullptr;\n'
      '  void* m_queue_lock_handle = nullptr;\n'
      '#endif\n')
patch('src/common/vulkan/context.cpp',
      'void Context::Destroy()\n{\n  g_vulkan_context.reset();\n}\n',
      'void Context::Destroy()\n{\n  g_vulkan_context.reset();\n}\n\n'
      '#ifdef EMUSUNI_PORT\n'
      'bool Context::CreateFromFrontendDevice(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, VkQueue queue,\n'
      '                                       uint32_t queue_family_index,\n'
      '                                       const VkPhysicalDeviceFeatures& enabled_features)\n'
      '{\n'
      '  g_vulkan_context.reset(new Context(instance, gpu, false));\n'
      '  Context* context = g_vulkan_context.get();\n'
      '  context->m_device = device;\n'
      '  context->m_graphics_queue = queue;\n'
      '  context->m_graphics_queue_family_index = queue_family_index;\n'
      '  context->m_present_queue = queue;\n'
      '  context->m_present_queue_family_index = queue_family_index;\n'
      '  context->m_device_features = enabled_features;\n'
      '  if (!LoadVulkanDeviceFunctions(device) || !context->CreateGlobalDescriptorPool() ||\n'
      '      !context->CreateCommandBuffers())\n'
      '  {\n'
      '    Log_ErrorPrintf("Vulkan: the frontend\'s device can\'t be used");\n'
      '    g_vulkan_context.reset();\n'
      '    return false;\n'
      '  }\n'
      '  return true;\n'
      '}\n'
      '#endif\n')
patch('src/common/vulkan/context.cpp',
      'void Context::WaitForGPUIdle()\n{\n  vkDeviceWaitIdle(m_device);\n}\n',
      'void Context::WaitForGPUIdle()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: every queue of the device is the frontend\'s too.\n'
      '  if (m_queue_lock)\n    m_queue_lock(m_queue_lock_handle);\n'
      '  vkDeviceWaitIdle(m_device);\n'
      '  if (m_queue_unlock)\n    m_queue_unlock(m_queue_lock_handle);\n'
      '#else\n'
      '  vkDeviceWaitIdle(m_device);\n'
      '#endif\n}\n')
patch('src/common/vulkan/context.cpp',
      '  VkResult res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);\n',
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: the queue is the frontend\'s (its menus draw on it): under its lock.\n'
      '  if (m_queue_lock)\n    m_queue_lock(m_queue_lock_handle);\n'
      '  VkResult res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);\n'
      '  if (m_queue_unlock)\n    m_queue_unlock(m_queue_lock_handle);\n'
      '#else\n'
      '  VkResult res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);\n'
      '#endif\n')
patch('src/core/gpu_hw_vulkan.cpp',
      '#include <cstring>\nLog_SetChannel(GPU_HW_Vulkan);\n',
      '#include <cstring>\n'
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#endif\n'
      'Log_SetChannel(GPU_HW_Vulkan);\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the features the frontend\'s device was made with, of the two the renderer uses:\n'
      '// EMUSUNI_VK_DEVICE_FEATURES lists them (VkPhysicalDeviceFeatures names, comma-separated); without\n'
      '// it, what ParaLLEl-RDP\'s Granite (which makes EmuSuNi\'s device) enables: sampleRateShading when the\n'
      '// GPU has it, never dualSrcBlend.\n'
      'static bool EmuSuNiListedFeature(const char* list, const char* name)\n'
      '{\n'
      '  const size_t length = std::strlen(name);\n'
      '  for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += length)\n'
      '  {\n'
      '    if ((p == list || p[-1] == \',\') && (p[length] == \'\\0\' || p[length] == \',\'))\n'
      '      return true;\n'
      '  }\n'
      '  return false;\n'
      '}\n\n'
      'static bool EmuSuNiAdoptFrontendDevice(const retro_hw_render_interface_vulkan* ri)\n'
      '{\n'
      '  vkGetInstanceProcAddr = ri->get_instance_proc_addr;\n'
      '  if (!vkGetInstanceProcAddr || !Vulkan::LoadVulkanInstanceFunctions(ri->instance))\n'
      '  {\n'
      '    Log_ErrorPrintf("Vulkan: the frontend\'s instance functions can\'t be loaded");\n'
      '    Vulkan::ResetVulkanLibraryFunctionPointers();\n'
      '    return false;\n'
      '  }\n'
      '  if (ri->get_device_proc_addr)\n'
      '    vkGetDeviceProcAddr = ri->get_device_proc_addr;\n'
      '  VkPhysicalDeviceFeatures available = {};\n'
      '  vkGetPhysicalDeviceFeatures(ri->gpu, &available);\n'
      '  const char* list = std::getenv("EMUSUNI_VK_DEVICE_FEATURES");\n'
      '  VkPhysicalDeviceFeatures enabled = {};\n'
      '  enabled.dualSrcBlend = available.dualSrcBlend && list && EmuSuNiListedFeature(list, "dualSrcBlend");\n'
      '  enabled.sampleRateShading =\n'
      '    available.sampleRateShading && (!list || EmuSuNiListedFeature(list, "sampleRateShading"));\n'
      '  Log_InfoPrintf("Vulkan: the frontend\'s device (queue family %u), dual-source blend %s, per-sample shading %s",\n'
      '                 ri->queue_index, enabled.dualSrcBlend ? "on" : "off", enabled.sampleRateShading ? "on" : "off");\n'
      '  if (!Vulkan::Context::CreateFromFrontendDevice(ri->instance, ri->gpu, ri->device, ri->queue, ri->queue_index,\n'
      '                                                enabled))\n'
      '  {\n'
      '    Vulkan::ResetVulkanLibraryFunctionPointers();\n'
      '    return false;\n'
      '  }\n'
      '  return true;\n'
      '}\n'
      '#endif\n')
patch('src/core/gpu_hw_vulkan.cpp',
      '  if (!g_vulkan_context)\n  {\n    Log_ErrorPrintf("Vulkan context was not negotiated/created");\n    return false;\n  }\n\n'
      '  // TODO: Grab queue? it should be the same\n'
      '  m_ri = reinterpret_cast<retro_hw_render_interface_vulkan*>(ri);\n  return true;\n',
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: the frontend made its device itself and never calls create_device: adopt it.\n'
      '  if (!g_vulkan_context && !EmuSuNiAdoptFrontendDevice(reinterpret_cast<retro_hw_render_interface_vulkan*>(ri)))\n'
      '    return false;\n'
      '#endif\n'
      '  if (!g_vulkan_context)\n  {\n    Log_ErrorPrintf("Vulkan context was not negotiated/created");\n    return false;\n  }\n\n'
      '  // TODO: Grab queue? it should be the same\n'
      '  m_ri = reinterpret_cast<retro_hw_render_interface_vulkan*>(ri);\n'
      '#ifdef EMUSUNI_PORT\n'
      '  g_vulkan_context->SetQueueLock(m_ri->lock_queue, m_ri->unlock_queue, m_ri->handle);\n'
      '#endif\n'
      '  return true;\n')
patch('src/core/gpu_hw_vulkan.cpp',
      '  m_ri->lock_queue(m_ri->handle);\n  g_vulkan_context->SubmitCommandBuffer();\n  m_ri->unlock_queue(m_ri->handle);\n',
      '#ifdef EMUSUNI_PORT\n'
      '  g_vulkan_context->SubmitCommandBuffer();  // EmuSuNi: under the queue lock (the context takes it)\n'
      '#else\n'
      '  m_ri->lock_queue(m_ri->handle);\n  g_vulkan_context->SubmitCommandBuffer();\n  m_ri->unlock_queue(m_ri->handle);\n'
      '#endif\n')

for path, text in patched.items():
    full = root + '/' + path
    with open(full, encoding='utf-8') as f:
        same = f.read() == text
    if not same:
        with open(full, 'w', encoding='utf-8') as f:
            f.write(text)
print("SwanStation: patches applied")
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-ps1-core.sh: the swanstation core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f swanstation_$f"; done
} > "$OUT/prefix_swanstation.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_swanstation.h.new" "$OUT/prefix_swanstation.h"; then
  rm -f "$OUT/prefix_swanstation.h.new"
else
  mv "$OUT/prefix_swanstation.h.new" "$OUT/prefix_swanstation.h"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): CMake's -O3, the
# PS4's CPU and the renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -include $OUT/prefix_swanstation.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a"
OPTIONS=(
  -DEMUSUNI_PORT=ON -DLIBRETRO_COMMON_NO_MMAP=ON -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CORE"
)
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  }
else
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
WANT="$SRC | $(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown) | host $HOST | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "SwanStation: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "SwanStation: configuring failed (see $LOG)"
    exit 1
  fi
fi
if ! ninja -C "$BUILD" swanstation_libretro >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "SwanStation: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: only the libretro API (renamed) stays global. Every library the core target was made
# of (the core, its common code, libchdr and its codecs, libretro-common, the Vulkan loader...).
# Replaced only when it changed (the app isn't linked again for nothing).
mapfile -t ARCHIVES < <(find "$BUILD" -name '*.a' | sort)
ld.lld-21 -r --whole-archive "${ARCHIVES[@]}" -o "$OUT/ps1_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='swanstation_retro_*' "$OUT/ps1_core_all.o" "$OUT/ps1_core.o.new"
rm -f "$OUT/ps1_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/ps1_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "swanstation_$f"; done | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "SwanStation: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/ps1_core.o.new" | awk '{print $NF}' | sort -u)
if llvm-nm-21 --defined-only "$OUT/ps1_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "SwanStation: the object defines a C++ operator new/delete of its own"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  # What the app gives it: operator new/delete (port/ps4_new.cpp) and the executable memory
  # (port/ps4_n64.c), compiled here as the app compiles them, only to list their symbols.
  PS4C="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay"
  PS4C+=" -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -march=btver2"
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $PS4C -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  clang-21 $PS4C -O2 -c "$PROJECT/port/ps4_n64.c" -o "$OUT/check_ps4_n64.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o" | awk 'NF >= 3 {print $3}';
                 echo __dso_handle; } | sort -u)
  rm -f "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o"
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "SwanStation: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
    echo "$NOT_APPS"
    exit 1
  fi
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a $PS4_BUILD_ROOT/sysroot/lib/libc++.a
    $PS4_BUILD_ROOT/sysroot/lib/libc++abi.a $OPENORBIS/lib/libunwind.a"
  PROVIDED=$( { for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done | awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVvi]$/ {print $NF}'; echo "$APP_GIVES"; } | sort -u)
  MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
  if [ -n "$MISSING" ]; then
    echo "SwanStation: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "SwanStation: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app" \
       "($(echo "$NEW_DELETE" | sed '/^$/d' | wc -l) operator new/delete, port/ps4_new.cpp's)"
fi
if cmp -s "$OUT/ps1_core.o.new" "$OUT/ps1_core.o"; then
  rm -f "$OUT/ps1_core.o.new"
else
  mv "$OUT/ps1_core.o.new" "$OUT/ps1_core.o"
fi
ls -la "$OUT/ps1_core.o"
