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
#   - Fast memory for dynarmic, on by default since 00.68 (the file /data/EmuSuNi/3ds-fastmem-off
#     turns it off; common/emusuni_fastmem.cpp; the PC tests: EMUSUNI_3DS_FASTMEM=0 off): a page
#     table's 4 GiB reserved,
#     the 16 KiB groups of guest pages that lie in order in the 3DS's memory blocks mapped there a
#     second time (direct memory aliasing), so a recompiled load or store is one instruction; anything
#     else faults once and is recompiled with the page table (dynarmic's POSIX fault handler, on the PS4
#     too, installed only then and taken out at the game's end). A self-test before the first use, the
#     page table whenever anything is refused, the marker files 3ds-fastmem-trying and -crashed for a
#     start that dies while trying it; the loader's FCRAM at the guest addresses' 16 KiB phase. Turned
#     off: the page table and no fault handler, as before. Nothing is mprotect-ed (checked at the
#     end: the only objects that still name mprotect are Xbyak's inline code in the generators given
#     EmuSuNi's allocator, which never runs it).
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
#     (the PS4 runs threads by fixed priorities, which the hook sets). 3 pipeline and 3 shader
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
#   - Shader stalls: a pipeline that a draw needs while it still waits in the workers' queue is built
#     at once; the driver's pipeline cache is saved every 60 s while it grows, not only when a game
#     ends cleanly; asynchronous shader compilation by default since 00.68 (a draw whose pipeline
#     isn't ready is skipped until it is; /data/EmuSuNi/3ds-sync-shaders waits instead). A frame of
#     more than 100 ms says in the log where its time went (the guest's code, the JIT compiling it,
#     the HLE calls, the GPU's commands, the audio; pipelines, shaders, textures, file reads, the wait
#     for the GPU side), and every 600 frames a line sums it up. dynarmic without its IR verification
#     pass (asserts only); the audio's resampling over a contiguous copy (the same output).
#   - The JIT's compiles (00.69): dynarmic writes the same x86 code in about 40% less time and with a
#     quarter of the heap allocations (the VFP and ASIMD decoders searched only where their patterns can
#     match; the register allocator without its scans, copies and temporary vectors; the IR pool's slab
#     kept for the next block; the blocks' ranges in 4 KiB page buckets; Xbyak's label tables' nodes from
#     free lists of the thread); the JIT warm-up (citra_libretro/emusuni_jitwarm.h: the blocks of the
#     game's code segment and its accesses outside the fast memory kept per game in
#     shaders/emusuni-jit/<program id>.bin, compiled ahead from the next start's first frame;
#     /data/EmuSuNi/3ds-jit-warmup-off turns it off). The FS service's file work on two threads made
#     once (RunAsync made one per request; System::Shutdown waits for them first); ETC1 textures decoded
#     a 4x4 subtile at a time, and every texture into ordinary memory, then copied once into the staging
#     buffer (write-combined on the PS4).
#   - The draws (00.70): a draw whose texture and utility descriptor writes are the last draw's, in the same
#     submission, binds that descriptor set again (upstream wrote a new set for every draw); a pipeline state
#     equal byte for byte to the last draw's gives its pipeline without the two hashes and lookups; two loops of
#     every draw without masked moves (vmaskmovps, microcoded on the Jaguar). A save state's load applies 3DS CPU
#     Clock again (the timers came back at 100%). The stall log counts the draws, their vertices and time, the
#     command lists' size and the game's own pictures.
#   - $HOME unset (the PS4): /data/EmuSuNi instead of a failed assertion (the libretro save
#     directory is where the core's files really go).
#   - Built as a static library, at -O2 with the profile scripts/3ds-core.profdata.xz (PGO, below),
#     without LTO, precompiled headers on, no -Werror, without the stack
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
#   CTR_CORE_PGO=off build-3ds-core.sh <out dir>  without the PGO profile;
#   CTR_CORE_HOST=1 CTR_CORE_PGO=generate ...     instrumented, to make a new one (PGO, below).
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
      '    emusuni_dmem.h\n    emusuni_fastmem.cpp\n    emusuni_fastmem.h\n    emusuni_fault.h\n'
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
      '// EmuSuNi: 3 pipeline and 3 shader compile workers (upstream: half the CPUs each). On the PS4 they run\n'
      '// below every other thread of the game on cores 0, 1, 3, 4 and 5 (port/ps4_n64.c), all but the\n'
      '// emulation\'s: a burst of new pipelines (0.25-0.5 s each there) is built 3 at a time while those cores\n'
      '// idle (2 until 00.65: the third\'s core was free; the GPU filter\'s compiles use the same five).\n'
      'static u32 EmuSuNiWorkerThreads() {\n    return 3;\n}\n'
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
#include <cstdint>
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

// The blocks of direct memory (all of them on the PS4; on the PC tests with the fast memory, its stand-ins'):
// where each is mapped, its size and its offset in direct memory, for the fast memory to map them again
// (common/emusuni_fastmem.cpp). Returns how many it wrote.
struct DirectBlock {
    void* at;
    std::size_t size;
    std::int64_t phys;
};
int PagesDirectBlocks(DirectBlock* out, int max);
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
#include <cstdlib>
#include "common/emusuni_dmem.h"
#include "common/emusuni_fastmem.h"
#include "common/emusuni_mem.h"
#include "common/logging/log.h"

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
    const bool direct = true;
#else
    // (The PC tests: the stand-ins' direct memory (common/emusuni_dmem.h) when the fast memory is on, for it to
    // map these blocks a second time, or with EMUSUNI_3DS_DMEM_SHIM=1 to compare on the same memory; else an mmap.)
    const bool direct = Fastmem::On() || std::getenv("EMUSUNI_3DS_DMEM_SHIM") != nullptr;
#endif
    constexpr int32_t PROT_CPU_RW = 0x03; // SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE
    constexpr int32_t WB_ONION = 0;       // SCE_KERNEL_WB_ONION: CPU write-back memory
    if (direct && sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), size,
                                                64 * 1024, WB_ONION, &phys) == 0) {
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
        sceKernelMunmap(block.at, block.size);
        sceKernelReleaseDirectMemory(block.phys, block.size);
        break;
    case Kind::Mapped:
        munmap(block.at, block.size);
        break;
    case Kind::Heap:
        delete[] static_cast<unsigned char*>(block.at);
        break;
    }
}

