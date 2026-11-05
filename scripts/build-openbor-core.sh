#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds the OpenBOR multi-engine libretro core (zanac/openbor-6391-libretro, ~/src/openbor-libretro,
# BSD-3-Clause like OpenBOR itself) for the PS4 into one object that scripts/build.sh links into
# EmuSuNi: OpenBOR, the beat 'em up engine whose games ("mods") are .pak files. The core holds several
# official OpenBOR releases and picks the one each .pak was made for (mods aren't compatible between
# engine versions); here two of them: v3.0 Build 6391 (the long-lived "stable" most mods were made with)
# and v4.0 Build 8025 (the newest). Not 8025_webm (WebM videos: 4 to 6 threads per video, libvpx) nor
# the older or in-between builds for now (see local/conocimiento/notas/openbor-prueba.md).
# Its own Makefile (platform=unix: -DLINUX, its zlib, libpng, libogg/libvorbis and libco) is run in a copy
# of the sources with the PS4 compiler and flags of toolchain/ps4-love-modern.cmake (as
# build-arcade-core.sh), -O2 (its own), the PS4's CPU (btver2), a section for each function, no unwind
# tables (C: nothing unwinds through it). Each engine is compiled on its own, linked into one relocatable
# object and every global symbol in it but its ob_engine_<build>() made local (the Makefile's own way: the
# engines share their names); the API of the common libretro layer becomes openbor_retro_*.
# Changes (applied each time to the copy, from the files as git has them at the pinned commit, checked):
#   - An engine can start again. The core started each engine once only (its statics: a second game on
#     the same engine needed the core to be loaded again, and here it is linked into the app). Each
#     engine's writable data is put in two sections of its own while it is linked (engine_r.ld.in:
#     .data.ob_<build> with .data, .data.rel and .data.rel.local, .bss.ob_<build> with .bss, each between
#     two symbols, ob_data_start_<build>... ; .data.rel.ro stays read-only), which the app's link puts inside
#     its .data and .bss; the common layer keeps a copy of the data from before the engine's first start
#     (~20 KB) and puts it back, the bss (~110 KB) zeroed, when the game is unloaded or the engine fails to
#     load it. (PC test: the same frames after two restarts, the heap back where it was.)
#   - The engine's coroutine stack (libco) is 2 MiB, not 16: the engines used 23-35 KiB of it in the PC
#     tests, and it comes from the app's heap while a game runs.
#   - The sprite queue (800-920 KB per engine) is made when the engine starts and given back when it
#     ends, not a static array: the PS4 charges the app's whole static memory from its start.
#   - (01.00, two engine bugs AddressSanitizer found with Super Mario Brawl, whose exit crashed on the PS4) 6391's
#     model weapon list is always the model's own, of its "weapons" line's size, as 8025 does (a subclassed model
#     wrote past its parent's list, over dlmalloc's bookkeeping); the scripts' numbered animation constants
#     ("ANI_FOLLOW41") are checked against their list's size, -1 past it (upstream read past it), in both
#     engines, and 6391's ANI_BLOCKPAIN / ANI_BACKBLOCKPAIN read the right lists, as 8025 does.
#   - The engines compiled with -fno-common (upstream: -fcommon): ld.lld -r leaves common symbols common,
#     and made local they would have no storage. 6391's two tentative definitions in a header are defined
#     once, in openbor.c.
#   - (PS4) sys/sysinfo.h, which FreeBSD lacks, for ram.c's -DLINUX branch: fixed figures (512 MiB, 256
#     free). They only show in the engine's log, its System Options menu and two script variants. No
#     mallinfo either (the out-of-memory message goes without the heap's size), and 8025's script date
#     functions take their POSIX branch.
# The object is checked at the end: only openbor_retro_* defined globally, no weak references left, no
# C++ operator new or delete, no static array of 1 MiB or more, no loaded section the PS4 link script doesn't
# place, both sections and their bounds for every engine, and every symbol it needs found in the PS4
# libraries the app links (OpenOrbis' libc, libkernel, libSceLibcInternal, clang's builtins).
# Only what changed is compiled again (make, with its own dependency files); everything when the flags,
# the engines or this script change.
#   build-openbor-core.sh <out dir> [clean]
#   OPENBOR_CORE_HOST=1 build-openbor-core.sh <out dir>   the same object for this PC (Linux, clang 21),
#                                                         for the PC tests (local/herramientas/prueba-openbor):
#                                                         no PS4 checks
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${OPENBOR_DIR:-$HOME/src/openbor-libretro}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/openbor-core.log"
HOST="${OPENBOR_CORE_HOST:-0}"
JOBS="${EMUSUNI_JOBS:-4}"
ENGINES="6391 8025"
PIN=933aed06e62e5a8e463123bc5bc328b141f14643

