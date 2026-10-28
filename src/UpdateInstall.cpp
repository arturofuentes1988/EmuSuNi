// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The PS4's background installer (BGFT) as the homebrew stores use it for a local package (LightningMods' Itemzflow
// installpkg.cpp and Store-R2, UnityOrbisBridge, flatz's write-up "PKG installation from internal HDD"): App Inst
// Util and BGFT loaded and initialised (a 1 MiB heap of its own, kept), then RegisterTaskByStorageEx with the package's
// path as SceShellCore sees it (entitlement 5, no content id: read from the package, playgo scenario "0", visible in
// Notifications > Downloads), then StartTask. BGFT copies the package and leaves the file.
// When the PS4 says the same application is installed already (0x80990088) or its download is queued (0x80990086) -
// EmuSuNi's own task from its last installation - that old task is released (found by the content id, stopped,
// unregistered) and the new one registered at once, as pkg-mutant-shop measured it working on a PS4 (13.52): the
// newer version then installs over the old one; uninstalling (what the stores do for other apps) is never done here.
// Not tried on the owner's PS4 yet: every call's answer goes to the log.

#include "UpdateInstall.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/Sysmodule.h>

#include "Log.h"

namespace Update::Install
{
namespace
{
// A task's progress as the firmware writes it: its sizes 64-bit (OpenOrbis' OrbisBgftTaskProgress has them 32-bit,
// which every installer with a working progress bar avoids). Read into a bigger buffer, both ways (Sizes).
struct Progress64
{
  uint32_t bits;
  int32_t error;
  uint64_t length, transferred, length_total, transferred_total;
  uint32_t index, count, rest_seconds, rest_seconds_total;
  int32_t preparing_percent, local_copy_percent;
};
static_assert(sizeof(Progress64) == 0x40, "BGFT's progress");
static_assert(sizeof(OrbisBgftDownloadParam) == 0x68 && sizeof(OrbisBgftDownloadParamEx) == 0x70 &&
                  offsetof(OrbisBgftDownloadParamEx, slot) == 0x68,
              "BGFT's download parameters");

constexpr int ERROR_ALREADY_INITIALISED = static_cast<int>(0x80990001);
constexpr int ERROR_ALREADY_DOWNLOADING = static_cast<int>(0x80990086);
constexpr int ERROR_ALREADY_INSTALLED = static_cast<int>(0x80990088);

std::mutex s_mutex;
bool s_ready = false;
OrbisBgftInitParams s_bgft = {};
int s_task = -1;
bool s_raw_logged = false;

void Log(const std::string& text)
{
  LogLine("update: installer: " + text);
}

std::string Code(int code)
{
  char text[16];
  snprintf(text, sizeof(text), "%#x", static_cast<unsigned>(code));
  return text;
}

// What a BGFT or App Inst Util code means, as far as the stores and write-ups tell.
std::string Meaning(int code)
{
  switch (static_cast<uint32_t>(code))
  {
  case 0: return "ok";
  case 0x80990001: return "BGFT initialised already";
  case 0x80990004: return "the task ended in an error";
  case 0x80990015: return "already installed or installing";
  case 0x80990019: return "no task for the title";
  case 0x8099002C: return "out of BGFT's memory";
  case 0x80990033: return "not a .pkg name";
  case 0x80990039: return "not enough storage space";
  case 0x80990085: return "not enough free space in one piece";
  case 0x80990086: return "a download of this content is in Notifications > Downloads already";
  case 0x80990088: return "the same application is installed already";
  case 0x80A30026: return "not enough space for the application";
  case 0x80A3000C: return "the application is running";
  case 0x80A31000: return "App Inst Util not initialised";
  default: return "unknown";
  }
}

std::string Said(int code)
{
  return Code(code) + " (" + Meaning(code) + ")";
}

// The package's path as SceShellCore sees it: the app's /usb0/... is /mnt/usb0/..., its /data/... is /user/data/....
std::string SystemPath(const std::string& path)
{
  if (path.compare(0, 4, "/usb") == 0)
    return "/mnt" + path;
  if (path.compare(0, 6, "/data/") == 0)
    return "/user" + path;
  return path;
}

// The package's content id (its header: 36 characters at 0x40), "" when it can't be read.
std::string ContentId(const std::string& path)
{
  unsigned char header[0x64] = {};
  FILE* file = std::fopen(path.c_str(), "rb");
  const bool read = file && std::fread(header, 1, sizeof(header), file) == sizeof(header);
  if (file)
    std::fclose(file);
  if (!read || std::memcmp(header, "\x7F" "CNT", 4) != 0)
    return {};
  std::string id(reinterpret_cast<const char*>(header + 0x40), 0x24);
  for (const char c : id)
  {
    if (c < 0x20 || c > 0x7E)
      return {};
  }
  return id;
}

// Modules and initialisation, once.
bool Ready(std::string* error)
{
  if (s_ready)
    return true;
  const uint32_t util = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL);
  const uint32_t bgft = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT);
  Log("modules: App Inst Util " + Code(static_cast<int>(util)) + ", BGFT " + Code(static_cast<int>(bgft)));
  if (util != 0 || bgft != 0)
  {
    *error = "the PS4's installer modules couldn't be loaded (App Inst Util " + Code(static_cast<int>(util)) +
             ", BGFT " + Code(static_cast<int>(bgft)) + ")";
    return false;
  }
  const int initialised = sceAppInstUtilInitialize();
  Log("sceAppInstUtilInitialize: " + Said(initialised));
  if (!s_bgft.heap)
  {
    s_bgft.heapSize = 1024 * 1024;
    s_bgft.heap = std::calloc(1, s_bgft.heapSize);  // (kept: BGFT uses it until the process ends)
    if (!s_bgft.heap)
    {
      *error = "no memory for BGFT's heap";
      return false;
    }
  }
  const int started = sceBgftServiceIntInit(&s_bgft);
  Log("sceBgftServiceIntInit: " + Said(started));
  if (started != 0 && started != ERROR_ALREADY_INITIALISED)
  {
    *error = "the PS4's installer (BGFT) couldn't start: " + Said(started);
    return false;
  }
  s_ready = true;
  return true;
}

