// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#pragma once

// (01.91, the owner: "un ranking ELO de los 10 juegos más jugados en línea", the winner known "Automático por juego")
// The ranked games online and how each one's match ends, read from its memory as both consoles run the same frames:
// a small table (Elo.cpp: the console, the versions - No-Intro's names, FinalBurn Neo's sets -, and conditions in
// RetroAchievements' syntax, evaluated by rcheevos), and a detector that says once "port 1 won" or "port 2 won" per
// match. src/Netplay.cpp runs it in a ranked match and sends what it saw to the server (NetProto's EloResult), which
// counts it only when both players' consoles say the same.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct rc_trigger_t;

namespace Elo
{
// One ranked game.
struct Game
{
  const char* key;      // the server's (NetProto::ELO_GAMES): "snes-ki"
  const char* title;    // as the Online tab's ELO Ranking page shows it
  const char* label;    // its chip there (short)
  const char* console;  // "snes", "arcade"
  // Its versions: SNES - No-Intro names' beginnings ("Killer Instinct ("), '|' between; Arcade - FinalBurn Neo's sets,
  // ' ' around each (" mk mkr4 ").
  const char* versions;
  // The room's title when the content is no standard version (another dump, a set of another romset): its words
  // (Normalized), '|' between - the title as a whole or followed by more words ("killer instinct" fits "Killer
  // Instinct (USA) [!]"), never a hack's.
  const char* titles;
  // The conditions (rcheevos triggers, RetroAchievements' memory: SNES $7E0000 at 0, an Arcade board's RAM as
  // FinalBurn Neo shows it):
  //  fight - a match of two players under way: it arms the detector (held ARM_FRAMES);
  //  p1/p2 - that match over, port 1's player / port 2's won it (held `hold` frames).
  const char* fight;
  const char* p1;
  const char* p2;
  int hold;
  // Or the game's own count of each player's wins (counter_bytes > 0: little-endian, at counter1 - port 1's - and
  // counter2): a match won is that count one more, held `hold` frames, with p1 true then (a guard; p2 unused).
  uint32_t counter1 = 0, counter2 = 0;
  int counter_bytes = 0;
};

constexpr int ARM_FRAMES = 60;     // a fight seen this long (consecutive frames) before a result counts
constexpr int REARM_FRAMES = 120;  // after a result, neither result's condition true this long before the next fight

int GameCount();
const Game& GameAt(int index);
int GameByKey(const std::string& key);  // -1: none
// The ranked game a room plays (-1: none): its console, its content's CRC32 and size (NetplayLibrary::ContentId),
// its standard name (NetplayLibrary::StandardName: "Killer Instinct (USA)", "Mortal Kombat (rev 5.0 ...) [mk]"; ""
// none) and the room's title.
int Identify(const std::string& console, uint32_t crc, uint64_t size, const std::string& title,
             const std::string& standard_name);
// A title as Identify compares it: lower case, letters and digits, one space between words.
std::string Normalized(const std::string& title);

// The detector: Start with the game and its core's memory, Frame after each frame the match runs (the newest, after
// any rollback), its answer once per match: 1 or 2 (that port's player won), else 0.
class Detector
{
public:
  Detector() = default;
  Detector(const Detector&) = delete;
  Detector& operator=(const Detector&) = delete;
  bool Start(int game, void* (*get_data)(unsigned), size_t (*get_size)(unsigned));
  void Stop();
  bool On() const { return game_ >= 0; }
  int GameIndex() const { return game_; }
  int Frame();
  std::string Line() const;  // what it sees (the log's)
  int Results() const { return results_; }

private:
  struct Trigger
  {
    std::vector<uint8_t> buffer;
    rc_trigger_t* trigger = nullptr;
  };
  struct Counter
  {
    uint32_t base = 0, seen = 0;
    int seen_frames = 0;
    bool known = false;
  };
  static bool Parse(const char* text, Trigger* t);
  bool Test(Trigger& t);
  static uint32_t Peek(uint32_t address, uint32_t bytes, void* self);
  // The count's value held `hold` frames: true once when it is one more than the one held before it.
  bool Rose(Counter& c, uint32_t address);

  enum class State
  {
    Waiting,  // for a fight
    Armed,    // a fight seen: for its result
    Done,     // a result said: for both results false again
  };
  int game_ = -1;
  void* (*get_data_)(unsigned) = nullptr;
  size_t (*get_size_)(unsigned) = nullptr;
  Trigger fight_, p1_, p2_;
  Counter count1_, count2_;
  State state_ = State::Waiting;
  int fight_frames_ = 0, p1_frames_ = 0, p2_frames_ = 0, quiet_frames_ = 0;
  int results_ = 0;
  int64_t frames_ = 0;
};
}  // namespace Elo
