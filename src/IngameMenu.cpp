// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4IngameMenu.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "IngameMenu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>

#include <orbis/Pad.h>

#include "Achievements.h"
#include "AchievementsUi.h"
#include "Controls.h"
#include "Display.h"
#include "Image.h"
#include "Log.h"
#include "MenuNav.h"
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

// An achievement's date in the app's language (00.76): numbers outside English (the months' names are English).
std::string AchievementDate(int64_t when)
{
  if (when <= 0 || PS4Lang::Current().code == std::string("en"))
    return DateText(when);
  const std::time_t then = static_cast<std::time_t>(when);
  std::tm day = {};
  localtime_r(&then, &day);
  char text[48];
  std::strftime(text, sizeof(text), "%d/%m/%Y  %H:%M", &day);
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
  m_capture = false;
  m_page = Page::Main;
  m_main_selected = 0;
  m_disc = m_disc_in;
  BuildRows();
  // Closed by the Arcade Button last time: open on it again (the operator menu's SERVICE presses, one after another,
  // are L3 + R3 and Cross each).
  if (m_arcade_again)
  {
    for (size_t i = 0; i < m_rows.size(); i++)
    {
      if (m_rows[i].key == "arcade")
        m_main_selected = m_selected = static_cast<int>(i);
    }
    m_arcade_again = false;
  }
  LogLine("menu: opened");
}

