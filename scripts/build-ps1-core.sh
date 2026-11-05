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
#   - The recompiler's code buffer (16 MiB of code and 8 of slow paths, as upstream's 32-bit ARM
#     build, not 32 + 16; a full buffer is flushed) is a part of the app's shared JIT arena
#     (port/ps4_n64.c: emusuni_jit_arena, "ps1"; 16 KiB aligned, read/write/execute the ways the PS4
#     allows, within 2 GiB of the core's code), taken at every start of the recompiler, not an array
#     of the core's own: the PS4 charges the app's whole static memory from its start, and only one
#     core runs at a time. No guard pages, and its protection is never changed (the next core's
#     arena); nothing of it is kept after the game (the app fills it with HLT then). If the arena
#     can't be had the core runs the cached interpreter instead of crashing.
#   - No fastmem: it relies on page faults (the app's fault handler is its crash log) and, in its
#     mmap form, on shared memory objects, which the PS4 doesn't have the normal way. The RAM comes
#     from the heap; the recompiler calls the memory handlers.
#   - No dlopen of a Vulkan library (the frontend's vkGetInstanceProcAddr is all it needs).
#   - Built as a static library, without LTO (ld -r of LLVM bitcode); the files are read with
#     plain reads (libretro-common without mmap: no mapping of disc images on the USB drive).
#   - (00.81) Jackie Chan Stuntmaster on the recompiler (it crawled on the PS4 with the interpreter that
#     the core's game database forced for it): an interrupt that writing SR makes pending is taken at
#     once (DuckStation 312790c9, which dropped that from its database too), in the recompiler's code
#     and the interpreter's uncached blocks.
#   - (00.81) Disc images of 2048-byte sectors (an ISO made by a converter; a .cue's MODE1/2048 or
#     MODE2/2048 track): each sector made a whole Mode 2 Form 1 one as it is read; a lone .bin, .img or
#     .iso gets its sector size from what it holds (it was always read as 2352: such a game never started).
#   - (00.81) A LibCrypt disc without its .sbi starts (the core refused it: the app could only say the
#     game didn't load), logged for the app to tell the player; and each start logs the disc (or none:
#     an image the core couldn't open boots the BIOS without one), its code, the BIOS and the CPU.
#   - (00.89) Pauses as a game goes to a new scene (GitHub #1): the shader worker compiles only the game's texture
#     filter and the depth tests it can use (not ~12000 pipelines), its pipelines are written to the cache file as
#     soon as it is done (not only as the game ends), it and the disc's read thread get PS4 priorities and cores of
#     their own (the POSIX "lower" raised the worker on the PS4), and the log says what each pause waited for.
#   - (00.90) The 00.81 interrupt change leaves the block early only outside a branch's delay slot (there it lost the
#     branch: a game could jump to the wrong place); and any image of 2048-byte sectors (a .cue's MODE1/2048 track, a
#     .chd made from an ISO, not only a lone .iso) is logged once, for the app to tell the player.
# The object is checked at the end: only swanstation_retro_* defined globally, the C++ operator
# new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and none defined in
# it, and every other symbol it needs found in what the app links (OpenOrbis' libc, libkernel,
# libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-ps1-core.sh <out dir> [clean]
#   PS1_CORE_HOST=1 build-ps1-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                 the PC tests: no PS4 checks; the test frontend
#                                                 provides the JIT arena (emusuni_jit_arena)
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

# --- The recompiler's code buffer: a part of the app's shared JIT arena, taken at every start.
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
      '  // EmuSuNi: the buffer is a part of the app\'s JIT arena (port/ps4_n64.c), already read/write/execute:\n'
      '  // no guard pages and no change of its protection, here or at Destroy (the next core\'s arena too).\n'
      '  m_code_ptr = static_cast<uint8_t*>(buffer);\n  m_old_protection = 0;\n'
      '#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)\n'
      '  if (mprotect(buffer, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)\n')
patch('src/common/jit_code_buffer.cpp',
      '#else\n    mprotect(m_code_ptr, m_total_size, m_old_protection);\n#endif\n',
      '#elif !defined(EMUSUNI_PORT)\n    mprotect(m_code_ptr, m_total_size, m_old_protection);\n#endif\n')
patch('src/core/cpu_code_cache.cpp',
      '#include <cstring>\nLog_SetChannel(CPU::CodeCache);\n',
      '#include <cstring>\nLog_SetChannel(CPU::CodeCache);\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (port/ps4_n64.c): a part of the app\'s shared JIT arena (16 KiB aligned, read/write/execute,\n'
      '// within 2 GiB of the core\'s code), the same one again for the same name until the app gives the\n'
      '// arena back after the game; NULL if it can\'t be had.\n'
      'extern "C" void* emusuni_jit_arena(size_t size, const char* name);\n'
      '#endif\n')
patch('src/core/cpu_code_cache.cpp',
      '#if defined(CPU_AARCH32)\n// Use a smaller code buffer size on AArch32 to have a better chance of being in range.\n',
      '#if defined(CPU_AARCH32) || defined(EMUSUNI_PORT)\n'
      '// Use a smaller code buffer size on AArch32 to have a better chance of being in range.\n'
      '// EmuSuNi: and in the PS4 app\'s JIT arena (24 MiB of its 32, not 48); a full buffer is flushed\n'
      '// ("Out of code space" in the log) and refilled.\n')