// A task's progress: false when it can't be read. Its sizes are taken the way that matches the package's size (64-bit
// fields, else 32-bit: OpenOrbis' layout); the first reading's bytes go to the log once.
bool Read(int task, int64_t size, int64_t* done, int64_t* total, int* task_error)
{
  alignas(8) unsigned char raw[128] = {};
  const int result = sceBgftServiceDownloadGetProgress(task, reinterpret_cast<OrbisBgftTaskProgress*>(raw));
  if (result != 0)
  {
    Log("sceBgftServiceDownloadGetProgress(" + std::to_string(task) + "): " + Said(result));
    return false;
  }
  Progress64 wide;
  std::memcpy(&wide, raw, sizeof(wide));
  uint32_t narrow[4];
  std::memcpy(narrow, raw + 8, sizeof(narrow));  // (32-bit: length, transferred, length total, transferred total)
  if (!s_raw_logged)
  {
    s_raw_logged = true;
    std::string hex;
    for (int i = 0; i < 64; i++)
    {
      char byte[4];
      snprintf(byte, sizeof(byte), "%02x%s", raw[i], i % 8 == 7 ? " " : "");
      hex += byte;
    }
    Log("first progress, as the PS4 wrote it: " + hex);
  }
  *task_error = wide.error;
  const auto near = [size](uint64_t length) {
    return size > 0 && length > 0 && static_cast<int64_t>(length) <= size + (16 << 20) &&
           static_cast<int64_t>(length) + (16 << 20) >= size;
  };
  if (near(wide.length) || (!near(narrow[0]) && wide.length > 0 && wide.transferred <= wide.length &&
                            wide.length < (uint64_t{64} << 30)))
  {
    *done = static_cast<int64_t>(wide.transferred);
    *total = static_cast<int64_t>(wide.length);
  }
  else if (narrow[0] > 0 && narrow[1] <= narrow[0])
  {
    *done = narrow[1];
    *total = narrow[0];
  }
  else
  {
    *done = 0;
    *total = 0;  // (not started, or a layout not understood)
  }
  return true;
}
}  // namespace

