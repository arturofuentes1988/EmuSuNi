// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Start-up: the system modules, the log, the settings, the display, then the menu and the games
// in turn - all in this one process (a Super Nintendo is small enough that the menu and the
// emulator share it; Dolphin for PS4 restarts itself between them).

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <imgui.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include "Achievements.h"
#include "Controls.h"
#include "Display.h"
#include "Emulator.h"
#include "Library.h"
#include "Log.h"
#include "Menu.h"
#include "PS4Lang.h"
#include "Saves.h"
#include "Settings.h"
#include "StartGuard.h"
#include "Tour.h"
#ifdef EMUSUNI_SATURN
#include "SaturnTest.h"
#endif

#ifdef EMUSUNI_N64
extern "C" int ps4_exec_probe();  // port/ps4_runtime.cpp
#endif
#if defined(EMUSUNI_N64) || defined(EMUSUNI_PS1) || defined(EMUSUNI_NDS) || defined(EMUSUNI_PSP) || \
    defined(EMUSUNI_DC) || defined(EMUSUNI_3DS)
#define EMUSUNI_JIT_CORES 1  // port/ps4_n64.c is linked: it has the JIT arena
extern "C" void emusuni_jit_window_probe(void);  // port/ps4_n64.c
#endif
extern "C" int emusuni_enable_dmem_aliasing(char* text, size_t size);  // port/ps4_runtime.cpp

namespace
{
// A start that never shows a picture (the GPU set-up stuck) would leave the console's screen
// frozen on the start-up logo with the PS button dead: after 20 s without three frames shown the
// app closes itself instead.
void StartWatchdog()
{
  std::thread([] {
    for (int i = 0; i < 200; i++)
    {
      if (g_display_frames.load(std::memory_order_relaxed) >= 3)
        return;
      sceKernelUsleep(100000);
    }
    LogLine("watchdog: no picture 20 s after the start - closing the app");
    sceSystemServiceLoadExec("exit", nullptr);
    _exit(1);
  }).detach();
}

void LoadSystemModules()
{
  // Calling into a system library whose module isn't loaded kills the app with
  // PRX_NOT_RESOLVED_FUNCTION (love-ps4, DolphinPS4).
  const struct
  {
    const char* name;
    uint32_t id;
  } modules[] = {
      {"SystemService", ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE},
      {"UserService", ORBIS_SYSMODULE_INTERNAL_USER_SERVICE},
      {"VideoOut", ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT},
      {"AudioOut", ORBIS_SYSMODULE_INTERNAL_AUDIOOUT},
      {"Pad", ORBIS_SYSMODULE_INTERNAL_PAD},
  };
  for (const auto& module : modules)
  {
    const uint32_t result = sceSysmoduleLoadModuleInternal(module.id);
    if (result != 0)
      LogF("loading system module %s failed (%#x)", module.name, result);
  }
}

// The PS4's time zone for the C library (TZ), which is UTC without it: the menu's clock and dates,
// and the cartridges' clocks (Pokémon Ruby, Sapphire and Emerald read the time through it).
void SetTimeZone()
{
  int32_t minutes = 0, summer = 0;
  if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_TIME_ZONE, &minutes) != 0)
  {
    LogLine("time zone: unknown (UTC)");
    return;
  }
  if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_SUMMERTIME, &summer) != 0)
    summer = 0;
  const int offset = minutes + (summer ? 60 : 0);  // minutes ahead of UTC
  // POSIX's sign is the other way round: "UTC-3" is three hours ahead of UTC.
  char tz[32];
  snprintf(tz, sizeof(tz), "UTC%c%d:%02d", offset > 0 ? '-' : '+', std::abs(offset) / 60, std::abs(offset) % 60);
  setenv("TZ", tz, 1);
  tzset();
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  localtime_r(&now, &local);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
  LogF("time zone: %d minutes, summer time %d: TZ=%s, local time %s", minutes, summer, tz, text);
}

