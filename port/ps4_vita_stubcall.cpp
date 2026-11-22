// EmuSuNi's PS Vita port (19-11, for 01.83; the call-bridge specialist's): an import stub made by the recompiler
// into one direct call of its function, found when the stub's block is translated.
//
// A game calls a function of the system through a stub of three words that Vita3K's loader writes:
// "svc #0; mov pc, lr; <the function's number>". Until now each such call cost, besides the function itself: the
// recompiled code stopping at the supervisor call, the recompiler's CallSVC, the port's hook reading the number
// from the game's memory and looking it up in four tables (port/ps4_vita_native.cpp, _fastids.cpp, _lwfast.cpp,
// _gxmfast.cpp), two or three reads of the time stamp counter, Vita3K's call_import (a std::function, its own
// bookkeeping, the bridge), the global exclusive monitor's spinlock (clear_exclusive) and a second block for
// "mov pc, lr". The games' busiest threads make 1.6 to 9 million such calls every 10 s.
//
// With the switch "stubcall" (port/ps4_vita_sys.h; OFF until the console has tried it), the recompiler, when it
// translates a block that starts at such a stub, asks here what the function is - once, for every thread's
// recompiler - and makes the stub one block: for a call the port's hook answers inside the recompiler anyway (the
// switches nativelib, fastcalls, fastgxm, gxmfast, lwfast, fastids), a supervisor call that carries the slot of
// the function already found (bit 31 set: a game's stub always says 0), then the return to the caller; for
// sceKernelGetThreadId and sceKernelGetTLSAddr (with "fastids") a call of a few instructions here with the
// thread's own numbers, written into the block when it is made (each guest thread has a recompiler of its own).
// Every other call (one that waits, runs a callback of the game's, ends the thread...) gets a slot too, that sends
// it straight to the guest thread's loop as before - without the hook's four look-ups first - where thread.cpp
// takes the number from the slot; the program counter is then the caller's return address already (no function
// of Vita3K's changes LR, the only difference that would make). The kernel's side (scripts/build-vita-core.sh,
// kernel.cpp): the slot's function run, with no clear_exclusive (these are functions of libraries on a PS Vita, no
// system call clears an exclusive access there) and the lightweight mutex's thread kept by the host thread; a
// lightweight mutex's lock that has to wait goes to the loop the same way.
//
// The switch "callstats" (OFF): the time of the calls made that way measured on one call in 16 (and counted
// 16 times) - each read of the time stamp counter costs about as much as a short call.
// The switch "stubgxm" (OFF; with "stubcall" only): the GPU library's texture and context setters below (kMoreGxm)
// answered inside the recompiler too.
//
// The log: "vita: stubcall:" lines when the 1st, 64th, 512th... stub block is made, and every 2^22 calls that went
// to the thread's loop the numbers that went most (what is still called the ordinary way).
// The PC: EMUSUNI_VITA_STUBCALL=1, EMUSUNI_VITA_CALLSTATS=1; tests: EMUSUNI_VITA_TEST_STUB_CLREX=1 puts
// clear_exclusive back, EMUSUNI_VITA_TEST_STUB_LEFT=0 leaves the other stubs as they were (=1: the line every 2^18).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ps4_vita_sys.h"

extern "C" int emusuni_vita_gxm_kind(unsigned nid);
extern "C" int emusuni_vita_ids_kind(unsigned nid);
extern "C" int emusuni_vita_lw_kind(unsigned nid);
extern "C" int emusuni_vita_svc_kind(unsigned nid);
extern "C" const void* emusuni_vita_import_fn(unsigned nid);  // (module_parent.cpp: Vita3K's own function of a number)
extern "C" void emusuni_vita_stat_inline(unsigned nid, unsigned long long ticks);

