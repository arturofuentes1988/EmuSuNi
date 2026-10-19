#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Flycast (flyinghead/flycast v2.7, ~/src/flycast with the submodules its libretro core
# needs, GPL-2.0-or-later: the Dreamcast emulator) for the PS4 into one object that scripts/build.sh
# links into EmuSuNi: the Dreamcast. Its own CMake (LIBRETRO=ON) makes the libretro core with the
# PS4 toolchain (toolchain/ps4-love-modern.cmake): the x86-64 recompilers of the SH4 (the main CPU),
# the ARM7 (the sound CPU) and the AICA's DSP, the Vulkan renderer (internal resolution, alpha
# sorting per triangle or per pixel...; its GLSL compiled at run time by glslang), GDI/CHD/CDI/CUE/
# ELF read by the core itself (stdio, never mapped or cached whole), the BIOS emulated (reios) when
# <system>/dc has no dc_boot.bin and dc_flash.bin. Its libretro API becomes flycast_retro_* and
# everything else in it private, as for FCEUmm (CMakeLists.txt). The PS4 changes to its sources
# (applied each time to the files as git has them, checked; all under EMUSUNI_PORT, which the CMake
# option of that name defines):
#   - The Dreamcast's memory without the virtual memory: upstream maps the RAM, VRAM and sound RAM
#     several times (their mirrors) in a 512 MiB range, which the PS4 doesn't allow (PPSSPP's mirrors
#     came out "not aliased"), so Flycast's own fallback (nvmem disabled): the recompiled code goes
#     through the memory handlers (no fast memory; ~3% slower on this PC's RAM-heavy test). RAM
#     (16 MiB), VRAM (8 MiB), sound RAM (2 MiB) and the SH4 context with its 128 MiB table of
#     compiled blocks each in a block of their own, 16 KiB aligned, off the app's heap: direct memory
#     on the PS4 (sceKernelAllocateDirectMemory, sceKernelMapDirectMemory), an mmap elsewhere
#     (oslib/emusuni_mem.cpp). Freed by retro_deinit, allocated again by every game's start; the SH4
#     executors follow the context wherever it lands (upstream's kept its first address: a crash
#     when it moved). The table is reset only where blocks were (upstream wrote all 128 MiB at every
#     flush of the code cache).
#   - Pages are still write-protected (mprotect) as Flycast wants: the textures' VRAM (a write drops
#     the texture) and the RAM of compiled code (a write drops its blocks). The core's fault handler
#     (linux/common.cpp) takes only those faults: on the PS4 with the FreeBSD kernel's values written
#     out (SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, si_addr 24 bytes in; struct sigaction checked at
#     compile time), no alternate stack, every other fault passed on to the handler before (the app's
#     crash log) as the kernel would. When it's installed a page is protected and written once: if
#     that doesn't fault and come back, nothing is protected (no texture invalidation; the
#     recompiler then checks the code of each block when it runs).
#   - The recompilers' code memory (SH4 11 MiB, ARM7 4 MiB, DSP 32 KiB: arrays in the core's data, 16
#     KiB aligned, within 2 GiB of its code) is made executable once through EmuSuNi (port/ps4_n64.c:
#     emusuni_make_executable, the ways the PS4 allows) instead of mmap/mprotect. If that fails, the
#     interpreters run (the SH4's, the ARM7's and the DSP's, all built in).
#   - Threads: every thread Flycast names ("Flycast-emu", the emulation with threaded rendering, a
#     new one at every start: game start, reset, save state loaded; "CustomTexLoader", "PicoTCP"...)
#     first calls emusuni_dc_thread(name), which the app defines (port/): its core, priority and name.
#   - libretro: the emulator set up once per process (retro_deinit frees the Dreamcast's memory), as
#     upstream does on Linux, on the PS4 too (upstream's other path can't start again after its
#     term()). The frontend's functions only from the thread of retro_run (and retro_load_game):
#     rumble, messages and frame rate changes of the emulation thread wait for the next retro_run.
#     The pads may be set before retro_load_game (then none is unplugged at the first frames, which
#     matters for a state loaded at once): a VMU option changed since the last game no longer
#     crashes it there (upstream's reconnection read the SH4 context retro_deinit had freed), nor a
#     renderer change (the Transparency's sorting): the renderer isn't made again before the
#     graphics context exists (upstream's made it without one).
#   - Vulkan: the renderer adopts the frontend's device. EmuSuNi makes its only VkDevice at start
#     (src/Display.cpp) and never calls the context negotiation's create_device (where upstream loads
#     Vulkan's functions and looks at the device): the functions come from the frontend's
#     get_instance_proc_addr, instance and device, and what the device was made with from
#     EMUSUNI_VK_DEVICE_FEATURES (VkPhysicalDeviceFeatures names, plus provokingVertexLast) and
#     EMUSUNI_VK_DEVICE_EXTENSIONS, comma-separated (none when unset: no anisotropic filtering, and
#     per-triangle sorting even when per-pixel is asked for, which needs fragmentStoresAndAtomics;
#     upstream only hid that choice). Every queue submit and wait of the core under the frontend's
#     lock_queue / unlock_queue. The frame images in a ring of 3 of the core's own, not the frontend's
#     sync index (the frontend may call retro_run several times between its frames, or draw on another
#     thread meanwhile); their render pass waits for the frontend's sampling of earlier images and
#     makes its writes visible to it.
#   - Pipelines and shaders: the driver's pipeline cache (<system>/dc/vulkan_pipeline.cache) used only
#     when its header is this GPU's and driver's (vendor, device, pipelineCacheUUID), written beside
#     and renamed at the end and every 600 frames when pipelines were made; the GLSL compiled to
#     SPIR-V once (<system>/dc/vulkan_spirv.cache, by a hash of each shader, Flycast's version in its
#     header).
#   - A line in the log every 10 s (retro_run), per frame: retro_run's time, the renderer's and its
#     waits for the emulation, the emulation thread's CPU time and its waits for the renderer, the
#     pipelines and shaders made and their time, the GPU waits and full GPU syncs.
#   - Built as a static library, without LTO, with its own zlib, zstd (without its tracing hooks),
#     libchdr, libzip, glslang... (nothing of the system's), without UPnP, Lua, breakpad, Discord,
#     OpenMP or the GDB server, for 16 KiB pages. The PS4's CMake system is "Generic": asio without
#     kqueue, libzip without the Annex K functions the PS4's libc has but doesn't declare. As one
#     object: libchdr's LZMA SDK (24.05) only for what the rest doesn't have (the core's own 7-Zip
#     copy has the same functions, which upstream's link takes).
# The object is checked at the end: only flycast_retro_* defined globally, no weak references left,
# the C++ operator new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and
# none defined in it, and every other symbol it needs found in what the app links (OpenOrbis' libc,
# libkernel, libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c;
# emusuni_dc_thread from port/).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-dc-core.sh <out dir> [clean]
#   DC_CORE_HOST=1 build-dc-core.sh <out dir>   the same object for this PC (Linux, clang 21), for the
#                                               PC tests: no PS4 checks; the test frontend provides
#                                               emusuni_make_executable and emusuni_dc_thread.
#                                               Run with EMUSUNI_DC_VMEM=1: upstream's virtual memory
#                                               (to compare); EMUSUNI_DC_FRESH_ADDRESSES=1: freed
#                                               memory never given again, any use of it a fault
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${FLYCAST_DIR:-$HOME/src/flycast}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/dc-core.log"
HOST="${DC_CORE_HOST:-0}"
JOBS="${DC_CORE_JOBS:-4}"   # at most 4 compilers at once (the PC's load)

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
    key = (repo or root, path)
    if key not in patched:
        patched[key] = git_file(key[0], path)
    text = patched[key]
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[key] = text.replace(old, new)
def addfile(path, text):
    # A file of EmuSuNi's own in the tree (git doesn't know it).
    patched[(root, path)] = text

# --- The build: the EMUSUNI_PORT option, a static libretro core, 16 KiB pages, no UPnP.
patch('CMakeLists.txt',
      'option(LIBRETRO "Build libretro core" OFF)\n',
      'option(LIBRETRO "Build libretro core" OFF)\n'
      '# EmuSuNi (scripts/build-dc-core.sh): the libretro core as one object of the PS4 app (a static\n'
      '# library), with the changes under EMUSUNI_PORT.\n'
      'option(EMUSUNI_PORT "Build the libretro core for EmuSuNi (PS4)" OFF)\n'
      'if(EMUSUNI_PORT)\n\tadd_compile_definitions(EMUSUNI_PORT=1)\n'
      '\t# No zstd tracing hooks (weak references nothing defines).\n'
      '\tadd_compile_definitions(ZSTD_TRACE=0)\n'
      '\tif(NOT UNIX)\n'
      '\t\t# The PS4 (CMake\'s "Generic" system, FreeBSD headers): asio\'s select reactor, no kqueue.\n'
      '\t\tadd_compile_definitions(ASIO_DISABLE_KQUEUE)\n'
      '\tendif()\n'
      'endif()\n')
patch('CMakeLists.txt',
      'elseif(LIBRETRO)\n\tadd_library(${PROJECT_NAME} SHARED core/emulator.cpp)\n',
      'elseif(LIBRETRO)\n'
      '\tif(EMUSUNI_PORT)\n'
      '\t\t# EmuSuNi: linked into the app (scripts/build-dc-core.sh makes one object of it).\n'
      '\t\tadd_library(${PROJECT_NAME} STATIC core/emulator.cpp)\n'
      '\telse()\n'
      '\t\tadd_library(${PROJECT_NAME} SHARED core/emulator.cpp)\n'
      '\tendif()\n')
patch('CMakeLists.txt',
      'if(UNIX AND NOT ANDROID AND NOT APPLE)\n'
      '\texecute_process(COMMAND getconf PAGESIZE OUTPUT_VARIABLE PAGE_SIZE OUTPUT_STRIP_TRAILING_WHITESPACE)\n',
      'if(EMUSUNI_PORT)\n'
      '\t# EmuSuNi: the PS4\'s pages (16 KiB), on the PC tests too (a multiple of its 4 KiB ones).\n'
      '\ttarget_compile_definitions(${PROJECT_NAME} PRIVATE PAGE_SIZE=16384)\n'
      'elseif(UNIX AND NOT ANDROID AND NOT APPLE)\n'
      '\texecute_process(COMMAND getconf PAGESIZE OUTPUT_VARIABLE PAGE_SIZE OUTPUT_STRIP_TRAILING_WHITESPACE)\n')
patch('CMakeLists.txt',
      'if(NOT MINIUPNP_FOUND)\n\tif(NINTENDO_SWITCH)\n',
      'if(NOT MINIUPNP_FOUND)\n\tif(NINTENDO_SWITCH OR EMUSUNI_PORT)  # EmuSuNi: no UPnP\n')
patch('core/oslib/CMakeLists.txt',
      'target_sources(${PROJECT_NAME} PRIVATE\n        directory.cpp\n',
      'target_sources(${PROJECT_NAME} PRIVATE\n        emusuni_mem.cpp\n        emusuni_timing.cpp\n        directory.cpp\n')

