// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // (fopencookie)
#endif

#include "AssetPack.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include "Log.h"

namespace AssetPack
{
namespace
{
struct Entry
{
  uint32_t header = 0;  // where its local header is
  uint32_t size = 0;
};

std::once_flag s_once;
std::string s_prefix, s_zip;  // "<assets>/xmb/", its assets.zip
std::unordered_map<std::string, Entry> s_entries;  // by its name in the zip ("bezels/tv.png")

uint16_t U16(const unsigned char* p)
{
  return static_cast<uint16_t>(p[0] | p[1] << 8);
}

uint32_t U32(const unsigned char* p)
{
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

bool ReadAt(FILE* file, long at, void* out, size_t size)
{
  return std::fseek(file, at, SEEK_SET) == 0 && std::fread(out, 1, size, file) == size;
}

// The zip's central directory: its stored entries (a compressed one isn't read here: left out, logged).
void ReadIndex()
{
  s_prefix = std::string(App::ASSETS_DIR) + "/xmb/";
  s_zip = s_prefix + "assets.zip";
  FILE* file = std::fopen(s_zip.c_str(), "rb");
  if (!file)
    return;  // (the PC, a package without it: the loose files)
  long size = -1;
  if (std::fseek(file, 0, SEEK_END) == 0)
    size = std::ftell(file);
  // The end of the central directory: its last 22 bytes and a comment (none in ours; up to 64 KiB looked at).
  const long tail = std::min<long>(size, 22 + 65535);
  std::vector<unsigned char> end(tail > 0 ? static_cast<size_t>(tail) : 0);
  long found = -1;
  if (tail >= 22 && ReadAt(file, size - tail, end.data(), end.size()))
  {
    for (long i = tail - 22; i >= 0 && found < 0; i--)
    {
      if (U32(&end[static_cast<size_t>(i)]) == 0x06054b50)
        found = i;
    }
  }
  if (found < 0)
  {
    LogF("assets: %s isn't a zip that can be read: the loose files only", s_zip.c_str());
    std::fclose(file);
    return;
  }
  const unsigned char* eocd = &end[static_cast<size_t>(found)];
  const uint32_t count = U16(eocd + 10), dir_size = U32(eocd + 12), dir_at = U32(eocd + 16);
  std::vector<unsigned char> dir(dir_size);
  if (static_cast<long>(dir_at) + static_cast<long>(dir_size) > size || !ReadAt(file, dir_at, dir.data(), dir.size()))
  {
    LogF("assets: %s's directory can't be read: the loose files only", s_zip.c_str());
    std::fclose(file);
    return;
  }
  std::fclose(file);
  size_t at = 0, compressed = 0;
  for (uint32_t i = 0; i < count && at + 46 <= dir.size() && U32(&dir[at]) == 0x02014b50; i++)
  {
    const unsigned char* p = &dir[at];
    const uint16_t method = U16(p + 10), name_size = U16(p + 28), extra = U16(p + 30), comment = U16(p + 32);
    const uint32_t packed = U32(p + 20), whole = U32(p + 24), header = U32(p + 42);
    if (at + 46 + name_size > dir.size())
      break;
    const std::string name(reinterpret_cast<const char*>(p + 46), name_size);
    if (method == 0 && packed == whole)
      s_entries[name] = {header, whole};
    else
      compressed++;
    at += 46u + name_size + extra + comment;
  }
  LogF("assets: %zu files in %s%s", s_entries.size(), s_zip.c_str(),
       compressed > 0 ? (" (" + std::to_string(compressed) + " compressed ones left out)").c_str() : "");
}

// The path's entry in the zip, if it is one of its folder's.
const Entry* Find(const std::string& path)
{
  std::call_once(s_once, ReadIndex);
  if (s_entries.empty() || path.compare(0, s_prefix.size(), s_prefix) != 0)
    return nullptr;
  const auto it = s_entries.find(path.substr(s_prefix.size()));
  return it == s_entries.end() ? nullptr : &it->second;
}

bool ReadEntry(const Entry& entry, std::string* data)
{
  FILE* file = std::fopen(s_zip.c_str(), "rb");
  if (!file)
    return false;
  unsigned char local[30];
  bool read = ReadAt(file, entry.header, local, sizeof(local)) && U32(local) == 0x04034b50;
  if (read)
  {
    data->resize(entry.size);
    const long start = static_cast<long>(entry.header) + 30 + U16(local + 26) + U16(local + 28);
    read = entry.size == 0 || ReadAt(file, start, &(*data)[0], entry.size);
  }
  std::fclose(file);
  return read;
}

bool Loose(const std::string& path)
{
  struct stat st = {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// An entry's bytes as a FILE (Open): read, seek, close.
struct Stream
{
  std::string data;
  size_t at = 0;
};

ssize_t StreamRead(void* cookie, char* out, size_t size)
{
  Stream* s = static_cast<Stream*>(cookie);
  const size_t n = std::min(size, s->data.size() - s->at);
  std::memcpy(out, s->data.data() + s->at, n);
  s->at += n;
  return static_cast<ssize_t>(n);
}

int StreamSeek(void* cookie, off_t* offset, int whence)
{
  Stream* s = static_cast<Stream*>(cookie);
  const off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<off_t>(s->at) :
                                                                     static_cast<off_t>(s->data.size());
  const off_t to = base + *offset;
  if (to < 0 || to > static_cast<off_t>(s->data.size()))
    return -1;
  s->at = static_cast<size_t>(to);
  *offset = to;
  return 0;
}

int StreamClose(void* cookie)
{
  delete static_cast<Stream*>(cookie);
  return 0;
}
}  // namespace

bool Exists(const std::string& path)
{
  return Loose(path) || Find(path) != nullptr;
}

bool Read(const std::string& path, std::string* data)
{
  data->clear();
  if (FILE* file = std::fopen(path.c_str(), "rb"))
  {
    char buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
      data->append(buffer, n);
    std::fclose(file);
    return true;
  }
  const Entry* entry = Find(path);
  return entry && ReadEntry(*entry, data);
}

FILE* Open(const std::string& path)
{
  if (FILE* file = std::fopen(path.c_str(), "rb"))
    return file;
  const int loose_error = errno;
  const Entry* entry = Find(path);
  if (!entry)
  {
    errno = loose_error;
    return nullptr;
  }
  Stream* stream = new Stream;
  if (!ReadEntry(*entry, &stream->data))
  {
    delete stream;
    errno = EIO;
    return nullptr;
  }
  cookie_io_functions_t io = {};
  io.read = StreamRead;
  io.seek = StreamSeek;
  io.close = StreamClose;
  FILE* file = fopencookie(stream, "rb", io);
  if (!file)
    delete stream;
  return file;
}
}  // namespace AssetPack
