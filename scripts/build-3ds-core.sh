#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Azahar (azahar-emu/azahar 2126.2, ~/src/azahar: the unified source tarball with every
# dependency in it, committed once by setup.sh so the patches below start from pristine files;
# GPL-2.0-or-later: the Nintendo 3DS emulator) for the PS4 into one object that scripts/build.sh
# links into EmuSuNi: the 3DS. Its own CMake (ENABLE_LIBRETRO=ON) makes the libretro core with the
# PS4 toolchain (toolchain/ps4-love-modern.cmake): dynarmic (the ARM11's x86-64 recompiler, one per
# emulated core), the PICA200 vertex-shader JIT, the Vulkan renderer (GLSL compiled at run time by
# glslang, SPIR-V written by sirit), the software renderer as a fallback (never a display path on the
# PS4: a second a frame), HLE audio (teakra's DSP LLE built in), decrypted .3ds/.cci/.cxi/.3dsx read
# by the core itself (stdio). Its libretro API becomes azahar_retro_* and everything else in it
# private, as for FCEUmm (CMakeLists.txt). The PS4 changes to its sources (applied each time to the
# files as git has them, checked; all under EMUSUNI_PORT, which the CMake option of that name
# defines):
#   - The JITs' code memory: one 64 MiB mapping (common/emusuni_jit.cpp), readable, writable and
#     executable, that EmuSuNi makes when the first 3DS game starts (port/ps4_n64.c:
#     emusuni_map_executable: system flexible memory on a range reserved above the app's image and
#     heap, as DolphinPS4's JIT buffers, checked by running code there) and that goes back to the
#     system when the core shuts down with nothing of it in use (retro_deinit). Never an array in the
#     core's data: the PS4 charges an executable's whole .bss to the games' flexible memory at load,
#     3DS game or not (this pool was 64 of the core's 64.4 MiB of it). Shared out first-fit to
#     dynarmic's blocks of code (24 MiB per emulated core in Old 3DS mode: 2 cores; 12 MiB each on a
#     New 3DS: 4 cores; upstream reserved 128 MiB each) and to the PICA shader JIT's 256 KiB per
#     shader (16 MiB left for them). If the PS4 refuses the mapping, the game doesn't start: a message
#     says so and the log why (the interpreters, DynCom and the shader interpreter, would be far too
#     slow there). If it is full: dynarmic's block of code is tried again at half the
#     size, down to 6 MiB (its cache flushes more often: slower, not dead), a shader that gets no
#     block runs in the shader interpreter, and the JITs of page tables no process has any more (the
#     one made before any process existed, a process that ended, the processes a save state's load
#     replaced) are given back (upstream kept them all and threw Xbyak's ERR_CANT_ALLOC out of
#     retro_run or the process switch, which ended the whole app); as a last try, a mapping of its
#     own made the same way (never reached in the tests). A game whose first JIT can't be made fails
#     to load with a message, and an exception out of the emulation ends the game
#     (RETRO_ENVIRONMENT_SHUTDOWN), not the app.
#     dynarmic's spin lock (the exclusive monitor's, taken by every LDREX/STREX) was x86 code emitted
#     at static initialisation through Xbyak's own mmap and mprotect(RWX): before main, for every
#     EmuSuNi user, whichever console they play; a console that refuses RWX would have ended the app
#     at launch. Plain C++ with the same protocol instead (common/spin_lock_x64.cpp).
#     The mapping can be anywhere: both JITs reach the core's code and data only through far calls
#     (dynarmic's CallFunction, the shader JIT's CallFarFunction: mov reg, imm64; call reg when out of
#     a 32-bit offset's reach) and absolute addresses in registers; their rip-relative operands and
#     direct jumps stay in their own buffers (dynarmic's constant pool and blocks, the shader JIT's
#     constants and labels). On this PC the mapping lands far from the code too: the PC tests run so.
#   - No page tricks: Azahar's dynarmic uses a page table, no fastmem, so there is no fault handler
#     (dynarmic's generic one on the PS4's "Generic" CMake system); nothing is mprotect-ed (checked at
#     the end: the only objects that still name mprotect are Xbyak's inline code in the generators
#     given EmuSuNi's allocator, which never runs it).
#   - Vulkan: the renderer adopts the frontend's device. EmuSuNi makes its only VkDevice at start
#     (src/Display.cpp) and never calls the context negotiation's create_device: the core takes the
#     instance, GPU, device and queue of GET_HW_RENDER_INTERFACE (upstream does too) but believed the
#     device had every extension and feature the GPU offers; now it has only what
#     EMUSUNI_VK_DEVICE_EXTENSIONS and EMUSUNI_VK_DEVICE_FEATURES say (comma-separated; the
#     VkPhysicalDeviceFeatures names plus the extension features' names: timelineSemaphore,
#     extendedDynamicState, customBorderColors, customBorderColorWithoutFormat, indexTypeUint8,
#     fragmentShaderPixelInterlock, pipelineCreationCacheControl, fragmentShaderBarycentric; none
#     when unset: the core copes, as it does on GPUs without them). Every vkDeviceWaitIdle of the
#     core under the frontend's lock_queue / unlock_queue (its submits already were). The frames in a
#     ring of 3 images of the core's own, one step per frame presented, not the frontend's sync
#     index; their render pass waits for the frontend's sampling of earlier images and makes its
#     writes visible to it.
#   - Threads: every thread Azahar names ("VulkanWorker", "Pipeline workers", "Shader workers",
#     "citra:Log", "SwRenderer workers", "PlayTimeReport"...) first calls emusuni_3ds_thread(name),
#     which the app defines (port/): its core, priority and name; the ones upstream left unnamed get
#     one ("VulkanFence": the fence wait thread; "HleAsync": the HLE's asynchronous file work;
#     "Detached"; "Teakra": the DSP LLE's own thread; "DlpServer" and "DlpClient": Download Play;
#     "AmScan": the title scans; "CamCapture"; "HttpRequest"). The AM scans' std::async takes
#     std::launch::async: the default policy may run the task on the thread that waits for it, the
#     emulation thread, which the hook must never re-place. The core's own priority calls do nothing
#     (the PS4 runs threads by fixed priorities, which the hook sets). 2 pipeline and 2 shader
#     compile workers (upstream: half the CPUs each).
#   - No networking or SSL: LibreSSL isn't built, cpp-httplib without it (the 3DS's HTTPS requests
#     fail at once; plain HTTP, the NUS downloads and the rooms compile against the PS4's sockets
#     but nothing in the app uses them). Every random byte from common/emusuni_random.cpp (getentropy,
#     else /dev/urandom, else a generator seeded from the clock; said once in the log): the ssl:C
#     service's and the AM ticket keys' (LibreSSL's RAND_bytes), Crypto++'s OS generator
#     (arc4random_buf on a FreeBSD target, which the PS4's libc hasn't) and every std::random_device
#     (libc++'s opens /dev/urandom, which the PS4 may not have: the core timing's jitter, the rooms,
#     cpp-httplib's multipart boundary; checked at the end: none left in the object).
#   - No built-in keys (ENABLE_BUILTIN_KEYBLOB off): upstream compiles Nintendo's AES keys into the
#     binary (encrypted in its source) for players without a key dump. As the other consoles' BIOS
#     files, they are the player's: sysdata/aes_keys.txt in the core's directory (GodMode9 dumps it
#     with the games). Decrypted games run without them; the few that encrypt through the PS or APT
#     services (and amiibo) need them, and the core logs which key it misses.
#   - The 3DS's memory off the app's heap: FCRAM (256 MiB, always the New 3DS size), VRAM and the
#     New 3DS extra RAM in blocks of their own (common/emusuni_mem.cpp): direct memory on the PS4
#     (sceKernelAllocateDirectMemory, sceKernelMapDirectMemory), an mmap elsewhere; freed with the
#     emulator. DynCom's 125 MiB translation cache the same way, and only when the interpreter runs
#     (upstream's array stayed in the app's memory for nothing). A save state is serialised into one
#     vector with room for the emulated RAM reserved and zstd-compressed from it by frames, and read
#     back in place (upstream's string stream, its copy and the one-shot compression peaked at 5 x
#     the RAM on the heap: 1.2 GiB measured on a New 3DS; the load copied the state twice more).
#   - The Vulkan pipeline cache makes its shader cache object on first use when no disk resources
#     were loaded (the disk cache option off, or the renderer made again by a save state's load):
#     upstream read through a null pointer and crashed at the first draw.
#   - $HOME unset (the PS4): /data/EmuSuNi instead of a failed assertion (the libretro save
#     directory is where the core's files really go).
#   - Built as a static library, without LTO, precompiled headers on, no -Werror, without the stack
#     protector and _FORTIFY_SOURCE (musl has neither), with its own Boost 1.89 (serialization and
#     iostreams compiled), Crypto++, fmt, zstd, glslang, SPIRV-Tools, sirit, VMA, teakra, faad2,
#     SoundTouch, enet, xxHash, lodepng, inih (nothing of the system's), as upstream's libretro
#     builds. The PS4's CMake system is "Generic": glslang's POSIX parts taken anyway (as the PSP
#     port), Boost.Asio without kqueue (no sys/event.h), the submodule check (no .git in the
#     submodules) skipped.
# The object is linked from Azahar's own libraries whole (the libretro glue, Boost.Serialization's
# class registrations live there) and from its dependencies only what they reference: linked whole,
# they carried 4.6 MB of code and 6.4 MB of static memory nothing used (glslang's disassembler
# tables, teakra's test generator, Crypto++'s other ciphers).
# The object is checked at the end: only azahar_retro_* defined globally, no weak references left,
# the C++ operator new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and
# none defined in it, no std::random_device, mprotect named only by the objects listed above, and
# every other symbol it needs found in what the app links (OpenOrbis' libc, libkernel,
# libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c: emusuni_3ds_thread
# and the JIT memory's emusuni_map_executable / emusuni_unmap_executable).
# The sources must be the 2126.2 tarball setup.sh commits (its GIT-COMMIT file is checked first).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-3ds-core.sh <out dir> [clean]
#   CTR_CORE_HOST=1 build-3ds-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                 the PC tests: no PS4 checks; the test frontend
#                                                 (scratchpad: 3dsfront.cpp) provides
#                                                 emusuni_map_executable, emusuni_unmap_executable
#                                                 and emusuni_3ds_thread.
#                                                 (env 3DS_CORE_HOST=1 is read too; bash can't name
#                                                 a variable that starts with a digit.)
#   CTR_CORE_SECTIONS=1 build-3ds-core.sh <another out dir>
#                                                 a section for each function and variable (-ffunction-
#                                                 sections -fdata-sections), for a final link with
#                                                 --gc-sections to drop what nothing reaches.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${AZAHAR_DIR:-$HOME/src/azahar}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/3ds-core.log"
HOST="${CTR_CORE_HOST:-$(printenv 3DS_CORE_HOST 2>/dev/null || echo 0)}"
JOBS="${CTR_CORE_JOBS:-4}"   # at most 4 compilers at once (the PC's load)

# The tree setup.sh commits: the 2126.2 unified tarball, whose GIT-COMMIT file names the commit it
# was made from. Another tree would fail the patches' checks below one at a time; said once here.
AZAHAR_GIT_COMMIT=219d743a1e698101fe1ab8bef43e6563126c3c27
TREE_COMMIT="$(tr -d '[:space:]' < "$SRC/GIT-COMMIT" 2>/dev/null || true)"
if [ "$TREE_COMMIT" != "$AZAHAR_GIT_COMMIT" ]; then
  echo "Azahar: $SRC isn't the 2126.2 unified source (GIT-COMMIT ${TREE_COMMIT:-missing}, wanted $AZAHAR_GIT_COMMIT): run scripts/setup.sh"
  exit 1
fi
if ! git -C "$SRC" rev-parse -q --verify HEAD >/dev/null 2>&1; then
  echo "Azahar: $SRC has no commit to patch from (setup.sh commits the tarball once, git add -A -f)"
  exit 1
fi

python3 -I - "$SRC" <<'PATCH'
import os
import subprocess
import sys
root = sys.argv[1]
patched = {}
def git_file(repo, path):
    run = subprocess.run(['git', '-C', repo, 'show', 'HEAD:' + path], capture_output=True)
    if run.returncode != 0:
        # (setup.sh commits the whole tarball, git add -A -f: its own .gitignore would hide files.)
        sys.exit("Azahar: %s isn't in the commit of %s: run scripts/setup.sh again" % (path, repo))
    return run.stdout.decode('utf-8').replace('\r\n', '\n')
def patch(path, old, new, repo=None):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    # (The unified tarball is one repository: the submodules' files are reached by their paths.)
    key = (repo or root, path)
    if key not in patched:
        patched[key] = git_file(key[0], path)
    text = patched[key]
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[key] = text.replace(old, new)
def addfile(path, text):
    # A file of EmuSuNi's own in the tree (git doesn't know it).
    patched[(root, path)] = text

# --- The build: the EMUSUNI_PORT option, a static libretro core, no submodule check, no stack
# protector or fortify, no LibreSSL.
patch('CMakeLists.txt',
      'option(ENABLE_LIBRETRO "Build as a LibRetro core" OFF)\n',
      'option(ENABLE_LIBRETRO "Build as a LibRetro core" OFF)\n'
      '# EmuSuNi (scripts/build-3ds-core.sh): the libretro core as one object of the PS4 app (a static\n'
      '# library), with the changes under EMUSUNI_PORT.\n'
      'option(EMUSUNI_PORT "Build the libretro core for EmuSuNi (PS4)" OFF)\n'
      'if(EMUSUNI_PORT)\n'
      '    add_compile_definitions(EMUSUNI_PORT=1)\n'
      '    # No zstd tracing hooks (weak references nothing defines).\n'
      '    add_compile_definitions(ZSTD_TRACE=0)\n'
      '    if(NOT UNIX)\n'
      '        # The PS4 (CMake\'s "Generic" system, FreeBSD headers without sys/event.h): Boost.Asio\'s\n'
      '        # select reactor, no kqueue.\n'
      '        add_compile_definitions(BOOST_ASIO_DISABLE_KQUEUE)\n'
      '    endif()\n'
      'endif()\n')
patch('CMakeLists.txt',
      'if (EXISTS "${PROJECT_SOURCE_DIR}/.git/objects")\n    # only check submodules when source is obtained via Git\n',
      'if (EXISTS "${PROJECT_SOURCE_DIR}/.git/objects" AND NOT EMUSUNI_PORT)  # EmuSuNi: the unified tarball, one repository\n'
      '    # only check submodules when source is obtained via Git\n')
patch('src/CMakeLists.txt',
      '    add_compile_options(\n        -Wall\n'
      '        # In case a flag isn\'t supported on e.g. a certain architecture, don\'t error.\n'
      '        -Wno-unused-command-line-argument\n'
      '        # Build fortification options\n'
      '        -fstack-protector-strong\n    )\n',
      '    add_compile_options(\n        -Wall\n'
      '        # In case a flag isn\'t supported on e.g. a certain architecture, don\'t error.\n'
      '        -Wno-unused-command-line-argument\n    )\n'
      '    if (NOT EMUSUNI_PORT)  # EmuSuNi: no stack protector on the PS4 (speed; musl has no fortify)\n'
      '        # Build fortification options\n'
      '        add_compile_options(-fstack-protector-strong)\n'
      '    endif()\n')
