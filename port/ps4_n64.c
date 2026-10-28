// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// What the N64 core (Mupen64Plus-Next) and the other recompiling cores need from the PS4 side:
//   - Executable memory for the code their recompilers write: one shared arena in EmuSuNi's data
//     (emusuni_jit_arena below: the N64's dynarec, the PS1's, PSP's, DS's and Dreamcast's JITs take
//     turns on it). Their code jumps to the cores' functions and data with 32-bit offsets, so it must
//     stay within 2 GiB of them. emusuni_make_executable() turns pages into code the ways a
//     jailbroken PS4 may allow, each tried once and checked by running a small function there.
//     emusuni_jit_check() makes the arena executable before an N64 game starts: the emulator then
//     chooses the dynarec or the cached interpreter (no JIT).
//   - OpenGL: GLideN64 (the core's other renderer) is built in but never used here (EmuSuNi draws
//     N64 games with ParaLLEl-RDP on Vulkan); its GL calls go nowhere.

#include <pthread.h>
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
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelVirtualQuery(const void* addr, int flags, void* info, size_t info_size);
void* __real_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
extern char __text_start[];  // OpenOrbis link.x: the start of the program's code

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

// The games' flexible memory free now, in KiB (thread stacks come out of it: 2 MiB + 16 KiB for each new
// thread, which libkernel keeps for the next thread of the same size when it ends).
static unsigned long FlexibleFreeKiB(void)
{
  size_t size = 0;
  return sceKernelAvailableFlexibleMemorySize(&size) == 0 ? (unsigned long)(size / 1024) : 0ul;
}

/* ---- The shared JIT arena (00.63) ----
 * The code the recompilers write (N64, PS1, PSP, DS, Dreamcast) goes into one 32 MiB array of EmuSuNi's data, not
 * an array in each core: those were 111 MiB of .bss together, and the PS4 charges the program's whole .bss to the
 * games' flexible memory when it loads it. Only one core runs at a time, and each one starts and stops within one
 * Emulator::Run (src/Emulator.cpp), which calls emusuni_jit_arena_reset() after every core's deinit. In the
 * executable's data the arena is within 2 GiB of all the cores' code and data, which their x86-64 code calls and
 * reads with 32-bit offsets.
 *
 * A core takes its part when it starts, every time: emusuni_jit_arena(size, name), with "n64", "ps1", "psp" or "nds",
 * or one name per cache ("dc sh4", "dc arm7", "dc dsp": the first word names the core). The same name asked for again
 * before the reset gets the same memory back (the N64's dynarec starts again when its game resets; the Dreamcast asks
 * for its DSP's at every reset of the sound chip); a new name gets the next free part. Each part is 16 KiB aligned,
 * read/write/execute, and holds only 0xF4 when it is handed out new. After the reset another core writes there:
 * nothing that points into the arena, or says what was compiled there, may be kept for the next game
 * (emusuni_jit_arena_generation() changes at every reset). NULL when the arena can't be made executable on this
 * console, or has no room left: the core runs its interpreter then. A name of another core while parts are out means
 * the last core's end wasn't given back: only one core runs at a time, so that one is gone and the arena is taken
 * back from it first (the log says so).
 *
 * Made read/write/execute the first time anything asks (emusuni_make_executable: mprotect on the owner's console, way
 * 0), and with mprotect again at every take: a core may take the execute right away at its end (the N64's dynarec
 * did) or when it starts again. At the reset the parts handed out are filled with 0xF4 again (HLT: a jump into code
 * of a core that is gone faults - SIGBUS - into crash.log instead of running it); the log says how much of each part
 * was written. */
#define JIT_ARENA_SIZE ((size_t)32 << 20)
#define JIT_ARENA_PARTS 8
#define JIT_ARENA_FILL 0xF4

static unsigned char s_jit_arena[JIT_ARENA_SIZE] __attribute__((aligned(16384)));
static pthread_mutex_t s_jit_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_jit_way = -2;  // how it was made executable (0-3), -1: it can't be, -2: not tried yet
static unsigned s_jit_generation = 0;
static struct
{
  char name[32];
  size_t offset, size;
} s_jit_parts[JIT_ARENA_PARTS];
static unsigned s_jit_part_count = 0;
static size_t s_jit_handed = 0;  // bytes handed out since the last reset (whole pages)

static double MiB(size_t bytes)
{
  return (double)bytes / 1048576.0;
}

// "11.0 MiB", or "32 KiB" below 1 MiB (a part of the Dreamcast's DSP).
static const char* SizeText(char* text, size_t length, size_t bytes)
{
  if (bytes == 0 || bytes >= ((size_t)1 << 20))
    snprintf(text, length, "%.1f MiB", MiB(bytes));
  else
    snprintf(text, length, "%u KiB", (unsigned)((bytes + 1023) / 1024));
  return text;
}

// Signed MiB from the start of the program's code.
static double MiBFromCode(const void* address)
{
  return (double)((intptr_t)address - (intptr_t)__text_start) / 1048576.0;
}

