// EmuSuNi's PS Vita port: what the game really does, in the log every 10 seconds (01.31).
//
// Vita3K runs each of the guest's threads as a thread of the system, at its own pace: retro_run only counts the
// screen's refreshes and hands over the last picture. So the app's "speed:" line says 100% and 0.3 ms a frame
// whatever the game does, and its sound line 99.9% (the core fills what the game didn't) - 01.30's console log,
// of two games their owner called very slow. These lines measure the game itself:
//
//   vita: speed: the pictures the game showed (its sceDisplaySetFrameBuf calls) against the screen's refreshes;
//                its threads' time in recompiled code and in the system's calls (their waits too), and their
//                CPU time where the system tells a thread's; what the recompiler, the shaders, the pipelines,
//                the renderer and the files took.
//   vita: threads: the busiest threads: one at ~100% of a core is what holds the game back.
//   (17-11) vita: faults: the faults of pages the emulator protects, and what the recompiler's fast memory lost to
//                them; vita: calls: the system's calls by name, the costliest and the most made; vita: memory: how
//                much of the guest's memory is in 2 MiB pages (a line in six).
//   (20-11) vita: renderer: the renderer's thread part by part - a draw's pieces, its lists' commands by kind, the
//                textures it binds, hashes and sends, the vertices and indices it copies, its submissions to the GPU -,
//                to set beside HENkaku-PS4's own "renderer:" line in one scene.
//
// Times are the CPU's time stamp counter's, read around each piece and scaled by the clock at each line: two
// reads cost less than the call they are around. The core's hooks: scripts/build-vita-core.sh (STAT_API).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <unistd.h>
#include <x86intrin.h>

extern "C" void emusuni_vita_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
// (port/ps4_vita_sys.c's, for the lines of 17-11.)
extern "C" void emusuni_vita_fault_counts(unsigned long long* handled, unsigned long long* first);
extern "C" void emusuni_vita_mem_large(unsigned* pieces, unsigned* whole, int* large);
// (20-11, for 01.86) the memory watches (the core's mem.cpp, the "surfacewatch" switch): out[0-2] since the last call
// (protections, of them on a range already watched, watches dropped), out[3] ranges, out[4] watches, out[5] the most
extern "C" void emusuni_vita_watch_stats(unsigned long long* out);
#if defined(__ORBIS__)
extern "C" int sceKernelGetCurrentCpu(void);
#else
#include <sched.h>
#endif