patch('src/CMakeLists.txt',
      '    if (NOT CMAKE_BUILD_TYPE STREQUAL Debug AND NOT FORTIFY_SOURCE_DEFINED)\n',
      '    if (NOT CMAKE_BUILD_TYPE STREQUAL Debug AND NOT FORTIFY_SOURCE_DEFINED AND NOT EMUSUNI_PORT)\n')
patch('src/citra_libretro/CMakeLists.txt',
      'add_library(citra_libretro SHARED\n        citra_libretro.cpp\n',
      'if(EMUSUNI_PORT)\n'
      '  # EmuSuNi: linked into the app (scripts/build-3ds-core.sh makes one object of it).\n'
      '  set(EMUSUNI_LIBRETRO_KIND STATIC)\n'
      'else()\n'
      '  set(EMUSUNI_LIBRETRO_KIND SHARED)\n'
      'endif()\n'
      'add_library(citra_libretro ${EMUSUNI_LIBRETRO_KIND}\n        citra_libretro.cpp\n')
patch('externals/CMakeLists.txt',
      '# OpenSSL\nif (USE_SYSTEM_OPENSSL)\n',
      '# OpenSSL\n'
      'if (EMUSUNI_PORT)\n'
      '    # EmuSuNi: no LibreSSL (no HTTPS: the app has no network use; the OpenSSL licence question with it).\n'
      '    set(OPENSSL_LIBRARIES "")\n'
      'elseif (USE_SYSTEM_OPENSSL)\n')
patch('externals/CMakeLists.txt',
      'target_compile_options(httplib INTERFACE -DCPPHTTPLIB_OPENSSL_SUPPORT)\n'
      'target_link_libraries(httplib INTERFACE ${OPENSSL_LIBRARIES})\n',
      'if (NOT EMUSUNI_PORT)  # EmuSuNi: cpp-httplib without SSL\n'
      '    target_compile_options(httplib INTERFACE -DCPPHTTPLIB_OPENSSL_SUPPORT)\n'
      '    target_link_libraries(httplib INTERFACE ${OPENSSL_LIBRARIES})\n'
      'endif()\n')
# The PS4's CMake system is "Generic" (OpenOrbis' toolchain): glslang's POSIX OS layer (upstream
# picks it only for UNIX/Fuchsia/Android), as the PSP port does.
patch('externals/glslang/CMakeLists.txt',
      'elseif(UNIX OR ANDROID)\n    add_compile_definitions(GLSLANG_OSINCLUDE_UNIX)\n',
      'elseif(UNIX OR ANDROID OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n    add_compile_definitions(GLSLANG_OSINCLUDE_UNIX)\n')
patch('externals/glslang/glslang/CMakeLists.txt',
      'elseif(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID)\n    add_subdirectory(OSDependent/Unix)\n',
      'elseif(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n'
      '    add_subdirectory(OSDependent/Unix)\n')
patch('externals/glslang/glslang/CMakeLists.txt',
      'if(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID)\n    set(THREADS_PREFER_PTHREAD_FLAG ON)\n',
      'if(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n'
      '    set(THREADS_PREFER_PTHREAD_FLAG ON)\n')
patch('src/common/CMakeLists.txt',
      'add_library(citra_common STATIC EXCLUDE_FROM_ALL\n    aarch64/cpu_detect.cpp\n',
      'add_library(citra_common STATIC EXCLUDE_FROM_ALL\n'
      '    emusuni_jit.cpp\n    emusuni_jit.h\n    emusuni_mem.cpp\n    emusuni_mem.h\n    emusuni_random.cpp\n'
      '    aarch64/cpu_detect.cpp\n')

# --- The JITs' code memory: one mapping the app makes executable when the first game starts, shared out
# to dynarmic's blocks of code and the PICA shader JIT, given back when the core shuts down.
addfile('src/common/emusuni_jit.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the JITs' code memory. One mapping the app makes readable,
// writable and executable when the first game starts (port/ps4_n64.c: emusuni_map_executable), shared
// out to dynarmic's blocks of code (core/arm/dynarmic) and the PICA shader JIT's (video_core/shader),
// and given back when the core shuts down (retro_deinit). Without it the game doesn't start
// (core/core.cpp): never part of the executable, whose whole .bss the PS4 charges at load.
#pragma once
#ifdef EMUSUNI_PORT
#include <cstddef>

namespace Xbyak {
struct Allocator;
}

namespace EmuSuNi {
// Whether the JIT memory is there: mapped on the first call after the core starts (a refusal is tried
// again by the next game).
bool JitAvailable();
// A block of it (16 KiB granularity), or of a mapping of its own when it is full; nullptr when neither
// is possible.
void* JitAlloc(std::size_t size);
void JitFree(void* p);
// For Xbyak::CodeGenerator (the shader JIT): the JIT memory, no mprotect.
Xbyak::Allocator* XbyakAllocator();
// The core shuts down (retro_deinit): the mapping back to the system when nothing of it is in use.
void JitRelease();
} // namespace EmuSuNi
#endif
''')
addfile('src/common/emusuni_jit.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): see emusuni_jit.h.
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <xbyak/xbyak.h>
#include "common/common_types.h"
#include "common/emusuni_jit.h"
#include "common/logging/log.h"

// The app's (port/ps4_n64.c; on this PC the test frontend's): `size` bytes to write code in and run it,
// readable, writable and executable (on the PS4: system flexible memory on a range reserved above the
// app's image and heap, checked by running code there), or nullptr when the system refuses (the app's
// log says why); emusuni_unmap_executable gives them back.
extern "C" void* emusuni_map_executable(size_t size, const char* name);
extern "C" void emusuni_unmap_executable(void* address, size_t size);

namespace EmuSuNi {
namespace {
// 64 MiB: dynarmic's blocks (24 MiB per emulated core on an Old 3DS, 12 MiB each on a New 3DS: 48 MiB
// either way) and 16 MiB for the PICA shader JIT (256 KiB a shader). Wherever the system puts it: both
// JITs reach the core's code and data through far calls and absolute addresses, never 32-bit offsets.
constexpr size_t POOL_SIZE = 64u << 20;
constexpr size_t GRANULE = 16384; // the PS4's page
u8* g_pool = nullptr;
std::mutex g_lock;
int g_state = 0; // 0: not mapped (not tried yet, or given back), 1: mapped, -1: refused
std::map<size_t, size_t> g_free;  // offset -> size of each free block of the pool
std::map<void*, size_t> g_used;   // block -> size, of the pool's blocks
std::map<void*, size_t> g_mapped; // blocks mapped on their own when the pool was full -> their size
size_t g_test_cap = 0;            // the PC tests' cap on the pool (EMUSUNI_3DS_JIT_TEST_KIB), 0 otherwise

bool Available() {
    if (g_state == 0) {
        g_pool = static_cast<u8*>(emusuni_map_executable(POOL_SIZE, "3ds jit"));
        g_state = g_pool != nullptr ? 1 : -1;
        if (g_pool != nullptr) {
            g_free.clear();
            g_free[0] = POOL_SIZE;
            g_test_cap = 0;
            // (The PC tests: EMUSUNI_3DS_JIT_TEST_KIB caps the pool and refuses the mappings of their own
            // below, as a console short of both would; nothing sets it on the PS4.)
            if (const char* cap = std::getenv("EMUSUNI_3DS_JIT_TEST_KIB")) {
                g_test_cap = (static_cast<size_t>(std::strtoull(cap, nullptr, 10)) * 1024) & ~(GRANULE - 1);
                if (g_test_cap > 0 && g_test_cap < POOL_SIZE) {
                    g_free[0] = g_test_cap;
                    LOG_WARNING(Common, "EmuSuNi: the JIT memory capped at {} KiB for a test", g_test_cap / 1024);
                }
            }
            LOG_INFO(Common, "EmuSuNi: {} MiB of executable JIT memory at {}", POOL_SIZE >> 20,
                     static_cast<void*>(g_pool));
        } else {
            LOG_CRITICAL(Common, "EmuSuNi: no executable JIT memory ({} MiB): the game can't start",
                         POOL_SIZE >> 20);
        }
    }
    return g_state > 0;
}

class PoolAllocator final : public Xbyak::Allocator {
public:
    uint8_t* alloc(size_t size) override {
        void* p = JitAlloc(size);
        if (p == nullptr) {
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        return static_cast<uint8_t*>(p);
    }
    void free(uint8_t* p) override {
        JitFree(p);
    }
    bool useProtect() const override {
        return false;
    }
};
PoolAllocator g_xbyak_allocator;
} // namespace

bool JitAvailable() {
    std::lock_guard guard{g_lock};
    return Available();
}

void* JitAlloc(size_t size) {
    std::lock_guard guard{g_lock};
    if (!Available() || size == 0) {
        return nullptr;
    }
    const size_t want = (size + GRANULE - 1) & ~(GRANULE - 1);
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        if (it->second < want) {
            continue;
        }
        const size_t offset = it->first, left = it->second - want;
        g_free.erase(it);
        if (left) {
            g_free[offset + want] = left;
        }
        g_used[g_pool + offset] = want;
        return g_pool + offset;
    }
    // Full (the callers cope: a smaller block of code, the shader interpreter). A last try: a mapping
    // of its own, made the same way.
    size_t left = 0;
    for (const auto& block : g_free) {
        left += block.second;
    }
    LOG_WARNING(Common, "EmuSuNi: the JIT memory is full: {} KiB wanted, {} KiB free in {} pieces", want / 1024,
                left / 1024, g_free.size());
    if (g_test_cap != 0) {
        return nullptr; // (the test: no mapping either)
    }
    void* p = emusuni_map_executable(want, "3ds jit (more)");
    if (p == nullptr) {
        LOG_ERROR(Common, "EmuSuNi: the JIT memory is full and no mapping for {} KiB more", want / 1024);
        return nullptr;
    }
    LOG_WARNING(Common, "EmuSuNi: the JIT memory is full: {} KiB mapped at {}", want / 1024, p);
    g_mapped[p] = want;
    return p;
}

void JitFree(void* p) {
    if (p == nullptr) {
        return;
    }
    std::lock_guard guard{g_lock};
    if (auto mapped = g_mapped.find(p); mapped != g_mapped.end()) {
        emusuni_unmap_executable(p, mapped->second);
        g_mapped.erase(mapped);
        return;
    }
    auto used = g_used.find(p);
    if (used == g_used.end()) {
        LOG_ERROR(Common, "EmuSuNi: freeing {}, not a block of the JIT memory", p);
        return;
    }
    size_t offset = static_cast<u8*>(p) - g_pool;
    size_t length = used->second;
    g_used.erase(used);
    // Back to the free blocks, merged with its neighbours.
    auto next = g_free.lower_bound(offset);
    if (next != g_free.end() && offset + length == next->first) {
        length += next->second;
        next = g_free.erase(next);
    }
    if (next != g_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += length;
            return;
        }
    }
    g_free[offset] = length;
}

Xbyak::Allocator* XbyakAllocator() {
    return &g_xbyak_allocator;
}

void JitRelease() {
    std::lock_guard guard{g_lock};
    if (g_state < 0) {
        g_state = 0; // (refused: the next game asks again)
        return;
    }
    if (g_state == 0) {
        return;
    }
    if (!g_used.empty() || !g_mapped.empty()) {
        // (Code of the emulator still there: the mapping stays, as it is, for the next game.)
        LOG_WARNING(Common, "EmuSuNi: the JIT memory kept: {} blocks of it still in use",
                    g_used.size() + g_mapped.size());
        return;
    }
    emusuni_unmap_executable(g_pool, POOL_SIZE);
    LOG_INFO(Common, "EmuSuNi: the {} MiB of JIT memory given back", POOL_SIZE >> 20);
    g_pool = nullptr;
    g_free.clear();
    g_state = 0;
}
} // namespace EmuSuNi
#endif
''')
patch('externals/dynarmic/src/dynarmic/backend/x64/block_of_code.cpp',
      '#else\n    static constexpr size_t DYNARMIC_PAGE_SIZE = 4096;\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '    // EmuSuNi (scripts/build-3ds-core.sh): the blocks of code from EmuSuNi\'s pool of executable\n'
      '    // memory (src/common/emusuni_jit.cpp): never mmap and mprotect, which the PS4 refuses.\n'
      '    uint8_t* alloc(size_t size) override {\n'
      '        void* p = EmuSuNi::JitAlloc(size);\n'
      '        if (p == nullptr) {\n'
      '            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);\n'
      '        }\n'
      '        return static_cast<uint8_t*>(p);\n'
      '    }\n\n'
      '    void free(uint8_t* p) override {\n'
      '        EmuSuNi::JitFree(p);\n'
      '    }\n\n'
      '    bool useProtect() const override { return false; }\n'
      '#else\n    static constexpr size_t DYNARMIC_PAGE_SIZE = 4096;\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/block_of_code.cpp',
      'namespace Dynarmic::Backend::X64 {\n\n#ifdef _WIN32\nconst Xbyak::Reg64 BlockOfCode::ABI_RETURN',
      '#ifdef EMUSUNI_PORT\n'
      'namespace EmuSuNi {  // src/common/emusuni_jit.cpp\n'
      'void* JitAlloc(size_t size);\n'
      'void JitFree(void* p);\n'
      '}  // namespace EmuSuNi\n'
      '#endif\n\n'
      'namespace Dynarmic::Backend::X64 {\n\n#ifdef _WIN32\nconst Xbyak::Reg64 BlockOfCode::ABI_RETURN')