COMMIT="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo none)"
if [ "$COMMIT" != "$PIN" ]; then
  echo "OpenBOR: $SRC is not zanac/openbor-6391-libretro at $PIN (it is at $COMMIT):"
  echo "  git clone https://github.com/zanac/openbor-6391-libretro $SRC && git -C $SRC checkout $PIN"
  exit 1
fi

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
mkdir -p "$OUT/include/sys"
{
  echo "// Generated by scripts/build-openbor-core.sh: the OpenBOR core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f openbor_$f"; done
} > "$OUT/prefix_openbor.h.new"
cat > "$OUT/include/sys/sysinfo.h.new" <<'EOF'
/* Generated by scripts/build-openbor-core.sh (PS4): Linux's sysinfo for OpenBOR's ram.c (-DLINUX).
   Fixed figures: they only show in the engine's log, its System Options menu and two script variants. */
#pragma once
struct sysinfo { unsigned long totalram, freeram; unsigned int mem_unit; };
static inline int sysinfo(struct sysinfo *info)
{
    info->totalram = 512UL << 20;
    info->freeram = 256UL << 20;
    info->mem_unit = 1;
    return 0;
}
EOF
# Rewritten only when different: every source depends on them.
for f in "$OUT/prefix_openbor.h" "$OUT/include/sys/sysinfo.h"; do
  if cmp -s "$f.new" "$f"; then rm -f "$f.new"; else mv "$f.new" "$f"; fi
done

# The compiler as the Makefile runs it ($(CC)): the PS4 target and headers as toolchain/ps4-love-modern.cmake,
# the PS4's CPU, a section for each function (not for each variable: an engine's data must stay in the few
# sections engine_r.ld gathers), no unwind tables, the renamed API, no warnings in the log.
if [ "$HOST" = 1 ]; then
  # (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
  CC_CMD="clang-21 -fPIC -march=btver2 -mno-sse4a"
else
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  CC_CMD="clang-21 -target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  CC_CMD+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $OUT/include"
  CC_CMD+=" -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay -isystem $OPENORBIS/include -I$OPENORBIS/usr/include"
  CC_CMD+=" -march=btver2"
fi
CC_CMD+=" -w -ffunction-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/prefix_openbor.h"

STAMP="$OUT/.emusuni-flags"
# (This script is in it: a changed patch or flag compiles everything again; make alone would not see a
# changed flag in an engine.mk.)
WANT="$SRC | $COMMIT | host $HOST | $CC_CMD | engines $ENGINES | $(sha256sum "${BASH_SOURCE[0]}" | cut -d" " -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/src/Makefile" ]; then
  echo "OpenBOR: copying the sources and compiling everything (new flags, or clean)"
  rm -rf "$BUILD" "$STAMP"
  mkdir -p "$BUILD/src"
  git -C "$SRC" archive HEAD | tar -x -C "$BUILD/src"
fi

