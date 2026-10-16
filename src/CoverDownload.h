// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Box art from libretro's thumbnails (thumbnails.libretro.com, the pictures of
// github.com/libretro-thumbnails: the "Named_Boxarts" of each console), as Dolphin for PS4 gets
// GameTDB's: each console's list of pictures once (kept for a week), each game matched by its name
// (its words in any order, its parts around " - ", close spellings; the region of the file name
// preferred), saved as covers/<game key>.png (covers/nes/<file name>.png for a NES game). A game
// without a box gets its title screen ("Named_Titles").

#pragma once

#include <atomic>
#include <functional>
#include <memory>
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
// A cover was written (game key, file).
using Saved = std::function<void(const std::string& key, const std::string& path)>;

// The games of any console; covers_dir is data_dir/covers. Waits while `pause` is set (a game
// is running), stops when `cancel` is set. A game with a name of the user's own (Rename) is
// matched by it, then by its file's. closest (Games Without Cover's "Closest for All"): a game no
// picture is named like gets the closest one when it is sure of it (Closest()); the doubtful ones
// are left for Find Cover.
Result Download(const std::vector<Library::Game>& games, const std::string& covers_dir,
                const std::string& ca_file, const Progress& progress, const Saved& saved,
                const std::atomic<bool>& pause, const std::atomic<bool>& cancel, bool closest = false);

// ---- One game's cover, chosen by hand (Triangle on a game -> Find Cover) ----
// A console's picture names: box art, or title screens (the list kept in covers/, else the
// server's). Empty when it can't be read (*error says why).
std::vector<std::string> PictureNames(SystemId system, bool title_screens, const std::string& covers_dir,
                                      const std::string& ca_file, std::string* error);
// The names most like a text, best first: its words, then close spellings.
std::vector<std::string> Search(const std::vector<std::string>& names, const std::string& text, size_t max);
// A console's picture names (PictureNames()) made ready for Closest(), once per list (about 0.2 s on
// the PS4). `names` must outlive it and stay as they are.
struct Prepared;
std::shared_ptr<const Prepared> Prepare(const std::vector<std::string>& names);
// The picture for a name: Download()'s own match (*sure), else the closest title when one is close
// enough: never another game of a series (a number one has and the other lacks), a word that names
// a game in common, or one name inside the other, or spelt alike. *sure: the same numbers and most
// of the words; else a guess to look at first. nullptr: none is close enough.
const std::string* Closest(const Prepared& list, const std::string& name, const Library::Game& game, bool* sure);
// One picture into a PNG file, made smaller to max_w x max_h when it is bigger.
bool DownloadPicture(SystemId system, bool title_screen, const std::string& name, const std::string& path,
                     uint32_t max_w, uint32_t max_h, const std::string& ca_file, std::string* error);
}  // namespace CoverDownload
