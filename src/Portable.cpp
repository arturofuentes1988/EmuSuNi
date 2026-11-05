// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Portable data (00.87): see Portable.h.

#include "Portable.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <thread>

#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "DrivePaths.h"
#include "Log.h"

namespace Portable
{
namespace
{
struct State
{
  std::mutex mutex;
  std::string local = App::DATA_DIR;
  std::string user;    // "" = local
  std::string root;    // the drive in use this session ("/usb1")
  std::string id;      // its data's id
  std::string folder;  // root/EmuSuNi-Portable
  bool active = false, lost = false;
  bool write_warned = false;
  std::string warning;
  std::map<std::string, std::string> answers;  // data id -> home, yes, no, off
  std::map<std::string, std::string> writers;  // data id -> the library cache's last writer word (this PS4's)
};

State& S()
{
  static State state;
  return state;
}

std::string LocalFile(const std::string& local)
{
  return local + "/portable-drives.ini";
}

// portable-drives.ini's text from the state (under its lock).
std::string LocalText(const State& s)
{
  std::string text = "# EmuSuNi: the USB drives with portable data this PS4 has seen (Settings -> Keep My Data on the USB "
                     "Drive)\n# drive.<its data's id>: home (turned on here), yes (used here), no (not used), off "
                     "(turned off here)\n";
  for (const auto& [id, answer] : s.answers)
    text += "drive." + id + "=" + answer + "\n";
  for (const auto& [id, word] : s.writers)
    text += "cache." + id + "=" + word + "\n";
  return text;
}

// A data id as portable.ini may have it: letters and digits, at most 32 ("" for anything else).
std::string CleanId(std::string id)
{
  while (!id.empty() && (id.back() == '\r' || id.back() == ' '))
    id.pop_back();
  if (id.empty() || id.size() > 32)
    return {};
  for (const char c : id)
  {
    if (!std::isalnum(static_cast<unsigned char>(c)))
      return {};
  }
  return id;
}

uint64_t Mix(uint64_t x)
{
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// The file's size and modification time (false: not a file there).
bool Stat(const std::string& path, uint64_t* size, int64_t* time)
{
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode))
    return false;
  *size = static_cast<uint64_t>(st.st_size);
  *time = static_cast<int64_t>(st.st_mtime);
  return true;
}

bool IsDir(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string Parent(const std::string& path)
{
  const size_t slash = path.rfind('/');
  return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

// Written whole through a temporary file, then renamed (fsync'd when `sync`): a cut leaves the file before.
bool WriteWhole(const std::string& path, const std::string& data, bool sync, std::string* error)
{
  const std::string temp = path + ".new";
  FILE* file = std::fopen(temp.c_str(), "wb");
  if (!file)
  {
    const int open_errno = errno;
    *error = temp + ": " + std::strerror(open_errno);
    errno = open_errno;  // (the caller's TargetResult reads it)
    return false;
  }
  bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
  const int write_errno = errno;
  ok = std::fflush(file) == 0 && ok;
  if (sync)
    fsync(fileno(file));
  ok = std::fclose(file) == 0 && ok;
  if (!ok || std::rename(temp.c_str(), path.c_str()) != 0)
  {
    const int failed_errno = ok ? errno : write_errno;
    *error = path + ": " + std::strerror(failed_errno);
    std::remove(temp.c_str());
    errno = failed_errno;
    return false;
  }
  return true;
}

// ---- What travels ----
const char* const ROOT_FILES[] = {"xmb.ini", "settings.ini", "controls.ini", "favorites.ini", "names.ini", "played.ini"};
const char* const FOLDERS[] = {"saves", "states", "cheats", "bezels", "covers", "themes"};
const char* const LIBRARY = "cache/library.tsv";
// Never copied: Azahar's keys (sysdata: like a BIOS, the player's to put there) and its log.
const char* const LEFT_OUT[] = {"saves/3ds/Azahar/sysdata", "saves/3ds/Azahar/log"};

enum Kind
{
  PLAIN,
  KEY_VALUE,  // xmb.ini: "key=value", its paths in the values
  SPACED,     // played.ini: "<time> <path>"
  TSV,        // the library's cache
};

Kind KindOf(const std::string& rel)
{
  if (rel == "xmb.ini")
    return KEY_VALUE;
  if (rel == "played.ini")
    return SPACED;
  if (rel == LIBRARY)
    return TSV;
  return PLAIN;
}

bool Temporary(const std::string& name)
{
  auto ends = [&name](const char* tail) {
    const size_t n = std::strlen(tail);
    return name.size() >= n && name.compare(name.size() - n, n, tail) == 0;
  };
  return name.empty() || ends(".new") || ends(".tmp") || ends(".part") || name.rfind(".preview", 0) == 0;
}

struct Item
{
  std::string rel;
  uint64_t size = 0;
  int64_t time = 0;
};

// Reconcile: the files a session without the drive wrote that are the player's progress - the settings' files, the
// battery saves (a .srm, its clock, its disc: not a core's own folder), the states, the cheats, the pictures.
bool Reconciled(const std::string& rel)
{
  for (const char* file : ROOT_FILES)
  {
    if (rel == file)
      return true;
  }
  for (const char* folder : {"states/", "cheats/", "covers/", "themes/", "bezels/"})
  {
    if (rel.rfind(folder, 0) == 0)
      return true;
  }
  if (rel.rfind("saves/", 0) != 0 || std::count(rel.begin(), rel.end(), '/') > 2)
    return false;
  for (const char* ext : {".srm", ".rtc", ".disc"})
  {
    const size_t n = std::strlen(ext);
    if (rel.size() > n && rel.compare(rel.size() - n, n, ext) == 0)
      return true;
  }
  return false;
}

void ListFolder(const std::string& base, const std::string& rel, int depth, std::vector<Item>* items,
                CopyProgress* progress)
{
  for (const char* out : LEFT_OUT)
  {
    if (rel == out)
      return;
  }
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(base + "/" + rel, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
  {
    if (progress->stop.load())
      return;
    const std::string name = it->path().filename().string();
    if (Temporary(name))
      continue;
    const std::string child = rel + "/" + name;
    std::error_code type_error;
    if (it->is_directory(type_error))
    {
      if (depth < 16)
        ListFolder(base, child, depth + 1, items, progress);
      continue;
    }
    Item item;
    item.rel = child;
    if (Stat(base + "/" + child, &item.size, &item.time))
      items->push_back(std::move(item));
  }
}

void SetError(CopyProgress* progress, const std::string& error)
{
  std::lock_guard lock(progress->mutex);
  if (progress->error.empty())
    progress->error = error;
}

// The library's cache with its writer word: the old one's line out, the new one after the version's line.
std::string WithWriter(const std::string& tsv, const std::string& writer)
{
  std::string out;
  out.reserve(tsv.size() + 40);
  size_t start = 0;
  bool put = writer.empty();
  while (start < tsv.size())
  {
    size_t end = tsv.find('\n', start);
    const bool last = end == std::string::npos;
    if (last)
      end = tsv.size();
    const std::string line = tsv.substr(start, end - start);
    if (line.rfind("W\t", 0) != 0)
    {
      out += line;
      if (!last)
        out.push_back('\n');
      if (!put && line.rfind("V\t", 0) == 0)
      {
        out += "W\t" + writer + "\n";
        put = true;
      }
    }
    start = end + 1;
  }
  return out;
}

enum class Result
{
  Copied,
  SourceFailed,
  TargetFailed,
  NameRefused,  // the drive doesn't take the file's name (FAT and exFAT: none of " * : < > ? \ | in it)
};

// What a failed write on the drive says: its name refused, or the drive itself (full, gone, read-only).
Result TargetResult(int error)
{
  return error == EINVAL || error == EILSEQ || error == ENAMETOOLONG ? Result::NameRefused : Result::TargetFailed;
}

// A path a FAT or exFAT drive takes (none of " * : < > ? \ | or a control character in it).
bool FatName(const std::string& rel)
{
  for (const char c : rel)
  {
    if (static_cast<unsigned char>(c) < 0x20 || std::strchr("\"*:<>?\\|", c) != nullptr)
      return false;
  }
  return true;
}

Result CopyOne(const CopyPlan& plan, const std::string& rel, std::vector<char>* buffer, std::string* error)
{
  const std::string src = plan.from + "/" + rel, dst = plan.to + "/" + rel;
  // Pictures aren't synced one by one (a drive's thousands of covers: minutes); a save, a state, a setting is.
  const bool sync = rel.rfind("covers/", 0) != 0 && rel.rfind("bezels/", 0) != 0 && rel.rfind("themes/", 0) != 0;
  const Kind kind = KindOf(rel);
  if (kind != PLAIN && plan.copying != Copying::Reconcile)
  {
    FILE* probe = std::fopen(src.c_str(), "rb");
    if (!probe)
    {
      *error = src + ": " + std::strerror(errno);
      return Result::SourceFailed;
    }
    std::fclose(probe);
    std::string text = ReadTextFile(src);
    const bool stored = plan.copying == Copying::ToDrive;
    if (kind == TSV)
      text = WithWriter(DrivePaths::TsvFields(text, plan.drive, stored), plan.writer);
    else
      text = DrivePaths::LinesAfter(text, kind == KEY_VALUE ? '=' : ' ', plan.drive, stored);
    if (WriteWhole(dst, text, sync, error))
      return Result::Copied;
    return TargetResult(errno);
  }
  FILE* in = std::fopen(src.c_str(), "rb");
  if (!in)
  {
    *error = src + ": " + std::strerror(errno);
    return Result::SourceFailed;
  }
  const std::string temp = dst + ".new";
  FILE* out = std::fopen(temp.c_str(), "wb");
  if (!out)
  {
    const int open_errno = errno;
    *error = temp + ": " + std::strerror(open_errno);
    std::fclose(in);
    return TargetResult(open_errno);
  }
  bool read_ok = true, write_ok = true;
  int failure = 0;
  for (;;)
  {
    const size_t got = std::fread(buffer->data(), 1, buffer->size(), in);
    if (got > 0 && std::fwrite(buffer->data(), 1, got, out) != got)
    {
      write_ok = false;
      failure = errno;
      break;
    }
    if (got < buffer->size())
    {
      read_ok = !std::ferror(in);
      if (!read_ok)
        failure = errno;
      break;
    }
  }
  std::fclose(in);
  write_ok = std::fflush(out) == 0 && write_ok;
  if (write_ok && sync)
    fsync(fileno(out));
  write_ok = std::fclose(out) == 0 && write_ok;
  if (!read_ok || !write_ok || std::rename(temp.c_str(), dst.c_str()) != 0)
  {
    if (failure == 0)
      failure = errno;
    std::remove(temp.c_str());
    *error = (read_ok ? dst : src) + ": " + std::strerror(failure);
    return read_ok ? TargetResult(failure) : Result::SourceFailed;
  }
  return Result::Copied;
}

std::string NewId()
{
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  uint64_t a = static_cast<uint64_t>(now.tv_nsec) ^ (static_cast<uint64_t>(now.tv_sec) << 30);
#if defined(__x86_64__)
  a ^= Mix(__builtin_ia32_rdtsc());
#endif
  void* heap = std::malloc(16);
  uint64_t b = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&now)) ^
               (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(heap)) << 7) ^ static_cast<uint64_t>(getpid());
  std::free(heap);
  static std::atomic<uint64_t> counter{0};
  b ^= Mix(counter.fetch_add(1) + 0x9E3779B97F4A7C15ull);
  for (int round = 0; round < 4; round++)
  {
    a = Mix(a ^ b);
#if defined(__x86_64__)
    b = Mix(b ^ a ^ __builtin_ia32_rdtsc());
#else
    b = Mix(b ^ a);
#endif
  }
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(a));
  return text;
}

