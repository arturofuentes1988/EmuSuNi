// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4IngameMenu.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "IngameMenu.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include <orbis/Pad.h>

#include "Display.h"
#include "Image.h"
#include "Log.h"
#include "PS4Glyphs.h"
#include "PS4Lang.h"
#include "Saves.h"
#include "Settings.h"

namespace
{
enum MainItem
{
  MAIN_RESUME,
  MAIN_SAVE,
  MAIN_LOAD,
  MAIN_SETTINGS,
  MAIN_CHEATS,
  MAIN_RESET,
  MAIN_QUIT,
  MAIN_COUNT,
};

std::string DateText(int64_t when)
{
  if (when <= 0)
    return "";
  const std::time_t then = static_cast<std::time_t>(when);
  std::tm day = {};
  localtime_r(&then, &day);
  char text[48];
  std::strftime(text, sizeof(text), "%b %e, %Y  %H:%M", &day);
  return text;
}
}  // namespace

void IngameMenu::Init(Display* display, ImFont* regular, ImFont* bold)
{
  m_display = display;
  m_font = regular;
  m_bold = bold;
}

void IngameMenu::SetGame(const std::string& data_dir, const std::string& game_id,
                         const std::string& title)
{
  m_data_dir = data_dir;
  m_game_id = game_id;
  m_title = title;
  m_open = false;
  m_appear = 0.0f;
  m_hint_time = 0.0f;
  m_toast.clear();
  m_slot = 1;
  m_cheats.Load(data_dir, game_id);
  ForgetPictures();
}

void IngameMenu::ForgetPictures()
{
  if (m_picture != ImTextureID{} && m_display)
    m_display->DestroyTexture(m_picture);
  m_picture = ImTextureID{};
  m_picture_slot = -1;
}

void IngameMenu::Open()
{
  m_open = true;
  m_page = Page::Main;
  m_main_selected = 0;
  BuildRows();
  LogLine("menu: opened");
}

void IngameMenu::Close()
{
  m_open = false;
  ForgetPictures();
  LogLine("menu: closed");
}

bool IngameMenu::TakeSettingsChanged()
{
  const bool changed = m_settings_changed;
  m_settings_changed = false;
  return changed;
}

bool IngameMenu::TakeCheatsChanged()
{
  const bool changed = m_cheats_changed;
  m_cheats_changed = false;
  return changed;
}

void IngameMenu::Toast(const std::string& text, float seconds)
{
  m_toast = text;
  m_toast_time = seconds;
}

std::string IngameMenu::SlotLabel(int slot) const
{
  const std::string label = "Slot " + std::to_string(slot);
  return Saves::Modified(Saves::StatePath(m_game_id, slot)) ? label : label + " (Empty)";
}

