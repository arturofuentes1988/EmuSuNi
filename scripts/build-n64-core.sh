#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Mupen64Plus-Next (libretro-mupen64plus-nx, ~/src/mupen64plus-next) for the PS4 into one
# object that scripts/build.sh links into EmuSuNi: the x86_64 dynarec, the ParaLLEl RSP
# (GNU Lightning JIT) and RDP (Vulkan compute), the CXD4 RSP (no JIT) as the fallback. Its
# libretro API becomes mupen64plus_retro_* and everything else in it private, as for FCEUmm
# (CMakeLists.txt). The PS4 changes to its sources (applied each time, checked):
#   - ParaLLEl-RDP keeps its Vulkan context between games: EmuSuNi creates its only Vulkan device
#     with it once, at start (src/Display.cpp).
#   - Granite (ParaLLEl's Vulkan layer) uses one queue for everything, the one the menus draw on.
#   - The dynarec's code cache is not an array in the core's data (34 MiB of g_dev) but the app's
#     shared JIT arena (port/ps4_n64.c's emusuni_jit_arena: 32 MiB of the app's .bss that the PS1's,
#     PSP's, DS's and Dreamcast's JITs use too, one core at a time; within 2 GiB of the core, made
#     executable the ways the PS4 allows), taken again at every start of the dynarec. Its end leaves
#     the arena executable for the next core (no mprotect back to read/write).
#   - GLideN64's 4 MiB RDP command buffer (that renderer is never used here) on the heap, made in
#     RDP_Init, not in the .bss.
#   - The N64's memory: not the 512 MiB layout (the app's heap is 512 MiB) but the compressed one,
#     aligned to 64 KiB as ParaLLEl-RDP wants it.
#   - No VK_EXT_external_memory_host: the PS4's RADV lists it but can't import host memory
#     (ac_drm_create_bo_from_user_mem), so ParaLLEl-RDP copies RDRAM from the start.
#   - ParaLLEl-RDP's compiled pipelines are kept in the file EMUSUNI_RDP_PIPELINE_CACHE names
#     (src/Emulator.cpp): read at each game start, written every 600 frames and at its end, by a
#     thread of its own (the file's writing showed as a stall on the emulation thread).
#   - "hle" RSP with ParaLLEl-RDP: HLE does the sound, ParaLLEl-RSP the rest (graphics, and any task
#     HLE doesn't know), as mupen64plus's RspFallback does: the PS4's CPU is too slow for all of
#     the N64's sound in LLE.
#   - ParaLLEl-RDP's threads on cores of their own (port/ps4_n64.c's emusuni_core_thread): the
#     command thread at the emulation thread's priority (480; the emulation waits for it at the end
#     of every frame), the fence worker just below, the pipeline compiler low.
#   - ParaLLEl-RDP's command ring without a lock, a notify and a possible wake-up per RDP command:
#     the emulation thread publishes commands with atomics, the command thread takes all of them
#     at once, and each wakes the other only when it sleeps (with N64 CPU Cores Fixed both spin a
#     little first). 256 KiB, not 16: one Mortal Kombat 4 frame is about 180 KiB of commands. Its
#     counters are in the log every 600 frames ("RDP command thread per frame").
#   - ParaLLEl-RSP's vector loads/stores and DMAs move whole halfwords and lines (its byte loops only
#     for odd addresses or elements): the same result, checked against HEAD on every address.
#   - ParaLLEl-RDP's VI scanout can run on its command thread (EMUSUNI_ASYNC_SCANOUT=1, the N64
#     Picture Timing setting): the emulation doesn't wait for it every frame; the picture comes one
#     frame later.
#   - For GoldenEye 007's kind of frame (its microcode hands the RDP one command at a time): the
#     ring's producer copies a command at once, ParaLLEl-RSP jumps between its compiled blocks and
#     reads/writes the plain COP0 registers without calls into C, and the dynarec's JR lookup has a
#     small cache in front. The same pictures, sound and RDRAM; about 12% less RSP time there.
# Everything is compiled again when the flags or this script change (make tracks neither flags nor
# headers: in the N64 test 00.02, Granite's objects saw two Vulkan header versions and disagreed on
# its Context, and the app closed at every N64 game's start).
#   build-n64-core.sh <out dir> [clean]
set -euo pipefail
SRC="${MUPEN64PLUS_DIR:-$HOME/src/mupen64plus-next}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
LOG="$OUT/n64-core.log"

python3 - "$SRC" <<'PATCH'
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
    assert text.count(old) == 1, (path, text.count(old))
    patched[path] = text.replace(old, new)
patch('mupen64plus-video-paraLLEl/parallel-rdp/vulkan/context.cpp',
      "\tbool broken_async_queues =\n\t\t\text.supports_driver_properties &&\n"
      "\t\t\text.driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY;",
      "\t// EmuSuNi (PS4): one queue for everything, the frontend's.\n\tbool broken_async_queues = true;")
patch('mupen64plus-video-paraLLEl/parallel-rdp/vulkan/context.cpp',
      "\tif (!find_vacant_queue(queue_info.family_indices[QUEUE_INDEX_TRANSFER], queue_indices[QUEUE_INDEX_TRANSFER],\n"
      "\t                       VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0.5f) &&",
      "\tif (broken_async_queues ||\n"
      "\t    !find_vacant_queue(queue_info.family_indices[QUEUE_INDEX_TRANSFER], queue_indices[QUEUE_INDEX_TRANSFER],\n"
      "\t                       VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0.5f) &&")
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "#else\n  mprotect ((u_char *)g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2,\n"
      "            PROT_READ | PROT_WRITE | PROT_EXEC);\n  base_addr = g_dev.r4300.extra_memory;\n"
      "  base_addr_rx = base_addr;\n#endif\n#endif\n#endif",
      "#elif defined(__PS4__)\n"
      "  // EmuSuNi: the shared JIT arena's part (ensure_dynarec_cache_alignment), read/write/execute already.\n"
      "  if (g_dev.r4300.extra_memory == NULL)\n"
      "    DebugMessage(M64MSG_ERROR, \"dynarec: no executable memory for the code cache (EmuSuNi's JIT arena)\");\n"
      "  base_addr = g_dev.r4300.extra_memory;\n  base_addr_rx = base_addr;\n"
      "#else\n  mprotect ((u_char *)g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2,\n"
      "            PROT_READ | PROT_WRITE | PROT_EXEC);\n  base_addr = g_dev.r4300.extra_memory;\n"
      "  base_addr_rx = base_addr;\n#endif\n#endif\n#endif")
# EmuSuNi 00.63: the code cache out of g_dev (34 MiB of the app's .bss, which the PS4 charges at load): a
# part of the app's shared JIT arena, taken at every start of the dynarec (another core may have used it
# between two N64 games) and left executable at its end. g_dev's layout changes: the assembly's offsets
# (asm_defines_nasm.h) are made again from asm_defines.o by the core's Makefile.
patch('mupen64plus-core/src/device/r4300/r4300_core.h',
      "    ALIGN(4096, unsigned char extra_memory_buffer[NEW_DYNAREC_CACHE_SIZE + NEW_DYNAREC_CACHE_PAGE_PAD]);\n"
      "    unsigned char* extra_memory;\n",
      "#ifndef __PS4__\n"
      "    ALIGN(4096, unsigned char extra_memory_buffer[NEW_DYNAREC_CACHE_SIZE + NEW_DYNAREC_CACHE_PAGE_PAD]);\n"
      "#endif\n"
      "    /* EmuSuNi (PS4): the code cache, a part of the app's shared JIT arena (port/ps4_n64.c). */\n"
      "    unsigned char* extra_memory;\n")
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "static void ensure_dynarec_cache_alignment(void)\n{\n"
      "  if (g_dev.r4300.extra_memory != NULL)\n    return;\n\n"
      "  g_dev.r4300.extra_memory = (unsigned char*)\n"
      "      dynarec_align_cache_buffer(g_dev.r4300.extra_memory_buffer,\n"
      "                                 sizeof(g_dev.r4300.extra_memory_buffer));\n}\n",
      "static void ensure_dynarec_cache_alignment(void)\n{\n"
      "#ifdef __PS4__\n"
      "  /* EmuSuNi (port/ps4_n64.c): the app's shared JIT arena (the PS1's, PSP's, DS's and Dreamcast's JITs\n"
      "     write there between N64 games), taken again at every start: 32 MiB, read/write/execute, within\n"
      "     2 GiB of the core. NULL: no executable memory (the app then runs the cached interpreter). */\n"
      "  extern void *emusuni_jit_arena(size_t size, const char *name);\n"
      "  g_dev.r4300.extra_memory = (unsigned char*)emusuni_jit_arena(NEW_DYNAREC_CACHE_SIZE, \"n64\");\n"
      "#else\n"
      "  if (g_dev.r4300.extra_memory != NULL)\n    return;\n\n"
      "  g_dev.r4300.extra_memory = (unsigned char*)\n"
      "      dynarec_align_cache_buffer(g_dev.r4300.extra_memory_buffer,\n"
      "                                 sizeof(g_dev.r4300.extra_memory_buffer));\n"
      "#endif\n}\n")
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "  #else\n    mprotect(base_addr, 1<<TARGET_SIZE_2, PROT_READ | PROT_WRITE);\n  #endif\n",
      "  #elif defined(__PS4__)\n"
      "    /* EmuSuNi: the shared JIT arena stays read/write/execute for the next core (port/ps4_n64.c). */\n"
      "  #else\n    mprotect(base_addr, 1<<TARGET_SIZE_2, PROT_READ | PROT_WRITE);\n  #endif\n")
# GLideN64 (never used: ParaLLEl-RDP draws N64 games) - its 4 MiB command buffer on the heap, made by
# RDP_Init (never called), not in the .bss.
patch('GLideN64/src/RDP.h',
      "\tu32 cmd_data[MAXCMD + 32];\n",
      "#ifdef __PS4__\n"
      "\tu32 *cmd_data; // EmuSuNi: MAXCMD + 32 words on the heap (RDP_Init), not 4 MiB in the .bss\n"
      "#else\n"
      "\tu32 cmd_data[MAXCMD + 32];\n"
      "#endif\n")
patch('GLideN64/src/RDP.cpp',
      "#include <cstring>\n",
      "#include <cstring>\n#include <cstdlib>\n")
patch('GLideN64/src/RDP.cpp',
      "void RDP_Init()\n{\n",
      "void RDP_Init()\n{\n"
      "#ifdef __PS4__\n"
      "\tif (RDP.cmd_data == nullptr)\n"
      "\t\tRDP.cmd_data = static_cast<u32*>(calloc(MAXCMD + 32, sizeof(u32)));\n"
      "#endif\n")
patch('mupen64plus-core/src/device/memory/memory.c',
      "#else\n    if (posix_memalign(&mem_base, MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT, MB_MAX_SIZE_FULL) != 0)\n"
      "        mem_base = NULL;\n#endif // HAVE_LIBNX",
      "#elif defined(__PS4__)\n    /* EmuSuNi (PS4): not the 512 MiB layout (the app's heap is 512 MiB): the compressed one. */\n"
      "    mem_base = NULL;\n"
      "#else\n    if (posix_memalign(&mem_base, MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT, MB_MAX_SIZE_FULL) != 0)\n"
      "        mem_base = NULL;\n#endif // HAVE_LIBNX")