int PagesDirectBlocks(DirectBlock* out, int max) {
    std::lock_guard guard{g_lock};
    int n = 0;
    for (int i = 0; i < g_count && n < max; i++) {
        if (g_blocks[i].kind == Kind::Direct) {
            out[n++] = {g_blocks[i].at, g_blocks[i].size, static_cast<std::int64_t>(g_blocks[i].phys)};
        }
    }
    return n;
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


# --- Where a slow frame's time goes (for the log): the 3DS's stalls on the PS4 are 0.4-2.6 s frames every ~10 s in
# the first minutes (00.65 log), and the app's lines can't say why. The core counts, on every thread, the pipelines
# it builds, the shaders it compiles, the textures it decodes and uploads, the bytes it reads from files and the time
# the emulation thread waits for the GPU side; a retro_run of more than 100 ms logs where its time went
# ("EmuSuNi: slow frame ..."), and every 600 frames one line sums them up ("EmuSuNi: 600 frames: ..."). A few
# clock reads per event: nothing measurable on the PC (the A/B in the 3ds-speed v2 research).
addfile('src/common/emusuni_stats.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): what the core spends time on, for the log's slow-frame lines
// (citra_libretro.cpp). Counted on every thread; "emu" is the thread that runs retro_run.
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include <chrono>
#include <cstdint>

namespace EmuSuNi::Stats {
enum Counter {
    PipelinesEmu,   // pipelines built on the emulation thread (the driver's cache asked first)
    PipelineUsEmu,
    PipelinesOther, // on the pipeline workers
    PipelineUsOther,
    ShadersEmu,     // GLSL or SPIR-V compiled into a shader module
    ShaderUsEmu,
    ShadersOther,
    ShaderUsOther,
    UploadBytes,    // guest textures decoded and uploaded (emulation thread)
    UploadUs,
    ReadBytesEmu,   // bytes read from files (the game, saves, caches)
    ReadUsEmu,
    ReadBytesOther,
    ReadUsOther,
    WaitUs,         // the emulation thread waiting for the GPU worker or a GPU fence
    Count
};
inline std::atomic<std::uint64_t> counters[Count];
inline thread_local bool on_emulation_thread = false;

inline std::uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline void Add(Counter c, std::uint64_t v) {
    counters[c].fetch_add(v, std::memory_order_relaxed);
}
// Counts one event and its time into the emulation thread's pair (emu) or the other threads' (other).
struct Event {
    Counter count, us;
    std::uint64_t t0 = NowUs();
    Event(Counter emu_count, Counter other_count)
        : count(on_emulation_thread ? emu_count : other_count),
          us(static_cast<Counter>((on_emulation_thread ? emu_count : other_count) + 1)) {}
    ~Event() {
        Add(count, 1);
        Add(us, NowUs() - t0);
    }
};
struct Timer {
    Counter us;
    std::uint64_t t0 = NowUs();
    explicit Timer(Counter us_) : us(us_) {}
    ~Timer() {
        Add(us, NowUs() - t0);
    }
};
} // namespace EmuSuNi::Stats
#endif
''')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      'bool GraphicsPipeline::Build(bool fail_on_compile_required) {\n    MICROPROFILE_SCOPE(Vulkan_Pipeline);\n',
      'bool GraphicsPipeline::Build(bool fail_on_compile_required) {\n    MICROPROFILE_SCOPE(Vulkan_Pipeline);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      'vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {\n',
      'vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::ShadersEmu, EmuSuNi::Stats::ShadersOther};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      'std::vector<u32> CompileGLSL(std::string_view code, vk::ShaderStageFlagBits stage,\n'
      '                             std::string_view premable) {\n',
      'std::vector<u32> CompileGLSL(std::string_view code, vk::ShaderStageFlagBits stage,\n'
      '                             std::string_view premable) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (its time with the module\'s: one shader, counted by CompileSPV)\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::on_emulation_thread ? EmuSuNi::Stats::ShaderUsEmu\n'
      '                                                                                   : EmuSuNi::Stats::ShaderUsOther};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      '#include "common/assert.h"\n',
      '#include "common/assert.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      'void RasterizerCache<T>::UploadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_UploadSurface);\n',
      'void RasterizerCache<T>::UploadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_UploadSurface);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::UploadUs};\n'
      '    EmuSuNi::Stats::Add(EmuSuNi::Stats::UploadBytes, boost::icl::length(interval));\n'
      '#endif\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/common/file_util.cpp',
      '    std::size_t read = FREAD(data, elem_size, length, m_file);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const std::uint64_t emusuni_t0 = EmuSuNi::Stats::NowUs();\n'
      '#endif\n'
      '    std::size_t read = FREAD(data, elem_size, length, m_file);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        const bool emu = EmuSuNi::Stats::on_emulation_thread;\n'
      '        EmuSuNi::Stats::Add(emu ? EmuSuNi::Stats::ReadBytesEmu : EmuSuNi::Stats::ReadBytesOther, read * elem_size);\n'
      '        EmuSuNi::Stats::Add(emu ? EmuSuNi::Stats::ReadUsEmu : EmuSuNi::Stats::ReadUsOther,\n'
      '                            EmuSuNi::Stats::NowUs() - emusuni_t0);\n'
      '    }\n'
      '#endif\n')
patch('src/common/file_util.cpp',
      '#include "common/assert.h"\n',
      '#include "common/assert.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);\n    DispatchWork();\n',
      '    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n'
      '#endif\n'
      '    DispatchWork();\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      'void Scheduler::Wait(u64 tick) {\n',
      'void Scheduler::Wait(u64 tick) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (Most calls find the tick done already: the stream buffers ask at every draw. Only a real wait is timed:\n'
      '    // two clock reads at every draw were 0.5% of the emulation thread on the PC, where reading the clock costs\n'
      '    // no system call.)\n'
      '    if (master_semaphore->IsFree(tick)) {\n'
      '        return;\n'
      '    }\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '#endif\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: where a slow frame\'s time went (common/emusuni_stats.h), and a summary every 600 frames.\n'
      '    struct FrameStats {\n'
      '        std::uint64_t t0 = EmuSuNi::Stats::NowUs();\n'
      '        std::uint64_t at[EmuSuNi::Stats::Count];\n'
      '        FrameStats() {\n'
      '            EmuSuNi::Stats::on_emulation_thread = true;\n'
      '            for (int i = 0; i < EmuSuNi::Stats::Count; i++)\n'
      '                at[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);\n'
      '        }\n'
      '        static std::string Describe(const std::uint64_t* d, std::uint64_t ms) {\n'
      '            using namespace EmuSuNi::Stats;\n'
      '            return fmt::format("{} ms: waited {} ms for the GPU side; pipelines {} here ({} ms) and {} on the "\n'
      '                               "workers ({} ms); shaders {} here ({} ms) and {} on the workers ({} ms); "\n'
      '                               "textures {} KiB ({} ms); files read {} KiB here ({} ms) and {} KiB elsewhere ({} ms)",\n'
      '                               ms, d[WaitUs] / 1000, d[PipelinesEmu], d[PipelineUsEmu] / 1000, d[PipelinesOther],\n'
      '                               d[PipelineUsOther] / 1000, d[ShadersEmu], d[ShaderUsEmu] / 1000, d[ShadersOther],\n'
      '                               d[ShaderUsOther] / 1000, d[UploadBytes] / 1024, d[UploadUs] / 1000,\n'
      '                               d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024,\n'
      '                               d[ReadUsOther] / 1000);\n'
      '        }\n'
      '        ~FrameStats() {\n'
      '            static std::uint64_t period_at[EmuSuNi::Stats::Count];\n'
      '            static std::uint64_t period_t0 = t0;\n'
      '            static int frames = 0;\n'
      '            const std::uint64_t ms = (EmuSuNi::Stats::NowUs() - t0) / 1000;\n'
      '            std::uint64_t d[EmuSuNi::Stats::Count];\n'
      '            // (100 ms, or EMUSUNI_3DS_SLOW_MS on the PC tests)\n'
      '            static const std::uint64_t slow_ms =\n'
      '                std::getenv("EMUSUNI_3DS_SLOW_MS") ? std::strtoull(std::getenv("EMUSUNI_3DS_SLOW_MS"), nullptr, 10) : 100;\n'
      '            if (ms >= slow_ms) {\n'
      '                for (int i = 0; i < EmuSuNi::Stats::Count; i++)\n'
      '                    d[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed) - at[i];\n'
      '                LOG_WARNING(Frontend, "EmuSuNi: slow frame {}", Describe(d, ms));\n'
      '            }\n'
      '            if (++frames == 600) {\n'
      '                for (int i = 0; i < EmuSuNi::Stats::Count; i++) {\n'
      '                    const std::uint64_t now = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);\n'
      '                    d[i] = now - period_at[i];\n'
      '                    period_at[i] = now;\n'
      '                }\n'
      '                LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, (EmuSuNi::Stats::NowUs() - period_t0) / 1000));\n'
      '                period_t0 = EmuSuNi::Stats::NowUs();\n'
      '                frames = 0;\n'
      '            }\n'
      '        }\n'
      '    } emusuni_frame_stats;\n'
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "citra_libretro/citra_libretro.h"\n',
      '#include "citra_libretro/citra_libretro.h"\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include "common/emusuni_stats.h"\n#endif\n')


# --- Shorter shader stalls (3ds-speed v2). On the PS4 the 3DS stops for 0.4-2.6 s every ~10 s in a game's first
# minutes (00.65 log): a draw whose Vulkan pipeline isn't built yet is recorded anyway, the Vulkan worker waits for
# the pipeline (GraphicsPipeline::Build on the pipeline workers), and the emulation thread waits for the worker at the
# end of every frame (RasterizerVulkan::TickFrame -> Scheduler::WaitWorker). Three changes:
#  1. A pipeline still waiting in the workers' queue when a draw needs it is built at once on the emulation thread,
#     instead of after everything queued before it (the start-up precompile of the disk cache queues every pipeline
#     the driver's cache doesn't have: hundreds after an update of the app, whose new Mesa build invalidates it).
#  2. The driver's pipeline cache (saves/3ds/Azahar/shaders/vulkan/pipeline/*.bin) is also saved every 60 s while new
#     pipelines appear, on the pipeline workers, not only when the game ends cleanly: a session closed from the PS4's
#     menu or by a crash kept nothing of what it compiled. (Never during a switch to another title's cache.)
#  3. Azahar's asynchronous shader compilation (by default since 00.68; the file /data/EmuSuNi/3ds-sync-shaders
#     turns it off): a draw whose pipeline isn't ready is skipped (an object missing for a few frames) instead of
#     stopping the game.
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '    std::array<Shader*, 3> stages;\n    bool is_pending{};\n};\n',
      '    std::array<Shader*, 3> stages;\n    bool is_pending{};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::atomic<int> emusuni_claimed{0}; // EmuSuNi: 1 once a thread has started building it\n'
      '#endif\n'
      '};\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    if (is_pending) {\n        return wait_built;\n    }\n',
      '    if (is_pending) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: still in the workers\' queue (no worker has started it): built now, here, rather than after\n'
      '        // everything queued before it.\n'
      '        int expected = 0;\n'
      '        if (wait_built && emusuni_claimed.compare_exchange_strong(expected, 1)) {\n'
      '            Build();\n'
      '        }\n'
      '#endif\n'
      '        return wait_built;\n'
      '    }\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    // Fallback to (a)synchronous compilation\n    worker->QueueWork([this] { Build(); });\n',
      '    // Fallback to (a)synchronous compilation\n'
      '#ifdef EMUSUNI_PORT\n'
      '    worker->QueueWork([this] {\n'
      '        int expected = 0;\n'
      '        if (emusuni_claimed.compare_exchange_strong(expected, 1)) {\n'
      '            Build();\n'
      '        }\n'
      '    });\n'
      '#else\n'
      '    worker->QueueWork([this] { Build(); });\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    void SaveDriverPipelineDiskCache();\n',
      '    void SaveDriverPipelineDiskCache();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: EmuSuNiSaveNowAndThen\'s state.\n'
      '    u64 emusuni_last_save_ms = 0;\n'
      '    size_t emusuni_saved_size = 0; // the data size at the last save (a new game starts a new PipelineCache)\n'
      '    // A save on a worker never runs while SwitchCache replaces the driver\'s cache, and one queued before\n'
      '    // the switch is dropped (the generation, changed only by the emulation thread under the mutex).\n'
      '    std::mutex emusuni_save_mutex;\n'
      '    u64 emusuni_save_generation = 0;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '#include <bitset>\n',
      '#include <bitset>\n#ifdef EMUSUNI_PORT\n#include <mutex>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    // Save current driver cache, update program ID and load the new driver cache\n'
      '    SaveDriverPipelineDiskCache();\n'
      '    SetProgramID(title_id);\n'
      '    LoadDriverPipelineDiskCache(stop_loading, nullptr);\n',
      '    // Save current driver cache, update program ID and load the new driver cache\n'
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        // EmuSuNi: not during a save of EmuSuNiSaveNowAndThen\'s, and none queued before it runs after.\n'
      '        std::scoped_lock emusuni_lock{emusuni_save_mutex};\n'
      '        emusuni_save_generation++;\n'
      '        emusuni_saved_size = 0;\n'
      '        SaveDriverPipelineDiskCache();\n'
      '        SetProgramID(title_id);\n'
      '        LoadDriverPipelineDiskCache(stop_loading, nullptr);\n'
      '    }\n'
      '#else\n'
      '    SaveDriverPipelineDiskCache();\n'
      '    SetProgramID(title_id);\n'
      '    LoadDriverPipelineDiskCache(stop_loading, nullptr);\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'PipelineCache::~PipelineCache() {\n',
      '#ifdef EMUSUNI_PORT\n'
      'void PipelineCache::EmuSuNiSaveNowAndThen() {\n'
      '    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(\n'
      '                                         std::chrono::steady_clock::now().time_since_epoch())\n'
      '                                         .count());\n'
      '    if (emusuni_last_save_ms == 0) {\n'
      '        emusuni_last_save_ms = now;\n'
      '        return;\n'
      '    }\n'
      '    if (now - emusuni_last_save_ms < 60000) {\n'
      '        return;\n'
      '    }\n'
      '    emusuni_last_save_ms = now;\n'
      '    // Only when the driver compiled something since the last save (its data grew).\n'
      '    if (!driver_pipeline_cache) {\n'
      '        return;\n'
      '    }\n'
      '    size_t size = 0;\n'
      '    if (instance.GetDevice().getPipelineCacheData(*driver_pipeline_cache, &size, nullptr) != vk::Result::eSuccess ||\n'
      '        size <= emusuni_saved_size) {\n'
      '        return;\n'
      '    }\n'
      '    emusuni_saved_size = size;\n'
      '    const u64 generation = emusuni_save_generation;\n'
      '    pipeline_workers.QueueWork([this, generation] {\n'
      '        std::scoped_lock lock{emusuni_save_mutex};\n'
      '        if (generation == emusuni_save_generation) {\n'
      '            SaveDriverPipelineDiskCache();\n'
      '        }\n'
      '    });\n'
      '}\n'
      '#endif\n'
      '\n'
      'PipelineCache::~PipelineCache() {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::TickFrame() {\n    scheduler.WaitWorker();\n    res_cache.TickFrame();\n}\n',
      'void RasterizerVulkan::TickFrame() {\n    scheduler.WaitWorker();\n    res_cache.TickFrame();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    pipeline_cache.EmuSuNiSaveNowAndThen();\n'
      '#endif\n'
      '}\n')
patch('src/citra_libretro/core_settings.cpp',
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n#endif\n',
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n'
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for, with the file\n'
      '    // /data/EmuSuNi/3ds-async-shaders (or EMUSUNI_3DS_ASYNC_SHADERS=1 on the PC).\n'
      '    Settings::values.async_shader_compilation = std::getenv("EMUSUNI_3DS_ASYNC_SHADERS") != nullptr ||\n'
      '                                                ::access("/data/EmuSuNi/3ds-async-shaders", F_OK) == 0;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '#include <boost/container/static_vector.hpp>\n',
      '#include <boost/container/static_vector.hpp>\n#ifdef EMUSUNI_PORT\n#include <chrono>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    bool BindPipeline(PipelineInfo& info, bool wait_built = false);\n',
      '    bool BindPipeline(PipelineInfo& info, bool wait_built = false);\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: the driver\'s cache saved again (on the pipeline workers) at most every 60 s while it grows;\n'
      '    /// called once a frame (RasterizerVulkan::TickFrame).\n'
      '    void EmuSuNiSaveNowAndThen();\n'
      '#endif\n')

# --- Fast memory for dynarmic, the ARM11's recompiler (common/emusuni_fastmem.h): a page table's guest 4 GiB as one
# reserved range in which the 16 KiB groups of guest pages backed by the 3DS's memory blocks (direct memory) are mapped
# a second time, so that a recompiled load or store is one instruction; anything else faults once and dynarmic
# recompiles that access with the page table. As the Dreamcast's (scripts/build-dc-core.sh: core/linux/emusuni_vmem.cpp):
# a self-test before the first use (aliasing, a fault with its address, pc and stack, the call it is turned into),
# the page table whenever anything is refused, the marker files for a start that dies while trying it. On by default
# since 00.68 (00.66-00.67: only with the file /data/EmuSuNi/3ds-fastmem); the file /data/EmuSuNi/3ds-fastmem-off
# turns it off (the PC tests: EMUSUNI_3DS_FASTMEM=0, on the stand-ins of common/emusuni_dmem.h).
addfile('src/common/emusuni_dmem.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the PS4's direct memory and address space calls, declared by hand as the
// kernel has them (several OpenOrbis prototypes are wrong). On the PC tests (not __ORBIS__) the same names are
// stand-ins with the PS4's rules (common/emusuni_fastmem.cpp): direct memory is a sparse memfd, mapping it an
// mmap(MAP_SHARED) of it, a reservation an mmap(PROT_NONE); everything in 16 KiB pages, a reservation hint that
// isn't free fails instead of moving. So the fast memory's very code runs there (on unless EMUSUNI_3DS_FASTMEM=0).
#pragma once
#ifdef EMUSUNI_PORT
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace EmuSuNi::Dmem {
constexpr int32_t kProtRead = 0x01;  // SCE_KERNEL_PROT_CPU_READ
constexpr int32_t kProtRW = 0x03;    // SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE
constexpr int32_t kWbOnion = 0;      // SCE_KERNEL_WB_ONION: CPU write-back memory
constexpr int32_t kMapFixed = 0x10;  // SCE_KERNEL_MAP_FIXED
constexpr std::size_t kPage = 0x4000;
} // namespace EmuSuNi::Dmem

#if defined(__ORBIS__)
extern "C" {
int32_t sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t length, size_t alignment,
                                      int32_t memoryType, off_t* physicalOut);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t length);
int32_t sceKernelMapDirectMemory(void** address, size_t length, int32_t protection, int32_t flags,
                                 off_t directMemoryStart, size_t alignment);
int32_t sceKernelReserveVirtualRange(void** address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* address, size_t length);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelInternalMemoryGetAvailableSize(size_t* size); // (as src/Log.cpp's LogMemory)
}
#else
namespace EmuSuNi::Dmem {
int32_t AllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t length, size_t alignment, int32_t memoryType,
                             off_t* physicalOut);
int32_t ReleaseDirectMemory(off_t start, size_t length);
int32_t MapDirectMemory(void** address, size_t length, int32_t protection, int32_t flags, off_t directMemoryStart,
                        size_t alignment);
int32_t ReserveVirtualRange(void** address, size_t length, int32_t flags, size_t alignment);
int32_t Munmap(void* address, size_t length);
size_t GetDirectMemorySize();
int InternalMemoryGetAvailableSize(size_t* size);
// EMUSUNI_3DS_SHIM_FAIL=<list> (comma-separated) makes one thing fail, to test the fallbacks: alloc, reserve,
// alias (a second mapping of the same direct memory refused), map=<n> (the n-th mapping), nofault (the self-test's
// unmapped page is mapped), noredirect (the self-test's handler leaves the context as it is), crash (the process
// is killed while it tries: the marker files).
bool Injected(const char* what, long* value = nullptr);
} // namespace EmuSuNi::Dmem
#define sceKernelAllocateDirectMemory EmuSuNi::Dmem::AllocateDirectMemory
#define sceKernelReleaseDirectMemory EmuSuNi::Dmem::ReleaseDirectMemory
#define sceKernelMapDirectMemory EmuSuNi::Dmem::MapDirectMemory
#define sceKernelReserveVirtualRange EmuSuNi::Dmem::ReserveVirtualRange
#define sceKernelMunmap EmuSuNi::Dmem::Munmap
#define sceKernelGetDirectMemorySize EmuSuNi::Dmem::GetDirectMemorySize
#define sceKernelInternalMemoryGetAvailableSize EmuSuNi::Dmem::InternalMemoryGetAvailableSize
#endif
#endif
''')
addfile('src/common/emusuni_fault.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): a fault as a signal handler sees it, for dynarmic's fault handler
// (externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp) and the fast memory's self-test
// (common/emusuni_fastmem.cpp): the same numbers and the same register slots in both.
#pragma once
#ifdef EMUSUNI_PORT
#include <csignal>
#include <cstddef>
#include <cstdint>
#if !defined(__ORBIS__)
#include <ucontext.h>
#endif

namespace EmuSuNi::Fault {
#if defined(__ORBIS__)
// The PS4's FreeBSD kernel, whatever a header says (as the Dreamcast's and the PSP's handlers have them, and
// port/ps4_crashlog.cpp: measured on the console): SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, si_addr 24 bytes into
// the siginfo, the registers 0x40 bytes into the context in mcontext order (rip the 21st 8-byte slot, rsp the
// 24th); the struct sigaction handed to the kernel is FreeBSD's (the PSChrome libc overlay's headers come first).
constexpr int kSigSegv = 11, kSigBus = 10, kSaSiginfo = 0x40;
constexpr std::size_t kSiAddr = 24;
static_assert(offsetof(struct sigaction, sa_flags) == 8 && sizeof(sigset_t) == 16,
              "struct sigaction isn't FreeBSD's: the PSChrome libc overlay's headers must come first");
inline std::uint64_t& Slot(void* context, int slot) {
    return reinterpret_cast<std::uint64_t*>(static_cast<char*>(context) + 0x40)[slot];
}
inline std::uint64_t& Rip(void* context) {
    return Slot(context, 20);
}
inline std::uint64_t& Rsp(void* context) {
    return Slot(context, 23);
}
#else
constexpr int kSigSegv = SIGSEGV, kSigBus = SIGBUS, kSaSiginfo = SA_SIGINFO;
constexpr std::size_t kSiAddr = offsetof(siginfo_t, si_addr);
inline std::uint64_t& Rip(void* context) {
    return reinterpret_cast<std::uint64_t&>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
}
inline std::uint64_t& Rsp(void* context) {
    return reinterpret_cast<std::uint64_t&>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
}
#endif
inline void* Address(const siginfo_t* info) {
    return *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(info) + kSiAddr);
}
} // namespace EmuSuNi::Fault

// dynarmic's fault handler (exception_handler_posix.cpp), for the fast memory (common/emusuni_fastmem.cpp): the
// JITs made while it is wanted install it (the handlers before kept), uninstall puts those back.
extern "C" void emusuni_dynarmic_fault_handler_wanted(bool wanted);
extern "C" bool emusuni_dynarmic_fault_handler_installed(void);
extern "C" void emusuni_dynarmic_fault_handler_uninstall(void);
// common/emusuni_fastmem.cpp: one fault of the JIT's turned into a call of its memory fallback (a count).
extern "C" void emusuni_3ds_fastmem_fault(void);
#endif
''')
addfile('src/common/emusuni_fastmem.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): fast memory for dynarmic, the ARM11's recompiler. A page table's guest 4 GiB
// as one reserved range of the address space, in which every 16 KiB group of guest pages that lies in order in one of
// the 3DS's memory blocks (common/emusuni_mem.cpp: FCRAM, VRAM, the New 3DS RAM, direct memory on the PS4) is mapped
// a second time (sceKernelMapDirectMemory into the range: direct memory aliasing, which the app turns on at its
// start). A recompiled load or store is then one instruction at the range's base plus the guest address, instead of a
// walk of the page table. Anything else (I/O, the pages the GPU cache watches, a group whose pages aren't in order)
// isn't mapped: its first access faults, and dynarmic's fault handler turns the fault into a call of that access's
// fallback and recompiles it with the page table (common/emusuni_fault.h). After the Dreamcast's fast memory
// (scripts/build-dc-core.sh: core/linux/emusuni_vmem.cpp): the same kernel calls, a self-test before the first use
// and the page table whenever anything is refused.
// On by default since 00.68; the file /data/EmuSuNi/3ds-fastmem-off (read at each game's start; the PC tests:
// EMUSUNI_3DS_FASTMEM=0) turns it off. A start that dies while trying it leaves /data/EmuSuNi/3ds-fastmem-trying
// (from the self-test until the first game has run a few seconds with it, or ended): the next start makes
// 3ds-fastmem-crashed of it and doesn't try again until that file is deleted (the log says so).
#pragma once
#ifdef EMUSUNI_PORT
#include <cstdint>

namespace EmuSuNi::Fastmem {
struct Arena;

// A game's: made first and destroyed last with its memory (Memory::MemorySystem). At the start: whether this game
// uses fast memory (the switch, the marker files, the self-test the first time); at the end: dynarmic's fault handler
// taken out again (the handlers before put back) and what the fast memory did, in the log.
struct Game {
    Game();
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
};

// This game uses fast memory.
bool On();

// A page table's range, made with its first JIT: nullptr when the fast memory is off or the system refuses (logged).
Arena* Create();
void Destroy(Arena* arena);
std::uintptr_t Base(const Arena* arena);
// The range follows the page table over pages [first_page, first_page + count) (raw: the table's host pointers).
void Sync(Arena* arena, std::uint8_t* const* raw, std::uint32_t first_page, std::uint32_t count);

// Once a frame (retro_run): the "trying" marker goes once the first game has run a while with the fast memory.
void FrameDone();
} // namespace EmuSuNi::Fastmem
#endif
''')
addfile('src/common/emusuni_fastmem.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): see emusuni_fastmem.h. After the Dreamcast's fast memory
// (scripts/build-dc-core.sh: core/linux/emusuni_vmem.cpp, itself Resonance's shell/ps4/ps4_vmem.cpp) and the
// 3ds-speed research's prototype (its Sync).
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "common/emusuni_dmem.h"
#include "common/emusuni_fastmem.h"
#include "common/emusuni_fault.h"
#include "common/emusuni_mem.h"
#include "common/logging/log.h"
#if !defined(__ORBIS__)
#include <fcntl.h>
#include <mutex>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

// ---- The PC tests' stand-ins for the PS4's calls (emusuni_dmem.h).
#if !defined(__ORBIS__)
namespace EmuSuNi::Dmem {
namespace {
constexpr int32_t kInval = static_cast<int32_t>(0x80020016), kNoMem = static_cast<int32_t>(0x8002000c),
                  kBusy = static_cast<int32_t>(0x80020010);
constexpr off_t kPool = off_t(16) << 30; // a sparse memfd
std::mutex g_shim;
int g_fd = -1;
off_t g_top = 0;
struct Range {
    off_t start;
    size_t length;
};
std::vector<Range> g_free; // given back: taken again first
struct Mapped {
    std::uintptr_t address;
    size_t length;
    off_t phys;
};
std::vector<Mapped> g_mapped;
long g_map_calls = 0;

bool Aligned(std::uintptr_t v, size_t a) {
    return (v & (a - 1)) == 0;
}

// Drops [address, address + length) from the records of what is mapped.
void Forget(std::uintptr_t address, size_t length) {
    std::vector<Mapped> kept;
    for (const Mapped& m : g_mapped) {
        if (m.address + m.length <= address || m.address >= address + length) {
            kept.push_back(m);
            continue;
        }
        if (m.address < address) {
            kept.push_back({m.address, address - m.address, m.phys});
        }
        if (m.address + m.length > address + length) {
            const size_t cut = address + length - m.address;
            kept.push_back({address + length, m.address + m.length - (address + length), m.phys + off_t(cut)});
        }
    }
    g_mapped.swap(kept);
}

bool AlreadyMapped(off_t phys, size_t length) {
    for (const Mapped& m : g_mapped) {
        if (m.phys < phys + off_t(length) && phys < m.phys + off_t(m.length)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool Injected(const char* what, long* value) {
    const char* list = std::getenv("EMUSUNI_3DS_SHIM_FAIL");
    if (list == nullptr) {
        return false;
    }
    const size_t length = std::strlen(what);
    for (const char* p = std::strstr(list, what); p != nullptr; p = std::strstr(p + 1, what)) {
        if ((p == list || p[-1] == ',') && (p[length] == '\0' || p[length] == ',' || p[length] == '=')) {
            if (value != nullptr) {
                *value = p[length] == '=' ? std::atol(p + length + 1) : 0;
            }
            return true;
        }
    }
    return false;
}

int32_t AllocateDirectMemory(off_t, off_t, size_t length, size_t alignment, int32_t, off_t* physicalOut) {
    std::lock_guard lock{g_shim};
    if (Injected("alloc")) {
        return kNoMem;
    }
    if (length == 0 || !Aligned(length, kPage) || alignment < kPage || (alignment & (alignment - 1)) != 0) {
        return kInval;
    }
    if (g_fd < 0) {
        g_fd = memfd_create("emusuni-3ds-dmem", 0);
        if (g_fd < 0 || ftruncate(g_fd, kPool) != 0) {
            return kNoMem;
        }
    }
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        const off_t start = (it->start + off_t(alignment) - 1) & ~off_t(alignment - 1);
        const off_t end = it->start + off_t(it->length);
        if (start + off_t(length) <= end) {
            const Range before{it->start, size_t(start - it->start)};
            const Range after{start + off_t(length), size_t(end - start - off_t(length))};
            g_free.erase(it);
            if (before.length != 0) {
                g_free.push_back(before);
            }
            if (after.length != 0) {
                g_free.push_back(after);
            }
            *physicalOut = start;
            return 0;
        }
    }
    const off_t start = (g_top + off_t(alignment) - 1) & ~off_t(alignment - 1);
    if (start + off_t(length) > kPool) {
        return kNoMem;
    }
    g_top = start + off_t(length);
    *physicalOut = start;
    return 0;
}

int32_t ReleaseDirectMemory(off_t start, size_t length) {
    std::lock_guard lock{g_shim};
    if (g_fd >= 0) {
        fallocate(g_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, start, off_t(length)); // the pages back
    }
    g_free.push_back({start, length});
    return 0;
}

int32_t MapDirectMemory(void** address, size_t length, int32_t protection, int32_t flags, off_t directMemoryStart,
                        size_t alignment) {
    std::lock_guard lock{g_shim};
    const long call = ++g_map_calls;
    long n = 0;
    if (Injected("map", &n) && n == call) {
        return kNoMem;
    }
    const bool fixed = (flags & kMapFixed) != 0;
    if (g_fd < 0 || length == 0 || !Aligned(length, kPage) || !Aligned(std::uintptr_t(directMemoryStart), kPage) ||
        alignment < kPage || (fixed && !Aligned(reinterpret_cast<std::uintptr_t>(*address), kPage))) {
        return kInval;
    }
    if (alignment > kPage && (!Aligned(std::uintptr_t(directMemoryStart), alignment) || !Aligned(length, alignment) ||
                              (fixed && !Aligned(reinterpret_cast<std::uintptr_t>(*address), alignment)))) {
        return kInval;
    }
    if (Injected("alias") && AlreadyMapped(directMemoryStart, length)) {
        return kBusy; // as the PS4 before aliasing is enabled
    }
    const int prot = (protection & 2) != 0 ? PROT_READ | PROT_WRITE : PROT_READ;
    void* p = mmap(*address, length, prot, MAP_SHARED | (fixed ? MAP_FIXED : 0), g_fd, directMemoryStart);
    if (p == MAP_FAILED) {
        return kNoMem;
    }
    Forget(reinterpret_cast<std::uintptr_t>(p), length);
    g_mapped.push_back({reinterpret_cast<std::uintptr_t>(p), length, directMemoryStart});
    *address = p;
    return 0;
}

int32_t ReserveVirtualRange(void** address, size_t length, int32_t flags, size_t alignment) {
    std::lock_guard lock{g_shim};
    if (Injected("reserve")) {
        return kNoMem;
    }
    if (length == 0 || !Aligned(length, kPage) || alignment < kPage || (alignment & (alignment - 1)) != 0) {
        return kInval;
    }
    constexpr int kAnon = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void* want = *address;
    if ((flags & kMapFixed) != 0) {
        void* p = mmap(want, length, PROT_NONE, kAnon | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) {
            return kNoMem;
        }
        Forget(reinterpret_cast<std::uintptr_t>(p), length);
        return 0;
    }
    if (want != nullptr) {
        // A hint that isn't free fails, as on the PS4 (the kernel doesn't look on).
        if (!Aligned(reinterpret_cast<std::uintptr_t>(want), alignment)) {
            return kInval;
        }
        void* p = mmap(want, length, PROT_NONE, kAnon | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) {
            return kNoMem;
        }
        if (p != want) {
            munmap(p, length);
            return kNoMem;
        }
        return 0;
    }
    // Anywhere, aligned: more than asked for, the ends given back.
    void* p = mmap(nullptr, length + alignment, PROT_NONE, kAnon, -1, 0);
    if (p == MAP_FAILED) {
        return kNoMem;
    }
    const std::uintptr_t low = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t start = (low + alignment - 1) & ~std::uintptr_t(alignment - 1);
    if (start > low) {
        munmap(p, start - low);
    }
    if (low + length + alignment > start + length) {
        munmap(reinterpret_cast<void*>(start + length), low + length + alignment - (start + length));
    }
    *address = reinterpret_cast<void*>(start);
    return 0;
}

int32_t Munmap(void* address, size_t length) {
    std::lock_guard lock{g_shim};
    Forget(reinterpret_cast<std::uintptr_t>(address), length);
    return munmap(address, length) == 0 ? 0 : kInval;
}

size_t GetDirectMemorySize() {
    return size_t(kPool);
}

int InternalMemoryGetAvailableSize(size_t* size) {
    *size = 0;
    return 0;
}
} // namespace EmuSuNi::Dmem
#endif

// ---- The self-test's probe: a load of the byte at rdi, then ret; and the call a fault in that load is turned into
// (as dynarmic turns a fast-memory fault into a call of the access's fallback): eax = 0x5a, ret to the probe's ret.
extern "C" unsigned emusuni_fastmem_probe(const volatile void* address);
extern "C" const char emusuni_fastmem_probe_resume[];
extern "C" const char emusuni_fastmem_probe_call[];
asm(".pushsection .text\n"
    ".p2align 4\n"
    ".globl emusuni_fastmem_probe\n"
    ".hidden emusuni_fastmem_probe\n"
    ".globl emusuni_fastmem_probe_resume\n"
    ".hidden emusuni_fastmem_probe_resume\n"
    ".globl emusuni_fastmem_probe_call\n"
    ".hidden emusuni_fastmem_probe_call\n"
    "emusuni_fastmem_probe:\n"
    "    movzbl (%rdi), %eax\n"
    "emusuni_fastmem_probe_resume:\n"
    "    ret\n"
    ".p2align 4\n"
    "emusuni_fastmem_probe_call:\n"
    "    movl $0x5a, %eax\n"
    "    ret\n"
    ".popsection\n");

namespace {
std::atomic<std::uint64_t> g_faults{0}; // the JIT's faults turned into calls, since the app started
} // namespace

extern "C" void emusuni_3ds_fastmem_fault(void) {
    g_faults.fetch_add(1, std::memory_order_relaxed); // (in the signal handler: a lock-free atomic)
}

namespace EmuSuNi::Fastmem {
namespace {
constexpr std::size_t kGuestPage = 4096;
constexpr std::size_t kGroup = Dmem::kPage;              // 16 KiB: the PS4's page
constexpr std::size_t kGroupPages = kGroup / kGuestPage; // 4 guest pages
constexpr std::size_t kSpace = std::size_t(1) << 32;     // the guest's address space
constexpr std::size_t kGuard = 64 * 1024;                // an access across its end faults there
constexpr std::size_t kGroups = kSpace / kGroup;
constexpr std::size_t kLarge = 2 * 1024 * 1024;
constexpr int kMaxArenas = 4;
// The kernel library puts its own objects at 0x200000000 and the app's heap is below 0x1800000000
// (port/ps4_runtime.cpp), the 3DS's memory blocks near 0x2600000000, the Dreamcast's range at 0x2800000000 to
// 0x3800000000 (not while a 3DS game runs): above all of them first, 16 GiB apart (a hint that isn't free fails and
// the next is tried), then between them, else anywhere above 0x400000000.
constexpr std::uintptr_t kHints[] = {0x4000000000ull, 0x4400000000ull, 0x4800000000ull, 0x4C00000000ull,
                                     0x5000000000ull, 0x2000000000ull, 0x1C00000000ull, 0x3000000000ull,
                                     0x3400000000ull, 0};
constexpr std::uintptr_t kNotBelow = 0x400000000ull;

std::mutex g_lock;  // Decide, End, Create, Destroy
std::mutex g_sync;  // Sync (the emulation thread's, and any other's)
bool g_on = false;  // this game
int g_tested = -1;  // the self-test: -1 not yet, 0 failed (g_why), 1 passed (for the app's life)
char g_why[240] = "";
bool g_said_off = false;
bool g_trying = false; // the marker file is there
std::uint64_t g_frames = 0;
std::uint64_t g_faults_at_start = 0;
int g_live = 0;
// This game's counts.
std::uint64_t g_syncs = 0, g_maps = 0, g_unmaps = 0, g_refused = 0;
bool g_said_cap = false;

struct ProbeState {
    volatile int armed = 0;
    volatile int faults = 0;
    volatile int signal = 0;
    volatile std::uintptr_t address = 0, rip = 0, rsp = 0;
    sigjmp_buf jump;
};
ProbeState g_probe;
} // namespace

struct Arena {
    std::uint8_t* base = nullptr;
    // Per 16 KiB group of the guest's: 0 nothing mapped there (reserved), else 1 + the direct memory offset mapped
    // there in 16 KiB units (consecutive groups of one run have consecutive values).
    std::vector<std::uint32_t> mapped;
    std::size_t groups_mapped = 0;
};

namespace {
std::size_t InternalFreeKiB() {
    size_t size = 0;
    sceKernelInternalMemoryGetAvailableSize(&size);
    return size / 1024;
}

bool MarkerPath(const char* name, char* path, std::size_t size) {
#if defined(__ORBIS__)
    std::snprintf(path, size, "/data/EmuSuNi/%s", name);
    return true;
#else
    const char* dir = std::getenv("EMUSUNI_3DS_MARKER_DIR");
    if (dir == nullptr) {
        return false;
    }
    std::snprintf(path, size, "%s/%s", dir, name);
    return true;
#endif
}
bool MarkerExists(const char* name) {
    char path[200];
    return MarkerPath(name, path, sizeof(path)) && ::access(path, F_OK) == 0;
}
void MarkerMake(const char* name) {
    char path[200];
    if (MarkerPath(name, path, sizeof(path))) {
        if (std::FILE* file = std::fopen(path, "wb")) {
            std::fclose(file);
        }
    }
}
void MarkerRemove(const char* name) {
    char path[200];
    if (MarkerPath(name, path, sizeof(path))) {
        std::remove(path);
    }
}

bool SwitchOn(char* how, std::size_t size) {
#if defined(__ORBIS__)
    const bool on = ::access("/data/EmuSuNi/3ds-fastmem", F_OK) == 0;
    std::snprintf(how, size, on ? "the file /data/EmuSuNi/3ds-fastmem" : "no file /data/EmuSuNi/3ds-fastmem");
    return on;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM");
    const bool on = value != nullptr && std::strcmp(value, "1") == 0;
    std::snprintf(how, size, on ? "EMUSUNI_3DS_FASTMEM=1" : "EMUSUNI_3DS_FASTMEM isn't 1");
    return on;
#endif
}

// One page table's 4 GiB and the guard, reserved (2 MiB aligned) at the first hint that is free.
int32_t ReserveSpace(void** out) {
    int32_t r = -1;
    for (const std::uintptr_t hint : kHints) {
        void* base = reinterpret_cast<void*>(hint);
        r = sceKernelReserveVirtualRange(&base, kSpace + kGuard, 0, kLarge);
        if (r == 0 && reinterpret_cast<std::uintptr_t>(base) >= kNotBelow) {
            *out = base;
            return 0;
        }
        if (r == 0) {
            sceKernelMunmap(base, kSpace + kGuard);
            r = -2;
        }
    }
    return r;
}

// Unmaps a range of a reservation and reserves it again, so nothing else lands there.
bool UnmapToReserved(void* address, std::size_t size) {
    sceKernelMunmap(address, size);
    void* at = address;
    const int32_t r = sceKernelReserveVirtualRange(&at, size, Dmem::kMapFixed, Dmem::kPage);
    if (r != 0 || at != address) {
        LOG_ERROR(Common, "EmuSuNi: fastmem: reserving {} again failed ({:#x})", address, static_cast<unsigned>(r));
        return false;
    }
    return true;
}

void ProbeHandler(int sig, siginfo_t* info, void* context) {
    if (g_probe.armed) {
        const int faults = g_probe.faults + 1;
        g_probe.faults = faults;
        if (faults == 1) {
            g_probe.signal = sig;
            g_probe.address = reinterpret_cast<std::uintptr_t>(Fault::Address(info));
            g_probe.rip = Fault::Rip(context);
            g_probe.rsp = Fault::Rsp(context);
#if !defined(__ORBIS__)
            if (Dmem::Injected("noredirect")) {
                return;
            }
#endif
            // What dynarmic's handler does with a fast-memory fault: a call of the access's fallback, which
            // returns after the load.
            Fault::Rsp(context) -= sizeof(std::uint64_t);
            *reinterpret_cast<std::uint64_t*>(Fault::Rsp(context)) =
                reinterpret_cast<std::uint64_t>(emusuni_fastmem_probe_resume);
            Fault::Rip(context) = reinterpret_cast<std::uint64_t>(emusuni_fastmem_probe_call);
            return;
        }
        siglongjmp(g_probe.jump, 1); // the load ran again: the changes to the context didn't take
    }
    // Not the test's (another thread's fault meanwhile): the default action, as without a handler.
    struct sigaction dfl {};
    sigaction(sig, &dfl, nullptr);
}

__attribute__((noinline)) unsigned RunProbe(const volatile void* address, bool* again, std::uintptr_t* frame) {
    *frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    g_probe.faults = 0;
    g_probe.armed = 1;
    volatile unsigned value = 0;
    if (sigsetjmp(g_probe.jump, 1) == 0) {
        value = emusuni_fastmem_probe(address);
    } else {
        *again = true;
    }
    g_probe.armed = 0;
    return value;
}

const char* SignalName(int sig) {
    return sig == Fault::kSigSegv ? "SIGSEGV" : sig == Fault::kSigBus ? "SIGBUS" : "?";
}

// What the fast memory needs of the system, tried once before its first use: the address space of a page table;
// direct memory mapped a second time at a fixed address of a reserved range (aliasing) is the same memory; a load
// from a reserved page that isn't mapped faults, the handler sees the address, the load's pc and the stack, and the
// fault turned into a call (the stack and pc changed in the context) returns where the load was. "" when all is
// well, else what isn't.
const char* SelfTest(char* detail, std::size_t detail_size) {
    static char why[240];
    void* space = nullptr;
    int32_t r = ReserveSpace(&space);
    if (r != 0) {
        std::snprintf(why, sizeof(why), "no 4 GiB of address space (%x)", static_cast<unsigned>(r));
        return why;
    }
    sceKernelMunmap(space, kSpace + kGuard);

    off_t phys = 0;
    r = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), Dmem::kPage, Dmem::kPage,
                                      Dmem::kWbOnion, &phys);
    if (r != 0) {
        std::snprintf(why, sizeof(why), "no page of direct memory for the self-test (%x)", static_cast<unsigned>(r));
        return why;
    }
    void* range = nullptr;
    r = ReserveSpace(&range); // (the probe's range: a page table's, as the JIT's will be)
    if (r != 0) {
        sceKernelReleaseDirectMemory(phys, Dmem::kPage);
        std::snprintf(why, sizeof(why), "no address space for the self-test (%x)", static_cast<unsigned>(r));
        return why;
    }
    auto* page = static_cast<std::uint8_t*>(range) + 0x10000000; // (a group well inside it)
    void* plain = reinterpret_cast<void*>(0x1f00200000ull);         // a hint: anywhere will do
    const int32_t r_plain = sceKernelMapDirectMemory(&plain, Dmem::kPage, Dmem::kProtRW, 0, phys, Dmem::kPage);
    void* fixed = page;
    const int32_t r_fixed =
        r_plain == 0 ? sceKernelMapDirectMemory(&fixed, Dmem::kPage, Dmem::kProtRW, Dmem::kMapFixed, phys, Dmem::kPage)
                     : -1;
    const char* result = "";
    if (r_plain != 0) {
        std::snprintf(why, sizeof(why), "the self-test's page of direct memory can't be mapped (%x)",
                      static_cast<unsigned>(r_plain));
        result = why;
    } else if (r_fixed != 0 || fixed != page) {
        std::snprintf(why, sizeof(why), "the kernel won't map direct memory a second time in a reserved range (%x)",
                      static_cast<unsigned>(r_fixed));
        result = why;
    } else {
        auto* a = static_cast<volatile std::uint8_t*>(plain);
        auto* b = static_cast<volatile std::uint8_t*>(fixed);
        a[0x123] = 0x3c;
        b[0x456] = 0xc3;
        if (b[0x123] != 0x3c || a[0x456] != 0xc3) {
            result = "two mappings of the same direct memory are not the same memory";
        }
    }
    if (r_fixed == 0 && fixed == page) {
        UnmapToReserved(page, Dmem::kPage);
    }
    if (result[0] == '\0') {
#if !defined(__ORBIS__)
        if (Dmem::Injected("nofault")) {
            void* again = page;
            sceKernelMapDirectMemory(&again, Dmem::kPage, Dmem::kProtRW, Dmem::kMapFixed, phys, Dmem::kPage);
        }
#endif
        struct sigaction act {}, old_segv {}, old_bus {}; // (an empty mask)
        act.sa_sigaction = ProbeHandler;
        act.sa_flags = Fault::kSaSiginfo;
        sigaction(Fault::kSigSegv, &act, &old_segv);
        sigaction(Fault::kSigBus, &act, &old_bus);
        const auto* target = page + 0x123;
        bool again = false;
        std::uintptr_t frame = 0;
        const unsigned value = RunProbe(target, &again, &frame);
        sigaction(Fault::kSigSegv, &old_segv, nullptr);
        sigaction(Fault::kSigBus, &old_bus, nullptr);
        const auto load = reinterpret_cast<std::uintptr_t>(&emusuni_fastmem_probe);
        if (again) {
            result = "the fault's context was changed and the load ran again anyway";
        } else if (g_probe.faults == 0) {
            result = "a load from a reserved page that isn't mapped didn't fault";
        } else if (g_probe.address != reinterpret_cast<std::uintptr_t>(target)) {
            std::snprintf(why, sizeof(why), "fault address %p, the load was from %p",
                          reinterpret_cast<void*>(g_probe.address), static_cast<const void*>(target));
            result = why;
        } else if (g_probe.rip != load) {
            std::snprintf(why, sizeof(why), "fault pc %p, the load is at %p", reinterpret_cast<void*>(g_probe.rip),
                          reinterpret_cast<void*>(load));
            result = why;
        } else if (g_probe.rsp + 4096 < frame || g_probe.rsp > frame + 4096) {
            std::snprintf(why, sizeof(why), "fault rsp %p, the stack is at %p", reinterpret_cast<void*>(g_probe.rsp),
                          reinterpret_cast<void*>(frame));
            result = why;
        } else if (value != 0x5a) {
            std::snprintf(why, sizeof(why), "the call the fault was turned into gave %#x, not 0x5a", value);
            result = why;
        } else {
            std::snprintf(detail, detail_size,
                          "aliasing; a load from an unmapped page faults (%s) with its address, pc and stack, and "
                          "turned into a call it returns",
                          SignalName(g_probe.signal));
        }
    }
    sceKernelMunmap(range, kSpace + kGuard);
    if (r_plain == 0) {
        sceKernelMunmap(plain, Dmem::kPage);
    }
    sceKernelReleaseDirectMemory(phys, Dmem::kPage);
    return result;
}

// This game's start: on or off, and why (the log's one line).
void Decide() {
    std::lock_guard lock{g_lock};
    g_on = false;
    g_frames = 0;
    g_faults_at_start = g_faults.load(std::memory_order_relaxed);
    g_syncs = g_maps = g_unmaps = g_refused = 0;
    g_said_cap = false;
    char how[96];
    if (!SwitchOn(how, sizeof(how))) {
        if (!g_said_off) {
            LOG_INFO(Common, "EmuSuNi: fastmem: off ({}): the page table", how);
            g_said_off = true;
        }
        emusuni_dynarmic_fault_handler_wanted(false);
        return;
    }
    if (g_tested < 0) {
        char path[200] = "";
        if (MarkerExists("3ds-fastmem-crashed")) {
            MarkerPath("3ds-fastmem-crashed", path, sizeof(path));
            std::snprintf(g_why, sizeof(g_why), "an earlier start died while trying it: delete the file %s to try again",
                          path);
            g_tested = 0;
        } else if (MarkerExists("3ds-fastmem-trying")) {
            MarkerMake("3ds-fastmem-crashed");
            MarkerRemove("3ds-fastmem-trying");
            MarkerPath("3ds-fastmem-crashed", path, sizeof(path));
            std::snprintf(g_why, sizeof(g_why),
                          "the start before died while trying it (noted in the file %s: delete it to try again)", path);
            g_tested = 0;
        } else {
            MarkerMake("3ds-fastmem-trying");
            g_trying = true;
#if !defined(__ORBIS__)
            if (Dmem::Injected("crash")) {
                std::raise(SIGKILL); // (the marker files' test)
            }
#endif
            const std::size_t internal = InternalFreeKiB();
            LOG_INFO(Common, "EmuSuNi: fastmem: trying it ({}; kernel memory {} KiB free)", how, internal);
            char detail[200] = "";
            const char* why = SelfTest(detail, sizeof(detail));
            if (why[0] != '\0') {
                std::snprintf(g_why, sizeof(g_why), "%s", why);
                MarkerRemove("3ds-fastmem-trying");
                g_trying = false;
                g_tested = 0;
            } else {
                g_tested = 1;
                LOG_INFO(Common, "EmuSuNi: fastmem: self-test ok: {}; kernel memory {} KiB free (was {})", detail,
                         InternalFreeKiB(), internal);
            }
        }
    }
    if (g_tested == 0) {
        LOG_WARNING(Common, "EmuSuNi: fastmem: off: {} (the page table instead)", g_why);
        emusuni_dynarmic_fault_handler_wanted(false);
        return;
    }
    g_on = true;
    emusuni_dynarmic_fault_handler_wanted(true);
    LOG_INFO(Common, "EmuSuNi: fastmem: on for this game ({})", how);
}

// This game's end (its JITs are gone): dynarmic's handler out again, and the counts.
void End() {
    std::lock_guard lock{g_lock};
    emusuni_dynarmic_fault_handler_uninstall();
    if (g_on) {
        LOG_INFO(Common,
                 "EmuSuNi: fastmem: this game: {} syncs, {} mappings and {} unmappings ({} refused), {} faults "
                 "turned into calls (each recompiled with the page table), {} frames; kernel memory {} KiB free",
                 g_syncs, g_maps, g_unmaps, g_refused, g_faults.load(std::memory_order_relaxed) - g_faults_at_start,
                 g_frames, InternalFreeKiB());
    }
    g_on = false;
}
} // namespace

Game::Game() {
    Decide();
}

Game::~Game() {
    End();
}

bool On() {
    return g_on;
}

Arena* Create() {
    std::lock_guard lock{g_lock};
    if (!g_on) {
        return nullptr;
    }
    if (g_live >= kMaxArenas) {
        if (!g_said_cap) {
            LOG_WARNING(Common, "EmuSuNi: fastmem: {} page tables have a range already: this one's JIT takes the "
                                "page table", g_live);
            g_said_cap = true;
        }
        return nullptr;
    }
    DirectBlock blocks[16];
    if (PagesDirectBlocks(blocks, 16) == 0) {
        if (!g_said_cap) {
            LOG_WARNING(Common, "EmuSuNi: fastmem: the 3DS's memory isn't in direct memory: the page table");
            g_said_cap = true;
        }
        return nullptr;
    }
    void* base = nullptr;
    const int32_t r = ReserveSpace(&base);
    if (r != 0) {
        LOG_WARNING(Common, "EmuSuNi: fastmem: no 4 GiB of address space for a page table ({:#x}): its JIT takes the "
                            "page table", static_cast<unsigned>(r));
        return nullptr;
    }
    auto* arena = new (std::nothrow) Arena;
    if (arena == nullptr) {
        sceKernelMunmap(base, kSpace + kGuard);
        return nullptr;
    }
    arena->base = static_cast<std::uint8_t*>(base);
    arena->mapped.assign(kGroups, 0);
    g_live++;
    LOG_INFO(Common, "EmuSuNi: fastmem: a page table's 4 GiB at {} (kernel memory {} KiB free)", base,
             InternalFreeKiB());
    return arena;
}

void Destroy(Arena* arena) {
    if (arena == nullptr) {
        return;
    }
    std::lock_guard lock{g_lock};
    LOG_INFO(Common, "EmuSuNi: fastmem: a page table's range given back ({} KiB were mapped)",
             arena->groups_mapped * kGroup / 1024);
    sceKernelMunmap(arena->base, kSpace + kGuard);
    delete arena;
    g_live--;
}

std::uintptr_t Base(const Arena* arena) {
    return reinterpret_cast<std::uintptr_t>(arena->base);
}

void Sync(Arena* arena, std::uint8_t* const* raw, std::uint32_t first_page, std::uint32_t count) {
    if (arena == nullptr || count == 0) {
        return;
    }
    std::lock_guard lock{g_sync};
    DirectBlock blocks[16];
    const int block_count = PagesDirectBlocks(blocks, 16);
    g_syncs++;
    const std::size_t g0 = first_page / kGroupPages;
    const std::size_t g1 = std::min<std::size_t>((std::size_t(first_page) + count + kGroupPages - 1) / kGroupPages,
                                                 kGroups);
    // Runs of groups that change the same way: [run_g, run_g + run_n) mapped to direct memory from run_first
    // (consecutive), or made a reservation again (run_first 0).
    std::size_t run_g = 0, run_n = 0;
    std::uint32_t run_first = 0;
    bool run_was_mapped = false;
    auto flush = [&]() {
        if (run_n == 0) {
            return;
        }
        std::uint8_t* address = arena->base + run_g * kGroup;
        const std::size_t length = run_n * kGroup;
        if (run_was_mapped) {
            UnmapToReserved(address, length);
            g_unmaps++;
        }
        bool mapped = false;
        if (run_first != 0) {
            void* at = address;
            const off_t phys = off_t(run_first - 1) * off_t(kGroup);
            const int32_t r = sceKernelMapDirectMemory(&at, length, Dmem::kProtRW, Dmem::kMapFixed, phys, Dmem::kPage);
            g_maps++;
            mapped = r == 0 && at == address;
            if (!mapped) {
                if (r == 0) {
                    UnmapToReserved(at, length);
                }
                if (g_refused++ < 8) {
                    LOG_WARNING(Common, "EmuSuNi: fastmem: mapping {} KiB at guest {:#x} refused ({:#x}): that range "
                                        "takes the page table", length / 1024, run_g * kGroup,
                                static_cast<unsigned>(r));
                }
            }
        }
        for (std::size_t i = 0; i < run_n; i++) {
            std::uint32_t& slot = arena->mapped[run_g + i];
            arena->groups_mapped -= slot != 0 ? 1 : 0;
            slot = mapped ? run_first + std::uint32_t(i) : 0;
            arena->groups_mapped += slot != 0 ? 1 : 0;
        }
        run_n = 0;
    };
    for (std::size_t group = g0; group < g1; group++) {
        // What this group should be: the direct memory behind its four pages when they lie in order in one block,
        // from a whole group's offset in it; else nothing.
        const std::size_t page = group * kGroupPages;
        const std::uint8_t* p = raw[page];
        std::uint32_t want = 0;
        if (p != nullptr) {
            for (int i = 0; i < block_count; i++) {
                const auto* at = static_cast<const std::uint8_t*>(blocks[i].at);
                if (p < at || p >= at + blocks[i].size) {
                    continue;
                }
                const std::size_t offset = std::size_t(p - at);
                if (offset % kGroup == 0 && offset + kGroup <= blocks[i].size && raw[page + 1] == p + kGuestPage &&
                    raw[page + 2] == p + 2 * kGuestPage && raw[page + 3] == p + 3 * kGuestPage) {
                    want = std::uint32_t((std::uint64_t(blocks[i].phys) + offset) / kGroup) + 1;
                }
                break;
            }
        }
        const std::uint32_t have = arena->mapped[group];
        if (want == have) {
            flush();
            continue;
        }
        const bool extends = run_n > 0 && ((want == 0 && run_first == 0) ||
                                           (want != 0 && run_first != 0 && want == run_first + run_n));
        if (!extends) {
            flush();
            run_g = group;
            run_first = want;
            run_was_mapped = false;
        }
        run_was_mapped |= have != 0;
        run_n++;
    }
    flush();
}

void FrameDone() {
    if (!g_on) {
        return;
    }
    g_frames++;
    if (!g_trying) {
        return;
    }
    const std::uint64_t faults = g_faults.load(std::memory_order_relaxed) - g_faults_at_start;
    if ((g_frames >= 600 && faults >= 64) || g_frames >= 3600) {
        MarkerRemove("3ds-fastmem-trying");
        g_trying = false;
        LOG_INFO(Common, "EmuSuNi: fastmem: {} frames and {} faults turned into calls: it works here (the start "
                         "marker is gone)", g_frames, faults);
    }
}
} // namespace EmuSuNi::Fastmem
#endif
''')
# dynarmic's POSIX fault handler on the PS4 too (upstream builds the generic one, which has no fast memory, for
# CMake's "Generic" system), changed for EmuSuNi: installed only while a game uses fast memory, taken out again at its
# end, no alternate signal stack, SIGBUS too, the PS4's numbers and register slots (common/emusuni_fault.h). Without
# fast memory nothing is installed, as before.
patch('externals/dynarmic/src/dynarmic/CMakeLists.txt',
      'elseif (UNIX)\n    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")\n',
      'elseif (UNIX OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too (the fast memory, common/emusuni_fastmem.h)\n'
      '    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      '#include "dynarmic/backend/exception_handler.h"\n',
      '#include "dynarmic/backend/exception_handler.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#    include <algorithm>\n'
      '#    include "../../../../../src/common/emusuni_fault.h"\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      '#    ifndef __OpenBSD__\n#        include <ucontext.h>\n#    endif\n',
      '#    if !defined(__OpenBSD__) && !(defined(EMUSUNI_PORT) && defined(__ORBIS__))\n'
      '#        include <ucontext.h>\n'
      '#    endif\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      'namespace Dynarmic::Backend {\n\nnamespace {\n',
      r'''namespace Dynarmic::Backend {

namespace {

#if defined(EMUSUNI_PORT) && defined(MCL_ARCHITECTURE_X86_64)
// EmuSuNi (scripts/build-3ds-core.sh): the fault handler of the fast memory (common/emusuni_fastmem.h). Installed
// when a JIT registers while the core wants it (emusuni_dynarmic_fault_handler_wanted: a game with fast memory), taken
// out again at the game's end (emusuni_dynarmic_fault_handler_uninstall), the handlers before put back, as the other
// cores' handlers do (the Dreamcast's, the PSP's): the chain is the running core's handler, then the app's crash log.
// No alternate signal stack (the app gives its threads none), SIGBUS as well as SIGSEGV, the PS4's numbers and
// register slots (common/emusuni_fault.h). Without fast memory nothing is installed (as the generic handler before).
struct CodeBlockInfo {
    u64 code_begin, code_end;
    std::function<FakeCall(u64)> cb;
};

class SigHandler {
public:
    void AddCodeBlock(CodeBlockInfo info);
    void RemoveCodeBlock(u64 host_pc);
    bool SupportsFastmem() const { return installed; }
    void Install();
    void Uninstall();

    auto FindCodeBlockInfo(u64 host_pc) {
        return std::find_if(code_block_infos.begin(), code_block_infos.end(), [&](const auto& x) { return x.code_begin <= host_pc && x.code_end > host_pc; });
    }

    bool installed = false;
    std::vector<CodeBlockInfo> code_block_infos;
    std::mutex code_block_infos_mutex;
    struct sigaction old_sa_segv {};
    struct sigaction old_sa_bus {};

    static void SigAction(int sig, siginfo_t* info, void* raw_context);
};

std::mutex handler_lock;
std::optional<SigHandler> sig_handler;
bool handler_wanted = false;

void RegisterHandler() {
    std::lock_guard<std::mutex> guard(handler_lock);
    if (!sig_handler) {
        sig_handler.emplace();
    }
    if (handler_wanted) {
        sig_handler->Install();
    }
}

void SigHandler::Install() {
    if (installed) {
        return;
    }
    struct sigaction sa {};  // (an empty mask)
    sa.sa_sigaction = &SigHandler::SigAction;
    sa.sa_flags = ::EmuSuNi::Fault::kSaSiginfo;
    if (sigaction(::EmuSuNi::Fault::kSigSegv, &sa, &old_sa_segv) != 0) {
        fmt::print(stderr, "dynarmic: EmuSuNi: could not set the SIGSEGV handler\n");
        return;
    }
    if (sigaction(::EmuSuNi::Fault::kSigBus, &sa, &old_sa_bus) != 0) {
        sigaction(::EmuSuNi::Fault::kSigSegv, &old_sa_segv, nullptr);
        fmt::print(stderr, "dynarmic: EmuSuNi: could not set the SIGBUS handler\n");
        return;
    }
    installed = true;
}

void SigHandler::Uninstall() {
    if (!installed) {
        return;
    }
    sigaction(::EmuSuNi::Fault::kSigSegv, &old_sa_segv, nullptr);
    sigaction(::EmuSuNi::Fault::kSigBus, &old_sa_bus, nullptr);
    installed = false;
}

void SigHandler::AddCodeBlock(CodeBlockInfo cbi) {
    std::lock_guard<std::mutex> guard(code_block_infos_mutex);
    if (auto iter = FindCodeBlockInfo(cbi.code_begin); iter != code_block_infos.end()) {
        code_block_infos.erase(iter);
    }
    code_block_infos.push_back(cbi);
}

void SigHandler::RemoveCodeBlock(u64 host_pc) {
    std::lock_guard<std::mutex> guard(code_block_infos_mutex);
    const auto iter = FindCodeBlockInfo(host_pc);
    if (iter == code_block_infos.end()) {
        return;
    }
    code_block_infos.erase(iter);
}

void SigHandler::SigAction(int sig, siginfo_t* info, void* raw_context) {
    {
        std::lock_guard<std::mutex> guard(sig_handler->code_block_infos_mutex);
        const u64 rip = ::EmuSuNi::Fault::Rip(raw_context);
        const auto iter = sig_handler->FindCodeBlockInfo(rip);
        if (iter != sig_handler->code_block_infos.end()) {
            // A fast-memory access that faulted: a call of its fallback (through the page table), which returns
            // after the access; dynarmic recompiles that access with the page table.
            const FakeCall fc = iter->cb(rip);
            u64& rsp = ::EmuSuNi::Fault::Rsp(raw_context);
            rsp -= sizeof(u64);
            *mcl::bit_cast<u64*>(rsp) = fc.ret_rip;
            ::EmuSuNi::Fault::Rip(raw_context) = fc.call_rip;
            emusuni_3ds_fastmem_fault();
            return;
        }
    }
    // Not the JIT's: the handler before (the app's crash log), as the kernel would have called it.
    struct sigaction* retry_sa = sig == ::EmuSuNi::Fault::kSigSegv ? &sig_handler->old_sa_segv : &sig_handler->old_sa_bus;
    if (retry_sa->sa_flags & ::EmuSuNi::Fault::kSaSiginfo) {
        retry_sa->sa_sigaction(sig, info, raw_context);
        return;
    }
    if (retry_sa->sa_handler == SIG_IGN) {
        return;
    }
    if (retry_sa->sa_handler == SIG_DFL) {
        struct sigaction dfl {};  // the default action: the fault again, without a handler
        sigaction(sig, &dfl, nullptr);
        return;
    }
    retry_sa->sa_handler(sig);
}
#else
''')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      '}  // anonymous namespace\n',
      '#endif  // EMUSUNI_PORT\n}  // anonymous namespace\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      'void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {\n    impl->SetCallback(cb);\n}\n',
      r'''void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {
    impl->SetCallback(cb);
}

#if defined(EMUSUNI_PORT) && defined(MCL_ARCHITECTURE_X86_64)
// EmuSuNi: the fast memory's switches of the handler (common/emusuni_fault.h).
extern "C" void emusuni_dynarmic_fault_handler_wanted(bool wanted) {
    std::lock_guard<std::mutex> guard(handler_lock);
    handler_wanted = wanted;
}

extern "C" bool emusuni_dynarmic_fault_handler_installed(void) {
    std::lock_guard<std::mutex> guard(handler_lock);
    return sig_handler && sig_handler->installed;
}

extern "C" void emusuni_dynarmic_fault_handler_uninstall(void) {
    std::lock_guard<std::mutex> guard(handler_lock);
    handler_wanted = false;
    if (sig_handler) {
        sig_handler->Uninstall();
    }
}
#endif
''')

# The page table keeps its range in step: every change of its pointers (the mappings, the pages the GPU cache marks
# and unmarks, the watchpoints, a save state's load, Clear) syncs the pages it changed, in runs.
patch('src/core/memory.h',
      '#include "common/swap.h"\n',
      '#include "common/swap.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <utility>\n'
      '#include <vector>\n'
      'namespace EmuSuNi::Fastmem {\n'
      'struct Arena;\n'
      '}\n'
      '#endif\n')
patch('src/core/memory.h',
      '    std::unordered_map<VAddr, WatchpointPageInfo> watchpoint_pages_map{};\n\n    void Clear();\n',
      '    std::unordered_map<VAddr, WatchpointPageInfo> watchpoint_pages_map{};\n\n    void Clear();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the fast memory\'s range of this address space (common/emusuni_fastmem.h), made with its first\n'
      '    // JIT, kept in step with the pointers.\n'
      '    EmuSuNi::Fastmem::Arena* fastmem = nullptr;\n'
      '    void FastmemSync(u32 first_page, u32 count);\n'
      '    // Pages of several tables, synced in runs (sorts `pages`).\n'
      '    static void FastmemSyncPages(std::vector<std::pair<PageTable*, u32>>& pages);\n'
      '    PageTable() = default;\n'
      '    PageTable(const PageTable&) = delete;\n'
      '    PageTable& operator=(const PageTable&) = delete;\n'
      '    ~PageTable();\n'
      '#endif\n')
patch('src/core/memory.h',
      '        for (std::size_t i = 0; i < PAGE_TABLE_NUM_ENTRIES; i++) {\n'
      '            pointers.raw[i] = pointers.refs[i].GetPtr();\n'
      '        }\n    }\n',
      '        for (std::size_t i = 0; i < PAGE_TABLE_NUM_ENTRIES; i++) {\n'
      '            pointers.raw[i] = pointers.refs[i].GetPtr();\n'
      '        }\n'
      '#ifdef EMUSUNI_PORT\n'
      '        FastmemSync(0, PAGE_TABLE_NUM_ENTRIES); // (a save state\'s load)\n'
      '#endif\n'
      '    }\n')
patch('src/core/memory.cpp',
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mem.h"\n#endif\n',
      '#ifdef EMUSUNI_PORT\n#include <algorithm>\n#include <functional>\n#include "common/emusuni_fastmem.h"\n'
      '#include "common/emusuni_mem.h"\n#endif\n')
patch('src/core/memory.cpp',
      'void PageTable::Clear() {\n    pointers.raw.fill(nullptr);\n    pointers.refs.fill(MemoryRef());\n'
      '    attributes.fill(PageType::Unmapped);\n}\n',
      r'''void PageTable::Clear() {
    pointers.raw.fill(nullptr);
    pointers.refs.fill(MemoryRef());
    attributes.fill(PageType::Unmapped);
#ifdef EMUSUNI_PORT
    FastmemSync(0, PAGE_TABLE_NUM_ENTRIES);
#endif
}
#ifdef EMUSUNI_PORT
void PageTable::FastmemSync(u32 first_page, u32 count) {
    if (fastmem != nullptr) {
        EmuSuNi::Fastmem::Sync(fastmem, pointers.raw.data(), first_page, count);
    }
}

void PageTable::FastmemSyncPages(std::vector<std::pair<PageTable*, u32>>& pages) {
    std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? std::less<PageTable*>{}(a.first, b.first) : a.second < b.second;
    });
    for (std::size_t i = 0; i < pages.size();) {
        PageTable* const table = pages[i].first;
        const u32 first = pages[i].second;
        u32 last = first;
        std::size_t j = i + 1;
        while (j < pages.size() && pages[j].first == table && pages[j].second <= last + 1) {
            last = std::max(last, pages[j].second);
            j++;
        }
        table->FastmemSync(first, last - first + 1);
        i = j;
    }
}

PageTable::~PageTable() {
    EmuSuNi::Fastmem::Destroy(fastmem);
}
#endif
''')
patch('src/core/memory.cpp',
      '    // EmuSuNi: pages of their own, off the app\'s heap (direct memory on the PS4; common/emusuni_mem.h).\n'
      '    EmuSuNi::Pages fcram = EmuSuNi::MakePages(Memory::FCRAM_N3DS_SIZE);\n',
      '    // EmuSuNi: made first and destroyed last: whether this game uses fast memory, decided before its memory is\n'
      '    // allocated (common/emusuni_fastmem.h).\n'
      '    EmuSuNi::Fastmem::Game emusuni_fastmem_game;\n'
      '    // EmuSuNi: pages of their own, off the app\'s heap (direct memory on the PS4; common/emusuni_mem.h).\n'
      '    EmuSuNi::Pages fcram = EmuSuNi::MakePages(Memory::FCRAM_N3DS_SIZE);\n')
patch('src/core/memory.cpp',
      '    u32 end = base + size;\n    while (base != end) {\n',
      '    u32 end = base + size;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const u32 emusuni_first = base;\n'
      '#endif\n'
      '    while (base != end) {\n')
patch('src/core/memory.cpp',
      '        base += 1;\n        if (memory != nullptr && memory.GetSize() > CITRA_PAGE_SIZE)\n'
      '            memory += CITRA_PAGE_SIZE;\n    }\n}\n',
      '        base += 1;\n        if (memory != nullptr && memory.GetSize() > CITRA_PAGE_SIZE)\n'
      '            memory += CITRA_PAGE_SIZE;\n    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    page_table.FastmemSync(emusuni_first, size);\n'
      '#endif\n'
      '}\n')
patch('src/core/memory.cpp',
      'void MemorySystem::RasterizerMarkRegionCached(PAddr start, u32 size, bool cached) {\n'
      '    if (start == 0) {\n        return;\n    }\n',
      'void MemorySystem::RasterizerMarkRegionCached(PAddr start, u32 size, bool cached) {\n'
      '    if (start == 0) {\n        return;\n    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the pages whose pointers this changes, for the fast memory to follow in runs at the end.\n'
      '    std::vector<std::pair<PageTable*, u32>> emusuni_changed;\n'
      '#endif\n')
patch('src/core/memory.cpp',
      '                        page_table->pointers[vaddr >> CITRA_PAGE_BITS] = nullptr;\n                        break;\n',
      '                        page_table->pointers[vaddr >> CITRA_PAGE_BITS] = nullptr;\n'
      '#ifdef EMUSUNI_PORT\n'
      '                        if (page_table->fastmem != nullptr) {\n'
      '                            emusuni_changed.emplace_back(page_table.get(), vaddr >> CITRA_PAGE_BITS);\n'
      '                        }\n'
      '#endif\n'
      '                        break;\n')
patch('src/core/memory.cpp',
      '                            page_table->pointers[vaddr >> CITRA_PAGE_BITS] =\n'
      '                                GetPointerForRasterizerCache(vaddr & ~CITRA_PAGE_MASK);\n',
      '                            page_table->pointers[vaddr >> CITRA_PAGE_BITS] =\n'
      '                                GetPointerForRasterizerCache(vaddr & ~CITRA_PAGE_MASK);\n'
      '#ifdef EMUSUNI_PORT\n'
      '                            if (page_table->fastmem != nullptr) {\n'
      '                                emusuni_changed.emplace_back(page_table.get(), vaddr >> CITRA_PAGE_BITS);\n'
      '                            }\n'
      '#endif\n')
patch('src/core/memory.cpp',
      '    }\n}\n\nu8 MemorySystem::Read8(const VAddr addr) {\n',
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    PageTable::FastmemSyncPages(emusuni_changed);\n'
      '#endif\n'
      '}\n\nu8 MemorySystem::Read8(const VAddr addr) {\n')
patch('src/core/memory.cpp',
      '                type = PageType::MemoryWatchpoint;\n                page_table.pointers[page_index] = nullptr;\n'
      '                break;\n',
      '                type = PageType::MemoryWatchpoint;\n                page_table.pointers[page_index] = nullptr;\n'
      '#ifdef EMUSUNI_PORT\n'
      '                page_table.FastmemSync(page_index, 1);\n'
      '#endif\n'
      '                break;\n')
patch('src/core/memory.cpp',
      '                    type = PageType::Memory;\n                    page_table.pointers[page_index] = it->second.memory;\n'
      '                    break;\n',
      '                    type = PageType::Memory;\n                    page_table.pointers[page_index] = it->second.memory;\n'
      '#ifdef EMUSUNI_PORT\n'
      '                    page_table.FastmemSync(page_index, 1);\n'
      '#endif\n'
      '                    break;\n')
# Each JIT of a page table: the range's base as dynarmic's fast memory pointer (its loads and stores at base + the
# guest address; the page table stays for the accesses that fault).
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/settings.h"\n',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/emusuni_fastmem.h"\n#include "common/settings.h"\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    config.code_cache_size = Settings::values.is_new_3ds.GetValue() ? 12u << 20 : 24u << 20;\n#endif\n',
      '    config.code_cache_size = Settings::values.is_new_3ds.GetValue() ? 12u << 20 : 24u << 20;\n'
      '    // EmuSuNi: fast memory (common/emusuni_fastmem.h): the page table\'s range, made with its first JIT; the\n'
      '    // JIT installs dynarmic\'s fault handler as it registers.\n'
      '    if (current_page_table && EmuSuNi::Fastmem::On()) {\n'
      '        if (current_page_table->fastmem == nullptr) {\n'
      '            current_page_table->fastmem = EmuSuNi::Fastmem::Create();\n'
      '            current_page_table->FastmemSync(0, Memory::PAGE_TABLE_NUM_ENTRIES);\n'
      '        }\n'
      '        if (current_page_table->fastmem != nullptr) {\n'
      '            config.fastmem_pointer = EmuSuNi::Fastmem::Base(current_page_table->fastmem);\n'
      '        }\n'
      '    }\n'
      '#endif\n')
# Once a frame: the fast memory's start marker goes once the first game has run a while with it.
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "citra_libretro/citra_libretro.h"\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include "common/emusuni_stats.h"\n#endif\n',
      '#include "citra_libretro/citra_libretro.h"\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include "common/emusuni_fastmem.h"\n'
      '#include "common/emusuni_stats.h"\n#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    // EmuSuNi: where a slow frame\'s time went (common/emusuni_stats.h), and a summary every 600 frames.\n',
      '    EmuSuNi::Fastmem::FrameDone(); // EmuSuNi: (common/emusuni_fastmem.h)\n'
      '    // EmuSuNi: where a slow frame\'s time went (common/emusuni_stats.h), and a summary every 600 frames.\n')

# --- With the fast memory, a process's code, data and stack at the guest address's 16 KiB phase in FCRAM: the
# FCRAM behind them (what the loader maps when the process starts) is taken at the same 16 KiB phase as the guest
# address it is mapped at, so that those ranges are mapped by whole 16 KiB pages. Upstream takes the top of the
# highest free block whatever its phase: then a 16 KiB group of guest pages straddles two 16 KiB pages of FCRAM and
# the fast memory can't map it (Super Mario 3D Land: its code, data and stack). The space the phase leaves above a
# block (less than 16 KiB) is charged to the process, used and freed with it: a game that asks for every free byte of
# its region (Super Mario 3D Land's linear heap) must not find the region fragmented, nor its commit limit off. Heap
# commits (svcControlMemory) keep upstream's allocator: they can be freed and committed again, and a charged gap would
# leak each time. Without the fast memory, upstream's allocation as before.
patch('src/core/hle/kernel/memory.h',
      '    IntervalSet HeapAllocate(u32 size);\n',
      '    IntervalSet HeapAllocate(u32 size);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: one interval whose start is `phase` modulo `align` (a power of two), the space left above it\n'
      '    /// in its free block (less than `align`) taken too and returned in `gap`; else as HeapAllocate.\n'
      '    IntervalSet HeapAllocateAligned(u32 size, u32 phase, u32 align, IntervalSet& gap);\n'
      '#endif\n')
patch('src/core/hle/kernel/memory.cpp',
      'bool MemoryRegionInfo::LinearAllocate(u32 offset, u32 size) {\n',
      r'''#ifdef EMUSUNI_PORT
MemoryRegionInfo::IntervalSet MemoryRegionInfo::HeapAllocateAligned(u32 size, u32 phase, u32 align,
                                                                      IntervalSet& gap) {
    ASSERT(!is_locked);
    // From the higher addresses, as HeapAllocate: the highest start in a free block with the right phase.
    for (auto iter = free_blocks.rbegin(); iter != free_blocks.rend(); ++iter) {
        const u32 lower = iter->lower();
        const u32 upper = iter->upper();
        if (upper - lower < size) {
            continue;
        }
        u32 start = ((upper - size) & ~(align - 1)) + phase;
        if (start > upper - size) {
            if (start < align) {
                continue;
            }
            start -= align;
        }
        if (start < lower) {
            continue;
        }
        IntervalSet result;
        result += Interval(start, start + size);
        free_blocks -= Interval(start, upper);
        used += upper - start;
        if (start + size < upper) {
            gap += Interval(start + size, upper);
        }
        return result;
    }
    return HeapAllocate(size);
}
#endif

bool MemoryRegionInfo::LinearAllocate(u32 offset, u32 size) {
''')
patch('src/core/hle/kernel/process.h',
      '    MemoryRegionInfo::IntervalSet holding_memory;\n',
      '    MemoryRegionInfo::IntervalSet holding_memory;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: while the loader maps the code, data and stack (Run), with the fast memory: FCRAM at the guest\n'
      '    /// address\'s 16 KiB phase.\n'
      '    bool emusuni_phase_aligned = false;\n'
      '#endif\n')
patch('src/core/hle/kernel/process.cpp',
      '#include "common/serialization/boost_vector.hpp"\n',
      '#include "common/serialization/boost_vector.hpp"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_fastmem.h"\n#endif\n')
patch('src/core/hle/kernel/process.cpp',
      '    auto allocated_fcram = memory_region->HeapAllocate(size);\n',
      r'''#ifdef EMUSUNI_PORT
    // EmuSuNi: the loader's mappings at the guest address's 16 KiB phase (the fast memory maps 16 KiB pages on the
    // PS4); the space that leaves above them is the process's, freed with it.
    MemoryRegionInfo::IntervalSet emusuni_gap;
    auto allocated_fcram = emusuni_phase_aligned
                               ? memory_region->HeapAllocateAligned(size, target & 0x3FFF, 0x4000, emusuni_gap)
                               : memory_region->HeapAllocate(size);
    // (The gap counts as the process's memory everywhere the kernel counts it: its intervals, its use and its commit
    // limit, all given back by the process's cleanup, which frees holding_memory interval by interval.)
    holding_memory += emusuni_gap;
    for (const auto& gap : emusuni_gap) {
        memory_used += gap.upper() - gap.lower();
        resource_limit->Reserve(ResourceLimitType::Commit, gap.upper() - gap.lower());
    }
#else
    auto allocated_fcram = memory_region->HeapAllocate(size);
#endif
''')
patch('src/core/hle/kernel/process.cpp',
      '    // Map CodeSet segments\n',
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_phase_aligned = EmuSuNi::Fastmem::On();\n'
      '#endif\n'
      '    // Map CodeSet segments\n')
patch('src/core/hle/kernel/process.cpp',
      '    HeapAllocate(std::addressof(out_addr), Memory::HEAP_VADDR_END - stack_size, stack_size,\n'
      '                 VMAPermission::ReadWrite, MemoryState::Locked, true);\n',
      '    HeapAllocate(std::addressof(out_addr), Memory::HEAP_VADDR_END - stack_size, stack_size,\n'
      '                 VMAPermission::ReadWrite, MemoryState::Locked, true);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_phase_aligned = false;\n'
      '#endif\n')

# --- 00.68: where the emulation thread's own time goes, in the stall log. 00.67's log (Super Mario 3D Land on the
# PS4) had 35 frames of 100-683 ms in the game's loads with no pipeline, shader, texture, file or GPU wait behind them:
# the emulation thread's own work, which the log didn't divide. Now each frame counts, with the CPU's time stamp
# counter (cheap enough for every HLE call; its rate measured against the clock over the frame itself): the guest's
# code (dynarmic's run), the JIT compiling it (blocks, and the flushes of its whole cache), the HLE kernel and
# services (SVCs), the GPU's commands (GSP's, PICA's; with the surfaces made for them) and the audio DSP's frames, and
# the draws skipped while their pipeline compiles (async shaders). common/emusuni_frame_stats.h writes the lines.
addfile('src/common/emusuni_frame_stats.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the log's account of the emulation thread's time (citra_libretro.cpp's
// retro_run makes one of these for every frame): a frame of more than 100 ms logs "EmuSuNi: slow frame ...", and every
// 600 frames one line sums them up ("EmuSuNi: 600 frames in ..."), from common/emusuni_stats.h's counters. Of the
// emulation thread's own work: the guest's code (the ARM11's, recompiled), dynarmic compiling it (blocks, and the
// flushes of its whole cache), the HLE kernel and services, the GPU's commands (PICA, with the surfaces made for them)
// and the audio DSP; then what it waited for and what other threads did.
#pragma once
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstdlib>
#include <string>
#include <fmt/format.h>
#include "common/emusuni_stats.h"
#include "common/logging/log.h"

namespace EmuSuNi::Stats {
struct FrameStats {
    std::uint64_t t0 = NowUs();
    std::uint64_t k0 = Ticks();
    std::uint64_t at[Count];
    std::uint64_t ticks_at[TickCount];
    std::uint64_t counts_at[CountCount];

    FrameStats() {
        on_emulation_thread = true;
        for (int i = 0; i < Count; i++) {
            at[i] = counters[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < TickCount; i++) {
            ticks_at[i] = ticks[i];
        }
        for (int i = 0; i < CountCount; i++) {
            counts_at[i] = counts[i];
        }
    }

    // d: the counters' differences, dt: the ticks', dc: the counts'; ms of wall time; tpu: ticks per microsecond.
    static std::string Describe(const std::uint64_t* d, const std::uint64_t* dt, const std::uint64_t* dc,
                                std::uint64_t ms, double tpu) {
        const auto t_ms = [tpu](std::uint64_t t) -> std::uint64_t {
            return tpu > 0 ? static_cast<std::uint64_t>(static_cast<double>(t) / tpu / 1000.0) : 0;
        };
        // (Inside the guest's run: its code, the JIT's compiling and the HLE calls; the GPU's commands mostly come
        // through an HLE call, GSP's, and are taken out of it.)
        const std::uint64_t jit = dt[JitTicks], svc = dt[SvcTicks], gpu_in_svc = dt[GpuInSvcTicks];
        const std::uint64_t guest = dt[GuestTicks] > jit + svc ? dt[GuestTicks] - jit - svc : 0;
        const std::uint64_t hle = svc > gpu_in_svc ? svc - gpu_in_svc : 0;
        return fmt::format("{} ms: guest code {} ms, JIT compiling {} ms ({} blocks, {} cache flushes), HLE {} ms ({} "
                           "calls), GPU commands {} ms (surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "
                           "side; pipelines {} here ({} ms) and {} on the workers ({} ms); shaders {} here ({} ms) and {} "
                           "on the workers ({} ms); draws skipped {}; textures {} KiB ({} ms); files read {} KiB here "
                           "({} ms) and {} KiB elsewhere ({} ms)",
                           ms, t_ms(guest), t_ms(jit), dc[JitBlocks], dc[JitFlushes], t_ms(hle), dc[SvcCalls],
                           t_ms(dt[GpuTicks]), dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),
                           d[WaitUs] / 1000, d[PipelinesEmu], d[PipelineUsEmu] / 1000, d[PipelinesOther],
                           d[PipelineUsOther] / 1000, d[ShadersEmu], d[ShaderUsEmu] / 1000, d[ShadersOther],
                           d[ShaderUsOther] / 1000, dc[DrawsSkipped], d[UploadBytes] / 1024, d[UploadUs] / 1000,
                           d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024, d[ReadUsOther] / 1000);
    }

    ~FrameStats() {
        static std::uint64_t period_at[Count], period_ticks[TickCount], period_counts[CountCount];
        static std::uint64_t period_t0 = t0, period_k0 = k0;
        static int frames = 0;
        static bool first = true;
        if (first) {
            // (The first period starts with the first frame, not with what the game's load counted.)
            for (int i = 0; i < Count; i++) {
                period_at[i] = at[i];
            }
            for (int i = 0; i < TickCount; i++) {
                period_ticks[i] = ticks_at[i];
            }
            for (int i = 0; i < CountCount; i++) {
                period_counts[i] = counts_at[i];
            }
            first = false;
        }
        const std::uint64_t now_us = NowUs(), now_k = Ticks();
        const std::uint64_t ms = (now_us - t0) / 1000;
        std::uint64_t d[Count], dt[TickCount], dc[CountCount];
        // (100 ms, or EMUSUNI_3DS_SLOW_MS on the PC tests)
        static const std::uint64_t slow_ms =
            std::getenv("EMUSUNI_3DS_SLOW_MS") ? std::strtoull(std::getenv("EMUSUNI_3DS_SLOW_MS"), nullptr, 10) : 100;
        if (ms >= slow_ms) {
            for (int i = 0; i < Count; i++) {
                d[i] = counters[i].load(std::memory_order_relaxed) - at[i];
            }
            for (int i = 0; i < TickCount; i++) {
                dt[i] = ticks[i] - ticks_at[i];
            }
            for (int i = 0; i < CountCount; i++) {
                dc[i] = counts[i] - counts_at[i];
            }
            const double tpu = now_us > t0 ? static_cast<double>(now_k - k0) / static_cast<double>(now_us - t0) : 0;
            LOG_WARNING(Frontend, "EmuSuNi: slow frame {}", Describe(d, dt, dc, ms, tpu));
        }
        if (++frames == 600) {
            for (int i = 0; i < Count; i++) {
                const std::uint64_t now = counters[i].load(std::memory_order_relaxed);
                d[i] = now - period_at[i];
                period_at[i] = now;
            }
            for (int i = 0; i < TickCount; i++) {
                dt[i] = ticks[i] - period_ticks[i];
                period_ticks[i] = ticks[i];
            }
            for (int i = 0; i < CountCount; i++) {
                dc[i] = counts[i] - period_counts[i];
                period_counts[i] = counts[i];
            }
            const double tpu = now_us > period_t0 ? static_cast<double>(now_k - period_k0) /
                                                        static_cast<double>(now_us - period_t0)
                                                  : 0;
            LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, dt, dc, (now_us - period_t0) / 1000, tpu));
            period_t0 = now_us;
            period_k0 = now_k;
            frames = 0;
        }
    }
};
} // namespace EmuSuNi::Stats
#endif
''')
patch('src/common/emusuni_stats.h',
      '} // namespace EmuSuNi::Stats\n#endif\n',
      r'''
// The emulation thread's own work, in ticks of the CPU's time stamp counter (read at every HLE call, so cheaper than
// the clock: a system call on the PS4). Written and read on that thread only (retro_run's), so plain counters;
// common/emusuni_frame_stats.h turns ticks into ms with the counter's rate measured over the frame.
enum TickCounter { GuestTicks, JitTicks, SvcTicks, GpuTicks, GpuInSvcTicks, DspTicks, SurfaceTicks, TickCount };
enum CountCounter { JitBlocks, JitFlushes, SvcCalls, Surfaces, DrawsSkipped, CountCount };
inline std::uint64_t ticks[TickCount];
inline std::uint64_t counts[CountCount];
inline thread_local bool in_svc = false;
#if defined(__ORBIS__)
extern "C" std::uint64_t sceKernelReadTsc(void);
inline std::uint64_t Ticks() {
    return sceKernelReadTsc();
}
#else
inline std::uint64_t Ticks() {
    return __builtin_ia32_rdtsc();
}
#endif
struct TickTimer {
    TickCounter counter;
    std::uint64_t k0 = Ticks();
    explicit TickTimer(TickCounter counter_) : counter(counter_) {}
    ~TickTimer() {
        ticks[counter] += Ticks() - k0;
    }
};
// An HLE call (SVC): its time, and "inside an HLE call" for the GPU's commands that come through one.
struct SvcTimer {
    std::uint64_t k0 = Ticks();
    SvcTimer() {
        counts[SvcCalls]++;
        in_svc = true;
    }
    ~SvcTimer() {
        in_svc = false;
        ticks[SvcTicks] += Ticks() - k0;
    }
};
struct GpuTimer {
    std::uint64_t k0 = Ticks();
    bool svc = in_svc;
    ~GpuTimer() {
        const std::uint64_t d = Ticks() - k0;
        ticks[GpuTicks] += d;
        if (svc) {
            ticks[GpuInSvcTicks] += d;
        }
    }
};
} // namespace EmuSuNi::Stats
#endif
''')
patch('src/citra_libretro/citra_libretro.cpp',
      r'''    struct FrameStats {
        std::uint64_t t0 = EmuSuNi::Stats::NowUs();
        std::uint64_t at[EmuSuNi::Stats::Count];
        FrameStats() {
            EmuSuNi::Stats::on_emulation_thread = true;
            for (int i = 0; i < EmuSuNi::Stats::Count; i++)
                at[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);
        }
        static std::string Describe(const std::uint64_t* d, std::uint64_t ms) {
            using namespace EmuSuNi::Stats;
            return fmt::format("{} ms: waited {} ms for the GPU side; pipelines {} here ({} ms) and {} on the "
                               "workers ({} ms); shaders {} here ({} ms) and {} on the workers ({} ms); "
                               "textures {} KiB ({} ms); files read {} KiB here ({} ms) and {} KiB elsewhere ({} ms)",
                               ms, d[WaitUs] / 1000, d[PipelinesEmu], d[PipelineUsEmu] / 1000, d[PipelinesOther],
                               d[PipelineUsOther] / 1000, d[ShadersEmu], d[ShaderUsEmu] / 1000, d[ShadersOther],
                               d[ShaderUsOther] / 1000, d[UploadBytes] / 1024, d[UploadUs] / 1000,
                               d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024,
                               d[ReadUsOther] / 1000);
        }
        ~FrameStats() {
            static std::uint64_t period_at[EmuSuNi::Stats::Count];
            static std::uint64_t period_t0 = t0;
            static int frames = 0;
            const std::uint64_t ms = (EmuSuNi::Stats::NowUs() - t0) / 1000;
            std::uint64_t d[EmuSuNi::Stats::Count];
            // (100 ms, or EMUSUNI_3DS_SLOW_MS on the PC tests)
            static const std::uint64_t slow_ms =
                std::getenv("EMUSUNI_3DS_SLOW_MS") ? std::strtoull(std::getenv("EMUSUNI_3DS_SLOW_MS"), nullptr, 10) : 100;
            if (ms >= slow_ms) {
                for (int i = 0; i < EmuSuNi::Stats::Count; i++)
                    d[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed) - at[i];
                LOG_WARNING(Frontend, "EmuSuNi: slow frame {}", Describe(d, ms));
            }
            if (++frames == 600) {
                for (int i = 0; i < EmuSuNi::Stats::Count; i++) {
                    const std::uint64_t now = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);
                    d[i] = now - period_at[i];
                    period_at[i] = now;
                }
                LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, (EmuSuNi::Stats::NowUs() - period_t0) / 1000));
                period_t0 = EmuSuNi::Stats::NowUs();
                frames = 0;
            }
        }
    } emusuni_frame_stats;
''',
      '    EmuSuNi::Stats::FrameStats emusuni_frame_stats; // (common/emusuni_frame_stats.h)\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "common/emusuni_fastmem.h"\n#include "common/emusuni_stats.h"\n#endif\n',
      '#include "common/emusuni_fastmem.h"\n#include "common/emusuni_frame_stats.h"\n#include "common/emusuni_stats.h"\n'
      '#endif\n')
# The guest's run (its code, the JIT's compiling and the HLE calls made from it).
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/emusuni_fastmem.h"\n#include "common/settings.h"\n',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/emusuni_fastmem.h"\n#include "common/emusuni_stats.h"\n'
      '#include "common/settings.h"\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    if (break_flag) [[unlikely]] {\n        return;\n    }\n\n    jit->Run();\n}\n',
      '    if (break_flag) [[unlikely]] {\n        return;\n    }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::GuestTicks};\n'
      '#endif\n'
      '    jit->Run();\n}\n')
# The HLE calls.
patch('src/core/hle/kernel/svc.cpp',
      '#include "common/scm_rev.h"\n',
      '#include "common/scm_rev.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/core/hle/kernel/svc.cpp',
      'void SVC::CallSVC(u32 immediate) {\n    MICROPROFILE_SCOPE(Kernel_SVC);\n',
      'void SVC::CallSVC(u32 immediate) {\n    MICROPROFILE_SCOPE(Kernel_SVC);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::SvcTimer emusuni_timer;\n'
      '#endif\n')
# The GPU's commands (GSP's: PICA command lists, transfers, fills).
patch('src/video_core/gpu.cpp',
      '#include "common/hacks/hack_manager.h"\n',
      '#include "common/hacks/hack_manager.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/gpu.cpp',
      'void GPU::Execute(const Service::GSP::Command& command) {\n',
      'void GPU::Execute(const Service::GSP::Command& command) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::GpuTimer emusuni_timer;\n'
      '#endif\n')
# The audio DSP's frames (every 4.9 ms of the game's time).
patch('src/audio_core/hle/hle.cpp',
      '#include "audio_core/sink.h"\n',
      '#include "audio_core/sink.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/audio_core/hle/hle.cpp',
      'void DspHle::Impl::AudioTickCallback(s64 cycles_late) {\n',
      'void DspHle::Impl::AudioTickCallback(s64 cycles_late) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::DspTicks};\n'
      '#endif\n')
# The surfaces the rasterizer cache makes (a texture or render target the GPU hasn't had yet).
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '        return slot_surfaces.insert(runtime, params, initial_flags);\n    }();\n',
      '#ifdef EMUSUNI_PORT\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::SurfaceTicks};\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::Surfaces]++;\n'
      '#endif\n'
      '        return slot_surfaces.insert(runtime, params, initial_flags);\n    }();\n')
