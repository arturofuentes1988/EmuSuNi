// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game library: each console's ROMs (Systems.h: .sfc .smc .swc .fig .bs for the Super
// Nintendo, .nes .fds .unf for the NES, or one of them in a .zip) in its game folder and the
// subfolders; an arcade game is the .zip itself, named after FinalBurn Neo's driver (src/Arcade.h), a
// NAOMI or Atomiswave game after Flycast's set (src/Naomi.h).
// A game is known by its key (Systems.h): its settings, saves, states, cheats and cover use it.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Systems.h"

namespace Library
{
struct Game
{
  std::string path;    // the ROM or .zip
  std::string id;      // file name without extension
  std::string title;   // tidied up: "The 7th Saga" for "7th Saga, The (U) [!]"
  std::string region;  // "USA", "Europe", "Japan", ... or empty
  std::string tags;    // what else the name says: "[!]", "(V1.1)", "(Proto)"
  uint64_t size = 0;
  SystemId system = SystemId::SNES;
  std::string key;     // id for the Super Nintendo, "<console>/<id>" for the others (Systems.h)
  bool custom_title = false;  // the title is a name of the user's own (Rename)
  // A PlayStation or Dreamcast game on several discs ("(Disc 1)", "(Disc 2)"... side by side): each
  // disc's file in order (path is the first one's); id and key are the name without the disc's number.
  std::vector<std::string> discs;
  // An Arcade game (a romset, src/Arcade.h): the names its covers are looked up by - its full name,
  // then its parent's (libretro's thumbnails are named so) - and the zips it may need that aren't there
  // (its BIOS set's, neogeo.zip; its parent's: a split set needs them, a non-merged one doesn't), which
  // Information says, and the start too when the core refuses the game. A NAOMI or Atomiswave game (src/Naomi.h)
  // the same way: its full name's covers, and its BIOS's zip, a clone's parent zip, a GD-ROM game's disc image.
  std::vector<std::string> cover_names;
  std::vector<std::string> missing;
};

bool IsRomName(const std::string& name);
// One console's games in folder and its subfolders (4 levels), by title. open_zips: a .zip is
// opened to see which console's ROM it holds (needed when consoles share a folder), else every
// .zip there is taken as the console's.
std::vector<Game> Scan(const std::string& folder, SystemId system, bool open_zips = true);
// How many of a console's games a folder holds directly (the folder browser).
int CountGames(const std::string& folder, SystemId system);
// The ROM's bytes (a .zip's: its first file with one of the console's extensions; a .md or a .bin,
// a readme's or a CD track's too, only when there is no other).
bool ReadRom(const std::string& path, SystemId system, std::vector<uint8_t>* data, std::string* error);
// The title, region and tags from a file name without its extension.
void ParseName(const std::string& stem, std::string* title, std::string* region, std::string* tags);
// A disc's number in a name - "(Disc 2)", "(Disc 2 of 3)", "(Disk 2)", "(CD2)", "[Disc 2]" - and the
// name without it (base). 0: none.
int DiscNumber(const std::string& name, std::string* base);
// The discs the core can change between: a game on several discs' (Game::discs) or those an .m3u
// lists; none for a game on one disc.
std::vector<std::string> Discs(const Game& game);
// Every file of the game on the drive: its file (each disc's), the tracks its .cue or .gdi names and
// the discs its .m3u lists - those in its own folder (or below), and not named by another game there.
// Delete Game deletes them; Information adds up their size.
std::vector<std::string> GameFiles(const Game& game);
// A PlayStation disc's tracks left without their .cue (00.81): for "Name (Track 1).bin" (or "(Track 01)"), it and the
// "Name (Track N).bin" beside it, N from 2 on without a gap, as Redump names them; none for any other file, or when
// it is alone (its data track is then the whole disc). The game is started with a .cue made for them (Emulator.cpp).
std::vector<std::string> LoneTracks(const std::string& path);

// ---- Where the games are found (00.62) ----
// The USB drives connected now, as the app sees them: /usb0 ... /usb7 that have something in them (an empty one
// is a mount point without a drive); `root` before each: "" on the PS4, a test's folder on the PC.
std::vector<std::string> UsbDrives(const std::string& root = {});
// The folder in `folder` named `name` in any letters' case ("roms" finds "Roms" or "ROMS"; the same name
// first): its path, else "".
std::string FindEntry(const std::string& folder, const std::string& name);
// The folders a console's games are found in by themselves (a folder chosen in Game Folders or not):
// <data_dir>/Roms/<console key> and Roms/<console key> on each USB drive, in any letters' case. Finding them
// reads data_dir, each drive's top folder and the Roms folders, nothing deeper: cheap enough to look again
// each time the player enters a tab (Menu.cpp), so a drive plugged in later is found.
struct Places
{
  std::vector<std::string> drives;
  std::map<std::string, std::vector<std::string>> folders;  // console key -> its folders (data_dir's first)
  bool operator==(const Places& other) const { return drives == other.drives && folders == other.folders; }
  bool operator!=(const Places& other) const { return !(*this == other); }
};
Places FindPlaces(const std::string& data_dir, const std::vector<std::string>& drives);
// The places read on the menu's own thread (BuildGames, a look made there) and a look on another thread
// (Menu.cpp's CheckPlaces): that look's answer is out of date when the places were read again since it
// started - older than what the menu has, it would make it read every game again for nothing.
struct PlacesReads
{
  unsigned reads = 0;       // the menu thread's reads so far
  unsigned look_start = 0;  // `reads` when the other thread's look started
  void Read() { reads++; }
  void LookStarted() { look_start = reads; }
  bool LookCurrent() const { return look_start == reads; }
};
// What two paths to one file or folder have in common: the path tidied up and, on a USB drive (FAT and exFAT
// don't tell the letters' case apart; /mnt/usbN is /usbN outside the app's sandbox), in lower case.
std::string PathKey(const std::string& path, const std::vector<std::string>& drives);
// A console's folders, each once: `chosen` (its folder in Game Folders, else its default one), then its
// Places. Of two paths to one folder, the one that is there is kept.
std::vector<std::string> ConsoleFolders(const System& system, const std::string& chosen, const Places& places);
// Is the folder the console's Roms/<key> of data_dir or of a USB drive (any letters' case)?
bool IsRomsFolder(const std::string& folder, const std::string& key, const std::string& data_dir);
// Every console's games: Scan() of each of its ConsoleFolders() (chosen[id]: its folder in Game Folders, else
// its default one). A folder that holds another console's games too (the same one, or one inside the other)
// has its .zip files opened to see whose they are; a file found through two folders is one game.
struct Found
{
  std::vector<Game> games;  // console by console, each folder's by title
  std::array<std::vector<std::string>, SYSTEM_COUNT> folders;  // the folders looked in
  std::array<std::vector<int>, SYSTEM_COUNT> found_in;         // how many games each of them gave
  std::array<int, SYSTEM_COUNT> counts{};                      // each console's games
  bool stopped = false;  // ScanProgress::stop ended the read before its end: these aren't all the games
  // (01.06) Games' files their folders list but the drive says aren't there (left out): how many, the first.
  int missing = 0;
  std::string missing_first;
};
// ---- Big libraries (00.85) ----
// What a read learns of a file by looking inside it - the console of a .zip's ROM, a CD track's data, a disc's console,
// a NAOMI cartridge's header, the files a .cue, .gdi or .m3u names - with the file's size and date then. ScanAll looks
// inside a file once for every console of a shared folder (before 00.85: once per console, each .zip of a drive whose
// top folder every console was given opened twenty times), and takes again what an earlier read learnt of a file that
// still has the same size and date: the menu keeps these in its cache (SaveCache), so the read of each start lists the
// folders but opens only the files that are new or changed.
struct FileFacts
{
  uint64_t size = 0;
  int64_t time = 0;     // modified (seconds)
  unsigned looked = 0;  // which of these were looked at (Library.cpp's LOOKED_*)
  int zip = -1;         // a .zip: its ROM's console (a SystemId), -1 none
  bool cd_data = false, cd_playstation = false;  // a CD track: a data track (an ISO 9660 volume), a PlayStation disc's
  bool dreamcast = false, psp = false;           // a disc image: a Dreamcast GD-ROM's, a PSP game's
  bool saturn = false;                           // an image whose first sector is a Saturn disc's
  bool naomi = false;                            // a NAOMI cartridge image, and its header's names:
  std::string naomi_japan, naomi_usa, naomi_export;
  bool naomi_vertical = false;
  std::vector<std::string> listed;  // a .cue, .gdi or .m3u: the files it names (as ListedFiles gives them)
};
using Contents = std::unordered_map<std::string, FileFacts>;  // by the file's path
// A read as it goes, for the menu's line ("Reading your games... 12345") and from the menu: stop (the read ends at its
// next file; what it found then is incomplete: Found::stopped), pause (it waits between files while a game runs).
struct ScanProgress
{
  std::atomic<int> games{0};  // found so far (a game a later step leaves out - a disc's track, one found twice - too)
  std::atomic<bool> stop{false}, pause{false};
};
// Every console's games, as above; with a progress (its games counted up, its stop and pause followed) and the files'
// contents: what an earlier read learnt of them in, what this one learnt of the files it looked at out (a read stopped
// before its end keeps the earlier read's others too). The games are the same as without them.
Found ScanAll(const std::array<std::string, SYSTEM_COUNT>& chosen, const Places& places, ScanProgress* progress = nullptr,
              Contents* contents = nullptr);
// The library's cache (00.85, the menu's data_dir/cache/library.tsv): what the last read found - each console's folder
// asked for, the places, every game as ScanAll gave it - and what it learnt of the files. A start shows those games at
// once (made by the same version, the same folders asked for) while its read looks again on a thread (Menu.cpp). One
// line per thing, tab-separated; written whole through a temporary file (a cut leaves the one before); a file made by
// another version or cut short is no cache. The contents are kept while the consoles' table is the same (their
// extensions decide a .zip's console): `facts_ok` false - another table - leaves them out.
struct Cache
{
  std::string version;                           // the app's version that made it
  // (00.87) Who wrote it, on a USB drive's portable data (src/Portable.h's LibraryWriter): "" elsewhere.
  std::string writer;
  std::array<std::string, SYSTEM_COUNT> chosen;  // each console's folder asked for (Game Folders, else its default)
  Places places;
  Found found;
  Contents contents;
  bool facts_ok = false;
};
// (00.87) drive: the portable data's drive (src/Portable.h) - its files' paths written relative to it
// (src/DrivePaths.h), read back with its root of now; "" (the PS4's own cache) as before. writer: a word written with
// it (Cache::writer).
bool SaveCache(const std::string& path, const std::string& version, const std::array<std::string, SYSTEM_COUNT>& chosen,
               const Places& places, const Found& found, const Contents& contents, const std::string& drive = {},
               const std::string& writer = {});
bool LoadCache(const std::string& path, Cache* cache, const std::string& drive = {});
// A short sum of everything a read found (its games' every field, in order, and the folders): two reads that found the
// same have the same one, so the menu leaves its games as they are when nothing changed (no reorder, no reload).
uint64_t Fingerprint(const Found& found);
// The consoles that get a tab (Settings -> Consoles Shown), in `order`: all of them ("all", or an unknown
// mode), those with games ("games": games[id] > 0), or those not turned off ("chosen": `off` lists their
// keys, "psp,dc"). All of them when that would leave none.
std::vector<SystemId> ShownConsoles(const std::vector<SystemId>& order, const std::string& mode,
                                     const std::array<int, SYSTEM_COUNT>& games, const std::string& off);
// Covers on a USB drive (Settings -> Covers Saved On): <drive>/EmuSuNi/covers on the first drive that has an
// EmuSuNi folder (any letters' case), else on the first that has a Roms folder, else on the first; "" without
// a drive.
std::string UsbCoversFolder(const std::vector<std::string>& drives);
// The covers folders the drives have now (<drive>/EmuSuNi/covers, any letters' case): the menu shows them all.
std::vector<std::string> UsbCoversFolders(const std::vector<std::string>& drives);

// (00.87) Where the marks of a game's missing BIOS (an Arcade's, a NAOMI's, an Atomiswave's: Game::missing) look for
// it: the app's system folder (data_dir/system), unless the start said another for the console (the BIOS on a USB
// drive's portable data, src/Portable.h). Set before the first read.
void SetSystemFolder(SystemId system, const std::string& folder);
std::string SystemFolder(SystemId system);
// (00.87) The portable data's folder on a USB drive (src/Portable.h): saves, BIOS, covers, never games - not read.
inline constexpr const char* PORTABLE_FOLDER = "EmuSuNi-Portable";

// ---- The consoles' folders, made by the app (00.63) ----
// Does the drive look like the PS4's own storage (its games under user/app, user/patch, user/addcont, as on an
// extended storage it formatted), not a player's drive? Such a drive is never written to.
bool LooksLikePs4Storage(const std::string& drive);
// The Roms folders of the drives a player keeps games on: each drive's that has one, in any letters' case (its
// own spelling: "ROMS"), and - when no drive has one - /usb0's (to be made); never one of a drive that looks
// like the PS4's own storage.
std::vector<std::string> GameRomsFolders(const std::vector<std::string>& drives);
// Makes the folder `roms` (when it isn't there) and in it a folder for each console of the build (Systems():
// a console added gets its folder too) that has none in any letters' case. Stops at the first that can't be
// made (a read-only or full drive: the rest would fail the same way); never throws.
struct FolderMaking
{
  int consoles = 0;            // the build's
  int made = 0, there = 0;     // made now, there already
  int failed = 0;              // not made
  std::string error;           // why (the first failure's message)
};
FolderMaking MakeConsoleFolders(const std::string& roms);
}  // namespace Library