patch('mupen64plus-core/src/device/memory/memory.c',
      "        mem_base = malloc(MB_MAX_SIZE);\n",
      "#ifdef __PS4__\n        /* EmuSuNi: RDRAM (at its start) 64 KiB aligned, as ParaLLEl-RDP wants it. */\n"
      "        if (posix_memalign(&mem_base, MB_RDRAM_DRAM_ALIGNMENT_REQUIREMENT, MB_MAX_SIZE) != 0)\n"
      "            mem_base = NULL;\n#else\n        mem_base = malloc(MB_MAX_SIZE);\n#endif\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/vulkan/context.cpp',
      "\tif (ext.supports_external && has_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))\n",
      "#ifdef __PS4__\n\t// EmuSuNi: the PS4's RADV lists it but can't import host memory: ParaLLEl-RDP copies RDRAM.\n"
      "\tif (false)\n#else\n"
      "\tif (ext.supports_external && has_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))\n#endif\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "#include <assert.h>\n",
      "#include <assert.h>\n#include <chrono>\n#include <condition_variable>\n#include <mutex>\n"
      "#include <stdio.h>\n#include <stdlib.h>\n#include <string>\n#include <thread>\n#include <vector>\n"
      "#ifdef __PS4__\nextern \"C\" void emusuni_core_thread(int role);\n#endif\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "bool init()\n{\n",
      "#ifdef __PS4__\n"
      "// EmuSuNi: the pipelines ParaLLEl-RDP compiled (the PS4 takes up to 0.7 s for one) kept in the file\n"
      "// EMUSUNI_RDP_PIPELINE_CACHE names: read at each start, written whenever new ones came.\n"
      "static size_t emusuni_cache_kept = 0;\n"
      "static unsigned emusuni_cache_frames = 0;\n"
      "\n"
      "static void emusuni_load_pipeline_cache()\n"
      "{\n"
      "\tconst char *path = getenv(\"EMUSUNI_RDP_PIPELINE_CACHE\");\n"
      "\tstd::vector<uint8_t> data;\n"
      "\tif (path)\n"
      "\t{\n"
      "\t\tif (FILE *file = fopen(path, \"rb\"))\n"
      "\t\t{\n"
      "\t\t\tif (fseek(file, 0, SEEK_END) == 0)\n"
      "\t\t\t{\n"
      "\t\t\t\tconst long size = ftell(file);\n"
      "\t\t\t\tif (size > 0 && size < (64l << 20) && fseek(file, 0, SEEK_SET) == 0)\n"
      "\t\t\t\t{\n"
      "\t\t\t\t\tdata.resize(size_t(size));\n"
      "\t\t\t\t\tif (fread(data.data(), 1, data.size(), file) != data.size())\n"
      "\t\t\t\t\t\tdata.clear();\n"
      "\t\t\t\t}\n"
      "\t\t\t}\n"
      "\t\t\tfclose(file);\n"
      "\t\t}\n"
      "\t}\n"
      "\t// Always a cache (Granite makes none without its filesystem): new pipelines go into it. Its\n"
      "\t// data is checked (this GPU and driver, its hash), else a fresh one.\n"
      "\tdevice->init_pipeline_cache(data.empty() ? nullptr : data.data(), data.size());\n"
      "\temusuni_cache_kept = device->get_pipeline_cache_size();\n"
      "\temusuni_cache_frames = 0;\n"
      "\tif (log_cb)\n"
      "\t\tlog_cb(RETRO_LOG_INFO, \"EmuSuNi: pipeline cache %s: %u bytes read, %u in use.\\n\",\n"
      "\t\t       path ? path : \"(no file named)\", unsigned(data.size()), unsigned(emusuni_cache_kept));\n"
      "}\n"
      "\n"
      "// The file is written by a thread of its own (the PS4's storage can take 10-100 ms: on the\n"
      "// emulation thread that showed as a stall); the data is taken on the emulation thread.\n"
      "static std::mutex emusuni_writer_lock;\n"
      "static std::condition_variable emusuni_writer_wake;\n"
      "static std::vector<uint8_t> emusuni_writer_data;  // waiting to be written\n"
      "static bool emusuni_writer_stop = false;\n"
      "static std::thread emusuni_writer;\n"
      "\n"
      "static void emusuni_write_cache_file(const char *path, const std::vector<uint8_t> &data)\n"
      "{\n"
      "\tconst auto start = std::chrono::steady_clock::now();\n"
      "\tconst std::string temp = std::string(path) + \".new\";\n"
      "\tbool written = false;\n"
      "\tif (FILE *file = fopen(temp.c_str(), \"wb\"))\n"
      "\t{\n"
      "\t\twritten = fwrite(data.data(), 1, data.size(), file) == data.size();\n"
      "\t\twritten = fclose(file) == 0 && written;\n"
      "\t}\n"
      "\tif (!written || rename(temp.c_str(), path) != 0)\n"
      "\t{\n"
      "\t\twritten = false;\n"
      "\t\tremove(temp.c_str());\n"
      "\t}\n"
      "\tif (log_cb)\n"
      "\t\tlog_cb(RETRO_LOG_INFO, \"EmuSuNi: pipeline cache: %u bytes %s in %.0f ms.\\n\", unsigned(data.size()),\n"
      "\t\t       written ? \"kept\" : \"couldn't be written\",\n"
      "\t\t       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());\n"
      "}\n"
      "\n"
      "static void emusuni_writer_loop(std::string path)\n"
      "{\n"
      "\temusuni_core_thread(4);  // low, off the emulation's core (port/ps4_n64.c)\n"
      "\tstd::unique_lock<std::mutex> lock(emusuni_writer_lock);\n"
      "\tfor (;;)\n"
      "\t{\n"
      "\t\temusuni_writer_wake.wait(lock, [] { return emusuni_writer_stop || !emusuni_writer_data.empty(); });\n"
      "\t\tif (emusuni_writer_data.empty())\n"
      "\t\t\tbreak;  // stopped, and everything written\n"
      "\t\tstd::vector<uint8_t> data;\n"
      "\t\tdata.swap(emusuni_writer_data);\n"
      "\t\tlock.unlock();\n"
      "\t\temusuni_write_cache_file(path.c_str(), data);\n"
      "\t\tlock.lock();\n"
      "\t}\n"
      "}\n"
      "\n"
      "// last: the game's end (the writer finishes, before the device goes).\n"
      "static void emusuni_save_pipeline_cache(bool last)\n"
      "{\n"
      "\tconst char *path = getenv(\"EMUSUNI_RDP_PIPELINE_CACHE\");\n"
      "\tif (path && device)\n"
      "\t{\n"
      "\t\tconst size_t size = device->get_pipeline_cache_size();\n"
      "\t\tstd::vector<uint8_t> data(size);\n"
      "\t\tif (size != 0 && size != emusuni_cache_kept && device->get_pipeline_cache_data(data.data(), size))\n"
      "\t\t{\n"
      "\t\t\temusuni_cache_kept = size;\n"
      "\t\t\tstd::lock_guard<std::mutex> lock(emusuni_writer_lock);\n"
      "\t\t\temusuni_writer_data.swap(data);\n"
      "\t\t\tif (!emusuni_writer.joinable())\n"
      "\t\t\t\temusuni_writer = std::thread(emusuni_writer_loop, std::string(path));\n"
      "\t\t\temusuni_writer_wake.notify_one();\n"
      "\t\t}\n"
      "\t}\n"
      "\tif (last && emusuni_writer.joinable())\n"
      "\t{\n"
      "\t\t{\n"
      "\t\t\tstd::lock_guard<std::mutex> lock(emusuni_writer_lock);\n"
      "\t\t\temusuni_writer_stop = true;\n"
      "\t\t\temusuni_writer_wake.notify_one();\n"
      "\t\t}\n"
      "\t\temusuni_writer.join();\n"
      "\t\temusuni_writer_stop = false;\n"
      "\t}\n"
      "}\n"
      "#endif\n"
      "\n"
      "bool init()\n{\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tdevice->set_context(*context);\n\tdevice->init_frame_contexts(num_sync_frames);\n",
      "\tdevice->set_context(*context);\n"
      "#ifdef __PS4__\n"
      "\temusuni_load_pipeline_cache();\n"
      "#endif\n"
      "\tdevice->init_frame_contexts(num_sync_frames);\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tfrontend.reset();\n\tdevice.reset();\n\tcontext.reset();\n\ttimeline_trace_file.reset();",
      "\tfrontend.reset();\n"
      "#ifdef __PS4__\n"
      "\temusuni_save_pipeline_cache(true);  // EmuSuNi: the pipelines compiled, for the next start\n"
      "#endif\n"
      "\tdevice.reset();\n\tcontext.reset();\n\ttimeline_trace_file.reset();")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "void complete_frame()\n{\n",
      "void complete_frame()\n{\n"
      "#ifdef __PS4__\n"
      "\t// EmuSuNi: every 600 frames, the pipelines compiled meanwhile are kept (the app can be closed\n"
      "\t// with the PS button, without a deinit).\n"
      "\tif (frontend && ++emusuni_cache_frames % 600 == 0)\n"
      "\t\temusuni_save_pipeline_cache(false);\n"
      "#endif\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tdevice.reset();\n\tcontext.reset();\n\ttimeline_trace_file.reset();",
      "\tdevice.reset();\n\t// EmuSuNi: the frontend's device was made with this context, once: kept for the next game.\n"
      "\ttimeline_trace_file.reset();")
patch('libretro/libretro.c',
      "       var.key = CORE_NAME \"-rsp-plugin\";\n",
      "#ifdef __PS4__\n"
      "       {\n"
      "          extern int emusuni_hle_forward;\n"
      "          emusuni_hle_forward = 0;  /* EmuSuNi: set below for HLE sound with ParaLLEl */\n"
      "       }\n"
      "#endif\n"
      "       var.key = CORE_NAME \"-rsp-plugin\";\n")
patch('libretro/libretro.c',
      "#if defined(HAVE_PARALLEL_RSP)\n"
      "                plugin_connect_rsp_api(RSP_PLUGIN_PARALLEL);\n"
      "                log_cb(RETRO_LOG_INFO, \"Selected HLE RSP with Angrylion, falling back to Parallel RSP!\\n\");\n",
      "#if defined(HAVE_PARALLEL_RSP) && defined(__PS4__)\n"
      "                if (current_rdp_type == RDP_PLUGIN_PARALLEL)\n"
      "                {\n"
      "                   /* EmuSuNi: the sound by HLE, graphics (and any task HLE doesn't know) by ParaLLEl-RSP. */\n"
      "                   extern int emusuni_hle_forward;\n"
      "                   emusuni_hle_forward = 1;\n"
      "                   plugin_connect_rsp_api(RSP_PLUGIN_HLE);\n"
      "                   log_cb(RETRO_LOG_INFO, \"EmuSuNi: HLE RSP for the sound, ParaLLEl-RSP for the rest.\\n\");\n"
      "                }\n"
      "                else\n"
      "                   plugin_connect_rsp_api(RSP_PLUGIN_PARALLEL);\n"
      "#elif defined(HAVE_PARALLEL_RSP)\n"
      "                plugin_connect_rsp_api(RSP_PLUGIN_PARALLEL);\n"
      "                log_cb(RETRO_LOG_INFO, \"Selected HLE RSP with Angrylion, falling back to Parallel RSP!\\n\");\n")
patch('mupen64plus-rsp-hle/src/plugin.c',
      "#include \"m64p_types.h\"\n",
      "#include \"m64p_types.h\"\n"
      "\n"
      "#if defined(__PS4__) && defined(HAVE_PARALLEL_RSP)\n"
      "/* EmuSuNi: the tasks HLE doesn't do (graphics: ParaLLEl-RDP has no HLE display lists) go to\n"
      " * ParaLLEl-RSP, as mupen64plus's RspFallback does. */\n"
      "int emusuni_hle_forward = 0;\n"
      "unsigned int parallelRSPDoRspCycles(unsigned int Cycles);\n"
      "void parallelRSPInitiateRSP(RSP_INFO Rsp_Info, unsigned int *CycleCount);\n"
      "void parallelRSPRomClosed(void);\n"
      "#endif\n")
patch('mupen64plus-rsp-hle/src/plugin.c',
      "int HleForwardTask(void* user_defined)\n{\n    return -1;\n}\n",
      "int HleForwardTask(void* user_defined)\n{\n"
      "#if defined(__PS4__) && defined(HAVE_PARALLEL_RSP)\n"
      "    if (emusuni_hle_forward)\n"
      "    {\n"
      "        parallelRSPDoRspCycles(0xffffffffu);\n"
      "        return 0;\n"
      "    }\n"
      "#endif\n"
      "    return -1;\n}\n")
patch('mupen64plus-rsp-hle/src/plugin.c',
      "    g_hle.hle_gfx = 1;\n    g_hle.hle_aud = 0;\n",
      "    g_hle.hle_gfx = 1;\n    g_hle.hle_aud = 0;\n"
      "#if defined(__PS4__) && defined(HAVE_PARALLEL_RSP)\n"
      "    if (emusuni_hle_forward)\n"
      "    {\n"
      "        g_hle.hle_gfx = 0;  /* graphics tasks are forwarded */\n"
      "        parallelRSPInitiateRSP(Rsp_Info, CycleCount);\n"
      "    }\n"
      "#endif\n")
patch('mupen64plus-rsp-hle/src/plugin.c',
      "     g_hle.cached_ucodes.count = 0;\n",
      "     g_hle.cached_ucodes.count = 0;\n"
      "#if defined(__PS4__) && defined(HAVE_PARALLEL_RSP)\n"
      "    if (emusuni_hle_forward)\n"
      "        parallelRSPRomClosed();\n"
      "#endif\n")
