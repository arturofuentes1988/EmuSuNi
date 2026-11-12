// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#pragma once

// (01.09, the owner: "Sí, hazlo") The 3DS's updates and DLC installed from the player's CIA files, with no game
// running: the core's installer (Azahar's Service::AM, scripts/build-3ds-core.sh - its functions below, which the app
// defines weakly until the core's object brings them) writes them into the emulated SD card (<save_dir>/Azahar/sdmc),
// where the core finds them when the game loads. An encrypted CIA is refused by the core (no keys are ever used or
// shipped); a game's own CIA isn't installed (the app starts .3ds / .cci / .cxi / .3dsx only). The CIAs are found in a
// "cia" folder inside each of the 3DS's game folders (Roms/3ds/cia on a USB drive or in /data/EmuSuNi), which the
// Library never lists as games (.cia isn't one of the 3DS's extensions: src/Systems.cpp).
//
// The calls into the core are made one at a time: the information and the list on the menu's thread (they read only
// a CIA's header, its ticket's and TMD's sizes and its SMDH), the install on a thread of its own (Start: one at a
// time), whose progress the menu reads each frame (Status).

#include <cstdint>
#include <string>
#include <vector>

namespace ThreeDsCia
{
// The core's codes (negative) and the app's own.
enum Code : int
{
  OK = 0,
  ENCRYPTED = -1,    // its contents are encrypted: refused
  INVALID = -2,      // not a CIA, or damaged
  IO = -3,           // couldn't be read or written (or no room)
  CANCELLED = -4,    // stopped by the player: what was installed before is back
  CRASHED = -5,      // the core failed and its rescue caught it: what was installed before is back
  NOT_INSTALLED = -6,  // a kind the app doesn't install (a game, the system's)
  GAME_RUNNING = -7,   // a 3DS game is running: nothing installed or removed under it
  CORE_BUSY = -8,      // the core is installing or removing something else
  NO_CORE = -100,    // this build's 3DS core has no installer
  NO_ROOM = -101,    // less free space than it needs (the app's check)
  BUSY = -102,       // another install runs
  NO_THREAD = -103,  // the install's thread couldn't be made
};

// What a title ID is (its high half: 00040000 a game, 0004000E its update, 0004008C its DLC).
enum class Kind
{
  Game,
  Update,
  Dlc,
  Other,
};
Kind KindOf(uint64_t title_id);
// The game an update's or DLC's title ID belongs to (00040000 and its low half); a game's own as it is.
uint64_t GameOf(uint64_t title_id);
// "000400000F70CC00": the title ID as the core's log, its mods' folder and the CIA tools write it.
std::string Hex(uint64_t title_id);
// A title's version as the 3DS shows it: "v1.2" (major.minor), "v1.2.3" with a micro number; "v0" for 0.
std::string VersionText(uint16_t version);
// "640 KB", "512 MB", "1.4 GB".
std::string SizeText(uint64_t bytes);
// What a code says to the player (English; the menu translates it).
std::string CodeText(int code);

// Does this build's core install CIAs?
bool Available();

// A CIA's information, from the core (read: false and `code` < 0 when it couldn't be read).
struct Info
{
  bool read = false;
  int code = 0;
  uint64_t title_id = 0;
  uint16_t version = 0;
  bool encrypted = false;
  uint64_t file_bytes = 0;
  uint64_t install_bytes = 0;  // what it takes in the emulated SD card
  uint32_t contents = 0;
  std::string name;  // the SMDH's short English title ("" none: an update or DLC often has none)
  Kind kind = Kind::Other;
};
Info Read(const std::string& path);

// The "cia" folders of the 3DS's game folders (any letters' case; `make`: made in each game folder that has none).
std::vector<std::string> Folders(const std::vector<std::string>& game_folders, bool make);
// The .cia files in those folders (any letters' case, not in their subfolders), by name; each with its Info.
struct File
{
  std::string path;
  std::string file_name;
  uint64_t size = 0;
  Info info;
};
std::vector<File> List(const std::vector<std::string>& folders);

// The free space of the file system holding `folder` (bytes; -1 not known).
int64_t FreeBytes(const std::string& folder);
// The room an install asks for besides its own bytes: the SD card's files and the core's work.
constexpr uint64_t SPARE_BYTES = 64ull << 20;

// ---- The install (one at a time, on its own thread) ----
struct Progress
{
  bool busy = false;   // installing
  bool done = false;   // finished since Start (result below), until the next Start
  std::string path;    // the CIA
  uint64_t bytes = 0, total = 0;
  int result = 0;
  double seconds = 0.0;  // how long it took (done) or has taken so far
};
// save_dir: the 3DS's save folder (the libretro save directory its core gets in a game: <UserDir>/saves/3ds).
// False: not started (Status().result says why: BUSY, NO_CORE, NO_THREAD).
bool Start(const std::string& save_dir, const std::string& path);
Progress Status();
void Cancel();  // the install stops at its next progress report (CANCELLED)
// The finished install's thread joined, its result kept (the menu: when it has said it).
void Forget();

// ---- A game's updates, DLC and mods ----
// A game file's title ID, from its header: a .3ds / .cci (NCSD: its media ID), a .cxi (NCCH: its program ID); 0 for a
// .3dsx (homebrew has none) or a file that can't be read.
uint64_t RomTitleId(const std::string& path);
// What is installed for a game (its title ID 00040000...).
struct Installed
{
  bool read = false;
  int code = 0;
  bool has_update = false;
  uint16_t update_version = 0;
  uint64_t update_bytes = 0;
  uint32_t dlc_titles = 0, dlc_contents = 0;
  uint64_t dlc_bytes = 0;
};
Installed InstalledFor(const std::string& save_dir, uint64_t game_title_id);
// Removes an installed update (0004000E...) or DLC (0004008C...). A code.
int Uninstall(const std::string& save_dir, uint64_t title_id);
// The game's mods' folder (LayeredFS: romfs/, exefs/, code.ips...) and whether it has anything in it.
std::string ModsFolder(const std::string& save_dir, uint64_t game_title_id);
bool HasMods(const std::string& save_dir, uint64_t game_title_id);
// Mods: On / Off for a game (On unless turned off: settings.ini's 3ds_mods_off, its title IDs), for the core's
// emusuni_mods at the game's start (Emulator.cpp). Saved at once (Settings::Set).
bool ModsOn(uint64_t game_title_id);
void SetModsOn(uint64_t game_title_id, bool on);
}  // namespace ThreeDsCia
