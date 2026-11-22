/* EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
 *
 * The PS Vita core's memory, page faults and code memory on the PS4 (port/ps4_vita_sys.h). EmuSuNi's
 * own, after the ways the Dreamcast's and the 3DS's cores already run on the console
 * (scripts/build-dc-core.sh: emusuni_vmem.cpp; scripts/build-3ds-core.sh: emusuni_fastmem.cpp).
 *
 * The guest's memory. Vita3K reserves 4 GiB with mmap(PROT_NONE) and turns pages on and off with
 * mprotect. The PS4's kernel can't reserve that much with mmap and has no memory behind a
 * reservation, so:
 *   - the 4 GiB are a sceKernelReserveVirtualRange reservation;
 *   - the memory is direct memory, taken in 2 MiB pieces: the first page in use of a 2 MiB piece of
 *     the guest's addresses allocates one and maps it whole (2 MiB aligned on both sides: a large
 *     page where the kernel gives one), the last page out of use gives it back;
 *   - "no access" unmaps the pages (the range reserved again); the direct memory keeps what they
 *     hold, and mapping them again brings it back. "Read only" is sceKernelMprotect.
 * So a page of a piece that isn't in use can still be read and written (Vita3K: it faults). Nothing
 * correct depends on that.
 *
 * The PC: stand-ins for the kernel's calls that copy what the PS4 does and the PC wouldn't mind
 * (16 KiB pages, a fixed mapping only inside a reservation, direct memory that comes as it was
 * left, filled with 0xA5), so that the same code is what the PC tests run.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ps4_vita_sys.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define PAGE ((uint64_t)EMUSUNI_VITA_PAGE)
#define PIECE ((uint64_t)2 << 20)
#define SPACE ((uint64_t)1 << 32)
/* (Past the end: a recompiled access at the last bytes reads a few bytes on.) */
#define GUARD PIECE
#define PAGES ((unsigned)(SPACE / PAGE))
#define PIECES ((unsigned)(SPACE / PIECE))
#define PIECE_PAGES ((unsigned)(PIECE / PAGE))

enum { ACCESS_NONE = 0, ACCESS_READ = 1, ACCESS_RW = 3, ACCESS_BITS = 3, IN_USE = 0x80 };

/* ================= The kernel's calls ================= */
#if defined(__ORBIS__)

/* Declared by hand: several OpenOrbis prototypes are wrong (as the other cores have them). */
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t length, size_t alignment,
                                      int32_t memory_type, off_t *physical);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t length);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int32_t protection, int32_t flags, off_t physical,
                                 size_t alignment);
int32_t sceKernelReserveVirtualRange(void **address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void *address, size_t length);
int32_t sceKernelMprotect(const void *address, size_t length, int32_t protection);
int32_t sceKernelMapNamedSystemFlexibleMemory(void **address, size_t length, int32_t protection, int32_t flags,
                                              const char *name);
/* The app's (src/Log.cpp). */
void emusuni_log_line(const char *text);

#define K_FIXED 0x10 /* SCE_KERNEL_MAP_FIXED */

static void log_text(const char *text)
{
  emusuni_log_line(text);
}

static int32_t k_reserve(void **address, size_t length, int fixed, size_t alignment)
{
  return sceKernelReserveVirtualRange(address, length, fixed ? K_FIXED : 0, alignment);
}

static int32_t k_munmap(void *address, size_t length)
{
  return sceKernelMunmap(address, length);
}

static int32_t k_allocate(size_t length, size_t alignment, off_t *physical)
{
  return sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), length, alignment, 0 /* WB_ONION */,
                                       physical);
}

static int32_t k_release(off_t physical, size_t length)
{
  return sceKernelReleaseDirectMemory(physical, length);
}

static int32_t k_map(void *address, size_t length, int access, off_t physical, size_t alignment)
{
  void *at = address;
  const int32_t r = sceKernelMapDirectMemory(&at, length, access, K_FIXED, physical, alignment);
  if (r == 0 && at != address)
  {
    sceKernelMunmap(at, length);
    return -1;
  }
  return r;
}

/* (17-11) Is the page at `at` part of a 2 MiB page? The kernel's own word: FreeBSD's mincore and its
 * MINCORE_SUPER bit, which libkernel exports (called by a name of this file's: the headers' prototype differs).
 * 1 yes, 0 no, -1 not told. */
int emusuni_vita_mincore(const void *address, size_t length, char *vec) __asm__("mincore");
static int k_large(void *at)
{
  char vec[4] = {0, 0, 0, 0};
  if (emusuni_vita_mincore(at, (size_t)PAGE, vec) != 0)
    return -1;
  return (vec[0] & 0x20) ? 1 : 0;
}

static int32_t k_protect(void *address, size_t length, int access)
{
  return sceKernelMprotect(address, length, access);
}

/* Memory for code inside a reservation: system flexible memory (as the app's heap and the 3DS core's code
 * memory, port/ps4_n64.c), readable, writable and executable at once, or readable and writable only. */
static int32_t k_map_code(void *address, size_t length, int executable)
{
  void *at = address;
  const int32_t r = sceKernelMapNamedSystemFlexibleMemory(&at, length, executable ? 7 : 3, K_FIXED, "vita jit");
  if (r == 0 && at != address)
  {
    sceKernelMunmap(at, length);
    return -1;
  }
  return r;
}

#else /* the PC's stand-ins */

#include <fcntl.h>
#include <ucontext.h>
#include <unistd.h>

#define DM_SIZE ((uint64_t)2 << 30)
#define DM_PIECES ((unsigned)(DM_SIZE / PIECE))
static int s_dm_fd = -1;
static uint8_t s_dm_taken[DM_PIECES];

static void log_text(const char *text)
{
  fprintf(stderr, "[emusuni-vita] %s\n", text);
}

/* EMUSUNI_VITA_SYS_FAIL=<word>: one thing refused, for the tests (reserve, allocate, map, exec). */
static int refused_for_a_test(const char *what)
{
  const char *list = getenv("EMUSUNI_VITA_SYS_FAIL");
  return list != NULL && strcmp(list, what) == 0;
}

static int32_t k_reserve(void **address, size_t length, int fixed, size_t alignment)
{
  const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
  if ((((uintptr_t)*address) | length) & (PAGE - 1))
    return -22;
  if (refused_for_a_test("reserve"))
    return -12;
  void *p;
  if (fixed)
    p = mmap(*address, length, PROT_NONE, flags | MAP_FIXED, -1, 0);
  else
  {
    /* (A hint that isn't free fails, as on the console.) */
    p = mmap(*address, length, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED && p != *address)
    {
      munmap(p, length);
      p = MAP_FAILED;
    }
  }
  if (p == MAP_FAILED)
    return -12;
  if ((uintptr_t)p & (alignment - 1))
  {
    munmap(p, length);
    return -22;
  }
  *address = p;
  return 0;
}

static int32_t k_munmap(void *address, size_t length)
{
  if ((((uintptr_t)address) | length) & (PAGE - 1))
    return -22;
  return munmap(address, length) == 0 ? 0 : -22;
}

static int32_t k_allocate(size_t length, size_t alignment, off_t *physical)
{
  if (length != PIECE || alignment != PIECE || refused_for_a_test("allocate"))
    return -12;
  if (s_dm_fd < 0)
  {
    s_dm_fd = memfd_create("vita-direct-memory", 0);
    if (s_dm_fd < 0 || ftruncate(s_dm_fd, (off_t)DM_SIZE) != 0)
      return -12;
  }
  for (unsigned i = 0; i < DM_PIECES; i++)
  {
    if (s_dm_taken[i])
      continue;
    void *p = mmap(NULL, PIECE, PROT_READ | PROT_WRITE, MAP_SHARED, s_dm_fd, (off_t)(i * PIECE));
    if (p == MAP_FAILED)
      return -12;
    memset(p, 0xA5, PIECE); /* as it was left: nothing may count on zeros */
    munmap(p, PIECE);
    s_dm_taken[i] = 1;
    *physical = (off_t)(i * PIECE);
    return 0;
  }
  return -12;
}

