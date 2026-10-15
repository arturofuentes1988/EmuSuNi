// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Box art from libretro's thumbnails (thumbnails.libretro.com, the pictures of
// github.com/libretro-thumbnails: "Named_Boxarts" of the Super Nintendo), as Dolphin for PS4 gets
// GameTDB's: the list of pictures once (kept for a week), each game matched by its name (its words
// in any order, its parts around " - ", close spellings; the region of the file name preferred),
// saved as covers/<game file name>.png. A game without a box gets its title screen
// ("Named_Titles").

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "Library.h"

namespace CoverDownload
{
struct Result
{
  int downloaded = 0, already = 0, not_found = 0, failed = 0;
  std::string error;
};
using Progress = std::function<void(int done, int total, const std::string& title)>;
// A cover was written (game id, file).
using Saved = std::function<void(const std::string& id, const std::string& path)>;

// Waits while `pause` is set (a game is running), stops when `cancel` is set.
Result Download(const std::vector<Library::Game>& games, const std::string& covers_dir,
                const std::string& ca_file, const Progress& progress, const Saved& saved,
                const std::atomic<bool>& pause, const std::atomic<bool>& cancel);
}  // namespace CoverDownload