# dynarmic's spin lock (the exclusive monitor's: every LDREX/STREX takes it) was two routines emitted
# with Xbyak by a global CodeGenerator at static initialisation: Xbyak's own mmap and mprotect(RWX)
# before main, for every EmuSuNi user whatever console they play (some PS4 firmwares refuse RWX: the
# app would have ended at launch). The end check on mprotect can't see such a generator (Xbyak's
# inline code names mprotect in every one), so none may exist. The same protocol in C++: the JIT's
# blocks take and release the lock with it (backend/x64/emit_x64_memory.h: lock xchg, test, pause;
# xchg 0, mfence).
patch('externals/dynarmic/src/dynarmic/common/spin_lock_x64.cpp',
      r'''namespace {

struct SpinLockImpl {
    void Initialize();

    Xbyak::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;
SpinLockImpl impl;

void SpinLockImpl::Initialize() {
    const Xbyak::Reg64 ABI_PARAM1 = Backend::X64::HostLocToReg64(Backend::X64::ABI_PARAM1);

    code.align();
    lock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, ABI_PARAM1, code.eax);
    code.ret();

    code.align();
    unlock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, ABI_PARAM1, code.eax);
    code.ret();
}

}  // namespace

void SpinLock::Lock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
}

void SpinLock::Unlock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
}
''',
      r'''#ifdef EMUSUNI_PORT
// EmuSuNi (scripts/build-3ds-core.sh): upstream emitted these two routines with Xbyak at static
// initialisation (a global CodeGenerator: its own mmap and mprotect(RWX) before main, whichever
// console runs; some PS4 firmwares refuse RWX and the app would end at launch). The same protocol
// in C++: the JIT's blocks take and release the exclusive monitor's lock with it
// (backend/x64/emit_x64_memory.h: lock xchg, test, pause; xchg 0, mfence).
void SpinLock::Lock() {
    while (__atomic_exchange_n(&storage, 1, __ATOMIC_ACQUIRE) != 0) {
        __builtin_ia32_pause();
    }
}

void SpinLock::Unlock() {
    __atomic_exchange_n(&storage, 0, __ATOMIC_SEQ_CST);
    __builtin_ia32_mfence();
}
#else
namespace {

struct SpinLockImpl {
    void Initialize();

    Xbyak::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;
SpinLockImpl impl;

void SpinLockImpl::Initialize() {
    const Xbyak::Reg64 ABI_PARAM1 = Backend::X64::HostLocToReg64(Backend::X64::ABI_PARAM1);

    code.align();
    lock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, ABI_PARAM1, code.eax);
    code.ret();

    code.align();
    unlock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, ABI_PARAM1, code.eax);
    code.ret();
}

}  // namespace

void SpinLock::Lock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
}

void SpinLock::Unlock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
}
#endif
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/settings.h"\n'
      '#include "core/hle/kernel/kernel.h"\n#include "core/hle/kernel/process.h"\n#endif\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    config.define_unpredictable_behaviour = true;\n\n    // Multi-process state\n',
      '    config.define_unpredictable_behaviour = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a share of the 64 MiB pool (common/emusuni_jit.cpp) for each emulated core (upstream\n'
      '    // reserved 128 MiB each): 24 MiB on an Old 3DS (2 cores), 12 MiB on a New 3DS (4 cores).\n'
      '    config.code_cache_size = Settings::values.is_new_3ds.GetValue() ? 12u << 20 : 24u << 20;\n'
      '#endif\n\n'
      '    // Multi-process state\n')
# The pool may not have a whole share left (the shader JIT, a process more): half the size again,
# down to 6 MiB (24, 12, 6; 12, 6 on a New 3DS); nothing at all is a clear error for the loader
# (do_load_game catches it: the game fails with a message). Upstream let Xbyak's exception out of
# the process switch end the app.
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    config.global_monitor = &exclusive_monitor.monitor;\n\n'
      '    return std::make_unique<Dynarmic::A32::Jit>(config);\n}\n',
      '    config.global_monitor = &exclusive_monitor.monitor;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the block of code comes from the 64 MiB pool (common/emusuni_jit.cpp). When it hasn\'t\n'
      '    // that much left, half the size again, down to 6 MiB (the cache flushes more often: slower, not\n'
      '    // dead); nothing at all is a clear error for the loader (upstream let Xbyak\'s end the app).\n'
      '    for (;;) {\n'
      '        try {\n'
      '            return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '        } catch (const std::exception& error) {\n'
      '            if (config.code_cache_size < 8u << 20) {\n'
      '                throw std::runtime_error(fmt::format("no executable memory for the CPU JIT ({} MiB: {})",\n'
      '                                                     config.code_cache_size >> 20, error.what()));\n'
      '            }\n'
      '            LOG_WARNING(Core_ARM11, "EmuSuNi: no {} MiB of JIT memory for core {} ({}): {} MiB tried",\n'
      '                        config.code_cache_size >> 20, GetID(), error.what(), config.code_cache_size >> 21);\n'
      '            config.code_cache_size /= 2;\n'
      '        }\n'
      '    }\n'
      '#else\n'
      '    return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '#endif\n'
      '}\n')
# Each core's JIT is made before any process exists (a null page table) and again for the process:
# upstream kept the first (a map by page table, 128 MiB each reserved, never touched), and the JITs of
# processes that ended and of the processes a save state's load replaced (new page tables). With the
# pool shared out they are given back here, before the new JIT takes its share: a page table is dead
# when no process of the kernel's list has it (the cores' maps alone hold it).
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    auto new_jit = MakeJit();\n    jit = new_jit.get();\n    LoadContext(ctx);\n'
      '    jits.emplace(current_page_table, std::move(new_jit));\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the JITs of page tables no process has any more (the one made before any process\n'
      '    // existed, a process that ended, the processes a save state\'s load replaced): their shares of\n'
      '    // the pool back (common/emusuni_jit.cpp) before the new JIT takes its own. Upstream kept them\n'
      '    // all, 128 MiB reserved each. (The context was saved above; jit is set again below.)\n'
      '    jit = nullptr;\n'
      '    for (auto it = jits.begin(); it != jits.end();) {\n'
      '        bool live = false;\n'
      '        if (it->first != nullptr) {\n'
      '            for (const auto& process : system.Kernel().GetProcessList()) {\n'
      '                live |= process->vm_manager.page_table == it->first;\n'
      '            }\n'
      '        }\n'
      '        if (live) {\n'
      '            ++it;\n'
      '        } else {\n'
      '            LOG_INFO(Core_ARM11, "EmuSuNi: core {}: the JIT of a page table no process has given back", GetID());\n'
      '            it = jits.erase(it);\n'
      '        }\n'
      '    }\n'
      '#endif\n'
      '    auto new_jit = MakeJit();\n    jit = new_jit.get();\n    LoadContext(ctx);\n'
      '    jits.emplace(current_page_table, std::move(new_jit));\n'
      '}\n')
patch('src/core/core.cpp',
      '#include "common/scope_exit.h"\n#include "common/settings.h"\n',
      '#include "common/scope_exit.h"\n#include "common/settings.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <stdexcept>\n'
      '#include "common/emusuni_jit.h"\n'
      '// EmuSuNi: dynarmic in the JIT memory the app maps when the game starts (common/emusuni_jit.cpp).\n'
      '// Without it the game doesn\'t start (do_load_game shows the message, in the player\'s words): DynCom,\n'
      '// the interpreter, is far too slow on the PS4.\n'
      'static bool EmuSuNiCpuJit() {\n'
      '    if (!Settings::values.use_cpu_jit.GetValue()) {\n'
      '        return false;\n'
      '    }\n'
      '    if (!EmuSuNi::JitAvailable()) {\n'
      '        throw std::runtime_error("The PS4 gave no memory for the 3DS\'s processor\\n"\n'
      '                                 "(64 MiB to run its code: /data/EmuSuNi/emusuni.log says why)");\n'
      '    }\n'
      '    return true;\n'
      '}\n'
      '#define EMUSUNI_3DS_CPU_JIT EmuSuNiCpuJit()\n'
      '#else\n'
      '#define EMUSUNI_3DS_CPU_JIT Settings::values.use_cpu_jit\n'
      '#endif\n')
patch('src/core/core.cpp',
      '    if (Settings::values.use_cpu_jit) {\n#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)\n',
      '    if (EMUSUNI_3DS_CPU_JIT) {\n#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)\n')
patch('src/video_core/shader/shader.cpp',
      '#include "common/arch.h"\n#include "video_core/shader/shader_interpreter.h"\n',
      '#include "common/arch.h"\n#include "video_core/shader/shader_interpreter.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "common/emusuni_jit.h"\n'
      '// EmuSuNi: the shader JIT only when its code memory is executable (else the interpreter).\n'
      '#define EMUSUNI_3DS_SHADER_JIT && EmuSuNi::JitAvailable()\n'
      '#else\n'
      '#define EMUSUNI_3DS_SHADER_JIT\n'
      '#endif\n')
patch('src/video_core/shader/shader.cpp',
      '    if (use_jit) {\n        return std::make_unique<Shader::JitEngine>();\n',
      '    if (use_jit EMUSUNI_3DS_SHADER_JIT) {\n        return std::make_unique<Shader::JitEngine>();\n')
patch('src/video_core/shader/shader_jit_x64_compiler.cpp',
      '#include "common/x64/xbyak_util.h"\n#include "video_core/pica/shader_unit.h"\n',
      '#include "common/x64/xbyak_util.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_jit.h"\n#endif\n'
      '#include "video_core/pica/shader_unit.h"\n')
patch('src/video_core/shader/shader_jit_x64_compiler.cpp',
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE) {}\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the code from the pool of executable memory (common/emusuni_jit.cpp), never Xbyak\'s mmap\n'
      '// and mprotect.\n'
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE, nullptr, EmuSuNi::XbyakAllocator()) {}\n'
      '#else\n'
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE) {}\n'
      '#endif\n')
# A shader program that gets no block of the pool (256 KiB each, never freed by upstream's cache; the
# pool full) runs in the shader interpreter instead: upstream threw Xbyak's error out of retro_run.
patch('src/video_core/shader/shader_jit.h',
      '#include <memory>\n#include <unordered_map>\n#include "common/common_types.h"\n'
      '#include "video_core/shader/shader.h"\n',
      '#include <memory>\n#include <unordered_map>\n'
      '#ifdef EMUSUNI_PORT\n#include <unordered_set>\n#endif\n'
      '#include "common/common_types.h"\n'
      '#include "video_core/shader/shader.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "video_core/shader/shader_interpreter.h"\n#endif\n')
patch('src/video_core/shader/shader_jit.h',
      'private:\n    std::unordered_map<u64, std::unique_ptr<JitShader>> cache;\n};\n',
      'private:\n    std::unordered_map<u64, std::unique_ptr<JitShader>> cache;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the programs that got no block of the JITs\' memory run here instead.\n'
      '    InterpreterEngine emusuni_interpreter;\n'
      '    std::unordered_set<u64> emusuni_interpreted;\n'
      '#endif\n'
      '};\n')
patch('src/video_core/shader/shader_jit.cpp',
      '#include "common/microprofile.h"\n#include "video_core/shader/shader.h"\n',
      '#include "common/microprofile.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/logging/log.h"\n#endif\n'
      '#include "video_core/shader/shader.h"\n')
patch('src/video_core/shader/shader_jit.cpp',
      '    auto iter = cache.find(cache_key);\n'
      '    if (iter != cache.end()) {\n'
      '        setup.cached_shader = iter->second.get();\n'
      '    } else {\n'
      '        auto shader = std::make_unique<JitShader>();\n'
      '        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());\n'
      '        setup.cached_shader = shader.get();\n'
      '        cache.emplace_hint(iter, cache_key, std::move(shader));\n'
      '    }\n'
      '}\n',
      '    auto iter = cache.find(cache_key);\n'
      '    if (iter != cache.end()) {\n'
      '        setup.cached_shader = iter->second.get();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    } else if (emusuni_interpreted.contains(cache_key)) {\n'
      '        setup.cached_shader = nullptr; // EmuSuNi: interpreted (Run)\n'
      '    } else {\n'
      '        // EmuSuNi: a program that gets no block of the JITs\' memory (common/emusuni_jit.cpp: the pool\n'
      '        // full, no mapping) runs in the interpreter; upstream threw Xbyak\'s error out of retro_run.\n'
      '        std::unique_ptr<JitShader> shader;\n'
      '        try {\n'
      '            shader = std::make_unique<JitShader>();\n'
      '        } catch (const Xbyak::Error& error) {\n'
      '            LOG_ERROR(HW_GPU, "EmuSuNi: no JIT memory for shader program {:016X} ({}): interpreted", cache_key,\n'
      '                      error.what());\n'
      '            emusuni_interpreted.insert(cache_key);\n'
      '            setup.cached_shader = nullptr;\n'
      '            return;\n'
      '        }\n'
      '        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());\n'
      '        setup.cached_shader = shader.get();\n'
      '        cache.emplace_hint(iter, cache_key, std::move(shader));\n'
      '    }\n'
      '#else\n'
      '    } else {\n'
      '        auto shader = std::make_unique<JitShader>();\n'
      '        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());\n'
      '        setup.cached_shader = shader.get();\n'
      '        cache.emplace_hint(iter, cache_key, std::move(shader));\n'
      '    }\n'
      '#endif\n'
      '}\n')
patch('src/video_core/shader/shader_jit.cpp',
      'void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {\n'
      '    ASSERT(setup.cached_shader != nullptr);\n',
      'void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (setup.cached_shader == nullptr) { // EmuSuNi: no block of JIT memory for it (SetupBatch)\n'
      '        emusuni_interpreter.Run(setup, state);\n'
      '        return;\n'
      '    }\n'
      '#else\n'
      '    ASSERT(setup.cached_shader != nullptr);\n'
      '#endif\n')

# --- The libretro glue: what the emulation throws (the JITs\' memory, above) ends the game, never the
# app. Upstream caught nothing: the exception left retro_run or context_reset and terminated.
patch('src/citra_libretro/citra_libretro.cpp',
      '#include <list>\n#include <numeric>\n#include <vector>\n',
      '#include <list>\n#include <numeric>\n#include <vector>\n'
      '#ifdef EMUSUNI_PORT\n#include <exception>\n#include <string>\n#include "common/emusuni_jit.h"\n#endif\n')
# The JITs' memory (common/emusuni_jit.cpp) back to the system when the core shuts down: the emulator is
# gone, so nothing of it is in use (it says so and keeps the mapping if something is).
patch('src/citra_libretro/citra_libretro.cpp',
      '    delete emu_instance;\n\n    Common::Log::Stop();\n}\n',
      '    delete emu_instance;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::JitRelease(); // EmuSuNi: the JIT memory back to the system (the next game maps it again)\n'
      '#endif\n\n'
      '    Common::Log::Stop();\n}\n')
patch('src/citra_libretro/citra_libretro.cpp',
      'static bool do_load_game() {\n'
      '    const Core::System::ResultStatus load_result{\n'
      '        Core::System::GetInstance().Load(*emu_instance->emu_window, LibRetro::settings.file_path)};\n\n'
      '    switch (load_result) {\n',
      'static bool do_load_game() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: what the load throws (no executable memory for the CPU JIT: core/arm/dynarmic) fails\n'
      '    // the game with a message; upstream let it end the app.\n'
      '    Core::System::ResultStatus load_result{};\n'
      '    try {\n'
      '        load_result =\n'
      '            Core::System::GetInstance().Load(*emu_instance->emu_window, LibRetro::settings.file_path);\n'
      '    } catch (const std::exception& error) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: loading the game threw: {}", error.what());\n'
      '        // (A message of the port\'s own, in lines, is the player\'s as it is: no JIT memory, core/core.cpp.)\n'
      '        const std::string what = error.what();\n'
      '        LibRetro::DisplayMessage(what.find(\'\\n\') != std::string::npos\n'
      '                                     ? what.c_str()\n'
      '                                     : ("Azahar could not start the game: " + what).c_str());\n'
      '        return false;\n'
      '    }\n'
      '#else\n'
      '    const Core::System::ResultStatus load_result{\n'
      '        Core::System::GetInstance().Load(*emu_instance->emu_window, LibRetro::settings.file_path)};\n'
      '#endif\n\n'
      '    switch (load_result) {\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    if (!emu_instance->game_loaded) {\n'
      '        emu_instance->game_loaded = do_load_game();\n'
      '    } else {\n',
      '    if (!emu_instance->game_loaded) {\n'
      '        emu_instance->game_loaded = do_load_game();\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (!emu_instance->game_loaded) {\n'
      '            LibRetro::Shutdown(); // EmuSuNi: back to the menu with the message, not a black screen\n'
      '        }\n'
      '#endif\n'
      '    } else {\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    while (!emu_instance->emu_window->HasSubmittedFrame()) {\n'
      '        auto result = Core::System::GetInstance().RunLoop();\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '#endif\n'
      '    while (!emu_instance->emu_window->HasSubmittedFrame()) {\n'
      '        auto result = Core::System::GetInstance().RunLoop();\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '            LibRetro::DisplayMessage(msg.c_str());\n'
      '        }\n'
      '    }\n'
      '}\n\n'
      'static void setup_memory_maps() {\n',
      '            LibRetro::DisplayMessage(msg.c_str());\n'
      '        }\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    } catch (const std::exception& error) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: the emulation threw: {}", error.what());\n'
      '        LibRetro::DisplayMessage((std::string("Azahar stopped: ") + error.what()).c_str());\n'
      '        emu_instance->game_loaded = false; // (empty frames until the frontend stops the game)\n'
      '        LibRetro::Shutdown();\n'
      '    }\n'
      '#endif\n'
      '}\n\n'
      'static void setup_memory_maps() {\n')

# The Vulkan pipeline cache reads through its shader cache object, which only exists once the disk
# resources were loaded: never with the disk cache option off, nor after a save state's load made
# the renderer again (upstream's own note at SwitchDiskCache). It is made on first use instead (the
# disk cache object honours the option itself).
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    /// Switches the disk cache at runtime to use a different title ID\n'
      '    void SwitchDiskCache(u64 title_id, const std::atomic_bool& stop_loading,\n'
      '                         const VideoCore::DiskResourceLoadCallback& callback);\n',
      '    /// Switches the disk cache at runtime to use a different title ID\n'
      '    void SwitchDiskCache(u64 title_id, const std::atomic_bool& stop_loading,\n'
      '                         const VideoCore::DiskResourceLoadCallback& callback);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: the shader cache object, made on first use when none was loaded\n'
      '    const std::shared_ptr<ShaderDiskCache>& EmuSuNiDiskCache();\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'bool PipelineCache::UseProgrammableVertexShader(const Pica::RegsInternal& regs,\n',
      '#ifdef EMUSUNI_PORT\n'
      'const std::shared_ptr<ShaderDiskCache>& PipelineCache::EmuSuNiDiskCache() {\n'
      '    if (!curr_disk_cache) {\n'
      '        // EmuSuNi: no disk resources were loaded (the option off, or the renderer made again by a save\n'
      '        // state\'s load): the cache object for the program anyway (it reads no file when the option is\n'
      '        // off). Upstream read through the null pointer.\n'
      '        LOG_WARNING(Render_Vulkan, "EmuSuNi: no shader cache loaded for title {:016X}: made now", GetProgramID());\n'
      '        LoadDiskCache();\n'
      '    }\n'
      '    return curr_disk_cache;\n'
      '}\n'
      '#endif\n\n'
      'bool PipelineCache::UseProgrammableVertexShader(const Pica::RegsInternal& regs,\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    GraphicsPipeline* const pipeline = curr_disk_cache->GetPipeline(info);\n',
      '    GraphicsPipeline* const pipeline = EMUSUNI_DISK_CACHE->GetPipeline(info);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    auto res = curr_disk_cache->UseProgrammableVertexShader(regs, setup, layout);\n',
      '    auto res = EMUSUNI_DISK_CACHE->UseProgrammableVertexShader(regs, setup, layout);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    auto res = curr_disk_cache->UseFixedGeometryShader(regs);\n',
      '    auto res = EMUSUNI_DISK_CACHE->UseFixedGeometryShader(regs);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    auto res = curr_disk_cache->UseFragmentShader(regs, user);\n',
      '    auto res = EMUSUNI_DISK_CACHE->UseFragmentShader(regs, user);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '#include "video_core/renderer_vulkan/vk_pipeline_cache.h"\n',
      '#include "video_core/renderer_vulkan/vk_pipeline_cache.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#define EMUSUNI_DISK_CACHE EmuSuNiDiskCache()\n'
      '#else\n'
      '#define EMUSUNI_DISK_CACHE curr_disk_cache\n'
      '#endif\n')

# --- Save states: serialised into one vector and compressed from it by frames; read back in place.
# Upstream's string stream, its copy and the one-shot compression peaked at ~5 x the emulated RAM on
# the heap (1.2 GiB measured on a New 3DS), which the PS4's heap hasn't; the load copied the state
# into a string and a string stream (3 x).
patch('src/common/zstd_compression.h',
      '[[nodiscard]] std::vector<u8> DecompressDataZSTD(std::span<const u8> compressed);\n',
      '[[nodiscard]] std::vector<u8> DecompressDataZSTD(std::span<const u8> compressed);\n'
      '\n#ifdef EMUSUNI_PORT\n'
      '/**\n'
      ' * EmuSuNi: compresses source by frames of 1 MiB, appending to out (no buffer the size of the\n'
      ' * input); the frame carries the content size, as CompressDataZSTD\'s does.\n'
      ' */\n'
      'bool CompressDataZSTDStreaming(std::span<const u8> source, std::vector<u8>& out);\n'
      '#endif\n')
patch('src/common/zstd_compression.cpp',
      'std::vector<u8> CompressDataZSTDDefault(std::span<const u8> source) {\n'
      '    return CompressDataZSTD(source, ZSTD_CLEVEL_DEFAULT);\n}\n',
      'std::vector<u8> CompressDataZSTDDefault(std::span<const u8> source) {\n'
      '    return CompressDataZSTD(source, ZSTD_CLEVEL_DEFAULT);\n}\n'
      '\n#ifdef EMUSUNI_PORT\n'
      'bool CompressDataZSTDStreaming(std::span<const u8> source, std::vector<u8>& out) {\n'
      '    ZSTD_CStream* stream = ZSTD_createCStream();\n'
      '    if (stream == nullptr) {\n'
      '        return false;\n'
      '    }\n'
      '    ZSTD_CCtx_setParameter(stream, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);\n'
      '    ZSTD_CCtx_setPledgedSrcSize(stream, source.size()); // the content size in the frame header\n'
      '    std::vector<u8> buffer(ZSTD_CStreamOutSize());\n'
      '    std::size_t pos = 0;\n'
      '    bool ok = true;\n'
      '    while (ok) {\n'
      '        const std::size_t chunk = std::min<std::size_t>(source.size() - pos, 1u << 20);\n'
      '        ZSTD_inBuffer in{source.data() + pos, chunk, 0};\n'
      '        const ZSTD_EndDirective mode = pos + chunk == source.size() ? ZSTD_e_end : ZSTD_e_continue;\n'
      '        std::size_t remaining = 0;\n'
      '        do {\n'
      '            ZSTD_outBuffer o{buffer.data(), buffer.size(), 0};\n'
      '            remaining = ZSTD_compressStream2(stream, &o, &in, mode);\n'
      '            if (ZSTD_isError(remaining)) {\n'
      '                LOG_ERROR(Common, "Error compressing ZSTD data: {}", ZSTD_getErrorName(remaining));\n'
      '                ok = false;\n'
      '                break;\n'
      '            }\n'
      '            out.insert(out.end(), buffer.data(), buffer.data() + o.pos);\n'
      '        } while (mode == ZSTD_e_end ? remaining != 0 : in.pos < in.size);\n'
      '        pos += chunk;\n'
      '        if (mode == ZSTD_e_end) {\n'
      '            break;\n'
      '        }\n'
      '    }\n'
      '    ZSTD_freeCStream(stream);\n'
      '    return ok;\n'
      '}\n'
      '#endif\n')
patch('src/core/savestate.cpp',
      '#include "network/network.h"\n',
      '#include "network/network.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <span>\n#include <streambuf>\n'
      '#include "common/settings.h"\n'
      '#include "core/memory.h"\n\n'
      'namespace EmuSuNi {\n'
      '// The archive written straight into a vector (room reserved first).\n'
      'class VectorStreamBuf : public std::streambuf {\n'
      'public:\n'
      '    void Reserve(std::size_t size) {\n'
      '        data.reserve(size);\n'
      '    }\n'
      '    std::span<const u8> Data() const {\n'
      '        return data;\n'
      '    }\n\n'
      'protected:\n'
      '    std::streamsize xsputn(const char* s, std::streamsize n) override {\n'
      '        data.insert(data.end(), s, s + n);\n'
      '        return n;\n'
      '    }\n'
      '    int overflow(int c) override {\n'
      '        if (c != traits_type::eof()) {\n'
      '            data.push_back(static_cast<u8>(c));\n'
      '        }\n'
      '        return c;\n'
      '    }\n\n'
      'private:\n'
      '    std::vector<u8> data;\n'
      '};\n\n'
      '// The archive read in place.\n'
      'class SpanStreamBuf : public std::streambuf {\n'
      'public:\n'
      '    explicit SpanStreamBuf(std::vector<u8>& v) {\n'
      '        char* begin = reinterpret_cast<char*>(v.data());\n'
      '        setg(begin, begin, begin + v.size());\n'
      '    }\n'
      '};\n'
      '} // namespace EmuSuNi\n'
      '#endif\n')
patch('src/core/savestate.cpp',
      'std::vector<u8> System::SaveStateBuffer() const {\n'
      '    std::ostringstream sstream{std::ios_base::binary};\n'
      '    // Serialize\n'
      '    oarchive oa{sstream};\n'
      '    oa&* this;\n\n'
      '    const std::string& str{sstream.str()};\n'
      '    const auto data = std::span<const u8>{reinterpret_cast<const u8*>(str.data()), str.size()};\n'
      '    auto buffer = Common::Compression::CompressDataZSTDDefault(data);\n\n'
      '    CSTHeader header{};\n',
      'std::vector<u8> System::SaveStateBuffer() const {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: into one vector, with room for the emulated RAM reserved (no string copy), then\n'
      '    // compressed from it by frames behind the header.\n'
      '    EmuSuNi::VectorStreamBuf buf;\n'
      '    buf.Reserve((Settings::values.is_new_3ds.GetValue() ? Memory::FCRAM_N3DS_SIZE : Memory::FCRAM_SIZE) +\n'
      '                Memory::VRAM_SIZE + Memory::N3DS_EXTRA_RAM_SIZE + (32u << 20));\n'
      '    {\n'
      '        std::ostream stream{&buf};\n'
      '        oarchive oa{stream};\n'
      '        oa&* this;\n'
      '    }\n'
      '    const auto data = buf.Data();\n'
      '    std::vector<u8> buffer; // (the compressed state goes behind the header: see below)\n'
      '#else\n'
      '    std::ostringstream sstream{std::ios_base::binary};\n'
      '    // Serialize\n'
      '    oarchive oa{sstream};\n'
      '    oa&* this;\n\n'
      '    const std::string& str{sstream.str()};\n'
      '    const auto data = std::span<const u8>{reinterpret_cast<const u8*>(str.data()), str.size()};\n'
      '    auto buffer = Common::Compression::CompressDataZSTDDefault(data);\n'
      '#endif\n\n'
      '    CSTHeader header{};\n')
patch('src/core/savestate.cpp',
      '    std::vector<u8> result((u8*)&header, (u8*)&header + sizeof(header));\n'
      '    std::copy(buffer.begin(), buffer.end(), std::back_inserter(result));\n\n'
      '    return result;\n',
      '    std::vector<u8> result((u8*)&header, (u8*)&header + sizeof(header));\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (!Common::Compression::CompressDataZSTDStreaming(data, result)) {\n'
      '        throw std::runtime_error("Could not compress the save state");\n'
      '    }\n'
      '#else\n'
      '    std::copy(buffer.begin(), buffer.end(), std::back_inserter(result));\n'
      '#endif\n\n'
      '    return result;\n')
patch('src/core/savestate.cpp',
      '    std::vector<u8> state(buffer.begin() + sizeof(CSTHeader), buffer.end());\n'
      '    auto decompressed = Common::Compression::DecompressDataZSTD(state);\n\n'
      '    std::istringstream sstream{\n'
      '        std::string{reinterpret_cast<char*>(decompressed.data()), decompressed.size()},\n'
      '        std::ios_base::binary};\n'
      '    decompressed.clear();\n\n'
      '    // Deserialize\n'
      '    iarchive ia{sstream};\n'
      '    ia&* this;\n\n'
      '    return true;\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: decompressed once from the buffer itself and read in place (upstream copied the\n'
      '    // compressed state, then the decompressed one into a string and a string stream).\n'
      '    auto decompressed = Common::Compression::DecompressDataZSTD(\n'
      '        std::span<const u8>(buffer.data() + sizeof(CSTHeader), buffer.size() - sizeof(CSTHeader)));\n'
      '    if (decompressed.empty()) {\n'
      '        LOG_ERROR(Core, "Save state could not be decompressed");\n'
      '        return false;\n'
      '    }\n'
      '    buffer.clear();\n'
      '    buffer.shrink_to_fit();\n'
      '    EmuSuNi::SpanStreamBuf buf{decompressed};\n'
      '    std::istream stream{&buf};\n\n'
      '    // Deserialize\n'
      '    iarchive ia{stream};\n'
      '    ia&* this;\n\n'
      '    return true;\n'
      '}\n'
      '#else\n'
      '    std::vector<u8> state(buffer.begin() + sizeof(CSTHeader), buffer.end());\n'
      '    auto decompressed = Common::Compression::DecompressDataZSTD(state);\n\n'
      '    std::istringstream sstream{\n'
      '        std::string{reinterpret_cast<char*>(decompressed.data()), decompressed.size()},\n'
      '        std::ios_base::binary};\n'
      '    decompressed.clear();\n\n'
      '    // Deserialize\n'
      '    iarchive ia{sstream};\n'
      '    ia&* this;\n\n'
      '    return true;\n'
      '}\n'
      '#endif\n')

# --- Threads: the app names and places them (emusuni_3ds_thread, port/); the unnamed ones get names.
patch('src/common/thread.cpp',
      '#ifdef __FreeBSD__\n#define cpu_set_t cpuset_t\n#endif\n\nnamespace Common {\n',
      '#ifdef __FreeBSD__\n#define cpu_set_t cpuset_t\n#endif\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (the app, port/): called first thing in every thread Azahar names ("VulkanWorker",\n'
      '// "Pipeline workers", "Shader workers", "citra:Log", "VulkanFence"...): its core, priority and name.\n'
      'extern "C" void emusuni_3ds_thread(const char* name);\n'
      '#endif\n\n'
      'namespace Common {\n')
patch('src/common/thread.cpp',
      'void SetCurrentThreadPriority(ThreadPriority new_priority) {\n    pthread_t this_thread = pthread_self();\n',
      'void SetCurrentThreadPriority(ThreadPriority new_priority) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    (void)new_priority;\n'
      '    return; // EmuSuNi: the hook sets the priorities (the PS4 runs threads by fixed priorities)\n'
      '#endif\n'
      '    pthread_t this_thread = pthread_self();\n')
patch('src/common/thread.cpp',
      '#if !defined(_WIN32) || defined(_MSC_VER)\nvoid SetCurrentThreadName(const char* name) {\n#ifdef __APPLE__\n',
      '#if !defined(_WIN32) || defined(_MSC_VER)\nvoid SetCurrentThreadName(const char* name) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_3ds_thread(name);\n'
      '    return;\n'
      '#endif\n'
      '#ifdef __APPLE__\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: 2 pipeline and 2 shader compile workers (upstream: half the CPUs each; the PS4\'s cores\n'
      '// 1, 4 and 5 are shared with the sound and the app).\n'
      'static u32 EmuSuNiWorkerThreads() {\n    return 2;\n}\n'
      '#else\n'
      'static u32 EmuSuNiWorkerThreads() {\n    return std::max(std::thread::hardware_concurrency(), 2U) / 2;\n}\n'
      '#endif\n\n'
      'PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '      num_worker_threads{std::max(std::thread::hardware_concurrency(), 2U) / 2},\n',
      '      num_worker_threads{EmuSuNiWorkerThreads()},\n')
patch('src/core/hle/kernel/hle_ipc.h',
      '#include "common/thread_worker.h"\n',
      '#include "common/thread_worker.h"\n#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/kernel/hle_ipc.h',
      '                    std::move(std::async(std::launch::async, [this, async_section] {\n'
      '                        s64 sleep_for = async_section(*this);\n',
      '                    std::move(std::async(std::launch::async, [this, async_section] {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                        Common::SetCurrentThreadName("HleAsync"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '                        s64 sleep_for = async_section(*this);\n')
patch('src/common/detached_tasks.cpp',
      '#include "common/detached_tasks.h"\n',
      '#include "common/detached_tasks.h"\n#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/common/detached_tasks.cpp',
      '    std::thread([task{std::move(task)}]() {\n        task();\n',
      '    std::thread([task{std::move(task)}]() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        Common::SetCurrentThreadName("Detached"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '        task();\n')
# The threads upstream starts without a name (none of them spins, but on the PS4 an unplaced thread
# runs at the default priority on any core): the DSP LLE's own thread (the "lle_multithread" audio
# option: the emulation thread waits for it every slice), Download Play's connection managers, the
# AM title scans, the camera capture and the HTTP requests.
patch('src/audio_core/lle/lle.cpp',
      '    void TeakraThread() {\n        while (true) {\n',
      '    void TeakraThread() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        Common::SetCurrentThreadName("Teakra"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '        while (true) {\n')
patch('src/core/hle/service/dlp/dlp_srvr.cpp',
      '#include "common/archives.h"\n#include "common/common_types.h"\n',
      '#include "common/archives.h"\n#include "common/common_types.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/dlp/dlp_srvr.cpp',
      'void DLP_SRVR::ServerConnectionManager() {\n    auto uds = GetUDS();\n',
      'void DLP_SRVR::ServerConnectionManager() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("DlpServer"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '    auto uds = GetUDS();\n')
patch('src/core/hle/service/dlp/dlp_clt_base.cpp',
      '#include "common/alignment.h"\n#include "common/string_util.h"\n',
      '#include "common/alignment.h"\n#include "common/string_util.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/dlp/dlp_clt_base.cpp',
      'void DLP_Clt_Base::ClientConnectionManager() {\n    auto uds = GetUDS();\n',
      'void DLP_Clt_Base::ClientConnectionManager() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("DlpClient"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '    auto uds = GetUDS();\n')
patch('src/core/hle/service/am/am.cpp',
      '#include "common/alignment.h"\n#include "common/archives.h"\n',
      '#include "common/alignment.h"\n#include "common/archives.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
# (std::launch::async: with the default policy the task may run deferred, on the thread that waits for
# it, the emulation thread, which the hook must never re-place.)
for future, capture in (('scan_tickets_future', '[this]'), ('scan_titles_future', '[this, media_type]'),
                        ('scan_all_future', '[this]')):
    patch('src/core/hle/service/am/am.cpp',
          '        %s = std::async(%s() {\n            std::scoped_lock lock(am_lists_mutex);\n' % (future, capture),
          '#ifdef EMUSUNI_PORT\n'
          '        // EmuSuNi: a thread of its own (never deferred onto the emulation thread), placed by the app.\n'
          '        %s = std::async(std::launch::async, %s() {\n' % (future, capture) +
          '            Common::SetCurrentThreadName("AmScan");\n'
          '#else\n'
          '        %s = std::async(%s() {\n' % (future, capture) +
          '#endif\n'
          '            std::scoped_lock lock(am_lists_mutex);\n')
patch('src/core/hle/service/cam/cam.cpp',
      '#include "common/archives.h"\n#include "common/bit_set.h"\n',
      '#include "common/archives.h"\n#include "common/bit_set.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/cam/cam.cpp',
      '    port.capture_result = std::async(std::launch::async, [&camera, &port, this] {\n',
      '    port.capture_result = std::async(std::launch::async, [&camera, &port, this] {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        Common::SetCurrentThreadName("CamCapture"); // EmuSuNi: the app places it\n'
      '#endif\n')
patch('src/core/hle/service/http/http_c.cpp',
      '#include "common/archives.h"\n#include "common/assert.h"\n',
      '#include "common/archives.h"\n#include "common/assert.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/http/http_c.cpp',
      'void Context::MakeRequest() {\n    ASSERT(state == RequestState::NotStarted);\n',
      'void Context::MakeRequest() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("HttpRequest"); // EmuSuNi: the app places it (always a std::async task)\n'
      '#endif\n'
      '    ASSERT(state == RequestState::NotStarted);\n')

# --- Vulkan: the frontend's device adopted as it was made (EMUSUNI_VK_DEVICE_EXTENSIONS and
# EMUSUNI_VK_DEVICE_FEATURES), the idle waits under the frontend's lock, the frame images a ring of
# the core's own.
patch('src/citra_libretro/libretro_vk.cpp',
      '#include "common/settings.h"\n',
      '#include "common/settings.h"\n'
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <cstring>\n#include "common/thread.h"\n#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '} // namespace LibRetro\n\nnamespace Vulkan {\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the queue is the frontend\'s (its menus draw on it too): every wait for it under its lock.\n'
      'void LockQueue() {\n'
      '    if (vulkan_intf && vulkan_intf->lock_queue) {\n'
      '        vulkan_intf->lock_queue(vulkan_intf->handle);\n'
      '    }\n'
      '}\n\n'
      'void UnlockQueue() {\n'
      '    if (vulkan_intf && vulkan_intf->unlock_queue) {\n'
      '        vulkan_intf->unlock_queue(vulkan_intf->handle);\n'
      '    }\n'
      '}\n\n'
      '// EmuSuNi: what the frontend made its device with (src/Display.cpp): EMUSUNI_VK_DEVICE_EXTENSIONS,\n'
      '// comma-separated; none when unset.\n'
      'std::vector<std::string> EnabledDeviceExtensions() {\n'
      '    std::vector<std::string> list;\n'
      '    const char* env = std::getenv("EMUSUNI_VK_DEVICE_EXTENSIONS");\n'
      '    if (env == nullptr) {\n'
      '        return list;\n'
      '    }\n'
      '    for (const char* at = env; *at != 0;) {\n'
      '        const char* end = std::strchr(at, \',\');\n'
      '        const std::string name = end ? std::string(at, end) : std::string(at);\n'
      '        if (!name.empty()) {\n'
      '            list.push_back(name);\n'
      '        }\n'
      '        at = end ? end + 1 : at + name.size();\n'
      '    }\n'
      '    return list;\n'
      '}\n'
      '#endif\n\n'
      '} // namespace LibRetro\n\nnamespace Vulkan {\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    const std::vector extensions = physical_device.enumerateDeviceExtensionProperties();\n'
      '    available_extensions.reserve(extensions.size());\n'
      '    for (const auto& extension : extensions) {\n'
      '        available_extensions.emplace_back(extension.extensionName.data());\n'
      '    }\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the device was made by the frontend with these (not everything the GPU has).\n'
      '    available_extensions = LibRetro::EnabledDeviceExtensions();\n'
      '    std::string emusuni_listed;\n'
      '    for (const auto& name : available_extensions) {\n'
      '        emusuni_listed += emusuni_listed.empty() ? name : " " + name;\n'
      '    }\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: the frontend\'s device has {} extensions enabled: {}",\n'
      '             available_extensions.size(), emusuni_listed);\n'
      '    available_extensions.emplace_back("EmuSuNi"); // (never empty: CreateDevice takes that as no device)\n'
      '#else\n'
      '    const std::vector extensions = physical_device.enumerateDeviceExtensionProperties();\n'
      '    available_extensions.reserve(extensions.size());\n'
      '    for (const auto& extension : extensions) {\n'
      '        available_extensions.emplace_back(extension.extensionName.data());\n'
      '    }\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    // Wait for any pending operations\n    WaitPresent();\n    device.waitIdle();\n',
      '    // Wait for any pending operations\n    WaitPresent();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    LibRetro::LockQueue(); // EmuSuNi: the menus\' queue too\n'
      '    device.waitIdle();\n'
      '    LibRetro::UnlockQueue();\n'
      '#else\n'
      '    device.waitIdle();\n'
      '#endif\n')
# The render pass that writes the frame images: it waits for the fragment shader reads of every
# earlier submission (the frontend may still sample an older image of the ring when the core writes
# it again) and makes its writes visible to the frontend's sampling (upstream's implicit end
# dependency doesn't).
patch('src/citra_libretro/libretro_vk.cpp',
      '    const vk::RenderPassCreateInfo renderpass_info = {\n'
      '        .attachmentCount = 1,\n'
      '        .pAttachments = &color_attachment,\n'
      '        .subpassCount = 1,\n'
      '        .pSubpasses = &subpass,\n'
      '        .dependencyCount = 1,\n'
      '        .pDependencies = &dependency,\n'
      '    };\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: wait for the frontend\'s sampling of the image (an earlier frame of the ring), and\n'
      '    // make the writes visible to its next sampling.\n'
      '    const std::array<vk::SubpassDependency, 2> emusuni_dependencies = {\n'
      '        vk::SubpassDependency{\n'
      '            .srcSubpass = VK_SUBPASS_EXTERNAL,\n'
      '            .dstSubpass = 0,\n'
      '            .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader |\n'
      '                            vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '            .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '            .srcAccessMask = {},\n'
      '            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,\n'
      '        },\n'
      '        vk::SubpassDependency{\n'
      '            .srcSubpass = 0,\n'
      '            .dstSubpass = VK_SUBPASS_EXTERNAL,\n'
      '            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '            .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,\n'
      '            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,\n'
      '            .dstAccessMask = vk::AccessFlagBits::eShaderRead,\n'
      '        },\n'
      '    };\n'
      '    const vk::RenderPassCreateInfo renderpass_info = {\n'
      '        .attachmentCount = 1,\n'
      '        .pAttachments = &color_attachment,\n'
      '        .subpassCount = 1,\n'
      '        .pSubpasses = &subpass,\n'
      '        .dependencyCount = static_cast<u32>(emusuni_dependencies.size()),\n'
      '        .pDependencies = emusuni_dependencies.data(),\n'
      '    };\n'
      '#else\n'
      '    const vk::RenderPassCreateInfo renderpass_info = {\n'
      '        .attachmentCount = 1,\n'
      '        .pAttachments = &color_attachment,\n'
      '        .subpassCount = 1,\n'
      '        .pSubpasses = &subpass,\n'
      '        .dependencyCount = 1,\n'
      '        .pDependencies = &dependency,\n'
      '    };\n'
      '#endif\n')
# The frame images: a ring of 3 of the core's own (upstream: 2 slots sharing one image), one step per
# frame presented, never the frontend's sync index (the frontend may call retro_run several times
# between two of its frames, or draw on another thread meanwhile).
patch('src/citra_libretro/libretro_vk.cpp',
      '    const u32 frame_count = 2; // Double buffering for LibRetro\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const u32 frame_count = EMUSUNI_IMAGES; // EmuSuNi: a ring of images of the core\'s own\n'
      '#else\n'
      '    const u32 frame_count = 2; // Double buffering for LibRetro\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '        frame.image = output_image; // All frames use the same output texture\n'
      '        frame.image_view = output_image_view;\n'
      '        frame.allocation = {}; // VMA allocation handled separately\n'
      '        frame.cmdbuf = command_buffers[i];\n',
      '#ifdef EMUSUNI_PORT\n'
      '        if (i == 0) {\n'
      '            frame.image = output_image;\n'
      '            frame.image_view = output_image_view;\n'
      '            frame.allocation = {}; // (the output texture\'s, destroyed with it)\n'
      '        } else {\n'
      '            // EmuSuNi: an image of this frame\'s own, like the output texture.\n'
      '            vk::ImageCreateInfo image_info = {\n'
      '                .imageType = vk::ImageType::e2D,\n'
      '                .format = output_format,\n'
      '                .extent = {output_width, output_height, 1},\n'
      '                .mipLevels = 1,\n'
      '                .arrayLayers = 1,\n'
      '                .samples = vk::SampleCountFlagBits::e1,\n'
      '                .tiling = vk::ImageTiling::eOptimal,\n'
      '                .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc |\n'
      '                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,\n'
      '                .sharingMode = vk::SharingMode::eExclusive,\n'
      '                .initialLayout = vk::ImageLayout::eUndefined,\n'
      '            };\n'
      '            VmaAllocationCreateInfo alloc_create = {};\n'
      '            alloc_create.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;\n'
      '            VkImage vk_image;\n'
      '            const VkResult result = vmaCreateImage(instance.GetAllocator(),\n'
      '                                                   reinterpret_cast<const VkImageCreateInfo*>(&image_info),\n'
      '                                                   &alloc_create, &vk_image, &frame.allocation, nullptr);\n'
      '            if (result != VK_SUCCESS) {\n'
      '                LOG_CRITICAL(Render_Vulkan, "EmuSuNi: failed to create frame image {}: {}", i, static_cast<int>(result));\n'
      '                throw std::runtime_error("Failed to create LibRetro frame image");\n'
      '            }\n'
      '            frame.image = vk::Image{vk_image};\n'
      '            vk::ImageViewCreateInfo view_info = output_view_create_info;\n'
      '            view_info.image = frame.image;\n'
      '            frame.image_view = device.createImageView(view_info);\n'
      '        }\n'
      '#else\n'
      '        frame.image = output_image; // All frames use the same output texture\n'
      '        frame.image_view = output_image_view;\n'
      '        frame.allocation = {}; // VMA allocation handled separately\n'
      '#endif\n'
      '        frame.cmdbuf = command_buffers[i];\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '            .attachmentCount = 1,\n            .pAttachments = &output_image_view,\n            .width = output_width,\n',
      '            .attachmentCount = 1,\n'
      '#ifdef EMUSUNI_PORT\n'
      '            .pAttachments = &frame.image_view, // EmuSuNi: this frame\'s image\n'
      '#else\n'
      '            .pAttachments = &output_image_view,\n'
      '#endif\n'
      '            .width = output_width,\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    for (auto& frame : frame_pool) {\n        if (frame.framebuffer) {\n            device.destroyFramebuffer(frame.framebuffer);\n        }\n',
      '    for (auto& frame : frame_pool) {\n        if (frame.framebuffer) {\n            device.destroyFramebuffer(frame.framebuffer);\n        }\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (frame.allocation) { // EmuSuNi: an image of this frame\'s own (not the output texture)\n'
      '            device.destroyImageView(frame.image_view);\n'
      '            vmaDestroyImage(instance.GetAllocator(), static_cast<VkImage>(frame.image), frame.allocation);\n'
      '            frame.allocation = {};\n'
      '        }\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    // LibRetro synchronization: Use LibRetro\'s wait mechanism instead of fences\n'
      '    if (vulkan_intf && vulkan_intf->wait_sync_index && vulkan_intf->handle) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the next image of the ring (never the one the frontend shows now, set_image\'s).\n'
      '    current_frame_index = (current_frame_index + 1) % frame_pool.size();\n'
      '    return &frame_pool[current_frame_index];\n'
      '#endif\n'
      '    // LibRetro synchronization: Use LibRetro\'s wait mechanism instead of fences\n'
      '    if (vulkan_intf && vulkan_intf->wait_sync_index && vulkan_intf->handle) {\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    persistent_libretro_image.image_view = static_cast<VkImageView>(frame->image_view);\n'
      '    persistent_libretro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;\n'
      '    persistent_libretro_image.create_info =\n'
      '        static_cast<VkImageViewCreateInfo>(output_view_create_info);\n\n'
      '    vulkan_intf->set_image(vulkan_intf->handle, &persistent_libretro_image, 0, nullptr,\n'
      '                           instance.GetGraphicsQueueFamilyIndex());\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: one descriptor per image of the ring (the frontend keeps the pointer).\n'
      '    retro_vulkan_image& libretro_image = emusuni_images[frame - frame_pool.data()];\n'
      '    libretro_image.image_view = static_cast<VkImageView>(frame->image_view);\n'
      '    libretro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;\n'
      '    libretro_image.create_info = static_cast<VkImageViewCreateInfo>(output_view_create_info);\n'
      '    libretro_image.create_info.image = static_cast<VkImage>(frame->image);\n\n'
      '    vulkan_intf->set_image(vulkan_intf->handle, &libretro_image, 0, nullptr,\n'
      '                           instance.GetGraphicsQueueFamilyIndex());\n'
      '#else\n'
      '    persistent_libretro_image.image_view = static_cast<VkImageView>(frame->image_view);\n'
      '    persistent_libretro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;\n'
      '    persistent_libretro_image.create_info =\n'
      '        static_cast<VkImageViewCreateInfo>(output_view_create_info);\n\n'
      '    vulkan_intf->set_image(vulkan_intf->handle, &persistent_libretro_image, 0, nullptr,\n'
      '                           instance.GetGraphicsQueueFamilyIndex());\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.h',
      '    retro_vulkan_image persistent_libretro_image{};\n};\n',
      '    retro_vulkan_image persistent_libretro_image{};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the ring of frame images (scripts/build-3ds-core.sh), a descriptor each.\n'
      '    static constexpr u32 EMUSUNI_IMAGES = 3;\n'
      '    std::array<retro_vulkan_image, EMUSUNI_IMAGES> emusuni_images{};\n'
      '#endif\n'
      '};\n')
patch('src/citra_libretro/libretro_vk.h',
      '#include <memory>\n#include <variant>\n',
      '#include <array>\n#include <memory>\n#include <variant>\n')
# The fence wait thread, named for the app's hook.
patch('src/citra_libretro/libretro_vk.cpp',
      'void MasterSemaphoreLibRetro::WaitThread(std::stop_token token) {\n    const vk::Device device{instance.GetDevice()};\n',
      'void MasterSemaphoreLibRetro::WaitThread(std::stop_token token) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("VulkanFence"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '    const vk::Device device{instance.GetDevice()};\n')
# What the device was made with: the features of EMUSUNI_VK_DEVICE_FEATURES only (upstream took the
# physical device's).
patch('src/video_core/renderer_vulkan/vk_instance.cpp',
      '#include <vk_mem_alloc.h>\n',
      '#include <vk_mem_alloc.h>\n'
      r'''
#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
// EmuSuNi (scripts/build-3ds-core.sh): the frontend made the device (src/Display.cpp) with the features
// of EMUSUNI_VK_DEVICE_FEATURES (comma-separated VkPhysicalDeviceFeatures names, and the extension
// features' names); none when unset.
bool EmuSuNiFeatureListed(const char* name) {
    const char* list = std::getenv("EMUSUNI_VK_DEVICE_FEATURES");
    if (list == nullptr) {
        return false;
    }
    const size_t length = std::strlen(name);
    for (const char* at = std::strstr(list, name); at != nullptr; at = std::strstr(at + 1, name)) {
        if ((at == list || at[-1] == ',') && (at[length] == '\0' || at[length] == ',')) {
            return true;
        }
    }
    return false;
}

// VkPhysicalDeviceFeatures as Vulkan lays it out: 55 VkBool32 in this order.
constexpr std::array<const char*, 55> EMUSUNI_FEATURE_NAMES = {
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
static_assert(sizeof(VkPhysicalDeviceFeatures) == EMUSUNI_FEATURE_NAMES.size() * sizeof(VkBool32));

// Of what the GPU has, only what the device was made with.
void EmuSuNiMaskFeatures(vk::PhysicalDeviceFeatures& features) {
    VkBool32* flags = reinterpret_cast<VkBool32*>(&features);
    std::string enabled;
    for (size_t i = 0; i < EMUSUNI_FEATURE_NAMES.size(); i++) {
        if (flags[i] && !EmuSuNiFeatureListed(EMUSUNI_FEATURE_NAMES[i])) {
            flags[i] = VK_FALSE;
        } else if (flags[i]) {
            enabled += enabled.empty() ? EMUSUNI_FEATURE_NAMES[i] : std::string(" ") + EMUSUNI_FEATURE_NAMES[i];
        }
    }
    LOG_INFO(Render_Vulkan, "EmuSuNi: the frontend's device has these features enabled: {}", enabled);
}
} // namespace
#define EMUSUNI_3DS_FEATURE_LISTED(feature) && EmuSuNiFeatureListed(#feature)
#else
#define EMUSUNI_3DS_FEATURE_LISTED(feature)
#endif
''')
patch('src/video_core/renderer_vulkan/vk_instance.cpp',
      '    features = feature_chain.get().features;\n    if (available_extensions.empty()) {\n',
      '    features = feature_chain.get().features;\n'
      '#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)\n'
      '    EmuSuNiMaskFeatures(features); // EmuSuNi: only what the frontend\'s device was made with\n'
      '#endif\n'
      '    if (available_extensions.empty()) {\n')
patch('src/video_core/renderer_vulkan/vk_instance.cpp',
      'if (feature_chain.get<structName>().feature) {',
      'if (feature_chain.get<structName>().feature EMUSUNI_3DS_FEATURE_LISTED(feature)) {')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '#include "video_core/renderer_vulkan/vk_shader_util.h"\n',
      '#include "video_core/renderer_vulkan/vk_shader_util.h"\n'
      '#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)\n'
      'namespace LibRetro { // citra_libretro/libretro_vk.cpp: the frontend\'s queue lock\n'
      'void LockQueue();\n'
      'void UnlockQueue();\n'
      '} // namespace LibRetro\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '    main_present_window.WaitPresent();\n    device.waitIdle();\n\n    device.destroyShaderModule(present_vertex_shader);\n',
      '    main_present_window.WaitPresent();\n'
      '#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)\n'
      '    LibRetro::LockQueue(); // EmuSuNi: the menus\' queue too\n'
      '    device.waitIdle();\n'
      '    LibRetro::UnlockQueue();\n'
      '#else\n'
      '    device.waitIdle();\n'
      '#endif\n\n'
      '    device.destroyShaderModule(present_vertex_shader);\n')

# --- No SSL: the ssl:C service's random bytes and the AM ticket keys from the PS4's entropy; HTTPS
# requests refused at once.
addfile('src/common/emusuni_random.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): every random byte of the core. LibreSSL isn't built and
// Crypto++'s OS generator wanted arc4random_buf on a FreeBSD target (the PS4's libc hasn't it);
// libc++'s std::random_device opens /dev/urandom, which the PS4 may not have. Here: getentropy, else
// /dev/urandom, else a generator seeded from the clock (said once in the log).
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
#include "common/logging/log.h"

namespace {
std::mutex g_lock;
int g_source = 0; // 0: not tried yet, 1: getentropy, 2: /dev/urandom, 3: the clock
int g_fd = -1;
uint64_t g_state = 0;

uint64_t SplitMix() {
    uint64_t z = (g_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Choose() {
    unsigned char probe[16];
    if (getentropy(probe, sizeof(probe)) == 0) {
        g_source = 1;
        return;
    }
    g_fd = open("/dev/urandom", O_RDONLY);
    if (g_fd >= 0 && read(g_fd, probe, sizeof(probe)) == static_cast<ssize_t>(sizeof(probe))) {
        g_source = 2;
        return;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    g_state = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec) ^
              reinterpret_cast<uintptr_t>(&probe);
    g_source = 3;
    LOG_WARNING(Common, "EmuSuNi: no getentropy and no /dev/urandom: random bytes from a generator seeded by the clock");
}
} // namespace

extern "C" void emusuni_3ds_random_bytes(void* out, size_t size) {
    std::lock_guard guard{g_lock};
    if (g_source == 0) {
        Choose();
    }
    unsigned char* at = static_cast<unsigned char*>(out);
    while (size > 0) {
        size_t chunk = size < 256 ? size : 256;
        bool ok = false;
        if (g_source == 1) {
            ok = getentropy(at, chunk) == 0;
        } else if (g_source == 2) {
            const ssize_t got = read(g_fd, at, chunk);
            ok = got > 0;
            chunk = ok ? static_cast<size_t>(got) : 0;
        }
        if (!ok) {
            for (size_t i = 0; i < chunk; i += 8) {
                const uint64_t word = SplitMix();
                std::memcpy(at + i, &word, chunk - i < 8 ? chunk - i : 8);
            }
        }
        at += chunk;
        size -= chunk;
    }
}

extern "C" unsigned emusuni_3ds_random_seed(void) {
    unsigned seed = 0;
    emusuni_3ds_random_bytes(&seed, sizeof(seed));
    return seed;
}
#endif
''')
patch('src/core/hle/service/ssl/ssl_c.cpp',
      '#include <openssl/rand.h>\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" void emusuni_3ds_random_bytes(void* out, size_t size); // common/emusuni_random.cpp\n'
      '#else\n#include <openssl/rand.h>\n#endif\n')
