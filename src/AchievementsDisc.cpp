// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "AchievementsDisc.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <zlib.h>

#include <libchdr/chd.h>

#include "rc_hash.h"

#include "Log.h"

namespace AchievementsDisc
{
namespace
{
std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// "game.CHD" -> "chd"
std::string Extension(const std::string& path)
{
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  return Lower(path.substr(dot + 1));
}

// The file beside `path` with its name and another extension, in any letters' case ("" when there is none).
std::string Beside(const std::string& path, const char* extension)
{
  const std::filesystem::path p(path);
  const std::string want = Lower(p.stem().string() + "." + extension);
  std::error_code error;
  const std::filesystem::path folder = p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
  const std::string exact = (folder / (p.stem().string() + "." + extension)).string();
  if (std::filesystem::exists(exact, error))
    return exact;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error))
  {
    if (Lower(entry.path().filename().string()) == want)
      return entry.path().string();
  }
  return {};
}

// ---- CHD (libchdr) ----
// A CD image's frames are 2352 bytes of sector and 96 of subcode; its tracks follow each other, each padded to a
// multiple of 4 frames. A GD-ROM's three areas are its tracks too (the high-density area's first track at sector
// 45000, as a .gdi says). A DVD image (the PSP's UMDs: chdman createdvd) is 2048-byte sectors, no tracks.
constexpr uint32_t CD_FRAME = 2352 + 96;
constexpr uint32_t TRACK_PADDING = 4;
constexpr uint32_t SESSION_GAP = 11400;  // a two-session CD's lead-out, lead-in and pregap (a Dreamcast MIL-CD)

struct ChdInfo
{
  int number = 0;
  std::string type;   // "MODE1_RAW", "MODE2_RAW", "AUDIO", ...
  uint32_t frames = 0, pregap = 0;
  bool pregap_stored = false;  // PGTYPE "V...": the pregap's frames are in the image
  uint32_t first_lba = 0, frame0 = 0;
};

struct ChdTrack
{
  chd_file* chd = nullptr;
  uint32_t hunkbytes = 0, frame_bytes = CD_FRAME, frames_per_hunk = 0;
  std::vector<uint8_t> hunk;
  int64_t hunk_at = -1;
  uint32_t first_lba = 0, frame0 = 0, sectors = 0;
  bool raw = true;          // 2352-byte sectors (the user data after a header) - else cooked (2048, or 2336)
  uint32_t cooked_offset = 0;  // a cooked sector's user data (MODE2/2336: after its 8-byte subheader)
};

bool IsData(const std::string& type)
{
  return type != "AUDIO";
}

uint32_t SectorBytes(const std::string& type)
{
  if (type == "MODE1" || type == "MODE1/2048" || type == "MODE2_FORM1")
    return 2048;
  if (type == "MODE2" || type == "MODE2/2336" || type == "MODE2_FORM_MIX")
    return 2336;
  return 2352;  // MODE1_RAW, MODE2_RAW, CDI/2352, AUDIO
}