// The arena made read/write/execute and filled, the first time: the way (0-3) or -1. With s_jit_lock held.
static int JitArenaReady(void)
{
  if (s_jit_way != -2)
    return s_jit_way;
  s_jit_way = emusuni_make_executable(s_jit_arena, JIT_ARENA_SIZE);
  char line[256];
  if (s_jit_way >= 0)
  {
    memset(s_jit_arena, JIT_ARENA_FILL, JIT_ARENA_SIZE);
    snprintf(line, sizeof(line), "jit arena: %.1f MiB at %p (%+.1f MiB from the code's start), read/write/execute by way %d (%s)",
             MiB(JIT_ARENA_SIZE), (void*)s_jit_arena, MiBFromCode(s_jit_arena), s_jit_way, METHODS[s_jit_way]);
  }
  else
    snprintf(line, sizeof(line), "jit arena: %.1f MiB at %p can't be made executable: the recompiling cores run their interpreters",
             MiB(JIT_ARENA_SIZE), (void*)s_jit_arena);
  emusuni_log_line(line);
  return s_jit_way;
}

// How much of a part was written: up to its last byte that isn't the fill.
static size_t JitWritten(const unsigned char* start, size_t size)
{
  const uint64_t fill = 0x0101010101010101ull * JIT_ARENA_FILL;
  size_t words = size / 8;
  for (; words > 0; words--)
  {
    uint64_t word;
    memcpy(&word, start + (words - 1) * 8, 8);
    if (word != fill)
      break;
  }
  size_t bytes = words * 8;
  while (bytes > 0 && start[bytes - 1] == JIT_ARENA_FILL)
    bytes--;
  return bytes;
}

// Every part forgotten and what was handed out filled again; the log names them (`late`: taken back from a core
// whose end wasn't given back). With s_jit_lock held.
static void JitArenaGiveBack(int late)
{
  if (s_jit_part_count > 0)
  {
    char line[400], written[24], size[24];
    size_t at = (size_t)snprintf(line, sizeof(line), "jit arena: given back%s by", late ? " late (no reset after its end)" : "");
    for (unsigned i = 0; i < s_jit_part_count && at < sizeof(line); i++)
      at += (size_t)snprintf(
          line + at, sizeof(line) - at, "%s %s (%s of %s written)", i ? "," : "", s_jit_parts[i].name,
          SizeText(written, sizeof(written), JitWritten(s_jit_arena + s_jit_parts[i].offset, s_jit_parts[i].size)),
          SizeText(size, sizeof(size), s_jit_parts[i].size));
    emusuni_log_line(line);
    // Writable again first (a core may have left its code read-only).
    if (s_jit_way == 0)
      sceKernelMprotect(s_jit_arena, s_jit_handed, PROT_RWX);
    memset(s_jit_arena, JIT_ARENA_FILL, s_jit_handed);
  }
  s_jit_part_count = 0;
  s_jit_handed = 0;
  __atomic_store_n(&s_jit_generation, s_jit_generation + 1, __ATOMIC_RELEASE);
}

// The core a part's name belongs to: its first word.
static int SameCore(const char* a, const char* b)
{
  size_t i = 0;
  for (; a[i] && a[i] != ' ' && b[i] && b[i] != ' '; i++)
    if (a[i] != b[i])
      return 0;
  return (a[i] == 0 || a[i] == ' ') && (b[i] == 0 || b[i] == ' ');
}