python3 -I - "$SRC" "$BUILD/src" <<'PATCH'
import os
import subprocess
import sys
root, copy = sys.argv[1], sys.argv[2]
patched = {}
def patch(path, old, new, count=1):
    # On the file as git has it (the pinned commit), so a changed patch replaces the old one.
    if path not in patched:
        patched[path] = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + path], check=True,
                                       capture_output=True).stdout.decode('latin-1')
    text = patched[path]
    assert text.count(old) == count, (path, text.count(old), old[:70])
    patched[path] = text.replace(old, new)

# Each engine's writable data in two sections of its own (see the top), with symbols at their bounds; the -r
# link by ld.lld; the list of the objects for this script's own link. The sections are named .data.ob_<build>
# and .bss.ob_<build>: the app's link script (OpenOrbis' link.x) puts them inside its .data and .bss. (00.99 named
# them ob_data_<build> / ob_bss_<build> and took the bounds from the linker's __start_/__stop_: sections the
# link script doesn't name land after .data and .bss, and create-fself's data segment is .data's bytes and
# .bss's end: their initial bytes were left out, the bss sections weren't mapped, and the app died at launch.)
patched['engine_r.ld.in'] = '''/* Generated by scripts/build-openbor-core.sh: engine @ID@'s -r link. Its writable data in two sections of
   its own, with symbols at their bounds, so that the common layer can put them back as they were before the
   engine's first start. .data.rel.ro is not writable after the relocations. */
SECTIONS
{
  .data.ob_@ID@ : { ob_data_start_@ID@ = .; *(.data) *(.data.rel) *(.data.rel.local) ob_data_end_@ID@ = .; }
  .bss.ob_@ID@  : { ob_bss_start_@ID@ = .; *(.bss) *(.bss.*) ob_bss_end_@ID@ = .; }
}
'''
patch('Makefile',
      '\t$$(CC) $(fpic) -nostdlib -r -Wl,-d -o $$@.tmp $$^\n'
      '\t$$(OBJCOPY) --keep-global-symbol=$(SYM_PREFIX)ob_engine_$(1) $$@.tmp $$@\n',
      '\tsed \'s/@ID@/$(1)/g\' $(CORE_DIR)/engine_r.ld.in > $$@.ld\n'
      '\t$$(LDR) -r -T $$@.ld -o $$@.tmp $$^\n'
      '\t$$(OBJCOPY) --keep-global-symbol=$(SYM_PREFIX)ob_engine_$(1) \\\n'
      '\t  --keep-global-symbol=ob_data_start_$(1) --keep-global-symbol=ob_data_end_$(1) \\\n'
      '\t  --keep-global-symbol=ob_bss_start_$(1) --keep-global-symbol=ob_bss_end_$(1) $$@.tmp $$@\n')
patch('Makefile',
      'all: $(TARGET)\n',
      'all: $(TARGET)\n\n'
      '# EmuSuNi: the objects only (scripts/build-openbor-core.sh links them itself), and their list.\n'
      'emusuni-objects: $(OBJECTS) $(ENGINE_OBJECTS)\n'
      'emusuni-print-objects:\n'
      '\t@echo $(OBJECTS) $(ENGINE_OBJECTS)\n')
# No common symbols: ld.lld -r leaves them common (it has no --define-common, as GNU ld's -r -d), and
# made local by the objcopy above they would have no storage. The engines were compiled with -fcommon for
# the old code's tentative definitions; only two of them were in a header (6391's openbor.h, so in several
# objects): defined once now, in openbor.c.
for e in ('6391', '8025'):
    patch('engines/%s/libretro/engine.mk' % e,
          'ENGINE_%s_FLAGS := -fcommon \\\n' % e,
          'ENGINE_%s_FLAGS := -fno-common \\\n' % e)
patch('engines/6391/openbor.h', 's_anim_list *anim_list;\n',
      'extern s_anim_list *anim_list;  /* EmuSuNi: defined in openbor.c (-fno-common) */\n')
patch('engines/6391/openbor.h', 's_modelcache *model_cache;\n',
      'extern s_modelcache *model_cache;  /* EmuSuNi: defined in openbor.c (-fno-common) */\n')
