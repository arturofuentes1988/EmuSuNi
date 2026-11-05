// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Online play's marks on the game's screen (00.23, src/Netplay.h): in a corner the ping (and "relay" when the match
// goes through a relay server) and who is speaking in the voice chat; in the middle what the match waits for
// ("Connecting to Ana...", "Waiting for Ana...", "Resyncing..."). And the screen a guest sees while the host's settings
// come, before its game starts.
// (00.92) The corner is the latency bar: a row for each player - its nickname, its ping and its connection's quality
// as bars in green, amber or red (src/Netplay.h's PlayerView::quality: the ping, its jitter, the pings lost, the waits
// for that player's inputs) - and the match's input delay; hidden with Online Settings' Latency Bar (the in-game menu's
// online page too). The room's screen in the menu (src/Menu.cpp) draws the same rows.

#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "Netplay.h"

namespace NetplayUi
{
// Over the game's picture (Netplay::GetStatus(): any thread - the drawing beside the emulation too).
void DrawOverlay(ImDrawList* dl, ImFont* regular, ImFont* bold);
// A whole screen saying `line` (translated), before the game's picture.
void DrawWaiting(ImDrawList* dl, ImFont* regular, ImFont* bold, const std::string& line);
// (00.92) The latency bar's box, its top right corner at `corner`: a row for each player (`delay` > 0: the match's
// input delay under them). Its bottom.
float DrawLatencyBar(ImDrawList* dl, ImFont* regular, ImFont* bold, const std::vector<Netplay::PlayerView>& players,
                     ImVec2 corner, int delay, float alpha);
// A connection's quality as four bars (from `at`, `h` tall), lit by it - good 4, fair 3, bad 2 (1 over 250 ms), none
// while not known - in its colour.
void DrawBars(ImDrawList* dl, ImVec2 at, float h, int quality, int ping_ms, float alpha);
ImU32 QualityColor(int quality, float alpha);
}  // namespace NetplayUi
