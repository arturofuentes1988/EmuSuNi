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
      // discs of one game): the core reads them itself from their path, from the USB drive. A .bin no .cue
      // names is a game too (00.81), though not one of these extensions: a .bin is the Genesis's
      // (SystemOfFile); src/Library.cpp tells a PlayStation disc's by what it holds.
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
#ifdef EMUSUNI_SEGA
      // The four share Genesis Plus GX; each has its tab, folder and covers. The core tells them apart
      // by the file's extension (a .zip's game: the console's first one); a .sc is an SG-1000 game
      // too (scripts/build-gpgx-core.sh), a .bin only ever a Mega Drive one. Badge colours unlike the
      // others' (All Games shows every console's covers side by side): the Mega Drive's navy, not the
      // PlayStation's blue; the Master System's vermilion, not the Famicom's red.
      {SystemId::SG1000, "sg1000", "SG-1000", "SG-1000", "SG-1000", "SG-1000", "SG-1000", 0x9A5B23, 0x9A5B23,
       "sg1000/", "/usb0/Roms/sg1000", {".sg", ".sc"}, "Sega - SG-1000", "libretro-thumbnails/Sega_-_SG-1000",
       &Cores::GPGX},
      {SystemId::SMS, "sms", "Master System", "Mark III", "SMS", "SMS", "SMS", 0xE0301E, 0xE0301E, "sms/",
       "/usb0/Roms/sms", {".sms", ".bms"}, "Sega - Master System - Mark III",
       "libretro-thumbnails/Sega_-_Master_System_-_Mark_III", &Cores::GPGX},
      {SystemId::GG, "gg", "Game Gear", "Game Gear", "GG", "GG", "GG", 0x00838F, 0x00838F, "gg/", "/usb0/Roms/gg",
       {".gg"}, "Sega - Game Gear", "libretro-thumbnails/Sega_-_Game_Gear", &Cores::GPGX},
      // Genesis (the user's name for it; Japanese games: Mega Drive, as Super Famicom for the SNES).
      {SystemId::MD, "md", "Genesis", "Mega Drive", "GEN", "GENESIS", "MEGA DRIVE", 0x0E2A66, 0x0E2A66, "md/", "/usb0/Roms/md",
       {".md", ".gen", ".smd", ".bin", ".mdx", ".68k", ".sgd"}, "Sega - Mega Drive - Genesis",
       "libretro-thumbnails/Sega_-_Mega_Drive_-_Genesis", &Cores::GPGX},
#endif
#ifdef EMUSUNI_ARCADE
      // Arcade games are romsets: a .zip (or .7z) named after FinalBurn Neo's driver (mslug.zip), which
      // the core reads itself from its path (by_path; the app never opens it): the Library lists the
      // zips whose name is a driver's (src/Arcade.h). The badge in the cabinets' black.
      {SystemId::ARCADE, "arcade", "Arcade", "Arcade", "Arcade", "ARCADE", "ARCADE", 0x2E2E36, 0x2E2E36, "arcade/",
       "/usb0/Roms/arcade", {".zip", ".7z"}, "FBNeo - Arcade Games", "libretro-thumbnails/FBNeo_-_Arcade_Games",
       &Cores::FBNEO, true},