void* emusuni_jit_arena(size_t size, const char* name)
{
  char line[256], size_text[24], other_text[24];
  void* part = NULL;
  int index = -1;
  int32_t ret = 0;
  if (name == NULL || name[0] == 0)
    name = "?";
  size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
  pthread_mutex_lock(&s_jit_lock);
  if (JitArenaReady() < 0)
  {
    snprintf(line, sizeof(line), "jit arena: none for %s (not executable on this console)", name);
    emusuni_log_line(line);
    goto done;
  }
  if (size == 0 || size > JIT_ARENA_SIZE)
  {
    snprintf(line, sizeof(line), "jit arena: %s asked for %s, the arena has %.1f MiB", name,
             SizeText(size_text, sizeof(size_text), size), MiB(JIT_ARENA_SIZE));
    emusuni_log_line(line);
    goto done;
  }
  for (unsigned i = 0; i < s_jit_part_count; i++)
    if (strncmp(s_jit_parts[i].name, name, sizeof(s_jit_parts[i].name) - 1) == 0)
      index = (int)i;
  if (index < 0 && s_jit_part_count > 0 && !SameCore(s_jit_parts[0].name, name))
    JitArenaGiveBack(1);
  if (index >= 0)
  {
    // Asked for again in the same game: the same memory, executable again.
    if (size > s_jit_parts[index].size)
    {
      snprintf(line, sizeof(line), "jit arena: %s asked for %s, its part has %s", name, SizeText(size_text, sizeof(size_text), size),
               SizeText(other_text, sizeof(other_text), s_jit_parts[index].size));
      emusuni_log_line(line);
      goto done;
    }
    part = s_jit_arena + s_jit_parts[index].offset;
    size = s_jit_parts[index].size;
  }
  else
  {
    if (s_jit_part_count == JIT_ARENA_PARTS || size > JIT_ARENA_SIZE - s_jit_handed)
    {
      snprintf(line, sizeof(line), "jit arena: no room for %s (%s asked, %s free, %u parts out)", name,
               SizeText(size_text, sizeof(size_text), size), SizeText(other_text, sizeof(other_text), JIT_ARENA_SIZE - s_jit_handed),
               s_jit_part_count);
      emusuni_log_line(line);
      goto done;
    }
    part = s_jit_arena + s_jit_handed;
  }
  if (s_jit_way == 0)
  {
    ret = sceKernelMprotect(part, size, PROT_RWX);
    if (ret != 0)
    {
      snprintf(line, sizeof(line), "jit arena: %s: %s at %p not executable again (%#x)", name,
               SizeText(size_text, sizeof(size_text), size), part, (unsigned)ret);
      emusuni_log_line(line);
      part = NULL;
      goto done;
    }
  }
  if (index < 0)
  {
    snprintf(s_jit_parts[s_jit_part_count].name, sizeof(s_jit_parts[0].name), "%s", name);
    s_jit_parts[s_jit_part_count].offset = s_jit_handed;
    s_jit_parts[s_jit_part_count].size = size;
    s_jit_part_count++;
    s_jit_handed += size;
    snprintf(line, sizeof(line), "jit arena: %s at %p for %s (+%s of the arena, way %d)", SizeText(size_text, sizeof(size_text), size),
             part, name, SizeText(other_text, sizeof(other_text), (size_t)((unsigned char*)part - s_jit_arena)), s_jit_way);
  }
  else
    snprintf(line, sizeof(line), "jit arena: %s at %p for %s again", SizeText(size_text, sizeof(size_text), size), part, name);
  emusuni_log_line(line);
done:
  pthread_mutex_unlock(&s_jit_lock);
  return part;
}

void emusuni_jit_arena_reset(void)
{
  pthread_mutex_lock(&s_jit_lock);
  JitArenaGiveBack(0);
  pthread_mutex_unlock(&s_jit_lock);
}

unsigned emusuni_jit_arena_generation(void)
{
  return __atomic_load_n(&s_jit_generation, __ATOMIC_ACQUIRE);
}

// Before an N64 game: may the dynarec's code run in the arena? The way (0-3) or -1 (the cached interpreter then).
int emusuni_jit_check(void)
{
  pthread_mutex_lock(&s_jit_lock);
  const int way = JitArenaReady();
  pthread_mutex_unlock(&s_jit_lock);
  return way;
}

/* ---- 00.63: where 00.64 may put the arena instead (a look, once at start: main.cpp) ----
 * 00.64 may take the arena out of the .bss (32 MiB less for the PS4 to charge at load): mapped from the system's
 * flexible memory when a recompiling core starts and given back at its end, as the 3DS's JIT memory is
 * (emusuni_map_executable) - but just past the program, where the cores' 32-bit offsets still reach, not at
 * 0x600000000 or above. Nothing of ours was ever mapped down there (the kernel library keeps objects of its own
 * low, port/ps4_runtime.cpp), so this only looks, and gives everything back at once: the program's data (the
 * mapping that holds the arena and those right after it, without a gap) and the next mapping above it; 64 MiB
 * reserved from the first 2 MiB boundary past the program (a hint: the kernel takes the first free range from
 * there); 16 KiB of system flexible memory mapped read/write/execute at its start and code run there; then
 * Resonance's way (its Dreamcast port): a plain read/write/execute mmap of 16 KiB, hinted 64 MiB past the code's
 * start and on in 64 MiB steps up to 1.5 GiB. Two lines in the log ("memory probe: ..."). */
#define WINDOW_SIZE ((size_t)64 << 20)
#define WINDOW_ALIGN ((uintptr_t)2 << 20)

struct VirtualQueryInfo  // libkernel's SceKernelVirtualQueryInfo
{
  uintptr_t start, end;
  int64_t offset;
  int32_t protection, memory_type;
  uint32_t flags;  // flexible, direct, stack, pooled, committed (bits 0-4)
  char name[32];
};