# --- The Dreamcast's memory without the virtual memory: the PS4 can't map memory twice (PPSSPP's VRAM
# mirrors came out "NOT aliased"), so Flycast's own fallback (nvmem disabled): RAM, VRAM, ARAM and the
# SH4 context with its 128 MiB jump table in blocks of their own (direct memory on the PS4), the
# recompiler calling the memory handlers (no fast memory, no rewriting of faulting accesses). Pages of
# them are still write-protected (texture cache in VRAM, compiled code in RAM): a write faults and
# the core's handler (linux/common.cpp) unprotects the page and invalidates what was there.
addfile('core/oslib/emusuni_mem.cpp', r'''// EmuSuNi (scripts/build-dc-core.sh): the Dreamcast's memory blocks, each in pages of its own, mapped
// here instead of on the heap: on the PS4 direct memory (the 4.5 GiB pool), elsewhere an anonymous
// mmap. Pages of them are write-protected (mprotect) for the texture cache and the recompiler's
// blocks, which must not touch the heap's own data; the 128 MiB SH4 jump table stays off the app's
// heap. If the system can't give direct memory: an mmap (on the PS4 the app's, from flexible memory).
#include "types.h"

#ifdef EMUSUNI_PORT

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <sys/types.h>

#if defined(__ORBIS__)
extern "C" {
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t alignment, int32_t type, off_t *phys);
int32_t sceKernelMapDirectMemory(void **addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t alignment);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
int32_t sceKernelMunmap(void *addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
}
#endif

// Whether a write-protected page faults and the core's handler gets it (linux/common.cpp tests it
// once when it installs the handler): until then, and if not, nothing is write-protected.
bool emusuniWriteProtectOk = false;
bool emusuniWriteProtectTested = false;  // (the test made)

namespace {
constexpr size_t PAGE = 16384;
struct Block {
	void *at;
	size_t size;
	off_t phys;
	bool direct;
};
std::mutex g_lock;
Block g_blocks[32];
int g_count = 0;
}  // namespace

void *emusuni_pages_alloc(size_t size)
{
	// (A size of 0, the Elan RAM of a Dreamcast: a page, a pointer of its own as upstream's.)
	size = size == 0 ? PAGE : (size + PAGE - 1) & ~(PAGE - 1);
	void *at = nullptr;
	off_t phys = 0;
	bool direct = false;
#if defined(__ORBIS__)
	constexpr int32_t PROT_CPU_RW = 0x03;  // SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE
	constexpr int32_t WB_ONION = 0;        // SCE_KERNEL_WB_ONION: CPU write-back memory
	if (sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), size, 64 * 1024, WB_ONION, &phys) == 0)
	{
		// A hint well above 0x200000000, where the kernel library puts its own objects.
		void *base = (void *)0x2400000000ull;
		const int32_t ret = sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, PAGE);
		if (ret == 0 && base != nullptr)
		{
			at = base;
			direct = true;
			memset(at, 0, size);  // direct memory comes as it was left
		}
		else
		{
			WARN_LOG(VMEM, "EmuSuNi: mapping %d KiB of direct memory failed (%08x)", (int)(size / 1024), (unsigned)ret);
			sceKernelReleaseDirectMemory(phys, size);
		}
	}
#endif
	if (at == nullptr)
	{
		// 16 KiB aligned (a write protection covers whole 16 KiB pages, never a neighbour's): a page
		// more mapped, what lies outside the aligned block unmapped again.
		void *p = mmap(nullptr, size + PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (p == MAP_FAILED)
		{
			ERROR_LOG(VMEM, "EmuSuNi: no memory for %d KiB", (int)(size / 1024));
			return nullptr;
		}
		const uintptr_t start = (uintptr_t)p, aligned = (start + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
		if (aligned > start)
			munmap(p, aligned - start);
		if (aligned + size < start + size + PAGE)
			munmap((void *)(aligned + size), start + size + PAGE - (aligned + size));
		at = (void *)aligned;
	}
	{
		std::lock_guard<std::mutex> guard(g_lock);
		if (g_count < (int)(sizeof(g_blocks) / sizeof(g_blocks[0])))
			g_blocks[g_count++] = { at, size, phys, direct };
		else
			ERROR_LOG(VMEM, "EmuSuNi: too many memory blocks (%p kept)", at);
	}
	DEBUG_LOG(VMEM, "EmuSuNi: %d KiB of %s at %p", (int)(size / 1024), direct ? "direct memory" : "mapped memory", at);
	return at;
}

void emusuni_pages_free(void *p)
{
	if (p == nullptr)
		return;
	Block block {};
	{
		std::lock_guard<std::mutex> guard(g_lock);
		for (int i = 0; i < g_count; i++)
		{
			if (g_blocks[i].at == p)
			{
				block = g_blocks[i];
				g_blocks[i] = g_blocks[--g_count];
				break;
			}
		}
	}
	if (block.at == nullptr)
	{
		ERROR_LOG(VMEM, "EmuSuNi: freeing %p, not a block of emusuni_pages_alloc", p);
		return;
	}
#if defined(__ORBIS__)
	if (block.direct)
	{
		sceKernelMunmap(block.at, block.size);
		sceKernelReleaseDirectMemory(block.phys, block.size);
		return;
	}
#else
	// (The PC tests, EMUSUNI_DC_FRESH_ADDRESSES=1: the addresses of a freed block never given again
	// and any use of them a fault, so a pointer still into the memory of the game before shows.)
	static const bool fresh = getenv("EMUSUNI_DC_FRESH_ADDRESSES") != nullptr;
	if (fresh)
	{
		mmap(block.at, block.size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
		return;
	}
#endif
	munmap(block.at, block.size);
}

#endif  // EMUSUNI_PORT
''')
patch('core/linux/posix_vmem.cpp',
      'bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)\n{\n',
      'bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: no virtual memory (its mirrors are the same memory mapped several times, which the\n'
      '\t// PS4 doesn\'t allow): Flycast\'s fallback, the memory blocks of emusuni_mem.cpp. (The PC tests\n'
      '\t// may compare with upstream\'s: EMUSUNI_DC_VMEM=1.)\n'
      '#if !defined(__ORBIS__)\n'
      '\tif (getenv("EMUSUNI_DC_VMEM") == nullptr)\n'
      '#endif\n'
      '\t\treturn false;\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'namespace virtmem\n{\n\nbool region_lock(void *start, size_t len)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern bool emusuniWriteProtectOk, emusuniWriteProtectTested;  // oslib/emusuni_mem.cpp\n'
      'extern "C" int emusuni_make_executable(void *address, size_t size);  // port/ps4_n64.c\n'
      '#endif\n\n'
      'namespace virtmem\n{\n\nbool region_lock(void *start, size_t len)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!emusuniWriteProtectOk)\n'
      '\t\treturn false;  // EmuSuNi: no page protection here (linux/common.cpp\'s test)\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      '\tif (mprotect((u8*)start - inpage, len + inpage, PROT_READ))\n\t\tdie("mprotect failed...");\n',
      '\tif (mprotect((u8*)start - inpage, len + inpage, PROT_READ))\n'
      '\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tERROR_LOG(VMEM, "EmuSuNi: write-protecting %p (%d KiB) failed: errno %d", start, (int)(len / 1024), errno);\n'
      '#endif\n'
      '\t\tdie("mprotect failed...");\n'
      '\t}\n')
patch('core/linux/posix_vmem.cpp',
      '\tif (mprotect((u8*)start - inpage, len + inpage, PROT_READ | PROT_WRITE))\n'
      '\t\t// Add some way to see why it failed? gdb> info proc mappings\n'
      '\t\tdie("mprotect  failed...");\n',
      '\tif (mprotect((u8*)start - inpage, len + inpage, PROT_READ | PROT_WRITE))\n'
      '\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tERROR_LOG(VMEM, "EmuSuNi: unprotecting %p (%d KiB) failed: errno %d", start, (int)(len / 1024), errno);\n'
      '#endif\n'
      '\t\t// Add some way to see why it failed? gdb> info proc mappings\n'
      '\t\tdie("mprotect  failed...");\n'
      '\t}\n')
patch('core/linux/posix_vmem.cpp',
      'bool region_unlock(void *start, size_t len)\n{\n',
      'bool region_unlock(void *start, size_t len)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (emusuniWriteProtectTested && !emusuniWriteProtectOk)\n'
      '\t\treturn false;  // EmuSuNi: nothing was write-protected (and mprotect may be refused)\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)\n{\n',
      'bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the code cache (an array in the core\'s data, virtmem.h) made executable the ways the\n'
      '\t// PS4 allows (port/ps4_n64.c), else no JIT (the interpreters run). Once: it stays so (the DSP\'s\n'
      '\t// is asked for again at every reset of the AICA, while nothing else of the core runs).\n'
      '\tstatic void *executable[8];\n'
      '\tstatic int executableCount = 0;\n'
      '\tfor (int i = 0; i < executableCount; i++)\n'
      '\t\tif (executable[i] == code_area)\n'
      '\t\t{\n'
      '\t\t\t*code_area_rwx = code_area;\n'
      '\t\t\treturn true;\n'
      '\t\t}\n'
      '\tif (code_area != nullptr && emusuni_make_executable(code_area, size) >= 0)\n'
      '\t{\n'
      '\t\tif (executableCount < 8)\n'
      '\t\t\texecutable[executableCount++] = code_area;\n'
      '\t\t*code_area_rwx = code_area;\n'
      '\t\tNOTICE_LOG(VMEM, "EmuSuNi: %d KiB of JIT memory at %p", (int)(size / 1024), code_area);\n'
      '\t\treturn true;\n'
      '\t}\n'
      '\tERROR_LOG(VMEM, "EmuSuNi: %d KiB of JIT memory at %p can\'t be made executable", (int)(size / 1024), code_area);\n'
      '\treturn false;\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'void release_jit_block(void *code_area, size_t size)\n{\n',
      'void release_jit_block(void *code_area, size_t size)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\treturn;  // EmuSuNi: the code caches are arrays of the core\'s (never unmapped)\n'
      '#endif\n')
patch('core/oslib/virtmem.h',
      '#if defined(_WIN32) || defined(__APPLE__)\n#define DECLARE_CODE_CACHE(Name, Size) static u8 *Name;\n',
      '#if defined(EMUSUNI_PORT)\n'
      '// EmuSuNi: an array in the core\'s data (the x86-64 code reaches the core\'s code and data with 32-bit\n'
      '// offsets), on 16 KiB pages of its own (the PS4\'s), made executable by prepare_jit_block.\n'
      '#define DECLARE_CODE_CACHE(Name, Size) alignas(16384) static u8 Name[((Size) + 16383) & ~(size_t)16383];\n'
      '#elif defined(_WIN32) || defined(__APPLE__)\n#define DECLARE_CODE_CACHE(Name, Size) static u8 *Name;\n')
patch('core/hw/mem/addrspace.cpp',
      'static void *malloc_pages(size_t size)\n{\n\treturn allocAligned(PAGE_SIZE, size);\n}\n\n'
      'static void free_pages(void *p)\n{\n\tfreeAligned(p);\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (oslib/emusuni_mem.cpp): blocks of their own, off the heap.\n'
      'static void *malloc_pages(size_t size)\n{\n\treturn ::emusuni_pages_alloc(size);\n}\n\n'
      'static void free_pages(void *p)\n{\n\t::emusuni_pages_free(p);\n}\n'
      '#else\n'
      'static void *malloc_pages(size_t size)\n{\n\treturn allocAligned(PAGE_SIZE, size);\n}\n\n'
      'static void free_pages(void *p)\n{\n\tfreeAligned(p);\n}\n'
      '#endif\n')
patch('core/hw/mem/addrspace.cpp',
      '\telse\n\t{\n\t\tunprotectVram(0, VRAM_SIZE);\n\t\ttermMappings();\n\t}\n',
      '\telse\n\t{\n\t\tunprotectVram(0, VRAM_SIZE);\n\t\ttermMappings();\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: no VRAM any more: the next game\'s setPlatform must not unprotect it (upstream\'s\n'
      '\t\t// fallback never started again after release; its virtual memory is reserved again first).\n'
      '\t\tsettings.platform.vram_size = 0;\n'
      '#endif\n'
      '\t}\n')
patch('core/hw/mem/addrspace.cpp',
      '\nnamespace addrspace\n{\n\n#define HANDLER_MAX 0x1F\n',
      '\n#ifdef EMUSUNI_PORT\n'
      'void *emusuni_pages_alloc(size_t size);  // oslib/emusuni_mem.cpp\n'
      'void emusuni_pages_free(void *p);\n'
      '#endif\n\n'
      'namespace addrspace\n{\n\n#define HANDLER_MAX 0x1F\n')
patch('core/stdclass.cpp',
      'void RamRegion::alloc(size_t size)\n{\n\tthis->size = size;\n\tdata = (u8 *)allocAligned(PAGE_SIZE, size);\n',
      '#ifdef EMUSUNI_PORT\n'
      'void *emusuni_pages_alloc(size_t size);  // oslib/emusuni_mem.cpp: off the heap\n'
      'void emusuni_pages_free(void *p);\n'
      '#endif\n\n'
      'void RamRegion::alloc(size_t size)\n{\n\tthis->size = size;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tdata = (u8 *)emusuni_pages_alloc(size);\n'
      '#else\n'
      '\tdata = (u8 *)allocAligned(PAGE_SIZE, size);\n'
      '#endif\n')
patch('core/stdclass.cpp',
      '\tif (ownsMemory)\n\t\tfreeAligned(data);\n',
      '\tif (ownsMemory)\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuni_pages_free(data);\n'
      '#else\n'
      '\t\tfreeAligned(data);\n'
      '#endif\n')
# The 128 MiB jump table: filled again whole at every reset of the code cache without the virtual
# memory (~25 ms of writes on the PS4); only the entries of the blocks there are, while the "not
# compiled" handler it's filled with stays the same.
patch('core/hw/sh4/dyna/blockmanager.cpp',
      'void bm_ResetCache()\n{\n\tsh4Dynarec->reset();\n\taddrspace::bm_reset();\n',
      'void bm_ResetCache()\n{\n\tsh4Dynarec->reset();\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!addrspace::virtmemEnabled() && emusuniFpcbFilledWith == (void *)ngen_FailedToFindBlock)\n'
      '\t{\n'
      '\t\t// EmuSuNi: only the entries of the blocks there are (every other one is still the handler).\n'
      '\t\tfor (const auto& it : blkmap)\n'
      '\t\t\tFPCA(it.second->addr) = ngen_FailedToFindBlock;\n'
      '\t}\n'
      '\telse\n'
      '#endif\n'
      '\taddrspace::bm_reset();\n')
patch('core/hw/sh4/dyna/blockmanager.cpp',
      'void bm_vmem_pagefill(void** ptr, u32 size_bytes)\n{\n',
      'void bm_vmem_pagefill(void** ptr, u32 size_bytes)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (size_bytes == sizeof(p_sh4rcb->fpcb))\n'
      '\t\temusuniFpcbFilledWith = (void *)ngen_FailedToFindBlock;  // the whole table\n'
      '#endif\n')
patch('core/hw/sh4/dyna/blockmanager.cpp',
      '#define FPCA(x) ((DynarecCodeEntryPtr&)p_sh4rcb->fpcb[(x>>1)&FPCB_MASK])\n',
      '#define FPCA(x) ((DynarecCodeEntryPtr&)p_sh4rcb->fpcb[(x>>1)&FPCB_MASK])\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: what the whole jump table was last filled with (bm_vmem_pagefill), for bm_ResetCache.\n'
      'static void *emusuniFpcbFilledWith = nullptr;\n'
      '// oslib/emusuni_mem.cpp: whether pages can be write-protected (else every block checks its code).\n'
      'extern bool emusuniWriteProtectOk;\n'
      '#endif\n')
patch('core/hw/sh4/dyna/blockmanager.cpp',
      'void RuntimeBlockInfo::SetProtectedFlags()\n{\n',
      'void RuntimeBlockInfo::SetProtectedFlags()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!emusuniWriteProtectOk)\n'
      '\t{\n'
      '\t\t// EmuSuNi: no page protection: the block checks its code each time it runs.\n'
      '\t\tthis->read_only = false;\n'
      '\t\tunprotected_blocks++;\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')

# --- The JITs' code memory (virtmem.h, posix_vmem.cpp): if it can't be made executable, the
# interpreters run: the SH4's (always built in), the ARM7's and the AICA DSP's (built in too here).
patch('core/hw/sh4/dyna/driver.cpp',
      'static Sh4CodeBuffer codeBuffer;\n',
      'static Sh4CodeBuffer codeBuffer;\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: whether the code cache could be made executable (else Emulator uses the interpreter).\n'
      'bool emusuniSh4JitOk = false;\n'
      '#endif\n')
patch('core/hw/sh4/dyna/driver.cpp',
      '\tverify(rc);\n\t// Ensure the pointer returned is non-null\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!rc || CodeCache == nullptr)\n'
      '\t{\n'
      '\t\tERROR_LOG(DYNAREC, "EmuSuNi: no executable memory for the SH4 recompiler: the SH4 interpreter runs");\n'
      '\t\tCodeCache = nullptr;\n'
      '\t\temusuniSh4JitOk = false;\n'
      '\t\treturn;\n'
      '\t}\n'
      '\temusuniSh4JitOk = true;\n'
      '#endif\n'
      '\tverify(rc);\n\t// Ensure the pointer returned is non-null\n')
patch('core/emulator.cpp',
      'static void loadSpecialSettings()\n',
      '#if defined(EMUSUNI_PORT) && FEAT_SHREC != DYNAREC_NONE\n'
      '// EmuSuNi (hw/sh4/dyna/driver.cpp): the recompiler only when its code memory is executable.\n'
      'extern bool emusuniSh4JitOk;\n'
      '#define EMUSUNI_SH4_JIT && emusuniSh4JitOk\n'
      '#else\n'
      '#define EMUSUNI_SH4_JIT\n'
      '#endif\n\n'
      'static void loadSpecialSettings()\n')
patch('core/emulator.cpp',
      '\trecompiler->Init();\n\tif(config::DynarecEnabled)\n',
      '\trecompiler->Init();\n\tif(config::DynarecEnabled EMUSUNI_SH4_JIT)\n')
patch('core/emulator.cpp',
      'Sh4Executor *Emulator::getSh4Executor()\n{\n#if FEAT_SHREC != DYNAREC_NONE\n\tif(config::DynarecEnabled)\n',
      'Sh4Executor *Emulator::getSh4Executor()\n{\n#if FEAT_SHREC != DYNAREC_NONE\n\tif(config::DynarecEnabled EMUSUNI_SH4_JIT)\n')
# The SH4 context moves: without the virtual memory every game's setPlatform allocates the memory
# again (addrspace.cpp's initMappings), the SH4 context with it, not always where it was (another
# core may have mapped memory there in between). Upstream's executors keep the address they had when
# the emulator was set up (the interpreter, its cycle counter and caches, the recompiler for the code
# it writes): they follow the context before the game's reset, which drops the code made for the old.
patch('core/hw/sh4/sh4_interpreter.h',
      '\tSh4Context *getContext() { return ctx; }\n',
      '\tSh4Context *getContext() { return ctx; }\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the context was allocated again (Emulator::loadGame).\n'
      '\tvirtual void emusuniContextMoved();\n'
      '#endif\n')
patch('core/hw/sh4/interpr/sh4_interpreter.cpp',
      'void Sh4Interpreter::Term()\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'void Sh4Interpreter::emusuniContextMoved()\n'
      '{\n'
      '\tctx = &p_sh4rcb->cntx;\n'
      '\tsh4cycles.init(ctx);\n'
      '\ticache.init(ctx);\n'
      '\tocache.init(ctx);\n'
      '}\n'
      '#endif\n\n'
      'void Sh4Interpreter::Term()\n{\n')
patch('core/hw/sh4/dyna/ngen.h',
      '\tvoid clear_temp_cache(bool full);\n',
      '\tvoid clear_temp_cache(bool full);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tvoid emusuniContextMoved() override;\n'
      '#endif\n')
patch('core/hw/sh4/dyna/driver.cpp',
      'void Sh4Recompiler::Term()\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'void Sh4Recompiler::emusuniContextMoved()\n'
      '{\n'
      '\tsuper::emusuniContextMoved();\n'
      '\t// The code has the context\'s address in it: what is compiled from now on gets the new one (the\n'
      '\t// reset of the code cache that follows writes the main loop again and drops every block).\n'
      '\tif (CodeCache != nullptr)\n'
      '\t\tsh4Dynarec->init(*getContext(), codeBuffer);\n'
      '}\n'
      '#endif\n\n'
      'void Sh4Recompiler::Term()\n{\n')
patch('core/emulator.cpp',
      '\t\tsetPlatform(getGamePlatform(settings.content.fileName));\n',
      '\t\tsetPlatform(getGamePlatform(settings.content.fileName));\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: the SH4 context allocated again with the memory: the executors follow it (dc_reset\n'
      '\t\t// below drops the code compiled for the old one).\n'
      '\t\tif (interpreter != nullptr)\n'
      '\t\t\tstatic_cast<Sh4Interpreter *>(interpreter)->emusuniContextMoved();\n'
      '\t\tif (recompiler != nullptr)\n'
      '\t\t\tstatic_cast<Sh4Interpreter *>(recompiler)->emusuniContextMoved();\n'
      '#endif\n')
# ARM7 (the sound CPU): its interpreter beside the recompiler, used when the recompiler's memory isn't
# executable (the choice is made once, when its code memory is tried).
patch('core/hw/arm7/arm7.cpp',
      '#if FEAT_AREC == DYNAREC_NONE\n\nstatic void runInterpreter(u32 CycleCount)\n',
      '#if FEAT_AREC == DYNAREC_NONE || defined(EMUSUNI_PORT)\n'
      '#if FEAT_AREC != DYNAREC_NONE\n'
      '// EmuSuNi: the interpreter too, for when the recompiler\'s code memory can\'t be made executable\n'
      '// (arm7_rec.cpp).\n'
      '#define avoidRaceCondition interpreterAvoidRaceCondition\n'
      '#define run interpreterRun\n'
      '#endif\n\n'
      'static void runInterpreter(u32 CycleCount)\n')
patch('core/hw/arm7/arm7.cpp',
      '\t\trunInterpreter(ARM_CYCLES_PER_SAMPLE);\n\t\ttimeStep();\n\t}\n}\n#endif\n',
      '\t\trunInterpreter(ARM_CYCLES_PER_SAMPLE);\n\t\ttimeStep();\n\t}\n}\n'
      '#undef avoidRaceCondition\n#undef run\n#endif\n')
patch('core/hw/arm7/arm7_rec.cpp',
      '// Run a timeslice of arm7\n\nvoid run(u32 samples)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (arm7.cpp): the interpreter, when the code memory isn\'t executable.\n'
      'void interpreterRun(u32 samples);\n'
      'void interpreterAvoidRaceCondition();\n'
      '#endif\n\n'
      '// Run a timeslice of arm7\n\nvoid run(u32 samples)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (recompiler::ICache == nullptr)\n'
      '\t{\n'
      '\t\tinterpreterRun(samples);\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('core/hw/arm7/arm7_rec.cpp',
      'void avoidRaceCondition()\n{\n',
      'void avoidRaceCondition()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (recompiler::ICache == nullptr)\n'
      '\t{\n'
      '\t\tinterpreterAvoidRaceCondition();\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('core/hw/arm7/arm7_rec.cpp',
      'void flush()\n{\n\ticPtr = ICache;\n',
      'void flush()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (ICache == nullptr)\n'
      '\t\treturn;  // EmuSuNi: no code memory, the interpreter runs\n'
      '#endif\n'
      '\ticPtr = ICache;\n')
patch('core/hw/arm7/arm7_rec.cpp',
      '\tverify(rc);\n\n\tflush();\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!rc)\n'
      '\t{\n'
      '\t\tERROR_LOG(AICA_ARM, "EmuSuNi: no executable memory for the ARM7 recompiler: the ARM7 interpreter runs");\n'
      '\t\tICache = nullptr;\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n'
      '\tverify(rc);\n\n\tflush();\n}\n')
patch('core/hw/aica/dsp_interp.cpp',
      '#if FEAT_DSPREC != DYNAREC_JIT\n',
      '#if FEAT_DSPREC != DYNAREC_JIT || defined(EMUSUNI_PORT)\n'
      '#if FEAT_DSPREC == DYNAREC_JIT\n'
      '// EmuSuNi: the interpreter too, for when the recompiler\'s code memory can\'t be made executable\n'
      '// (dsp_x64.cpp).\n'
      '#define runStep runStepInterpreter\n'
      '#endif\n')
patch('core/hw/aica/dsp_x64.cpp',
      'void recompile()\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'void runStepInterpreter();  // EmuSuNi (dsp_interp.cpp)\n'
      '#endif\n\n'
      'void recompile()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (pCodeBuffer == nullptr)\n'
      '\t\treturn;  // EmuSuNi: no code memory, the interpreter runs\n'
      '#endif\n')
patch('core/hw/aica/dsp_x64.cpp',
      '\t\tdie("virtmem::prepare_jit_block failed in x64 dsp");\n',
      '\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tERROR_LOG(AICA_ARM, "EmuSuNi: no executable memory for the DSP recompiler: the DSP interpreter runs");\n'
      '\t\tpCodeBuffer = nullptr;\n'
      '#else\n'
      '\t\tdie("virtmem::prepare_jit_block failed in x64 dsp");\n'
      '#endif\n'
      '\t}\n')
patch('core/hw/aica/dsp_x64.cpp',
      'void runStep()\n{\n',
      'void runStep()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (pCodeBuffer == nullptr)\n'
      '\t{\n'
      '\t\trunStepInterpreter();\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')

# --- The fault handler: write-protected pages only (no virtual memory: nothing else is the core's).
# On the PS4 with the FreeBSD kernel's values written out (as port/ps4_crashlog.cpp has them: SIGSEGV
# 11, SIGBUS 10, SA_SIGINFO 0x40, si_addr 24 bytes into the siginfo), whatever a header says; the
# struct sigaction handed to the kernel checked to be FreeBSD's when this compiles. A fault that isn't
# a write to a protected page of the core's goes on to the handler before (the app's crash log), as
# the kernel would. Checked once when it's installed: a page protected and written to must fault and
# come back written (else nothing is write-protected: the blocks check their code instead).
patch('core/linux/common.cpp',
      'void fault_handler(int sn, siginfo_t * si, void *segfault_ctx)\n{\n',
      r'''#ifdef EMUSUNI_PORT
// EmuSuNi (scripts/build-dc-core.sh): see the top of the script.
#include <atomic>
#include <csetjmp>
#include <cstddef>
#include <sys/mman.h>

#if defined(__ORBIS__)
static constexpr int kEmuSigBus = 10, kEmuSigSegv = 11, kEmuSaSiginfo = 0x40;
static constexpr size_t kEmuSiAddr = 24;
static_assert(offsetof(struct sigaction, sa_flags) == 8 && sizeof(sigset_t) == 16,
	"struct sigaction isn't FreeBSD's: the PSChrome libc overlay's headers must come first");
#else
static constexpr int kEmuSigBus = SIGBUS, kEmuSigSegv = SIGSEGV, kEmuSaSiginfo = SA_SIGINFO;
static constexpr size_t kEmuSiAddr = offsetof(siginfo_t, si_addr);
#endif

extern bool emusuniWriteProtectOk, emusuniWriteProtectTested;  // oslib/emusuni_mem.cpp
void *emusuni_pages_alloc(size_t size);
void emusuni_pages_free(void *p);

static struct sigaction emusuniOldSegv, emusuniOldBus;
static bool emusuniInstalled = false;
static u8 *volatile emusuniProbePage = nullptr;
static std::atomic<int> emusuniProbeFaults { 0 };
static sigjmp_buf emusuniProbeJump;

// The self-test's write (emusuniProbeWriteProtection).
static bool emusuniProbeFault(void *address)
{
	u8 *page = emusuniProbePage;
	if (page == nullptr || (u8 *)address < page || (u8 *)address >= page + PAGE_SIZE)
		return false;
	if (++emusuniProbeFaults > 1 || mprotect(page, PAGE_SIZE, PROT_READ | PROT_WRITE) != 0)
		siglongjmp(emusuniProbeJump, 1);  // it would fault again and again
	return true;
}

static void fault_handler(int sn, siginfo_t *si, void *segfault_ctx)
{
	void *address = *(void **)((char *)si + kEmuSiAddr);
	if (emusuniProbeFault(address))
		return;
	// Ram watcher for net rollback
	if (memwatch::writeAccess(address))
		return;
	// code protection in RAM
	if (bm_RamWriteAccess(address))
		return;
	// texture protection in VRAM
	if (VramLockedWrite((u8 *)address))
		return;
#if !defined(__ORBIS__)
	// (The PC tests' comparison with upstream's virtual memory, EMUSUNI_DC_VMEM=1: its jump table's
	// pages made on demand, its fast memory accesses rewritten. Nothing of it without that memory.)
	if (addrspace::bm_lockedWrite((u8 *)address))
		return;
	host_context_t ctx;
	context_from_segfault(&ctx, segfault_ctx);
	if (sh4Dynarec->rewrite(ctx, address))
	{
		context_to_segfault(&ctx, segfault_ctx);
		return;
	}
	ERROR_LOG(COMMON, "SIGSEGV @ %p invalid access to %p", (void *)ctx.pc, address);
#endif
	// Not the core's: the handler before (the app's crash log), as the kernel would have called it.
	struct sigaction *old = sn == kEmuSigSegv ? &emusuniOldSegv : &emusuniOldBus;
	if (old->sa_flags & kEmuSaSiginfo)
	{
		old->sa_sigaction(sn, si, segfault_ctx);
		return;
	}
	if (old->sa_handler == SIG_IGN)
		return;
	if (old->sa_handler == SIG_DFL)
	{
		// The default action: back to it, and the faulting instruction runs again.
		struct sigaction dfl {};
		sigaction(sn, &dfl, nullptr);
		return;
	}
	old->sa_handler(sn);
}

// Does a write to a write-protected page fault, reach the handler and come back written?
static void emusuniProbeWriteProtection()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	u8 *page = (u8 *)emusuni_pages_alloc(PAGE_SIZE);
	bool ok = false;
	const char *why = "no memory";
	if (page != nullptr)
	{
		if (mprotect(page, PAGE_SIZE, PROT_READ) != 0)
			why = "mprotect refused";
		else
		{
			emusuniProbeFaults = 0;
			emusuniProbePage = page;
			if (sigsetjmp(emusuniProbeJump, 1) == 0)
			{
				((volatile u8 *)page)[100] = 0x5a;
				ok = ((volatile u8 *)page)[100] == 0x5a && emusuniProbeFaults == 1;
				why = emusuniProbeFaults == 0 ? "the write didn't fault" : "?";
			}
			else
				why = "the page couldn't be made writable again in the handler";
			emusuniProbePage = nullptr;
			mprotect(page, PAGE_SIZE, PROT_READ | PROT_WRITE);
		}
		emusuni_pages_free(page);
	}
	emusuniWriteProtectOk = ok;
	emusuniWriteProtectTested = true;
	if (ok)
		NOTICE_LOG(VMEM, "EmuSuNi: write-protected pages fault and come back: texture and code protection on");
	else
		ERROR_LOG(VMEM, "EmuSuNi: write-protected pages don't work (%s): no texture invalidation, the recompiler checks its blocks", why);
}

void os_InstallFaultHandler()
{
	if (emusuniInstalled)
		return;
	struct sigaction act {};  // (an empty mask)
	act.sa_sigaction = fault_handler;
	act.sa_flags = kEmuSaSiginfo;
	sigaction(kEmuSigSegv, &act, &emusuniOldSegv);
	sigaction(kEmuSigBus, &act, &emusuniOldBus);
	emusuniInstalled = true;
	emusuniProbeWriteProtection();
}

void os_UninstallFaultHandler()
{
	if (!emusuniInstalled)
		return;
	// The handlers before (the app's crash log) back.
	sigaction(kEmuSigSegv, &emusuniOldSegv, nullptr);
	sigaction(kEmuSigBus, &emusuniOldBus, nullptr);
	emusuniInstalled = false;
}
#else  // !EMUSUNI_PORT

void fault_handler(int sn, siginfo_t * si, void *segfault_ctx)
{
''')
patch('core/linux/common.cpp',
      'void os_UninstallFaultHandler()\n{\n#ifndef __SWITCH__\n\tsigaction(SIGSEGV, &next_segv_handler, nullptr);\n'
      '#endif\n#if defined(__APPLE__)\n\tsigaction(SIGBUS, &next_bus_handler, nullptr);\n#endif\n}\n',
      'void os_UninstallFaultHandler()\n{\n#ifndef __SWITCH__\n\tsigaction(SIGSEGV, &next_segv_handler, nullptr);\n'
      '#endif\n#if defined(__APPLE__)\n\tsigaction(SIGBUS, &next_bus_handler, nullptr);\n#endif\n}\n'
      '#endif  // !EMUSUNI_PORT\n')

# --- Threads: the app names and places them.
patch('core/linux/common.cpp',
      'void os_SetThreadName(const char *name)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (the app, port/): called first thing in every thread Flycast makes (each names itself with\n'
      '// a ThreadName: "Flycast-emu" for the emulation with threaded rendering...): its core, priority and\n'
      '// name. "flycast" is what a ThreadName gives back when its thread ends: nothing then.\n'
      'extern "C" void emusuni_dc_thread(const char *name);\n'
      '#endif\n\n'
      'void os_SetThreadName(const char *name)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (strcmp(name, "flycast") != 0)\n'
      '\t\temusuni_dc_thread(name);\n'
      '\treturn;\n'
      '#endif\n')

# --- libretro: the emulator set up once per process (retro_deinit keeps it, frees the Dreamcast's
# memory), as upstream does on Linux, here and on the PS4 alike (upstream's other path can't start
# again after its term()). The frontend's functions only from the thread of retro_run and
# retro_load_game: rumble, messages and timing changes of the emulation thread (threaded rendering)
# wait for the next retro_run.
patch('shell/libretro/libretro.cpp',
      '#if defined(__APPLE__) || (defined(__GNUC__) && defined(__linux__) && !defined(__ANDROID__))\n\tif (!emuInited)\n',
      '#if defined(__APPLE__) || (defined(__GNUC__) && defined(__linux__) && !defined(__ANDROID__)) || defined(EMUSUNI_PORT)\n\tif (!emuInited)\n')
patch('shell/libretro/libretro.cpp',
      '#if defined(__APPLE__) || (defined(__GNUC__) && defined(__linux__) && !defined(__ANDROID__))\n\taddrspace::release();\n',
      '#if defined(__APPLE__) || (defined(__GNUC__) && defined(__linux__) && !defined(__ANDROID__)) || defined(EMUSUNI_PORT)\n\taddrspace::release();\n')
patch('shell/libretro/libretro.cpp',
      'static void refresh_devices(bool first_startup);\n',
      'static void refresh_devices(bool first_startup);\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: what the emulation thread asks of the frontend, for the next retro_run.\n'
      'static std::thread::id emusuniFrontendThread;\n'
      'static std::mutex emusuniDeferredLock;\n'
      'static std::vector<std::pair<std::string, int>> emusuniMessages;\n'
      'static bool emusuniFpsPending = false;\n'
      'static double emusuniFps = 0;\n'
      'struct EmusuniVibration { bool pending; float power, inclination; u32 durationMs; };\n'
      'static EmusuniVibration emusuniVibration[4];\n'
      'static bool emusuniOnFrontendThread() { return std::this_thread::get_id() == emusuniFrontendThread; }\n'
      '#endif\n')
patch('shell/libretro/libretro.cpp',
      'void retro_init()\n{\n',
      'void retro_init()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniFrontendThread = std::this_thread::get_id();\n'
      '#endif\n')
patch('shell/libretro/libretro.cpp',
      'bool retro_refresh_av_info(double fps)\n{\n',
      'bool retro_refresh_av_info(double fps)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!emusuniOnFrontendThread())\n'
      '\t{\n'
      '\t\tstd::lock_guard<std::mutex> lock(emusuniDeferredLock);\n'
      '\t\temusuniFpsPending = true;\n'
      '\t\temusuniFps = fps;\n'
      '\t\treturn false;\n'
      '\t}\n'
      '#endif\n')
patch('shell/libretro/libretro.cpp',
      'void retro_run()\n{\n\tbool updated = false;\n',
      '#ifdef EMUSUNI_PORT\n'
      'static void emusuniFlushDeferred()\n'
      '{\n'
      '\tstd::vector<std::pair<std::string, int>> messages;\n'
      '\tEmusuniVibration vibration[4];\n'
      '\tbool fpsPending;\n'
      '\tdouble fps;\n'
      '\t{\n'
      '\t\tstd::lock_guard<std::mutex> lock(emusuniDeferredLock);\n'
      '\t\tmessages.swap(emusuniMessages);\n'
      '\t\tmemcpy(vibration, emusuniVibration, sizeof(vibration));\n'
      '\t\tmemset(emusuniVibration, 0, sizeof(emusuniVibration));\n'
      '\t\tfpsPending = emusuniFpsPending;\n'
      '\t\tfps = emusuniFps;\n'
      '\t\temusuniFpsPending = false;\n'
      '\t}\n'
      '\tfor (const auto& message : messages)\n'
      '\t\tos_notify(message.first.c_str(), message.second, nullptr);\n'
      '\tfor (u32 port = 0; port < 4; port++)\n'
      '\t\tif (vibration[port].pending)\n'
      '\t\t\tupdateVibration(port, vibration[port].power, vibration[port].inclination, vibration[port].durationMs);\n'
      '\tif (fpsPending)\n'
      '\t\tretro_refresh_av_info(fps);\n'
      '}\n'
      '#endif\n\n'
      'void retro_run()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tconst int64_t emusuniRunStart = emusuni::nowNs();\n'
      '\temusuniFrontendThread = std::this_thread::get_id();\n'
      '\temusuniFlushDeferred();\n'
      '#endif\n'
      '\tbool updated = false;\n')
patch('shell/libretro/libretro.cpp',
      'bool retro_load_game(const struct retro_game_info *game)\n{\n',
      'bool retro_load_game(const struct retro_game_info *game)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniFrontendThread = std::this_thread::get_id();\n'
      '\t// EmuSuNi: the core stays in the app from game to game: its disc changer starts afresh. read_m3u\n'
      '\t// counts the discs up from disk_index: a game left on disc 2 made the next .m3u read nothing and\n'
      '\t// load the .m3u itself ("Unknown disk format") until the app started again. (Not the initial\n'
      '\t// image: the frontend gives it between retro_init and here.)\n'
      '\tdisk_index = 0;\n'
      '\tdisk_paths.clear();\n'
      '\tdisk_labels.clear();\n'
      '\tdisc_tray_open = false;\n'
      '\t// What the last game forced (Flycast\'s per-game settings: its region, the real BIOS, framebuffer\n'
      '\t// emulation...) goes before this game\'s options are read (RetroArch loads the core afresh).\n'
      '\tconfig::Settings::instance().resetOverrides();\n'
      '\t{\n'
      '\t\t// What the last game\'s emulation thread asked after its last retro_run (its thread has ended):\n'
      '\t\t// a "Reboot to BIOS" left there restarted the next game at its first frame.\n'
      '\t\tstd::lock_guard<std::mutex> lock(emusuniDeferredLock);\n'
      '\t\temusuniMessages.clear();\n'
      '\t\tmemset(emusuniVibration, 0, sizeof(emusuniVibration));\n'
      '\t\temusuniFpsPending = false;\n'
      '\t}\n'
      '#endif\n')
# No state of a machine whose emulation ended in an error (Flycast's HLE BIOS throws "Reboot to BIOS"
# with the lid open; the app starts the console again once the lid closes): its SH4 is stopped inside the
# BIOS call, and start() can't run a state loaded into it. A state that can't be loaded leaves the
# emulator running as it was (upstream left it stopped: the game froze).
for name, fail in (('retro_serialize_size");', '0'), ('retro_serialize %d bytes", (int)size);', 'false'),
                   ('retro_unserialize");', 'false')):
    patch('shell/libretro/libretro.cpp',
          '\tDEBUG_LOG(SAVESTATE, "' + name + '\n\tstd::lock_guard<std::mutex> lock(mtx_serialization);\n',
          '\tDEBUG_LOG(SAVESTATE, "' + name + '\n\tstd::lock_guard<std::mutex> lock(mtx_serialization);\n'
          '#ifdef EMUSUNI_PORT\n'
          '\tif (!first_run && !emu.running())\n'
          '\t\treturn ' + fail + ';  // EmuSuNi: the emulation ended in an error\n'
          '#endif\n')
# A state that fails half way (a file cut short: only its header is checked first) leaves the machine as
# it was - what was already read of it would run mixed with what wasn't -, and the game goes on (upstream
# left the emulator stopped: the game froze). The messages the replaced machine queued go with it.
patch('shell/libretro/libretro.cpp',
      '\ttry {\n'
      '\t\tDeserializer deser(data, size);\n'
      '\t\temu.loadstate(deser);\n'
      '\t    retro_audio_flush_buffer();\n'
      '\t\tif (!first_run)\n'
      '\t\t\temu.start();\n'
      '\n'
      '\t\treturn true;\n'
      '\t} catch (const Deserializer::Exception& e) {\n'
      '\t\tERROR_LOG(SAVESTATE, "Loading state failed: %s", e.what());\n'
      '\t\treturn false;\n'
      '\t}\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tstd::vector<u8> undo;  // EmuSuNi: the machine as it was\n'
      '\ttry {\n'
      '\t\tSerializer dry;\n'
      '\t\tdc_serialize(dry);\n'
      '\t\tundo.resize(dry.size());\n'
      '\t\tSerializer ser(undo.data(), undo.size());\n'
      '\t\tdc_serialize(ser);\n'
      '\t} catch (...) {\n'
      '\t\tstd::vector<u8>().swap(undo);  // (no memory for it: loaded without)\n'
      '\t}\n'
      '#endif\n'
      '\ttry {\n'
      '\t\tDeserializer deser(data, size);\n'
      '\t\temu.loadstate(deser);\n'
      '\t    retro_audio_flush_buffer();\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t{\n'
      '\t\t\tstd::lock_guard<std::mutex> lock(emusuniDeferredLock);\n'
      '\t\t\temusuniMessages.clear();  // the replaced machine\'s\n'
      '\t\t}\n'
      '#endif\n'
      '\t\tif (!first_run)\n'
      '\t\t\temu.start();\n'
      '\n'
      '\t\treturn true;\n'
      '\t} catch (const Deserializer::Exception& e) {\n'
      '\t\tERROR_LOG(SAVESTATE, "Loading state failed: %s", e.what());\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (!undo.empty())\n'
      '\t\t\ttry {\n'
      '\t\t\t\tDeserializer back(undo.data(), undo.size());\n'
      '\t\t\t\temu.loadstate(back);\n'
      '\t\t\t} catch (const Deserializer::Exception& e2) {\n'
      '\t\t\t\tERROR_LOG(SAVESTATE, "EmuSuNi: the machine before couldn\'t be put back: %s", e2.what());\n'
      '\t\t\t}\n'
      '\t\tif (!first_run)\n'
      '\t\t\temu.start();  // the game goes on as it was\n'
      '#endif\n'
      '\t\treturn false;\n'
      '\t}\n')
patch('shell/libretro/libretro.cpp',
      'void retro_reset()\n{\n\tstd::lock_guard<std::mutex> lock(mtx_serialization);\n\n\temu.unloadGame();\n',
      'void retro_reset()\n{\n\tstd::lock_guard<std::mutex> lock(mtx_serialization);\n\n\temu.unloadGame();\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t{\n'
      '\t\tstd::lock_guard<std::mutex> lock(emusuniDeferredLock);\n'
      '\t\temusuniMessages.clear();  // EmuSuNi: the stopped machine\'s ("Reboot to BIOS": no restart twice)\n'
      '\t}\n'
      '#endif\n')
patch('shell/libretro/libretro.cpp',
      'static void updateVibration(u32 port, float power, float inclination, u32 durationMs)\n{\n'
      '\tif (!rumble.set_rumble_state)\n',
      'static void updateVibration(u32 port, float power, float inclination, u32 durationMs)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!emusuniOnFrontendThread())\n'
      '\t{\n'
      '\t\tstd::lock_guard<std::mutex> lock(emusuniDeferredLock);\n'
      '\t\tif (port < 4)\n'
      '\t\t\temusuniVibration[port] = { true, power, inclination, durationMs };\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n'
      '\tif (!rumble.set_rumble_state)\n')
patch('shell/libretro/libretro.cpp',
      'void os_notify(const char *msg, int durationMs, const char *details)\n{\n',
      'void os_notify(const char *msg, int durationMs, const char *details)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!emusuniOnFrontendThread())\n'
      '\t{\n'
      '\t\tstd::lock_guard<std::mutex> lock(emusuniDeferredLock);\n'
      '\t\temusuniMessages.emplace_back(msg != nullptr ? msg : "", durationMs);\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('shell/libretro/libretro.cpp',
      '#include <mutex>\n',
      '#include <mutex>\n#ifdef EMUSUNI_PORT\n#include <thread>\n#include <vector>\n#include "emusuni_timing.h"\n#endif\n')
# The pads may be set before retro_load_game (no unplugged pads at the first frames then): an option
# that recreates the VMUs, changed since the game before, unplugs them at once, and upstream's
# reconnection timer reads the SH4 context, which retro_deinit freed (a crash). The game's start
# creates the devices anyway.
patch('core/hw/maple/maple_if.cpp',
      'void maple_ReconnectDevices()\n{\n\tmcfg_DestroyDevices();\n',
      'void maple_ReconnectDevices()\n{\n\tmcfg_DestroyDevices();\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (p_sh4rcb == nullptr)\n'
      '\t\treturn;  // EmuSuNi: no Dreamcast memory (between games): the next game makes the devices\n'
      '#endif\n')
patch('core/hw/maple/maple_if.cpp',
      '#include "hw/sh4/sh4_sched.h"\n',
      '#include "hw/sh4/sh4_sched.h"\n#include "hw/sh4/sh4_if.h"\n')
# The options read before the game (the pads set before retro_load_game read them): the renderer isn't
# made again while there is no graphics context (Vulkan's comes with context_reset, after the game).
# Upstream's did when the options changed the renderer (the Transparency's per-pixel sorting is
# Vulkan_OIT, per-triangle Vulkan) and made it without a context: a null pointer, a crash at every
# game's start with per-pixel sorting (the Ultra level). context_reset makes the renderer the options
# say anyway.
patch('shell/libretro/libretro.cpp',
      '\tif (!first_startup && previous_renderer != config::RendererType) {\n'
      '\t\trend_term_renderer();\n'
      '\t\trend_init_renderer();\n'
      '\t}\n',
      '\tif (!first_startup && previous_renderer != config::RendererType\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t&& GraphicsContext::Instance() != nullptr  // EmuSuNi: none before context_reset\n'
      '#endif\n'
      '\t\t\t) {\n'
      '\t\trend_term_renderer();\n'
      '\t\trend_init_renderer();\n'
      '\t}\n')
# A reset boots the disc in the drive, as the console's: upstream's retro_reset loads the disc the game
# started with again. A game on several discs (EmuSuNi's .m3u) whose game goes back to the BIOS when
# the lid opens (Crazy Taxi does; the HLE BIOS has no menu to go back to: src/Emulator.cpp starts the
# console again then) starts with the disc put in.
# The options a game forced (Option::override: its region, the real BIOS, framebuffer emulation...) are
# undone when the next game loads (retro_load_game's patch above): libretro's Settings::reset() does
# nothing, as RetroArch loads the core afresh for each game, and in the app the core stays - after a
# game that forced the HLE BIOS the next ones booted it too, even with the real BIOS there; after one
# that forced the real BIOS they said "This game requires a real BIOS"; Crazy Taxi after Marvel vs.
# Capcom 2 started with Europe's region.
patch('core/cfg/option.h',
      '\tvirtual void reset() = 0;\n};\n',
      '\tvirtual void reset() = 0;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tvirtual void resetOverride() {}\n'
      '#endif\n'
      '};\n')
patch('shell/libretro/option_lr.h',
      'class Settings {\npublic:\n\tvoid reset() {}\n',
      'class Settings {\npublic:\n\tvoid reset() {}\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tvoid resetOverrides() {\n'
      '\t\tfor (const auto& o : options)\n'
      '\t\t\to->resetOverride();\n'
      '\t}\n'
      '#endif\n')
patch('shell/libretro/option_lr.h',
      '\tvoid reset() override {\n\t\tset(defaultValue);\n\t\toverridden = false;\n\t}\n',
      '\tvoid reset() override {\n\t\tset(defaultValue);\n\t\toverridden = false;\n\t}\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tvoid resetOverride() override {\n'
      '\t\tif (overridden)\n'
      '\t\t\treset();\n'
      '\t}\n'
      '#endif\n')
# The disc changed with Flycast-emu stopped, as retro_serialize does (Flycast's own menu stops the
# emulator before Eject / Insert too): the emulation thread runs between retro_run calls and reads the
# drive (the disc an eject deletes), the SH4 scheduler and reios's state that a disc change writes.
patch('shell/libretro/libretro.cpp',
      'static bool retro_set_eject_state(bool ejected)\n{\n\tdisc_tray_open = ejected;\n',
      '#ifdef EMUSUNI_PORT\n'
      'struct EmusuniEmuPaused\n'
      '{\n'
      '\tstd::lock_guard<std::mutex> lock { mtx_serialization };\n'
      '\tconst bool wasRunning = emu.running();\n'
      '\tEmusuniEmuPaused()\n'
      '\t{\n'
      '\t\tif (wasRunning)\n'
      '\t\t\ttry {\n'
      '\t\t\t\temu.stop();\n'
      '\t\t\t} catch (const FlycastException& e) {\n'
      '\t\t\t\t// The emulation had ended in an error no retro_run said yet: said now, as retro_run does\n'
      '\t\t\t\t// ("Reboot to BIOS": the frontend starts the console again).\n'
      '\t\t\t\tERROR_LOG(COMMON, "%s", e.what());\n'
      '\t\t\t\tos_notify(e.what(), 5000);\n'
      '\t\t\t\tenviron_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);\n'
      '\t\t\t}\n'
      '\t}\n'
      '\t~EmusuniEmuPaused()\n'
      '\t{\n'
      '\t\tif (wasRunning)\n'
      '\t\t\temu.start();  // (only from Loaded)\n'
      '\t}\n'
      '};\n'
      '#endif\n\n'
      'static bool retro_set_eject_state(bool ejected)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tEmusuniEmuPaused paused;\n'
      '#endif\n'
      '\tdisc_tray_open = ejected;\n')
patch('shell/libretro/libretro.cpp',
      'static bool retro_set_image_index(unsigned index)\n{\n\tdisk_index = index;\n',
      'static bool retro_set_image_index(unsigned index)\n{\n\tdisk_index = index;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (disc_tray_open && disk_index < disk_paths.size())\n'
      '\t\treturn true;  // the disc goes in with retro_set_eject_state(false)\n'
      '\tEmusuniEmuPaused paused;\n'
      '#endif\n')
patch('shell/libretro/libretro.cpp',
      '\t\t\temu.insertGdrom(disk_paths[disk_index]);\n',
      '\t\t\temu.insertGdrom(disk_paths[disk_index]);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\tgame_data = disk_paths[disk_index];  // EmuSuNi: the disc a reset boots\n'
      '#endif\n')
patch('shell/libretro/libretro.cpp',
      '\t\temu.insertGdrom(disk_paths[index]);\n',
      '\t\temu.insertGdrom(disk_paths[index]);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tgame_data = disk_paths[index];  // EmuSuNi: the disc a reset boots\n'
      '#endif\n')

# --- Vulkan: the frontend's device adopted (EmuSuNi never calls the negotiation's create_device, where
# upstream loads Vulkan's functions and finds what the device has), the queue always under the
# frontend's lock, the images of the frames the core's own.
patch('core/rend/vulkan/vk_context_lr.cpp',
      'bool VulkanContext::init(retro_hw_render_interface_vulkan *retro_render_if)\n{\n',
      r'''#ifdef EMUSUNI_PORT
#include <cstdlib>
#include <cstring>

static bool EmuSuNiListed(const char *list, const char *name)
{
	if (list == nullptr)
		return false;
	const size_t length = strlen(name);
	for (const char *at = strstr(list, name); at != nullptr; at = strstr(at + 1, name))
	{
		if ((at == list || at[-1] == ',') && (at[length] == '\0' || at[length] == ','))
			return true;
	}
	return false;
}

// EmuSuNi: the frontend made its only VkDevice when it started (src/Display.cpp) and never calls
// the negotiation interface's create_device: Vulkan's functions come from its interface, and what the
// device was made with from EMUSUNI_VK_DEVICE_FEATURES (VkPhysicalDeviceFeatures names, and
// provokingVertexLast) and EMUSUNI_VK_DEVICE_EXTENSIONS, comma-separated; none of them when unset.
static void EmuSuNiAdoptDevice(const retro_hw_render_interface_vulkan *ri)
{
	VULKAN_HPP_DEFAULT_DISPATCHER.init(ri->get_instance_proc_addr);
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(ri->instance));
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(ri->device));
	const char *features = getenv("EMUSUNI_VK_DEVICE_FEATURES");
	const char *extensions = getenv("EMUSUNI_VK_DEVICE_EXTENSIONS");
	VulkanContext::fragmentStoresAndAtomics = EmuSuNiListed(features, "fragmentStoresAndAtomics");
	VulkanContext::samplerAnisotropy = EmuSuNiListed(features, "samplerAnisotropy");
	VulkanContext::dedicatedAllocationSupported = true;  // Vulkan 1.1 (EmuSuNi's instance)
	VulkanContext::provokingVertexSupported = EmuSuNiListed(extensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME)
		&& EmuSuNiListed(features, "provokingVertexLast");
	VulkanContext::bufferDeviceAddressSupported = false;
	NOTICE_LOG(RENDERER, "EmuSuNi: the frontend's Vulkan device, queue family %u: per-pixel sorting %s, anisotropy %s, provoking vertex %s",
		ri->queue_index, VulkanContext::fragmentStoresAndAtomics ? "yes" : "no",
		VulkanContext::samplerAnisotropy ? "yes" : "no", VulkanContext::provokingVertexSupported ? "yes" : "no");
}
#endif

bool VulkanContext::init(retro_hw_render_interface_vulkan *retro_render_if)
{
''')
patch('core/rend/vulkan/vk_context_lr.cpp',
      '\tqueue = vk::Queue(retro_render_if->queue);\n',
      '\tqueue = vk::Queue(retro_render_if->queue);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tEmuSuNiAdoptDevice(retro_render_if);\n'
      '#endif\n')
patch('core/rend/vulkan/vk_context_lr.cpp',
      '\tif (image == vk::Image())\n\t\treturn;\n\tfloat shiftX, shiftY;\n',
      '\tif (image == vk::Image())\n\t\treturn;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the next of the frame images (never the one the frontend shows now, set_image\'s).\n'
      '\temusuniImage = (emusuniImage + 1) % EMUSUNI_IMAGES;\n'
      '#endif\n'
      '\tfloat shiftX, shiftY;\n')
patch('core/rend/vulkan/vk_context_lr.cpp',
      '\tif (device)\n\t{\n\t\tdevice.waitIdle();\n',
      '\tif (device)\n\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tretro_render_if->lock_queue(retro_render_if->handle);  // EmuSuNi: the menus\' queue too\n'
      '\t\tdevice.waitIdle();\n'
      '\t\tretro_render_if->unlock_queue(retro_render_if->handle);\n'
      '#else\n'
      '\t\tdevice.waitIdle();\n'
      '#endif\n')
patch('core/rend/vulkan/vk_context_lr.h',
      '\tu32 GetSwapChainSize() const { u32 m = retro_render_if->get_sync_index_mask(retro_render_if->handle); u32 n = 1; while (m >>= 1) n++; return n; }\n'
      '\tint GetCurrentImageIndex() const { return retro_render_if->get_sync_index(retro_render_if->handle); }\n\n'
      '\tvoid WaitIdle() const { queue.waitIdle(); }\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the frame images (and their descriptor sets) in a ring of the core\'s own, one step per\n'
      '\t// frame presented (PresentFrame), not the frontend\'s sync index: the frontend may call retro_run\n'
      '\t// several times between two of its frames, or draw on another thread while it runs. The GPU is\n'
      '\t// done with a slot when it comes round again (CommandPool waits for the frame 3 frames back).\n'
      '\tu32 GetSwapChainSize() const { return EMUSUNI_IMAGES; }\n'
      '\tint GetCurrentImageIndex() const { return (int)emusuniImage; }\n\n'
      '\tvoid WaitIdle() const {\n'
      '\t\tretro_render_if->lock_queue(retro_render_if->handle);  // EmuSuNi: the menus\' queue too\n'
      '\t\tqueue.waitIdle();\n'
      '\t\tretro_render_if->unlock_queue(retro_render_if->handle);\n'
      '\t}\n'
      '#else\n'
      '\tu32 GetSwapChainSize() const { u32 m = retro_render_if->get_sync_index_mask(retro_render_if->handle); u32 n = 1; while (m >>= 1) n++; return n; }\n'
      '\tint GetCurrentImageIndex() const { return retro_render_if->get_sync_index(retro_render_if->handle); }\n\n'
      '\tvoid WaitIdle() const { queue.waitIdle(); }\n'
      '#endif\n')
patch('core/rend/vulkan/vk_context_lr.h',
      '\tretro_vulkan_image retro_image;\n};\n',
      '\tretro_vulkan_image retro_image;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tstatic constexpr u32 EMUSUNI_IMAGES = 3;\n'
      '\tu32 emusuniImage = 0;\n'
      '\tint emusuniFramesSinceSave = 0;\n'
      '\tint emusuniPipelinesSaved = 0;\n'
      '#endif\n'
      '};\n')
# The render pass that writes the frame images: it waits for the fragment shader reads of every
# earlier submission (the frontend may still sample an older image of the ring when the core writes
# it again: no barrier would order that), and makes its writes visible to the frontend's sampling.
patch('core/rend/vulkan/vk_context_lr.cpp',
      '\trenderPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),\n'
      '\t\t\tattachmentDescription,\tsubpass));\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tconst std::array<vk::SubpassDependency, 2> dependencies {\n'
      '\t\tvk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0,\n'
      '\t\t\tvk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '\t\t\tvk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlags(), vk::AccessFlagBits::eColorAttachmentWrite),\n'
      '\t\tvk::SubpassDependency(0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '\t\t\tvk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead),\n'
      '\t};\n'
      '\trenderPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),\n'
      '\t\t\tattachmentDescription, subpass, dependencies));\n'
      '#else\n'
      '\trenderPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),\n'
      '\t\t\tattachmentDescription,\tsubpass));\n'
      '#endif\n')
