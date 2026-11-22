// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// (01.27) The program's own data pointers set by the program itself (PS4_RELR, toolchain/ps4-love-style.cmake).
//
// Every pointer in the program's data needs the address the program was loaded at added to it: 820 000 of them in
// 01.26, each a 24-byte entry of the dynamic relocations (.rela.dyn), which create-fself copies into the loader's own
// table in the eboot (PT_SCE_DYNLIBDATA): 19.7 MB of the package. After the link (toolchain/relr-link.py) those in
// .data.rel.ro and .data are packed the way of ELF's RELR - an address, then bitmaps of the 63 words after it, 8 bytes
// for up to 64 pointers - into .emusuni_relr (read-only, in the image: ~0.3 MB), each pointer's link-time value
// written in its place. The loader keeps the rest: the GOT, .init_array, the TLS image, the process parameters and
// crt1's libc tables (read before the program starts), the pointers not 8-aligned, those to symbols.
//
// ps4_relr_apply adds the load address to each, once, before anything of the program's reads one: from _start
// (port/ps4_crt1.S, before libc starts), from crt1's malloc-replacement entries (Sony's libc calls them before
// _start, when its heap starts) and as the very first constructor (should the loader run the constructors itself).
// It reads only its table and writes only those words: no data of the program's with a pointer in it, no libc.
// .data.rel.ro is in the loader's RELRO segment, read-only by then: made writable while this runs, its protection
// given back after. Should that fail, /data/EmuSuNi/relr-error.txt says why and the program stops there
// (cmake -DPS4_RELR_RELRO=OFF packs only .data's, -DPS4_RELR=OFF none: as before 01.27).

#include <stddef.h>
#include <stdint.h>

#ifndef PS4_RELR_TEST
int32_t sceKernelMprotect(const void* addr, size_t len, int32_t prot);
int32_t sceKernelQueryMemoryProtection(void* addr, void** start, void** end, uint32_t* prot);
int32_t sceKernelOpen(const char* path, int32_t flags, uint16_t mode);
int64_t sceKernelWrite(int32_t fd, const void* data, size_t size);
int32_t sceKernelClose(int32_t fd);
#else
// The PC's test (toolchain/relr-check.py): the same code over an image in memory.
int32_t ps4_relr_test_mprotect(const void* addr, size_t len, int32_t prot);
int32_t ps4_relr_test_query(void* addr, void** start, void** end, uint32_t* prot);
#define sceKernelMprotect ps4_relr_test_mprotect
#define sceKernelQueryMemoryProtection ps4_relr_test_query
#endif

#define PS4_RELR_MAGIC 0x31524c4552554d45ull  // "EMURELR1"
#define PROT_CPU_READ 0x1
#define PROT_CPU_WRITE 0x2

// Written by toolchain/relr-link.py at the start of .emusuni_relr; the RELR words follow it.
struct Ps4RelrTable
{
  uint64_t magic;
  uint64_t self;           // this header's link-time address: the load address is its address less this
  uint64_t words;          // RELR words after the header
  uint64_t count;          // pointers they stand for (relr-link.py's check)
  uint64_t protect_start;  // link-time pages to make writable while relocating (.data.rel.ro's); equal: none
  uint64_t protect_end;
};

#ifndef PS4_RELR_TEST
extern const struct Ps4RelrTable __emusuni_relr_table __attribute__((visibility("hidden")));
#define RELR_TABLE (&__emusuni_relr_table)
#else
extern const struct Ps4RelrTable* ps4_relr_test_table;
#define RELR_TABLE ps4_relr_test_table
#endif
// .emusuni_relr's input: the header's room (the linker script adds the words' room after it).
__attribute__((section(".emusuni_relr"), used, aligned(16))) static const uint64_t s_header_room[6] = {0};

// 1 once done: crt1's malloc-replacement entries look at it before calling ps4_relr_apply.
__attribute__((visibility("hidden"))) volatile uint8_t ps4_relr_done;
static int s_state;  // 0 not started, 1 relocating, 2 done

