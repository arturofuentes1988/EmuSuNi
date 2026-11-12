// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#include "GameExtras.h"

#include <pthread.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>

#include "Log.h"

namespace GameExtras
{
namespace
{
std::string LowerText(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return text;
}

std::mutex s_ask_mutex;
AfterGame s_ask;
std::map<SystemId, std::vector<std::string>> s_folders;

// The set's files in `dir`: <set>.zip and <set>.7z, any letters' case (the .zip first).
std::vector<PatchedFile> SetFiles(const std::string& dir, const std::string& set)
{
  std::vector<PatchedFile> zips, sevens;
  std::error_code ec;
  const std::string lower = LowerText(set);
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
  {
    std::error_code file_error;
    if (!it->is_regular_file(file_error))
      continue;
    const std::string name = it->path().filename().string(), low = LowerText(name);
    if (low != lower + ".zip" && low != lower + ".7z")
      continue;
    (low == lower + ".zip" ? zips : sevens)
        .push_back({it->path().string(), name, static_cast<uint64_t>(it->file_size(file_error))});
  }
  zips.insert(zips.end(), sevens.begin(), sevens.end());
  return zips;
}
}  // namespace

void AskAfterGame(const std::string& game_path, const std::string& action)
{
  std::lock_guard lock(s_ask_mutex);
  s_ask = {game_path, action};
  LogLine("game extras: after the game: " + action);
}

bool TakeAfterGame(AfterGame* out)
{
  std::lock_guard lock(s_ask_mutex);
  if (s_ask.action.empty())
    return false;
  *out = s_ask;
  s_ask = {};
  return true;
}

void SetInstallFolders(SystemId system, const std::vector<std::string>& folders)
{
  std::lock_guard lock(s_ask_mutex);
  s_folders[system] = folders;
}

std::vector<std::string> InstallFolders(SystemId system)
{
  std::lock_guard lock(s_ask_mutex);
  const auto it = s_folders.find(system);
  return it == s_folders.end() ? std::vector<std::string>{} : it->second;
}

// ---- The Arcade's patched romsets ----

PatchedFile PatchedInUse(const std::string& dir, const std::string& set)
{
  // (FinalBurn Neo opens <set>.zip, else <set>.7z: the set's name - a drive of any case.)
  const std::vector<PatchedFile> files = SetFiles(dir, set);
  return files.empty() ? PatchedFile{} : files.front();
}

std::vector<std::string> PatchedFolders(const std::vector<std::string>& game_folders, bool make)
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
      std::error_code dir_error;
      if (LowerText(it->path().filename().string()) == "patched" && it->is_directory(dir_error))
        found = it->path().string();
    }
    if (found.empty() && make)
    {
      const std::string made = folder + "/patched";
      std::error_code make_error;
      if (std::filesystem::create_directory(made, make_error))
      {
        LogLine("arcade patched: made " + made);
        found = made;
      }
    }
    if (!found.empty() && std::find(out.begin(), out.end(), found) == out.end())
      out.push_back(found);
  }
  return out;
}

std::vector<PatchedFile> PatchedFound(const std::vector<std::string>& folders, const std::string& set)
{
  std::vector<PatchedFile> out;
  for (const std::string& folder : folders)
  {
    const std::vector<PatchedFile> files = SetFiles(folder, set);
    out.insert(out.end(), files.begin(), files.end());
  }
  return out;
}

bool RemovePatched(const std::string& dir, const std::string& set, std::string* error)
{
  bool ok = true;
  for (const PatchedFile& file : SetFiles(dir, set))  // (a .zip and a .7z both there: each)
  {
    std::error_code ec;
    if (std::filesystem::remove(file.path, ec) || !ec)
    {
      LogLine("arcade patched: deleted " + file.path);
      continue;
    }
    ok = false;
    if (error && error->empty())
      *error = ec.message();
    LogLine("arcade patched: couldn't delete " + file.path + ": " + ec.message());
  }
  return ok;
}

