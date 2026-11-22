// EmuSuNi's PS Vita port (19-11, the performance specialist's; for the Vita specialist to take in): where the
// Vita emulation's CPU time goes on the console, sampled - the switch "profile" (the file
// /data/EmuSuNi/vita-profile-on). Modelled on port/ps4_3ds_profile.cpp.
//
// Why: the log's "vita: threads:" line says how long each guest thread spends in recompiled code (Uncharted's main
// thread ~7.5 s of every 10 on the console), not where. The PC's perf can say it for the PC, whose CPU runs the
// game's code many times faster; the console's share can only be measured on it, and there is no hardware counter.
//
// How: the profiling timer (setitimer ITIMER_PROF, 1 ms of the app's CPU time) sends SIGPROF to the thread that is
// running, whose handler keeps the instruction address, the thread and the CPU. A thread of this file sorts the
// samples, thread by thread, into
//   g  the game's code        a block dynarmic translated (dynarmic says where each is: emusuni_vita_prof_block)
//   d  dynarmic's own         a JIT's memory outside every block: its dispatcher, the return stack's and the fast
//                              dispatch's lookups, the memory accesses' slow paths (named: emusuni_vita_prof_piece)
//   a  the app's C++          the eboot's code: the core's kernel and libraries, dynarmic's C++ (its block lookup,
//                              its translation), the C library (kept as ELF addresses, for the build's ELF)
//   s  the system             anywhere else, by the mapping's name (libkernel's calls, the GPU's libraries...)
// Every guest thread has a JIT of its own (its code memory: emusuni_vita_prof_free says when it is given back), and
// is named by the core's kernel (emusuni_vita_prof_thread); the other threads (the renderer's, the audio's...) are
// counted by their own without a name. Every 10 s: lines in the log (the whole, the three busiest threads, the
// busiest one's hottest blocks) and the window's table to /data/EmuSuNi/vita-profile.txt, for the specialist's
// script (perfil-vita.py: the C++ named with the build's ELF, the blocks by the guest's address).
// A signal the timer meant for a thread that keeps SIGPROF out goes to another thread, often one that waits (the PC
// bench's lavapipe threads keep it out): so each sample carries its thread's CPU time (its CPU clock, found at the
// start) and weighs that thread's CPU time since its last sample - every count is microseconds of CPU, and a sample
// that came to a thread that wasn't running weighs next to nothing. The guest threads let SIGPROF in.
// The file's word "thread" (or the timer giving nothing in 3 s) signals instead, every 0.5-1.5 ms (pthread_kill),
// each live guest thread that is on a core right then (its CPU clock moves): the guest threads only, none starved.
// Off (no file) the hooks return at once; the file is looked for again every ~10 s of hooks (made by FTP meanwhile).
// Sampling stops when the game ends (every JIT's memory given back) and starts again at the next game's first block.
// On the PC (the bench): the variable EMUSUNI_VITA_PROFILE (1 or thread) and EMUSUNI_VITA_PROFILE_FILE (the table).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <x86intrin.h>

#include "ps4_vita_sys.h"

#if !defined(__ORBIS__)
#include <dlfcn.h>
#include <link.h>
#include <sched.h>
#include <ucontext.h>
#endif

#if defined(__ORBIS__)
// FreeBSD's values (the PS4 kernel's), as port/ps4_3ds_profile.cpp has them; the registers 0x40 bytes into the
// context, rip the 21st 8-byte slot.
static_assert(sizeof(sigset_t) == 16, "struct sigaction isn't FreeBSD's: the libc overlay's headers must come first");
extern "C" char __text_start[];  // the OpenOrbis link.x: the start of .text (ELF address 0)
extern "C" int sceKernelGetCurrentCpu(void);
extern "C" int32_t sceKernelVirtualQuery(const void* addr, int flags, void* info, size_t info_size);
extern "C" int sysctlbyname(const char* name, void* old, size_t* old_size, const void* value, size_t size);
#endif

namespace {

#if defined(__ORBIS__)
constexpr int kSigProf = 27, kSaSiginfo = 0x40, kSaRestart = 0x2, kSaOnstack = 0x1, kSigUnblock = 2;
constexpr int kMcontextOffset = 0x40, kRipIndex = 20;
// libkernel's SceKernelVirtualQueryInfo (as port/ps4_crashlog.cpp has it).
struct VirtualQueryInfo {
    uintptr_t start, end;
    int64_t offset;
    int32_t protection, memory_type;
    uint32_t flags;
    char name[32];
};
constexpr int32_t kProtCpuExec = 0x4;
const char* const kSwitchFile = "/data/EmuSuNi/vita-profile-on";
const char* const kTableFile = "/data/EmuSuNi/vita-profile.txt";
uint64_t RipOf(void* context) {
    return reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset)[kRipIndex];
}
int CpuNow() {
    return sceKernelGetCurrentCpu();
}
#else
constexpr int kSigProf = SIGPROF, kSaSiginfo = SA_SIGINFO, kSaRestart = SA_RESTART, kSaOnstack = SA_ONSTACK,
              kSigUnblock = SIG_UNBLOCK;
uint64_t RipOf(void* context) {
    return static_cast<uint64_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
}
int CpuNow() {
    return sched_getcpu();
}
#endif

// ---- The switch: 0 off, 1 the profiling timer (every thread), 2 the guest threads signalled in turn ----
int SwitchMode() {
#if defined(__ORBIS__)
    FILE* const file = fopen(kSwitchFile, "r");
    if (file == nullptr) {
        return 0;
    }
    char word[16] = {};
    const size_t got = fread(word, 1, sizeof(word) - 1, file);
    fclose(file);
    word[got] = 0;
    return strncmp(word, "thread", 6) == 0 ? 2 : 1;
#else
    const char* const value = getenv("EMUSUNI_VITA_PROFILE");
    if (value == nullptr || value[0] == 0 || strcmp(value, "0") == 0 || strcmp(value, "off") == 0) {
        return 0;
    }
    return strcmp(value, "thread") == 0 ? 2 : 1;
#endif
}