namespace {

// What a slot does (the kernel's emusuni_stub_run in the script reads the same numbers).
enum : int16_t {
    kGxmGet = 1,   // a GPU library getter answered on Vita3K's structures (sub: which), else the export
    kExport = 2,   // the export called directly (a GPU setter, a fast call)
    kIds = 3,      // sceKernelGetTLSAddr (sub 1) / sceKernelGetThreadId (sub 2), when the thread's numbers aren't known
    kLw = 4,       // a lightweight mutex's lock (sub 1), try (2), unlock (3); a lock that would wait: the thread's loop
    kNative = 5,   // one of the library's routines done by the PS4 (sub: port/ps4_vita_native.cpp's kind)
    kLeft = 6,     // any other call: straight to the guest thread's loop, without the hook's four look-ups; counted
};

struct Stub {
    uint32_t nid;
    int16_t way;
    int16_t sub;
    const void* fn;
};
constexpr int kStubs = 4096;
Stub s_stubs[kStubs];
int s_count = 0;  // (under s_lock; the slots below it never change until the next game)
std::mutex s_lock;
bool s_on = false;
bool s_sampled = false;
std::atomic<uint32_t> s_blocks{0}, s_ids{0}, s_left{0};
std::atomic<uint32_t> s_bad_keys{0};
std::atomic<uint32_t> s_back[kStubs];  // a slot's calls that went to the guest thread's loop
std::atomic<uint32_t> s_back_all{0};
bool s_left_slots = true;      // (the PC: EMUSUNI_VITA_TEST_STUB_LEFT=0 leaves the other stubs as they were)
uint32_t s_back_said = 0x3FFFFF;  // (a line every 2^22 calls through the loop; the PC's test, every 2^18)

// (The switch "stubgxm", with "stubcall") More of the GPU library's functions answered inside the recompiler, of the
// kind port/ps4_vita_native.cpp's fastgxm list has: each was read in Vita3K's SceGxm.cpp - it reads or writes a
// texture's, a precomputed state's or the context's words, at most hands the renderer an order for later, and never
// waits, runs code of the game's or ends the thread. (Still left out: the draws, the reserving of a default uniform
// buffer, what makes a program, sceGxmColorSurfaceSetClip - not done in Vita3K, it logs.) Street Fighter X Tekken's
// fight still sent ~100 thousand calls of three texture setters to the thread's loop every 10 s on the bench (01.82's
// core): sceGxmTextureSetUAddrModeSafe, ...VAddrModeSafe, ...SetLodBias. An underscore before a name (Vita3K's second
// export of the same function) counts as the name.
constexpr const char* kMoreGxm[] = {
    "sceGxmTextureSetData", "sceGxmTextureSetFormat", "sceGxmTextureSetGammaMode", "sceGxmTextureSetHeight",
    "sceGxmTextureSetLodBias", "sceGxmTextureSetLodMin", "sceGxmTextureSetMagFilter", "sceGxmTextureSetMinFilter",
    "sceGxmTextureSetMipFilter", "sceGxmTextureSetMipmapCount", "sceGxmTextureSetNormalizeMode", "sceGxmTextureSetPalette",
    "sceGxmTextureSetStride", "sceGxmTextureSetUAddrMode", "sceGxmTextureSetUAddrModeSafe", "sceGxmTextureSetVAddrMode",
    "sceGxmTextureSetVAddrModeSafe", "sceGxmTextureSetWidth",
    "sceGxmTextureInitCube", "sceGxmTextureInitCubeArbitrary", "sceGxmTextureInitLinear", "sceGxmTextureInitLinearStrided",
    "sceGxmTextureInitSwizzled", "sceGxmTextureInitSwizzledArbitrary", "sceGxmTextureInitTiled",
    "sceGxmSetViewport", "sceGxmSetViewportEnable", "sceGxmSetRegionClip", "sceGxmSetDefaultRegionClipAndViewport",
    "sceGxmSetFrontPointLineWidth", "sceGxmSetBackPointLineWidth", "sceGxmSetFrontLineFillLastPixelEnable",
    "sceGxmSetBackLineFillLastPixelEnable", "sceGxmSetFrontFragmentProgramEnable", "sceGxmSetBackFragmentProgramEnable",
    "sceGxmSetFrontVisibilityTestEnable", "sceGxmSetBackVisibilityTestEnable", "sceGxmSetFrontVisibilityTestIndex",
    "sceGxmSetBackVisibilityTestIndex", "sceGxmSetFrontVisibilityTestOp", "sceGxmSetBackVisibilityTestOp",
    "sceGxmSetWBufferEnable", "sceGxmSetWClampEnable", "sceGxmSetWClampValue", "sceGxmSetVertexTexture",
    "sceGxmSetFragmentUniformBuffer", "sceGxmSetVertexUniformBuffer", "sceGxmSetFragmentDefaultUniformBuffer",
    "sceGxmSetVertexDefaultUniformBuffer", "sceGxmSetPrecomputedFragmentState", "sceGxmSetPrecomputedVertexState",
    "sceGxmSetCullMode", "sceGxmSetTwoSidedEnable", "sceGxmSetFragmentTexture", "sceGxmSetVertexStream",
    "sceGxmSetFrontDepthFunc", "sceGxmSetBackDepthFunc", "sceGxmSetFrontDepthWriteEnable", "sceGxmSetBackDepthWriteEnable",
    "sceGxmSetFrontDepthBias", "sceGxmSetBackDepthBias", "sceGxmSetFrontPolygonMode", "sceGxmSetBackPolygonMode",
    "sceGxmSetFrontStencilFunc", "sceGxmSetBackStencilFunc", "sceGxmSetFrontStencilRef", "sceGxmSetBackStencilRef",
    // (a precomputed state's words: Uncharted ~37 thousand of the two SetUniformBuffer every 10 s on the bench)
    "sceGxmPrecomputedVertexStateSetTexture", "sceGxmPrecomputedVertexStateSetAllTextures",
    "sceGxmPrecomputedVertexStateSetUniformBuffer", "sceGxmPrecomputedVertexStateSetAllUniformBuffers",
    "sceGxmPrecomputedVertexStateSetDefaultUniformBuffer", "sceGxmPrecomputedFragmentStateSetTexture",
    "sceGxmPrecomputedFragmentStateSetAllTextures", "sceGxmPrecomputedFragmentStateSetUniformBuffer",
    "sceGxmPrecomputedFragmentStateSetAllUniformBuffers", "sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer",
    // (a texture's words read: MotorStorm RC's sceGxmTextureGetLodBias, ~85 thousand every 10 s on the bench)
    "sceGxmTextureGetData", "sceGxmTextureGetFormat", "sceGxmTextureGetGammaMode", "sceGxmTextureGetHeight",
    "sceGxmTextureGetLodBias", "sceGxmTextureGetLodMin", "sceGxmTextureGetMagFilter", "sceGxmTextureGetMinFilter",
    "sceGxmTextureGetMipFilter", "sceGxmTextureGetMipmapCount", "sceGxmTextureGetMipmapCountUnsafe",
    "sceGxmTextureGetNormalizeMode", "sceGxmTextureGetPalette", "sceGxmTextureGetStride", "sceGxmTextureGetType",
    "sceGxmTextureGetUAddrMode", "sceGxmTextureGetUAddrModeSafe", "sceGxmTextureGetVAddrMode",
    "sceGxmTextureGetVAddrModeSafe", "sceGxmTextureGetWidth",
};
const char* (*s_name)(unsigned) = nullptr;
bool s_more = false;

bool More(uint32_t nid) {
    const char* name = s_name != nullptr ? s_name(nid) : nullptr;
    if (name == nullptr || name[0] == 0) {
        return false;
    }
    if (name[0] == '_') {
        name++;
    }
    for (const char* const more : kMoreGxm) {
        if (std::strcmp(more, name) == 0) {
            return true;
        }
    }
    return false;
}

int Decide(uint32_t nid, int16_t* sub, const void** fn) {
    *sub = 0;
    *fn = emusuni_vita_import_fn(nid);
    // (The order of the hook's look-ups, kernel.cpp's emusuni_inline_svc.)
    if (const int gxm = emusuni_vita_gxm_kind(nid)) {
        if (gxm >= 100) {
            return *fn != nullptr ? kExport : 0;
        }
        *sub = static_cast<int16_t>(gxm);
        return kGxmGet;
    }
    if (const int id = emusuni_vita_ids_kind(nid)) {
        *sub = static_cast<int16_t>(id);
        return *fn != nullptr ? kIds : 0;
    }
    if (const int lw = emusuni_vita_lw_kind(nid)) {
        *sub = static_cast<int16_t>(lw);
        return kLw;
    }
    const int kind = emusuni_vita_svc_kind(nid);
    if (kind == 1) {
        return *fn != nullptr ? kExport : 0;
    }
    if (kind >= 2) {
        *sub = static_cast<int16_t>(kind);
        return kNative;
    }
    if (s_more && More(nid)) {
        return *fn != nullptr ? kExport : 0;
    }
    return 0;
}

}  // namespace

