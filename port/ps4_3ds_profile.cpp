// The 3DS emulation's CPU time on the console, sampled (01.41, a test switch: the file /data/EmuSuNi/3ds-cpu-profile).
//
// The performance specialist's JIT study (rendimiento-jit-estudio.md, its step 2): on the PC, perf and dynarmic's
// block map say where the 3DS core's time goes, but the console runs the game's translated code 13-16 times slower
// than the PC and the C++ 3-8 times, so its split can only be measured on it. No hardware counter here: the profiling
// timer (setitimer ITIMER_PROF, 1 ms of the process's CPU time) sends SIGPROF to the thread that is running, whose
// handler keeps the instruction address and the CPU; a thread of this file (cores 4 and 5) sorts the samples into
//   the game's code    a block dynarmic translated (the core tells where each is: emusuni_3ds_jit_block, below)
//   dynarmic's own     the JIT's memory outside every block (its dispatcher, the slow paths of memory accesses...)
//   the app's C++      the eboot's code: the app, the cores, the C library (kept as ELF addresses)
//   the system         anywhere else (libkernel's calls, the shader JIT of the GPU commands...), by mapping name
// and every 10 s of play writes three lines to the log (the emulation thread's split, its busiest blocks, the other
// threads by core) and the window's whole table to /data/EmuSuNi/3ds-profile.txt, for the specialist's script (the
// ELF addresses symbolized with the build's ELF, the game's code by guest address).
// If no SIGPROF comes within 3 s (a kernel without the profiling timer), the sorting thread signals the emulation
// thread itself every 1 ms instead (pthread_kill: only that thread; the word "thread" in the file asks for it).
// Without the file the core's hook returns at once (it looks for the file again every ~10 s of block compiles).
// (01.48) Sampling stops when the 3DS game ends: once a second the sorting thread looks whether the JIT's memory is
// still mapped; when none is (the app gave it back), the timer stops, the window so far is written and the blocks are
// forgotten, until the next 3DS game's first block starts it again (a new "start" line in the table, its windows
// numbered from 1). Before, the timer went on through the menu and the other cores.
// On the PC (the bench): the variable EMUSUNI_3DS_CPU_PROFILE (1 or thread) and EMUSUNI_3DS_CPU_PROFILE_FILE.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <x86intrin.h>

#if !defined(__ORBIS__)
#include <dlfcn.h>
#include <sched.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

extern "C" void emusuni_log_line(const char* text);

namespace {

#if defined(__ORBIS__)
// FreeBSD's values (the PS4 kernel's), as port/ps4_crashlog.cpp has them; the registers 0x40 bytes into the context,
// rip the 21st 8-byte slot (measured on hardware).
constexpr int kSigProf = 27, kSaSiginfo = 0x40, kSaRestart = 0x2, kSaOnstack = 0x1;
constexpr int kMcontextOffset = 0x40, kRipIndex = 20;
static_assert(sizeof(sigset_t) == 16, "struct sigaction isn't FreeBSD's: the libc overlay's headers must come first");
extern "C" char __text_start[];  // the OpenOrbis link.x: the start of .text (ELF address 0)
extern "C" int sceKernelGetCurrentCpu(void);
extern "C" int32_t sceKernelVirtualQuery(const void* addr, int flags, void* info, size_t info_size);
extern "C" void* emusuni_profile_self(void) __asm__("scePthreadSelf");
extern "C" int emusuni_profile_affinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
extern "C" int sysctlbyname(const char* name, void* old, size_t* old_size, const void* value, size_t size);
// libkernel's SceKernelVirtualQueryInfo (as port/ps4_crashlog.cpp has it).
struct VirtualQueryInfo {
    uintptr_t start, end;
    int64_t offset;
    int32_t protection, memory_type;
    uint32_t flags;
    char name[32];
};
constexpr int32_t kProtCpuExec = 0x4;
const char* const kSwitchFile = "/data/EmuSuNi/3ds-cpu-profile";
const char* const kTableFile = "/data/EmuSuNi/3ds-profile.txt";
uint64_t ripOf(void* context) {
    return reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset)[kRipIndex];
}
int cpuNow() { return sceKernelGetCurrentCpu(); }
#else
constexpr int kSigProf = SIGPROF, kSaSiginfo = SA_SIGINFO, kSaRestart = SA_RESTART, kSaOnstack = SA_ONSTACK;
extern "C" char __executable_start[];
extern "C" char etext[];
uint64_t ripOf(void* context) {
    return static_cast<uint64_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
}
int cpuNow() { return sched_getcpu(); }
#endif

