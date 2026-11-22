// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#include "ThreeDsTextures.h"

#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>

#include <png.h>

#include "Log.h"
#include "Settings.h"

#ifdef __ORBIS__
// The system's free areas of direct memory (the GPU's: Mesa takes each buffer from it) - names of our own, as
// StartGuard.cpp's affinity call (OpenOrbis' libkernel.h declares the first with plain numbers for its pointers).
extern "C" int32_t emusuni_tex_direct_available(off_t start, off_t end, size_t align, off_t* phys, size_t* size)
    __asm__("sceKernelAvailableDirectMemorySize");
extern "C" size_t emusuni_tex_direct_size(void) __asm__("sceKernelGetDirectMemorySize");
#endif

namespace ThreeDsTextures
{
namespace
{
namespace fs = std::filesystem;

constexpr uint32_t CORE_SIDE = 2048;     // the core leaves a bigger picture out
constexpr uint32_t TIMES = 4;            // a picture at most 4 times its texture's size, each side
constexpr uint32_t OTHER_SIDE = 1024;    // a picture with another name: at most this a side (the PC script's)
constexpr uint64_t SPARE_BYTES = 64ull << 20;
constexpr uint64_t JSON_MAX = 4ull << 20;
constexpr size_t WHOLE_MAX = size_t{64} << 20;  // an interlaced picture is read whole: past this, copied as it is
constexpr int ROWS_PER_CHECK = 64;              // a cancel looked at every this many rows of a picture made smaller

std::string Hex(uint64_t title)
{
  char text[24];
  std::snprintf(text, sizeof(text), "%016" PRIX64, title);
  return text;
}

std::string Lower(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool IsPngName(const std::string& lower)
{
  return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0;
}

uint32_t Be32(const unsigned char* p)
{
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
         p[3];
}

// A PNG's size from its first 24 bytes: its signature and its header (false: not a PNG).
bool PngSize(const std::string& path, uint32_t* w, uint32_t* h)
{
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;
  unsigned char head[24];
  const bool read = std::fread(head, 1, sizeof(head), file) == sizeof(head);
  std::fclose(file);
  static const unsigned char SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (!read || std::memcmp(head, SIGNATURE, 8) != 0 || std::memcmp(head + 12, "IHDR", 4) != 0)
    return false;
  *w = Be32(head + 16);
  *h = Be32(head + 20);
  return *w > 0 && *h > 0;
}

// The size of the texture a picture replaces, from its name (tex1_<W>x<H>_...); false: another name.
bool TextureSize(const std::string& lower, uint32_t* w, uint32_t* h)
{
  if (lower.compare(0, 5, "tex1_") != 0)
    return false;
  const char* p = lower.c_str() + 5;
  char* end = nullptr;
  const unsigned long tw = std::strtoul(p, &end, 10);
  if (end == p || *end != 'x')
    return false;
  p = end + 1;
  const unsigned long th = std::strtoul(p, &end, 10);
  if (end == p || *end != '_' || tw == 0 || th == 0 || tw > 4096 || th > 4096)
    return false;
  *w = static_cast<uint32_t>(tw);
  *h = static_cast<uint32_t>(th);
  return true;
}

// By halves while it is bigger than max_w x max_h and both sides can be halved exactly.
uint32_t Factor(uint32_t w, uint32_t h, uint32_t max_w, uint32_t max_h)
{
  uint32_t f = 1;
  while ((w / f > max_w || h / f > max_h) && f < (1u << 12) && w % (f * 2) == 0 && h % (f * 2) == 0)
    f *= 2;
  return f;
}

// A folder and all it holds deleted (unlink, rmdir: the C++ library's remove_all deletes nothing on the PS4, and its
// remove no folder). depth: the levels of folders it goes into.
void RemoveTree(const std::string& path, int depth, int* removed, int* failed)
{
  std::error_code ec;
  std::vector<std::string> files, folders;
  for (auto it = fs::directory_iterator(path, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
  {
    std::error_code type_error;
    if (fs::is_directory(it->symlink_status(type_error)))
      folders.push_back(it->path().string());
    else
      files.push_back(it->path().string());
  }
  for (const std::string& folder : folders)
  {
    if (depth > 0)
      RemoveTree(folder, depth - 1, removed, failed);
    else
      (*failed)++;
  }
  for (const std::string& file : files)
    (::unlink(file.c_str()) == 0 ? *removed : *failed)++;
  (::rmdir(path.c_str()) == 0 ? *removed : *failed)++;
}

bool IsFolder(const std::string& path)
{
  std::error_code ec;
  return fs::is_directory(path, ec);
}

int64_t FreeBytes(const std::string& folder)
{
  struct statvfs info;
  if (statvfs(folder.c_str(), &info) != 0)
    return -1;
  return static_cast<int64_t>(info.f_bavail) * static_cast<int64_t>(info.f_frsize ? info.f_frsize : info.f_bsize);
}

pthread_t StartThread(void* (*main)(void*), bool* made)
{
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 2u << 20);  // (the default's size: libthr keeps a stack for the next thread)
  pthread_t thread{};
  *made = pthread_create(&thread, &attr, main, nullptr) == 0;
  pthread_attr_destroy(&attr);
  return thread;
}

// ---- The counts ----
std::mutex s_count_mutex;
std::deque<std::string> s_count_queue;
std::map<std::string, Count> s_counted;
std::string s_counting;
pthread_t s_count_thread{};
bool s_count_running = false, s_count_made = false;

void* CountMain(void*)
{
  for (;;)
  {
    std::string folder;
    {
      std::lock_guard lock(s_count_mutex);
      if (s_count_queue.empty())
      {
        s_count_running = false;
        return nullptr;
      }
      folder = s_count_queue.front();
      s_count_queue.pop_front();
      s_counting = folder;
    }
    const auto start = std::chrono::steady_clock::now();
    Count count;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
      std::error_code type_error;
      if (!it->is_regular_file(type_error) || !IsPngName(Lower(it->path().filename().string())))
        continue;
      count.pngs++;
      const uintmax_t size = it->file_size(type_error);
      count.bytes += type_error ? 0 : static_cast<uint64_t>(size);
    }
    LogF("3ds textures: %s: %d pictures, %s (counted in %.0f ms)", folder.c_str(), count.pngs,
         SizeText(count.bytes).c_str(),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    std::lock_guard lock(s_count_mutex);
    s_counted[folder] = count;
    s_counting.clear();
  }
}

// ---- The install ----
std::mutex s_mutex;
Progress s_progress;  // (under s_mutex, but for the atomics below)
std::atomic<int> s_files_done{0};
std::atomic<uint64_t> s_bytes_done{0};
std::atomic<bool> s_cancel{false};
std::string s_from, s_to;
pthread_t s_thread{};
bool s_thread_made = false;
std::chrono::steady_clock::time_point s_started;

struct PngJump
{
  char message[160] = {};
};

void OnPngError(png_structp png, png_const_charp message)
{
  PngJump* jump = static_cast<PngJump*>(png_get_error_ptr(png));
  std::snprintf(jump->message, sizeof(jump->message), "%s", message ? message : "");
  png_longjmp(png, 1);
}

void OnPngWarning(png_structp, png_const_charp)
{
}

// The buffers of a picture made smaller (outside MakeSmaller: libpng leaves it by longjmp).
struct Smaller
{
  std::vector<uint8_t> row, whole, out;
  std::vector<uint64_t> sums;  // each new pixel's: alpha, colour x alpha (3), colour (3)
  PngJump read_jump, write_jump;
};

// `from` written to `to` with each side divided by f (both exact): each new pixel the average of the f x f it
// replaces, its colour weighted by alpha (a transparent pixel's colour doesn't bleed into its neighbours). nullptr:
// written; "cancelled"; "interlaced" (too big to read whole: the caller copies it as it is); else why not.
const char* MakeSmaller(FILE* in, FILE* out, uint32_t f, Smaller* s)
{
  s->read_jump.message[0] = s->write_jump.message[0] = '\0';
  png_structp rpng = png_create_read_struct(PNG_LIBPNG_VER_STRING, &s->read_jump, OnPngError, OnPngWarning);
  png_infop rinfo = rpng ? png_create_info_struct(rpng) : nullptr;
  png_structp wpng = png_create_write_struct(PNG_LIBPNG_VER_STRING, &s->write_jump, OnPngError, OnPngWarning);
  png_infop winfo = wpng ? png_create_info_struct(wpng) : nullptr;
  if (!rpng || !rinfo || !wpng || !winfo)
  {
    png_destroy_read_struct(&rpng, &rinfo, nullptr);
    png_destroy_write_struct(&wpng, &winfo);
    return "no memory for libpng";
  }
  if (setjmp(png_jmpbuf(rpng)))
  {
    png_destroy_read_struct(&rpng, &rinfo, nullptr);
    png_destroy_write_struct(&wpng, &winfo);
    return s->read_jump.message[0] ? s->read_jump.message : "a damaged picture";
  }
  if (setjmp(png_jmpbuf(wpng)))
  {
    png_destroy_read_struct(&rpng, &rinfo, nullptr);
    png_destroy_write_struct(&wpng, &winfo);
    return s->write_jump.message[0] ? s->write_jump.message : "couldn't be written";
  }
  png_init_io(rpng, in);
  png_read_info(rpng, rinfo);
  const png_byte color = png_get_color_type(rpng, rinfo);
  const png_byte depth = png_get_bit_depth(rpng, rinfo);
  const bool alpha = (color & PNG_COLOR_MASK_ALPHA) != 0 || png_get_valid(rpng, rinfo, PNG_INFO_tRNS);
  if (depth == 16)
    png_set_strip_16(rpng);
  if (color == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(rpng);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
    png_set_expand_gray_1_2_4_to_8(rpng);
  if (png_get_valid(rpng, rinfo, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(rpng);
  if (color == PNG_COLOR_TYPE_RGB || color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(rpng, 0xFF, PNG_FILLER_AFTER);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(rpng);
  const int passes = png_set_interlace_handling(rpng);
  png_read_update_info(rpng, rinfo);
  const uint32_t w = png_get_image_width(rpng, rinfo), h = png_get_image_height(rpng, rinfo);
  const size_t stride = static_cast<size_t>(w) * 4;
  const char* refused = nullptr;
  if (png_get_rowbytes(rpng, rinfo) != stride || w % f != 0 || h % f != 0)
    refused = "a damaged picture";
  else if (passes > 1 && stride * h > WHOLE_MAX)
    refused = "interlaced";
  if (refused)
  {
    png_destroy_read_struct(&rpng, &rinfo, nullptr);
    png_destroy_write_struct(&wpng, &winfo);
    return refused;
  }
  const uint32_t ow = w / f, oh = h / f, channels = alpha ? 4 : 3, n = f * f;
  s->row.resize(stride);
  s->out.resize(static_cast<size_t>(ow) * channels);
  s->sums.resize(static_cast<size_t>(ow) * 7);
  if (passes > 1)
  {
    s->whole.resize(stride * h);
    for (int pass = 0; pass < passes; pass++)
    {
      for (uint32_t y = 0; y < h; y++)
        png_read_row(rpng, s->whole.data() + y * stride, nullptr);
    }
  }
  png_init_io(wpng, out);
  png_set_IHDR(wpng, winfo, ow, oh, 8, alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_set_compression_level(wpng, 3);  // (the console's CPU: a little bigger, much quicker than zlib's 6)
  png_write_info(wpng, winfo);
  for (uint32_t oy = 0; oy < oh; oy++)
  {
    if (oy % ROWS_PER_CHECK == 0 && s_cancel.load(std::memory_order_relaxed))
    {
      png_destroy_read_struct(&rpng, &rinfo, nullptr);
      png_destroy_write_struct(&wpng, &winfo);
      return "cancelled";
    }
    std::fill(s->sums.begin(), s->sums.end(), 0);
    for (uint32_t k = 0; k < f; k++)
    {
      const uint8_t* src;
      if (passes > 1)
      {
        src = s->whole.data() + (static_cast<size_t>(oy) * f + k) * stride;
      }
      else
      {
        png_read_row(rpng, s->row.data(), nullptr);
        src = s->row.data();
      }
      for (uint32_t ox = 0; ox < ow; ox++)
      {
        uint64_t* sum = &s->sums[static_cast<size_t>(ox) * 7];
        const uint8_t* px = src + static_cast<size_t>(ox) * f * 4;
        for (uint32_t j = 0; j < f; j++, px += 4)
        {
          const uint64_t a = px[3];
          sum[0] += a;
          sum[1] += px[0] * a;
          sum[2] += px[1] * a;
          sum[3] += px[2] * a;
          sum[4] += px[0];
          sum[5] += px[1];
          sum[6] += px[2];
        }
      }
    }
    for (uint32_t ox = 0; ox < ow; ox++)
    {
      const uint64_t* sum = &s->sums[static_cast<size_t>(ox) * 7];
      uint8_t* to = s->out.data() + static_cast<size_t>(ox) * channels;
      for (int c = 0; c < 3; c++)
        to[c] = static_cast<uint8_t>(sum[0] > 0 ? (sum[1 + c] + sum[0] / 2) / sum[0] : (sum[4 + c] + n / 2) / n);
      if (alpha)
        to[3] = static_cast<uint8_t>((sum[0] + n / 2) / n);
    }
    png_write_row(wpng, s->out.data());
  }
  png_write_end(wpng, winfo);
  // (Not png_read_end: what follows the pixels isn't needed.)
  png_destroy_read_struct(&rpng, &rinfo, nullptr);
  png_destroy_write_struct(&wpng, &winfo);
  return nullptr;
}

// A file copied as it is, in 1 MiB pieces (its bytes counted as they go). nullptr: copied; "cancelled"; else why not.
const char* CopyAsItIs(const std::string& from, const std::string& to, std::vector<uint8_t>* buffer)
{
  FILE* in = std::fopen(from.c_str(), "rb");
  if (!in)
    return "couldn't be read";
  FILE* out = std::fopen(to.c_str(), "wb");
  if (!out)
  {
    std::fclose(in);
    return "couldn't be written";
  }
  buffer->resize(1u << 20);
  const char* why = nullptr;
  for (;;)
  {
    if (s_cancel.load(std::memory_order_relaxed))
    {
      why = "cancelled";
      break;
    }
    const size_t read = std::fread(buffer->data(), 1, buffer->size(), in);
    if (read > 0 && std::fwrite(buffer->data(), 1, read, out) != read)
    {
      why = "couldn't be written";
      break;
    }
    s_bytes_done.fetch_add(read, std::memory_order_relaxed);
    if (read < buffer->size())
    {
      if (std::ferror(in))
        why = "couldn't be read";
      break;
    }
  }
  std::fclose(in);
  if (std::fclose(out) != 0 && !why)
    why = "couldn't be written";
  return why;
}

uint64_t SizeOf(const std::string& path)
{
  struct stat info;
  return stat(path.c_str(), &info) == 0 ? static_cast<uint64_t>(info.st_size) : 0;
}

void Finish(int result, const std::string& why)
{
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_started).count();
  std::lock_guard lock(s_mutex);
  s_progress.busy = false;
  s_progress.scanning = false;
  s_progress.done = true;
  s_progress.result = result;
  s_progress.why = why;
  s_progress.seconds = seconds;
  s_progress.files_done = s_files_done.load();
  s_progress.bytes_done = s_bytes_done.load();
}

void* InstallMain(void*)
{
  std::string from, to;
  {
    std::lock_guard lock(s_mutex);
    from = s_from;
    to = s_to;
  }
  // The pack's files: its pictures and pack.json, by their place in it.
  struct Item
  {
    std::string rel;
    uint64_t size = 0;
    bool json = false;
  };
  std::vector<Item> items;
  int left_out = 0, pngs = 0;
  uint64_t bytes = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
  {
    if (s_cancel.load())
    {
      Finish(CANCELLED, "");
      return nullptr;
    }
    std::error_code type_error;
    const fs::file_status status = it->symlink_status(type_error);
    if (fs::is_directory(status))
      continue;
    const std::string path = it->path().string();
    const std::string lower = Lower(it->path().filename().string());
    const uint64_t size = fs::is_regular_file(status) ? static_cast<uint64_t>(it->file_size(type_error)) : 0;
    const bool json = lower == "pack.json" && size <= JSON_MAX;
    if (!fs::is_regular_file(status) || (!json && !IsPngName(lower)) || path.size() <= from.size() + 1)
    {
      left_out++;
      continue;
    }
    items.push_back({path.substr(from.size() + 1), size, json});
    pngs += json ? 0 : 1;
    bytes += size;
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rel < b.rel; });
  {
    std::lock_guard lock(s_mutex);
    s_progress.scanning = false;
    s_progress.files = static_cast<int>(items.size());
    s_progress.bytes = bytes;
    s_progress.left_out = left_out;
  }
  LogF("3ds textures: installing %s into %s: %d pictures and %zu other files to copy, %s; %d files left out",
       from.c_str(), to.c_str(), pngs, items.size() - static_cast<size_t>(pngs), SizeText(bytes).c_str(), left_out);
  if (pngs == 0)
  {
    Finish(NO_PICTURES, "");
    return nullptr;
  }
  const std::string parent = fs::path(to).parent_path().string();
  MakeDirs(parent);
  const int64_t free = FreeBytes(parent);
  if (free >= 0 && static_cast<uint64_t>(free) < bytes + SPARE_BYTES)
  {
    Finish(NO_ROOM, SizeText(static_cast<uint64_t>(free)) + " free, " + SizeText(bytes + SPARE_BYTES) + " needed");
    return nullptr;
  }
  // Into a folder of its own first: the installed one is replaced only by a whole copy.
  const std::string temp = to + ".installing";
  int removed = 0, failed = 0;
  if (IsFolder(temp))
    RemoveTree(temp, 16, &removed, &failed);
  MakeDirs(temp);
  Smaller smaller;
  std::vector<uint8_t> buffer;
  int reduced = 0, too_big = 0, damaged = 0, copied = 0;
  uint64_t written = 0;
  std::string made_dir;
  const char* why = nullptr;
  std::string failed_file;
  for (const Item& item : items)
  {
    const std::string source = from + "/" + item.rel, target = temp + "/" + item.rel;
    const std::string dir = fs::path(target).parent_path().string();
    if (dir != made_dir)
    {
      MakeDirs(dir);
      made_dir = dir;
    }
    const uint64_t before = s_bytes_done.load();
    uint32_t w = 0, h = 0;
    if (item.json)
    {
      why = CopyAsItIs(source, target, &buffer);
    }
    else if (!PngSize(source, &w, &h))
    {
      left_out++;  // (named .png, but not a PNG)
    }
    else
    {
      uint32_t tw = 0, th = 0;
      const bool named = TextureSize(Lower(fs::path(item.rel).filename().string()), &tw, &th);
      const uint32_t max_w = std::min(named ? tw * TIMES : OTHER_SIDE, CORE_SIDE);
      const uint32_t max_h = std::min(named ? th * TIMES : OTHER_SIDE, CORE_SIDE);
      const uint32_t f = Factor(w, h, max_w, max_h);
      bool as_it_is = f == 1;
      if (!as_it_is)
      {
        FILE* in = std::fopen(source.c_str(), "rb");
        FILE* out = in ? std::fopen(target.c_str(), "wb") : nullptr;
        why = !in ? "couldn't be read" : !out ? "couldn't be written" : MakeSmaller(in, out, f, &smaller);
        if (in)
          std::fclose(in);
        if (out && std::fclose(out) != 0 && !why)
          why = "couldn't be written";
        if (!why)
        {
          reduced++;
          w /= f;
          h /= f;
        }
        else if (std::strcmp(why, "interlaced") == 0)
        {
          as_it_is = true;
          why = nullptr;
        }
        else if (std::strcmp(why, "cancelled") != 0 && std::strcmp(why, "couldn't be written") != 0 &&
                 std::strcmp(why, "couldn't be read") != 0)
        {
          // A damaged picture: left out (the core would leave it out too).
          if (damaged++ < 5)
            LogF("3ds textures: %s left out: %s", item.rel.c_str(), why);
          ::unlink(target.c_str());
          left_out++;
          why = nullptr;
          w = h = 0;
        }
        if (!why)
          s_bytes_done.store(before + item.size);
      }
      if (as_it_is)
        why = CopyAsItIs(source, target, &buffer);
      if (!why && (w > CORE_SIDE || h > CORE_SIDE))
        too_big++;
    }
    if (why)
    {
      failed_file = item.rel;
      break;
    }
    s_bytes_done.store(before + item.size);
    s_files_done.fetch_add(1);
    const uint64_t target_size = SizeOf(target);
    copied += target_size > 0 ? 1 : 0;
    written += target_size;
    std::lock_guard lock(s_mutex);
    s_progress.reduced = reduced;
    s_progress.too_big = too_big;
    s_progress.left_out = left_out;
    s_progress.written = written;
    s_progress.copied = copied;
  }
  if (why)
  {
    removed = failed = 0;
    RemoveTree(temp, 16, &removed, &failed);
    const bool cancelled = std::strcmp(why, "cancelled") == 0;
    LogF("3ds textures: the install %s%s%s; its copy deleted (%d files and folders%s)",
         cancelled ? "cancelled" : "failed: ", cancelled ? "" : failed_file.c_str(),
         cancelled ? "" : (std::string(" ") + why).c_str(), removed,
         failed > 0 ? (", " + std::to_string(failed) + " couldn't be").c_str() : "");
    Finish(cancelled ? CANCELLED : FAILED, cancelled ? std::string() : failed_file + ": " + why);
    return nullptr;
  }
  // The copy is whole: it takes the installed one's place.
  if (IsFolder(to))
  {
    removed = failed = 0;
    RemoveTree(to, 16, &removed, &failed);
    if (failed > 0)
      LogF("3ds textures: the pack installed before: %d files and folders couldn't be deleted", failed);
  }
  if (std::rename(temp.c_str(), to.c_str()) != 0)
  {
    const std::string reason = std::strerror(errno);
    removed = failed = 0;
    RemoveTree(temp, 16, &removed, &failed);
    LogF("3ds textures: the copy couldn't take its place (%s): deleted", reason.c_str());
    Finish(FAILED, "the folder couldn't be renamed: " + reason);
    return nullptr;
  }
  {
    std::lock_guard lock(s_mutex);
    s_progress.reduced = reduced;
    s_progress.too_big = too_big;
    s_progress.left_out = left_out;
    s_progress.written = written;
    s_progress.copied = copied;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_started).count();
  LogF("3ds textures: installed in %.1f s: %d files (%d pictures made smaller, %d over %u a side the core leaves out), "
       "%d files left out; %s read, %s written",
       seconds, copied, reduced, too_big, CORE_SIDE, left_out, SizeText(bytes).c_str(), SizeText(written).c_str());
  Finish(DONE, "");
  return nullptr;
}
}  // namespace

std::string InstalledFolder(const std::string& save_dir, uint64_t title)
{
  return save_dir + "/Azahar/load/textures/" + Hex(title);
}

std::string PackFolder(const std::vector<std::string>& game_folders, uint64_t title)
{
  const std::string id = Hex(title);
  for (const std::string& folder : game_folders)
  {
    for (const std::string& name : {id, Lower(id)})
    {
      const std::string path = folder + "/textures/" + name;
      if (IsFolder(path))
        return path;
    }
  }
  return {};
}

bool HasPng(const std::string& folder)
{
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
  {
    std::error_code type_error;
    if (it->is_regular_file(type_error) && IsPngName(Lower(it->path().filename().string())))
      return true;
  }
  return false;
}

bool On(uint64_t title)
{
  std::stringstream in(Settings::Get("3ds_textures_on"));
  const std::string id = Hex(title);
  for (std::string item; std::getline(in, item, ',');)
  {
    if (item == id)
      return true;
  }
  return false;
}

void SetOn(uint64_t title, bool on)
{
  const std::string id = Hex(title);
  std::stringstream in(Settings::Get("3ds_textures_on"));
  std::string list;
  for (std::string item; std::getline(in, item, ',');)
  {
    if (!item.empty() && item != id)
      list += (list.empty() ? "" : ",") + item;
  }
  if (on)
    list += (list.empty() ? "" : ",") + id;
  Settings::Set("3ds_textures_on", list);  // (saved by it)
  LogF("3ds textures: the texture pack %s for %s", on ? "on" : "off", id.c_str());
}

int GpuFreeMiB()
{
#ifdef __ORBIS__
  // Each call gives a free area at or after `start`: added up, area after area (at worst some are missed: less said
  // than there is, never more).
  const off_t end = static_cast<off_t>(emusuni_tex_direct_size());
  uint64_t free = 0;
  off_t start = 0;
  for (int i = 0; i < 1024 && start < end; i++)
  {
    off_t phys = 0;
    size_t size = 0;
    if (emusuni_tex_direct_available(start, end, 64 * 1024, &phys, &size) != 0 || size == 0 ||
        phys + static_cast<off_t>(size) <= start)
      break;
    free += size;
    start = phys + static_cast<off_t>(size);
  }
  return static_cast<int>(free >> 20);
#else
  return -1;
#endif
}

void RequestCount(const std::string& folder)
{
  std::lock_guard lock(s_count_mutex);
  if (s_counted.count(folder) || s_counting == folder ||
      std::find(s_count_queue.begin(), s_count_queue.end(), folder) != s_count_queue.end())
    return;
  s_count_queue.push_back(folder);
  if (s_count_running)
    return;
  if (s_count_made)
    pthread_join(s_count_thread, nullptr);  // (it has ended: not running)
  s_count_thread = StartThread(CountMain, &s_count_made);
  s_count_running = s_count_made;
  if (!s_count_made)
  {
    s_count_queue.clear();
    LogLine("3ds textures: no thread to count the pictures");
  }
}

bool Counted(const std::string& folder, Count* count)
{
  std::lock_guard lock(s_count_mutex);
  const auto it = s_counted.find(folder);
  if (it == s_counted.end())
    return false;
  *count = it->second;
  return true;
}

void ForgetCount(const std::string& folder)
{
  std::lock_guard lock(s_count_mutex);
  s_counted.erase(folder);
}

bool Install(const std::string& from, const std::string& to)
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
  s_from = from;
  s_to = to;
  while (s_from.size() > 1 && s_from.back() == '/')
    s_from.pop_back();
  s_files_done = 0;
  s_bytes_done = 0;
  s_cancel = false;
  s_started = std::chrono::steady_clock::now();
  s_progress.busy = true;
  s_progress.scanning = true;
  s_thread = StartThread(InstallMain, &s_thread_made);
  if (!s_thread_made)
  {
    s_progress = {};
    s_progress.done = true;
    s_progress.result = NO_THREAD;
    LogLine("3ds textures: no thread for the install");
    return false;
  }
  return true;
}

Progress Status()
{
  std::lock_guard lock(s_mutex);
  Progress progress = s_progress;
  if (progress.busy)
  {
    progress.files_done = s_files_done.load();
    progress.bytes_done = s_bytes_done.load();
    progress.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_started).count();
  }
  return progress;
}

void Cancel()
{
  s_cancel = true;
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

bool Remove(const std::string& folder, std::string* why)
{
  // Only an installed pack's folder: .../Azahar/load/textures/<16 hex digits>.
  const size_t at = folder.rfind("/Azahar/load/textures/");
  const std::string id = at == std::string::npos ? std::string() : folder.substr(at + 22);
  const auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
  if (id.size() != 16 || !std::all_of(id.begin(), id.end(), hex))
  {
    *why = "not a texture pack's folder";
    return false;
  }
  if (!IsFolder(folder))
    return true;
  int removed = 0, failed = 0;
  RemoveTree(folder, 16, &removed, &failed);
  LogF("3ds textures: %s removed: %d files and folders deleted%s", folder.c_str(), removed,
       failed > 0 ? (", " + std::to_string(failed) + " couldn't be").c_str() : "");
  ForgetCount(folder);
  if (failed > 0 || IsFolder(folder))
  {
    *why = std::to_string(failed) + " files couldn't be deleted";
    return false;
  }
  return true;
}

std::string SizeText(uint64_t bytes)
{
  char text[32];
  if (bytes >= (1ull << 30))
    std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
  else if (bytes >= (1ull << 20))
    std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
  else
    std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / (1ull << 10));
  return text;
}
}  // namespace ThreeDsTextures