// A game starts (the kernel is made): the switches as they are now, the slots empty.
extern "C" void emusuni_vita_stub_start(const char* (*name_of)(unsigned)) {
    const std::lock_guard<std::mutex> lock(s_lock);
    s_count = 0;
    s_name = name_of;
    s_on = emusuni_vita_switch(EMUSUNI_VITA_STUBCALL) != 0;
    s_sampled = emusuni_vita_switch(EMUSUNI_VITA_CALLSTATS) != 0;
    s_more = emusuni_vita_switch(EMUSUNI_VITA_STUBGXM) != 0;
    s_blocks.store(0, std::memory_order_relaxed);
    s_ids.store(0, std::memory_order_relaxed);
    s_left.store(0, std::memory_order_relaxed);
    s_back_all.store(0, std::memory_order_relaxed);
    for (std::atomic<uint32_t>& back : s_back) {
        back.store(0, std::memory_order_relaxed);
    }
#if !defined(__ORBIS__)
    const char* const left = getenv("EMUSUNI_VITA_TEST_STUB_LEFT");
    s_left_slots = left == nullptr || left[0] != '0';
    s_back_said = left != nullptr && left[0] == '1' ? 0x3FFFF : 0x3FFFFF;
#endif
    emusuni_vita_log("vita: stubcall: import stubs made into direct calls when translated: %s; their time measured on %s; "
                     "%u more of the GPU library's simplest functions answered inside: %s",
                     s_on ? "on (the file vita-stubcall-on)" : "off (the file vita-stubcall-on makes it on)",
                     s_sampled ? "one call in 16 (the file vita-callstats-on)" : "every call",
                     static_cast<unsigned>(sizeof(kMoreGxm) / sizeof(kMoreGxm[0])),
                     s_more ? (s_on ? "yes (the file vita-stubgxm-on)" : "no (stubgxm is on but needs stubcall)")
                            : "no (the file vita-stubgxm-on makes it so, with stubcall)");
}