# The draws skipped while their pipeline compiles (async shaders).
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#include "video_core/texture/texture_decode.h"\n',
      '#include "video_core/texture/texture_decode.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    if (!pipeline_cache.BindPipeline(pipeline_info, wait_built)) {\n        return true;\n    }\n',
      '    if (!pipeline_cache.BindPipeline(pipeline_info, wait_built)) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::DrawsSkipped]++;\n'
      '#endif\n'
      '        return true;\n    }\n')
# dynarmic compiling a block (and the flushes of its whole cache: full, or a cache clear). Its IR verification pass,
# which only asserts (a map of every instruction's uses, for every block compiled), is left out: compiling is the
# loads' cost on the PS4.
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '#include "dynarmic/common/atomic.h"\n',
      '#include "dynarmic/common/atomic.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#    include "../../../../../../src/common/emusuni_stats.h"\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '        auto block = emitter.GetBasicBlock(descriptor);\n        if (block)\n            return *block;\n',
      '        auto block = emitter.GetBasicBlock(descriptor);\n        if (block)\n            return *block;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const ::EmuSuNi::Stats::TickTimer emusuni_timer{::EmuSuNi::Stats::JitTicks};\n'
      '        ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitBlocks]++;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '        Optimization::VerificationPass(ir_block);\n',
      '#ifndef EMUSUNI_PORT\n'
      '        Optimization::VerificationPass(ir_block);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '            if (invalidate_entire_cache) {\n                block_of_code.ClearCache();\n',
      '            if (invalidate_entire_cache) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitFlushes]++;\n'
      '#endif\n'
      '                block_of_code.ClearCache();\n')

