// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Portable data (00.87): the player's data on a USB drive, to take it to another PS4 with EmuSuNi and play there with
// it. Settings -> Keep My Data on the USB Drive copies it (never moving or deleting anything of the PS4's) into the
// folder EmuSuNi-Portable at the drive's root, whose file portable.ini (a random id of the data, the version that made
// it) marks it: a folder of that name alone isn't portable data (players keep games in folders called EmuSuNi; the
// covers' USB folder is EmuSuNi/covers - another name, so neither is ever taken for it).
//
// Where the player's data lives is decided once, at the start (main.cpp, before the settings are read), and used by
// what travels - saves/ (every core's: the cartridges' .srm, PPSSPP's memory stick, Flycast's VMUs, the Saturn's
// backup memory, Azahar's sdmc and nand...), states/ (with their pictures), cheats/, bezels/, covers/, themes/,
// xmb.ini, settings.ini, controls.ini, favorites.ini, names.ini, played.ini and cache/library.tsv (the game list, so
// another PS4 shows the games at once). What doesn't travel stays in /data/EmuSuNi on every PS4: the logs, the
// RetroAchievements login and history, the online profile and nickname (netplay.ini), the statistics' id (stats.ini),
// My Stats (this PS4's own numbers), the updater's files, the caches, the Roms folders, the switch files. BIOS files are
// never copied by the app: a console's core reads them from the drive's EmuSuNi-Portable/system when the player put
// them there (else from /data/EmuSuNi/system).
//
// The drive is known by its data's id. This PS4's answers are kept in /data/EmuSuNi/portable-drives.ini (never on the
// drive): home (turned on here), yes (asked: Yes), no, off (turned off here). A start looks at the drives only when
// this PS4 said yes to one (else no USB drive is touched before the menu's first frame, as before 00.87); the menu's
// own read of the drives finds the others and asks "Use the data on this drive?" (Yes restarts EmuSuNi with it).
// Paths in the files on the drive that refer to the drive are kept relative to it (src/DrivePaths.h): it can be /usb0
// on one PS4 and /usb1 on another.
//
// A drive taken out while its data is in use: the next write to it fails, the file goes to
// /data/EmuSuNi/portable-fallback/<id>/ instead (never over the PS4's own data: on a friend's PS4 that would be the
// friend's saves) with a warning on the screen; the next start with the drive copies the saves, states, cheats and
// settings written there to it when newer (a core's own folders - a PSP memory stick, a VMU - stay there: logged).
// Nothing here throws.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Portable
{
inline constexpr const char* FOLDER = "EmuSuNi-Portable";
inline constexpr const char* MARKER = "portable.ini";

// ---- The start (main.cpp) ----
// Decides where the player's data lives this session. local_dir: /data/EmuSuNi. usb_root: before "/usb0"... ("" on the
// PS4; a test's folder on the PC). The drives are looked at only when this PS4 said yes to one, on a thread given
// `wait_ms` (a drive that doesn't answer leaves this PS4's data for this session). A drive in use: what a session
// without it wrote in the fallback folder is copied to it first.
void Start(const std::string& local_dir, const std::string& usb_root = {}, int wait_ms = 8000);

std::string LocalDir();   // /data/EmuSuNi
std::string UserDir();    // the player's data now: the drive's folder, this PS4's, or (the drive gone) the fallback one
bool Active();            // the drive's data in use (and the drive still there)
bool Lost();              // the drive in use went away this session (saving to the fallback folder)
std::string DriveRoot();  // the drive in use this session ("/usb1"; "" none) - kept after it went away
std::string DriveId();
std::string DataFolder();  // DriveRoot()/EmuSuNi-Portable ("" none)
// A warning for the screen (the drive went away, a write to it failed), once: the menu's or the game's.
std::string TakeWarning();

// Paths written to the player's files: relative to the drive in use when they are on it (DrivePaths::Stored), and
// back. As they are without a drive in use (exactly as before 00.87).
std::string ToStored(const std::string& path);
std::string FromStored(const std::string& stored);

// The folder a console's core reads its BIOS from (its system directory): the drive's EmuSuNi-Portable/system when the
// drive in use has the console's BIOS files there, else /data/EmuSuNi/system. console_key: Systems.h's key ("ps1").
std::string SystemDir(const std::string& console_key);
// Where a BIOS is looked for by name (the Saturn's): system/<sub> and system of the drive in use, then this PS4's.
std::vector<std::string> BiosFolders(const std::string& sub);

// Is the drive in use still there (a look at its portable.ini)? A game's start and end, the menu's drives changing.
// Gone: the fallback from now on (and the warning). True without a drive in use.
bool CheckDrive();
// Log.h's write redirect (SetWriteRedirect): a write into the drive's folder that failed because the drive is gone is
// written in the fallback folder instead; "" for any other.
std::string Redirect(const std::string& failed_path);

// ---- The menu ----
// A drive with portable data (its portable.ini) and what this PS4 answered for it.
struct DriveData
{
  std::string root;    // "/usb0"
  std::string id;
  std::string answer;  // "home", "yes", "no", "off" or "" (never asked)
  bool in_use = false;
};
// The drives (Library::UsbDrives' roots) that hold portable data: one look at each one's portable.ini.
std::vector<DriveData> Look(const std::vector<std::string>& drives);
// This PS4's answer for a drive's data (portable-drives.ini).
void Remember(const std::string& id, const std::string& answer);
std::string AnswerFor(const std::string& id);
// The library's cache on the drive (00.87): who wrote it last. Each write by this PS4 gets a new random word, kept here
// (never an id of the PS4): the same word when the cache is read again says this PS4 wrote it - else another PS4 did,
// and its games on that PS4's own storage (or other drives) aren't this one's.
std::string NewWord();
std::string LibraryWriter();
void SetLibraryWriter(const std::string& word, const std::string& id = {});  // (id: the drive in use's when "")

// ---- Copies (on the menu's thread for them; also the start's) ----
enum class Copying
{
  ToDrive,    // this PS4's data to a drive: Turn On (paths on that drive made relative)
  Back,       // the drive's data to this PS4: Turn Off and Copy (paths made the drive's of now; no game list)
  Reconcile,  // the fallback folder to the drive: the saves, states, cheats, settings a session without it wrote
};
enum class Policy
{
  Missing,  // only what the other side hasn't got (a drive with portable data already: its files are kept)
  Newer,    // what it hasn't got, or has older (a copy begun before and cut; Turn Off's copy back)
};
struct CopyPlan
{
  Copying copying = Copying::ToDrive;
  Policy policy = Policy::Missing;
  std::string from, to;  // the two data folders
  std::string drive;     // the drive's root (its paths relative to it)
  std::string writer;    // ToDrive: the game list's writer word put in the copy of library.tsv
};
struct CopyProgress
{
  std::atomic<int> files{0};     // to look at
  std::atomic<int> looked{0};
  std::atomic<int> copied{0};    // written
  std::atomic<int> kept{0};      // there already and not copied: different (Missing) or newer there (Newer)
  std::atomic<int> same{0};      // there already, the same size and not older
  std::atomic<int> failed{0};
  std::atomic<int> refused{0};  // ToDrive: a name the drive doesn't take (" * : < > ? \ |): the file stays here
  std::atomic<uint64_t> bytes_total{0}, bytes_done{0};
  std::atomic<bool> listing{true};  // still finding the files
  std::atomic<bool> stop{false};    // asked to stop (at the next file)
  std::atomic<bool> done{false};
  std::mutex mutex;
  std::string error;  // the first failure: what and why
  std::vector<std::string> left;  // Reconcile: files left in the fallback folder (a core's own)
  std::vector<std::string> refused_names;  // the first 20 refused
};
// The copy (synchronous; nothing is deleted, every file through a temporary one then renamed). Stops at the first
// file that can't be written (a full drive). True when every file is there.
bool CopyData(const CopyPlan& plan, CopyProgress* progress);
// ToDrive's last step: the drive's portable.ini (its id kept when it has one; a new one else). The id, "" when it
// couldn't be written.
std::string MarkDrive(const std::string& drive_root);
// A drive's portable.ini: its id ("" none).
std::string DriveDataId(const std::string& drive_root);
// The drive's data folder (<drive>/EmuSuNi-Portable).
std::string FolderOf(const std::string& drive_root);
}  // namespace Portable