// ---- The JIT's blocks, as the core reports them (its emulation thread writes, the sorting thread reads) ----
struct Block {
    uint32_t offset, size;  // from the region's start
    uint32_t pc;            // the guest's address, bit 0 set for Thumb
};
struct Region {
    std::atomic<uintptr_t> begin{0};
    size_t size = 0;
    Block* blocks = nullptr;
    uint32_t capacity = 0;
    std::atomic<uint32_t> count{0};  // published after the block is written
    uint32_t last = 0;               // the last block's offset: one below it starts again (the cache emptied)
    std::atomic<uint32_t> resets{0}, untracked{0};
};
constexpr int kRegions = 8;
Region g_regions[kRegions];
std::atomic<int> g_regionCount{0};
std::atomic_flag g_hookLock = ATOMIC_FLAG_INIT;
std::atomic<uintptr_t> g_emulation{0};  // the thread that compiles blocks (retro_run's)

// ---- The samples: the signal handler (any thread) to the sorting thread, without a lock ----
struct Sample {
    std::atomic<uint64_t> seq;
    uint64_t rip;
    uint32_t info;  // the CPU, kEmulationBit for the emulation thread
};
constexpr uint32_t kRing = 8192, kEmulationBit = 0x100;
Sample* g_ring = nullptr;
std::atomic<uint64_t> g_head{0};
uint64_t g_tail = 0;
std::atomic<uint32_t> g_dropped{0};
std::atomic<uint64_t> g_signals{0};
// (01.48) False once the 3DS game ended (the sorting thread stops the timer); the core's hook asks for it again.
std::atomic<bool> g_sampling{true};
std::atomic<bool> g_rearm{false};

uintptr_t selfId() { return (uintptr_t)pthread_self(); }

void push(uint64_t rip, uint32_t info) {
    uint64_t pos = g_head.load(std::memory_order_relaxed);
    for (;;) {
        Sample& slot = g_ring[pos & (kRing - 1)];
        const int64_t ahead = static_cast<int64_t>(slot.seq.load(std::memory_order_acquire) - pos);
        if (ahead == 0) {
            if (g_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                slot.rip = rip;
                slot.info = info;
                slot.seq.store(pos + 1, std::memory_order_release);
                return;
            }
        } else if (ahead < 0) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);  // full: the sorting thread is behind
            return;
        } else {
            pos = g_head.load(std::memory_order_relaxed);
        }
    }
}

bool pop(uint64_t& rip, uint32_t& info) {
    Sample& slot = g_ring[g_tail & (kRing - 1)];
    if (slot.seq.load(std::memory_order_acquire) != g_tail + 1)
        return false;
    rip = slot.rip;
    info = slot.info;
    slot.seq.store(g_tail + kRing, std::memory_order_release);
    g_tail++;
    return true;
}

void onSample(int, siginfo_t*, void* context) {
    const int saved = errno;
    g_signals.fetch_add(1, std::memory_order_relaxed);
    int cpu = cpuNow();
    if (cpu < 0 || cpu > 15)
        cpu = 15;
    const bool emulation = selfId() == g_emulation.load(std::memory_order_relaxed);
    push(ripOf(context), static_cast<uint32_t>(cpu) | (emulation ? kEmulationBit : 0));
    errno = saved;
}

// ---- The switch ----
// 0: off, 1: the profiling timer (all threads), 2: the emulation thread signalled by the sorting thread.
int switchMode() {
#if defined(__ORBIS__)
    FILE* file = fopen(kSwitchFile, "r");
    if (!file)
        return 0;
    char word[16] = {};
    const size_t got = fread(word, 1, sizeof(word) - 1, file);
    fclose(file);
    word[got] = 0;
    return strncmp(word, "thread", 6) == 0 ? 2 : 1;
#else
    const char* value = getenv("EMUSUNI_3DS_CPU_PROFILE");
    if (!value || !*value || strcmp(value, "0") == 0 || strcmp(value, "off") == 0)
        return 0;
    return strcmp(value, "thread") == 0 ? 2 : 1;
#endif
}

