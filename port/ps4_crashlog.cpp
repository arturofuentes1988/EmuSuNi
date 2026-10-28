// Crash diagnostics for PS4 test builds.
//
// PS4 coredumps are encrypted and klog is shared with other tools, so the app reports its own
// crashes: a constructor that runs before all others (priority 101) starts
// /data/EmuSuNi/boot-trace.log and installs a handler for fatal signals that appends the
// signal, fault address, registers and the code addresses found on the stack to
// /data/EmuSuNi/crash.log. Addresses are printed relative to the start of .text, i.e. as ELF
// virtual addresses: symbolize with `llvm-symbolizer --obj=<elf> 0x...`.
//
// Handlers installed later (Dolphin's fastmem handler) chain to this one for faults they don't
// handle.

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cxxabi.h>
#include <exception>
#include <typeinfo>

extern "C" char __text_start[];  // defined by the OpenOrbis link.x at the start of .text
// port/ps4_runtime.cpp: the calling thread's stack, when known (threads started through pthread_create and the
// main thread). Weak: a program may link without it.
extern "C" __attribute__((weak)) int ps4rt_thread_stack(uintptr_t* low, uintptr_t* high);
// src/StartGuard.cpp (00.70): a crash while a game starts or ends - the app's start watchdog restarts EmuSuNi for it
// meanwhile; it waits a few seconds at most and comes back. Weak: a program may link without it.
extern "C" __attribute__((weak)) void emusuni_start_crashed(int sig);
// (The headers' stack_t is musl's, its fields in another order than the kernel's.)
extern "C" int ps4_crash_sigaltstack(const void* stack, void* old) __asm__("sigaltstack");

