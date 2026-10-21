#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds YabaSanshiro 3.4.2 (libretro/yabause, branch yabasanshiro: ~/src/yabasanshiro, GPL-2.0-or-later) for the
# PS4 into one object that scripts/build.sh links into EmuSuNi: the Sega Saturn, in 00.64 only for its speed test
# (src/SaturnTest.cpp: with the file /data/EmuSuNi/saturn-test, Settings has "Saturn (speed test)", which runs the
# owner's games with no picture and logs what the PS4's CPU takes for each frame). Its libretro build's sources
# (libretro/Makefile.common) in a CMake of its own made here, compiled with the PS4 toolchain
# (toolchain/ps4-love-modern.cmake) and the defines of its unix build: the SH-2 interpreter (no SH-2 recompiler
# exists for x86-64), Musashi for the 68000, the new SCSP on its own thread, libchdr, libretro-common's own file
# access (the app answers no VFS interface), -O3 -ffast-math as upstream. Not as its libretro build:
#   - No renderer (EMUSUNI_NO_GL): the core's dummy video interface (VIDDummy), without the OpenGL renderer (vidogl,
#     ygl*, glsm: the PS4 has no OpenGL without Mesa's Zink, which isn't built) and without the software one
#     (vidsoft: 1.9 MiB of static memory, seven threads). The console starts in retro_load_game (the GL build starts
#     it once its GL context exists). What the renderers do for the rest of the core, VIDDummy does too: it walks
#     the VDP1's command list as they start drawing (Vdp1Draw leaves that to them: without it the VDP1 never ends
#     its frame, and the games wait at their title screens), and its end of frame (Vdp2DrawEnd) calls YuiSwapBuffers
#     as the GL renderer's YglRender does, which gives the sound thread the frame's room for sound. Its VDP1 frame
#     buffer reads and writes go to the VDP1's frame buffer memory (the dummy's own functions left the value read
#     undefined). The speed test's numbers are therefore the console's emulation without the renderer's own drawing
#     work on the CPU (its log says how much that adds, as measured on the PC).
#   - The VDP1's registers zeroed when they are made (calloc): upstream's Vdp1Reset clears only sizeof(Vdp1Regs) (a
#     pointer: 8 bytes), so EDSR, LOPR, COPR... began as whatever the heap held, which the games read at their start
#     (Daytona USA then never drew its 3D here; the GL build's pictures of the same frames showed its demo race).
#   - Frame skip off by default: on (the core's default), the emulation thread busy-waits to 60 frames a second
#     (16.6 ms of CPU a frame on the PC, whatever the game).
#   - The fast paths the PC measurement found (identical pictures and cycle counts; Daytona USA -17%, Sonic R -9% on
#     the PC): the SH-2s' accesses to their high and low work RAM go straight to it in the six memory accessors, the
#     same cycles counted, and the interpreter fetches its instructions from the high work RAM without the fetch
#     table's call.
#   - The SCSP thread (the 68000 and the sound chip, beside the SH-2s) waits for the SH-2s' cycles by calling the app
#     (emusuni_saturn_scsp_idle: a sleep of some microseconds) where it spun on them, and tells the app at the end of
#     each frame (emusuni_saturn_scsp_frame: its CPU time). Spinning, it took a core whole (66-89% of its time was
#     the spin on the PC).
#   - Its threads (only the SCSP's without a renderer) start through the app (emusuni_saturn_thread: their core and
#     priority on the PS4), not Linux's affinity system calls.
#   - The static tables of 64 KiB or more on the heap while the core runs (the PS4 charges the program's whole
#     static memory to the games' flexible memory from the app's start): the SH-2's 64K instruction handlers
#     (512 KiB), Musashi's 64K handlers and cycle counts (704 KiB), the CD audio buffer (345 KiB), the VDP2's
#     line registers and cell scroll tables (170 KiB), the zip reader's buffer (64 KiB).
#   - The backup RAM file read into the heap and written back when the game ends (the PS4 maps no files).
#   - The BIOS the app found, by its path (EMUSUNI_SATURN_BIOS: the owner's file keeps its own name).
# Its libretro API becomes yabasanshiro_retro_* and everything else in it private (CMakeLists.txt's FCEUmm way).
# The object is checked at the end: only that API defined globally, no weak references, no operator new/delete of
# its own, no static array of 64 KiB or more, every symbol it needs in the PS4 libraries or the app.
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-saturn-core.sh <out dir> [clean]
#   SATURN_CORE_HOST=1 build-saturn-core.sh <out dir>   the same object for this PC (Linux, clang 21), for the PC
#                                                       check of the speed test: no PS4 checks
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
      '#else\n'
      '#include "vidogl.h"\n#include "vidsoft.h"\n#include "ygl.h"\n'
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
      '#ifdef EMUSUNI_NO_GL\n'
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
      '#ifdef EMUSUNI_NO_GL\n'
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
      '#ifdef EMUSUNI_NO_GL\n'
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
      '\temusuni_saturn_thread(start.id == YAB_THREAD_SCSP ? "scsp" : "other");\n'
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

