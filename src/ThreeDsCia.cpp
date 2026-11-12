// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#include "ThreeDsCia.h"

#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>

#include "Log.h"
#include "Settings.h"

// ---- The core's side (scripts/build-3ds-core.sh: its emusuni_cia.h, interface version 1) ----
// Defined weakly here: a 3DS core whose object has them (strong) replaces these, one without them - or a build
// without the 3DS - keeps them, and the menu says the installer isn't in this version. (A weak definition, never a weak
// reference: nothing is left for the PS4's loader to resolve.) The core's install reports its progress every 64 KiB
// (the CIA's bytes done and its size), reads *cancel before each, puts back what was there when cancelled or failed,
// and checks the free space too; its information and what is installed may be read from any thread.
extern "C"
{
struct emusuni_3ds_cia_info
{
  uint64_t title_id;
  uint16_t version;
  uint8_t encrypted;
  uint8_t reserved[5];
  uint64_t file_bytes;
  uint64_t install_bytes;
  uint32_t contents;
  char name[128];
};
struct emusuni_3ds_installed
{
  uint16_t update_version;
  uint8_t has_update, reserved[5];
  uint64_t update_bytes;
  uint32_t dlc_titles, dlc_contents;
  uint64_t dlc_bytes;
};
typedef void (*emusuni_3ds_cia_progress)(uint64_t done, uint64_t total, void* user);
// (The core's emusuni_cia.h has the same, byte for byte: a change there is a new interface version.)
static_assert(sizeof(struct emusuni_3ds_cia_info) == 168, "the core's struct emusuni_3ds_cia_info");
static_assert(sizeof(struct emusuni_3ds_installed) == 32, "the core's struct emusuni_3ds_installed");

__attribute__((weak)) int emusuni_3ds_cia_info(const char*, struct emusuni_3ds_cia_info*)
{
  return ThreeDsCia::NO_CORE;
}
__attribute__((weak)) int emusuni_3ds_cia_install(const char*, const char*, emusuni_3ds_cia_progress, void*,
                                                  volatile int*)
{
  return ThreeDsCia::NO_CORE;
}
__attribute__((weak)) int emusuni_3ds_installed(const char*, uint64_t, struct emusuni_3ds_installed*)
{
  return ThreeDsCia::NO_CORE;
}
__attribute__((weak)) int emusuni_3ds_uninstall(const char*, uint64_t)
{
  return ThreeDsCia::NO_CORE;
}
// The installer's interface version in the core (1: these four as above); 0 here, without one.
__attribute__((weak)) int emusuni_3ds_cia_version(void)
{
  return 0;
}
}

namespace ThreeDsCia
{
namespace
{
std::string LowerText(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

uint64_t ReadLe64(const unsigned char* p)
{
  uint64_t value = 0;
  for (int i = 7; i >= 0; i--)
    value = (value << 8) | p[i];
  return value;
}
}  // namespace

Kind KindOf(uint64_t title_id)
{
  switch (static_cast<uint32_t>(title_id >> 32))
  {
  case 0x00040000:
    return Kind::Game;
  case 0x0004000E:
    return Kind::Update;
  case 0x0004008C:
    return Kind::Dlc;
  default:
    return Kind::Other;
  }
}

uint64_t GameOf(uint64_t title_id)
{
  const Kind kind = KindOf(title_id);
  if (kind == Kind::Update || kind == Kind::Dlc)
    return (uint64_t{0x00040000} << 32) | (title_id & 0xFFFFFFFFull);
  return title_id;
}

std::string Hex(uint64_t title_id)
{
  char text[24];
  std::snprintf(text, sizeof(text), "%016" PRIX64, title_id);
  return text;
}

std::string VersionText(uint16_t version)
{
  const unsigned major = version >> 10, minor = (version >> 4) & 0x3F, micro = version & 0xF;
  if (version == 0)
    return "v0";
  char text[24];
  if (micro != 0)
    std::snprintf(text, sizeof(text), "v%u.%u.%u", major, minor, micro);
  else
    std::snprintf(text, sizeof(text), "v%u.%u", major, minor);
  return text;
}

std::string SizeText(uint64_t bytes)
{
  char text[32];
  if (bytes >= (1ull << 30))
    std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
  else if (bytes >= (1ull << 20))
    std::snprintf(text, sizeof(text), "%" PRIu64 " MB", static_cast<uint64_t>((bytes + (uint64_t{1} << 19)) >> 20));
  else
    std::snprintf(text, sizeof(text), "%" PRIu64 " KB", static_cast<uint64_t>((bytes + 1023) >> 10));
  return text;
}

std::string CodeText(int code)
{
  switch (code)
  {
  case OK:
    return "Installed";
  case ENCRYPTED:
    return "This CIA is encrypted: only decrypted CIAs can be installed";
  case INVALID:
    return "This file isn't a CIA, or it is damaged";
  case IO:
    return "It couldn't be read or written (the drive, or no room left)";
  case CANCELLED:
    return "Cancelled: the game keeps what it had before";
  case CRASHED:
    return "The installer failed with this file: the game keeps what it had before";
  case NOT_INSTALLED:
    return "Only updates and DLC are installed (not games nor the system's titles)";
  case GAME_RUNNING:
    return "A 3DS game is running: close it first";
  case CORE_BUSY:
    return "The 3DS emulator is installing or removing something else";
  case NO_CORE:
    return "This version's 3DS emulator can't install CIAs";
  case NO_ROOM:
    return "Not enough free space";
  case BUSY:
    return "Another CIA is being installed";
  case NO_THREAD:
    return "The install couldn't start (no thread)";
  default:
    return "It failed (code " + std::to_string(code) + ")";
  }
}

bool Available()
{
  return emusuni_3ds_cia_version() == 1;
}

Info Read(const std::string& path)
{
  Info info;
  struct emusuni_3ds_cia_info raw;
  std::memset(&raw, 0, sizeof(raw));
  info.code = emusuni_3ds_cia_info(path.c_str(), &raw);
  info.read = info.code == OK;
  if (!info.read)
    return info;
  raw.name[sizeof(raw.name) - 1] = '\0';
  info.title_id = raw.title_id;
  info.version = raw.version;
  info.encrypted = raw.encrypted != 0;
  info.file_bytes = raw.file_bytes;
  info.install_bytes = raw.install_bytes;
  info.contents = raw.contents;
  info.name = raw.name;
  info.kind = KindOf(raw.title_id);
  return info;
}

std::vector<std::string> Folders(const std::vector<std::string>& game_folders, bool make)
{
  std::vector<std::string> out;
  for (const std::string& folder : game_folders)
  {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec))
      continue;
    std::string found;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
      const std::string name = it->path().filename().string();
      if (LowerText(name) == "cia" && it->is_directory(ec))
        found = it->path().string();
    }
    if (found.empty() && make)
    {
      const std::string made = folder + "/cia";
      std::error_code make_error;
      if (std::filesystem::create_directory(made, make_error))
      {
        LogLine("3ds cia: made " + made);
        found = made;
      }
    }
    if (!found.empty() && std::find(out.begin(), out.end(), found) == out.end())
      out.push_back(found);
  }
  return out;
}

