// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Start-up: the system modules, the log, the settings, the display, then the menu and the games
// in turn - all in this one process (a Super Nintendo is small enough that the menu and the
// emulator share it; Dolphin for PS4 restarts itself between them).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <cpuid.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/sysctl.h>
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
#include "AppSession.h"
#include "Controls.h"
#include "Display.h"
#include "Emulator.h"
#include "Library.h"
#include "Log.h"
#include "LogReport.h"
#include "Menu.h"
#include "PS4Lang.h"
#include "Platform.h"
#include "Portable.h"
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
extern "C" int emusuni_describe_mount(const char* path, char* out, size_t size);  // (01.25) port/ps4_diskfree.c
// (01.27) libSceSystemService's status (the SDK's SceSystemServiceStatus: an int32 of events waiting, then the bools
// isSystemUiOverlaid and isInBackgroundExecution...; 136 bytes): the header declares it without its argument.
extern "C" int32_t emusuni_system_status(void* status) __asm__("sceSystemServiceGetStatus");

namespace
{
// A start that never shows a picture (the GPU set-up stuck) would leave the console's screen
// frozen on the start-up logo with the PS button dead: after 20 s without three frames shown the
// app closes itself instead. (01.11) Its line says the start's step then (g_start_step), and start-closed.txt keeps it
// for the next start: a safe one (SafeStartStep).
void StartWatchdog(const std::string& data_dir)
{
  const std::string closed = data_dir + "/start-closed.txt";
  std::thread([closed] {
    for (int i = 0; i < 200; i++)
    {
      if (g_display_frames.load(std::memory_order_relaxed) >= 3)
        return;
      sceKernelUsleep(100000);
    }
    const char* step = g_start_step.load(std::memory_order_relaxed);
    LogF("watchdog: no picture 20 s after the start - closing the app (the start's step: %s)", step);
    const int fd = open(closed.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
      const ssize_t wrote = write(fd, step, strlen(step));
      (void)wrote;
      fsync(fd);
      close(fd);
    }
    AppSession::Closing("the start watchdog: no picture 20 s after the start");  // (01.27)
    sceSystemServiceLoadExec("exit", nullptr);
    _exit(1);
  }).detach();
}

// (01.27) The console's start (Unix time; 0: not known), for the session before's line (src/AppSession.h).
int64_t BootTime()
{
  struct timeval boot = {};
  size_t size = sizeof(boot);
  if (sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) == 0 && boot.tv_sec > 0)
    return static_cast<int64_t>(boot.tv_sec);
  timespec up = {};
  if (clock_gettime(CLOCK_MONOTONIC, &up) == 0 && up.tv_sec > 0)
    return static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(up.tv_sec);
  return 0;
}

// (01.27, a PS5 player: "after leaving the app, it won't open again until the console restarts") The PS4's own screen
// over EmuSuNi and back (sceSystemServiceGetStatus every half second): "system:" lines, the log through to the disk
// while it is behind, the session's file (src/AppSession.h) - and the session's heartbeat every minute. 200 lines at
// most a session.
void WatchSystem()
{
  std::thread([] {
    bool overlaid = false, background = false, failed = false;
    int lines = 0;
    for (unsigned tick = 1;; tick++)
    {
      sceKernelUsleep(500000);
      if (tick % 120 == 0)
        AppSession::Seen();
      if (failed)
        continue;
      alignas(8) unsigned char status[256] = {};
      const int32_t result = emusuni_system_status(status);
      if (result != 0)
      {
        LogF("system: the PS4's status can't be read (%#x): no \"system:\" lines", static_cast<unsigned>(result));
        failed = true;
        continue;
      }
      const bool now_overlaid = status[4] != 0, now_background = status[5] != 0;
      if (now_overlaid == overlaid && now_background == background)
        continue;
      overlaid = now_overlaid;
      background = now_background;
      AppSession::Front(!overlaid && !background);
      if (lines >= 200)
        continue;
      lines++;
      LogF("system: %s (%s)",
           background ? "EmuSuNi runs behind the PS4's own screen (the home screen in front)" :
           overlaid   ? "the PS4's own screen over EmuSuNi (the PS button's menu, a notice)" :
                        "EmuSuNi in front again",
           AppSession::DoingNow().c_str());
      if (overlaid || background)
        LogSync();
    }
  }).detach();
}

