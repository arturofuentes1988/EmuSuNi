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

// (01.15, the owner: the nicknames "como superpuestos") A chat's line - the room's, the world chat's, the game's menu's:
// its nickname and ":" in bold, then its words in the regular font after it, wrapped under it. (Until 01.14 the whole
// line was drawn in the regular font and the nickname again over it in bold - wider: the bold one ran over the words
// and the regular one showed under it.) Laid out first - its height, to go up from the newest -, then drawn.
struct ChatLine
{
  std::string who;                 // "Ana:" (a nickname wider than the line: cut with "...")
  float indent = 0.0f;             // where the first line's words start
  std::vector<std::string> lines;  // the words' lines (the first empty: a long nickname, the words under it)
  float height = 0.0f;
};
ChatLine LayoutChatLine(ImFont* regular, ImFont* bold, float size, float line_h, const std::string& nick,
                        const std::string& words, float width);
void DrawChatLine(ImDrawList* dl, ImFont* regular, ImFont* bold, float size, float line_h, const ChatLine& line,
                  ImVec2 at, ImU32 who_colour, ImU32 colour);
}  // namespace NetplayUi
