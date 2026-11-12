#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Azahar's dedicated room server, azahar-room (azahar-emu/azahar 2126.2, ~/src/azahar: the
# unified source tarball setup.sh committed, the same tree as scripts/build-3ds-core.sh;
# GPL-2.0-or-later), for EmuSuNi's 3DS online play: every PS4 that plays a 3DS game with 3DS Online on
# joins this room (the core: build-3ds-core.sh, 01.04), and the room passes each console's local
# wireless packets (UDS: beacons and data) to the others, as Azahar's own rooms do. Upstream's code,
# with these changes:
#   - No address in its log: upstream wrote each player's IP when they joined, left, were kicked or
#     banned ("[1.2.3.4] name has joined."); now only the nickname. (The room never sent an address to
#     the players: they reach each other through it.)
#   - No chat text in its log: a chat message is logged as one, without its words (EmuSuNi sends no
#     chat; an Azahar for PC in the same room can).
#   - Linked statically (glibc, libstdc++): one file that runs on the server's older Linux (Ubuntu
#     20.04 on the Oracle VM) whatever the PC that builds it.
# Run it as a private room (no token: never announced to a lobby), no password, no ban list file (it wants a preferred
# game's name, shown to Azahar for PC only):
#   azahar-room --room-name "EmuSuNi 3DS" --preferred-app "Any 3DS game" --port 24872 --max_members 32 \
#     --log-file <file>
# (UDP 24872 open on the server; never runs as root.)
#
# Usage:
#   build-3ds-room.sh <out dir>     <out dir>/azahar-room (x86-64 Linux), its sha-256 printed
set -euo pipefail
SRC="${AZAHAR_DIR:-$HOME/src/azahar}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
TREE="$OUT/src"
BUILD="$OUT/build"
LOG="$OUT/3ds-room.log"
JOBS="${CTR_ROOM_JOBS:-4}"

AZAHAR_GIT_COMMIT=219d743a1e698101fe1ab8bef43e6563126c3c27
TREE_COMMIT="$(tr -d '[:space:]' < "$SRC/GIT-COMMIT" 2>/dev/null || true)"
if [ "$TREE_COMMIT" != "$AZAHAR_GIT_COMMIT" ]; then
  echo "azahar-room: $SRC isn't the 2126.2 unified source (GIT-COMMIT ${TREE_COMMIT:-missing}): run scripts/setup.sh"
  exit 1
fi
# A copy of the commit's files (build-3ds-core.sh patches $SRC in place for the PS4; the room is upstream's code),
# without git: Azahar's CMake checks the submodules of a git tree.
if [ "$(tr -d '[:space:]' < "$TREE/GIT-COMMIT" 2>/dev/null || true)" != "$AZAHAR_GIT_COMMIT" ]; then
  rm -rf "$TREE"
  mkdir -p "$TREE"
  git -C "$SRC" archive --format=tar HEAD | tar -x -C "$TREE"
fi

python3 -I - "$SRC" "$TREE" <<'PATCH'
import subprocess
import sys
src, tree = sys.argv[1], sys.argv[2]
def git_file(path):
    run = subprocess.run(['git', '-C', src, 'show', 'HEAD:' + path], capture_output=True, check=True)
    return run.stdout.decode('utf-8')
patched = {}
def patch(path, old, new):
    # On the file as git has it, so that a changed patch replaces the old one.
    if path not in patched:
        patched[path] = git_file(path)
    assert patched[path].count(old) == 1, (path, patched[path].count(old), old[:70])
    patched[path] = patched[path].replace(old, new)

# No address in the log.
for what in ('joined', 'left', 'been kicked', 'been banned'):
    patch('src/network/room.cpp',
          '        LOG_INFO(Network, "[{}] {} has %s.", ip, display_name);\n' % what,
          '        LOG_INFO(Network, "{} has %s.", display_name); // EmuSuNi: no address in the log\n' % what)
# No chat text in the log.
patch('src/network/room.cpp',
      '    if (sending_member->user_data.username.empty()) {\n'
      '        LOG_INFO(Network, "{}: {}", sending_member->nickname, message);\n'
      '    } else {\n'
      '        LOG_INFO(Network, "{} ({}): {}", sending_member->nickname,\n'
      '                 sending_member->user_data.username, message);\n'
      '    }\n',
      '    // EmuSuNi: a chat message in the log without its words.\n'
      '    LOG_INFO(Network, "{} sent a chat message ({} characters).", sending_member->nickname, message.size());\n')

for path, text in patched.items():
    with open(tree + '/' + path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
print('azahar-room: %d files patched' % len(patched))
PATCH

if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -G Ninja -S "$TREE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CC:-clang-21}" -DCMAKE_CXX_COMPILER="${CXX:-clang++-21}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DENABLE_ROOM=ON -DENABLE_ROOM_STANDALONE=ON -DENABLE_LIBRETRO=OFF -DENABLE_QT=OFF -DENABLE_QT_TRANSLATION=OFF \
    -DENABLE_SDL2=OFF -DENABLE_SDL2_FRONTEND=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_TESTS=OFF -DENABLE_SCRIPTING=OFF \
    -DENABLE_GDBSTUB=OFF -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF -DENABLE_LIBUSB=OFF -DENABLE_DISCORD_RPC=OFF \
    -DENABLE_MICROPROFILE=OFF -DENABLE_OPENGL=OFF -DENABLE_VULKAN=OFF -DENABLE_SOFTWARE_RENDERER=ON \
    -DENABLE_LTO=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF -DCITRA_USE_PRECOMPILED_HEADERS=OFF -DZSTD_BUILD_DICTBUILDER=OFF \
    -DENABLE_BUILTIN_KEYBLOB=OFF -DCMAKE_EXE_LINKER_FLAGS="-static" > "$LOG" 2>&1 \
    || { tail -30 "$LOG"; echo "azahar-room: cmake failed ($LOG)"; exit 1; }
fi
ninja -C "$BUILD" -j "$JOBS" citra_room_standalone >> "$LOG" 2>&1 \
  || { grep -E "error" "$LOG" | head -30; echo "azahar-room: the build failed ($LOG)"; exit 1; }
BIN="$(find "$BUILD" -name azahar-room -type f | head -1)"
cp "$BIN" "$OUT/azahar-room"
strip "$OUT/azahar-room"
if ! file "$OUT/azahar-room" | grep -q -E "statically linked|static-pie linked"; then
  echo "azahar-room: not linked statically: $(file "$OUT/azahar-room")"
  exit 1
fi
# The log lines: none with an address left.
if strings "$OUT/azahar-room" | grep -q '^\[{}\] {} has'; then
  echo "azahar-room: an address is still in a log line"
  exit 1
fi
SIZE="$(stat -c %s "$OUT/azahar-room")"
echo "azahar-room: $OUT/azahar-room ($SIZE bytes), sha-256 $(sha256sum "$OUT/azahar-room" | cut -c1-64)"
