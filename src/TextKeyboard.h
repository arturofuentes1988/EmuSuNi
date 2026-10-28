// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The app's keyboard (00.80, the owner: "normalizar la escritura de teclado en la app, ya que hay un teclado en que
// elijo la letra con X y otro teclado en que elijo la letra con círculo"). Every text the app asks for is typed on it -
// the games' search (R2), a game's Rename, Find Cover's Look For, RetroAchievements' user name and password - with the
// menu's buttons: Cross types the selected key, Circle goes back, Square deletes, Triangle types a space. (Until 00.79
// a name or a login was typed on the PS4's own keyboard, libSceImeDialog: it chooses a key with the console's Enter
// button - Circle on some consoles - and its settings have no way to say otherwise.)
// Its keys, pages and Shift, the text typed and the selected key's moves are here, without the drawing (src/Menu.cpp
// draws it and hands it the buttons): the PC's unit tests run them as they are.

#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "MenuNav.h"

namespace TextKeyboard
{
// What is typed.
enum class Mode
{
  Search,    // the games' search: the lists follow it as it is typed; the letters as capitals (the search doesn't tell
             // them apart), Done
  Text,      // a name (Rename, Look For): a capital by itself at each word's start; accents, signs; OK / Cancel
  Plain,     // a user name: no capital by itself
  Password,  // the same, shown as dots (Show shows it)
};

// What a key or a button does.
enum class Action
{
  None,
  Char,    // the key's character
  Space,
  Delete,  // the last character
  Clear,   // every character
  Done,    // the search: the keyboard closes, the search stays
  Ok,      // the text as typed
  Cancel,  // closed without it (the search: closed, the search as it is)
  Shift,   // capitals: the next letter, every letter, none
  Page,    // another page: the letters, the signs, the accents
  Show,    // a password shown, or dots again
  Browse,  // the search: the games found, one by one
};

// The controller's buttons as the keyboard knows them (the menu's own become these).
enum class Button
{
  Cross,
  Circle,
  Square,
  Triangle,
  L1,
  R1,
  L2,
  R2,
  Options,
};

constexpr int COLUMNS = 10;   // keys in a row
constexpr int CHAR_ROWS = 4;  // the rows of characters; the wide keys' rows are under them

// A key over several columns, with the button that does the same (PS4Glyphs' name; nullptr: none).
struct WideKey
{
  const char* label;
  int first, span;  // columns
  const char* glyph;
  Action action;
};

// The search's keys (as before 00.80).
constexpr const char* SEARCH_ROWS[CHAR_ROWS] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM'&."};
constexpr WideKey SEARCH_KEYS[] = {{"Space", 0, 3, "Triangle", Action::Space},
                                   {"Delete", 3, 2, "Square", Action::Delete},
                                   {"Clear All", 5, 3, "L2", Action::Clear},
                                   {"Done", 8, 2, "R2", Action::Done}};

// The other modes' pages: four rows of ten characters (UTF-8), as typed without Shift and with it. The letters (the
// ñ after the l, as on a Spanish keyboard), the signs (every one of ASCII's: a password may have any), the accents.
struct Page
{
  const char* name;  // on the key that brings it
  const char* lower[CHAR_ROWS];
  const char* upper[CHAR_ROWS];
};
constexpr Page PAGES[] = {
    {"abc",
     {"1234567890", "qwertyuiop", "asdfghjklñ", "zxcvbnm,.-"},
     {"1234567890", "QWERTYUIOP", "ASDFGHJKLÑ", "ZXCVBNM,.-"}},
    {"#+=",
     {"!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|", "}~€£¥°§·«»"},
     {"!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|", "}~€£¥°§·«»"}},
    {"àé",
     {"áéíóúàèìòù", "äëïöüâêîôû", "çãõåæøœßÿł", "āēīōūý¡¿ºª"},
     {"ÁÉÍÓÚÀÈÌÒÙ", "ÄËÏÖÜÂÊÎÔÛ", "ÇÃÕÅÆØŒßŸŁ", "ĀĒĪŌŪÝ¡¿ºª"}},
};
constexpr int PAGE_COUNT = static_cast<int>(std::size(PAGES));

// Under the characters: Shift, the next page, Space, Delete; then Clear All, Cancel and OK (a password's: Show too).
constexpr WideKey EDIT_KEYS[] = {{"Shift", 0, 2, "L2", Action::Shift},
                                 {"", 2, 2, "R1", Action::Page},  // (its label: the page it brings, Board::Label)
                                 {"Space", 4, 3, "Triangle", Action::Space},
                                 {"Delete", 7, 3, "Square", Action::Delete}};
constexpr WideKey END_KEYS[] = {{"Clear All", 0, 3, nullptr, Action::Clear},
                                {"Cancel", 3, 3, "Circle", Action::Cancel},
                                {"OK", 6, 4, "R2", Action::Ok}};
constexpr WideKey PASSWORD_END_KEYS[] = {{"Show", 0, 2, nullptr, Action::Show},
                                         {"Clear All", 2, 2, nullptr, Action::Clear},
                                         {"Cancel", 4, 3, "Circle", Action::Cancel},
                                         {"OK", 7, 3, "R2", Action::Ok}};

// What a button does (Cross: the selected key, Board::Press). The same in every mode but for the shoulders: the search
// has the games to browse and Clear All, the others the pages and Shift.
constexpr Action ButtonAction(Mode mode, Button button)
{
  const bool search = mode == Mode::Search;
  switch (button)
  {
  case Button::Circle:
    return Action::Cancel;
  case Button::Square:
    return Action::Delete;
  case Button::Triangle:
    return Action::Space;
  case Button::L1:
  case Button::R1:
    return search ? Action::Browse : Action::Page;
  case Button::L2:
    return search ? Action::Clear : Action::Shift;
  case Button::R2:
  case Button::Options:
    return search ? Action::Done : Action::Ok;
  default:
    return Action::None;
  }
}

constexpr const char* GlyphOf(Button button)
{
  constexpr const char* NAMES[] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Options"};
  return NAMES[static_cast<int>(button)];
}

// An action's name on the hints line (the wide keys say the same words).
constexpr const char* ActionName(Mode mode, Action action)
{
  switch (action)
  {
  case Action::Char:
    return "Type";
  case Action::Space:
    return "Space";
  case Action::Delete:
    return "Delete";
  case Action::Clear:
    return "Clear All";
  case Action::Done:
    return "Done";
  case Action::Ok:
    return "OK";
  case Action::Cancel:
    return mode == Mode::Search ? "Close" : "Cancel";
  case Action::Shift:
    return "Shift";
  case Action::Page:
    return "Pages";
  case Action::Show:
    return "Show";
  case Action::Browse:
    return "Browse Games";
  default:
    return "";
  }
}

// The hints line: a button (PS4Glyphs' name; "L1/R1" both) and what it does - the Library's line has the first three
// at its left, the others at its right.
struct Hint
{
  const char* glyph;
  const char* label;
  bool right;
};
inline std::vector<Hint> Hints(Mode mode)
{
  std::vector<Hint> hints = {{"D-Pad", "Move", false},
                             {"Cross", ActionName(mode, Action::Char), false},
                             {"L1/R1", ActionName(mode, ButtonAction(mode, Button::R1)), false}};
  for (const Button button : {Button::R2, Button::L2, Button::Triangle, Button::Square, Button::Circle})
    hints.push_back({GlyphOf(button), ActionName(mode, ButtonAction(mode, button)), true});
  return hints;
}

// UTF-8: a character's bytes from its first one; the characters in a text; a row's character; the last one off.
inline size_t CharBytes(unsigned char lead)
{
  return lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : 4;
}
inline size_t CountChars(const std::string& text)
{
  size_t count = 0;
  for (const char c : text)
    count += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  return count;
}
inline std::string CharAt(const std::string& row, int index)
{
  size_t at = 0;
  for (int i = 0; at < row.size(); i++)
  {
    const size_t bytes = std::min(CharBytes(static_cast<unsigned char>(row[at])), row.size() - at);
    if (i == index)
      return row.substr(at, bytes);
    at += bytes;
  }
  return "";
}
inline void PopChar(std::string& text)
{
  while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
    text.pop_back();
  if (!text.empty())
    text.pop_back();
}

// Shift: off, the next letter, every letter.
enum class Caps
{
  Off,
  Once,
  Lock,
};

// What a press did (the menu plays its sound, closes the keyboard...).
enum class Result
{
  None,     // nothing for it to do
  Typed,    // a character or a space more
  Erased,   // one less, or all
  Toggled,  // Shift, the page, Show
  Refused,  // full, nothing to delete, a space where none goes, OK on nothing
  Done,     // the search's Done
  Ok,       // OK: the text is Take()n
  Cancel,
  Browse,
};

class Board
{
public:
  // Opens on `text` (the search's, a game's name...): at most `max_chars` characters.
  void Open(Mode mode, std::string text, size_t max_chars)
  {
    Wipe();
    m_mode = mode;
    m_text = std::move(text);
    m_max = max_chars;
    m_page = 0;
    m_caps = Caps::Off;
    m_shown = false;
    AutoCaps();
    // (The search keeps its key between two openings; a text asked for starts on the Q.)
    if (mode != Mode::Search || m_row >= Rows())
    {
      m_row = 1;
      m_col = 0;
    }
  }
  Mode GetMode() const { return m_mode; }
  int Rows() const { return CHAR_ROWS + (m_mode == Mode::Search ? 1 : 2); }
  int Row() const { return m_row; }
  int Column() const { return m_col; }
  int PageIndex() const { return m_page; }
  Caps GetCaps() const { return m_caps; }
  bool Shown() const { return m_shown; }
  const std::string& Text() const { return m_text; }
  size_t Length() const { return CountChars(m_text); }
  size_t Max() const { return m_max; }
  // The search changed without the keyboard (Circle in the games, the tour): the keyboard's text follows.
  void SetText(const std::string& text) { m_text = text; }
  // The text as shown: a password's characters as dots, unless Show.
  std::string Display() const
  {
    if (m_mode != Mode::Password || m_shown)
      return m_text;
    std::string dots;
    for (size_t i = 0, n = Length(); i < n; i++)
      dots += "\xE2\x80\xA2";  // (U+2022)
    return dots;
  }
  // The text, taken: the keyboard keeps nothing of it (a password).
  std::string Take()
  {
    std::string text = m_text;
    Wipe();
    return text;
  }