patch('src/core/hle/service/ssl/ssl_c.cpp',
      '    RAND_bytes(out.data(), static_cast<int>(out.size()));\n',
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_3ds_random_bytes(out.data(), out.size()); // EmuSuNi: no LibreSSL\n'
      '#else\n'
      '    RAND_bytes(out.data(), static_cast<int>(out.size()));\n'
      '#endif\n')
patch('src/core/hle/service/am/am.cpp',
      '#include <openssl/rand.h>\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" void emusuni_3ds_random_bytes(void* out, size_t size); // common/emusuni_random.cpp\n'
      '#else\n#include <openssl/rand.h>\n#endif\n')
patch('src/core/hle/service/am/am.cpp',
      '    RAND_bytes(key.data(), static_cast<int>(key.size()));\n'
      '    RAND_bytes(iv.data(), static_cast<int>(iv.size()));\n',
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_3ds_random_bytes(key.data(), key.size()); // EmuSuNi: no LibreSSL\n'
      '    emusuni_3ds_random_bytes(iv.data(), iv.size());\n'
      '#else\n'
      '    RAND_bytes(key.data(), static_cast<int>(key.size()));\n'
      '    RAND_bytes(iv.data(), static_cast<int>(iv.size()));\n'
      '#endif\n')
patch('src/core/hle/service/http/http_c.cpp',
      'void Context::MakeRequestSSL(httplib::Request& request, const Common::URLInfo& url_info,\n'
      '                             std::vector<Context::RequestHeader>& pending_headers) {\n'
      '    httplib::Error error{-1};\n'
      '    X509* cert = nullptr;\n',
      'void Context::MakeRequestSSL(httplib::Request& request, const Common::URLInfo& url_info,\n'
      '                             std::vector<Context::RequestHeader>& pending_headers) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: built without LibreSSL: no HTTPS (the request fails as if the server were unreachable).\n'
      '    LOG_ERROR(Service_HTTP, "EmuSuNi: HTTPS request to {} refused: no SSL in this build", url_info.host);\n'
      '    state = RequestState::Completed;\n'
      '    FinishTransfer();\n'
      '}\n'
      '#else\n'
      '    httplib::Error error{-1};\n'
      '    X509* cert = nullptr;\n')
