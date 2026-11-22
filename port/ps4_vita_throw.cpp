// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// Every C++ exception the PS Vita core throws, in the log before it is thrown: its type, its message and
// where it was thrown from. On the PS4 a C++ exception never reaches its catch (the app's own __cxa_throw
// with Sony's unwinder): the app ends there, without a crash report, so this line is all there is to go by.
// On the PC the catches work, and the lines say which exceptions the core throws as a matter of course:
// those are the ones to take out before the console.
//
// scripts/build-vita-core.sh links the core with ld.lld -r --wrap=__cxa_throw: the core's calls of
// __cxa_throw come here, __real___cxa_throw is the runtime's. After the 3DS core's own
// (scripts/build-3ds-core.sh: common/emusuni_throw.cpp), without its way back to retro_run: the Vita's
// threads run on their own, and most throws aren't under retro_run.
//
// (01.30) Rescue points, as the 3DS core's: a place a thread goes back to (longjmp) when a C++ exception is
// thrown under it - the PS4's stand-in for the catch that is never reached. The thread that prepares the
// content has one for all it does and one around each firmware part's install, so a failure there ends the
// load with a message (the menu says why) and EmuSuNi stays. Used as (the core's sources declare the calls):
//   EmusuniVitaRescueScope scope;                       // puts the thread's points back as they were, at its end
//   if (setjmp(*emusuni_vita_rescue_push("the firmware's install")) != 0) {
//       ... emusuni_vita_rescued(): the exception's type and message; fail as the catch would ...
//   }
// What the jump skips isn't undone (destructors, a lock held): what was being done is given up, the content
// closed. Always on the PS4; on the PC with EMUSUNI_VITA_RESCUE=1 (the tests), else the exception is thrown.
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

#include <cxxabi.h>
#include <unistd.h>
#include <xmmintrin.h>

extern "C" void retro_run(void); // (vita3k_retro_run: the build's prefix header renames it; its address goes in the log)

