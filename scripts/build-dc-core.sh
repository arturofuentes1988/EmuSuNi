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
# <system>/dc has no dc_boot.bin (the flash, dc_nvmem.bin, Flycast makes itself). Its libretro API
# becomes flycast_retro_* and everything else in it private, as for FCEUmm (CMakeLists.txt). The PS4
# changes to its sources
# (applied each time to the files as git has them, checked; all under EMUSUNI_PORT, which the CMake
# option of that name defines):
#   - Fast memory, by default (core/linux/emusuni_vmem.cpp, adapted from Resonance's shell/ps4/
#     ps4_vmem.cpp: https://github.com/iHaiDeeZ/Resonance, GPL-2.0, a Flycast-based Dreamcast emulator
#     for the PS4 by the author of DolphinPS4; the method and most of the code are Resonance's): the
#     recompiled SH4 code reads and writes the Dreamcast's RAM, VRAM and sound RAM directly instead of
#     calling the memory handlers. The console mirrors its memory (the RAM four times, the VRAM and
#     sound RAM too) in a 512 MiB range, which the PS4 doesn't map the usual way: one block of direct
#     memory (the SH4 context with its 128 MiB table of compiled blocks, then the RAM, VRAM and sound
#     RAM: 218 MiB) is mapped into one sceKernelReserveVirtualRange reservation (652 MiB at 0x2800000000
#     or the next of a few hints, well above the app's heap) as often as the console mirrors it, which
#     needs sceKernelEnableDmemAliasing (PPSSPP's mirrors came out "not aliased": its port never calls
#     it; the first Dreamcast game of a session turns it on for the whole process); the table
#     in 2 MiB pieces on the first access, 2 MiB pages where the addresses allow, else 16 KiB ones. An
#     access to the hardware's addresses (not mapped) faults once and is rewritten as a call of the
#     memory handlers (the fault handler below). Tried when the core starts (retro_init): the kernel
#     maps the same memory twice and the two are the same; a read of unmapped memory faults and the
#     handler sees its address, pc, stack, r9 and rdi; a write to a locked page faults and unlocking
#     lets it through. If any of that, the 218 MiB, the address space or a mapping (also later, at the
#     game's start) is refused, or the switch below says so, the log has one line, "EmuSuNi: vmem: fast
#     memory off: <why>", and the same process goes on with Flycast's own fallback, the next point. A
#     start that dies inside those kernel calls (a crash, or a hang the player ends) leaves
#     /data/EmuSuNi/dc-fastmem-trying (until the first game's memory is mapped): the next start makes
#     dc-fastmem-crashed of it and doesn't try the fast memory again until that file is deleted.
#   - Without it, Flycast's own fallback (nvmem disabled): the recompiled code goes through the memory
#     handlers (~3% slower on this PC's RAM-heavy test, much more on the PS4's Jaguar). RAM (16 MiB),
#     VRAM (8 MiB), sound RAM (2 MiB) and the SH4 context with its 128 MiB table of compiled blocks
#     each in a block of their own, 16 KiB aligned, off the app's heap: direct memory on the PS4
#     (sceKernelAllocateDirectMemory, sceKernelMapDirectMemory), an mmap elsewhere (oslib/
#     emusuni_mem.cpp). Freed by retro_deinit, allocated again by every game's start; the SH4
#     executors follow the context wherever it lands (upstream's kept its first address: a crash
#     when it moved). The table is reset only where blocks were (upstream wrote all 128 MiB at every
#     flush of the code cache).
#   - Switches for the speed changes (this one, the dispatch and the hot pages below): on, unless
#     EMUSUNI_DC_FASTMEM=0, EMUSUNI_DC_DISPATCH=0 or EMUSUNI_DC_HOTPAGES=0 is in the environment or, on
#     the PS4 (nothing sets environment variables there), the file /data/EmuSuNi/dc-fastmem-off,
#     dc-dispatch-off or dc-hotpages-off exists: to compare, or to get a game going if one breaks it,
#     without building the app again (core/emusuni_options.h; the log says which are off and why).
#   - Pages are still write-protected (mprotect) as Flycast wants: the textures' VRAM (a write drops
#     the texture) and the RAM of compiled code (a write drops its blocks). The core's fault handler
#     (linux/common.cpp) takes only those faults and the fast memory's (the table's pieces not mapped
#     yet, the rewrites): on the PS4 with the FreeBSD kernel's values written out (SIGSEGV 11, SIGBUS
#     10, SA_SIGINFO 0x40, si_addr 24 bytes in, the registers 0x40 bytes into the context; struct
#     sigaction checked at compile time), no alternate stack (the app gives its threads none), every
#     other fault passed on to the handler before (the app's crash log) as the kernel would. Without
#     the fast memory, when it's installed a page is protected and written once: if that doesn't fault
#     and come back, nothing is protected (no texture invalidation; the recompiler then checks the code
#     of each block when it runs). Each kind of fault is counted (the log line below).
#   - The recompiler's dispatch (Resonance's changes, plain x86-64): a block that ends with time left in
#     the slice and the MMU off jumps straight to the next block through the jump table, and the main
#     loop reads the table inline instead of calling bm_GetCodeByVAddr (core/rec-x64/rec_x64.cpp).
#   - Hot VRAM pages (Resonance's too): a page that faults more than 6 times in a second stops being
#     protected for 10 s, the textures on it are checked by hash when used (a PS4 fault costs about a
#     millisecond; core/rend/TexCache.cpp).
#   - The recompilers' code memory (SH4 21 MiB, ARM7 8 MiB, DSP 32 KiB: since 00.69 twice upstream's caches, which
#     filled up in the owner's sessions and were then all compiled again) is three parts of the app's JIT
#     arena (00.66; port/ps4_n64.c's emusuni_jit_arena: one array in the app's data, within 2 GiB of the
#     code, made executable the ways the PS4 allows, which every recompiling core shares - one runs at a time -
#     and the app gives back after each core's end), taken again at every game's start, the code written
#     again; not arrays of the core's own (15 MiB of the image the PS4 charges from the app's start). If the
#     arena can't be had, the interpreters run (the SH4's, the ARM7's and the DSP's, all built in). The
#     big tables are on the heap while a game runs (the ARM7's 16 MiB table of blocks, the SH4 MMU's 5.5 MiB;
#     glslang's 6 MiB SPIR-V table when first used, which it isn't): no static array of 1 MiB or more (checked).
#   - The SH4 recompiler's FTRV and FIPR (the vector maths 3D games do for every vertex) in x86 in the block,
#     not a call of their C versions, the same double-precision sums in the same order (the same results to
#     the bit); its memory accesses without saving the float registers around each call (the handlers that
#     call C keep them themselves). Switches EMUSUNI_DC_VECTOR=0 / EMUSUNI_DC_MEMCALLS=0 (or the files
#     /data/EmuSuNi/dc-vector-off, dc-memcalls-off), as below.
#   - The sound chip's 64 channels: only the playing ones stepped each sample (the same sound to the bit).
#   - The SH4 waiting in a loop (00.69): a loop that only reads memory and comes round to the same registers with
#     nothing changed (Sega's library waiting for the next frame: 54-64% of the SH4's time in Crazy Taxi's and
#     Daytona USA's races) lets the time go by to the next event or interrupt without running (the pictures and
#     the sound the same to the bit on the PC tests); a library's counted wait (Marvel vs. Capcom 2, Virtua Tennis 2)
#     too, its count counted down by the rounds that went by. The SH4's memory accesses in the block itself (00.69), not a
#     call of the fast memory's 4-instruction function each (its call and return 5-15% of the emulation thread); an
#     access to the hardware's addresses is rewritten as the call of the slow handler when it faults, as before.
#     Switches EMUSUNI_DC_IDLE=0 / EMUSUNI_DC_INLINEMEM=0 (files /data/EmuSuNi/dc-idle-off, dc-inlinemem-off).
#   - The Atomiswave cartridge's decryption by tables (00.69; its bit permutations were loops, 9% of the emulation
#     thread while an Atomiswave boots).
#   - The console's pace (00.69): flycast_emusuni_pace(fps) gives the frames a second the app runs it at; with
#     threaded rendering the emulation thread sleeps at a frame more than 3 frames ahead of it (a game that draws
#     nothing - a loading screen, a board's BIOS - ran as fast as the CPU could, its sound too fast for the app).
#   - C++ exceptions (00.81): on the PS4 one the core throws never reaches its catch and the app ends without a crash
#     report, so every throw is logged first (oslib/emusuni_throw.cpp, linked over __cxa_throw with ld.lld -r --wrap):
#     its type, its message, the SH4's pc and the game. A display list with a parameter the TA doesn't know no longer
#     throws (upstream's catch only left out the rest of the list). The board's video timing is logged when it changes.
#   - PGO: scripts/dc-core.profdata.xz (below).
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
#   - A line in the log every 10 s (retro_run), per frame: retro_run's time, the renderer's (of it the TA
#     lists' conversion with the textures) and its waits for the emulation, the emulation thread's CPU time
#     (of it the sound chip: ARM7, channels, DSP) and its waits for the renderer, the disc's reads (count,
#     time, the longest), the pipelines and shaders made and their time, the GPU waits and full GPU syncs;
#     and in that window the faults by kind (writes to pages of compiled code, to textures' VRAM, the table's
#     pieces mapped, the hardware accesses rewritten, others passed on) and the mprotect calls. Since 00.69 two
#     more lines: the SH4's waits let go by (their share of the console's time) and the time the emulation was
#     held back to the app's pace; the renderer's parts (its draws' commands and their count, the presenting, the
#     queue's submits and the waits for the app's lock of the queue).
#   - Built as a static library, without LTO, with its own zlib, zstd (without its tracing hooks),
#     libchdr, libzip, glslang... (nothing of the system's), without UPnP, Lua, breakpad, Discord,
#     OpenMP or the GDB server, for 16 KiB pages. The PS4's CMake system is "Generic": asio without
#     kqueue, libzip without the Annex K functions the PS4's libc has but doesn't declare. As one
#     object: libchdr's LZMA SDK (24.05) only for what the rest doesn't have (the core's own 7-Zip
#     copy has the same functions, which upstream's link takes).
#   - The arcade boards (00.63, the app's NAOMI and Atomiswave tabs): flycast_emusuni_arcade_list gives the
#     app every game Flycast's list has (set, parent, full name, BIOS zip, board, GD-ROM image, flags); the
#     NAOMI printer's fonts are in the program again (a few NAOMI games print). flycast_emusuni_check_set
#     (00.65) says what would make a set's start fail, before the core starts and without an exception.
#   - The console's frames per picture, for the app's pacing (00.68): every message of the renderer's queue
#     carries the emulation's frame count of when it was sent, a picture presented keeps its own, and
#     flycast_emusuni_vblanks gives the app the last picture's (a 30 FPS game, Frame Skip: 2 to 4 frames a
#     picture; src/Emulator.cpp DcFrames).
# The object is checked at the end: only flycast_retro_* (and the app's four flycast_emusuni_* helpers) defined globally, no weak references left,
# the C++ operator new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and
# none defined in it, and every other symbol it needs found in what the app links (OpenOrbis' libc,
# libkernel, libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c;
# emusuni_dc_thread from port/).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-dc-core.sh <out dir> [clean]
#   DC_CORE_HOST=1 build-dc-core.sh <out dir>   the same object for this PC (Linux, clang 21), for the
#                                               PC tests: no PS4 checks; the test frontend provides
#                                               the JIT arena (emusuni_jit_arena,
#                                               emusuni_jit_arena_generation; its reset after
#                                               retro_deinit) and emusuni_dc_thread.
#   DC_CORE_PGO=off|generate                     without the profile; instrumented to make one (PC)
#                                               The fast memory runs on stand-ins for the PS4's calls
#                                               (EMUSUNI_DC_HOST: a memfd and mmap; direct memory
#                                               comes filled with 0xA5; EMUSUNI_DC_SHIM_FAIL=alias,
#                                               alloc, reserve, map=<n>, mapfrom=<n>, maprange=<a>-<b>,
#                                               nofault, noprotect or large makes one thing fail, to
#                                               test the fallback).
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
      'target_sources(${PROJECT_NAME} PRIVATE\n        emusuni_mem.cpp\n        emusuni_throw.cpp\n        emusuni_timing.cpp\n        directory.cpp\n')
patch('CMakeLists.txt',
      '\t\t\tcore/linux/posix_vmem.cpp\n',
      '\t\t\tcore/linux/posix_vmem.cpp\n\t\t\tcore/linux/emusuni_vmem.cpp\n')
# --- Embedded resources the libretro core doesn't need, 1.5 MB of the program image (resource::load answers
# "not found" for a file that isn't there): the F355 Challenge's printer template (only the picture its
# print-outs are printed on: without it they come out plain, hw/naomi/printer.cpp) and the GUI's translations
# (the app has its own menus; one file of them stays, see below). The NAOMI printer's fonts (444 KB) stay since
# 00.63, for the NAOMI games that print (a cabinet's printer: Touch de Uno! 1 and 2, Mirai Yosou Studio; F355
# Challenge's needs linked boards, which this build hasn't), and a missing font is read as a null pointer - the
# emulation would crash at the first print.
patch('resources/resources.cmake',
      '        resources/flash/smarinef.nvmem.zip\t# standard cabinet\n        resources/picture/f355_print_template.png)\n',
      '        resources/flash/smarinef.nvmem.zip\t# standard cabinet\n        )\n')
patch('resources/resources.cmake',
      'cmrc_add_resources(flycast-resources\n\tWHENCE resources\n\tresources/i18n/fr.po\n\tresources/i18n/hu.po\n'
      '\tresources/i18n/ja.po\n\tresources/i18n/pt_BR.po\n\tresources/i18n/sv.po\n\tresources/i18n/zh_CN.po\n'
      '\tresources/i18n/zh_HK.po\n\tresources/i18n/zh_TW.po)\n',
      '# EmuSuNi: the GUI\'s translations are left out but for one (64 KB): with none the i18n directory is not in\n'
      '# the embedded filesystem and the first translation looked up makes listDirectory throw (and catch) a C++\n'
      '# exception.\n'
      'cmrc_add_resources(flycast-resources\n\tWHENCE resources\n\tresources/i18n/zh_CN.po)\n')

# --- The arcade boards' games for the app (00.63: its NAOMI and Atomiswave lists, src/Naomi.cpp):
# flycast_emusuni_arcade_list calls `add` once for each game of Games[] (hw/naomi/naomi_roms.cpp, MAME's sets): its
# set's name (the zip's), its parent's, its full name, the zip of the BIOS it boots (BIOS[]'s file: naomi-jp-d boots
# from naomi.zip), the GD-ROM image a GD-ROM game reads (<set>/<name>.chd), its board (DC_PLATFORM_*, as
# naomi_cart_GetPlatform says), its cartridge's memory (bytes) and flags: 1 vertical (ROT270), 2 analog controls (a
# wheel, a stick, pedals), 4 a light gun (its reload or trigger key), 8 several boards linked. Returns the count.
patch('core/hw/naomi/naomi_roms.cpp',
      '\t\t"mda-c0056a",\n    },\n\t{\n\t\tnullptr\n\t}\n};\n',
      '\t\t"mda-c0056a",\n    },\n\t{\n\t\tnullptr\n\t}\n};\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <cstring>\n'
      '\n'
      '// EmuSuNi (scripts/build-dc-core.sh): the games above for the app\'s lists.\n'
      'extern "C" unsigned flycast_emusuni_arcade_list(void (*add)(void *user, const char *name, const char *parent,\n'
      '\t\tconst char *title, const char *bios_zip, const char *gdrom, int platform, unsigned size, unsigned flags), void *user)\n'
      '{\n'
      '\tunsigned count = 0;\n'
      '\tfor (const Game *game = Games; game->name != nullptr; game++, count++)\n'
      '\t{\n'
      '\t\tconst char *biosName = game->bios != nullptr ? game->bios : "naomi";\n'
      '\t\tconst char *biosZip = biosName;\n'
      '\t\tfor (const BIOS_t *bios = BIOS; bios->name != nullptr; bios++)\n'
      '\t\t\tif (!strcmp(bios->name, biosName))\n'
      '\t\t\t{\n'
      '\t\t\t\tbiosZip = bios->filename != nullptr ? bios->filename : bios->name;\n'
      '\t\t\t\tbreak;\n'
      '\t\t\t}\n'
      '\t\tconst int platform = game->cart_type == AW ? DC_PLATFORM_ATOMISWAVE\n'
      '\t\t\t\t: !strcmp(biosName, "naomi2") ? DC_PLATFORM_NAOMI2\n'
      '\t\t\t\t: !strcmp(biosName, "segasp") ? DC_PLATFORM_SYSTEMSP\n'
      '\t\t\t\t: DC_PLATFORM_NAOMI;\n'
      '\t\tunsigned flags = game->rotation_flag == ROT270 ? 1 : 0;\n'
      '\t\tif (game->inputs != nullptr)\n'
      '\t\t{\n'
      '\t\t\tif (game->inputs->axes[0].name != nullptr)\n'
      '\t\t\t\tflags |= 2;\n'
      '\t\t\tfor (const ButtonDescriptor& button : game->inputs->buttons)\n'
      '\t\t\t\tif (button.source == NAOMI_RELOAD_KEY || button.source == AWAVE_TRIGGER_KEY)\n'
      '\t\t\t\t\tflags |= 4;\n'
      '\t\t}\n'
      '\t\tif (game->multiboard > 0)\n'
      '\t\t\tflags |= 8;\n'
      '\t\tadd(user, game->name, game->parent_name, game->description, biosZip, game->gdrom_name, platform, game->size, flags);\n'
      '\t}\n'
      '\treturn count;\n'
      '}\n'
      '#endif\n')

# --- The arcade boards' start without C++ exceptions (00.64). On the PS4 an exception the core throws doesn't come
# back to its catch: the app ends at once, without a crash report - every Atomiswave game in 00.63, right after the
# board's memory was set up. A NAOMI's or an Atomiswave's normal start threw three (the PC test's [throw] lines,
# scratchpad wsl/naomi/front/throws.sh; a Dreamcast disc's throws none): naomi_cart_LoadBios asks OpenArchive for the
# game's zip by its bare name, which isn't a file (getFileInfo throws for a missing one), and resource::load opens the
# game's flash defaults, which aren't there (CMakeRC throws). Both ask first now. (A start that fails - no BIOS, a
# file not in the set - still throws: src/Emulator.cpp asks flycast_emusuni_check_set, below, before it starts
# the core.)
patch('core/archive/archive.cpp',
      '\ttry {\n\t\tfileInfo = hostfs::storage().getFileInfo(path);\n\t\tif (!fileInfo.isDirectory)\n'
      '\t\t\tfile = hostfs::storage().openFile(path, "rb");\n\t} catch (const hostfs::StorageException& e) {\n\t}\n',
      '\ttry {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (hostfs::storage().exists(path))  // EmuSuNi: no exception for a file that isn\'t there\n'
      '#endif\n'
      '\t\t{\n'
      '\t\tfileInfo = hostfs::storage().getFileInfo(path);\n\t\tif (!fileInfo.isDirectory)\n'
      '\t\t\tfile = hostfs::storage().openFile(path, "rb");\n'
      '\t\t}\n'
      '\t} catch (const hostfs::StorageException& e) {\n\t}\n')
patch('core/oslib/resources.cpp',
      '\t\telse\n\t\t{\n\t\t\tcmrc::file file = fs.open(path);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\telse if (fs.exists(path))  // EmuSuNi: no exception for a resource that isn\'t there\n'
      '#else\n'
      '\t\telse\n'
      '#endif\n'
      '\t\t{\n\t\t\tcmrc::file file = fs.open(path);\n')
# --- (00.81) Every C++ exception the core throws, named in the log before it's thrown. On the PS4 one never reaches its
# catch: the app ends at once, with no crash report (00.64), so a game that closes EmuSuNi left nothing to go on (a
# player's The King of Fighters XI on the Atomiswave, as its first fight started). The final link renames the core's
# calls of __cxa_throw to oslib/emusuni_throw.cpp's (ld.lld -r --wrap, below), which logs the exception's type, its
# message (FlycastException's and SH4ThrownException's, kept by their constructors), the SH4's pc and the game, then
# throws it as before: on the PC the same catches catch it.
addfile('core/oslib/emusuni_throw.cpp', r'''// EmuSuNi (scripts/build-dc-core.sh, 00.81): every C++ exception the core throws, in the log before it's thrown (on
// the PS4 its catch is never reached: the app ends).
#include "types.h"

#ifdef EMUSUNI_PORT
#include "hw/sh4/sh4_if.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <typeinfo>

namespace {
char g_what[192];                   // the message of the exception being made (its constructor), if it has one
std::atomic<int> g_thrown { 0 };
}  // namespace

void emusuniThrowWhat(const char *what)
{
	snprintf(g_what, sizeof(g_what), "%s", what != nullptr ? what : "");
}

void emusuniThrowSh4(u32 epc, u32 code)
{
	snprintf(g_what, sizeof(g_what), "the SH4's exception %03x at %08x", code, epc);
}

extern "C" [[noreturn]] void __real___cxa_throw(void *object, std::type_info *type, void (*destructor)(void *));

extern "C" [[noreturn]] void __wrap___cxa_throw(void *object, std::type_info *type, void (*destructor)(void *))
{
	const int n = ++g_thrown;
	// (The first 20, then one in 100: on the PC a caught one may come again.)
	if (n <= 20 || n % 100 == 0)
	{
		const u32 pc = p_sh4rcb != nullptr ? Sh4cntx.pc : 0;
#if defined(__ORBIS__)
		const char *after = "; on the PS4 its catch is never reached: the app ends here";
#else
		const char *after = "";
#endif
		ERROR_LOG(COMMON, "EmuSuNi: C++ exception %d thrown in the core: %s%s%s (SH4 pc %08x, game %s)%s", n,
				type != nullptr ? type->name() : "?", g_what[0] != '\0' ? ": " : "", g_what, pc,
				settings.content.gameId.c_str(), after);
	}
	g_what[0] = '\0';
	__real___cxa_throw(object, type, destructor);
}
#endif
''')
patch('core/types.h',
      'class FlycastException : public std::runtime_error\n{\npublic:\n'
      '\tFlycastException(const std::string& reason) : std::runtime_error(reason) {}\n'
      '\tFlycastException(const char *reason) : std::runtime_error(reason) {}\n};\n',
      '#ifdef EMUSUNI_PORT\n'
      'void emusuniThrowWhat(const char *what);  // oslib/emusuni_throw.cpp: the message, for the log of the throw\n'
      'void emusuniThrowSh4(u32 epc, u32 code);\n'
      '#endif\n'
      'class FlycastException : public std::runtime_error\n{\npublic:\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tFlycastException(const std::string& reason) : std::runtime_error(reason) { emusuniThrowWhat(reason.c_str()); }\n'
      '\tFlycastException(const char *reason) : std::runtime_error(reason) { emusuniThrowWhat(reason); }\n'
      '#else\n'
      '\tFlycastException(const std::string& reason) : std::runtime_error(reason) {}\n'
      '\tFlycastException(const char *reason) : std::runtime_error(reason) {}\n'
      '#endif\n'
      '};\n')
patch('core/hw/sh4/sh4_core.h',
      '\tSH4ThrownException(u32 epc, Sh4ExceptionCode expEvn) : epc(epc), expEvn(expEvn) { }\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tSH4ThrownException(u32 epc, Sh4ExceptionCode expEvn) : epc(epc), expEvn(expEvn) { emusuniThrowSh4(epc, expEvn); }\n'
      '#else\n'
      '\tSH4ThrownException(u32 epc, Sh4ExceptionCode expEvn) : epc(epc), expEvn(expEvn) { }\n'
      '#endif\n')
# A display list with a parameter the TA doesn't know (types 3 and 6): upstream throws TAParserException there, which
# ta_parse catches to leave out the rest of the list - on the PS4 the end of the app. The list's parsing stops there
# without the exception (the picture as upstream's: the rest of that list left out); NAOMI 2's path (ta_add_ta_data,
# which elan.cpp catches) as upstream. The first logged, the count in the 10 s line.
patch('core/hw/pvr/ta_vtx.cpp',
      '\t\t\t\tWARN_LOG(PVR, "Unhandled param type pcw %08x", data->pcw.full);\n\t\t\t\tthrow TAParserException();\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\tif (fetchTextures)\n'
      '\t\t\t\t{\n'
      '\t\t\t\t\t// EmuSuNi (00.81): the rest of the list left out as ta_parse\'s catch does, without the exception.\n'
      '\t\t\t\t\tif (emusuni::timing.taUnknown++ == 0)\n'
      '\t\t\t\t\t\tNOTICE_LOG(PVR, "EmuSuNi: a display list with a parameter the TA doesn\'t know (pcw %08x): the rest of the list "\n'
      '\t\t\t\t\t\t\t"left out, as upstream, without its C++ exception (the end of the app on the PS4)", data->pcw.full);\n'
      '\t\t\t\t\treturn data_end;\n'
      '\t\t\t\t}\n'
      '#endif\n'
      '\t\t\t\tWARN_LOG(PVR, "Unhandled param type pcw %08x", data->pcw.full);\n\t\t\t\tthrow TAParserException();\n')
patch('core/hw/pvr/ta_vtx.cpp',
      '#include "ta_ctx.h"\n',
      '#include "ta_ctx.h"\n#include "emusuni_timing.h"\n')
# The board's video timing (its frames a second come from it: the app runs the core at that rate), logged when it
# changes: the lines, the pixel clocks a line and the clock, interlaced or not (a rate of 100 or more isn't passed on
# to the app by upstream's retro_refresh_av_info: the line shows it). At most 30 a session.
patch('core/hw/pvr/spg.cpp',
      '#ifdef LIBRETRO\n\tretro_refresh_av_info((double)SH4_MAIN_CLOCK / ((double)Line_Cycles * (double)pvr_numscanlines));\n#endif\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t{\n'
      '\t\t// EmuSuNi (00.81): the video timing when it changes.\n'
      '\t\tstatic u32 lastLines = 0, lastLine = 0;\n'
      '\t\tstatic int logged = 0;\n'
      '\t\tif ((pvr_numscanlines != lastLines || Line_Cycles != lastLine) && logged < 30)\n'
      '\t\t{\n'
      '\t\t\tlastLines = pvr_numscanlines;\n'
      '\t\t\tlastLine = Line_Cycles;\n'
      '\t\t\tlogged++;\n'
      '\t\t\tNOTICE_LOG(PVR, "EmuSuNi: video timing: %u lines of %u pixel clocks at %s MHz%s: %.3f frames a second", pvr_numscanlines,\n'
      '\t\t\t\t\tSPG_LOAD.hcount + 1, FB_R_CTRL.vclk_div ? "27" : "13.5", SPG_CONTROL.interlace ? ", interlaced (a field a frame)" : "",\n'
      '\t\t\t\t\t(double)SH4_MAIN_CLOCK / ((double)Line_Cycles * (double)pvr_numscanlines));\n'
      '\t\t}\n'
      '\t}\n'
      '#endif\n'
      '#ifdef LIBRETRO\n\tretro_refresh_av_info((double)SH4_MAIN_CLOCK / ((double)Line_Cycles * (double)pvr_numscanlines));\n#endif\n')

# The cartridge's memory (a NAOMI's or Atomiswave's ROM, 16 to 384 MiB; a cartridge image's; a GD-ROM game's DIMM
# memory) off the app's heap, as the Dreamcast's RAM (oslib/emusuni_mem.cpp): direct memory on the PS4, the
# 4.5 GiB pool, rather than the system flexible memory the heap shares with Sony's modules.
patch('core/hw/naomi/naomi_cart.cpp',
      '#include "oslib/i18n.h"\n',
      '#include "oslib/i18n.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'void *emusuni_pages_alloc(size_t size);  // oslib/emusuni_mem.cpp: off the heap\n'
      'void emusuni_pages_free(void *p);\n'
      '#endif\n')
patch('core/hw/naomi/naomi_cart.cpp',
      'Cartridge::Cartridge(u32 size)\n{\n\tRomPtr = (u8 *)malloc(size);\n',
      'Cartridge::Cartridge(u32 size)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tRomPtr = (u8 *)emusuni_pages_alloc(size);\n'
      '#else\n'
      '\tRomPtr = (u8 *)malloc(size);\n'
      '#endif\n')
patch('core/hw/naomi/naomi_cart.cpp',
      'Cartridge::~Cartridge()\n{\n\tif (RomPtr != NULL)\n\t\tfree(RomPtr);\n}\n',
      'Cartridge::~Cartridge()\n{\n\tif (RomPtr != NULL)\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuni_pages_free(RomPtr);\n'
      '#else\n'
      '\t\tfree(RomPtr);\n'
      '#endif\n'
      '}\n')
patch('core/hw/naomi/naomi_cart.cpp',
      '\tu8 *romBase = (u8 *)malloc(romSize);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tu8 *romBase = (u8 *)emusuni_pages_alloc(romSize);\n'
      '#else\n'
      '\tu8 *romBase = (u8 *)malloc(romSize);\n'
      '#endif\n')
