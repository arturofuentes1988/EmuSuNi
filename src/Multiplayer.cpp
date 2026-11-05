// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Multiplayer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

#include <zlib.h>

namespace Multiplayer
{
namespace
{
// libretro's devices (libretro.h: RETRO_DEVICE_SUBCLASS(base, id) = ((id + 1) << 8) | base) as each core names them.
constexpr unsigned DEVICE_NONE = 0, DEVICE_JOYPAD = 1, DEVICE_ANALOG = 5;
constexpr unsigned Subclass(unsigned base, unsigned id)
{
  return ((id + 1) << 8) | base;
}
constexpr unsigned SNES_MULTITAP = Subclass(DEVICE_JOYPAD, 0);    // Snes9x's "Multitap"
constexpr unsigned NES_GAMEPAD = Subclass(DEVICE_JOYPAD, 1);      // FCEUmm's players 3 and 4 plugged in
constexpr unsigned NES_FC_4PLAYERS = Subclass(DEVICE_JOYPAD, 2);  // its expansion port's 4 player adapter
constexpr unsigned NES_FC_AUTO = DEVICE_JOYPAD;                   // the expansion device of the game's own list
// Genesis Plus GX's "MD Joypad 3 Button + 4-WayPlay", "MD Joypad 6 Button + 4-WayPlay", "... + Teamplayer".
constexpr unsigned MD_WAYPLAY_3B = Subclass(DEVICE_JOYPAD, 3);
constexpr unsigned MD_WAYPLAY_6B = Subclass(DEVICE_JOYPAD, 4);
constexpr unsigned MD_TEAMPLAYER_3B = Subclass(DEVICE_JOYPAD, 5);
constexpr unsigned MD_TEAMPLAYER_6B = Subclass(DEVICE_JOYPAD, 6);
constexpr unsigned PS1_DUALSHOCK = Subclass(DEVICE_ANALOG, 0);  // SwanStation's analog controller

// FCEUmm's lists (libretro.c fourscore_db_list and famicom_4p_db_list, commit ac0d00a; the entries it leaves out
// under #if 0 left out too), by the cartridge's CRC32 (PRG and CHR ROM). The core plugs the adapter in for these games
// as they load, but reads players 3 and 4 only once the frontend plugs them in (ports 2 and 3), and it keeps "this game
// has 4 players" from one game to the next: so the app finds the game in the lists itself.
struct NesGame
{
  uint32_t crc;
  const char* title;
};
const NesGame FOUR_SCORE_GAMES[] = {
    {0x1ebb5b42, "Bomberman II (USA)"},
    {0xf99e37eb, "Chris Evert & Ivan Lendl in Top Players' Tennis (USA)"},
    {0x48b8ee58, "Four Players' Tennis (Europe)"},
    {0x79f688bc, "Gauntlet II (Europe)"},
    {0x1b71ccdb, "Gauntlet II (USA)"},
    {0x1352f1b9, "Greg Norman's Golf Power (USA)"},
    {0x2e6ee98d, "Harlem Globetrotters (USA)"},
    {0x05104517, "Ivan 'Ironman' Stewart's Super Off Road (Europe)"},
    {0x4b041b6b, "Ivan 'Ironman' Stewart's Super Off Road (USA)"},
    {0xf54b34bd, "Kings of the Beach - Professional Beach Volleyball (USA)"},
    {0xc6c2edb5, "Magic Johnson's Fast Break (USA)"},
    {0x0939852f, "M.U.L.E. (USA)"},
    {0x4e6b9078, "Micro Mages"},
    {0x2f698c4d, "Monster Truck Rally (USA)"},
    {0xb9b4d9e0, "NES Play Action Football (USA)"},
    {0xda2cb59a, "Nightmare on Elm Street, A (USA)"},
    {0x8da6667d, "Nintendo World Cup (Europe)"},
    {0x7c16f819, "Nintendo World Cup (Europe) (Rev A)"},
    {0x7f08d0d9, "Nintendo World Cup (Europe) (Rev B)"},
    {0xa22657fa, "Nintendo World Cup (USA)"},
    {0x308da987, "R.C. Pro-Am II (Europe)"},
    {0x9edd2159, "R.C. Pro-Am II (USA)"},
    {0x8fa6e92c, "Rackets & Rivals (Europe)"},
    {0xad0394f0, "Roundball - 2-on-2 Challenge (Europe)"},
    {0x6e4dcfd2, "Roundball - 2-on-2 Challenge (USA)"},
    {0x0abdd5ca, "Spot - The Video Game (Japan)"},
    {0xcfae9dfa, "Spot - The Video Game (USA)"},
    {0x0b8f8128, "Smash T.V. (Europe)"},
    {0x6ee94d32, "Smash T.V. (USA)"},
    {0xcf4487a2, "Super Jeopardy! (USA)"},
    {0xc05a63b2, "Super Spike V'Ball (Europe)"},
    {0xe840fd21, "Super Spike V'Ball (USA)"},
    {0x407d6ffd, "Super Spike V'Ball + Nintendo World Cup (USA)"},
    {0xd153caf6, "Swords and Serpents (Europe)"},
    {0x46135141, "Swords and Serpents (France)"},
    {0x3417ec46, "Swords and Serpents (USA)"},
    {0x69977c9e, "Battle City (Japan) (4 Players Hack)"},
    {0x2da5ece0, "Bomberman 3 (Homebrew)"},
    {0x90d2e9f0, "K.Y.F.F. (Homebrew)"},
    {0x1394ded0, "Super PakPak (Homebrew)"},
    {0x73298c87, "Super Mario Bros. + Tetris + Nintendo World Cup (Europe)"},
    {0xf46ef39a, "Super Mario Bros. + Tetris + Nintendo World Cup (Europe) (Rev A)"},
};
const NesGame FAMICOM_4P_GAMES[] = {
    {0xc39b3bb2, "Bakutoushi Patton-Kun (Japan) (FDS)"},
    {0x0c401790, "Bomber Man II (Japan)"},
    {0x9992f445, "Championship Bowling (Japan)"},
    {0x3e470fe0, "Downtown - Nekketsu Koushinkyoku - Soreyuke Daiundoukai (Japan)"},
    {0x4f032933, "Ike Ike! Nekketsu Hockey-bu - Subette Koronde Dairantou (Japan)"},
    {0x4b5177e9, "Kunio-kun no Nekketsu Soccer League (Japan)"},
    {0x9f03b11f, "Moero TwinBee - Cinnamon Hakase o Sukue! (Japan)"},
    {0x13205221, "Moero TwinBee - Cinnamon Hakase wo Sukue! (Japan) (FDS)"},
    {0x37e24797, "Nekketsu Kakutou Densetsu (Japan)"},
    {0x62c67984, "Nekketsu Koukou Dodgeball-bu (Japan)"},
    {0x88062d9a, "Nekketsu! Street Basket - Ganbare Dunk Heroes (Japan)"},
    {0x689971f9, "Super Dodge Ball (USA) (3-4p with Game Genie code GEUOLZZA)"},
    {0x4ff17864, "Super Dodge Ball (USA) (patched)"},
    {0x213cb3fb, "U.S. Championship V'Ball (Japan)"},
    {0xd7077d96, "U.S. Championship V'Ball (Japan) (Beta)"},
    {0xb1b16b8a, "Wit's (Japan)"},
};

const NesGame* FindNes(const NesGame* begin, const NesGame* end, uint32_t crc)
{
  const NesGame* found = std::find_if(begin, end, [crc](const NesGame& game) { return game.crc == crc; });
  return found == end ? nullptr : found;
}

// The Super Nintendo's games for 3 to 5 players at once with the Super Multitap (by title: the file's name as the lists
// compare it, Normalized). Snes9x has no list of its own. The libretro database's games for 3 or more players
// (metadat/maxusers) are where these come from, the ones that take turns on one pad (golf, pinball, board and quiz
// games) left out. A game made without the Multitap still reads player 2 through it, as the Multitap passes port 2's
// first pad on until a game asks for the others (Snes9x's MP5 with the I/O bit high, its value at power on).
const char* const SNES_MULTITAP_GAMES[] = {
    "Bakutou Dochers - Bumps-jima wa Oosawagi", "Barkley no Power Dunk", "Barkley Shut Up and Jam!",
    "Battle Cross", "Bill Walsh College Football", "Bomberman B-Daman", "Capcom's Soccer Shootout",
    "College Football USA 97", "College Slam", "Dino Dini's Soccer!", "Dragon - The Bruce Lee Story",
    "Dream Basketball - Dunk & Hoop", "Elite Soccer", "Emmitt Smith Football", "Eric Cantona Football Challenge",
    "ESPN National Hockey Night", "Fever Pitch Soccer", "FIFA 97", "FIFA 98 - Road to World Cup",
    "FIFA - Road to World Cup 98", "FIFA International Soccer", "FIFA Soccer 96", "FIFA Soccer 97", "Finalset",
    "Go! Go! Dodge League", "Hammer Lock Wrestling", "Harapeko Bakka", "Hat Trick Hero 2", "Head-On Soccer",
    "Hebereke no Oishii Puzzle wa Irimasenka", "International Superstar Soccer Deluxe", "J.League '96 Dream Stadium",
    "J.League Excite Stage '94", "J.League Excite Stage '95", "J.League Excite Stage '96",
    "J.League Super Soccer '95 - Jikkyou Stadium", "Jigsaw Party", "Jikkyou Power Pro Wrestling '96 - Max Voltage",
    "Jikkyou World Soccer 2 - Fighting Eleven", "JWP Joshi Pro Wres - Pure Wrestle Queens",
    "Kunio-kun no Dodge Ball Da yo Zenin Shuugou!", "Kunio-kun no Dodge Ball Da yo Zenin Shuugou! - Tournament Special",
    "Looney Tunes B-Ball", "Looney Tunes Basketball", "Madden NFL '94", "Madden NFL 95", "Madden NFL 96",
    "Madden NFL 97", "Madden NFL 98", "Micro Machines", "Micro Machines 2 - Turbo Tournament", "Multi Play Volleyball",
    "Muscle Bomber - The Body Explosion", "NBA Give 'n Go", "NBA Hang Time", "NBA Jam", "NBA Jam - Tournament Edition",
    "NBA Jikkyou Basket - Winning Dunk", "NBA Live 95", "NBA Live 96", "NBA Live 97", "NBA Live 98",
    "NFL Pro Football '94", "NFL Quarterback Club", "NFL Quarterback Club '95", "NFL Quarterback Club 96", "NHL '94",
    "NHL 95", "NHL 96", "NHL 97", "NHL 98", "NHL Pro Hockey '94", "Otoboke Ninja Colosseum", "Pieces",
    "Planet's Champ TG 3000, The", "Rap Jam - Volume One", "Saturday Night Slam Masters", "Secret of Mana",
    "Seiken Densetsu 2", "Shin Nihon Pro Wrestling Kounin - '95 Tokyo Dome Battle 7", "Smash Tennis", "Soccer Shootout",
    "Spark World", "Stardust Suplex", "Street Hockey '95", "Street Racer", "Street Sports - Street Hockey '95",
    "Striker", "Sugoi Hebereke", "Super Bomberman", "Super Bomberman - Panic Bomber W", "Super Bomberman 2",
    "Super Bomberman 3", "Super Bomberman 4", "Super Bomberman 5", "Super Family Tennis", "Super Final Match Tennis",
    "Super Fire Pro Wrestling - Queen's Special", "Super Fire Pro Wrestling 2", "Super Fire Pro Wrestling 3",
    "Super Fire Pro Wrestling III - Easy Type", "Super Fire Pro Wrestling III - Final Bout",
    "Super Fire Pro Wrestling Special", "Super Fire Pro Wrestling X", "Super Formation Soccer 94 - World Cup Edition",
    "Super Formation Soccer 94 - World Cup Final Data", "Super Formation Soccer 95 - della Serie A",
    "Super Formation Soccer 96 - World Club Edition", "Super Formation Soccer II", "Super Puyo Puyo Tsuu",
    "Super Puyo Puyo Tsuu Remix", "Super Tekkyuu Fight!", "Super Tetris 3",
    "Tenryuu Genichirou no Pro Wres Revolution", "Tiny Toon Adventures - Dotabata Daiundoukai",
    "Tiny Toon Adventures - Wacky Sports Challenge", "Tiny Toon Adventures - Wild & Wacky Sports", "Top Gear 3000",
    "Turbo Toons", "Vs. Collection", "Waratte Iitomo! Tamolympic", "World Cup Striker",
    "World Soccer 94 - Road to Glory",
    "WWF Raw", "Yuujin no Furi Furi Girls", "Zen-Nihon Pro Wres 2 - 3-4 Budoukan",
    "Zen-Nihon Pro Wres' - Sekai Saikyou Tag",
};

// The Genesis's games for 3 or 4 players with EA's 4 Way Play (Electronic Arts' own: the adapter is theirs, made
// for their sports games; Sega's first Team Player can't play them). From the libretro database's games for 3 or more
// players (as the Super Nintendo's). A game of these is given the 4 Way Play whatever its header says.
const char* const MD_4WAYPLAY_GAMES[] = {
    "Australian Rugby League", "Bill Walsh College Football", "Bill Walsh College Football 95",
    "Coach K College Basketball", "College Football USA 96", "College Football USA 97", "Elitserien 95",
    "Elitserien 96", "FIFA 98 - Road to World Cup", "FIFA International Soccer", "FIFA Soccer 95", "FIFA Soccer 96",
    "FIFA Soccer 97", "General Chaos", "General Chaos Daikonsen", "IMG International Tour Tennis", "Madden NFL '94",
    "Madden NFL 95", "Madden NFL 96", "Madden NFL 97", "Madden NFL 98", "Mutant League Hockey", "NBA Live 95",
    "NBA Live 96", "NBA Live 97", "NBA Live 98", "NBA Showdown '94", "NHL '94", "NHL 95", "NHL 96", "NHL 97", "NHL 98",
    "Rugby World Cup 95",
};
// Its games for the Team Player (in port 1, as its manual says) whose header may not say so; the header's '4' finds
// the others.
const char* const MD_TEAM_PLAYER_GAMES[] = {
    "Barkley Shut Up and Jam!", "Barkley Shut Up and Jam 2", "College Slam", "Columns III",
    "Columns III - Taiketsu! Columns World", "Dino Dini's Soccer", "Dragon - The Bruce Lee Story",
    "ESPN National Hockey Night", "Gauntlet IV", "International Superstar Soccer Deluxe", "J. League Pro Striker",
    "J. League Pro Striker 2", "J. League Pro Striker Perfect", "Lost Vikings, The", "Mega Bomberman",
    "NBA Action '94", "NBA Action '95 Starring David Robinson", "NBA Hang Time", "NBA Jam",
    "NBA Jam - Tournament Edition", "NFL '95", "NFL 98", "NFL Quarterback Club", "NFL Quarterback Club 96",
    "Pepenga Pengo", "Prime Time NFL Starring Deion Sanders", "Pro Striker - Final Stage", "Street Racer",
    "Tiny Toon Adventures - ACME All-Stars", "Ultimate Soccer", "Wayne Gretzky and the NHLPA All-Stars",
    "Wimbledon", "Wimbledon Championship Tennis", "World Championship Soccer II", "World Cup USA 94", "WWF RAW",
    "Yu Yu Hakusho - Makyou Toitsusen",
};

bool InList(const char* const* begin, const char* const* end, const std::string& name)
{
  const std::string title = Normalized(name);
  return !title.empty() &&
         std::any_of(begin, end, [&title](const char* entry) { return Normalized(entry) == title; });
}

// The PlayStation discs DuckStation's game database (stenzek/duckstation data/resources/gamedb.yaml, October 2026)
// marks "multitap: true" - their codes and the other codes of the same entries, as SwanStation (DuckStation's fork)
// reads them from the disc's SYSTEM.CNF. SwanStation has no list of its own. One text, each code between spaces.
const char* const PS1_MULTITAP_DISCS =
    " HPS-105 HPS-91 SCES-00067 SCES-00263 SCES-00269 SCES-00392 SCES-00408 SCES-00623 SCES-00983 SCES-01022 "
    "SCES-01043 SCES-01077 SCES-01078 SCES-01079 SCES-01205 SCES-01206 SCES-01312 SCES-01443 SCES-01630 SCES-01700 "
    "SCES-01701 SCES-01702 SCES-01703 SCES-01704 SCES-01736 SCES-01763 SCES-01833 SCES-01882 SCES-01923 SCES-02060 "
    "SCES-02105 SCES-02146 SCES-02269 SCES-02451 SCES-02834 SCES-02986 SCES-03070 SCES-03071 SCES-03072 SCES-03073 "
    "SCES-03074 SCES-03075 SCES-03076 SCES-03085 SCES-03086 SCES-03087 SCES-03236 SCES-03705 SCES-03706 SCES-03745 "
    "SCES-03746 SCES-03747 SCES-03748 SCES-03749 SCES-03922 SCPS-10042 SCPS-10051 SCPS-10093 SCPS-10118 SCPS-10135 "
    "SCPS-10140 SCPS-45034 SCPS-45168 SCPS-45197 SCPS-45268 SCPS-45421 SCPS-45470 SCPS-45507 SCPS-45508 SCPS-45509 "
    "SCPS-45511 SCPS-45516 SCPS-91126 SCPS-91166 SCPS-91197 SCPS-91230 SCPS-91303 SCPS-91319 SCPS-91328 SCUS-94156 "
    "SCUS-94171 SCUS-94172 SCUS-94173 SCUS-94174 SCUS-94188 SCUS-94234 SCUS-94235 SCUS-94246 SCUS-94249 SCUS-94264 "
    "SCUS-94272 SCUS-94274 SCUS-94308 SCUS-94361 SCUS-94420 SCUS-94426 SCUS-94476 SCUS-94500 SCUS-94502 SCUS-94509 "
    "SCUS-94510 SCUS-94550 SCUS-94551 SCUS-94552 SCUS-94556 SCUS-94557 SCUS-94558 SCUS-94560 SCUS-94561 SCUS-94562 "
    "SCUS-94563 SCUS-94570 SCUS-94573 SCUS-94575 SCUS-94577 SCUS-94579 SCUS-94581 SCUS-94639 SCUS-94641 SCUS-94665 "
    "SCUS-94673 SCUS-94690 SCUS-94691 SCUS-94695 SIPS-60008 SIPS-60015 SIPS-60018 SIPS-60022 SLES-00015 SLES-00016 "
    "SLES-00021 SLES-00023 SLES-00044 SLES-00068 SLES-00076 SLES-00080 SLES-00095 SLES-00096 SLES-00110 SLES-00113 "
    "SLES-00116 SLES-00171 SLES-00188 SLES-00189 SLES-00201 SLES-00212 SLES-00213 SLES-00217 SLES-00225 SLES-00256 "
    "SLES-00258 SLES-00265 SLES-00270 SLES-00284 SLES-00286 SLES-00296 SLES-00309 SLES-00310 SLES-00313 SLES-00333 "
    "SLES-00377 SLES-00418 SLES-00436 SLES-00449 SLES-00472 SLES-00476 SLES-00492 SLES-00504 SLES-00505 SLES-00506 "
    "SLES-00511 SLES-00512 SLES-00517 SLES-00529 SLES-00533 SLES-00534 SLES-00548 SLES-00560 SLES-00611 SLES-00624 "
    "SLES-00626 SLES-00691 SLES-00704 SLES-00707 SLES-00712 SLES-00715 SLES-00717 SLES-00747 SLES-00753 SLES-00759 "
    "SLES-00761 SLES-00785 SLES-00804 SLES-00845 SLES-00857 SLES-00865 SLES-00882 SLES-00904 SLES-00906 SLES-00907 "
    "SLES-00914 SLES-00915 SLES-00916 SLES-00917 SLES-00918 SLES-00935 SLES-00945 SLES-00951 SLES-00952 SLES-00999 "
    "SLES-01003 SLES-01028 SLES-01029 SLES-01039 SLES-01041 SLES-01042 SLES-01061 SLES-01062 SLES-01103 SLES-01160 "
    "SLES-01166 SLES-01167 SLES-01175 SLES-01203 SLES-01210 SLES-01226 SLES-01239 SLES-01241 SLES-01265 SLES-01266 "
    "SLES-01267 SLES-01268 SLES-01269 SLES-01273 SLES-01274 SLES-01286 SLES-01327 SLES-01328 SLES-01341 SLES-01376 "
    "SLES-01396 SLES-01427 SLES-01433 SLES-01436 SLES-01445 SLES-01446 SLES-01454 SLES-01455 SLES-01456 SLES-01457 "
    "SLES-01458 SLES-01534 SLES-01536 SLES-01537 SLES-01559 SLES-01576 SLES-01584 SLES-01585 SLES-01586 SLES-01587 "
    "SLES-01588 SLES-01594 SLES-01622 SLES-01644 SLES-01645 SLES-01646 SLES-01647 SLES-01658 SLES-01674 SLES-01675 "
    "SLES-01733 SLES-01744 SLES-01745 SLES-01746 SLES-01747 SLES-01875 SLES-01893 SLES-01907 SLES-01921 SLES-01970 "
    "SLES-01972 SLES-01980 SLES-02020 SLES-02021 SLES-02061 SLES-02095 SLES-02142 SLES-02143 SLES-02170 SLES-02171 "
    "SLES-02172 SLES-02173 SLES-02192 SLES-02193 SLES-02194 SLES-02224 SLES-02225 SLES-02226 SLES-02227 SLES-02255 "
    "SLES-02272 SLES-02292 SLES-02293 SLES-02298 SLES-02299 SLES-02315 SLES-02316 SLES-02317 SLES-02318 SLES-02319 "
    "SLES-02320 SLES-02321 SLES-02335 SLES-02336 SLES-02339 SLES-02358 SLES-02359 SLES-02360 SLES-02361 SLES-02362 "
    "SLES-02370 SLES-02371 SLES-02396 SLES-02424 SLES-02448 SLES-02476 SLES-02499 SLES-02513 SLES-02514 SLES-02535 "
    "SLES-02536 SLES-02537 SLES-02550 SLES-02551 SLES-02552 SLES-02563 SLES-02572 SLES-02573 SLES-02577 SLES-02578 "
    "SLES-02579 SLES-02580 SLES-02581 SLES-02595 SLES-02618 SLES-02619 SLES-02689 SLES-02693 SLES-02697 SLES-02700 "
    "SLES-02702 SLES-02704 SLES-02705 SLES-02706 SLES-02707 SLES-02708 SLES-02764 SLES-02765 SLES-02766 SLES-02767 "
    "SLES-02768 SLES-02769 SLES-02806 SLES-02807 SLES-02808 SLES-02823 SLES-02825 SLES-02849 SLES-02853 SLES-02854 "
    "SLES-02857 SLES-02858 SLES-02859 SLES-02860 SLES-02861 SLES-02918 SLES-02948 SLES-02950 SLES-02988 SLES-03063 "
    "SLES-03064 SLES-03065 SLES-03067 SLES-03069 SLES-03082 SLES-03084 SLES-03128 SLES-03129 SLES-03130 SLES-03131 "
    "SLES-03132 SLES-03139 SLES-03140 SLES-03141 SLES-03142 SLES-03143 SLES-03144 SLES-03145 SLES-03146 SLES-03148 "
    "SLES-03149 SLES-03154 SLES-03155 SLES-03206 SLES-03210 SLES-03246 SLES-03251 SLES-03262 SLES-03280 SLES-03281 "
    "SLES-03282 SLES-03283 SLES-03321 SLES-03322 SLES-03327 SLES-03328 SLES-03329 SLES-03337 SLES-03341 SLES-03362 "
    "SLES-03480 SLES-03481 SLES-03482 SLES-03483 SLES-03488 SLES-03499 SLES-03511 SLES-03579 SLES-03580 SLES-03581 "
    "SLES-03582 SLES-03583 SLES-03584 SLES-03585 SLES-03592 SLES-03593 SLES-03594 SLES-03595 SLES-03642 SLES-03666 "
    "SLES-03668 SLES-03669 SLES-03671 SLES-03672 SLES-03682 SLES-03718 SLES-03719 SLES-03720 SLES-03721 SLES-03722 "
    "SLES-03729 SLES-03751 SLES-03776 SLES-03779 SLES-03795 SLES-03796 SLES-03806 SLES-03808 SLES-03813 SLES-03830 "
    "SLES-03831 SLES-03832 SLES-03833 SLES-03834 SLES-03856 SLES-03921 SLES-03946 SLES-03957 SLES-03963 SLES-03968 "
    "SLES-03969 SLES-03970 SLES-03971 SLES-03977 SLES-03978 SLES-03979 SLES-03980 SLES-03981 SLES-03982 SLES-03989 "
    "SLES-04047 SLES-04060 SLES-04104 SLES-04115 SLES-04116 SLES-04117 SLES-04118 SLES-04119 SLES-04165 SLES-04166 "
    "SLES-04167 SLES-04168 SLES-04169 SLES-11436 SLES-13813 SLPM-86002 SLPM-86011 SLPM-86016 SLPM-86034 SLPM-86044 "
    "SLPM-86056 SLPM-86060 SLPM-86061 SLPM-86063 SLPM-86078 SLPM-86080 SLPM-86120 SLPM-86123 SLPM-86132 SLPM-86170 "
    "SLPM-86176 SLPM-86193 SLPM-86223 SLPM-86240 SLPM-86259 SLPM-86269 SLPM-86270 SLPM-86272 SLPM-86276 SLPM-86291 "
    "SLPM-86297 SLPM-86317 SLPM-86365 SLPM-86392 SLPM-86397 SLPM-86400 SLPM-86403 SLPM-86442 SLPM-86464 SLPM-86482 "
    "SLPM-86487 SLPM-86538 SLPM-86539 SLPM-86541 SLPM-86543 SLPM-86544 SLPM-86584 SLPM-86590 SLPM-86600 SLPM-86617 "
    "SLPM-86620 SLPM-86621 SLPM-86626 SLPM-86643 SLPM-86651 SLPM-86657 SLPM-86658 SLPM-86664 SLPM-86668 SLPM-86690 "
    "SLPM-86696 SLPM-86713 SLPM-86714 SLPM-86734 SLPM-86736 SLPM-86750 SLPM-86757 SLPM-86758 SLPM-86761 SLPM-86799 "
    "SLPM-86814 SLPM-86820 SLPM-86824 SLPM-86825 SLPM-86829 SLPM-86835 SLPM-86842 SLPM-86844 SLPM-86864 SLPM-86868 "
    "SLPM-86871 SLPM-86888 SLPM-86899 SLPM-86908 SLPM-86915 SLPM-86944 SLPM-86950 SLPM-86963 SLPM-86989 SLPM-87015 "
    "SLPM-87017 SLPM-87018 SLPM-87032 SLPM-87056 SLPM-87080 SLPM-87097 SLPM-87098 SLPM-87133 SLPM-87135 SLPM-87156 "
    "SLPM-87187 SLPM-87195 SLPM-87201 SLPM-87205 SLPM-87206 SLPM-87208 SLPM-87241 SLPM-87242 SLPM-87251 SLPM-87257 "
    "SLPM-87285 SLPM-87316 SLPM-87340 SLPM-87344 SLPM-87345 SLPM-87346 SLPM-87347 SLPM-87348 SLPM-87351 SLPM-87361 "
    "SLPM-87363 SLPM-87364 SLPM-87368 SLPS-00089 SLPS-00115 SLPS-00127 SLPS-00155 SLPS-00158 SLPS-00164 SLPS-00188 "
    "SLPS-00199 SLPS-00202 SLPS-00221 SLPS-00236 SLPS-00237 SLPS-00241 SLPS-00263 SLPS-00270 SLPS-00278 SLPS-00298 "
    "SLPS-00299 SLPS-00301 SLPS-00321 SLPS-00359 SLPS-00381 SLPS-00382 SLPS-00389 SLPS-00426 SLPS-00450 SLPS-00465 "
    "SLPS-00467 SLPS-00469 SLPS-00523 SLPS-00531 SLPS-00569 SLPS-00594 SLPS-00595 SLPS-00607 SLPS-00610 SLPS-00615 "
    "SLPS-00622 SLPS-00623 SLPS-00626 SLPS-00637 SLPS-00648 SLPS-00652 SLPS-00658 SLPS-00682 SLPS-00686 SLPS-00695 "
    "SLPS-00697 SLPS-00699 SLPS-00732 SLPS-00736 SLPS-00741 SLPS-00768 SLPS-00785 SLPS-00821 SLPS-00832 SLPS-00858 "
    "SLPS-00859 SLPS-00861 SLPS-00878 SLPS-00887 SLPS-00892 SLPS-00899 SLPS-00915 SLPS-00918 SLPS-00968 SLPS-01006 "
    "SLPS-01012 SLPS-01025 SLPS-01026 SLPS-01034 SLPS-01090 SLPS-01111 SLPS-01113 SLPS-01121 SLPS-01155 SLPS-01156 "
    "SLPS-01162 SLPS-01166 SLPS-01167 SLPS-01173 SLPS-01177 SLPS-01209 SLPS-01215 SLPS-01240 SLPS-01241 SLPS-01242 "
    "SLPS-01243 SLPS-01264 SLPS-01265 SLPS-01268 SLPS-01283 SLPS-01287 SLPS-01296 SLPS-01303 SLPS-01306 SLPS-01314 "
    "SLPS-01361 SLPS-01366 SLPS-01379 SLPS-01383 SLPS-01389 SLPS-01399 SLPS-01414 SLPS-01438 SLPS-01440 SLPS-01473 "
    "SLPS-01483 SLPS-01486 SLPS-01505 SLPS-01522 SLPS-01546 SLPS-01559 SLPS-01560 SLPS-01561 SLPS-01567 SLPS-01576 "
    "SLPS-01586 SLPS-01621 SLPS-01624 SLPS-01647 SLPS-01667 SLPS-01682 SLPS-01693 SLPS-01695 SLPS-01701 SLPS-01715 "
    "SLPS-01717 SLPS-01719 SLPS-01730 SLPS-01740 SLPS-01789 SLPS-01802 SLPS-01820 SLPS-01849 SLPS-01904 SLPS-01937 "
    "SLPS-01962 SLPS-01983 SLPS-02011 SLPS-02012 SLPS-02033 SLPS-02057 SLPS-02065 SLPS-02075 SLPS-02105 SLPS-02120 "
    "SLPS-02135 SLPS-02168 SLPS-02189 SLPS-02200 SLPS-02201 SLPS-02202 SLPS-02225 SLPS-02228 SLPS-02260 SLPS-02282 "
    "SLPS-02309 SLPS-02310 SLPS-02346 SLPS-02456 SLPS-02457 SLPS-02458 SLPS-02469 SLPS-02491 SLPS-02510 SLPS-02516 "
    "SLPS-02552 SLPS-02554 SLPS-02566 SLPS-02567 SLPS-02603 SLPS-02612 SLPS-02653 SLPS-02675 SLPS-02687 SLPS-02710 "
    "SLPS-02731 SLPS-02751 SLPS-02756 SLPS-02796 SLPS-02797 SLPS-02817 SLPS-02851 SLPS-02852 SLPS-02857 SLPS-02877 "
    "SLPS-02885 SLPS-02900 SLPS-02901 SLPS-02902 SLPS-02903 SLPS-02909 SLPS-02912 SLPS-02913 SLPS-02914 SLPS-02915 "
    "SLPS-02934 SLPS-02943 SLPS-02983 SLPS-03000 SLPS-03001 SLPS-03002 SLPS-03045 SLPS-03046 SLPS-03047 SLPS-03049 "
    "SLPS-03050 SLPS-03051 SLPS-03052 SLPS-03056 SLPS-03070 SLPS-03071 SLPS-03098 SLPS-03114 SLPS-03122 SLPS-03162 "
    "SLPS-03195 SLPS-03206 SLPS-03207 SLPS-03250 SLPS-03266 SLPS-03303 SLPS-03321 SLPS-03362 SLPS-03364 SLPS-03384 "
    "SLPS-03433 SLPS-03436 SLPS-03458 SLPS-03460 SLPS-03491 SLPS-03494 SLPS-03553 SLPS-03575 SLPS-91025 SLPS-91036 "
    "SLPS-91046 SLPS-91053 SLPS-91065 SLPS-91066 SLPS-91115 SLPS-91149 SLPS-91150 SLPS-91178 SLPS-91190 SLPS-91247 "
    "SLPS-91248 SLPS-91249 SLPS-91407 SLPS-91416 SLPS-91417 SLPS-91428 SLPS-91429 SLPS-91450 SLPS-91451 SLPS-91452 "
    "SLPS-91467 SLPS-91476 SLPS-91484 SLPS-91488 SLPS-91497 SLPS-91515 SLPS-91529 SLUS-00002 SLUS-00011 SLUS-00018 "
    "SLUS-00022 SLUS-00030 SLUS-00038 SLUS-00048 SLUS-00060 SLUS-00099 SLUS-00142 SLUS-00146 SLUS-00147 SLUS-00148 "
    "SLUS-00156 SLUS-00196 SLUS-00198 SLUS-00210 SLUS-00219 SLUS-00227 SLUS-00237 SLUS-00238 SLUS-00243 SLUS-00246 "
    "SLUS-00262 SLUS-00267 SLUS-00269 SLUS-00294 SLUS-00300 SLUS-00327 SLUS-00329 SLUS-00377 SLUS-00388 SLUS-00391 "
    "SLUS-00395 SLUS-00445 SLUS-00449 SLUS-00465 SLUS-00486 SLUS-00492 SLUS-00495 SLUS-00500 SLUS-00506 SLUS-00507 "
    "SLUS-00514 SLUS-00516 SLUS-00519 SLUS-00520 SLUS-00523 SLUS-00526 SLUS-00528 SLUS-00547 SLUS-00558 SLUS-00559 "
    "SLUS-00571 SLUS-00591 SLUS-00596 SLUS-00616 SLUS-00618 SLUS-00635 SLUS-00636 SLUS-00644 SLUS-00656 SLUS-00657 "
    "SLUS-00659 SLUS-00672 SLUS-00680 SLUS-00681 SLUS-00682 SLUS-00688 SLUS-00692 SLUS-00697 SLUS-00729 SLUS-00735 "
    "SLUS-00736 SLUS-00742 SLUS-00757 SLUS-00769 SLUS-00771 SLUS-00782 SLUS-00785 SLUS-00791 SLUS-00805 SLUS-00817 "
    "SLUS-00825 SLUS-00831 SLUS-00836 SLUS-00856 SLUS-00861 SLUS-00864 SLUS-00869 SLUS-00889 SLUS-00912 SLUS-00925 "
    "SLUS-00926 SLUS-00927 SLUS-00929 SLUS-00932 SLUS-00948 SLUS-00952 SLUS-00953 SLUS-00955 SLUS-00961 SLUS-00963 "
    "SLUS-00965 SLUS-00970 SLUS-00987 SLUS-00994 SLUS-00997 SLUS-00998 SLUS-01003 SLUS-01004 SLUS-01006 SLUS-01014 "
    "SLUS-01018 SLUS-01023 SLUS-01028 SLUS-01045 SLUS-01053 SLUS-01054 SLUS-01065 SLUS-01078 SLUS-01089 SLUS-01094 "
    "SLUS-01104 SLUS-01125 SLUS-01129 SLUS-01130 SLUS-01139 SLUS-01146 SLUS-01169 SLUS-01170 SLUS-01172 SLUS-01173 "
    "SLUS-01174 SLUS-01177 SLUS-01178 SLUS-01186 SLUS-01189 SLUS-01195 SLUS-01196 SLUS-01219 SLUS-01234 SLUS-01241 "
    "SLUS-01262 SLUS-01264 SLUS-01267 SLUS-01271 SLUS-01273 SLUS-01293 SLUS-01320 SLUS-01331 SLUS-01333 SLUS-01335 "
    "SLUS-01348 SLUS-01355 SLUS-01367 SLUS-01368 SLUS-01371 SLUS-01402 SLUS-01408 SLUS-01416 SLUS-01422 SLUS-01427 "
    "SLUS-01437 SLUS-01445 SLUS-01449 SLUS-01454 SLUS-01455 SLUS-01461 SLUS-01482 SLUS-01483 SLUS-01504 SLUS-01570 "
    "SLUS-01578 SLUS-01584 SLUS-01585 SLUS-80889 ";

bool Ps1MultitapDisc(const std::string& code)
{
  return code.size() >= 4 && std::strstr(PS1_MULTITAP_DISCS, (" " + code + " ").c_str()) != nullptr;
}

// A name's "(...)" or "[...]" tag says this ("(Japan)", "(J)", "(USA, Japan)").
bool HasTag(const std::string& name, const char* const* tags, size_t count)
{
  size_t at = 0;
  while ((at = name.find_first_of("([", at)) != std::string::npos)
  {
    const size_t end = name.find_first_of(")]", at);
    if (end == std::string::npos)
      break;
    std::string inside = name.substr(at + 1, end - at - 1);
    // Each of its parts: "USA, Japan" -> "USA", "Japan".
    size_t from = 0;
    while (from <= inside.size())
    {
      size_t comma = inside.find(',', from);
      if (comma == std::string::npos)
        comma = inside.size();
      std::string part = inside.substr(from, comma - from);
      part.erase(0, part.find_first_not_of(' '));
      part.erase(part.find_last_not_of(' ') + 1);
      for (size_t i = 0; i < count; i++)
      {
        if (part == tags[i])
          return true;
      }
      from = comma + 1;
    }
    at = end + 1;
  }
  return false;
}

bool JapaneseName(const std::string& name)
{
  static const char* const JAPAN[] = {"Japan", "J", "JP", "Jpn"};
  return HasTag(name, JAPAN, std::size(JAPAN));
}

uint32_t BigEndian32(const std::vector<uint8_t>& rom, size_t at)
{
  if (rom.size() < at + 4)
    return 0;
  return uint32_t(rom[at]) << 24 | uint32_t(rom[at + 1]) << 16 | uint32_t(rom[at + 2]) << 8 | rom[at + 3];
}

// Genesis Plus GX's J-Cart games (core/cart_hw/eeprom_i2c.c: the entries with mapper_i2c_jcart_init, but the Brian Lara
// Cricket games, which have its EEPROM and no J-Cart), found as it finds them: the header's product code (0x182, 12
// bytes) holds the id, and the header's checksum (0x18E) and the ROM's first long word (the stack) are the entry's
// where it gives them.
bool MdJcart(const std::vector<uint8_t>& rom)
{
  struct Entry
  {
    const char* id;
    uint32_t sp;
    uint16_t checksum;
  };
  static const Entry JCART[] = {
      {"00000000", 0x444e4c44, 0x168B},  // Micro Machines Military
      {"00000000", 0x444e4c44, 0x165E},  // Micro Machines Turbo Tournament 96
      {"T-120096", 0, 0},                // Micro Machines 2 - Turbo Tournament
      {"00000000", 0xfffffffc, 0x168B},  // Super Skidmarks
      {"00000000", 0xfffffffc, 0x165E},  // Pete Sampras Tennis (Prototype)
      {"T-120066", 0, 0},                // Pete Sampras Tennis
      {"T-123456", 0, 0},                // Pete Sampras Tennis 96
      {"XXXXXXXX", 0, 0xDF39},           // Pete Sampras Tennis 96 (Prototype ?)
  };
  if (rom.size() < 0x200)
    return false;
  const std::string product(reinterpret_cast<const char*>(rom.data()) + 0x182, 12);
  const uint16_t checksum = static_cast<uint16_t>((rom[0x18E] << 8) | rom[0x18F]);
  const uint32_t sp = BigEndian32(rom, 0);
  for (const Entry& entry : JCART)
  {
    if (product.find(entry.id) != std::string::npos && (entry.sp == 0 || entry.sp == sp) &&
        (entry.checksum == 0 || entry.checksum == checksum))
      return true;
  }
  return false;
}

std::string ExtensionOf(const std::string& path)
{
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  std::string extension = path.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}
}  // namespace

std::string Normalized(const std::string& name)
{
  // Letters and digits in lower case, without the tags ("(USA)", "[!]") and the words "the" and "and" ("Lost Vikings,
  // The" and "The Lost Vikings", "Dunk & Hoop" and "Dunk and Hoop" are the same game).
  std::string out, word;
  auto flush = [&]() {
    if (word != "the" && word != "and")
      out += word;
    word.clear();
  };
  int depth = 0;
  for (const char c : name)
  {
    if (c == '(' || c == '[')
    {
      flush();
      depth++;
    }
    else if (c == ')' || c == ']')
    {
      depth = std::max(0, depth - 1);
    }
    else if (depth == 0 && c != '\'')  // (an apostrophe joins: "Give 'n Go", "'94")
    {
      if (std::isalnum(static_cast<unsigned char>(c)))
        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      else
        flush();
    }
  }
  flush();
  return out;
}

bool NesCartCrc(const std::vector<uint8_t>& rom, uint32_t* crc)
{
  // An iNES file: "NES" 0x1A, the PRG ROM's size in 16 KiB, the CHR ROM's in 8 KiB (NES 2.0: their high bits in byte 9;
  // the exponent form, 0xF, isn't counted here), a 512-byte trainer when byte 6 says so. FCEUmm counts the PRG ROM then
  // the CHR ROM as the header gives their sizes (a short file's missing bytes as 0xFF), an iNES 1.0 PRG size 0 as 256.
  if (rom.size() < 16 || std::memcmp(rom.data(), "NES\x1a", 4) != 0)
    return false;
  const bool nes2 = (rom[7] & 0x0C) == 0x08;
  size_t prg_units = rom[4], chr_units = rom[5];
  if (nes2)
  {
    if ((rom[9] & 0x0F) == 0x0F || (rom[9] & 0xF0) == 0xF0)
      return false;
    prg_units |= static_cast<size_t>(rom[9] & 0x0F) << 8;
    chr_units |= static_cast<size_t>(rom[9] & 0xF0) << 4;
  }
  else if (prg_units == 0)
  {
    prg_units = 256;
  }
  const size_t start = 16 + ((rom[6] & 0x04) ? 512 : 0);
  const size_t length = prg_units * 16384 + chr_units * 8192;
  uLong sum = crc32(0L, Z_NULL, 0);
  const size_t have = rom.size() > start ? std::min(rom.size() - start, length) : 0;
  if (have > 0)
    sum = crc32(sum, rom.data() + start, static_cast<uInt>(have));
  // (Missing bytes: 0xFF, as the core leaves them. The PRG ROM's and the CHR ROM's apart only when both are short: the
  // core pads each one, which is the same bytes in a row here unless the PRG ROM itself is short.)
  if (have < length)
  {
    static const std::vector<uint8_t> PAD(8192, 0xFF);
    for (size_t left = length - have; left > 0;)
    {
      const size_t n = std::min(left, PAD.size());
      sum = crc32(sum, PAD.data(), static_cast<uInt>(n));
      left -= n;
    }
  }
  *crc = static_cast<uint32_t>(sum);
  return true;
}

Game ReadGame(SystemId system, const std::string& path, const std::vector<uint8_t>& rom)
{
  Game game;
  game.system = system;
  const size_t slash = path.find_last_of('/');
  game.name = slash == std::string::npos ? path : path.substr(slash + 1);
  game.extension = ExtensionOf(path);
  if (!game.extension.empty() && game.name.size() > game.extension.size())
    game.name.erase(game.name.size() - game.extension.size());
  if (system == SystemId::NES)
  {
    game.nes_crc_known = NesCartCrc(rom, &game.nes_crc);
    // NES 2.0's "default expansion device" (byte 15): 0x02 the Four Score, 0x03 the Famicom's 4 player adapter.
    if (game.nes_crc_known && (rom[7] & 0x0C) == 0x08)
      game.nes_expansion = rom[15] & 0x3F;
    // A Famicom Disk System disk: the Famicom's own (its adapter in the expansion port).
    if (rom.size() >= 16 &&
        (std::memcmp(rom.data(), "FDS\x1a", 4) == 0 || std::memcmp(rom.data() + 1, "*NINTENDO-HVC*", 14) == 0))
      game.extension = ".fds";
  }
  if (system == SystemId::MD && rom.size() >= 0x200)
  {
    // The header's I/O devices (0x190: 14 of its 16 bytes, as the core reads them) and copyright (0x110).
    const std::string io(reinterpret_cast<const char*>(rom.data()) + 0x190, 14);
    const std::string copyright(reinterpret_cast<const char*>(rom.data()) + 0x110, 16);
    game.md_team_player = io.find('4') != std::string::npos;
    game.md_six_button = io.find('6') != std::string::npos;
    game.md_ea = copyright.find("T-50") != std::string::npos || copyright.find("ELEC") != std::string::npos;
    game.md_jcart = MdJcart(rom);
  }
  return game;
}

bool HasAdapter(SystemId system)
{
  return system == SystemId::SNES || system == SystemId::NES || system == SystemId::MD || system == SystemId::PS1 ||
         system == SystemId::SATURN;
}

Decision Decide(const std::string& setting, const Game& game)
{
  const bool on = setting == "on", off = setting == "off";
  const std::string mode = on ? "On: " : "Auto: ";  // (On with a game the lists know: their adapter for it)
  const Decision none{Adapter::None, off ? "Off" : "Auto: not a game the lists know"};
  switch (game.system)
  {
  case SystemId::SNES:
    if (off)
      return none;
    if (on)
      return {Adapter::SnesMultitap, "On"};
    if (InList(std::begin(SNES_MULTITAP_GAMES), std::end(SNES_MULTITAP_GAMES), game.name))
      return {Adapter::SnesMultitap, "Auto: a Super Multitap game (its title)"};
    return none;
  case SystemId::NES:
  {
    if (off)
      return none;
    const NesGame* four =
        game.nes_crc_known ? FindNes(std::begin(FOUR_SCORE_GAMES), std::end(FOUR_SCORE_GAMES), game.nes_crc) : nullptr;
    const NesGame* famicom =
        game.nes_crc_known ? FindNes(std::begin(FAMICOM_4P_GAMES), std::end(FAMICOM_4P_GAMES), game.nes_crc) : nullptr;
    if (famicom)
      return {Adapter::Famicom4P, mode + "FCEUmm's Famicom 4 player list (" + famicom->title + ")"};
    if (four)
      return {Adapter::FourScore, mode + "FCEUmm's Four Score list (" + four->title + ")"};
    if (game.nes_expansion == 0x03)
      return {Adapter::Famicom4P, mode + "its NES 2.0 header (expansion device 3)"};
    if (game.nes_expansion == 0x02)
      return {Adapter::FourScore, mode + "its NES 2.0 header (expansion device 2)"};
    if (!on)
      return none;
    // On, a game the lists don't know: a Famicom game's adapter is the Famicom's, in its expansion port (a Japanese
    // cartridge, a disk); the others', the Four Score.
    if (game.extension == ".fds" || JapaneseName(game.name))
      return {Adapter::Famicom4P, "On: a Famicom game"};
    return {Adapter::FourScore, "On"};
  }
  case SystemId::MD:
  {
    if (game.md_jcart)
      return {Adapter::Cartridge, "the cartridge's own two ports (a J-Cart: Genesis Plus GX's list)"};
    if (off)
      return none;
    if (InList(std::begin(MD_4WAYPLAY_GAMES), std::end(MD_4WAYPLAY_GAMES), game.name))
      return {Adapter::FourWayPlay, mode + "an EA 4 Way Play game (its title)"};
    if (game.md_team_player && game.md_ea)
      return {Adapter::FourWayPlay, mode + "an EA game whose header says multitap"};
    if (game.md_team_player)
      return {Adapter::TeamPlayer, mode + "its header says Team Player"};
    if (InList(std::begin(MD_TEAM_PLAYER_GAMES), std::end(MD_TEAM_PLAYER_GAMES), game.name))
      return {Adapter::TeamPlayer, mode + "a Team Player game (its title)"};
    if (!on)
      return none;
    return game.md_ea ? Decision{Adapter::FourWayPlay, "On: an EA game"} : Decision{Adapter::TeamPlayer, "On"};
  }
  case SystemId::PS1:
    if (off)
      return none;
    if (on)
      return {Adapter::Ps1Multitap, "On"};
    if (Ps1MultitapDisc(game.disc_code))
      return {Adapter::Ps1Multitap, "Auto: a Multitap disc (" + game.disc_code + ": DuckStation's game database)"};
    return {Adapter::None, game.disc_code.empty() ? "Auto: the disc's code isn't known yet" :
                                                    "Auto: " + game.disc_code + " isn't a Multitap disc"};
  case SystemId::SATURN:
    if (on)
      return {Adapter::SaturnTap, "On"};
    return {Adapter::None, off ? "Off" : "Auto: no list of the 6 Player Adaptor's games (On plugs it in)"};
  default:
    return {Adapter::None, "no adapter for this console"};
  }
}

const char* Name(Adapter adapter)
{
  switch (adapter)
  {
  case Adapter::None:
    return "none";
  case Adapter::Cartridge:
    return "the J-Cart's ports";
  case Adapter::SnesMultitap:
    return "Super Multitap";
  case Adapter::FourScore:
    return "Four Score";
  case Adapter::Famicom4P:
    return "Famicom 4 player adapter";
  case Adapter::TeamPlayer:
    return "Team Player";
  case Adapter::FourWayPlay:
    return "EA 4 Way Play";
  case Adapter::Ps1Multitap:
    return "Multitap";
  case Adapter::SaturnTap:
    return "6 Player Adaptor";
  }
  return "?";
}

int MaxPlayers(Adapter adapter)
{
  switch (adapter)
  {
  case Adapter::SnesMultitap:
  case Adapter::TeamPlayer:  // (and port 2's pad)
  case Adapter::Ps1Multitap:
    return 5;
  case Adapter::Cartridge:
  case Adapter::FourScore:
  case Adapter::Famicom4P:
  case Adapter::FourWayPlay:
    return 4;
  case Adapter::SaturnTap:
    return 12;
  case Adapter::None:
    break;
  }
  return 2;
}

std::vector<std::pair<unsigned, unsigned>> PortDevices(const Game& game, Adapter adapter, Adapter before,
                                                       bool nes_expansion_set)
{
  std::vector<std::pair<unsigned, unsigned>> calls;
  switch (game.system)
  {
  case SystemId::SNES:
    // Port 2: the Multitap, players 2 to 5 on it (Snes9x's ports 1 to 4), or the pad.
    calls.emplace_back(1, adapter == Adapter::SnesMultitap ? SNES_MULTITAP : DEVICE_JOYPAD);
    break;
  case SystemId::NES:
  {
    // FCEUmm reads players 3 and 4 (its ports 2 and 3) when they are plugged in, and turns the Four Score on with them
    // - off when its expansion port (port 4) has the Famicom's adapter, which reads them instead. Its expansion port's
    // device stays from one game to the next: back to the game's own (its list's: none for most) before ports 2 and 3,
    // else the Four Score would stay off.
    if (adapter != Adapter::Famicom4P && (nes_expansion_set || before == Adapter::Famicom4P))
      calls.emplace_back(4, NES_FC_AUTO);
    const bool four = adapter == Adapter::FourScore || adapter == Adapter::Famicom4P;
    calls.emplace_back(2, four ? NES_GAMEPAD : DEVICE_NONE);
    calls.emplace_back(3, four ? NES_GAMEPAD : DEVICE_NONE);
    if (adapter == Adapter::Famicom4P)
      calls.emplace_back(4, NES_FC_4PLAYERS);
    break;
  }
  case SystemId::MD:
    // The Team Player in port 1 (its four pads players 1 to 4; port 2's pad player 5), EA's 4 Way Play in both ports
    // (players 1 to 4): pads of three or six buttons as the game's header says (the core's own choice for a pad).
    if (adapter == Adapter::TeamPlayer)
    {
      calls.emplace_back(0, game.md_six_button ? MD_TEAMPLAYER_6B : MD_TEAMPLAYER_3B);
      calls.emplace_back(1, DEVICE_JOYPAD);
    }
    else if (adapter == Adapter::FourWayPlay)
    {
      calls.emplace_back(0, game.md_six_button ? MD_WAYPLAY_6B : MD_WAYPLAY_3B);
      calls.emplace_back(1, game.md_six_button ? MD_WAYPLAY_6B : MD_WAYPLAY_3B);
    }
    else if (before == Adapter::TeamPlayer || before == Adapter::FourWayPlay)
    {
      calls.emplace_back(0, DEVICE_JOYPAD);
      calls.emplace_back(1, DEVICE_JOYPAD);
    }
    break;
  case SystemId::PS1:
    // The Multitap in port 2: SwanStation's ports 1 to 4 are its pads A to D (DualShocks, as ports 0 and 1).
    if (adapter == Adapter::Ps1Multitap || before == Adapter::Ps1Multitap)
    {
      for (unsigned port = 2; port <= 4; port++)
        calls.emplace_back(port, adapter == Adapter::Ps1Multitap ? PS1_DUALSHOCK : DEVICE_NONE);
    }
    break;
  default:
    break;  // (the Saturn's adaptors are options; its 12 ports are pads already)
  }
  return calls;
}

void AddVariables(SystemId system, Adapter adapter, std::map<std::string, std::string>* variables)
{
  if (system == SystemId::PS1)
  {
    const bool tap = adapter == Adapter::Ps1Multitap;
    (*variables)["swanstation_ControllerPorts_MultitapMode"] = tap ? "Port2Only" : "Disabled";
    // Players 2 to 5's DualShocks as player 1's (PS1 Controller: analog or not; the left stick as the D-Pad in digital
    // mode): with the Multitap only, so that two players play as before 00.23.
    for (int n = 2; tap && n <= 5; n++)
    {
      for (const char* option : {"ForceAnalog", "AnalogDPadInDigitalMode"})
      {
        const auto first = variables->find(std::string("swanstation_Controller1_") + option);
        if (first != variables->end())
          (*variables)["swanstation_Controller" + std::to_string(n) + "_" + option] = first->second;
      }
    }
  }
  else if (system == SystemId::SATURN)
  {
    // Both adaptors: YabaSanshiro counts its players right only with none or both (one alone keeps the count of the
    // game before it); players 1 to 6 are on port 1's.
    const char* value = adapter == Adapter::SaturnTap ? "enabled" : "disabled";
    (*variables)["yabasanshiro_multitap_port1"] = value;
    (*variables)["yabasanshiro_multitap_port2"] = value;
  }
}

Colour PlayerColour(int player)
{
  static const Colour COLOURS[] = {{0x00, 0x30, 0xFF}, {0xFF, 0x10, 0x10}, {0x10, 0xE0, 0x30}, {0xFF, 0x30, 0xB0},
                                   {0xFF, 0x80, 0x00}};
  return COLOURS[std::clamp(player, 0, static_cast<int>(std::size(COLOURS)) - 1)];
}
}  // namespace Multiplayer
