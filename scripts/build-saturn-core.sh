#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds YabaSanshiro 3.4.2 (libretro/yabause, branch yabasanshiro: ~/src/yabasanshiro, GPL-2.0-or-later) for the
# PS4 into one object that scripts/build.sh links into EmuSuNi (with-saturn): the Sega Saturn, experimental (00.68),
# with its software renderer (vidsoft) drawing on threads of its own. Its libretro build's sources
# (libretro/Makefile.common) in a CMake of its own made here, compiled with the PS4 toolchain
# (toolchain/ps4-love-modern.cmake) and the defines of its unix build: the SH-2 interpreter (no SH-2 recompiler
# exists for x86-64), Musashi for the 68000, the new SCSP on its own thread, libchdr, libretro-common's own file
# access (the app answers no VFS interface), -O3 -ffast-math as upstream. Not as its libretro build:
#   - Its software renderer (vidsoft and its compositor titan), not the OpenGL one (vidogl, ygl*, glsm: the PS4 has no
#     OpenGL without Mesa's Zink, which isn't built; EMUSUNI_NO_GL), its pictures RGB565 (the app's own format). The
#     PC measured the renderer's work at 2.2 times the emulation's own (Daytona USA's demo race: the emulation 2.1 ms a
#     frame, the VDP1's drawing 1.9, the VDP2's 3.0), so it runs on three threads of its own beside the emulation
#     thread (the soft_threads files below): the VDP1's draws its command list into the back frame buffer from a copy
#     of the VDP1's memory and registers taken where upstream draws (Vdp1Draw), while the emulation thread walks the
#     same list without drawing (the registers and status exactly as when it draws); the VDP2's and its helper draw
#     the screens, the sprite layer and titan's composition in two bands of lines from a copy of the VDP2's state at
#     VBlank OUT (where upstream draws them), and make the picture. The emulation thread waits for the VDP1's drawing
#     only where it sees it (the CPUs' reads and writes of the drawn buffer, the next drawing, the erase of a buffer
#     still drawn): at the frame buffers' change the picture waits for it instead, on the VDP2's threads, which draw
#     the scroll screens meanwhile and the sprite layer from the drawn buffer itself after it (Sonic R starts each
#     drawing just before the change: the emulation waited for all of it). It waits for the VDP2's only when a
#     picture takes longer than a frame: at VBlank IN it shows the frame's picture if done, else the one before.
#     Upstream's own threads (vidsoft's seven, titan's five: they wait in pause() for a wake that never comes and
#     spin on flags) are never started.
#   - The VDP1's erase and frame buffer change as YabaSanshiro's VBlank OUT does them for its GL renderer
#     (Vdp1EraseWrite, Vdp1FrameChange: vidsoft had them empty and changed the buffers itself at VBlank IN, which a
#     manual change - Daytona USA's - never reached: no 3D); vidsoft's colour RAM reads its own, of the copy it draws
#     from (it read the GL renderer's, the colour RAM's address passed as an alpha).
#   - The VDP1's registers zeroed when they are made (calloc): upstream's Vdp1Reset clears only sizeof(Vdp1Regs) (a
#     pointer: 8 bytes), so EDSR, LOPR, COPR... began as whatever the heap held, which the games read at their start
#     (Daytona USA then never drew its 3D).
#   - Frame skip off by default: on (the core's default), the emulation thread busy-waits to 60 frames a second.
#   - The fast paths the PC measurement found (identical pictures and cycle counts; Daytona USA -17%, Sonic R -9% on
#     the PC): the SH-2s' accesses to their high and low work RAM go straight to it in the six memory accessors, the
#     same cycles counted, and the interpreter fetches its instructions from the high work RAM without the fetch
#     table's call.
#   - The SCSP thread (the 68000 and the sound chip, beside the SH-2s) waits for the SH-2s' cycles by calling the app
#     (emusuni_saturn_scsp_idle: a sleep of some microseconds) where it spun on them, and tells the app at the end of
#     each frame (emusuni_saturn_scsp_frame: its CPU time). Spinning, it took a core whole.
#   - Its threads start through the app (emusuni_saturn_thread: their core and priority on the PS4 - "scsp", "vdp1",
#     "vdp2", "vdp2b"), not Linux's affinity system calls; the renderer's threads end with the game (joined).
#   - The static tables of 64 KiB or more on the heap while the core runs (the PS4 charges the program's whole
#     static memory to the games' flexible memory from the app's start): the SH-2's 64K instruction handlers
#     (512 KiB), Musashi's 64K handlers and cycle counts (704 KiB), the CD audio buffer (345 KiB), the VDP2's
#     line registers and cell scroll tables (170 KiB), the zip reader's buffer (64 KiB), vidsoft's sprite window mask
#     (352 KiB) and mosaic table (64 KiB); vidsoft's thread copies (1.4 MiB) never made. What the renderer makes it
#     frees at the game's end (titan kept its buffers, and its next start used them freed).
#   - The backup RAM file read into the heap and written back when the game ends (the PS4 maps no files).
#   - The BIOS the app found, by its path (EMUSUNI_SATURN_BIOS: the owner's file keeps its own name).
#   - 00.69 (the owner: "anda mal el rendimiento, a veces va a tirones"), each below where it is made: the SH-2s'
#     waits counted, not run (sh2int.c, memory.c); the VDP1's drawing as one inlined path, in two threads by bands of
#     rows ("vdp1", "vdp1b"); the VDP2's layers erased after their composition band by band, the back screen's lines
#     filled only when they change, the window tests answered first when the windows are off, the scroll screens'
#     pixel stores inlined; only the VDP1's and VDP2's memory pages written since the last copy copied; the picture
#     shown "now" or "a frame later" with hysteresis instead of whichever was done; the renderer's threads idle before
#     the game ends, a state is saved or loaded, a reset; the renderer without -ffast-math; PGO
#     (scripts/saturn-core.profdata.xz). The pictures checked on the PC against 00.68's renderer, every one of both
#     games' runs, in a deterministic test mode (the emulation waiting for the 68000's thread at every tenth of a line).
# Its libretro API becomes yabasanshiro_retro_* and everything else in it private (CMakeLists.txt's FCEUmm way).
# The object is checked at the end: only that API defined globally, no weak references, no operator new/delete of
# its own, no static array of 64 KiB or more, every symbol it needs in the PS4 libraries or the app.
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-saturn-core.sh <out dir> [clean]
#   SATURN_CORE_HOST=1 build-saturn-core.sh <out dir>   the same object for this PC (Linux, clang 21), for the PC
#                                                       checks: no PS4 checks
#   SATURN_RENDERER=none    no renderer (00.64's speed test: VIDDummy, no picture); soft (the default): the above
#   SATURN_PROF=1           (with SATURN_CORE_HOST=1) timers around the renderer's frame parts, for the PC measurement
#   SATURN_CORE_PGO=off|generate  without the profile; instrumented to make one (PC: SATURN_CORE_HOST=1)
#   EMUSUNI_SATURN_THREADS=0 (the environment, as the core runs) the renderer on the emulation thread, as upstream;
#   EMUSUNI_SATURN_WAIT=1    always the frame's own picture (the PC's picture check)
#   EMUSUNI_SATURN_VDP1_BANDS=1  the VDP1's drawing on one thread
#   EMUSUNI_SATURN_SH2_SKIP=0    every turn of the SH-2s' waits run (the app sets it: /data/EmuSuNi/saturn-sh2-skip-off)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${YABASANSHIRO_DIR:-$HOME/src/yabasanshiro}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/saturn-core.log"
HOST="${SATURN_CORE_HOST:-0}"

python3 -I - "$SRC" <<'PATCH'
import subprocess
import sys
root = sys.argv[1]
Y = 'yabause/src/'
patched = {}
def patch(path, old, new, count=1):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    path = Y + path
    if path not in patched:
        patched[path] = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + path], check=True,
                                       capture_output=True).stdout.decode('utf-8')
    text = patched[path]
    assert text.count(old) == count, (path, text.count(old), old[:80])
    patched[path] = text.replace(old, new)

# --- libretro.c: no renderer, the console started at load, frame skip off, the app's BIOS ---
patch('libretro/libretro.c',
      '#include "m68kcore.h"\n#include "vidogl.h"\n#include "vidsoft.h"\n#include "ygl.h"\n',
      '#include "m68kcore.h"\n'
      '#ifdef EMUSUNI_NO_GL\n'
      '/* EmuSuNi: no renderer (VIDDummy): the GL renderer\'s headers (and the GL headers) left out; the few values of\n'
      '   theirs this file uses are in emusuni_saturn_nogl.h (scripts/build-saturn-core.sh). */\n'
      '#include "osdcore.h"\n'
      '#include "emusuni_saturn_nogl.h"\n'
      '#ifdef EMUSUNI_SOFT\n'
      '#include "vidsoft.h" /* EmuSuNi: the software renderer (its pictures: emusuni_soft_frame) */\n'
      '#endif\n'
      '#else\n'
      '#include "vidogl.h"\n#include "vidsoft.h"\n#include "ygl.h"\n'
      '#endif\n')
patch('libretro/libretro.c',
      '   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;\n',
      '#ifdef EMUSUNI_SOFT\n'
      '   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565; /* EmuSuNi: the software renderer\'s pictures */\n'
      '#else\n'
      '   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;\n'
      '#endif\n')
patch('libretro/libretro.c',
      'static int g_frame_skip = 1;\n',
      '/* EmuSuNi: frame skip off by default: on, the emulation thread busy-waits to 60 frames a second. */\n'
      'static int g_frame_skip = 0;\n')
patch('libretro/libretro.c',
      '{ "yabasanshiro_frameskip", "Auto-frameskip; enabled|disabled" },',
      '{ "yabasanshiro_frameskip", "Auto-frameskip; disabled|enabled" },')
patch('libretro/libretro.c',
      '#if defined(_USEGLEW_)\nstatic struct retro_hw_render_callback hw_render;\n#else\n',
      '#if defined(_USEGLEW_) || defined(EMUSUNI_NO_GL)\nstatic struct retro_hw_render_callback hw_render;\n#else\n')
patch('libretro/libretro.c',
      'VideoInterface_struct *VIDCoreList[] = {\n    //&VIDDummy,\n    &VIDOGL,\n    &VIDSoft,\n    NULL\n};\n',
      'VideoInterface_struct *VIDCoreList[] = {\n'
      '#if defined(EMUSUNI_SOFT)\n'
      '    &VIDSoft, /* EmuSuNi: the software renderer */\n'
      '#elif defined(EMUSUNI_NO_GL)\n'
      '    &VIDDummy, /* EmuSuNi: no renderer */\n'
      '#else\n'
      '    //&VIDDummy,\n    &VIDOGL,\n    &VIDSoft,\n'
      '#endif\n'
      '    NULL\n};\n')
for what in ('BIND', 'UNBIND'):
    patch('libretro/libretro.c',
          '#if !defined(_USEGLEW_)\n  return glsm_ctl(GLSM_CTL_STATE_%s, NULL);\n#endif\n}\n' % what,
          '#if !defined(_USEGLEW_) && !defined(EMUSUNI_NO_GL)\n  return glsm_ctl(GLSM_CTL_STATE_%s, NULL);\n'
          '#else\n  return 0;\n#endif\n}\n' % what)
patch('libretro/libretro.c',
      'int YuiGetFB(void)\n{\n  return hw_render.get_current_framebuffer();\n}\n',
      'int YuiGetFB(void)\n{\n#ifdef EMUSUNI_NO_GL\n  return 0;\n#else\n'
      '  return hw_render.get_current_framebuffer();\n#endif\n}\n')
patch('libretro/libretro.c',
      'void YuiSwapBuffers(void)\n{\n',
      'void YuiSwapBuffers(void)\n{\n'
      '#if defined(EMUSUNI_SOFT)\n'
      '   /* EmuSuNi: the software renderer\'s newest picture (RGB565: emusuni_soft_threads.h) to the frontend, once a\n'
      '      frame (its Vdp2DrawEnd; none yet: a duplicate), and the frame\'s room for sound to the sound thread, as the\n'
      '      GL renderer\'s YuiSwapBuffers gives it. */\n'
      '   audio_size = soundlen;\n'
      '   video_cb(emusuni_soft_frame, emusuni_soft_w, emusuni_soft_h, emusuni_soft_w * 2);\n'
      '   one_frame_rendered = true;\n'
      '   return;\n'
      '#elif defined(EMUSUNI_NO_GL)\n'
      '   /* EmuSuNi: no picture. Called once a frame by VIDDummyVdp2DrawEnd, as YglRender calls it: the frame\'s room\n'
      '      for sound given to the sound thread as with the GL renderer, a duplicated frame to the frontend. */\n'
      '   audio_size = soundlen;\n'
      '   video_cb(NULL, current_width, current_height, 0);\n'
      '   one_frame_rendered = true;\n'
      '   return;\n'
      '#endif\n')
patch('libretro/libretro.c',
      '#if !defined(_USEGLEW_)\n   glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);\n',
      '#if !defined(_USEGLEW_) && !defined(EMUSUNI_NO_GL)\n   glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);\n')
patch('libretro/libretro.c',
      '#if !defined(_USEGLEW_)\n   glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);\n',
      '#if !defined(_USEGLEW_) && !defined(EMUSUNI_NO_GL)\n   glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);\n')
patch('libretro/libretro.c',
      'static bool retro_init_hw_context(void)\n{\n#if defined(_USEGLEW_)\n',
      'static bool retro_init_hw_context(void)\n{\n'
      '#if defined(EMUSUNI_NO_GL)\n'
      '   return true; /* EmuSuNi: no renderer, no context */\n'
      '#elif defined(_USEGLEW_)\n')
patch('libretro/libretro.c',
      '   yinit.vidcoretype               = VIDCORE_OGL;\n',
      '#if defined(EMUSUNI_SOFT)\n'
      '   yinit.vidcoretype               = VIDCORE_SOFT;\n'
      '#elif defined(EMUSUNI_NO_GL)\n'
      '   yinit.vidcoretype               = VIDCORE_DUMMY;\n'
      '#else\n'
      '   yinit.vidcoretype               = VIDCORE_OGL;\n'
      '#endif\n')
patch('libretro/libretro.c',
      '   yinit.video_filter_type         = 0;\n\n   return true;\n}\n',
      '   yinit.video_filter_type         = 0;\n\n'
      '#ifdef EMUSUNI_NO_GL\n'
      '   /* EmuSuNi: the console starts here (the GL build starts it in context_reset, once its context exists). */\n'
      '   game_width = current_width = 320;\n'
      '   game_height = current_height = 240;\n'
      '   first_ctx_reset = 0;\n'
      '   if (YabauseInit(&yinit) != 0)\n'
      '   {\n'
      '      log_cb(RETRO_LOG_ERROR, "YabauseInit failed\\n");\n'
      '      return false;\n'
      '   }\n'
      '   renderer_running = true;\n'
      '   OSDChangeCore(OSDCORE_DUMMY);\n'
      '#endif\n'
      '   return true;\n}\n')
patch('libretro/libretro.c',
      '   // Real bios is REQUIRED, even if we support HLE bios\n',
      '#ifdef EMUSUNI_PORT\n'
      '   /* EmuSuNi: the BIOS the app found, when it says one (the owner\'s file keeps its own name). */\n'
      '   if (getenv("EMUSUNI_SATURN_BIOS") && *getenv("EMUSUNI_SATURN_BIOS"))\n'
      '      snprintf(bios_path, sizeof(bios_path), "%s", getenv("EMUSUNI_SATURN_BIOS"));\n'
      '#endif\n'
      '   // Real bios is REQUIRED, even if we support HLE bios\n')
patch('libretro/libretro.c',
      'void reset_global_gl_state()\n{\n',
      '#ifndef EMUSUNI_NO_GL\nvoid reset_global_gl_state()\n{\n')
# (00.77) RetroAchievements: the console's work RAM shown to the frontend as Beetle Saturn shows it to RetroArch, so
# rcheevos finds its two Saturn regions - Work RAM Low ($000000-$0FFFFF, the SH-2's 0x00200000) and Work RAM High
# ($100000-$1FFFFF, 0x06000000) - by those addresses in the map; 1 MiB each, both in 16-bit words of the host's order
# (memory.h's T2ReadByte: address ^ 1), as Beetle's WorkRAM. Until 00.76 the core showed no memory at all
# (retro_get_memory_data's NULL) and a Saturn game had no achievements.
patch('libretro/libretro.c',
      '   renderer_running = true;\n'
      '   OSDChangeCore(OSDCORE_DUMMY);\n'
      '#endif\n'
      '   return true;\n}\n',
      '   renderer_running = true;\n'
      '   OSDChangeCore(OSDCORE_DUMMY);\n'
      '   {\n'
      '      /* EmuSuNi: the work RAM for RetroAchievements (YabauseInit has made it). */\n'
      '      extern u8 *LowWram;\n'
      '      extern u8 *HighWram;\n'
      '      static struct retro_memory_descriptor emusuni_ram[2];\n'
      '      static struct retro_memory_map emusuni_ram_map = { emusuni_ram, 2 };\n'
      '      emusuni_ram[0].flags = RETRO_MEMDESC_SYSTEM_RAM;\n'
      '      emusuni_ram[0].ptr = LowWram;\n'
      '      emusuni_ram[0].start = 0x00200000;\n'
      '      emusuni_ram[0].len = 0x100000;\n'
      '      emusuni_ram[1].flags = RETRO_MEMDESC_SYSTEM_RAM;\n'
      '      emusuni_ram[1].ptr = HighWram;\n'
      '      emusuni_ram[1].start = 0x06000000;\n'
      '      emusuni_ram[1].len = 0x100000;\n'
      '      if (LowWram && HighWram)\n'
      '         environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &emusuni_ram_map);\n'
      '   }\n'
      '#endif\n'
      '   return true;\n}\n')
# (00.69) The software renderer's threads done with their drawing before the core frees, loads, saves or resets what
# they read: the game's end freed the VDP2's memory while its thread still drew the last picture (the PC's slow
# instrumented build crashed there; on the PS4 a picture takes 13 ms).
patch('libretro/libretro.c',
      'bool retro_load_game(const struct retro_game_info *info)\n{\n',
      'bool retro_load_game(const struct retro_game_info *info)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '   { extern int emusuni_sh2_skip; emusuni_sh2_skip = -1; } /* EmuSuNi: EMUSUNI_SATURN_SH2_SKIP read for this game */\n'
      '#endif\n')
for head in ('void retro_unload_game(void)\n{\n', 'bool retro_serialize(void *data, size_t size)\n{\n',
             'bool retro_unserialize(const void *data, size_t size)\n{\n', 'void retro_reset(void)\n{\n'):
    patch('libretro/libretro.c', head,
          head + '#ifdef EMUSUNI_SOFT\n   emusuni_soft_quiesce(); /* EmuSuNi: the renderer\'s threads idle first */\n#endif\n')
patch('libretro/libretro.c',
      '\n}\n\nvoid retro_run(void)\n',
      '\n}\n#endif\n\nvoid retro_run(void)\n')

# --- vdp1.cpp: VIDDummy's end of frame as the GL renderer's (YuiSwapBuffers); its frame buffer reads from memory ---
patch('vdp1.cpp',
      '\tVIDDummVdp1ReadFrameBuffer,\n\tVIDDummVdp1WriteFrameBuffer,\n',
      '#ifdef EMUSUNI_NO_GL\n'
      '\t/* EmuSuNi: the VDP1 frame buffer\'s memory itself (Vdp1FrameBufferRead* left the value undefined) */\n'
      '\tNULL,\n\tNULL,\n'
      '#else\n'
      '\tVIDDummVdp1ReadFrameBuffer,\n\tVIDDummVdp1WriteFrameBuffer,\n'
      '#endif\n')
patch('vdp1.cpp',
      '   if ((Vdp1Regs = (Vdp1 *) malloc(sizeof(Vdp1))) == NULL)\n',
      '   /* EmuSuNi: zeroed - Vdp1Reset clears only sizeof(Vdp1Regs), a pointer\'s 8 bytes, so EDSR, LOPR, COPR... began as\n'
      '      whatever the heap held, which the games read at their start (Daytona USA then never drew its 3D) */\n'
      '   if ((Vdp1Regs = (Vdp1 *) calloc(1, sizeof(Vdp1))) == NULL)\n')
patch('vdp1.cpp',
      'void VIDDummyVdp1DrawStart(void)\n{\n}\n',
      'void VIDDummyVdp1DrawStart(void)\n{\n'
      '#ifdef EMUSUNI_NO_GL\n'
      '   /* EmuSuNi: the command list walked as the renderers walk it as they start drawing (vidogl, vidsoft: Vdp1Draw\n'
      '      leaves it to them). Its end makes the VDP1 idle, then its draw end interrupt comes: without it the games\n'
      '      wait at their title screens (Daytona USA, Sonic R never start their demo). The dummy draws nothing. */\n'
      '   Vdp1DrawCommands(Vdp1Ram, Vdp1Regs, NULL);\n'
      '#endif\n}\n')
patch('vdp1.cpp',
      'void VIDDummyVdp2DrawEnd(void)\n{\n}\n',
      'void VIDDummyVdp2DrawEnd(void)\n{\n'
      '#ifdef EMUSUNI_NO_GL\n'
      '   YuiSwapBuffers(); /* EmuSuNi: once a frame, as the GL renderer\'s YglRender (the sound thread\'s room) */\n'
      '#endif\n}\n')