#endif
#ifdef EMUSUNI_DC
      // Sega's NAOMI (and NAOMI 2) and Sammy's Atomiswave: the Dreamcast's hardware in arcade boards, played by
      // the Dreamcast's core (Flycast). A game is a MAME set: a .zip (or .7z) named after it (mvsc2.zip,
      // kofxi.zip) that the core reads itself (by_path; the app never opens it), the Library lists the zips whose
      // name is one of Flycast's sets of the board (src/Naomi.h), and its BIOS (naomi.zip, awbios.zip) goes
      // beside the games or in system/dc. Keys of their own (naomi/, atomiswave/: their saves, settings and
      // covers apart from the Arcade's), libretro's thumbnails of each board ("Atomiswave" is that folder's
      // name on the thumbnail server), badges in colours no other console has: the NAOMI's gold, the
      // Atomiswave's green. A NAOMI game can also be a cartridge image, a loose .dat or .bin (00.64: its header
      // says NAOMI; the Library reads it).
      {SystemId::NAOMI, "naomi", "NAOMI", "NAOMI", "NAOMI", "NAOMI", "NAOMI", 0x9A7400, 0x9A7400, "naomi/",
       "/usb0/Roms/naomi", {".zip", ".7z", ".dat", ".bin"}, "Sega - Naomi", "libretro-thumbnails/Sega_-_Naomi",
       &Cores::FLYCAST, true},
      {SystemId::ATOMISWAVE, "atomiswave", "Atomiswave", "Atomiswave", "Atomiswave", "ATOMISWAVE", "ATOMISWAVE",
       0x558B2F, 0x558B2F, "atomiswave/", "/usb0/Roms/atomiswave", {".zip", ".7z"}, "Atomiswave",
       "libretro-thumbnails/Atomiswave", &Cores::FLYCAST, true},
#endif
#ifdef EMUSUNI_3DS
      // 3DS games are decrypted dumps (.3ds / .cci, .cxi, or a homebrew's .3dsx; no .cia, which the core
      // can't install) of up to 4 GiB: the core reads them itself from their path (by_path), so no .zip
      // either. The badge in the 3DS's aqua blue.
      {SystemId::THREEDS, "3ds", "Nintendo 3DS", "Nintendo 3DS", "3DS", "3DS", "3DS", 0x1CA0DC, 0x1CA0DC, "3ds/",
       "/usb0/Roms/3ds", {".3ds", ".cci", ".cxi", ".3dsx"}, "Nintendo - Nintendo 3DS",
       "libretro-thumbnails/Nintendo_-_Nintendo_3DS", &Cores::AZAHAR, true},
#endif
#ifdef EMUSUNI_SATURN
      // Saturn games are CD images (a .cue with its .bin tracks, .chd, .iso, .ccd, .mds) that the core reads itself
      // from their path (by_path: no .zip). A .cue, .chd, .iso or .mds is the PlayStation's, the PSP's, the
      // Dreamcast's or the Saturn's by its folder or by what is inside it (src/Library.cpp: after them here, so the
      // others keep those extensions first); a .ccd only the Saturn's. Experimental: YabaSanshiro with its software
      // renderer on threads of its own (scripts/build-saturn-core.sh). The badge in a brown no other console has.
      {SystemId::SATURN, "saturn", "Saturn", "Saturn", "Saturn", "SATURN", "SATURN", 0x6D4C41, 0x6D4C41, "saturn/",
       "/usb0/Roms/saturn", {".cue", ".chd", ".iso", ".ccd", ".mds"}, "Sega - Saturn",
       "libretro-thumbnails/Sega_-_Saturn", &Cores::YABASANSHIRO, true},
#endif
#ifdef EMUSUNI_PCE
      // The PC Engine (TurboGrafx-16 for the games of the West: its badge; the PC Engine's for Japan's): Beetle PCE
      // Fast (scripts/build-beetle-cores.sh). HuCards: a .pce (or a .zip of one), read by the app and handed over from
      // memory. (No SuperGrafx: this core has none of its hardware.) The badge in the TurboGrafx's orange.
      {SystemId::PCE, "pce", "PC Engine", "PC Engine", "PCE", "TURBOGRAFX-16", "PC ENGINE", 0xD0461A, 0xD0461A, "pce/",
       "/usb0/Roms/pce", {".pce"}, "NEC - PC Engine - TurboGrafx 16",
       "libretro-thumbnails/NEC_-_PC_Engine_-_TurboGrafx_16", &Cores::BEETLE_PCE},
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
    // (Not the Arcade's, the NAOMI's and the Atomiswave's .zip and .7z: a .zip is any console's, and an arcade
    // game is known by its name, not its extension - Library.cpp.)
    if (IsRomsetSystem(system.id))
      continue;
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