patch('src/core/cpu_code_cache.cpp',
      'alignas(Recompiler::CODE_STORAGE_ALIGNMENT) static uint8_t\n'
      '  s_code_storage[RECOMPILER_CODE_CACHE_SIZE + RECOMPILER_FAR_CODE_CACHE_SIZE];\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: no array of its own (24 MiB of the app\'s static memory, which the PS4 charges from the\n'
      '// app\'s start): a part of the app\'s JIT arena, taken at every Initialize (Shutdown keeps nothing of\n'
      '// it: the arena is the next core\'s once the game ends).\n'
      'static constexpr uint32_t EMUSUNI_CODE_STORAGE_SIZE = RECOMPILER_CODE_CACHE_SIZE + RECOMPILER_FAR_CODE_CACHE_SIZE;\n'
      '#else\n'
      'alignas(Recompiler::CODE_STORAGE_ALIGNMENT) static uint8_t\n'
      '  s_code_storage[RECOMPILER_CODE_CACHE_SIZE + RECOMPILER_FAR_CODE_CACHE_SIZE];\n'
      '#endif\n')
patch('src/core/cpu_code_cache.cpp',
      '    const bool has_buffer = s_code_buffer.Initialize(s_code_storage, sizeof(s_code_storage),\n'
      '                                                     RECOMPILER_FAR_CODE_CACHE_SIZE, RECOMPILER_GUARD_SIZE);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    void* const code_storage = emusuni_jit_arena(EMUSUNI_CODE_STORAGE_SIZE, "ps1");\n'
      '    const bool has_buffer = code_storage && s_code_buffer.Initialize(code_storage, EMUSUNI_CODE_STORAGE_SIZE,\n'
      '                                                                     RECOMPILER_FAR_CODE_CACHE_SIZE, RECOMPILER_GUARD_SIZE);\n'
      '#else\n'
      '    const bool has_buffer = s_code_buffer.Initialize(s_code_storage, sizeof(s_code_storage),\n'
      '                                                     RECOMPILER_FAR_CODE_CACHE_SIZE, RECOMPILER_GUARD_SIZE);\n'
      '#endif\n')
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
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the arena\'s part again (the same one within a game).\n'
      '    void* const code_storage = emusuni_jit_arena(EMUSUNI_CODE_STORAGE_SIZE, "ps1");\n'
      '    if (!code_storage || !s_code_buffer.Initialize(code_storage, EMUSUNI_CODE_STORAGE_SIZE, RECOMPILER_FAR_CODE_CACHE_SIZE,\n'
      '                                                   RECOMPILER_GUARD_SIZE))\n'
      '    {\n'
      '      Log_ErrorPrintf("No executable memory for the recompiler: the cached interpreter runs instead");\n'
      '      g_settings.cpu_execution_mode = CPUExecutionMode::CachedInterpreter;\n'
      '      return;\n'
      '    }\n'
      '#else\n'
      '    s_code_buffer.Initialize(s_code_storage, sizeof(s_code_storage), RECOMPILER_FAR_CODE_CACHE_SIZE,RECOMPILER_GUARD_SIZE);\n'
      '#endif\n'
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

# --- (00.81) Jackie Chan Stuntmaster: the recompiler, not the interpreter. The game raises a software interrupt by
# writing the status register's interrupt mask (mtc0 SR) with the interrupt already pending, and expects it taken at
# once; the recompiler only made the block's end run the events, so the rest of the block ran first and the game
# crashed - DuckStation forced its interpreter for it (its database, 2020-10), which is several times slower: on the
# PS4 the game crawled. DuckStation's fix (commit 312790c9, 2023-11, "CPU/NewRec: Handle mtc0 rt, sr"), which also
# dropped that, for this recompiler: the block is left right after the mtc0 (the next instruction's PC written,
# the events run at once, which dispatch the interrupt), and the interpreter's uncached blocks end there too.
patch('src/core/cpu_recompiler_code_generator.cpp',
      '            EmitConditionalBranch(Condition::Zero, false, &no_interrupt);\n'
      '            EmitStoreCPUStructField(offsetof(State, downcount), Value::FromConstantU32(0));\n'
      '            EmitBindLabel(&no_interrupt);\n'
      '            m_register_cache.UninhibitAllocation();\n'
      '          }\n'
      '          else if (reg == Cop0Reg::DCIC && g_settings.cpu_recompiler_memory_exceptions)\n',
      '            EmitConditionalBranch(Condition::Zero, false, &no_interrupt);\n'
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi (DuckStation 312790c9): an interrupt pending now is taken now, not at the block\'s end\n'
      '            // (Jackie Chan Stuntmaster, MTV Sports games: a software interrupt raised by writing SR.Im).\n'
      '            // (00.90) Not in a branch\'s delay slot: the PC known there is the one after the delay slot (or a\n'
      '            // stale one after a jump to a register), so leaving the block lost the branch. The block ends\n'
      '            // right after the delay slot anyway, and the events run then, as upstream.\n'
      '            if (!cbi.is_branch_delay_slot && m_pc_valid)\n'
      '            {\n'
      '              EmitBranch(GetCurrentFarCodePointer());\n'
      '              SwitchToFarCode();\n'
      '              WriteNewPC(CalculatePC(), false);\n'
      '              EmitStoreCPUStructField(offsetof(State, downcount), Value::FromConstantU32(0));\n'
      '              EmitExceptionExit();\n'
      '              SwitchToNearCode();\n'
      '            }\n'
      '            else\n'
      '            {\n'
      '              EmitStoreCPUStructField(offsetof(State, downcount), Value::FromConstantU32(0));\n'
      '            }\n'
      '#else\n'
      '            EmitStoreCPUStructField(offsetof(State, downcount), Value::FromConstantU32(0));\n'
      '#endif\n'
      '            EmitBindLabel(&no_interrupt);\n'
      '            m_register_cache.UninhibitAllocation();\n'
      '          }\n'
      '          else if (reg == Cop0Reg::DCIC && g_settings.cpu_recompiler_memory_exceptions)\n')