# (00.69) The VDP1's and the VDP2's memory: the 4 KiB pages written since the renderer's threads last took their copy
# (emusuni_vdp1_dirty, emusuni_vdp2_dirty: every write goes through these handlers - the SCU's DMA too, its direct
# OPTIMIZED_DMA path isn't built -, a state's load and the VDP1's reset mark every page), so a drawing's or a picture's
# copy takes only those (vidsoft.c's threads: emu_copy_pages) instead of 512 KiB each time (0.3-0.4 ms a frame of the
# emulation thread on the PS4 in 00.68's log).
patch('vdp1.cpp',
      'extern "C" void FASTCALL Vdp1RamWriteByte(u32 addr, u8 val) {\n   addr &= 0x7FFFF;\n',
      '/* EmuSuNi: the pages written since the renderer\'s copy (vidsoft.c\'s threads) */\n'
      'extern "C" { unsigned char emusuni_vdp1_dirty[128]; }\n\n'
      'extern "C" void FASTCALL Vdp1RamWriteByte(u32 addr, u8 val) {\n   addr &= 0x7FFFF;\n'
      '   emusuni_vdp1_dirty[addr >> 12] = 1;\n')
patch('vdp1.cpp',
      'extern "C" void FASTCALL Vdp1RamWriteWord(u32 addr, u16 val) {\n   addr &= 0x7FFFF;\n',
      'extern "C" void FASTCALL Vdp1RamWriteWord(u32 addr, u16 val) {\n   addr &= 0x7FFFF;\n'
      '   emusuni_vdp1_dirty[addr >> 12] = 1;\n')
patch('vdp1.cpp',
      'extern "C" void FASTCALL Vdp1RamWriteLong(u32 addr, u32 val) {\n   addr &= 0x7FFFF;\n',
      'extern "C" void FASTCALL Vdp1RamWriteLong(u32 addr, u32 val) {\n   addr &= 0x7FFFF;\n'
      '   emusuni_vdp1_dirty[addr >> 12] = 1;\n')
patch('vdp1.cpp',
      '   T1WriteWord(Vdp1Ram, 0x40000, 0x8000);\n',
      '   T1WriteWord(Vdp1Ram, 0x40000, 0x8000);\n'
      '   memset(emusuni_vdp1_dirty, 1, sizeof(emusuni_vdp1_dirty)); /* EmuSuNi: written here directly */\n')
patch('vdp1.cpp',
      '   // Read VDP1 ram\n   yread(&check, (void *)Vdp1Ram, 0x80000, 1, fp);\n',
      '   // Read VDP1 ram\n   yread(&check, (void *)Vdp1Ram, 0x80000, 1, fp);\n'
      '   memset(emusuni_vdp1_dirty, 1, sizeof(emusuni_vdp1_dirty)); /* EmuSuNi: all of it new */\n')
patch('vdp2.cpp',
      'void FASTCALL Vdp2RamWriteByte(u32 addr, u8 val) {\n   addr &= 0x7FFFF;\n',
      '/* EmuSuNi: the pages written since the renderer\'s copy (vidsoft.c\'s threads) */\n'
      'extern "C" { unsigned char emusuni_vdp2_dirty[128]; }\n\n'
      'void FASTCALL Vdp2RamWriteByte(u32 addr, u8 val) {\n   addr &= 0x7FFFF;\n'
      '   emusuni_vdp2_dirty[addr >> 12] = 1;\n')
patch('vdp2.cpp',
      'void FASTCALL Vdp2RamWriteWord(u32 addr, u16 val) {\n   addr &= 0x7FFFF;\n',
      'void FASTCALL Vdp2RamWriteWord(u32 addr, u16 val) {\n   addr &= 0x7FFFF;\n'
      '   emusuni_vdp2_dirty[addr >> 12] = 1;\n')
patch('vdp2.cpp',
      'void FASTCALL Vdp2RamWriteLong(u32 addr, u32 val) {\n  addr &= 0x7FFFF;\n',
      'void FASTCALL Vdp2RamWriteLong(u32 addr, u32 val) {\n  addr &= 0x7FFFF;\n'
      '  emusuni_vdp2_dirty[addr >> 12] = 1;\n')
patch('vdp2.cpp',
      '   // Read VDP2 ram\n   yread(&check, (void *)Vdp2Ram, 0x80000, 1, fp);\n',
      '   // Read VDP2 ram\n   yread(&check, (void *)Vdp2Ram, 0x80000, 1, fp);\n'
      '   memset(emusuni_vdp2_dirty, 1, sizeof(emusuni_vdp2_dirty)); /* EmuSuNi: all of it new */\n')

# --- vdp2.cpp, vdp2.h: the line registers and cell scroll tables on the heap ---
patch('vdp2.cpp',
      'struct CellScrollData cell_scroll_data[270];\nVdp2 Vdp2Lines[270];\n',
      '/* EmuSuNi: 270 of each on the heap (Vdp2Init), not in the program\'s static memory (170 KiB) */\n'
      'struct CellScrollData *cell_scroll_data = NULL;\nVdp2 *Vdp2Lines = NULL;\n')
patch('vdp2.cpp',
      '   memset(Vdp2Lines, 0, sizeof(Vdp2) * 270);\n',
      '   if (Vdp2Lines == NULL && (Vdp2Lines = (Vdp2 *)calloc(270, sizeof(Vdp2))) == NULL)\n'
      '      return -1;\n'
      '   if (cell_scroll_data == NULL &&\n'
      '       (cell_scroll_data = (struct CellScrollData *)calloc(270, sizeof(struct CellScrollData))) == NULL)\n'
      '      return -1;\n'
      '   memset(Vdp2Lines, 0, sizeof(Vdp2) * 270);\n')
# (Frame skip's end: no renderer's functions to put back - VIDCore is VIDDummy, and frame skip is off.)
patch('vdp2.cpp',
      '      if (VIDCore->id == VIDCORE_OGL) {\n',
      '#ifndef EMUSUNI_NO_GL\n      if (VIDCore->id == VIDCORE_OGL) {\n')
patch('vdp2.cpp',
      '        VIDCore->Vdp2DrawScreens = VIDSoftVdp2DrawScreens;\n      }\n',
      '        VIDCore->Vdp2DrawScreens = VIDSoftVdp2DrawScreens;\n      }\n#endif\n')
patch('vdp2.h', 'extern Vdp2 Vdp2Lines[270];', 'extern Vdp2 *Vdp2Lines; /* EmuSuNi: 270 of them, on the heap */')
patch('vdp2.h', 'extern struct CellScrollData cell_scroll_data[270];',
      'extern struct CellScrollData *cell_scroll_data; /* EmuSuNi: 270, on the heap */')

# --- scsp.c: the CD audio buffer on the heap; the sound thread waits through the app, reports each frame ---
patch('scsp.c',
      'static union {\n   u8 data[CDDA_NUM_BUFFERS*2352];\n} cddabuf;\n',
      'static union emusuni_cddabuf {\n   u8 data[CDDA_NUM_BUFFERS*2352];\n'
      '} *emusuni_cddabuf; /* EmuSuNi: 345 KiB on the heap (ScspInit), not in the program\'s static memory */\n'
      '#define cddabuf (*emusuni_cddabuf)\n')
patch('scsp.c',
      '  if ((SoundRam = T2MemoryInit (0x80000)) == NULL)\n    return -1;\n\n'
      '  if ((ScspInternalVars = (ScspInternal *)calloc(1, sizeof(ScspInternal))) == NULL)\n',
      '  if ((SoundRam = T2MemoryInit (0x80000)) == NULL)\n    return -1;\n\n'
      '  if (emusuni_cddabuf == NULL &&\n'
      '      (emusuni_cddabuf = (union emusuni_cddabuf *)calloc(1, sizeof(union emusuni_cddabuf))) == NULL)\n'
      '    return -1;\n\n'
      '  if ((ScspInternalVars = (ScspInternal *)calloc(1, sizeof(ScspInternal))) == NULL)\n')
patch('scsp.c',
      '      m68k_cycle = m68k_integer_part - pre_m68k_cycle;\n'
      '      if (thread_running == 0) break;\n'
      '    } while (m68k_cycle == 0);\n',
      '      m68k_cycle = m68k_integer_part - pre_m68k_cycle;\n'
      '      if (thread_running == 0) break;\n'
      '#ifdef EMUSUNI_PORT\n'
      '      /* EmuSuNi: no new cycles from the SH-2s yet: the app\'s short sleep, where it spun (a whole core) */\n'
      '      if (m68k_cycle == 0) emusuni_saturn_scsp_idle();\n'
      '#endif\n'
      '    } while (m68k_cycle == 0);\n')
patch('scsp.c',
      '        ScspInternalVars->scsptiming1 = scsplines;\n'
      '        ScspExecAsync();\n\n'
      '        YabAddEventQueue( q_scsp_finish , 0);\n',
      '        ScspInternalVars->scsptiming1 = scsplines;\n'
      '        ScspExecAsync();\n'
      '#ifdef EMUSUNI_PORT\n'
      '        emusuni_saturn_scsp_frame(); /* EmuSuNi: the frame\'s sound done (the app reads this thread\'s CPU time) */\n'
      '#endif\n\n'
      '        YabAddEventQueue( q_scsp_finish , 0);\n')

# --- thr-rthreads.c: threads start through the app; no Linux affinity calls ---
patch('thr-rthreads.c',
      '#ifndef _WIN32\n#include <sched.h>\n#include <unistd.h>\n#include <sys/syscall.h>\n#include <errno.h>\n#endif\n',
      '#if defined(EMUSUNI_PORT)\n#include <sched.h>\n#include <unistd.h>\n#include <stdlib.h>\n'
      '#elif !defined(_WIN32)\n#include <sched.h>\n#include <unistd.h>\n#include <sys/syscall.h>\n#include <errno.h>\n#endif\n')
patch('thr-rthreads.c',
      '//////////////////////////////////////////////////////////////////////////////\n\n'
      'int YabThreadStart(unsigned int id, void * (*func)(void *), void *arg)\n',
      '//////////////////////////////////////////////////////////////////////////////\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '/* EmuSuNi: each thread first tells the app (emusuni_saturn_thread: its core and priority on the PS4). */\n'
      'struct emusuni_yab_start\n{\n\tvoid * (*func)(void *);\n\tvoid *arg;\n\tunsigned int id;\n};\n'
      'static void emusuni_yab_thread(void *p)\n{\n'
      '\tstruct emusuni_yab_start start = *(struct emusuni_yab_start *)p;\n'
      '\tfree(p);\n'
      '\temusuni_saturn_thread(start.id == YAB_THREAD_SCSP ? "scsp" :\n'
      '\t                      start.id == YAB_THREAD_VIDSOFT_VDP1 ? "vdp1" : /* the software renderer\'s */\n'
      '\t                      start.id == YAB_THREAD_VIDSOFT_LAYER_NBG0 ? "vdp2" :\n'
      '\t                      start.id == YAB_THREAD_VIDSOFT_LAYER_NBG1 ? "vdp2b" :\n'
      '\t                      start.id == YAB_THREAD_VIDSOFT_LAYER_NBG2 ? "vdp1b" : "other");\n'
      '\tstart.func(start.arg);\n}\n'
      '#endif\n\n'
      'int YabThreadStart(unsigned int id, void * (*func)(void *), void *arg)\n')
patch('thr-rthreads.c',
      '\tif ((thread_handle[id].thd = sthread_create((void *)func, arg)) == NULL)\n'
      '\t{\n\t\tperror("CreateThread");\n\t\treturn -1;\n\t}\n',
      '#ifdef EMUSUNI_PORT\n'
      '\tstruct emusuni_yab_start *start = (struct emusuni_yab_start *)malloc(sizeof(*start));\n'
      '\tif (start == NULL)\n\t\treturn -1;\n'
      '\tstart->func = func;\n\tstart->arg = arg;\n\tstart->id = id;\n'
      '\tif ((thread_handle[id].thd = sthread_create(emusuni_yab_thread, start)) == NULL)\n'
      '\t{\n\t\tfree(start);\n\t\tperror("CreateThread");\n\t\treturn -1;\n\t}\n'
      '#else\n'
      '\tif ((thread_handle[id].thd = sthread_create((void *)func, arg)) == NULL)\n'
      '\t{\n\t\tperror("CreateThread");\n\t\treturn -1;\n\t}\n'
      '#endif\n')
patch('thr-rthreads.c',
      '#elif !defined(ANDROID) && !defined(__APPLE__) // it needs more than android-21\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '\t(void)mask; /* EmuSuNi: placed by the app as it started (emusuni_saturn_thread) */\n'
      '#elif !defined(ANDROID) && !defined(__APPLE__) // it needs more than android-21\n')

# --- sh2int.c, sh2int.h: the handler table on the heap; instructions fetched from the high work RAM directly ---
patch('sh2int.c',
      'opcodefunc opcodes[0x10000];\n',
      '/* EmuSuNi: the 64K handlers (512 KiB) on the heap (SH2InterpreterInit), not in the program\'s static memory */\n'
      'opcodefunc *opcodes = NULL;\n')
patch('sh2int.c',
      '   // Initialize any internal variables\n   for(i = 0;i < 0x10000;i++)\n      opcodes[i] = decode(i);\n',
      '   // Initialize any internal variables\n'
      '   if (opcodes == NULL && (opcodes = (opcodefunc *)malloc(0x10000 * sizeof(opcodefunc))) == NULL)\n'
      '      return -1;\n'
      '   for(i = 0;i < 0x10000;i++)\n      opcodes[i] = decode(i);\n')
patch('sh2int.c',
      'context->instruction = fetchlist[(context->regs.PC >> 20) & 0x0FF](context->regs.PC);',
      '/* EmuSuNi: the high work RAM\'s instructions read here (FetchHWram\'s read, without its call) */\n'
      '      context->instruction = ((context->regs.PC & 0xDFF00000) == 0x06000000) ?\n'
      '         T2ReadWord(HighWram, context->regs.PC & 0xFFFFF) :\n'
      '         fetchlist[(context->regs.PC >> 20) & 0x0FF](context->regs.PC);', count=2)
patch('sh2int.h', 'extern opcodefunc opcodes[0x10000];', 'extern opcodefunc *opcodes; /* EmuSuNi: 0x10000, on the heap */')
# (00.69) The SH-2s' waits skipped, exactly. Most of their instructions are waits: Daytona USA's master SH-2 spends 57%
# of its own in a loop of 21 NOPs around a flag's read, its slave 77% reading its FRT's flag until the master starts
# it (the SGL's slave loop); Sonic R's 67% and 68% in loops of 4 instructions (the PC: every address counted over 600
# frames of their races). Upstream's idle detection (sh2idle.c) takes loops of up to 14 instructions ending in a
# branch back, and missed all of these. Here, at each slice's start: the interpreter runs as always, watching for up to
# 64 instructions whether the code comes back to where the slice started with every register as it was, having
# written nothing and read only work RAM or the FRT's registers (memory.c counts the rest: emusuni_sh2_side). Then
# each further turn of that loop is the same turn (nothing it reads can change within the slice: the other SH-2, the
# DMA, the interrupts and the timers run between slices), so all but the last one are counted, not run: the cycles
# added, the registers as they are; the last turn and the slice's end are run as before. The same states, instruction
# for instruction where it matters; the PC checks every picture of both games' runs. EMUSUNI_SATURN_SH2_SKIP=0: off.
patch('sh2int.c',
      'FASTCALL void SH2InterpreterExec(SH2_struct *context, u32 cycles)\n{\n',
      '/* EmuSuNi: see scripts/build-saturn-core.sh (the SH-2s\' waits skipped) */\n'
      '#include <stdlib.h>\n#include <string.h>\n'
      'extern u32 emusuni_sh2_side;\n'
      'unsigned long long emusuni_sh2_skipped[2]; /* cycles counted without being run: the master\'s, the slave\'s */\n'
      'int emusuni_sh2_skip = -1;                  /* -1: not decided yet (EMUSUNI_SATURN_SH2_SKIP) */\n'
      'static void emu_sh2_wait(SH2_struct *context, int target_cycle)\n'
      '{\n'
      '   const sh2regs_struct start = context->regs;\n'
      '   const u32 side = emusuni_sh2_side, pc = context->regs.PC, c0 = context->cycles;\n'
      '   int n;\n'
      '   for (n = 0; n < 64 && context->cycles < target_cycle; n++)\n'
      '   {\n'
      '      context->instruction = ((context->regs.PC & 0xDFF00000) == 0x06000000) ?\n'
      '         T2ReadWord(HighWram, context->regs.PC & 0xFFFFF) :\n'
      '         fetchlist[(context->regs.PC >> 20) & 0x0FF](context->regs.PC);\n'
      '      opcodes[context->instruction](context);\n'
      '      if (emusuni_sh2_side != side)\n'
      '         return; /* it wrote, or read something that changes */\n'
      '      if (context->regs.PC == pc)\n'
      '      {\n'
      '         int turn, k;\n'
      '         if (memcmp(&context->regs, &start, sizeof(start)) != 0)\n'
      '            return; /* a loop that changes something (a count) */\n'
      '         turn = (int)(context->cycles - c0);\n'
      '         if (turn <= 0)\n'
      '            return;\n'
      '         k = (target_cycle - (int)context->cycles) / turn - 1;\n'
      '         if (k > 0)\n'
      '         {\n'
      '            context->cycles += (u32)(k * turn);\n'
      '            emusuni_sh2_skipped[context != MSH2] += (unsigned long long)k * turn;\n'
      '         }\n'
      '         return;\n'
      '      }\n'
      '   }\n'
      '}\n\n'
      'FASTCALL void SH2InterpreterExec(SH2_struct *context, u32 cycles)\n{\n')
patch('sh2int.c',
      '#ifndef EXEC_FROM_CACHE\n   if (context->isIdle)\n     SH2idleParse(context, target_cycle);\n'
      '   else\n     SH2idleCheck(context, target_cycle);\n#endif\n',
      '#ifndef EXEC_FROM_CACHE\n   if (context->isIdle)\n     SH2idleParse(context, target_cycle);\n'
      '   else\n     SH2idleCheck(context, target_cycle);\n#endif\n'
      '   /* EmuSuNi: a wait\'s turns counted, not run (emu_sh2_wait) */\n'
      '   if (emusuni_sh2_skip < 0)\n'
      '      emusuni_sh2_skip = !(getenv("EMUSUNI_SATURN_SH2_SKIP") && getenv("EMUSUNI_SATURN_SH2_SKIP")[0] == \'0\');\n'
      '   if (emusuni_sh2_skip && !context->isIdle && context->cycles < target_cycle)\n'
      '      emu_sh2_wait(context, target_cycle);\n')

# --- memory.c: the work RAMs' fast paths (the same data and cycles as the table's handlers); the backup RAM file ---
READ = ('  if ((addr & 0xDFF00000) == 0x06000000) { if (cycle != NULL) *cycle = 0; return T2Read%s(HighWram, addr & 0xFFFFF); }\n'
        '  if ((addr & 0xDFF00000) == 0x00200000) { if (cycle != NULL) *cycle = 12; return T2Read%s(LowWram, addr & 0xFFFFF); }\n')
WRITE = ('  if ((addr & 0xDFF00000) == 0x06000000) { if (cycle != NULL) *cycle = 2; T2Write%s(HighWram, addr & 0xFFFFF, val); return; }\n'
         '  if ((addr & 0xDFF00000) == 0x00200000) { if (cycle != NULL) *cycle = 7; T2Write%s(LowWram, addr & 0xFFFFF, val); return; }\n')
NOTE = ('  /* EmuSuNi: the high and low work RAM straight away (their handlers\' reads and writes, and the cycles\n'
        '     GET_MEM_CYCLE_R / GET_MEM_CYCLE_W give them) */\n')
# (00.69) Every write and every read of what isn't work RAM - but the FRT's registers, which change only between the
# SH-2s' slices - counted (emusuni_sh2_side): the interpreter's wait skip (sh2int.c) sees whether a turn of a loop
# could have changed anything or read something that changes.
SIDE_READ = '  if ((addr & 0xFFFFFFF0) != 0xFFFFFE10) emusuni_sh2_side++; /* EmuSuNi: not work RAM: maybe changing */\n'
SIDE_WRITE = '  emusuni_sh2_side++; /* EmuSuNi: a write */\n'
patch('memory.c', '#include "memory.h"\n',
      '#include "memory.h"\n'
      '/* EmuSuNi (00.69): the SH-2s\' writes and reads of anything but work RAM, counted (sh2int.c\'s wait skip) */\n'
      'u32 emusuni_sh2_side;\n')
for size, kind in (('Byte', 'u8'), ('Word', 'u16'), ('Long', 'u32')):
    sig = '%s FASTCALL MappedMemoryRead%s(u32 addr, u32 * cycle)\n#endif\n{\n' % (kind, size)
    patch('memory.c', sig, sig + NOTE + READ % (size, size) + SIDE_READ)
for size, sig in (('Byte', 'void FASTCALL MappedMemoryWriteByte(u32 addr, u8 val, u32 * cycle)\n#endif\n{\n'),
                  ('Word', 'void FASTCALL MappedMemoryWriteWord(u32 addr, u16 val, u32 * cycle )\n#endif\n{\n'),
                  ('Long', 'void FASTCALL MappedMemoryWriteLong(u32 addr, u32 val, u32 * cycle )\n#endif\n{\n')):
    patch('memory.c', sig, sig + SIDE_WRITE + NOTE + WRITE % (size, size))