namespace {

// FreeBSD values (the PS4 kernel's); the OpenOrbis musl headers may carry Linux ones.
constexpr int kSigIll = 4, kSigAbrt = 6, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
constexpr int kSaSiginfo = 0x40;
// The handler runs on the thread's stack for signals (sigaltstack): a thread whose own stack overflowed has no
// room left on it for the handler, and the app used to end without a word.
constexpr int kSaOnstack = 0x0001;
// A fault or stack pointer this far below the thread's stack is taken for an overflow of it.
constexpr uintptr_t kOverflowReach = 1024 * 1024;
// The main thread's stack for signals (the other threads get theirs as they start, port/ps4_runtime.cpp).
alignas(16) char g_mainSignalStack[64 * 1024];
constexpr int kMcontextOffset = 0x40;  // measured on hardware
// Registers in FreeBSD mcontext order (after mc_onstack).
constexpr const char* kRegNames[] = {"rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                     "rbp", "r10", "r11", "r12", "r13", "r14", "r15"};
constexpr int kRipIndex = 20, kRspIndex = 23;
constexpr uintptr_t kTextWindow = 0x4000000;  // 64 MiB covers the eboot's code

int g_traceFd = -1;

// Opened on first use: the heap may log before this file's constructor runs.
int traceFd() {
    if (g_traceFd < 0) {
        mkdir("/data/EmuSuNi", 0777);
        // A game start (launch.txt from the XMB) writes boot-trace.log; the XMB writes
        // xmb-trace.log, so restarting into the XMB keeps the last game's trace.
        struct stat st;
        const bool game = stat("/data/EmuSuNi/launch.txt", &st) == 0;
        if (!game) {
            // The traces of the sessions before (00.70, as src/main.cpp's logs): xmb-trace.log becomes
            // xmb-trace-1.log, ... xmb-trace-4.log (the oldest goes). Their lines have their times.
            rename("/data/EmuSuNi/xmb-trace-3.log", "/data/EmuSuNi/xmb-trace-4.log");
            rename("/data/EmuSuNi/xmb-trace-2.log", "/data/EmuSuNi/xmb-trace-3.log");
            rename("/data/EmuSuNi/xmb-trace-1.log", "/data/EmuSuNi/xmb-trace-2.log");
            rename("/data/EmuSuNi/xmb-trace.log", "/data/EmuSuNi/xmb-trace-1.log");
        }
        g_traceFd = open(game ? "/data/EmuSuNi/boot-trace.log" : "/data/EmuSuNi/xmb-trace.log",
                         O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    return g_traceFd;
}

void writeAll(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

__attribute__((format(printf, 2, 3))) void writeFormat(int fd, const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    writeAll(fd, line);
}

bool isCode(uint64_t address) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start);
    return address >= base && address < base + kTextWindow;
}

uint64_t toElf(uint64_t address) { return address - reinterpret_cast<uintptr_t>(__text_start); }

void setHandler(int sig, void (*handler)(int, siginfo_t*, void*), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    // The handler union is the first member; its member names differ between headers.
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void crashHandler(int sig, siginfo_t* info, void* context) {
    static volatile int s_depth = 0;
    if (s_depth++ > 0)
        return;  // crashed while reporting: give up

    const int fd = open("/data/EmuSuNi/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const uint64_t rip = regs[kRipIndex];
        const uint64_t rsp = regs[kRspIndex];
        const uint64_t address = *reinterpret_cast<const uint64_t*>(reinterpret_cast<char*>(info) + 24);
        writeFormat(fd, "\n=== signal %d (code %d), fault address 0x%llx\n", sig,
                    *reinterpret_cast<const int*>(reinterpret_cast<char*>(info) + 8),
                    static_cast<unsigned long long>(address));
        writeFormat(fd, "rip 0x%llx%s elf 0x%llx, rsp 0x%llx, text at %p\n",
                    static_cast<unsigned long long>(rip), isCode(rip) ? "" : " (not eboot code)",
                    static_cast<unsigned long long>(toElf(rip)), static_cast<unsigned long long>(rsp),
                    static_cast<void*>(__text_start));
        for (int i = 0; i < 15; i++)
            writeFormat(fd, "%s 0x%llx%s", kRegNames[i], static_cast<unsigned long long>(regs[1 + i]),
                        i % 4 == 3 ? "\n" : "  ");
        writeAll(fd, "\n");
        // The 16 KiB above the stack pointer, inside the thread's stack when it is known: below its bottom (an
        // overflow) or past its top there may be nothing to read, and a fault in here ends the app at once.
        uintptr_t from = rsp, to = rsp + 2048 * 8;
        uintptr_t low = 0, high = 0;
        if (ps4rt_thread_stack && ps4rt_thread_stack(&low, &high) && rsp + kOverflowReach >= low && rsp < high) {
            const bool overflow = rsp < low || (address < low && address + kOverflowReach >= low);
            writeFormat(fd, "thread stack 0x%llx-0x%llx (%llu KiB)%s\n", static_cast<unsigned long long>(low),
                        static_cast<unsigned long long>(high), static_cast<unsigned long long>((high - low) / 1024),
                        overflow ? ": it OVERFLOWED (the stack pointer or the fault is below it)" : "");
            if (from < low) {
                from = low;  // (the 16 KiB at its bottom: the deepest calls)
                to = low + 2048 * 8;
            }
            if (to > high)
                to = high;
        }
        writeAll(fd, "code addresses on the stack (elf):\n");
        int found = 0;
        for (uintptr_t at = from; at + 8 <= to && found < 48; at += 8) {
            const uint64_t value = *reinterpret_cast<const uint64_t*>(at);
            if (isCode(value)) {
                writeFormat(fd, "  [rsp+0x%llx] 0x%llx\n", static_cast<unsigned long long>(at - rsp),
                            static_cast<unsigned long long>(toElf(value)));
                found++;
            }
        }
        fsync(fd);  // (kept if the console hangs or is switched off next)
        close(fd);
    }
    if (traceFd() >= 0) {
        writeAll(traceFd(), "crashed (see crash.log)\n");
        fsync(traceFd());
    }
    // A game starting or ending: the app's start watchdog restarts EmuSuNi (src/StartGuard.cpp); this thread waits
    // for it here, 6 s at most.
    if (emusuni_start_crashed)
        emusuni_start_crashed(sig);

    // Back to the default action: returning re-executes the faulting instruction, which now
    // ends the process with the system's crash report.
    setHandler(sig, nullptr, 0);
    s_depth = 0;
}

// std::terminate (e.g. an uncaught exception): record what was thrown, then abort as usual.
[[noreturn]] void terminateHandler() {
    char message[512] = "std::terminate called";
    if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        int status = 0;
        // Type only: Dolphin builds with -fno-exceptions, so what() can't be fetched here.
        char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        snprintf(message, sizeof(message), "uncaught exception %s", name ? name : type->name());
    }
    const int fd = open("/data/EmuSuNi/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        writeFormat(fd, "\n=== %s\n", message);
        close(fd);
    }
    if (traceFd() >= 0)
        writeFormat(traceFd(), "%s\n", message);
    abort();
}

__attribute__((constructor(101))) void earlyInit() {
    if (traceFd() >= 0)
        writeFormat(traceFd(), "static init (text at %p)\n", static_cast<void*>(__text_start));
    std::set_terminate(terminateHandler);
    // FreeBSD's stack_t: the stack, its size, the flags.
    const struct {
        void* sp;
        size_t size;
        int flags;
    } stack = {g_mainSignalStack, sizeof(g_mainSignalStack), 0};
    ps4_crash_sigaltstack(&stack, nullptr);
    const int signals[] = {kSigIll, kSigAbrt, kSigFpe, kSigBus, kSigSegv};
    for (int sig : signals)
        setHandler(sig, crashHandler, kSaSiginfo | kSaOnstack);
}

}  // namespace

// Appends "<what>: callers 0x.. 0x.." to the boot trace: code addresses found on the calling
// thread's stack (ELF addresses, symbolize like crash.log). A cheap "who calls this" without
// frame pointers; may include stale return addresses.
extern "C" void ps4_boot_trace(const char* stage);
extern "C" __attribute__((noinline)) void ps4_trace_callers(const char* what) {
    // Read from the stack pointer itself: walking past a local variable is undefined behaviour
    // the optimizer is free to drop (an earlier version printed nothing).
    const uint64_t* stack;
    asm volatile("mov %%rsp, %0" : "=r"(stack));
    char line[320];
    int len = snprintf(line, sizeof(line), "%s: callers", what);
    int found = 0;
    for (int i = 0; i < 512 && found < 10 && len < 280; i++) {
        if (isCode(stack[i])) {
            len += snprintf(line + len, sizeof(line) - len, " 0x%llx",
                            static_cast<unsigned long long>(toElf(stack[i])));
            found++;
        }
    }
    ps4_boot_trace(line);
}

// The trace written through to the disk (src/Log.cpp LogSync: the start watchdog's, every 2 s while a game starts).
extern "C" void ps4_trace_sync(void) {
    if (g_traceFd >= 0)
        fsync(g_traceFd);
}

// Appends a line to /data/EmuSuNi/boot-trace.log (startup milestones), prefixed with the
// seconds since the first trace line.
extern "C" void ps4_boot_trace(const char* stage) {
    const int fd = traceFd();
    if (fd < 0)
        return;
    static timespec s_start;
    static bool s_started = false;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!s_started) {
        s_start = now;
        s_started = true;
    }
    const double seconds = static_cast<double>(now.tv_sec - s_start.tv_sec) +
                           static_cast<double>(now.tv_nsec - s_start.tv_nsec) / 1e9;
    // One write per line, so lines from different threads don't interleave.
    // Room for the monitor's long lines; a cut line still ends with its newline (a truncated
    // monitor line used to run into the next one).
    char line[2048];
    const size_t length = strlen(stage);
    const bool newline = length == 0 || stage[length - 1] != '\n';
    const int written =
        snprintf(line, sizeof(line), "[%8.3f] %s%s", seconds, stage, newline ? "\n" : "");
    if (written >= static_cast<int>(sizeof(line)))
        line[sizeof(line) - 2] = '\n';
    writeAll(fd, line);
}
