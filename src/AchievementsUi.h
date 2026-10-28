// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// RetroAchievements over the game (src/Achievements.h): the popups - an achievement unlocked with its picture and
// name, the game's achievements loaded (or why not), a game completed, a leaderboard - at the top right, one after
// another; and what stays while it lasts: the challenge indicators (achievements one move away) at the bottom left,
// a measured achievement's progress ("5/10") and the leaderboards' trackers.

#pragma once

#include <string>

#include <imgui.h>

class Display;

namespace AchievementsUi
{
// Each pass of the game, after the in-game menu's drawing (the drawing's thread: the display's).
void Draw(ImDrawList* dl, Display& display, ImFont* regular, ImFont* bold, float dt);
// A picture by its address (an achievement's, a game's): its texture once its file is downloaded (data_dir/cache/ra),
// else null (the download starts). The drawing's thread; a few new textures a frame at most.
ImTextureID Picture(Display& display, const std::string& address);
// The game's end: the popups left and the pictures' textures go.
void Forget(Display& display);
}  // namespace AchievementsUi