patch('core/hw/naomi/naomi_cart.cpp',
      '\tif (load_error)\n\t{\n\t\tfree(romBase);\n',
      '\tif (load_error)\n\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuni_pages_free(romBase);\n'
      '#else\n'
      '\t\tfree(romBase);\n'
      '#endif\n')
patch('core/hw/naomi/naomi_cart.h',
      '\tDecryptedCartridge(u8 *rom_ptr, u32 size) : NaomiCartridge(size) { free(RomPtr); RomPtr = rom_ptr; }\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the image\'s memory is emusuni_pages_alloc\'s (naomi_cart.cpp): a page of its own given back.\n'
      '\tDecryptedCartridge(u8 *rom_ptr, u32 size) : NaomiCartridge(0) { emusuniFree(RomPtr); RomPtr = rom_ptr; RomSize = size; }\n'
      '\tstatic void emusuniFree(u8 *p);\n'
      '#else\n'
      '\tDecryptedCartridge(u8 *rom_ptr, u32 size) : NaomiCartridge(size) { free(RomPtr); RomPtr = rom_ptr; }\n'
      '#endif\n')
patch('core/hw/naomi/naomi_cart.cpp',
      'Cartridge::~Cartridge()\n',
      '#ifdef EMUSUNI_PORT\n'
      'void DecryptedCartridge::emusuniFree(u8 *p)\n{\n\temusuni_pages_free(p);\n}\n\n'
      '#endif\n'
      'Cartridge::~Cartridge()\n')
patch('core/hw/naomi/gdcartridge.cpp',
      '\tif (dimm_data != NULL)\n\t{\n\t\tfree(dimm_data);\n\t\tdimm_data = NULL;\n\t}\n',
      '\tif (dimm_data != NULL)\n\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuni_pages_free(dimm_data);\n'
      '#else\n'
      '\t\tfree(dimm_data);\n'
      '#endif\n'
      '\t\tdimm_data = NULL;\n\t}\n')
patch('core/hw/naomi/gdcartridge.cpp',
      '\t\t\tdimm_data = (u8 *)malloc(dimm_data_size);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\tdimm_data = (u8 *)emusuni_pages_alloc(dimm_data_size);\n'
      '#else\n'
      '\t\t\tdimm_data = (u8 *)malloc(dimm_data_size);\n'
      '#endif\n')
patch('core/hw/naomi/gdcartridge.cpp',
      'GDCartridge::~GDCartridge()\n{\n\tfree(dimm_data);\n',
      'GDCartridge::~GDCartridge()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuni_pages_free(dimm_data);\n'
      '#else\n'
      '\tfree(dimm_data);\n'
      '#endif\n')
patch('core/hw/naomi/gdcartridge.cpp',
      '#include "hw/sh4/sh4_sched.h"\n',
      '#include "hw/sh4/sh4_sched.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'void *emusuni_pages_alloc(size_t size);  // oslib/emusuni_mem.cpp: off the heap\n'
      'void emusuni_pages_free(void *p);\n'
      '#endif\n')
# Where a board's start is (the log: a start that ends the app says how far it got): its zip open, its BIOS, its
# cartridge's memory, its files read.
patch('core/hw/naomi/naomi_cart.cpp',
      '\t// Load the BIOS\n\tnaomi_cart_LoadBios(fileName.c_str());\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tNOTICE_LOG(NAOMI, "EmuSuNi: start: %s opened, the BIOS next", fileName.c_str());\n'
      '#endif\n'
      '\t// Load the BIOS\n\tnaomi_cart_LoadBios(fileName.c_str());\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tNOTICE_LOG(NAOMI, "EmuSuNi: start: BIOS loaded, the cartridge next (%u MiB)", game->size >> 20);\n'
      '#endif\n')
patch('core/hw/naomi/naomi_cart.cpp',
      '\t\tstd::vector<u8> gdromDigest;\n\t\tCurrentCartridge->Init(progress, config::GGPOEnable ? &gdromDigest : nullptr);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\tNOTICE_LOG(NAOMI, "EmuSuNi: start: the cartridge\'s %d files read", romCount);\n'
      '#endif\n'
      '\t\tstd::vector<u8> gdromDigest;\n\t\tCurrentCartridge->Init(progress, config::GGPOEnable ? &gdromDigest : nullptr);\n')

# --- What would make a board's start fail, found before it starts (00.65): flycast_emusuni_check_set, which
# src/Emulator.cpp asks before it starts the core (Naomi::CheckSet). It looks for what loadMameRom would refuse the
# set for, in its order and where it and loadBios look, without reading or loading anything and without a C++
# exception (above: one ends the app on the PS4): the set not in Games[]; neither its zip nor its parent's there;
# none of its BIOS's files, any region's (loadBios takes another region's when the chosen one's isn't there), in its
# BIOS zip (found as findNaomiBios finds it: Flycast's system/dc, then beside the game; naomi_cart_LoadBios opens
# the set's own zips by their bare names, from the current folder); a file of the set in neither of its zips, by
# CRC, then by name (a default EEPROM is optional). The message is Flycast's own, in English (the app reads it:
# Naomi::LoadError); nothing is translated or logged (the core may not be set up yet). A 7z's files by its
# directory: opening one unpacks its whole block.
patch('core/archive/archive.h',
      '\tvirtual ArchiveFile *OpenFileByCrc(u32 crc) = 0;\n',
      '\tvirtual ArchiveFile *OpenFileByCrc(u32 crc) = 0;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi (scripts/build-dc-core.sh): whether OpenFile / OpenFileByCrc would find the file, without reading it.\n'
      '\tvirtual bool HasFile(const char *name) {\n'
      '\t\tArchiveFile *file = OpenFile(name);\n'
      '\t\tconst bool found = file != nullptr;\n'
      '\t\tdelete file;\n'
      '\t\treturn found;\n'
      '\t}\n'
      '\tvirtual bool HasFileByCrc(u32 crc) {\n'
      '\t\tArchiveFile *file = OpenFileByCrc(crc);\n'
      '\t\tconst bool found = file != nullptr;\n'
      '\t\tdelete file;\n'
      '\t\treturn found;\n'
      '\t}\n'
      '#endif\n')
patch('core/archive/7zArchive.h',
      '\tArchiveFile *OpenFileByCrc(u32 crc) override;\n',
      '\tArchiveFile *OpenFileByCrc(u32 crc) override;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tbool HasFile(const char *name) override;\n'
      '\tbool HasFileByCrc(u32 crc) override;\n'
      '#endif\n')
patch('core/archive/7zArchive.cpp',
      'SzArchive::~SzArchive()\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (scripts/build-dc-core.sh): OpenFile\'s and OpenFileByCrc\'s lookups, in the directory: nothing unpacked.\n'
      'bool SzArchive::HasFile(const char *name)\n'
      '{\n'
      '\tu16 fname[512];\n'
      '\tfor (UInt32 i = 0; i < szarchive.NumFiles; i++)\n'
      '\t{\n'
      '\t\tif (SzArEx_IsDir(&szarchive, i))\n'
      '\t\t\tcontinue;\n'
      '\t\tsize_t len = SzArEx_GetFileNameUtf16(&szarchive, i, fname);\n'
      '\t\tchar szname[512];\n'
      '\t\tsize_t j = 0;\n'
      '\t\tfor (; j < len && j < sizeof(szname) - 1; j++)\n'
      '\t\t\tszname[j] = fname[j];\n'
      '\t\tszname[j] = 0;\n'
      '\t\tif (!strcmp(name, szname))\n'
      '\t\t\treturn true;\n'
      '\t}\n'
      '\treturn false;\n'
      '}\n'
      '\n'
      'bool SzArchive::HasFileByCrc(u32 crc)\n'
      '{\n'
      '\tif (crc == 0)\n'
      '\t\treturn false;\n'
      '\tfor (UInt32 i = 0; i < szarchive.NumFiles; i++)\n'
      '\t\tif (!SzArEx_IsDir(&szarchive, i) && crc == szarchive.CRCs.Vals[i])\n'
      '\t\t\treturn true;\n'
      '\treturn false;\n'
      '}\n'
      '\n'
      '#endif\n'
      'SzArchive::~SzArchive()\n')