void emusuni_jit_window_probe(void)
{
  char line[512];
  const unsigned long free_before = FlexibleFreeKiB();
  // The program's end: the mapping that holds the arena and the ones that follow it without a gap (an mprotect
  // splits a mapping in parts), at most 32 of them; else the arena's end.
  struct VirtualQueryInfo here, next;
  memset(&here, 0, sizeof(here));
  memset(&next, 0, sizeof(next));
  uintptr_t image_end = (uintptr_t)s_jit_arena + JIT_ARENA_SIZE;
  const int32_t asked = sceKernelVirtualQuery(s_jit_arena, 0, &here, sizeof(here));
  int32_t asked_next = -1;
  unsigned parts = 0;
  int gap = 0;  // the next mapping found, above a gap
  if (asked == 0)
  {
    image_end = here.end;
    for (parts = 1; parts < 32; parts++)
    {
      asked_next = sceKernelVirtualQuery((const void*)image_end, 1 /* the one there, or the next */, &next, sizeof(next));
      if (asked_next != 0)
        break;
      if (next.start != image_end)
      {
        gap = 1;
        break;
      }
      image_end = next.end;
    }
  }
  if (image_end < (uintptr_t)s_jit_arena + JIT_ARENA_SIZE)
    image_end = (uintptr_t)s_jit_arena + JIT_ARENA_SIZE;
  here.name[sizeof(here.name) - 1] = 0;
  next.name[sizeof(next.name) - 1] = 0;
  if (asked == 0)
    snprintf(line, sizeof(line),
             "memory probe: the program's data %#llx-%#llx (%u mapping(s) from \"%s\", protection %#x), it ends %.1f MiB past "
             "the code's start; ",
             (unsigned long long)here.start, (unsigned long long)image_end, parts, here.name, (unsigned)here.protection,
             MiBFromCode((const void*)image_end));
  else
    snprintf(line, sizeof(line), "memory probe: the program's data not found (%#x), the arena ends %.1f MiB past the code's start; ",
             (unsigned)asked, MiBFromCode((const void*)image_end));
  const size_t at = strlen(line);
  if (gap)
    snprintf(line + at, sizeof(line) - at, "next mapping %#llx-%#llx (\"%s\", protection %#x), %.1f MiB above", (unsigned long long)next.start,
             (unsigned long long)next.end, next.name, (unsigned)next.protection,
             (double)((intptr_t)next.start - (intptr_t)image_end) / 1048576.0);
  else
    snprintf(line + at, sizeof(line) - at, "no mapping above it found (%#x)", (unsigned)asked_next);
  emusuni_log_line(line);

  // 64 MiB reserved from the first 2 MiB boundary past the program, 16 KiB of it mapped and run.
  void* window = (void*)((image_end + WINDOW_ALIGN - 1) & ~(WINDOW_ALIGN - 1));
  const int32_t reserved = sceKernelReserveVirtualRange(&window, WINDOW_SIZE, 0, WINDOW_ALIGN);
  char window_text[200] = "";
  if (reserved != 0)
    snprintf(window_text, sizeof(window_text), "64 MiB near %p not reserved (%#x)", window, (unsigned)reserved);
  else
  {
    const uintptr_t code = (uintptr_t)__text_start;
    char result[96];
    int32_t given_back = 0;
    if ((uintptr_t)window <= code || (uintptr_t)window + WINDOW_SIZE - code >= ((uintptr_t)2 << 30))
      snprintf(result, sizeof(result), "not tried (out of the code's 2 GiB reach)");
    else
    {
      void* map = window;
      const int32_t mapped =
          sceKernelMapNamedSystemFlexibleMemory(&map, PAGE, PROT_RWX, MAP_FIXED_FLAG, "emusuni jit window probe");
      if (mapped != 0)
        snprintf(result, sizeof(result), "refused (%#x)", (unsigned)mapped);
      else if (map != window)
        snprintf(result, sizeof(result), "mapped at %p instead", map);
      else if (Runs((unsigned char*)window))
        snprintf(result, sizeof(result), "works");
      else
        snprintf(result, sizeof(result), "maps, but code there faults");
      if (mapped == 0)
        given_back = sceKernelMunmap(map, PAGE);
    }
    const int32_t unreserved = sceKernelMunmap(window, WINDOW_SIZE);
    snprintf(window_text, sizeof(window_text),
             "64.0 MiB reserved at %p (%.1f MiB past the code's start); system flexible memory read/write/execute there: %s; "
             "given back (%#x, %#x)",
             window, MiBFromCode(window), result, (unsigned)given_back, (unsigned)unreserved);
  }

  // Resonance's way: the first plain read/write/execute mmap within 1800 MiB of the code.
  void* hinted = NULL;
  for (uintptr_t hint = ((uintptr_t)__text_start + ((uintptr_t)64 << 20) + WINDOW_ALIGN - 1) & ~(WINDOW_ALIGN - 1);
       hint < (uintptr_t)__text_start + ((uintptr_t)1536 << 20); hint += (uintptr_t)64 << 20)
  {
    void* p = __real_mmap((void*)hint, PAGE, PROT_RWX, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED || p == NULL)
      continue;
    const double distance = MiBFromCode(p);
    if (distance > -1800.0 && distance < 1800.0)
    {
      hinted = p;
      break;
    }
    sceKernelMunmap(p, PAGE);
  }
  char hinted_text[160];
  if (hinted == NULL)
    snprintf(hinted_text, sizeof(hinted_text), "hinted mmap read/write/execute: none within reach");
  else
  {
    const int works = Runs((unsigned char*)hinted);
    const int32_t given_back = sceKernelMunmap(hinted, PAGE);
    snprintf(hinted_text, sizeof(hinted_text), "hinted mmap read/write/execute at %p (%.1f MiB past the code's start): %s (given back %#x)",
             hinted, MiBFromCode(hinted), works ? "works" : "code there faults", (unsigned)given_back);
  }
  snprintf(line, sizeof(line), "memory probe: %s; %s; flexible %lu KiB free before, %lu after", window_text, hinted_text,
           free_before, FlexibleFreeKiB());
  emusuni_log_line(line);
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
  char line[224];
  if (emusuni_n64_fixed_cores)
    snprintf(line, sizeof(line), "n64: thread \"%s\": core %d (%#x), priority %d (%#x), running on CPU %d, flexible %lu KiB free",
             ROLES[role - 1].name, core, (unsigned)affinity, priority, (unsigned)prio, sceKernelGetCurrentCpu(),
             FlexibleFreeKiB());
  else
    snprintf(line, sizeof(line), "n64: thread \"%s\": the PS4's cores, priority %d (%#x), running on CPU %d, flexible %lu KiB free",
             ROLES[role - 1].name, priority, (unsigned)prio, sceKernelGetCurrentCpu(), FlexibleFreeKiB());
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
  char line[224];
  snprintf(line, sizeof(line), "nds: thread \"%s\": core %d (%#x), priority 480 (%#x), running on CPU %d, flexible %lu KiB free",
           name, core, (unsigned)affinity, (unsigned)prio, sceKernelGetCurrentCpu(), FlexibleFreeKiB());
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
  char line[224];
  snprintf(line, sizeof(line), "psp: thread \"%s\": cores %#llx (%#x), priority %d (%#x), running on CPU %d, flexible %lu KiB free",
           name, (unsigned long long)mask, (unsigned)affinity, priority, (unsigned)prio, sceKernelGetCurrentCpu(),
           FlexibleFreeKiB());
  emusuni_log_line(line);
}

