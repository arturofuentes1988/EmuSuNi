// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Cheat codes from libretro's collection (RetroArch's online updater: the .cht files of
// github.com/libretro/libretro-database, all in one cheats.zip on buildbot.libretro.com, plain
// HTTP), found for one game when the user asks (Cheats -> Find Cheats Online): the collection is
// downloaded once into data_dir/cache/cheats.zip (again after 30 days), the console's lists most
// like the game's name are offered (CoverDownload::Search; a PSP disc's ID first), and the codes
// the user picks from one of them join the game's own .cht file (Cheats::List::Add).

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Cheats.h"
#include "Systems.h"

namespace CheatDownload
{
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

  std::thread m_thread;
  std::atomic<State> m_state{State::Idle};
  std::atomic<int> m_mb{0}, m_total_mb{0};
  std::atomic<bool> m_stop{false};
  mutable std::mutex m_mutex;
  std::string m_error, m_reason;
  std::string m_zip_path;
  std::vector<std::string> m_results;   // shown names
  std::vector<std::string> m_entries;   // their paths in the zip
  std::string m_open_name;
  std::vector<Cheats::Entry> m_codes;
  std::vector<bool> m_picked;
};
}  // namespace CheatDownload