// The image's tracks (none: a DVD image, or not a disc). *gdrom: a GD-ROM.
std::vector<ChdInfo> ChdTracks(chd_file* chd, bool* gdrom)
{
  std::vector<ChdInfo> tracks;
  *gdrom = false;
  uint32_t lba = 0, frame = 0;
  for (uint32_t index = 0; index < 99; index++)
  {
    char text[512] = {};
    uint32_t length = 0, tag = 0;
    uint8_t flags = 0;
    char type[32] = {}, subtype[32] = {}, pgtype[32] = {}, pgsub[32] = {};
    int number = 0, frames = 0, pregap = 0, postgap = 0, pad = 0;
    bool found = false;
    if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, text, sizeof(text) - 1, &length, &tag, &flags) ==
        CHDERR_NONE)
    {
      found = std::sscanf(text, CDROM_TRACK_METADATA2_FORMAT, &number, type, subtype, &frames, &pregap, pgtype, pgsub,
                          &postgap) >= 4;
    }
    else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, text, sizeof(text) - 1, &length, &tag, &flags) ==
             CHDERR_NONE)
    {
      found = std::sscanf(text, CDROM_TRACK_METADATA_FORMAT, &number, type, subtype, &frames) >= 4;
    }
    else if (chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, index, text, sizeof(text) - 1, &length, &tag, &flags) ==
                 CHDERR_NONE ||
             chd_get_metadata(chd, GDROM_OLD_METADATA_TAG, index, text, sizeof(text) - 1, &length, &tag, &flags) ==
                 CHDERR_NONE)
    {
      found = std::sscanf(text, GDROM_TRACK_METADATA_FORMAT, &number, type, subtype, &frames, &pad, &pregap, pgtype,
                          pgsub, &postgap) >= 4;
      *gdrom = true;
    }
    if (!found || frames <= 0)
      break;
    ChdInfo track;
    track.number = number;
    track.type = type;
    track.frames = static_cast<uint32_t>(frames);
    track.pregap_stored = pregap > 0 && pgtype[0] == 'V';
    track.pregap = track.pregap_stored ? static_cast<uint32_t>(pregap) : 0;
    track.first_lba = lba + track.pregap;
    track.frame0 = frame + track.pregap;
    tracks.push_back(track);
    lba += track.frames;
    frame += (track.frames + TRACK_PADDING - 1) / TRACK_PADDING * TRACK_PADDING;
  }
  // A Dreamcast MIL-CD: an audio track (the first session), then the data (the second, after the sessions' gap).
  if (!*gdrom && tracks.size() == 2 && !IsData(tracks[0].type) && IsData(tracks[1].type))
    tracks[1].first_lba += SESSION_GAP;
  return tracks;
}

void* ChdOpen(const char* path, uint32_t number)
{
  chd_file* chd = nullptr;
  const chd_error error = chd_open(path, CHD_OPEN_READ, nullptr, &chd);
  if (error != CHDERR_NONE || !chd)
  {
    LogF("ra: chd: %s can't be opened: %s", path, chd_error_string(error));
    return nullptr;
  }
  const chd_header* header = chd_get_header(chd);
  auto* track = new ChdTrack;
  track->chd = chd;
  track->hunkbytes = header->hunkbytes;
  track->hunk.resize(header->hunkbytes);
  bool gdrom = false;
  const std::vector<ChdInfo> tracks = ChdTracks(chd, &gdrom);
  if (tracks.empty())
  {
    // A DVD image: one track of 2048-byte sectors (a CD image without its tracks' metadata would be unreadable).
    if (header->hunkbytes % 2048 != 0 || (number > 1 && number < RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION) ||
        number == RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION)
    {
      chd_close(chd);
      delete track;
      return nullptr;
    }
    track->frame_bytes = 2048;
    track->frames_per_hunk = header->hunkbytes / 2048;
    track->sectors = static_cast<uint32_t>(header->logicalbytes / 2048);
    track->raw = false;
    LogF("ra: chd: %s: a DVD image, %u sectors", path, track->sectors);
    return track;
  }
  if (header->hunkbytes % CD_FRAME != 0)
  {
    LogF("ra: chd: %s: hunks of %u bytes aren't CD frames", path, header->hunkbytes);
    chd_close(chd);
    delete track;
    return nullptr;
  }
  track->frames_per_hunk = header->hunkbytes / CD_FRAME;
  // The track asked for: its number, the first data track, the last, or the largest.
  const ChdInfo* chosen = nullptr;
  for (const ChdInfo& info : tracks)
  {
    if (number == RC_HASH_CDTRACK_FIRST_DATA ? IsData(info.type) && !chosen :
        number == RC_HASH_CDTRACK_LAST       ? true :
        number == RC_HASH_CDTRACK_LARGEST || number == 0 ? !chosen || info.frames > chosen->frames :
                                                           static_cast<uint32_t>(info.number) == number)
      chosen = &info;
  }
  if (!chosen || number == RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION)
  {
    chd_close(chd);
    delete track;
    return nullptr;
  }
  track->first_lba = chosen->first_lba;
  track->frame0 = chosen->frame0;
  track->sectors = chosen->frames - chosen->pregap;
  const uint32_t bytes = SectorBytes(chosen->type);
  track->raw = bytes == 2352;
  track->cooked_offset = bytes == 2336 ? 8 : 0;
  LogF("ra: chd: %s: %s%zu tracks, track %d (%s) at sector %u, %u sectors", path, gdrom ? "a GD-ROM, " : "",
       tracks.size(), chosen->number, chosen->type.c_str(), track->first_lba, track->sectors);
  return track;
}

