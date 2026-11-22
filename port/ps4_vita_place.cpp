// EmuSuNi's PS Vita port (19-11, the performance specialist's; for the Vita specialist to take in): where the game's
// threads run on the PS4 - the switch "vitaplace" (the kernel's side: scripts/build-vita-core.sh, thread.cpp's run_loop
// calls emusuni_vita_place_tick after each run of recompiled code).
//
// Why: every thread of the Vita's floats on the PS4's 7 cores (the log's "cores 0x7f"), at the same priority, and the
// PS4's scheduler has fixed priorities. The busiest ones - Uncharted's main thread at 98% of a core, God of War's main
// and "Render", Rayman's main - share their cores with every other thread of the game's and of the emulator's.
//
// What: each guest thread adds its time in recompiled code (the stats' clock, rdtsc) to its slot. Every 2 s the first
// thread past the window's end looks: the busiest threads at 50% of a core or more (at most as many as the dedicated
// cores, 2 by default: PS4 cores 2 and 3) get a core of their own, kept while they stay above 30%; every other thread of
// the game's is kept off those cores. Each thread sets its own affinity, at its next return from recompiled code after a
// change (no system call otherwise). A slot idle for 3 looks is given back (64 slots; threads come and go). With
// the file's text "prio" the dedicated threads also go up to the PS4 priority 600 (the default is 700; the emulation
// thread 480, the sound thread 400) and back to 700 when they lose their core.
//
// Switch: its own, off by default: the file /data/EmuSuNi/vita-place-on (the PC: EMUSUNI_VITA_PLACE=1, or =prio), read
// once, at the first guest thread's first run. Its text may say "prio", and "cores=2,3" (up to 4 cores, 0-6). Lines
// "vita: place: ..." in the log say each change. On the PC, EMUSUNI_VITA_PLACE_MIN=<percent> lowers the 50% (the bench's
// threads are seldom that busy: its GPU is a software one).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>
#include <x86intrin.h>

#include "ps4_vita_sys.h"

#if defined(__ORBIS__)
extern "C" int emusuni_place_affinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
extern "C" int emusuni_place_prio(void* thread, int priority) __asm__("scePthreadSetprio");
extern "C" void* emusuni_place_self(void) __asm__("scePthreadSelf");
#else
#include <sched.h>
#endif

