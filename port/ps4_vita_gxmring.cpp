// EmuSuNi's PS Vita port (19-11, the performance specialist's; for the Vita specialist to take in): where the GPU
// library's commands to the renderer come from - the switch "gxmring" (the kernel's side: scripts/build-vita-core.sh,
// SceGxm.cpp's SceGxmContext::allocate_new_command and free_new_command).
//
// Why: a draw is 8-15 commands; an immediate context takes them from a ring in the game's VDM buffer, of its size
// (128 KiB = ~2,340 commands by default), and when that ring is used up - Uncharted makes ~7,800 commands a picture,
// with the renderer up to a picture behind - each command is a new (the app's malloc: one lock for every thread) in the
// game's thread and a delete in the renderer's. On the console a draw costs 8.6 microseconds (Uncharted, 01.78), 19
// times the PC's.
//
// What (the port's table, ps4_vita_sys.h: "gxmringcount", 49, and "gxmring", 48, both OFF):
//   "count" (the file vita-gxmringcount-on; the PC EMUSUNI_VITA_GXMRINGCOUNT=1): the same ways as ever, counted: a line every 2^20 commands
//           says how many came from the game's ring and how many with new.
//   on      (the file vita-gxmring-on; the PC EMUSUNI_VITA_GXMRING=1): each immediate context gets a ring of its own of 32,768 commands
//           (the emulator's memory, not the game's), used first; full, the game's ring and then new as ever. The
//           renderer frees in the order it got them, as with the game's ring: a count of frees. And the thread that
//           makes commands and the renderer's each get an arena of the app's heap of their own (ps4_heap_thread_arena,
//           as the 3DS's threads). Counted as above, with the own ring's share.
// Read once, at the first command.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unistd.h>

#include "ps4_vita_sys.h"

#if defined(__ORBIS__)
extern "C" int ps4_heap_thread_arena(const char* name);  // (port/ps4_runtime.cpp)
#else
static int ps4_heap_thread_arena(const char*) {
    return 0;  // (the PC's heap: no arenas)
}
#endif

namespace {

constexpr size_t kRingCommands = 32768;
constexpr int kRings = 4;

struct Ring {
    std::atomic<const void*> ctx{nullptr};
    unsigned char* begin = nullptr;
    unsigned char* end = nullptr;
    size_t size = 0;                         // one command's bytes
    uint64_t next = 0;                       // (the context's thread only)
    std::atomic<uint64_t> last_free{kRingCommands - 1};
};
Ring s_rings[kRings];

std::atomic<int> s_mode{-1};  // -1 not read, 0 off, 1 count, 2 on
std::atomic<uint64_t> s_count[3];  // own ring, the game's ring, new
std::atomic<int> s_arenas{0};

int Mode() {
    int mode = s_mode.load(std::memory_order_acquire);
    if (mode >= 0) {
        return mode;
    }
    int want = 0;
    if (emusuni_vita_switch(EMUSUNI_VITA_GXMRING) != 0) {
        want = 2;
    } else if (emusuni_vita_switch(EMUSUNI_VITA_GXMRINGCOUNT) != 0) {
        want = 1;
    }
    int none = -1;
    if (s_mode.compare_exchange_strong(none, want)) {
        emusuni_vita_log("vita: gxmring: the GPU library's commands: %s",
                         want == 2 ? "a ring of the emulator's own (32768 commands) first, and arenas of their own for the "
                                     "threads that make and free them (the file vita-gxmring-on; read once)"
                         : want == 1 ? "as ever, counted (the file vita-gxmringcount-on; read once)"
                                     : "as ever (the files vita-gxmring-on / vita-gxmringcount-on change it; read once)");
    }
    return s_mode.load(std::memory_order_acquire);
}

void Count(int how) {
    s_count[how].fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint64_t> total{0};
    if ((total.fetch_add(1, std::memory_order_relaxed) & 0xFFFFF) == 0xFFFFF) {
        const uint64_t own = s_count[0].load(), game = s_count[1].load(), heap = s_count[2].load();
        const uint64_t all = own + game + heap;
        emusuni_vita_log("vita: gxmring: %llu commands so far: %llu from the own ring, %llu from the game's, %llu with new "
                         "(%.1f%%); %d threads with an arena of their own",
                         (unsigned long long)all, (unsigned long long)own, (unsigned long long)game, (unsigned long long)heap,
                         all ? 100.0 * static_cast<double>(heap) / static_cast<double>(all) : 0.0, s_arenas.load());
    }
}

Ring* RingOf(const void* ctx, size_t size) {
    for (Ring& r : s_rings) {
        if (r.ctx.load(std::memory_order_acquire) == ctx) {
            return &r;
        }
    }
    for (Ring& r : s_rings) {
        const void* none = nullptr;
        if (r.ctx.load(std::memory_order_relaxed) == nullptr && r.ctx.compare_exchange_strong(none, ctx)) {
            void* const memory = aligned_alloc(64, ((kRingCommands * size) + 63) & ~static_cast<size_t>(63));
            if (memory == nullptr) {
                return nullptr;
            }
            r.size = size;
            r.begin = static_cast<unsigned char*>(memory);
            r.end = r.begin + kRingCommands * size;
            emusuni_vita_log("vita: gxmring: a ring of %zu commands (%zu KiB) for the context at %p", kRingCommands,
                             kRingCommands * size / 1024, ctx);
            return &r;
        }
    }
    return nullptr;  // (more contexts than rings: as ever)
}

}  // namespace