bool Start(const std::string& path, const std::string& version, const std::string& data_dir, std::string* error)
{
  std::lock_guard lock(s_mutex);
  if (!Ready(error))
    return false;
  const std::string content = ContentId(path);
  if (content.empty())
  {
    *error = path + " isn't a PS4 package (no content id)";
    return false;
  }
  const std::string system = SystemPath(path);
  const std::string name = "EmuSuNi " + version;
  OrbisBgftDownloadParamEx params;
  std::memset(&params, 0, sizeof(params));
  params.params.entitlementType = 5;
  params.params.id = "";  // (read from the package)
  params.params.contentUrl = system.c_str();
  params.params.contentName = name.c_str();
  params.params.iconPath = "";
  params.params.playgoScenarioId = "0";
  params.params.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;  // (shown in Notifications > Downloads)
  params.slot = 0;
  int task = -1;
  int result = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&params, &task);
  Log("RegisterTaskByStorageEx(" + system + ", " + content + "): " + Said(result) + ", task " + std::to_string(task));
  if (result == ERROR_ALREADY_INSTALLED || result == ERROR_ALREADY_DOWNLOADING)
  {
    if (FileExists(data_dir + "/update-keep-tasks"))
    {
      *error = "the PS4 says " + Said(result) + " and update-keep-tasks keeps its old task";
      return false;
    }
    // EmuSuNi's old task (its last installation): released, and the new one registered at once.
    int old = -1;
    const int found = sceBgftServiceDownloadFindTaskByContentId(content.c_str(), ORBIS_BGFT_TASK_SUB_TYPE_GAME, &old);
    Log("FindTaskByContentId(" + content + "): " + Said(found) + ", task " + std::to_string(old));
    if (found != 0 || old < 0)
    {
      *error = "the PS4 says " + Said(result) + " and has no task of EmuSuNi to release";
      return false;
    }
    int64_t done = 0, total = 0;
    int task_error = 0;
    if (Read(old, -1, &done, &total, &task_error))
    {
      Log("the old task: " + std::to_string(done) + " of " + std::to_string(total) + " bytes, error " +
          Code(task_error));
      if (result == ERROR_ALREADY_DOWNLOADING && total > 0 && done < total && task_error == 0)
      {
        *error = "an installation of EmuSuNi is under way in Notifications > Downloads already";
        return false;
      }
    }
    const int stopped = sceBgftServiceIntDownloadStopTask(old);
    const int released = sceBgftServiceIntDownloadUnregisterTask(old);
    Log("the old task " + std::to_string(old) + ": stopped " + Said(stopped) + ", unregistered " + Said(released));
    task = -1;
    result = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&params, &task);
    Log("RegisterTaskByStorageEx again: " + Said(result) + ", task " + std::to_string(task));
  }
  if (result != 0 || task < 0)
  {
    *error = "the PS4's installer refused the package: " + Said(result);
    return false;
  }
  const int started = sceBgftServiceDownloadStartTask(task);
  Log("StartTask(" + std::to_string(task) + "): " + Said(started));
  if (started != 0)
  {
    // (A task may start by itself - flatz's write-up: then its progress says so.)
    int64_t done = 0, total = 0;
    int task_error = 0;
    if (!Read(task, -1, &done, &total, &task_error) || task_error != 0)
    {
      *error = "the PS4's installer didn't start it: " + Said(started);
      return false;
    }
  }
  s_task = task;
  return true;
}

Result Follow(int64_t size, std::atomic<bool>* stop, std::string* state, void (*progress)(int64_t done, int64_t total))
{
  int task;
  {
    std::lock_guard lock(s_mutex);
    task = s_task;
  }
  if (task < 0)
  {
    *state = "no task to follow";
    return Result::Unknown;
  }
  const auto start = std::chrono::steady_clock::now();
  auto last_move = start;
  int64_t last = -1;
  int failures = 0;
  while (!(stop && stop->load()))
  {
    int64_t done = 0, total = 0;
    int task_error = 0;
    bool read;
    {
      std::lock_guard lock(s_mutex);
      read = Read(task, size, &done, &total, &task_error);
    }
    const auto now = std::chrono::steady_clock::now();
    if (!read)
    {
      if (++failures >= 8)
      {
        *state = "its progress can't be read";
        return Result::Unknown;
      }
    }
    else
    {
      failures = 0;
      if (task_error != 0 && (done > 0 || now - start > std::chrono::seconds(20)))
      {
        *state = "the task failed: " + Said(task_error) + " at " + std::to_string(done) + " of " + std::to_string(total) +
                 " bytes";
        return Result::Failed;
      }
      if (progress && total > 0)
        progress(done, total);
      if (total > 0 && done >= total)
      {
        *state = "the PS4 copied all " + std::to_string(total) + " bytes";
        return Result::Installed;
      }
      if (done != last)
      {
        last = done;
        last_move = now;
      }
    }
    // Nothing moving for three minutes, or half an hour in all: not followed any more.
    if (now - last_move > std::chrono::minutes(3) || now - start > std::chrono::minutes(30))
    {
      *state = "no progress the app can read (" + std::to_string(last) + " bytes)";
      return Result::Unknown;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  *state = "stopped following it";
  return Result::Unknown;
}
}  // namespace Update::Install