patch('memory.c',
      '#if defined(NX)\n\nvoid * YabMemMap(char * filename, u32 size ) {\n',
      '#if defined(EMUSUNI_PORT)\n\n'
      '/* EmuSuNi: the backup RAM file read into the heap and written back when the game ends (YabFreeMap), where the\n'
      '   other builds map it (the PS4 maps no files). */\n'
      'static void * emusuni_map = NULL;\n'
      'static long emusuni_map_size = 0;\n'
      'static char emusuni_map_path[1024];\n\n'
      'void * YabMemMap(char * filename, u32 size ) {\n'
      '  FILE * f = fopen(filename, "rb");\n'
      '  long length;\n'
      '  void * p;\n'
      '  if (f == NULL)\n    return NULL;\n'
      '  fseek(f, 0, SEEK_END);\n'
      '  length = ftell(f);\n'
      '  fseek(f, 0, SEEK_SET);\n'
      '  p = length > 0 ? calloc(1, length) : NULL;\n'
      '  if (p == NULL || fread(p, 1, length, f) != (size_t)length) {\n'
      '    fclose(f);\n    free(p);\n    return NULL;\n  }\n'
      '  fclose(f);\n'
      '  emusuni_map = p;\n'
      '  emusuni_map_size = length;\n'
      '  snprintf(emusuni_map_path, sizeof(emusuni_map_path), "%s", filename);\n'
      '  return p;\n'
      '}\n\n'
      'void YabFreeMap(void * p) {\n'
      '  if (p != NULL && p == emusuni_map) {\n'
      '    FILE * f = fopen(emusuni_map_path, "r+b");\n'
      '    if (f != NULL) {\n'
      '      fwrite(p, 1, emusuni_map_size, f);\n'
      '      fclose(f);\n'
      '    }\n'
      '    emusuni_map = NULL;\n'
      '  }\n'
      '  free(p);\n'
      '}\n\n'
      '#elif defined(NX)\n\nvoid * YabMemMap(char * filename, u32 size ) {\n')

# --- Musashi: its handler table and cycle counts on the heap (built once, by m68k_init) ---
patch('musashi/m68kops.c',
      '#include "m68kops.h"\n',
      '#include "m68kops.h"\n#include <stdlib.h>\n')
patch('musashi/m68kops.c',
      'void  (*m68ki_instruction_jump_table[0x10000])(void); /* opcode handler jump table */\n'
      'unsigned char m68ki_cycles[NUM_CPU_TYPES][0x10000]; /* Cycles used by CPU type */\n',
      '/* EmuSuNi: on the heap (m68ki_build_opcode_table: 704 KiB), not in the program\'s static memory */\n'
      'void  (**m68ki_instruction_jump_table)(void) = NULL; /* opcode handler jump table */\n'
      'unsigned char (*m68ki_cycles)[0x10000] = NULL; /* Cycles used by CPU type */\n')
patch('musashi/m68kops.c',
      '\tint j;\n\tint k;\n\n\tfor(i = 0; i < 0x10000; i++)\n',
      '\tint j;\n\tint k;\n\n'
      '\tif(m68ki_instruction_jump_table == NULL)\n'
      '\t\tm68ki_instruction_jump_table = (void (**)(void))malloc(0x10000 * sizeof(*m68ki_instruction_jump_table));\n'
      '\tif(m68ki_cycles == NULL)\n'
      '\t\tm68ki_cycles = (unsigned char (*)[0x10000])malloc(NUM_CPU_TYPES * sizeof(*m68ki_cycles));\n'
      '\tif(m68ki_instruction_jump_table == NULL || m68ki_cycles == NULL)\n'
      '\t\tabort();\n\n'
      '\tfor(i = 0; i < 0x10000; i++)\n')
patch('musashi/m68kops.h',
      'extern void (*m68ki_instruction_jump_table[0x10000])(void); /* opcode handler jump table */\n'
      'extern unsigned char m68ki_cycles[][0x10000];',
      'extern void (**m68ki_instruction_jump_table)(void); /* opcode handler jump table (EmuSuNi: on the heap) */\n'
      'extern unsigned char (*m68ki_cycles)[0x10000];')

# --- junzip.c: its buffer on the heap, made when a zip is first read ---
patch('junzip.c',
      'unsigned char jzBuffer[JZ_BUFFER_SIZE]; // limits maximum zip descriptor size\n',
      '/* EmuSuNi: the buffer (64 KiB) on the heap, made when a zip is first read (a .cue or .chd never needs it) */\n'
      'static unsigned char (*emusuni_jzBuffer)[JZ_BUFFER_SIZE]; // limits maximum zip descriptor size\n'
      '#define jzBuffer (*emusuni_jzBuffer)\n'
      'static int emusuni_jzReady(void) {\n'
      '    if (emusuni_jzBuffer == NULL)\n'
      '        emusuni_jzBuffer = (unsigned char (*)[JZ_BUFFER_SIZE])malloc(sizeof(*emusuni_jzBuffer));\n'
      '    return emusuni_jzBuffer != NULL;\n'
      '}\n')
patch('junzip.c',
      'int jzReadEndRecord(JZFile *zip, JZEndRecord *endRecord) {\n    long fileSize, readBytes, i;\n    JZEndRecord *er;\n',
      'int jzReadEndRecord(JZFile *zip, JZEndRecord *endRecord) {\n    long fileSize, readBytes, i;\n    JZEndRecord *er;\n'
      '    if (!emusuni_jzReady())\n        return Z_ERRNO;\n')
patch('junzip.c',
      '    JZGlobalFileHeader fileHeader;\n    JZFileHeader header;\n    int i;\n',
      '    JZGlobalFileHeader fileHeader;\n    JZFileHeader header;\n    int i;\n'
      '    if (!emusuni_jzReady())\n        return Z_ERRNO;\n')
patch('junzip.c',
      '    z_stream strm;\n    int ret;\n',
      '    z_stream strm;\n    int ret;\n'
      '    if (!emusuni_jzReady())\n        return Z_ERRNO;\n')

# --- vidsoft.c: without the GL headers; timers around its frame's parts (EMUSUNI_SOFT_PROF: the PC measurement) ---
patch('vidsoft.c',
      '#include "vidsoft.h"\n#include "ygl.h"\n#include "vidshared.h"\n',
      '#include "vidsoft.h"\n#ifdef EMUSUNI_NO_GL\n#include "emusuni_saturn_nogl.h"\n#else\n#include "ygl.h"\n#endif\n'
      '#include "vidshared.h"\n')
# (Its colour RAM reads through the GL renderer's Vdp2ColorRamGetColor, the colour RAM's address passed as the alpha:
# its own, which reads the colour RAM it is given - the threads' copy - and keeps the colour's MSB for the sprites'
# colour calculation mode 3, as its own comment says.)
patch('vidsoft.c', 'lineColor = Vdp2ColorRamGetColor(lineColorAddr, (int)color_ram);',
      'lineColor = Vdp2ColorRamGetColorSoft(lineColorAddr, color_ram);')
patch('vidsoft.c', 'dot = Vdp2ColorRamGetColor(color, (int)Vdp2ColorRam);',
      'dot = Vdp2ColorRamGetColorSoft(color, Vdp2ColorRam);', 2)
patch('vidsoft.c', 'dot = Vdp2ColorRamGetColor(vdp1coloroffset + pixel,(int)color_ram);',
      'dot = Vdp2ColorRamGetColorSoft(vdp1coloroffset + pixel, color_ram);')
patch('vidsoft.c', 'dot = Vdp2ColorRamGetColor(vdp1coloroffset + pixel, (int)color_ram);',
      'dot = Vdp2ColorRamGetColorSoft(vdp1coloroffset + pixel, color_ram);')
patch('vidsoft.c',
      'VIDSoftVdp1Reset,\nVIDSoftVdp1DrawStart,\nVIDSoftVdp1DrawEnd,\n',
      'VIDSoftVdp1Reset,\n#ifdef EMUSUNI_SOFT_PROF\nEmuProfVdp1DrawStart,\n#else\nVIDSoftVdp1DrawStart,\n#endif\n'
      'VIDSoftVdp1DrawEnd,\n')
patch('vidsoft.c',
      'VIDSoftVdp2Reset,\nVIDSoftVdp2DrawStart,\nVIDSoftVdp2DrawEnd,\nVIDSoftVdp2DrawScreens,\n',
      'VIDSoftVdp2Reset,\n#ifdef EMUSUNI_SOFT_PROF\nEmuProfVdp2DrawStart,\nEmuProfVdp2DrawEnd,\nEmuProfVdp2DrawScreens,\n'
      '#else\nVIDSoftVdp2DrawStart,\nVIDSoftVdp2DrawEnd,\nVIDSoftVdp2DrawScreens,\n#endif\n')
# Its drawing on two threads of its own (emusuni_soft_threads.h, at the end of the file; emusuni_soft_decl.h after its
# includes: the VDP2's state its drawing reads through pointers - the console's own, or its thread's copy).
patch('vidsoft.c',
      '#include <stdlib.h>\n#include <limits.h>\n',
      '#include <stdlib.h>\n#include <limits.h>\n#include "emusuni_soft_decl.h"\n')
patched[Y + 'vidsoft.c'] += '\n#include "emusuni_soft_threads.h"\n'
# Upstream's frame functions kept for the drawing on the emulation thread (EMUSUNI_SATURN_THREADS=0), under names of
# their own; the renderer's own are emusuni_soft_threads.h's.
patch('vidsoft.c', 'void VIDSoftVdp1DrawStart()\n{\n', 'static void emu_vdp1_start_sync(void)\n{\n')
patch('vidsoft.c', 'void VIDSoftVdp2DrawStart(void)\n{\n   int titanblendmode',
      'static void emu_vdp2_start_body(void)\n{\n   int titanblendmode')
patch('vidsoft.c', 'void VIDSoftVdp2DrawScreens(void)\n{\n', 'static void emu_vdp2_screens_sync(void)\n{\n')
patch('vidsoft.c', 'void VIDSoftVdp2DrawEnd(void)\n{\n', 'static void emu_vdp2_end_sync(void)\n{\n')
patch('vidsoft.c',
      '   if (! OSDUseBuffer())\n      OSDDisplayMessages(NULL, -1, -1);\n  \n   YuiSwapBuffers();\n}\n',
      '   if (! OSDUseBuffer())\n      OSDDisplayMessages(NULL, -1, -1);\n}\n')
# The emulation thread's walk of the VDP1's command list (no frame buffer): the drawing functions draw nothing.
for head in ('void VIDSoftVdp1NormalSpriteDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer) {\n',
             'void VIDSoftVdp1ScaledSpriteDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer){\n',
             'void VIDSoftVdp1DistortedSpriteDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer) {\n',
             'void VIDSoftVdp1PolylineDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer)\n{\n',
             'void VIDSoftVdp1LineDraw(u8* ram, Vdp1*regs, u8* back_framebuffer)\n{\n'):
    patch('vidsoft.c', head, head + '   if (back_framebuffer == NULL)\n      return; /* EmuSuNi: the emulation\'s walk */\n')
# Waits for the VDP1's drawing: its thread's.
patch('vidsoft.c',
      'void VidsoftWaitForVdp1Thread()\n{\n   if (vidsoft_vdp1_thread_enabled)\n',
      'void VidsoftWaitForVdp1Thread()\n{\n   if (emu_threads)\n   {\n      emu_vdp1_wait();\n      return;\n   }\n'
      '   if (vidsoft_vdp1_thread_enabled)\n')
patch('vidsoft.c',
      '      if (vidsoft_vdp1_thread_enabled)\n      {\n         VidsoftWaitForVdp1Thread();\n      }\n',
      '      VidsoftWaitForVdp1Thread();\n')
# YabaSanshiro's VBlank OUT (vdp2.cpp) erases the displayed VDP1 frame buffer and changes the buffers through the
# renderer's Vdp1EraseWrite and Vdp1FrameChange, as its GL renderer has them (vidsoft's were empty: the manual
# change of Daytona USA never showed its 3D); vidsoft's own erase as the drawing starts and its own change at VBlank
# IN (from Yabause, which had neither) would do it twice: out.
patch('vidsoft.c', 'void VIDSoftVdp1EraseWrite(){};\nvoid VIDSoftVdp1FrameChange(){};\n',
      'void VIDSoftVdp1EraseWrite(void);\nvoid VIDSoftVdp1FrameChange(void);\n')
patch('vidsoft.c', '   VIDSoftVdp1EraseFrameBuffer(regs, back_framebuffer);\n',
      '   /* EmuSuNi: erased by vdp2.cpp (Vdp1EraseWrite) */\n')
patch('vidsoft.c', '   VIDSoftVdp1SwapFrameBuffer();\n',
      '   /* EmuSuNi: changed by vdp2.cpp (Vdp1FrameChange) */\n')
# Upstream's threads not started (seven here, five in titan.c: 24 MiB of stacks, never stopped), their copies never
# made (1.4 MiB of static memory: pointers to nothing); the sprite window's mask and the mosaic table on the heap.
patch('vidsoft.c',
      '   for (i = 0; i < 6; i++)\n   {\n      vidsoft_thread_context.draw_finished[i] = 1;\n'
      '      vidsoft_thread_context.need_draw[i] = 0;\n   }\n\n'
      '   vidsoft_vdp1_thread_context.need_draw = 0;\n   vidsoft_vdp1_thread_context.draw_finished = 1;\n'
      '   YabThreadStart(YAB_THREAD_VIDSOFT_VDP1, (void * (*)(void *))VidsoftVdp1Thread, 0);\n\n'
      '   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_RBG0, (void * (*)(void *))VidsoftRbg0Thread, 0);\n'
      '   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG0, (void * (*)(void *))VidsoftNbg0Thread, 0);\n'
      '   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG1, (void * (*)(void *))VidsoftNbg1Thread, 0);\n'
      '   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG2, (void * (*)(void *))VidsoftNbg2Thread, 0);\n'
      '   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG3, (void * (*)(void *))VidsoftNbg3Thread, 0);\n'
      '   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_SPRITE, (void * (*)(void *))VidsoftSpriteThread, 0);\n',
      '   (void)i;\n   if (emu_soft_init() != 0)\n   {\n      emu_soft_deinit();\n      return -1;\n   }\n')
patch('vidsoft.c',
      'void VIDSoftDeInit(void)\n{\n',
      'void VIDSoftDeInit(void)\n{\n   emu_soft_deinit();\n   TitanDeInit();\n')
patch('vidsoft.c',
      '   if (vdp1framebuffer[0])\n      free(vdp1framebuffer[0]);\n\n'
      '   if (vdp1framebuffer[1])\n      free(vdp1framebuffer[1]);\n',
      '   free(vdp1framebuffer[0]);\n   free(vdp1framebuffer[1]);\n   vdp1framebuffer[0] = vdp1framebuffer[1] = NULL;\n')
patch('vidsoft.c', 'u8 sprite_window_mask[704 * 512];\n',
      'u8 *sprite_window_mask; /* EmuSuNi: 352 KiB, on the heap (emu_soft_init) */\n')
patch('vidsoft.c',
      '   u8 back_framebuffer[0x40000];\n}vidsoft_vdp1_thread_context;\n',
      '   u8 back_framebuffer[0x40000];\n} *emusuni_vidsoft_vdp1_thread_context; /* EmuSuNi: never made */\n'
      '#define vidsoft_vdp1_thread_context (*emusuni_vidsoft_vdp1_thread_context)\n')
patch('vidsoft.c',
      '   struct CellScrollData cell_scroll_data[270];\n}vidsoft_thread_context;\n',
      '   struct CellScrollData cell_scroll_data[270];\n} *emusuni_vidsoft_thread_context; /* EmuSuNi: never made */\n'
      '#define vidsoft_thread_context (*emusuni_vidsoft_thread_context)\n')
patch('vidsoft.c',
      '\t   static int tables_initialized = 0;\n\t   static int mosaic_table[16][1024];\n'
      '\t   if(!tables_initialized)\n\t   {\n\t\t   tables_initialized = 1;\n',
      '\t   int (*mosaic_table)[1024] = emu_mosaic_table; /* EmuSuNi: its 64 KiB on the heap (emu_soft_init) */\n'
      '\t   if(!emu_mosaic_ready)\n\t   {\n\t\t   emu_mosaic_ready = 1;\n')

# The VDP2's scroll screens (NBG0-3, and RBG0 when it doesn't rotate) and its sprite layer drawn in two bands of
# lines by its two threads: the copy of the registers a band's thread gives them says which lines
# (emusuni_soft_decl.h's emu_band); the lines of the other band only step their per-line state (line windows, line
# parameters, the output line), so each band draws what the whole drawing draws there.
patch('vidsoft.c', '   int num_vertical_cell_scroll_enabled = 0;\n',
      '   int num_vertical_cell_scroll_enabled = 0;\n   int emu_y0, emu_y1;\n')
patch('vidsoft.c', '   Vdp2GetInterlaceInfo(&start_line, &line_increment);\n\n   if (regs->SCRCTL & 1)\n',
      '   Vdp2GetInterlaceInfo(&start_line, &line_increment);\n   emu_band(regs, &emu_y0, &emu_y1);\n\n'
      '   if (regs->SCRCTL & 1)\n')
patch('vidsoft.c', '      if (!info->enable)\n         continue;\n',
      '      if (!info->enable)\n         continue;\n'
      '      if (j < emu_y0 || j >= emu_y1)\n      {\n         output_y++; /* EmuSuNi: the other band\'s line */\n'
      '         continue;\n      }\n')
patch('vidsoft.c', '   int vdp1spritetype = 0;\n',
      '   int vdp1spritetype = 0;\n   int emu_y0, emu_y1;\n\n   emu_band(vdp2_regs, &emu_y0, &emu_y1);\n')
patch('vidsoft.c', '         {\n\n            info.titan_shadow_type = 0;\n',
      '         {\n\n            if (i2 < emu_y0 || i2 >= emu_y1)\n               break; /* EmuSuNi: the other band\'s line */\n'
      '            info.titan_shadow_type = 0;\n')
patch('vidsoft.c', '   VidsoftWaitForVdp1Thread();\n\n   switch (type)\n',
      '   emu_vdp1_wait_cpu(); /* EmuSuNi: the CPU reads or writes the drawn frame buffer */\n\n   switch (type)\n', 2)
# The VDP2's window tests, a few times a pixel of every layer (00.69): with every window off (most games, most of the
# time) the answer first - upstream worked out both windows and read the sprite window's mask before it looked - and
# the sprite window's mask read only when that window is on. Their answers are the same (the tests change nothing).
patch('vidsoft.c',
      '   mask = sprite_window_mask[addr];\n\n   if (wctl & 0x20)//sprite window enabled on layer\n',
      '   if (!(wctl & 0x20))\n      return 3; /* EmuSuNi: the sprite window off: its mask not read */\n\n'
      '   mask = sprite_window_mask[addr];\n\n   if (wctl & 0x20)//sprite window enabled on layer\n')
patch('vidsoft.c',
      'static INLINE int TestBothWindow(int wctl, clipping_struct *clip, int x, int y)\n{\n'
      '    int w0 = TestWindow(wctl, 0x2, 0x1, &clip[0], x, y);\n'
      '    int w1 = TestWindow(wctl, 0x8, 0x4, &clip[1], x, y);\n'
      '    int spr = TestSpriteWindow(wctl, x,y);\n',
      'static INLINE int TestBothWindow(int wctl, clipping_struct *clip, int x, int y)\n{\n'
      '    int w0, w1, spr;\n'
      '    /* EmuSuNi: all windows disabled - the answer below, before the tests */\n'
      '    if ((wctl & 0x2a) == 0)\n'
      '       return (wctl & 0x80) == 0x80 ? 0 : 1;\n'
      '    w0 = TestWindow(wctl, 0x2, 0x1, &clip[0], x, y);\n'
      '    w1 = TestWindow(wctl, 0x8, 0x4, &clip[1], x, y);\n'
      '    spr = TestSpriteWindow(wctl, x,y);\n')
# (00.69) The scroll screens' pixel put (Vdp2DrawScroll, the VDP2's busiest loop) without its two calls: titan's
# TitanPutPixel (another file: a call for every pixel of every layer) as an inline store into the layer
# (emusuni_titan_layer gives it, the same struct as titan's PixelData), and the colour offset's function pointer
# compared with its two functions' addresses so they can be inlined. The same stores.
patch('vidsoft.c',
      '            TitanPutPixel(priority, i, output_y, info->PostPixelFetchCalc(info, COLSAT2YAB32(alpha, color)), '
      'info->linescreen, info);\n',
      '            emu_put_pixel(emu_layer, priority, i, output_y, emu_post(info, COLSAT2YAB32(alpha, color)), '
      'info->linescreen, info);\n')
patch('vidsoft.c', '   int num_vertical_cell_scroll_enabled = 0;\n   int emu_y0, emu_y1;\n',
      '   int num_vertical_cell_scroll_enabled = 0;\n   int emu_y0, emu_y1;\n'
      '   EmuPixel *const emu_layer = (EmuPixel *)emusuni_titan_layer(info->titan_which_layer);\n')
