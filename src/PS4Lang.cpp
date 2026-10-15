// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "PS4Lang.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern "C" void ps4_boot_trace(const char* line);  // port/ps4_crashlog.cpp

namespace PS4Lang
{
namespace
{
// Wii system languages: 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian,
// 6 Dutch, 7 Simplified Chinese, 8 Traditional Chinese, 9 Korean. GameCube: 0 English, 1 German,
// 2 French, 3 Spanish, 4 Italian, 5 Dutch. Languages neither console has: English.
const std::vector<Language> LANGUAGES = {
    {"en", "English", 0, 1},       {"es", "Español", 3, 4},   {"fr", "Français", 2, 3},
    {"de", "Deutsch", 1, 2},       {"it", "Italiano", 4, 5},  {"pt", "Português", 0, 1},
    {"nl", "Nederlands", 5, 6},    {"pl", "Polski", 0, 1},    {"tr", "Türkçe", 0, 1},
    {"ru", "Русский", 0, 1},       {"ja", "日本語", 0, 0},     {"zh", "中文 (简体)", 0, 7},
    {"ko", "한국어", 0, 9},
};

struct Pattern
{
  std::vector<std::string> pieces;  // the English text around each {}
  std::string translation;
  size_t literal = 0;  // longer patterns are tried first
};

std::mutex s_mutex;
const Language* s_current = &LANGUAGES[0];
std::unordered_map<std::string, std::string> s_exact;
std::vector<Pattern> s_patterns;
std::unordered_map<std::string, std::string> s_cache;  // texts a pattern translated
std::unordered_set<std::string> s_untranslated;        // texts without a translation

std::string Unescape(std::string text)
{
  for (size_t at = 0; (at = text.find("\\n", at)) != std::string::npos;)
    text.replace(at, 2, "\n");
  return text;
}

std::vector<std::string> Split(const std::string& english)
{
  std::vector<std::string> pieces;
  size_t start = 0;
  for (size_t at; (at = english.find("{}", start)) != std::string::npos; start = at + 2)
    pieces.push_back(english.substr(start, at - start));
  pieces.push_back(english.substr(start));
  return pieces;
}

// The pattern's {} parts in text, or false.
bool Match(const Pattern& pattern, const std::string& text, std::vector<std::string>* parts)
{
  const auto& pieces = pattern.pieces;
  const std::string& first = pieces.front();
  const std::string& last = pieces.back();
  if (text.size() < first.size() + last.size() + (pieces.size() - 1) ||
      text.compare(0, first.size(), first) != 0 ||
      text.compare(text.size() - last.size(), last.size(), last) != 0)
  {
    return false;
  }
  parts->clear();
  size_t at = first.size();
  const size_t end = text.size() - last.size();
  for (size_t i = 1; i + 1 < pieces.size(); i++)
  {
    // Each part is at least one character; the piece after it is searched from there.
    const size_t found = text.find(pieces[i], at + 1);
    if (found == std::string::npos || found + pieces[i].size() > end)
      return false;
    parts->push_back(text.substr(at, found - at));
    at = found + pieces[i].size();
  }
  if (at >= end)
    return false;
  parts->push_back(text.substr(at, end - at));
  return true;
}

std::string Fill(const std::string& translation, const std::vector<std::string>& parts)
{
  std::string out;
  size_t next = 0;
  for (size_t i = 0; i < translation.size(); i++)
  {
    if (translation[i] == '{' && i + 1 < translation.size())
    {
      if (translation[i + 1] == '}')
      {
        if (next < parts.size())
          out += parts[next];
        next++;
        i++;
        continue;
      }
      if (translation[i + 1] >= '1' && translation[i + 1] <= '9' && i + 2 < translation.size() &&
          translation[i + 2] == '}')
      {
        const size_t index = static_cast<size_t>(translation[i + 1] - '1');
        if (index < parts.size())
          out += parts[index];
        i += 2;
        continue;
      }
    }
    out += translation[i];
  }
  return out;
}
}  // namespace

const std::vector<Language>& All()
{
  return LANGUAGES;
}

const Language& Current()
{
  return *s_current;
}

void Load(const std::string& assets_dir, const std::string& code)
{
  std::lock_guard lock(s_mutex);
  s_current = &LANGUAGES[0];
  for (const Language& language : LANGUAGES)
  {
    if (code == language.code)
      s_current = &language;
  }
  s_exact.clear();
  s_patterns.clear();
  s_cache.clear();
  s_untranslated.clear();
  if (std::string(s_current->code) == "en")
    return;
  const std::string path = assets_dir + "/xmb/lang/" + s_current->code + ".ini";
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const size_t eq = line.find(" = ");
    if (eq == std::string::npos)
      continue;
    const std::string english = Unescape(line.substr(0, eq));
    const std::string translation = Unescape(line.substr(eq + 3));
    if (english.find("{}") == std::string::npos)
    {
      s_exact[english] = translation;
      continue;
    }
    Pattern pattern;
    pattern.pieces = Split(english);
    pattern.translation = translation;
    for (const std::string& piece : pattern.pieces)
      pattern.literal += piece.size();
    s_patterns.push_back(std::move(pattern));
  }
  std::stable_sort(s_patterns.begin(), s_patterns.end(),
                   [](const Pattern& a, const Pattern& b) { return a.literal > b.literal; });
  char text[160];
  snprintf(text, sizeof(text), "language: %s (%zu texts, %zu patterns from %s)", s_current->code,
           s_exact.size(), s_patterns.size(), path.c_str());
  ps4_boot_trace(text);
}

