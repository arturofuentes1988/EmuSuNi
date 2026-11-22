// EmuSuNi's PS Vita port (18-11, the performance specialist's, taken in with the set 0177): the two calls a
// thread makes to know itself, answered at once inside the recompiler's call for them (the kernel's side: in
// scripts/build-vita-core.sh, kernel.cpp's emusuni_inline_svc: emusuni_ids_fast).
//
// Why: Uncharted: Golden Abyss's main thread ("PCSA00029", 98-99% of a PS4 core, 01.75) makes ~1.3 million
// sceKernelGetTLSAddr and ~1.1 million sceKernelGetThreadId calls every 10 s. With "fastcalls" they are answered
// inside the recompiler already, but each still goes through Vita3K's export: the TLS one finds the thread under the
// kernel's lock every time (KernelState::get_thread: a lock, a map, a shared pointer) - 0.86 s and 0.22 s of every 10
// on the console, ~11% of that thread.
//
// What: sceKernelGetThreadId: the guest thread's id, which the recompiler's state has. sceKernelGetTLSAddr(key): the
// thread's TLS block (ThreadState::tls, made with the thread and the same for its whole life) + 4 x key, the block's
// address kept for the host thread the first time (every guest thread is a host thread of its own). A key out of
// 0..0x100, or a thread not found, goes the ordinary way (its export).
//
// Switch: "fastids" of the port's table (ps4_vita_sys.h; 39, OFF): the empty file /data/EmuSuNi/vita-fastids-on (the
// PC: EMUSUNI_VITA_FASTIDS=1), read at each game's start; a line "vita: fastids: ..." in the log says which.

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
    {"sceKernelGetTLSAddr", 1},
    {"sceKernelGetThreadId", 2},
};

bool s_on = false;
const char* (*s_name)(unsigned) = nullptr;

// What each call's number is, found out once: a table without a lock (as port/ps4_vita_lwfast.cpp's).
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
    return emusuni_vita_switch(EMUSUNI_VITA_FASTIDS) != 0;
}

}  // namespace

// A game starts (the kernel is made): the switch and the table as they are now.
extern "C" void emusuni_vita_ids_start(const char* (*name_of)(unsigned)) {
    for (Slot& slot : s_slots) {
        slot.kind.store(-1, std::memory_order_relaxed);
        slot.nid.store(0, std::memory_order_release);
    }
    s_name = name_of;
    s_on = SwitchOn();
    emusuni_vita_log("vita: fastids: sceKernelGetTLSAddr and sceKernelGetThreadId answered at once from the thread's own: %s",
                     s_on ? "on (the file vita-fastids-on)" : "off (the file vita-fastids-on makes it on)");
}

extern "C" int emusuni_vita_ids_on(void) {
    return s_on ? 1 : 0;
}

// 0: not one of these (or the switch is off); 1 sceKernelGetTLSAddr, 2 sceKernelGetThreadId.
extern "C" int emusuni_vita_ids_kind(unsigned nid) {
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