# --- titan/titan.c (the software renderer's compositor): its threads not started; the VDP2's registers its drawing
# reads (emusuni_soft_decl.h); all it allocated freed, and allocated again for the next game ---
patch('titan/titan.c',
      '#include "../threads.h"\n\n#include <stdlib.h>\n',
      '#include "../threads.h"\n\n#include <stdlib.h>\n'
      '#ifdef EMUSUNI_SOFT\n'
      '/* EmuSuNi: the VDP2\'s registers the drawing reads: the console\'s, or its thread\'s copy (vidsoft.c) */\n'
      'extern Vdp2 *emusuni_v2regs;\n#define Vdp2Regs emusuni_v2regs\n'
      '/* EmuSuNi (00.69): each back screen line\'s colour and width when it was last filled (TitanPutBackHLine; all\n'
      '   forgotten when the width changes - the lines\' memory moves - and with a new back screen) */\n'
      '#include <string.h>\n'
      'static u32 emusuni_back_color[512];\nstatic int emusuni_back_width[512], emusuni_back_last;\n'
      '#endif\n')
patch('titan/titan.c',
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_0, (void * (*)(void *))VidsoftPriorityThread0, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_1, (void * (*)(void *))VidsoftPriorityThread1, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_2, (void * (*)(void *))VidsoftPriorityThread2, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_3, (void * (*)(void *))VidsoftPriorityThread3, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_4, (void * (*)(void *))VidsoftPriorityThread4, NULL);\n',
      '#ifndef EMUSUNI_SOFT\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_0, (void * (*)(void *))VidsoftPriorityThread0, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_1, (void * (*)(void *))VidsoftPriorityThread1, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_2, (void * (*)(void *))VidsoftPriorityThread2, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_3, (void * (*)(void *))VidsoftPriorityThread3, NULL);\n'
      '      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_4, (void * (*)(void *))VidsoftPriorityThread4, NULL);\n'
      '#endif\n')
# (00.69) The layers erased band by band after their composition (vidsoft.c's threads: emu_vdp2_compose), only the
# layers something was drawn into (emusuni_titan_dirty), instead of all six whole on one thread before the drawing (3.8
# MiB a frame at 352x224: a sixth of the VDP2's work, done while its helper waited): every pixel the drawing writes is
# in the rows the composition reads, so the layers are blank again before the next drawing, as TitanErase leaves them.
# The back screen's lines filled only when their colour or the width changed (the same memory, the same pixels).
patch('titan/titan.c',
      'void TitanPutBackHLine(s32 y, u32 color)\n{\n'
      '   struct PixelData* buffer = &tt_context.backscreen[(y * tt_context.vdp2width)];\n   int i;\n',
      'void TitanPutBackHLine(s32 y, u32 color)\n{\n'
      '   struct PixelData* buffer = &tt_context.backscreen[(y * tt_context.vdp2width)];\n   int i;\n'
      '#ifdef EMUSUNI_SOFT\n'
      '   if (emusuni_back_last != tt_context.vdp2width)\n'
      '   {\n'
      '      memset(emusuni_back_width, 0, sizeof(emusuni_back_width));\n'
      '      emusuni_back_last = tt_context.vdp2width;\n'
      '   }\n'
      '   if (y >= 0 && y < 512)\n'
      '   {\n'
      '      if (emusuni_back_width[y] == tt_context.vdp2width && emusuni_back_color[y] == color)\n'
      '         return; /* EmuSuNi: the line as it is */\n'
      '      emusuni_back_width[y] = tt_context.vdp2width;\n'
      '      emusuni_back_color[y] = color;\n'
      '   }\n'
      '#endif\n')
patch('titan/titan.c',
      '      tt_context.inited = 1;\n   }\n',
      '      tt_context.inited = 1;\n'
      '#ifdef EMUSUNI_SOFT\n'
      '      memset(emusuni_back_width, 0, sizeof(emusuni_back_width)); /* EmuSuNi: a new back screen */\n'
      '#endif\n'
      '   }\n')
patch('titan/titan.c',
      'void TitanPutPixel(int priority, s32 x, s32 y, u32 color, int linescreen, vdp2draw_struct* info)\n{\n'
      '   if (priority == 0) return;\n',
      '#ifdef EMUSUNI_SOFT\n'
      '/* EmuSuNi: the layers something was drawn into since they were last erased */\n'
      'int emusuni_titan_dirty[6];\n'
      '#endif\n'
      'void TitanPutPixel(int priority, s32 x, s32 y, u32 color, int linescreen, vdp2draw_struct* info)\n{\n'
      '   if (priority == 0) return;\n'
      '#ifdef EMUSUNI_SOFT\n'
      '   emusuni_titan_dirty[info->titan_which_layer] = 1;\n'
      '#endif\n')
patch('titan/titan.c',
      'void TitanPutHLine(int priority, s32 x, s32 y, s32 width, u32 color)\n{\n   if (priority == 0) return;\n',
      'void TitanPutHLine(int priority, s32 x, s32 y, s32 width, u32 color)\n{\n   if (priority == 0) return;\n'
      '#ifdef EMUSUNI_SOFT\n'
      '   if (priority < 6)\n      emusuni_titan_dirty[priority] = 1;\n'
      '#endif\n')
patch('titan/titan.c',
      'void TitanRender(pixel_t * dispbuffer)\n{\n',
      '#ifdef EMUSUNI_SOFT\n'
      '/* EmuSuNi: TitanErase for the layers\' rows [y0, y1) (rows as TitanErase counts them: halved interlaced), of the\n'
      '   layers drawn into; the caller clears emusuni_titan_dirty once every band is erased. */\n'
      'void emusuni_titan_erase_rows(int y0, int y1)\n{\n'
      '   int i, height = tt_context.vdp2height;\n'
      '   if (vdp2_interlace)\n      height /= 2;\n'
      '   if (y1 > height)\n      y1 = height;\n'
      '   if (y0 < 0)\n      y0 = 0;\n'
      '   if (y1 <= y0 || !tt_context.inited)\n      return;\n'
      '   for (i = 0; i < 6; i++)\n'
      '   {\n'
      '      if (emusuni_titan_dirty[i])\n'
      '         memset(tt_context.vdp2framebuffer[i] + y0 * tt_context.vdp2width, 0,\n'
      '                sizeof(struct PixelData) * tt_context.vdp2width * (y1 - y0));\n'
      '   }\n'
      '}\n'
      '/* EmuSuNi: a layer\'s pixels, for vidsoft.c\'s inline put (its EmuPixel: this struct PixelData, checked here) */\n'
      'typedef char emusuni_pixeldata_is_8_bytes[sizeof(struct PixelData) == 8 ? 1 : -1];\n'
      'void *emusuni_titan_layer(int layer)\n{\n   return tt_context.vdp2framebuffer[layer];\n}\n'
      '#endif\n\n'
      '#ifdef EMUSUNI_SOFT\n'
      '/* EmuSuNi: TitanRender\'s start, for the composition of bands of lines on two threads (vidsoft.c): -1 when\n'
      '   there is nothing to compose, else whether the simplified way serves. */\n'
      'int emusuni_titan_prepare(void)\n{\n'
      '   int can_use_simplified_rendering = 1;\n'
      '   if (!tt_context.inited || (!tt_context.trans))\n      return -1;\n'
      '   if ((Vdp2Regs->CCCTL & 0x807f) != 0)\n      can_use_simplified_rendering = 0;\n'
      '   if ((Vdp2Regs->SFPRMD & 0x3ff) != 0)\n      can_use_simplified_rendering = 0;\n'
      '   if ((Vdp2Regs->LNCLEN & 0x1f) != 0)\n      can_use_simplified_rendering = 0;\n'
      '   if ((Vdp2Regs->SDCTL & 0x13F) != 0)\n      can_use_simplified_rendering = 0;\n'
      '   tt_context.layer_priority[TITAN_NBG0] = Vdp2Regs->PRINA & 0x7;\n'
      '   tt_context.layer_priority[TITAN_NBG1] = ((Vdp2Regs->PRINA >> 8) & 0x7);\n'
      '   tt_context.layer_priority[TITAN_NBG2] = (Vdp2Regs->PRINB & 0x7);\n'
      '   tt_context.layer_priority[TITAN_NBG3] = ((Vdp2Regs->PRINB >> 8) & 0x7);\n'
      '   tt_context.layer_priority[TITAN_RBG0] = (Vdp2Regs->PRIR & 0x7);\n'
      '   return can_use_simplified_rendering;\n}\n#endif\n\n'
      'void TitanRender(pixel_t * dispbuffer)\n{\n')
patch('titan/titan.c',
      '   for(i = 1;i < 4;i++)\n      free(tt_context.linescreen[i]);\n\n   return 0;\n}\n',
      '   for(i = 1;i < 4;i++)\n      free(tt_context.linescreen[i]);\n'
      '#ifdef EMUSUNI_SOFT\n'
      '   free(tt_context.backscreen);\n'
      '   for(i = 0;i < 6;i++)\n      tt_context.vdp2framebuffer[i] = NULL;\n'
      '   for(i = 1;i < 4;i++)\n      tt_context.linescreen[i] = NULL;\n'
      '   tt_context.backscreen = NULL;\n'
      '   tt_context.inited = 0;\n'
      '#endif\n\n   return 0;\n}\n')

written = 0
for path, text in patched.items():
    full = root + '/' + path
    data = text.encode('utf-8')
    with open(full, 'rb') as f:
        same = f.read() == data
    if not same:
        with open(full, 'wb') as f:
            f.write(data)
        written += 1
print("YabaSanshiro: patches applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
# The header forced into every source: the renamed API, the app's hooks, the renderers' functions the rest of the
# core calls (emusuni_saturn_stubs.c). Rewritten only when different: every source depends on it.
mkdir -p "$OUT/include"
write_if_changed() {
  if cmp -s "$1.new" "$1"; then rm -f "$1.new"; else mv "$1.new" "$1"; fi
}
{
  echo "/* Generated by scripts/build-saturn-core.sh: forced into every source of YabaSanshiro. */"
  echo "#pragma once"
  echo "/* Its libretro API, renamed (yabasanshiro_retro_*). */"
  for f in $API; do echo "#define $f yabasanshiro_$f"; done
  cat <<'HEADER'
#ifdef __cplusplus
extern "C" {
#endif
/* The app's (src/SaturnBench.cpp): a thread of the core starts (its core and priority on the PS4); the SCSP thread
   has no new SH-2 cycles to run yet (a short sleep); the SCSP thread finished a frame (its CPU time read). */
void emusuni_saturn_thread(const char *name);
void emusuni_saturn_scsp_idle(void);
void emusuni_saturn_scsp_frame(void);
/* The renderers' functions the rest of the core calls (no renderer: emusuni_saturn_stubs.c), and libretro.c's. */
void YglOnUpdateColorRamWord(unsigned int addr);
void YuiSwapBuffers(void);
#ifdef EMUSUNI_SOFT
/* The software renderer's newest picture (RGB565; NULL: none yet) and its size (emusuni_soft_threads.h); the app's
   (src/SaturnBench.cpp): a line for its log about the renderer's threads, every 600 frames. */
extern const void *emusuni_soft_frame;
extern int emusuni_soft_w, emusuni_soft_h;
void emusuni_saturn_stats(const char *text);
/* Until the renderer's threads have no drawing left (libretro.c: before the game ends, a state is saved or loaded, a
   reset). */
void emusuni_soft_quiesce(void);
#endif
#ifdef EMUSUNI_SOFT_PROF
/* The PC measurement's timers around the software renderer's frame parts (emusuni_soft_prof.c). */
void EmuProfVdp1DrawStart(void);
void EmuProfVdp2DrawStart(void);
void EmuProfVdp2DrawEnd(void);
void EmuProfVdp2DrawScreens(void);
#endif
#ifdef __cplusplus
}
#endif
HEADER
} > "$OUT/include/emusuni_saturn.h.new"
write_if_changed "$OUT/include/emusuni_saturn.h"
# ygl.h's values libretro.c uses (ygl.h is all OpenGL: without HAVE_LIBGL it is empty).
cat > "$OUT/include/emusuni_saturn_nogl.h.new" <<'HEADER'
/* Generated by scripts/build-saturn-core.sh: ygl.h's values libretro.c uses, for the build without a renderer. */
#pragma once
typedef enum { PERSPECTIVE_CORRECTION = 0, CPU_TESSERATION, GPU_TESSERATION } POLYGONMODE;
typedef enum { RES_NATIVE = 0, RES_4x, RES_2x, RES_ORIGINAL, RES_720P, RES_1080P } RESOLUTION_MODE;
typedef enum { RBG_RES_ORIGINAL = 0, RBG_RES_2x, RBG_RES_720P, RBG_RES_1080P, RBG_RES_FIT_TO_EMULATION } RBG_RESOLUTION_MODE;
typedef enum { ORIGINAL = 0, _4_3, _16_9, FULL } ASPECT_RATE_MODE;
HEADER
write_if_changed "$OUT/include/emusuni_saturn_nogl.h"
# What the core calls of the OpenGL and software renderers, which aren't built: nothing to do without a picture.
cat > "$OUT/include/emusuni_saturn_stubs.c.new" <<'STUBS'
/* Generated by scripts/build-saturn-core.sh: the renderers' functions and data the rest of YabaSanshiro uses
   (vdp2.cpp's colour RAM writes, yabause.c's software renderer threads - titan.c has the third -, the software
   compositor titan.c's interlace), with no renderer built: nothing to do, no interlace. */
#include "core.h"
void YglOnUpdateColorRamWord(u32 addr) { (void)addr; }
#ifndef EMUSUNI_SOFT
int vdp2_interlace = 0;
void VIDSoftSetVdp1ThreadEnable(int b) { (void)b; }
void VIDSoftSetNumLayerThreads(int num) { (void)num; }
void Vdp2GetInterlaceInfo(int * start_line, int * line_increment) { *start_line = 0; *line_increment = 1; }
#endif
STUBS
write_if_changed "$OUT/include/emusuni_saturn_stubs.c"
# The PC measurement's timers (SATURN_PROF=1): the software renderer's frame parts, on the calling thread's CPU clock.
cat > "$OUT/include/emusuni_soft_prof.c.new" <<'PROF'
/* Generated by scripts/build-saturn-core.sh (SATURN_PROF=1): the CPU time of the software renderer's frame parts. */
#include <time.h>
long long emusuni_soft_prof_ns[4];
void VIDSoftVdp1DrawStart(void);
void VIDSoftVdp2DrawStart(void);
void VIDSoftVdp2DrawEnd(void);
void VIDSoftVdp2DrawScreens(void);
static long long emusuni_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
#define EMUSUNI_WRAP(name, i) \
  void EmuProf##name(void) { long long t = emusuni_now(); VIDSoft##name(); emusuni_soft_prof_ns[i] += emusuni_now() - t; }
EMUSUNI_WRAP(Vdp1DrawStart, 0)
EMUSUNI_WRAP(Vdp2DrawStart, 1)
EMUSUNI_WRAP(Vdp2DrawScreens, 2)
EMUSUNI_WRAP(Vdp2DrawEnd, 3)
PROF
write_if_changed "$OUT/include/emusuni_soft_prof.c"
# The software renderer's threads (SATURN_RENDERER=soft): emusuni_soft_decl.h after vidsoft.c's includes (what the
# rest of the file needs of them, the VDP2's state its drawing reads through pointers), emusuni_soft_threads.h at its
# end (their code).
cat > "$OUT/include/emusuni_soft_decl.h.new" <<'DECL'
/* Generated by scripts/build-saturn-core.sh, included in vidsoft.c after its includes: what the file needs of its
   threads (emusuni_soft_threads.h, at its end). */
#pragma once
static int emu_threads = 1; /* 0: upstream's single thread (EMUSUNI_SATURN_THREADS=0: the PC's measurement) */
static void emu_vdp1_wait(void);
static int emu_soft_init(void);
static void emu_soft_deinit(void);
/* The VDP2's state the drawing reads: the console's own while the emulation thread draws (upstream's way), the copy
   its thread draws from: the file's (and titan.c's) Vdp2Regs, Vdp2Ram, Vdp2ColorRam and vdp2_is_odd_frame. */
static Vdp2 **const emu_live_v2regs = &Vdp2Regs;
static u8 **const emu_live_v2ram = &Vdp2Ram;
static u8 **const emu_live_v2cram = &Vdp2ColorRam;
static int *const emu_live_odd = &vdp2_is_odd_frame;
Vdp2 *emusuni_v2regs;
static u8 *emu_v2ram, *emu_v2cram;
static int emu_v2odd;
#define Vdp2Regs emusuni_v2regs
#define Vdp2Ram emu_v2ram
#define Vdp2ColorRam emu_v2cram
#define vdp2_is_odd_frame emu_v2odd
/* Vdp2DrawScroll's mosaic table (64 KiB), made with the renderer. */
static int (*emu_mosaic_table)[1024];
static int emu_mosaic_ready;
/* The VDP2's drawing in two bands of lines (its two threads): the copy of its registers a band's layers are given
   says which lines they draw [y0, y1); any other registers (the console's, the thread's whole copy): all. */
static Vdp2 emu_band_regs[2];
static int emu_band_y0[2], emu_band_y1[2];
static int emu_band(const Vdp2 *regs, int *y0, int *y1)
{
   int k;
   for (k = 0; k < 2; k++)
   {
      if (regs == &emu_band_regs[k])
      {
         *y0 = emu_band_y0[k];
         *y1 = emu_band_y1[k];
         return 1;
      }
   }
   *y0 = 0;
   *y1 = 1 << 30;
   return 0;
}
static void emu_vdp1_wait_cpu(void);
static void emu_vdp2_release(const u8 *buffer);
int emusuni_titan_prepare(void);
/* titan.c's (00.69): the layers drawn into since their last erase; their rows [y0, y1) erased. */
extern int emusuni_titan_dirty[6];
void emusuni_titan_erase_rows(int y0, int y1);
/* (00.69) A titan layer's pixel (titan.c's struct PixelData, which it checks) and the scroll screens' inline
   TitanPutPixel (the same stores); the colour offset's function called by name when it is one of its two. */
typedef struct EmuPixel
{
   u32 pixel;
   u8 priority, linescreen, shadow_type, shadow_enabled;
} EmuPixel;
void *emusuni_titan_layer(int layer);
extern int vdp2width; /* (vidsoft.c's, below: titan's own width, TitanSetResolution) */
static INLINE void emu_put_pixel(EmuPixel *layer, int priority, s32 x, s32 y, u32 color, int linescreen,
                                 vdp2draw_struct *info)
{
   EmuPixel *p;
   if (priority == 0)
      return;
   emusuni_titan_dirty[info->titan_which_layer] = 1;
   p = &layer[(y * vdp2width) + x];
   p->pixel = color;
   p->priority = priority;
   p->linescreen = linescreen;
   p->shadow_enabled = info->titan_shadow_enabled;
   p->shadow_type = info->titan_shadow_type;
}
#define emu_post(info, px)                                                                                              \
   ((info)->PostPixelFetchCalc == &DoNothing       ? (px) :                                                             \
    (info)->PostPixelFetchCalc == &DoColorOffset ? DoColorOffset((info), (px)) :                                       \
                                                   (info)->PostPixelFetchCalc((info), (px)))
void TitanRenderSimplifiedCheck(pixel_t *buf, int start, int end, int can_use_simplified_rendering);
DECL
write_if_changed "$OUT/include/emusuni_soft_decl.h"
cat > "$OUT/include/emusuni_soft_threads.h.new" <<'THREADS'
/* Generated by scripts/build-saturn-core.sh, included at the end of vidsoft.c (YabaSanshiro's software renderer): its
   drawing on two threads of its own beside the emulation thread. The VDP1's thread draws its command list into the
   back frame buffer, the VDP2's thread its screens, the sprite layer (from the VDP1's front frame buffer), titan's
   composition and the picture (RGB565). Each draws from a copy of what it reads, taken by the emulation thread where
   upstream draws (Vdp1Draw; VBlank OUT for the VDP2), so they draw what upstream draws. The emulation thread walks the
   VDP1's command list itself without drawing (its registers and status as when it draws), and waits for the VDP1's
   drawing only where the console sees it: the frame buffers' swap (VBlank IN), the CPUs' reads and writes of the
   frame buffer, the next drawing. At VBlank IN it shows the VDP2's picture of the frame if its thread is done, else
   the one before: it waits for the VDP2's drawing only when it takes longer than a frame. Upstream's own threads
   (vidsoft's seven and titan's five) wait in pause() for a wake that never comes (rthreads' condition variables) and
   spin on volatile flags: never started. EMUSUNI_SATURN_THREADS=0 (the PC's measurement): everything on the emulation
   thread, as upstream. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct EmuSoftWorker
{
   unsigned id;
   pthread_mutex_t mutex;
   pthread_cond_t job, idle;
   int pending, busy, quit, started;
   int tag, done_tag;   /* the job's (the VDP2's: its picture), the last one done (-1: none) */
   void (*run)(int tag);
   long long cpu_ns;    /* its jobs' CPU time since the last log line */
   unsigned jobs;
} EmuSoftWorker;

/* The VDP1's drawing: what it draws from (as Vdp1Draw started it), what it draws into (the back frame buffer then). */
typedef struct EmuVdp1Job
{
   Vdp1 regs;
   u8 *target;
   unsigned long long seq; /* the drawings' count, this one's */
   u8 ram[0x80000];
} EmuVdp1Job;

/* The VDP2's drawing: its state at VBlank OUT, the VDP1's registers then and its front frame buffer (the sprite
   layer, read in place once the drawing into it is done; the emulation thread draws, erases or writes it only after
   the picture is done with it: emu_vdp2_release). */