patch('src/core/hle/service/http/http_c.cpp',
      '    client->enable_server_certificate_verification(false);\n\n'
      '    client->set_header_writer(\n'
      '        [this, &pending_headers](httplib::Stream& strm, httplib::Headers& httplib_headers) {\n'
      '            return HandleHeaderWrite(pending_headers, strm, httplib_headers);\n'
      '        });\n\n'
      '    if (!client->send(request, response, error)) {\n'
      '        LOG_ERROR(Service_HTTP, "Request failed: {}: {}", error, httplib::to_string(error));\n'
      '        state = RequestState::Completed;\n'
      '    } else {\n'
      '        LOG_DEBUG(Service_HTTP, "Request successful");\n'
      '        state = RequestState::ReceivingBody;\n'
      '    }\n\n'
      '    FinishTransfer();\n'
      '}\n',
      '    client->enable_server_certificate_verification(false);\n\n'
      '    client->set_header_writer(\n'
      '        [this, &pending_headers](httplib::Stream& strm, httplib::Headers& httplib_headers) {\n'
      '            return HandleHeaderWrite(pending_headers, strm, httplib_headers);\n'
      '        });\n\n'
      '    if (!client->send(request, response, error)) {\n'
      '        LOG_ERROR(Service_HTTP, "Request failed: {}: {}", error, httplib::to_string(error));\n'
      '        state = RequestState::Completed;\n'
      '    } else {\n'
      '        LOG_DEBUG(Service_HTTP, "Request successful");\n'
      '        state = RequestState::ReceivingBody;\n'
      '    }\n\n'
      '    FinishTransfer();\n'
      '}\n'
      '#endif // !EMUSUNI_PORT\n')