namespace {

// (The numbers the core's hooks give: STAT_API in the script.)
// (01.40) 7-13, for a game whose pictures come out wrong or that doesn't answer the pad: a transfer through the
// guest's memory (a copy, a downscale, a fill: the GPU's picture of a painted surface isn't in that memory here,
// nor does a fill reach it), a painted surface copied to be sampled, a draw that reads the colour already
// painted, a refresh with a button of player 1's down, a refresh with a finger on the touch screen.
// (17-11) 14-15, the recompiler's fast memory: a fault of recompiled code that the recompiler took (the access done
// through a call instead), and an instruction it then made again with a call in its place for good.
enum {
    kPicture, kBlock, kShader, kPipeline, kFile, kBatch, kFull,
    kXferCopy, kXferDown, kXferFill, kSurfaceCopy, kFetchDraw, kPad, kTouch,
    kFastFail, kFastMark,
    // 16-18 (the same day, after a profile of Mortal Kombat on the PC had 42% of its threads' time in them and 23%
    // in looking blocks up): a memory access of recompiled code done through a call of the emulator's, a block
    // the dispatcher had to look up, a system call answered inside the recompiler (port/ps4_vita_native.cpp).
    kMemCall, kLookup, kInline,
    // 19-22 (after 01.68: the console's Street Fighter X Tekken with its menus full of lines, the bench's right):
    // what a draw reads from the game's memory, copied when the game asks for the draw - its uniform buffers, its
    // vertices, its indices (the count: copies; the "ticks": bytes) -, and how long after the game sent a list of
    // drawing commands the renderer's thread got to it (the count: lists; the ticks: that wait).
    kCopyUniform, kCopyVertex, kCopyIndex, kListWait,
    // 23: a vertex stream given to the GPU with its vertices padded to a multiple of 4 bytes (the "vertexalign"
    // switch: the PS4's GPU and vertices an odd number of bytes apart).
    kPadded,
    // 24-26 (after 01.70: Street Fighter X Tekken's minutes at its "VS" screen on the console), a compressed file
    // of a game's ZIP: bytes inflated only to get to the place a read asks for (the ticks: bytes), a stream
    // started at a file's first byte, a stream started from a mark (the "zipmarks" switch).
    kZipSkip, kZipStart, kZipMark,
    // 27: a vertex stream given to the GPU laid out anew, every attribute at a multiple of 4 bytes (the
    // "vertexrelay" switch: numbers of 2 or more bytes at odd places inside the vertex).
    kRelaid,
    // 28-31 (after 01.71, the "coreturns" switch: the threads a game ties to one of the Vita's cores run one at a
    // time): a thread that had to wait for its core's turn (the ticks: that wait), and the turns taken from a
    // thread that was inside a system call, that had had its 10 ms, or by a thread of higher priority.
    kTurnWait, kTurnCall, kTurnTime, kTurnPriority,
    // 32 (after 01.73): a lightweight mutex let go of that was left for its waiter - a thread tied to one of the
    // Vita's cores - to take once it has its core's turn, where upstream hands it over at once.
    kTurnRetry,
    // 33-36 (after 01.78: LittleBigPlanet's 3D that is not drawn), a texture that lies on a surface the game
    // painted: taken from a surface; one whose surface the game had written to since it was painted (read from
    // the game's memory instead); not taken, the surface is laid out otherwise (read from the game's memory);
    // and, of those taken, the ones taken from another surface than the nearest (the "surfacepick" switch).
    kSurfaceTaken, kSurfaceWritten, kSurfaceOther, kSurfacePicked,
    // 37-53 (20-11, for 01.84: HENkaku-PS4's Uncharted at 30 pictures a second where ours made 21-24, its renderer's
    // thread at 33-57% of a core where ours worked 87%; its own "renderer:" line splits a draw's time - the same parts
    // here, to set the two side by side in one scene), all of the renderer's thread's: a draw (the ticks: its whole
    // time), its pipeline, its uniforms and textures, its indices and vertices; its lists' commands by kind (draws and
    // draw packets, state, scenes, the others); a texture bound for a draw (its whole time), hashed to see whether it
    // changed (the time; then the bytes), sent to the GPU (the bytes); the vertex streams copied (bytes), the part of
    // them below the draw's lowest vertex (bytes: what copying from that vertex would leave out), the indices copied
    // (bytes); a submission to the GPU (its whole time) and its wait for the frontend's queue lock.
    kRDraw, kRDrawPipe, kRDrawBind, kRDrawGeom,
    kRCmdDraw, kRCmdState, kRCmdScene, kRCmdOther,
    kRTexBind, kRTexHash, kRTexHashBytes, kRTexUpload,
    kRVertex, kRVertexBelow, kRIndex, kRSubmit, kRSubmitLock,
    // 54-55 (20-11, for 01.85): the renderer's waits for the submission thread to make the submissions it needs (the
    // "submitthread" switch; only the waits that waited), and its wait for the GPU to copy a picture for the frontend
    // (each picture: right after the copy's submission; with "presentnowait", at the next copy).
    kRSubmitWait, kRPresent,
    kKinds
};
struct Kind {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> ticks{0};
};
Kind s_kinds[kKinds];
std::atomic<uint64_t> s_list_wait_most{0};  // the longest of kListWait's waits since the last line (ticks)

// A guest thread's numbers, written by the thread alone and read by the line's writer.
struct Slot {
    std::atomic<int> state{0};       // 0 free, 3 being taken, 1 a thread's, 2 its thread ended (the next line frees it)
    char name[28];
    int id;
    std::atomic<uint64_t> guest{0};  // ticks in recompiled code (the recompiler's own work in it)
    std::atomic<uint64_t> calls{0};  // ticks in the system's calls, their waits too
    std::atomic<uint64_t> count{0};  // those calls
    std::atomic<uint64_t> cpu_ns{0}; // the thread's CPU clock at its last look
    std::atomic<uint32_t> cores{0};  // the cores it was seen on since the last line
    // (After 01.68: on the console God of War's launcher stopped before its first picture - its threads went quiet
    // one by one, and the lines only count a call when it comes back.) The system call the thread is in now, and
    // since when: for the "vita: waits:" line.
    std::atomic<uint32_t> in_nid{0};
    std::atomic<uint64_t> in_since{0};
    // (After 01.70: in 01.70's log that launcher's "SceFiosIO" thread sat in sceKernelLockLwMutex for good, and the
    // line could not say whose the mutex was.) When the call the thread is in waits for a mutex: the thread that
    // holds it (its id; 0: the call isn't waiting for one) and how many times it holds it.
    std::atomic<int32_t> wait_owner{0};
    std::atomic<int32_t> wait_count{0};
    uint64_t guest_said, calls_said, count_said, cpu_said;  // (the line writer's: what it has told already)
};
constexpr int kSlots = 192;
Slot s_slots[kSlots];
std::atomic<uint64_t> s_unlisted{0};  // threads that found no slot

constinit thread_local Slot* t_slot = nullptr;
constinit thread_local uint64_t t_inner = 0;   // ticks of the levels inside this one (a callback's guest code)
constinit thread_local uint64_t t_looked = 0;  // when the thread's CPU clock was last read

// (17-11) The system's calls by which one: how many and how long (their waits too), for the two lines that name
// the costliest and the most made - what a "fast call" would have to be about, and whether a thread with millions
// of calls is working or waiting. A table by the call's number (its NID), filled without a lock: a slot is one
// call's for good once a thread has put its number there.
struct Call {
    std::atomic<uint32_t> nid{0};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> ticks{0};
    uint64_t count_said, ticks_said;  // (the line writer's)
};
constexpr uint32_t kCalls = 4096;  // (a power of two; a game uses a few hundred)
Call s_calls[kCalls];
std::atomic<const char* (*)(unsigned)> s_call_name{nullptr};
constinit thread_local uint32_t t_nid = 0;      // the call this thread is in (the core says it as it starts)
constinit thread_local uint32_t t_nid_outer[8]; // those of the levels around (a call that runs a callback)
constinit thread_local int t_nid_depth = 0;

Call* CallSlot(uint32_t nid) {
    uint32_t at = (nid * 2654435761u) >> 20;  // (12 bits)
    for (uint32_t tries = 0; tries < 64; tries++, at = (at + 1) & (kCalls - 1)) {
        Call& call = s_calls[at];
        uint32_t there = call.nid.load(std::memory_order_relaxed);
        if (there == nid) {
            return &call;
        }
        if (there == 0 && call.nid.compare_exchange_strong(there, nid, std::memory_order_relaxed)) {
            return &call;
        }
        if (there == nid) {
            return &call;
        }
    }
    return nullptr;
}

std::atomic<uint64_t> s_ticks_per_s{1600000000ull};  // (the line sets it: the PS4's CPU runs at 1.6 GHz)
std::atomic<bool> s_restart{true};

uint64_t MonotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// ---- The calling thread's CPU time: the clock's number isn't the same in every C library and kernel (the PS4's
// kernel is FreeBSD's, where it is 14), and a number that is taken may count something else. So: each candidate is
// tried once - a sleep must cost it next to nothing and a busy wait about its length. ----
std::atomic<int> s_cpu_clock{-2};  // -2 not tried yet, -1 none, else the clock's number
std::atomic_flag s_cpu_clock_trying = ATOMIC_FLAG_INIT;

bool ClockNs(int id, uint64_t* ns) {
    timespec ts;
    if (clock_gettime(static_cast<clockid_t>(id), &ts) != 0) {
        return false;
    }
    *ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    return true;
}

int FindCpuClock() {
    static const int kCandidates[] = {static_cast<int>(CLOCK_THREAD_CPUTIME_ID), 14};
    for (const int id : kCandidates) {
        uint64_t c0 = 0, c1 = 0, c2 = 0;
        if (!ClockNs(id, &c0)) {
            continue;
        }
        const uint64_t w0 = MonotonicNs();
        usleep(3000);
        const uint64_t w1 = MonotonicNs();
        ClockNs(id, &c1);
        volatile uint64_t spins = 0;
        while (MonotonicNs() - w1 < 3000000) {
            spins = spins + 1;
        }
        const uint64_t w2 = MonotonicNs();
        ClockNs(id, &c2);
        const double slept = static_cast<double>(c1 - c0), sleep = static_cast<double>(w1 - w0);
        const double spun = static_cast<double>(c2 - c1), busy = static_cast<double>(w2 - w1);
        if (slept < 0.3 * sleep && spun > 0.5 * busy && spun < 1.5 * busy) {
            emusuni_vita_log("vita: speed: a thread's CPU time is clock %d here (a %.1f ms sleep counted %.2f ms, "
                             "%.1f ms busy counted %.2f)", id, sleep / 1e6, slept / 1e6, busy / 1e6, spun / 1e6);
            return id;
        }
    }
    emusuni_vita_log("vita: speed: no clock tells a thread's CPU time here: the lines give the threads' time in guest "
                     "code and in system calls (their waits in it) only");
    return -1;
}

int CpuClock() {
    int id = s_cpu_clock.load(std::memory_order_relaxed);
    if (id != -2) {
        return id;
    }
    if (s_cpu_clock_trying.test_and_set(std::memory_order_acquire)) {
        return -1;  // (another thread is finding it out: this look goes without)
    }
    id = s_cpu_clock.load(std::memory_order_relaxed);
    if (id == -2) {
        id = FindCpuClock();
        s_cpu_clock.store(id, std::memory_order_relaxed);
    }
    s_cpu_clock_trying.clear(std::memory_order_release);
    return id;
}

int CurrentCore() {
#if defined(__ORBIS__)
    return sceKernelGetCurrentCpu();
#else
    return sched_getcpu();
#endif
}

// (After 01.66, from the performance specialist's profile of the three bench games: these lines' own bookkeeping
// was 2.8-3.7% of the game threads' time - for each system call two locked adds on memory that every thread
// writes.) What a thread counts, by the call and by the kind, stays in tables of its own and goes into the shared
// ones 32 times a second (when its CPU clock is looked at), when a slot of its table is needed for another call,
// and when the thread ends.
struct Mine {
    uint32_t nid;
    uint32_t count;
    uint64_t ticks;
};
constexpr uint32_t kMine = 64;  // (a power of two: the slot is the top 6 bits of the number's hash)
constinit thread_local Mine t_mine[kMine] = {};
constinit thread_local uint64_t t_counts[2] = {0, 0};  // 0 a memory access through a call, 1 a block looked up
constinit thread_local uint64_t t_inline_count = 0, t_inline_ticks = 0;

void PutOne(Mine& mine) {
    if (mine.count == 0) {
        return;
    }
    if (Call* const call = CallSlot(mine.nid)) {
        call->count.fetch_add(mine.count, std::memory_order_relaxed);
        call->ticks.fetch_add(mine.ticks, std::memory_order_relaxed);
    }
    mine.count = 0;
    mine.ticks = 0;
}

inline void CountCall(uint32_t nid, uint64_t ticks) {
    Mine& mine = t_mine[(nid * 2654435761u) >> 26];
    if (mine.nid != nid) {
        PutOne(mine);
        mine.nid = nid;
    }
    mine.count++;
    mine.ticks += ticks;
}

void PutMine() {
    for (Mine& mine : t_mine) {
        PutOne(mine);
    }
    for (int which = 0; which < 2; which++) {
        if (t_counts[which] != 0) {
            s_kinds[which == 0 ? kMemCall : kLookup].count.fetch_add(t_counts[which], std::memory_order_relaxed);
            t_counts[which] = 0;
        }
    }
    if (t_inline_count != 0) {
        s_kinds[kInline].count.fetch_add(t_inline_count, std::memory_order_relaxed);
        s_kinds[kInline].ticks.fetch_add(t_inline_ticks, std::memory_order_relaxed);
        t_inline_count = 0;
        t_inline_ticks = 0;
    }
}

// The thread's CPU clock and core, into its slot; and what it has counted since the last look, into the shared
// tables.
void Look(Slot* slot, uint64_t now) {
    t_looked = now;
    PutMine();
    const int clock = CpuClock();
    uint64_t ns = 0;
    if (clock >= 0 && ClockNs(clock, &ns)) {
        slot->cpu_ns.store(ns, std::memory_order_relaxed);
    }
    const int core = CurrentCore();
    if (core >= 0 && core < 32) {
        slot->cores.fetch_or(1u << core, std::memory_order_relaxed);
    }
}

void Bump(std::atomic<uint64_t>& value, uint64_t by) {
    value.store(value.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);  // (one writer)
}

}  // namespace