// The drive's BIOS for a console: the files its core reads from the system folder, any of them there.
bool HasBios(const std::string& sys, const std::string& key)
{
  static const std::map<std::string, std::vector<const char*>> NAMES = {
      {"nes", {"disksys.rom"}},
      {"gb", {"gb_bios.bin", "sgb_bios.bin"}},
      {"gbc", {"gbc_bios.bin", "gb_bios.bin"}},
      {"gba", {"gba_bios.bin"}},
      {"nds", {"bios7.bin", "bios9.bin", "firmware.bin", "melonDS DS/bios7.bin", "melonDS DS/bios9.bin",
               "melonDS DS/firmware.bin"}},
      {"dc", {"dc/dc_boot.bin"}},
      {"naomi", {"dc/naomi.zip", "dc/naomi2.zip"}},
      {"atomiswave", {"dc/awbios.zip"}},
      // (00.90) Atari's optional ones, right in system (ProSystem's names; Virtual Jaguar's: src/Emulator.cpp).
      {"atari7800", {"7800 BIOS (U).rom", "7800 BIOS (E).rom"}},
      {"atarijaguar", {"[BIOS] Atari Jaguar (World).j64", "jagboot.rom"}},
  };
  const auto it = NAMES.find(key);
  if (it != NAMES.end())
  {
    for (const char* name : it->second)
    {
      if (FileExists(sys + "/" + name))
        return true;
    }
    return false;
  }
  // The PlayStation's: any 512 KiB .bin (SwanStation finds its BIOS by its contents); the Arcade's: a zip (neogeo.zip
  // and the like) in system or system/fbneo; the Saturn's: anything in system/saturn.
  auto any = [](const std::string& dir, const char* ext, uint64_t size) {
    std::error_code ec;
    for (auto entry = std::filesystem::directory_iterator(dir, ec); !ec && entry != std::filesystem::directory_iterator();
         entry.increment(ec))
    {
      const std::string name = entry->path().filename().string();
      if (ext && (name.size() < 4 || strcasecmp(name.c_str() + name.size() - 4, ext) != 0))
        continue;
      uint64_t bytes = 0;
      int64_t time = 0;
      if (Stat(entry->path().string(), &bytes, &time) && (size == 0 || bytes == size))
        return true;
    }
    return false;
  };
  if (key == "ps1")
    return any(sys, ".bin", 512 * 1024);
  if (key == "arcade")
    return any(sys, ".zip", 0) || any(sys + "/fbneo", ".zip", 0);
  if (key == "saturn")
    return any(sys + "/saturn", nullptr, 0);
  return false;
}

