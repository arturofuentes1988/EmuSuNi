#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds PPSSPP (hrydgard/ppsspp v1.20.4, ~/src/ppsspp with its submodules, GPL-2.0-or-later: the
# PSP emulator) for the PS4 into one object that scripts/build.sh links into EmuSuNi: the PSP. Its
# own CMake (LIBRETRO=ON) makes the libretro core with the PS4 toolchain
# (toolchain/ps4-love-modern.cmake): the x86-64 JIT (and the IR interpreter), the Vulkan renderer
# (internal resolution, texture scaling, MSAA; GLSL compiled at run time by glslang), the software
# renderer, ISO/CSO/CHD/PBP/ELF/PRX read by the core itself (stdio), the HLE kernel and firmware
# modules (no PSP firmware needed; the PPSSPP assets in <system>/PPSSPP), and FFmpeg 3.0.2 (PPSSPP's
# own fork, its ffmpeg/ submodule) built here with the same toolchain for the H.264 cutscenes,
# MP3 and AAC (ATRAC3/3+ have PPSSPP's own decoder). Its libretro API becomes ppsspp_retro_* and
# everything else in it private, as for FCEUmm (CMakeLists.txt). The PS4 changes to its sources
# (applied each time to the files as git has them, checked; all under EMUSUNI_PORT, which the
# CMake option of that name defines):
#   - Vulkan only: no OpenGL (PPSSPP_API_ANY_GL off, the GL backends left out, as in upstream's
#     GL-less Windows builds). The renderer adopts the frontend's device: EmuSuNi makes its only
#     VkDevice at start (src/Display.cpp, through ParaLLEl-RDP's Granite) and never calls the
#     context negotiation's create_device, so the core takes the instance, GPU, device and queue of
#     GET_HW_RENDER_INTERFACE when the context is reset. The instance has no surface extension and
#     the device no swapchain: the surface and the swapchain are emulated (upstream already
#     emulates the swapchain on top of the frontend's: its images go to set_image). What the core
#     sees of the GPU is what that device was made with: Vulkan 1.1 (EMUSUNI_VK_API_VERSION to
#     change it), the device extensions listed in EMUSUNI_VK_DEVICE_EXTENSIONS and the features in
#     EMUSUNI_VK_DEVICE_FEATURES (comma-separated VkPhysicalDeviceFeatures names, plus multiview,
#     provokingVertexLast and scalarBlockLayout for the extension features); none when unset. The
#     shader cache's pipelines that need a feature the device wasn't made with are skipped.
#   - Every vkQueueSubmit, vkQueueWaitIdle and vkDeviceWaitIdle of the core under the frontend's
#     lock_queue / unlock_queue (the queue is the menus' one too; upstream locked the first two).
#     The swapchain's images: the core calls the frontend's set_image only at the end of retro_run
#     (the newest image presented) and never get_sync_index / wait_sync_index; it takes the images
#     round robin, never the one the frontend shows (libretro_vulkan.cpp). So the frontend may draw
#     on another thread while retro_run runs, and PPSSPP's render thread (EMUSUNI_PSP_RENDER_THREAD=1;
#     off unless set) records and submits the GPU work beside the emulation, calling only
#     lock_queue / unlock_queue (the frontend then gets each frame one retro_run later).
#   - The JIT's code memory (one 22 MiB array in the core's data, within 2 GiB of its code, 16 KiB
#     aligned: the MIPS JIT's 16 MiB, its thunks and safe-memory functions, the vertex decoder JIT,
#     the software renderer's) is made executable once through EmuSuNi (port/ps4_n64.c:
#     emusuni_make_executable, the ways the PS4 allows) instead of mmap/mprotect, and shared out to
#     the code blocks. If that fails, the core says it can't JIT and runs the IR interpreter.
#   - The PSP's memory: masked addresses (MASKED_PSP_MEMORY, as upstream's 32-bit builds). Fast Memory
#     as the option ppsspp_fast_memory says when the game starts (EMUSUNI_PSP_FASTMEM=0/1 overrides):
#     then the core's fault handler (only then) skips the JIT's stray accesses as upstream's does and
#     passes every other fault on (the app's crash log). On the PS4 a handler of its own with the
#     FreeBSD kernel's values written out (SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, si_addr at 24,
#     the mcontext 0x40 bytes into the context; the layouts checked at compile time), no alternate
#     stack (as DolphinPS4's port of Dolphin's).
#     The views (scratchpad, VRAM and its three mirrors, RAM) are mapped in a 1 GiB reserved range
#     (every masked address; Common/MemArenaEmuSuNi.cpp): on the PS4 one block of direct memory,
#     mapped again for the mirrors (sceKernelReserveVirtualRange, sceKernelAllocateDirectMemory,
#     sceKernelMapDirectMemory); if the system refuses a second mapping a mirror gets its own memory
#     (upstream's NO_MMAP behaviour) - the log says which ("VRAM mirrors aliased"). On this PC: a
#     memfd mapped the same way (EMUSUNI_PSP_NO_ALIAS=1 tries the PS4's fallback).
#   - Threads: every thread PPSSPP names (its pool's "PoolW n" / "PoolW IO n", "ShaderCompile",
#     "IO", "ExecLoader", "SaveIO"...; "Main" for the emulation thread in retro_init) first calls
#     emusuni_psp_thread(name), which the app defines (port/): its core, priority and name. The
#     thread pool has at most 4 compute threads (and 4 for blocking I/O).
#   - ISOs are never cached whole in RAM (ppsspp_cache_iso is ignored); files are read with stdio
#     (libretro-common without mmap). The sound conversion fixed for blocks of more than 1024 frames
#     (upstream read past a 1024-frame stack buffer).
#   - One retro_run per vblank, always (frame duplication on: the app paces by the display; without
#     it a 30 fps game would end a frame every other vblank and run twice as fast). A game that can't
#     start asks for RETRO_ENVIRONMENT_SHUTDOWN; retro_run then does nothing (upstream ran the CPU it
#     never set up: a crash). A program's debug output (homebrew, PPSSPP's tests) logged at INFO.
#   - A few lines in the log every ~10 s (PSP_TIMING_FRAMES=n: every n frames; Common/EmuSuNiTiming.h,
#     the TSC, a few dozen reads a frame): retro_run's parts (the display lists and their blocking
#     readbacks, waiting for the render thread and for the GPU, set_image and the sound, the rest),
#     the render thread's (its waits for pipelines, the queue lock, vkQueueSubmit), the pipelines and
#     shaders compiled, the slowest frame's parts, the GPU emulation's events per frame (draws,
#     readbacks and why, block transfers, uploads, copies), the resolution and MSAA, and the GPU's own
#     time per kind of step and its heaviest steps (PPSSPP's timestamp profiler, 8 frames a line).
#   - God of War at 30 FPS when the option ppsspp_emusuni_30fps says so (PPSSPP's FramerateHack30
#     for weak hardware: its hook waits two vblanks a frame, half the work a second).
#   - The driver's pipeline cache (vkGetPipelineCacheData) saved with the shader cache when
#     EMUSUNI_PSP_PIPELINE_CACHE=1 (off unless set; its files a cache version of their own): the
#     pipelines of earlier sessions are cache hits instead of compiled again at each start; used only
#     when the device's pipelineCacheUUID matches (else the keys alone, as before).
#   - God of War: Ghost of Sparta without the first-frame readbacks (compat.ini's
#     DisableFirstFrameReadback): each new framebuffer was copied to the PSP's memory with the GPU
#     drained, a hitch each time; the game never reads it (the same pictures through its intro).
#   - Built as a static library, without LTO (ld -r of LLVM bitcode), with its own zlib, libpng,
#     zstd (without its dictionary builder), libchdr... (nothing of the system's), no Discord, UPnP or
#     HTTPS. As one object: the JIT's common files only in Core (upstream also puts them in Common for
#     ARM), the LZMA decoder files once (libchdr's; 7-Zip's are the same files), Common/UI's popups
#     and the atlas tool's code left out (they call code the libretro core doesn't have). The PS4's
#     CMake system is "Generic": glslang's and aemu_postoffice's POSIX parts taken anyway, CPUID
#     instead of cpu_features (its FreeBSD side reads /var/run/dmesg.boot), ghc::filesystem told
#     it's a BSD.
# The object is checked at the end: only ppsspp_retro_* defined globally, no weak references left,
# the C++ operator new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and
# none defined in it, and every other symbol it needs found in what the app links (OpenOrbis' libc,
# libkernel, libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c;
# emusuni_psp_thread from port/).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
# FFmpeg is configured and built again only when its flags change.
#   build-psp-core.sh <out dir> [clean]
#   PSP_CORE_HOST=1 build-psp-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                 the PC tests: no PS4 checks; the test frontend
#                                                 provides emusuni_make_executable and
#                                                 emusuni_psp_thread
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${PPSSPP_DIR:-$HOME/src/ppsspp}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/psp-core.log"
HOST="${PSP_CORE_HOST:-0}"
JOBS="${PSP_CORE_JOBS:-4}"   # at most 4 compilers at once (the PC's load)

python3 -I - "$SRC" <<'PATCH'
import os
import subprocess
import sys
root = sys.argv[1]
patched = {}
def git_file(repo, path):
    blob = subprocess.run(['git', '-C', repo, 'show', 'HEAD:' + path], check=True,
                          capture_output=True).stdout
    return blob.decode('utf-8').replace('\r\n', '\n')
def patch(path, old, new, repo=None):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    # repo: a submodule's own tree (path relative to it).
    key = (repo or root, path)
    if key not in patched:
        patched[key] = git_file(key[0], path)
    text = patched[key]
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[key] = text.replace(old, new)
def addfile(path, text):
    # A file of EmuSuNi's own in the tree (git doesn't know it).
    patched[(root, path)] = text

# --- The build: the EMUSUNI_PORT option, a static libretro core, Vulkan only, no system libraries.
patch('CMakeLists.txt',
      'option(LIBRETRO "Set to ON to generate the libretro target" OFF)\n',
      'option(LIBRETRO "Set to ON to generate the libretro target" OFF)\n'
      '# EmuSuNi (scripts/build-psp-core.sh): the libretro core as one object of the PS4 app (a static\n'
      '# library, Vulkan only), with the changes under EMUSUNI_PORT.\n'
      'option(EMUSUNI_PORT "Build the libretro core for EmuSuNi (PS4)" OFF)\n'
      'if(EMUSUNI_PORT)\n\tadd_compile_definitions(EMUSUNI_PORT=1)\n'
      '\t# No zstd tracing hooks (weak references nothing defines).\n'
      '\tadd_compile_definitions(ZSTD_TRACE=0)\nendif()\n')
patch('CMakeLists.txt',
      'if(NOT OPENGL_LIBRARIES)\n\tif(POLICY CMP0072)\n',
      'if(NOT OPENGL_LIBRARIES AND NOT EMUSUNI_PORT)\n\tif(POLICY CMP0072)\n')
patch('CMakeLists.txt',
      'find_package(ZLIB)\nif(ZLIB_FOUND AND NOT ANDROID)\n',
      '# EmuSuNi: the bundled zlib (in the core\'s object, private).\n'
      'if(NOT EMUSUNI_PORT)\n\tfind_package(ZLIB)\nendif()\nif(ZLIB_FOUND AND NOT ANDROID)\n')
patch('CMakeLists.txt',
      '\tCommon/MemArenaHorizon.cpp\n',
      '\tCommon/MemArenaHorizon.cpp\n\tCommon/MemArenaEmuSuNi.cpp\n')
patch('CMakeLists.txt',
      'if(LIBRETRO)\n\tadd_subdirectory(libretro)\nendif()\n',
      'if(EMUSUNI_PORT)\n'
      '\t# EmuSuNi: Vulkan only. The OpenGL backends\' sources are left out (PPSSPP_API_ANY_GL is off, as in\n'
      '\t# upstream\'s GL-less Windows builds); GLFeatures.cpp (data the shader generators read) stays.\n'
      '\t# The JIT\'s common files and DisArm64 are in Core and (for the ARM builds) Common: only Core\'s\n'
      '\t# (the core becomes one object of every library, so nothing may be in two).\n'
      '\tforeach(emusuni_target Common native ${CoreLibName})\n'
      '\t\tif(TARGET ${emusuni_target})\n'
      '\t\t\tget_target_property(emusuni_sources ${emusuni_target} SOURCES)\n'
      '\t\t\tlist(FILTER emusuni_sources EXCLUDE REGEX "(^|/)(Common/GPU/OpenGL/|GPU/GLES/|ext/glew/)")\n'
      '\t\t\tif(emusuni_target STREQUAL "Common")\n'
      '\t\t\t\tlist(FILTER emusuni_sources EXCLUDE REGEX "(^|/)Core/(MIPS/JitCommon/|Util/DisArm64)")\n'
      '\t\t\t\t# Two files that call code the libretro core doesn\'t have (the desktop UI\'s NoticeView; a\n'
      '\t\t\t\t# SaveZIM of libretro\'s file type): unused there, upstream\'s linker leaves them out.\n'
      '\t\t\t\tlist(FILTER emusuni_sources EXCLUDE REGEX "(^|/)Common/(UI/PopupScreens|Render/AtlasGen)\\\\.")\n'
      '\t\t\t\tlist(APPEND emusuni_sources Common/GPU/OpenGL/GLFeatures.cpp Common/GPU/OpenGL/GLFeatures.h)\n'
      '\t\t\tendif()\n'
      '\t\t\tset_property(TARGET ${emusuni_target} PROPERTY SOURCES ${emusuni_sources})\n'
      '\t\tendif()\n'
      '\tendforeach()\n'
      'endif()\n\n'
      'if(LIBRETRO)\n\tadd_subdirectory(libretro)\nendif()\n')
# The PS4's CMake system is "Generic" (OpenOrbis' toolchain): the POSIX versions of glslang's OS
# layer and of aemu_postoffice's sockets (upstream picks them only for UNIX/Linux/BSD...).
patch('glslang/CMakeLists.txt',
      'elseif(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia")\n    add_subdirectory(OSDependent/Unix)\n',
      'elseif(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n'
      '    add_subdirectory(OSDependent/Unix)\n',
      repo=root + '/ext/glslang')
patch('CMakeLists.txt',
      'if(LINUX OR ANDROID OR MACOSX OR IOS OR BSD)\n\tlist(APPEND aemu_postoffice\n',
      'if(LINUX OR ANDROID OR MACOSX OR IOS OR BSD OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n\tlist(APPEND aemu_postoffice\n')
patch('ext/zstd-build/CMakeLists.txt',
      'add_library(libzstd_static STATIC ${ALL_SOURCE_FILES})\n',
      'if(EMUSUNI_PORT)\n'
      '\t# EmuSuNi: no dictionary builder (PPSSPP never trains zstd dictionaries; it needs qsort_r).\n'
      '\tlist(FILTER ALL_SOURCE_FILES EXCLUDE REGEX "/dictBuilder/")\n'
      'endif()\n'
      'add_library(libzstd_static STATIC ${ALL_SOURCE_FILES})\n')
patch('ext/CMakeLists.txt',
      'if(NOT USING_GLES2)\n\tadd_subdirectory(glew)\nendif()\n',
      'if(NOT USING_GLES2 AND NOT EMUSUNI_PORT)  # EmuSuNi: no OpenGL\n\tadd_subdirectory(glew)\nendif()\n')
patch('libretro/CMakeLists.txt',
      'set(LIBRETRO_SRCS\n\tlibretro.cpp\n\tLibretroGraphicsContext.cpp\n\tLibretroGLContext.cpp\n'
      '\tLibretroGLCoreContext.cpp\n\tLibretroVulkanContext.cpp\n\tlibretro_vulkan.cpp)\n',
      'if(EMUSUNI_PORT)\n'
      '\t# EmuSuNi: Vulkan only (no OpenGL contexts).\n'
      '\tset(LIBRETRO_SRCS\n\t\tlibretro.cpp\n\t\tLibretroGraphicsContext.cpp\n\t\tLibretroVulkanContext.cpp\n'
      '\t\tlibretro_vulkan.cpp)\n'
      'else()\n'
      '\tset(LIBRETRO_SRCS\n\t\tlibretro.cpp\n\t\tLibretroGraphicsContext.cpp\n\t\tLibretroGLContext.cpp\n'
      '\t\tLibretroGLCoreContext.cpp\n\t\tLibretroVulkanContext.cpp\n\t\tlibretro_vulkan.cpp)\n'
      'endif()\n')
patch('libretro/CMakeLists.txt',
      'add_library(ppsspp_libretro SHARED ${LIBRETRO_SRCS})\n',
      'if(EMUSUNI_PORT)\n'
      '\t# EmuSuNi: linked into the app (scripts/build-psp-core.sh makes one object of it).\n'
      '\tadd_library(ppsspp_libretro STATIC ${LIBRETRO_SRCS})\n'
      'else()\n'
      '\tadd_library(ppsspp_libretro SHARED ${LIBRETRO_SRCS})\n'
      'endif()\n')
patch('ppsspp_config.h',
      '#if !PPSSPP_PLATFORM(WINDOWS) || ((!PPSSPP_ARCH(ARM) && !PPSSPP_ARCH(ARM64)) && !PPSSPP_PLATFORM(UWP))\n'
      '#define PPSSPP_API_ANY_GL 1\n#endif\n',
      '// EmuSuNi: Vulkan only (scripts/build-psp-core.sh).\n'
      '#if !defined(EMUSUNI_PORT) && (!PPSSPP_PLATFORM(WINDOWS) || ((!PPSSPP_ARCH(ARM) && !PPSSPP_ARCH(ARM64)) && !PPSSPP_PLATFORM(UWP)))\n'
      '#define PPSSPP_API_ANY_GL 1\n#endif\n')
