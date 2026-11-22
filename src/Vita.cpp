// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Vita.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <new>
#include <system_error>

#include <pthread.h>
#include <sys/stat.h>

#ifdef EMUSUNI_VITA_NOKEYS
#include <mbedtls/sha256.h>
#endif

#include "GameExtras.h"
#include "Log.h"
#include "Update.h"

namespace Vita
{
namespace
{
// Sony's firmware 3.74 for the PS Vita: its three files as Sony gives them (their sizes and SHA-256 as of
// 2026-11-12), told apart by their size whatever the player's names for them. The names here are the ones the core
// tells the packages apart by. No address: the app never asks Sony's servers for them (src/Vita.h).
struct Package
{
  const char* name;   // in <system>/vita3k
  const char* what;   // in the log and the messages
  int64_t size;
  const char* sha256;
};
const Package PACKAGES[PART_COUNT] = {
    {"PSP2UPDAT.PUP", "the system", 133834240, "6ef6dc8da6db026f28647713e473486d770087a605c52a8d751bfca7478386cf"},
    {"PSP2UPDAT_PREINST.PUP", "the preinstalled package", 128798720,
     "339d1439eb329cfbd1a936f0a1563458e0555ef52975486b2844e24b6049e33e"},
    {"PSP2UPDAT_FONT.PUP", "the fonts", 56778752, "c3c03fc7363dd573d90e5157629bf11551f434b283cc898d9ffc71dd716b791c"},
};
// The names of the copies StartCopy made in <system>/vita3k, one a line (RemoveCopiedFiles).
constexpr const char* COPIED_LIST = "emusuni-copies.txt";

std::string Lower(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

int64_t FileSize(const std::string& path)
{
  struct stat info = {};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) ? static_cast<int64_t>(info.st_size) : -1;
}

bool Exists(const std::string& path)
{
  struct stat info = {};
  return stat(path.c_str(), &info) == 0;
}

// A folder with something in it.
bool NonEmptyDir(const std::string& path)
{
  std::error_code ec;
  std::filesystem::directory_iterator it(path, ec);
  return !ec && it != std::filesystem::directory_iterator();
}

// The .PUP files of a folder, each with its lower-case name.
template <typename Each>
void EachPup(const std::string& folder, Each each)
{
  std::error_code ec;
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
  {
    const std::string path = it->path().string();
    const std::string name = Lower(it->path().filename().string());
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pup") == 0 && FileSize(path) > 0)
      each(name);
  }
}

std::string CoreDir(const std::string& system_dir)
{
  return system_dir + "/vita3k";
}

// ---- the copy from the player's own files ----
std::mutex s_mutex;
Status s_status;
std::atomic<bool> s_cancel{false};
pthread_t s_thread;
bool s_thread_made = false;
std::string s_system_dir;
std::vector<std::string> s_folders;  // where they are looked for

template <typename Change>
void Set(Change change)
{
  std::lock_guard lock(s_mutex);
  change(&s_status);
  s_status.serial++;
}

void Fail(const std::string& message)
{
  LogF("vita: firmware: the copy failed: %s", message.c_str());
  Set([&](Status* s) {
    s->phase = Phase::Failed;
    s->message = message;
  });
}

// The .PUP files of a folder that are as long as one of Sony's three, whatever their names: their paths, by the
// package. deeper: those of the folder's "firmware" (in any letters' case) too.
void LookForCopies(const std::string& folder, std::vector<std::string> found[PART_COUNT], bool deeper)
{
  std::error_code ec;
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
  {
    const std::string path = it->path().string();
    const std::string name = Lower(it->path().filename().string());
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0)
      continue;
    if (S_ISDIR(info.st_mode))
    {
      if (deeper && name == "firmware")
        LookForCopies(path, found, false);
      continue;
    }
    if (!S_ISREG(info.st_mode) || name.size() <= 4 || name.compare(name.size() - 4, 4, ".pup") != 0)
      continue;
    for (int p = 0; p < PART_COUNT; p++)
    {
      // (Once each: a folder may come twice.)
      if (static_cast<int64_t>(info.st_size) == PACKAGES[p].size &&
          std::find(found[p].begin(), found[p].end(), path) == found[p].end())
        found[p].push_back(path);
    }
  }
}

// `from` copied to `to` (made anew; not left there unless whole), `count` told the bytes so far.
enum class Copied { Whole, Stopped, NotRead, NotWritten };
template <typename Count>
Copied CopyFile(const std::string& from, const std::string& to, Count count)
{
  constexpr size_t PIECE = 1 << 20;
  char* buffer = new (std::nothrow) char[PIECE];
  FILE* in = buffer ? fopen(from.c_str(), "rb") : nullptr;
  FILE* out = in ? fopen(to.c_str(), "wb") : nullptr;
  Copied result = !in ? Copied::NotRead : !out ? Copied::NotWritten : Copied::Whole;
  int64_t done = 0;
  while (result == Copied::Whole)
  {
    const size_t got = fread(buffer, 1, PIECE, in);
    if (got == 0)
    {
      if (ferror(in))
        result = Copied::NotRead;
      break;
    }
    if (fwrite(buffer, 1, got, out) != got)
      result = Copied::NotWritten;
    else if (s_cancel.load())
      result = Copied::Stopped;
    else
      count(done += static_cast<int64_t>(got));
  }
  if (in)
    fclose(in);
  if (out && fclose(out) != 0 && result == Copied::Whole)
    result = Copied::NotWritten;
  if (out && result != Copied::Whole)
    std::remove(to.c_str());
  delete[] buffer;
  return result;
}

void CopyJob()
{
  const std::string core = CoreDir(s_system_dir);
  MakeDirs(core);
  // (What 01.26's download from Sony's servers may have left there: a part of a file. That download is gone.)
  for (const Package& package : PACKAGES)
    std::remove((core + "/" + package.name + ".part").c_str());
  const Firmware have = FindFirmware(s_system_dir);
  std::vector<std::string> found[PART_COUNT];
  for (const std::string& folder : s_folders)
  {
    LookForCopies(folder, found, true);
    LogF("vita: firmware: looked for the player's own .PUP files in %s", folder.c_str());
  }
  int64_t wanted = 0, there = 0;
  bool want[PART_COUNT];
  for (int p = 0; p < PART_COUNT; p++)
  {
    want[p] = !have.installed[p] && !have.file[p];
    if (!want[p])
      there += PACKAGES[p].size;
    else if (!found[p].empty())
      wanted += PACKAGES[p].size;
  }
  LogF("vita: firmware: %s; files of Sony's sizes there: %zu of the system's, %zu of the preinstalled package's, %zu "
       "of the fonts'",
       Describe(have).c_str(), found[SYSTEM].size(), found[PREINST].size(), found[FONTS].size());
  Set([&](Status* s) {
    s->phase = Phase::Copying;
    s->message.clear();
    s->done = there;
    s->total = FILES_BYTES;
  });
  // Room for the copies and for what the core makes of them.
  const int64_t free = GameExtras::FreeBytes(core);
  const int64_t need = wanted + static_cast<int64_t>(INSTALLED_MIB + 16) * 1048576;
  if (wanted > 0 && free >= 0 && free < need)
  {
    Fail("Not enough free space: " + std::to_string(need / 1048576) + " MB are needed, " +
         std::to_string(free / 1048576) + " MB are free.");
    return;
  }
  int64_t before = there;
  bool not_sonys = false, not_read = false;
  for (int p = 0; p < PART_COUNT; p++)
  {
    if (!want[p])
      continue;
    const Package& package = PACKAGES[p];
    const std::string path = core + "/" + package.name;
    const std::string copy = path + ".copy";
    // Each file of its size, until one is Sony's.
    for (const std::string& from : found[p])
    {
      Set([&](Status* s) {
        s->phase = Phase::Copying;
        s->part = p;
        s->done = before;
      });
      LogF("vita: firmware: %s: copying %s (%.1f MB)", package.what, from.c_str(), package.size / 1048576.0);
      const Copied copied =
          CopyFile(from, copy, [&](int64_t done) { Set([&](Status* s) { s->done = before + done; }); });
      if (copied == Copied::Stopped)
      {
        LogLine("vita: firmware: the copy was stopped by the player");
        Set([&](Status* s) {
          s->phase = Phase::Idle;
          s->done = before;
        });
        return;
      }
      if (copied == Copied::NotWritten)
      {
        LogF("vita: firmware: %s couldn't be written", copy.c_str());
        Fail("The file couldn't be written (no room left?)");
        return;
      }
      if (copied == Copied::NotRead)
      {
        LogF("vita: firmware: %s couldn't be read", from.c_str());
        not_read = true;
        continue;
      }
      // Checked before it counts: the copy's size, then its SHA-256.
      Set([&](Status* s) {
        s->phase = Phase::Verifying;
        s->done = before + package.size;
      });
      const std::string sha256 = FileSize(copy) == package.size ? Update::FileSha256(copy) : std::string();
      if (sha256 != package.sha256)
      {
        std::remove(copy.c_str());
        LogF("vita: firmware: %s is as long as %s but isn't it: SHA-256 %s, not %s", from.c_str(), package.what,
             sha256.empty() ? "(unreadable)" : sha256.c_str(), package.sha256);
        not_sonys = true;
        Set([&](Status* s) { s->not_sonys[p] = true; });  // ((01.72) the panel says which)
        continue;
      }
      Set([&](Status* s) { s->not_sonys[p] = false; });
      std::remove(path.c_str());
      if (std::rename(copy.c_str(), path.c_str()) != 0)
      {
        std::remove(copy.c_str());
        LogF("vita: firmware: %s couldn't be named %s", copy.c_str(), package.name);
        Fail("The file couldn't be written (no room left?)");
        return;
      }
      LogF("vita: firmware: %s is there, a copy of %s, its size and SHA-256 as expected", package.name, from.c_str());
      // (A copy of the app's making, deleted once installed: the player's file stays where it is.)
      const std::string list = core + "/" + COPIED_LIST;
      WriteTextFile(list, ReadTextFile(list) + package.name + "\n");
      before += package.size;
      break;
    }
  }
  const Firmware now = FindFirmware(s_system_dir);
  if (now.Ready())
  {
    LogLine("vita: firmware: the three packages are there: the next PS Vita game installs them");
    Set([](Status* s) {
      s->phase = Phase::Done;
      s->done = s->total;
    });
    return;
  }
  int ready = 0;
  for (int p = 0; p < PART_COUNT; p++)
    ready += now.installed[p] || now.file[p];
  LogF("vita: firmware: after the copy: %s", Describe(now).c_str());
  Set([&](Status* s) { s->done = before; });
  if (not_read)
    Fail("The drive's data can't be read");
  else if (not_sonys)
    Fail("A .PUP file in Roms/psvita isn't Sony's firmware (its checksum is wrong): download it again.");
  else if (ready == 0)
    Fail("Sony's firmware files aren't in Roms/psvita.");
  else
    Fail("Only " + std::to_string(ready) + " of the 3 firmware files are there: the others aren't in Roms/psvita.");
}

// (pthread, not std::thread: a thread that can't be made is an error code, never an exception - as src/Update.cpp's.)
void* Thread(void*)
{
  CopyJob();
  Set([](Status* s) { s->busy = false; });
  return nullptr;
}
}  // namespace