// Lose: the drive in use is gone; the fallback folder from now on.
void Lose(const std::string& why)
{
  std::string user, folder;
  {
    std::lock_guard lock(S().mutex);
    State& s = S();
    if (!s.active || s.lost)
      return;
    s.lost = true;
    s.active = false;
    s.user = s.local + "/portable-fallback/" + s.id;
    s.warning = "The USB drive with your data was removed: saving to this PS4 until EmuSuNi starts again with it";
    user = s.user;
    folder = s.folder;
  }
  MakeDirs(user);
  LogF("portable: the USB drive with the data is gone (%s): %s; from now on saving to %s (copied to the drive at the "
       "next start with it)",
       why.c_str(), folder.c_str(), user.c_str());
}

// Start's copy of what a session without the drive wrote (the fallback folder), then that folder kept aside.
void ReconcileFallback(const std::string& local, const std::string& id, const std::string& root,
                       const std::string& folder)
{
  const std::string fallback = local + "/portable-fallback/" + id;
  if (!IsDir(fallback))
    return;
  const auto start = std::chrono::steady_clock::now();
  CopyPlan plan;
  plan.copying = Copying::Reconcile;
  plan.policy = Policy::Newer;
  plan.from = fallback;
  plan.to = folder;
  plan.drive = root;
  CopyProgress progress;
  const bool ok = CopyData(plan, &progress);
  LogF("portable: what was saved while the drive was away (%s): %d copied to it, %d older than the drive's, %d the "
       "same%s (%.0f ms)",
       fallback.c_str(), progress.copied.load(), progress.kept.load(), progress.same.load(),
       ok ? "" : (", " + std::to_string(progress.failed.load()) + " failed: " + progress.error).c_str(),
       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  for (const std::string& left : progress.left)
    LogF("portable: %s/%s stays on this PS4 (a core's own file: copy it by hand if you need it)", fallback.c_str(),
         left.c_str());
  if (ok)
  {
    // (Kept, never deleted: a copy on this PS4 of what went to the drive.)
    const std::string aside = fallback + ".copied-" + std::to_string(static_cast<long long>(std::time(nullptr)));
    if (std::rename(fallback.c_str(), aside.c_str()) == 0)
      LogF("portable: %s kept as %s", fallback.c_str(), aside.c_str());
  }
}
}  // namespace