patch('engines/6391/openbor.c', 's_sprite_list *sprite_list;\n',
      's_anim_list *anim_list;  /* EmuSuNi: here, not in openbor.h (-fno-common) */\n'
      's_modelcache *model_cache;\n'
      's_sprite_list *sprite_list;\n')

# The common layer: restartable engines, a 2 MiB coroutine stack.
L = 'common/libretro.c'
patch(L, '#define ENGINE_STACK     (16 * 1024 * 1024)',
         '#define ENGINE_STACK     (2 * 1024 * 1024)  /* EmuSuNi: 16 MiB upstream; the engines use 23-35 KiB */')
ENGINE_IDS = ['3711', '4153', '6391', '7555', '7735', '8025', '8025_webm']
decl = ''.join('#ifdef HAVE_ENGINE_%s\nSNAP_DECL(%s)\n#endif\n' % (e, e) for e in ENGINE_IDS)
case = ''.join('#ifdef HAVE_ENGINE_%s\n    SNAP_CASE(%s)\n#endif\n' % (e, e) for e in ENGINE_IDS)
patch(L, 'static int engine_used[16];                /* engines already started (single use each) */\n',
      'static int engine_used[16];                /* engines already started (single use each) */\n'
      'static int engine_idx = -1;                /* EmuSuNi: the index of `engine` in engines[] */\n'
      '\n'
      '/* EmuSuNi: restartable engines. Each engine\'s writable data is in sections of its own\n'
      ' * (scripts/build-openbor-core.sh): .data.ob_<build> (.data, .data.rel, .data.rel.local) and\n'
      ' * .bss.ob_<build>, between the symbols ob_data_start_<build> / ob_data_end_<build> and\n'
      ' * ob_bss_start_<build> / ob_bss_end_<build>. A copy of the data is taken before the engine\'s first\n'
      ' * start; when its game is unloaded (or it fails to load one) the copy is put back and the bss zeroed:\n'
      ' * it can start again. */\n'
      '#define SNAP_DECL(id) extern char ob_data_start_##id[], ob_data_end_##id[], \\\n'
      '                                  ob_bss_start_##id[], ob_bss_end_##id[];\n'
      '#define SNAP_CASE(id) if (!strcmp(name, #id)) { *ds = ob_data_start_##id; *de = ob_data_end_##id; \\\n'
      '                                                 *bs = ob_bss_start_##id;  *be = ob_bss_end_##id; return 1; }\n'
      + decl +
      'static int snap_get(const char *name, char **ds, char **de, char **bs, char **be)\n'
      '{\n'
      + case +
      '    return 0;\n'
      '}\n'
      'static void *snap_copy[16];\n')
patch(L, 'static int start_engine(int idx, const char *pak, const char *root_dir, const char *paks_dir)\n'
         '{\n'
         '    static char engine_root[PATH_LEN];\n'
         '    unsigned t0;\n'
         '\n'
         '    engine = engines[idx].get();\n',
      '/* EmuSuNi: see snap_get */\n'
      'static void snap_save(int idx)\n'
      '{\n'
      '    char *ds, *de, *bs, *be;\n'
      '    if (snap_copy[idx] || !snap_get(engines[idx].name, &ds, &de, &bs, &be))\n'
      '        return;\n'
      '    snap_copy[idx] = malloc((size_t)(de - ds) + 1);\n'
      '    if (snap_copy[idx])\n'
      '        memcpy(snap_copy[idx], ds, (size_t)(de - ds));\n'
      '}\n'
      '\n'
      'static void snap_restore(int idx)\n'
      '{\n'
      '    char *ds, *de, *bs, *be;\n'
      '    if (idx < 0 || !snap_copy[idx] || !snap_get(engines[idx].name, &ds, &de, &bs, &be))\n'
      '        return;\n'
      '    memcpy(ds, snap_copy[idx], (size_t)(de - ds));\n'
      '    memset(bs, 0, (size_t)(be - bs));\n'
      '    engine_used[idx] = 0;\n'
      '    core_log(RETRO_LOG_INFO, "Engine %s can start again (%ld + %ld bytes put back)\\n",\n'
      '             engines[idx].name, (long)(de - ds), (long)(be - bs));\n'
      '}\n'
      '\n'
      'static int start_engine(int idx, const char *pak, const char *root_dir, const char *paks_dir)\n'
      '{\n'
      '    static char engine_root[PATH_LEN];\n'
      '    unsigned t0;\n'
      '\n'
      '    snap_save(idx);  /* EmuSuNi */\n'
      '    engine_idx = idx;\n'
      '    engine = engines[idx].get();\n')