static int32_t k_release(off_t physical, size_t length)
{
  if (length != PIECE || ((uint64_t)physical & (PIECE - 1)) || !s_dm_taken[(uint64_t)physical / PIECE])
    return -22;
  s_dm_taken[(uint64_t)physical / PIECE] = 0;
  fallocate(s_dm_fd, 0x03 /* FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE */, physical, (off_t)length);
  return 0;
}

static int32_t k_map(void *address, size_t length, int access, off_t physical, size_t alignment)
{
  if (length == 0 || ((((uintptr_t)address) | length | (uint64_t)physical) & (PAGE - 1)))
    return -22;
  if (alignment > PAGE && ((((uintptr_t)address) | length | (uint64_t)physical) & (alignment - 1)))
    return -22;
  if (refused_for_a_test("map"))
    return -12;
  const int prot = (access & 1 ? PROT_READ : 0) | (access & 2 ? PROT_WRITE : 0);
  return mmap(address, length, prot, MAP_SHARED | MAP_FIXED, s_dm_fd, physical) == MAP_FAILED ? -12 : 0;
}

static int32_t k_protect(void *address, size_t length, int access)
{
  if ((((uintptr_t)address) | length) & (PAGE - 1))
    return -22;
  const int prot = (access & 1 ? PROT_READ : 0) | (access & 2 ? PROT_WRITE : 0) | (access & 4 ? PROT_EXEC : 0);
  return mprotect(address, length, prot) == 0 ? 0 : -22;
}

/* (Linux's mincore has no word for a large page.) */
static int k_large(void *at)
{
  (void)at;
  return -1;
}

/* (For the tests: "rwx" refuses executable at once, as a console that only takes the second way; "exec"
 * refuses both.) */