  // A key of the character rows, as Cross types it now.
  std::string Char(int row, int column) const
  {
    if (row < 0 || row >= CHAR_ROWS || column < 0 || column >= COLUMNS)
      return "";
    if (m_mode == Mode::Search)
      return CharAt(SEARCH_ROWS[row], column);
    const Page& page = PAGES[m_page];
    return CharAt(m_caps != Caps::Off ? page.upper[row] : page.lower[row], column);
  }
  // A row's wide keys (the rows after the characters'), how many in *count.
  const WideKey* WideKeys(int row, int* count) const
  {
    const WideKey* keys = nullptr;
    int n = 0;
    if (m_mode == Mode::Search && row == CHAR_ROWS)
    {
      keys = SEARCH_KEYS;
      n = static_cast<int>(std::size(SEARCH_KEYS));
    }
    else if (m_mode != Mode::Search && row == CHAR_ROWS)
    {
      keys = EDIT_KEYS;
      n = static_cast<int>(std::size(EDIT_KEYS));
    }
    else if (m_mode == Mode::Password && row == CHAR_ROWS + 1)
    {
      keys = PASSWORD_END_KEYS;
      n = static_cast<int>(std::size(PASSWORD_END_KEYS));
    }
    else if (m_mode != Mode::Search && row == CHAR_ROWS + 1)
    {
      keys = END_KEYS;
      n = static_cast<int>(std::size(END_KEYS));
    }
    if (count)
      *count = n;
    return keys;
  }
  // Which of a row's wide keys is over a column.
  int WideAt(int row, int column) const
  {
    int count = 0;
    const WideKey* keys = WideKeys(row, &count);
    for (int i = 0; i < count; i++)
    {
      if (column >= keys[i].first && column < keys[i].first + keys[i].span)
        return i;
    }
    return 0;
  }
  // A wide key's label as shown: the page key names the page it brings; Show says Hide while the password shows.
  std::string Label(const WideKey& key) const
  {
    if (key.action == Action::Page)
      return PAGES[(m_page + 1) % PAGE_COUNT].name;
    if (key.action == Action::Show && m_shown)
      return "Hide";
    return key.label;
  }
  // The selected key's action (and character).
  Action Selected(std::string* ch = nullptr) const
  {
    if (m_row < CHAR_ROWS)
    {
      if (ch)
        *ch = Char(m_row, m_col);
      return Action::Char;
    }
    int count = 0;
    const WideKey* keys = WideKeys(m_row, &count);
    return count > 0 ? keys[WideAt(m_row, m_col)].action : Action::None;
  }
  bool IsSelected(int row, int column) const
  {
    if (row != m_row)
      return false;
    return row < CHAR_ROWS ? column == m_col : WideAt(row, column) == WideAt(m_row, m_col);
  }