patch('mupen64plus-rsp-hle/src/hle.c',
      "    if (!match)\n    {\n        info = &cached_ucodes->infos[cached_ucodes->count];\n",
      "    if (!match)\n    {\n"
      "        if (cached_ucodes->count >= CACHED_UCODES_MAX_SIZE)\n"
      "            cached_ucodes->count = 0;  /* EmuSuNi: full - start again (it overflowed before) */\n"
      "        info = &cached_ucodes->infos[cached_ucodes->count];\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "static void complete_frame_error()\n",
      "#ifdef __PS4__\n"
      "// EmuSuNi: the time the emulation thread spends ending each frame, in the log every 600 frames.\n"
      "static double emusuni_scanout_ms = 0.0, emusuni_wait_ms = 0.0;\n"
      "static unsigned emusuni_timing_frames = 0;\n"
      "static double emusuni_ms_since(std::chrono::steady_clock::time_point start)\n"
      "{\n"
      "\treturn std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();\n"
      "}\n"
      "#endif\n"
      "\n"
      "static void complete_frame_error()\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tauto image = frontend->scanout(opts);\n",
      "#ifdef __PS4__\n"
      "\tconst auto emusuni_scanout_start = std::chrono::steady_clock::now();\n"
      "#endif\n"
      "\tauto image = frontend->scanout(opts);\n"
      "#ifdef __PS4__\n"
      "\temusuni_scanout_ms += emusuni_ms_since(emusuni_scanout_start);\n"
      "#endif\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tfrontend->begin_frame_context();\n"
      "}\n",
      "#ifdef __PS4__\n"
      "\tconst auto emusuni_wait_start = std::chrono::steady_clock::now();\n"
      "#endif\n"
      "\tfrontend->begin_frame_context();\n"
      "#ifdef __PS4__\n"
      "\temusuni_wait_ms += emusuni_ms_since(emusuni_wait_start);\n"
      "\tif (++emusuni_timing_frames == 600)\n"
      "\t{\n"
      "\t\t// EmuSuNi: and the hand-off to the command thread (parallel-rdp/command_ring.cpp).\n"
      "\t\tCommandRingStats &stats = emusuni_ring_stats;\n"
      "\t\tconst double commands = double(stats.commands.exchange(0, std::memory_order_relaxed)) / 600.0;\n"
      "\t\tconst double batches = double(stats.batches.exchange(0, std::memory_order_relaxed)) / 600.0;\n"
      "\t\tconst double busy_ms = double(stats.busy_ns.exchange(0, std::memory_order_relaxed)) / 600e6;\n"
      "\t\tconst double sleeps = double(stats.sleeps.exchange(0, std::memory_order_relaxed)) / 600.0;\n"
      "\t\tconst double wakes = double(stats.wakes.exchange(0, std::memory_order_relaxed)) / 600.0;\n"
      "\t\tconst double drains = double(stats.drains.exchange(0, std::memory_order_relaxed)) / 600.0;\n"
      "\t\tconst double full = double(stats.full.exchange(0, std::memory_order_relaxed)) / 600.0;\n"
      "\t\tconst double waits_slept = double(stats.waits_slept.exchange(0, std::memory_order_relaxed)) / 600.0;\n"
      "\t\tconst double wait_ms = double(stats.wait_ns.exchange(0, std::memory_order_relaxed)) / 600e6;\n"
      "\t\tconst double timeouts = double(stats.timeouts.exchange(0, std::memory_order_relaxed));\n"
      "\t\tconst double late = double(stats.late.exchange(0, std::memory_order_relaxed));\n"
      "\t\tif (log_cb)\n"
      "\t\t{\n"
      "\t\t\tlog_cb(RETRO_LOG_INFO, \"EmuSuNi: RDP per frame: scanout %.2f ms, waiting for a free frame (GPU) %.2f ms.\\n\",\n"
      "\t\t\t       emusuni_scanout_ms / 600.0, emusuni_wait_ms / 600.0);\n"
      "\t\t\tlog_cb(RETRO_LOG_INFO, \"EmuSuNi: RDP command thread per frame: %.0f commands in %.1f batches, busy %.2f ms, \"\n"
      "\t\t\t       \"slept %.1f times, woken %.1f times; the emulation waited for it %.2f ms (%.1f drains, ring full \"\n"
      "\t\t\t       \"%.1f times, %.1f sleeps); in 600 frames: %.0f timeouts, %.0f late.\\n\", commands, batches, busy_ms,\n"
      "\t\t\t       sleeps, wakes, wait_ms, drains, full, waits_slept, timeouts, late);\n"
      "\t\t}\n"
      "\t\temusuni_timing_frames = 0;\n"
      "\t\temusuni_scanout_ms = emusuni_wait_ms = 0.0;\n"
      "\t}\n"
      "#endif\n"
      "}\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "#include \"thread_id.hpp\"\n"
      "#include <assert.h>\n",
      "#include \"thread_id.hpp\"\n"
      "#include <assert.h>\n"
      "#include <stdlib.h>\n"
      "#include <algorithm>\n"
      "#ifdef __PS4__\n"
      "extern \"C\" void emusuni_core_thread(int role);\n"
      "// port/ps4_n64.c: N64 CPU Cores Fixed (1) or the PS4's choice (0). Weak: a build without it\n"
      "// (a PC test of these patches) spins.\n"
      "extern \"C\" int emusuni_n64_fixed_cores __attribute__((weak));\n"
      "#endif\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "namespace RDP\n"
      "{\n"
      "void CommandRing::init(\n",
      "namespace RDP\n"
      "{\n"
      "CommandRingStats emusuni_ring_stats;\n"
      "\n"
      "// EmuSuNi: how long each side spins before it sleeps, when the threads have cores of their own:\n"
      "// the other side is usually back within microseconds, and a sleep and its wake-up are system\n"
      "// calls (slow on the PS4). The command thread's 500 us MetaIdle timeout is kept as it was.\n"
      "static const std::chrono::microseconds emusuni_consumer_spin(100);  // the command thread, idle\n"
      "static const std::chrono::microseconds emusuni_producer_spin(500);  // drain() and a full ring\n"
      "static const uint64_t emusuni_kick_words = 1024;      // the producer's exact wake check, at least\n"
      "static const uint64_t emusuni_progress_words = 2048;  // the command thread's progress, at least\n"
      "\n"
      "static inline void emusuni_cpu_pause()\n"
      "{\n"
      "#if defined(__x86_64__) || defined(__i386__)\n"
      "\t__builtin_ia32_pause();\n"
      "#endif\n"
      "}\n"
      "\n"
      "static inline uint64_t emusuni_ns_since(std::chrono::steady_clock::time_point start)\n"
      "{\n"
      "\treturn uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(\n"
      "\t       std::chrono::steady_clock::now() - start).count());\n"
      "}\n"
      "\n"
      "void CommandRing::init(\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "\tring.resize(count);\n"
      "\twrite_count = 0;\n"
      "\tread_count = 0;\n",
      "\tring.resize(count);\n"
      "\t// EmuSuNi: all of it back to the start (completed_count wasn't, in a second init).\n"
      "\twrite_count.store(0, std::memory_order_relaxed);\n"
      "\tread_count.store(0, std::memory_order_relaxed);\n"
      "\tproducer_until.store(~uint64_t(0), std::memory_order_relaxed);\n"
      "\tconsumer_sleeping.store(false, std::memory_order_relaxed);\n"
      "\twritten = 0;\n"
      "\tkicked = 0;\n"
      "\troom_end = count;\n"
      "\tspin = true;\n"
      "#ifdef __PS4__\n"
      "\t// Without cores of their own, a spinning thread could hold the core the other one needs.\n"
      "\tif (&emusuni_n64_fixed_cores)\n"
      "\t\tspin = emusuni_n64_fixed_cores != 0;\n"
      "#endif\n"
      "\tif (const char *env = getenv(\"PARALLEL_RDP_RING_SPIN\"))\n"
      "\t\tspin = strtol(env, nullptr, 0) > 0;\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "void CommandRing::drain()\n"
      "{\n"
      "\tstd::unique_lock<std::mutex> holder{lock};\n"
      "\tcond.wait(holder, [this]() {\n"
      "\t\treturn write_count == completed_count;\n"
      "\t});\n"
      "}\n"
      "\n"
      "void CommandRing::enqueue_command(unsigned num_words, const uint32_t *words)\n"
      "{\n"
      "\tstd::unique_lock<std::mutex> holder{lock};\n"
      "\tcond.wait(holder, [this, num_words]() {\n"
      "\t\treturn write_count + num_words + 1 <= read_count + ring.size();\n"
      "\t});\n"
      "\n"
      "\tsize_t mask = ring.size() - 1;\n"
      "\tring[write_count++ & mask] = num_words;\n"
      "\tfor (unsigned i = 0; i < num_words; i++)\n"
      "\t\tring[write_count++ & mask] = words[i];\n"
      "\n"
      "\tcond.notify_one();\n"
      "}\n"
      "\n"
      "void CommandRing::thread_loop()\n"
      "{\n"
      "\tUtil::register_thread_index(0);\n"
      "\n"
      "#ifdef PARALLEL_RDP_SHADER_DIR\n"
      "\t// Here to let the RDP play nice with full Granite.\n"
      "\t// When we move to standalone Granite, we won't need to interact with global subsystems like this.\n"
      "\tGranite::Global::set_thread_context(*global_handles);\n"
      "\tglobal_handles.reset();\n"
      "#endif\n"
      "\n"
      "\tstd::vector<uint32_t> tmp_buffer;\n"
      "\ttmp_buffer.reserve(64);\n"
      "\tsize_t mask = ring.size() - 1;\n"
      "\n"
      "\tfor (;;)\n"
      "\t{\n"
      "\t\tbool is_idle = false;\n"
      "\t\t{\n"
      "\t\t\tstd::unique_lock<std::mutex> holder{lock};\n"
      "\t\t\tif (cond.wait_for(holder, std::chrono::microseconds(500), [this]() { return write_count > read_count; }))\n"
      "\t\t\t{\n"
      "\t\t\t\tuint32_t num_words = ring[read_count++ & mask];\n"
      "\t\t\t\ttmp_buffer.resize(num_words);\n"
      "\t\t\t\tfor (uint32_t i = 0; i < num_words; i++)\n"
      "\t\t\t\t\ttmp_buffer[i] = ring[read_count++ & mask];\n"
      "\t\t\t}\n"
      "\t\t\telse\n"
      "\t\t\t{\n"
      "\t\t\t\t// If we don't receive commands at a steady pace,\n"
      "\t\t\t\t// notify rendering thread that we should probably kick some work.\n"
      "\t\t\t\ttmp_buffer.resize(1);\n"
      "\t\t\t\ttmp_buffer[0] = uint32_t(Op::MetaIdle) << 24;\n"
      "\t\t\t\tis_idle = true;\n"
      "\t\t\t}\n"
      "\t\t}\n"
      "\n"
      "\t\tif (tmp_buffer.empty())\n"
      "\t\t\tbreak;\n"
      "\n"
      "\t\tprocessor->enqueue_command_direct(tmp_buffer.size(), tmp_buffer.data());\n"
      "\t\tif (!is_idle)\n"
      "\t\t{\n"
      "\t\t\tstd::lock_guard<std::mutex> holder{lock};\n"
      "\t\t\tcompleted_count = read_count;\n"
      "\t\t\tcond.notify_one();\n"
      "\t\t}\n"
      "\t}\n"
      "}\n",
      "void CommandRing::drain()\n"
      "{\n"
      "\t// EmuSuNi: no lock while the command thread keeps up; else wait_for_read().\n"
      "\tconst uint64_t target = write_count.load(std::memory_order_acquire);\n"
      "\tif (read_count.load(std::memory_order_acquire) >= target)\n"
      "\t\treturn;\n"
      "\temusuni_ring_stats.drains.fetch_add(1, std::memory_order_relaxed);\n"
      "\twait_for_read(target);\n"
      "}\n"
      "\n"
      "void CommandRing::enqueue_command(unsigned num_words, const uint32_t *words)\n"
      "{\n"
      "\t// EmuSuNi: no lock. The room is known from the last look at read_count; a full ring waits.\n"
      "\tconst uint64_t size = ring.size();\n"
      "\tconst uint64_t needed = uint64_t(num_words) + 1;\n"
      "\tassert(needed <= size);\n"
      "\tif (written + needed > room_end)\n"
      "\t{\n"
      "\t\troom_end = read_count.load(std::memory_order_acquire) + size;\n"
      "\t\tif (written + needed > room_end)\n"
      "\t\t{\n"
      "\t\t\temusuni_ring_stats.full.fetch_add(1, std::memory_order_relaxed);\n"
      "\t\t\twait_for_read(written + needed - size);\n"
      "\t\t\troom_end = read_count.load(std::memory_order_acquire) + size;\n"
      "\t\t}\n"
      "\t}\n"
      "\n"
      "\tconst uint64_t mask = size - 1;\n"
      "\tuint32_t *base = ring.data();\n"
      "\tbase[written++ & mask] = num_words;\n"
      "\tfor (unsigned i = 0; i < num_words; i++)\n"
      "\t\tbase[written++ & mask] = words[i];\n"
      "\twrite_count.store(written, std::memory_order_release);\n"
      "\n"
      "\t// Wake the command thread if it sleeps: at once for the stop and the Meta ops (one may be waited\n"
      "\t// for: wait_for_timeline() doesn't drain), every 1024 words, and at the end of each\n"
      "\t// ProcessRdpList (rdp.cpp's kick). When it spins (cores of its own) it seldom sleeps, and then\n"
      "\t// the first command that sees it asleep wakes it (a look without a fence: kick() is exact);\n"
      "\t// without the spin it would sleep after almost every command, so it waits for those kicks:\n"
      "\t// one wake-up (a system call) per batch, not per command.\n"
      "\tconst bool urgent = num_words == 0 || ((words[0] >> 24) & 63) < 8;\n"
      "\tif (urgent || written - kicked >= emusuni_kick_words ||\n"
      "\t    (spin && consumer_sleeping.load(std::memory_order_relaxed)))\n"
      "\t\tkick();\n"
      "}\n"
      "\n"
      "void CommandRing::kick()\n"
      "{\n"
      "\tkicked = written;\n"
      "\t// With the command thread's way to sleep (thread_loop): either it sees these commands, or\n"
      "\t// this sees it asleep and wakes it.\n"
      "\twrite_count.store(written, std::memory_order_seq_cst);\n"
      "\tif (consumer_sleeping.load(std::memory_order_seq_cst))\n"
      "\t{\n"
      "\t\tstd::lock_guard<std::mutex> holder{lock};\n"
      "\t\tif (consumer_sleeping.load(std::memory_order_relaxed))\n"
      "\t\t{\n"
      "\t\t\tconsumer_sleeping.store(false, std::memory_order_relaxed);  // no more kicks until it sleeps again\n"
      "\t\t\temusuni_ring_stats.wakes.fetch_add(1, std::memory_order_relaxed);\n"
      "\t\t\tcond.notify_one();\n"
      "\t\t}\n"
      "\t}\n"
      "}\n"
      "\n"
      "// EmuSuNi (command thread): everything before `read` processed, and free again.\n"
      "void CommandRing::publish_read(uint64_t read)\n"
      "{\n"
      "\tread_count.store(read, std::memory_order_seq_cst);\n"
      "\tif (read >= producer_until.load(std::memory_order_seq_cst))\n"
      "\t{\n"
      "\t\tstd::lock_guard<std::mutex> holder{lock};\n"
      "\t\tproducer_cond.notify_one();\n"
      "\t}\n"
      "}\n"
      "\n"
      "// EmuSuNi (producer): until the command thread has processed everything before `target`.\n"
      "void CommandRing::wait_for_read(uint64_t target)\n"
      "{\n"
      "\tconst auto start = std::chrono::steady_clock::now();\n"
      "\tkick();  // it may be asleep with commands it hasn't seen\n"
      "\tbool done = read_count.load(std::memory_order_acquire) >= target;\n"
      "\tif (!done && spin)\n"
      "\t{\n"
      "\t\tconst auto spin_end = start + emusuni_producer_spin;\n"
      "\t\tfor (unsigned i = 1; !done; i++)\n"
      "\t\t{\n"
      "\t\t\temusuni_cpu_pause();\n"
      "\t\t\tdone = read_count.load(std::memory_order_acquire) >= target;\n"
      "\t\t\tif ((i & 63) == 0 && std::chrono::steady_clock::now() >= spin_end)\n"
      "\t\t\t\tbreak;\n"
      "\t\t}\n"
      "\t}\n"
      "\tif (!done)\n"
      "\t{\n"
      "\t\tstd::unique_lock<std::mutex> holder{lock};\n"
      "\t\t// With publish_read(): either it sees this target, or this sees its progress.\n"
      "\t\tproducer_until.store(target, std::memory_order_seq_cst);\n"
      "\t\twhile (read_count.load(std::memory_order_seq_cst) < target)\n"
      "\t\t{\n"
      "\t\t\temusuni_ring_stats.waits_slept.fetch_add(1, std::memory_order_relaxed);\n"
      "\t\t\t// Timed, just in case: a lost wake-up (the tests found none) would cost 5 ms, not the game.\n"
      "\t\t\tif (producer_cond.wait_for(holder, std::chrono::milliseconds(5)) == std::cv_status::timeout)\n"
      "\t\t\t\temusuni_ring_stats.timeouts.fetch_add(1, std::memory_order_relaxed);\n"
      "\t\t}\n"
      "\t\tproducer_until.store(~uint64_t(0), std::memory_order_relaxed);\n"
      "\t}\n"
      "\temusuni_ring_stats.wait_ns.fetch_add(emusuni_ns_since(start), std::memory_order_relaxed);\n"
      "}\n"
      "\n"
      "void CommandRing::thread_loop()\n"
      "{\n"
      "\tUtil::register_thread_index(0);\n"
      "#ifdef __PS4__\n"
      "\t// EmuSuNi: a core of its own, as high as the emulation thread (port/ps4_n64.c): the\n"
      "\t// emulation waits for this thread to drain the ring every frame (scanout).\n"
      "\temusuni_core_thread(1);\n"
      "#endif\n"
      "\n"
      "#ifdef PARALLEL_RDP_SHADER_DIR\n"
      "\t// Here to let the RDP play nice with full Granite.\n"
      "\t// When we move to standalone Granite, we won't need to interact with global subsystems like this.\n"
      "\tGranite::Global::set_thread_context(*global_handles);\n"
      "\tglobal_handles.reset();\n"
      "#endif\n"
      "\n"
      "\t// EmuSuNi: all the commands published so far in one go, each read where it is in the ring\n"
      "\t// (copied only when it wraps around the end), and no lock unless this thread sleeps.\n"
      "\tconst uint64_t size = ring.size();\n"
      "\tconst uint64_t mask = size - 1;\n"
      "\tconst uint32_t *base = ring.data();\n"
      "\tstd::vector<uint32_t> wrapped;\n"
      "\tuint64_t read = read_count.load(std::memory_order_relaxed);\n"
      "\tbool idle = false;  // the last wait ended in MetaIdle: no spin before the next one\n"
      "\t// Busy time: from the end of one wait to the start of the next (the clock is read only there:\n"
      "\t// on the PS4 each read is a TSC read, a multiply and two divisions).\n"
      "\tauto busy_start = std::chrono::steady_clock::now();\n"
      "\n"
      "\tfor (;;)\n"
      "\t{\n"
      "\t\tuint64_t write = write_count.load(std::memory_order_acquire);\n"
      "\t\tif (write == read)\n"
      "\t\t{\n"
      "\t\t\tconst auto now = std::chrono::steady_clock::now();\n"
      "\t\t\temusuni_ring_stats.busy_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(\n"
      "\t\t\t       now - busy_start).count()), std::memory_order_relaxed);\n"
      "\t\t\tconst auto deadline = now + std::chrono::microseconds(500);\n"
      "\t\t\tif (spin && !idle)\n"
      "\t\t\t{\n"
      "\t\t\t\tconst auto spin_end = std::min(deadline, now + emusuni_consumer_spin);\n"
      "\t\t\t\tfor (unsigned i = 1; write == read; i++)\n"
      "\t\t\t\t{\n"
      "\t\t\t\t\temusuni_cpu_pause();\n"
      "\t\t\t\t\twrite = write_count.load(std::memory_order_acquire);\n"
      "\t\t\t\t\tif ((i & 63) == 0 && std::chrono::steady_clock::now() >= spin_end)\n"
      "\t\t\t\t\t\tbreak;\n"
      "\t\t\t\t}\n"
      "\t\t\t}\n"
      "\t\t\tif (write == read)\n"
      "\t\t\t{\n"
      "\t\t\t\tstd::unique_lock<std::mutex> holder{lock};\n"
      "\t\t\t\tfor (;;)\n"
      "\t\t\t\t{\n"
      "\t\t\t\t\t// With kick(): either it sees this flag, or this sees its commands.\n"
      "\t\t\t\t\tconsumer_sleeping.store(true, std::memory_order_seq_cst);\n"
      "\t\t\t\t\twrite = write_count.load(std::memory_order_seq_cst);\n"
      "\t\t\t\t\tif (write != read)\n"
      "\t\t\t\t\t\tbreak;\n"
      "\t\t\t\t\temusuni_ring_stats.sleeps.fetch_add(1, std::memory_order_relaxed);\n"
      "\t\t\t\t\tif (cond.wait_until(holder, deadline) == std::cv_status::timeout)\n"
      "\t\t\t\t\t{\n"
      "\t\t\t\t\t\twrite = write_count.load(std::memory_order_seq_cst);\n"
      "\t\t\t\t\t\t// Commands, and no kick() cleared the flag: they came without a wake-up.\n"
      "\t\t\t\t\t\tif (write != read && consumer_sleeping.load(std::memory_order_relaxed))\n"
      "\t\t\t\t\t\t\temusuni_ring_stats.late.fetch_add(1, std::memory_order_relaxed);\n"
      "\t\t\t\t\t\tbreak;\n"
      "\t\t\t\t\t}\n"
      "\t\t\t\t}\n"
      "\t\t\t\tconsumer_sleeping.store(false, std::memory_order_relaxed);\n"
      "\t\t\t}\n"
      "\t\t\tbusy_start = std::chrono::steady_clock::now();  // work again: commands, or MetaIdle\n"
      "\t\t\tif (write == read)\n"
      "\t\t\t{\n"
      "\t\t\t\t// If we don't receive commands at a steady pace,\n"
      "\t\t\t\t// notify rendering thread that we should probably kick some work.\n"
      "\t\t\t\tconst uint32_t idle_command = uint32_t(Op::MetaIdle) << 24;\n"
      "\t\t\t\tprocessor->enqueue_command_direct(1, &idle_command);\n"
      "\t\t\t\tidle = true;\n"
      "\t\t\t\tcontinue;\n"
      "\t\t\t}\n"
      "\t\t}\n"
      "\t\tidle = false;\n"
      "\n"
      "\t\tuint64_t commands = 0;\n"
      "\t\tuint64_t published = read;\n"
      "\t\tbool stop = false;\n"
      "\t\twhile (read != write)\n"
      "\t\t{\n"
      "\t\t\tconst uint32_t num_words = base[read & mask];\n"
      "\t\t\tif (num_words == 0)\n"
      "\t\t\t{\n"
      "\t\t\t\tstop = true;  // teardown_thread()'s record, the last one\n"
      "\t\t\t\tread++;\n"
      "\t\t\t\tbreak;\n"
      "\t\t\t}\n"
      "\t\t\tconst uint64_t first = (read + 1) & mask;\n"
      "\t\t\tconst uint32_t *words = base + first;\n"
      "\t\t\tif (first + num_words > size)\n"
      "\t\t\t{\n"
      "\t\t\t\twrapped.resize(num_words);\n"
      "\t\t\t\tfor (uint32_t i = 0; i < num_words; i++)\n"
      "\t\t\t\t\twrapped[i] = base[(first + i) & mask];\n"
      "\t\t\t\twords = wrapped.data();\n"
      "\t\t\t}\n"
      "\t\t\tprocessor->enqueue_command_direct(num_words, words);\n"
      "\t\t\tread += uint64_t(num_words) + 1;\n"
      "\t\t\tcommands++;\n"
      "\t\t\tif (read - published >= emusuni_progress_words && read != write)\n"
      "\t\t\t{\n"
      "\t\t\t\tpublish_read(read);  // room, for a producer waiting on a full ring\n"
      "\t\t\t\tpublished = read;\n"
      "\t\t\t}\n"
      "\t\t}\n"
      "\t\tpublish_read(read);\n"
      "\t\temusuni_ring_stats.commands.fetch_add(commands, std::memory_order_relaxed);\n"
      "\t\temusuni_ring_stats.batches.fetch_add(1, std::memory_order_relaxed);\n"
      "\t\tif (stop)\n"
      "\t\t\tbreak;\n"
      "\t}\n"
      "}\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.hpp',
      "#include <vector>\n",
      "#include <vector>\n"
      "#include <atomic>\n"
      "#include <stdint.h>\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.hpp',
      "namespace RDP\n"
      "{\n"
      "class CommandProcessor;\n"
      "class CommandRing\n"
      "{\n",
      "namespace RDP\n"
      "{\n"
      "class CommandProcessor;\n"
      "\n"
      "// EmuSuNi: what the command thread and the hand-off to it did, for the log (rdp.cpp, every 600\n"
      "// frames, taken with exchange(0)).\n"
      "struct CommandRingStats\n"
      "{\n"
      "\tstd::atomic<uint64_t> commands{0};     // commands the command thread processed\n"
      "\tstd::atomic<uint64_t> batches{0};      // times it found new ones\n"
      "\tstd::atomic<uint64_t> busy_ns{0};      // time it worked (not spinning or asleep)\n"
      "\tstd::atomic<uint64_t> sleeps{0};       // times it slept (nothing new after its spin)\n"
      "\tstd::atomic<uint64_t> wakes{0};        // times the emulation thread had to wake it\n"
      "\tstd::atomic<uint64_t> drains{0};       // drains that found work left (scanout, end of frame)\n"
      "\tstd::atomic<uint64_t> full{0};         // times the ring was full\n"
      "\tstd::atomic<uint64_t> waits_slept{0};  // sleeps in those waits (the spin wasn't enough)\n"
      "\tstd::atomic<uint64_t> wait_ns{0};      // time the emulation thread spent in those waits\n"
      "\tstd::atomic<uint64_t> timeouts{0};     // 5 ms timeouts in those sleeps (long waits; lost wake-ups)\n"
      "\tstd::atomic<uint64_t> late{0};         // its 500 us timeouts that found unkicked commands: ~0\n"
      "};\n"
      "extern CommandRingStats emusuni_ring_stats;\n"
      "\n"
      "class CommandRing\n"
      "{\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.hpp',
      "\tvoid enqueue_command(unsigned num_words, const uint32_t *words);\n"
      "\n"
      "private:\n"
      "\tCommandProcessor *processor = nullptr;\n"
      "\tstd::thread thr;\n"
      "\tstd::mutex lock;\n"
      "\tstd::condition_variable cond;\n"
      "\n"
      "\tstd::vector<uint32_t> ring;\n"
      "\tuint64_t write_count = 0;\n"
      "\tuint64_t read_count = 0;\n"
      "\tuint64_t completed_count = 0;\n"
      "\n"
      "\tvoid thread_loop();\n"
      "\tvoid teardown_thread();\n",
      "\tvoid enqueue_command(unsigned num_words, const uint32_t *words);\n"
      "\t// EmuSuNi: the command thread sees every command enqueued so far, even if it was asleep\n"
      "\t// (enqueue_command wakes it at once only for the Meta ops). rdp.cpp calls this at the end of\n"
      "\t// each ProcessRdpList. Like enqueue_command and drain: only from the thread that enqueues.\n"
      "\tvoid kick();\n"
      "\n"
      "private:\n"
      "\tCommandProcessor *processor = nullptr;\n"
      "\tstd::thread thr;\n"
      "\tstd::mutex lock;\n"
      "\tstd::condition_variable cond;           // the command thread sleeps here\n"
      "\tstd::condition_variable producer_cond;  // EmuSuNi: drain() and a full ring sleep here\n"
      "\n"
      "\tstd::vector<uint32_t> ring;\n"
      "\t// EmuSuNi: one thread enqueues (the emulation thread), one processes (the command thread),\n"
      "\t// with no lock per command; each wakes the other only when that one sleeps. write_count:\n"
      "\t// published by the producer; read_count: processed by the command thread (and free again).\n"
      "\t// Apart, on cache lines of their own.\n"
      "\tstd::atomic<uint64_t> write_count{0};\n"
      "\tuint64_t written = 0;   // the producer's copy of write_count\n"
      "\tuint64_t kicked = 0;    // write_count at the last kick()\n"
      "\tuint64_t room_end = 0;  // read_count + ring size, as the producer last saw it\n"
      "\tchar pad_producer[64];\n"
      "\tstd::atomic<uint64_t> read_count{0};\n"
      "\tchar pad_consumer[64];\n"
      "\tstd::atomic<bool> consumer_sleeping{false};\n"
      "\tstd::atomic<uint64_t> producer_until{~uint64_t(0)};  // the producer sleeps until read_count gets here\n"
      "\tbool spin = true;  // spin before sleeping (the threads have cores of their own)\n"
      "\tchar pad_flags[64];\n"
      "\n"
      "\tvoid thread_loop();\n"
      "\tvoid teardown_thread();\n"
      "\tvoid publish_read(uint64_t read);\n"
      "\tvoid wait_for_read(uint64_t target);\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.hpp',
      "\tvoid enqueue_command_direct(unsigned num_words, const uint32_t *words);\n",
      "\tvoid enqueue_command_direct(unsigned num_words, const uint32_t *words);\n"
      "\t// EmuSuNi: the command thread sees what was enqueued so far (command_ring.cpp's kick()).\n"
      "\tvoid kick_command_thread();\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.cpp',
      "void CommandProcessor::enqueue_command_direct(unsigned, const uint32_t *words)\n"
      "{\n",
      "void CommandProcessor::kick_command_thread()\n"
      "{\n"
      "\tif (!single_threaded_processing)\n"
      "\t\tring.kick();\n"
      "}\n"
      "\n"
      "void CommandProcessor::enqueue_command_direct(unsigned, const uint32_t *words)\n"
      "{\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.cpp',
      "\t\t\t\tthis, 4 * 1024);\n",
      "\t\t\t\t// EmuSuNi: 256 KiB, not 16: a long display list doesn't wait for room.\n"
      "\t\t\t\tthis, 64 * 1024);\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "void process_commands()\n"
      "{\n",
      "void process_commands()\n"
      "{\n"
      "\t// EmuSuNi: what this enqueues reaches ParaLLEl-RDP's command thread when it returns, even if\n"
      "\t// that thread was asleep (parallel-rdp/command_ring.cpp: no wake-up per command).\n"
      "\tstruct KickAtReturn\n"
      "\t{\n"
      "\t\t~KickAtReturn()\n"
      "\t\t{\n"
      "\t\t\tif (frontend)\n"
      "\t\t\t\tfrontend->kick_command_thread();\n"
      "\t\t}\n"
      "\t} kick_at_return;\n")
