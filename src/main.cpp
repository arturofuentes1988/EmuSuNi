// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Start-up: the system modules, the log, the settings, the display, then the menu and the games
// in turn - all in this one process (a Super Nintendo is small enough that the menu and the
// emulator share it; Dolphin for PS4 restarts itself between them).

#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>

#include <imgui.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include "Controls.h"
#include "Display.h"
#include "Emulator.h"
#include "Library.h"
#include "Log.h"
#include "Menu.h"
#include "PS4Lang.h"
#include "Saves.h"
#include "Settings.h"
#include "Tour.h"

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
}  // namespace

int main()
{
  const std::string data_dir = App::DATA_DIR;
  MakeDirs(data_dir);
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
  LogF("EmuSuNi %s starting", App::VERSION);
  LoadSystemModules();
  sceUserServiceInitialize(nullptr);
  scePadInit();

  Settings::Load(data_dir);
  Controls::Load(data_dir);
  Saves::Init(data_dir);
  PS4Lang::Load(App::ASSETS_DIR, Settings::Get("language"));
  Tour::Load(data_dir);

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
  bool start_now = !Tour::Active() && menu.DefaultGame(&game) && !MenuButtonHeld();
  if (start_now)
    LogLine("default game: " + game.path);
  for (;;)
  {
    if (!start_now && !menu.Run(&game))
      continue;
    start_now = false;
    std::string error;
    const Emulator::Result result = Emulator::Run(display, game, regular, bold, &error);
    menu.GameEnded(result == Emulator::Result::Failed ? error : std::string());
  }
}