// L1 held while the app starts: the menu instead of the default game.
bool MenuButtonHeld()
{
  int32_t user = -1;
  if (sceUserServiceGetInitialUser(&user) != 0 || user < 0)
    return false;
  const int pad = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
  if (pad < 0)
    return false;
  bool held = false;
  for (int i = 0; i < 10 && !held; i++)
  {
    OrbisPadData data = {};
    if (scePadReadState(pad, &data) == 0 && data.connected)
      held = (data.buttons & ORBIS_PAD_BUTTON_L1) != 0;
    sceKernelUsleep(20000);
  }
  scePadClose(pad);
  return held;
}

// 00.73 had error reports the player could send; 00.74 took them out. What 00.73 kept of them - data_dir/reports (a
// report waiting to be offered or sent, the random ID it went with) and report-server.ini (a test server's address, if
// one was made) - is deleted at the first start of a version without them (the next starts find nothing).
// A folder and what it holds (its folders' too, a few levels), with the calls the menu already uses
// (directory_iterator, remove): the files and folders deleted, the ones that couldn't be.
void RemoveFolder(const std::filesystem::path& folder, int depth, int* removed, int* failed)
{
  std::error_code ec;
  std::vector<std::filesystem::path> files, folders;
  for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
       it.increment(ec))
  {
    std::error_code type_error;
    if (it->is_directory(type_error) && !it->is_symlink(type_error))
      folders.push_back(it->path());
    else
      files.push_back(it->path());
  }
  for (const auto& sub : folders)
  {
    if (depth > 0)
      RemoveFolder(sub, depth - 1, removed, failed);
    else
      (*failed)++;
  }
  files.push_back(folder);  // (last: empty by then)
  for (const auto& file : files)
  {
    if (std::filesystem::remove(file, ec))
      (*removed)++;
    else
      (*failed)++;
  }
}

void RemoveOldReports(const std::string& data_dir)
{
  std::error_code ec;
  const std::string folder = data_dir + "/reports";
  if (std::filesystem::is_directory(folder, ec))
  {
    int removed = 0, failed = 0;
    RemoveFolder(folder, 3, &removed, &failed);
    LogF("reports: 00.73's error reports are gone in this version: %s deleted (%d files and folders%s)", folder.c_str(),
         removed, failed > 0 ? (", " + std::to_string(failed) + " couldn't be").c_str() : "");
  }
  const std::string server = data_dir + "/report-server.ini";
  if (std::filesystem::exists(server, ec) && std::filesystem::remove(server, ec))
    LogF("reports: %s deleted", server.c_str());
}
}  // namespace

