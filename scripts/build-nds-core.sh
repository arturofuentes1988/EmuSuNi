#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds melonDS DS (JesseTG/melonds-ds, ~/src/melonds-ds, GPL-3.0: the libretro core of a recent
# melonDS) for the PS4 into one object that scripts/build.sh links into EmuSuNi: the Nintendo DS.
# Its own CMake makes the core with the PS4 toolchain (toolchain/ps4-love-modern.cmake): melonDS's
# x86-64 JIT, the software renderer (2D and 3D; in threads of their own when
# melonds_threaded_renderer is on), FreeBIOS and generated firmware (built in: used when no BIOS or
# firmware file is found), every screen layout of the core. No OpenGL (nor the compute renderer),
# no networking (Wi-Fi), no dlopen, no Tracy. The dependencies its CMake would download (melonDS
# itself, libretro-common, fmt, glm, zlib...) come from the clones of scripts/setup.sh in
# ~/src/melonds-ds-deps, at the commits it pins (nothing is downloaded here). Its libretro API
# becomes melondsds_retro_* and everything else in it private, as for FCEUmm (CMakeLists.txt).
# The PS4 changes to the sources (applied each time to the files as git has them, checked; all
# under EMUSUNI_PORT, which the CMake option of that name defines):
#   - The JIT's code buffer (melonDS's own array in the core's data, so that its code reaches the
#     core's functions with 32-bit calls): one slice of 16 MiB (upstream: four of 32, one per
#     console instance; EmuSuNi runs one at a time), made executable through EmuSuNi
#     (port/ps4_n64.c: emusuni_make_executable, the ways the PS4 allows) instead of mprotect, once.
#     If that fails the core runs melonDS's interpreter instead of crashing.
#   - No fastmem: it relies on page faults (the app's fault handler is its crash log), an 8 GiB
#     reservation and a shared memory object (shm_open), which the PS4 doesn't have the normal way.
#     The emulated RAM comes from the heap; the JIT calls the memory handlers. No SIGSEGV handler.
#   - The threaded renderer (melonds_threaded_renderer) uses three threads besides the emulation
#     thread, each calling emusuni_nds_thread(name) first, which the app defines (port/): their
#     cores (none the emulation thread's, nor each other's: they wait for each other awake),
#     priority and name. All of it gives the same pictures as melonDS's single thread (checked
#     frame by frame on Mario Kart DS and New Super Mario Bros.):
#       "nds 3d", "nds 3d 2": the 3D rasterizer split in two, the even lines and the odd ones (each
#       thread with its own copy of the polygons' edges); the emulation thread gets each line as it
#       is finished, in order, and waits for one awake for up to ~2 ms of the TSC before it sleeps.
#       "nds 2d": the sub screen's 2D engine (B), each line from a copy of its registers taken at the
#       line's HBlank, while the emulation thread draws the main one's (A) and goes on emulating;
#       it waits for B's lines only when 8 are queued, at the frame's end, or before a write to
#       what B's drawing reads (B's palette or OAM, VRAM mapped to B, the VRAM mapping).
#     Their threads get 1 MiB stacks.
#   - The idle turns of NDS::RunFrame's loop (both CPUs halted waiting for an interrupt, no DMA, the
#     3D engine idle: 60% of the turns in a race) taken at once up to the next event or timer
#     overflow, with the same result as one 64-cycle turn after another.
#   - The JIT's main RAM accesses (DS mode) straight from its slow path (no thread_local, no virtual
#     call); the extended BGs' palette worked out in the loop.
#   - A frame the frontend won't show (GET_AUDIO_VIDEO_ENABLE without the picture: the app's frame
#     skip and catch-up) isn't drawn: no 2D, no screen layout, a duplicated frame (NULL) instead;
#     the emulation, and the frames shown, are the same.
#   - A log line every ~10 s: where the emulation thread's time went (2D, waiting for the 3D or the
#     2D helper, sound, the CPUs and the rest), the screens, the frontend's video and audio, the 3D
#     threads' and the 2D helper's time, the polygons (the TSC: a thousand reads a frame).
#   - PGO: scripts/nds-core.profdata.xz (see below).
#   - One copy of the ROM, the cart's (upstream: two while playing, three or four while loading):
#     the frontend's data is borrowed (not copied) during retro_load_game, then only its header is
#     kept, and a reset moves the cart to the new console. The app may free its copy once
#     retro_load_game returns. Or the ROM from its path (a retro_game_info with a path and no
#     data): read with stdio straight into the cart's buffer, the only allocation; if it can't be
#     read, retro_load_game returns false. (need_fullpath stays false: data works as before.)
#   - No networking: melonDS DS's network state does nothing (its own build without networking
#     doesn't compile: it always includes libslirp's and libpcap's code).
#   - melonDS's thread_local console pointer declared constant-initialised (no weak reference to
#     an initialiser left in the object: the app would export it as an import nobody gives).
#   - Built as a static library, without LTO (ld -r of LLVM bitcode), without mmap of files, with
#     the few libretro-common files the others call (a shared library may leave those undefined).
# The object is checked at the end: only melondsds_retro_* defined globally, no weak references
# left, the C++ operator new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's
# heap) and none defined in it, and every other symbol it needs found in what the app links
# (OpenOrbis' libc, libkernel, libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind,
# port/ps4_n64.c; emusuni_nds_thread from port/).
# Only what changed is compiled again (ninja); everything when the flags, the profile or this script
# change.
#   build-nds-core.sh <out dir> [clean]
#   NDS_CORE_HOST=1 build-nds-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                 the PC tests: no PS4 checks; the test frontend
#                                                 provides emusuni_make_executable and
#                                                 emusuni_nds_thread
#   NDS_CORE_PGO=off|generate                     without the profile; instrumented to make one (PC)
#   NDS_CORE_TIMING_DETAIL=1                      the log's "detail" line too (the PC tests)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${MELONDSDS_DIR:-$HOME/src/melonds-ds}"
DEPS="${MELONDSDS_DEPS_DIR:-$HOME/src/melonds-ds-deps}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/nds-core.log"
HOST="${NDS_CORE_HOST:-0}"

python3 -I - "$SRC" "$DEPS" <<'PATCH'
import subprocess
import sys
core, deps = sys.argv[1], sys.argv[2]
patched = {}
def patch(root, path, old, new):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    key = (root, path)
    if key not in patched:
        blob = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + path], check=True,
                              capture_output=True).stdout
        patched[key] = blob.decode('utf-8').replace('\r\n', '\n')
    text = patched[key]
    assert text.count(old) == 1, (root, path, text.count(old), old[:60])
    patched[key] = text.replace(old, new)
melonds = deps + '/melonDS'
lrc = deps + '/libretro-common'

# --- The build: the EMUSUNI_PORT option, the dependencies' own trees, a static core.
patch(core, 'CMakeLists.txt',
      'include(cmake/utils.cmake)\ninclude(cmake/FetchDependencies.cmake)\n',
      '# EmuSuNi (scripts/build-nds-core.sh): the core as one object of the PS4 app (a static library),\n'
      '# with the changes under EMUSUNI_PORT (melonDS\'s too).\n'
      'option(EMUSUNI_PORT "Build the core for EmuSuNi (PS4)" OFF)\n'
      'if (EMUSUNI_PORT)\n    add_compile_definitions(EMUSUNI_PORT=1)\nendif ()\n\n'
      'include(cmake/utils.cmake)\ninclude(cmake/FetchDependencies.cmake)\n')
patch(core, 'cmake/FetchDependencies.cmake',
      'set(CMAKE_MODULE_PATH "${FETCHCONTENT_BASE_DIR}/melonds-src/cmake" "${FETCHCONTENT_BASE_DIR}/embed-binaries-src/cmake" "${CMAKE_MODULE_PATH}")\n',
      'if (FETCHCONTENT_SOURCE_DIR_MELONDS AND FETCHCONTENT_SOURCE_DIR_EMBED-BINARIES)\n'
      '    # EmuSuNi: the dependencies come from their own source trees (scripts/setup.sh clones them).\n'
      '    set(CMAKE_MODULE_PATH "${FETCHCONTENT_SOURCE_DIR_MELONDS}/cmake" "${FETCHCONTENT_SOURCE_DIR_EMBED-BINARIES}/cmake" "${CMAKE_MODULE_PATH}")\n'
      'else ()\n'
      '    set(CMAKE_MODULE_PATH "${FETCHCONTENT_BASE_DIR}/melonds-src/cmake" "${FETCHCONTENT_BASE_DIR}/embed-binaries-src/cmake" "${CMAKE_MODULE_PATH}")\n'
      'endif ()\n')
patch(core, 'src/libretro/CMakeLists.txt',
      'if (BUILD_AS_SHARED_LIBRARY)\n    set(LIBRARY_TYPE SHARED)\n',
      'if (EMUSUNI_PORT)\n    # EmuSuNi: linked into the app (scripts/build-nds-core.sh makes one object of it).\n'
      '    set(LIBRARY_TYPE STATIC)\nelseif (BUILD_AS_SHARED_LIBRARY)\n    set(LIBRARY_TYPE SHARED)\n')
patch(core, 'src/libretro/CMakeLists.txt',
      'if (APPLE)\n    set_target_properties(melondsds_libretro PROPERTIES SUFFIX ".dylib")\n',
      'if (EMUSUNI_PORT)\n    # EmuSuNi: a static library, named as one.\n'
      'elseif (APPLE)\n    set_target_properties(melondsds_libretro PROPERTIES SUFFIX ".dylib")\n')
# --- No networking (Wi-Fi through libslirp's sockets, or libpcap through dlopen): melonDS DS's
# network state does nothing (upstream never builds without it; its own guards are incomplete).
patch(core, 'src/libretro/net/net.hpp',
      '#include "Net.h"\n',
      '#ifdef HAVE_NETWORKING\n#include "Net.h"\n#else\n#include <vector>\n#include "types.h" // EmuSuNi: no melonDS::Net\n#endif\n')
patch(core, 'src/libretro/net/net.hpp',
      'namespace melonDS\n{\n    class NetDriver;\n}\n',
      'namespace melonDS\n{\n    class NetDriver;\n'
      '    struct AdapterData; // EmuSuNi: Net_PCap.h defines it, included only with the direct mode\n}\n')
patch(core, 'src/libretro/net/net.hpp',
      '    private:\n        melonDS::Net _net;\n',
      '    private:\n#ifdef HAVE_NETWORKING\n        melonDS::Net _net;\n#endif\n')
patch(core, 'src/libretro/net/net.cpp',
      '#include "net.hpp"\n',
      '#include "net.hpp"\n\n#ifdef HAVE_NETWORKING\n')
patch(core, 'src/libretro/net/net.cpp',
      '    return NetworkMode::None;\n}\n',
      '    return NetworkMode::None;\n}\n'
      '#else\n'
      '// EmuSuNi: built without networking: no packets, no driver, the network mode is always None.\n'
      'MelonDsDs::NetState::NetState() {}\n'
      'MelonDsDs::NetState::~NetState() noexcept {}\n'
      'int MelonDsDs::NetState::SendPacket(std::span<std::byte>) noexcept { return 0; }\n'
      'int MelonDsDs::NetState::RecvPacket(melonDS::u8*) noexcept { return 0; }\n'
      'void MelonDsDs::NetState::Apply(const CoreConfig&) noexcept {}\n'
      'MelonDsDs::NetworkMode MelonDsDs::NetState::GetNetworkMode() const noexcept { return NetworkMode::None; }\n'
      '#endif\n')
patch(core, 'src/libretro/config/config.cpp',
      '#include <Net_PCap.h>\n',
      '#ifdef HAVE_NETWORKING_DIRECT_MODE // EmuSuNi: libpcap\'s, only with libpcap\n#include <Net_PCap.h>\n#endif\n')
patch(core, 'src/libretro/retro/http.cpp',
      '#include "http.hpp"\n',
      '#include "http.hpp"\n\n#ifdef HAVE_NETWORKING // EmuSuNi: libretro-common\'s HTTP (sockets), only with networking\n')
patch(core, 'src/libretro/retro/http.cpp',
      '    return std::span((const std::byte*)payload, length);\n}',
      '    return std::span((const std::byte*)payload, length);\n}\n#endif')
# --- libretro-common: what the files it builds call, though never in this core (a shared library
# may leave those undefined; linked into the app, every symbol must be there).
patch(core, 'cmake/libretro-common.cmake',
      '    ${libretro-common_SOURCE_DIR}/vfs/vfs_implementation.c\n    )\n',
      '    ${libretro-common_SOURCE_DIR}/vfs/vfs_implementation.c\n    )\n\n'
      'if (EMUSUNI_PORT)\n'
      '    # EmuSuNi: the rest of what those files call (the core is linked into the app).\n'
      '    target_sources(libretro-common PRIVATE\n'
      '        ${libretro-common_SOURCE_DIR}/compat/compat_strcasestr.c\n'
      '        ${libretro-common_SOURCE_DIR}/formats/data_transfer.c\n'
      '        ${libretro-common_SOURCE_DIR}/formats/png/rpng_apng.c\n'
      '        ${libretro-common_SOURCE_DIR}/formats/xml/rxml.c\n'
      '        )\n'
      'endif ()\n')
patch(core, 'src/libretro/format.cpp',
      'auto fmt::formatter<MelonDsDs::FormattedPCapFlags>::format(',
      '#ifdef HAVE_NETWORKING_DIRECT_MODE // EmuSuNi: libpcap\'s flags, only with libpcap\n'
      'auto fmt::formatter<MelonDsDs::FormattedPCapFlags>::format(')
patch(core, 'src/libretro/format.cpp',
      '    return formatter<decltype(flagNames)>::format(flagNames, ctx);\n}\n',
      '    return formatter<decltype(flagNames)>::format(flagNames, ctx);\n}\n#endif\n')

# --- The JIT's code buffer: one 16 MiB slice, executable through EmuSuNi, else the interpreter.
patch(melonds, 'src/ARMJIT_Global.h',
      'static constexpr size_t CodeMemorySliceSize = 1024*1024*32;\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: 16 MiB (in the PS4 app\'s data, next to the other cores\' buffers); a full buffer is\n'
      '// flushed ("near reset"/"far reset" in the log) and filled again.\n'
      'static constexpr size_t CodeMemorySliceSize = 1024*1024*16;\n'
      '#else\n'
      'static constexpr size_t CodeMemorySliceSize = 1024*1024*32;\n'
      '#endif\n')
patch(melonds, 'src/ARMJIT_Global.h',
      'void* AllocateCodeMem();\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: whether the code buffer could be made executable (tried once, the first time).\n'
      'bool CodeMemoryExecutable();\n'
      '#endif\n\n'
      'void* AllocateCodeMem();\n')
patch(melonds, 'src/ARMJIT_Global.cpp',
      '#include <mutex>\n',
      '#include <mutex>\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "Platform.h"\n\n'
      '// EmuSuNi (port/ps4_n64.c): makes [address, address + size) executable the ways the PS4 allows,\n'
      '// each checked by running a small function there; the way that worked (0-3) or -1.\n'
      'extern "C" int emusuni_make_executable(void* address, size_t size);\n'
      '#endif\n')
patch(melonds, 'src/ARMJIT_Global.cpp',
      'static constexpr size_t NumCodeMemSlices = 4;\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: one console at a time (a reset destroys the old one before it makes the new one).\n'
      'static constexpr size_t NumCodeMemSlices = 1;\n'
      '#else\n'
      'static constexpr size_t NumCodeMemSlices = 4;\n'
      '#endif\n')
