// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The PS4's own keyboard (libSceImeDialog), over the app: any letter, accents and signs, as for a
// game's name (Rename) or a cover search. The menu falls back to its own keyboard when the
// system's can't open.

#pragma once

#include <cstdint>
#include <string>

namespace SystemKeyboard
{
// Shows the keyboard with a title and the text to edit; false when it can't open.
bool Open(const std::string& title, const std::string& text, uint32_t max_length = 100);
bool IsOpen();

enum class Result
{
  Running,
  Done,       // *text is what was typed
  Cancelled,  // closed without a text
};
// Each frame while open.
Result Poll(std::string* text);
}  // namespace SystemKeyboard