void IngameMenu::BuildRows()
{
  std::vector<Row> rows;
  if (m_page == Page::Main)
  {
    rows.push_back({"Resume Game"});
    rows.push_back({"Save State", SlotLabel(m_slot)});
    rows.push_back({"Load State", SlotLabel(m_slot)});
    rows.push_back({"Settings"});
    rows.push_back({"Cheats", std::to_string(m_cheats.Entries().size())});
    rows.push_back({"Reset Game"});
    rows.push_back({"Quit Game"});
    m_selected = m_main_selected;
  }
  else if (m_page == Page::Cheats)
  {
    const auto& entries = m_cheats.Entries();
    if (!entries.empty())
    {
      rows.push_back({"Enable All", "", false, false, 0, "cheatall:on"});
      rows.push_back({"Disable All", "", false, false, 0, "cheatall:off"});
      rows.push_back({"Codes", "", true});
    }
    for (size_t i = 0; i < entries.size(); i++)
      rows.push_back({entries[i].name, "", false, false, entries[i].enabled ? 1 : -1,
                      "cheat:" + std::to_string(i)});
    if (entries.empty())
    {
      rows.push_back({"No cheats for this game", "", true});
      rows.push_back({"Put codes in " + m_cheats.Path(), "", true});
    }
  }
  else
  {
    const bool custom = Settings::IsCustom(m_game_id);
    rows.push_back({"Apply To", custom ? "This Game" : "All Games", false, false, 0, "__scope__"});
    if (custom)
      rows.push_back({"Use for All Games", "", false, false, 0, "__toall__"});
    for (const char* category : Settings::Categories())
    {
      rows.push_back({category, "", true});
      for (const Settings::Setting& setting : Settings::All())
      {
        if (std::string(setting.category) != category || std::string(setting.key) == "language")
          continue;
        const std::string value = Settings::Get(setting.key, m_game_id);
        rows.push_back({setting.label, Settings::LabelOf(setting, value), false, !setting.live, 0,
                        setting.key});
      }
    }
    rows.push_back({"", "", true});
    rows.push_back({custom ? "Reset This Game" : "Reset to Defaults", "", false, false, 0,
                    "__reset__"});
  }
  m_rows = std::move(rows);
  m_selected = std::clamp(m_selected, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
  if (m_page != Page::Main && !m_rows.empty() && m_rows[m_selected].header)
    Move(1);
}

void IngameMenu::Move(int direction)
{
  const int count = static_cast<int>(m_rows.size());
  int row = m_selected + direction;
  while (row >= 0 && row < count && m_rows[row].header)
    row += direction > 0 ? 1 : -1;
  if (row >= 0 && row < count)
    m_selected = row;
  if (m_page == Page::Main)
    m_main_selected = m_selected;
}

void IngameMenu::Change(int direction)
{
  if (m_selected < 0 || m_selected >= static_cast<int>(m_rows.size()))
    return;
  const std::string key = m_rows[m_selected].key;
  if (m_page == Page::Cheats)
  {
    if (key.rfind("cheatall:", 0) == 0)
    {
      const bool on = key == "cheatall:on";
      m_cheats.SetAll(on);
      Toast(on ? "All cheats on" : "All cheats off");
    }
    else if (key.rfind("cheat:", 0) == 0)
    {
      const size_t index = static_cast<size_t>(std::atoi(key.c_str() + 6));
      m_cheats.Toggle(index);
      const auto& entries = m_cheats.Entries();
      if (index < entries.size())
        Toast(entries[index].name + (entries[index].enabled ? ": on" : ": off"));
    }
    m_cheats_changed = true;
    return;
  }
  if (key == "__scope__")
  {
    const bool custom = !Settings::IsCustom(m_game_id);
    Settings::SetCustom(m_game_id, custom);
    Toast(custom ? "Settings for this game only" : "This game follows the global settings");
    m_settings_changed = true;
    return;
  }
  if (key == "__toall__")
  {
    if (direction <= 0)
      return;
    Settings::CopyGameToGlobal(m_game_id);
    Toast("Applied to all games");
    m_settings_changed = true;
    return;
  }
  if (key == "__reset__")
  {
    if (direction <= 0)
      return;
    if (Settings::IsCustom(m_game_id))
    {
      Settings::ResetGame(m_game_id);
      Toast("This game uses the global settings again");
    }
    else
    {
      for (const Settings::Setting& setting : Settings::All())
      {
        if (std::string(setting.key) != "language")
          Settings::ResetGlobal(setting.key);
      }
      Toast("Settings reset to defaults");
    }
    m_settings_changed = true;
    return;
  }
  const Settings::Setting* setting = Settings::Find(key);
  if (!setting)
    return;
  const std::string value = Settings::Step(*setting, Settings::Get(key, m_game_id), direction);
  Settings::Set(key, value, Settings::IsCustom(m_game_id) ? m_game_id : std::string());
  if (!setting->live)
    Toast(std::string(setting->label) + ": next time a game starts");
  m_settings_changed = true;
}

std::string IngameMenu::RowHelp(const Row& row) const
{
  auto T = [](const char* text) { return std::string(PS4Lang::T(text)); };
  if (m_page == Page::Main)
  {
    static const std::pair<const char*, const char*> MAIN[] = {
        {"Resume Game", "Back to the game."},
        {"Save State", "Saves this very moment of the game in the slot (Left / Right choose the slot)."},
        {"Load State", "Goes back to the moment saved in the slot (Left / Right choose the slot)."},
        {"Settings", "Picture, sound and emulation settings, for this game or for all games."},
        {"Cheats", "Turns this game's cheat codes on and off."},
        {"Reset Game", "Like the console's Reset button."},
        {"Quit Game", "Back to your games (the battery save is kept)."},
    };
    for (const auto& [label, help] : MAIN)
    {
      if (row.label == label)
        return T(help);
    }
    return {};
  }
  if (m_page == Page::Cheats)
  {
    if (row.key == "cheatall:on")
      return T("Turns every cheat on.");
    if (row.key == "cheatall:off")
      return T("Turns every cheat off.");
    if (row.key.rfind("cheat:", 0) == 0)
      return T("Cross turns this cheat on or off.");
    return {};
  }
  if (row.key == "__scope__")
    return T("All Games: what you change here applies to every game. This Game: only to this game.");
  if (row.key == "__toall__")
    return T("Copies this game's settings to every game; this game follows them again.");
  if (row.key == "__reset__")
    return T("Every setting here back to its default.");
  const Settings::Setting* setting = Settings::Find(row.key);
  if (!setting)
    return {};
  std::string help = T(setting->help);
  if (!setting->live)
    help += " " + T("(Next game start.)");
  const Settings::Choice* choice = Settings::FindChoice(*setting, Settings::Get(row.key, m_game_id));
  if (choice && choice->help)
    help += "\n" + T(choice->label) + ": " + T(choice->help);
  return help;
}

IngameMenu::Action IngameMenu::Activate()
{
  if (m_page != Page::Main)
  {
    Change(1);
    return Action::None;
  }
  switch (m_main_selected)
  {
  case MAIN_RESUME:
    Close();
    return Action::Closed;
  case MAIN_SAVE:
    return Action::Save;
  case MAIN_LOAD:
    if (!Saves::Modified(Saves::StatePath(m_game_id, m_slot)))
    {
      Toast("Slot " + std::to_string(m_slot) + " is empty");
      return Action::None;
    }
    return Action::Load;
  case MAIN_SETTINGS:
    m_page = Page::Settings;
    m_selected = 0;
    return Action::None;
  case MAIN_CHEATS:
    m_page = Page::Cheats;
    m_selected = 0;
    return Action::None;
  case MAIN_RESET:
    return Action::Reset;
  case MAIN_QUIT:
    return Action::Quit;
  }
  return Action::None;
}

IngameMenu::Action IngameMenu::Poll(uint32_t pressed, uint32_t held, float dt)
{
  if (!m_open)
    return Action::None;
  // Directions repeat while held.
  uint32_t nav = pressed;
  constexpr uint32_t DIRS = ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT |
                            ORBIS_PAD_BUTTON_RIGHT;
  if (held & DIRS)
  {
    if (pressed & DIRS)
    {
      m_repeat = 0.35f;
    }
    else
    {
      m_repeat -= dt;
      if (m_repeat <= 0.0f)
      {
        nav |= held & DIRS;
        m_repeat = 0.08f;
      }
    }
  }
  Action action = Action::None;
  bool rebuild = false;
  if (nav & ORBIS_PAD_BUTTON_UP)
  {
    Move(-1);
    rebuild = true;
  }
  if (nav & ORBIS_PAD_BUTTON_DOWN)
  {
    Move(1);
    rebuild = true;
  }
  if (nav & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT))
  {
    const int direction = (nav & ORBIS_PAD_BUTTON_LEFT) ? -1 : 1;
    if (m_page == Page::Main)
    {
      if (m_main_selected == MAIN_SAVE || m_main_selected == MAIN_LOAD)
        m_slot = (m_slot - 1 + direction + Saves::SLOTS) % Saves::SLOTS + 1;
    }
    else
    {
      Change(direction);
    }
    rebuild = true;
  }
  constexpr uint32_t COMBO = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
  if (pressed & ORBIS_PAD_BUTTON_CROSS)
  {
    action = Activate();
    rebuild = true;
  }
  else if ((held & COMBO) == COMBO && (pressed & COMBO))
  {
    Close();
    return Action::Closed;
  }
  else if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
  {
    if (m_page != Page::Main)
    {
      m_page = Page::Main;
      rebuild = true;
    }
    else
    {
      Close();
      return Action::Closed;
    }
  }
  if (rebuild && m_open)
    BuildRows();
  return action;
}

