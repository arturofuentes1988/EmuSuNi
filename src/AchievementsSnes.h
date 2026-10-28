// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// RetroAchievements (00.73, src/Achievements.h): the Super Nintendo's memory for Snes9x, which gives libretro no memory
// map. Its work RAM, the cartridge's RAM (an SA-1 game's BW-RAM too) and an SA-1 game's 2 KiB of I-RAM - which
// Snes9x's retro_get_memory_data doesn't show at all (RetroArch with Snes9x lacks it too), and which RetroAchievements'
// achievements of SA-1 games (Super Mario RPG, Kirby Super Star...) can read - at the addresses RetroAchievements gives
// them (rcheevos' consoleinfo.c: work RAM at $7E0000, the cartridge's RAM at $1000000, I-RAM at $1080000).
// (Snes9x's own headers stay in AchievementsSnes.cpp: its Settings would meet the app's.)

#pragma once

#include <vector>

#include <libretro.h>

// After the game loaded: the map, as if Snes9x had sent it (Achievements::SetMemoryMap); false before a game.
bool AchievementsSnesMap(std::vector<retro_memory_descriptor>* map);