patch('libretro/LibretroGraphicsContext.cpp',
      '#include "libretro/LibretroGLContext.h"\n#include "libretro/LibretroGLCoreContext.h"\n',
      '#ifndef EMUSUNI_PORT  // EmuSuNi: Vulkan only\n'
      '#include "libretro/LibretroGLContext.h"\n#include "libretro/LibretroGLCoreContext.h"\n#endif\n')
patch('libretro/LibretroGraphicsContext.cpp',
      '#ifndef USING_GLES2\n\tif (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_OPENGL_CORE) {\n',
      '#if !defined(USING_GLES2) && !defined(EMUSUNI_PORT)\n'
      '\tif (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_OPENGL_CORE) {\n')
patch('libretro/LibretroGraphicsContext.cpp',
      '\tif (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_OPENGL || preferred == RETRO_HW_CONTEXT_OPENGLES3) {\n'
      '\t\tctx = new LibretroGLContext();\n\n\t\tif (ctx->Init()) {\n\t\t\treturn ctx;\n\t\t}\n\t\tdelete ctx;\n\t}\n',
      '#ifndef EMUSUNI_PORT\n'
      '\tif (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_OPENGL || preferred == RETRO_HW_CONTEXT_OPENGLES3) {\n'
      '\t\tctx = new LibretroGLContext();\n\n\t\tif (ctx->Init()) {\n\t\t\treturn ctx;\n\t\t}\n\t\tdelete ctx;\n\t}\n'
      '#endif\n')

# --- The PSP's memory: masked addresses, the views in a reserved range, the fault handler for Fast Memory.
patch('Core/MemMap.h',
      '#if PPSSPP_ARCH(32BIT) || PPSSPP_PLATFORM(UWP) || USE_ASAN || PPSSPP_PLATFORM(IOS) || defined(__EMSCRIPTEN__)\n'
      '#define MASKED_PSP_MEMORY\n#endif\n',
      '// EmuSuNi: masked too (Common/MemArenaEmuSuNi.cpp maps only the views below 0x10000000).\n'
      '#if PPSSPP_ARCH(32BIT) || PPSSPP_PLATFORM(UWP) || USE_ASAN || PPSSPP_PLATFORM(IOS) || defined(__EMSCRIPTEN__) || defined(EMUSUNI_PORT)\n'
      '#define MASKED_PSP_MEMORY\n#endif\n')
patch('Common/MemArenaPosix.cpp',
      '#if !defined(_WIN32) && !defined(ANDROID) && !defined(__APPLE__) && !PPSSPP_PLATFORM(SWITCH)\n',
      '#if !defined(_WIN32) && !defined(ANDROID) && !defined(__APPLE__) && !PPSSPP_PLATFORM(SWITCH) && !defined(EMUSUNI_PORT)\n')
addfile('Common/MemArenaEmuSuNi.cpp', r'''// EmuSuNi (scripts/build-psp-core.sh): the PSP's memory map without shared memory objects
// (shm_open/memfd, which the PS4 doesn't have the normal way). With MASKED_PSP_MEMORY (Core/MemMap.h)
// every address is masked to 30 bits, so only the views below 0x10000000 are made: the scratchpad at
// 0x00010000, VRAM at 0x04000000 and its three mirrors, the RAM at 0x08000000. They are mapped in one
// reserved range of 1 GiB (every address the JIT can make), nothing else in it: with Fast Memory a
// stray access faults there and the core's handler skips it (Core/MemFault.cpp), as upstream's. A
// mirror is the same memory mapped again (the PS4: one block of direct memory; this PC: a memfd);
// if the system refuses a second mapping, the mirror gets memory of its own (upstream's NO_MMAP
// behaviour) and Memory::Init logs it.
#include "ppsspp_config.h"

#ifdef EMUSUNI_PORT

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#include "Common/Log.h"
#include "Common/MemArena.h"

#if defined(__ORBIS__)
#include <sys/types.h>
extern "C" {
int32_t sceKernelReserveVirtualRange(void **addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t alignment, int32_t type, off_t *phys);
int32_t sceKernelMapDirectMemory(void **addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t alignment);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
int32_t sceKernelMapNamedSystemFlexibleMemory(void **addr, size_t len, int32_t prot, int32_t flags, const char *name);
int32_t sceKernelMunmap(void *addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
}
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
constexpr size_t SPAN = 0x40000000;   // the masked map: 0x00000000-0x3FFFFFFF
constexpr size_t GRANULE = 0x10000;   // where views start in the backing memory (64 KiB)
uintptr_t g_base = 0;                 // the reserved range
size_t g_size = 0;                    // the backing memory
s64 g_mappedOffsets[32];              // offsets of the backing memory mapped once already
int g_mappedCount = 0;
#if defined(__ORBIS__)
constexpr int32_t PROT_CPU_RW = 0x03;   // SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE
constexpr int32_t MAP_FIXED_FLAG = 0x10;  // SCE_KERNEL_MAP_FIXED
constexpr int32_t WB_ONION = 0;           // SCE_KERNEL_WB_ONION: CPU write-back memory
constexpr size_t PAGE = 16384;
off_t g_phys = 0;
#else
int g_fd = -1;
#endif

bool MappedBefore(s64 offset) {
	for (int i = 0; i < g_mappedCount; i++) {
		if (g_mappedOffsets[i] == offset)
			return true;
	}
	return false;
}

void RememberMapped(s64 offset) {
	if (g_mappedCount < (int)(sizeof(g_mappedOffsets) / sizeof(g_mappedOffsets[0])))
		g_mappedOffsets[g_mappedCount++] = offset;
}
}  // namespace

size_t MemArena::roundup(size_t x) {
	return (x + GRANULE - 1) & ~(GRANULE - 1);
}

bool MemArena::NeedsProbing() {
	return false;
}

bool MemArena::GrabMemSpace(size_t size) {
	g_size = (size + GRANULE - 1) & ~(GRANULE - 1);
	g_mappedCount = 0;
#if defined(__ORBIS__)
	off_t phys = 0;
	const int32_t ret = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), g_size, GRANULE, WB_ONION, &phys);
	if (ret != 0) {
		ERROR_LOG(Log::MemMap, "EmuSuNi: no direct memory for the PSP's memory (%d KiB, error %08x)", (int)(g_size / 1024), (unsigned)ret);
		g_size = 0;
		return false;
	}
	g_phys = phys;
#else
	g_fd = -1;
	if (!getenv("EMUSUNI_PSP_NO_ALIAS")) {
		g_fd = memfd_create("ppsspp-psp-memory", MFD_CLOEXEC);
		if (g_fd < 0 || ftruncate(g_fd, (off_t)g_size) != 0) {
			ERROR_LOG(Log::MemMap, "EmuSuNi: memfd for the PSP's memory failed: %s", strerror(errno));
			if (g_fd >= 0)
				close(g_fd);
			g_fd = -1;
			g_size = 0;
			return false;
		}
	}
#endif
	return true;
}

void MemArena::ReleaseSpace() {
#if defined(__ORBIS__)
	if (g_base)
		sceKernelMunmap((void *)g_base, SPAN);
	if (g_size)
		sceKernelReleaseDirectMemory(g_phys, g_size);
#else
	if (g_base)
		munmap((void *)g_base, SPAN);
	if (g_fd >= 0)
		close(g_fd);
	g_fd = -1;
#endif
	g_base = 0;
	g_size = 0;
	g_mappedCount = 0;
}

void *MemArena::CreateView(s64 offset, size_t size, void *base) {
	const bool mirror = MappedBefore(offset);
#if defined(__ORBIS__)
	void *at = base;
	int32_t ret = sceKernelMapDirectMemory(&at, size, PROT_CPU_RW, MAP_FIXED_FLAG, g_phys + offset, PAGE);
	if (ret == 0 && at == base) {
		if (!mirror) {
			memset(base, 0, size);  // direct memory comes as it was left
			RememberMapped(offset);
		}
		return base;
	}
	if (!mirror) {
		ERROR_LOG(Log::MemMap, "EmuSuNi: mapping %d KiB of the PSP's memory at %p failed (%08x)", (int)(size / 1024), base, (unsigned)ret);
		return nullptr;
	}
	// A mirror the system won't map again: memory of its own.
	at = base;
	ret = sceKernelMapNamedSystemFlexibleMemory(&at, size, PROT_CPU_RW, MAP_FIXED_FLAG, "ppsspp mirror");
	if (ret != 0 || at != base) {
		ERROR_LOG(Log::MemMap, "EmuSuNi: no memory for a %d KiB mirror at %p (%08x)", (int)(size / 1024), base, (unsigned)ret);
		return nullptr;
	}
	memset(base, 0, size);
	return base;
#else
	void *at;
	if (g_fd >= 0)
		at = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_fd, offset);
	else  // EMUSUNI_PSP_NO_ALIAS: every view memory of its own, as the PS4's fallback
		at = mmap(base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (at != base) {
		ERROR_LOG(Log::MemMap, "EmuSuNi: mapping %d KiB of the PSP's memory at %p failed: %s", (int)(size / 1024), base, strerror(errno));
		return nullptr;
	}
	if (!mirror)
		RememberMapped(offset);
	return base;
#endif
}

void MemArena::ReleaseView(s64 offset, void *view, size_t size) {
#if defined(__ORBIS__)
	sceKernelMunmap(view, size);
#else
	// Back to reserved address space.
	mmap(view, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
#endif
}

u8 *MemArena::Find4GBBase() {
#if defined(__ORBIS__)
	// Well above 0x200000000, where the PS4's kernel library puts its own objects (as the app's heap).
	static const uintptr_t hints[] = {0x3000000000ull, 0x2800000000ull, 0x2000000000ull, 0};
	for (uintptr_t hint : hints) {
		void *at = (void *)hint;
		if (sceKernelReserveVirtualRange(&at, SPAN, 0, GRANULE) != 0)
			continue;
		if ((uintptr_t)at >= 0x400000000ull) {
			g_base = (uintptr_t)at;
			return (u8 *)at;
		}
		sceKernelMunmap(at, SPAN);
	}
	ERROR_LOG(Log::MemMap, "EmuSuNi: no 1 GiB of address space for the PSP's memory");
	return nullptr;
#else
	void *at = mmap(nullptr, SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (at == MAP_FAILED) {
		ERROR_LOG(Log::MemMap, "EmuSuNi: no 1 GiB of address space for the PSP's memory: %s", strerror(errno));
		return nullptr;
	}
	g_base = (uintptr_t)at;
	return (u8 *)at;
#endif
}

#endif  // EMUSUNI_PORT
''')
patch('Core/MemMap.cpp',
      '\tif (!MemoryMap_Setup(flags)) {\n\t\treturn false;\n\t}\n',
      '\tif (!MemoryMap_Setup(flags)) {\n\t\treturn false;\n\t}\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t{\n'
      '\t\t// EmuSuNi (Common/MemArenaEmuSuNi.cpp): are the VRAM mirrors the same memory?\n'
      '\t\tvolatile u32 *vram = (volatile u32 *)(base + 0x04000000);\n'
      '\t\tvolatile u32 *mirror = (volatile u32 *)(base + 0x04600000);\n'
      '\t\tconst u32 old = *vram;\n'
      '\t\t*vram = 0x45535550;\n'
      '\t\tconst bool aliased = *mirror == 0x45535550;\n'
      '\t\t*vram = old;\n'
      '\t\tINFO_LOG(Log::MemMap, "EmuSuNi: PSP memory map at %p, %u MiB of RAM, VRAM mirrors %s", base,\n'
      '\t\t\t(unsigned)(g_MemorySize >> 20), aliased ? "aliased" : "NOT aliased (memory of their own, as upstream\'s NO_MMAP build)");\n'
      '\t}\n'
      '#endif\n')
patch('Core/System.cpp',
      '\tInstallExceptionHandler(&Memory::HandleFault);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the core\'s fault handler only with Fast Memory (its stray accesses are skipped, as\n'
      '\t// upstream does); any other fault goes on to the handler before it: the app\'s crash log.\n'
      '\tif (g_Config.bFastMemory)\n'
      '#endif\n'
      '\tInstallExceptionHandler(&Memory::HandleFault);\n')
patch('libretro/libretro.cpp',
      '         g_Config.bFastMemory = true;\n   }\n',
      '         g_Config.bFastMemory = true;\n   }\n'
      '#ifdef EMUSUNI_PORT\n'
      '   {\n'
      '      // EmuSuNi: Fast Memory as the option said when the game started (the JIT and the fault\n'
      '      // handler are set up for it then); EMUSUNI_PSP_FASTMEM=0/1 overrides the option.\n'
      '      static bool latched = false;\n'
      '      if (const char *fast = getenv("EMUSUNI_PSP_FASTMEM"))\n'
      '         g_Config.bFastMemory = atoi(fast) != 0;\n'
      '      if (Libretro::g_pendingBoot || PSP_IsInited())\n'
      '         g_Config.bFastMemory = latched;\n'
      '      else\n'
      '         latched = g_Config.bFastMemory;\n'
      '   }\n'
      '#endif\n')
# The fault handler on the PS4: its own, with the FreeBSD kernel's numbers and layouts written out
# (as port/ps4_crashlog.cpp has them: SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, si_addr 24 bytes into
# the siginfo, FreeBSD's mcontext 0x40 bytes into the context - measured on hardware - with rip its
# 21st 8-byte slot), whatever a header says; the struct sigaction handed to the kernel and the
# mcontext HandleFault reads are checked to be FreeBSD's when this compiles (the PSChrome libc
# overlay's headers, which come before OpenOrbis' musl ones in the PS4's include path). No alternate
# signal stack (as DolphinPS4's port of Dolphin's handler). A fault that isn't the JIT's stray access
# into the PSP's 1 GiB goes on to the handler before (the app's crash log), as the kernel would.
patch('Common/ExceptionHandlerSetup.cpp',
      '\t\told_sa->sa_handler(sig);\n\t}\n}\n\nvoid InstallExceptionHandler(BadAccessHandler badAccessHandler) {\n',
      '\t\told_sa->sa_handler(sig);\n\t}\n}\n\n'
      '#if defined(EMUSUNI_PORT) && defined(__ORBIS__)\n'
      '// EmuSuNi (scripts/build-psp-core.sh): the PS4 kernel\'s (FreeBSD\'s) values, written out.\n'
      '#include <cstddef>\n'
      'static constexpr int kFbsdSigBus = 10, kFbsdSigSegv = 11, kFbsdSaSiginfo = 0x40;\n'
      'static constexpr size_t kFbsdSiAddr = 24, kFbsdMcontext = 0x40;\n'
      'static_assert(offsetof(struct sigaction, sa_flags) == 8 && sizeof(sigset_t) == 16,\n'
      '\t"struct sigaction isn\'t FreeBSD\'s: the PSChrome libc overlay\'s headers must come first");\n'
      'static_assert(offsetof(SContext, CTX_RIP) == 20 * 8, "SContext isn\'t FreeBSD\'s mcontext (rip: slot 20)");\n\n'
      'static void emusuni_ps4_fault_handler(int sig, siginfo_t *info, void *raw_context) {\n'
      '\tif (sig != kFbsdSigSegv && sig != kFbsdSigBus)\n'
      '\t\treturn;\n'
      '\tconst uintptr_t bad_address = *(const uintptr_t *)((const char *)info + kFbsdSiAddr);\n'
      '\tSContext *ctx = (SContext *)((char *)raw_context + kFbsdMcontext);\n'
      '\tif (g_badAccessHandler && g_badAccessHandler(bad_address, ctx))\n'
      '\t\treturn;  // the JIT\'s stray access, skipped (its rip moved on in ctx)\n'
      '\t// Not ours: the handler before (the app\'s crash log), as the kernel would have called it.\n'
      '\tstruct sigaction *old_sa = sig == kFbsdSigSegv ? &old_sa_segv : &old_sa_bus;\n'
      '\tif (old_sa->sa_flags & kFbsdSaSiginfo) {\n'
      '\t\told_sa->sa_sigaction(sig, info, raw_context);\n'
      '\t\treturn;\n'
      '\t}\n'
      '\tif (old_sa->sa_handler == SIG_IGN)\n'
      '\t\treturn;\n'
      '\tif (old_sa->sa_handler == SIG_DFL) {\n'
      '\t\t// The default action: back to it, and the faulting instruction runs again.\n'
      '\t\tstruct sigaction dfl{};\n'
      '\t\tsigaction(sig, &dfl, nullptr);\n'
      '\t\treturn;\n'
      '\t}\n'
      '\told_sa->sa_handler(sig);\n'
      '}\n'
      '#endif\n\n'
      'void InstallExceptionHandler(BadAccessHandler badAccessHandler) {\n')