std::vector<File> List(const std::vector<std::string>& folders)
{
  std::vector<File> out;
  for (const std::string& folder : folders)
  {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
      std::error_code file_error;
      if (!it->is_regular_file(file_error))
        continue;
      const std::string name = it->path().filename().string();
      if (name.size() < 5 || LowerText(name.substr(name.size() - 4)) != ".cia" || name[0] == '.')
        continue;  // (and macOS's "._x.cia" files)
      File file;
      file.path = it->path().string();
      file.file_name = name;
      file.size = it->file_size(file_error);
      out.push_back(std::move(file));
    }
  }
  std::sort(out.begin(), out.end(), [](const File& a, const File& b) {
    return LowerText(a.file_name) < LowerText(b.file_name);
  });
  for (File& file : out)
    file.info = Read(file.path);
  return out;
}

int64_t FreeBytes(const std::string& folder)
{
  struct statvfs fs;
  if (statvfs(folder.c_str(), &fs) != 0)
    return -1;
  return static_cast<int64_t>(fs.f_bavail) * static_cast<int64_t>(fs.f_frsize ? fs.f_frsize : fs.f_bsize);
}

// ---- The install's thread ----
namespace
{
std::mutex s_mutex;
Progress s_progress;  // (under s_mutex, but for its bytes: the atomics below)
std::atomic<uint64_t> s_bytes{0}, s_total{0};
volatile int s_cancel = 0;
std::string s_save_dir;
pthread_t s_thread;
bool s_thread_made = false;
std::chrono::steady_clock::time_point s_started;

void OnProgress(uint64_t done, uint64_t total, void*)
{
  s_bytes = done;
  s_total = total;
}

void* InstallMain(void*)
{
  std::string save_dir, path;
  {
    std::lock_guard lock(s_mutex);
    save_dir = s_save_dir;
    path = s_progress.path;
  }
  const int result = emusuni_3ds_cia_install(save_dir.c_str(), path.c_str(), OnProgress, nullptr, &s_cancel);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_started).count();
  LogF("3ds cia: %s: %s (%d), %.1f s", std::filesystem::path(path).filename().string().c_str(),
       result == OK ? "installed" : CodeText(result).c_str(), result, seconds);
  std::lock_guard lock(s_mutex);
  s_progress.busy = false;
  s_progress.done = true;
  s_progress.result = result;
  s_progress.seconds = seconds;
  return nullptr;
}
}  // namespace

