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
#include "GameExtras.h"  // ((01.22) a game's Updates, DLC and Mods / Patched Romset: asked of the menu after the game)
#include "Image.h"
#include "Log.h"
#include "MenuNav.h"
#include "Netplay.h"
#include "NetplayUi.h"
#include "PS4Glyphs.h"
#include "PS4Lang.h"
#include "Portable.h"
#include "Saves.h"
#include "Settings.h"
#ifdef EMUSUNI_3DS
#include "ThreeDsCia.h"
#endif

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
  m_extras_rows.clear();
  m_extras_files.clear();
  m_extras_ask.clear();
  m_extras_title = 0;
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
  // (00.86) Opened for a player asking to join our room (the game's notice said so): on Accept Player.
  if (m_join_rows && m_page == Page::Main && m_rows.size() > 1)
    m_main_selected = m_selected = 1;
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
  if (m_page == Page::Main && Netplay::Active())
  {
    // (00.23) Online: the match's own rows (no states, disc, cheats, reset or achievements: the match is every
    // console's).
    OnlineRows(&rows);
    // ((01.23) A request to join the match - entry with permission -: Accept and Reject under Resume Game; the rows
    // below keep their selection, as for a game played alone.)
    const bool join_rows = Netplay::GetJoinRequest().member >= 0;
    if (join_rows != m_join_rows)
    {
      if (join_rows && m_main_selected >= 1)
        m_main_selected += 2;
      else if (!join_rows && m_main_selected >= 3)
        m_main_selected -= 2;
      else if (!join_rows)
        m_main_selected = 0;
      m_join_rows = join_rows;
    }
    m_main_selected = std::clamp(m_main_selected, 0, static_cast<int>(rows.size()) - 1);
    m_selected = m_main_selected;
  }
  else if (m_page == Page::Main)
  {
    rows.push_back({"Resume Game", "", false, false, 0, "resume"});
    // (00.86) Our online room open while this game is played alone, a player asking to join it: its answer here.
    const Netplay::JoinRequest ask = Netplay::GetJoinRequest();
    const bool join_rows = ask.member >= 0;
    if (join_rows)
    {
      const std::string id = std::to_string(ask.member);
      Row accept{"Accept Player", ask.nick + "  " + std::to_string(ask.seconds_left) + " s", false, false, 0,
                 "np:accept:" + id};
      Row reject{"Reject Player", ask.nick, false, false, 0, "np:reject:" + id};
      rows.push_back(accept);
      rows.push_back(reject);
    }
    if (join_rows != m_join_rows)
    {
      // (The rows below keep their selection; one of these two gone: Resume Game.)
      if (join_rows && m_main_selected >= 1)
        m_main_selected += 2;
      else if (!join_rows && m_main_selected >= 3)
        m_main_selected -= 2;
      else if (!join_rows)
        m_main_selected = 0;
      m_join_rows = join_rows;
    }
#ifdef EMUSUNI_OPENBOR
    // (with-openbor) OpenBOR's core has no states, no cheats and no reset (its games save their own progress; the
    // game's own menus go back to its title): those rows aren't there.
    const bool openbor = m_system == SystemId::OPENBOR;
#else
    const bool openbor = false;
#endif
    if (!openbor)
    {
      rows.push_back({"Save State", SlotLabel(m_slot), false, false, 0, "save"});
      rows.push_back({"Load State", SlotLabel(m_slot), false, false, 0, "load"});
    }
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
    if (!openbor)
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
    // (01.22) What the game has installed: the 3DS's update, DLC and mods; the Arcade's patched romset.
    if (HasExtras())
      rows.push_back({m_system == SystemId::ARCADE ? "Patched Romset" : "Updates, DLC and Mods", "", false, false, 0,
                      "extras"});
    if (!openbor)
      rows.push_back({"Reset Game", "", false, false, 0, "reset"});
    rows.push_back({"Quit Game", "", false, false, 0, "quit"});
    m_selected = m_main_selected;
  }
  else if (m_page == Page::Extras)
  {
    ExtrasRows(&rows);
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
  else if (m_page == Page::Cheats && (m_system == SystemId::A2600 || m_system == SystemId::A7800))
  {
    // (00.90) Stella and ProSystem take none (their retro_cheat_set does nothing); Virtual Jaguar does.
    rows.push_back({"The Atari 2600's and 7800's emulators take no cheat codes", "", true});
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
      const bool macro = Controls::IsMacro(bindings[i]);  // (00.89: "R1 = Cross + Triangle")
      rows.push_back({bindings[i].label,
                      waiting && m_capture_step == 1 ? std::string("Hold its buttons...") :
                      waiting                        ? std::string("Press a button...") :
                      macro                          ? Controls::MacroText(bindings[i], edit, m_system) :
                      bindings[i].stick_group        ? value :
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
  if (m_page == Page::Extras)
  {
    // (01.22) Left / Right: the 3DS's Mods On / Off only (Cross does the rest: ExtrasActivate).
    if (key == "x:mods")
      ExtrasActivate(key);
    return;
  }
  if (m_page == Page::Cheats && key == "cheatfind")
  {
    if (direction <= 0)
      return;
    // libretro's lists for this game (the collection downloaded first when it isn't here).
    m_page = Page::CheatSearch;
    m_selected = 0;
    m_search_selected = 0;
    // (00.87: the collection's download stays in this PS4's cache - m_data_dir can be a USB drive's portable data.)
    m_finder.Start(App::DATA_DIR, m_system, m_game_path, m_title);
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
        m_capture_step = 0;
        m_capture_chord = 0;
      }
      else
      {
        Controls::Set(binding, "None", edit, m_system);  // Left: none
        if (Controls::IsMacro(binding))
          Controls::SetMacroPresses(binding, "", edit, m_system);
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
    // (00.23) Online's rows.
    if (row.key == "np:voice")
      return T("Your microphone and the other players' voices. In a public room it starts off: turn it on to talk.");
    if (row.key == "np:me")
      return T("You: your controllers' players in this match.");
    if (row.key == "np:phrase")
      return T("Left / Right choose a phrase, Cross sends it to everyone in the room (in their language).");
    if (row.key.rfind("np:player:", 0) == 0)
      return T("Cross mutes or unmutes this player's voice. Their ping: the round trip to their console.");
    if (row.key.rfind("np:kick:", 0) == 0)
      return T(Netplay::GetStatus().room_back && !Netplay::GetStatus().room_back_old ?
                   "Removes this player from the room: they can't join it again. The match ends, the others back in "
                   "the room (it stays open)." :
                   "Removes this player from the room: they can't join it again. The match ends.");
    // (00.86) A player asking to join our room while this game is played alone.
    if (row.key.rfind("np:accept:", 0) == 0)
      return T(Netplay::Active() ? "Lets this player into your room: they join the match under way (everyone stops a "
                                   "moment)." :
                                   "Lets this player into your room: this game ends and your room opens - press Start "
                                   "there to play together.");
    if (row.key.rfind("np:reject:", 0) == 0)
      return T("This player doesn't join your room, and can't ask again.");
    if (row.key == "quit" && Netplay::Active())
    {
      // ((01.23) A guest who leaves no longer closes the host's room: the others go back to it.)
      const Netplay::Status status = Netplay::GetStatus();
      return T(status.watching                         ? "Leaves the room: the match goes on without you." :
               status.host                             ? "Closes the room: the match ends for everyone." :
               status.room_back && !status.room_back_old ? "Leaves the room: the match ends, the others go back to "
                                                           "the room." :
                                                           "Leaves the room: the match ends for everyone.");
    }
    if (row.key == "np:end")
      return T(Netplay::GetStatus().together ?
                   "Ends this match for both: back to the room's games, to pick another." :
                   "Ends this match for every player: all back in the room (its chat, its players), where the host "
                   "can start again. You stay in the room.");
    if (row.key == "np:reset")
      return T("Resets the game for every player at once: everyone stops a moment, then it starts again.");
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
        {"Updates, DLC and Mods", "What this game has installed, its mods on or off; its CIAs to install (the game "
                                  "closes first)."},
        {"Patched Romset", "The patched romset this game uses, or one to install from Roms/arcade/patched (the game "
                           "closes first)."},
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
    if (m_capture && m_capture_step == 1)
      return T("Now hold together the buttons it presses, then let them go...  (5 s: cancel)");
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
    if (Controls::IsMacro(bindings[index]))
      return T("Several buttons in one (fighting games). Cross: press the macro's button, then hold together the "
               "buttons it presses and let them go. Left: none. Its button stops doing what it did.");
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
    // (00.90, GitHub #45) The N64's sticks: what they do whatever the rows say.
    if (m_system == SystemId::N64 && bindings[index].stick_group)
      return T("Left/Right: which controls move the N64's D-pad. The left stick is always its control stick.");
    if (m_system == SystemId::N64 && key.rfind("c_", 0) == 0)
      return T("Cross: press a new button or push a stick. Left: none. The right stick presses the C buttons in any "
               "case.");
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
    if (Controls::TakesSticks(bindings[index]))
      return T("Cross: press a new button or push a stick for it. Left: none. D-Pad: Left/Right to choose.");
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
  if (m_page == Page::Extras)
    return m_selected >= 0 && m_selected < static_cast<int>(m_rows.size()) ? ExtrasActivate(m_rows[m_selected].key) :
                                                                             Action::None;
  if (m_page != Page::Main)
  {
    Change(1);
    return Action::None;
  }
  const std::string key = MainKey();
  if (key == "extras")
  {
    m_page = Page::Extras;  // ((01.22) read now: the CIAs' headers, what is installed)
    m_selected = 0;
    m_extras_ask.clear();
    LoadExtras();
    return Action::None;
  }
  if (key == "resume")
  {
    Close();
    return Action::Closed;
  }
  if (key == "np:end")
  {
    // (00.23: both back to the Choose Together room. (01.23) Any room's: every player back in it.)
    if (Netplay::GetStatus().room_back_old)
    {
      Toast("A player has an older EmuSuNi: the match can't go back to the room");
      return Action::None;
    }
    if (!Netplay::EndMatchToRoom())
    {
      Toast("Not now: try again in a moment");
      return Action::None;
    }
    LogLine("menu: the online match ends: back to the room");
    return Action::Quit;
  }
  if (key == "np:reset")
  {
    // (01.14) The host resets the game for every player at once (Netplay::ResetMatch): a moment's stop, then it resets.
    const Netplay::Status status = Netplay::GetStatus();
    if (status.reset_old)
    {
      Toast("A player has an older EmuSuNi: it can't be reset online");
      return Action::None;
    }
    if (!Netplay::ResetMatch())
    {
      Toast("Not now: try again in a moment");
      return Action::None;
    }
    LogLine("menu: the host resets the online match's game");
    Close();
    return Action::Closed;
  }
  if (key.rfind("np:accept:", 0) == 0 || key.rfind("np:reject:", 0) == 0)
  {
    // (00.86) A player asking to join our room while this game is played alone: let in - the game ends, the menu
    // opens the room (Start there) -, or not.
    const int member = std::atoi(key.c_str() + 10);
    const Netplay::JoinRequest ask = Netplay::GetJoinRequest();
    m_online_refresh = 0.0f;
    if (ask.member != member)
    {
      Toast("The request to join is over");
      BuildRows();
      return Action::None;
    }
    if (key.rfind("np:accept:", 0) == 0 && Netplay::Active())
    {
      // ((01.23) Our match under way, entry with permission: it joins the match - no game ends.)
      LogF("menu: %s accepted in our match", ask.nick.c_str());
      Netplay::Admit(member);
      Toast(ask.nick + " joins the match in a moment");
      BuildRows();
      return Action::None;
    }
    if (key.rfind("np:accept:", 0) == 0)
    {
      LogF("menu: %s accepted in our room: the game ends, the room opens", ask.nick.c_str());
      Netplay::Admit(member);
      return Action::Quit;
    }
    Netplay::Refuse(member);
    Toast(ask.nick + " was not accepted");
    BuildRows();
    return Action::None;
  }
  if (key.rfind("np:", 0) == 0)
  {
    OnlineActivate(key, 1);  // (00.23: a player muted, the voice chat, Remove Player)
    return Action::None;
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

// ---- (01.22, the owner: "en el menú L2 y R2 en un juego") What the game has installed ----

bool IngameMenu::HasExtras() const
{
#ifdef EMUSUNI_3DS
  if (m_system == SystemId::THREEDS)
    return true;
#endif
  return m_system == SystemId::ARCADE;
}

// Read as the page opens: what is installed (the core's SD card, the patched romsets' folder), what can be installed
// (the menu's folders: GameExtras::InstallFolders - the CIAs' headers read by the core, any thread).
void IngameMenu::LoadExtras()
{
  std::vector<Row> rows;
  m_extras_files.clear();
  auto T = [](const std::string& text) { return std::string(PS4Lang::T(text)); };
  // A row with its help (RowHelp says a row's own as it is: translated here).
  auto add = [&rows](const std::string& label, const std::string& value, const std::string& key,
                     const std::string& help) {
    Row row{label, value, false, false, 0, key};
    row.help = help;
    rows.push_back(std::move(row));
  };
  const std::string closes = T("The game closes first; then the menu asks.");
#ifdef EMUSUNI_3DS
  if (m_system == SystemId::THREEDS)
  {
    m_extras_title = ThreeDsCia::RomTitleId(m_game_path);
    if (m_extras_title == 0)
    {
      PushWrapped(&rows, "A homebrew game (.3dsx) has no title ID: no updates, DLC or mods.");
      m_extras_rows = std::move(rows);
      return;
    }
    const std::string save_dir = m_data_dir + "/saves/3ds";  // (the menu's ThreeDsSaveDir: the core's save folder)
    Row id{ThreeDsCia::Hex(m_extras_title), "", true};
    id.raw = true;
    rows.push_back(id);
    if (ThreeDsCia::Available())
    {
      const ThreeDsCia::Installed has = ThreeDsCia::InstalledFor(save_dir, m_extras_title);
      add("Update", has.read && has.has_update ? ThreeDsCia::VersionText(has.update_version) : std::string("None"),
          "x:update", T("The update the game started with (an update installed now: from its next start)."));
      add("DLC", has.read && has.dlc_titles > 0 ? std::string("Installed") : std::string("None"), "x:dlc",
          has.read && has.dlc_titles > 0 ? T("Its DLC's contents installed: " + std::to_string(has.dlc_contents)) :
                                           T("Install them from their CIA files: copied to Roms/3ds/cia, they show "
                                             "here."));
      if (has.read && has.has_update)
        add("Remove the Update", "", "x:remove_update", closes);
      if (has.read && has.dlc_titles > 0)
        add("Remove the DLC", "", "x:remove_dlc", closes);
      rows.push_back({"Install from CIA Files", "", true});
      for (const ThreeDsCia::File& file : ThreeDsCia::List(GameExtras::InstallFolders(SystemId::THREEDS)))
      {
        const ThreeDsCia::Info& info = file.info;
        if (!info.read || ThreeDsCia::GameOf(info.title_id) != m_extras_title || info.kind == ThreeDsCia::Kind::Game)
          continue;
        const bool update = info.kind == ThreeDsCia::Kind::Update;
        const std::string what = update ? "Update " + ThreeDsCia::VersionText(info.version) : std::string("DLC");
        const std::string help = file.file_name + "  \xC2\xB7  " + ThreeDsCia::SizeText(file.size) + "\n";
        if (info.encrypted)
        {
          add(what, "Encrypted", "x:encrypted", help + T(ThreeDsCia::CodeText(ThreeDsCia::ENCRYPTED)));
          continue;
        }
        if (update && has.read && has.has_update && has.update_version >= info.version)
        {
          add(what, "Installed", "x:installed",
              help + T("Installed already: " + ThreeDsCia::VersionText(has.update_version)));
          continue;
        }
        add(what, ThreeDsCia::SizeText(info.install_bytes), "x:file:" + std::to_string(m_extras_files.size()),
            help + closes);
        m_extras_files.push_back(file.path);
      }
      if (m_extras_files.empty())
        add("No CIA Files for It", "", "x:nocia",
            T("Copy its decrypted update or DLC CIAs to the folder cia in Roms/3ds:") + "\nUSB: Roms/3ds/cia");
    }
    else
    {
      add("Updates and DLC", "Not in This Version", "x:none", T(ThreeDsCia::CodeText(ThreeDsCia::NO_CORE)));
    }
    Row mods{"Mods", "", false, true, ThreeDsCia::ModsOn(m_extras_title) ? 1 : -1, "x:mods"};
    mods.help = T("Its mods on or off: from the game's next start (Quit Game, then start it again). Their folder: "
                  "saves/3ds/Azahar/load/mods/" + ThreeDsCia::Hex(m_extras_title));
    rows.push_back(std::move(mods));
  }
#endif
  if (m_system == SystemId::ARCADE)
  {
    // (The Arcade's set: the game's id - its zip's name -, as Emulator.cpp's patched romset.)
    const std::string dir = Portable::SystemDir(GetSystem(SystemId::ARCADE).key) + "/fbneo/patched";
    const GameExtras::PatchedFile in_use = GameExtras::PatchedInUse(dir, m_game_id);
    Row set{m_game_id, "", true};
    set.raw = true;
    rows.push_back(set);
    add("In Use", in_use.path.empty() ? std::string("None") : in_use.name, "x:inuse",
        in_use.path.empty() ? T("The game started from its own zip.") :
                              T("FinalBurn Neo started the game from it instead of its own zip.") + "\n" + in_use.path);
    if (!in_use.path.empty())
      add("Remove It", "", "x:remove_patched", closes);
    rows.push_back({"Install from Roms/arcade/patched", "", true});
    for (const GameExtras::PatchedFile& file :
         GameExtras::PatchedFound(GameExtras::InstallFolders(SystemId::ARCADE), m_game_id))
    {
      Row row{file.name, GameExtras::SizeText(file.size), false, false, 0,
              "x:file:" + std::to_string(m_extras_files.size())};
      row.raw = true;  // (its file's name as it is)
      row.help = file.path + "\n" + closes;
      rows.push_back(std::move(row));
      m_extras_files.push_back(file.path);
    }
    if (m_extras_files.empty())
      add("None Found", "", "x:nopatched",
          T("Copy the patched romset, named as the game's set, to the folder patched in Roms/arcade: " + m_game_id +
            ".zip"));
  }
  m_extras_rows = std::move(rows);
}

void IngameMenu::ExtrasRows(std::vector<Row>* rows)
{
  if (m_extras_ask.empty())
  {
    *rows = m_extras_rows;
    return;
  }
  // The action asked: the game closes first - said, with Close the Game or Cancel.
  rows->push_back({m_extras_what, "", true});
  PushWrapped(rows, m_system == SystemId::ARCADE ?
                        "To do it the game closes: the patched romset is read when the game starts." :
                        "To do it the game closes: the 3DS emulator installs and removes only with no game running.");
  PushWrapped(rows, "Save first (Save State, or in the game): what isn't saved is lost. Then the menu asks before "
                    "anything is done, and offers to start the game again.");
  rows->push_back({"", "", true});
  rows->push_back({"Close the Game", "", false, false, 0, "x:go"});
  rows->push_back({"Cancel", "", false, false, 0, "x:cancel"});
}

IngameMenu::Action IngameMenu::ExtrasActivate(const std::string& key)
{
  if (key == "x:go" && !m_extras_ask.empty())
  {
    // The menu does it after the game (its panel, its confirmation): the game ends here as Quit Game.
    GameExtras::AskAfterGame(m_game_path, m_extras_ask);
    m_extras_ask.clear();
    return Action::Quit;
  }
  if (key == "x:cancel")
  {
    m_extras_ask.clear();
    m_selected = 0;
    BuildRows();
    return Action::None;
  }
#ifdef EMUSUNI_3DS
  if (key == "x:mods" && m_extras_title != 0)
  {
    const bool on = !ThreeDsCia::ModsOn(m_extras_title);
    ThreeDsCia::SetModsOn(m_extras_title, on);
    for (Row& row : m_extras_rows)
    {
      if (row.key == "x:mods")
        row.tag = on ? 1 : -1;
    }
    LogF("menu: the game's mods %s (from its next start)", on ? "on" : "off");
    Toast(on ? "Mods on: from the game's next start" : "Mods off: from the game's next start");
    BuildRows();
    return Action::None;
  }
#endif
  std::string ask;
  if (key.rfind("x:file:", 0) == 0)
  {
    const size_t index = static_cast<size_t>(std::atoi(key.c_str() + 7));
    if (index < m_extras_files.size())
      ask = (m_system == SystemId::ARCADE ? "arcade:install:" : "3ds:install:") + m_extras_files[index];
  }
  else if (key == "x:remove_update" || key == "x:remove_dlc")
  {
    ask = key == "x:remove_dlc" ? "3ds:remove_dlc" : "3ds:remove_update";
  }
  else if (key == "x:remove_patched")
  {
    ask = "arcade:remove";
  }
  if (ask.empty())
  {
    // An information row: where its files go.
    if (key == "x:nocia" || key == "x:dlc" || key == "x:update")
      Toast("Copy its CIA files to Roms/3ds/cia: they show here", 3.0f);
    else if (key == "x:nopatched")
      Toast("Copy it to Roms/arcade/patched, named as the game's set: it shows here", 3.0f);
    return Action::None;
  }
  m_extras_ask = ask;
  for (const Row& row : m_extras_rows)
  {
    if (row.key == key)
      m_extras_what = PS4Lang::T(row.label) + (row.value.empty() ? "" : "  -  " + std::string(PS4Lang::T(row.value)));
  }
  BuildRows();
  for (size_t i = 0; i < m_rows.size(); i++)
  {
    if (m_rows[i].key == "x:go")
      m_selected = static_cast<int>(i);
  }
  return Action::None;
}

// (00.23) Online: the match's players (their nicknames as they are, their ping or "Muted", the players they are),
// the voice chat, Remove Player (the host's), Settings, Controls, Leave Room.
// (01.05) The match's chat at the right of the screen while the menu is open: its lines, the newest at the bottom (a
// quick phrase in this console's language), and how to write.
void IngameMenu::DrawChat(ImDrawList* dl, float a)
{
  const Netplay::Status st = Netplay::GetStatus();
  constexpr float X0 = 940.0f, X1 = 1840.0f, Y0 = 110.0f, Y1 = 860.0f, PAD = 24.0f, SIZE = 28.0f;
  dl->AddRectFilled(ImVec2(X0, Y0), ImVec2(X1, Y1), IM_COL32(8, 4, 18, static_cast<int>(200 * a)), 18.0f);
  dl->AddRect(ImVec2(X0, Y0), ImVec2(X1, Y1), IM_COL32(176, 140, 255, static_cast<int>(160 * a)), 18.0f, 0, 2.0f);
  dl->AddText(m_bold, 32, ImVec2(X0 + PAD, Y0 + 18), IM_COL32(196, 170, 255, static_cast<int>(255 * a)),
              PS4Lang::T("Chat"));
  static const ImU32 COLOURS[] = {IM_COL32(98, 230, 255, 255), IM_COL32(255, 200, 112, 255),
                                  IM_COL32(70, 245, 174, 255), IM_COL32(255, 138, 200, 255)};
  const float wrap = X1 - X0 - PAD * 2.0f, top = Y0 + 70.0f;
  float y = Y1 - 70.0f;
  dl->PushClipRect(ImVec2(X0, top), ImVec2(X1, Y1 - 60.0f), true);
  for (auto it = st.chat.rbegin(); it != st.chat.rend() && y > top; ++it)
  {
    if (it->notice)
    {
      // (01.06) A note of ours (a player of an older EmuSuNi, without the chat): dim, no nickname.
      const std::string note = PS4Lang::T(it->text);
      y -= m_font->CalcTextSizeA(SIZE, FLT_MAX, wrap, note.c_str()).y + 8.0f;
      dl->AddText(m_font, SIZE, ImVec2(X0 + PAD, y), IM_COL32(255, 255, 255, static_cast<int>(150 * a)), note.c_str(),
                  nullptr, wrap);
      continue;
    }
    const std::string nick = it->mine ? PS4Lang::T("You") : it->nick;
    // ((01.15) The nickname in bold and its colour, the words after it: NetplayUi::LayoutChatLine. Until 01.14 the
    // line in the regular font and the nickname again over it in bold - wider: over the words.)
    const NetplayUi::ChatLine line = NetplayUi::LayoutChatLine(
        m_font, m_bold, SIZE, SIZE + 6.0f, nick,
        it->phrase ? PS4Lang::ChatPhraseText(it->phrase, it->param) : it->text, wrap);
    y -= line.height + 8.0f;
    const ImU32 who = it->mine ? IM_COL32(196, 170, 255, 255) : COLOURS[std::clamp(it->from, 0, 3)];
    NetplayUi::DrawChatLine(dl, m_font, m_bold, SIZE, SIZE + 6.0f, line, ImVec2(X0 + PAD, y),
                            (who & 0x00FFFFFF) | (static_cast<ImU32>(255 * a) << 24),
                            IM_COL32(255, 255, 255, static_cast<int>(235 * a)));
  }
  dl->PopClipRect();
  if (st.chat.empty())
    dl->AddText(m_font, 26, ImVec2(X0 + PAD, top + 10), IM_COL32(255, 255, 255, static_cast<int>(150 * a)),
                PS4Lang::T("No messages yet."));
  dl->AddText(m_font, 22, ImVec2(X0 + PAD, Y1 - 48), IM_COL32(255, 255, 255, static_cast<int>(150 * a)),
              PS4Lang::T("Quick Phrase: in the menu. Typed lines: in the room, before or after the match."), nullptr,
              wrap);
}

void IngameMenu::OnlineRows(std::vector<Row>* rows)
{
  const Netplay::Status status = Netplay::GetStatus();
  rows->push_back({"Resume Game", "", false, false, 0, "resume"});
  // (01.23, the owner) Entry with permission, the host's match under way: a player asking to join it - Accept (it
  // joins the match: everyone stops a moment) or Reject.
  const Netplay::JoinRequest ask = Netplay::GetJoinRequest();
  if (ask.member >= 0)
  {
    const std::string id = std::to_string(ask.member);
    rows->push_back({"Accept Player", ask.nick + "  " + std::to_string(ask.seconds_left) + " s", false, false, 0,
                     "np:accept:" + id});
    rows->push_back({"Reject Player", ask.nick, false, false, 0, "np:reject:" + id});
  }
  for (const Netplay::PlayerView& p : status.players)
  {
    std::string value;
    if (p.ports > 0)
      value = p.ports == 1 ? "P" + std::to_string(p.first_port + 1) :
                             "P" + std::to_string(p.first_port + 1) + "-" + std::to_string(p.first_port + p.ports);
    if (!p.me)
      value += std::string(value.empty() ? "" : "  ") +
               (p.muted ? std::string(PS4Lang::T("Muted")) :
                p.ping_ms >= 0 ? std::to_string(p.ping_ms) + " ms" : std::string("..."));
    if (p.talking)
      value = std::string(PS4Lang::T("Speaking")) + "  " + value;
    Row row{p.nick, value, false, false, 0, p.me ? "np:me" : "np:player:" + std::to_string(p.id)};
    row.raw = true;  // (a nickname: not translated)
    rows->push_back(row);
  }
  // (01.05) A quick phrase to everyone (Left / Right: which; Cross: sent) - in each console's language. (A spectator
  // has neither chat nor voice.)
  if (!status.watching)
  {
    // ((01.07) "I'm from ..." with our country's name.)
    Row row{"Quick Phrase", PS4Lang::ChatPhraseText(m_chat_phrase, MyCountry()), false, false, 0, "np:phrase"};
    rows->push_back(row);
    rows->push_back({"Voice Chat", status.voice_on ? "On" : "Off", false, false, 0, "np:voice"});
  }
  // (00.92) The players' ping and connection in the game's corner (Netplay::SetLatencyBar: kept for the next ones).
  rows->push_back({"Latency Bar", status.latency_bar ? "On" : "Off", false, false, 0, "np:bar"});
  if (status.host)
  {
    // (01.14) The host resets the game for everyone (not a spectator's; not with a player of an older EmuSuNi).
    if (status.can_reset || status.reset_old)
      rows->push_back({"Reset Game", status.reset_old ? "Not with an older EmuSuNi" : "", false, false, 0, "np:reset"});
    for (const Netplay::PlayerView& p : status.players)
    {
      if (!p.me)
        rows->push_back({"Remove Player", p.nick, false, false, 0, "np:kick:" + std::to_string(p.id)});
    }
  }
  rows->push_back({"Settings", "", false, false, 0, "settings"});
  rows->push_back({"Controls", "", false, false, 0, "controls"});
  // (A Choose Together room: the match ends for both, back to the room's games to pick another.) (01.23, the owner:
  // "salir del juego sin salir de la sala") Any room's but a link cable's: the match ends for every player, all back in
  // the room (its chat, its players, the host's Start) - not with a player of an older EmuSuNi.
  if (status.room_back)
    rows->push_back({"Back to the Room", status.room_back_old ? "Not with an older EmuSuNi" : "", false, false, 0,
                     "np:end"});
  rows->push_back({status.watching ? "Stop Watching" : "Leave Room", "", false, false, 0, "quit"});
}

std::string IngameMenu::MyCountry()
{
  for (const Netplay::PlayerView& p : Netplay::GetStatus().players)
  {
    if (p.me)
      return p.country;
  }
  return {};
}

void IngameMenu::OnlineActivate(const std::string& key, int direction)
{
  const Netplay::Status status = Netplay::GetStatus();
  if (key == "np:phrase")
  {
    // (01.05) Sent to everyone in the match (one line every 2 s).
    const Netplay::ChatResult r = Netplay::SendChatPhrase(m_chat_phrase);
    if (r == Netplay::ChatResult::TooSoon)
      Toast("One message every 2 seconds");
    else if (r != Netplay::ChatResult::Sent)
      Toast("Not in the room yet");
    return;
  }
  if (key == "np:voice")
  {
    Netplay::SetVoiceOn(!status.voice_on);
    Toast(Netplay::GetStatus().voice_on ? "Voice chat on" : "Voice chat off");
  }
  else if (key == "np:bar")
  {
    Netplay::SetLatencyBar(m_data_dir, !status.latency_bar);
  }
  else if (key.rfind("np:player:", 0) == 0 && direction > 0)
  {
    const int id = std::atoi(key.c_str() + 10);
    for (const Netplay::PlayerView& p : status.players)
    {
      if (p.id != id || p.me)
        continue;
      Netplay::SetPeerMuted(id, !p.muted);
      Toast(p.nick + (p.muted ? " can be heard again" : " muted"));
    }
  }
  else if (key.rfind("np:kick:", 0) == 0 && direction > 0)
  {
    // Asked twice: a stranger isn't removed by one press too many.
    const int id = std::atoi(key.c_str() + 8);
    std::string nick;
    for (const Netplay::PlayerView& p : status.players)
    {
      if (p.id == id)
        nick = p.nick;
    }
    if (m_kick_asked != id)
    {
      m_kick_asked = id;
      m_kick_time = 3.0f;
      Toast("Cross again: " + nick + " out of the room", 3.0f);
      return;
    }
    m_kick_asked = -1;
    Netplay::Kick(id);
  }
  m_online_refresh = 0.0f;  // (the rows as they are now)
}

// The Controls page's capture: once every button is up, the first one pressed is the row's new button (5 s
// without one: nothing changes).
void IngameMenu::Capture(uint32_t held, float dt)
{
  m_capture_time += dt;
  OrbisPadData data = {};
  // (00.90) The sticks' directions come as their bits (src/Emulator.cpp: Controls::StickBits), the sticks in the
  // middle here.
  data.buttons = held & (Controls::PAD_BUTTONS | Controls::STICK_BITS);
  data.leftStick.x = data.leftStick.y = data.rightStick.x = data.rightStick.y = 128;
  if (!m_capture_released)
  {
    m_capture_released = Controls::IsAtRest(data);  // (the Cross that asked for it is let go first)
    return;
  }
  const auto& bindings = Controls::Bindings(m_system);
  const size_t index = m_capture_row >= 0 && m_capture_row < static_cast<int>(m_rows.size()) ?
                           static_cast<size_t>(std::atoi(m_rows[static_cast<size_t>(m_capture_row)].key.c_str() + 4)) :
                           bindings.size();
  if ((m_capture_time > 5.0f && m_capture_chord == 0) || index >= bindings.size())
  {
    m_capture = false;
    m_capture_step = 0;
    Toast("No button pressed: nothing changed");
    BuildRows();
    return;
  }
  const std::string edit = Controls::HasGameProfile(m_game_id) ? m_game_id : std::string();
  if (m_capture_step == 1)
  {
    // (00.89) A macro's presses: every button held together, taken once all of them are let go.
    m_capture_chord |= Controls::PressedControls(data);
    if (m_capture_chord == 0 || !Controls::IsAtRest(data))
      return;
    Controls::Set(bindings[index], m_capture_trigger, edit, m_system);
    Controls::SetMacroPresses(bindings[index], Controls::NamesOf(m_capture_chord), edit, m_system);
    m_capture = false;
    m_capture_step = 0;
    Toast(PS4Lang::T(bindings[index].label) + std::string(": ") +
          PS4Lang::T(Controls::MacroText(bindings[index], edit, m_system)));
    BuildRows();
    return;
  }
  const std::string control = Controls::Capture(data, Controls::TakesSticks(bindings[index]));  // (00.90: a stick too)
  if (control.empty())
    return;
  if (Controls::IsMacro(bindings[index]))
  {
    // Its button: now its presses (kept with it once they come; 5 s without them: nothing changes).
    m_capture_trigger = control;
    m_capture_step = 1;
    m_capture_chord = 0;
    m_capture_released = false;
    m_capture_time = 0.0f;
    BuildRows();
    return;
  }
  Controls::Set(bindings[index], control, edit, m_system);
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
  // (00.23) Online: the players' rows made again twice a second (their pings, who speaks); a Remove Player asked
  // once forgotten after 3 s.
  m_kick_time = std::max(0.0f, m_kick_time - dt);
  if (m_kick_time <= 0.0f)
    m_kick_asked = -1;
  // (00.86: also our room open while the game is played alone - a request to join, its seconds left.)
  if (m_page == Page::Main && (Netplay::Active() || Netplay::Hosting()))
  {
    m_online_refresh -= dt;
    if (m_online_refresh <= 0.0f)
    {
      m_online_refresh = 0.5f;
      BuildRows();
    }
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
      else if (key == "np:voice" || key == "np:bar")
        OnlineActivate(key, direction);  // (00.23; 00.92: the latency bar)
      else if (key == "np:phrase")
      {
        // (01.05) The next or the one before (round). ((01.07) "I'm from ..." only with our country shown.)
        m_chat_phrase = (m_chat_phrase - 1 + direction + NetProto::CHAT_PHRASES) % NetProto::CHAT_PHRASES + 1;
        if (m_chat_phrase == NetProto::PHRASE_FROM && MyCountry().empty())
          m_chat_phrase = (m_chat_phrase - 1 + direction + NetProto::CHAT_PHRASES) % NetProto::CHAT_PHRASES + 1;
        BuildRows();
      }
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
    else if (m_page == Page::Extras && !m_extras_ask.empty())
    {
      m_extras_ask.clear();  // ((01.22) Close the Game not chosen: the page again)
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
  m_picture_saved = Saves::Modified(Saves::StatePath(m_game_id, slot)) != 0;
  ImageRGBA image;
  if (LoadPng(Saves::StatePicturePath(m_game_id, slot), &image, 512, 448))
  {
    m_picture = m_display->CreateTexture(image.pixels.data(), image.width, image.height);
    // Its own shape (00.85: a picture of the screen - a 16:9 PSP game's, the DS's two screens - isn't the Super
    // Nintendo's 8:7 all were drawn at).
    m_picture_aspect = image.height ? std::clamp(static_cast<float>(image.width) / image.height, 0.5f, 2.5f) : 8.0f / 7.0f;
  }
}

void IngameMenu::Draw(ImDrawList* dl, float dt)
{
  m_appear += ((m_open ? 1.0f : 0.0f) - m_appear) * (1.0f - std::exp(-14.0f * dt));
  m_hint_time += dt;
  if (m_toast_time > 0.0f)
    m_toast_time -= dt;
  UpdatePicture();
  // (01.05) The others' new chat lines over the game while the menu is closed (not those from before the match).
  if (Netplay::Active())
  {
    const Netplay::Status st = Netplay::GetStatus();
    const uint32_t newest = st.chat.empty() ? 0 : st.chat.back().serial;
    if (!m_chat_primed)
    {
      m_chat_seen = newest;
      m_chat_primed = true;
    }
    else if (newest > m_chat_seen)
    {
      for (const Netplay::ChatLine& c : st.chat)
      {
        if (c.serial <= m_chat_seen || c.mine || m_open)
          continue;
        std::string line = c.notice ? PS4Lang::T(c.text) :  // ((01.06) a note of ours)
                               c.nick + ": " + (c.phrase ? PS4Lang::ChatPhraseText(c.phrase, c.param) : c.text);
        if (line.size() > 90)
        {
          size_t cut = 87;
          while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
            cut--;
          line = line.substr(0, cut) + "...";
        }
        Toast(line, 4.0f);  // (several at once: the newest)
      }
      m_chat_seen = newest;
    }
  }
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
                           m_page == Page::Extras ? (m_system == SystemId::ARCADE ? "PATCHED ROMSET" :
                                                                                     "UPDATES, DLC AND MODS") :
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
      // (In a box of 600 x 525, its shape kept; an empty slot's box the whole of it.)
      const float aspect = m_picture != ImTextureID{} ? m_picture_aspect : 8.0f / 7.0f;
      const float pw = std::min(600.0f, 525.0f * aspect), ph = pw / aspect;
      const ImVec2 p0(x0 + panel_w + 120 + (600.0f - pw) * 0.5f, size.y * 0.5f - ph * 0.5f - 30), p1(p0.x + pw, p0.y + ph);
      dl->AddRectFilled(ImVec2(p0.x - 6, p0.y - 6), ImVec2(p1.x + 6, p1.y + 6),
                        IM_COL32(0, 0, 0, static_cast<int>(200 * a)), 10);
      if (m_picture != ImTextureID{})
      {
        dl->AddImage(ImTextureRef(m_picture), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, static_cast<int>(255 * a)));
      }
      else
      {
        // (00.85: a state saved without a picture - before 00.85 every state of a console that draws its own image -
        // isn't an empty slot.)
        const char* empty = m_picture_saved ? "No picture" : "Empty slot";
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
                    m_page == Page::Extras       ? "Select" :
                                                  "Change");
      hint("D-Pad", "Move");
      hint("Circle", "Back");
    }
  }

  // (01.05) An online match's chat at the right while the menu is open (the whole screen: the room's view).
  if (a > 0.01f && m_page == Page::Main && Netplay::Active() && !Netplay::GetStatus().watching)
    DrawChat(dl, a);  // ((01.05) a spectator has none)

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