# --- 00.68: asynchronous shaders by default. 00.67's log: Super Mario 3D Land at full speed once loaded, but stopped
# for 0.26-0.38 s whenever a new effect needed a pipeline the driver's cache hadn't (three times in four minutes), the
# whole game waiting for it. Now such a draw is skipped until its pipeline is built on the workers (an object shows a
# few frames late), as Azahar does on Android; draws of 6 vertices or fewer still wait (Azahar's rule: the 2D parts).
# The file /data/EmuSuNi/3ds-sync-shaders (or EMUSUNI_3DS_SYNC_SHADERS=1 on the PC) waits for every pipeline as before.
patch('src/citra_libretro/core_settings.cpp',
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for, with the file\n'
      '    // /data/EmuSuNi/3ds-async-shaders (or EMUSUNI_3DS_ASYNC_SHADERS=1 on the PC).\n'
      '    Settings::values.async_shader_compilation = std::getenv("EMUSUNI_3DS_ASYNC_SHADERS") != nullptr ||\n'
      '                                                ::access("/data/EmuSuNi/3ds-async-shaders", F_OK) == 0;\n',
      r'''    // EmuSuNi: a draw whose pipeline isn't built yet is skipped rather than waited for (the object shows a few
    // frames late instead of the game stopping), by default since 00.68; the file /data/EmuSuNi/3ds-sync-shaders (or
    // EMUSUNI_3DS_SYNC_SHADERS=1 on the PC) waits for every pipeline as before.
    Settings::values.async_shader_compilation = std::getenv("EMUSUNI_3DS_SYNC_SHADERS") == nullptr &&
                                                ::access("/data/EmuSuNi/3ds-sync-shaders", F_OK) != 0;
    {
        static int said = -1; // (said once, and again if it changes)
        const int now = Settings::values.async_shader_compilation.GetValue() ? 1 : 0;
        if (now != said) {
            LOG_INFO(Frontend, "EmuSuNi: shaders: {}",
                     now ? "a draw whose pipeline isn't built yet is skipped until it is (async; the file "
                           "/data/EmuSuNi/3ds-sync-shaders waits instead)"
                         : "every draw waits for its pipeline (the file /data/EmuSuNi/3ds-sync-shaders)");
            said = now;
        }
    }
''')
# The PC tests only: pipelines that take as long to compile as the PS4's.
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '#include "common/hash.h"\n',
      '#include "common/hash.h"\n'
      '#if defined(EMUSUNI_PORT) && !defined(__ORBIS__)\n#include <chrono>\n#include <cstdlib>\n#include <thread>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n'
      '#endif\n',
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n'
      '#endif\n'
      '#if defined(EMUSUNI_PORT) && !defined(__ORBIS__)\n'
      '    // EmuSuNi, the PC tests only: EMUSUNI_3DS_TEST_PIPELINE_MS=<n> makes each compile n ms longer, as long as the\n'
      '    // PS4\'s (0.25-0.5 s), to see what a game does meanwhile (async shaders\' pop-in, the stalls).\n'
      '    if (!fail_on_compile_required) {\n'
      '        static const int emusuni_test_ms = std::getenv("EMUSUNI_3DS_TEST_PIPELINE_MS")\n'
      '                                               ? std::atoi(std::getenv("EMUSUNI_3DS_TEST_PIPELINE_MS"))\n'
      '                                               : 0;\n'
      '        if (emusuni_test_ms > 0) {\n'
      '            std::this_thread::sleep_for(std::chrono::milliseconds(emusuni_test_ms));\n'
      '        }\n'
      '    }\n'
      '#endif\n')

# --- 00.68: the audio's resampling without the deque's indexing. AudioInterp::Linear (2.7% of the emulation thread in
# Super Mario 3D Land on the PC, the first of the audio's) put the two samples kept from the last call in front of the
# deque and indexed it three times a sample; now the same steps run over a contiguous copy of the samples the call can
# reach. The output is the same bit for bit (the PC test: the hash of every sample over a scene).
patch('src/audio_core/interpolate.cpp',
      '#include "common/assert.h"\n',
      '#include "common/assert.h"\n#ifdef EMUSUNI_PORT\n#include <vector>\n#endif\n')
patch('src/audio_core/interpolate.cpp',
      '    if (input.empty())\n        return;\n\n    input.insert(input.begin(), {state.xn2, state.xn1});\n',
      r'''    if (input.empty())
        return;

#ifdef EMUSUNI_PORT
    {
        // EmuSuNi: upstream's steps (below) over a contiguous copy: [xn2, xn1, the samples this call can reach].
        const u64 step_size = static_cast<u64>(rate * scale_factor);
        u64 fposition = state.fposition;
        const std::size_t total = input.size() + 2; // the samples upstream's loop sees
        const std::size_t steps = output.size() - outputi;
        const u64 last = fposition + step_size * (steps > 0 ? steps - 1 : 0);
        const std::size_t reach = std::min<std::size_t>(total, static_cast<std::size_t>(last / scale_factor) + 3);
        thread_local std::vector<std::array<s16, 2>> buffer;
        buffer.resize(reach);
        buffer[0] = state.xn2;
        buffer[1] = state.xn1;
        std::copy_n(input.begin(), reach - 2, buffer.begin() + 2);
        std::size_t inputi = 0;
        while (outputi < output.size()) {
            inputi = static_cast<std::size_t>(fposition / scale_factor);
            if (inputi + 2 >= total) {
                inputi = total - 2;
                break;
            }
            const u64 fraction = fposition & scale_mask;
            output[outputi++] = fn(fraction, buffer[inputi], buffer[inputi + 1], buffer[inputi + 2]);
            fposition += step_size;
        }
        state.xn2 = buffer[inputi];
        state.xn1 = buffer[inputi + 1];
        state.fposition = fposition - inputi * scale_factor;
        input.erase(input.begin(), std::next(input.begin(), inputi));
        return;
    }
#endif
    input.insert(input.begin(), {state.xn2, state.xn1});
''')