patch('src/core/cpu_core.cpp',
      '    if (g_state.exception_raised || (!branch && in_branch_delay_slot) ||\n'
      '        IsExitBlockInstruction(g_state.current_instruction))\n'
      '    {\n'
      '      break;\n'
      '    }\n'
      '\n'
      '    in_branch_delay_slot = branch;\n'
      '  }\n'
      '}\n'
      '\n'
      'template void InterpretUncachedBlock<PGXPMode::Disabled>();\n',
      '    if (g_state.exception_raised || (!branch && in_branch_delay_slot) ||\n'
      '        IsExitBlockInstruction(g_state.current_instruction))\n'
      '    {\n'
      '      break;\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    else if ((g_state.current_instruction.bits & 0xFFC0FFFFu) == 0x40806000u && HasPendingInterrupt())\n'
      '    {\n'
      '      // EmuSuNi (DuckStation 312790c9): mtc0 rt, sr with an interrupt pending - Jackie Chan Stuntmaster, MTV\n'
      '      // Sports games raise a software interrupt by writing SR.Im: the block ends, the interrupt is taken.\n'
      '      break;\n'
      '    }\n'
      '#endif\n'
      '\n'
      '    in_branch_delay_slot = branch;\n'
      '  }\n'
      '}\n'
      '\n'
      'template void InterpretUncachedBlock<PGXPMode::Disabled>();\n')
patch('src/libretro/libretro_game_settings.cpp',
      '      || game_code == "SLUS-00684" /* Jackie Chan\'s Stuntmaster (NTSC-U) */\n'
      '      || game_code == "SCES-01444" /* Jackie Chan\'s Stuntmaster (PAL) */\n',
      '#ifndef EMUSUNI_PORT\n'
      '      // EmuSuNi: Jackie Chan Stuntmaster plays on the recompiler (the mtc0 SR change above; DuckStation\'s\n'
      '      // database dropped this with it).\n'
      '      || game_code == "SLUS-00684" /* Jackie Chan\'s Stuntmaster (NTSC-U) */\n'
      '      || game_code == "SCES-01444" /* Jackie Chan\'s Stuntmaster (PAL) */\n'
      '#endif\n')

# --- (00.81) Disc images of 2048-byte sectors (a cooked ISO: a converter's "BIN to ISO", the user data only; a .cue's
# MODE1/2048 or MODE2/2048 track): their sectors made whole as they are read - the core read them as 2352 bytes, so
# such a game never started. The PlayStation's CD controller hands the CPU a data sector's bytes from offset 24 (Mode
# 2 Form 1) and drops a sector of another mode, so each is made a Mode 2 Form 1 one: sync, header (its position on
# the disc as BCD minutes, seconds and frames; mode 2), a data subheader (twice), the 2048 bytes, no EDC/ECC (not
# checked). A converter's ISO keeps no Form 2 sectors (XA sound, STR films): those games play without them.
patch('src/common/cd_image.cpp',
      '      if (!ReadSectorFromIndex(buffer, *m_current_index, m_position_in_index))\n'
      '      {\n'
      '        Log_ErrorPrintf("Read of LBA %u failed", m_position_on_disc);\n'
      '        Seek(m_position_on_disc);\n'
      '        return false;\n'
      '      }\n',
      '      if (!ReadSectorFromIndex(buffer, *m_current_index, m_position_in_index))\n'
      '      {\n'
      '        Log_ErrorPrintf("Read of LBA %u failed", m_position_on_disc);\n'
      '        Seek(m_position_on_disc);\n'
      '        return false;\n'
      '      }\n'
      '#ifdef EMUSUNI_PORT\n'
      '      // EmuSuNi: a 2048-byte sector made a whole Mode 2 Form 1 one (scripts/build-ps1-core.sh).\n'
      '      if (m_current_index->file_sector_size == DATA_SECTOR_SIZE &&\n'
      '          (m_current_index->mode == TrackMode::Mode1 || m_current_index->mode == TrackMode::Mode2Form1))\n'
      '      {\n'
      '        // (00.90) Said once an image, whatever holds it (a .cue\'s MODE1/2048 track, a .chd made from an ISO): the\n'
      '        // app tells the player that films and voices are lost (src/Emulator.cpp looks for this line).\n'
      '        if (!m_emusuni_cooked_logged)\n'
      '        {\n'
      '          m_emusuni_cooked_logged = true;\n'
      '          Log_InfoPrintf("EmuSuNi: \'%s\' has 2048-byte sectors (an ISO\'s: no sound or film sectors): read as Mode 2 Form 1",\n'
      '                         m_filename.c_str());\n'
      '        }\n'
      '        static constexpr uint8_t sync[SECTOR_SYNC_SIZE] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,\n'
      '                                                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};\n'
      '        uint8_t* const sector = static_cast<uint8_t*>(buffer);\n'
      '        std::memmove(sector + 24, sector, DATA_SECTOR_SIZE);\n'
      '        std::memcpy(sector, sync, sizeof(sync));\n'
      '        const Position position = Position::FromLBA(m_current_index->start_lba_on_disc + m_position_in_index);\n'
      '        sector[12] = BinaryToBCD(position.minute);\n'
      '        sector[13] = BinaryToBCD(position.second);\n'
      '        sector[14] = BinaryToBCD(position.frame);\n'
      '        sector[15] = 0x02;\n'
      '        std::memset(sector + 16, 0, 8);\n'
      '        sector[18] = 0x08;  // submode: data\n'
      '        sector[22] = 0x08;\n'
      '        std::memset(sector + 24 + DATA_SECTOR_SIZE, 0, RAW_SECTOR_SIZE - 24 - DATA_SECTOR_SIZE);\n'
      '      }\n'
      '#endif\n')
