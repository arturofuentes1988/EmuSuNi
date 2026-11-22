// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Cheat codes from libretro's collection (RetroArch's online updater: the .cht files of
// github.com/libretro/libretro-database, all in one cheats.zip on buildbot.libretro.com, plain
// HTTP), found for one game when the user asks (Cheats -> Find Cheats Online): the collection is
// downloaded once into data_dir/cache/cheats.zip (again after 30 days), the console's lists most
// like the game's name are offered (CoverDownload::Search; a PSP disc's ID first), and the codes
// the user picks from one of them join the game's own .cht file (Cheats::List::Add).
// (01.26, the owner) The Dreamcast's lists ("Sega - Dreamcast") are addresses and values: Open turns them into
// GameShark pairs (Cheats::Parse's dreamcast). The 3DS isn't in libretro's collection: its game's own file of
// github.com/iSharingan/CTRPF-AR-CHEAT-CODES (Action Replay codes, a file per title ID: Cheats/<game>/<title ID>.txt),
// the owner's choice - that collection has no license, so nothing of it is in the package, the repository or our
// server: the app finds the game's file in the repository's list (GitHub's API, kept in memory only) and downloads
// that file alone, when the player asks, into memory; only the codes the player picks are kept, in the game's .cht.
// No file for the game: none found (the player's own Azahar cheats file, <title ID>.txt, still works).
// (01.26, the owner) The Arcade's: FinalBurn Neo's own collection, github.com/finalburnneo/FBNeo-cheats (a file per set,
// cheats/<set>.ini; no license either: nothing of it in the package, the repository or our server). The game's file
// alone, when the player asks (Start's data_dir: the core's cheats folder), saved there as <set>.ini.new - the core
// reads its cheats as the game starts, so it is put in place before the next start (src/ArcadeCheats.h). No list to
// pick from: Done with Results() {"<set>.ini"} and CheatCount(), or none.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Cheats.h"
#include "Systems.h"

namespace CheatDownload
{
// ---- (01.57, the owner) The N64's cheats by the cartridge, not the file's name ----
// Mupen64Plus's mupencheat.txt (GPL-2.0-or-later, from mupen64plus-core/data: scripts/package.sh puts it in the
// package's n64/ folder) knows each cartridge by its header's two CRCs and its country: a game renamed is found all
// the same, and offline. Its list comes first among the results (CartridgeResults()), libretro's after it as before.
// A cheat with choices ("8013CD0E ???? 0001:"1 Lap",0002:"2 Laps"") is one entry per choice ("Laps Of Race: 2 Laps")
// for the player to pick; one with more than 64 choices, several kinds of them or more than 100 lines (the core reads
// 256 numbers at most) is left out, and said so by a title line.
struct N64Cart
{
  uint32_t crc1 = 0, crc2 = 0;
  uint8_t country = 0;  // the header's 0x3E: 'E' USA, 'J' Japan, 'P' Europe...
};
// The cartridge's header from its file - .z64, .v64 or .n64 (any byte order), or the first of them in a .zip.
bool ReadN64Cart(const std::string& path, N64Cart* cart);
// Its block in the list's text: where its "crc" line begins (npos: none), and its name ("gn").
size_t FindN64Cheats(const std::string& text, const N64Cart& cart, std::string* name);
// The block's cheats (choices made entries of their own), and how many were left out.
std::vector<Cheats::Entry> ParseN64Cheats(const std::string& text, size_t at, int* left_out);
// Where the list is ("/app0/n64/mupencheat.txt"; the PC's tests set another).
void SetN64CheatList(const std::string& path);

class Finder
{
public:
  enum class State
  {
    Idle,
    Downloading,  // the collection (DownloadedMb())
    Searching,
    Done,         // Results() (maybe none)
    Failed,       // Error()
  };

  Finder() = default;
  ~Finder();
  Finder(const Finder&) = delete;
  Finder& operator=(const Finder&) = delete;

  // Looks for the game's lists in a thread of its own (a search before is stopped first).
  void Start(const std::string& data_dir, SystemId system, const std::string& game_path, const std::string& title);
  // Stops the search (the menu went back).
  void Stop();
  State GetState() const { return m_state; }
  int DownloadedMb() const { return m_mb; }
  int TotalMb() const { return m_total_mb; }  // the collection's size as the server says it (0: not known yet)
  // Failed: what (Error(): "The cheat collection couldn't be downloaded") and why, in plain words (Reason(): no
  // internet connection, the server didn't answer or had a problem, the connection was cut, what came isn't the
  // collection - a network's sign-in page -, the PS4's storage is full...). English, for PS4Lang::T.
  std::string Error() const;
  std::string Reason() const;
  // The lists' names (their files' names without .cht), the most like the game first.
  const std::vector<std::string>& Results() const { return m_results; }
  // (01.57) How many of Results() - the first ones - are the N64 cartridge's own (mupencheat.txt): 0 or 1.
  size_t CartridgeResults() const { return m_cartridge; }
  // (01.26) Where the lists come from, for the screen: "libretro", or the 3DS's "CTRPF-AR-CHEAT-CODES (GitHub)".
  std::string Source() const;
  // (01.26) The 3DS game's title ID as its cheats' files name it ("0004000000053F00"; "" none or not read yet); the
  // Arcade game's set ("mslug").
  std::string TitleId() const;
  // (01.26) The Arcade's file downloaded: how many cheats it has.
  int CheatCount() const { return m_cheat_count; }

  // ---- One list's codes, to pick from (after Done) ----
  bool Open(size_t result, std::string* error);
  const std::string& OpenName() const { return m_open_name; }
  const std::vector<Cheats::Entry>& Codes() const { return m_codes; }
  // A title line of the list (a code of zeros or none: "[>>> FPS Cheats <<<]"), not a cheat.
  bool IsTitle(size_t index) const;
  bool Picked(size_t index) const { return index < m_picked.size() && m_picked[index]; }
  void Pick(size_t index, bool on);
  void PickAll(bool on);
  int PickedCount() const;
  std::vector<Cheats::Entry> PickedCodes() const;

private:
  void Run(std::string data_dir, SystemId system, std::string game_path, std::string title);
  void Run3ds(const std::string& game_path);  // (01.26)
  void RunArcade(const std::string& cheats_dir, const std::string& game_path);  // (01.26)

  SystemId m_system = SystemId::SNES;  // (01.26) the search's console
  std::string m_title_id;              // (01.26) the 3DS game's
  std::string m_3ds_text;              // (01.26) its file's text, downloaded (memory only)
  std::thread m_thread;
  std::atomic<State> m_state{State::Idle};
  std::atomic<int> m_mb{0}, m_total_mb{0}, m_cheat_count{0};
  std::atomic<bool> m_stop{false};
  mutable std::mutex m_mutex;
  std::string m_error, m_reason;
  std::string m_zip_path;
  std::vector<std::string> m_results;   // shown names
  std::vector<std::string> m_entries;   // their paths in the zip ((01.57) "mupencheat:<offset>": the N64 cartridge's)
  std::atomic<size_t> m_cartridge{0};
  std::string m_open_name;
  std::vector<Cheats::Entry> m_codes;
  std::vector<bool> m_picked;
};
}  // namespace CheatDownload