# --- memory.c: the work RAMs' fast paths (the same data and cycles as the table's handlers); the backup RAM file ---
READ = ('  if ((addr & 0xDFF00000) == 0x06000000) { if (cycle != NULL) *cycle = 0; return T2Read%s(HighWram, addr & 0xFFFFF); }\n'
        '  if ((addr & 0xDFF00000) == 0x00200000) { if (cycle != NULL) *cycle = 12; return T2Read%s(LowWram, addr & 0xFFFFF); }\n')
WRITE = ('  if ((addr & 0xDFF00000) == 0x06000000) { if (cycle != NULL) *cycle = 2; T2Write%s(HighWram, addr & 0xFFFFF, val); return; }\n'
         '  if ((addr & 0xDFF00000) == 0x00200000) { if (cycle != NULL) *cycle = 7; T2Write%s(LowWram, addr & 0xFFFFF, val); return; }\n')
NOTE = ('  /* EmuSuNi: the high and low work RAM straight away (their handlers\' reads and writes, and the cycles\n'
        '     GET_MEM_CYCLE_R / GET_MEM_CYCLE_W give them) */\n')
for size, kind in (('Byte', 'u8'), ('Word', 'u16'), ('Long', 'u32')):
    sig = '%s FASTCALL MappedMemoryRead%s(u32 addr, u32 * cycle)\n#endif\n{\n' % (kind, size)
    patch('memory.c', sig, sig + NOTE + READ % (size, size))
for size, sig in (('Byte', 'void FASTCALL MappedMemoryWriteByte(u32 addr, u8 val, u32 * cycle)\n#endif\n{\n'),
                  ('Word', 'void FASTCALL MappedMemoryWriteWord(u32 addr, u16 val, u32 * cycle )\n#endif\n{\n'),
                  ('Long', 'void FASTCALL MappedMemoryWriteLong(u32 addr, u32 val, u32 * cycle )\n#endif\n{\n')):
    patch('memory.c', sig, sig + NOTE + WRITE % (size, size))
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
int vdp2_interlace = 0;
void YglOnUpdateColorRamWord(u32 addr) { (void)addr; }
void VIDSoftSetVdp1ThreadEnable(int b) { (void)b; }
void VIDSoftSetNumLayerThreads(int num) { (void)num; }
void Vdp2GetInterlaceInfo(int * start_line, int * line_increment) { *start_line = 0; *line_increment = 1; }
STUBS
write_if_changed "$OUT/include/emusuni_saturn_stubs.c"

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
CMAKE
write_if_changed "$OUT/cmake/CMakeLists.txt"

# The Release flags (CMAKE_<LANG>_FLAGS would replace the toolchain's target flags): -O3 -ffast-math (its
# Makefile's FASTMATH), the PS4's CPU, a section for each function and variable (the app's link drops the unused
# ones), no unwind tables (nothing unwinds through it), the forced header.
FLAGS_CORE="-O3 -DNDEBUG -ffast-math -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/include/emusuni_saturn.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a -fPIC"
OPTIONS=(-DYABASANSHIRO_DIR="$SRC" -DEMUSUNI_INCLUDE="$OUT/include" "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE"
         "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CORE")
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

# One object: only the libretro API (renamed) stays global. Replaced only when it changed (the app isn't linked
# again for nothing).
ld.lld-21 -r --whole-archive "$BUILD/libyabasanshiro_libretro.a" -o "$OUT/saturn_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='yabasanshiro_retro_*' "$OUT/saturn_core_all.o" "$OUT/saturn_core.o.new"
rm -f "$OUT/saturn_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/saturn_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "yabasanshiro_$f"; done | sort)
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
  exit 1
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
                 echo emusuni_saturn_scsp_frame; } | sort -u)
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