patch('core/hw/naomi/naomi_cart.cpp',
      'static void loadDecryptedRom(const std::string& path, const std::string& fileName, LoadProgress *progress)\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (scripts/build-dc-core.sh): whether one of the archives has the file, by its CRC or else by its name, as\n'
      '// loadMameRom and loadBios look for it.\n'
      'static bool emusuniHasFile(std::initializer_list<Archive *> archives, u32 crc, const char *name)\n'
      '{\n'
      '\tfor (Archive *archive : archives)\n'
      '\t\tif (archive != nullptr && archive->HasFileByCrc(crc))\n'
      '\t\t\treturn true;\n'
      '\tfor (Archive *archive : archives)\n'
      '\t\tif (archive != nullptr && archive->HasFile(name))\n'
      '\t\t\treturn true;\n'
      '\treturn false;\n'
      '}\n'
      '\n'
      '// What loadMameRom would refuse the set at path for (scripts/build-dc-core.sh says what and why); bios_dir is\n'
      '// Flycast\'s system/dc. 1 when it would start, and for anything but a .zip or .7z (a cartridge image); else 0 and\n'
      '// Flycast\'s message in error.\n'
      'extern "C" int flycast_emusuni_check_set(const char *path, const char *bios_dir, char *error, unsigned error_size)\n'
      '{\n'
      '\tconst auto refuse = [error, error_size](const std::string& message) {\n'
      '\t\tif (error != nullptr && error_size > 0)\n'
      '\t\t\tsnprintf(error, error_size, "%s", message.c_str());\n'
      '\t\treturn 0;\n'
      '\t};\n'
      '\tif (error != nullptr && error_size > 0)\n'
      '\t\terror[0] = 0;\n'
      '\tconst std::string gamePath(path);\n'
      '\t// (Emulator::loadGame\'s fileName: the path\'s last part; libretro.cpp\'s g_roms_dir: its folder, with the slash.)\n'
      '\tconst size_t slash = get_last_slash_pos(gamePath);\n'
      '\tconst std::string fileName = slash == std::string::npos ? gamePath : gamePath.substr(slash + 1);\n'
      '\tconst std::string romsDir = slash == std::string::npos ? std::string() : gamePath.substr(0, slash + 1);\n'
      '\tconst std::string extension = get_file_extension(fileName);\n'
      '\tif (extension != "zip" && extension != "7z")\n'
      '\t\treturn 1;\n'
      '\tconst Game *game = FindGame(fileName.c_str());\n'
      '\tif (game == nullptr)\n'
      '\t\treturn refuse("Unknown game");\n'
      '\t// The set\'s zip and its parent\'s (loadMameRom).\n'
      '\tstd::unique_ptr<Archive> archive(OpenArchive(gamePath));\n'
      '\tstd::unique_ptr<Archive> parentArchive;\n'
      '\tif (game->parent_name != nullptr)\n'
      '\t{\n'
      '\t\ttry {\n'
      '\t\t\tstd::string parentPath = hostfs::storage().getParentPath(gamePath);\n'
      '\t\t\tparentPath = hostfs::storage().getSubPath(parentPath, game->parent_name);\n'
      '\t\t\tparentArchive.reset(OpenArchive(parentPath));\n'
      '\t\t} catch (const FlycastException& e) {\n'
      '\t\t}\n'
      '\t}\n'
      '\tif (archive == nullptr && parentArchive == nullptr)\n'
      '\t{\n'
      '\t\tif (game->parent_name != nullptr)\n'
      '\t\t\treturn refuse(strprintf("Cannot open %s or %s", fileName.c_str(), game->parent_name));\n'
      '\t\treturn refuse(strprintf("Cannot open %s", fileName.c_str()));\n'
      '\t}\n'
      '\t// Its BIOS (naomi_cart_LoadBios, loadBios): one of its files, any region\'s (not the default EEPROM).\n'
      '\tconst char *biosName = game->bios != nullptr ? game->bios : "naomi";\n'
      '\tconst BIOS_t *bios = nullptr;\n'
      '\tfor (int biosid = 0; BIOS[biosid].name != nullptr; biosid++)\n'
      '\t\tif (!stricmp(BIOS[biosid].name, biosName))\n'
      '\t\t{\n'
      '\t\t\tbios = &BIOS[biosid];\n'
      '\t\t\tbreak;\n'
      '\t\t}\n'
      '\tbool biosFound = false;\n'
      '\tif (bios != nullptr)\n'
      '\t{\n'
      '\t\t// (naomi_cart_LoadBios opens the set\'s zips by the bare name: from the current folder.)\n'
      '\t\tstd::unique_ptr<Archive> childArchive(OpenArchive(fileName));\n'
      '\t\tstd::unique_ptr<Archive> childParentArchive;\n'
      '\t\tif (game->parent_name != nullptr)\n'
      '\t\t{\n'
      '\t\t\ttry {\n'
      '\t\t\t\tstd::string parentPath = hostfs::storage().getParentPath(fileName);\n'
      '\t\t\t\tparentPath = hostfs::storage().getSubPath(parentPath, game->parent_name);\n'
      '\t\t\t\tchildParentArchive.reset(OpenArchive(parentPath));\n'
      '\t\t\t} catch (const FlycastException& e) {\n'
      '\t\t\t}\n'
      '\t\t}\n'
      '\t\t// findNaomiBios: <name>.zip in Flycast\'s system/dc, else beside the game; then <name>.7z the same way.\n'
      '\t\tconst std::string archName(bios->filename != nullptr ? bios->filename : biosName);\n'
      '\t\tconst std::string systemDir(bios_dir != nullptr ? bios_dir : "");\n'
      '\t\tstd::string biosPath;\n'
      '\t\tfor (const char *ext : { ".zip", ".7z" })\n'
      '\t\t{\n'
      '\t\t\tif (!systemDir.empty() && hostfs::storage().exists(systemDir + "/" + archName + ext))\n'
      '\t\t\t\tbiosPath = systemDir + "/" + archName + ext;\n'
      '\t\t\telse if (hostfs::storage().exists(romsDir + archName + ext))\n'
      '\t\t\t\tbiosPath = romsDir + archName + ext;\n'
      '\t\t\tif (!biosPath.empty())\n'
      '\t\t\t\tbreak;\n'
      '\t\t}\n'
      '\t\tstd::unique_ptr<Archive> biosArchive(OpenArchive(biosPath));\n'
      '\t\tfor (int romid = 0; bios->blobs[romid].filename != nullptr && !biosFound; romid++)\n'
      '\t\t\tbiosFound = bios->blobs[romid].blob_type == Normal\n'
      '\t\t\t\t\t&& emusuniHasFile({ childArchive.get(), childParentArchive.get(), biosArchive.get() },\n'
      '\t\t\t\t\t\t\tbios->blobs[romid].crc, bios->blobs[romid].filename);\n'
      '\t}\n'
      '\tif (!biosFound)\n'
      '\t\treturn refuse(strprintf("Error: cannot load BIOS %s", game->bios != nullptr ? game->bios : "naomi.zip"));\n'
      '\t// Its files (loadMameRom): by CRC, then by name, in its zip and its parent\'s; a default EEPROM is optional.\n'
      '\tfor (int romid = 0; game->blobs[romid].filename != nullptr; romid++)\n'
      '\t{\n'
      '\t\tif (game->blobs[romid].blob_type == Copy || game->blobs[romid].blob_type == Eeprom)\n'
      '\t\t\tcontinue;\n'
      '\t\tif (!emusuniHasFile({ archive.get(), parentArchive.get() }, game->blobs[romid].crc, game->blobs[romid].filename))\n'
      '\t\t\treturn refuse(strprintf("Cannot find %s", game->blobs[romid].filename));\n'
      '\t}\n'
      '\treturn 1;\n'
      '}\n'
      '#endif\n'
      '\n'
      'static void loadDecryptedRom(const std::string& path, const std::string& fileName, LoadProgress *progress)\n')

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
	// (00.81: a big block - an arcade cartridge, The King of Fighters XI's 320 MiB - in the log, and where it went.)
	if (size >= 64 * 1024 * 1024)
	{
#if defined(__ORBIS__)
		const char *where = direct ? "direct memory" : "mapped memory (no direct memory for it: the system's flexible memory)";
#else
		const char *where = direct ? "direct memory" : "mapped memory";
#endif
		NOTICE_LOG(VMEM, "EmuSuNi: %d MiB of %s at %p", (int)(size >> 20), where, at);
	}
	else
	{
		DEBUG_LOG(VMEM, "EmuSuNi: %d KiB of %s at %p", (int)(size / 1024), direct ? "direct memory" : "mapped memory", at);
	}
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
addfile('core/emusuni_options.h', r'''// EmuSuNi (scripts/build-dc-core.sh): the switches of the speed changes (fast memory, the recompiler's
// dispatch, hot pages). A change is on unless EMUSUNI_DC_<NAME>=0 is in the environment; on the PS4,
// where nothing sets environment variables, also off while the file /data/EmuSuNi/dc-<name>-off exists
// (name in lower case): the way to compare, or to get a game running if a change breaks it, without
// building the app again. EMUSUNI_DC_<NAME>=1 switches it on whatever the file says.
#pragma once

#ifdef EMUSUNI_PORT
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace emusuni
{
// Whether the change is on. `how` (if given) says what switched it off (or on): "EMUSUNI_DC_FASTMEM=0",
// "the file /data/EmuSuNi/dc-fastmem-off", ""
inline bool dcOption(const char *name, char *how = nullptr, size_t howSize = 0)
{
	char key[48];
	snprintf(key, sizeof(key), "EMUSUNI_DC_%s", name);
	if (how != nullptr && howSize > 0)
		how[0] = '\0';
	if (const char *value = getenv(key))
	{
		const bool on = strcmp(value, "0") != 0;
		if (how != nullptr && howSize > 0)
			snprintf(how, howSize, "%s=%s", key, value);
		return on;
	}
#ifdef __ORBIS__
	char path[96];
	int length = snprintf(path, sizeof(path), "/data/EmuSuNi/dc-%s-off", name);
	for (char *c = path + 17; length > 17 && *c != '\0'; c++)
		if (*c >= 'A' && *c <= 'Z')
			*c = (char)(*c - 'A' + 'a');
	if (FILE *file = fopen(path, "rb"))
	{
		fclose(file);
		if (how != nullptr && howSize > 0)
			snprintf(how, howSize, "the file %s", path);
		return false;
	}
#endif
	return true;
}
}  // namespace emusuni
#endif
''')
addfile('core/emusuni_vmem.h', r'''// EmuSuNi (scripts/build-dc-core.sh): fast memory for Flycast on the PS4 (core/linux/emusuni_vmem.cpp).
#pragma once

#ifdef EMUSUNI_PORT
#include "types.h"
#include "oslib/virtmem.h"
#include <csignal>
#include <cstddef>

namespace emusuni
{
// What the kernel's signals look like. On the PS4 the FreeBSD kernel's values are written out (as
// port/ps4_crashlog.cpp has them: SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, si_addr 24 bytes into the
// siginfo), whatever a header says; the struct sigaction handed to the kernel is checked to be
// FreeBSD's when this compiles (the PSChrome libc overlay's headers come before OpenOrbis' musl ones).
#if defined(__ORBIS__)
constexpr int kSigBus = 10, kSigSegv = 11, kSaSiginfo = 0x40;
constexpr size_t kSiAddr = 24;
static_assert(offsetof(struct sigaction, sa_flags) == 8 && sizeof(sigset_t) == 16,
	"struct sigaction isn't FreeBSD's: the PSChrome libc overlay's headers must come first");
#else
constexpr int kSigBus = SIGBUS, kSigSegv = SIGSEGV, kSaSiginfo = SA_SIGINFO;
constexpr size_t kSiAddr = offsetof(siginfo_t, si_addr);
#endif

inline void *faultAddress(const siginfo_t *si)
{
	return *(void **)((const char *)si + kSiAddr);
}

// Fast memory: Flycast's virtmem API (oslib/virtmem.h) on the PS4's kernel calls (on the PC tests, on
// stand-ins for them). linux/posix_vmem.cpp calls these while `active`.
namespace vmem
{
extern bool active;        // in use: init() succeeded, destroy() not called since
extern bool protectionOk;  // locking pages was tested at start (a write to a locked page faults, unlocking lets it through)

// false: not available, nothing changed (Flycast's fallback, the memory handlers, takes over); why is logged.
bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize);
void destroy();
void resetMem(void *ptr, unsigned size_bytes);
void ondemandPage(void *address, unsigned size_bytes);
// false: a mapping failed (logged): addrspace::initMappings then falls back.
bool createMappings(const virtmem::Mapping *maps, unsigned count);
bool regionLock(void *start, size_t len);
bool regionUnlock(void *start, size_t len);
}  // namespace vmem
}  // namespace emusuni
#endif
''')
addfile('core/linux/emusuni_vmem.cpp', r'''/*
	EmuSuNi (scripts/build-dc-core.sh): fast memory (Flycast's virtual memory) on the PS4.

	Adapted from Resonance's shell/ps4/ps4_vmem.cpp (https://github.com/iHaiDeeZ/Resonance, GPL-2.0, a
	Flycast-based Dreamcast emulator for the PS4 by the author of DolphinPS4), which implements Flycast's
	virtmem API on the PS4's kernel calls. Here, for the Flycast EmuSuNi pins and the app's runtime: a switch
	(emusuni_options.h), a fallback to Flycast's memory handlers whenever anything is refused (checked by a
	self-test at start: aliasing, a fault with its registers, a locked page), counters, the log, and stand-ins
	for the PS4's calls so the PC tests run this very code (EMUSUNI_DC_HOST).

	The PS4 kernel can't reserve hundreds of MiB with mmap(PROT_NONE) and has no anonymous shared memory to
	map at fixed addresses, so, like DolphinPS4's MemArenaPS4:
	- the address space is a sceKernelReserveVirtualRange reservation,
	- the emulated memory is one block of direct (physical) memory, mapped into the reservation as often as
	  the Dreamcast mirrors it (direct memory aliasing, enabled for the process),
	- the dynarec's block table (fpcb) is mapped on demand, in 2 MiB pieces, as on Linux page by page.
	Everything maps in 16 KiB pages (2 MiB ones where addresses and sizes allow). The JIT's code memory is not
	here: it stays in the core's data (linux/posix_vmem.cpp, port/ps4_n64.c).

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Flycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "types.h"

#ifdef EMUSUNI_PORT

#include "emusuni_options.h"
#include "emusuni_timing.h"
#include "emusuni_vmem.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/sh4_if.h"
#include "oslib/host_context.h"
#include "oslib/virtmem.h"

#include <algorithm>
#include <cerrno>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/types.h>
#include <vector>

void context_from_segfault(host_context_t *hctx, void *segfault_ctx);  // linux/context.cpp

#if defined(__ORBIS__)

extern "C" {
// Declared by hand: several OpenOrbis prototypes are wrong.
size_t sceKernelGetDirectMemorySize();
int32_t sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t length, size_t alignment,
		int32_t memoryType, off_t *physicalOut);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t length);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int32_t protection, int32_t flags,
		off_t directMemoryStart, size_t alignment);
int32_t sceKernelReserveVirtualRange(void **address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void *address, size_t length);
int32_t sceKernelMprotect(const void *address, size_t length, int32_t protection);
int32_t sceKernelEnableDmemAliasing();
int32_t sceKernelEnableDmemAliasing2();
int sceKernelInternalMemoryGetAvailableSize(size_t *size);  // (as src/Log.cpp's LogMemory)
}

#elif defined(EMUSUNI_DC_HOST)

// The PC tests' stand-ins for the PS4's calls (EMUSUNI_DC_HOST: scripts/build-dc-core.sh with
// DC_CORE_HOST=1), so that this file's real code runs under the real SIGSEGV path. The "direct memory" is
// a sparse memfd, mapping it is mmap(MAP_SHARED | MAP_FIXED) over a PROT_NONE reservation. What the PS4
// does that the stand-ins copy, to catch mistakes the PC would let through: 16 KiB granularity; direct
// memory that comes as it was left (filled with 0xA5 when allocated: nothing may rely on zeros); a second
// mapping of the same direct memory refused (EBUSY) until aliasing is enabled; a reservation hint that
// isn't free failing instead of moving; a 2 MiB alignment asked for that the address, the direct memory
// offset and the length don't have refused; a Munmap that drops reservations too.
// EMUSUNI_DC_SHIM_FAIL=<list> (comma-separated) makes one thing fail, to test the fallback: alias (aliasing
// refused), alloc, reserve, map=<n> (the n-th mapping), mapfrom=<n> (every mapping from the n-th on), maprange=<a>-<b>
// (the a-th to the b-th), nofault (unmapped memory doesn't fault), noprotect (locking a page does nothing), large (2 MiB
// alignment refused), crash (the process is killed in the reservation: the marker files below).
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace
{
namespace shim
{
constexpr int32_t kBusy = (int32_t)0x80020010, kInval = (int32_t)0x80020016, kNoMem = (int32_t)0x8002000c;
constexpr size_t kPageSize = 0x4000;
constexpr size_t kPool = 1024u * 1024u * 1024u;

struct Mapped
{
	uintptr_t address;
	size_t length;
	off_t phys;
};
struct Block
{
	off_t start;
	size_t length;
};
std::mutex lock;
int fd = -1;
bool aliasing = false;
std::vector<Mapped> mapped;
std::vector<Block> blocks;
unsigned mapCalls = 0;

bool injected(const char *what, long *value = nullptr)
{
	const char *list = getenv("EMUSUNI_DC_SHIM_FAIL");
	if (list == nullptr)
		return false;
	const size_t length = strlen(what);
	for (const char *p = strstr(list, what); p != nullptr; p = strstr(p + 1, what))
	{
		if ((p == list || p[-1] == ',') && (p[length] == '\0' || p[length] == ',' || p[length] == '='))
		{
			if (value != nullptr)
				*value = p[length] == '=' ? atol(p + length + 1) : 0;
			return true;
		}
	}
	return false;
}

// Drops [address, address + length) from the records of what is mapped.
void forget(uintptr_t address, size_t length)
{
	std::vector<Mapped> kept;
	for (const Mapped &m : mapped)
	{
		if (m.address + m.length <= address || m.address >= address + length)
		{
			kept.push_back(m);
			continue;
		}
		if (m.address < address)
			kept.push_back({ m.address, address - m.address, m.phys });
		if (m.address + m.length > address + length)
		{
			const size_t cut = address + length - m.address;
			kept.push_back({ address + length, m.address + m.length - (address + length), m.phys + (off_t)cut });
		}
	}
	mapped.swap(kept);
}

int32_t enableAliasing()
{
	if (injected("alias"))
		return kInval;
	aliasing = true;
	return 0;
}

size_t directMemorySize()
{
	return kPool;
}

int32_t allocateDirectMemory(off_t searchStart, off_t searchEnd, size_t length, size_t alignment, int32_t, off_t *physical)
{
	std::lock_guard<std::mutex> guard(lock);
	if (injected("alloc"))
		return kNoMem;
	if (alignment < kPageSize)
		alignment = kPageSize;
	if (fd < 0)
	{
		fd = memfd_create("dc-direct-memory", 0);
		if (fd < 0 || ftruncate(fd, (off_t)kPool) != 0)
			return kNoMem;
	}
	std::sort(blocks.begin(), blocks.end(), [](const Block &a, const Block &b) { return a.start < b.start; });
	off_t candidate = (searchStart + (off_t)alignment - 1) & ~((off_t)alignment - 1);
	for (const Block &b : blocks)
	{
		if (candidate + (off_t)length <= b.start)
			break;
		if (b.start + (off_t)b.length > candidate)
			candidate = (b.start + (off_t)b.length + (off_t)alignment - 1) & ~((off_t)alignment - 1);
	}
	if (candidate + (off_t)length > std::min<off_t>(searchEnd, (off_t)kPool))
		return kNoMem;
	blocks.push_back({ candidate, length });
	// as it was left
	void *p = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, candidate);
	if (p != MAP_FAILED)
	{
		memset(p, 0xA5, length);
		munmap(p, length);
	}
	*physical = candidate;
	return 0;
}

int32_t releaseDirectMemory(off_t start, size_t length)
{
	std::lock_guard<std::mutex> guard(lock);
	for (size_t i = 0; i < blocks.size(); i++)
	{
		if (blocks[i].start == start && blocks[i].length == length)
		{
			for (const Mapped &m : mapped)
				if (m.phys < start + (off_t)length && m.phys + (off_t)m.length > start)
					fprintf(stderr, "[shim] direct memory %llx released while still mapped at %p\n", (unsigned long long)start,
							(void *)m.address);
			blocks.erase(blocks.begin() + i);
			fallocate(fd, 0x03 /* FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE */, start, (off_t)length);
			return 0;
		}
	}
	return kInval;
}

int32_t reserveVirtualRange(void **address, size_t length, int32_t flags, size_t alignment)
{
	std::lock_guard<std::mutex> guard(lock);
	if (injected("reserve"))
		return kNoMem;
	if (injected("crash"))
		raise(SIGKILL);  // (the process dies inside the kernel calls)
	if (alignment < kPageSize)
		alignment = kPageSize;
	if (((uintptr_t)*address | length) & (kPageSize - 1))
		return kInval;
	const int prot = injected("nofault") ? (PROT_READ | PROT_WRITE) : PROT_NONE;
	const int base = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	void *p;
	if (flags & 0x10)  // SCE_KERNEL_MAP_FIXED
	{
		p = mmap(*address, length, prot, base | MAP_FIXED, -1, 0);
		if (p == MAP_FAILED)
			return kNoMem;
		forget((uintptr_t)p, length);
	}
	else if (*address != nullptr)
	{
		// (a hint that isn't free fails)
		p = mmap(*address, length, prot, base | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == MAP_FAILED)
			return kNoMem;
		if (p != *address)
		{
			munmap(p, length);
			return kNoMem;
		}
	}
	else
	{
		p = mmap(nullptr, length + alignment, prot, base, -1, 0);
		if (p == MAP_FAILED)
			return kNoMem;
		const uintptr_t start = (uintptr_t)p, aligned = (start + alignment - 1) & ~(uintptr_t)(alignment - 1);
		if (aligned > start)
			munmap(p, aligned - start);
		if (aligned + length < start + length + alignment)
			munmap((void *)(aligned + length), start + length + alignment - (aligned + length));
		p = (void *)aligned;
	}
	if ((uintptr_t)p & (alignment - 1))
	{
		munmap(p, length);
		return kInval;
	}
	*address = p;
	return 0;
}

int32_t munmapRange(void *address, size_t length)
{
	std::lock_guard<std::mutex> guard(lock);
	if (((uintptr_t)address | length) & (kPageSize - 1))
		return kInval;
	if (munmap(address, length) != 0)
		return kInval;
	forget((uintptr_t)address, length);
	return 0;
}

int32_t mapDirectMemory(void **address, size_t length, int32_t protection, int32_t flags, off_t phys, size_t alignment)
{
	std::lock_guard<std::mutex> guard(lock);
	long failAt = 0;
	mapCalls++;
	if (injected("map", &failAt) && (failAt == 0 || mapCalls == (unsigned)failAt))
		return kInval;
	if (injected("mapfrom", &failAt) && mapCalls >= (unsigned)failAt)
		return kInval;
	if (injected("maprange", &failAt))
	{
		const char *dash = strchr(strstr(getenv("EMUSUNI_DC_SHIM_FAIL"), "maprange="), '-');
		if (mapCalls >= (unsigned)failAt && mapCalls <= (unsigned)(dash != nullptr ? atol(dash + 1) : failAt))
			return kInval;
	}
	if (!(flags & 0x10) || length == 0 || (((uintptr_t)*address | length | (size_t)phys) & (kPageSize - 1)))
		return kInval;
	if (alignment > kPageSize && (injected("large") || (((uintptr_t)*address | length | (size_t)phys) & (alignment - 1))))
		return kInval;
	bool inBlock = false;
	for (const Block &b : blocks)
		inBlock = inBlock || (phys >= b.start && phys + (off_t)length <= b.start + (off_t)b.length);
	if (!inBlock)
		return kInval;
	if (!aliasing || injected("alias"))
	{
		for (const Mapped &m : mapped)
			if (m.phys < phys + (off_t)length && m.phys + (off_t)m.length > phys)
				return kBusy;
	}
	int prot = 0;
	if (protection & 1)
		prot |= PROT_READ;
	if (protection & 2)
		prot |= PROT_WRITE;
	if (protection & 4)
		prot |= PROT_EXEC;
	void *p = mmap(*address, length, prot, MAP_SHARED | MAP_FIXED, fd, phys);
	if (p == MAP_FAILED)
		return kNoMem;
	forget((uintptr_t)p, length);
	mapped.push_back({ (uintptr_t)p, length, phys });
	*address = p;
	return 0;
}

int internalMemory(size_t *size)
{
	*size = 0;
	return 0;
}

int32_t protectRange(const void *address, size_t length, int32_t protection)
{
	if (((uintptr_t)address | length) & (kPageSize - 1))
		return kInval;
	if (injected("noprotect") && !(protection & 2))
		return 0;  // (lies: nothing is locked)
	int prot = 0;
	if (protection & 1)
		prot |= PROT_READ;
	if (protection & 2)
		prot |= PROT_WRITE;
	if (protection & 4)
		prot |= PROT_EXEC;
	return mprotect(const_cast<void *>(address), length, prot) == 0 ? 0 : kInval;
}
}  // namespace shim
}  // namespace

#define sceKernelEnableDmemAliasing shim::enableAliasing
#define sceKernelEnableDmemAliasing2 shim::enableAliasing
#define sceKernelGetDirectMemorySize shim::directMemorySize
#define sceKernelAllocateDirectMemory shim::allocateDirectMemory
#define sceKernelReleaseDirectMemory shim::releaseDirectMemory
#define sceKernelReserveVirtualRange shim::reserveVirtualRange
#define sceKernelMunmap shim::munmapRange
#define sceKernelMapDirectMemory shim::mapDirectMemory
#define sceKernelMprotect shim::protectRange
#define sceKernelInternalMemoryGetAvailableSize shim::internalMemory

#else
#error "emusuni_vmem.cpp: neither the PS4 (__ORBIS__) nor the PC tests (EMUSUNI_DC_HOST)"
#endif

namespace emusuni::vmem
{
bool active = false;
bool protectionOk = false;

namespace
{
constexpr size_t kPage = 0x4000;
constexpr size_t kLarge = 2_MB;  // large pages: the virtual and the direct memory address and the size 2 MiB aligned
constexpr int32_t kWbOnion = 0;       // SCE_KERNEL_WB_ONION: CPU write-back memory
constexpr int32_t kMapFixed = 0x10;   // SCE_KERNEL_MAP_FIXED
constexpr int32_t kProtRead = 0x1;    // SCE_KERNEL_PROT_CPU_READ
constexpr int32_t kProtRW = 0x3;      // SCE_KERNEL_PROT_CPU_RW
constexpr size_t kFpcbSize = sizeof(Sh4RCB::fpcb);
// The kernel library puts its own objects at 0x200000000 (the app's heap, port/ps4_runtime.cpp, keeps away from
// there the same way), so nothing of ours below 0x400000000.
constexpr uintptr_t kNotBelow = 0x400000000ull;

// The direct memory block (2 MiB aligned): the Sh4RCB (fpcb + SH4 context) at physSh4rcb, the emulated memory
// right after it. The Sh4RCB's virtual and direct memory addresses are equal modulo 2 MiB, which puts the
// emulated memory (sizeof(Sh4RCB) after it) on 2 MiB boundaries in both, and lets the fpcb be mapped in 2 MiB
// pieces that are aligned in both: the PS4 can then use large pages, and the JIT's block lookups and memory
// accesses miss the TLB far less.
off_t dmem = -1;
size_t dmemSize = 0;
off_t physSh4rcb = 0;
u8 *reservedBase = nullptr;
size_t reservedSize = 0;
u8 *sh4rcbBase = nullptr;  // inside the reservation, `lead` bytes below a 2 MiB boundary
u8 *ramBase = nullptr;     // sh4rcbBase + sizeof(Sh4RCB): the emulated 512 MiB (+ the sound RAM's 8 MiB at 0x20000000)
std::vector<bool> fpcbChunkMapped;  // per 2 MiB piece of the fpcb
bool largeOk = true;                // 2 MiB pages taken (else 16 KiB ones)
int32_t lastMapError = 0;           // (what the last failed mapping said: the caller's one line has it)
int snapshot[8];                    // the fault counters when this began

size_t alignUp(size_t v, size_t a)
{
	return (v + a - 1) & ~(a - 1);
}

// The kernel library's own memory left (page tables and map entries come out of it: a diagnostic, in the log).
size_t internalFree()
{
	size_t size = 0;
	sceKernelInternalMemoryGetAvailableSize(&size);
	return size;
}
size_t internalAtStart = 0;

// A start that dies inside the kernel calls (a crash, or a hang the player ends) leaves a file behind, and the fast memory is not
// tried again by itself: the log says which file to delete. "trying" exists from the start of init() until the first game's mappings
// are made (or init or they failed, or the memory is given back); "crashed" is what the next start makes of a "trying" it finds.
// The PS4's folder of the app; on the PC tests the folder EMUSUNI_DC_MARKER_DIR says (none: no files).
bool markerPath(const char *name, char *path, size_t size)
{
#if defined(__ORBIS__)
	snprintf(path, size, "/data/EmuSuNi/%s", name);
	return true;
#else
	const char *dir = getenv("EMUSUNI_DC_MARKER_DIR");
	if (dir == nullptr)
		return false;
	snprintf(path, size, "%s/%s", dir, name);
	return true;
#endif
}
bool markerExists(const char *name)
{
	char path[160];
	FILE *file = markerPath(name, path, sizeof(path)) ? fopen(path, "rb") : nullptr;
	if (file != nullptr)
		fclose(file);
	return file != nullptr;
}
void markerMake(const char *name)
{
	char path[160];
	if (FILE *file = markerPath(name, path, sizeof(path)) ? fopen(path, "wb") : nullptr)
		fclose(file);
}
void markerRemove(const char *name)
{
	char path[160];
	if (markerPath(name, path, sizeof(path)))
		remove(path);
}
bool trying = false;
void tryingDone()
{
	if (trying)
	{
		markerRemove("dc-fastmem-trying");
		trying = false;
	}
}

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...)
{
	char line[400];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	NOTICE_LOG(VMEM, "EmuSuNi: vmem: %s", line);
}

// Fast memory is not to be had: the one line that says why, Flycast's fallback takes over.
bool fastMemoryOff(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool fastMemoryOff(const char *fmt, ...)
{
	char line[400];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	WARN_LOG(VMEM, "EmuSuNi: vmem: fast memory off: %s (Flycast's memory handlers instead)", line);
	tryingDone();
	return false;
}

// Maps direct memory [offset, offset + size) at a fixed address inside the reservation. 2 MiB pages where the
// addresses and the size allow, else (or if the kernel doesn't take them) 16 KiB ones.
bool mapFixed(void *address, size_t size, off_t offset, bool writable)
{
	const int32_t protection = writable ? kProtRW : kProtRead;
	const size_t length = alignUp(size, kPage);
	int32_t largeError = 0;
	if (largeOk && (((uintptr_t)address | (uintptr_t)(dmem + offset) | length) & (kLarge - 1)) == 0)
	{
		void *addr = address;
		const int32_t r = sceKernelMapDirectMemory(&addr, length, protection, kMapFixed, dmem + offset, kLarge);
		if (r == 0 && addr == address)
			return true;
		if (r == 0)
			sceKernelMunmap(addr, length);
		largeError = r != 0 ? r : -1;
	}
	void *addr = address;
	const int32_t r = sceKernelMapDirectMemory(&addr, length, protection, kMapFixed, dmem + offset, kPage);
	if (r != 0 || addr != address)
	{
		lastMapError = r != 0 ? r : -1;
		if (r == 0)
			sceKernelMunmap(addr, length);
		return false;
	}
	if (largeError != 0)
	{
		// 16 KiB pages work, 2 MiB ones don't.
		largeOk = false;
		say("2 MiB pages refused (%08x): 16 KiB ones", (unsigned)largeError);
	}
	return true;
}

// Unmaps a range of the reservation and reserves it again, so nothing else lands there.
void unmapToReserved(void *address, size_t size)
{
	size = alignUp(size, kPage);
	sceKernelMunmap(address, size);
	void *addr = address;
	const int32_t r = sceKernelReserveVirtualRange(&addr, size, kMapFixed, kPage);
	if (r != 0 || addr != address)
		ERROR_LOG(VMEM, "EmuSuNi: vmem: re-reserving %p failed: %x", address, (unsigned)r);
}

// Locks and unlocks pages: the kernel's own call, else the libc's.
bool protect(void *address, size_t length, int32_t protection)
{
	if (sceKernelMprotect(address, length, protection) == 0)
		return true;
	return mprotect(address, length, protection == kProtRead ? PROT_READ : (PROT_READ | PROT_WRITE)) == 0;
}

// ---- The self-test: what the fast memory needs of the system, tried once it is mapped, before it is used.
// A handler of its own is in place meanwhile (neither the app's crash log's nor Flycast's is for this).
struct sigaction savedSegv, savedBus;
sigjmp_buf testJump;
volatile int testMode = 0;  // 1: an access to unmapped memory, 2: a write to a locked page
volatile int testSignal = 0;
volatile uintptr_t testAddress = 0, testPc = 0, testRsp = 0, testR9 = 0, testRdi = 0, testFrame = 0;
u8 *volatile testPage = nullptr;
volatile int testFaults = 0;
constexpr uint64_t kTestR9 = 0x1357924680ace135ull, kTestRdi = 0x2468ace013579bdfull;

void testHandler(int sig, siginfo_t *si, void *context)
{
	testSignal = sig;
	void *address = faultAddress(si);
	if (testMode == 2)
	{
		if ((u8 *)address >= testPage && (u8 *)address < testPage + kPage && ++testFaults == 1 && protect(testPage, kPage, kProtRW))
			return;  // the write goes through
		siglongjmp(testJump, 2);
	}
	if (testMode == 1)
	{
		host_context_t hc;
		context_from_segfault(&hc, context);
		testAddress = (uintptr_t)address;
		testPc = hc.pc;
#if HOST_CPU == CPU_X64
		testRsp = hc.rsp;
		testR9 = hc.r9;
		testRdi = hc.rdi;
#endif
		siglongjmp(testJump, 1);
	}
	// not a test's: the default action
	struct sigaction dfl {};
	sigaction(sig, &dfl, nullptr);
}

const char *signalName(int sig)
{
	return sig == kSigSegv ? "SIGSEGV" : sig == kSigBus ? "SIGBUS" : "?";
}

// A read of unmapped memory must fault, and the handler must see the address, the code's address, the stack
// and the two registers the recompiler's rewrite of a memory access uses (r9, rdi) as they were.
__attribute__((noinline)) bool testUnmappedRead(u8 *unmapped)
{
	testMode = 1;
	testFrame = (uintptr_t)__builtin_frame_address(0);
	if (sigsetjmp(testJump, 1) == 0)
	{
#if HOST_CPU == CPU_X64
		asm volatile(
			"movq %[r9v], %%r9\n\t"
			"movq %[rdiv], %%rdi\n\t"
			"movb (%[addr]), %%al\n\t"
			:
			: [r9v] "r"(kTestR9), [rdiv] "r"(kTestRdi), [addr] "r"(unmapped)
			: "rax", "r9", "rdi", "memory");
#else
		(void)*(volatile u8 *)unmapped;
#endif
		testMode = 0;
		return false;  // no fault
	}
	testMode = 0;
	return true;
}

// A write to a page locked with the protection call must fault, and unlocking it in the handler let it through.
__attribute__((noinline)) int testLockedWrite(u8 *page)
{
	testPage = page;
	testFaults = 0;
	if (!protect(page, kPage, kProtRead))
		return -1;  // the lock is refused
	testMode = 2;
	if (sigsetjmp(testJump, 1) == 0)
	{
		((volatile u8 *)page)[100] = 0x5a;
		testMode = 0;
		if (testFaults == 0)
			return 0;  // the write didn't fault
		return ((volatile u8 *)page)[100] == 0x5a && testFaults == 1 ? 1 : -2;
	}
	testMode = 0;
	protect(page, kPage, kProtRW);
	return -3;  // the handler couldn't let it through
}

// "": all well; else what is wrong.
const char *selfTest(char *detail, size_t detailSize)
{
	static char why[240];
	// Aliasing: the RAM's first two mirrors are the same memory.
	u8 *a = ramBase + 0x0C000000, *b = ramBase + 0x0D000000;
	const off_t ram = (off_t)(physSh4rcb + sizeof(Sh4RCB));
	if (!mapFixed(a, kLarge, ram, true))
	{
		snprintf(why, sizeof(why), "the kernel won't map the memory block (%x)", (unsigned)lastMapError);
		return why;
	}
	if (!mapFixed(b, kLarge, ram, true))
	{
		snprintf(why, sizeof(why), "the kernel won't map the same direct memory twice (%x): no aliasing", (unsigned)lastMapError);
		unmapToReserved(a, kLarge);
		return why;
	}
	a[0x123] = 0x3c;
	b[0x456] = 0xc3;
	const bool aliased = b[0x123] == 0x3c && a[0x456] == 0xc3;
	unmapToReserved(a, kLarge);
	unmapToReserved(b, kLarge);
	if (!aliased)
		return "two mappings of the same direct memory are not the same memory";

	struct sigaction act {};  // (an empty mask)
	act.sa_sigaction = testHandler;
	act.sa_flags = kSaSiginfo;
	sigaction(kSigSegv, &act, &savedSegv);
	sigaction(kSigBus, &act, &savedBus);
	const char *result = "";
	// Unmapped memory (the reservation's area 0): the fault, as the recompiler's rewrite needs it.
	u8 *unmapped = ramBase + 0x00001000;
	const bool faulted = testUnmappedRead(unmapped);
	const int unmappedSignal = testSignal;
	if (!faulted)
		result = "a read of unmapped memory didn't fault";
	else if ((u8 *)testAddress != unmapped)
	{
		snprintf(why, sizeof(why), "fault address %p, expected %p", (void *)testAddress, (void *)unmapped);
		result = why;
	}
	else if (testPc < (uintptr_t)&testUnmappedRead || testPc >= (uintptr_t)&testUnmappedRead + 1024)
	{
		snprintf(why, sizeof(why), "fault pc %p, not in the test's code at %p", (void *)testPc, (void *)&testUnmappedRead);
		result = why;
	}
#if HOST_CPU == CPU_X64
	else if (testRsp + 4096 < testFrame || testRsp > testFrame + 4096)
	{
		snprintf(why, sizeof(why), "fault rsp %p, the stack is at %p", (void *)testRsp, (void *)testFrame);
		result = why;
	}
	else if (testR9 != kTestR9 || testRdi != kTestRdi)
	{
		snprintf(why, sizeof(why), "fault registers r9 %llx rdi %llx, expected %llx %llx", (unsigned long long)testR9,
				(unsigned long long)testRdi, (unsigned long long)kTestR9, (unsigned long long)kTestRdi);
		result = why;
	}
#endif
	// A locked page (the SH4 context's padding, mapped for good): the fault, and unlocking it lets the write through.
	int lockedSignal = 0;
	if (result[0] == '\0')
	{
		const int locked = testLockedWrite(sh4rcbBase + kFpcbSize + kPage);
		lockedSignal = testSignal;
		if (locked != 1)
		{
			snprintf(why, sizeof(why), "%s", locked == -1 ? "locking a page is refused"
					: locked == 0 ? "a write to a locked page didn't fault"
					: locked == -2 ? "a write to a locked page didn't come through after unlocking"
					: "the handler couldn't unlock a page");
			result = why;
		}
	}
	sigaction(kSigSegv, &savedSegv, nullptr);
	sigaction(kSigBus, &savedBus, nullptr);
	if (result[0] == '\0')
		snprintf(detail, detailSize, "self-test ok: aliasing; unmapped access %s with address, pc, stack, r9, rdi; locked page %s",
				signalName(unmappedSignal), signalName(lockedSignal));
	return result;
}

// Frees everything (what init and destroy share).
void release()
{
	if (reservedBase != nullptr)
	{
		sceKernelMunmap(reservedBase, reservedSize);
		reservedBase = nullptr;
	}
	sh4rcbBase = nullptr;
	ramBase = nullptr;
	if (dmem >= 0)
	{
		sceKernelReleaseDirectMemory(dmem, dmemSize);
		dmem = -1;
	}
}

void countersNow(int *out)
{
	const Timing &t = timing;
	out[0] = t.faultsCode;
	out[1] = t.faultsVram;
	out[2] = t.faultsTable;
	out[3] = t.faultsRewrite;
	out[4] = t.faultsOther;
	out[5] = t.protects;
	out[6] = t.unprotects;
}
}  // namespace

bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)
{
	if (active)
		return true;
	char how[96];
	if (!dcOption("FASTMEM", how, sizeof(how)))
		return fastMemoryOff("switched off by %s", how);
	const bool forced = how[0] != '\0';  // EMUSUNI_DC_FASTMEM=1: whatever the files say
	if (!forced && markerExists("dc-fastmem-crashed"))
	{
		char path[160];
		markerPath("dc-fastmem-crashed", path, sizeof(path));
		return fastMemoryOff("an earlier start died while trying it: delete the file %s to try again", path);
	}
	if (!forced && markerExists("dc-fastmem-trying"))
	{
		char path[160];
		markerMake("dc-fastmem-crashed");
		markerRemove("dc-fastmem-trying");
		markerPath("dc-fastmem-crashed", path, sizeof(path));
		return fastMemoryOff("the start before died while trying it (noted in the file %s: delete it to try again)", path);
	}
	markerMake("dc-fastmem-trying");
	trying = true;
	internalAtStart = internalFree();
	say("trying fast memory");

	static bool aliasingTried = false;
	static int32_t aliasing1 = 0, aliasing2 = 0;
	if (!aliasingTried)
	{
		// Mapping the same direct memory twice (the Dreamcast's mirrors) fails with EBUSY until aliasing is
		// enabled for the process (the test below finds out whether it took).
		aliasingTried = true;
		aliasing1 = sceKernelEnableDmemAliasing();
		aliasing2 = aliasing1 == 0 ? 0 : sceKernelEnableDmemAliasing2();
	}

	// The Sh4RCB starts this far below a 2 MiB boundary, in both address spaces.
	const size_t lead = (kLarge - sizeof(Sh4RCB) % kLarge) % kLarge;
	physSh4rcb = (off_t)lead;
	dmemSize = alignUp(lead + sizeof(Sh4RCB) + ramSize, kLarge);
	off_t physical = 0;
	int32_t r = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), dmemSize, kLarge, kWbOnion, &physical);
	if (r != 0)
		return fastMemoryOff("no %zu MiB of direct memory (%x)", dmemSize >> 20, (unsigned)r);
	dmem = physical;

	reservedSize = 512_MB + sizeof(Sh4RCB) + ARAM_SIZE_MAX + 2 * kLarge;
	// A hint whose range isn't free fails (ENOMEM) instead of the kernel looking on: a few are tried. Well above
	// the app's heap (port/ps4_runtime.cpp: 0x600000000 to 0x1800000000) and the other cores' blocks.
	void *base = nullptr;
	r = -1;
	for (uintptr_t hint : { 0x2800000000ull, 0x3000000000ull, 0x2000000000ull, 0x3800000000ull, 0x0ull })
	{
		base = (void *)hint;
		r = sceKernelReserveVirtualRange(&base, reservedSize, 0, kLarge);
		if (r == 0 && (uintptr_t)base >= kNotBelow)
			break;
		if (r == 0)
		{
			sceKernelMunmap(base, reservedSize);
			r = -2;
		}
	}
	if (r != 0)
	{
		release();
		return fastMemoryOff("no %zu MiB of address space (%x)", reservedSize >> 20, (unsigned)r);
	}
	reservedBase = (u8 *)base;
	// The same offset from a 2 MiB boundary as physSh4rcb: the emulated memory after it is aligned.
	sh4rcbBase = (u8 *)alignUp((uintptr_t)reservedBase, kLarge) + lead;
	ramBase = sh4rcbBase + sizeof(Sh4RCB);
	if (((uintptr_t)ramBase & (kLarge - 1)) != 0 || ((physSh4rcb + sizeof(Sh4RCB)) & (kLarge - 1)) != 0
			|| ramBase + 512_MB + ARAM_SIZE_MAX > reservedBase + reservedSize)
	{
		release();
		return fastMemoryOff("the layout doesn't fit the reservation at %p", base);
	}
	fpcbChunkMapped.assign(kFpcbSize / kLarge + 2, false);
	largeOk = true;

	// The SH4 context after the fpcb (the fpcb itself is mapped on demand: ondemandPage). Direct memory comes as
	// it was left.
	if (!mapFixed(sh4rcbBase + kFpcbSize, sizeof(Sh4RCB) - kFpcbSize, physSh4rcb + (off_t)kFpcbSize, true))
	{
		release();
		return fastMemoryOff("the SH4 context can't be mapped (%x)", (unsigned)lastMapError);
	}
	memset(sh4rcbBase + kFpcbSize, 0, sizeof(Sh4RCB) - kFpcbSize);

	say("%zu MiB of direct memory at %llx, %zu MiB of address space at %p (sh4rcb %p, ram base %p), aliasing calls %x/%x: testing it",
			dmemSize >> 20, (unsigned long long)dmem, reservedSize >> 20, reservedBase, sh4rcbBase, ramBase, (unsigned)aliasing1,
			(unsigned)aliasing2);
	char detail[200] = "";
	if (const char *why = selfTest(detail, sizeof(detail)); why[0] != '\0')
	{
		release();
		return fastMemoryOff("%s (aliasing calls %x/%x)", why, (unsigned)aliasing1, (unsigned)aliasing2);
	}

	active = true;
	protectionOk = true;
	countersNow(snapshot);
	*sh4rcb_addr = sh4rcbBase;
	*vmem_base_addr = ramBase;
	say("fast memory on, %s pages; %s; kernel library memory %zu KiB free (was %zu)", largeOk ? "2 MiB" : "16 KiB", detail,
			internalFree() / 1024, internalAtStart / 1024);
	return true;
}

void destroy()
{
	tryingDone();
	if (!active)
		return;
	int now[8];
	countersNow(now);
	active = false;
	protectionOk = false;
	release();
	say("closed; faults while it was in use: code pages %d, VRAM %d, jump table %d, access rewrites %d, not ours %d; protection calls %d/%d; "
			"kernel library memory %zu KiB free (was %zu)",
			now[0] - snapshot[0], now[1] - snapshot[1], now[2] - snapshot[2], now[3] - snapshot[3], now[4] - snapshot[4],
			now[5] - snapshot[5], now[6] - snapshot[6], internalFree() / 1024, internalAtStart / 1024);
}

// The fpcb goes back to faulting on first access.
void resetMem(void *ptr, unsigned size_bytes)
{
	unmapToReserved(ptr, size_bytes);
	std::fill(fpcbChunkMapped.begin(), fpcbChunkMapped.end(), false);
}

// The fpcb page holding `address`: maps the whole 2 MiB piece around it (clipped to the fpcb) and fills it with
// "no block yet" (the caller, bm_lockedWrite, fills its own page again).
void ondemandPage(void *address, unsigned size_bytes)
{
	u8 *fpcbStart = sh4rcbBase;
	u8 *fpcbEnd = sh4rcbBase + kFpcbSize;
	const uintptr_t piece = (uintptr_t)address & ~(uintptr_t)(kLarge - 1);
	u8 *start = std::max(fpcbStart, (u8 *)piece);
	u8 *end = std::min(fpcbEnd, (u8 *)(piece + kLarge));
	// Pieces are numbered from the 2 MiB boundary at or below the fpcb's start.
	const size_t chunk = (piece - ((uintptr_t)fpcbStart & ~(uintptr_t)(kLarge - 1))) / kLarge;
	if (chunk < fpcbChunkMapped.size() && fpcbChunkMapped[chunk])
		return;  // already mapped (a fault on a page mapped meanwhile): nothing to do
	if (!mapFixed(start, end - start, physSh4rcb + (off_t)(start - sh4rcbBase), true))
	{
		ERROR_LOG(VMEM, "EmuSuNi: vmem: the jump table's piece at %p can't be mapped (%x)", start, (unsigned)lastMapError);
		die("EmuSuNi: vmem: the jump table can't be mapped");
	}
	bm_vmem_pagefill((void **)start, (u32)(end - start));
	if (chunk < fpcbChunkMapped.size())
		fpcbChunkMapped[chunk] = true;
}

bool createMappings(const virtmem::Mapping *maps, unsigned count)
{
	for (unsigned i = 0; i < count; i++)
	{
		// Unmapped areas stay reserved (they fault).
		if (maps[i].memsize == 0)
			continue;
		const u64 rangeSize = maps[i].end_address - maps[i].start_address;
		const unsigned mirrors = (unsigned)(rangeSize / maps[i].memsize);
		if (rangeSize % maps[i].memsize != 0 || mirrors < 1)
			return fastMemoryOff("a memory map that doesn't divide into mirrors");
		for (unsigned j = 0; j < mirrors; j++)
		{
			const u64 offset = maps[i].start_address + j * maps[i].memsize;
			u8 *address = &ramBase[offset];
			// A mapping from an earlier game (another memory size) may be in the way.
			unmapToReserved(address, maps[i].memsize);
			if (!mapFixed(address, maps[i].memsize, (off_t)(physSh4rcb + sizeof(Sh4RCB) + maps[i].memoffset), maps[i].allow_writes))
				return fastMemoryOff("a mirror of the memory (%zx bytes at %p) can't be mapped (%x)", (size_t)maps[i].memsize, (void *)address,
						(unsigned)lastMapError);
		}
	}
	tryingDone();  // the first game's memory is mapped: past the kernel calls that could take the start down
	return true;
}

bool regionLock(void *start, size_t len)
{
	const size_t inpage = (uintptr_t)start & (kPage - 1);
	if (!protect((u8 *)start - inpage, alignUp(len + inpage, kPage), kProtRead))
	{
		ERROR_LOG(VMEM, "EmuSuNi: vmem: locking %p (%zu KiB) failed", start, len / 1024);
		return false;  // (nothing protected there: the page stays writable)
	}
	return true;
}

bool regionUnlock(void *start, size_t len)
{
	const size_t inpage = (uintptr_t)start & (kPage - 1);
	if (!protect((u8 *)start - inpage, alignUp(len + inpage, kPage), kProtRW))
		die("EmuSuNi: vmem: unlocking a page failed");
	return true;
}
}  // namespace emusuni::vmem

#endif  // EMUSUNI_PORT
''')
patch('core/linux/posix_vmem.cpp',
      '#include "oslib/virtmem.h"\n\n#ifndef MAP_NOSYNC\n',
      '#include "oslib/virtmem.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "emusuni_timing.h"\n'
      '#include "emusuni_vmem.h"\n'
      '#endif\n\n#ifndef MAP_NOSYNC\n')