patch(melonds, 'src/ARMJIT_Global.cpp',
      'int RefCounter = 0;\n',
      '#ifdef EMUSUNI_PORT\n'
      'bool CodeMemoryExecutable()\n'
      '{\n'
      '    // On its own pages (the PS4\'s are 16 KiB: the array has room to align to them); it stays\n'
      '    // executable while the app runs (the next game uses it again).\n'
      '    static int method = -2;\n'
      '    if (method == -2)\n'
      '    {\n'
      '        method = emusuni_make_executable(GetAlignedCodeMemoryStart(), CodeMemoryAlignedSize);\n'
      '        if (method >= 0)\n'
      '            Platform::Log(Platform::LogLevel::Info, "JIT: %zu MiB of executable code memory at %p (way %d)\\n",\n'
      '                          CodeMemoryAlignedSize >> 20, (void*)GetAlignedCodeMemoryStart(), method);\n'
      '        else\n'
      '            Platform::Log(Platform::LogLevel::Error, "JIT: the code memory can\'t be made executable\\n");\n'
      '    }\n'
      '    return method >= 0;\n'
      '}\n'
      '#endif\n\n'
      'int RefCounter = 0;\n')
patch(melonds, 'src/ARMJIT_Global.cpp',
      '        #else\n'
      '            mprotect(GetAlignedCodeMemoryStart(), CodeMemoryAlignedSize, PROT_EXEC | PROT_READ | PROT_WRITE);\n'
      '        #endif\n',
      '        #elif defined(EMUSUNI_PORT)\n'
      '            CodeMemoryExecutable();\n'
      '        #else\n'
      '            mprotect(GetAlignedCodeMemoryStart(), CodeMemoryAlignedSize, PROT_EXEC | PROT_READ | PROT_WRITE);\n'
      '        #endif\n')
patch(melonds, 'src/ARMJIT_x64/ARMJIT_Compiler.cpp',
      '    FarStart = ResetStart + 1024*1024*24;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the same split as upstream\'s 24 MiB of near code in 32, in a smaller buffer.\n'
      '    FarStart = ResetStart + ARMJIT_Global::CodeMemorySliceSize / 4 * 3;\n'
      '#else\n'
      '    FarStart = ResetStart + 1024*1024*24;\n'
      '#endif\n')
patch(core, 'src/libretro/config/console.cpp',
      '#include <FreeBIOS.h>\n',
      '#include <FreeBIOS.h>\n'
      '#if defined(JIT_ENABLED) && defined(EMUSUNI_PORT)\n#include <ARMJIT_Global.h>\n#endif\n')
patch(core, 'src/libretro/config/console.cpp',
      '#ifdef JIT_ENABLED\n    if (config.JitEnable()) {\n',
      '#ifdef JIT_ENABLED\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the JIT only if its code memory could be made executable; else the interpreter.\n'
      '    if (config.JitEnable() && !melonDS::ARMJIT_Global::CodeMemoryExecutable()) {\n'
      '        retro::error("No executable memory for the JIT: the interpreter runs instead");\n'
      '    }\n'
      '    if (config.JitEnable() && melonDS::ARMJIT_Global::CodeMemoryExecutable()) {\n'
      '#else\n'
      '    if (config.JitEnable()) {\n'
      '#endif\n')

# --- One copy of the ROM (upstream keeps two, the content's and the cart's, and makes a third while
# loading: three or four times the ROM with the frontend's, too much for the PS4 app's 512 MiB heap
# with 128-256 MiB games): the content is borrowed from the frontend while the game loads, then only
# its header is kept; the cart holds the ROM, and a reset moves the cart to the new console instead
# of reading the ROM again. (A DSi console, which installs DSiWare from it, keeps a full copy.)
patch(core, 'src/libretro/retro/info.hpp',
      '        GameInfo(const retro_game_info& info) noexcept;\n',
      '        GameInfo(const retro_game_info& info) noexcept;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: the frontend\'s data borrowed (not copied) when borrow is true, until\n'
      '        // KeepOnlyHeader (a copy of its first bytes, the rest dropped) or KeepAll (a full copy).\n'
      '        GameInfo(const retro_game_info& info, bool borrow) noexcept;\n'
      '        void KeepOnlyHeader(size_t length) noexcept;\n'
      '        void KeepAll() noexcept;\n'
      '#endif\n')
patch(core, 'src/libretro/retro/info.hpp',
      '        std::span<const std::byte> GetData() const noexcept {\n'
      '            return std::span(_data.get(), _size);\n',
      '        std::span<const std::byte> GetData() const noexcept {\n'
      '#ifdef EMUSUNI_PORT\n'
      '            if (_borrowed)\n'
      '                return std::span(_borrowed, _size);\n'
      '#endif\n'
      '            return std::span(_data.get(), _size);\n')
patch(core, 'src/libretro/retro/info.hpp',
      '        std::string _meta;\n    };\n',
      '        std::string _meta;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const std::byte* _borrowed = nullptr;\n'
      '#endif\n'
      '    };\n')
patch(core, 'src/libretro/retro/info.cpp',
      '    if (_data) {\n        memcpy(_data.get(), info.data, info.size);\n    }\n}\n',
      '    if (_data) {\n        memcpy(_data.get(), info.data, info.size);\n    }\n}\n'
      '\n#ifdef EMUSUNI_PORT\n'
      'retro::GameInfo::GameInfo(const retro_game_info& info, bool borrow) noexcept :\n'
      '    _path(info.path ? info.path : ""),\n'
      '    _data(info.data && info.size && !borrow ? std::make_unique<std::byte[]>(info.size) : nullptr),\n'
      '    _size(info.size),\n'
      '    _meta(info.meta ? info.meta : ""),\n'
      '    _borrowed(info.data && info.size && borrow ? static_cast<const std::byte*>(info.data) : nullptr)\n'
      '{\n'
      '    if (_data) {\n        memcpy(_data.get(), info.data, info.size);\n    }\n'
      '}\n\n'
      'void retro::GameInfo::KeepOnlyHeader(size_t length) noexcept {\n'
      '    const std::byte* from = _borrowed ? _borrowed : _data.get();\n'
      '    if (!from || length >= _size) {\n'
      '        KeepAll();\n'
      '        return;\n'
      '    }\n'
      '    auto header = std::make_unique<std::byte[]>(length);\n'
      '    memcpy(header.get(), from, length);\n'
      '    _data = std::move(header);\n'
      '    _size = length;\n'
      '    _borrowed = nullptr;\n'
      '}\n\n'
      'void retro::GameInfo::KeepAll() noexcept {\n'
      '    if (!_borrowed)\n'
      '        return;\n'
      '    _data = std::make_unique<std::byte[]>(_size);\n'
      '    memcpy(_data.get(), _borrowed, _size);\n'
      '    _borrowed = nullptr;\n'
      '}\n'
      '#endif\n')
patch(core, 'src/libretro/core/core.cpp',
      '                _ndsInfo = game[0];\n',
      '#ifdef EMUSUNI_PORT\n'
      '                _ndsInfo.emplace(game[0], true); // EmuSuNi: borrowed until the console has its cart\n'
      '#else\n'
      '                _ndsInfo = game[0];\n'
      '#endif\n')
patch(core, 'src/libretro/core/core.cpp',
      '    melonDS::NDS::Current = Console.get();\n    RegisterMemoryMap();\n\n'
      '    if (melonDS::NDSCart::CartCommon* cart = Console->GetNDSCart()) {\n',
      '    melonDS::NDS::Current = Console.get();\n    RegisterMemoryMap();\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (_ndsInfo) {\n'
      '        // EmuSuNi: the cart has its own copy of the ROM now (the frontend may free its own).\n'
      '        if (static_cast<ConsoleType>(Console->ConsoleType) == ConsoleType::DSi)\n'
      '            _ndsInfo->KeepAll();\n'
      '        else\n'
      '            _ndsInfo->KeepOnlyHeader(sizeof(melonDS::NDSHeader));\n'
      '    }\n'
      '#endif\n\n'
      '    if (melonDS::NDSCart::CartCommon* cart = Console->GetNDSCart()) {\n')
patch(core, 'src/libretro/core/core.cpp',
      'catch (const config_exception& e) {\n    retro::error("{}", e.what());\n\n    return InitErrorScreen(e);\n}\n',
      'catch (const config_exception& e) {\n    retro::error("{}", e.what());\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (_ndsInfo)\n'
      '        _ndsInfo->KeepOnlyHeader(sizeof(melonDS::NDSHeader)); // EmuSuNi: nothing borrowed past the load\n'
      '#endif\n'
      '    return InitErrorScreen(e);\n}\n')
patch(core, 'src/libretro/core/core.cpp',
      '    Console = nullptr;\n    melonDS::NDS::Current = nullptr;\n    Console = CreateConsole(\n'
      '        *this,\n        Config,\n        _ndsInfo ? &*_ndsInfo : nullptr,\n'
      '        _gbaInfo ? &*_gbaInfo : nullptr,\n        _gbaSaveInfo ? &*_gbaSaveInfo : nullptr\n    );\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the cart (the one full copy of the ROM) moves to the new console.\n'
      '    std::unique_ptr<melonDS::NDSCart::CartCommon> ndsCart = Console->EjectCart();\n'
      '#endif\n'
      '    Console = nullptr;\n    melonDS::NDS::Current = nullptr;\n    Console = CreateConsole(\n'
      '        *this,\n        Config,\n        _ndsInfo ? &*_ndsInfo : nullptr,\n'
      '        _gbaInfo ? &*_gbaInfo : nullptr,\n        _gbaSaveInfo ? &*_gbaSaveInfo : nullptr\n'
      '#ifdef EMUSUNI_PORT\n'
      '        , std::move(ndsCart)\n'
      '#endif\n'
      '    );\n')
patch(core, 'src/libretro/config/console.hpp',
      'namespace melonDS {\n    class NDS;\n    struct NDSHeader;\n}\n',
      'namespace melonDS {\n    class NDS;\n    struct NDSHeader;\n'
      '#ifdef EMUSUNI_PORT\n    namespace NDSCart { class CartCommon; }\n#endif\n'
      '}\n'
      '#ifdef EMUSUNI_PORT\n#include <cstdint>\n#endif\n')
patch(core, 'src/libretro/config/console.hpp',
      '    std::unique_ptr<melonDS::NDS> CreateConsole(\n        CoreState& state,\n        const CoreConfig& config,\n'
      '        const retro::GameInfo* ndsInfo,\n        const retro::GameInfo* gbaInfo,\n'
      '        const retro::GameInfo* gbaSaveInfo\n    );\n',
      '    std::unique_ptr<melonDS::NDS> CreateConsole(\n        CoreState& state,\n        const CoreConfig& config,\n'
      '        const retro::GameInfo* ndsInfo,\n        const retro::GameInfo* gbaInfo,\n'
      '        const retro::GameInfo* gbaSaveInfo\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: a cart to insert (a reset\'s) instead of one read from ndsInfo.\n'
      '        , std::unique_ptr<melonDS::NDSCart::CartCommon> ndsCart = nullptr\n'
      '#endif\n'
      '    );\n')
patch(core, 'src/libretro/config/console.cpp',
      '        const retro::GameInfo* gbaSaveInfo,\n        CoreState& state\n    );\n',
      '        const retro::GameInfo* gbaSaveInfo,\n        CoreState& state\n'
      '#ifdef EMUSUNI_PORT\n        , std::unique_ptr<melonDS::NDSCart::CartCommon> ndsCart\n#endif\n'
      '    );\n')
patch(core, 'src/libretro/config/console.cpp',
      'std::unique_ptr<melonDS::NDS> MelonDsDs::CreateConsole(\n    CoreState& state,\n    const CoreConfig& config,\n'
      '    const retro::GameInfo* ndsInfo,\n    const retro::GameInfo* gbaInfo,\n    const retro::GameInfo* gbaSaveInfo\n) {\n',
      'std::unique_ptr<melonDS::NDS> MelonDsDs::CreateConsole(\n    CoreState& state,\n    const CoreConfig& config,\n'
      '    const retro::GameInfo* ndsInfo,\n    const retro::GameInfo* gbaInfo,\n    const retro::GameInfo* gbaSaveInfo\n'
      '#ifdef EMUSUNI_PORT\n    , std::unique_ptr<melonDS::NDSCart::CartCommon> ndsCart\n#endif\n'
      ') {\n')
patch(core, 'src/libretro/config/console.cpp',
      '        NDSArgs args = GetNdsArgs(config, ndsInfo, gbaInfo, gbaSaveInfo, state);\n',
      '#ifdef EMUSUNI_PORT\n'
      '        NDSArgs args = GetNdsArgs(config, ndsInfo, gbaInfo, gbaSaveInfo, state, std::move(ndsCart));\n'
      '#else\n'
      '        NDSArgs args = GetNdsArgs(config, ndsInfo, gbaInfo, gbaSaveInfo, state);\n'
      '#endif\n')
patch(core, 'src/libretro/config/console.cpp',
      'static MelonDsDs::NDSArgs MelonDsDs::GetNdsArgs(\n    const CoreConfig& config,\n    const retro::GameInfo* ndsInfo,\n'
      '    const retro::GameInfo* gbaInfo,\n    const retro::GameInfo* gbaSaveInfo,\n    CoreState& state\n) {\n',
      'static MelonDsDs::NDSArgs MelonDsDs::GetNdsArgs(\n    const CoreConfig& config,\n    const retro::GameInfo* ndsInfo,\n'
      '    const retro::GameInfo* gbaInfo,\n    const retro::GameInfo* gbaSaveInfo,\n    CoreState& state\n'
      '#ifdef EMUSUNI_PORT\n    , std::unique_ptr<melonDS::NDSCart::CartCommon> ndsCart\n#endif\n'
      ') {\n')
patch(core, 'src/libretro/config/console.cpp',
      '        ndsargs.ndsCart = LoadNdsCart(config, *ndsInfo);\n',
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: a reset\'s cart, else the ROM read from the content (only while it loads).\n'
      '        ndsargs.ndsCart = ndsCart ? std::move(ndsCart) : LoadNdsCart(config, *ndsInfo);\n'
      '#else\n'
      '        ndsargs.ndsCart = LoadNdsCart(config, *ndsInfo);\n'
      '#endif\n')

# --- The ROM from its path (retro_game_info with a path, no data): read with stdio straight into the
# cart's buffer, the one allocation (a power of two, zero-filled past the file, as melonDS pads a
# ROM given as data, so the cart takes it as it is). Any failure (can't open or read it, not a DS
# ROM's size, no memory) is a content error: retro_load_game returns false. A ROM given as data
# works as before.
patch(core, 'src/libretro/core/core.hpp',
      '        std::optional<retro::GameInfo> _ndsInfo = std::nullopt;\n',
      '        std::optional<retro::GameInfo> _ndsInfo = std::nullopt;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: a ROM read from its path, until LoadGame makes the cart of it.\n'
      '        std::unique_ptr<uint8_t[]> _ndsRomFromPath;\n'
      '        uint32_t _ndsRomFromPathLength = 0;\n'
      '#endif\n')
