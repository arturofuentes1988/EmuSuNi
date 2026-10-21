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
#     pipelines and shaders made and their time, the GPU waits and full GPU syncs; and in that
#     window the faults by kind (writes to pages of compiled code, to textures' VRAM, the table's
#     pieces mapped, the hardware accesses rewritten, others passed on) and the mprotect calls.
#   - Built as a static library, without LTO, with its own zlib, zstd (without its tracing hooks),
#     libchdr, libzip, glslang... (nothing of the system's), without UPnP, Lua, breakpad, Discord,
#     OpenMP or the GDB server, for 16 KiB pages. The PS4's CMake system is "Generic": asio without
#     kqueue, libzip without the Annex K functions the PS4's libc has but doesn't declare. As one
#     object: libchdr's LZMA SDK (24.05) only for what the rest doesn't have (the core's own 7-Zip
#     copy has the same functions, which upstream's link takes).
#   - The arcade boards (00.63, the app's NAOMI and Atomiswave tabs): flycast_emusuni_arcade_list gives the
#     app every game Flycast's list has (set, parent, full name, BIOS zip, board, GD-ROM image, flags); the
#     NAOMI printer's fonts are in the program again (a few NAOMI games print).
# The object is checked at the end: only flycast_retro_* (and flycast_emusuni_arcade_list) defined globally, no weak references left,
# the C++ operator new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and
# none defined in it, and every other symbol it needs found in what the app links (OpenOrbis' libc,
# libkernel, libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c;
# emusuni_dc_thread from port/).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-dc-core.sh <out dir> [clean]
#   DC_CORE_HOST=1 build-dc-core.sh <out dir>   the same object for this PC (Linux, clang 21), for the
#                                               PC tests: no PS4 checks; the test frontend provides
#                                               emusuni_make_executable and emusuni_dc_thread.
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
      'target_sources(${PROJECT_NAME} PRIVATE\n        emusuni_mem.cpp\n        emusuni_timing.cpp\n        directory.cpp\n')
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
# file not in the set - still throws: src/Emulator.cpp looks for what it can before.)
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
      'extern "C" int emusuni_make_executable(void *address, size_t size);  // port/ps4_n64.c\n'
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
      '\tstruct Counters { int64_t emuRun, emuWait, render, renderWait, pipeline, shader, gpuWait; int pipelines, shaders, compiled, idleWaits;\n'
      '\t\tint faultsCode, faultsVram, faultsTable, faultsRewrite, faultsOther, protects, unprotects; };\n'
      '\tauto read = []() {\n'
      '\t\tconst emusuni::Timing& t = emusuni::timing;\n'
      '\t\treturn Counters { t.emuRunNs.load(), t.emuWaitNs.load(), t.renderNs.load(), t.renderWaitNs.load(), t.pipelineNs.load(),\n'
      '\t\t\tt.shaderNs.load(), t.gpuWaitNs.load(), t.pipelines.load(), t.shaders.load(), t.shadersCompiled.load(), t.idleWaits.load(),\n'
      '\t\t\tt.faultsCode.load(), t.faultsVram.load(), t.faultsTable.load(), t.faultsRewrite.load(), t.faultsOther.load(),\n'
      '\t\t\tt.protects.load(), t.unprotects.load() };\n'
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
      '\tNOTICE_LOG(COMMON, "EmuSuNi: dc per frame (%d runs, %.1f pictures/s, %s, SH4 %s, %s): retro_run %.2f ms, of it rendering %.2f "\n'
      '\t\t"and waiting for the emulation %.2f; emulation thread busy %.2f ms (waiting for the renderer %.2f); in the rendering: new pipelines %d "\n'
      '\t\t"(%.1f ms), shaders %d of them compiled %d (%.1f ms), GPU waits %.2f ms (%d full syncs); faults: code %d, VRAM %d, table %d, "\n'
      '\t\t"hardware %d, other %d; mprotect %d+%d",\n'
      '\t\truns, pictures / ((now - windowStart) / 1e9), threaded ? "threaded" : "one thread", emusuniSh4JitOk ? "recompiler" : "interpreter",\n'
      '\t\taddrspace::virtmemEnabled() ? "fast memory" : "memory handlers",\n'
      '\t\trunNs / ms, (c.render - before.render) / ms, (c.renderWait - before.renderWait) / ms, emulation,\n'
      '\t\t(c.emuWait - before.emuWait) / ms, c.pipelines - before.pipelines, (c.pipeline - before.pipeline) / 1e6,\n'
      '\t\tc.shaders - before.shaders, c.compiled - before.compiled, (c.shader - before.shader) / 1e6,\n'
      '\t\t(c.gpuWait - before.gpuWait) / ms, c.idleWaits - before.idleWaits,\n'
      '\t\tc.faultsCode - before.faultsCode, c.faultsVram - before.faultsVram, c.faultsTable - before.faultsTable,\n'
      '\t\tc.faultsRewrite - before.faultsRewrite, c.faultsOther - before.faultsOther,\n'
      '\t\tc.protects - before.protects, c.unprotects - before.unprotects);\n'
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
# PS4's CPU, the core options' translations left out (HAVE_NO_LANGEXTRA: 88 tables of the options in
# other languages, 9.4 MB of the program image; the app doesn't ask for a language and sets the
# options itself), a section for each function and variable (the app's link, --gc-sections, drops the
# ones nothing uses), unwind tables only for the functions that can throw (not the toolchain's
# -funwind-tables, a frame for every function) and the renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -DHAVE_NO_LANGEXTRA -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/prefix_flycast.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have, and the PS4 calls of
# linux/emusuni_vmem.cpp done by stand-ins on Linux: EMUSUNI_DC_HOST.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a -DEMUSUNI_DC_HOST=1"
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
# (And the app's own helper: the arcade boards' games, flycast_emusuni_arcade_list in hw/naomi/naomi_roms.cpp.)
HELPERS="flycast_emusuni_arcade_list"
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