// One sector's frame (into the hunk read), or null.
const uint8_t* ChdFrame(ChdTrack* track, uint32_t index)
{
  const uint64_t frame = static_cast<uint64_t>(track->frame0) + index;
  const uint64_t hunk = frame / track->frames_per_hunk;
  if (static_cast<int64_t>(hunk) != track->hunk_at)
  {
    if (chd_read(track->chd, static_cast<uint32_t>(hunk), track->hunk.data()) != CHDERR_NONE)
    {
      track->hunk_at = -1;
      return nullptr;
    }
    track->hunk_at = static_cast<int64_t>(hunk);
  }
  return track->hunk.data() + (frame % track->frames_per_hunk) * track->frame_bytes;
}

size_t ChdRead(ChdTrack* track, uint32_t sector, uint8_t* out, size_t bytes)
{
  if (sector < track->first_lba)
    return 0;
  uint32_t index = sector - track->first_lba;
  size_t done = 0;
  while (done < bytes && index < track->sectors)
  {
    const uint8_t* frame = ChdFrame(track, index);
    if (!frame)
      break;
    // A raw sector's user data: after its sync and header (mode 1: 16 bytes), and a mode 2 sector's subheader (24).
    const size_t offset = track->raw ? (frame[15] == 2 ? 24 : 16) : track->cooked_offset;
    const size_t take = std::min<size_t>(2048, bytes - done);
    std::memcpy(out + done, frame + offset, take);
    done += take;
    index++;
  }
  return done;
}

void ChdClose(ChdTrack* track)
{
  if (track->chd)
    chd_close(track->chd);
  delete track;
}

// ---- CSO (the PSP's compressed ISO: "CISO") ----
// A header, then a table of the blocks' places in the file (the high bit: stored as it is, in version 1); each block
// is deflated (raw deflate) unless stored. Version 2 (maxcso) stores a block whose compressed size reaches the block
// size, and marks LZ4 blocks with the high bit (not read here: their game can't be identified).
struct CsoTrack
{
  FILE* file = nullptr;
  uint64_t total = 0;
  uint32_t block_size = 0;
  uint8_t align = 0, version = 1;
  std::vector<uint32_t> index;
  std::vector<uint8_t> packed, block;
  int64_t block_at = -1;
};