// (01.27) The player's data on a USB drive by itself (src/Portable.h's Local): this session's log there too, its lines
// so far copied (the data folder can't be written, or doesn't keep it: Send Log sends it from there, and the next
// start finds this one's as emusuni-1.log). The log stays where it is when the drive doesn't take it.
void LogToFolder(const std::string& folder, std::string* log_dir)
{
  const std::string before = *log_dir + "/emusuni.log";
  RotateLogs(folder + "/emusuni", 4);
  const std::string text = ReadTextFile(before);
  const int fd = open((folder + "/emusuni.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
  {
    LogF("data: %s/emusuni.log can't be written (%s): this log stays %s", folder.c_str(), std::strerror(errno),
         before.c_str());
    return;
  }
  size_t done = 0;
  while (done < text.size())
  {
    const ssize_t wrote = write(fd, text.data() + done, text.size() - done);
    if (wrote <= 0)
      break;
    done += static_cast<size_t>(wrote);
  }
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  close(fd);
  std::remove(before.c_str());  // (copied: Send Log takes the data folder's first)
  LogF("data: this log is %s/emusuni.log from here on (its lines before copied from %s)", folder.c_str(),
       before.c_str());
  *log_dir = folder;
  LogReport::SetLogDir(folder);
}

// (01.11) Gabriel's PS4 (9 725 bezels on a USB drive): the menu's first frame came after the watchdog's 20 s, every
// start, and the app never opened - not even to send its log. The start after a start the watchdog closed: its step
// (start-closed.txt, read and deleted), and a safe start - the menu's big inventories left out (src/Menu.h: SafeStart).
std::string SafeStartStep(const std::string& data_dir)
{
  const std::string closed = data_dir + "/start-closed.txt";
  FILE* file = std::fopen(closed.c_str(), "rb");
  if (!file)
    return "";
  char text[128] = {};
  const size_t got = std::fread(text, 1, sizeof(text) - 1, file);
  std::fclose(file);
  std::remove(closed.c_str());
  const std::string step = got > 0 ? std::string(text, got) : std::string("unknown");
  LogLine("start: the last start showed no picture in 20 s (its step: " + step +
          "): a safe start, without the bezels' count, the star ratings' match or the covers' and bezels' downloads");
  return step;
}

// (01.25, the owner: "¿en este registro viene la versión de hackeo?") The start's "platform:" line (src/Platform.h): the
// console, its system software, its jailbreak's folder and the modules in this process - read without asking the
// jailbreak anything (a call a HEN lacks could close the app), with libkernel's own calls every PS4 game may make;
// nothing of the player's.
void LogPlatform()
{
  Platform::Facts f;
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (__get_cpuid(1, &a, &b, &c, &d))
    f.cpuid_eax1 = a;
  if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004)
  {
    char brand[49] = {};
    for (unsigned i = 0; i < 3; i++)
    {
      unsigned r[4] = {};
      __get_cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3]);
      std::memcpy(brand + i * 16, r, 16);
    }
    f.cpu_brand = brand;
    while (!f.cpu_brand.empty() && f.cpu_brand.back() == ' ')
      f.cpu_brand.pop_back();
    while (!f.cpu_brand.empty() && f.cpu_brand.front() == ' ')
      f.cpu_brand.erase(0, 1);
  }
  f.neo = sceKernelIsNeoMode() ? 1 : 0;
  OrbisKernelSwVersion version = {};
  const uint64_t version_size = sizeof(version);
  std::memcpy(version.padding, &version_size, sizeof(version_size));
  if (sceKernelGetSystemSwVersion(&version) == 0)
    f.sw_version = version.i_version;
  const int cpu_hz = sceKernelGetCpuFrequency();
  f.cpu_hz = cpu_hz > 0 ? static_cast<uint64_t>(cpu_hz) : 0;
  f.tsc_hz = sceKernelGetTscFrequency();
  char model[128] = {};
  size_t model_size = sizeof(model) - 1;
  if (sysctlbyname("hw.model", model, &model_size, nullptr, 0) == 0)
    f.hw_model = std::string(model, strnlen(model, sizeof(model) - 1));
  for (const char* folder : {"GoldHEN", "etaHEN", "mira", "HEN"})
  {
    struct stat st = {};
    if (stat((std::string("/data/") + folder).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      f.folders.push_back(folder);
  }
  OrbisKernelModule handles[256] = {};
  size_t count = 0;
  if (sceKernelGetModuleList(handles, 256, &count) == 0)
  {
    for (size_t i = 0; i < count && i < 256; i++)
    {
      OrbisKernelModuleInfo info = {};
      info.size = sizeof(info);
      if (sceKernelGetModuleInfo(handles[i], &info) == 0)
        f.modules.emplace_back(info.name, strnlen(info.name, sizeof(info.name)));
    }
  }
  LogLine("platform: " + Platform::Line(f));
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
  int log_fd = open((data_dir + "/emusuni.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  // (01.25, the owner: "más compatible con ps5" - players on a PS5 saw /data/EmuSuNi never made) The data folder can't
  // be written: the log on a USB drive's top folder instead (emusuni.log, the session before's emusuni-1.log), so it
  // says why and Send Log still has it (LogReport::SetLogDir).
  std::string log_dir = data_dir;
  const int log_error = log_fd < 0 ? errno : 0;
  if (log_fd < 0)
  {
    for (const char* drive : {"/usb0", "/usb1", "/mnt/usb0", "/mnt/usb1"})
    {
      RotateLogs(std::string(drive) + "/emusuni", 1);
      log_fd = open((std::string(drive) + "/emusuni.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (log_fd >= 0)
      {
        log_dir = drive;
        break;
      }
    }
  }
  if (log_fd >= 0)
  {
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  LogF("%s %s starting", App::NAME, App::VERSION);
  if (log_dir != data_dir)
  {
    LogF("log: %s/emusuni.log can't be written (%s): this log is %s/emusuni.log", data_dir.c_str(),
         std::strerror(log_error), log_dir.c_str());
    LogReport::SetLogDir(log_dir);
  }
  // ((01.25) The data folder as the PS4 - or the PS5 - gives it: Log.h's CheckDataDir, and the mounts of /data and of
  // the folder: port/ps4_diskfree.c.)
  DataDirCheck data_check;
  for (const std::string& line : CheckDataDir(data_dir, &data_check))
    LogLine("data: " + line);
  // ((01.27) What that says for the player's data (src/Portable.h's Local): kept - as always -, can't be written, or
  // nothing of a session before (no start counted, no log of one, no settings: a first start, or a folder emptied).)
  const Portable::Local data_local =
      !data_check.writable ? Portable::Local::Unwritable :
      data_check.starts_before == 0 && !FileExists(data_dir + "/emusuni-1.log") &&
              !FileExists(data_dir + "/xmb.ini") && !FileExists(data_dir + "/settings.ini") ?
                             Portable::Local::Fresh :
                             Portable::Local::Kept;
  LogF("data: %s", data_local == Portable::Local::Kept ? "the data folder keeps its files: the player's data as always" :
                   data_local == Portable::Local::Unwritable ?
                                                         "the data folder can't be written: a USB drive for the "
                                                         "player's data (src/Portable.h)" :
                                                         "nothing of a session before in the data folder (a first "
                                                         "start, or emptied since the last one): the USB drives asked");
  {
    char mount[512] = {}, own[512] = {};
    emusuni_describe_mount("/data", mount, sizeof(mount));
    emusuni_describe_mount(data_dir.c_str(), own, sizeof(own));
    LogF("data: /data's mount: %s", mount);
    if (std::strcmp(mount, own) != 0)
      LogF("data: %s's mount: %s", data_dir.c_str(), own);
  }
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
  LogPlatform();  // ((01.25) the console, its system and its jailbreak: src/Platform.h)
  // The start watchdog (src/StartGuard.h): what the process before left - a game's start that didn't finish, a game
  // to start again at once (it restarted EmuSuNi for it), a failure to show - and its thread.
  std::string again_path, again_message;
  StartGuard::AppStarted(data_dir, &again_path, &again_message);
  const std::string safe_step = SafeStartStep(data_dir);  // (01.11)
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

  // (00.87) Where the player's data lives this session (src/Portable.h): a USB drive's portable data (Settings -> Keep
  // My Data on the USB Drive), looked for only when this PS4 used one, else /data/EmuSuNi as always. The settings,
  // controls, saves, states, cheats, bezels, covers, themes, favorites and the game list are read from there; the logs,
  // the logins, the statistics and the caches stay in data_dir. A write to a drive taken out goes to this PS4 instead.
  // (01.27) A data folder that can't be written or doesn't keep its files (a PS5's?): the data on a USB drive by itself
  // when one says so (data_local), and this log with it.
  Portable::Start(data_dir, {}, 8000, data_local);
  if (Portable::Automatic())
    LogToFolder(Portable::DataFolder(), &log_dir);
  // ((01.92, Lei on a PS5 in portable mode: where do the BIOS go?) The BIOS folder made, empty, where the app reads them -
  // this PS4's, and the portable data's when it is in use (Portable::SystemDir) -: nothing is ever put in it.
  MakeDirs(data_dir + "/system");
  if (Portable::Active())
    MakeDirs(Portable::DataFolder() + "/system");
  // (01.27) How the session before ended, this one's file and the PS4's own screen over EmuSuNi (src/AppSession.h).
  AppSession::Begin(log_dir + "/emusuni-session.ini", BootTime());
  WatchSystem();
  SetWriteRedirect(Portable::Redirect);
  const std::string user_dir = Portable::UserDir();
  for (SystemId board : {SystemId::ARCADE, SystemId::NAOMI, SystemId::ATOMISWAVE})
  {
    if (HasSystem(board))
      Library::SetSystemFolder(board, Portable::SystemDir(GetSystem(board).key));  // (their missing BIOS's marks)
  }
  Settings::Load(user_dir);
  Controls::Load(user_dir);
  Saves::Init(user_dir);
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
  StartWatchdog(data_dir);
  // (01.39, the 3DS specialist's 3ds-0137 test: Super Mario 3D Land hangs the PS4's GPU and gpu-hang.txt can't tell
  // which draw) RADV's trace, off unless /data/EmuSuNi/gpu-trace is there. Mesa reads RADV_PS4_TRACE (any value) as
  // the device is made (radv_device.c, ac_ps4_drm.c): the GPU writes an id after each draw - gpu-hang.txt's execution
  // check then has one "done" among them, the draw it stopped on - and its flight recorder copies every submission
  // to /data/DolphinPS4/flight (a ring of 8 files, and progress.log). Mesa makes flight/ only: its folder first.
  const bool gpu_trace = FileExists(data_dir + "/gpu-trace");
  if (gpu_trace)
  {
    mkdir("/data/DolphinPS4", 0777);
    setenv("RADV_PS4_TRACE", "1", 1);
  }
  LogF("gpu: RADV_PS4_TRACE %s", gpu_trace ? "on (/data/EmuSuNi/gpu-trace): a trace id after each draw, the flight "
                                             "recorder in /data/DolphinPS4/flight" :
                                             "off");
  g_start_step = "the display's set-up";
  Display display;
  if (!display.Initialize())
  {
    LogLine("the display couldn't start");
    for (;;)
      sceKernelUsleep(1000000);
  }
  sceSystemServiceHideSplashScreen();
  LogMemory("display ready");

  g_start_step = "the menu's set-up";
  Menu menu(display, data_dir, App::ASSETS_DIR);
  if (!safe_step.empty())
    menu.SafeStart();
  ImFont* regular = menu.RegularFont();
  ImFont* bold = menu.BoldFont();

  Library::Game game;
  bool start_now = false;
  g_start_step = "the games' list (a game to start at once?)";
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
    AppSession::Doing("the menu");  // ((01.27) a change only)
    if (!start_now && !menu.Run(&game))
      continue;
    start_now = false;
    AppSession::Doing("a game: " + game.key);
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
    // ((01.57) The N64 Resolution changed while it ran: the same game again, from its state kept apart.)
    for (int again = 0; result == Emulator::Result::Restart && again < 8; again++)
    {
      AppSession::Doing("a game started again: " + game.key);
      error.clear();
      result = Emulator::Run(display, game, regular, bold, &error);
    }
    if (result == Emulator::Result::Restart)
      result = Emulator::Result::Menu;
    menu.GameEnded(result == Emulator::Result::Failed ? error : std::string());
  }
}