patch('Common/ExceptionHandlerSetup.cpp',
      '\tsize_t altStackSize = SIGSTKSZ;\n',
      '#if defined(EMUSUNI_PORT) && defined(__ORBIS__)\n'
      '\t// EmuSuNi: no alternate signal stack on the PS4 (the JIT\'s faults don\'t need one); SIGBUS too.\n'
      '\tINFO_LOG(Log::System, "EmuSuNi: fault handler for Fast Memory installed");\n'
      '\tg_badAccessHandler = badAccessHandler;\n'
      '\t{\n'
      '\t\tstruct sigaction sa{};  // (an empty mask)\n'
      '\t\tsa.sa_sigaction = &emusuni_ps4_fault_handler;\n'
      '\t\tsa.sa_flags = kFbsdSaSiginfo;\n'
      '\t\tsigaction(kFbsdSigSegv, &sa, &old_sa_segv);\n'
      '\t\tsigaction(kFbsdSigBus, &sa, &old_sa_bus);\n'
      '\t}\n'
      '\treturn;\n'
      '#endif\n'
      '\tsize_t altStackSize = SIGSTKSZ;\n')
patch('Common/ExceptionHandlerSetup.cpp',
      'void UninstallExceptionHandler() {\n\tif (!g_badAccessHandler) {\n\t\treturn;\n\t}\n',
      'void UninstallExceptionHandler() {\n\tif (!g_badAccessHandler) {\n\t\treturn;\n\t}\n'
      '#if defined(EMUSUNI_PORT) && defined(__ORBIS__)\n'
      '\t// EmuSuNi: the handlers before (the app\'s crash log) back.\n'
      '\tsigaction(kFbsdSigSegv, &old_sa_segv, nullptr);\n'
      '\tsigaction(kFbsdSigBus, &old_sa_bus, nullptr);\n'
      '\tg_badAccessHandler = nullptr;\n'
      '\treturn;\n'
      '#endif\n')
patch('libretro/libretro.cpp',
      '         g_Config.bCacheFullIsoInRam = true;\n   }\n',
      '         g_Config.bCacheFullIsoInRam = true;\n   }\n'
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: games of up to 1.8 GB are read from the USB drive, never held whole on the heap.\n'
      '   g_Config.bCacheFullIsoInRam = false;\n'
      '#endif\n')