  // The selected key moves `rows` up (-1) or down (+1), or `columns` left or right (a wide key at a time); past an
  // edge it goes round on a fresh press (`wrap`), a held one stops there (src/MenuNav.h). True if it moved.
  bool Move(int rows, int columns, bool wrap)
  {
    if (rows != 0)
    {
      const int to = MenuNav::Step(m_row, rows, Rows(), wrap);
      const bool moved = to != m_row;
      m_row = to;
      return moved;
    }
    if (columns == 0)
      return false;
    if (m_row < CHAR_ROWS)
    {
      const int to = MenuNav::Step(m_col, columns, COLUMNS, wrap);
      const bool moved = to != m_col;
      m_col = to;
      return moved;
    }
    int count = 0;
    const WideKey* keys = WideKeys(m_row, &count);
    const int key = WideAt(m_row, m_col), to = MenuNav::Step(key, columns, count, wrap);
    if (to == key || count == 0)
      return false;
    m_col = keys[to].first;
    return true;
  }

  // Cross: the selected key.
  Result Press()
  {
    std::string ch;
    const Action action = Selected(&ch);
    return action == Action::Char ? Type(ch) : Do(action);
  }
  // A key's or a button's action (the pages: `direction`, round past the last on a fresh press, `wrap`).
  Result Do(Action action, int direction = 1, bool wrap = true)
  {
    const bool search = m_mode == Mode::Search;
    switch (action)
    {
    case Action::Space:
      // (No space first or after another in a search or a name; a password may have them anywhere.)
      if (Length() >= m_max || ((search || m_mode == Mode::Text) && (m_text.empty() || m_text.back() == ' ')))
        return Result::Refused;
      m_text += ' ';
      AutoCaps();
      return Result::Typed;
    case Action::Delete:
      if (m_text.empty())
        return Result::Refused;
      PopChar(m_text);
      AutoCaps();
      return Result::Erased;
    case Action::Clear:
      if (m_text.empty())
        return Result::Refused;
      Wipe();
      AutoCaps();
      return Result::Erased;
    case Action::Done:
      return search ? Result::Done : Result::None;
    case Action::Ok:
      if (search)
        return Result::None;
      // (A user name, a password: OK only with something typed. A name may be left empty: Rename's own name again.)
      if (m_mode != Mode::Text && m_text.empty())
        return Result::Refused;
      while (m_mode == Mode::Text && !m_text.empty() && m_text.back() == ' ')
        m_text.pop_back();
      return Result::Ok;
    case Action::Cancel:
      return Result::Cancel;
    case Action::Shift:
      if (search)
        return Result::None;
      m_caps = m_caps == Caps::Off ? Caps::Once : m_caps == Caps::Once ? Caps::Lock : Caps::Off;
      return Result::Toggled;
    case Action::Page:
    {
      if (search)
        return Result::None;
      const int to = MenuNav::Step(m_page, direction < 0 ? -1 : 1, PAGE_COUNT, wrap);
      if (to == m_page)
        return Result::None;
      m_page = to;
      return Result::Toggled;
    }
    case Action::Show:
      if (m_mode != Mode::Password)
        return Result::None;
      m_shown = !m_shown;
      return Result::Toggled;
    case Action::Browse:
      return search ? Result::Browse : Result::None;
    default:
      return Result::None;
    }
  }
  // A character typed (the selected key's).
  Result Type(const std::string& ch)
  {
    if (ch.empty() || Length() >= m_max)
      return Result::Refused;
    m_text += ch;
    // (Shift once: one letter - a digit or a sign leaves it on.)
    if (m_caps == Caps::Once && Cased(ch))
      m_caps = Caps::Off;
    return Result::Typed;
  }

private:
  // A character Shift changes (a letter of a page).
  static bool Cased(const std::string& ch)
  {
    for (const Page& page : PAGES)
    {
      for (int row = 0; row < CHAR_ROWS; row++)
      {
        for (int column = 0; column < COLUMNS; column++)
        {
          const std::string lower = CharAt(page.lower[row], column), upper = CharAt(page.upper[row], column);
          if (lower != upper && (ch == lower || ch == upper))
            return true;
        }
      }
    }
    return false;
  }
  // A name's capitals by themselves: at its start and after a space (unless Shift is on for every letter).
  void AutoCaps()
  {
    if (m_mode == Mode::Text && m_caps != Caps::Lock)
      m_caps = m_text.empty() || m_text.back() == ' ' ? Caps::Once : Caps::Off;
  }
  void Wipe()
  {
    std::fill(m_text.begin(), m_text.end(), '\0');
    m_text.clear();
  }

  Mode m_mode = Mode::Search;
  std::string m_text;
  size_t m_max = 40;
  int m_row = 1, m_col = 0, m_page = 0;
  Caps m_caps = Caps::Off;
  bool m_shown = false;
};
}  // namespace TextKeyboard