// ---- The start ----

void Start(const std::string& local_dir, const std::string& usb_root, int wait_ms)
{
  {
    std::lock_guard lock(S().mutex);
    State& s = S();
    s.local = local_dir;
    s.user.clear();
    s.root.clear();
    s.id.clear();
    s.folder.clear();
    s.active = s.lost = s.write_warned = false;
    s.warning.clear();
    s.answers.clear();
    s.writers.clear();
  }
  // This PS4's answers.
  const std::string text = ReadTextFile(LocalFile(local_dir));
  bool any_yes = false;
  {
    std::lock_guard lock(S().mutex);
    size_t at = 0;
    while (at < text.size())
    {
      const size_t end = std::min(text.find('\n', at), text.size());
      std::string line = text.substr(at, end - at);
      at = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos)
        continue;
      const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
      if (key.rfind("drive.", 0) == 0 && !CleanId(key.substr(6)).empty() &&
          (value == "home" || value == "yes" || value == "no" || value == "off"))
      {
        S().answers[key.substr(6)] = value;
        any_yes = any_yes || value == "home" || value == "yes";
      }
      else if (key.rfind("cache.", 0) == 0 && !CleanId(key.substr(6)).empty() && !CleanId(value).empty())
      {
        S().writers[key.substr(6)] = value;
      }
    }
  }
  if (!any_yes)
  {
    LogF("portable: this PS4's data (%s): no USB drive's data was used here", local_dir.c_str());
    return;
  }
  // The drives' portable.ini, on a thread: a drive that doesn't answer (asleep, broken) can't hold the start.
  struct Probe
  {
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;
    std::vector<std::pair<std::string, std::string>> found;  // root, id
  };
  auto probe = std::make_shared<Probe>();
  const auto start = std::chrono::steady_clock::now();
  std::thread([probe, usb_root] {
    std::vector<std::pair<std::string, std::string>> found;
    for (int i = 0; i < 8; i++)
    {
      const std::string root = usb_root + "/usb" + std::to_string(i);
      const std::string id = DriveDataId(root);
      if (!id.empty())
        found.emplace_back(root, id);
    }
    std::lock_guard lock(probe->mutex);
    probe->found = std::move(found);
    probe->done = true;
    probe->done_cv.notify_all();
  }).detach();
  std::vector<std::pair<std::string, std::string>> found;
  {
    std::unique_lock lock(probe->mutex);
    if (!probe->done_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&probe] { return probe->done; }))
    {
      LogF("portable: the USB drives didn't answer in %.1f s: this PS4's data this time (%s)", wait_ms / 1000.0,
           local_dir.c_str());
      std::lock_guard state_lock(S().mutex);
      S().warning = "The USB drive with your data didn't answer: this PS4's data is used this time";
      return;
    }
    found = probe->found;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::string root, id, answer;
  for (const auto& [drive, data] : found)
  {
    const std::string said = AnswerFor(data);
    LogF("portable: %s/%s: data %s (%s)", drive.c_str(), FOLDER, data.c_str(),
         said.empty() ? "never asked here" : said.c_str());
    if (root.empty() && (said == "home" || said == "yes"))
    {
      root = drive;
      id = data;
      answer = said;
    }
  }
  if (root.empty())
  {
    LogF("portable: this PS4's data (%s): no USB drive with data used here is connected (looked in %.0f ms)",
         local_dir.c_str(), ms);
    return;
  }
  const std::string folder = FolderOf(root);
  {
    std::lock_guard lock(S().mutex);
    State& s = S();
    s.root = root;
    s.id = id;
    s.folder = folder;
    s.user = folder;
    s.active = true;
  }
  LogF("portable: the player's data is on the USB drive: %s (data %s, %s; looked in %.0f ms)",
       folder.c_str(), id.c_str(), answer == "home" ? "turned on here" : "used here", ms);
  ReconcileFallback(local_dir, id, root, folder);
}