patch('libretro/libretro.cpp',
      '         g_Config.bRenderDuplicateFrames = true;\n   }\n',
      '         g_Config.bRenderDuplicateFrames = true;\n   }\n'
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: one retro_run per vblank, always (the app paces by the display: without this a\n'
      '   // 30 fps game ends a frame every second vblank and would run twice as fast).\n'
      '   g_Config.bRenderDuplicateFrames = true;\n'
      '#endif\n')

# --- The JIT's code memory: one array in the core's data, executable through EmuSuNi, else no JIT.
patch('Common/MemoryUtil.h',
      'bool PlatformIsWXExclusive();\n',
      'bool PlatformIsWXExclusive();\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (MemoryUtil.cpp): whether the JIT\'s code memory could be made executable (tried once).\n'
      'bool EmuSuNiJitAvailable();\n'
      '#endif\n')
patch('Common/MemoryUtil.cpp',
      'static int hint_location;\n',
      '#ifdef EMUSUNI_PORT\n'
      '#include <iterator>\n#include <map>\n#include <mutex>\n\n'
      '// EmuSuNi (port/ps4_n64.c): makes [address, address + size) executable the ways the PS4 allows,\n'
      '// each checked by running a small function there; the way that worked (0-3) or -1.\n'
      'extern "C" int emusuni_make_executable(void *address, size_t size);\n\n'
      '// EmuSuNi: the JIT\'s code memory is one array in the core\'s data (the x86-64 JIT reaches the core\'s\n'
      '// code and data with 32-bit offsets), made executable once through EmuSuNi instead of mmap and\n'
      '// mprotect, on 16 KiB pages of its own (the PS4\'s). It is shared out to the code blocks (the MIPS\n'
      '// JIT 16 MiB, its thunks 1 MiB and safe-memory functions 512 KiB, the vertex decoder JIT 256 KiB,\n'
      '// the software renderer 2 x 256 KiB); a freed block is used again. If it can\'t be made executable,\n'
      '// SYSPROP_CAN_JIT is false (libretro/libretro.cpp): the IR interpreter runs instead.\n'
      'namespace {\n'
      'constexpr size_t EMUSUNI_JIT_SIZE = 22 * 1024 * 1024;\n'
      'constexpr size_t EMUSUNI_JIT_PAGE = 16384;\n'
      'alignas(16384) uint8_t g_emusuniJitMemory[EMUSUNI_JIT_SIZE];\n'
      'std::mutex g_emusuniJitLock;\n'
      'int g_emusuniJitState = 0;  // 0: not tried yet, 1: executable, -1: not possible\n'
      'std::map<size_t, size_t> g_emusuniJitFree;  // offset -> size of each free block\n'
      '}  // namespace\n\n'
      'bool EmuSuNiJitAvailable() {\n'
      '\tstd::lock_guard<std::mutex> guard(g_emusuniJitLock);\n'
      '\tif (g_emusuniJitState == 0) {\n'
      '\t\tconst int way = emusuni_make_executable(g_emusuniJitMemory, EMUSUNI_JIT_SIZE);\n'
      '\t\tg_emusuniJitState = way >= 0 ? 1 : -1;\n'
      '\t\tif (way >= 0) {\n'
      '\t\t\tg_emusuniJitFree[0] = EMUSUNI_JIT_SIZE;\n'
      '\t\t\tINFO_LOG(Log::JIT, "EmuSuNi: %d MiB of executable JIT memory at %p (way %d)", (int)(EMUSUNI_JIT_SIZE >> 20), (void *)g_emusuniJitMemory, way);\n'
      '\t\t} else {\n'
      '\t\t\tERROR_LOG(Log::JIT, "EmuSuNi: the JIT memory can\'t be made executable: the IR interpreter runs instead");\n'
      '\t\t}\n'
      '\t}\n'
      '\treturn g_emusuniJitState > 0;\n'
      '}\n\n'
      'static bool EmuSuNiInJitMemory(const void *ptr) {\n'
      '\treturn (const uint8_t *)ptr >= g_emusuniJitMemory && (const uint8_t *)ptr < g_emusuniJitMemory + EMUSUNI_JIT_SIZE;\n'
      '}\n'
      '#endif\n\n'
      'static int hint_location;\n')
patch('Common/MemoryUtil.cpp',
      'void *AllocateExecutableMemory(size_t size) {\n\tstatic char *map_hint = nullptr;\n',
      'void *AllocateExecutableMemory(size_t size) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the first free block of the JIT\'s array that is big enough.\n'
      '\tif (!EmuSuNiJitAvailable()) {\n'
      '\t\tERROR_LOG(Log::MemMap, "EmuSuNi: no executable memory for %d bytes of code", (int)size);\n'
      '\t\treturn nullptr;\n'
      '\t}\n'
      '\t{\n'
      '\t\tconst size_t want = (size + EMUSUNI_JIT_PAGE - 1) & ~(EMUSUNI_JIT_PAGE - 1);\n'
      '\t\tstd::lock_guard<std::mutex> guard(g_emusuniJitLock);\n'
      '\t\tfor (auto it = g_emusuniJitFree.begin(); it != g_emusuniJitFree.end(); ++it) {\n'
      '\t\t\tif (it->second < want)\n'
      '\t\t\t\tcontinue;\n'
      '\t\t\tconst size_t offset = it->first, left = it->second - want;\n'
      '\t\t\tg_emusuniJitFree.erase(it);\n'
      '\t\t\tif (left)\n'
      '\t\t\t\tg_emusuniJitFree[offset + want] = left;\n'
      '\t\t\treturn g_emusuniJitMemory + offset;\n'
      '\t\t}\n'
      '\t}\n'
      '\tERROR_LOG(Log::MemMap, "EmuSuNi: the JIT memory is full (%d bytes wanted)", (int)size);\n'
      '\treturn nullptr;\n'
      '#endif\n'
      '\tstatic char *map_hint = nullptr;\n')
patch('Common/MemoryUtil.cpp',
      'void FreeExecutableMemory(void *ptr, size_t size) {\n\tFreeMemoryPages(ptr, size);\n}\n',
      'void FreeExecutableMemory(void *ptr, size_t size) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (ptr && EmuSuNiInJitMemory(ptr)) {\n'
      '\t\t// EmuSuNi: back to the JIT array\'s free blocks, merged with its neighbours.\n'
      '\t\tstd::lock_guard<std::mutex> guard(g_emusuniJitLock);\n'
      '\t\tsize_t offset = (const uint8_t *)ptr - g_emusuniJitMemory;\n'
      '\t\tsize_t length = (size + EMUSUNI_JIT_PAGE - 1) & ~(EMUSUNI_JIT_PAGE - 1);\n'
      '\t\tauto next = g_emusuniJitFree.lower_bound(offset);\n'
      '\t\tif (next != g_emusuniJitFree.end() && offset + length == next->first) {\n'
      '\t\t\tlength += next->second;\n'
      '\t\t\tnext = g_emusuniJitFree.erase(next);\n'
      '\t\t}\n'
      '\t\tif (next != g_emusuniJitFree.begin()) {\n'
      '\t\t\tauto prev = std::prev(next);\n'
      '\t\t\tif (prev->first + prev->second == offset) {\n'
      '\t\t\t\tprev->second += length;\n'
      '\t\t\t\treturn;\n'
      '\t\t\t}\n'
      '\t\t}\n'
      '\t\tg_emusuniJitFree[offset] = length;\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n'
      '\tFreeMemoryPages(ptr, size);\n}\n')
patch('Common/MemoryUtil.cpp',
      'bool ProtectMemoryPages(const void* ptr, size_t size, uint32_t memProtFlags) {\n',
      'bool ProtectMemoryPages(const void* ptr, size_t size, uint32_t memProtFlags) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (EmuSuNiInJitMemory(ptr))\n'
      '\t\treturn true;  // EmuSuNi: the JIT\'s array stays read/write/execute\n'
      '#endif\n')
patch('libretro/libretro.cpp',
      '#include "Common/StringUtils.h"\n',
      '#include "Common/StringUtils.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "Common/MemoryUtil.h"\n#endif\n')
patch('libretro/libretro.cpp',
      '   var.key = "ppsspp_fast_memory";\n',
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: no executable memory (Common/MemoryUtil.cpp): no JIT anywhere.\n'
      '   if (!EmuSuNiJitAvailable())\n'
      '      g_Config.bSoftwareRenderingJit = false;\n'
      '#endif\n\n'
      '   var.key = "ppsspp_fast_memory";\n')
patch('libretro/libretro.cpp',
      '   case SYSPROP_CAN_JIT:\n#if PPSSPP_PLATFORM(IOS)\n',
      '   case SYSPROP_CAN_JIT:\n'
      '#if defined(EMUSUNI_PORT)\n'
      '      return EmuSuNiJitAvailable();  // EmuSuNi: whether the JIT memory could be made executable\n'
      '#elif PPSSPP_PLATFORM(IOS)\n')
patch('Common/CPUDetect.cpp',
      '#if defined(CPU_FEATURES_OS_FREEBSD) || defined(CPU_FEATURES_OS_LINUX) || defined(CPU_FEATURES_OS_ANDROID) || defined(CPU_FEATURES_OS_MACOS) || defined(CPU_FEATURES_OS_WINDOWS)\n'
      '#define USE_CPU_FEATURES 1\n',
      '// EmuSuNi: PPSSPP\'s own CPUID code (cpu_features\' FreeBSD support reads /var/run/dmesg.boot).\n'
      '#if !defined(EMUSUNI_PORT) && (defined(CPU_FEATURES_OS_FREEBSD) || defined(CPU_FEATURES_OS_LINUX) || defined(CPU_FEATURES_OS_ANDROID) || defined(CPU_FEATURES_OS_MACOS) || defined(CPU_FEATURES_OS_WINDOWS))\n'
      '#define USE_CPU_FEATURES 1\n')
patch('Common/CPUDetect.cpp',
      '#ifdef _M_SSE\n\n#include <emmintrin.h>\n\nstatic uint64_t do_xgetbv(unsigned int index) {\n',
      '#if defined(_M_SSE) || defined(EMUSUNI_PORT)  // EmuSuNi: the CPUID path above needs it\n\n'
      '#include <emmintrin.h>\n\nstatic uint64_t do_xgetbv(unsigned int index) {\n')

# --- Vulkan: no library to load; the frontend's device adopted; surface and swapchain emulated.
patch('Common/GPU/Vulkan/VulkanLoader.cpp',
      '#if !PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(SWITCH)\n#include <dlfcn.h>\n#endif\n',
      '#if !PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(SWITCH) && !defined(EMUSUNI_PORT)\n#include <dlfcn.h>\n#endif\n')
patch('Common/GPU/Vulkan/VulkanLoader.cpp',
      '#if !PPSSPP_PLATFORM(IOS_APP_STORE)\nstatic VulkanLibraryHandle VulkanLoadLibrary(std::string *errorString) {\n',
      '#if !PPSSPP_PLATFORM(IOS_APP_STORE) && !defined(EMUSUNI_PORT)\nstatic VulkanLibraryHandle VulkanLoadLibrary(std::string *errorString) {\n')
patch('Common/GPU/Vulkan/VulkanLoader.cpp',
      '#if !PPSSPP_PLATFORM(IOS_APP_STORE)\nstatic void VulkanFreeLibrary(VulkanLibraryHandle &h) {\n',
      '#if !PPSSPP_PLATFORM(IOS_APP_STORE) && !defined(EMUSUNI_PORT)\nstatic void VulkanFreeLibrary(VulkanLibraryHandle &h) {\n')
patch('Common/GPU/Vulkan/VulkanLoader.cpp',
      'bool VulkanMayBeAvailable() {\n#if PPSSPP_PLATFORM(IOS)\n',
      'bool VulkanMayBeAvailable() {\n'
      '#if defined(EMUSUNI_PORT)\n'
      '\t// EmuSuNi: the frontend\'s Vulkan (its vkGetInstanceProcAddr), nothing to load or try.\n'
      '\tg_vulkanAvailabilityChecked = true;\n'
      '\tg_vulkanMayBeAvailable = true;\n'
      '\treturn true;\n'
      '#elif PPSSPP_PLATFORM(IOS)\n')
patch('Common/GPU/Vulkan/VulkanLoader.cpp',
      'bool VulkanLoad(std::string *errorStr) {\n#if PPSSPP_PLATFORM(IOS_APP_STORE)\n',
      'bool VulkanLoad(std::string *errorStr) {\n#if PPSSPP_PLATFORM(IOS_APP_STORE) || defined(EMUSUNI_PORT)\n')
patch('Common/GPU/Vulkan/VulkanLoader.cpp',
      'void VulkanFree() {\n#if !PPSSPP_PLATFORM(IOS_APP_STORE)\n',
      'void VulkanFree() {\n#if !PPSSPP_PLATFORM(IOS_APP_STORE) && !defined(EMUSUNI_PORT)\n')
patch('Common/GPU/Vulkan/VulkanContext.h',
      '#ifdef VK_USE_PLATFORM_DISPLAY_KHR\n\tWINDOWSYSTEM_DISPLAY,\n#endif\n};\n',
      '#ifdef VK_USE_PLATFORM_DISPLAY_KHR\n\tWINDOWSYSTEM_DISPLAY,\n#endif\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tWINDOWSYSTEM_EMUSUNI,  // EmuSuNi: no window system: the surface (data1) is libretro_vulkan.cpp\'s emulated one\n'
      '#endif\n};\n')
patch('Common/GPU/Vulkan/VulkanContext.cpp',
      '\tswitch (winsys_) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tif (winsys_ == WINDOWSYSTEM_EMUSUNI)\n'
      '\t\tsurface_ = (VkSurfaceKHR)winsysData1_;  // EmuSuNi: never given to the driver (libretro_vulkan.cpp)\n'
      '\telse\n'
      '#endif\n'
      '\tswitch (winsys_) {\n')
patch('libretro/libretro_vulkan.cpp',
      'static VkSwapchainKHR_T chain;\n',
      'static VkSwapchainKHR_T chain;\n'
      r'''
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

// EmuSuNi: the frontend's own device, adopted. EmuSuNi (src/Display.cpp) makes its only VkDevice
// when it starts and never calls the negotiation interface's create_device: the instance, GPU,
// device and queue come from GET_HW_RENDER_INTERFACE (vk_libretro_adopt). EmuSuNi's instance has
// no extension (no VK_KHR_surface) and its device no swapchain: the surface functions are answered
// here and the swapchain is upstream's emulated one below. What the VulkanContext sees of the GPU
// is what that device was made with: the Vulkan version (EMUSUNI_VK_API_VERSION, "1.1" by default:
// EmuSuNi's instance), the device extensions in EMUSUNI_VK_DEVICE_EXTENSIONS and the features in
// EMUSUNI_VK_DEVICE_FEATURES (VkPhysicalDeviceFeatures names, and multiview, provokingVertexLast,
// scalarBlockLayout for the extension features), comma-separated; none of them when unset.
static struct {
	bool active = false;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	uint32_t api_version = VK_API_VERSION_1_1;
	std::string features;
	std::string extensions;
} adopted;

static bool EmuSuNiListed(const std::string &list, const char *name) {
	const size_t length = strlen(name);
	for (size_t at = list.find(name); at != std::string::npos; at = list.find(name, at + 1)) {
		if ((at == 0 || list[at - 1] == ',') && (at + length == list.size() || list[at + length] == ','))
			return true;
	}
	return false;
}

static const char *const EMUSUNI_FEATURE_NAMES[] = {
	"robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
	"tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
	"drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds",
	"wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy",
	"textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise",
	"pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics",
	"shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended", "shaderStorageImageExtendedFormats",
	"shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
	"shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
	"shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
	"shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
	"shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
	"sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples",
	"sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries",
};
static_assert(sizeof(EMUSUNI_FEATURE_NAMES) / sizeof(EMUSUNI_FEATURE_NAMES[0]) == sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32),
	"VkPhysicalDeviceFeatures has another layout");

// The features the device has AND was made with.
static void EmuSuNiMaskFeatures(VkPhysicalDeviceFeatures *features) {
	VkBool32 *bits = (VkBool32 *)features;
	for (size_t i = 0; i < sizeof(EMUSUNI_FEATURE_NAMES) / sizeof(EMUSUNI_FEATURE_NAMES[0]); i++) {
		if (bits[i] && !EmuSuNiListed(adopted.features, EMUSUNI_FEATURE_NAMES[i]))
			bits[i] = VK_FALSE;
	}
}

static uint32_t EmuSuNiApiVersion(uint32_t version) {
	return (version & 0xFFFFF000u) > (adopted.api_version & 0xFFFFF000u) ? adopted.api_version : version;
}

static PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures_emusuni_org;
static PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2_emusuni_org;
static PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties_emusuni_org;
static PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2_emusuni_org;
static PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties_emusuni_org;
static PFN_vkGetDeviceQueue vkGetDeviceQueue_emusuni_org;
static PFN_vkDeviceWaitIdle vkDeviceWaitIdle_emusuni_org;

static VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures_emusuni(VkPhysicalDevice gpu, VkPhysicalDeviceFeatures *features) {
	vkGetPhysicalDeviceFeatures_emusuni_org(gpu, features);
	EmuSuNiMaskFeatures(features);
}

static VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2_emusuni(VkPhysicalDevice gpu, VkPhysicalDeviceFeatures2 *features) {
	vkGetPhysicalDeviceFeatures2_emusuni_org(gpu, features);
	EmuSuNiMaskFeatures(&features->features);
	for (VkBaseOutStructure *next = (VkBaseOutStructure *)features->pNext; next; next = next->pNext) {
		switch (next->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
			VkPhysicalDeviceMultiviewFeatures *f = (VkPhysicalDeviceMultiviewFeatures *)next;
			if (!EmuSuNiListed(adopted.features, "multiview"))
				f->multiview = f->multiviewGeometryShader = f->multiviewTessellationShader = VK_FALSE;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT: {
			VkPhysicalDeviceProvokingVertexFeaturesEXT *f = (VkPhysicalDeviceProvokingVertexFeaturesEXT *)next;
			if (!EmuSuNiListed(adopted.features, "provokingVertexLast"))
				f->provokingVertexLast = VK_FALSE;
			f->transformFeedbackPreservesProvokingVertex = VK_FALSE;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES: {
			VkPhysicalDeviceScalarBlockLayoutFeatures *f = (VkPhysicalDeviceScalarBlockLayoutFeatures *)next;
			if (!EmuSuNiListed(adopted.features, "scalarBlockLayout"))
				f->scalarBlockLayout = VK_FALSE;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
			((VkPhysicalDevicePresentWaitFeaturesKHR *)next)->presentWait = VK_FALSE;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
			((VkPhysicalDevicePresentIdFeaturesKHR *)next)->presentId = VK_FALSE;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR:
			((VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR *)next)->presentModeFifoLatestReady = VK_FALSE;
			break;
		default:
			break;
		}
	}
}

static VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties_emusuni(VkPhysicalDevice gpu, VkPhysicalDeviceProperties *properties) {
	vkGetPhysicalDeviceProperties_emusuni_org(gpu, properties);
	properties->apiVersion = EmuSuNiApiVersion(properties->apiVersion);
}

static VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2_emusuni(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2 *properties) {
	vkGetPhysicalDeviceProperties2_emusuni_org(gpu, properties);
	properties->properties.apiVersion = EmuSuNiApiVersion(properties->properties.apiVersion);
}

// Only the extensions the device was made with.
static VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties_emusuni(VkPhysicalDevice gpu, const char *layer, uint32_t *count, VkExtensionProperties *properties) {
	if (layer) {
		*count = 0;
		return VK_SUCCESS;
	}
	uint32_t all = 0;
	VkResult res = vkEnumerateDeviceExtensionProperties_emusuni_org(gpu, nullptr, &all, nullptr);
	if (res != VK_SUCCESS)
		return res;
	std::vector<VkExtensionProperties> list(all);
	res = vkEnumerateDeviceExtensionProperties_emusuni_org(gpu, nullptr, &all, list.data());
	if (res != VK_SUCCESS && res != VK_INCOMPLETE)
		return res;
	list.resize(all);
	list.erase(std::remove_if(list.begin(), list.end(), [](const VkExtensionProperties &e) {
		return !EmuSuNiListed(adopted.extensions, e.extensionName);
	}), list.end());
	if (!properties) {
		*count = (uint32_t)list.size();
		return VK_SUCCESS;
	}
	const uint32_t n = std::min(*count, (uint32_t)list.size());
	if (n)
		memcpy(properties, list.data(), n * sizeof(VkExtensionProperties));
	*count = n;
	return n < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion_emusuni(uint32_t *version) {
	*version = adopted.api_version;
	return VK_SUCCESS;
}

// The frontend's instance: only VK_KHR_surface, which VulkanContext insists on (answered here).
static VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties_emusuni(const char *layer, uint32_t *count, VkExtensionProperties *properties) {
	if (layer) {
		*count = 0;
		return VK_SUCCESS;
	}
	if (!properties) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	memset(properties, 0, sizeof(*properties));
	snprintf(properties->extensionName, sizeof(properties->extensionName), "%s", VK_KHR_SURFACE_EXTENSION_NAME);
	properties->specVersion = 25;
	*count = 1;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties_emusuni(uint32_t *count, VkLayerProperties *) {
	*count = 0;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR_emusuni(VkPhysicalDevice, uint32_t family, VkSurfaceKHR, VkBool32 *supported) {
	*supported = family == adopted.queue_family ? VK_TRUE : VK_FALSE;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR_emusuni(VkPhysicalDevice, VkSurfaceKHR, uint32_t *count, VkSurfaceFormatKHR *formats) {
	if (!formats) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	formats[0].format = VK_FORMAT_B8G8R8A8_UNORM;
	formats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	*count = 1;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR_emusuni(VkPhysicalDevice, VkSurfaceKHR, uint32_t *count, VkPresentModeKHR *modes) {
	if (!modes) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	modes[0] = VK_PRESENT_MODE_FIFO_KHR;
	*count = 1;
	return VK_SUCCESS;
}

// The frontend's queue (which may be another queue of the family than the first).
static VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue_emusuni(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
	if (device == adopted.device && family == adopted.queue_family && index == 0) {
		*queue = adopted.queue;
		return;
	}
	vkGetDeviceQueue_emusuni_org(device, family, index, queue);
}

// Every queue of the device is the frontend's too: under its lock, as vkQueueSubmit and vkQueueWaitIdle.
static VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle_emusuni(VkDevice device) {
	if (!vulkan)
		return vkDeviceWaitIdle_emusuni_org(device);
	vulkan->lock_queue(vulkan->handle);
	VkResult res = vkDeviceWaitIdle_emusuni_org(device);
	vulkan->unlock_queue(vulkan->handle);
	return res;
}

static VKAPI_ATTR void VKAPI_CALL EmuSuNiNoDriverFunction() {
	ERROR_LOG(Log::G3D, "EmuSuNi: a surface or swapchain function of the driver was called");
}

// The functions the adoption answers itself or wraps (fptr: the frontend's, possibly null), else null.
static PFN_vkVoidFunction EmuSuNiFunction(const char *name, PFN_vkVoidFunction fptr) {
#define EMUSUNI_OWN(x) if (!strcmp(name, #x)) return (PFN_vkVoidFunction)x##_emusuni;
#define EMUSUNI_WRAP(x, as) if (!strcmp(name, #as)) { if (!fptr) return nullptr; x##_emusuni_org = (PFN_##x)fptr; return (PFN_vkVoidFunction)x##_emusuni; }
	EMUSUNI_OWN(vkEnumerateInstanceVersion)
	EMUSUNI_OWN(vkEnumerateInstanceExtensionProperties)
	EMUSUNI_OWN(vkEnumerateInstanceLayerProperties)
	EMUSUNI_OWN(vkGetPhysicalDeviceSurfaceSupportKHR)
	EMUSUNI_OWN(vkGetPhysicalDeviceSurfaceFormatsKHR)
	EMUSUNI_OWN(vkGetPhysicalDeviceSurfacePresentModesKHR)
	EMUSUNI_WRAP(vkGetPhysicalDeviceFeatures, vkGetPhysicalDeviceFeatures)
	EMUSUNI_WRAP(vkGetPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2)
	EMUSUNI_WRAP(vkGetPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2KHR)
	EMUSUNI_WRAP(vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties)
	EMUSUNI_WRAP(vkGetPhysicalDeviceProperties2, vkGetPhysicalDeviceProperties2)
	EMUSUNI_WRAP(vkGetPhysicalDeviceProperties2, vkGetPhysicalDeviceProperties2KHR)
	EMUSUNI_WRAP(vkEnumerateDeviceExtensionProperties, vkEnumerateDeviceExtensionProperties)
	EMUSUNI_WRAP(vkGetDeviceQueue, vkGetDeviceQueue)
	EMUSUNI_WRAP(vkDeviceWaitIdle, vkDeviceWaitIdle)
#undef EMUSUNI_OWN
#undef EMUSUNI_WRAP
	return nullptr;
}

// Upstream's emulated surface and swapchain functions (the list below) stand in even when the
// frontend's driver has none of their own.
static bool EmuSuNiEmulated(const char *name) {
	static const char *const names[] = {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkDestroySurfaceKHR",
		"vkCreateSwapchainKHR", "vkGetSwapchainImagesKHR", "vkAcquireNextImageKHR", "vkQueuePresentKHR",
		"vkDestroySwapchainKHR"};
	for (const char *n : names) {
		if (!strcmp(name, n))
			return true;
	}
	return false;
}
#endif
''')
patch('libretro/libretro_vulkan.cpp',
      'static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice_libretro(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {\n',
      'static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice_libretro(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '   if (adopted.active) {\n'
      '      *pDevice = adopted.device;  // EmuSuNi: the frontend\'s, as it was made\n'
      '      return VK_SUCCESS;\n'
      '   }\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      '\tVkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR_org(physicalDevice, surface, pSurfaceCapabilities);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tVkResult res = VK_SUCCESS;\n'
      '\tif (adopted.active) {\n'
      '\t\t// EmuSuNi: no surface: what the emulated swapchain\'s images can be.\n'
      '\t\t*pSurfaceCapabilities = {};\n'
      '\t\tpSurfaceCapabilities->minImageCount = 1;\n'
      '\t\tpSurfaceCapabilities->maxImageCount = VULKAN_MAX_SWAPCHAIN_IMAGES;\n'
      '\t\tpSurfaceCapabilities->maxImageArrayLayers = 1;\n'
      '\t\tpSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;\n'
      '\t\tpSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;\n'
      '\t\tpSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;\n'
      '\t\tpSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |\n'
      '\t\t\tVK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;\n'
      '\t} else {\n'
      '\t\tres = vkGetPhysicalDeviceSurfaceCapabilitiesKHR_org(physicalDevice, surface, pSurfaceCapabilities);\n'
      '\t}\n'
      '#else\n'
      '\tVkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR_org(physicalDevice, surface, pSurfaceCapabilities);\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      '\tPFN_vkVoidFunction fptr = vk_init_info.get_instance_proc_addr(instance, pName);\n',
      '\tPFN_vkVoidFunction fptr = vk_init_info.get_instance_proc_addr(instance, pName);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (adopted.active) {\n'
      '\t\tif (PFN_vkVoidFunction own = EmuSuNiFunction(pName, fptr))\n'
      '\t\t\treturn own;\n'
      '\t\tif (!fptr && EmuSuNiEmulated(pName))\n'
      '\t\t\tfptr = (PFN_vkVoidFunction)EmuSuNiNoDriverFunction;  // the list below answers it\n'
      '\t}\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      '\tPFN_vkVoidFunction fptr = vkGetDeviceProcAddr_org(device, pName);\n',
      '\tPFN_vkVoidFunction fptr = vkGetDeviceProcAddr_org(device, pName);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (adopted.active) {\n'
      '\t\tif (PFN_vkVoidFunction own = EmuSuNiFunction(pName, fptr))\n'
      '\t\t\treturn own;\n'
      '\t\tif (!fptr && EmuSuNiEmulated(pName))\n'
      '\t\t\tfptr = (PFN_vkVoidFunction)EmuSuNiNoDriverFunction;  // the list below answers it\n'
      '\t}\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      'void vk_libretro_set_hwrender_interface(retro_hw_render_interface *hw_render_interface) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the context on the frontend\'s own device (see the top of this file); false if it can\'t be.\n'
      'bool vk_libretro_adopt(const retro_hw_render_interface_vulkan *ri) {\n'
      '\tmemset(&vk_init_info, 0, sizeof(vk_init_info));\n'
      '\tvk_init_info.instance = ri->instance;\n'
      '\tvk_init_info.gpu = ri->gpu;\n'
      '\tvk_init_info.get_instance_proc_addr = ri->get_instance_proc_addr;\n'
      '\tadopted.active = true;\n'
      '\tadopted.device = ri->device;\n'
      '\tadopted.queue = ri->queue;\n'
      '\tadopted.queue_family = ri->queue_index;\n'
      '\tunsigned major = 1, minor = 1;\n'
      '\tif (const char *version = getenv("EMUSUNI_VK_API_VERSION"))\n'
      '\t\tsscanf(version, "%u.%u", &major, &minor);\n'
      '\tadopted.api_version = VK_MAKE_API_VERSION(0, major, minor, 0);\n'
      '\tconst char *features = getenv("EMUSUNI_VK_DEVICE_FEATURES");\n'
      '\tconst char *extensions = getenv("EMUSUNI_VK_DEVICE_EXTENSIONS");\n'
      '\tadopted.features = features ? features : "";\n'
      '\tadopted.extensions = extensions ? extensions : "";\n'
      '\tINFO_LOG(Log::G3D, "EmuSuNi: the frontend\'s Vulkan device, Vulkan %u.%u, queue family %u; features: %s; extensions: %s",\n'
      '\t\tmajor, minor, ri->queue_index, features ? features : "(none)", extensions ? extensions : "(none)");\n'
      '\tvkGetInstanceProcAddr_org = vkGetInstanceProcAddr;\n'
      '\tvkGetInstanceProcAddr = vkGetInstanceProcAddr_libretro;\n'
      '\tvkGetDeviceProcAddr_org = ri->get_device_proc_addr ? ri->get_device_proc_addr :\n'
      '\t\t(PFN_vkGetDeviceProcAddr)ri->get_instance_proc_addr(ri->instance, "vkGetDeviceProcAddr");\n'
      '\tvkGetDeviceProcAddr = vkGetDeviceProcAddr_libretro;\n'
      '\tvkCreateInstance = vkCreateInstance_libretro;\n'
      '\tvkEnumerateInstanceVersion = vkEnumerateInstanceVersion_emusuni;\n'
      '\tvkEnumerateInstanceExtensionProperties = vkEnumerateInstanceExtensionProperties_emusuni;\n'
      '\tvkEnumerateInstanceLayerProperties = vkEnumerateInstanceLayerProperties_emusuni;\n'
      '\treturn vkGetDeviceProcAddr_org != nullptr;\n'
      '}\n'
      '#endif\n\n'
      'void vk_libretro_set_hwrender_interface(retro_hw_render_interface *hw_render_interface) {\n')
patch('libretro/libretro_vulkan.cpp',
      '\tDEDICATED_ALLOCATION = false;\n}\n',
      '\tDEDICATED_ALLOCATION = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tadopted.active = false;\n'
      '\tadopted.device = VK_NULL_HANDLE;\n'
      '\tadopted.queue = VK_NULL_HANDLE;\n'
      '#endif\n'
      '}\n')
patch('libretro/LibretroVulkanContext.cpp',
      'void vk_libretro_wait_for_presentation();\n',
      'void vk_libretro_wait_for_presentation();\n'
      '#ifdef EMUSUNI_PORT\n'
      'bool vk_libretro_adopt(const retro_hw_render_interface_vulkan *ri);\n\n'
      '// EmuSuNi: the frontend made its device itself and never calls create_device: the context on it\n'
      '// (libretro_vulkan.cpp), its surface and swapchain emulated.\n'
      'static bool EmuSuNiAdoptFrontendDevice(const retro_hw_render_interface_vulkan *ri) {\n'
      '\tinit_glslang();\n'
      '\tvk = new VulkanContext();\n'
      '\tbool instance = vk_libretro_adopt(ri) && vk->CreateInstance({}) == VK_SUCCESS;\n'
      '\tint physical_device = -1;\n'
      '\tfor (int i = 0; instance && i < vk->GetNumPhysicalDevices(); i++) {\n'
      '\t\tif (vk->GetPhysicalDevice(i) == ri->gpu)\n'
      '\t\t\tphysical_device = i;\n'
      '\t}\n'
      '\tbool device = physical_device >= 0 && vk->CreateDevice(physical_device) == VK_SUCCESS;\n'
      '\tif (device && vk->InitSurface(WINDOWSYSTEM_EMUSUNI, (void *)ri, nullptr) == VK_SUCCESS) {\n'
      '\t\tINFO_LOG(Log::G3D, "EmuSuNi: Vulkan context on the frontend\'s device (GPU %d)", physical_device);\n'
      '\t\treturn true;\n'
      '\t}\n'
      '\tERROR_LOG(Log::G3D, "EmuSuNi: the frontend\'s Vulkan device can\'t be used: %s", vk->InitError().c_str());\n'
      '\tif (device)\n'
      '\t\tvk->DestroyDevice();\n'
      '\tif (instance)\n'
      '\t\tvk->DestroyInstance();\n'
      '\tdelete vk;\n'
      '\tvk = nullptr;\n'
      '\tfinalize_glslang();\n'
      '\tvk_libretro_shutdown();\n'
      '\treturn false;\n'
      '}\n'
      '#endif\n')
patch('libretro/LibretroVulkanContext.cpp',
      '   vk_libretro_set_hwrender_interface(vulkan);\n',
      '   vk_libretro_set_hwrender_interface(vulkan);\n'
      '#ifdef EMUSUNI_PORT\n'
      '   if (!vk && !EmuSuNiAdoptFrontendDevice((const retro_hw_render_interface_vulkan *)vulkan)) {\n'
      '      ERROR_LOG(Log::G3D, "EmuSuNi: no Vulkan context: nothing can be drawn");\n'
      '      return;\n'
      '   }\n'
      '#endif\n')
patch('libretro/libretro.cpp',
      '         g_Config.iInflightFrames = 3;\n   }\n',
      '         g_Config.iInflightFrames = 3;\n   }\n'
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: no render thread unless EMUSUNI_PSP_RENDER_THREAD=1 (upstream\'s: the GPU work recorded\n'
      '   // and submitted beside the emulation; the frontend\'s calls stay on the thread of retro_run,\n'
      '   // libretro_vulkan.cpp, and it shows each frame one retro_run later).\n'
      '   {\n'
      '      const char *thread = getenv("EMUSUNI_PSP_RENDER_THREAD");\n'
      '      g_Config.bRenderMultiThreading = thread && atoi(thread) != 0;\n'
      '   }\n'
      '#endif\n')
# The pipeline cache (<memstick>/PSP/SYSTEM/CACHE/<game>.vkshadercache) keeps pipelines as they were
# made: one made on a device with more features (an earlier EmuSuNi with depth clamp or logic ops in
# EMUSUNI_VK_DEVICE_FEATURES) isn't made again from it on a device without them (upstream made it,
# against the Vulkan rules); the game makes it again if it needs it, as this device can.
patch('GPU/Vulkan/PipelineManagerVulkan.cpp',
      '#include "GPU/Vulkan/PipelineManagerVulkan.h"\n',
      '#include "GPU/Vulkan/PipelineManagerVulkan.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "GPU/GPUState.h"\n'
      '#endif\n')
patch('GPU/Vulkan/PipelineManagerVulkan.cpp',
      '\t\t\tWARN_LOG(Log::G3D, "Bad raster key in cache, ignoring");\n'
      '\t\t\tcontinue;\n'
      '\t\t}\n',
      '\t\t\tWARN_LOG(Log::G3D, "Bad raster key in cache, ignoring");\n'
      '\t\t\tcontinue;\n'
      '\t\t}\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tauto dualSource = [](unsigned factor) { return factor >= VK_BLEND_FACTOR_SRC1_COLOR && factor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; };\n'
      '\t\tif ((key.raster.depthClampEnable && !gstate_c.Use(GPU_USE_DEPTH_CLAMP)) ||\n'
      '\t\t\t(key.raster.logicOpEnable && !gstate_c.Use(GPU_USE_LOGIC_OP)) ||\n'
      '\t\t\t(key.raster.blendEnable && !gstate_c.Use(GPU_USE_DUALSOURCE_BLEND) &&\n'
      '\t\t\t (dualSource(key.raster.srcColor) || dualSource(key.raster.destColor) ||\n'
      '\t\t\t  dualSource(key.raster.srcAlpha) || dualSource(key.raster.destAlpha)))) {\n'
      '\t\t\tWARN_LOG(Log::G3D, "EmuSuNi: pipeline %d in the cache needs a device feature this device wasn\'t made with, skipped", (int)i);\n'
      '\t\t\tcontinue;\n'
      '\t\t}\n'
      '#endif\n')

# --- A game that can't start (a bad file, an ELF it can't place): the core asks the frontend to
# shut it down (RETRO_ENVIRONMENT_SHUTDOWN); if the frontend calls retro_run again anyway, nothing
# runs (upstream ran the CPU it never set up: a crash).
patch('libretro/libretro.cpp',
      '   g_pendingBoot = true;\n',
      '   g_pendingBoot = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '   g_bootFailed = false;\n'
      '#endif\n')
patch('libretro/libretro.cpp',
      '      case BootState::Failed:\n         g_pendingBoot = false;\n',
      '      case BootState::Failed:\n         g_pendingBoot = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '         g_bootFailed = true;\n'
      '#endif\n')
patch('libretro/libretro.cpp',
      'void retro_run(void)\n{\n   if (g_pendingBoot) {\n',
      'void retro_run(void)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: the game couldn\'t start (SHUTDOWN was asked for): nothing to run.\n'
      '   if (g_bootFailed)\n'
      '      return;\n'
      '#endif\n'
      '   if (g_pendingBoot) {\n')
patch('libretro/libretro.cpp',
      '   bool g_pendingBoot = false;\n',
      '   bool g_pendingBoot = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '   bool g_bootFailed = false;  // EmuSuNi: the boot failed, retro_run does nothing\n'
      '#endif\n')

# --- A program's debug output (the emulator's devctl, which homebrew and PPSSPP's tests print with)
# in the log at INFO (upstream: DEBUG, which the libretro core never shows).
patch('libretro/libretro.cpp',
      'bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1, const std::string &param2, int64_t param3, int64_t param4) { return false; }\n',
      'bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1, const std::string &param2, int64_t param3, int64_t param4) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '   if (type == SystemRequestType::SEND_DEBUG_OUTPUT) {\n'
      '      INFO_LOG(Log::Printf, "debug output: %s", param1.c_str());  // EmuSuNi\n'
      '      return true;\n'
      '   }\n'
      '#endif\n'
      '   return false;\n'
      '}\n')

# --- Threads: the app names and places them; a pool of at most 4 + 4.
patch('Common/Thread/ThreadUtil.cpp',
      'void SetCurrentThreadName(const char *threadName) {\n#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (the app, port/): called first thing in every thread PPSSPP makes (it names each one) and\n'
      '// by retro_init on the emulation thread ("Main"): the thread\'s core, priority and name.\n'
      'extern "C" void emusuni_psp_thread(const char *name);\n'
      '#endif\n\n'
      'void SetCurrentThreadName(const char *threadName) {\n'
      '#if defined(EMUSUNI_PORT)\n'
      '\temusuni_psp_thread(threadName);\n'
      '#elif PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)\n')
patch('libretro/libretro.cpp',
      '   g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);\n',
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: at most 4 compute threads (and as many for blocking I/O): the PS4 has 7 cores for the\n'
      '   // app, the emulation thread is on one of them and the GPU work on others.\n'
      '   g_threadManager.Init(std::max(1, std::min(cpu_info.num_cores * cpu_info.logical_cpu_count, 4)), 1);\n'
      '#else\n'
      '   g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);\n'
      '#endif\n')

# --- Sound: every frame converted (upstream converted at most 1024 into a stack buffer, then copied
# all of them out of it).
patch('libretro/libretro.cpp',
      'void System_AudioPushSamples(const int32_t *audio, int numSamples, float volume) {\n',
      'void System_AudioPushSamples(const int32_t *audio, int numSamples, float volume) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: straight into the output buffer, whatever the number of frames.\n'
      '   {\n'
      '      const int values = numSamples * 2;\n'
      '      if (output_audio_buffer.capacity - output_audio_buffer.size < values)\n'
      '         ensure_output_audio_buffer_capacity((output_audio_buffer.capacity + values) * 1.5);\n'
      '      int16_t *out = output_audio_buffer.data + output_audio_buffer.size;\n'
      '      for (int i = 0; i < values; i++)\n'
      '         out[i] = Clamp16(audio[i]);\n'
      '      output_audio_buffer.size += values;\n'
      '      return;\n'
      '   }\n'
      '#endif\n')

# --- The swapchain's images and the frontend: the core calls the frontend's set_image only at the end
# of retro_run, and never get_sync_index / wait_sync_index. Upstream's emulated swapchain called them
# when PPSSPP acquired and presented: in the middle of retro_run, or from the render thread at any
# time (racing with the frontend's own frame: its frame index, its fences, the image it draws). Now
# the images are taken round robin, never the one the frontend shows now, and the newest presented
# image goes to set_image in SwapBuffers. So the frontend may draw (on another thread) while retro_run
# runs, and PPSSPP's render thread (EMUSUNI_PSP_RENDER_THREAD=1) calls only lock_queue/unlock_queue
# (libretro allows submitting from any thread). The GPU stays in order: one queue, the backbuffer
# render pass waits for all earlier work (its external dependency from BOTTOM_OF_PIPE) before it
# writes the image, and the frontend only samples it.
patch('libretro/libretro_vulkan.cpp',
      'static VkSwapchainKHR_T chain;\n',
      'static VkSwapchainKHR_T chain;\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the swapchain\'s images and the frontend (vkAcquireNextImageKHR_libretro), under chain.mutex.\n'
      'static uint32_t emusuni_next_image = 0;  // where the round robin goes on\n'
      'static int emusuni_presented = -1;       // the newest image presented\n'
      'static int emusuni_shown = -1;           // the image last handed to set_image\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      '\tchain.current_index = -1;\n\tchain.ever_presented = false;\n\t*pSwapchain = (VkSwapchainKHR)&chain;\n',
      '\tchain.current_index = -1;\n\tchain.ever_presented = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuni_next_image = 0;\n\temusuni_presented = -1;\n\temusuni_shown = -1;\n'
      '#endif\n'
      '\t*pSwapchain = (VkSwapchainKHR)&chain;\n')
patch('libretro/libretro_vulkan.cpp',
      'static VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR_libretro(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {\n',
      'static VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR_libretro(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t{\n'
      '\t\t// EmuSuNi: the next image round robin, never the one the frontend shows now (not the frontend\'s\n'
      '\t\t// sync index: this may be the render thread, and the frontend may be drawing meanwhile).\n'
      '\t\tstd::lock_guard<std::mutex> lock(chain.mutex);\n'
      '\t\tuint32_t index = chain.count ? emusuni_next_image % chain.count : 0;\n'
      '\t\tif ((int)index == emusuni_shown && chain.count > 1)\n'
      '\t\t\tindex = (index + 1) % chain.count;\n'
      '\t\temusuni_next_image = index + 1;\n'
      '\t\t*pImageIndex = index;\n'
      '\t\treturn VK_SUCCESS;\n'
      '\t}\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      '\tVkSwapchainKHR_T *swapchain = (VkSwapchainKHR_T *)pPresentInfo->pSwapchains[0];\n'
      '\tstd::unique_lock<std::mutex> lock(swapchain->mutex);\n',
      '\tVkSwapchainKHR_T *swapchain = (VkSwapchainKHR_T *)pPresentInfo->pSwapchains[0];\n'
      '\tstd::unique_lock<std::mutex> lock(swapchain->mutex);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t{\n'
      '\t\t// EmuSuNi: remembered; retro_run hands it to set_image at its end (SwapBuffers).\n'
      '\t\tchain.current_index = pPresentInfo->pImageIndices[0];\n'
      '\t\temusuni_presented = chain.current_index;\n'
      '\t\tchain.ever_presented = true;\n'
      '\t\tswapchain->condVar.notify_all();\n'
      '\t\treturn VK_SUCCESS;\n'
      '\t}\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      'void vk_libretro_wait_for_presentation() {\n',
      'void vk_libretro_wait_for_presentation() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t{\n'
      '\t\t// EmuSuNi: at the end of retro_run, on its thread: the newest image presented (this frame\'s;\n'
      '\t\t// with the render thread usually the one before) to the frontend. Nothing yet: nothing.\n'
      '\t\tint show = -1;\n'
      '\t\t{\n'
      '\t\t\tstd::lock_guard<std::mutex> lock(chain.mutex);\n'
      '\t\t\tif (emusuni_presented >= 0 && emusuni_presented < (int)chain.count) {\n'
      '\t\t\t\tshow = emusuni_presented;\n'
      '\t\t\t\temusuni_shown = show;\n'
      '\t\t\t}\n'
      '\t\t}\n'
      '\t\tif (show >= 0 && vulkan)\n'
      '\t\t\tvulkan->set_image(vulkan->handle, &chain.images[show].retro_image, 0, nullptr, vulkan->queue_index);\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('libretro/libretro_vulkan.cpp',
      '\tmemset(&chain.images, 0x00, sizeof(chain.images));\n\tchain.count = 0;\n',
      '\tmemset(&chain.images, 0x00, sizeof(chain.images));\n\tchain.count = 0;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuni_next_image = 0;\n\temusuni_presented = -1;\n\temusuni_shown = -1;\n'
      '#endif\n')

# --- Timing: where the time goes, a few lines in the log every ~10 s (the TSC, a few dozen reads a
# frame): retro_run's parts on the emulation thread (the display lists and their blocking readbacks,
# waiting for the render thread and for the GPU, the frontend's calls), the render thread's, the
# queue lock's, the compiles, the GPU emulation's events per frame, the slowest frame's parts, and
# the GPU's own time per step for 8 frames after each line (PPSSPP's timestamp profiler).
def tabs(text):
    # The blocks below are written with 4 spaces; PPSSPP's files (but libretro/) indent with tabs.
    return text.replace('    ', '\t')

addfile('Common/EmuSuNiTiming.h', tabs(r'''// EmuSuNi (scripts/build-psp-core.sh): where the time goes, in TSC ticks summed from any thread and
// logged every ~10 s from retro_run (libretro.cpp), which then zeroes them. A few dozen reads a frame.
#pragma once

#include <atomic>
#include <cstdint>

namespace EmuSuNiTiming {

enum Slot {
    RetroRun,          // retro_run (the emulation thread, after the boot)
    DisplayLists,      // the GE's display lists (GPUCommon::ProcessDLQueue), their readbacks included
    Readbacks,         // the blocking readbacks (FlushSync: the render thread and the GPU drained)
    WaitRenderThread,  // BeginFrame: the render thread hasn't submitted the frame data's last frame yet
    WaitGpu,           // BeginFrame: the GPU hasn't finished it (the frames in flight)
    Frontend,          // set_image and the sound batch, the frontend's calls at the end of retro_run
    Recording,         // VulkanRenderManager::Run: recording and submitting (the render thread's)
    WaitPipeline,      // the recording waiting for a pipeline still compiling
    QueueLock,         // vkQueueSubmit waiting for the frontend's queue lock
    QueueSubmit,       // vkQueueSubmit under it
    CompilePipeline,   // vkCreateGraphicsPipelines (the pool's threads, or inline)
    CompileShader,     // GLSL to SPIR-V (glslang, its own threads)
    NumSlots
};

extern std::atomic<uint64_t> Ticks[NumSlots];
extern std::atomic<uint32_t> Counts[NumSlots];

static inline uint64_t Now() { return __builtin_ia32_rdtsc(); }

static inline void Add(Slot slot, uint64_t ticks) {
    Ticks[slot].fetch_add(ticks, std::memory_order_relaxed);
    Counts[slot].fetch_add(1, std::memory_order_relaxed);
}

// A wait: its time always, counted only when it took more than 32768 ticks (~20 us on the PS4).
static inline void AddWait(Slot slot, uint64_t ticks) {
    Ticks[slot].fetch_add(ticks, std::memory_order_relaxed);
    if (ticks > 32768)
        Counts[slot].fetch_add(1, std::memory_order_relaxed);
}

struct Timer {
    explicit Timer(Slot s) : slot(s), start(Now()) {}
    ~Timer() { Add(slot, Now() - start); }
    Slot slot;
    uint64_t start;
};

// The emulation thread's: why the next blocking readback happens (FramebufferManagerCommon.cpp),
// each readback by that reason (or its tag), the GPU's time per step of the frames profiled
// (BeginFrame), and whether to profile the frame starting now (12 after each log line).
extern const char *g_readbackReason;
struct ReadbackReason {
    explicit ReadbackReason(const char *why) { g_readbackReason = why; }
    ~ReadbackReason() { g_readbackReason = nullptr; }
};
void Readback(const char *tag, uint64_t ticks);
void GpuStep(const char *description, double ms);
void GpuFrame(double spanMs, float timestampPeriodNs);
bool ProfileFrame();

}  // namespace EmuSuNiTiming
'''))

# The emulation thread: the display lists, BeginFrame's waits, the blocking readbacks and why.
patch('GPU/GPUCommon.cpp',
      '#include "Common/LogReporting.h"\n',
      '#include "Common/LogReporting.h"\n#ifdef EMUSUNI_PORT\n#include "Common/EmuSuNiTiming.h"\n#endif\n')
patch('GPU/GPUCommon.cpp',
      '\tTimeCollector collectStat(&gpuStats.msProcessingDisplayLists, coreCollectDebugStats);\n',
      '\tTimeCollector collectStat(&gpuStats.msProcessingDisplayLists, coreCollectDebugStats);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tEmuSuNiTiming::Timer emusuniTimer(EmuSuNiTiming::DisplayLists);  // EmuSuNi: timed\n'
      '#endif\n')
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      '#include "Common/Thread/ThreadUtil.h"\n',
      '#include "Common/Thread/ThreadUtil.h"\n#ifdef EMUSUNI_PORT\n#include "Common/EmuSuNiTiming.h"\n#endif\n')
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      '\tif (useRenderThread_) {\n'
      '\t\tstd::unique_lock<std::mutex> lock(frameData.fenceMutex);\n'
      '\t\twhile (!frameData.readyForFence) {\n'
      '\t\t\tframeData.fenceCondVar.wait(lock);\n'
      '\t\t}\n'
      '\t\tframeData.readyForFence = false;\n'
      '\t}\n'
      '\n'
      '\t// This must be the very first Vulkan call we do in a new frame.\n'
      '\t// Makes sure the very last command buffer from the frame before the previous has been fully executed.\n'
      '\tif (vkWaitForFences(device, 1, &frameData.fence, true, UINT64_MAX) == VK_ERROR_DEVICE_LOST) {\n'
      '\t\t_assert_msg_(false, "Device lost in vkWaitForFences");\n'
      '\t}\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: both waits timed (the render thread behind, the GPU behind).\n'
      '\tconst uint64_t emusuniT0 = EmuSuNiTiming::Now();\n'
      '#endif\n'
      '\tif (useRenderThread_) {\n'
      '\t\tstd::unique_lock<std::mutex> lock(frameData.fenceMutex);\n'
      '\t\twhile (!frameData.readyForFence) {\n'
      '\t\t\tframeData.fenceCondVar.wait(lock);\n'
      '\t\t}\n'
      '\t\tframeData.readyForFence = false;\n'
      '\t}\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tconst uint64_t emusuniT1 = EmuSuNiTiming::Now();\n'
      '\tif (useRenderThread_)\n'
      '\t\tEmuSuNiTiming::AddWait(EmuSuNiTiming::WaitRenderThread, emusuniT1 - emusuniT0);\n'
      '#endif\n'
      '\n'
      '\t// This must be the very first Vulkan call we do in a new frame.\n'
      '\t// Makes sure the very last command buffer from the frame before the previous has been fully executed.\n'
      '\tif (vkWaitForFences(device, 1, &frameData.fence, true, UINT64_MAX) == VK_ERROR_DEVICE_LOST) {\n'
      '\t\t_assert_msg_(false, "Device lost in vkWaitForFences");\n'
      '\t}\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tEmuSuNiTiming::AddWait(EmuSuNiTiming::WaitGpu, EmuSuNiTiming::Now() - emusuniT1);\n'
      '#endif\n')
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      'void VulkanRenderManager::FlushSync() {\n\t_dbg_assert_(!curRenderStep_);\n',
      'void VulkanRenderManager::FlushSync() {\n\t_dbg_assert_(!curRenderStep_);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tEmuSuNiTiming::Timer emusuniTimer(EmuSuNiTiming::Readbacks);  // EmuSuNi: timed\n'
      '#endif\n')
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      '\tif (mode == Draw::ReadbackMode::BLOCK) {\n\t\tFlushSync();\n\t}\n',
      '\tif (mode == Draw::ReadbackMode::BLOCK) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: each by its reason (EmuSuNiTiming::ReadbackReason) or tag.\n'
      '\t\tconst uint64_t emusuniT0 = EmuSuNiTiming::Now();\n'
      '\t\tFlushSync();\n'
      '\t\tEmuSuNiTiming::Readback(tag, EmuSuNiTiming::Now() - emusuniT0);\n'
      '#else\n'
      '\t\tFlushSync();\n'
      '#endif\n'
      '\t}\n')
patch('GPU/Common/FramebufferManagerCommon.cpp',
      '#include "Common/StringUtils.h"\n',
      '#include "Common/StringUtils.h"\n#ifdef EMUSUNI_PORT\n#include "Common/EmuSuNiTiming.h"\n'
      '// EmuSuNi: why the blocking readback that follows happens (the timing lines in the log).\n'
      '#define EMUSUNI_READBACK_REASON(why) EmuSuNiTiming::ReadbackReason emusuniReason(why)\n'
      '#else\n#define EMUSUNI_READBACK_REASON(why)\n#endif\n')
for call, why in (
        ('\t\t\tReadFramebufferToMemory(vfb, 0, 0, vfb->safeWidth, vfb->safeHeight, RASTER_COLOR, Draw::ReadbackMode::BLOCK);\n',
         "a new framebuffer's first frame"),
        ('\t\t\tReadFramebufferToMemory(prevVfb, 0, 0, prevVfb->width, prevVfb->height, RasterChannel::RASTER_DEPTH, Draw::ReadbackMode::BLOCK);\n',
         'depth at a framebuffer switch'),
        ('\t\t\tReadFramebufferToMemory(vfb, 0, 0, vfb->width, vfb->height, RASTER_COLOR, Draw::ReadbackMode::BLOCK);\n',
         'a framebuffer the game reads (frame end)'),
        ('\t\t\tReadFramebufferToMemory(srcBuffer, 0, srcY, srcBuffer->width, srcH, channel, Draw::ReadbackMode::BLOCK);\n',
         'a memcpy from a framebuffer'),
        ('\t\t\t\tReadFramebufferToMemory(srcRect.vfb, static_cast<int>(srcX * srcXFactor), srcY, static_cast<int>(srcRect.w_bytes * srcXFactor), srcRect.h, RASTER_COLOR, Draw::ReadbackMode::BLOCK);\n',
         'a block transfer from a framebuffer'),
        ('\t\t\tReadbackFramebuffer(vfb, x, y, w, h, RASTER_COLOR, Draw::ReadbackMode::BLOCK);\n',
         'a palette (CLUT) from a framebuffer')):
    indent = call[:len(call) - len(call.lstrip('\t'))]
    patch('GPU/Common/FramebufferManagerCommon.cpp', call,
          indent + 'EMUSUNI_READBACK_REASON("' + why + '");\n' + call)

# The render thread: its recording and submitting, the pipelines it waits for; the compiles.
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      'void VulkanRenderManager::Run(VKRRenderThreadTask &task) {\n\tFrameData &frameData = frameData_[task.frame];\n',
      'void VulkanRenderManager::Run(VKRRenderThreadTask &task) {\n\tFrameData &frameData = frameData_[task.frame];\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tEmuSuNiTiming::Timer emusuniTimer(EmuSuNiTiming::Recording);  // EmuSuNi: timed\n'
      '#endif\n')
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      '\tVkResult result = vkCreateGraphicsPipelines(vulkan->GetDevice(), desc->pipelineCache, 1, &pipe, nullptr, &vkpipeline);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tconst uint64_t emusuniT0 = EmuSuNiTiming::Now();\n'
      '#endif\n'
      '\tVkResult result = vkCreateGraphicsPipelines(vulkan->GetDevice(), desc->pipelineCache, 1, &pipe, nullptr, &vkpipeline);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tEmuSuNiTiming::Add(EmuSuNiTiming::CompilePipeline, EmuSuNiTiming::Now() - emusuniT0);\n'
      '#endif\n')
