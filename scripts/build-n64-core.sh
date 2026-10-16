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
#   - The dynarec's code cache (an array in the core's data) is made executable through EmuSuNi
#     (port/ps4_n64.c), the ways the PS4 allows.
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
      "#elif defined(__PS4__)\n  {\n    // EmuSuNi (port/ps4_n64.c): the ways the PS4 allows code made at run time.\n"
      "    extern int emusuni_make_executable(void *address, size_t size);\n"
      "    if (emusuni_make_executable(g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2) < 0)\n"
      "      DebugMessage(M64MSG_ERROR, \"dynarec: the code cache can't be made executable\");\n  }\n"
      "  base_addr = g_dev.r4300.extra_memory;\n  base_addr_rx = base_addr;\n"
      "#else\n  mprotect ((u_char *)g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2,\n"
      "            PROT_READ | PROT_WRITE | PROT_EXEC);\n  base_addr = g_dev.r4300.extra_memory;\n"
      "  base_addr_rx = base_addr;\n#endif\n#endif\n#endif")
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
      "\tfrontend->begin_frame_context();\n}\n",
      "#ifdef __PS4__\n"
      "\tconst auto emusuni_wait_start = std::chrono::steady_clock::now();\n"
      "#endif\n"
      "\tfrontend->begin_frame_context();\n"
      "#ifdef __PS4__\n"
      "\temusuni_wait_ms += emusuni_ms_since(emusuni_wait_start);\n"
      "\tif (++emusuni_timing_frames == 600)\n"
      "\t{\n"
      "\t\tif (log_cb)\n"
      "\t\t\tlog_cb(RETRO_LOG_INFO, \"EmuSuNi: RDP per frame: scanout %.2f ms, waiting for a free frame (GPU) %.2f ms.\\n\",\n"
      "\t\t\t       emusuni_scanout_ms / 600.0, emusuni_wait_ms / 600.0);\n"
      "\t\temusuni_timing_frames = 0;\n"
      "\t\temusuni_scanout_ms = emusuni_wait_ms = 0.0;\n"
      "\t}\n"
      "#endif\n"
      "}\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "#include \"thread_id.hpp\"\n#include <assert.h>\n",
      "#include \"thread_id.hpp\"\n#include <assert.h>\n"
      "#ifdef __PS4__\nextern \"C\" void emusuni_core_thread(int role);\n#endif\n")
patch('mupen64plus-video-paraLLEl/parallel-rdp/parallel-rdp/command_ring.cpp',
      "void CommandRing::thread_loop()\n{\n\tUtil::register_thread_index(0);\n",
      "void CommandRing::thread_loop()\n{\n\tUtil::register_thread_index(0);\n"
      "#ifdef __PS4__\n"
      "\t// EmuSuNi: a core of its own, as high as the emulation thread (port/ps4_n64.c): the\n"
      "\t// emulation waits for this thread to drain the ring every frame (scanout).\n"
      "\temusuni_core_thread(1);\n"
      "#endif\n")
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

# The PS4 target, as the toolchain file sets it for the app.
PS4="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
PS4+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -funwind-tables -isysroot /opt/pacbrew/ps4/openorbis"
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
if ! make -j"$(nproc)" $MAKEFLAGS_CORE TARGET=mupen64plus_next_libretro.a \
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