# Per-pixel alpha sorting needs fragmentStoresAndAtomics: without it (a device made without the
# feature) the per-triangle renderer (upstream only hides the option's value from the frontend).
patch('core/hw/pvr/Renderer_if.cpp',
      '\tcase RenderType::Vulkan_OIT:\n\t\trenderer = rend_OITVulkan();\n\t\tbreak;\n',
      '\tcase RenderType::Vulkan_OIT:\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (GraphicsContext::Instance() == nullptr || !GraphicsContext::Instance()->hasPerPixel())\n'
      '\t\t{\n'
      '\t\t\tNOTICE_LOG(RENDERER, "EmuSuNi: per-pixel sorting needs fragmentStoresAndAtomics: per-triangle sorting");\n'
      '\t\t\trenderer = rend_Vulkan();\n'
      '\t\t\tbreak;\n'
      '\t\t}\n'
      '#endif\n'
      '\t\trenderer = rend_OITVulkan();\n\t\tbreak;\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '#include "profiler/fc_profiler.h"\n',
      '#include "profiler/fc_profiler.h"\n#include "wsi/context.h"\n')

# --- Where the frames' time goes: a line every 10 s in the log (retro_run): the emulation (SH4,
# AICA, ARM7) and its waits for the renderer, the renderer (TA lists to Vulkan) and its waits for the
# emulation, the pipelines and shaders made (the PS4's driver compiles slowly), the GPU waits.
addfile('core/emusuni_timing.h', r'''// EmuSuNi (scripts/build-dc-core.sh): where the frames' time goes, logged every 10 s by retro_run.
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>

namespace emusuni
{
struct Timing
{
	std::atomic<int64_t> emuRunNs { 0 };      // the emulation thread's CPU time in Emulator::runInternal, counted at each vblank
	std::atomic<int64_t> emuMark { 0 };       // (when it was last counted; 0: not running)
	std::atomic<int64_t> emuWaitNs { 0 };     // of it waiting for the renderer (threaded rendering)
	std::atomic<int64_t> renderNs { 0 };      // the renderer: TA lists to Vulkan, framebuffers, presenting
	std::atomic<int64_t> renderWaitNs { 0 };  // the renderer waiting for the emulation's frame
	std::atomic<int64_t> pipelineNs { 0 };    // vkCreateGraphicsPipelines
	std::atomic<int> pipelines { 0 };
	std::atomic<int64_t> shaderNs { 0 };      // GLSL to SPIR-V (glslang) and the shader module
	std::atomic<int> shaders { 0 };
	std::atomic<int> shadersCompiled { 0 };   // of them not in the SPIR-V cache
	std::atomic<int64_t> gpuWaitNs { 0 };     // fences and idle waits: the GPU behind
	std::atomic<int> idleWaits { 0 };         // vkQueueWaitIdle / vkDeviceWaitIdle: full GPU syncs
};
extern Timing timing;

inline int64_t nowNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Adds the time of its scope to a counter.
struct ScopeTime
{
	std::atomic<int64_t>& to;
	const int64_t start = nowNs();
	explicit ScopeTime(std::atomic<int64_t>& to) : to(to) {}
	~ScopeTime() { to += nowNs() - start; }
};

// The calling thread's CPU time (its work without its waits); the wall clock if the system can't tell.
inline int64_t threadCpuNs()
{
	timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return nowNs();
	return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

// The emulation's CPU time so far, on the thread that runs it (runInternal starts and ends it, every
// vblank counts it: with threaded rendering runInternal lasts as long as the game runs).
inline void countEmulation(bool running)
{
	const int64_t now = threadCpuNs();
	const int64_t mark = timing.emuMark.exchange(running ? now : 0);
	if (mark != 0)
		timing.emuRunNs += now - mark;
}
}  // namespace emusuni
#endif
''')
addfile('core/oslib/emusuni_timing.cpp', r'''// EmuSuNi (scripts/build-dc-core.sh): see emusuni_timing.h.
#include "emusuni_timing.h"

#ifdef EMUSUNI_PORT
emusuni::Timing emusuni::timing;
#endif
''')
patch('core/emulator.cpp',
      '#include "emulator.h"\n',
      '#include "emulator.h"\n#include "emusuni_timing.h"\n')