# The fence worker and the pipeline compiler: their cores and priorities too.
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/worker_thread.hpp',
      "namespace RDP\n{\ntemplate <typename T, typename Executor>",
      "#ifdef __PS4__\nextern \"C\" void emusuni_core_thread(int role);\n#endif\n\n"
      "namespace RDP\n{\ntemplate <typename T, typename Executor>")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/worker_thread.hpp',
      "\tvoid main_loop()\n\t{\n",
      "\tvoid main_loop()\n\t{\n"
      "#ifdef __PS4__\n\t\temusuni_core_thread(Executor::emusuni_role);  // EmuSuNi: its core\n#endif\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.hpp',
      "\tstruct FenceExecutor\n\t{\n",
      "\tstruct FenceExecutor\n\t{\n\t\tstatic constexpr int emusuni_role = 2;  // EmuSuNi (port/ps4_n64.c)\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_renderer.hpp',
      "\tstruct PipelineExecutor\n\t{\n",
      "\tstruct PipelineExecutor\n\t{\n\t\tstatic constexpr int emusuni_role = 3;  // EmuSuNi (port/ps4_n64.c)\n")
patch('libretro-common/glsm/glsm.c',
      "static struct gl_program_uniforms program_uniforms[MAX_UNIFORMS][MAX_UNIFORMS];",
      "#ifdef __PS4__\n/* EmuSuNi: no OpenGL on the PS4 (ParaLLEl-RDP draws N64 games): not 80 MiB for it. */\n"
      "static struct gl_program_uniforms program_uniforms[2][2];\n#else\n"
      "static struct gl_program_uniforms program_uniforms[MAX_UNIFORMS][MAX_UNIFORMS];\n#endif")
