// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game library: each console's ROMs (Systems.h: .sfc .smc .swc .fig .bs for the Super
// Nintendo, .nes .fds .unf for the NES, or one of them in a .zip) in its game folder and the
// subfolders; an arcade game is the .zip itself, named after FinalBurn Neo's driver (src/Arcade.h), a
// NAOMI or Atomiswave game after Flycast's set (src/Naomi.h).
// A game is known by its key (Systems.h): its settings, saves, states, cheats and cover use it.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
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
};
Found ScanAll(const std::array<std::string, SYSTEM_COUNT>& chosen, const Places& places);
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
