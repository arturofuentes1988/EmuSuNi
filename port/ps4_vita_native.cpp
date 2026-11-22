// EmuSuNi's PS Vita port (17-11, after 01.63): the firmware library's long routines done by the PS4 itself, and
// the system's simplest calls answered without leaving the recompiler.
//
// A PS Vita game's C library is a module of the firmware (libc.suprx, in the game's own folder) that Vita3K runs as
// the game's code is run: recompiled. So are its copies: every memcpy, memmove, memset and strlen of the game goes
// through the recompiler, a byte or a vector at a time as the ARM code has it. Here those four are done by the
// PS4's own - the routines whose result the C standard fixes to the last bit, so a game can't tell (a comparison's
// result is only fixed in its sign, and the maths' not at all: those stay the firmware's).
//
// How: a game reaches a library routine through an import stub of three words, which Vita3K's loader fills with a
// jump to the routine when the module that has it is loaded, or with a supervisor call when the routine is one of
// Vita3K's own. For the routines here the loader keeps the supervisor call (scripts/build-vita-core.sh), and the
// call is answered inside the recompiler's own call for it: no stop of the recompiled code, no turn of the guest
// thread's loop - a stop and a start cost 1.1 microseconds on the PS4 (01.35's and 01.63's console logs: millions
// of calls at that), more than a short copy itself.
//
// The same way in serves the calls of the system's that are Vita3K's own already and neither wait nor run code of
// the game's: answered there too, the "fast calls". Which ones: the list below, by name.
//
// Switches (port/ps4_vita_sys.h): "nativelib" and "fastcalls". A routine or a call left out by name: the file
// /data/EmuSuNi/vita-native-off.txt (the PC: EMUSUNI_VITA_NATIVE_OFF), names apart by spaces, commas or lines; more
// fast calls for a test: /data/EmuSuNi/vita-fastcalls.txt (the PC: EMUSUNI_VITA_FASTCALLS_ADD).
//
// (18-11, after 01.76) And a third switch, "fastgxm": the graphics library's setters - a state of the next
// draw, a number for a shader, a texture's filter - answered the same way. A game makes them by the hundred
// thousand: God of War's "Render" thread, 3.1 million calls every 10 s on the console (01.75's log) and 5.9 of
// those 10 s inside them, one in three "sceGxmSetUniformDataF". Each of the list below was read: it writes the
// game's memory or the context's state, at most hands the renderer an order for later, and never waits, runs
// code of the game's or ends the thread. (Left out for that: the two that reserve the default uniform
// buffer, which on a deferred context may call the game back for more memory; the draws; what makes a program.)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ps4_vita_sys.h"