patch(L, '        engine->video_free();\n'
         '        engine_state = ENGINE_NONE;\n'
         '        return 0;\n'
         '    }\n'
         '    return 1;\n'
         '}\n',
      '        engine->video_free();\n'
      '        engine_state = ENGINE_NONE;\n'
      '        snap_restore(idx);  /* EmuSuNi */\n'
      '        return 0;\n'
      '    }\n'
      '    return 1;\n'
      '}\n')
patch(L, '    engine_state = ENGINE_EXITED;\n'
         '    if (engine)\n'
         '        engine->video_free();\n'
         '}\n',
      '    engine_state = ENGINE_EXITED;\n'
      '    if (engine)\n'
      '        engine->video_free();\n'
      '    snap_restore(engine_idx);  /* EmuSuNi */\n'
      '}\n')

# The sprite queue on the heap while the engine runs (made at its start, given back at its end).
for e in ('6391', '8025'):
    patch('engines/%s/source/gamelib/spriteq.c' % e,
          'static qstruct queue[MAXQSPRITES];\n',
          '/* EmuSuNi: on the heap while the engine runs (spriteq_emusuni_alloc/free, from the libretro glue),\n'
          '   not in the static memory, which the PS4 charges in full from the app\'s start */\n'
          'static qstruct *queue;\n'
          'int spriteq_emusuni_alloc(void)\n'
          '{\n'
          '    if (!queue)\n'
          '        queue = (qstruct *)calloc(MAXQSPRITES, sizeof(qstruct));\n'
          '    return queue != NULL;\n'
          '}\n'
          'void spriteq_emusuni_free(void)\n'
          '{\n'
          '    free(queue);\n'
          '    queue = NULL;\n'
          '}\n')
    patch('engines/%s/source/gamelib/spriteq.c' % e,
          '#include <stdio.h>\n',
          '#include <stdio.h>\n#include <stdlib.h> /* EmuSuNi: calloc, free */\n')
    G = 'engines/%s/libretro/glue.c' % e
    patch(G, 'void borExit(int reset)\n{\n',
             'int spriteq_emusuni_alloc(void);  /* EmuSuNi: the sprite queue on the heap (spriteq.c) */\n'
             'void spriteq_emusuni_free(void);\n'
             '\n'
             'void borExit(int reset)\n{\n'
             '    spriteq_emusuni_free();\n')
    patch(G, '    setSystemRam();\n',
             '    if (!spriteq_emusuni_alloc())  /* EmuSuNi */\n'
             '    {\n'
             '        libretro_shutdown_message(1, "Not enough memory for the sprite queue");\n'
             '        borExit(1);\n'
             '    }\n'
             '    setSystemRam();\n')

# (PS4) No mallinfo in OpenOrbis' libc (the app's heap is its own dlmalloc): the out-of-memory message
# goes without the heap's size (6391) or with getUsedRam's (8025's own other branch).
patch('engines/6391/source/utils.c',
      '#ifndef WIN\n        writeToLogFile("Memory usage at exit: %u\\n", mallinfo().arena);\n',
      '#if !defined(WIN) && !defined(__ORBIS__) /* EmuSuNi: no mallinfo on the PS4 */\n'
      '        writeToLogFile("Memory usage at exit: %u\\n", mallinfo().arena);\n')