extern "C" unsigned long long emusuni_vita_stat_now(void) {
    return __rdtsc();
}

extern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks) {
    if (what < 0 || what >= kKinds) {
        return;
    }
    s_kinds[what].count.fetch_add(1, std::memory_order_relaxed);
    s_kinds[what].ticks.fetch_add(ticks, std::memory_order_relaxed);
    if (what == kListWait && ticks > s_list_wait_most.load(std::memory_order_relaxed)) {
        s_list_wait_most.store(ticks, std::memory_order_relaxed);  // (one writer: the renderer's thread)
    }
}

// (After 01.65: on the console Street Fighter X Tekken went from its main menu back to its title - what the game
// does for Circle and then Cross there -, and the log had only how long a button was down, not which.) The buttons
// of player 1's pad as the game is given them, once a refresh: a press is a button that goes down. Said in the
// "vita: gfx:" line by the PS Vita's own names.
static std::atomic<uint32_t> s_pad_before{0};
static std::atomic<uint32_t> s_pad_presses[16];
extern "C" void emusuni_vita_stat_pad(unsigned buttons) {
    const uint32_t now = buttons & 0xFFFFu;
    uint32_t pressed = now & ~s_pad_before.exchange(now, std::memory_order_relaxed);
    while (pressed != 0) {
        s_pad_presses[__builtin_ctz(pressed)].fetch_add(1, std::memory_order_relaxed);
        pressed &= pressed - 1;
    }
}

// A guest thread's loop begins (top: the thread's own; else a callback's, inside a system call of the thread's).
// Returns what the level around this one must have back at the end.
// The system call a guest thread is about to make (the core's call of an import).
extern "C" void emusuni_vita_stat_call(unsigned nid) {
    t_nid = nid;
    if (Slot* const slot = t_slot) {
        slot->wait_owner.store(0, std::memory_order_relaxed);
        slot->in_since.store(__rdtsc(), std::memory_order_relaxed);
        slot->in_nid.store(nid, std::memory_order_relaxed);
    }
}

// The call this thread is in is about to wait for a mutex that another thread holds (the core's kernel says it
// as the wait begins): that thread's id and how many times it holds the mutex, for the "vita: waits:" line.
extern "C" void emusuni_vita_stat_wait_owner(int owner, int count) {
    if (Slot* const slot = t_slot) {
        slot->wait_count.store(count, std::memory_order_relaxed);
        slot->wait_owner.store(owner, std::memory_order_relaxed);
    }
}

// Counted by the thread that does them, millions a second at worst: kept by the thread (t_counts, above) and added
// to the kinds when its clock is looked at. which: 0 a memory access through a call, 1 a block looked up.
extern "C" void emusuni_vita_stat_tl(int which) {
    t_counts[which & 1]++;
}

// A system call answered inside the recompiler's own call for it (port/ps4_vita_native.cpp): its time is taken
// off the recompiled code's, which the thread's loop measures around it.
static constinit thread_local uint64_t t_inline = 0;
extern "C" void emusuni_vita_stat_inline(unsigned nid, unsigned long long ticks) {
    Slot* const slot = t_slot;
    if (slot == nullptr) {
        return;
    }
    t_inline += ticks;
    Bump(slot->calls, ticks);
    Bump(slot->count, 1);
    CountCall(nid, ticks);
    t_inline_count++;
    t_inline_ticks += ticks;
}

// How a call's number is named (the core's table of them), for the lines.
extern "C" void emusuni_vita_stat_names(const char* (*name)(unsigned)) {
    s_call_name.store(name, std::memory_order_relaxed);
}

extern "C" unsigned long long emusuni_vita_stat_level_begin(const char* name, int id, int top) {
    const uint64_t outer = t_inner;
    t_inner = 0;
    // (A callback's guest code, inside a call: that call's number is kept for when the callback is over.)
    if (t_nid_depth < 8) {
        t_nid_outer[t_nid_depth] = t_nid;
    }
    t_nid_depth++;
    t_nid = 0;
    if (top == 0 || t_slot != nullptr) {
        return outer;
    }
    for (Slot& slot : s_slots) {
        int free_state = 0;
        if (!slot.state.compare_exchange_strong(free_state, 3, std::memory_order_acquire)) {
            continue;
        }
        snprintf(slot.name, sizeof(slot.name), "%s", name != nullptr && name[0] != 0 ? name : "?");
        slot.id = id;
        slot.guest.store(0, std::memory_order_relaxed);
        slot.calls.store(0, std::memory_order_relaxed);
        slot.count.store(0, std::memory_order_relaxed);
        slot.cores.store(0, std::memory_order_relaxed);
        slot.in_nid.store(0, std::memory_order_relaxed);
        slot.in_since.store(0, std::memory_order_relaxed);
        slot.wait_owner.store(0, std::memory_order_relaxed);
        slot.wait_count.store(0, std::memory_order_relaxed);
        slot.guest_said = slot.calls_said = slot.count_said = 0;
        slot.cpu_ns.store(0, std::memory_order_relaxed);
        t_slot = &slot;
        Look(&slot, __rdtsc());
        slot.cpu_said = slot.cpu_ns.load(std::memory_order_relaxed);
        slot.state.store(1, std::memory_order_release);
        return outer;
    }
    s_unlisted.fetch_add(1, std::memory_order_relaxed);
    return outer;
}

extern "C" void emusuni_vita_stat_level_end(int top, unsigned long long outer, unsigned long long ticks) {
    t_inner = outer + ticks;
    if (t_nid_depth > 0) {
        t_nid_depth--;
    }
    t_nid = t_nid_depth < 8 ? t_nid_outer[t_nid_depth] : 0;
    if (top == 0 && t_slot != nullptr && t_nid != 0) {
        // (A callback is over: the thread is back in the call that ran it.)
        t_slot->wait_owner.store(0, std::memory_order_relaxed);
        t_slot->in_since.store(__rdtsc(), std::memory_order_relaxed);
        t_slot->in_nid.store(t_nid, std::memory_order_relaxed);
    }
    if (top == 0 || t_slot == nullptr) {
        return;
    }
    Look(t_slot, __rdtsc());
    t_slot->state.store(2, std::memory_order_release);
    t_slot = nullptr;
    t_inner = 0;
}