patch('src/common/cd_image.h',
      '  std::string m_filename;\n',
      '  std::string m_filename;\n'
      '#ifdef EMUSUNI_PORT\n'
      '  bool m_emusuni_cooked_logged = false; // EmuSuNi (00.90): its 2048-byte sectors already in the log\n'
      '#endif\n')
# A lone .bin, .img or .iso (the core opens it as one data track): its sector size from what it holds - a raw image's
# first sector begins with the sync (2352 bytes a sector, as before), a cooked one has its volume descriptor
# ("\1CD001") at 32768, sector 16 (2048 bytes, Mode 2 Form 1). Anything else raw, as before.
patch('src/common/cd_image_bin.cpp',
      '  const uint32_t track_sector_size = RAW_SECTOR_SIZE;\n',
      '#ifdef EMUSUNI_PORT\n'
      '  uint32_t track_sector_size = RAW_SECTOR_SIZE;\n'
      '  TrackMode mode = TrackMode::Mode2Raw;\n'
      '#else\n'
      '  const uint32_t track_sector_size = RAW_SECTOR_SIZE;\n'
      '#endif\n')
patch('src/common/cd_image_bin.cpp',
      '  const uint32_t file_size = static_cast<uint32_t>(rftell(m_fp));\n'
      '  rfseek(m_fp, 0, SEEK_SET);\n',
      '  const uint32_t file_size = static_cast<uint32_t>(rftell(m_fp));\n'
      '  rfseek(m_fp, 0, SEEK_SET);\n'
      '#ifdef EMUSUNI_PORT\n'
      '  {\n'
      '    // EmuSuNi: the sector size from what the image holds (scripts/build-ps1-core.sh).\n'
      '    static constexpr uint8_t sync[SECTOR_SYNC_SIZE] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,\n'
      '                                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};\n'
      '    uint8_t head[SECTOR_SYNC_SIZE] = {}, volume[6] = {};\n'
      '    const bool raw = rfread(head, sizeof(head), 1, m_fp) == 1 && std::memcmp(head, sync, sizeof(sync)) == 0;\n'
      '    const bool cooked = !raw && file_size % DATA_SECTOR_SIZE == 0 &&\n'
      '                        rfseek(m_fp, 16 * DATA_SECTOR_SIZE, SEEK_SET) == 0 &&\n'
      '                        rfread(volume, sizeof(volume), 1, m_fp) == 1 && std::memcmp(volume, "\\1CD001", 6) == 0;\n'
      '    if (cooked)\n'
      '    {\n'
      '      track_sector_size = DATA_SECTOR_SIZE;\n'
      '      mode = TrackMode::Mode2Form1;\n'
      '      Log_InfoPrintf("EmuSuNi: \'%s\' has 2048-byte sectors (an ISO\'s: no sound or film sectors): read as Mode 2 Form 1",\n'
      '                     filename);\n'
      '    }\n'
      '    rfseek(m_fp, 0, SEEK_SET);\n'
      '  }\n'
      '#endif\n')
patch('src/common/cd_image_bin.cpp',
      '  SubChannelQ::Control control = {};\n'
      '  TrackMode mode = TrackMode::Mode2Raw;\n',
      '  SubChannelQ::Control control = {};\n'
      '#ifndef EMUSUNI_PORT\n'
      '  TrackMode mode = TrackMode::Mode2Raw;\n'
      '#endif\n')

# --- (00.81) A LibCrypt-protected disc (dozens of European games: Parasite Eve II, MediEvil, Spyro 3...) without the
# .sbi file of its protection's subchannel data: the core refused to start it - its "are you sure?" needs a dialog
# the libretro frontend hasn't, so the app said only that the game couldn't be loaded. Now it starts (as DuckStation
# does once the player agrees: the game may stop at its protection's checks later), and the log says so for the app
# to tell the player where the .sbi goes ("EmuSuNi: LibCrypt" in src/Emulator.cpp).
patch('src/core/system.cpp',
      '  if (g_host_interface->GetBoolSettingValue("CDROM", "AllowBootingWithoutSBIFile", false))\n',
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: started anyway; the app tells the player (src/Emulator.cpp looks for this line).\n'
      '  Log_WarningPrintf("EmuSuNi: LibCrypt-protected disc %s (%s) without its .sbi file: started anyway",\n'
      '                    s_running_game_code.c_str(), s_running_game_title.c_str());\n'
      '  if (true)\n'
      '#else\n'
      '  if (g_host_interface->GetBoolSettingValue("CDROM", "AllowBootingWithoutSBIFile", false))\n'
      '#endif\n')