std::string LocalDir()
{
  std::lock_guard lock(S().mutex);
  return S().local;
}

std::string UserDir()
{
  std::lock_guard lock(S().mutex);
  return S().user.empty() ? S().local : S().user;
}

bool Active()
{
  std::lock_guard lock(S().mutex);
  return S().active;
}

bool Lost()
{
  std::lock_guard lock(S().mutex);
  return S().lost;
}

std::string DriveRoot()
{
  std::lock_guard lock(S().mutex);
  return S().root;
}

std::string DriveId()
{
  std::lock_guard lock(S().mutex);
  return S().id;
}

std::string DataFolder()
{
  std::lock_guard lock(S().mutex);
  return S().folder;
}

std::string TakeWarning()
{
  std::lock_guard lock(S().mutex);
  std::string warning;
  warning.swap(S().warning);
  return warning;
}

std::string ToStored(const std::string& path)
{
  return DrivePaths::Stored(path, DriveRoot());
}

std::string FromStored(const std::string& stored)
{
  return DrivePaths::Resolved(stored, DriveRoot());
}

std::string SystemDir(const std::string& console_key)
{
  std::string local, folder;
  {
    std::lock_guard lock(S().mutex);
    local = S().local + "/system";
    if (!S().active)
      return local;
    folder = S().folder;
  }
  const std::string sys = folder + "/system";
  if (!IsDir(sys) || !HasBios(sys, console_key))
    return local;
  LogF("portable: %s: the BIOS from the USB drive's %s", console_key.c_str(), sys.c_str());
  return sys;
}