// ---- The sorting thread ----
enum Kind { kGame, kDynarmic, kApp, kSystem, kKinds };
const char* const kKindNames[kKinds] = {"g", "d", "a", "s"};

struct Window {
    uint32_t emulation[kKinds] = {}, other[kKinds] = {};
    uint32_t cpu[16] = {}, emulationCpu[16] = {};
    std::unordered_map<uint64_t, uint32_t> keys[2][kKinds];  // [emulation thread?][kind]: key -> samples
};

uintptr_t g_textStart = 0, g_textEnd = 0;
std::unordered_map<uint64_t, std::string> g_pageNames;  // the system's pages (4 KiB) by mapping name
int g_mode = 0;

void findText() {
#if defined(__ORBIS__)
    g_textStart = reinterpret_cast<uintptr_t>(__text_start);
    g_textEnd = g_textStart + 0x4000000;  // (port/ps4_crashlog.cpp's window if the kernel won't say)
    VirtualQueryInfo info;
    uintptr_t at = g_textStart, end = 0;
    // The executable mappings from .text's start on, one after the other (the eboot's .text is ~93 MiB).
    for (int i = 0; i < 16; i++) {
        memset(&info, 0, sizeof(info));
        if (sceKernelVirtualQuery(reinterpret_cast<const void*>(at), 0, &info, sizeof(info)) != 0 ||
            info.start > at || info.end <= at || !(info.protection & kProtCpuExec))
            break;
        end = info.end;
        at = info.end;
    }
    if (end > g_textStart)
        g_textEnd = end;
#else
    g_textStart = reinterpret_cast<uintptr_t>(__executable_start);
    g_textEnd = reinterpret_cast<uintptr_t>(etext);
#endif
}

std::string pageName(uint64_t page) {
    auto found = g_pageNames.find(page);
    if (found != g_pageNames.end())
        return found->second;
    std::string name = "?";
#if defined(__ORBIS__)
    VirtualQueryInfo info;
    memset(&info, 0, sizeof(info));
    if (sceKernelVirtualQuery(reinterpret_cast<const void*>(page << 12), 0, &info, sizeof(info)) == 0) {
        info.name[sizeof(info.name) - 1] = 0;
        name = info.name[0] ? info.name : "anonymous";
    }
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(page << 12), &info) && info.dli_fname) {
        name = info.dli_fname;
        const size_t slash = name.rfind('/');
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
    }
#endif
    for (char& c : name)
        if (c == ' ' || c < 0x20)
            c = '_';
    g_pageNames.emplace(page, name);
    return name;
}

Kind classify(uint64_t rip, uint64_t& key) {
    if (rip >= g_textStart && rip < g_textEnd) {
        key = rip - g_textStart;
        return kApp;
    }
    const int regions = g_regionCount.load(std::memory_order_acquire);
    for (int r = 0; r < regions; r++) {
        Region& region = g_regions[r];
        const uintptr_t begin = region.begin.load(std::memory_order_acquire);
        if (rip < begin || rip - begin >= region.size)
            continue;
        const uint32_t offset = static_cast<uint32_t>(rip - begin);
        const uint32_t count = region.count.load(std::memory_order_acquire);
        // The last block that starts at or before the address (offsets rise until the cache empties).
        const Block* first = region.blocks;
        const Block* last = region.blocks + count;
        const Block* after = std::upper_bound(first, last, offset, [](uint32_t value, const Block& block) {
            return value < block.offset;
        });
        if (after != first && offset - after[-1].offset < after[-1].size) {
            key = after[-1].pc;
            return kGame;
        }
        key = (static_cast<uint64_t>(r) << 32) | (offset & ~63u);
        return kDynarmic;
    }
    key = rip >> 12;
    return kSystem;
}

double secondsNow() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

// How many of the most sampled keys hold a share of the samples.
uint32_t keysFor(const std::vector<uint32_t>& sorted, uint64_t total, double share) {
    uint64_t sum = 0;
    for (size_t i = 0; i < sorted.size(); i++) {
        sum += sorted[i];
        if (static_cast<double>(sum) >= share * static_cast<double>(total))
            return static_cast<uint32_t>(i + 1);
    }
    return static_cast<uint32_t>(sorted.size());
}