patch('core/emulator.cpp',
      'void Emulator::runInternal()\n{\n',
      'void Emulator::runInternal()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tstruct EmuSuNiRunning\n'
      '\t{\n'
      '\t\tEmuSuNiRunning() { emusuni::countEmulation(true); }\n'
      '\t\t~EmuSuNiRunning() { emusuni::countEmulation(false); }\n'
      '\t} emusuniRunning;\n'
      '#endif\n')
patch('core/emulator.cpp',
      'void Emulator::vblank()\n{\n',
      'void Emulator::vblank()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuni::countEmulation(true);\n'
      '#endif\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '#include "network/ggpo.h"\n',
      '#include "network/ggpo.h"\n#include "emusuni_timing.h"\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\t\t\t\t\tif (type == Stop)\n\t\t\t\t\t\treturn;\n\t\t\t\t\tdequeueEvent.Wait();\n',
      '\t\t\t\t\tif (type == Stop)\n\t\t\t\t\t\treturn;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\temusuni::ScopeTime emusuniTime(emusuni::timing.emuWaitNs);\n'
      '#endif\n'
      '\t\t\t\t\tdequeueEvent.Wait();\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\t\t\tif (timeoutMs == -1)\n\t\t\t\tenqueueEvent.Wait();\n\t\t\telse if (!enqueueEvent.Wait(timeoutMs))\n\t\t\t\tbreak;\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\temusuni::ScopeTime emusuniTime(emusuni::timing.renderWaitNs);\n'
      '#endif\n'
      '\t\t\tif (timeoutMs == -1)\n\t\t\t\tenqueueEvent.Wait();\n\t\t\telse if (!enqueueEvent.Wait(timeoutMs))\n\t\t\t\tbreak;\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\tbool execute(Message msg)\n\t{\n',
      '\tbool execute(Message msg)\n\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuni::ScopeTime emusuniTime(emusuni::timing.renderNs);\n'
      '#endif\n')
