// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// What the N64 core (Mupen64Plus-Next) needs from the PS4 side:
//   - Executable memory for its dynarec's code cache. The cache is an array in the core's own data
//     (its code jumps to the core's functions and data with 32-bit offsets, so it must stay within
//     2 GiB of them): emusuni_make_executable() turns those pages into code the ways a jailbroken
//     PS4 may allow, each tried once and checked by running a small function there.
//     emusuni_jit_check() tries the same on a page of EmuSuNi's own, before a game starts: the
//     emulator then chooses the dynarec or the cached interpreter (no JIT).
//   - OpenGL: GLideN64 (the core's other renderer) is built in but never used here (EmuSuNi draws
//     N64 games with ParaLLEl-RDP on Vulkan); its GL calls go nowhere.

#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

int32_t sceKernelMprotect(const void* addr, size_t len, int32_t prot);
int32_t sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags,
                                              const char* name);
void* __real_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);

#define PROT_RWX (PROT_READ | PROT_WRITE | PROT_EXEC)
#define PAGE 16384u
#define MAP_FIXED_FLAG 0x10

static const char* const METHODS[] = {"mprotect", "fixed mmap", "fixed flexible memory",
                                      "fixed system flexible memory"};

// Into EmuSuNi's log (src/Log.cpp: one line at a time, whatever thread writes it).
void emusuni_log_line(const char* text);

static void Trace(const char* format, const char* a, int b)
{
  char line[256];
  snprintf(line, sizeof(line), format, a, b);
  emusuni_log_line(line);
}

static sigjmp_buf s_jump;
static void Fault(int signal)
{
  (void)signal;
  siglongjmp(s_jump, 1);
}

// Runs `mov eax, 0x4E36; ret` at code: 1 when it came back with that value.
static int Runs(unsigned char* code)
{
  static const unsigned char TEST[] = {0xB8, 0x36, 0x4E, 0x00, 0x00, 0xC3};
  struct sigaction fault, old_segv, old_bus;
  memset(&fault, 0, sizeof(fault));
  fault.sa_handler = Fault;
  sigemptyset(&fault.sa_mask);
  sigaction(SIGSEGV, &fault, &old_segv);
  sigaction(SIGBUS, &fault, &old_bus);
  volatile int result = 0;
  if (sigsetjmp(s_jump, 1) == 0)
  {
    memcpy(code, TEST, sizeof(TEST));
    result = ((int (*)(void))code)();
  }
  sigaction(SIGSEGV, &old_segv, NULL);
  sigaction(SIGBUS, &old_bus, NULL);
  memset(code, 0, sizeof(TEST));
  return result == 0x4E36;
}

static int Map(int method, void* address, size_t size)
{
  void* at = address;
  switch (method)
  {
  case 0:
    return sceKernelMprotect(address, size, PROT_RWX) == 0;
  case 1:
    return __real_mmap(address, size, PROT_RWX, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) == address;
  case 2:
    return sceKernelMapNamedFlexibleMemory(&at, size, PROT_RWX, MAP_FIXED_FLAG, "n64 dynarec") == 0 &&
           at == address;
  case 3:
    return sceKernelMapNamedSystemFlexibleMemory(&at, size, PROT_RWX, MAP_FIXED_FLAG, "n64 dynarec") == 0 &&
           at == address;
  }
  return 0;
}