patch('engines/8025/source/utils.c',
      '#if LINUX && !DARWIN && !ANDROID\n        writeToLogFile("Memory usage at exit: %u\\n", mallinfo2().arena);\n',
      '#if LINUX && !DARWIN && !ANDROID && !defined(__ORBIS__) /* EmuSuNi: no mallinfo2 on the PS4 */\n'
      '        writeToLogFile("Memory usage at exit: %u\\n", mallinfo2().arena);\n')
# (PS4) The script date and time functions of 8025: FreeBSD has the POSIX ones it uses on Linux.
patch('engines/8025/source/openborscript/datetime.c',
      '#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)\n',
      '#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__ORBIS__) /* EmuSuNi */\n')

# (01.00) 6391: a model's weapon list is always its own, of its "weapons" line's size, as build 8025 does. A
# subclassed model had its parent's list (and a model kept a list from an earlier, shorter line): a longer line wrote
# past its end, over the heap's own bookkeeping - Super Mario Brawl, found by AddressSanitizer: on the PS4 dlmalloc
# then crashed freeing the models as the game ended (glibc's slack hid it on the PC). The parent's list isn't written
# into any more either.
patch('engines/6391/openbor.c',
      '    newchar->numweapons = weap;\n'
      '\n'
      '    if(!newchar->weapon)\n'
      '    {\n'
      '        newchar->weapon = malloc(sizeof(*newchar->weapon) * newchar->numweapons);\n'
      '        memset(newchar->weapon, 0xFF, sizeof(*newchar->weapon)*newchar->numweapons);\n'
      '        newchar->ownweapons = 1;\n'
      '    }\n',
      '    /* EmuSuNi: a list of its own, of this line\'s size, as build 8025 does (a subclassed model had its parent\'s:\n'
      '       a longer line wrote past its end, over the heap\'s bookkeeping) */\n'
      '    if(newchar->weapon && newchar->ownweapons && hasFreetype(newchar, MF_WEAPONS))\n'
      '    {\n'
      '        free(newchar->weapon);\n'
      '    }\n'
      '    newchar->numweapons = weap;\n'
      '    newchar->weapon = malloc(sizeof(*newchar->weapon) * newchar->numweapons);\n'
      '    memset(newchar->weapon, 0xFF, sizeof(*newchar->weapon)*newchar->numweapons);\n'
      '    newchar->ownweapons = 1;\n'
      '    newchar->freetypes |= MF_WEAPONS;\n')
# (01.00) The numbered animation constants of the scripts ("ANI_FOLLOW41": animfollows[40]): their number checked
# against the list's size, -1 (no animation: ent_set_anim refuses it) past it. Upstream read past the list - garbage
# on the PS4's heap, an animation id anything could index (Super Mario Brawl's scripts, AddressSanitizer). 6391's
# ANI_BLOCKPAIN / ANI_BACKBLOCKPAIN read the arrays blkpains / backblkpains through pointers scriptcommon.h declares
# for them (a wild address): the animblkpains / animbackblkpains lists, as build 8025 does.
ANIM_HELPER = (
    '/* EmuSuNi: the animation id a numbered script constant stands for, or -1 (no animation: ent_set_anim refuses it)\n'
    '   when its number is past its list (upstream read past it: garbage on the PS4\'s heap). */\n'
    'extern int max_downs, max_ups, max_backwalks, max_walks, max_idles, max_attack_types, max_freespecials,\n'
    '           max_follows, max_attacks;\n'
    'extern int *animblkpains, *animbackblkpains;\n'
    'static int emusuni_anim_count(const int *a)\n'
    '{\n'
    '    if(a == animdowns) return max_downs;\n'
    '    if(a == animups) return max_ups;\n'
    '    if(a == animbackwalks) return max_backwalks;\n'
    '    if(a == animwalks) return max_walks;\n'
    '    if(a == animidles) return max_idles;\n'
    '    if(a == animattacks) return max_attacks;\n'
    '    if(a == animfollows) return max_follows;\n'
    '    if(a == animspecials) return max_freespecials;\n'
    '    return max_attack_types;  /* the pains, falls, rises, deaths and block pains, the back ones too */\n'
    '}\n'
    'static int emusuni_anim_id(const int *a, int i)\n'
    '{\n'
    '    return a && i >= 0 && i < emusuni_anim_count(a) ? a[i] : -1;\n'
    '}\n'
    '\n')
