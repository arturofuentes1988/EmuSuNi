// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The PS Vita's firmware (builds with EMUSUNI_VITA: scripts/build.sh with-vita). Vita3K runs the Vita's own system
// modules and fonts: Sony's firmware 3.74, in three packages (.PUP files: the system's, its preinstalled package's
// and the fonts'). None of it is EmuSuNi's to give - it isn't in the package nor on any server of ours - and the
// app never asks Sony's servers for it: a jailbroken PS4 must not reach them (the owner's rule of 2026-11-13: no
// connection to Sony's servers, ever; this file names none of them). The player brings the three files: in the
// PS Vita's game folder (Roms/psvita of a USB drive or of the PS4's storage) under any names, taken from there by
// Settings -> PS Vita Firmware (or the panel a Vita game without it opens) and checked by their size and SHA-256
// before they count; or in <system>/vita3k by hand, under the names the core knows them by. The core installs
// each package itself at the next Vita game's start (about 323 MiB in vs0, pd0, sa0 and os0 there; a minute or
// more the first time, its progress on the screen).
// Every step goes to the log ("vita: firmware: ..."). Nothing here throws, and nothing blocks the caller: the
// files are a thread's.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Vita
{
// The three packages.
enum Part { SYSTEM = 0, PREINST = 1, FONTS = 2, PART_COUNT = 3 };

// The three files' size and what they take once installed.
constexpr int64_t FILES_BYTES = 319411712;
constexpr int INSTALLED_MIB = 323;
// What the player is to bring, for the firmware's panel and a game's start without it (English, for PS4Lang::T).
constexpr const char* FILES_WORDS =
    "Bring Sony's three PSP2UPDAT.PUP files of firmware 3.74: the system (128 MB), the preinstalled package (123 MB) "
    "and the fonts (54 MB).";

struct Firmware
{
  bool installed[PART_COUNT] = {};  // the core installed it: its folder is there, with something in it
  bool file[PART_COUNT] = {};       // its .PUP is in <system>/vita3k: the core installs it at the next game's start
  // Every part installed, or its file there to be.
  bool Ready() const
  {
    return (installed[SYSTEM] || file[SYSTEM]) && (installed[PREINST] || file[PREINST]) &&
           (installed[FONTS] || file[FONTS]);
  }
  bool Installed() const { return installed[SYSTEM] && installed[PREINST] && installed[FONTS]; }
};

// What is there. system_dir: the app's system folder (the core's own folder is its "vita3k"). As the core tells it
// (Vita3K's libretro_installer.cpp): the system by vs0/sys/external, the preinstalled package by pd0, the fonts by
// sa0, none of them while its install was cut short; a .PUP by its name ("preinst" in it, "font" in it, any other).
Firmware FindFirmware(const std::string& system_dir);
// For the log: "the system installed, the preinstalled package a .PUP to install, the fonts missing".
std::string Describe(const Firmware& firmware);
// What a Vita game's start says when a part is neither installed nor there to be (Emulator::Run's error; the menu
// opens the firmware's panel on it: IsMissingMessage). One line each, for the menu's panel.
std::string MissingMessage(const Firmware& firmware, const std::string& system_dir);
bool IsMissingMessage(const std::string& error);
// After a game's start with everything installed: the copies StartCopy made in <system>/vita3k (about 305 MiB) are
// deleted; files the player put there by hand stay, and so do the player's files in the game folders.
void RemoveCopiedFiles(const std::string& system_dir);

// ---- The keys (a build with EMUSUNI_VITA_NOKEYS: scripts/build.sh with-vita-nokeys) ----
// Vita3K's source has Sony's keys written in it, and a build with-vita has them in its eboot. A build
// with-vita-nokeys has none: its core reads them at a game's start from <system>/vita3k/vita-keys.txt, a file the
// player puts there as a console's BIOS (scripts/make-vita-keys.py makes it out of Vita3K's source code), and takes
// it only when it is the one the build was made for (its SHA-256, as that script tells it). Without the file no
// PS Vita game starts: the firmware can't be installed from its .PUP files, and one already installed doesn't help,
// for the system's modules stay encrypted on the disk and every game loads them (libc.suprx first); an encrypted
// game (a NoNpDrm dump) can't be installed either. The app looks first, here, and says so instead of asking the
// core, which would refuse too.
enum class Keys
{
  Inside,   // a build with the keys in the core: no file is needed
  Ready,    // the file is there and is the one expected
  Missing,  // no file
  Wrong,    // a file, but not the one expected
};
Keys FindKeys(const std::string& system_dir);
// For the log: "vita-keys.txt is the file expected", "... isn't there", ...
std::string DescribeKeys(Keys keys, const std::string& system_dir);
// What a Vita game's start says when the keys aren't there (Emulator::Run's error). for_firmware: the firmware's
// .PUP files are there and can't be installed (the menu opens the firmware's panel on it: IsMissingMessage); else
// the firmware is installed and the game can't start.
std::string KeysMessage(Keys keys, const std::string& system_dir, bool for_firmware);
// For the firmware's panel: what the player is to know about the keys, a line each; "" when there is nothing to say
// (a build with them inside, or the file there).
std::string KeysNote(const std::string& system_dir);

// ---- The copy from the player's own files (01.29) ----
// Sony's three files as the player brought them, in the PS Vita's game folders (Roms/psvita of a USB drive or of
// the PS4's storage) or in the "firmware" folder of one. Any .PUP there counts whatever its name - a computer that
// downloads the three gets the same name three times, and numbers the second and the third as it likes -: it is
// known by its size, copied to <system>/vita3k under the name the core knows it by, and taken only when the copy's
// SHA-256 is that of Sony's file. The copies are deleted once installed; the player's files are only read.
enum class Phase
{
  Idle,       // nothing asked yet (or a copy the player stopped)
  Copying,    // a package is copied
  Verifying,  // its SHA-256
  Done,       // the three files are there: the next Vita game installs them
  Failed,     // message says why; what was copied before stays
};
struct Status
{
  Phase phase = Phase::Idle;
  bool busy = false;    // its thread runs
  int part = 0;         // the package it is at
  int64_t done = 0;     // bytes of the three so far (those already there count)
  int64_t total = FILES_BYTES;
  std::string message;  // Failed: why, in plain words (English, for PS4Lang::T)
  int serial = 0;       // one more with every change
  // ((01.72) The last copy's packages whose file had Sony's size but another SHA-256: not Sony's (the panel says so).)
  bool not_sonys[PART_COUNT] = {};
};
// (01.72, the owner: «explicar un poco de los archivos pup en la consola cuando no existe») Each package's file and
// size, for the firmware's panel ("PSP2UPDAT_FONT.PUP", 56778752).
const char* PackageName(int part);
int64_t PackageBytes(int part);
// Which packages have a file of Sony's size in the PS Vita's game folders (or their "firmware" folder), whatever its
// name - not read, only listed: what Copy the Firmware would take.
void FindPlayerFiles(const std::vector<std::string>& folders, bool found[PART_COUNT]);
// Starts it (nothing when it runs already). folders: the PS Vita's game folders (Library::ConsoleFolders).
void StartCopy(const std::string& system_dir, const std::vector<std::string>& folders);
// Stops it (from any thread); a file half copied isn't kept.
void StopCopy();
Status GetStatus();
int Serial();
}  // namespace Vita
