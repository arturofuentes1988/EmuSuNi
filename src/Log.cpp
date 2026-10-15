// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>

#include <sys/stat.h>
#include <unistd.h>

extern "C" void ps4_boot_trace(const char* line);  // port/ps4_crashlog.cpp
extern "C" int sceKernelInternalMemoryGetAvailableSize(size_t* size);
extern "C" int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);

void LogLine(const std::string& text)
{
  fprintf(stderr, "%s\n", text.c_str());
  ps4_boot_trace(text.c_str());
}

void LogF(const char* format, ...)
{
  char line[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  LogLine(line);
}

void LogMemory(const char* where)
{
  size_t internal = 0, flexible = 0;
  sceKernelInternalMemoryGetAvailableSize(&internal);
  sceKernelAvailableFlexibleMemorySize(&flexible);
  LogF("memory (%s): internal %zu KiB free, flexible %zu MiB free", where, internal / 1024,
       flexible / (1024 * 1024));
}

std::string ReadTextFile(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool WriteTextFile(const std::string& path, const std::string& text)
{
  const std::string temp = path + ".new";
  FILE* file = fopen(temp.c_str(), "wb");
  if (!file)
    return false;
  const bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
  fflush(file);
  fsync(fileno(file));
  fclose(file);
  return written && std::rename(temp.c_str(), path.c_str()) == 0;
}

bool FileExists(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void MakeDirs(const std::string& path)
{
  for (size_t at = 1; at <= path.size(); at++)
  {
    if (at == path.size() || path[at] == '/')
      mkdir(path.substr(0, at).c_str(), 0777);
  }
}