void report(Window& window, int number, double from, double length, FILE* table) {
    uint64_t emulation = 0, other = 0;
    for (int k = 0; k < kKinds; k++) {
        emulation += window.emulation[k];
        other += window.other[k];
    }
    if (emulation + other == 0)
        return;
    auto per = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * static_cast<double>(part) / whole : 0.0; };
    std::vector<std::pair<uint64_t, uint32_t>> blocks(window.keys[1][kGame].begin(), window.keys[1][kGame].end());
    std::sort(blocks.begin(), blocks.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<uint32_t> counts;
    for (const auto& block : blocks)
        counts.push_back(block.second);
    // The system's samples of the emulation thread by mapping name.
    std::unordered_map<std::string, uint32_t> systemNames;
    for (const auto& page : window.keys[1][kSystem])
        systemNames[pageName(page.first)] += page.second;
    std::vector<std::pair<std::string, uint32_t>> systems(systemNames.begin(), systemNames.end());
    std::sort(systems.begin(), systems.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    char line[512];
    int n = snprintf(line, sizeof(line),
                     "3ds-cpu-profile: %.1f s at %.0f s, the emulation thread %llu samples (%.0f a second): the game's "
                     "code %.1f%% (%zu blocks; half of it in %u, 90%% in %u), dynarmic's own %.1f%%, the app's C++ "
                     "%.1f%%, the system %.1f%%",
                     length, from, static_cast<unsigned long long>(emulation), emulation / length,
                     per(window.emulation[kGame], emulation), blocks.size(),
                     keysFor(counts, window.emulation[kGame], 0.5), keysFor(counts, window.emulation[kGame], 0.9),
                     per(window.emulation[kDynarmic], emulation), per(window.emulation[kApp], emulation),
                     per(window.emulation[kSystem], emulation));
    for (size_t i = 0; i < systems.size() && i < 2 && n < 440; i++)
        n += snprintf(line + n, sizeof(line) - n, "%s%s %.1f", i ? ", " : " (", systems[i].first.c_str(),
                      per(systems[i].second, emulation));
    if (!systems.empty() && n < 500)
        n += snprintf(line + n, sizeof(line) - n, ")");
    emusuni_log_line(line);

    n = snprintf(line, sizeof(line), "3ds-cpu-profile: the busiest blocks (guest address, %% of the thread):");
    for (size_t i = 0; i < blocks.size() && i < 12 && n < 470; i++)
        n += snprintf(line + n, sizeof(line) - n, " %08llX%s %.1f",
                      static_cast<unsigned long long>(blocks[i].first & ~1ull), (blocks[i].first & 1) ? "t" : "",
                      per(blocks[i].second, emulation));
    emusuni_log_line(line);

    n = snprintf(line, sizeof(line),
                 "3ds-cpu-profile: the other threads %llu samples (the app's C++ %.1f%%, the system %.1f%%, "
                 "JIT %.1f%%); "
                 "all threads by core (ms of CPU, 1 a sample):",
                 static_cast<unsigned long long>(other), per(window.other[kApp], other),
                 per(window.other[kSystem], other), per(window.other[kGame] + window.other[kDynarmic], other));
    for (int c = 0; c < 16 && n < 480; c++)
        if (window.cpu[c])
            n += snprintf(line + n, sizeof(line) - n, " %d: %u", c, window.cpu[c]);
    const uint32_t dropped = g_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped && n < 480)
        n += snprintf(line + n, sizeof(line) - n, "; %u lost", dropped);
    emusuni_log_line(line);

    if (!table)
        return;
    fprintf(table, "window %d %.3f %.3f mode %d emulation %llu", number, from, length, g_mode,
            static_cast<unsigned long long>(emulation));
    for (int k = 0; k < kKinds; k++)
        fprintf(table, " %s %u", kKindNames[k], window.emulation[k]);
    fprintf(table, " other %llu", static_cast<unsigned long long>(other));
    for (int k = 0; k < kKinds; k++)
        fprintf(table, " %s %u", kKindNames[k], window.other[k]);
    fprintf(table, " text %llx dropped %u\n", static_cast<unsigned long long>(g_textEnd - g_textStart), dropped);
    for (int c = 0; c < 16; c++)
        if (window.cpu[c])
            fprintf(table, "cpu %d %u %u\n", c, window.cpu[c], window.emulationCpu[c]);
    for (int e = 1; e >= 0; e--) {
        for (int k = 0; k < kKinds; k++) {
            if (k == kSystem) {
                std::unordered_map<std::string, uint32_t> names;
                for (const auto& page : window.keys[e][k])
                    names[pageName(page.first)] += page.second;
                for (const auto& name : names)
                    fprintf(table, "%c%s %s %u\n", e ? 'e' : 'o', kKindNames[k], name.first.c_str(), name.second);
                continue;
            }
            for (const auto& key : window.keys[e][k])
                fprintf(table, "%c%s %llx %u\n", e ? 'e' : 'o', kKindNames[k],
                        static_cast<unsigned long long>(key.first), key.second);
        }
    }
    fflush(table);
}