patch(core, 'src/libretro/core/core.cpp',
      'void MelonDsDs::CoreState::InitContent(unsigned type, std::span<const retro_game_info> game) {\n'
      '    ZoneScopedN(TracyFunction);\n',
      '#ifdef EMUSUNI_PORT\n'
      '#include <algorithm>\n#include <cerrno>\n#include <cstdio>\n#include <cstring>\n#include <new>\n\n'
      '// EmuSuNi: the ROM at path in one buffer of a power-of-two size, zero-filled past the end of the\n'
      '// file (what melonDS makes of a ROM given as data); its size in padded.\n'
      'static std::unique_ptr<uint8_t[]> EmuSuNiReadRom(const char* path, uint32_t& padded) {\n'
      '    FILE* file = fopen(path, "rb");\n'
      '    if (!file)\n'
      '        throw MelonDsDs::content_exception(fmt::format("Can\'t open the ROM \\"{}\\": {}", path, strerror(errno)));\n'
      '    long size = -1;\n'
      '    if (fseek(file, 0, SEEK_END) == 0)\n'
      '        size = ftell(file);\n'
      '    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {\n'
      '        fclose(file);\n'
      '        throw MelonDsDs::content_exception(fmt::format("Can\'t read the ROM \\"{}\\"", path));\n'
      '    }\n'
      '    if (size < (long)std::max<size_t>(0x1000, sizeof(melonDS::NDSHeader)) || size > 512L * 1024 * 1024) {\n'
      '        fclose(file);\n'
      '        throw MelonDsDs::content_exception(fmt::format("\\"{}\\" ({} bytes) isn\'t a Nintendo DS ROM", path, size));\n'
      '    }\n'
      '    const uint32_t length = static_cast<uint32_t>(size);\n'
      '    padded = 1;\n'
      '    while (padded < length)\n'
      '        padded <<= 1;\n'
      '    std::unique_ptr<uint8_t[]> rom(new (std::nothrow) uint8_t[padded]);\n'
      '    if (!rom) {\n'
      '        fclose(file);\n'
      '        throw MelonDsDs::content_exception(fmt::format("Not enough memory for the ROM ({} MiB)", padded >> 20));\n'
      '    }\n'
      '    size_t done = 0;\n'
      '    while (done < length) {\n'
      '        const size_t got = fread(rom.get() + done, 1, length - done, file);\n'
      '        if (got == 0)\n'
      '            break;\n'
      '        done += got;\n'
      '    }\n'
      '    fclose(file);\n'
      '    if (done != length)\n'
      '        throw MelonDsDs::content_exception(fmt::format("Read only {} of the {} bytes of \\"{}\\"", done, length, path));\n'
      '    memset(rom.get() + length, 0, padded - length);\n'
      '    retro::info("Read the {}-byte ROM \\"{}\\" into a {}-byte buffer", length, path, padded);\n'
      '    return rom;\n'
      '}\n'
      '#endif\n\n'
      'void MelonDsDs::CoreState::InitContent(unsigned type, std::span<const retro_game_info> game) {\n'
      '    ZoneScopedN(TracyFunction);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    _ndsRomFromPath.reset(); // EmuSuNi: nothing left from an earlier load that failed\n'
      '    _ndsRomFromPathLength = 0;\n'
      '#endif\n')