patch('Common/GPU/Vulkan/VulkanQueueRunner.cpp',
      '#include "Common/TimeUtil.h"\n',
      '#include "Common/TimeUtil.h"\n#ifdef EMUSUNI_PORT\n#include "Common/EmuSuNiTiming.h"\n#endif\n')
patch('Common/GPU/Vulkan/VulkanQueueRunner.cpp',
      '\t\t\t\tVkPipeline pipeline;\n'
      '\n'
      '\t\t\t\t{\n'
      '\t\t\t\t\tstd::lock_guard<std::mutex> lock(graphicsPipeline->mutex_);\n',
      '\t\t\t\tVkPipeline pipeline;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\tconst uint64_t emusuniT0 = EmuSuNiTiming::Now();  // EmuSuNi: the wait for it timed\n'
      '#endif\n'
      '\n'
      '\t\t\t\t{\n'
      '\t\t\t\t\tstd::lock_guard<std::mutex> lock(graphicsPipeline->mutex_);\n')
patch('Common/GPU/Vulkan/VulkanQueueRunner.cpp',
      '\t\t\t\t\tpipeline = graphicsPipeline->pipeline[(size_t)rpType]->BlockUntilReady();\n\t\t\t\t}\n',
      '\t\t\t\t\tpipeline = graphicsPipeline->pipeline[(size_t)rpType]->BlockUntilReady();\n\t\t\t\t}\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\tEmuSuNiTiming::AddWait(EmuSuNiTiming::WaitPipeline, EmuSuNiTiming::Now() - emusuniT0);\n'
      '#endif\n')