// (01.48) Whether an address is mapped still (the JIT's memory: the app gives it back when the 3DS game ends).
bool mapped(uintptr_t at) {
#if defined(__ORBIS__)
    VirtualQueryInfo info;
    memset(&info, 0, sizeof(info));
    return sceKernelVirtualQuery(reinterpret_cast<const void*>(at), 0, &info, sizeof(info)) == 0 && info.start <= at &&
           at < info.end;
#else
    return msync(reinterpret_cast<void*>(at & ~static_cast<uintptr_t>(4095)), 4096, MS_ASYNC) == 0 || errno != ENOMEM;
#endif
}

// (01.48) The ended game's blocks forgotten (under the hook's lock: the next game registers its own).
void forgetRegions() {
    while (g_hookLock.test_and_set(std::memory_order_acquire)) {
    }
    const int regions = g_regionCount.load(std::memory_order_relaxed);
    g_regionCount.store(0, std::memory_order_release);
    for (int r = 0; r < regions; r++) {
        Region& region = g_regions[r];
        region.begin.store(0, std::memory_order_relaxed);
        region.count.store(0, std::memory_order_relaxed);
        free(region.blocks);
        region.blocks = nullptr;
        region.capacity = 0;
        region.size = 0;
        region.last = 0;
        region.resets.store(0, std::memory_order_relaxed);
        region.untracked.store(0, std::memory_order_relaxed);
    }
    g_emulation.store(0, std::memory_order_relaxed);
    g_hookLock.clear(std::memory_order_release);
}

int setTimer(bool on) {
    itimerval timer;
    memset(&timer, 0, sizeof(timer));
    if (on) {
        timer.it_interval.tv_usec = 1000;
        timer.it_value.tv_usec = 1000;
    }
    return setitimer(ITIMER_PROF, &timer, nullptr);
}