// The pages around [address, address + size): the way that worked (0-3), or -1.
int emusuni_make_executable(void* address, size_t size)
{
  const uintptr_t start = (uintptr_t)address & ~(uintptr_t)(PAGE - 1);
  const uintptr_t end = ((uintptr_t)address + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
  for (int method = 0; method < 4; method++)
  {
    if (!Map(method, (void*)start, end - start))
    {
      Trace("n64: dynarec memory: %s refused (%d)", METHODS[method], method);
      continue;
    }
    if (Runs((unsigned char*)start))
    {
      Trace("n64: dynarec memory: %s works (method %d)", METHODS[method], method);
      return method;
    }
    Trace("n64: dynarec memory: %s maps, but code there faults (%d)", METHODS[method], method);
  }
  return -1;
}

// A page like the core's code cache (in the executable's data), tried once.
static unsigned char s_probe[2 * PAGE] __attribute__((aligned(16384)));
int emusuni_jit_check(void)
{
  static int result = -2;
  if (result == -2)
    result = emusuni_make_executable(s_probe, PAGE);
  return result;
}

// ---- OpenGL, for GLideN64's code (never called) ----
#define GL_STUB(name) \
  void name(void) {}
GL_STUB(glBindTexture)
GL_STUB(glBlendFunc)
GL_STUB(glClear)
GL_STUB(glClearColor)
GL_STUB(glClearDepth)
GL_STUB(glColorMask)
GL_STUB(glCullFace)
GL_STUB(glDeleteTextures)
GL_STUB(glDepthFunc)
GL_STUB(glDepthMask)
GL_STUB(glDepthRange)
GL_STUB(glDisable)
GL_STUB(glDrawArrays)
GL_STUB(glDrawElements)
GL_STUB(glEnable)
GL_STUB(glFinish)
GL_STUB(glFrontFace)
GL_STUB(glGenTextures)
GL_STUB(glGetFloatv)
GL_STUB(glGetIntegerv)
GL_STUB(glLineWidth)
GL_STUB(glPixelStorei)
GL_STUB(glPolygonMode)
GL_STUB(glPolygonOffset)
GL_STUB(glReadBuffer)
GL_STUB(glReadPixels)
GL_STUB(glScissor)
GL_STUB(glStencilFunc)
GL_STUB(glStencilMask)
GL_STUB(glStencilOp)
GL_STUB(glTexParameteri)
GL_STUB(glTexSubImage2D)
GL_STUB(glViewport)
unsigned glGetError(void)
{
  return 0;
}
const unsigned char* glGetString(unsigned name)
{
  (void)name;
  return (const unsigned char*)"";
}

/* ---- Where ParaLLEl-RDP's threads run (scripts/build-n64-core.sh calls this first thing in each):
 * cores of their own, next to the emulation thread's core 2 (src/Emulator.cpp). 1 = the command
 * thread, which the emulation waits for at the end of every frame: as high as the emulation (480);
 * 2 = the fence worker (the GPU's results back to the N64's memory): just below; 3 = the pipeline
 * compiler (shaders ahead of their use) and 4 = the pipeline cache's file writer: low. With N64 CPU
 * Cores on PS4's Choice (emusuni_n64_fixed_cores 0, set by the emulator before the game starts)
 * only the priorities. ---- */
int emusuni_n64_fixed_cores = 1;
/* libkernel's calls as they really are (OpenOrbis declares some without parameters). */
extern void* emusuni_pthread_self(void) __asm__("scePthreadSelf");
extern int emusuni_set_affinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
extern int emusuni_set_priority(void* thread, int priority) __asm__("scePthreadSetprio");
extern int emusuni_rename_thread(void* thread, const char* name) __asm__("scePthreadRename");
int sceKernelGetCurrentCpu(void);

void emusuni_core_thread(int role)
{
  static const struct
  {
    const char* name;
    int core, priority;
  } ROLES[] = {{"rdp commands", 3, 480},
               {"rdp fences", 1, 495},
               {"rdp compiler", 5, 760},
               {"rdp cache writer", 5, 760}};
  if (role < 1 || role > 4)
    return;
  void* self = emusuni_pthread_self();
  const int core = ROLES[role - 1].core, priority = ROLES[role - 1].priority;
  const int affinity = emusuni_n64_fixed_cores ? emusuni_set_affinity(self, 1ull << core) : -1;
  const int prio = emusuni_set_priority(self, priority);
  emusuni_rename_thread(self, ROLES[role - 1].name);
  char line[160];
  if (emusuni_n64_fixed_cores)
    snprintf(line, sizeof(line), "n64: thread \"%s\": core %d (%#x), priority %d (%#x), running on CPU %d",
             ROLES[role - 1].name, core, (unsigned)affinity, priority, (unsigned)prio, sceKernelGetCurrentCpu());
  else
    snprintf(line, sizeof(line), "n64: thread \"%s\": the PS4's cores, priority %d (%#x), running on CPU %d",
             ROLES[role - 1].name, priority, (unsigned)prio, sceKernelGetCurrentCpu());
  emusuni_log_line(line);
}

/* melonDS DS's renderer threads (scripts/build-nds-core.sh) call this first: "nds 3d" and "nds 3d 2"
 * (the 3D rasterizer: even and odd lines), "nds 2d" (the sub screen's 2D, a line at a time while the
 * emulation thread on core 2 goes on). Each on a core of its own, none on core 2 (they and it wait for
 * each other awake), at its priority: the 2D helper beside it on core 3 (same CPU module: handed ~192
 * lines a frame), the 3D threads together on cores 4 and 5. */
void emusuni_nds_thread(const char* name)
{
  int core = 4;
  if (strcmp(name, "nds 3d 2") == 0)
    core = 5;
  else if (strcmp(name, "nds 2d") == 0)
    core = 3;
  void* self = emusuni_pthread_self();
  const int affinity = emusuni_set_affinity(self, 1ull << core);
  const int prio = emusuni_set_priority(self, 480);
  emusuni_rename_thread(self, name);
  char line[160];
  snprintf(line, sizeof(line), "nds: thread \"%s\": core %d (%#x), priority 480 (%#x), running on CPU %d", name, core,
           (unsigned)affinity, (unsigned)prio, sceKernelGetCurrentCpu());
  emusuni_log_line(line);
}

/* PPSSPP's threads (scripts/build-psp-core.sh) call this first with the name PPSSPP gives them
 * ("SAS", "PoolW 0".."PoolW 3", "PoolW IO 4".."PoolW IO 7", "ShaderCompile", "IO", "ExecLoader",
 * "FlushMemInfo"...); retro_init calls it with "Main" on the emulation thread, left on core 2
 * (src/Emulator.cpp pins it while a PSP game runs). None of the others on core 2: the sound mixing
 * (waited for every frame) on core 3 at the emulation thread's priority; the shader compiles, the
 * pipeline batching and the pool's workers at one priority on cores 1, 3, 4, 5 (the PS4 runs threads
 * by fixed priorities: God of War's 86 cached pipelines compiled at its start by threads of three
 * priorities froze the app on the heap's old spin lock, port/ps4_dlmalloc.c). */
void emusuni_psp_thread(const char* name)
{
  uint64_t mask = (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5);
  int priority = 700;
  if (strcmp(name, "Main") == 0)
    return;
  if (strcmp(name, "SAS") == 0)
  {
    mask = 1ull << 3;
    priority = 480;
  }
  else if (strcmp(name, "VulkanRenderMan") == 0)
  {
    mask = 1ull << 4;
    priority = 480;
  }
  else if (strcmp(name, "ShaderCompile") == 0)
    priority = 700; /* pipeline batching: with the compile workers (cores 1,3,4,5), not alone on core 5 */
  else if (strncmp(name, "PoolW IO", 8) == 0)
    priority = 760; /* blocking file reads */
  else if (strncmp(name, "PoolW", 5) == 0)
    priority = 700; /* decoding, texture work, pipeline compiles: as the shader compiles */
  else if (strcmp(name, "IO") == 0)
    priority = 500; /* the PSP's own asynchronous reads */
  void* self = emusuni_pthread_self();
  const int affinity = emusuni_set_affinity(self, mask);
  const int prio = emusuni_set_priority(self, priority);
  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "psp %s", name);
  emusuni_rename_thread(self, thread_name);
  char line[160];
  snprintf(line, sizeof(line), "psp: thread \"%s\": cores %#llx (%#x), priority %d (%#x), running on CPU %d", name,
           (unsigned long long)mask, (unsigned)affinity, priority, (unsigned)prio, sceKernelGetCurrentCpu());
  emusuni_log_line(line);
}