# The virtmem API on the PS4's kernel (linux/emusuni_vmem.cpp: fast memory, the PC tests on stand-ins for its
# calls) while that is in use (emusuni::vmem::active); else Flycast's fallback (init returns false: no virtual
# memory, the memory blocks of emusuni_mem.cpp, the memory handlers) or, on the PC tests, upstream's own.
patch('core/linux/posix_vmem.cpp',
      'bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)\n{\n',
      'bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: fast memory (linux/emusuni_vmem.cpp) unless it is switched off or the system refuses it (the\n'
      '\t// log says why): then Flycast\'s fallback, no virtual memory. (The PC tests may compare with upstream\'s own\n'
      '\t// virtual memory: EMUSUNI_DC_VMEM=1.)\n'
      '#if !defined(__ORBIS__)\n'
      '\tif (getenv("EMUSUNI_DC_VMEM") == nullptr)\n'
      '#endif\n'
      '\t\treturn emusuni::vmem::init(vmem_base_addr, sh4rcb_addr, ramSize);\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'void destroy()\n{\n',
      'void destroy()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (emusuni::vmem::active)\n'
      '\t{\n'
      '\t\temusuni::vmem::destroy();\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'void reset_mem(void *ptr, unsigned size_bytes) {\n',
      'void reset_mem(void *ptr, unsigned size_bytes) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (emusuni::vmem::active)\n'
      '\t{\n'
      '\t\temusuni::vmem::resetMem(ptr, size_bytes);\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'void ondemand_page(void *address, unsigned size_bytes) {\n',
      'void ondemand_page(void *address, unsigned size_bytes) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (emusuni::vmem::active)\n'
      '\t{\n'
      '\t\temusuni::vmem::ondemandPage(address, size_bytes);\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'void create_mappings(const Mapping *vmem_maps, unsigned nummaps) {\n',
      'void create_mappings(const Mapping *vmem_maps, unsigned nummaps) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (emusuni::vmem::active)\n'
      '\t{\n'
      '\t\temusuni::vmem::createMappings(vmem_maps, nummaps);\n'
      '\t\treturn;\n'
      '\t}\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      '// Prepares the code region for JIT operations, thus marking it as RWX\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: create_mappings that says whether it worked (addrspace.cpp falls back when it didn\'t).\n'
      'bool create_mappings_checked(const Mapping *vmem_maps, unsigned nummaps)\n'
      '{\n'
      '\tif (emusuni::vmem::active)\n'
      '\t\treturn emusuni::vmem::createMappings(vmem_maps, nummaps);\n'
      '\tcreate_mappings(vmem_maps, nummaps);\n'
      '\treturn true;\n'
      '}\n'
      '#endif\n\n'
      '// Prepares the code region for JIT operations, thus marking it as RWX\n')
# (00.69) The SH4's code cache 20 MiB (+1 for its temporary blocks), the ARM7's 8 MiB: twice upstream's, in the same
# 32 MiB arena (29 MiB of it now; the arena is the app's either way, no memory more). Upstream's 10 and 4 filled up in
# the owner's sessions (the log's "jit arena: given back by dc sh4 (10.0 MiB of 11.0 MiB written), dc arm7 (4.0 MiB of
# 4.0 MiB written)" after Dolphin Blue, Jet Set Radio, Daytona USA, Virtua Tennis 2): a full cache drops every block
# and compiles them all again as they run, a hitch.
patch('core/hw/sh4/dyna/driver.cpp',
      'constexpr u32 CODE_SIZE = 10_MB;\n',
      '#ifdef EMUSUNI_PORT\n'
      'constexpr u32 CODE_SIZE = 20_MB;  // EmuSuNi (00.69): twice upstream\'s (the JIT arena\'s part, linux/posix_vmem.cpp)\n'
      '#else\n'
      'constexpr u32 CODE_SIZE = 10_MB;\n'
      '#endif\n')
patch('core/hw/arm7/arm7_rec.h',
      'const u32 ICacheSize = 4_MB;\n',
      '#ifdef EMUSUNI_PORT\n'
      'const u32 ICacheSize = 8_MB;  // EmuSuNi (00.69): twice upstream\'s (the JIT arena\'s part, linux/posix_vmem.cpp)\n'
      '#else\n'
      'const u32 ICacheSize = 4_MB;\n'
      '#endif\n')
patch('core/oslib/virtmem.h',
      '// To create the mappings in the address space.\nvoid create_mappings(const Mapping *vmem_maps, unsigned nummaps);\n',
      '// To create the mappings in the address space.\nvoid create_mappings(const Mapping *vmem_maps, unsigned nummaps);\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the same, false if a mapping failed (addrspace::initMappings then falls back).\n'
      'bool create_mappings_checked(const Mapping *vmem_maps, unsigned nummaps);\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'namespace virtmem\n{\n\nbool region_lock(void *start, size_t len)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern bool emusuniWriteProtectOk, emusuniWriteProtectTested;  // oslib/emusuni_mem.cpp\n'
      '// port/ps4_n64.c (the PC tests: their stand-in): the app\'s one JIT arena, which every recompiling core shares (one\n'
      '// runs at a time), a part for each cache, given back after each core\'s end (Emulator::Run).\n'
      'extern "C" void *emusuni_jit_arena(size_t size, const char *name);\n'
      'extern "C" unsigned emusuni_jit_arena_generation(void);\n'
      '#include <mutex>\n'
      '\n'
      'namespace emusuni\n'
      '{\n'
      '// The recompilers\' code caches in the app\'s JIT arena (00.66; they were 15 MiB of arrays in the program image,\n'
      '// which the PS4 charges to the games\' memory from the app\'s start): the SH4\'s (11 MiB), the ARM7\'s (4 MiB) and\n'
      '// the AICA DSP\'s (32 KiB), told apart by their sizes, taken all three in this order whenever the arena was given\n'
      '// back since (another core has written there): at the same places in every game. NULL: no JIT (the interpreters).\n'
      'void *jitPart(size_t size)\n'
      '{\n'
      '\tstatic std::mutex lock;\n'
      '\tstatic void *parts[3];\n'
      '\tstatic unsigned generation = ~0u;\n'
      '\tstatic const size_t sizes[3] = { 21u << 20, 8u << 20, 32u << 10 };  // (00.69: 21 and 8; 11 and 4 before)\n'
      '\tstatic const char *const names[3] = { "dc sh4", "dc arm7", "dc dsp" };\n'
      '\tstd::lock_guard<std::mutex> guard(lock);\n'
      '\tconst unsigned now = emusuni_jit_arena_generation();\n'
      '\tif (now != generation)\n'
      '\t{\n'
      '\t\tgeneration = now;\n'
      '\t\tfor (int i = 0; i < 3; i++)\n'
      '\t\t\tparts[i] = emusuni_jit_arena(sizes[i], names[i]);\n'
      '\t}\n'
      '\tfor (int i = 0; i < 3; i++)\n'
      '\t\tif (size == sizes[i])\n'
      '\t\t\treturn parts[i];\n'
      '\treturn emusuni_jit_arena(size, "dc other");\n'
      '}\n'
      '}  // namespace emusuni\n'
      '#endif\n\n'
      'namespace virtmem\n{\n\nbool region_lock(void *start, size_t len)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (!emusuniWriteProtectOk)\n'
      '\t\treturn false;  // EmuSuNi: no page protection here (linux/common.cpp\'s test)\n'
      '\temusuni::timing.protects++;\n'
      '\tif (emusuni::vmem::active)\n'
      '\t\treturn emusuni::vmem::regionLock(start, len);\n'
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
      '\temusuni::timing.unprotects++;\n'
      '\tif (emusuni::vmem::active)\n'
      '\t\treturn emusuni::vmem::regionUnlock(start, len);\n'
      '#endif\n')
patch('core/linux/posix_vmem.cpp',
      'bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)\n{\n',
      'bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the cache\'s part of the app\'s JIT arena (jitPart above; read/write/execute the ways the PS4\n'
      '\t// allows, port/ps4_n64.c), else no JIT (the interpreters run). The DSP\'s is asked for again at every reset of\n'
      '\t// the AICA, the SH4\'s and the ARM7\'s at every game\'s start (Sh4Recompiler::emusuniContextMoved,\n'
      '\t// arm7_rec.cpp\'s flush): the same part while the arena isn\'t given back.\n'
      '\t(void)code_area;\n'
      '\t*code_area_rwx = emusuni::jitPart(size);\n'
      '\tif (*code_area_rwx != nullptr)\n'
      '\t{\n'
      '\t\tNOTICE_LOG(VMEM, "EmuSuNi: %d KiB of JIT memory at %p (the app\'s JIT arena)", (int)(size / 1024), *code_area_rwx);\n'
      '\t\treturn true;\n'
      '\t}\n'
      '\tERROR_LOG(VMEM, "EmuSuNi: no JIT memory (%d KiB) in the app\'s JIT arena", (int)(size / 1024));\n'
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
      '// EmuSuNi: no array of the core\'s own: prepare_jit_block gives a part of the app\'s JIT arena (in its data, within\n'
      '// 2 GiB of the code: the x86-64 code reaches the core\'s code and data with 32-bit offsets).\n'
      '#define DECLARE_CODE_CACHE(Name, Size) static u8 *Name;\n'
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
# The same with the fast memory: it is unmapped and reserved again by the next game's retro_init, so nothing may
# touch it between games (maple_ReconnectDevices, setPlatform's unprotectVram of the game before...).
patch('core/hw/mem/addrspace.cpp',
      '\t\tvirtmem::destroy();\n\t\tram_base = nullptr;\n',
      '\t\tvirtmem::destroy();\n\t\tram_base = nullptr;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: no memory between games, as the fallback leaves it (termMappings).\n'
      '\t\tp_sh4rcb = nullptr;\n'
      '\t\tmem_b.free();\n'
      '\t\tvram.free();\n'
      '\t\taica::aica_ram.free();\n'
      '\t\telan::RAM = nullptr;\n'
      '\t\tsettings.platform.vram_size = 0;\n'
      '#endif\n')
# Fast memory whose mirrors can't be mapped (reserve() got the address space and the memory block at start): the
# fallback, as if it hadn't been had.
patch('core/hw/mem/addrspace.cpp',
      '\t\tvirtmem::create_mappings(&mem_mappings[0], std::size(mem_mappings));\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (!virtmem::create_mappings_checked(&mem_mappings[0], std::size(mem_mappings)))\n'
      '\t\t{\n'
      '\t\t\t// EmuSuNi: the log says why; Flycast\'s fallback, the memory handlers, from this game on.\n'
      '\t\t\tvirtmem::destroy();\n'
      '\t\t\tram_base = nullptr;\n'
      '\t\t\tp_sh4rcb = nullptr;\n'
      '\t\t\temusuniProbeAfterFallback();\n'
      '\t\t\tinitMappings();\n'
      '\t\t\treturn;\n'
      '\t\t}\n'
      '#else\n'
      '\t\tvirtmem::create_mappings(&mem_mappings[0], std::size(mem_mappings));\n'
      '#endif\n')
patch('core/hw/mem/addrspace.cpp',
      '\nnamespace addrspace\n{\n\n#define HANDLER_MAX 0x1F\n',
      '\n#ifdef EMUSUNI_PORT\n'
      'void *emusuni_pages_alloc(size_t size);  // oslib/emusuni_mem.cpp\n'
      'void emusuni_pages_free(void *p);\n'
      'void emusuniProbeAfterFallback();  // linux/common.cpp\n'
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
# (00.69) The PC tests only (EMUSUNI_DC_HOST, DCTEST_PERFMAP=1 in the environment): each block compiled is written to
# /tmp/perf-<pid>.map with its SH4 address and code, so perf's profiles name the SH4 code the time went to.
patch('core/hw/sh4/dyna/blockmanager.cpp',
      '#include "oslib/virtmem.h"\n',
      '#include "oslib/virtmem.h"\n'
      '#if defined(EMUSUNI_PORT) && defined(EMUSUNI_DC_HOST)\n'
      '#include <unistd.h>\n'
      '#endif\n')
patch('core/hw/sh4/dyna/blockmanager.cpp',
      '\tblkmap[(void*)block->code] = block;\n',
      '\tblkmap[(void*)block->code] = block;\n'
      '#if defined(EMUSUNI_PORT) && defined(EMUSUNI_DC_HOST)\n'
      '\tstatic const bool emusuniPerfMap = getenv("DCTEST_PERFMAP") != nullptr;\n'
      '\tif (emusuniPerfMap)\n'
      '\t{\n'
      '\t\tstatic FILE *map = nullptr;\n'
      '\t\tif (map == nullptr)\n'
      '\t\t{\n'
      '\t\t\tchar name[64];\n'
      '\t\t\tsnprintf(name, sizeof(name), "/tmp/perf-%d.map", (int)getpid());\n'
      '\t\t\tmap = fopen(name, "w");\n'
      '\t\t}\n'
      '\t\tif (map != nullptr)\n'
      '\t\t{\n'
      '\t\t\tstd::string text;\n'
      '\t\t\t// (the code read from the RAM only: area 3)\n'
      '\t\t\tfor (u32 i = 0; i < block->guest_opcodes && i < 24 && (block->vaddr & 0x1C000000) == 0x0C000000; i++)\n'
      '\t\t\t{\n'
      '\t\t\t\tconst u32 pc = block->vaddr + i * 2;\n'
      '\t\t\t\tconst u16 op = *(const u16 *)&mem_b[pc & RAM_MASK];\n'
      '\t\t\t\tchar buf[1024];\n'
      '\t\t\t\tOpDesc[op]->Disassemble(buf, pc, op);\n'
      '\t\t\t\tif (char *regs = strstr(buf, " ; "))\n'
      '\t\t\t\t\t*regs = 0;  // (not the registers\' values)\n'
      '\t\t\t\tfor (char *c = buf; *c != 0; c++)\n'
      '\t\t\t\t\tif (*c == \'\\n\')\n'
      '\t\t\t\t\t\t*c = \' \';\n'
      '\t\t\t\ttext += buf;\n'
      '\t\t\t\ttext += "; ";\n'
      '\t\t\t}\n'
      '\t\t\tfprintf(map, "%lx %x sh4_%08X_c%u_n%u %s\\n", (unsigned long)(uintptr_t)block->code, block->host_code_size,\n'
      '\t\t\t\t\tblock->vaddr, block->guest_cycles, block->guest_opcodes, text.c_str());\n'
      '\t\t\tfflush(map);\n'
      '\t\t}\n'
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
      'namespace emusuni { void *jitPart(size_t size); }  // linux/posix_vmem.cpp: the app\'s JIT arena\n'
      '#endif\n')
# (The main loop written again by the next reset: the code cache it was in may have moved, or been written by another
# core since, the JIT arena's.)
patch('core/rec-x64/rec_x64.cpp',
      '\tvoid init(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer) override\n\t{\n'
      '\t\tthis->sh4ctx = &sh4ctx;\n\t\tthis->codeBuffer = &codeBuffer;\n',
      '\tvoid init(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer) override\n\t{\n'
      '\t\tthis->sh4ctx = &sh4ctx;\n\t\tthis->codeBuffer = &codeBuffer;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t::mainloop = nullptr;  // EmuSuNi: written again by the reset that follows (the JIT arena)\n'
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
      '\t{\n'
      '\t\t// The code cache\'s part of the app\'s JIT arena again (00.66; linux/posix_vmem.cpp): another core may\n'
      '\t\t// have written there since the last game. Without it the SH4 interpreter runs this game.\n'
      '\t\tu8 *part = (u8 *)emusuni::jitPart(FULL_SIZE);\n'
      '\t\tif (part == nullptr)\n'
      '\t\t{\n'
      '\t\t\tERROR_LOG(DYNAREC, "EmuSuNi: no executable memory for the SH4 recompiler: the SH4 interpreter runs");\n'
      '\t\t\tCodeCache = TempCodeCache = nullptr;\n'
      '\t\t\temusuniSh4JitOk = false;\n'
      '\t\t\treturn;\n'
      '\t\t}\n'
      '\t\tCodeCache = part;\n'
      '\t\tTempCodeCache = CodeCache + CODE_SIZE;\n'
      '\t\tsh4Dynarec->init(*getContext(), codeBuffer);\n'
      '\t}\n'
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
      '\t// EmuSuNi (00.66): the code cache\'s part of the app\'s JIT arena again (another core may have written there\n'
      '\t// since the last game: every game\'s start resets the AICA, which comes here), and the table of the compiled\n'
      '\t// blocks on the heap (16 MiB; given back at retro_deinit: emusuniFreeTables).\n'
      '\tif (ICache != nullptr)\n'
      '\t\tICache = (u8 *)emusuni::jitPart(ICacheSize);\n'
      '\tif (ICache != nullptr && EntryPoints == nullptr)\n'
      '\t\tEntryPoints = (void (**)())malloc(sizeof(void (*)()) * (ARAM_SIZE_MAX / 4));\n'
      '\tif (ICache == nullptr || EntryPoints == nullptr)\n'
      '\t{\n'
      '\t\tif (ICache != nullptr)\n'
      '\t\t\tERROR_LOG(AICA_ARM, "EmuSuNi: no memory for the ARM7 recompiler\'s table: the ARM7 interpreter runs");\n'
      '\t\tICache = nullptr;\n'
      '\t\treturn;  // EmuSuNi: no code memory, the interpreter runs\n'
      '\t}\n'
      '#endif\n'
      '\ticPtr = ICache;\n')
patch('core/hw/arm7/arm7_rec.cpp',
      '#include "oslib/virtmem.h"\n#include "arm_mem.h"\n',
      '#include "oslib/virtmem.h"\n#include "arm_mem.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <cstdlib>\n'
      'namespace emusuni { void *jitPart(size_t size); }  // linux/posix_vmem.cpp: the app\'s JIT arena\n'
      '#endif\n')
patch('core/hw/arm7/arm7_rec.cpp',
      'void (*EntryPoints[ARAM_SIZE_MAX / 4])();\n',
      '#ifdef EMUSUNI_PORT\n'
      'void (**EntryPoints)();  // EmuSuNi: ARAM_SIZE_MAX / 4 of them, on the heap (flush)\n'
      'void emusuniFreeTables();\n'
      '#else\n'
      'void (*EntryPoints[ARAM_SIZE_MAX / 4])();\n'
      '#endif\n')
patch('core/hw/arm7/arm7_rec.cpp',
      '\tfor (u32 i = 0; i < std::size(EntryPoints); i++)\n\t\tEntryPoints[i] = arm_compilecode;\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tfor (u32 i = 0; i < ARAM_SIZE_MAX / 4; i++)\n'
      '#else\n'
      '\tfor (u32 i = 0; i < std::size(EntryPoints); i++)\n'
      '#endif\n'
      '\t\tEntryPoints[i] = arm_compilecode;\n}\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (libretro.cpp\'s retro_deinit): the table back to the heap between games (the next flush makes it again).\n'
      'void emusuniFreeTables()\n'
      '{\n'
      '\tfree(EntryPoints);\n'
      '\tEntryPoints = nullptr;\n'
      '}\n'
      '#endif\n')
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

# --- The sound chip's 64 channels (00.66): each of the 44,100 samples a second called ChannelEx::Step for every
# channel, playing or not (a silent one only gave back zeros: ~5% of the emulation thread on the PC in AICA_Sample,
# the call and its stack traffic times 64); now only the playing ones, in the same order, from a mask of them kept
# where a channel starts and stops (enable, disable) and where a state sets it (deserialize). A silent channel
# added exactly nothing (its outputs 0, its DSP input += 0), so the sound is the same to the bit. A channel that
# starts while the channels after it are stepped is still stepped in that sample, as before (the mask is read
# again after each one).
patch('core/hw/aica/sgc_if.cpp',
      '\tstatic ChannelEx Chans[64];\n',
      '\tstatic ChannelEx Chans[64];\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tstatic u64 emusuniOn;\t// EmuSuNi: bit n set while Chans[n].enabled (StepAll)\n'
      '#endif\n')
patch('core/hw/aica/sgc_if.cpp',
      '\tvoid disable()\n\t{\n\t\tenabled = false;\n',
      '\tvoid disable()\n\t{\n\t\tenabled = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuniOn &= ~(1ull << ChannelNumber);\n'
      '#endif\n')
patch('core/hw/aica/sgc_if.cpp',
      '\tvoid enable()\n\t{\n\t\tenabled = true;\n',
      '\tvoid enable()\n\t{\n\t\tenabled = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuniOn |= 1ull << ChannelNumber;\n'
      '#endif\n')
patch('core/hw/aica/sgc_if.cpp',
      '\tstatic void StepAll(SampleType& mixl, SampleType& mixr)\n\t{\n'
      '\t\tfor (ChannelEx& channel : Chans)\n\t\t\tchannel.Step(mixl, mixr);\n\t}\n',
      '\tstatic void StepAll(SampleType& mixl, SampleType& mixr)\n\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi: the playing channels only, lowest first, as the loop over all of them stepped them.\n'
      '\t\tfor (u64 on = emusuniOn; on != 0; )\n'
      '\t\t{\n'
      '\t\t\tconst int n = __builtin_ctzll(on);\n'
      '\t\t\tChans[n].Step(mixl, mixr);\n'
      '\t\t\ton = n == 63 ? 0 : emusuniOn & (~0ull << (n + 1));\n'
      '\t\t}\n'
      '#else\n'
      '\t\tfor (ChannelEx& channel : Chans)\n\t\t\tchannel.Step(mixl, mixr);\n'
      '#endif\n'
      '\t}\n')
patch('core/hw/aica/sgc_if.cpp',
      '\t\tdeser >> channel.enabled;\n\t\tchannel.quiet = false;\n',
      '\t\tdeser >> channel.enabled;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (channel.enabled)\n'
      '\t\t\tChannelEx::emusuniOn |= 1ull << channel.ChannelNumber;\n'
      '\t\telse\n'
      '\t\t\tChannelEx::emusuniOn &= ~(1ull << channel.ChannelNumber);\n'
      '#endif\n'
      '\t\tchannel.quiet = false;\n')
patch('core/hw/aica/sgc_if.cpp',
      'ChannelEx ChannelEx::Chans[64];\n',
      'ChannelEx ChannelEx::Chans[64];\n'
      '#ifdef EMUSUNI_PORT\n'
      'u64 ChannelEx::emusuniOn;\n'
      '#endif\n')

# --- The core's big tables off the program image (00.66; the PS4 charges the app's whole image, .bss included, to the
# games' memory from its start: with the code caches above, 42.5 MiB less for every game): on the heap while a
# Dreamcast game runs, given back at retro_deinit (the code caches are in the app's JIT arena, above):
#   - the ARM7 recompiler's table of compiled blocks (16 MiB): arm7_rec.cpp's flush, above;
#   - the SH4 MMU's address table (4 MiB, the MMU games: Windows CE) and its TLB cache (1.5 MiB): made when first
#     needed (the address table at the MMU's start and reset, its kernel half filled as MMU_init did);
#   - glslang's SPIR-V instruction table (6 MiB, kept in the image by its own static constructor): made by the first
#     spv::Parameterize(), which only glslang's disassembler and SPIR-V remapper call (Flycast calls neither); never
#     given back (as upstream's), as scripts/build-psp-core.sh does for PPSSPP's copy.
patch('core/hw/sh4/modules/mmu.h',
      'extern u32 mmuAddressLUT[0x100000];\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: on the heap (mmu.cpp): emusuniMmuLUT() makes it when it isn\'t there.\n'
      'extern u32 *mmuAddressLUT;\n'
      'u32 *emusuniMmuLUT();\n'
      'void emusuniFreeMmuTables();\n'
      '#else\n'
      'extern u32 mmuAddressLUT[0x100000];\n'
      '#endif\n')
patch('core/hw/sh4/modules/mmu.h',
      '\tif (full)\n\t\tmemset(mmuAddressLUT, 0, sizeof(mmuAddressLUT) / 2);\t// flush user memory\n'
      '\telse\n\t{\n\t\tconstexpr u32 slotPages = (32 * 1024 * 1024) >> 12;\n'
      '\t\tmemset(mmuAddressLUT, 0, slotPages * sizeof(u32));\t\t// flush slot 0\n\t}\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tif (full)\n\t\tmemset(emusuniMmuLUT(), 0, 0x100000 * sizeof(u32) / 2);\t// flush user memory\n'
      '\telse\n\t{\n\t\tconstexpr u32 slotPages = (32 * 1024 * 1024) >> 12;\n'
      '\t\tmemset(emusuniMmuLUT(), 0, slotPages * sizeof(u32));\t\t// flush slot 0\n\t}\n'
      '#else\n'
      '\tif (full)\n\t\tmemset(mmuAddressLUT, 0, sizeof(mmuAddressLUT) / 2);\t// flush user memory\n'
      '\telse\n\t{\n\t\tconstexpr u32 slotPages = (32 * 1024 * 1024) >> 12;\n'
      '\t\tmemset(mmuAddressLUT, 0, slotPages * sizeof(u32));\t\t// flush slot 0\n\t}\n'
      '#endif\n')
patch('core/hw/sh4/modules/mmu.cpp',
      '#ifdef FAST_MMU\nu32 mmuAddressLUT[0x100000];\n#endif\n',
      '#ifdef FAST_MMU\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <cstdlib>\n'
      'u32 *mmuAddressLUT;\n'
      'void emusuniFreeFullTable();  // fastmmu.cpp\n'
      '\n'
      '// EmuSuNi: the address table, made (its kernel half filled, as MMU_init did) when it isn\'t there.\n'
      'u32 *emusuniMmuLUT()\n'
      '{\n'
      '\tif (mmuAddressLUT == nullptr)\n'
      '\t{\n'
      '\t\tmmuAddressLUT = (u32 *)calloc(0x100000, sizeof(u32));\n'
      '\t\tverify(mmuAddressLUT != nullptr);\n'
      '\t\tfor (u32 vpn = 0x100000 / 2; vpn < 0x100000; vpn++)\n'
      '\t\t\tmmuAddressLUT[vpn] = vpn << 12;\n'
      '\t}\n'
      '\treturn mmuAddressLUT;\n'
      '}\n'
      '\n'
      '// EmuSuNi (libretro.cpp\'s retro_deinit): the MMU\'s tables back to the heap between games.\n'
      'void emusuniFreeMmuTables()\n'
      '{\n'
      '\tfree(mmuAddressLUT);\n'
      '\tmmuAddressLUT = nullptr;\n'
      '\temusuniFreeFullTable();\n'
      '}\n'
      '#else\n'
      'u32 mmuAddressLUT[0x100000];\n'
      '#endif\n'
      '#endif\n')
patch('core/hw/sh4/modules/mmu.cpp',
      '\t// pre-fill kernel memory\n'
      '\tfor (u32 vpn = std::size(mmuAddressLUT) / 2; vpn < std::size(mmuAddressLUT); vpn++)\n'
      '\t\tmmuAddressLUT[vpn] = vpn << 12;\n',
      '\t// pre-fill kernel memory\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniMmuLUT();  // EmuSuNi: made with it\n'
      '#else\n'
      '\tfor (u32 vpn = std::size(mmuAddressLUT) / 2; vpn < std::size(mmuAddressLUT); vpn++)\n'
      '\t\tmmuAddressLUT[vpn] = vpn << 12;\n'
      '#endif\n')
patch('core/hw/sh4/modules/mmu.cpp',
      'void MMU_reset()\n{\n',
      'void MMU_reset()\n{\n'
      '#if defined(EMUSUNI_PORT) && defined(FAST_MMU)\n'
      '\temusuniMmuLUT();  // EmuSuNi: there from every game\'s start (retro_deinit gave it back)\n'
      '#endif\n')
patch('core/hw/sh4/modules/fastmmu.cpp',
      'static TLB_LinkedEntry full_table[65536];\n',
      '#ifdef EMUSUNI_PORT\n'
      '#include <cstdlib>\n'
      '// EmuSuNi: on the heap when first needed (the MMU games), given back at retro_deinit (emusuniFreeFullTable).\n'
      'static TLB_LinkedEntry *full_table;\n'
      'constexpr u32 emusuniFullTableSize = 65536;\n'
      '#else\n'
      'static TLB_LinkedEntry full_table[65536];\n'
      '#endif\n')
patch('core/hw/sh4/modules/fastmmu.cpp',
      '\tif (full_table_size >= std::size(full_table))\n\t\treturn;\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tif (full_table == nullptr)\n'
      '\t\tfull_table = (TLB_LinkedEntry *)malloc(sizeof(TLB_LinkedEntry) * emusuniFullTableSize);\n'
      '\tif (full_table == nullptr || full_table_size >= emusuniFullTableSize)\n'
      '\t\treturn;\n'
      '#else\n'
      '\tif (full_table_size >= std::size(full_table))\n\t\treturn;\n'
      '#endif\n')
patch('core/hw/sh4/modules/fastmmu.cpp',
      'static void flush_cache()\n{\n\tfull_table_size = 0;\n\tmemset(entry_buckets, 0, sizeof(entry_buckets));\n}\n',
      'static void flush_cache()\n{\n\tfull_table_size = 0;\n\tmemset(entry_buckets, 0, sizeof(entry_buckets));\n}\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (mmu.cpp\'s emusuniFreeMmuTables): the TLB cache back to the heap, emptied first.\n'
      'void emusuniFreeFullTable()\n'
      '{\n'
      '\tflush_cache();\n'
      '\tfree(full_table);\n'
      '\tfull_table = nullptr;\n'
      '}\n'
      '#endif\n')
patch('SPIRV/doc.cpp',
      'InstructionParameters InstructionDesc[OpCodeMask + 1];\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (scripts/build-dc-core.sh): on the heap from the first Parameterize() (the disassembler and the SPIR-V\n'
      '// remapper call it before they read it; Flycast calls neither), not 6 MiB of the app\'s image.\n'
      'InstructionParameters *InstructionDesc = nullptr;\n'
      '#else\n'
      'InstructionParameters InstructionDesc[OpCodeMask + 1];\n'
      '#endif\n',
      repo=root + '/core/deps/glslang')
patch('SPIRV/doc.cpp',
      '    static std::once_flag initialized;\n    std::call_once(initialized, [](){\n',
      '    static std::once_flag initialized;\n    std::call_once(initialized, [](){\n'
      '#ifdef EMUSUNI_PORT\n'
      '        InstructionDesc = new InstructionParameters[OpCodeMask + 1];  // EmuSuNi: on the heap (above)\n'
      '#endif\n',
      repo=root + '/core/deps/glslang')
patch('SPIRV/doc.h',
      'extern InstructionParameters InstructionDesc[];\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern InstructionParameters *InstructionDesc;  // EmuSuNi: on the heap (doc.cpp)\n'
      '#else\n'
      'extern InstructionParameters InstructionDesc[];\n'
      '#endif\n',
      repo=root + '/core/deps/glslang')
patch('shell/libretro/libretro.cpp',
      '\taddrspace::release();\n#else\n\temu.term();\n#endif\n',
      '\taddrspace::release();\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: the tables made on the heap for the game back too (hw/arm7/arm7_rec.cpp, hw/sh4/modules/mmu.cpp), and\n'
      '\t// the recompilers\' unwind records unregistered while the JIT arena still holds them (the app gives it away next).\n'
      '\taica::arm::recompiler::emusuniFreeTables();\n'
      '\temusuniFreeMmuTables();\n'
      '\temusuniSh4UnwindClear();\n'
      '\taica::arm::emusuniArmUnwindClear();\n'
      '#endif\n'
      '#else\n\temu.term();\n#endif\n')
patch('shell/libretro/libretro.cpp',
      'static void refresh_devices(bool first_startup);\n',
      'static void refresh_devices(bool first_startup);\n'
      '#ifdef EMUSUNI_PORT\n'
      'namespace aica::arm::recompiler { void emusuniFreeTables(); }  // hw/arm7/arm7_rec.cpp\n'
      'void emusuniFreeMmuTables();  // hw/sh4/modules/mmu.cpp\n'
      'void emusuniSh4UnwindClear();  // rec-x64/rec_x64.cpp\n'
      'namespace aica::arm { void emusuniArmUnwindClear(); }  // hw/arm7/arm7_rec_x64.cpp\n'
      '#endif\n')
# The recompilers' main loops register their unwind information (the DWARF records Flycast writes into the code cache
# after each main loop) with the unwinder, which reads them again when they are unregistered (the next reset): taken
# back at retro_deinit while the code cache still holds them, as the app gives the JIT arena to the next core after
# the deinit and fills it with HLT (PC test: the second game of a process crashed in __deregister_frame without it).
patch('core/rec-x64/rec_x64.cpp',
      'static UnwindInfo unwinder;\n',
      'static UnwindInfo unwinder;\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (libretro.cpp\'s retro_deinit): the main loop\'s unwind records unregistered while they are still there.\n'
      'void emusuniSh4UnwindClear()\n'
      '{\n'
      '\tunwinder.clear();\n'
      '}\n'
      '#endif\n')
patch('core/hw/arm7/arm7_rec_x64.cpp',
      'static UnwindInfo unwinder;\n',
      'static UnwindInfo unwinder;\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (libretro.cpp\'s retro_deinit): the main loop\'s unwind records unregistered while they are still there.\n'
      'void emusuniArmUnwindClear()\n'
      '{\n'
      '\tunwinder.clear();\n'
      '}\n'
      '#endif\n')

# --- The fault handler: the core's own faults only, everything else goes on to the handler before (the
# app's crash log), as the kernel would call it. The core's: a write to a page the core write-protected
# (the textures' VRAM, the RAM of compiled code) and, with the fast memory (linux/emusuni_vmem.cpp), the SH4
# jump table's pieces not mapped yet and the recompiled accesses to the hardware's addresses (not mapped),
# rewritten as calls of the memory handlers. On the PS4 with the FreeBSD kernel's values written out
# (emusuni_vmem.h, as port/ps4_crashlog.cpp has them: SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, si_addr 24
# bytes into the siginfo, the registers 0x40 bytes into the context: linux/context.cpp), whatever a header
# says; the struct sigaction handed to the kernel checked to be FreeBSD's when this compiles. Without the
# fast memory, checked once when it's installed: a page protected and written to must fault and come back
# written (else nothing is write-protected: the blocks check their code instead). Every kind of fault is
# counted (emusuni_timing.h; the log line every 10 s).
patch('core/linux/common.cpp',
      'void fault_handler(int sn, siginfo_t * si, void *segfault_ctx)\n{\n',
      r'''#ifdef EMUSUNI_PORT
// EmuSuNi (scripts/build-dc-core.sh): see the top of the script.
#include <atomic>
#include <csetjmp>
#include <cstddef>
#include <sys/mman.h>
#include "emusuni_timing.h"
#include "emusuni_vmem.h"

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
	void *address = emusuni::faultAddress(si);
	if (emusuniProbeFault(address))
		return;
	emusuni::Timing& count = emusuni::timing;
	// Ram watcher for net rollback
	if (memwatch::writeAccess(address))
		return;
	// code protection in RAM
	if (bm_RamWriteAccess(address))
	{
		count.faultsCode++;
		return;
	}
	// texture protection in VRAM
	if (VramLockedWrite((u8 *)address))
	{
		count.faultsVram++;
		return;
	}
	if (addrspace::virtmemEnabled())
	{
		// The fast memory's (linux/emusuni_vmem.cpp; on the PC tests also upstream's own virtual memory,
		// EMUSUNI_DC_VMEM=1): the jump table's pieces not mapped yet, and the recompiled accesses to the
		// hardware's addresses (not mapped), rewritten as calls of the memory handlers. Without that memory
		// nothing but the two above is the core's: the context is not even read.
		if (addrspace::bm_lockedWrite((u8 *)address))
		{
			count.faultsTable++;
			return;
		}
		host_context_t ctx;
		context_from_segfault(&ctx, segfault_ctx);
		if (sh4Dynarec->rewrite(ctx, address))
		{
			context_to_segfault(&ctx, segfault_ctx);
			count.faultsRewrite++;
			return;
		}
		ERROR_LOG(COMMON, "SIGSEGV @ %p invalid access to %p", (void *)ctx.pc, address);
	}
	count.faultsOther++;
	// Not the core's: the handler before (the app's crash log), as the kernel would have called it.
	struct sigaction *old = sn == emusuni::kSigSegv ? &emusuniOldSegv : &emusuniOldBus;
	if (old->sa_flags & emusuni::kSaSiginfo)
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

// Does a write to a write-protected page fault, reach the handler and come back written? (Without the fast
// memory, which tests its own page locking: linux/emusuni_vmem.cpp.) Tried once.
static void emusuniProbeWriteProtection()
{
	static int result = -1;  // -1: not tried yet
	if (result < 0)
	{
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
		result = ok ? 1 : 0;
		if (ok)
			NOTICE_LOG(VMEM, "EmuSuNi: write-protected pages fault and come back: texture and code protection on");
		else
			ERROR_LOG(VMEM, "EmuSuNi: write-protected pages don't work (%s): no texture invalidation, the recompiler checks its blocks", why);
	}
	emusuniWriteProtectOk = result > 0;
	emusuniWriteProtectTested = true;
}

// The fast memory given up at a game's start (addrspace.cpp): the memory blocks' pages are locked with the libc's
// mprotect from here on, which the test above may not have tried yet in this process.
void emusuniProbeAfterFallback()
{
	emusuniProbeWriteProtection();
}

// No alternate signal stack: the app gives its threads none (SA_ONSTACK would do nothing). SIGBUS as well as
// SIGSEGV: on the PS4 a write to a read-only direct memory page may raise either.
void os_InstallFaultHandler()
{
	if (emusuniInstalled)
		return;
	struct sigaction act {};  // (an empty mask)
	act.sa_sigaction = fault_handler;
	act.sa_flags = emusuni::kSaSiginfo;
	sigaction(emusuni::kSigSegv, &act, &emusuniOldSegv);
	sigaction(emusuni::kSigBus, &act, &emusuniOldBus);
	emusuniInstalled = true;
	if (emusuni::vmem::active)
	{
		// The fast memory tested locking its own pages when it started.
		emusuniWriteProtectOk = emusuni::vmem::protectionOk;
		emusuniWriteProtectTested = true;
	}
	else
		emusuniProbeWriteProtection();
}

void os_UninstallFaultHandler()
{
	if (!emusuniInstalled)
		return;
	// The handlers before (the app's crash log) back.
	sigaction(emusuni::kSigSegv, &emusuniOldSegv, nullptr);
	sigaction(emusuni::kSigBus, &emusuniOldBus, nullptr);
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

# The registers the recompiler's rewrite of a memory access reads and writes (the fault's pc, rsp, r9 and rdi)
# in the signal context, on the PS4: the kernel's (FreeBSD's) register block starts 0x40 bytes into it
# (measured on hardware, as port/ps4_crashlog.cpp has it), in mcontext order: rdi the 2nd 8-byte slot, r9 the
# 7th, rip the 21st, rsp the 24th. The libc overlay's ucontext_t places them elsewhere.
patch('core/linux/context.cpp',
      '\t#if defined(__FreeBSD__) || defined(__DragonFly__)\n\t\tbicopy<ToSegfault>(hostctx->pc, MCTX(.mc_rip));\n',
      '\t#if defined(EMUSUNI_PORT) && defined(__ORBIS__)\n'
      '\t\tu64 *mc = (u64 *)((u8 *)segfault_ctx + 0x40);\n'
      '\t\tbicopy<ToSegfault>(hostctx->pc, mc[20]);\n'
      '\t\tbicopy<ToSegfault>(hostctx->rsp, mc[23]);\n'
      '\t\tbicopy<ToSegfault>(hostctx->r9, mc[6]);\n'
      '\t\tbicopy<ToSegfault>(hostctx->rdi, mc[1]);\n'
      '\t#elif defined(__FreeBSD__) || defined(__DragonFly__)\n\t\tbicopy<ToSegfault>(hostctx->pc, MCTX(.mc_rip));\n')

# --- The recompiler's dispatch, for the PS4's Jaguar cores: two changes of Resonance's (https://github.com/iHaiDeeZ/
# Resonance, GPL-2.0, a Flycast-based Dreamcast emulator for the PS4 by the author of DolphinPS4), plain x86-64, so
# the PC tests run them. (1) A block that ends with time left in the slice and the MMU off jumps straight to the
# next block through the jump table, instead of returning to the main loop to look it up and call it: the stack is
# as the main loop's call left it, so the next block (or ngen_FailedToFindBlock) runs exactly as if called from
# there, and its ret goes back to the main loop; an indirect jump at every block's end predicts far better on the
# Jaguar than the main loop's one shared call. (2) The main loop reads the jump table inline instead of calling
# bm_GetCodeByVAddr (without the MMU that is all it does), which saved and restored six registers around one load:
# ~30% of the emulation thread on the Jaguar by Resonance's profile. EMUSUNI_DC_DISPATCH=0 (or the file
# /data/EmuSuNi/dc-dispatch-off on the PS4) switches both off.
patch('core/rec-x64/rec_x64.cpp',
      '#include "cfg/option.h"\n',
      '#include "cfg/option.h"\n#ifdef EMUSUNI_PORT\n#include "emusuni_options.h"\n#endif\n')
patch('core/rec-x64/rec_x64.cpp',
      'static u64 jmp_rsp;\n',
      'static u64 jmp_rsp;\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the two dispatch changes below, unless switched off (emusuni_options.h).\n'
      'static bool emusuniDispatch()\n'
      '{\n'
      '\tstatic const bool enabled = []() {\n'
      '\t\tchar how[96];\n'
      '\t\tconst bool on = emusuni::dcOption("DISPATCH", how, sizeof(how));\n'
      '\t\tNOTICE_LOG(DYNAREC, "EmuSuNi: SH4 recompiler: tail dispatch and inline block lookup %s%s%s", on ? "on" : "off",\n'
      '\t\t\thow[0] != \'\\0\' ? ": " : "", how);\n'
      '\t\treturn on;\n'
      '\t}();\n'
      '\treturn enabled;\n'
      '}\n'
      '#endif\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\tL(exit_block);\n\t\tadd(rsp, STACK_ALIGN);\n\t\tret();\n',
      '\t\tL(exit_block);\n\t\tadd(rsp, STACK_ALIGN);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (emusuniDispatch())\n'
      '\t\t{\n'
      '\t\t\tXbyak::Label toMainloop;\n'
      '\t\t\tmov(rax, (uintptr_t)&sh4ctx.cycle_counter);\n'
      '\t\t\tcmp(dword[rax], 0);\n'
      '\t\t\tjle(toMainloop, T_NEAR);\n'
      '\t\t\tmov(rax, (size_t)&mmuOn);\n'
      '\t\t\tcmp(byte[rax], 0);\n'
      '\t\t\tjne(toMainloop, T_NEAR);\n'
      '\t\t\tmov(rax, (size_t)&sh4ctx.pc);\n'
      '\t\t\tmov(edx, dword[rax]);\n'
      '\t\t\tand_(edx, FPCB_MASK << 1);\n'
      '\t\t\tmov(rax, (size_t)&p_sh4rcb);\n'
      '\t\t\tmov(rax, qword[rax]);\n'
      '\t\t\tjmp(qword[rax + rdx * 4]);\n'
      '\t\t\tL(toMainloop);\n'
      '\t\t}\n'
      '#endif\n'
      '\t\tret();\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\tcall(bm_GetCodeByVAddr);\n\t\tcall(rax);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (emusuniDispatch())\n'
      '\t\t{\n'
      '\t\t\tXbyak::Label slowLookup, haveCode;\n'
      '\t\t\tmov(rax, (size_t)&mmuOn);\n'
      '\t\t\tcmp(byte[rax], 0);\n'
      '\t\t\tjne(slowLookup, T_NEAR);\n'
      '\t\t\tmov(rax, (size_t)&p_sh4rcb);\n'
      '\t\t\tmov(rax, qword[rax]);  // the jump table is the first member of the Sh4RCB\n'
      '\t\t\tmov(edx, call_regs[0]);\n'
      '\t\t\tand_(edx, FPCB_MASK << 1);\n'
      '\t\t\tmov(rax, qword[rax + rdx * 4]);  // fpcb[(pc >> 1) & FPCB_MASK]\n'
      '\t\t\tjmp(haveCode, T_NEAR);\n'
      '\t\t\tL(slowLookup);\n'
      '\t\t\tcall(bm_GetCodeByVAddr);\n'
      '\t\t\tL(haveCode);\n'
      '\t\t}\n'
      '\t\telse\n'
      '#endif\n'
      '\t\t\tcall(bm_GetCodeByVAddr);\n'
      '\t\tcall(rax);\n')

# --- The SH4's vector instructions (00.66): FTRV (a vector times the 4x4 matrix) and FIPR (a dot product), which
# 3D games run for every vertex they transform, done in x86 inside the recompiled block instead of a call of their
# C versions (shil_canonical.h's innerProduct; with the call around them ~7-9% of the emulation thread in Daytona
# USA and Crazy Taxi on the PC): the same double-precision products (exact: two floats' product fits a double),
# summed in the same order and rounded to float by the same rounding (the x86's, which the SH4's FPSCR sets), so
# the results are the same to the bit. The vectors are in the SH4 context's memory, as for the call (the register
# allocator writes them back before the instruction); only xmm0-xmm4, rax, rcx and rdx are used, which the call
# clobbered too, so the allocated xmm8-xmm11 aren't saved and restored around it. EMUSUNI_DC_VECTOR=0 (or the
# file /data/EmuSuNi/dc-vector-off) switches it off.
patch('core/rec-x64/rec_x64.cpp',
      'static bool emusuniDispatch()\n',
      'static bool emusuniVectorOps()\n'
      '{\n'
      '\tstatic const bool enabled = []() {\n'
      '\t\tchar how[96];\n'
      '\t\tconst bool on = emusuni::dcOption("VECTOR", how, sizeof(how));\n'
      '\t\tNOTICE_LOG(DYNAREC, "EmuSuNi: SH4 recompiler: FTRV and FIPR in x86 %s%s%s", on ? "on" : "off",\n'
      '\t\t\thow[0] != \'\\0\' ? ": " : "", how);\n'
      '\t\treturn on;\n'
      '\t}();\n'
      '\treturn enabled;\n'
      '}\n\n'
      'static bool emusuniDispatch()\n')
patch('core/rec-x64/rec_x64.cpp',
      '\tvoid canonStart(const shil_opcode& op)\n\t{\n\t\tCC_pars.clear();\n\t}\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t// FTRV: fd[j] = ((fn[0] * fm[j] + fn[1] * fm[4 + j]) + fn[2] * fm[8 + j]) + fn[3] * fm[12 + j] in doubles, j = 0..3\n'
      '\t// two at a time. Each operation\'s operands in the order the C version has them compiled (clang -O3,\n'
      '\t// btver2): the products fn[i] * fm, the first sum acc + p, the next ones p + acc - which of two NaNs comes\n'
      '\t// out of an operation is its first operand\'s. fd is fn: everything is read before it is written.\n'
      '\tvoid emusuniGenFtrv(const shil_opcode& op)\n'
      '\t{\n'
      '\t\tmov(rax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));\t// fn: FVn\n'
      '\t\tmov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));\t// fm: XMTRX (xf0..xf15)\n'
      '\t\tfor (int i = 0; i < 4; i++)\n'
      '\t\t{\n'
      '\t\t\tcvtss2sd(xmm4, dword[rax + i * 4]);\t\t// fn[i], twice\n'
      '\t\t\tunpcklpd(xmm4, xmm4);\n'
      '\t\t\tcvtps2pd(xmm2, qword[rcx + i * 16]);\t\t// fm[4i], fm[4i + 1]\n'
      '\t\t\tcvtps2pd(xmm3, qword[rcx + i * 16 + 8]);\t// fm[4i + 2], fm[4i + 3]\n'
      '\t\t\tconst Xbyak::Xmm lo = i == 0 ? xmm0 : xmm5;\n'
      '\t\t\tconst Xbyak::Xmm hi = i == 0 ? xmm1 : xmm4;\n'
      '\t\t\tmovapd(lo, xmm4);\n'
      '\t\t\tmulpd(lo, xmm2);\t\t\t\t\t\t\t// fn[i] * fm[4i], fn[i] * fm[4i + 1]\n'
      '\t\t\tif (i == 0)\n'
      '\t\t\t\tmovapd(hi, xmm4);\n'
      '\t\t\tmulpd(hi, xmm3);\t\t\t\t\t\t\t// fn[i] * fm[4i + 2], fn[i] * fm[4i + 3]\n'
      '\t\t\tif (i == 1)\n'
      '\t\t\t{\n'
      '\t\t\t\taddpd(xmm0, xmm5);\t\t\t\t\t\t// acc + p\n'
      '\t\t\t\taddpd(xmm1, xmm4);\n'
      '\t\t\t}\n'
      '\t\t\telse if (i > 1)\n'
      '\t\t\t{\n'
      '\t\t\t\taddpd(xmm5, xmm0);\t\t\t\t\t\t// p + acc\n'
      '\t\t\t\taddpd(xmm4, xmm1);\n'
      '\t\t\t\tmovapd(xmm0, xmm5);\n'
      '\t\t\t\tmovapd(xmm1, xmm4);\n'
      '\t\t\t}\n'
      '\t\t}\n'
      '\t\tcvtpd2ps(xmm0, xmm0);\n'
      '\t\tcvtpd2ps(xmm1, xmm1);\n'
      '\t\tmovlhps(xmm0, xmm1);\n'
      '\t\tmov(rdx, (uintptr_t)op.rd.reg_ptr(sh4ctx));\t// fd: FVn\n'
      '\t\tmovups(xword[rdx], xmm0);\n'
      '\t}\n\n'
      '\t// FIPR: fr[n + 3] = ((fn[0] * fm[0] + fn[1] * fm[1]) + fn[2] * fm[2]) + fn[3] * fm[3] in doubles, the operands\n'
      '\t// in the C version\'s order as for FTRV.\n'
      '\tvoid emusuniGenFipr(const shil_opcode& op)\n'
      '\t{\n'
      '\t\tmov(rax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));\t// fn: FVn\n'
      '\t\tmov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));\t// fm: FVm\n'
      '\t\tcvtps2pd(xmm0, qword[rax]);\n'
      '\t\tcvtps2pd(xmm1, qword[rcx]);\n'
      '\t\tmulpd(xmm0, xmm1);\t\t\t\t// fn[0] * fm[0], fn[1] * fm[1]\n'
      '\t\tcvtps2pd(xmm2, qword[rax + 8]);\n'
      '\t\tcvtps2pd(xmm3, qword[rcx + 8]);\n'
      '\t\tmulpd(xmm2, xmm3);\t\t\t\t// fn[2] * fm[2], fn[3] * fm[3]\n'
      '\t\tmovapd(xmm1, xmm0);\n'
      '\t\tunpckhpd(xmm1, xmm1);\n'
      '\t\taddsd(xmm0, xmm1);\t\t\t\t// acc = p0 + p1\n'
      '\t\tmovapd(xmm3, xmm2);\n'
      '\t\taddsd(xmm3, xmm0);\t\t\t\t// acc = p2 + acc\n'
      '\t\tunpckhpd(xmm2, xmm2);\n'
      '\t\taddsd(xmm2, xmm3);\t\t\t\t// p3 + acc\n'
      '\t\tcvtsd2ss(xmm0, xmm2);\n'
      '\t\thost_reg_to_shil_param(op.rd, xmm0);\t// as the call\'s result (fr[n + 3], maybe in a register)\n'
      '\t}\n'
      '#endif\n\n'
      '\tvoid canonStart(const shil_opcode& op)\n\t{\n\t\tCC_pars.clear();\n\t}\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\t\tdefault:\n#ifndef CANONICAL_TEST\n\t\t\t\tif (!genBaseOpcode(op))\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\tcase shop_ftrv:\n'
      '\t\t\tcase shop_fipr:\n'
      '\t\t\t\tif (emusuniVectorOps())\n'
      '\t\t\t\t{\n'
      '\t\t\t\t\tif (op.op == shop_ftrv)\n'
      '\t\t\t\t\t\temusuniGenFtrv(op);\n'
      '\t\t\t\t\telse\n'
      '\t\t\t\t\t\temusuniGenFipr(op);\n'
      '\t\t\t\t}\n'
      '\t\t\t\telse\n'
      '\t\t\t\t\tshil_chf[op.op](&op);\n'
      '\t\t\t\tbreak;\n'
      '#endif\n\n'
      '\t\t\tdefault:\n#ifndef CANONICAL_TEST\n\t\t\t\tif (!genBaseOpcode(op))\n')

# --- The SH4's memory accesses without saving the float registers (00.66): the recompiled code reads and writes the
# Dreamcast's memory through a call of a small handler (fast memory: one load or store; else, or for the hardware's
# addresses, Flycast's C functions), and around each of those calls it saved and restored the float registers it
# keeps in xmm8-xmm11 (up to 8 instructions a call, in the blocks that do 3D maths the most), because a C function may
# overwrite them. Now the handlers that call C (the slow ones, the store queue's) keep xmm8-xmm11 themselves (saved
# on their own stack around the call: only on that slow path), the fast ones never touch them, and the blocks call
# both without saving: the same values in the same registers, so the same results. EMUSUNI_DC_MEMCALLS=0 (or the
# file /data/EmuSuNi/dc-memcalls-off) switches it off.
patch('core/rec-x64/rec_x64.cpp',
      'static bool emusuniDispatch()\n',
      'static bool emusuniMemCallsKeepXmm()\n'
      '{\n'
      '\tstatic const bool enabled = []() {\n'
      '\t\tchar how[96];\n'
      '\t\tconst bool on = emusuni::dcOption("MEMCALLS", how, sizeof(how));\n'
      '\t\tNOTICE_LOG(DYNAREC, "EmuSuNi: SH4 recompiler: memory calls without saving xmm8-xmm11 %s%s%s", on ? "on" : "off",\n'
      '\t\t\thow[0] != \'\\0\' ? ": " : "", how);\n'
      '\t\treturn on;\n'
      '\t}();\n'
      '\treturn enabled;\n'
      '}\n\n'
      'static bool emusuniDispatch()\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::R], mmu_enabled());\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::R], mmu_enabled() || emusuniMemCallsKeepXmm());\n'
      '#else\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::R], mmu_enabled());\n'
      '#endif\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::W], mmu_enabled());\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::W], mmu_enabled() || emusuniMemCallsKeepXmm());\n'
      '#else\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::W], mmu_enabled());\n'
      '#endif\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\t\t\t\t\tL(no_sqw);\n'
      '\t\t\t\t\t\tif (size == MemSize::S32)\n'
      '\t\t\t\t\t\t\tjmp((const void *)addrspace::write32);\t// tail call\n',
      '\t\t\t\t\t\tL(no_sqw);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\t\tif (emusuniMemCallsKeepXmm())\n'
      '\t\t\t\t\t\t{\n'
      '\t\t\t\t\t\t\temusuniSlowCall(size == MemSize::S32 ? (const void *)addrspace::write32 : (const void *)addrspace::write64, 0);\n'
      '\t\t\t\t\t\t\tcontinue;\n'
      '\t\t\t\t\t\t}\n'
      '#endif\n'
      '\t\t\t\t\t\tif (size == MemSize::S32)\n'
      '\t\t\t\t\t\t\tjmp((const void *)addrspace::write32);\t// tail call\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\t\t\t\telse\n\t\t\t\t\t{\n\t\t\t\t\t\t// Slow path\n',
      '\t\t\t\t\telse\n\t\t\t\t\t{\n\t\t\t\t\t\t// Slow path\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\t\tif (emusuniMemCallsKeepXmm())\n'
      '\t\t\t\t\t\t{\n'
      '\t\t\t\t\t\t\tconst void *function;\n'
      '\t\t\t\t\t\t\tif (op == MemOp::R)\n'
      '\t\t\t\t\t\t\t\tfunction = size == MemSize::S8 ? (const void *)addrspace::read8 : size == MemSize::S16 ? (const void *)addrspace::read16\n'
      '\t\t\t\t\t\t\t\t\t\t: size == MemSize::S32 ? (const void *)addrspace::read32 : (const void *)addrspace::read64;\n'
      '\t\t\t\t\t\t\telse\n'
      '\t\t\t\t\t\t\t\tfunction = size == MemSize::S8 ? (const void *)addrspace::write8 : size == MemSize::S16 ? (const void *)addrspace::write16\n'
      '\t\t\t\t\t\t\t\t\t\t: size == MemSize::S32 ? (const void *)addrspace::write32 : (const void *)addrspace::write64;\n'
      '\t\t\t\t\t\t\temusuniSlowCall(function, op == MemOp::R && size == MemSize::S8 ? 1 : op == MemOp::R && size == MemSize::S16 ? 2 : 0);\n'
      '\t\t\t\t\t\t\tcontinue;\n'
      '\t\t\t\t\t\t}\n'
      '#endif\n')
patch('core/rec-x64/rec_x64.cpp',
      '\tvoid saveXmmRegisters()\n\t{\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t// A memory handler\'s slow path: the C function called with the blocks\' float registers (xmm8-xmm11, their low\n'
      '\t// 32 bits as saveXmmRegisters keeps them) saved around it on the handler\'s own stack (16-byte aligned for the\n'
      '\t// call), a byte or a word read sign-extended as before, then ret.\n'
      '\tvoid emusuniSlowCall(const void *function, int extend)\n'
      '\t{\n'
      '\t\tsub(rsp, 24);\n'
      '\t\tmovd(dword[rsp], xmm8);\n'
      '\t\tmovd(dword[rsp + 4], xmm9);\n'
      '\t\tmovd(dword[rsp + 8], xmm10);\n'
      '\t\tmovd(dword[rsp + 12], xmm11);\n'
      '\t\tcall(function);\n'
      '\t\tif (extend == 1)\n'
      '\t\t\tmovsx(eax, al);\n'
      '\t\telse if (extend == 2)\n'
      '\t\t\tmovsx(eax, ax);\n'
      '\t\tmovd(xmm8, dword[rsp]);\n'
      '\t\tmovd(xmm9, dword[rsp + 4]);\n'
      '\t\tmovd(xmm10, dword[rsp + 8]);\n'
      '\t\tmovd(xmm11, dword[rsp + 12]);\n'
      '\t\tadd(rsp, 24);\n'
      '\t\tret();\n'
      '\t}\n'
      '#endif\n\n'
      '\tvoid saveXmmRegisters()\n\t{\n')

# --- The Atomiswave cartridge's decryption (00.69): every 16-bit word the BIOS and the game take from the cartridge by
# DMA goes through two bit permutations, each a loop over its 16 bits (bitswap16 and decrypt: 9% of the emulation thread
# while Dolphin Blue boots, on the PC). By tables now: a permutation of 16 bits is the OR of the permutations of its two
# bytes (each bit of the result is one bit of the input), so two lookups give what the loop gave, for the four
# permutation boxes of the keys and the address's; the same words to the bit. And the word's index taken modulo the
# cartridge's size only past its end (a division each word).
patch('core/hw/naomi/awcartridge.cpp',
      'static u16 decrypt(u16 cipherText, u32 address, const u8 key)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (00.69): bitswap16 by tables (built once, thread-safe as a function\'s static).\n'
      'namespace {\n'
      'struct EmusuniSwapTables\n'
      '{\n'
      '\tu16 textLo[4][256], textHi[4][256], addrLo[256], addrHi[256];\n'
      '\tEmusuniSwapTables()\n'
      '\t{\n'
      '\t\tstatic const u8 addr_swap_vec[] = { 13,5,2, 14,10,9,4, 15,11,6,1, 12,8,7,3,0 };\n'
      '\t\tfor (int box = 0; box < 4; box++)\n'
      '\t\t{\n'
      '\t\t\tu8 text_swap_vec[16];\n'
      '\t\t\tfor (int i = 0; i < 16; i++)\n'
      '\t\t\t\ttext_swap_vec[i] = permutation_table[box][15 - i];\n'
      '\t\t\tfor (int b = 0; b < 256; b++)\n'
      '\t\t\t{\n'
      '\t\t\t\ttextLo[box][b] = bitswap16((u16)b, text_swap_vec);\n'
      '\t\t\t\ttextHi[box][b] = bitswap16((u16)(b << 8), text_swap_vec);\n'
      '\t\t\t}\n'
      '\t\t}\n'
      '\t\tfor (int b = 0; b < 256; b++)\n'
      '\t\t{\n'
      '\t\t\taddrLo[b] = bitswap16((u16)b, addr_swap_vec);\n'
      '\t\t\taddrHi[b] = bitswap16((u16)(b << 8), addr_swap_vec);\n'
      '\t\t}\n'
      '\t}\n'
      '};\n'
      '}\n'
      '#endif\n\n'
      'static u16 decrypt(u16 cipherText, u32 address, const u8 key)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tstatic const EmusuniSwapTables tables;\n'
      '\tconst int box = key >> 6;\n'
      '\tu16 aux = tables.textLo[box][cipherText & 0xff] | tables.textHi[box][cipherText >> 8];\n'
      '\taux = aux ^ (tables.addrLo[address & 0xff] | tables.addrHi[(address >> 8) & 0xff]);\n'
      '\tconst sbox_set* ss = &sboxes_table[(key>>4)&3];\n'
      '\tconst u8 b0 = ss->S0[aux & 0x1f];\n'
      '\tconst u8 b1 = ss->S1[(aux >> 5) & 0xf];\n'
      '\tconst u8 b2 = ss->S2[(aux >> 9) & 0xf];\n'
      '\tconst u8 b3 = ss->S3[aux >> 13];\n'
      '\treturn ((b3<<13)|(b2<<9)|(b1<<5)|b0)^xor_table[key&0xf];\n'
      '#else\n')
patch('core/hw/naomi/awcartridge.cpp',
      '\tb3 = ss->S3[b3];\n\n\treturn ((b3<<13)|(b2<<9)|(b1<<5)|b0)^xor_table[key&0xf];\n}\n',
      '\tb3 = ss->S3[b3];\n\n\treturn ((b3<<13)|(b2<<9)|(b1<<5)|b0)^xor_table[key&0xf];\n'
      '#endif\n'
      '}\n')
patch('core/hw/naomi/awcartridge.cpp',
      'u16 AWCartridge::decrypt16(u32 address) {\n\treturn decrypt(((u16 *)RomPtr)[address % (RomSize / 2)], address, rombd_key);\n',
      'u16 AWCartridge::decrypt16(u32 address) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tconst u32 words = RomSize / 2;\n'
      '\tconst u32 index = address < words ? address : address % words;  // (EmuSuNi: no division inside the cartridge)\n'
      '\treturn decrypt(((u16 *)RomPtr)[index], address, rombd_key);\n'
      '#else\n'
      '\treturn decrypt(((u16 *)RomPtr)[address % (RomSize / 2)], address, rombd_key);\n'
      '#endif\n')

# --- The SH4 waiting in a loop (00.69). Games wait for the next frame in a loop that only reads memory until the vblank's
# interrupt changes it: Crazy Taxi and Daytona USA (Sega's library: an empty function called, four words read, round
# again) spent 25-32% of the emulation thread there on the PC, Crazy Taxi's SH4 63% of its time in its races. A loop
# that comes round to the same state with nothing changed in between would go on the same way until something outside
# it changes the memory it reads, which only happens at an event (the scheduler's: the vblank, DMA ends, the sound chip,
# which also runs the ARM7) or an interrupt. So the time up to the next event goes by without running it, in whole
# slices as the main loop counts them (the TMU, the sound and every event see the same times), and the loop looks again
# after it.
#   - Each block that can write memory (a store, a store queue's write-back, an interpreted instruction) counts one
#     change when it starts (emusuniIdleChanges); so do every event the scheduler runs, an interrupt or exception taken,
#     a state loaded, a reset, and the reads of registers that change with time or by being read (the SH4's on-chip
#     ones: the TMU's counters...; the GD-ROM's and the arcade cartridge's ports).
#   - A block without any of that whose branch goes back (a loop's end), with no block that writes compiled between the
#     branch's target and it (MvC2's wait counts down a word in memory), checks when it takes the branch (inline, a call
#     only when nothing changed since the last check at the same target): if the SH4's registers (general, banked,
#     float, MAC, GBR, PR, FPUL, SR with T, FPSCR, the system ones) are what they were at that last check, the loop is at
#     a fixed point: the slice ends at once and UpdateSystem_INTC lets the next ones go by (the SH4 not run) until an
#     event runs or an interrupt is pending, at most 1024 slices. The loop then runs again from that branch's target
#     (the PC it would have passed every round). Only the interrupt's moment in the loop moves (at most one round of it).
#   - A library's counted wait (Marvel vs. Capcom 2 on the Dreamcast and the NAOMI, Virtua Tennis 2: 30-52% of the
#     emulation thread) writes as it waits: it calls an empty function through a dispatcher, counts a word in memory
#     down, and goes on while another word is the same and the count isn't 0. Its three blocks are recognised when its
#     end is compiled (their SH4 code, word for word), the dispatcher and the empty function where they run; at its
#     branch back the rounds that fit before the next event go by, the count counted down by them (never to 0: the
#     loop ends by itself) before that event runs. The same pictures and sound to the bit on the PC tests.
#   - Not with the MMU on (Windows CE games), and off with EMUSUNI_DC_IDLE=0 or the file /data/EmuSuNi/dc-idle-off.
#   - The log's 10 s line gives the share of the console's time let go by this way.
addfile('core/emusuni_idle.h', r'''// EmuSuNi (scripts/build-dc-core.sh, 00.69): the SH4 waiting in a loop, let go by to the next event.
#pragma once
#ifdef EMUSUNI_PORT
#include "types.h"

// The last check of a loop's branch back (rec-x64/rec_x64.cpp writes target and seen inline; the call compares the
// registers): hw/sh4/interpr/sh4_interpreter.cpp.
struct EmusuniIdle
{
	u32 seen;        // emusuniIdleChanges at the last check
	u32 target;      // its branch's target
	u32 saved;       // whether the registers below are that check's
	u32 status, T, fpscr;
	u32 cooldown;    // after registers that differed (a loop that works), the checks let go by before the next compare
	bool skip;       // the time goes by to the next event (UpdateSystem_INTC)
	// A library's counted wait (emusuniCountdownCheck): its counter's address, the cycles of one round, the cycles that may go
	// by (its count less one round: the loop ends by itself at 0), those already gone (the slice's rest).
	bool countdown;
	u32 cdAddress, cdCycles;
	int64_t cdBudget, cdSkipped;
	u8 regs[512];    // xf..fpul: the float registers, the general ones, MAC, the banked ones, GBR...VBR, PR, FPUL
};
extern EmusuniIdle emusuniIdle;
// Anything that may change what a waiting loop reads (a block that writes memory as it starts, an event, an
// interrupt, a state loaded, a read of the TMU's counters...).
extern u32 emusuniIdleChanges;
// The registers compared with the last check's (called when nothing changed since it and its target is the same).
void emusuniIdleCheck(u32 target);
// A library's counted wait, at its loop's branch back (rec-x64/rec_x64.cpp found its code): the rounds the time to the next event
// holds go by, its counter counted down by them.
void emusuniCountdownCheck(u32 target);
#endif
''')
patch('core/rec-x64/rec_x64.cpp',
      'static bool emusuniDispatch()\n{\n',
      '// EmuSuNi (00.69, hw/sh4/interpr/sh4_interpreter.cpp): the SH4 waiting in a loop.\n'
      '#include "emusuni_idle.h"\n'
      'static bool emusuniIdleOn()\n'
      '{\n'
      '\tstatic const bool enabled = []() {\n'
      '\t\tchar how[96];\n'
      '\t\tconst bool on = emusuni::dcOption("IDLE", how, sizeof(how));\n'
      '\t\tNOTICE_LOG(DYNAREC, "EmuSuNi: SH4 recompiler: waiting loops let go by to the next event %s%s%s", on ? "on" : "off",\n'
      '\t\t\thow[0] != \'\\0\' ? ": " : "", how);\n'
      '\t\treturn on;\n'
      '\t}();\n'
      '\treturn enabled;\n'
      '}\n'
      '\n'
      '// Whether a block writes memory (a store, a store queue\'s write-back, an interpreted instruction).\n'
      'static bool emusuniWrites(const RuntimeBlockInfo *block)\n'
      '{\n'
      '\tfor (const shil_opcode& op : block->oplist)\n'
      '\t\tif (op.op == shop_writem || op.op == shop_pref || op.op == shop_ifb)\n'
      '\t\t\treturn true;\n'
      '\treturn false;\n'
      '}\n'
      '\n'
      '// Whether a loop\'s branch back from `block` to `target` is worth checking: nothing compiled between them writes\n'
      '// (a loop that writes on every round never comes round to the same state), within 1 KiB.\n'
      'static bool emusuniLoopReadsOnly(const RuntimeBlockInfo *block, u32 target)\n'
      '{\n'
      '\tif (target > block->vaddr || block->vaddr - target > 0x400)\n'
      '\t\treturn false;\n'
      '\tfor (u32 a = target; a < block->vaddr; a += 2)\n'
      '\t{\n'
      '\t\tRuntimeBlockInfoPtr other = bm_GetBlock(a);\n'
      '\t\tif (other != nullptr && emusuniWrites(other.get()))\n'
      '\t\t\treturn false;\n'
      '\t}\n'
      '\treturn true;\n'
      '}\n'
      '\n'
      '// A library\'s counted wait (Marvel vs. Capcom 2 on the Dreamcast and the NAOMI, Virtua Tennis 2: 30-52% of the\n'
      '// emulation thread):\n'
      '//   A: jsr @Rn; mov #imm,R4             (its dispatcher: table[imm].function(table[imm].argument), an empty one)\n'
      '//   B: mov.l @R15,R3; mov.l @R3,R2; add #-1,R2; mov.l R2,@R3     (--*counter)\n'
      '//      mov.l @(4,R15),R3; mov.l @R3,R2; cmp/eq R14,R2; bf out     (*flag != R14: out)\n'
      '//   C: mov.l @R15,R1; mov.l @R1,R3; tst R3,R3; bf A               (*counter != 0: again)\n'
      '// Whether `block` is its C (A and B before it, as the RAM holds them; the dispatcher is checked as it runs).\n'
      'static bool emusuniIsCountdownLoop(const RuntimeBlockInfo *block)\n'
      '{\n'
      '\tconst u32 c = block->vaddr, a = block->BranchBlock;\n'
      '\tif ((c & 0x1C000000) != 0x0C000000 || (a & 0x1C000000) != 0x0C000000 || a + 20 != c || block->guest_opcodes != 4)\n'
      '\t\treturn false;\n'
      '\tauto op = [](u32 at) { return *(const u16 *)&mem_b[at & RAM_MASK]; };\n'
      '\tif ((op(a) & 0xF0FF) != 0x400B || (op(a + 2) & 0xFF00) != 0xE400)\n'
      '\t\treturn false;\n'
      '\tstatic const u16 B[7] = { 0x63F2, 0x6232, 0x72FF, 0x2322, 0x53F1, 0x6232, 0x32E0 };\n'
      '\tfor (int i = 0; i < 7; i++)\n'
      '\t\tif (op(a + 4 + i * 2) != B[i])\n'
      '\t\t\treturn false;\n'
      '\tstatic const u16 C[3] = { 0x61F2, 0x6312, 0x2338 };\n'
      '\tfor (int i = 0; i < 3; i++)\n'
      '\t\tif (op(c + i * 2) != C[i])\n'
      '\t\t\treturn false;\n'
      '\tconst u16 bf = op(c + 6);\n'
      '\treturn (op(a + 18) & 0xFF00) == 0x8B00 && (bf & 0xFF00) == 0x8B00 && c + 6 + 4 + (s32)(s8)(bf & 0xFF) * 2 == a;\n'
      '}\n'
      '\n'
      'static bool emusuniDispatch()\n{\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\tsub(dword[rax], block->guest_cycles);\n\n\t\tregalloc.DoAlloc(block);\n',
      '\t\tsub(dword[rax], block->guest_cycles);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t// EmuSuNi (00.69): a block that can write memory counts a change as it starts (emusuni_idle.h).\n'
      '\t\tconst bool emusuniIdle = emusuniIdleOn() && !mmu_enabled();\n'
      '\t\tconst bool emusuniStores = emusuniWrites(block);\n'
      '\t\tif (emusuniIdle && emusuniStores)\n'
      '\t\t{\n'
      '\t\t\tmov(rax, (uintptr_t)&emusuniIdleChanges);\n'
      '\t\t\tinc(dword[rax]);\n'
      '\t\t}\n'
      '\t\t// (its branch back, when taken: the check, for a loop that only reads)\n'
      '\t\tconst bool emusuniBackCheck = emusuniIdle && !emusuniStores\n'
      '\t\t\t\t&& (block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1 || block->BlockType == BET_StaticJump)\n'
      '\t\t\t\t&& emusuniLoopReadsOnly(block, block->BranchBlock);\n'
      '\t\t// (the library\'s counted wait: its own check)\n'
      '\t\tconst bool emusuniCountdown = emusuniIdle && !emusuniStores\n'
      '\t\t\t\t&& (block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1) && emusuniIsCountdownLoop(block);\n'
      '#endif\n'
      '\n\t\tregalloc.DoAlloc(block);\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\t\t\tjne(branch_not_taken, T_SHORT);\n\t\t\t\tmov(dword[rax], block->BranchBlock);\n\t\t\t\tL(branch_not_taken);\n',
      '\t\t\t\tjne(branch_not_taken, T_SHORT);\n\t\t\t\tmov(dword[rax], block->BranchBlock);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\tif (emusuniCountdown)\n'
      '\t\t\t\t{\n'
      '\t\t\t\t\tmov(call_regs[0], block->BranchBlock);\n'
      '\t\t\t\t\tGenCall(emusuniCountdownCheck, true);  // (the registers are written back: no float register live)\n'
      '\t\t\t\t}\n'
      '\t\t\t\telse if (emusuniBackCheck)\n'
      '\t\t\t\t\temusuniGenIdleCheck(block->BranchBlock);\n'
      '#endif\n'
      '\t\t\t\tL(branch_not_taken);\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\tcase BET_StaticJump:\n\t\tcase BET_StaticCall:\n\t\t\t//next_pc = block->BranchBlock;\n\t\t\tmov(dword[rax], block->BranchBlock);\n\t\t\tbreak;\n',
      '\t\tcase BET_StaticJump:\n\t\tcase BET_StaticCall:\n\t\t\t//next_pc = block->BranchBlock;\n\t\t\tmov(dword[rax], block->BranchBlock);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\tif (emusuniBackCheck && block->BlockType == BET_StaticJump)\n'
      '\t\t\t\temusuniGenIdleCheck(block->BranchBlock);\n'
      '#endif\n'
      '\t\t\tbreak;\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\tret();\n\t}\n#endif\n\n\tvoid saveXmmRegisters()\n\t{\n',
      '\t\tret();\n\t}\n\n'
      '\t// (00.69) A loop\'s branch back taken: the check (emusuni_idle.h). Inline while something changed since the last\n'
      '\t// check or its target was another (the check\'s state written, a few instructions), else the call that compares\n'
      '\t// the registers. The SH4\'s registers are written back here; rax, rcx, rdi and the call\'s scratch registers are free.\n'
      '\tvoid emusuniGenIdleCheck(u32 target)\n'
      '\t{\n'
      '\t\tXbyak::Label changed, done;\n'
      '\t\tmov(rax, (uintptr_t)&emusuniIdleChanges);\n'
      '\t\tmov(ecx, dword[rax]);\n'
      '\t\tmov(rax, (uintptr_t)&emusuniIdle);\n'
      '\t\tcmp(ecx, dword[rax + (int)offsetof(EmusuniIdle, seen)]);\n'
      '\t\tjne(changed, T_SHORT);\n'
      '\t\tcmp(dword[rax + (int)offsetof(EmusuniIdle, target)], target);\n'
      '\t\tjne(changed, T_SHORT);\n'
      '\t\tmov(call_regs[0], target);\n'
      '\t\tGenCall(emusuniIdleCheck, true);\n'
      '\t\tjmp(done, T_SHORT);\n'
      '\t\tL(changed);\n'
      '\t\tmov(dword[rax + (int)offsetof(EmusuniIdle, seen)], ecx);\n'
      '\t\tmov(dword[rax + (int)offsetof(EmusuniIdle, target)], target);\n'
      '\t\tmov(dword[rax + (int)offsetof(EmusuniIdle, saved)], 0);\n'
      '\t\tL(done);\n'
      '\t}\n'
      '#endif\n\n\tvoid saveXmmRegisters()\n\t{\n')
patch('core/hw/sh4/interpr/sh4_interpreter.cpp',
      '#include "../sh4_cycles.h"\n',
      '#include "../sh4_cycles.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "emusuni_idle.h"\n'
      '#include "emusuni_timing.h"\n'
      '#include "hw/sh4/dyna/blockmanager.h"\n'
      '#include <cstring>\n'
      '#endif\n')
patch('core/hw/sh4/interpr/sh4_interpreter.cpp',
      'int UpdateSystem_INTC()\n{\n\tSh4cntx.sh4_sched_next -= SH4_TIMESLICE;\n\tif (Sh4cntx.sh4_sched_next < 0)\n\t\tsh4_sched_tick(SH4_TIMESLICE);\n'
      '\tif (Sh4cntx.interrupt_pend)\n\t\treturn UpdateINTC();\n\telse\n\t\treturn 0;\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (00.69, emusuni_idle.h): the SH4 waiting in a loop.\n'
      'u32 emusuniIdleChanges;\n'
      'EmusuniIdle emusuniIdle { 0, ~0u };\n'
      '\n'
      '// The registers at a loop\'s branch back, the same target as the last check and nothing changed since it: if they\n'
      '// are that check\'s, the loop is at a fixed point - the slice ends here and the time goes by to the next event.\n'
      'void emusuniIdleCheck(u32 target)\n'
      '{\n'
      '\tSh4Context& ctx = p_sh4rcb->cntx;\n'
      '\tconst u8 *regs = (const u8 *)&ctx.xf[0];\n'
      '\tconst size_t size = (const u8 *)(&ctx.fpul + 1) - regs;\n'
      '\tconst size_t r = (const u8 *)&ctx.r[0] - regs;  // (the general registers first: a loop that works changes them)\n'
      '\tif (size > sizeof(emusuniIdle.regs))\n'
      '\t\treturn;\n'
      '\tif (!emusuniIdle.saved)\n'
      '\t\temusuniIdle.cooldown = 0;\n'
      '\telse if (emusuniIdle.cooldown > 0)\n'
      '\t{\n'
      '\t\temusuniIdle.cooldown--;  // (a loop that works: compared again in a few rounds, still with nothing changed)\n'
      '\t\treturn;\n'
      '\t}\n'
      '\telse if (memcmp(emusuniIdle.regs + r, regs + r, sizeof(ctx.r)) == 0\n'
      '\t\t\t&& memcmp(emusuniIdle.regs, regs, size) == 0 && emusuniIdle.status == ctx.sr.status && emusuniIdle.T == ctx.sr.T\n'
      '\t\t\t&& emusuniIdle.fpscr == ctx.fpscr.full)\n'
      '\t{\n'
      '\t\tif (ctx.cycle_counter > 0)\n'
      '\t\t\temusuni::timing.idleCycles += ctx.cycle_counter;\n'
      '\t\tctx.cycle_counter = 0;\n'
      '\t\temusuniIdle.skip = true;\n'
      '\t\treturn;\n'
      '\t}\n'
      '\telse\n'
      '\t\temusuniIdle.cooldown = 7;\n'
      '\tmemcpy(emusuniIdle.regs, regs, size);\n'
      '\temusuniIdle.status = ctx.sr.status;\n'
      '\temusuniIdle.T = ctx.sr.T;\n'
      '\temusuniIdle.fpscr = ctx.fpscr.full;\n'
      '\temusuniIdle.saved = 1;\n'
      '}\n'
      '\n'
      '// The library\'s counted wait at its branch back (rec-x64/rec_x64.cpp: emusuniIsCountdownLoop found A, B and C): a round calls\n'
      '// table[imm].function through the dispatcher in Rn (the same 17 instructions, its stack words the same each round)\n'
      '// where an empty function (rts; nop) stands, counts *counter down, and goes on while *flag is R14 and *counter isn\'t\n'
      '// 0. Until the next event nothing else changes what it reads: the rounds that fit in that time go by, *counter\n'
      '// counted down by them (never to 0: the loop ends by itself), the cycles of a round the six blocks\' own.\n'
      'void emusuniCountdownCheck(u32 target)\n'
      '{\n'
      '\tSh4Context& ctx = p_sh4rcb->cntx;\n'
      '\tauto ram = [](u32 at) { return (at & 0x1C000000) == 0x0C000000; };\n'
      '\tauto op = [](u32 at) { return *(const u16 *)&mem_b[at & RAM_MASK]; };\n'
      '\tauto word = [](u32 at) { return *(const u32 *)&mem_b[at & RAM_MASK & ~3u]; };\n'
      '\tconst u32 d = ctx.r[(op(target) >> 8) & 0xF];  // jsr @Rn\n'
      '\tconst s32 imm = (s8)(op(target + 2) & 0xFF);   // mov #imm,R4\n'
      '\tif (!ram(d) || (d & 1) != 0)\n'
      '\t\treturn;\n'
      '\tstatic const u16 D[17] = { 0x4F22, 0x7FF8, 0x6243, 0xD300, 0x4208, 0x2F42, 0x4200, 0x323C, 0x6423, 0x1F21, 0x6122,\n'
      '\t\t\t0x410B, 0x5441, 0x7F08, 0x4F26, 0x000B, 0xE000 };\n'
      '\tfor (int i = 0; i < 17; i++)\n'
      '\t\tif ((i == 3 ? op(d + i * 2) & 0xFF00 : op(d + i * 2)) != D[i])\n'
      '\t\t\treturn;\n'
      '\tconst u32 tableAt = ((d + 6) & ~3u) + 4 + (op(d + 6) & 0xFF) * 4;  // mov.l @(disp,PC),R3\n'
      '\tif (!ram(tableAt))\n'
      '\t\treturn;\n'
      '\tconst u32 entry = word(tableAt) + (u32)(imm * 8);\n'
      '\tif (!ram(entry) || (entry & 3) != 0)\n'
      '\t\treturn;\n'
      '\tconst u32 function = word(entry);\n'
      '\tif (!ram(function) || (function & 1) != 0 || op(function) != 0x000B || op(function + 2) != 0x0009)  // rts; nop\n'
      '\t\treturn;\n'
      '\tconst u32 sp = ctx.r[15];\n'
      '\tif (!ram(sp) || (sp & 3) != 0)\n'
      '\t\treturn;\n'
      '\tconst u32 counterAt = word(sp), flagAt = word(sp + 4);\n'
      '\tif (!ram(counterAt) || !ram(flagAt) || ((counterAt | flagAt) & 3) != 0)\n'
      '\t\treturn;\n'
      '\tconst u32 count = word(counterAt);\n'
      '\tif (count < 2 || word(flagAt) != ctx.r[14])\n'
      '\t\treturn;\n'
      '\tu32 cycles = 0;\n'
      '\tfor (u32 at : { target, target + 4, target + 20, d, d + 26, function })\n'
      '\t{\n'
      '\t\tRuntimeBlockInfoPtr block = bm_GetBlock(at);\n'
      '\t\tif (block == nullptr)\n'
      '\t\t\treturn;  // (not all compiled yet: a round as before)\n'
      '\t\tcycles += block->guest_cycles;\n'
      '\t}\n'
      '\tconst int64_t left = ctx.cycle_counter > 0 ? ctx.cycle_counter : 0;\n'
      '\tconst int64_t budget = (int64_t)(count - 1) * cycles;\n'
      '\tif (cycles == 0 || budget < left + SH4_TIMESLICE)\n'
      '\t\treturn;\n'
      '\temusuniIdle.countdown = true;\n'
      '\temusuniIdle.cdAddress = counterAt;\n'
      '\temusuniIdle.cdCycles = cycles;\n'
      '\temusuniIdle.cdBudget = budget;\n'
      '\temusuniIdle.cdSkipped = left;\n'
      '\temusuni::timing.idleCycles += left;\n'
      '\tctx.cycle_counter = 0;\n'
      '\temusuniIdle.skip = true;\n'
      '}\n'
      '\n'
      '// The counted wait: its counter counted down by the rounds that went by (before the event that ends them runs).\n'
      'static void emusuniCountdownApply(int64_t skipped)\n'
      '{\n'
      '\tif (!emusuniIdle.countdown)\n'
      '\t\treturn;\n'
      '\temusuniIdle.countdown = false;\n'
      '\tconst u32 rounds = (u32)((emusuniIdle.cdSkipped + skipped) / emusuniIdle.cdCycles);\n'
      '\tu32& counter = *(u32 *)&mem_b[emusuniIdle.cdAddress & RAM_MASK];\n'
      '\tconst u32 most = counter > 0 ? counter - 1 : 0;\n'
      '\tcounter -= rounds < most ? rounds : most;\n'
      '}\n'
      '\n'
      '// The slices go by without the SH4 (the loop would only come round again) until an event is due or an interrupt is\n'
      '// pending (the counted wait: or its rounds run out): the scheduler counted as the main loop counts it, slice by slice.\n'
      'static void emusuniIdleWait()\n'
      '{\n'
      '\tint64_t skipped = 0;\n'
      '\tfor (int i = 0; i < 1024 && !Sh4cntx.interrupt_pend; i++)\n'
      '\t{\n'
      '\t\tif (emusuniIdle.countdown && emusuniIdle.cdSkipped + skipped + SH4_TIMESLICE > emusuniIdle.cdBudget)\n'
      '\t\t\tbreak;\n'
      '\t\tSh4cntx.sh4_sched_next -= SH4_TIMESLICE;\n'
      '\t\tskipped += SH4_TIMESLICE;\n'
      '\t\tif (Sh4cntx.sh4_sched_next < 0)\n'
      '\t\t{\n'
      '\t\t\temusuniCountdownApply(skipped);\n'
      '\t\t\tsh4_sched_tick(SH4_TIMESLICE);\n'
      '\t\t\tbreak;\n'
      '\t\t}\n'
      '\t}\n'
      '\temusuniCountdownApply(skipped);\n'
      '\temusuni::timing.idleCycles += skipped;\n'
      '\temusuni::timing.idleSkips++;\n'
      '}\n'
      '#endif\n'
      '\n'
      'int UpdateSystem_INTC()\n{\n\tSh4cntx.sh4_sched_next -= SH4_TIMESLICE;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (Sh4cntx.sh4_sched_next < 0)\n'
      '\t\temusuniCountdownApply(0);  // (the slice\'s rest went by: counted before the event runs)\n'
      '#endif\n'
      '\tif (Sh4cntx.sh4_sched_next < 0)\n\t\tsh4_sched_tick(SH4_TIMESLICE);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\telse if (emusuniIdle.skip)\n'
      '\t\temusuniIdleWait();\n'
      '\temusuniCountdownApply(0);\n'
      '\temusuniIdle.skip = false;\n'
      '#endif\n'
      '\tif (Sh4cntx.interrupt_pend)\n\t\treturn UpdateINTC();\n\telse\n\t\treturn 0;\n}\n')
patch('core/hw/sh4/sh4_sched.cpp',
      'void sh4_sched_tick(int cycles)\n{\n\tif (Sh4cntx.sh4_sched_next >= 0)\n\t\treturn;\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern u32 emusuniIdleChanges;  // (00.69: emusuni_idle.h)\n'
      '#endif\n'
      'void sh4_sched_tick(int cycles)\n{\n\tif (Sh4cntx.sh4_sched_next >= 0)\n\t\treturn;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniIdleChanges++;  // an event runs: what a waiting loop reads may change\n'
      '#endif\n')
patch('core/hw/sh4/sh4_sched.cpp',
      'void sh4_sched_deserialize(Deserializer& deser)\n{\n\tdeser >> sh4_sched_ffb;\n',
      'void sh4_sched_deserialize(Deserializer& deser)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniIdleChanges++;  // a state loaded\n'
      '#endif\n'
      '\tdeser >> sh4_sched_ffb;\n')
patch('core/hw/sh4/sh4_sched.cpp',
      'void sh4_sched_reset(bool hard)\n{\n',
      'void sh4_sched_reset(bool hard)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniIdleChanges++;\n'
      '#endif\n')
patch('core/hw/sh4/sh4_interrupts.cpp',
      '\tDo_Interrupt(InterruptEnvId[bitscanrev(Sh4cntx.interrupt_pend)]);\n\treturn 1;\n',
      '#ifdef EMUSUNI_PORT\n'
      '\textern u32 emusuniIdleChanges;\n'
      '\temusuniIdleChanges++;  // (00.69) an interrupt taken\n'
      '#endif\n'
      '\tDo_Interrupt(InterruptEnvId[bitscanrev(Sh4cntx.interrupt_pend)]);\n\treturn 1;\n')
patch('core/hw/sh4/sh4_interrupts.cpp',
      '\tCCN_EXPEVT = expEvn;\n',
      '#ifdef EMUSUNI_PORT\n'
      '\textern u32 emusuniIdleChanges;\n'
      '\temusuniIdleChanges++;  // (00.69) an exception taken\n'
      '#endif\n'
      '\tCCN_EXPEVT = expEvn;\n')
patch('core/hw/sh4/sh4_mmr.cpp',
      'T DYNACALL ReadMem_p4mmr(u32 addr)\n{\n',
      'T DYNACALL ReadMem_p4mmr(u32 addr)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\textern u32 emusuniIdleChanges;\n'
      '\temusuniIdleChanges++;  // (00.69) the on-chip registers: the TMU\'s counters change with time\n'
      '#endif\n')
patch('core/hw/holly/sb_mem.cpp',
      '\t\tif (addr >= 0x005F7000 && addr <= 0x005F70FF)\n\t\t{\n\t\t\tif constexpr (System == DC_PLATFORM_DREAMCAST)\n\t\t\t\treturn (T)ReadMem_gdrom(addr, sz);\n',
      '\t\tif (addr >= 0x005F7000 && addr <= 0x005F70FF)\n\t\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\textern u32 emusuniIdleChanges;\n'
      '\t\t\temusuniIdleChanges++;  // (00.69) the GD-ROM\'s and the cartridge\'s ports: a read takes data\n'
      '#endif\n'
      '\t\t\tif constexpr (System == DC_PLATFORM_DREAMCAST)\n\t\t\t\treturn (T)ReadMem_gdrom(addr, sz);\n')

# --- The SH4's memory accesses in the block (00.69). With the fast memory each access whose address isn't known when
# the block is compiled was a call of a 4-instruction function and its return (the address masked, the RAM read or
# written at the fast memory's base); the call and the return took 5-15% of the emulation thread on the PC (Virtua
# Tennis 2, Marvel vs. Capcom 2: 15%). Those 4 instructions are now in the block itself, the same bytes as the
# function's (the base as a 64-bit constant, the address kept in r9). An access to the hardware's addresses still
# faults; the fault handler finds the 4 instructions before the faulting one (compared with the function's own bytes)
# and writes over them the call of the slow handler (or the store queue's) that the call would have been rewritten to,
# with no-ops after it, and the access goes on there - as for the call before. Only with the fast memory, the MMU off
# and the float registers kept by the slow handlers (the switch above, on by default); off with EMUSUNI_DC_INLINEMEM=0
# or the file /data/EmuSuNi/dc-inlinemem-off.
patch('core/rec-x64/rec_x64.cpp',
      'static bool emusuniDispatch()\n{\n',
      '// EmuSuNi (00.69): the memory accesses in the block (BlockCompiler::emusuniGenInlineAccess).\n'
      '#include <cstring>\n'
      'static bool emusuniInlineMemOn()\n'
      '{\n'
      '\tstatic const bool enabled = []() {\n'
      '\t\tchar how[96];\n'
      '\t\tconst bool on = emusuni::dcOption("INLINEMEM", how, sizeof(how));\n'
      '\t\tNOTICE_LOG(DYNAREC, "EmuSuNi: SH4 recompiler: memory accesses in the block %s%s%s", on ? "on" : "off",\n'
      '\t\t\thow[0] != \'\\0\' ? ": " : "", how);\n'
      '\t\treturn on;\n'
      '\t}();\n'
      '\treturn enabled;\n'
      '}\n'
      '\n'
      'static bool emusuniDispatch()\n{\n')
patch('core/rec-x64/rec_x64.cpp',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::R], mmu_enabled() || emusuniMemCallsKeepXmm());\n'
      '#else\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\tif (optimise && emusuniInlineMem())\n'
      '\t\t\t\t\t\temusuniGenInlineAccess(size, MemOp::R);\n'
      '\t\t\t\t\telse\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::R], mmu_enabled() || emusuniMemCallsKeepXmm());\n'
      '#else\n')
patch('core/rec-x64/rec_x64.cpp',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::W], mmu_enabled() || emusuniMemCallsKeepXmm());\n'
      '#else\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t\t\tif (optimise && emusuniInlineMem())\n'
      '\t\t\t\t\t\temusuniGenInlineAccess(size, MemOp::W);\n'
      '\t\t\t\t\telse\n'
      '\t\t\t\t\tGenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::W], mmu_enabled() || emusuniMemCallsKeepXmm());\n'
      '#else\n')
patch('core/rec-x64/rec_x64.cpp',
      '\t\tret();\n\t}\n\n'
      '\t// (00.69) A loop\'s branch back taken: the check (emusuni_idle.h).',
      '\t\tret();\n\t}\n\n'
      'public:\n'
      '\t// (00.69) Whether a block\'s accesses go in it (emusuniGenInlineAccess): the fast memory, its base a 64-bit address\n'
      '\t// (the 10-byte mov the fault handler looks for), the MMU off, the slow handlers keeping the float registers.\n'
      '\tbool emusuniInlineMem()\n'
      '\t{\n'
      '\t\treturn emusuniInlineMemOn() && addrspace::virtmemEnabled() && !mmu_enabled() && emusuniMemCallsKeepXmm()\n'
      '\t\t\t\t&& ((uintptr_t)addrspace::ram_base >> 32) != 0;\n'
      '\t}\n'
      '\n'
      '\t// (00.69) A fast access in the block: the fast handler\'s instructions but its ret (genMemHandlers), the address in\n'
      '\t// call_regs[0] (kept in r9), a write\'s value in call_regs64[1], a read\'s in eax/rax.\n'
      '\tvoid emusuniGenInlineAccess(int size, int op)\n'
      '\t{\n'
      '\t\tmov(rax, (uintptr_t)addrspace::ram_base);\n'
      '\t\tmov(r9, call_regs64[0]);\n'
      '\t\tand_(call_regs[0], 0x1FFFFFFF);\n'
      '\t\tswitch (size)\n'
      '\t\t{\n'
      '\t\tcase MemSize::S8:\n'
      '\t\t\tif (op == MemOp::R)\n'
      '\t\t\t\tmovsx(eax, byte[rax + call_regs64[0]]);\n'
      '\t\t\telse\n'
      '\t\t\t\tmov(byte[rax + call_regs64[0]], call_regs[1].cvt8());\n'
      '\t\t\tbreak;\n'
      '\t\tcase MemSize::S16:\n'
      '\t\t\tif (op == MemOp::R)\n'
      '\t\t\t\tmovsx(eax, word[rax + call_regs64[0]]);\n'
      '\t\t\telse\n'
      '\t\t\t\tmov(word[rax + call_regs64[0]], call_regs[1].cvt16());\n'
      '\t\t\tbreak;\n'
      '\t\tcase MemSize::S32:\n'
      '\t\t\tif (op == MemOp::R)\n'
      '\t\t\t\tmov(eax, dword[rax + call_regs64[0]]);\n'
      '\t\t\telse\n'
      '\t\t\t\tmov(dword[rax + call_regs64[0]], call_regs[1]);\n'
      '\t\t\tbreak;\n'
      '\t\tcase MemSize::S64:\n'
      '\t\t\tif (op == MemOp::R)\n'
      '\t\t\t\tmov(rax, qword[rax + call_regs64[0]]);\n'
      '\t\t\telse\n'
      '\t\t\t\tmov(qword[rax + call_regs64[0]], call_regs64[1]);\n'
      '\t\t\tbreak;\n'
      '\t\t}\n'
      '\t}\n'
      '\n'
      '\t// (00.69) An access in the block faulted at `pc` (the hardware\'s addresses): the 19 bytes before it and it are a fast\n'
      '\t// handler\'s (the same bytes but its ret)? Then the call its fault would have been rewritten to, over them: the slow\n'
      '\t// handler, the store queue\'s for a write there, and no-ops to the end; the access then runs from there.\n'
      '\tstatic bool emusuniFindInlineAccess(const u8 *pc, int& size, int& op, int& length)\n'
      '\t{\n'
      '\t\tfor (size = 0; size < MemSize::Count; size++)\n'
      '\t\t\tfor (op = 0; op < MemOp::Count; op++)\n'
      '\t\t\t{\n'
      '\t\t\t\tconst u8 *stub = (const u8 *)MemHandlers[MemType::Fast][size][op];\n'
      '\t\t\t\tif (stub == nullptr || memcmp(pc - 19, stub, 19) != 0)\n'
      '\t\t\t\t\tcontinue;\n'
      '\t\t\t\tlength = 0;\n'
      '\t\t\t\twhile (length < 8 && stub[19 + length] != 0xC3)  // (the access, up to the handler\'s ret)\n'
      '\t\t\t\t\tlength++;\n'
      '\t\t\t\tif (length > 0 && length < 8 && memcmp(pc, stub + 19, length) == 0)\n'
      '\t\t\t\t\treturn true;\n'
      '\t\t\t}\n'
      '\t\treturn false;\n'
      '\t}\n'
      '\tvoid emusuniRewriteInlineAccess(int size, int op, int length, u32 memAddress)\n'
      '\t{\n'
      '\t\tconst u8 *start = getCurr();\n'
      '\t\tif (op == MemOp::W && size >= MemSize::S32 && (memAddress >> 26) == 0x38)\n'
      '\t\t\tcall(MemHandlers[MemType::StoreQueue][size][MemOp::W]);\n'
      '\t\telse\n'
      '\t\t\tcall(MemHandlers[MemType::Slow][size][op]);\n'
      '\t\tnop(19 + length - (getCurr() - start));\n'
      '\t\tready();\n'
      '\t}\n'
      'private:\n'
      '\n'
      '\t// (00.69) A loop\'s branch back taken: the check (emusuni_idle.h).')
patch('core/rec-x64/rec_x64.cpp',
      '\t\tif (codeBuffer == nullptr)\n\t\t\t// init() not called yet\n\t\t\treturn false;\n\t\tu8 *retAddr = *(u8**)context.rsp - 5;\n',
      '\t\tif (codeBuffer == nullptr)\n\t\t\t// init() not called yet\n\t\t\treturn false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t{\n'
      '\t\t\t// EmuSuNi (00.69): an access in the block (BlockCompiler::emusuniGenInlineAccess).\n'
      '\t\t\tu8 *pc = (u8 *)context.pc;\n'
      '\t\t\tu8 *base = (u8 *)codeBuffer->getBase();\n'
      '\t\t\tint size, op, length;\n'
      '\t\t\tif (pc >= base + 19 && pc + 8 <= base + codeBuffer->getSize()\n'
      '\t\t\t\t\t&& (pc < MemHandlerStart || pc >= MemHandlerEnd)\n'
      '\t\t\t\t\t&& BlockCompiler::emusuniFindInlineAccess(pc, size, op, length))\n'
      '\t\t\t{\n'
      '\t\t\t\tu8 *start = pc - 19;\n'
      '\t\t\t\tvirtmem::jit_set_exec(start, 32, false);\n'
      '\t\t\t\tBlockCompiler compiler(*sh4ctx, *codeBuffer, start);\n'
      '\t\t\t\tcompiler.emusuniRewriteInlineAccess(size, op, length, (u32)context.r9);\n'
      '\t\t\t\tvirtmem::jit_set_exec(start, 32, true);\n'
      '\t\t\t\tcontext.pc = (uintptr_t)start;\n'
      '\t\t\t\t// the address, unmasked, back where the handler takes it\n'
      '#ifdef _WIN32\n'
      '\t\t\t\tcontext.rcx = context.r9;\n'
      '#else\n'
      '\t\t\t\tcontext.rdi = context.r9;\n'
      '#endif\n'
      '\t\t\t\treturn true;\n'
      '\t\t\t}\n'
      '\t\t}\n'
      '#endif\n'
      '\t\tu8 *retAddr = *(u8**)context.rsp - 5;\n')

# --- The renderer's time in parts (00.69), for the PS4's log (its 10 s line): Daytona USA's renderer took 14.9 ms a
# frame at 3x (9.6-12 at 1x in 00.67), of it only 2.1 ms the TA lists and textures, its GPU waits 0.01 - the rest wasn't
# told apart. Now: the drawing's commands (Drawer::Draw, the vertex upload with them), the draws made, the picture's
# presenting (its render pass ended and submitted), and the queue's submits with the time waiting for the app's lock of
# the queue (src/Display.cpp: the app's own drawing submits on it too).
patch('core/rend/vulkan/vk_context_lr.h',
      '#include "overlay.h"\n',
      '#include "overlay.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "emusuni_timing.h"\n'
      '#endif\n')
patch('core/rend/vulkan/vk_context_lr.h',
      '\tvoid SubmitCommandBuffers(const std::vector<vk::CommandBuffer> &buffers, vk::Fence fence) {\n'
      '\t\tretro_render_if->lock_queue(retro_render_if->handle);\n'
      '\t\tqueue.submit(vk::SubmitInfo(nullptr, nullptr, buffers, nullptr), fence);\n'
      '\t\tretro_render_if->unlock_queue(retro_render_if->handle);\n',
      '\tvoid SubmitCommandBuffers(const std::vector<vk::CommandBuffer> &buffers, vk::Fence fence) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tconst int64_t emusuniStart = emusuni::nowNs();\n'
      '\t\tretro_render_if->lock_queue(retro_render_if->handle);\n'
      '\t\tconst int64_t emusuniLocked = emusuni::nowNs();\n'
      '\t\tqueue.submit(vk::SubmitInfo(nullptr, nullptr, buffers, nullptr), fence);\n'
      '\t\tretro_render_if->unlock_queue(retro_render_if->handle);\n'
      '\t\temusuni::timing.queueLockNs += emusuniLocked - emusuniStart;\n'
      '\t\temusuni::timing.submitNs += emusuni::nowNs() - emusuniStart;\n'
      '\t\temusuni::timing.submits++;\n'
      '#else\n'
      '\t\tretro_render_if->lock_queue(retro_render_if->handle);\n'
      '\t\tqueue.submit(vk::SubmitInfo(nullptr, nullptr, buffers, nullptr), fence);\n'
      '\t\tretro_render_if->unlock_queue(retro_render_if->handle);\n'
      '#endif\n')
patch('core/rend/vulkan/vulkan_renderer.cpp',
      '#include "rend/transform_matrix.h"\n',
      '#include "rend/transform_matrix.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "emusuni_timing.h"\n'
      '#endif\n')
patch('core/rend/vulkan/vulkan_renderer.cpp',
      '\t\t\tdrawer->setRendContext(rendContext);\n\t\t\tdrawer->Draw(fogTexture.get(), paletteTexture.get());\n',
      '\t\t\tdrawer->setRendContext(rendContext);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\t{\n'
      '\t\t\t\temusuni::ScopeTime emusuniTime(emusuni::timing.drawNs);\n'
      '\t\t\t\tdrawer->Draw(fogTexture.get(), paletteTexture.get());\n'
      '\t\t\t}\n'
      '#else\n'
      '\t\t\tdrawer->Draw(fogTexture.get(), paletteTexture.get());\n'
      '#endif\n')
patch('core/rend/vulkan/vulkan_renderer.cpp',
      '\tbool Present() override\n\t{\n\t\tif (clearLastFrame)\n\t\t\treturn false;\n',
      '\tbool Present() override\n\t{\n\t\tif (clearLastFrame)\n\t\t\treturn false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuni::ScopeTime emusuniTime(emusuni::timing.presentNs);\n'
      '#endif\n')
patch('core/rend/vulkan/drawer.cpp',
      '#include "rend/sorter.h"\n',
      '#include "rend/sorter.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "emusuni_timing.h"\n'
      '#endif\n')
patch('core/rend/vulkan/drawer.cpp',
      '\tvk::Pipeline pipeline = pipelineManager->GetPipeline(listType, sortTriangles, poly, gpuPalette, dithering);\n'
      '\tcmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);\n',
      '#ifdef EMUSUNI_PORT\n'
      '\temusuni::timing.drawCalls++;\n'
      '#endif\n'
      '\tvk::Pipeline pipeline = pipelineManager->GetPipeline(listType, sortTriangles, poly, gpuPalette, dithering);\n'
      '\tcmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);\n')

# --- One descriptor set per texture and sampler a frame (00.69). Each textured polygon got a descriptor set of its own
# every frame (allocated, written with vkUpdateDescriptorSets, its writes in a vector, kept in a map: two heap
# allocations each), though a frame's polygons share far fewer textures (Daytona USA's race: 1,700 draws a frame). The
# set says only which texture and sampler (but NAOMI 2's, which also say the polygon's uniforms: as before), so polygons
# with the same pair share it now, and a set already bound isn't bound again; the same descriptors for every draw, the
# same picture. Per frame as before (the sets go back with the frame's slot of the ring).
patch('core/rend/vulkan/pipeline.h',
      '\tvoid bindPerPolyDescriptorSets(vk::CommandBuffer cmdBuffer, const PolyParam& poly, int polyNumber, vk::Buffer buffer,\n'
      '\t\t\tvk::DeviceSize uniformOffset, vk::DeviceSize lightOffset, bool punchThrough)\n'
      '\t{\n',
      '\tvoid bindPerPolyDescriptorSets(vk::CommandBuffer cmdBuffer, const PolyParam& poly, int polyNumber, vk::Buffer buffer,\n'
      '\t\t\tvk::DeviceSize uniformOffset, vk::DeviceSize lightOffset, bool punchThrough)\n'
      '\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tif (!poly.isNaomi2())\n'
      '\t\t{\n'
      '\t\t\t// EmuSuNi (00.69): the set of this texture and sampler this frame, made by the first polygon with them.\n'
      '\t\t\tvk::Sampler sampler;\n'
      '\t\t\tvk::ImageView view;\n'
      '\t\t\tif (poly.texture != nullptr)\n'
      '\t\t\t{\n'
      '\t\t\t\tsampler = samplerManager->GetSampler(poly, punchThrough);\n'
      '\t\t\t\tview = ((Texture *)poly.texture)->GetReadOnlyImageView();\n'
      '\t\t\t}\n'
      '\t\t\tconst EmusuniTexKey key { (u64)(VkSampler)sampler, (u64)(VkImageView)view, poly.texture != nullptr };\n'
      '\t\t\tvk::DescriptorSet set;\n'
      '\t\t\tauto it = emusuniTexSets.find(key);\n'
      '\t\t\tif (it == emusuniTexSets.end())\n'
      '\t\t\t{\n'
      '\t\t\t\tset = perPolyAlloc.alloc();\n'
      '\t\t\t\tif (poly.texture != nullptr)\n'
      '\t\t\t\t{\n'
      '\t\t\t\t\tconst vk::DescriptorImageInfo imageInfo(sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);\n'
      '\t\t\t\t\tconst vk::WriteDescriptorSet write(set, 0, 0, vk::DescriptorType::eCombinedImageSampler, imageInfo);\n'
      '\t\t\t\t\tgetContext()->GetDevice().updateDescriptorSets(write, nullptr);\n'
      '\t\t\t\t}\n'
      '\t\t\t\temusuniTexSets.emplace(key, set);\n'
      '\t\t\t}\n'
      '\t\t\telse\n'
      '\t\t\t\tset = it->second;\n'
      '\t\t\tif (set != emusuniBound)\n'
      '\t\t\t{\n'
      '\t\t\t\tcmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 1, set, nullptr);\n'
      '\t\t\t\temusuniBound = set;\n'
      '\t\t\t}\n'
      '\t\t\treturn;\n'
      '\t\t}\n'
      '\t\temusuniBound = vk::DescriptorSet{};  // (NAOMI 2\'s: its own set, bound below)\n'
      '#endif\n')
patch('core/rend/vulkan/pipeline.h',
      '\tvoid bindPerFrameDescriptorSets(vk::CommandBuffer cmdBuffer)\n\t{\n',
      '\tvoid bindPerFrameDescriptorSets(vk::CommandBuffer cmdBuffer)\n\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuniBound = vk::DescriptorSet{};  // (a new command buffer or render: nothing bound in it yet)\n'
      '#endif\n')
patch('core/rend/vulkan/pipeline.h',
      '\t\tperFrameDescSet = vk::DescriptorSet{};\n\t\tperPolyDescSets.clear();\n\t}\n',
      '\t\tperFrameDescSet = vk::DescriptorSet{};\n\t\tperPolyDescSets.clear();\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuniTexSets.clear();\n'
      '\t\temusuniBound = vk::DescriptorSet{};\n'
      '#endif\n'
      '\t}\n')
patch('core/rend/vulkan/pipeline.h',
      '\tstd::unordered_map<const void *, vk::DescriptorSet> perPolyDescSets;\n',
      '\tstd::unordered_map<const void *, vk::DescriptorSet> perPolyDescSets;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi (00.69): this frame\'s sets by texture and sampler, and the set last bound (bindPerPolyDescriptorSets).\n'
      '\tstruct EmusuniTexKey\n'
      '\t{\n'
      '\t\tu64 sampler, view;\n'
      '\t\tbool textured;\n'
      '\t\tbool operator==(const EmusuniTexKey& other) const {\n'
      '\t\t\treturn sampler == other.sampler && view == other.view && textured == other.textured;\n'
      '\t\t}\n'
      '\t};\n'
      '\tstruct EmusuniTexHash\n'
      '\t{\n'
      '\t\tsize_t operator()(const EmusuniTexKey& key) const {\n'
      '\t\t\treturn (size_t)(key.view * 0x9E3779B97F4A7C15ull ^ (key.sampler + 0x632BE59BD9B4E019ull) ^ key.textured);\n'
      '\t\t}\n'
      '\t};\n'
      '\tstd::unordered_map<EmusuniTexKey, vk::DescriptorSet, EmusuniTexHash> emusuniTexSets;\n'
      '\tvk::DescriptorSet emusuniBound;\n'
      '#endif\n')
patch('core/rend/vulkan/pipeline.h',
      '\t\t\tgetContext()->GetDevice().updateDescriptorSets(writeDescriptorSet, nullptr);\n'
      '\t\t\tperPolyDescSets[&mvParam] = perPolyDescSet;\n'
      '\t\t}\n'
      '\t\telse\n'
      '\t\t\tperPolyDescSet = it->second;\n'
      '\t\tcmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 1, perPolyDescSet, nullptr);\n',
      '\t\t\tgetContext()->GetDevice().updateDescriptorSets(writeDescriptorSet, nullptr);\n'
      '\t\t\tperPolyDescSets[&mvParam] = perPolyDescSet;\n'
      '\t\t}\n'
      '\t\telse\n'
      '\t\t\tperPolyDescSet = it->second;\n'
      '\t\tcmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 1, perPolyDescSet, nullptr);\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuniBound = vk::DescriptorSet{};  // (another set bound: the next polygon binds its own)\n'
      '#endif\n')

# --- Hot pages, for the PS4's page faults (Resonance's too): a fault costs the PS4 a signal, about a millisecond
# with the protection changes, and its pages are 16 KiB: a page a game keeps writing (a texture it animates,
# data next to textures) can fault hundreds of times a second. A VRAM page that faults more than 6 times in a
# second stops being protected for 10 s; the textures on it are checked by hash when used instead (once a frame),
# and protected again when it has cooled down. EMUSUNI_DC_HOTPAGES=0 (or the file /data/EmuSuNi/dc-hotpages-off)
# switches it off.
patch('core/rend/TexCache.cpp',
      'static std::mutex vramlist_lock;\n',
      'static std::mutex vramlist_lock;\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "emusuni_options.h"\n'
      '#include <atomic>\n'
      'namespace\n'
      '{\n'
      'constexpr u32 kHotFaults = 6;\n'
      'constexpr u64 kHotMs = 10000;\n'
      'constexpr size_t kMaxPages = VRAM_SIZE_MAX / PAGE_SIZE;\n'
      'std::atomic<u32> pageFaults[kMaxPages];\n'
      'std::atomic<u64> pageWindow[kMaxPages];\n'
      'std::atomic<u64> pageHotUntil[kMaxPages];\n'
      '\n'
      'bool hotPagesOn()\n'
      '{\n'
      '\tstatic const bool enabled = []() {\n'
      '\t\tchar how[96];\n'
      '\t\tconst bool on = emusuni::dcOption("HOTPAGES", how, sizeof(how));\n'
      '\t\tNOTICE_LOG(RENDERER, "EmuSuNi: hot VRAM pages %s%s%s", on ? "on" : "off", how[0] != \'\\0\' ? ": " : "", how);\n'
      '\t\treturn on;\n'
      '\t}();\n'
      '\treturn enabled;\n'
      '}\n'
      '\n'
      'void notePageFault(size_t page)\n'
      '{\n'
      '\tif (page >= kMaxPages || !hotPagesOn())\n'
      '\t\treturn;\n'
      '\tconst u64 now = getTimeMs();\n'
      '\tif (now - pageWindow[page] > 1000)\n'
      '\t{\n'
      '\t\tpageWindow[page] = now;\n'
      '\t\tpageFaults[page] = 0;\n'
      '\t}\n'
      '\tif (++pageFaults[page] >= kHotFaults && pageHotUntil[page] < now)\n'
      '\t{\n'
      '\t\tpageHotUntil[page] = now + kHotMs;\n'
      '\t\tstatic int said = 0;\n'
      '\t\tif (said++ < 8)\n'
      '\t\t\tNOTICE_LOG(RENDERER, "EmuSuNi: VRAM page %x is hot: its textures are hash-checked for %d s", (u32)(page * PAGE_SIZE), (int)(kHotMs / 1000));\n'
      '\t}\n'
      '}\n'
      '\n'
      'bool rangeHot(u32 start, u32 end)\n'
      '{\n'
      '\tif (!hotPagesOn())\n'
      '\t\treturn false;\n'
      '\tconst u64 now = getTimeMs();\n'
      '\tfor (size_t page = start / PAGE_SIZE; page <= end / PAGE_SIZE && page < kMaxPages; page++)\n'
      '\t\tif (pageHotUntil[page] > now)\n'
      '\t\t\treturn true;\n'
      '\treturn false;\n'
      '}\n'
      '}  // namespace\n'
      '#endif\n')
patch('core/rend/TexCache.cpp',
      '\tsize_t addr_hash = offset / PAGE_SIZE;\n',
      '\tsize_t addr_hash = offset / PAGE_SIZE;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tnotePageFault(addr_hash);\n'
      '#endif\n')
patch('core/rend/TexCache.cpp',
      '//true if : dirty or paletted texture and hashes don\'t match\nbool BaseTextureCacheData::NeedsUpdate() {\n\tbool rc = dirty != 0;\n',
      '#ifdef EMUSUNI_PORT\n'
      '// The hash of the texture\'s data in VRAM now (hot pages), inside the VRAM whatever the texture says.\n'
      'u32 BaseTextureCacheData::currentDataHash() const\n'
      '{\n'
      '\tconst u32 start = std::min<u32>(mmStartAddress, VRAM_SIZE);\n'
      '\tu32 h = XXH32(&vram[start], std::min<u32>(size, VRAM_SIZE - start), 11);\n'
      '\tif (tcw.VQ_Comp)\n'
      '\t\th ^= XXH32(&vram[std::min<u32>(startAddress, VRAM_SIZE - VQ_CODEBOOK_SIZE)], VQ_CODEBOOK_SIZE, 13);\n'
      '\treturn h;\n'
      '}\n\n'
      '#endif\n'
      '//true if : dirty or paletted texture and hashes don\'t match\nbool BaseTextureCacheData::NeedsUpdate() {\n\tbool rc = dirty != 0;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tif (hashWatched && !rc && lock_block == nullptr && watchFrame != FrameCount)\n'
      '\t{\n'
      '\t\t// On a hot page: the data checked once a frame instead of a write protection.\n'
      '\t\twatchFrame = FrameCount;\n'
      '\t\tif (currentDataHash() != watchHash)\n'
      '\t\t\trc = true;\n'
      '\t\telse if (!rangeHot(startAddress, std::min<u32>(mmStartAddress + size - 1, VRAM_SIZE - 1)))\n'
      '\t\t\tprotectVRam();  // the page has cooled down: protected again\n'
      '\t}\n'
      '#endif\n')
patch('core/rend/TexCache.cpp',
      '\tif (startAddress > end)\n\t{\n\t\tWARN_LOG(PVR, "vramlock_Lock: startAddress > end. Tried to lock negative block");\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tif (rangeHot(startAddress, end))\n'
      '\t{\n'
      '\t\t// A hot page: not protected, checked by its hash when used (NeedsUpdate).\n'
      '\t\thashWatched = true;\n'
      '\t\twatchHash = currentDataHash();\n'
      '\t\twatchFrame = FrameCount;\n'
      '\t\treturn;\n'
      '\t}\n'
      '\thashWatched = false;\n'
      '#endif\n'
      '\tif (startAddress > end)\n\t{\n\t\tWARN_LOG(PVR, "vramlock_Lock: startAddress > end. Tried to lock negative block");\n')
patch('core/rend/TexCache.h',
      '\t\tgpuPalette = other.gpuPalette;\n\t\tarea = other.area;\n',
      '\t\tgpuPalette = other.gpuPalette;\n\t\tarea = other.area;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\thashWatched = other.hashWatched;\n'
      '\t\twatchHash = other.watchHash;\n'
      '\t\twatchFrame = other.watchFrame;\n'
      '#endif\n')
patch('core/rend/TexCache.h',
      '\tu32 dirty;\t\t\t// frame number at which texture was overwritten\n\tvram_block* lock_block;\n',
      '\tu32 dirty;\t\t\t// frame number at which texture was overwritten\n\tvram_block* lock_block;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t// EmuSuNi: on a hot VRAM page the texture isn\'t write-protected but checked by its hash when used (TexCache.cpp)\n'
      '\tbool hashWatched = false;\n'
      '\tu32 watchHash = 0;\n'
      '\tu32 watchFrame = 0;\n'
      '\tu32 currentDataHash() const;\n'
      '#endif\n')

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
	// 00.66, the emulation thread's parts and the renderer's (the PS4 can't be profiled: these tell where the time goes):
	std::atomic<int64_t> soundNs { 0 };       // the sound chip: the ARM7 and the AICA's channels and DSP (AicaUpdate)
	std::atomic<int64_t> discNs { 0 };        // the disc's reads (libGDR_ReadSector: GD-ROM sectors, CD audio)
	std::atomic<int> discReads { 0 };
	std::atomic<int64_t> discMaxNs { 0 };     // the longest read since the last line
	std::atomic<int64_t> processNs { 0 };     // the renderer's TA lists to polygons and its textures (Renderer::Process)
	// 00.69: the SH4's waits in a loop let go by (hw/sh4/interpr/sh4_interpreter.cpp: emusuniIdleCheck), the console's frames
	std::atomic<int64_t> idleCycles { 0 };    // SH4 cycles not run: the loop would only have come round to the same state
	std::atomic<int> idleSkips { 0 };         // the times it was let go by (until the next event or interrupt)
	std::atomic<int> vblanks { 0 };           // the console's frames (Emulator::vblank)
	std::atomic<int64_t> paceNs { 0 };        // the emulation thread asleep, ahead of the app's pace (emulator.cpp)
	// 00.69, the renderer's parts (rend/vulkan): its commands, its presenting, the queue's submits and the lock's waits
	std::atomic<int64_t> drawNs { 0 };        // Drawer::Draw: the vertex upload and the draws' commands
	std::atomic<int> drawCalls { 0 };         // the draws (Drawer::DrawPoly)
	std::atomic<int64_t> presentNs { 0 };     // Renderer::Present: the render pass ended, submitted, the picture handed over
	std::atomic<int64_t> submitNs { 0 };      // the queue's submits, with...
	std::atomic<int64_t> queueLockNs { 0 };   // ...the waits for the app's lock of the queue
	std::atomic<int> submits { 0 };
	// 00.81: display lists with a parameter the TA doesn't know, the rest of each left out (hw/pvr/ta_vtx.cpp)
	std::atomic<int> taUnknown { 0 };
	// The fault handler's (linux/common.cpp), by kind, and the page locks (linux/posix_vmem.cpp):
	std::atomic<int> faultsCode { 0 };        // a write to a page of compiled code (the blocks of it dropped)
	std::atomic<int> faultsVram { 0 };        // a write to a texture's VRAM (the texture dropped)
	std::atomic<int> faultsTable { 0 };       // the SH4 jump table's piece not mapped yet (fast memory)
	std::atomic<int> faultsRewrite { 0 };     // a recompiled access to the hardware's addresses, rewritten (fast memory)
	std::atomic<int> faultsOther { 0 };       // not the core's: passed on (the crash log)
	std::atomic<int> protects { 0 };          // pages locked (mprotect calls)...
	std::atomic<int> unprotects { 0 };        // ...and unlocked
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

// The time of a scope called very often (the sound chip's, 44,100 times a second), measured in one call of 16 and
// counted 16 times: a sixteenth of the clock reads.
struct SampledTime
{
	std::atomic<int64_t>& to;
	int64_t start = 0;
	SampledTime(std::atomic<int64_t>& to, unsigned& calls) : to(to)
	{
		if ((++calls & 15) == 0)
			start = nowNs();
	}
	~SampledTime()
	{
		if (start != 0)
			to += (nowNs() - start) * 16;
	}
};

// A read of the disc (libGDR_ReadSector): its time, the count and the longest (a slow USB stick shows there).
struct DiscTime
{
	const int64_t start = nowNs();
	~DiscTime()
	{
		const int64_t took = nowNs() - start;
		timing.discNs += took;
		timing.discReads++;
		int64_t most = timing.discMaxNs.load();
		while (took > most && !timing.discMaxNs.compare_exchange_weak(most, took))
			;
	}
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
      '\temusuni::timing.vblanks++;\n'
      '\temusuniPace();\n'
      '#endif\n')
# (00.69) The console's speed kept to the app's while no picture holds it back. With threaded rendering the emulation
# thread waits only for the renderer (a frame ahead at most); a game that draws nothing for a while (a loading screen,
# a menu between its pictures, the arcade boards' BIOS checking the cartridge) ran as fast as the CPU could, its sound
# too: Daytona USA's menus on the PS4 at 107-113% of the sound's rate, which the app's audio cut down; NAOMI and
# Atomiswave boots 4-8 times faster than real on the PC, 1.3 s of sound a run dropped. The app gives the frames per
# second it runs the console at (flycast_emusuni_pace: its speed, fast forward's x2-x8; 0 none); at each of the
# console's frames the emulation thread sleeps while it is more than 3 frames ahead of that pace, never longer than
# 100 ms at a time; 6 frames behind, the pace starts again from there (no catching up: the app's own pacing does).
patch('core/emulator.cpp',
      'void Emulator::vblank()\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '#include <thread>\n'
      'static std::atomic<double> emusuniPaceFps { 0.0 };\n'
      '// EmuSuNi (src/Emulator.cpp): the console\'s frames per second the app runs it at, 0 for no limit.\n'
      'extern "C" void flycast_emusuni_pace(double fps)\n'
      '{\n'
      '\temusuniPaceFps.store(fps, std::memory_order_relaxed);\n'
      '}\n'
      '\n'
      '// At each of the console\'s frames (the emulation thread): asleep while more than 3 frames ahead of the app\'s pace.\n'
      'static void emusuniPace()\n'
      '{\n'
      '\tstatic int64_t start = 0;\n'
      '\tstatic double startFps = 0.0;\n'
      '\tstatic u64 frames = 0;\n'
      '\tconst double fps = emusuniPaceFps.load(std::memory_order_relaxed);\n'
      '\tif (fps <= 0.0 || !config::ThreadedRendering)\n'
      '\t{\n'
      '\t\tstart = 0;\n'
      '\t\treturn;\n'
      '\t}\n'
      '\tconst int64_t now = emusuni::nowNs();\n'
      '\tconst double frameNs = 1e9 / fps;\n'
      '\tif (start == 0 || fps != startFps)\n'
      '\t{\n'
      '\t\tstart = now;\n'
      '\t\tstartFps = fps;\n'
      '\t\tframes = 0;\n'
      '\t\treturn;\n'
      '\t}\n'
      '\tframes++;\n'
      '\tconst double due = start + frames * frameNs;  // when this frame is due at the app\'s pace\n'
      '\tif (now - due > 6 * frameNs)\n'
      '\t{\n'
      '\t\tstart = now;  // behind: the pace from here on\n'
      '\t\tframes = 0;\n'
      '\t\treturn;\n'
      '\t}\n'
      '\tconst double ahead = due - now - 3 * frameNs;\n'
      '\tif (ahead > 0.0)\n'
      '\t{\n'
      '\t\temusuni::ScopeTime emusuniTime(emusuni::timing.paceNs);\n'
      '\t\tstd::this_thread::sleep_for(std::chrono::nanoseconds((int64_t)std::min(ahead, 100e6)));\n'
      '\t}\n'
      '}\n'
      '#endif\n\n'
      'void Emulator::vblank()\n{\n')
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
# (00.66: the parts of the emulation thread and of the rendering the PS4 log can't get otherwise: the sound chip
# (timed in one of every 16 of its 44,100 calls a second), the disc's reads, the TA lists' conversion with the
# textures.)
patch('core/hw/pvr/Renderer_if.cpp',
      '\t\t\tFC_PROFILE_SCOPE_NAMED("Renderer::Process");\n',
      '\t\t\tFC_PROFILE_SCOPE_NAMED("Renderer::Process");\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\temusuni::ScopeTime emusuniProcess(emusuni::timing.processNs);\n'
      '#endif\n')
# The emulation's two other waits for the renderer, counted with the first (the queue full): a frame's start while
# the renderer still has the frame before (QueueRender, without auto frame skip) and the end of a frame's rendering
# while the renderer is still converting its TA lists (rend_end_render): the time the renderer holds the emulation.
patch('core/hw/pvr/ta_ctx.cpp',
      '#include "stdclass.h"\n',
      '#include "stdclass.h"\n#include "emusuni_timing.h"\n')
patch('core/hw/pvr/ta_ctx.cpp',
      '\t\t\t// fast enough over the last frames\n\t\t\tframe_finished.Wait();\n',
      '\t\t\t// fast enough over the last frames\n'
      '\t\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\temusuni::ScopeTime emusuniTime(emusuni::timing.emuWaitNs);\n'
      '#endif\n'
      '\t\t\tframe_finished.Wait();\n'
      '\t\t}\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\tif (pend_rend && config::ThreadedRendering)\n\t\trenderEnd.Wait();\n',
      '\tif (pend_rend && config::ThreadedRendering)\n'
      '\t{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuni::ScopeTime emusuniTime(emusuni::timing.emuWaitNs);\n'
      '#endif\n'
      '\t\trenderEnd.Wait();\n'
      '\t}\n')
patch('core/hw/aica/aica.cpp',
      '#include "hw/arm7/arm_mem.h"\n',
      '#include "hw/arm7/arm_mem.h"\n#include "emusuni_timing.h"\n')
patch('core/hw/aica/aica.cpp',
      'static int AicaUpdate(int tag, int cycles, int jitter, void *arg)\n{\n\tarm::run(1);\n',
      'static int AicaUpdate(int tag, int cycles, int jitter, void *arg)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tstatic unsigned emusuniCalls;\n'
      '\temusuni::SampledTime emusuniTime(emusuni::timing.soundNs, emusuniCalls);\n'
      '#endif\n'
      '\tarm::run(1);\n')
patch('core/imgread/common.cpp',
      'u32 libGDR_ReadSector(u8 *buff, u32 startSector, u32 sectorCount, u32 sectorSize, bool stopOnMiss)\n{\n',
      '#ifdef EMUSUNI_PORT\n#include "emusuni_timing.h"\n#endif\n\n'
      'u32 libGDR_ReadSector(u8 *buff, u32 startSector, u32 sectorCount, u32 sectorSize, bool stopOnMiss)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuni::DiscTime emusuniTime;\n'
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
      '\tstruct Counters { int64_t emuRun, emuWait, render, renderWait, pipeline, shader, gpuWait; int pipelines, shaders, compiled, idleWaits;\n'
      '\t\tint faultsCode, faultsVram, faultsTable, faultsRewrite, faultsOther, protects, unprotects; int64_t sound, disc, process; int discReads; };\n'
      '\tauto read = []() {\n'
      '\t\tconst emusuni::Timing& t = emusuni::timing;\n'
      '\t\treturn Counters { t.emuRunNs.load(), t.emuWaitNs.load(), t.renderNs.load(), t.renderWaitNs.load(), t.pipelineNs.load(),\n'
      '\t\t\tt.shaderNs.load(), t.gpuWaitNs.load(), t.pipelines.load(), t.shaders.load(), t.shadersCompiled.load(), t.idleWaits.load(),\n'
      '\t\t\tt.faultsCode.load(), t.faultsVram.load(), t.faultsTable.load(), t.faultsRewrite.load(), t.faultsOther.load(),\n'
      '\t\t\tt.protects.load(), t.unprotects.load(), t.soundNs.load(), t.discNs.load(), t.processNs.load(), t.discReads.load() };\n'
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
      '\tconst int64_t discMost = emusuni::timing.discMaxNs.exchange(0);  // (the longest read of this window)\n'
      '\tNOTICE_LOG(COMMON, "EmuSuNi: dc per frame (%d runs, %.1f pictures/s, %s, SH4 %s, %s): retro_run %.2f ms, of it rendering %.2f "\n'
      '\t\t"(TA lists and textures %.2f) and waiting for the emulation %.2f; emulation thread busy %.2f ms (the sound chip %.2f of it; waiting for "\n'
      '\t\t"the renderer %.2f); disc reads %d (%.2f ms a frame, the longest %.1f ms); in the rendering: new pipelines %d "\n'
      '\t\t"(%.1f ms), shaders %d of them compiled %d (%.1f ms), GPU waits %.2f ms (%d full syncs); faults: code %d, VRAM %d, table %d, "\n'
      '\t\t"hardware %d, other %d; mprotect %d+%d",\n'
      '\t\truns, pictures / ((now - windowStart) / 1e9), threaded ? "threaded" : "one thread", emusuniSh4JitOk ? "recompiler" : "interpreter",\n'
      '\t\taddrspace::virtmemEnabled() ? "fast memory" : "memory handlers",\n'
      '\t\trunNs / ms, (c.render - before.render) / ms, (c.process - before.process) / ms, (c.renderWait - before.renderWait) / ms,\n'
      '\t\temulation, (c.sound - before.sound) / ms, (c.emuWait - before.emuWait) / ms, c.discReads - before.discReads,\n'
      '\t\t(c.disc - before.disc) / ms, discMost / 1e6, c.pipelines - before.pipelines, (c.pipeline - before.pipeline) / 1e6,\n'
      '\t\tc.shaders - before.shaders, c.compiled - before.compiled, (c.shader - before.shader) / 1e6,\n'
      '\t\t(c.gpuWait - before.gpuWait) / ms, c.idleWaits - before.idleWaits,\n'
      '\t\tc.faultsCode - before.faultsCode, c.faultsVram - before.faultsVram, c.faultsTable - before.faultsTable,\n'
      '\t\tc.faultsRewrite - before.faultsRewrite, c.faultsOther - before.faultsOther,\n'
      '\t\tc.protects - before.protects, c.unprotects - before.unprotects);\n'
      '\t// (00.69) the SH4 waiting in loops let go by (hw/sh4/interpr/sh4_interpreter.cpp): its share of the console\'s time\n'
      '\tconst int frames = emusuni::timing.vblanks.exchange(0);\n'
      '\tconst int64_t idleCycles = emusuni::timing.idleCycles.exchange(0);\n'
      '\tconst int idleSkips = emusuni::timing.idleSkips.exchange(0);\n'
      '\tconst int64_t paceNs = emusuni::timing.paceNs.exchange(0);\n'
      '\tif (frames > 0)\n'
      '\t\tNOTICE_LOG(COMMON, "EmuSuNi: dc: the SH4 waited in a loop %.1f%% of the console\'s time, let go by to the next event "\n'
      '\t\t\t"%d times (%d frames); the emulation held back to the app\'s pace %.2f ms a frame", 100.0 * idleCycles\n'
      '\t\t\t/ (frames * (SH4_MAIN_CLOCK / 59.94)), idleSkips, frames, paceNs / 1e6 / frames);\n'
      '\t// (00.69) the renderer\'s parts, per run as the line above\n'
      '\tconst int64_t drawNs = emusuni::timing.drawNs.exchange(0), presentNs = emusuni::timing.presentNs.exchange(0);\n'
      '\tconst int64_t submitNs = emusuni::timing.submitNs.exchange(0), lockNs = emusuni::timing.queueLockNs.exchange(0);\n'
      '\tconst int draws = emusuni::timing.drawCalls.exchange(0), submits = emusuni::timing.submits.exchange(0);\n'
      '\tNOTICE_LOG(COMMON, "EmuSuNi: dc renderer per frame: drawing %.2f ms (%.0f draws), presenting %.2f ms, queue submits %.1f "\n'
      '\t\t"(%.2f ms, of it waiting for the app\'s lock of the queue %.2f ms)", drawNs / ms, draws / (double)runs, presentNs / ms,\n'
      '\t\tsubmits / (double)runs, submitNs / ms, lockNs / ms);\n'
      '\t// (00.81) display lists cut short (a parameter the TA doesn\'t know), when there were any\n'
      '\tif (const int taUnknown = emusuni::timing.taUnknown.exchange(0))\n'
      '\t\tNOTICE_LOG(COMMON, "EmuSuNi: dc: %d display lists with a parameter the TA doesn\'t know, the rest of each "\n'
      '\t\t\t"left out", taUnknown);\n'
      '\twindowStart = now;\n'
      '\trunNs = runs = pictures = 0;\n'
      '\tbefore = c;\n'
      '}\n\n'
      'static void emusuniFlushDeferred()\n{\n')

# --- The console's frames per picture, for the app's pacing (00.68): a run of the core (retro_run) is one picture,
# and a game at 30 FPS (Jet Set Radio, all of it), Frame Skip 1 or 2, or Auto frame skip once it skips make it 2 to 4
# of the console's frames. The app paces Flycast by them (src/Emulator.cpp, flycast_emusuni_vblanks): every message of
# the renderer's queue carries the console's frame count of when the emulation sent it, and a picture presented
# records its own. The difference between two pictures is then exact, whatever the threads' timing (00.67 used
# Flycast's detect_vsync_swap_interval instead, which Flycast turns off with Auto frame skip: Jet Set Radio on the
# owner's PS4 ran at up to twice its speed again).
patch('core/hw/pvr/Renderer_if.cpp',
      'class PvrMessageQueue\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the console\'s frames (rend_vblank, the emulation thread) and the count of the last picture presented.\n'
      'static std::atomic<u64> emusuniVblanks { 0 };\n'
      'static std::atomic<u64> emusuniPresentedVblanks { 0 };\n'
      '#endif\n\n'
      'class PvrMessageQueue\n{\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\t\tMessageType type = NoMessage;\n\t\tFramebufferInfo config;\n\t};\n',
      '\t\tMessageType type = NoMessage;\n\t\tFramebufferInfo config;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tu64 emusuniVblank = 0;  // the console\'s frames when it was sent\n'
      '#endif\n'
      '\t};\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\t\tMessage msg { type, config };\n',
      '\t\tMessage msg { type, config };\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\tmsg.emusuniVblank = emusuniVblanks.load(std::memory_order_relaxed);\n'
      '#endif\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\t\tswitch (msg.type)\n',
      '#ifdef EMUSUNI_PORT\n'
      '\t\temusuniCurrent = msg.emusuniVblank;  // (render() presents too: its message\'s count)\n'
      '#endif\n'
      '\t\tswitch (msg.type)\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\t\t\tpresented = true;\n',
      '\t\t\tpresented = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\t\t\temusuniPresentedVblanks.store(emusuniCurrent, std::memory_order_relaxed);\n'
      '#endif\n')
patch('core/hw/pvr/Renderer_if.cpp',
      '\tstd::deque<Message> queue;\n};\n',
      '\tstd::deque<Message> queue;\n'
      '#ifdef EMUSUNI_PORT\n'
      '\tu64 emusuniCurrent = 0;  // the message being executed\'s count\n'
      '#endif\n'
      '};\n')
patch('core/hw/pvr/Renderer_if.cpp',
      'void rend_vblank()\n{\n',
      'void rend_vblank()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '\temusuniVblanks.fetch_add(1, std::memory_order_relaxed);\n'
      '#endif\n')
patch('core/hw/pvr/Renderer_if.cpp',
      'void check_framebuffer_write()\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (src/Emulator.cpp): the console\'s frame count of the last picture presented (retro_run\'s thread).\n'
      'extern "C" unsigned long long flycast_emusuni_vblanks(void)\n'
      '{\n'
      '\treturn emusuniPresentedVblanks.load(std::memory_order_relaxed);\n'
      '}\n'
      '#endif\n\n'
      'void check_framebuffer_write()\n{\n')

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
# PS4's CPU, the core options' translations left out (HAVE_NO_LANGEXTRA: 88 tables of the options in
# other languages, 9.4 MB of the program image; the app doesn't ask for a language and sets the
# options itself), a section for each function and variable (the app's link, --gc-sections, drops the
# ones nothing uses), unwind tables only for the functions that can throw (not the toolchain's
# -funwind-tables, a frame for every function) and the renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -DHAVE_NO_LANGEXTRA -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/prefix_flycast.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have, and the PS4 calls of
# linux/emusuni_vmem.cpp done by stand-ins on Linux: EMUSUNI_DC_HOST.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a -DEMUSUNI_DC_HOST=1"
# PGO (profile-guided optimisation, 00.66): scripts/dc-core.profdata.xz, a profile of the PC build playing
# Crazy Taxi, Marvel vs. Capcom 2, Daytona USA and Jet Set Radio (races, a fight, the streets) and their
# starts, used by both builds when it's there (DC_CORE_PGO=off: without it). Only the C++ is optimised by it
# (the sound chip's channels, the scheduler, the TA and the renderer...; the recompilers' code isn't C++). A
# profile older than the sources only optimises less (the functions that changed get none; the log counts
# them). A new one: DC_CORE_HOST=1 DC_CORE_PGO=generate build-dc-core.sh <dir> (instrumented), the PC test
# frontend linked with -fprofile-instr-generate and run on the games with LLVM_PROFILE_FILE=<dir>/%p.profraw,
# then llvm-profdata-21 merge -o dc-core.profdata <dir>/*.profraw; xz -9 it into scripts/.
PROFILE="${DC_CORE_PROFILE:-$HERE/dc-core.profdata.xz}"
PGO="none"
case "${DC_CORE_PGO:-use}" in
  generate)
    if [ "$HOST" != 1 ]; then
      echo "Flycast: DC_CORE_PGO=generate is for the PC build (DC_CORE_HOST=1)"
      exit 1
    fi
    FLAGS_CORE+=" -fprofile-instr-generate"
    PGO="generate" ;;
  off) ;;
  *)
    if [ -f "$PROFILE" ]; then
      xz -dc "$PROFILE" > "$OUT/dc-core.profdata.new"
      if cmp -s "$OUT/dc-core.profdata.new" "$OUT/dc-core.profdata"; then
        rm -f "$OUT/dc-core.profdata.new"
      else
        mv "$OUT/dc-core.profdata.new" "$OUT/dc-core.profdata"
      fi
      FLAGS_CORE+=" -fprofile-instr-use=$OUT/dc-core.profdata -Wno-profile-instr-unprofiled"
      PGO="profile $(sha256sum "$PROFILE" | cut -c1-16)"
    fi ;;
esac
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
WANT="$SRC | $COMMITS | host $HOST | pgo $PGO | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
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
if [ "${PGO%% *}" = profile ]; then
  # (clang's -Wprofile-instr-out-of-date, one line per file: "of N functions, M have mismatched data")
  STALE=$( { grep -oE "of [0-9]+ functions, [0-9]+ ha(s|ve) mismatched data" "$LOG" || true; } |
           awk '{n += $2; m += $4} END {print m + 0, "of", n + 0}')
  echo "Flycast: PGO with $(basename "$PROFILE") (functions it doesn't match in the files compiled now: $STALE in the files with any)"
fi

# One object: only the libretro API (renamed) stays global. Every library the core target was made
# of (Flycast's own, glslang, libchdr, zstd, zlib, libzip, xxHash, nowide, tinygettext, libelf and the
# embedded resources). libchdr's LZMA SDK (24.05) only for what the rest still needs: the core's own
# copy (core/deps/lzma, for 7z archives) has the same functions, and upstream's link takes those.
# Replaced only when it changed (the app isn't linked again for nothing).
mapfile -t ARCHIVES < <(find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -not -name 'liblzma.a' | sort)
# (00.81: --wrap - the core's calls of __cxa_throw go to oslib/emusuni_throw.cpp's __wrap___cxa_throw, which logs the
# exception and calls the real one.)
ld.lld-21 -r --wrap=__cxa_throw --whole-archive "${ARCHIVES[@]}" --no-whole-archive \
  "$BUILD/core/deps/libchdr/deps/lzma-24.05/liblzma.a" -o "$OUT/dc_core_all.o"
# (And the app's own helpers: the arcade boards' games, flycast_emusuni_arcade_list in hw/naomi/naomi_roms.cpp;
# what would make a board's start fail, flycast_emusuni_check_set in hw/naomi/naomi_cart.cpp; the console's frames
# of the last picture, flycast_emusuni_vblanks in hw/pvr/Renderer_if.cpp; the app's pace for the emulation,
# flycast_emusuni_pace in emulator.cpp.)
# (00.81) The throws' logger linked in.
if ! llvm-nm-21 --defined-only "$OUT/dc_core_all.o" | grep " T __wrap___cxa_throw$" > /dev/null; then
  echo "Flycast: the object has no __wrap___cxa_throw (oslib/emusuni_throw.cpp): its throws wouldn't be logged"
  exit 1
fi
HELPERS="flycast_emusuni_arcade_list flycast_emusuni_check_set flycast_emusuni_pace flycast_emusuni_vblanks"
llvm-objcopy-21 --wildcard --keep-global-symbol='flycast_retro_*' --keep-global-symbol='flycast_emusuni_*' \
  "$OUT/dc_core_all.o" "$OUT/dc_core.o.new"
rm -f "$OUT/dc_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/dc_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$( { for f in $API; do echo "flycast_$f"; done; for f in $HELPERS; do echo "$f"; done; } | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "Flycast: the object's global symbols aren't exactly the renamed libretro API and the app's helper:"
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
# No static array of 1 MiB or more (00.66): the PS4 charges the app's whole static memory from its start, used or not
# (the recompilers' code is in the app's JIT arena, the big tables on the heap while a game runs).
BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/dc_core.o.new" | awk '($3 == "b" || $3 == "B") && $2 + 0 >= 1048576 {print $4}')
if [ -n "$BIG" ]; then
  echo "Flycast: the object has static arrays of 1 MiB or more:"
  echo "$BIG"
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