void StartCopy(const std::string& system_dir, const std::vector<std::string>& folders)
{
  std::lock_guard lock(s_mutex);
  if (s_status.busy)
    return;
  if (s_thread_made)
  {
    pthread_join(s_thread, nullptr);  // (finished: busy is false)
    s_thread_made = false;
  }
  s_system_dir = system_dir;
  s_folders = folders;
  s_cancel = false;
  s_status.busy = true;
  s_status.phase = Phase::Copying;
  s_status.message.clear();
  for (bool& not_sonys : s_status.not_sonys)
    not_sonys = false;
  s_status.serial++;
  const int made = pthread_create(&s_thread, nullptr, Thread, nullptr);
  if (made != 0)
  {
    // (No thread: nothing is copied, the panel stays as it was; the log says why.)
    s_status.busy = false;
    s_status.phase = Phase::Idle;
    s_status.serial++;
    LogF("vita: firmware: the copy's thread couldn't start (pthread_create: %d)", made);
    return;
  }
  s_thread_made = true;
}

void StopCopy()
{
  s_cancel = true;
}

Status GetStatus()
{
  std::lock_guard lock(s_mutex);
  return s_status;
}

const char* PackageName(int part)
{
  return part >= 0 && part < PART_COUNT ? PACKAGES[part].name : "";
}

