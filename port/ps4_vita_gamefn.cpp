// EmuSuNi's PS Vita port (19-11, the performance specialist's; for the Vita specialist to take in): a game's own hot
// functions done natively - the switch "gamefn" (the kernel's side: scripts/build-vita-core.sh, dynarmic_cpu.cpp's
// PreCodeReadHook: a block that starts at one of them is a call of the native function and a return).
//
// Which (each found by its first bytes and a hash of all of its bytes, wherever the game has it; no game code here):
//   Uncharted: Golden Abyss's tree search by hash (0x816035B8 in its 1.00, 130 bytes of Thumb-2): 11-13% of the main
//   thread's recompiled code on the PC bench where it is used. It walks a node's entries (36 bytes each) for a key: a
//   group entry (flags & 0x200) marks the key seen when it is the key, and is searched into when its key is the key or
//   0x9945DA05; any other entry ends the search when its key is the key, or when the key was seen in a group and its
//   key is 0x9945DA05. It reads the guest's memory only and gives 0 or 1 in r0, as the game's code does (r1-r3 and the
//   flags left as they were: the ARM calling convention lets a callee change them, and the callers don't look).
//
// Switch: "gamefn" of the port's table (ps4_vita_sys.h; 47, OFF): the empty file /data/EmuSuNi/vita-gamefn-on (the PC:
// EMUSUNI_VITA_GAMEFN=1), read
// once, when the first block is translated; a line "vita: gamefn: ..." in the log says which, and each function found.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "ps4_vita_sys.h"

namespace {

inline uint32_t Rd32(const uint8_t* base, uint32_t at) {
    uint32_t v;
    std::memcpy(&v, base + at, 4);
    return v;
}

// ---- Uncharted's tree search: (node, key, seen) -> 0/1, the game's loop and recursion step by step. ----
std::atomic<uint64_t> s_tree_calls{0};
uint32_t TreeFind(const uint8_t* base, uint32_t node, uint32_t key, uint32_t seen, int depth) {
    int32_t count = static_cast<int32_t>(Rd32(base, node + 4));
    if (0 >= count || depth > 4096) {
        return 0;
    }
    int32_t i = 0;
    uint32_t off = 0;
    do {
        const uint32_t entries = Rd32(base, node + 8);
        const uint32_t entry = entries + off;
        const uint32_t entry_key = Rd32(base, entries + off);
        if (Rd32(base, entry + 8) & 0x200) {
            if (entry_key == key) {
                seen = 1;
            }
            if (entry_key == key || entry_key == 0x9945DA05u) {
                const uint32_t found = TreeFind(base, Rd32(base, entry + 0x14), key, seen & 0xFF, depth + 1);
                count = static_cast<int32_t>(Rd32(base, node + 4));  // (the game reads the count again)
                if (found != 0) {
                    return 1;
                }
            }
        } else {
            if (entry_key == key) {
                return 1;
            }
            if (seen != 0 && entry_key == 0x9945DA05u) {
                return 1;
            }
        }
        i++;
        off += 0x24;
    } while (i < count);
    return 0;
}
void TreeFindCall(uint64_t regs, uint64_t base, uint64_t) {
    uint32_t* const r = reinterpret_cast<uint32_t*>(regs);
    r[0] = TreeFind(reinterpret_cast<const uint8_t*>(base), r[0], r[1], r[2] & 0xFF, 0);
    s_tree_calls.fetch_add(1, std::memory_order_relaxed);
}

struct Native {
    const char* name;
    bool thumb;
    uint32_t first[2];  // its first 8 bytes (a common prologue), as words
    uint32_t size;      // the bytes the hash covers
    uint64_t hash;      // FNV-1a 64 of them
    void (*run)(uint64_t regs, uint64_t base, uint64_t);
};
constexpr Native kNatives[] = {
    {"Uncharted's tree search (PCSA00029)", true, {0x47F0E92Du, 0x68601C04u}, 130, 0x176100180023F8D4ull, TreeFindCall},
};

uint64_t Fnv(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

std::atomic<int> s_state{0};  // 0 not read, 1 off, 2 on

bool On() {
    int state = s_state.load(std::memory_order_acquire);
    if (state == 0) {
        const bool yes = emusuni_vita_switch(EMUSUNI_VITA_GAMEFN) != 0;
        int none = 0;
        if (s_state.compare_exchange_strong(none, yes ? 2 : 1)) {
            emusuni_vita_log("vita: gamefn: a game's own hot functions done natively (Uncharted's tree search): %s",
                             yes ? "on (the file vita-gamefn-on; read once)" : "off (the file vita-gamefn-on makes it on; read once)");
        }
        state = s_state.load(std::memory_order_acquire);
    }
    return state == 2;
}

}  // namespace

// A block is about to be translated at pc: the native function to call instead, or nullptr.
extern "C" void* emusuni_vita_gamefn_find(int thumb, uint32_t pc, const unsigned char* base) {
    if (!On()) {
        return nullptr;
    }
    for (size_t k = 0; k < sizeof(kNatives) / sizeof(kNatives[0]); k++) {
        const Native& n = kNatives[k];
        if (n.thumb != (thumb != 0) || Rd32(base, pc) != n.first[0] || Rd32(base, pc + 4) != n.first[1]) {
            continue;
        }
        if (Fnv(base + pc, n.size) != n.hash) {
            continue;
        }
        emusuni_vita_log("vita: gamefn: %s at 0x%08X: native", n.name, pc);
        return reinterpret_cast<void*>(n.run);
    }
    return nullptr;
}

// (19-11, 01.83) The guest bytes a native function stands for (0 for any other pointer): the recompiler's block that
// calls it covers them, so that a change of that code (dynarmic's invalidation of a range) finds the block. Before, the
// block's range was empty.
extern "C" unsigned emusuni_vita_gamefn_bytes(const void* fn) {
    for (size_t k = 0; k < sizeof(kNatives) / sizeof(kNatives[0]); k++) {
        if (reinterpret_cast<const void*>(kNatives[k].run) == fn) {
            return kNatives[k].size;
        }
    }
    return 0;
}

// (The log's count, now and then: how many calls each took.)
extern "C" unsigned long long emusuni_vita_gamefn_calls(void) {
    return s_tree_calls.load(std::memory_order_relaxed);
}