# std::random_device: libc++'s opens /dev/urandom, which the PS4 may not have (it would throw); the
# three users (the core timing's jitter, the rooms, cpp-httplib's multipart boundary) seed from the
# chain above instead, and the end check wants none left.
patch('src/core/core_timing.cpp',
      '#include "core/core_timing.h"\n',
      '#include "core/core_timing.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" unsigned emusuni_3ds_random_seed(void); // common/emusuni_random.cpp\n'
      '#endif\n')
patch('src/core/core_timing.cpp',
      '    std::mt19937 random_gen(std::random_device{}());\n',
      '#ifdef EMUSUNI_PORT\n'
      '    std::mt19937 random_gen(emusuni_3ds_random_seed()); // EmuSuNi: the PS4\'s entropy (no /dev/urandom?)\n'
      '#else\n'
      '    std::mt19937 random_gen(std::random_device{}());\n'
      '#endif\n')
patch('externals/httplib/httplib.h',
      'inline std::string make_multipart_data_boundary() {\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" unsigned emusuni_3ds_random_seed(void); // EmuSuNi: src/common/emusuni_random.cpp\n'
      '#endif\n'
      'inline std::string make_multipart_data_boundary() {\n')
patch('externals/httplib/httplib.h',
      '  // std::random_device might actually be deterministic on some\n'
      '  // platforms, but due to lack of support in the c++ standard library,\n'
      '  // doing better requires either some ugly hacks or breaking portability.\n'
      '  std::random_device seed_gen;\n\n'
      '  // Request 128 bits of entropy for initialization\n'
      '  std::seed_seq seed_sequence{seed_gen(), seed_gen(), seed_gen(), seed_gen()};\n',
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi (scripts/build-3ds-core.sh): the PS4\'s entropy, not std::random_device (it would open\n'
      '  // /dev/urandom, which the PS4 may not have).\n'
      '  std::seed_seq seed_sequence{emusuni_3ds_random_seed(), emusuni_3ds_random_seed(),\n'
      '                              emusuni_3ds_random_seed(), emusuni_3ds_random_seed()};\n'
      '#else\n'
      '  // std::random_device might actually be deterministic on some\n'
      '  // platforms, but due to lack of support in the c++ standard library,\n'
      '  // doing better requires either some ugly hacks or breaking portability.\n'
      '  std::random_device seed_gen;\n\n'
      '  // Request 128 bits of entropy for initialization\n'
      '  std::seed_seq seed_sequence{seed_gen(), seed_gen(), seed_gen(), seed_gen()};\n'
      '#endif\n')