typedef struct EmuVdp2Job
{
   Vdp2 regs;
   Vdp1 vdp1_regs;
   int screens;   /* 0: the display off (the back screen only) */
   u8 *front;     /* the VDP1's front frame buffer, read once the drawing into it is done */
   unsigned long long front_seq;
   int odd;
   Vdp2 lines[270];
   struct CellScrollData cell_scroll_data[270];
   u8 color_ram[0x1000];
   u8 ram[0x80000];
} EmuVdp2Job;

/* The VDP1's; the VDP2's, and its helper (the second band); the VDP1's helper (00.69: its second band of rows). */
static EmuSoftWorker emu_w1, emu_w2, emu_w3, emu_w4;
static EmuVdp1Job *emu_j1;
static EmuVdp2Job *emu_j2;
/* The pictures (RGB565): the one shown, the one before it (the app may still draw it), the one being made. */
static u16 *emu_out[3];
static int emu_out_w[3], emu_out_h[3];
static int emu_out_next;
static int emu_frame_kicked;   /* the VDP2's drawing of this frame started (VBlank OUT) */
static int emu_present_wait;   /* EMUSUNI_SATURN_WAIT=1 (the PC's picture check): always this frame's picture */
const void *emusuni_soft_frame;
int emusuni_soft_w = 320, emusuni_soft_h = 224;
/* For the log (every 600 frames) and the PC's measurement (emusuni_soft_stats_ns, totals): the VDP1's and the VDP2's
   threads' CPU time, the emulation thread's waits for each, its copies (wall time); pictures shown late. */
long long emusuni_soft_stats_ns[10];
static long long emu_wait1_ns, emu_wait2_ns, emu_copy_ns, emu_wait_cpu_ns;
static unsigned emu_waits_cpu;
/* The VDP2's bands: the boundary of the layers' (moved toward the thread with less to do), their last CPU times. */
static int emu_split = -1, emu_sprite_done, emu_titan_simplified;
static long long emu_phase_ns[2], emu_band_wait_ns[2];
/* The VDP1's drawings: kicked (the emulation thread), done (its thread, under emu_w1.mutex); the last one into each
   frame buffer; the front buffer the VDP2's thread is still to copy (under emu_w2.mutex; NULL: none). */
static unsigned long long emu_vdp1_kicked, emu_vdp1_done, emu_buf_seq[2];
static u8 *emu_vdp2_front_pending;
static long long emu_vdp2_wait1_ns; /* the VDP2's thread's waits for the VDP1's drawing (for the log) */
static unsigned emu_frames, emu_late, emu_draws1;
/* The picture shown (VIDSoftVdp2DrawEnd, 00.69): "a frame later" or "now", the frames in a row whose own picture was
   done in time, the run that switches back to "now"; pictures started; for the log: switches, frames a frame later. */
static int emu_later, emu_ready_run, emu_later_need = 600;
static unsigned emu_pictures, emu_switches, emu_frames_later;

/* (00.69) vdp1.cpp's and vdp2.cpp's: the 4 KiB pages of their memory written since the last copy. A copy of 512 KiB
   (the VDP1's for a drawing, the VDP2's for a picture) takes only those: the copy is the threads' own, untouched since
   it was taken (the next is taken only once its job is done), so the other pages are as they are in the console. */
extern unsigned char emusuni_vdp1_dirty[128], emusuni_vdp2_dirty[128];
static unsigned emu_pages_copied;
static void emu_copy_pages(u8 *copy, const u8 *live, unsigned char *dirty)
{
   int p;
   for (p = 0; p < 128; p++)
   {
      if (dirty[p])
      {
         dirty[p] = 0;
         memcpy(copy + (p << 12), live + (p << 12), 0x1000);
         emu_pages_copied++;
      }
   }
}

static long long emu_now(clockid_t clock)
{
   struct timespec t;
   clock_gettime(clock, &t);
   return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void *emu_worker_main(void *arg)
{
   EmuSoftWorker *w = (EmuSoftWorker *)arg;
   pthread_mutex_lock(&w->mutex);
   for (;;)
   {
      int tag;
      long long t;
      while (!w->pending && !w->quit)
         pthread_cond_wait(&w->job, &w->mutex);
      if (w->quit)
         break;
      w->pending = 0;
      w->busy = 1;
      tag = w->tag;
      pthread_mutex_unlock(&w->mutex);
      t = emu_now(CLOCK_THREAD_CPUTIME_ID);
      w->run(tag);
      t = emu_now(CLOCK_THREAD_CPUTIME_ID) - t;
      pthread_mutex_lock(&w->mutex);
      w->cpu_ns += t;
      w->jobs++;
      emusuni_soft_stats_ns[w == &emu_w1 ? 0 : w == &emu_w2 ? 1 : w == &emu_w4 ? 8 : 6] += t;
      w->done_tag = tag;
      if (w == &emu_w1)
         emu_vdp1_done = emu_j1->seq;
      w->busy = 0;
      pthread_cond_broadcast(&w->idle);
   }
   pthread_mutex_unlock(&w->mutex);
   return NULL;
}

static int emu_worker_start(EmuSoftWorker *w, unsigned id, void (*run)(int))
{
   memset(w, 0, sizeof(*w));
   w->id = id;
   w->run = run;
   w->done_tag = -1;
   pthread_mutex_init(&w->mutex, NULL);
   pthread_cond_init(&w->job, NULL);
   pthread_cond_init(&w->idle, NULL);
   /* (Through the app: its core and priority on the PS4, emusuni_saturn_thread "vdp1" / "vdp2".) */
   if (YabThreadStart(id, emu_worker_main, w) != 0)
   {
      pthread_cond_destroy(&w->job);
      pthread_cond_destroy(&w->idle);
      pthread_mutex_destroy(&w->mutex);
      return -1;
   }
   w->started = 1;
   return 0;
}

static void emu_worker_stop(EmuSoftWorker *w)
{
   if (!w->started)
      return;
   pthread_mutex_lock(&w->mutex);
   w->quit = 1;
   pthread_cond_broadcast(&w->job);
   pthread_mutex_unlock(&w->mutex);
   YabThreadWait(w->id);
   pthread_cond_destroy(&w->job);
   pthread_cond_destroy(&w->idle);
   pthread_mutex_destroy(&w->mutex);
   w->started = 0;
}

static void emu_worker_kick(EmuSoftWorker *w, int tag)
{
   pthread_mutex_lock(&w->mutex);
   w->tag = tag;
   w->pending = 1;
   pthread_cond_signal(&w->job);
   pthread_mutex_unlock(&w->mutex);
}

/* Until its job is done; the wall time waited. */
static long long emu_worker_wait(EmuSoftWorker *w)
{
   long long t = 0;
   if (!w->started)
      return 0;
   pthread_mutex_lock(&w->mutex);
   if (w->pending || w->busy)
   {
      t = emu_now(CLOCK_MONOTONIC);
      while (w->pending || w->busy)
         pthread_cond_wait(&w->idle, &w->mutex);
      t = emu_now(CLOCK_MONOTONIC) - t;
   }
   pthread_mutex_unlock(&w->mutex);
   return t;
}

/* ---- The VDP1's drawing, faster (00.69) ----
   Upstream's drawQuad, DrawLine, iterateOverLine, DrawLineCallback, getpixel and putpixel (vidsoft.c) as one inlined
   path: the command's values decoded once per command instead of at every pixel, upstream's per-pixel globals
   (characterWidth and characterHeight, leftColumnColor, currentPixel and currentPixelIsVisible, the gouraud table, the
   quad's edges) in a state of the drawing thread's own (EmuV1), the line's y halved for the interlace without a divide,
   lines with no pixel inside the system clipping (all above, below, left or right of it) left out. The same pixels as
   upstream's, in the same order: the PC compares every picture of both games' runs (hashes) with 00.68's. The state
   upstream keeps from one drawing to the next (the gouraud table, the character size, the last pixel) is copied in
   and out around each drawing (emu_vdp1_run). */

typedef struct EmuV1
{
   int cw, ch;                      /* characterWidth, characterHeight */
   double lr, lg, lb;               /* leftColumnColor */
   u16 ga, gb, gc, gd;              /* gouraudA..D (r bits 0-4, g 5-9, b 10-14) */
   int px, vis;                     /* currentPixel, currentPixelIsVisible */
   int width, pixelsize, interlace; /* the frame buffer's form */
   int r0, r1;                      /* the frame buffer's rows this thread draws: [r0, r1) */
   int xleft[1000], yleft[1000], xright[1000], yright[1000];
} EmuV1;

/* A command's values for its pixels (getpixel's and putpixel's). */
typedef struct EmuV1Cmd
{
   u32 charaddr, colorlut;
   int colorbank, spd, endcodes, flip, textured, untextured, colormode;
   int mesh, msbon, ccmode, userclip, outside, ux1, uy1, ux2, uy2, sx2, sy2, dil;
} EmuV1Cmd;

#define EMU_V1_R(c) ((c) & 0x1F)
#define EMU_V1_G(c) (((c) >> 5) & 0x1F)
#define EMU_V1_B(c) (((c) >> 10) & 0x1F)
#define EMU_V1_COLOR(r, g, b) ((((r) & 0x1F) | (((g) & 0x1F) << 5) | (((b) & 0x1F) << 10) | 0x8000))

static void emu_v1_cmd(EmuV1Cmd *c, const vdp1cmd_struct *cmd, const Vdp1 *regs)
{
   const int shape = cmd->CMDCTRL & 0x7;
   c->charaddr = (u32)cmd->CMDSRCA << 3;
   c->colorbank = cmd->CMDCOLR;
   c->colorlut = (u32)cmd->CMDCOLR << 3;
   c->spd = (cmd->CMDPMOD & 0x40) != 0;
   c->endcodes = (cmd->CMDPMOD & 0x80) == 0;
   c->flip = (cmd->CMDCTRL & 0x30) >> 4;
   c->textured = !(shape == 4 || shape == 5 || shape == 6);
   c->untextured = cmd->CMDCOLR;
   c->colormode = (cmd->CMDPMOD >> 3) & 0x7;
   c->mesh = (cmd->CMDPMOD & 0x0100) != 0;
   c->msbon = (cmd->CMDPMOD & 0x8000) != 0;
   c->ccmode = cmd->CMDPMOD & 0x7;
   c->userclip = (cmd->CMDPMOD & 0x0400) != 0;
   c->outside = ((cmd->CMDPMOD >> 9) & 0x3) == 0x3;
   c->ux1 = regs->userclipX1;
   c->uy1 = regs->userclipY1;
   c->ux2 = regs->userclipX2;
   c->uy2 = regs->userclipY2;
   c->sx2 = regs->systemclipX2;
   c->sy2 = regs->systemclipY2;
   c->dil = (regs->FBCR >> 2) & 1;
}

/* getpixel: 1 for an end code (nothing drawn), else the pixel in v->px (and its visible bits in v->vis). */
static INLINE int emu_v1_get(EmuV1 *v, const EmuV1Cmd *c, int linenumber, int idx, u8 *ram)
{
   int px;
   switch (c->flip)
   {
   case 1:
      idx = v->cw - idx - 1;
      break;
   case 2:
      linenumber = v->ch - linenumber - 1;
      break;
   case 3:
      linenumber = v->ch - linenumber - 1;
      idx = v->cw - idx - 1;
      break;
   }
   switch (c->colormode)
   {
   case 0x0: /* 4bpp bank */
      px = Vdp1ReadPattern16(c->charaddr + (linenumber * (v->cw >> 1)), idx, ram);
      if (c->textured && c->endcodes && px == 0xf)
      {
         v->px = px;
         return 1;
      }
      if (!((px == 0) && !c->spd))
         px = (c->colorbank & 0xfff0) | px;
      v->vis = 0xf;
      break;
   case 0x1: /* 4bpp lut */
      px = Vdp1ReadPattern16(c->charaddr + (linenumber * (v->cw >> 1)), idx, ram);
      if (c->textured && c->endcodes && px == 0xf)
      {
         v->px = px;
         return 1;
      }
      if (!(px == 0 && !c->spd))
         px = T1ReadWord(ram, (px * 2 + c->colorlut) & 0x7FFFF);
      v->vis = 0xffff;
      break;
   case 0x2: /* 8bpp bank (64 colours) */
      px = Vdp1ReadPattern64(c->charaddr + (linenumber * (v->cw)), idx, ram);
      if (c->textured && c->endcodes && px == 63)
         px = 0;
      if (!((px == 0) && !c->spd))
         px = (c->colorbank & 0xffc0) | px;
      v->vis = 0x3f;
      break;
   case 0x3: /* 128 colours */
      px = Vdp1ReadPattern128(c->charaddr + (linenumber * v->cw), idx, ram);
      if (c->textured && c->endcodes && px == 0xff)
      {
         v->px = px;
         return 1;
      }
      if (!((px == 0) && !c->spd))
         px = (c->colorbank & 0xff80) | px;
      v->vis = 0x7f;
      break;
   case 0x4: /* 256 colours */
      px = Vdp1ReadPattern256(c->charaddr + (linenumber * v->cw), idx, ram);
      if (c->textured && c->endcodes && px == 0xff)
      {
         v->px = px;
         return 1;
      }
      v->vis = 0xff;
      if (!((px == 0) && !c->spd))
         px = (c->colorbank & 0xff00) | px;
      break;
   case 0x5: /* 16bpp */
      px = Vdp1ReadPattern64k(c->charaddr + (linenumber * v->cw * 2), idx, ram);
      if (c->textured && c->endcodes && px == 0x7fff)
      {
         v->px = px;
         return 1;
      }
      if (!(px & 0x8000) && !c->spd)
         px = 0;
      v->vis = 0xffff;
      break;
   default: /* (6, 7: none of upstream's cases - the pixel and its visible bits stay as they were) */
      px = v->px;
      break;
   }
   if (!c->textured)
      px = c->untextured;
   v->px = px;
   return 0;
}

/* putpixel (a 16-bit frame buffer). */
static INLINE void emu_v1_put16(EmuV1 *v, const EmuV1Cmd *c, int x, int y, double lr, double lg, double lb, u8 *fb)
{
   u16 *pix;
   int yy, px = v->px;
   if (v->interlace == 2 && (c->dil ? (y & 1) == 0 : (y & 1) != 0)) /* CheckDil */
      return;
   yy = v->interlace == 2 ? y / 2 : y;
   if (yy < v->r0 || yy >= v->r1)
      return; /* another thread's rows */
   pix = &((u16 *)fb)[(yy * v->width) + x];
   if (pix >= (u16 *)(fb + 0x40000))
      return;
   if (c->mesh && ((x ^ yy) & 1))
      return;
   if (c->userclip)
   {
      int user = !(x >= c->ux1 && x <= c->ux2 && y >= c->uy1 && y <= c->uy2);
      if (c->outside)
         user = !user;
      if (user)
         return;
   }
   if (!(x >= 0 && x <= c->sx2 && y >= 0 && y <= c->sy2))
      return;
   if (c->msbon && px)
   {
      *pix |= 0x8000;
      return;
   }
   if (c->spd || (px & v->vis))
   {
      switch (c->ccmode)
      {
      case 0: /* replace */
         if (!((px == 0) && !c->spd))
            *pix = px;
         break;
      case 1: /* shadow */
         if (*pix & (1 << 15))
            *pix = alphablend16(*pix, 0, (1 << 7)) | (1 << 15);
         break;
      case 2: /* half luminance */
         *pix = ((px & ~0x8421) >> 1) | (1 << 15);
         break;
      case 3: /* half transparent */
         if (*pix & (1 << 15))
            *pix = alphablend16(*pix, px, (1 << 7)) | (1 << 15);
         else
            *pix = px;
         break;
      case 4: /* gouraud */
         if ((c->colormode != 5) && (c->colormode != 1) && (int)lg == 16 && (int)lb == 16)
         {
            int k = (int)(lr - 0x10);
            if (k < 0)
               k = 0;
            px = px + k;
            v->px = px;
            *pix = px;
            break;
         }
         *pix = EMU_V1_COLOR(gouraudAdjust(px & 0x001F, (int)lr), gouraudAdjust((px & 0x03e0) >> 5, (int)lg),
                             gouraudAdjust((px & 0x7c00) >> 10, (int)lb));
         break;
      default:
         *pix = alphablend16(EMU_V1_COLOR((int)lr, (int)lg, (int)lb), px, (1 << 7)) | (1 << 15);
         break;
      }
   }
}

/* putpixel8 (an 8-bit frame buffer). */
static INLINE void emu_v1_put8(EmuV1 *v, const EmuV1Cmd *c, int x, int y, u8 *fb)
{
   const int y2 = v->interlace == 2 ? y / 2 : y;
   u8 *pix = &fb[(y2 * v->width) + x];
   if (y2 < v->r0 || y2 >= v->r1)
      return; /* another thread's rows */
   if (pix >= (fb + 0x40000))
      return;
   if (v->interlace == 2 && (c->dil ? (y & 1) == 0 : (y & 1) != 0))
      return;
   v->px &= 0xFF;
   if (c->mesh && ((x ^ y2) & 1))
      return;
   if (c->userclip)
   {
      int user = !(x >= c->ux1 && x <= c->ux2 && y >= c->uy1 && y <= c->uy2);
      if (c->outside)
         user = !user;
      if (user)
         return;
   }
   if (!(x >= 0 && x <= c->sx2 && y >= 0 && y <= c->sy2))
      return;
   if (c->spd || (v->px & v->vis))
   {
      if (!((v->px == 0) && !c->spd))
         *pix = v->px;
   }
}

/* A line none of whose pixels can be drawn by this thread: its pixels' y (and x) lie between its ends', and every
   pixel above, below, left or right of the system clipping is clipped. */
static INLINE int emu_v1_skip(const EmuV1 *v, const EmuV1Cmd *c, int xa, int ya, int xb, int yb)
{
   const int ylo = ya < yb ? ya : yb, yhi = ya < yb ? yb : ya;
   const int xlo = xa < xb ? xa : xb, xhi = xa < xb ? xb : xa;
   if (yhi < 0 || ylo > c->sy2 || xhi < 0 || xlo > c->sx2)
      return 1;
   if (v->r0 > 0 || v->r1 < (1 << 20))
   {
      /* (the rows of its pixels at y >= 0: y, or y / 2 interlaced) */
      const int lo = ylo < 0 ? 0 : (v->interlace == 2 ? ylo / 2 : ylo), hi = v->interlace == 2 ? yhi / 2 : yhi;
      if (hi < v->r0 || lo >= v->r1)
         return 1;
   }
   return 0;
}

/* iterateOverLine with no callback: the line's length (its pixels, greedy ones included), without walking it: the
   walk takes a step along its longer side each time (|major| of them) and one along the shorter side |minor| times in
   all (its error term, of the shorter side's sign, passes the longer side's length floor(k |minor| / |major|) times in
   k steps), the greedy walk a pixel more at each of those, and the end pixel. */
static INLINE int emu_v1_count(int x1, int y1, int x2, int y2, int greedy)
{
   const int dx = abs(x2 - x1), dy = abs(y2 - y1);
   if (dx > 999 || dy > 999)
      return INT_MAX;
   return dx > dy ? dx + (greedy ? dy : 0) + 1 : dy + (greedy ? dx : 0) + 1;
}

/* iterateOverLine with storeLineCoords (not greedy): a quad's edge into xs, ys; its length. */
static int emu_v1_edge(int x1, int y1, int x2, int y2, int *xs, int *ys)
{
   int i = 0, a = 0, ax, ay, dx = x2 - x1, dy = y2 - y1;
   ax = (dx >= 0) ? 1 : -1;
   ay = (dy >= 0) ? 1 : -1;
   if (abs(dx) > 999 || abs(dy) > 999)
      return INT_MAX;
   if (abs(dx) > abs(dy))
   {
      if (ax != ay)
         dx = -dx;
      for (; x1 != x2; x1 += ax, i++)
      {
         xs[i] = x1;
         ys[i] = y1;
         a += dy;
         if (abs(a) >= abs(dx))
         {
            a -= dx;
            y1 += ay;
         }
      }
   }
   else
   {
      if (ax != ay)
         dy = -dy;
      for (; y1 != y2; y1 += ay, i++)
      {
         xs[i] = x1;
         ys[i] = y1;
         a += dx;
         if (abs(a) >= abs(dy))
         {
            a -= dy;
            x1 += ax;
         }
      }
   }
   xs[i] = x2;
   ys[i] = y2;
   return i + 1;
}

/* DrawLine: iterateOverLine with DrawLineCallback in it - each pixel's gouraud step, its texel, two end codes end the
   line. */
/* currentPixelIsVisible of colour modes 0-5 (getpixel's). */
static INLINE int emu_v1_vis(int colormode)
{
   static const int vis[6] = { 0xf, 0xffff, 0x3f, 0x7f, 0xff, 0xffff };
   return vis[colormode];
}

static void emu_v1_line(EmuV1 *v, const EmuV1Cmd *c, int x1, int y1, int x2, int y2, int greedy, double linenumber,
                        double texturestep, double xr, double xg, double xb, u8 *ram, u8 *fb)
{
   int i, a, ax, ay, dx, dy, prev = 123456789, ends = 0;
   const int ln = (int)linenumber, wide = v->pixelsize == 2;
   /* Untextured (polygons, polylines, lines) in colour modes 0-5, getpixel gives every pixel the command's colour and
      the mode's visible bits, never an end code; the gouraud colours are read by colour calculation modes 4-7 only
      (their sums otherwise unused: each shaded line starts them again). */
   const int plain = !c->textured && c->colormode <= 5, shade = wide && c->ccmode >= 4;
   double lr = v->lr, lg = v->lg, lb = v->lb;
   if (plain)
      v->vis = emu_v1_vis(c->colormode);
#define EMU_V1_PIXEL(PX, PY, PI)                                                                                        \
   do                                                                                                                   \
   {                                                                                                                    \
      if (shade)                                                                                                        \
      {                                                                                                                 \
         lr += xr;                                                                                                      \
         lg += xg;                                                                                                      \
         lb += xb;                                                                                                      \
      }                                                                                                                 \
      if (plain)                                                                                                        \
         v->px = c->untextured;                                                                                         \
      else                                                                                                              \
      {                                                                                                                 \
         const int step_ = (int)((PI) * texturestep);                                                                   \
         if (emu_v1_get(v, c, ln, step_, ram))                                                                          \
         {                                                                                                              \
            if (step_ != prev)                                                                                          \
            {                                                                                                           \
               prev = step_;                                                                                            \
               ends++;                                                                                                  \
            }                                                                                                           \
            if (ends == 2)                                                                                              \
               goto done;                                                                                               \
            break;                                                                                                      \
         }                                                                                                              \
      }                                                                                                                 \
      if (wide)                                                                                                         \
         emu_v1_put16(v, c, (PX), (PY), lr, lg, lb, fb);                                                                \
      else                                                                                                              \
         emu_v1_put8(v, c, (PX), (PY), fb);                                                                             \
   } while (0)
   a = i = 0;
   dx = x2 - x1;
   dy = y2 - y1;
   ax = (dx >= 0) ? 1 : -1;
   ay = (dy >= 0) ? 1 : -1;
   if (abs(dx) > 999 || abs(dy) > 999)
      return;
   if (emu_v1_skip(v, c, x1, y1, x2, y2))
      return;
   if (abs(dx) > abs(dy))
   {
      if (ax != ay)
         dx = -dx;
      for (; x1 != x2; x1 += ax, i++)
      {
         EMU_V1_PIXEL(x1, y1, i);
         a += dy;
         if (abs(a) >= abs(dx))
         {
            a -= dx;
            y1 += ay;
            if (greedy)
            {
               i++;
               if (ax == ay)
                  EMU_V1_PIXEL(x1 + ax, y1 - ay, i);
               else
                  EMU_V1_PIXEL(x1, y1, i);
            }
         }
      }
      EMU_V1_PIXEL(x2, y2, i);
   }
   else
   {
      if (ax != ay)
         dy = -dy;
      for (; y1 != y2; y1 += ay, i++)
      {
         EMU_V1_PIXEL(x1, y1, i);
         a += dx;
         if (abs(a) >= abs(dy))
         {
            a -= dy;
            x1 += ax;
            if (greedy)
            {
               i++;
               if (ay == ax)
                  EMU_V1_PIXEL(x1, y1, i);
               else
                  EMU_V1_PIXEL(x1 - ax, y1 + ay, i);
            }
         }
      }
      EMU_V1_PIXEL(x2, y2, i);
   }
done:
#undef EMU_V1_PIXEL
   v->lr = lr;
   v->lg = lg;
   v->lb = lb;
}

/* gouraudTable */
static void emu_v1_gouraud_table(EmuV1 *v, u8 *ram, const vdp1cmd_struct *cmd)
{
   const int address = (((unsigned int)cmd->CMDGRDA) << 3);
   v->ga = T1ReadWord(ram, address);
   v->gb = T1ReadWord(ram, address + 2);
   v->gc = T1ReadWord(ram, address + 4);
   v->gd = T1ReadWord(ram, address + 6);
}

/* drawQuad */
static void emu_v1_quad(EmuV1 *v, s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y,
                        u8 *ram, const Vdp1 *regs, const vdp1cmd_struct *cmd, u8 *fb)
{
   int totalleft, totalright, total, i, y_val;
   double lsr = 0, lsg = 0, lsb = 0, rsr = 0, rsg = 0, rsb = 0; /* topLeftToBottomLeftColorStep, topRight...'s */
   double leftLineStep = 1, rightLineStep = 1;
   u16 colors[4] = { 0, 0, 0, 0 };
   const int gouraud = (cmd->CMDPMOD & (1 << 2)) != 0;
   EmuV1Cmd c;
   /* is_pre_clipped (vdp1interlace is 1 or 2 here: its y doubled as upstream's) */
   y_val = regs->systemclipY2;
   if (v->interlace)
      y_val *= 2;
   if ((tl_x < 0 && bl_x < 0 && tr_x < 0 && br_x < 0) ||
       (tl_x > regs->systemclipX2 && bl_x > regs->systemclipX2 && tr_x > regs->systemclipX2 &&
        br_x > regs->systemclipX2) ||
       (tl_y < 0 && bl_y < 0 && tr_y < 0 && br_y < 0) ||
       (tl_y > y_val && bl_y > y_val && tr_y > y_val && br_y > y_val))
      return;
   v->cw = ((cmd->CMDSIZE >> 8) & 0x3F) * 8;
   v->ch = cmd->CMDSIZE & 0xFF;
   totalleft = emu_v1_edge(tl_x, tl_y, bl_x, bl_y, v->xleft, v->yleft);
   totalright = emu_v1_edge(tr_x, tr_y, br_x, br_y, v->xright, v->yright);
   if (totalleft == INT_MAX || totalright == INT_MAX)
      return;
   total = totalleft > totalright ? totalleft : totalright;
   if (gouraud)
   {
      emu_v1_gouraud_table(v, ram, cmd);
      colors[0] = v->ga;
      colors[1] = v->gd;
      colors[2] = v->gb;
      colors[3] = v->gc;
      lsr = interpolate(EMU_V1_R(colors[0]), EMU_V1_R(colors[1]), total);
      lsg = interpolate(EMU_V1_G(colors[0]), EMU_V1_G(colors[1]), total);
      lsb = interpolate(EMU_V1_B(colors[0]), EMU_V1_B(colors[1]), total);
      rsr = interpolate(EMU_V1_R(colors[2]), EMU_V1_R(colors[3]), total);
      rsg = interpolate(EMU_V1_G(colors[2]), EMU_V1_G(colors[3]), total);
      rsb = interpolate(EMU_V1_B(colors[2]), EMU_V1_B(colors[3]), total);
   }
   if (total == totalleft && totalleft != totalright)
   {
      leftLineStep = 1;
      rightLineStep = (double)totalright / totalleft;
   }
   else if (totalleft != totalright)
   {
      rightLineStep = 1;
      leftLineStep = (double)totalleft / totalright;
   }
   emu_v1_cmd(&c, cmd, regs);
   for (i = 0; i < total; i++)
   {
      const int li = (int)(i * leftLineStep), ri = (int)(i * rightLineStep);
      const int xl = v->xleft[li], yl = v->yleft[li], xr = v->xright[ri], yr = v->yright[ri];
      int xlinelength;
      double xtexturestep, ytexturestep, ltr = 0, ltg = 0, ltb = 0;
      if (emu_v1_skip(v, &c, xl, yl, xr, yr))
         continue; /* (no pixel of it drawn here: its length, steps and colours unused) */
      xlinelength = emu_v1_count(xl, yl, xr, yr, 1);
      xtexturestep = interpolate(0, v->cw, xlinelength);
      ytexturestep = interpolate(0, v->ch, total);
      if (gouraud)
      {
         double rr, rg, rb;
         v->lr = EMU_V1_R(colors[0]) + (lsr * i);
         v->lg = EMU_V1_G(colors[0]) + (lsg * i);
         v->lb = EMU_V1_B(colors[0]) + (lsb * i);
         rr = EMU_V1_R(colors[2]) + (rsr * i);
         rg = EMU_V1_G(colors[2]) + (rsg * i);
         rb = EMU_V1_B(colors[2]) + (rsb * i);
         ltr = interpolate(v->lr, rr, xlinelength);
         ltg = interpolate(v->lg, rg, xlinelength);
         ltb = interpolate(v->lb, rb, xlinelength);
      }
      emu_v1_line(v, &c, xl, yl, xr, yr, 1, ytexturestep * i, xtexturestep, ltr, ltg, ltb, ram, fb);
   }
}

static void emu_v1_normal(EmuV1 *v, u8 *ram, const Vdp1 *regs, u8 *fb)
{
   s16 topLeftx, topLefty, topRightx, topRighty, bottomRightx, bottomRighty, bottomLeftx, bottomLefty;
   int spriteWidth, spriteHeight;
   vdp1cmd_struct cmd;
   Vdp1ReadCommand(&cmd, regs->addr, ram);
   topLeftx = cmd.CMDXA + regs->localX;
   topLefty = cmd.CMDYA + regs->localY;
   spriteWidth = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
   spriteHeight = cmd.CMDSIZE & 0xFF;
   topRightx = topLeftx + (spriteWidth - 1);
   topRighty = topLefty;
   bottomRightx = topLeftx + (spriteWidth - 1);
   bottomRighty = topLefty + (spriteHeight - 1);
   bottomLeftx = topLeftx;
   bottomLefty = topLefty + (spriteHeight - 1);
   emu_v1_quad(v, topLeftx, topLefty, bottomLeftx, bottomLefty, topRightx, topRighty, bottomRightx, bottomRighty, ram,
               regs, &cmd, fb);
}

static void emu_v1_scaled(EmuV1 *v, u8 *ram, const Vdp1 *regs, u8 *fb)
{
   s32 topLeftx, topLefty, topRightx, topRighty, bottomRightx, bottomRighty, bottomLeftx, bottomLefty;
   int x0, y0, x1, y1;
   vdp1cmd_struct cmd;
   Vdp1ReadCommand(&cmd, regs->addr, ram);
   x0 = cmd.CMDXA + regs->localX;
   y0 = cmd.CMDYA + regs->localY;
   switch ((cmd.CMDCTRL >> 8) & 0xF)
   {
   case 0x0: /* only two coordinates */
   default:
      x1 = ((int)cmd.CMDXC) - x0 + regs->localX + 1;
      y1 = ((int)cmd.CMDYC) - y0 + regs->localY + 1;
      break;
   case 0x5: /* upper-left */
      x1 = ((int)cmd.CMDXB) + 1;
      y1 = ((int)cmd.CMDYB) + 1;
      break;
   case 0x6: /* upper-center */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      x0 = x0 - x1 / 2;
      x1++;
      y1++;
      break;
   case 0x7: /* upper-right */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      x0 = x0 - x1;
      x1++;
      y1++;
      break;
   case 0x9: /* center-left */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      y0 = y0 - y1 / 2;
      x1++;
      y1++;
      break;
   case 0xA: /* center-center */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      x0 = x0 - x1 / 2;
      y0 = y0 - y1 / 2;
      x1++;
      y1++;
      break;
   case 0xB: /* center-right */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      x0 = x0 - x1;
      y0 = y0 - y1 / 2;
      x1++;
      y1++;
      break;
   case 0xD: /* lower-left */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      y0 = y0 - y1;
      x1++;
      y1++;
      break;
   case 0xE: /* lower-center */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      x0 = x0 - x1 / 2;
      y0 = y0 - y1;
      x1++;
      y1++;
      break;
   case 0xF: /* lower-right */
      x1 = ((int)cmd.CMDXB);
      y1 = ((int)cmd.CMDYB);
      x0 = x0 - x1;
      y0 = y0 - y1;
      x1++;
      y1++;
      break;
   }
   topLeftx = x0;
   topLefty = y0;
   topRightx = x1 + x0 - 1;
   topRighty = topLefty;
   bottomRightx = x1 + x0 - 1;
   bottomRighty = y1 + y0 - 1;
   bottomLeftx = topLeftx;
   bottomLefty = y1 + y0 - 1;
   emu_v1_quad(v, topLeftx, topLefty, bottomLeftx, bottomLefty, topRightx, topRighty, bottomRightx, bottomRighty, ram,
               regs, &cmd, fb);
}

static void emu_v1_distorted(EmuV1 *v, u8 *ram, const Vdp1 *regs, u8 *fb)
{
   s32 xa, ya, xb, yb, xc, yc, xd, yd;
   vdp1cmd_struct cmd;
   Vdp1ReadCommand(&cmd, regs->addr, ram);
   xa = (s32)(cmd.CMDXA + regs->localX);
   ya = (s32)(cmd.CMDYA + regs->localY);
   xb = (s32)(cmd.CMDXB + regs->localX);
   yb = (s32)(cmd.CMDYB + regs->localY);
   xc = (s32)(cmd.CMDXC + regs->localX);
   yc = (s32)(cmd.CMDYC + regs->localY);
   xd = (s32)(cmd.CMDXD + regs->localX);
   yd = (s32)(cmd.CMDYD + regs->localY);
   emu_v1_quad(v, xa, ya, xd, yd, xb, yb, xc, yc, ram, regs, &cmd, fb);
}

/* gouraudLineSetup: the table read after its two colours were taken (upstream passes the ones read before). */
static void emu_v1_gouraud_line(EmuV1 *v, double *redstep, double *greenstep, double *bluestep, int length, u16 table1,
                                u16 table2, u8 *ram, const vdp1cmd_struct *cmd)
{
   emu_v1_gouraud_table(v, ram, cmd);
   *redstep = interpolate(EMU_V1_R(table1), EMU_V1_R(table2), length);
   *greenstep = interpolate(EMU_V1_G(table1), EMU_V1_G(table2), length);
   *bluestep = interpolate(EMU_V1_B(table1), EMU_V1_B(table2), length);
   v->lr = EMU_V1_R(table1);
   v->lg = EMU_V1_G(table1);
   v->lb = EMU_V1_B(table1);
}

static void emu_v1_polyline(EmuV1 *v, u8 *ram, const Vdp1 *regs, u8 *fb)
{
   int X[4], Y[4], length;
   double redstep = 0, greenstep = 0, bluestep = 0;
   vdp1cmd_struct cmd;
   EmuV1Cmd c;
   Vdp1ReadCommand(&cmd, regs->addr, ram);
   emu_v1_cmd(&c, &cmd, regs);
   X[0] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x0C));
   Y[0] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x0E));
   X[1] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x10));
   Y[1] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x12));
   X[2] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x14));
   Y[2] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x16));
   X[3] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x18));
   Y[3] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x1A));
   length = emu_v1_count(X[0], Y[0], X[1], Y[1], 1);
   emu_v1_gouraud_line(v, &redstep, &greenstep, &bluestep, length, v->ga, v->gb, ram, &cmd);
   emu_v1_line(v, &c, X[0], Y[0], X[1], Y[1], 0, 0, 0, redstep, greenstep, bluestep, ram, fb);
   length = emu_v1_count(X[1], Y[1], X[2], Y[2], 1);
   emu_v1_gouraud_line(v, &redstep, &greenstep, &bluestep, length, v->gb, v->gc, ram, &cmd);
   emu_v1_line(v, &c, X[1], Y[1], X[2], Y[2], 0, 0, 0, redstep, greenstep, bluestep, ram, fb);
   length = emu_v1_count(X[2], Y[2], X[3], Y[3], 1);
   emu_v1_gouraud_line(v, &redstep, &greenstep, &bluestep, length, v->gd, v->gc, ram, &cmd);
   emu_v1_line(v, &c, X[3], Y[3], X[2], Y[2], 0, 0, 0, redstep, greenstep, bluestep, ram, fb);
   length = emu_v1_count(X[3], Y[3], X[0], Y[0], 1);
   emu_v1_gouraud_line(v, &redstep, &greenstep, &bluestep, length, v->ga, v->gd, ram, &cmd);
   emu_v1_line(v, &c, X[0], Y[0], X[3], Y[3], 0, 0, 0, redstep, greenstep, bluestep, ram, fb);
}

