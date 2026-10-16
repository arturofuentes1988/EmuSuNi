// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "SystemKeyboard.h"

#include <algorithm>

#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include "Log.h"

namespace SystemKeyboard
{
namespace
{
// The dialog's texts are UTF-16 (wchar_t is 4 bytes for this compiler, 2 for the PS4's libraries).
uint16_t s_text[256];
uint16_t s_title[128];
bool s_loaded = false, s_failed = false, s_open = false;

bool Load()
{
  if (s_loaded || s_failed)
    return s_loaded;
  // Calling into a module that isn't loaded kills the app (main.cpp): load both first.
  int result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
  if (result == 0)
    result = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
  if (result != 0)
  {
    LogF("keyboard: the system keyboard's modules can't be loaded (%#x)", static_cast<unsigned>(result));
    s_failed = true;
    return false;
  }
  result = sceCommonDialogInitialize();
  LogF("keyboard: common dialog %#x", static_cast<unsigned>(result));  // "already" is fine too
  s_loaded = true;
  return true;
}

// UTF-8 -> UTF-16 (the Basic Multilingual Plane: every letter a game's name needs).
void ToUtf16(const std::string& text, uint16_t* out, size_t capacity)
{
  size_t n = 0;
  for (size_t i = 0; i < text.size() && n + 1 < capacity;)
  {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + length > text.size())
      break;  // a cut-off character
    uint32_t code = length == 1 ? c : length == 2 ? (c & 0x1Fu) : length == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < length; k++)
      code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
    i += length;
    out[n++] = code > 0xFFFF ? static_cast<uint16_t>('?') : static_cast<uint16_t>(code);
  }
  out[n] = 0;
}

std::string FromUtf16(const uint16_t* text)
{
  std::string out;
  for (; *text; text++)
  {
    const uint32_t code = *text;
    if (code < 0x80)
    {
      out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }
  return out;
}
}  // namespace

bool Open(const std::string& title, const std::string& text, uint32_t max_length)
{
  if (s_open || !Load())
    return false;
  ToUtf16(text, s_text, sizeof(s_text) / sizeof(s_text[0]));
  ToUtf16(title, s_title, sizeof(s_title) / sizeof(s_title[0]));
  int32_t user = -1;
  sceUserServiceGetInitialUser(&user);
  OrbisImeDialogSetting setting = {};
  setting.userId = static_cast<uint32_t>(user);
  setting.type = ORBIS_TYPE_DEFAULT;
  setting.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
  setting.inputMethod = ORBIS__DEFAULT;
  setting.maxTextLength = std::min<uint32_t>(max_length, sizeof(s_text) / sizeof(s_text[0]) - 1);
  setting.inputTextBuffer = reinterpret_cast<wchar_t*>(s_text);
  setting.posx = 960.0f;
  setting.posy = 540.0f;
  setting.horizontalAlignment = ORBIS_H_CENTER;
  setting.verticalAlignment = ORBIS_V_CENTER;
  setting.title = reinterpret_cast<const wchar_t*>(s_title);
  const int result = sceImeDialogInit(&setting, nullptr);
  if (result != 0)
  {
    LogF("keyboard: the system keyboard didn't open (%#x)", static_cast<unsigned>(result));
    return false;
  }
  s_open = true;
  return true;
}

bool IsOpen()
{
  return s_open;
}

Result Poll(std::string* text)
{
  if (!s_open)
    return Result::Cancelled;
  if (sceImeDialogGetStatus() == ORBIS_DIALOG_STATUS_RUNNING)
    return Result::Running;
  OrbisDialogResult result = {};
  sceImeDialogGetResult(&result);
  sceImeDialogTerm();
  s_open = false;
  if (result.endstatus != ORBIS_DIALOG_OK)
    return Result::Cancelled;
  *text = FromUtf16(s_text);
  return Result::Done;
}
}  // namespace SystemKeyboard