#ifndef PS4_RELR_TEST
// The reason in /data/EmuSuNi/relr-error.txt ("<what> 0x<code>"), then the program stops: nothing works unrelocated.
__attribute__((noreturn, no_stack_protector)) static void Fail(const char* what, int32_t code)
{
  char line[64];
  size_t n = 0;
  while (*what && n < 40)
    line[n++] = *what++;
  line[n++] = ' ';
  line[n++] = '0';
  line[n++] = 'x';
  for (int shift = 28; shift >= 0; shift -= 4)
    line[n++] = "0123456789abcdef"[(code >> shift) & 0xf];
  line[n++] = '\n';
  const int32_t fd = sceKernelOpen("/data/EmuSuNi/relr-error.txt", 0x1 | 0x200 | 0x400, 0666);  // write, create, trunc
  if (fd >= 0)
  {
    sceKernelWrite(fd, line, n);
    sceKernelClose(fd);
  }
  __builtin_trap();
}
#else
__attribute__((noreturn)) void ps4_relr_test_fail(const char* what, int32_t code);
#define Fail ps4_relr_test_fail
#endif

__attribute__((visibility("hidden"), no_stack_protector)) void ps4_relr_apply(void)
{
  if (__atomic_load_n(&s_state, __ATOMIC_ACQUIRE) == 2)
    return;
  int expected = 0;
  if (!__atomic_compare_exchange_n(&s_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
  {
    // Another thread is at it (only Sony's modules run before _start): wait for it.
    while (__atomic_load_n(&s_state, __ATOMIC_ACQUIRE) != 2)
      __builtin_ia32_pause();
    return;
  }
  const struct Ps4RelrTable* table = RELR_TABLE;
  if (table->magic == PS4_RELR_MAGIC && table->words > 0)
  {
    const uintptr_t base = (uintptr_t)table - table->self;
    // The pages to make writable, each mapping of them with its protection (given back after).
    enum { MAX_RANGES = 8 };
    uintptr_t range_at[MAX_RANGES], range_end[MAX_RANGES];
    uint32_t range_prot[MAX_RANGES];
    int ranges = 0;
    if (table->protect_end > table->protect_start)
    {
      uintptr_t at = base + table->protect_start;
      const uintptr_t end = base + table->protect_end;
      while (at < end)
      {
        void* start = 0;
        void* stop = 0;
        uint32_t prot = 0;
        int32_t result = sceKernelQueryMemoryProtection((void*)at, &start, &stop, &prot);
        if (result != 0)
          Fail("relr: sceKernelQueryMemoryProtection", result);
        uintptr_t next = (uintptr_t)stop;
        if (next <= at || next > end)
          next = end;
        if (!(prot & PROT_CPU_WRITE))
        {
          if (ranges == MAX_RANGES)
            Fail("relr: mappings", ranges);
          result = sceKernelMprotect((void*)at, next - at, (int32_t)(prot | PROT_CPU_READ | PROT_CPU_WRITE));
          if (result != 0)
            Fail("relr: sceKernelMprotect", result);
          range_at[ranges] = at;
          range_end[ranges] = next;
          range_prot[ranges++] = prot;
        }
        at = next;
      }
    }
    const uint64_t* word = (const uint64_t*)(table + 1);
    const uint64_t* const last = word + table->words;
    uint64_t* where = 0;
    for (; word < last; word++)
    {
      const uint64_t entry = *word;
      if ((entry & 1) == 0)
      {
        // An address: the pointer there, then the bitmaps go on from the word after it.
        where = (uint64_t*)(base + entry);
        *where++ += base;
      }
      else
      {
        // A bitmap: bit i (from 1) for the pointer i - 1 words on.
        uint64_t* at = where;
        for (uint64_t bits = entry >> 1; bits != 0; bits >>= 1, at++)
        {
          if (bits & 1)
            *at += base;
        }
        where += 63;
      }
    }
    for (int i = 0; i < ranges; i++)
    {
      const int32_t result = sceKernelMprotect((void*)range_at[i], range_end[i] - range_at[i], (int32_t)range_prot[i]);
      if (result != 0)
        Fail("relr: sceKernelMprotect back", result);
    }
  }
  ps4_relr_done = 1;
  __atomic_store_n(&s_state, 2, __ATOMIC_RELEASE);
}

#ifndef PS4_RELR_TEST
// The very first constructor (.init_array is sorted by priority, this one 0): a no-op after _start's call, but first
// should the loader run the constructors before _start. (Its own entry is one of the loader's relocations.)
static void FirstConstructor(void)
{
  ps4_relr_apply();
}
__attribute__((section(".init_array.00000"), used)) static void (*const s_first_constructor)(void) = FirstConstructor;
#endif
