#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Vita3K's libretro core (WizzardSK/vita3k-libretro, branch libretro, GPL-2.0; upstream
# Vita3K v0.2.1 build 4250) into one object for EmuSuNi: the PS Vita, experimental. EmuSuNi's own
# port: nothing here comes from another PS4 port of Vita3K.
#
# State (phase 2, 2026-11-13): both builds end in the object. The PC one is tested (firmware install,
# homebrew, a commercial game) on the PS4's ways of the guest memory, the page faults and the JIT's
# code memory (port/ps4_vita_sys.c, on stand-ins for the kernel's calls). The PS4 one compiles and
# isn't linked into the app yet, so nothing of it has run on the console.
#
#   VITA_CORE_HOST=1 build-vita-core.sh <out dir>   this PC (Linux, clang 21): <out>/vita_core.o
#                                                   and <out>/vita_core_test.so (the object linked
#                                                   with this PC's libraries, for the test frontend:
#                                                   local/herramientas/prueba-vita/vitafront.cpp
#                                                   --prefix vita3k_).
#   build-vita-core.sh <out dir>                    the PS4 (toolchain/ps4-love-modern.cmake).
#   ... <out dir> clean                             everything again.
#   VITA_CORE_NOKEYS=1 ... <its own out dir>        the core without Sony's keys inside (scripts/build.sh
#                                                   with-vita-nokeys): Vita3K's source has them written
#                                                   in it, and this build takes them from a file of the
#                                                   player's instead (port/ps4_vita_keys.cpp,
#                                                   scripts/make-vita-keys.py). <out>/vita-keys.sha256:
#                                                   the SHA-256 it takes that file by, for the app.
#
# The sources: ~/vita-port/src/vita3k-libretro (VITA3K_DIR), cloned with its submodules and at the
# commit below. The patches are applied each time to the files as git has them, each one checked.
# All of them are under the CMake option EMUSUNI_PORT (the macro of that name in the sources).
#   - One object: the core is a static library (upstream: a shared object with a version script),
#     linked here with ld.lld -r, its libretro API renamed vita3k_retro_* and everything else local.
#   - No OpenSSL, no curl: the decryption of games and firmware goes through EmuSuNi's own EVP layer
#     over Mbed TLS (port/ps4_vita_evp.c, checked by local/herramientas/prueba-vita/evptest.sh); the
#     Vita's TLS and the emulator's HTTP client answer that there is no network.
#   - No network for the games either (the owner's rule: no connection to Sony's servers, ever): the
#     core's calls that would connect, send, take a connection or ask a name server are linked over
#     with port/ps4_vita_nonet.c's, which fail; checked at the end, with the object's texts.
#   - The guest's 4 GiB, its page faults and the recompiler's code memory through port/ps4_vita_sys.c
#     (checked by local/herramientas/prueba-vita/systest.sh): a reservation with direct memory in
#     2 MiB pieces under the pages in use, the PS4's signal numbers and context, a block of code
#     memory for each guest thread's recompiler (16 MiB of addresses, with executable memory only
#     under what was written; upstream: 128 MiB of addresses each).
#     From the app the PS4 object needs emusuni_log_line (src/Log.cpp) and nothing else of its own.
#   - Every C++ exception the core throws is logged first (port/ps4_vita_throw.cpp, linked over
#     __cxa_throw): on the PS4 a throw never reaches its catch and ends the app.
#   - (01.30) The thread that prepares the content has rescue points (that file): a throw under one goes
#     back to it and the load ends with a message. And the core's std::filesystem calls go through
#     port/ps4_vita_files.cpp (linked over the C++ library's entry points, FSWRAP below): remove_all and
#     remove are its own - the C++ library's need unlinkat and fdopendir, which the PS4 doesn't have as
#     the app's C library expects them: 01.29 ended there, in the firmware's install -, and the others
#     never throw from inside the C++ library.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${VITA3K_DIR:-$HOME/vita-port/src/vita3k-libretro}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/vita-core.log"
HOST="${VITA_CORE_HOST:-0}"
# (16-11: 4 jobs at most, the rule for every heavy job on the PC after its blue screens under load; and run this
# script under `flock /root/heavy-job.lock`, one heavy job at a time.)
JOBS="${VITA_CORE_JOBS:-4}"

VITA3K_COMMIT=87666afd9e84e1a59ec07d495bab4dc114f0798a
TREE_COMMIT="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)"
if [ "$TREE_COMMIT" != "$VITA3K_COMMIT" ]; then
  echo "Vita3K: $SRC isn't at $VITA3K_COMMIT (it is at ${TREE_COMMIT:-nothing}):"
  echo "  git clone --recursive -b libretro https://github.com/WizzardSK/vita3k-libretro.git $SRC"
  echo "  git -C $SRC checkout $VITA3K_COMMIT && git -C $SRC submodule update --init --recursive"
  exit 1
fi

python3 -I - "$SRC" <<'PATCH'
import re
import subprocess
import sys
root = sys.argv[1]
def git_file(path, repo):
    # (repo: the submodule the file is in, its path from the root; git shows a file of its own tree only.)
    inside = path[len(repo) + 1:] if repo else path
    run = subprocess.run(['git', '-C', root + ('/' + repo if repo else ''), 'show', 'HEAD:' + inside],
                         capture_output=True)
    if run.returncode != 0:
        sys.exit("Vita3K: %s isn't in the commit of %s" % (path, root))
    return run.stdout.decode('utf-8').replace('\r\n', '\n')
PATCHED = {}  # (by repository - '' the tree's own, else a submodule's path -: the files this script patches)
def patch(path, edits, repo='', tables=()):
    PATCHED.setdefault(repo, set()).add(path[len(repo) + 1:] if repo else path)
    text = git_file(path, repo)
    for old, new in edits:
        if text.count(old) != 1:
            sys.exit("Vita3K: %s: expected once, found %d times:\n%s" % (path, text.count(old), old))
        text = text.replace(old, new)
    # (tables: a table of bytes found by its name - "... name[..] = { ... };" - with what stands for it in a
    # build without Sony's keys, EMUSUNI_VITA_NOKEYS. Its bytes are never written in this script.)
    for name, without in tables:
        found = re.findall(r'^[^\n;{}]*\b' + name + r'\b[^\n;=]*= *\{[^{};]*\};', text, flags=re.M)
        if len(found) != 1:
            sys.exit("Vita3K: %s: expected the table %s once, found it %d times" % (path, name, len(found)))
        text = text.replace(found[0], '#ifdef EMUSUNI_VITA_NOKEYS\n' + without + '\n#else\n' + found[0] + '\n#endif')
    full = root + '/' + path
    try:
        with open(full, encoding='utf-8', newline='') as f:
            if f.read() == text:
                return  # as it is: its time stamp stays, nothing compiles again
    except OSError:
        pass
    with open(full, 'w', encoding='utf-8', newline='') as f:
        f.write(text)

# (For a build without Sony's keys, EMUSUNI_VITA_NOKEYS: port/ps4_vita_keys.h's calls, said in each source that
# uses them - not every library of Vita3K's has the port's folder among its headers.)
KEYS_API = ('#ifdef EMUSUNI_VITA_NOKEYS\n'
            'extern "C" int emusuni_vita_keys_load(const char *path);\n'
            'extern "C" int emusuni_vita_keys_missing(const char *what);\n'
            '#endif\n')
# (01.29) The steps of a game's start, each in the log and on the disk before it is taken (port/ps4_vita_sys.c):
# the 01.26's first tries on a console ended without a signal nor a crash.log (the console closes the app so
# for a fault of the GPU's), and the log's last line was all there was to go by.
STEP_API = ('#ifdef EMUSUNI_PORT\n'
            'extern "C" void emusuni_vita_step(const char *format, ...);\n'
            'extern "C" void emusuni_vita_sync(void);\n'
            '#endif\n')
# (01.30) The rescue points (port/ps4_vita_throw.cpp): where a thread goes back to when a C++ exception is thrown
# under it on the PS4, whose catch is never reached. See that file for how they are used.
RESCUE_API = ('#ifdef EMUSUNI_PORT\n'
              '#include <csetjmp>\n'
              'extern "C" std::jmp_buf *emusuni_vita_rescue_push(const char *where);\n'
              'extern "C" int emusuni_vita_rescue_depth(void);\n'
              'extern "C" void emusuni_vita_rescue_leave(int depth);\n'
              'extern "C" const char *emusuni_vita_rescued(void);\n'
              'struct EmusuniVitaRescueScope {\n'
              '    int depth = emusuni_vita_rescue_depth();\n'
              '    ~EmusuniVitaRescueScope() { emusuni_vita_rescue_leave(depth); }\n'
              '};\n'
              '#endif\n')
# (01.31) What the game really does, in the log every 10 s (port/ps4_vita_stats.cpp): the app's own speed line
# measures retro_run, which in this core only counts the screen's refreshes - 01.30's console log said 100% of
# two games their owner called very slow. `what`: 0 a picture the game shows, 1 a block the recompiler makes,
# 2 a shader, 3 a pipeline, 4 a file opened, 5 a command list the renderer works on, 6 a thread's code memory
# full and emptied.
STAT_API = ('#ifdef EMUSUNI_PORT\n'
            'extern "C" unsigned long long emusuni_vita_stat_now(void);\n'
            'extern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks);\n'
            'extern "C" unsigned long long emusuni_vita_stat_level_begin(const char *name, int id, int top);\n'
            'extern "C" void emusuni_vita_stat_level_end(int top, unsigned long long outer, unsigned long long ticks);\n'
            'extern "C" void emusuni_vita_stat_guest(unsigned long long guest, unsigned long long call, int called);\n'
            'extern "C" void emusuni_vita_stat_start(void);\n'
            'extern "C" void emusuni_vita_stat_tick(void);\n'
            'extern "C" void emusuni_vita_stat_tl(int which);\n'
            'extern "C" void emusuni_vita_stat_pad(unsigned buttons);\n'
            'struct EmusuniVitaStatScope {\n'
            '    const int what;\n'
            '    const unsigned long long from = emusuni_vita_stat_now();\n'
            '    explicit EmusuniVitaStatScope(int what) : what(what) {}\n'
            '    ~EmusuniVitaStatScope() { emusuni_vita_stat_add(what, emusuni_vita_stat_now() - from); }\n'
            '};\n'
            '#endif\n')
# (01.34) The speed switches (port/ps4_vita_sys.h says what each one is and how a file turns it on or off):
# 0 fastfp, 1 bigjit, 2 framequeue, 3 fastatomics, 4 hints, 5 filewalk, 6 copybarrier, 7 mxcsr, 8 texsync,
# 9 accuracy, 10 syncshaders, 11 gpubarrier, 12 centertouch, 13 keepfastmem, 14 nativelib, 15 fastcalls,
# 16 cpuopt, 17 ngsdefer, 18 offline, 19 mic, 20 drawcopy, 21 freshshaders, 22 vertexw, 23 vertexalign,
# 24 condrelease, 25 zipmarks, 26 uniformlog, 27 vertexrelay, 28 coreturns, 29 monitor64, 30 lwfast, 31 queuelimits, 32 negp2, 33 loopcap, 34 intclamp, 35 fastgxm, 36 ngsresample, 37 res2x, 38 netbind, 39 fastids, 40 msgauto, 41 surfacepick, 42 gxmfast, 43 rawhalves, 44 movpclr, 45 nzcvword, 46 vfp64, 47 gamefn, 48 gxmring,
# 49 gxmringcount, 50 gxmpacket, 51 stubcall, 52 callstats, 53 iolock, 54 uniformcopy, 55 stubgxm, 56 sixcores, 57 texbatch, 58 vertexrange, 59 texviewport, 60 submitthread, 61 presentnowait, 62 surfacewatch. Said here for the sources that haven't the port's folder among their headers.
SWITCH_API = ('#ifdef EMUSUNI_PORT\n'
              'extern "C" int emusuni_vita_switch(int which);\n'
              'extern "C" const char *emusuni_vita_test_tick(void);\n'
              'extern "C" int emusuni_vita_jit_tight(const void *at);\n'
              'extern "C" void emusuni_vita_log(const char *format, ...);\n'
              'extern "C" void emusuni_vita_game(const char *title);\n'
              '#endif\n')

# (After 01.70) The "vertexrelay" switch (27, ON): a vertex stream that holds numbers of 2 or more bytes at a place
# inside the vertex that is not a multiple of their size is laid out anew - each attribute at the next multiple of
# 4 bytes, in the order they have in the vertex, each with the bytes the pipeline reads for it. HENkaku-PS4's notes
# say the PS4's GPU reads such numbers wrong when the vertices are an odd number of bytes apart or the number is at
# an odd place; the first half was seen on the console (01.70: Street Fighter X Tekken's menus right with the
# "vertexalign" padding), the second can't be yet - God of War, the one game of the bench with such vertices (15
# and 19 bytes, 16-bit numbers at odd places), doesn't get past its launcher there. The pipeline (where each
# attribute is, how far apart the vertices are) and the draw (the stream copied so) take the same layout from
# this function. (Only what the stream's own formats say is looked at to tell an odd place: an attribute the
# shader reads as wider numbers than the stream says is not seen.)
RELAY_FN = ('#ifdef EMUSUNI_PORT\n'
            '// (EmuSuNi, the "vertexrelay" switch) Where each attribute of a stream goes when the stream is laid out anew.\n'
            'struct EmusuniRelay {\n'
            '    bool relaid = false;\n'
            '    uint32_t stride = 0;\n'
            '    uint16_t offsets[32] = {}; // (by the attribute\'s place in the program\'s list)\n'
            '    uint16_t sizes[32] = {};\n'
            '};\n'
            'static inline EmusuniRelay emusuni_relay(const SceGxmVertexProgram &vertex_program, unsigned stream_index) {\n'
            '    EmusuniRelay relay;\n'
            '    const auto &attributes = vertex_program.attributes;\n'
            '    const size_t count = attributes.size();\n'
            '    bool odd = false;\n'
            '    for (size_t a = 0; a < count; a++) {\n'
            '        if (attributes[a].streamIndex != stream_index)\n'
            '            continue;\n'
            '        const uint32_t part = static_cast<uint32_t>(gxm::attribute_format_size(static_cast<SceGxmAttributeFormat>(attributes[a].format)));\n'
            '        odd = odd || (part >= 2 && attributes[a].offset % part != 0);\n'
            '    }\n'
            '    if (!odd || count > 32 || !emusuni_vita_switch(27))\n'
            '        return relay;\n'
            '    // (the bytes the pipeline reads for each attribute: PipelineCache::get_vertex_input_state)\n'
            '    const auto *const program = vertex_program.renderer_data.get();\n'
            '    for (size_t a = 0; a < count; a++) {\n'
            '        if (attributes[a].streamIndex != stream_index)\n'
            '            continue;\n'
            '        uint32_t part = static_cast<uint32_t>(gxm::attribute_format_size(static_cast<SceGxmAttributeFormat>(attributes[a].format)));\n'
            '        uint32_t components = attributes[a].componentCount;\n'
            '        if (program) {\n'
            '            const auto info = program->attribute_infos.find(attributes[a].regIndex);\n'
            '            if (info == program->attribute_infos.end())\n'
            '                continue; // (the shader doesn\'t read it)\n'
            '            if (info->second.regformat) {\n'
            '                components = info->second.component_count;\n'
            '                switch (info->second.gxm_type) {\n'
            '                case SCE_GXM_PARAMETER_TYPE_U8:\n'
            '                case SCE_GXM_PARAMETER_TYPE_S8:\n'
            '                    part = 1;\n'
            '                    break;\n'
            '                case SCE_GXM_PARAMETER_TYPE_C10:\n'
            '                    part = 1;\n'
            '                    components = (components * 10 + 7) / 8;\n'
            '                    break;\n'
            '                case SCE_GXM_PARAMETER_TYPE_U16:\n'
            '                case SCE_GXM_PARAMETER_TYPE_S16:\n'
            '                case SCE_GXM_PARAMETER_TYPE_F16:\n'
            '                    part = 2;\n'
            '                    break;\n'
            '                default:\n'
            '                    part = 4;\n'
            '                    break;\n'
            '                }\n'
            '                if (components > 4)\n'
            '                    components = ((components + 3) / 4) * 4;\n'
            '            }\n'
            '        }\n'
            '        relay.sizes[a] = static_cast<uint16_t>(part * components);\n'
            '    }\n'
            '    uint32_t at = 0;\n'
            '    bool placed[32] = {};\n'
            '    for (;;) {\n'
            '        size_t next = count;\n'
            '        for (size_t a = 0; a < count; a++) {\n'
            '            if (relay.sizes[a] != 0 && !placed[a] && (next == count || attributes[a].offset < attributes[next].offset))\n'
            '                next = a;\n'
            '        }\n'
            '        if (next == count)\n'
            '            break;\n'
            '        placed[next] = true;\n'
            '        relay.offsets[next] = static_cast<uint16_t>(at);\n'
            '        at += (relay.sizes[next] + 3u) & ~3u;\n'
            '    }\n'
            '    relay.relaid = at != 0;\n'
            '    relay.stride = at;\n'
            '    return relay;\n'
            '}\n'
            '#endif\n')

# One object for the app: a static library, none of the shared object's link options.
patch('vita3k/libretro/CMakeLists.txt', [
    ('add_library(vita3k_libretro SHARED\n',
     'option(EMUSUNI_PORT "EmuSuNi: the core as a static library, to be linked into one object" OFF)\n'
     'if(EMUSUNI_PORT)\n\tset(VITA3K_LIBRETRO_KIND STATIC)\nelse()\n\tset(VITA3K_LIBRETRO_KIND SHARED)\nendif()\n'
     'add_library(vita3k_libretro ${VITA3K_LIBRETRO_KIND}\n'),
    ('if(LINUX)\n\ttarget_link_libraries(vita3k_libretro PRIVATE -static-libgcc -static-libstdc++)',
     'if(LINUX AND NOT EMUSUNI_PORT)\n\ttarget_link_libraries(vita3k_libretro PRIVATE -static-libgcc -static-libstdc++)'),
])

# SDL3 (threads, timers, the gamepad, sensor, camera and audio stream types the core's own code is
# written with): the PS4 toolchain's system is "Generic", where SDL's CMake knows no threads. For SDL
# alone the PS4 is a Unix console: pthreads, the Unix timers and file system, dummy drivers for the rest.
patch('external/CMakeLists.txt', [
    ('add_subdirectory(sdl)\n',
     'if(EMUSUNI_PORT AND NOT LINUX)\n'
     '\tset(UNIX 1)\n'
     '\tforeach(emusuni_sdl_off SDL_ALSA SDL_PULSEAUDIO SDL_PIPEWIRE SDL_JACK SDL_SNDIO SDL_OSS SDL_DISKAUDIO\n'
     '\t\tSDL_LIBUDEV SDL_DBUS SDL_IBUS SDL_FCITX SDL_LIBURING SDL_KMSDRM SDL_RPI SDL_VIVANTE SDL_OFFSCREEN SDL_OPENGL\n'
     '\t\tSDL_OPENGLES SDL_VULKAN SDL_OPENVR SDL_HIDAPI SDL_HIDAPI_LIBUSB SDL_HIDAPI_JOYSTICK SDL_VIRTUAL_JOYSTICK\n'
     '\t\tSDL_RENDER SDL_GPU SDL_HAPTIC SDL_DIALOG SDL_TRAY SDL_PROCESS SDL_SHARED SDL_TESTS SDL_EXAMPLES SDL_INSTALL\n'
     '\t\tSDL_CAMERA_V4L2 SDL_LIBTHAI SDL_TEST_LIBRARY)\n'
     '\t\tset(${emusuni_sdl_off} OFF CACHE BOOL "" FORCE)\n'
     '\tendforeach()\n'
     '\tset(SDL_STATIC ON CACHE BOOL "" FORCE)\n'
     'endif()\n'
     'add_subdirectory(sdl)\n'
     'if(EMUSUNI_PORT AND NOT LINUX)\n'
     '\tunset(UNIX)\n'
     'endif()\n'),
    # OpenSSL: not looked for. Its EVP interface is EmuSuNi's layer over Mbed TLS (port/ps4_vita_evp.c
    # with port/vita-openssl; AES, SHA-1 and SHA-256 from the sources of the app's Mbed TLS), its TLS
    # the header that answers "no TLS" (port/vita-openssl/openssl/ssl.h).
    ('if(NOT FORCE_BUILD_OPENSSL_MAC)\n\tfind_package(OpenSSL QUIET)\nendif()\n',
     'if(NOT FORCE_BUILD_OPENSSL_MAC AND NOT EMUSUNI_PORT)\n\tfind_package(OpenSSL QUIET)\nendif()\n'),
    ('if(NOT OPENSSL_FOUND OR FORCE_BUILD_OPENSSL_MAC)\n',
     'if((NOT OPENSSL_FOUND OR FORCE_BUILD_OPENSSL_MAC) AND NOT EMUSUNI_PORT)\n'),
    ('add_library(ssl INTERFACE)\n'
     'target_link_libraries(ssl INTERFACE OpenSSL::SSL)\n'
     'add_library(crypto INTERFACE)\n'
     'target_link_libraries(crypto INTERFACE OpenSSL::Crypto)\n',
     'if(EMUSUNI_PORT)\n'
     '\tadd_library(emusuni_evp STATIC\n'
     '\t\t"${EMUSUNI_PORT_DIR}/ps4_vita_evp.c"\n'
     '\t\t"${EMUSUNI_MBEDTLS_DIR}/library/aes.c" "${EMUSUNI_MBEDTLS_DIR}/library/aesni.c"\n'
     '\t\t"${EMUSUNI_MBEDTLS_DIR}/library/sha1.c" "${EMUSUNI_MBEDTLS_DIR}/library/sha256.c"\n'
     '\t\t"${EMUSUNI_MBEDTLS_DIR}/library/platform_util.c")\n'
     '\ttarget_include_directories(emusuni_evp PUBLIC "${EMUSUNI_PORT_DIR}/vita-openssl"\n'
     '\t\tPRIVATE "${EMUSUNI_MBEDTLS_DIR}/include")\n'
     '\tadd_library(OpenSSL::Crypto ALIAS emusuni_evp)\n'
     '\t# The guest memory, its page faults and the recompiler\'s code memory on the PS4\'s kernel calls.\n'
     '\t# (And the log line for every C++ exception the core throws, linked over __cxa_throw below.)\n'
     '\t# (And the keys from a file of the player\'s, in a build without them: nothing in the others.)\n'
     '\t# (And the Vita without a network: the core\'s calls that would connect, linked over them below.)\n'
     '\t# (And the core\'s std::filesystem calls: its deleting on what the PS4 has, none throwing from the C++ library.)\n'
     '\tadd_library(emusuni_vita_sys STATIC "${EMUSUNI_PORT_DIR}/ps4_vita_sys.c"\n'
     '\t\t"${EMUSUNI_PORT_DIR}/ps4_vita_throw.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_keys.cpp"\n'
     '\t\t"${EMUSUNI_PORT_DIR}/ps4_vita_nonet.c" "${EMUSUNI_PORT_DIR}/ps4_vita_files.cpp"\n'
     '\t\t"${EMUSUNI_PORT_DIR}/ps4_vita_stats.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_native.cpp"\n'
     '\t\t"${EMUSUNI_PORT_DIR}/ps4_vita_lwfast.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_jitopt.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_place.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_profile.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_gxmring.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_gxmpacket.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_gamefn.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_gxmfast.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_fastids.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_ngsresample.cpp" "${EMUSUNI_PORT_DIR}/ps4_vita_stubcall.cpp")\n'
     '\ttarget_include_directories(emusuni_vita_sys PUBLIC "${EMUSUNI_PORT_DIR}")\n'
     '\ttarget_link_libraries(emusuni_vita_sys PRIVATE emusuni_evp)\n'
     '\tadd_library(ssl INTERFACE)\n'
     '\ttarget_link_libraries(ssl INTERFACE emusuni_evp)\n'
     '\tadd_library(crypto INTERFACE)\n'
     '\ttarget_link_libraries(crypto INTERFACE emusuni_evp)\n'
     'else()\n'
     '\tadd_library(ssl INTERFACE)\n'
     '\ttarget_link_libraries(ssl INTERFACE OpenSSL::SSL)\n'
     '\tadd_library(crypto INTERFACE)\n'
     '\ttarget_link_libraries(crypto INTERFACE OpenSSL::Crypto)\n'
     'endif()\n'),
    # curl (the emulator's own HTTP client: update checks and the like): not looked for, not downloaded.
    ('find_package(CURL QUIET)\nif(NOT CURL_FOUND)\n',
     'if(NOT EMUSUNI_PORT)\n\tfind_package(CURL QUIET)\nendif()\n'
     'if(EMUSUNI_PORT)\n\tadd_library(libcurl INTERFACE)\nelseif(NOT CURL_FOUND)\n'),
    # The file dialog library: nothing of the libretro core uses it, and it knows no system like the PS4.
    ('if(NOT ANDROID)\n\t# nativefiledialog-extended\n',
     'if(NOT ANDROID AND NOT EMUSUNI_PORT)\n\t# nativefiledialog-extended\n'),
])

# The guest's memory (port/ps4_vita_sys.c). Upstream reserves 4 GiB with mmap(PROT_NONE) and turns
# pages on and off with mprotect, which the PS4's kernel can't do. Here: the reservation, the pages in
# use, their access and the page fault handler are the port's, with 16 KiB pages on the PC too (the
# PS4's), so that the PC tests walk the same paths. A range outside the guest's memory (a buffer of
# the GPU's mapped through the page table, which the PS4 doesn't use) is protected as before.
patch('vita3k/mem/CMakeLists.txt', [
    ('target_link_libraries(mem PUBLIC util)\n',
     'target_link_libraries(mem PUBLIC util)\n'
     'if(EMUSUNI_PORT)\n\ttarget_link_libraries(mem PUBLIC emusuni_vita_sys)\nendif()\n'),
])
patch('vita3k/mem/src/mem.cpp', [
    ('#include <algorithm>\n#include <cassert>\n',
     '#ifdef EMUSUNI_PORT\n#include <ps4_vita_sys.h>\n#endif\n\n#include <algorithm>\n#include <cassert>\n'),
    ('bool init(MemState &state, const bool use_page_table) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '// EmuSuNi: the access of a range (0 none, 1 read, 3 read and write).\n'
     'static int emusuni_protect(MemState &state, uint8_t *start, size_t size, int access) {\n'
     '    const uintptr_t base = reinterpret_cast<uintptr_t>(state.memory.get());\n'
     '    const uintptr_t at = reinterpret_cast<uintptr_t>(start);\n'
     '    if (at >= base && at + size <= base + TOTAL_MEM_SIZE)\n'
     '        return emusuni_vita_mem_protect(at - base, size, access) ? 0 : -1;\n'
     '    return mprotect(start, size, access == 0 ? PROT_NONE : (access == 1 ? PROT_READ : (PROT_READ | PROT_WRITE)));\n'
     '}\n'
     '#endif\n\n'
     'bool init(MemState &state, const bool use_page_table) {\n'),
    ('    state.host_page_size = static_cast<int>(sysconf(_SC_PAGESIZE));\n',
     '#ifdef EMUSUNI_PORT\n'
     '    state.host_page_size = EMUSUNI_VITA_PAGE;\n'
     '#else\n'
     '    state.host_page_size = static_cast<int>(sysconf(_SC_PAGESIZE));\n'
     '#endif\n'),
    ('    state.memory = Memory(static_cast<uint8_t *>(mmap(preferred_address, TOTAL_MEM_SIZE, prot, flags, fd, offset)), delete_memory);\n'
     '    if (state.memory.get() == MAP_FAILED) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '    (void)preferred_address, (void)prot, (void)flags, (void)fd, (void)offset;\n'
     '    // (The recompiler throws when it gets no code memory, and on the PS4 a throw ends the app: asked here.)\n'
     '    if (!emusuni_vita_jit_ready()) {\n'
     '        LOG_CRITICAL("EmuSuNi: no executable memory for the recompiler: the game cannot start");\n'
     '        return false;\n'
     '    }\n'
     '    state.memory = Memory(static_cast<uint8_t *>(emusuni_vita_mem_reserve()), delete_memory);\n'
     '    if (!state.memory) {\n'
     '        errno = ENOMEM;\n'
     '#else\n'
     '    state.memory = Memory(static_cast<uint8_t *>(mmap(preferred_address, TOTAL_MEM_SIZE, prot, flags, fd, offset)), delete_memory);\n'
     '    if (state.memory.get() == MAP_FAILED) {\n'
     '#endif\n'),
    ('    const int ret = mprotect(state.memory.get(), state.host_page_size, PROT_NONE);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const int ret = emusuni_vita_mem_protect(0, state.host_page_size, 0) ? 0 : -1;\n'
     '#else\n'
     '    const int ret = mprotect(state.memory.get(), state.host_page_size, PROT_NONE);\n'
     '#endif\n'),
    ('        munmap(memory, TOTAL_MEM_SIZE);\n',
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_mem_release();\n'
     '#else\n'
     '        munmap(memory, TOTAL_MEM_SIZE);\n'
     '#endif\n'),
    ('    const int ret = mprotect(commit_ptr, commit_size, PROT_READ | PROT_WRITE);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const int ret = emusuni_vita_mem_commit(commit_start, commit_size) ? 0 : -1;\n'
     '#else\n'
     '    const int ret = mprotect(commit_ptr, commit_size, PROT_READ | PROT_WRITE);\n'
     '#endif\n'),
    ('    const int ret = mprotect(aligned_start, aligned_size, PROT_READ | PROT_WRITE);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const int ret = emusuni_protect(state, aligned_start, aligned_size, 3);\n'
     '#else\n'
     '    const int ret = mprotect(aligned_start, aligned_size, PROT_READ | PROT_WRITE);\n'
     '#endif\n'),
    ('    const int ret = mprotect(aligned_start, aligned_size, (perm == MemPerm::None) ? PROT_NONE : ((perm == MemPerm::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE)));\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const int ret = emusuni_protect(state, aligned_start, aligned_size, (perm == MemPerm::None) ? 0 : ((perm == MemPerm::ReadOnly) ? 1 : 3));\n'
     '#else\n'
     '    const int ret = mprotect(aligned_start, aligned_size, (perm == MemPerm::None) ? PROT_NONE : ((perm == MemPerm::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE)));\n'
     '#endif\n'),
    # (The two places where free() gives host pages back: the first inside its loop, the second after it.)
    ('#else\n            int ret = mprotect(memory, batch_size, PROT_NONE);\n',
     '#elif defined(EMUSUNI_PORT)\n'
     '            (void)memory;\n'
     '            emusuni_vita_mem_decommit(batch_start, batch_size);\n'
     '#else\n            int ret = mprotect(memory, batch_size, PROT_NONE);\n'),
    ('#else\n        int ret = mprotect(memory, batch_size, PROT_NONE);\n',
     '#elif defined(EMUSUNI_PORT)\n'
     '        (void)memory;\n'
     '        emusuni_vita_mem_decommit(batch_start, batch_size);\n'
     '#else\n        int ret = mprotect(memory, batch_size, PROT_NONE);\n'),
    # The page fault handler: the port's (the PS4's signal numbers and context, SIGBUS too, the handler
    # there was before called for anything that isn't the guest's).
    ('#else\n\n// The handlers there were before ours: put back when the memory goes (the\n',
     '#elif defined(EMUSUNI_PORT)\n\n'
     'static int emusuni_fault(void *address, int write) {\n'
     '    return access_violation_handler && access_violation_handler(static_cast<uint8_t *>(address), write != 0);\n'
     '}\n\n'
     'static void register_access_violation_handler(const AccessViolationHandler &handler) {\n'
     '    access_violation_handler = handler;\n'
     '    emusuni_vita_fault_install(emusuni_fault);\n'
     '}\n\n'
     'void unregister_access_violation_handler() {\n'
     '    emusuni_vita_fault_uninstall();\n'
     '    access_violation_handler = nullptr;\n'
     '}\n\n'
     '#else\n\n// The handlers there were before ours: put back when the memory goes (the\n'),
    # (20-11) a PC test: the memory protections (EMUSUNI_VITA_TEST_SCENES)
    ('bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback) {\n    const std::lock_guard<std::mutex> lock(state.protect_mutex);\n',
     '#if !defined(__PS4__)\n#include <chrono>\n#include <cstdio>\n#include <cstdlib>\n// (20-11, a PC test: EMUSUNI_VITA_TEST_SCENES=1) the memory protections, said every 10 s\nstatic void emusuni_protect_report(MemState &state, bool merged, double us) {\n    static int on = -1;\n    if (on < 0) {\n        const char *e = getenv("EMUSUNI_VITA_TEST_SCENES");\n        on = e && *e == \'1\';\n    }\n    if (!on)\n        return;\n    static unsigned long long calls = 0, merges = 0;\n    static double spent = 0;\n    static auto last = std::chrono::steady_clock::now();\n    calls++;\n    merges += merged;\n    spent += us;\n    const auto now = std::chrono::steady_clock::now();\n    if (now - last < std::chrono::seconds(10))\n        return;\n    last = now;\n    size_t blocks = 0, most = 0;\n    for (const auto &segment : state.protect_tree) {\n        blocks += segment.second.blocks.size();\n        most = std::max(most, segment.second.blocks.size());\n    }\n    fprintf(stderr, "[emusuni-vita] test: protect: %llu calls (%llu on a range already protected), %.1f us each; "\n        "%zu ranges with %zu blocks, the most in one range %zu\\n", calls, merges, calls ? spent / calls : 0.0,\n        state.protect_tree.size(), blocks, most);\n    calls = merges = 0;\n    spent = 0;\n}\n#endif\nbool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback) {\n    const std::lock_guard<std::mutex> lock(state.protect_mutex);\n#if !defined(__PS4__)\n    const auto emusuni_from = std::chrono::steady_clock::now();\n    bool emusuni_merged = false;\n#endif\n'),
    ('        protect.blocks.merge(it->second.blocks); // transfer blocks to the new protect\n',
     '        protect.blocks.merge(it->second.blocks); // transfer blocks to the new protect\n#if !defined(__PS4__)\n        emusuni_merged = true;\n#endif\n'),
    ('    state.protect_tree.emplace(addr, std::move(protect));\n    return true;\n}\n',
     '    state.protect_tree.emplace(addr, std::move(protect));\n#if !defined(__PS4__)\n    emusuni_protect_report(state, emusuni_merged, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - emusuni_from).count());\n#endif\n    return true;\n}\n'),
    # (20-11, for 01.86) the switch "surfacewatch" (62, ON)
    ('bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback) {\n',
     '// (20-11, for 01.86, the switch "surfacewatch", 62, ON) Each colour surface made watches its memory (add_protect: a\n// CPU write marks it dirty). A surface made again in the same place - Uncharted remakes ~2 a picture, the same\n// address in another size or format - watched the range again, and the watch it had there before stayed: its\n// callback only sets the old surface\'s flags, which nothing reads any more, and the GPU\'s drawing never trips the\n// watch to clear it. So the range\'s watches piled up, ~2 a picture, and each new one merged them all: 01.84 and\n// 01.85 on the PS4, a scene\'s commands 200 -> 500 us in minutes, the scene the same (the PC protects no surfaces:\n// not seen there). HENkaku-PS4\'s "memory watch": 60 protections a second, all on a range already watched, and 11\n// ranges with 11 blocks. With the switch the slot\'s earlier watch on the range is dropped as the new one comes:\n// the same callbacks are left that would do anything.\n#include <atomic>\nstatic std::atomic<unsigned long long> s_emusuni_watch[3]; // protections, of them on a range already watched, watches dropped\nstatic MemState *s_emusuni_watch_state = nullptr;\n// The line "vita: memory watch:" (port/ps4_vita_stats.cpp): out[0-2] since the last call, out[3] the ranges, out[4]\n// their watches, out[5] the most on one range.\nextern "C" void emusuni_vita_watch_stats(unsigned long long *out) {\n    for (int i = 0; i < 3; i++)\n        out[i] = s_emusuni_watch[i].exchange(0, std::memory_order_relaxed);\n    out[3] = out[4] = out[5] = 0;\n    MemState *state = s_emusuni_watch_state;\n    if (!state)\n        return;\n    const std::lock_guard<std::mutex> lock(state->protect_mutex);\n    for (const auto &segment : state->protect_tree) {\n        out[3]++;\n        out[4] += segment.second.blocks.size();\n        out[5] = std::max<unsigned long long>(out[5], segment.second.blocks.size());\n    }\n}\nbool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback, const void *emusuni_owner) {\n'),
    ('    ProtectBlockInfo block;\n    block.size = size;\n    block.callback = callback;\n',
     '    ProtectBlockInfo block;\n    block.size = size;\n    block.callback = callback;\n    block.emusuni_owner = emusuni_owner;\n    s_emusuni_watch_state = &state;\n    s_emusuni_watch[0].fetch_add(1, std::memory_order_relaxed);\n    const bool emusuni_drop = emusuni_owner != nullptr && emusuni_vita_switch(EMUSUNI_VITA_SURFACEWATCH);\n    bool emusuni_on_watched = false;\n'),
    ('        protect.blocks.merge(it->second.blocks); // transfer blocks to the new protect\n',
     "        emusuni_on_watched = true;\n        if (emusuni_drop) {\n            // (surfacewatch) the owner's earlier watch here: its callbacks set flags nobody reads any more\n            for (auto emusuni_b = it->second.blocks.begin(); emusuni_b != it->second.blocks.end();) {\n                if (emusuni_b->second.emusuni_owner == emusuni_owner) {\n                    emusuni_b = it->second.blocks.erase(emusuni_b);\n                    s_emusuni_watch[2].fetch_add(1, std::memory_order_relaxed);\n                } else\n                    ++emusuni_b;\n            }\n        }\n        protect.blocks.merge(it->second.blocks); // transfer blocks to the new protect\n"),
    ('    protect_inner(state, addr, protect.size, protect.perm);\n',
     '    if (emusuni_on_watched)\n        s_emusuni_watch[1].fetch_add(1, std::memory_order_relaxed);\n    protect_inner(state, addr, protect.size, protect.perm);\n'),
    # (22-11, for 01.92) a fix: the memory watch line never walks a finished game's tree
    ('static MemState *s_emusuni_watch_state = nullptr;\n',
     'static MemState *s_emusuni_watch_state = nullptr;\n// (01.92, a fix) The pointer\'s own lock: deinit_mem clears the pointer under it, and the line "vita: memory watch:"\n// holds it while it walks the tree - so never the tree of a game that ended (its MemState freed afterwards). Taken\n// before a protect_mutex, never inside one.\nstatic std::mutex s_emusuni_watch_lock;\nstatic void emusuni_watch_point(MemState *state, MemState *only_if) {\n    const std::lock_guard<std::mutex> lock(s_emusuni_watch_lock);\n    if (only_if == nullptr || s_emusuni_watch_state == only_if)\n        s_emusuni_watch_state = state;\n}\n'),
    ('    out[3] = out[4] = out[5] = 0;\n    MemState *state = s_emusuni_watch_state;\n    if (!state)\n        return;\n',
     '    out[3] = out[4] = out[5] = 0;\n    const std::lock_guard<std::mutex> watch_lock(s_emusuni_watch_lock);\n    MemState *state = s_emusuni_watch_state;\n    if (!state)\n        return;\n'),
    ('bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback, const void *emusuni_owner) {\n    const std::lock_guard<std::mutex> lock(state.protect_mutex);\n',
     "bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback, const void *emusuni_owner) {\n    emusuni_watch_point(&state, nullptr); // (01.92: before the tree's lock, see s_emusuni_watch_lock)\n    const std::lock_guard<std::mutex> lock(state.protect_mutex);\n"),
    ('    s_emusuni_watch_state = &state;\n',
     ''),
    ('void deinit_mem(MemState &state) {\n    const std::lock_guard<std::mutex> gen_lock(state.generation_mutex);\n',
     'void deinit_mem(MemState &state) {\n    // (01.92, a fix) the line "vita: memory watch:" no longer walks this game\'s tree (its MemState is freed next)\n    emusuni_watch_point(nullptr, &state);\n    const std::lock_guard<std::mutex> gen_lock(state.generation_mutex);\n'),
])
# (20-11, for 01.86) the switch "surfacewatch" (62, ON): a memory watch's owner
patch('vita3k/mem/include/mem/state.h', [
    ('struct ProtectBlockInfo {\n    uint32_t size = 0;\n    ProtectCallback callback;\n',
     'struct ProtectBlockInfo {\n    uint32_t size = 0;\n    ProtectCallback callback;\n#ifdef EMUSUNI_PORT\n    // (surfacewatch, 62) whose watch this is (a surface cache slot), or none\n    const void *emusuni_owner = nullptr;\n#endif\n'),
])
# (20-11, for 01.86) the switch "surfacewatch" (62, ON): a memory watch's owner
patch('vita3k/mem/include/mem/functions.h', [
    ('bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback);\n',
     '#ifdef EMUSUNI_PORT\n// (surfacewatch, 62) owner: a watch of the same owner already on the range is dropped (see mem.cpp)\nbool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback, const void *emusuni_owner = nullptr);\n#else\nbool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback);\n#endif\n'),
])

# dynarmic, the ARM recompiler.
#   - Its fault handler (a recompiled access at an address without memory becomes a call) on the PS4
#     too: upstream builds the one that does nothing for CMake's "Generic" system. There: FreeBSD's
#     signal numbers and context whatever a header says, SIGBUS too, no signal stack of its own (the
#     app gives every thread one). As the 3DS core's (scripts/build-3ds-core.sh).
#   - Its code memory from the port's (emusuni_vita_jit_alloc: addresses, with executable memory put
#     under each 256 KiB piece when it is first written), not mmap, and never write-protected.
#   - No line on stderr for each fault it hands on (the guest memory's handler takes most of them).
patch('external/dynarmic/src/dynarmic/CMakeLists.txt', [
    ('elseif (UNIX)\n    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")\n',
     'elseif (UNIX OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")\n'),
    ('if (DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)\n',
     '# (Only its header: the library comes with Vita3K\'s memory, and dynarmic\'s targets are exported.)\n'
     'if (EMUSUNI_PORT)\n    target_include_directories(dynarmic PRIVATE "${EMUSUNI_PORT_DIR}")\nendif()\n'
     'if (DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)\n'),
], repo='external/dynarmic')
patch('external/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp', [
    ('#ifdef __APPLE__\n#    include <signal.h>\n#    include <sys/ucontext.h>\n#else\n#    include <signal.h>\n'
     '#    ifndef __OpenBSD__\n#        include <ucontext.h>\n#    endif\n#endif\n',
     '#if defined(EMUSUNI_PORT) && defined(__ORBIS__)\n'
     '#    include <signal.h>\n'
     '#    define EMUSUNI_ORBIS 1\n'
     '#    define EMUSUNI_SA_SIGINFO 0x40\n'
     '#elif defined(__APPLE__)\n#    include <signal.h>\n#    include <sys/ucontext.h>\n#else\n#    include <signal.h>\n'
     '#    ifndef __OpenBSD__\n#        include <ucontext.h>\n#    endif\n#endif\n'
     '#ifndef EMUSUNI_SA_SIGINFO\n#    define EMUSUNI_SA_SIGINFO SA_SIGINFO\n#endif\n'
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" int emusuni_vita_fault_first(void* info, void* context);\n'
     'extern "C" int emusuni_vita_fault_on_top(void);\n'
     '#endif\n'),
    # (17-11) The recompiler's handler is put in place once, by the first recompiler of the process, over the guest
    # memory's (port/ps4_vita_sys.c), which is there since the game's memory was made. That one is taken out when
    # a game ends, by putting back what was there before it - and the recompiler's goes with it, for good: the
    # second PS Vita game of a session, and any game after a reset (Uncharted's own start is one), ran without it.
    # A game then has its faults of protected pages dealt with at once, but a stray access of its code to a bad
    # address ends the app where the first game of the session only logged it. Now it is put in place again when
    # a recompiler is made and the guest memory's handler is the one on top.
    ('    static void SigAction(int sig, siginfo_t* info, void* raw_context);\n};\n',
     '    static void SigAction(int sig, siginfo_t* info, void* raw_context);\n'
     '#ifdef EMUSUNI_PORT\n'
     'public:\n'
     '    void EmusuniAgain() {\n'
     '        struct sigaction sa;\n'
     '        std::memset(&sa, 0, sizeof(sa));\n'
     '        sa.sa_sigaction = &SigHandler::SigAction;\n'
     '        sigemptyset(&sa.sa_mask);\n'
     '#    ifdef EMUSUNI_ORBIS\n'
     '        sa.sa_flags = 0x40 | 0x1;  // SA_SIGINFO | SA_ONSTACK\n'
     '        sigaction(11, &sa, &old_sa_segv);\n'
     '        sigaction(10, &sa, &old_sa_bus);\n'
     '#    else\n'
     '        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;\n'
     '        sigaction(SIGSEGV, &sa, &old_sa_segv);\n'
     '#    endif\n'
     '    }\n'
     '#endif\n'
     '};\n'),
    ('    if (!sig_handler) {\n        sig_handler.emplace();\n    }\n}\n',
     '    if (!sig_handler) {\n        sig_handler.emplace();\n    }\n'
     '#ifdef EMUSUNI_PORT\n'
     '    else if (emusuni_vita_fault_on_top()) {\n'
     '        sig_handler->EmusuniAgain();\n'
     '    }\n'
     '#endif\n'
     '}\n'),
    # (17-11) The "keepfastmem" switch (port/ps4_vita_sys.h): a fault on a page the emulator protects is the
    # emulator's first, and the instruction runs again as it is.
    ('void SigHandler::SigAction(int sig, siginfo_t* info, void* raw_context) {\n',
     'void SigHandler::SigAction(int sig, siginfo_t* info, void* raw_context) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_fault_first(info, raw_context)) {\n'
     '        return;\n'
     '    }\n'
     '#endif\n'),
    ('SigHandler::SigHandler() {\n',
     'SigHandler::SigHandler() {\n'
     '#ifdef EMUSUNI_ORBIS\n'
     '    {\n'
     '        struct sigaction sa;\n'
     '        std::memset(&sa, 0, sizeof(sa));\n'
     '        sa.sa_sigaction = &SigHandler::SigAction;\n'
     '        sa.sa_flags = 0x40 | 0x1;  // SA_SIGINFO | SA_ONSTACK\n'
     '        sigemptyset(&sa.sa_mask);\n'
     '        if (sigaction(11, &sa, &old_sa_segv) != 0 || sigaction(10, &sa, &old_sa_bus) != 0) {\n'
     '            supports_fast_mem = false;\n'
     '        }\n'
     '        return;\n'
     '    }\n'
     '#endif\n'),
    ('    ASSERT(sig == SIGSEGV || sig == SIGBUS);\n\n#ifndef MCL_ARCHITECTURE_RISCV\n',
     '#ifndef EMUSUNI_ORBIS\n    ASSERT(sig == SIGSEGV || sig == SIGBUS);\n#endif\n\n'
     '#if !defined(MCL_ARCHITECTURE_RISCV) && !defined(EMUSUNI_ORBIS)\n'),
    ('#    if defined(__APPLE__)\n#        define CTX_RIP (mctx->__ss.__rip)\n',
     '#    if defined(EMUSUNI_ORBIS)\n'
     '#        define CTX_RIP (reinterpret_cast<u64*>(static_cast<char*>(raw_context) + 0x40)[20])\n'
     '#        define CTX_RSP (reinterpret_cast<u64*>(static_cast<char*>(raw_context) + 0x40)[23])\n'
     '#    elif defined(__APPLE__)\n#        define CTX_RIP (mctx->__ss.__rip)\n'),
    ('    fmt::print(stderr, "Unhandled {} at rip {:#018x}\\n", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", CTX_RIP);\n',
     '#ifndef EMUSUNI_PORT\n'
     '    fmt::print(stderr, "Unhandled {} at rip {:#018x}\\n", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", CTX_RIP);\n'
     '#endif\n'),
    ('    if (retry_sa->sa_flags & SA_SIGINFO) {\n', '    if (retry_sa->sa_flags & EMUSUNI_SA_SIGINFO) {\n'),
], repo='external/dynarmic')
patch('external/dynarmic/src/dynarmic/backend/x64/block_of_code.cpp', [
    ('namespace Dynarmic::Backend::X64 {\n',
     '#ifdef EMUSUNI_PORT\n#    include <cstdio>\n#    include <cstdlib>\n#    include <ps4_vita_sys.h>\n#endif\n\nnamespace Dynarmic::Backend::X64 {\n'),
    ('    uint8_t* alloc(size_t size) override {\n        // Waste a page to store the size\n',
     '    uint8_t* alloc(size_t size) override {\n'
     '#    ifdef EMUSUNI_PORT\n'
     '        if (void* block = emusuni_vita_jit_alloc(size)) {\n'
     '            return static_cast<uint8_t*>(block);\n'
     '        }\n'
     '        throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);\n'
     '#    endif\n'
     '        // Waste a page to store the size\n'),
    ('    void free(uint8_t* p) override {\n        size_t size;\n',
     '    void free(uint8_t* p) override {\n'
     '#    ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_jit_free(p);\n'
     '        return;\n'
     '#    endif\n'
     '        size_t size;\n'),
    # (Its 2 MiB pool of constants is cleared when it is made: that write would put memory under all of
    # it, in every guest thread's block. New code memory holds zeros already.)
    ('    size_ += alloc_size;\n    memset(ret, 0, alloc_size);\n',
     '    size_ += alloc_size;\n#ifndef EMUSUNI_PORT\n    memset(ret, 0, alloc_size);\n#endif\n'),
    ('#    ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT\n    bool useProtect() const override { return false; }\n#    endif\n#endif\n};\n',
     '#    if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(EMUSUNI_PORT)\n'
     '    bool useProtect() const override { return false; }\n#    endif\n#endif\n};\n'),
    # (After 01.68) A test, on the PC only: EMUSUNI_VITA_TEST_PS4_CPU=1 leaves the recompiler only what the PS4's
    # CPU has. The recompiler writes different x86 code for the same ARM instruction by what the CPU it runs on
    # can do, the NEON ones above all; the bench's PC (an Intel with AVX-512) and the console (an AMD Jaguar: AVX,
    # no AVX2, no FMA, no BMI2) run different code of it - what is wrong only in the Jaguar's was never run here.
    ('    return features;\n}\n\n#ifdef __APPLE__\nbool IsUnderRosetta() {\n',
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '    if (const char* const only = getenv("EMUSUNI_VITA_TEST_PS4_CPU"); only != nullptr && only[0] == \'1\') {\n'
     '        features &= ~(HostFeature::AVX2 | HostFeature::AVX512F | HostFeature::AVX512CD | HostFeature::AVX512VL\n'
     '                      | HostFeature::AVX512BW | HostFeature::AVX512DQ | HostFeature::AVX512BITALG | HostFeature::AVX512VBMI\n'
     '                      | HostFeature::FMA | HostFeature::SHA | HostFeature::BMI2 | HostFeature::GFNI | HostFeature::FastBMI2);\n'
     '        static bool said = false;\n'
     '        if (!said) {\n'
     '            said = true;\n'
     '            fprintf(stderr, "[emusuni-vita] test: the recompiler is left only the PS4\'s CPU features (%#llx)\\n",\n'
     '                    static_cast<unsigned long long>(features));\n'
     '        }\n'
     '    }\n'
     '#endif\n'
     '    return features;\n}\n\n#ifdef __APPLE__\nbool IsUnderRosetta() {\n'),
    # (19-11, the switch "profile", port/ps4_vita_profile.cpp) A JIT's memory given back, for the console's CPU profile.
    ('#ifdef EMUSUNI_PORT\n#    include <cstdio>\n#    include <cstdlib>\n#    include <ps4_vita_sys.h>\n#endif\n\nnamespace Dynarmic::Backend::X64 {\n',
     '#ifdef EMUSUNI_PORT\n#    include <cstdio>\n#    include <cstdlib>\n#    include <ps4_vita_sys.h>\n'
     'extern "C" void emusuni_vita_prof_free(const void *code_begin);\n#endif\n\nnamespace Dynarmic::Backend::X64 {\n'),
    ('        emusuni_vita_jit_free(p);\n        return;\n',
     '        emusuni_vita_prof_free(p);\n        emusuni_vita_jit_free(p);\n        return;\n'),
], repo='external/dynarmic')
# (19-11, the switch "profile", port/ps4_vita_profile.cpp: the console's sampling CPU profile, off without the file
# /data/EmuSuNi/vita-profile-on) Where each block dynarmic translates is (its JIT's memory, its code, the guest's
# address), and each named piece of a JIT's own code (its dispatcher, its handlers, the memory accesses' slow paths):
# the profile sorts every sample of a thread's time by them.
patch('external/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp', [
    ('EmitX64::BlockDescriptor EmitX64::RegisterBlock(const IR::LocationDescriptor& descriptor, CodePtr entrypoint, size_t size) {\n'
     '    PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void emusuni_vita_prof_block(const void *code_begin, size_t code_size, const void *entry, const void *end,\n'
     '                                        unsigned long long location);\n'
     '#endif\n'
     'EmitX64::BlockDescriptor EmitX64::RegisterBlock(const IR::LocationDescriptor& descriptor, CodePtr entrypoint, size_t size) {\n'
     '    PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_prof_block(code.getCode(), code.GetTotalCodeSize(), entrypoint, code.getCurr(), descriptor.Value());\n'
     '#endif\n'),
], repo='external/dynarmic')
patch('external/dynarmic/src/dynarmic/backend/x64/perf_map.cpp', [
    ('#include <string>\n\n#ifdef __linux__\n',
     '#include <string>\n\n'
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void emusuni_vita_prof_piece(const void *start, const void *end, const char *name, size_t length);\n'
     '#endif\n\n'
     '#ifdef __linux__\n'),
    ('void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name) {\n    if (start == end) {\n',
     'void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_prof_piece(start, end, friendly_name.data(), friendly_name.size());\n'
     '#endif\n'
     '    if (start == end) {\n'),
    ('void PerfMapRegister(const void*, const void*, std::string_view) {}\n',
     '#ifdef EMUSUNI_PORT\n'
     'void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name) {\n'
     '    emusuni_vita_prof_piece(start, end, friendly_name.data(), friendly_name.size());\n'
     '}\n'
     '#else\n'
     'void PerfMapRegister(const void*, const void*, std::string_view) {}\n'
     '#endif\n'),
], repo='external/dynarmic')
# (17-11) Counted for the log's "vita: faults:" line (port/ps4_vita_stats.cpp): a fault of recompiled code the
# recompiler took for a bad address (the access then done through a call), and an instruction it made again
# with a call in its place for good.
patch('external/dynarmic/src/dynarmic/backend/x64/emit_x64_memory.cpp.inc', [
    ('FakeCall AxxEmitX64::FastmemCallback(u64 rip_) {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks);\n'
     '#endif\n\n'
     'FakeCall AxxEmitX64::FastmemCallback(u64 rip_) {\n'),
    ('    if (iter->second.recompile) {\n        const auto marker = iter->second.marker;\n        do_not_fastmem.insert(marker);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_stat_add(14, 0);\n'
     '#endif\n'
     '    if (iter->second.recompile) {\n        const auto marker = iter->second.marker;\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (do_not_fastmem.count(marker) == 0) {\n'
     '            emusuni_vita_stat_add(15, 0);\n'
     '        }\n'
     '#endif\n'
     '        do_not_fastmem.insert(marker);\n'),
], repo='external/dynarmic')
# (17-11) The system's calls by name, for the log's "vita: calls:" lines: which call a guest thread is making.
patch('vita3k/modules/module_parent.cpp', [
    ('static const ImportFn *resolve_import(uint32_t nid) {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void emusuni_vita_stat_call(unsigned nid);\n'
     'extern "C" void emusuni_vita_stat_names(const char *(*name)(unsigned));\n'
     '#endif\n\n'
     'static const ImportFn *resolve_import(uint32_t nid) {\n'),
    ('    const ImportFn *fn = resolve_import(nid);\n    if (fn) {\n        (*fn)(emuenv, cpu, thread_id);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    {\n'
     '        static const bool emusuni_named = (emusuni_vita_stat_names([](unsigned number) -> const char * { return import_name(number); }), true);\n'
     '        (void)emusuni_named;\n'
     '        emusuni_vita_stat_call(nid);\n'
     '    }\n'
     # (After 01.66, from the performance specialist's profile: 0.7-1.5% of the game threads' time went in
     # resolve_import, a switch over every number the core knows - a search of some fourteen steps for each
     # call.) What a thread has resolved stays in a small table of its own: a number's function never changes.
     '    struct EmusuniImport {\n'
     '        uint32_t nid;\n'
     '        const ImportFn *fn;\n'
     '    };\n'
     '    static thread_local EmusuniImport emusuni_imports[256] = {};\n'
     '    EmusuniImport &emusuni_import = emusuni_imports[(nid * 2654435761u) >> 24];\n'
     '    if (emusuni_import.nid != nid || emusuni_import.fn == nullptr) {\n'
     '        emusuni_import.nid = nid;\n'
     '        emusuni_import.fn = resolve_import(nid);\n'
     '    }\n'
     '    const ImportFn *fn = emusuni_import.fn;\n'
     '#else\n'
     '    const ImportFn *fn = resolve_import(nid);\n'
     '#endif\n'
     '    if (fn) {\n        (*fn)(emuenv, cpu, thread_id);\n'),
    # (19-11, for 01.83, the switch "stubcall") a number's function for the recompiler, and its call
    ('bool has_hle_implementation(uint32_t nid) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '// (19-11, the switch "stubcall", port/ps4_vita_stubcall.cpp) A number\'s function, found when an import stub\'s block\n'
     '// is translated, and its call from the recompiler\'s (without call_import\'s bookkeeping: these calls never wait).\n'
     'static EmuEnvState *emusuni_emuenv = nullptr;\n'
     'extern "C" const void *emusuni_vita_import_fn(unsigned nid) {\n'
     '    return static_cast<const void *>(resolve_import(nid));\n'
     '}\n'
     'void emusuni_vita_import_call(const void *fn, CPUState &cpu) {\n'
     '    (*static_cast<const ImportFn *>(fn))(*emusuni_emuenv, cpu, cpu.thread_id);\n'
     '}\n'
     '#endif\n'
     '\n'
     'bool has_hle_implementation(uint32_t nid) {\n'),
    ('void init_exported_vars(EmuEnvState &emuenv) {\n',
     'void init_exported_vars(EmuEnvState &emuenv) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_emuenv = &emuenv;\n'
     '#endif\n'),
])
# Each recompiler's code memory: upstream asks for 128 MiB of addresses a guest thread; here 64 MiB since 01.34
# (16 before: 01.32's console log had a thread's block full and emptied five times in 250 s, each followed by
# 10-20 s of making its code again), with all the blocks' memory kept under a limit - port/ps4_vita_sys.c.
# (01.34) And two of the speed switches: the floating point with the host's own instructions (the PS4's CPU has no
# fused multiply-add, which the exact way then does in software), and the exclusive loads and stores inline
# without the monitor every thread shares (off by default).
patch('vita3k/cpu/src/dynarmic_cpu.cpp', [
    ('#include <mem/ptr.h>\n', '#include <mem/ptr.h>\n#ifdef EMUSUNI_PORT\n#include <ps4_vita_sys.h>\n#endif\n'),
    ('    config.enable_cycle_counting = false;\n',
     '    config.enable_cycle_counting = false;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    config.code_cache_size = emusuni_vita_jit_cache_size();\n'
     '    if (cpu_opt && emusuni_vita_switch(EMUSUNI_VITA_FASTFP)) {\n'
     '        config.unsafe_optimizations = true;\n'
     '        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;\n'
     '        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;\n'
     '        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;\n'
     '        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;\n'
     '    }\n'
     '    if (cpu_opt && config.fastmem_pointer.has_value() && emusuni_vita_switch(EMUSUNI_VITA_FASTATOMICS)) {\n'
     '        config.fastmem_exclusive_access = true;\n'
     '        config.unsafe_optimizations = true;\n'
     '        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;\n'
     '    }\n'
     '#endif\n'),
    # (17-11) A supervisor call answered where it is, inside the recompiler's call for it, with no stop of the
    # recompiled code (port/ps4_vita_native.cpp: the library's long routines done natively, the system's simplest
    # calls). The kernel sets the pointer (kernel.cpp below): the CPU's library doesn't know the kernel.
    # And the memory accesses that come through a call of the emulator's, counted for the log's "vita: jit:" line.
    ('class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {\n',
     '#ifdef EMUSUNI_PORT\n'
     'bool (*emusuni_vita_inline_svc)(CPUState &cpu) = nullptr;\n'
     'extern "C" void emusuni_vita_stat_tl(int which);\n'
     '#endif\n\n'
     'class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {\n'),
    ('    void CallSVC(uint32_t svc) override {\n',
     '    void CallSVC(uint32_t svc) override {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_inline_svc != nullptr && emusuni_vita_inline_svc(*parent))\n'
     '            return;\n'
     '#endif\n'),
    ('    T MemoryRead(Dynarmic::A32::VAddr addr) {\n',
     '    T MemoryRead(Dynarmic::A32::VAddr addr) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_stat_tl(0);\n'
     '#endif\n'),
    ('    void MemoryWrite(Dynarmic::A32::VAddr addr, T value) {\n',
     '    void MemoryWrite(Dynarmic::A32::VAddr addr, T value) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_stat_tl(0);\n'
     '#endif\n'),
    # (19-11, the switch "gamefn", port/ps4_vita_gamefn.cpp) A block that starts at one of a game's hot functions
    # done natively: the native function on the guest's r0-r2 (it writes r0 in the JitState) and a return.
    ('class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void *emusuni_vita_gamefn_find(int thumb, uint32_t pc, const unsigned char *base);\n'
     '#endif\n'
     'class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {\n'),
    ('    void CallSVC(uint32_t svc) override {\n',
     '#ifdef EMUSUNI_PORT\n'
     '    bool PreCodeReadHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {\n'
     '        if (!ir.block.empty())\n'
     '            return true;\n'
     '        void *const fn = emusuni_vita_gamefn_find(is_thumb ? 1 : 0, pc, parent->mem->memory.get());\n'
     '        if (fn == nullptr)\n'
     '            return true;\n'
     '        ir.CallHostFunction(reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint64_t)>(fn),\n'
     '            ir.Imm64(reinterpret_cast<std::uint64_t>(cpu->jit->Regs().data())),\n'
     '            ir.Imm64(reinterpret_cast<std::uint64_t>(parent->mem->memory.get())), ir.Imm64(0));\n'
     '        ir.BXWritePC(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))); // (LR)\n'
     '        ir.SetTerm(Dynarmic::IR::Term::PopRSBHint{});\n'
     '        return false;\n'
     '    }\n'
     '#endif\n'
     '    void CallSVC(uint32_t svc) override {\n'),
    # (19-11, 01.83, the performance specialist's) gamefn's block covers its function's bytes: a change of that code
    # (an invalidation of the range) finds it. Before, its range was empty.
    ('extern "C" void *emusuni_vita_gamefn_find(int thumb, uint32_t pc, const unsigned char *base);\n',
     'extern "C" void *emusuni_vita_gamefn_find(int thumb, uint32_t pc, const unsigned char *base);\n'
     'extern "C" unsigned emusuni_vita_gamefn_bytes(const void *fn);\n'),
    ('        ir.BXWritePC(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))); // (LR)\n'
     '        ir.SetTerm(Dynarmic::IR::Term::PopRSBHint{});\n',
     '        ir.BXWritePC(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))); // (LR)\n'
     '        ir.SetTerm(Dynarmic::IR::Term::PopRSBHint{});\n'
     '        ir.current_location = ir.current_location.AdvancePC(static_cast<int>(emusuni_vita_gamefn_bytes(fn)));\n'),
    # (19-11, for 01.83, the switch "stubcall", port/ps4_vita_stubcall.cpp) An import stub's block made as one call of its function, found when it is translated.
    ('class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {\n',
     '#ifdef EMUSUNI_PORT\n'
     '// (19-11, for 01.83, the switch "stubcall": port/ps4_vita_stubcall.cpp; kernel.cpp\'s emusuni_stub_plan and\n'
     '// emusuni_stub_run set these) How a block that starts at an import stub is made, and the run of a slot\'s call.\n'
     'struct EmusuniStubPlan {\n'
     '    int how; // 1 a call of the slot\'s (a supervisor call that carries it), 2 the thread\'s own id or TLS block\n'
     '    uint32_t slot;\n'
     '    uint64_t value;\n'
     '    uint64_t arg;\n'
     '};\n'
     'bool (*emusuni_vita_stub_plan)(CPUState &cpu, uint32_t nid, EmusuniStubPlan &plan) = nullptr;\n'
     'bool (*emusuni_vita_stub_call)(CPUState &cpu, uint32_t slot) = nullptr;\n'
     'extern "C" void emusuni_vita_stub_ids(std::uint64_t regs, std::uint64_t value, std::uint64_t nid_kind);\n'
     '#endif\n'
     'class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {\n'),
    ('        void *const fn = emusuni_vita_gamefn_find(is_thumb ? 1 : 0, pc, parent->mem->memory.get());\n',
     '        // (stubcall) A block that starts at an import stub ("svc #0; mov pc, lr; <number>", ARM code): one call of\n'
     '        // the function found now - for sceKernelGetThreadId/GetTLSAddr the thread\'s own numbers, written in -, then\n'
     '        // the return to the caller, as "mov pc, lr" does. The block covers the stub\'s three words, so a stub written\n'
     '        // again later (a module loaded) takes it away. The supervisor call switches the floating point\'s state as\n'
     '        // the stub\'s own did; the thread\'s numbers need no switch.\n'
     '        if (!is_thumb && emusuni_vita_stub_plan != nullptr && pc >= parent->mem->host_page_size\n'
     '            && Ptr<uint32_t>(pc).valid(*parent->mem) && Ptr<uint32_t>(pc + 8).valid(*parent->mem)) {\n'
     '            const uint32_t *const words = Ptr<uint32_t>(pc).get(*parent->mem);\n'
     '            EmusuniStubPlan plan{};\n'
     '            if (words[0] == 0xef000000u && words[1] == 0xe1a0f00eu && emusuni_vita_stub_plan(*parent, words[2], plan)) {\n'
     '                if (plan.how == 2) {\n'
     '                    ir.CallHostFunction(reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint64_t)>(&emusuni_vita_stub_ids),\n'
     '                        ir.Imm64(reinterpret_cast<std::uint64_t>(cpu->jit->Regs().data())), ir.Imm64(plan.value), ir.Imm64(plan.arg));\n'
     '                } else {\n'
     '                    ir.BranchWritePC(ir.Imm32(pc + 4));\n'
     '                    ir.CallSupervisor(ir.Imm32(0x80000000u | plan.slot));\n'
     '                }\n'
     '                ir.BXWritePC(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))); // (LR)\n'
     '                ir.SetTerm(Dynarmic::IR::Term::CheckHalt{ Dynarmic::IR::Term::PopRSBHint{} });\n'
     '                ir.current_location = ir.current_location.AdvancePC(12);\n'
     '                return false;\n'
     '            }\n'
     '        }\n'
     '        void *const fn = emusuni_vita_gamefn_find(is_thumb ? 1 : 0, pc, parent->mem->memory.get());\n'),
    ('        if (emusuni_vita_inline_svc != nullptr && emusuni_vita_inline_svc(*parent))\n',
     '        // (stubcall) The supervisor call of a stub\'s block made by PreCodeReadHook: bit 31 and the slot (a game\'s\n'
     '        // stub always says 0). What the slot doesn\'t answer here - a call that waits, runs a callback or ends the\n'
     '        // thread, a lightweight mutex\'s lock that has to wait - goes to the guest thread\'s loop as any call, which\n'
     '        // takes the number from the slot (thread.cpp); the program counter is the caller\'s return address by then,\n'
     '        // so the thread goes back to its caller after the call.\n'
     '        if (svc >= 0x80000000u) {\n'
     '            if (emusuni_vita_stub_call != nullptr && emusuni_vita_stub_call(*parent, svc & 0x7fffffffu))\n'
     '                return;\n'
     '            parent->svc_called = true;\n'
     '            parent->svc = svc;\n'
     '            cpu->jit->HaltExecution(Dynarmic::HaltReason::UserDefined8);\n'
     '            return;\n'
     '        }\n'
     '        if (emusuni_vita_inline_svc != nullptr && emusuni_vita_inline_svc(*parent))\n'),
])
# The loader's side of the native routines: the stubs of the modules that import one stay supervisor calls, where
# upstream writes a jump to the routine once the module that has it (the game's libc.suprx) is loaded.
# And, for a profile on the PC (local/herramientas/prueba-vita/vprof.sh), each exported function's address.
patch('vita3k/kernel/src/load_self.cpp', [
    ('static bool load_func_imports(',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" int emusuni_vita_native_stub(unsigned nid);\n'
     'extern "C" void emusuni_vita_log(const char *format, ...);\n'
     '#endif\n\n'
     'static bool load_func_imports('),
    ('        if (!func_address) {\n            stub[0] = 0xef000000; // svc #0 - Call our interrupt hook.\n',
     '#ifdef EMUSUNI_PORT\n'
     '        if (!func_address || emusuni_vita_native_stub(nid)) {\n'
     '#else\n'
     '        if (!func_address) {\n'
     '#endif\n'
     '            stub[0] = 0xef000000; // svc #0 - Call our interrupt hook.\n'),
    ('            if (it->second.library_nid != library_nid)\n                continue;\n'
     '            auto address = it->second.entry_address;\n',
     '            if (it->second.library_nid != library_nid)\n                continue;\n'
     '#ifdef EMUSUNI_PORT\n'
     '            if (emusuni_vita_native_stub(nid))\n'
     '                continue;\n'
     '#endif\n'
     '            auto address = it->second.entry_address;\n'),
    ('            kernel.invalidate_jit_cache(address, 3 * sizeof(uint32_t));\n        }\n\n'
     '        if (kernel.debugger.log_exports) {\n',
     '            kernel.invalidate_jit_cache(address, 3 * sizeof(uint32_t));\n        }\n\n'
     '#ifdef EMUSUNI_PORT\n'
     '        {\n'
     '            static const bool emusuni_exports = getenv("EMUSUNI_VITA_EXPORTS") != nullptr;\n'
     '            if (emusuni_exports)\n'
     '                emusuni_vita_log("vita: export: %#x %s %s", static_cast<unsigned>(entry.address()), import_name(nid), kernel_module_info->module_name);\n'
     '        }\n'
     '#endif\n'
     '        if (kernel.debugger.log_exports) {\n'),
])
# The app's Vulkan device (one for the whole app, made by src/Display.cpp; the core's own
# create_device is never called). The core then takes the device as it is, with memory mapping and
# its optional extensions off, but reads which features it may use from the GPU, not from the
# device: the app turns on five of the seven it reads (fillModeNonSolid, samplerAnisotropy,
# fragmentStoresAndAtomics, shaderStorageImageExtendedFormats, shaderInt16) and not wide lines nor
# precise occlusion queries, which the renderer does without when they are off. And "disabled" is
# the default of the memory mapping option, so that no message says at each start that the method
# asked for isn't there (the PS4's driver hangs with buffer device addresses, which the others need).
patch('vita3k/renderer/src/vulkan/renderer.cpp', [
    ('    if (!support.created)\n'
     '        LOG_WARN("Vulkan: RetroArch made the device itself, so memory mapping and the optional extensions are off");\n',
     '    if (!support.created)\n'
     '        LOG_WARN("Vulkan: RetroArch made the device itself, so memory mapping and the optional extensions are off");\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (!support.created) {\n'
     '        s.physical_device_features.wideLines = VK_FALSE;\n'
     '        s.physical_device_features.occlusionQueryPrecise = VK_FALSE;\n'
     '    }\n'
     '#endif\n'),
    # (20-11) the renderer's line, "vita: renderer:" (port/ps4_vita_stats.cpp)
    ("namespace renderer::vulkan {\n\n// RetroArch's queue is shared with the frontend, which must be told around\n",
     '#ifdef EMUSUNI_PORT\nextern "C" unsigned long long emusuni_vita_stat_now(void);\nextern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks);\nstruct EmusuniVitaStatScope {\n    const int what;\n    const unsigned long long from = emusuni_vita_stat_now();\n    explicit EmusuniVitaStatScope(int what) : what(what) {}\n    ~EmusuniVitaStatScope() { emusuni_vita_stat_add(what, emusuni_vita_stat_now() - from); }\n};\n#endif\nnamespace renderer::vulkan {\n\n// RetroArch\'s queue is shared with the frontend, which must be told around\n'),
    ('void VKState::locked_queue_submit(vk::Queue queue, const vk::SubmitInfo &submit_info, vk::Fence fence) {\n    if (libretro_lock_queue && libretro_queue_handle) {\n        libretro_lock_queue(libretro_queue_handle);\n',
     "void VKState::locked_queue_submit(vk::Queue queue, const vk::SubmitInfo &submit_info, vk::Fence fence) {\n#ifdef EMUSUNI_PORT\n    // (20-11) a submission's time (52), and in it the wait for the frontend's queue lock (53)\n    const EmusuniVitaStatScope emusuni_stat_submit(52);\n#endif\n    if (libretro_lock_queue && libretro_queue_handle) {\n#ifdef EMUSUNI_PORT\n        const unsigned long long emusuni_lock_from = emusuni_vita_stat_now();\n#endif\n        libretro_lock_queue(libretro_queue_handle);\n#ifdef EMUSUNI_PORT\n        emusuni_vita_stat_add(53, emusuni_vita_stat_now() - emusuni_lock_from);\n#endif\n"),
    # (20-11, for 01.84) the switch "texviewport" (59, OFF)
    ('static bool libretro_adopt_device(VKState &s, bool &support_dedicated_allocations) {\n',
     '#ifdef EMUSUNI_PORT\nextern "C" int emusuni_vita_switch(int which);\nextern "C" void emusuni_vita_log(const char *format, ...);\n#endif\nstatic bool libretro_adopt_device(VKState &s, bool &support_dedicated_allocations) {\n'),
    ('#ifdef EMUSUNI_PORT\n    if (!support.created) {\n        s.physical_device_features.wideLines = VK_FALSE;\n        s.physical_device_features.occlusionQueryPrecise = VK_FALSE;\n    }\n#endif\n',
     '#ifdef EMUSUNI_PORT\n    if (!support.created) {\n        s.physical_device_features.wideLines = VK_FALSE;\n        s.physical_device_features.occlusionQueryPrecise = VK_FALSE;\n    }\n    // (20-11, for 01.84, the switch "texviewport", 59, OFF) The app makes the device itself (Granite, the N64\n    // core\'s Vulkan context: it enables every feature of its chain the GPU offers, uniform buffers in the standard\n    // layout among them - the console\'s log: "Enabling device extension: VK_KHR_uniform_buffer_standard_layout"),\n    // so the core took none of the optional features, and with no standard layout no texture viewports: each part of\n    // a painted surface a texture reads was copied for it (Uncharted\'s first chapter on the bench: 12 copies a\n    // picture, 70% of them parts of the surface being drawn on; HENkaku-PS4, its own device: 2). With the switch,\n    // the standard layout is taken when the GPU offers it, and texture viewports with it (renderer.cpp\'s\n    // late_init: "using texture viewport").\n    if (!support.created && !s.support_standard_layout && emusuni_vita_switch(59)) {\n        bool emusuni_has = false;\n        for (const vk::ExtensionProperties &emusuni_ext : s.physical_device.enumerateDeviceExtensionProperties())\n            if (std::string_view(emusuni_ext.extensionName.data()) == vk::KHRUniformBufferStandardLayoutExtensionName)\n                emusuni_has = true;\n        if (emusuni_has) {\n            const auto emusuni_features = s.physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceUniformBufferStandardLayoutFeatures>();\n            emusuni_has = static_cast<bool>(emusuni_features.get<vk::PhysicalDeviceUniformBufferStandardLayoutFeatures>().uniformBufferStandardLayout);\n        }\n        s.support_standard_layout = emusuni_has;\n        emusuni_vita_log("vita: texviewport: the GPU %s uniform buffers in the standard layout: texture viewports %s (the file vita-texviewport-on)",\n            emusuni_has ? "has" : "has not", emusuni_has ? "on - a part of a painted surface sampled in place, not copied" : "stay off");\n    }\n#endif\n'),
    # (20-11, for 01.85) the switches "submitthread" (60) and "presentnowait" (61), OFF
    ('    vk_state->libretro_unlock_queue = unlock_queue;\n}\n',
     '    vk_state->libretro_unlock_queue = unlock_queue;\n#ifdef EMUSUNI_PORT\n    emusuni_submit_take_lock(*vk_state);\n#endif\n}\n'),
    ('void set_libretro_queue_lock(renderer::State *state, void *handle,\n',
     '#ifdef EMUSUNI_PORT\nstatic void emusuni_submit_take_lock(renderer::vulkan::VKState &state);\n#endif\nvoid set_libretro_queue_lock(renderer::State *state, void *handle,\n'),
    ('    ~EmusuniVitaStatScope() { emusuni_vita_stat_add(what, emusuni_vita_stat_now() - from); }\n};\n#endif\nnamespace renderer::vulkan {\n',
     '    ~EmusuniVitaStatScope() { emusuni_vita_stat_add(what, emusuni_vita_stat_now() - from); }\n};\n// (20-11, for 01.85, the switch "submitthread", 60, OFF) The core\'s submissions to the GPU made on a thread of\n// their own, in their order. The PS4\'s GPU driver (DolphinPS4\'s RADV winsys, ac_ps4_drm.c) makes two kernel calls\n// for each one on the thread that submits - sceGnmSubmitCommandBuffers (about 0.4 ms: Crema\'s own measure of it)\n// and sceGnmSubmitDone -, and here under the frontend\'s queue lock: Uncharted\'s renderer waited 0.58 ms for each,\n// 14 a picture (01.84\'s log). HENkaku-PS4 (since its 1.08, with Crema\'s winsys) makes those calls on a thread of\n// the driver\'s: its renderer sees 45 us. Here the renderer only queues the submission; the thread takes the\n// frontend\'s lock, submits, lets the lock go. What waits for the GPU first waits for the submissions it needs to\n// have been made: a fence\'s wait, status or reset for the job carrying it; the waits for the queue or the device\n// to be idle, and the core\'s own taking of the frontend\'s lock (its submissions outside this path: a render\n// target\'s first layouts, a surface read back), for all the jobs queued.\n#include <condition_variable>\n#include <deque>\n#include <thread>\nextern "C" int emusuni_vita_switch(int which);\nextern "C" void emusuni_vita_log(const char *format, ...);\nextern "C" void emusuni_vita_sixcores_here(int who);\nnamespace {\nstruct EmusuniSubmitJob {\n    VkQueue queue = VK_NULL_HANDLE;\n    VkFence fence = VK_NULL_HANDLE;\n    uint64_t number = 0;\n    void *handle = nullptr;\n    void (*lock)(void *) = nullptr;\n    void (*unlock)(void *) = nullptr;\n    std::vector<VkCommandBuffer> commands;\n    std::vector<VkSemaphore> waits;\n    std::vector<VkPipelineStageFlags> stages;\n    std::vector<VkSemaphore> signals;\n};\nstruct EmusuniSubmitter {\n    std::mutex lock;\n    std::condition_variable ready, made_one;\n    std::deque<EmusuniSubmitJob> jobs;\n    std::deque<std::pair<VkFence, uint64_t>> fences; // the queued jobs\' fences, with their job\'s number\n    uint64_t queued = 0, made = 0; // the jobs queued, and of them the ones submitted (in order)\n    bool on = false, started = false;\n    // the frontend\'s queue lock (retro_hw_render_interface_vulkan) as it gave it\n    void *handle = nullptr;\n    void (*queue_lock)(void *) = nullptr;\n    void (*queue_unlock)(void *) = nullptr;\n    // the driver\'s functions, under the waits put in their place in the dispatcher\n    PFN_vkQueueSubmit submit = nullptr;\n    PFN_vkWaitForFences wait_for_fences = nullptr;\n    PFN_vkGetFenceStatus get_fence_status = nullptr;\n    PFN_vkResetFences reset_fences = nullptr;\n    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;\n    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;\n};\nEmusuniSubmitter s_emusuni_submit;\n// This thread has the frontend\'s queue lock through the core\'s own taking of it: what it waits for then must not\n// need the submission thread, which would be waiting for the lock.\nthread_local bool s_emusuni_holds_queue = false;\n\nvoid emusuni_submit_loop() {\n    emusuni_vita_sixcores_here(1);\n    EmusuniSubmitter &s = s_emusuni_submit;\n    std::unique_lock<std::mutex> hold(s.lock);\n    for (;;) {\n        s.ready.wait(hold, [&s] { return !s.jobs.empty(); });\n        EmusuniSubmitJob job = std::move(s.jobs.front());\n        s.jobs.pop_front();\n        hold.unlock();\n        VkSubmitInfo info{};\n        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;\n        info.waitSemaphoreCount = static_cast<uint32_t>(job.waits.size());\n        info.pWaitSemaphores = job.waits.data();\n        info.pWaitDstStageMask = job.stages.data();\n        info.commandBufferCount = static_cast<uint32_t>(job.commands.size());\n        info.pCommandBuffers = job.commands.data();\n        info.signalSemaphoreCount = static_cast<uint32_t>(job.signals.size());\n        info.pSignalSemaphores = job.signals.data();\n        VkResult result;\n        {\n            // (the line "vita: renderer:": a submission\'s time, 52, and its wait for the lock, 53)\n            const EmusuniVitaStatScope stat(52);\n            if (job.lock && job.handle) {\n                const unsigned long long from = emusuni_vita_stat_now();\n                job.lock(job.handle);\n                emusuni_vita_stat_add(53, emusuni_vita_stat_now() - from);\n            }\n            result = s.submit(job.queue, 1, &info, job.fence);\n            if (job.unlock && job.handle)\n                job.unlock(job.handle);\n        }\n        if (result != VK_SUCCESS) {\n            static int said = 0;\n            if (said++ < 4)\n                emusuni_vita_log("vita: submitthread: a submission to the GPU failed (%d)", static_cast<int>(result));\n        }\n        hold.lock();\n        s.made = job.number;\n        while (!s.fences.empty() && s.fences.front().second <= s.made)\n            s.fences.pop_front();\n        s.made_one.notify_all();\n    }\n}\n\n// Waits until the jobs carrying these fences have been submitted (no fences: all the jobs queued). Its time: 54.\nvoid emusuni_submit_wait(uint32_t count, const VkFence *fences) {\n    EmusuniSubmitter &s = s_emusuni_submit;\n    if (s_emusuni_holds_queue)\n        return;\n    std::unique_lock<std::mutex> hold(s.lock);\n    uint64_t need = 0;\n    if (!fences)\n        need = s.queued;\n    else\n        for (const auto &queued : s.fences)\n            for (uint32_t i = 0; i < count; i++)\n                if (queued.first == fences[i])\n                    need = std::max(need, queued.second);\n    if (need <= s.made)\n        return;\n    const unsigned long long from = emusuni_vita_stat_now();\n    s.made_one.wait(hold, [&s, need] { return s.made >= need; });\n    emusuni_vita_stat_add(54, emusuni_vita_stat_now() - from);\n}\n\n// Queues a submission for the thread.\nvoid emusuni_submit_queue(VkQueue queue, const vk::SubmitInfo &info, VkFence fence) {\n    EmusuniSubmitter &s = s_emusuni_submit;\n    EmusuniSubmitJob job;\n    job.queue = queue;\n    job.fence = fence;\n    job.handle = s.handle;\n    job.lock = s.queue_lock;\n    job.unlock = s.queue_unlock;\n    const auto *commands = reinterpret_cast<const VkCommandBuffer *>(info.pCommandBuffers);\n    job.commands.assign(commands, commands + info.commandBufferCount);\n    const auto *waits = reinterpret_cast<const VkSemaphore *>(info.pWaitSemaphores);\n    job.waits.assign(waits, waits + info.waitSemaphoreCount);\n    const auto *stages = reinterpret_cast<const VkPipelineStageFlags *>(info.pWaitDstStageMask);\n    if (stages)\n        job.stages.assign(stages, stages + info.waitSemaphoreCount);\n    const auto *signals = reinterpret_cast<const VkSemaphore *>(info.pSignalSemaphores);\n    job.signals.assign(signals, signals + info.signalSemaphoreCount);\n    std::unique_lock<std::mutex> hold(s.lock);\n    if (!s.started) {\n        s.started = true;\n        std::thread(emusuni_submit_loop).detach();\n    }\n    // (at most 64 waiting: the renderer is not let run far ahead of the GPU\'s driver)\n    s.made_one.wait(hold, [&s] { return s.queued - s.made < 64; });\n    job.number = ++s.queued;\n    if (fence != VK_NULL_HANDLE)\n        s.fences.emplace_back(fence, job.number);\n    s.jobs.push_back(std::move(job));\n    hold.unlock();\n    s.ready.notify_one();\n}\n\nVKAPI_ATTR VkResult VKAPI_CALL emusuni_wait_for_fences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {\n    emusuni_submit_wait(count, fences);\n    return s_emusuni_submit.wait_for_fences(device, count, fences, all, timeout);\n}\nVKAPI_ATTR VkResult VKAPI_CALL emusuni_get_fence_status(VkDevice device, VkFence fence) {\n    emusuni_submit_wait(1, &fence);\n    return s_emusuni_submit.get_fence_status(device, fence);\n}\nVKAPI_ATTR VkResult VKAPI_CALL emusuni_reset_fences(VkDevice device, uint32_t count, const VkFence *fences) {\n    emusuni_submit_wait(count, fences);\n    return s_emusuni_submit.reset_fences(device, count, fences);\n}\nVKAPI_ATTR VkResult VKAPI_CALL emusuni_queue_wait_idle(VkQueue queue) {\n    emusuni_submit_wait(0, nullptr);\n    return s_emusuni_submit.queue_wait_idle(queue);\n}\nVKAPI_ATTR VkResult VKAPI_CALL emusuni_device_wait_idle(VkDevice device) {\n    emusuni_submit_wait(0, nullptr);\n    return s_emusuni_submit.device_wait_idle(device);\n}\n// The core\'s own taking of the frontend\'s queue lock: after the jobs queued.\nvoid emusuni_lock_after_jobs(void *handle) {\n    emusuni_submit_wait(0, nullptr);\n    s_emusuni_submit.queue_lock(handle);\n    s_emusuni_holds_queue = true;\n}\nvoid emusuni_unlock_after_jobs(void *handle) {\n    s_emusuni_holds_queue = false;\n    s_emusuni_submit.queue_unlock(handle);\n}\n} // namespace\n\nstatic void emusuni_submit_take_lock(renderer::vulkan::VKState &state) {\n    EmusuniSubmitter &s = s_emusuni_submit;\n    if (!s.on || !state.libretro_lock_queue || state.libretro_lock_queue == emusuni_lock_after_jobs)\n        return;\n    s.handle = state.libretro_queue_handle;\n    s.queue_lock = state.libretro_lock_queue;\n    s.queue_unlock = state.libretro_unlock_queue;\n    state.libretro_lock_queue = emusuni_lock_after_jobs;\n    state.libretro_unlock_queue = emusuni_unlock_after_jobs;\n}\n\n// At the renderer\'s start (libretro_adopt_device, the dispatcher just loaded): the switch looked at; ON, the waits\n// put in the dispatcher in place of the driver\'s, and the core\'s taking of the queue lock wrapped.\nstatic void emusuni_submit_install(renderer::vulkan::VKState &state) {\n    EmusuniSubmitter &s = s_emusuni_submit;\n    emusuni_submit_wait(0, nullptr); // (a game before this one: all its submissions made)\n    s.on = emusuni_vita_switch(60) != 0;\n    if (!s.on)\n        return;\n    auto &d = VULKAN_HPP_DEFAULT_DISPATCHER;\n    if (d.vkWaitForFences != emusuni_wait_for_fences) {\n        s.submit = d.vkQueueSubmit;\n        s.wait_for_fences = d.vkWaitForFences;\n        s.get_fence_status = d.vkGetFenceStatus;\n        s.reset_fences = d.vkResetFences;\n        s.queue_wait_idle = d.vkQueueWaitIdle;\n        s.device_wait_idle = d.vkDeviceWaitIdle;\n        d.vkWaitForFences = emusuni_wait_for_fences;\n        d.vkGetFenceStatus = emusuni_get_fence_status;\n        d.vkResetFences = emusuni_reset_fences;\n        d.vkQueueWaitIdle = emusuni_queue_wait_idle;\n        d.vkDeviceWaitIdle = emusuni_device_wait_idle;\n    }\n    emusuni_submit_take_lock(state);\n    emusuni_vita_log("vita: submitthread: the core\'s submissions to the GPU made on a thread of their own, in order: the "\n                     "renderer only queues them (the file vita-submitthread-on)");\n}\n\n// (presentnowait, 61) A fence\'s submission made (not its GPU work done): libretro_vita3k.cpp\'s copy for the frontend.\nextern "C" void emusuni_vita_submitted(void *fence) {\n    const VkFence f = reinterpret_cast<VkFence>(fence);\n    emusuni_submit_wait(1, &f);\n}\n#endif\nnamespace renderer::vulkan {\n'),
    ('void VKState::locked_queue_submit(vk::Queue queue, const vk::SubmitInfo &submit_info, vk::Fence fence) {\n#ifdef EMUSUNI_PORT\n',
     'void VKState::locked_queue_submit(vk::Queue queue, const vk::SubmitInfo &submit_info, vk::Fence fence) {\n#ifdef EMUSUNI_PORT\n    // (20-11, for 01.85, the switch "submitthread", 60) queued for the submission thread; one it can\'t take (a\n    // chain after the submit info) made here after all the queued ones\n    if (s_emusuni_submit.on) {\n        if (!submit_info.pNext && libretro_lock_queue == emusuni_lock_after_jobs) {\n            emusuni_submit_queue(static_cast<VkQueue>(queue), submit_info, static_cast<VkFence>(fence));\n            return;\n        }\n        emusuni_submit_wait(0, nullptr);\n    }\n'),
    ('    VULKAN_HPP_DEFAULT_DISPATCHER.init(s.device);\n\n    s.physical_device_properties = s.physical_device.getProperties();\n',
     '    VULKAN_HPP_DEFAULT_DISPATCHER.init(s.device);\n#ifdef EMUSUNI_PORT\n    emusuni_submit_install(s);\n#endif\n\n    s.physical_device_properties = s.physical_device.getProperties();\n'),
    # (20-11, for 01.86) the switch "surfacewatch" (62, ON)
    ('        || std::string_view(physical_device_properties.deviceName).contains("NVIDIA");\n#endif\n',
     '        || std::string_view(physical_device_properties.deviceName).contains("NVIDIA");\n#endif\n#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n    // (20-11, a PC test: EMUSUNI_VITA_TEST_MPROTECT=1) the surfaces\' memory watched as on the PS4, where this stays\n    // true (not Linux): the surfacewatch switch\'s case\n    if (const char *emusuni_e = getenv("EMUSUNI_VITA_TEST_MPROTECT"); emusuni_e && *emusuni_e == \'1\')\n        surface_cache.can_mprotect_mapped_memory = true;\n#endif\n'),
])
patch('vita3k/libretro/libretro_core_options.h', [
    ('            { "page-table", "Page Table" },\n            { NULL, NULL },\n        },\n        "double-buffer"\n',
     '            { "page-table", "Page Table" },\n            { NULL, NULL },\n        },\n'
     '#ifdef EMUSUNI_PORT\n        "disabled"\n#else\n        "double-buffer"\n#endif\n'),
])

# glslang's table of what each SPIR-V instruction looks like (for its disassembler, which Vita3K calls
# only to dump a shader): 6 MiB of the .bss, which the PS4 charges the app at its start, game or no
# game. Here: made the first time it is asked for, from the heap.
patch('external/glslang/SPIRV/doc.h', [
    ('extern InstructionParameters InstructionDesc[];\n',
     '#ifdef EMUSUNI_PORT\n'
     'InstructionParameters* EmusuniInstructionDesc();\n'
     '#define InstructionDesc EmusuniInstructionDesc()\n'
     '#else\n'
     'extern InstructionParameters InstructionDesc[];\n'
     '#endif\n'),
], repo='external/glslang')
patch('external/glslang/SPIRV/doc.cpp', [
    ('InstructionParameters InstructionDesc[OpCodeMask + 1];\n',
     '#ifdef EMUSUNI_PORT\n'
     'InstructionParameters* EmusuniInstructionDesc() {\n'
     '    static InstructionParameters* const table = new InstructionParameters[OpCodeMask + 1];\n'
     '    return table;\n'
     '}\n'
     '#else\n'
     'InstructionParameters InstructionDesc[OpCodeMask + 1];\n'
     '#endif\n'),
], repo='external/glslang')

# A variable each thread has its own of (the shader disassembler's output), declared for other files without
# saying that it needs no code to start: the compiler then asks, before each use, for a function that starts
# it, by a weak symbol nothing defines. On a PC that is zero and skipped; the PS4's executable maker
# (create-fself) takes it for an import no module has, and makes no executable. Said: constinit.
patch('vita3k/shader/include/shader/usse_disasm.h', [
    ('extern thread_local std::stringstream *disasm_storage;\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern constinit thread_local std::stringstream *disasm_storage;\n'
     '#else\n'
     'extern thread_local std::stringstream *disasm_storage;\n'
     '#endif\n'),
])
patch('vita3k/shader/src/usse_disasm.cpp', [
    ('thread_local std::stringstream *disasm_storage = nullptr;\n',
     '#ifdef EMUSUNI_PORT\n'
     'constinit thread_local std::stringstream *disasm_storage = nullptr;\n'
     '#else\n'
     'thread_local std::stringstream *disasm_storage = nullptr;\n'
     '#endif\n'),
    # (the "negp2" switch, below)
    ('    case ExtPredicate::PN: return "pN ";\n',
     '    case ExtPredicate::PN: return "pN ";\n'
     '#ifdef EMUSUNI_PORT\n'
     '    case ExtPredicate::NEGP2: return "!p2 ";\n'
     '#endif\n'),
])
# (After 01.74) Three fixes of the translator of the Vita's shader programs, each with its switch, and the saved
# shaders' version changed once for the three: a game's shaders are all made again at its first start with them.
#  - "negp2" (32, ON). A vector instruction can run on "not p2"; the instructions' common list of conditions has no
#    such entry, and upstream made those instructions unconditional (a TODO of its own, with a line in the log:
#    "ExtVecPredicate::NEGP2 case hit" - Mortal Kombat's shaders give it 18 times a start on the bench). Here the
#    condition is kept: an entry after the others (their numbers are the instructions' own), read as p2 negated,
#    and a write to p2 ends a block that runs on it. HENkaku-PS4's notes tell of a loop of Mortal Kombat's that
#    never ended on the PS4's GPU for this (read, its code not taken).
#  - "loopcap" (33, ON). A translated loop ends after 4096 turns at most: a counter of its own in each loop. A loop
#    that never ends is a GPU that never finishes, and the PS4 closes the app for that without a trace.
#  - "intclamp" (34, ON). A number made an integer of 8 or 16 bits is first held inside what that type can hold.
#    Out of it the conversion is whatever the GPU does - not the same on the PC's and on the PS4's. Vita3K's pull
#    request 4220 (open) has it; this is that part of it.
patch('vita3k/shader/include/shader/usse_types.h', [
    ('namespace shader::usse {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     '#endif\n\n'
     'namespace shader::usse {\n'),
    ('    NEGP0,\n    NEGP1,\n    PN\n};\n\nenum class ExtVecPredicate : uint8_t {\n',
     '    NEGP0,\n    NEGP1,\n    PN,\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, the "negp2" switch) "not p2", which only a vector instruction can say.\n'
     '    NEGP2,\n'
     '#endif\n'
     '};\n\nenum class ExtVecPredicate : uint8_t {\n'),
    ('    case ExtVecPredicate::NEGP2:\n        // TODO\n',
     '    case ExtVecPredicate::NEGP2:\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_switch(32))\n'
     '            return ExtPredicate::NEGP2;\n'
     '#endif\n'
     '        // TODO\n'),
])
patch('vita3k/shader/src/usse_program_analyzer.cpp', [
    ('                    is_predicate_invalidated = ((predicate_writed_to + 1) == current_code->condition) || ((predicate_writed_to + 5) == current_code->condition);\n',
     '                    is_predicate_invalidated = ((predicate_writed_to + 1) == current_code->condition) || ((predicate_writed_to + 5) == current_code->condition);\n'
     '#ifdef EMUSUNI_PORT\n'
     '                    if (predicate_writed_to == 2 && current_code->condition == static_cast<std::uint8_t>(ExtPredicate::NEGP2))\n'
     '                        is_predicate_invalidated = true;\n'
     '#endif\n'),
])
patch('vita3k/shader/src/usse_translator_entry.cpp', [
    # (A test, on the PC only: EMUSUNI_VITA_TEST_LOOP=1. None of the bench's games has a shader with a loop: an
    # empty one that never ends goes before each program's own code, made by the function that translates a game's
    # loops - only the "loopcap" switch gets a shader out of it. With EMUSUNI_VITA_FRESHSHADERS=1, so that the
    # shaders are made anew; the game's saved shaders are to be removed after such a run.)
    ('#include <optional>\n', '#include <optional>\n#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n#include <cstdlib>\n#include <memory>\n#endif\n'),
    ('    compile_block(tree_block_node);\n\n    b.leaveFunction();\n',
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '    {\n'
     '        static const char *const emusuni_test_loop = getenv("EMUSUNI_VITA_TEST_LOOP");\n'
     '        if (emusuni_test_loop && emusuni_test_loop[0] == \'1\') {\n'
     '            usse::USSEBlockNode emusuni_root(nullptr, 0);\n'
     '            usse::USSELoopNode emusuni_loop(&emusuni_root, 0);\n'
     '            usse::USSEBaseNodeInstance emusuni_body = std::make_unique<usse::USSEBlockNode>(&emusuni_loop, 0);\n'
     '            emusuni_loop.set_content_block(emusuni_body);\n'
     '            compile_loop_node(emusuni_loop);\n'
     '        }\n'
     '    }\n'
     '#endif\n'
     '    compile_block(tree_block_node);\n\n    b.leaveFunction();\n'),
    ('        do_neg = !do_neg;\n    }\n',
     '        do_neg = !do_neg;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    } else if (predicator == ExtPredicate::NEGP2) {\n'
     '        pred_opr.num = 2;\n'
     '        do_neg = !do_neg;\n'
     '#endif\n'
     '    }\n'),
    ('    spv::Builder::LoopBlocks loops = b.makeNewLoop();\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (the "loopcap" switch) This loop\'s own count of its turns.\n'
     '    spv::Id emusuni_turns = spv::NoResult;\n'
     '    if (emusuni_vita_switch(33)) {\n'
     '        emusuni_turns = b.createVariable(spv::NoPrecision, spv::StorageClassFunction, b.makeIntType(32), "emusuni_loop_turns");\n'
     '        b.createStore(b.makeIntConstant(0), emusuni_turns);\n'
     '    }\n'
     '#endif\n'
     '    spv::Builder::LoopBlocks loops = b.makeNewLoop();\n'),
    ('    // If true\n    const usse::USSEBlockNode &content_block = *(loop.content_block());\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_turns != spv::NoResult) {\n'
     '        // one more turn; after 4096 the loop is left, as a break of the game\'s own would leave it\n'
     '        const spv::Id emusuni_now = b.createBinOp(spv::OpIAdd, b.makeIntType(32), b.createLoad(emusuni_turns, spv::NoPrecision), b.makeIntConstant(1));\n'
     '        b.createStore(emusuni_now, emusuni_turns);\n'
     '        spv::Builder::If emusuni_over(b.createBinOp(spv::OpSGreaterThan, b.makeBoolType(), emusuni_now, b.makeIntConstant(4096)), spv::SelectionControlMaskNone, b);\n'
     '        b.createLoopExit();\n'
     '        emusuni_over.makeEndIf();\n'
     '    }\n'
     '#endif\n'
     '    // If true\n    const usse::USSEBlockNode &content_block = *(loop.content_block());\n'),
])
patch('vita3k/shader/src/usse_utilities.cpp', [
    ('    if (!is_uint) {\n        opr = b.createUnaryOp(spv::OpConvertFToS, target_type, opr);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (the "intclamp" switch) Held inside what the type can hold before it is made an integer.\n'
     '    if (emusuni_vita_switch(34) && type != DataType::INT32 && type != DataType::UINT32 && type != DataType::C10) {\n'
     '        const float emusuni_top = get_int_normalize_range_constants(type);\n'
     '        const float emusuni_bottom = is_uint ? 0.0f : -(emusuni_top + 1.0f);\n'
     '        opr = b.createBuiltinCall(opr_type, utils.std_builtins, GLSLstd450FClamp,\n'
     '            { opr, create_constant_vector_or_scalar(b, b.makeFloatConstant(emusuni_bottom), comp_count),\n'
     '                create_constant_vector_or_scalar(b, b.makeFloatConstant(emusuni_top), comp_count) });\n'
     '    }\n'
     '#endif\n'
     '    if (!is_uint) {\n        opr = b.createUnaryOp(spv::OpConvertFToS, target_type, opr);\n'),
])
# (After 01.78: Resistance - Burning Skies' 3D pictures, red and blue with no green, on the console and on the
# bench) The "rawhalves" switch (43, ON) - port/ps4_vita_sys.h says what it is. A program that declares its output
# as two unsigned 32-bit words ("the raw content") was given the register's four words as four floats, from the
# register's start: on a surface of four half floats each word must be two halves with its bits, as the programs
# that declare four halves already get, and from where the program says the output starts (two words on in six of
# that game's seventeen such programs).
patch('vita3k/shader/src/spirv_recompiler.cpp', [
    ('namespace shader {\n', SWITCH_API + 'namespace shader {\n'),
    ('    // if the shader tries to write a INT32 or UINT32, it means the raw content\n'
     '    if (color_val_operand.type == DataType::INT32 || color_val_operand.type == DataType::UINT32)\n'
     '        color_val_operand.type = DataType::F32;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const bool emusuni_raw_halves = (color_val_operand.type == DataType::INT32 || color_val_operand.type == DataType::UINT32)\n'
     '        && vertex_varyings_ptr->output_comp_count == 2 && emusuni_vita_switch(43) != 0\n'
     '        && gxm::get_base_format(translate_state.hints->color_format) == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16;\n'
     '    if (emusuni_raw_halves)\n'
     '        color_val_operand.type = DataType::F16;\n'
     '#endif\n'
     '    // if the shader tries to write a INT32 or UINT32, it means the raw content\n'
     '    if (color_val_operand.type == DataType::INT32 || color_val_operand.type == DataType::UINT32)\n'
     '        color_val_operand.type = DataType::F32;\n'),
    ('    if (!program.is_native_color() && vertex_varyings_ptr->output_param_type == 1) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (!program.is_native_color() && (vertex_varyings_ptr->output_param_type == 1 || emusuni_raw_halves)) {\n'
     '#else\n'
     '    if (!program.is_native_color() && vertex_varyings_ptr->output_param_type == 1) {\n'
     '#endif\n'),
])
# (The saved shaders' version: upstream's is 15 at this tree's commit and its open pull requests take 16. A number
# of our own, so that no shader made before these three fixes is read again.)
patch('vita3k/shader/include/shader/spirv_recompiler.h', [
    ('static constexpr uint32_t CURRENT_VERSION = 15;\n',
     '#ifdef EMUSUNI_PORT\n'
     'static constexpr uint32_t CURRENT_VERSION = 1502;\n'
     '#else\n'
     'static constexpr uint32_t CURRENT_VERSION = 15;\n'
     '#endif\n'),
])

# The guest's threads, each a thread of the system: 1 MiB of stack (what upstream's get on Windows),
# not the system's default. The PS4 gives 2 MiB and 16 KiB to a thread that doesn't say, out of the
# games' flexible memory, and keeps each stack for the next thread of the same size: the first game
# tried had more than 30 of them at once.
patch('vita3k/kernel/src/kernel.cpp', [
    # (After 01.71) Each thread a game makes, in the log: its name, its priority and the Vita's cores it may run on
    # (0x10000, 0x20000, 0x40000; 0: the game's default). God of War Collection's launcher stops on the console
    # with two threads of Sony's file library each holding the mutex the other waits for; on a Vita two threads
    # tied to one core never run at once, here every thread has a core of the PS4's to itself.
    ('    ThreadStatePtr thread = std::make_shared<ThreadState>(id, *this, mem, std::move(cpu), name, entry_point.address(), init_priority, affinity_mask, stack_size, attr, option);\n',
     '    ThreadStatePtr thread = std::make_shared<ThreadState>(id, *this, mem, std::move(cpu), name, entry_point.address(), init_priority, affinity_mask, stack_size, attr, option);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_thread_made(name, static_cast<int>(id), init_priority, static_cast<unsigned>(affinity_mask));\n'
     '#endif\n'),
    ('    SDL_DetachThread(SDL_CreateThread(&thread_function, thread->name.c_str(), &params));\n',
     '#ifdef EMUSUNI_PORT\n'
     '    {\n'
     '        const SDL_PropertiesID props = SDL_CreateProperties();\n'
     '        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, reinterpret_cast<void *>(&thread_function));\n'
     '        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, thread->name.c_str());\n'
     '        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &params);\n'
     '        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 1024 * 1024);\n'
     '        SDL_DetachThread(SDL_CreateThreadWithProperties(props));\n'
     '        SDL_DestroyProperties(props);\n'
     '    }\n'
     '#else\n'
     '    SDL_DetachThread(SDL_CreateThread(&thread_function, thread->name.c_str(), &params));\n'
     '#endif\n'),
    # (17-11) A supervisor call of recompiled code answered inside the recompiler's own call for it
    # (port/ps4_vita_native.cpp): one of the library's routines done natively - its arguments and its result
    # through the guest's registers -, or one of the system's simplest calls, made as the guest thread's loop makes
    # them but without the stop and the start around it. Its time goes to the log's lines as a call's.
    ('bool KernelState::init(MemState &mem, const CallImportFunc &call_import, bool cpu_opt) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <nids/functions.h>\n'
     'extern bool (*emusuni_vita_inline_svc)(CPUState &cpu);\n'
     'extern "C" void emusuni_vita_native_start(const char *(*name_of)(unsigned));\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     'extern "C" int emusuni_vita_svc_kind(unsigned nid);\n'
     'extern "C" void emusuni_vita_native_run(int kind, unsigned *r, unsigned char *base);\n'
     'extern "C" unsigned long long emusuni_vita_stat_now(void);\n'
     'extern "C" void emusuni_vita_stat_inline(unsigned nid, unsigned long long ticks);\n'
     'extern "C" void emusuni_vita_lw_start(const char *(*name_of)(unsigned));\n'
     'extern "C" int emusuni_vita_lw_on(void);\n'
     'extern "C" int emusuni_vita_lw_kind(unsigned nid);\n'
     '#include <cstdlib>\n'
     'extern "C" void emusuni_vita_gxm_start(const char *(*name_of)(unsigned));\n'
     'extern "C" int emusuni_vita_gxm_on(void);\n'
     'extern "C" int emusuni_vita_gxm_kind(unsigned nid);\n'
     'extern "C" int emusuni_gxm_get(int what, const unsigned char *base, unsigned address, unsigned *result);\n'
     'extern "C" void emusuni_vita_gxm_checked(int kind, unsigned mine, unsigned export_said);\n'
     'extern "C" void emusuni_vita_ids_start(const char *(*name_of)(unsigned));\n'
     'extern "C" int emusuni_vita_ids_on(void);\n'
     'extern "C" int emusuni_vita_ids_kind(unsigned nid);\n'
     'extern "C" void emusuni_vita_thread_made(const char *name, int id, int priority, unsigned cores);\n'
     'static KernelState *emusuni_kernel = nullptr;\n'
     '// (18-11, the switch "lwfast", port/ps4_vita_lwfast.cpp) A lightweight mutex\'s lock, try or unlock answered\n'
     '// here when nothing has to wait: the same calls on Vita3K\'s object as the exports make (Mutex::try_acquire,\n'
     '// Mutex::release). Anything else - no work area, no such mutex or thread, a lock that would wait or fails - goes\n'
     '// the ordinary way, its export.\n'
     'static bool emusuni_lw_fast(KernelState &kernel, CPUState &cpu, int kind) {\n'
     '    MemState &mem = *cpu.mem;\n'
     '    const Address work = read_reg(cpu, 0);\n'
     '    if (work == 0)\n'
     '        return false;\n'
     '    const MutexPtr mutex = kernel.objects.find<LwMutex>(Ptr<SceKernelLwMutexWork>(work).get(mem)->uid);\n'
     '    if (!mutex)\n'
     '        return false;\n'
     '    const ThreadStatePtr thread = kernel.get_thread(cpu.thread_id);\n'
     '    if (!thread)\n'
     '        return false;\n'
     '    const SceInt32 count = static_cast<SceInt32>(read_reg(cpu, 1));\n'
     '    const SceInt32 result = kind == 3 ? mutex->release(mem, thread, count) : mutex->try_acquire(mem, thread, count);\n'
     '    if (kind == 1 && result != SCE_KERNEL_OK)\n'
     '        return false;\n'
     '    write_reg(cpu, 0, static_cast<uint32_t>(result));\n'
     '    clear_exclusive(cpu);\n'
     '    return true;\n'
     '}\n'
     '// (18-11, the switch "fastids", port/ps4_vita_fastids.cpp) sceKernelGetThreadId and sceKernelGetTLSAddr answered\n'
     '// here at once: the thread\'s id is the recompiler\'s; its TLS block (ThreadState::tls) is the same for its whole\n'
     '// life, kept for the host thread the first time. A key out of 0..0x100 or no such thread: the ordinary way.\n'
     'static bool emusuni_ids_fast(KernelState &kernel, CPUState &cpu, int kind) {\n'
     '    if (kind == 2) {\n'
     '        write_reg(cpu, 0, static_cast<uint32_t>(cpu.thread_id));\n'
     '        return true;\n'
     '    }\n'
     '    const uint32_t key = read_reg(cpu, 0);\n'
     '    if (key > 0x100)\n'
     '        return false;\n'
     '    thread_local SceUID tls_thread = -1;\n'
     '    thread_local Address tls_base = 0;\n'
     '    if (tls_thread != cpu.thread_id) {\n'
     '        const ThreadStatePtr thread = kernel.get_thread(cpu.thread_id);\n'
     '        if (!thread)\n'
     '            return false;\n'
     '        tls_base = thread->tls.get_ptr<Ptr<void>>().address();\n'
     '        tls_thread = cpu.thread_id;\n'
     '    }\n'
     '    write_reg(cpu, 0, tls_base + key * 4);\n'
     '    return true;\n'
     '}\n'
     'static bool emusuni_inline_svc(CPUState &cpu) {\n'
     '    KernelState *const kernel = emusuni_kernel;\n'
     '    if (kernel == nullptr)\n'
     '        return false;\n'
     '    const uint32_t nid = *Ptr<uint32_t>(read_pc(cpu) + 4).get(*cpu.mem);\n'
     '    // (19-11, the switch "gxmfast", port/ps4_vita_gxmfast.cpp) The GPU library\'s simplest calls: library\n'
     '    // functions on the Vita, not system calls (no clear_exclusive). A getter answered on Vita3K\'s structures\n'
     '    // (SceGxm.cpp\'s emusuni_gxm_get), counted without its time (a few ns); the others, the export from here.\n'
     '    if (const int gxm = emusuni_vita_gxm_kind(nid)) {\n'
     '        if (gxm >= 100) {\n'
     '            const unsigned long long from = emusuni_vita_stat_now();\n'
     '            kernel->call_import(cpu, nid, cpu.thread_id);\n'
     '            emusuni_vita_stat_inline(nid, emusuni_vita_stat_now() - from);\n'
     '            return true;\n'
     '        }\n'
     '        unsigned result = 0;\n'
     '        if (emusuni_gxm_get(gxm, cpu.mem->memory.get(), read_reg(cpu, 0), &result)) {\n'
     '#if !defined(__ORBIS__)\n'
     '            // (The PC, EMUSUNI_VITA_GXMFAST_CHECK=1: the export too, and the two compared.)\n'
     '            static const bool check = getenv("EMUSUNI_VITA_GXMFAST_CHECK") != nullptr;\n'
     '            if (check) {\n'
     '                kernel->call_import(cpu, nid, cpu.thread_id);\n'
     '                emusuni_vita_gxm_checked(gxm, result, read_reg(cpu, 0));\n'
     '            }\n'
     '#endif\n'
     '            write_reg(cpu, 0, result);\n'
     '            emusuni_vita_stat_inline(nid, 0);\n'
     '            return true;\n'
     '        }\n'
     '    }\n'
     '    // (Counted, its time not taken: a few nanoseconds, less than the two clock reads that would measure it. And no\n'
     '    // clear_exclusive - the global monitor, under a spinlock every guest thread shares: on the Vita these two are\n'
     '    // read from the thread itself, not system calls, so nothing clears an exclusive access around them there.)\n'
     '    if (const int id = emusuni_vita_ids_kind(nid)) {\n'
     '        if (emusuni_ids_fast(*kernel, cpu, id)) {\n'
     '            emusuni_vita_stat_inline(nid, 0);\n'
     '            return true;\n'
     '        }\n'
     '    }\n'
     '    if (const int lw = emusuni_vita_lw_kind(nid)) {\n'
     '        const unsigned long long from = emusuni_vita_stat_now();\n'
     '        if (emusuni_lw_fast(*kernel, cpu, lw)) {\n'
     '            emusuni_vita_stat_inline(nid, emusuni_vita_stat_now() - from);\n'
     '            return true;\n'
     '        }\n'
     '    }\n'
     '    const int kind = emusuni_vita_svc_kind(nid);\n'
     '    if (kind == 0)\n'
     '        return false;\n'
     '    const unsigned long long from = emusuni_vita_stat_now();\n'
     '    if (kind == 1) {\n'
     '        kernel->call_import(cpu, nid, cpu.thread_id);\n'
     '        clear_exclusive(cpu);\n'
     '    } else {\n'
     '        unsigned r[4] = { read_reg(cpu, 0), read_reg(cpu, 1), read_reg(cpu, 2), read_reg(cpu, 3) };\n'
     '        const unsigned r0 = r[0];\n'
     '        emusuni_vita_native_run(kind, r, cpu.mem->memory.get());\n'
     '        if (r[0] != r0)\n'
     '            write_reg(cpu, 0, r[0]);\n'
     '    }\n'
     '    emusuni_vita_stat_inline(nid, emusuni_vita_stat_now() - from);\n'
     '    return true;\n'
     '}\n'
     '#endif\n\n'
     'bool KernelState::init(MemState &mem, const CallImportFunc &call_import, bool cpu_opt) {\n'),
    ('    this->call_import = call_import;\n',
     '    this->call_import = call_import;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_kernel = this;\n'
     '    emusuni_vita_native_start([](unsigned number) -> const char * { return import_name(number); });\n'
     '    // (After 01.66: only with one of its two switches on - nativelib, fastcalls. The hook itself, a call and a\n'
     '    // look in a table for every system call, was 0.8% of Mortal Kombat\'s threads\' time with both off.)\n'
     '    emusuni_vita_lw_start([](unsigned number) -> const char * { return import_name(number); });\n'
     '    emusuni_vita_ids_start([](unsigned number) -> const char * { return import_name(number); });\n'
     '    // (After 01.76: or with "fastgxm", 35 - the graphics library\'s setters among the fast calls -, or "fastids".)\n'
     '    emusuni_vita_gxm_start([](unsigned number) -> const char * { return import_name(number); });\n'
     '    emusuni_vita_inline_svc = (emusuni_vita_switch(14) != 0 || emusuni_vita_switch(15) != 0 || emusuni_vita_lw_on() != 0\n'
     '        || emusuni_vita_gxm_on() != 0\n'
     '            || emusuni_vita_switch(35) != 0 || emusuni_vita_ids_on() != 0)\n'
     '        ? emusuni_inline_svc : nullptr;\n'
     '#endif\n'),
    # (19-11, for 01.83, the switches "stubcall" and "callstats") an import stub's call, found when translated
    ('bool KernelState::init(MemState &mem, const CallImportFunc &call_import, bool cpu_opt) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '// (19-11, for 01.83: the switches "stubcall" and "callstats", port/ps4_vita_stubcall.cpp) A block that starts at an\n'
     '// import stub, made by the recompiler as one call of the function found when it was translated (dynarmic_cpu.cpp\'s\n'
     '// PreCodeReadHook asks emusuni_stub_plan; its CallSVC runs emusuni_stub_run with the slot): the answers of\n'
     '// emusuni_inline_svc above, without its four look-ups and call_import\'s bookkeeping, and with no clear_exclusive\n'
     '// (functions of libraries on a PS Vita: nothing clears an exclusive access around them there). With "callstats"\n'
     '// the time stamp counter is read on one call in 16.\n'
     'struct EmusuniStub {\n'
     '    uint32_t nid;\n'
     '    int16_t way; // 1 a GPU getter (sub: which), 2 the export, 3 GetTLSAddr/GetThreadId (sub 1/2), 4 a lightweight mutex (sub), 5 a native routine (sub)\n'
     '    int16_t sub;\n'
     '    const void *fn;\n'
     '};\n'
     'struct EmusuniStubPlan {\n'
     '    int how; // 1 a call of the slot\'s (a supervisor call that carries it), 2 the thread\'s own id or TLS block\n'
     '    uint32_t slot;\n'
     '    uint64_t value;\n'
     '    uint64_t arg;\n'
     '};\n'
     'extern bool (*emusuni_vita_stub_plan)(CPUState &cpu, uint32_t nid, EmusuniStubPlan &plan);\n'
     'extern bool (*emusuni_vita_stub_call)(CPUState &cpu, uint32_t slot);\n'
     'extern "C" void emusuni_vita_stub_start(const char *(*name_of)(unsigned));\n'
     'extern "C" int emusuni_vita_stub_on(void);\n'
     'extern "C" int emusuni_vita_stub_sampled(void);\n'
     'extern "C" int emusuni_vita_stub_clrex(void);\n'
     'extern "C" int emusuni_vita_stub_slot(unsigned nid, int *way, int *sub, const void **fn);\n'
     'extern "C" const void *emusuni_vita_stub_table(void);\n'
     'extern "C" void emusuni_vita_stub_made(int ids);\n'
     'void emusuni_vita_import_call(const void *fn, CPUState &cpu);\n'
     'static const EmusuniStub *emusuni_stubs = nullptr;\n'
     'static bool emusuni_stub_sampled = false;\n'
     'static bool emusuni_stub_clrex = false;\n'
     '// (A lightweight mutex as emusuni_lw_fast, without clear_exclusive, and with the calling thread kept by the host\n'
     '// thread: every guest thread runs on a host thread of its own, made for it and ended with it (thread_function), and\n'
     '// is alive while it runs its code. Mutex::try_acquire/release only read its id, so it is handed on without a share\n'
     '// of its ownership - no look-up under the kernel\'s lock and no shared pointer counted up and down at each call.)\n'
     'static bool emusuni_stub_lw(KernelState &kernel, CPUState &cpu, int kind) {\n'
     '    MemState &mem = *cpu.mem;\n'
     '    const Address work = read_reg(cpu, 0);\n'
     '    if (work == 0)\n'
     '        return false;\n'
     '    const MutexPtr mutex = kernel.objects.find<LwMutex>(Ptr<SceKernelLwMutexWork>(work).get(mem)->uid);\n'
     '    if (!mutex)\n'
     '        return false;\n'
     '    thread_local SceUID kept_id = -1;\n'
     '    thread_local ThreadState *kept = nullptr;\n'
     '    if (kept_id != cpu.thread_id) {\n'
     '        const ThreadStatePtr found = kernel.get_thread(cpu.thread_id);\n'
     '        if (!found)\n'
     '            return false;\n'
     '        kept = found.get();\n'
     '        kept_id = cpu.thread_id;\n'
     '    }\n'
     '    const ThreadStatePtr thread(ThreadStatePtr(), kept);\n'
     '    const SceInt32 count = static_cast<SceInt32>(read_reg(cpu, 1));\n'
     '    const SceInt32 result = kind == 3 ? mutex->release(mem, thread, count) : mutex->try_acquire(mem, thread, count);\n'
     '    if (kind == 1 && result != SCE_KERNEL_OK)\n'
     '        return false;\n'
     '    write_reg(cpu, 0, static_cast<uint32_t>(result));\n'
     '    return true;\n'
     '}\n'
     '// The recompiler translates a block at a stub of this function: the slot, or for sceKernelGetThreadId/GetTLSAddr\n'
     '// the thread\'s own numbers (a thread\'s recompiler is its own, its id and its TLS block never change).\n'
     'static bool emusuni_stub_plan(CPUState &cpu, uint32_t nid, EmusuniStubPlan &plan) {\n'
     '    int way = 0;\n'
     '    int sub = 0;\n'
     '    const void *fn = nullptr;\n'
     '    const int slot = emusuni_vita_stub_slot(nid, &way, &sub, &fn);\n'
     '    if (slot < 0)\n'
     '        return false;\n'
     '    plan.how = 1;\n'
     '    plan.slot = static_cast<uint32_t>(slot);\n'
     '    plan.value = 0;\n'
     '    plan.arg = static_cast<uint64_t>(nid) | (static_cast<uint64_t>(static_cast<uint32_t>(sub)) << 32);\n'
     '    if (way == 3 && emusuni_kernel != nullptr) {\n'
     '        if (sub == 2) {\n'
     '            plan.how = 2;\n'
     '            plan.value = static_cast<uint32_t>(cpu.thread_id);\n'
     '        } else if (const ThreadStatePtr thread = emusuni_kernel->get_thread(cpu.thread_id)) {\n'
     '            plan.how = 2;\n'
     '            plan.value = thread->tls.get_ptr<Ptr<void>>().address();\n'
     '        }\n'
     '    }\n'
     '    emusuni_vita_stub_made(plan.how == 2 ? 1 : 0);\n'
     '    return true;\n'
     '}\n'
     '// A slot\'s call, from the recompiled code (CallSVC). false: the guest thread\'s loop makes it (a lock that waits).\n'
     'static bool emusuni_stub_run(CPUState &cpu, uint32_t slot) {\n'
     '    const EmusuniStub &stub = emusuni_stubs[slot];\n'
     '    unsigned long long from = 0;\n'
     '    bool timed = false;\n'
     '    if (stub.way == 2 || stub.way == 4 || stub.way == 5) {\n'
     '        static thread_local uint32_t turn = 0;\n'
     '        timed = !emusuni_stub_sampled || (turn++ & 15u) == 0;\n'
     '        if (timed)\n'
     '            from = emusuni_vita_stat_now();\n'
     '    }\n'
     '    switch (stub.way) {\n'
     '    case 1: {\n'
     '        unsigned result = 0;\n'
     '        if (emusuni_gxm_get(stub.sub, cpu.mem->memory.get(), read_reg(cpu, 0), &result))\n'
     '            write_reg(cpu, 0, result);\n'
     '        else\n'
     '            emusuni_vita_import_call(stub.fn, cpu);\n'
     '        break;\n'
     '    }\n'
     '    case 2:\n'
     '        emusuni_vita_import_call(stub.fn, cpu);\n'
     '        break;\n'
     '    case 3:\n'
     '        if (!emusuni_ids_fast(*emusuni_kernel, cpu, stub.sub))\n'
     '            emusuni_vita_import_call(stub.fn, cpu);\n'
     '        break;\n'
     '    case 4:\n'
     '        if (!emusuni_stub_lw(*emusuni_kernel, cpu, stub.sub))\n'
     '            return false;\n'
     '        break;\n'
     '    case 5: {\n'
     '        unsigned r[4] = { read_reg(cpu, 0), read_reg(cpu, 1), read_reg(cpu, 2), read_reg(cpu, 3) };\n'
     '        const unsigned r0 = r[0];\n'
     '        emusuni_vita_native_run(stub.sub, r, cpu.mem->memory.get());\n'
     '        if (r[0] != r0)\n'
     '            write_reg(cpu, 0, r[0]);\n'
     '        break;\n'
     '    }\n'
     '    default:\n'
     '        return false;\n'
     '    }\n'
     '    if (emusuni_stub_clrex)\n'
     '        clear_exclusive(cpu);\n'
     '    emusuni_vita_stat_inline(stub.nid, timed ? (emusuni_vita_stat_now() - from) * (emusuni_stub_sampled ? 16u : 1u) : 0);\n'
     '    return true;\n'
     '}\n'
     '#endif\n'
     '\n'
     'bool KernelState::init(MemState &mem, const CallImportFunc &call_import, bool cpu_opt) {\n'),
    ('        ? emusuni_inline_svc : nullptr;\n',
     '        ? emusuni_inline_svc : nullptr;\n'
     '    // (19-11, "stubcall" and "callstats": the slots empty, the switches as they are now)\n'
     '    emusuni_vita_stub_start([](unsigned number) -> const char * { return import_name(number); });\n'
     '    emusuni_stubs = static_cast<const EmusuniStub *>(emusuni_vita_stub_table());\n'
     '    emusuni_stub_sampled = emusuni_vita_stub_sampled() != 0;\n'
     '    emusuni_stub_clrex = emusuni_vita_stub_clrex() != 0;\n'
     '    emusuni_vita_stub_plan = emusuni_vita_stub_on() != 0 ? emusuni_stub_plan : nullptr;\n'
     '    emusuni_vita_stub_call = emusuni_vita_stub_on() != 0 ? emusuni_stub_run : nullptr;\n'),
])

# The signal stack upstream puts back on the frontend's thread at the core's end: not on the PS4,
# where dynarmic sets none and the headers' stack_t isn't the kernel's.
# The core's "[TRACE n]" lines: those of the start stay, each written to the disk at once (01.29); those
# of retro_run (one and more for every picture) only for the first pictures - EMUSUNI_VITA_TRACE on the
# PC turns all of them on.
# (01.29) The start's steps ("vita: step: ..."), on the disk before each is taken, on the preparation's
# thread and around the first pictures the GPU makes; and the wait for the content without the GPU: a
# picture made by the CPU (EMUSUNI_VITA_SOFT_CARD on the PC), not Vita3K's card - see the comment there.
patch('vita3k/libretro/libretro_vita3k.cpp', [
    # (After 01.76, the "res2x" switch, 37.) Twice the PS Vita's resolution whatever the frontend's option says:
    # Vita3K's own multiplier, read where its option is read.
    ('        libretro.cfg->current_config.resolution_multiplier = parse_float_option(value, 1.0f);\n',
     '        libretro.cfg->current_config.resolution_multiplier = parse_float_option(value, 1.0f);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_switch(37)) {\n'
     '            libretro.cfg->current_config.resolution_multiplier = 2.0f;\n'
     '            emusuni_vita_log("vita: display: the picture is made at twice the PS Vita\'s size, 1920x1088 (the res2x switch is ON)");\n'
     '        }\n'
     '#endif\n'),
    # (After 01.67) The render thread's stop: its wait for a sync object is told to give up (renderer/src/sync.cpp,
    # the patch after 01.67 says why).
    ('    libretro.render_thread_running = false;\n    if (libretro.render_thread->joinable())\n        libretro.render_thread->join();\n',
     '    libretro.render_thread_running = false;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_render_stopping.store(true);\n'
     '#endif\n'
     '    if (libretro.render_thread->joinable())\n        libretro.render_thread->join();\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_render_stopping.store(false);\n'
     '#endif\n'),
    ('static void stop_render_thread() {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern std::atomic<bool> emusuni_vita_render_stopping;\n'
     '#endif\n'
     'static void stop_render_thread() {\n'),
    ('static void lr_trace(const char *tag, const char *fmt = nullptr, ...) {\n',
     KEYS_API + STEP_API + RESCUE_API + STAT_API + SWITCH_API +
     '#ifdef EMUSUNI_PORT\n'
     'extern bool emusuni_vita_firmware_failed; // (libretro_installer.cpp: a firmware part\'s install failed in this load)\n'
     '#endif\n'
     'static void lr_trace(const char *tag, const char *fmt = nullptr, ...) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    static const bool emusuni_trace = std::getenv("EMUSUNI_VITA_TRACE") != nullptr;\n'
     '    if (!emusuni_trace && tag && !std::strncmp(tag, "retro_run.", 10)) {\n'
     '        static std::atomic<unsigned> emusuni_run_traces{ 0 };\n'
     '        if (emusuni_run_traces.fetch_add(1, std::memory_order_relaxed) >= 36)\n'
     '            return;\n'
     '    }\n'
     '#endif\n'),
    ('        lr_log(RETRO_LOG_INFO, "[TRACE %llu] %s\\n", static_cast<unsigned long long>(seq), safe_tag);\n\n}\n',
     '        lr_log(RETRO_LOG_INFO, "[TRACE %llu] %s\\n", static_cast<unsigned long long>(seq), safe_tag);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_sync();\n'
     '#endif\n\n}\n'),
    # The preparation's thread.
    # ((01.30) The last word of a load that fails, from the preparation's thread: what the menu says. The step
    # of the progress not shown yet is dropped: it would reach the frontend after it and be the one kept.)
    ('void lr_load_progress(const std::string &what) {\n',
     'static void emusuni_last_message(const char *text) {\n'
     '    {\n'
     '        std::lock_guard lock(s_job.mutex);\n'
     '        s_job.what.clear();\n'
     '        s_job.progress_changed = false;\n'
     '    }\n'
     '    retro_message emusuni_message{ text, 600 };\n'
     '    libretro.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &emusuni_message);\n'
     '}\n\n'
     'void lr_load_progress(const std::string &what) {\n'),
    # ((01.30) Its catch is never reached on the PS4: a throw anywhere in the preparation goes back to the rescue
    # point instead, and the load ends as the catch would end it, with the app's own words for the menu.)
    ('        t_load_job = true;\n        try {\n',
     '        t_load_job = true;\n'
     '        emusuni_vita_step("load: the preparation\'s thread runs");\n'
     '        EmusuniVitaRescueScope emusuni_rescue_scope;\n'
     '        if (setjmp(*emusuni_vita_rescue_push("the content\'s preparation")) != 0) {\n'
     '            lr_log(RETRO_LOG_ERROR, "Preparing the content failed: %s\\n", emusuni_vita_rescued());\n'
     '            emusuni_last_message("The emulator couldn\'t start this game");\n'
     '            s_job.result.success = false;\n'
     '            emusuni_vita_step("load: the preparation\'s thread is done (it failed)");\n'
     '            s_job.done = true;\n'
     '            return;\n'
     '        }\n'
     '        try {\n'),
    # ((01.30) The system firmware's install was tried and failed: no game starts without it, so the load ends
    # there with what to do - the app's words, in its languages -, not in whatever a game without it runs into.
    # With no .PUP to install it goes on as upstream: homebrew that needs no firmware, the tests'.)
    ('                    lr_trace("retro_load_game.firmware", "firmware not found at %s", system_dir.generic_string().c_str());\n'
     '                }\n',
     '                    lr_trace("retro_load_game.firmware", "firmware not found at %s", system_dir.generic_string().c_str());\n'
     '                    if (emusuni_vita_firmware_failed) {\n'
     '                        emusuni_last_message("The PS Vita\'s firmware couldn\'t be installed. Check that the PS4 has 1 GB free and start the game again.");\n'
     '                        s_job.result.success = false;\n'
     '                        emusuni_vita_step("load: the system firmware is NOT installed (its install failed): the load ends here");\n'
     '                        emusuni_vita_step("load: the preparation\'s thread is done (no firmware)");\n'
     '                        s_job.done = true;\n'
     '                        return;\n'
     '                    }\n'
     '                }\n'),
    ('            offer_storage_locations(system_dir);\n',
     '            emusuni_vita_step("load: the storage is %s; the Storage Location option is settled", storage.c_str());\n'
     '            offer_storage_locations(system_dir);\n'
     '            emusuni_vita_step("load: the user data, kept in %s", save_root.empty() ? "the storage" : save_root.c_str());\n'),
    ('                if (has_user_data_to_move(storage_path)) {\n'
     '                    lr_load_progress(lr_text("msg_moving_data"));\n',
     '                if (has_user_data_to_move(storage_path)) {\n'
     '                    emusuni_vita_step("load: the storage\'s user data moves to the save directory");\n'
     '                    lr_load_progress(lr_text("msg_moving_data"));\n'),
    ('                lr_load_progress(lr_text("msg_preparing_title"));\n'
     '                if (!check_firmware_installed(storage_path, system_dir)) {\n',
     '                lr_load_progress(lr_text("msg_preparing_title"));\n'
     '                emusuni_vita_step("load: the firmware (installed from its .PUP files when it isn\'t)");\n'
     '                if (!check_firmware_installed(storage_path, system_dir)) {\n'),
    ('                lr_load_progress(lr_text("msg_preparing_title"));\n'
     '                s_job.result = open_content();\n',
     '                lr_load_progress(lr_text("msg_preparing_title"));\n'
     '                emusuni_vita_step("load: the firmware is done; the game is opened (installed the first time)");\n'
     '                s_job.result = open_content();\n'
     '                emusuni_vita_step("load: the game is %s", s_job.result.success ? "prepared" : "NOT prepared");\n'),
    ('        s_job.done = true;\n    });\n}\n',
     '        emusuni_vita_step("load: the preparation\'s thread is done");\n'
     '        s_job.done = true;\n    });\n}\n'),
    # The wait for it, in retro_run: no work of the GPU's.
    ('// retro_run while the content is prepared: the call the thread waits on\n',
     '#ifdef EMUSUNI_PORT\n'
     '// EmuSuNi (01.29): the wait for the content without the GPU. The card Vita3K draws meanwhile with its Vulkan\n'
     '// renderer is the core\'s first work on the console\'s GPU, and 01.26\'s first tries on a PS4 ended at the first\n'
     '// of those pictures: the console closed the app, no signal, no crash.log - what it does for a fault of the\n'
     '// GPU\'s. The firmware\'s install needs no card: a picture made by the CPU instead - black, a bar that goes from\n'
     '// side to side, a mark for each step so far - handed over as a software frame (the frontend draws those as\n'
     '// any console\'s; a picture keeps its start watchdog quiet too), the step\'s words as the frontend\'s message.\n'
     '// The renderer draws its first picture with the game, between steps that say so.\n'
     '// On the PS4 always; on the PC with EMUSUNI_VITA_SOFT_CARD (the tests), else Vita3K\'s card as upstream.\n'
     'static bool emusuni_soft_card() {\n'
     '#if defined(__ORBIS__)\n'
     '    return true;\n'
     '#else\n'
     '    static const bool soft = std::getenv("EMUSUNI_VITA_SOFT_CARD") != nullptr;\n'
     '    return soft;\n'
     '#endif\n'
     '}\n'
     'static void emusuni_draw_soft_card(const std::string &what, bool changed) {\n'
     '    constexpr unsigned W = 960, H = 544, X0 = 240, X1 = 720, Y0 = 300, Y1 = 312, BLOCK = 96;\n'
     '    static std::vector<uint32_t> picture;\n'
     '    static unsigned frame = 0, steps = 0;\n'
     '    if (picture.empty()) {\n'
     '        emusuni_vita_step("load: the wait\'s first picture, made by the CPU (no work of the core\'s on the GPU yet)");\n'
     '        picture.assign(static_cast<size_t>(W) * H, 0xFF000000u);\n'
     '    }\n'
     '    if (changed) {\n'
     '        steps++;\n'
     '        lr_load_progress_show(what);\n'
     '    }\n'
     '    const unsigned span = X1 - X0 - BLOCK, t = frame % (2 * span);\n'
     '    const unsigned at = X0 + (t < span ? t : 2 * span - t);\n'
     '    for (unsigned y = Y0; y < Y1; y++) {\n'
     '        uint32_t *row = picture.data() + static_cast<size_t>(y) * W;\n'
     '        for (unsigned x = X0; x < X1; x++)\n'
     '            row[x] = x >= at && x < at + BLOCK ? 0xFFE0E0E0u : 0xFF303030u;\n'
     '    }\n'
     '    for (unsigned i = 0; i < 16; i++) {\n'
     '        const uint32_t color = i < steps ? 0xFF909090u : 0xFF000000u;\n'
     '        for (unsigned y = Y1 + 16; y < Y1 + 22; y++) {\n'
     '            uint32_t *row = picture.data() + static_cast<size_t>(y) * W;\n'
     '            for (unsigned x = X0 + i * 30; x < X0 + i * 30 + 18; x++)\n'
     '                row[x] = color;\n'
     '        }\n'
     '    }\n'
     '    frame += 4;\n'
     '    if (frame == 4)\n'
     '        emusuni_vita_step("load: that picture is handed to the frontend");\n'
     '    if (libretro.video_cb)\n'
     '        libretro.video_cb(picture.data(), W, H, W * sizeof(uint32_t));\n'
     '    if (frame == 4)\n'
     '        emusuni_vita_step("load: the frontend took it; the wait goes on with a picture for each call");\n'
     '}\n'
     '#endif\n\n'
     '// retro_run while the content is prepared: the call the thread waits on\n'),
    ('    auto *boot_vk = dynamic_cast<renderer::vulkan::VKState *>(s_boot_renderer.get());\n'
     '    if (boot_vk && s_boot_overlays && libretro.renderer_ready) {\n'
     '        // Vita3K\'s overlay with the step, drawn over black and handed over as\n',
     '    auto *boot_vk = dynamic_cast<renderer::vulkan::VKState *>(s_boot_renderer.get());\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_soft_card()) {\n'
     '        emusuni_draw_soft_card(what, changed);\n'
     '    } else\n'
     '#endif\n'
     '    if (boot_vk && s_boot_overlays && libretro.renderer_ready) {\n'
     '        // Vita3K\'s overlay with the step, drawn over black and handed over as\n'),
    ('    if (!s_job.done)\n        return;\n    s_job.thread.join();\n    s_job.active = false;\n',
     '    if (!s_job.done)\n        return;\n'
     '    emusuni_vita_step("load: the content is prepared; the rest of the load, in retro_run");\n'
     '    s_job.thread.join();\n    s_job.active = false;\n'),
    # The rest of the load.
    ('    if (!libretro_init_emuenv(emuenv, root)) {\n'
     '        lr_log(RETRO_LOG_ERROR, "Failed to initialize emulator environment!\\n");\n',
     '    emusuni_vita_step("load: the emulator is made (its memory, its kernel, its sound)");\n'
     '    if (!libretro_init_emuenv(emuenv, root)) {\n'
     '        lr_log(RETRO_LOG_ERROR, "Failed to initialize emulator environment!\\n");\n'),
    ('    int32_t main_module_id = -1;\n    ExitCode load_err = libretro_load_app(main_module_id, emuenv);\n',
     '    int32_t main_module_id = -1;\n'
     '    emusuni_vita_step("load: the emulator is ready; the game\'s and the system\'s modules are loaded");\n'
     '    emusuni_vita_game(emuenv.io.title_id.c_str());\n'
     '    ExitCode load_err = libretro_load_app(main_module_id, emuenv);\n'
     '    emusuni_vita_step("load: the modules are loaded (%d)", static_cast<int>(load_err));\n'),
    ('    if (s_boot_renderer) {\n'
     '        // the load progress\' renderer becomes the emulator\'s, with its\n',
     '    emusuni_vita_step("load: the game is loaded; the renderer is given to the emulator and the game starts");\n'
     '    if (s_boot_renderer) {\n'
     '        // the load progress\' renderer becomes the emulator\'s, with its\n'),
    # The first pictures the GPU makes: the game's frame copied for the frontend, then handed over.
    ('    vk::SubmitInfo submit{};\n    submit.setCommandBuffers(cmd);\n'
     '    vk_state.locked_queue_submit(vk_state.general_queue, submit, s_present.fence);\n',
     '    vk::SubmitInfo submit{};\n    submit.setCommandBuffers(cmd);\n'
     '    static unsigned emusuni_copies = 0;\n'
     '    const bool emusuni_say = emusuni_copies < 3;\n'
     '    emusuni_copies += emusuni_say;\n'
     '    if (emusuni_say)\n'
     '        emusuni_vita_step("gpu: picture %u (%ux%u, %s%s) is copied for the frontend: submitted", emusuni_copies, w, h,\n'
     '            surface ? "the game\'s surface" : pixels ? "pixels" : "black", has_overlays ? ", with overlays" : "");\n'
     '    vk_state.locked_queue_submit(vk_state.general_queue, submit, s_present.fence);\n'),
    ('    (void)vk_state.device.waitForFences(s_present.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());\n'
     '    vk_state.device.resetFences(s_present.fence);\n\n'
     '    std::lock_guard<std::mutex> lock(libretro.rendered_frame_mutex);\n    s_present.published = target;\n',
     '    (void)vk_state.device.waitForFences(s_present.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());\n'
     '    vk_state.device.resetFences(s_present.fence);\n'
     '    if (emusuni_say)\n'
     '        emusuni_vita_step("gpu: picture %u: the GPU did the copy", emusuni_copies);\n\n'
     '    std::lock_guard<std::mutex> lock(libretro.rendered_frame_mutex);\n    s_present.published = target;\n'),
    ('    vulkan->lock_queue(vulkan->handle);\n    vkfn.QueueSubmit(vulkan->queue, 1, &submit, vkp.fences[index]);\n'
     '    vulkan->unlock_queue(vulkan->handle);\n    vkp.fence_pending[index] = true;\n',
     '    static unsigned emusuni_presents = 0;\n'
     '    const bool emusuni_say = emusuni_presents < 3;\n'
     '    emusuni_presents += emusuni_say;\n'
     '    if (emusuni_say)\n'
     '        emusuni_vita_step("gpu: present %u (%ux%u from %s, image %u of %u): its blit is submitted", emusuni_presents,\n'
     '            vkp.width, vkp.height, src_image != VK_NULL_HANDLE ? "a picture" : "nothing: black", index, vkp.num_images);\n'
     '    vulkan->lock_queue(vulkan->handle);\n    vkfn.QueueSubmit(vulkan->queue, 1, &submit, vkp.fences[index]);\n'
     '    vulkan->unlock_queue(vulkan->handle);\n    vkp.fence_pending[index] = true;\n'),
    ('    vulkan->set_image(vulkan->handle, &vkp.images[index], 0, nullptr, VK_QUEUE_FAMILY_IGNORED);\n'
     '    report_frame_size(vkp.width, vkp.height);\n'
     '    libretro.video_cb(RETRO_HW_FRAME_BUFFER_VALID, vkp.width, vkp.height, 0);\n    return true;\n',
     '    if (emusuni_say)\n'
     '        emusuni_vita_step("gpu: present %u: the image is handed to the frontend", emusuni_presents);\n'
     '    vulkan->set_image(vulkan->handle, &vkp.images[index], 0, nullptr, VK_QUEUE_FAMILY_IGNORED);\n'
     '    report_frame_size(vkp.width, vkp.height);\n'
     '    libretro.video_cb(RETRO_HW_FRAME_BUFFER_VALID, vkp.width, vkp.height, 0);\n'
     '    if (emusuni_say)\n'
     '        emusuni_vita_step("gpu: present %u: the frontend took it", emusuni_presents);\n'
     '    return true;\n'),
    ('#ifndef _WIN32\n    frontend_signal_stack_saved = sigaltstack(nullptr, &frontend_signal_stack) == 0;\n#endif\n',
     '#if !defined(_WIN32) && !defined(__ORBIS__)\n'
     '    frontend_signal_stack_saved = sigaltstack(nullptr, &frontend_signal_stack) == 0;\n#endif\n'),
    # (A build without Sony's keys: the player's file, read before anything is installed or loaded.)
    ('    start_load_job();\n',
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    emusuni_vita_keys_load((libretro.system_dir + "/vita-keys.txt").c_str());\n'
     '#endif\n'
     '    start_load_job();\n'),
    # (01.31) The game's own speed, in the log every 10 s: counted from the game's start, a refresh at a time.
    ('            lr_log(RETRO_LOG_INFO, "context_reset_finalize: app start complete\\n");\n',
     '            lr_log(RETRO_LOG_INFO, "context_reset_finalize: app start complete\\n");\n'
     '#ifdef EMUSUNI_PORT\n'
     '            emusuni_vita_stat_start();\n'
     '#endif\n'),
    ('    if (libretro.app_started)\n        libretro_vblank_tick(emuenv);\n',
     '    if (libretro.app_started) {\n'
     '        libretro_vblank_tick(emuenv);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_stat_tick();\n'
     '        // ((17-11) The round of the graphics\' test switches, with the file vita-test-cycle: each step\'s words on\n'
     '        // the screen as it starts, and the pipelines compiled before their draws or aside as the step says.)\n'
     '        if (const char *const emusuni_test = emusuni_vita_test_tick()) {\n'
     '            retro_message emusuni_test_message{ emusuni_test, 900 };\n'
     '            libretro.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &emusuni_test_message);\n'
     '            if (emuenv.renderer)\n'
     '                emuenv.renderer->set_async_compilation(libretro.cfg->current_config.async_pipeline_compilation && !emusuni_vita_switch(10));\n'
     '        }\n'
     '#endif\n'
     '    }\n'),
    # (01.40) Two of Vita3K's own options by a switch's file, for a test on the console without a menu for them:
    # "accuracy" turns the renderer's high accuracy on, "syncshaders" has a pipeline compiled before its draw.
    ('        libretro.cfg->current_config.high_accuracy = parse_enabled_option(value, false);\n',
     '        libretro.cfg->current_config.high_accuracy = parse_enabled_option(value, false);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_switch(9))\n'
     '            libretro.cfg->current_config.high_accuracy = true;\n'
     '#endif\n'),
    ('        libretro.cfg->current_config.async_pipeline_compilation = parse_enabled_option(value, true);\n',
     '        libretro.cfg->current_config.async_pipeline_compilation = parse_enabled_option(value, true);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_switch(10))\n'
     '            libretro.cfg->current_config.async_pipeline_compilation = false;\n'
     '#endif\n'),
    # (After 01.47) sceAppMgrLoadExec: a game whose eboot.bin is a small launcher (Uncharted: Golden Abyss, 12 KB)
    # asks to go on with another of its executables. Upstream's frontend closes the game and starts it again with
    # that one; this core had no such step, so the launcher's thread span for good with nothing on the screen.
    # Here the request (kept in the core's state: libretro_state.h, set in libretro_game_loader.cpp) is taken by
    # retro_run as a reset is - the content closed and loaded again -, and the loader takes the executable asked
    # for. A reset from the frontend, a load and an unload forget it: the game starts from its eboot.bin again.
    # The reset destroys the images the core handed to the frontend (set_image), and the frontend - which was not
    # the one to ask for this reset, and at its own one has no word of it either - goes on drawing the last one
    # until a new one comes: the PC bench crashed in the Vulkan driver right there, and on the PS4 a read of freed
    # GPU memory closes the app with no crash.log (01.09's 3DS reset). At a reset those images are kept aside, and
    # destroyed once the frontend has had 120 new ones, or when the context goes.
    ('static void libretro_vk_destroy_presentation_resources() {\n',
     'static struct {\n'
     '    VkDevice device = VK_NULL_HANDLE;\n'
     '    VkImageView views[LIBRETRO_MAX_SWAPCHAIN] = {};\n'
     '    VkImage images[LIBRETRO_MAX_SWAPCHAIN] = {};\n'
     '    VkDeviceMemory memory[LIBRETRO_MAX_SWAPCHAIN] = {};\n'
     '    uint32_t count = 0;\n'
     '    int frames_left = 0;\n'
     '    bool keep = false; // (set around the reset\'s own context_destroy)\n'
     '} emusuni_kept;\n\n'
     'static void emusuni_free_kept_images() {\n'
     '    if (!emusuni_kept.count)\n'
     '        return;\n'
     '    for (uint32_t i = 0; i < emusuni_kept.count; i++) {\n'
     '        if (emusuni_kept.views[i]) vkfn.DestroyImageView(emusuni_kept.device, emusuni_kept.views[i], nullptr);\n'
     '        if (emusuni_kept.images[i]) vkfn.DestroyImage(emusuni_kept.device, emusuni_kept.images[i], nullptr);\n'
     '        if (emusuni_kept.memory[i]) vkfn.FreeMemory(emusuni_kept.device, emusuni_kept.memory[i], nullptr);\n'
     '        emusuni_kept.views[i] = VK_NULL_HANDLE;\n'
     '        emusuni_kept.images[i] = VK_NULL_HANDLE;\n'
     '        emusuni_kept.memory[i] = VK_NULL_HANDLE;\n'
     '    }\n'
     '    lr_log(RETRO_LOG_INFO, "The %u images of before the reset are destroyed\\n", emusuni_kept.count);\n'
     '    emusuni_kept.count = 0;\n'
     '}\n\n'
     'static void libretro_vk_destroy_presentation_resources() {\n'),
    ('    for (uint32_t i = 0; i < vkp.num_images; i++) {\n'
     '        if (vkp.vk_views[i]) vkfn.DestroyImageView(device, vkp.vk_views[i], nullptr);\n',
     '    // (Those of a reset before, when one of these has been handed over since: the device is idle, they can go.\n'
     '    // With none handed over since, the frontend still has one of those and none of these: those stay kept,\n'
     '    // these are destroyed below.)\n'
     '    const bool emusuni_none_since = emusuni_kept.count && emusuni_kept.frames_left == 120;\n'
     '    if (!emusuni_kept.keep || !emusuni_none_since)\n'
     '        emusuni_free_kept_images();\n'
     '    if (emusuni_kept.keep && !emusuni_none_since) {\n'
     '        emusuni_kept.device = device;\n'
     '        emusuni_kept.count = vkp.num_images;\n'
     '        emusuni_kept.frames_left = 120;\n'
     '        for (uint32_t i = 0; i < vkp.num_images; i++) {\n'
     '            emusuni_kept.views[i] = vkp.vk_views[i];\n'
     '            emusuni_kept.images[i] = vkp.vk_images[i];\n'
     '            emusuni_kept.memory[i] = vkp.vk_memory[i];\n'
     '            vkp.vk_views[i] = VK_NULL_HANDLE;\n'
     '            vkp.vk_images[i] = VK_NULL_HANDLE;\n'
     '            vkp.vk_memory[i] = VK_NULL_HANDLE;\n'
     '        }\n'
     '        lr_log(RETRO_LOG_INFO, "The %u images handed to the frontend are kept until it has new ones\\n", emusuni_kept.count);\n'
     '    }\n'
     '    for (uint32_t i = 0; i < vkp.num_images; i++) {\n'
     '        if (vkp.vk_views[i]) vkfn.DestroyImageView(device, vkp.vk_views[i], nullptr);\n'),
    ('    vulkan->set_image(vulkan->handle, &vkp.images[index], 0, nullptr, VK_QUEUE_FAMILY_IGNORED);\n',
     '    vulkan->set_image(vulkan->handle, &vkp.images[index], 0, nullptr, VK_QUEUE_FAMILY_IGNORED);\n'
     '    if (emusuni_kept.count && --emusuni_kept.frames_left <= 0)\n'
     '        emusuni_free_kept_images();\n'),
    ('    release_renderer(*libretro.emuenv);\n    context_destroy();\n',
     '    release_renderer(*libretro.emuenv);\n'
     '    emusuni_kept.keep = true;\n'
     '    context_destroy();\n'
     '    emusuni_kept.keep = false;\n'),
    ('    libretro.reset_requested = true;\n',
     '    libretro.exec_forget();\n'
     '    libretro.reset_requested = true;\n'),
    ('    if (libretro.reset_requested.exchange(false)) {\n',
     '    if (libretro.exec_requested.exchange(false)) {\n'
     '        lr_log(RETRO_LOG_INFO, "retro_run: the game asked to go on with another of its executables (sceAppMgrLoadExec): the content is loaded again with it\\n");\n'
     '        libretro.reset_requested.store(true);\n'
     '    }\n'
     '    if (libretro.reset_requested.exchange(false)) {\n'),
    ('    libretro.game_path = game->path;\n',
     '    libretro.game_path = game->path;\n'
     '    libretro.exec_forget();\n'),
    ('    libretro.installed_title_id.clear();\n    libretro.installed_title.clear();\n}\n',
     '    libretro.installed_title_id.clear();\n    libretro.installed_title.clear();\n    libretro.exec_forget();\n}\n'),
    # (20-11, for 01.84) the "sixcores" switch (56, OFF): port/ps4_vita_place.cpp
    ('static void context_reset_finalize() {\n',
     '#ifdef EMUSUNI_PORT\nextern "C" void emusuni_vita_sixcores_here(int who);\n#endif\nstatic void context_reset_finalize() {\n'),
    ('        libretro.render_thread = std::make_unique<std::thread>([]() {\n            EmuEnvState &emuenv = *libretro.emuenv;\n',
     '        libretro.render_thread = std::make_unique<std::thread>([]() {\n#ifdef EMUSUNI_PORT\n            // (20-11, "sixcores", 56) the renderer\'s thread on the PS4\'s cores 0-5 (port/ps4_vita_place.cpp)\n            emusuni_vita_sixcores_here(1);\n#endif\n            EmuEnvState &emuenv = *libretro.emuenv;\n'),
    # (20-11, for 01.85) the switches "submitthread" (60) and "presentnowait" (61), OFF
    ('static void libretro_copy_frame_for_presentation(renderer::vulkan::VKState &vk_state, vk::Image surface,\n    vk::Format surface_format, const void *pixels, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h);\n',
     '#ifdef EMUSUNI_PORT\nextern "C" int emusuni_vita_switch(int which);\nextern "C" void emusuni_vita_submitted(void *fence);\n// (20-11, for 01.85, the switch "presentnowait", 61) the last picture\'s copy for the frontend, not waited for yet\nstatic bool s_emusuni_present_pending = false;\n#endif\nstatic void libretro_copy_frame_for_presentation(renderer::vulkan::VKState &vk_state, vk::Image surface,\n    vk::Format surface_format, const void *pixels, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h);\n'),
    ('        s_present.fence = vk_state.device.createFence({});\n    }\n',
     '        s_present.fence = vk_state.device.createFence({});\n    }\n#ifdef EMUSUNI_PORT\n    // (presentnowait, 61) the last copy waited for here - long done by now -, before its command buffer, its\n    // staging buffer or the overlays\' data are used again (the line "vita: renderer:", 55: the wait)\n    if (s_emusuni_present_pending) {\n        const unsigned long long emusuni_from = emusuni_vita_stat_now();\n        (void)vk_state.device.waitForFences(s_present.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());\n        vk_state.device.resetFences(s_present.fence);\n        s_emusuni_present_pending = false;\n        emusuni_vita_stat_add(55, emusuni_vita_stat_now() - emusuni_from);\n    }\n#endif\n'),
    ('        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, after);\n    cmd.end();\n',
     "        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, after);\n#ifdef EMUSUNI_PORT\n    // (presentnowait, 61) the game's later drawing into the surface after this copy's read of it, on the GPU: the\n    // CPU no longer waits for the copy before the renderer goes on\n    if (emusuni_vita_switch(61))\n        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eColorAttachmentOutput,\n            vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, {});\n#endif\n    cmd.end();\n"),
    ('    vk_state.locked_queue_submit(vk_state.general_queue, submit, s_present.fence);\n    // the copy is done before the surface may change again, and before the\n    // command buffer is recorded anew\n    (void)vk_state.device.waitForFences(s_present.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());\n    vk_state.device.resetFences(s_present.fence);\n',
     '    vk_state.locked_queue_submit(vk_state.general_queue, submit, s_present.fence);\n#ifdef EMUSUNI_PORT\n    // (20-11, for 01.85, the switch "presentnowait", 61, OFF) The renderer waited here for the GPU to finish all\n    // the picture\'s work and the copy, every picture; HENkaku-PS4 hands its picture to a flip done by the GPU. With\n    // the switch, only for the copy to be submitted (with "submitthread"), so the frontend\'s use of it comes after\n    // on the queue; the wait for the GPU at the next copy (above). The line "vita: renderer:", 55: the wait.\n    if (emusuni_vita_switch(61)) {\n        emusuni_vita_submitted(static_cast<VkFence>(s_present.fence));\n        s_emusuni_present_pending = true;\n    } else {\n        const unsigned long long emusuni_from = emusuni_vita_stat_now();\n#endif\n    // the copy is done before the surface may change again, and before the\n    // command buffer is recorded anew\n    (void)vk_state.device.waitForFences(s_present.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());\n    vk_state.device.resetFences(s_present.fence);\n#ifdef EMUSUNI_PORT\n        emusuni_vita_stat_add(55, emusuni_vita_stat_now() - emusuni_from);\n    }\n#endif\n'),
    ('    s_present.fence = nullptr;\n',
     '    s_present.fence = nullptr;\n#ifdef EMUSUNI_PORT\n    s_emusuni_present_pending = false; // (presentnowait: waited for by the waitIdle above)\n#endif\n'),
])
patch('vita3k/libretro/libretro_state.h', [
    ('    std::atomic<bool> reset_requested{false};\n',
     '    std::atomic<bool> reset_requested{false};\n'
     '    // (EmuSuNi) sceAppMgrLoadExec: the executable the game asked to go on with (its path below app0:) and its\n'
     '    // arguments. Set on the guest\'s thread, taken by retro_run, which loads the content again with them.\n'
     '    std::mutex exec_mutex;\n'
     '    std::string exec_self;\n'
     '    std::vector<std::string> exec_argv;\n'
     '    std::atomic<bool> exec_requested{false};\n'
     '    void exec_forget() {\n'
     '        const std::lock_guard<std::mutex> lock(exec_mutex);\n'
     '        exec_self.clear();\n'
     '        exec_argv.clear();\n'
     '        exec_requested = false;\n'
     '    }\n'),
])
# The loader's side of it: the kernel's word that the game ends or asks for another executable, which nothing
# listened to in this core; the executable loaded, eboot.bin unless one was asked for; and that one's arguments,
# one after another in the guest's memory, each ended by a zero, as a thread's argument block is.
patch('vita3k/libretro/libretro_game_loader.cpp', [
    ('#include <module/load_module.h>\n',
     '#include <module/load_module.h>\n'
     '#include <mem/functions.h>\n'
     '#include <cstring>\n'
     '#include <optional>\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     'extern "C" void emusuni_vita_log(const char *format, ...);\n'),
    # (17-11) The recompiler's optimizations. The standalone starts its kernel with the CPU Optimisation option
    # (interface.cpp: emuenv.cfg.current_config.cpu_opt, on by default); this loader passed the kernel's own field
    # of that name, which nothing sets: false. So in this core every guest thread's recompiler was made with no
    # optimizations at all - no jumps from block to block, no return prediction, no fast dispatch: each block left
    # through the dispatcher's lookup - and without fast memory: each memory access of the game's a call of the
    # emulator's, which first checks the address in the allocator's bitmap. Mortal Kombat on the PC: 34 million
    # such accesses and 10 million lookups a second, 73% of the game's threads' time in the emulator's own code
    # and 18% in the game's recompiled code. The "cpuopt" switch, ON, gives the kernel what the option says.
    ('    if (!emuenv.kernel.init(emuenv.mem, call_import, emuenv.kernel.cpu_opt)) {\n',
     '    const bool emusuni_cpu_opt = emuenv.cfg.current_config.cpu_opt && emusuni_vita_switch(16) != 0;\n'
     '    emusuni_vita_log("vita: jit: the recompiler\'s optimizations and fast memory: %s", emusuni_cpu_opt ? "ON"\n'
     '        : emuenv.cfg.current_config.cpu_opt ? "OFF (the cpuopt switch)" : "OFF (the CPU Optimisation option)");\n'
     '    if (!emuenv.kernel.init(emuenv.mem, call_import, emusuni_cpu_opt)) {\n'),
    ('    LOG_INFO("Kernel initialized");\n',
     '    LOG_INFO("Kernel initialized");\n'
     '    emuenv.kernel.process_exit_callback = [](int res, std::optional<AppLaunchRequest> relaunch) {\n'
     '        if (relaunch && relaunch->reason == AppLaunchReason::LoadExec && !relaunch->self_path.empty()) {\n'
     '            LOG_INFO("The game asks to go on with its executable app0:{} ({} arguments)", relaunch->self_path, relaunch->argv.size());\n'
     '            {\n'
     '                const std::lock_guard<std::mutex> lock(libretro.exec_mutex);\n'
     '                libretro.exec_self = relaunch->self_path;\n'
     '                libretro.exec_argv = relaunch->argv;\n'
     '            }\n'
     '            libretro.exec_requested = true;\n'
     '        } else {\n'
     '            LOG_INFO("The game asked to end (code {}): nothing is done about it in this core", res);\n'
     '        }\n'
     '    };\n'),
    ('    emuenv.self_path = !emuenv.cfg.self_path.empty() ? emuenv.cfg.self_path : EBOOT_PATH;\n',
     '    emuenv.self_path = !emuenv.cfg.self_path.empty() ? emuenv.cfg.self_path : EBOOT_PATH;\n'
     '    {\n'
     '        const std::lock_guard<std::mutex> lock(libretro.exec_mutex);\n'
     '        if (!libretro.exec_self.empty())\n'
     '            emuenv.self_path = libretro.exec_self;\n'
     '    }\n'),
    ('    if (main_thread->start(param.size, Ptr<void>(param.attr), true) < 0) {\n',
     '    {\n'
     '        std::string exec_args;\n'
     '        {\n'
     '            const std::lock_guard<std::mutex> lock(libretro.exec_mutex);\n'
     '            for (const std::string &one : libretro.exec_argv) {\n'
     '                exec_args += one;\n'
     '                exec_args.push_back(\'\\0\');\n'
     '            }\n'
     '        }\n'
     '        if (!exec_args.empty()) {\n'
     '            const Address exec_at = alloc(emuenv.mem, static_cast<uint32_t>(exec_args.size()), "exec arguments");\n'
     '            if (exec_at) {\n'
     '                std::memcpy(Ptr<char>(exec_at).get(emuenv.mem), exec_args.data(), exec_args.size());\n'
     '                param.size = static_cast<SceSize>(exec_args.size());\n'
     '                param.attr = exec_at;\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '    if (main_thread->start(param.size, Ptr<void>(param.attr), true) < 0) {\n'),
])

# (01.31) The measuring points of port/ps4_vita_stats.cpp's lines.
# A picture the game shows: its sceDisplaySetFrameBuf.
patch('vita3k/modules/SceDisplay/SceDisplay.cpp', [
    ('static int display_wait(EmuEnvState &emuenv, SceUID thread_id, int vcount, const bool is_since_setbuf, const bool is_cb) {\n',
     STAT_API +
     'static int display_wait(EmuEnvState &emuenv, SceUID thread_id, int vcount, const bool is_since_setbuf, const bool is_cb) {\n'),
    ('    emuenv.frame_count++;\n',
     '    emuenv.frame_count++;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_stat_add(0, 0);\n'
     '#endif\n'),
])
# (After 01.71) The "coreturns" switch (28; ON for God of War Collection, PCSA00126, OFF for the rest): the threads
# a game ties to one of the Vita's three cores run the game's code one at a time, as on the Vita. There a core
# goes from a thread to another when the one that runs waits for something, not in the middle of what it does,
# and Sony's file library counts on it: its "SceFiosIO" and "SceFiosCallback" threads take two mutexes in
# opposite orders. Here every thread has a core of the PS4's to itself, the two cross, and each ends up holding
# the mutex the other waits for - God of War Collection's launcher stopped so on the console at every start
# (01.71's log names the two threads and their mutexes; HENkaku-PS4's notes tell of the same stop and of this
# way out of it - read, its code not taken).
# A thread tied to one core takes that core's turn before it runs the game's code and keeps it through its
# system calls; it gives it up when it waits (the kernel's waits) and when it stops running. Another thread of
# the same core takes the turn when nobody has it, when its holder has been inside a system call for 0.3 ms, when
# the holder has had it for 10 ms, or at once when its own priority is higher. Threads not tied to one core are
# as before. Since the set 0174 a thread whose wait is over takes the turn back before it goes on inside its system
# call, and a lightweight mutex let go of is left for its waiter to take with the turn (the patch of
# sync_primitives.h says why). With EMUSUNI_VITA_TEST_SLOW=<start of a thread's name>:<microseconds> (the PC only) the threads so
# named sleep that long before each stretch of the game's code: the bench's way to have the two threads cross.
CORES_FN = ('#ifdef EMUSUNI_PORT\n'
            '#include <chrono>\n'
            '#include <condition_variable>\n'
            '#include <cstdlib>\n'
            '#include <cstring>\n'
            '#include <thread>\n'
            'namespace {\n'
            '// (EmuSuNi, the "coreturns" switch) One of the Vita\'s cores: the thread that has its turn.\n'
            'struct EmusuniCore {\n'
            '    std::mutex mutex;\n'
            '    std::condition_variable freed;\n'
            '    SceUID owner = 0; // (0: nobody)\n'
            '    int owner_priority = 0;\n'
            '    uint64_t owned_since = 0; // (ns)\n'
            '    uint64_t in_call_since = 0; // when its holder went into a system call (0: it runs the game\'s code)\n'
            '};\n'
            'EmusuniCore emusuni_cores[3];\n'
            '\n'
            'uint64_t emusuni_core_now() {\n'
            '    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());\n'
            '}\n'
            '\n'
            'EmusuniCore *emusuni_core_of(SceInt32 affinity_mask) {\n'
            '    if (!emusuni_vita_switch(28))\n'
            '        return nullptr;\n'
            '    switch (affinity_mask & 0x70000) {\n'
            '    case 0x10000:\n'
            '        return &emusuni_cores[0];\n'
            '    case 0x20000:\n'
            '        return &emusuni_cores[1];\n'
            '    case 0x40000:\n'
            '        return &emusuni_cores[2];\n'
            '    default:\n'
            '        return nullptr;\n'
            '    }\n'
            '}\n'
            '\n'
            '// The thread is about to run the game\'s code: it waits for its core\'s turn.\n'
            'void emusuni_core_run(SceUID id, SceInt32 affinity_mask, int priority) {\n'
            '    EmusuniCore *const core = emusuni_core_of(affinity_mask);\n'
            '    if (!core)\n'
            '        return;\n'
            '    std::unique_lock<std::mutex> lock(core->mutex);\n'
            '    if (core->owner == id) {\n'
            '        core->in_call_since = 0;\n'
            '        return;\n'
            '    }\n'
            '    const unsigned long long from = emusuni_vita_stat_now();\n'
            '    int how = 0;\n'
            '    for (;;) {\n'
            '        const uint64_t now = emusuni_core_now();\n'
            '        if (core->owner == 0)\n'
            '            break;\n'
            '        if (core->in_call_since != 0 && now - core->in_call_since >= 300000ull) {\n'
            '            how = 29; // (its holder is inside a system call: waiting for something, most likely)\n'
            '            break;\n'
            '        }\n'
            '        if (now - core->owned_since >= 10000000ull) {\n'
            '            how = 30; // (its holder has had it for 10 ms)\n'
            '            break;\n'
            '        }\n'
            '        if (priority < core->owner_priority) {\n'
            '            how = 31; // (a lower number is a higher priority)\n'
            '            break;\n'
            '        }\n'
            '        core->freed.wait_for(lock, std::chrono::microseconds(300));\n'
            '    }\n'
            '    core->owner = id;\n'
            '    core->owner_priority = priority;\n'
            '    core->owned_since = emusuni_core_now();\n'
            '    core->in_call_since = 0;\n'
            '    lock.unlock();\n'
            '    emusuni_vita_stat_add(28, emusuni_vita_stat_now() - from);\n'
            '    if (how != 0)\n'
            '        emusuni_vita_stat_add(how, 0);\n'
            '}\n'
            '\n'
            '// The thread goes into a system call with the turn: from now on it can be taken from it.\n'
            'void emusuni_core_call(SceUID id, SceInt32 affinity_mask) {\n'
            '    EmusuniCore *const core = emusuni_core_of(affinity_mask);\n'
            '    if (!core)\n'
            '        return;\n'
            '    const std::lock_guard<std::mutex> lock(core->mutex);\n'
            '    if (core->owner == id)\n'
            '        core->in_call_since = emusuni_core_now();\n'
            '}\n'
            '\n'
            '// The thread waits or stops running: the turn is free.\n'
            'void emusuni_core_give(SceUID id, SceInt32 affinity_mask) {\n'
            '    EmusuniCore *const core = emusuni_core_of(affinity_mask);\n'
            '    if (!core)\n'
            '        return;\n'
            '    {\n'
            '        const std::lock_guard<std::mutex> lock(core->mutex);\n'
            '        if (core->owner != id)\n'
            '            return;\n'
            '        core->owner = 0;\n'
            '    }\n'
            '    core->freed.notify_all();\n'
            '}\n'
            '\n'
            '// The thread\'s wait is over: it goes on with the system call it is in - which may take a mutex for it -\n'
            '// only with its core\'s turn, as on the Vita, where a thread that was woken does nothing until its core\n'
            '// runs it.\n'
            'void emusuni_core_back(SceUID id, SceInt32 affinity_mask, int priority) {\n'
            '    if (!emusuni_core_of(affinity_mask))\n'
            '        return;\n'
            '    emusuni_core_run(id, affinity_mask, priority);\n'
            '    emusuni_core_call(id, affinity_mask);\n'
            '}\n'
            '\n'
            '#if !defined(__PS4__)\n'
            '// (A test, on the PC only: EMUSUNI_VITA_TEST_SLOW=<start of a thread\'s name>:<microseconds>. The sleep\n'
            '// is not counted in the 10 ms a thread may keep its core\'s turn.)\n'
            'void emusuni_test_slow(const std::string &name, SceUID id, SceInt32 affinity_mask) {\n'
            '    static const char *const text = getenv("EMUSUNI_VITA_TEST_SLOW");\n'
            '    static const char *const colon = text ? strchr(text, \':\') : nullptr;\n'
            '    static const int micros = colon ? atoi(colon + 1) : 0;\n'
            '    if (micros <= 0 || name.compare(0, static_cast<size_t>(colon - text), text, static_cast<size_t>(colon - text)) != 0)\n'
            '        return;\n'
            '    const uint64_t from = emusuni_core_now();\n'
            '    std::this_thread::sleep_for(std::chrono::microseconds(micros));\n'
            '    if (EmusuniCore *const core = emusuni_core_of(affinity_mask)) {\n'
            '        const std::lock_guard<std::mutex> lock(core->mutex);\n'
            '        if (core->owner == id)\n'
            '            core->owned_since += emusuni_core_now() - from;\n'
            '    }\n'
            '}\n'
            '#endif\n'
            '} // namespace\n'
            '\n'
            '// (For the kernel\'s mutexes, sync_primitives.cpp: is a thread with these cores tied to one of them, with\n'
            '// the switch ON?)\n'
            'extern "C" int emusuni_vita_core_tied(int affinity_mask) {\n'
            '    return emusuni_core_of(static_cast<SceInt32>(affinity_mask)) != nullptr ? 1 : 0;\n'
            '}\n'
            '#endif\n')

# A guest thread's loop: its time in recompiled code and in the system call that stopped it (a callback's loop,
# inside a call, is taken out of that call's time).
patch('vita3k/kernel/src/thread.cpp', [
    ('void ThreadState::run_loop(bool top_level) {\n    bool guest_returned = false;\n',
     STAT_API + SWITCH_API + CORES_FN +
     'void ThreadState::run_loop(bool top_level) {\n'
     '    bool guest_returned = false;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    struct EmusuniStatLevel {\n'
     '        const int top;\n'
     '        const unsigned long long from = emusuni_vita_stat_now();\n'
     '        const unsigned long long outer;\n'
     '        EmusuniStatLevel(const char *name, int id, bool top_level)\n'
     '            : top(top_level ? 1 : 0)\n'
     '            , outer(emusuni_vita_stat_level_begin(name, id, top_level ? 1 : 0)) {}\n'
     '        ~EmusuniStatLevel() { emusuni_vita_stat_level_end(top, outer, emusuni_vita_stat_now() - from); }\n'
     '    } emusuni_stat_level(name.c_str(), static_cast<int>(id), top_level);\n'
     '#endif\n'),
    ('            const int res = do_step ? step(*cpu) : run(*cpu);\n',
     '#ifdef EMUSUNI_PORT\n'
     '            emusuni_core_run(id, affinity_mask, priority);\n'
     '#if !defined(__PS4__)\n'
     '            emusuni_test_slow(name, id, affinity_mask);\n'
     '#endif\n'
     '            const unsigned long long emusuni_stat_from = emusuni_vita_stat_now();\n'
     '#endif\n'
     '            const int res = do_step ? step(*cpu) : run(*cpu);\n'
     '#ifdef EMUSUNI_PORT\n'
     '            const unsigned long long emusuni_stat_ran = emusuni_vita_stat_now();\n'
     '#endif\n'),
    ('                kernel.call_import(*cpu, nid, id);\n',
     '#ifdef EMUSUNI_PORT\n'
     '                emusuni_core_call(id, affinity_mask);\n'
     '#endif\n'
     '                kernel.call_import(*cpu, nid, id);\n'),
    # (the turn is given up where the thread stops running: it parks - stopped, suspended, ended - or its loop is
    # left for good. Since the set 0174 not where a callback's loop, inside a system call, goes back into that
    # call: its thread goes on.)
    ('                guest_returned = true;\n            }\n        }\n    }\n}\n',
     '                guest_returned = true;\n            }\n        }\n    }\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (top_level)\n'
     '        emusuni_core_give(id, affinity_mask);\n'
     '#endif\n'
     '}\n'),
    ('            status_cond.wait(lock, [&] { return status == ThreadStatus::running || exiting(); });\n',
     '#ifdef EMUSUNI_PORT\n'
     '            emusuni_core_give(id, affinity_mask);\n'
     '#endif\n'
     '            status_cond.wait(lock, [&] { return status == ThreadStatus::running || exiting(); });\n'),
    # (since the set 0174: the wait over, the thread takes its core's turn back before it goes on inside its call)
    ('    wake_pending = false;\n    if (callbacks_due)\n        return SCE_KERNEL_ERROR_NOTIFY_CALLBACK;\n',
     '    wake_pending = false;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    {\n'
     '        const SceInt32 emusuni_mask = affinity_mask;\n'
     '        const int emusuni_priority = priority;\n'
     '        lock.unlock();\n'
     '        emusuni_core_back(id, emusuni_mask, emusuni_priority);\n'
     '    }\n'
     '#endif\n'
     '    if (callbacks_due)\n        return SCE_KERNEL_ERROR_NOTIFY_CALLBACK;\n'),
    ('    wait_target = target;\n    update_status(ThreadStatus::waiting);\n',
     '    wait_target = target;\n    update_status(ThreadStatus::waiting);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_core_give(id, affinity_mask);\n'
     '#endif\n'),
    ('void ThreadState::begin_host_wait() {\n    const std::lock_guard<std::mutex> lock(mutex);\n    update_status(ThreadStatus::waiting);\n',
     'void ThreadState::begin_host_wait() {\n    const std::lock_guard<std::mutex> lock(mutex);\n    update_status(ThreadStatus::waiting);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_core_give(id, affinity_mask);\n'
     '#endif\n'),
    ('                dispatch_abort(*cpu);\n\n            lock.lock();\n',
     '                dispatch_abort(*cpu);\n\n'
     '#ifdef EMUSUNI_PORT\n'
     '            emusuni_vita_stat_guest(emusuni_stat_ran - emusuni_stat_from, emusuni_vita_stat_now() - emusuni_stat_ran, syscall ? 1 : 0);\n'
     '#endif\n'
     '            lock.lock();\n'),
    # (19-11, the switch "profile", port/ps4_vita_profile.cpp) A guest thread's name, on the thread, for the console's
    # CPU profile (its loop at the top only: a callback's inside a call changes nothing).
    ('void ThreadState::run_loop(bool top_level) {\n    bool guest_returned = false;\n#ifdef EMUSUNI_PORT\n    struct EmusuniStatLevel {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void emusuni_vita_prof_thread(const char *name, int id, int how);\n'
     '#endif\n'
     'void ThreadState::run_loop(bool top_level) {\n    bool guest_returned = false;\n#ifdef EMUSUNI_PORT\n    struct EmusuniStatLevel {\n'),
    ('    } emusuni_stat_level(name.c_str(), static_cast<int>(id), top_level);\n#endif\n',
     '    } emusuni_stat_level(name.c_str(), static_cast<int>(id), top_level);\n'
     '    struct EmusuniProfThread {\n'
     '        const int top;\n'
     '        EmusuniProfThread(const char *name, int id, bool top_level)\n'
     '            : top(top_level ? 1 : 0) { emusuni_vita_prof_thread(name, id, top); }\n'
     '        ~EmusuniProfThread() { emusuni_vita_prof_thread(nullptr, 0, -top); }\n'
     '    } emusuni_prof_thread(name.c_str(), static_cast<int>(id), top_level);\n'
     '#endif\n'),
    # (19-11, the switch "vitaplace", port/ps4_vita_place.cpp) Each run of recompiled code told to it: the busiest
    # threads get PS4 cores of their own (that file says how).
    ('void ThreadState::run_loop(bool top_level) {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void emusuni_vita_place_tick(int id, const char *name, unsigned long long ticks);\n'
     '#endif\n'
     'void ThreadState::run_loop(bool top_level) {\n'),
    ('            const unsigned long long emusuni_stat_ran = emusuni_vita_stat_now();\n',
     '            const unsigned long long emusuni_stat_ran = emusuni_vita_stat_now();\n'
     '            emusuni_vita_place_tick(static_cast<int>(id), name.c_str(), emusuni_stat_ran - emusuni_stat_from);\n'),
    # (19-11, for 01.83, "stubcall") the number of a stub's call that came back to the loop
    ('void ThreadState::run_loop(bool top_level) {\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" unsigned emusuni_vita_stub_nid(unsigned slot);\n'
     '#endif\n'
     'void ThreadState::run_loop(bool top_level) {\n'),
    ('                const uint32_t nid = *Ptr<uint32_t>(read_pc(*cpu) + 4).get(mem);\n',
     '#ifdef EMUSUNI_PORT\n'
     '                // (19-11, "stubcall": a stub\'s block whose call came back here says its slot, bit 31 set, and the\n'
     '                // program counter is already the caller\'s return address)\n'
     '                const uint32_t nid = cpu->svc >= 0x80000000u ? emusuni_vita_stub_nid(cpu->svc & 0x7fffffffu) : *Ptr<uint32_t>(read_pc(*cpu) + 4).get(mem);\n'
     '#else\n'
     '                const uint32_t nid = *Ptr<uint32_t>(read_pc(*cpu) + 4).get(mem);\n'
     '#endif\n'),
    # (20-11, for 01.84) the "sixcores" switch (56, OFF): port/ps4_vita_place.cpp
    ('void ThreadState::run_loop(bool top_level) {\n',
     '#ifdef EMUSUNI_PORT\nextern "C" void emusuni_vita_sixcores_here(int who);\n#endif\nvoid ThreadState::run_loop(bool top_level) {\n'),
    ('    } emusuni_prof_thread(name.c_str(), static_cast<int>(id), top_level);\n#endif\n',
     '    } emusuni_prof_thread(name.c_str(), static_cast<int>(id), top_level);\n    // (20-11, "sixcores", 56) a guest thread on the PS4\'s cores 0-5 as it starts (port/ps4_vita_place.cpp)\n    if (top_level)\n        emusuni_vita_sixcores_here(0);\n#endif\n'),
])
# A block the recompiler makes (inside the guest thread's time in its code).
patch('external/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp', [
    ('namespace Dynarmic::A32 {\n', STAT_API + SWITCH_API + 'namespace Dynarmic::A32 {\n'),
    # (01.34) A thread's code memory is emptied and its code made again when it is full, as upstream, and also
    # when the memory of all the threads' is at its limit and this one would take more (port/ps4_vita_sys.c).
    ('        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE || emusuni_vita_jit_tight(block_of_code.getCurr())) {\n'
     '#else\n'
     '        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {\n'
     '#endif\n'),
    ('            invalidate_entire_cache = true;\n            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);\n        }\n'
     '        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);\n',
     '            invalidate_entire_cache = true;\n            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);\n'
     '#ifdef EMUSUNI_PORT\n'
     '            emusuni_vita_stat_add(6, 0);\n'
     '#endif\n'
     '        }\n'
     '        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);\n'),
    ('        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);\n',
     '        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        const EmusuniVitaStatScope emusuni_stat_block(1);\n'
     '#endif\n'),
    # (17-11) A block the dispatcher had to look up (the jumps the blocks don't make themselves), counted.
    ('    CodePtr GetCurrentBlock() {\n',
     '    CodePtr GetCurrentBlock() {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_stat_tl(1);\n'
     '#endif\n'),
], repo='external/dynarmic')
# (01.34) The "hints" switch: an ARM hint the emulator does nothing for (a preload, a yield, an event - Vita3K's
# handler only breaks out of its switch for them) is no code at all. Upstream each one ends its block with a call
# out and a new look for the next block: a copy loop with a preload in it pays that at every turn. The wait for
# an interrupt, which is how Vita3K stops a thread, and the breakpoint stay as they are.
# (19-11, the performance specialist's) The switch "movpclr" (44 of the port's table, OFF: /data/EmuSuNi/vita-movpclr-on;
# the PC: EMUSUNI_VITA_MOVPCLR=1), read once, when the first block is translated. Every import stub Vita3K writes ends
# with the ARM MOV PC, LR (kernel/src/load_self.cpp, modules/module_parent.cpp, taiHEN): upstream dynarmic ends the block
# of a MOV to the PC with ReturnToDispatch - the dispatcher's loop, a C++ look-up of the next block (GetCurrentBlock, an
# unordered_map), and the caller's return address left on the return stack buffer, which then misses the returns after
# it. So every system call made through a stub - inline or not - came back that way: the log's "blocks looked up by the
# dispatcher" is the number of system calls (01.78: MotorStorm 3.72 and 3.71 million every 10 s, Uncharted 4.31 and 4.29).
# With the switch a MOV PC, LR with no shift ends its block as BX LR does (PopRSBHint): the same PC and Thumb bit
# (ALUWritePC is BXWritePC on ARMv7), only how the next block is found differs.
patch('external/dynarmic/src/dynarmic/frontend/A32/translate/impl/data_processing.cpp', [
    ('namespace Dynarmic::A32 {\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <cstdlib>\n'
     '#include <cstring>\n'
     '#include <unistd.h>\n'
     'extern "C" void emusuni_vita_log(const char *format, ...);\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     '// (EmuSuNi, the switch "movpclr", 44 of the table in port/ps4_vita_sys.c) MOV PC, LR ends its block as a return: read once.\n'
     'static bool emusuni_movpclr() {\n'
     '    static const bool on = [] {\n'
     '        const bool yes = emusuni_vita_switch(44) != 0;\n'
     '        emusuni_vita_log("vita: movpclr: MOV PC, LR (the end of every import stub) ends its block as a return, with the "\n'
     '                         "return stack buffer: %s", yes ? "on (the file vita-movpclr-on; read once)" : "off (the file vita-movpclr-on makes it on; read once)");\n'
     '        return yes;\n'
     '    }();\n'
     '    return on;\n'
     '}\n'
     '#endif\n'
     'namespace Dynarmic::A32 {\n'),
    ('bool TranslatorVisitor::arm_MOV_reg(Cond cond, bool S, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {\n'
     '    if (!ArmConditionPassed(cond)) {\n'
     '        return true;\n'
     '    }\n'
     '\n'
     '    const auto carry_in = ir.GetCFlag();\n'
     '    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);\n'
     '    const auto result = shifted.result;\n'
     '    if (d == Reg::PC) {\n'
     '        if (S) {\n'
     '            // This is UNPREDICTABLE when in user-mode.\n'
     '            return UnpredictableInstruction();\n'
     '        }\n'
     '\n'
     '        ir.ALUWritePC(result);\n'
     '        ir.SetTerm(IR::Term::ReturnToDispatch{});\n',
     'bool TranslatorVisitor::arm_MOV_reg(Cond cond, bool S, Reg d, Imm<5> imm5, ShiftType shift, Reg m) {\n'
     '    if (!ArmConditionPassed(cond)) {\n'
     '        return true;\n'
     '    }\n'
     '\n'
     '    const auto carry_in = ir.GetCFlag();\n'
     '    const auto shifted = EmitImmShift(ir.GetRegister(m), shift, imm5, carry_in);\n'
     '    const auto result = shifted.result;\n'
     '    if (d == Reg::PC) {\n'
     '        if (S) {\n'
     '            // This is UNPREDICTABLE when in user-mode.\n'
     '            return UnpredictableInstruction();\n'
     '        }\n'
     '\n'
     '        ir.ALUWritePC(result);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        // (the switch "movpclr") MOV PC, LR with no shift: a return, as BX LR\n'
     '        if (m == Reg::LR && shift == ShiftType::LSL && imm5.ZeroExtend() == 0 && emusuni_movpclr()) {\n'
     '            ir.SetTerm(IR::Term::PopRSBHint{});\n'
     '            return false;\n'
     '        }\n'
     '#endif\n'
     '        ir.SetTerm(IR::Term::ReturnToDispatch{});\n'),
], repo='external/dynarmic')
# (19-11, the performance specialist's) The switch "vfp64" (46 of the port's table, OFF: /data/EmuSuNi/vita-vfp64-on; the
# PC: EMUSUNI_VITA_VFP64=1), read once, when the first block is translated: a double register's load or store (VLDR,
# VSTR, VLDM, VSTM) one 64-bit access, not two 32-bit ones and a pack (MotorStorm's hottest blocks are vector maths: 8 x86
# instructions for each double loaded, 3 with it). Little-endian only.
patch('external/dynarmic/src/dynarmic/frontend/A32/translate/impl/vfp.cpp', [
    ('namespace Dynarmic::A32 {\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <cstdlib>\n'
     '#include <unistd.h>\n'
     'extern "C" void emusuni_vita_log(const char *format, ...);\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     '// (EmuSuNi, the switch "vfp64", 46 of the table in port/ps4_vita_sys.c) a double register loaded or stored with one 64-bit access: read once.\n'
     'static bool emusuni_vfp64() {\n'
     '    static const bool on = [] {\n'
     '        const bool yes = emusuni_vita_switch(46) != 0;\n'
     '        emusuni_vita_log("vita: vfp64: a double register\'s VLDR/VSTR/VLDM/VSTM as one 64-bit access: %s",\n'
     '                         yes ? "on (the file vita-vfp64-on; read once)" : "off (the file vita-vfp64-on makes it on; read once)");\n'
     '        return yes;\n'
     '    }();\n'
     '    return on;\n'
     '}\n'
     '#endif\n'
     'namespace Dynarmic::A32 {\n'),
    ('    if (sz) {\n        auto lo = ir.ReadMemory32(address, IR::AccType::ATOMIC);\n',
     '    if (sz) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (!ir.current_location.EFlag() && emusuni_vfp64()) {\n'
     '            ir.SetExtendedRegister(d, ir.ReadMemory64(address, IR::AccType::ATOMIC));\n'
     '            return true;\n'
     '        }\n'
     '#endif\n'
     '        auto lo = ir.ReadMemory32(address, IR::AccType::ATOMIC);\n'),
    ('    if (sz) {\n        const auto reg_d = ir.GetExtendedRegister(d);\n        auto lo = ir.LeastSignificantWord(reg_d);\n',
     '    if (sz) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (!ir.current_location.EFlag() && emusuni_vfp64()) {\n'
     '            ir.WriteMemory64(address, ir.GetExtendedRegister(d), IR::AccType::ATOMIC);\n'
     '            return true;\n'
     '        }\n'
     '#endif\n'
     '        const auto reg_d = ir.GetExtendedRegister(d);\n        auto lo = ir.LeastSignificantWord(reg_d);\n'),
    ('    for (size_t i = 0; i < regs; i++) {\n        auto word1 = ir.ReadMemory32(address, IR::AccType::ATOMIC);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (!ir.current_location.EFlag() && emusuni_vfp64()) {\n'
     '        for (size_t i = 0; i < regs; i++) {\n'
     '            ir.SetExtendedRegister(d + i, ir.ReadMemory64(address, IR::AccType::ATOMIC));\n'
     '            address = ir.Add(address, ir.Imm32(8));\n'
     '        }\n'
     '        return true;\n'
     '    }\n'
     '#endif\n'
     '    for (size_t i = 0; i < regs; i++) {\n        auto word1 = ir.ReadMemory32(address, IR::AccType::ATOMIC);\n'),
    ('    for (size_t i = 0; i < regs; i++) {\n        const auto value = ir.GetExtendedRegister(d + i);\n        auto word1 = ir.LeastSignificantWord(value);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (!ir.current_location.EFlag() && emusuni_vfp64()) {\n'
     '        for (size_t i = 0; i < regs; i++) {\n'
     '            ir.WriteMemory64(address, ir.GetExtendedRegister(d + i), IR::AccType::ATOMIC);\n'
     '            address = ir.Add(address, ir.Imm32(8));\n'
     '        }\n'
     '        return true;\n'
     '    }\n'
     '#endif\n'
     '    for (size_t i = 0; i < regs; i++) {\n        const auto value = ir.GetExtendedRegister(d + i);\n        auto word1 = ir.LeastSignificantWord(value);\n'),
], repo='external/dynarmic')
patch('external/dynarmic/src/dynarmic/frontend/A32/translate/impl/a32_translate_impl.cpp', [
    ('namespace Dynarmic::A32 {\n', SWITCH_API + 'namespace Dynarmic::A32 {\n'),
    ('bool TranslatorVisitor::RaiseException(Exception exception) {\n',
     'bool TranslatorVisitor::RaiseException(Exception exception) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    switch (exception) {\n'
     '    case Exception::PreloadData:\n'
     '    case Exception::PreloadDataWithIntentToWrite:\n'
     '    case Exception::PreloadInstruction:\n'
     '    case Exception::SendEvent:\n'
     '    case Exception::SendEventLocal:\n'
     '    case Exception::WaitForEvent:\n'
     '    case Exception::Yield:\n'
     '        if (emusuni_vita_switch(4))\n'
     '            return true;\n'
     '        break;\n'
     '    default:\n'
     '        break;\n'
     '    }\n'
     '#endif\n'),
], repo='external/dynarmic')
# (After 01.73) The "monitor64" switch (29, ON): with "fastatomics" the recompiler does every exclusive load and
# store inline and - our choice of its options - without the monitor: a store goes through when the memory still
# holds the value the load saw. For the 64-bit ones (LDREXD/STREXD) that is not enough: a queue without a lock
# keeps its head in such a pair of words, and a head taken out and put back by another thread in between has
# the same value (Uncharted stopped for good on the bench with "fastatomics", set 0173; HENkaku-PS4's notes say
# the same of it). With the switch those two go the recompiler's other way, through the monitor every thread
# shares: a store by any thread there takes the others' reservations away. The 8, 16 and 32-bit ones stay inline.
patch('external/dynarmic/src/dynarmic/backend/x64/a32_emit_x64_memory.cpp', [
    ('namespace Dynarmic::Backend::X64 {\n', SWITCH_API + 'namespace Dynarmic::Backend::X64 {\n'),
    ('    if (conf.fastmem_exclusive_access) {\n'
     '        EmitExclusiveReadMemoryInline<64, &A32::UserCallbacks::MemoryRead64>(ctx, inst);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (conf.fastmem_exclusive_access && !emusuni_vita_switch(29)) {\n'
     '#else\n'
     '    if (conf.fastmem_exclusive_access) {\n'
     '#endif\n'
     '        EmitExclusiveReadMemoryInline<64, &A32::UserCallbacks::MemoryRead64>(ctx, inst);\n'),
    ('    if (conf.fastmem_exclusive_access) {\n'
     '        EmitExclusiveWriteMemoryInline<64, &A32::UserCallbacks::MemoryWriteExclusive64>(ctx, inst);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (conf.fastmem_exclusive_access && !emusuni_vita_switch(29)) {\n'
     '#else\n'
     '    if (conf.fastmem_exclusive_access) {\n'
     '#endif\n'
     '        EmitExclusiveWriteMemoryInline<64, &A32::UserCallbacks::MemoryWriteExclusive64>(ctx, inst);\n'),
], repo='external/dynarmic')
# (01.34) The "mxcsr" switch: the guest's floating point state back after a call out of the recompiled code (a
# system call, a hint). Upstream puts it back only inside its cycle counting, which Vita3K turns off: the host's
# state then stays, and the way out of the recompiler at the call's halt saves the host's over the guest's - what
# the game had set (flush-to-zero, a rounding mode) is gone from each thread's first system call on.
# (19-11, the performance specialist's; port/ps4_vita_jitopt.cpp) The switches "condflags" and "rsbsite": what
# a32_emit_x64.cpp's terminals need to know (members of every build: the header is the same everywhere).
patch('external/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h', [
    ('    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64) = nullptr;\n',
     '    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64) = nullptr;\n'
     '    // (EmuSuNi) "rsbsite": where the shared handlers go on after a miss of the RSB / of the fast dispatch\n'
     '    // table (rbx the location, ebp the PC / rbp the table\'s entry); "condflags": the block being made ends\n'
     '    // with the CPU\'s flags holding the guest\'s.\n'
     '    const void* emusuni_rsb_miss = nullptr;\n'
     '    const void* emusuni_fd_miss = nullptr;\n'
     '    bool emusuni_flags_live = false;\n'),
], repo='external/dynarmic')
patch('external/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.cpp', [
    ('namespace Dynarmic::Backend::X64 {\n', SWITCH_API + 'namespace Dynarmic::Backend::X64 {\n'),
    ('    Devirtualize<&A32::UserCallbacks::CallSVC>(conf.callbacks).EmitCall(code);\n',
     '    Devirtualize<&A32::UserCallbacks::CallSVC>(conf.callbacks).EmitCall(code);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (!conf.enable_cycle_counting && emusuni_vita_switch(7))\n'
     '        code.SwitchMxcsrOnEntry();\n'
     '#endif\n'),
    ('        code.mov(param[1], exception);\n    });\n',
     '        code.mov(param[1], exception);\n    });\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (!conf.enable_cycle_counting && emusuni_vita_switch(7))\n'
     '        code.SwitchMxcsrOnEntry();\n'
     '#endif\n'),
    # (19-11, the performance specialist's) The switch "nzcvword" (45 of the port's table, OFF:
    # /data/EmuSuNi/vita-nzcvword-on; the PC: EMUSUNI_VITA_NZCVWORD=1), read once, when the first block is made. An
    # instruction that sets N and Z only (ANDS, MOVS, MULS...) or N, Z and C (TST, LSLS...) wrote one byte of the flags'
    # word (cpsr_nzcv + 1); the condition of the branch right after it (EmitCond) and the carry reads (EmitGetFlag) read
    # the whole word: the CPU cannot forward a byte store to a word load and waits for the store to finish. In the
    # hottest block of Uncharted's main thread (its tree search: ADDS, ANDS, BEQ) 46 of 69 samples sat on that wait.
    # With the switch both write the whole word (read, merge, write): the same flags, a forwardable store.
    ('namespace Dynarmic::Backend::X64 {\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <cstdlib>\n'
     '#include <unistd.h>\n'
     'extern "C" void emusuni_vita_log(const char *format, ...);\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     '// (EmuSuNi, the switch "nzcvword", 45 of the table in port/ps4_vita_sys.c) the flags\' word written whole: read once.\n'
     'static bool emusuni_nzcv_word() {\n'
     '    static const bool on = [] {\n'
     '        const bool yes = emusuni_vita_switch(45) != 0;\n'
     '        emusuni_vita_log("vita: nzcvword: the guest\'s N, Z (and C) written as the whole flags word, not one byte of it: %s",\n'
     '                         yes ? "on (the file vita-nzcvword-on; read once)" : "off (the file vita-nzcvword-on makes it on; read once)");\n'
     '        return yes;\n'
     '    }();\n'
     '    return on;\n'
     '}\n'
     '#endif\n'
     'namespace Dynarmic::Backend::X64 {\n'),
    ('void A32EmitX64::EmitA32SetCpsrNZ(A32EmitContext& ctx, IR::Inst* inst) {\n'
     '    auto args = ctx.reg_alloc.GetArgumentInfo(inst);\n',
     'void A32EmitX64::EmitA32SetCpsrNZ(A32EmitContext& ctx, IR::Inst* inst) {\n'
     '    auto args = ctx.reg_alloc.GetArgumentInfo(inst);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_nzcv_word()) {\n'
     '        // (byte 1 of the word = (its bit 0, the C flag) | nz, as below; bytes 0, 2 and 3 kept)\n'
     '        const Xbyak::Reg32 nz = ctx.reg_alloc.UseScratchGpr(args[0]).cvt32();\n'
     '        const Xbyak::Reg32 tmp = ctx.reg_alloc.ScratchGpr().cvt32();\n'
     '        code.movzx(nz, nz.cvt8());\n'
     '        code.shl(nz, 8);\n'
     '        code.mov(tmp, dword[r15 + offsetof(A32JitState, cpsr_nzcv)]);\n'
     '        code.and_(tmp, 0xFFFF01FF);\n'
     '        code.or_(tmp, nz);\n'
     '        code.mov(dword[r15 + offsetof(A32JitState, cpsr_nzcv)], tmp);\n'
     '        return;\n'
     '    }\n'
     '#endif\n'),
    ('void A32EmitX64::EmitA32SetCpsrNZC(A32EmitContext& ctx, IR::Inst* inst) {\n'
     '    auto args = ctx.reg_alloc.GetArgumentInfo(inst);\n',
     'void A32EmitX64::EmitA32SetCpsrNZC(A32EmitContext& ctx, IR::Inst* inst) {\n'
     '    auto args = ctx.reg_alloc.GetArgumentInfo(inst);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_nzcv_word()) {\n'
     '        // (byte 1 of the word = nz | c - nz taken as 0 when it is an immediate, as below; bytes 0, 2 and 3 kept)\n'
     '        const bool nz_imm = args[0].IsImmediate();\n'
     '        const bool c_imm = args[1].IsImmediate();\n'
     '        Xbyak::Reg32 nz_reg, c_reg;\n'
     '        if (!nz_imm)\n'
     '            nz_reg = ctx.reg_alloc.UseGpr(args[0]).cvt32();\n'
     '        if (!c_imm)\n'
     '            c_reg = ctx.reg_alloc.UseGpr(args[1]).cvt32();\n'
     '        const Xbyak::Reg32 v = ctx.reg_alloc.ScratchGpr().cvt32();\n'
     '        const Xbyak::Reg32 tmp = ctx.reg_alloc.ScratchGpr().cvt32();\n'
     '        if (nz_imm)\n'
     '            code.xor_(v, v);\n'
     '        else\n'
     '            code.mov(v, nz_reg);\n'
     '        if (c_imm) {\n'
     '            if (args[1].GetImmediateU1())\n'
     '                code.or_(v, 1);\n'
     '        } else {\n'
     '            code.or_(v, c_reg);\n'
     '        }\n'
     '        code.movzx(v, v.cvt8());\n'
     '        code.shl(v, 8);\n'
     '        code.mov(tmp, dword[r15 + offsetof(A32JitState, cpsr_nzcv)]);\n'
     '        code.and_(tmp, 0xFFFF00FF);\n'
     '        code.or_(tmp, v);\n'
     '        code.mov(dword[r15 + offsetof(A32JitState, cpsr_nzcv)], tmp);\n'
     '        return;\n'
     '    }\n'
     '#endif\n'),
    # (19-11, the performance specialist's; port/ps4_vita_jitopt.cpp) The switches "condflags" (a block's conditional
    # branch on the CPU's flags as its ADD/SUB/CMP left them, not on the stored ones read back) and "rsbsite" (each
    # return and indirect branch with its own indirect jump, not the shared handler's one), both OFF.
    ('namespace Dynarmic::Backend::X64 {\n',
     '#ifdef EMUSUNI_PORT\n'
     '// (EmuSuNi, port/ps4_vita_jitopt.cpp) the switches "condflags" and "rsbsite", read once; the log\'s counts.\n'
     'extern "C" int emusuni_vita_condflags(void);\n'
     'extern "C" int emusuni_vita_rsbsite(void);\n'
     'extern "C" void emusuni_vita_condflags_seen(int fused);\n'
     'extern "C" void emusuni_vita_rsbsite_seen(int kind);\n'
     '#endif\n'
     'namespace Dynarmic::Backend::X64 {\n'),
    ('A32EmitX64::BlockDescriptor A32EmitX64::Emit(IR::Block& block) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '// (EmuSuNi, the switch "condflags") Do the CPU\'s flags at the block\'s end hold the guest\'s N, Z, C and V as stored? They\n'
     '// do when the block\'s last flags come from an Add32/Sub32 that keeps them (its GetNZCVFromOp: lahf and seto right after\n'
     '// the add, sub or cmp - so the stored word is the CPU\'s flags) or from a GetNZCVFromOp of its own (test; lahf; mov al, 0:\n'
     '// C and V 0 in both), stored whole by SetCpsrNZCV, and every instruction after it only moves guest registers (no code\n'
     '// that changes the flags: mov, movd; the register allocator\'s spills are moves too).\n'
     'static bool EmusuniFlagsLiveAtEnd(IR::Block& block) {\n'
     '    const IR::Inst* producer = nullptr;\n'
     '    const IR::Inst* nzcv = nullptr;\n'
     '    bool stored = false;\n'
     '    for (auto& inst : block) {\n'
     '        bool keep = false;\n'
     '        switch (inst.GetOpcode()) {\n'
     '        case IR::Opcode::A32GetRegister:\n'
     '        case IR::Opcode::A32SetRegister:\n'
     '        case IR::Opcode::GetCarryFromOp:\n'
     '        case IR::Opcode::GetOverflowFromOp:\n'
     '        case IR::Opcode::Void:\n'
     '        case IR::Opcode::Identity:\n'
     '            keep = true;\n'
     '            break;\n'
     '        case IR::Opcode::Add32:\n'
     '        case IR::Opcode::Sub32:\n'
     '            if (inst.GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp) != nullptr) {\n'
     '                producer = &inst;\n'
     '                nzcv = nullptr;\n'
     '                stored = false;\n'
     '                keep = true;\n'
     '            }\n'
     '            break;\n'
     '        case IR::Opcode::GetNZCVFromOp:\n'
     '            if (!inst.GetArg(0).IsImmediate()) {\n'
     '                const IR::Inst* const of = inst.GetArg(0).GetInst();\n'
     '                const IR::Opcode o = of->GetOpcode();\n'
     '                if (o == IR::Opcode::Add32 || o == IR::Opcode::Sub32 || o == IR::Opcode::Add64 || o == IR::Opcode::Sub64) {\n'
     '                    keep = of == producer && nzcv == nullptr;  // (the add\'s or sub\'s own: no code of its own)\n'
     '                    if (keep)\n'
     '                        nzcv = &inst;\n'
     '                } else {\n'
     '                    producer = &inst;  // (its own test of the value)\n'
     '                    nzcv = &inst;\n'
     '                    stored = false;\n'
     '                    keep = true;\n'
     '                }\n'
     '            }\n'
     '            break;\n'
     '        case IR::Opcode::A32SetCpsrNZCV:\n'
     '            keep = nzcv != nullptr && !inst.GetArg(0).IsImmediate() && inst.GetArg(0).GetInst() == nzcv;\n'
     '            if (keep)\n'
     '                stored = true;\n'
     '            break;\n'
     '        default:\n'
     '            break;\n'
     '        }\n'
     '        if (!keep) {\n'
     '            producer = nullptr;\n'
     '            nzcv = nullptr;\n'
     '            stored = false;\n'
     '        }\n'
     '    }\n'
     '    return producer != nullptr && nzcv != nullptr && stored;\n'
     '}\n'
     '#endif\n'
     'A32EmitX64::BlockDescriptor A32EmitX64::Emit(IR::Block& block) {\n'),
    ('    EmitX64::EmitTerminal(block.GetTerminal(), ctx.Location().SetSingleStepping(false), ctx.IsSingleStep());\n'
     '    code.int3();\n',
     '#ifdef EMUSUNI_PORT\n'
     '    {\n'
     '        const IR::Terminal emusuni_term = block.GetTerminal();\n'
     '        const int emusuni_mode = boost::get<IR::Term::If>(&emusuni_term) != nullptr ? emusuni_vita_condflags() : 0;\n'
     '        emusuni_flags_live = emusuni_mode != 0 && !conf.enable_cycle_counting && EmusuniFlagsLiveAtEnd(block);\n'
     '        if (emusuni_mode != 0)\n'
     '            emusuni_vita_condflags_seen(emusuni_flags_live ? 1 : 0);\n'
     '    }\n'
     '#endif\n'
     '    EmitX64::EmitTerminal(block.GetTerminal(), ctx.Location().SetSingleStepping(false), ctx.IsSingleStep());\n'
     '    code.int3();\n'),
    ('        calculate_location_descriptor();\n'
     '        code.L(rsb_cache_miss);\n',
     '        calculate_location_descriptor();\n'
     '        code.L(rsb_cache_miss);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_rsb_miss = code.getCurr<const void*>();\n'
     '#endif\n'),
    ('        code.L(fast_dispatch_cache_miss);\n',
     '        code.L(fast_dispatch_cache_miss);\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_fd_miss = code.getCurr<const void*>();\n'
     '#endif\n'),
    ('    code.jmp(terminal_handler_pop_rsb_hint);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_rsb_miss != nullptr && emusuni_vita_rsbsite()) {\n'
     '        // (the switch "rsbsite") the shared handler\'s code, here: this return\'s own indirect jump\n'
     '        emusuni_vita_rsbsite_seen(0);\n'
     '        code.mov(ebx, dword[r15 + offsetof(A32JitState, upper_location_descriptor)]);\n'
     '        code.shl(rbx, 32);\n'
     '        code.mov(ecx, MJitStateReg(A32::Reg::PC));\n'
     '        code.mov(ebp, ecx);\n'
     '        code.or_(rbx, rcx);\n'
     '        code.mov(eax, dword[r15 + offsetof(A32JitState, rsb_ptr)]);\n'
     '        code.sub(eax, 1);\n'
     '        code.and_(eax, u32(A32JitState::RSBPtrMask));\n'
     '        code.mov(dword[r15 + offsetof(A32JitState, rsb_ptr)], eax);\n'
     '        code.cmp(rbx, qword[r15 + offsetof(A32JitState, rsb_location_descriptors) + rax * sizeof(u64)]);\n'
     '        code.jne(emusuni_rsb_miss);\n'
     '        code.jmp(qword[r15 + offsetof(A32JitState, rsb_codeptrs) + rax * sizeof(u64)]);\n'
     '        return;\n'
     '    }\n'
     '#endif\n'
     '    code.jmp(terminal_handler_pop_rsb_hint);\n'),
    ('    code.jmp(terminal_handler_fast_dispatch_hint);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_fd_miss != nullptr && emusuni_vita_rsbsite()) {\n'
     '        // (the switch "rsbsite") the shared handler\'s code, here: this branch\'s own indirect jump\n'
     '        emusuni_vita_rsbsite_seen(1);\n'
     '        code.mov(ebx, dword[r15 + offsetof(A32JitState, upper_location_descriptor)]);\n'
     '        code.shl(rbx, 32);\n'
     '        code.mov(ecx, MJitStateReg(A32::Reg::PC));\n'
     '        code.or_(rbx, rcx);\n'
     '        code.mov(r12, reinterpret_cast<u64>(fast_dispatch_table.data()));\n'
     '        code.mov(rbp, rbx);\n'
     '        if (code.HasHostFeature(HostFeature::SSE42)) {\n'
     '            code.crc32(rbp, r12);\n'
     '        }\n'
     '        code.and_(ebp, fast_dispatch_table_mask);\n'
     '        code.lea(rbp, ptr[r12 + rbp]);\n'
     '        code.cmp(rbx, qword[rbp + offsetof(FastDispatchEntry, location_descriptor)]);\n'
     '        code.jne(emusuni_fd_miss);\n'
     '        code.jmp(ptr[rbp + offsetof(FastDispatchEntry, code_ptr)]);\n'
     '        return;\n'
     '    }\n'
     '#endif\n'
     '    code.jmp(terminal_handler_fast_dispatch_hint);\n'),
    ('void A32EmitX64::EmitTerminalImpl(IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) {\n'
     '    Xbyak::Label pass = EmitCond(terminal.if_);\n',
     'void A32EmitX64::EmitTerminalImpl(IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_flags_live) {\n'
     '        // (the switch "condflags") the CPU\'s flags are the guest\'s here (EmusuniFlagsLiveAtEnd): the branch on them, as\n'
     '        // EmitCond\'s after its load (HI and LS: the carry turned over first, as there).\n'
     '        emusuni_flags_live = false;\n'
     '#if !defined(__PS4__)\n'
     '        const bool check = emusuni_vita_condflags() == 2;\n'
     '#else\n'
     '        const bool check = false;\n'
     '#endif\n'
     '        const auto jtype = check ? code.T_NEAR : code.T_AUTO;\n'
     '        Xbyak::Label host_pass;\n'
     '        bool done = true;\n'
     '        switch (terminal.if_) {\n'
     '        case IR::Cond::EQ: code.jz(host_pass, jtype); break;\n'
     '        case IR::Cond::NE: code.jnz(host_pass, jtype); break;\n'
     '        case IR::Cond::CS: code.jc(host_pass, jtype); break;\n'
     '        case IR::Cond::CC: code.jnc(host_pass, jtype); break;\n'
     '        case IR::Cond::MI: code.js(host_pass, jtype); break;\n'
     '        case IR::Cond::PL: code.jns(host_pass, jtype); break;\n'
     '        case IR::Cond::VS: code.jo(host_pass, jtype); break;\n'
     '        case IR::Cond::VC: code.jno(host_pass, jtype); break;\n'
     '        case IR::Cond::HI: code.cmc(); code.ja(host_pass, jtype); break;\n'
     '        case IR::Cond::LS: code.cmc(); code.jna(host_pass, jtype); break;\n'
     '        case IR::Cond::GE: code.jge(host_pass, jtype); break;\n'
     '        case IR::Cond::LT: code.jl(host_pass, jtype); break;\n'
     '        case IR::Cond::GT: code.jg(host_pass, jtype); break;\n'
     '        case IR::Cond::LE: code.jle(host_pass, jtype); break;\n'
     '        default: done = false; break;\n'
     '        }\n'
     '        if (done) {\n'
     '            if (check) {\n'
     '                // (the PC\'s check: the stored flags must say the same; ud2 where they don\'t)\n'
     '                Xbyak::Label stored_pass = EmitCond(terminal.if_);  // (the CPU\'s said no)\n'
     '                Xbyak::Label agree_fail, then_part;\n'
     '                code.jmp(agree_fail, code.T_NEAR);\n'
     '                code.L(stored_pass);\n'
     '                code.ud2();\n'
     '                code.L(host_pass);\n'
     '                Xbyak::Label agree_pass = EmitCond(terminal.if_);  // (the CPU\'s said yes)\n'
     '                code.ud2();\n'
     '                code.L(agree_pass);\n'
     '                code.jmp(then_part, code.T_NEAR);\n'
     '                code.L(agree_fail);\n'
     '                EmitTerminal(terminal.else_, initial_location, is_single_step);\n'
     '                code.L(then_part);\n'
     '                EmitTerminal(terminal.then_, initial_location, is_single_step);\n'
     '                return;\n'
     '            }\n'
     '            EmitTerminal(terminal.else_, initial_location, is_single_step);\n'
     '            code.L(host_pass);\n'
     '            EmitTerminal(terminal.then_, initial_location, is_single_step);\n'
     '            return;\n'
     '        }\n'
     '    }\n'
     '#endif\n'
     '    Xbyak::Label pass = EmitCond(terminal.if_);\n'),
], repo='external/dynarmic')
# (01.34) The "framequeue" switch, and what queue the game asks for in the log. A game that keeps one image
# waiting to be shown (sceGxmInitialize's displayQueueMaxPendingCount of 1): upstream its hand-over of an image
# waits until that very image is shown, so the game's work, the renderer's and the wait for the screen's refresh
# go one after the other. With the switch it waits, in the queue's push, only for the image before to be shown,
# as the console does; the sync objects still keep it from drawing on the buffer that is on the screen.
# (After 01.74) The "queuelimits" switch (31, ON): the queue is as long as the game asks for. Upstream, since its
# commit ecee684 (September), an image stays in the queue while the game's own "image shown" callback runs, but the
# queue's length was still the one of before - the game's number less one, 2 at most: Street Fighter X Tekken asks
# for 3 and had 2, Uncharted and MotorStorm RC ask for 2 and had 1. A game then waits for the screen more often
# than on a Vita; Vita3K's issue 4145 tells of games at 24 or 48 pictures a second for 30 or 60, and its pull
# request 4354 (merged in November, after the commit this tree is at) is the fix this follows: the game's own
# number (16 at most), and the hand-over waits for room in the queue before it marks the two buffers as waiting to
# be shown. A game that asks for 1 is as before (the "framequeue" switch is about that one).
patch('vita3k/threads/include/threads/queue.h', [
    ('    size_t size() {\n        return queue_.size();\n    }\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, the "queuelimits" switch) Waits until there is room for one more: false when the queue was aborted.\n'
     '    bool emusuni_wait_space() {\n'
     '        std::unique_lock<std::mutex> mlock(mutex_);\n'
     '        cond_.wait(mlock, [&]() { return aborted || queue_.size() < maxPendingCount_; });\n'
     '        return !aborted;\n'
     '    }\n'
     '#endif\n\n'
     '    size_t size() {\n        return queue_.size();\n    }\n'),
])
patch('vita3k/modules/SceGxm/SceGxm.cpp', [
    # (19-11, the switch "gxmfast", port/ps4_vita_gxmfast.cpp) The simplest getters, answered inside the
    # recompiler's call for them (kernel.cpp's emusuni_inline_svc) on these very structures: each the export's line.
    ('EXPORT(int, sceGxmProgramParameterGetType, const SceGxmProgramParameter *parameter) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '// (EmuSuNi, the switch "gxmfast") what: 1 Type, 2 ComponentCount, 3 ArraySize, 4 ResourceIndex, 5 Category,\n'
     '// 6 ContainerIndex, 7 SemanticIndex, 8 Semantic (of a program parameter), 9 and 10 a precomputed vertex and\n'
     '// fragment state\'s default uniform buffer, 11 a program\'s parameter count. 0: not here (a null address): the export.\n'
     'extern "C" int emusuni_gxm_get(int what, const unsigned char *base, unsigned address, unsigned *result) {\n'
     '    if (address == 0)\n'
     '        return 0;\n'
     '    if (what >= 1 && what <= 8) {\n'
     '        const SceGxmProgramParameter *parameter = reinterpret_cast<const SceGxmProgramParameter *>(base + address);\n'
     '        switch (what) {\n'
     '        case 1: *result = static_cast<unsigned>(parameter->type); return 1;\n'
     '        case 2: *result = parameter->component_count; return 1;\n'
     '        case 3: *result = parameter->array_size; return 1;\n'
     '        case 4: *result = parameter->resource_index; return 1;\n'
     '        case 5: *result = static_cast<unsigned>(parameter->category); return 1;\n'
     '        case 6: *result = parameter->container_index; return 1;\n'
     '        case 7: *result = parameter->semantic_index & 0xf; return 1;\n'
     '        default:\n'
     '            *result = static_cast<unsigned>(parameter->category != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE ? SCE_GXM_PARAMETER_SEMANTIC_NONE : parameter->semantic);\n'
     '            return 1;\n'
     '        }\n'
     '    }\n'
     '    if (what == 9 || what == 10) {\n'
     '        const Address buffers = what == 9\n'
     '            ? reinterpret_cast<const SceGxmPrecomputedVertexState *>(base + address)->uniform_buffers.address()\n'
     '            : reinterpret_cast<const SceGxmPrecomputedFragmentState *>(base + address)->uniform_buffers.address();\n'
     '        if (buffers == 0)\n'
     '            return 0;\n'
     '        const UniformBuffer *uniform_buffers = reinterpret_cast<const UniformBuffer *>(base + buffers);\n'
     '        const Ptr<const void> buffer = uniform_buffers[SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX];\n'
     '        *result = buffer.address();\n'
     '        return 1;\n'
     '    }\n'
     '    if (what == 11) {\n'
     '        *result = reinterpret_cast<const SceGxmProgram *>(base + address)->parameter_count;\n'
     '        return 1;\n'
     '    }\n'
     '    return 0;\n'
     '}\n'
     '#endif\n\n'
     'EXPORT(int, sceGxmProgramParameterGetType, const SceGxmProgramParameter *parameter) {\n'),
    ('TRACY_MODULE_NAME(SceGxm);\n', 'TRACY_MODULE_NAME(SceGxm);\n' + SWITCH_API),
    ('    if (emuenv.gxm.params.displayQueueMaxPendingCount == 1)\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (emuenv.gxm.params.displayQueueMaxPendingCount == 1 && !emusuni_vita_switch(2))\n'
     '#else\n'
     '    if (emuenv.gxm.params.displayQueueMaxPendingCount == 1)\n'
     '#endif\n'),
    # (the "queuelimits" switch: room in the queue first)
    ('    const Address address = alloc(emuenv.mem, emuenv.gxm.params.displayQueueCallbackDataSize, __FUNCTION__);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (the "queuelimits" switch) Room in the queue first, then the two buffers marked as waiting to be shown:\n'
     '    // the order the console\'s own library keeps. One hand-over at a time.\n'
     '    static std::mutex emusuni_add_mutex;\n'
     '    std::unique_lock<std::mutex> emusuni_add_guard(emusuni_add_mutex, std::defer_lock);\n'
     '    if (emusuni_vita_switch(31)) {\n'
     '        emusuni_add_guard.lock();\n'
     '        if (!emuenv.gxm.display_queue.emusuni_wait_space())\n'
     '            return 0;\n'
     '    }\n'
     '#endif\n'
     '    const Address address = alloc(emuenv.mem, emuenv.gxm.params.displayQueueCallbackDataSize, __FUNCTION__);\n'),
    ('    const uint32_t max_queue_size = std::max(std::min(params->displayQueueMaxPendingCount, 3U) - 1, 1U);\n'
     '    emuenv.gxm.display_queue.maxPendingCount_ = max_queue_size;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (the "queuelimits" switch) As many as the game asks for, 16 at most: an image is in the queue until its\n'
     '    // callback has run. OFF, upstream\'s number of before: one less, 2 at most.\n'
     '    const uint32_t max_queue_size = emusuni_vita_switch(31)\n'
     '        ? std::max(std::min(params->displayQueueMaxPendingCount, 16U), 1U)\n'
     '        : std::max(std::min(params->displayQueueMaxPendingCount, 3U) - 1, 1U);\n'
     '#else\n'
     '    const uint32_t max_queue_size = std::max(std::min(params->displayQueueMaxPendingCount, 3U) - 1, 1U);\n'
     '#endif\n'
     '    emuenv.gxm.display_queue.maxPendingCount_ = max_queue_size;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (params->displayQueueMaxPendingCount == 1)\n'
     '        emusuni_vita_log("vita: display: the game keeps a queue of 1 image to show; handing one over, it waits %s",\n'
     '            emusuni_vita_switch(2) ? "for the image before only (the frame queue switch is ON)"\n'
     '                                   : "until that very image is shown (the frame queue switch is OFF)");\n'
     '    else\n'
     '        emusuni_vita_log("vita: display: the game keeps a queue of %u images to show (%u here: the queuelimits switch is %s); the frame queue switch makes no difference to it",\n'
     '            static_cast<unsigned>(params->displayQueueMaxPendingCount), static_cast<unsigned>(max_queue_size), emusuni_vita_switch(31) ? "ON" : "OFF");\n'
     '#endif\n'),
    # (01.40) The "texsync" switch, a test: upstream looks at a texture again (its bytes hashed, and sent to the GPU
    # again when they changed) at the first draw after the game bound it or started a scene. A game that writes
    # into a texture it keeps bound - a text's glyphs, drawn as they are needed - then draws with the old bytes.
    # With the switch every texture a draw samples is looked at again at that draw. Slow (a hash a draw).
    ('    // set textures that are dirty\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(8)) {\n'
     '        context->is_vert_texture_dirty |= gxm_vertex_program.renderer_data->textures_used;\n'
     '        context->is_frag_texture_dirty |= gxm_fragment_program.renderer_data->textures_used;\n'
     '    }\n'
     '#endif\n'
     '    // set textures that are dirty\n'),
    # (01.65) sceGxmUnmapMemory waits for the renderer. Upstream it does only with "memory mapping" on (its
    # MemoryUnmap command is sent with a wait); here that option is off, and the game went on to free the memory
    # while the renderer's thread still had, in its queue, a draw sampling a texture in it: the texture's bytes
    # were read when they had gone. Seen with Street Fighter X Tekken closing a film, once the recompiler's
    # optimizations made the game's thread quicker than the renderer's: the frame of the video, read by the
    # picture converter, in a 2 MiB piece given back - the core's process ended there.
    ('        renderer::send_single_command(*emuenv.renderer, nullptr, renderer::CommandOpcode::MemoryUnmap, true, aligned_base);\n',
     '        renderer::send_single_command(*emuenv.renderer, nullptr, renderer::CommandOpcode::MemoryUnmap, true, aligned_base);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    else if (ite->second.size > 0)\n'
     '        renderer::finish(*emuenv.renderer, nullptr);\n'
     '#endif\n'),
    # (After 01.68, the "drawcopy" switch: see renderer.cpp's patch) Around a draw of an immediate context's, what
    # the draw reads from the game's memory is copied as its commands are made.
    ('    const SceGxmFragmentProgram &gxm_fragment_program = *context->state.fragment_program.get(emuenv.mem);\n'
     '    const SceGxmVertexProgram &gxm_vertex_program = *context->state.vertex_program.get(emuenv.mem);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const renderer::EmusuniDrawCopy emusuni_draw_copy(emuenv.mem,\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping);\n'
     '#endif\n'
     '    const SceGxmFragmentProgram &gxm_fragment_program = *context->state.fragment_program.get(emuenv.mem);\n'
     '    const SceGxmVertexProgram &gxm_vertex_program = *context->state.vertex_program.get(emuenv.mem);\n'),
    ('    renderer::set_program(*emuenv.renderer, context->renderer.get(), fragment_program_gptr, true);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const renderer::EmusuniDrawCopy emusuni_draw_copy(emuenv.mem,\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping);\n'
     '#endif\n'
     '    renderer::set_program(*emuenv.renderer, context->renderer.get(), fragment_program_gptr, true);\n'),
    # (19-11, the switch "gxmring", port/ps4_vita_gxmring.cpp) An immediate context's commands: a ring of the emulator's
    # own first (on), the ways as ever counted (count, on); and the own ring's commands given back when freed.
    ('TRACY_MODULE_NAME(SceGxm);\n',
     'TRACY_MODULE_NAME(SceGxm);\n'
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" int emusuni_vita_gxmring_mode(void);\n'
     'extern "C" void *emusuni_vita_gxmring_alloc(const void *ctx, size_t size);\n'
     'extern "C" int emusuni_vita_gxmring_free(const void *command);\n'
     'extern "C" void emusuni_vita_gxmring_count(int how);\n'
     '#endif\n'),
    ('            if (command_allocator_size > 0 && command_next_free_pos <= command_last_free_pos.load(std::memory_order_acquire)) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '            if (emusuni_vita_gxmring_mode() == 2) {\n'
     '                if (void *const emusuni_at = emusuni_vita_gxmring_alloc(this, sizeof(renderer::Command)))\n'
     '                    return new (emusuni_at) renderer::Command;\n'
     '            }\n'
     '#endif\n'
     '            if (command_allocator_size > 0 && command_next_free_pos <= command_last_free_pos.load(std::memory_order_acquire)) {\n'),
    ('                new (new_command) renderer::Command;\n'
     '            } else {\n'
     '                new_command = new renderer::Command;\n'
     '                new_command->flags |= renderer::Command::FLAG_FROM_HOST;\n',
     '                new (new_command) renderer::Command;\n'
     '#ifdef EMUSUNI_PORT\n'
     '                if (emusuni_vita_gxmring_mode() != 0)\n'
     '                    emusuni_vita_gxmring_count(1);\n'
     '#endif\n'
     '            } else {\n'
     '                new_command = new renderer::Command;\n'
     '                new_command->flags |= renderer::Command::FLAG_FROM_HOST;\n'
     '#ifdef EMUSUNI_PORT\n'
     '                if (emusuni_vita_gxmring_mode() != 0)\n'
     '                    emusuni_vita_gxmring_count(2);\n'
     '#endif\n'),
    ('    void free_new_command(renderer::Command *cmd) {\n',
     '    void free_new_command(renderer::Command *cmd) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_gxmring_mode() == 2 && emusuni_vita_gxmring_free(cmd))\n'
     '            return;\n'
     '#endif\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) A draw of an immediate context written as one packet; the max index left to the renderer; frees kept back.
    ('extern "C" void emusuni_vita_gxmring_count(int how);\n',
     'extern "C" void emusuni_vita_gxmring_count(int how);\n'
     'extern "C" int emusuni_vita_gxmring_free_later(const void *command);\n'
     'extern "C" void emusuni_vita_gxmring_publish(void);\n'
     'extern "C" void emusuni_vita_packet_publish(void);\n'),
    ('struct SceGxmContext {\n'
     '    GxmContextState state;\n',
     '#ifdef EMUSUNI_PORT\n'
     'struct SceGxmContext;\n'
     'static void emusuni_frees_give();\n'
     'static void emusuni_frees_later(SceGxmContext *context);\n'
     '#endif\n'
     'struct SceGxmContext {\n'
     '    GxmContextState state;\n'),
    ('    void free_new_command(renderer::Command *cmd) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_gxmring_mode() == 2 && emusuni_vita_gxmring_free(cmd))\n'
     '            return;\n'
     '#endif\n'
     '        if (!(cmd->flags & renderer::Command::FLAG_NO_FREE)) {\n'
     '            if (cmd->flags & renderer::Command::FLAG_FROM_HOST) {\n'
     '                delete cmd;\n'
     '            } else {\n'
     '                command_last_free_pos.fetch_add(1, std::memory_order_release);\n'
     '            }\n'
     '        }\n'
     '    }\n',
     '    void free_new_command(renderer::Command *cmd) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        // (EmuSuNi, "gxmpacket") null: the end of a list - the frees this thread kept back (the game\'s ring, the own\n'
     "        // ring, the packets' bytes) given now\n"
     '        if (!cmd) {\n'
     '            emusuni_frees_give();\n'
     '            emusuni_vita_gxmring_publish();\n'
     '            emusuni_vita_packet_publish();\n'
     '            return;\n'
     '        }\n'
     '        const bool emusuni_later = emusuni_vita_switch(50) != 0;\n'
     '        if (emusuni_vita_gxmring_mode() == 2 && (emusuni_later ? emusuni_vita_gxmring_free_later(cmd) : emusuni_vita_gxmring_free(cmd)))\n'
     '            return;\n'
     '#endif\n'
     '        if (!(cmd->flags & renderer::Command::FLAG_NO_FREE)) {\n'
     '            if (cmd->flags & renderer::Command::FLAG_FROM_HOST) {\n'
     '                delete cmd;\n'
     '            } else {\n'
     '#ifdef EMUSUNI_PORT\n'
     '                if (emusuni_later) {\n'
     '                    emusuni_frees_later(this);\n'
     '                    return;\n'
     '                }\n'
     '#endif\n'
     '                command_last_free_pos.fetch_add(1, std::memory_order_release);\n'
     '            }\n'
     '        }\n'
     '    }\n'),
    ('static_assert(sizeof(SceGxmContext) + 4 <= 2048);\n',
     'static_assert(sizeof(SceGxmContext) + 4 <= 2048);\n'
     '\n'
     '#ifdef EMUSUNI_PORT\n'
     '// (EmuSuNi, the switch "gxmpacket", 50) The game\'s command ring\'s frees kept by this thread (the renderer\'s; a\n'
     "// context's own at its end) and given back every 64 commands and at the end of each list, not with one atomic add a\n"
     "// command on the line the game's thread reads at every command it makes.\n"
     'static thread_local SceGxmContext *emusuni_kept_context = nullptr;\n'
     'static thread_local size_t emusuni_kept_frees = 0;\n'
     'static void emusuni_frees_give() {\n'
     '    if (emusuni_kept_context && emusuni_kept_frees)\n'
     '        emusuni_kept_context->command_last_free_pos.fetch_add(emusuni_kept_frees, std::memory_order_release);\n'
     '    emusuni_kept_frees = 0;\n'
     '}\n'
     'static void emusuni_frees_later(SceGxmContext *context) {\n'
     '    if (emusuni_kept_context != context) {\n'
     '        emusuni_frees_give();\n'
     '        emusuni_kept_context = context;\n'
     '    }\n'
     '    if (++emusuni_kept_frees >= 64)\n'
     '        emusuni_frees_give();\n'
     '}\n'
     '#endif\n'),
    ('        context->free_new_command(cmd);\n'
     '        cmd = next;\n'
     '    }\n'
     '\n'
     '    renderer::reset_command_list(context->renderer->command_list);\n',
     '        context->free_new_command(cmd);\n'
     '        cmd = next;\n'
     '    }\n'
     '#ifdef EMUSUNI_PORT\n'
     '    context->free_new_command(nullptr); // ("gxmpacket": what this thread kept back, given now)\n'
     '#endif\n'
     '\n'
     '    renderer::reset_command_list(context->renderer->command_list);\n'),
    ('    ctx->renderer->free_func = [ctx](renderer::Command *cmd) {\n'
     '        return ctx->free_new_command(cmd);\n'
     '    };\n',
     '    ctx->renderer->free_func = [ctx](renderer::Command *cmd) {\n'
     '        return ctx->free_new_command(cmd);\n'
     '    };\n'
     '#ifdef EMUSUNI_PORT\n'
     '    ctx->renderer->emusuni_flush_frees = true;\n'
     '#endif\n'),
    ('    const renderer::EmusuniDrawCopy emusuni_draw_copy(emuenv.mem,\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping);\n'
     '#endif\n'
     '    const SceGxmFragmentProgram &gxm_fragment_program = *context->state.fragment_program.get(emuenv.mem);\n',
     '    const renderer::EmusuniDrawCopy emusuni_draw_copy(emuenv.mem,\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping);\n'
     '    // ("gxmpacket") the commands this draw makes, as one packet\n'
     '    renderer::EmusuniPacket emusuni_packet(context->renderer.get(),\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping\n'
     '            && emuenv.renderer->current_backend == renderer::Backend::Vulkan,\n'
     '        context->state.vertex_uniform_buffers.size() + context->state.fragment_uniform_buffers.size());\n'
     '#endif\n'
     '    const SceGxmFragmentProgram &gxm_fragment_program = *context->state.fragment_program.get(emuenv.mem);\n'),
    ('    size_t max_index = 0;\n'
     '    if (!emuenv.renderer->features.enable_memory_mapping) {\n'
     "        // we don't need to get the vertex buffer size with memory mapping\n"
     '        if (indexType == SCE_GXM_INDEX_FORMAT_U16) {\n',
     '    size_t max_index = 0;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // ("gxmpacket") in a packet the renderer finds it, from the indices it copies anyway\n'
     '    if (!emuenv.renderer->features.enable_memory_mapping && !emusuni_packet.open()) {\n'
     '#else\n'
     '    if (!emuenv.renderer->features.enable_memory_mapping) {\n'
     '#endif\n'
     "        // we don't need to get the vertex buffer size with memory mapping\n"
     '        if (indexType == SCE_GXM_INDEX_FORMAT_U16) {\n'),
    ('    const renderer::EmusuniDrawCopy emusuni_draw_copy(emuenv.mem,\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping);\n'
     '#endif\n'
     '    renderer::set_program(*emuenv.renderer, context->renderer.get(), fragment_program_gptr, true);\n',
     '    const renderer::EmusuniDrawCopy emusuni_draw_copy(emuenv.mem,\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping);\n'
     '    // ("gxmpacket") the commands this draw makes, as one packet\n'
     '    renderer::EmusuniPacket emusuni_packet(context->renderer.get(),\n'
     '        context->state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && !emuenv.renderer->features.enable_memory_mapping\n'
     '            && emuenv.renderer->current_backend == renderer::Backend::Vulkan,\n'
     '        static_cast<size_t>(vertex_state ? vertex_state->buffer_count : context->state.vertex_uniform_buffers.size())\n'
     '            + static_cast<size_t>(fragment_state ? fragment_state->buffer_count : context->state.fragment_uniform_buffers.size()));\n'
     '#endif\n'
     '    renderer::set_program(*emuenv.renderer, context->renderer.get(), fragment_program_gptr, true);\n'),
    ('    uint32_t max_index = 0;\n'
     '    if (!emuenv.renderer->features.enable_memory_mapping) {\n'
     "        // we don't need to get the vertex buffer size with memory mapping\n"
     '        if (draw->index_format == SCE_GXM_INDEX_FORMAT_U16) {\n',
     '    uint32_t max_index = 0;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // ("gxmpacket") in a packet the renderer finds it, from the indices it copies anyway\n'
     '    if (!emuenv.renderer->features.enable_memory_mapping && !emusuni_packet.open()) {\n'
     '#else\n'
     '    if (!emuenv.renderer->features.enable_memory_mapping) {\n'
     '#endif\n'
     "        // we don't need to get the vertex buffer size with memory mapping\n"
     '        if (draw->index_format == SCE_GXM_INDEX_FORMAT_U16) {\n'),
])
# (01.40) What the log says every 10 s about the pictures and the pad (port/ps4_vita_stats.cpp, "vita: gfx:"): the
# transfers the game asks for, which go through its memory - where the GPU's picture of a painted surface is not,
# here, and from where a fill doesn't reach the GPU's; the draws that read the colour already painted; the
# refreshes with a button of player 1's down and with a finger on the touch screen.
patch('vita3k/renderer/src/transfer.cpp', [
    ('namespace renderer {\n', STAT_API + 'namespace renderer {\n'),
    ('COMMAND(handle_transfer_copy) {\n',
     'COMMAND(handle_transfer_copy) {\n#ifdef EMUSUNI_PORT\n    emusuni_vita_stat_add(7, 0);\n#endif\n'),
    ('COMMAND(handle_transfer_downscale) {\n',
     'COMMAND(handle_transfer_downscale) {\n#ifdef EMUSUNI_PORT\n    emusuni_vita_stat_add(8, 0);\n#endif\n'),
    ('COMMAND(handle_transfer_fill) {\n',
     'COMMAND(handle_transfer_fill) {\n#ifdef EMUSUNI_PORT\n    emusuni_vita_stat_add(9, 0);\n#endif\n'),
])
# (01.40) The "gpubarrier" switch, a test for the PS4's GPU: 01.40's PC bench runs Street Fighter X Tekken into
# a fight with the PS4's renderer options and its windows come out right, where on the console they leave a
# trail - so what differs is the GPU's driver. With the switch every scene's first commands (the ones that run
# before its draws) start with a barrier for everything written so far, which has the PS4's driver empty its
# colour and depth caches before anything of the scene reads them.
patch('vita3k/renderer/src/vulkan/context.cpp', [
    ('namespace renderer::vulkan {\n', SWITCH_API + 'namespace renderer::vulkan {\n'),
    ('    prerender_cmd.begin(begin_info);\n',
     '    prerender_cmd.begin(begin_info);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(11)) {\n'
     '        const vk::MemoryBarrier emusuni_all{\n'
     '            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite\n'
     '                | vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eShaderWrite,\n'
     '            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentRead\n'
     '                | vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead\n'
     '        };\n'
     '        prerender_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,\n'
     '            {}, emusuni_all, {}, {});\n'
     '    }\n'
     '#endif\n'),
    # (20-11) a PC test: the scene commands' time by command and by part (EMUSUNI_VITA_TEST_SCENES)
    ('namespace renderer::vulkan {\n\nvoid VKContext::wait_thread_function(const MemState &mem) {\n',
     '#if !defined(__PS4__)\n// (20-11, a PC test: EMUSUNI_VITA_TEST_SCENES=1) the scene commands\' time by command (0 SetContext, 1\n// SyncSurfaceData, 2 NewFrame, 3 MidSceneFlush: batch.cpp) and by part (4-17 below), said every 10 s\n#include <cstdio>\n#include <cstdlib>\n#include <chrono>\nextern "C" unsigned long long emusuni_vita_stat_now(void);\nstatic bool emusuni_test_scenes_on() {\n    static int on = -1;\n    if (on < 0) {\n        const char *e = getenv("EMUSUNI_VITA_TEST_SCENES");\n        on = e && *e == \'1\';\n    }\n    return on != 0;\n}\nstatic unsigned long long emusuni_ts_count[24], emusuni_ts_ticks[24], emusuni_ts_from;\nextern "C" void emusuni_vita_test_scene(int what, unsigned long long ticks) {\n    if (!emusuni_test_scenes_on() || what < 0 || what >= 24)\n        return;\n    emusuni_ts_count[what]++;\n    emusuni_ts_ticks[what] += ticks;\n}\nstatic void emusuni_test_part(int what) {\n    if (!emusuni_test_scenes_on())\n        return;\n    const unsigned long long now = emusuni_vita_stat_now();\n    if (what >= 0)\n        emusuni_vita_test_scene(what, now - emusuni_ts_from);\n    emusuni_ts_from = now;\n}\nstatic void emusuni_test_scene_report(renderer::vulkan::VKContext &context) {\n    if (!emusuni_test_scenes_on())\n        return;\n    static auto last = std::chrono::steady_clock::now();\n    static unsigned long long last_tsc = emusuni_vita_stat_now();\n    const auto now = std::chrono::steady_clock::now();\n    const double us = std::chrono::duration<double, std::micro>(now - last).count();\n    if (us < 10e6)\n        return;\n    const unsigned long long tsc = emusuni_vita_stat_now();\n    const double per_us = static_cast<double>(tsc - last_tsc) / us;\n    last = now;\n    last_tsc = tsc;\n    const auto each = [&](int k) { return emusuni_ts_count[k] ? static_cast<double>(emusuni_ts_ticks[k]) / per_us / static_cast<double>(emusuni_ts_count[k]) : 0.0; };\n    long pages = 0, resident = 0;\n    if (FILE *f = fopen("/proc/self/statm", "r")) {\n        if (fscanf(f, "%ld %ld", &pages, &resident) != 2)\n            resident = 0;\n        fclose(f);\n    }\n    size_t fences = 0, cmds = 0;\n    if (context.render_target) {\n        fences = context.render_target->fences.size();\n        for (const auto &v : context.render_target->cmd_buffers)\n            cmds += v.size();\n    }\n    fprintf(stderr, "[emusuni-vita] test: scenes: setcontext %llu x %.1f us (start %.1f, recording %.1f, mask %.1f, pass %.1f, framebuffer %.1f, the rest %.1f); "\n        "sync %llu x %.1f us (pass end %.1f, visibility %.1f, the rest %.1f, submit %.1f, after %.1f); newframe %llu x %.1f us (start %.1f, fence wait %.1f, "\n        "fence reset %.1f, pools %.1f, descriptors and destroy queue %.1f); midflush %llu x %.1f us; descriptor pools %zu; this target\'s fences %zu, "\n        "command buffers %zu; resident %ld MiB\\n",\n        emusuni_ts_count[0], each(0), each(4), each(5), each(6), each(7), each(8), each(9),\n        emusuni_ts_count[1], each(1), each(10), each(11), each(12), each(13), each(14),\n        emusuni_ts_count[2], each(2), each(15), each(16), each(17), each(18), each(19),\n        emusuni_ts_count[3], each(3), context.state.frame_descriptor_pools.size(), fences, cmds, resident * 4 / 1024);\n    for (int k = 0; k < 24; k++)\n        emusuni_ts_count[k] = emusuni_ts_ticks[k] = 0;\n}\n#endif\nnamespace renderer::vulkan {\n\nvoid VKContext::wait_thread_function(const MemState &mem) {\n'),
    ('void set_context(VKContext &context, MemState &mem, VKRenderTarget *rt, const FeatureState &features) {\n',
     'void set_context(VKContext &context, MemState &mem, VKRenderTarget *rt, const FeatureState &features) {\n#if !defined(__PS4__)\n    emusuni_test_part(-1);\n#endif\n'),
    ('    VKState &state = context.state;\n    state.surface_cache.set_render_target(rt);\n\n    context.start_recording(true);\n',
     '    VKState &state = context.state;\n    state.surface_cache.set_render_target(rt);\n#if !defined(__PS4__)\n    emusuni_test_part(4);\n#endif\n\n    context.start_recording(true);\n#if !defined(__PS4__)\n    emusuni_test_part(5);\n#endif\n'),
    ('        rt->mask.layout = vkutil::ImageLayout::StorageImage;\n    }\n',
     '        rt->mask.layout = vkutil::ImageLayout::StorageImage;\n    }\n#if !defined(__PS4__)\n    emusuni_test_part(6);\n#endif\n'),
    ('    Framebuffer &framebuffer = state.surface_cache.retrieve_framebuffer_handle(',
     '#if !defined(__PS4__)\n    emusuni_test_part(7);\n#endif\n    Framebuffer &framebuffer = state.surface_cache.retrieve_framebuffer_handle('),
    ('    context.current_color_base_image = framebuffer.base_image;\n',
     '    context.current_color_base_image = framebuffer.base_image;\n#if !defined(__PS4__)\n    emusuni_test_part(8);\n#endif\n'),
    ('    context.ignore_macroblock = false;\n}\n\nvoid VKContext::start_recording(bool first_in_scene) {\n',
     '    context.ignore_macroblock = false;\n#if !defined(__PS4__)\n    emusuni_test_part(9);\n#endif\n}\n\nvoid VKContext::start_recording(bool first_in_scene) {\n'),
    ('    if (in_renderpass)\n        stop_render_pass();\n\n    resolve_visibility_queries();\n',
     '#if !defined(__PS4__)\n    emusuni_test_part(-1);\n#endif\n    if (in_renderpass)\n        stop_render_pass();\n#if !defined(__PS4__)\n    emusuni_test_part(10);\n#endif\n\n    resolve_visibility_queries();\n#if !defined(__PS4__)\n    emusuni_test_part(11);\n#endif\n'),
    ('    state.locked_queue_submit(state.general_queue, submit_info, fence);\n',
     '#if !defined(__PS4__)\n    emusuni_test_part(12);\n#endif\n    state.locked_queue_submit(state.general_queue, submit_info, fence);\n#if !defined(__PS4__)\n    emusuni_test_part(13);\n#endif\n'),
    ('    cmdbuffers_to_submit.clear();\n    state.frame().rendered_fences.push_back(fence);\n',
     '    cmdbuffers_to_submit.clear();\n    state.frame().rendered_fences.push_back(fence);\n#if !defined(__PS4__)\n    emusuni_test_part(14);\n#endif\n'),
    ('void new_frame(VKContext &context) {\n',
     'void new_frame(VKContext &context) {\n#if !defined(__PS4__)\n    emusuni_test_part(-1);\n#endif\n'),
    ('    // wait on all fences still present to make sure\n',
     '#if !defined(__PS4__)\n    emusuni_test_part(15);\n#endif\n    // wait on all fences still present to make sure\n'),
    ('        // reset the fences in both case (the wait thread does not do that as they can still be used)\n',
     '#if !defined(__PS4__)\n        emusuni_test_part(16);\n#endif\n        // reset the fences in both case (the wait thread does not do that as they can still be used)\n'),
    ('    device.resetCommandPool(frame.prerender_pool);\n',
     '#if !defined(__PS4__)\n    emusuni_test_part(17);\n#endif\n    device.resetCommandPool(frame.prerender_pool);\n'),
    ('    // set the position in the used descriptor queue back to the beginning\n',
     '#if !defined(__PS4__)\n    emusuni_test_part(18);\n#endif\n    // set the position in the used descriptor queue back to the beginning\n'),
    ('    frame.frame_timestamp = context.frame_timestamp;\n}\n',
     '    frame.frame_timestamp = context.frame_timestamp;\n#if !defined(__PS4__)\n    emusuni_test_part(19);\n#endif\n#if !defined(__PS4__)\n    emusuni_test_scene_report(context);\n#endif\n}\n'),
])
patch('vita3k/renderer/src/vulkan/scene.cpp', [
    ('namespace renderer::vulkan {\n',
     STAT_API + SWITCH_API + '#ifdef EMUSUNI_PORT\n#include <cstdio>\n#include <cstdlib>\n#include <ctime>\n#include <util/align.h>\n'
     'extern "C" void emusuni_vita_stat_uniforms(const unsigned char *program, int vertex, int block, const void *data, unsigned size);\n'
     '#endif\n'
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     'extern unsigned emusuni_test_textures[SCE_GXM_MAX_TEXTURE_UNITS][4];\n'
     '#endif\n'
     'namespace renderer::vulkan {\n' + RELAY_FN +
     '#ifdef EMUSUNI_PORT\n'
     '// (EmuSuNi, the "vertexrelay" switch) The stream copied with each attribute where emusuni_relay() puts it; what is\n'
     '// between them is zero. (As restride_stream below: the caller deletes the copy.)\n'
     'static void emusuni_relay_stream(const uint8_t *&stream, uint32_t &size, uint32_t stride, const SceGxmVertexProgram &vertex_program,\n'
     '    unsigned stream_index, const EmusuniRelay &relay) {\n'
     '    const uint32_t vertices = stride != 0 ? (size + stride - 1) / stride : 0;\n'
     '    uint8_t *const data = new uint8_t[static_cast<size_t>(vertices) * relay.stride + 4]();\n'
     '    const size_t count = vertex_program.attributes.size() < 32 ? vertex_program.attributes.size() : 32;\n'
     '    for (size_t a = 0; a < count; a++) {\n'
     '        if (vertex_program.attributes[a].streamIndex != stream_index || relay.sizes[a] == 0)\n'
     '            continue;\n'
     '        const uint32_t from = vertex_program.attributes[a].offset;\n'
     '        for (uint32_t v = 0; v < vertices; v++) {\n'
     '            const uint64_t at = static_cast<uint64_t>(v) * stride + from;\n'
     '            if (at >= size)\n'
     '                break; // (past what the game gave)\n'
     '            const uint64_t bytes = size - at < relay.sizes[a] ? size - at : relay.sizes[a];\n'
     '            memcpy(data + static_cast<size_t>(v) * relay.stride + relay.offsets[a], stream + at, static_cast<size_t>(bytes));\n'
     '        }\n'
     '    }\n'
     '    stream = data;\n'
     '    size = vertices * relay.stride;\n'
     '}\n'
     '#endif\n'),
    # (After 01.68) The "vertexalign" switch (23, ON): a vertex stream whose vertices aren't a whole number of 4
    # bytes apart is copied with each vertex padded to the next multiple of 4, and the pipeline told so. The PS4's
    # GPU reads numbers of 2 and 4 bytes wrong when the vertices that hold them are an odd number of bytes apart
    # (what HENkaku-PS4's notes on its own port say - read on 14-11, never tried here: this switch is the first
    # try). Street Fighter X Tekken's menus - Scaleform's - draw their text and some of
    # their shapes with vertices 17 and 21 bytes apart that hold 4-byte numbers (two floats for the place, two for
    # the texture, a byte for the batch): on the console, shapes pulled across the screen and no text, since its
    # first versions; on the PC bench, whose GPU reads any stride, never. The padding is upstream's own, written
    # for Apple's Metal, which only takes strides that are multiples of 4: here it runs behind the switch.
    ('#ifdef __APPLE__\n// restride vertex attribute binding strides to multiple of 4\n',
     '#if defined(__APPLE__) || defined(EMUSUNI_PORT)\n// restride vertex attribute binding strides to multiple of 4\n'),
    ('#ifdef __APPLE__\n'
     '                // Vulkan allows any stride, but Metal only allows multiples of 4.\n'
     '                const bool restride = vertex_program.streams[i].stride % 4 != 0;\n',
     '#if defined(__APPLE__) || defined(EMUSUNI_PORT)\n'
     '                // Vulkan allows any stride, but Metal only allows multiples of 4.\n'
     '#ifdef EMUSUNI_PORT\n'
     '                const EmusuniRelay emusuni_layout = emusuni_relay(vertex_program, static_cast<unsigned>(i));\n'
     '                const bool restride = emusuni_layout.relaid || (vertex_program.streams[i].stride % 4 != 0 && emusuni_vita_switch(23));\n'
     '                if (restride)\n'
     '                    emusuni_vita_stat_add(emusuni_layout.relaid ? 27 : 23, 0);\n'
     '#else\n'
     '                const bool restride = vertex_program.streams[i].stride % 4 != 0;\n'
     '#endif\n'),
    ('                    restride_stream(stream, stream_size, vertex_program.streams[i].stride);\n',
     '#ifdef EMUSUNI_PORT\n'
     '                    if (emusuni_layout.relaid)\n'
     '                        emusuni_relay_stream(stream, stream_size, vertex_program.streams[i].stride, vertex_program, static_cast<unsigned>(i), emusuni_layout);\n'
     '                    else\n'
     '                        restride_stream(stream, stream_size, vertex_program.streams[i].stride);\n'
     '#else\n'
     '                    restride_stream(stream, stream_size, vertex_program.streams[i].stride);\n'
     '#endif\n'),
    ('#ifdef __APPLE__\n'
     '                if (restride) {\n'
     '                    delete[] stream;\n',
     '#if defined(__APPLE__) || defined(EMUSUNI_PORT)\n'
     '                if (restride) {\n'
     '                    delete[] stream;\n'),
    # (After 01.68, the "drawcopy" switch) A draw's uniform buffers, vertex streams and indices: from the copy the
    # game's thread kept at the draw, when there is one (see renderer.cpp's patch).
    ('            context.vertex_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, data.get(mem), offset_start_upload);\n',
     '#ifdef EMUSUNI_PORT\n'
     '            context.vertex_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload,\n'
     '                context.emusuni_uniform_host ? context.emusuni_uniform_host : data.get(mem), offset_start_upload);\n'
     '#else\n'
     '            context.vertex_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, data.get(mem), offset_start_upload);\n'
     '#endif\n'),
    ('            context.fragment_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, data.get(mem), offset_start_upload);\n',
     '#ifdef EMUSUNI_PORT\n'
     '            context.fragment_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload,\n'
     '                context.emusuni_uniform_host ? context.emusuni_uniform_host : data.get(mem), offset_start_upload);\n'
     '#else\n'
     '            context.fragment_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, data.get(mem), offset_start_upload);\n'
     '#endif\n'),
    # (After 01.70) The "uniformlog" switch (26, ON in the one set it comes in), a look at what a game gives its shaders: the numbers of each
    # uniform buffer a program is given, the first times they differ, in the log ("vita: uniforms:"). On the
    # console Street Fighter X Tekken's fighters have their skin black - lips, eyes, clothes and hair right - and
    # on the bench they are right. Their shaders take the skin's colour from numbers the game gives them (its
    # material colours, g_MaterialCol0_PS...), not from a texture: with the same lines from the console and from
    # the bench, either the game gives a black colour there (its data, its save, a load that took too long) or
    # the numbers are the same and it is the GPU that works the picture out wrong.
    ('    const uint32_t data_size_upload = std::min<uint32_t>(size, program->uniform_buffer_sizes.at(block_num) * 4);\n',
     '    const uint32_t data_size_upload = std::min<uint32_t>(size, program->uniform_buffer_sizes.at(block_num) * 4);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(26))\n'
     '        emusuni_vita_stat_uniforms(program->hash.data(), vertex_shader ? 1 : 0, block_num,\n'
     '            context.emusuni_uniform_host ? context.emusuni_uniform_host : data.get(mem), data_size_upload);\n'
     '#endif\n'),
    ('                const uint8_t *stream = state.vertex_streams[i].data.get(mem);\n',
     '#ifdef EMUSUNI_PORT\n'
     '                const uint8_t *stream = state.vertex_streams[i].host ? static_cast<const uint8_t *>(state.vertex_streams[i].host)\n'
     '                                                                     : state.vertex_streams[i].data.get(mem);\n'
     '#else\n'
     '                const uint8_t *stream = state.vertex_streams[i].data.get(mem);\n'
     '#endif\n'),
    ('            state.vertex_streams[i].data = nullptr;\n            state.vertex_streams[i].size = 0;\n',
     '            state.vertex_streams[i].data = nullptr;\n            state.vertex_streams[i].size = 0;\n'
     '#ifdef EMUSUNI_PORT\n'
     '            free(state.vertex_streams[i].host);\n'
     '            state.vertex_streams[i].host = nullptr;\n'
     '#endif\n'),
    ('    void *indices_ptr = indices.get(mem);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    void *indices_ptr = context.emusuni_index_host ? const_cast<void *>(context.emusuni_index_host) : indices.get(mem);\n'
     '#else\n'
     '    void *indices_ptr = indices.get(mem);\n'
     '#endif\n'
     # (After 01.68) A test, on the PC only: EMUSUNI_VITA_TEST_DRAWS=<seconds> lists, that long after the first
     # draw, the next 400 draws as the renderer gets them - the programs, the vertex attributes' formats and
     # strides, the textures' formats, the surface -, to see what the draws of a screen that comes out wrong on
     # the console have that the others don't.
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '    {\n'
     '        static const int emusuni_at = [] {\n'
     '            const char *const text = getenv("EMUSUNI_VITA_TEST_DRAWS");\n'
     '            return text ? atoi(text) : 0;\n'
     '        }();\n'
     '        static unsigned long long emusuni_first = 0;\n'
     '        static int emusuni_left = 400;\n'
     '        if (emusuni_at > 0 && emusuni_left > 0) {\n'
     '            timespec emusuni_now;\n'
     '            clock_gettime(CLOCK_MONOTONIC, &emusuni_now);\n'
     '            const unsigned long long emusuni_ms = emusuni_now.tv_sec * 1000ull + emusuni_now.tv_nsec / 1000000ull;\n'
     '            if (emusuni_first == 0)\n'
     '                emusuni_first = emusuni_ms;\n'
     '            if (emusuni_ms - emusuni_first >= static_cast<unsigned long long>(emusuni_at) * 1000ull) {\n'
     '                emusuni_left--;\n'
     '                const SceGxmVertexProgram &emusuni_vp = *context.record.vertex_program.get(mem);\n'
     '                const SceGxmProgram &emusuni_fgxp = *gxm_fragment_program.program.get(mem);\n'
     '                char emusuni_line[900];\n'
     '                int emusuni_n = snprintf(emusuni_line, sizeof(emusuni_line), "[emusuni-vita] test: draw: prim %#x, %zu indices of %d bytes, x%u; vp %02x%02x%02x%02x fp %02x%02x%02x%02x; surface format %#x %ux%u%s; attributes:",\n'
     '                    static_cast<unsigned>(type), count, format == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4, instance_count,\n'
     '                    context.record.vertex_program_hash[0], context.record.vertex_program_hash[1], context.record.vertex_program_hash[2], context.record.vertex_program_hash[3],\n'
     '                    context.record.fragment_program_hash[0], context.record.fragment_program_hash[1], context.record.fragment_program_hash[2], context.record.fragment_program_hash[3],\n'
     '                    static_cast<unsigned>(context.record.color_surface.colorFormat), static_cast<unsigned>(context.record.color_surface.width),\n'
     '                    static_cast<unsigned>(context.record.color_surface.height), gxm_fragment_program.is_maskupdate ? " (mask update)" : "");\n'
     '                for (const SceGxmVertexAttribute &emusuni_a : emusuni_vp.attributes) {\n'
     '                    if (emusuni_n < 0 || emusuni_n > 760)\n'
     '                        break;\n'
     '                    const unsigned emusuni_stride = emusuni_a.streamIndex < emusuni_vp.streams.size() ? emusuni_vp.streams[emusuni_a.streamIndex].stride : 0;\n'
     '                    emusuni_n += snprintf(emusuni_line + emusuni_n, sizeof(emusuni_line) - emusuni_n, " [stream %u+%u/%u fmt %u x%u reg %u]",\n'
     '                        emusuni_a.streamIndex, emusuni_a.offset, emusuni_stride, static_cast<unsigned>(emusuni_a.format), emusuni_a.componentCount, emusuni_a.regIndex);\n'
     '                }\n'
     '                if (emusuni_n > 0 && emusuni_n < 800)\n'
     '                    emusuni_n += snprintf(emusuni_line + emusuni_n, sizeof(emusuni_line) - emusuni_n, "; textures:");\n'
     '                // (the units the fragment program samples: format, type - 0x40000000 a cube map -, size, mips)\n'
     '                for (int emusuni_t = 0; emusuni_t < SCE_GXM_MAX_TEXTURE_UNITS && emusuni_n > 0 && emusuni_n < 830; emusuni_t++) {\n'
     '                    if (gxm_fragment_program.renderer_data->textures_used[emusuni_t])\n'
     '                        emusuni_n += snprintf(emusuni_line + emusuni_n, sizeof(emusuni_line) - emusuni_n, " %d:%#x type %#x %ux%u mips %u;", emusuni_t,\n'
     '                            static_cast<unsigned>(context.shader_hints.fragment_textures[emusuni_t]), emusuni_test_textures[emusuni_t][0],\n'
     '                            emusuni_test_textures[emusuni_t][1], emusuni_test_textures[emusuni_t][2], emusuni_test_textures[emusuni_t][3]);\n'
     '                }\n'
     '                (void)emusuni_fgxp;\n'
     '                fprintf(stderr, "%s\\n", emusuni_line);\n'
     '            }\n'
     '        }\n'
     '    }\n'
     # (After 01.70) And EMUSUNI_VITA_TEST_SKIP_FP / _SKIP_VP=<the program's first 8 hex digits, as the listing
     # and the shader cache's files have them>: the draws of that fragment or vertex program are not made - what
     # goes missing from the picture is what that program draws (which of a fighter's programs is its skin).
     '    {\n'
     '        static const unsigned long emusuni_skip_fp = [] {\n'
     '            const char *const text = getenv("EMUSUNI_VITA_TEST_SKIP_FP");\n'
     '            return text ? strtoul(text, nullptr, 16) : 0ul;\n'
     '        }();\n'
     '        static const unsigned long emusuni_skip_vp = [] {\n'
     '            const char *const text = getenv("EMUSUNI_VITA_TEST_SKIP_VP");\n'
     '            return text ? strtoul(text, nullptr, 16) : 0ul;\n'
     '        }();\n'
     '        const auto emusuni_first4 = [](const Sha256Hash &hash) {\n'
     '            return static_cast<unsigned long>(hash[0]) << 24 | static_cast<unsigned long>(hash[1]) << 16 | static_cast<unsigned long>(hash[2]) << 8 | hash[3];\n'
     '        };\n'
     '        if ((emusuni_skip_fp != 0 && emusuni_first4(context.record.fragment_program_hash) == emusuni_skip_fp)\n'
     '            || (emusuni_skip_vp != 0 && emusuni_first4(context.record.vertex_program_hash) == emusuni_skip_vp))\n'
     '            return;\n'
     '    }\n'
     '#endif\n'),
    ('    if ((context.state.features.direct_fragcolor || masked_fetch) && uses_framebuffer_fetch)\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (uses_framebuffer_fetch)\n'
     '        emusuni_vita_stat_add(11, 0);\n'
     '#endif\n'
     '    if ((context.state.features.direct_fragcolor || masked_fetch) && uses_framebuffer_fetch)\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) The draw's highest index and its streams' sizes found here for a packet's draw.
    ('    if (max_stream_idx == 0)\n'
     '        return;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, "gxmpacket") a packet\'s draw: the streams\' sizes as SceGxm.cpp made them in the game\'s thread, from\n'
     '    // the highest index draw() found\n'
     '    if (context.emusuni_late_max && !context.state.features.enable_memory_mapping) {\n'
     '        for (int i = 0; i < max_stream_idx; i++)\n'
     '            state.vertex_streams[i].size = 0;\n'
     '        for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {\n'
     '            const SceGxmAttributeFormat attribute_format = static_cast<SceGxmAttributeFormat>(attribute.format);\n'
     '            const size_t attribute_size = gxm::attribute_format_size(attribute_format) * attribute.componentCount;\n'
     '            const SceGxmVertexStream &stream = vertex_program.streams[attribute.streamIndex];\n'
     '            const SceGxmIndexSource index_source = static_cast<SceGxmIndexSource>(stream.indexSource);\n'
     '            const size_t data_passed_length = gxm::is_stream_instancing(index_source) ? ((instance_count - 1) * stream.stride) : (max_index * stream.stride);\n'
     '            const size_t data_length = attribute.offset + data_passed_length + attribute_size;\n'
     '            state.vertex_streams[attribute.streamIndex].size = std::max<size_t>(state.vertex_streams[attribute.streamIndex].size, data_length);\n'
     '        }\n'
     '    }\n'
     '#endif\n'
     '    if (max_stream_idx == 0)\n'
     '        return;\n'),
    ('    uint32_t max_index = 0;\n'
     '    if (use_memory_mapping) {\n',
     '    uint32_t max_index = 0;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, "gxmpacket") a packet\'s draw: its highest index found here, from the indices about to be copied\n'
     '    if (!use_memory_mapping && context.emusuni_late_max && count > 0) {\n'
     '        if (format == SCE_GXM_INDEX_FORMAT_U16) {\n'
     '            const uint16_t *const data = static_cast<const uint16_t *>(indices_ptr);\n'
     '            max_index = *std::max_element(data, data + count);\n'
     '        } else {\n'
     '            const uint32_t *const data = static_cast<const uint32_t *>(indices_ptr);\n'
     '            max_index = *std::max_element(data, data + count);\n'
     '        }\n'
     '    }\n'
     '#endif\n'
     '    if (use_memory_mapping) {\n'),
    # (20-11) the renderer's line, "vita: renderer:" (port/ps4_vita_stats.cpp)
    ('static void bind_vertex_streams(VKContext &context, MemState &mem, uint32_t instance_count, uint32_t max_index) {\n',
     "#ifdef EMUSUNI_PORT\n// (20-11) The draw's lowest index (draw() finds it; 0 when it did not): bind_vertex_streams counts the bytes of each\n// stream below that vertex, copied for nothing (the renderer's thread only)\nstatic uint32_t emusuni_draw_low = 0;\n#endif\nstatic void bind_vertex_streams(VKContext &context, MemState &mem, uint32_t instance_count, uint32_t max_index) {\n"),
    ('                uint32_t stream_size = state.vertex_streams[i].size;\n',
     '                uint32_t stream_size = state.vertex_streams[i].size;\n#ifdef EMUSUNI_PORT\n                {\n                    const SceGxmIndexSource emusuni_source = static_cast<SceGxmIndexSource>(vertex_program.streams[i].indexSource);\n                    const unsigned long long emusuni_below = gxm::is_stream_instancing(emusuni_source)\n                        ? 0ull\n                        : std::min<unsigned long long>(stream_size, static_cast<unsigned long long>(emusuni_draw_low) * vertex_program.streams[i].stride);\n                    emusuni_vita_stat_add(49, stream_size);\n                    if (emusuni_below != 0)\n                        emusuni_vita_stat_add(50, emusuni_below);\n                }\n#endif\n'),
    ('    context.check_for_macroblock_change(true);\n\n    const bool maskupdate = gxm_fragment_program.is_maskupdate;\n',
     '#ifdef EMUSUNI_PORT\n    const EmusuniVitaStatScope emusuni_stat_draw(37);\n#endif\n    context.check_for_macroblock_change(true);\n\n    const bool maskupdate = gxm_fragment_program.is_maskupdate;\n'),
    ('        vk::Pipeline new_pipeline = context.state.pipeline_cache.retrieve_pipeline(context, type, !can_be_whole_quad, mem);\n',
     '#ifdef EMUSUNI_PORT\n        const unsigned long long emusuni_pipe_from = emusuni_vita_stat_now();\n#endif\n        vk::Pipeline new_pipeline = context.state.pipeline_cache.retrieve_pipeline(context, type, !can_be_whole_quad, mem);\n#ifdef EMUSUNI_PORT\n        emusuni_vita_stat_add(38, emusuni_vita_stat_now() - emusuni_pipe_from);\n#endif\n'),
    ('    // create, update and bind descriptors (uniforms and textures)\n    draw_bind_descriptors(context, mem);\n',
     '    // create, update and bind descriptors (uniforms and textures)\n#ifdef EMUSUNI_PORT\n    const unsigned long long emusuni_bind_from = emusuni_vita_stat_now();\n#endif\n    draw_bind_descriptors(context, mem);\n#ifdef EMUSUNI_PORT\n    const unsigned long long emusuni_geom_from = emusuni_vita_stat_now();\n    emusuni_vita_stat_add(39, emusuni_geom_from - emusuni_bind_from);\n#endif\n'),
    ('        const size_t index_buffer_size = index_size * count;\n',
     '        const size_t index_buffer_size = index_size * count;\n#ifdef EMUSUNI_PORT\n        emusuni_vita_stat_add(51, index_buffer_size);\n#endif\n'),
    ('    // bind the vertex streams\n    bind_vertex_streams(context, mem, instance_count, max_index);\n',
     '#ifdef EMUSUNI_PORT\n    emusuni_draw_low = 0;\n    if (!use_memory_mapping && !ordered_indices && count > 0 && indices_ptr) {\n        if (format == SCE_GXM_INDEX_FORMAT_U16) {\n            const uint16_t *const emusuni_data = static_cast<const uint16_t *>(indices_ptr);\n            emusuni_draw_low = *std::min_element(emusuni_data, emusuni_data + count);\n        } else {\n            const uint32_t *const emusuni_data = static_cast<const uint32_t *>(indices_ptr);\n            emusuni_draw_low = *std::min_element(emusuni_data, emusuni_data + count);\n        }\n    }\n#endif\n    // bind the vertex streams\n    bind_vertex_streams(context, mem, instance_count, max_index);\n#ifdef EMUSUNI_PORT\n    emusuni_vita_stat_add(40, emusuni_vita_stat_now() - emusuni_geom_from);\n#endif\n'),
    # (20-11, for 01.84) the switches "texbatch" (57) and "vertexrange" (58), OFF
    ('static uint32_t emusuni_draw_low = 0;\n',
     'static uint32_t emusuni_draw_low = 0;\n// (20-11, "vertexrange", 58, OFF) the vertex the draw\'s per-vertex streams are copied from (0: from vertex 0)\nstatic uint32_t emusuni_range_low = 0;\n'),
    ('                    emusuni_vita_stat_add(49, stream_size);\n                    if (emusuni_below != 0)\n                        emusuni_vita_stat_add(50, emusuni_below);\n',
     '                    const unsigned long long emusuni_left_out = emusuni_range_low != 0 ? emusuni_below : 0ull;\n                    emusuni_vita_stat_add(49, stream_size - emusuni_left_out);\n                    if (emusuni_below != emusuni_left_out)\n                        emusuni_vita_stat_add(50, emusuni_below - emusuni_left_out);\n'),
    ('#if defined(__APPLE__) || defined(EMUSUNI_PORT)\n                // Vulkan allows any stride, but Metal only allows multiples of 4.\n',
     '#ifdef EMUSUNI_PORT\n                // ("vertexrange") a per-vertex stream from the draw\'s lowest vertex; draw() gives the GPU vertexOffset\n                // = -lowest, so index i fetches vertex i - lowest of the copy\n                if (emusuni_range_low != 0\n                    && !gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(vertex_program.streams[i].indexSource))) {\n                    const uint64_t emusuni_skip = static_cast<uint64_t>(emusuni_range_low) * vertex_program.streams[i].stride;\n                    if (emusuni_skip < stream_size) {\n                        stream += emusuni_skip;\n                        stream_size -= static_cast<uint32_t>(emusuni_skip);\n                    }\n                }\n#endif\n#if defined(__APPLE__) || defined(EMUSUNI_PORT)\n                // Vulkan allows any stride, but Metal only allows multiples of 4.\n'),
    ('    // bind the vertex streams\n    bind_vertex_streams(context, mem, instance_count, max_index);\n',
     '#ifdef EMUSUNI_PORT\n    // ("vertexrange", 58, OFF: HENkaku-PS4 copies "only the vertex range the draw uses") not on the ordered path,\n    // whose draws are one primitive each\n    emusuni_range_low = (!ordered_fetch && emusuni_draw_low != 0 && emusuni_vita_switch(58)) ? emusuni_draw_low : 0;\n#endif\n    // bind the vertex streams\n    bind_vertex_streams(context, mem, instance_count, max_index);\n'),
    ('    } else {\n        context.render_cmd.drawIndexed(count, instance_count, 0, 0, 0);\n    }\n',
     '    } else {\n#ifdef EMUSUNI_PORT\n        context.render_cmd.drawIndexed(count, instance_count, 0, -static_cast<int32_t>(emusuni_range_low), 0);\n        emusuni_range_low = 0;\n#else\n        context.render_cmd.drawIndexed(count, instance_count, 0, 0, 0);\n#endif\n    }\n'),
])
# (20-11, for 01.84, the switch "texbatch", 57, OFF) The command list in which a texture's bytes were last hashed
# (texture/cache.cpp).
patch('vita3k/renderer/include/renderer/texture_cache.h', [
    ('    uint16_t mip_count = 0;\n    SceGxmTextureBaseFormat format;\n};\n',
     '    uint16_t mip_count = 0;\n    SceGxmTextureBaseFormat format;\n#ifdef EMUSUNI_PORT\n    // (EmuSuNi, "texbatch") the command list in which its bytes were last hashed (cache.cpp)\n    uint64_t emusuni_batch = 0;\n#endif\n};\n'),
])
# (20-11) The renderer's line, "vita: renderer:" (port/ps4_vita_stats.cpp): a texture bound for a draw, its hash
# (time and bytes) and its sending to the GPU. HENkaku-PS4's own renderer line has the same parts; the two set side
# by side in one scene say where this renderer's thread spends what theirs does not (01.83's Uncharted: 87% of a core).
patch('vita3k/renderer/src/texture/cache.cpp', [
    ('namespace renderer {\nnamespace texture {\n',
     STAT_API + 'namespace renderer {\nnamespace texture {\n'),
    ('uint64_t hash_texture_data(const SceGxmTexture &texture, uint32_t texture_size, const MemState &mem) {\n',
     "uint64_t hash_texture_data(const SceGxmTexture &texture, uint32_t texture_size, const MemState &mem) {\n#ifdef EMUSUNI_PORT\n    // (20-11) a texture's bytes hashed to see whether they changed: the time (46) and the bytes (47)\n    const EmusuniVitaStatScope emusuni_stat_hash(46);\n    emusuni_vita_stat_add(47, texture_size);\n#endif\n"),
    ('void TextureCache::cache_and_bind_texture(const SceGxmTexture &gxm_texture, MemState &mem) {\n    R_PROFILE(__func__);\n',
     'void TextureCache::cache_and_bind_texture(const SceGxmTexture &gxm_texture, MemState &mem) {\n    R_PROFILE(__func__);\n#ifdef EMUSUNI_PORT\n    // (20-11) a texture bound for a draw: the time, its hash and its sending to the GPU in it (45)\n    const EmusuniVitaStatScope emusuni_stat_bind(45);\n#endif\n'),
    ('        else\n            upload_texture(gxm_texture, mem);\n',
     '        else {\n#ifdef EMUSUNI_PORT\n            emusuni_vita_stat_add(48, info->texture_size);\n#endif\n            upload_texture(gxm_texture, mem);\n        }\n'),
    # (20-11, for 01.84) the switches "texbatch" (57) and "vertexrange" (58), OFF
    ('namespace renderer {\nnamespace texture {\n',
     '#ifdef EMUSUNI_PORT\nextern "C" int emusuni_vita_switch(int which);\n// (EmuSuNi, "texbatch", 57) the renderer\'s command lists counted (batch.cpp: one more at each list\'s start; the\n// renderer\'s thread only)\nstatic uint64_t emusuni_texbatch = 1;\nextern "C" void emusuni_vita_texbatch_next(void) {\n    emusuni_texbatch++;\n}\n#endif\nnamespace renderer {\nnamespace texture {\n'),
    ('        info->use_hash = should_use_hash;\n',
     '        info->use_hash = should_use_hash;\n#ifdef EMUSUNI_PORT\n        info->emusuni_batch = emusuni_texbatch;\n#endif\n'),
    ('        if (info->use_hash) {\n            const uint64_t previous_hash = info->hash;\n            if (import_textures || export_textures)\n                info->hash = hash_texture_nostride(gxm_texture, mem);\n            else\n                info->hash = hash_texture_data(gxm_texture, info->texture_size, mem) ^ 1;\n\n            upload = previous_hash != info->hash;\n        } else {\n',
     '        if (info->use_hash) {\n#ifdef EMUSUNI_PORT\n            // (EmuSuNi, "texbatch", 57, OFF) hashed already in this command list: the draws of a list read the\n            // textures as they are when the renderer gets to it, so a second look finds the same bytes. HENkaku-PS4\n            // hashes so ("God of War II: 11,800 times a second"); upstream looks at every bind after the game\'s.\n            if (info->emusuni_batch == emusuni_texbatch && emusuni_vita_switch(57) && !emusuni_vita_switch(8)) {\n                upload = false;\n            } else {\n#endif\n            const uint64_t previous_hash = info->hash;\n            if (import_textures || export_textures)\n                info->hash = hash_texture_nostride(gxm_texture, mem);\n            else\n                info->hash = hash_texture_data(gxm_texture, info->texture_size, mem) ^ 1;\n\n            upload = previous_hash != info->hash;\n#ifdef EMUSUNI_PORT\n            }\n            info->emusuni_batch = emusuni_texbatch;\n#endif\n        } else {\n'),
])
patch('vita3k/libretro/libretro_input.cpp', [
    ('static uint8_t axis_to_vita(int16_t val) {\n', STAT_API + SWITCH_API + 'static uint8_t axis_to_vita(int16_t val) {\n'),
    ('    // L2 = rear touchpad modifier\n',
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_stat_pad(port_data[0].buttons);\n'
     '    if (port_data[0].buttons != 0)\n'
     '        emusuni_vita_stat_add(12, 0);\n'
     '#endif\n'
     '    // L2 = rear touchpad modifier\n'),
    # (After 01.47) The "centertouch" switch. The app sends no touch for this console yet (it does for the DS and
    # the 3DS), and Uncharted: Golden Abyss's title waits for one: "Touch to Start". While the frontend's pointer
    # isn't down, R3 - a button the PS Vita doesn't have - is a finger on the middle of the front screen (of the
    # rear panel with L2 held, as the frontend's own pointer goes there with it).
    ('    float ax = 0.0f, ay = 0.0f, az = -1.0f;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (touch_count == 0 && (port_data[0].buttons & SCE_CTRL_R3) && emusuni_vita_switch(12)) {\n'
     '        touch_points[0].x = 960;\n'
     '        touch_points[0].y = rear_modifier ? 498 : 544;\n'
     '        touch_points[0].id = 0;\n'
     '        touch_points[0].active = true;\n'
     '        touch_count = 1;\n'
     '    }\n'
     '    if (touch_count != 0)\n'
     '        emusuni_vita_stat_add(13, 0);\n'
     '#endif\n'
     '    float ax = 0.0f, ay = 0.0f, az = -1.0f;\n'),
    # (20-11, for 01.88) the Vita's motion from the app's sensors (the console's setting vita_motion)
    ('extern "C" unsigned long long emusuni_vita_stat_now(void);\n',
     'extern "C" unsigned long long emusuni_vita_stat_now(void);\n#ifdef EMUSUNI_PORT\nextern "C" int emusuni_vita_switch(int which);\nextern "C" void emusuni_vita_log(const char *format, ...);\n#endif\n'),
    ('        libretro.input.motion_enabled = true;\n    } else {\n        libretro.input.has_motion = false;\n    }\n',
     '        libretro.input.motion_enabled = true;\n    } else {\n        libretro.input.has_motion = false;\n    }\n#ifdef EMUSUNI_PORT\n    // (20-11, for 01.88) The DualShock 4\'s accelerometer and gyroscope as the Vita\'s (sceMotion: Rayman\'s tilt): the\n    // app offers them through the sensor interface only with the console\'s setting PS Vita Motion Sensor: DualShock 4\n    // (src/Emulator.cpp MotionVita, vita_motion), in the axes Vita3K gives a controller (SDL\'s x, -z, y: the\n    // controller\'s face is the Vita\'s screen). Said in the log.\n    emusuni_vita_log("vita: motion: %s", libretro.input.has_motion\n            ? "the DualShock 4\'s accelerometer and gyroscope are the Vita\'s (sceMotion; the setting PS Vita Motion Sensor)"\n            : "none: the Vita lies still (the setting PS Vita Motion Sensor: DualShock 4 gives it the controller\'s)");\n#endif\n'),
])
# (01.34) The "copybarrier" switch. A surface the game has drawn on and then samples as a texture in another
# format or part is copied first, in the commands that run before the scene's draws. Upstream that copy has no
# barrier of its own: on the PS4's GPU nothing then says that what earlier scenes drew must be on the surface
# before it is read, nor that the copy's first step (to a buffer) must end before its second - parts not written
# yet get copied, old picture or garbage. The destination's own change of layout after the copy covers the rest.
patch('vita3k/renderer/src/vulkan/surface_cache.cpp', [
    ('namespace renderer::vulkan {\n', STAT_API + SWITCH_API +
     '#ifdef EMUSUNI_PORT\n'
     '#include <atomic>\n'
     '#include <cstdio>\n'
     '#include <cstdlib>\n'
     '#include <cstring>\n'
     '#include <ctime>\n'
     '#include <set>\n'
     '#include <utility>\n'
     '#endif\n'
     'namespace renderer::vulkan {\n'
     # (After 01.78, LittleBigPlanet's 3D that is not drawn) A test, on the PC only: EMUSUNI_VITA_TEST_SURFACES=1
     # says once, for each texture that lies on a painted surface, what was done with it and why; and the first
     # writes of the game to a painted surface's memory.
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     'static bool emusuni_surfaces_on() {\n'
     '    static const bool on = getenv("EMUSUNI_VITA_TEST_SURFACES") != nullptr;\n'
     '    return on;\n'
     '}\n'
     'static void emusuni_surface_said(const SceGxmTexture &texture, uint32_t line_bytes, const ColorSurfaceCacheInfo &info, uint32_t at, const char *what) {\n'
     '    static std::set<std::pair<uint32_t, const char *>> said;\n'
     '    if (!emusuni_surfaces_on() || said.size() >= 240 || !said.emplace(static_cast<uint32_t>(texture.data_addr << 2), what).second)\n'
     '        return;\n'
     '    fprintf(stderr, "[emusuni-vita] test: surface: texture at %#x, %ux%u, type %#x, format %#x, %u bytes a line, and the surface painted at %#x, %ux%u, format %#x, %u bytes a line, %u in all: %s\\n",\n'
     '        static_cast<unsigned>(texture.data_addr << 2), static_cast<unsigned>(gxm::get_width(texture)), static_cast<unsigned>(gxm::get_height(texture)),\n'
     '        static_cast<unsigned>(texture.texture_type()), static_cast<unsigned>(gxm::get_format(texture)), static_cast<unsigned>(line_bytes), static_cast<unsigned>(at),\n'
     '        static_cast<unsigned>(info.original_width), static_cast<unsigned>(info.original_height), static_cast<unsigned>(info.format),\n'
     '        static_cast<unsigned>(info.stride_bytes), static_cast<unsigned>(info.total_bytes), what);\n'
     '}\n'
     'static void emusuni_surface_written(uint32_t surface, uint32_t bytes, uint32_t where) {\n'
     '    static std::atomic<int> left{80};\n'
     '    if (!emusuni_surfaces_on() || left.fetch_sub(1) <= 0)\n'
     '        return;\n'
     '    fprintf(stderr, "[emusuni-vita] test: surface: the game wrote at %#x: byte %d of the %u of the surface painted at %#x\\n",\n'
     '        static_cast<unsigned>(where), static_cast<int>(where - surface), static_cast<unsigned>(bytes), static_cast<unsigned>(surface));\n'
     '}\n'
     '#elif defined(EMUSUNI_PORT)\n'
     '#define emusuni_surface_said(...) ((void)0)\n'
     '#endif\n'),
    ('        [dirty, need_sync](Address, bool write) {\n'
     '            if (write && dirty)\n'
     '                *dirty = true;\n',
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '        [dirty, need_sync, emusuni_at = info.data.address(), emusuni_bytes = info.total_bytes](Address emusuni_where, bool write) {\n'
     '            if (write && dirty && !*dirty)\n'
     '                emusuni_surface_written(emusuni_at, emusuni_bytes, emusuni_where);\n'
     '#else\n'
     '        [dirty, need_sync](Address, bool write) {\n'
     '#endif\n'
     '            if (write && dirty)\n'
     '                *dirty = true;\n'),
    # (After 01.78) The counts of the log's second "vita: gfx:" line (port/ps4_vita_stats.cpp, kinds 33-36) and the
    # "surfacepick" switch (41) - port/ps4_vita_sys.h says what it is.
    ('    if (*ite->second->dirty)\n'
     '        // Guest wrote to the surface backing memory since it was rendered, so GPU data is stale.\n'
     '        return std::nullopt;\n',
     '    if (*ite->second->dirty) {\n'
     '        // Guest wrote to the surface backing memory since it was rendered, so GPU data is stale.\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_stat_add(34, 0);\n'
     '        emusuni_surface_said(texture, 0, *ite->second, ite->first, "not taken: the game wrote to the surface since it was painted");\n'
     '#endif\n'
     '        return std::nullopt;\n'
     '    }\n'),
    ('    uint32_t total_surface_size = stride_bytes * original_height;\n'
     '\n'
     '    ColorSurfaceCacheInfo &info = *ite->second;\n',
     '    uint32_t total_surface_size = stride_bytes * original_height;\n'
     '\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(41) && (tiling != ite->second->tiling || ite->second->stride_bytes != stride_bytes)) {\n'
     '        // the nearest surface below the texture is laid out otherwise: another one that holds it whole, laid out as it?\n'
     '        auto emusuni_other = ite;\n'
     '        for (int emusuni_step = 0; emusuni_step < 24 && emusuni_other != color_address_lookup.begin(); emusuni_step++) {\n'
     '            --emusuni_other;\n'
     '            const ColorSurfaceCacheInfo &emusuni_o = *emusuni_other->second;\n'
     '            if (emusuni_other->first + emusuni_o.total_bytes > address && emusuni_o.tiling == tiling && emusuni_o.stride_bytes == stride_bytes\n'
     '                && !*emusuni_o.dirty && address + total_surface_size <= emusuni_other->first + emusuni_o.total_bytes + 4) {\n'
     '                emusuni_picked = true;\n'
     '                emusuni_surface_said(texture, stride_bytes, *ite->second, ite->first, "the nearest surface, laid out otherwise: another one that holds the texture is looked at (surfacepick)");\n'
     '                ite = emusuni_other;\n'
     '                break;\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '#endif\n'
     '    ColorSurfaceCacheInfo &info = *ite->second;\n'),
    ('    if (tiling != info.tiling || info.stride_bytes != stride_bytes)\n'
     '        // if the tiling is different, also don\'t try to match them\n'
     '        // about the strides, I\'ve yet to see a case where the byte stride is different\n'
     '        return std::nullopt;\n',
     '    if (tiling != info.tiling || info.stride_bytes != stride_bytes) {\n'
     '        // if the tiling is different, also don\'t try to match them\n'
     '        // about the strides, I\'ve yet to see a case where the byte stride is different\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_stat_add(35, 0);\n'
     '        emusuni_surface_said(texture, stride_bytes, info, ite->first, "not taken: another tiling or another number of bytes a line");\n'
     '#endif\n'
     '        return std::nullopt;\n'
     '    }\n'),
    ('    // We should be able to use this texture, so set it as mru\n'
     '    color_surface_queue.set_as_mru(&info);\n',
     '    // We should be able to use this texture, so set it as mru\n'
     '    color_surface_queue.set_as_mru(&info);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_stat_add(33, 0);\n'
     '    if (emusuni_picked)\n'
     '        emusuni_vita_stat_add(36, 0);\n'
     '    emusuni_surface_said(texture, stride_bytes, info, ite->first, "taken from the surface");\n'
     '#endif\n'),
    ('        casted->scene_timestamp = scene_timestamp;\n',
     '        casted->scene_timestamp = scene_timestamp;\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_stat_add(10, 0);\n'
     '        if (emusuni_vita_switch(6)) {\n'
     '            const vk::MemoryBarrier emusuni_drawn{\n'
     '                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,\n'
     '                .dstAccessMask = vk::AccessFlagBits::eTransferRead\n'
     '            };\n'
     '            cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,\n'
     '                {}, emusuni_drawn, {}, {});\n'
     '        }\n'
     '#endif\n'),
    ('            // then the buffer to the image\n',
     '#ifdef EMUSUNI_PORT\n'
     '            if (emusuni_vita_switch(6)) {\n'
     '                const vk::MemoryBarrier emusuni_copied{\n'
     '                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,\n'
     '                    .dstAccessMask = vk::AccessFlagBits::eTransferRead\n'
     '                };\n'
     '                cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,\n'
     '                    {}, emusuni_copied, {}, {});\n'
     '            }\n'
     '#endif\n'
     '            // then the buffer to the image\n'),
    # (The same switch, the other way to miss: the nearest surface below the texture ends before it - upstream
    # stops there, "no painted surface" - while one further below holds it. This edit's old text is the dirty
    # check as the edit above leaves it.)
    ('    if (!overlap)\n'
     '        return std::nullopt;\n'
     '\n'
     '    if (*ite->second->dirty) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '    bool emusuni_picked = false;\n'
     '    if (!overlap && ite != color_address_lookup.begin() && emusuni_vita_switch(41)) {\n'
     '        auto emusuni_other = ite;\n'
     '        for (int emusuni_step = 0; emusuni_step < 24 && emusuni_other != color_address_lookup.begin(); emusuni_step++) {\n'
     '            --emusuni_other;\n'
     '            if (emusuni_other->first + emusuni_other->second->total_bytes > address) {\n'
     '                emusuni_surface_said(texture, 0, *ite->second, ite->first, "the nearest surface ends before the texture: another one that holds its address is looked at (surfacepick)");\n'
     '                ite = emusuni_other;\n'
     '                overlap = true;\n'
     '                emusuni_picked = true;\n'
     '                break;\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '#endif\n'
     '    if (!overlap)\n'
     '        return std::nullopt;\n'
     '\n'
     '    if (*ite->second->dirty) {\n'),
    # (After 01.78) A test, on the PC only: EMUSUNI_VITA_TEST_DUMP_SURFACES=<seconds>[,<seconds>...] with
    # EMUSUNI_VITA_TEST_DUMP_DIR=<folder> writes, that long after the first scene, every painted surface as the GPU
    # has it to a file each (local/herramientas/prueba-vita/surfpng.py makes pictures of them): the steps between
    # a game's scene and the screen, which the bench could not see.
    ('SurfaceRetrieveResult VKSurfaceCache::retrieve_color_surface_for_framebuffer(MemState &mem, SceGxmColorSurface *color) {\n',
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     'template <typename Lookup>\n'
     'static void emusuni_dump_surfaces(VKState &state, Lookup &lookup) {\n'
     '    static const char *const text = getenv("EMUSUNI_VITA_TEST_DUMP_SURFACES");\n'
     '    if (text == nullptr)\n'
     '        return;\n'
     '    static const char *at = text;\n'
     '    static int nth = 0;\n'
     '    static unsigned long long first = 0;\n'
     '    timespec now;\n'
     '    clock_gettime(CLOCK_MONOTONIC, &now);\n'
     '    const unsigned long long ms = now.tv_sec * 1000ull + now.tv_nsec / 1000000ull;\n'
     '    if (first == 0)\n'
     '        first = ms;\n'
     '    if (at == nullptr || *at == 0 || ms - first < strtoull(at, nullptr, 10) * 1000ull)\n'
     '        return;\n'
     '    at = strchr(at, 44); // (the next time, after the comma)\n'
     '    if (at != nullptr)\n'
     '        at++;\n'
     '    nth++;\n'
     '    const char *dir = getenv("EMUSUNI_VITA_TEST_DUMP_DIR");\n'
     '    if (dir == nullptr)\n'
     '        dir = "/tmp";\n'
     '    int written = 0;\n'
     '    for (auto &[address, info] : lookup) {\n'
     '        if (!info->texture.image)\n'
     '            continue;\n'
     '        const vk::DeviceSize size = static_cast<vk::DeviceSize>(info->width) * info->height * vk::blockSize(info->texture.format);\n'
     '        vkutil::Buffer buffer(size);\n'
     '        buffer.init_buffer(vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);\n'
     '        vk::CommandBuffer cmd = vkutil::create_single_time_command(state.device, state.general_command_pool);\n'
     '        const vk::BufferImageCopy copy{\n'
     '            .bufferOffset = 0,\n'
     '            .bufferRowLength = info->width,\n'
     '            .bufferImageHeight = info->height,\n'
     '            .imageSubresource = vkutil::color_subresource_layer,\n'
     '            .imageOffset = { 0, 0, 0 },\n'
     '            .imageExtent = { info->width, info->height, 1 }\n'
     '        };\n'
     '        cmd.copyImageToBuffer(info->texture.image, vk::ImageLayout::eGeneral, buffer.buffer, copy);\n'
     '        if (state.libretro_lock_queue)\n'
     '            state.libretro_lock_queue(state.libretro_queue_handle);\n'
     '        vkutil::end_single_time_command(state.device, state.general_queue, state.general_command_pool, cmd);\n'
     '        if (state.libretro_unlock_queue)\n'
     '            state.libretro_unlock_queue(state.libretro_queue_handle);\n'
     '        char name[600];\n'
     '        snprintf(name, sizeof(name), "%s/surf%d-%08x-%ux%u-g%08x-v%d.raw", dir, nth, static_cast<unsigned>(address), static_cast<unsigned>(info->width),\n'
     '            static_cast<unsigned>(info->height), static_cast<unsigned>(info->format), static_cast<int>(info->texture.format));\n'
     '        if (FILE *file = fopen(name, "wb")) {\n'
     '            fwrite(buffer.mapped_data, 1, size, file);\n'
     '            fclose(file);\n'
     '            written++;\n'
     '        }\n'
     '    }\n'
     '    fprintf(stderr, "[emusuni-vita] test: surfaces: dump %d: %d painted surfaces written to %s\\n", nth, written, dir);\n'
     '}\n'
     '#endif\n'
     '\n'
     'SurfaceRetrieveResult VKSurfaceCache::retrieve_color_surface_for_framebuffer(MemState &mem, SceGxmColorSurface *color) {\n'
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '    emusuni_dump_surfaces(state, color_address_lookup);\n'
     '#endif\n'),
    # (20-11) a PC test: why each copy of a painted surface for a texture is made
    ('        emusuni_vita_stat_add(10, 0);\n',
     '        emusuni_vita_stat_add(10, 0);\n#if !defined(__PS4__)\n        if (emusuni_surfaces_on()) {\n            // (20-11, a PC test) why this copy: 1 the surface is the one being drawn on, 2 the texture starts\n            // inside it, 4 another size, 8 another format\n            static unsigned long long emusuni_why[16];\n            static unsigned long long emusuni_casts = 0;\n            static std::set<unsigned long long> emusuni_said;\n            const int emusuni_reason = (is_same_image ? 1 : 0) | ((start_sourced_line != 0 || start_x != 0) ? 2 : 0)\n                | ((info.width != width || info.height != height) ? 4 : 0) | ((info.format != base_format) ? 8 : 0);\n            emusuni_why[emusuni_reason]++;\n            if (emusuni_said.size() < 120 && emusuni_said.insert(static_cast<unsigned long long>(ite->first) << 32 | static_cast<unsigned long long>(emusuni_reason) << 28 | static_cast<unsigned long long>(width & 0x3fff) << 14 | (height & 0x3fff)).second)\n                fprintf(stderr, "[emusuni-vita] test: cast: surface at %#x %ux%u format %#x (drawn now: %d) copied for a texture %ux%u format %#x from %u,%u; reason %d; scene %llu\\n",\n                    static_cast<unsigned>(ite->first), static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),\n                    static_cast<unsigned>(info.format), is_same_image ? 1 : 0, static_cast<unsigned>(width), static_cast<unsigned>(height),\n                    static_cast<unsigned>(base_format), static_cast<unsigned>(start_x), static_cast<unsigned>(start_sourced_line),\n                    emusuni_reason, static_cast<unsigned long long>(scene_timestamp));\n            if (++emusuni_casts % 1000 == 0) {\n                char emusuni_line[400];\n                int emusuni_n = snprintf(emusuni_line, sizeof(emusuni_line), "[emusuni-vita] test: casts: %llu copies, by reason:", emusuni_casts);\n                for (int emusuni_r = 0; emusuni_r < 16 && emusuni_n > 0 && emusuni_n < 360; emusuni_r++)\n                    if (emusuni_why[emusuni_r] != 0)\n                        emusuni_n += snprintf(emusuni_line + emusuni_n, sizeof(emusuni_line) - emusuni_n, " %d:%llu", emusuni_r, emusuni_why[emusuni_r]);\n                fprintf(stderr, "%s\\n", emusuni_line);\n            }\n        }\n#endif\n'),
    # (20-11) a PC test: the surface cache at each framebuffer (EMUSUNI_VITA_TEST_SCENES)
    ('#include <set>\n#include <utility>\n#endif\nnamespace renderer::vulkan {\n',
     '#include <set>\n#include <utility>\n#endif\n#if !defined(__PS4__)\n#include <chrono>\n#endif\n#if !defined(__PS4__)\n// (20-11, a PC test: EMUSUNI_VITA_TEST_SCENES=1) the surface cache at each scene\'s framebuffer: 0 calls, 1 colour\n// surfaces made, 2 of them after an invalidation, 3 invalidated as freed, 4 as changed (extent, format), 5 as out of\n// range, 6 depth-stencil surfaces made, 7 framebuffers made; the time of: 8 the colour surface, 9 the depth-stencil\n// surface, 10 the rest (the lookup, a framebuffer made)\nstatic unsigned long long emusuni_fb[11];\nstatic std::chrono::steady_clock::time_point emusuni_fb_at;\nstatic bool emusuni_fb_on() {\n    static int on = -1;\n    if (on < 0) {\n        const char *e = getenv("EMUSUNI_VITA_TEST_SCENES");\n        on = e && *e == \'1\';\n    }\n    return on != 0;\n}\nstatic void emusuni_fb_say(size_t framebuffers, size_t colours, size_t depths) {\n    const auto now = std::chrono::steady_clock::now();\n    if (emusuni_fb_at.time_since_epoch().count() == 0)\n        emusuni_fb_at = now;\n    if (now - emusuni_fb_at < std::chrono::seconds(10))\n        return;\n    emusuni_fb_at = now;\n    const double n = emusuni_fb[0] ? static_cast<double>(emusuni_fb[0]) : 1.0;\n    fprintf(stderr, "[emusuni-vita] test: framebuffer: %llu scenes; colour surfaces made %llu (%llu after an invalidation: "\n        "freed %llu, changed %llu, out of range %llu), depth-stencil made %llu, framebuffers made %llu; us each: colour %.1f, "\n        "depth %.1f, the rest %.1f; framebuffers kept %zu, colour surfaces %zu, depth %zu\\n",\n        emusuni_fb[0], emusuni_fb[1], emusuni_fb[2], emusuni_fb[3], emusuni_fb[4], emusuni_fb[5], emusuni_fb[6], emusuni_fb[7],\n        emusuni_fb[8] / 1000.0 / n, emusuni_fb[9] / 1000.0 / n, emusuni_fb[10] / 1000.0 / n, framebuffers, colours, depths);\n    for (auto &v : emusuni_fb)\n        v = 0;\n}\nstatic unsigned long long emusuni_fb_ns() {\n    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());\n}\n#endif\nnamespace renderer::vulkan {\n'),
    ('    // First retrieve separately the color surface and ds surface\n    SurfaceRetrieveResult color_result;\n    SurfaceRetrieveResult ds_result;\n',
     '    // First retrieve separately the color surface and ds surface\n    SurfaceRetrieveResult color_result;\n    SurfaceRetrieveResult ds_result;\n#if !defined(__PS4__)\n    const bool emusuni_on = emusuni_fb_on();\n    unsigned long long emusuni_t = emusuni_on ? emusuni_fb_ns() : 0;\n    if (emusuni_on)\n        emusuni_fb[0]++;\n#endif\n'),
    ('    if (depth_stencil) {\n        ds_result = retrieve_depth_stencil_for_framebuffer(depth_stencil, target->width, target->height);\n',
     '#if !defined(__PS4__)\n    if (emusuni_on) {\n        const unsigned long long emusuni_now = emusuni_fb_ns();\n        emusuni_fb[8] += emusuni_now - emusuni_t;\n        emusuni_t = emusuni_now;\n    }\n#endif\n    if (depth_stencil) {\n        ds_result = retrieve_depth_stencil_for_framebuffer(depth_stencil, target->width, target->height);\n'),
    ('    color_view = color_result.view;\n    ds_view = ds_result.view;\n',
     '#if !defined(__PS4__)\n    if (emusuni_on) {\n        const unsigned long long emusuni_now = emusuni_fb_ns();\n        emusuni_fb[9] += emusuni_now - emusuni_t;\n        emusuni_t = emusuni_now;\n    }\n#endif\n    color_view = color_result.view;\n    ds_view = ds_result.view;\n'),
    ('    if (it != framebuffer_array.end()) {\n        // we already created a framebuffer for this pair\n        return it->second;\n    }\n',
     '    if (it != framebuffer_array.end()) {\n        // we already created a framebuffer for this pair\n#if !defined(__PS4__)\n        if (emusuni_on) {\n            emusuni_fb[10] += emusuni_fb_ns() - emusuni_t;\n            emusuni_fb_say(framebuffer_array.size(), color_address_lookup.size(), depth_address_lookup.size());\n        }\n#endif\n        return it->second;\n    }\n#if !defined(__PS4__)\n    if (emusuni_on)\n        emusuni_fb[7]++;\n#endif\n'),
    ('    return (framebuffer_array[key] = { fb_standard, fb_interlock, color_result.base_image });\n',
     '#if !defined(__PS4__)\n    if (emusuni_on) {\n        emusuni_fb[10] += emusuni_fb_ns() - emusuni_t;\n        emusuni_fb_say(framebuffer_array.size() + 1, color_address_lookup.size(), depth_address_lookup.size());\n    }\n#endif\n    return (framebuffer_array[key] = { fb_standard, fb_interlock, color_result.base_image });\n'),
    ('        if (invalidated) {\n            destroy_surface(info);\n',
     '        if (invalidated) {\n#if !defined(__PS4__)\n            if (emusuni_fb_on()) {\n                emusuni_fb[2]++;\n                emusuni_fb[cache_probably_freed ? 3 : surface_stat_changed ? 4 : 5]++;\n            }\n#endif\n            destroy_surface(info);\n'),
    ('    // get the least recently used (probably unused) color surface\n    ColorSurfaceCacheInfo &info_added = *color_surface_queue.get_lru();\n',
     '    // get the least recently used (probably unused) color surface\n    ColorSurfaceCacheInfo &info_added = *color_surface_queue.get_lru();\n#if !defined(__PS4__)\n    if (emusuni_fb_on())\n        emusuni_fb[1]++;\n#endif\n'),
    ('    // erase it if it was used previously\n    if (cached_info->surface.depth_data)\n',
     '#if !defined(__PS4__)\n    if (emusuni_fb_on())\n        emusuni_fb[6]++;\n#endif\n    // erase it if it was used previously\n    if (cached_info->surface.depth_data)\n'),
    # (20-11, for 01.86) the switch "surfacewatch" (62, ON)
    ('            return true;\n        });\n}\n\nColorSurfaceCacheInfo::~ColorSurfaceCacheInfo() {\n',
     "            return true;\n        }\n#ifdef EMUSUNI_PORT\n        // (surfacewatch, 62) this slot's watch: one made again here replaces it (mem.cpp)\n        , static_cast<const void *>(&info)\n#endif\n    );\n}\n\nColorSurfaceCacheInfo::~ColorSurfaceCacheInfo() {\n"),
])
# A shader made from the game's, and a pipeline compiled (on the renderer's thread or on one of the three that
# compile them aside).
# (After 01.68) The "vertexw" switch (22): a vertex shader for each layout of its vertex data, on a GPU that
# doesn't take vertex data of 3 components of 8 or 16 bits - AMD's of the PS4's generation, upstream's own
# comment says, "checked on AMD Radeon HD 7800". For such an attribute upstream then reads 4 components (the 4th is
# whatever lies after the 3rd in the vertex) and writes into the vertex shader, attribute by attribute, whether
# that 4th one must be set to 1 - as the layout it is first made for has it. But a shader is kept, in memory and
# in the disk's cache, by its program alone: a game that uses the same vertex program with another layout gets
# the shader of the first one - a 4th component of garbage where 1 was meant (a position's: the vertex goes
# anywhere; a colour's: another colour), or 1 where the vertex had a value. With the switch the shader is kept
# by the program AND by which of its attributes have 3 components, so each layout has its own; the disk's files
# of such a GPU's vertex shaders get a name of their own too (the old ones, made for who knows which layout,
# are not read). The PC bench's GPU takes those formats and never ran this: EMUSUNI_VITA_TEST_NO_RGB_ATTR=1
# (the PC only) has it taken for one that doesn't. And a line of the log says what the GPU takes ("vita: gpu:").
patch('vita3k/renderer/src/vulkan/pipeline_cache.cpp', [
    ('namespace renderer::vulkan {\n',
     STAT_API + SWITCH_API + '#ifdef EMUSUNI_PORT\n#include <atomic>\n#include <cstdlib>\n#include <string>\n#endif\nnamespace renderer::vulkan {\n' + RELAY_FN),
    ('            if (!(rgb_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer)) {\n'
     '                unsupported_rgb_vertex_attribute_formats.emplace(fmt);\n'
     '            }\n'
     '        }\n',
     '            if (!(rgb_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer)) {\n'
     '                unsupported_rgb_vertex_attribute_formats.emplace(fmt);\n'
     '            }\n'
     '        }\n'
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '        if (getenv("EMUSUNI_VITA_TEST_NO_RGB_ATTR")) {\n'
     '            for (auto fmt : formats)\n'
     '                unsupported_rgb_vertex_attribute_formats.emplace(fmt);\n'
     '        }\n'
     '#endif\n'),
    ('        state.features.support_rgb_attributes = unsupported_rgb_vertex_attribute_formats.empty();\n',
     '        state.features.support_rgb_attributes = unsupported_rgb_vertex_attribute_formats.empty();\n'
     '#ifdef EMUSUNI_PORT\n'
     '        emusuni_vita_log("vita: gpu: vertex data of 3 components: the GPU doesn\'t take %d of its 13 formats of 8 and 16 bits%s; scaled vertex formats: %s",\n'
     '            static_cast<int>(unsupported_rgb_vertex_attribute_formats.size()),\n'
     '            unsupported_rgb_vertex_attribute_formats.empty() ? "" : (emusuni_vita_switch(22)\n'
     '                ? " (4 components are read for those; a vertex shader for each layout: the vertexw switch is ON)"\n'
     '                : " (4 components are read for those; one vertex shader whatever the layout: the vertexw switch is OFF)"),\n'
     '            support_scaled_vertex_attribute ? "taken" : "not taken (converted in the shader)");\n'
     '#endif\n'),
    ('vk::PipelineShaderStageCreateInfo PipelineCache::retrieve_shader(const SceGxmProgram *program, const Sha256Hash &hash, bool is_vertex, bool maskupdate, MemState &mem, const shader::Hints &hints, bool is_srgb) {\n',
     'vk::PipelineShaderStageCreateInfo PipelineCache::retrieve_shader(const SceGxmProgram *program, const Sha256Hash &emusuni_program_hash, bool is_vertex, bool maskupdate, MemState &mem, const shader::Hints &hints, bool is_srgb) {\n'
     '    Sha256Hash hash = emusuni_program_hash;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // (The "vertexw" switch) Which attributes of this layout have 3 components, as a number that doesn\'t depend on\n'
     '    // their order: part of the shader\'s key here and of its file\'s name.\n'
     '    std::string emusuni_variant;\n'
     '    if (is_vertex && !state.features.support_rgb_attributes && hints.attributes && emusuni_vita_switch(22)) {\n'
     '        std::uint32_t emusuni_layout = 0x9E3779B9u;\n'
     '        for (const SceGxmVertexAttribute &attribute : *hints.attributes) {\n'
     '            if (attribute.componentCount == 3)\n'
     '                emusuni_layout += (static_cast<std::uint32_t>(attribute.regIndex) + 1u) * 2654435761u;\n'
     '        }\n'
     '        char emusuni_text[16];\n'
     '        snprintf(emusuni_text, sizeof(emusuni_text), "-w%08x", static_cast<unsigned>(emusuni_layout));\n'
     '        emusuni_variant = emusuni_text;\n'
     '        for (int i = 0; i < 4; i++)\n'
     '            hash[i] ^= static_cast<std::uint8_t>(emusuni_layout >> (8 * i));\n'
     '        hash[4] ^= 0x5A;\n'
     '    }\n'
     '#endif\n'),
    ('    const std::string version = masked_fetch ? shader_version + "-masked-fetch1" : shader_version;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const std::string version = (masked_fetch ? shader_version + "-masked-fetch1" : shader_version) + emusuni_variant;\n'
     '#else\n'
     '    const std::string version = masked_fetch ? shader_version + "-masked-fetch1" : shader_version;\n'
     '#endif\n'),
    # (The "vertexrelay" switch) Each attribute where the draw's copy of its stream has it.
    ('        for (uint32_t i = 0; i < array_size; i++) {\n            attr_descr.push_back(vk::VertexInputAttributeDescription{\n',
     '#ifdef EMUSUNI_PORT\n'
     '        const EmusuniRelay emusuni_attr_layout = emusuni_relay(vertex_program, attribute.streamIndex);\n'
     '        const size_t emusuni_index = static_cast<size_t>(&attribute - vertex_program.attributes.data());\n'
     '        const uint32_t emusuni_offset = emusuni_attr_layout.relaid && emusuni_index < 32 ? emusuni_attr_layout.offsets[emusuni_index] : attribute.offset;\n'
     '#else\n'
     '        const uint32_t emusuni_offset = attribute.offset;\n'
     '#endif\n'
     '        for (uint32_t i = 0; i < array_size; i++) {\n            attr_descr.push_back(vk::VertexInputAttributeDescription{\n'),
    ('                .offset = attribute.offset + i * array_element_size });\n',
     '                .offset = emusuni_offset + i * array_element_size });\n'),
    # (The "vertexalign" switch: see vulkan/scene.cpp's patch) The pipeline is told the padded distance between
    # vertices; and the log names the layouts that hold numbers of 2 or 4 bytes at places the PS4's GPU may read
    # wrong - vertices an odd distance apart (padded with that switch), or a number at an odd place inside its
    # vertex (the stream laid out anew with the "vertexrelay" switch).
    ('#ifdef __APPLE__\n        const uint32_t stride = align(stream.stride, 4);\n#else\n        const uint32_t stride = stream.stride;\n#endif\n',
     '#if defined(__APPLE__)\n'
     '        const uint32_t stride = align(stream.stride, 4);\n'
     '#elif defined(EMUSUNI_PORT)\n'
     '        const EmusuniRelay emusuni_layout = emusuni_relay(vertex_program, stream_index);\n'
     '        const uint32_t stride = emusuni_layout.relaid ? emusuni_layout.stride\n'
     '            : (emusuni_vita_switch(23) ? ((static_cast<uint32_t>(stream.stride) + 3u) & ~3u) : stream.stride);\n'
     '        {\n'
     '            static std::atomic<int> emusuni_said{ 0 };\n'
     '            for (const SceGxmVertexAttribute &emusuni_a : vertex_program.attributes) {\n'
     '                if (emusuni_a.streamIndex != stream_index)\n'
     '                    continue;\n'
     '                const uint32_t emusuni_part = static_cast<uint32_t>(gxm::attribute_format_size(static_cast<SceGxmAttributeFormat>(emusuni_a.format)));\n'
     '                const bool emusuni_odd_place = emusuni_part >= 2 && (emusuni_a.offset % emusuni_part) != 0;\n'
     '                const bool emusuni_odd_apart = emusuni_part >= 2 && (stream.stride % emusuni_part) != 0;\n'
     '                if ((emusuni_odd_place || emusuni_odd_apart) && emusuni_said.fetch_add(1) < 16)\n'
     '                    emusuni_vita_log("vita: gpu: a vertex layout has numbers of %u bytes at byte %u of vertices %u bytes apart: %s",\n'
     '                        static_cast<unsigned>(emusuni_part), static_cast<unsigned>(emusuni_a.offset), static_cast<unsigned>(stream.stride),\n'
     '                        emusuni_odd_place ? (emusuni_layout.relaid ? "an odd place inside the vertex: the stream is laid out anew, each attribute at a multiple of 4 bytes (the vertexrelay switch is ON)"\n'
     '                                                                   : "an odd place inside the vertex, as it is (the vertexrelay switch is OFF)")\n'
     '                                          : (emusuni_vita_switch(23) ? "the vertices are padded to a multiple of 4 bytes (the vertexalign switch is ON)"\n'
     '                                                                     : "as they are (the vertexalign switch is OFF)"));\n'
     '            }\n'
     '        }\n'
     '#else\n'
     '        const uint32_t stride = stream.stride;\n'
     '#endif\n'),
    ('    LOG_INFO("Generating vulkan spv shader {}", hash_text);\n',
     '    LOG_INFO("Generating vulkan spv shader {}", hash_text);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    const EmusuniVitaStatScope emusuni_stat_shader(2);\n'
     '#endif\n'),
    ('    const VertexProgram &vertex_program = *vertex_program_gxm.renderer_data;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const EmusuniVitaStatScope emusuni_stat_pipeline(3);\n'
     '#endif\n'
     '    const VertexProgram &vertex_program = *vertex_program_gxm.renderer_data;\n'),
    # (20-11, for 01.84) the "sixcores" switch (56, OFF): port/ps4_vita_place.cpp
    ('void PipelineCache::compiler_thread(MemState &mem) {\n',
     '#ifdef EMUSUNI_PORT\nextern "C" void emusuni_vita_sixcores_here(int who);\n#endif\nvoid PipelineCache::compiler_thread(MemState &mem) {\n#ifdef EMUSUNI_PORT\n    // (20-11, "sixcores", 56) a pipeline compiler on the PS4\'s cores 0-5 (port/ps4_vita_place.cpp)\n    emusuni_vita_sixcores_here(2);\n#endif\n'),
])
# A file the game opens (the search for its name in another case in it, when it isn't there as asked).
# (01.31) And the search for a file's name in another case: upstream lists the game's whole folder again for every
# file that isn't there - 50 ms each on the PS4 (01.30's log: 292 files and 181 stat calls of Street Fighter X
# Tekken's, each probed in ten folders before the one that has it). Here a folder listed less than 10 s ago isn't
# listed again: what it holds is in the cache already (app0, addcont0 and vs0 are read-only to the game). The
# times are forgotten when the cache is emptied (a game's start): a game started again at once would otherwise
# find the cache empty and its folder "just listed".
# (01.34) The "filewalk" switch: one such listing still cost 2.2-2.4 s on the console (01.32's log: the file
# thread's time in 8 of 25 stretches). With the switch nothing is listed whole: the path is followed name by
# name below the game's folder, each name taken as it is when it is there and, when it isn't, looked for
# whatever its case among its own folder's names (that one folder listed, kept 10 s). Only the path found goes
# into the cache. And one lock for the cache and these, which the game's threads use at once.
# (19-11, the switch "iolock", 53, ON) IOState's tables - open files, terminals, folders, found paths - behind a
# lock each while they are looked into or changed, and next_fd atomic: 01.82's LittleBigPlanet fell 18 s after
# its start in std::map's insert inside open_file (crash.log: __tree_balance_after_insert <- open_file), many of
# its threads opening fonts at once. io.cpp keeps its calls as they are.
patch('vita3k/io/include/io/state.h', [
    ('using TtyFiles = std::map<SceUID, TtyType>;\n'
     'using StdFiles = std::map<SceUID, FileStats>;\n'
     'using DirEntries = std::map<SceUID, DirStats>;\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <atomic>\n'
     '#include <mutex>\n'
     '#include <utility>\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     '// (EmuSuNi, the switch "iolock", 53, ON) A table of IOState with a lock of its own, held only while it is looked\n'
     "// into or changed - the file's own work (reading, writing) is done outside it: the game's threads open, read and\n"
     "// close files at once (LittleBigPlanet's fonts at its start: 01.82 fell in std::map's insert inside open_file,\n"
     "// a tree two threads changed together). The same calls as the map's; an iterator stays good while its entry is\n"
     "// not erased, as std::map's. The file vita-iolock-off: no lock, as upstream.\n"
     'template <typename Map>\n'
     'struct EmusuniLockedTable {\n'
     '    using iterator = typename Map::iterator;\n'
     '    using const_iterator = typename Map::const_iterator;\n'
     '    struct Guard {\n'
     '        std::mutex *held;\n'
     '        explicit Guard(std::mutex &mutex)\n'
     '            : held(emusuni_vita_switch(53) ? &mutex : nullptr) {\n'
     '            if (held)\n'
     '                held->lock();\n'
     '        }\n'
     '        ~Guard() {\n'
     '            if (held)\n'
     '                held->unlock();\n'
     '        }\n'
     '    };\n'
     '    template <typename Key>\n'
     '    iterator find(const Key &key) {\n'
     '        Guard guard(mutex);\n'
     '        return map.find(key);\n'
     '    }\n'
     '    template <typename Key>\n'
     '    const_iterator find(const Key &key) const {\n'
     '        Guard guard(mutex);\n'
     '        return map.find(key);\n'
     '    }\n'
     '    iterator end() { return map.end(); }\n'
     '    const_iterator end() const { return map.end(); }\n'
     '    template <typename... Args>\n'
     '    auto emplace(Args &&...args) {\n'
     '        Guard guard(mutex);\n'
     '        return map.emplace(std::forward<Args>(args)...);\n'
     '    }\n'
     '    template <typename Key>\n'
     '    auto erase(const Key &key) {\n'
     '        Guard guard(mutex);\n'
     '        return map.erase(key);\n'
     '    }\n'
     '    void clear() {\n'
     '        Guard guard(mutex);\n'
     '        map.clear();\n'
     '    }\n'
     '\n'
     '    Map map;\n'
     '    mutable std::mutex mutex;\n'
     '};\n'
     'using TtyFiles = EmusuniLockedTable<std::map<SceUID, TtyType>>;\n'
     'using StdFiles = EmusuniLockedTable<std::map<SceUID, FileStats>>;\n'
     'using DirEntries = EmusuniLockedTable<std::map<SceUID, DirStats>>;\n'
     '#else\n'
     'using TtyFiles = std::map<SceUID, TtyType>;\n'
     'using StdFiles = std::map<SceUID, FileStats>;\n'
     'using DirEntries = std::map<SceUID, DirStats>;\n'
     '#endif\n'),
    ('    SceUID next_fd = 0;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    std::atomic<SceUID> next_fd = 0; // ("iolock") two threads opening at once get two numbers\n'
     '#else\n'
     '    SceUID next_fd = 0;\n'
     '#endif\n'),
    ('    std::unordered_map<std::string, std::string> cachemap;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    EmusuniLockedTable<std::unordered_map<std::string, std::string>> cachemap; // ("iolock")\n'
     '#else\n'
     '    std::unordered_map<std::string, std::string> cachemap;\n'
     '#endif\n'),
])

patch('vita3k/io/src/io.cpp', [
    # (After 01.70) A test, on the PC only: EMUSUNI_VITA_TEST_READ_MS=<ms> has every read of a game's file take
    # that long more (with EMUSUNI_VITA_TEST_READ_MIN=<bytes>, only the reads of that many bytes or more) - the
    # console reads a game out of a ZIP on a USB drive, 3 to 200 ms a read, the bench from a disk in
    # microseconds: what a game does while it waits for its files only shows with reads that take their time.
    ('        const auto read = file->second.read(data, 1, size);\n',
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '        {\n'
     '            static const int emusuni_read_ms = [] {\n'
     '                const char *const text = getenv("EMUSUNI_VITA_TEST_READ_MS");\n'
     '                return text ? atoi(text) : 0;\n'
     '            }();\n'
     '            static const long emusuni_read_min = [] {\n'
     '                const char *const text = getenv("EMUSUNI_VITA_TEST_READ_MIN");\n'
     '                return text ? atol(text) : 0;\n'
     '            }();\n'
     '            if (emusuni_read_ms > 0 && static_cast<long>(size) >= emusuni_read_min)\n'
     '                std::this_thread::sleep_for(std::chrono::milliseconds(emusuni_read_ms));\n'
     '        }\n'
     '#endif\n'
     '        const auto read = file->second.read(data, 1, size);\n'),
    ('namespace vfs {\n',
     STAT_API + SWITCH_API +
     '#ifdef EMUSUNI_PORT\n#include <chrono>\n#include <cstdlib>\n#include <mutex>\n#include <thread>\n#include <unordered_map>\n'
     '// (The search of a name in another case: what it keeps, and the cache it fills. Emptied with the cache.)\n'
     'static std::mutex emusuni_names_mutex;\n'
     '// (When each game folder was last listed whole: the "filewalk" switch OFF.)\n'
     'static std::unordered_map<std::string, std::chrono::steady_clock::time_point> emusuni_listed;\n'
     '// (A folder\'s own names by their lower-case form, kept 10 s: the "filewalk" switch ON.)\n'
     'struct EmusuniFolderNames {\n'
     '    std::chrono::steady_clock::time_point listed;\n'
     '    std::unordered_map<std::string, std::string> names;\n'
     '};\n'
     'static std::unordered_map<std::string, EmusuniFolderNames> emusuni_folders;\n'
     '// The path `rest` below the folder `base`, each of its names as its folder has it whatever the case: true and\n'
     '// the path in `found` when all of them are there. (emusuni_names_mutex held.)\n'
     'static bool emusuni_find_below(const std::string &base, const std::string &rest, std::string &found) {\n'
     '    std::string at = base;\n'
     '    while (at.size() > 1 && at.back() == \'/\')\n'
     '        at.pop_back();\n'
     '    std::error_code ec;\n'
     '    size_t from = 0;\n'
     '    while (from < rest.size()) {\n'
     '        size_t to = rest.find(\'/\', from);\n'
     '        if (to == std::string::npos)\n'
     '            to = rest.size();\n'
     '        const std::string name = rest.substr(from, to - from);\n'
     '        from = to + 1;\n'
     '        if (name.empty() || name == ".")\n'
     '            continue;\n'
     '        std::string next = at + \'/\' + name;\n'
     '        if (!fs::exists(next, ec)) {\n'
     '            EmusuniFolderNames &folder = emusuni_folders[at];\n'
     '            const auto now = std::chrono::steady_clock::now();\n'
     '            if (folder.listed == std::chrono::steady_clock::time_point{} || now - folder.listed >= std::chrono::seconds(10)) {\n'
     '                folder.names.clear();\n'
     '                folder.listed = now;\n'
     '                for (fs::directory_iterator it(at, ec), end; !ec && it != end; it.increment(ec)) {\n'
     '                    const std::string real = it->path().filename().string();\n'
     '                    folder.names.emplace(string_utils::tolower(real), real);\n'
     '                }\n'
     '            }\n'
     '            const auto match = folder.names.find(string_utils::tolower(name));\n'
     '            if (match == folder.names.end())\n'
     '                return false;\n'
     '            next = at + \'/\' + match->second;\n'
     '        }\n'
     '        at = std::move(next);\n'
     '    }\n'
     '    found = std::move(at);\n'
     '    return true;\n'
     '}\n'
     '#endif\n'
     'namespace vfs {\n'),
    ('    io.cachemap.clear();\n',
     '#ifdef EMUSUNI_PORT\n'
     '    {\n'
     '        const std::lock_guard<std::mutex> emusuni_lock(emusuni_names_mutex);\n'
     '        emusuni_listed.clear();\n'
     '        emusuni_folders.clear();\n'
     '        io.cachemap.clear();\n'
     '    }\n'
     '#else\n'
     '    io.cachemap.clear();\n'
     '#endif\n'),
    ('fs::path find_in_cache(IOState &io, const std::string &system_path) {\n',
     'fs::path find_in_cache(IOState &io, const std::string &system_path) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    const std::lock_guard<std::mutex> emusuni_lock(emusuni_names_mutex);\n'
     '#endif\n'),
    ('    if (!fs::exists(final_path))\n        return false;\n\n'
     '    for (const auto &file : fs::recursive_directory_iterator(final_path)) {\n',
     '    if (!fs::exists(final_path))\n        return false;\n\n'
     '#ifdef EMUSUNI_PORT\n'
     '    const std::lock_guard<std::mutex> emusuni_lock(emusuni_names_mutex);\n'
     '    if (emusuni_vita_switch(5)) {\n'
     '        const std::string emusuni_whole = system_path.string();\n'
     '        std::string emusuni_found;\n'
     '        if (emusuni_whole.size() < final_path.size() || emusuni_whole.compare(0, final_path.size(), final_path) != 0\n'
     '            || !emusuni_find_below(final_path, emusuni_whole.substr(final_path.size()), emusuni_found))\n'
     '            return false;\n'
     '        io.cachemap.emplace(string_utils::tolower(emusuni_whole), emusuni_found);\n'
     '        return true;\n'
     '    }\n'
     '    {\n'
     '        const auto emusuni_now = std::chrono::steady_clock::now();\n'
     '        const auto emusuni_found = emusuni_listed.find(final_path);\n'
     '        if (emusuni_found != emusuni_listed.end() && emusuni_now - emusuni_found->second < std::chrono::seconds(10))\n'
     '            return true;\n'
     '        emusuni_listed[final_path] = emusuni_now;\n'
     '    }\n'
     '#endif\n'
     '    for (const auto &file : fs::recursive_directory_iterator(final_path)) {\n'),
    ('SceUID open_file(IOState &io, const char *path, const int flags, const fs::path &vita_fs_path, const char *export_name) {\n',
     'SceUID open_file(IOState &io, const char *path, const int flags, const fs::path &vita_fs_path, const char *export_name) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    const EmusuniVitaStatScope emusuni_stat_file(4);\n'
     '#endif\n'),
])
# A list of the game's drawing commands the renderer works on.
# (After 01.68) And how long after the game sent it the renderer's thread got to it ("vita: draws:" in the log).
# On the PC only, a test: EMUSUNI_VITA_TEST_LIST_MS holds that thread back before each list, as the PS4's slower
# CPU does - what a game does meanwhile to the data its draws read shows on the bench then.
patch('vita3k/renderer/src/batch.cpp', [
    ('namespace renderer {\n',
     STAT_API +
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '#include <chrono>\n'
     '#include <cstdlib>\n'
     '#include <thread>\n'
     '#endif\n'
     'namespace renderer {\n'),
    ('        process_batch(state, features, mem, config, *cmd_list);\n',
     '#ifdef EMUSUNI_PORT\n'
     '        if (cmd_list->emusuni_sent != 0)\n'
     '            emusuni_vita_stat_add(22, emusuni_vita_stat_now() - cmd_list->emusuni_sent);\n'
     '#if !defined(__PS4__)\n'
     '        {\n'
     '            static const int emusuni_wait_ms = [] {\n'
     '                const char *const text = getenv("EMUSUNI_VITA_TEST_LIST_MS");\n'
     '                return text ? atoi(text) : 0;\n'
     '            }();\n'
     '            if (emusuni_wait_ms > 0 && cmd_list->context)\n'
     '                std::this_thread::sleep_for(std::chrono::milliseconds(emusuni_wait_ms));\n'
     '        }\n'
     '#endif\n'
     '        const EmusuniVitaStatScope emusuni_stat_batch(5);\n'
     '#endif\n'
     '        process_batch(state, features, mem, config, *cmd_list);\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) The packet's handler; the frees kept back given at the end of each list.
    ('static void process_batch(renderer::State &state, const FeatureState &features, MemState &mem, Config &config, CommandList &command_list) {\n',
     '#ifdef EMUSUNI_PORT\n'
     'COMMAND(handle_emusuni_draw_packet); // (scene.cpp)\n'
     '#endif\n'
     'static void process_batch(renderer::State &state, const FeatureState &features, MemState &mem, Config &config, CommandList &command_list) {\n'),
    ('        { CommandOpcode::DestroyContext, cmd_handle_destroy_context }\n'
     '    };\n',
     '        { CommandOpcode::DestroyContext, cmd_handle_destroy_context }\n'
     '#ifdef EMUSUNI_PORT\n'
     '        , { CommandOpcode::EmusuniDrawPacket, cmd_handle_emusuni_draw_packet }\n'
     '#endif\n'
     '    };\n'),
    ('    } while (true);\n'
     '}\n',
     '    } while (true);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, "gxmpacket") the list\'s end: the frees kept back given now (SceGxm.cpp\'s free_new_command)\n'
     '    if (command_list.context && command_list.context->emusuni_flush_frees)\n'
     '        command_list.context->free_func(nullptr);\n'
     '#endif\n'
     '}\n'),
    # (20-11) the renderer's line, "vita: renderer:" (port/ps4_vita_stats.cpp)
    ('            CommandHelper helper(cmd);\n            handler->second(state, mem, config, helper, features, command_list.context);\n',
     '            CommandHelper helper(cmd);\n#ifdef EMUSUNI_PORT\n            // (20-11) each command\'s time by its kind, for the line "vita: renderer:" (41 draws and draw packets,\n            // 42 state, 43 scenes, 44 the others)\n            const CommandOpcode emusuni_opcode = cmd->opcode;\n            const unsigned long long emusuni_from = emusuni_vita_stat_now();\n#endif\n            handler->second(state, mem, config, helper, features, command_list.context);\n#ifdef EMUSUNI_PORT\n            {\n                int emusuni_kind = 44;\n                switch (emusuni_opcode) {\n                case CommandOpcode::Draw:\n                case CommandOpcode::EmusuniDrawPacket:\n                    emusuni_kind = 41;\n                    break;\n                case CommandOpcode::SetState:\n                    emusuni_kind = 42;\n                    break;\n                case CommandOpcode::SetContext:\n                case CommandOpcode::SyncSurfaceData:\n                case CommandOpcode::MidSceneFlush:\n                case CommandOpcode::NewFrame:\n                    emusuni_kind = 43;\n                    break;\n                default:\n                    break;\n                }\n                emusuni_vita_stat_add(emusuni_kind, emusuni_vita_stat_now() - emusuni_from);\n            }\n#endif\n'),
    # (20-11, for 01.84) the switches "texbatch" (57) and "vertexrange" (58), OFF
    ('    if (command_list.context)\n        state.context = command_list.context;\n',
     '    if (command_list.context)\n        state.context = command_list.context;\n#ifdef EMUSUNI_PORT\n    // (EmuSuNi, "texbatch", 57) a new list: its textures are hashed again, once (texture/cache.cpp)\n    emusuni_vita_texbatch_next();\n#endif\n'),
    ('static void process_batch(renderer::State &state, const FeatureState &features, MemState &mem, Config &config, CommandList &command_list) {\n',
     '#ifdef EMUSUNI_PORT\nextern "C" void emusuni_vita_texbatch_next(void);\n#endif\nstatic void process_batch(renderer::State &state, const FeatureState &features, MemState &mem, Config &config, CommandList &command_list) {\n'),
    # (20-11) a PC test: the scene commands' time by command and by part (EMUSUNI_VITA_TEST_SCENES)
    ('extern "C" void emusuni_vita_texbatch_next(void);\n',
     'extern "C" void emusuni_vita_texbatch_next(void);\n#if !defined(__PS4__)\nextern "C" void emusuni_vita_test_scene(int what, unsigned long long ticks);\n#endif\n'),
    ('                emusuni_vita_stat_add(emusuni_kind, emusuni_vita_stat_now() - emusuni_from);\n',
     "                emusuni_vita_stat_add(emusuni_kind, emusuni_vita_stat_now() - emusuni_from);\n#if !defined(__PS4__)\n                {\n                    // (20-11, a PC test: EMUSUNI_VITA_TEST_SCENES=1) each scene command's own time (context.cpp)\n                    const int emusuni_which = emusuni_opcode == CommandOpcode::SetContext ? 0\n                        : emusuni_opcode == CommandOpcode::SyncSurfaceData ? 1\n                        : emusuni_opcode == CommandOpcode::NewFrame ? 2\n                        : emusuni_opcode == CommandOpcode::MidSceneFlush ? 3 : -1;\n                    if (emusuni_which >= 0)\n                        emusuni_vita_test_scene(emusuni_which, emusuni_vita_stat_now() - emusuni_from);\n                }\n#endif\n"),
])
# (After 01.68) The "drawcopy" switch (20, OFF: a test): what a draw reads from the game's memory is copied when
# the game asks for the draw. Upstream hands the renderer's thread the ADDRESSES of a draw's uniform buffers,
# vertex streams and indices (with its "memory mapping" option off, as here: with it on the GPU reads the game's
# memory itself), and that thread reads them when it gets to the draw's command. A PS Vita's GPU has read a
# scene's data soon after the scene ends; a game that counts on that and writes its next pictures' data in the
# same buffers would get, with a renderer's thread that runs behind, a draw with the vertices of one picture
# and the uniforms or the indices of another.
# OFF from the factory: it was written for the console's Street Fighter X Tekken (menus full of lines) and
# LittleBigPlanet (letters broken), but on the PC bench neither shows that with the renderer's thread held back
# (EMUSUNI_VITA_TEST_LIST_MS=10: Street Fighter X Tekken's main menu drawn right with that thread 75 ms behind
# the game on average and up to 200, LittleBigPlanet's text with it 22 ms behind and up to 50). Kept for a test on
# the console (the empty file vita-drawcopy-on) and for a game that does write over its draws' data that soon.
# How: renderer::set_uniform_buffer, set_vertex_stream and draw - called by the game's thread, in sceGxmDraw and
# sceGxmDrawPrecomputed - each keep a copy (malloc) and put its address first in their command; the command's
# handler, on the renderer's thread, reads the copy instead and frees it (a vertex stream's goes to the stream's
# record until the draw that uses it). A command destroyed without being run frees its copy too
# (destroy_command_payload, which upstream has for the surfaces of a scene's start). Only for an immediate
# context's draws: a deferred context's commands are kept by the game in lists of its own.
patch('vita3k/renderer/include/renderer/commands.h', [
    ('    Context *context; ///< The HLE context that try to execute this buffer.\n};\n',
     '    Context *context; ///< The HLE context that try to execute this buffer.\n'
     '#ifdef EMUSUNI_PORT\n'
     '    unsigned long long emusuni_sent = 0; ///< (EmuSuNi) when the game sent the list: the processor\'s tick count\n'
     '#endif\n'
     '};\n'
     '#ifdef EMUSUNI_PORT\n'
     '} // namespace renderer\n'
     'struct MemState;\n'
     'namespace renderer {\n'
     '// (EmuSuNi, the "drawcopy" switch) While one of these lives - around a draw -, the calling thread\'s draw\n'
     '// commands keep copies of what they read from the game\'s memory (wanted: an immediate context\'s draw, and\n'
     '// the renderer reading that memory itself).\n'
     'struct EmusuniDrawCopy {\n'
     '    EmusuniDrawCopy(const MemState &mem, bool wanted);\n'
     '    ~EmusuniDrawCopy();\n'
     '    EmusuniDrawCopy(const EmusuniDrawCopy &) = delete;\n'
     '    EmusuniDrawCopy &operator=(const EmusuniDrawCopy &) = delete;\n'
     '};\n'
     '#endif\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) Its opcode, its records and the object that opens one.
    ('    DestroyRenderTarget,\n'
     '    DestroyContext\n'
     '};\n',
     '    DestroyRenderTarget,\n'
     '    DestroyContext\n'
     '#ifdef EMUSUNI_PORT\n'
     '    , EmusuniDrawPacket ///< (EmuSuNi, "gxmpacket") a draw\'s records in one packet: renderer.cpp\'s EmusuniPacket\n'
     '#endif\n'
     '};\n'),
    ('struct EmusuniDrawCopy {\n'
     '    EmusuniDrawCopy(const MemState &mem, bool wanted);\n'
     '    ~EmusuniDrawCopy();\n'
     '    EmusuniDrawCopy(const EmusuniDrawCopy &) = delete;\n'
     '    EmusuniDrawCopy &operator=(const EmusuniDrawCopy &) = delete;\n'
     '};\n',
     'struct EmusuniDrawCopy {\n'
     '    EmusuniDrawCopy(const MemState &mem, bool wanted);\n'
     '    ~EmusuniDrawCopy();\n'
     '    EmusuniDrawCopy(const EmusuniDrawCopy &) = delete;\n'
     '    EmusuniDrawCopy &operator=(const EmusuniDrawCopy &) = delete;\n'
     '};\n'
     '// (EmuSuNi, the "gxmpacket" switch, 50; port/ps4_vita_gxmpacket.cpp) While one of these is open - around a draw of an\n'
     '// immediate context -, the five calls the draw makes for that context (set_uniform_buffer, set_program, set_texture,\n'
     '// set_vertex_stream, draw) write records, in their order and with their arguments, into one packet instead of a\n'
     '// command each; closed, the packet is one command (EmusuniDrawPacket) that scene.cpp runs record by record with the\n'
     "// very handlers of the commands they stand for, the draw's highest index found there. Not open: the commands as ever.\n"
     'struct EmusuniPacket {\n'
     '    EmusuniPacket(Context *ctx, bool wanted, std::size_t uniform_buffers);\n'
     '    ~EmusuniPacket();\n'
     '    bool open() const;\n'
     '    EmusuniPacket(const EmusuniPacket &) = delete;\n'
     '    EmusuniPacket &operator=(const EmusuniPacket &) = delete;\n'
     '\n'
     'private:\n'
     '    bool is_open = false;\n'
     '};\n'
     "// A packet: 4 bytes its room in the ring (ps4_vita_gxmpacket.cpp's), 4 bytes the number of records, the records -\n"
     "// a uniform buffer's record followed by its copy when it carries one in the packet (flag 2: words[1] bytes).\n"
     'constexpr std::size_t EMUSUNI_PACKET_HEAD = 8;\n'
     'constexpr std::size_t EMUSUNI_PACKET_DATA = 4096; ///< room for uniform copies in a packet (more: a copy of its own)\n'
     'struct EmusuniPacketRecord {\n'
     '    std::uint8_t tag; ///< 1 a program, 2 a uniform buffer, 3 a texture, 4 a vertex stream, 5 the draw\n'
     '    std::uint8_t flag; ///< a program: 1 a fragment one; a uniform buffer: 1 a vertex one, 2 its copy follows, 4 its copy in words[2-3]\n'
     '    std::uint16_t index; ///< a uniform buffer: its block; a texture: its unit; a stream: its number\n'
     "    std::uint32_t address; ///< the game's address of the program, buffer, stream or indices\n"
     "    std::uint32_t words[4]; ///< a buffer: [0] its bytes, [1] its copy's; a stream: [0] its bytes; a texture: its 16 bytes; the draw: type, format, count, instances\n"
     '};\n'
     '#if !defined(__PS4__)\n'
     "// (a test, the PC only: EMUSUNI_VITA_TEST_DRAWCHECK=1) A print of the first bytes of a draw's data as the game asked for\n"
     "// the draw (a record's flag 8: a uniform buffer's in words[2], a stream's in words[1], the indices' in index), taken\n"
     '// again when the renderer runs it: which data the game had changed in between.\n'
     'inline std::uint32_t emusuni_check_print(const std::uint8_t *data, std::size_t size) {\n'
     '    std::uint32_t print = 2166136261u;\n'
     '    for (std::size_t i = 0; i < size && i < 64; i++)\n'
     '        print = (print ^ data[i]) * 16777619u;\n'
     '    return print;\n'
     '}\n'
     '#endif\n'),
])
# (After 01.68) The "freshshaders" switch (21, OFF: a test). When a game is closed the core writes the list of the
# shaders it made for it (hashs-vk.dat) and the GPU driver's own pipeline cache; at the game's next start the
# listed shaders are read from the disk, not made again, and the driver's cache is given back to the driver.
# The console goes that way from a game's second start on; what the disk holds may be of another version of the
# app (the list is only dropped when the shader generator's version or the GPU's features change). With the
# switch nothing saved is read: every shader is made anew and the driver compiles every pipeline, as in a first
# start - to see on the console whether a picture that comes out wrong comes out right then. (On the PC bench
# Street Fighter X Tekken's main menu and LittleBigPlanet's text come out the same from the cache and anew.)
patch('vita3k/renderer/src/shaders.cpp', [
    ('namespace renderer {\n', SWITCH_API + 'namespace renderer {\n'),
    ('bool get_shaders_cache_hashs(State &renderer) {\n',
     'bool get_shaders_cache_hashs(State &renderer) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(21)) {\n'
     '        renderer.shaders_cache_hashs.clear();\n'
     '        emusuni_vita_log("vita: shaders: the saved list of shaders and the GPU\'s saved pipeline cache are not read (the freshshaders switch is ON): all are made again");\n'
     '        return false;\n'
     '    }\n'
     '#endif\n'),
])
# (After 01.70) The "condrelease" switch (24, ON): a thread that waits for a condition gives up its mutex whole.
# A guess, not seen at work yet. On the console God of War Collection's launcher stops before its first picture
# (01.68's and 01.70's logs: its main thread in sceKernelWaitLwCond, a thread of FIOS - Sony's own file library -
# in sceKernelLockLwMutex, for good). One way to that: upstream's wait unlocks the condition's mutex once (its
# relock knows the case, "the owner's own relock"), so a thread that holds it twice goes on holding it while it
# waits, and whoever must take that mutex to signal the condition never can. With the switch the wait gives up
# every lock the thread has on the mutex and takes them all back after it. On the bench the launcher never
# stops - not with every read 3 ms longer either (EMUSUNI_VITA_TEST_READ_MS) - and no thread of its waits that
# way: the log's "vita: kernel:" line says so the first time one does, with the switch ON or OFF.
patch('vita3k/kernel/include/kernel/sync_primitives.h', [
    ('    SceInt32 release(MemState &mem, const ThreadStatePtr &thread, SceInt32 unlock_count);\n',
     '    SceInt32 release(MemState &mem, const ThreadStatePtr &thread, SceInt32 unlock_count);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, the "condrelease" switch) How many times thread holds the mutex now: 0 when it isn\'t its owner.\n'
     '    SceInt32 emusuni_held(const ThreadStatePtr &thread) const;\n'
     '#endif\n'),
    # (After 01.73, the "coreturns" switch made whole.) A lightweight mutex let go of is not handed to a waiter that
    # is tied to one of the Vita's cores: the waiter is woken to try again (Mutex::serve), and tries once it has its
    # core's turn (ThreadState::wait takes it back). On the Vita a lightweight mutex is the game's own word of
    # memory: the thread that lets go of it runs on and may take it again before the one it woke gets its core - and
    # Sony's file library counts on that too. Handed over at once, as upstream does, the thread that let go of it
    # found it another's: one launch in three of God of War Collection's stopped so on the bench with the turns ON
    # (the set 0173; each of the two threads holding one mutex once).
    ('            } while (!waiter.result && result && *result == SCE_KERNEL_OK);\n',
     '#ifdef EMUSUNI_PORT\n'
     '            } while (!waiter.result && !waiter.emusuni_retry && result && *result == SCE_KERNEL_OK);\n'
     '#else\n'
     '            } while (!waiter.result && result && *result == SCE_KERNEL_OK);\n'
     '#endif\n'),
    ('            if (result && *result == SCE_KERNEL_ERROR_WAIT_TIMEOUT)\n                timed_out(thread);\n',
     '#ifdef EMUSUNI_PORT\n'
     '            // (EmuSuNi, the "coreturns" switch) Woken to try again, nothing handed over: out of the queue and a\n'
     '            // new round. By now the thread has its core\'s turn back.\n'
     '            if (waiter.emusuni_retry) {\n'
     '                waiter.emusuni_retry = false;\n'
     '                if (result && *result == SCE_KERNEL_OK) {\n'
     '                    queue.remove(waiter);\n'
     '                    serve();\n'
     '                    continue;\n'
     '                }\n'
     '            }\n'
     '#endif\n'
     '            if (result && *result == SCE_KERNEL_ERROR_WAIT_TIMEOUT)\n                timed_out(thread);\n'),
])
patch('vita3k/kernel/include/kernel/thread/wait_queue.h', [
    ('        std::optional<SceInt32> result;\n',
     '        std::optional<SceInt32> result;\n'
     '#ifdef EMUSUNI_PORT\n'
     '        // (EmuSuNi, the "coreturns" switch) Woken to try again for what it waits for, not given it.\n'
     '        bool emusuni_retry = false;\n'
     '#endif\n'),
])
patch('vita3k/kernel/src/sync_primitives.cpp', [
    ('#include <algorithm>\n', '#include <algorithm>\n' + SWITCH_API + '#ifdef EMUSUNI_PORT\n#include <atomic>\n'
     'extern "C" void emusuni_vita_stat_wait_owner(int owner, int count);\n'
     'extern "C" int emusuni_vita_core_tied(int affinity_mask);\n'
     'extern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks);\n#endif\n'),
    ('    if (auto *waiter = waiters.front()) {\n        lock_count = waiter->entry.lock_count;\n        owner = waiter->thread->id;\n        waiters.wake(*waiter);\n',
     '    if (auto *waiter = waiters.front()) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        // (the "coreturns" switch, sync_primitives.h says why) Not handed to a waiter tied to one of the Vita\'s\n'
     '        // cores: woken to try again with its core\'s turn. The waiters behind it are served when it leaves the queue.\n'
     '        if (lightweight() && emusuni_vita_core_tied(static_cast<int>(waiter->thread->get_affinity_mask())) != 0) {\n'
     '            if (!waiter->emusuni_retry) {\n'
     '                waiter->emusuni_retry = true;\n'
     '                waiter->thread->wake();\n'
     '                emusuni_vita_stat_add(32, 0);\n'
     '            }\n'
     '            return;\n'
     '        }\n'
     '#endif\n'
     '        lock_count = waiter->entry.lock_count;\n        owner = waiter->thread->id;\n        waiters.wake(*waiter);\n'),
    # (The same day: whose a mutex is when a thread has to wait for it, for the log's "vita: waits:" lines. On the
    # bench the launcher never stops, even with every read 3 ms longer, and no thread of its ever waits for a
    # condition holding its mutex twice: what stops it on the console is still to be seen, and the log of 01.70
    # could not say which thread held the mutex "SceFiosIO" waited 48 s for.)
    ('        return try_take(mem, thread, count);\n\n    const WaitResult r = wait_loop(',
     '        return try_take(mem, thread, count);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (owner)\n'
     '        emusuni_vita_stat_wait_owner(owner, lock_count);\n'
     '#endif\n'
     '\n    const WaitResult r = wait_loop('),
    ('        return try_take(mem, thread, 1);\n\n    const WaitResult r = wait_loop(',
     '        return try_take(mem, thread, 1);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (owner)\n'
     '        emusuni_vita_stat_wait_owner(owner, lock_count);\n'
     '#endif\n'
     '\n    const WaitResult r = wait_loop('),
    ('SceInt32 Condvar::wait(MemState &mem, const ThreadStatePtr &thread, SceUInt32 *timeout, bool callbacks) {\n',
     '#ifdef EMUSUNI_PORT\n'
     'SceInt32 Mutex::emusuni_held(const ThreadStatePtr &thread) const {\n'
     '    const auto guard = lock();\n'
     '    return guard && owner == thread->id ? lock_count : 0;\n'
     '}\n'
     '#endif\n\n'
     'SceInt32 Condvar::wait(MemState &mem, const ThreadStatePtr &thread, SceUInt32 *timeout, bool callbacks) {\n'),
    ('    if (const SceInt32 error = associated_mutex->release(mem, thread, 1))\n        return error;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    const SceInt32 emusuni_held = associated_mutex->emusuni_held(thread);\n'
     '    const bool emusuni_all = emusuni_held > 1 && emusuni_vita_switch(24);\n'
     '    if (const SceInt32 error = associated_mutex->release(mem, thread, emusuni_all ? emusuni_held : 1))\n'
     '        return error;\n'
     '    if (emusuni_held > 1) {\n'
     '        static std::atomic<bool> emusuni_said{ false };\n'
     '        if (!emusuni_said.exchange(true))\n'
     '            emusuni_vita_log("vita: kernel: a thread waits for a condition holding its mutex %d times: %s (the condrelease switch)", static_cast<int>(emusuni_held),\n'
     '                emusuni_all ? "all given up for the wait, and taken back after it" : "one given up, the mutex stays the thread\'s while it waits");\n'
     '    }\n'
     '#else\n'
     '    if (const SceInt32 error = associated_mutex->release(mem, thread, 1))\n        return error;\n'
     '#endif\n'),
    ('    if (timed_out && result == SCE_KERNEL_OK)\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_all && result == SCE_KERNEL_OK)\n'
     '        associated_mutex->try_acquire(mem, thread, emusuni_held - 1);\n'
     '#endif\n'
     '    if (timed_out && result == SCE_KERNEL_OK)\n'),
])
# (After 01.70) Two tests, on the PC only, for the console's Street Fighter X Tekken with its fighters' skin black
# and Uncharted "dark, without the water's reflections" - the two games of the bench, with MotorStorm RC, whose
# shaders sample cube maps (Mortal Kombat's, LittleBigPlanet's and God of War's don't, and they have no such
# fault): what kind each texture of a draw is, for the list of a screen's draws (EMUSUNI_VITA_TEST_DRAWS); and
# EMUSUNI_VITA_TEST_CUBES=black (every face of a cube map uploaded as zeros) or =first (all faces but the first),
# to see whether the picture then is the console's.
patch('vita3k/renderer/src/vulkan/texture.cpp', [
    ('namespace renderer::vulkan {\n',
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '#include <cstdlib>\n'
     '#include <cstring>\n'
     '// (a draw\'s textures by unit, as they were last bound: type, width, height, mips)\n'
     'unsigned emusuni_test_textures[SCE_GXM_MAX_TEXTURE_UNITS][4];\n'
     '#endif\n'
     'namespace renderer::vulkan {\n'),
    ('        context.shader_hints.fragment_textures[index] = format;\n',
     '        context.shader_hints.fragment_textures[index] = format;\n'
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '        emusuni_test_textures[index][0] = static_cast<unsigned>(texture.texture_type());\n'
     '        emusuni_test_textures[index][1] = gxm::get_width(texture);\n'
     '        emusuni_test_textures[index][2] = gxm::get_height(texture);\n'
     '        emusuni_test_textures[index][3] = texture.true_mip_count();\n'
     '#endif\n'),
    ('    memcpy(static_cast<uint8_t *>(staging_buffer.buffer.mapped_data) + staging_buffer.used_so_far, text_data, upload_size);\n',
     '    memcpy(static_cast<uint8_t *>(staging_buffer.buffer.mapped_data) + staging_buffer.used_so_far, text_data, upload_size);\n'
     '#if defined(EMUSUNI_PORT) && !defined(__PS4__)\n'
     '    if (current_texture->is_cube) {\n'
     '        static const int emusuni_cubes = [] {\n'
     '            const char *const text = getenv("EMUSUNI_VITA_TEST_CUBES");\n'
     '            return !text ? 0 : (strcmp(text, "black") == 0 ? 1 : (strcmp(text, "first") == 0 ? 2 : 0));\n'
     '        }();\n'
     '        if (emusuni_cubes == 1 || (emusuni_cubes == 2 && face > 0))\n'
     '            memset(static_cast<uint8_t *>(staging_buffer.buffer.mapped_data) + staging_buffer.used_so_far, 0, upload_size);\n'
     '    }\n'
     '#endif\n'),
])
patch('vita3k/renderer/include/renderer/types.h', [
    ('    Ptr<const uint8_t> data = Ptr<const uint8_t>(0);\n    size_t size = 0;\n};\n',
     '    Ptr<const uint8_t> data = Ptr<const uint8_t>(0);\n    size_t size = 0;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    void *host = nullptr; ///< (EmuSuNi, "drawcopy") the stream as the game had it at its draw: the record\'s to free\n'
     '#endif\n'
     '};\n'),
    ('    CommandFreeFunc free_func;\n',
     '    CommandFreeFunc free_func;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, "drawcopy") What the command being run reads, as the game had it when it asked for the draw: a\n'
     '    // uniform buffer, a draw\'s indices. Null: the game\'s memory, as upstream.\n'
     '    const std::uint8_t *emusuni_uniform_host = nullptr;\n'
     '    const void *emusuni_index_host = nullptr;\n'
     '#endif\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) The renderer's context: the draw finds its highest index; frees given at each list's end.
    ('    const void *emusuni_index_host = nullptr;\n'
     '#endif\n',
     '    const void *emusuni_index_host = nullptr;\n'
     '    // (EmuSuNi, "gxmpacket") The draw being run came in a packet: its highest index (and so its vertex streams\'\n'
     "    // sizes) is found in vulkan::draw, from the indices it copies, not in the game's thread.\n"
     '    bool emusuni_late_max = false;\n'
     '    // (EmuSuNi, "gxmpacket") An immediate context: free_func(nullptr) at the end of each list it ran gives back the\n'
     "    // frees kept until then (SceGxm.cpp's free_new_command).\n"
     '    bool emusuni_flush_frees = false;\n'
     '    // (EmuSuNi, "uniformcopy") The copy, in a draw\'s packet, of the uniform buffer being set (not the renderer\'s to free).\n'
     '    const std::uint8_t *emusuni_uniform_borrowed = nullptr;\n'
     '#endif\n'),
])
patch('vita3k/renderer/src/renderer.cpp', [
    ('#include <util/log.h>\n\nnamespace renderer {\n',
     '#include <util/log.h>\n\n'
     '#ifdef EMUSUNI_PORT\n'
     '#include <cstdlib>\n'
     '#include <cstring>\n'
     '#include <mem/state.h>\n'
     'extern "C" int emusuni_vita_switch(int which);\n'
     'extern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks);\n'
     '#endif\n\n'
     'namespace renderer {\n'
     '#ifdef EMUSUNI_PORT\n'
     'static thread_local const MemState *emusuni_copy_mem = nullptr;\n'
     'EmusuniDrawCopy::EmusuniDrawCopy(const MemState &mem, bool wanted) {\n'
     '    emusuni_copy_mem = wanted && emusuni_vita_switch(20) ? &mem : nullptr;\n'
     '}\n'
     'EmusuniDrawCopy::~EmusuniDrawCopy() {\n'
     '    emusuni_copy_mem = nullptr;\n'
     '}\n'
     '// size bytes of the game\'s memory in a block of room bytes (the rest zeros), or null: no copy is wanted now.\n'
     '// what: 19 a uniform buffer, 20 a vertex stream, 21 a draw\'s indices (the log\'s counts).\n'
     'static void *emusuni_keep(Ptr<const void> data, std::size_t size, std::size_t room, int what) {\n'
     '    if (!emusuni_copy_mem || !data || size == 0)\n'
     '        return nullptr;\n'
     '    if (room < size)\n'
     '        room = size;\n'
     '    std::uint8_t *const copy = static_cast<std::uint8_t *>(std::malloc(room));\n'
     '    if (!copy)\n'
     '        return nullptr;\n'
     '    std::memcpy(copy, data.get(*emusuni_copy_mem), size);\n'
     '    if (room > size)\n'
     '        std::memset(copy + size, 0, room - size);\n'
     '    emusuni_vita_stat_add(what, size);\n'
     '    return copy;\n'
     '}\n'
     '#endif\n'),
    ('    renderer::add_state_set_command(ctx, renderer::GXMState::VertexStream, stream, index, data_len);\n',
     '#ifdef EMUSUNI_PORT\n'
     # (64 bytes of room after it: the stride's padding reads whole vertices, the last one past the stream's end.)
     '    renderer::add_state_set_command(ctx, renderer::GXMState::VertexStream, emusuni_keep(stream, data_len, data_len + 64, 20), stream,\n'
     '        static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(data_len));\n'
     '#else\n'
     '    renderer::add_state_set_command(ctx, renderer::GXMState::VertexStream, stream, index, data_len);\n'
     '#endif\n'),
    ('    renderer::add_command(ctx, renderer::CommandOpcode::Draw, nullptr, prim_type, index_type, index_data, index_count, instance_count);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    renderer::add_command(ctx, renderer::CommandOpcode::Draw, nullptr,\n'
     '        emusuni_keep(index_data, static_cast<std::size_t>(index_count) * (index_type == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4), 0, 21),\n'
     '        prim_type, index_type, index_data, index_count, instance_count);\n'
     '#else\n'
     '    renderer::add_command(ctx, renderer::CommandOpcode::Draw, nullptr, prim_type, index_type, index_data, index_count, instance_count);\n'
     '#endif\n'),
    ('    renderer::add_state_set_command(ctx, renderer::GXMState::UniformBuffer, buffer, is_vertex_uniform, block_number, bytes_to_copy_and_pad);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    renderer::add_state_set_command(ctx, renderer::GXMState::UniformBuffer, emusuni_keep(buffer, block_size, bytes_to_copy_and_pad, 19),\n'
     '        buffer, is_vertex_uniform, block_number, bytes_to_copy_and_pad);\n'
     '#else\n'
     '    renderer::add_state_set_command(ctx, renderer::GXMState::UniformBuffer, buffer, is_vertex_uniform, block_number, bytes_to_copy_and_pad);\n'
     '#endif\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) The packet opened around a draw, and the five calls that write into it.
    ('extern "C" int emusuni_vita_switch(int which);\n'
     'extern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks);\n'
     '#endif\n',
     'extern "C" int emusuni_vita_switch(int which);\n'
     'extern "C" void emusuni_vita_stat_add(int what, unsigned long long ticks);\n'
     'extern "C" void emusuni_vita_log(const char *format, ...);\n'
     'extern "C" void *emusuni_vita_packet_open(const void *ctx, std::size_t bytes);\n'
     'extern "C" void emusuni_vita_packet_close(void *packet, std::size_t used);\n'
     'extern "C" void emusuni_vita_packet_count(int how);\n'
     '#endif\n'),
    ('EmusuniDrawCopy::EmusuniDrawCopy(const MemState &mem, bool wanted) {\n'
     '    emusuni_copy_mem = wanted && emusuni_vita_switch(20) ? &mem : nullptr;\n'
     '}\n'
     'EmusuniDrawCopy::~EmusuniDrawCopy() {\n'
     '    emusuni_copy_mem = nullptr;\n'
     '}\n',
     '// (EmuSuNi, the "uniformcopy" switch, 54) While a draw of an immediate context is asked for: the game\'s memory its\n'
     '// default uniform buffer is copied from at once (null: not copied).\n'
     'static thread_local const MemState *emusuni_uniform_mem = nullptr;\n'
     '#if !defined(__PS4__)\n'
     "// (a test, the PC only: EMUSUNI_VITA_TEST_DRAWCHECK=1; commands.h's emusuni_check_print) the game's memory, while a\n"
     '// draw is asked for, to take the prints of its data from\n'
     'static thread_local const MemState *emusuni_check_mem = nullptr;\n'
     '#endif\n'
     'EmusuniDrawCopy::EmusuniDrawCopy(const MemState &mem, bool wanted) {\n'
     '    emusuni_copy_mem = wanted && emusuni_vita_switch(20) ? &mem : nullptr;\n'
     '    emusuni_uniform_mem = wanted && emusuni_vita_switch(54) ? &mem : nullptr;\n'
     '#if !defined(__PS4__)\n'
     '    static const bool emusuni_check = getenv("EMUSUNI_VITA_TEST_DRAWCHECK") != nullptr;\n'
     '    emusuni_check_mem = wanted && emusuni_check ? &mem : nullptr;\n'
     '#endif\n'
     '}\n'
     'EmusuniDrawCopy::~EmusuniDrawCopy() {\n'
     '    emusuni_copy_mem = nullptr;\n'
     '    emusuni_uniform_mem = nullptr;\n'
     '#if !defined(__PS4__)\n'
     '    emusuni_check_mem = nullptr;\n'
     '#endif\n'
     '}\n'),
    ('    emusuni_vita_stat_add(what, size);\n'
     '    return copy;\n'
     '}\n'
     '#endif\n',
     '    emusuni_vita_stat_add(what, size);\n'
     '    return copy;\n'
     '}\n'
     '\n'
     '// (EmuSuNi, the "uniformcopy" switch, 54) A draw\'s default uniform buffer (block 0: the one GXM\'s ring gives out and,\n'
     "// in Vita3K, takes back after a lap without waiting for the renderer - MotorStorm RC's shapes when the renderer is\n"
     '// a lap late) copied as the draw is asked for, in a block of its own (the renderer frees it).\n'
     'static void *emusuni_uniform_copy(Ptr<const void> buffer, std::size_t size, std::size_t room) {\n'
     '    void *const copy = std::malloc(room);\n'
     '    if (!copy)\n'
     '        return nullptr;\n'
     '    std::memcpy(copy, buffer.get(*emusuni_uniform_mem), size);\n'
     '    if (room > size)\n'
     '        std::memset(static_cast<std::uint8_t *>(copy) + size, 0, room - size);\n'
     '    emusuni_vita_stat_add(19, size);\n'
     '    return copy;\n'
     '}\n'
     "// A uniform buffer's copy for its command: drawcopy's, else uniformcopy's (the default buffer), else none.\n"
     'static void *emusuni_keep_uniform(Ptr<const void> buffer, std::size_t size, std::size_t room, int block) {\n'
     '    if (void *const kept = emusuni_keep(buffer, size, room, 19))\n'
     '        return kept;\n'
     '    if (block == SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX && emusuni_uniform_mem && buffer && size > 0)\n'
     '        return emusuni_uniform_copy(buffer, size, room);\n'
     '    return nullptr;\n'
     '}\n'
     '\n'
     '// (EmuSuNi, the "gxmpacket" switch, 50) The packet this thread is writing, if any (commands.h\'s EmusuniPacket).\n'
     'struct EmusuniOpenPacket {\n'
     '    Context *ctx = nullptr;\n'
     '    std::uint8_t *base = nullptr;\n'
     '    std::uint32_t records = 0;\n'
     '    std::uint32_t room = 0; // (records)\n'
     '    std::uint32_t cursor = 0; // (bytes written)\n'
     '    std::uint32_t data = 0; // (bytes of uniform copies written, EMUSUNI_PACKET_DATA at most)\n'
     '};\n'
     'static thread_local EmusuniOpenPacket emusuni_open;\n'
     'static_assert(sizeof(EmusuniPacketRecord) == 24 && sizeof(SceGxmTexture) == 16);\n'
     '\n'
     '// The records written so far as one command, and the packet closed.\n'
     'static void emusuni_packet_send() {\n'
     '    const EmusuniOpenPacket done = emusuni_open;\n'
     '    emusuni_open = EmusuniOpenPacket{};\n'
     '    if (!done.base)\n'
     '        return;\n'
     '    if (done.records == 0) {\n'
     '        emusuni_vita_packet_close(done.base, 0);\n'
     '        return;\n'
     '    }\n'
     '    std::memcpy(done.base + 4, &done.records, 4);\n'
     '    emusuni_vita_packet_close(done.base, done.cursor);\n'
     '    emusuni_vita_packet_count(0);\n'
     '    const void *const packet = done.base;\n'
     '    renderer::add_command(done.ctx, renderer::CommandOpcode::EmusuniDrawPacket, nullptr, packet);\n'
     '}\n'
     '\n'
     'EmusuniPacket::EmusuniPacket(Context *ctx, bool wanted, std::size_t uniform_buffers) {\n'
     '    if (!wanted || !ctx || emusuni_open.base || !emusuni_vita_switch(50) || emusuni_vita_switch(20))\n'
     '        return;\n'
     '    // every record the draw can make: its uniform buffers, two programs, a texture a unit, a stream each, the draw\n'
     '    const std::size_t room = uniform_buffers + 2 + 2 * SCE_GXM_MAX_TEXTURE_UNITS + SCE_GXM_MAX_VERTEX_STREAMS + 1;\n'
     '    if (room > 256) {\n'
     '        emusuni_vita_packet_count(2);\n'
     '        return;\n'
     '    }\n'
     '    void *const at = emusuni_vita_packet_open(ctx, EMUSUNI_PACKET_HEAD + room * sizeof(EmusuniPacketRecord) + EMUSUNI_PACKET_DATA);\n'
     '    if (!at) {\n'
     '        emusuni_vita_packet_count(1);\n'
     '        return;\n'
     '    }\n'
     '    emusuni_open.ctx = ctx;\n'
     '    emusuni_open.base = static_cast<std::uint8_t *>(at);\n'
     '    emusuni_open.records = 0;\n'
     '    emusuni_open.room = static_cast<std::uint32_t>(room);\n'
     '    emusuni_open.cursor = EMUSUNI_PACKET_HEAD;\n'
     '    emusuni_open.data = 0;\n'
     '    is_open = true;\n'
     '}\n'
     '\n'
     'EmusuniPacket::~EmusuniPacket() {\n'
     '    if (is_open)\n'
     '        emusuni_packet_send();\n'
     '}\n'
     '\n'
     'bool EmusuniPacket::open() const {\n'
     '    return is_open && emusuni_open.base != nullptr;\n'
     '}\n'
     '\n'
     "// A record for this context's open packet, or null: no packet open for it (the command as ever).\n"
     'static EmusuniPacketRecord *emusuni_record(Context *ctx, std::uint8_t tag) {\n'
     '    if (!ctx || !emusuni_open.base || emusuni_open.ctx != ctx)\n'
     '        return nullptr;\n'
     '    if (emusuni_open.records >= emusuni_open.room) {\n'
     '        // (Cannot be: the room counts every record a draw can make.) What is written goes now, the rest as ever.\n'
     '        static bool said = false;\n'
     '        if (!said) {\n'
     '            said = true;\n'
     '            emusuni_vita_log("vita: gxmpacket: a draw made more records than its packet had room for (%u)", emusuni_open.room);\n'
     '        }\n'
     '        emusuni_packet_send();\n'
     '        return nullptr;\n'
     '    }\n'
     '    EmusuniPacketRecord *const record = reinterpret_cast<EmusuniPacketRecord *>(emusuni_open.base + emusuni_open.cursor);\n'
     '    emusuni_open.cursor += sizeof(EmusuniPacketRecord);\n'
     '    emusuni_open.records++;\n'
     '    std::memset(record, 0, sizeof(*record));\n'
     '    record->tag = tag;\n'
     '    return record;\n'
     '}\n'
     '#endif\n'),
    ('void set_program(State &state, Context *ctx, Ptr<const void> program, const bool is_fragment) {\n'
     '    renderer::add_state_set_command(ctx, renderer::GXMState::Program, program, is_fragment);\n',
     'void set_program(State &state, Context *ctx, Ptr<const void> program, const bool is_fragment) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (EmusuniPacketRecord *const record = emusuni_record(ctx, 1)) {\n'
     '        record->flag = is_fragment ? 1 : 0;\n'
     '        record->address = program.address();\n'
     '        return;\n'
     '    }\n'
     '#endif\n'
     '    renderer::add_state_set_command(ctx, renderer::GXMState::Program, program, is_fragment);\n'),
    ('void set_texture(State &state, Context *ctx, const std::uint32_t tex_index, const SceGxmTexture tex) {\n'
     '    renderer::add_state_set_command(ctx, renderer::GXMState::Texture, tex_index, tex);\n',
     'void set_texture(State &state, Context *ctx, const std::uint32_t tex_index, const SceGxmTexture tex) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (EmusuniPacketRecord *const record = emusuni_record(ctx, 3)) {\n'
     '        record->index = static_cast<std::uint16_t>(tex_index);\n'
     '        std::memcpy(record->words, &tex, sizeof(tex));\n'
     '        return;\n'
     '    }\n'
     '#endif\n'
     '    renderer::add_state_set_command(ctx, renderer::GXMState::Texture, tex_index, tex);\n'),
    ('void set_vertex_stream(State &state, Context *ctx, const std::size_t index, const std::size_t data_len, const Ptr<const void> stream) {\n'
     '#ifdef EMUSUNI_PORT\n',
     'void set_vertex_stream(State &state, Context *ctx, const std::size_t index, const std::size_t data_len, const Ptr<const void> stream) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (EmusuniPacketRecord *const record = emusuni_record(ctx, 4)) {\n'
     '        record->index = static_cast<std::uint16_t>(index);\n'
     '        record->address = stream.address();\n'
     '        record->words[0] = static_cast<std::uint32_t>(data_len);\n'
     '#if !defined(__PS4__)\n'
     '        if (emusuni_check_mem && stream && data_len > 0) {\n'
     '            record->flag |= 8;\n'
     '            record->words[1] = emusuni_check_print(static_cast<const std::uint8_t *>(stream.get(*emusuni_check_mem)), data_len);\n'
     '        }\n'
     '#endif\n'
     '        return;\n'
     '    }\n'),
    ('void draw(State &state, Context *ctx, SceGxmPrimitiveType prim_type, SceGxmIndexFormat index_type, Ptr<const void> index_data, const std::uint32_t index_count, const std::uint32_t instance_count) {\n'
     '#ifdef EMUSUNI_PORT\n',
     'void draw(State &state, Context *ctx, SceGxmPrimitiveType prim_type, SceGxmIndexFormat index_type, Ptr<const void> index_data, const std::uint32_t index_count, const std::uint32_t instance_count) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (EmusuniPacketRecord *const record = emusuni_record(ctx, 5)) {\n'
     '        record->address = index_data.address();\n'
     '        record->words[0] = static_cast<std::uint32_t>(prim_type);\n'
     '        record->words[1] = static_cast<std::uint32_t>(index_type);\n'
     '        record->words[2] = index_count;\n'
     '        record->words[3] = instance_count;\n'
     '#if !defined(__PS4__)\n'
     '        if (emusuni_check_mem && index_data && index_count > 0) {\n'
     '            record->flag |= 8;\n'
     '            record->index = static_cast<std::uint16_t>(emusuni_check_print(static_cast<const std::uint8_t *>(index_data.get(*emusuni_check_mem)),\n'
     '                static_cast<std::size_t>(index_count) * (index_type == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4)));\n'
     '        }\n'
     '#endif\n'
     '        return;\n'
     '    }\n'),
    ('    std::uint32_t bytes_to_copy_and_pad = ((block_size + 15) / 16) * 16;\n'
     '\n'
     '#ifdef EMUSUNI_PORT\n',
     '    std::uint32_t bytes_to_copy_and_pad = ((block_size + 15) / 16) * 16;\n'
     '\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (EmusuniPacketRecord *const record = emusuni_record(ctx, 2)) {\n'
     '        record->flag = is_vertex_uniform ? 1 : 0;\n'
     '        record->index = static_cast<std::uint16_t>(block_number);\n'
     '        record->address = buffer.address();\n'
     '        record->words[0] = bytes_to_copy_and_pad;\n'
     '        // ("uniformcopy") the default buffer copied now: right after its record when the packet has room, else alone\n'
     '        if (block_number == SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX && emusuni_uniform_mem && buffer && block_size > 0) {\n'
     '            if (emusuni_open.data + bytes_to_copy_and_pad <= EMUSUNI_PACKET_DATA) {\n'
     '                std::uint8_t *const copy = emusuni_open.base + emusuni_open.cursor;\n'
     '                std::memcpy(copy, buffer.get(*emusuni_uniform_mem), block_size);\n'
     '                if (bytes_to_copy_and_pad > block_size)\n'
     '                    std::memset(copy + block_size, 0, bytes_to_copy_and_pad - block_size);\n'
     '                emusuni_open.cursor += bytes_to_copy_and_pad;\n'
     '                emusuni_open.data += bytes_to_copy_and_pad;\n'
     '                record->flag |= 2;\n'
     '                record->words[1] = bytes_to_copy_and_pad;\n'
     '                emusuni_vita_stat_add(19, block_size);\n'
     '            } else if (void *const own = emusuni_uniform_copy(buffer, block_size, bytes_to_copy_and_pad)) {\n'
     '                record->flag |= 4;\n'
     '                std::memcpy(&record->words[2], &own, sizeof(own));\n'
     '            }\n'
     '        }\n'
     '#if !defined(__PS4__)\n'
     '        if (emusuni_check_mem && buffer && block_size > 0 && !(record->flag & 6)) {\n'
     '            record->flag |= 8;\n'
     '            record->words[2] = emusuni_check_print(static_cast<const std::uint8_t *>(buffer.get(*emusuni_check_mem)), bytes_to_copy_and_pad);\n'
     '        }\n'
     '#endif\n'
     '        return;\n'
     '    }\n'),
    ('    renderer::add_state_set_command(ctx, renderer::GXMState::UniformBuffer, emusuni_keep(buffer, block_size, bytes_to_copy_and_pad, 19),\n',
     '    renderer::add_state_set_command(ctx, renderer::GXMState::UniformBuffer, emusuni_keep_uniform(buffer, block_size, bytes_to_copy_and_pad, block_number),\n'),
    # (19-11) A test, the PC only: EMUSUNI_VITA_TEST_DRAWCOPY_KINDS - "drawcopy" for the kinds named only.
    ('static void *emusuni_keep(Ptr<const void> data, std::size_t size, std::size_t room, int what) {\n'
     '    if (!emusuni_copy_mem || !data || size == 0)\n'
     '        return nullptr;\n',
     'static void *emusuni_keep(Ptr<const void> data, std::size_t size, std::size_t room, int what) {\n'
     '    if (!emusuni_copy_mem || !data || size == 0)\n'
     '        return nullptr;\n'
     '#if !defined(__PS4__)\n'
     '    // (a test, the PC only) EMUSUNI_VITA_TEST_DRAWCOPY_KINDS=19,20,21: only the kinds named are copied\n'
     '    static const char *const emusuni_kinds = getenv("EMUSUNI_VITA_TEST_DRAWCOPY_KINDS");\n'
     '    if (emusuni_kinds) {\n'
     "        const char emusuni_kind[3] = { static_cast<char>('0' + what / 10 % 10), static_cast<char>('0' + what % 10), 0 };\n"
     '        if (!strstr(emusuni_kinds, emusuni_kind))\n'
     '            return nullptr;\n'
     '    }\n'
     '#endif\n'),
])
patch('vita3k/renderer/src/state_set.cpp', [
    ('COMMAND_SET_STATE(uniform_buffer) {\n    TRACY_FUNC_COMMANDS_SET_STATE(uniform_buffer);\n',
     'COMMAND_SET_STATE(uniform_buffer) {\n    TRACY_FUNC_COMMANDS_SET_STATE(uniform_buffer);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    void *const emusuni_host = helper.pop<void *>();\n'
     '    render_context->emusuni_uniform_host = static_cast<const std::uint8_t *>(emusuni_host);\n'
     '#endif\n'),
    ('    const auto offset = program->uniform_buffer_data_offsets.at(block_num);\n'
     '    if (offset != static_cast<uint32_t>(-1) && config.log_active_shaders) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '    render_context->emusuni_uniform_host = nullptr;\n'
     '    free(emusuni_host);\n'
     '    *reinterpret_cast<void **>(&helper.cmd->data[sizeof(GXMState)]) = nullptr;\n'
     '#endif\n'
     '    const auto offset = program->uniform_buffer_data_offsets.at(block_num);\n'
     '    if (offset != static_cast<uint32_t>(-1) && config.log_active_shaders) {\n'),
    ('    const Ptr<const uint8_t> stream_data = helper.pop<Ptr<const uint8_t>>();\n'
     '    const std::size_t stream_index = helper.pop<std::size_t>();\n'
     '    const std::size_t stream_data_length = helper.pop<std::size_t>();\n'
     '\n'
     '    renderer::GXMStreamInfo &info = render_context->record.vertex_streams[stream_index];\n'
     '    info.data = stream_data;\n'
     '    info.size = stream_data_length;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    void *const emusuni_host = helper.pop<void *>();\n'
     '    *reinterpret_cast<void **>(&helper.cmd->data[sizeof(GXMState)]) = nullptr; // (the record\'s from here on)\n'
     '    const Ptr<const uint8_t> stream_data = helper.pop<Ptr<const uint8_t>>();\n'
     '    const std::size_t stream_index = helper.pop<std::uint32_t>();\n'
     '    const std::size_t stream_data_length = helper.pop<std::uint32_t>();\n'
     '#else\n'
     '    const Ptr<const uint8_t> stream_data = helper.pop<Ptr<const uint8_t>>();\n'
     '    const std::size_t stream_index = helper.pop<std::size_t>();\n'
     '    const std::size_t stream_data_length = helper.pop<std::size_t>();\n'
     '#endif\n'
     '\n'
     '    renderer::GXMStreamInfo &info = render_context->record.vertex_streams[stream_index];\n'
     '    info.data = stream_data;\n'
     '    info.size = stream_data_length;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    free(info.host); // (one set and not drawn with)\n'
     '    info.host = emusuni_host;\n'
     '#endif\n'),
    ('namespace renderer {\n', '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#endif\nnamespace renderer {\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) "uniformcopy": the copy a draw's packet carries.
    ('    render_context->emusuni_uniform_host = static_cast<const std::uint8_t *>(emusuni_host);\n',
     '    // ("uniformcopy", 54) the copy a draw\'s packet lends when the command has none of its own\n'
     '    render_context->emusuni_uniform_host = emusuni_host ? static_cast<const std::uint8_t *>(emusuni_host) : render_context->emusuni_uniform_borrowed;\n'),
])
patch('vita3k/renderer/src/scene.cpp', [
    ('    default:\n        break;\n    }\n}\n\nCOMMAND(handle_set_context) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, the "drawcopy" switch) The copy a draw\'s command keeps of what it reads from the game\'s memory:\n'
     '    // its address is the first thing in the command\'s data (after the state\'s number, in a state\'s command).\n'
     '    case CommandOpcode::Draw: {\n'
     '        void **const kept = reinterpret_cast<void **>(&cmd.data[0]);\n'
     '        free(*kept);\n'
     '        *kept = nullptr;\n'
     '        break;\n'
     '    }\n'
     '    case CommandOpcode::SetState: {\n'
     '        const GXMState which = *reinterpret_cast<const GXMState *>(&cmd.data[0]);\n'
     '        if (which == GXMState::UniformBuffer || which == GXMState::VertexStream) {\n'
     '            void **const kept = reinterpret_cast<void **>(&cmd.data[sizeof(GXMState)]);\n'
     '            free(*kept);\n'
     '            *kept = nullptr;\n'
     '        }\n'
     '        break;\n'
     '    }\n'
     '#endif\n'
     '    default:\n        break;\n    }\n}\n\nCOMMAND(handle_set_context) {\n'),
    ('COMMAND(handle_draw) {\n    TRACY_FUNC_COMMANDS(handle_draw);\n',
     'COMMAND(handle_draw) {\n    TRACY_FUNC_COMMANDS(handle_draw);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    render_context->emusuni_index_host = helper.pop<void *>();\n'
     '#endif\n'),
    ('        vulkan::draw(*reinterpret_cast<vulkan::VKContext *>(render_context), type, format, indices.cast<void>(),\n'
     '            count, instance_count, mem, config);\n'
     '        break;\n'
     '\n'
     '    default:\n'
     '        REPORT_MISSING(renderer.current_backend);\n'
     '        break;\n'
     '    }\n',
     '        vulkan::draw(*reinterpret_cast<vulkan::VKContext *>(render_context), type, format, indices.cast<void>(),\n'
     '            count, instance_count, mem, config);\n'
     '        break;\n'
     '\n'
     '    default:\n'
     '        REPORT_MISSING(renderer.current_backend);\n'
     '        break;\n'
     '    }\n'
     '#ifdef EMUSUNI_PORT\n'
     '    render_context->emusuni_index_host = nullptr;\n'
     '    destroy_command_payload(*helper.cmd);\n'
     '#endif\n'),
    ('namespace renderer {\nvoid destroy_command_payload(Command &cmd) {\n',
     '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#endif\nnamespace renderer {\nvoid destroy_command_payload(Command &cmd) {\n'),
    # (19-11, the switch "gxmpacket", 50, port/ps4_vita_gxmpacket.cpp: a draw's commands as one packet) The packet's handler, and its bytes given back when a command is thrown away.
    ('#ifdef EMUSUNI_PORT\n'
     '#include <cstdlib>\n'
     '#endif\n'
     'namespace renderer {\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <cstdlib>\n'
     '#include <atomic>\n'
     '#include <cstring>\n'
     'extern "C" void emusuni_vita_packet_free(const void *packet);\n'
     'extern "C" void emusuni_vita_log(const char *format, ...);\n'
     '#endif\n'
     'namespace renderer {\n'),
    ('#endif\n'
     '    default:\n'
     '        break;\n'
     '    }\n'
     '}\n',
     '    // (EmuSuNi, "gxmpacket") a draw\'s packet not run: the uniform copies of their own freed, its bytes given back\n'
     '    case CommandOpcode::EmusuniDrawPacket: {\n'
     '        void **const packet = reinterpret_cast<void **>(&cmd.data[0]);\n'
     '        if (*packet) {\n'
     '            const std::uint8_t *const bytes = static_cast<const std::uint8_t *>(*packet);\n'
     '            std::uint32_t records = 0;\n'
     '            std::memcpy(&records, bytes + 4, 4);\n'
     '            const std::uint8_t *at = bytes + EMUSUNI_PACKET_HEAD;\n'
     '            for (std::uint32_t i = 0; i < records; i++) {\n'
     '                const EmusuniPacketRecord &record = *reinterpret_cast<const EmusuniPacketRecord *>(at);\n'
     '                at += sizeof(EmusuniPacketRecord);\n'
     '                if (record.tag == 2 && (record.flag & 4)) {\n'
     '                    void *own = nullptr;\n'
     '                    std::memcpy(&own, &record.words[2], sizeof(own));\n'
     '                    free(own);\n'
     '                }\n'
     '                if (record.tag == 2 && (record.flag & 2))\n'
     '                    at += record.words[1];\n'
     '            }\n'
     '            emusuni_vita_packet_free(*packet);\n'
     '        }\n'
     '        *packet = nullptr;\n'
     '        break;\n'
     '    }\n'
     '#endif\n'
     '    default:\n'
     '        break;\n'
     '    }\n'
     '}\n'),
    ('#ifdef EMUSUNI_PORT\n'
     '    render_context->emusuni_index_host = nullptr;\n'
     '    destroy_command_payload(*helper.cmd);\n'
     '#endif\n'
     '}\n'
     '} // namespace renderer\n',
     '#ifdef EMUSUNI_PORT\n'
     '    render_context->emusuni_index_host = nullptr;\n'
     '    destroy_command_payload(*helper.cmd);\n'
     '#endif\n'
     '}\n'
     '\n'
     '#ifdef EMUSUNI_PORT\n'
     '// (EmuSuNi, the "gxmpacket" switch, 50) A draw\'s packet (renderer.cpp\'s EmusuniPacket): each record run by the very\n'
     '// handler of the command it stands for, given the same arguments in the same order (a command made here on the\n'
     "// stack), the draw with its highest index found in vulkan::draw; then the packet's bytes given back.\n"
     "COMMAND_SET_STATE(program); // (state_set.cpp's handlers)\n"
     'COMMAND_SET_STATE(uniform_buffer);\n'
     'COMMAND_SET_STATE(texture);\n'
     'COMMAND_SET_STATE(vertex_stream);\n'
     '#if !defined(__PS4__)\n'
     "// (a test, the PC only: EMUSUNI_VITA_TEST_DRAWCHECK=1) A record's data printed again now that the renderer runs it, and\n"
     '// counted by kind when it is no longer what the game had as it asked for the draw.\n'
     'static std::atomic<unsigned long long> emusuni_checked[4], emusuni_changed[4];\n'
     'static void emusuni_check(MemState &mem, const EmusuniPacketRecord &record) {\n'
     '    int kind = 0;\n'
     '    std::uint32_t was = 0;\n'
     '    std::size_t size = 0;\n'
     '    switch (record.tag) {\n'
     '    case 2: kind = record.index == 0 ? 0 : 1; was = record.words[2]; size = record.words[0]; break;\n'
     '    case 4: kind = 2; was = record.words[1]; size = record.words[0]; break;\n'
     '    case 5: kind = 3; was = record.index; size = static_cast<std::size_t>(record.words[2]) * (record.words[1] == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4); break;\n'
     '    default: return;\n'
     '    }\n'
     '    const std::uint32_t now = emusuni_check_print(static_cast<const std::uint8_t *>(Ptr<const void>(record.address).get(mem)), size);\n'
     '    emusuni_checked[kind]++;\n'
     '    if (record.tag == 5 ? (now & 0xFFFF) != was : now != was)\n'
     '        emusuni_changed[kind]++;\n'
     '    static std::atomic<unsigned long long> items{0};\n'
     '    if ((++items & 0x3FFFF) == 0)\n'
     '        emusuni_vita_log("vita: drawcheck: changed before the renderer read them: default uniform buffers %llu of %llu, other uniform "\n'
     '                         "buffers %llu of %llu, vertex streams %llu of %llu, indices %llu of %llu",\n'
     '            emusuni_changed[0].load(), emusuni_checked[0].load(), emusuni_changed[1].load(), emusuni_checked[1].load(),\n'
     '            emusuni_changed[2].load(), emusuni_checked[2].load(), emusuni_changed[3].load(), emusuni_checked[3].load());\n'
     '}\n'
     '#endif\n'
     'COMMAND(handle_emusuni_draw_packet) {\n'
     '    void **const kept = reinterpret_cast<void **>(&helper.cmd->data[0]);\n'
     '    const std::uint8_t *const packet = static_cast<const std::uint8_t *>(*kept);\n'
     '    if (!packet)\n'
     '        return;\n'
     '    std::uint32_t records = 0;\n'
     '    std::memcpy(&records, packet + 4, 4);\n'
     '    const std::uint8_t *at = packet + EMUSUNI_PACKET_HEAD;\n'
     '    for (std::uint32_t i = 0; i < records; i++) {\n'
     '        const EmusuniPacketRecord &record = *reinterpret_cast<const EmusuniPacketRecord *>(at);\n'
     '        at += sizeof(EmusuniPacketRecord);\n'
     '#if !defined(__PS4__)\n'
     '        if (record.flag & 8)\n'
     '            emusuni_check(mem, record);\n'
     '#endif\n'
     '        Command one;\n'
     '        one.opcode = CommandOpcode::SetState;\n'
     '        one.status = nullptr;\n'
     '        CommandHelper sub(&one);\n'
     '        const Ptr<const void> address(record.address);\n'
     '        void *const none = nullptr;\n'
     '        switch (record.tag) {\n'
     '        case 1:\n'
     '            do_command_push_data(sub, GXMState::Program, address, record.flag != 0);\n'
     '            sub.point = 0;\n'
     '            sub.pop<GXMState>();\n'
     '            cmd_set_state_program(renderer, mem, config, sub, render_context);\n'
     '            break;\n'
     '        case 2: {\n'
     '            // ("uniformcopy") its copy: in the packet right after the record (lent: the handler takes it as the\n'
     "            // command's own copy but doesn't free it), or a block of its own (the handler frees it)\n"
     '            void *own = nullptr;\n'
     '            if (record.flag & 4)\n'
     '                std::memcpy(&own, &record.words[2], sizeof(own));\n'
     '            if (record.flag & 2) {\n'
     '                render_context->emusuni_uniform_borrowed = at;\n'
     '                at += record.words[1];\n'
     '            }\n'
     '            do_command_push_data(sub, GXMState::UniformBuffer, own, address, (record.flag & 1) != 0, static_cast<int>(record.index), record.words[0]);\n'
     '            sub.point = 0;\n'
     '            sub.pop<GXMState>();\n'
     '            cmd_set_state_uniform_buffer(renderer, mem, config, sub, render_context);\n'
     '            render_context->emusuni_uniform_borrowed = nullptr;\n'
     '            break;\n'
     '        }\n'
     '        case 3: {\n'
     '            SceGxmTexture texture;\n'
     '            std::memcpy(&texture, record.words, sizeof(texture));\n'
     '            do_command_push_data(sub, GXMState::Texture, static_cast<std::uint32_t>(record.index), texture);\n'
     '            sub.point = 0;\n'
     '            sub.pop<GXMState>();\n'
     '            cmd_set_state_texture(renderer, mem, config, sub, render_context);\n'
     '            break;\n'
     '        }\n'
     '        case 4:\n'
     '            do_command_push_data(sub, GXMState::VertexStream, none, address, static_cast<std::uint32_t>(record.index), record.words[0]);\n'
     '            sub.point = 0;\n'
     '            sub.pop<GXMState>();\n'
     '            cmd_set_state_vertex_stream(renderer, mem, config, sub, render_context);\n'
     '            break;\n'
     '        case 5:\n'
     '            one.opcode = CommandOpcode::Draw;\n'
     '            do_command_push_data(sub, none, static_cast<SceGxmPrimitiveType>(record.words[0]), static_cast<SceGxmIndexFormat>(record.words[1]),\n'
     '                address, record.words[2], record.words[3]);\n'
     '            sub.point = 0;\n'
     '            render_context->emusuni_late_max = true;\n'
     '            cmd_handle_draw(renderer, mem, config, sub, features, render_context);\n'
     '            render_context->emusuni_late_max = false;\n'
     '            break;\n'
     '        default:\n'
     '            break;\n'
     '        }\n'
     '    }\n'
     '    emusuni_vita_packet_free(packet);\n'
     '    *kept = nullptr;\n'
     '}\n'
     '#endif\n'
     '} // namespace renderer\n'),
])
# (01.31) The game's licence is not a thing for the log (01.30's had it whole, as text, at each start: a log that
# a player sends would carry it), nor the 380 lines of a package's checked pages.
patch('external/psvpfstools/psvpfsparser/rif2zrif.cpp', [
    ('        std::cout << "rif2zrif sanity check: " << key << std::endl;\n',
     '#ifndef EMUSUNI_PORT\n'
     '        std::cout << "rif2zrif sanity check: " << key << std::endl;\n'
     '#endif\n'),
], repo='external/psvpfstools/psvpfsparser')
patch('external/psvpfstools/psvpfsparser/HashTree.cpp', [
    ('      std::cout << std::string(level, \'.\') << it->second.page;\n',
     '#ifndef EMUSUNI_PORT\n'
     '      std::cout << std::string(level, \'.\') << it->second.page;\n'
     '#endif\n'),
    ('            std::cout << " - OK : ";\n\n            print_bytes(it->second.icv, 0x14);\n',
     '#ifndef EMUSUNI_PORT\n'
     '            std::cout << " - OK : ";\n\n            print_bytes(it->second.icv, 0x14);\n'
     '#endif\n'),
    ('         std::cout << " - Hash does not match" << std::endl;\n',
     '#ifdef EMUSUNI_PORT\n'
     '         std::cout << "Page " << it->second.page << " of the package\'s hash tree";\n'
     '#endif\n'
     '         std::cout << " - Hash does not match" << std::endl;\n'),
], repo='external/psvpfstools/psvpfsparser')

# The standalone emulator's paths (never run in the core: it returns before; it must compile): the
# branch of Linux for a system that is none of those it names.
patch('vita3k/app/src/app_init.cpp', [
    ('#elif defined(__linux__)\n    fs::path portable_path = "";\n',
     '#elif defined(__linux__) || defined(EMUSUNI_PORT)\n    fs::path portable_path = "";\n'),
])

# The Vita's network: no HTTP client of the emulator's own (curl). The Vita's TLS is off in
# port/vita-openssl/openssl/ssl.h. Its sockets (SceNet, SceHttp, ad hoc play) are the system's upstream: here
# nothing of theirs leaves the console, comes in or asks a name server (port/ps4_vita_nonet.c, linked over those
# calls at the end of this script) - a jailbroken PS4 must never reach Sony's servers, and a game would.
# (The one place that didn't expect an address without a name.)
patch('vita3k/modules/SceNet/SceNet.cpp', [
    ('    struct hostent *resolved = gethostbyaddr(reinterpret_cast<const char *>(addr), len, AF_INET);\n'
     '    strcpy(hostname, resolved->h_name);\n',
     '    struct hostent *resolved = gethostbyaddr(reinterpret_cast<const char *>(addr), len, AF_INET);\n'
     '    if (resolved == nullptr) {\n'
     '        RET_NET_ERRNO(SCE_NET_ERROR_EHOSTUNREACH);\n'
     '    }\n'
     '    strcpy(hostname, resolved->h_name);\n'),
])
# (After 01.65) The PlayStation Store's catalogue (SceNpCommerce2): upstream every call of it is a stub that
# answers "done" and fills nothing in. MotorStorm RC, after its title, asks for the store's session and a
# category's contents on a thread of its own, takes the count of the contents from what was never filled in and
# asks for each - 11 million calls every 10 s on the console, 58 million on the PC - while its main thread waits
# for that thread: the game stayed there. A PS Vita without a network answers an error to the calls that would
# reach the store, and the game goes on without it; here the same, and the calls that read the store's answers
# say so too (a game that didn't look at the first error). What makes and ends things stays "done".
# The error is the network's own "no way to the network" (SCE_NET_ERROR_ENETUNREACH, net/types.h).
COMMERCE_NO_NETWORK = (
    'CreateSessionStart', 'CreateSessionGetResult', 'GetSessionInfo',
    'GetCategoryContentsStart', 'GetCategoryContentsGetResult', 'InitGetCategoryContentsResult',
    'GetCategoryInfo', 'GetCategoryInfoFromContentInfo', 'GetContentInfo', 'GetContentRatingDescriptor',
    'GetContentRatingInfoFromCategoryInfo', 'GetContentRatingInfoFromGameProductInfo',
    'GetProductInfoStart', 'GetProductInfoGetResult', 'InitGetProductInfoResult',
    'GetProductInfoListStart', 'GetProductInfoListGetResult', 'InitGetProductInfoListResult',
    'GetGameProductInfo', 'GetGameProductInfoFromContentInfo', 'GetGameProductInfoFromGetProductInfoListResult',
    'GetGameSkuInfoFromGameProductInfo', 'GetPrice',
)
patch('vita3k/modules/SceNpCommerce2/SceNpCommerce2.cpp', [
    ('#include <module/module.h>\n',
     '#include <module/module.h>\n\n'
     '#ifdef EMUSUNI_PORT\n'
     '// EmuSuNi: no network, so no store (scripts/build-vita-core.sh says why).\n'
     'static constexpr int EMUSUNI_NO_NETWORK = static_cast<int>(0x80410133u); // SCE_NET_ERROR_ENETUNREACH\n'
     '#endif\n'),
] + [
    ('EXPORT(int, sceNpCommerce2%s) {\n    return UNIMPLEMENTED();\n}\n' % name,
     'EXPORT(int, sceNpCommerce2%s) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    (void)UNIMPLEMENTED();\n'
     '    return EMUSUNI_NO_NETWORK;\n'
     '#else\n'
     '    return UNIMPLEMENTED();\n'
     '#endif\n'
     '}\n' % name)
    for name in COMMERCE_NO_NETWORK
])
# (After 01.68) The "offline" switch (18, OFF: a test): the game is told what is true here, that the console has
# no network. Upstream sceNetCtlInetGetState always says an address was obtained, and the system's network check
# dialog (sceNetCheckDialog) always ends well in its PlayStation Network modes; a game then goes on to its
# servers - which nothing here reaches: port/ps4_vita_nonet.c - and waits for them. LittleBigPlanet (PCSA00549)
# stays at "Connecting to LittleBigPlanet PS Vita Online" (seen on the PC bench, 90 s and no end). With the
# switch: "disconnected", and the dialog ends as when the player closes it without connecting. Ad hoc play has its
# own state and dialog mode: as they were. OFF from the factory: LittleBigPlanet stays at that screen with it
# too, and it changes what every game is told.
patch('vita3k/modules/SceNetCtl/SceNetCtl.cpp', [
    ('#include <util/net_utils.h>\n', '#include <util/net_utils.h>\n' + SWITCH_API),
    ('    *state = SCE_NET_CTL_STATE_IPOBTAINED;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    *state = emusuni_vita_switch(18) ? SCE_NET_CTL_STATE_DISCONNECTED : SCE_NET_CTL_STATE_IPOBTAINED;\n'
     '    if (emusuni_vita_switch(18))\n'
     '        return 0;\n'
     '#else\n'
     '    *state = SCE_NET_CTL_STATE_IPOBTAINED;\n'
     '#endif\n'),
])
patch('vita3k/modules/SceCommonDialog/SceCommonDialog.cpp', [
    ('#include <dialog/types.h>\n', '#include <dialog/types.h>\n' + SWITCH_API),
    ('        emuenv.common_dialog.result = SCE_COMMON_DIALOG_RESULT_OK;\n',
     '#ifdef EMUSUNI_PORT\n'
     '        emuenv.common_dialog.result = emusuni_vita_switch(18) ? SCE_COMMON_DIALOG_RESULT_USER_CANCELED : SCE_COMMON_DIALOG_RESULT_OK;\n'
     '#else\n'
     '        emuenv.common_dialog.result = SCE_COMMON_DIALOG_RESULT_OK;\n'
     '#endif\n'),
    # (The result's second word - did the PlayStation Network mode go well - was left as the game's memory had it.)
    ('        STUBBED("result->result = 0");\n',
     '        STUBBED("result->result = 0");\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(18) && emuenv.common_dialog.netcheck.mode != SCE_NETCHECK_DIALOG_MODE_ADHOC_CONN)\n'
     '        result->psnModeSucceeded = 0;\n'
     '#endif\n'),
    # (01.78) The "msgauto" switch (40, ON): the system's message dialog answers itself - port/ps4_vita_sys.h says
    # how. The standalone's overlay draws the message and gives the pressed button's value; this core draws none,
    # and the game asked for ever whether the dialog had finished.
    ('EXPORT(int, sceMsgDialogGetStatus) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <chrono>\n'
     '#include <cstdio>\n'
     '#include <string>\n'
     'static std::chrono::steady_clock::time_point emusuni_msg_since;\n'
     'static unsigned emusuni_msg_asks, emusuni_msg_said;\n'
     'static int emusuni_msg_kind; // 0 none open, 1 answered after 1.5 s, 2 a lone "Cancel": after 20 s, 3 a wait: never\n'
     'static void emusuni_msg_begin(EmuEnvState &emuenv, int system_type) {\n'
     '    auto &d = emuenv.common_dialog;\n'
     '    emusuni_msg_since = std::chrono::steady_clock::now();\n'
     '    emusuni_msg_asks = 0;\n'
     '    const bool wait = d.msg.has_progress_bar || system_type == SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT\n'
     '        || system_type == SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT_SMALL || system_type == SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT_CANCEL;\n'
     '    const bool lone_cancel = d.msg.btn_num == 1 && d.msg.btn_val[0] == SCE_MSG_DIALOG_BUTTON_ID_INVALID\n'
     '        && (d.msg.mode == SCE_MSG_DIALOG_MODE_USER_MSG || system_type == SCE_MSG_DIALOG_SYSMSG_TYPE_MAGNETIC_CALIBRATION);\n'
     '    emusuni_msg_kind = wait || d.msg.btn_num == 0 ? 3 : lone_cancel ? 2 : 1;\n'
     '    if (emusuni_msg_said++ >= 12)\n'
     '        return;\n'
     '    std::string text = d.msg.message;\n'
     '    for (char &c : text)\n'
     '        if (c == 10 || c == 13)\n'
     '            c = 32;\n'
     '    if (text.size() > 300)\n'
     '        text.resize(300);\n'
     '    char type[40] = "";\n'
     '    if (system_type >= 0)\n'
     '        snprintf(type, sizeof(type), ", the system\'s message %d", system_type);\n'
     '    emusuni_vita_log("vita: dialog: the game opened a message dialog (mode %d%s, %d buttons%s): \\"%s\\"%s", (int)d.msg.mode, type,\n'
     '        (int)d.msg.btn_num, d.msg.has_progress_bar ? ", a progress bar" : "", text.c_str(),\n'
     '        !emusuni_vita_switch(40) ? " - not answered, the msgauto switch is OFF" : emusuni_msg_kind == 3 ? " - the game\'s to close" : "");\n'
     '}\n'
     'static void emusuni_msg_answer(EmuEnvState &emuenv) {\n'
     '    auto &d = emuenv.common_dialog;\n'
     '    if (emusuni_msg_kind == 0 || d.status != SCE_COMMON_DIALOG_STATUS_RUNNING)\n'
     '        return;\n'
     '    emusuni_msg_asks++;\n'
     '    if (emusuni_msg_kind == 3 || emusuni_msg_asks < 8 || !emusuni_vita_switch(40))\n'
     '        return;\n'
     '    if (std::chrono::steady_clock::now() - emusuni_msg_since < std::chrono::milliseconds(emusuni_msg_kind == 2 ? 20000 : 1500))\n'
     '        return;\n'
     '    std::lock_guard<std::recursive_mutex> lock(d.mutex);\n'
     '    if (d.type != MESSAGE_DIALOG || d.status != SCE_COMMON_DIALOG_STATUS_RUNNING || d.msg.btn_num == 0)\n'
     '        return;\n'
     '    int pick = 0;\n'
     '    for (int i = 1; i < d.msg.btn_num && i < 3; i++)\n'
     '        if (d.msg.btn_val[i] == SCE_MSG_DIALOG_BUTTON_ID_NO)\n'
     '            pick = i;\n'
     '    d.msg.status = d.msg.btn_val[pick];\n'
     '    d.result = SCE_COMMON_DIALOG_RESULT_OK;\n'
     '    d.status = SCE_COMMON_DIALOG_STATUS_FINISHED;\n'
     '    emusuni_msg_kind = 0;\n'
     '    if (emusuni_msg_said <= 12)\n'
     '        emusuni_vita_log("vita: dialog: answered by itself after %u asks: button %d of %d, \\"%s\\" (the msgauto switch)", emusuni_msg_asks, pick + 1,\n'
     '            (int)d.msg.btn_num, d.msg.btn[pick].c_str());\n'
     '}\n'
     '#endif\n'
     'EXPORT(int, sceMsgDialogGetStatus) {\n'),
    ('    if (emuenv.common_dialog.type != MESSAGE_DIALOG)\n'
     '        return SCE_COMMON_DIALOG_STATUS_NONE;\n'
     '\n'
     '    return emuenv.common_dialog.status;\n',
     '    if (emuenv.common_dialog.type != MESSAGE_DIALOG)\n'
     '        return SCE_COMMON_DIALOG_STATUS_NONE;\n'
     '\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_msg_answer(emuenv);\n'
     '#endif\n'
     '    return emuenv.common_dialog.status;\n'),
    ('        LOG_ERROR("Attempt to init message dialog with unknown mode: {}", log_hex(p->mode));\n'
     '        break;\n'
     '    }\n'
     '\n'
     '    return 0;\n',
     '        LOG_ERROR("Attempt to init message dialog with unknown mode: {}", log_hex(p->mode));\n'
     '        break;\n'
     '    }\n'
     '\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_msg_begin(emuenv, p->mode == SCE_MSG_DIALOG_MODE_SYSTEM_MSG ? (int)p->sysMsgParam.get(emuenv.mem)->sysMsgType : -1);\n'
     '#endif\n'
     '    return 0;\n'),
])
# (After 01.67) A wait on sockets with nothing to wait for takes its time. Upstream sceNetEpollWait comes back at
# once when none of its sockets is a real one - here none ever is: nothing connects (port/ps4_vita_nonet.c) -,
# whatever the time it was given. LittleBigPlanet's "game host thread" waits so in a loop: 30 thousand times a
# second, most of a core. A PS Vita's wait lasts its time; here the same, in steps of 100 ms at most (the game
# asks again).
patch('vita3k/net/src/epoll.cpp', [
    ('#include <optional>\n', '#include <optional>\n#ifdef EMUSUNI_PORT\n#include <chrono>\n#include <thread>\n#endif\n'),
    ('    if (maxFd == 0)\n        return 0;\n',
     '    if (maxFd == 0) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (timeout_microseconds != 0) {\n'
     '            const long long emusuni_us = timeout_microseconds < 0 || timeout_microseconds > 100000 ? 100000 : timeout_microseconds;\n'
     '            std::this_thread::sleep_for(std::chrono::microseconds(emusuni_us));\n'
     '        }\n'
     '#endif\n'
     '        return 0;\n'
     '    }\n'),
])
# (After 01.67) The "mic" switch (19, ON): the microphone hears silence at its real pace. In the libretro core
# sceAudioInInput fills its block with silence and comes back at once - a PS Vita's waits until the block is
# recorded, 16 or 32 ms - and sceAudioInGetStatus says "muted" while no port is open. LittleBigPlanet's "Mic
# Thread" asked that status 5 million times a second: a whole core of the PC, more than one of the PS4's.
patch('vita3k/modules/SceAudioIn/SceAudioIn.cpp', [
    ('TRACY_MODULE_NAME(SceAudioIn);\n',
     'TRACY_MODULE_NAME(SceAudioIn);\n' + SWITCH_API +
     '#ifdef EMUSUNI_PORT\n'
     '#include <chrono>\n'
     '#include <thread>\n'
     'static int emusuni_in_freq = 16000; // (the open port\'s samples a second)\n'
     '// (A second port: a PS Vita has one of each kind, voice and raw; this core had one in all, and a game that\n'
     '// opens both - LittleBigPlanet - went on asking for sound with the failed port\'s number, without a wait.)\n'
     'static struct {\n'
     '    bool running;\n'
     '    int len_bytes;\n'
     '    int freq;\n'
     '} emusuni_in_more = { false, 0, 16000 };\n'
     '#endif\n'),
    ('    if (emuenv.audio.in_port.running) {\n        return RET_ERROR(SCE_AUDIO_IN_ERROR_PORT_FULL);\n    }\n',
     '    if (emuenv.audio.in_port.running) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_switch(19) && !emusuni_in_more.running && grain > 0 && freq > 0 && param == SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO) {\n'
     '            emusuni_in_more.running = true;\n'
     '            emusuni_in_more.len_bytes = grain * 2;\n'
     '            emusuni_in_more.freq = freq;\n'
     '            return PORT_ID + 1;\n'
     '        }\n'
     '#endif\n'
     '        return RET_ERROR(SCE_AUDIO_IN_ERROR_PORT_FULL);\n    }\n'),
    ('    if (port != PORT_ID) {\n        return RET_ERROR(SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM);\n    }\n\n#ifdef BUILD_LIBRETRO\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (port == PORT_ID + 1 && emusuni_in_more.running && emusuni_vita_switch(19)) {\n'
     '        memset(destPtr, 0, emusuni_in_more.len_bytes);\n'
     '        std::this_thread::sleep_for(std::chrono::microseconds(1000000ll * (emusuni_in_more.len_bytes / 2) / (emusuni_in_more.freq > 0 ? emusuni_in_more.freq : 16000)));\n'
     '        return 0;\n'
     '    }\n'
     '#endif\n'
     '    if (port != PORT_ID) {\n        return RET_ERROR(SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM);\n    }\n\n#ifdef BUILD_LIBRETRO\n'),
    ('    if (port != PORT_ID) {\n        return RET_ERROR(SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM);\n    }\n'
     '    if (!emuenv.audio.in_port.running) {\n        return RET_ERROR(SCE_AUDIO_IN_ERROR_NOT_OPENED);\n    }\n'
     '    emuenv.audio.in_port.running = false;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (port == PORT_ID + 1 && emusuni_in_more.running) {\n'
     '        emusuni_in_more.running = false;\n'
     '        return 0;\n'
     '    }\n'
     '#endif\n'
     '    if (port != PORT_ID) {\n        return RET_ERROR(SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM);\n    }\n'
     '    if (!emuenv.audio.in_port.running) {\n        return RET_ERROR(SCE_AUDIO_IN_ERROR_NOT_OPENED);\n    }\n'
     '    emuenv.audio.in_port.running = false;\n'),
    ('    return emuenv.audio.in_port.running ? 0 : 1;\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(19))\n'
     '        return 0; // (not muted)\n'
     '#endif\n'
     '    return emuenv.audio.in_port.running ? 0 : 1;\n'),
    ('    memset(destPtr, 0, emuenv.audio.in_port.len_bytes);\n',
     '    memset(destPtr, 0, emuenv.audio.in_port.len_bytes);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(19)) {\n'
     '        // (the block\'s own time: its samples - two bytes each - at the port\'s rate)\n'
     '        const long long emusuni_us = 1000000ll * (emuenv.audio.in_port.len_bytes / 2) / (emusuni_in_freq > 0 ? emusuni_in_freq : 16000);\n'
     '        std::this_thread::sleep_for(std::chrono::microseconds(emusuni_us));\n'
     '    }\n'
     '#endif\n'),
    ('    emuenv.audio.in_port.len_bytes = grain * 2;\n    emuenv.audio.in_port.running = true;\n    return PORT_ID;\n#else\n',
     '    emuenv.audio.in_port.len_bytes = grain * 2;\n    emuenv.audio.in_port.running = true;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_in_freq = freq;\n'
     '#endif\n'
     '    return PORT_ID;\n#else\n'),
])
# (After 01.67) sceAppMgrLoadExec finds the executable whatever the case of its name. The PS Vita's file system
# doesn't tell upper case from lower; the PS4's and Linux's do (Windows', where Vita3K is mostly run, doesn't).
# God of War Collection's launcher (PCSA00126) asks for "app0:GOW1.self" and "app0:GOW2.self"; its folder has
# gow1.self and gow2.self: the call answered "invalid self path", and the launcher went on to write at address
# 0x10 - a black screen (seen on the PC bench with the launcher's "God of War" touched). Each name of the path is
# taken as it is when it is there and, when it isn't, looked for among its folder's names without the case.
patch('vita3k/modules/SceAppMgr/SceAppMgr.cpp', [
    ('TRACY_MODULE_NAME(SceAppMgr);\n',
     'TRACY_MODULE_NAME(SceAppMgr);\n' + SWITCH_API +
     '#ifdef EMUSUNI_PORT\n#include <cctype>\n#include <system_error>\n#include <util/fs.h>\n#endif\n'),
    ('    LOG_INFO("sceAppMgrLoadExec run self: {}", appPath);\n',
     '    LOG_INFO("sceAppMgrLoadExec run self: {}", appPath);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    {\n'
     '        const fs::path emusuni_app = emuenv.vita_fs_path / "ux0" / "app" / emuenv.io.app_path;\n'
     '        std::error_code emusuni_ec;\n'
     '        if (!fs::exists(emusuni_app / exec_path, emusuni_ec)) {\n'
     '            const auto emusuni_lower = [](std::string s) {\n'
     '                for (char &c : s)\n'
     '                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));\n'
     '                return s;\n'
     '            };\n'
     '            fs::path emusuni_at = emusuni_app;\n'
     '            std::string emusuni_real;\n'
     '            bool emusuni_found = true;\n'
     '            size_t emusuni_from = 0;\n'
     '            while (emusuni_found && emusuni_from < exec_path.size()) {\n'
     '                size_t emusuni_to = exec_path.find(\'/\', emusuni_from);\n'
     '                if (emusuni_to == std::string::npos)\n'
     '                    emusuni_to = exec_path.size();\n'
     '                const std::string emusuni_name = exec_path.substr(emusuni_from, emusuni_to - emusuni_from);\n'
     '                emusuni_from = emusuni_to + 1;\n'
     '                if (emusuni_name.empty())\n'
     '                    continue;\n'
     '                std::string emusuni_is = emusuni_name;\n'
     '                if (!fs::exists(emusuni_at / emusuni_name, emusuni_ec)) {\n'
     '                    emusuni_found = false;\n'
     '                    for (fs::directory_iterator emusuni_it(emusuni_at, emusuni_ec), emusuni_end; !emusuni_ec && emusuni_it != emusuni_end; emusuni_it.increment(emusuni_ec)) {\n'
     '                        const std::string emusuni_there = emusuni_it->path().filename().string();\n'
     '                        if (emusuni_lower(emusuni_there) == emusuni_lower(emusuni_name)) {\n'
     '                            emusuni_is = emusuni_there;\n'
     '                            emusuni_found = true;\n'
     '                            break;\n'
     '                        }\n'
     '                    }\n'
     '                }\n'
     '                emusuni_at /= emusuni_is;\n'
     '                emusuni_real += (emusuni_real.empty() ? "" : "/") + emusuni_is;\n'
     '            }\n'
     '            if (emusuni_found && !emusuni_real.empty() && emusuni_real != exec_path) {\n'
     '                emusuni_vita_log("vita: exec: the game asks for %s; its folder has it as %s", exec_path.c_str(), emusuni_real.c_str());\n'
     '                exec_path = emusuni_real;\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '#endif\n'),
])
# (After 01.67) The render thread's stop gets through its wait for a sync object. At a reset and at a game's end
# the core stops its render thread first (stop_render_thread: a flag and a join) and only then wakes what waits on
# GXM. When the thread is in a WaitSyncObject command - waiting for an image to be shown - and the game's display
# callback is waiting for the screen's next refresh, nothing ends: the refreshes come from retro_run, which is the
# one waiting for the thread. God of War Collection's launcher (60 pictures a second, a queue of 1, its callback
# on sceDisplayWaitVblankStart) stayed there for good when it started gow1.self (seen with gdb on the PC bench);
# closing such a game would hang the same way. The command now waits in slices of 20 ms and gives up once the
# stop is asked for; the other waits on sync objects (the display's own thread, the game's) are as they were.
patch('vita3k/renderer/src/sync.cpp', [
    ('namespace renderer {\n',
     '#ifdef EMUSUNI_PORT\n'
     '#include <atomic>\n'
     '// (Set by the libretro core around its render thread\'s join.)\n'
     'std::atomic<bool> emusuni_vita_render_stopping{ false };\n'
     'extern "C" unsigned long long emusuni_vita_stat_now(void);\n'
     '#endif\n\n'
     'namespace renderer {\n'),
    # (After 01.68) When the game sent the list, for the log's "vita: draws:" line (batch.cpp's patch).
    ('    command_list.context = context;\n    state.command_buffer_queue.push(std::move(command_list));\n',
     '    command_list.context = context;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    command_list.emusuni_sent = emusuni_vita_stat_now();\n'
     '#endif\n'
     '    state.command_buffer_queue.push(std::move(command_list));\n'),
    ('    renderer::wishlist(sync, timestamp);\n',
     '#ifdef EMUSUNI_PORT\n'
     '    while (renderer::wishlist(sync, timestamp, 20000) == SyncWaitResult::TimedOut) {\n'
     '        if (emusuni_vita_render_stopping.load(std::memory_order_relaxed))\n'
     '            break;\n'
     '    }\n'
     '#else\n'
     '    renderer::wishlist(sync, timestamp);\n'
     '#endif\n'),
])
# (After 01.66) The "ngsdefer" switch (17, ON). A voice keyed off with sceNgsVoiceKeyOff ends at the next
# sceNgsSystemUpdate, and its "finished" callback runs there, on the thread that makes that call - as on a PS
# Vita. Upstream ends the voice and runs the callback inside sceNgsVoiceKeyOff, on the thread that keyed it off.
# MotorStorm RC's "Game Audio Update Thread" keys voices off while it holds its sound system's lock (a word it
# sets with ldrex/strex and waits for with sceKernelDelayThread(250), no limit), and its callback takes that
# lock: the thread waited for itself, and the main thread and the "NGSynth Update Thread" after it - no more
# pictures from the first frames of its tutorial's race on (seen with gdb on the PC bench: the three threads in
# that wait, two of them inside ngs::Voice::invoke_callback).
# Only for a voice that has that callback, is playing and isn't paused (a paused voice is out of the scheduler's
# queue: it would never be reached): the others end at once, as before.
patch('vita3k/modules/SceNgsUser/SceNgs.cpp', [
    ('TRACY_MODULE_NAME(SceNgs);\n', 'TRACY_MODULE_NAME(SceNgs);\n' + SWITCH_API),
    ('    voice->rack->system->voice_scheduler.off(emuenv.mem, voice);\n',
     '    voice->rack->system->voice_scheduler.off(emuenv.mem, voice);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(17) && voice->finished_callback && voice->state == ngs::VOICE_STATE_FINALIZING && !voice->is_paused) {\n'
     '        static std::atomic<bool> emusuni_said{ false };\n'
     '        if (!emusuni_said.exchange(true))\n'
     '            emusuni_vita_log("vita: sound: a voice keyed off ends, and its callback runs, at the next sound update (the ngsdefer switch is ON)");\n'
     '        return SCE_NGS_OK; // (is_keyed_off stays set: the scheduler\'s update ends the voice)\n'
     '    }\n'
     '#endif\n'),
    ('#include <ngs/state.h>\n', '#include <atomic>\n#include <ngs/state.h>\n'),
])
# (18-11, the performance specialist's) The switch "ngsresample" (its own file, off by default:
# /data/EmuSuNi/vita-ngsresample-on; the PC: EMUSUNI_VITA_NGSRESAMPLE=1): the NGS voices that are not at 48 kHz go
# through port/ps4_vita_ngsresample.cpp (16 products a sample, its tables made once, a voice going on from where it
# was when its pitch changes) instead of an FFmpeg resampler made anew at every pitch change (MotorStorm RC: its
# "NGSynth Update Thread" 79-88% of a PS4 core, sceNgsSystemUpdate 11 ms a call; that file says why).
NGSRS_API = ('#ifdef EMUSUNI_PORT\n'
             'extern "C" int emusuni_vita_ngs_rs_on(void);\n'
             'extern "C" void emusuni_vita_ngs_rs_reset(void *state);\n'
             'extern "C" unsigned emusuni_vita_ngs_rs_bound(const void *state, int source_rate, int dest_rate, unsigned in_frames);\n'
             'extern "C" unsigned emusuni_vita_ngs_rs_run(void *state, int source_rate, int dest_rate, const float *in,\n'
             '    unsigned in_frames, float *out);\n'
             '#endif\n')
patch('vita3k/ngs/include/ngs/rate_resampler.h', [
    ('    std::vector<uint8_t> scratch_buffer;\n};\n',
     '    std::vector<uint8_t> scratch_buffer;\n'
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, the switch "ngsresample") This voice goes through the own resampler; its state.\n'
     '    bool emusuni_own = false;\n'
     '    alignas(16) unsigned char emusuni_state[160];\n'
     '#endif\n'
     '};\n'),
])
patch('vita3k/ngs/src/rate_resampler.cpp', [
    ('#include <algorithm>\n#include <cstdint>\n', '#include <algorithm>\n#include <cstdint>\n' + NGSRS_API),
    ('    runtime.source_rate = 0;\n    runtime.dest_rate = 0;\n    runtime.scratch_buffer.clear();\n}\n',
     '    runtime.source_rate = 0;\n    runtime.dest_rate = 0;\n    runtime.scratch_buffer.clear();\n'
     '#ifdef EMUSUNI_PORT\n'
     '    runtime.emusuni_own = false;\n'
     '#endif\n'
     '}\n'),
    ('    const bool needs_recreate = logical.needs_reset || !runtime.context || runtime.source_rate != source_rate\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // The own resampler: a new rate is only a new step; a reset (the voice keyed on or off) starts it from silence.\n'
     '    if (emusuni_vita_ngs_rs_on()) {\n'
     '        if (logical.needs_reset || !runtime.emusuni_own) {\n'
     '            if (runtime.context)\n'
     '                swr_free(&runtime.context);\n'
     '            emusuni_vita_ngs_rs_reset(runtime.emusuni_state);\n'
     '            runtime.emusuni_own = true;\n'
     '            logical.input_history.clear();\n'
     '            logical.needs_reset = false;\n'
     '        }\n'
     '        runtime.source_rate = source_rate;\n'
     '        runtime.dest_rate = dest_rate;\n'
     '        return true;\n'
     '    }\n'
     '#endif\n'
     '    const bool needs_recreate = logical.needs_reset || !runtime.context || runtime.source_rate != source_rate\n'),
    ('    if (!input || input_frames == 0 || !runtime.context) {\n        return 0;\n    }\n',
     '#ifdef EMUSUNI_PORT\n'
     '    if (runtime.emusuni_own) {\n'
     '        if (!input || input_frames == 0)\n'
     '            return 0;\n'
     '        const unsigned bound = emusuni_vita_ngs_rs_bound(runtime.emusuni_state, runtime.source_rate, runtime.dest_rate, input_frames);\n'
     '        const size_t old_size = output.samples.size();\n'
     '        output.samples.resize(old_size + static_cast<size_t>(bound) * stereo_channels);\n'
     '        const unsigned made = emusuni_vita_ngs_rs_run(runtime.emusuni_state, runtime.source_rate, runtime.dest_rate,\n'
     '            reinterpret_cast<const float *>(input), input_frames, output.samples.data() + old_size);\n'
     '        output.samples.resize(old_size + static_cast<size_t>(made) * stereo_channels);\n'
     '        return made;\n'
     '    }\n'
     '#endif\n'
     '    if (!input || input_frames == 0 || !runtime.context) {\n        return 0;\n    }\n'),
])
patch('vita3k/ngs/src/modules/player.cpp', [
    ('#include <ngs/modules/player.h>\n', '#include <ngs/modules/player.h>\n' + NGSRS_API),
    ('        std::fill_n(logical->adpcm_history, SCE_NGS_PLAYER_MAX_PCM_CHANNELS, hist_empty);\n\n'
     '        logical->rate_resampler.reset();\n',
     '        std::fill_n(logical->adpcm_history, SCE_NGS_PLAYER_MAX_PCM_CHANNELS, hist_empty);\n\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (!emusuni_vita_ngs_rs_on()) // (the own resampler goes on at the new rate)\n'
     '#endif\n'
     '        logical->rate_resampler.reset();\n'),
])
patch('vita3k/ngs/src/modules/atrac9.cpp', [
    ('#include <ngs/modules/atrac9.h>\n', '#include <ngs/modules/atrac9.h>\n' + NGSRS_API),
    ('    if (old_params->playback_frequency != new_params->playback_frequency || old_params->playback_scalar != new_params->playback_scalar) {\n'
     '        logical->rate_resampler.reset();\n',
     '    if (old_params->playback_frequency != new_params->playback_frequency || old_params->playback_scalar != new_params->playback_scalar) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (!emusuni_vita_ngs_rs_on()) // (the own resampler goes on at the new rate)\n'
     '#endif\n'
     '        logical->rate_resampler.reset();\n'),
])
patch('vita3k/ngs/src/ngs.cpp', [
    ('bool init(State &ngs, MemState &mem) {\n    voice_definition_init(ngs, mem);\n',
     '#ifdef EMUSUNI_PORT\n'
     'extern "C" void emusuni_vita_ngs_rs_start(void);\n'
     '#endif\n'
     'bool init(State &ngs, MemState &mem) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    emusuni_vita_ngs_rs_start(); // (the switch "ngsresample" as it is now)\n'
     '#endif\n'
     '    voice_definition_init(ngs, mem);\n'),
])
patch('vita3k/ngs/src/scheduler.cpp', [
    ('namespace ngs {\n', SWITCH_API + 'namespace ngs {\n'),
    # (A voice stopped before that update - sceNgsVoiceKill - is no longer keyed off: the mark would stay on it
    # into its next use, where the player's modules take it for a key-off and start their buffer again.)
    ('    voice->transition(mem, VOICE_STATE_AVAILABLE);\n',
     '    voice->transition(mem, VOICE_STATE_AVAILABLE);\n'
     '#ifdef EMUSUNI_PORT\n'
     '    if (emusuni_vita_switch(17))\n'
     '        voice->is_keyed_off = false;\n'
     '#endif\n'),
    ('        // Modify the state, in peace....\n',
     '#ifdef EMUSUNI_PORT\n'
     '        // A voice keyed off since the last update (sceNgsVoiceKeyOff left it so): its end, here.\n'
     '        if (voice->is_keyed_off && voice->state == VOICE_STATE_FINALIZING && emusuni_vita_switch(17)) {\n'
     '            if (voice->finished_callback) {\n'
     '                scheduler_lock.unlock();\n'
     '                voice->invoke_callback(kern, mem, thread_id, voice->finished_callback, voice->finished_callback_user_data, 0);\n'
     '                scheduler_lock.lock();\n'
     '            }\n'
     '            voice->is_keyed_off = false;\n'
     '            stop(mem, voice);\n'
     '            continue;\n'
     '        }\n'
     '#endif\n'
     '        // Modify the state, in peace....\n'),
])
patch('vita3k/util/src/net_utils.cpp', [
    ('#include <curl/curl.h>\n', '#ifndef EMUSUNI_PORT\n#include <curl/curl.h>\n#endif\n'),
    ('std::string get_web_response(const std::string &url) {\n',
     'std::string get_web_response(const std::string &url) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '    return {}; // EmuSuNi: no HTTP client here\n'
     '#else\n'),
    ('    if (response_code / 100 == 2)\n        return response_string;\n\n    return {};\n}\n',
     '    if (response_code / 100 == 2)\n        return response_string;\n\n    return {};\n#endif\n}\n'),
])

# FFmpeg: on the PS4 the libraries this script builds (EMUSUNI_FFMPEG_DIR), their own headers too;
# upstream downloads libraries built for the system, and has none for this one.
patch('external/ffmpeg/CMakeLists.txt', [
    ('# Detect OS\n',
     'if(EMUSUNI_FFMPEG_DIR)\n'
     '\tset_target_properties(${FFMPEG_CORE_NAME} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${EMUSUNI_FFMPEG_DIR}/include")\n'
     '\ttarget_link_libraries(${FFMPEG_CORE_NAME} INTERFACE\n'
     '\t\t"${EMUSUNI_FFMPEG_DIR}/lib/libavformat.a" "${EMUSUNI_FFMPEG_DIR}/lib/libavcodec.a"\n'
     '\t\t"${EMUSUNI_FFMPEG_DIR}/lib/libswscale.a" "${EMUSUNI_FFMPEG_DIR}/lib/libswresample.a"\n'
     '\t\t"${EMUSUNI_FFMPEG_DIR}/lib/libavutil.a")\n'
     '\treturn()\n'
     'endif()\n'
     '# Detect OS\n'),
], repo='external/ffmpeg')

# The keys (VITA_CORE_NOKEYS=1: the macro EMUSUNI_VITA_NOKEYS, which that build's prefix header gives every
# source). Vita3K's source has Sony's keys written in it: those of the system's modules and packages
# (register_keys), of the store's packages, of a game card's image, of a game's encrypted file system
# (psvpfstools) and the table the system's registry is scrambled with. A build without them takes them from a
# file of the player's (port/ps4_vita_keys.cpp, read at retro_load_game above): the modules' keys are that
# file's, each table is zeros until it is read (the packages', the card's and the registry's are then the
# port's own variables), and what needs a key without the file says so in the log and fails as a file that
# can't be read does - the firmware's install, an encrypted game's install or start, a package, a card's image;
# the registry is then an empty one. A build with the keys is the source as it was.
patch('vita3k/packages/src/sce_utils.cpp', [
    ('void register_keys(KeyStore &SCE_KEYS, int type) {\n',
     KEYS_API +
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     'extern "C" int emusuni_vita_keys_sce(int index, int *set, const char **names, int *revision, const char **key,\n'
     '    const char **iv, unsigned long long *versions);\n'
     'void register_keys(KeyStore &SCE_KEYS, int type) {\n'
     '    int set = 0, revision = 0;\n'
     '    const char *names[3] = {}, *key = nullptr, *iv = nullptr;\n'
     '    unsigned long long versions[2] = {};\n'
     '    for (int i = 0; emusuni_vita_keys_sce(i, &set, names, &revision, &key, &iv, versions); i++) {\n'
     '        if (set != type)\n'
     '            continue;\n'
     '        const std::string key_type = names[0], sce_type = names[1], self_type = names[2];\n'
     '        const KeyType kt = key_type == "NPDRM" ? KeyType::NPDRM : KeyType::METADATA;\n'
     '        const SceType st = sce_type == "SELF" ? SceType::SELF : sce_type == "SRVK" ? SceType::SRVK\n'
     '            : sce_type == "SPKG" ? SceType::SPKG : SceType::DEV;\n'
     '        const SelfType ft = self_type == "KERNEL" ? SelfType::KERNEL : self_type == "APP" ? SelfType::APP\n'
     '            : self_type == "BOOT" ? SelfType::BOOT : self_type == "SECURE" ? SelfType::SECURE\n'
     '            : self_type == "USER" ? SelfType::USER : SelfType::NONE;\n'
     '        SCE_KEYS.register_keys(kt, st, revision, key, iv, versions[0], versions[1], ft);\n'
     '    }\n'
     '}\n'
     '#else\n'
     'void register_keys(KeyStore &SCE_KEYS, int type) {\n'),
    ('        break;\n    }\n}\n\nstatic void extract_file(Fat16::Image &img, Fat16::Entry &entry, const fs::path &path) {\n',
     '        break;\n    }\n}\n#endif\n\n'
     'static void extract_file(Fat16::Image &img, Fat16::Entry &entry, const fs::path &path) {\n'),
    ('    if (!is_fself_encrypted(fself))\n        return fself; // Self is not encrypted, return the original self\n',
     '    if (!is_fself_encrypted(fself))\n        return fself; // Self is not encrypted, return the original self\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    if (emusuni_vita_keys_missing("an encrypted module of the game"))\n'
     '        return {};\n'
     '#endif\n'),
])
patch('vita3k/packages/src/pup.cpp', [
    ('std::string install_pup(const fs::path &vita_fs_path, const fs::path &pup_path, const std::function<void(uint32_t)> &progress_callback) {\n',
     KEYS_API + STEP_API +
     'std::string install_pup(const fs::path &vita_fs_path, const fs::path &pup_path, const std::function<void(uint32_t)> &progress_callback) {\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    if (emusuni_vita_keys_missing("the firmware\'s install"))\n'
     '        return {};\n'
     '#endif\n'),
    # (01.29: the install's steps, each on the disk before it is taken.)
    ('    extract_pup_files(pup_path, pup_dest);\n    update_progress(20);\n',
     '    emusuni_vita_step("firmware: the package\'s files are taken out of %s", pup_path.filename().string().c_str());\n'
     '    extract_pup_files(pup_path, pup_dest);\n    update_progress(20);\n'
     '    emusuni_vita_step("firmware: its keys");\n'),
    ('    update_progress(30);\n    decrypt_pup_packages(pup_dest, pup_dec, SCE_KEYS);\n\n    update_progress(70);\n',
     '    update_progress(30);\n'
     '    emusuni_vita_step("firmware: its packages are decrypted");\n'
     '    decrypt_pup_packages(pup_dest, pup_dec, SCE_KEYS);\n\n    update_progress(70);\n'
     '    emusuni_vita_step("firmware: its disk images are unpacked into the storage");\n'),
    ('    update_progress(100);\n\n    // get firmware version\n',
     '    update_progress(100);\n'
     '    emusuni_vita_step("firmware: unpacked; what the install left is deleted");\n\n'
     '    // get firmware version\n'),
    # (01.30: the folder the install works in. 01.29 ended the app at each of these two deletes - see
    # port/ps4_vita_files.cpp. Now with an error code, and a folder that can't be deleted is no failure: before
    # the install it goes on in one of another name beside it, after it the firmware is installed all the same.)
    ('    if (fs::exists(pup_dec_root)) {\n'
     '        LOG_WARN("Path already exists, deleting it and reinstalling");\n'
     '        fs::remove_all(pup_dec_root);\n'
     '    }\n',
     '    {\n'
     '        std::error_code leftover_ec;\n'
     '        for (int n = 2; n <= 9 && fs::exists(pup_dec_root, leftover_ec); n++) {\n'
     '            LOG_WARN("{} already exists, deleting it and reinstalling", pup_dec_root);\n'
     '            emusuni_vita_step("firmware: %s, left by an install, is deleted", pup_dec_root.filename().string().c_str());\n'
     '            fs::remove_all(pup_dec_root, leftover_ec);\n'
     '            if (fs::exists(pup_dec_root, leftover_ec)) {\n'
     '                LOG_ERROR("{} couldn\'t be deleted: the install goes on in a folder beside it", pup_dec_root);\n'
     '                pup_dec_root = vita_fs_path / fmt::format("PUP_DEC{}", n);\n'
     '            }\n'
     '        }\n'
     '    }\n'),
    ('    fs::remove_all(pup_dec_root);\n\n    return fw_version;\n',
     '    {\n'
     '        std::error_code leftover_ec;\n'
     '        fs::remove_all(pup_dec_root, leftover_ec);\n'
     '        if (leftover_ec)\n'
     '            LOG_ERROR("{} couldn\'t be deleted ({}): it stays, what it held is installed all the same", pup_dec_root, leftover_ec.message());\n'
     '        emusuni_vita_step("firmware: what the install left is %s", leftover_ec ? "NOT deleted whole: it stays" : "deleted");\n'
     '    }\n\n'
     '    return fw_version;\n'),
])
patch('vita3k/libretro/libretro_installer.cpp', [
    # (Before the note that an install was begun: nothing is left behind.)
    ('static bool install_pup_file(const fs::path &pref_path, const fs::path &pup_path, const char *name_key) {\n',
     KEYS_API + STEP_API + RESCUE_API +
     '#ifdef EMUSUNI_PORT\n'
     'bool emusuni_vita_firmware_failed = false; // (a firmware part\'s install failed in this load: libretro_vita3k.cpp)\n'
     '#endif\n'
     'static bool install_pup_file(const fs::path &pref_path, const fs::path &pup_path, const char *name_key) {\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    if (emusuni_vita_keys_missing("the firmware\'s install"))\n'
     '        return false;\n'
     '#endif\n'
     '    emusuni_vita_step("firmware: installing %s", pup_path.filename().string().c_str());\n'),
    # (01.29: each part's end on the disk at once, as its steps are.)
    ('        lr_log(RETRO_LOG_INFO, "%s version %s installed successfully!\\n", label, fw_version.c_str());\n',
     '        lr_log(RETRO_LOG_INFO, "%s version %s installed successfully!\\n", label, fw_version.c_str());\n'
     '        emusuni_vita_sync();\n'),
    ('    const FirmwareStatus status = check_firmware_status(pref_path);\n'
     '    const std::tuple<PupKind, const char *, bool> parts[] = {\n',
     '    emusuni_vita_step("firmware: what is installed is looked at");\n'
     '    const FirmwareStatus status = check_firmware_status(pref_path);\n'
     '    const std::tuple<PupKind, const char *, bool> parts[] = {\n'),
    # (01.30) A part's install: on the PS4 its catches are never reached, so a throw under it goes back to the
    # rescue point instead, which ends the part as they do. And each way it fails is noted for the load.
    ('    std::string fw_version;\n    try {\n',
     '    std::string fw_version;\n'
     '    EmusuniVitaRescueScope emusuni_rescue_scope;\n'
     '    if (setjmp(*emusuni_vita_rescue_push("the firmware\'s install")) != 0) {\n'
     '        lr_log(RETRO_LOG_ERROR, "%s installation failed: %s\\n", label, emusuni_vita_rescued());\n'
     '        lr_log(RETRO_LOG_ERROR, "PUP file may be corrupted or wrong format: %s\\n", pup_path.generic_string().c_str());\n'
     '        lr_msg(lr_textf("msg_part_install_failed", lr_text(name_key)).c_str(), 600);\n'
     '        emusuni_vita_firmware_failed = true;\n'
     '        return false;\n'
     '    }\n'
     '    try {\n'),
    ('        lr_log(RETRO_LOG_ERROR, "%s installation threw exception: %s\\n", label, e.what());\n',
     '        lr_log(RETRO_LOG_ERROR, "%s installation threw exception: %s\\n", label, e.what());\n'
     '        emusuni_vita_firmware_failed = true;\n'),
    ('        lr_log(RETRO_LOG_ERROR, "%s installation threw unknown exception\\n", label);\n',
     '        lr_log(RETRO_LOG_ERROR, "%s installation threw unknown exception\\n", label);\n'
     '        emusuni_vita_firmware_failed = true;\n'),
    # (01.30) What an install cut short left: said as it went - 01.29's "Removed PUP_DEC" was said of a folder
    # that stayed -, and with the folders an install made beside one that couldn't be deleted.
    ('    if (fs::exists(pref_path / "PUP_DEC", ec)) {\n'
     '        fs::remove_all(pref_path / "PUP_DEC", ec);\n'
     '        lr_log(RETRO_LOG_INFO, "Removed PUP_DEC, left by a firmware install that was not finished\\n");\n'
     '    }\n',
     '    emusuni_vita_firmware_failed = false;\n'
     '    for (const char *left : { "PUP_DEC", "PUP_DEC2", "PUP_DEC3", "PUP_DEC4", "PUP_DEC5", "PUP_DEC6", "PUP_DEC7", "PUP_DEC8", "PUP_DEC9" }) {\n'
     '        if (!fs::exists(pref_path / left, ec))\n'
     '            continue;\n'
     '        emusuni_vita_step("firmware: %s, left by an install that was not finished, is deleted", left);\n'
     '        fs::remove_all(pref_path / left, ec);\n'
     '        const bool stays = fs::exists(pref_path / left, ec);\n'
     '        lr_log(stays ? RETRO_LOG_ERROR : RETRO_LOG_INFO, "%s, left by a firmware install that was not finished, %s\\n", left,\n'
     '            stays ? "could NOT be removed: it stays" : "is removed");\n'
     '    }\n'),
    # (No address in the core's own words about the firmware - Sony's site and two short links: the app says
    # where the firmware's files go, and nothing in the PS4's code points to Sony's servers.)
    ('(pd0/) → https://bit.ly/4hlePsX\\n");\n', '(pd0/)\\n");\n'),
    ('(vs0/) → https://www.playstation.com/en-us/support/hardware/psvita/system-software\\n");\n',
     '(vs0/)\\n");\n'),
    ('(sa0/) → https://bit.ly/48ouDaa\\n");\n', '(sa0/)\\n");\n'),
])
patch('vita3k/packages/src/pkg.cpp', [
    ('bool decrypt_install_nonpdrm(EmuEnvState &emuenv, const fs::path &drmlicpath, const fs::path &title_path, const std::function<void(float)> &progress_callback) {\n',
     KEYS_API +
     'bool decrypt_install_nonpdrm(EmuEnvState &emuenv, const fs::path &drmlicpath, const fs::path &title_path, const std::function<void(float)> &progress_callback) {\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    if (emusuni_vita_keys_missing("an encrypted game\'s install (NoNpDrm)"))\n'
     '        return false;\n'
     '#endif\n'),
    ('bool install_pkg(const fs::path &pkg_path, EmuEnvState &emuenv, std::string &p_zRIF, const std::function<void(float)> &progress_callback) {\n',
     'bool install_pkg(const fs::path &pkg_path, EmuEnvState &emuenv, std::string &p_zRIF, const std::function<void(float)> &progress_callback) {\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    if (emusuni_vita_keys_missing("a package\'s install (.pkg)"))\n'
     '        return false;\n'
     '#endif\n'),
])
patch('vita3k/packages/include/packages/pkg.h', [], tables=[
    ('pkg_vita_2', 'extern uint8_t pkg_vita_2[16];'),
    ('pkg_vita_3', 'extern uint8_t pkg_vita_3[16];'),
    ('pkg_vita_4', 'extern uint8_t pkg_vita_4[16];'),
])
patch('vita3k/packages/src/vci.cpp', [
    ('bool install_vci(const fs::path &vci_path, EmuEnvState &emuenv, const std::function<void(float)> &progress_callback) {\n',
     KEYS_API +
     'bool install_vci(const fs::path &vci_path, EmuEnvState &emuenv, const std::function<void(float)> &progress_callback) {\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    if (emusuni_vita_keys_missing("a game card\'s image (.vci)"))\n'
     '        return false;\n'
     '#endif\n'),
])
patch('vita3k/packages/include/packages/vci.h', [], tables=[
    ('BIND_KEY', 'extern uint8_t BIND_KEY[0x10];'),
])
# (A game read where it lies, the core's "Run" mode: its encrypted file system, a package's own layer.)
# (After 01.70) The "zipmarks" switch (25, ON): a compressed file of a game's ZIP read at any place without going
# back to its start. Upstream inflates such a file as it is read, one stream a file, and when a part before the
# stream's place is asked for it starts again from the file's first byte, inflating and throwing away everything
# up to that part. Street Fighter X Tekken keeps its game in one file of 1 GiB (archive/CMN/CMN.eaf), compressed in
# the owner's ZIP (to no gain: 0% smaller), and reads it here and there: on the console a read of it took 50 to
# 190 ms (01.70's log: sceIoPread, the game's file thread a whole core, minutes at the "VS" screen). With the
# switch: six streams at a time, by where they are and not one a file - a read takes the one of its file that is
# the furthest in without being past its place -, and a mark is left every 4 MiB a stream goes past (the
# inflater's state and the last 32 KiB written, some 45 KiB each, 320 at most), from which a new stream starts
# when no stream is near. The log's "vita: zip:" line says how much was inflated only to get somewhere, with
# the switch or without.
patch('vita3k/libretro/libretro_lazy_pkg.cpp', [
    ('#include "libretro_lazy_pkg.h"\n', '#include "libretro_lazy_pkg.h"\n' + KEYS_API + STAT_API + SWITCH_API),
    # (the same counts with the switch OFF: what upstream's way inflates only to get somewhere)
    ('            m_streams.push_back({ index, state, 0 });\n            s = std::prev(m_streams.end());\n',
     '            m_streams.push_back({ index, state, 0 });\n            s = std::prev(m_streams.end());\n'
     '#ifdef EMUSUNI_PORT\n'
     '            emusuni_vita_stat_add(25, 0);\n'
     '#endif\n'),
    ('            s->position += n;\n        }\n        std::size_t done = 0;\n',
     '            s->position += n;\n'
     '#ifdef EMUSUNI_PORT\n'
     '            emusuni_vita_stat_add(24, n);\n'
     '#endif\n'
     '        }\n        std::size_t done = 0;\n'),
    ('    bool inflate_at(mz_uint index, std::uint64_t at, std::uint8_t *out, std::size_t size) {\n',
     '#ifdef EMUSUNI_PORT\n'
     '    // (EmuSuNi, the "zipmarks" switch) A stream as it was at a place of its file: its numbers with its\n'
     '    // inflater\'s state, and the 32 KiB it had written last. (The compressed bytes it had read ahead are read\n'
     '    // again by the stream that starts from the mark.)\n'
     '    struct EmusuniMark {\n'
     '        mz_uint index;\n'
     '        std::uint64_t position;\n'
     '        mz_zip_reader_extract_iter_state state;\n'
     '        std::uint8_t written[TINFL_LZ_DICT_SIZE];\n'
     '    };\n'
     '\n'
     '    // A mark where the stream is, when its file has none near.\n'
     '    void emusuni_mark(const Inflating &s) {\n'
     '        if (s.position < EMUSUNI_MARK_EVERY || m_marks.size() >= EMUSUNI_MARKS_MOST || !s.state->pRead_buf || !s.state->pWrite_buf)\n'
     '            return;\n'
     '        for (const auto &m : m_marks) {\n'
     '            if (m->index == s.index && m->position + EMUSUNI_MARK_EVERY > s.position && m->position < s.position + EMUSUNI_MARK_EVERY / 2)\n'
     '                return;\n'
     '        }\n'
     '        std::unique_ptr<EmusuniMark> mark(new EmusuniMark);\n'
     '        mark->index = s.index;\n'
     '        mark->position = s.position;\n'
     '        mark->state = *s.state;\n'
     '        memcpy(mark->written, s.state->pWrite_buf, TINFL_LZ_DICT_SIZE);\n'
     '        mark->state.cur_file_ofs -= mark->state.read_buf_avail;\n'
     '        mark->state.comp_remaining += mark->state.read_buf_avail;\n'
     '        mark->state.read_buf_avail = 0;\n'
     '        mark->state.read_buf_ofs = 0;\n'
     '        mark->state.pRead_buf = nullptr;\n'
     '        mark->state.pWrite_buf = nullptr;\n'
     '        m_marks.push_back(std::move(mark));\n'
     '    }\n'
     '\n'
     '    // A stream that goes on from a mark.\n'
     '    mz_zip_reader_extract_iter_state *emusuni_from(const EmusuniMark &mark) {\n'
     '        mz_zip_reader_extract_iter_state *to = mz_zip_reader_extract_iter_new(&m_zip, mark.index, 0);\n'
     '        if (!to)\n'
     '            return nullptr;\n'
     '        void *const read_buf = to->pRead_buf;\n'
     '        void *const write_buf = to->pWrite_buf;\n'
     '        if (!read_buf || !write_buf || to->read_buf_size != mark.state.read_buf_size) {\n'
     '            mz_zip_reader_extract_iter_free(to);\n'
     '            return nullptr;\n'
     '        }\n'
     '        *to = mark.state;\n'
     '        to->pRead_buf = read_buf;\n'
     '        to->pWrite_buf = write_buf;\n'
     '        memcpy(write_buf, mark.written, TINFL_LZ_DICT_SIZE);\n'
     '        return to;\n'
     '    }\n'
     '\n'
     '    bool emusuni_inflate_at(mz_uint index, std::uint64_t at, std::uint8_t *out, std::size_t size) {\n'
     '        // The stream of this file that is the furthest in without being past the place asked for, and the mark.\n'
     '        auto s = m_streams.end();\n'
     '        for (auto i = m_streams.begin(); i != m_streams.end(); ++i) {\n'
     '            if (i->index == index && i->position <= at && (s == m_streams.end() || i->position > s->position))\n'
     '                s = i;\n'
     '        }\n'
     '        const EmusuniMark *mark = nullptr;\n'
     '        for (const auto &m : m_marks) {\n'
     '            if (m->index == index && m->position <= at && (!mark || m->position > mark->position))\n'
     '                mark = m.get();\n'
     '        }\n'
     '        mz_zip_reader_extract_iter_state *state = nullptr;\n'
     '        std::uint64_t from = 0;\n'
     '        // (a mark, when it saves a MiB or more over the stream there is)\n'
     '        if (mark && (s == m_streams.end() || mark->position >= s->position + (1ull << 20))) {\n'
     '            state = emusuni_from(*mark);\n'
     '            if (state) {\n'
     '                from = mark->position;\n'
     '                emusuni_vita_stat_add(26, 0);\n'
     '            }\n'
     '        }\n'
     '        if (!state && s == m_streams.end()) {\n'
     '            state = mz_zip_reader_extract_iter_new(&m_zip, index, 0);\n'
     '            if (!state)\n'
     '                return false;\n'
     '            emusuni_vita_stat_add(25, 0);\n'
     '        }\n'
     '        if (state) {\n'
     '            // (six streams at a time: the one used longest ago goes)\n'
     '            if (m_streams.size() >= 6) {\n'
     '                mz_zip_reader_extract_iter_free(m_streams.front().state);\n'
     '                m_streams.erase(m_streams.begin());\n'
     '            }\n'
     '            m_streams.push_back({ index, state, from });\n'
     '        } else if (std::next(s) != m_streams.end()) {\n'
     '            std::rotate(s, std::next(s), m_streams.end());\n'
     '        }\n'
     '        s = std::prev(m_streams.end());\n'
     '        std::vector<std::uint8_t> skip;\n'
     '        std::uint64_t skipped = 0;\n'
     '        while (s->position < at) {\n'
     '            skip.resize(static_cast<std::size_t>(std::min<std::uint64_t>(at - s->position, 1 << 20)));\n'
     '            const std::size_t n = mz_zip_reader_extract_iter_read(s->state, skip.data(), skip.size());\n'
     '            if (n == 0)\n'
     '                return false;\n'
     '            s->position += n;\n'
     '            skipped += n;\n'
     '            emusuni_mark(*s);\n'
     '        }\n'
     '        if (skipped != 0)\n'
     '            emusuni_vita_stat_add(24, skipped);\n'
     '        std::size_t done = 0;\n'
     '        while (done < size) {\n'
     '            const std::size_t n = mz_zip_reader_extract_iter_read(s->state, out + done, size - done);\n'
     '            if (n == 0)\n'
     '                return false;\n'
     '            done += n;\n'
     '            s->position += n;\n'
     '        }\n'
     '        emusuni_mark(*s);\n'
     '        return true;\n'
     '    }\n'
     '#endif\n'
     '\n'
     '    bool inflate_at(mz_uint index, std::uint64_t at, std::uint8_t *out, std::size_t size) {\n'
     '#ifdef EMUSUNI_PORT\n'
     '        if (emusuni_vita_switch(25))\n'
     '            return emusuni_inflate_at(index, at, out, size);\n'
     '#endif\n'),
    ('    FILE *m_file = nullptr;\n    mz_zip_archive m_zip{};\n',
     '#ifdef EMUSUNI_PORT\n'
     '    static constexpr std::uint64_t EMUSUNI_MARK_EVERY = 4ull << 20;\n'
     '    static constexpr std::size_t EMUSUNI_MARKS_MOST = 320;\n'
     '    std::vector<std::unique_ptr<EmusuniMark>> m_marks; // (the "zipmarks" switch)\n'
     '#endif\n'
     '    FILE *m_file = nullptr;\n    mz_zip_archive m_zip{};\n'),
    ('        const uint8_t *key = nullptr;\n        switch (byte_swap(ext.data_type2) & 7) {\n',
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '        if (emusuni_vita_keys_missing("a package (.pkg)")) {\n'
     '            error = "the PS Vita\'s keys file is missing";\n'
     '            return false;\n'
     '        }\n'
     '#endif\n'
     '        const uint8_t *key = nullptr;\n        switch (byte_swap(ext.data_type2) & 7) {\n'),
    ('    if (has_pfs) {\n        const auto lic = decode_license_np(zrif);\n',
     '    if (has_pfs) {\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '        if (emusuni_vita_keys_missing("an encrypted game (NoNpDrm)")) {\n'
     '            error = "the PS Vita\'s keys file is missing";\n'
     '            return nullptr;\n'
     '        }\n'
     '#endif\n'
     '        const auto lic = decode_license_np(zrif);\n'),
    # (After 01.47) A file read whole before the game has opened it - the executable sceAppMgrLoadExec names, and
    # so the one the content is loaded again with -: it was decrypted in one piece, which only a file of a single
    # signature block allows, so a larger one (Uncharted's 4.6 MiB uncharted.self) ended in "could not decrypt"
    # and was left as its placeholder. Block by block where the image allows it, as the files of the start are.
    ('    if (m->materialize(p))\n',
     '    if (m->streamable(p) ? m->stream_to_place(p) : m->materialize(p))\n'),
])
patch('vita3k/regmgr/src/regmgr.cpp', [
    ('static std::string decryptRegistryFile(const fs::path &reg_path) {\n',
     'static std::string decryptRegistryFile(const fs::path &reg_path) {\n'
     '#ifdef EMUSUNI_VITA_NOKEYS\n'
     '    if (emusuni_vita_keys_missing("the system\'s registry"))\n'
     '        return {}; // (as without a firmware: an empty registry)\n'
     '#endif\n'),
], tables=[
    ('xorKey', 'extern "C" int emusuni_vita_keys_missing(const char *what);\n'
               'extern "C" unsigned char emusuni_vita_registry_xor[16];\n'
               'static const unsigned char *const xorKey = emusuni_vita_registry_xor;'),
])
patch('external/psvpfstools/psvpfsparser/PfsKeys.cpp', [], repo='external/psvpfstools/psvpfsparser', tables=[
    ('hmac_key0', 'unsigned char hmac_key0[0x14] = {0};'),
    ('hmac_key1', 'unsigned char hmac_key1[0x14] = {0};'),
    ('iv0', 'unsigned char iv0[0x10] = {0};'),
])
patch('external/psvpfstools/psvpfsparser/F00DNativeKeyEncryptor.cpp', [], repo='external/psvpfstools/psvpfsparser', tables=[
    ('contract_key0', 'unsigned char contract_key0[0x10] = {0};'),
])
patch('external/psvpfstools/psvpfsparser/LocalKeys.cpp', [], repo='external/psvpfstools/psvpfsparser', tables=[
    ('sealedkey_retail_key', 'std::uint8_t sealedkey_retail_key[0x10] = {0};'),
    ('keystone_hmac_secret', 'std::uint8_t keystone_hmac_secret[0x20] = {0};'),
    ('passcode_hmac_secret', 'std::uint8_t passcode_hmac_secret[0x20] = {0};'),
    ('PFS_EncKey', 'std::uint8_t PFS_EncKey[0x10] = {0};'),
])

# (After 01.68) Nothing else of the tree differs from its commit. The patches above are written over the files
# this script patches, each from the commit's text; a file that ANOTHER version of this script patched and this
# one doesn't stayed as that version left it, and was compiled: 01.68's PS Vita object had four files of the
# specialist's work in progress in it (its working copy had built from this same tree an hour before). So: every
# file of the tree and of its submodules that differs from its commit and isn't one of this script's is put
# back as the commit has it, and said; and if any is left after that, the script ends here.
def changed(repo):
    run = subprocess.run(['git', '-C', root + ('/' + repo if repo else ''), 'status', '--porcelain', '--ignore-submodules=all'],
                         capture_output=True)
    if run.returncode != 0:
        sys.exit("Vita3K: git status failed in %s/%s" % (root, repo))
    files = []
    for line in run.stdout.decode('utf-8').splitlines():
        if line[:2] == '??' or len(line) < 4:
            continue  # (a file git doesn't know: nothing of the commit's)
        name = line[3:]
        files.append(name.split(' -> ')[-1].strip('"'))
    return files
run = subprocess.run(['git', '-C', root, 'submodule', 'foreach', '--recursive', '--quiet', 'echo $displaypath'],
                     capture_output=True)
if run.returncode != 0:
    sys.exit("Vita3K: the submodules of %s can't be listed" % root)
REPOS = [''] + [line for line in run.stdout.decode('utf-8').splitlines() if line]
def strays():
    found = []
    for repo in REPOS:
        mine = PATCHED.get(repo, set())
        found += [(repo, name) for name in changed(repo) if name not in mine]
    return found
LOOK_ONLY = 'DRY' in globals()  # (the block run dry by a test tool, which gives it that name: nothing is written)
for repo, name in strays():
    if LOOK_ONLY:
        print("Vita3K: %s differs from the commit and isn't one of this script's files" % ((repo + '/' if repo else '') + name))
        continue
    back = subprocess.run(['git', '-C', root + ('/' + repo if repo else ''), 'checkout', 'HEAD', '--', name], capture_output=True)
    print("Vita3K: %s differed from the commit and isn't one of this script's files: put back as the commit has it%s"
          % ((repo + '/' if repo else '') + name, '' if back.returncode == 0 else ' - FAILED'))
left = [] if LOOK_ONLY else strays()
if left:
    sys.exit("Vita3K: the tree isn't as this script leaves it: %s"
             % ', '.join((repo + '/' if repo else '') + name for repo, name in left))
PATCH

# The libretro API, renamed for every source of the core.
API="retro_set_environment retro_set_video_refresh retro_set_audio_sample retro_set_audio_sample_batch
  retro_set_input_poll retro_set_input_state retro_init retro_deinit retro_api_version retro_get_system_info
  retro_get_system_av_info retro_set_controller_port_device retro_reset retro_run retro_serialize_size
  retro_serialize retro_unserialize retro_cheat_reset retro_cheat_set retro_load_game retro_load_game_special
  retro_unload_game retro_get_region retro_get_memory_data retro_get_memory_size"
# A build without Sony's keys (VITA_CORE_NOKEYS=1): the SHA-256 the player's file is taken by is that of the keys
# as this source has them (scripts/make-vita-keys.py; they aren't written anywhere here), for the core and, in
# <out>/vita-keys.sha256, for the app.
NOKEYS="${VITA_CORE_NOKEYS:-0}"
if [ "$NOKEYS" = 1 ]; then
  KEYS_SHA256="$(python3 -I "$HERE/make-vita-keys.py" --sha256 "$SRC")"
  if ! [[ "$KEYS_SHA256" =~ ^[0-9a-f]{64}$ ]]; then
    echo "Vita3K: scripts/make-vita-keys.py gave no SHA-256 for the keys of $SRC"
    exit 1
  fi
  printf '%s\n' "$KEYS_SHA256" > "$OUT/vita-keys.sha256"
else
  rm -f "$OUT/vita-keys.sha256"
fi
{
  echo "/* Made by scripts/build-vita-core.sh: the core's libretro API, renamed for EmuSuNi. */"
  for f in $API; do echo "#define $f vita3k_$f"; done
  if [ "$NOKEYS" = 1 ]; then
    echo "/* No key of Sony's inside: they come from the player's file, taken by this SHA-256. */"
    echo "#define EMUSUNI_VITA_NOKEYS 1"
    echo "#define EMUSUNI_VITA_KEYS_SHA256 \"$KEYS_SHA256\""
  fi
} > "$OUT/prefix_vita3k.h.new"
if cmp -s "$OUT/prefix_vita3k.h.new" "$OUT/prefix_vita3k.h"; then
  rm -f "$OUT/prefix_vita3k.h.new"
else
  mv "$OUT/prefix_vita3k.h.new" "$OUT/prefix_vita3k.h"
fi

if [ "$HOST" != 1 ]; then
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  PS4C="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -funwind-tables -isysroot $OPENORBIS"
  PS4C+=" -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay -isystem $OPENORBIS/include -I$OPENORBIS/usr/include"
fi
: > "$LOG"

# --- FFmpeg 7.1 for the PS4 (the PC build keeps the Linux libraries Vita3K downloads): Vita3K needs
# 5.1 or later (AVChannelLayout); its own builds are 7.1 with the decoders, demuxers and parsers
# below (external/ffmpeg/ffmpeg.patch), here without the encoders and the muxer (video dumps), the
# filters and the devices. From the release tarball (ffmpeg.org, sha256 40973d44970dbc83...), in
# ~/vita-port/src/ffmpeg-7.1 (VITA_FFMPEG_DIR). No threads, no external libraries. Its symbols stay
# local to the core's object, so PPSSPP's FFmpeg 3.0.2 and this one never meet.
FF_PREFIX="$OUT/ffmpeg"
if [ "$HOST" != 1 ]; then
  FF_SRC="${VITA_FFMPEG_DIR:-$HOME/vita-port/src/ffmpeg-7.1}"
  FF_BUILD="$OUT/ffmpeg-build"
  if [ "$(cat "$FF_SRC/RELEASE" 2>/dev/null)" != "7.1" ]; then
    echo "Vita3K: $FF_SRC isn't FFmpeg 7.1 (https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz, unpacked there)"
    exit 1
  fi
  FF_CFLAGS="-O3 -march=btver2 -fno-unwind-tables -fno-asynchronous-unwind-tables"
  FF_OPTS=(
    --prefix="$FF_PREFIX" --enable-static --disable-shared --enable-pic --arch=x86_64
    --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-network
    --disable-autodetect --disable-everything --disable-pthreads --disable-w32threads --disable-os2threads
    --disable-x86asm --disable-debug
    --enable-decoder=aac,aac_latm,atrac3,atrac3p,atrac9,mp3,pcm_s16le,pcm_s8,h264,mpeg4,mpeg2video,mjpeg,mjpegb
    --enable-demuxer=h264,m4v,mp3,mpegvideo,mpegps,mjpeg,mov,avi,aac,pmp,oma,pcm_s16le,pcm_s8,wav
    --enable-parser=h264,mpeg4video,mpegaudio,mpegvideo,mjpeg,aac,aac_latm
    --enable-protocol=file --enable-bsf=mjpeg2jpeg
    --enable-cross-compile --target-os=freebsd --cc=clang-21 --cxx=clang++-21 --ar=llvm-ar-21
    --ranlib=llvm-ranlib-21 --nm=llvm-nm-21 --strip=llvm-strip-21 "--extra-cflags=$PS4C $FF_CFLAGS"
    "--extra-ldflags=--target=x86_64-pc-freebsd12-elf -fuse-ld=lld -nostdlib -Wl,-pie -Wl,--eh-frame-hdr -Wl,--script=$OPENORBIS/link.x -L$OPENORBIS/lib -L$OPENORBIS/usr/lib"
    "--extra-libs=-lc -lclang_rt.builtins-x86_64 -lkernel -lSceLibcInternal"
  )
  FF_STAMP="$FF_PREFIX/.emusuni-flags"
  # (The last word: the patch below, so that a change of it builds FFmpeg again.)
  FF_WANT="7.1 | ${FF_OPTS[*]} | transforms up to 65536"
  [ "$NOKEYS" = 1 ] && FF_WANT+=" | no OpenMG keys"
  if [ "${2:-}" = clean ] || [ "$(cat "$FF_STAMP" 2>/dev/null)" != "$FF_WANT" ] || [ ! -f "$FF_PREFIX/lib/libavcodec.a" ]; then
    echo "Vita3K: building FFmpeg 7.1 (new flags or clean)"
    # A copy of the sources with one change (the tarball's stay as they came): no transform tables above
    # 65536 points. libavutil keeps one table for each power of two up to 2097152, for floats, doubles and
    # integers, all in the .bss: 15.5 MiB the PS4 would charge the app at its start, game or no game, for
    # sizes no decoder here asks for (the audio ones stay under 8192).
    FF_PATCHED="$OUT/ffmpeg-src"
    rm -rf "$FF_BUILD" "$FF_PREFIX" "$FF_PATCHED"
    cp -a "$FF_SRC" "$FF_PATCHED"
    python3 -I - "$FF_PATCHED/libavutil/tx_template.c" <<'FFPATCH'
import sys
path = sys.argv[1]
with open(path, encoding='utf-8', newline='') as f:
    text = f.read()
for size in ('131072', '262144', '524288', '1048576', '2097152'):
    table = [line for line in text.split('\n') if line.strip().rstrip('\\').strip() == 'SR_TABLE(%s)' % size]
    codelet = [line for line in text.split('\n') if line.startswith('DECL_SR_CODELET(%s,' % size)]
    listed = [line for line in text.split('\n') if line.strip() == '&TX_NAME(ff_tx_fft%s_ns_def),' % size]
    if len(table) != 1 or len(codelet) != 1 or len(listed) != 1:
        sys.exit("FFmpeg: libavutil/tx_template.c isn't as expected for the %s-point transform" % size)
    for line in (table[0], codelet[0], listed[0]):
        text = text.replace(line + '\n', '', 1)
with open(path, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
FFPATCH
    # A build without Sony's keys: FFmpeg has one table of them too, in its demuxer of Sony's OMA audio files
    # (the keys of OpenMG, for files with that protection: none of a Vita game's). Zeros here, found by its name:
    # such a file is then refused as one whose key isn't known; OMA files without protection play as before.
    if [ "$NOKEYS" = 1 ]; then
      python3 -I - "$FF_PATCHED/libavformat/omadec.c" <<'FFKEYS'
import re
import sys
path = sys.argv[1]
with open(path, encoding='utf-8', newline='') as f:
    text = f.read()
found = re.findall(r'static const uint64_t leaf_table\[\] = \{[^{};]*\};', text)
if len(found) != 1:
    sys.exit("FFmpeg: libavformat/omadec.c isn't as expected (its leaf_table)")
count = len(re.findall(r'0x[0-9a-fA-F]+', found[0]))
if count < 2 or count % 2:
    sys.exit("FFmpeg: libavformat/omadec.c isn't as expected (its leaf_table has %d numbers)" % count)
text = text.replace(found[0], 'static const uint64_t leaf_table[%d] = { 0 };' % count)
with open(path, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
FFKEYS
    fi
    mkdir -p "$FF_BUILD"
    if ! (cd "$FF_BUILD" && env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS -u CC -u CXX \
            "$FF_PATCHED/configure" "${FF_OPTS[@]}") >> "$LOG" 2>&1; then
      tail -30 "$LOG"
      tail -30 "$FF_BUILD/ffbuild/config.log" 2>/dev/null || true
      echo "Vita3K: configuring FFmpeg failed (see $LOG)"
      exit 1
    fi
    if ! env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS make -C "$FF_BUILD" -j "$JOBS" install >> "$LOG" 2>&1; then
      grep -E "error:|Error " "$LOG" | head -30
      echo "Vita3K: building FFmpeg failed (see $LOG)"
      exit 1
    fi
    printf '%s' "$FF_WANT" > "$FF_STAMP"
  fi
  # One header FFmpeg doesn't install and Vita3K's AAC decoder reads (FFCodec, for its decode
  # callback): Vita3K's own copy of 7.1's, which is the release's without its config.h lines.
  cmp -s "$SRC/external/ffmpeg/include/libavcodec/codec_internal.h" "$FF_PREFIX/include/libavcodec/codec_internal.h" ||
    cp "$SRC/external/ffmpeg/include/libavcodec/codec_internal.h" "$FF_PREFIX/include/libavcodec/codec_internal.h"
fi

# -O2 and the PS4's CPU, as the 3DS core (on the PC without AMD's SSE4A, which Intel CPUs lack).
FLAGS_CORE="-O2 -DNDEBUG -DEMUSUNI_PORT=1 -march=btver2 -include $OUT/prefix_vita3k.h"
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a"
# Mbed TLS 2.16.6's sources, those scripts/build-mbedtls.sh unpacks for the app (its AES and SHA only).
MBED_SRC="${EMUSUNI_BUILD:-$HOME/emusuni-build}/mbedtls/src"
if [ ! -f "$MBED_SRC/library/aes.c" ]; then
  echo "Vita3K: $MBED_SRC has no Mbed TLS sources (scripts/build-mbedtls.sh <its parent> unpacks them)"
  exit 1
fi
OPTIONS=(
  "-DEMUSUNI_PORT_DIR=$PROJECT/port" "-DEMUSUNI_MBEDTLS_DIR=$MBED_SRC"
  -DBUILD_LIBRETRO=ON -DEMUSUNI_PORT=ON -DUSE_DISCORD_RICH_PRESENCE=OFF
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DVITA3K_FORCE_CUSTOM_BOOST=ON
  # (Boost's headers by their path: the PS4 toolchain looks for packages in its own root only.)
  "-DBoost_INCLUDE_DIR=$SRC/external/boost"
  # No LTO: the PS4's linker (ld.lld 13) can't read clang 21's bitcode, and the object is one already.
  -DUSE_LTO=NEVER
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CORE"
)
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  }
else
  OPTIONS+=("-DEMUSUNI_FFMPEG_DIR=$FF_PREFIX")
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
WANT="$SRC | $TREE_COMMIT | host $HOST | no keys $NOKEYS | ${OPTIONS[*]} | $(sed -n '1,/^# The checks\.$/p' "${BASH_SOURCE[0]}" | sha256sum | cut -d' ' -f1)"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "Vita3K: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! CMAKE_POLICY_VERSION_MINIMUM=3.5 configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    grep -E "CMake Error|error:" -A6 "$LOG" | head -80
    echo "Vita3K: configuring failed (see $LOG)"
    exit 1
  fi
  printf '%s' "$WANT" > "$STAMP"
fi
# (-k 0: every error at once, not only the first.)
if ! ninja -C "$BUILD" -j "$JOBS" -k 0 vita3k_libretro >> "$LOG" 2>&1; then
  grep -E "error:" "$LOG" | sed 's/^.*vita3k-libretro\///' | sort | uniq -c | sort -rn | head -80
  echo "Vita3K: build failed (see $LOG): $(grep -c "^FAILED:" "$LOG") targets failed"
  exit 1
fi

# One object: the core's own library whole, everything else as far as referenced; only the renamed
# libretro API stays global.
mapfile -t OWN < <(find "$BUILD" -name 'vita3k_libretro.a' -not -path '*/CMakeFiles/*')
mapfile -t DEPS < <(find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -not -name 'vita3k_libretro.a' | sort)
# (The PS4's FFmpeg is outside the build directory.)
[ "$HOST" = 1 ] || DEPS+=("$FF_PREFIX"/lib/*.a)
if [ "${#OWN[@]}" != 1 ]; then
  echo "Vita3K: expected one vita3k_libretro.a, found ${#OWN[@]}"
  exit 1
fi
# (--wrap: the core's calls of __cxa_throw go to port/ps4_vita_throw.cpp's __wrap___cxa_throw, which logs
# the exception and throws it with the runtime's own.)
# (And the calls that would reach the network go to port/ps4_vita_nonet.c's __wrap_<name>, which fail: NONET.)
NONET="connect send sendto bind listen accept getaddrinfo gethostbyname gethostbyaddr"
WRAPS=()
for f in $NONET; do WRAPS+=("--wrap=$f"); done
# ((01.30) And the C++ library's file system entry points go to port/ps4_vita_files.cpp's: FSWRAP. On the PS4
# every one the core calls that takes an error_code* - the checks stop at one that isn't in this list; on the
# PC, whose C++ library has other names, remove_all and remove alone, so that the tests delete with that file.)
if [ "$HOST" = 1 ]; then
  FSWRAP="_ZNSt10filesystem10remove_allERKNS_7__cxx114pathE
_ZNSt10filesystem10remove_allERKNS_7__cxx114pathERSt10error_code
_ZNSt10filesystem6removeERKNS_7__cxx114pathE
_ZNSt10filesystem6removeERKNS_7__cxx114pathERSt10error_code"
else
  FSWRAP="_ZNSt3__14__fs10filesystem12__remove_allERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem8__removeERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem8__statusERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem11__file_sizeERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem13__fs_is_emptyERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem18__create_directoryERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem20__create_directoriesERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem17__last_write_timeERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem7__spaceERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem10__absoluteERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem18__weakly_canonicalERKNS1_4pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem12__equivalentERKNS1_4pathES4_PNS_10error_codeE
_ZNSt3__14__fs10filesystem8__renameERKNS1_4pathES4_PNS_10error_codeE
_ZNSt3__14__fs10filesystem11__copy_fileERKNS1_4pathES4_NS1_12copy_optionsEPNS_10error_codeE
_ZNSt3__14__fs10filesystem6__copyERKNS1_4pathES4_NS1_12copy_optionsEPNS_10error_codeE
_ZNSt3__14__fs10filesystem13__resize_fileERKNS1_4pathEmPNS_10error_codeE
_ZNSt3__14__fs10filesystem14__current_pathEPNS_10error_codeE
_ZNSt3__14__fs10filesystem18directory_iteratorC2ERKNS1_4pathEPNS_10error_codeENS1_17directory_optionsE
_ZNSt3__14__fs10filesystem18directory_iterator11__incrementEPNS_10error_codeE
_ZNSt3__14__fs10filesystem28recursive_directory_iteratorC2ERKNS1_4pathENS1_17directory_optionsEPNS_10error_codeE
_ZNSt3__14__fs10filesystem28recursive_directory_iterator11__incrementEPNS_10error_codeE"
fi
for f in $FSWRAP; do WRAPS+=("--wrap=$f"); done
ld.lld-21 -r --wrap=__cxa_throw "${WRAPS[@]}" --whole-archive "${OWN[@]}" --no-whole-archive --start-group "${DEPS[@]}" \
  --end-group -o "$OUT/vita_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='vita3k_retro_*' "$OUT/vita_core_all.o" "$OUT/vita_core.o.new"
rm -f "$OUT/vita_core_all.o"

# The checks.
# (After 01.71) Two more of the object's names stay for the app to take, besides the renamed libretro API: the
# pictures a game showed, for the app's FPS counter (port/ps4_vita_stats.cpp). They are made global again here,
# below the line the build's stamp is taken up to, so that this step can change without everything compiling again.
APP_GLOBALS="emusuni_vita_pictures_per_second emusuni_vita_pictures_total"
# (The list whole into a variable first, as further down: a grep that stops at its first match ends the pipe early.)
OWN_NAMES=$(llvm-nm-21 --defined-only "$OUT/vita_core.o.new" | awk '{print $3}')
for f in $APP_GLOBALS; do
  if ! grep -qx "$f" <<< "$OWN_NAMES"; then
    echo "Vita3K: the object has no $f for the app"
    exit 1
  fi
done
# shellcheck disable=SC2046
llvm-objcopy-21 $(for f in $APP_GLOBALS; do echo "--globalize-symbol=$f"; done) "$OUT/vita_core.o.new"
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/vita_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$({ for f in $API; do echo "vita3k_$f"; done; for f in $APP_GLOBALS; do echo "$f"; done; } | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "Vita3K: the object's global symbols aren't exactly the renamed libretro API and the app's two names:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
# (A weak symbol nothing defines: the PS4's executable maker refuses the app for it.)
WEAK=$(llvm-nm-21 -u "$OUT/vita_core.o.new" | awk '$1 == "w" || $1 == "v" {print $2}')
if [ -n "$WEAK" ]; then
  echo "Vita3K: the object asks for weak symbols nothing defines (create-fself would refuse the app):"
  echo "$WEAK"
  exit 1
fi
if ! llvm-nm-21 "$OUT/vita_core.o.new" | grep " t __wrap___cxa_throw$" > /dev/null ||
   llvm-nm-21 -u "$OUT/vita_core.o.new" | grep "__wrap___cxa_throw\|__real___cxa_throw" > /dev/null; then
  echo "Vita3K: the object's throws don't go through port/ps4_vita_throw.cpp: they wouldn't be logged"
  exit 1
fi
# (No way out to the network: the object asks the system for none of the calls that would connect, send, take
# a connection or ask a name server - each is port/ps4_vita_nonet.c's -, and no text in it names a server of
# Sony's or a short link.)
# (Each list whole into a variable first: a grep that stops at its first match would end the pipe early, which
# this script's pipefail takes for a failure.)
ASKED=$(llvm-nm-21 -u "$OUT/vita_core.o.new" | awk '{print $2}')
OWN_TEXT=$(llvm-nm-21 --defined-only "$OUT/vita_core.o.new" | awk '$2 == "t" {print $3}')
for f in $NONET; do
  if grep -qx -e "$f" -e "__wrap_$f" -e "__real_$f" <<< "$ASKED" || ! grep -qx "__wrap_$f" <<< "$OWN_TEXT"; then
    echo "Vita3K: the object's $f doesn't go to port/ps4_vita_nonet.c: a game could reach the network"
    exit 1
  fi
done
# ((01.30) The file system: each of FSWRAP's entry points is port/ps4_vita_files.cpp's in the object, and
# remove_all and remove aren't asked of the C++ library at all - its remove_all ended the app on the PS4. On the
# PS4, besides, the object asks the C++ library for no file system call with an error_code* that isn't in the
# list: one new to the core would throw from inside the library, where nothing sees it.)
for f in $FSWRAP; do
  if grep -qx -e "__wrap_$f" -e "__real_$f" <<< "$ASKED" || ! grep -qx "__wrap_$f" <<< "$OWN_TEXT"; then
    echo "Vita3K: the object's $f doesn't go to port/ps4_vita_files.cpp"
    exit 1
  fi
done
GONE=$(grep -E 'filesystem(10|12__)remove_all|filesystem(6|8__)remove|^(fdopendir|openat|unlinkat)$' <<< "$ASKED" || true)
if [ -n "$GONE" ]; then
  echo "Vita3K: the object still asks the C++ library to delete (port/ps4_vita_files.cpp's own was to), or asks"
  echo "for fdopendir, openat or unlinkat, which the PS4 doesn't have as the app's C library expects them:"
  echo "$GONE"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  UNGUARDED=$(grep -E '^_ZNSt3__14__fs10filesystem.*10error_codeE' <<< "$ASKED" | grep -vxF -f <(echo "$FSWRAP") || true)
  if [ -n "$UNGUARDED" ]; then
    echo "Vita3K: the object asks the C++ library for file system calls that port/ps4_vita_files.cpp doesn't guard"
    echo "(they would throw from inside the library; add each to that file and to FSWRAP):"
    echo "$UNGUARDED"
    exit 1
  fi
fi
NAMED=$(strings -n 6 "$OUT/vita_core.o.new" | grep -iE 'playstation\.(com|net)|sonyentertainmentnetwork|bit\.ly/' || true)
if [ -n "$NAMED" ]; then
  echo "Vita3K: the object names a server of Sony's or a short link:"
  echo "$NAMED"
  exit 1
fi
# (A build without Sony's keys: none of those of Vita3K's source is in the object - as bytes, as text, whole or
# in pieces of 8 bytes - nor, on the PS4, FFmpeg's OpenMG ones; the PC build links Vita3K's own FFmpeg libraries,
# which have those.)
if [ "$NOKEYS" = 1 ]; then
  KEY_SOURCES=("$SRC")
  [ "$HOST" = 1 ] || KEY_SOURCES+=("$FF_SRC")
  if ! python3 -I "$HERE/make-vita-keys.py" --scan "$OUT/vita_core.o.new" "${KEY_SOURCES[@]}"; then
    echo "Vita3K: the object was to have none of Sony's keys inside"
    exit 1
  fi
fi
if cmp -s "$OUT/vita_core.o.new" "$OUT/vita_core.o"; then
  rm -f "$OUT/vita_core.o.new"
else
  mv "$OUT/vita_core.o.new" "$OUT/vita_core.o"
fi
ls -la "$OUT/vita_core.o"
if [ "$HOST" = 1 ]; then
  # For the test frontend: the object with this PC's libraries (those upstream's shared object links).
  clang++-21 -shared --ld-path=/usr/bin/ld.lld-21 -o "$OUT/vita_core_test.so" "$OUT/vita_core.o" \
    -Wl,-z,defs -static-libgcc -static-libstdc++ -lz -lbrotlidec -lbrotlicommon -ldl -lm -lrt -lpthread
  ls -la "$OUT/vita_core_test.so"
fi