extern "C" int emusuni_vita_stub_on(void) {
    return s_on ? 1 : 0;
}
extern "C" int emusuni_vita_stub_sampled(void) {
    return s_sampled ? 1 : 0;
}
extern "C" int emusuni_vita_stub_clrex(void) {
#if defined(__ORBIS__)
    return 0;
#else
    static const bool test = getenv("EMUSUNI_VITA_TEST_STUB_CLREX") != nullptr;
    return test ? 1 : 0;
#endif
}

// The recompiler translates a block that starts at an import stub (the kernel asks this for it): the slot that
// answers the function, or -1 (the ordinary way). way/sub/fn: what the slot does.
extern "C" int emusuni_vita_stub_slot(unsigned nid, int* way, int* sub, const void** fn) {
    const std::lock_guard<std::mutex> lock(s_lock);
    for (int i = 0; i < s_count; i++) {
        if (s_stubs[i].nid == nid) {
            *way = s_stubs[i].way;
            *sub = s_stubs[i].sub;
            *fn = s_stubs[i].fn;
            return i;
        }
    }
    int16_t what = 0;
    const void* found = nullptr;
    int how = Decide(nid, &what, &found);
    if (how == 0) {
        s_left.fetch_add(1, std::memory_order_relaxed);
        if (!s_left_slots) {
            return -1;
        }
        how = kLeft;
    }
    if (s_count >= kStubs) {
        return -1;
    }
    Stub& stub = s_stubs[s_count];
    stub.nid = nid;
    stub.way = static_cast<int16_t>(how);
    stub.sub = what;
    stub.fn = found;
    *way = how;
    *sub = what;
    *fn = found;
    return s_count++;
}

// The slots, for the kernel's run of one (read without the lock: a slot is written before its number is given out).
extern "C" const void* emusuni_vita_stub_table(void) {
    return s_stubs;
}