std::vector<std::string> BiosFolders(const std::string& sub)
{
  std::vector<std::string> folders;
  std::string local, folder;
  {
    std::lock_guard lock(S().mutex);
    local = S().local;
    if (S().active)
      folder = S().folder;
  }
  if (!folder.empty())
  {
    folders.push_back(folder + "/system/" + sub);
    folders.push_back(folder + "/system");
  }
  folders.push_back(local + "/system/" + sub);
  folders.push_back(local + "/system");
  return folders;
}

bool CheckDrive()
{
  std::string marker;
  {
    std::lock_guard lock(S().mutex);
    if (!S().active)
      return true;
    marker = S().folder + "/" + MARKER;
  }
  if (FileExists(marker))
    return true;
  Lose("its " + std::string(MARKER) + " isn't there");
  return false;
}

std::string Redirect(const std::string& failed_path)
{
  std::string folder;
  bool lost = false;
  {
    std::lock_guard lock(S().mutex);
    folder = S().folder;
    lost = S().lost;
  }
  if (folder.empty() || failed_path.size() <= folder.size() + 1 ||
      DrivePaths::RootAt(failed_path, folder) == std::string::npos)
    return {};
  if (!lost)
  {
    if (FileExists(folder + "/" + MARKER))
    {
      // (Still there: a full or read-only drive. The write fails, as on the PS4's own storage.)
      LogF("portable: %s couldn't be written, and the drive is still there (full or read-only?)", failed_path.c_str());
      std::lock_guard lock(S().mutex);
      if (!S().write_warned)
      {
        S().write_warned = true;
        S().warning = "Couldn't save to the USB drive: is it full?";
      }
      return {};
    }
    Lose("a write to it failed");
  }
  const std::string redirected = UserDir() + failed_path.substr(folder.size());
  MakeDirs(Parent(redirected));
  LogF("portable: %s written to %s instead (the drive is gone)", failed_path.c_str(), redirected.c_str());
  return redirected;
}