void* sorter(void*) {
#if defined(__ORBIS__)
    emusuni_profile_affinity(emusuni_profile_self(), (1ull << 4) | (1ull << 5));
    const char* tablePath = kTableFile;
#else
    const char* tablePath = getenv("EMUSUNI_3DS_CPU_PROFILE_FILE");
#endif
    findText();
    FILE* table = tablePath ? fopen(tablePath, "a") : nullptr;
    struct {
        int hz, tick, spare, stathz, profhz;  // FreeBSD's struct clockinfo
    } clock = {};
#if defined(__ORBIS__)
    size_t clockSize = sizeof(clock);
    const int clockKnown = sysctlbyname("kern.clockrate", &clock, &clockSize, nullptr, 0);
#else
    const int clockKnown = -1;
#endif
    int timerResult = 0;
    if (g_mode == 1) {
        timerResult = setTimer(true);
        if (timerResult != 0)
            g_mode = 2;
    }
    char line[400];
    snprintf(line, sizeof(line),
             "3ds-cpu-profile: on (01.41, the file 3ds-cpu-profile): %s; the kernel's clock %d Hz "
             "(stat %d, prof %d%s); "
             "the eboot's code %llu KiB; the table in %s",
             g_mode == 1 ? "the profiling timer, every thread, 1 sample a ms of CPU"
                         : (timerResult != 0 ? "the emulation thread signalled every ms (the profiling timer refused)"
                                             : "the emulation thread signalled every ms (asked: thread)"),
             clock.hz, clock.stathz, clock.profhz, clockKnown == 0 ? "" : ", not known",
             static_cast<unsigned long long>((g_textEnd - g_textStart) / 1024), table ? tablePath : "no file");
    emusuni_log_line(line);
    auto writeStart = [&]() {
        if (table)
            fprintf(table, "start mode %d hz %d stathz %d profhz %d text %llx\n", g_mode, clock.hz, clock.stathz,
                    clock.profhz, static_cast<unsigned long long>(g_textEnd - g_textStart));
    };
    writeStart();

    Window* window = new Window;
    double start = secondsNow();
    double windowStart = start;
    double lastCheck = start;
    int number = 0;
    uint64_t seen = 0;
    uint32_t random = 0x9E3779B9u;
    for (;;) {
        // 0.5-1.5 ms, at random: signals at a fixed pace would keep in step with the emulation's own (a wait the
        // signal ends starts its loop again) and sample the same places over and over (the PC test: 33/33/33 %).
        random = random * 1664525u + 1013904223u;
        timespec pause = {0, 500000 + static_cast<long>((random >> 8) % 1000000)};
        nanosleep(&pause, nullptr);  // (a SIGPROF may end it early)
        const bool sampling = g_sampling.load(std::memory_order_relaxed);
        if (g_mode == 2 && sampling) {
            const uintptr_t emulation = g_emulation.load(std::memory_order_relaxed);
            if (emulation)
                pthread_kill((pthread_t)emulation, kSigProf);
        }
        uint64_t rip;
        uint32_t info;
        if (!sampling) {
            // (01.48) The 3DS game ended: nothing sampled until the next game's first block asks again.
            while (pop(rip, info)) {
            }
            if (g_rearm.exchange(false, std::memory_order_relaxed)) {
                if (g_mode == 1)
                    setTimer(true);
                g_sampling.store(true, std::memory_order_relaxed);
                writeStart();
                emusuni_log_line("3ds-cpu-profile: a 3DS game again: sampling (a new start in the table, its "
                                 "windows from 1)");
                delete window;
                window = new Window;
                start = windowStart = lastCheck = secondsNow();
                number = 0;
            }
            continue;
        }
        while (pop(rip, info)) {
            seen++;
            uint64_t key = 0;
            const Kind kind = classify(rip, key);
            const bool emulation = (info & kEmulationBit) != 0;
            const uint32_t cpu = info & 15;
            (emulation ? window->emulation : window->other)[kind]++;
            window->cpu[cpu]++;
            if (emulation)
                window->emulationCpu[cpu]++;
            window->keys[emulation ? 1 : 0][kind][key]++;
        }
        const double now = secondsNow();
        if (g_mode == 1 && seen == 0 && now - start > 3.0) {
            setTimer(false);
            g_mode = 2;
            emusuni_log_line("3ds-cpu-profile: no SIGPROF from the profiling timer in 3 s: the emulation thread is "
                             "signalled every ms instead");
        }
        if (now - lastCheck >= 1.0) {
            // (01.48) The 3DS game ended when none of its JIT's memory is mapped any more.
            lastCheck = now;
            const int regions = g_regionCount.load(std::memory_order_acquire);
            bool any = false;
            for (int r = 0; r < regions && !any; r++)
                any = mapped(g_regions[r].begin.load(std::memory_order_acquire));
            if (regions > 0 && !any) {
                if (g_mode == 1)
                    setTimer(false);
                g_sampling.store(false, std::memory_order_relaxed);
                report(*window, ++number, windowStart - start, now - windowStart, table);  // (the last part)
                delete window;
                window = new Window;
                forgetRegions();
                emusuni_log_line("3ds-cpu-profile: the 3DS game ended (its JIT memory given back): sampling stops "
                                 "until the next 3DS game");
                continue;
            }
        }
        if (now - windowStart >= 10.0) {
            report(*window, ++number, windowStart - start, now - windowStart, table);
            delete window;
            window = new Window;
            windowStart = now;
        }
    }
    return nullptr;
}