patch('core/rend/vulkan/compiler.cpp',
      '#include "vulkan_context.h"\n',
      '#include "vulkan_context.h"\n#include "emusuni_timing.h"\n')
# The GLSL compiled to SPIR-V once: glslang takes milliseconds a shader on the PS4's CPU (and the
# pipeline cache only keeps the driver's part). Kept in <system>/dc/vulkan_spirv.cache, by a hash of
# each shader's text, with Flycast's version in its header; written with the pipeline cache.
patch('core/rend/vulkan/compiler.cpp',
      'vk::UniqueShaderModule ShaderCompiler::Compile(vk::ShaderStageFlagBits shaderStage, std::string const& shaderText)\n{\n'
      '\tstd::vector<unsigned int> shaderSPV;\n'
      '\tbool ok = GLSLtoSPV(shaderStage, shaderText, shaderSPV);\n'
      '\tverify(ok);\n',
      r'''#ifdef EMUSUNI_PORT
#include "oslib/oslib.h"
#include "version.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <xxhash.h>

namespace
{
constexpr char EMUSUNI_SPIRV_MAGIC[8] = { 'E', 'M', 'S', 'P', 'I', 'R', 'V', '1' };
std::unordered_map<u64, std::vector<unsigned int>> emusuniSpirv;
bool emusuniSpirvLoaded = false;
bool emusuniSpirvDirty = false;

u64 emusuniSpirvKey(vk::ShaderStageFlagBits stage, const std::string& text)
{
	return XXH3_64bits_withSeed(text.data(), text.size(), (u64)stage) ^ ((u64)text.size() << 40);
}

void emusuniSpirvLoad()
{
	emusuniSpirvLoaded = true;
	const std::string path = hostfs::getShaderCachePath("vulkan_spirv.cache");
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return;
	char magic[8];
	char version[32] = {};
	u32 count = 0;
	bool ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, EMUSUNI_SPIRV_MAGIC, 8) == 0
		&& fread(version, 1, sizeof(version), f) == sizeof(version) && strncmp(version, GIT_HASH, sizeof(version)) == 0
		&& fread(&count, 4, 1, f) == 1 && count < 100000;
	for (u32 i = 0; ok && i < count; i++)
	{
		u64 key;
		u32 words;
		ok = fread(&key, 8, 1, f) == 1 && fread(&words, 4, 1, f) == 1 && words > 0 && words < 4 * 1024 * 1024;
		if (!ok)
			break;
		std::vector<unsigned int> code(words);
		ok = fread(code.data(), 4, words, f) == words;
		if (ok)
			emusuniSpirv[key] = std::move(code);
	}
	fclose(f);
	if (!ok)
		emusuniSpirv.clear();
	NOTICE_LOG(RENDERER, "EmuSuNi: SPIR-V cache %s: %d shaders%s", path.c_str(), (int)emusuniSpirv.size(), ok ? "" : " (not this version's: not used)");
}
}  // namespace

// Called with the pipeline cache's (vk_context_lr.cpp): the shaders compiled since, written beside it first.
void EmuSuNiSaveSpirvCache()
{
	if (!emusuniSpirvDirty)
		return;
	const std::string path = hostfs::getShaderCachePath("vulkan_spirv.cache");
	const std::string part = path + ".new";
	FILE *f = fopen(part.c_str(), "wb");
	if (f == nullptr)
		return;
	char version[32] = {};
	strncpy(version, GIT_HASH, sizeof(version) - 1);
	const u32 count = (u32)emusuniSpirv.size();
	bool ok = fwrite(EMUSUNI_SPIRV_MAGIC, 1, 8, f) == 8 && fwrite(version, 1, sizeof(version), f) == sizeof(version)
		&& fwrite(&count, 4, 1, f) == 1;
	for (const auto& [key, code] : emusuniSpirv)
	{
		const u32 words = (u32)code.size();
		ok = ok && fwrite(&key, 8, 1, f) == 1 && fwrite(&words, 4, 1, f) == 1 && fwrite(code.data(), 4, words, f) == words;
	}
	if (fclose(f) != 0 || !ok || rename(part.c_str(), path.c_str()) != 0)
	{
		WARN_LOG(RENDERER, "EmuSuNi: writing the SPIR-V cache %s failed", path.c_str());
		remove(part.c_str());
		return;
	}
	emusuniSpirvDirty = false;
	NOTICE_LOG(RENDERER, "EmuSuNi: SPIR-V cache saved: %d shaders", (int)count);
}
#endif

vk::UniqueShaderModule ShaderCompiler::Compile(vk::ShaderStageFlagBits shaderStage, std::string const& shaderText)
{
#ifdef EMUSUNI_PORT
	emusuni::ScopeTime emusuniTime(emusuni::timing.shaderNs);
	emusuni::timing.shaders++;
	if (!emusuniSpirvLoaded)
		emusuniSpirvLoad();
	const u64 key = emusuniSpirvKey(shaderStage, shaderText);
	auto cached = emusuniSpirv.find(key);
	if (cached != emusuniSpirv.end())
		return VulkanContext::Instance()->GetDevice().createShaderModuleUnique
				(vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(), cached->second));
	emusuni::timing.shadersCompiled++;
#endif
	std::vector<unsigned int> shaderSPV;
	bool ok = GLSLtoSPV(shaderStage, shaderText, shaderSPV);
	verify(ok);
#ifdef EMUSUNI_PORT
	emusuniSpirv[key] = shaderSPV;
	emusuniSpirvDirty = true;
#endif
''')
# The Vulkan functions timed through the dispatcher (every caller of them), and the pipeline cache:
# only data this GPU and driver made, written again (as upstream at the end) when pipelines were
# made, at most every 600 frames: what the PS4's driver compiled slowly isn't lost if the app closes.
patch('core/rend/vulkan/vk_context_lr.cpp',
      '#include <set>\n',
      '#include <set>\n#include "emusuni_timing.h"\n')
