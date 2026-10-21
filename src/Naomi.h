// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Flycast's arcade boards, played by the Dreamcast's core: Sega's NAOMI (and NAOMI 2) and Sammy's Atomiswave.
// A game is a MAME set, a .zip (or .7z) named after it (mvsc2.zip, kofxi.zip: the core reads it itself and finds
// the game by that name), its title the set's full name in Flycast's list, its covers named after it (libretro's
// "Sega - Naomi" and "Atomiswave" thumbnails), and it boots from its board's BIOS (naomi.zip, awbios.zip...),
// which the player brings: beside the games or in system/dc, where the core looks for it. The list is the core's
// own (hw/naomi/naomi_roms.cpp, given by scripts/build-dc-core.sh's flycast_emusuni_arcade_list); without the
// Dreamcast (EMUSUNI_DC) it is empty.
// A NAOMI game can also be a cartridge image (00.64): the whole cartridge dumped as one decrypted file, a .dat or a
// .bin, which Flycast plays as it is (naomi_cart.cpp's loadDecryptedRom) with naomi.zip - loose only: an archive
// it reads as a MAME set, by its name. Its header names the game in each region; the Library matches it with the
// set of that name for its title and covers.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Systems.h"

namespace Naomi
{
enum class Board
{
  Naomi,
  Naomi2,
  Atomiswave,
  SystemSp,  // Sega's System SP, the NAOMI's successor: in Flycast's list, on neither tab (another board)
};

struct Set
{
  std::string name;    // the set's name, the zip's ("mvsc2")
  std::string title;   // its full name: "Marvel vs. Capcom 2 New Age of Heroes (Export, Korea)"
  std::string parent;  // a clone's parent set ("mvsc2" for mvsc2u), else empty
  std::string bios;    // the zip of the BIOS it boots, without ".zip": "naomi", "awbios", "naomi2", "hod2bios"...
  std::string gdrom;   // a GD-ROM game's disc image, <set>/<gdrom>.chd beside its zip; else empty
  Board board = Board::Naomi;
  uint32_t size = 0;      // the cartridge's memory (the core takes it from the app's heap); a GD-ROM game's: 0
  bool vertical = false;  // its monitor stands upright (the picture is turned)
  bool analog = false;    // a wheel, a stick or pedals (the left stick and the triggers are analog then)
  bool gun = false;       // a light gun game (the left stick aims; Reload)
  bool linked = false;    // made for several boards linked together (here one of them plays)
};

// Every set of the core, read once, the first time (while no game runs: the Library's first scan).
const std::vector<Set>& Sets();
// The set a zip's name (without the extension, in any letters' case) is, else nullptr.
const Set* Find(const std::string& name);
// A clone's parent (nullptr for a parent, or an unknown one).
const Set* Parent(const Set& set);
// Is the set one of that console's games: the NAOMI's tab has the NAOMI and NAOMI 2 games, the Atomiswave's its own
// (the System SP's are on neither).
bool Plays(SystemId system, const Set& set);
// The game's title in the Library: a parent's full name; a clone's parent title plus what its own name adds, in
// parentheses (as Arcade::Title).
std::string Title(const Set& set);
// The region its full name says ("USA", "Japan", "Export"...), else empty.
std::string Region(const Set& set);
// Its board, as Information says it: "NAOMI", "NAOMI GD-ROM", "NAOMI 2", "NAOMI 2 GD-ROM", "Atomiswave", "System SP".
std::string BoardName(const Set& set);
// The BIOS's file: "naomi.zip".
std::string BiosZip(const Set& set);
// The GD-ROM image's path from the game's folder ("ikaruga/gdl-0010.chd"), else empty.
std::string GdromFile(const Set& set);
// What the game needs that isn't there, in the places the core looks: its BIOS's zip (beside the game or in
// system/dc), a clone's parent zip (beside it: "may", a non-merged set carries its parent's files) and a GD-ROM
// game's disc image (in a folder named after the set, or after its parent, beside the zip). None when all are there.
std::vector<std::string> Missing(const Set& set, const std::string& folder, const std::string& system_dir);
// The message for a game the core refused: its reason (Flycast's message, as it sent it) in the app's words, and
// where the files go. '\n' between the lines: each of English that fits the error page (the language file
// translates it), or data under such a line (a file's name, a path).
std::string LoadError(const std::string& core_message, const Set* set, const std::vector<std::string>& missing,
                      const std::string& system_dir);

// A NAOMI cartridge image's header (RomBootID, hw/naomi/naomi_cart.h): its titles for Japan, the USA and export,
// as written ("MARVEL VS CAPCOM 2 USA", " VIRTUA TENNIS 2 IN USA --"), and its monitor's way up.
struct Image
{
  std::string japan, usa, export_title;
  bool vertical = false;
};
// A .dat or .bin file's name (any letters' case): what a cartridge image is called.
bool IsImageName(const std::string& name);
// Is the file a NAOMI cartridge image (its header's board "NAOMI"; not a NAOMI 2's, an Atomiswave's or anything
// else of that extension), and its header.
bool ReadImage(const std::string& path, Image* image);
// The game an image is: the NAOMI cartridge set (not GD-ROM, parents first) whose name is the file's title (without
// its parentheses) or the header's, word for word or as its start ("Marvel vs. Capcom 2" for "Marvel vs. Capcom 2
// New Age of Heroes"); nullptr when none is.
const Set* MatchImage(const std::string& title, const Image& image);
// The image's title in the Library: its set's name without the parentheses when one matched, else the file's own
// title, else (a file named nothing useful) the header's name ("VIRTUA TENNIS 2 IN USA --" -> "Virtua Tennis 2").
std::string ImageTitle(const std::string& file_title, const Image& image, const Set* set);
// What an image needs and isn't there: naomi.zip (beside it or in system/dc, where the core looks).
std::vector<std::string> ImageMissing(const std::string& folder, const std::string& system_dir);
}  // namespace Naomi