C6 = 'engines/6391/source/openborscript/constants.c'
patch(C6, '#define ICMPSCONSTA(x, y) \\\n', ANIM_HELPER + '#define ICMPSCONSTA(x, y) \\\n')
patch(C6, '\tv.lVal = (LONG)(y[atoi(constname+(sizeof(#x)-1))-1]);\\\n',
          '\tv.lVal = (LONG)emusuni_anim_id(y, atoi(constname+(sizeof(#x)-1))-1);\\\n')
patch(C6, '\tv.lVal = (LONG)(y[atoi(constname+(sizeof(#x)-1))+STA_ATKS-1]);\\\n',
          '\tv.lVal = (LONG)emusuni_anim_id(y, atoi(constname+(sizeof(#x)-1))+STA_ATKS-1);\\\n')
patch(C6, '        ICMPSCONSTA(ANI_BLOCKPAIN, blkpains)\n',
          '        ICMPSCONSTA(ANI_BLOCKPAIN, animblkpains)  /* EmuSuNi: not blkpains (an array, not a pointer) */\n')
patch(C6, '        ICMPSCONSTA(ANI_BACKBLOCKPAIN, backblkpains)\n',
          '        ICMPSCONSTA(ANI_BACKBLOCKPAIN, animbackblkpains)  /* EmuSuNi: as 8025 */\n')
# (8025's lists are animation_id_t, and its block pains already the right lists.)
ANIM_HELPER_8025 = (ANIM_HELPER.replace('extern int *animblkpains, *animbackblkpains;\n', '')
                    .replace('const int *a', 'const animation_id_t *a')
                    .replace('static int emusuni_anim_id', 'static animation_id_t emusuni_anim_id')
                    .replace(': -1;', ': (animation_id_t)-1;'))
C8 = 'engines/8025/source/openborscript/constants.c'
patch(C8, '#define ICMPSCONSTA(x, y) \\\n', ANIM_HELPER_8025 + '#define ICMPSCONSTA(x, y) \\\n')
patch(C8, '\tv.ullVal = (uint64_t)(y[atoi(constname+(sizeof(#x)-1))-1]);\\\n',
          '\tv.ullVal = (uint64_t)emusuni_anim_id(y, atoi(constname+(sizeof(#x)-1))-1);\\\n')
patch(C8, '\tv.ullVal = (uint64_t)(y[atoi(constname+(sizeof(#x)-1))+STA_ATKS-1]);\\\n',
          '\tv.ullVal = (uint64_t)emusuni_anim_id(y, atoi(constname+(sizeof(#x)-1))+STA_ATKS-1);\\\n')

written = 0
for path, text in patched.items():
    full = os.path.join(copy, path)
    data = text.encode('latin-1')
    try:
        with open(full, 'rb') as f:
            same = f.read() == data
    except FileNotFoundError:
        same = False
    if not same:
        with open(full, 'wb') as f:
            f.write(data)
        written += 1
print("OpenBOR: patches applied (%d files changed)" % written)
PATCH

MAKE_ARGS=(-C "$BUILD/src" platform=unix ENGINES="$ENGINES" CC="$CC_CMD" OBJCOPY=llvm-objcopy-21 LDR=ld.lld-21
           OBJDIR="$BUILD/obj" GIT_VERSION="${PIN:0:7}")