// One turn of a guest thread's loop: `guest` ticks in recompiled code, then `call` ticks in the system call that
// stopped it (called: there was one).
extern "C" void emusuni_vita_stat_guest(unsigned long long guest, unsigned long long call, int called) {
    const uint64_t inner = t_inner;
    t_inner = 0;
    Slot* const slot = t_slot;
    if (slot == nullptr) {
        return;
    }
    const uint64_t answered_inside = t_inline;
    t_inline = 0;
    Bump(slot->guest, guest > answered_inside ? guest - answered_inside : 0);
    Bump(slot->calls, call > inner ? call - inner : 0);
    if (called != 0) {
        Bump(slot->count, 1);
        if (const uint32_t nid = t_nid; nid != 0) {
            CountCall(nid, call > inner ? call - inner : 0);
        }
        slot->in_nid.store(0, std::memory_order_relaxed);
    }
    t_nid = 0;
    const uint64_t now = __rdtsc();
    if (now - t_looked > s_ticks_per_s.load(std::memory_order_relaxed) / 32) {
        Look(slot, now);
    }
}

// (After 01.70) The "uniformlog" switch: the numbers a game gives its shader programs, for a picture that is wrong
// on the console and right on the PC bench - the same lines from both say whether the game gave other numbers or
// the GPU made another picture of the same ones. The core calls this for each uniform buffer a draw's program is
// given (its renderer's thread, the switch ON): a set of numbers is said the first time a program is given it -
// 24 sets a fragment program at most (its first 72 numbers), one a vertex program (its first 24: its matrices
// change at every draw) -, 2400 lines a game at most and nothing after its first 20 minutes: from then on the
// call costs a look at a flag. The program is named by the first four bytes of its hash, as its files in the
// shader cache are; the buffer by the renderer's number (0: the program's default one).
namespace {
struct UniformSeen {
    uint32_t key;  // the program's first four hash bytes; 0: free
    uint8_t vertex, block, count;
    uint64_t sets[24];  // the sets said already (a hash of their bytes)
};
constexpr uint32_t kUniformPrograms = 1024;
constexpr int kUniformLines = 2400;
UniformSeen s_uniform_seen[kUniformPrograms];
int s_uniform_lines = 0;
uint64_t s_uniform_from = 0;  // (the time stamp counter at the game's first call)
std::atomic<uint32_t> s_uniform_game_seen{0};
std::atomic<uint32_t> s_uniform_game{0};  // (a game started: everything is said again)
std::atomic<bool> s_uniform_over{false};  // (the lines or the minutes are spent: nothing more for this game)
std::atomic<bool> s_uniform_busy{false};
}  // namespace

extern "C" void emusuni_vita_stat_uniforms(const unsigned char* program, int vertex, int block, const void* data, unsigned size) {
    const uint32_t game = s_uniform_game.load(std::memory_order_relaxed);
    if (s_uniform_over.load(std::memory_order_relaxed) && game == s_uniform_game_seen.load(std::memory_order_relaxed)) {
        return;
    }
    if (program == nullptr || data == nullptr || size < 4 || s_uniform_busy.exchange(true, std::memory_order_acquire)) {
        return;
    }
    const uint64_t now = __rdtsc();
    if (game != s_uniform_game_seen.load(std::memory_order_relaxed) || s_uniform_from == 0) {
        s_uniform_game_seen.store(game, std::memory_order_relaxed);
        memset(s_uniform_seen, 0, sizeof(s_uniform_seen));
        s_uniform_lines = 0;
        s_uniform_from = now;
        s_uniform_over.store(false, std::memory_order_relaxed);
    }
    if (s_uniform_lines >= kUniformLines || now - s_uniform_from > 1200 * s_ticks_per_s.load(std::memory_order_relaxed)) {
        emusuni_vita_log("vita: uniforms: no more for this game (%d lines said; the uniformlog switch says at most %d, in a game's first 20 minutes)",
                         s_uniform_lines, kUniformLines);
        s_uniform_over.store(true, std::memory_order_relaxed);
        s_uniform_busy.store(false, std::memory_order_release);
        return;
    }
    uint32_t key = static_cast<uint32_t>(program[0]) << 24 | static_cast<uint32_t>(program[1]) << 16 |
                   static_cast<uint32_t>(program[2]) << 8 | program[3];
    if (key == 0) {
        key = 1;
    }
    UniformSeen* seen = nullptr;
    const uint32_t first = (key * 2654435761u) ^ (static_cast<uint32_t>(block) << 1) ^ static_cast<uint32_t>(vertex & 1);
    for (uint32_t probe = 0; probe < 32; probe++) {
        UniformSeen& entry = s_uniform_seen[(first + probe) % kUniformPrograms];
        if (entry.key == 0) {
            entry.key = key;
            entry.vertex = static_cast<uint8_t>(vertex & 1);
            entry.block = static_cast<uint8_t>(block);
            entry.count = 0;
            seen = &entry;
            break;
        }
        if (entry.key == key && entry.vertex == static_cast<uint8_t>(vertex & 1) && entry.block == static_cast<uint8_t>(block)) {
            seen = &entry;
            break;
        }
    }
    // (A program's sets after its fourth only while six tenths of the lines are unspent: a program the game comes
    // to late still has its first ones said.)
    if (seen != nullptr && seen->count < (seen->vertex != 0 ? 1 : 24) && (seen->count < 4 || s_uniform_lines < kUniformLines * 6 / 10)) {
        const unsigned char* const bytes = static_cast<const unsigned char*>(data);
        uint64_t hash = 1469598103934665603ull;  // (FNV-1a)
        for (unsigned i = 0; i < size; i++) {
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        }
        bool said = false;
        for (int i = 0; i < seen->count; i++) {
            said = said || seen->sets[i] == hash;
        }
        if (!said) {
            seen->sets[seen->count++] = hash;
            // (24 numbers a line)
            const unsigned most = seen->vertex != 0 ? 24 : 72;
            const unsigned numbers = size / 4 < most ? size / 4 : most;
            for (unsigned from = 0; from < numbers; from += 24) {
                char line[500];
                int at = snprintf(line, sizeof(line), "vita: uniforms: %s %08x buffer %d, set %d (%u bytes), from number %u:",
                                  vertex != 0 ? "vp" : "fp", key, block, static_cast<int>(seen->count), size, from);
                for (unsigned i = from; i < numbers && i < from + 24 && at > 0 && at < static_cast<int>(sizeof(line)) - 16; i++) {
                    float value;
                    memcpy(&value, bytes + 4 * i, sizeof(value));
                    at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %.4g", static_cast<double>(value));
                }
                emusuni_vita_log("%s", line);
                s_uniform_lines++;
            }
        }
    }
    s_uniform_busy.store(false, std::memory_order_release);
}

// (After 01.71) A thread the game makes (the core's kernel says it): its name, its priority and the Vita's cores
// it may run on - 0x10000, 0x20000 and 0x40000 are the three a game has; 0 is the game's default. For a game that
// stops with two of its threads each holding what the other waits for (God of War Collection's launcher, its two
// FIOS threads): on a Vita two threads tied to the same core never run at the same time. 300 lines a game.
namespace {
std::atomic<uint32_t> s_threads_game{0};
std::atomic<int> s_threads_said{0};
}  // namespace