static void emu_v1_lineDraw(EmuV1 *v, u8 *ram, const Vdp1 *regs, u8 *fb)
{
   int x1, y1, x2, y2, length;
   double redstep = 0, greenstep = 0, bluestep = 0;
   vdp1cmd_struct cmd;
   EmuV1Cmd c;
   Vdp1ReadCommand(&cmd, regs->addr, ram);
   emu_v1_cmd(&c, &cmd, regs);
   x1 = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x0C));
   y1 = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x0E));
   x2 = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x10));
   y2 = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x12));
   length = emu_v1_count(x1, y1, x2, y2, 1);
   /* (upstream's order: its green and blue steps the other way round) */
   emu_v1_gouraud_line(v, &redstep, &bluestep, &greenstep, length, v->ga, v->gb, ram, &cmd);
   emu_v1_line(v, &c, x1, y1, x2, y2, 0, 0, 0, redstep, greenstep, bluestep, ram, fb);
}

/* ---- The VDP1 ---- */

static void emu_vdp1_wait(void)
{
   const long long t = emu_worker_wait(&emu_w1);
   emu_wait1_ns += t;
   emusuni_soft_stats_ns[2] += t;
}

/* Until the drawing seq (and those before it) is done: any thread. */
static void emu_vdp1_wait_seq(unsigned long long seq)
{
   if (!emu_w1.started)
      return;
   pthread_mutex_lock(&emu_w1.mutex);
   while (emu_vdp1_done < seq)
      pthread_cond_wait(&emu_w1.idle, &emu_w1.mutex);
   pthread_mutex_unlock(&emu_w1.mutex);
}

static int emu_buf_index(const u8 *buffer)
{
   return buffer == vdp1framebuffer[1] ? 1 : 0;
}

/* A frame buffer about to be drawn, erased or written: once the VDP2's thread has copied it, if its picture is to
   (it was the front one when that picture started). */
static void emu_vdp2_release(const u8 *buffer)
{
   int pending;
   if (!emu_w2.started)
      return;
   pthread_mutex_lock(&emu_w2.mutex);
   pending = emu_vdp2_front_pending == buffer;
   pthread_mutex_unlock(&emu_w2.mutex);
   if (pending)
   {
      const long long t = emu_worker_wait(&emu_w2);
      emu_wait2_ns += t;
      emusuni_soft_stats_ns[3] += t;
   }
}

/* The CPUs' reads and writes of the drawn frame buffer: after its drawing (counted apart). */
static void emu_vdp1_wait_cpu(void)
{
   long long t;
   if (!emu_threads)
      return;
   t = emu_worker_wait(&emu_w1);
   emu_wait_cpu_ns += t;
   emusuni_soft_stats_ns[7] += t;
   if (t > 0)
      emu_waits_cpu++;
   emu_vdp2_release(vdp1backframebuffer); /* (a picture still to copy it, after a change) */
}

/* The frame buffer's form (VIDSoftVdp1DrawStartBody's): its width, height and bytes a pixel. */
static void emu_vdp1_form(const Vdp1 *regs, int *width, int *height, int *pixelsize)
{
   if (regs->TVMR & 0x1)
   {
      *width = (regs->TVMR & 0x2) ? 512 : 1024;
      *height = (regs->TVMR & 0x2) ? 512 : 256;
      *pixelsize = 1;
   }
   else
   {
      *width = 512;
      *height = 256;
      *pixelsize = 2;
   }
}

/* VIDSoftVdp1EraseFrameBuffer's erase (EWLR to EWRR with EWDR), of a frame buffer of that form. */
static void emu_vdp1_erase(Vdp1 *regs, u8 *back_framebuffer, int vdp1width, int vdp1height, int vdp1pixelsize)
{
   int i, i2, w, h;
   h = (regs->EWRR & 0x1FF) + 1;
   if (h > vdp1height) h = vdp1height;
   w = ((regs->EWRR >> 6) & 0x3F8) + 8;
   if (w > vdp1width) w = vdp1width;
   if (vdp1pixelsize == 2)
   {
      for (i2 = (regs->EWLR & 0x1FF); i2 < h; i2++)
      {
         for (i = ((regs->EWLR >> 6) & 0x1F8); i < w; i++)
            ((u16 *)back_framebuffer)[(i2 * vdp1width) + i] = regs->EWDR;
      }
   }
   else
   {
      w = regs->EWRR >> 9;
      w *= 16;
      for (i2 = (regs->EWLR & 0x1FF); i2 < h; i2++)
      {
         for (i = ((regs->EWLR >> 6) & 0x1F8); i < w; i++)
         {
            int pos = (i2 * vdp1width) + i;
            if (pos < 0x3FFFF)
               back_framebuffer[pos] = regs->EWDR & 0xFF;
         }
      }
   }
}

/* vdp1.cpp's Vdp1DrawCommands on the copy, drawing: the same walk (its status and registers are the emulation's own
   walk's), without its lock (the emulation thread's walk holds it) and without the console's status. The drawing is
   emu_v1_*'s (above), in the thread's own state v. */