patch('GPU/Vulkan/ShaderManagerVulkan.cpp',
      '#include "Common/TimeUtil.h"\n',
      '#include "Common/TimeUtil.h"\n#ifdef EMUSUNI_PORT\n#include "Common/EmuSuNiTiming.h"\n#endif\n')
patch('GPU/Vulkan/ShaderManagerVulkan.cpp',
      '\tauto compile = [=] {\n\t\tPROFILE_THIS_SCOPE("shadercomp");\n',
      '\tauto compile = [=] {\n\t\tPROFILE_THIS_SCOPE("shadercomp");\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tEmuSuNiTiming::Timer emusuniTimer(EmuSuNiTiming::CompileShader);  // EmuSuNi: timed\n'
      '#endif\n')
# The queue: the frontend's lock and the submit under it.
patch('libretro/libretro_vulkan.cpp',
      '#include "Core/Config.h"\n',
      '#include "Core/Config.h"\n#ifdef EMUSUNI_PORT\n#include "Common/EmuSuNiTiming.h"\n#endif\n')
patch('libretro/libretro_vulkan.cpp',
      '\tvulkan->lock_queue(vulkan->handle);\n'
      '\tres = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);\n'
      '\tvulkan->unlock_queue(vulkan->handle);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: timed, the wait for the frontend\'s lock and the submit under it.\n'
      '\tconst uint64_t emusuniT0 = EmuSuNiTiming::Now();\n'
      '\tvulkan->lock_queue(vulkan->handle);\n'
      '\tconst uint64_t emusuniT1 = EmuSuNiTiming::Now();\n'
      '\tres = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);\n'
      '\tconst uint64_t emusuniT2 = EmuSuNiTiming::Now();\n'
      '\tvulkan->unlock_queue(vulkan->handle);\n'
      '\tEmuSuNiTiming::AddWait(EmuSuNiTiming::QueueLock, emusuniT1 - emusuniT0);\n'
      '\tEmuSuNiTiming::Add(EmuSuNiTiming::QueueSubmit, emusuniT2 - emusuniT1);\n'
      '#else\n'
      '\tvulkan->lock_queue(vulkan->handle);\n'
      '\tres = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);\n'
      '\tvulkan->unlock_queue(vulkan->handle);\n'
      '#endif\n')
# The GPU: PPSSPP's own timestamp profiler, its results per step (the frames EmuFrame profiles).
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      '\t\t\t\t\tif (i == 0) {\n\t\t\t\t\t\tinitTimeMs_.Update(milliseconds);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\tEmuSuNiTiming::GpuStep(i == 0 ? "(the init command buffer)" : frameData.profile.timestampDescriptions[i + 1].c_str(), milliseconds);\n'
      '#endif\n'
      '\t\t\t\t\tif (i == 0) {\n\t\t\t\t\t\tinitTimeMs_.Update(milliseconds);\n')
patch('Common/GPU/Vulkan/VulkanRenderManager.cpp',
      '\t\t\t\tframeData.profile.profileSummary = str.str();\n'
      '\t\t\t} else {\n'
      '\t\t\t\tframeData.profile.profileSummary = "(error getting GPU profile - not ready?)";\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\tEmuSuNiTiming::GpuFrame((double)((queryResults[numQueries - 1] - queryResults[0]) & timestampDiffMask) * timestampConversionFactor,\n'
      '\t\t\t\t\tvulkan_->GetPhysicalDeviceProperties().properties.limits.timestampPeriod);\n'
      '#endif\n'
      '\t\t\t\tframeData.profile.profileSummary = str.str();\n'
      '\t\t\t} else {\n'
      '\t\t\t\tframeData.profile.profileSummary = "(error getting GPU profile - not ready?)";\n')
patch('libretro/libretro.cpp',
      '         draw->BeginFrame(Draw::DebugFlags::NONE);\n',
      '#ifdef EMUSUNI_PORT\n'
      '         // EmuSuNi: the GPU\'s timestamps for a few frames after each timing line.\n'
      '         draw->BeginFrame(EmuSuNiTiming::ProfileFrame() ? Draw::DebugFlags::PROFILE_TIMESTAMPS : Draw::DebugFlags::NONE);\n'
      '#else\n'
      '         draw->BeginFrame(Draw::DebugFlags::NONE);\n'
      '#endif\n')
# retro_run timed, and the lines written.
patch('libretro/libretro.cpp',
      '#include "GPU/Common/FramebufferManagerCommon.h"\n',
      '#include "GPU/Common/FramebufferManagerCommon.h"\n'
      '#ifdef EMUSUNI_PORT\n#include <algorithm>\n#include <chrono>\n#include "Common/EmuSuNiTiming.h"\n\n'
      '// EmuSuNi: the option ppsspp_emusuni_30fps (check_variables), applied each retro_run.\n'
      'static bool g_emusuni30fps = false;\n#endif\n')
patch('libretro/libretro.cpp',
      '   VsyncSwapIntervalDetect();\n   ctx->SwapBuffers();\n   upload_output_audio_buffer();\n}\n',
      '   VsyncSwapIntervalDetect();\n'
      '#ifdef EMUSUNI_PORT\n'
      '   {\n'
      '      EmuSuNiTiming::Timer emusuniFrontend(EmuSuNiTiming::Frontend);  // EmuSuNi: timed\n'
      '      ctx->SwapBuffers();\n'
      '      upload_output_audio_buffer();\n'
      '   }\n'
      '#else\n'
      '   ctx->SwapBuffers();\n'
      '   upload_output_audio_buffer();\n'
      '#endif\n'
      '}\n')
patch('libretro/libretro.cpp',
      '   // TODO: This seems dubious.\n   if (softwareRenderInitHack)\n',
      '#ifdef EMUSUNI_PORT\n'
      '   EmuSuNiRunTimer emusuniRunTimer;  // EmuSuNi: from here (the game running) to retro_run\'s end\n'
      '#endif\n'
      '   // TODO: This seems dubious.\n   if (softwareRenderInitHack)\n')