# --- (00.81) What the app's log needs to tell a PlayStation game's trouble (players' black screens and slow games):
# the disc in the drive or none (an image the core couldn't open boots the BIOS without a disc: a black screen), the
# disc's code, the BIOS (OpenBIOS, built in, when no BIOS file was found: it starts fewer games) and the CPU the game
# runs on (the core's game database forces the interpreter for a few games).
patch('src/libretro/libretro_host_interface.cpp',
      '  if (!BootSystem(std::move(bp)))\n'
      '    return false;\n',
      '  if (!BootSystem(std::move(bp)))\n'
      '    return false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '  Log_InfoPrintf("EmuSuNi: disc %s, code \'%s\', BIOS %s, CPU %s", System::HasMedia() ? "in" : "NONE",\n'
      '                 System::GetRunningCode().c_str(),\n'
      '                 std::memcmp(Bus::g_bios + 0x78, "OpenBIOS", 8) == 0 ? "OpenBIOS (built in: no BIOS file found)" :\n'
      '                                                                      "from a BIOS file",\n'
      '                 Settings::GetCPUExecutionModeName(g_settings.cpu_execution_mode));\n'
      '#endif\n')

# --- (00.89) Pauses of seconds as a PlayStation game goes from one scene to the next (Bust a Groove after Fever Time,
# Ninpuu Sentai Harikenger after its first boss: GitHub #1). The Vulkan renderer's pipelines are compiled the first
# time a draw needs one ("Lazy": the emulation thread waits for it) while a worker thread compiles them all ahead of
# use. Three things made that worse on the PS4:
#   - The worker walked the whole matrix of every texture filter (7 of them) and depth test (3): ~12000 pipelines,
#     minutes of a PS4 core and their memory; a game draws with one filter, and the PGXP depth buffer's test only when
#     it is on (the app never turns it on). Now the game's filter and the depth tests it can use: about a tenth.
#   - Its pipeline cache file (system/swanstation/vulkan_pipelines.bin) was written only as the game ended: a game left
#     any other way kept nothing and compiled everything again next time. Now also as soon as the worker's walk ends.
#   - Its priority: LowerCurrentThreadPriority moves halfway to POSIX's minimum, and the PS4's priorities run the other
#     way (256 the highest, 767 the lowest): it could put the worker above the emulation thread (480). Now the app's
#     workers' 700, off core 2; and the disc's read thread, which the emulation waits for at every seek (it kept the
#     priority it was made with), at the emulation's 480. Both logged with what POSIX said.
# And the app's log now says what a pause was: a pipeline the emulation thread had to build itself (and how long), a
# wait for the disc's read thread of 20 ms or more, and what the worker did ("EmuSuNi: shader worker done: ...").
patch('src/common/thread_priority.h',
      '}\n\n} // namespace ThreadPriority\n',
      '}\n\n} // namespace ThreadPriority\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (scripts/build-ps1-core.sh): the core\'s threads placed as the app places the other cores\' (port/ps4_n64.c):\n'
      '// a priority and cores of the PS4\'s own. Its priorities run the other way from POSIX\'s (256 the highest, 767 the\n'
      '// lowest, 700 the default): LowerCurrentThreadPriority\'s "halfway to the minimum" could raise a thread above the\n'
      '// emulation\'s 480. What POSIX says of the thread is kept for the log. On the PC (the tests) nothing is changed.\n'
      '#include <cstdint>\n'
      '#include <pthread.h>\n'
      '#include <sched.h>\n'
      '#if defined(__ORBIS__)\n'
      'extern "C" void* emusuni_ps1_thread_self(void) __asm__("scePthreadSelf");\n'
      'extern "C" int emusuni_ps1_thread_getprio(void* thread, int* priority) __asm__("scePthreadGetprio");\n'
      'extern "C" int emusuni_ps1_thread_setprio(void* thread, int priority) __asm__("scePthreadSetprio");\n'
      'extern "C" int emusuni_ps1_thread_setaffinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");\n'
      'extern "C" int emusuni_ps1_thread_rename(void* thread, const char* name) __asm__("scePthreadRename");\n'
      'extern "C" int sceKernelGetCurrentCpu(void);\n'
      '#endif\n\n'
      'namespace ThreadPriority {\n\n'
      'struct EmuSuNiPlacement\n'
      '{\n'
      '  int posix_policy = -1, posix_priority = -1, posix_min = -1, posix_max = -1; // what POSIX says of the thread\n'
      '  int before = -1;                              // the PS4 priority it had\n'
      '  int priority_result = -1, cores_result = -1;  // scePthreadSetprio\'s, scePthreadSetaffinity\'s (0: done)\n'
      '  int cpu = -1;                                 // the core it runs on now\n'
      '};\n\n'
      '// Places the calling thread: the PS4 priority, the cores (a mask) and the name.\n'
      'inline EmuSuNiPlacement EmuSuNiPlaceThread(const char* name, int priority, uint64_t cores)\n'
      '{\n'
      '  EmuSuNiPlacement placed;\n'
      '  sched_param sp = {};\n'
      '  if (pthread_getschedparam(pthread_self(), &placed.posix_policy, &sp) == 0)\n'
      '  {\n'
      '    placed.posix_priority = sp.sched_priority;\n'
      '    placed.posix_min = sched_get_priority_min(placed.posix_policy);\n'
      '    placed.posix_max = sched_get_priority_max(placed.posix_policy);\n'
      '  }\n'
      '#if defined(__ORBIS__)\n'
      '  void* const self = emusuni_ps1_thread_self();\n'
      '  emusuni_ps1_thread_getprio(self, &placed.before);\n'
      '  placed.cores_result = emusuni_ps1_thread_setaffinity(self, cores);\n'
      '  placed.priority_result = emusuni_ps1_thread_setprio(self, priority);\n'
      '  emusuni_ps1_thread_rename(self, name);\n'
      '  placed.cpu = sceKernelGetCurrentCpu();\n'
      '#else\n'
      '  (void)name;\n  (void)priority;\n  (void)cores;\n'
      '#endif\n'
      '  return placed;\n'
      '}\n\n'
      '} // namespace ThreadPriority\n'
      '#endif\n')
patch('src/common/vulkan/shader_cache.h',
      '  void MergeWorkerPipelineCache(VkPipelineCache cache);\n',
      '  void MergeWorkerPipelineCache(VkPipelineCache cache);\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '  /// EmuSuNi: writes a worker\'s cache (seeded with this one\'s) to the file now, when it holds more than the file\n'
      '  /// did; by that worker, its walk done (the file was written only at the end of the game).\n'
      '  bool SaveWorkerPipelineCache(VkPipelineCache cache);\n'
      '#endif\n')
patch('src/common/vulkan/shader_cache.h',
      '  bool m_pipeline_cache_dirty = false;\n',
      '  bool m_pipeline_cache_dirty = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '  size_t m_saved_size = 0; // EmuSuNi: the file\'s size as it was read, or as a worker last wrote it\n'
      '#endif\n')
patch('src/common/vulkan/shader_cache.cpp',
      '#include <file/file_path.h>\n',
      '#include <file/file_path.h>\n'
      '#ifdef EMUSUNI_PORT\n#include <cstdio>\n#endif\n')
patch('src/common/vulkan/shader_cache.cpp',
      '    LOG_VULKAN_ERROR(res, "vkCreatePipelineCache() failed: ");\n'
      '    return false;\n'
      '  }\n\n'
      '  return true;\n'
      '}\n\n'
      'bool ShaderCache::FlushPipelineCache()\n',
      '    LOG_VULKAN_ERROR(res, "vkCreatePipelineCache() failed: ");\n'
      '    return false;\n'
      '  }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '  m_saved_size = data->size();\n'
      '  Log_InfoPrintf("EmuSuNi: pipeline cache: %zu bytes read from \'%s\'", data->size(), m_pipeline_cache_filename.c_str());\n'
      '#endif\n'
      '  return true;\n'
      '}\n\n'
      'bool ShaderCache::FlushPipelineCache()\n')
patch('src/common/vulkan/shader_cache.cpp',
      'void ShaderCache::ClosePipelineCache()\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'bool ShaderCache::SaveWorkerPipelineCache(VkPipelineCache cache)\n'
      '{\n'
      '  // EmuSuNi: only the worker uses its cache (vkGetPipelineCacheData from it is safe here); the file is written\n'
      '  // beside and renamed over the old one, so an app closed meanwhile keeps the old file whole.\n'
      '  if (cache == VK_NULL_HANDLE || m_pipeline_cache_filename.empty())\n'
      '    return false;\n'
      '  const VkDevice device = g_vulkan_context->GetDevice();\n'
      '  size_t data_size = 0;\n'
      '  if (vkGetPipelineCacheData(device, cache, &data_size, nullptr) != VK_SUCCESS || data_size <= m_saved_size)\n'
      '    return false;\n'
      '  std::vector<uint8_t> data(data_size);\n'
      '  if (vkGetPipelineCacheData(device, cache, &data_size, data.data()) != VK_SUCCESS)\n'
      '    return false;\n'
      '  const std::string temp = m_pipeline_cache_filename + ".new";\n'
      '  if (!FileSystem::WriteBinaryFile(temp.c_str(), data.data(), data_size) ||\n'
      '      std::rename(temp.c_str(), m_pipeline_cache_filename.c_str()) != 0)\n'
      '  {\n'
      '    Log_ErrorPrintf("EmuSuNi: the shader worker\'s pipelines couldn\'t be written to \'%s\'", m_pipeline_cache_filename.c_str());\n'
      '    filestream_delete(temp.c_str());\n'
      '    return false;\n'
      '  }\n'
      '  m_saved_size = data_size;\n'
      '  Log_InfoPrintf("EmuSuNi: pipeline cache: the shader worker\'s %zu bytes written to \'%s\'", data_size,\n'
      '                 m_pipeline_cache_filename.c_str());\n'
      '  return true;\n'
      '}\n\n'
      '#endif\n'
      'void ShaderCache::ClosePipelineCache()\n{\n')
patch('src/core/gpu_hw_vulkan.h',
      '  VkPipelineCache m_worker_pipeline_cache = VK_NULL_HANDLE;\n',
      '  VkPipelineCache m_worker_pipeline_cache = VK_NULL_HANDLE;\n'
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi (scripts/build-ps1-core.sh), for the app\'s log: the pipelines the emulation thread built itself (the\n'
      '  // worker hadn\'t reached them) and their time - the emulation thread\'s only; the worker\'s count - the worker\'s.\n'
      '  uint32_t m_emusuni_main_builds = 0;\n'
      '  uint32_t m_emusuni_main_logged = 0;\n'
      '  double m_emusuni_main_build_ms = 0.0;\n'
      '  uint32_t m_emusuni_worker_builds = 0;\n'
      '#endif\n')
patch('src/core/gpu_hw_vulkan.cpp',
      '  ThreadPriority::LowerCurrentThreadPriority();\n\n'
      '  // Filter is the outermost loop.',
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: below the emulation thread (480), the sound and the disc\'s read thread: the app\'s workers\' one priority\n'
      '  // (700), off core 2, as the other cores\' workers (port/ps4_n64.c). On the PS4 LowerCurrentThreadPriority\'s\n'
      '  // "halfway to the minimum" counts the wrong way (256 is its highest priority).\n'
      '  const ThreadPriority::EmuSuNiPlacement placed = ThreadPriority::EmuSuNiPlaceThread("ps1 shaders", 700, 0x3Aull);\n'
      '  Log_InfoPrintf("EmuSuNi: thread \\"ps1 shaders\\": priority %d -> 700 (%#x), cores 1,3,4,5 (%#x), running on CPU %d; "\n'
      '                 "POSIX: policy %d, priority %d, range %d to %d",\n'
      '                 placed.before, static_cast<unsigned>(placed.priority_result), static_cast<unsigned>(placed.cores_result),\n'
      '                 placed.cpu, placed.posix_policy, placed.posix_priority, placed.posix_min, placed.posix_max);\n'
      '  const Common::Timer::Value emusuni_start = Common::Timer::GetValue();\n'
      '  m_emusuni_worker_builds = 0;\n'
      '  const bool emusuni_depth_buffer = m_pgxp_depth_buffer;\n'
      '#else\n'
      '  ThreadPriority::LowerCurrentThreadPriority();\n'
      '#endif\n\n'
      '  // Filter is the outermost loop.')
patch('src/core/gpu_hw_vulkan.cpp',
      '  const bool scaled_dithering = m_scaled_dithering;\n'
      '  for (uint8_t f_offset = 0;',
      '  const bool scaled_dithering = m_scaled_dithering;\n'
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: what the worker did, in the app\'s log; its walk done, its pipelines written to the cache file at once.\n'
      '  const auto emusuni_summary = [&](bool done) {\n'
      '    const double seconds = static_cast<double>(Common::Timer::GetValue() - emusuni_start) / 1e9;\n'
      '    Log_InfoPrintf("EmuSuNi: shader worker %s: %u pipelines built in %.1f s (texture filter %u, PGXP depth buffer %s)",\n'
      '                   done ? "done" : "stopped (the game ended, or a setting changed)", m_emusuni_worker_builds, seconds,\n'
      '                   static_cast<unsigned>(current_filter), emusuni_depth_buffer ? "on" : "off");\n'
      '    if (done)\n'
      '      g_vulkan_shader_cache->SaveWorkerPipelineCache(m_worker_pipeline_cache);\n'
      '  };\n'
      '#endif\n'
      '  for (uint8_t f_offset = 0;')
patch('src/core/gpu_hw_vulkan.cpp',
      '    if (!dual_source && TextureFilterRequiresDualSourceBlend(filter))\n'
      '      continue;\n\n'
      '    for (uint8_t depth_test = 0; depth_test < 3; depth_test++)\n'
      '    {\n',
      '    if (!dual_source && TextureFilterRequiresDualSourceBlend(filter))\n'
      '      continue;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the filter the game draws with only (the other six were most of the walk: minutes of a PS4 core and\n'
      '    // their pipelines\' memory, for a change of filter the player seldom makes; a change starts a worker for it).\n'
      '    if (f_offset > 0)\n'
      '      break;\n'
      '#endif\n\n'
      '    for (uint8_t depth_test = 0; depth_test < 3; depth_test++)\n'
      '    {\n'
      '#ifdef EMUSUNI_PORT\n'
      '      // EmuSuNi: the PGXP depth buffer\'s test (2) only while it is on: the draws ask for it only then.\n'
      '      if (depth_test == 2 && !emusuni_depth_buffer)\n'
      '        continue;\n'
      '#endif\n')
patch('src/core/gpu_hw_vulkan.cpp',
      '                if (retro_atomic_load_acquire_int(&m_shader_compile_thread_quit))\n'
      '                  return;\n',
      '                if (retro_atomic_load_acquire_int(&m_shader_compile_thread_quit))\n'
      '                {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                  emusuni_summary(false);\n'
      '#endif\n'
      '                  return;\n'
      '                }\n')
patch('src/core/gpu_hw_vulkan.cpp',
      '  }\n}\n\nVkShaderModule GPU_HW_Vulkan::GetBatchFragmentShader(',
      '  }\n'
      '#ifdef EMUSUNI_PORT\n'
      '  emusuni_summary(true);\n'
      '#endif\n'
      '}\n\nVkShaderModule GPU_HW_Vulkan::GetBatchFragmentShader(')
patch('src/core/gpu_hw_vulkan.cpp',
      '  const VkPipeline fresh = gpbuilder.Create(g_vulkan_context->GetDevice(), pipeline_cache);\n'
      '  if (fresh == VK_NULL_HANDLE)\n'
      '  {\n'
      '    Log_ErrorPrintf("Lazy batch PSO compile failed',
      '#ifdef EMUSUNI_PORT\n'
      '  const Common::Timer::Value emusuni_start = Common::Timer::GetValue();\n'
      '#endif\n'
      '  const VkPipeline fresh = gpbuilder.Create(g_vulkan_context->GetDevice(), pipeline_cache);\n'
      '#ifdef EMUSUNI_PORT\n'
      '  if (fresh != VK_NULL_HANDLE && on_worker)\n'
      '  {\n'
      '    m_emusuni_worker_builds++;\n'
      '  }\n'
      '  else if (fresh != VK_NULL_HANDLE)\n'
      '  {\n'
      '    // EmuSuNi: one the worker hadn\'t reached, built on the emulation thread: the game waited for it (a new\n'
      '    // scene\'s first draws). The first 40 of 10 ms or more in the app\'s log, then every 50th, with the totals.\n'
      '    const double ms = static_cast<double>(Common::Timer::GetValue() - emusuni_start) / 1e6;\n'
      '    m_emusuni_main_builds++;\n'
      '    m_emusuni_main_build_ms += ms;\n'
      '    if (ms >= 10.0 && (m_emusuni_main_logged < 40 || m_emusuni_main_builds % 50 == 0))\n'
      '    {\n'
      '      m_emusuni_main_logged++;\n'
      '      Log_InfoPrintf("EmuSuNi: a pipeline built on the emulation thread in %.0f ms (the shader worker hadn\'t "\n'
      '                     "reached it): %u so far, %.0f ms in all", ms, m_emusuni_main_builds, m_emusuni_main_build_ms);\n'
      '    }\n'
      '  }\n'
      '#endif\n'
      '  if (fresh == VK_NULL_HANDLE)\n'
      '  {\n'
      '    Log_ErrorPrintf("Lazy batch PSO compile failed')
patch('src/core/cdrom_async_reader.cpp',
      '#include <rthreads/rthreads.h>\nLog_SetChannel(CDROMAsyncReader);\n',
      '#include <rthreads/rthreads.h>\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "common/thread_priority.h"\n'
      '#include "common/timer.h"\n'
      '#endif\n'
      'Log_SetChannel(CDROMAsyncReader);\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (scripts/build-ps1-core.sh): the emulation\'s waits for the read thread long enough to be seen (20 ms or\n'
      '// more: a frame), for the app\'s log - the first 30 of a game, then every 25th, with the totals.\n'
      'static uint32_t s_emusuni_long_waits = 0;\n'
      'static double s_emusuni_long_wait_ms = 0.0;\n'
      '#endif\n')
patch('src/core/cdrom_async_reader.cpp',
      '  m_buffers.clear();\n  m_buffers.resize(readahead_count);\n',
      '  m_buffers.clear();\n  m_buffers.resize(readahead_count);\n'
      '#ifdef EMUSUNI_PORT\n'
      '  s_emusuni_long_waits = 0;\n'
      '  s_emusuni_long_wait_ms = 0.0;\n'
      '#endif\n')
patch('src/core/cdrom_async_reader.cpp',
      '  int state;\n'
      '  while ((state = poll()) < 0)\n'
      '  {\n'
      '    const int key = retro_eventcount_prepare_wait(&t->reader_ec);\n'
      '    if ((state = poll()) >= 0)\n'
      '    {\n'
      '      retro_eventcount_cancel_wait(&t->reader_ec);\n'
      '      break;\n'
      '    }\n'
      '    retro_eventcount_commit_wait(&t->reader_ec, key);\n'
      '  }\n',
      '#ifdef EMUSUNI_PORT\n'
      '  Common::Timer::Value emusuni_from = 0;\n'
      '#endif\n'
      '  int state;\n'
      '  while ((state = poll()) < 0)\n'
      '  {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_from == 0)\n'
      '      emusuni_from = Common::Timer::GetValue();\n'
      '#endif\n'
      '    const int key = retro_eventcount_prepare_wait(&t->reader_ec);\n'
      '    if ((state = poll()) >= 0)\n'
      '    {\n'
      '      retro_eventcount_cancel_wait(&t->reader_ec);\n'
      '      break;\n'
      '    }\n'
      '    retro_eventcount_commit_wait(&t->reader_ec, key);\n'
      '  }\n'
      '#ifdef EMUSUNI_PORT\n'
      '  if (emusuni_from != 0)\n'
      '  {\n'
      '    const double ms = static_cast<double>(Common::Timer::GetValue() - emusuni_from) / 1e6;\n'
      '    if (ms >= 20.0)\n'
      '    {\n'
      '      s_emusuni_long_waits++;\n'
      '      s_emusuni_long_wait_ms += ms;\n'
      '      if (s_emusuni_long_waits <= 30 || s_emusuni_long_waits % 25 == 0)\n'
      '        Log_InfoPrintf("EmuSuNi: the disc\'s next sector (reading from LBA %d) waited %.0f ms for the read thread: "\n'
      '                       "%u such waits, %.0f ms in all", retro_atomic_load_relaxed_int(&t->request_lba), ms,\n'
      '                       s_emusuni_long_waits, s_emusuni_long_wait_ms);\n'
      '    }\n'
      '  }\n'
      '#endif\n')
patch('src/core/cdrom_async_reader.cpp',
      '  ThreadState* t = static_cast<ThreadState*>(userdata);\n'
      '  t->owner->WorkerThread(t);\n',
      '  ThreadState* t = static_cast<ThreadState*>(userdata);\n'
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi: the emulation waits for this thread at every seek: at the emulation thread\'s priority (480), as the\n'
      '  // app\'s other cores\' threads it waits for (port/ps4_n64.c), off core 2. It kept the priority it was made with.\n'
      '  const ThreadPriority::EmuSuNiPlacement placed = ThreadPriority::EmuSuNiPlaceThread("ps1 disc", 480, 0x3Aull);\n'
      '  Log_InfoPrintf("EmuSuNi: thread \\"ps1 disc\\": priority %d -> 480 (%#x), cores 1,3,4,5 (%#x), running on CPU %d; "\n'
      '                 "POSIX: policy %d, priority %d, range %d to %d",\n'
      '                 placed.before, static_cast<unsigned>(placed.priority_result), static_cast<unsigned>(placed.cores_result),\n'
      '                 placed.cpu, placed.posix_policy, placed.posix_priority, placed.posix_min, placed.posix_max);\n'
      '#endif\n'
      '  t->owner->WorkerThread(t);\n')

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
# PS4's CPU, a section for each function and variable (the app's link, --gc-sections, drops the ones
# nothing uses: the renderers and libraries the app doesn't), unwind tables only for the functions that
# can throw (not the toolchain's -funwind-tables, a frame for every function) and the renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/prefix_swanstation.h"
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
# (At most 4 compilers at once: the PC's load, scripts/build.sh.)
if ! ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}" swanstation_libretro >> "$LOG" 2>&1; then
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
# No static array of 1 MiB or more: the PS4 charges the app's whole static memory from its start, used or
# not (the recompiler's code buffer is a part of the app's JIT arena).
BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/ps1_core.o.new" | awk '($3 == "b" || $3 == "B") && $2 + 0 >= 1048576 {print $4}')
if [ -n "$BIG" ]; then
  echo "SwanStation: the object has static arrays of 1 MiB or more:"
  echo "$BIG"
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