void start() {
    g_ring = static_cast<Sample*>(malloc(sizeof(Sample) * kRing));
    if (!g_ring)
        return;
    for (uint32_t i = 0; i < kRing; i++) {
        new (&g_ring[i].seq) std::atomic<uint64_t>(i);
        g_ring[i].rip = 0;
        g_ring[i].info = 0;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = onSample;
    memcpy(&action, &handler, sizeof(handler));  // (the handler union is the first member; its names differ)
    action.sa_flags = kSaSiginfo | kSaRestart | kSaOnstack;
    sigemptyset(&action.sa_mask);
    if (sigaction(kSigProf, &action, nullptr) != 0) {
        emusuni_log_line("3ds-cpu-profile: SIGPROF's handler wasn't taken: no profile");
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, sorter, nullptr) != 0) {
        emusuni_log_line("3ds-cpu-profile: its thread didn't start: no profile");
        return;
    }
    pthread_detach(thread);
}

}  // namespace

// dynarmic's emit_x64.cpp (scripts/build-3ds-core.sh, on the PS4) at every block it translates, on the emulation
// thread: the JIT's memory (code_begin, code_size), the block's code [entry, end) and its location (the guest's
// address in the low 32 bits, bit 32 Thumb).
extern "C" void emusuni_mem_probe_start(const void* jit_begin, size_t jit_size);  // port/ps4_mem_probe.cpp

extern "C" void emusuni_3ds_jit_block(const void* code_begin, size_t code_size, const void* entry, const void* end,
                                      uint64_t location) {
    static bool s_probed = false;  // (01.50) the memory probe, once, at the first 3DS game's first block
    if (!s_probed) {
        s_probed = true;
        emusuni_mem_probe_start(code_begin, code_size);
    }
    static int s_state = 0;          // 0 not looked yet, 1 on, -1 off
    static uint64_t s_lookedAt = 0;  // the time stamp counter when the switch was last looked at
    if (s_state < 0) {
        // (Off: looked at again after ~10 s - 16e9 ticks at the PS4's 1.6 GHz - for a file made meanwhile.)
        if (__rdtsc() - s_lookedAt < 16000000000ull)
            return;
    }
    if (s_state <= 0) {
        s_lookedAt = __rdtsc();
        const int mode = switchMode();
        if (mode == 0) {
            s_state = -1;
            return;
        }
        g_mode = mode;
        s_state = 1;
        start();
    }
    if (!g_sampling.load(std::memory_order_relaxed))
        g_rearm.store(true, std::memory_order_relaxed);  // (01.48) a 3DS game again: the sorting thread starts
    while (g_hookLock.test_and_set(std::memory_order_acquire)) {
    }
    g_emulation.store(selfId(), std::memory_order_relaxed);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(code_begin);
    Region* region = nullptr;
    const int regions = g_regionCount.load(std::memory_order_relaxed);
    for (int r = 0; r < regions; r++)
        if (g_regions[r].begin.load(std::memory_order_relaxed) == begin)
            region = &g_regions[r];
    if (!region && regions < kRegions) {
        region = &g_regions[regions];
        // Room for a block every 256 bytes of the JIT's memory (blocks take ~300 near the start).
        const size_t capacity = std::min<size_t>(std::max<size_t>(code_size / 256, 4096), 1u << 19);
        region->blocks = static_cast<Block*>(malloc(capacity * sizeof(Block)));
        region->capacity = region->blocks ? static_cast<uint32_t>(capacity) : 0;
        region->size = code_size;
        region->begin.store(begin, std::memory_order_release);
        g_regionCount.store(regions + 1, std::memory_order_release);
    }
    if (region) {
        const uintptr_t at = reinterpret_cast<uintptr_t>(entry);
        const uint32_t offset = static_cast<uint32_t>(at - begin);
        uint32_t count = region->count.load(std::memory_order_relaxed);
        if (count > 0 && offset <= region->last) {
            count = 0;  // the cache emptied: its blocks are written from the start again
            region->count.store(0, std::memory_order_release);
            region->resets.fetch_add(1, std::memory_order_relaxed);
        }
        region->last = offset;
        if (count < region->capacity) {
            region->blocks[count] = {offset, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(end) - at),
                                     static_cast<uint32_t>(location) | static_cast<uint32_t>((location >> 32) & 1)};
            region->count.store(count + 1, std::memory_order_release);
        } else {
            region->untracked.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_hookLock.clear(std::memory_order_release);
}
