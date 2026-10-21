// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "SaturnTest.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#include <zlib.h>

#include "Cores.h"
#include "Display.h"
#include "Log.h"
#include "MemoryGuard.h"
#include "PS4Lang.h"
#include "SaturnBench.h"

// libkernel's affinity calls as they really are (OpenOrbis declares them without parameters): bit n is core n.
extern "C" int saturn_test_get_affinity(OrbisPthread thread, uint64_t* mask) __asm__("scePthreadGetaffinity");
extern "C" int saturn_test_set_affinity(OrbisPthread thread, uint64_t mask) __asm__("scePthreadSetaffinity");

namespace SaturnTest
{
namespace
{
constexpr const char* ENTRY_PATH = "saturn-test:";
constexpr uint32_t BIOS_SIZE = 512 * 1024;

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string FileName(const std::string& path)
{
  return std::filesystem::path(path).filename().string();
}

// The test's settings from the file's lines (key=value): frames (every game's; else a known game's own,
// SaturnBench::FramesFor), seconds, sleep_us. *frames_set: the file says the frames.
SaturnBench::Options ReadOptions(bool* frames_set)
{
  SaturnBench::Options options;
  *frames_set = false;
  const std::string text = ReadTextFile(std::string(App::DATA_DIR) + "/saturn-test");
  for (size_t start = 0; start < text.size();)
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    const size_t equal = line.find('=');
    if (equal == std::string::npos)
      continue;
    const std::string key = Lower(line.substr(0, equal));
    const long value = std::strtol(line.c_str() + equal + 1, nullptr, 10);
    if (key == "frames" && value >= 600 && value <= 60000)
    {
      options.frames = static_cast<int>(value);
      *frames_set = true;
    }
    else if (key == "seconds" && value >= 30 && value <= 3600)
      options.max_seconds = static_cast<double>(value);
    else if (key == "sleep_us" && value >= 0 && value <= 1000)
      options.scsp_sleep_us = static_cast<int>(value);
  }
  return options;
}

// Where the games may be: Roms/saturn on the USB drives (/usb0.. as the app sees them; FTP's /mnt/usb0..), then
// data_dir/Roms/saturn.
std::vector<std::string> GameFolders()
{
  std::vector<std::string> folders;
  for (int drive = 0; drive < 8; drive++)
    folders.push_back("/usb" + std::to_string(drive) + "/Roms/saturn");
  folders.push_back(std::string(App::DATA_DIR) + "/Roms/saturn");
  return folders;
}

bool IsGameFile(const std::string& name)
{
  // The core's files (retro_get_system_info: cue|iso|mds|ccd|chd); a .cue's tracks (.bin) come with it.
  const std::string extension = Lower(std::filesystem::path(name).extension().string());
  return extension == ".cue" || extension == ".chd" || extension == ".iso" || extension == ".ccd" ||
         extension == ".mds";
}

// The games of a folder and of its subfolders (one level: "Daytona USA (USA)/Daytona USA (USA).cue"), by name.
std::vector<std::string> FindGames(const std::string& folder)
{
  std::vector<std::string> games;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
  {
    std::error_code type_error;
    if (entry.is_directory(type_error))
    {
      std::error_code sub_error;
      for (const auto& sub : std::filesystem::directory_iterator(entry.path(), sub_error))
      {
        std::error_code sub_type;
        if (sub.is_regular_file(sub_type) && IsGameFile(sub.path().filename().string()))
          games.push_back(sub.path().string());
      }
    }
    else if (entry.is_regular_file(type_error) && IsGameFile(entry.path().filename().string()))
    {
      games.push_back(entry.path().string());
    }
  }
  std::sort(games.begin(), games.end(), [](const std::string& a, const std::string& b) {
    return Lower(FileName(a)) < Lower(FileName(b));
  });
  return games;
}

// The BIOS: a file of 512 KiB by a name the core knows (saturn_bios.bin and mpr-17933.bin: the USA and Europe BIOS
// v1.01a; sega_101.bin: Japan's v1.01), or any such .bin with "bios" in its name - the owner's "Sega Saturn BIOS
// (USA).bin" keeps its name -, in system/saturn, system or a games folder; a USA or Europe one first.
std::string FindBios(const std::vector<std::string>& folders, uint32_t* crc)
{
  std::string best;
  int best_score = 0;
  for (const std::string& folder : folders)
  {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
      std::error_code size_error;
      if (!entry.is_regular_file(size_error) || entry.file_size(size_error) != BIOS_SIZE)
        continue;
      const std::string name = Lower(entry.path().filename().string());
      if (name.size() < 4 || name.compare(name.size() - 4, 4, ".bin") != 0)
        continue;
      const bool known_west = name == "saturn_bios.bin" || name == "mpr-17933.bin";
      const bool bios = name.find("bios") != std::string::npos;
      const bool west = name.find("usa") != std::string::npos || name.find("eur") != std::string::npos ||
                        name.find("17933") != std::string::npos;
      const int score = known_west ? 4 : bios && west ? 3 : name == "sega_101.bin" ? 2 : bios ? 1 : 0;
      if (score > best_score)
      {
        best_score = score;
        best = entry.path().string();
      }
    }
  }
  *crc = 0;
  if (!best.empty())
  {
    if (FILE* f = std::fopen(best.c_str(), "rb"))
    {
      std::vector<unsigned char> data(BIOS_SIZE);
      const size_t got = std::fread(data.data(), 1, data.size(), f);
      std::fclose(f);
      *crc = static_cast<uint32_t>(::crc32(0L, data.data(), static_cast<uInt>(got)));
    }
  }
  return best;
}

