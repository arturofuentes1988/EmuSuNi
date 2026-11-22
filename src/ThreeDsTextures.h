// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#pragma once

// (01.42, the owner) A 3DS game's texture pack: the player's own PNG pictures that Azahar draws instead of the game's
// textures (its custom textures: tex1_<W>x<H>_<hash>_<format>_mip<N>.png, each named after the texture it replaces).
// The core (01.42, scripts/build-3ds-core.sh) loads them from <save_dir>/Azahar/load/textures/<title ID> when the app
// gives the game custom_textures=enabled, with its own limits for the PS4: one loading thread, nothing over 2048 a
// side, at most 256 MiB of them in the GPU, each one at its first use. The pack is the player's: never in the PKG, on
// GitHub or on our server.
//  - Texture Pack (a 3DS game's Updates, DLC and Mods): On / Off for the game, Off unless turned on (settings.ini's
//    3ds_textures_on, its title IDs); offered only when the game's folder has a picture.
//  - Install Textures: a pack the player left beside the games (Roms/3ds/textures/<title ID>, on a USB drive or in
//    /data/EmuSuNi) copied into that folder on a thread of its own - only its .png pictures (real PNGs) and pack.json,
//    with their subfolders; nothing else of it is copied or run. A picture over 4 times its texture's size (both
//    sides), or over the core's 2048 a side, is made smaller as it is copied, by halves: each new pixel the average of
//    the ones it replaces, the colour weighted by alpha (local/herramientas/scripts-pc/reducir-texturas.py's way).
//  - Remove Textures: the installed copy deleted (the player's pack where they left it is never touched).
// Counting a folder's pictures (a pack can have thousands, in subfolders) is done on a thread of its own as well: the
// menu shows "counting" until it is done.

#include <cstdint>
#include <string>
#include <vector>

namespace ThreeDsTextures
{
// What the core takes for a pack at most: GPU memory (its cap) and the flexible memory of its loading thread's stack.
constexpr int GPU_MIB = 256;
constexpr int THREAD_MIB = 2;
// The GPU memory left free besides it for a start with the pack (src/Emulator.cpp's memory check).
constexpr int GPU_MARGIN_MIB = 64;

// The installed pack: <save_dir>/Azahar/load/textures/<title ID> (save_dir: the 3DS's save folder,
// <UserDir>/saves/3ds).
std::string InstalledFolder(const std::string& save_dir, uint64_t title);
// A pack the player left: the first <a 3DS games folder>/textures/<title ID> that is a folder ("": none).
std::string PackFolder(const std::vector<std::string>& game_folders, uint64_t title);
// Whether a folder (or one of its subfolders) has a .png: the first one found ends the look.
bool HasPng(const std::string& folder);

// Texture Pack: On / Off for a game (Off unless turned on). Saved at once (Settings::Set).
bool On(uint64_t title);
void SetOn(uint64_t title, bool on);

// The GPU's (direct) memory free now, in MiB, as the system's free areas add up (-1: not known - the PC).
int GpuFreeMiB();

// ---- A folder's pictures counted beside the menu ----
struct Count
{
  int pngs = 0;
  uint64_t bytes = 0;
};
// Asks for a folder's count (nothing new when it is counted or being counted).
void RequestCount(const std::string& folder);
// True once it is counted (*count: what it has).
bool Counted(const std::string& folder, Count* count);
// Its count forgotten (the folder changed: installed or removed).
void ForgetCount(const std::string& folder);

// ---- The install ----
enum Result : int
{
  DONE = 0,
  CANCELLED = 1,
  NO_PICTURES = 2,  // the pack has no PNG picture
  NO_ROOM = 3,      // not enough free space for it
  FAILED = 4,       // a file couldn't be read or written (why: Progress::why)
  BUSY = 5,
  NO_THREAD = 6,
};
struct Progress
{
  bool busy = false;
  bool done = false;      // finished since Install, until Forget
  bool scanning = false;  // the pack's files still being listed
  int files = 0, files_done = 0;
  uint64_t bytes = 0, bytes_done = 0;  // the pack's (its pictures' and pack.json's)
  int reduced = 0;         // pictures made smaller
  int too_big = 0;         // still over 2048 a side (their sides can't be halved): copied, the core leaves them out
  int left_out = 0;        // other files, or .png files that aren't PNGs
  int copied = 0;          // files in the installed copy (its pictures and pack.json)
  uint64_t written = 0;    // the installed copy's bytes
  int result = DONE;
  std::string why;         // a failure's file and reason (English, for the log)
  double seconds = 0.0;
};
// Copies the pack `from` into `to` (the installed folder: made anew, the one before replaced once the copy is whole).
// False: not started (Status().result says why).
bool Install(const std::string& from, const std::string& to);
Progress Status();
void Cancel();  // the install stops at its next file or row block; the installed folder stays as it was
// The finished install's thread joined, its result kept until the next Install.
void Forget();

// The installed copy deleted, with all it holds (the PS4's C++ library can't delete folders: unlink and rmdir).
// False: some of it is left (*why).
bool Remove(const std::string& folder, std::string* why);
// Text for sizes ("54 MB").
std::string SizeText(uint64_t bytes);
}  // namespace ThreeDsTextures