/* Flycast's threads (scripts/build-dc-core.sh) call this first with Flycast's name for them:
 * "Flycast-emu" (SH4, ARM7, AICA with threaded rendering; a new one at every start: game start,
 * reset, state loaded), "PicoTCP" (modem networking, only if a game dials), "CustomTexLoader" (off)...
 * retro_run's thread is the renderer (core 2, 480) and waits for Flycast-emu every frame on condition
 * variables (no spinning): Flycast-emu beside it on core 3 (same module, shared L2) at the same
 * priority; the rest below them on cores 1, 4, 5. */
void emusuni_dc_thread(const char* name)
{
  uint64_t mask = (1ull << 1) | (1ull << 4) | (1ull << 5);
  int priority = 700;
  if (strcmp(name, "Flycast-emu") == 0)
  {
    mask = 1ull << 3;
    priority = 480;
  }
  void* self = emusuni_pthread_self();
  const int affinity = emusuni_set_affinity(self, mask);
  const int prio = emusuni_set_priority(self, priority);
  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "dc %s", name);
  emusuni_rename_thread(self, thread_name);
  char line[160];
  snprintf(line, sizeof(line), "dc: thread \"%s\": cores %#llx (%#x), priority %d (%#x), running on CPU %d", name,
           (unsigned long long)mask, (unsigned)affinity, priority, (unsigned)prio, sceKernelGetCurrentCpu());
  emusuni_log_line(line);
}