/* Flycast's threads (scripts/build-dc-core.sh) call this first with Flycast's name for them:
 * "Flycast-emu" (SH4, ARM7, AICA with threaded rendering; a new one at every start: game start,
 * reset, state loaded), "PicoTCP" (modem networking, only if a game dials), "CustomTexLoader" (off)...
 * retro_run's thread is the renderer (core 2, 480) and waits for Flycast-emu every frame on condition
 * variables (no spinning): Flycast-emu beside it on core 3 (same module, shared L2) at the same
 * priority; the rest below them on cores 1, 4, 5. With emusuni_dc_threads_free (src/Emulator.cpp sets it
 * before the game starts, while the file /data/EmuSuNi/dc-threads-free exists: an A/B for the PS4, as
 * Resonance places none) each is left on the cores and at the priority it starts with. */
int emusuni_dc_threads_free = 0;
extern int emusuni_get_priority(void* thread, int* priority) __asm__("scePthreadGetprio");
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
  if (emusuni_dc_threads_free)
  {
    int now = 0;
    const int got = emusuni_get_priority(self, &now);
    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "dc %s", name);
    emusuni_rename_thread(self, thread_name);
    char line[256];
    snprintf(line, sizeof(line),
             "dc: thread \"%s\": the PS4's cores, priority %d as it started (%#x; dc-threads-free), running on CPU %d, "
             "flexible %lu KiB free",
             name, now, (unsigned)got, sceKernelGetCurrentCpu(), FlexibleFreeKiB());
    emusuni_log_line(line);
    return;
  }
  const int affinity = emusuni_set_affinity(self, mask);
  const int prio = emusuni_set_priority(self, priority);
  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "dc %s", name);
  emusuni_rename_thread(self, thread_name);
  char line[224];
  snprintf(line, sizeof(line), "dc: thread \"%s\": cores %#llx (%#x), priority %d (%#x), running on CPU %d, flexible %lu KiB free",
           name, (unsigned long long)mask, (unsigned)affinity, priority, (unsigned)prio, sceKernelGetCurrentCpu(),
           FlexibleFreeKiB());
  emusuni_log_line(line);
}

/* Azahar's threads (scripts/build-3ds-core.sh, builds with EMUSUNI_3DS) call this first with the name
 * Azahar gives them, or the script: "VulkanWorker" (records and submits the Vulkan work beside the
 * emulation: retro_run waits for it at the end of every frame), "VulkanFence" (waits for the GPU's
 * fences and tells the emulation), "Pipeline workers" and "Shader workers" (3 each: pipelines and
 * GLSL/SPIR-V compiled ahead of use), "Disk Shader Cache Append Worker", "FSUSER_Worker" and "HleAsync"
 * (the games' file work: many short-lived ones), "citra:Log", "Teakra", "AmScan"... The emulation
 * thread (retro_run's: both or all four ARM11 cores, one after the other) is never named:
 * src/Emulator.cpp keeps it on core 2 at 480. The Vulkan worker beside it on core 3 (the same CPU
 * module) at its priority, and the fence thread (it only sleeps and signals) at the same off core 3;
 * all the workers at ONE priority (700) on cores 0, 1, 3, 4 and 5 (every core but the emulation's: core
 * 0 has only the sound's short turns at 400 in a game, so a burst of new pipelines is built 3 at a time
 * where 2 were; the GPU filter's compiles take the same five cores), the log and the disk writers a little
 * below (760): the PS4 runs threads by fixed priorities, and workers of several priorities taking the
 * same locks froze the app once (the heap's spin lock, port/ps4_dlmalloc.c - a mutex now, but the
 * emulation waits for the Vulkan worker, so nothing else may run ahead of it on its core). Each name is
 * logged once (HleAsync would fill the log). */