extern "C" void emusuni_vita_thread_made(const char* name, int id, int priority, unsigned cores) {
    const uint32_t game = s_uniform_game.load(std::memory_order_relaxed);
    if (s_threads_game.exchange(game, std::memory_order_relaxed) != game) {
        s_threads_said.store(0, std::memory_order_relaxed);
    }
    if (s_threads_said.fetch_add(1, std::memory_order_relaxed) >= 300) {
        return;
    }
    const unsigned user = cores & 0x70000u;
    emusuni_vita_log("vita: thread: \"%.40s\" (thread %d) made: priority %d, the Vita's cores %#x%s", name != nullptr ? name : "?", id, priority, cores,
                     cores == 0 ? " (the game's default)"
                                : (user == 0x10000u || user == 0x20000u || user == 0x40000u) ? " (tied to one core)" : "");
}

// (After 01.71) For the app's FPS counter. It counted the screen's refreshes, which are 60 a second whatever the
// game does - a game stopped at its first screen read "60 FPS 100%", a fight at 48 pictures a second "59-60" (the
// owner: "it always says 60 and the game stutters"). These give it the pictures the game itself showed, the count
// the "vita: speed:" line has (the game's sceDisplaySetFrameBuf calls):
//   emusuni_vita_pictures_total()       how many since the core was loaded;
//   emusuni_vita_pictures_per_second()  those of the last second, as a rate - to be called once a frame while the
//                                       game runs (less often works too: then over the time since the last call,
//                                       up to 2 s). 0 until a quarter of a second has been seen; after 2 s
//                                       without a call (the app's menu was open) it starts again.
extern "C" unsigned long long emusuni_vita_pictures_total(void) {
    return s_kinds[kPicture].count.load(std::memory_order_relaxed);
}

extern "C" unsigned emusuni_vita_pictures_per_second(void) {
    struct Sample {
        uint64_t ns, total;
    };
    constexpr unsigned kSamples = 128;  // (two seconds of calls at 60 a second)
    static Sample ring[kSamples];
    static unsigned next = 0, used = 0;
    static std::atomic<unsigned> last{0};
    static std::atomic<bool> busy{false};
    if (busy.exchange(true, std::memory_order_acquire)) {
        return last.load(std::memory_order_relaxed);
    }
    const uint64_t now = MonotonicNs();
    const uint64_t total = s_kinds[kPicture].count.load(std::memory_order_relaxed);
    if (used != 0 && now - ring[(next + kSamples - 1) % kSamples].ns > 2000000000ull) {
        used = 0;  // (a pause: what came before it doesn't count)
        last.store(0, std::memory_order_relaxed);
    }
    // The sample to count from: the oldest one of the last second; with none there, the one before this call.
    const Sample* from = nullptr;
    for (unsigned i = used; i > 0; i--) {
        const Sample& sample = ring[(next + kSamples - i) % kSamples];
        if (now - sample.ns <= 1000000000ull) {
            from = &sample;
            break;
        }
    }
    if (from == nullptr && used != 0) {
        from = &ring[(next + kSamples - 1) % kSamples];
    }
    if (from != nullptr && now - from->ns >= 250000000ull && total >= from->total) {
        last.store(static_cast<unsigned>(((total - from->total) * 1000000000ull + (now - from->ns) / 2) / (now - from->ns)),
                   std::memory_order_relaxed);
    }
    ring[next] = Sample{now, total};
    next = (next + 1) % kSamples;
    if (used < kSamples) {
        used++;
    }
    busy.store(false, std::memory_order_release);
    return last.load(std::memory_order_relaxed);
}

// A game starts: the first line counts from here.
extern "C" void emusuni_vita_stat_start(void) {
    s_restart.store(true, std::memory_order_relaxed);
    s_uniform_game.fetch_add(1, std::memory_order_relaxed);
}