patch('src/network/room.cpp',
      '#include "network/verify_user.h"\n',
      '#include "network/verify_user.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" unsigned emusuni_3ds_random_seed(void); // common/emusuni_random.cpp\n'
      '#define EMUSUNI_3DS_RANDOM_SEED emusuni_3ds_random_seed()\n'
      '#else\n'
      '#define EMUSUNI_3DS_RANDOM_SEED std::random_device()()\n'
      '#endif\n')
patch('src/network/room.cpp',
      '        : NintendoOUI{0x00, 0x1F, 0x32, 0x00, 0x00, 0x00}, random_gen(std::random_device()()) {}\n',
      '        : NintendoOUI{0x00, 0x1F, 0x32, 0x00, 0x00, 0x00}, random_gen(EMUSUNI_3DS_RANDOM_SEED) {}\n')
# Crypto++'s CPU detection includes FreeBSD's ELF auxiliary vector headers (only ARM needs them; the
# PS4 toolchain hasn't sys/elf_common.h and its getauxval clashes with Crypto++'s own stub).
patch('externals/cryptopp/src/core/cpu.cpp',
      '#if defined(__FreeBSD__)\n# include <sys/auxv.h>\n# include <sys/elf_common.h>\n#endif\n',
      '#if defined(__FreeBSD__) && !defined(EMUSUNI_PORT)  // EmuSuNi: not on the PS4 (x86-64 uses CPUID)\n'
      '# include <sys/auxv.h>\n# include <sys/elf_common.h>\n#endif\n')
# Crypto++ probes the CPU when the program starts (a namespace-scope object: DetectX86Features), before any
# game, for every console the app plays - and its CpuId does it under a temporary SIGILL handler (signal,
# sigprocmask, setjmp), which then puts back the handler it found as a plain one: the app's crash log
# installs its SIGILL handler with SA_SIGINFO, and would be called without its siginfo and context after
# that. x86-64 always has CPUID: the instruction alone.
patch('externals/cryptopp/src/core/cpu.cpp',
      '\tvolatile bool result = true;\n\n\tvolatile SigHandler oldHandler = signal(SIGILL, SigIllHandler);\n',
      '#if defined(EMUSUNI_PORT)\n'
      '\t// EmuSuNi (scripts/build-3ds-core.sh): CPUID itself, no SIGILL handler (it would replace the app\'s).\n'
      '\tasm volatile\n\t(\n'
      '\t\t"pushq %%rbx; cpuid; mov %%ebx, %%edi; popq %%rbx"\n'
      '\t\t: "=a" (output[0]), "=D" (output[1]), "=c" (output[2]), "=d" (output[3])\n'
      '\t\t: "a" (func), "c" (subfunc)\n\t\t: "cc"\n\t);\n'
      '\treturn true;\n'
      '#endif\n'
      '\tvolatile bool result = true;\n\n\tvolatile SigHandler oldHandler = signal(SIGILL, SigIllHandler);\n')
# The libretro log: the core formats every message it passes (HLE services log a lot at Debug level, on the
# emulation thread) and the app drops everything below Info: the filter at Info.
patch('src/citra_libretro/citra_libretro.cpp',
      '    CitraLibRetro() : log_filter(Common::Log::Level::Debug) {}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    CitraLibRetro() : log_filter(Common::Log::Level::Info) {} // EmuSuNi: the app drops what is below Info\n'
      '#else\n'
      '    CitraLibRetro() : log_filter(Common::Log::Level::Debug) {}\n'
      '#endif\n')
# Crypto++'s OS generator: arc4random_buf on FreeBSD (the PS4 hasn't it), /dev/urandom elsewhere.
patch('externals/cryptopp/src/random/osrng.cpp',
      '#ifdef __FreeBSD__\n# define DONT_USE_O_NOFOLLOW 1\n# define USE_FREEBSD_ARC4RANDOM 1\n',
      '#if defined(EMUSUNI_PORT)\n'
      '// EmuSuNi (scripts/build-3ds-core.sh): no arc4random on the PS4: src/common/emusuni_random.cpp.\n'
      'extern "C" void emusuni_3ds_random_bytes(void* out, size_t size);\n'
      '#elif defined(__FreeBSD__)\n# define DONT_USE_O_NOFOLLOW 1\n# define USE_FREEBSD_ARC4RANDOM 1\n')
patch('externals/cryptopp/src/random/osrng.cpp',
      '\tm_fd = open("/dev/urandom", flags);\n\tif (m_fd == -1)\n\t\tthrow OS_RNG_Err("open /dev/urandom");\n',
      '#if defined(EMUSUNI_PORT)\n'
      '\tm_fd = -1;  // EmuSuNi: emusuni_3ds_random_bytes\n'
      '#else\n'
      '\tm_fd = open("/dev/urandom", flags);\n\tif (m_fd == -1)\n\t\tthrow OS_RNG_Err("open /dev/urandom");\n'
      '#endif\n')
patch('externals/cryptopp/src/random/osrng.cpp',
      '#if !defined(CRYPTOPP_WIN32_AVAILABLE) && !defined(USE_FREEBSD_ARC4RANDOM)\n\tclose(m_fd);\n#endif\n',
      '#if !defined(CRYPTOPP_WIN32_AVAILABLE) && !defined(USE_FREEBSD_ARC4RANDOM)\n'
      '\tif (m_fd != -1)  // EmuSuNi: none opened\n'
      '\t\tclose(m_fd);\n#endif\n')
patch('externals/cryptopp/src/random/osrng.cpp',
      '# if defined(USE_FREEBSD_ARC4RANDOM)\n\t// Cryptographic quality prng based on ChaCha20,\n',
      '# if defined(EMUSUNI_PORT)\n'
      '\temusuni_3ds_random_bytes(output, size);  // EmuSuNi: the PS4\'s entropy\n'
      '# elif defined(USE_FREEBSD_ARC4RANDOM)\n\t// Cryptographic quality prng based on ChaCha20,\n')

# --- The 3DS's memory off the app's heap (direct memory on the PS4).
addfile('src/common/emusuni_mem.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the 3DS's memory blocks (FCRAM, VRAM, the New 3DS extra RAM)
// in pages of their own, off the app's heap: on the PS4 direct memory (the 4.5 GiB pool), elsewhere
// an anonymous mmap; the app's heap (new[]) if neither can.
#pragma once
#ifdef EMUSUNI_PORT
#include <cstddef>
#include <memory>

namespace EmuSuNi {
// Zeroed, 16 KiB aligned; nullptr when nothing can be had.
void* PagesAlloc(std::size_t size);
void PagesFree(void* p);

struct PagesDeleter {
    void operator()(unsigned char* p) const {
        PagesFree(p);
    }
};
using Pages = std::unique_ptr<unsigned char[], PagesDeleter>;
inline Pages MakePages(std::size_t size) {
    return Pages(static_cast<unsigned char*>(PagesAlloc(size)));
}
} // namespace EmuSuNi
#endif
''')
addfile('src/common/emusuni_mem.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): see emusuni_mem.h. After Flycast's port (scripts/build-dc-core.sh).
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <sys/types.h>
#include "common/emusuni_mem.h"
#include "common/logging/log.h"

#if defined(__ORBIS__)
extern "C" {
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t alignment, int32_t type, off_t* phys);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t alignment);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
int32_t sceKernelMunmap(void* addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
}
#endif

namespace EmuSuNi {
namespace {
constexpr size_t PAGE = 16384;
enum class Kind { Direct, Mapped, Heap };
struct Block {
    void* at;
    size_t size;
    off_t phys;
    Kind kind;
};
std::mutex g_lock;
Block g_blocks[16];
int g_count = 0;
} // namespace

void* PagesAlloc(size_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (size == 0) {
        return nullptr;
    }
    void* at = nullptr;
    off_t phys = 0;
    Kind kind = Kind::Mapped;
#if defined(__ORBIS__)
    constexpr int32_t PROT_CPU_RW = 0x03; // SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE
    constexpr int32_t WB_ONION = 0;       // SCE_KERNEL_WB_ONION: CPU write-back memory
    if (sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), size, 64 * 1024, WB_ONION,
                                      &phys) == 0) {
        // A hint well above 0x200000000, where the kernel library puts its own objects.
        void* base = reinterpret_cast<void*>(0x2600000000ull);
        const int32_t ret = sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, PAGE);
        if (ret == 0 && base != nullptr) {
            at = base;
            kind = Kind::Direct;
            std::memset(at, 0, size); // direct memory comes as it was left
        } else {
            LOG_WARNING(Common, "EmuSuNi: mapping {} KiB of direct memory failed ({:#x})", size / 1024,
                        static_cast<unsigned>(ret));
            sceKernelReleaseDirectMemory(phys, size);
        }
    }
#endif
    if (at == nullptr) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) {
            at = p; // (mmap gives whole pages, zeroed)
        } else {
            LOG_WARNING(Common, "EmuSuNi: no mapping for {} KiB: the heap", size / 1024);
            at = new (std::nothrow) unsigned char[size]();
            kind = Kind::Heap;
            if (at == nullptr) {
                LOG_ERROR(Common, "EmuSuNi: no memory for {} KiB", size / 1024);
                return nullptr;
            }
        }
    }
    {
        std::lock_guard guard{g_lock};
        if (g_count < static_cast<int>(sizeof(g_blocks) / sizeof(g_blocks[0]))) {
            g_blocks[g_count++] = {at, size, phys, kind};
        } else {
            LOG_ERROR(Common, "EmuSuNi: too many memory blocks ({} kept)", at);
        }
    }
    LOG_INFO(Common, "EmuSuNi: {} KiB of {} at {}", size / 1024,
             kind == Kind::Direct ? "direct memory" : kind == Kind::Mapped ? "mapped memory" : "the heap", at);
    return at;
}

void PagesFree(void* p) {
    if (p == nullptr) {
        return;
    }
    Block block{};
    {
        std::lock_guard guard{g_lock};
        for (int i = 0; i < g_count; i++) {
            if (g_blocks[i].at == p) {
                block = g_blocks[i];
                g_blocks[i] = g_blocks[--g_count];
                break;
            }
        }
    }
    if (block.at == nullptr) {
        LOG_ERROR(Common, "EmuSuNi: freeing {}, not a block of PagesAlloc", p);
        return;
    }
    switch (block.kind) {
    case Kind::Direct:
#if defined(__ORBIS__)
        sceKernelMunmap(block.at, block.size);
        sceKernelReleaseDirectMemory(block.phys, block.size);
#endif
        break;
    case Kind::Mapped:
        munmap(block.at, block.size);
        break;
    case Kind::Heap:
        delete[] static_cast<unsigned char*>(block.at);
        break;
    }
}
} // namespace EmuSuNi
#endif
''')
patch('src/core/memory.cpp',
      '#include "common/settings.h"\n#include "common/swap.h"\n',
      '#include "common/settings.h"\n#include "common/swap.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mem.h"\n#endif\n')
patch('src/core/memory.cpp',
      '    std::unique_ptr<u8[]> fcram = std::make_unique<u8[]>(Memory::FCRAM_N3DS_SIZE);\n'
      '    std::unique_ptr<u8[]> vram = std::make_unique<u8[]>(Memory::VRAM_SIZE);\n'
      '    std::unique_ptr<u8[]> n3ds_extra_ram = std::make_unique<u8[]>(Memory::N3DS_EXTRA_RAM_SIZE);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: pages of their own, off the app\'s heap (direct memory on the PS4; common/emusuni_mem.h).\n'
      '    EmuSuNi::Pages fcram = EmuSuNi::MakePages(Memory::FCRAM_N3DS_SIZE);\n'
      '    EmuSuNi::Pages vram = EmuSuNi::MakePages(Memory::VRAM_SIZE);\n'
      '    EmuSuNi::Pages n3ds_extra_ram = EmuSuNi::MakePages(Memory::N3DS_EXTRA_RAM_SIZE);\n'
      '#else\n'
      '    std::unique_ptr<u8[]> fcram = std::make_unique<u8[]>(Memory::FCRAM_N3DS_SIZE);\n'
      '    std::unique_ptr<u8[]> vram = std::make_unique<u8[]>(Memory::VRAM_SIZE);\n'
      '    std::unique_ptr<u8[]> n3ds_extra_ram = std::make_unique<u8[]>(Memory::N3DS_EXTRA_RAM_SIZE);\n'
      '#endif\n')

# DynCom's translation cache (the interpreter's, 125 MiB): an array in the core's data upstream, which
# stays reserved in the app whether the interpreter runs or not; a block of its own when first used.
patch('src/core/arm/dyncom/arm_dyncom_trans.h',
      '#define TRANS_CACHE_SIZE (64 * 1024 * 2000)\nextern char trans_cache_buf[TRANS_CACHE_SIZE];\n',
      '#define TRANS_CACHE_SIZE (64 * 1024 * 2000)\n'
      '#ifdef EMUSUNI_PORT\n'
      'extern char* trans_cache_buf; // EmuSuNi: taken when the interpreter first translates (arm_dyncom_trans.cpp)\n'
      '#else\n'
      'extern char trans_cache_buf[TRANS_CACHE_SIZE];\n'
      '#endif\n')