namespace {
std::atomic<unsigned> g_thrown{0};

// A line straight into the log (stderr: on the PS4 the app's log file), written and synced at once: no lock,
// no allocation, and on the disk before the app ends.
void RawLine(const char* text) {
    char line[1024];
    const int n = std::snprintf(line, sizeof(line), "Vita3K: %s\n", text);
    if (n > 0) {
        const size_t length = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
        if (write(STDERR_FILENO, line, length) < 0) {
        }
#if defined(__ORBIS__)
        fsync(STDERR_FILENO);
#endif
    }
}

// The Itanium C++ ABI's type_info layouts (the same for every runtime): a class without bases, one with a
// single public base at offset 0, one with several (each with its offset; a virtual base's is in the vtable).
struct ClassInfo {
    const void* vptr;
    const char* name;
};
struct SingleInfo {
    const void* vptr;
    const char* name;
    const ClassInfo* base;
};
struct BaseInfo {
    const ClassInfo* base;
    long offset_flags; // offset << 8; 1: virtual, 2: public
};
struct MultiInfo {
    const void* vptr;
    const char* name;
    unsigned flags;
    unsigned count;
    BaseInfo bases[1];
};
// The vtables of those kinds, from classes of this file.
struct KindA {
    virtual ~KindA();
};
KindA::~KindA() = default;
struct KindB : KindA {};
struct KindC {
    virtual ~KindC();
};
KindC::~KindC() = default;
struct KindD : KindA, KindC {};

const void* VptrOf(const std::type_info& type) {
    return *reinterpret_cast<const void* const*>(&type);
}
const char* NameOf(const ClassInfo* type) {
    const char* name = type->name;
    return name[0] == '*' ? name + 1 : name;
}
// The std::exception in the thrown object, if its class has one. std's own exceptions (St..., NSt3__1...,
// NSt7__cxx11...) derive from it alone, at offset 0 (their type_info may come from another runtime, Sony's:
// their kind isn't told by the vtable).
const std::exception* FindException(char* object, const ClassInfo* type, int depth) {
    if (type == nullptr || object == nullptr || depth > 12) {
        return nullptr;
    }
    const char* name = NameOf(type);
    if (std::strncmp(name, "St", 2) == 0 || std::strncmp(name, "NSt3__1", 7) == 0 ||
        std::strncmp(name, "NSt7__cxx11", 11) == 0) {
        return std::strcmp(name, "St13bad_exception") == 0 || std::strcmp(name, "NSt3__117nested_exceptionE") == 0 ||
                       std::strcmp(name, "St16nested_exception") == 0
                   ? nullptr
                   : reinterpret_cast<const std::exception*>(object);
    }
    if (type->vptr == VptrOf(typeid(KindB))) {
        return FindException(object, reinterpret_cast<const SingleInfo*>(type)->base, depth + 1);
    }
    if (type->vptr == VptrOf(typeid(KindD))) {
        const auto* multi = reinterpret_cast<const MultiInfo*>(type);
        for (unsigned i = 0; i < multi->count && i < 16; i++) {
            const long flags = multi->bases[i].offset_flags;
            if ((flags & 2) == 0) {
                continue;
            }
            long offset = flags >> 8;
            if ((flags & 1) != 0) {
                const char* vtable = *reinterpret_cast<char* const*>(object);
                offset = *reinterpret_cast<const long*>(vtable + offset);
            }
            if (const std::exception* found = FindException(object + offset, multi->bases[i].base, depth + 1)) {
                return found;
            }
        }
    }
    return nullptr;
}

// The rescue points of each thread, the innermost last.
struct Point {
    std::jmp_buf env;
    const char* where;
    unsigned mxcsr; // (the recompiler's rounding and flush-to-zero may be in MXCSR when the throw comes)
};
constexpr int kPoints = 8;
thread_local Point t_points[kPoints];
thread_local int t_depth = 0;       // (one more than kPoints deep: counted, not kept - no throw goes back to it)
thread_local std::jmp_buf t_spare;  // (that one's)
thread_local char t_rescued[512];   // what the thread's last rescue came back from

bool RescueOn() {
#if defined(__ORBIS__)
    return true;
#else
    static const bool on = std::getenv("EMUSUNI_VITA_RESCUE") != nullptr;
    return on;
#endif
}
bool Armed() {
    return t_depth > 0 && t_depth <= kPoints && RescueOn();
}
// Back to this thread's innermost point (Armed() only), which is the thread's no more.
[[noreturn]] void GoBack(const char* why) {
    Point& point = t_points[--t_depth];
    std::snprintf(t_rescued, sizeof(t_rescued), "%s", why);
    char line[400];
    std::snprintf(line, sizeof(line),
                  "EmuSuNi: it goes back to \"%s\": EmuSuNi stays, what was being done there is given up",
                  point.where);
    RawLine(line);
    _mm_setcsr(point.mxcsr);
    std::longjmp(point.env, 1);
}
} // namespace

extern "C" std::jmp_buf* emusuni_vita_rescue_push(const char* where) {
    if (t_depth < 0 || t_depth >= kPoints) {
        t_depth = t_depth < 0 ? 1 : t_depth + 1;
        return &t_spare;
    }
    Point& point = t_points[t_depth++];
    point.where = where;
    point.mxcsr = _mm_getcsr();
    return &point.env;
}
// The thread's points now; and back to that many (the end of a scope that pushed one, rescued or not).
extern "C" int emusuni_vita_rescue_depth(void) {
    return t_depth;
}
extern "C" void emusuni_vita_rescue_leave(int depth) {
    if (depth >= 0 && depth < t_depth) {
        t_depth = depth;
    }
}
extern "C" const char* emusuni_vita_rescued(void) {
    return t_rescued;
}
// For a failure that isn't a C++ exception yet (port/ps4_vita_files.cpp: the C++ library's own throws are
// outside this file's reach, so the port asks first): 1 when the thread has a point to go back to, and the
// way there, with the log's line - as for a throw.
extern "C" int emusuni_vita_rescue_armed(void) {
    return Armed() ? 1 : 0;
}
extern "C" void emusuni_vita_rescue_back(const char* why) {
    if (!Armed()) {
        return;
    }
    char line[768];
    std::snprintf(line, sizeof(line), "EmuSuNi: a failure a C++ exception would tell, in the core: %.600s", why);
    RawLine(line);
    GoBack(why);
}

