// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4IngameMenu.cpp (Dolphin Emulator Project, GPL-2.0+).

// The in-game menu (L3 + R3): resume, save states in eight slots (with their pictures), the disc
// (a game on several discs), a NAOMI's or Atomiswave's TEST and SERVICE buttons (Arcade Button), the
// settings (all games or this game), the controls (this console's or this game's), cheats, reset and quit; plus the
// short "L3 + R3: Menu" hint when a game starts, and messages ("Saved to slot 2").

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include "CheatDownload.h"
#include "Cheats.h"
#include "Controls.h"
#include "Systems.h"

class Display;

class IngameMenu
{
public:
  enum class Action
  {
    None,
    Save,    // save a state to Slot()
    Load,    // load the state in Slot()
    Reset,   // the console's reset button
    Quit,    // back to the game list
    Closed,  // the menu closed (the game goes on)
    ChangeDisc,  // put Disc() in (the console's lid opens and closes)
    ArcadeButton,  // back to the game with the board's TEST button pressed (ArcadeTest()), else SERVICE
  };

  void Init(Display* display, ImFont* regular, ImFont* bold);
  void SetGame(const std::string& data_dir, const std::string& game_key, const std::string& title,
               SystemId system, const std::string& game_path);
  void Open();
  void Close();
  bool IsOpen() const { return m_open; }
  // The buttons that open and close the menu (Settings: menu_hotkey, read by SetGame).
  const Controls::MenuHotkey& Hotkey() const { return m_hotkey; }
  // DS4 buttons that went down this frame and those held (directions repeat while held).
  Action Poll(uint32_t pressed, uint32_t held, float dt);
  int Slot() const { return m_slot; }
  // The discs the core can change between (under 2: no Disc line) and the one in it now (from 0),
  // before Open.
  void SetDiscs(int count, int inserted);
  int Disc() const { return m_disc; }  // the disc chosen (from 0)
  // A NAOMI's or Atomiswave's Arcade Button: TEST (its operator menu) or SERVICE, chosen with Left / Right.
  bool ArcadeTest() const { return m_arcade_test; }
  void Toast(const std::string& text, float seconds = 2.2f);
  void Draw(ImDrawList* dl, float dt);
  // Changed while the menu was open: the emulator applies them.
  bool TakeSettingsChanged();
  bool TakeCheatsChanged();
  Cheats::List& CheatList() { return m_cheats; }
  // The slot pictures are stale after a save.
  void ForgetPictures();

private:
  enum class Page
  {
    Main,
    Settings,
    Cheats,
    CheatSearch,  // Find Cheats Online: libretro's lists for this game
    CheatPick,    // one list's codes, to choose from
    Controls,     // this console's buttons on the DualShock 4 (src/Controls.h): applied back in the game
  };
  struct Row
  {
    std::string label;
    std::string value;
    bool header = false;
    bool note = false;  // takes effect at the next game start (amber)
    int tag = 0;        // cheats: 1 ON, -1 OFF
    std::string key;
  };
  void BuildRows();
  // The main page's selected line ("resume", "save", "load", "disc", ...), else "".
  std::string MainKey() const;
  void Move(int direction);
  Action Activate();
  void Change(int direction);
  // What the row does (translated), shown small under the list.
  std::string RowHelp(const Row& row) const;
  std::string SlotLabel(int slot) const;
  void UpdatePicture();

  Display* m_display = nullptr;
  ImFont* m_font = nullptr;
  ImFont* m_bold = nullptr;
  std::string m_data_dir, m_game_id, m_title, m_game_path;
  SystemId m_system = SystemId::SNES;
  bool m_open = false;
  Page m_page = Page::Main;
  std::vector<Row> m_rows;
  int m_selected = 0;
  int m_main_selected = 0;
  int m_slot = 1;
  int m_disc_count = 0, m_disc_in = 0, m_disc = 0;
  bool m_arcade_test = true;
  bool m_arcade_again = false;  // the Arcade Button closed the menu: the next Open starts on its row
  float m_appear = 0.0f, m_scroll = 0.0f, m_repeat = 0.0f;
  std::string m_toast;
  float m_toast_time = 0.0f;
  float m_hint_time = 0.0f;  // since the game started
  Controls::MenuHotkey m_hotkey = Controls::MenuHotkeyOf("l3r3");
  bool m_settings_changed = false, m_cheats_changed = false;
  // The Controls page's button capture: the row waiting for a press (taken once the pad was at rest; 5 s: none).
  bool m_capture = false, m_capture_released = false;
  int m_capture_row = -1;
  float m_capture_time = 0.0f;
  void Capture(uint32_t held, float dt);
  Cheats::List m_cheats;
  CheatDownload::Finder m_finder;
  CheatDownload::Finder::State m_finder_state = CheatDownload::Finder::State::Idle;
  int m_finder_mb = -1;
  int m_search_selected = 0;  // the list chosen last (back from its codes)
  // The selected slot's picture.
  int m_picture_slot = -1;
  ImTextureID m_picture{};
  float m_picture_aspect = 8.0f / 7.0f;
};