# EmuSuNi: ParaLLEl-RSP's vector loads/stores in whole halfwords when the address and element are
# even (the usual case; the byte loops otherwise) and its DMAs as memcpy: bit-exact against HEAD on
# every address x element x op, about a quarter less RSP time (the PC's profile: 22% of the
# emulation thread went to byte-by-byte loads and stores).
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(LSV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 2;\n",
      "\t\tTRACE_LS(LSV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 2;\n"
      "\t\t// EmuSuNi: whole halfwords (even element and address: the usual case) at once.\n"
      "\t\tif (!((addr | e) & 1))\n"
      "\t\t{\n"
      "\t\t\trsp->cp2.regs[rt].e[e >> 1] = READ_MEM_U16(rsp->dmem, addr & 0xfff);\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(SSV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 2) & 0xfff;\n",
      "\t\tTRACE_LS(SSV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 2) & 0xfff;\n"
      "\t\tif (!((addr | e) & 1))  // EmuSuNi: a whole halfword\n"
      "\t\t{\n"
      "\t\t\tWRITE_MEM_U16(rsp->dmem, addr, rsp->cp2.regs[rt].e[e >> 1]);\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(LLV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 4;\n",
      "\t\tTRACE_LS(LLV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 4;\n"
      "\t\tif (!((addr | e) & 1) && e <= 12)  // EmuSuNi: whole halfwords\n"
      "\t\t{\n"
      "\t\t\tuint16_t *reg = rsp->cp2.regs[rt].e + (e >> 1);\n"
      "\t\t\treg[0] = READ_MEM_U16(rsp->dmem, addr & 0xfff);\n"
      "\t\t\treg[1] = READ_MEM_U16(rsp->dmem, (addr + 2) & 0xfff);\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(SLV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 4) & 0xfff;\n",
      "\t\tTRACE_LS(SLV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 4) & 0xfff;\n"
      "\t\tif (!((addr | e) & 1) && addr <= 0xffc)  // EmuSuNi: whole halfwords\n"
      "\t\t{\n"
      "\t\t\tconst uint16_t *reg = rsp->cp2.regs[rt].e;\n"
      "\t\t\tWRITE_MEM_U16(rsp->dmem, addr, reg[(e >> 1) & 7]);\n"
      "\t\t\tWRITE_MEM_U16(rsp->dmem, addr + 2, reg[((e >> 1) + 1) & 7]);\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(LDV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 8;\n",
      "\t\tTRACE_LS(LDV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 8;\n"
      "\t\tif (!((addr | e) & 1) && e <= 8)  // EmuSuNi: whole halfwords\n"
      "\t\t{\n"
      "\t\t\tuint16_t *reg = rsp->cp2.regs[rt].e + (e >> 1);\n"
      "\t\t\tfor (unsigned i = 0; i < 4; i++)\n"
      "\t\t\t\treg[i] = READ_MEM_U16(rsp->dmem, (addr + 2 * i) & 0xfff);\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(LQV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 16;\n"
      "\t\tunsigned end = 16 + e - (addr & 0xf);\n"
      "\t\tif (end > 16) end = 16;\n",
      "\t\tTRACE_LS(LQV);\n"
      "\t\tunsigned addr = rsp->sr[base] + offset * 16;\n"
      "\t\tunsigned end = 16 + e - (addr & 0xf);\n"
      "\t\tif (end > 16) end = 16;\n"
      "\t\tif (!((addr | e) & 1))  // EmuSuNi: whole halfwords\n"
      "\t\t{\n"
      "\t\t\tuint16_t *reg = rsp->cp2.regs[rt].e;\n"
      "\t\t\tconst unsigned at = addr & 0xfff;\n"
      "\t\t\tfor (unsigned i = e; i < end; i += 2)\n"
      "\t\t\t\treg[i >> 1] = READ_MEM_U16(rsp->dmem, at + (i - e));\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(SQV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;\n"
      "\t\t\n"
      "\t\tconst unsigned end = e + (16 - (addr & 15));\n",
      "\t\tTRACE_LS(SQV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 16) & 0xfff;\n"
      "\t\t\n"
      "\t\tconst unsigned end = e + (16 - (addr & 15));\n"
      "\t\tif (!((addr | e) & 1))  // EmuSuNi: whole halfwords\n"
      "\t\t{\n"
      "\t\t\tconst uint16_t *reg = rsp->cp2.regs[rt].e;\n"
      "\t\t\tfor (unsigned i = e; i < end; i += 2)\n"
      "\t\t\t\tWRITE_MEM_U16(rsp->dmem, addr + (i - e), reg[(i >> 1) & 7]);\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(LPV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 8) & 0xfff;\n"
      "\t\tconst unsigned index = (addr & 7) - e;\n"
      "\t\taddr &= ~7;\n"
      "\n"
      "\t\tauto *reg = rsp->cp2.regs[rt].e;\n",
      "\t\tTRACE_LS(LPV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 8) & 0xfff;\n"
      "\t\tconst unsigned index = (addr & 7) - e;\n"
      "\t\taddr &= ~7;\n"
      "\n"
      "\t\tauto *reg = rsp->cp2.regs[rt].e;\n"
      "\t\tif (index == 0)  // EmuSuNi: the doubleword's two words\n"
      "\t\t{\n"
      "\t\t\tconst uint32_t w0 = READ_MEM_U32(rsp->dmem, addr), w1 = READ_MEM_U32(rsp->dmem, (addr + 4));\n"
      "\t\t\tfor (unsigned i = 0; i < 4; i++)\n"
      "\t\t\t{\n"
      "\t\t\t\treg[i] = uint16_t(((w0 >> (24 - 8 * i)) & 0xff) << 8);\n"
      "\t\t\t\treg[i + 4] = uint16_t(((w1 >> (24 - 8 * i)) & 0xff) << 8);\n"
      "\t\t\t}\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/ls.cpp',
      "\t\tTRACE_LS(LUV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 8) & 0xfff;\n"
      "\t\tconst unsigned index = (addr & 7) - e;\n"
      "\t\taddr &= ~7;\n"
      "\n"
      "\t\tauto *reg = rsp->cp2.regs[rt].e;\n",
      "\t\tTRACE_LS(LUV);\n"
      "\t\tunsigned addr = (rsp->sr[base] + offset * 8) & 0xfff;\n"
      "\t\tconst unsigned index = (addr & 7) - e;\n"
      "\t\taddr &= ~7;\n"
      "\n"
      "\t\tauto *reg = rsp->cp2.regs[rt].e;\n"
      "\t\tif (index == 0)  // EmuSuNi: the doubleword's two words\n"
      "\t\t{\n"
      "\t\t\tconst uint32_t w0 = READ_MEM_U32(rsp->dmem, addr), w1 = READ_MEM_U32(rsp->dmem, (addr + 4));\n"
      "\t\t\tfor (unsigned i = 0; i < 4; i++)\n"
      "\t\t\t{\n"
      "\t\t\t\treg[i] = uint16_t(((w0 >> (24 - 8 * i)) & 0xff) << 7);\n"
      "\t\t\t\treg[i + 4] = uint16_t(((w1 >> (24 - 8 * i)) & 0xff) << 7);\n"
      "\t\t\t}\n"
      "\t\t\treturn;\n"
      "\t\t}\n")
