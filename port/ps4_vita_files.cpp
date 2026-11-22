// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// The PS Vita core's std::filesystem calls on the PS4 (01.30).
//
// What 01.29 showed on a console: the firmware's install ended the app twice, each time in a
// std::filesystem::remove_all. The C++ library's remove_all goes down the folder with openat, fdopendir and
// unlinkat. On the PS4 the app's C library has no unlinkat (it answers ENOSYS), and fdopendir is Sony's C
// library's, whose folder handle the app's own readdir and closedir then take for one of theirs: nothing is
// read, the handle is freed into the wrong heap (the log's "free ... not from this heap", from closedir) and
// a file descriptor that isn't the folder's is closed. So remove_all deletes nothing there: with an
// error_code it says so and the core went on ("Removed PUP_DEC" - it wasn't); without one it throws, and on
// the PS4 a C++ exception never reaches its catch: the app ends, without a crash report.
// std::filesystem::remove can't delete a folder there either: the C library's remove() tries rmdir only
// after unlink's EISDIR, and the PS4's unlink says EPERM for a folder.
//
// scripts/build-vita-core.sh links the core's calls of the C++ library's file system entry points over
// with the ones here (ld.lld -r --wrap; its FSWRAP lists, which its checks hold against the object):
//   - remove_all and remove are this file's own, on the calls that do work on the PS4: lstat, opendir,
//     readdir, closedir, unlink, rmdir, each by the whole path.
//   - Every other one that takes an error_code* (the PS4's C++ library: all of them do, the throwing forms
//     pass none) gets one when the core gave none, so the C++ library never throws from inside itself -
//     where port/ps4_vita_throw.cpp's log and rescue don't see it. A failure the core gave no error_code
//     for then goes back to the thread's rescue point (that file), as the exception would to its catch on
//     a PC: the load ends with a message. Where the thread has no rescue point (a game's own threads, the
//     frames) the PS4 gets the answer the error_code form gives, and a line in the log: a throw would end
//     the app.
// On the PC (the tests: the GNU C++ library, other names) only remove_all and remove are linked over, so
// that the tests delete with this file's code; a failure without an error_code is thrown there as before.
// EMUSUNI_VITA_TEST_NODELETE=<words> (PC): a folder whose path has those words can't be deleted, as on 01.29.
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

extern "C" void emusuni_vita_log(const char* format, ...);
extern "C" void emusuni_vita_sync(void);
extern "C" int emusuni_vita_rescue_armed(void);
extern "C" void emusuni_vita_rescue_back(const char* why);

// (The PC's throw of what the C++ library would have thrown. Never on the PS4: Failed() answers 0 there.)
#if defined(__ORBIS__)
#define EMUSUNI_VITA_THROW(error) ((void)0)
#else
#define EMUSUNI_VITA_THROW(error) throw error
#endif

namespace {
// A file, a link or a folder with all it holds, deleted; `count` gets what was deleted. 0, or the errno of
// the first thing that couldn't be (the rest is still deleted as far as it goes). A path that isn't there: 0.
int RemoveTree(std::string& path, std::uintmax_t& count, int depth) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT ? 0 : errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path.c_str()) != 0) {
            return errno == ENOENT ? 0 : errno;
        }
        count++;
        return 0;
    }
    if (depth > 100) {
        return ELOOP;
    }
#if !defined(__ORBIS__)
    static const char* const stuck = std::getenv("EMUSUNI_VITA_TEST_NODELETE");
    if (stuck != nullptr && stuck[0] != 0 && path.find(stuck) != std::string::npos) {
        return EPERM;
    }