patch(core, 'src/libretro/core/core.cpp',
      '                    throw content_exception("Loaded a save file instead of a ROM, ensure that you opened the right file.");\n'
      '                }\n\n'
      '                if (game[0].size == 0) {\n',
      '                    throw content_exception("Loaded a save file instead of a ROM, ensure that you opened the right file.");\n'
      '                }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '                if (game[0].data == nullptr && game[0].size == 0 && game[0].path && game[0].path[0]) {\n'
      '                    // EmuSuNi: the ROM from its path; of the content only the header (and the\n'
      '                    // path) is kept, as for data once the console has its cart (LoadGame).\n'
      '                    _ndsRomFromPath = EmuSuNiReadRom(game[0].path, _ndsRomFromPathLength);\n'
      '                    retro_game_info header = game[0];\n'
      '                    header.data = _ndsRomFromPath.get();\n'
      '                    header.size = sizeof(melonDS::NDSHeader);\n'
      '                    _ndsInfo.emplace(header, false);\n'
      '                    break;\n'
      '                }\n'
      '#endif\n\n'
      '                if (game[0].size == 0) {\n')
patch(core, 'src/libretro/core/core.cpp',
      '    retro_assert(Console == nullptr);\n'
      '    // Instantiates the console with games and save data installed\n'
      '    Console = CreateConsole(\n        *this,\n        Config,\n        _ndsInfo ? &*_ndsInfo : nullptr,\n'
      '        _gbaInfo ? &*_gbaInfo : nullptr,\n        _gbaSaveInfo ? &*_gbaSaveInfo : nullptr\n    );\n',
      '    retro_assert(Console == nullptr);\n'
      '    // Instantiates the console with games and save data installed\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a ROM read from its path becomes the cart here (its only copy).\n'
      '    std::unique_ptr<melonDS::NDSCart::CartCommon> ndsCart;\n'
      '    if (_ndsRomFromPath && _ndsInfo)\n'
      '        ndsCart = LoadNdsCart(Config, *_ndsInfo, std::move(_ndsRomFromPath), _ndsRomFromPathLength);\n'
      '#endif\n'
      '    Console = CreateConsole(\n        *this,\n        Config,\n        _ndsInfo ? &*_ndsInfo : nullptr,\n'
      '        _gbaInfo ? &*_gbaInfo : nullptr,\n        _gbaSaveInfo ? &*_gbaSaveInfo : nullptr\n'
      '#ifdef EMUSUNI_PORT\n'
      '        , std::move(ndsCart)\n'
      '#endif\n'
      '    );\n')
patch(core, 'src/libretro/config/console.hpp',
      '    /// Modify a console instance with core options that are safe to adjust at runtime.\n',
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: the cart of a ROM read from its path: the checks of a ROM given as data, then the\n'
      '    /// cart takes rom (romLength bytes, a power of two) as it is.\n'
      '    std::unique_ptr<melonDS::NDSCart::CartCommon> LoadNdsCart(\n'
      '        const CoreConfig& config,\n'
      '        const retro::GameInfo& ndsInfo,\n'
      '        std::unique_ptr<uint8_t[]>&& rom,\n'
      '        uint32_t romLength\n'
      '    );\n\n'
      '#endif\n'
      '    /// Modify a console instance with core options that are safe to adjust at runtime.\n')
patch(core, 'src/libretro/config/console.cpp',
      '    retro::debug("Parsed NDS ROM: \\"{}\\"", ndsInfo.GetPath());\n\n    return cart;\n}\n',
      '    retro::debug("Parsed NDS ROM: \\"{}\\"", ndsInfo.GetPath());\n\n    return cart;\n}\n'
      '\n#ifdef EMUSUNI_PORT\n'
      'std::unique_ptr<melonDS::NDSCart::CartCommon> MelonDsDs::LoadNdsCart(\n'
      '    const CoreConfig& config,\n'
      '    const retro::GameInfo& ndsInfo,\n'
      '    std::unique_ptr<uint8_t[]>&& rom,\n'
      '    uint32_t romLength\n'
      ') {\n'
      '    ZoneScopedN(TracyFunction);\n'
      '    if (!rom || romLength < sizeof(NDSHeader)) {\n'
      '        throw invalid_rom_exception("ROM is too small to be valid.");\n'
      '    }\n\n'
      '    const NDSHeader &header = *reinterpret_cast<const NDSHeader*>(rom.get());\n\n'
      '    // The same checks as LoadNdsCart\'s for a ROM given as data\n'
      '    if (header.ARM9EntryAddress < 0x2000000 || header.ARM9EntryAddress > 0x23BFE00) {\n'
      '        retro::error("Expected ARM9 entry address between 0x2000000 and 0x23BFE00, got 0x{:08x}", header.ARM9EntryAddress);\n'
      '        throw invalid_rom_exception("ROM isn\'t valid, did you select the right file?");\n'
      '    }\n\n'
      '    if (header.NintendoLogoCRC16 != 0xCF56 && !header.IsHomebrew()) {\n'
      '        retro::error("Expected logo CRC16 of 0xCF56, got 0x{:04x}", header.NintendoLogoCRC16);\n'
      '        throw invalid_rom_exception("ROM isn\'t valid, did you select the right file?");\n'
      '    }\n\n'
      '    melonDS::NDSCart::NDSCartArgs sdargs = {\n'
      '        .SDCard = config.DldiSdCardArgs(),\n'
      '        .SRAM = nullptr, // SRAM is loaded separately by retro_get_memory\n'
      '        .SRAMLength = 0,\n'
      '    };\n\n'
      '    std::unique_ptr<melonDS::NDSCart::CartCommon> cart =\n'
      '        melonDS::NDSCart::ParseROM(std::move(rom), romLength, nullptr, std::move(sdargs));\n'
      '    if (!cart) {\n'
      '        throw invalid_rom_exception("Failed to parse the DS ROM image. Is it valid?");\n'
      '    }\n\n'
      '    retro::debug("Parsed NDS ROM read from \\"{}\\"", ndsInfo.GetPath());\n\n'
      '    return cart;\n'
      '}\n'
      '#endif\n')
patch(core, 'src/libretro/config/console.cpp',
      '    if (type == ConsoleType::DSi) {\n        // If we\'re in DSi mode...\n',
      '    if (type == ConsoleType::DSi) {\n        // If we\'re in DSi mode...\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (ndsInfo && ndsInfo->GetData().size() <= sizeof(melonDS::NDSHeader)) {\n'
      '            // EmuSuNi: a DSi console reads (and installs) the whole ROM from the content, of which\n'
      '            // only the header is kept (a ROM read from its path, or a DS session reset into DSi mode).\n'
      '            throw content_exception("DSi mode needs the whole ROM as data.");\n'
      '        }\n'
      '#endif\n')

# --- melonDS's thread_local console pointer: constant-initialised, said where it's declared, so the
# files using it don't keep a weak reference to an initialiser (the app would export it unresolved,
# which the PS4's loader may refuse).
patch(melonds, 'src/NDS.h',
      '    static thread_local NDS* Current;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    __attribute__((require_constant_initialization)) static thread_local NDS* Current;\n'
      '#else\n'
      '    static thread_local NDS* Current;\n'
      '#endif\n')

# --- No fastmem: no shared memory object, no 8 GiB reservation, no fault handler. RAM from the heap.
patch(melonds, 'src/ARMJIT_Memory.cpp',
      'bool ARMJIT_Memory::IsFastMemSupported()\n{\n#if defined(__APPLE__) || defined(__OpenBSD__)\n',
      'bool ARMJIT_Memory::IsFastMemSupported()\n{\n'
      '#if defined(__APPLE__) || defined(__OpenBSD__) || defined(EMUSUNI_PORT)\n'
      '    // EmuSuNi: no fastmem (page faults, which the PS4 app keeps for its crash log).\n')
patch(melonds, 'src/ARMJIT_Memory.cpp',
      '#else\n    struct sigaction sa;\n    sa.sa_handler = nullptr;\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '    // EmuSuNi: no SIGSEGV handler of its own (only fastmem needs one).\n'
      '#else\n    struct sigaction sa;\n    sa.sa_handler = nullptr;\n')
patch(melonds, 'src/ARMJIT_Memory.cpp',
      '#else\n    sigaction(SIGSEGV, &OldSaSegv, nullptr);\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '#else\n    sigaction(SIGSEGV, &OldSaSegv, nullptr);\n')
patch(melonds, 'src/ARMJIT_Memory.cpp',
      '#else\n    MemoryBase = (u8*)mmap(nullptr, VirtmemAreaSize, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '    // EmuSuNi: the emulated RAM from the heap, as on old Windows: no shared memory object (the PS4\n'
      '    // has none the normal way), no 8 GiB of address space; only fastmem would need them.\n'
      '    MemoryBase = new u8[MemoryTotalSize]();\n'
      '#else\n    MemoryBase = (u8*)mmap(nullptr, VirtmemAreaSize, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);\n')
patch(melonds, 'src/ARMJIT_Memory.cpp',
      '#else\n    if (MemoryBase)\n    {\n        munmap(MemoryBase, VirtmemAreaSize);\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '    delete[] MemoryBase;\n    MemoryBase = nullptr;\n    FastMem9Start = nullptr;\n    FastMem7Start = nullptr;\n'
      '#else\n    if (MemoryBase)\n    {\n        munmap(MemoryBase, VirtmemAreaSize);\n')

# --- Both CPUs halted (waiting for an interrupt: 60% of NDS::RunFrame's 64-cycle turns in a Mario
# Kart DS race), no DMA, the 3D engine idle: the turns up to the next thing that happens (an event, a
# timer overflowing) taken at once, with the same result as one turn after another, in which nothing
# changes but the time (the next event's turn, and a timer's overflow, at the same cycle as before).
patch(melonds, 'src/NDS.h',
      '    u64 NextTarget();\n',
      '    u64 NextTarget();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    u64 EmuSuNiIdleTarget(u64 target);\n'
      '#endif\n')
patch(melonds, 'src/NDS.cpp',
      'u64 NDS::NextTargetSleep()\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: target is NextTarget()\'s, a full turn (no event in it). With both CPUs halted (and no\n'
      '// interrupt for them), no DMA and the 3D engine idle, every turn until the next event\'s only moves the\n'
      '// time on, unless a timer overflows (an interrupt, maybe): those turns at once, up to the one with the\n'
      '// event or the overflow, which then runs as before.\n'
      'u64 NDS::EmuSuNiIdleTarget(u64 target)\n'
      '{\n'
      '    if (CPUStop || ConsoleType != 0 || ARM9.Halted != 1 || ARM7.Halted != 1\n'
      '        || HaltInterrupted(0) || HaltInterrupted(1))\n'
      '        return target;\n'
      '    if (GPU.GPU3D.GeometryEnabled && !GPU.GPU3D.FlushRequest\n'
      '        && (!GPU.GPU3D.CmdPIPE.IsEmpty() || (GPU.GPU3D.GXStat & (1<<27))))\n'
      '        return target;\n\n'
      '    u64 minEvent = UINT64_MAX;\n'
      '    u32 mask = SchedListMask;\n'
      '    for (int i = 0; i < Event_MAX; i++)\n'
      '    {\n'
      '        if (!mask) break;\n'
      '        if ((mask & 0x1) && SchedList[i].Timestamp < minEvent)\n'
      '            minEvent = SchedList[i].Timestamp;\n'
      '        mask >>= 1;\n'
      '    }\n'
      '    const u64 start = SysTimestamp;\n'
      '    if (minEvent < start + kMaxIterationCycles + kIterationCycleMargin)\n'
      '        return target;\n'
      '    // NextTarget gives full turns while the event is at least the margin past their end\n'
      '    u64 turns = (minEvent - kIterationCycleMargin - start) / kMaxIterationCycles;\n'
      '    // a timer overflows in the first turn that ends at or after its overflow (RunTimers)\n'
      '    for (u32 cpu = 0; cpu < 2; cpu++)\n'
      '    {\n'
      '        for (u32 t = 0; t < 4; t++)\n'
      '        {\n'
      '            if (!(TimerCheckMask[cpu] & (1 << t)))\n'
      '                continue;\n'
      '            const Timer& timer = Timers[(cpu << 2) + t];\n'
      '            const u64 needed = (((u64)1 << 26) - timer.Counter + ((u64)1 << timer.CycleShift) - 1) >> timer.CycleShift;\n'
      '            const u64 at = TimerTimestamp[cpu] + needed;\n'
      '            const u64 turn = at <= start ? 1 : (at - start + kMaxIterationCycles - 1) / kMaxIterationCycles;\n'
      '            if (turn < turns)\n'
      '                turns = turn;\n'
      '        }\n'
      '    }\n'
      '    if (turns <= 1)\n'
      '        return target;\n'
      '    return start + turns * kMaxIterationCycles;\n'
      '}\n'
      '#endif\n\n'
      'u64 NDS::NextTargetSleep()\n')
patch(melonds, 'src/NDS.cpp',
      '                u64 target = NextTarget();\n                ARM9Target = target << ARM9ClockShift;\n',
      '                u64 target = NextTarget();\n'
      '#ifdef EMUSUNI_PORT\n'
      '                if (target == SysTimestamp + kMaxIterationCycles)\n'
      '                    target = EmuSuNiIdleTarget(target); // EmuSuNi: the idle turns at once\n'
      '#endif\n'
      '                ARM9Target = target << ARM9ClockShift;\n')

# --- The JIT's memory accesses that fastmem would do (main RAM, on a DS): straight from the ARM9's
# slow path, without the thread_local console pointer and NDS::ARM9Read/Write's virtual call and switch
# (what those do for main RAM, the code there invalidated as before).
patch(melonds, 'src/ARMJIT.cpp',
      '    else if ((addr & cpu->DTCMMask) == cpu->DTCMBase)\n'
      '        val = *(T*)&cpu->DTCM[addr & 0x3FFF];\n'
      '    else if (std::is_same<T, u32>::value)\n'
      '        val = NDS::Current->ARM9Read32(addr);\n',
      '    else if ((addr & cpu->DTCMMask) == cpu->DTCMBase)\n'
      '        val = *(T*)&cpu->DTCM[addr & 0x3FFF];\n'
      '#ifdef EMUSUNI_PORT\n'
      '    else if (ConsoleType == 0 && (addr & 0xFF000000) == 0x02000000)\n'
      '        val = *(T*)&cpu->NDS.MainRAM[addr & cpu->NDS.MainRAMMask]; // EmuSuNi: main RAM, at once\n'
      '#endif\n'
      '    else if (std::is_same<T, u32>::value)\n'
      '        val = NDS::Current->ARM9Read32(addr);\n')
patch(melonds, 'src/ARMJIT.cpp',
      '    else if ((addr & cpu->DTCMMask) == cpu->DTCMBase)\n'
      '    {\n'
      '        *(T*)&cpu->DTCM[addr & 0x3FFF] = val;\n'
      '    }\n',
      '    else if ((addr & cpu->DTCMMask) == cpu->DTCMBase)\n'
      '    {\n'
      '        *(T*)&cpu->DTCM[addr & 0x3FFF] = val;\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    else if (ConsoleType == 0 && (addr & 0xFF000000) == 0x02000000)\n'
      '    {\n'
      '        // EmuSuNi: main RAM, at once\n'
      '        cpu->NDS.JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_MainRAM>(addr);\n'
      '        *(T*)&cpu->NDS.MainRAM[addr & cpu->NDS.MainRAMMask] = val;\n'
      '    }\n'
      '#endif\n')

# --- The 2D's affine/extended BGs with extended palettes: the palette's address worked out in the
# loop instead of a call to GPU2D::GetBGExtPal (in another file) for every pixel.
patch(melonds, 'src/GPU2D_Soft.cpp',
      '        u16 curtile;\n'
      '        u16* curpal;\n'
      '        u8 color;\n\n'
      '        yshift -= 3;\n\n',
      '        u16 curtile;\n'
      '        u16* curpal;\n'
      '        u8 color;\n\n'
      '        yshift -= 3;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: the slot\'s first palette (each one 256 colours after the last)\n'
      '        u16* const extpal0 = extpal ? GPU2D.GetBGExtPal(bgnum, 0) : nullptr;\n'
      '#endif\n')
patch(melonds, 'src/GPU2D_Soft.cpp',
      '                    if (extpal) curpal = GPU2D.GetBGExtPal(bgnum, curtile>>12);\n'
      '                    else        curpal = pal;\n\n'
      '                    // draw pixel\n'
      '                    u32 tilexoff = (finalX >> 8) & 0x7;\n',
      '#ifdef EMUSUNI_PORT\n'
      '                    if (extpal) curpal = extpal0 + ((curtile >> 12) << 8);\n'
      '#else\n'
      '                    if (extpal) curpal = GPU2D.GetBGExtPal(bgnum, curtile>>12);\n'
      '#endif\n'
      '                    else        curpal = pal;\n\n'
      '                    // draw pixel\n'
      '                    u32 tilexoff = (finalX >> 8) & 0x7;\n')

# --- Timing: where the time goes, one line in the log every ~10 s (cheap: the TSC, read a thousand
# times a frame). The emulation thread's parts (2D, waiting for 3D, sound...), the 3D threads' work.
patch(melonds, 'src/GPU.h',
      '#include <memory>\n',
      '#include <memory>\n#ifdef EMUSUNI_PORT\n#include <atomic>\n#endif\n')
patch(melonds, 'src/GPU.h',
      'namespace melonDS\n{\nclass GPU3D;\nclass ARMJIT;\n',
      'namespace melonDS\n{\nclass GPU3D;\nclass ARMJIT;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: where the time goes, in TSC ticks summed since melonDS DS last logged them (its Run, every\n'
      '// ~10 s, which then zeroes them): the emulation thread\'s (only it writes Ticks) and the 3D threads\'.\n'
      'namespace EmuSuNiTiming\n'
      '{\n'
      'enum Slot { Frame, RunFrame, Screens, Video, Audio, Draw2D, Wait2D, Wait2DOther, Wait3DLine, Wait3DFrame, Start3D,\n'
      '            Mix, ARM9, ARM7, GX, System, Idle, NumSlots };\n'
      'extern u64 Ticks[NumSlots];\n'
      'extern u64 Turns, IdleTurns; // (the detail\'s)\n'
      'extern std::atomic<u64> Render3D[2], Help2D, Polygons, Frames3D;\n'
      'static inline u64 Now() { return __builtin_ia32_rdtsc(); }\n'
      '}\n'
      '// EmuSuNi: a line of the sub screen\'s 2D given to the helper thread and not waited for yet (the\n'
      '// emulation thread\'s; GPU_Soft.cpp): what it reads is written only once it\'s drawn.\n'
      'extern bool EmuSuNi2DPending;\n'
      'void EmuSuNi2DSyncPending();\n'
      '// EmuSuNi: the frame being emulated won\'t be shown (the frontend\'s frame skip): no 2D drawn\n'
      '// (GPU_Soft.cpp), set by melonDS DS\'s Run.\n'
      'extern bool EmuSuNiSkipDraw;\n'
      '// The CPUs\' share, the 3D commands\', the events\' (NDS::RunFrame): only for the PC tests\n'
      '// (NDS_CORE_TIMING_DETAIL=1), four TSC reads per turn of its loop.\n'
      '#ifdef EMUSUNI_TIMING_DETAIL\n'
      '#define EMUSUNI_DETAIL_START(v) const u64 v = EmuSuNiTiming::Now()\n'
      '#define EMUSUNI_DETAIL_END(slot, v) EmuSuNiTiming::Ticks[EmuSuNiTiming::slot] += EmuSuNiTiming::Now() - v\n'
      '#else\n'
      '#define EMUSUNI_DETAIL_START(v)\n'
      '#define EMUSUNI_DETAIL_END(slot, v)\n'
      '#endif\n'
      '#endif\n')
patch(melonds, 'src/GPU.cpp',
      'namespace melonDS\n{\nusing Platform::Log;\n',
      'namespace melonDS\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      'namespace EmuSuNiTiming\n{\nu64 Ticks[NumSlots];\nu64 Turns, IdleTurns;\nstd::atomic<u64> Render3D[2], Help2D, Polygons, Frames3D;\n}\n'
      'bool EmuSuNiSkipDraw = false;\n'
      '#endif\n'
      'using Platform::Log;\n')
patch(melonds, 'src/GPU.cpp',
      '        if (line < 192)\n            Rend->DrawScanline(line);\n        if (line < 191)\n            Rend->DrawSprites(line+1);\n',
      '#ifdef EMUSUNI_PORT\n'
      '        const u64 emusuniT0 = EmuSuNiTiming::Now();\n'
      '#endif\n'
      '        if (line < 192)\n            Rend->DrawScanline(line);\n        if (line < 191)\n            Rend->DrawSprites(line+1);\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNiTiming::Ticks[EmuSuNiTiming::Draw2D] += EmuSuNiTiming::Now() - emusuniT0;\n'
      '#endif\n')
patch(melonds, 'src/GPU.cpp',
      '        Rend->Start3DRendering();\n',
      '#ifdef EMUSUNI_PORT\n'
      '        const u64 emusuniT0 = EmuSuNiTiming::Now();\n'
      '        Rend->Start3DRendering();\n'
      '        EmuSuNiTiming::Ticks[EmuSuNiTiming::Start3D] += EmuSuNiTiming::Now() - emusuniT0;\n'
      '#else\n'
      '        Rend->Start3DRendering();\n'
      '#endif\n')
patch(melonds, 'src/GPU.cpp',
      '        // sprites are pre-rendered one scanline in advance\n        Rend->DrawSprites(0);\n',
      '        // sprites are pre-rendered one scanline in advance\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const u64 emusuniT0 = EmuSuNiTiming::Now();\n'
      '        Rend->DrawSprites(0);\n'
      '        EmuSuNiTiming::Ticks[EmuSuNiTiming::Draw2D] += EmuSuNiTiming::Now() - emusuniT0;\n'
      '#else\n'
      '        Rend->DrawSprites(0);\n'
      '#endif\n')
patch(melonds, 'src/GPU.cpp',
      '        Rend->Finish3DRendering();\n',
      '#ifdef EMUSUNI_PORT\n'
      '        const u64 emusuniT0 = EmuSuNiTiming::Now();\n'
      '        Rend->Finish3DRendering();\n'
      '        EmuSuNiTiming::Ticks[EmuSuNiTiming::Wait3DFrame] += EmuSuNiTiming::Now() - emusuniT0;\n'
      '#else\n'
      '        Rend->Finish3DRendering();\n'
      '#endif\n')
patch(melonds, 'src/SPU.h',
      '    void Mix(u32 spucycles);\n',
      '    void Mix(u32 spucycles);\n#ifdef EMUSUNI_PORT\n    void MixTimed(u32 spucycles);\n#endif\n')
patch(melonds, 'src/SPU.cpp',
      'void SPU::Mix(u32 spucycles)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'void SPU::Mix(u32 spucycles)\n'
      '{\n'
      '    // EmuSuNi: timed (the sound\'s share of the emulation thread)\n'
      '    const u64 t0 = EmuSuNiTiming::Now();\n'
      '    MixTimed(spucycles);\n'
      '    EmuSuNiTiming::Ticks[EmuSuNiTiming::Mix] += EmuSuNiTiming::Now() - t0;\n'
      '}\n\n'
      'void SPU::MixTimed(u32 spucycles)\n'
      '#else\n'
      'void SPU::Mix(u32 spucycles)\n'
      '#endif\n'
      '{\n')
patch(melonds, 'src/NDS.cpp',
      '                CurCPU = 0;\n\n                if (CPUStop & CPUStop_GXStall)\n',
      '                CurCPU = 0;\n'
      '#ifdef EMUSUNI_PORT\n                EMUSUNI_DETAIL_START(emusuniT9);\n'
      '#ifdef EMUSUNI_TIMING_DETAIL\n'
      '                // the turns of this loop, and those with both CPUs halted (waiting for an interrupt)\n'
      '                const bool emusuniIdle = ARM9.Halted && ARM7.Halted;\n'
      '                EmuSuNiTiming::Turns++;\n'
      '#endif\n'
      '#endif\n\n'
      '                if (CPUStop & CPUStop_GXStall)\n')
patch(melonds, 'src/NDS.cpp',
      '                RunTimers(0);\n                GPU.GPU3D.Run();\n',
      '                RunTimers(0);\n'
      '#ifdef EMUSUNI_PORT\n'
      '                EMUSUNI_DETAIL_END(ARM9, emusuniT9);\n'
      '                EMUSUNI_DETAIL_START(emusuniTG);\n'
      '                GPU.GPU3D.Run();\n'
      '                EMUSUNI_DETAIL_END(GX, emusuniTG);\n'
      '                EMUSUNI_DETAIL_START(emusuniT7);\n'
      '#else\n'
      '                GPU.GPU3D.Run();\n'
      '#endif\n')
patch(melonds, 'src/NDS.cpp',
      '                    RunTimers(1);\n                }\n\n                RunSystem(target);\n',
      '                    RunTimers(1);\n                }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '                EMUSUNI_DETAIL_END(ARM7, emusuniT7);\n'
      '                EMUSUNI_DETAIL_START(emusuniTS);\n'
      '                RunSystem(target);\n'
      '                EMUSUNI_DETAIL_END(System, emusuniTS);\n'
      '#ifdef EMUSUNI_TIMING_DETAIL\n'
      '                if (emusuniIdle)\n'
      '                {\n'
      '                    EmuSuNiTiming::IdleTurns++;\n'
      '                    EMUSUNI_DETAIL_END(Idle, emusuniT9);\n'
      '                }\n'
      '#endif\n'
      '#else\n'
      '                RunSystem(target);\n'
      '#endif\n')
patch(core, 'src/libretro/environment.cpp',
      'void retro::video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {\n'
      '    ZoneScopedN(TracyFunction);\n'
      '    if (_video_refresh) {\n'
      '        _video_refresh(data, width, height, pitch);\n'
      '    }\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n#include <GPU.h>\n#endif\n'
      'void retro::video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {\n'
      '    ZoneScopedN(TracyFunction);\n'
      '    if (_video_refresh) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const uint64_t t0 = melonDS::EmuSuNiTiming::Now(); // EmuSuNi: the frontend\'s time, timed\n'
      '        _video_refresh(data, width, height, pitch);\n'
      '        melonDS::EmuSuNiTiming::Ticks[melonDS::EmuSuNiTiming::Video] += melonDS::EmuSuNiTiming::Now() - t0;\n'
      '#else\n'
      '        _video_refresh(data, width, height, pitch);\n'
      '#endif\n'
      '    }\n'
      '}\n')
patch(core, 'src/libretro/core/core.cpp',
      'void MelonDsDs::CoreState::Run() noexcept {\n    ZoneScopedN(TracyFunction);\n',
      '#ifdef EMUSUNI_PORT\n'
      '#include <chrono>\n'
      '#include <cstdlib>\n'
      '#include <GPU.h>\n\n'
      '// EmuSuNi: every retro_run timed, and where its time went (melonDS\'s EmuSuNiTiming) logged every\n'
      '// ~10 s (or every NDS_TIMING_FRAMES frames, the PC tests\' environment variable).\n'
      'namespace {\n'
      'struct EmuSuNiRunTimer {\n'
      '    const uint64_t start = melonDS::EmuSuNiTiming::Now();\n'
      '    ~EmuSuNiRunTimer() noexcept;\n'
      '};\n\n'
      'EmuSuNiRunTimer::~EmuSuNiRunTimer() noexcept {\n'
      '    using namespace melonDS::EmuSuNiTiming;\n'
      '    const uint64_t end = Now();\n'
      '    Ticks[Frame] += end - start;\n'
      '    static uint64_t frames = 0, lastTsc = 0, everyFrames = 0;\n'
      '    static std::chrono::steady_clock::time_point lastTime;\n'
      '    const auto now = std::chrono::steady_clock::now();\n'
      '    if (lastTsc == 0) {\n'
      '        const char* every = getenv("NDS_TIMING_FRAMES");\n'
      '        everyFrames = every ? strtoull(every, nullptr, 10) : 0;\n'
      '    }\n'
      '    else {\n'
      '        frames++;\n'
      '        const double seconds = std::chrono::duration<double>(now - lastTime).count();\n'
      '        if (everyFrames ? frames < everyFrames : seconds < 10.0)\n'
      '            return;\n'
      '        const double perFrame = seconds * 1000.0 / double(end - lastTsc) / double(frames);\n'
      '        auto ms = [perFrame](uint64_t ticks) { return double(ticks) * perFrame; };\n'
      '        // (the 2D drawing\'s time holds the waits for the 3D\'s lines and some for the 2D helper)\n'
      '        const double draw2d = ms(Ticks[Draw2D]) - ms(Ticks[Wait3DLine]) - ms(Ticks[Wait2D]);\n'
      '        const double wait2d = ms(Ticks[Wait2D]) + ms(Ticks[Wait2DOther]);\n'
      '        const double emulation = ms(Ticks[RunFrame]);\n'
      '        const double rest = emulation - draw2d - wait2d - ms(Ticks[Wait3DLine]) - ms(Ticks[Wait3DFrame]) - ms(Ticks[Start3D]) - ms(Ticks[Mix]);\n'
      '        const uint64_t frames3d = Frames3D.load();\n'
      '        retro::info("nds: {} frames in {:.1f} s ({:.1f}/s); per frame: retro_run {:.2f} ms = emulation {:.2f} "\n'
      '                    "[2D {:.2f}, waiting for the 2D helper {:.2f} ({:.2f} of it at a write or the frame\'s end), "\n'
      '                    "for 3D {:.2f} + {:.2f} at the end, starting 3D {:.2f}, "\n'
      '                    "sound {:.2f}, CPUs, 3D geometry and the rest {:.2f}] + screens {:.2f} + video {:.2f} + audio {:.2f}; "\n'
      '                    "3D rendering {:.2f} + {:.2f} ms on two threads, 2D helper {:.2f} ms, {} polygons, {} frames of 3D drawn",\n'
      '                    frames, seconds, frames / seconds, ms(Ticks[Frame]), emulation, draw2d, wait2d, ms(Ticks[Wait2DOther]),\n'
      '                    ms(Ticks[Wait3DLine]), ms(Ticks[Wait3DFrame]), ms(Ticks[Start3D]), ms(Ticks[Mix]), rest,\n'
      '                    ms(Ticks[Screens]) - ms(Ticks[Video]), ms(Ticks[Video]), ms(Ticks[Audio]),\n'
      '                    ms(Render3D[0].load()), ms(Render3D[1].load()), ms(Help2D.load()),\n'
      '                    frames3d ? Polygons.load() / frames3d : 0, frames3d);\n'
      '#ifdef EMUSUNI_TIMING_DETAIL\n'
      '        retro::info("nds: detail per frame: ARM9 {:.2f} ms (with its memory, DMA and 3D commands), ARM7 {:.2f}, "\n'
      '                    "3D geometry run {:.2f}, events {:.2f} (2D, sound, timers...); {} turns of the loop, {} with both "\n'
      '                    "CPUs halted ({:.2f} ms)",\n'
      '                    ms(Ticks[ARM9]), ms(Ticks[ARM7]), ms(Ticks[GX]), ms(Ticks[System]), Turns / frames,\n'
      '                    IdleTurns / frames, ms(Ticks[Idle]));\n'
      '#endif\n'
      '    }\n'
      '    for (uint64_t& t : Ticks)\n'
      '        t = 0;\n'
      '    Turns = 0;\n'
      '    IdleTurns = 0;\n'
      '    Render3D[0] = 0;\n'
      '    Render3D[1] = 0;\n'
      '    Help2D = 0;\n'
      '    Polygons = 0;\n'
      '    Frames3D = 0;\n'
      '    frames = 0;\n'
      '    lastTsc = Now();\n'
      '    lastTime = now;\n'
      '}\n'
      '}\n'
      '#endif\n\n'
      'void MelonDsDs::CoreState::Run() noexcept {\n    ZoneScopedN(TracyFunction);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNiRunTimer emusuniTimer;\n'
      '#endif\n')
patch(core, 'src/libretro/core/core.cpp',
      '                nds.RunFrame();\n',
      '#ifdef EMUSUNI_PORT\n'
      '                // EmuSuNi: a frame the frontend won\'t show (its frame skip, GET_AUDIO_VIDEO_ENABLE without\n'
      '                // the picture) isn\'t drawn: no 2D, no screens (a duplicated frame instead).\n'
      '                int emusuniAv = 3;\n'
      '                melonDS::EmuSuNiSkipDraw = retro::environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &emusuniAv) && !(emusuniAv & 1);\n'
      '                const uint64_t emusuniT0 = melonDS::EmuSuNiTiming::Now();\n'
      '                nds.RunFrame();\n'
      '                melonDS::EmuSuNiTiming::Ticks[melonDS::EmuSuNiTiming::RunFrame] += melonDS::EmuSuNiTiming::Now() - emusuniT0;\n'
      '#else\n'
      '                nds.RunFrame();\n'
      '#endif\n')
patch(core, 'src/libretro/core/core.cpp',
      '        _renderState.Render(nds, _inputState, Config, _screenLayout);\n        RenderAudio(*Console);\n',
      '#ifdef EMUSUNI_PORT\n'
      '        const uint64_t emusuniT0 = melonDS::EmuSuNiTiming::Now();\n'
      '        if (melonDS::EmuSuNiSkipDraw) {\n'
      '            const glm::uvec2 size = _screenLayout.BufferSize();\n'
      '            retro::video_refresh(nullptr, size.x, size.y, size.x * sizeof(uint32_t));\n'
      '            melonDS::EmuSuNiSkipDraw = false;\n'
      '        }\n'
      '        else\n'
      '            _renderState.Render(nds, _inputState, Config, _screenLayout);\n'
      '        const uint64_t emusuniT1 = melonDS::EmuSuNiTiming::Now();\n'
      '        RenderAudio(*Console);\n'
      '        melonDS::EmuSuNiTiming::Ticks[melonDS::EmuSuNiTiming::Screens] += emusuniT1 - emusuniT0;\n'
      '        melonDS::EmuSuNiTiming::Ticks[melonDS::EmuSuNiTiming::Audio] += melonDS::EmuSuNiTiming::Now() - emusuniT1;\n'
      '#else\n'
      '        _renderState.Render(nds, _inputState, Config, _screenLayout);\n'
      '        RenderAudio(*Console);\n'
      '#endif\n')

# --- The threaded 3D renderer, on two threads: the render thread rasterizes the even lines, a helper
# the odd ones (each with its own copy of the polygons' edges, stepped over the other's lines), the
# same pixels as one thread. The emulation thread gets the lines in order as before and waits for
# each one awake for a while (up to EMUSUNI_SCANLINE_SPIN_TICKS of the TSC) before it sleeps.
patch(melonds, 'src/GPU3D_Soft.h',
      '        constexpr s32 Step()\n        {\n            dx += Increment;\n            y++;\n',
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: Step() up to line ny at once (the lines between were another thread\'s): the\n'
      '        // same state as that many Step()s.\n'
      '        constexpr s32 StepTo(s32 ny)\n'
      '        {\n'
      '            dx += (ny - y) * Increment;\n'
      '            y = ny;\n\n'
      '            s32 x = XVal();\n'
      '            Interp.SetX(y);\n'
      '            return x;\n'
      '        }\n\n'
      '        constexpr s32 CurY() const { return y; }\n'
      '#endif\n\n'
      '        constexpr s32 Step()\n        {\n            dx += Increment;\n            y++;\n')
patch(melonds, 'src/GPU3D_Soft.h',
      '    void RenderShadowMaskScanline(RendererPolygon* rp, s32 y);\n'
      '    void RenderPolygonScanline(RendererPolygon* rp, s32 y);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: PrevIsShadowMask is the calling rasterizer thread\'s (the parameter hides the member)\n'
      '    void RenderShadowMaskScanline(RendererPolygon* rp, s32 y, bool& PrevIsShadowMask);\n'
      '    void RenderPolygonScanline(RendererPolygon* rp, s32 y, bool& PrevIsShadowMask);\n'
      '#else\n'
      '    void RenderShadowMaskScanline(RendererPolygon* rp, s32 y);\n'
      '    void RenderPolygonScanline(RendererPolygon* rp, s32 y);\n'
      '#endif\n')
patch(melonds, 'src/GPU3D_Soft.h',
      '    // threading\n\n    bool Threaded = false;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the frame rasterized by two threads (GPU3D_Soft.cpp, EmuSuNiRenderLines).\n'
      '    RendererPolygon EmuSuNiPolygonList[2048]; // the helper\'s copy of the polygons\' edges\n'
      '    void EmuSuNiClearRows(int k);\n'
      '    void EmuSuNiRenderLines(int k, RendererPolygon* list, Polygon** polygons, int npolys);\n'
      '    void EmuSuNiFinishLine(s32 y);\n'
      '    void EmuSuNiHelperFunc();\n'
      '    bool EmuSuNiStartShadow = false; // PrevIsShadowMask as the frame starts\n'
      '    std::atomic<s32> EmuSuNiRendered[2]; // the last line each thread rasterized (-1: none yet)\n'
      '    std::atomic<u8> EmuSuNiLineDone[192]; // final pass done\n'
      '    std::atomic<s32> EmuSuNiNextLine; // the next line to give the emulation thread\n'
      '    std::atomic_bool EmuSuNiHelperRunning;\n'
      '    Platform::Thread* EmuSuNiHelper = nullptr;\n'
      '    Platform::Semaphore* Sema_HelperStart = nullptr;\n'
      '    Platform::Semaphore* Sema_HelperDone = nullptr;\n'
      '#endif\n\n'
      '    // threading\n\n    bool Threaded = false;\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      'void SoftRenderer3D::RenderShadowMaskScanline(RendererPolygon* rp, s32 y)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'void SoftRenderer3D::RenderShadowMaskScanline(RendererPolygon* rp, s32 y, bool& PrevIsShadowMask)\n'
      '#else\n'
      'void SoftRenderer3D::RenderShadowMaskScanline(RendererPolygon* rp, s32 y)\n'
      '#endif\n'
      '{\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      'void SoftRenderer3D::RenderPolygonScanline(RendererPolygon* rp, s32 y)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      'void SoftRenderer3D::RenderPolygonScanline(RendererPolygon* rp, s32 y, bool& PrevIsShadowMask)\n'
      '#else\n'
      'void SoftRenderer3D::RenderPolygonScanline(RendererPolygon* rp, s32 y)\n'
      '#endif\n'
      '{\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '            if (polygon->IsShadowMask)\n'
      '                RenderShadowMaskScanline(rp, y);\n'
      '            else\n'
      '                RenderPolygonScanline(rp, y);\n',
      '#ifdef EMUSUNI_PORT\n'
      '            if (polygon->IsShadowMask)\n'
      '                RenderShadowMaskScanline(rp, y, PrevIsShadowMask);\n'
      '            else\n'
      '                RenderPolygonScanline(rp, y, PrevIsShadowMask);\n'
      '#else\n'
      '            if (polygon->IsShadowMask)\n'
      '                RenderShadowMaskScanline(rp, y);\n'
      '            else\n'
      '                RenderPolygonScanline(rp, y);\n'
      '#endif\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '        Platform::Thread_Wait(RenderThread);\n'
      '        Platform::Thread_Free(RenderThread);\n'
      '        RenderThread = nullptr;\n',
      '        Platform::Thread_Wait(RenderThread);\n'
      '        Platform::Thread_Free(RenderThread);\n'
      '        RenderThread = nullptr;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (EmuSuNiHelper)\n'
      '        {\n'
      '            // EmuSuNi: and its helper (idle: the render thread waited for it each frame)\n'
      '            EmuSuNiHelperRunning = false;\n'
      '            Platform::Semaphore_Post(Sema_HelperStart);\n'
      '            Platform::Thread_Wait(EmuSuNiHelper);\n'
      '            Platform::Thread_Free(EmuSuNiHelper);\n'
      '            EmuSuNiHelper = nullptr;\n'
      '        }\n'
      '#endif\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '            RenderThreadRunning = true; // "Time for work, render thread!"\n',
      '            RenderThreadRunning = true; // "Time for work, render thread!"\n'
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi: its helper, which rasterizes the odd lines (made first: the render thread\n'
      '            // looks at EmuSuNiHelper once it gets a frame)\n'
      '            if (!getenv("NDS_NO_3D_HELPER")) // (a PC test\'s switch)\n'
      '            {\n'
      '                EmuSuNiHelperRunning = true;\n'
      '                EmuSuNiHelper = Platform::Thread_Create([this]() {\n'
      '                    EmuSuNiHelperFunc();\n'
      '                });\n'
      '            }\n'
      '#endif\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '#include <string.h>\n',
      '#include <string.h>\n#ifdef EMUSUNI_PORT\n#include <stdlib.h>\n#endif\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '    Sema_ScanlineCount = Platform::Semaphore_Create();\n\n',
      '    Sema_ScanlineCount = Platform::Semaphore_Create();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Sema_HelperStart = Platform::Semaphore_Create();\n'
      '    Sema_HelperDone = Platform::Semaphore_Create();\n'
      '    EmuSuNiHelperRunning = false;\n'
      '#endif\n\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '    Platform::Semaphore_Free(Sema_ScanlineCount);\n',
      '    Platform::Semaphore_Free(Sema_ScanlineCount);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Platform::Semaphore_Free(Sema_HelperStart);\n'
      '    Platform::Semaphore_Free(Sema_HelperDone);\n'
      '#endif\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '        if (line < 192)\n'
      '            // We need a scanline, so let\'s wait for the render thread to finish it.\n'
      '            // (both threads process scanlines from top-to-bottom,\n'
      '            // so we don\'t need to wait for a specific row)\n'
      '            Platform::Semaphore_Wait(Sema_ScanlineCount);\n',
      '        if (line < 192)\n'
      '        {\n'
      '            // We need a scanline, so let\'s wait for the render thread to finish it.\n'
      '            // (both threads process scanlines from top-to-bottom,\n'
      '            // so we don\'t need to wait for a specific row)\n'
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi: the render threads (on other cores) are usually just finishing that line:\n'
      '            // wait for it awake first, asleep only after that; timed.\n'
      '            const u64 t0 = EmuSuNiTiming::Now();\n'
      '            bool ready = Platform::Semaphore_TryWait(Sema_ScanlineCount, 0);\n'
      '            while (!ready && EmuSuNiTiming::Now() - t0 < EMUSUNI_SCANLINE_SPIN_TICKS)\n'
      '            {\n'
      '                for (int i = 0; i < 8; i++)\n'
      '                    __builtin_ia32_pause();\n'
      '                ready = Platform::Semaphore_TryWait(Sema_ScanlineCount, 0);\n'
      '            }\n'
      '            if (!ready)\n'
      '                Platform::Semaphore_Wait(Sema_ScanlineCount);\n'
      '            EmuSuNiTiming::Ticks[EmuSuNiTiming::Wait3DLine] += EmuSuNiTiming::Now() - t0;\n'
      '#else\n'
      '            Platform::Semaphore_Wait(Sema_ScanlineCount);\n'
      '#endif\n'
      '        }\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      'void SoftRenderer3D::RenderPolygons(bool threaded, Polygon** polygons, int npolys)\n',
      '#ifdef EMUSUNI_PORT\n'
      '#ifndef EMUSUNI_SCANLINE_SPIN_TICKS\n'
      '#define EMUSUNI_SCANLINE_SPIN_TICKS 3000000 // ~2 ms of the PS4\'s TSC (1.6 GHz)\n'
      '#endif\n'
      '// EmuSuNi (the app, port/): called first thing in melonDS\'s threads ("nds 3d": the 3D render thread,\n'
      '// "nds 3d 2": its helper, "nds 2d": the 2D helper, GPU_Soft.cpp), to give them their core, priority\n'
      '// and name; none of them may share the emulation thread\'s core, nor each other\'s (they wait for\n'
      '// each other awake).\n'
      'extern "C" void emusuni_nds_thread(const char* name);\n\n'
      '// The frame rasterized by two threads, the render thread the even lines and its helper the odd ones,\n'
      '// each with its own copy of the polygons\' edges: the same pixels as one thread drawing every line in\n'
      '// turn. Drawing a line reads and writes only its own row of the buffers (and its half of the stencil\n'
      '// buffer, the even or the odd lines\'), except the final pass (edge marking), which reads the rows\n'
      '// above and below: a thread finishes one of its lines once the other has drawn the next one. The\n'
      '// lines go to the emulation thread in order, one Sema_ScanlineCount each, as with one thread.\n\n'
      '// ClearBuffers, this thread\'s rows of it.\n'
      'void SoftRenderer3D::EmuSuNiClearRows(int k)\n'
      '{\n'
      '    u32 clearz = ((GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;\n'
      '    u32 polyid = GPU3D.RenderClearAttr1 & 0x3F000000; // this sets the opaque polygonID\n\n'
      '    // the borders for edge marking: the row above the screen (thread 0) or below it (thread 1), and\n'
      '    // left and right of this thread\'s rows\n'
      '    const int border = k == 0 ? 0 : ScanlineWidth*193;\n'
      '    for (int x = border; x < border + ScanlineWidth; x++)\n'
      '    {\n'
      '        ColorBuffer[x] = 0;\n'
      '        DepthBuffer[x] = clearz;\n'
      '        AttrBuffer[x] = polyid;\n'
      '    }\n'
      '    for (int y = k; y < 192; y += 2)\n'
      '    {\n'
      '        const int x = ScanlineWidth * (y + 1);\n'
      '        ColorBuffer[x] = 0;\n'
      '        DepthBuffer[x] = clearz;\n'
      '        AttrBuffer[x] = polyid;\n'
      '        ColorBuffer[x+257] = 0;\n'
      '        DepthBuffer[x+257] = clearz;\n'
      '        AttrBuffer[x+257] = polyid;\n'
      '    }\n\n'
      '    if (GPU3D.RenderDispCnt & (1<<14))\n'
      '    {\n'
      '        const u8 xoff0 = (GPU3D.RenderClearAttr2 >> 16) & 0xFF;\n'
      '        const u8 yoff0 = (GPU3D.RenderClearAttr2 >> 24) & 0xFF;\n\n'
      '        for (int y = k; y < 192; y += 2)\n'
      '        {\n'
      '            u8 xoff = xoff0;\n'
      '            const u8 yoff = yoff0 + y;\n'
      '            for (int x = 0; x < 256; x++)\n'
      '            {\n'
      '                u16 val2 = GPU.ReadVRAMFlat_Texture<u16>(0x40000 + (yoff << 9) + (xoff << 1));\n'
      '                u16 val3 = GPU.ReadVRAMFlat_Texture<u16>(0x60000 + (yoff << 9) + (xoff << 1));\n\n'
      '                u32 r = (val2 << 1) & 0x3E; if (r) r++;\n'
      '                u32 g = (val2 >> 4) & 0x3E; if (g) g++;\n'
      '                u32 b = (val2 >> 9) & 0x3E; if (b) b++;\n'
      '                u32 a = (val2 & 0x8000) ? 0x1F000000 : 0;\n'
      '                u32 color = r | (g << 8) | (b << 16) | a;\n\n'
      '                u32 z = ((val3 & 0x7FFF) * 0x200) + 0x1FF;\n\n'
      '                u32 pixeladdr = FirstPixelOffset + y*ScanlineWidth + x;\n'
      '                ColorBuffer[pixeladdr] = color;\n'
      '                DepthBuffer[pixeladdr] = z;\n'
      '                AttrBuffer[pixeladdr] = polyid | (val3 & 0x8000);\n\n'
      '                xoff++;\n'
      '            }\n'
      '        }\n'
      '    }\n'
      '    else\n'
      '    {\n'
      '        u32 r = (GPU3D.RenderClearAttr1 << 1) & 0x3E; if (r) r++;\n'
      '        u32 g = (GPU3D.RenderClearAttr1 >> 4) & 0x3E; if (g) g++;\n'
      '        u32 b = (GPU3D.RenderClearAttr1 >> 9) & 0x3E; if (b) b++;\n'
      '        u32 a = (GPU3D.RenderClearAttr1 >> 16) & 0x1F;\n'
      '        u32 color = r | (g << 8) | (b << 16) | (a << 24);\n\n'
      '        polyid |= (GPU3D.RenderClearAttr1 & 0x8000);\n\n'
      '        for (int y = k; y < 192; y += 2)\n'
      '        {\n'
      '            for (int x = 0; x < 256; x++)\n'
      '            {\n'
      '                u32 pixeladdr = FirstPixelOffset + y*ScanlineWidth + x;\n'
      '                ColorBuffer[pixeladdr] = color;\n'
      '                DepthBuffer[pixeladdr] = clearz;\n'
      '                AttrBuffer[pixeladdr] = polyid;\n'
      '            }\n'
      '        }\n'
      '    }\n'
      '}\n\n'
      '// Line y is finished: the lines finished in order from the next one the emulation thread gets are\n'
      '// given to it (by whichever thread finishes the first of them).\n'
      'void SoftRenderer3D::EmuSuNiFinishLine(s32 y)\n'
      '{\n'
      '    EmuSuNiLineDone[y].store(1);\n'
      '    for (;;)\n'
      '    {\n'
      '        s32 next = EmuSuNiNextLine.load();\n'
      '        if (next >= 192 || !EmuSuNiLineDone[next].load())\n'
      '            return;\n'
      '        if (EmuSuNiNextLine.compare_exchange_strong(next, next + 1))\n'
      '            Platform::Semaphore_Post(Sema_ScanlineCount);\n'
      '    }\n'
      '}\n\n'
      '// Thread k\'s lines (y % 2 == k): drawn, then each one\'s final pass once the other thread has drawn\n'
      '// the lines around it.\n'
      'void SoftRenderer3D::EmuSuNiRenderLines(int k, RendererPolygon* list, Polygon** polygons, int npolys)\n'
      '{\n'
      '    const u64 t0 = EmuSuNiTiming::Now();\n'
      '    int j = 0;\n'
      '    for (int i = 0; i < npolys; i++)\n'
      '    {\n'
      '        if (polygons[i]->Degenerate) continue;\n'
      '        SetupPolygon(&list[j++], polygons[i]);\n'
      '    }\n'
      '    EmuSuNiClearRows(k);\n\n'
      '    auto covers = [](const Polygon* p, s32 y) {\n'
      '        return y >= p->YTop && (y < p->YBottom || (y == p->YTop && p->YBottom == p->YTop));\n'
      '    };\n'
      '    // PrevIsShadowMask as one thread drawing every line would have it as each of this thread\'s lines\n'
      '    // starts: the kind of the last polygon drawn on the line before, if any was, else what it was when\n'
      '    // that line started.\n'
      '    bool prevShadow = EmuSuNiStartShadow;\n'
      '    int lastNext = -1; // the kind of the last polygon on the next line (-1: none)\n'
      '    if (k == 1)\n'
      '    {\n'
      '        for (int i = 0; i < j; i++)\n'
      '            if (covers(list[i].PolyData, 0))\n'
      '                lastNext = list[i].PolyData->IsShadowMask;\n'
      '    }\n\n'
      '    const int other = 1 - k;\n'
      '    s32 finishing = k; // this thread\'s next line to finish\n'
      '    for (s32 y = k; y < 192; y += 2)\n'
      '    {\n'
      '        if (lastNext >= 0)\n'
      '            prevShadow = lastNext;\n'
      '        lastNext = -1;\n'
      '        for (int i = 0; i < j; i++)\n'
      '        {\n'
      '            RendererPolygon* rp = &list[i];\n'
      '            Polygon* polygon = rp->PolyData;\n'
      '            if (covers(polygon, y))\n'
      '            {\n'
      '                if (polygon->YTop != polygon->YBottom && rp->SlopeL.CurY() != y)\n'
      '                {\n'
      '                    // the edges stepped over the other thread\'s lines\n'
      '                    rp->XL = rp->SlopeL.StepTo(y);\n'
      '                    rp->XR = rp->SlopeR.StepTo(y);\n'
      '                }\n'
      '                if (polygon->IsShadowMask)\n'
      '                    RenderShadowMaskScanline(rp, y, prevShadow);\n'
      '                else\n'
      '                    RenderPolygonScanline(rp, y, prevShadow);\n'
      '            }\n'
      '            if (covers(polygon, y + 1))\n'
      '                lastNext = polygon->IsShadowMask;\n'
      '        }\n'
      '        EmuSuNiRendered[k].store(y, std::memory_order_release);\n\n'
      '        // this thread\'s lines with both neighbours drawn: final pass, then to the emulation thread\n'
      '        while (finishing <= y)\n'
      '        {\n'
      '            const s32 need = finishing + 1 < 192 ? finishing + 1 : finishing - 1;\n'
      '            if (EmuSuNiRendered[other].load(std::memory_order_acquire) < need)\n'
      '                break;\n'
      '            ScanlineFinalPass(finishing);\n'
      '            EmuSuNiFinishLine(finishing);\n'
      '            finishing += 2;\n'
      '        }\n'
      '    }\n'
      '    // the others, as the other thread draws the lines they need\n'
      '    while (finishing < 192)\n'
      '    {\n'
      '        const s32 need = finishing + 1 < 192 ? finishing + 1 : finishing - 1;\n'
      '        for (int spins = 0; EmuSuNiRendered[other].load(std::memory_order_acquire) < need; spins++)\n'
      '        {\n'
      '            if (spins < 100000)\n'
      '                __builtin_ia32_pause();\n'
      '            else\n'
      '                std::this_thread::yield();\n'
      '        }\n'
      '        ScanlineFinalPass(finishing);\n'
      '        EmuSuNiFinishLine(finishing);\n'
      '        finishing += 2;\n'
      '    }\n'
      '    if (k == 1)\n'
      '        PrevIsShadowMask = prevShadow; // as after line 191, for the next frame\n'
      '    EmuSuNiTiming::Render3D[k] += EmuSuNiTiming::Now() - t0;\n'
      '}\n\n'
      'void SoftRenderer3D::EmuSuNiHelperFunc()\n'
      '{\n'
      '    emusuni_nds_thread("nds 3d 2");\n'
      '    for (;;)\n'
      '    {\n'
      '        Platform::Semaphore_Wait(Sema_HelperStart);\n'
      '        if (!EmuSuNiHelperRunning)\n'
      '            return;\n'
      '        EmuSuNiRenderLines(1, EmuSuNiPolygonList, &GPU3D.RenderPolygonRAM[0], GPU3D.RenderNumPolygons);\n'
      '        Platform::Semaphore_Post(Sema_HelperDone);\n'
      '    }\n'
      '}\n'
      '#endif\n\n'
      'void SoftRenderer3D::RenderPolygons(bool threaded, Polygon** polygons, int npolys)\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '    if (RenderThreadRunning.load(std::memory_order_relaxed) && !GPU3D.AbortFrame)\n'
      '        Platform::Semaphore_Wait(Sema_RenderDone);\n}\n',
      '    if (RenderThreadRunning.load(std::memory_order_relaxed) && !GPU3D.AbortFrame)\n'
      '    {\n'
      '        Platform::Semaphore_Wait(Sema_RenderDone);\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: the lines the 2D didn\'t ask for (a frame not shown: EmuSuNiSkipDraw) let go; all\n'
      '        // 192 were given by now (none left when the 2D asked for them all).\n'
      '        Platform::Semaphore_Reset(Sema_ScanlineCount);\n'
      '#endif\n'
      '    }\n'
      '}\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      'void SoftRenderer3D::RenderThreadFunc()\n{\n',
      'void SoftRenderer3D::RenderThreadFunc()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_nds_thread("nds 3d");\n'
      '#endif\n')
patch(melonds, 'src/GPU3D_Soft.cpp',
      '            ClearBuffers();\n'
      '            RenderPolygons(true, &GPU3D.RenderPolygonRAM[0], GPU3D.RenderNumPolygons);\n',
      '#ifdef EMUSUNI_PORT\n'
      '            EmuSuNiTiming::Polygons += GPU3D.RenderNumPolygons;\n'
      '            EmuSuNiTiming::Frames3D++;\n'
      '            if (EmuSuNiHelper)\n'
      '            {\n'
      '                // EmuSuNi: the odd lines to the helper, the even ones here\n'
      '                for (int y = 0; y < 192; y++)\n'
      '                    EmuSuNiLineDone[y].store(0, std::memory_order_relaxed);\n'
      '                EmuSuNiNextLine.store(0, std::memory_order_relaxed);\n'
      '                EmuSuNiRendered[0].store(-1, std::memory_order_relaxed);\n'
      '                EmuSuNiRendered[1].store(-1, std::memory_order_relaxed);\n'
      '                EmuSuNiStartShadow = PrevIsShadowMask;\n'
      '                Platform::Semaphore_Post(Sema_HelperStart);\n'
      '                EmuSuNiRenderLines(0, PolygonList, &GPU3D.RenderPolygonRAM[0], GPU3D.RenderNumPolygons);\n'
      '                Platform::Semaphore_Wait(Sema_HelperDone);\n'
      '            }\n'
      '            else\n'
      '            {\n'
      '                const u64 t0 = EmuSuNiTiming::Now();\n'
      '                ClearBuffers();\n'
      '                RenderPolygons(true, &GPU3D.RenderPolygonRAM[0], GPU3D.RenderNumPolygons);\n'
      '                EmuSuNiTiming::Render3D[0] += EmuSuNiTiming::Now() - t0;\n'
      '            }\n'
      '#else\n'
      '            ClearBuffers();\n'
      '            RenderPolygons(true, &GPU3D.RenderPolygonRAM[0], GPU3D.RenderNumPolygons);\n'
      '#endif\n')

# --- The 2D, with the threaded renderer: the sub screen's engine (B) drawn by a helper thread while the
# emulation thread draws the main one's (A) and the 3D's line, then goes on emulating. Each of B's lines
# (and its sprites for the next line) is a job with a copy of B's registers taken at the line's HBlank,
# up to 8 lines queued; the emulation thread waits for them only when 8 are queued, at the frame's end,
# or before a write to something they read (B's palette or OAM, VRAM mapped to B, the VRAM mapping):
# each line as melonDS would have drawn it at its HBlank. The one thing B's drawing changes besides the
# picture, its windows' flags, the emulation thread works out itself (the same, see below). The two
# engines share nothing they write (each its own buffers and VRAM copies). A frame the frontend won't
# show (EmuSuNiSkipDraw) isn't drawn: only those windows' flags change.
patch(melonds, 'src/GPU.h',
      '    template<typename T>\n'
      '    void WriteVRAM_BBG(u32 addr, T val)\n'
      '    {\n',
      '    template<typename T>\n'
      '    void WriteVRAM_BBG(u32 addr, T val)\n'
      '    {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (EmuSuNi2DPending) EmuSuNi2DSyncPending(); // EmuSuNi: B\'s lines drawn first\n'
      '#endif\n')
patch(melonds, 'src/GPU.h',
      '    template<typename T>\n'
      '    void WriteVRAM_BOBJ(u32 addr, T val)\n'
      '    {\n',
      '    template<typename T>\n'
      '    void WriteVRAM_BOBJ(u32 addr, T val)\n'
      '    {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (EmuSuNi2DPending) EmuSuNi2DSyncPending(); // EmuSuNi: B\'s lines drawn first\n'
      '#endif\n')
patch(melonds, 'src/GPU.h',
      '    void WritePalette(u32 addr, T val)\n'
      '    {\n'
      '        addr &= 0x7FF;\n',
      '    void WritePalette(u32 addr, T val)\n'
      '    {\n'
      '        addr &= 0x7FF;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (EmuSuNi2DPending && (addr & 0x400)) EmuSuNi2DSyncPending(); // EmuSuNi: B\'s lines drawn first\n'
      '#endif\n')
patch(melonds, 'src/GPU.h',
      '    void WriteOAM(u32 addr, T val)\n'
      '    {\n'
      '        addr &= 0x7FF;\n',
      '    void WriteOAM(u32 addr, T val)\n'
      '    {\n'
      '        addr &= 0x7FF;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (EmuSuNi2DPending && (addr & 0x400)) EmuSuNi2DSyncPending(); // EmuSuNi: B\'s lines drawn first\n'
      '#endif\n')
for fn in ['void GPU::ResetVRAMCache() noexcept\n{\n', 'void GPU::Reset() noexcept\n{\n', 'void GPU::Stop() noexcept\n{\n',
           'void GPU::DoSavestate(Savestate* file) noexcept\n{\n',
           'void GPU::SetRenderer(std::unique_ptr<Renderer>&& renderer) noexcept\n{\n',
           'void GPU::MapVRAM_AB(u32 bank, u8 cnt) noexcept\n{\n', 'void GPU::MapVRAM_CD(u32 bank, u8 cnt) noexcept\n{\n',
           'void GPU::MapVRAM_E(u32 bank, u8 cnt) noexcept\n{\n', 'void GPU::MapVRAM_FG(u32 bank, u8 cnt) noexcept\n{\n',
           'void GPU::MapVRAM_H(u32 bank, u8 cnt) noexcept\n{\n', 'void GPU::MapVRAM_I(u32 bank, u8 cnt) noexcept\n{\n',
           'void GPU::FinishFrame(u32 lines) noexcept\n{\n']:
    patch(melonds, 'src/GPU.cpp', fn,
          fn + '#ifdef EMUSUNI_PORT\n'
               '    if (EmuSuNi2DPending) EmuSuNi2DSyncPending(); // EmuSuNi: the 2D helper\'s lines drawn first\n'
               '#endif\n')
patch(melonds, 'src/GPU_Soft.h',
      '    void ApplyMasterBrightness(u16 regval, u32* dst);\n    void ExpandColor(u32* dst);\n};\n',
      '    void ApplyMasterBrightness(u16 regval, u32* dst);\n    void ExpandColor(u32* dst);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: B\'s lines by a helper thread (GPU_Soft.cpp); Rend2D_B draws with EmuSuNiB, set to the\n'
      '    // copy of GPU2D_B\'s registers each line\'s job has.\n'
      '    friend void EmuSuNi2DSyncPending();\n'
      '    static constexpr u32 EmuSuNi2DQueue = 8;\n'
      '    struct EmuSuNi2DLineJob\n'
      '    {\n'
      '        std::unique_ptr<GPU2D> Regs; // B\'s registers at the line\'s HBlank\n'
      '        u32 Line = 0;\n'
      '        u32* Dst = nullptr;\n'
      '        bool ScreensEnabled = false;\n'
      '        u16 Brightness = 0;\n'
      '        s32 Sprites = -1; // the line whose sprites it draws too (-1: none)\n'
      '    };\n'
      '    void EmuSuNi2DStart();\n'
      '    void EmuSuNi2DStop();\n'
      '    void EmuSuNi2DHelperFunc();\n'
      '    void EmuSuNi2DDrawB(const EmuSuNi2DLineJob& job);\n'
      '    void EmuSuNi2DWaitFor(u32 jobs, bool inDraw);\n'
      '    void EmuSuNi2DSync(bool inDraw = false);\n'
      '    GPU2D EmuSuNiB;\n'
      '    EmuSuNi2DLineJob EmuSuNi2DJobs[EmuSuNi2DQueue];\n'
      '    Platform::Thread* EmuSuNi2DHelper = nullptr;\n'
      '    Platform::Semaphore* Sema_2DWake = nullptr;\n'
      '    std::atomic<u32> EmuSuNi2DJob{0}, EmuSuNi2DDone{0}; // the lines given to it, the lines it drew\n'
      '    std::atomic_bool EmuSuNi2DRunning{false}, EmuSuNi2DAsleep{false};\n'
      '    // (the emulation thread\'s) the line whose B sprites were given with B\'s line\n'
      '    s32 EmuSuNi2DSpritesDone = -1;\n'
      '#endif\n'
      '};\n')
patch(melonds, 'src/GPU_Soft.cpp',
      '#include "GPU_ColorOp.h"\n',
      '#include "GPU_ColorOp.h"\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#endif\n')
patch(melonds, 'src/GPU_Soft.cpp',
      'SoftRenderer::SoftRenderer(melonDS::NDS& nds)\n    : Renderer(nds.GPU)\n{\n',
      'SoftRenderer::SoftRenderer(melonDS::NDS& nds)\n    : Renderer(nds.GPU)\n'
      '#ifdef EMUSUNI_PORT\n'
      '    , EmuSuNiB(1, nds.GPU)\n'
      '#endif\n'
      '{\n'
      '#ifdef EMUSUNI_PORT\n'
      '    for (EmuSuNi2DLineJob& job : EmuSuNi2DJobs)\n'
      '        job.Regs = std::make_unique<GPU2D>(1, nds.GPU);\n'
      '#endif\n')
patch(melonds, 'src/GPU_Soft.cpp',
      '    Rend2D_B = std::make_unique<SoftRenderer2D>(GPU.GPU2D_B, *this);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    Rend2D_B = std::make_unique<SoftRenderer2D>(EmuSuNiB, *this); // EmuSuNi: with the copy of B\'s registers\n'
      '#else\n'
      '    Rend2D_B = std::make_unique<SoftRenderer2D>(GPU.GPU2D_B, *this);\n'
      '#endif\n')
patch(melonds, 'src/GPU_Soft.cpp',
      'SoftRenderer::~SoftRenderer()\n{\n',
      'SoftRenderer::~SoftRenderer()\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi2DStop();\n'
      '#endif\n')
for fn in ['void SoftRenderer::Reset()\n{\n', 'void SoftRenderer::Stop()\n{\n', 'void SoftRenderer::PreSavestate()\n{\n']:
    patch(melonds, 'src/GPU_Soft.cpp', fn,
          fn + '#ifdef EMUSUNI_PORT\n    EmuSuNi2DSync();\n#endif\n')
patch(melonds, 'src/GPU_Soft.cpp',
      '    rend3d->SetThreaded(settings.Threaded);\n}\n',
      '    rend3d->SetThreaded(settings.Threaded);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the 2D helper too, with the threaded renderer\n'
      '    if (settings.Threaded)\n'
      '        EmuSuNi2DStart();\n'
      '    else\n'
      '        EmuSuNi2DStop();\n'
      '#endif\n'
      '}\n')
patch(melonds, 'src/GPU_Soft.cpp',
      'void SoftRenderer::DrawScanline(u32 line)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: what drawing a line changes in a 2D engine\'s state besides the picture: its windows\'\n'
      '// horizontal flags (GPU2D::CalculateWindowMask, only with the engine on and a window on), which end\n'
      '// the line set if the window starts after it ends (x1 > x2), else clear, whatever they were.\n'
      'static void EmuSuNiLineWindows(GPU2D& unit)\n'
      '{\n'
      '    if (!unit.Enabled || unit.ForcedBlank || !(unit.DispCnt & 0xE000))\n'
      '        return;\n'
      '    if (unit.DispCnt & (1<<14))\n'
      '        unit.Win1Active = (unit.Win1Active & ~0x2) | (unit.Win1Coords[0] > unit.Win1Coords[1] ? 0x2 : 0);\n'
      '    if (unit.DispCnt & (1<<13))\n'
      '        unit.Win0Active = (unit.Win0Active & ~0x2) | (unit.Win0Coords[0] > unit.Win0Coords[1] ? 0x2 : 0);\n'
      '}\n\n'
      '// The 2D helper: lines given to it and maybe not drawn yet (the emulation thread\'s), and the\n'
      '// renderer they\'re for.\n'
      'bool EmuSuNi2DPending = false;\n'
      'static SoftRenderer* EmuSuNi2DOwner = nullptr;\n\n'
      '// A copy of a 2D engine\'s registers (GPU2D\'s data from Enabled to EVY, in that order).\n'
      'static void EmuSuNiCopy2D(GPU2D& to, const GPU2D& from)\n'
      '{\n'
      '    memcpy(&to.Enabled, &from.Enabled, (const u8*)(&from.EVY + 1) - (const u8*)&from.Enabled);\n'
      '}\n'
      '#endif\n\n'
      'void SoftRenderer::DrawScanline(u32 line)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a frame the frontend won\'t show: no 2D (the 3D\'s lines are let go at the frame\'s end,\n'
      '    // FinishRendering), unless display capture needs this line\'s; only what the drawing would\n'
      '    // have changed besides the picture.\n'
      '    if (EmuSuNiSkipDraw && !GPU.CaptureEnable)\n'
      '    {\n'
      '        if (GPU.VCount < 192)\n'
      '        {\n'
      '            EmuSuNiLineWindows(GPU.GPU2D_A);\n'
      '            EmuSuNiLineWindows(GPU.GPU2D_B);\n'
      '        }\n'
      '        return;\n'
      '    }\n'
      '#endif\n')
patch(melonds, 'src/GPU_Soft.cpp',
      '    // the position used for drawing operations is based on VCOUNT\n    line = GPU.VCount;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    if (GPU.VCount < 192)\n'
      '    {\n'
      '        // EmuSuNi: B\'s line, colours and sprites for the next line (StartHBlank asks for them right\n'
      '        // after this) by the helper with B\'s registers as they are now, while A\'s and the 3D\'s line\n'
      '        // are drawn here (or B\'s here first, without the helper).\n'
      '        const s32 sprites = line < 191 ? (s32)line + 1 : -1;\n'
      '        EmuSuNi2DSpritesDone = sprites;\n'
      '        const u32 next = EmuSuNi2DJob.load(std::memory_order_relaxed);\n'
      '        if (EmuSuNi2DHelper && next - EmuSuNi2DDone.load(std::memory_order_acquire) >= EmuSuNi2DQueue)\n'
      '            EmuSuNi2DWaitFor(next - EmuSuNi2DQueue + 1, true); // (a free job)\n'
      '        EmuSuNi2DLineJob& job = EmuSuNi2DJobs[EmuSuNi2DHelper ? next % EmuSuNi2DQueue : 0];\n'
      '        EmuSuNiCopy2D(*job.Regs, GPU.GPU2D_B);\n'
      '        job.Line = GPU.VCount;\n'
      '        job.Dst = dstB;\n'
      '        job.ScreensEnabled = GPU.ScreensEnabled;\n'
      '        job.Brightness = GPU.MasterBrightnessB;\n'
      '        job.Sprites = sprites;\n'
      '        EmuSuNiLineWindows(GPU.GPU2D_B); // what the line\'s drawing leaves in them\n'
      '        if (EmuSuNi2DHelper)\n'
      '        {\n'
      '            EmuSuNi2DJob.store(next + 1);\n'
      '            if (EmuSuNi2DAsleep.load())\n'
      '                Platform::Semaphore_Post(Sema_2DWake);\n'
      '            EmuSuNi2DPending = true;\n'
      '            EmuSuNi2DOwner = this;\n'
      '        }\n'
      '        else\n'
      '            EmuSuNi2DDrawB(job);\n\n'
      '        line = GPU.VCount;\n'
      '        Output3D = Rend3D->GetLine(line);\n'
      '        Rend2D_A->DrawScanline(line);\n'
      '        DrawScanlineA(line, dstA);\n'
      '        // (capture reads only A\'s and the 3D\'s line, and writes a bank mapped to the LCDC)\n'
      '        if (GPU.CaptureEnable)\n'
      '            DoCapture(line);\n'
      '        if (GPU.ScreensEnabled)\n'
      '            ExpandColor(dstA);\n'
      '        else\n'
      '        {\n'
      '            for (int i = 0; i < 256; i++)\n'
      '                dstA[i] = 0xFF000000;\n'
      '        }\n'
      '        return;\n'
      '    }\n'
      '#endif\n'
      '    // the position used for drawing operations is based on VCOUNT\n    line = GPU.VCount;\n')
patch(melonds, 'src/GPU_Soft.cpp',
      'void SoftRenderer::DrawSprites(u32 line)\n{\n'
      '    Rend2D_A->DrawSprites(line);\n'
      '    Rend2D_B->DrawSprites(line);\n'
      '}\n',
      'void SoftRenderer::DrawSprites(u32 line)\n{\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: B\'s were given with its line (the helper\'s, maybe still drawing), else drawn here with\n'
      '    // B\'s registers as they are now; none in a frame not shown (line 0\'s are drawn at the end of the\n'
      '    // frame before: the next frame\'s, which may be shown).\n'
      '    if (EmuSuNi2DSpritesDone == (s32)line)\n'
      '    {\n'
      '        EmuSuNi2DSpritesDone = -1;\n'
      '        Rend2D_A->DrawSprites(line);\n'
      '        return;\n'
      '    }\n'
      '    if (EmuSuNiSkipDraw && !GPU.CaptureEnable && line != 0)\n'
      '        return;\n'
      '    EmuSuNi2DSync(true);\n'
      '    Rend2D_A->DrawSprites(line);\n'
      '    EmuSuNiCopy2D(EmuSuNiB, GPU.GPU2D_B);\n'
      '    Rend2D_B->DrawSprites(line);\n'
      '#else\n'
      '    Rend2D_A->DrawSprites(line);\n'
      '    Rend2D_B->DrawSprites(line);\n'
      '#endif\n'
      '}\n')
patch(melonds, 'src/GPU_Soft.cpp',
      'bool SoftRenderer::GetFramebuffers(void** top, void** bottom)\n{\n',
      '#ifdef EMUSUNI_PORT\n'
      '#ifndef EMUSUNI_2D_SPIN_TICKS\n'
      '#define EMUSUNI_2D_SPIN_TICKS 400000 // ~250 us of the PS4\'s TSC: more than a line, less than VBlank\n'
      '#endif\n'
      'extern "C" void emusuni_nds_thread(const char* name);\n\n'
      'void EmuSuNi2DSyncPending()\n'
      '{\n'
      '    if (EmuSuNi2DOwner)\n'
      '        EmuSuNi2DOwner->EmuSuNi2DSync();\n'
      '    EmuSuNi2DPending = false;\n'
      '}\n\n'
      '// B\'s line and its sprites for the next line: a job of the helper\'s (or the emulation thread\'s).\n'
      'void SoftRenderer::EmuSuNi2DDrawB(const EmuSuNi2DLineJob& job)\n'
      '{\n'
      '    EmuSuNiCopy2D(EmuSuNiB, *job.Regs);\n'
      '    u32* dst = job.Dst;\n'
      '    Rend2D_B->DrawScanline(job.Line);\n'
      '    // DrawScanlineB with the copy\'s registers and the brightness as it was\n'
      '    if (!((EmuSuNiB.DispCnt >> 16) & 0x1))\n'
      '    {\n'
      '        for (int i = 0; i < 256; i++)\n'
      '            dst[i] = 0xFF3F3F3F;\n'
      '    }\n'
      '    else\n'
      '    {\n'
      '        for (int i = 0; i < 256; i+=2)\n'
      '            *(u64*)&dst[i] = *(u64*)&Output2D[1][i];\n'
      '        ApplyMasterBrightness(job.Brightness, dst);\n'
      '    }\n'
      '    if (job.ScreensEnabled)\n'
      '        ExpandColor(dst);\n'
      '    else\n'
      '    {\n'
      '        for (int i = 0; i < 256; i++)\n'
      '            dst[i] = 0xFF000000;\n'
      '    }\n'
      '    if (job.Sprites >= 0)\n'
      '        Rend2D_B->DrawSprites(job.Sprites);\n'
      '}\n\n'
      'void SoftRenderer::EmuSuNi2DStart()\n'
      '{\n'
      '    if (EmuSuNi2DHelper || getenv("NDS_NO_2D_HELPER")) // (a PC test\'s switch)\n'
      '        return;\n'
      '    if (!Sema_2DWake)\n'
      '        Sema_2DWake = Platform::Semaphore_Create();\n'
      '    EmuSuNi2DRunning = true;\n'
      '    EmuSuNi2DHelper = Platform::Thread_Create([this]() {\n'
      '        EmuSuNi2DHelperFunc();\n'
      '    });\n'
      '}\n\n'
      'void SoftRenderer::EmuSuNi2DStop()\n'
      '{\n'
      '    EmuSuNi2DSync();\n'
      '    if (EmuSuNi2DHelper)\n'
      '    {\n'
      '        EmuSuNi2DRunning = false;\n'
      '        EmuSuNi2DJob.store(EmuSuNi2DJob.load() + 1);\n'
      '        Platform::Semaphore_Post(Sema_2DWake);\n'
      '        Platform::Thread_Wait(EmuSuNi2DHelper);\n'
      '        Platform::Thread_Free(EmuSuNi2DHelper);\n'
      '        EmuSuNi2DHelper = nullptr;\n'
      '        EmuSuNi2DDone.store(EmuSuNi2DJob.load());\n'
      '    }\n'
      '    if (Sema_2DWake)\n'
      '    {\n'
      '        Platform::Semaphore_Free(Sema_2DWake);\n'
      '        Sema_2DWake = nullptr;\n'
      '    }\n'
      '    if (EmuSuNi2DOwner == this)\n'
      '        EmuSuNi2DOwner = nullptr;\n'
      '}\n\n'
      'void SoftRenderer::EmuSuNi2DHelperFunc()\n'
      '{\n'
      '    emusuni_nds_thread("nds 2d");\n'
      '    u32 done = EmuSuNi2DDone.load();\n'
      '    for (;;)\n'
      '    {\n'
      '        u64 t0 = EmuSuNiTiming::Now();\n'
      '        while (EmuSuNi2DJob.load(std::memory_order_acquire) == done)\n'
      '        {\n'
      '            if (EmuSuNiTiming::Now() - t0 < EMUSUNI_2D_SPIN_TICKS)\n'
      '            {\n'
      '                for (int i = 0; i < 8; i++)\n'
      '                    __builtin_ia32_pause();\n'
      '                continue;\n'
      '            }\n'
      '            // nothing for a while (the frame\'s end): asleep until the next line\n'
      '            EmuSuNi2DAsleep.store(true);\n'
      '            if (EmuSuNi2DJob.load() == done)\n'
      '                Platform::Semaphore_Wait(Sema_2DWake);\n'
      '            EmuSuNi2DAsleep.store(false);\n'
      '            t0 = EmuSuNiTiming::Now();\n'
      '        }\n'
      '        if (!EmuSuNi2DRunning)\n'
      '            return;\n'
      '        // the next line given to it, in order\n'
      '        const u64 t1 = EmuSuNiTiming::Now();\n'
      '        EmuSuNi2DDrawB(EmuSuNi2DJobs[done % EmuSuNi2DQueue]);\n'
      '        EmuSuNiTiming::Help2D += EmuSuNiTiming::Now() - t1;\n'
      '        done++;\n'
      '        EmuSuNi2DDone.store(done, std::memory_order_release);\n'
      '    }\n'
      '}\n\n'
      '// The emulation thread: waits (awake) until the helper has drawn that many lines.\n'
      'void SoftRenderer::EmuSuNi2DWaitFor(u32 jobs, bool inDraw)\n'
      '{\n'
      '    if ((s32)(EmuSuNi2DDone.load(std::memory_order_acquire) - jobs) >= 0)\n'
      '        return;\n'
      '    const u64 t0 = EmuSuNiTiming::Now();\n'
      '    for (u32 spins = 0; (s32)(EmuSuNi2DDone.load(std::memory_order_acquire) - jobs) < 0; spins++)\n'
      '    {\n'
      '        if (spins < 1000000)\n'
      '            __builtin_ia32_pause();\n'
      '        else\n'
      '            std::this_thread::yield(); // (not running: another thread has its core)\n'
      '    }\n'
      '    EmuSuNiTiming::Ticks[inDraw ? EmuSuNiTiming::Wait2D : EmuSuNiTiming::Wait2DOther] += EmuSuNiTiming::Now() - t0;\n'
      '}\n\n'
      '// ... every line given to it.\n'
      'void SoftRenderer::EmuSuNi2DSync(bool inDraw)\n'
      '{\n'
      '    if (!EmuSuNi2DPending || EmuSuNi2DOwner != this)\n'
      '        return;\n'
      '    EmuSuNi2DWaitFor(EmuSuNi2DJob.load(std::memory_order_relaxed), inDraw);\n'
      '    EmuSuNi2DPending = false;\n'
      '}\n'
      '#endif\n\n'
      'bool SoftRenderer::GetFramebuffers(void** top, void** bottom)\n{\n')

# --- Threads: 1 MiB stacks (libretro-common leaves the platform's default otherwise).
patch(lrc, 'rthreads/rthreads.c',
      '#elif defined(__APPLE__)\n   /* Default stack size on Apple is 512Kb;\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '   /* EmuSuNi: 1 MiB for melonDS\'s renderer threads, whatever the PS4\'s default is. */\n'
      '   pthread_attr_setstacksize(&thread_attr , 0x100000 );\n'
      '   thread_attr_needed = true;\n'
      '#elif defined(__APPLE__)\n   /* Default stack size on Apple is 512Kb;\n')

for (root, path), text in patched.items():
    full = root + '/' + path
    with open(full, encoding='utf-8') as f:
        same = f.read() == text
    if not same:
        with open(full, 'w', encoding='utf-8') as f:
            f.write(text)
print("melonDS DS: patches applied")
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-nds-core.sh: the melondsds core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f melondsds_$f"; done
} > "$OUT/prefix_melondsds.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_melondsds.h.new" "$OUT/prefix_melondsds.h"; then
  rm -f "$OUT/prefix_melondsds.h.new"
else
  mv "$OUT/prefix_melondsds.h.new" "$OUT/prefix_melondsds.h"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): CMake's -O3, the
# PS4's CPU and the renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -include $OUT/prefix_melondsds.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a"
# The PC tests' timing of the CPUs, the 3D commands and the events too (the log's "detail" line).
[ "${NDS_CORE_TIMING_DETAIL:-0}" = 1 ] && FLAGS_CORE+=" -DEMUSUNI_TIMING_DETAIL=1"
# (Experiments: more compiler flags.)
FLAGS_CORE+="${NDS_CORE_EXTRA_FLAGS:+ $NDS_CORE_EXTRA_FLAGS}"
# PGO (profile-guided optimisation, ~10% faster on the PC): scripts/nds-core.profdata.xz, a profile
# of the PC build playing Mario Kart DS (the menus, a race) and New Super Mario Bros. (a level), used
# by both builds when it's there (NDS_CORE_PGO=off: without it). A profile older than the sources only
# optimises less (the functions that changed get none; the log counts them). A new one:
# NDS_CORE_HOST=1 NDS_CORE_PGO=generate build-nds-core.sh <dir> (instrumented), the PC test frontend
# linked with -fprofile-instr-generate and run on the games with LLVM_PROFILE_FILE=<dir>/%p.profraw,
# then llvm-profdata-21 merge -o nds-core.profdata <dir>/*.profraw; xz -9 it into scripts/.
PROFILE="$HERE/nds-core.profdata.xz"
PGO="none"
case "${NDS_CORE_PGO:-use}" in
  generate)
    if [ "$HOST" != 1 ]; then
      echo "melonDS DS: NDS_CORE_PGO=generate is for the PC build (NDS_CORE_HOST=1)"
      exit 1
    fi
    FLAGS_CORE+=" -fprofile-instr-generate"
    PGO="generate" ;;
  off) ;;
  *)
    if [ -f "$PROFILE" ]; then
      xz -dc "$PROFILE" > "$OUT/nds-core.profdata.new"
      if cmp -s "$OUT/nds-core.profdata.new" "$OUT/nds-core.profdata"; then
        rm -f "$OUT/nds-core.profdata.new"
      else
        mv "$OUT/nds-core.profdata.new" "$OUT/nds-core.profdata"
      fi
      FLAGS_CORE+=" -fprofile-instr-use=$OUT/nds-core.profdata -Wno-profile-instr-unprofiled"
      PGO="profile $(sha256sum "$PROFILE" | cut -c1-16)"
    fi ;;
