// The memory probe (01.50, a measurement only: the word off in /data/EmuSuNi/3ds-mem-probe turns it off; on the PC
// EMUSUNI_3DS_MEM_PROBE=0). The performance specialist's: whether the PS4 gives 2 MiB pages and what they're worth for
// the 3DS emulation, whose game code (dynarmic's blocks, ~2 MiB hot over ~1,100 pages of 4 KiB) and game data (the
// fast memory's view, mapped in 16 KiB pieces) look bound by the TLB and the caches more than by instructions.
// Once, at the first 3DS game's first translated block (port/ps4_3ds_profile.cpp's hook calls it), on a thread of its
// own on cores 4-5, in well under a second; nothing of the game's is touched. Four lines in the log ("mem-probe:"):
//  1. 2 MiB pages, by mincore's MINCORE_SUPER (the kernel's own word): the probe's direct memory aligned to 2 MiB, the
//     same memory mapped again 16 KiB off (where a 2 MiB page can't be), the probe's system flexible memory
//     (read/write/execute, as the 3DS JIT's), the 3DS JIT's own memory and the 3DS memory's backing (0x2600000000).
//  2. Data: pointer chases (ns a step) on the same direct memory through both mappings: 256 pages in 1 MiB (the TLB
//     holds them either way), 4,096 pages in 16 MiB and 16,384 in 64 MiB (a line each: the 4 KiB TLB can't hold them,
//     2 MiB pages could), and random lines over 64 MiB (memory's own latency). A slower misaligned mapping = 2 MiB
//     pages that pay.
//  3. Code: a chain of 1,024 jumps, one in each of 1,024 pages, against the same chain packed in 16 pages (ns a jump),
//     in system flexible memory read/write/execute (run only when the 3DS JIT's own memory is: its test ran code
//     there); direct memory mapped read/write/execute is looked at (2 MiB pages, protection), never run.
//  4. What it took.
// It needs ~85 MiB of direct memory for ~0.3 s (given back; skipped below 256 MiB free) and 8 MiB of system flexible
// memory (skipped if refused).

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <vector>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#if !defined(__ORBIS__)
#include <sched.h>
#endif

extern "C" void emusuni_log_line(const char* text);

#if defined(__ORBIS__)
extern "C" {
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* phys);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t* phys, size_t* size);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name);
int32_t sceKernelVirtualQuery(const void* addr, int flags, void* info, size_t info_size);
int emusuni_probe_mincore(const void* addr, size_t len, char* vec) __asm__("mincore");  // (libkernel's, FreeBSD's)
void* emusuni_probe_self(void) __asm__("scePthreadSelf");
int emusuni_probe_affinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
}
#endif

namespace {

constexpr size_t kMiB = 1024 * 1024, k2M = 2 * kMiB, kSpan = 64 * kMiB, kLine = 64, kPage4 = 4096;
#if defined(__ORBIS__)
constexpr size_t kOsPage = 16384;  // the PS4's page (mincore's unit)
constexpr int32_t kProtRW = 0x3, kProtRWX = 0x7, kMapFixed = 0x10;
struct VirtualQueryInfo {  // libkernel's SceKernelVirtualQueryInfo (as port/ps4_crashlog.cpp has it)
    uintptr_t start, end;
    int64_t offset;
    int32_t protection, memory_type;
    uint32_t flags;
    char name[32];
};
#else
constexpr size_t kOsPage = 4096;
#endif

double seconds() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) / 1e9;
}

// ---- Memory: two mappings of the same memory (2 MiB aligned, and 16 KiB off) ----
struct Pair {
    unsigned char* aligned = nullptr;
    unsigned char* off = nullptr;  // the same memory 16 KiB on (its first 16 KiB at the aligned one's 16 KiB)
    size_t len = 0;
    bool same = false;  // off really is the same memory (direct memory aliasing)
    int err = 0;
#if defined(__ORBIS__)
    off_t phys = 0;
    size_t physLen = 0;
#endif
};