int main()
{
  const std::string data_dir = App::DATA_DIR;
  MakeDirs(data_dir);
  // The logs of the four sessions before (00.70): emusuni.log becomes emusuni-1.log, ... emusuni-4.log, so a start
  // that went wrong is still there after EmuSuNi was opened again (xmb-trace.log the same: port/ps4_crashlog.cpp).
  RotateLogs(data_dir + "/emusuni", 4);
  // stdout/stderr go nowhere on PS4: to a file, unbuffered so it survives a crash.
  const int log_fd = open((data_dir + "/emusuni.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (log_fd >= 0)
  {
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  LogF("%s %s starting", App::NAME, App::VERSION);
  // The stack a thread gets by default: what each thread alive at once takes of the regular flexible memory.
  LogThreadDefaults();
  // Direct memory aliasing for the whole process, before the first mapping of direct memory (the GPU's,
  // with the display): the PSP's VRAM mirrors and the Dreamcast's fast memory map the same memory twice,
  // which the kernel refuses until then (port/ps4_runtime.cpp; the cores check it again themselves).
  {
    char aliasing[200] = "";
    emusuni_enable_dmem_aliasing(aliasing, sizeof(aliasing));
    LogF("dmem: %s", aliasing);
  }
  LoadSystemModules();
  SetTimeZone();
  {
    // (00.80) The console's Enter button: the PS4's own keyboard chose its keys with it (Circle on some consoles)
    // until 00.79; the app's menus and keyboard always choose with Cross.
    int32_t enter = -1;
    sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN, &enter);
    LogF("enter button: %s", enter == 0 ? "Circle" : enter == 1 ? "Cross" : "unknown");
  }
  // The start watchdog (src/StartGuard.h): what the process before left - a game's start that didn't finish, a game
  // to start again at once (it restarted EmuSuNi for it), a failure to show - and its thread.
  std::string again_path, again_message;
  StartGuard::AppStarted(data_dir, &again_path, &again_message);
#ifdef EMUSUNI_N64
  // How the PS4 lets code made at run time run (the N64 core's recompilers), once at start: every
  // executable mapping after this is made that way (port/ps4_runtime.cpp's mmap wrapper).
  LogF("n64: executable memory: method %d", ps4_exec_probe());
#endif
#ifdef EMUSUNI_JIT_CORES
  // Could the JIT arena leave the program's data (00.64: 32 MiB more free at the start)? System flexible memory
  // just past the program, made read/write/execute and run: tried once, all given back at once ("memory probe:").
  emusuni_jit_window_probe();
#endif
  sceUserServiceInitialize(nullptr);
  scePadInit();

  Settings::Load(data_dir);
  Controls::Load(data_dir);
  Saves::Init(data_dir);
  PS4Lang::Load(App::ASSETS_DIR, Settings::Get("language"));
  Tour::Load(data_dir);
  // RetroAchievements (00.73): the account kept in retroachievements.ini logs in again with its token, in the
  // background (src/Achievements.h).
  Achievements::Init(data_dir, App::ASSETS_DIR);
  Achievements::SetLanguage(PS4Lang::Current().code);  // (00.76: the achievements' texts translated to it)
  Achievements::SetRomReader(Library::ReadRom);  // (00.77: a zipped cartridge's ROM for the library's marks)
  // 00.73's error reports were taken out in 00.74 (the owner's choice): what 00.73 kept of them goes, once.
  RemoveOldReports(data_dir);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.DisplaySize = ImVec2(static_cast<float>(Display::WIDTH), static_cast<float>(Display::HEIGHT));
  StartWatchdog();
  Display display;
  if (!display.Initialize())
  {
    LogLine("the display couldn't start");
    for (;;)
      sceKernelUsleep(1000000);
  }
  sceSystemServiceHideSplashScreen();
  LogMemory("display ready");

  Menu menu(display, data_dir, App::ASSETS_DIR);
  ImFont* regular = menu.RegularFont();
  ImFont* bold = menu.BoldFont();

  Library::Game game;
  bool start_now = false;
  if (!again_path.empty())
  {
    // The game the start watchdog restarted EmuSuNi for: started again at once (L1 held: the menu instead).
    start_now = menu.FindGame(again_path, &game) && !MenuButtonHeld();
    LogLine(start_now ? "start: the game starts again: " + game.path :
                        "start: not started again (not in the games now, or L1 held): " + again_path);
    if (!start_now)
      StartGuard::Forget();
  }
  else
  {
    start_now = !Tour::Active() && menu.DefaultGame(&game) && !MenuButtonHeld();
    if (start_now)
      LogLine("default game: " + game.path);
  }
  if (!again_message.empty())
    menu.GameEnded(again_message);  // (the menu shows it as it opens)
  for (;;)
  {
    if (!start_now && !menu.Run(&game))
      continue;
    start_now = false;
    std::string error;
    Emulator::Result result = Emulator::Run(display, game, regular, bold, &error);
    if (result == Emulator::Result::Retry)
    {
      // The start watchdog stopped a start that showed no picture: the game once more, at once (src/StartGuard.h; the
      // second time it ends with a message instead).
      error.clear();
      result = Emulator::Run(display, game, regular, bold, &error);
      if (result == Emulator::Result::Retry)
        result = Emulator::Result::Failed;  // (not reached: a second attempt fails instead)
    }
    menu.GameEnded(result == Emulator::Result::Failed ? error : std::string());
  }
}
