// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (01.19, the owner: "Borrar" has to delete everything of the game - in v00.23 a deleted game could stay in the list
// with its cover, and opened it gave an error) Delete Game's work on the drive, apart from the menu (src/Menu.cpp's
// DeleteGames and ForgetGame): the PC tests run it as it is (local/herramientas/prueba-borrar).

#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "Library.h"

namespace GameDelete
{
struct Files
{
  std::vector<std::string> deleted;  // deleted now, or not there any more
  std::vector<std::string> failed;   // still there
  std::string error;                 // the first failure's
  bool own_gone = false;             // its own file (each disc's) isn't there any more: the game is gone
};
// The game's files (Library::GameFiles: its own, a .cue's or .gdi's tracks, an .m3u's discs, each disc's - in its
// folder, none another game there names), each deleted. One not there counts as deleted (a USB drive's exFAT names one
// file in two cases; one deleted by hand). The game is gone when its own file is, whatever else stayed: until 01.18
// one file of its list not deleted kept it in the lists without its own one.
Files DeleteFiles(const Library::Game& game);
// A game named after its folder ("Game Name/EBOOT.PBP", "Game Name/disc.gdi"): that folder, once it is empty - nothing
// else in it is ever deleted. True when it was removed.
bool RemoveEmptyFolder(const Library::Game& game);
// The pictures of a game's cover names (`names`, CoverFiles::Names' way) in `index` (name -> picture) that no other
// game shows (`kept`: the pictures of the remaining games' names): a title's picture another region's game shows stays.
std::set<std::string> OwnPictures(const std::vector<std::string>& names, const std::map<std::string, std::string>& index,
                                  const std::unordered_set<std::string>& kept);
// A "<Unix time> <path>" list (data_dir/played.ini) without the lines of `paths` (each line's path as from_stored
// gives it). True when it was written again.
bool DropPlayed(const std::string& file, const std::set<std::string>& paths,
                const std::function<std::string(const std::string&)>& from_stored);
}  // namespace GameDelete