const std::string& T(const std::string& english)
{
  std::lock_guard lock(s_mutex);
  if (s_exact.empty() && s_patterns.empty())
    return english;
  if (const auto it = s_exact.find(english); it != s_exact.end())
    return it->second;
  if (const auto it = s_cache.find(english); it != s_cache.end())
    return it->second;
  if (s_untranslated.count(english))
    return english;
  // Lines joined from pieces ("GameCube  -  Played today  -  2 discs", text over several lines):
  // each piece on its own.
  for (const char* separator : {"\n", "  -  "})
  {
    if (english.find(separator) == std::string::npos)
      continue;
    std::string joined;
    bool changed = false;
    size_t start = 0;
    const size_t length = std::char_traits<char>::length(separator);
    while (true)
    {
      const size_t at = english.find(separator, start);
      const std::string piece = english.substr(start, at == std::string::npos ? at : at - start);
      s_mutex.unlock();
      const std::string translated = T(piece);
      s_mutex.lock();
      changed = changed || translated != piece;
      joined += translated;
      if (at == std::string::npos)
        break;
      joined += separator;
      start = at + length;
    }
    if (changed)
      return s_cache.emplace(english, joined).first->second;
    break;
  }
  std::vector<std::string> parts;
  for (const Pattern& pattern : s_patterns)
  {
    if (Match(pattern, english, &parts))
    {
      // The parts may be translatable themselves ("Settings: On").
      for (std::string& part : parts)
      {
        if (const auto it = s_exact.find(part); it != s_exact.end())
          part = it->second;
      }
      return s_cache.emplace(english, Fill(pattern.translation, parts)).first->second;
    }
  }
  // Texts that change all the time (progress, clocks) would fill this up: start over now and then.
  if (s_untranslated.size() > 20000)
    s_untranslated.clear();
  s_untranslated.insert(english);
  return english;
}

const char* T(const char* english)
{
  if (!english)
    return english;
  {
    std::lock_guard lock(s_mutex);
    if (s_exact.empty() && s_patterns.empty())
      return english;
  }
  const std::string text(english);
  const std::string& translated = T(text);
  // An untranslated text comes back as the temporary above: return the caller's pointer.
  return &translated == &text ? english : translated.c_str();
}

std::vector<std::string> ExtraFonts(const std::string& assets_dir, bool bold)
{
  // Japanese, Chinese and Korean draw the same Han characters differently: the current
  // language's font first, so its shapes win; the others for the language names in Settings.
  std::vector<std::string> order = {"ja", "zh", "ko"};
  const std::string current = s_current->code;
  if (const auto it = std::find(order.begin(), order.end(), current); it != order.end())
    std::rotate(order.begin(), it, it + 1);
  std::vector<std::string> fonts;
  for (const std::string& code : order)
  {
    const char* name = code == "ja" ? "NotoSansJP" : code == "zh" ? "NotoSansSC" : "NotoSansKR";
    const std::string path =
        assets_dir + "/xmb/fonts/" + name + (bold ? "-Bold" : "-Regular") + ".ttf";
    if (std::ifstream(path).good())
      fonts.push_back(path);
  }
  return fonts;
}
}  // namespace PS4Lang