static int32_t k_map_code(void *address, size_t length, int executable)
{
  if (length == 0 || ((((uintptr_t)address) | length) & (PAGE - 1)))
    return -22;
  if (refused_for_a_test("exec") || (executable && refused_for_a_test("rwx")))
    return -13;
  const int prot = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
  return mmap(address, length, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ? -12 : 0;
}

#endif

void emusuni_vita_log(const char *format, ...)
{
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  log_text(line);
}

/* (01.29) What is in the log so far, on the disk now: when the console ends the app (a fault of the GPU's: no
 * signal, no crash.log) the log is all there is to go by. The PS4's log is its stderr. */
void emusuni_vita_sync(void)
{
#if defined(__ORBIS__)
  fsync(STDERR_FILENO);
#else
  fflush(stderr);
#endif
}

/* (01.29) A step of the start ("vita: step: ..."), in the log and on the disk before the step is taken: the
 * last one there says where the app ended. */
#if !defined(__ORBIS__)
/* (port/ps4_vita_throw.cpp: the PC tests' failure at a step. Weak: a test of this file alone has none.) */
__attribute__((weak)) void emusuni_vita_test_fail(const char *step);
#endif
void emusuni_vita_step(const char *format, ...)
{
  char line[512] = "vita: step: ";
  const size_t head = strlen(line);
  va_list args;
  va_start(args, format);
  vsnprintf(line + head, sizeof(line) - head, format, args);
  va_end(args);
  log_text(line);
  emusuni_vita_sync();
#if !defined(__ORBIS__)
  if (emusuni_vita_test_fail)
    emusuni_vita_test_fail(line + head);
#endif
}

/* ================= The guest's memory ================= */
struct Piece
{
  int64_t physical; /* its direct memory, -1: none */
  uint32_t in_use;  /* how many of its pages are */
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_base;
static uint8_t *s_page; /* per page: IN_USE | its access now */
static struct Piece *s_piece;
static unsigned s_pieces_now, s_pieces_most, s_refusals;

static void jit_trim(void);

/* The first refusals of a kind in the log, not thousands of them. */
static void refused(const char *what, uint64_t address, uint64_t size, int32_t code)
{
  if (++s_refusals <= 16)
    emusuni_vita_log("vita: memory: %s refused at guest %#llx, %llu KiB (%#x)%s", what, (unsigned long long)address,
                     (unsigned long long)(size / 1024), (unsigned)code, s_refusals == 16 ? "; no more of these lines" : "");
}

/* Out of a reservation and reserved again, so nothing else lands there. */
static int unmap_to_reserved(void *address, size_t size)
{
  k_munmap(address, size);
  void *at = address;
  const int32_t r = k_reserve(&at, size, 1, PAGE);
  if (r != 0 || at != address)
  {
    if (++s_refusals <= 16)
      emusuni_vita_log("vita: memory: %zu KiB at %p not reserved again (%#x)", size / 1024, address, (unsigned)r);
    return 0;
  }
  return 1;
}

void *emusuni_vita_mem_reserve(void)
{
  static const uintptr_t HINTS[] = {0x4000000000ull, 0x4400000000ull, 0x4800000000ull, 0x5000000000ull,
                                    0x2000000000ull, 0x3000000000ull, 0x1C00000000ull};
  pthread_mutex_lock(&s_lock);
  void *base = NULL;
  if (s_base != NULL)
  {
    emusuni_vita_log("vita: memory: asked for twice");
    goto done;
  }
  s_page = (uint8_t *)calloc(PAGES, 1);
  s_piece = (struct Piece *)malloc(PIECES * sizeof(struct Piece));
  if (s_page == NULL || s_piece == NULL)
  {
    emusuni_vita_log("vita: memory: no memory for its tables");
    goto failed;
  }
  for (unsigned i = 0; i < PIECES; i++)
  {
    s_piece[i].physical = -1;
    s_piece[i].in_use = 0;
  }
  int32_t r = -1;
  for (unsigned i = 0; i < sizeof(HINTS) / sizeof(HINTS[0]) && base == NULL; i++)
  {
    void *at = (void *)HINTS[i];
    r = k_reserve(&at, SPACE + GUARD, 0, PIECE);
    if (r != 0)
      continue;
    /* (Low addresses are the kernel's own, port/ps4_n64.c.) */
    if ((uintptr_t)at < 0x400000000ull || ((uintptr_t)at & (PIECE - 1)))
    {
      k_munmap(at, SPACE + GUARD);
      r = -2;
      continue;
    }
    base = at;
  }
  if (base == NULL)
  {
    emusuni_vita_log("vita: memory: no 4 GiB of addresses to reserve (%#x)", (unsigned)r);
    goto failed;
  }
  s_base = (uint8_t *)base;
  s_pieces_now = s_pieces_most = s_refusals = 0;
  emusuni_vita_log("vita: memory: 4 GiB of addresses reserved at %p; direct memory in 2 MiB pieces as they are used", base);
  goto done;
failed:
  free(s_page);
  free(s_piece);
  s_page = NULL;
  s_piece = NULL;
done:
  pthread_mutex_unlock(&s_lock);
  return base;
}

void emusuni_vita_mem_release(void)
{
  pthread_mutex_lock(&s_lock);
  if (s_base != NULL)
  {
    k_munmap(s_base, SPACE + GUARD);
    for (unsigned i = 0; i < PIECES; i++)
    {
      if (s_piece[i].physical >= 0)
        k_release((off_t)s_piece[i].physical, PIECE);
    }
    emusuni_vita_log("vita: memory: given back; %u MiB under the guest's addresses at the end, %u MiB at most",
                     s_pieces_now * 2, s_pieces_most * 2);
    free(s_page);
    free(s_piece);
    s_page = NULL;
    s_piece = NULL;
    s_base = NULL;
    s_pieces_now = 0;
  }
  pthread_mutex_unlock(&s_lock);
  jit_trim();
}

void emusuni_vita_mem_usage(unsigned *now_mib, unsigned *most_mib)
{
  pthread_mutex_lock(&s_lock);
  *now_mib = s_pieces_now * 2;
  *most_mib = s_pieces_most * 2;
  pthread_mutex_unlock(&s_lock);
}

/* (17-11) For the log: which pieces could be, and are, one 2 MiB page. The table is read under the lock; the
 * kernel is asked after it, a piece at a time (one that went meanwhile isn't told: not counted). */
void emusuni_vita_mem_large(unsigned *pieces, unsigned *whole, int *large)
{
  static uint16_t s_asked[PIECES];
  static pthread_mutex_t s_asking = PTHREAD_MUTEX_INITIALIZER;
  unsigned mapped = 0, uniform = 0, ask = 0;
  int said = 0, told = 0;
  pthread_mutex_lock(&s_asking);
  pthread_mutex_lock(&s_lock);
  uint8_t *const base = s_base;
  if (base != NULL)
  {
    for (unsigned piece = 0; piece < PIECES; piece++)
    {
      if (s_piece[piece].physical < 0)
        continue;
      mapped++;
      unsigned page = 0;
      while (page < PIECE_PAGES && (s_page[piece * PIECE_PAGES + page] & ACCESS_BITS) == ACCESS_RW)
        page++;
      if (page == PIECE_PAGES)
        uniform++;
      s_asked[ask++] = (uint16_t)piece;
    }
  }
  pthread_mutex_unlock(&s_lock);
  for (unsigned i = 0; i < ask; i++)
  {
    const int is = k_large(base + s_asked[i] * PIECE);
    if (is < 0)
      continue;
    told = 1;
    said += is;
  }
  pthread_mutex_unlock(&s_asking);
  *pieces = mapped;
  *whole = uniform;
  *large = told ? said : -1;
}

/* A piece's direct memory, mapped whole, readable and writable. */
static int piece_map(unsigned piece)
{
  uint8_t *at = s_base + piece * PIECE;
  off_t physical = 0;
  int32_t r = k_allocate(PIECE, PIECE, &physical);
  if (r != 0)
  {
    refused("2 MiB of direct memory", piece * PIECE, PIECE, r);
    return 0;
  }
  r = k_map(at, PIECE, ACCESS_RW, physical, PIECE);
  if (r != 0)
    r = k_map(at, PIECE, ACCESS_RW, physical, PAGE);
  if (r != 0)
  {
    k_release(physical, PIECE);
    refused("mapping a piece", piece * PIECE, PIECE, r);
    return 0;
  }
  s_piece[piece].physical = physical;
  s_piece[piece].in_use = 0;
  memset(s_page + piece * PIECE_PAGES, ACCESS_RW, PIECE_PAGES);
  if (++s_pieces_now > s_pieces_most)
    s_pieces_most = s_pieces_now;
  return 1;
}

static void piece_unmap(unsigned piece)
{
  unmap_to_reserved(s_base + piece * PIECE, PIECE);
  k_release((off_t)s_piece[piece].physical, PIECE);
  s_piece[piece].physical = -1;
  s_piece[piece].in_use = 0;
  memset(s_page + piece * PIECE_PAGES, 0, PIECE_PAGES);
  s_pieces_now--;
}

/* count pages from `page` on, all of one mapped piece and of one access now, to `access`. */
static int run_access(unsigned page, unsigned count, int access)
{
  const int now = s_page[page] & ACCESS_BITS;
  uint8_t *at = s_base + page * PAGE;
  const size_t length = count * PAGE;
  if (now == access)
    return 1;
  if (access == ACCESS_NONE)
  {
    if (!unmap_to_reserved(at, length))
      return 0;
  }
  else
  {
    int32_t r;
    if (now == ACCESS_NONE)
      r = k_map(at, length, access, (off_t)(s_piece[page / PIECE_PAGES].physical + (page % PIECE_PAGES) * PAGE), PAGE);
    else
      r = k_protect(at, length, access);
    if (r != 0)
    {
      refused(now == ACCESS_NONE ? "mapping pages again" : "changing pages' access", page * PAGE, length, r);
      return 0;
    }
  }
  for (unsigned i = 0; i < count; i++)
    s_page[page + i] = (uint8_t)((s_page[page + i] & ~ACCESS_BITS) | access);
  return 1;
}

/* The pages [first, end) to `access`, in runs. Pages of pieces without memory stay as they are: not done. */
static int pages_access(unsigned first, unsigned end, int access)
{
  int done = 1;
  unsigned page = first;
  while (page < end)
  {
    const unsigned piece = page / PIECE_PAGES;
    unsigned piece_end = (piece + 1) * PIECE_PAGES;
    if (piece_end > end)
      piece_end = end;
    if (s_piece[piece].physical < 0)
    {
      done = 0;
      page = piece_end;
      continue;
    }
    unsigned run = 1;
    while (page + run < piece_end && (s_page[page + run] & ACCESS_BITS) == (s_page[page] & ACCESS_BITS))
      run++;
    if (!run_access(page, run, access))
      done = 0;
    page += run;
  }
  return done;
}

static int range_pages(uint64_t address, uint64_t size, unsigned *first, unsigned *end)
{
  if (s_base == NULL || size == 0 || ((address | size) & (PAGE - 1)) || address >= SPACE || size > SPACE - address)
    return 0;
  *first = (unsigned)(address / PAGE);
  *end = (unsigned)((address + size) / PAGE);
  return 1;
}

int emusuni_vita_mem_commit(uint64_t address, uint64_t size)
{
  unsigned first, end;
  int done = 0;
  pthread_mutex_lock(&s_lock);
  if (range_pages(address, size, &first, &end))
  {
    done = 1;
    for (unsigned page = first; page < end && done; page++)
    {
      const unsigned piece = page / PIECE_PAGES;
      if (s_piece[piece].physical < 0 && !piece_map(piece))
      {
        done = 0;
        break;
      }
      if (!(s_page[page] & IN_USE))
      {
        s_page[page] |= IN_USE;
        s_piece[piece].in_use++;
      }
    }
    if (done)
      done = pages_access(first, end, ACCESS_RW);
  }
  pthread_mutex_unlock(&s_lock);
  return done;
}

void emusuni_vita_mem_decommit(uint64_t address, uint64_t size)
{
  unsigned first, end;
  pthread_mutex_lock(&s_lock);
  if (range_pages(address, size, &first, &end))
  {
    for (unsigned page = first; page < end; page++)
    {
      const unsigned piece = page / PIECE_PAGES;
      if (!(s_page[page] & IN_USE))
        continue;
      s_page[page] &= (uint8_t)~IN_USE;
      if (--s_piece[piece].in_use == 0)
        piece_unmap(piece);
    }
  }
  pthread_mutex_unlock(&s_lock);
}

int emusuni_vita_mem_protect(uint64_t address, uint64_t size, int access)
{
  unsigned first, end;
  int done = 0;
  pthread_mutex_lock(&s_lock);
  if (range_pages(address, size, &first, &end) && (access == ACCESS_NONE || access == ACCESS_READ || access == ACCESS_RW))
    done = pages_access(first, end, access);
  pthread_mutex_unlock(&s_lock);
  return done;
}

/* ================= Page faults ================= */
#if defined(__ORBIS__)
/* The PS4's FreeBSD kernel, whatever a header says (measured on the console: port/ps4_crashlog.cpp, the 3DS's
 * emusuni_fault.h): SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, SA_ONSTACK 1 (the app gives every thread a stack for
 * signals, port/ps4_runtime.cpp), si_addr 24 bytes into the siginfo, the registers 0x40 bytes into the context
 * in mcontext order: the page fault's error code is the 20th 8-byte slot (mc_err, before mc_rip). The struct
 * sigaction handed to the kernel is FreeBSD's (the libc overlay's headers come first). */
#define F_SIGSEGV 11
#define F_SIGBUS 10
#define F_SA_FLAGS (0x40 | 0x0001)
#define F_SA_SIGINFO 0x40
_Static_assert(offsetof(struct sigaction, sa_flags) == 8 && sizeof(sigset_t) == 16,
               "struct sigaction isn't FreeBSD's: the libc overlay's headers must come first");
static void *fault_address(const siginfo_t *info)
{
  return *(void *const *)((const char *)info + 24);
}
static uint64_t fault_error(void *context)
{
  return ((const uint64_t *)((const char *)context + 0x40))[19];
}
#else
#define F_SIGSEGV SIGSEGV
#define F_SIGBUS SIGBUS
#define F_SA_FLAGS SA_SIGINFO
#define F_SA_SIGINFO SA_SIGINFO
static void *fault_address(const siginfo_t *info)
{
  return info->si_addr;
}
static uint64_t fault_error(void *context)
{
  return (uint64_t)((ucontext_t *)context)->uc_mcontext.gregs[REG_ERR];
}
#endif

static emusuni_vita_fault_fn s_fault_handler;
static struct sigaction s_before_segv, s_before_bus;
static int s_fault_installed;
/* (17-11, for the log) Faults the guest memory's handler took, and those of them it was given first. */
static unsigned long long s_faults_handled, s_faults_first;

void emusuni_vita_fault_counts(unsigned long long *handled, unsigned long long *first)
{
  *handled = __atomic_load_n(&s_faults_handled, __ATOMIC_RELAXED);
  *first = __atomic_load_n(&s_faults_first, __ATOMIC_RELAXED);
}

/* The code memory's part in a fault (below). */
static volatile int s_jit_probing; /* a thread is finding out whether code runs in a mapping */
static pthread_t s_jit_prober;
static sigjmp_buf s_jit_probe_failed;
static int jit_fault(void *address);

static void on_fault(int sig, siginfo_t *info, void *context)
{
  if (s_jit_probing && pthread_equal(pthread_self(), s_jit_prober))
    siglongjmp(s_jit_probe_failed, 1);
  /* A first write to a piece of a recompiler's block: its memory comes now. */
  if (jit_fault(fault_address(info)))
    return;
  /* (x86's page fault error code: bit 1 a write, bit 4 an instruction fetch.) */
  const uint64_t error = fault_error(context);
  const emusuni_vita_fault_fn handler = s_fault_handler;
  if (!(error & 0x10) && handler != NULL && handler(fault_address(info), (error & 2) != 0))
  {
    __atomic_fetch_add(&s_faults_handled, 1, __ATOMIC_RELAXED);
    return;
  }
  /* Not the guest's memory, or nothing to do about it: the handler there was before (the app's crash log). */
  const struct sigaction *before = sig == F_SIGBUS ? &s_before_bus : &s_before_segv;
  if (before->sa_flags & F_SA_SIGINFO)
  {
    before->sa_sigaction(sig, info, context);
    return;
  }
  /* (No handler with the fault's details before: put back as it was, the access faults again under it.) */
  sigaction(sig, before, NULL);
}

/* The handler in place (the guest memory's part of it is set apart: the code memory needs it too). */
static void fault_handlers_in(void)
{
  if (s_fault_installed)
    return;
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = on_fault;
  action.sa_flags = F_SA_FLAGS;
  sigemptyset(&action.sa_mask);
  const int segv = sigaction(F_SIGSEGV, &action, &s_before_segv);
  const int bus = sigaction(F_SIGBUS, &action, &s_before_bus);
  if (segv != 0 || bus != 0)
    emusuni_vita_log("vita: the page fault handler wasn't taken (SIGSEGV %d, SIGBUS %d, errno %d)", segv, bus, errno);
  s_fault_installed = 1;
}

void emusuni_vita_fault_install(emusuni_vita_fault_fn handler)
{
  s_fault_handler = handler;
  fault_handlers_in();
}

/* (17-11) Is this file's handler the one the system calls first? The recompiler's own asks as each recompiler
 * is made: it belongs over this one, and emusuni_vita_fault_uninstall takes both out. */
int emusuni_vita_fault_on_top(void)
{
  struct sigaction now;
  if (!s_fault_installed || sigaction(F_SIGSEGV, NULL, &now) != 0)
    return 0;
  return (now.sa_flags & F_SA_SIGINFO) && now.sa_sigaction == on_fault;
}

void emusuni_vita_fault_uninstall(void)
{
  if (s_fault_installed)
  {
    sigaction(F_SIGSEGV, &s_before_segv, NULL);
    sigaction(F_SIGBUS, &s_before_bus, NULL);
  }
  s_fault_installed = 0;
  s_fault_handler = NULL;
}

/* ================= The recompiler's code memory ================= */
/* Vita3K makes a recompiler for each of the guest's threads (43 at once in the first game tried), each with
 * a block of code memory of its own: the code that calls back for every kind of memory access (1.4 MiB,
 * written when the recompiler is made), then the guest's code as it is recompiled. Upstream asks for 128 MiB
 * of addresses each and the system puts memory under what gets written. On the PS4 a mapping is memory at
 * once, so here:
 *   - one reservation of 4 GiB of addresses holds every block (nothing mapped: it costs no memory);
 *   - a block is a part of it; its memory comes in pieces of 256 KiB, each when it is first written (the
 *     write faults, the fault handler above maps the piece, the write goes on);
 *   - a block that ends gives its pieces back.
 * The memory is system flexible memory, readable, writable and executable. The first piece of all says how
 * the console lets that be made: mapped so at once, or mapped readable and writable and then changed with
 * sceKernelMprotect (the two ways port/ps4_n64.c's emusuni_map_executable knows), each checked by running
 * code there. */
#define JIT_SPACE ((uint64_t)4 << 30)
#define JIT_PIECE ((size_t)256 << 10)
#define JIT_BLOCKS 256
#define JIT_BLOCK_MOST ((size_t)128 << 20)
#define JIT_PIECES_MOST ((unsigned)(JIT_BLOCK_MOST / JIT_PIECE))
/* (01.34) With blocks of 64 MiB: all the blocks' memory together is kept at this, and what a block may always
 * have (what every block had before: see emusuni_vita_jit_tight). */
#define JIT_BUDGET_PIECES ((unsigned)(((size_t)112 << 20) / JIT_PIECE))
#define JIT_FIRST_PIECES ((unsigned)(((size_t)16 << 20) / JIT_PIECE))
#define JIT_BIG_BLOCKS 40

struct JitBlock
{
  uint8_t *base; /* NULL: free entry */
  size_t size;
  unsigned pieces;                     /* mapped now */
  unsigned again;                      /* faults in a row at pieces already mapped */
  uint8_t mapped[JIT_PIECES_MOST / 8]; /* a bit per piece */
};

static pthread_mutex_t s_jit_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_jit_space;
static struct JitBlock s_jit_block[JIT_BLOCKS];
static int s_jit_way; /* 0: not known yet, 1: executable at once, 2: read/write then mprotect, -1: neither */
static unsigned s_jit_blocks_now, s_jit_blocks_most, s_jit_pieces_now, s_jit_pieces_most, s_jit_blocks_made;
static int s_jit_tight, s_jit_tight_said; /* the memory of all the blocks is at JIT_BUDGET_PIECES; said in the log */
static int s_switch_looked;               /* the speed switches' files were read for this game (further down) */

static size_t jit_piece_size(const struct JitBlock *b, unsigned piece)
{
  const size_t left = b->size - piece * JIT_PIECE;
  return left < JIT_PIECE ? left : JIT_PIECE;
}

/* Memory under a piece, the way `way`. */
static int32_t jit_map(uint8_t *at, size_t length, int way)
{
  int32_t r = k_map_code(at, length, way == 1);
  if (r == 0 && way == 2)
  {
    r = k_protect(at, length, 7);
    if (r != 0)
      unmap_to_reserved(at, length);
  }
  return r;
}

/* Runs `mov eax, 0x4E36; ret` at code: 1 when it came back with that value (a fault on the way comes back
 * here through the fault handler). */
static int jit_runs(uint8_t *code)
{
  static const unsigned char TEST[] = {0xB8, 0x36, 0x4E, 0x00, 0x00, 0xC3};
  volatile int result = 0;
  fault_handlers_in();
  s_jit_prober = pthread_self();
  if (sigsetjmp(s_jit_probe_failed, 1) == 0)
  {
    s_jit_probing = 1;
    memcpy(code, TEST, sizeof(TEST));
    result = ((int (*)(void))code)();
    s_jit_probing = 0;
    memset(code, 0, sizeof(TEST));
  }
  s_jit_probing = 0;
  return result == 0x4E36;
}

/* The first block's first piece: which way gives memory that code runs in. */
static int jit_find_way(struct JitBlock *b)
{
  static const char *const WAYS[] = {"", "readable, writable and executable at once",
                                     "readable and writable, then made executable (mprotect)"};
  const size_t length = jit_piece_size(b, 0);
  for (int way = 1; way <= 2; way++)
  {
    const int32_t r = jit_map(b->base, length, way);
    if (r != 0)
    {
      emusuni_vita_log("vita: jit: system flexible memory %s: refused (%#x)", WAYS[way], (unsigned)r);
      continue;
    }
    if (jit_runs(b->base))
    {
      emusuni_vita_log("vita: jit: code memory is system flexible memory mapped %s, in pieces of %zu KiB as it is "
                       "written; its addresses are reserved at %p",
                       WAYS[way], JIT_PIECE >> 10, (void *)s_jit_space);
      s_jit_way = way;
      b->mapped[0] |= 1;
      b->pieces = 1;
      if (++s_jit_pieces_now > s_jit_pieces_most)
        s_jit_pieces_most = s_jit_pieces_now;
      return 1;
    }
    emusuni_vita_log("vita: jit: system flexible memory %s: it maps, but code there faults", WAYS[way]);
    unmap_to_reserved(b->base, length);
  }
  emusuni_vita_log("vita: jit: no way to memory that code runs in: the game can't start");
  s_jit_way = -1;
  return 0;
}

/* The lowest free run of `size` bytes of the reservation, NULL when there is none. */
static uint8_t *jit_find(size_t size)
{
  uint8_t *at = s_jit_space;
  uint8_t *const end = at + JIT_SPACE;
  for (;;)
  {
    if ((uint64_t)(end - at) < size)
      return NULL;
    uint8_t *next = NULL;
    for (unsigned i = 0; i < JIT_BLOCKS; i++)
    {
      const struct JitBlock *b = &s_jit_block[i];
      if (b->base != NULL && b->base < at + size && b->base + b->size > at && (next == NULL || b->base + b->size > next))
        next = b->base + b->size;
    }
    if (next == NULL)
      return at;
    at = next;
  }
}

static int jit_reserve(void)
{
  /* (Away from the guest's memory's hints above, and from the kernel's own low addresses.) */
  static const uintptr_t HINTS[] = {0x1000000000ull, 0x1400000000ull, 0x1800000000ull, 0x6000000000ull,
                                    0x3800000000ull, 0x800000000ull};
  int32_t r = -1;
  for (unsigned i = 0; i < sizeof(HINTS) / sizeof(HINTS[0]); i++)
  {
    void *at = (void *)HINTS[i];
    r = k_reserve(&at, JIT_SPACE, 0, PAGE);
    if (r != 0)
      continue;
    if ((uintptr_t)at < 0x400000000ull)
    {
      k_munmap(at, JIT_SPACE);
      r = -2;
      continue;
    }
    s_jit_space = (uint8_t *)at;
    return 1;
  }
  emusuni_vita_log("vita: jit: no 4 GiB of addresses to reserve for the code memory (%#x)", (unsigned)r);
  return 0;
}

void *emusuni_vita_jit_alloc(size_t size)
{
  uint8_t *block = NULL;
  size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
  if (size == 0 || size > JIT_BLOCK_MOST)
    return NULL;
  pthread_mutex_lock(&s_jit_lock);
  if (s_jit_way < 0 || (s_jit_space == NULL && !jit_reserve()))
    goto done;
  unsigned entry = 0;
  while (entry < JIT_BLOCKS && s_jit_block[entry].base != NULL)
    entry++;
  if (entry == JIT_BLOCKS || (block = jit_find(size)) == NULL)
  {
    emusuni_vita_log("vita: jit: no room for one more block of code memory (%u in use)", s_jit_blocks_now);
    block = NULL;
    goto done;
  }
  struct JitBlock *b = &s_jit_block[entry];
  memset(b, 0, sizeof(*b));
  b->base = block;
  b->size = size;
  if (s_jit_way == 0 && !jit_find_way(b))
  {
    b->base = NULL;
    block = NULL;
    goto done;
  }
  /* (The handler that maps the pieces: in place even if the guest's memory hasn't asked for it.) */
  fault_handlers_in();
  s_jit_blocks_made++;
  if (++s_jit_blocks_now > s_jit_blocks_most)
    s_jit_blocks_most = s_jit_blocks_now;
done:
  pthread_mutex_unlock(&s_jit_lock);
  return block;
}

/* A fault at `address`: 1 when it was the first access to a piece of a block, which has its memory now. */
static int jit_fault(void *address)
{
  uint8_t *const at = (uint8_t *)address;
  int done = 0;
  unsigned grown_to = 0, grown_blocks = 0;
  /* (Read without the lock: a fault anywhere else must not wait for it.) */
  uint8_t *const space = s_jit_space;
  if (space == NULL || at < space || at >= space + JIT_SPACE)
    return 0;
  pthread_mutex_lock(&s_jit_lock);
  for (unsigned i = 0; i < JIT_BLOCKS; i++)
  {
    struct JitBlock *b = &s_jit_block[i];
    if (b->base == NULL || at < b->base || at >= b->base + b->size)
      continue;
    const unsigned piece = (unsigned)((size_t)(at - b->base) / JIT_PIECE);
    if (b->mapped[piece / 8] & (1u << (piece % 8)))
    {
      /* Mapped already: another thread got here first, and the access goes through now. Unless it keeps
       * faulting: then it isn't the memory that is missing, and the fault is for the handlers before. */
      done = ++b->again <= 64;
      break;
    }
    const size_t length = jit_piece_size(b, piece);
    const int32_t r = jit_map(b->base + piece * JIT_PIECE, length, s_jit_way);
    if (r != 0)
    {
      emusuni_vita_log("vita: jit: no memory for one more piece of code memory (%#x; %u KiB mapped in %u blocks)",
                       (unsigned)r, (unsigned)(s_jit_pieces_now * (JIT_PIECE >> 10)), s_jit_blocks_now);
      break;
    }
    b->mapped[piece / 8] |= (uint8_t)(1u << (piece % 8));
    b->pieces++;
    b->again = 0;
    if (s_jit_pieces_now + 1 >= JIT_BUDGET_PIECES)
      __atomic_store_n(&s_jit_tight, 1, __ATOMIC_RELAXED);
    if (++s_jit_pieces_now > s_jit_pieces_most)
    {
      s_jit_pieces_most = s_jit_pieces_now;
      /* (A line in the log for every 16 MiB more than ever before.) */
      if (s_jit_pieces_most % (unsigned)(((size_t)16 << 20) / JIT_PIECE) == 0)
      {
        grown_to = s_jit_pieces_most;
        grown_blocks = s_jit_blocks_now;
      }
    }
    done = 1;
    break;
  }
  pthread_mutex_unlock(&s_jit_lock);
  if (grown_to != 0)
    emusuni_vita_log("vita: jit: %u MiB of code memory now, in %u blocks", (unsigned)(grown_to * JIT_PIECE >> 20),
                     grown_blocks);
  return done;
}

void emusuni_vita_jit_free(void *block)
{
  if (block == NULL)
    return;
  pthread_mutex_lock(&s_jit_lock);
  for (unsigned i = 0; i < JIT_BLOCKS; i++)
  {
    struct JitBlock *b = &s_jit_block[i];
    if (b->base != (uint8_t *)block)
      continue;
#if !defined(__ORBIS__)
    if (getenv("EMUSUNI_VITA_JIT_STATS") != NULL)
      emusuni_vita_log("vita: jit: a block ends with %zu KiB mapped", b->pieces * (JIT_PIECE >> 10));
#endif
    /* Its pieces back, in runs. */
    const unsigned pieces = (unsigned)((b->size + JIT_PIECE - 1) / JIT_PIECE);
    for (unsigned piece = 0; piece < pieces;)
    {
      if (!(b->mapped[piece / 8] & (1u << (piece % 8))))
      {
        piece++;
        continue;
      }
      unsigned run = 1;
      while (piece + run < pieces && (b->mapped[(piece + run) / 8] & (1u << ((piece + run) % 8))))
        run++;
      size_t length = run * JIT_PIECE;
      if (piece * JIT_PIECE + length > b->size)
        length = b->size - piece * JIT_PIECE;
      unmap_to_reserved(b->base + piece * JIT_PIECE, length);
      piece += run;
    }
    s_jit_pieces_now -= b->pieces;
    s_jit_blocks_now--;
    b->base = NULL;
    break;
  }
  pthread_mutex_unlock(&s_jit_lock);
}

int emusuni_vita_jit_ready(void)
{
  /* (A block of one piece: the first one of all finds the way, or says in the log that there is none.) */
  pthread_mutex_lock(&s_jit_lock);
  if (s_jit_way < 0)
    s_jit_way = 0; /* (refused before: a new game asks again) */
  pthread_mutex_unlock(&s_jit_lock);
  void *block = emusuni_vita_jit_alloc(JIT_PIECE);
  if (block == NULL)
    return 0;
  emusuni_vita_jit_free(block);
  return 1;
}

void emusuni_vita_jit_usage(unsigned *now_kib, unsigned *most_kib, unsigned *blocks_now)
{
  pthread_mutex_lock(&s_jit_lock);
  *now_kib = (unsigned)(s_jit_pieces_now * (JIT_PIECE >> 10));
  *most_kib = (unsigned)(s_jit_pieces_most * (JIT_PIECE >> 10));
  *blocks_now = s_jit_blocks_now;
  pthread_mutex_unlock(&s_jit_lock);
}

/* The game's end: what its recompilers took, in the log; the reservation back when no block is left. */
static void jit_trim(void)
{
  pthread_mutex_lock(&s_jit_lock);
  if (s_jit_blocks_made != 0)
    emusuni_vita_log("vita: jit: %u blocks made, %u at once at most; %.1f MiB of code memory at most; %u blocks and "
                     "%.1f MiB left at the end",
                     s_jit_blocks_made, s_jit_blocks_most, s_jit_pieces_most * (JIT_PIECE / 1048576.0), s_jit_blocks_now,
                     s_jit_pieces_now * (JIT_PIECE / 1048576.0));
  if (s_jit_space != NULL && s_jit_blocks_now == 0)
  {
    k_munmap(s_jit_space, JIT_SPACE);
    s_jit_space = NULL;
  }
  if (s_jit_way < 0)
    s_jit_way = 0; /* (the next game tries again) */
  s_jit_blocks_made = 0;
  s_jit_blocks_most = s_jit_blocks_now;
  s_jit_pieces_most = s_jit_pieces_now;
  s_jit_tight_said = 0;
  pthread_mutex_unlock(&s_jit_lock);
  /* (The next game looks at the switches' files again.) */
  __atomic_store_n(&s_switch_looked, 0, __ATOMIC_RELEASE);
}

/* ================= (01.34) The speed switches ================= */
/* (The frame queue was OFF until a game that asks for the queue of one image it is about could be tried. Since
 * 17-11 it is ON: Mortal Kombat is such a game, its fights come out the same on the PC with it, and 01.45's
 * console log has them at three refreshes of the screen a picture - port/ps4_vita_sys.h.) */
#define EMUSUNI_VITA_FRAMEQUEUE_DEFAULT 1
/* (The game's FP state: OFF until measured.) */
#define EMUSUNI_VITA_MXCSR_DEFAULT 0
static const struct
{
  const char *name;
  const char *said;
  int on;
} SWITCHES[EMUSUNI_VITA_SWITCHES] = {
    [EMUSUNI_VITA_FASTFP] = {"fastfp", "fast floating point", 1},
    [EMUSUNI_VITA_BIGJIT] = {"bigjit", "64 MiB of code memory a thread", 1},
    [EMUSUNI_VITA_FRAMEQUEUE] = {"framequeue", "frame queue", EMUSUNI_VITA_FRAMEQUEUE_DEFAULT},
    /* (ON since the set 0174. The owner tried 01.71 with the file: Street Fighter X Tekken's fights went from
     * 477-480 to 558-574 pictures in 10 s on the console. The set 0173's bench pass with it ON stopped Uncharted
     * for good after 161 pictures - its threads asleep in their waits -: "monitor64" below is what lets it play.) */
    [EMUSUNI_VITA_FASTATOMICS] = {"fastatomics", "fast atomics", 1},
    [EMUSUNI_VITA_HINTS] = {"hints", "hints without a call", 1},
    [EMUSUNI_VITA_FILEWALK] = {"filewalk", "file names folder by folder", 1},
    [EMUSUNI_VITA_COPYBARRIER] = {"copybarrier", "barriers at a surface's copy", 1},
    [EMUSUNI_VITA_MXCSR] = {"mxcsr", "the game's FP state kept across calls", EMUSUNI_VITA_MXCSR_DEFAULT},
    [EMUSUNI_VITA_TEXSYNC] = {"texsync", "textures looked at again at each draw", 0},
    [EMUSUNI_VITA_ACCURACY] = {"accuracy", "the renderer's high accuracy", 0},
    [EMUSUNI_VITA_SYNCSHADERS] = {"syncshaders", "pipelines compiled before their draw", 0},
    [EMUSUNI_VITA_GPUBARRIER] = {"gpubarrier", "a barrier for everything before each scene", 0},
    [EMUSUNI_VITA_CENTERTOUCH] = {"centertouch", "R3 as a finger on the middle of the screen", 1},
    /* (ON: with the recompiler's handler back in every game - the script says why -, OFF would have every game lose
     * its fast memory to those faults, as only the first of a session did. A profile of Mortal Kombat on the PC, a
     * first game: 42% of its threads' time in memory accesses through calls.) */
    [EMUSUNI_VITA_KEEPFASTMEM] = {"keepfastmem", "a protected page's fault doesn't cost the instruction its fast memory", 1},
    /* (OFF in the first set that has them, 01.65: written and compiled, not yet measured one against the other.) */
    [EMUSUNI_VITA_NATIVELIB] = {"nativelib", "the library's long routines done by the PS4 itself", 0},
    [EMUSUNI_VITA_FASTCALLS] = {"fastcalls", "the simplest system calls answered inside the recompiler", 0},
    [EMUSUNI_VITA_CPUOPT] = {"cpuopt", "the recompiler's optimizations and fast memory", 1},
    [EMUSUNI_VITA_NGSDEFER] = {"ngsdefer", "a keyed-off voice's callback runs at the next sound update, not inside the key-off", 1},
    [EMUSUNI_VITA_OFFLINE] = {"offline", "the game is told the console has no network (it has none here)", 0},
    [EMUSUNI_VITA_MIC] = {"mic", "the microphone hears silence at its real pace and isn't muted", 1},
    [EMUSUNI_VITA_DRAWCOPY] = {"drawcopy", "a draw's uniform buffers, vertices and indices are copied when the game asks for the draw", 0},
    [EMUSUNI_VITA_FRESHSHADERS] = {"freshshaders", "the shaders and the pipeline cache an earlier start saved are not read: all made again", 0},
    [EMUSUNI_VITA_VERTEXW] = {"vertexw", "a vertex shader for each layout of its vertex data, where the GPU reads 4 components for 3", 1},
    [EMUSUNI_VITA_VERTEXALIGN] = {"vertexalign", "vertices an odd number of bytes apart are padded to a multiple of 4 for the GPU", 1},
    [EMUSUNI_VITA_CONDRELEASE] = {"condrelease", "a wait for a condition gives up every lock the thread has on the condition's mutex", 1},
    [EMUSUNI_VITA_ZIPMARKS] = {"zipmarks", "a compressed file of a game's ZIP is read at any place without inflating it from its start", 1},
    /* (ON in the set it comes in, 0172, for the one version that looks at Street Fighter X Tekken's black skin on the
     * console: OFF again after it.) */
    [EMUSUNI_VITA_UNIFORMLOG] = {"uniformlog", "the numbers a game gives its shader programs go into the log (its first 20 minutes, 2400 lines)", 0},
    [EMUSUNI_VITA_VERTEXRELAY] = {"vertexrelay", "vertices with numbers at odd places inside them are laid out anew for the GPU", 1},
    /* (OFF here: switches_look() turns it ON for the one game that needs it, God of War Collection.) */
    [EMUSUNI_VITA_CORETURNS] = {"coreturns", "the threads a game ties to one of the Vita's cores run one at a time", 0},
    [EMUSUNI_VITA_MONITOR64] = {"monitor64", "the 64-bit exclusive stores through the monitor with fast atomics too", 1},
    [EMUSUNI_VITA_LWFAST] = {"lwfast", "lightweight mutexes answered inside the recompiler", 0},
    [EMUSUNI_VITA_QUEUELIMITS] = {"queuelimits", "the queue of images as long as the game asks for", 1},
    [EMUSUNI_VITA_NEGP2] = {"negp2", "shader instructions on not p2 keep their condition", 1},
    [EMUSUNI_VITA_LOOPCAP] = {"loopcap", "a shader's loop ends after 4096 turns", 1},
    [EMUSUNI_VITA_INTCLAMP] = {"intclamp", "numbers held in range before they are made small integers", 1},
    [EMUSUNI_VITA_FASTGXM] = {"fastgxm", "the graphics library's setters answered inside the recompiler", 0},
    [EMUSUNI_VITA_NGSRESAMPLE] = {"ngsresample", "the sound's voices through the port's own resampler", 0},
    [EMUSUNI_VITA_RES2X] = {"res2x", "the picture made at twice the PS Vita's size", 0},
    /* (ON: on the bench LittleBigPlanet gets past its "Connecting..." screen with it, the one game of the six that
     * binds a socket; nothing is bound - local/herramientas/prueba-vita/nonettest checks the socket has no port.) */
    [EMUSUNI_VITA_NETBIND] = {"netbind", "a socket's bind answered done, nothing bound", 1},
    [EMUSUNI_VITA_FASTIDS] = {"fastids", "a thread's id and TLS address answered from the thread's own", 0},
    /* (OFF: none of the bench's six games opens a message dialog, so the answering has not been seen at work.) */
    [EMUSUNI_VITA_MSGAUTO] = {"msgauto", "the system's message dialog answers itself", 0},
    [EMUSUNI_VITA_SURFACEPICK] = {"surfacepick", "a texture on painted surfaces that share memory: the one laid out as it", 1},
    [EMUSUNI_VITA_GXMFAST] = {"gxmfast", "the GPU library's getters and uniform setter answered inside the recompiler", 0},
    [EMUSUNI_VITA_RAWHALVES] = {"rawhalves", "64 bits of raw output on a surface of four half floats written as they are", 1},
    [EMUSUNI_VITA_MOVPCLR] = {"movpclr", "MOV PC, LR (the end of every import stub) ends its block as a return", 0},
    [EMUSUNI_VITA_NZCVWORD] = {"nzcvword", "the guest's N, Z (and C) flags written as the whole flags word", 0},
    [EMUSUNI_VITA_VFP64] = {"vfp64", "a double register loaded or stored with one 64-bit access", 0},
    [EMUSUNI_VITA_GAMEFN] = {"gamefn", "a game's hot function done natively (Uncharted's tree search)", 0},
    [EMUSUNI_VITA_GXMRING] = {"gxmring", "the GPU library's commands from a ring of the emulator's own, own arenas", 0},
    [EMUSUNI_VITA_GXMRINGCOUNT] = {"gxmringcount", "the GPU library's commands counted: game's ring or new", 0},
    [EMUSUNI_VITA_GXMPACKET] = {"gxmpacket", "a draw's commands to the renderer as one packet, its highest index found there", 0},
    [EMUSUNI_VITA_STUBCALL] = {"stubcall", "an import stub made into one direct call of its function when translated", 0},
    [EMUSUNI_VITA_CALLSTATS] = {"callstats", "the time of the calls made so measured on one call in 16", 0},
    [EMUSUNI_VITA_IOLOCK] = {"iolock", "the open files' tables behind a lock while they change (threads opening files at once)", 1},
    [EMUSUNI_VITA_UNIFORMCOPY] = {"uniformcopy", "a draw's default uniform buffer copied as it is asked for (GXM's ring reuses it)", 0},
    [EMUSUNI_VITA_STUBGXM] = {"stubgxm", "with stubcall, 97 more GPU setters and getters answered inside", 0},
    [EMUSUNI_VITA_SIXCORES] = {"sixcores", "the core's threads on the PS4's cores 0-5, never the seventh (the system's too)", 0},
    [EMUSUNI_VITA_TEXBATCH] = {"texbatch", "a hashed texture looked at once a command list, not at each bind", 0},
    [EMUSUNI_VITA_VERTEXRANGE] = {"vertexrange", "a draw's vertices copied from its lowest index, not from vertex 0", 0},
    [EMUSUNI_VITA_TEXVIEWPORT] = {"texviewport", "texture viewports: a part of a painted surface sampled in place, not copied", 0},
    [EMUSUNI_VITA_SUBMITTHREAD] = {"submitthread", "the core's submissions to the GPU made on a thread of their own, in order", 0},
    [EMUSUNI_VITA_PRESENTNOWAIT] = {"presentnowait", "a picture's copy for the frontend not waited for on the CPU", 0},
    [EMUSUNI_VITA_SURFACEWATCH] = {"surfacewatch", "a surface made again replaces its own memory watch, not piles another (fix)", 1},
};
/* (The game being loaded: emusuni_vita_game().) */
static char s_game_title[16];
static pthread_mutex_t s_switch_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_switch[EMUSUNI_VITA_SWITCHES]; /* (read without the lock once s_switch_looked is 1: set last) */

/* (17-11) The test's round: its file is looked for with the switches, once a game. */
static int s_test_cycle;
static unsigned s_test_refreshes;

static void switches_look(void)
{
  /* (By their names, which are the files' too. A line of the log holds 511 characters: since the set 0173 the
   * switches that are ON have a line of their own - there were too many for one with the rest. (20-11) 320 bytes a
   * list left out the last names: 01.83's console log named neither iolock nor the bridge's among the ON ones.) */
  char said[4][488] = {"", "", "", ""}; /* on, off, changed by a file, set for this game only */
  size_t used[4] = {0, 0, 0, 0};
  pthread_mutex_lock(&s_switch_lock);
  if (__atomic_load_n(&s_switch_looked, __ATOMIC_ACQUIRE))
  {
    pthread_mutex_unlock(&s_switch_lock);
    return;
  }
  for (int i = 0; i < EMUSUNI_VITA_SWITCHES; i++)
  {
    /* (what the switch is without a file: the table's, or the game's own) */
    int usual = SWITCHES[i].on;
    char path[96];
    if (i == EMUSUNI_VITA_CORETURNS && strcmp(s_game_title, "PCSA00126") == 0)
      usual = 1;
    /* (01.81) MotorStorm RC gets ahead of the renderer with the longer queue: ps4_vita_sys.h, queuelimits. */
    if (i == EMUSUNI_VITA_QUEUELIMITS && strcmp(s_game_title, "PCSC00014") == 0)
      usual = 0;
    int on = usual;
#if defined(__ORBIS__)
    snprintf(path, sizeof(path), "/data/EmuSuNi/vita-%s-on", SWITCHES[i].name);
    if (access(path, F_OK) == 0)
      on = 1;
    snprintf(path, sizeof(path), "/data/EmuSuNi/vita-%s-off", SWITCHES[i].name);
    if (access(path, F_OK) == 0)
      on = 0;
#else
    snprintf(path, sizeof(path), "EMUSUNI_VITA_%s", SWITCHES[i].name);
    for (char *c = path; *c != '\0'; c++)
      if (*c >= 'a' && *c <= 'z')
        *c = (char)(*c - 'a' + 'A');
    const char *text = getenv(path);
    if (text != NULL && (text[0] == '0' || text[0] == '1'))
      on = text[0] == '1';
#endif
    s_switch[i] = on;
    for (int list = 0; list < 4; list++)
    {
      if (list == 0 ? !on : list == 1 ? on : list == 2 ? on == usual : !(on == usual && usual != SWITCHES[i].on))
        continue;
      const int n = snprintf(said[list] + used[list], sizeof(said[list]) - used[list], "%s%s%s", used[list] ? " " : "",
                             SWITCHES[i].name, list == 3 ? (on ? " ON" : " OFF") : "");
      if (n > 0 && (size_t)n < sizeof(said[list]) - used[list])
        used[list] += (size_t)n;
    }
  }
#if defined(__ORBIS__)
  s_test_cycle = access("/data/EmuSuNi/vita-test-cycle", F_OK) == 0;
#else
  s_test_cycle = getenv("EMUSUNI_VITA_TEST_CYCLE") != NULL;
#endif
  s_test_refreshes = 0;
  __atomic_store_n(&s_switch_looked, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&s_switch_lock);
  emusuni_vita_log("vita: switches: ON: %s", used[0] ? said[0] : "none");
  emusuni_vita_log("vita: switches: OFF: %s; changed by a file: %s; set for this game only (%s): %s", used[1] ? said[1] : "none",
                   used[2] ? said[2] : "none", s_game_title[0] != '\0' ? s_game_title : "no game yet", used[3] ? said[3] : "none");
  emusuni_vita_log("vita: switches: an empty file /data/EmuSuNi/vita-<name>-on or vita-<name>-off changes one at the "
                   "next game (port/ps4_vita_sys.h says what each one is)");
}

void emusuni_vita_game(const char *title)
{
  pthread_mutex_lock(&s_switch_lock);
  snprintf(s_game_title, sizeof(s_game_title), "%s", title != NULL ? title : "");
  /* (looked at again, now that the game is known) */
  __atomic_store_n(&s_switch_looked, 0, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&s_switch_lock);
}

int emusuni_vita_switch(int which)
{
  if (which < 0 || which >= EMUSUNI_VITA_SWITCHES)
    return 0;
  if (!__atomic_load_n(&s_switch_looked, __ATOMIC_ACQUIRE))
    switches_look();
  return s_switch[which];
}

/* (17-11) The test's round (port/ps4_vita_sys.h). */
const char *emusuni_vita_test_tick(void)
{
  static const struct
  {
    int which;
    const char *words;
  } STEPS[] = {
      {-1, "TEST 0: none of the test switches"},
      {EMUSUNI_VITA_GPUBARRIER, "TEST 1: gpubarrier"},
      {EMUSUNI_VITA_TEXSYNC, "TEST 2: texsync"},
      {EMUSUNI_VITA_SYNCSHADERS, "TEST 3: syncshaders"},
      {EMUSUNI_VITA_DRAWCOPY, "TEST 4: drawcopy"},
  };
  if (!__atomic_load_n(&s_switch_looked, __ATOMIC_ACQUIRE) || !s_test_cycle)
    return NULL;
  const unsigned at = s_test_refreshes++;
  if (at % 1200 != 0)
    return NULL;
  const unsigned step = (at / 1200) % (sizeof(STEPS) / sizeof(STEPS[0]));
  for (unsigned i = 1; i < sizeof(STEPS) / sizeof(STEPS[0]); i++)
    s_switch[STEPS[i].which] = i == step;
  emusuni_vita_log("vita: test: %s, for the next 20 seconds (the file vita-test-cycle)", STEPS[step].words);
  return STEPS[step].words;
}

/* (17-11) Called by the recompiler's fault handler, in the signal: nothing here but reads, and the guest
 * memory's handler, which this file's own handler calls in a signal too. (The switches were looked at when the
 * first recompiler was made - before any recompiled code ran; were they not, the answer is no.) */
int emusuni_vita_fault_first(void *info, void *context)
{
  if (!__atomic_load_n(&s_switch_looked, __ATOMIC_ACQUIRE) || !s_switch[EMUSUNI_VITA_KEEPFASTMEM])
    return 0;
  const emusuni_vita_fault_fn handler = s_fault_handler;
  uint8_t *const base = s_base;
  uint8_t *const at = (uint8_t *)fault_address((const siginfo_t *)info);
  if (handler == NULL || base == NULL || at < base || at >= base + SPACE)
    return 0;
  const uint64_t error = fault_error(context);
  if ((error & 0x10) || !handler(at, (error & 2) != 0))
    return 0;
  __atomic_fetch_add(&s_faults_handled, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&s_faults_first, 1, __ATOMIC_RELAXED);
  return 1;
}

int emusuni_vita_jit_tight(const void *at)
{
  /* (Asked before every piece of code the recompilers make: no lock while there is room.) */
  if (!__atomic_load_n(&s_jit_tight, __ATOMIC_RELAXED))
    return 0;
  const uint8_t *const p = (const uint8_t *)at;
  int tight = 0, said = 0;
  pthread_mutex_lock(&s_jit_lock);
  if (s_jit_pieces_now >= JIT_BUDGET_PIECES)
  {
    for (unsigned i = 0; i < JIT_BLOCKS; i++)
    {
      const struct JitBlock *b = &s_jit_block[i];
      if (b->base == NULL || p < b->base || p >= b->base + b->size)
        continue;
      /* (The megabyte ahead of it is what the recompiler asks to have before it writes a piece of code.) */
      const size_t ahead = (size_t)(p - b->base) + ((size_t)1 << 20);
      const unsigned piece = (unsigned)(ahead / JIT_PIECE);
      if (piece >= JIT_FIRST_PIECES && ahead < b->size && !(b->mapped[piece / 8] & (1u << (piece % 8))))
      {
        tight = 1;
        said = !s_jit_tight_said;
        s_jit_tight_said = 1;
      }
      break;
    }
  }
  else
    __atomic_store_n(&s_jit_tight, 0, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&s_jit_lock);
  if (said)
    emusuni_vita_log("vita: jit: the code memory is at its %u MiB: a thread past its first 16 MiB starts over "
                     "instead of taking more",
                     (unsigned)(JIT_BUDGET_PIECES * JIT_PIECE >> 20));
  return tight;
}

size_t emusuni_vita_jit_cache_size(void)
{
  long mib = 0;
#if defined(__ORBIS__)
  FILE *f = fopen("/data/EmuSuNi/vita-jit-mib", "r");
  if (f != NULL)
  {
    if (fscanf(f, "%ld", &mib) != 1)
      mib = 0;
    fclose(f);
  }
#else
  const char *text = getenv("EMUSUNI_VITA_JIT_MIB");
  if (text != NULL)
    mib = atol(text);
#endif
  if (mib < 4 || mib > 128)
  {
    mib = 16;
    if (emusuni_vita_switch(EMUSUNI_VITA_BIGJIT))
    {
      pthread_mutex_lock(&s_jit_lock);
      if (s_jit_blocks_now < JIT_BIG_BLOCKS)
        mib = 64;
      pthread_mutex_unlock(&s_jit_lock);
    }
  }
  return (size_t)mib << 20;
}