std::atomic<int> s_state{0};  // 0 not looked at yet, 1 on, 2 being looked at, -1 off (looked at again), -2 failed
std::atomic<uint64_t> s_looked{0};  // the time stamp counter when the file was last looked for
std::atomic<int> s_mode{0};
bool Start();

bool On() {
    int state = s_state.load(std::memory_order_acquire);
    if (state == 1) {
        return true;
    }
    if (state == 2 || state == -2) {
        return false;
    }
    // (Off: looked at again after ~10 s - 16e9 ticks at the PS4's 1.6 GHz - for a file made meanwhile.)
    if (state == -1 && __rdtsc() - s_looked.load(std::memory_order_relaxed) < 16000000000ull) {
        return false;
    }
    if (!s_state.compare_exchange_strong(state, 2, std::memory_order_acquire)) {
        return s_state.load(std::memory_order_acquire) == 1;
    }
    s_looked.store(__rdtsc(), std::memory_order_relaxed);
    const int mode = SwitchMode();
    if (mode == 0) {
        s_state.store(-1, std::memory_order_release);
        return false;
    }
    s_mode.store(mode, std::memory_order_relaxed);
    if (!Start()) {
        s_state.store(-2, std::memory_order_release);
        return false;
    }
    s_state.store(1, std::memory_order_release);
    return true;
}

std::mutex s_mutex;  // the hooks' (the core's threads) and the sorting thread's when it lets lists go

// ---- The JITs' blocks (each guest thread's JIT writes its own region; the sorting thread reads) ----
struct Block {
    uint32_t offset, size;  // from the region's start; the block's bytes
    uint32_t pc;            // the guest's address, bit 0 set for Thumb
};
struct Blocks {
    uint32_t capacity;
    Block at[1];
};
Blocks* NewBlocks(uint32_t capacity) {
    void* const memory = malloc(sizeof(Blocks) + (capacity - 1) * sizeof(Block));
    if (memory == nullptr) {
        return nullptr;
    }
    Blocks* const list = static_cast<Blocks*>(memory);
    list->capacity = capacity;
    return list;
}
constexpr uint32_t kFirstBlocks = 4096, kMostBlocks = 1u << 20;

struct Region {
    std::atomic<int> state{0};  // 0 free, 3 being made, 1 live, 2 given back (the sorting thread lets its list go)
    std::atomic<uintptr_t> begin{0};
    std::atomic<size_t> size{0};
    std::atomic<Blocks*> blocks{nullptr};
    std::atomic<uint32_t> count{0};  // published after the block is written
    uint32_t last = 0;               // the last block's offset: one below it starts again (the cache emptied)
    uint32_t resets = 0, untracked = 0;
};
constexpr int kRegions = 256;
Region s_regions[kRegions];
std::atomic<int> s_live{0};          // live regions: none (after some) = the game ended
std::atomic<uint32_t> s_unplaced{0}; // blocks of a JIT that found no region
std::vector<Blocks*> s_retired;      // lists grown out of, for the sorting thread to free (under the mutex)
constinit thread_local Region* t_region = nullptr;

// ---- dynarmic's named pieces of each JIT (its dispatcher, its handlers, the memory accesses' slow paths) ----
struct Piece {
    uintptr_t start, end;
    char name[44];
};
constexpr int kPieces = 512;
Piece s_pieces[kPieces];
int s_piece_count = 0;  // (under the mutex)

// ---- The guest threads, as the core's kernel names them ----
struct Thread {
    uint64_t self = 0;  // pthread_self()
    int id = 0;
    bool live = false;
    bool has_clock = false;  // its CPU clock (pthread_getcpuclockid, on the thread itself): mode 2 signals it when it ran
    clockid_t clock = 0;
    uint64_t checked_ns = 0;  // (the sorting thread's) its CPU time at the last look
    char name[32] = {};
};
constexpr int kThreads = 256;
Thread s_threads[kThreads];   // (under the mutex)
std::atomic<uint32_t> s_threads_changed{0};

// ---- The samples: the signal handler (any thread) to the sorting thread, without a lock ----
struct Sample {
    std::atomic<uint64_t> seq;
    uint64_t rip;
    uint64_t self;
    uint64_t cpu_ns;  // the thread's CPU time (0: no clock tells it)
    uint32_t cpu;
};
// The calling thread's CPU clock (FreeBSD's is 14; found once at the start, as port/ps4_vita_stats.cpp finds it):
// a signal the timer meant for a thread that keeps it out (the PC bench's lavapipe threads) goes to another one,
// often one that is waiting; such a sample - its thread hasn't run since its last one - is counted apart.
std::atomic<int> s_cpu_clock{-1};
constexpr uint64_t kRanAtLeastNs = 250000;
constexpr uint32_t kRing = 16384;
Sample* s_ring = nullptr;
std::atomic<uint64_t> s_head{0};
uint64_t s_tail = 0;
std::atomic<uint32_t> s_dropped{0};
std::atomic<uint64_t> s_signals{0};
std::atomic<bool> s_sampling{true};  // false once the game ended (the sorting thread stopped the timer)
std::atomic<bool> s_rearm{false};    // a block of the next game: the sorting thread starts again

void Push(uint64_t rip, uint64_t self, uint64_t cpu_ns, uint32_t cpu) {
    uint64_t pos = s_head.load(std::memory_order_relaxed);
    for (;;) {
        Sample& slot = s_ring[pos & (kRing - 1)];
        const int64_t ahead = static_cast<int64_t>(slot.seq.load(std::memory_order_acquire) - pos);
        if (ahead == 0) {
            if (s_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                slot.rip = rip;
                slot.self = self;
                slot.cpu_ns = cpu_ns;
                slot.cpu = cpu;
                slot.seq.store(pos + 1, std::memory_order_release);
                return;
            }
        } else if (ahead < 0) {
            s_dropped.fetch_add(1, std::memory_order_relaxed);  // full: the sorting thread is behind
            return;
        } else {
            pos = s_head.load(std::memory_order_relaxed);
        }
    }
}