patch('libretro/libretro.cpp',
      'void retro_run(void)\n{\n',
      r'''#ifdef EMUSUNI_PORT
// EmuSuNi: where the time goes (Common/EmuSuNiTiming.h), a few lines in the log every ~10 s (or every
// PSP_TIMING_FRAMES frames: the PC tests' environment variable).
namespace EmuSuNiTiming {
std::atomic<uint64_t> Ticks[NumSlots];
std::atomic<uint32_t> Counts[NumSlots];
const char *g_readbackReason = nullptr;
}

namespace {
// The emulation thread's tables: the blocking readbacks by reason, the GPU's steps of the frames
// profiled (by their description without the draws and sizes: the same pass from frame to frame).
struct EmuSuNiReadbacks {
   const char *name;
   uint64_t ticks;
   uint32_t count;
};
EmuSuNiReadbacks s_emusuniReadbacks[8];
struct EmuSuNiGpuStep {
   char key[72];
   double ms;
   uint32_t count;
};
EmuSuNiGpuStep s_emusuniGpuSteps[32];
// Render passes, copies, blits, readbacks, the rest (the init command buffer: uploads).
double s_emusuniGpuKind[5];
uint32_t s_emusuniGpuKindCount[5];
double s_emusuniGpuSpan = 0.0;
float s_emusuniGpuPeriod = 0.0f;
int s_emusuniGpuFrames = 0;
int s_emusuniProfileLeft = 0;
// The slowest frame of these 10 s: its share of each slot (the other threads' meanwhile).
uint64_t s_emusuniSlowest = 0;
uint64_t s_emusuniSlowestTicks[EmuSuNiTiming::NumSlots];
uint32_t s_emusuniSlowestCompiled = 0;

struct EmuSuNiRunTimer {
   EmuSuNiRunTimer() {
      for (int i = 0; i < EmuSuNiTiming::NumSlots; i++)
         before[i] = EmuSuNiTiming::Ticks[i].load(std::memory_order_relaxed);
      compiled = EmuSuNiTiming::Counts[EmuSuNiTiming::CompilePipeline].load(std::memory_order_relaxed);
      start = EmuSuNiTiming::Now();
   }
   ~EmuSuNiRunTimer();
   uint64_t before[EmuSuNiTiming::NumSlots];
   uint32_t compiled;
   uint64_t start;
};
}

void EmuSuNiTiming::Readback(const char *tag, uint64_t ticks) {
   const char *name = g_readbackReason ? g_readbackReason : tag ? tag : "?";
   const size_t n = sizeof(s_emusuniReadbacks) / sizeof(s_emusuniReadbacks[0]);
   for (size_t i = 0; i < n; i++) {
      EmuSuNiReadbacks &r = s_emusuniReadbacks[i];
      const bool same = r.name && (r.name == name || strcmp(r.name, name) == 0);
      if (r.name && !same && i + 1 < n)
         continue;
      if (!r.name)
         r.name = name;
      else if (!same)
         r.name = "others";  // (the last one, when there are more reasons)
      r.ticks += ticks;
      r.count++;
      return;
   }
}

void EmuSuNiTiming::GpuStep(const char *description, double ms) {
   int kind = 0;
   if (!strncmp(description, "COPY", 4))
      kind = 1;
   else if (!strncmp(description, "BLIT", 4))
      kind = 2;
   else if (!strncmp(description, "READBACK", 8))
      kind = 3;
   else if (description[0] == '(')
      kind = 4;
   s_emusuniGpuKind[kind] += ms;
   s_emusuniGpuKindCount[kind]++;
   const char *paren = strstr(description, " (");
   size_t length = paren && paren != description ? (size_t)(paren - description) : strlen(description);
   length = std::min(length, sizeof(s_emusuniGpuSteps[0].key) - 1);
   for (EmuSuNiGpuStep &step : s_emusuniGpuSteps) {
      if (step.count && (strncmp(step.key, description, length) != 0 || step.key[length] != 0))
         continue;
      if (!step.count) {
         memcpy(step.key, description, length);
         step.key[length] = 0;
      }
      step.ms += ms;
      step.count++;
      return;
   }
}

void EmuSuNiTiming::GpuFrame(double spanMs, float timestampPeriodNs) {
   s_emusuniGpuSpan += spanMs;
   s_emusuniGpuPeriod = timestampPeriodNs;
   s_emusuniGpuFrames++;
}

bool EmuSuNiTiming::ProfileFrame() {
   if (s_emusuniProfileLeft <= 0)
      return false;
   s_emusuniProfileLeft--;
   return true;
}

// The GPU emulation's counters (never reset in the libretro core): their change per frame.
static void EmuSuNiGpuStats(uint32_t stats[16]) {
   const int values[16] = { gpuStats.numFlips, gpuStats.numDrawCalls, gpuStats.numFlushes,
      gpuStats.numBlockingReadbacks, gpuStats.numReadbacks, gpuStats.numBlockTransfers, gpuStats.numUploads,
      gpuStats.numCopiesForSelfTex, gpuStats.numCopiesForShaderBlend, gpuStats.numColorCopies,
      gpuStats.numDepthCopies, gpuStats.numReinterpretCopies, gpuStats.numTexturesDecoded,
      gpuStats.numFBOsCreated, gpuStats.numDepal, gpuStats.numClears };
   for (int i = 0; i < 16; i++)
      stats[i] = (uint32_t)values[i];
}

static void EmuSuNiLog(uint64_t frames, double seconds, double msPerTick, const uint32_t *stats, const uint32_t *last) {
   using namespace EmuSuNiTiming;
   if (!log_cb || frames == 0 || seconds <= 0.0)
      return;
   uint64_t t[NumSlots];
   uint32_t n[NumSlots];
   for (int i = 0; i < NumSlots; i++) {
      t[i] = Ticks[i].load(std::memory_order_relaxed);
      n[i] = Counts[i].load(std::memory_order_relaxed);
   }
   const double f = (double)frames;
   auto ms = [&](int slot) { return (double)t[slot] * msPerTick / f; };
   auto per = [&](int i) { return (double)(uint32_t)(stats[i] - last[i]) / f; };
   const bool thread = g_Config.bRenderMultiThreading && g_Config.iInflightFrames > 1;
   const double recording = thread ? 0.0 : std::max(0.0, ms(Recording) - ms(Readbacks));
   const double rest = ms(RetroRun) - ms(DisplayLists) - ms(WaitRenderThread) - ms(WaitGpu) - ms(Frontend) - recording;
   char line[512];
   snprintf(line, sizeof(line),
      "psp: %llu frames in %.1f s (%.1f/s), the game's own %.1f/s; per frame: retro_run %.2f ms = display lists %.2f "
      "(blocking readbacks %.2f, %.2f a frame) + %s %.2f (%u times) + waiting for the GPU %.2f (%u times, %d frames in "
      "flight) + set_image and sound %.2f + the CPU and the rest %.2f",
      (unsigned long long)frames, seconds, f / seconds, (double)(uint32_t)(stats[0] - last[0]) / seconds,
      ms(RetroRun), ms(DisplayLists), ms(Readbacks), (double)n[Readbacks] / f,
      thread ? "waiting for the render thread" : "recording and submitting", thread ? ms(WaitRenderThread) : recording,
      thread ? n[WaitRenderThread] : n[Recording], ms(WaitGpu), n[WaitGpu], g_Config.iInflightFrames, ms(Frontend), rest);
   log_cb(RETRO_LOG_INFO, "%s\n", line);
   snprintf(line, sizeof(line),
      "psp: %s %.2f ms per frame (%.1f runs; waiting for pipelines %.2f, %u times; for the queue lock %.3f, %u times; in "
      "vkQueueSubmit %.2f, %.1f a frame); compiled %u pipelines (%.0f ms) and %u shaders (%.0f ms)",
      thread ? "the render thread" : "recording and submitting (no render thread)", ms(Recording),
      (double)n[Recording] / f, ms(WaitPipeline), n[WaitPipeline], ms(QueueLock), n[QueueLock], ms(QueueSubmit),
      (double)n[QueueSubmit] / f, n[CompilePipeline], (double)t[CompilePipeline] * msPerTick, n[CompileShader],
      (double)t[CompileShader] * msPerTick);
   log_cb(RETRO_LOG_INFO, "%s\n", line);
   auto slow = [&](int slot) { return (double)s_emusuniSlowestTicks[slot] * msPerTick; };
   snprintf(line, sizeof(line),
      "psp: the slowest frame: retro_run %.1f ms = display lists %.1f (blocking readbacks %.1f) + waiting for the render "
      "thread %.1f + for the GPU %.1f + set_image and sound %.1f; meanwhile recording %.1f (waiting for pipelines %.1f), "
      "%u pipelines compiled (%.0f ms)",
      slow(RetroRun), slow(DisplayLists), slow(Readbacks), slow(WaitRenderThread), slow(WaitGpu), slow(Frontend),
      slow(Recording), slow(WaitPipeline), s_emusuniSlowestCompiled, slow(CompilePipeline));
   log_cb(RETRO_LOG_INFO, "%s\n", line);
   snprintf(line, sizeof(line),
      "psp: per frame %.0f draws in %.0f batches; readbacks %.2f blocking, %.2f not; %.2f block transfers, %.2f uploads; "
      "copies %.2f for self-texturing, %.2f for blending, %.2f color, %.2f depth, %.2f reinterpreting; %.2f textures "
      "decoded, %.3f framebuffers made, %.2f depal passes, %.1f clears",
      per(1), per(2), per(3), per(4), per(5), per(6), per(7), per(8), per(9), per(10), per(11), per(12), per(13),
      per(14), per(15));
   log_cb(RETRO_LOG_INFO, "%s\n", line);
   if (s_emusuniReadbacks[0].name) {
      size_t at = (size_t)snprintf(line, sizeof(line), "psp: blocking readbacks by reason:");
      for (const EmuSuNiReadbacks &r : s_emusuniReadbacks) {
         if (r.name && at < sizeof(line))
            at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %.2f a frame (%.2f ms each);", r.name,
                                   (double)r.count / f, (double)r.ticks * msPerTick / std::max(r.count, 1u));
      }
      log_cb(RETRO_LOG_INFO, "%s\n", line);
   }
   char settings[192];
   snprintf(settings, sizeof(settings), "%dx%d (%dx), MSAA x%d, render thread %s, Fast Memory %s, %s skinning%s",
      PSP_CoreParameter().renderWidth, PSP_CoreParameter().renderHeight, g_Config.iInternalResolution,
      1 << g_Config.iMultiSampleLevel, thread ? "on" : "off", g_Config.bFastMemory ? "on" : "off",
      g_Config.bSoftwareSkinning ? "software" : "hardware",
      PSP_CoreParameter().compat.flags().FramerateHack30 ? ", the 30 FPS hack" : "");
   if (s_emusuniGpuFrames > 0) {
      const double g = (double)s_emusuniGpuFrames;
      const double work = s_emusuniGpuKind[0] + s_emusuniGpuKind[1] + s_emusuniGpuKind[2] + s_emusuniGpuKind[3] + s_emusuniGpuKind[4];
      snprintf(line, sizeof(line),
         "psp: %s; the GPU (timestamps of %d frames, %.1f ns): %.2f ms of work per frame (%.2f from first to last): passes "
         "%.2f (%.1f), copies %.2f (%.1f), blits %.2f (%.1f), readbacks %.2f (%.1f), the rest %.2f",
         settings, s_emusuniGpuFrames, (double)s_emusuniGpuPeriod, work / g, s_emusuniGpuSpan / g,
         s_emusuniGpuKind[0] / g, s_emusuniGpuKindCount[0] / g, s_emusuniGpuKind[1] / g, s_emusuniGpuKindCount[1] / g,
         s_emusuniGpuKind[2] / g, s_emusuniGpuKindCount[2] / g, s_emusuniGpuKind[3] / g, s_emusuniGpuKindCount[3] / g,
         s_emusuniGpuKind[4] / g);
      log_cb(RETRO_LOG_INFO, "%s\n", line);
      // The heaviest steps (the same one from frame to frame summed).
      EmuSuNiGpuStep *top[4] = {};
      for (EmuSuNiGpuStep &step : s_emusuniGpuSteps) {
         if (!step.count)
            continue;
         EmuSuNiGpuStep *s = &step;
         for (EmuSuNiGpuStep *&slot : top) {
            if (!slot || s->ms > slot->ms)
               std::swap(slot, s);
            if (!s)
               break;
         }
      }
      size_t at = (size_t)snprintf(line, sizeof(line), "psp: the GPU's heaviest steps per frame:");
      for (EmuSuNiGpuStep *s : top) {
         if (s && at < sizeof(line))
            at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %.2f ms (%.1f);", s->key, s->ms / g, s->count / g);
      }
      log_cb(RETRO_LOG_INFO, "%s\n", line);
   } else {
      log_cb(RETRO_LOG_INFO, "psp: %s; the GPU not timed\n", settings);
   }
}

EmuSuNiRunTimer::~EmuSuNiRunTimer() {
   using namespace EmuSuNiTiming;
   const uint64_t end = Now();
   Add(RetroRun, end - start);
   if (end - start > s_emusuniSlowest) {
      s_emusuniSlowest = end - start;
      for (int i = 0; i < NumSlots; i++)
         s_emusuniSlowestTicks[i] = Ticks[i].load(std::memory_order_relaxed) - before[i];
      s_emusuniSlowestCompiled = Counts[CompilePipeline].load(std::memory_order_relaxed) - compiled;
   }
   static uint64_t frames = 0, lastTsc = 0, everyFrames = 0;
   static std::chrono::steady_clock::time_point lastTime;
   static uint32_t lastStats[16];
   const auto now = std::chrono::steady_clock::now();
   uint32_t stats[16];
   EmuSuNiGpuStats(stats);
   if (lastTsc == 0) {
      const char *every = getenv("PSP_TIMING_FRAMES");
      everyFrames = every ? strtoull(every, nullptr, 10) : 0;
   } else {
      frames++;
      const double seconds = std::chrono::duration<double>(now - lastTime).count();
      if (everyFrames ? frames < everyFrames : seconds < 10.0)
         return;
      EmuSuNiLog(frames, seconds, seconds * 1000.0 / (double)(end - lastTsc), stats, lastStats);
   }
   // The next lines' window (and the GPU's timestamps for its first frames).
   for (int i = 0; i < NumSlots; i++) {
      Ticks[i].store(0, std::memory_order_relaxed);
      Counts[i].store(0, std::memory_order_relaxed);
   }
   memset(s_emusuniReadbacks, 0, sizeof(s_emusuniReadbacks));
   memset(s_emusuniGpuSteps, 0, sizeof(s_emusuniGpuSteps));
   memset(s_emusuniGpuKind, 0, sizeof(s_emusuniGpuKind));
   memset(s_emusuniGpuKindCount, 0, sizeof(s_emusuniGpuKindCount));
   s_emusuniGpuSpan = 0.0;
   s_emusuniGpuFrames = 0;
   s_emusuniProfileLeft = 12;
   s_emusuniSlowest = 0;
   memset(s_emusuniSlowestTicks, 0, sizeof(s_emusuniSlowestTicks));
   s_emusuniSlowestCompiled = 0;
   memcpy(lastStats, stats, sizeof(lastStats));
   frames = 0;
   lastTsc = Now();
   lastTime = now;
}
#endif

void retro_run(void)
{
''')

# --- God of War at 30 FPS: PPSSPP's FramerateHack30 (its hook waits two vblanks a frame instead of
# one: half the CPU and GPU work a second, for weak hardware) for the games with GoWFramerateHack60,
# as the frontend's option ppsspp_emusuni_30fps says (disabled/enabled; it can change while playing).
patch('Core/Compatibility.h',
      '\tvoid Load(const std::string &gameID);\n',
      '\tvoid Load(const std::string &gameID);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: PPSSPP\'s 30 FPS hack (FramerateHack30) for the God of War games (GoWFramerateHack60) by\n'
      '\t// the frontend\'s option too (libretro.cpp: ppsspp_emusuni_30fps), on top of what compat.ini says.\n'
      '\tvoid EmuSuNiSetFramerateHack30(bool on) { flags_.FramerateHack30 = emusuniLoaded30_ || (on && flags_.GoWFramerateHack60); }\n'
      '#endif\n')
patch('Core/Compatibility.h',
      '\tstd::string activeList_;\n};\n',
      '\tstd::string activeList_;\n'
      '#ifdef EMUSUNI_PORT\n\tbool emusuniLoaded30_ = false;  // compat.ini\'s FramerateHack30\n#endif\n'
      '};\n')
patch('Core/Compatibility.cpp',
      '\t\tif (compat2.Load(path)) {\n\t\t\tCheckSettings(compat2, gameID);\n\t\t}\n\t}\n',
      '\t\tif (compat2.Load(path)) {\n\t\t\tCheckSettings(compat2, gameID);\n\t\t}\n\t}\n'
      '#ifdef EMUSUNI_PORT\n\temusuniLoaded30_ = flags_.FramerateHack30;\n#endif\n')
patch('Core/Compatibility.cpp',
      '\tmemset(&flags_, 0, sizeof(flags_));\n',
      '\tmemset(&flags_, 0, sizeof(flags_));\n#ifdef EMUSUNI_PORT\n\temusuniLoaded30_ = false;\n#endif\n')