static void emu_vdp1_draw_list(EmuV1 *v, u8 *ram, Vdp1 *regs, u8 *back_framebuffer)
{
   u32 returnAddr = 0xffffffff;
   int command_count = 0;
   u16 command;
   if (regs->addr > 0x7FFFF)
      return;
   command = T1ReadWord(ram, regs->addr);
   while (!(command & 0x8000) && command_count < 4096)
   {
      if (!(command & 0x4000))
      {
         switch (command & 0x000F)
         {
         case 0: emu_v1_normal(v, ram, regs, back_framebuffer); break;
         case 1: emu_v1_scaled(v, ram, regs, back_framebuffer); break;
         case 2: case 3: case 4: emu_v1_distorted(v, ram, regs, back_framebuffer); break;
         case 5: case 7: emu_v1_polyline(v, ram, regs, back_framebuffer); break;
         case 6: emu_v1_lineDraw(v, ram, regs, back_framebuffer); break;
         case 8: case 11: VIDSoftVdp1UserClipping(ram, regs); break;
         case 9: VIDSoftVdp1SystemClipping(ram, regs); break;
         case 10: VIDSoftVdp1LocalCoordinate(ram, regs); break;
         default:
            regs->EDSR |= 2;
            return;
         }
      }
      if (regs->EDSR & 0x02)
         return;
      switch ((command & 0x3000) >> 12)
      {
      case 0:
         regs->addr += 0x20;
         break;
      case 1:
         regs->addr = T1ReadWord(ram, regs->addr + 2) * 8;
         if (regs->addr == 0)
            return;
         break;
      case 2:
         if (returnAddr == 0xFFFFFFFF)
            returnAddr = regs->addr + 0x20;
         regs->addr = T1ReadWord(ram, regs->addr + 2) * 8;
         if (regs->addr == 0)
            return;
         break;
      case 3:
         if (returnAddr != 0xFFFFFFFF)
         {
            regs->addr = returnAddr;
            returnAddr = 0xFFFFFFFF;
         }
         else
            regs->addr += 0x20;
         if (regs->addr == 0)
            return;
         break;
      }
      command = T1ReadWord(ram, regs->addr & 0x7FFFF);
      command_count++;
   }
}

/* The drawing threads' states (the lead's, the helper's). */
static EmuV1 *emu_v1[2];
/* The VDP1's drawing in two bands of the frame buffer's rows (00.69: its lead and its helper each walk the whole
   command list, each draws the pixels of its rows; the pixels and their order in each row are the one thread's): the
   boundary, moved 8 rows a drawing toward the band that took less, and the bands' last CPU times. */
static int emu_v1_split = -1;
static long long emu_v1_ns[2];

/* A band's drawing: thread k's state set from the drawing's form and from upstream's state between drawings. */
static void emu_vdp1_band(int k, int r0, int r1)
{
   EmuVdp1Job *j = emu_j1;
   Vdp1 regs = j->regs; /* (the walk moves its address and clipping: each band's own) */
   EmuV1 *v = emu_v1[k];
   const long long t = emu_now(CLOCK_THREAD_CPUTIME_ID);
   v->interlace = vdp1interlace;
   v->width = vdp1width;
   v->pixelsize = vdp1pixelsize;
   v->r0 = r0;
   v->r1 = r1;
   v->cw = characterWidth;
   v->ch = characterHeight;
   v->lr = leftColumnColor.r;
   v->lg = leftColumnColor.g;
   v->lb = leftColumnColor.b;
   v->ga = gouraudA.value;
   v->gb = gouraudB.value;
   v->gc = gouraudC.value;
   v->gd = gouraudD.value;
   v->px = currentPixel;
   v->vis = currentPixelIsVisible;
   emu_vdp1_draw_list(v, j->ram, &regs, j->target);
   emu_v1_ns[k] = emu_now(CLOCK_THREAD_CPUTIME_ID) - t;
}

/* The helper: the band from the boundary down. */
static void emu_vdp1_help(int tag)
{
   (void)tag;
   emu_vdp1_band(1, emu_v1_split, 1 << 30);
}

/* The VDP1's thread: VIDSoftVdp1DrawStartBody's frame buffer form, the drawing (with its helper, the rows in two
   bands); upstream's state kept between drawings (its globals: the gouraud table, the character size, the last pixel)
   out from the lead's band (the helper's walk sets the same values). */
static void emu_vdp1_run(int tag)
{
   EmuVdp1Job *j = emu_j1;
   const int help = emu_w4.started;
   EmuV1 *v = emu_v1[0];
   (void)tag;
   vdp1interlace = (j->regs.FBCR & 8) ? 2 : 1;
   emu_vdp1_form(&j->regs, &vdp1width, &vdp1height, &vdp1pixelsize);
   if (help)
   {
      const int low = vdp1height / 16, high = vdp1height - vdp1height / 16;
      if (emu_v1_split < low || emu_v1_split > high)
         emu_v1_split = vdp1height == 256 ? 112 : vdp1height / 2;
      emu_worker_kick(&emu_w4, 0);
      emu_vdp1_band(0, 0, emu_v1_split);
      emu_worker_wait(&emu_w4);
      /* The next drawing's boundary: 8 rows toward the band that took less. */
      if (emu_v1_ns[0] * 10 > emu_v1_ns[1] * 11)
         emu_v1_split -= 8;
      else if (emu_v1_ns[1] * 10 > emu_v1_ns[0] * 11)
         emu_v1_split += 8;
      if (emu_v1_split < low)
         emu_v1_split = low;
      if (emu_v1_split > high)
         emu_v1_split = high;
   }
   else
      emu_vdp1_band(0, 0, 1 << 30);
   characterWidth = v->cw;
   characterHeight = v->ch;
   leftColumnColor.r = v->lr;
   leftColumnColor.g = v->lg;
   leftColumnColor.b = v->lb;
   gouraudA.value = v->ga;
   gouraudB.value = v->gb;
   gouraudC.value = v->gc;
   gouraudD.value = v->gd;
   currentPixel = v->px;
   currentPixelIsVisible = v->vis;
}

void VIDSoftVdp1DrawStart(void)
{
   long long t;
   if (!emu_threads)
   {
      emu_vdp1_start_sync();
      return;
   }
   emu_vdp1_wait(); /* the drawing before (its copy is this one's) */
   emu_vdp2_release(vdp1backframebuffer);
   t = emu_now(CLOCK_MONOTONIC);
   emu_copy_pages(emu_j1->ram, Vdp1Ram, emusuni_vdp1_dirty);
   memcpy(&emu_j1->regs, Vdp1Regs, sizeof(Vdp1));
   emu_j1->target = vdp1backframebuffer;
   emu_j1->seq = ++emu_vdp1_kicked;
   emu_buf_seq[emu_buf_index(vdp1backframebuffer)] = emu_j1->seq;
   t = emu_now(CLOCK_MONOTONIC) - t;
   emu_copy_ns += t;
   emusuni_soft_stats_ns[4] += t;
   emu_worker_kick(&emu_w1, 0);
   emu_draws1++;
   /* The console's walk of the command list: its registers and status as when it draws, nothing drawn. */
   Vdp1DrawCommands(Vdp1Ram, Vdp1Regs, NULL);
}

/* VBlank OUT (vdp2.cpp): the displayed frame buffer erased (VBlank erase, one-cycle mode, or a manual erase before
   the change: it becomes the drawn one), as the GL renderer's YglEraseWriteVDP1 erases its read frame. The VDP1's
   thread draws the other one; the VDP2's thread shows its own copy. */
void VIDSoftVdp1EraseWrite(void)
{
   int width, height, pixelsize;
   if (emu_threads)
   {
      const long long t = emu_now(CLOCK_MONOTONIC);
      emu_vdp1_wait_seq(emu_buf_seq[emu_buf_index(vdp1frontframebuffer)]);
      emu_wait1_ns += emu_now(CLOCK_MONOTONIC) - t;
      emu_vdp2_release(vdp1frontframebuffer);
   }
   emu_vdp1_form(Vdp1Regs, &width, &height, &pixelsize);
   emu_vdp1_erase(Vdp1Regs, vdp1frontframebuffer, width, height, pixelsize);
}

/* VBlank OUT (vdp2.cpp): the frame buffers change (one-cycle mode, a manual change), the drawn one shown. Its drawing
   may still be on its thread: the picture waits for it (the VDP2's thread, emu_vdp2_run), not the emulation (Sonic R
   starts its drawing just before the change: the emulation thread waited for all of it). */
void VIDSoftVdp1FrameChange(void)
{
   u8 *temp;
   if (!emu_threads)
      VidsoftWaitForVdp1Thread();
   temp = vdp1frontframebuffer;
   vdp1frontframebuffer = vdp1backframebuffer;
   vdp1backframebuffer = temp;
}

/* ---- The VDP2 ---- */

static void emu_use_live_vdp2(void)
{
   emusuni_v2regs = *emu_live_v2regs;
   emu_v2ram = *emu_live_v2ram;
   emu_v2cram = *emu_live_v2cram;
   emu_v2odd = *emu_live_odd;
}

/* Titan's picture (0xAABBGGRR) as RGB565. */
static void emu_to_rgb565(const u32 *in, u16 *out, int w, int h)
{
   const int n = w * h;
   int i;
   for (i = 0; i < n; i++)
   {
      const u32 c = in[i];
      out[i] = (u16)(((c << 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 19) & 0x001F));
   }
}

/* A band's layers (k: 0 the lead's, 1 the helper's): the scroll screens (the helper RBG0 whole: its rotation's
   per-line state isn't split) while the VDP1's drawing may still be on its thread, then the sprite layer once it is
   done, unless it was drawn whole first. */
static void emu_vdp2_layers(int k)
{
   EmuVdp2Job *j = emu_j2;
   Vdp2 *regs = &emu_band_regs[k];
   long long t = emu_now(CLOCK_THREAD_CPUTIME_ID), w;
   Vdp2DrawNBG0(j->lines, regs, j->ram, j->color_ram, j->cell_scroll_data);
   Vdp2DrawNBG1(j->lines, regs, j->ram, j->color_ram, j->cell_scroll_data);
   Vdp2DrawNBG2(j->lines, regs, j->ram, j->color_ram, j->cell_scroll_data);
   Vdp2DrawNBG3(j->lines, regs, j->ram, j->color_ram, j->cell_scroll_data);
   if (k == 1)
      Vdp2DrawRBG0(j->lines, &j->regs, j->ram, j->color_ram, j->cell_scroll_data);
   if (!emu_sprite_done)
   {
      w = emu_now(CLOCK_MONOTONIC);
      emu_vdp1_wait_seq(j->front_seq);
      emu_band_wait_ns[k] = emu_now(CLOCK_MONOTONIC) - w;
      VidsoftDrawSprite(regs, sprite_window_mask, j->front, j->ram, &j->vdp1_regs, j->lines, j->color_ram);
   }
   emu_phase_ns[k] = emu_now(CLOCK_THREAD_CPUTIME_ID) - t;
}

/* A band's composition (titan) and RGB565 picture: lines [y0, y1); then the layers' rows it read erased for the next
   drawing (titan.c: emusuni_titan_erase_rows - TitanErase's work, band by band). */
static void emu_vdp2_compose(int y0, int y1, int out)
{
   if (y1 <= y0)
      return;
   if (emu_titan_simplified >= 0)
      TitanRenderSimplifiedCheck(dispbuffer, y0, y1, emu_titan_simplified);
   emu_to_rgb565(dispbuffer + y0 * vdp2width, emu_out[out] + y0 * vdp2width, vdp2width, y1 - y0);
   emusuni_titan_erase_rows(vdp2_interlace ? y0 / 2 : y0, vdp2_interlace ? y1 / 2 : y1);
}

/* The helper: (1 << 4 | picture) its band's layers, (2 << 4 | picture) its band's composition. */
static void emu_vdp2_help(int tag)
{
   if ((tag >> 4) == 1)
      emu_vdp2_layers(1);
   else
      emu_vdp2_compose((vdp2height / 2) & ~1, vdp2height, tag & 15);
}

/* The VDP2's thread: upstream's Vdp2DrawStart, Vdp2DrawScreens and Vdp2DrawEnd's drawing, on the copy; the layers
   and the composition in two bands of lines with its helper. */
static void emu_vdp2_run(int out)
{
   EmuVdp2Job *j = emu_j2;
   const int help = emu_w3.started;
   int h, middle;
   emusuni_v2regs = &j->regs;
   emu_v2ram = j->ram;
   emu_v2cram = j->color_ram;
   emu_v2odd = j->odd;
   VIDSoftVdp2SetResolution(j->regs.TVMD);
   emu_vdp2_start_body();
   /* (No TitanErase: the layers were erased after the composition before - emu_vdp2_compose.) */
   emu_titan_simplified = emusuni_titan_prepare();
   h = vdp2height;
   if (j->screens)
   {
      const int low = (h / 4) & ~1, high = (3 * h / 4) & ~1;
      emu_band_wait_ns[0] = emu_band_wait_ns[1] = 0;
      if (emu_split < low || emu_split > high)
         emu_split = (h / 2) & ~1;
      memcpy(&emu_band_regs[0], &j->regs, sizeof(Vdp2));
      memcpy(&emu_band_regs[1], &j->regs, sizeof(Vdp2));
      emu_band_y0[0] = 0;
      emu_band_y1[0] = help ? emu_split : 1 << 30;
      emu_band_y0[1] = emu_split;
      emu_band_y1[1] = 1 << 30;
      /* The sprite window (SPCTL bit 4): its mask is made by the sprite layer and read by the others, by
         positions of their own - the sprite layer whole first. */
      emu_sprite_done = 0;
      if (j->regs.SPCTL & 0x10)
      {
         const long long w = emu_now(CLOCK_MONOTONIC);
         emu_vdp1_wait_seq(j->front_seq);
         emu_band_wait_ns[0] = emu_now(CLOCK_MONOTONIC) - w;
         VidsoftDrawSprite(&j->regs, sprite_window_mask, j->front, j->ram, &j->vdp1_regs, j->lines, j->color_ram);
         emu_sprite_done = 1;
      }
      if (help)
      {
         emu_worker_kick(&emu_w3, 1 << 4 | out);
         emu_vdp2_layers(0);
         emu_worker_wait(&emu_w3);
         /* The next frame's boundary: 8 lines toward the band whose thread had less to do. */
         if (emu_phase_ns[0] * 10 > emu_phase_ns[1] * 11)
            emu_split -= 8;
         else if (emu_phase_ns[1] * 10 > emu_phase_ns[0] * 11)
            emu_split += 8;
         if (emu_split < low)
            emu_split = low;
         if (emu_split > high)
            emu_split = high;
      }
      else
      {
         emu_vdp2_layers(0);
         Vdp2DrawRBG0(j->lines, &j->regs, j->ram, j->color_ram, j->cell_scroll_data);
      }
      /* The front buffer read: the emulation thread may draw, erase or write it again. */
      pthread_mutex_lock(&emu_w2.mutex);
      emu_vdp2_front_pending = NULL;
      emu_vdp2_wait1_ns += emu_band_wait_ns[0] > emu_band_wait_ns[1] ? emu_band_wait_ns[0] : emu_band_wait_ns[1];
      pthread_mutex_unlock(&emu_w2.mutex);
   }
   middle = help ? (h / 2) & ~1 : h;
   if (help)
      emu_worker_kick(&emu_w3, 2 << 4 | out);
   emu_vdp2_compose(0, middle, out);
   if (help)
      emu_worker_wait(&emu_w3);
   /* (Every band's rows erased: the layers are blank.) */
   memset(emusuni_titan_dirty, 0, sizeof(emusuni_titan_dirty));
   emu_out_w[out] = vdp2width;
   emu_out_h[out] = h;
}

static void emu_vdp2_kick(int screens)
{
   EmuVdp2Job *j = emu_j2;
   long long t = emu_worker_wait(&emu_w2); /* the picture before (its copy is this one's) */
   int out;
   emu_wait2_ns += t;
   emusuni_soft_stats_ns[3] += t;
   t = emu_now(CLOCK_MONOTONIC);
   memcpy(&j->regs, *emu_live_v2regs, sizeof(Vdp2));
   memcpy(&j->vdp1_regs, Vdp1Regs, sizeof(Vdp1));
   memcpy(j->lines, Vdp2Lines, sizeof(Vdp2) * 270);
   memcpy(j->cell_scroll_data, cell_scroll_data, sizeof(struct CellScrollData) * 270);
   memcpy(j->color_ram, *emu_live_v2cram, 0x1000);
   emu_copy_pages(j->ram, *emu_live_v2ram, emusuni_vdp2_dirty);
   j->front = vdp1frontframebuffer;
   j->front_seq = emu_buf_seq[emu_buf_index(vdp1frontframebuffer)];
   j->screens = screens;
   j->odd = *emu_live_odd;
   out = emu_out_next;
   emu_out_next = (emu_out_next + 1) % 3;
   emu_pictures++;
   t = emu_now(CLOCK_MONOTONIC) - t;
   emu_copy_ns += t;
   emusuni_soft_stats_ns[4] += t;
   if (screens)
   {
      pthread_mutex_lock(&emu_w2.mutex);
      emu_vdp2_front_pending = j->front;
      pthread_mutex_unlock(&emu_w2.mutex);
   }
   emu_worker_kick(&emu_w2, out);
   emu_frame_kicked = 1;
}

void VIDSoftVdp2DrawStart(void)
{
   if (!emu_threads)
   {
      emu_use_live_vdp2();
      emu_vdp2_start_body();
      return;
   }
   emu_frame_kicked = 0;
}

void VIDSoftVdp2DrawScreens(void)
{
   if (!emu_threads)
   {
      emu_use_live_vdp2();
      emu_vdp2_screens_sync();
      return;
   }
   emu_vdp2_kick(1);
}

/* Every 600 frames, a line for the app's log. */
static void emu_frame_stats(void)
{
   char text[640];
   long long c1 = 0, c2 = 0, c3 = 0, c4 = 0, v21 = 0;
   unsigned j1 = 0, j2 = 0;
   const double n = 600.0;
   if (++emu_frames < 600)
      return;
   if (emu_w1.started)
   {
      pthread_mutex_lock(&emu_w1.mutex);
      c1 = emu_w1.cpu_ns;
      j1 = emu_w1.jobs;
      emu_w1.cpu_ns = 0;
      emu_w1.jobs = 0;
      pthread_mutex_unlock(&emu_w1.mutex);
   }
   if (emu_w2.started)
   {
      pthread_mutex_lock(&emu_w2.mutex);
      c2 = emu_w2.cpu_ns;
      j2 = emu_w2.jobs;
      v21 = emu_vdp2_wait1_ns;
      emu_w2.cpu_ns = 0;
      emu_w2.jobs = 0;
      emu_vdp2_wait1_ns = 0;
      pthread_mutex_unlock(&emu_w2.mutex);
   }
   if (emu_w3.started)
   {
      pthread_mutex_lock(&emu_w3.mutex);
      c3 = emu_w3.cpu_ns;
      emu_w3.cpu_ns = 0;
      emu_w3.jobs = 0;
      pthread_mutex_unlock(&emu_w3.mutex);
   }
   if (emu_w4.started)
   {
      pthread_mutex_lock(&emu_w4.mutex);
      c4 = emu_w4.cpu_ns;
      emu_w4.cpu_ns = 0;
      emu_w4.jobs = 0;
      pthread_mutex_unlock(&emu_w4.mutex);
   }
   if (emu_threads)
      snprintf(text, sizeof(text),
               "renderer: 600 frames: VDP1 threads %.2f + %.2f ms a frame (%u drawings, rows split at %d), VDP2 threads "
               "%.2f + %.2f ms a frame (%u pictures, bands split at line %d, they waited %.2f ms a frame for the VDP1's "
               "drawing); shown: %u a frame later, %u switches (the frame's own late %u times); the emulation thread "
               "waited %.2f ms a frame for the VDP1's drawing, %.2f for its frame buffer (%u times), %.2f for the "
               "VDP2, copied %.2f",
               c1 / 1e6 / n, c4 / 1e6 / n, j1, emu_v1_split, c2 / 1e6 / n, c3 / 1e6 / n, j2, emu_split, v21 / 1e6 / n,
               emu_frames_later, emu_switches, emu_late, emu_wait1_ns / 1e6 / n, emu_wait_cpu_ns / 1e6 / n,
               emu_waits_cpu, emu_wait2_ns / 1e6 / n, emu_copy_ns / 1e6 / n);
   else
      snprintf(text, sizeof(text), "renderer: 600 frames, drawn on the emulation thread (EMUSUNI_SATURN_THREADS=0)");
   emusuni_saturn_stats(text);
   {
      /* (sh2int.c: the SH-2s' waits counted, not run) */
      extern unsigned long long emusuni_sh2_skipped[2];
      extern int emusuni_sh2_skip;
      static unsigned long long before[2];
      const unsigned long long m = emusuni_sh2_skipped[0] - before[0], s = emusuni_sh2_skipped[1] - before[1];
      before[0] = emusuni_sh2_skipped[0];
      before[1] = emusuni_sh2_skipped[1];
      snprintf(text, sizeof(text),
               "SH-2 waits: %.0fk cycles a frame of the master's and %.0fk of the slave's counted, not run (of about "
               "450-480k each)%s", m / n / 1000.0, s / n / 1000.0, emusuni_sh2_skip ? "" : " - off (EMUSUNI_SATURN_SH2_SKIP=0)");
      emusuni_saturn_stats(text);
   }
   emu_frames = 0;
   emu_late = 0;
   emu_draws1 = 0;
   emu_frames_later = emu_switches = 0;
   emu_wait1_ns = emu_wait2_ns = emu_copy_ns = emu_wait_cpu_ns = 0;
   emu_waits_cpu = 0;
}