#ifdef EMUSUNI_3DS
void emusuni_3ds_thread(const char* name)
{
  uint64_t mask = (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5);
  int priority = 700;
  if (strcmp(name, "VulkanWorker") == 0)
  {
    mask = 1ull << 3;
    priority = 480;
  }
  else if (strcmp(name, "VulkanFence") == 0)
  {
    mask = (1ull << 4) | (1ull << 5);
    priority = 480;
  }
  else if (strcmp(name, "citra:Log") == 0 || strncmp(name, "Disk Shader Cache", 17) == 0)
  {
    priority = 760;
  }
  void* self = emusuni_pthread_self();
  const int affinity = emusuni_set_affinity(self, mask);
  const int prio = emusuni_set_priority(self, priority);
  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "3ds %s", name);
  emusuni_rename_thread(self, thread_name);
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static char seen[24][40];
  static unsigned seen_count = 0;
  int first = 1;
  pthread_mutex_lock(&lock);
  for (unsigned i = 0; i < seen_count; i++)
  {
    if (strncmp(seen[i], name, sizeof(seen[i]) - 1) == 0)
      first = 0;
  }
  if (first && seen_count < sizeof(seen) / sizeof(seen[0]))
  {
    snprintf(seen[seen_count], sizeof(seen[0]), "%s", name);
    seen_count++;
  }
  pthread_mutex_unlock(&lock);
  if (!first)
    return;
  char line[256];
  snprintf(line, sizeof(line), "3ds: thread \"%s\": cores %#llx (%#x), priority %d (%#x), running on CPU %d, flexible %lu KiB free",
           name, (unsigned long long)mask, (unsigned)affinity, priority, (unsigned)prio, sceKernelGetCurrentCpu(),
           FlexibleFreeKiB());
  emusuni_log_line(line);
}

/* The 3DS core's JIT memory (scripts/build-3ds-core.sh: common/emusuni_jit.cpp), where its recompilers
 * write their code and run it: `size` bytes of system flexible memory on a range reserved above the app's
 * image and heap (as port/ps4_runtime.cpp's heap and DolphinPS4's JIT buffers; the kernel's own objects
 * live lower). First mapped readable, writable and executable at once (DolphinPS4's way); when the PS4
 * refuses that or code there faults, mapped read/write from the same pool (as the heap is) and made
 * executable the ways emusuni_make_executable knows - mprotect first, the way every other JIT pool runs
 * on the owner's console. Each checked by running code there; the log says which way it took. Mapped when
 * the first 3DS game starts and given back when the core shuts down (emusuni_unmap_executable): never an
 * array in the executable, whose whole .bss the PS4 charges to the games' flexible memory at load. The
 * recompilers reach the core only through far calls, so the range can be anywhere. NULL when no way works
 * (the log says why): then the 3DS game doesn't start, and nothing else is touched. */