// (thread.cpp) The function's number of a slot whose call went to the guest thread's loop: a call not answered
// inside (one that waits, runs a callback, ends the thread...) or a lightweight mutex's lock that has to wait.
// Counted; every 2^22 of them (the PC's test: 2^18) the log says which numbers went most (way: 4 a lightweight
// mutex's lock, 6 any other).
extern "C" unsigned emusuni_vita_stub_nid(unsigned slot) {
    if (slot >= static_cast<unsigned>(kStubs)) {
        return 0;
    }
    s_back[slot].fetch_add(1, std::memory_order_relaxed);
    if (((s_back_all.fetch_add(1, std::memory_order_relaxed) + 1) & s_back_said) == 0) {
        int count = 0;
        {
            const std::lock_guard<std::mutex> lock(s_lock);
            count = s_count;
        }
        char text[400];
        size_t used = 0;
        bool said[kStubs] = {};
        for (int n = 0; n < 12; n++) {
            int best = -1;
            uint32_t most = 0;
            for (int i = 0; i < count; i++) {
                const uint32_t calls = s_back[i].load(std::memory_order_relaxed);
                if (!said[i] && calls > most) {
                    most = calls;
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            said[best] = true;
            const int wrote = snprintf(text + used, sizeof(text) - used, " %08X(%d) %u", s_stubs[best].nid,
                                       s_stubs[best].way, static_cast<unsigned>(most));
            if (wrote <= 0 || static_cast<size_t>(wrote) >= sizeof(text) - used) {
                break;
            }
            used += static_cast<size_t>(wrote);
        }
        text[used] = 0;
        emusuni_vita_log("vita: stubcall: %u calls of stubs' blocks went to the thread's loop; most:%s",
                         static_cast<unsigned>(s_back_all.load(std::memory_order_relaxed)), text);
    }
    return s_stubs[slot].nid;
}

// A block made: 0 a slot's call, 1 the thread's own numbers. Said in the log at the 1st, 64th, 512th, 4096th and
// 32768th (the blocks of every thread's recompiler: each has its own).
extern "C" void emusuni_vita_stub_made(int ids) {
    (ids ? s_ids : s_blocks).fetch_add(1, std::memory_order_relaxed);
    const uint32_t made = s_blocks.load(std::memory_order_relaxed) + s_ids.load(std::memory_order_relaxed);
    if (made == 1 || made == 64 || made == 512 || made == 4096 || made == 32768) {
        int slots = 0;
        {
            const std::lock_guard<std::mutex> lock(s_lock);
            slots = s_count;
        }
        emusuni_vita_log("vita: stubcall: %u stub blocks made (%u of them with the thread's own id or TLS block), %d "
                         "functions; %u stubs whose calls go to the thread's loop as before",
                         static_cast<unsigned>(made), static_cast<unsigned>(s_ids.load(std::memory_order_relaxed)), slots,
                         static_cast<unsigned>(s_left.load(std::memory_order_relaxed)));
    }
}

// sceKernelGetThreadId / sceKernelGetTLSAddr with the thread's numbers written into the block (the recompiler calls
// this from the block with the guest's registers): kind 2 the id (value), kind 1 the TLS block (value) + 4 x key,
// as KernelState::get_thread_tls_addr gives it - 0 for a key out of 0..0x100 (whose error line is said once here).
extern "C" void emusuni_vita_stub_ids(uint64_t regs, uint64_t value, uint64_t nid_kind) {
    uint32_t* const r = reinterpret_cast<uint32_t*>(regs);
    const uint32_t nid = static_cast<uint32_t>(nid_kind);
    if ((nid_kind >> 32) == 2) {
        r[0] = static_cast<uint32_t>(value);
    } else if (r[0] <= 0x100u) {
        r[0] = static_cast<uint32_t>(value) + 4u * r[0];
    } else {
        if (s_bad_keys.fetch_add(1, std::memory_order_relaxed) == 0) {
            emusuni_vita_log("vita: stubcall: sceKernelGetTLSAddr asked for the slot %d: 0 given (Wrong tls slot index)",
                             static_cast<int>(r[0]));
        }
        r[0] = 0;
    }
    emusuni_vita_stat_inline(nid, 0);
}