# --- 00.68: the fast memory on by default (00.66-00.67: only with the file /data/EmuSuNi/3ds-fastmem); the file
# /data/EmuSuNi/3ds-fastmem-off (or EMUSUNI_3DS_FASTMEM=0 on the PC) turns it off. The self-test, the fallbacks and the
# marker files as before; the "trying" marker now goes once the first game has run 120 frames with 64 of the JIT's
# faults turned into calls (or 1800 frames), or when that game ends normally: a session closed early is no crash.
patch('src/common/emusuni_fastmem.cpp',
      r'''bool SwitchOn(char* how, std::size_t size) {
#if defined(__ORBIS__)
    const bool on = ::access("/data/EmuSuNi/3ds-fastmem", F_OK) == 0;
    std::snprintf(how, size, on ? "the file /data/EmuSuNi/3ds-fastmem" : "no file /data/EmuSuNi/3ds-fastmem");
    return on;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM");
    const bool on = value != nullptr && std::strcmp(value, "1") == 0;
    std::snprintf(how, size, on ? "EMUSUNI_3DS_FASTMEM=1" : "EMUSUNI_3DS_FASTMEM isn't 1");
    return on;
#endif
}
''',
      r'''bool SwitchOn(char* how, std::size_t size) {
#if defined(__ORBIS__)
    const bool off = ::access("/data/EmuSuNi/3ds-fastmem-off", F_OK) == 0;
    std::snprintf(how, size, off ? "the file /data/EmuSuNi/3ds-fastmem-off" : "by default");
    return !off;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM");
    const bool off = value != nullptr && std::strcmp(value, "0") == 0;
    std::snprintf(how, size, off ? "EMUSUNI_3DS_FASTMEM=0" : "by default");
    return !off;
#endif
}
''')
patch('src/common/emusuni_fastmem.cpp',
      '    if ((g_frames >= 600 && faults >= 64) || g_frames >= 3600) {\n',
      '    if ((g_frames >= 120 && faults >= 64) || g_frames >= 1800) {\n')
patch('src/common/emusuni_fastmem.cpp',
      '    std::lock_guard lock{g_lock};\n    emusuni_dynarmic_fault_handler_uninstall();\n',
      '    std::lock_guard lock{g_lock};\n    emusuni_dynarmic_fault_handler_uninstall();\n'
      '    if (g_on && g_trying) {\n'
      '        // (The game ended normally before the frames above: no crash while trying it.)\n'
      '        MarkerRemove("3ds-fastmem-trying");\n'
      '        g_trying = false;\n'
      '    }\n')


# --- 00.69: dynarmic compiles a block faster, the same code. The 00.68 log (the PS4): the slow frames of the games'
# loads and new areas are dynarmic compiling the ARM11's code for the first time - Super Mario 3D Land's first 600
# frames 61,324 blocks in 5.1 s (83 us a block; this PC: 11 us). Each change leaves the x86 code it writes as it was:
#  - The ARM decoder searched the VFP table (62 patterns) and the ASIMD table (147, NEON: the 3DS's ARM11 has none)
#    one pattern after another for every instruction before the fast table of the ARM ones. Every VFP pattern has
#    bits 27-26 = 11 and 11-9 = 101 and every ASIMD pattern the top nibble 1111 (checked against vfp.inc and asimd.inc
#    of this tree): a table is searched only when its patterns can match.
#  - The register allocator released all 102 of its places (16 registers, 16 XMM, 6 flags, 64 spill slots) after
#    every IR instruction; now only the ones used since the last release (releasing is idempotent: the rest are as
#    the release left them).
#  - The IR's memory pool took room for 4096 instructions (about 400 KiB from the app's heap) for every block; now 256
#    at a time, more when a block needs them.
#  - The perf map's name of every block (a formatted string) only when the perf map is on (PERF_BUILDID_DIR, Linux).
patch('externals/dynarmic/src/dynarmic/frontend/A32/translate/translate_arm.cpp',
      '            if (const auto vfp_decoder = DecodeVFP<TranslatorVisitor>(*arm_instruction)) {\n'
      '                should_continue = vfp_decoder->get().call(visitor, *arm_instruction);\n'
      '            } else if (const auto asimd_decoder = DecodeASIMD<TranslatorVisitor>(*arm_instruction)) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi: the VFP and ASIMD tables only where their patterns can match (scripts/build-3ds-core.sh).\n'
      '            if (const auto vfp_decoder = (*arm_instruction & 0x0C000E00) == 0x0C000A00\n'
      '                                             ? DecodeVFP<TranslatorVisitor>(*arm_instruction)\n'
      '                                             : std::nullopt) {\n'
      '                should_continue = vfp_decoder->get().call(visitor, *arm_instruction);\n'
      '            } else if (const auto asimd_decoder = (*arm_instruction & 0xF0000000) == 0xF0000000\n'
      '                                                      ? DecodeASIMD<TranslatorVisitor>(*arm_instruction)\n'
      '                                                      : std::nullopt) {\n'
      '#else\n'
      '            if (const auto vfp_decoder = DecodeVFP<TranslatorVisitor>(*arm_instruction)) {\n'
      '                should_continue = vfp_decoder->get().call(visitor, *arm_instruction);\n'
      '            } else if (const auto asimd_decoder = DecodeASIMD<TranslatorVisitor>(*arm_instruction)) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    std::vector<HostLocInfo> hostloc_info;\n    HostLocInfo& LocInfo(HostLoc loc);\n',
      '    std::vector<HostLocInfo> hostloc_info;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the places LocInfo handed out for change since the last EndOfAllocScope (bit = HostLoc).\n'
      '    std::uint64_t emusuni_used[2] = {0, 0};\n'
      '#endif\n'
      '    HostLocInfo& LocInfo(HostLoc loc);\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::EndOfAllocScope() {\n    for (auto& iter : hostloc_info) {\n        iter.ReleaseAll();\n    }\n}\n',
      'void RegAlloc::EndOfAllocScope() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: only the places used since the last release (ReleaseAll is idempotent: the others are as it left\n'
      '    // them; every change goes through the non-const LocInfo).\n'
      '    static_assert(NonSpillHostLocCount + SpillCount <= 128);\n'
      '    for (size_t word = 0; word < 2; word++) {\n'
      '        std::uint64_t bits = emusuni_used[word];\n'
      '        emusuni_used[word] = 0;\n'
      '        while (bits != 0) {\n'
      '            hostloc_info[word * 64 + static_cast<size_t>(__builtin_ctzll(bits))].ReleaseAll();\n'
      '            bits &= bits - 1;\n'
      '        }\n'
      '    }\n'
      '#else\n'
      '    for (auto& iter : hostloc_info) {\n        iter.ReleaseAll();\n    }\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLocInfo& RegAlloc::LocInfo(HostLoc loc) {\n'
      '    ASSERT(loc != HostLoc::RSP && loc != HostLoc::R15);\n'
      '    return hostloc_info[static_cast<size_t>(loc)];\n}\n',
      'HostLocInfo& RegAlloc::LocInfo(HostLoc loc) {\n'
      '    ASSERT(loc != HostLoc::RSP && loc != HostLoc::R15);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const size_t index = static_cast<size_t>(loc);\n'
      '    emusuni_used[index >> 6] |= std::uint64_t{1} << (index & 63);\n'
      '    return hostloc_info[index];\n'
      '#else\n'
      '    return hostloc_info[static_cast<size_t>(loc)];\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/ir/basic_block.cpp',
      '        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 4096)} {}\n',
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: room for 256 instructions at a time (4096 took ~400 KiB of the heap for every block).\n'
      '        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 256)} {}\n'
      '#else\n'
      '        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 4096)} {}\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp',
      '#include <iterator>\n',
      '#include <iterator>\n#ifdef EMUSUNI_PORT\n#include <stdlib.h>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp',
      '    PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n'
      '    Patch(descriptor, entrypoint);\n',
      '#ifdef EMUSUNI_PORT\n'
      '#ifdef __linux__\n'
      '    // EmuSuNi: the block\'s name only when the perf map is on (its file is written only then; elsewhere\n'
      '    // PerfMapRegister does nothing).\n'
      '    static const bool emusuni_perf_map = ::getenv("PERF_BUILDID_DIR") != nullptr;\n'
      '    if (emusuni_perf_map) {\n'
      '        PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n'
      '    }\n'
      '#endif\n'
      '#else\n'
      '    PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n'
      '#endif\n'
      '    Patch(descriptor, entrypoint);\n')

# --- 00.69: dynarmic's register allocator without its assertion scans. dynarmic's ASSERTs are always on (mcl), and
# three of them scanned all 102 places for every IR instruction: DefineValue's "not defined yet" (ValueLocation of a
# value that is nowhere: the whole scan, with a search of every place's list) and AssertNoMoreUses at every block's
# end; GetArgumentInfo and DefineValue looked the same value up twice (once in the assertion). The checks are of
# dynarmic's own invariants (the same code is written); the lookups that remain are kept.
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '            ASSERT_MSG(ValueLocation(arg.GetInst()), "argument must already been defined");\n'
      '            LocInfo(*ValueLocation(arg.GetInst())).AddArgReference();\n',
      '#ifdef EMUSUNI_PORT\n'
      '            const auto emusuni_location = ValueLocation(arg.GetInst());  // EmuSuNi: looked up once\n'
      '            ASSERT_MSG(emusuni_location, "argument must already been defined");\n'
      '            LocInfo(*emusuni_location).AddArgReference();\n'
      '#else\n'
      '            ASSERT_MSG(ValueLocation(arg.GetInst()), "argument must already been defined");\n'
      '            LocInfo(*ValueLocation(arg.GetInst())).AddArgReference();\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, HostLoc host_loc) {\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, HostLoc host_loc) {\n'
      '#ifndef EMUSUNI_PORT  // EmuSuNi: a scan of every place for every value defined\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, const IR::Value& use_inst) {\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, const IR::Value& use_inst) {\n'
      '#ifndef EMUSUNI_PORT  // EmuSuNi: a scan of every place for every value defined\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    ASSERT_MSG(ValueLocation(use_inst.GetInst()), "use_inst must already be defined");\n'
      '    const HostLoc location = *ValueLocation(use_inst.GetInst());\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const auto emusuni_location = ValueLocation(use_inst.GetInst());  // EmuSuNi: looked up once\n'
      '    ASSERT_MSG(emusuni_location, "use_inst must already be defined");\n'
      '    const HostLoc location = *emusuni_location;\n'
      '#else\n'
      '    ASSERT_MSG(ValueLocation(use_inst.GetInst()), "use_inst must already be defined");\n'
      '    const HostLoc location = *ValueLocation(use_inst.GetInst());\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::AssertNoMoreUses() {\n'
      '    ASSERT(std::all_of(hostloc_info.begin(), hostloc_info.end(), [](const auto& i) { return i.IsEmpty(); }));\n'
      '}\n',
      'void RegAlloc::AssertNoMoreUses() {\n'
      '#ifndef EMUSUNI_PORT  // EmuSuNi: a scan of every place at every block\'s end\n'
      '    ASSERT(std::all_of(hostloc_info.begin(), hostloc_info.end(), [](const auto& i) { return i.IsEmpty(); }));\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    ASSERT(IsValueLive(inst) || !inst->HasUses());\n',
      '    ASSERT(!inst->HasUses() || IsValueLive(inst));  // (EmuSuNi: the cheap test first; the same check)\n')
# The memory accesses' "no fast memory here" markers (each instruction that faulted once): a hash set instead of a
# tree (looked up for every load and store compiled; thousands of entries in a game).
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '    std::set<DoNotFastmemMarker> do_not_fastmem;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    struct EmuSuNiMarkerHash {\n'
      '        size_t operator()(const DoNotFastmemMarker& m) const noexcept {\n'
      '            return std::hash<u64>{}(std::get<0>(m).Value() * 0x9E3779B97F4A7C15ull ^ std::get<1>(m));\n'
      '        }\n'
      '    };\n'
      '    tsl::robin_set<DoNotFastmemMarker, EmuSuNiMarkerHash> do_not_fastmem;  // EmuSuNi: a hash set\n'
      '#else\n'
      '    std::set<DoNotFastmemMarker> do_not_fastmem;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '#include <tsl/robin_map.h>\n',
      '#include <tsl/robin_map.h>\n#ifdef EMUSUNI_PORT\n#include <tsl/robin_set.h>\n#endif\n')

# --- 00.69: dynarmic compiles a block with fewer allocations, the same code. This PC counted 55 allocations of the
# heap for every block compiled (68 in all, the decoder's tables at the first block included: 650,000 once); on the
# PS4 each malloc/free pair takes the app's heap mutex. The ones that went:
#  - the register allocator copied its candidate registers into a new vector for every register it chose (6 a block)
#    and made a one-element vector for every fixed register (a host call: one per caller-saved register): a span of
#    them, the candidates copied into a stack array (the same partitions, so the same registers);
#  - its 102 places in an array instead of a heap vector, its register orders as spans of the emitters' (any_xmm is
#    a namespace-scope initializer_list: static storage; the A32 order a member), and the
#    A32 emitter's register order made once, not for every block;
#  - each place's list of values with room for 4 inside (a small vector);
#  - the identity removal pass's list of removed instructions, the same;
#  - room for 8 deferred emits from the start (the vector grew 2-3 times a block);
#  - the IR pool's slab kept for the next block on the same thread (one slab, its usual size only);
#  - the ARM decoder's 4096 buckets hold pointers to the matchers instead of copies of them (each copy a std::function
#    allocation: 650,000 at the first block of the first game);
#  - the blocks' ranges (for invalidation) in buckets of 4 KiB of the guest's addresses instead of an interval map of
#    sets (whose every insertion allocated 3 times and split intervals): the same locations found for a range.
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '#include <optional>\n#include <utility>\n#include <vector>\n',
      '#include <optional>\n#include <utility>\n#include <vector>\n'
      '#ifdef EMUSUNI_PORT\n#include <span>\n#include <boost/container/small_vector.hpp>\n'
      '#include "dynarmic/backend/x64/stack_layout.h"\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    // Value state\n    std::vector<IR::Inst*> values;\n',
      '    // Value state\n'
      '#ifdef EMUSUNI_PORT\n'
      '    boost::container::small_vector<IR::Inst*, 4> values;  // EmuSuNi: room for 4 inside\n'
      '#else\n'
      '    std::vector<IR::Inst*> values;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    explicit RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the orders as spans (the emitters\' vectors and any_xmm, a namespace-scope initializer_list,\n'
      '    // outlive every RegAlloc they make).\n'
      '    explicit RegAlloc(BlockOfCode& code, std::span<const HostLoc> gpr_order, std::span<const HostLoc> xmm_order);\n'
      '#else\n'
      '    explicit RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    std::vector<HostLoc> gpr_order;\n    std::vector<HostLoc> xmm_order;\n\n'
      '    HostLoc SelectARegister(const std::vector<HostLoc>& desired_locations) const;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    std::span<const HostLoc> gpr_order;\n    std::span<const HostLoc> xmm_order;\n\n'
      '    HostLoc SelectARegister(std::span<const HostLoc> desired_locations) const;\n'
      '#else\n'
      '    std::vector<HostLoc> gpr_order;\n    std::vector<HostLoc> xmm_order;\n\n'
      '    HostLoc SelectARegister(const std::vector<HostLoc>& desired_locations) const;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    HostLoc UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc ScratchImpl(const std::vector<HostLoc>& desired_locations);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    HostLoc UseImpl(IR::Value use_value, std::span<const HostLoc> desired_locations);\n'
      '    HostLoc UseScratchImpl(IR::Value use_value, std::span<const HostLoc> desired_locations);\n'
      '    HostLoc ScratchImpl(std::span<const HostLoc> desired_locations);\n'
      '#else\n'
      '    HostLoc UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc ScratchImpl(const std::vector<HostLoc>& desired_locations);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    std::vector<HostLocInfo> hostloc_info;\n#ifdef EMUSUNI_PORT\n',
      '#ifdef EMUSUNI_PORT\n'
      '    std::array<HostLocInfo, NonSpillHostLocCount + SpillCount> hostloc_info;  // EmuSuNi: no heap vector\n'
      '#else\n'
      '    std::vector<HostLocInfo> hostloc_info;\n'
      '#endif\n'
      '#ifdef EMUSUNI_PORT\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'RegAlloc::RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order)\n'
      '        : gpr_order(gpr_order)\n'
      '        , xmm_order(xmm_order)\n'
      '        , hostloc_info(NonSpillHostLocCount + SpillCount)\n'
      '        , code(code) {}\n',
      '#ifdef EMUSUNI_PORT\n'
      'RegAlloc::RegAlloc(BlockOfCode& code, std::span<const HostLoc> gpr_order, std::span<const HostLoc> xmm_order)\n'
      '        : gpr_order(gpr_order)\n'
      '        , xmm_order(xmm_order)\n'
      '        , code(code) {}\n'
      '#else\n'
      'RegAlloc::RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order)\n'
      '        : gpr_order(gpr_order)\n'
      '        , xmm_order(xmm_order)\n'
      '        , hostloc_info(NonSpillHostLocCount + SpillCount)\n'
      '        , code(code) {}\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    arg.allocated = true;\n    UseImpl(arg.value, {host_loc});\n',
      '    arg.allocated = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    UseImpl(arg.value, std::span<const HostLoc>{&host_loc, 1});\n'
      '#else\n'
      '    UseImpl(arg.value, {host_loc});\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    arg.allocated = true;\n    UseScratchImpl(arg.value, {host_loc});\n',
      '    arg.allocated = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    UseScratchImpl(arg.value, std::span<const HostLoc>{&host_loc, 1});\n'
      '#else\n'
      '    UseScratchImpl(arg.value, {host_loc});\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'Xbyak::Reg64 RegAlloc::ScratchGpr(HostLoc desired_location) {\n'
      '    return HostLocToReg64(ScratchImpl({desired_location}));\n}\n',
      'Xbyak::Reg64 RegAlloc::ScratchGpr(HostLoc desired_location) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    return HostLocToReg64(ScratchImpl(std::span<const HostLoc>{&desired_location, 1}));\n'
      '#else\n'
      '    return HostLocToReg64(ScratchImpl({desired_location}));\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'Xbyak::Xmm RegAlloc::ScratchXmm(HostLoc desired_location) {\n'
      '    return HostLocToXmm(ScratchImpl({desired_location}));\n}\n',
      'Xbyak::Xmm RegAlloc::ScratchXmm(HostLoc desired_location) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    return HostLocToXmm(ScratchImpl(std::span<const HostLoc>{&desired_location, 1}));\n'
      '#else\n'
      '    return HostLocToXmm(ScratchImpl({desired_location}));\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n',
      '#ifdef EMUSUNI_PORT\n'
      'HostLoc RegAlloc::UseImpl(IR::Value use_value, std::span<const HostLoc> desired_locations) {\n'
      '#else\n'
      'HostLoc RegAlloc::UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n',
      '#ifdef EMUSUNI_PORT\n'
      'HostLoc RegAlloc::UseScratchImpl(IR::Value use_value, std::span<const HostLoc> desired_locations) {\n'
      '#else\n'
      'HostLoc RegAlloc::UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::ScratchImpl(const std::vector<HostLoc>& desired_locations) {\n',
      '#ifdef EMUSUNI_PORT\n'
      'HostLoc RegAlloc::ScratchImpl(std::span<const HostLoc> desired_locations) {\n'
      '#else\n'
      'HostLoc RegAlloc::ScratchImpl(const std::vector<HostLoc>& desired_locations) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    for (HostLoc caller_saved : other_caller_save) {\n        ScratchImpl({caller_saved});\n    }\n',
      '    for (HostLoc caller_saved : other_caller_save) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        ScratchImpl(std::span<const HostLoc>{&caller_saved, 1});\n'
      '#else\n'
      '        ScratchImpl({caller_saved});\n'
      '#endif\n'
      '    }\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::SelectARegister(const std::vector<HostLoc>& desired_locations) const {\n'
      '    std::vector<HostLoc> candidates = desired_locations;\n\n'
      '    // Find all locations that have not been allocated..\n'
      '    const auto allocated_locs = std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return !this->LocInfo(loc).IsLocked();\n'
      '    });\n'
      '    candidates.erase(allocated_locs, candidates.end());\n'
      '    ASSERT_MSG(!candidates.empty(), "All candidate registers have already been allocated");\n\n'
      '    // Selects the best location out of the available locations.\n'
      '    // TODO: Actually do LRU or something. Currently we just try to pick something without a value if possible.\n\n'
      '    std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return this->LocInfo(loc).IsEmpty();\n'
      '    });\n\n'
      '    return candidates.front();\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the same two partitions on a copy in a stack array (random-access iterators both: the same algorithm,\n'
      '// so the same register), not a new vector each time.\n'
      'HostLoc RegAlloc::SelectARegister(std::span<const HostLoc> desired_locations) const {\n'
      '    std::array<HostLoc, NonSpillHostLocCount> candidates;\n'
      '    ASSERT(desired_locations.size() <= candidates.size());\n'
      '    HostLoc* const first = candidates.data();\n'
      '    HostLoc* last = std::copy(desired_locations.begin(), desired_locations.end(), first);\n\n'
      '    // Find all locations that have not been allocated..\n'
      '    last = std::partition(first, last, [this](auto loc) {\n'
      '        return !this->LocInfo(loc).IsLocked();\n'
      '    });\n'
      '    ASSERT_MSG(first != last, "All candidate registers have already been allocated");\n\n'
      '    std::partition(first, last, [this](auto loc) {\n'
      '        return this->LocInfo(loc).IsEmpty();\n'
      '    });\n\n'
      '    return *first;\n'
      '}\n'
      '#else\n'
      'HostLoc RegAlloc::SelectARegister(const std::vector<HostLoc>& desired_locations) const {\n'
      '    std::vector<HostLoc> candidates = desired_locations;\n\n'
      '    // Find all locations that have not been allocated..\n'
      '    const auto allocated_locs = std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return !this->LocInfo(loc).IsLocked();\n'
      '    });\n'
      '    candidates.erase(allocated_locs, candidates.end());\n'
      '    ASSERT_MSG(!candidates.empty(), "All candidate registers have already been allocated");\n\n'
      '    // Selects the best location out of the available locations.\n'
      '    // TODO: Actually do LRU or something. Currently we just try to pick something without a value if possible.\n\n'
      '    std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return this->LocInfo(loc).IsEmpty();\n'
      '    });\n\n'
      '    return candidates.front();\n'
      '}\n'
      '#endif\n')
# The A32 emitter: its register order made once; room for 8 deferred emits.
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.cpp',
      '    const std::vector<HostLoc> gpr_order = [this] {\n'
      '        std::vector<HostLoc> gprs{any_gpr};\n'
      '        if (conf.page_table) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R14));\n'
      '        }\n'
      '        if (conf.fastmem_pointer) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));\n'
      '        }\n'
      '        return gprs;\n'
      '    }();\n\n'
      '    RegAlloc reg_alloc{code, gpr_order, any_xmm};\n'
      '    A32EmitContext ctx{conf, reg_alloc, block};\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the order made once (conf doesn\'t change), not for every block.\n'
      '    if (emusuni_gpr_order.empty()) {\n'
      '        emusuni_gpr_order = [this] {\n'
      '            std::vector<HostLoc> gprs{any_gpr};\n'
      '            if (conf.page_table) {\n'
      '                gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R14));\n'
      '            }\n'
      '            if (conf.fastmem_pointer) {\n'
      '                gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));\n'
      '            }\n'
      '            return gprs;\n'
      '        }();\n'
      '    }\n'
      '    const std::vector<HostLoc>& gpr_order = emusuni_gpr_order;\n'
      '#else\n'
      '    const std::vector<HostLoc> gpr_order = [this] {\n'
      '        std::vector<HostLoc> gprs{any_gpr};\n'
      '        if (conf.page_table) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R14));\n'
      '        }\n'
      '        if (conf.fastmem_pointer) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));\n'
      '        }\n'
      '        return gprs;\n'
      '    }();\n'
      '#endif\n\n'
      '    RegAlloc reg_alloc{code, gpr_order, any_xmm};\n'
      '    A32EmitContext ctx{conf, reg_alloc, block};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    ctx.deferred_emits.reserve(8);  // EmuSuNi: grown 2-3 times a block otherwise\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '    BlockDescriptor Emit(IR::Block& block);\n\n    void ClearCache() override;\n',
      '    BlockDescriptor Emit(IR::Block& block);\n\n    void ClearCache() override;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::vector<HostLoc> emusuni_gpr_order;  // EmuSuNi: Emit\'s register order, made once\n'
      '#endif\n')
# The identity removal pass's list.
patch('externals/dynarmic/src/dynarmic/ir/opt/identity_removal_pass.cpp',
      '#include <vector>\n',
      '#include <vector>\n#ifdef EMUSUNI_PORT\n#include <boost/container/small_vector.hpp>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/ir/opt/identity_removal_pass.cpp',
      'void IdentityRemovalPass(IR::Block& block) {\n    std::vector<IR::Inst*> to_invalidate;\n',
      'void IdentityRemovalPass(IR::Block& block) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    boost::container::small_vector<IR::Inst*, 32> to_invalidate;  // EmuSuNi: room for 32 inside\n'
      '#else\n'
      '    std::vector<IR::Inst*> to_invalidate;\n'
      '#endif\n')
# The IR pool's slab kept for the next block (one, of the usual size, per thread).
patch('externals/dynarmic/src/dynarmic/common/memory_pool.cpp',
      'Pool::~Pool() {\n    std::free(current_slab);\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: one slab of the usual size kept per thread for the next block\'s pool (a block\'s pool lives on one\n'
      '// thread; a slab kept at a thread\'s end is the only memory left behind).\n'
      'namespace {\n'
      'thread_local char* emusuni_kept_slab = nullptr;\n'
      'thread_local size_t emusuni_kept_bytes = 0;\n'
      '}  // namespace\n'
      '#endif\n\n'
      'Pool::~Pool() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_kept_slab == nullptr && current_slab != nullptr) {\n'
      '        emusuni_kept_slab = current_slab;\n'
      '        emusuni_kept_bytes = object_size * slab_size;\n'
      '        current_slab = nullptr;\n'
      '    }\n'
      '#endif\n'
      '    std::free(current_slab);\n')
patch('externals/dynarmic/src/dynarmic/common/memory_pool.cpp',
      'void Pool::AllocateNewSlab() {\n    current_slab = static_cast<char*>(std::malloc(object_size * slab_size));\n',
      'void Pool::AllocateNewSlab() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_kept_slab != nullptr && emusuni_kept_bytes == object_size * slab_size) {\n'
      '        current_slab = emusuni_kept_slab;\n'
      '        emusuni_kept_slab = nullptr;\n'
      '    } else {\n'
      '        current_slab = static_cast<char*>(std::malloc(object_size * slab_size));\n'
      '    }\n'
      '#else\n'
      '    current_slab = static_cast<char*>(std::malloc(object_size * slab_size));\n'
      '#endif\n')