uint32_t Le32(const uint8_t* p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void CsoClose(CsoTrack* track)
{
  if (track->file)
    std::fclose(track->file);
  delete track;
}

void* CsoOpen(const char* path, uint32_t number)
{
  if (number > 1 && number != RC_HASH_CDTRACK_FIRST_DATA && number != RC_HASH_CDTRACK_LAST &&
      number != RC_HASH_CDTRACK_LARGEST)
    return nullptr;
  auto* track = new CsoTrack;
  track->file = std::fopen(path, "rb");
  uint8_t header[24] = {};
  if (!track->file || std::fread(header, 1, sizeof(header), track->file) != sizeof(header) ||
      std::memcmp(header, "CISO", 4) != 0)
  {
    LogF("ra: cso: %s isn't a CSO image", path);
    CsoClose(track);
    return nullptr;
  }
  track->total = Le32(header + 8) | (static_cast<uint64_t>(Le32(header + 12)) << 32);
  track->block_size = Le32(header + 16);
  track->version = header[20];
  track->align = header[21];
  if (track->block_size < 2048 || track->block_size > (1u << 20) || track->block_size % 2048 != 0 || track->total == 0 ||
      track->align > 16)
  {
    LogF("ra: cso: %s: blocks of %u bytes, %llu bytes in all: not read", path, track->block_size,
         static_cast<unsigned long long>(track->total));
    CsoClose(track);
    return nullptr;
  }
  const uint64_t blocks = (track->total + track->block_size - 1) / track->block_size;
  track->index.resize(static_cast<size_t>(blocks) + 1);
  std::vector<uint8_t> table(track->index.size() * 4);
  const uint32_t header_size = Le32(header + 4) >= 24 ? Le32(header + 4) : 24;
  if (std::fseek(track->file, static_cast<long>(header_size), SEEK_SET) != 0 ||
      std::fread(table.data(), 1, table.size(), track->file) != table.size())
  {
    LogF("ra: cso: %s: its block table can't be read", path);
    CsoClose(track);
    return nullptr;
  }
  for (size_t i = 0; i < track->index.size(); i++)
    track->index[i] = Le32(table.data() + i * 4);
  track->block.resize(track->block_size);
  LogF("ra: cso: %s: version %u, %llu bytes in %llu blocks of %u", path, track->version,
       static_cast<unsigned long long>(track->total), static_cast<unsigned long long>(blocks), track->block_size);
  return track;
}

bool CsoBlock(CsoTrack* track, uint32_t number)
{
  if (static_cast<int64_t>(number) == track->block_at)
    return true;
  track->block_at = -1;
  if (number + 1 >= track->index.size())
    return false;
  const uint32_t entry = track->index[number];
  const uint64_t start = static_cast<uint64_t>(entry & 0x7FFFFFFFu) << track->align;
  const uint64_t end = static_cast<uint64_t>(track->index[number + 1] & 0x7FFFFFFFu) << track->align;
  if (end <= start || end - start > track->block_size + 1024 * 64)
    return false;
  const size_t size = static_cast<size_t>(end - start);
  const bool high = (entry & 0x80000000u) != 0;
  const bool stored = track->version >= 2 ? size >= track->block_size : high;
  if (track->version >= 2 && !stored && high)
  {
    LogF("ra: cso: an LZ4 block (CSO version 2): not read");
    return false;
  }
  track->packed.resize(size);
  if (fseeko(track->file, static_cast<off_t>(start), SEEK_SET) != 0 ||
      std::fread(track->packed.data(), 1, size, track->file) != size)
    return false;
  if (stored)
  {
    std::memcpy(track->block.data(), track->packed.data(), std::min<size_t>(size, track->block_size));
  }
  else
  {
    z_stream z = {};
    if (inflateInit2(&z, -15) != Z_OK)
      return false;
    z.next_in = track->packed.data();
    z.avail_in = static_cast<uInt>(size);
    z.next_out = track->block.data();
    z.avail_out = track->block_size;
    const int result = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (result != Z_STREAM_END && !(result == Z_OK && z.avail_out == 0) && !(result == Z_BUF_ERROR && z.avail_out == 0))
      return false;
  }
  track->block_at = number;
  return true;
}

size_t CsoRead(CsoTrack* track, uint32_t sector, uint8_t* out, size_t bytes)
{
  uint64_t at = static_cast<uint64_t>(sector) * 2048;
  size_t done = 0;
  while (done < bytes && at < track->total)
  {
    const uint32_t number = static_cast<uint32_t>(at / track->block_size);
    if (!CsoBlock(track, number))
      break;
    const size_t offset = static_cast<size_t>(at % track->block_size);
    const size_t take = static_cast<size_t>(
        std::min<uint64_t>({track->block_size - offset, bytes - done, track->total - at}));
    std::memcpy(out + done, track->block.data() + offset, take);
    done += take;
    at += take;
  }
  return done;
}

// ---- The reader rc_hash calls: each track is one of the three kinds ----
struct Track
{
  enum Kind
  {
    OWN,  // rcheevos' own reader's track
    CHD,
    CSO,
  } kind = OWN;
  void* inner = nullptr;
};

rc_hash_cdreader_t s_own;  // rcheevos' own reader (rc_hash_get_default_cdreader)

void* RC_CCONV OpenTrackIterator(const char* path, uint32_t number, const rc_hash_iterator_t* iterator)
{
  const std::string extension = Extension(path);
  Track track;
  if (extension == "chd")
  {
    track.kind = Track::CHD;
    track.inner = ChdOpen(path, number);
  }
  else if (extension == "cso")
  {
    track.kind = Track::CSO;
    track.inner = CsoOpen(path, number);
  }
  else if (iterator && s_own.open_track_iterator)
  {
    track.inner = s_own.open_track_iterator(HashPath(path).c_str(), number, iterator);
  }
  if (!track.inner)
    return nullptr;
  return new Track(track);
}

// (rc_hash takes the reader only when this is set; it calls OpenTrackIterator.)
void* RC_CCONV OpenTrack(const char* path, uint32_t number)
{
  return OpenTrackIterator(path, number, nullptr);
}

size_t RC_CCONV ReadSector(void* handle, uint32_t sector, void* buffer, size_t bytes)
{
  auto* track = static_cast<Track*>(handle);
  if (!track || !buffer)
    return 0;
  switch (track->kind)
  {
  case Track::CHD:
    return ChdRead(static_cast<ChdTrack*>(track->inner), sector, static_cast<uint8_t*>(buffer), bytes);
  case Track::CSO:
    return CsoRead(static_cast<CsoTrack*>(track->inner), sector, static_cast<uint8_t*>(buffer), bytes);
  default:
    return s_own.read_sector(track->inner, sector, buffer, bytes);
  }
}

void RC_CCONV CloseTrack(void* handle)
{
  auto* track = static_cast<Track*>(handle);
  if (!track)
    return;
  switch (track->kind)
  {
  case Track::CHD:
    ChdClose(static_cast<ChdTrack*>(track->inner));
    break;
  case Track::CSO:
    CsoClose(static_cast<CsoTrack*>(track->inner));
    break;
  default:
    s_own.close_track(track->inner);
    break;
  }
  delete track;
}

uint32_t RC_CCONV FirstTrackSector(void* handle)
{
  auto* track = static_cast<Track*>(handle);
  if (!track)
    return 0;
  switch (track->kind)
  {
  case Track::CHD:
    return static_cast<ChdTrack*>(track->inner)->first_lba;
  case Track::CSO:
    return 0;
  default:
    return s_own.first_track_sector(track->inner);
  }
}
}  // namespace