namespace {

constexpr int kSlots = 64;
constexpr int kMaxCores = 4;
constexpr uint64_t kAllCores = 0x7f;  // the PS4's 7 cores of the game's

// (one cache line each: every thread adds to its own slot at each return from recompiled code)
struct alignas(64) Slot {
    std::atomic<int> id{0};           // 0: free
    std::atomic<uint64_t> ticks{0};   // recompiled code in this window
    std::atomic<int> core{-1};        // its own core, or -1
    int idle = 0;                     // windows in a row with no recompiled code (only the look touches it)
    char name[32] = "";               // (copied: the thread's string may go)
    std::atomic<bool> named{false};
};
Slot s_slots[kSlots];
constexpr int kIdleWindows = 3;  // a slot idle this long is given back (its thread ended, or sleeps: it takes one again)
constexpr int kMaxLines = 400;   // the log's "vita: place:" lines, at most, per run of the app

std::atomic<int> s_state{0};  // 0 not read, 1 off, 2 on
bool s_prio = false;
int s_cores[kMaxCores] = {2, 3, -1, -1};
int s_core_count = 2;
double s_min = 0.50, s_keep = 0.30;
std::atomic<uint64_t> s_window_from{0};
std::atomic<uint32_t> s_generation{1};
std::atomic<uint64_t> s_dedicated{0};       // the mask of the cores given away
std::atomic<double> s_ticks_per_s{0};       // (measured over the first window)
uint64_t s_mono_from = 0;
int s_lines = 0;                            // (only the look writes lines)

bool LineLeft() {
    if (s_lines >= kMaxLines) {
        return false;
    }
    if (++s_lines == kMaxLines) {
        emusuni_vita_log("vita: place: %d lines; no more of them in this run of the app", kMaxLines);
        return false;
    }
    return true;
}

uint64_t MonoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void ReadSwitch() {
    char text[128] = "";
    bool on = false;
#if defined(__ORBIS__)
    if (FILE* const f = fopen("/data/EmuSuNi/vita-place-on", "r")) {
        on = true;
        const size_t n = fread(text, 1, sizeof(text) - 1, f);
        text[n] = '\0';
        fclose(f);
    }
#else
    if (const char* const value = getenv("EMUSUNI_VITA_PLACE"); value != nullptr && value[0] != '0' && value[0] != '\0') {
        on = true;
        snprintf(text, sizeof(text), "%s", value);
    }
    if (const char* const min = getenv("EMUSUNI_VITA_PLACE_MIN")) {
        s_min = atof(min) / 100.0;
        s_keep = s_min * 0.6;
    }
#endif
    s_prio = strstr(text, "prio") != nullptr;
    if (const char* const cores = strstr(text, "cores=")) {
        s_core_count = 0;
        for (const char* p = cores + 6; *p != '\0' && s_core_count < kMaxCores; p++) {
            if (*p >= '0' && *p <= '6') {
                s_cores[s_core_count++] = *p - '0';
            } else if (*p != ',') {
                break;
            }
        }
        if (s_core_count == 0) {
            s_cores[0] = 2;
            s_cores[1] = 3;
            s_core_count = 2;
        }
    }
    s_state.store(on ? 2 : 1, std::memory_order_release);
    char list[16] = "";
    for (int k = 0, at = 0; k < s_core_count && at < static_cast<int>(sizeof(list)) - 3; k++) {
        at += snprintf(list + at, sizeof(list) - static_cast<size_t>(at), k > 0 ? ",%d" : "%d", s_cores[k]);
    }
    emusuni_vita_log("vita: place: the game's busiest threads on PS4 cores of their own: %s%s (cores %s, from %.0f%% of a core)",
                     on ? "on (the file vita-place-on; read once)" : "off (the file vita-place-on makes it on; read once)",
                     on && s_prio ? ", their priority 600" : "", list, s_min * 100);
}

// (20-11) The cores the game's threads may float on: all seven as before, or the PS4's 0-5 with "sixcores" (56).
uint64_t AllCores() {
    return emusuni_vita_switch(EMUSUNI_VITA_SIXCORES) ? 0x3full : kAllCores;
}

void SetSelf(uint64_t mask, int priority) {
#if defined(__ORBIS__)
    void* const self = emusuni_place_self();
    emusuni_place_affinity(self, mask);
    if (s_prio) {
        emusuni_place_prio(self, priority);
    }
#else
    (void)priority;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c = 0; c < 64; c++) {
        if (mask & (1ull << c)) {
            CPU_SET(c, &set);
        }
    }
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

// The look at the window's end: who is busy, who gets a core.
void Decide(uint64_t now, uint64_t from) {
    const uint64_t span = now - from;
    if (s_ticks_per_s.load(std::memory_order_relaxed) == 0) {  // (the first window measures the clock)
        const uint64_t ns = MonoNs() - s_mono_from;
        s_ticks_per_s.store(ns > 0 ? static_cast<double>(span) * 1e9 / static_cast<double>(ns) : 1e9, std::memory_order_relaxed);
    }
    double busy[kSlots];
    for (int i = 0; i < kSlots; i++) {
        busy[i] = s_slots[i].id.load(std::memory_order_relaxed) != 0
                      ? static_cast<double>(s_slots[i].ticks.exchange(0, std::memory_order_relaxed)) / static_cast<double>(span)
                      : 0.0;
    }
    // Slots idle for kIdleWindows looks in a row (and without a core) are given back. Their thread, if it runs again,
    // sees its slot is no longer its own and takes a free one.
    for (int i = 0; i < kSlots; i++) {
        Slot& s = s_slots[i];
        if (s.id.load(std::memory_order_relaxed) == 0 || s.core.load(std::memory_order_relaxed) >= 0 || busy[i] > 0) {
            s.idle = 0;
        } else if (++s.idle >= kIdleWindows) {
            s.idle = 0;
            s.named.store(false, std::memory_order_relaxed);
            s.id.store(0, std::memory_order_release);
        }
    }
    // Keep the cores of those still busy enough; free the others.
    bool taken[kMaxCores] = {};
    bool changed = false;
    for (int i = 0; i < kSlots; i++) {
        const int core = s_slots[i].core.load(std::memory_order_relaxed);
        if (core < 0) {
            continue;
        }
        int k = 0;
        while (k < s_core_count && s_cores[k] != core) {
            k++;
        }
        if (k < s_core_count && busy[i] >= s_keep && s_slots[i].id.load() != 0) {
            taken[k] = true;
        } else {
            s_slots[i].core.store(-1, std::memory_order_relaxed);
            changed = true;
            if (LineLeft()) emusuni_vita_log("vita: place: \"%s\" (thread %d) leaves core %d (%.0f%% of a core)",
                             s_slots[i].named.load() ? s_slots[i].name : "?", s_slots[i].id.load(), core, busy[i] * 100);
        }
    }
    // Give the free ones to the busiest without one.
    for (int k = 0; k < s_core_count; k++) {
        if (taken[k]) {
            continue;
        }
        int best = -1;
        for (int i = 0; i < kSlots; i++) {
            if (s_slots[i].id.load(std::memory_order_relaxed) != 0 && s_slots[i].core.load(std::memory_order_relaxed) < 0 &&
                busy[i] >= s_min && (best < 0 || busy[i] > busy[best])) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        s_slots[best].core.store(s_cores[k], std::memory_order_relaxed);
        taken[k] = true;
        changed = true;
        if (LineLeft()) emusuni_vita_log("vita: place: \"%s\" (thread %d) gets core %d (%.0f%% of a core)",
                         s_slots[best].named.load() ? s_slots[best].name : "?", s_slots[best].id.load(), s_cores[k], busy[best] * 100);
    }
    uint64_t dedicated = 0;
    for (int i = 0; i < kSlots; i++) {
        const int core = s_slots[i].core.load(std::memory_order_relaxed);
        if (core >= 0) {
            dedicated |= 1ull << core;
        }
    }
    if (dedicated != s_dedicated.load(std::memory_order_relaxed)) {
        s_dedicated.store(dedicated, std::memory_order_relaxed);
        changed = true;
    }
    if (changed) {  // (each thread sets its affinity again only after a change)
        s_generation.fetch_add(1, std::memory_order_release);
    }
}

}  // namespace

// A guest thread came back from recompiled code after <ticks> of it (the stats' clock).
extern "C" void emusuni_vita_place_tick(int id, const char* name, unsigned long long ticks) {
    int state = s_state.load(std::memory_order_acquire);
    if (state == 0) {
        static std::atomic<bool> reading{false};
        if (!reading.exchange(true)) {
            s_mono_from = MonoNs();
            s_window_from.store(__rdtsc(), std::memory_order_relaxed);
            ReadSwitch();
        }
        return;
    }
    if (state != 2) {
        return;
    }
    thread_local Slot* slot = nullptr;
    thread_local uint32_t applied = 0;
    thread_local int my_core = -2;
    if (slot != nullptr && slot->id.load(std::memory_order_relaxed) != id) {
        slot = nullptr;  // (given back while this thread had no recompiled code for a while: it takes a free one)
        applied = 0;
    }
    if (slot == nullptr) {
        for (Slot& s : s_slots) {
            int none = 0;
            if (s.id.compare_exchange_strong(none, id)) {
                s.ticks.store(0, std::memory_order_relaxed);
                snprintf(s.name, sizeof(s.name), "%s", name != nullptr ? name : "?");
                s.named.store(true, std::memory_order_release);
                slot = &s;
                break;
            }
        }
        if (slot == nullptr) {
            return;  // (more than 64 threads with recompiled code in the last few windows: this one stays where it is
                     // until a slot is given back)
        }
    }
    slot->ticks.fetch_add(ticks, std::memory_order_relaxed);
    const uint64_t now = __rdtsc();
    uint64_t from = s_window_from.load(std::memory_order_relaxed);
    const double per_s = s_ticks_per_s.load(std::memory_order_relaxed);
    const double window = per_s > 0 ? per_s * 2.0 : 2.0e9;  // (2 s; the first window guesses 1 GHz)
    if (static_cast<double>(now - from) >= window && s_window_from.compare_exchange_strong(from, now)) {
        Decide(now, from);
    }
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    if (generation != applied) {
        applied = generation;
        const int core = slot->core.load(std::memory_order_relaxed);
        if (core != my_core || core < 0) {
            my_core = core;
            const uint64_t all = AllCores();
            const uint64_t mask = core >= 0 ? (1ull << core) : (all & ~s_dedicated.load(std::memory_order_relaxed));
            SetSelf(mask != 0 ? mask : all, core >= 0 ? 600 : 700);
        }
    }
}

// (20-11, the Vita specialist's, for 01.84) The switch "sixcores" (56, OFF; port/ps4_vita_sys.h): each of the core's
// threads, as it starts, keeps to the PS4's cores 0-5 (0x3f) - who: 0 a guest thread (thread.cpp's top run_loop),
// 1 the renderer's (libretro's render thread), 2 a pipeline compiler (pipeline_cache.cpp's compiler_thread).
// Why: HENkaku-PS4 starts every thread of its own on those six (its runtime's pthread_create: "the app's cores"); ours
// floated on all seven (the log's "cores 0x7f"), and the seventh is the system's too. Uncharted on 01.83: the game's
// main thread at 85% of a core and the renderer's at 87%, both seen on 0x7f. With the switch OFF: one look at the
// switch per thread start, nothing set. (vitaplace, when it is on too, keeps to the same six: AllCores().)
extern "C" void emusuni_vita_sixcores_here(int who) {
    if (!emusuni_vita_switch(EMUSUNI_VITA_SIXCORES)) {
        return;
    }
#if defined(__ORBIS__)
    emusuni_place_affinity(emusuni_place_self(), 0x3full);
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c = 0; c < 6; c++) {
        CPU_SET(c, &set);
    }
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
    static std::atomic<unsigned> s_started[3];
    static const char* const kWho[3] = {"the game's threads", "the renderer's thread", "the pipeline compilers"};
    const int at = who >= 0 && who < 3 ? who : 0;
    if (s_started[at].fetch_add(1, std::memory_order_relaxed) == 0) {
        emusuni_vita_log("vita: sixcores: %s on the PS4's cores 0-5 (0x3f) as they start, never the seventh (the file "
                         "vita-sixcores-on)", kWho[at]);
    }
}