bool Pop(Sample& out) {
    Sample& slot = s_ring[s_tail & (kRing - 1)];
    if (slot.seq.load(std::memory_order_acquire) != s_tail + 1) {
        return false;
    }
    out.rip = slot.rip;
    out.self = slot.self;
    out.cpu_ns = slot.cpu_ns;
    out.cpu = slot.cpu;
    slot.seq.store(s_tail + kRing, std::memory_order_release);
    s_tail++;
    return true;
}

void OnSample(int, siginfo_t*, void* context) {
    const int saved = errno;
    s_signals.fetch_add(1, std::memory_order_relaxed);
    int cpu = CpuNow();
    if (cpu < 0 || cpu > 15) {
        cpu = 15;
    }
    uint64_t cpu_ns = 0;
    const int clock = s_cpu_clock.load(std::memory_order_relaxed);
    timespec ts;
    if (clock >= 0 && clock_gettime(static_cast<clockid_t>(clock), &ts) == 0) {
        cpu_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    }
    Push(RipOf(context), reinterpret_cast<uint64_t>(pthread_self()), cpu_ns, static_cast<uint32_t>(cpu));
    errno = saved;
}

// ---- The sorting thread ----
enum Kind { kGame, kDynarmic, kApp, kSystem, kKinds };
const char* const kKindNames[kKinds] = {"g", "d", "a", "s"};

uintptr_t s_text_start = 0, s_text_end = 0, s_text_base = 0;  // the eboot's code; its ELF address 0
std::unordered_map<uint64_t, std::string> s_page_names;      // the system's pages (4 KiB) by mapping name

#if !defined(__ORBIS__)
int FindObject(struct dl_phdr_info* info, size_t, void* data) {
    const uintptr_t mine = *static_cast<uintptr_t*>(data);
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || !(header.p_flags & PF_X)) {
            continue;
        }
        const uintptr_t from = info->dlpi_addr + header.p_vaddr;
        if (mine >= from && mine < from + header.p_memsz) {
            s_text_start = from;
            s_text_end = from + header.p_memsz;
            s_text_base = info->dlpi_addr;
            return 1;
        }
    }
    return 0;
}
#endif

void FindText() {
#if defined(__ORBIS__)
    s_text_start = s_text_base = reinterpret_cast<uintptr_t>(__text_start);
    s_text_end = s_text_start + 0x4000000;  // (port/ps4_crashlog.cpp's window if the kernel won't say)
    VirtualQueryInfo info;
    uintptr_t at = s_text_start, end = 0;
    // The executable mappings from .text's start on, one after the other (the eboot's .text is ~100 MiB).
    for (int i = 0; i < 16; i++) {
        memset(&info, 0, sizeof(info));
        if (sceKernelVirtualQuery(reinterpret_cast<const void*>(at), 0, &info, sizeof(info)) != 0 || info.start > at ||
            info.end <= at || !(info.protection & kProtCpuExec)) {
            break;
        }
        end = info.end;
        at = info.end;
    }
    if (end > s_text_start) {
        s_text_end = end;
    }
#else
    // (The bench: the core is a shared object; its code by ELF address, for nm or llvm-symbolizer on it.)
    uintptr_t mine = reinterpret_cast<uintptr_t>(&FindText);
    dl_iterate_phdr(FindObject, &mine);
#endif
}

std::string PageName(uint64_t page) {
    const auto found = s_page_names.find(page);
    if (found != s_page_names.end()) {
        return found->second;
    }
    std::string name = "?";
#if defined(__ORBIS__)
    VirtualQueryInfo info;
    memset(&info, 0, sizeof(info));
    if (sceKernelVirtualQuery(reinterpret_cast<const void*>(page << 12), 0, &info, sizeof(info)) == 0) {
        info.name[sizeof(info.name) - 1] = 0;
        name = info.name[0] != 0 ? info.name : "anonymous";
    }
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(page << 12), &info) != 0 && info.dli_fname != nullptr) {
        name = info.dli_fname;
        const size_t slash = name.rfind('/');
        if (slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
    }
#endif
    for (char& c : name) {
        if (c == ' ' || c < 0x20) {
            c = '_';
        }
    }
    s_page_names.emplace(page, name);
    return name;
}

// A sample's kind and key: g the guest's address (and the block's bytes in `bytes`), d the offset in its JIT's
// memory (16-byte steps), a the ELF address, s the page.
Kind Classify(uint64_t rip, uint64_t& key, uint32_t& bytes) {
    if (rip >= s_text_start && rip < s_text_end) {
        key = rip - s_text_base;
        return kApp;
    }
    for (Region& region : s_regions) {
        if (region.state.load(std::memory_order_acquire) != 1) {
            continue;
        }
        const uintptr_t begin = region.begin.load(std::memory_order_acquire);
        if (rip < begin || rip - begin >= region.size.load(std::memory_order_relaxed)) {
            continue;
        }
        const uint32_t offset = static_cast<uint32_t>(rip - begin);
        const Blocks* const list = region.blocks.load(std::memory_order_acquire);
        uint32_t count = region.count.load(std::memory_order_acquire);
        if (list != nullptr) {
            count = std::min(count, list->capacity);
            // The last block that starts at or before the address (offsets rise until the cache empties).
            const Block* const first = list->at;
            const Block* const after = std::upper_bound(first, first + count, offset,
                                                        [](uint32_t value, const Block& block) { return value < block.offset; });
            if (after != first && offset - after[-1].offset < after[-1].size) {
                key = after[-1].pc;
                bytes = after[-1].size;
                return kGame;
            }
        }
        key = offset & ~15u;
        return kDynarmic;
    }
    key = rip >> 12;
    return kSystem;
}

double SecondsNow() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

// How many of the most sampled keys hold a share of the samples.
uint32_t KeysFor(const std::vector<std::pair<uint64_t, uint64_t>>& sorted, uint64_t total, double share) {
    uint64_t sum = 0;
    for (size_t i = 0; i < sorted.size(); i++) {
        sum += sorted[i].second;
        if (static_cast<double>(sum) >= share * static_cast<double>(total)) {
            return static_cast<uint32_t>(i + 1);
        }
    }
    return static_cast<uint32_t>(sorted.size());
}