# The ARM decoder's buckets: pointers to the matchers of one list (made once, never changed), not copies.
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      'template<typename Visitor>\nusing ArmDecodeTable = std::array<std::vector<ArmMatcher<Visitor>>, 0x1000>;\n',
      'template<typename Visitor>\n'
      '#ifdef EMUSUNI_PORT\n'
      'using ArmDecodeTable = std::array<std::vector<const ArmMatcher<Visitor>*>, 0x1000>;  // EmuSuNi: pointers\n'
      '#else\n'
      'using ArmDecodeTable = std::array<std::vector<ArmMatcher<Visitor>>, 0x1000>;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      'template<typename V>\nArmDecodeTable<V> GetArmDecodeTable() {\n    std::vector<ArmMatcher<V>> list = {\n',
      'template<typename V>\nArmDecodeTable<V> GetArmDecodeTable() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    static std::vector<ArmMatcher<V>> list = {  // EmuSuNi: kept (the buckets point into it)\n'
      '#else\n'
      '    std::vector<ArmMatcher<V>> list = {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      '    ArmDecodeTable<V> table{};\n    for (size_t i = 0; i < table.size(); ++i) {\n        for (auto matcher : list) {\n'
      '            const auto expect = detail::ToFastLookupIndexArm(matcher.GetExpected());\n'
      '            const auto mask = detail::ToFastLookupIndexArm(matcher.GetMask());\n'
      '            if ((i & mask) == expect) {\n'
      '                table[i].push_back(matcher);\n',
      '    ArmDecodeTable<V> table{};\n    for (size_t i = 0; i < table.size(); ++i) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        for (const auto& matcher : list) {\n'
      '#else\n'
      '        for (auto matcher : list) {\n'
      '#endif\n'
      '            const auto expect = detail::ToFastLookupIndexArm(matcher.GetExpected());\n'
      '            const auto mask = detail::ToFastLookupIndexArm(matcher.GetMask());\n'
      '            if ((i & mask) == expect) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                table[i].push_back(&matcher);\n'
      '#else\n'
      '                table[i].push_back(matcher);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      '    const auto matches_instruction = [instruction](const auto& matcher) { return matcher.Matches(instruction); };\n\n'
      '    const auto& subtable = table[detail::ToFastLookupIndexArm(instruction)];\n'
      '    auto iter = std::find_if(subtable.begin(), subtable.end(), matches_instruction);\n'
      '    return iter != subtable.end() ? std::optional<std::reference_wrapper<const ArmMatcher<V>>>(*iter) : std::nullopt;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const auto matches_instruction = [instruction](const auto* matcher) { return matcher->Matches(instruction); };\n\n'
      '    const auto& subtable = table[detail::ToFastLookupIndexArm(instruction)];\n'
      '    auto iter = std::find_if(subtable.begin(), subtable.end(), matches_instruction);\n'
      '    return iter != subtable.end() ? std::optional<std::reference_wrapper<const ArmMatcher<V>>>(**iter) : std::nullopt;\n'
      '#else\n'
      '    const auto matches_instruction = [instruction](const auto& matcher) { return matcher.Matches(instruction); };\n\n'
      '    const auto& subtable = table[detail::ToFastLookupIndexArm(instruction)];\n'
      '    auto iter = std::find_if(subtable.begin(), subtable.end(), matches_instruction);\n'
      '    return iter != subtable.end() ? std::optional<std::reference_wrapper<const ArmMatcher<V>>>(*iter) : std::nullopt;\n'
      '#endif\n')
# The blocks' ranges: buckets of 4 KiB pages (each block in every page it covers), the same locations returned.
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.h',
      'private:\n    boost::icl::interval_map<ProgramCounterType, std::set<IR::LocationDescriptor>> block_ranges;\n};\n',
      'private:\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: every block\'s range in the bucket of each 4 KiB page it covers (an interval map of sets\n'
      '    // allocated 3 times and split intervals for every block added).\n'
      '    struct EmuSuNiRange {\n'
      '        ProgramCounterType first, last;\n'
      '        IR::LocationDescriptor location;\n'
      '    };\n'
      '    tsl::robin_map<ProgramCounterType, std::vector<EmuSuNiRange>> emusuni_pages;\n'
      '#else\n'
      '    boost::icl::interval_map<ProgramCounterType, std::set<IR::LocationDescriptor>> block_ranges;\n'
      '#endif\n'
      '};\n')
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.h',
      '#include <tsl/robin_set.h>\n',
      '#include <tsl/robin_set.h>\n#ifdef EMUSUNI_PORT\n#include <vector>\n#include <tsl/robin_map.h>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.cpp',
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {\n'
      '    block_ranges.add(std::make_pair(range, std::set<IR::LocationDescriptor>{location}));\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::ClearCache() {\n'
      '    block_ranges.clear();\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {\n'
      '    if (boost::icl::is_empty(range)) {\n'
      '        return;  // (as the interval map: nothing added)\n'
      '    }\n'
      '    const ProgramCounterType first = boost::icl::first(range), last = boost::icl::last(range);\n'
      '    for (ProgramCounterType page = first >> 12;; page++) {\n'
      '        emusuni_pages[page].push_back(EmuSuNiRange{first, last, location});\n'
      '        if (page == (last >> 12)) {\n'
      '            break;\n'
      '        }\n'
      '    }\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::ClearCache() {\n'
      '    emusuni_pages.clear();\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {\n'
      '    tsl::robin_set<IR::LocationDescriptor> erase_locations;\n'
      '    for (auto invalidate_interval : ranges) {\n'
      '        if (boost::icl::is_empty(invalidate_interval)) {\n'
      '            continue;\n'
      '        }\n'
      '        const ProgramCounterType first = boost::icl::first(invalidate_interval), last = boost::icl::last(invalidate_interval);\n'
      '        // (Pages of the range that hold blocks: each page of the range, or each page held when that is fewer.)\n'
      '        const auto check = [&](const std::vector<EmuSuNiRange>& entries) {\n'
      '            for (const EmuSuNiRange& entry : entries) {\n'
      '                if (entry.first <= last && entry.last >= first) {\n'
      '                    erase_locations.insert(entry.location);\n'
      '                }\n'
      '            }\n'
      '        };\n'
      '        const ProgramCounterType first_page = first >> 12, last_page = last >> 12;\n'
      '        if (static_cast<u64>(last_page - first_page) >= emusuni_pages.size()) {\n'
      '            for (const auto& [page, entries] : emusuni_pages) {\n'
      '                if (page >= first_page && page <= last_page) {\n'
      '                    check(entries);\n'
      '                }\n'
      '            }\n'
      '        } else {\n'
      '            for (ProgramCounterType page = first_page;; page++) {\n'
      '                if (const auto it = emusuni_pages.find(page); it != emusuni_pages.end()) {\n'
      '                    check(it->second);\n'
      '                }\n'
      '                if (page == last_page) {\n'
      '                    break;\n'
      '                }\n'
      '            }\n'
      '        }\n'
      '    }\n'
      '    return erase_locations;\n'
      '}\n'
      '#else\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {\n'
      '    block_ranges.add(std::make_pair(range, std::set<IR::LocationDescriptor>{location}));\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::ClearCache() {\n'
      '    block_ranges.clear();\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.cpp',
      '    // TODO: EFFICIENCY: Remove ranges that are to be erased.\n    return erase_locations;\n}\n',
      '    // TODO: EFFICIENCY: Remove ranges that are to be erased.\n    return erase_locations;\n}\n'
      '#endif\n')

# --- 00.69: the JIT warm-up. The 00.68 log: the games' loads and new areas stop for 0.1-0.66 s while dynarmic compiles
# the ARM11's code met for the first time (Super Mario 3D Land: 61,324 blocks in its first 600 frames, 5.1 s; the same
# code at every start), and each of the 2,000-9,000 memory accesses a session finds outside the fast memory costs a
# fault and a recompile. Now both are kept per game, by location only (no code: the game's own code segment is
# compiled again from memory), in saves/3ds/Azahar/shaders/emusuni-jit/<program id>.bin, written every minute while
# they grow (on a worker thread) and when the game ends. At the next start the accesses are marked first and the
# blocks compiled again in the order they were first met, from the first frame on: 0.4 s in the first frame, 12 ms a
# frame in the next 120 (the boot's light frames), then 6 ms a frame, ahead of the game (its boot runs the same code in
# the same order). Only blocks inside the game's code segment (the code that can't change while the game runs: the
# file also keeps the segment's address, size and
# hash, and a file made for other code - an update - is started again), on core 0 (the application core: Super Mario
# 3D Land compiles nothing on core 1, Zelda 8%), and leaving 8 MiB of the 24 MiB code cache free (no flush). The file
# /data/EmuSuNi/3ds-jit-warmup-off (EMUSUNI_3DS_JIT_WARMUP=0 on the PC) turns it off. dynarmic gets four calls: the
# locations it compiled, compile a location now, its "no fast memory here" marks, and add one.
patch('externals/dynarmic/src/dynarmic/interface/A32/a32.h',
      '    /// Debugging: Dump a disassembly all compiled code to the console.\n',
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (citra_libretro/emusuni_jitwarm.h): the location (UniqueHash) of every block compiled since the\n'
      '    /// JIT was made, in order (a recompiled block again).\n'
      '    const std::vector<std::uint64_t>& EmuSuNiCompiledLocations() const;\n'
      '    /// EmuSuNi: the block at this location compiled now, as when it is first run: 1 compiled, 0 there already,\n'
      '    /// -1 not compiled (the code cache keeps 8 MiB free for the game). Not from a callback.\n'
      '    int EmuSuNiPrecompile(std::uint64_t location);\n'
      '    /// EmuSuNi: the memory accesses compiled with the page table because they faulted with the fast memory\n'
      '    /// (block location, IR instruction name), and one added (before its block is compiled).\n'
      '    std::vector<std::pair<std::uint64_t, std::uint32_t>> EmuSuNiFastmemMarkers() const;\n'
      '    void EmuSuNiAddFastmemMarker(std::uint64_t location, std::uint32_t name);\n'
      '#endif\n\n'
      '    /// Debugging: Dump a disassembly all compiled code to the console.\n')
patch('externals/dynarmic/src/dynarmic/interface/A32/a32.h',
      '#include <string>\n#include <vector>\n',
      '#include <string>\n#include <vector>\n#ifdef EMUSUNI_PORT\n#include <utility>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '    A32EmitX64::BlockDescriptor GetBasicBlock(IR::LocationDescriptor descriptor) {\n'
      '        auto block = emitter.GetBasicBlock(descriptor);\n'
      '        if (block)\n'
      '            return *block;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const ::EmuSuNi::Stats::TickTimer emusuni_timer{::EmuSuNi::Stats::JitTicks};\n'
      '        ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitBlocks]++;\n'
      '#endif\n',
      '    A32EmitX64::BlockDescriptor GetBasicBlock(IR::LocationDescriptor descriptor) {\n'
      '        auto block = emitter.GetBasicBlock(descriptor);\n'
      '        if (block)\n'
      '            return *block;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const ::EmuSuNi::Stats::TickTimer emusuni_timer{::EmuSuNi::Stats::JitTicks};\n'
      '        ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitBlocks]++;\n'
      '        if (emusuni_compiled.size() < 600000) {\n'
      '            emusuni_compiled.push_back(descriptor.Value());  // EmuSuNi: (the JIT warm-up)\n'
      '        }\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '    A32JitState jit_state;\n    BlockOfCode block_of_code;\n    A32EmitX64 emitter;\n',
      '#ifdef EMUSUNI_PORT\n'
      'public:\n'
      '    // EmuSuNi: the JIT warm-up (citra_libretro/emusuni_jitwarm.h).\n'
      '    std::vector<u64> emusuni_compiled;\n'
      '    int EmuSuNiPrecompile(u64 location) {\n'
      '        const IR::LocationDescriptor descriptor{location};\n'
      '        if (emitter.GetBasicBlock(descriptor)) {\n'
      '            return 0;\n'
      '        }\n'
      '        if (block_of_code.SpaceRemaining() < 8 * 1024 * 1024) {\n'
      '            return -1;\n'
      '        }\n'
      '        GetBasicBlock(descriptor);\n'
      '        return 1;\n'
      '    }\n'
      '    std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const {\n'
      '        return emitter.EmuSuNiFastmemMarkers();\n'
      '    }\n'
      '    void EmuSuNiAddFastmemMarker(u64 location, u32 name) {\n'
      '        emitter.EmuSuNiAddFastmemMarker(location, name);\n'
      '    }\n\n'
      'private:\n'
      '#endif\n'
      '    A32JitState jit_state;\n    BlockOfCode block_of_code;\n    A32EmitX64 emitter;\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      'void Jit::InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {\n'
      '    impl->InvalidateCacheRange(start_address, length);\n'
      '}\n',
      'void Jit::InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {\n'
      '    impl->InvalidateCacheRange(start_address, length);\n'
      '}\n'
      '#ifdef EMUSUNI_PORT\n\n'
      'const std::vector<std::uint64_t>& Jit::EmuSuNiCompiledLocations() const {\n'
      '    return impl->emusuni_compiled;\n'
      '}\n\n'
      'int Jit::EmuSuNiPrecompile(std::uint64_t location) {\n'
      '    ASSERT(!is_executing);\n'
      '    return impl->EmuSuNiPrecompile(location);\n'
      '}\n\n'
      'std::vector<std::pair<std::uint64_t, std::uint32_t>> Jit::EmuSuNiFastmemMarkers() const {\n'
      '    return impl->EmuSuNiFastmemMarkers();\n'
      '}\n\n'
      'void Jit::EmuSuNiAddFastmemMarker(std::uint64_t location, std::uint32_t name) {\n'
      '    impl->EmuSuNiAddFastmemMarker(location, name);\n'
      '}\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '    std::vector<HostLoc> emusuni_gpr_order;  // EmuSuNi: Emit\'s register order, made once\n#endif\n',
      '    std::vector<HostLoc> emusuni_gpr_order;  // EmuSuNi: Emit\'s register order, made once\n'
      '    // EmuSuNi: the JIT warm-up\'s (citra_libretro/emusuni_jitwarm.h) "no fast memory here" marks.\n'
      '    std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const {\n'
      '        std::vector<std::pair<u64, u32>> markers;\n'
      '        markers.reserve(do_not_fastmem.size());\n'
      '        for (const auto& marker : do_not_fastmem) {\n'
      '            markers.emplace_back(std::get<0>(marker).Value(), static_cast<u32>(std::get<1>(marker)));\n'
      '        }\n'
      '        return markers;\n'
      '    }\n'
      '    void EmuSuNiAddFastmemMarker(u64 location, u32 name) {\n'
      '        do_not_fastmem.insert(DoNotFastmemMarker{IR::LocationDescriptor{location}, name});\n'
      '    }\n'
      '#endif\n')
# Azahar: a core's JIT of its current page table, for the warm-up.
patch('src/core/arm/arm_interface.h',
      '    /// Notify CPU emulation that page tables have changed\n'
      '    virtual void SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) = 0;\n',
      '    /// Notify CPU emulation that page tables have changed\n'
      '    virtual void SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) = 0;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (citra_libretro/emusuni_jitwarm.h): the JIT warm-up\'s calls on this core\'s JIT of its current\n'
      '    /// page table (dynarmic\'s; none for an interpreter): that page table, the locations it compiled, compile one\n'
      '    /// now (1 compiled, 0 there already, -1 not), its "no fast memory here" marks and add one.\n'
      '    virtual std::shared_ptr<Memory::PageTable> EmuSuNiJitPageTable() const {\n'
      '        return nullptr;\n'
      '    }\n'
      '    virtual const std::vector<u64>* EmuSuNiCompiledLocations() const {\n'
      '        return nullptr;\n'
      '    }\n'
      '    virtual int EmuSuNiPrecompile(u64 location) {\n'
      '        return -1;\n'
      '    }\n'
      '    virtual std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const {\n'
      '        return {};\n'
      '    }\n'
      '    virtual void EmuSuNiAddFastmemMarker(u64 location, u32 name) {}\n'
      '#endif\n')
patch('src/core/arm/arm_interface.h',
      '#include <cstddef>\n#include <memory>\n',
      '#include <cstddef>\n#include <memory>\n#ifdef EMUSUNI_PORT\n#include <utility>\n#include <vector>\n#endif\n')
patch('src/core/arm/dynarmic/arm_dynarmic.h',
      '    bool HasSingleInstructionBreakAccuracy() override {\n        return false;\n    }\n',
      '    bool HasSingleInstructionBreakAccuracy() override {\n        return false;\n    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::shared_ptr<Memory::PageTable> EmuSuNiJitPageTable() const override {\n'
      '        return jit != nullptr ? current_page_table : nullptr;\n'
      '    }\n'
      '    const std::vector<u64>* EmuSuNiCompiledLocations() const override {\n'
      '        return jit != nullptr ? &jit->EmuSuNiCompiledLocations() : nullptr;\n'
      '    }\n'
      '    int EmuSuNiPrecompile(u64 location) override {\n'
      '        return jit != nullptr ? jit->EmuSuNiPrecompile(location) : -1;\n'
      '    }\n'
      '    std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const override {\n'
      '        return jit != nullptr ? jit->EmuSuNiFastmemMarkers() : std::vector<std::pair<u64, u32>>{};\n'
      '    }\n'
      '    void EmuSuNiAddFastmemMarker(u64 location, u32 name) override {\n'
      '        if (jit != nullptr) {\n'
      '            jit->EmuSuNiAddFastmemMarker(location, name);\n'
      '        }\n'
      '    }\n'
      '#endif\n')
addfile('src/citra_libretro/emusuni_jitwarm.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the JIT warm-up. The blocks of the game's code segment that dynarmic compiled on
// core 0, and its memory accesses there that faulted with the fast memory, are kept by location in
// <user>/shaders/emusuni-jit/<program id>.bin. At the next start the accesses are marked first (compiled with the page
// table at once: no fault, no recompile) and the blocks compiled again in the order they were first met, from the
// first frame on (0.4 s in the first frame, 12 ms a frame in the next 120, then 6 ms), so that the game's boot and the
// areas met before don't stop to compile them. Written every minute while it grows (on a worker thread) and at the
// game's end.
// A file made for other code (another version, an update) is started again; /data/EmuSuNi/3ds-jit-warmup-off
// (EMUSUNI_3DS_JIT_WARMUP=0 on the PC) turns it all off. Called on the emulation thread only (retro_run,
// retro_unload_game).
#pragma once
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/thread_worker.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace EmuSuNi::JitWarm {

// The file: this header, `count` block locations (u64), then `markers` accesses (u64 block location, u32 IR name,
// u32 0).
struct FileHeader {
    char magic[4];
    std::uint32_t version;
    std::uint64_t program_id;
    std::uint32_t code_addr, code_size;
    std::uint64_t code_hash;
    std::uint32_t count, markers;
};
static_assert(sizeof(FileHeader) == 40);
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxLocations = 200000, kMaxMarkers = 100000;
constexpr std::uint64_t kSingleStepping = std::uint64_t{1} << 34;  // (A32 LocationDescriptor::UniqueHash)

struct State {
    bool started = false, off = false;
    std::uint64_t program_id = 0, code_hash = 0;
    std::uint32_t code_addr = 0, code_size = 0;
    std::string path;
    std::vector<std::uint64_t> loaded;  // the file's, in order
    std::size_t next = 0, compiled = 0, already = 0, saved = 0, saved_markers = 0;
    std::uint64_t spent_us = 0, last_save_ms = 0;
    int frames = 0, tries = 0;
};
inline State g;

inline std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

inline bool SwitchedOff() {
#if defined(__ORBIS__)
    return ::access("/data/EmuSuNi/3ds-jit-warmup-off", F_OK) == 0;
#else
    const char* value = std::getenv("EMUSUNI_3DS_JIT_WARMUP");
    return value != nullptr && std::strcmp(value, "0") == 0;
#endif
}

inline bool InCode(std::uint64_t location) {
    const std::uint32_t pc = static_cast<std::uint32_t>(location);
    return location != 0 && (location & kSingleStepping) == 0 && pc >= g.code_addr && pc - g.code_addr < g.code_size;
}

// Core 0, when its JIT is the one of the game process's page table; the process too.
inline Core::ARM_Interface* GameJit(Core::System& system, std::shared_ptr<Kernel::Process>& process) {
    if (system.GetNumCores() == 0) {
        return nullptr;
    }
    process = system.Kernel().GetCurrentProcess();
    if (!process || !process->codeset || process->codeset->CodeSegment().size == 0) {
        return nullptr;
    }
    Core::ARM_Interface& core = system.GetCore(0);
    const std::shared_ptr<Memory::PageTable> table = core.EmuSuNiJitPageTable();
    if (!table || table != process->vm_manager.page_table || core.EmuSuNiCompiledLocations() == nullptr) {
        return nullptr;
    }
    return &core;
}

inline void WriteFile(const std::string& path, const std::string& tmp, const std::vector<std::uint8_t>& data) {
    FileUtil::CreateFullPath(path);
    {
        FileUtil::IOFile file(tmp, "wb");
        if (!file.IsOpen() || file.WriteBytes(data.data(), data.size()) != data.size()) {
            LOG_WARNING(Frontend, "EmuSuNi: JIT warm-up: {} not written", tmp);
            return;
        }
    }
    if (!FileUtil::Rename(tmp, path)) {
        LOG_WARNING(Frontend, "EmuSuNi: JIT warm-up: {} not renamed to {}", tmp, path);
    }
}

inline void Start(Core::System& system) {
    std::shared_ptr<Kernel::Process> process;
    Core::ARM_Interface* core = GameJit(system, process);
    if (core == nullptr) {
        // (Not yet: the game's page table isn't core 0's. Given up after 600 frames.)
        if (++g.tries >= 600) {
            g.started = g.off = true;
        }
        return;
    }
    g.started = true;
    if (SwitchedOff()) {
        g.off = true;
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: off (the file /data/EmuSuNi/3ds-jit-warmup-off)");
        return;
    }
    const auto& code = process->codeset->CodeSegment();
    g.program_id = process->codeset->program_id;
    g.code_addr = code.addr;
    g.code_size = code.size;
    std::vector<std::uint8_t> bytes(code.size);
    system.Memory().ReadBlock(*process, code.addr, bytes.data(), bytes.size());
    g.code_hash = Common::ComputeHash64(bytes.data(), bytes.size());
    g.path = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "emusuni-jit/" +
             fmt::format("{:016X}.bin", g.program_id);
    FileUtil::IOFile file(g.path, "rb");
    if (!file.IsOpen()) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: nothing kept yet for this game ({})", g.path);
        return;
    }
    FileHeader header{};
    const std::uint64_t size = file.GetSize();
    if (size < sizeof(header) || file.ReadBytes(&header, sizeof(header)) != sizeof(header) ||
        std::memcmp(header.magic, "EJW1", 4) != 0 || header.version != kVersion || header.count > kMaxLocations ||
        header.markers > kMaxMarkers ||
        size != sizeof(header) + std::uint64_t{header.count} * 8 + std::uint64_t{header.markers} * 16) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} isn't a list of this version: started again", g.path);
        return;
    }
    if (header.program_id != g.program_id || header.code_addr != g.code_addr || header.code_size != g.code_size ||
        header.code_hash != g.code_hash) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} was made for other code (another version or an update): "
                 "started again", g.path);
        return;
    }
    std::vector<std::uint64_t> locations(header.count);
    std::vector<std::uint64_t> markers(std::size_t{header.markers} * 2);
    if (file.ReadBytes(locations.data(), locations.size() * 8) != locations.size() * 8 ||
        file.ReadBytes(markers.data(), markers.size() * 8) != markers.size() * 8) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} not read: started again", g.path);
        return;
    }
    // (The marks first: the blocks compiled from now on take the page table for those accesses.)
    std::size_t marked = 0;
    for (std::size_t i = 0; i < header.markers; i++) {
        if (InCode(markers[i * 2])) {
            core->EmuSuNiAddFastmemMarker(markers[i * 2], static_cast<std::uint32_t>(markers[i * 2 + 1]));
            marked++;
        }
    }
    g.loaded = std::move(locations);
    g.saved = g.loaded.size();
    g.saved_markers = header.markers;
    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} blocks of this game's code from earlier sessions to compile, {} of "
             "their memory accesses outside the fast memory ({})", g.loaded.size(), marked, g.path);
}

// The file's locations then this session's, each once, those in the code segment (a small open-addressed set: one
// allocation, not a node per location).
inline std::vector<std::uint64_t> Collect(Core::ARM_Interface& core) {
    const std::vector<std::uint64_t>& compiled = *core.EmuSuNiCompiledLocations();
    std::size_t capacity = 1024;
    while (capacity < 2 * (g.loaded.size() + compiled.size())) {
        capacity *= 2;
    }
    std::vector<std::uint64_t> set(capacity, 0);
    std::vector<std::uint64_t> out;
    out.reserve(std::min(kMaxLocations, g.loaded.size() + compiled.size()));
    const auto add = [&](std::uint64_t location) {
        if (!InCode(location) || out.size() >= kMaxLocations) {
            return;
        }
        std::size_t slot = static_cast<std::size_t>((location * 0x9E3779B97F4A7C15ull) >> 20) & (capacity - 1);
        while (set[slot] != 0) {
            if (set[slot] == location) {
                return;
            }
            slot = (slot + 1) & (capacity - 1);
        }
        set[slot] = location;
        out.push_back(location);
    };
    for (const std::uint64_t location : g.loaded) {
        add(location);
    }
    for (const std::uint64_t location : compiled) {
        add(location);
    }
    return out;
}

inline Common::ThreadWorker& Writer() {
    // (Made at the first save, never destroyed: the app may end with a write queued.)
    static Common::ThreadWorker* worker = new Common::ThreadWorker(1, "EmuSuNi JIT warm-up");
    return *worker;
}

inline void Save(Core::System& system, bool at_end) {
    if (g.off || !g.started || g.path.empty()) {
        return;
    }
    std::shared_ptr<Kernel::Process> process;
    Core::ARM_Interface* core = GameJit(system, process);
    if (core == nullptr || process->codeset->program_id != g.program_id) {
        return;
    }
    std::vector<std::uint64_t> locations = Collect(*core);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> markers = core->EmuSuNiFastmemMarkers();
    markers.erase(std::remove_if(markers.begin(), markers.end(), [](const auto& m) { return !InCode(m.first); }),
                  markers.end());
    if (markers.size() > kMaxMarkers) {
        markers.resize(kMaxMarkers);
    }
    // (Every minute only when the blocks grew by 1000 or more; at the end when anything grew.)
    const bool grew = locations.size() > g.saved || markers.size() > g.saved_markers;
    if (!grew || (!at_end && locations.size() < g.saved + 1000)) {
        return;
    }
    FileHeader header{};
    std::memcpy(header.magic, "EJW1", 4);
    header.version = kVersion;
    header.program_id = g.program_id;
    header.code_addr = g.code_addr;
    header.code_size = g.code_size;
    header.code_hash = g.code_hash;
    header.count = static_cast<std::uint32_t>(locations.size());
    header.markers = static_cast<std::uint32_t>(markers.size());
    auto data = std::make_shared<std::vector<std::uint8_t>>(sizeof(header) + locations.size() * 8 +
                                                            markers.size() * 16);
    std::uint8_t* out = data->data();
    std::memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    std::memcpy(out, locations.data(), locations.size() * 8);
    out += locations.size() * 8;
    for (const auto& [location, name] : markers) {
        const std::uint64_t record[2] = {location, name};
        std::memcpy(out, record, 16);
        out += 16;
    }
    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} blocks and {} accesses kept for the next session (+{} blocks, {} KiB)",
             locations.size(), markers.size(), locations.size() - std::min(locations.size(), g.saved),
             data->size() / 1024);
    g.saved = locations.size();
    g.saved_markers = markers.size();
    if (at_end) {
        WriteFile(g.path, g.path + ".tmp", *data);
    } else {
        const std::string path = g.path;
        Writer().QueueWork([path, data] { WriteFile(path, path + ".w.tmp", *data); });
    }
}

