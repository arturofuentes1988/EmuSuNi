// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (01.91) The ranked games and their detector (Elo.h). The memory's addresses: from RetroAchievements' public code
// notes of each game (their numbering: SNES $7E0000-$7FFFFF at 0, an Arcade board's RAM as FinalBurn Neo's
// SYSTEM_RAM shows it), the conditions our own - each checked on the PC (local/trabajo-elo/banco: a versus match
// played to its end, both ways).

#include "Elo.h"

#include <cctype>
#include <cstring>

#include "libretro.h"
#include "rc_runtime_types.h"

namespace Elo
{
namespace
{
// The order is the server's (NetProto::ELO_GAMES) and the Online tab's: the owner's ten, the most played first.
const Game GAMES[] = {
    // Killer Instinct (SNES, USA and its Rev 1): no rounds - two life bars each; 0x10ec the fight's end (9 P1 won, 8
    // P2 won - by knock-out or time; 0x11 / 0x10 a player thrown out of the stage), back to 0 at the next fight; 0x109b
    // 0 in a two players' game (0xff against the CPU); 0x0000 the screen (0x86ee a fight).
    {"snes-ki", "Killer Instinct", "KI", "snes", "Killer Instinct (", "killer instinct",
     "0x0000=h86ee_0xH109b=0",
     "0xH109b=0S0xH10ec=9S0xH10ec=h11",
     "0xH109b=0S0xH10ec=8S0xH10ec=h10", 60},
    // Mortal Kombat (Arcade, T-Unit rev 5.0 and rev 4.0): 0xa232 the game's state (2 a fight), each player's state
    // 0xa240 / 0xa29c (1 fighting: both humans - the winner's next fight, against the CPU, has the loser's at 2 or 0),
    // rounds won 0xa266 / 0xa2c2 (16 bits; two win the match).
    {"arc-mk", "Mortal Kombat", "MK", "arcade", " mk mkr4 ",
     "mortal kombat rev 5 0 t unit 03 19 93|mortal kombat rev 4 0 t unit 02 11 93",
     "0xHa232=2_0xHa240=1_0xHa29c=1",
     "0xHa240=1_0xHa29c=1_0xa266=2_0xa2c2<2",
     "0xHa240=1_0xHa29c=1_0xa2c2=2_0xa266<2", 60},
    // Ultimate Mortal Kombat 3 (SNES): each side's player 0x36c6 (P1) / 0x388a (P2) - 1 playing (2 its continue, 0
    // none: the winner's next fight, against the CPU, has the loser's at 2 or 0) -, rounds won 0x36e0 / 0x38a4 (two
    // win the match).
    {"snes-umk3", "Ultimate Mortal Kombat 3", "UMK3", "snes", "Ultimate Mortal Kombat 3 (", "ultimate mortal kombat 3",
     "0x36c6=1_0x388a=1_0x36e0<2_0x38a4<2",
     "0x36c6=1_0x388a=1_0x36e0=2_0x38a4<2",
     "0x36c6=1_0x388a=1_0x38a4=2_0x36e0<2", 60},
    // The King of Fighters '97 (Neo Geo): the BIOS's player states 0xfdb7 (P1) / 0xfdb6 (P2) 1 - both playing (a
    // challenger's match on the MVS, a versus on the AES) -, 0xa787 the fight's state (0x1a fighting), 0xa847 the mode
    // (bit 0: teams - three rounds lost lose the match -, else singles - two), rounds lost 0xa84b (P1) / 0xa85a (P2).
    {"arc-kof97", "The King of Fighters '97", "KOF '97", "arcade", " kof97 kof97h kof97k kof97pls kof97oro kof97pm kog ",
     "the king of fighters 97 ngm 2320|the king of fighters 97 ngh 2320|the king of fighters 97 korean release",
     "0xHa787=h1a_0xHfdb6=1_0xHfdb7=1",
     "0xHfdb6=1_0xHfdb7=1S0xMa847=1_0xHa85a=3_0xHa84b<3S0xMa847=0_0xHa85a=2_0xHa84b<2",
     "0xHfdb6=1_0xHfdb7=1S0xMa847=1_0xHa84b=3_0xHa85a<3S0xMa847=0_0xHa84b=2_0xHa85a<2", 60},
    // Mortal Kombat II (Arcade, rev L3.1 and the sets RetroAchievements counts as the same): as Mortal Kombat - the
    // game's state 0xc03a, the players' 0xc03e / 0xc1b8, round wins 0xc062 / 0xc1dc.
    {"arc-mk2", "Mortal Kombat II", "MK II", "arcade", " mk2 mk2r21 mk2r31e mk2r42 mk2r91 ", "mortal kombat ii rev l3 1",
     "0xHc03a=2_0xHc03e=1_0xHc1b8=1",
     "0xHc03e=1_0xHc1b8=1_0xc062=2_0xc1dc<2",
     "0xHc03e=1_0xHc1b8=1_0xc1dc=2_0xc062<2", 60},
    // The King of Fighters 2002 (Neo Geo; the Magic Plus II bootleg shares its memory): the BIOS's player states as in
    // '97, 0xa7e0 the mode (bit 0: teams), rounds lost 0xa7e4 (P1) / 0xa80a (P2).
    {"arc-kof2002", "The King of Fighters 2002", "KOF 2002", "arcade", " kof2002 kf2k2mp2 ",
     "the king of fighters 2002 ngm 2650 ngh 2650|the king of fighters 2002 magic plus ii bootleg",
     "0xHfdb6=1_0xHfdb7=1",
     "0xHfdb6=1_0xHfdb7=1S0xMa7e0=1_0xHa80a=3_0xHa7e4<3S0xMa7e0=0_0xHa80a=2_0xHa7e4<2",
     "0xHfdb6=1_0xHfdb7=1S0xMa7e0=1_0xHa7e4=3_0xHa80a<3S0xMa7e0=0_0xHa7e4=2_0xHa80a<2", 60},
    // Super Mario Kart (SNES): two players (0x2e 0) racing or fighting (0x36 2), a match race (0x2c 2) or a battle
    // (6); each player's wins (16 bits, the "W" the game shows after each: 0x1f30 P1, 0x1f32 P2 - back to 0 for a
    // new series).
    {"snes-smk", "Super Mario Kart", "Mario Kart", "snes", "Super Mario Kart (", "super mario kart",
     "0xH002e=0_0x0036=2S0xH002c=2S0xH002c=6",
     "0xH002e=0", "", 60, 0x1f30, 0x1f32, 2},
    // Street Fighter II' Champion Edition (CPS-1, World 920513): 0x89d6 the scene (0x12 a fight), 0x82e1 the players
    // in (bit 0 P1, bit 1 P2: 3 both - the next fight against the CPU starts with the loser's continue counting, but
    // never lasts two rounds), rounds won 0x864f (P1) / 0x894f (P2).
    {"arc-sf2ce", "Street Fighter II' Champion Edition", "SF II' CE", "arcade", " sf2ce ",
     "street fighter ii champion edition world 920513",
     "0xH89d6=h12_0xH82e1=3",
     "0xH82e1=3_0xH864f=2_0xH894f<2",
     "0xH82e1=3_0xH894f=2_0xH864f<2", 60},
    // The King of Fighters '98 (Neo Geo): the BIOS's player states as in '97, 0xa848 the mode (bit 0: teams), rounds
    // won 0xa85f (P1) / 0xa84c (P2).
    {"arc-kof98", "The King of Fighters '98", "KOF '98", "arcade", " kof98 ",
     "the king of fighters 98 the slugfest king of fighters 98 dream match never ends ngm 2420",
     "0xHfdb6=1_0xHfdb7=1",
     "0xHfdb6=1_0xHfdb7=1S0xMa848=1_0xHa85f=3_0xHa84c<3S0xMa848=0_0xHa85f=2_0xHa84c<2",
     "0xHfdb6=1_0xHfdb7=1S0xMa848=1_0xHa84c=3_0xHa85f<3S0xMa848=0_0xHa84c=2_0xHa85f<2", 60},
    // Mortal Kombat II (SNES, USA and its Rev 1): each side's player 0x2eee (P1) / 0x309c (P2) - 1 fighting, as the
    // arcade's (the winner's next fight, against the CPU, has the loser's at 2 or 0) -, rounds won 0x2f04 / 0x30b2.
    {"snes-mk2", "Mortal Kombat II (SNES)", "MK II SNES", "snes", "Mortal Kombat II (", "mortal kombat ii",
     "0xH2eee=1_0xH309c=1",
     "0xH2eee=1_0xH309c=1_0xH2f04=2_0xH30b2<2",
     "0xH2eee=1_0xH309c=1_0xH30b2=2_0xH2f04<2", 60},
};
constexpr int COUNT = static_cast<int>(sizeof(GAMES) / sizeof(GAMES[0]));

// A '|'-separated list's items.
std::vector<std::string> Items(const char* list, char sep)
{
  std::vector<std::string> out;
  std::string one;
  for (const char* p = list; ; p++)
  {
    if (*p == sep || *p == '\0')
    {
      if (!one.empty())
        out.push_back(one);
      one.clear();
      if (*p == '\0')
        break;
    }
    else
      one += *p;
  }
  return out;
}
}  // namespace

int GameCount()
{
  return COUNT;
}

const Game& GameAt(int index)
{
  return GAMES[index < 0 || index >= COUNT ? 0 : index];
}

int GameByKey(const std::string& key)
{
  for (int i = 0; i < COUNT; i++)
  {
    if (key == GAMES[i].key)
      return i;
  }
  return -1;
}

std::string Normalized(const std::string& title)
{
  std::string out;
  bool space = false;
  for (const unsigned char c : title)
  {
    if (std::isalnum(c))
    {
      if (space && !out.empty())
        out += ' ';
      space = false;
      out += static_cast<char>(std::tolower(c));
    }
    else if (c != '\'')  // ("Fighters '97": 97)
      space = true;
  }
  return out;
}

int Identify(const std::string& console, uint32_t, uint64_t, const std::string& title,
             const std::string& standard_name)
{
  // (A hack's memory may be another: never by its title.)
  const std::string norm = Normalized(title);
  const bool hack = norm.find("hack") != std::string::npos;
  for (int i = 0; i < COUNT; i++)
  {
    const Game& g = GAMES[i];
    if (console != g.console)
      continue;
    if (!standard_name.empty())
    {
      if (console == "arcade")
      {
        const size_t open = standard_name.rfind('[');
        const size_t close = standard_name.rfind(']');
        if (open != std::string::npos && close != std::string::npos && close > open &&
            std::strstr(g.versions, (" " + standard_name.substr(open + 1, close - open - 1) + " ").c_str()))
          return i;
      }
      else
      {
        for (const std::string& v : Items(g.versions, '|'))
        {
          if (standard_name.compare(0, v.size(), v) == 0)
            return i;
        }
      }
      continue;  // (a standard version of another game)
    }
    if (hack)
      continue;
    for (const std::string& t : Items(g.titles, '|'))
    {
      if (norm == t || (norm.compare(0, t.size(), t) == 0 && norm[t.size()] == ' '))
      {
        // ("mortal kombat ii" isn't "mortal kombat"'s: a title's next word a number or a numeral is another game.)
        const std::string next = norm.size() > t.size() ? norm.substr(t.size() + 1, norm.find(' ', t.size() + 1) -
                                                                                        t.size() - 1)
                                                        : std::string();
        const bool sequel = !next.empty() && (std::isdigit(static_cast<unsigned char>(next[0])) || next == "ii" ||
                                              next == "iii" || next == "gold" || next == "plus");
        if (!sequel)
          return i;
      }
    }
  }
  return -1;
}

// ---- The detector ----
bool Detector::Parse(const char* text, Trigger* t)
{
  t->trigger = nullptr;
  const int size = rc_trigger_size(text);
  if (size <= 0)
    return false;
  t->buffer.assign(static_cast<size_t>(size), 0);
  t->trigger = rc_parse_trigger(t->buffer.data(), text, nullptr, 0);
  return t->trigger != nullptr;
}

uint32_t Detector::Peek(uint32_t address, uint32_t bytes, void* self)
{
  const Detector* d = static_cast<const Detector*>(self);
  const uint8_t* ram = static_cast<const uint8_t*>(d->get_data_(RETRO_MEMORY_SYSTEM_RAM));
  const size_t size = d->get_size_(RETRO_MEMORY_SYSTEM_RAM);
  uint32_t v = 0;
  for (uint32_t i = 0; i < bytes && i < 4; i++)
  {
    if (ram && address + i < size)
      v |= static_cast<uint32_t>(ram[address + i]) << (8 * i);
  }
  return v;
}

bool Detector::Test(Trigger& t)
{
  return t.trigger && rc_test_trigger(t.trigger, Peek, this, nullptr) != 0;
}

bool Detector::Start(int game, void* (*get_data)(unsigned), size_t (*get_size)(unsigned))
{
  Stop();
  if (game < 0 || game >= COUNT || !get_data || !get_size)
    return false;
  const Game& g = GAMES[game];
  if (!Parse(g.fight, &fight_) || !Parse(g.p1, &p1_) || (g.counter_bytes == 0 && !Parse(g.p2, &p2_)))
    return false;
  game_ = game;
  get_data_ = get_data;
  get_size_ = get_size;
  return true;
}

void Detector::Stop()
{
  game_ = -1;
  state_ = State::Waiting;
  fight_frames_ = p1_frames_ = p2_frames_ = quiet_frames_ = 0;
  count1_ = Counter();
  count2_ = Counter();
  results_ = 0;
  frames_ = 0;
}

bool Detector::Rose(Counter& c, uint32_t address)
{
  const Game& g = GAMES[game_];
  const uint32_t v = Peek(address, static_cast<uint32_t>(g.counter_bytes), this);
  if (v != c.seen)
  {
    c.seen = v;
    c.seen_frames = 0;
  }
  if (++c.seen_frames != g.hold)
    return false;
  // (Held now: the count as it stands - one more than before is a win; any other change, a new series' 0, isn't.)
  const bool rose = c.known && v == c.base + 1;
  c.base = v;
  c.known = true;
  return rose;
}

int Detector::Frame()
{
  if (game_ < 0 || !get_data_(RETRO_MEMORY_SYSTEM_RAM))
    return 0;
  frames_++;
  const Game& g = GAMES[game_];
  const int hold = g.hold;
  const bool fight = Test(fight_);
  fight_frames_ = fight ? fight_frames_ + 1 : 0;
  int won = 0;
  if (g.counter_bytes > 0)
  {
    // (Both counts are followed every frame - in any state -, so a change seen while not armed is never taken later.)
    const bool rose1 = Rose(count1_, g.counter1), rose2 = Rose(count2_, g.counter2);
    if (rose1 != rose2 && Test(p1_))
      won = rose1 ? 1 : 2;
    quiet_frames_++;
  }
  else
  {
    const bool p1 = Test(p1_), p2 = Test(p2_);
    // (Both at once: neither - a draw, or a memory between two screens.)
    p1_frames_ = p1 && !p2 ? p1_frames_ + 1 : 0;
    p2_frames_ = p2 && !p1 ? p2_frames_ + 1 : 0;
    quiet_frames_ = !p1 && !p2 ? quiet_frames_ + 1 : 0;
    if (p1_frames_ >= hold || p2_frames_ >= hold)
      won = p1_frames_ >= hold ? 1 : 2;
  }
  switch (state_)
  {
  case State::Waiting:
    if (fight_frames_ >= ARM_FRAMES && quiet_frames_ >= ARM_FRAMES)
      state_ = State::Armed;
    return 0;
  case State::Armed:
    if (won)
    {
      state_ = State::Done;
      quiet_frames_ = 0;
      results_++;
    }
    return won;
  case State::Done:
    if (quiet_frames_ >= REARM_FRAMES)
      state_ = State::Waiting;
    return 0;
  }
  return 0;
}

std::string Detector::Line() const
{
  if (game_ < 0)
    return "off";
  const char* state = state_ == State::Waiting ? "waiting for a fight" : state_ == State::Armed ? "a fight seen"
                                                                                                : "result said";
  return std::string(GAMES[game_].key) + ": " + state + ", " + std::to_string(results_) + " result(s) in " +
         std::to_string(frames_) + " frames";
}
}  // namespace Elo