patch('src/core/arm/dyncom/arm_dyncom_trans.cpp',
      '#include "core/arm/skyeye_common/vfp/vfp.h"\n\n'
      'char trans_cache_buf[TRANS_CACHE_SIZE];\n'
      'size_t trans_cache_buf_top = 0;\n\n'
      'static void* AllocBuffer(std::size_t size) {\n'
      '    std::size_t start = trans_cache_buf_top;\n',
      '#include "core/arm/skyeye_common/vfp/vfp.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mem.h"\n#endif\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: pages of its own, only when the interpreter runs (the array stayed in the app\'s memory).\n'
      'char* trans_cache_buf = nullptr;\n'
      '#else\n'
      'char trans_cache_buf[TRANS_CACHE_SIZE];\n'
      '#endif\n'
      'size_t trans_cache_buf_top = 0;\n\n'
      'static void* AllocBuffer(std::size_t size) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (trans_cache_buf == nullptr) {\n'
      '        trans_cache_buf = static_cast<char*>(EmuSuNi::PagesAlloc(TRANS_CACHE_SIZE));\n'
      '        ASSERT_MSG(trans_cache_buf != nullptr, "No memory for the interpreter\'s translation cache");\n'
      '    }\n'
      '#endif\n'
      '    std::size_t start = trans_cache_buf_top;\n')

# --- $HOME: unset on the PS4 (the core's files go to the libretro save directory anyway).
patch('src/common/file_util.cpp',
      '            auto pw = getpwuid(getuid());\n'
      '            ASSERT_MSG(pw,\n'
      '                       "$HOME isn’t defined, and the current user can’t be found in /etc/passwd.");\n'
      '            home_path = pw->pw_dir;\n',
      '            auto pw = getpwuid(getuid());\n'
      '#ifdef EMUSUNI_PORT\n'
      '            if (pw == nullptr) {\n'
      '                home_path = "/data/EmuSuNi"; // EmuSuNi: the PS4 has no $HOME and no passwd\n'
      '            } else\n'
      '#else\n'
      '            ASSERT_MSG(pw,\n'
      '                       "$HOME isn’t defined, and the current user can’t be found in /etc/passwd.");\n'
      '#endif\n'
      '            home_path = pw->pw_dir;\n')

# --- No geometry shaders on the PS4 (as Azahar on Android: Instance::UseGeometryShaders() false). Without fragment
# shader barycentrics (RADV has them from GFX10.3 on, the PS4's GPU is GFX7) Azahar gives every lit draw a geometry
# shader, and the first one hung the PS4's GPU: 00.62 closed at the 3D title screens of Super Mario 3D Land and Hey!
# Pikmin (the screen went black; "Run out of pools" in the log: descriptor sets that the stopped GPU never gave back).
# On the PS4 the rings' sizes (VGT_ESGS_RING_SIZE, VGT_GSVS_RING_SIZE) belong to the system's driver
# (libSceGnmDriver's sceGnmSetGsRingSizes, 4-8 MiB, 1 MiB aligned); RADV writes them from its command buffer. On a
# GCN GPU of that age a geometry shader also costs memory traffic (ES -> ring -> GS -> ring -> copy shader). Without
# it the vertex shader passes the normal quaternions straight on, as on Android (some lit edges may shade a little
# differently).
patch('src/video_core/renderer_vulkan/vk_instance.h',
      '    bool UseGeometryShaders() const {\n#ifdef __ANDROID__\n',
      '    bool UseGeometryShaders() const {\n'
      '#if defined(__ANDROID__) || defined(EMUSUNI_PORT) // EmuSuNi: the PS4 too (scripts/build-3ds-core.sh)\n')
# --- Descriptor pools: an allocation that fails for any other reason than a full pool looped forever (the emulation
# thread spinning, the picture frozen); now it throws, and the libretro glue ends the game with the message. The log
# line says which heap grew and to how many pools.
patch('src/video_core/renderer_vulkan/vk_resource_pool.cpp',
      '            current_pool++;\n'
      '            if (current_pool == pools.size()) {\n'
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one!");\n'
      '                AppendDescriptorPool();\n'
      '            }\n'
      '            alloc_info.descriptorPool = *pools[current_pool];\n'
      '        }\n',
      '            current_pool++;\n'
      '            if (current_pool == pools.size()) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one! ({} sets each, {} pools now)",\n'
      '                         descriptor_heap_count, pools.size() + 1);\n'
      '#else\n'
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one!");\n'
      '#endif\n'
      '                AppendDescriptorPool();\n'
      '            }\n'
      '            alloc_info.descriptorPool = *pools[current_pool];\n'
      '        }\n'
      '#ifdef EMUSUNI_PORT\n'
      '        else {\n'
      '            throw std::runtime_error(\n'
      '                fmt::format("vkAllocateDescriptorSets failed: {}", vk::to_string(result)));\n'
      '        }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_resource_pool.cpp',
      '#include <cstddef>\n#include <optional>\n',
      '#include <cstddef>\n#include <optional>\n#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include <fmt/format.h>\n#endif\n')

# --- The right eye not drawn (Azahar's "Disable Right Eye Render", off by default upstream because it "can cause major
# graphical issues" in some games, azahar issue #594; "up to 50%" faster in others, release 2120). EmuSuNi never shows
# 3D, yet some games draw both eyes all the time: on the PC, Super Mario 3D Land 1-1 took 15% less of the emulation
# thread's time and 30% less of the Vulkan worker's (Hey! Pikmin: 4% less). The titles Azahar knows it breaks are kept
# drawing by its hack list (Luigi's Mansion 1 and 2, Rayman Origins: HackType::RIGHT_EYE_DISABLE, read per process in
# GSP_GPU::AcquireGpuRight). A game that flickers draws it again with the file /data/EmuSuNi/3ds-right-eye (any
# content; read at each game start), or EMUSUNI_3DS_RIGHT_EYE=1 on the PC.
patch('src/citra_libretro/core_settings.cpp',
      '    Settings::values.use_disk_shader_cache =\n'
      '        LibRetro::FetchVariable(config::graphics::use_disk_shader_cache, config::enabled) ==\n'
      '        config::enabled;\n',
      '    Settings::values.use_disk_shader_cache =\n'
      '        LibRetro::FetchVariable(config::graphics::use_disk_shader_cache, config::enabled) ==\n'
      '        config::enabled;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the right eye is never shown on the PS4 (no 3D): not drawn (scripts/build-3ds-core.sh).\n'
      '    Settings::values.disable_right_eye_render = std::getenv("EMUSUNI_3DS_RIGHT_EYE") == nullptr &&\n'
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n'
      '#endif\n')
patch('src/citra_libretro/core_settings.cpp',
      '#include "citra_libretro/core_settings.h"\n',
      '#include "citra_libretro/core_settings.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <cstdlib>\n'
      '#include <unistd.h>\n'
      '#endif\n')

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
print("Azahar: patches applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-3ds-core.sh: the Azahar core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f azahar_$f"; done
} > "$OUT/prefix_azahar.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_azahar.h.new" "$OUT/prefix_azahar.h"; then
  rm -f "$OUT/prefix_azahar.h.new"
else
  mv "$OUT/prefix_azahar.h.new" "$OUT/prefix_azahar.h"
fi

if [ "$HOST" != 1 ]; then
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
fi
: > "$LOG"

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): -O2 (2-7% faster than CMake's -O3
# in the 00.63 3DS speed tests), the PS4's CPU and the renamed API.
FLAGS_CORE="-O2 -DNDEBUG -march=btver2 -include $OUT/prefix_azahar.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a"
# CTR_CORE_SECTIONS=1: a section for each function and each variable, so that a link with --gc-sections (the
# app's) can drop what nothing reaches: the 3DS core's code and data are part of the app's image, which the PS4
# takes out of the memory the games use. Off, the flags (and the object) are what they were before this option;
# on, build in another <out dir> (the flags make another stamp).
[ "${CTR_CORE_SECTIONS:-0}" = 1 ] && FLAGS_CORE+=" -ffunction-sections -fdata-sections"
OPTIONS=(
  -DENABLE_LIBRETRO=ON -DEMUSUNI_PORT=ON -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DENABLE_LTO=OFF
  -DENABLE_OPENGL=OFF -DENABLE_VULKAN=ON -DENABLE_SOFTWARE_RENDERER=ON
  -DCITRA_WARNINGS_AS_ERRORS=OFF -DCITRA_USE_PRECOMPILED_HEADERS=ON -DENABLE_TESTS=OFF
  -DENABLE_QT=OFF -DENABLE_QT_TRANSLATION=OFF -DENABLE_SDL2=OFF -DENABLE_WEB_SERVICE=OFF
  -DENABLE_SCRIPTING=OFF -DENABLE_GDBSTUB=OFF -DENABLE_ROOM=OFF -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF
  -DENABLE_LIBUSB=OFF -DENABLE_DISCORD_RPC=OFF -DENABLE_MICROPROFILE=OFF
  # (zstd's dictionary builder wants qsort_r, which the PS4's libc hasn't; Azahar doesn't use it.)
  -DZSTD_BUILD_DICTBUILDER=OFF
  # No Nintendo keys compiled in (upstream's default: an encrypted blob for players without a dump).
  # As the other consoles' BIOS files, the player's own: sysdata/aes_keys.txt in the core's directory
  # (GodMode9 dumps it). Decrypted games run without; PS/APT encryption and amiibo need it (logged).
  -DENABLE_BUILTIN_KEYBLOB=OFF
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
# (The tree's GIT-COMMIT, checked above, not the commit setup.sh made locally: the same everywhere. Of
# this script, what makes the build: up to the checks, which a change to needn't compile everything
# again.)
WANT="$SRC | $TREE_COMMIT | host $HOST | ${OPTIONS[*]} | $(sed -n '1,/^# The checks\.$/p' "${BASH_SOURCE[0]}" | sha256sum | cut -d' ' -f1)"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "Azahar: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "Azahar: configuring failed (see $LOG)"
    exit 1
  fi
  # The stamp says what the build directory was configured with, written now: a build cut short (WSL
  # has died under one on this PC) goes on from where it was next time, ninja knowing what is done.
  printf '%s' "$WANT" > "$STAMP"
fi
# (-k 0: every error at once, not only the first.)
if ! ninja -C "$BUILD" -j "$JOBS" -k 0 azahar_libretro >> "$LOG" 2>&1; then
  grep -E "error:" "$LOG" | sort | uniq -c | sort -rn | head -60
  echo "Azahar: build failed (see $LOG)"
  exit 1
fi

# One object: only the libretro API (renamed) stays global. Azahar's own libraries whole (the
# libretro glue, core, video, audio, network, common: Boost.Serialization's class registrations are
# static objects in them that nothing references); its dependencies (Boost's serialization and
# iostreams, Crypto++, dynarmic with mcl and Zydis, fmt, zstd, glslang, SPIRV-Tools, sirit, teakra,
# faad2, SoundTouch, enet, xxHash, lodepng, inih, the open-source system archives) only as far as
# referenced: linked whole they carried 4.6 MB of code and 6.4 MB of static memory nothing used
# (glslang's disassembler tables, teakra's test generator, Crypto++'s other ciphers...).
# Replaced only when it changed (the app isn't linked again for nothing).
mapfile -t OWN < <(find "$BUILD/src" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
mapfile -t DEPS < <(find "$BUILD/externals" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
OTHERS=$(find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -not -path "$BUILD/src/*" -not -path "$BUILD/externals/*")
if [ -n "$OTHERS" ]; then
  echo "Azahar: libraries outside src/ and externals/ (whole, or as far as referenced?):"
  echo "$OTHERS"
  exit 1
fi
ld.lld-21 -r --whole-archive "${OWN[@]}" --no-whole-archive "${DEPS[@]}" -o "$OUT/3ds_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='azahar_retro_*' "$OUT/3ds_core_all.o" "$OUT/3ds_core.o.new"
rm -f "$OUT/3ds_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/3ds_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "azahar_$f"; done | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "Azahar: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/3ds_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved: the app would export them as imports nobody gives (the
# PS4's loader may refuse those).
WEAK=$(llvm-nm-21 -u "$OUT/3ds_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "Azahar: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
# No std::random_device: libc++'s opens /dev/urandom, which the PS4 may not have (it throws then);
# everything random goes through common/emusuni_random.cpp.
if echo "$UNDEFINED" | grep -q '^_ZNSt3__113random_device'; then
  echo "Azahar: the object uses std::random_device (/dev/urandom on the PS4): common/emusuni_random.cpp instead"
  exit 1
fi
# mprotect, named only by Xbyak's inline code in the objects whose generators take EmuSuNi's allocator
# (dead there: useProtect() is false), and by Zydis's memory helper, which nothing references. Another
# object naming it is a generator left with Xbyak's own allocator (mmap, then mprotect(RWX): as
# dynarmic's spin lock was, at static initialisation) or a new mprotect user: it must take the pool.
MPROTECT_OK="a32_interface.cpp.o block_of_code.cpp.o Memory.c.o shader_jit_x64_compiler.cpp.o"
# (The members as "file.o": CMake keeps their directory in the archive, the PS4 toolchain names them
# .obj, this PC's .o.)
MPROTECT_USERS=$(for a in "${OWN[@]}" "${DEPS[@]}"; do llvm-nm-21 -u --print-file-name "$a" 2>/dev/null; done |
  awk '$(NF-1) == "U" && $NF == "mprotect" {print $1}' | sed 's/^.*\.a://; s/:$//; s#^.*/##; s/\.obj$/.o/' | sort -u)
MPROTECT_NEW=$(comm -23 <(echo "$MPROTECT_USERS") <(echo "$MPROTECT_OK" | tr ' ' '\n' | sort -u))
if [ -n "$MPROTECT_NEW" ]; then
  echo "Azahar: objects naming mprotect the port doesn't know (a Xbyak generator without EmuSuNi's allocator?):"
  echo "$MPROTECT_NEW"
  exit 1
fi
if llvm-nm-21 --defined-only "$OUT/3ds_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "Azahar: the object defines a C++ operator new/delete of its own"
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
  clang-21 $PS4C_CHECK -O2 -DEMUSUNI_3DS=1 -c "$PROJECT/port/ps4_n64.c" -o "$OUT/check_ps4_n64.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores. The threads' hook,
  # emusuni_3ds_thread, is the app's to define in port/: a note if it isn't yet.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o" | awk 'NF >= 3 {print $3}';
                 echo __dso_handle; echo emusuni_3ds_thread; } | sort -u)
  rm -f "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o"
  if ! grep -qs "emusuni_3ds_thread" "$PROJECT"/port/*.c "$PROJECT"/port/*.cpp; then
    echo "Azahar: note: port/ doesn't define emusuni_3ds_thread yet (the app won't link without it)"
  fi
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "Azahar: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
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
    echo "Azahar: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "Azahar: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app" \
       "($(echo "$NEW_DELETE" | sed '/^$/d' | wc -l) operator new/delete, port/ps4_new.cpp's)"
fi
if cmp -s "$OUT/3ds_core.o.new" "$OUT/3ds_core.o"; then
  rm -f "$OUT/3ds_core.o.new"
else
  mv "$OUT/3ds_core.o.new" "$OUT/3ds_core.o"
fi
ls -la "$OUT/3ds_core.o"