// 0 off, 1 count only, 2 on.
extern "C" int emusuni_vita_gxmring_mode(void) {
    return Mode();
}

// A command of the own ring for this immediate context, or nullptr (off, full, or no ring left).
extern "C" void* emusuni_vita_gxmring_alloc(const void* ctx, size_t size) {
    thread_local bool armed = false;
    if (!armed) {
        armed = true;
        if (ps4_heap_thread_arena("vita gxm commands") != 0) {
            s_arenas.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Ring* const r = RingOf(ctx, size);
    if (r == nullptr || r->begin == nullptr || r->size != size) {
        return nullptr;
    }
    if (r->next > r->last_free.load(std::memory_order_acquire)) {
        return nullptr;
    }
    void* const at = r->begin + (r->next % kRingCommands) * size;
    r->next++;
    Count(0);
    return at;
}

// A command freed (the renderer's thread): 1 when it was the own ring's.
extern "C" int emusuni_vita_gxmring_free(const void* command) {
    thread_local bool armed = false;
    if (!armed) {
        armed = true;
        if (ps4_heap_thread_arena("vita renderer") != 0) {
            s_arenas.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const unsigned char* const at = static_cast<const unsigned char*>(command);
    for (Ring& r : s_rings) {
        if (r.begin != nullptr && at >= r.begin && at < r.end) {
            r.last_free.fetch_add(1, std::memory_order_release);
            return 1;
        }
    }
    return 0;
}

// (19-11, the Vita specialist's: the switch "gxmpacket", 50) As emusuni_vita_gxmring_free, but the own ring's frees kept
// by this thread and given back every 64 commands and at the end of each list (emusuni_vita_gxmring_publish): the
// line the game's thread reads at every command is written 64 times less.
namespace {
thread_local Ring* t_kept_ring = nullptr;
thread_local uint64_t t_kept = 0;
void GiveKept() {
    if (t_kept_ring != nullptr && t_kept != 0) {
        t_kept_ring->last_free.fetch_add(t_kept, std::memory_order_release);
    }
    t_kept = 0;
}
}  // namespace

extern "C" int emusuni_vita_gxmring_free_later(const void* command) {
    thread_local bool armed = false;
    if (!armed) {
        armed = true;
        if (ps4_heap_thread_arena("vita renderer") != 0) {
            s_arenas.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const unsigned char* const at = static_cast<const unsigned char*>(command);
    for (Ring& r : s_rings) {
        if (r.begin != nullptr && at >= r.begin && at < r.end) {
            if (t_kept_ring != &r) {
                GiveKept();
                t_kept_ring = &r;
            }
            if (++t_kept >= 64) {
                GiveKept();
            }
            return 1;
        }
    }
    return 0;
}

// The own ring's frees this thread kept back given now (the end of a list).
extern "C" void emusuni_vita_gxmring_publish(void) {
    GiveKept();
}

// The ways as ever, counted: 1 the game's ring, 2 new.
extern "C" void emusuni_vita_gxmring_count(int how) {
    Count(how == 1 ? 1 : 2);
}
