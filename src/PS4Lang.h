// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Languages for the PS4 menus (Library/XMB, the in-game menu, settings): Settings -> Language.
//
// The menus are written in English and translated where they draw text: T() looks the text up in
// <assets>/xmb/lang/<code>.ini ("English = translation" lines, UTF-8). "{}" in an English line
// stands for any text (a number, a name), repeated in the translation as {} in order or as
// {1}..{9}; "\n" is a line break. Text without a translation (game titles, file names) is drawn
// as it is. The language also sets the GameCube / Wii system language where they have it.

#pragma once

#include <string>
#include <vector>

namespace PS4Lang
{
struct Language
{
  const char* code;  // "en", "ja", ...; the file xmb/lang/<code>.ini
  const char* name;  // in its own language
  int gc_language;   // GameCube system language (SRAM): English 0, German 1, French 2, ...
  int wii_language;  // Wii system language (SYSCONF IPL.LNG): Japanese 0, English 1, ...
};

const std::vector<Language>& All();
const Language& Current();

// Loads the language (settings.ini "language"); English needs no file. Call before drawing.
void Load(const std::string& assets_dir, const std::string& code);

// The text in the current language (cached). The result stays valid until the next Load.
const std::string& T(const std::string& english);
const char* T(const char* english);

// Font files with the glyphs the menus need beyond Nunito's (Japanese, Chinese, Korean - only
// the characters the translations use), the current language's first. regular or bold.
std::vector<std::string> ExtraFonts(const std::string& assets_dir, bool bold);
}  // namespace PS4Lang