patch('mupen64plus-rsp-paraLLEl/rsp/cp0.cpp',
      "\t\tdo\n"
      "\t\t{\n"
      "\t\t\tunsigned j = 0;\n"
      "\t\t\tdo\n"
      "\t\t\t{\n"
      "\t\t\t\tuint32_t source_addr = (source + j) & 0x7FFFFC;\n",
      "\t\tdo\n"
      "\t\t{\n"
      "\t\t\t// EmuSuNi: the line in one copy (the words are in the same order on both sides).\n"
      "\t\t\tif (((dest & 0xFFF) + length) <= 0x1000 && ((source & 0x7FFFFC) + length) <= 0x800000)\n"
      "\t\t\t{\n"
      "\t\t\t\tconst uint32_t *from = rsp->rdram + ((source & 0x7FFFFC) >> 2);\n"
      "\t\t\t\tif (dest & 0x1000)\n"
      "\t\t\t\t{\n"
      "\t\t\t\t\tconst unsigned first = (dest & 0xFFC) / CODE_BLOCK_SIZE;\n"
      "\t\t\t\t\tconst unsigned last = ((dest & 0xFFC) + length - 4) / CODE_BLOCK_SIZE;\n"
      "\t\t\t\t\tfor (unsigned block = first; block <= last; block++)\n"
      "\t\t\t\t\t\trsp->dirty_blocks |= (0x3 << block) >> 1;\n"
      "\t\t\t\t\tmemcpy(rsp->imem + ((dest & 0xFFC) >> 2), from, length);\n"
      "\t\t\t\t}\n"
      "\t\t\t\telse\n"
      "\t\t\t\t\tmemcpy(rsp->dmem + ((dest & 0xFFC) >> 2), from, length);\n"
      "\t\t\t\tsource += length + skip;\n"
      "\t\t\t\tdest += length;\n"
      "\t\t\t\tcontinue;\n"
      "\t\t\t}\n"
      "\t\t\tunsigned j = 0;\n"
      "\t\t\tdo\n"
      "\t\t\t{\n"
      "\t\t\t\tuint32_t source_addr = (source + j) & 0x7FFFFC;\n")
patch('mupen64plus-rsp-paraLLEl/rsp/cp0.cpp',
      "\t\tunsigned i = 0;\n"
      "\t\tdo\n"
      "\t\t{\n"
      "\t\t\tunsigned j = 0;\n"
      "\n"
      "\t\t\tdo\n"
      "\t\t\t{\n"
      "\t\t\t\tuint32_t source_addr = (source + j) & 0x1FFC;\n",
      "\t\tunsigned i = 0;\n"
      "\t\tdo\n"
      "\t\t{\n"
      "\t\t\tif (((source & 0xFFF) + length) <= 0x1000 && ((dest & 0x7FFFFC) + length) <= 0x800000)\n"
      "\t\t\t{\n"
      "\t\t\t\t// EmuSuNi: the line in one copy.\n"
      "\t\t\t\tconst uint32_t *from = ((source & 0x1000) ? rsp->imem : rsp->dmem) + ((source & 0xFFC) >> 2);\n"
      "\t\t\t\tmemcpy(rsp->rdram + ((dest & 0x7FFFFC) >> 2), from, length);\n"
      "\t\t\t\tsource += length;\n"
      "\t\t\t\tdest += length + skip;\n"
      "\t\t\t\tcontinue;\n"
      "\t\t\t}\n"
      "\t\t\tunsigned j = 0;\n"
      "\n"
      "\t\t\tdo\n"
      "\t\t\t{\n"
      "\t\t\t\tuint32_t source_addr = (source + j) & 0x1FFC;\n")
patch('mupen64plus-rsp-paraLLEl/rsp/cp0.cpp',
      "#include \"../state.hpp\"\n",
      "#include \"../state.hpp\"\n"
      "#include <string.h>\n")