void* emusuni_map_executable(size_t size, const char* name)
{
  static const uintptr_t HINTS[] = {0x1000000000ull, 0x800000000ull, 0x600000000ull};
  char line[256];
  size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
  for (unsigned i = 0; i < sizeof(HINTS) / sizeof(HINTS[0]); i++)
  {
    void* base = (void*)HINTS[i];
    int32_t ret = sceKernelReserveVirtualRange(&base, size, 0, PAGE);
    if (ret != 0)
    {
      snprintf(line, sizeof(line), "3ds: JIT memory (%s): no range of %.1f MiB near %#llx (%#x)", name,
               size / 1048576.0, (unsigned long long)HINTS[i], (unsigned)ret);
      emusuni_log_line(line);
      continue;
    }
    if ((uintptr_t)base < 0x400000000ull)
    {
      snprintf(line, sizeof(line), "3ds: JIT memory (%s): the range came at %p, among the kernel's: another", name, base);
      emusuni_log_line(line);
      sceKernelMunmap(base, size);
      continue;
    }
    /* 1. Read, write and execute at once. */
    void* at = base;
    ret = sceKernelMapNamedSystemFlexibleMemory(&at, size, PROT_RWX, MAP_FIXED_FLAG, name);
    if (ret == 0 && at == base && Runs((unsigned char*)base))
    {
      snprintf(line, sizeof(line),
               "3ds: JIT memory (%s): %.1f MiB of system flexible memory at %p, read, write and execute at once", name,
               size / 1048576.0, base);
      emusuni_log_line(line);
      return base;
    }
    if (ret != 0)
      snprintf(line, sizeof(line),
               "3ds: JIT memory (%s): %.1f MiB of system flexible memory read, write and execute refused (%#x): "
               "read/write, then made executable",
               name, size / 1048576.0, (unsigned)ret);
    else
      snprintf(line, sizeof(line),
               "3ds: JIT memory (%s): %.1f MiB mapped read, write and execute at %p, but code there faults: "
               "read/write, then made executable",
               name, size / 1048576.0, at);
    emusuni_log_line(line);
    if (ret == 0)
    {
      /* Nothing of the first try left before the second: its mapping out (with the reservation under it), the
       * same range reserved again. */
      sceKernelMunmap(at, size);
      if (at == base)
      {
        void* again = base;
        ret = sceKernelReserveVirtualRange(&again, size, MAP_FIXED_FLAG, PAGE);
        if (ret != 0 || again != base)
        {
          snprintf(line, sizeof(line), "3ds: JIT memory (%s): the range at %p not reserved again (%#x, %p)", name, base,
                   (unsigned)ret, again);
          emusuni_log_line(line);
          if (ret == 0)
            sceKernelMunmap(again, size);
          return NULL;
        }
      }
    }
    /* 2. Read/write from the same pool, then made executable (mprotect first). */
    at = base;
    ret = sceKernelMapNamedSystemFlexibleMemory(&at, size, PROT_READ | PROT_WRITE, MAP_FIXED_FLAG, name);
    if (ret != 0 || at != base)
    {
      snprintf(line, sizeof(line), "3ds: JIT memory (%s): %.1f MiB of system flexible memory refused read/write too (%#x)",
               name, size / 1048576.0, (unsigned)ret);
      emusuni_log_line(line);
      if (ret == 0)
        sceKernelMunmap(at, size);
      sceKernelMunmap(base, size);
      return NULL;
    }
    const int way = emusuni_make_executable(base, size);
    if (way >= 0)
    {
      snprintf(line, sizeof(line), "3ds: JIT memory (%s): %.1f MiB of system flexible memory at %p, made executable by way %d (%s)",
               name, size / 1048576.0, base, way, METHODS[way]);
      emusuni_log_line(line);
      return base;
    }
    snprintf(line, sizeof(line), "3ds: JIT memory (%s): %.1f MiB at %p can't be made executable by any way", name,
             size / 1048576.0, base);
    emusuni_log_line(line);
    sceKernelMunmap(base, size);
    return NULL;
  }
  emusuni_log_line("3ds: JIT memory: no range could be reserved for it");
  return NULL;
}

void emusuni_unmap_executable(void* address, size_t size)
{
  char line[160];
  size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
  const int32_t ret = sceKernelMunmap(address, size);
  snprintf(line, sizeof(line), "3ds: JIT memory: %.1f MiB at %p given back (%#x)", size / 1048576.0, address,
           (unsigned)ret);
  emusuni_log_line(line);
}
#endif /* EMUSUNI_3DS */

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

/* EmuSuNi 00.72: ParaLLEl-RSP's tasks that stopped to wait for a signal from the CPU and ran on when it came
 * (scripts/build-n64-core.sh: World Driver Championship's and Stunt Racer 64's microcode does it dozens of times a
 * frame; most games never). Counted by the core, read and cleared here every 10 s. */
unsigned emusuni_n64_rsp_waits = 0;

static long long s_section_ns[NUM_TIMED_SECTIONS], s_section_start[NUM_TIMED_SECTIONS];
/* EmuSuNi 00.68: how many of each (a graphics or sound task: one RSP task) and the longest one, per window. */
static unsigned s_section_count[NUM_TIMED_SECTIONS];
static long long s_section_max_ns[NUM_TIMED_SECTIONS];
static long long s_window_start = 0;
static unsigned s_window_frames = 0;
/* EmuSuNi 00.72: this frame's time in each section, and the window's heaviest frame by them (what a hitch was made
 * of: an RSP task, the dynarec compiling code run for the first time, the sound); the pictures the game showed (the VI
 * origin's changes, scripts/build-n64-core.sh). */
static long long s_frame_ns[NUM_TIMED_SECTIONS], s_heaviest_ns[NUM_TIMED_SECTIONS];
static unsigned s_frame_count[NUM_TIMED_SECTIONS], s_heaviest_count[NUM_TIMED_SECTIONS];
static unsigned s_vi_origins = 0;

void emusuni_n64_vi_origin(void)
{
  s_vi_origins++;
}

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
  const long long took = now_ns() - s_section_start[section];
  s_section_ns[section] += took;
  s_section_count[section]++;
  if (took > s_section_max_ns[section])
    s_section_max_ns[section] = took;
  s_frame_ns[section] += took;
  s_frame_count[section]++;
}