extern "C" [[noreturn]] void __real___cxa_throw(void* object, std::type_info* type, void (*destructor)(void*));

extern "C" [[noreturn]] void __wrap___cxa_throw(void* object, std::type_info* type, void (*destructor)(void*)) {
    const unsigned n = ++g_thrown;
    void* const from = __builtin_return_address(0);
    const bool rescue = Armed();
#if defined(__ORBIS__)
    const bool every = true; // (without a rescue point the first one ends the app)
#else
    // (On the PC a caught one may come again and again: the first 20, then one in 100. EMUSUNI_VITA_THROWS=all
    // for every one, the tests' list.)
    static const bool all = std::getenv("EMUSUNI_VITA_THROWS") != nullptr;
    const bool every = all;
#endif
    char what[448] = "";
    if (every || rescue || n <= 20 || n % 100 == 0) {
        const char* mangled = type != nullptr ? type->name() : "?";
        int status = -1;
        char* demangled = type != nullptr ? abi::__cxa_demangle(mangled, nullptr, nullptr, &status) : nullptr;
        const std::exception* exception =
            type != nullptr ? FindException(static_cast<char*>(object), reinterpret_cast<const ClassInfo*>(type), 0)
                            : nullptr;
        const char* message = exception != nullptr ? exception->what() : nullptr;
        std::snprintf(what, sizeof(what), "%s%s%.300s", demangled != nullptr && status == 0 ? demangled : mangled,
                      message != nullptr ? ": " : "", message != nullptr ? message : "");
        std::free(demangled);
        // (Where from, as a distance from retro_run: the same in every run, for the build's symbols.)
        const long distance = static_cast<long>(reinterpret_cast<char*>(from) - reinterpret_cast<char*>(&retro_run));
        char line[768];
        std::snprintf(line, sizeof(line), "EmuSuNi: C++ exception %u thrown in the core: %s (thrown from %p, retro_run%+ld)%s",
                      n, what, from, distance,
#if defined(__ORBIS__)
                      rescue ? "" : "; on the PS4 its catch is never reached: EmuSuNi ends here"
#else
                      ""
#endif
        );
        RawLine(line);
    }
    if (rescue) {
        GoBack(what); // (the exception's object is left: a few bytes)
    }
    __real___cxa_throw(object, type, destructor);
}

#if !defined(__ORBIS__)
// The PC tests' proof that the lines above come: EMUSUNI_VITA_TEST_THROW throws two exceptions, and catches
// them, when the core is loaded (one of the runtime's classes, one of another kind).
#include <stdexcept>
namespace {
struct TestThrow {
    TestThrow() {
        if (std::getenv("EMUSUNI_VITA_TEST_THROW") == nullptr) {
            return;
        }
        try {
            throw std::runtime_error("EMUSUNI_VITA_TEST_THROW: a test exception");
        } catch (const std::exception&) {
        }
        try {
            throw 42;
        } catch (int) {
        }
    }
} g_test_throw;
} // namespace

// The PC tests' failure in the middle of the load: EMUSUNI_VITA_TEST_FAIL=<words> throws, once, at the first
// step of the start whose text has those words (port/ps4_vita_sys.c's emusuni_vita_step calls this).
#include <string>
extern "C" void emusuni_vita_test_fail(const char* step) {
    static const char* const at = std::getenv("EMUSUNI_VITA_TEST_FAIL");
    static std::atomic<bool> done{false};
    if (at != nullptr && at[0] != 0 && std::strstr(step, at) != nullptr && !done.exchange(true)) {
        throw std::runtime_error(std::string("EMUSUNI_VITA_TEST_FAIL: a test exception at the step \"") + step + "\"");
    }
}
#endif