namespace {

constexpr uint64_t kSpace = 1ull << 32;  // the guest's addresses

inline bool Inside(uint32_t at, uint32_t size) {
    return static_cast<uint64_t>(at) + size <= kSpace;
}

// ---- The routines. r: the guest's r0-r3 (arguments in, r0 the result out); base: where its memory starts. ----
// (memcpy as memmove: where source and destination overlap the C standard says nothing, and one that goes wrong
// either way is the lesser risk.)
void Copy(uint32_t* r, uint8_t* base) {
    if (r[2] != 0 && Inside(r[0], r[2]) && Inside(r[1], r[2])) {
        std::memmove(base + r[0], base + r[1], r[2]);
    }
}
void Fill(uint32_t* r, uint8_t* base) {
    if (r[2] != 0 && Inside(r[0], r[2])) {
        std::memset(base + r[0], static_cast<int>(r[1] & 0xFF), r[2]);
    }
}
void Length(uint32_t* r, uint8_t* base) {
    r[0] = static_cast<uint32_t>(strnlen(reinterpret_cast<const char*>(base) + r[0], static_cast<size_t>(kSpace - r[0])));
}

struct Routine {
    const char* name;
    void (*run)(uint32_t* r, uint8_t* base);
    bool of_a_module;  // an export of a recompiled module: the loader keeps its importers' stubs as calls
};
constexpr Routine kRoutines[] = {
    {"memcpy", Copy, true},         {"memmove", Copy, true},        {"memset", Fill, true},
    {"strlen", Length, true},
    // (SceLibKernel's own copies are Vita3K's already - a full call each: here they cost a fifth of it.)
    {"sceClibMemcpy", Copy, false}, {"sceClibMemcpy_safe", Copy, false}, {"sceClibMemmove", Copy, false},
    {"sceClibMemset", Fill, false},
};
constexpr int kRoutineCount = static_cast<int>(sizeof(kRoutines) / sizeof(kRoutines[0]));

// ---- The fast calls: Vita3K's own, done in a few instructions, that neither wait for anything, nor run code of
// the game's (a callback), nor end or switch the thread. ----
constexpr const char* kFast[] = {
    "sceKernelGetTLSAddr",        "sceKernelGetThreadId",       "sceKernelGetThreadCurrentPriority",
    "sceKernelGetProcessTime",    "sceKernelGetProcessTimeLow", "sceKernelGetProcessTimeWide",
    "sceKernelGetSystemTimeWide", "sceRtcGetCurrentTick",
    "sceClibStrnlen",             "sceClibStrcmp",              "sceClibStrncmp",
    "sceClibMemcmp",              "sceClibMemchr",              "sceClibStrchr",
};

// ---- The graphics library's setters (the "fastgxm" switch). ----
constexpr const char* kFastGxm[] = {
    "sceGxmSetUniformDataF",
    "sceGxmSetVertexStream",          "sceGxmSetVertexProgram",         "sceGxmSetFragmentProgram",
    "sceGxmSetFragmentTexture",       "sceGxmSetCullMode",              "sceGxmSetTwoSidedEnable",
    "sceGxmSetFrontDepthWriteEnable", "sceGxmSetBackDepthWriteEnable",  "sceGxmSetFrontDepthFunc",
    "sceGxmSetBackDepthFunc",         "sceGxmSetFrontDepthBias",        "sceGxmSetBackDepthBias",
    "sceGxmSetFrontPolygonMode",      "sceGxmSetBackPolygonMode",       "sceGxmSetFrontStencilFunc",
    "sceGxmSetBackStencilFunc",       "sceGxmSetFrontStencilRef",       "sceGxmSetBackStencilRef",
    "sceGxmTextureSetUAddrMode",      "sceGxmTextureSetVAddrMode",      "sceGxmTextureSetMagFilter",
    "sceGxmTextureSetMinFilter",      "sceGxmTextureSetMipFilter",      "sceGxmShaderPatcherGetProgramFromId",
    "sceGxmPrecomputedVertexStateGetDefaultUniformBuffer",
    "sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer",
};

// ---- What each call's number is, found out once (the core names a number): a table without a lock. ----
enum : uint8_t { kNotKnown = 0, kPlain = 1, kFastCall = 2, kRoutineBase = 3 };
struct Slot {
    std::atomic<uint32_t> nid{0};
    std::atomic<uint8_t> kind{kNotKnown};
};
constexpr uint32_t kSlots = 8192;
Slot s_slots[kSlots];

const char* (*s_name)(unsigned) = nullptr;
bool s_native = false, s_fast = false, s_gxm = false;
std::string s_off, s_more;  // " name name " lists: left out, and added to the fast calls

bool Listed(const std::string& list, const char* name) {
    return !list.empty() && list.find(std::string(" ") + name + " ") != std::string::npos;
}

std::string ReadNames(const char* file, const char* variable) {
    std::string text;
#if defined(__ORBIS__)
    (void)variable;
    if (FILE* const f = fopen(file, "rb")) {
        char buffer[2048];
        const size_t got = fread(buffer, 1, sizeof(buffer) - 1, f);
        fclose(f);
        text.assign(buffer, got);
    }
#else
    (void)file;
    if (const char* const value = getenv(variable)) {
        text = value;
    }
#endif
    std::string list = " ";
    std::string word;
    for (const char c : text + " ") {
        const bool part = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (part) {
            word.push_back(c);
        } else if (!word.empty()) {
            list += word + " ";
            word.clear();
        }
    }
    return list.size() > 1 ? list : std::string();
}

uint8_t Decide(uint32_t nid) {
    const char* const name = s_name != nullptr ? s_name(nid) : nullptr;
    if (name == nullptr || name[0] == 0 || Listed(s_off, name)) {
        return kPlain;
    }
    if (s_native) {
        for (int i = 0; i < kRoutineCount; i++) {
            if (std::strcmp(kRoutines[i].name, name) == 0) {
                return static_cast<uint8_t>(kRoutineBase + i);
            }
        }
    }
    if (s_fast) {
        for (const char* const fast : kFast) {
            if (std::strcmp(fast, name) == 0) {
                return kFastCall;
            }
        }
        if (Listed(s_more, name)) {
            return kFastCall;
        }
    }
    if (s_gxm) {
        for (const char* const fast : kFastGxm) {
            if (std::strcmp(fast, name) == 0) {
                return kFastCall;
            }
        }
    }
    return kPlain;
}

uint8_t Kind(uint32_t nid) {
    uint32_t at = (nid * 2654435761u) >> 19;  // (13 bits)
    for (uint32_t tries = 0; tries < 32; tries++, at = (at + 1) & (kSlots - 1)) {
        Slot& slot = s_slots[at];
        uint32_t there = slot.nid.load(std::memory_order_acquire);
        if (there == 0) {
            // (Two threads at once may both decide: the same answer; one of them takes the slot.)
            const uint8_t kind = Decide(nid);
            uint32_t none = 0;
            if (slot.nid.compare_exchange_strong(none, nid, std::memory_order_acq_rel)) {
                slot.kind.store(kind, std::memory_order_release);
                return kind;
            }
            there = none;
        }
        if (there == nid) {
            const uint8_t kind = slot.kind.load(std::memory_order_acquire);
            return kind != kNotKnown ? kind : Decide(nid);
        }
    }
    return Decide(nid);  // (a table that full: decided each time)
}

}  // namespace