patch('libretro/libretro.cpp',
      '         g_Config.iBloomHack = 3;\n   }\n',
      '         g_Config.iBloomHack = 3;\n   }\n'
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: God of War at 30 FPS (PPSSPP\'s FramerateHack30), applied in retro_run.\n'
      '   var.key = "ppsspp_emusuni_30fps";\n'
      '   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)\n'
      '      g_emusuni30fps = !strcmp(var.value, "enabled");\n'
      '#endif\n')
patch('libretro/libretro.cpp',
      '   retro_input();\n\n   if (useEmuThread)\n',
      '#ifdef EMUSUNI_PORT\n'
      '   // EmuSuNi: the 30 FPS hack as the option says now (the compat flags are the game\'s since its boot).\n'
      '   PSP_CoreParameter().compat.EmuSuNiSetFramerateHack30(g_emusuni30fps);\n'
      '#endif\n'
      '   retro_input();\n\n   if (useEmuThread)\n')

# --- The driver's pipeline cache kept with the shader cache, when EMUSUNI_PSP_PIPELINE_CACHE=1 (off
# unless set): PPSSPP saves only its pipelines' keys and compiles them all again at each start (on the
# PS4 5-140 ms each, 4-5 times longer with MSAA, on the pool's threads beside the game); with the
# VkPipelineCache's data (vkGetPipelineCacheData) those, and every pipeline an earlier session
# compiled, are cache hits. The data is used only when its header and the device's pipelineCacheUUID
# match (another driver build: the keys alone, as before; a file cut short: made again). Its own cache
# version (53 + 1000): a file of the other kind is made again from scratch once.
patch('GPU/Vulkan/ShaderManagerVulkan.h',
      '#include "Common/File/FileUtil.h"\n',
      '#include "Common/File/FileUtil.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <cstdlib>\n'
      '// EmuSuNi: the driver\'s pipeline cache saved with the shader cache (EMUSUNI_PSP_PIPELINE_CACHE=1).\n'
      'inline bool EmuSuNiDriverPipelineCache() {\n'
      '\tstatic const bool on = [] { const char *v = getenv("EMUSUNI_PSP_PIPELINE_CACHE"); return v && atoi(v) != 0; }();\n'
      '\treturn on;\n'
      '}\n'
      '#endif\n')
patch('GPU/Vulkan/GPU_Vulkan.cpp',
      '\t\tresult = pipelineManager_->LoadPipelineCache(f, false, shaderManagerVulkan_, draw_, drawEngine_.GetPipelineLayout(), msaaLevel_);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: with the driver\'s pipeline cache when it is kept (saved below).\n'
      '\t\tresult = pipelineManager_->LoadPipelineCache(f, EmuSuNiDriverPipelineCache(), shaderManagerVulkan_, draw_, drawEngine_.GetPipelineLayout(), msaaLevel_);\n'
      '#else\n'
      '\t\tresult = pipelineManager_->LoadPipelineCache(f, false, shaderManagerVulkan_, draw_, drawEngine_.GetPipelineLayout(), msaaLevel_);\n'
      '#endif\n')
patch('GPU/Vulkan/GPU_Vulkan.cpp',
      '\tpipelineManager_->SavePipelineCache(f, false, shaderManagerVulkan_, draw_);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tpipelineManager_->SavePipelineCache(f, EmuSuNiDriverPipelineCache(), shaderManagerVulkan_, draw_);  // EmuSuNi\n'
      '#else\n'
      '\tpipelineManager_->SavePipelineCache(f, false, shaderManagerVulkan_, draw_);\n'
      '#endif\n')
patch('GPU/Vulkan/ShaderManagerVulkan.cpp',
      '#define CACHE_VERSION 53\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: another version for the files with the driver\'s pipeline cache (GPU_Vulkan.cpp).\n'
      '#define CACHE_VERSION (EmuSuNiDriverPipelineCache() ? 53 + 1000 : 53)\n'
      '#else\n'
      '#define CACHE_VERSION 53\n'
      '#endif\n')
patch('GPU/Vulkan/PipelineManagerVulkan.cpp',
      '\t\tVkResult result = vkGetPipelineCacheData(vulkan_->GetDevice(), pipelineCache_, &dataSize, nullptr);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: none made yet (no pipeline this session): nothing to save.\n'
      '\t\tVkResult result = pipelineCache_ ? vkGetPipelineCacheData(vulkan_->GetDevice(), pipelineCache_, &dataSize, nullptr) : VK_INCOMPLETE;\n'
      '#else\n'
      '\t\tVkResult result = vkGetPipelineCacheData(vulkan_->GetDevice(), pipelineCache_, &dataSize, nullptr);\n'
      '#endif\n')
patch('GPU/Vulkan/PipelineManagerVulkan.cpp',
      '\t\tauto buffer = std::make_unique<uint8_t[]>(size);\n'
      '\t\tsuccess = fread(buffer.get(), 1, size, file) == size;\n'
      '\t\t// Verify header.\n'
      '\t\tVkPipelineCacheHeader *header = (VkPipelineCacheHeader *)buffer.get();\n'
      '\t\tif (!success || header->version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {\n'
      '\t\t\t// Bad header, don\'t do anything.\n'
      '\t\t\tWARN_LOG(Log::G3D, "Bad Vulkan pipeline cache header - ignoring");\n'
      '\t\t\treturn false;\n'
      '\t\t}\n'
      '\t\tif (0 != memcmp(header->uuid, vulkan_->GetPhysicalDeviceProperties().properties.pipelineCacheUUID, VK_UUID_SIZE)) {\n'
      '\t\t\t// Wrong hardware/driver/etc.\n'
      '\t\t\tWARN_LOG(Log::G3D, "Bad Vulkan pipeline cache UUID - ignoring");\n'
      '\t\t\treturn false;\n'
      '\t\t}\n'
      '\n'
      '\t\tVkPipelineCacheCreateInfo pc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };\n'
      '\t\tpc.pInitialData = buffer.get();\n'
      '\t\tpc.initialDataSize = size;\n',
      '\t\tauto buffer = std::make_unique<uint8_t[]>(size);\n'
      '\t\tsuccess = fread(buffer.get(), 1, size, file) == size;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: a file cut short is made again; another driver\'s data leaves the keys below, compiled\n'
      '\t\t// as before.\n'
      '\t\tif (!success) {\n'
      '\t\t\tWARN_LOG(Log::G3D, "Vulkan pipeline cache cut short - ignoring");\n'
      '\t\t\treturn false;\n'
      '\t\t}\n'
      '\t\tconst VkPipelineCacheHeader *header = (const VkPipelineCacheHeader *)buffer.get();\n'
      '\t\tconst bool usable = size >= sizeof(VkPipelineCacheHeader) && header->version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&\n'
      '\t\t\t0 == memcmp(header->uuid, vulkan_->GetPhysicalDeviceProperties().properties.pipelineCacheUUID, VK_UUID_SIZE);\n'
      '\t\tif (!usable)\n'
      '\t\t\tWARN_LOG(Log::G3D, "EmuSuNi: the Vulkan pipeline cache is another driver\'s - its pipelines compiled again");\n'
      '\n'
      '\t\tVkPipelineCacheCreateInfo pc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };\n'
      '\t\tpc.pInitialData = usable ? buffer.get() : nullptr;\n'
      '\t\tpc.initialDataSize = usable ? size : 0;\n'
      '#else\n'
      '\t\t// Verify header.\n'
      '\t\tVkPipelineCacheHeader *header = (VkPipelineCacheHeader *)buffer.get();\n'
      '\t\tif (!success || header->version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {\n'
      '\t\t\t// Bad header, don\'t do anything.\n'
      '\t\t\tWARN_LOG(Log::G3D, "Bad Vulkan pipeline cache header - ignoring");\n'
      '\t\t\treturn false;\n'
      '\t\t}\n'
      '\t\tif (0 != memcmp(header->uuid, vulkan_->GetPhysicalDeviceProperties().properties.pipelineCacheUUID, VK_UUID_SIZE)) {\n'
      '\t\t\t// Wrong hardware/driver/etc.\n'
      '\t\t\tWARN_LOG(Log::G3D, "Bad Vulkan pipeline cache UUID - ignoring");\n'
      '\t\t\treturn false;\n'
      '\t\t}\n'
      '\n'
      '\t\tVkPipelineCacheCreateInfo pc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };\n'
      '\t\tpc.pInitialData = buffer.get();\n'
      '\t\tpc.initialDataSize = size;\n'
      '#endif\n')

# --- God of War: Ghost of Sparta without the first-frame readbacks (compat.ini's DisableFirstFrameReadback,
# a section upstream has no games in): PPSSPP copies each new framebuffer's first frame to the PSP's
# memory, a blocking readback - the render thread and the GPU drained, a hitch each time - for games
# that use that memory later; this one doesn't (the same pictures through its intro and first fight).
patch('assets/compat.ini',
      '[GoWFramerateHack60]\n',
      '[DisableFirstFrameReadback]\n'
      '# EmuSuNi (scripts/build-psp-core.sh): no blocking readback of each new framebuffer\'s first frame\n'
      '# (a hitch each time); the game never reads that memory.\n'
      '# GOW : Ghost of Sparta\n'
      'UCUS98737 = true\nUCAS40323 = true\nNPHG00092 = true\nNPEG00044 = true\nNPEG00045 = true\n'
      'NPJG00120 = true\nNPUG80508 = true\nUCJS10114 = true\nUCES01401 = true\nUCES01473 = true\n'
      '# GOW : Ghost of Sparta Demo\n'
      'NPEG90035 = true\nNPUG70125 = true\nNPJG90095 = true\n'
      '\n'
      '[GoWFramerateHack60]\n')

written = 0
for (repo, path), text in patched.items():
    full = repo + '/' + path
    try:
        with open(full, encoding='utf-8') as f:
            same = f.read() == text
    except FileNotFoundError:
        same = False
    if not same:
        with open(full, 'w', encoding='utf-8') as f:
            f.write(text)
        written += 1
print("PPSSPP: patches applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-psp-core.sh: the ppsspp core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f ppsspp_$f"; done
} > "$OUT/prefix_ppsspp.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_ppsspp.h.new" "$OUT/prefix_ppsspp.h"; then
  rm -f "$OUT/prefix_ppsspp.h.new"
else
  mv "$OUT/prefix_ppsspp.h.new" "$OUT/prefix_ppsspp.h"
fi

if [ "$HOST" != 1 ]; then
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  PS4C="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -funwind-tables -isysroot $OPENORBIS"
  PS4C+=" -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay -isystem $OPENORBIS/include -I$OPENORBIS/usr/include"
fi
: > "$LOG"

# --- FFmpeg 3.0.2 (PPSSPP's fork, its ffmpeg/ submodule): the decoders, demuxers and parsers of
# PPSSPP's own build scripts (ffmpeg/linux_x86-64.sh) without ATRAC3/3+ (PPSSPP's own decoder, whose
# symbols would clash) and the encoders/muxers (video dumps), no threads, no external libraries,
# FFmpeg's C and inline x86 code (no external assembler, as upstream builds it). On the PS4 its
# configure links its tests against OpenOrbis' libraries, so it finds what the PS4 really has.
FF_SRC="$SRC/ffmpeg"
FF_BUILD="$OUT/ffmpeg-build"
FF_PREFIX="$OUT/ffmpeg"
FF_CFLAGS="-O3 -march=btver2 -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion"
FF_CFLAGS+=" -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types"
[ "$HOST" = 1 ] && FF_CFLAGS+=" -mno-sse4a"
FF_OPTS=(
  --prefix="$FF_PREFIX" --enable-static --disable-shared --enable-pic --arch=x86_64
  --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-postproc
  --disable-network --disable-everything --disable-encoders --disable-muxers --disable-hwaccels
  --disable-yasm --disable-pthreads --disable-w32threads --disable-os2threads
  --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl --disable-xlib
  --disable-vaapi --disable-vdpau --disable-vda --disable-d3d11va --disable-dxva2
  --disable-securetransport --disable-schannel
  --enable-decoder=h264,mpeg4,h263,h263p,mpeg2video,mjpeg,mjpegb,aac,aac_latm,mp3,pcm_s16le,pcm_s8
  --enable-demuxer=h264,h263,m4v,mpegps,mpegvideo,avi,mp3,aac,pmp,pcm_s16le,pcm_s8,wav
  --enable-parser=h264,mpeg4video,mpegvideo,aac,aac_latm,mpegaudio
  --enable-protocol=file
)
if [ "$HOST" = 1 ]; then
  FF_OPTS+=(--cc=clang-21 "--extra-cflags=$FF_CFLAGS")
else
  FF_OPTS+=(
    --enable-cross-compile --target-os=freebsd --cc=clang-21 --cxx=clang++-21 --ar=llvm-ar-21
    --ranlib=llvm-ranlib-21 --nm=llvm-nm-21 --strip=llvm-strip-21 "--extra-cflags=$PS4C $FF_CFLAGS"
    "--extra-ldflags=--target=x86_64-pc-freebsd12-elf -fuse-ld=lld -nostdlib -Wl,-pie -Wl,--eh-frame-hdr -Wl,--script=$OPENORBIS/link.x -L$OPENORBIS/lib -L$OPENORBIS/usr/lib"
    "--extra-libs=-lc -lclang_rt.builtins-x86_64 -lkernel -lSceLibcInternal"
  )
fi
FF_STAMP="$FF_PREFIX/.emusuni-flags"
FF_WANT="$(git -C "$FF_SRC" rev-parse HEAD 2>/dev/null || echo unknown) | host $HOST | ${FF_OPTS[*]}"
if [ "${2:-}" = clean ] || [ "$(cat "$FF_STAMP" 2>/dev/null)" != "$FF_WANT" ] || [ ! -f "$FF_PREFIX/lib/libavcodec.a" ]; then
  echo "PPSSPP: building FFmpeg $(cat "$FF_SRC/RELEASE" 2>/dev/null) (new flags or clean)"
  rm -rf "$FF_BUILD" "$FF_PREFIX"
  mkdir -p "$FF_BUILD"
  if ! (cd "$FF_BUILD" && env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS -u CC -u CXX \
          "$FF_SRC/configure" "${FF_OPTS[@]}") >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    tail -30 "$FF_BUILD/ffbuild/config.log" 2>/dev/null || tail -30 "$FF_BUILD/config.log" 2>/dev/null || true
    echo "PPSSPP: configuring FFmpeg failed (see $LOG)"
    exit 1
  fi
  if ! env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS make -C "$FF_BUILD" -j "$JOBS" install >> "$LOG" 2>&1; then
    grep -E "error:|Error " "$LOG" | head -30
    echo "PPSSPP: building FFmpeg failed (see $LOG)"
    exit 1
  fi
  printf '%s' "$FF_WANT" > "$FF_STAMP"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): CMake's -O3, the
# PS4's CPU and the renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -include $OUT/prefix_ppsspp.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a"
# The PS4: a FreeBSD for armips' ghc::filesystem (OpenOrbis' sys/param.h doesn't say BSD).
[ "$HOST" != 1 ] && FLAGS_CORE+=" -DGHC_OS_DETECTED -DGHC_OS_BSD"
OPTIONS=(
  -DLIBRETRO=ON -DEMUSUNI_PORT=ON -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
  -DUSE_FFMPEG=ON -DUSE_SYSTEM_FFMPEG=OFF "-DFFMPEG_DIR=$FF_PREFIX"
  -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_ZSTD=OFF
  -DUSE_SYSTEM_LIBZIP=OFF -DUSE_SYSTEM_SNAPPY=OFF -DUSE_SYSTEM_LIBCHDR=OFF -DUSE_SYSTEM_FREETYPE=OFF
  -DUSE_SYSTEM_RAPIDJSON=OFF -DUSE_SYSTEM_MINIUPNPC=OFF -DUSE_SYSTEM_LIBSDL2=OFF
  -DHEADLESS=OFF -DUNITTEST=OFF -DATLAS_TOOL=OFF -DUSE_CCACHE=OFF -DUSING_QT_UI=OFF -DOPENXR=OFF
  -DUSING_X11_VULKAN=OFF -DUSE_WAYLAND_WSI=OFF -DUSE_VULKAN_DISPLAY_KHR=OFF -DUSE_ASAN=OFF -DUSE_UBSAN=OFF
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CORE"
)
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  }
else
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
COMMITS="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
COMMITS+=" $(git -C "$SRC" submodule status 2>/dev/null | awk '{print $1}' | tr -d '+-' | tr '\n' ' ')"
WANT="$SRC | $COMMITS | host $HOST | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "PPSSPP: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "PPSSPP: configuring failed (see $LOG)"
    exit 1
  fi
fi
# (-k 0: every error at once, not only the first.)
if ! ninja -C "$BUILD" -j "$JOBS" -k 0 ppsspp_libretro >> "$LOG" 2>&1; then
  grep -E "error:" "$LOG" | sort | uniq -c | sort -rn | head -60
  echo "PPSSPP: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: only the libretro API (renamed) stays global. Every library the core target was made
# of (Core, Common, GPU, glslang, SPIRV-Cross, armips, rcheevos, Lua, libchdr, zstd, zlib, libpng,
# libzip, snappy, basis_universal, ...) and FFmpeg's. The 7-Zip reader (ext/lzma-sdk) only for what
# the rest still needs: its LZMA decoder files are libchdr's (the same files), as upstream's link
# takes one of them.
# Replaced only when it changed (the app isn't linked again for nothing).
mapfile -t ARCHIVES < <(find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -not -name 'lib7zip.a' | sort)
ARCHIVES+=("$FF_PREFIX/lib/libavformat.a" "$FF_PREFIX/lib/libavcodec.a" "$FF_PREFIX/lib/libswresample.a"
           "$FF_PREFIX/lib/libswscale.a" "$FF_PREFIX/lib/libavutil.a")
ld.lld-21 -r --whole-archive "${ARCHIVES[@]}" --no-whole-archive "$BUILD/lib/lib7zip.a" -o "$OUT/psp_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='ppsspp_retro_*' "$OUT/psp_core_all.o" "$OUT/psp_core.o.new"
rm -f "$OUT/psp_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/psp_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "ppsspp_$f"; done | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "PPSSPP: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/psp_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved: the app would export them as imports nobody gives (the
# PS4's loader may refuse those).
WEAK=$(llvm-nm-21 -u "$OUT/psp_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "PPSSPP: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if llvm-nm-21 --defined-only "$OUT/psp_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "PPSSPP: the object defines a C++ operator new/delete of its own"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  # What the app gives it: operator new/delete (port/ps4_new.cpp) and the executable memory
  # (port/ps4_n64.c), compiled here as the app compiles them, only to list their symbols.
  PS4C_CHECK="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C_CHECK+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay"
  PS4C_CHECK+=" -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -march=btver2"
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $PS4C_CHECK -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  clang-21 $PS4C_CHECK -O2 -c "$PROJECT/port/ps4_n64.c" -o "$OUT/check_ps4_n64.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores. The threads' hook,
  # emusuni_psp_thread, is the app's to define in port/: a note if it isn't yet.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o" | awk 'NF >= 3 {print $3}';
                 echo __dso_handle; echo emusuni_psp_thread; } | sort -u)
  rm -f "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o"
  if ! grep -qs "emusuni_psp_thread" "$PROJECT"/port/*.c "$PROJECT"/port/*.cpp; then
    echo "PPSSPP: note: port/ doesn't define emusuni_psp_thread yet (the app won't link without it)"
  fi
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "PPSSPP: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
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
    echo "PPSSPP: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "PPSSPP: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app" \
       "($(echo "$NEW_DELETE" | sed '/^$/d' | wc -l) operator new/delete, port/ps4_new.cpp's)"
fi
if cmp -s "$OUT/psp_core.o.new" "$OUT/psp_core.o"; then
  rm -f "$OUT/psp_core.o.new"
else
  mv "$OUT/psp_core.o.new" "$OUT/psp_core.o"
fi
ls -la "$OUT/psp_core.o"