patch('core/rend/vulkan/vk_context_lr.cpp',
      'static void EmuSuNiAdoptDevice(const retro_hw_render_interface_vulkan *ri)\n{\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.init(ri->get_instance_proc_addr);\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(ri->instance));\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(ri->device));\n',
      'static PFN_vkCreateGraphicsPipelines emusuniCreateGraphicsPipelines;\n'
      'static PFN_vkWaitForFences emusuniWaitForFences;\n'
      'static PFN_vkQueueWaitIdle emusuniQueueWaitIdle;\n'
      'static PFN_vkDeviceWaitIdle emusuniDeviceWaitIdle;\n\n'
      'static VKAPI_ATTR VkResult VKAPI_CALL EmuSuNiCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,\n'
      '\tconst VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)\n'
      '{\n'
      '\temusuni::ScopeTime time(emusuni::timing.pipelineNs);\n'
      '\temusuni::timing.pipelines += (int)count;\n'
      '\treturn emusuniCreateGraphicsPipelines(device, cache, count, infos, allocator, pipelines);\n'
      '}\n\n'
      'static VKAPI_ATTR VkResult VKAPI_CALL EmuSuNiWaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout)\n'
      '{\n'
      '\temusuni::ScopeTime time(emusuni::timing.gpuWaitNs);\n'
      '\treturn emusuniWaitForFences(device, count, fences, all, timeout);\n'
      '}\n\n'
      'static VKAPI_ATTR VkResult VKAPI_CALL EmuSuNiQueueWaitIdle(VkQueue queue)\n'
      '{\n'
      '\temusuni::ScopeTime time(emusuni::timing.gpuWaitNs);\n'
      '\temusuni::timing.idleWaits++;\n'
      '\treturn emusuniQueueWaitIdle(queue);\n'
      '}\n\n'
      'static VKAPI_ATTR VkResult VKAPI_CALL EmuSuNiDeviceWaitIdle(VkDevice device)\n'
      '{\n'
      '\temusuni::ScopeTime time(emusuni::timing.gpuWaitNs);\n'
      '\temusuni::timing.idleWaits++;\n'
      '\treturn emusuniDeviceWaitIdle(device);\n'
      '}\n\n'
      'void EmuSuNiSaveSpirvCache();  // compiler.cpp\n\n'
      'static void EmuSuNiSavePipelineCache(vk::Device device, vk::PipelineCache cache)\n'
      '{\n'
      '\tconst int64_t start = emusuni::nowNs();\n'
      '\tEmuSuNiSaveSpirvCache();\n'
      '\tstd::vector<u8> data = device.getPipelineCacheData(cache);\n'
      '\tif (data.empty())\n'
      '\t\treturn;\n'
      '\t// Written beside it first: a cut write never replaces the file.\n'
      '\tconst std::string path = hostfs::getShaderCachePath("vulkan_pipeline.cache");\n'
      '\tconst std::string part = path + ".new";\n'
      '\tFILE *f = fopen(part.c_str(), "wb");\n'
      '\tif (f == nullptr)\n'
      '\t{\n'
      '\t\tWARN_LOG(RENDERER, "EmuSuNi: can\'t write the pipeline cache %s", part.c_str());\n'
      '\t\treturn;\n'
      '\t}\n'
      '\tconst bool written = fwrite(data.data(), 1, data.size(), f) == data.size();\n'
      '\tif (fclose(f) != 0 || !written || rename(part.c_str(), path.c_str()) != 0)\n'
      '\t{\n'
      '\t\tWARN_LOG(RENDERER, "EmuSuNi: writing the pipeline cache %s failed", path.c_str());\n'
      '\t\tremove(part.c_str());\n'
      '\t\treturn;\n'
      '\t}\n'
      '\tNOTICE_LOG(RENDERER, "EmuSuNi: pipeline cache saved: %d bytes (%.1f ms)", (int)data.size(), (emusuni::nowNs() - start) / 1e6);\n'
      '}\n\n'
      'static void EmuSuNiAdoptDevice(const retro_hw_render_interface_vulkan *ri)\n{\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.init(ri->get_instance_proc_addr);\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(ri->instance));\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(ri->device));\n'
      '\temusuniCreateGraphicsPipelines = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines;\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines = EmuSuNiCreateGraphicsPipelines;\n'
      '\temusuniWaitForFences = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences;\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences = EmuSuNiWaitForFences;\n'
      '\temusuniQueueWaitIdle = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle;\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle = EmuSuNiQueueWaitIdle;\n'
      '\temusuniDeviceWaitIdle = VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle;\n'
      '\tVULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle = EmuSuNiDeviceWaitIdle;\n')