#endif
    // (The names first: the folder isn't read while it changes.)
    std::vector<std::string> names;
    DIR* const dir = opendir(path.c_str());
    if (dir == nullptr) {
        return errno;
    }
    while (const struct dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            names.emplace_back(entry->d_name);
        }
    }
    closedir(dir);
    int first = 0;
    const size_t length = path.size();
    for (const std::string& name : names) {
        path += '/';
        path += name;
        const int error = RemoveTree(path, count, depth + 1);
        path.resize(length);
        if (error != 0 && first == 0) {
            first = error;
        }
    }
    if (rmdir(path.c_str()) != 0) {
        if (first == 0 && errno != ENOENT) {
            first = errno;
        }
    } else {
        count++;
    }
    return first;
}

// std::filesystem::remove: a file or link, or an empty folder. 1 deleted, 0 it wasn't there; else -1 and errno.
int RemoveOne(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if ((S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    return 1;
}

// A call the core gave no error_code failed. With a rescue point on this thread: back to it (no return).
// On the PC: 1, the caller throws what the C++ library would. On the PS4 without a rescue point: a line in
// the log (the first of each run, then one in a hundred) and 0 - the caller answers as with an error_code.
int Failed(const char* what, const char* path1, const char* path2, const std::error_code& error) {
    char why[700];
    std::snprintf(why, sizeof(why), "std::filesystem %s(%.250s%s%.250s) failed: %.80s (error %d)", what,
                  path1 != nullptr ? path1 : "", path2 != nullptr ? ", " : "", path2 != nullptr ? path2 : "",
                  error.message().c_str(), error.value());
    if (emusuni_vita_rescue_armed()) {
        emusuni_vita_rescue_back(why);
    }
#if defined(__ORBIS__)
    static std::atomic<unsigned> said{0};
    const unsigned n = ++said;
    if (n <= 40 || n % 100 == 0) {
        emusuni_vita_log("vita: files: %s; no rescue point on this thread: the call answers as with an error code "
                         "(a C++ exception would end EmuSuNi) [%u]",
                         why, n);
        emusuni_vita_sync();
    }
    return 0;
#else
    return 1;
#endif
}

std::uintmax_t RemoveAll(const fs::path& p, std::error_code* ec, const char* what) {
    if (ec != nullptr) {
        ec->clear();
    }
    std::string path = p.string();
    std::uintmax_t count = 0;
    const int error = path.empty() ? 0 : RemoveTree(path, count, 0);
    if (error == 0) {
        return count;
    }
    const std::error_code code(error, std::generic_category());
    // (Always said: a folder that stays is a firmware install's or a game's leftover.)
    emusuni_vita_log("vita: files: %s couldn't be deleted whole: %s (error %d), %llu deleted of it", p.string().c_str(),
                     code.message().c_str(), error, static_cast<unsigned long long>(count));
    if (ec != nullptr) {
        *ec = code;
    } else if (Failed(what, p.c_str(), nullptr, code) != 0) {
        EMUSUNI_VITA_THROW(fs::filesystem_error(what, p, code));
    }
    return static_cast<std::uintmax_t>(-1);
}

bool Remove(const fs::path& p, std::error_code* ec, const char* what) {
    if (ec != nullptr) {
        ec->clear();
    }
    const int removed = RemoveOne(p.c_str());
    if (removed >= 0) {
        return removed == 1;
    }
    const std::error_code code(errno, std::generic_category());
    if (ec != nullptr) {
        *ec = code;
    } else if (Failed(what, p.c_str(), nullptr, code) != 0) {
        EMUSUNI_VITA_THROW(fs::filesystem_error(what, p, code));
    }
    return false;
}
} // namespace

// The tests' way to this file's own deleting, without a C++ library in between. 0, or the errno.
extern "C" int emusuni_vita_remove_tree(const char* path, unsigned long long* deleted) {
    std::string whole = path;
    std::uintmax_t count = 0;
    const int error = RemoveTree(whole, count, 0);
    if (deleted != nullptr) {
        *deleted = count;
    }
    return error;
}

#if defined(_LIBCPP_VERSION)
// ---- The PS4's C++ library (LLVM's): each entry point takes an error_code*, null from the throwing forms. ----
#define EMUSUNI_FS "_ZNSt3__14__fs10filesystem"
#define EMUSUNI_REAL(mangled) __asm__("__real_" EMUSUNI_FS mangled)
#define EMUSUNI_WRAP(mangled) __asm__("__wrap_" EMUSUNI_FS mangled)

namespace {
// `call` with the core's error_code, or with one of this file's when it gave none: then a failure goes to
// Failed() instead of being thrown inside the C++ library. (not_found: for status(), whose error_code also
// tells of a path that isn't there, which is no failure and throws nothing.)
template <class Call>
auto Guarded(const char* what, const fs::path* p1, const fs::path* p2, std::error_code* ec, Call call,
             bool not_found_is_fine = false) -> decltype(call(ec)) {
    if (ec != nullptr) {
        return call(ec);
    }
    std::error_code own;
    const auto failed = [&] {
        if (!own || (not_found_is_fine && (own.value() == ENOENT || own.value() == ENOTDIR))) {
            return;
        }
        // (01.31) And a name with no folder ("Arial", "ltn0.pvf": the overlay looks for its fonts by their bare
        // name first): the PS4 has no current folder and refuses a relative path with EINVAL. Nothing is there,
        // which is what the caller is told either way - 01.30's log had a line for each.
        if (not_found_is_fine && own.value() == EINVAL && p1 != nullptr && !p1->is_absolute()) {
            return;
        }
        if (Failed(what, p1 != nullptr ? p1->c_str() : nullptr, p2 != nullptr ? p2->c_str() : nullptr, own) != 0) {
            if (p2 != nullptr) {
                EMUSUNI_VITA_THROW(fs::filesystem_error(what, *p1, *p2, own));
            } else if (p1 != nullptr) {
                EMUSUNI_VITA_THROW(fs::filesystem_error(what, *p1, own));
            }
            EMUSUNI_VITA_THROW(fs::filesystem_error(what, own));
        }
    };
    if constexpr (std::is_void_v<decltype(call(ec))>) {
        call(&own);
        failed();
    } else {
        auto result = call(&own);
        failed();
        return result;
    }
}
} // namespace

std::uintmax_t EmusuniRemoveAll(const fs::path&, std::error_code*) EMUSUNI_WRAP("12__remove_allERKNS1_4pathEPNS_10error_codeE");
std::uintmax_t EmusuniRemoveAll(const fs::path& p, std::error_code* ec) {
    return RemoveAll(p, ec, "remove_all");
}
bool EmusuniRemove(const fs::path&, std::error_code*) EMUSUNI_WRAP("8__removeERKNS1_4pathEPNS_10error_codeE");
bool EmusuniRemove(const fs::path& p, std::error_code* ec) {
    return Remove(p, ec, "remove");
}

// (One path.)
#define EMUSUNI_GUARD_1(RESULT, NAME, MANGLED, NOT_FOUND_IS_FINE)                                           \
    RESULT EmusuniReal_##NAME(const fs::path&, std::error_code*) EMUSUNI_REAL(MANGLED);                     \
    RESULT Emusuni_##NAME(const fs::path&, std::error_code*) EMUSUNI_WRAP(MANGLED);                         \
    RESULT Emusuni_##NAME(const fs::path& p, std::error_code* ec) {                                         \
        return Guarded(#NAME, &p, nullptr, ec, [&](std::error_code* e) { return EmusuniReal_##NAME(p, e); }, \
                       NOT_FOUND_IS_FINE);                                                                  \
    }