int64_t PackageBytes(int part)
{
  return part >= 0 && part < PART_COUNT ? PACKAGES[part].size : 0;
}

void FindPlayerFiles(const std::vector<std::string>& folders, bool found[PART_COUNT])
{
  std::vector<std::string> paths[PART_COUNT];
  for (const std::string& folder : folders)
    LookForCopies(folder, paths, true);
  for (int p = 0; p < PART_COUNT; p++)
    found[p] = !paths[p].empty();
}

int Serial()
{
  std::lock_guard lock(s_mutex);
  return s_status.serial;
}

Firmware FindFirmware(const std::string& system_dir)
{
  Firmware firmware;
  const std::string core = CoreDir(system_dir);
  // (The core's note of an install under way: one cut short left it, and that part is installed again.)
  const std::string note = core + "/ux0/libretro_pkg/pup_";
  firmware.installed[SYSTEM] = NonEmptyDir(core + "/vs0/sys/external") && !Exists(note + "firmware");
  firmware.installed[PREINST] = NonEmptyDir(core + "/pd0") && !Exists(note + "preinst");
  firmware.installed[FONTS] = NonEmptyDir(core + "/sa0") && !Exists(note + "font");
  EachPup(core, [&](const std::string& name) {
    const Part part = name.find("preinst") != std::string::npos ? PREINST :
                      name.find("font") != std::string::npos    ? FONTS :
                                                                  SYSTEM;
    firmware.file[part] = true;
  });
  // The system's also one folder up, under the names Sony gives it.
  EachPup(system_dir, [&](const std::string& name) {
    if (name == "psp2updat.pup" || name == "psvupdat.pup")
      firmware.file[SYSTEM] = true;
  });
  return firmware;
}