# EmuSuNi: ParaLLEl-RDP's VI scanout on its command thread (N64 Picture Timing: One Frame Later;
# EMUSUNI_ASYNC_SCANOUT=1, set by src/Emulator.cpp before the game loads and read by RDP::init();
# unset or 0 = as before). The emulation thread no longer drains the command ring and records the VI
# passes at every VI: it queues the VI registers as a command (MetaScanout) and hands the frontend
# the previous VI's picture, which the command thread finished meanwhile. The last two finished
# pictures are kept (picture k in slot k & 1) and the previous VI's is always the one handed over:
# every picture is shown, each exactly one frame later.
# Checked on the PC: perf/compose-test/report.md.
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_common.hpp',
      "\tMetaSetQuirks = 4,\n",
      "\tMetaSetQuirks = 4,\n"
      "\tMetaScanout = 5, // EmuSuNi: the VI scanout, on the command thread (rdp_device.cpp)\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.hpp',
      "#include <queue>\n",
      "#include <queue>\n"
      "#include <condition_variable>\n"
      "#include <mutex>\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.hpp',
      "\tVulkan::ImageHandle scanout(const ScanoutOptions &opts = {});\n",
      "\tVulkan::ImageHandle scanout(const ScanoutOptions &opts = {});\n"
      "\t// EmuSuNi: the scanout on the command thread, after the commands before it, with the VI registers\n"
      "\t// given here (as they were at the VI); take_async_scanout() gives the newest picture finished.\n"
      "\tvoid queue_scanout(const uint32_t *vi_registers, const ScanoutOptions &opts);\n"
      "\tbool take_async_scanout(Vulkan::ImageHandle &image);\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.hpp',
      "\t// Tear-down order is important here.\n"
      "\tRenderer renderer;\n",
      "\t// EmuSuNi: the pictures of queue_scanout() for take_async_scanout(): how many were queued and\n"
      "\t// finished, and the last two finished (picture k in slot k & 1), so that the previous VI's is\n"
      "\t// the one taken even when this VI's is finished too. Destroyed after the command thread stopped.\n"
      "\tstd::mutex async_scanout_lock;\n"
      "\tstd::condition_variable async_scanout_done;\n"
      "\tuint64_t async_scanout_queued = 0;\n"
      "\tuint64_t async_scanout_finished = 0;\n"
      "\tVulkan::ImageHandle async_scanout_images[2];\n"
      "\tvoid scanout_on_command_thread(const uint32_t *words);\n"
      "\n"
      "\t// Tear-down order is important here.\n"
      "\tRenderer renderer;\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.cpp',
      "\tcase Op::MetaSetQuirks:\n"
      "\t{\n"
      "\t\tquirks.u.words[0] = words[1];\n"
      "\t\tbreak;\n"
      "\t}\n",
      "\tcase Op::MetaSetQuirks:\n"
      "\t{\n"
      "\t\tquirks.u.words[0] = words[1];\n"
      "\t\tbreak;\n"
      "\t}\n"
      "\n"
      "\tcase Op::MetaScanout:\n"
      "\t\tscanout_on_command_thread(words);\n"
      "\t\tbreak;\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/rdp_device.cpp',
      "Vulkan::ImageHandle CommandProcessor::scanout(const ScanoutOptions &opts)\n"
      "{\n"
      "\treturn scanout(opts, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);\n"
      "}\n",
      "Vulkan::ImageHandle CommandProcessor::scanout(const ScanoutOptions &opts)\n"
      "{\n"
      "\treturn scanout(opts, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);\n"
      "}\n"
      "\n"
      "// EmuSuNi: asynchronous scanout. words: the op, the VI registers, two words of options.\n"
      "void CommandProcessor::queue_scanout(const uint32_t *vi_registers, const ScanoutOptions &opts)\n"
      "{\n"
      "\tconstexpr unsigned count = unsigned(VIRegister::Count);\n"
      "\tuint32_t words[1 + count + 2];\n"
      "\twords[0] = uint32_t(Op::MetaScanout) << 24;\n"
      "\tfor (unsigned i = 0; i < count; i++)\n"
      "\t\twords[1 + i] = vi_registers[i];\n"
      "\twords[1 + count] = (opts.vi.aa ? 1u : 0u) | (opts.vi.scale ? 2u : 0u) | (opts.vi.serrate ? 4u : 0u) |\n"
      "\t                   (opts.vi.dither_filter ? 8u : 0u) | (opts.vi.divot_filter ? 16u : 0u) |\n"
      "\t                   (opts.vi.gamma_dither ? 32u : 0u) | (opts.blend_previous_frame ? 64u : 0u) |\n"
      "\t                   (opts.upscale_deinterlacing ? 128u : 0u) | (opts.persist_frame_on_invalid_input ? 256u : 0u);\n"
      "\twords[2 + count] = (opts.crop_overscan_pixels & 0xffffu) | (opts.downscale_steps << 16);\n"
      "\t{\n"
      "\t\tstd::lock_guard<std::mutex> hold(async_scanout_lock);\n"
      "\t\tasync_scanout_queued++;\n"
      "\t}\n"
      "\tenqueue_command_inner(1 + count + 2, words);\n"
      "}\n"
      "\n"
      "void CommandProcessor::scanout_on_command_thread(const uint32_t *words)\n"
      "{\n"
      "\tconstexpr unsigned count = unsigned(VIRegister::Count);\n"
      "\tfor (unsigned i = 0; i < count; i++)\n"
      "\t\tvi.set_vi_register(VIRegister(i), words[1 + i]);\n"
      "\tconst uint32_t bits = words[1 + count], more = words[2 + count];\n"
      "\tScanoutOptions opts;\n"
      "\topts.vi.aa = (bits & 1u) != 0;\n"
      "\topts.vi.scale = (bits & 2u) != 0;\n"
      "\topts.vi.serrate = (bits & 4u) != 0;\n"
      "\topts.vi.dither_filter = (bits & 8u) != 0;\n"
      "\topts.vi.divot_filter = (bits & 16u) != 0;\n"
      "\topts.vi.gamma_dither = (bits & 32u) != 0;\n"
      "\topts.blend_previous_frame = (bits & 64u) != 0;\n"
      "\topts.upscale_deinterlacing = (bits & 128u) != 0;\n"
      "\topts.persist_frame_on_invalid_input = (bits & 256u) != 0;\n"
      "\topts.crop_overscan_pixels = more & 0xffffu;\n"
      "\topts.downscale_steps = more >> 16;\n"
      "\n"
      "\t// What scanout() does after its drain: this thread has done every command queued before this one.\n"
      "\trenderer.lock_command_processing();\n"
      "\trenderer.flush_and_signal();\n"
      "\tif (!is_host_coherent)\n"
      "\t{\n"
      "\t\tunsigned offset, length;\n"
      "\t\tvi.scanout_memory_range(offset, length);\n"
      "\t\trenderer.resolve_coherency_external(offset, length);\n"
      "\t}\n"
      "\trenderer.unlock_command_processing();\n"
      "\tauto image = vi.scanout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, opts, renderer.get_scaling_factor());\n"
      "\tif (!image)\n"
      "\t{\n"
      "\t\t// No picture (as rdp.cpp's complete_frame()): one black pixel.\n"
      "\t\tauto info = ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);\n"
      "\t\tinfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;\n"
      "\t\tinfo.misc = IMAGE_MISC_MUTABLE_SRGB_BIT;\n"
      "\t\tinfo.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;\n"
      "\t\timage = device.create_image(info);\n"
      "\t\tauto cmd = device.request_command_buffer();\n"
      "\t\tcmd->image_barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,\n"
      "\t\t                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,\n"
      "\t\t                   VK_ACCESS_TRANSFER_WRITE_BIT);\n"
      "\t\tcmd->clear_image(*image, {});\n"
      "\t\tcmd->image_barrier(*image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,\n"
      "\t\t                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,\n"
      "\t\t                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);\n"
      "\t\tdevice.submit(cmd);\n"
      "\t}\n"
      "\n"
      "\t// What begin_frame_context() does after its drain: the frame submitted, the next frame context.\n"
      "\trenderer.flush_and_signal();\n"
      "\tdevice.next_frame_context();\n"
      "\n"
      "\t// Handed over only now: the frontend's frame, which samples it, is submitted after this one.\n"
      "\tVulkan::ImageHandle older;  // the picture before the last: released after the lock\n"
      "\t{\n"
      "\t\tstd::lock_guard<std::mutex> hold(async_scanout_lock);\n"
      "\t\tasync_scanout_finished++;\n"
      "\t\tolder = std::move(async_scanout_images[async_scanout_finished & 1]);\n"
      "\t\tasync_scanout_images[async_scanout_finished & 1] = std::move(image);\n"
      "\t}\n"
      "\tasync_scanout_done.notify_all();\n"
      "}\n"
      "\n"
      "// Exactly one frame late: the previous VI's picture, waited for if it isn't finished (so a slow\n"
      "// command thread holds the emulation back as the drain did before), and taken even when this VI's\n"
      "// is finished too, so every picture is shown, each for one frame.\n"
      "bool CommandProcessor::take_async_scanout(Vulkan::ImageHandle &image)\n"
      "{\n"
      "\tstd::unique_lock<std::mutex> hold(async_scanout_lock);\n"
      "\tif (async_scanout_queued == 0)\n"
      "\t{\n"
      "\t\timage.reset();\n"
      "\t\treturn false;\n"
      "\t}\n"
      "\tconst uint64_t previous = async_scanout_queued - 1;  // 0: this is the first VI\n"
      "\tasync_scanout_done.wait(hold, [this, previous]() { return async_scanout_finished >= previous; });\n"
      "\t// (At most this VI's is finished after it: the previous one's slot holds it still.)\n"
      "\tif (previous)\n"
      "\t\timage = async_scanout_images[previous & 1];\n"
      "\telse\n"
      "\t\timage.reset();\n"
      "\treturn bool(image);\n"
      "}\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "static int cmd_cur;\n",
      "// EmuSuNi: the scanout on the command thread (EMUSUNI_ASYNC_SCANOUT=1, read at init()).\n"
      "static bool emusuni_async_scanout = false;\n"
      "static int cmd_cur;\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tdevice->init_frame_contexts(num_sync_frames);\n",
      "\t{\n"
      "\t\tconst char *async = getenv(\"EMUSUNI_ASYNC_SCANOUT\");\n"
      "\t\temusuni_async_scanout = async && async[0] == '1';\n"
      "\t\tlog_cb(RETRO_LOG_INFO, \"EmuSuNi: the picture is made %s.\\n\",\n"
      "\t\t       emusuni_async_scanout ? \"by the command thread, one frame later\" : \"at the VI\");\n"
      "\t}\n"
      "\tdevice->init_frame_contexts(num_sync_frames);\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tfrontend->set_vi_register(VIRegister::Control, *GET_GFX_INFO(VI_STATUS_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::Origin, *GET_GFX_INFO(VI_ORIGIN_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::Width, *GET_GFX_INFO(VI_WIDTH_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::Intr, *GET_GFX_INFO(VI_INTR_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::VCurrentLine, *GET_GFX_INFO(VI_V_CURRENT_LINE_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::Timing, *GET_GFX_INFO(VI_V_BURST_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::VSync, *GET_GFX_INFO(VI_V_SYNC_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::HSync, *GET_GFX_INFO(VI_H_SYNC_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::Leap, *GET_GFX_INFO(VI_LEAP_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::HStart, *GET_GFX_INFO(VI_H_START_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::VStart, *GET_GFX_INFO(VI_V_START_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::VBurst, *GET_GFX_INFO(VI_V_BURST_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::XScale, *GET_GFX_INFO(VI_X_SCALE_REG));\n"
      "\tfrontend->set_vi_register(VIRegister::YScale, *GET_GFX_INFO(VI_Y_SCALE_REG));\n",
      "\t// EmuSuNi: the VI registers in VIRegister order (as the calls before did; Timing is V_BURST too).\n"
      "\tconst uint32_t emusuni_vi[unsigned(VIRegister::Count)] = {\n"
      "\t\t*GET_GFX_INFO(VI_STATUS_REG), *GET_GFX_INFO(VI_ORIGIN_REG), *GET_GFX_INFO(VI_WIDTH_REG),\n"
      "\t\t*GET_GFX_INFO(VI_INTR_REG), *GET_GFX_INFO(VI_V_CURRENT_LINE_REG), *GET_GFX_INFO(VI_V_BURST_REG),\n"
      "\t\t*GET_GFX_INFO(VI_V_SYNC_REG), *GET_GFX_INFO(VI_H_SYNC_REG), *GET_GFX_INFO(VI_LEAP_REG),\n"
      "\t\t*GET_GFX_INFO(VI_H_START_REG), *GET_GFX_INFO(VI_V_START_REG), *GET_GFX_INFO(VI_V_BURST_REG),\n"
      "\t\t*GET_GFX_INFO(VI_X_SCALE_REG), *GET_GFX_INFO(VI_Y_SCALE_REG),\n"
      "\t};\n"
      "\tif (!emusuni_async_scanout)  // the command thread's VI object: only through the queue then\n"
      "\t\tfor (unsigned i = 0; i < unsigned(VIRegister::Count); i++)\n"
      "\t\t\tfrontend->set_vi_register(VIRegister(i), emusuni_vi[i]);\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tauto image = frontend->scanout(opts);\n",
      "\tVulkan::ImageHandle image;\n"
      "\tif (emusuni_async_scanout)\n"
      "\t{\n"
      "\t\tfrontend->queue_scanout(emusuni_vi, opts);\n"
      "\t\tfrontend->take_async_scanout(image);  // the previous VI's: none at the first VI\n"
      "\t}\n"
      "\telse\n"
      "\t\timage = frontend->scanout(opts);\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tunsigned index = vulkan->get_sync_index(vulkan->handle);\n"
      "\n"
      "\tif (!image)\n"
      "\t{\n",
      "\tunsigned index = vulkan->get_sync_index(vulkan->handle);\n"
      "\n"
      "\t// (Asynchronous: no picture yet. This thread mustn't record: Granite's command pools are shared\n"
      "\t// with the command thread, thread index 0.)\n"
      "\tif (!image && !emusuni_async_scanout)\n"
      "\t{\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\t\tdevice->submit(cmd);\n"
      "\t}\n"
      "\n"
      "\tassert(index < retro_images.size());\n",
      "\t\tdevice->submit(cmd);\n"
      "\t}\n"
      "\n"
      "\tif (image)\n"
      "\t{\n"
      "\tassert(index < retro_images.size());\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tretro_image_handles[index] = image;\n"
      "\n"
      "\tend_ts = device->write_calibrated_timestamp();\n",
      "\tretro_image_handles[index] = image;\n"
      "\t}\n"
      "\n"
      "\tend_ts = device->write_calibrated_timestamp();\n")
patch('mupen64plus-video-paraLLEl/rdp.cpp',
      "\tfrontend->begin_frame_context();\n",
      "\tif (!emusuni_async_scanout)  // else the command thread did it after the picture\n"
      "\t\tfrontend->begin_frame_context();\n")
# GoldenEye 007 (Fast3D, its RDP commands handed over one at a time: about 2900 per frame) - the
# command ring's producer: the core is built with -fno-strict-aliasing, so the ring's word stores may
# alias `written`, which was loaded and stored once per word on the cache line the command thread
# polls. Now a local index, one copy unless the command wraps, and write_count on a line of its own.
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "#include <stdlib.h>\n#include <algorithm>\n",
      "#include <stdlib.h>\n#include <string.h>\n#include <algorithm>\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "\tconst uint64_t mask = size - 1;\n"
      "\tuint32_t *base = ring.data();\n"
      "\tbase[written++ & mask] = num_words;\n"
      "\tfor (unsigned i = 0; i < num_words; i++)\n"
      "\t\tbase[written++ & mask] = words[i];\n"
      "\twrite_count.store(written, std::memory_order_release);\n",
      "\t// EmuSuNi: a local index (the ring's stores may alias `written`: -fno-strict-aliasing), the\n"
      "\t// words in one copy unless they wrap.\n"
      "\tconst uint64_t mask = size - 1;\n"
      "\tuint32_t *base = ring.data();\n"
      "\tuint64_t at = written;\n"
      "\tbase[at++ & mask] = num_words;\n"
      "\tconst uint64_t first = at & mask;\n"
      "\tif (first + num_words <= size)\n"
      "\t\tmemcpy(base + first, words, num_words * sizeof(uint32_t));\n"
      "\telse\n"
      "\t\tfor (unsigned i = 0; i < num_words; i++)\n"
      "\t\t\tbase[(at + i) & mask] = words[i];\n"
      "\tat += num_words;\n"
      "\twritten = at;\n"
      "\twrite_count.store(at, std::memory_order_release);\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.hpp',
      "\tstd::atomic<uint64_t> write_count{0};\n"
      "\tuint64_t written = 0;   // the producer's copy of write_count\n"
      "\tuint64_t kicked = 0;    // write_count at the last kick()\n"
      "\tuint64_t room_end = 0;  // read_count + ring size, as the producer last saw it\n"
      "\tchar pad_producer[64];\n",
      "\tuint64_t written = 0;   // the producer's copy of write_count\n"
      "\tuint64_t kicked = 0;    // write_count at the last kick()\n"
      "\tuint64_t room_end = 0;  // read_count + ring size, as the producer last saw it\n"
      "\tchar pad_private[64];   // EmuSuNi: those not on the line the command thread polls\n"
      "\tstd::atomic<uint64_t> write_count{0};\n"
      "\tchar pad_producer[64];\n")
# ParaLLEl-RSP's jumps between compiled blocks (about 56000 per GoldenEye frame) went through a call
# into C (rsp_enter -> get_jit_block): the enter thunk now reads blocks[pc] itself and only calls C for
# an empty entry. blocks[] is cleared only outside the JIT (invalidate_code at the start of run()).
patch('mupen64plus-rsp-paraLLEl/rsp_jit.cpp',
      "\t// When thunks need non-local goto, they jump here.\n"
      "\tauto *entry_label = jit_indirect();\n",
      "\t// When thunks need non-local goto, they jump here.\n"
      "\tauto *entry_label = jit_indirect();\n"
      "\n"
      "\t// EmuSuNi: a known block (blocks[pc]) is jumped to straight away, without the call into C.\n"
      "\t// JIT_R2 is free here (blocks flush their registers before they come to the thunk).\n"
      "\tstatic_assert(sizeof(Func) == 8, \"blocks[] entries are 8 bytes\");\n"
      "\tjit_andi(JIT_R2, JIT_REGISTER_NEXT_PC, IMEM_SIZE - 4);\n"
      "\tjit_lshi(JIT_R2, JIT_R2, 1); // (pc >> 2) * 8\n"
      "\tjit_addr(JIT_R2, JIT_R2, JIT_REGISTER_STATE); // the CPU object starts with its state\n"
      "\tjit_ldxi(JIT_R2, JIT_R2, offsetof(CPU, blocks));\n"
      "\tjit_node_t *emusuni_unknown = jit_beqi(JIT_R2, 0);\n"
      "\tjit_movr(JIT_REGISTER_NEXT_PC, JIT_R2);\n"
      "\tjit_node_t *emusuni_known = jit_jmpi();\n"
      "\tjit_patch(emusuni_unknown);\n")
