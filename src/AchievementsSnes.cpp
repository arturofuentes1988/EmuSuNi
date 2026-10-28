// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "AchievementsSnes.h"

#include <algorithm>

// (snes9x.h first: its types and sizes, which memmap.h uses.)
#include "snes9x.h"
#include "memmap.h"

bool AchievementsSnesMap(std::vector<retro_memory_descriptor>* map)
{
  map->clear();
  if (!Memory.FillRAM)
    return false;
  // The work RAM (128 KiB).
  retro_memory_descriptor ram = {};
  ram.flags = RETRO_MEMDESC_SYSTEM_RAM;
  ram.ptr = Memory.RAM;
  ram.start = 0x7E0000;
  ram.len = sizeof(Memory.RAM);
  map->push_back(ram);
  // The cartridge's RAM, its size as Snes9x maps it (its SRAMMask: the header's size), up to its 512 KiB buffer
  // (memmap.h's SRAM_SIZE): an SA-1 game's BW-RAM is there too.
  const size_t sram = Memory.SRAMSize ? std::min<size_t>(static_cast<size_t>(1u << (Memory.SRAMSize + 3)) * 128,
                                                         Memory.SRAMStorage.size()) :
                                        0;
  if (sram > 0 && Memory.SRAM)
  {
    retro_memory_descriptor cart = {};
    cart.flags = RETRO_MEMDESC_SAVE_RAM;
    cart.ptr = Memory.SRAM;
    cart.start = 0x1000000;
    cart.len = sram;
    map->push_back(cart);
  }
  // An SA-1 game's I-RAM: the SA-1's 2 KiB at $3000 of its own map, which Snes9x keeps at FillRAM + 0x3000
  // (memmap.cpp's SA-1 maps).
  if (Settings.SA1)
  {
    retro_memory_descriptor iram = {};
    iram.flags = RETRO_MEMDESC_SYSTEM_RAM;
    iram.ptr = Memory.FillRAM + 0x3000;
    iram.start = 0x1080000;
    iram.len = 0x800;
    map->push_back(iram);
  }
  return true;
}
