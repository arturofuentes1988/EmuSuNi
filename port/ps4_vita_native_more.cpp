// EmuSuNi's PS Vita port (17-11, the performance specialist's; not built into the app yet: the Vita specialist wires
// it next to port/ps4_vita_native.cpp). One more call answered inside the recompiler, found by profiling the three
// bench games with the recompiler optimised (cpuopt on; note vita-0165-perfil-rendimiento.md).
//
// sceKernelDelayThread(0) and sceKernelDelayThreadCB(0).
//
// What the game sees stays exactly what Vita3K gives it: a delay of 0 microseconds is refused at once with
// SCE_KERNEL_ERROR_INVALID_ARGUMENT (0x80020005; SceThreadmgr.cpp's delay_thread and delay_thread_cb, before any
// wait, any callback or any change of the thread's state). Answered here instead, the result is the same word in r0
// and nothing else of the guest changes.
//
// What changes is the PS4's: Street Fighter X Tekken's main thread waits for the time to pass with
// "while (sceKernelGetProcessTime() < target) sceKernelDelayThread(0);" (counted with gdb on the PC bench: 315 of 600
// delay calls, all on "PCSE00005", with 0; its "Resource Manager" thread does the same with 1 microsecond). Vita3K
// answers 0 at once, so the loop spins: in its loads, menus and films up to 25 million calls of each per 10 s, with
// 1-2.5 of the PC's cores busy (the fight itself has no such loop: 0.13-0.15 cores). On the PS4 that spin keeps a core
// from the game's other threads ("Afx Rendering" above all: the console's 01.32 log saw the main thread and it take
// turns on about one core). A real Vita's kernel makes the thread give way at that call; here the PS4's thread gives
// its core away too (std::this_thread::yield), or sleeps a few microseconds (kZeroDelayHostUs), and the game still
// gets 0x80020005.
//
// Interface for the wiring (the Vita specialist's choice): emusuni_vita_native_more_kind(name) once per call name
// (0: not one of these), then emusuni_vita_native_more_call(kind, r) on each call, with r the guest's r0-r3: 1 when it
// answered (r[0] is the result), 0 when the call must go the ordinary way (any delay but 0: a real wait, left to
// Vita3K). Switch suggestion: "delayzero", on after measuring on the bench (SFxT's menus: cores busy and pictures a
// second) and on the console.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

constexpr uint32_t kInvalidArgument = 0x80020005u;  // SCE_KERNEL_ERROR_INVALID_ARGUMENT (kernel/types.h)

// The PS4's side of a delay of 0: 0 gives the core away (sched_yield) and goes on at once; more sleeps that many
// microseconds first (a spinning wait for the time then costs about 1 / (that + the loop's own time) of a core).
constexpr long kZeroDelayHostUs = 0;

enum Kind : int { kNone = 0, kDelayThread = 1, kDelayThreadCB = 2 };

void GiveWay() {
    if (kZeroDelayHostUs > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(kZeroDelayHostUs));
    } else {
        std::this_thread::yield();  // (sched_yield)
    }
}

}  // namespace

extern "C" int emusuni_vita_native_more_kind(const char* name) {
    if (name == nullptr) {
        return kNone;
    }
    if (std::strcmp(name, "sceKernelDelayThread") == 0) {
        return kDelayThread;
    }
    if (std::strcmp(name, "sceKernelDelayThreadCB") == 0) {
        return kDelayThreadCB;
    }
    return kNone;
}

extern "C" int emusuni_vita_native_more_call(int kind, unsigned* r) {
    switch (kind) {
    case kDelayThread:
    case kDelayThreadCB:
        if (r[0] != 0) {
            return 0;  // (a real delay: Vita3K's own wait, callbacks and all)
        }
        r[0] = kInvalidArgument;
        GiveWay();
        return 1;
    default:
        return 0;
    }
}
