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
                         const std::string& title, SystemId system, const std::string& game_path)
{
  m_data_dir = data_dir;
  m_game_id = game_id;
  m_title = title;
  m_system = system;
  m_game_path = game_path;
  m_finder.Stop();
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

void IngameMenu::SetDiscs(int count, int inserted)
{
  m_disc_count = count;
  m_disc_in = std::clamp(inserted, 0, std::max(0, count - 1));
}

void IngameMenu::Open()
{
  m_open = true;
  m_page = Page::Main;
  m_main_selected = 0;
  m_disc = m_disc_in;
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
    rows.push_back({"Resume Game", "", false, false, 0, "resume"});
    rows.push_back({"Save State", SlotLabel(m_slot), false, false, 0, "save"});
    rows.push_back({"Load State", SlotLabel(m_slot), false, false, 0, "load"});
    if (m_disc_count > 1)
    {
      rows.push_back({"Disc", "Disc " + std::to_string(m_disc + 1) + " of " + std::to_string(m_disc_count), false,
                      false, 0, "disc"});
    }
    rows.push_back({"Settings", "", false, false, 0, "settings"});
    rows.push_back({"Cheats", std::to_string(m_cheats.Entries().size()), false, false, 0, "cheats"});
    rows.push_back({"Reset Game", "", false, false, 0, "reset"});
    rows.push_back({"Quit Game", "", false, false, 0, "quit"});
    m_selected = m_main_selected;
  }
  else if (m_page == Page::Cheats && m_system == SystemId::DC)
  {
    rows.push_back({"The Dreamcast's emulator takes no cheat codes", "", true});
  }
  else if (m_page == Page::Cheats)
  {
    const auto& entries = m_cheats.Entries();
    rows.push_back({"Find Cheats Online", "", false, false, 0, "cheatfind"});
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
  else if (m_page == Page::CheatSearch)
  {
    using State = CheatDownload::Finder::State;
    const State state = m_finder.GetState();
    if (state == State::Downloading)
      rows.push_back({"Downloading the cheat collection: " + std::to_string(m_finder.DownloadedMb()) + " MB", "", true});
    else if (state == State::Searching || state == State::Idle)
      rows.push_back({"Looking for this game's cheats...", "", true});
    else if (state == State::Failed)
    {
      rows.push_back({m_finder.Error(), "", true});
      rows.push_back({"Check the PS4's internet connection", "", true});
    }
    else if (m_finder.Results().empty())
      rows.push_back({"No cheats found for this game", "", true});
    else
    {
      rows.push_back({"Cheat lists (libretro)", "", true});
      const auto& results = m_finder.Results();
      for (size_t i = 0; i < results.size(); i++)
        rows.push_back({results[i], "", false, false, 0, "cheatfile:" + std::to_string(i)});
    }
  }
  else if (m_page == Page::CheatPick)
  {
    rows.push_back({"Add the Chosen Cheats", std::to_string(m_finder.PickedCount()), false, false, 0, "pickadd"});
    rows.push_back({"Choose All", "", false, false, 0, "pickall:on"});
    rows.push_back({"Choose None", "", false, false, 0, "pickall:off"});
    rows.push_back({m_finder.OpenName(), "", true});
    const auto& codes = m_finder.Codes();
    for (size_t i = 0; i < codes.size(); i++)
    {
      if (m_finder.IsTitle(i))
        rows.push_back({codes[i].name, "", true});
      else
        rows.push_back({codes[i].name, "", false, false, m_finder.Picked(i) ? 1 : -1, "pick:" + std::to_string(i)});
    }
  }
  else
  {
    const Settings::Scope scope = Settings::ScopeOf(m_game_id);
    rows.push_back({"Apply To",
                    scope == Settings::Scope::Game      ? "This Game" :
                    scope == Settings::Scope::Console   ? "This Console" :
                                                          "All Games",
                    false, false, 0, "__scope__"});
    if (scope == Settings::Scope::Game)
      rows.push_back({"Use for All Games", "", false, false, 0, "__toall__"});
    if (m_system == SystemId::DC)
      rows.push_back({"Dreamcast: Low quality only", "", true});  // (Emulator.cpp BuildDcVariables)
    // The settings of this game's console.
    for (const char* category : Settings::Categories())
    {
      bool header = false;
      for (const Settings::Setting& setting : Settings::All())
      {
        if (std::string(setting.category) != category || std::string(setting.key) == "language" ||
            !Settings::AppliesTo(setting, m_system))
          continue;
        if (!header)
          rows.push_back({category, "", true});
        header = true;
        const std::string value = Settings::Get(setting.key, m_game_id);
        rows.push_back({setting.label, Settings::LabelOf(setting, value), false, !setting.live, 0,
                        setting.key});
      }
    }
    rows.push_back({"", "", true});
    rows.push_back({scope == Settings::Scope::Game      ? "Reset This Game" :
                    scope == Settings::Scope::Console   ? "Reset This Console" :
                                                          "Reset to Defaults",
                    "", false, false, 0, "__reset__"});
  }
  m_rows = std::move(rows);
  m_selected = std::clamp(m_selected, 0, std::max(0, static_cast<int>(m_rows.size()) - 1));
  if (m_page != Page::Main && !m_rows.empty() && m_rows[m_selected].header)
    Move(1);
}

std::string IngameMenu::MainKey() const
{
  if (m_page != Page::Main || m_main_selected < 0 || m_main_selected >= static_cast<int>(m_rows.size()))
    return {};
  return m_rows[m_main_selected].key;
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
  if (m_page == Page::Cheats && key == "cheatfind")
  {
    if (direction <= 0)
      return;
    // libretro's lists for this game (the collection downloaded first when it isn't here).
    m_page = Page::CheatSearch;
    m_selected = 0;
    m_search_selected = 0;
    m_finder.Start(m_data_dir, m_system, m_game_path, m_title);
    return;
  }
  if (m_page == Page::CheatSearch)
  {
    if (direction <= 0 || key.rfind("cheatfile:", 0) != 0)
      return;
    const size_t index = static_cast<size_t>(std::atoi(key.c_str() + 10));
    std::string error;
    if (m_finder.Open(index, &error))
    {
      m_search_selected = m_selected;
      m_page = Page::CheatPick;
      m_selected = 0;
    }
    else
    {
      Toast(error);
    }
    return;
  }
  if (m_page == Page::CheatPick)
  {
    if (key == "pickadd")
    {
      if (direction <= 0)
        return;
      if (m_finder.PickedCount() == 0)
      {
        Toast("Choose the cheats to add first");
        return;
      }
      const int added = m_cheats.Add(m_finder.PickedCodes());
      Toast(added == 1 ? std::string("1 cheat added") : std::to_string(added) + " cheats added");
      m_cheats_changed = true;
      m_page = Page::Cheats;
      m_selected = 0;
    }
    else if (key.rfind("pickall:", 0) == 0)
    {
      m_finder.PickAll(key == "pickall:on");
    }
    else if (key.rfind("pick:", 0) == 0)
    {
      const size_t index = static_cast<size_t>(std::atoi(key.c_str() + 5));
      m_finder.Pick(index, !m_finder.Picked(index));
    }
    return;
  }
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
    // All Games -> This Console -> This Game (Right), the other way round with Left.
    const Settings::Scope order[] = {Settings::Scope::Global, Settings::Scope::Console, Settings::Scope::Game};
    const Settings::Scope now = Settings::ScopeOf(m_game_id);
    const int at = now == Settings::Scope::Global ? 0 : now == Settings::Scope::Console ? 1 : 2;
    const Settings::Scope next = order[(at + (direction > 0 ? 1 : 2)) % 3];
    Settings::SetScope(m_game_id, next);
    Toast(next == Settings::Scope::Game    ? "Settings for this game only" :
          next == Settings::Scope::Console ? "Settings for this console's games" :
                                             "This game follows the global settings");
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
    if (Settings::ScopeOf(m_game_id) == Settings::Scope::Console)
    {
      Settings::ResetConsole(m_game_id);
      Toast("This console's games use the global settings again");
    }
    else if (Settings::IsCustom(m_game_id))
    {
      Settings::ResetGame(m_game_id);
      Toast("This game uses the global settings again");
    }
    else
    {
      for (const Settings::Setting& setting : Settings::All())
      {
        if (std::string(setting.key) != "language" && Settings::AppliesTo(setting, m_system))
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
  Settings::Set(key, value, m_game_id);  // where the game's settings come from (Apply To)
  if (!setting->live)
    Toast(std::string(setting->label) + ": next time a game starts");
  m_settings_changed = true;
}

std::string IngameMenu::RowHelp(const Row& row) const
{
  auto T = [](const char* text) { return std::string(PS4Lang::T(text)); };
  if (m_page == Page::Main)
  {
    if (row.key == "disc")
    {
      return PS4Lang::T("Disc " + std::to_string(m_disc_in + 1) + " is in the console.") + " " +
             T("Left / Right choose another disc, Cross puts it in (as if the console's lid were opened and "
               "closed).");
    }
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
    if (row.key == "cheatfind")
      return T("Looks for this game's cheats in libretro's collection (RetroArch's): you choose the "
               "ones to add. The collection (about 35 MB) is downloaded the first time.");
    return {};
  }
  if (m_page == Page::CheatSearch)
    return row.key.empty() ? std::string() : T("Cross: see this list's cheats and choose the ones to add.");
  if (m_page == Page::CheatPick)
  {
    if (row.key == "pickadd")
      return T("Adds the chosen cheats to this game's list, off: turn on the ones you want there.");
    if (row.key.rfind("pickall:", 0) == 0)
      return {};
    return T("Cross chooses this cheat to add, or leaves it out.");
  }
  if (row.key == "__scope__")
    return T("All Games: what you change here applies to every game. This Console: to every game of "
             "this console. This Game: only to this game.");
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
  const std::string key = MainKey();
  if (key == "resume")
  {
    Close();
    return Action::Closed;
  }
  if (key == "save")
    return Action::Save;
  if (key == "load")
  {
    if (!Saves::Modified(Saves::StatePath(m_game_id, m_slot)))
    {
      Toast("Slot " + std::to_string(m_slot) + " is empty");
      return Action::None;
    }
    return Action::Load;
  }
  if (key == "disc")
  {
    if (m_disc == m_disc_in)
    {
      Toast("Disc " + std::to_string(m_disc + 1) + " is in already");
      return Action::None;
    }
    return Action::ChangeDisc;
  }
  if (key == "settings" || key == "cheats")
  {
    m_page = key == "settings" ? Page::Settings : Page::Cheats;
    m_selected = 0;
    return Action::None;
  }
  if (key == "reset")
    return Action::Reset;
  if (key == "quit")
    return Action::Quit;
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
      const std::string key = MainKey();
      if (key == "save" || key == "load")
        m_slot = (m_slot - 1 + direction + Saves::SLOTS) % Saves::SLOTS + 1;
      else if (key == "disc" && m_disc_count > 1)
        m_disc = (m_disc + direction + m_disc_count) % m_disc_count;
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
    if (m_page == Page::CheatPick)
    {
      m_page = Page::CheatSearch;  // back to the lists, on the one chosen
      m_selected = m_search_selected;
      rebuild = true;
    }
    else if (m_page == Page::CheatSearch)
    {
      m_finder.Stop();
      m_page = Page::Cheats;
      m_selected = 0;
      rebuild = true;
    }
    else if (m_page != Page::Main)
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
  if (m_page == Page::CheatSearch)
  {
    const auto state = m_finder.GetState();
    const int mb = m_finder.DownloadedMb();
    if (state != m_finder_state || mb != m_finder_mb)
    {
      m_finder_state = state;
      m_finder_mb = mb;
      rebuild = true;
    }
  }
  if (rebuild && m_open)
    BuildRows();
  return action;
}

void IngameMenu::UpdatePicture()
{
  const std::string key = m_open ? MainKey() : std::string();
  const bool want = key == "save" || key == "load";
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
                           m_page == Page::CheatSearch ? "FIND CHEATS ONLINE" :
                           m_page == Page::CheatPick   ? "CHOOSE CHEATS" :
                           Settings::ScopeOf(m_game_id) == Settings::Scope::Game    ? "SETTINGS - THIS GAME" :
                           Settings::ScopeOf(m_game_id) == Settings::Scope::Console ? "SETTINGS - THIS CONSOLE" :
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
      const std::string key = MainKey();
      hint("Cross", key == "disc" ? "Put In" : "Select");
      if (key == "save" || key == "load")
        hint("D-Pad", "Slot");
      else if (key == "disc")
        hint("D-Pad", "Disc");
      hint("Circle", "Resume");
    }
    else
    {
      hint("Cross", m_page == Page::Cheats      ? "On/Off" :
                    m_page == Page::CheatSearch ? "Select" :
                    m_page == Page::CheatPick   ? "Choose" :
                                                  "Change");
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