# (The Makefile's own flags come after ours: -O2, -std=gnu99, its defines; the environment's CFLAGS,
# ps4vars.sh's, would be its starting point: cleared.)
if ! env -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS make -j "$JOBS" "${MAKE_ARGS[@]}" emusuni-objects >> "$LOG" 2>&1; then
  grep -E "error:|Error [0-9]|\*\*\*" "$LOG" | head -30
  echo "OpenBOR: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: only the libretro API (renamed) stays global. Replaced only when it changed (the app isn't
# linked again for nothing).
# shellcheck disable=SC2046
ld.lld-21 -r $(env -u CFLAGS make -s --no-print-directory "${MAKE_ARGS[@]}" emusuni-print-objects) -o "$OUT/openbor_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='openbor_retro_*' "$OUT/openbor_core_all.o" "$OUT/openbor_core.o.new"
rm -f "$OUT/openbor_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/openbor_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "openbor_$f"; done | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "OpenBOR: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
# Every section the program loads has a name the app's link script (OpenOrbis' link.x) puts in its output sections:
# one it doesn't name lands outside them, where create-fself's segments don't reach (00.99's ob_data_* / ob_bss_*).
SECTIONS=$(llvm-readelf-21 -S --wide "$OUT/openbor_core.o.new" | sed -n 's/^ *\[ *[0-9]*\] //p' | awk '$7 ~ /A/ {print $1}' | sort -u)
STRAY=$(echo "$SECTIONS" | grep -vE '^\.(text|rodata|data|bss|init_array|tdata|tbss)(\..*)?$|^\.eh_frame$' || true)
if [ -n "$STRAY" ]; then
  echo "OpenBOR: the object has loaded sections the PS4 link script doesn't place:"
  echo "$STRAY"
  exit 1
fi
# Each engine's data and bss in a section of its own, its bounds in the object (the restart puts them back).
SYMBOLS=$(llvm-nm-21 --defined-only "$OUT/openbor_core.o.new" | awk '{print $3}')
for e in $ENGINES; do
  for s in .data.ob_$e .bss.ob_$e; do
    if ! grep -qxF "$s" <<< "$SECTIONS"; then
      echo "OpenBOR: the engine $e's section $s is missing (its restart would not work)"
      exit 1
    fi
  done
  for s in ob_data_start_$e ob_data_end_$e ob_bss_start_$e ob_bss_end_$e; do
    if ! grep -qx "$s" <<< "$SYMBOLS"; then
      echo "OpenBOR: the engine $e's bound $s is missing (its restart would not work)"
      exit 1
    fi
  done
done
UNDEFINED=$(llvm-nm-21 -u "$OUT/openbor_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
WEAK=$(llvm-nm-21 -u "$OUT/openbor_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "OpenBOR: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)'; then
  echo "OpenBOR: the object uses C++ operator new/delete"
  exit 1
fi
COMMONS=$(llvm-nm-21 "$OUT/openbor_core.o.new" | awk '$2 == "C" || $2 == "c" {print $3}')
if [ -n "$COMMONS" ]; then
  echo "OpenBOR: the object has common symbols (made local, they would have no storage):"
  echo "$COMMONS" | head -20
  exit 1
fi
BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/openbor_core.o.new" | awk '($3 == "b" || $3 == "B" || $3 == "d" || $3 == "D") && $2 + 0 >= 1048576 {print $4}')
if [ -n "$BIG" ]; then
  echo "OpenBOR: the object has static arrays of 1 MiB or more:"
  echo "$BIG"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a"
  PROVIDED=$(for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done |
    awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVi]$/ {print $NF}' | sort -u)
  MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
  if [ -n "$MISSING" ]; then
    echo "OpenBOR: the object needs symbols the PS4 libraries don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "OpenBOR: $(echo "$UNDEFINED" | wc -l) C library symbols needed, all in the PS4 libraries"
fi
if cmp -s "$OUT/openbor_core.o.new" "$OUT/openbor_core.o"; then
  rm -f "$OUT/openbor_core.o.new"
else
  mv "$OUT/openbor_core.o.new" "$OUT/openbor_core.o"
fi
ls -la "$OUT/openbor_core.o"
