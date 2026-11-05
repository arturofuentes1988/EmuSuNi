// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Local multiplayer (00.23): 3 to 5 players on one PS4, each with a DualShock 4 signed in to a PS4 user (the PS4 lets
// 4 users in at once, so 4 players at most). Player 1 is the user who started EmuSuNi, the others are the users signed
// in after them, each in the first free player while signed in (src/Emulator.cpp, Pads).
//
// Most consoles had two controller ports; their 3-5 player games needed an adapter in them, which the game's core
// emulates as a controller "device" (retro_set_controller_port_device) or an option. The game setting Multiplayer
// Adapter (Settings: "multiplayer_adapter", Auto / Off / On) plugs the console's in:
//   - Super Nintendo (Snes9x): the Super Multitap in port 2 - players 2 to 5 on it (00.22's SNES-only Multitap
//     setting). Auto: the games of the list below (their file name).
//   - NES (FCEUmm): the Four Score, or the Famicom's 4 player adapter (its expansion port) for a Famicom game. Auto:
//     FCEUmm's own two lists of the games that use them (by the cartridge's CRC32, the core's), or an NES 2.0 header's
//     expansion device.
//   - Genesis (Genesis Plus GX): Sega's Team Player in port 1 (players 1 to 4 on it, player 5 in port 2: the Team
//     Player's manual), or EA's 4 Way Play (both ports) for EA's games. Auto: a header that says the game takes the
//     Team Player ("4" among its I/O devices, as the core reads it), the lists below. A J-Cart game (Codemasters'
//     Micro Machines 2, Pete Sampras...: the core's list) has players 3 and 4 on the cartridge: no adapter, whatever
//     the setting.
//   - PlayStation (SwanStation): the Multitap in port 2 - player 1 in port 1 as without it, players 2 to 5 on it (a
//     game made without it sees player 2 through it as in port 2). Auto: the discs DuckStation's game database marks
//     multitap (by the disc's code, as SwanStation reads it as the game loads).
//   - Saturn (YabaSanshiro): the 6 Player Adaptor in both ports (players 1 to 6 on port 1's; the core's count of
//     players is right only with both). Auto: never (no list of its games): On plugs it in.
// The others need none: the N64, the Dreamcast, the NAOMI, the Atomiswave and the Arcade have 4 ports (their cores
// read players 3 and 4 always), the PC Engine's core always has the TurboTap's 5 (Beetle PCE Fast), the handhelds and
// the Master System none. Off leaves every port as the console without the adapter (two players, as before 00.23).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Systems.h"

namespace Multiplayer
{
enum class Adapter
{
  None,          // the console's own two ports
  Cartridge,     // the game's own: a Genesis J-Cart's two extra ports (no adapter can be plugged in then)
  SnesMultitap,  // Super Multitap in port 2
  FourScore,     // NES Four Score
  Famicom4P,     // the Famicom's 4 player adapter, in its expansion port
  TeamPlayer,    // Sega Team Player in port 1
  FourWayPlay,   // EA 4 Way Play, in both ports
  Ps1Multitap,   // PlayStation Multitap in port 2
  SaturnTap,     // Saturn 6 Player Adaptor in both ports
};

// What decides a game's adapter, read once as the game starts (the cartridge's bytes may be freed once it loaded).
struct Game
{
  SystemId system = SystemId::SNES;
  std::string name;       // its file's name, without the folder and the extension ("Super Bomberman 2 (USA)")
  std::string extension;  // lower case, with the dot (".fds")
  // The NES: the cartridge's CRC32 as FCEUmm counts it (PRG and CHR ROM, no header: its lists' key), its NES 2.0
  // header's expansion device (0: none said).
  bool nes_crc_known = false;
  uint32_t nes_crc = 0;
  unsigned nes_expansion = 0;
  // The Genesis's header: a Team Player game ('4' among its I/O devices), a six-button pad's ('6'), EA's (its
  // copyright "(C)T-50": Electronic Arts' licensee code), a J-Cart cartridge (the core's list).
  bool md_team_player = false, md_six_button = false, md_ea = false, md_jcart = false;
  // The PlayStation disc's code ("SCUS-94426"), once the core has read it ("" before).
  std::string disc_code;
};
// `path` is the game's file (a .zip's name stands for its ROM); `rom` its bytes as the core gets them (empty: a disc).
Game ReadGame(SystemId system, const std::string& path, const std::vector<uint8_t>& rom);

// Whether the console has the setting (an adapter to plug in or not).
bool HasAdapter(SystemId system);

struct Decision
{
  Adapter adapter = Adapter::None;
  std::string reason;  // why, for the log
};
// The adapter for the game with the setting's value ("auto", "off", "on").
Decision Decide(const std::string& setting, const Game& game);

const char* Name(Adapter adapter);  // "Four Score", for the log
int MaxPlayers(Adapter adapter);    // the players the console reads with it (2 without)

// The calls that put the adapter in the game's ports (retro_set_controller_port_device: port, device), after the game
// loaded and whenever the setting changes while it runs; after the app's own calls for ports 1 and 2 (src/Emulator.cpp:
// pads, a PlayStation's DualShocks). `before`: the adapter the core has now (None at a game's start; FCEUmm keeps its
// expansion port's device from one game to the next, so `nes_expansion_set` says whether one of this session's NES
// games had the Famicom's adapter put there - the next one is given its own device back).
std::vector<std::pair<unsigned, unsigned>> PortDevices(const Game& game, Adapter adapter, Adapter before,
                                                       bool nes_expansion_set);
// The core options the adapter sets (SwanStation's Multitap, YabaSanshiro's adaptors), over the core's other options
// (BuildVariables' map: the PlayStation's players 2 to 5 take player 1's pad options from it).
void AddVariables(SystemId system, Adapter adapter, std::map<std::string, std::string>* variables);

// Each player's light bar colour, as the PS4 gives its users': blue, red, green, pink (and an orange fifth).
struct Colour
{
  uint8_t r, g, b;
};
Colour PlayerColour(int player);  // from 0

// For the tests (local/herramientas/prueba-multijugador): the title as the lists compare it, and FCEUmm's CRC32.
std::string Normalized(const std::string& name);
bool NesCartCrc(const std::vector<uint8_t>& rom, uint32_t* crc);
}  // namespace Multiplayer