bool Start(const std::string& save_dir, const std::string& path)
{
  std::lock_guard lock(s_mutex);
  if (s_progress.busy)
  {
    s_progress.result = BUSY;
    return false;
  }
  if (s_thread_made)
  {
    pthread_join(s_thread, nullptr);  // (finished: busy is false)
    s_thread_made = false;
  }
  s_progress = {};
  s_progress.path = path;
  if (!Available())
  {
    s_progress.done = true;
    s_progress.result = NO_CORE;
    return false;
  }
  s_save_dir = save_dir;
  s_bytes = 0;
  s_total = 0;
  s_cancel = 0;
  s_started = std::chrono::steady_clock::now();
  s_progress.busy = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 2u << 20);  // (the core's installer: Azahar's file and crypto code)
  const int made = pthread_create(&s_thread, &attr, InstallMain, nullptr);
  pthread_attr_destroy(&attr);
  if (made != 0)
  {
    s_progress.busy = false;
    s_progress.done = true;
    s_progress.result = NO_THREAD;
    LogF("3ds cia: no thread for the install (%d)", made);
    return false;
  }
  s_thread_made = true;
  LogLine("3ds cia: installing " + path);
  return true;
}

Progress Status()
{
  std::lock_guard lock(s_mutex);
  Progress progress = s_progress;
  progress.bytes = s_bytes;
  progress.total = s_total;
  if (progress.busy)
    progress.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_started).count();
  return progress;
}

void Cancel()
{
  s_cancel = 1;
}

void Forget()
{
  std::lock_guard lock(s_mutex);
  if (s_progress.busy)
    return;
  if (s_thread_made)
  {
    pthread_join(s_thread, nullptr);
    s_thread_made = false;
  }
  s_progress.done = false;
}

// ---- A game's updates, DLC and mods ----
// (As Emulator.cpp's ThreedsTitleId, which the game's start reads: the NCSD's first partition's program ID - its
// partition table at 0x120, in media units of 0x200 bytes -, or a .cxi's own. Both plain even in an encrypted dump.)
uint64_t RomTitleId(const std::string& path)
{
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return 0;
  unsigned char header[0x200];
  const auto read_at = [&](uint64_t offset) {
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
           std::fread(header, 1, sizeof(header), file) == sizeof(header);
  };
  const auto magic = [&](const char* word) { return std::memcmp(header + 0x100, word, 4) == 0; };
  uint64_t title = 0;
  if (read_at(0))
  {
    if (magic("NCSD"))
    {
      const uint64_t partition = static_cast<uint64_t>(header[0x120]) | static_cast<uint64_t>(header[0x121]) << 8 |
                                 static_cast<uint64_t>(header[0x122]) << 16 | static_cast<uint64_t>(header[0x123]) << 24;
      if (read_at(partition * 0x200) && magic("NCCH"))
        title = ReadLe64(header + 0x118);
    }
    else if (magic("NCCH"))
      title = ReadLe64(header + 0x118);
  }
  std::fclose(file);
  return title;
}

bool ModsOn(uint64_t game_title_id)
{
  const std::string off = Settings::Get("3ds_mods_off"), id = Hex(game_title_id);
  std::stringstream in(off);
  for (std::string item; std::getline(in, item, ',');)
  {
    if (item == id)
      return false;
  }
  return true;
}

void SetModsOn(uint64_t game_title_id, bool on)
{
  const std::string id = Hex(game_title_id);
  std::stringstream in(Settings::Get("3ds_mods_off"));
  std::string list;
  for (std::string item; std::getline(in, item, ',');)
  {
    if (!item.empty() && item != id)
      list += (list.empty() ? "" : ",") + item;
  }
  if (!on)
    list += (list.empty() ? "" : ",") + id;
  Settings::Set("3ds_mods_off", list);  // (saved by it)
  LogF("3ds cia: mods %s for %s", on ? "on" : "off", id.c_str());
}

Installed InstalledFor(const std::string& save_dir, uint64_t game_title_id)
{
  Installed installed;
  struct emusuni_3ds_installed raw;
  std::memset(&raw, 0, sizeof(raw));
  installed.code = emusuni_3ds_installed(save_dir.c_str(), game_title_id, &raw);
  installed.read = installed.code == OK;
  if (!installed.read)
    return installed;
  installed.has_update = raw.has_update != 0;
  installed.update_version = raw.update_version;
  installed.update_bytes = raw.update_bytes;
  installed.dlc_titles = raw.dlc_titles;
  installed.dlc_contents = raw.dlc_contents;
  installed.dlc_bytes = raw.dlc_bytes;
  return installed;
}

int Uninstall(const std::string& save_dir, uint64_t title_id)
{
  const int code = emusuni_3ds_uninstall(save_dir.c_str(), title_id);
  LogF("3ds cia: %s %s removed: %s (%d)", KindOf(title_id) == Kind::Dlc ? "the DLC" : "the update",
       Hex(title_id).c_str(), code == OK ? "yes" : CodeText(code).c_str(), code);
  return code;
}

std::string ModsFolder(const std::string& save_dir, uint64_t game_title_id)
{
  return save_dir + "/Azahar/load/mods/" + Hex(game_title_id);
}

bool HasMods(const std::string& save_dir, uint64_t game_title_id)
{
  std::error_code ec;
  const std::string folder = ModsFolder(save_dir, game_title_id);
  if (!std::filesystem::is_directory(folder, ec))
    return false;
  return std::filesystem::directory_iterator(folder, ec) != std::filesystem::directory_iterator();
}
}  // namespace ThreeDsCia