std::string Describe(const Firmware& firmware)
{
  std::string text;
  for (int p = 0; p < PART_COUNT; p++)
  {
    text += std::string(p ? ", " : "") + PACKAGES[p].what + " " +
            (firmware.installed[p] ? "installed" : firmware.file[p] ? "a .PUP to install" : "missing");
  }
  return text;
}

namespace
{
constexpr const char* MISSING_FIRST_LINE = "The PS Vita's firmware isn't installed.";
// (with-vita-nokeys) The firmware's .PUP files are there, the keys to install them aren't: KeysMessage.
constexpr const char* KEYS_FIRMWARE_FIRST_LINE = "The PS Vita's firmware can't be installed without its keys.";
}

std::string MissingMessage(const Firmware& firmware, const std::string& system_dir)
{
  std::string missing;
  for (int p = 0; p < PART_COUNT; p++)
  {
    if (!firmware.installed[p] && !firmware.file[p])
      missing += std::string(missing.empty() ? "" : ", ") + PACKAGES[p].what;
  }
  // (The menu opens the firmware's panel on it, which says where the files go; system_dir: for the log's reader.)
  (void)system_dir;
  return std::string(MISSING_FIRST_LINE) + "\nMissing: " + missing + ".\n" + FILES_WORDS;
}

bool IsMissingMessage(const std::string& error)
{
  return error.compare(0, std::char_traits<char>::length(MISSING_FIRST_LINE), MISSING_FIRST_LINE) == 0 ||
         error.compare(0, std::char_traits<char>::length(KEYS_FIRMWARE_FIRST_LINE), KEYS_FIRMWARE_FIRST_LINE) == 0;
}