patch('mupen64plus-rsp-paraLLEl/rsp_jit.cpp',
      "\tjit_finishi(reinterpret_cast<jit_pointer_t>(rsp_enter));\n"
      "\tjit_retval(JIT_REGISTER_NEXT_PC);\n",
      "\tjit_finishi(reinterpret_cast<jit_pointer_t>(rsp_enter));\n"
      "\tjit_retval(JIT_REGISTER_NEXT_PC);\n"
      "\tjit_patch(emusuni_known);\n")
# ParaLLEl-RSP's MFC0/MTC0 of the plain registers inline (GoldenEye: about 18000 MFC0 and 13000 MTC0
# per frame, each a call with the register window written back): RSP_MFC0 only reads, except for
# SP_STATUS (its reads count towards the CPU-wait timeout; the SP_SEMAPHORE side effect is #if 0'd),
# and RSP_MTC0 only masks and stores DMA_CACHE and DMA_DRAM. Everything else still calls C.
patch('mupen64plus-rsp-paraLLEl/rsp_jit.cpp',
      "\t\tcase 000: // MFC0\n"
      "\t\t{\n"
      "\t\t\tregs.flush_register_window(_jit);\n",
      "\t\tcase 000: // MFC0\n"
      "\t\t{\n"
      "#ifdef PARALLEL_INTEGRATION\n"
      "\t\t\t// EmuSuNi: a plain read (RSP_MFC0 has more to do only for SP_STATUS): no call.\n"
      "\t\t\tif ((rd & 15) != CP0_REGISTER_SP_STATUS)\n"
      "\t\t\t{\n"
      "\t\t\t\tif (rt != 0)\n"
      "\t\t\t\t{\n"
      "\t\t\t\t\tunsigned ptr_reg = regs.modify_mips_register(_jit, RegisterCache::SCRATCH_REGISTER0);\n"
      "\t\t\t\t\tjit_ldxi(ptr_reg, JIT_REGISTER_STATE,\n"
      "\t\t\t\t\t         offsetof(CPUState, cp0) + offsetof(CP0, cr) + (rd & 15) * sizeof(uint32_t *));\n"
      "\t\t\t\t\tunsigned rt_reg = regs.modify_mips_register(_jit, rt);\n"
      "\t\t\t\t\tjit_ldr_i(rt_reg, ptr_reg);\n"
      "\t\t\t\t\tregs.unlock_mips_register(rt);\n"
      "\t\t\t\t\tregs.unlock_mips_register(RegisterCache::SCRATCH_REGISTER0);\n"
      "\t\t\t\t}\n"
      "\t\t\t\tbreak;\n"
      "\t\t\t}\n"
      "#endif\n"
      "\t\t\tregs.flush_register_window(_jit);\n")
patch('mupen64plus-rsp-paraLLEl/rsp_jit.cpp',
      "\t\tcase 004: // MTC0\n"
      "\t\t{\n"
      "\t\t\tregs.flush_register_window(_jit);\n",
      "\t\tcase 004: // MTC0\n"
      "\t\t{\n"
      "\t\t\t// EmuSuNi: the DMA addresses are only masked and stored (RSP_MTC0): no call.\n"
      "\t\t\tif ((rd & 15) == CP0_REGISTER_DMA_CACHE || (rd & 15) == CP0_REGISTER_DMA_DRAM)\n"
      "\t\t\t{\n"
      "\t\t\t\tunsigned rt_reg = regs.load_mips_register_noext(_jit, rt);\n"
      "\t\t\t\tunsigned ptr_reg = regs.modify_mips_register(_jit, RegisterCache::SCRATCH_REGISTER0);\n"
      "\t\t\t\tunsigned val_reg = regs.modify_mips_register(_jit, RegisterCache::SCRATCH_REGISTER1);\n"
      "\t\t\t\tjit_andi(val_reg, rt_reg, (rd & 15) == CP0_REGISTER_DMA_CACHE ? 0x1fff : 0xffffff);\n"
      "\t\t\t\tjit_ldxi(ptr_reg, JIT_REGISTER_STATE,\n"
      "\t\t\t\t         offsetof(CPUState, cp0) + offsetof(CP0, cr) + (rd & 15) * sizeof(uint32_t *));\n"
      "\t\t\t\tjit_str_i(ptr_reg, val_reg);\n"
      "\t\t\t\tregs.unlock_mips_register(rt);\n"
      "\t\t\t\tregs.unlock_mips_register(RegisterCache::SCRATCH_REGISTER0);\n"
      "\t\t\t\tregs.unlock_mips_register(RegisterCache::SCRATCH_REGISTER1);\n"
      "\t\t\t\tbreak;\n"
      "\t\t\t}\n"
      "\t\t\tregs.flush_register_window(_jit);\n")
# The dynarec's JR/JALR lookup (get_addr_ht: the hash bin, then the block's entry - two dependent
# cache misses, about 9300 times per GoldenEye frame) with a direct-mapped vaddr -> code cache in
# front. It holds only answers the bins gave; an entry leaves it where it leaves the bins for good
# (remove_hash: every freed or invalidated block) and when get_addr is asked for that vaddr.
# Checked with GoldenEye, Perfect Dark and Donkey Kong 64 (TLB-mapped code): the same sound and RDRAM.
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "static struct ll_entry* hash_table[65536][2];\n",
      "static struct ll_entry* hash_table[65536][2];\n"
      "// EmuSuNi: get_addr_ht's answers, vaddr -> code (see get_addr_ht).\n"
      "static struct { u_int vaddr; void *addr; } emusuni_jump_cache[4096];\n"
      "#define EMUSUNI_JUMP_SLOT(v) (&emusuni_jump_cache[((v)>>2)&4095])\n")
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "  struct ll_entry **ht_bin=hash_table[(((vaddr)>>16)^vaddr)&0xFFFF];\n"
      "  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) {\n"
      "    ht_bin[1]=NULL;\n"
      "  }\n",
      "  struct ll_entry **ht_bin=hash_table[(((vaddr)>>16)^vaddr)&0xFFFF];\n"
      "  if(EMUSUNI_JUMP_SLOT(vaddr)->vaddr==vaddr) EMUSUNI_JUMP_SLOT(vaddr)->addr=NULL; // EmuSuNi\n"
      "  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) {\n"
      "    ht_bin[1]=NULL;\n"
      "  }\n")
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "void *get_addr(u_int vaddr)\n"
      "{\n"
      "  struct r4300_core* r4300 = &g_dev.r4300;\n"
      "  struct ll_entry *head;\n"
      "  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];\n",
      "void *get_addr(u_int vaddr)\n"
      "{\n"
      "  struct r4300_core* r4300 = &g_dev.r4300;\n"
      "  struct ll_entry *head;\n"
      "  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];\n"
      "  if(EMUSUNI_JUMP_SLOT(vaddr)->vaddr==vaddr) EMUSUNI_JUMP_SLOT(vaddr)->addr=NULL; // EmuSuNi: a newer block may come\n")
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "void *get_addr_ht(u_int vaddr)\n"
      "{\n"
      "  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];\n"
      "  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[0]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);\n"
      "  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[1]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);\n"
      "  return get_addr(vaddr);\n"
      "}\n",
      "void *get_addr_ht(u_int vaddr)\n"
      "{\n"
      "  // EmuSuNi: the cache first (one line), then the bins (the bin, then the block's entry).\n"
      "  if(EMUSUNI_JUMP_SLOT(vaddr)->vaddr==vaddr&&EMUSUNI_JUMP_SLOT(vaddr)->addr) return EMUSUNI_JUMP_SLOT(vaddr)->addr;\n"
      "  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];\n"
      "  struct ll_entry *hit=NULL;\n"
      "  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) hit=ht_bin[0];\n"
      "  else if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) hit=ht_bin[1];\n"
      "  if(hit) {\n"
      "    void *addr=(void *)(((intptr_t)hit->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);\n"
      "    EMUSUNI_JUMP_SLOT(vaddr)->vaddr=vaddr;\n"
      "    EMUSUNI_JUMP_SLOT(vaddr)->addr=addr;\n"
      "    return addr;\n"
      "  }\n"
      "  return get_addr(vaddr);\n"
      "}\n")
patch('mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c',
      "  for(n=0;n<65536;n++)\n"
      "    hash_table[n][0]=hash_table[n][1]=NULL;\n",
      "  for(n=0;n<65536;n++)\n"
      "    hash_table[n][0]=hash_table[n][1]=NULL;\n"
      "  memset(emusuni_jump_cache,0,sizeof(emusuni_jump_cache)); // EmuSuNi\n")
for path, text in patched.items():
    full = root + '/' + path
    with open(full, encoding='utf-8') as f:
        same = f.read() == text
    if not same:
        with open(full, 'w', encoding='utf-8') as f:
            f.write(text)
print("patches applied")
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-n64-core.sh: the mupen64plus core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f mupen64plus_$f"; done
} > "$OUT/prefix_mupen64plus.h"

# The PS4 target, as the toolchain file sets it for the app, except for the unwind tables (only for the
# functions that can throw, not a frame for every function) and a section for each function and variable
# (the app's link, --gc-sections, drops the ones nothing uses).
PS4="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
PS4+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -fno-unwind-tables -fno-asynchronous-unwind-tables"
PS4+=" -ffunction-sections -fdata-sections -isysroot /opt/pacbrew/ps4/openorbis"
PS4+=" -isystem $HOME/dolphinps4-build/sysroot/libc-overlay -isystem /opt/pacbrew/ps4/openorbis/include"
PS4+=" -I/opt/pacbrew/ps4/openorbis/usr/include -march=btver2"
# GL headers for GLideN64's code (never used), looked in last: Mesa's Vulkan headers are a newer
# version than the core's own, which volk and Granite were made for.
PS4+=" -idirafter $HOME/dolphinps4-build/src/mesa/include"
PS4+=" -include $OUT/prefix_mupen64plus.h"
# The core's timing of its graphics and sound tasks and its recompiler, in the log every 2 s.
PS4+=" -DPROFILE"
CXXSYS="-nostdinc++ -isystem $HOME/dolphinps4-build/sysroot/include/c++/v1"

cd "$SRC"
MAKEFLAGS_CORE="platform=unix ARCH=x86_64 STATIC_LINKING=1 HAVE_PARALLEL_RDP=1 HAVE_PARALLEL_RSP=1 HAVE_THR_AL=0 LLE=1 WITH_DYNAREC=x86_64"
STAMP="$SRC/.emusuni-flags"
WANT="$PS4 | $CXXSYS | $MAKEFLAGS_CORE | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ]; then
  echo "Mupen64Plus-Next: compiling everything (new flags, a new version of this script, or clean)"
  make clean > /dev/null 2>&1 || true
  rm -f "$STAMP" "$SRC/mupen64plus_next_libretro.a"  # make clean leaves the archive (updated in place)
fi
if ! make -j"${EMUSUNI_JOBS:-4}" $MAKEFLAGS_CORE TARGET=mupen64plus_next_libretro.a \
     CC="clang-21 $PS4" CXX="clang++-21 $CXXSYS $PS4" CC_AS="clang-21 $PS4" AR=llvm-ar-21 \
     NASM=nasm STRINGS=strings GIT_VERSION=" $(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
     > "$LOG" 2>&1; then
  grep -E "error:|Error [0-9]" "$LOG" | head -30
  echo "Mupen64Plus-Next: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: the libretro API (renamed) and ParaLLEl-RDP's device creation stay global.
# (GNU ld -r -d: the core is built with -fcommon; its common variables get their space here,
# else they couldn't be made private.)
ld.bfd -r -d --whole-archive "$SRC/mupen64plus_next_libretro.a" -o "$OUT/n64_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='mupen64plus_retro_*' \
  --keep-global-symbol=parallel_create_device "$OUT/n64_core_all.o" "$OUT/n64_core.o"
rm -f "$OUT/n64_core_all.o"
ls -la "$OUT/n64_core.o"