std::string BiosName(uint32_t crc)
{
  switch (crc)
  {
  case 0x4AFCF0FAu:
    return "USA / Europe v1.01a (mpr-17933): the one the PC measured";
  case 0x224B752Cu:
    return "Japan v1.01 (sega_101)";
  case 0x2ABA43C2u:
    return "Japan v1.00";
  default:
    return "not one the PC knows";
  }
}

// One screen: a title, lines, a bar (progress 0-1; below 0: none).
void Draw(Display& display, ImFont* regular, ImFont* bold, const std::string& title, const std::vector<std::string>& lines,
          float progress)
{
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = 1.0f / 60.0f;
  display.BeginFrame();
  ImGui::NewFrame();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const float w = static_cast<float>(Display::WIDTH), h = static_cast<float>(Display::HEIGHT);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(14, 17, 28, 255));
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, 8), IM_COL32(60, 110, 230, 255));
  dl->AddText(bold, 54, ImVec2(150, 120), IM_COL32(255, 255, 255, 255), PS4Lang::T(title).c_str());
  float y = 230;
  for (const std::string& line : lines)
  {
    const bool strong = line.rfind("VERDICT", 0) == 0;
    dl->AddText(strong ? bold : regular, strong ? 36.0f : 32.0f, ImVec2(150, y),
                strong ? IM_COL32(255, 215, 110, 255) : IM_COL32(220, 226, 238, 255), PS4Lang::T(line).c_str());
    y += strong ? 58 : 50;
  }
  if (progress >= 0.0f)
  {
    const ImVec2 p0(150, y + 30), p1(w - 150, y + 62);
    dl->AddRectFilled(p0, p1, IM_COL32(40, 46, 66, 255), 8);
    dl->AddRectFilled(p0, ImVec2(p0.x + (p1.x - p0.x) * std::clamp(progress, 0.0f, 1.0f), p1.y),
                      IM_COL32(60, 110, 230, 255), 8);
  }
  ImGui::Render();
  display.EndFrame();
}

// Player 1's pad: the buttons pressed since the last call.
struct Pad
{
  int handle = -1;
  uint32_t held = 0;
  void Open()
  {
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) == 0 && user >= 0)
      handle = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
  }
  void Close()
  {
    if (handle >= 0)
      scePadClose(handle);
    handle = -1;
  }
  uint32_t Pressed()
  {
    OrbisPadData data = {};
    if (handle < 0 || scePadReadState(handle, &data) != 0 || !data.connected)
      return 0;
    const uint32_t pressed = data.buttons & ~held;
    held = data.buttons;
    return pressed;
  }
};