// ---- The menu ----

std::vector<DriveData> Look(const std::vector<std::string>& drives)
{
  std::vector<DriveData> found;
  for (const std::string& root : drives)
  {
    DriveData data;
    data.root = root;
    data.id = DriveDataId(root);
    if (data.id.empty())
      continue;
    data.answer = AnswerFor(data.id);
    {
      std::lock_guard lock(S().mutex);
      data.in_use = S().active && S().id == data.id;
    }
    found.push_back(std::move(data));
  }
  return found;
}

void Remember(const std::string& id, const std::string& answer)
{
  if (CleanId(id).empty())
    return;
  std::string path, text;
  {
    std::lock_guard lock(S().mutex);
    S().answers[id] = answer;
    path = LocalFile(S().local);
    text = LocalText(S());
  }
  WriteTextFile(path, text);
  LogF("portable: data %s: \"%s\" on this PS4", id.c_str(), answer.c_str());
}

std::string AnswerFor(const std::string& id)
{
  std::lock_guard lock(S().mutex);
  const auto it = S().answers.find(id);
  return it == S().answers.end() ? std::string() : it->second;
}

std::string NewWord()
{
  return NewId();
}

std::string LibraryWriter()
{
  std::lock_guard lock(S().mutex);
  const auto it = S().writers.find(S().id);
  return S().id.empty() || it == S().writers.end() ? std::string() : it->second;
}

void SetLibraryWriter(const std::string& word, const std::string& id)
{
  std::string path, text;
  {
    std::lock_guard lock(S().mutex);
    const std::string drive = id.empty() ? S().id : id;
    if (CleanId(drive).empty())
      return;
    S().writers[drive] = word;
    path = LocalFile(S().local);
    text = LocalText(S());
  }
  WriteTextFile(path, text);
}

// ---- Copies ----