std::vector<std::pair<uint64_t, uint64_t>> Sorted(const std::unordered_map<uint64_t, uint64_t>& keys) {
    std::vector<std::pair<uint64_t, uint64_t>> out(keys.begin(), keys.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

// Every count is microseconds of CPU: a sample weighs its thread's CPU time since that thread's last sample (at most
// 4 ms; 1 ms for a thread's first, or when no clock tells it). A sample that came to a thread that wasn't running -
// the timer's signal meant for one that keeps SIGPROF out - so weighs next to nothing; those under 0.25 ms are
// counted apart too ("x").
struct PerThread {
    uint64_t kinds[kKinds] = {};
    uint32_t samples = 0, diverted = 0;
    uint32_t cpus = 0;  // the cores it was sampled on (a bit each)
    std::unordered_map<uint64_t, uint64_t> keys[kKinds];
};
struct Window {
    std::unordered_map<uint64_t, PerThread> threads;  // by pthread_self()
    std::unordered_map<uint64_t, uint32_t> bytes;     // a block's bytes by its guest address (the last seen)
    uint64_t cpu[16] = {};
    uint64_t total = 0, samples = 0, diverted = 0;
};
std::unordered_map<uint64_t, uint64_t> s_last_cpu_ns;  // each thread's CPU time at its last sample
constexpr uint64_t kFirstWeightUs = 1000, kMostWeightUs = 4000;

struct Named {
    int id;
    char name[32];
};
std::unordered_map<uint64_t, Named> s_names;  // the sorting thread's copy of the guest threads' names
uint32_t s_names_seen = ~0u;

void CopyNames() {
    const uint32_t changed = s_threads_changed.load(std::memory_order_acquire);
    if (changed == s_names_seen) {
        return;
    }
    s_names_seen = changed;
    std::lock_guard<std::mutex> guard(s_mutex);
    for (const Thread& thread : s_threads) {
        if (thread.self != 0) {
            Named& named = s_names[thread.self];
            named.id = thread.id;
            memcpy(named.name, thread.name, sizeof(named.name));
        }
    }
}

// dynarmic's pieces by their offset in a JIT's memory (the same in every guest thread's: one JIT's are enough).
struct PieceAt {
    uint32_t offset, size;
    std::string name;
};
std::vector<PieceAt> s_piece_offsets;
bool s_pieces_written = false;

void FindPieceOffsets() {
    if (!s_piece_offsets.empty()) {
        return;
    }
    std::lock_guard<std::mutex> guard(s_mutex);
    for (Region& region : s_regions) {
        if (region.state.load(std::memory_order_acquire) != 1) {
            continue;
        }
        const uintptr_t begin = region.begin.load(std::memory_order_relaxed);
        const size_t size = region.size.load(std::memory_order_relaxed);
        std::vector<PieceAt> found;
        for (int i = 0; i < s_piece_count; i++) {
            const Piece& piece = s_pieces[i];
            if (piece.start >= begin && piece.start - begin < size) {
                found.push_back({static_cast<uint32_t>(piece.start - begin), static_cast<uint32_t>(piece.end - piece.start),
                                 piece.name});
            }
        }
        if (found.size() >= 3) {
            std::sort(found.begin(), found.end(), [](const PieceAt& a, const PieceAt& b) { return a.offset < b.offset; });
            s_piece_offsets = std::move(found);
            return;
        }
    }
}

const char* PieceName(uint64_t offset) {
    for (const PieceAt& piece : s_piece_offsets) {
        if (offset >= piece.offset && offset < piece.offset + piece.size) {
            return piece.name.c_str();
        }
    }
    return "other";
}

const char* ThreadName(uint64_t self, int* id) {
    const auto found = s_names.find(self);
    if (found == s_names.end()) {
        *id = -1;
        return "-";
    }
    *id = found->second.id;
    return found->second.name;
}

void Report(Window& window, int number, double from, double length, FILE* table) {
    if (window.total == 0) {
        return;
    }
    CopyNames();
    FindPieceOffsets();
    const int mode = s_mode.load(std::memory_order_relaxed);
    auto per = [](uint64_t part, uint64_t whole) { return whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0; };
    // The threads by samples; the guest threads' sum.
    std::vector<std::pair<uint64_t, uint64_t>> order;
    uint64_t guest[kKinds] = {}, guest_total = 0;
    for (const auto& [self, thread] : window.threads) {
        uint64_t sum = 0;
        for (int k = 0; k < kKinds; k++) {
            sum += thread.kinds[k];
        }
        order.push_back({self, sum});
        if (s_names.count(self) != 0) {
            for (int k = 0; k < kKinds; k++) {
                guest[k] += thread.kinds[k];
            }
            guest_total += sum;
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const uint32_t dropped = s_dropped.exchange(0, std::memory_order_relaxed);

    char line[600];
    int n = snprintf(line, sizeof(line),
                     "vita: profile: %.1f s at %.0f s (%s): %.0f ms of CPU in %llu samples, %zu threads; the guest threads "
                     "%.1f%% (the game's code %.1f%%, dynarmic's own %.1f%%, the app's C++ %.1f%%, the system %.1f%%), the "
                     "others %.1f%%",
                     length, from, mode == 1 ? "the profiling timer" : "the guest threads that ran, signalled",
                     static_cast<double>(window.total) / 1000.0, static_cast<unsigned long long>(window.samples),
                     window.threads.size(), per(guest_total, window.total), per(guest[kGame], guest_total),
                     per(guest[kDynarmic], guest_total), per(guest[kApp], guest_total), per(guest[kSystem], guest_total),
                     per(window.total - guest_total, window.total));
    if (window.diverted != 0 && n < 520) {
        n += snprintf(line + n, sizeof(line) - n, "; %llu samples to threads that weren't running (they weigh next to nothing)",
                      static_cast<unsigned long long>(window.diverted));
    }
    if (dropped != 0 && n < 560) {
        n += snprintf(line + n, sizeof(line) - n, "; %u lost", dropped);
    }
    emusuni_vita_log("%s", line);

    // The three busiest threads.
    for (size_t t = 0; t < order.size() && t < 3; t++) {
        const PerThread& thread = window.threads[order[t].first];
        const uint64_t all = order[t].second;
        int id = -1;
        const char* const name = ThreadName(order[t].first, &id);
        const auto blocks = Sorted(thread.keys[kGame]);
        const auto pieces = Sorted(thread.keys[kDynarmic]);
        std::unordered_map<std::string, uint64_t> piece_sums;
        for (const auto& [offset, count] : pieces) {
            piece_sums[PieceName(offset)] += count;
        }
        std::vector<std::pair<std::string, uint64_t>> named(piece_sums.begin(), piece_sums.end());
        std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        n = snprintf(line, sizeof(line),
                     "vita: profile: \"%s\"%s %.0f%% of a core (%u samples): the game's code %.1f%% (%zu blocks; half "
                     "of it in %u, 90%% in %u), dynarmic's own %.1f%%",
                     id >= 0 ? name : "a thread of the app's", id >= 0 ? "" : " (not a guest thread)",
                     static_cast<double>(all) / length / 1e4, thread.samples,
                     per(thread.kinds[kGame], all), blocks.size(), KeysFor(blocks, thread.kinds[kGame], 0.5),
                     KeysFor(blocks, thread.kinds[kGame], 0.9), per(thread.kinds[kDynarmic], all));
        for (size_t i = 0; i < named.size() && i < 2 && n < 500; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%s%s %.1f", i == 0 ? " (" : ", ", named[i].first.c_str(),
                          per(named[i].second, all));
        }
        if (!named.empty() && n < 560) {
            n += snprintf(line + n, sizeof(line) - n, ")");
        }
        if (n < 520) {
            n += snprintf(line + n, sizeof(line) - n, ", the app's C++ %.1f%%, the system %.1f%%", per(thread.kinds[kApp], all),
                          per(thread.kinds[kSystem], all));
        }
        emusuni_vita_log("%s", line);
        if (t == 0 && !blocks.empty()) {
            n = snprintf(line, sizeof(line), "vita: profile: \"%s\"'s busiest blocks (the guest's address, %% of the thread):",
                         id >= 0 ? name : "-");
            for (size_t i = 0; i < blocks.size() && i < 12 && n < 540; i++) {
                n += snprintf(line + n, sizeof(line) - n, " %08llX%s %.1f",
                              static_cast<unsigned long long>(blocks[i].first & ~1ull), (blocks[i].first & 1) != 0 ? "t" : "",
                              per(blocks[i].second, all));
            }
            emusuni_vita_log("%s", line);
        }
    }

    if (table == nullptr) {
        return;
    }
    if (!s_pieces_written && !s_piece_offsets.empty()) {
        s_pieces_written = true;
        for (const PieceAt& piece : s_piece_offsets) {
            fprintf(table, "piece %x %x %s\n", piece.offset, piece.size, piece.name.c_str());
        }
    }
    // (Every count below in microseconds of CPU.)
    fprintf(table, "window %d %.3f %.3f mode %d us %llu samples %llu threads %zu dropped %u unplaced %u diverted %llu\n",
            number, from, length, mode, static_cast<unsigned long long>(window.total),
            static_cast<unsigned long long>(window.samples), window.threads.size(), dropped,
            s_unplaced.exchange(0, std::memory_order_relaxed), static_cast<unsigned long long>(window.diverted));
    for (int c = 0; c < 16; c++) {
        if (window.cpu[c] != 0) {
            fprintf(table, "cpu %d %llu\n", c, static_cast<unsigned long long>(window.cpu[c]));
        }
    }
    for (const auto& [self, all] : order) {
        const PerThread& thread = window.threads[self];
        int id = -1;
        std::string name = ThreadName(self, &id);
        for (char& c : name) {
            if (c == ' ' || c < 0x20) {
                c = '_';
            }
        }
        fprintf(table, "thread %llx %d %s %llu", static_cast<unsigned long long>(self), id, name.c_str(),
                static_cast<unsigned long long>(all));
        for (int k = 0; k < kKinds; k++) {
            fprintf(table, " %s %llu", kKindNames[k], static_cast<unsigned long long>(thread.kinds[k]));
        }
        fprintf(table, " x %u n %u cpus %x\n", thread.diverted, thread.samples, thread.cpus);
        // (The keys of the threads with at least 0.2% of the window's time.)
        if (all * 500 < window.total) {
            continue;
        }
        for (int k = 0; k < kKinds; k++) {
            if (k == kSystem) {
                std::unordered_map<std::string, uint64_t> names;
                for (const auto& [page, count] : thread.keys[k]) {
                    names[PageName(page)] += count;
                }
                for (const auto& [mapping, count] : names) {
                    fprintf(table, "k %llx s %s %llu\n", static_cast<unsigned long long>(self), mapping.c_str(),
                            static_cast<unsigned long long>(count));
                }
                continue;
            }
            for (const auto& [key, count] : thread.keys[k]) {
                if (count == 0) {
                    continue;
                }
                if (k == kGame) {
                    const auto size = window.bytes.find(key);
                    fprintf(table, "k %llx g %llx %llu %u\n", static_cast<unsigned long long>(self),
                            static_cast<unsigned long long>(key), static_cast<unsigned long long>(count),
                            size != window.bytes.end() ? size->second : 0);
                } else {
                    fprintf(table, "k %llx %s %llx %llu\n", static_cast<unsigned long long>(self), kKindNames[k],
                            static_cast<unsigned long long>(key), static_cast<unsigned long long>(count));
                }
            }
        }
    }
    fflush(table);
}

// The lists grown out of and those of the JITs given back, freed (the sorting thread reads them: only it frees).
void LetGo(bool all) {
    std::lock_guard<std::mutex> guard(s_mutex);
    for (Blocks* list : s_retired) {
        free(list);
    }
    s_retired.clear();
    for (Region& region : s_regions) {
        const int state = region.state.load(std::memory_order_relaxed);
        if (state == 2 || (all && state == 1)) {
            if (state == 1) {
                s_live.fetch_sub(1, std::memory_order_relaxed);
            }
            free(region.blocks.exchange(nullptr, std::memory_order_relaxed));
            region.count.store(0, std::memory_order_relaxed);
            region.begin.store(0, std::memory_order_relaxed);
            region.state.store(0, std::memory_order_release);
        }
    }
    if (all) {
        s_piece_count = 0;
    }
}

// (Mode 2) The live guest threads running right now, signalled: their CPU clocks read twice, a pass apart - a thread
// whose clock moved between the two is on a core (one that ran a while ago but waits now isn't signalled: its sample
// would land in its wait). Those whose clock isn't known, one in turn. Under the mutex: a thread leaves the list
// under it first.
void SignalRunning() {
    static int next = 0;
    std::lock_guard<std::mutex> guard(s_mutex);
    auto now_of = [](const Thread& thread, uint64_t* ns) {
        timespec ts;
        if (!thread.has_clock || clock_gettime(thread.clock, &ts) != 0) {
            return false;
        }
        *ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
        return true;
    };
    bool turn_taken = false;
    for (Thread& thread : s_threads) {
        uint64_t ns = 0;
        if (thread.live && thread.self != 0 && now_of(thread, &ns)) {
            thread.checked_ns = ns;
        }
    }
    for (int i = 0; i < kThreads; i++) {
        Thread& thread = s_threads[(next + i) % kThreads];
        if (!thread.live || thread.self == 0) {
            continue;
        }
        uint64_t ns = 0;
        if (now_of(thread, &ns)) {
            if (ns > thread.checked_ns) {
                pthread_kill(reinterpret_cast<pthread_t>(thread.self), kSigProf);
            }
        } else if (!turn_taken) {
            turn_taken = true;
            next = (next + i + 1) % kThreads;
            pthread_kill(reinterpret_cast<pthread_t>(thread.self), kSigProf);
        }
    }
}

int SetTimer(bool on) {
    itimerval timer;
    memset(&timer, 0, sizeof(timer));
    if (on) {
        timer.it_interval.tv_usec = 1000;
        timer.it_value.tv_usec = 1000;
    }
    return setitimer(ITIMER_PROF, &timer, nullptr);
}

void* Sorter(void*) {
#if defined(__ORBIS__)
    const char* const table_path = kTableFile;
#else
    const char* const table_path = getenv("EMUSUNI_VITA_PROFILE_FILE");
#endif
    FindText();
    FILE* const table = table_path != nullptr ? fopen(table_path, "a") : nullptr;
    struct {
        int hz, tick, spare, stathz, profhz;  // FreeBSD's struct clockinfo
    } clock = {};
#if defined(__ORBIS__)
    size_t clock_size = sizeof(clock);
    const int clock_known = sysctlbyname("kern.clockrate", &clock, &clock_size, nullptr, 0);
#else
    const int clock_known = -1;
#endif
    int timer_result = 0;
    if (s_mode.load() == 1) {
        timer_result = SetTimer(true);
        if (timer_result != 0) {
            s_mode.store(2);
        }
    }
    const int mode = s_mode.load();
    emusuni_vita_log("vita: profile: on (the file vita-profile-on): %s; the kernel's clock %d Hz (stat %d, prof %d%s); a "
                     "thread's CPU time %s; the app's code %llu KiB; the table in %s",
                     mode == 1 ? "the profiling timer, every thread, a sample a ms of CPU"
                               : (timer_result != 0 ? "the guest threads signalled in turn (the profiling timer refused)"
                                                    : "the guest threads signalled in turn (asked: thread)"),
                     clock.hz, clock.stathz, clock.profhz, clock_known == 0 ? "" : ", not known",
                     s_cpu_clock.load() >= 0 ? "known (a sample weighs its thread's CPU time since its last one)" : "not known",
                     static_cast<unsigned long long>((s_text_end - s_text_start) / 1024), table != nullptr ? table_path : "no file");
    auto write_start = [&]() {
        if (table != nullptr) {
            fprintf(table, "start mode %d hz %d stathz %d profhz %d text %llx\n", s_mode.load(), clock.hz, clock.stathz,
                    clock.profhz, static_cast<unsigned long long>(s_text_end - s_text_start));
            fflush(table);
        }
    };
    write_start();

    Window* window = new Window;
    double start = SecondsNow();
    double window_start = start;
    int number = 0;
    uint64_t seen = 0;
    bool ever_live = false;
    double none_since = start;
    uint32_t random = 0x9E3779B9u;
    for (;;) {
        // 0.5-1.5 ms, at random: at a fixed pace the signals would keep in step with a loop they wake.
        random = random * 1664525u + 1013904223u;
        timespec pause = {0, 500000 + static_cast<long>((random >> 8) % 1000000)};
        nanosleep(&pause, nullptr);  // (a SIGPROF may end it early)
        LetGo(false);
        const bool sampling = s_sampling.load(std::memory_order_relaxed);
        if (s_mode.load(std::memory_order_relaxed) == 2 && sampling) {
            SignalRunning();
        }
        Sample sample;
        if (!sampling) {
            // The game ended: nothing kept until the next game's first block asks again.
            while (Pop(sample)) {
            }
            if (s_rearm.exchange(false, std::memory_order_relaxed)) {
                if (s_mode.load() == 1) {
                    SetTimer(true);
                }
                s_sampling.store(true, std::memory_order_relaxed);
                write_start();
                emusuni_vita_log("vita: profile: a game again: sampling (a new start in the table, its windows from 1)");
                delete window;
                window = new Window;
                start = window_start = SecondsNow();
                number = 0;
                ever_live = false;
                none_since = start;
                s_piece_offsets.clear();
                s_pieces_written = false;
            }
            continue;
        }
        while (Pop(sample)) {
            seen++;
            PerThread& thread = window->threads[sample.self];
            uint64_t weight = kFirstWeightUs;
            if (sample.cpu_ns != 0) {
                uint64_t& last = s_last_cpu_ns[sample.self];
                if (last != 0 && sample.cpu_ns >= last) {
                    const uint64_t ran = sample.cpu_ns - last;
                    weight = std::min(ran / 1000, kMostWeightUs);
                    if (ran < kRanAtLeastNs) {
                        thread.diverted++;
                        window->diverted++;
                    }
                }
                last = sample.cpu_ns;
            }
            uint64_t key = 0;
            uint32_t bytes = 0;
            const Kind kind = Classify(sample.rip, key, bytes);
            thread.kinds[kind] += weight;
            thread.samples++;
            thread.cpus |= 1u << sample.cpu;
            thread.keys[kind][key] += weight;
            if (kind == kGame) {
                window->bytes[key] = bytes;
            }
            window->cpu[sample.cpu] += weight;
            window->total += weight;
            window->samples++;
        }
        const double now = SecondsNow();
        if (s_mode.load(std::memory_order_relaxed) == 1 && seen == 0 && now - start > 3.0) {
            SetTimer(false);
            s_mode.store(2);
            emusuni_vita_log("vita: profile: no SIGPROF from the profiling timer in 3 s: the guest threads are signalled in "
                             "turn instead");
        }
        const int live = s_live.load(std::memory_order_acquire);
        ever_live = ever_live || live > 0;
        if (live > 0 || !ever_live) {
            none_since = now;
        }
        if (now - none_since >= 2.0) {
            // The game ended: every JIT's memory given back, for 2 s (a module's start thread alone isn't the end).
            if (s_mode.load() == 1) {
                SetTimer(false);
            }
            s_sampling.store(false, std::memory_order_relaxed);
            Report(*window, ++number, window_start - start, now - window_start, table);  // (the last part)
            delete window;
            window = new Window;
            LetGo(true);
            emusuni_vita_log("vita: profile: the game ended (its JITs' memory given back): sampling stops until the next game");
            continue;
        }
        if (now - window_start >= 10.0) {
            Report(*window, ++number, window_start - start, now - window_start, table);
            delete window;
            window = new Window;
            window_start = now;
        }
    }
    return nullptr;
}

uint64_t ClockNs(int id) {
    timespec ts;
    if (clock_gettime(static_cast<clockid_t>(id), &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// The calling thread's CPU clock: a 2 ms sleep must count next to nothing on it, 2 ms busy about that.
int FindCpuClock() {
    static const int kCandidates[] = {static_cast<int>(CLOCK_THREAD_CPUTIME_ID), 14};
    for (const int id : kCandidates) {
        const uint64_t c0 = ClockNs(id);
        if (c0 == 0) {
            continue;
        }
        const uint64_t w0 = ClockNs(CLOCK_MONOTONIC);
        usleep(2000);
        const uint64_t w1 = ClockNs(CLOCK_MONOTONIC), c1 = ClockNs(id);
        volatile uint64_t spins = 0;
        while (ClockNs(CLOCK_MONOTONIC) - w1 < 2000000) {
            spins = spins + 1;
        }
        const uint64_t w2 = ClockNs(CLOCK_MONOTONIC), c2 = ClockNs(id);
        const double slept = static_cast<double>(c1 - c0), sleep = static_cast<double>(w1 - w0);
        const double spun = static_cast<double>(c2 - c1), busy = static_cast<double>(w2 - w1);
        if (slept < 0.3 * sleep && spun > 0.5 * busy && spun < 1.5 * busy) {
            return id;
        }
    }
    return -1;
}

bool Start() {
    s_ring = static_cast<Sample*>(malloc(sizeof(Sample) * kRing));
    if (s_ring == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < kRing; i++) {
        new (&s_ring[i].seq) std::atomic<uint64_t>(i);
        s_ring[i].rip = 0;
        s_ring[i].self = 0;
        s_ring[i].cpu_ns = 0;
        s_ring[i].cpu = 0;
    }
    s_cpu_clock.store(FindCpuClock(), std::memory_order_relaxed);
    s_retired.reserve(256);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = OnSample;
    memcpy(&action, &handler, sizeof(handler));  // (the handler union is the first member; its names differ)
    action.sa_flags = kSaSiginfo | kSaRestart | kSaOnstack;
    sigemptyset(&action.sa_mask);
    if (sigaction(kSigProf, &action, nullptr) != 0) {
        emusuni_vita_log("vita: profile: SIGPROF's handler wasn't taken: no profile");
        return false;
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Sorter, nullptr) != 0) {
        emusuni_vita_log("vita: profile: its thread didn't start: no profile");
        return false;
    }
    pthread_detach(thread);
    return true;
}

// A name dynarmic gives a block (A32: "a32_", a or t, the address in 8 hex digits, "_").
bool IsBlockName(const char* name, size_t length) {
    if (length < 14 || memcmp(name, "a32_", 4) != 0 || (name[4] != 'a' && name[4] != 't') || name[13] != '_') {
        return false;
    }
    for (int i = 5; i < 13; i++) {
        if (!isxdigit(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

// dynarmic's emit_x64.cpp at every block it translates, on the guest thread whose JIT it is: the JIT's whole memory
// (from before its prelude: getCode), the block's code [entry, end) and its location (the guest's address in the
// low 32 bits, bit 32 Thumb).
extern "C" void emusuni_vita_prof_block(const void* code_begin, size_t code_size, const void* entry, const void* end,
                                        unsigned long long location) {
    if (!On()) {
        return;
    }
    if (!s_sampling.load(std::memory_order_relaxed)) {
        s_rearm.store(true, std::memory_order_relaxed);  // a game again: the sorting thread starts
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(code_begin);
    const uint32_t offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry) - begin);
    const uint32_t size = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(entry));
    const uint32_t pc = static_cast<uint32_t>(location) | static_cast<uint32_t>((location >> 32) & 1);
    std::lock_guard<std::mutex> guard(s_mutex);
    Region* region = t_region;
    if (region == nullptr || region->state.load(std::memory_order_relaxed) != 1 ||
        region->begin.load(std::memory_order_relaxed) != begin) {
        region = nullptr;
        Region* free_one = nullptr;
        for (Region& at : s_regions) {
            const int state = at.state.load(std::memory_order_relaxed);
            if (state == 1 && at.begin.load(std::memory_order_relaxed) == begin) {
                region = &at;
                break;
            }
            if (state == 0 && free_one == nullptr) {
                free_one = &at;
            }
        }
        if (region == nullptr) {
            Blocks* const list = free_one != nullptr ? NewBlocks(kFirstBlocks) : nullptr;
            if (list == nullptr) {
                s_unplaced.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            region = free_one;
            region->state.store(3, std::memory_order_relaxed);
            region->blocks.store(list, std::memory_order_relaxed);
            region->count.store(0, std::memory_order_relaxed);
            region->last = 0;
            region->resets = 0;
            region->untracked = 0;
            region->size.store(code_size, std::memory_order_relaxed);
            region->begin.store(begin, std::memory_order_relaxed);
            region->state.store(1, std::memory_order_release);
            s_live.fetch_add(1, std::memory_order_release);
        }
        t_region = region;
    }
    uint32_t count = region->count.load(std::memory_order_relaxed);
    if (count > 0 && offset <= region->last) {
        count = 0;  // the cache emptied: its blocks are written from the start again
        region->count.store(0, std::memory_order_release);
        region->resets++;
    }
    region->last = offset;
    Blocks* list = region->blocks.load(std::memory_order_relaxed);
    if (count == list->capacity && list->capacity < kMostBlocks) {
        if (Blocks* const bigger = NewBlocks(list->capacity * 2)) {
            memcpy(bigger->at, list->at, count * sizeof(Block));
            region->blocks.store(bigger, std::memory_order_release);
            s_retired.push_back(list);
            list = bigger;
        }
    }
    if (count < list->capacity) {
        list->at[count] = {offset, size, pc};
        region->count.store(count + 1, std::memory_order_release);
    } else {
        region->untracked++;
    }
}

// dynarmic's perf_map.cpp, for every piece of code it names: its dispatcher, its handlers and the memory accesses'
// slow paths, made with each JIT (the blocks come to emusuni_vita_prof_block). Neighbours of one kind are one piece.
extern "C" void emusuni_vita_prof_piece(const void* start, const void* end, const char* name, size_t length) {
    if (start == end || name == nullptr || !On() || IsBlockName(name, length)) {
        return;
    }
    // (The name without its last "_<size>": a32_read_fallback_32 and a32_read_fallback_64 are one kind.)
    size_t kind = length;
    for (size_t i = length; i > 0; i--) {
        if (name[i - 1] == '_') {
            bool digits = i < length;
            for (size_t j = i; j < length; j++) {
                digits = digits && isdigit(static_cast<unsigned char>(name[j]));
            }
            if (digits) {
                kind = i - 1;
            }
            break;
        }
    }
    // (The memory accesses' slow paths - read, write and exclusive write, one of each for every pair of registers and
    // size, ~6000 a JIT, one after the other - are one piece: "a32_memory_fallbacks".)
    static const char kFallbacks[] = "a32_memory_fallbacks";
    if (kind >= 9 && memcmp(name + kind - 9, "_fallback", 9) == 0) {
        name = kFallbacks;
        kind = sizeof(kFallbacks) - 1;
    }
    kind = std::min(kind, sizeof(Piece::name) - 1);
    const uintptr_t from = reinterpret_cast<uintptr_t>(start), to = reinterpret_cast<uintptr_t>(end);
    std::lock_guard<std::mutex> guard(s_mutex);
    if (s_piece_count > 0) {
        Piece& last = s_pieces[s_piece_count - 1];
        if (from >= last.end && from - last.end <= 64 && strlen(last.name) == kind && memcmp(last.name, name, kind) == 0) {
            last.end = to;
            return;
        }
    }
    if (s_piece_count == kPieces) {
        return;
    }
    Piece& piece = s_pieces[s_piece_count++];
    piece.start = from;
    piece.end = to;
    memcpy(piece.name, name, kind);
    piece.name[kind] = 0;
}

// dynarmic's block_of_code.cpp: a JIT's memory given back (its guest thread ended, or the game).
extern "C" void emusuni_vita_prof_free(const void* code_begin) {
    if (s_state.load(std::memory_order_acquire) != 1) {
        return;
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(code_begin);
    std::lock_guard<std::mutex> guard(s_mutex);
    for (Region& region : s_regions) {
        if (region.state.load(std::memory_order_relaxed) == 1 && region.begin.load(std::memory_order_relaxed) == begin) {
            region.state.store(2, std::memory_order_release);
            s_live.fetch_sub(1, std::memory_order_release);
        }
    }
}

// The core's kernel (thread.cpp's run_loop): a guest thread's loop starts (how 1: its name) or ends (how -1), on
// the thread itself; a callback's loop inside a call (how 0) changes nothing.
extern "C" void emusuni_vita_prof_thread(const char* name, int id, int how) {
    if (how == 0 || (how < 0 && s_state.load(std::memory_order_acquire) != 1) || (how > 0 && !On())) {
        return;
    }
    const uint64_t self = reinterpret_cast<uint64_t>(pthread_self());
    if (how > 0) {
        // (On the PC bench the guest threads have SIGPROF blocked - 0x0e007007, the frontend's, inherited from the
        // thread that made them -: the timer's signal went to the threads that let it in, and none of the game's code
        // was sampled. Let in here, on the guest thread itself, whatever the console's threads have.)
        sigset_t profiling;
        sigemptyset(&profiling);
        sigaddset(&profiling, kSigProf);
        pthread_sigmask(kSigUnblock, &profiling, nullptr);
    }
    std::lock_guard<std::mutex> guard(s_mutex);
    if (how < 0) {
        for (Thread& thread : s_threads) {
            if (thread.self == self) {
                thread.live = false;
            }
        }
        return;
    }
    Thread* slot = nullptr;
    for (Thread& thread : s_threads) {
        if (thread.self == self) {
            slot = &thread;
            break;
        }
    }
    for (int pass = 0; pass < 2 && slot == nullptr; pass++) {
        for (Thread& thread : s_threads) {
            if (!thread.live && (pass == 1 || thread.self == 0)) {
                slot = &thread;
                break;
            }
        }
    }
    if (slot == nullptr) {
        return;
    }
    snprintf(slot->name, sizeof(slot->name), "%s", name != nullptr && name[0] != 0 ? name : "?");
    slot->id = id;
    slot->self = self;
    slot->live = true;
    slot->has_clock = pthread_getcpuclockid(pthread_self(), &slot->clock) == 0;
    slot->checked_ns = 0;
    s_threads_changed.fetch_add(1, std::memory_order_release);
}