void GetReader(rc_hash_cdreader* reader)
{
  rc_hash_get_default_cdreader(&s_own);
  reader->open_track = OpenTrack;
  reader->read_sector = ReadSector;
  reader->close_track = CloseTrack;
  reader->first_track_sector = FirstTrackSector;
  reader->open_track_iterator = OpenTrackIterator;
}

std::string HashPath(const std::string& path)
{
  const std::string extension = Extension(path);
  if (extension == "ccd")
  {
    const std::string image = Beside(path, "img");
    return image.empty() ? path : image;
  }
  if (extension == "mds")
  {
    const std::string image = Beside(path, "mdf");
    return image.empty() ? path : image;
  }
  return path;
}

bool CanHash(SystemId system, const std::string& path, std::string* why)
{
  const std::string extension = Extension(path);
  if (extension == "ecm")
  {
    *why = "An ECM-packed image can't be identified (a .cue with its .bin, or a .chd, can)";
    return false;
  }
  if ((extension == "ccd" || extension == "mds") && HashPath(path) == path)
  {
    *why = extension == "ccd" ? "The .img beside the .ccd isn't there" : "The .mdf beside the .mds isn't there";
    return false;
  }
  switch (system)
  {
  case SystemId::PS1:
    if (extension == "pbp")
    {
      *why = "A PlayStation .pbp can't be identified (a .cue with its .bin, or a .chd, can)";
      return false;
    }
    return true;
  case SystemId::DC:
    if (extension == "cdi")
    {
      *why = "A DiscJuggler .cdi can't be identified (a .gdi or a .chd can)";
      return false;
    }
    return true;
  case SystemId::PSP:
    if (extension == "elf")
    {
      *why = "A homebrew .elf has no achievements";
      return false;
    }
    return true;
  default:
    return true;
  }
}
}  // namespace AchievementsDisc