bool CopyData(const CopyPlan& plan, CopyProgress* progress)
{
  // What there is to copy (its sizes: the progress's total).
  std::vector<Item> items;
  auto add_file = [&](const std::string& rel) {
    Item item;
    item.rel = rel;
    if (Stat(plan.from + "/" + rel, &item.size, &item.time))
      items.push_back(std::move(item));
  };
  if (plan.copying == Copying::Reconcile)
  {
    std::vector<Item> all;
    for (const char* file : ROOT_FILES)
      add_file(file);
    for (const char* folder : FOLDERS)
      ListFolder(plan.from, folder, 1, &all, progress);
    for (Item& item : all)
    {
      if (Reconciled(item.rel))
        items.push_back(std::move(item));
      else
        progress->left.push_back(item.rel);
    }
  }
  else
  {
    for (const char* file : ROOT_FILES)
      add_file(file);
    if (plan.copying == Copying::ToDrive)
      add_file(LIBRARY);  // (Back: this PS4 reads its own games again)
    for (const char* folder : FOLDERS)
      ListFolder(plan.from, folder, 1, &items, progress);
  }
  uint64_t total = 0;
  for (const Item& item : items)
    total += item.size;
  progress->files = static_cast<int>(items.size());
  progress->bytes_total = total;
  progress->listing = false;
  // Each file: copied when the policy says so.
  std::vector<char> buffer(1 << 20);
  std::set<std::string> made;
  bool complete = true;
  for (const Item& item : items)
  {
    if (progress->stop.load())
    {
      SetError(progress, "stopped");
      complete = false;
      break;
    }
    progress->looked++;
    const std::string dst = plan.to + "/" + item.rel;
    uint64_t there_size = 0;
    int64_t there_time = 0;
    if (Stat(dst, &there_size, &there_time))
    {
      // (Reconcile: a file of the fallback folder was written after the drive went away - the same second as the
      // drive's is newer too.)
      const bool newer = item.time > there_time || (plan.copying == Copying::Reconcile && item.time == there_time);
      if (plan.policy == Policy::Missing || !newer)
      {
        if (there_size == item.size && !newer)
          progress->same++;
        else
          progress->kept++;
        progress->bytes_done += item.size;
        continue;
      }
    }
    auto refuse = [&](const std::string& why) {
      progress->refused++;
      progress->bytes_done += item.size;
      LogF("portable: copy: %s isn't copied (%s): it stays where it is", item.rel.c_str(), why.c_str());
      std::lock_guard lock(progress->mutex);
      if (progress->refused_names.size() < 20)
        progress->refused_names.push_back(item.rel);
    };
    if (plan.copying == Copying::ToDrive && !FatName(item.rel))
    {
      refuse("a name a USB drive doesn't take");
      continue;
    }
    const std::string parent = Parent(dst);
    if (made.insert(parent).second)
      MakeDirs(parent);
    std::string error;
    const Result result = CopyOne(plan, item.rel, &buffer, &error);
    if (result == Result::NameRefused)
    {
      refuse(error);
      continue;
    }
    progress->bytes_done += item.size;
    if (result == Result::Copied)
    {
      progress->copied++;
      continue;
    }
    progress->failed++;
    SetError(progress, error);
    LogF("portable: copy: %s", error.c_str());
    complete = false;
    if (result == Result::TargetFailed)
      break;  // (a full or gone drive: every next file would fail the same way)
  }
  progress->done = true;
  return complete;
}

std::string MarkDrive(const std::string& drive_root)
{
  std::string id = DriveDataId(drive_root);
  if (!id.empty())
    return id;
  id = NewId();
  const std::string text =
      "# EmuSuNi's data on this USB drive (Settings -> Keep My Data on the USB Drive): saves, states, settings, "
      "controls,\n# cheats, bezels, covers, themes, favorites and the game list. Another PS4 with EmuSuNi asks once "
      "whether to use it.\n# Your own BIOS files can go in the folder system here: any PS4 reads them from it (they "
      "are never copied by the app).\nid=" +
      id + "\nversion=" + App::VERSION + "\n";
  std::string error;
  MakeDirs(FolderOf(drive_root));
  if (!WriteWhole(FolderOf(drive_root) + "/" + MARKER, text, true, &error))
  {
    LogF("portable: %s couldn't be written: %s", MARKER, error.c_str());
    return {};
  }
  LogF("portable: %s/%s made (data %s)", FolderOf(drive_root).c_str(), MARKER, id.c_str());
  return id;
}

std::string DriveDataId(const std::string& drive_root)
{
  const std::string text = ReadTextFile(FolderOf(drive_root) + "/" + MARKER);
  size_t at = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    if (line.rfind("id=", 0) == 0)
      return CleanId(line.substr(3));
  }
  return {};
}

std::string FolderOf(const std::string& drive_root)
{
  return drive_root + "/" + FOLDER;
}
}  // namespace Portable