/* ---- The core's timing (mupen64plus-core's main/profile.h; scripts/build-n64-core.sh builds the
 * core with -DPROFILE, whose profile.c the libretro build leaves out): the time in its graphics
 * and sound tasks per emulated frame, on stderr (the log) every 10 s. new_vi() calls
 * timed_sections_refresh() once per frame. ---- */
enum timed_section
{
  TIMED_SECTION_ALL,
  TIMED_SECTION_GFX,
  TIMED_SECTION_AUDIO,
  TIMED_SECTION_COMPILER,
  TIMED_SECTION_IDLE,
  NUM_TIMED_SECTIONS
};

static long long s_section_ns[NUM_TIMED_SECTIONS], s_section_start[NUM_TIMED_SECTIONS];
static long long s_window_start = 0;
static unsigned s_window_frames = 0;

static long long now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void timed_section_start(enum timed_section section)
{
  s_section_start[section] = now_ns();
}

void timed_section_end(enum timed_section section)
{
  s_section_ns[section] += now_ns() - s_section_start[section];
}

void timed_sections_refresh(void)
{
  const long long now = now_ns();
  if (s_window_start == 0 || now - s_window_start > 60000000000LL)
  {
    s_window_start = now;  /* the first frame, or a game started again after a long pause */
    s_window_frames = 0;
    memset(s_section_ns, 0, sizeof(s_section_ns));
    return;
  }
  s_window_frames++;
  if (now - s_window_start < 10000000000LL)
    return;
  const double frames = s_window_frames ? (double)s_window_frames : 1.0;
  char line[200];
  snprintf(line, sizeof(line),
           "n64 core per frame: graphics tasks (RSP) %.2f ms, sound tasks %.2f ms, recompiler %.2f ms; "
           "%.1f frames per second",
           s_section_ns[TIMED_SECTION_GFX] / 1e6 / frames, s_section_ns[TIMED_SECTION_AUDIO] / 1e6 / frames,
           s_section_ns[TIMED_SECTION_COMPILER] / 1e6 / frames, frames / ((now - s_window_start) / 1e9));
  emusuni_log_line(line);
  memset(s_section_ns, 0, sizeof(s_section_ns));
  s_window_start = now;
  s_window_frames = 0;
}