// A message until Cross or Circle (at most ten minutes).
void Message(Display& display, ImFont* regular, ImFont* bold, Pad& pad, const std::string& title,
             const std::vector<std::string>& lines)
{
  const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  pad.Pressed();  // (what is held now isn't a press)
  while (std::chrono::steady_clock::now() < until)
  {
    Draw(display, regular, bold, title, lines, -1.0f);
    if (pad.Pressed() & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE))
      break;
  }
}
}  // namespace

bool Enabled()
{
  return FileExists(std::string(App::DATA_DIR) + "/saturn-test");
}

Library::Game Entry()
{
  Library::Game game;
  game.path = ENTRY_PATH;
  game.id = "saturn-test";
  game.title = "Saturn (speed test)";
  game.key = "saturn-test";
  return game;
}

bool IsEntry(const Library::Game& game)
{
  return game.path == ENTRY_PATH;
}

void Run(Display& display, ImFont* regular, ImFont* bold)
{
  const std::string data_dir = App::DATA_DIR;
  const std::string title = "Saturn speed test";
  bool frames_set = false;
  SaturnBench::Options options = ReadOptions(&frames_set);
  options.system_dir = data_dir + "/system/saturn";
  options.save_dir = data_dir + "/saves/saturn-test";
  MakeDirs(options.system_dir);
  MakeDirs(options.save_dir);
  LogF("saturn test: EmuSuNi %s, YabaSanshiro 3.4.2 without a renderer; %s, %.0f s at most a game, SCSP sleep %d us",
       App::VERSION,
       frames_set ? ("frames=" + std::to_string(options.frames) + " (the file saturn-test)").c_str() :
                    "each known game to the end of its demo race (Daytona USA 4200 frames, Sonic R 6600), others 4800",
       options.max_seconds, options.scsp_sleep_us);
  Pad pad;
  pad.Open();

  // The games and the BIOS.
  std::vector<std::string> games;
  std::vector<std::string> bios_folders = {options.system_dir, data_dir + "/system"};
  for (const std::string& folder : GameFolders())
  {
    std::vector<std::string> found = FindGames(folder);
    if (!found.empty())
    {
      LogF("saturn test: %zu game(s) in %s", found.size(), folder.c_str());
      bios_folders.push_back(folder);
    }
    games.insert(games.end(), found.begin(), found.end());
  }
  if (games.size() > 6)
    games.resize(6);
  uint32_t crc = 0;
  options.bios = FindBios(bios_folders, &crc);
  if (!options.bios.empty())
    LogF("saturn test: BIOS %s (CRC32 %08x: %s)", options.bios.c_str(), static_cast<unsigned>(crc), BiosName(crc).c_str());
  if (games.empty() || options.bios.empty())
  {
    LogF("saturn test: %s", games.empty() ? "no games in Roms/saturn" : "no BIOS (a .bin of 512 KiB)");
    Message(display, regular, bold, pad, title,
            {games.empty() ? "No Saturn games found." : "No Saturn BIOS found.",
             "",
             "Games: .cue (with its .bin tracks) or .chd, in Roms/saturn",
             "on the USB drive (FTP: /mnt/usb0/Roms/saturn).",
             "BIOS: the USA one, 512 KB (any name with \"bios\" in it),",
             "in the same folder or in /data/EmuSuNi/system/saturn.",
             "",
             "Cross: back to the menu"});
    pad.Close();
    return;
  }

  // The memory check, as before a game (src/MemoryGuard.h): its one thread (SCSP).
  {
    const int cold = MemoryGuard::ColdMiB("saturn");
    const int extra = MemoryGuard::ExtraFromText(ReadTextFile(data_dir + "/memory-guard"));
    const MemoryGuard::Decision decision = MemoryGuard::Decide(FlexibleFreeMiB(), FlexibleMostMiB(), cold, 0, extra);
    LogF("memory (before the Saturn test): flexible %d MiB free, %d at most so far (%d MiB of thread stacks to reuse); "
         "YabaSanshiro needs %d (its threads %d): %s",
         decision.free_mib, decision.most_mib, decision.reusable_mib, decision.need_mib, cold,
         decision.start ? "starting" : "not started");
    if (!decision.start)
    {
      std::vector<std::string> lines;
      const std::string text = MemoryGuard::Message();
      for (size_t start = 0; start <= text.size();)
      {
        const size_t end = std::min(text.find('\n', start), text.size());
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
      }
      Message(display, regular, bold, pad, title, lines);
      pad.Close();
      return;
    }
    MemorySessionStart();
  }

  // The emulation thread (this one) as a game's: on core 2 at priority 480, the SCSP thread beside it on core 3
  // (src/SaturnBench.cpp).
  int old_priority = 0;
  const bool priority_set =
      scePthreadGetprio(scePthreadSelf(), &old_priority) == 0 && scePthreadSetprio(scePthreadSelf(), 480) == 0;
  uint64_t old_mask = 0;
  const int got = saturn_test_get_affinity(scePthreadSelf(), &old_mask);
  const int set = got == 0 ? saturn_test_set_affinity(scePthreadSelf(), 1ull << 2) : -1;
  LogF("saturn test: emulation thread priority %d -> %s, on core 2: %s (cores before %#llx); running on CPU %d",
       old_priority, priority_set ? "480" : "unchanged", got == 0 && set == 0 ? "yes" : "no",
       static_cast<unsigned long long>(old_mask), sceKernelGetCurrentCpu());
  std::string clock_report;
  SaturnBench::CheckCpuClock(&clock_report);
  LogLine("saturn test: " + clock_report);

  std::vector<SaturnBench::Result> results;
  bool stopped = false;
  for (size_t i = 0; i < games.size() && !stopped; i++)
  {
    const std::string name = std::filesystem::path(games[i]).stem().string();
    const std::string game_count = std::to_string(i + 1), games_count = std::to_string(games.size());
    SaturnBench::Options game = options;
    if (!frames_set)
      game.frames = SaturnBench::FramesFor(name, options.frames);
    Draw(display, regular, bold, title, {"Starting " + name + "...", "", "Circle: stop"}, 0.0f);
    results.push_back(SaturnBench::Run(Cores::YABASANSHIRO, games[i], game,
                                       [&](const SaturnBench::Result& result, int frame) {
      // The last second of the console's time (60 frames), as it is now.
      const int last = static_cast<int>(result.cpu_ms.size());
      const SaturnBench::Stats now = SaturnBench::Measure(result, last - 60, last);
      char ms[32], budget[32];
      std::snprintf(ms, sizeof(ms), "%.1f", now.cpu);
      std::snprintf(budget, sizeof(budget), "%.1f", 1000.0 / result.fps);
      Draw(display, regular, bold, title,
           {"Measuring " + name,
            "Game " + game_count + " of " + games_count + ", frame " + std::to_string(frame) + " of " +
                std::to_string(game.frames),
            "The emulation now: " + std::string(ms) + " ms a frame (full speed: " + std::string(budget) + " ms)",
            "",
            "No picture and no sound: the PS4's CPU is being measured.",
            "Circle: stop"},
           static_cast<float>(frame) / static_cast<float>(game.frames));
      if (pad.Pressed() & ORBIS_PAD_BUTTON_CIRCLE)
      {
        stopped = true;
        return false;
      }
      return true;
    }));
  }
  const std::vector<std::string> verdict = SaturnBench::Verdict(results, options);
  LogMemorySession();
  if (priority_set)
    scePthreadSetprio(scePthreadSelf(), old_priority);
  if (got == 0 && set == 0)
    saturn_test_set_affinity(scePthreadSelf(), old_mask);
  std::vector<std::string> lines = verdict;
  lines.push_back("");
  lines.push_back("The numbers: /data/EmuSuNi/emusuni.log (lines \"saturn test\").");
  lines.push_back("Cross: back to the menu");
  Message(display, regular, bold, pad, "Saturn speed test: results", lines);
  pad.Close();
}
}  // namespace SaturnTest