void IngameMenu::Close()
{
  m_open = false;
  m_capture = false;
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
    // A NAOMI's or Atomiswave's buttons inside the cabinet: TEST (the operator menu) and SERVICE, pressed for a
    // moment back in the game (Emulator.cpp); none of the DualShock 4's presses them unless the player chose one.
    if (IsFlycastBoard(m_system))
      rows.push_back({"Arcade Button", m_arcade_test ? "TEST" : "SERVICE", false, false, 0, "arcade"});
    rows.push_back({"Settings", "", false, false, 0, "settings"});
    rows.push_back({"Controls", "", false, false, 0, "controls"});
    rows.push_back({"Cheats", std::to_string(m_cheats.Entries().size()), false, false, 0, "cheats"});
    // RetroAchievements (00.73): this game's achievements, when someone is logged in.
    {
      const Achievements::Summary ra = Achievements::GetSummary();
      using State = Achievements::GameState;
      if (ra.state != State::None)
      {
        const std::string value = ra.state == State::Active  ? std::to_string(ra.unlocked) + " / " + std::to_string(ra.total) :
                                  ra.state == State::Waiting ? std::string("Loading...") :
                                  ra.state == State::NoAchievements ? std::string("None yet") :
                                  ra.state == State::Unknown        ? std::string("Not known") :
                                  ra.state == State::Offline        ? std::string("Offline") :
                                                                      std::string("Not available");
        rows.push_back({"Achievements", value, false, false, 0, "achievements"});
      }
    }
    rows.push_back({"Reset Game", "", false, false, 0, "reset"});
    rows.push_back({"Quit Game", "", false, false, 0, "quit"});
    m_selected = m_main_selected;
  }
  else if (m_page == Page::Cheats && m_system == SystemId::DC)
  {
    rows.push_back({"The Dreamcast's emulator takes no cheat codes", "", true});
  }
  else if (m_page == Page::Cheats && IsFlycastBoard(m_system))
  {
    rows.push_back({"The arcade board's emulator takes no cheat codes", "", true});
  }
  else if (m_page == Page::Cheats && m_system == SystemId::THREEDS)
  {
    rows.push_back({"The Nintendo 3DS emulator takes no cheat codes", "", true});
  }
  else if (m_page == Page::Cheats && m_system == SystemId::ARCADE)
  {
    // (libretro's cheat collection doesn't fit FinalBurn Neo; its own cheats come in a later version.)
    rows.push_back({"The arcade emulator's cheats come in a later version", "", true});
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
      rows.push_back({"Downloading the cheat collection: " + std::to_string(m_finder.DownloadedMb()) +
                          (m_finder.TotalMb() > 0 ? " of " + std::to_string(m_finder.TotalMb()) : std::string()) + " MB",
                      "", true});
    else if (state == State::Searching || state == State::Idle)
      rows.push_back({"Looking for this game's cheats...", "", true});
    else if (state == State::Failed)
    {
      rows.push_back({m_finder.Error(), "", true});
      // Why (00.68: the plain reason), over as many lines as the panel's width takes (the headers' bold 26 px).
      const std::string reason = m_finder.Reason().empty() ? std::string() : PS4Lang::T("Why: " + m_finder.Reason());
      std::string current;
      std::istringstream words(reason);
      for (std::string word; words >> word;)
      {
        const std::string longer = current.empty() ? word : current + " " + word;
        if (!current.empty() && m_bold && m_bold->CalcTextSizeA(26, FLT_MAX, 0, longer.c_str()).x > 700.0f)
        {
          rows.push_back({current, "", true});
          current = word;
        }
        else
        {
          current = longer;
        }
      }
      if (!current.empty())
        rows.push_back({current, "", true});
      rows.push_back({"Circle, then Find Cheats Online: another try", "", true});
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
  else if (m_page == Page::Achievements)
  {
    AchievementRows(&rows);
  }
  else if (m_page == Page::Controls)
  {
    // This console's buttons, for every game of the console or for this game only (Apply To), as the menu's
    // Controls: kept in controls.ini (src/Controls.h), the game's layout made again when the menu closes
    // (Emulator.cpp), the presses that close it kept from the game (Controls::MenuRelease).
    const bool own = Controls::HasGameProfile(m_game_id);
    const std::string edit = own ? m_game_id : std::string();
    rows.push_back({"Apply To", own ? "This Game" : "All Games", false, false, 0, "__ctlscope__"});
    rows.push_back({GetSystem(m_system).name, "DualShock 4", true});
    const auto& bindings = Controls::Bindings(m_system);
    for (size_t i = 0; i < bindings.size(); i++)
    {
      const std::string value = Controls::Get(bindings[i], edit, m_system);
      const bool waiting = m_capture && m_capture_row == static_cast<int>(rows.size());
      rows.push_back({bindings[i].label,
                      waiting                  ? std::string("Press a button...") :
                      bindings[i].stick_group ? value :
                                                 Controls::Describe(value),
                      false, waiting, 0, "ctl:" + std::to_string(i)});
    }
    // The buttons that open this menu (00.68), for every game.
    rows.push_back({"Every Game", "", true});
    if (const Settings::Setting* menu_buttons = Settings::Find("menu_buttons"))
      rows.push_back({menu_buttons->label, Settings::LabelOf(*menu_buttons, Settings::Get("menu_buttons")), false, false,
                      0, "__menubuttons__"});
    rows.push_back({"", "", true});
    rows.push_back({"Reset to Default", "", false, false, 0, "__ctlreset__"});
  }
  else
  {
    Settings::UseConsoleByDefault(m_game_id);  // (This Console unless the player chose otherwise)
    const Settings::Scope scope = Settings::ScopeOf(m_game_id);
    rows.push_back({"Apply To",
                    scope == Settings::Scope::Game      ? "This Game" :
                    scope == Settings::Scope::Console   ? "This Console" :
                                                          "All Games",
                    false, false, 0, "__scope__"});
    if (scope == Settings::Scope::Game)
      rows.push_back({"Use for All Games", "", false, false, 0, "__toall__"});
    // (The 3DS: its own resolution only, 1x, from 00.64 to 00.70, which a second row said; the Quality levels again
    // since 00.71. Flycast's consoles had one fixed level, Standard, from 00.53 to 00.67: the levels again since 00.68.)
    if (m_system == SystemId::THREEDS)
      rows.push_back({"Nintendo 3DS: experimental", "", true});
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

void IngameMenu::PushWrapped(std::vector<Row>* rows, const std::string& text, bool translate)
{
  // (The cheats' reason's way: the words onto lines of 700 px at the headers' bold 26.)
  const std::string shown = translate ? PS4Lang::T(text) : text;
  auto push = [rows](const std::string& line) {
    Row row{line, "", true};
    row.raw = true;
    rows->push_back(row);
  };
  std::string current;
  std::istringstream words(shown);
  for (std::string word; words >> word;)
  {
    const std::string longer = current.empty() ? word : current + " " + word;
    if (!current.empty() && m_bold && m_bold->CalcTextSizeA(26, FLT_MAX, 0, longer.c_str()).x > 700.0f)
    {
      push(current);
      current = word;
    }
    else
    {
      current = longer;
    }
  }
  if (!current.empty())
    push(current);
}

// The Achievements page (00.73): the game's achievements by rc_client's groups (Recently Unlocked, Locked,
// Unlocked...), each with its picture and points, its description under the list. (BuildRows:
// the emulation thread, the rich presence's message with it.)
void IngameMenu::AchievementRows(std::vector<Row>* rows)
{
  using State = Achievements::GameState;
  const Achievements::Summary ra = Achievements::GetSummary(true);
  // The session's state first (00.75): connected or not; offline, the unlocks wait (rc_client sends them later).
  const Achievements::Status status = Achievements::GetStatus();
  PushWrapped(rows, Achievements::StatusLine(status));
  if (status.session == Achievements::Session::Offline && ra.state == State::Active)
    PushWrapped(rows, "The unlocks wait and are sent when the connection comes back (while EmuSuNi stays open).");
  if (ra.state != State::Active)
  {
    PushWrapped(rows, ra.state == State::Waiting ? std::string("RetroAchievements: this game's achievements are loading...") :
                      ra.note.empty()            ? std::string("RetroAchievements: no achievements for this game") :
                                                   ra.note);
    return;
  }
  auto header = [rows](const std::string& text) {
    Row row{text, "", true};
    row.raw = true;
    rows->push_back(row);
  };
  header(ra.title);
  header(PS4Lang::T(std::to_string(ra.unlocked) + " of " + std::to_string(ra.total) + " achievements") + "  -  " +
         PS4Lang::T(std::to_string(ra.points) + " of " + std::to_string(ra.total_points) + " points"));
  if (ra.unsupported > 0)
    PushWrapped(rows, std::to_string(ra.unsupported) + " of them can't be unlocked with this emulator yet");
  // (00.79) The texts still in English, said: being translated, or waiting for MyMemory's next free day.
  if (const std::string line = Achievements::TranslationLine(Achievements::GetTranslationState(ra.title)); !line.empty())
    PushWrapped(rows, line);
  if (!ra.rich_presence.empty())
    PushWrapped(rows, ra.rich_presence, false);
  std::string group;
  for (const Achievements::Entry& entry : Achievements::List())
  {
    if (entry.group != group)
    {
      group = entry.group;
      rows->push_back({group, "", true});  // (rc_client's labels: translated)
    }
    Row row;
    row.label = entry.title.size() > 46 ? entry.title.substr(0, 43) + "..." : entry.title;
    row.value = entry.points == 1 ? std::string("1 point") : std::to_string(entry.points) + " points";
    row.key = "ach:" + std::to_string(entry.id);
    row.badge = entry.badge;
    row.raw = true;
    row.dim = !entry.unlocked;
    row.help = entry.description;
    if (!entry.unlocked && !entry.progress.empty())
      row.help += "\n" + PS4Lang::T("Progress: " + entry.progress);
    if (entry.unlocked && entry.unlock_time > 0)
      row.help += "\n" + PS4Lang::T("Unlocked " + AchievementDate(entry.unlock_time));
    if (entry.rarity > 0.0f)
    {
      char rarity[32];
      std::snprintf(rarity, sizeof(rarity), "%.1f", entry.rarity);
      row.help += "\n" + PS4Lang::T(std::string(rarity) + "% of the players have it");
    }
    rows->push_back(std::move(row));
  }
  // (00.78: no leaderboards - Hardcore's only, rc_client runs none in Casual.)
}

std::string IngameMenu::MainKey() const
{
  if (m_page != Page::Main || m_main_selected < 0 || m_main_selected >= static_cast<int>(m_rows.size()))
    return {};
  return m_rows[m_main_selected].key;
}

void IngameMenu::Move(int direction, bool wrap)
{
  // (00.76: the first row to the last and back on a fresh press; a held arrow stops at the end, src/MenuNav.h.)
  const int count = static_cast<int>(m_rows.size());
  const int row = MenuNav::Step(m_selected, direction, count, wrap, [this](int i) { return !m_rows[i].header; });
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
  if (m_page == Page::Achievements)
    return;
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
  if (m_page == Page::Controls)
  {
    const std::string edit = Controls::HasGameProfile(m_game_id) ? m_game_id : std::string();
    if (key == "__ctlscope__")
    {
      // All Games (the console's normal layout) <-> This Game (its own, starting as a copy of it).
      if (edit.empty())
        Controls::CreateGameProfile(m_game_id, m_system);
      else
        Controls::DeleteGameProfile(m_game_id);
      Toast(edit.empty() ? "Controls for this game only" : "This game uses this console's controls");
    }
    else if (key == "__ctlreset__")
    {
      if (direction <= 0)
        return;
      Controls::ResetToDefault(edit, m_system);
      Toast("Controls back to the default");
    }
    else if (key == "__menubuttons__")
    {
      // Every game's (settings.ini [global]); the new ones close this menu already.
      if (const Settings::Setting* setting = Settings::Find("menu_buttons"))
        Settings::Set("menu_buttons", Settings::Step(*setting, Settings::Get("menu_buttons"), direction));
    }
    else if (key.rfind("ctl:", 0) == 0)
    {
      const auto& bindings = Controls::Bindings(m_system);
      const size_t index = static_cast<size_t>(std::atoi(key.c_str() + 4));
      if (index >= bindings.size())
        return;
      const Controls::Binding& binding = bindings[index];
      if (binding.stick_group)
      {
        // Which of the DualShock 4's controls move it: the choices, round and round.
        const auto choices = Controls::StickChoices();
        const std::string current = Controls::Get(binding, edit, m_system);
        int at = 0;
        for (size_t i = 0; i < choices.size(); i++)
        {
          if (choices[i] == current)
            at = static_cast<int>(i);
        }
        const int count = static_cast<int>(choices.size());
        Controls::Set(binding, choices[static_cast<size_t>(((at + direction) % count + count) % count)], edit, m_system);
      }
      else if (direction > 0)
      {
        // Cross (or Right): the next button pressed, once every button is up (Capture).
        m_capture = true;
        m_capture_released = false;
        m_capture_time = 0.0f;
        m_capture_row = m_selected;
      }
      else
      {
        Controls::Set(binding, "None", edit, m_system);  // Left: none
      }
    }
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
  if (!row.help.empty())
    return row.help;  // (an achievement's description: made for the row, translated where it is the app's)
  if (m_page == Page::Achievements)
    return std::string();
  if (m_page == Page::Main)
  {
    if (row.key == "achievements")
      return T("This game's achievements (RetroAchievements): unlocked or not, with their points.");
    if (row.key == "disc")
    {
      return PS4Lang::T("Disc " + std::to_string(m_disc_in + 1) + " is in the console.") + " " +
             T("Left / Right choose another disc, Cross puts it in (as if the console's lid were opened and "
               "closed).");
    }
    if (row.key == "arcade")
      return T("Cross presses TEST (the operator menu: coins, difficulty, the game's settings) or SERVICE back in the "
               "game; the menu opens on this line again. Left / Right: TEST or SERVICE. In the operator menu SERVICE "
               "goes down a line, TEST chooses it.");
    static const std::pair<const char*, const char*> MAIN[] = {
        {"Resume Game", "Back to the game."},
        {"Save State", "Saves this very moment of the game in the slot (Left / Right choose the slot)."},
        {"Load State", "Goes back to the moment saved in the slot (Left / Right choose the slot)."},
        {"Settings", "Picture, sound and emulation settings, for this game or for all games."},
        {"Controls", "Which DualShock 4 button does what, for this console's games or only this one: back in the "
                     "game they apply."},
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
  if (m_page == Page::Controls)
  {
    // (The menu's own texts: src/Menu.cpp DrawSidePanel.)
    if (m_capture)
      return PS4Lang::T("Press the DualShock 4 button for " + row.label + "...  (5 s: cancel)");
    if (row.key == "__ctlscope__")
      return T("All Games: the normal layout. This Game: its own.");
    if (row.key == "__ctlreset__")
      return T("Back to the default layout.");
    if (row.key == "__menubuttons__")
    {
      // The setting's help, then the choice's.
      const Settings::Setting* setting = Settings::Find("menu_buttons");
      const Settings::Choice* choice = setting ? Settings::FindChoice(*setting, Settings::Get("menu_buttons")) : nullptr;
      return setting ? T(setting->help) + (choice && choice->help ? "\n" + T(choice->label) + ": " + T(choice->help) : "") :
                       std::string();
    }
    const auto& bindings = Controls::Bindings(m_system);
    const size_t index = static_cast<size_t>(std::atoi(row.key.c_str() + 4));
    if (row.key.rfind("ctl:", 0) != 0 || index >= bindings.size())
      return {};
    const std::string key = bindings[index].key;
    if (key == "fast_forward")
      return T("Cross: press a new button. Left: none. Held, the game runs faster (how fast: Fast Forward in the "
               "game's settings). A button the console uses too does both.");
    if (IsFlycastBoard(m_system))
    {
      if (key == "l3")
        return T("Cross: press a new button. Left: none. TEST opens the board's operator menu (coins, difficulty, "
                 "the game's settings); the Arcade Button of the L3+R3 menu presses it too.");
      if (key == "r3")
        return T("Cross: press a new button. Left: none. SERVICE gives a credit; in the operator menu it moves to "
                 "the next line (TEST chooses it).");
      if (key == "reload")
        return T("Cross: press a new button. Left: none. A light gun game's reload (the gun shot off the screen); "
                 "the left stick aims.");
      if (bindings[index].stick_group)
        return T("D-Pad: Left/Right to choose. A game with a wheel, a flight stick or a gun has that on the left "
                 "stick, not the D-pad (unless Left Stick is chosen).");
    }
    if (bindings[index].stick_group)
      return T("Left / Right: which DualShock 4 controls move it.");
    if (m_system == SystemId::ARCADE)
      return T("Cross: press a new button. Left: none. Neo Geo games: A, B, C, D are Buttons 3, 1, 4, 2 (Square, "
               "Cross, Triangle, Circle). Six-button fighters: punches 3, 4, 6; kicks 1, 2, 5.");
    if (m_system == SystemId::NAOMI)
      return T("Cross: press a new button. Left: none. Fighting games: punches 1, 2, 3, kicks 4, 5, 6 (as on a "
               "cabinet: punches on Square, Triangle, R1, kicks on Cross, Circle, R2).");
    if (m_system == SystemId::ATOMISWAVE)
      return T("Cross: press a new button. Left: none. Fighting games: punches 1 and 2, kicks 4 and 5, 3 their "
               "special move.");
    return T("Cross: press a new button for it. Left: none. D-Pad: Left/Right to choose.");
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
  // Fast Forward: the button that does it in this game (its controls or its console's: the Controls page).
  std::string help = row.key == "fast_forward" ? PS4Lang::T(Controls::FastForwardHelp(m_game_id, m_system)) :
                                                 T(setting->help);
  if (!setting->live)
    help += " " + T("(Next game start.)");
  const Settings::Choice* choice = Settings::FindChoice(*setting, Settings::Get(row.key, m_game_id));
  // Quality: what the level does on this game's console.
  const std::string level = choice && row.key == "quality" ? Settings::QualityHelp(choice->value, m_system) : "";
  if (!level.empty())
    help += "\n" + T(choice->label) + ": " + PS4Lang::T(level);
  else if (choice && choice->help)
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
  if (key == "arcade")
  {
    Close();
    m_arcade_again = true;
    return Action::ArcadeButton;
  }
  if (key == "settings" || key == "cheats" || key == "controls" || key == "achievements")
  {
    m_page = key == "settings"     ? Page::Settings :
             key == "controls"     ? Page::Controls :
             key == "achievements" ? Page::Achievements :
                                     Page::Cheats;
    m_selected = 0;
    m_achievements_refresh = 1.0f;
    return Action::None;
  }
  if (key == "reset")
    return Action::Reset;
  if (key == "quit")
    return Action::Quit;
  return Action::None;
}

// The Controls page's capture: once every button is up, the first one pressed is the row's new button (5 s
// without one: nothing changes).
void IngameMenu::Capture(uint32_t held, float dt)
{
  m_capture_time += dt;
  OrbisPadData data = {};
  data.buttons = held & Controls::PAD_BUTTONS;
  if (!m_capture_released)
  {
    m_capture_released = Controls::IsAtRest(data);  // (the Cross that asked for it is let go first)
    return;
  }
  const auto& bindings = Controls::Bindings(m_system);
  const size_t index = m_capture_row >= 0 && m_capture_row < static_cast<int>(m_rows.size()) ?
                           static_cast<size_t>(std::atoi(m_rows[static_cast<size_t>(m_capture_row)].key.c_str() + 4)) :
                           bindings.size();
  if (m_capture_time > 5.0f || index >= bindings.size())
  {
    m_capture = false;
    Toast("No button pressed: nothing changed");
    BuildRows();
    return;
  }
  const std::string control = Controls::Capture(data);
  if (control.empty())
    return;
  Controls::Set(bindings[index], control, Controls::HasGameProfile(m_game_id) ? m_game_id : std::string(), m_system);
  m_capture = false;
  Toast(PS4Lang::T(bindings[index].label) + std::string(": ") + PS4Lang::T(Controls::Describe(control)));
  BuildRows();
}

IngameMenu::Action IngameMenu::Poll(uint32_t pressed, uint32_t held, float dt)
{
  if (!m_open)
    return Action::None;
  if (m_capture)
  {
    // Waiting for a button (the Controls page): every press is that, none the menu's - not L3 + R3, not Circle.
    Capture(held, dt);
    return Action::None;
  }
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
  // (A fresh press goes round the list; the repeat of one held stops at its end.)
  if (nav & ORBIS_PAD_BUTTON_UP)
  {
    Move(-1, (pressed & ORBIS_PAD_BUTTON_UP) != 0);
    rebuild = true;
  }
  if (nav & ORBIS_PAD_BUTTON_DOWN)
  {
    Move(1, (pressed & ORBIS_PAD_BUTTON_DOWN) != 0);
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
      else if (key == "arcade")
        m_arcade_test = !m_arcade_test;
    }
    else
    {
      Change(direction);
    }
    rebuild = true;
  }
  constexpr uint32_t COMBO = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
  // The menu's own buttons (Controls -> In-Game Menu, 00.68) close it too, pressed (a hold's needn't be held).
  const Controls::MenuButtons buttons = Controls::MenuButtonsFor(Settings::Get("menu_buttons"), m_system);
  if (pressed & ORBIS_PAD_BUTTON_CROSS)
  {
    action = Activate();
    rebuild = true;
  }
  else if (((held & COMBO) == COMBO && (pressed & COMBO)) ||
           (buttons.mask != 0 && (held & buttons.mask) == buttons.mask && (pressed & buttons.mask)))
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
  if (m_page == Page::Achievements)
  {
    // (Made again each second: the game's achievements may come while it is shown.)
    m_achievements_refresh -= dt;
    if (m_achievements_refresh <= 0.0f)
    {
      m_achievements_refresh = 1.0f;
      rebuild = true;
    }
    // (00.79) The achievements on screen translated first: the selected one, then those around it.
    std::vector<uint32_t> ids;
    auto add = [&](int i) {
      if (i >= 0 && i < static_cast<int>(m_rows.size()) && m_rows[i].key.rfind("ach:", 0) == 0)
        ids.push_back(static_cast<uint32_t>(std::strtoul(m_rows[i].key.c_str() + 4, nullptr, 10)));
    };
    add(m_selected);
    for (int d = 1; d <= 10; d++)
    {
      add(m_selected + d);
      if (d <= 5)
        add(m_selected - d);
    }
    Achievements::TranslateFirst(ids);
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
                           m_page == Page::Achievements ? "ACHIEVEMENTS" :
                           m_page == Page::CheatSearch ? "FIND CHEATS ONLINE" :
                           m_page == Page::CheatPick   ? "CHOOSE CHEATS" :
                           m_page == Page::Controls    ? (Controls::HasGameProfile(m_game_id) ? "CONTROLS - THIS GAME" :
                                                                                               "CONTROLS - ALL GAMES") :
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
        dl->AddText(bold, 26, ImVec2(x0 + 70, y + 16), accent, row.raw ? row.label.c_str() : T(row.label.c_str()));
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
      // An achievement's picture before its name (RetroAchievements, 00.73: a locked one's grey picture, dimmer).
      float label_x = x0 + 70;
      if (!row.badge.empty())
      {
        const ImTextureID picture = AchievementsUi::Picture(*m_display, row.badge);
        if (picture != ImTextureID{})
          dl->AddImage(ImTextureRef(picture), ImVec2(label_x, y + 2), ImVec2(label_x + 48, y + 50), ImVec2(0, 0),
                       ImVec2(1, 1), IM_COL32(255, 255, 255, static_cast<int>((row.dim ? 170 : 255) * a)));
        else
          dl->AddRectFilled(ImVec2(label_x, y + 2), ImVec2(label_x + 48, y + 50),
                            IM_COL32(255, 255, 255, static_cast<int>(30 * a)), 6);
        label_x += 62;
      }
      const ImU32 label_colour = row.dim && !sel ? IM_COL32(255, 255, 255, static_cast<int>(120 * a)) : sel ? white : dim;
      // (A long name of RetroAchievements' stops before the value.)
      const float value_w = row.raw && !row.value.empty() ?
                                regular->CalcTextSizeA(30, FLT_MAX, 0, T(row.value.c_str())).x + 24.0f :
                                0.0f;
      if (row.raw)
        dl->PushClipRect(ImVec2(label_x, y - 4), ImVec2(x0 + panel_w - 80 - value_w, y + 56), true);
      dl->AddText(sel ? bold : regular, sel ? 36.0f : 32.0f, ImVec2(label_x, y + 4), label_colour,
                  row.raw ? row.label.c_str() : T(row.label.c_str()));
      if (row.raw)
        dl->PopClipRect();
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
      hint("Cross", key == "disc" ? "Put In" : key == "arcade" ? "Press" : "Select");
      if (key == "save" || key == "load")
        hint("D-Pad", "Slot");
      else if (key == "disc")
        hint("D-Pad", "Disc");
      else if (key == "arcade")
        hint("D-Pad", "TEST / SERVICE");
      hint("Circle", "Resume");
    }
    else
    {
      hint("Cross", m_page == Page::Cheats      ? "On/Off" :
                    m_page == Page::CheatSearch ? "Select" :
                    m_page == Page::CheatPick   ? "Choose" :
                    m_page == Page::Achievements ? "Select" :
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
    // The menu's buttons as chosen (Controls -> In-Game Menu, 00.68): "L3 + R3", "R3 (hold)", "Touchpad (hold)",
    // "Options + Touchpad"; 00.69: "L1 + R1 (hold)", "L2 + R2 (hold)".
    const Controls::MenuButtons buttons = Controls::MenuButtonsFor(Settings::Get("menu_buttons"), m_system);
    const char* first = buttons.mask & ORBIS_PAD_BUTTON_L3 ? "L3" :
                        buttons.mask & ORBIS_PAD_BUTTON_OPTIONS ? "Options" :
                        buttons.mask & ORBIS_PAD_BUTTON_L1 ? "L1" :
                        buttons.mask & ORBIS_PAD_BUTTON_L2 ? "L2" :
                        buttons.mask & ORBIS_PAD_BUTTON_R3 ? "R3" :
                                                              "Touchpad";
    const char* second = buttons.mask & ORBIS_PAD_BUTTON_L3 ? "R3" :
                         buttons.mask & ORBIS_PAD_BUTTON_OPTIONS ? "Touchpad" :
                         buttons.mask & ORBIS_PAD_BUTTON_L1 ? "R1" :
                         buttons.mask & ORBIS_PAD_BUTTON_L2 ? "R2" :
                                                                   nullptr;
    const bool held = buttons.hold > 0.0f;  // "(hold)" after the buttons
    const char* label = T("Menu");
    const char* hold = T("(hold)");
    const float ts = 30, g1 = PS4Glyphs::Width(first, 40), g2 = second ? PS4Glyphs::Width(second, 40) : 0.0f;
    const float tw = bold->CalcTextSizeA(ts, FLT_MAX, 0, label).x;
    const float hw = held ? bold->CalcTextSizeA(26, FLT_MAX, 0, hold).x + 12 : 0.0f;
    const float pad = 22, gap = 14;
    const float right = size.x - 60, bottom = size.y - 60, height = 70;
    const float left = right - pad - tw - gap - g2 - (second ? 30 : 0) - hw - g1 - pad;
    const auto alpha = [hint_alpha](int v) { return static_cast<int>(v * hint_alpha); };
    dl->AddRectFilled(ImVec2(left, bottom - height), ImVec2(right, bottom),
                      IM_COL32(26, 12, 52, alpha(215)), 16);
    dl->AddRect(ImVec2(left, bottom - height), ImVec2(right, bottom),
                IM_COL32(196, 170, 255, alpha(90)), 16, 0, 1.5f);
    float x = left + pad;
    x += PS4Glyphs::Draw(dl, bold, first, x, bottom - height * 0.5f, 40, hint_alpha);
    if (second)
    {
      dl->AddText(bold, 30, ImVec2(x + 6, bottom - height * 0.5f - 17), IM_COL32(255, 255, 255, alpha(255)), "+");
      x += 30;
      x += PS4Glyphs::Draw(dl, bold, second, x, bottom - height * 0.5f, 40, hint_alpha);
    }
    if (held)
    {
      dl->AddText(bold, 26, ImVec2(x + 12, bottom - height * 0.5f - 15), IM_COL32(255, 255, 255, alpha(220)), hold);
      x += hw;
    }
    x += gap;
    dl->AddText(bold, ts, ImVec2(x, bottom - height * 0.5f - ts * 0.55f),
                IM_COL32(255, 255, 255, alpha(255)), label);
  }
}