void VIDSoftVdp2DrawEnd(void)
{
   if (!emu_threads)
   {
      emu_use_live_vdp2();
      emu_vdp2_end_sync(); /* upstream's: titan's composition */
      emu_to_rgb565(dispbuffer, emu_out[0], vdp2width, vdp2height);
      emusuni_soft_frame = emu_out[0];
      emusuni_soft_w = vdp2width;
      emusuni_soft_h = vdp2height;
      emu_frame_stats();
      YuiSwapBuffers();
      return;
   }
   /* The picture shown (00.69): the frame's own when its drawing is done ("now"), else the one before it; and once a
      frame's own was late, the one before every frame ("a frame later") until its own would have been done for 10 s
      in a row (20 s, 40 s, at most a minute after each later switch). So a picture is shown twice, or one jumped over,
      only at a switch. 00.68 showed whichever was done: in Sonic R's race 558 of 600 pictures were a frame late and
      42 not, and each change between the two showed a picture twice and never the next one - its stutter. */
   {
      int k, late, cur, prev, kicked_now = 0;
      if (!emu_frame_kicked)
      {
         emu_vdp2_kick(0); /* the display off this frame: the back screen, no layers - waited for, it's short */
         kicked_now = 1;
      }
      if (emu_present_wait || kicked_now)
      {
         const long long t = emu_worker_wait(&emu_w2);
         emu_wait2_ns += t;
         emusuni_soft_stats_ns[3] += t;
      }
      pthread_mutex_lock(&emu_w2.mutex);
      k = emu_w2.done_tag;
      late = emu_w2.pending || emu_w2.busy;
      pthread_mutex_unlock(&emu_w2.mutex);
      cur = (emu_out_next + 2) % 3;  /* the picture of this frame (the last one started) */
      prev = (emu_out_next + 1) % 3; /* the one before it (done: this one's start waited for it) */
      if (late)
      {
         emu_late++;
         emusuni_soft_stats_ns[5]++;
      }
      if (emu_present_wait)
         k = cur;
      else if (!emu_later)
      {
         if (late)
         {
            emu_later = 1;
            emu_ready_run = 0;
            emu_switches++;
            k = prev;
         }
      }
      else
      {
         emu_ready_run = late ? 0 : emu_ready_run + 1;
         if (emu_ready_run >= emu_later_need)
         {
            emu_later = 0;
            emu_switches++;
            emu_later_need = emu_later_need * 2 > 3600 ? 3600 : emu_later_need * 2;
            k = cur;
         }
         else
            k = prev;
      }
      if (emu_later)
         emu_frames_later++;
      if (k >= 0 && emu_pictures > (k == cur ? 0 : 1))
      {
         emusuni_soft_frame = emu_out[k];
         emusuni_soft_w = emu_out_w[k];
         emusuni_soft_h = emu_out_h[k];
      }
   }
   emu_frame_stats();
   YuiSwapBuffers();
}

/* No drawing left on the threads (the VDP2's picture first: it may wait for the VDP1's drawing). */
void emusuni_soft_quiesce(void)
{
   if (!emu_threads)
      return;
   emu_worker_wait(&emu_w2);
   emu_worker_wait(&emu_w1);
}

/* ---- Made with the renderer (VIDSoftInit), gone with it (VIDSoftDeInit: the game's end) ---- */

static int emu_soft_init(void)
{
   const char *threads = getenv("EMUSUNI_SATURN_THREADS"), *wait = getenv("EMUSUNI_SATURN_WAIT");
   int k;
   emu_threads = !(threads && threads[0] == '0');
   emu_present_wait = wait && wait[0] == '1';
   emu_use_live_vdp2();
   if ((sprite_window_mask = (u8 *)calloc(704 * 512, 1)) == NULL)
      return -1;
   if ((emu_mosaic_table = (int (*)[1024])malloc(sizeof(int) * 16 * 1024)) == NULL)
      return -1;
   for (k = 0; k < 16 * 1024; k++)
      emu_mosaic_table[k / 1024][k % 1024] = (k % 1024) / (k / 1024 + 1) * (k / 1024 + 1);
   emu_mosaic_ready = 1;
   emu_split = -1;
   emu_vdp1_kicked = emu_vdp1_done = 0;
   emu_buf_seq[0] = emu_buf_seq[1] = 0;
   emu_vdp2_front_pending = NULL;
   emu_vdp2_wait1_ns = 0;
   for (k = 0; k < 3; k++)
   {
      if ((emu_out[k] = (u16 *)calloc(704 * 512, sizeof(u16))) == NULL)
         return -1;
      emu_out_w[k] = 320;
      emu_out_h[k] = 224;
   }
   emu_out_next = 0;
   emu_frame_kicked = 0;
   emu_later = emu_ready_run = 0;
   emu_later_need = 600;
   emu_pictures = emu_switches = emu_frames_later = 0;
   /* (The threads' copies start empty: every page is copied the first time.) */
   memset(emusuni_vdp1_dirty, 1, sizeof(emusuni_vdp1_dirty));
   memset(emusuni_vdp2_dirty, 1, sizeof(emusuni_vdp2_dirty));
   emu_pages_copied = 0;
   emusuni_soft_frame = NULL;
   emu_frames = emu_late = emu_draws1 = emu_waits_cpu = 0;
   emu_wait1_ns = emu_wait2_ns = emu_copy_ns = emu_wait_cpu_ns = 0;
   if (!emu_threads)
      return 0;
   if ((emu_j1 = (EmuVdp1Job *)calloc(1, sizeof(EmuVdp1Job))) == NULL ||
       (emu_j2 = (EmuVdp2Job *)calloc(1, sizeof(EmuVdp2Job))) == NULL ||
       (emu_v1[0] = (EmuV1 *)calloc(1, sizeof(EmuV1))) == NULL || (emu_v1[1] = (EmuV1 *)calloc(1, sizeof(EmuV1))) == NULL)
      return -1;
   emu_v1_split = -1;
   /* (The VDP1's helper first - "vdp1b": without it the VDP1 draws all its rows alone, as with
      EMUSUNI_SATURN_VDP1_BANDS=1, the PC's comparison.) */
   if (!(getenv("EMUSUNI_SATURN_VDP1_BANDS") && getenv("EMUSUNI_SATURN_VDP1_BANDS")[0] == '1'))
      emu_worker_start(&emu_w4, YAB_THREAD_VIDSOFT_LAYER_NBG2, emu_vdp1_help);
   if (emu_worker_start(&emu_w1, YAB_THREAD_VIDSOFT_VDP1, emu_vdp1_run) != 0 ||
       emu_worker_start(&emu_w3, YAB_THREAD_VIDSOFT_LAYER_NBG1, emu_vdp2_help) != 0 ||
       emu_worker_start(&emu_w2, YAB_THREAD_VIDSOFT_LAYER_NBG0, emu_vdp2_run) != 0)
      return -1;
   return 0;
}

static void emu_soft_deinit(void)
{
   int k;
   /* (The VDP2's lead before its helper, and both before the VDP1's: a picture waits for the VDP1's drawing.) */
   emu_worker_stop(&emu_w2);
   emu_worker_stop(&emu_w3);
   emu_worker_stop(&emu_w1);
   emu_worker_stop(&emu_w4); /* (after the VDP1's lead: its drawing waits for its helper's band) */
   free(emu_j1);
   free(emu_j2);
   emu_j1 = NULL;
   emu_j2 = NULL;
   free(emu_v1[0]);
   free(emu_v1[1]);
   emu_v1[0] = emu_v1[1] = NULL;
   for (k = 0; k < 3; k++)
   {
      free(emu_out[k]);
      emu_out[k] = NULL;
   }
   free(sprite_window_mask);
   sprite_window_mask = NULL;
   free(emu_mosaic_table);
   emu_mosaic_table = NULL;
   emu_mosaic_ready = 0;
   emusuni_soft_frame = NULL;
}
THREADS
write_if_changed "$OUT/include/emusuni_soft_threads.h"

# The core's CMake: libretro/Makefile.common's sources for its unix build (HAVE_MUSASHI, USE_RTHREADS, the
# GLSYM_PRIVATE build's libretro-common files), without the renderers and their GL loader.
mkdir -p "$OUT/cmake"
cat > "$OUT/cmake/CMakeLists.txt.new" <<'CMAKE'
# Generated by scripts/build-saturn-core.sh: YabaSanshiro's libretro core, without a renderer, as a static library.
cmake_minimum_required(VERSION 3.20)
project(yabasanshiro C CXX)
set(S ${YABASANSHIRO_DIR}/yabause/src)
set(L ${S}/libretro/libretro-common)
set(CHDR ${S}/libchdr)
set(FLAC ${CHDR}/deps/flac-1.3.3)
set(LZMA ${CHDR}/deps/lzma-19.00)
set(ZLIB ${CHDR}/deps/zlib-1.2.11)
set(SOURCES
  ${S}/osdcore.c ${S}/bios.c ${S}/cd-libretro.c ${S}/cheat.c ${S}/coffelf.c ${S}/cs0.c ${S}/cs1.c ${S}/cs2.c
  ${S}/debug.c ${S}/error.c ${S}/gameinfo.c ${S}/japmodem.c ${S}/junzip.c ${S}/memory.c ${S}/movie.c
  ${S}/netlink.c ${S}/peripheral.c ${S}/profile.c ${S}/scspdsp.c ${S}/scu.c ${S}/scsp.c ${S}/sh2cache.c
  ${S}/sh2core.c ${S}/sh2d.c ${S}/sh2iasm.c ${S}/sh2int.c ${S}/smpc.c ${S}/m68kcore.c ${S}/m68kd.c
  ${S}/snddummy.c ${S}/vidshared.c ${S}/titan/titan.c ${S}/yabause.c
  ${L}/compat/compat_posix_string.c ${L}/compat/compat_strl.c ${L}/compat/fopen_utf8.c
  ${L}/encodings/encoding_utf.c ${L}/file/file_path.c ${L}/compat/compat_strcasestr.c
  ${L}/vfs/vfs_implementation.c ${L}/streams/file_stream.c ${L}/streams/file_stream_transforms.c
  ${L}/rthreads/rthreads.c ${S}/thr-rthreads.c
  ${CHDR}/src/libchdr_chd.c ${CHDR}/src/libchdr_bitstream.c ${CHDR}/src/libchdr_huffman.c
  ${CHDR}/src/libchdr_flac.c ${CHDR}/src/libchdr_cdrom.c
  ${FLAC}/src/stream_decoder.c ${FLAC}/src/format.c ${FLAC}/src/bitreader.c ${FLAC}/src/crc.c
  ${FLAC}/src/memory.c ${FLAC}/src/lpc.c ${FLAC}/src/cpu.c ${FLAC}/src/md5.c ${FLAC}/src/fixed.c
  ${LZMA}/src/LzmaEnc.c ${LZMA}/src/LzFind.c ${LZMA}/src/LzmaDec.c
  ${ZLIB}/adler32.c ${ZLIB}/crc32.c ${ZLIB}/inffast.c ${ZLIB}/inflate.c ${ZLIB}/inftrees.c ${ZLIB}/zutil.c
  ${S}/musashi/m68kcpu.c ${S}/musashi/m68kops.c ${S}/musashi/m68kopnz.c ${S}/musashi/m68kopac.c
  ${S}/musashi/m68kopdm.c ${S}/m68kmusashi.c
  ${S}/libretro/libretro.c
  ${S}/Counter.cpp ${S}/vdp1.cpp ${S}/vdp2.cpp
  ${EMUSUNI_INCLUDE}/emusuni_saturn_stubs.c)
add_library(yabasanshiro_libretro STATIC ${SOURCES})
target_include_directories(yabasanshiro_libretro PRIVATE ${EMUSUNI_INCLUDE} ${S}/libretro ${S} ${L}/include
  ${S}/musashi ${CHDR}/include ${LZMA}/include ${FLAC}/include ${FLAC}/src/include ${ZLIB})
# libretro/Makefile.common's and Makefile's defines (without HAVE_LIBGL, _OGL3_ and HAVE_GLSYM_PRIVATE: no renderer),
# and EmuSuNi's (the patches of scripts/build-saturn-core.sh).
target_compile_definitions(yabasanshiro_libretro PRIVATE
  __LIBRETRO__ VERSION="3.4.2" GIT_VERSION=" 09ed8e5" HAVE_LROUND _7ZIP_ST FLAC__HAS_OGG=0 FLAC__NO_DLL NO_CLI
  HAVE_BUILTIN_BSWAP16=1 HAVE_BUILTIN_BSWAP32=1 HAVE_C99_VARIADIC_MACROS=1 HAVE_FLOORF=1 HAVE_GETTIMEOFDAY=1
  HAVE_STDINT_H=1 HAVE_SYS_TIME_H=1 IMPROVED_SAVESTATES PACKAGE_VERSION="1.3.3" HAVE_SYS_PARAM_H HAVE_MUSASHI=1
  EMUSUNI_PORT EMUSUNI_NO_GL)
target_compile_options(yabasanshiro_libretro PRIVATE -w
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11> $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++11 -fno-exceptions -fno-rtti>)
# The software renderer (vidsoft: VDP1 and VDP2 drawn by the CPU, titan composes them), and its timers (PC only).
if(EMUSUNI_SOFT)
  target_sources(yabasanshiro_libretro PRIVATE ${S}/vidsoft.c)
  # (00.69) The renderer's floating point as written: its gouraud and texture sums (doubles, added a pixel at a time)
  # aren't reordered by -ffast-math, so its pictures don't depend on how the compiler lays the code out (two builds of
  # the same drawing with -ffast-math differed by a colour step in 51 of 1,500 Sonic R pictures).
  set_source_files_properties(${S}/vidsoft.c ${S}/titan/titan.c PROPERTIES COMPILE_OPTIONS -fno-fast-math)
  target_compile_definitions(yabasanshiro_libretro PRIVATE EMUSUNI_SOFT)
endif()
if(EMUSUNI_SOFT_PROF)
  target_sources(yabasanshiro_libretro PRIVATE ${EMUSUNI_INCLUDE}/emusuni_soft_prof.c)
  target_compile_definitions(yabasanshiro_libretro PRIVATE EMUSUNI_SOFT_PROF)
endif()
CMAKE
write_if_changed "$OUT/cmake/CMakeLists.txt"

# The Release flags (CMAKE_<LANG>_FLAGS would replace the toolchain's target flags): -O3 -ffast-math (its
# Makefile's FASTMATH), the PS4's CPU, a section for each function and variable (the app's link drops the unused
# ones), no unwind tables (nothing unwinds through it), the forced header.
FLAGS_CORE="-O3 -DNDEBUG -ffast-math -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/include/emusuni_saturn.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a -fPIC"
# PGO (profile-guided optimisation, 00.69): scripts/saturn-core.profdata.xz, a profile of the PC build running the
# owner's Daytona USA and Sonic R from the BIOS through their demo races (the SH-2s, the 68000 and the sound, the
# renderer's threads), used by both builds when it's there (SATURN_CORE_PGO=off: without it). A profile older than
# the sources only optimises less (the functions that changed get none; the build's line counts them). A new one:
# SATURN_CORE_HOST=1 SATURN_CORE_PGO=generate build-saturn-core.sh <dir> (instrumented), a PC frontend linked with
# -fprofile-instr-generate and run on the games with LLVM_PROFILE_FILE=<dir>/%p.profraw, then
# llvm-profdata-21 merge -o saturn-core.profdata <dir>/*.profraw; xz -9 it into scripts/.
PROFILE="${SATURN_CORE_PROFILE:-$HERE/saturn-core.profdata.xz}"
PGO="none"
case "${SATURN_CORE_PGO:-use}" in
  generate)
    if [ "$HOST" != 1 ]; then
      echo "YabaSanshiro: SATURN_CORE_PGO=generate is for the PC build (SATURN_CORE_HOST=1)"
      exit 1
    fi
    FLAGS_CORE+=" -fprofile-instr-generate"
    PGO="generate" ;;
  off) ;;
  *)
    if [ -f "$PROFILE" ]; then
      mkdir -p "$OUT"
      xz -dc "$PROFILE" > "$OUT/saturn-core.profdata.new"
      if cmp -s "$OUT/saturn-core.profdata.new" "$OUT/saturn-core.profdata"; then
        rm -f "$OUT/saturn-core.profdata.new"
      else
        mv "$OUT/saturn-core.profdata.new" "$OUT/saturn-core.profdata"
      fi
      FLAGS_CORE+=" -fprofile-instr-use=$OUT/saturn-core.profdata -Wno-profile-instr-unprofiled"
      PGO="profile $(sha256sum "$PROFILE" | cut -c1-16)"
    fi ;;
esac
RENDERER="${SATURN_RENDERER:-soft}"
PROF="${SATURN_PROF:-0}"
OPTIONS=(-DYABASANSHIRO_DIR="$SRC" -DEMUSUNI_INCLUDE="$OUT/include" "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE"
         "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CORE" -DEMUSUNI_SOFT=$([ "$RENDERER" = soft ] && echo ON || echo OFF)
         -DEMUSUNI_SOFT_PROF=$([ "$PROF" = 1 ] && echo ON || echo OFF))
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
# (SATURN_PATCH_ONLY=1: the patches and the generated files only, for a quick compile check of single files.)
[ "${SATURN_PATCH_ONLY:-0}" = 1 ] && { echo "YabaSanshiro: patched only"; exit 0; }
STAMP="$OUT/.emusuni-flags"
WANT="$SRC | $(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown) | host $HOST | pgo $PGO | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "YabaSanshiro: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$OUT/cmake" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "YabaSanshiro: configuring failed (see $LOG)"
    exit 1
  fi
fi
# (At most 4 compilers at once: the PC's load, scripts/build.sh.)
if ! ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}" yabasanshiro_libretro >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "YabaSanshiro: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"
if [ "${PGO%% *}" = profile ]; then
  # (-w keeps clang's warnings out, its profile mismatch ones too: a function changed since the profile is compiled
  # as without one.)
  echo "YabaSanshiro: PGO with $(basename "$PROFILE") (${PGO#profile })"
fi

# One object: only the libretro API (renamed) stays global. Replaced only when it changed (the app isn't linked
# again for nothing).
ld.lld-21 -r --whole-archive "$BUILD/libyabasanshiro_libretro.a" -o "$OUT/saturn_core_all.o"
KEEP=(--keep-global-symbol='yabasanshiro_retro_*')
if [ "$PROF" = 1 ]; then
  KEEP+=(--keep-global-symbol=emusuni_soft_prof_ns --keep-global-symbol=emusuni_soft_stats_ns)
fi
llvm-objcopy-21 --wildcard "${KEEP[@]}" "$OUT/saturn_core_all.o" "$OUT/saturn_core.o.new"
rm -f "$OUT/saturn_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/saturn_core.o.new" | awk '{print $3}' | sort)
# (An if, not an &&: a false test there would fail the pipeline, and with set -e the script, without a word.)
WANT_GLOBALS=$( { for f in $API; do echo "yabasanshiro_$f"; done
                 if [ "$PROF" = 1 ]; then echo emusuni_soft_prof_ns; echo emusuni_soft_stats_ns; fi; } | sort)
if [ "$PGO" = generate ]; then
  # (The instrumented PC build for a new profile: its counters and the profile runtime's hooks are no app's object.)
  echo "YabaSanshiro: instrumented for PGO (SATURN_CORE_PGO=generate): the object's checks left out"
  mv "$OUT/saturn_core.o.new" "$OUT/saturn_core.o"
  exit 0
fi
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "YabaSanshiro: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/saturn_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved: the app would export them as imports nobody gives.
WEAK=$(llvm-nm-21 -u "$OUT/saturn_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "YabaSanshiro: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if llvm-nm-21 --defined-only "$OUT/saturn_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "YabaSanshiro: the object defines a C++ operator new/delete of its own"
  exit 1
fi
# No static array of 64 KiB or more: the PS4 charges the app's whole static memory from its start, used or not
# (the core's big tables are the heap's while it runs).
BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/saturn_core.o.new" |
      awk '($3 == "b" || $3 == "B" || $3 == "d" || $3 == "D") && $2 + 0 >= 65536 {print $4 " (" $2 " bytes)"}')
if [ -n "$BIG" ]; then
  echo "YabaSanshiro: the object has static arrays of 64 KiB or more:"
  echo "$BIG"
  [ "$HOST" = 1 ] && [ "${SATURN_ALLOW_BIG:-0}" = 1 ] || exit 1
fi
if [ "$HOST" != 1 ]; then
  # What the app gives it: operator new/delete (port/ps4_new.cpp, compiled here as the app compiles it, only to
  # list its symbols) and the speed test's hooks (src/SaturnBench.cpp).
  PS4C="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay"
  PS4C+=" -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -march=btver2"
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $PS4C -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" | awk 'NF >= 3 {print $3}';
                 echo __dso_handle; echo emusuni_saturn_thread; echo emusuni_saturn_scsp_idle;
                 echo emusuni_saturn_scsp_frame; echo emusuni_saturn_stats; } | sort -u)
  rm -f "$OUT/check_ps4_new.o"
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "YabaSanshiro: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
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
    echo "YabaSanshiro: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "YabaSanshiro: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app"
fi
# What it adds to the program: code, data and static memory (the .bss is charged at load as well).
SIZES=$(llvm-size-21 "$OUT/saturn_core.o.new" | tail -1)
echo "YabaSanshiro: text $(echo "$SIZES" | awk '{print $1}'), data $(echo "$SIZES" | awk '{print $2}')," \
     "bss $(echo "$SIZES" | awk '{print $3}') bytes (before the app's link drops what nothing calls)"
if cmp -s "$OUT/saturn_core.o.new" "$OUT/saturn_core.o"; then
  rm -f "$OUT/saturn_core.o.new"
else
  mv "$OUT/saturn_core.o.new" "$OUT/saturn_core.o"
fi
ls -la "$OUT/saturn_core.o"