// (Two paths.)
#define EMUSUNI_GUARD_2(RESULT, NAME, MANGLED)                                                              \
    RESULT EmusuniReal_##NAME(const fs::path&, const fs::path&, std::error_code*) EMUSUNI_REAL(MANGLED);    \
    RESULT Emusuni_##NAME(const fs::path&, const fs::path&, std::error_code*) EMUSUNI_WRAP(MANGLED);        \
    RESULT Emusuni_##NAME(const fs::path& a, const fs::path& b, std::error_code* ec) {                      \
        return Guarded(#NAME, &a, &b, ec, [&](std::error_code* e) { return EmusuniReal_##NAME(a, b, e); }); \
    }
// (Two paths and the copy's options.)
#define EMUSUNI_GUARD_COPY(RESULT, NAME, MANGLED)                                                                         \
    RESULT EmusuniReal_##NAME(const fs::path&, const fs::path&, fs::copy_options, std::error_code*) EMUSUNI_REAL(MANGLED); \
    RESULT Emusuni_##NAME(const fs::path&, const fs::path&, fs::copy_options, std::error_code*) EMUSUNI_WRAP(MANGLED);     \
    RESULT Emusuni_##NAME(const fs::path& a, const fs::path& b, fs::copy_options options, std::error_code* ec) {           \
        return Guarded(#NAME, &a, &b, ec, [&](std::error_code* e) { return EmusuniReal_##NAME(a, b, options, e); });       \
    }