// retro_run, before the frame's emulation.
inline void BeforeFrame(Core::System& system) {
    if (!g.started) {
        Start(system);
        if (!g.started) {
            return;
        }
    }
    if (g.off) {
        return;
    }
    if (g.next < g.loaded.size()) {
        std::shared_ptr<Kernel::Process> process;
        Core::ARM_Interface* core = GameJit(system, process);
        if (core != nullptr) {
            const std::uint64_t budget_us = g.frames == 0 ? 400000 : g.frames < 120 ? 12000 : 6000;
            const std::uint64_t t0 = NowUs();
            while (g.next < g.loaded.size()) {
                const std::uint64_t location = g.loaded[g.next++];
                if (!InCode(location)) {
                    continue;
                }
                const int result = core->EmuSuNiPrecompile(location);
                if (result < 0) {
                    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: the code cache has no more room to spare: {} blocks "
                             "left for the game to compile when it meets them", g.loaded.size() - g.next + 1);
                    g.next = g.loaded.size();
                    break;
                }
                (result > 0 ? g.compiled : g.already)++;
                if ((g.next & 15) == 0 && NowUs() - t0 >= budget_us) {
                    break;
                }
            }
            g.spent_us += NowUs() - t0;
            g.frames++;
        }
        if (g.next >= g.loaded.size()) {
            LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} blocks compiled in {} ms over {} frames ({} there already)",
                     g.compiled, g.spent_us / 1000, g.frames, g.already);
        }
    }
    const std::uint64_t now_ms = NowUs() / 1000;
    if (g.last_save_ms == 0) {
        g.last_save_ms = now_ms;
    } else if (now_ms - g.last_save_ms >= 60000) {
        g.last_save_ms = now_ms;
        Save(system, false);
    }
}

// retro_unload_game, before the system shuts down: the list written now; the state made new for the next game.
inline void End(Core::System& system) {
    Save(system, true);
    g = State{};
}

} // namespace EmuSuNi::JitWarm
#endif
''')
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "common/emusuni_frame_stats.h"\n#include "common/emusuni_stats.h"\n',
      '#include "common/emusuni_frame_stats.h"\n#include "common/emusuni_stats.h"\n#include "citra_libretro/emusuni_jitwarm.h"\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n    try {\n#endif\n',
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n    try {\n'
      '    // EmuSuNi: the JIT warm-up (citra_libretro/emusuni_jitwarm.h): earlier sessions\' blocks compiled ahead.\n'
      '    EmuSuNi::JitWarm::BeforeFrame(Core::System::GetInstance());\n'
      '#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      'void retro_unload_game() {\n    LOG_DEBUG(Frontend, "Unloading game...");\n    Core::System::GetInstance().Shutdown();\n}\n',
      'void retro_unload_game() {\n    LOG_DEBUG(Frontend, "Unloading game...");\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::JitWarm::End(Core::System::GetInstance());  // EmuSuNi: (citra_libretro/emusuni_jitwarm.h)\n'
      '#endif\n'
      '    Core::System::GetInstance().Shutdown();\n}\n')

# --- 00.69: the games' file reads and writes on two threads made once. Azahar's RunAsync made a new thread
# (std::async) for every file request the FS cache doesn't answer: 1,449 threads in a Driver Renegade 3D session of
# the 00.68 log (758 in 3,000 frames on this PC), each a thread creation on the emulation thread and the hook's
# placing. Now the FS service's files and directories (file.cpp, directory.cpp) use RunOnThreadWorker, Azahar's own
# way for the same thing (FS_USER's worker), with one worker of two threads ("HleAsync"); the other services' requests
# (sockets, which may block for long, HTTP, AM) keep their threads. std::async's future waited for its request when
# destroyed (with the system); the worker's don't: System::Shutdown waits for the worker's requests first.
addfile('src/core/hle/service/fs/emusuni_fs_worker.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the FS service's files' and directories' asynchronous work (reads, writes,
// flushes, lists) on two threads made at the first request and kept (never destroyed: a request may be queued when the
// app ends), instead of a thread per request (RunAsync's std::async).
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include "common/thread_worker.h"

namespace Service::FS {
inline std::atomic<Common::ThreadWorker*> emusuni_file_worker{nullptr};

inline Common::ThreadWorker& EmuSuNiFileWorker() {
    static Common::ThreadWorker* const worker = [] {
        Common::ThreadWorker* made = new Common::ThreadWorker(2, "HleAsync");
        emusuni_file_worker.store(made);
        return made;
    }();
    return *worker;
}

/// The worker's requests finished (System::Shutdown, first: a request's context and client thread go with the
/// system; RunAsync's std::async future waited for its request when destroyed).
inline void EmuSuNiFileWorkerWait() {
    if (Common::ThreadWorker* worker = emusuni_file_worker.load()) {
        worker->WaitForRequests();
    }
}
} // namespace Service::FS

// The two files' ctx.RunAsync(...) calls become ctx.RunOnThreadWorker(that worker, ...): the same request, the same
// result callback on the emulation thread, the client thread asleep meanwhile. (Not where only the wait is wanted.)
#ifndef EMUSUNI_FS_WORKER_WAIT_ONLY
#define RunAsync(...) RunOnThreadWorker(::Service::FS::EmuSuNiFileWorker(), __VA_ARGS__)
#endif
#endif
''')
patch('src/core/hle/service/fs/file.cpp',
      '#include "core/hle/service/fs/file.h"\n',
      '#include "core/hle/service/fs/file.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "core/hle/service/fs/emusuni_fs_worker.h" // (last: it renames RunAsync)\n#endif\n')
patch('src/core/hle/service/fs/directory.cpp',
      '#include "core/hle/service/fs/directory.h"\n',
      '#include "core/hle/service/fs/directory.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "core/hle/service/fs/emusuni_fs_worker.h" // (last: it renames RunAsync)\n#endif\n')
patch('src/core/core.cpp',
      '#include "core/hle/service/sm/sm.h"\n',
      '#include "core/hle/service/sm/sm.h"\n'
      '#ifdef EMUSUNI_PORT\n#define EMUSUNI_FS_WORKER_WAIT_ONLY\n#include "core/hle/service/fs/emusuni_fs_worker.h"\n#endif\n')
patch('src/core/core.cpp',
      'void System::Shutdown(bool is_deserializing) {\n',
      'void System::Shutdown(bool is_deserializing) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the FS service\'s file requests on their worker finished first (service/fs/emusuni_fs_worker.h).\n'
      '    Service::FS::EmuSuNiFileWorkerWait();\n'
      '#endif\n')

# --- 00.69: textures decoded into ordinary memory, then copied to the staging buffer in one pass. The staging buffer
# is write-combined memory on the PS4 (garlic: the radv winsys maps every HOST_VISIBLE|HOST_COHERENT buffer that
# isn't HOST_CACHED so), and the tiled decoders write it 32 bytes of a row at a time, eight rows apart (an 8x8 tile),
# which write-combining memory takes badly (each partial line goes out on its own); Driver Renegade 3D's loads decoded
# 58 MB of textures in 364 ms, once 4.4 MB in 270 ms. The decode now writes a scratch buffer (cached; the emulation
# thread's own, grown to the largest texture and kept) and one memcpy streams it to the staging buffer. The same bytes.
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '#include <type_traits>\n#include <boost/container/small_vector.hpp>\n',
      '#include <type_traits>\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <cstring>\n#endif\n'
      '#include <boost/container/small_vector.hpp>\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '    DecodeTexture(load_info, load_info.addr, load_info.end, upload_data, staging.mapped,\n'
      '                  runtime.NeedsConversion(surface));\n',
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        // EmuSuNi: decoded into the thread\'s scratch buffer (cached memory), then one sequential copy into the\n'
      '        // staging buffer (write-combined on the PS4: scripts/build-3ds-core.sh). Kept for the thread\'s life (the\n'
      '        // emulation thread: the app\'s main thread).\n'
      '        static thread_local u8* emusuni_scratch = nullptr;\n'
      '        static thread_local std::size_t emusuni_scratch_size = 0;\n'
      '        std::span<u8> emusuni_decode_to = staging.mapped;\n'
      '        if (staging.mapped.size() <= std::size_t{64} << 20) {\n'
      '            if (emusuni_scratch_size < staging.mapped.size()) {\n'
      '                u8* grown = static_cast<u8*>(std::realloc(emusuni_scratch, staging.mapped.size()));\n'
      '                if (grown != nullptr) {\n'
      '                    emusuni_scratch = grown;\n'
      '                    emusuni_scratch_size = staging.mapped.size();\n'
      '                }\n'
      '            }\n'
      '            if (emusuni_scratch_size >= staging.mapped.size()) {\n'
      '                emusuni_decode_to = std::span<u8>{emusuni_scratch, staging.mapped.size()};\n'
      '            }\n'
      '        }\n'
      '        DecodeTexture(load_info, load_info.addr, load_info.end, upload_data, emusuni_decode_to,\n'
      '                      runtime.NeedsConversion(surface));\n'
      '        if (emusuni_decode_to.data() != staging.mapped.data()) {\n'
      '            std::memcpy(staging.mapped.data(), emusuni_decode_to.data(), emusuni_decode_to.size());\n'
      '        }\n'
      '    }\n'
      '#else\n'
      '    DecodeTexture(load_info, load_info.addr, load_info.end, upload_data, staging.mapped,\n'
      '                  runtime.NeedsConversion(surface));\n'
      '#endif\n')

# --- 00.69: Xbyak's labels without a heap allocation each. A block's code takes ~5 labels (its condition, its
# terminal's links, each memory access's slow path); each was 2-3 heap allocations (nodes of the label manager's hash
# tables: defined labels, the labels' list, jumps waiting for one) - 10 of the 23 a block still made. The three tables'
# nodes now come from free lists of the thread (16-byte size classes up to 128; the tables' bucket arrays as before).
# The same labels, the same code. In Xbyak itself (externals/xbyak, used by dynarmic and the PICA shader JIT alike).
patch('externals/xbyak/xbyak/xbyak.h',
      'class LabelManager;\n\nclass Label {\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (scripts/build-3ds-core.sh): the label tables\' nodes from free lists of the thread (a code generator is\n'
      '// used by one thread at a time; a node freed on another thread goes to that one\'s list). Nodes stay in the lists\n'
      '// (the most a thread had at once); bigger or array allocations as before.\n'
      'namespace emusuni {\n'
      'struct NodeLists {\n'
      '\tvoid *head[8];\n'
      '};\n'
      'inline NodeLists& Lists()\n'
      '{\n'
      '\tstatic thread_local NodeLists lists = {};\n'
      '\treturn lists;\n'
      '}\n'
      'template<class T>\n'
      'struct NodeAlloc {\n'
      '\ttypedef T value_type;\n'
      '\tNodeAlloc() noexcept {}\n'
      '\ttemplate<class U> NodeAlloc(const NodeAlloc<U>&) noexcept {}\n'
      '\tstatic constexpr bool pooled = sizeof(T) <= 128 && alignof(T) <= 16;\n'
      '\tstatic constexpr size_t size_class = (sizeof(T) + 15) / 16 - 1;\n'
      '\tT *allocate(size_t n)\n'
      '\t{\n'
      '\t\tif (pooled && n == 1) {\n'
      '\t\t\tvoid *&head = Lists().head[size_class];\n'
      '\t\t\tif (head) {\n'
      '\t\t\t\tvoid *p = head;\n'
      '\t\t\t\thead = *static_cast<void **>(p);\n'
      '\t\t\t\treturn static_cast<T *>(p);\n'
      '\t\t\t}\n'
      '\t\t\treturn static_cast<T *>(::operator new((size_class + 1) * 16));\n'
      '\t\t}\n'
      '\t\treturn static_cast<T *>(::operator new(n * sizeof(T)));\n'
      '\t}\n'
      '\tvoid deallocate(T *p, size_t n) noexcept\n'
      '\t{\n'
      '\t\tif (pooled && n == 1) {\n'
      '\t\t\tvoid *&head = Lists().head[size_class];\n'
      '\t\t\t*reinterpret_cast<void **>(p) = head;\n'
      '\t\t\thead = p;\n'
      '\t\t\treturn;\n'
      '\t\t}\n'
      '\t\t::operator delete(p);\n'
      '\t}\n'
      '\ttemplate<class U> bool operator==(const NodeAlloc<U>&) const noexcept { return true; }\n'
      '\ttemplate<class U> bool operator!=(const NodeAlloc<U>&) const noexcept { return false; }\n'
      '};\n'
      '} // emusuni\n'
      '#endif\n\n'
      'class LabelManager;\n\nclass Label {\n')
patch('externals/xbyak/xbyak/xbyak.h',
      '\ttypedef XBYAK_STD_UNORDERED_MAP<int, ClabelVal> ClabelDefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_MULTIMAP<int, const JmpLabel> ClabelUndefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_SET<Label*> LabelPtrList;\n',
      '#ifdef EMUSUNI_PORT\n'
      '\ttypedef std::unordered_map<int, ClabelVal, std::hash<int>, std::equal_to<int>, emusuni::NodeAlloc<std::pair<const int, ClabelVal> > > ClabelDefList;\n'
      '\ttypedef std::unordered_multimap<int, const JmpLabel, std::hash<int>, std::equal_to<int>, emusuni::NodeAlloc<std::pair<const int, const JmpLabel> > > ClabelUndefList;\n'
      '\ttypedef std::unordered_set<Label*, std::hash<Label*>, std::equal_to<Label*>, emusuni::NodeAlloc<Label*> > LabelPtrList;\n'
      '#else\n'
      '\ttypedef XBYAK_STD_UNORDERED_MAP<int, ClabelVal> ClabelDefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_MULTIMAP<int, const JmpLabel> ClabelUndefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_SET<Label*> LabelPtrList;\n'
      '#endif\n')

# --- 00.69: ETC1 textures decoded a 4x4 subtile at a time. The tiled decoder called the ETC1 sampler (another file:
# no inlining) for each of a tile's 64 pixels, and the sampler decoded the subtile's two base colours and tables for
# every pixel. Now each subtile's base colours and tables are worked out once (the sampler's own expressions, for its
# two halves) and its 16 pixels only add their modifier (driver Renegade 3D: 00.68's log decoded 4.4 MB of textures in
# one 270 ms frame, mostly ETC1). The same bytes: every subtile and pixel checked against the sampler on the PC.
patch('src/video_core/texture/etc1.h',
      'Common::Vec3<u8> SampleETC1Subtile(u64 value, unsigned int x, unsigned int y);\n',
      'Common::Vec3<u8> SampleETC1Subtile(u64 value, unsigned int x, unsigned int y);\n'
      '#ifdef EMUSUNI_PORT\n'
      '/// EmuSuNi: the 16 pixels of a subtile, out[x * 4 + y] = SampleETC1Subtile(value, x, y).\n'
      'void DecodeETC1Subtile(u64 value, Common::Vec3<u8>* out);\n'
      '#endif\n')
patch('src/video_core/texture/etc1.cpp',
      'Common::Vec3<u8> SampleETC1Subtile(u64 value, unsigned int x, unsigned int y) {\n'
      '    ETC1Tile tile{value};\n'
      '    return tile.GetRGB(x, y);\n'
      '}\n',
      'Common::Vec3<u8> SampleETC1Subtile(u64 value, unsigned int x, unsigned int y) {\n'
      '    ETC1Tile tile{value};\n'
      '    return tile.GetRGB(x, y);\n'
      '}\n'
      '#ifdef EMUSUNI_PORT\n\n'
      '// EmuSuNi: GetRGB for the 16 pixels at once. Its base colour and table depend on x only through x >= 2 (x after the\n'
      '// flip): worked out once for each half, with GetRGB\'s own expressions; each pixel then adds its modifier as GetRGB.\n'
      'void DecodeETC1Subtile(u64 value, Common::Vec3<u8>* out) {\n'
      '    const ETC1Tile tile{value};\n'
      '    Common::Vec3<int> base[2];\n'
      '    unsigned table[2];\n'
      '    for (unsigned int half = 0; half < 2; half++) {\n'
      '        const unsigned int x = half * 2;\n'
      '        Common::Vec3<int> ret;\n'
      '        if (tile.differential_mode) {\n'
      '            ret.r() = static_cast<int>(tile.differential.r);\n'
      '            ret.g() = static_cast<int>(tile.differential.g);\n'
      '            ret.b() = static_cast<int>(tile.differential.b);\n'
      '            if (x >= 2) {\n'
      '                ret.r() += static_cast<int>(tile.differential.dr);\n'
      '                ret.g() += static_cast<int>(tile.differential.dg);\n'
      '                ret.b() += static_cast<int>(tile.differential.db);\n'
      '            }\n'
      '            ret.r() = Common::Color::Convert5To8(ret.r());\n'
      '            ret.g() = Common::Color::Convert5To8(ret.g());\n'
      '            ret.b() = Common::Color::Convert5To8(ret.b());\n'
      '        } else {\n'
      '            if (x < 2) {\n'
      '                ret.r() = Common::Color::Convert4To8(static_cast<u8>(tile.separate.r1));\n'
      '                ret.g() = Common::Color::Convert4To8(static_cast<u8>(tile.separate.g1));\n'
      '                ret.b() = Common::Color::Convert4To8(static_cast<u8>(tile.separate.b1));\n'
      '            } else {\n'
      '                ret.r() = Common::Color::Convert4To8(static_cast<u8>(tile.separate.r2));\n'
      '                ret.g() = Common::Color::Convert4To8(static_cast<u8>(tile.separate.g2));\n'
      '                ret.b() = Common::Color::Convert4To8(static_cast<u8>(tile.separate.b2));\n'
      '            }\n'
      '        }\n'
      '        base[half] = ret;\n'
      '        table[half] = static_cast<int>((x < 2) ? tile.table_index_1.Value() : tile.table_index_2.Value());\n'
      '    }\n'
      '    const bool flip = tile.flip;\n'
      '    for (unsigned int x = 0; x < 4; x++) {\n'
      '        for (unsigned int y = 0; y < 4; y++) {\n'
      '            const int texel = 4 * x + y;\n'
      '            const unsigned int half = (flip ? y : x) >= 2 ? 1 : 0;\n'
      '            int modifier = etc1_modifier_table[table[half]][tile.GetTableSubIndex(texel)];\n'
      '            if (tile.GetNegationFlag(texel))\n'
      '                modifier *= -1;\n'
      '            Common::Vec3<int> ret;\n'
      '            ret.r() = std::clamp(base[half].r() + modifier, 0, 255);\n'
      '            ret.g() = std::clamp(base[half].g() + modifier, 0, 255);\n'
      '            ret.b() = std::clamp(base[half].b() + modifier, 0, 255);\n'
      '            out[x * 4 + y] = ret.Cast<u8>();\n'
      '        }\n'
      '    }\n'
      '}\n'
      '#endif\n')
patch('src/video_core/rasterizer_cache/texture_codec.h',
      '    constexpr bool is_4bit = format == PixelFormat::I4 || format == PixelFormat::A4;\n\n'
      '    for (u32 y = 0; y < 8; y++) {\n',
      '    constexpr bool is_4bit = format == PixelFormat::I4 || format == PixelFormat::A4;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if constexpr (morton_to_linear && is_compressed) {\n'
      '        // EmuSuNi: a subtile\'s 16 pixels from one decode (DecodePixelETC1 for each pixel decoded its subtile\'s\n'
      '        // colours every time), written to the same places with the same alpha.\n'
      '        constexpr bool has_alpha = format == PixelFormat::ETC1A4;\n'
      '        constexpr std::size_t subtile_size = has_alpha ? 16 : 8;\n'
      '        for (u32 subtile = 0; subtile < 4; subtile++) {\n'
      '            const u8* subtile_ptr = tile_buffer.data() + subtile * subtile_size;\n'
      '            u64_le packed_alpha{};\n'
      '            if constexpr (has_alpha) {\n'
      '                std::memcpy(&packed_alpha, subtile_ptr, sizeof(u64));\n'
      '                subtile_ptr += sizeof(u64);\n'
      '            }\n'
      '            Common::Vec3<u8> rgb[16];\n'
      '            Pica::Texture::DecodeETC1Subtile(MakeInt<u64_le>(subtile_ptr), rgb);\n'
      '            const u32 x0 = (subtile % 2) * 4, y0 = (subtile / 2) * 4;\n'
      '            for (u32 sy = 0; sy < 4; sy++) {\n'
      '                for (u32 sx = 0; sx < 4; sx++) {\n'
      '                    u8* dest_pixel = linear_buffer.data() +\n'
      '                                     ((7 - (y0 + sy)) * stride + (x0 + sx)) * linear_bytes_per_pixel;\n'
      '                    std::memcpy(dest_pixel, rgb[sx * 4 + sy].AsArray(), 3);\n'
      '                    u8 alpha = 255;\n'
      '                    if constexpr (has_alpha) {\n'
      '                        alpha = Common::Color::Convert4To8((packed_alpha >> (4 * (sx * 4 + sy))) & 0xF);\n'
      '                    }\n'
      '                    dest_pixel[3] = alpha;\n'
      '                }\n'
      '            }\n'
      '        }\n'
      '        return;\n'
      '    }\n'
      '#endif\n\n'
      '    for (u32 y = 0; y < 8; y++) {\n')

# --- 00.70: the 3DS CPU Clock after a save state's load. A state brings the core timing's timers back made anew, their
# clock scale at 100% (upstream doesn't keep it in the state and never applied the setting again): a game continued
# from a state (the app's Continue, a state loaded from the menu) ran at 100% whatever 3DS CPU Clock said.
patch('src/core/savestate.cpp',
      '    // Deserialize\n    iarchive ia{stream};\n    ia&* this;\n',
      '    // Deserialize\n    iarchive ia{stream};\n    ia&* this;\n'
      '    // EmuSuNi: the CPU clock again (the timers come back made anew, at 100%).\n'
      '    if (timing) {\n'
      '        timing->UpdateClockSpeed(Settings::values.cpu_clock_percentage.GetValue());\n'
      '    }\n')
patch('src/core/savestate.cpp',
      '#include "core/core.h"\n',
      '#include "core/core.h"\n#include "core/core_timing.h"\n')
# --- 00.70: less work per draw on the emulation thread (the GPU's commands were 40-45% of it in Hey! Pikmin's busy
# scenes on the PS4: 230-450 draws a frame, ~20 us each). The same Vulkan commands and descriptors, fewer made:
#   - a draw's texture and utility descriptor sets: upstream took a new set from the pool and wrote its 3-4 images for
#     every draw (930 descriptor writes a frame in Pikmin); now a draw whose writes are the same as the last draw's, in
#     the same submission (its set can't have gone back to the pool before that submission ran), binds that set again;
#   - the pipeline: upstream hashed the draw's whole pipeline state twice and looked both hashes up for every draw;
#     now a state equal to the last draw's (byte for byte) gives its pipeline at once;
#   - two short loops that clang (-march=btver2, with the PGO profile's small trip counts) vectorized with masked
#     loads and stores (vmaskmovps: microcoded on the PS4's Jaguar): the command lists' runs of registers, the fixed
#     vertex attributes' layout.
patch('src/video_core/renderer_vulkan/vk_rasterizer.h',
      '    /// Binds a texture cube to texture unit 0\n'
      '    void BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture,\n'
      '                         vk::DescriptorSet texture_set);\n',
      '    /// Binds a texture cube to texture unit 0\n'
      '    void BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture,\n'
      '                         vk::DescriptorSet texture_set);\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a descriptor set\'s image writes, gathered for a draw (SyncTextureUnits, SyncUtilityTextures),\n'
      '    // and the last set made from them: the same writes in the same submission bind it again (EmuSuNiCommitSet).\n'
      '    struct EmuSuNiImageWrite {\n'
      '        vk::ImageView view{};\n'
      '        vk::Sampler sampler{};\n'
      '        u8 binding = 0;\n'
      '        u8 array_index = 0;\n'
      '        bool storage = false;\n'
      '        bool operator==(const EmuSuNiImageWrite&) const = default;\n'
      '    };\n'
      '    struct EmuSuNiSetCache {\n'
      '        std::array<EmuSuNiImageWrite, 12> writes{};\n'
      '        u32 count = 0;\n'
      '        u64 tick = ~0ULL;\n'
      '        vk::DescriptorSet set{};\n'
      '    };\n'
      '    void EmuSuNiAddImage(u8 binding, u8 array_index, vk::ImageView view, vk::Sampler sampler, bool storage);\n'
      '    void EmuSuNiCommitSet(EmuSuNiSetCache& cache, DescriptorHeapType type);\n'
      '    std::array<EmuSuNiImageWrite, 12> emusuni_writes{};\n'
      '    u32 emusuni_write_count = 0;\n'
      '    EmuSuNiSetCache emusuni_texture_set;\n'
      '    EmuSuNiSetCache emusuni_utility_set;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    /// Sets the dynamic offset for the uniform buffer at binding\n',
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: binds a descriptor set taken earlier in this submission again (RasterizerVulkan::EmuSuNiCommitSet).\n'
      '    void EmuSuNiRebind(DescriptorHeapType type, vk::DescriptorSet descriptor_set) {\n'
      '        bound_descriptor_sets[static_cast<u32>(type)] = descriptor_set;\n'
      '    }\n'
      '#endif\n'
      '\n'
      '    /// Sets the dynamic offset for the uniform buffer at binding\n')