patch('core/rend/vulkan/vk_context_lr.cpp',
      '\t\tpipelineCache = device.createPipelineCacheUnique(vk::PipelineCacheCreateInfo(vk::PipelineCacheCreateFlags(), cacheSize, cacheData));\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: only data this GPU and driver made (its VkPipelineCacheHeaderVersionOne).\n'
      '\t\tif (cacheSize < 32 || *(u32 *)&cacheData[4] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE\n'
      '\t\t\t\t|| *(u32 *)&cacheData[8] != props.vendorID || *(u32 *)&cacheData[12] != props.deviceID\n'
      '\t\t\t\t|| memcmp(&cacheData[16], &props.pipelineCacheUUID[0], VK_UUID_SIZE) != 0)\n'
      '\t\t{\n'
      '\t\t\tNOTICE_LOG(RENDERER, "EmuSuNi: the pipeline cache %s (%d bytes) isn\'t this GPU\'s and driver\'s: not used", cachePath.c_str(), (int)cacheSize);\n'
      '\t\t\tcacheSize = 0;\n'
      '\t\t}\n'
      '\t\telse\n'
      '\t\t\tNOTICE_LOG(RENDERER, "EmuSuNi: pipeline cache %s: %d bytes", cachePath.c_str(), (int)cacheSize);\n'
      '#endif\n'
      '\t\tpipelineCache = device.createPipelineCacheUnique(vk::PipelineCacheCreateInfo(vk::PipelineCacheCreateFlags(), cacheSize, cacheData));\n')