EMUSUNI_GUARD_1(fs::file_status, status, "8__statusERKNS1_4pathEPNS_10error_codeE", true)
EMUSUNI_GUARD_1(std::uintmax_t, file_size, "11__file_sizeERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_1(bool, is_empty, "13__fs_is_emptyERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_1(bool, create_directory, "18__create_directoryERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_1(bool, create_directories, "20__create_directoriesERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_1(fs::file_time_type, last_write_time, "17__last_write_timeERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_1(fs::space_info, space, "7__spaceERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_1(fs::path, absolute, "10__absoluteERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_1(fs::path, weakly_canonical, "18__weakly_canonicalERKNS1_4pathEPNS_10error_codeE", false)
EMUSUNI_GUARD_2(bool, equivalent, "12__equivalentERKNS1_4pathES4_PNS_10error_codeE")
EMUSUNI_GUARD_2(void, rename, "8__renameERKNS1_4pathES4_PNS_10error_codeE")
EMUSUNI_GUARD_COPY(bool, copy_file, "11__copy_fileERKNS1_4pathES4_NS1_12copy_optionsEPNS_10error_codeE")
EMUSUNI_GUARD_COPY(void, copy, "6__copyERKNS1_4pathES4_NS1_12copy_optionsEPNS_10error_codeE")

void EmusuniReal_resize_file(const fs::path&, std::uintmax_t, std::error_code*) EMUSUNI_REAL("13__resize_fileERKNS1_4pathEmPNS_10error_codeE");
void Emusuni_resize_file(const fs::path&, std::uintmax_t, std::error_code*) EMUSUNI_WRAP("13__resize_fileERKNS1_4pathEmPNS_10error_codeE");
void Emusuni_resize_file(const fs::path& p, std::uintmax_t size, std::error_code* ec) {
    return Guarded("resize_file", &p, nullptr, ec, [&](std::error_code* e) { return EmusuniReal_resize_file(p, size, e); });
}

fs::path EmusuniReal_current_path(std::error_code*) EMUSUNI_REAL("14__current_pathEPNS_10error_codeE");
fs::path Emusuni_current_path(std::error_code*) EMUSUNI_WRAP("14__current_pathEPNS_10error_codeE");
fs::path Emusuni_current_path(std::error_code* ec) {
    return Guarded("current_path", nullptr, nullptr, ec, [&](std::error_code* e) { return EmusuniReal_current_path(e); });
}

// (The folders' readers: the constructors - which build the object in the memory given - and operator++.)
void EmusuniReal_dir(fs::directory_iterator*, const fs::path&, std::error_code*, fs::directory_options)
    EMUSUNI_REAL("18directory_iteratorC2ERKNS1_4pathEPNS_10error_codeENS1_17directory_optionsE");
void Emusuni_dir(fs::directory_iterator*, const fs::path&, std::error_code*, fs::directory_options)
    EMUSUNI_WRAP("18directory_iteratorC2ERKNS1_4pathEPNS_10error_codeENS1_17directory_optionsE");
void Emusuni_dir(fs::directory_iterator* self, const fs::path& p, std::error_code* ec, fs::directory_options options) {
    return Guarded("directory_iterator", &p, nullptr, ec,
                   [&](std::error_code* e) { return EmusuniReal_dir(self, p, e, options); });
}
fs::directory_iterator& EmusuniReal_dir_next(fs::directory_iterator*, std::error_code*)
    EMUSUNI_REAL("18directory_iterator11__incrementEPNS_10error_codeE");
fs::directory_iterator& Emusuni_dir_next(fs::directory_iterator*, std::error_code*)
    EMUSUNI_WRAP("18directory_iterator11__incrementEPNS_10error_codeE");
fs::directory_iterator& Emusuni_dir_next(fs::directory_iterator* self, std::error_code* ec) {
    Guarded("directory_iterator's next", nullptr, nullptr, ec, [&](std::error_code* e) { EmusuniReal_dir_next(self, e); });
    return *self;
}
void EmusuniReal_tree(fs::recursive_directory_iterator*, const fs::path&, fs::directory_options, std::error_code*)
    EMUSUNI_REAL("28recursive_directory_iteratorC2ERKNS1_4pathENS1_17directory_optionsEPNS_10error_codeE");
void Emusuni_tree(fs::recursive_directory_iterator*, const fs::path&, fs::directory_options, std::error_code*)
    EMUSUNI_WRAP("28recursive_directory_iteratorC2ERKNS1_4pathENS1_17directory_optionsEPNS_10error_codeE");
void Emusuni_tree(fs::recursive_directory_iterator* self, const fs::path& p, fs::directory_options options,
                  std::error_code* ec) {
    return Guarded("recursive_directory_iterator", &p, nullptr, ec,
                   [&](std::error_code* e) { return EmusuniReal_tree(self, p, options, e); });
}
fs::recursive_directory_iterator& EmusuniReal_tree_next(fs::recursive_directory_iterator*, std::error_code*)
    EMUSUNI_REAL("28recursive_directory_iterator11__incrementEPNS_10error_codeE");
fs::recursive_directory_iterator& Emusuni_tree_next(fs::recursive_directory_iterator*, std::error_code*)
    EMUSUNI_WRAP("28recursive_directory_iterator11__incrementEPNS_10error_codeE");
fs::recursive_directory_iterator& Emusuni_tree_next(fs::recursive_directory_iterator* self, std::error_code* ec) {
    Guarded("recursive_directory_iterator's next", nullptr, nullptr, ec,
            [&](std::error_code* e) { EmusuniReal_tree_next(self, e); });
    return *self;
}

#else
// ---- The PC's C++ library (GNU's): the throwing form and the error_code form are two functions. ----
std::uintmax_t EmusuniRemoveAllThrows(const fs::path&) __asm__("__wrap__ZNSt10filesystem10remove_allERKNS_7__cxx114pathE");
std::uintmax_t EmusuniRemoveAllThrows(const fs::path& p) {
    return RemoveAll(p, nullptr, "cannot remove all");
}
std::uintmax_t EmusuniRemoveAllCode(const fs::path&, std::error_code&)
    __asm__("__wrap__ZNSt10filesystem10remove_allERKNS_7__cxx114pathERSt10error_code");
std::uintmax_t EmusuniRemoveAllCode(const fs::path& p, std::error_code& ec) {
    return RemoveAll(p, &ec, "cannot remove all");
}
bool EmusuniRemoveThrows(const fs::path&) __asm__("__wrap__ZNSt10filesystem6removeERKNS_7__cxx114pathE");
bool EmusuniRemoveThrows(const fs::path& p) {
    return Remove(p, nullptr, "cannot remove");
}
bool EmusuniRemoveCode(const fs::path&, std::error_code&) __asm__("__wrap__ZNSt10filesystem6removeERKNS_7__cxx114pathERSt10error_code");
bool EmusuniRemoveCode(const fs::path& p, std::error_code& ec) {
    return Remove(p, &ec, "cannot remove");
}
#endif
