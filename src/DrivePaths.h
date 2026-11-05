// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (00.87) Paths kept on a USB drive that refer to files on that same drive (portable data, src/Portable.h). One drive
// can be /usb0 on one PS4 and /usb1 on another (or after it was plugged in again), so a file on it is written
// "<drive>/Roms/snes/Zelda.sfc" there, and read back with the drive's root of the moment ('<' and '>' can't be in a
// FAT or exFAT name: no real path starts so). Every other path is kept as it is. Header only: the library's cache
// (src/Library.cpp), the menu's files (src/Menu.cpp) and the copies (src/Portable.cpp) use the same rule.

#pragma once

#include <string>

namespace DrivePaths
{
inline constexpr const char TOKEN[] = "<drive>";
inline constexpr size_t TOKEN_SIZE = sizeof(TOKEN) - 1;

// The length of `root` at the start of `path` (the path is the root itself, or something in it), else npos.
inline size_t RootAt(const std::string& path, const std::string& root)
{
  if (root.empty() || path.compare(0, root.size(), root) != 0)
    return std::string::npos;
  return path.size() == root.size() || path[root.size()] == '/' ? root.size() : std::string::npos;
}

// "/usb1/Roms/x.sfc" (or "/mnt/usb1/Roms/x.sfc": the same drive outside the app's sandbox) for the drive "/usb1":
// "<drive>/Roms/x.sfc". Anything else (another drive's, the PS4's own storage, no drive given) as it is.
inline std::string Stored(const std::string& path, const std::string& drive)
{
  if (drive.empty())
    return path;
  size_t at = RootAt(path, drive);
  if (at == std::string::npos && drive.compare(0, 5, "/mnt/") != 0)
    at = RootAt(path, "/mnt" + drive);
  return at == std::string::npos ? path : std::string(TOKEN) + path.substr(at);
}

// Stored's text back to a path on the drive as it is now ("<drive>/Roms/x.sfc" -> "/usb0/Roms/x.sfc"). Anything else,
// or no drive given, as it is.
inline std::string Resolved(const std::string& stored, const std::string& drive)
{
  if (drive.empty() || RootAt(stored, TOKEN) == std::string::npos)
    return stored;
  return drive + stored.substr(TOKEN_SIZE);
}

inline bool IsStored(const std::string& text)
{
  return RootAt(text, TOKEN) != std::string::npos;
}

// A whole file's paths, for the copies: `stored` true makes them Stored, false Resolved.
//   lines_after: the part of each line after its first `separator` ("key=value": xmb.ini; "<time> <path>": played.ini).
inline std::string LinesAfter(const std::string& text, char separator, const std::string& drive, bool stored)
{
  std::string out;
  out.reserve(text.size() + 64);
  size_t start = 0;
  while (start < text.size())
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    const std::string line = text.substr(start, end - start);
    const size_t sep = line.find(separator);
    if (line.empty() || line[0] == '#' || sep == std::string::npos)
      out += line;
    else
    {
      std::string value = line.substr(sep + 1), tail;
      if (!value.empty() && value.back() == '\r')
      {
        value.pop_back();
        tail = "\r";
      }
      out += line.substr(0, sep + 1) + (stored ? Stored(value, drive) : Resolved(value, drive)) + tail;
    }
    if (end < text.size())
      out.push_back('\n');
    start = end + 1;
  }
  return out;
}

// The library's cache (src/Library.cpp's library.tsv): every tab-separated field and every item of a list field
// (0x1F before each), escaped as the file has them - a path's start ("/usb1/") has nothing escaped. Its "C" lines (the
// folders the menu asked for) as they are: the menu writes and compares them in its own form (Menu.cpp's StoredChosen:
// a console's default folder "/usb0/Roms/n64" is whichever drive is usb0, never made relative).
inline std::string TsvFields(const std::string& text, const std::string& drive, bool stored)
{
  std::string out;
  out.reserve(text.size() + 1024);
  size_t start = 0;
  bool line_start = true, as_is = false;
  while (start <= text.size())
  {
    if (line_start)
      as_is = text.compare(start, 2, "C\t") == 0;
    size_t end = start;
    while (end < text.size() && text[end] != '\t' && text[end] != '\x1f' && text[end] != '\n')
      end++;
    const std::string field = text.substr(start, end - start);
    out += as_is ? field : stored ? Stored(field, drive) : Resolved(field, drive);
    if (end >= text.size())
      break;
    out.push_back(text[end]);
    line_start = text[end] == '\n';
    start = end + 1;
  }
  return out;
}
}  // namespace DrivePaths