patch('core/rend/vulkan/vk_context_lr.cpp',
      '\tretro_render_if->set_image(retro_render_if->handle, &retro_image, 0, nullptr, vk::QueueFamilyIgnored);\n}\n',
      '\tretro_render_if->set_image(retro_render_if->handle, &retro_image, 0, nullptr, vk::QueueFamilyIgnored);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (++emusuniFramesSinceSave >= 600 && emusuni::timing.pipelines.load() != emusuniPipelinesSaved)\n'
      '\t{\n'
      '\t\temusuniFramesSinceSave = 0;\n'
      '\t\temusuniPipelinesSaved = emusuni::timing.pipelines.load();\n'
      '\t\tEmuSuNiSavePipelineCache(device, *pipelineCache);\n'
      '\t}\n'
      '#endif\n'
      '}\n')
patch('core/rend/vulkan/vk_context_lr.cpp',
      '\t\tif (pipelineCache)\n\t\t{\n\t\t\tstd::vector<u8> cacheData = device.getPipelineCacheData(*pipelineCache);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (pipelineCache)\n'
      '\t\t\tEmuSuNiSavePipelineCache(device, *pipelineCache);  // EmuSuNi: written beside it first\n'
      '\t\tif (false)\n'
      '#else\n'
      '\t\tif (pipelineCache)\n'
      '#endif\n'
      '\t\t{\n\t\t\tstd::vector<u8> cacheData = device.getPipelineCacheData(*pipelineCache);\n')
patch('shell/libretro/libretro.cpp',
      '\tfirst_run = false;\n}\n\nstatic bool loadGame()\n',
      '\tfirst_run = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniLogTiming(emusuniRunStart, !is_dupe);\n'
      '#endif\n'
      '}\n\nstatic bool loadGame()\n')
patch('shell/libretro/libretro.cpp',
      '#ifdef EMUSUNI_PORT\nstatic void emusuniFlushDeferred()\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern bool emusuniSh4JitOk;  // hw/sh4/dyna/driver.cpp\n\n'
      '// A line every 10 s: per retro_run, where the time went (emusuni_timing.h).\n'
      'static void emusuniLogTiming(int64_t runStart, bool picture)\n'
      '{\n'
      '\tstruct Counters { int64_t emuRun, emuWait, render, renderWait, pipeline, shader, gpuWait; int pipelines, shaders, compiled, idleWaits; };\n'
      '\tauto read = []() {\n'
      '\t\tconst emusuni::Timing& t = emusuni::timing;\n'
      '\t\treturn Counters { t.emuRunNs.load(), t.emuWaitNs.load(), t.renderNs.load(), t.renderWaitNs.load(), t.pipelineNs.load(),\n'
      '\t\t\tt.shaderNs.load(), t.gpuWaitNs.load(), t.pipelines.load(), t.shaders.load(), t.shadersCompiled.load(), t.idleWaits.load() };\n'
      '\t};\n'
      '\tstatic int64_t windowStart = 0, runNs = 0;\n'
      '\tstatic int runs = 0, pictures = 0;\n'
      '\tstatic Counters before;\n'
      '\tconst int64_t now = emusuni::nowNs();\n'
      '\tif (windowStart == 0 || now - windowStart > 60000000000ll)\n'
      '\t{\n'
      '\t\t// The first frame, or a game started again after a long pause.\n'
      '\t\twindowStart = now;\n'
      '\t\trunNs = runs = pictures = 0;\n'
      '\t\tbefore = read();\n'
      '\t\treturn;\n'
      '\t}\n'
      '\trunNs += now - runStart;\n'
      '\truns++;\n'
      '\tpictures += picture ? 1 : 0;\n'
      '\tif (now - windowStart < 10000000000ll)\n'
      '\t\treturn;\n'
      '\tconst Counters c = read();\n'
      '\tconst double ms = 1e6 * runs;\n'
      '\tconst bool threaded = config::ThreadedRendering;\n'
      '\t// The emulation thread\'s CPU time (its waits not in it); without threaded rendering the renderer\n'
      '\t// runs inside the emulation (runInternal): its time taken out.\n'
      '\tconst double emulation = (c.emuRun - before.emuRun - (threaded ? 0 : c.render - before.render)) / ms;\n'
      '\tNOTICE_LOG(COMMON, "EmuSuNi: dc per frame (%d runs, %.1f pictures/s, %s, SH4 %s): retro_run %.2f ms, of it rendering %.2f "\n'
      '\t\t"and waiting for the emulation %.2f; emulation thread busy %.2f ms (waiting for the renderer %.2f); in the rendering: new pipelines %d "\n'
      '\t\t"(%.1f ms), shaders %d of them compiled %d (%.1f ms), GPU waits %.2f ms (%d full syncs)",\n'
      '\t\truns, pictures / ((now - windowStart) / 1e9), threaded ? "threaded" : "one thread", emusuniSh4JitOk ? "recompiler" : "interpreter",\n'
      '\t\trunNs / ms, (c.render - before.render) / ms, (c.renderWait - before.renderWait) / ms, emulation,\n'
      '\t\t(c.emuWait - before.emuWait) / ms, c.pipelines - before.pipelines, (c.pipeline - before.pipeline) / 1e6,\n'
      '\t\tc.shaders - before.shaders, c.compiled - before.compiled, (c.shader - before.shader) / 1e6,\n'
      '\t\t(c.gpuWait - before.gpuWait) / ms, c.idleWaits - before.idleWaits);\n'
      '\twindowStart = now;\n'
      '\trunNs = runs = pictures = 0;\n'
      '\tbefore = c;\n'
      '}\n\n'
      'static void emusuniFlushDeferred()\n{\n')

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
print("Flycast: patches applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-dc-core.sh: the flycast core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f flycast_$f"; done
} > "$OUT/prefix_flycast.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_flycast.h.new" "$OUT/prefix_flycast.h"; then
  rm -f "$OUT/prefix_flycast.h.new"
else
  mv "$OUT/prefix_flycast.h.new" "$OUT/prefix_flycast.h"
fi

if [ "$HOST" != 1 ]; then
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
fi
: > "$LOG"

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): CMake's -O3, the
# PS4's CPU and the renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -include $OUT/prefix_flycast.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a"
OPTIONS=(
  -DLIBRETRO=ON -DEMUSUNI_PORT=ON -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
  -DUSE_VULKAN=ON -DUSE_OPENGL=OFF -DUSE_GLES=OFF -DUSE_GLES2=OFF -DUSE_OPENMP=OFF
  -DUSE_HOST_GLSLANG=OFF -DUSE_HOST_LIBCHDR=OFF -DUSE_HOST_LIBZIP=OFF -DUSE_HOST_SDL=OFF -DUSE_LUA=OFF -DUSE_BREAKPAD=OFF
  -DUSE_DISCORD=OFF -DUSE_LIBCDIO=OFF -DUSE_ALSA=OFF -DUSE_PULSEAUDIO=OFF -DUSE_LIBAO=OFF
  -DENABLE_CTEST=OFF -DBUILD_TESTING=OFF -DENABLE_GDB_SERVER=OFF -DCCACHE_PROGRAM=CCACHE_PROGRAM-NOTFOUND
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CORE"
)
if [ "$HOST" != 1 ]; then
  # libzip's checks find Annex K functions in the PS4's libraries, which its headers don't declare.
  OPTIONS+=(-DHAVE_MEMCPY_S=OFF -DHAVE_STRERROR_S=OFF -DHAVE_STRERRORLEN_S=OFF -DHAVE_STRNCPY_S=OFF -DHAVE__CLOSE=OFF)
fi
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
COMMITS+=" $(git -C "$SRC" submodule status 2>/dev/null | awk '$1 !~ /^-/ {print $1}' | tr -d '+' | tr '\n' ' ')"
WANT="$SRC | $COMMITS | host $HOST | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "Flycast: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "Flycast: configuring failed (see $LOG)"
    exit 1
  fi
fi
# (-k 0: every error at once, not only the first.)
if ! ninja -C "$BUILD" -j "$JOBS" -k 0 flycast_libretro >> "$LOG" 2>&1; then
  grep -E "error:" "$LOG" | sort | uniq -c | sort -rn | head -60
  echo "Flycast: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: only the libretro API (renamed) stays global. Every library the core target was made
# of (Flycast's own, glslang, libchdr, zstd, zlib, libzip, xxHash, nowide, tinygettext, libelf and the
# embedded resources). libchdr's LZMA SDK (24.05) only for what the rest still needs: the core's own
# copy (core/deps/lzma, for 7z archives) has the same functions, and upstream's link takes those.
# Replaced only when it changed (the app isn't linked again for nothing).
mapfile -t ARCHIVES < <(find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -not -name 'liblzma.a' | sort)
ld.lld-21 -r --whole-archive "${ARCHIVES[@]}" --no-whole-archive \
  "$BUILD/core/deps/libchdr/deps/lzma-24.05/liblzma.a" -o "$OUT/dc_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='flycast_retro_*' "$OUT/dc_core_all.o" "$OUT/dc_core.o.new"
rm -f "$OUT/dc_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/dc_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "flycast_$f"; done | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "Flycast: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/dc_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved: the app would export them as imports nobody gives (the
# PS4's loader may refuse those).
WEAK=$(llvm-nm-21 -u "$OUT/dc_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "Flycast: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if llvm-nm-21 --defined-only "$OUT/dc_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "Flycast: the object defines a C++ operator new/delete of its own"
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
  # emusuni_dc_thread, is the app's to define in port/: a note if it isn't yet.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o" | awk 'NF >= 3 {print $3}';
                 echo __dso_handle; echo emusuni_dc_thread; } | sort -u)
  rm -f "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o"
  if ! grep -qs "emusuni_dc_thread" "$PROJECT"/port/*.c "$PROJECT"/port/*.cpp; then
    echo "Flycast: note: port/ doesn't define emusuni_dc_thread yet (the app won't link without it)"
  fi
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "Flycast: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
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
    echo "Flycast: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "Flycast: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app" \
       "($(echo "$NEW_DELETE" | sed '/^$/d' | wc -l) operator new/delete, port/ps4_new.cpp's)"
fi
if cmp -s "$OUT/dc_core.o.new" "$OUT/dc_core.o"; then
  rm -f "$OUT/dc_core.o.new"
else
  mv "$OUT/dc_core.o.new" "$OUT/dc_core.o"
fi
ls -la "$OUT/dc_core.o"
