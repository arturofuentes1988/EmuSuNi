// EmuSuNi's PS Vita port (18-11, the performance specialist's, taken in with the set 0174): which supervisor
// calls are the lightweight mutexes' that the recompiler may answer itself when nothing has to wait (the kernel's
// side is in scripts/build-vita-core.sh, kernel.cpp's emusuni_inline_svc: emusuni_lw_fast).
//
// Why: in Street Fighter X Tekken's fight (01.71's console log, fastatomics on) the job threads make ~1.1 million
// sceKernelLockLwMutex / sceKernelUnlockLwMutex2 calls every 10 s, each a full stop and start of the recompiled code
// around Vita3K's own call (~1.1 microseconds on the PS4) - "Job Main" and "Job Worker 0" are the chain that keeps
// it under 60 pictures a second. Answered inside the recompiler's call for them, as the "fast calls" are, without the
// stop and the start; the mutex stays Vita3K's kernel object (owner, count, waiters, work area), the same calls on it
// as the exports make (Mutex::try_acquire, Mutex::release); a lock that would have to wait goes the ordinary way
// (its export, which waits).
//
// Which: by the export's name.
//   lock (1):    sceKernelLockLwMutex, sceKernelLockLwMutex_0, _sceKernelLockLwMutex - only when the mutex is free
//                or the thread's own (try_acquire OK); else the ordinary call. (Not the CB variants: their wait runs
//                callbacks first even when the mutex is free.)
//   try (2):     sceKernelTryLockLwMutex, sceKernelTryLockLwMutex_16XX - always (never waits).
//   unlock (3):  sceKernelUnlockLwMutex, sceKernelUnlockLwMutex_0, sceKernelUnlockLwMutex2 - always (never waits;
//                a waiter it hands the mutex to is woken as the export wakes it).
//
// Switch: "lwfast" of the port's table (ps4_vita_sys.h; 30, OFF): the empty file /data/EmuSuNi/vita-lwfast-on (the
// PC: EMUSUNI_VITA_LWFAST=1), read at each game's start; a line "vita: lwfast: ..." in the log says which.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "ps4_vita_sys.h"

namespace {

struct Name {
    const char* name;
    int kind;
};
constexpr Name kNames[] = {
    {"sceKernelLockLwMutex", 1},    {"sceKernelLockLwMutex_0", 1},          {"_sceKernelLockLwMutex", 1},
    {"sceKernelTryLockLwMutex", 2}, {"sceKernelTryLockLwMutex_16XX", 2},    {"sceKernelUnlockLwMutex", 3},
    {"sceKernelUnlockLwMutex_0", 3}, {"sceKernelUnlockLwMutex2", 3},
};

bool s_on = false;
const char* (*s_name)(unsigned) = nullptr;

// What each call's number is, found out once: a table without a lock (as port/ps4_vita_native.cpp's).
struct Slot {
    std::atomic<uint32_t> nid{0};
    std::atomic<int8_t> kind{-1};
};
constexpr uint32_t kSlots = 4096;
Slot s_slots[kSlots];

int Decide(uint32_t nid) {
    const char* const name = s_name != nullptr ? s_name(nid) : nullptr;
    if (name == nullptr) {
        return 0;
    }
    for (const Name& n : kNames) {
        if (std::strcmp(n.name, name) == 0) {
            return n.kind;
        }
    }
    return 0;
}

bool SwitchOn() {
    return emusuni_vita_switch(EMUSUNI_VITA_LWFAST) != 0;
}

}  // namespace

// A game starts (the kernel is made): the switch and the table as they are now.
extern "C" void emusuni_vita_lw_start(const char* (*name_of)(unsigned)) {
    for (Slot& slot : s_slots) {
        slot.kind.store(-1, std::memory_order_relaxed);
        slot.nid.store(0, std::memory_order_release);
    }
    s_name = name_of;
    s_on = SwitchOn();
    emusuni_vita_log("vita: lwfast: the lightweight mutexes' lock, try and unlock answered inside the recompiler when "
                     "nothing waits: %s", s_on ? "on (the file vita-lwfast-on)" : "off (the file vita-lwfast-on makes it on)");
}

extern "C" int emusuni_vita_lw_on(void) {
    return s_on ? 1 : 0;
}

// 0: not one of these (or the switch is off); 1 lock, 2 try, 3 unlock.
extern "C" int emusuni_vita_lw_kind(unsigned nid) {
    if (!s_on) {
        return 0;
    }
    uint32_t at = (nid * 2654435761u) >> 20;  // (12 bits)
    for (uint32_t tries = 0; tries < 32; tries++, at = (at + 1) & (kSlots - 1)) {
        Slot& slot = s_slots[at];
        uint32_t there = slot.nid.load(std::memory_order_acquire);
        if (there == 0) {
            const int kind = Decide(nid);
            uint32_t none = 0;
            if (slot.nid.compare_exchange_strong(none, nid, std::memory_order_acq_rel)) {
                slot.kind.store(static_cast<int8_t>(kind), std::memory_order_release);
                return kind;
            }
            there = none;
        }
        if (there == nid) {
            const int8_t kind = slot.kind.load(std::memory_order_acquire);
            return kind >= 0 ? kind : Decide(nid);
        }
    }
    return Decide(nid);
}
