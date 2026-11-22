// EmuSuNi's PS Vita port (19-11, the performance specialist's; for the Vita specialist to take in): two switches of the
// recompiler's code for the PS4's CPU (scripts/build-vita-core.sh: dynarmic's a32_emit_x64.cpp and .h).
//
// "condflags": a block that ends in a conditional branch (Thumb's B<cond>: dynarmic's If terminal) whose flags come from
// an ADD/SUB/CMP/CMN of the same block (or a value's test), stored whole, with nothing after it that changes the CPU's
// flags (only reads and writes of guest registers), branches on the CPU's flags as they are. Before, it stored them,
// read them back at once (a load, then sahf or cmp) and branched on that. On the PC, EMUSUNI_VITA_CONDFLAGS=check also
// reads them back and stops (ud2, SIGILL) where the two disagree.
//
// "rsbsite": a return (dynarmic's PopRSBHint terminal: BX LR, POP {..., PC}) and an indirect branch (FastDispatchHint)
// look their target up with their own copy of the shared handler's code: each site has its own indirect jump, which the
// CPU predicts on its own; through the shared handler every return of the game goes through one indirect jump. About 60
// bytes of code a site; a miss goes on in the shared handler as before.
//
// Switches: their own, off by default: the files /data/EmuSuNi/vita-condflags-on and /data/EmuSuNi/vita-rsbsite-on (the
// PC: EMUSUNI_VITA_CONDFLAGS=1 or =check, EMUSUNI_VITA_RSBSITE=1), each read once, when the first block is made. The log
// says each one's state, and now and then how many blocks used it.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps4_vita_sys.h"

namespace {

// 0 not read, 1 off, 2 on, 3 on with the PC's check
std::atomic<int> s_condflags{0};
std::atomic<int> s_rsbsite{0};
std::atomic<unsigned long long> s_if_ends{0}, s_if_fused{0};
std::atomic<unsigned long long> s_sites[2];

int Read(std::atomic<int>& state, const char* name, const char* what) {
    int now = state.load(std::memory_order_acquire);
    if (now != 0) {
        return now;
    }
    char text[32] = "";
    bool on = false;
#if defined(__ORBIS__)
    char path[96];
    snprintf(path, sizeof(path), "/data/EmuSuNi/vita-%s-on", name);
    if (FILE* const f = fopen(path, "r")) {
        on = true;
        fclose(f);
    }
#else
    char variable[64];
    snprintf(variable, sizeof(variable), "EMUSUNI_VITA_%s", name);
    for (char* p = variable; *p != '\0'; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = static_cast<char>(*p - 'a' + 'A');
        }
    }
    if (const char* const value = getenv(variable); value != nullptr && value[0] != '\0' && value[0] != '0') {
        on = true;
        snprintf(text, sizeof(text), "%s", value);
    }
#endif
    const int value = !on ? 1 : strcmp(text, "check") == 0 ? 3 : 2;
    int none = 0;
    if (state.compare_exchange_strong(none, value)) {
        emusuni_vita_log("vita: %s: %s: %s", name, what,
                         value == 1 ? "off (its file vita-...-on makes it on; read once)"
                         : value == 3 ? "on, with the PC's check (a stop where the CPU's flags and the stored ones disagree)"
                                      : "on (its file vita-...-on; read once)");
    }
    return state.load(std::memory_order_acquire);
}

}  // namespace

// 0 off, 1 on, 2 on with the PC's check (only the PC's build emits it).
extern "C" int emusuni_vita_condflags(void) {
    const int state = Read(s_condflags, "condflags", "a block's conditional branch on the CPU's flags as its ADD/SUB/CMP left them");
    return state <= 1 ? 0 : state == 3 ? 2 : 1;
}

extern "C" int emusuni_vita_rsbsite(void) {
    return Read(s_rsbsite, "rsbsite", "each return and indirect branch with its own indirect jump") >= 2 ? 1 : 0;
}

// A block with an If terminal made, with the switch on: on the CPU's flags (1) or reading them back (0).
extern "C" void emusuni_vita_condflags_seen(int fused) {
    const unsigned long long ends = s_if_ends.fetch_add(1, std::memory_order_relaxed) + 1;
    const unsigned long long on_cpu = fused ? s_if_fused.fetch_add(1, std::memory_order_relaxed) + 1
                                            : s_if_fused.load(std::memory_order_relaxed);
    if ((ends & (ends - 1)) == 0 && ends >= 256) {
        emusuni_vita_log("vita: condflags: %llu of %llu blocks that end in a conditional branch branch on the CPU's flags (%.0f%%)",
                         on_cpu, ends, 100.0 * static_cast<double>(on_cpu) / static_cast<double>(ends));
    }
}

// A site made with the switch on: 0 a return, 1 an indirect branch.
extern "C" void emusuni_vita_rsbsite_seen(int kind) {
    const unsigned long long n = s_sites[kind != 0 ? 1 : 0].fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0 && n >= 256) {
        emusuni_vita_log("vita: rsbsite: %llu %s with their own indirect jump", n, kind != 0 ? "indirect branches" : "returns");
    }
}
