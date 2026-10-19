// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Systems.h"

#include <algorithm>
#include <cctype>

const std::vector<System>& Systems()
{
  static const std::vector<System> SYSTEMS = {
      {SystemId::SNES, "snes", "Super Nintendo", "Super Famicom", "SNES", "SNES", "SUPER FAMICOM",
       0x7B4DFF, 0xD8424A, "", "games", {".sfc", ".smc", ".swc", ".fig", ".bs"},
       "Nintendo - Super Nintendo Entertainment System",
       "libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System", &Cores::SNES9X},
      {SystemId::NES, "nes", "NES", "Famicom", "NES", "NES", "FAMICOM", 0x5C6070, 0xB8293A, "nes/",
       "nes", {".nes", ".fds", ".unf", ".unif"}, "Nintendo - Nintendo Entertainment System",
       "libretro-thumbnails/Nintendo_-_Nintendo_Entertainment_System", &Cores::FCEUMM},
#ifdef EMUSUNI_N64
      // N64 games are big: their folder is on the USB drive (Roms/n64) until another is chosen. The
      // app sees the USB drives as /usb0, /usb1... (FTP shows them as /mnt/usb0).
      {SystemId::N64, "n64", "Nintendo 64", "Nintendo 64", "N64", "N64", "N64", 0x1E9E4A, 0xD8424A,
       "n64/", "/usb0/Roms/n64", {".z64", ".n64", ".v64"}, "Nintendo - Nintendo 64",
       "libretro-thumbnails/Nintendo_-_Nintendo_64", &Cores::MUPEN64PLUS},
#endif
#ifdef EMUSUNI_GBA
      // The three Game Boys share mGBA; each has its tab, folder and covers. A .gb game is the Game
      // Boy's, a .gbc one the Game Boy Color's (the ones for both included).
      {SystemId::GB, "gb", "Game Boy", "Game Boy", "GB", "GB", "GB", 0x7E8A2E, 0x7E8A2E, "gb/",
       "/usb0/Roms/gb", {".gb", ".sgb", ".dmg"}, "Nintendo - Game Boy", "libretro-thumbnails/Nintendo_-_Game_Boy",
       &Cores::MGBA},
      {SystemId::GBC, "gbc", "Game Boy Color", "Game Boy Color", "GBC", "GBC", "GBC", 0xC0247A,
       0xC0247A, "gbc/", "/usb0/Roms/gbc", {".gbc", ".cgb"}, "Nintendo - Game Boy Color",
       "libretro-thumbnails/Nintendo_-_Game_Boy_Color", &Cores::MGBA},
      {SystemId::GBA, "gba", "Game Boy Advance", "Game Boy Advance", "GBA", "GBA", "GBA", 0x3D4FC4,
       0x3D4FC4, "gba/", "/usb0/Roms/gba", {".gba", ".agb"}, "Nintendo - Game Boy Advance",
       "libretro-thumbnails/Nintendo_-_Game_Boy_Advance", &Cores::MGBA},
#endif
#ifdef EMUSUNI_PS1
      // PlayStation games are CD images (a .cue with its .bin tracks, .chd, .pbp, an .m3u for the
      // discs of one game): the core reads them itself from their path, from the USB drive.
      {SystemId::PS1, "ps1", "PlayStation", "PlayStation", "PS1", "PS1", "PS1", 0x2D5DB8, 0x2D5DB8, "ps1/",
       "/usb0/Roms/ps1", {".cue", ".chd", ".pbp", ".m3u", ".iso", ".img", ".ecm", ".mds"}, "Sony - PlayStation",
       "libretro-thumbnails/Sony_-_PlayStation", &Cores::SWANSTATION, true},
#endif
#ifdef EMUSUNI_NDS
      // DS games are read by the core itself from their path (one copy in memory: they are up to
      // 512 MiB), so no .zip either.
      {SystemId::NDS, "nds", "Nintendo DS", "Nintendo DS", "DS", "DS", "DS", 0x8A1C2B, 0x8A1C2B, "nds/",
       "/usb0/Roms/nds", {".nds", ".ids"}, "Nintendo - Nintendo DS", "libretro-thumbnails/Nintendo_-_Nintendo_DS",
       &Cores::MELONDSDS, true},
#endif
#ifdef EMUSUNI_PSP
      // PSP games are UMD images (.iso, .cso, .chd) or PSN downloads (.pbp), up to 1.8 GB: the core
      // reads them itself from their path. A .iso, .chd or .pbp is the PlayStation's or the PSP's
      // by its folder, or by what is inside it (src/Library.cpp).
      {SystemId::PSP, "psp", "PSP", "PSP", "PSP", "PSP", "PSP", 0x5B2D8C, 0x5B2D8C, "psp/", "/usb0/Roms/psp",
       {".iso", ".cso", ".chd", ".pbp", ".elf"}, "Sony - PlayStation Portable",
       "libretro-thumbnails/Sony_-_PlayStation_Portable", &Cores::PPSSPP, true},
#endif
#ifdef EMUSUNI_DC
      // Dreamcast games are GD-ROM/CD images (.gdi with its tracks, .chd, .cdi, .cue, an .m3u for the
      // discs of one game), up to ~1.2 GB: the core reads them from their path (stdio, never whole in
      // memory). A .chd, .cue or .m3u is the PlayStation's, the PSP's or the Dreamcast's by its folder or
      // by what is inside it (Library.cpp).
      // (Never .bin: Flycast takes .bin, .lst, .dat and .zip for NAOMI arcade games.)
      {SystemId::DC, "dc", "Dreamcast", "Dreamcast", "DC", "DC", "DREAMCAST", 0xF26722, 0xF26722, "dc/",
       "/usb0/Roms/dc", {".gdi", ".cdi", ".chd", ".cue", ".m3u"}, "Sega - Dreamcast",
       "libretro-thumbnails/Sega_-_Dreamcast", &Cores::FLYCAST, true},
#endif
  };
  return SYSTEMS;
}

const System& GetSystem(SystemId id)
{
  // By its id: a build without the N64 has no third entry.
  for (const System& system : Systems())
  {
    if (system.id == id)
      return system;
  }
  return Systems().front();
}

bool HasSystem(SystemId id)
{
  for (const System& system : Systems())
  {
    if (system.id == id)
      return true;
  }
  return false;
}

const System* SystemOfFile(const std::string& name)
{
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos)
    return nullptr;
  std::string extension = name.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const System& system : Systems())
  {
    if (std::find(system.extensions.begin(), system.extensions.end(), extension) != system.extensions.end())
      return &system;
  }
  return nullptr;
}

const System* SystemByKey(const std::string& key)
{
  for (const System& system : Systems())
  {
    if (key == system.key)
      return &system;
  }
  return nullptr;
}

const System& SystemOfKey(const std::string& game_key)
{
  for (const System& system : Systems())
  {
    const std::string prefix = system.prefix;
    if (!prefix.empty() && game_key.compare(0, prefix.size(), prefix) == 0)
      return system;
  }
  return GetSystem(SystemId::SNES);
}
