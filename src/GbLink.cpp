// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GbLink.h"

#include <cstdio>
#include <cstring>

#include "Log.h"
#include "Saves.h"

namespace GbLink
{
namespace
{
bool s_active = false;
Match s_match;

// A whole file (false: it isn't there or can't be read).
bool ReadWhole(const std::string& path, std::vector<uint8_t>* data, size_t max)
{
  data->clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  uint8_t buf[16384];
  size_t got;
  bool ok = true;
  while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
  {
    if (data->size() + got > max)
    {
      ok = false;
      break;
    }
    data->insert(data->end(), buf, buf + got);
  }
  std::fclose(f);
  return ok;
}

uint32_t Get32(const uint8_t* p)
{
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}
void Put32(uint8_t* p, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint64_t Get64(const uint8_t* p)
{
  return static_cast<uint64_t>(Get32(p)) | static_cast<uint64_t>(Get32(p + 4)) << 32;
}
void Put64(uint8_t* p, uint64_t v)
{
  Put32(p, static_cast<uint32_t>(v));
  Put32(p + 4, static_cast<uint32_t>(v >> 32));
}

constexpr uint8_t SAVE_VERSION = 1;
}  // namespace

void Set(const Match& match)
{
  s_match = match;
  s_active = true;
  std::string guest;
  if (match.local == 0)
    guest = match.guest_save.empty() ? ", the guest's save: none (a new game)" :
                                       ", the guest's save: " + std::to_string(match.guest_save.size()) + " bytes";
  const char* machine = Gba() ? "GBA" : "Game Boy";
  LogF("gblink: a link cable match: %s 1 %s, %s 2 %s; ours %s %d%s", machine, match.games[0].title.c_str(), machine,
       match.games[1].title.c_str(), machine, match.local + 1, guest.c_str());
}

bool Gba()
{
  return s_active && s_match.games[0].system == SystemId::GBA;
}

void Clear()
{
  s_active = false;
  s_match = Match();
}

bool Active()
{
  return s_active;
}

const Match& Get()
{
  return s_match;
}

void AdvanceClock(uint8_t* rtc, uint64_t now)
{
  const uint64_t then = Get64(rtc + 40);
  const uint32_t high = Get32(rtc + 16);
  if (high & 0x40 || then == 0 || now <= then)
  {
    if (!(high & 0x40) && then == 0)
      Put64(rtc + 40, now);  // (a clock never set: it starts now)
    return;  // stopped (its halt flag), or no time passed
  }
  const uint64_t day = Get32(rtc + 12) | (high & 1) << 8;
  uint64_t t = Get32(rtc + 0) % 60 + 60 * (Get32(rtc + 4) % 60) + 3600 * (Get32(rtc + 8) % 24) + 86400 * day +
               (now - then);
  const uint32_t seconds = static_cast<uint32_t>(t % 60);
  t /= 60;
  const uint32_t minutes = static_cast<uint32_t>(t % 60);
  t /= 60;
  const uint32_t hours = static_cast<uint32_t>(t % 24);
  t /= 24;
  uint32_t new_high = high & ~1u;
  if (t > 511)
  {
    new_high |= 0x80;  // the days' carry: past 511 days
    t %= 512;
  }
  new_high |= static_cast<uint32_t>(t >> 8) & 1;
  Put32(rtc + 0, seconds);
  Put32(rtc + 4, minutes);
  Put32(rtc + 8, hours);
  Put32(rtc + 12, static_cast<uint32_t>(t & 0xFF));
  Put32(rtc + 16, new_high);
  Put64(rtc + 40, now);
}

std::vector<uint8_t> ReadSave(const std::string& key, uint64_t now)
{
  std::vector<uint8_t> save, rtc;
  if (!ReadWhole(Saves::SramPath(key), &save, MAX_SAVE - RTC_SIZE))
  {
    if (!save.empty())
      LogF("gblink: the save of %s is bigger than a Game Boy's: not used", key.c_str());
    return {};
  }
  if (ReadWhole(Saves::RtcPath(key), &rtc, RTC_SIZE) && rtc.size() == RTC_SIZE)
  {
    AdvanceClock(rtc.data(), now);
    save.insert(save.end(), rtc.begin(), rtc.end());
  }
  return save;
}

bool WriteSave(const std::string& key, const uint8_t* data, size_t size, size_t ram_size, bool rtc_too)
{
  if (ram_size > size)
    return false;
  bool ok = WriteDataFile(Saves::SramPath(key), data, ram_size);
  if (rtc_too && size - ram_size == RTC_SIZE)
    ok = WriteDataFile(Saves::RtcPath(key), data + ram_size, RTC_SIZE) && ok;
  return ok;
}

void BackupSave(const std::string& key)
{
  for (const std::string& path : {Saves::SramPath(key), Saves::RtcPath(key)})
  {
    std::vector<uint8_t> data;
    if (!ReadWhole(path, &data, MAX_SAVE) || data.empty())
      continue;
    if (WriteDataFile(path + ".antes-link", data.data(), data.size()))
      LogF("gblink: a copy of %s before the match: %s.antes-link (%zu bytes)", key.c_str(), path.c_str(), data.size());
  }
}

std::vector<uint8_t> PackSave(const std::vector<uint8_t>& save)
{
  std::vector<uint8_t> out;
  out.reserve(save.size() + 1);
  out.push_back(SAVE_VERSION);
  out.insert(out.end(), save.begin(), save.end());
  return out;
}

bool UnpackSave(const uint8_t* data, size_t size, std::vector<uint8_t>* save)
{
  save->clear();
  if (!data || size < 1 || data[0] != SAVE_VERSION || size - 1 > MAX_SAVE)
    return false;
  save->assign(data + 1, data + size);
  return true;
}

void VisibleHalf(unsigned width, unsigned* first_column, unsigned* columns)
{
  if (!s_active || width < 2 * 160)
  {
    *first_column = 0;
    *columns = width;
    return;
  }
  *columns = width / 2;
  *first_column = s_match.local == 1 ? width / 2 : 0;
}
}  // namespace GbLink