// A game starts (the kernel is made, before any module is loaded): the switches and the lists as they are now.
extern "C" void emusuni_vita_native_start(const char* (*name_of)(unsigned)) {
    for (Slot& slot : s_slots) {
        slot.kind.store(kNotKnown, std::memory_order_relaxed);
        slot.nid.store(0, std::memory_order_release);
    }
    s_name = name_of;
    s_native = emusuni_vita_switch(EMUSUNI_VITA_NATIVELIB) != 0;
    s_fast = emusuni_vita_switch(EMUSUNI_VITA_FASTCALLS) != 0;
    s_gxm = emusuni_vita_switch(EMUSUNI_VITA_FASTGXM) != 0;
    s_off = ReadNames("/data/EmuSuNi/vita-native-off.txt", "EMUSUNI_VITA_NATIVE_OFF");
    s_more = ReadNames("/data/EmuSuNi/vita-fastcalls.txt", "EMUSUNI_VITA_FASTCALLS_ADD");
    char routines[200] = "";
    size_t used = 0;
    for (int i = 0; i < kRoutineCount && s_native; i++) {
        if (Listed(s_off, kRoutines[i].name)) {
            continue;
        }
        const int n = snprintf(routines + used, sizeof(routines) - used, "%s%s", used ? " " : "", kRoutines[i].name);
        if (n > 0 && static_cast<size_t>(n) < sizeof(routines) - used) {
            used += static_cast<size_t>(n);
        }
    }
    emusuni_vita_log("vita: native: the library's routines done by the PS4 itself: %s; calls answered inside the "
                     "recompiler: %s (%u names%s%.120s); the graphics library's setters too: %s (%u names); left out by "
                     "name:%.120s",
                     used ? routines : "none (the nativelib switch is off)", s_fast ? "yes" : "no (the fastcalls switch is off)",
                     static_cast<unsigned>(sizeof(kFast) / sizeof(kFast[0])), s_more.empty() ? "" : ", and", s_more.c_str(),
                     s_gxm ? "yes" : "no (the fastgxm switch is off)", static_cast<unsigned>(sizeof(kFastGxm) / sizeof(kFastGxm[0])),
                     s_off.empty() ? " none" : s_off.c_str());
}

// For the loader: 1 when this export of a recompiled module is one of the routines here, so that the stubs of
// the modules that import it stay supervisor calls.
extern "C" int emusuni_vita_native_stub(unsigned nid) {
    const uint8_t kind = Kind(nid);
    return kind >= kRoutineBase && kRoutines[kind - kRoutineBase].of_a_module ? 1 : 0;
}

// A supervisor call of recompiled code, by its number: 0 an ordinary one (the guest thread's loop takes it),
// 1 a fast call (the caller answers it where it is), 2 and up one of the routines (emusuni_vita_native_run).
extern "C" int emusuni_vita_svc_kind(unsigned nid) {
    const uint8_t kind = Kind(nid);
    return kind <= kPlain ? 0 : kind - kFastCall + 1;
}

extern "C" void emusuni_vita_native_run(int kind, unsigned* r, unsigned char* base) {
    const int which = kind - 2;
    if (which >= 0 && which < kRoutineCount) {
        kRoutines[which].run(r, base);
    }
}
