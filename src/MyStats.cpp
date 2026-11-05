// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "MyStats.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>

#include "Log.h"

namespace MyStats
{
namespace
{
constexpr int64_t MOST_A_SESSION = 24 * 3600;  // (a PS4 left on in a game: a day at most)

struct State
{
  bool loaded = false;
  std::string path;
  std::map<std::string, Game> games;  // by the game's path
  std::string playing;                // the game running ("": none)
  std::chrono::steady_clock::time_point start;
} s;

std::string Field(const std::string& line, size_t* at)
{
  const size_t tab = line.find('\t', *at);
  const std::string out = line.substr(*at, tab == std::string::npos ? std::string::npos : tab - *at);
  *at = tab == std::string::npos ? line.size() : tab + 1;
  return out;
}

void Save()
{
  std::string out = "# EmuSuNi's My Stats (Settings -> My Stats; never sent): path, console, seconds, sessions, last "
                    "played, online matches\n";
  for (const auto& [path, g] : s.games)
  {
    out += g.path + "\t" + g.console + "\t" + std::to_string(g.seconds) + "\t" + std::to_string(g.sessions) + "\t" +
           std::to_string(g.last) + "\t" + std::to_string(g.online) + "\n";
  }
  if (!WriteTextFile(s.path, out))
    LogLine("mystats: " + s.path + " couldn't be written");
}
}  // namespace

void Load(const std::string& data_dir)
{
  if (s.loaded)
    return;
  s.loaded = true;
  s.path = data_dir + "/mystats.ini";
  const std::string text = ReadTextFile(s.path);
  size_t at = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    if (line.empty() || line[0] == '#')
      continue;
    size_t f = 0;
    Game g;
    g.path = Field(line, &f);
    g.console = Field(line, &f);
    g.seconds = std::strtoll(Field(line, &f).c_str(), nullptr, 10);
    g.sessions = std::atoi(Field(line, &f).c_str());
    g.last = std::strtoll(Field(line, &f).c_str(), nullptr, 10);
    g.online = std::atoi(Field(line, &f).c_str());
    if (!g.path.empty())
      s.games[g.path] = g;
  }
}

void GameStarted(const std::string& path, const std::string& console, bool online)
{
  Load(App::DATA_DIR);
  if (path.empty())
    return;
  Game& g = s.games[path];
  g.path = path;
  g.console = console;
  g.sessions++;
  g.last = static_cast<int64_t>(std::time(nullptr));
  if (online)
    g.online++;
  s.playing = path;
  s.start = std::chrono::steady_clock::now();
  Save();  // (the session counted even if the game never ends cleanly)
}

void GameEnded()
{
  if (s.playing.empty())
    return;
  const int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s.start)
                              .count();
  Game& g = s.games[s.playing];
  g.seconds += std::clamp<int64_t>(seconds, 0, MOST_A_SESSION);
  g.last = static_cast<int64_t>(std::time(nullptr));
  s.playing.clear();
  Save();
}

std::vector<Game> Games()
{
  Load(App::DATA_DIR);
  std::vector<Game> out;
  for (const auto& [path, g] : s.games)
    out.push_back(g);
  std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) {
    return a.seconds != b.seconds ? a.seconds > b.seconds : a.last > b.last;
  });
  return out;
}
}  // namespace MyStats