/* The frame that just ended: the window's heaviest so far by the time its parts took? */
static void EndFrame(void)
{
  const long long parts = s_frame_ns[TIMED_SECTION_GFX] + s_frame_ns[TIMED_SECTION_AUDIO] +
                          s_frame_ns[TIMED_SECTION_COMPILER];
  const long long heaviest = s_heaviest_ns[TIMED_SECTION_GFX] + s_heaviest_ns[TIMED_SECTION_AUDIO] +
                             s_heaviest_ns[TIMED_SECTION_COMPILER];
  if (parts > heaviest)
  {
    memcpy(s_heaviest_ns, s_frame_ns, sizeof(s_heaviest_ns));
    memcpy(s_heaviest_count, s_frame_count, sizeof(s_heaviest_count));
  }
  memset(s_frame_ns, 0, sizeof(s_frame_ns));
  memset(s_frame_count, 0, sizeof(s_frame_count));
}

static void ResetWindow(long long now)
{
  s_window_start = now;
  s_window_frames = 0;
  memset(s_section_ns, 0, sizeof(s_section_ns));
  memset(s_section_count, 0, sizeof(s_section_count));
  memset(s_section_max_ns, 0, sizeof(s_section_max_ns));
  memset(s_heaviest_ns, 0, sizeof(s_heaviest_ns));
  memset(s_heaviest_count, 0, sizeof(s_heaviest_count));
  s_vi_origins = 0;
  emusuni_n64_rsp_waits = 0;
}

void timed_sections_refresh(void)
{
  const long long now = now_ns();
  EndFrame();
  if (s_window_start == 0 || now - s_window_start > 60000000000LL)
  {
    ResetWindow(now);  /* the first frame, or a game started again after a long pause */
    return;
  }
  s_window_frames++;
  if (now - s_window_start < 10000000000LL)
    return;
  const double frames = s_window_frames ? (double)s_window_frames : 1.0;
  const double seconds = (now - s_window_start) / 1e9;
  char line[640];
  /* (The tasks a frame: a game drawing at 30 FPS sends a graphics task every 2nd frame; the longest one is a
   * heavy frame's - a stutter.) 00.72: the recompiler's blocks (code run for the first time: a hitch where it is a lot),
   * the pictures the game showed a second (against the frames: the game's own frame rate), the heaviest frame's parts
   * (with the speed line's "most": what a hitch was), the RSP's waits for the CPU (World Driver Championship). */
  int at = snprintf(line, sizeof(line),
                    "n64 core per frame: graphics tasks (RSP) %.2f ms (%.2f tasks, the longest %.1f ms), sound tasks %.2f "
                    "ms (%.2f), recompiler %.2f ms; %.1f frames per second; the game showed %.1f pictures a second; "
                    "recompiled %u blocks in %.0f ms; the heaviest frame: RSP %.1f ms (%u tasks), recompiler %.1f ms "
                    "(%u blocks), sound %.1f ms",
                    s_section_ns[TIMED_SECTION_GFX] / 1e6 / frames, s_section_count[TIMED_SECTION_GFX] / frames,
                    s_section_max_ns[TIMED_SECTION_GFX] / 1e6, s_section_ns[TIMED_SECTION_AUDIO] / 1e6 / frames,
                    s_section_count[TIMED_SECTION_AUDIO] / frames, s_section_ns[TIMED_SECTION_COMPILER] / 1e6 / frames,
                    frames / seconds, s_vi_origins / seconds, s_section_count[TIMED_SECTION_COMPILER],
                    s_section_ns[TIMED_SECTION_COMPILER] / 1e6, s_heaviest_ns[TIMED_SECTION_GFX] / 1e6,
                    s_heaviest_count[TIMED_SECTION_GFX], s_heaviest_ns[TIMED_SECTION_COMPILER] / 1e6,
                    s_heaviest_count[TIMED_SECTION_COMPILER], s_heaviest_ns[TIMED_SECTION_AUDIO] / 1e6);
  if (emusuni_n64_rsp_waits > 0 && at > 0 && at < (int)sizeof(line))
    snprintf(line + at, sizeof(line) - (size_t)at, "; the RSP waited for the CPU %u times", emusuni_n64_rsp_waits);
  emusuni_log_line(line);
  /* A game that sends sound tasks but no graphics task for 10 s isn't drawing: its picture stays as it was (black
   * after a fade). Cruis'n USA and Resident Evil 2 did that with N64 Graphics Sync Fast, each waiting for something
   * the asynchronous RDP had not finished (src/Emulator.cpp N64AccurateGame); another game doing it is a candidate. */
  if (s_section_count[TIMED_SECTION_GFX] == 0 && s_section_count[TIMED_SECTION_AUDIO] > 0)
    emusuni_log_line("n64: no graphics task in these 10 s while the game's sound runs: the picture isn't changing "
                     "(black or frozen?). If it stays so, try N64 Graphics Sync: Accurate.");
  ResetWindow(now);
}
