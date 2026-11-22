// EmuSuNi's PS Vita port (19-11, the performance specialist's; for the Vita specialist to take in): the GPU library's
// simplest calls answered inside the recompiler's call for them (the kernel's side: scripts/build-vita-core.sh,
// kernel.cpp's emusuni_inline_svc; the getters themselves: SceGxm.cpp's emusuni_gxm_get, on Vita3K's own structures).
//
// Why: MotorStorm RC's main thread ("PCSC00014", 01.78) makes ~1.96 million sceGxmProgramParameterGet{Type,
// ComponentCount,ArraySize,ResourceIndex} calls every 10 s - each a field of a 16-byte structure -, every one a full
// stop and start of the recompiled code (~1.1 microseconds on the PS4) and Vita3K's export (0.33 microseconds):
// ~2.7 s of every 10 of that thread. Uncharted's main thread: ~250 thousand
// sceGxmPrecomputedVertexStateGetDefaultUniformBuffer and ~275 thousand sceGxmSetUniformDataF. On the Vita these are
// the library's own functions, not system calls.
//
// Which (by the export's name):
//   1-11, answered here (SceGxm.cpp's emusuni_gxm_get: the same lines as the exports, on the same structures): the
//   program parameter's Type, ComponentCount, ArraySize, ResourceIndex, Category, ContainerIndex, SemanticIndex,
//   Semantic; the precomputed vertex and fragment states' default uniform buffer; a program's parameter count. A null
//   address goes the ordinary way (the export).
//   100, the export itself called from there (it neither waits, nor locks, nor runs the game's code):
//   sceGxmSetUniformDataF, sceGxmProgramFindParameterBySemantic (and _), sceGxmProgramFindParameterByName,
//   sceGxmProgramGetParameter, sceGxmProgramParameterGetName, sceGxmProgramParameterGetIndex.
//
// Switch: "gxmfast" of the port's table (ps4_vita_sys.h; 42, OFF): the empty file /data/EmuSuNi/vita-gxmfast-on (the
// PC: EMUSUNI_VITA_GXMFAST=1), read at each game's start; a line "vita: gxmfast: ..." in the log says which. On the PC, EMUSUNI_VITA_GXMFAST_CHECK=1 makes
// every answer of 1-11 the export's too, and the log says how many differed.

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
    {"sceGxmProgramParameterGetType", 1},
    {"sceGxmProgramParameterGetComponentCount", 2},
    {"sceGxmProgramParameterGetArraySize", 3},
    {"sceGxmProgramParameterGetResourceIndex", 4},
    {"sceGxmProgramParameterGetCategory", 5},
    {"sceGxmProgramParameterGetContainerIndex", 6},
    {"sceGxmProgramParameterGetSemanticIndex", 7},
    {"sceGxmProgramParameterGetSemantic", 8},
    {"sceGxmPrecomputedVertexStateGetDefaultUniformBuffer", 9},
    {"sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer", 10},
    {"sceGxmProgramGetParameterCount", 11},
    {"sceGxmSetUniformDataF", 100},
    {"sceGxmProgramFindParameterBySemantic", 100},
    {"_sceGxmProgramFindParameterBySemantic", 100},
    {"sceGxmProgramFindParameterByName", 100},
    {"sceGxmProgramGetParameter", 100},
    {"sceGxmProgramParameterGetName", 100},
    {"sceGxmProgramParameterGetIndex", 100},
};

bool s_on = false;
const char* (*s_name)(unsigned) = nullptr;

// What each call's number is, found out once: a table without a lock (as port/ps4_vita_lwfast.cpp's).
struct Slot {
    std::atomic<uint32_t> nid{0};
    std::atomic<int16_t> kind{-1};
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
    return emusuni_vita_switch(EMUSUNI_VITA_GXMFAST) != 0;
}

}  // namespace

// A game starts (the kernel is made): the switch and the table as they are now.
extern "C" void emusuni_vita_gxm_start(const char* (*name_of)(unsigned)) {
    for (Slot& slot : s_slots) {
        slot.kind.store(-1, std::memory_order_relaxed);
        slot.nid.store(0, std::memory_order_release);
    }
    s_name = name_of;
    s_on = SwitchOn();
    emusuni_vita_log("vita: gxmfast: the GPU library's simplest calls (program parameters' getters, sceGxmSetUniformDataF, ...) "
                     "answered inside the recompiler: %s", s_on ? "on (the file vita-gxmfast-on)" : "off (the file vita-gxmfast-on makes it on)");
}

extern "C" int emusuni_vita_gxm_on(void) {
    return s_on ? 1 : 0;
}

// 0: not one of these (or the switch is off); 1-11 a getter answered by emusuni_gxm_get; 100 the export, from there.
extern "C" int emusuni_vita_gxm_kind(unsigned nid) {
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
                slot.kind.store(static_cast<int16_t>(kind), std::memory_order_release);
                return kind;
            }
            there = none;
        }
        if (there == nid) {
            const int16_t kind = slot.kind.load(std::memory_order_acquire);
            return kind >= 0 ? kind : Decide(nid);
        }
    }
    return Decide(nid);
}

// (The PC's check, EMUSUNI_VITA_GXMFAST_CHECK=1: kernel.cpp counts here, the log says it now and then.)
extern "C" void emusuni_vita_gxm_checked(int kind, unsigned mine, unsigned export_said) {
    static std::atomic<uint64_t> checked{0}, differ{0};
    if (mine != export_said && differ.fetch_add(1) < 20) {
        emusuni_vita_log("vita: gxmfast: check: kind %d answered %u here, the export %u", kind, mine, export_said);
    }
    if ((checked.fetch_add(1) & 0xFFFFF) == 0xFFFFF) {
        emusuni_vita_log("vita: gxmfast: check: %llu answers compared with the export's, %llu differ",
                         (unsigned long long)checked.load(), (unsigned long long)differ.load());
    }
}