// ---- The copy's thread ----
namespace
{
std::mutex s_copy_mutex;
CopyProgress s_copy;  // (under s_copy_mutex, but for its bytes: the atomic below)
std::atomic<uint64_t> s_copy_bytes{0};
std::atomic<bool> s_copy_cancel{false};
std::string s_copy_dir, s_copy_set;
pthread_t s_copy_thread;
bool s_copy_thread_made = false;

void* CopyMain(void*)
{
  std::string from, to, dir, set;
  {
    std::lock_guard lock(s_copy_mutex);
    from = s_copy.from;
    to = s_copy.to;
    dir = s_copy_dir;
    set = s_copy_set;
  }
  const std::string part = to + ".part";
  std::string error;
  bool cancelled = false;
  {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    FILE* in = std::fopen(from.c_str(), "rb");
    FILE* out = in ? std::fopen(part.c_str(), "wb") : nullptr;
    if (!in)
      error = "The file couldn't be read";
    else if (!out)
      error = "The file couldn't be written";
    constexpr size_t CHUNK = 1u << 20;
    std::unique_ptr<char[]> buffer(new char[CHUNK]);
    while (error.empty())
    {
      if (s_copy_cancel)
      {
        cancelled = true;
        break;
      }
      const size_t got = std::fread(buffer.get(), 1, CHUNK, in);
      if (got > 0 && std::fwrite(buffer.get(), 1, got, out) != got)
        error = "The file couldn't be written (no room left?)";
      s_copy_bytes += got;
      if (got < CHUNK)
      {
        if (std::ferror(in))
          error = "The file couldn't be read";
        break;
      }
    }
    if (in)
      std::fclose(in);
    if (out && std::fclose(out) != 0 && error.empty() && !cancelled)
      error = "The file couldn't be written (no room left?)";
  }
  bool ok = error.empty() && !cancelled;
  if (ok)
  {
    // Whole: the set's patched romsets of before out (another extension, another letters' case: FinalBurn Neo would
    // take a .zip before a .7z), the new one in.
    for (const PatchedFile& before : SetFiles(dir, set))
    {
      std::error_code ec;
      if (before.path != to && std::filesystem::remove(before.path, ec))
        LogLine("arcade patched: deleted the one before: " + before.path);
    }
    std::error_code ec;
    std::filesystem::rename(part, to, ec);
    if (ec)
    {
      ok = false;
      error = "The file couldn't be written";
      LogLine("arcade patched: " + part + " couldn't be renamed: " + ec.message());
    }
  }
  if (!ok)
  {
    std::error_code ec;
    std::filesystem::remove(part, ec);
  }
  LogF("arcade patched: %s -> %s: %s (%" PRIu64 " bytes)", from.c_str(), to.c_str(),
       ok ? "copied" : cancelled ? "cancelled" : error.c_str(), static_cast<uint64_t>(s_copy_bytes));
  std::lock_guard lock(s_copy_mutex);
  s_copy.busy = false;
  s_copy.done = true;
  s_copy.ok = ok;
  s_copy.cancelled = cancelled;
  s_copy.error = error;
  return nullptr;
}
}  // namespace

bool StartCopy(const std::string& from, const std::string& dir, const std::string& set)
{
  std::lock_guard lock(s_copy_mutex);
  if (s_copy.busy)
    return false;
  if (s_copy_thread_made)
  {
    pthread_join(s_copy_thread, nullptr);  // (finished: busy is false)
    s_copy_thread_made = false;
  }
  const std::string ext = LowerText(std::filesystem::path(from).extension().string());
  s_copy = {};
  s_copy.from = from;
  // (FinalBurn Neo looks for its driver's name: the set's, in small letters.)
  s_copy.to = dir + "/" + LowerText(set) + (ext == ".7z" ? ".7z" : ".zip");
  std::error_code ec;
  s_copy.total = static_cast<uint64_t>(std::filesystem::file_size(from, ec));
  s_copy_dir = dir;
  s_copy_set = set;
  s_copy_bytes = 0;
  s_copy_cancel = false;
  s_copy.busy = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256u << 10);
  const int made = pthread_create(&s_copy_thread, &attr, CopyMain, nullptr);
  pthread_attr_destroy(&attr);
  if (made != 0)
  {
    s_copy.busy = false;
    s_copy.done = true;
    s_copy.error = "The copy couldn't start (no thread)";
    LogF("arcade patched: no thread for the copy (%d)", made);
    return false;
  }
  s_copy_thread_made = true;
  LogLine("arcade patched: copying " + from + " to " + s_copy.to);
  return true;
}

CopyProgress CopyStatus()
{
  std::lock_guard lock(s_copy_mutex);
  CopyProgress progress = s_copy;
  progress.bytes = s_copy_bytes;
  return progress;
}

void CancelCopy()
{
  s_copy_cancel = true;
}

void ForgetCopy()
{
  std::lock_guard lock(s_copy_mutex);
  if (s_copy.busy)
    return;
  if (s_copy_thread_made)
  {
    pthread_join(s_copy_thread, nullptr);
    s_copy_thread_made = false;
  }
  s_copy.done = false;
}

int64_t FreeBytes(const std::string& folder)
{
  struct statvfs fs;
  if (statvfs(folder.c_str(), &fs) != 0)
    return -1;
  return static_cast<int64_t>(fs.f_bavail) * static_cast<int64_t>(fs.f_frsize ? fs.f_frsize : fs.f_bsize);
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
}  // namespace GameExtras