// ---- The keys ----
#ifdef EMUSUNI_VITA_NOKEYS
namespace
{
constexpr const char* KEYS_FILE = "vita-keys.txt";

// The file's SHA-256 as scripts/make-vita-keys.py tells it (and the core, port/ps4_vita_keys.cpp): of each line's
// words one after the other and a '\n', without the comments and the empty lines. "" when it can't be read.
std::string KeysDigest(const std::string& path)
{
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return {};
  std::string text;
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < (1u << 20))
    text.append(buffer, got);
  fclose(file);
  std::string all;
  for (size_t at = 0; at < text.size();)
  {
    size_t end = text.find('\n', at);
    end = end == std::string::npos ? text.size() : end;
    std::string line;
    for (size_t i = at; i < end; i++)
    {
      if (static_cast<unsigned char>(text[i]) > ' ')
        line += text[i];
    }
    if (!line.empty() && line[0] != '#')
      all += line + '\n';
    at = end + 1;
  }
  unsigned char digest[32];
  if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(all.data()), all.size(), digest, 0) != 0)
    return {};
  char hex[65];
  for (int i = 0; i < 32; i++)
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  return hex;
}
}  // namespace

Keys FindKeys(const std::string& system_dir)
{
  const std::string path = CoreDir(system_dir) + "/" + KEYS_FILE;
  if (FileSize(path) < 0)
    return Keys::Missing;
  return KeysDigest(path) == EMUSUNI_VITA_KEYS_SHA256 ? Keys::Ready : Keys::Wrong;
}

std::string DescribeKeys(Keys keys, const std::string& system_dir)
{
  const std::string path = CoreDir(system_dir) + "/" + KEYS_FILE;
  return keys == Keys::Ready   ? path + " is the file expected" :
         keys == Keys::Missing ? path + " isn't there (this build has no keys inside)" :
                                 path + " isn't the file expected (this build has no keys inside)";
}

std::string KeysMessage(Keys keys, const std::string& system_dir, bool for_firmware)
{
  const std::string what = keys == Keys::Wrong ? "The vita-keys.txt there isn't the file this version expects.\n" :
                                                 "This version of EmuSuNi has no PS Vita keys inside.\n";
  return std::string(for_firmware ? KEYS_FIRMWARE_FIRST_LINE : "PS Vita games can't start without the PS Vita's keys.") +
         "\n" + what + "Copy the file vita-keys.txt to\n" + CoreDir(system_dir);
}

std::string KeysNote(const std::string& system_dir)
{
  const Keys keys = FindKeys(system_dir);
  if (keys == Keys::Ready)
    return {};
  return std::string(keys == Keys::Wrong ? "The vita-keys.txt there isn't the file this version expects." :
                                           "This version of EmuSuNi has no PS Vita keys inside.") +
         "\nThe firmware's install and every PS Vita game need the file vita-keys.txt in " + CoreDir(system_dir);
}
#else
Keys FindKeys(const std::string&)
{
  return Keys::Inside;
}

std::string DescribeKeys(Keys, const std::string&)
{
  return "inside the core";
}

std::string KeysMessage(Keys, const std::string&, bool)
{
  return {};
}

std::string KeysNote(const std::string&)
{
  return {};
}
#endif

void RemoveCopiedFiles(const std::string& system_dir)
{
  const std::string core = CoreDir(system_dir);
  const std::string list = core + "/" + COPIED_LIST;
  if (!Exists(list) || !FindFirmware(system_dir).Installed())
    return;
  {
    std::lock_guard lock(s_mutex);
    if (s_status.busy)
      return;  // (a copy under way writes them)
  }
  const std::string names = ReadTextFile(list);
  int64_t freed = 0;
  for (const Package& package : PACKAGES)
  {
    // (Only the names the copy gives, and only those its list has.)
    if (names.find(std::string(package.name) + "\n") == std::string::npos)
      continue;
    const std::string path = core + "/" + package.name;
    const int64_t size = FileSize(path);
    if (size >= 0 && std::remove(path.c_str()) == 0)
      freed += size;
  }
  std::remove(list.c_str());
  LogF("vita: firmware: installed; the copies of its .PUP files are deleted (%.1f MB back)", freed / 1048576.0);
}
}  // namespace Vita