# The texture units and the utility textures: the same surfaces, samplers and copies as upstream in the same order;
# only their descriptor writes are gathered, then committed (EmuSuNiCommitSet). (Upstream's two functions kept for
# the other builds, after #else.)
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {\n',
      r'''#ifdef EMUSUNI_PORT
void RasterizerVulkan::EmuSuNiAddImage(u8 binding, u8 array_index, vk::ImageView view, vk::Sampler sampler,
                                       bool storage) {
    if (emusuni_write_count < emusuni_writes.size()) {
        emusuni_writes[emusuni_write_count++] = {view, sampler, binding, array_index, storage};
    }
}

void RasterizerVulkan::EmuSuNiCommitSet(EmuSuNiSetCache& cache, DescriptorHeapType type) {
    // The set made for an earlier draw of this submission (taken from the pool at this tick, it goes back only once
    // the GPU has run the submission) with the same writes: bound again, nothing written.
    const u64 tick = scheduler.CurrentTick();
    if (cache.set && cache.tick == tick && cache.count == emusuni_write_count &&
        std::equal(emusuni_writes.begin(), emusuni_writes.begin() + emusuni_write_count, cache.writes.begin())) {
        pipeline_cache.EmuSuNiRebind(type, cache.set);
        return;
    }
    const vk::DescriptorSet set = pipeline_cache.Acquire(type);
    for (u32 i = 0; i < emusuni_write_count; i++) {
        const EmuSuNiImageWrite& write = emusuni_writes[i];
        if (write.storage) {
            update_queue.AddStorageImage(set, write.binding, write.view);
        } else {
            update_queue.AddImageSampler(set, write.binding, write.array_index, write.view, write.sampler);
        }
    }
    std::copy(emusuni_writes.begin(), emusuni_writes.begin() + emusuni_write_count, cache.writes.begin());
    cache.count = emusuni_write_count;
    cache.tick = tick;
    cache.set = set;
}

void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {
    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
    using CubeFace = Pica::TexturingRegs::CubeFace;

    const auto pica_textures = regs.texturing.GetTextures();
    emusuni_write_count = 0;

    for (u32 texture_index = 0; texture_index < pica_textures.size(); ++texture_index) {
        const auto& texture = pica_textures[texture_index];

        // If the texture unit is disabled bind a null surface to it
        if (!texture.enabled) {
            switch (texture.config.type.Value()) {
            case TextureType::TextureCube:
            case TextureType::ShadowCube: {
                Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_CUBE_ID);
                const Sampler& null_sampler = res_cache.GetSampler(VideoCore::NULL_SURFACE_CUBE_ID);
                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, null_surface.ImageView(),
                                null_sampler.Handle(), false);
                break;
            }
            default: {
                Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
                const Sampler& null_sampler = res_cache.GetSampler(VideoCore::NULL_SURFACE_ID);
                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, null_surface.ImageView(),
                                null_sampler.Handle(), false);
                break;
            }
            }
            continue;
        }

        // Handle special tex0 configurations
        if (texture_index == 0) {
            switch (texture.config.type.Value()) {
            case TextureType::Shadow2D: {
                Surface& surface = res_cache.GetTextureSurface(texture);
                Sampler& sampler = res_cache.GetSampler(texture.config);
                surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, surface.StorageView(), sampler.Handle(), false);
                continue;
            }
            case TextureType::ShadowCube: {
                // (BindShadowCube's work, its writes gathered)
                auto info = Pica::Texture::TextureInfo::FromPicaRegister(texture.config, texture.format);
                constexpr std::array faces = {
                    CubeFace::PositiveX, CubeFace::NegativeX, CubeFace::PositiveY,
                    CubeFace::NegativeY, CubeFace::PositiveZ, CubeFace::NegativeZ,
                };
                Sampler& sampler = res_cache.GetSampler(texture.config);
                for (CubeFace face : faces) {
                    const u32 binding = static_cast<u32>(face);
                    info.physical_address = regs.texturing.GetCubePhysicalAddress(face);
                    const VideoCore::SurfaceId surface_id = res_cache.GetTextureSurface(info);
                    Surface& surface = res_cache.GetSurface(surface_id);
                    surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
                    EmuSuNiAddImage(0, static_cast<u8>(binding), surface.StorageView(), sampler.Handle(), false);
                }
                continue;
            }
            case TextureType::TextureCube: {
                // (BindTextureCube's work, its write gathered)
                const VideoCore::TextureCubeConfig config = {
                    .px = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX),
                    .nx = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX),
                    .py = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY),
                    .ny = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY),
                    .pz = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ),
                    .nz = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ),
                    .width = texture.config.width,
                    .levels = texture.config.lod.max_level + 1,
                    .format = texture.format,
                };
                Surface& surface = res_cache.GetTextureCube(config);
                Sampler& sampler = res_cache.GetSampler(texture.config);
                EmuSuNiAddImage(0, 0, surface.ImageView(), sampler.Handle(), false);
                continue;
            }
            default:
                break;
            }
        }

        // Bind the texture provided by the rasterizer cache
        Surface& surface = res_cache.GetTextureSurface(texture);
        Sampler& sampler = res_cache.GetSampler(texture.config);
        const vk::ImageView color_view = framebuffer->ImageView(SurfaceType::Color);
        const bool is_feedback_loop = color_view == surface.FramebufferView();
        const vk::ImageView texture_view =
            is_feedback_loop ? surface.CopyImageView() : surface.ImageView();
        EmuSuNiAddImage(static_cast<u8>(texture_index), 0, texture_view, sampler.Handle(), false);
    }
    EmuSuNiCommitSet(emusuni_texture_set, DescriptorHeapType::Texture);
}

void RasterizerVulkan::SyncUtilityTextures(const Framebuffer* framebuffer) {
    const bool shadow_writing = regs.framebuffer.IsShadowRendering();
    bool shadow_reading = regs.lighting.config0.enable_shadow;
    // Ensure the shadow-texture slot is actually enabled
    if (shadow_reading) {
        const u32 shadow_texture_unit = regs.lighting.config0.shadow_selector.Value();
        const auto shadow_texture = regs.texturing.GetTextures()[shadow_texture_unit];
        shadow_reading &= shadow_texture.enabled;
    }

    // Reading and writing are mutually exclusive
    assert(!(shadow_writing && shadow_reading));

    emusuni_write_count = 0;
    if (shadow_writing) {
        EmuSuNiAddImage(0, 0, framebuffer->ImageView(SurfaceType::Color), {}, true);
    } else if (shadow_reading) {
        const u32 shadow_texture_unit = regs.lighting.config0.shadow_selector.Value();
        const auto shadow_texture = regs.texturing.GetTextures()[shadow_texture_unit];
        Surface& shadow_surface = res_cache.GetTextureSurface(shadow_texture);
        EmuSuNiAddImage(0, 0, shadow_surface.StorageView(), {}, true);
    } else {
        Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
        EmuSuNiAddImage(0, 0, null_surface.StorageView(), {}, true);
    }
    EmuSuNiCommitSet(emusuni_utility_set, DescriptorHeapType::Utility);
}
#else
void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        update_queue.AddStorageImage(utility_set, 0, null_surface.StorageView());\n'
      '    }\n'
      '}\n'
      '\n'
      'void RasterizerVulkan::BindShadowCube(',
      '        update_queue.AddStorageImage(utility_set, 0, null_surface.StorageView());\n'
      '    }\n'
      '}\n'
      '#endif\n'
      '\n'
      'void RasterizerVulkan::BindShadowCube(')
# The pipeline of the last draw's state.
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      'GraphicsPipeline* ShaderDiskCache::GetPipeline(const PipelineInfo& info) {\n',
      'GraphicsPipeline* ShaderDiskCache::GetPipeline(const PipelineInfo& info) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the same state as the last draw\'s (byte for byte: its two hashes are the same): its pipeline.\n'
      '    if (emusuni_last_pipeline != nullptr &&\n'
      '        std::memcmp(&emusuni_last_state, &info.state, sizeof(info.state)) == 0) {\n'
      '        return emusuni_last_pipeline;\n'
      '    }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      '        AppendPLConfig(pl_cache, entry, hash);\n'
      '    }\n'
      '\n'
      '    return it.value().get();\n'
      '}\n',
      '        AppendPLConfig(pl_cache, entry, hash);\n'
      '    }\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::memcpy(&emusuni_last_state, &info.state, sizeof(info.state));\n'
      '    emusuni_last_pipeline = it.value().get();\n'
      '#endif\n'
      '    return it.value().get();\n'
      '}\n')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.h',
      '    std::unordered_set<u64> known_graphic_pipelines;\n',
      '    std::unordered_set<u64> known_graphic_pipelines;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the last state GetPipeline was asked for, and its pipeline (owned by graphics_pipelines).\n'
      '    StaticPipelineInfo emusuni_last_state{};\n'
      '    GraphicsPipeline* emusuni_last_pipeline = nullptr;\n'
      '#endif\n')
# No masked moves (vmaskmovps) in two loops of every draw on the Jaguar.
patch('src/video_core/pica/pica_core.cpp',
      '            u32* __restrict batch_dst = dst + offset;\n'
      '            const u32* __restrict batch_src = values + offset;\n'
      '            for (u32 i = 0; i < batch_count; ++i) {\n',
      '            u32* __restrict batch_dst = dst + offset;\n'
      '            const u32* __restrict batch_src = values + offset;\n'
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi: a few registers at a time; clang folded the loop\'s end into masked moves (vmaskmovps,\n'
      '            // microcoded on the PS4\'s Jaguar).\n'
      '#pragma clang loop vectorize(disable)\n'
      '#endif\n'
      '            for (u32 i = 0; i < batch_count; ++i) {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    // Loop one more time to find unused attributes and assign them to the default one\n'
      '    // If the attribute is just disabled, shove the default attribute to avoid\n'
      '    // errors if the shader ever decides to use it.\n'
      '    for (u32 i = 0; i < 16; i++) {\n',
      '    // Loop one more time to find unused attributes and assign them to the default one\n'
      '    // If the attribute is just disabled, shove the default attribute to avoid\n'
      '    // errors if the shader ever decides to use it.\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (EmuSuNi: not vectorized - its conditional stores were masked moves, microcoded on the Jaguar.)\n'
      '#pragma clang loop vectorize(disable)\n'
      '#endif\n'
      '    for (u32 i = 0; i < 16; i++) {\n')
# --- 00.70: the stall log divides the GPU's commands, and counts the game's own pictures. 00.69's log (Hey! Pikmin's
# busy scenes on the PS4): "GPU commands" 3.6-5.6 s of the emulation thread's 600 frames, as much as the guest's code;
# on the PC the same scenes make 230 draws a frame, a few microseconds each, and the game draws 30 pictures a second
# (every other frame). Now each slow frame and each 600-frame line say how many draws (and vertices) the GPU's
# commands made and the time the draws took of them (the rest: the command lists' registers), the command lists'
# size, the draws whose vertices the CPU had to shade (none in the games tried), and how many pictures the game drew
# (its top screen's buffer swaps: 300 in 600 frames is a 30 FPS game).
patch('src/common/emusuni_stats.h',
      'enum TickCounter { GuestTicks, JitTicks, SvcTicks, GpuTicks, GpuInSvcTicks, DspTicks, SurfaceTicks, TickCount };\n'
      'enum CountCounter { JitBlocks, JitFlushes, SvcCalls, Surfaces, DrawsSkipped, CountCount };\n',
      'enum TickCounter {\n'
      '    GuestTicks, JitTicks, SvcTicks, GpuTicks, GpuInSvcTicks, DspTicks, SurfaceTicks,\n'
      '    DrawTicks, // (00.70) the draws (PicaCore::DrawArrays: the vertices set up, the draw recorded)\n'
      '    TickCount\n'
      '};\n'
      'enum CountCounter {\n'
      '    JitBlocks, JitFlushes, SvcCalls, Surfaces, DrawsSkipped,\n'
      '    Draws, DrawVertices, SoftwareDraws, ListBytes, GamePictures, // (00.70)\n'
      '    CountCount\n'
      '};\n')
patch('src/common/emusuni_frame_stats.h',
      '        const std::uint64_t hle = svc > gpu_in_svc ? svc - gpu_in_svc : 0;\n',
      '        const std::uint64_t hle = svc > gpu_in_svc ? svc - gpu_in_svc : 0;\n'
      '        const std::string on_cpu =\n'
      '            dc[SoftwareDraws] != 0 ? fmt::format(", {} of them shaded on the CPU", dc[SoftwareDraws]) : std::string();\n')
patch('src/common/emusuni_frame_stats.h',
      '                           "calls), GPU commands {} ms (surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "\n',
      '                           "calls), GPU commands {} ms ({} draws of {} vertices{}, {} ms of it the draws; {} KiB of "\n'
      '                           "command lists; surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "\n')
patch('src/common/emusuni_frame_stats.h',
      '                           "({} ms) and {} KiB elsewhere ({} ms)",\n',
      '                           "({} ms) and {} KiB elsewhere ({} ms); the game drew {} pictures",\n')
patch('src/common/emusuni_frame_stats.h',
      '                           t_ms(dt[GpuTicks]), dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),\n',
      '                           t_ms(dt[GpuTicks]), dc[Draws], dc[DrawVertices], on_cpu, t_ms(dt[DrawTicks]),\n'
      '                           dc[ListBytes] / 1024, dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),\n')
patch('src/common/emusuni_frame_stats.h',
      '                           d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024, d[ReadUsOther] / 1000);\n',
      '                           d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024, d[ReadUsOther] / 1000,\n'
      '                           dc[GamePictures]);\n')
# The draws (the PICA's draw registers): accelerated (the GPU shades the vertices) or not (the CPU shades them).
patch('src/video_core/pica/pica_core.cpp',
      '#include "video_core/pica/pica_core.h"\n',
      '#include "video_core/pica/pica_core.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/pica/pica_core.cpp',
      '    // Attempt to use hardware vertex shaders if possible.\n'
      '    if (accelerate_draw && rasterizer->AccelerateDrawBatch(is_indexed)) {\n'
      '        return;\n'
      '    }\n'
      '\n'
      '    // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.\n'
      '    LoadVertices(is_indexed);\n'
      '\n'
      '    // Draw emitted triangles.\n'
      '    rasterizer->DrawTriangles();\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the draws and their time, for the log (common/emusuni_frame_stats.h).\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::Draws]++;\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::DrawVertices] += regs.internal.pipeline.num_vertices;\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::DrawTicks};\n'
      '        // Attempt to use hardware vertex shaders if possible.\n'
      '        if (accelerate_draw && rasterizer->AccelerateDrawBatch(is_indexed)) {\n'
      '            return;\n'
      '        }\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::SoftwareDraws]++;\n'
      '\n'
      '        // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.\n'
      '        LoadVertices(is_indexed);\n'
      '\n'
      '        // Draw emitted triangles.\n'
      '        rasterizer->DrawTriangles();\n'
      '    }\n'
      '#else\n'
      '    // Attempt to use hardware vertex shaders if possible.\n'
      '    if (accelerate_draw && rasterizer->AccelerateDrawBatch(is_indexed)) {\n'
      '        return;\n'
      '    }\n'
      '\n'
      '    // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.\n'
      '    LoadVertices(is_indexed);\n'
      '\n'
      '    // Draw emitted triangles.\n'
      '    rasterizer->DrawTriangles();\n'
      '#endif\n')
# The command lists' size; the game's pictures (its top screen's buffer swaps).
patch('src/video_core/gpu.cpp',
      '    case CommandId::SubmitCmdList: {\n'
      '        auto& params = command.submit_gpu_cmdlist;\n',
      '    case CommandId::SubmitCmdList: {\n'
      '        auto& params = command.submit_gpu_cmdlist;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::ListBytes] += params.size;\n'
      '#endif\n')
patch('src/video_core/gpu.cpp',
      '    if (screen_id == 0) {\n        MicroProfileFlip();\n',
      '    if (screen_id == 0) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::GamePictures]++;\n'
      '#endif\n'
      '        MicroProfileFlip();\n')
# --- 00.71: the stall log counts the surfaces read back from the GPU. 3DS Resolution goes up to 4x with the Quality
# levels: on the PC a bigger size cost the emulation thread nothing in the games tried (Hey! Pikmin, Super Mario 3D
# Land: the same CPU time a frame at 1x to 4x, no surface read back), but a game that reads a picture the GPU drew back
# into the 3DS's memory (RasterizerCache::DownloadSurface: a CPU read of it, a copy to ordinary memory) waits for the
# GPU's work - more of it at a bigger size, which also makes the surface smaller first. Each slow frame and each
# 600-frame line say how many surfaces were read back, their size and the emulation thread's time in it.
patch('src/common/emusuni_stats.h',
      '    DrawTicks, // (00.70) the draws (PicaCore::DrawArrays: the vertices set up, the draw recorded)\n',
      '    DrawTicks, // (00.70) the draws (PicaCore::DrawArrays: the vertices set up, the draw recorded)\n'
      '    ReadbackTicks, // (00.71) the surfaces read back from the GPU (RasterizerCache::DownloadSurface)\n')
patch('src/common/emusuni_stats.h',
      '    Draws, DrawVertices, SoftwareDraws, ListBytes, GamePictures, // (00.70)\n',
      '    Draws, DrawVertices, SoftwareDraws, ListBytes, GamePictures, // (00.70)\n'
      '    Readbacks, ReadbackBytes, // (00.71)\n')
patch('src/common/emusuni_frame_stats.h',
      '                           "command lists; surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "\n',
      '                           "command lists; surfaces made {}, {} ms; read back {} ({} KiB, {} ms)), audio {} ms; "\n'
      '                           "waited {} ms for the GPU "\n')
patch('src/common/emusuni_frame_stats.h',
      '                           dc[ListBytes] / 1024, dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),\n',
      '                           dc[ListBytes] / 1024, dc[Surfaces], t_ms(dt[SurfaceTicks]), dc[Readbacks],\n'
      '                           dc[ReadbackBytes] / 1024, t_ms(dt[ReadbackTicks]), t_ms(dt[DspTicks]),\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      'void RasterizerCache<T>::DownloadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_DownloadSurface);\n',
      'void RasterizerCache<T>::DownloadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_DownloadSurface);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a surface read back into the 3DS\'s memory, for the log (common/emusuni_frame_stats.h).\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::ReadbackTicks};\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::Readbacks]++;\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::ReadbackBytes] += boost::icl::length(interval);\n'
      '#endif\n')
# --- 00.81: the players' graphics errors (Pokémon X and Y, Majora's Mask 3D, Metroid: Samus Returns, Castlevania:
# Mirror of Fate). Three changes:
# - The right eye per game: the app's 3DS Right Eye (Auto: drawn for the games that break without it, Emulator.cpp
#   ThreedsRightEyeGame) comes as an option of the core's own, citra_emusuni_right_eye (declared, so that the app,
#   which gives Azahar only the options it declares - StrictOptions -, can send it); the file
#   /data/EmuSuNi/3ds-right-eye still draws it in every game.
# - Upstream's fix of the right eye's guess (azahar PR #2467, merged 2026-10-23, after 2126.2): a command list that
#   draws nothing is never taken for a repeat of the top screen and dropped. With the right eye left out, Majora's Mask
#   3D's intro stayed black, Metroid: Samus Returns' bottom screen was drawn out of place (azahar #740), the Ace
#   Attorney games' bottom screen black; upstream measured Ocarina of Time 3D still leaving out 4141 lists a run (4441
#   before). Its known cost: Fire Emblem Warriors' bottom screen black (the app's Auto draws its right eye).
# - Accurate multiplication: what the core used (the setting, or its hack list's forced titles) said in the log once
#   for each game (3DS Accurate Shader Multiplication: the app's, Auto by title ID).
patch('src/citra_libretro/core_settings.cpp',
      'static constexpr const char* shaders_accurate_mul = citra_setting(BaseKeys::shaders_accurate_mul);\n',
      'static constexpr const char* shaders_accurate_mul = citra_setting(BaseKeys::shaders_accurate_mul);\n'
      '#ifdef EMUSUNI_PORT\n'
      'static constexpr const char* emusuni_right_eye = "citra_emusuni_right_eye"; // (EmuSuNi\'s: the right eye drawn)\n'
      '#endif\n')
patch('src/citra_libretro/core_settings.cpp',
      '        "Use accurate multiplication in shaders. "\n'
      '        "More accurate but can reduce performance. Only works with hardware shaders.",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::enabled, "Enabled" },\n'
      '            { config::disabled, "Disabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::enabled\n'
      '    },\n',
      '        "Use accurate multiplication in shaders. "\n'
      '        "More accurate but can reduce performance. Only works with hardware shaders.",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::enabled, "Enabled" },\n'
      '            { config::disabled, "Disabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::enabled\n'
      '    },\n'
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        config::graphics::emusuni_right_eye,\n'
      '        "Right Eye (EmuSuNi)",\n'
      '        "Right Eye",\n'
      '        "Draw the right eye\'s picture of the 3D (never shown on the PS4; a few games need it drawn).",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { config::enabled, "Enabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n'
      '#endif\n')
patch('src/citra_libretro/core_settings.cpp',
      '    Settings::values.disable_right_eye_render = std::getenv("EMUSUNI_3DS_RIGHT_EYE") == nullptr &&\n'
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n'
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for (the object shows a few\n',
      '    // (00.81: and the app\'s 3DS Right Eye, per game.)\n'
      '    Settings::values.disable_right_eye_render =\n'
      '        std::getenv("EMUSUNI_3DS_RIGHT_EYE") == nullptr && ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0 &&\n'
      '        LibRetro::FetchVariable(config::graphics::emusuni_right_eye, config::disabled) != config::enabled;\n'
      '    {\n'
      '        static int said = -1; // (said once, and again if it changes)\n'
      '        const int now = Settings::values.disable_right_eye_render.GetValue() ? 1 : 0;\n'
      '        if (now != said) {\n'
      '            LOG_INFO(Frontend, "EmuSuNi: right eye: {}",\n'
      '                     now ? "not drawn (the PS4 shows no 3D)"\n'
      '                         : "drawn (3DS Right Eye, or the file /data/EmuSuNi/3ds-right-eye)");\n'
      '            said = now;\n'
      '        }\n'
      '    }\n'
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for (the object shows a few\n')
# Upstream's PR #2467 (src/video_core/pica/pica_core.cpp, pica_core.h, right_eye_disabler.cpp), as merged.
patch('src/video_core/pica/pica_core.h',
      '        bool vp_heigh_found = false;\n        bool paddr_found = false;\n',
      '        bool vp_heigh_found = false;\n        bool paddr_found = false;\n'
      '        // Whether the scanned list issues any geometry.\n        bool has_draw = false;\n')
patch('src/video_core/pica/pica_core.cpp',
      'PicaCore::RenderPropertiesGuess PicaCore::GuessCmdRenderProperties(PAddr list, u32 size) {\n'
      '    // Initialize command list tracking.\n'
      '    const u8* head = memory.GetPhysicalPointer(list);\n'
      '    cmd_list.Reset(list, head, size);\n'
      '\n'
      '    constexpr size_t max_iterations = 0x100;\n'
      '\n'
      '    RenderPropertiesGuess find_info{};\n'
      '\n'
      '    find_info.vp_height = regs.internal.rasterizer.viewport_size_y.Value();\n'
      '    find_info.paddr = regs.internal.framebuffer.framebuffer.color_buffer_address.Value() * 8;\n'
      '\n',
      'PicaCore::RenderPropertiesGuess PicaCore::GuessCmdRenderProperties(PAddr list, u32 size) {\n'
      '    constexpr size_t max_iterations = 0x100;\n'
      '\n'
      '    RenderPropertiesGuess find_info{};\n'
      '\n'
      '    find_info.vp_height = regs.internal.rasterizer.viewport_size_y.Value();\n'
      '    find_info.paddr = regs.internal.framebuffer.framebuffer.color_buffer_address.Value() * 8;\n'
      '\n'
      '    // Initialize command list tracking.\n'
      '    const u8* head = memory.GetPhysicalPointer(list);\n'
      '    if (head == nullptr) {\n'
      '        return find_info;\n'
      '    }\n'
      '    cmd_list.Reset(list, head, size);\n'
      '\n')
patch('src/video_core/pica/pica_core.cpp',
      '            find_info.paddr = value * 8;\n'
      '            find_info.paddr_found = true;\n'
      '            break;\n'
      '        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):\n',
      '            find_info.paddr = value * 8;\n'
      '            find_info.paddr_found = true;\n'
      '            break;\n'
      '        case PICA_REG_INDEX(pipeline.trigger_draw):\n'
      '        case PICA_REG_INDEX(pipeline.trigger_draw_indexed):\n'
      '            find_info.has_draw = true;\n'
      '            break;\n'
      '        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):\n')
patch('src/video_core/pica/pica_core.cpp',
      '            const u8* head = memory.GetPhysicalPointer(addr);\n'
      '            cmd_list.Reset(addr, head, size);\n'
      '            break;\n'
      '        }\n'
      '        default:\n'
      '            break;\n'
      '        }\n'
      '        return find_info.vp_heigh_found && find_info.paddr_found;\n',
      '            const u8* head = memory.GetPhysicalPointer(addr);\n'
      '            if (head != nullptr) {\n'
      '                cmd_list.Reset(addr, head, size);\n'
      '            }\n'
      '            break;\n'
      '        }\n'
      '        default:\n'
      '            break;\n'
      '        }\n'
      '        return find_info.vp_heigh_found && find_info.paddr_found && find_info.has_draw;\n')
patch('src/video_core/right_eye_disabler.cpp',
      '    if (guess.vp_height == top_screen_size && !top_screen_blocked) {\n',
      '    if (guess.has_draw && guess.vp_height == top_screen_size && !top_screen_blocked) {\n')
# EmuSuNi's limit on it: only a list scanned to its end without a draw counts as drawing nothing. A list cut off by the
# scan's 256 commands may draw further on, and is taken as before #2467: as merged, Super Mario 3D Land's right eye
# wasn't left out at all (PC, World 1-1: 133,726 draws in 600 frames against 70,613, the emulation thread 2.9 against
# 2.1 ms a frame - all 00.63's saving gone); with this limit 70,613 again and the same pictures.
patch('src/video_core/pica/pica_core.cpp',
      '        iterations++;\n    }\n\n    return find_info;\n}\n',
      '        iterations++;\n    }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a list scanned only in part may still draw (scripts/build-3ds-core.sh, 00.81).\n'
      '    if (!find_info.has_draw && iterations >= max_iterations) {\n'
      '        find_info.has_draw = true;\n'
      '    }\n'
      '#endif\n'
      '    return find_info;\n}\n')
# Accurate multiplication as the game got it, in the log (once a game; again if it changes).
patch('src/video_core/gpu.cpp',
      '#include "common/hacks/hack_manager.h"\n',
      '#include "common/hacks/hack_manager.h"\n#include "common/logging/log.h"\n')
patch('src/video_core/gpu.cpp',
      '    impl->rasterizer->SetAccurateMul(use_accurate_mul);\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        static u64 said_for = 0;\n'
      '        static int said = -1;\n'
      '        if (said_for != program_ID || said != (use_accurate_mul ? 1 : 0)) {\n'
      '            LOG_INFO(Frontend, "EmuSuNi: accurate multiplication {} for {:016X} (the setting {}{})",\n'
      '                     use_accurate_mul ? "on" : "off", program_ID,\n'
      '                     Settings::values.shaders_accurate_mul.GetValue() ? "on" : "off",\n'
      '                     hack && hack->mode == Common::Hacks::HackAllowMode::FORCE ? "; forced on by the hack list"\n'
      '                     : hack && hack->mode == Common::Hacks::HackAllowMode::DISALLOW ? "; off by the hack list"\n'
      '                                                                                   : "");\n'
      '            said_for = program_ID;\n'
      '            said = use_accurate_mul ? 1 : 0;\n'
      '        }\n'
      '    }\n'
      '#endif\n'
      '    impl->rasterizer->SetAccurateMul(use_accurate_mul);\n}\n')

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
# PGO (profile-guided optimisation): scripts/3ds-core.profdata.xz, a profile of the PC build (CTR_CORE_HOST=1)
# playing the starts of Super Mario 3D Land, Hey! Pikmin, The Legend of Zelda: Ocarina of Time 3D and Driver
# Renegade 3D, 3000 frames each, twice (the second with the JIT warm-up's list: 00.69's code),
# used by both builds when it's there (CTR_CORE_PGO=off: without it). A profile older than the sources only
# optimises less (the functions that changed get none). A new one: CTR_CORE_HOST=1 CTR_CORE_PGO=generate
# build-3ds-core.sh <dir> (instrumented), the PC test frontend linked with -fprofile-instr-generate and run on the
# games with LLVM_PROFILE_FILE=<dir>/%p.profraw, then llvm-profdata-21 merge -o 3ds-core.profdata <dir>/*.profraw;
# xz -9 it into scripts/. (The profile holds function counters only, nothing of the games.)
PROFILE="$HERE/3ds-core.profdata.xz"
PGO="none"
case "${CTR_CORE_PGO:-use}" in
  generate)
    if [ "$HOST" != 1 ]; then
      echo "Azahar: CTR_CORE_PGO=generate is for the PC build (CTR_CORE_HOST=1)"
      exit 1
    fi
    FLAGS_CORE+=" -fprofile-instr-generate"
    PGO="generate" ;;
  off) ;;
  *)
    if [ -f "$PROFILE" ]; then
      mkdir -p "$OUT"
      xz -dc "$PROFILE" > "$OUT/3ds-core.profdata.new"
      if cmp -s "$OUT/3ds-core.profdata.new" "$OUT/3ds-core.profdata"; then
        rm -f "$OUT/3ds-core.profdata.new"
      else
        mv "$OUT/3ds-core.profdata.new" "$OUT/3ds-core.profdata"
      fi
      FLAGS_CORE+=" -fprofile-instr-use=$OUT/3ds-core.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
      PGO="profile $(sha256sum "$PROFILE" | cut -c1-16)"
    fi ;;
esac
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
WANT="$SRC | $TREE_COMMIT | host $HOST | pgo $PGO | ${OPTIONS[*]} | $(sed -n '1,/^# The checks\.$/p' "${BASH_SOURCE[0]}" | sha256sum | cut -d' ' -f1)"
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