bool mapPair(Pair& pair, size_t len, bool exec) {
    pair.len = len;
#if defined(__ORBIS__)
    const int32_t prot = exec ? kProtRWX : kProtRW;
    pair.physLen = len + k2M;
    int32_t r = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), pair.physLen, k2M, 0,
                                              &pair.phys);
    if (r != 0) {
        pair.err = r;
        return false;
    }
    void* a = nullptr;
    r = sceKernelMapDirectMemory(&a, len, prot, 0, pair.phys, k2M);
    if (r != 0) {
        pair.err = r;
        sceKernelReleaseDirectMemory(pair.phys, pair.physLen);
        return false;
    }
    void* b = nullptr;
    r = sceKernelMapDirectMemory(&b, len, prot, 0, pair.phys + 16384, k2M);
    if (r != 0) {
        pair.err = r;
        sceKernelMunmap(a, len);
        sceKernelReleaseDirectMemory(pair.phys, pair.physLen);
        return false;
    }
    pair.aligned = static_cast<unsigned char*>(a);
    pair.off = static_cast<unsigned char*>(b);
    memset(pair.aligned, 0, len);
    pair.aligned[16384] = 0x5A;
    pair.same = pair.off[0] == 0x5A;
    return true;
#else
    // (The PC: anonymous memory with and without transparent huge pages; not the same memory.)
    const int prot = PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0);
    void* a = mmap(nullptr, len + k2M, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* b = mmap(nullptr, len + k2M, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) {
        pair.err = errno;
        return false;
    }
    pair.aligned = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(a) + k2M - 1) & ~(k2M - 1));
    pair.off = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(b) + k2M - 1) & ~(k2M - 1)) + 16384;
    madvise(pair.aligned, len, MADV_HUGEPAGE);
    madvise(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(b) & ~(kOsPage - 1)), len + k2M, MADV_NOHUGEPAGE);
    memset(pair.aligned, 0, len);
    memset(pair.off, 0, len);
    return true;
#endif
}

void unmapPair(Pair& pair) {
#if defined(__ORBIS__)
    if (pair.aligned) {
        sceKernelMunmap(pair.aligned, pair.len);
        sceKernelMunmap(pair.off, pair.len);
        sceKernelReleaseDirectMemory(pair.phys, pair.physLen);
    }
#else
    (void)pair;  // (the PC test process ends)
#endif
    pair.aligned = pair.off = nullptr;
}

int pageFlags(const void* at, size_t len, unsigned char* vec) {
#if defined(__ORBIS__)
    return emusuni_probe_mincore(at, len, reinterpret_cast<char*>(vec));
#else
    return mincore(const_cast<void*>(at), len, vec);
#endif
}

// MINCORE_SUPER pages (the kernel's page) of [at, at + len): "s/n", or "?" when mincore refuses.
void superPages(const void* at, size_t len, char* out, size_t outSize) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(at) & ~(kOsPage - 1);
    const size_t pages = (reinterpret_cast<uintptr_t>(at) + len - start + kOsPage - 1) / kOsPage;
    std::vector<unsigned char> vec(pages + 16, 0);
    if (pageFlags(reinterpret_cast<const void*>(start), pages * kOsPage, vec.data()) != 0) {
        snprintf(out, outSize, "? (mincore %d)", errno);
        return;
    }
    size_t super = 0, in = 0;
    for (size_t i = 0; i < pages; i++) {
        super += (vec[i] & 0x20) ? 1 : 0;
        in += (vec[i] & 0x1) ? 1 : 0;
    }
    snprintf(out, outSize, "%zu of %zu (%zu resident)", super, pages, in);
}