// A refresh of the screen (retro_run, with the game running). Every 10 seconds: the lines.
extern "C" void emusuni_vita_stat_tick(void) {
    static uint64_t from_ns = 0, from_ticks = 0, refreshes = 0, last_ns = 0;
#if !defined(__ORBIS__)
    // (A test, on the PC only: EMUSUNI_VITA_TEST_FPS=1 asks the app's FPS function once a refresh, as the app
    // will, and says its answer every 300 refreshes - to hold against the "vita: speed:" line's pictures.)
    {
        static const bool test = getenv("EMUSUNI_VITA_TEST_FPS") != nullptr;
        static unsigned asked = 0;
        if (test) {
            const unsigned rate = emusuni_vita_pictures_per_second();
            if (++asked % 300 == 0) {
                fprintf(stderr, "[emusuni-vita] test: fps: %u pictures a second now, %llu in all\n", rate, emusuni_vita_pictures_total());
            }
        }
    }
#endif
    static uint64_t said[kKinds][2];
    static uint64_t marks_from = 0;                // the instructions without fast memory when the game started
    static unsigned long long faults_said[2];      // the guest memory's faults told already
    static unsigned lines = 0;                     // lines since the game started (the memory's line: one in six)
    const uint64_t now_ns = MonotonicNs();
    // (No refresh for two seconds: the app's menu was open, or a state was loaded. That stretch isn't the
    // game's speed: the count starts again.)
    const bool paused = last_ns != 0 && now_ns - last_ns > 2000000000ull;
    last_ns = now_ns;
    const bool restart = s_restart.exchange(false, std::memory_order_relaxed);
    if (restart || paused) {
        from_ns = now_ns;
        from_ticks = __rdtsc();
        refreshes = 0;
        if (restart) {
            marks_from = s_kinds[kFastMark].count.load(std::memory_order_relaxed);
            lines = 0;
        }
        emusuni_vita_fault_counts(&faults_said[0], &faults_said[1]);
        for (Call& call : s_calls) {
            call.count_said = call.count.load(std::memory_order_relaxed);
            call.ticks_said = call.ticks.load(std::memory_order_relaxed);
        }
        for (int k = 0; k < kKinds; k++) {
            said[k][0] = s_kinds[k].count.load(std::memory_order_relaxed);
            said[k][1] = s_kinds[k].ticks.load(std::memory_order_relaxed);
        }
        for (Slot& slot : s_slots) {
            const int state = slot.state.load(std::memory_order_acquire);
            if (state == 1 || state == 2) {
                slot.guest_said = slot.guest.load(std::memory_order_relaxed);
                slot.calls_said = slot.calls.load(std::memory_order_relaxed);
                slot.count_said = slot.count.load(std::memory_order_relaxed);
                slot.cpu_said = slot.cpu_ns.load(std::memory_order_relaxed);
                if (state == 2) {
                    slot.state.store(0, std::memory_order_release);
                }
            }
        }
        return;
    }
    refreshes++;
    if (now_ns - from_ns < 10000000000ull) {
        return;
    }
    const uint64_t now_ticks = __rdtsc();
    const double seconds = static_cast<double>(now_ns - from_ns) / 1e9;
    const double ticks_per_s = static_cast<double>(now_ticks - from_ticks) / seconds;
    if (ticks_per_s > 1e8) {
        s_ticks_per_s.store(static_cast<uint64_t>(ticks_per_s), std::memory_order_relaxed);
    }
    const auto s = [&](uint64_t ticks) { return static_cast<double>(ticks) / ticks_per_s; };

    uint64_t count[kKinds], ticks[kKinds];
    for (int k = 0; k < kKinds; k++) {
        const uint64_t c = s_kinds[k].count.load(std::memory_order_relaxed);
        const uint64_t t = s_kinds[k].ticks.load(std::memory_order_relaxed);
        count[k] = c - said[k][0];
        ticks[k] = t - said[k][1];
        said[k][0] = c;
        said[k][1] = t;
    }

    struct Busy {
        Slot* slot;
        double guest, calls, cpu;
        uint64_t count;
        uint32_t cores;
        bool ended;
    };
    Busy busy[kSlots];
    int threads = 0, ended = 0;
    double guest_all = 0, calls_all = 0, cpu_all = 0;
    uint64_t calls_count = 0;
    uint32_t cores_all = 0;
    const bool cpu_known = s_cpu_clock.load(std::memory_order_relaxed) >= 0;
    for (Slot& slot : s_slots) {
        const int state = slot.state.load(std::memory_order_acquire);
        if (state != 1 && state != 2) {
            continue;
        }
        const uint64_t g = slot.guest.load(std::memory_order_relaxed);
        const uint64_t c = slot.calls.load(std::memory_order_relaxed);
        const uint64_t n = slot.count.load(std::memory_order_relaxed);
        const uint64_t cpu = slot.cpu_ns.load(std::memory_order_relaxed);
        Busy& b = busy[threads++];
        b.slot = &slot;
        b.guest = s(g - slot.guest_said);
        b.calls = s(c - slot.calls_said);
        b.cpu = cpu > slot.cpu_said ? static_cast<double>(cpu - slot.cpu_said) / 1e9 : 0;
        b.cores = slot.cores.exchange(0, std::memory_order_relaxed);
        b.ended = state == 2;
        b.count = n - slot.count_said;
        calls_count += b.count;
        slot.guest_said = g;
        slot.calls_said = c;
        slot.count_said = n;
        slot.cpu_said = cpu;
        guest_all += b.guest;
        calls_all += b.calls;
        cpu_all += b.cpu;
        cores_all |= b.cores;
        if (state == 2) {
            ended++;
        }
    }

    char line[500];
    int at = snprintf(line, sizeof(line),
                      "vita: speed: %.1f s: the game showed %llu pictures, %.1f a second (%llu refreshes of the screen); "
                      "its %d threads (%d ended): %.2f s in guest code, %.2f s in %llu system calls (their waits too)",
                      seconds, static_cast<unsigned long long>(count[kPicture]),
                      static_cast<double>(count[kPicture]) / seconds, static_cast<unsigned long long>(refreshes), threads,
                      ended, guest_all, calls_all, static_cast<unsigned long long>(calls_count));
    if (cpu_known && at > 0 && at < static_cast<int>(sizeof(line))) {
        snprintf(line + at, sizeof(line) - static_cast<size_t>(at), ", %.2f s of CPU = %.2f cores busy (seen on cores %#x)",
                 cpu_all, cpu_all / seconds, cores_all);
    }
    emusuni_vita_log("%s", line);
    emusuni_vita_log("vita: speed: in that: the recompiler made %llu blocks in %.2f s (a thread's code memory full and emptied %llu times); %llu shaders in %.2f s and %llu "
                     "pipelines in %.2f s (its shaders' time in a pipeline's); the renderer worked %.2f s on %llu "
                     "command lists; %llu files opened in %.2f s",
                     static_cast<unsigned long long>(count[kBlock]), s(ticks[kBlock]), static_cast<unsigned long long>(count[kFull]),
                     static_cast<unsigned long long>(count[kShader]), s(ticks[kShader]),
                     static_cast<unsigned long long>(count[kPipeline]), s(ticks[kPipeline]), s(ticks[kBatch]),
                     static_cast<unsigned long long>(count[kBatch]), static_cast<unsigned long long>(count[kFile]),
                     s(ticks[kFile]));
    // (The presses by button: SCE_CTRL_*'s bits 0-15, by the PS Vita's names; l and r are its own two, l1 and r1
    // the PS TV's second pair.)
    char pressed[200] = "";
    {
        static const char* const kButton[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                                "l", "r", "l1", "r1", "triangle", "circle", "cross", "square"};
        size_t used = 0;
        for (int bit = 0; bit < 16; bit++) {
            const uint32_t presses = s_pad_presses[bit].exchange(0, std::memory_order_relaxed);
            if (presses == 0) {
                continue;
            }
            const int wrote = snprintf(pressed + used, sizeof(pressed) - used, "%s%s %u", used ? ", " : "", kButton[bit],
                                       static_cast<unsigned>(presses));
            if (wrote > 0 && static_cast<size_t>(wrote) < sizeof(pressed) - used) {
                used += static_cast<size_t>(wrote);
            }
        }
    }
    emusuni_vita_log("vita: gfx: in that: %llu painted surfaces copied to be sampled, %llu draws reading the colour already "
                     "painted, %llu transfers through the guest's memory (%llu copies, %llu downscales, %llu fills); "
                     "the pad had a button down in %llu refreshes, the touch screen a finger in %llu; presses: %s",
                     static_cast<unsigned long long>(count[kSurfaceCopy]), static_cast<unsigned long long>(count[kFetchDraw]),
                     static_cast<unsigned long long>(count[kXferCopy] + count[kXferDown] + count[kXferFill]),
                     static_cast<unsigned long long>(count[kXferCopy]), static_cast<unsigned long long>(count[kXferDown]),
                     static_cast<unsigned long long>(count[kXferFill]), static_cast<unsigned long long>(count[kPad]),
                     static_cast<unsigned long long>(count[kTouch]), pressed[0] != 0 ? pressed : "none");
    if (count[kSurfaceTaken] + count[kSurfaceWritten] + count[kSurfaceOther] != 0) {
        emusuni_vita_log("vita: gfx: in that: textures that lie on a painted surface: %llu taken from one (%llu of them from another "
                         "than the nearest: the surfacepick switch), %llu not taken (laid out otherwise: read from the game's "
                         "memory), %llu not taken (the game had written to the surface since it was painted)",
                         static_cast<unsigned long long>(count[kSurfaceTaken]), static_cast<unsigned long long>(count[kSurfacePicked]),
                         static_cast<unsigned long long>(count[kSurfaceOther]), static_cast<unsigned long long>(count[kSurfaceWritten]));
    }

    // (After 01.68) How far behind the game the renderer's thread runs, and what the draws' copies cost.
    {
        const uint64_t most = s_list_wait_most.exchange(0, std::memory_order_relaxed);
        const uint64_t lists = count[kListWait];
        emusuni_vita_log("vita: draws: in that: the renderer's thread got to a list of drawing commands %.1f ms after the "
                         "game sent it (%.1f ms at most; %llu lists); copied as the game asked for the draws (the "
                         "drawcopy switch): %llu uniform buffers (%llu KiB), %llu vertex streams (%llu KiB), %llu draws' "
                         "indices (%llu KiB); vertex streams padded to a multiple of 4 bytes (the vertexalign switch): %llu, "
                         "laid out anew (the vertexrelay switch): %llu",
                         lists != 0 ? 1000.0 * s(ticks[kListWait]) / static_cast<double>(lists) : 0.0, 1000.0 * s(most),
                         static_cast<unsigned long long>(lists), static_cast<unsigned long long>(count[kCopyUniform]),
                         static_cast<unsigned long long>(ticks[kCopyUniform] >> 10),
                         static_cast<unsigned long long>(count[kCopyVertex]), static_cast<unsigned long long>(ticks[kCopyVertex] >> 10),
                         static_cast<unsigned long long>(count[kCopyIndex]), static_cast<unsigned long long>(ticks[kCopyIndex] >> 10),
                         static_cast<unsigned long long>(count[kPadded]), static_cast<unsigned long long>(count[kRelaid]));
    }

    // (20-11, for 01.84) The renderer's thread, part by part (see kRDraw). Said when it drew.
    if (count[kRDraw] != 0) {
        const auto us = [&](uint64_t t, uint64_t n) { return n != 0 ? 1e6 * s(t) / static_cast<double>(n) : 0.0; };
        const auto ms = [&](uint64_t t) { return 1000.0 * s(t); };
        const uint64_t draws = count[kRDraw];
        const uint64_t parts = ticks[kRDrawPipe] + ticks[kRDrawBind] + ticks[kRDrawGeom];
        emusuni_vita_log("vita: renderer: in that: %llu draws, %.1f us each (pipeline %.1f, uniforms and textures %.1f, "
                         "indices and vertices %.1f, the rest %.1f); its lists' commands: draws %.0f ms (%llu), state "
                         "%.0f ms (%llu), scenes %.0f ms (%llu), others %.0f ms (%llu)",
                         static_cast<unsigned long long>(draws), us(ticks[kRDraw], draws), us(ticks[kRDrawPipe], draws),
                         us(ticks[kRDrawBind], draws), us(ticks[kRDrawGeom], draws),
                         us(ticks[kRDraw] > parts ? ticks[kRDraw] - parts : 0, draws),
                         ms(ticks[kRCmdDraw]), static_cast<unsigned long long>(count[kRCmdDraw]), ms(ticks[kRCmdState]),
                         static_cast<unsigned long long>(count[kRCmdState]), ms(ticks[kRCmdScene]),
                         static_cast<unsigned long long>(count[kRCmdScene]), ms(ticks[kRCmdOther]),
                         static_cast<unsigned long long>(count[kRCmdOther]));
        emusuni_vita_log("vita: renderer: in that: textures bound %llu (%.1f us each), hashed %llu (%llu KiB, %.1f us "
                         "each), sent to the GPU %llu (%llu KiB); vertices copied %llu KiB in %llu streams (%llu KiB of it "
                         "below the draw's lowest vertex), indices %llu KiB; submissions to the GPU %llu (%.1f us each, "
                         "%.1f of it waiting for the frontend's queue lock), the renderer waiting for them to be made %llu "
                         "times, %.0f ms (the submitthread switch); pictures copied for the frontend %llu, the renderer "
                         "waiting for the GPU's copy %.0f ms (%.2f ms each)",
                         static_cast<unsigned long long>(count[kRTexBind]), us(ticks[kRTexBind], count[kRTexBind]),
                         static_cast<unsigned long long>(count[kRTexHash]), static_cast<unsigned long long>(ticks[kRTexHashBytes] >> 10),
                         us(ticks[kRTexHash], count[kRTexHash]), static_cast<unsigned long long>(count[kRTexUpload]),
                         static_cast<unsigned long long>(ticks[kRTexUpload] >> 10), static_cast<unsigned long long>(ticks[kRVertex] >> 10),
                         static_cast<unsigned long long>(count[kRVertex]), static_cast<unsigned long long>(ticks[kRVertexBelow] >> 10),
                         static_cast<unsigned long long>(ticks[kRIndex] >> 10), static_cast<unsigned long long>(count[kRSubmit]),
                         us(ticks[kRSubmit], count[kRSubmit]), us(ticks[kRSubmitLock], count[kRSubmit]),
                         static_cast<unsigned long long>(count[kRSubmitWait]), ms(ticks[kRSubmitWait]),
                         static_cast<unsigned long long>(count[kRPresent]), ms(ticks[kRPresent]),
                         us(ticks[kRPresent], count[kRPresent]) / 1000.0);
    }

    // (20-11, for 01.86) The memory watches: what HENkaku-PS4's "memory watch:" line says. A range's watches growing
    // without end was 01.84-01.85's scene time growing (the "surfacewatch" switch). Said when there was any.
    {
        unsigned long long watch[6] = {0, 0, 0, 0, 0, 0};
        emusuni_vita_watch_stats(watch);
        if (watch[0] != 0 || watch[3] != 0) {
            emusuni_vita_log("vita: memory watch: in that: %llu protections (%llu on a range already watched; %llu earlier "
                             "watches of a surface made again dropped: the surfacewatch switch); now %llu ranges with %llu "
                             "watches, the most on one range %llu",
                             watch[0], watch[1], watch[2], watch[3], watch[4], watch[5]);
        }
    }

    // (After 01.70) A game's ZIP: what was inflated only to get somewhere. (Said when there was any.)
    if (ticks[kZipSkip] != 0 || count[kZipStart] != 0 || count[kZipMark] != 0) {
        emusuni_vita_log("vita: zip: in that: %llu MiB of the ZIP's compressed files inflated only to get to the place "
                         "a read asked for; streams started: %llu at a file's first byte, %llu from a mark (the zipmarks "
                         "switch)",
                         static_cast<unsigned long long>(ticks[kZipSkip] >> 20), static_cast<unsigned long long>(count[kZipStart]),
                         static_cast<unsigned long long>(count[kZipMark]));
    }

    // (After 01.71) The turns of the Vita's cores. (Said when there was any.)
    if (count[kTurnWait] != 0) {
        emusuni_vita_log("vita: cores: in that: threads tied to one of the Vita's cores took its turn from another %llu times "
                         "(%.1f ms waited in all): %llu from a thread inside a system call, %llu from one that had had its "
                         "10 ms, %llu by a higher priority; a mutex left for its waiter to take with the turn %llu times "
                         "(the coreturns switch)",
                         static_cast<unsigned long long>(count[kTurnWait]), 1000.0 * s(ticks[kTurnWait]),
                         static_cast<unsigned long long>(count[kTurnCall]), static_cast<unsigned long long>(count[kTurnTime]),
                         static_cast<unsigned long long>(count[kTurnPriority]), static_cast<unsigned long long>(count[kTurnRetry]));
    }

    // (17-11) The pages the emulator protects and the recompiler's fast memory.
    {
        unsigned long long handled = 0, first = 0;
        emusuni_vita_fault_counts(&handled, &first);
        emusuni_vita_log("vita: faults: in that: %llu faults of pages the emulator protects (%llu of them given to it before "
                         "the recompiler: keepfastmem); the recompiler took %llu faults of its code for bad addresses and "
                         "made %llu instructions again without fast memory (%llu since the game started)",
                         handled - faults_said[0], first - faults_said[1], static_cast<unsigned long long>(count[kFastFail]),
                         static_cast<unsigned long long>(count[kFastMark]),
                         static_cast<unsigned long long>(s_kinds[kFastMark].count.load(std::memory_order_relaxed) - marks_from));
        faults_said[0] = handled;
        faults_said[1] = first;
        emusuni_vita_log("vita: jit: in that: %llu memory accesses of recompiled code through a call of the emulator's "
                         "(the rest go straight to the memory), %llu blocks looked up by the dispatcher, %llu system "
                         "calls answered inside the recompiler in %.2f s",
                         static_cast<unsigned long long>(count[kMemCall]), static_cast<unsigned long long>(count[kLookup]),
                         static_cast<unsigned long long>(count[kInline]), s(ticks[kInline]));
    }

    // (17-11) The system's calls by name: the costliest and the most made.
    {
        struct Made {
            uint32_t nid;
            uint64_t count, ticks;
        };
        static Made made[kCalls];
        int n = 0;
        for (Call& call : s_calls) {
            const uint32_t nid = call.nid.load(std::memory_order_relaxed);
            if (nid == 0) {
                continue;
            }
            const uint64_t c = call.count.load(std::memory_order_relaxed);
            const uint64_t t = call.ticks.load(std::memory_order_relaxed);
            if (c != call.count_said) {
                made[n++] = Made{nid, c - call.count_said, t - call.ticks_said};
            }
            call.count_said = c;
            call.ticks_said = t;
        }
        const auto name_of = s_call_name.load(std::memory_order_relaxed);
        for (int pass = 0; pass < 2 && n > 0; pass++) {
            const int most = std::min(n, pass == 0 ? 6 : 5);
            std::partial_sort(made, made + most, made + n, [&](const Made& a, const Made& b) {
                return pass == 0 ? a.ticks > b.ticks : a.count > b.count;
            });
            at = snprintf(line, sizeof(line), "%s",
                          pass == 0 ? "vita: calls: the costliest (their waits too):" : "vita: calls: the most made:");
            for (int i = 0; i < most && at > 0 && at < static_cast<int>(sizeof(line)) - 80; i++) {
                const char* const name = name_of != nullptr ? name_of(made[i].nid) : nullptr;
                char number[16];
                snprintf(number, sizeof(number), "%#x", made[i].nid);
                at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "%s %.40s %llu in %.2f s", i > 0 ? ";" : "",
                               name != nullptr && name[0] != 0 ? name : number, static_cast<unsigned long long>(made[i].count),
                               s(made[i].ticks));
            }
            emusuni_vita_log("%s", line);
        }
        // (After 01.67) Every call of the window, by how many, for a game that waits for something the two lines
        // above don't show (LittleBigPlanet at its "Connecting" screen): with the empty file
        // /data/EmuSuNi/vita-calls-all, looked for at each line; the PC: EMUSUNI_VITA_CALLS_ALL.
#if defined(__ORBIS__)
        const bool all = access("/data/EmuSuNi/vita-calls-all", F_OK) == 0;
#else
        const bool all = getenv("EMUSUNI_VITA_CALLS_ALL") != nullptr;
#endif
        if (all && n > 0) {
            std::sort(made, made + n, [](const Made& a, const Made& b) { return a.count > b.count; });
            for (int i = 0; i < n;) {
                at = snprintf(line, sizeof(line), "vita: calls: all:");
                for (; i < n && at > 0 && at < static_cast<int>(sizeof(line)) - 64; i++) {
                    const char* const name = name_of != nullptr ? name_of(made[i].nid) : nullptr;
                    char number[16];
                    snprintf(number, sizeof(number), "%#x", made[i].nid);
                    at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %.44s %llu;",
                                   name != nullptr && name[0] != 0 ? name : number, static_cast<unsigned long long>(made[i].count));
                }
                emusuni_vita_log("%s", line);
            }
        }
    }

    // (17-11) The guest's memory and the 2 MiB pages, a line in six.
    if (lines++ % 6 == 0) {
        unsigned pieces = 0, whole = 0;
        int large = -1;
        emusuni_vita_mem_large(&pieces, &whole, &large);
        if (large >= 0) {
            emusuni_vita_log("vita: memory: %u MiB under the guest's addresses in %u pieces of 2 MiB: %u whole (every page "
                             "readable and writable), %d one 2 MiB page for the kernel",
                             pieces * 2, pieces, whole, large);
        } else {
            emusuni_vita_log("vita: memory: %u MiB under the guest's addresses in %u pieces of 2 MiB: %u whole (every page "
                             "readable and writable); the kernel here doesn't say which are one 2 MiB page",
                             pieces * 2, pieces, whole);
        }
    }

    // The busiest five: by CPU time where it is told, else by the time in guest code.
    const auto weight = [&](const Busy& b) { return cpu_known ? b.cpu : b.guest; };
    std::sort(busy, busy + threads, [&](const Busy& a, const Busy& b) { return weight(a) > weight(b); });
    at = snprintf(line, sizeof(line), "vita: threads:");
    for (int i = 0; i < threads && i < 5 && at > 0 && at < static_cast<int>(sizeof(line)) - 125; i++) {
        const Busy& b = busy[i];
        if (weight(b) < 0.005 && i > 0) {
            break;
        }
        at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "%s \"%s\"%s ", i > 0 ? ";" : "", b.slot->name,
                       b.ended ? " (ended)" : "");
        if (cpu_known) {
            at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "%.0f%% of a core (", 100.0 * b.cpu / seconds);
        }
        at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "guest code %.2f s, %llu calls %.2f s", b.guest,
                       static_cast<unsigned long long>(b.count), b.calls);
        if (cpu_known) {
            at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), ", cores %#x)", b.cores);
        }
    }
    const uint64_t unlisted = s_unlisted.exchange(0, std::memory_order_relaxed);
    if (unlisted != 0 && at > 0 && at < static_cast<int>(sizeof(line)) - 60) {
        snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "; %llu more threads not counted",
                 static_cast<unsigned long long>(unlisted));
    }
    emusuni_vita_log("%s", line);

    // (After 01.68) The threads that have been inside one system call for five seconds or more, the longest first:
    // what a game that stopped is waiting for. (A game's idle workers are here too, always: it is the thread that
    // should be running - the game's own, named by its title's code - that tells.)
    {
        struct Waiting {
            const Slot* slot;
            uint32_t nid;
            double seconds;
        };
        Waiting waiting[kSlots];
        int n = 0;
        for (int i = 0; i < threads; i++) {
            const Slot* const slot = busy[i].slot;
            const uint32_t nid = slot->in_nid.load(std::memory_order_relaxed);
            const uint64_t since = slot->in_since.load(std::memory_order_relaxed);
            if (busy[i].ended || nid == 0 || since == 0 || now_ticks <= since || s(now_ticks - since) < 5.0) {
                continue;
            }
            // (Read without a lock: the call may have ended and another begun in between. Asked again, it must agree.)
            if (slot->in_nid.load(std::memory_order_relaxed) != nid || slot->in_since.load(std::memory_order_relaxed) != since) {
                continue;
            }
            waiting[n++] = Waiting{slot, nid, s(now_ticks - since)};
        }
        if (n > 0) {
            std::sort(waiting, waiting + n, [](const Waiting& a, const Waiting& b) { return a.seconds > b.seconds; });
            const auto name_of = s_call_name.load(std::memory_order_relaxed);
            at = snprintf(line, sizeof(line), "vita: waits: %d threads inside one call for 5 s or more:", n);
            for (int i = 0; i < n && at > 0 && at < static_cast<int>(sizeof(line)) - 90; i++) {
                const char* const name = name_of != nullptr ? name_of(waiting[i].nid) : nullptr;
                char number[16];
                snprintf(number, sizeof(number), "%#x", waiting[i].nid);
                at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "%s \"%s\" in %.40s %.0f s", i > 0 ? ";" : "",
                               waiting[i].slot->name, name != nullptr && name[0] != 0 ? name : number, waiting[i].seconds);
            }
            emusuni_vita_log("%s", line);
            // (After 01.70) Those of them that wait for a mutex: whose it is. (A line of its own, when there is any.)
            at = 0;
            for (int i = 0; i < n && at < static_cast<int>(sizeof(line)) - 110; i++) {
                const int32_t owner = waiting[i].slot->wait_owner.load(std::memory_order_relaxed);
                if (owner == 0) {
                    continue;
                }
                const char* holder = "a thread that is not in the lines";
                for (const Slot& slot : s_slots) {
                    const int state = slot.state.load(std::memory_order_acquire);
                    if ((state == 1 || state == 2) && slot.id == owner) {
                        holder = slot.name;
                        break;
                    }
                }
                const int wrote = snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "%s \"%s\" waits for a mutex that \"%.28s\" (thread %d) holds %d times",
                                           at > 0 ? ";" : "vita: waits: for a mutex:", waiting[i].slot->name, holder, static_cast<int>(owner),
                                           static_cast<int>(waiting[i].slot->wait_count.load(std::memory_order_relaxed)));
                if (wrote <= 0) {
                    break;
                }
                at += wrote;
            }
            if (at > 0) {
                emusuni_vita_log("%s", line);
            }
        }
    }

    // (Only the ended threads this line counted: one that ended since keeps its last numbers for the next.)
    for (int i = 0; i < threads; i++) {
        if (busy[i].ended) {
            busy[i].slot->state.store(0, std::memory_order_release);
        }
    }
    from_ns = now_ns;
    from_ticks = now_ticks;
    refreshes = 0;
}