void IngameMenu::UpdatePicture()
{
  const bool want = m_open && m_page == Page::Main &&
                    (m_main_selected == MAIN_SAVE || m_main_selected == MAIN_LOAD);
  const int slot = want ? m_slot : -1;
  if (slot == m_picture_slot)
    return;
  ForgetPictures();
  m_picture_slot = slot;
  if (slot < 0)
    return;
  ImageRGBA image;
  if (LoadPng(Saves::StatePicturePath(m_game_id, slot), &image, 512, 448))
  {
    m_picture = m_display->CreateTexture(image.pixels.data(), image.width, image.height);
    m_picture_aspect = 8.0f / 7.0f;
  }
}

void IngameMenu::Draw(ImDrawList* dl, float dt)
{
  m_appear += ((m_open ? 1.0f : 0.0f) - m_appear) * (1.0f - std::exp(-14.0f * dt));
  m_hint_time += dt;
  if (m_toast_time > 0.0f)
    m_toast_time -= dt;
  UpdatePicture();
  const ImVec2 size(static_cast<float>(Display::WIDTH), static_cast<float>(Display::HEIGHT));
  const float a = m_appear;
  ImFont* bold = m_bold;
  ImFont* regular = m_font;
  auto T = [](const char* text) { return PS4Lang::T(text); };

  if (a > 0.01f)
  {
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, static_cast<int>(150 * a)));
    const float panel_w = 860.0f;
    const float x0 = -panel_w * (1.0f - a);
    dl->AddRectFilledMultiColor(ImVec2(x0, 0), ImVec2(x0 + panel_w, size.y),
                                IM_COL32(26, 12, 52, static_cast<int>(240 * a)),
                                IM_COL32(26, 12, 52, static_cast<int>(150 * a)),
                                IM_COL32(8, 4, 18, static_cast<int>(150 * a)),
                                IM_COL32(8, 4, 18, static_cast<int>(240 * a)));
    dl->AddLine(ImVec2(x0 + panel_w, 0), ImVec2(x0 + panel_w, size.y),
                IM_COL32(176, 140, 255, static_cast<int>(200 * a)), 2.0f);
    const ImU32 white = IM_COL32(255, 255, 255, static_cast<int>(255 * a));
    const ImU32 dim = IM_COL32(255, 255, 255, static_cast<int>(165 * a));
    const ImU32 accent = IM_COL32(196, 170, 255, static_cast<int>(255 * a));
    const char* subtitle = m_page == Page::Main     ? "PAUSED" :
                           m_page == Page::Cheats   ? "CHEATS" :
                           Settings::IsCustom(m_game_id) ? "SETTINGS - THIS GAME" :
                                                           "SETTINGS - ALL GAMES";
    dl->AddText(regular, 26, ImVec2(x0 + 70, 90), accent, T(subtitle));
    dl->AddText(bold, 40, ImVec2(x0 + 70, 122), white, m_title.c_str(), nullptr, panel_w - 120);

    const float row_h = 62.0f;
    const float top = 220.0f, bottom = size.y - 280.0f;
    const float visible = (bottom - top) / row_h;
    const float target = std::clamp(static_cast<float>(m_selected) - visible * 0.5f + 1.0f, 0.0f,
                                    std::max(0.0f, static_cast<float>(m_rows.size()) - visible));
    m_scroll += (target - m_scroll) * (1.0f - std::exp(-16.0f * dt));
    dl->PushClipRect(ImVec2(x0, top - 10), ImVec2(x0 + panel_w, bottom + 10), true);
    for (size_t i = 0; i < m_rows.size(); i++)
    {
      const Row& row = m_rows[i];
      const float y = top + (static_cast<float>(i) - m_scroll) * row_h;
      if (y < top - row_h || y > bottom)
        continue;
      if (row.header)
      {
        if (row.label.empty())
          continue;
        dl->AddText(bold, 26, ImVec2(x0 + 70, y + 16), accent, T(row.label.c_str()));
        dl->AddLine(ImVec2(x0 + 70, y + 52), ImVec2(x0 + panel_w - 60, y + 52),
                    IM_COL32(196, 170, 255, static_cast<int>(70 * a)), 1.5f);
        continue;
      }
      const bool sel = static_cast<int>(i) == m_selected;
      if (sel)
      {
        dl->AddRectFilled(ImVec2(x0 + 40, y - 4), ImVec2(x0 + panel_w - 40, y + 54),
                          IM_COL32(160, 120, 255, static_cast<int>(80 * a)), 12);
      }
      dl->AddText(sel ? bold : regular, sel ? 36.0f : 32.0f, ImVec2(x0 + 70, y + 4),
                  sel ? white : dim, T(row.label.c_str()));
      if (row.tag != 0)
      {
        const char* label = row.tag > 0 ? "ON" : "OFF";
        const float tw = bold->CalcTextSizeA(24, FLT_MAX, 0, T(label)).x;
        const float right = x0 + panel_w - 80, w = tw + 30;
        const ImU32 fill = row.tag > 0 ? IM_COL32(40, 170, 90, static_cast<int>(230 * a)) :
                                         IM_COL32(210, 55, 70, static_cast<int>(230 * a));
        dl->AddRectFilled(ImVec2(right - w, y + 8), ImVec2(right, y + 44), fill, 18);
        dl->AddText(bold, 24, ImVec2(right - w + 15, y + 13), white, T(label));
      }
      else if (!row.value.empty())
      {
        const float tw = regular->CalcTextSizeA(30, FLT_MAX, 0, T(row.value.c_str())).x;
        const ImU32 colour = row.note ? IM_COL32(255, 210, 120, static_cast<int>((sel ? 255 : 170) * a)) :
                             sel      ? white :
                                        dim;
        dl->AddText(regular, 30, ImVec2(x0 + panel_w - 80 - tw, y + 7), colour, T(row.value.c_str()));
      }
    }
    dl->PopClipRect();
    // What the selected line does, small, under the list.
    if (m_selected >= 0 && m_selected < static_cast<int>(m_rows.size()))
    {
      const std::string help = RowHelp(m_rows[m_selected]);
      if (!help.empty())
        dl->AddText(regular, 24, ImVec2(x0 + 70, size.y - 255.0f), dim, help.c_str(), nullptr, panel_w - 140);
    }

    // The selected slot's picture and date, to the right of the panel.
    if (m_picture_slot >= 0)
    {
      const float pw = 600.0f, ph = pw / m_picture_aspect;
      const ImVec2 p0(x0 + panel_w + 120, size.y * 0.5f - ph * 0.5f - 30), p1(p0.x + pw, p0.y + ph);
      dl->AddRectFilled(ImVec2(p0.x - 6, p0.y - 6), ImVec2(p1.x + 6, p1.y + 6),
                        IM_COL32(0, 0, 0, static_cast<int>(200 * a)), 10);
      if (m_picture != ImTextureID{})
      {
        dl->AddImage(ImTextureRef(m_picture), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, static_cast<int>(255 * a)));
      }
      else
      {
        const char* empty = "Empty slot";
        const float tw = bold->CalcTextSizeA(34, FLT_MAX, 0, T(empty)).x;
        dl->AddText(bold, 34, ImVec2((p0.x + p1.x) * 0.5f - tw * 0.5f, (p0.y + p1.y) * 0.5f - 20),
                    dim, T(empty));
      }
      dl->AddRect(ImVec2(p0.x - 6, p0.y - 6), ImVec2(p1.x + 6, p1.y + 6), accent, 10, 0, 2.0f);
      const std::string caption =
          PS4Lang::T("Slot " + std::to_string(m_slot)) + "   " +
          DateText(Saves::Modified(Saves::StatePath(m_game_id, m_slot)));
      dl->AddText(bold, 30, ImVec2(p0.x, p1.y + 24), white, caption.c_str());
    }

    // Button hints.
    float hx = x0 + 70;
    const float hy = size.y - 90;
    auto hint = [&](const char* glyph, const char* text) {
      hx += PS4Glyphs::Draw(dl, bold, glyph, hx, hy, 36, a) + 12;
      dl->AddText(regular, 28, ImVec2(hx, hy - 17), dim, T(text));
      hx += regular->CalcTextSizeA(28, FLT_MAX, 0, T(text)).x + 40;
    };
    if (m_page == Page::Main)
    {
      hint("Cross", "Select");
      if (m_main_selected == MAIN_SAVE || m_main_selected == MAIN_LOAD)
        hint("D-Pad", "Slot");
      hint("Circle", "Resume");
    }
    else
    {
      hint("Cross", m_page == Page::Cheats ? "On/Off" : "Change");
      hint("D-Pad", "Move");
      hint("Circle", "Back");
    }
  }

  if (m_toast_time > 0.0f && !m_toast.empty())
  {
    const float fade = std::min(1.0f, m_toast_time / 0.3f);
    const char* text = T(m_toast.c_str());
    const float tw = bold->CalcTextSizeA(32, FLT_MAX, 0, text).x;
    const ImVec2 p(size.x - tw - 120, size.y - 150);
    dl->AddRectFilled(ImVec2(p.x - 30, p.y - 18), ImVec2(p.x + tw + 30, p.y + 56),
                      IM_COL32(26, 12, 52, static_cast<int>(225 * fade)), 14);
    dl->AddText(bold, 32, p, IM_COL32(255, 255, 255, static_cast<int>(255 * fade)), text);
  }

  // "L3 + R3: Menu", from 3 s to 10 s after the game started.
  const float hint_alpha = m_hint_time < 3.0f || m_hint_time > 10.0f ?
                               0.0f :
                               std::min({1.0f, (m_hint_time - 3.0f) / 0.4f, (10.0f - m_hint_time) / 0.6f});
  if (hint_alpha > 0.0f && a < 0.5f && m_toast_time <= 0.0f)
  {
    const char* label = T("Menu");
    const float ts = 30, gl = PS4Glyphs::Width("L3", 40), gr = PS4Glyphs::Width("R3", 40);
    const float tw = bold->CalcTextSizeA(ts, FLT_MAX, 0, label).x;
    const float pad = 22, gap = 14;
    const float right = size.x - 60, bottom = size.y - 60, height = 70;
    const float left = right - pad - tw - gap - gr - 30 - gl - pad;
    const auto alpha = [hint_alpha](int v) { return static_cast<int>(v * hint_alpha); };
    dl->AddRectFilled(ImVec2(left, bottom - height), ImVec2(right, bottom),
                      IM_COL32(26, 12, 52, alpha(215)), 16);
    dl->AddRect(ImVec2(left, bottom - height), ImVec2(right, bottom),
                IM_COL32(196, 170, 255, alpha(90)), 16, 0, 1.5f);
    float x = left + pad;
    x += PS4Glyphs::Draw(dl, bold, "L3", x, bottom - height * 0.5f, 40, hint_alpha);
    dl->AddText(bold, 30, ImVec2(x + 6, bottom - height * 0.5f - 17), IM_COL32(255, 255, 255, alpha(255)),
                "+");
    x += 30;
    x += PS4Glyphs::Draw(dl, bold, "R3", x, bottom - height * 0.5f, 40, hint_alpha) + gap;
    dl->AddText(bold, ts, ImVec2(x, bottom - height * 0.5f - ts * 0.55f),
                IM_COL32(255, 255, 255, alpha(255)), label);
  }
}