// ---- Data: pointer chases ----
// A cycle through `count` lines of [base, base + span): one in each of `count` pages of 4 KiB taken at random (each
// at its own line in the page, so the lines spread over the cache's sets), or random lines anywhere (lines). The same
// offsets for both mappings (seeded).
std::vector<uint32_t> chaseOffsets(size_t span, size_t count, bool lines, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint32_t> offsets;
    offsets.reserve(count);
    if (lines) {
        std::vector<uint32_t> all(span / kLine);
        for (size_t i = 0; i < all.size(); i++)
            all[i] = static_cast<uint32_t>(i);
        for (size_t i = 0; i < count; i++) {
            std::swap(all[i], all[i + rng() % (all.size() - i)]);
            offsets.push_back(all[i] * static_cast<uint32_t>(kLine));
        }
    } else {
        std::vector<uint32_t> pages(span / kPage4);
        for (size_t i = 0; i < pages.size(); i++)
            pages[i] = static_cast<uint32_t>(i);
        for (size_t i = 0; i < count; i++) {
            std::swap(pages[i], pages[i + rng() % (pages.size() - i)]);
            offsets.push_back(pages[i] * static_cast<uint32_t>(kPage4) +
                              static_cast<uint32_t>((pages[i] * 7 + i) % (kPage4 / kLine)) * static_cast<uint32_t>(kLine));
        }
    }
    return offsets;  // (already in a random order: the cycle follows it)
}

double chase(unsigned char* base, const std::vector<uint32_t>& offsets, size_t steps) {
    const size_t n = offsets.size();
    for (size_t i = 0; i < n; i++)
        *reinterpret_cast<void**>(base + offsets[i]) = base + offsets[(i + 1) % n];
    void* p = base + offsets[0];
    for (size_t i = 0; i < steps / 4; i++)  // (warm)
        p = *static_cast<void**>(p);
    const double t0 = seconds();
    for (size_t i = 0; i < steps; i++)
        p = *static_cast<void**>(p);
    const double t = seconds() - t0;
    void* volatile sink = p;  // (the chase kept)
    (void)sink;
    return t * 1e9 / static_cast<double>(steps);
}

// ---- Code: a chain of jumps ----
// `count` pieces "add eax, 1; jmp next" at offsets of [code, ...), in a random order, the last "add eax, 1; ret", the
// entry "xor eax, eax; jmp first". ns a jump over `runs` runs; -1 if it didn't count right.
double jumps(unsigned char* code, size_t stride, size_t count, int runs, uint32_t seed) {
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; i++)
        order[i] = i;
    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    auto at = [&](size_t i) { return code + 64 + order[i] * stride; };
    auto jmp = [](unsigned char* from, unsigned char* to) {
        from[0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(to - (from + 5));
        memcpy(from + 1, &rel, 4);
    };
    code[0] = 0x31;  // xor eax, eax
    code[1] = 0xC0;
    jmp(code + 2, at(0));
    for (size_t i = 0; i < count; i++) {
        unsigned char* p = at(i);
        p[0] = 0x83;  // add eax, 1
        p[1] = 0xC0;
        p[2] = 0x01;
        if (i + 1 < count)
            jmp(p + 3, at(i + 1));
        else
            p[3] = 0xC3;  // ret
    }
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + 64 + count * stride + 64));
    auto run = reinterpret_cast<int (*)()>(code);
    if (run() != static_cast<int>(count))
        return -1;
    const double t0 = seconds();
    for (int r = 0; r < runs; r++)
        run();
    return (seconds() - t0) * 1e9 / (static_cast<double>(runs) * static_cast<double>(count));
}

struct Jit {
    const void* begin;
    size_t size;
};

void* probe(void* arg) {
    const Jit jit = *static_cast<Jit*>(arg);
    delete static_cast<Jit*>(arg);
#if defined(__ORBIS__)
    emusuni_probe_affinity(emusuni_probe_self(), (1ull << 4) | (1ull << 5));
#endif
    const double start = seconds();
    char line[1024], a[96], b[96], c[96], d[96], e[96];

    // Direct memory for the data: 64 MiB, mapped twice.
    Pair data;
    bool haveData = false;
#if defined(__ORBIS__)
    off_t freeAt = 0;
    size_t freeSize = 0;
    sceKernelAvailableDirectMemorySize(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), k2M, &freeAt, &freeSize);
    if (freeSize >= 256 * kMiB)
        haveData = mapPair(data, kSpan, false);
    else
        data.err = -1;
#else
    haveData = mapPair(data, kSpan, false);
