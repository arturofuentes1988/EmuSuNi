// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// RetroAchievements' identification of a game on a disc (src/Achievements.h): rcheevos' rc_hash reads the disc's
// files through a "CD reader". Its own reads .cue sheets with their .bin tracks, .gdi sheets and plain .iso / .bin
// / .img images; this one adds the images the app plays that it can't read: CHD (MAME's compressed CD, GD-ROM and DVD
// images, through libchdr: the PlayStation's, the Dreamcast's, the PSP's and the Saturn's), CSO (the PSP's compressed
// ISO), CloneCD's .ccd (its .img) and Alcohol's .mds (its .mdf). Everything is read where the app reads the game
// (the USB drive's path), never copied.

#pragma once

#include <string>

#include "Systems.h"

struct rc_hash_cdreader;

namespace AchievementsDisc
{
// The reader for rc_hash (rc_client_set_hash_callbacks): the images above, the others by rcheevos' own reader.
void GetReader(rc_hash_cdreader* reader);
// Can rc_hash identify this console's game file? false, and why (English, for the screen), when it can't: a
// PlayStation .pbp (Sony's PSN package), an ECM-packed image, a DiscJuggler .cdi, a PSP homebrew .elf...
bool CanHash(SystemId system, const std::string& path, std::string* why);
// The path rc_hash should read for this file: a CloneCD .ccd's .img, an Alcohol .mds's .mdf (the same name beside
// it, any letters' case); else the path as it is.
std::string HashPath(const std::string& path);
}  // namespace AchievementsDisc