esac
FLAGS_CXX="$FLAGS_CORE"
# The dependencies melonDS DS's CMake would fetch: setup.sh's clones (FetchContent's names).
DEP_DIRS=()
for dep in melonDS libretro-common embed-binaries glm libslirp pntr fmt yamc span-lite date zlib; do
  if [ ! -d "$DEPS/$dep/.git" ]; then
    echo "melonDS DS: $DEPS/$dep is missing (scripts/setup.sh clones it)"
    exit 1
  fi
  DEP_DIRS+=("-DFETCHCONTENT_SOURCE_DIR_${dep^^}=$DEPS/$dep")
done
OPTIONS=(
  -DEMUSUNI_PORT=ON -DENABLE_JIT=ON -DENABLE_THREADED_RENDERER=ON -DENABLE_OPENGL=OFF
  -DENABLE_COMPUTE_RENDERER=OFF -DENABLE_EGL=OFF -DENABLE_NETWORKING=OFF -DENABLE_DYNAMIC=OFF
  -DENABLE_ZLIB=ON -DTRACY_ENABLE=OFF -DBUILD_TESTING=OFF -DENABLE_GDBSTUB=OFF
  -DENABLE_LTO_RELEASE=OFF -DENABLE_LTO=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
  -DHAVE_MMAP=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON "${DEP_DIRS[@]}"
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CXX"
)
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  }
else
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
COMMITS="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
for dep in "$DEPS"/*/; do COMMITS+=" $(git -C "$dep" rev-parse HEAD 2>/dev/null || echo unknown)"; done
WANT="$SRC | $COMMITS | host $HOST | pgo $PGO | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "melonDS DS: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "melonDS DS: configuring failed (see $LOG)"
    exit 1
  fi
fi
# At most 4 compilers at once (the PC's load).
if ! ninja -C "$BUILD" -j "${NDS_CORE_JOBS:-4}" melondsds_libretro >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "melonDS DS: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"
if [ "${PGO%% *}" = profile ]; then
  # (clang's -Wprofile-instr-out-of-date, one line per file: "of N functions, M have mismatched data")
  STALE=$( { grep -oE "of [0-9]+ functions, [0-9]+ ha(s|ve) mismatched data" "$LOG" || true; } |
           awk '{n += $2; m += $4} END {print m + 0, "of", n + 0}')
  echo "melonDS DS: PGO with scripts/nds-core.profdata.xz (functions it doesn't match in the files compiled now: $STALE in the files with any)"
fi

# One object: only the libretro API (renamed) stays global. Every library the core target was made
# of (melonDS DS, melonDS's core and Teakra, libretro-common, fmt, glm, zlib, the embedded assets).
# Replaced only when it changed (the app isn't linked again for nothing).
mapfile -t ARCHIVES < <(find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
ld.lld-21 -r --whole-archive "${ARCHIVES[@]}" -o "$OUT/nds_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='melondsds_retro_*' "$OUT/nds_core_all.o" "$OUT/nds_core.o.new"
rm -f "$OUT/nds_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/nds_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "melondsds_$f"; done | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "melonDS DS: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/nds_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved: the app would export them as imports nobody gives (the
# PS4's loader may refuse those).
WEAK=$(llvm-nm-21 -u "$OUT/nds_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "melonDS DS: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if llvm-nm-21 --defined-only "$OUT/nds_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "melonDS DS: the object defines a C++ operator new/delete of its own"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  # What the app gives it: operator new/delete (port/ps4_new.cpp) and the executable memory
  # (port/ps4_n64.c), compiled here as the app compiles them, only to list their symbols.
  PS4C="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay"
  PS4C+=" -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -march=btver2"
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $PS4C -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  clang-21 $PS4C -O2 -c "$PROJECT/port/ps4_n64.c" -o "$OUT/check_ps4_n64.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores. The threads'
  # hook, emusuni_nds_thread, is the app's to define in port/: a note if it isn't yet.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o" | awk 'NF >= 3 {print $3}';
                 echo __dso_handle; echo emusuni_nds_thread; } | sort -u)
  rm -f "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o"
  if ! grep -qs "emusuni_nds_thread" "$PROJECT"/port/*.c "$PROJECT"/port/*.cpp; then
    echo "melonDS DS: note: port/ doesn't define emusuni_nds_thread yet (the app won't link without it)"
  fi
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "melonDS DS: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
    echo "$NOT_APPS"
    exit 1
  fi
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a $PS4_BUILD_ROOT/sysroot/lib/libc++.a
    $PS4_BUILD_ROOT/sysroot/lib/libc++abi.a $OPENORBIS/lib/libunwind.a"
  PROVIDED=$( { for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done | awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVvi]$/ {print $NF}'; echo "$APP_GIVES"; } | sort -u)
  MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
  if [ -n "$MISSING" ]; then
    echo "melonDS DS: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "melonDS DS: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app" \
       "($(echo "$NEW_DELETE" | sed '/^$/d' | wc -l) operator new/delete, port/ps4_new.cpp's)"
fi
if cmp -s "$OUT/nds_core.o.new" "$OUT/nds_core.o"; then
  rm -f "$OUT/nds_core.o.new"
else
  mv "$OUT/nds_core.o.new" "$OUT/nds_core.o"
fi
ls -la "$OUT/nds_core.o"