#endif
    // System flexible memory, read/write/execute (as the 3DS JIT's), 8 MiB.
    unsigned char* flex = nullptr;
    int flexErr = 0;
    const size_t flexLen = 8 * kMiB;
#if defined(__ORBIS__)
    {
        void* base = reinterpret_cast<void*>(0x1400000000ull);
        flexErr = sceKernelReserveVirtualRange(&base, flexLen, 0, k2M);
        if (flexErr == 0) {
            void* at = base;
            flexErr = sceKernelMapNamedSystemFlexibleMemory(&at, flexLen, kProtRWX, kMapFixed, "3ds mem probe");
            if (flexErr == 0 && at == base)
                flex = static_cast<unsigned char*>(base);
            else
                sceKernelMunmap(base, flexLen);
        }
    }
#else
    {
        void* p = mmap(nullptr, flexLen, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        flex = p == MAP_FAILED ? nullptr : static_cast<unsigned char*>(p);
        flexErr = flex ? 0 : errno;
    }
#endif
    // Direct memory, read/write/execute, aligned and 16 KiB off, for the code (if the kernel allows it).
    Pair code;
    const bool haveCode = haveData && mapPair(code, 16 * kMiB, true);

    // 1. 2 MiB pages, as the kernel says.
    if (haveData) {
        superPages(data.aligned, data.len, a, sizeof(a));
        superPages(data.off, data.len, b, sizeof(b));
    } else {
        snprintf(a, sizeof(a), "not mapped (%#x)", static_cast<unsigned>(data.err));
        snprintf(b, sizeof(b), "-");
    }
    if (flex) {
        memset(flex, 0xCC, flexLen);
        superPages(flex, flexLen, c, sizeof(c));
    } else {
        snprintf(c, sizeof(c), "refused (%#x)", static_cast<unsigned>(flexErr));
    }
    superPages(jit.begin, std::min<size_t>(jit.size, 16 * kMiB), d, sizeof(d));
    snprintf(e, sizeof(e), "-");
#if defined(__ORBIS__)
    {
        VirtualQueryInfo info;
        memset(&info, 0, sizeof(info));
        const uintptr_t backing = 0x2600000000ull;
        if (sceKernelVirtualQuery(reinterpret_cast<const void*>(backing), 0, &info, sizeof(info)) == 0 &&
            info.start <= backing && backing < info.end)
            superPages(reinterpret_cast<const void*>(backing), std::min<size_t>(info.end - backing, 64 * kMiB), e,
                       sizeof(e));
    }
#endif
    snprintf(line, sizeof(line),
             "mem-probe: 2 MiB pages (mincore's MINCORE_SUPER, of the kernel's %zu KiB pages): direct memory aligned "
             "to 2 MiB %s; the same memory 16 KiB off %s%s; system flexible read/write/execute %s; the 3DS JIT's "
             "memory %s; the 3DS memory's backing %s (3ds-mem-probe)",
             kOsPage / 1024, a, b, haveData && !data.same ? " (not the same memory: no aliasing)" : "", c, d, e);
    emusuni_log_line(line);

    // 2. Data.
    if (haveData) {
        struct Test {
            const char* name;
            size_t span, count;
            bool lines;
            size_t steps;
        } tests[] = {{"256 pages in 1 MiB", 1 * kMiB, 256, false, 400000},
                     {"4,096 pages in 16 MiB", 16 * kMiB, 4096, false, 400000},
                     {"16,384 pages in 64 MiB", 64 * kMiB, 16384, false, 400000},
                     {"random lines over 64 MiB", 64 * kMiB, 131072, true, 300000}};
        int n = snprintf(line, sizeof(line), "mem-probe: data, ns a step (aligned to 2 MiB / 16 KiB off):");
        for (const Test& t : tests) {
            const auto offsets = chaseOffsets(t.span, t.count, t.lines, 49);
            const double x = chase(data.aligned, offsets, t.steps);
            const double y = chase(data.off, offsets, t.steps);
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f / %.1f;", t.name, x, y);
        }
        emusuni_log_line(line);
    }

    // 3. Code. Run only in system flexible memory mapped as the 3DS JIT's is, and only when that one is read/write/
    // execute at once (its own test ran code there before the first block): a fault here would take the app down,
    // and the game's fast memory needs the fault handlers as they are. Direct memory mapped executable is looked at
    // (its 2 MiB pages, its protection), never run.
    {
        bool jitRwx = true;
        int32_t jitProt = 7;
#if defined(__ORBIS__)
        VirtualQueryInfo info;
        memset(&info, 0, sizeof(info));
        const uintptr_t at = reinterpret_cast<uintptr_t>(jit.begin);
        jitRwx = sceKernelVirtualQuery(jit.begin, 0, &info, sizeof(info)) == 0 && info.start <= at && at < info.end &&
                 (info.protection & 7) == 7;
        jitProt = info.protection;
#endif
        const size_t count = 1024, spread = kPage4 + kLine, packed = kLine;
        int n = snprintf(line, sizeof(line), "mem-probe: code, ns a jump (1,024 jumps in 1,024 pages / packed in 16):");
        if (flex && jitRwx) {
            const double x = jumps(flex, spread, count, 300, 7);
            const double y = jumps(flex + 6 * kMiB, packed, count, 300, 7);
            n += snprintf(line + n, sizeof(line) - n, " system flexible %.1f / %.1f;", x, y);
        } else {
            n += snprintf(line + n, sizeof(line) - n, " not run (the 3DS JIT's memory isn't read/write/execute: %#x);",
                          static_cast<unsigned>(jitProt));
        }
        if (haveCode) {
            superPages(code.aligned, code.len, a, sizeof(a));
            superPages(code.off, code.len, b, sizeof(b));
            int32_t prot = 7;
#if defined(__ORBIS__)
            memset(&info, 0, sizeof(info));
            prot = sceKernelVirtualQuery(code.aligned, 0, &info, sizeof(info)) == 0 ? info.protection : -1;
#endif
            n += snprintf(line + n, sizeof(line) - n,
                          " direct memory read/write/execute: mapped (protection %#x), 2 MiB pages aligned %s, 16 KiB "
                          "off %s (not run: code there could fault)",
                          static_cast<unsigned>(prot), a, b);
        } else if (haveData) {
            n += snprintf(line + n, sizeof(line) - n, " direct memory read/write/execute: refused (%#x)",
                          static_cast<unsigned>(code.err));
        }
        emusuni_log_line(line);
    }

    unmapPair(code);
    unmapPair(data);
#if defined(__ORBIS__)
    if (flex)
        sceKernelMunmap(flex, flexLen);
#endif
    snprintf(line, sizeof(line), "mem-probe: done in %.0f ms (on CPU %d)", (seconds() - start) * 1000.0,
#if defined(__ORBIS__)
             -1
#else
             sched_getcpu()
#endif
    );
    emusuni_log_line(line);
    return nullptr;
}

bool switchedOff() {
#if defined(__ORBIS__)
    FILE* file = fopen("/data/EmuSuNi/3ds-mem-probe", "r");
    if (!file)
        return false;
    char word[16] = {};
    const size_t got = fread(word, 1, sizeof(word) - 1, file);
    fclose(file);
    word[got] = 0;
    return strncmp(word, "off", 3) == 0;
#else
    const char* value = getenv("EMUSUNI_3DS_MEM_PROBE");
    return value && (strcmp(value, "0") == 0 || strcmp(value, "off") == 0);
#endif
}

}  // namespace

// port/ps4_3ds_profile.cpp's hook, at the first 3DS game's first block: the JIT's memory. Once a process.
extern "C" void emusuni_mem_probe_start(const void* jit_begin, size_t jit_size) {
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;
    if (switchedOff()) {
        emusuni_log_line("mem-probe: off (the word off in 3ds-mem-probe)");
        return;
    }
    pthread_t thread;
    Jit* jit = new Jit{jit_begin, jit_size};
    if (pthread_create(&thread, nullptr, probe, jit) != 0) {
        delete jit;
        emusuni_log_line("mem-probe: its thread didn't start");
        return;
    }
    pthread_detach(thread);
}
