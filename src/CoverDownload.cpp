// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "CoverDownload.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#include <orbis/libkernel.h>

#include "Image.h"
#include "Log.h"
#include "Net.h"
#include "Saves.h"

namespace CoverDownload
{
namespace
{
// libretro's thumbnail server, the one RetroArch uses: the same pictures as the GitHub project, over
// plain HTTP. The PS4's TLS fails on raw.githubusercontent.com ("RSA - The PKCS#1 verification
// failed" in every handshake), so GitHub is only the second choice.
constexpr const char* SERVER = "http://thumbnails.libretro.com/";
// The size the menu shows a cover at (the Library's selected one): bigger pictures are made
// smaller once, when they are downloaded, instead of every time the menu reads them.
constexpr uint32_t COVER_W = 600, COVER_H = 430;

// The pictures tried, in order: the box, then the game's title screen (better than no picture).
struct Kind
{
  const char* folder;  // libretro's folder
  const char* cache;   // its list of names, kept a week in covers/ (covers/nes/...)
  const char* label;   // for the log
};
constexpr Kind KINDS[] = {{"Named_Boxarts", ".libretro-thumbnails.txt", "box"},
                          {"Named_Titles", ".libretro-titles.txt", "title screen"}};
constexpr int KIND_COUNT = 2;

// A console's folder of a kind of pictures (Systems.h: thumbnails).
std::string FolderUrl(const Kind& kind, const System& system)
{
  return std::string(SERVER) + Net::UrlEncode(system.thumbnails) + "/" + kind.folder + "/";
}

void Log(const std::string& text)
{
  LogLine("covers: " + text);
}

// "%20" -> " ", "&amp;" -> "&" (a link in the server's folder listing).
std::string DecodeHref(const std::string& href)
{
  static const std::pair<const char*, char> ENTITIES[] = {
      {"&amp;", '&'}, {"&#39;", '\''}, {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}};
  std::string html;
  for (size_t i = 0; i < href.size(); i++)
  {
    if (href[i] == '&')
    {
      bool known = false;
      for (const auto& [entity, ch] : ENTITIES)
      {
        const size_t length = std::char_traits<char>::length(entity);
        if (href.compare(i, length, entity) == 0)
        {
          html += ch;
          i += length - 1;
          known = true;
          break;
        }
      }
      if (known)
        continue;
    }
    html += href[i];
  }
  std::string out;
  for (size_t i = 0; i < html.size(); i++)
  {
    if (html[i] == '%' && i + 2 < html.size() && std::isxdigit(static_cast<unsigned char>(html[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(html[i + 2])))
    {
      out += static_cast<char>(std::stoi(html.substr(i + 1, 2), nullptr, 16));
      i += 2;
    }
    else
    {
      out += html[i];
    }
  }
  return out;
}

// The .png links of the server's folder listing, without ".png".
std::vector<std::string> ListedPictures(const std::string& html)
{
  std::vector<std::string> names;
  for (size_t at = html.find("href=\""); at != std::string::npos; at = html.find("href=\"", at + 6))
  {
    const size_t start = at + 6;
    const size_t end = html.find('"', start);
    if (end == std::string::npos)
      break;
    const std::string name = DecodeHref(html.substr(start, end - start));
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0 &&
        name.find('/') == std::string::npos && name.find('?') == std::string::npos)
    {
      names.push_back(name.substr(0, name.size() - 4));
    }
  }
  return names;
}

// The JSON strings after "path": in a GitHub tree listing.
std::vector<std::string> Paths(const std::string& json, std::vector<std::string>* types,
                               std::vector<std::string>* shas)
{
  std::vector<std::string> paths;
  auto read_string = [&](size_t at, size_t* end) {
    std::string value;
    size_t i = at;
    for (; i < json.size() && json[i] != '"'; i++)
    {
      if (json[i] == '\\' && i + 1 < json.size())
      {
        const char next = json[++i];
        if (next == 'u' && i + 4 < json.size())
        {
          const unsigned code = std::stoul(json.substr(i + 1, 4), nullptr, 16);
          i += 4;
          if (code < 0x80)
          {
            value += static_cast<char>(code);
          }
          else if (code < 0x800)
          {
            value += static_cast<char>(0xC0 | (code >> 6));
            value += static_cast<char>(0x80 | (code & 0x3F));
          }
          else
          {
            value += static_cast<char>(0xE0 | (code >> 12));
            value += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            value += static_cast<char>(0x80 | (code & 0x3F));
          }
        }
        else
        {
          value += next == 'n' ? '\n' : next;
        }
      }
      else
      {
        value += json[i];
      }
    }
    *end = i;
    return value;
  };
  for (size_t at = json.find("\"path\""); at != std::string::npos; at = json.find("\"path\"", at + 6))
  {
    const size_t quote = json.find('"', json.find(':', at) + 1);
    if (quote == std::string::npos)
      break;
    size_t end = quote;
    paths.push_back(read_string(quote + 1, &end));
    // "type" and "sha" follow in the same object.
    const size_t object_end = json.find('}', end);
    auto field = [&](const char* name) {
      const size_t f = json.find(std::string("\"") + name + "\"", end);
      if (f == std::string::npos || f > object_end)
        return std::string();
      const size_t q = json.find('"', json.find(':', f) + 1);
      size_t e = q;
      return read_string(q + 1, &e);
    };
    if (types)
      types->push_back(field("type"));
    if (shas)
      shas->push_back(field("sha"));
  }
  return paths;
}

// The names of a console's kind of pictures (without ".png"): from the list kept in covers/ (a
// week), else from the server's folder listing, else from GitHub.
std::vector<std::string> Names(const Kind& kind, const System& system, const std::string& covers_dir,
                               const std::string& ca_file, std::string* error, Net::Session* session)
{
  // One at a time per list (Download and Find Cover's thread): the second reads the list kept.
  static std::mutex mutexes[SYSTEM_COUNT][KIND_COUNT];
  std::lock_guard lock(mutexes[static_cast<int>(system.id)][&kind - KINDS]);
  const std::string cache = covers_dir + "/" + system.prefix + kind.cache;
  const int64_t age = std::time(nullptr) - Saves::Modified(cache);
  std::vector<std::string> names;
  if (Saves::Modified(cache) > 0 && age < 7 * 24 * 3600)
  {
    std::istringstream in(ReadTextFile(cache));
    std::string line;
    while (std::getline(in, line))
    {
      if (!line.empty())
        names.push_back(line);
    }
    if (!names.empty())
      return names;
  }
  auto keep = [&](const char* where) {
    std::string text;
    for (const std::string& name : names)
      text += name + "\n";
    WriteTextFile(cache, text);
    Log(std::to_string(names.size()) + " " + system.tab + " " + kind.label + " pictures listed by " + where);
  };
  {
    Net::Request request;
    request.url = FolderUrl(kind, system);
    std::string html, server_error;
    const long status = Net::GetText(request, &html, 16 << 20, &server_error, session);
    if (status == 200)
      names = ListedPictures(html);
    if (!names.empty())
    {
      keep("thumbnails.libretro.com");
      return names;
    }
    Log("thumbnails.libretro.com: " + (server_error.empty() ? "HTTP " + std::to_string(status) : server_error) +
        "; asking GitHub");
  }
  Net::Request request;
  request.ca_file = ca_file;
  request.headers = {"Accept: application/vnd.github+json"};
  std::string root;
  for (const char* branch : {"master", "main"})
  {
    request.url = std::string("https://api.github.com/repos/") + system.thumbnails_repo + "/git/trees/" + branch;
    if (Net::GetText(request, &root, 1 << 20, error, session) == 200)
      break;
    root.clear();
  }
  if (root.empty())
  {
    if (error->empty())
      *error = "GitHub didn't answer";
    return {};
  }
  std::vector<std::string> types, shas;
  const std::vector<std::string> top = Paths(root, &types, &shas);
  std::string sha;
  for (size_t i = 0; i < top.size(); i++)
  {
    if (top[i] == kind.folder && i < shas.size())
      sha = shas[i];
  }
  if (sha.empty())
  {
    *error = std::string("no ") + kind.folder + " folder in libretro's thumbnails";
    return {};
  }
  std::string listing;
  request.url = std::string("https://api.github.com/repos/") + system.thumbnails_repo + "/git/trees/" + sha;
  if (Net::GetText(request, &listing, 16 << 20, error, session) != 200)
  {
    if (error->empty())
      *error = "the list of pictures couldn't be read";
    return {};
  }
  for (const std::string& path : Paths(listing, nullptr, nullptr))
  {
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0)
      names.push_back(path.substr(0, path.size() - 4));
  }
  keep("GitHub");
  return names;
}

// ---- A game's file name against libretro's picture names ----
// The file names differ: GoodSNES "Mega Man VII (U) [!]" and "WildSnake (U)", libretro (No-Intro)
// "Mega Man 7 (USA)" and "Wild Snake (USA)"; "&" is "and" in one and "_" in the other; a title
// is sometimes the other one's subtitle ("P.T.O. - Pacific Theater of Operations"). Names are
// compared by their words, then by their parts, then by close spellings.

// The title: the name before its first "(" or "[".
std::string TitlePart(const std::string& name)
{
  for (size_t i = 1; i < name.size(); i++)
  {
    if (name[i] == '(' || name[i] == '[')
      return name.substr(0, i);
  }
  return name;
}

// Lower-case words: ' and . dropped ("Jr.'s" = "Jrs"), any other sign splits words ("Spider-Man"),
// Roman numbers II to IX as digits ("Mega Man VII" = "Mega Man 7"; not X: "Mega Man X"), and "the",
// "a", "an" and "and" left out.
std::vector<std::string> Words(const std::string& text)
{
  static const std::map<std::string, std::string> ROMAN = {
      {"ii", "2"}, {"iii", "3"}, {"iv", "4"}, {"v", "5"}, {"vi", "6"}, {"vii", "7"}, {"viii", "8"}, {"ix", "9"}};
  std::vector<std::string> words;
  std::string word;
  auto flush = [&] {
    if (word.empty())
      return;
    if (const auto it = ROMAN.find(word); it != ROMAN.end())
      word = it->second;
    if (word != "the" && word != "a" && word != "an" && word != "and")
      words.push_back(word);
    word.clear();
  };
  for (const char ch : text)
  {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '\'' || c == '.')
      continue;
    if (c == '$')
      word += 's';  // "Buckeroo$!"
    else if (std::isalnum(c))
      word += static_cast<char>(std::tolower(c));
    else
      flush();
  }
  flush();
  return words;
}

std::string Joined(const std::vector<std::string>& words, const char* separator = "")
{
  std::string out;
  for (const std::string& word : words)
    out += (out.empty() ? "" : separator) + word;
  return out;
}

// The numbers among the words ("Mega Man X2" and "Mega Man X" are different games).
std::vector<std::string> Numbers(const std::vector<std::string>& words)
{
  std::vector<std::string> numbers;
  for (const std::string& word : words)
  {
    if (std::all_of(word.begin(), word.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
      numbers.push_back(word);
  }
  std::sort(numbers.begin(), numbers.end());
  return numbers;
}

// The parts of a title around " - ".
std::vector<std::string> Parts(const std::string& title)
{
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t at; (at = title.find(" - ", start)) != std::string::npos; start = at + 3)
    parts.push_back(title.substr(start, at - start));
  parts.push_back(title.substr(start));
  return parts;
}

// Twice the longest common subsequence over both lengths: 1 for the same text.
double Similarity(const std::string& a, const std::string& b)
{
  if (a.empty() || b.empty())
    return 0.0;
  std::vector<int> previous(b.size() + 1, 0), row(b.size() + 1, 0);
  for (size_t i = 1; i <= a.size(); i++)
  {
    for (size_t j = 1; j <= b.size(); j++)
      row[j] = a[i - 1] == b[j - 1] ? previous[j - 1] + 1 : std::max(previous[j], row[j - 1]);
    std::swap(previous, row);
  }
  return 2.0 * previous[b.size()] / static_cast<double>(a.size() + b.size());
}

class NameIndex
{
public:
  // `names` must outlive the index.
  explicit NameIndex(const std::vector<std::string>& names)
  {
    for (const std::string& name : names)
    {
      const std::string title = TitlePart(name);
      std::vector<std::string> words = Words(title);
      const std::string key = Joined(words);
      if (key.empty())
        continue;
      Entry& entry = m_names[key];
      if (entry.names.empty())
        entry.words = words;
      entry.names.push_back(&name);
      std::sort(words.begin(), words.end());
      m_sorted[Joined(words, " ")].push_back(&name);
      const std::vector<std::string> parts = Parts(title);
      if (parts.size() > 1)
      {
        m_first[Joined(Words(parts[0]))].push_back(&name);
        for (size_t i = 1; i < parts.size(); i++)
        {
          const std::string part = Joined(Words(parts[i]));
          if (part.size() >= 6)
            m_subtitle[part].push_back(&name);
        }
      }
    }
  }

  // The pictures of the game (its name in every region and revision), and how they were found.
  std::vector<const std::string*> Find(const std::string& stem, std::string* how) const
  {
    const std::string title = TitlePart(stem);
    const std::vector<std::string> words = Words(title);
    const std::string key = Joined(words);
    if (key.empty())
      return {};
    auto found = [&](const std::vector<const std::string*>& names, const char* way) {
      *how = way;
      return names;
    };
    if (const auto it = m_names.find(key); it != m_names.end())
      return found(it->second.names, "same name");
    {
      std::vector<std::string> sorted = words;
      std::sort(sorted.begin(), sorted.end());
      if (const auto it = m_sorted.find(Joined(sorted, " ")); it != m_sorted.end())
        return found(it->second, "same words");
    }
    const std::vector<std::string> parts = Parts(title);
    if (parts.size() > 1)
    {
      if (const auto it = m_names.find(Joined(Words(parts[0]))); it != m_names.end())
        return found(it->second.names, "its first part");
      for (size_t i = 1; i < parts.size(); i++)
      {
        const std::string part = Joined(Words(parts[i]));
        const auto it = m_subtitle.find(part);
        if (part.size() >= 6 && it != m_subtitle.end() && it->second.size() <= 3)
          return found(it->second, "same subtitle");
      }
    }
    if (const auto it = m_first.find(key); it != m_first.end())
      return found(it->second, "their first part");
    if (const auto it = m_subtitle.find(key); it != m_subtitle.end() && it->second.size() <= 3)
      return found(it->second, "their subtitle");
    const std::vector<std::string> numbers = Numbers(words);
    // A spelling a letter or two apart ("Mega Man's Soccer"), with the same numbers.
    {
      const Entry* best = nullptr;
      double best_score = 0.88;
      for (const auto& [name_key, entry] : m_names)
      {
        const double longer = static_cast<double>(std::max(name_key.size(), key.size()));
        if (std::abs(static_cast<double>(name_key.size()) - static_cast<double>(key.size())) > longer * 0.25)
          continue;
        const double score = Similarity(key, name_key);
        if (score > best_score && Numbers(entry.words) == numbers)
        {
          best = &entry;
          best_score = score;
        }
      }
      if (best)
        return found(best->names, "close spelling");
    }
    // One name inside the other ("Scooby-Doo" in "Scooby-Doo Mystery"): the shorter one at least
    // 9 letters and 45% of the longer.
    {
      const Entry* best = nullptr;
      int hits = 0;
      for (const auto& [name_key, entry] : m_names)
      {
        const std::string& shorter = name_key.size() <= key.size() ? name_key : key;
        const std::string& longer = name_key.size() <= key.size() ? key : name_key;
        if (shorter.size() >= 9 && shorter.size() * 100 >= longer.size() * 45 &&
            longer.find(shorter) != std::string::npos && Numbers(entry.words) == numbers)
        {
          hits++;
          if (!best || name_key.size() > Joined(best->words).size())
            best = &entry;
        }
      }
      if (best && hits <= 2)
        return found(best->names, "inside");
    }
    // All our words in a longer name ("Wicked 18 Golf" in "True Golf Classics - Wicked 18"),
    // half its words at least.
    if (words.size() >= 2)
    {
      const Entry* best = nullptr;
      double best_share = 0.0;
      for (const auto& [name_key, entry] : m_names)
      {
        const bool all = std::all_of(words.begin(), words.end(), [&](const std::string& word) {
          return std::find(entry.words.begin(), entry.words.end(), word) != entry.words.end();
        });
        const double share = static_cast<double>(words.size()) / static_cast<double>(entry.words.size());
        if (all && share > best_share)
        {
          best = &entry;
          best_share = share;
        }
      }
      if (best && best_share >= 0.5)
        return found(best->names, "words");
    }
    return {};
  }

private:
  struct Entry
  {
    std::vector<std::string> words;
    std::vector<const std::string*> names;
  };
  std::map<std::string, Entry> m_names;  // the words joined -> the names
  std::map<std::string, std::vector<const std::string*>> m_sorted, m_first, m_subtitle;
};

// The best of the candidates for a game: its region first, then no prototypes or betas, then the
// original release rather than a re-release.
const std::string* Pick(const std::vector<const std::string*>& candidates, const Library::Game& game)
{
  // The NES list has TOSEC's names too: "Mega Man 2 (1989-06)(Capcom)(US)[b2]".
  static const std::map<std::string, const char*> TOSEC_REGIONS = {
      {"USA", "(US)"}, {"Europe", "(EU)"}, {"Japan", "(JP)"}};
  const std::string* best = nullptr;
  int best_score = -1000;
  for (const std::string* name : candidates)
  {
    int score = *name == game.id ? 20 : 0;  // the file's own name: the picture made for it
    if (!game.region.empty())
    {
      const std::string first = game.region.substr(0, game.region.find(','));
      const auto tosec = TOSEC_REGIONS.find(first);
      if (name->find("(" + first) != std::string::npos || name->find(", " + first) != std::string::npos)
        score += 10;
      else if (tosec != TOSEC_REGIONS.end() && name->find(tosec->second) != std::string::npos)
        score += 9;
    }
    if (game.discs.size() > 1 && name->find("(Disc 1)") != std::string::npos)
      score += 3;  // a game on several discs: the first one's box
    if (name->find("(USA") != std::string::npos)
      score += 2;  // a fallback when the region doesn't match
    else if (name->find("(US)") != std::string::npos)
      score += 1;
    for (const char* bad : {"(Beta", "(Proto", "(Sample", "(Demo", "(Pirate", "(Unl"})
    {
      if (name->find(bad) != std::string::npos)
        score -= 5;
    }
    for (const char* rerelease : {"Virtual Console", "Switch Online", "Classic Mini", "Evercade", "Edition)",
                                  "Aftermarket", "PlayChoice", "(VS"})
    {
      if (name->find(rerelease) != std::string::npos)
        score -= 3;
    }
    // The plain box first: a point less for each more group ("(Rev 1)", "(En)"), more for TOSEC's
    // dump marks ("[b]" a bad dump, "[h]" a hack, "[t]" a trainer).
    score -= std::max(0, static_cast<int>(std::count(name->begin(), name->end(), '(')) - 1);
    if (name->find('[') != std::string::npos)
      score -= 4;
    if (score > best_score)
    {
      best_score = score;
      best = name;
    }
  }
  return best;
}
// Covers are written one at a time (Download and Find Cover's thread), each to "<cover>.part" and
// then renamed: a cut write never leaves a broken cover that counts as there.
std::mutex g_cover_write;

bool SaveCover(const ImageRGBA& image, const std::string& path)
{
  const std::string part = path + ".part";
  if (!SavePng(part, image))
  {
    std::remove(part.c_str());
    return false;
  }
  if (std::rename(part.c_str(), path.c_str()) != 0)
  {
    std::remove(part.c_str());
    return false;
  }
  return true;
}

// ---- The closest picture, for a name Find() matches none of (Games Without Cover) ----
// Words that don't name a game, in the languages of the file names (Words() leaves out "the", "a",
// "an" and "and" already): "La Leyenda de Zelda" shares "leyenda" and "zelda" only.
bool Filler(const std::string& word)
{
  static const std::set<std::string> FILLER = {"of", "in", "on", "to", "no", "vs", "de", "del", "la", "las",
                                               "el", "los", "lo", "y", "e", "en", "un", "una", "da", "do",
                                               "du", "le", "les", "des", "di", "il"};
  return FILLER.count(word) > 0;
}

bool IsNumber(const std::string& word)
{
  return !word.empty() && std::all_of(word.begin(), word.end(),
                                      [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

std::vector<std::string> ContentWords(const std::vector<std::string>& words)
{
  std::vector<std::string> out;
  for (const std::string& word : words)
  {
    if (!Filler(word))
      out.push_back(word);
  }
  return out;
}

// A list's titles: each one's words (no filler) joined and apart, its numbers and its names (the
// regions and revisions), and in how many titles each word is.
struct Title
{
  std::string joined;
  std::vector<std::string> words, numbers;
  std::vector<const std::string*> names;
};
struct Titles
{
  std::vector<Title> titles;
  std::unordered_map<std::string, int> uses;
};

Titles ReadTitles(const std::vector<std::string>& names)
{
  Titles list;
  std::unordered_map<std::string, size_t> at;
  for (const std::string& name : names)
  {
    const std::vector<std::string> all = Words(TitlePart(name));
    std::vector<std::string> words = ContentWords(all);
    std::string joined = Joined(words);
    if (joined.empty())
      continue;
    const auto it = at.find(joined);
    if (it != at.end())
    {
      list.titles[it->second].names.push_back(&name);
      continue;
    }
    at.emplace(joined, list.titles.size());
    for (const std::string& word : std::set<std::string>(words.begin(), words.end()))
      list.uses[word]++;
    list.titles.push_back({std::move(joined), std::move(words), Numbers(all), {&name}});
  }
  return list;
}

// A word that names a game: 4 letters or more (a number: 3 digits or more, "2048"), in fewer than
// 2% of the list's titles ("super", "mario", "world" are in more).
bool Significant(const Titles& list, const std::string& word)
{
  if (word.size() < (IsNumber(word) ? 3u : 4u))
    return false;
  const auto it = list.uses.find(word);
  return it == list.uses.end() || static_cast<size_t>(it->second) * 50 < list.titles.size();
}

// The title most like a name, and its best picture for the game (nullptr: none close enough).
const std::string* Nearest(const Titles& list, const std::string& stem, const Library::Game& game, bool* sure)
{
  *sure = false;
  const std::vector<std::string> all = Words(TitlePart(stem));
  const std::vector<std::string> wanted = ContentWords(all);
  if (wanted.empty())
    return nullptr;
  const std::string joined = Joined(wanted);
  const std::vector<std::string> numbers = Numbers(all);
  const Title* best = nullptr;
  double best_score = 0.0;
  for (const Title& title : list.titles)
  {
    // Another game of the series: a number it has and the name lacks, or the name's sequel number
    // (one digit) it lacks. A longer number it lacks ("Olympic Summer Games 96") costs points.
    bool other_game = false;
    double number_penalty = 0.0;
    for (const std::string& number : title.numbers)
      other_game = other_game || std::find(numbers.begin(), numbers.end(), number) == numbers.end();
    for (const std::string& number : numbers)
    {
      if (std::find(title.numbers.begin(), title.numbers.end(), number) == title.numbers.end())
      {
        if (number.size() == 1)
          other_game = true;
        else
          number_penalty += 20.0;
      }
    }
    if (other_game)
      continue;
    double shared = 0.0;
    bool significant = false;
    for (const std::string& word : wanted)
    {
      if (std::find(title.words.begin(), title.words.end(), word) != title.words.end())
      {
        shared += 1.0;
        significant = significant || Significant(list, word);
      }
    }
    const double close = Similarity(joined, title.joined);
    const std::string& shorter = title.joined.size() <= joined.size() ? title.joined : joined;
    const std::string& longer = title.joined.size() <= joined.size() ? joined : title.joined;
    const bool inside = shorter.size() >= 6 && longer.find(shorter) != std::string::npos;
    // A word that names a game in common, or one name inside the other, or spelt alike.
    if (!significant && !(inside && shorter.size() >= 8) && close < 0.8)
      continue;
    const double coverage = shared / static_cast<double>(wanted.size());
    const double precision = shared / static_cast<double>(title.words.size());
    const double score = coverage * 60.0 + precision * 25.0 + close * 60.0 + (inside ? 15.0 : 0.0) - number_penalty;
    if (score > best_score)
    {
      best_score = score;
      best = &title;
      // Sure: the same numbers and nearly the same spelling. Shared words alone are often another
      // game ("Final Fight 3" -> "Final Fantasy III"): those are guesses, shown first by Find Cover.
      *sure = number_penalty == 0.0 && close >= 0.9;
    }
  }
  return best ? Pick(best->names, game) : nullptr;
}

// A cover put in the console's covers folder by hand: <file name>.png, <title>.png or, for an arcade game,
// its full name's or its parent's (libretro's thumbnails' names, which the Library lists the same way).
bool HasOwnCover(const std::string& covers_dir, const Library::Game& game)
{
  const std::string prefix = GetSystem(game.system).prefix;
  if (FileExists(covers_dir + "/" + game.key + ".png") || FileExists(covers_dir + "/" + prefix + game.title + ".png"))
    return true;
  for (const std::string& name : game.cover_names)
  {
    if (FileExists(covers_dir + "/" + prefix + name + ".png"))
      return true;
  }
  return false;
}
}  // namespace

// A list made ready for Closest(): Download()'s index and the titles.
struct Prepared
{
  explicit Prepared(const std::vector<std::string>& names) : index(names), titles(ReadTitles(names)) {}
  NameIndex index;
  Titles titles;
};

std::shared_ptr<const Prepared> Prepare(const std::vector<std::string>& names)
{
  return std::make_shared<const Prepared>(names);
}

const std::string* Closest(const Prepared& list, const std::string& name, const Library::Game& game, bool* sure)
{
  std::string how;
  if (const std::string* found = Pick(list.index.Find(name, &how), game))
  {
    *sure = true;
    return found;
  }
  return Nearest(list.titles, name, game, sure);
}

Result Download(const std::vector<Library::Game>& games, const std::string& covers_dir,
                const std::string& ca_file, const Progress& progress, const Saved& saved,
                const std::atomic<bool>& pause, const std::atomic<bool>& cancel, bool closest)
{
  Result result;
  MakeDirs(covers_dir);
  // The games without a picture (a cover put there by hand counts: <file name> or <title>.png, in
  // the console's covers folder).
  std::vector<const Library::Game*> wanted;
  for (const Library::Game& game : games)
  {
    if (HasOwnCover(covers_dir, game))
      result.already++;
    else
      wanted.push_back(&game);
  }
  const int total = static_cast<int>(wanted.size());
  if (total == 0)
  {
    if (progress)
      progress(0, 0, "");
    Log("every game has its picture (" + std::to_string(result.already) + ")");
    return result;
  }
  Net::Session session;
  // Each console's lists, read the first time one of its games needs them.
  std::vector<std::string> names[SYSTEM_COUNT][KIND_COUNT];
  std::unique_ptr<NameIndex> indexes[SYSTEM_COUNT][KIND_COUNT];
  std::unique_ptr<Titles> titles[SYSTEM_COUNT][KIND_COUNT];  // closest: made the first time needed
  bool listed[SYSTEM_COUNT][KIND_COUNT] = {};
  bool github = true;  // GitHub as the second choice, until its connection fails
  for (int i = 0; i < total; i++)
  {
    while (pause && !cancel)
      sceKernelUsleep(200000);
    if (cancel)
      break;
    const Library::Game& game = *wanted[i];
    const System& system = GetSystem(game.system);
    const int s = static_cast<int>(game.system);
    if (progress)
      progress(i, total, game.title);
    // The names the picture is looked for by: the game's own (its title when renamed, then its file's),
    // or an arcade game's romset full name and then its parent's (never the zip's name, "mslug").
    std::vector<std::string> stems = game.cover_names;
    if (stems.empty())
    {
      stems.push_back(game.custom_title ? game.title : game.id);
      if (game.custom_title)
        stems.push_back(game.id);
    }
    // The box, else the title screen.
    const std::string* name = nullptr;
    const Kind* kind = nullptr;
    std::string how;
    for (int k = 0; k < KIND_COUNT && !name; k++)
    {
      if (!listed[s][k])
      {
        listed[s][k] = true;
        std::string error;
        MakeDirs(covers_dir + "/" + system.prefix);  // the list is kept there
        names[s][k] = Names(KINDS[k], system, covers_dir, ca_file, &error, &session);
        indexes[s][k] = std::make_unique<NameIndex>(names[s][k]);
        if (names[s][k].empty())
        {
          Log(std::string(system.tab) + " " + KINDS[k].label + ": no list: " + error);
          if (k == 0 && result.error.empty())
            result.error = error;
        }
      }
      for (size_t i = 0; i < stems.size() && !name; i++)
        name = Pick(indexes[s][k]->Find(stems[i], &how), game);
      kind = &KINDS[k];
    }
    // Closest for All: the closest box art, else title screen, when it is sure of it (the doubtful
    // ones stay for Find Cover, which shows them first).
    std::string guesses;
    for (int k = 0; closest && !name && k < KIND_COUNT; k++)
    {
      if (names[s][k].empty())
        continue;
      if (!titles[s][k])
        titles[s][k] = std::make_unique<Titles>(ReadTitles(names[s][k]));
      for (const std::string& stem : stems)
      {
        bool sure = false;
        const std::string* guess = Nearest(*titles[s][k], stem, game, &sure);
        if (guess && sure)
        {
          name = guess;
          kind = &KINDS[k];
          how = "closest";
          break;
        }
        if (guess)
          guesses += (guesses.empty() ? "" : ", ") + *guess;
      }
    }
    if (!name && !guesses.empty())
      Log(game.key + ": maybe " + guesses + ": left for Find Cover");
    if (!name)
    {
      if (names[s][0].empty())
        result.failed++;  // the console's list couldn't be read
      else
        result.not_found++;
      Log(game.key + ": no picture found");
      continue;
    }
    if (kind != &KINDS[0] || how != "same name")
      Log(game.key + " -> " + *name + " (" + kind->label + ", " + how + ")");
    MakeDirs(covers_dir + "/" + system.prefix);
    const std::string target = covers_dir + "/" + game.key + ".png";
    if (HasOwnCover(covers_dir, game))
    {
      result.already++;  // chosen with Find Cover meanwhile: kept
      continue;
    }
    auto is_png = [](const std::string& data) { return data.size() > 8 && data.compare(1, 3, "PNG") == 0; };
    Net::Request request;
    request.url = FolderUrl(*kind, system) + Net::UrlEncode(*name) + ".png";
    std::string picture, error;
    long status = Net::GetText(request, &picture, 8 << 20, &error, &session);
    if ((status != 200 || !is_png(picture)) && github)
    {
      Log(game.key + ": thumbnails.libretro.com: " + (error.empty() ? "HTTP " + std::to_string(status) : error));
      request.ca_file = ca_file;
      request.url = std::string("https://raw.githubusercontent.com/") + system.thumbnails_repo + "/master/" +
                    kind->folder + "/" + Net::UrlEncode(*name) + ".png";
      error.clear();
      status = Net::GetText(request, &picture, 8 << 20, &error, &session);
      if (status == 0)
        github = false;  // no connection (the TLS handshake): not again for the next ones
    }
    if (status != 200 || !is_png(picture))
    {
      result.failed++;
      if (result.error.empty())
        result.error = error.empty() ? "HTTP " + std::to_string(status) : error;
      Log(game.key + ": " + (error.empty() ? "HTTP " + std::to_string(status) : error));
      continue;
    }
    // Written as it came, then made smaller (when it is bigger than the menu shows it).
    // Written under the covers' lock, unless one was chosen with Find Cover meanwhile (it wins).
    bool written = false;
    {
      std::lock_guard lock(g_cover_write);
      if (FileExists(target) || FileExists(covers_dir + "/" + system.prefix + game.title + ".png"))
      {
        result.already++;  // chosen with Find Cover while this one downloaded: kept
        continue;
      }
      const std::string temp = target + ".auto";  // Find Cover's DownloadPicture uses ".download"
      written = WriteTextFile(temp, picture);
      if (written)
      {
        ImageRGBA image;
        if (LoadPng(temp, &image, COVER_W, COVER_H) && SaveCover(image, target))
          std::remove(temp.c_str());
        else
          written = std::rename(temp.c_str(), target.c_str()) == 0;
      }
    }
    if (written)
    {
      result.downloaded++;
      if (saved)
        saved(game.key, target);
    }
    else
    {
      result.failed++;
    }
  }
  if (progress)
    progress(total, total, "");
  Log(std::to_string(result.downloaded) + " downloaded, " + std::to_string(result.already) +
      " already there, " + std::to_string(result.not_found) + " not found, " +
      std::to_string(result.failed) + " failed");
  return result;
}

std::vector<std::string> PictureNames(SystemId system, bool title_screens, const std::string& covers_dir,
                                      const std::string& ca_file, std::string* error)
{
  Net::Session session;
  MakeDirs(covers_dir + "/" + GetSystem(system).prefix);
  return Names(KINDS[title_screens ? 1 : 0], GetSystem(system), covers_dir, ca_file, error, &session);
}

std::vector<std::string> Search(const std::vector<std::string>& names, const std::string& text, size_t max)
{
  const std::vector<std::string> wanted = Words(TitlePart(text));
  if (wanted.empty())
    return {};
  const std::string wanted_joined = Joined(wanted);
  struct Hit
  {
    double score;
    const std::string* name;
  };
  std::vector<Hit> hits;
  for (const std::string& name : names)
  {
    const std::vector<std::string> words = Words(TitlePart(name));
    if (words.empty())
      continue;
    // The words typed that the name has (a word begun counts half: "zeld" in "zelda").
    double found = 0.0;
    for (const std::string& word : wanted)
    {
      if (std::find(words.begin(), words.end(), word) != words.end())
        found += 1.0;
      else if (word.size() >= 3 && std::any_of(words.begin(), words.end(), [&](const std::string& w) {
                 return w.compare(0, word.size(), word) == 0;
               }))
        found += 0.5;
    }
    const std::string joined = Joined(words);
    const double close = Similarity(wanted_joined, joined);
    if (found == 0.0 && close < 0.6)
      continue;
    double score = found / static_cast<double>(wanted.size()) * 100.0 + close * 40.0;
    score -= std::abs(static_cast<double>(words.size()) - found) * 2.0;  // the plain name first
    if (name.find("(USA") != std::string::npos)
      score += 2.0;
    if (name.find('[') != std::string::npos || name.find("(Beta") != std::string::npos ||
        name.find("(Proto") != std::string::npos)
      score -= 6.0;
    hits.push_back({score, &name});
  }
  std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
  std::vector<std::string> best;
  for (const Hit& hit : hits)
  {
    if (best.size() >= max)
      break;
    best.push_back(*hit.name);
  }
  return best;
}

bool DownloadPicture(SystemId system, bool title_screen, const std::string& name, const std::string& path,
                     uint32_t max_w, uint32_t max_h, const std::string& ca_file, std::string* error)
{
  const System& console = GetSystem(system);
  const Kind& kind = KINDS[title_screen ? 1 : 0];
  auto is_png = [](const std::string& data) { return data.size() > 8 && data.compare(1, 3, "PNG") == 0; };
  Net::Session session;
  Net::Request request;
  request.url = FolderUrl(kind, console) + Net::UrlEncode(name) + ".png";
  std::string picture;
  error->clear();
  long status = Net::GetText(request, &picture, 8 << 20, error, &session);
  if (status != 200 || !is_png(picture))
  {
    request.ca_file = ca_file;
    request.url = std::string("https://raw.githubusercontent.com/") + console.thumbnails_repo + "/master/" +
                  kind.folder + "/" + Net::UrlEncode(name) + ".png";
    error->clear();
    status = Net::GetText(request, &picture, 8 << 20, error, &session);
  }
  if (status != 200 || !is_png(picture))
  {
    if (error->empty())
      *error = "HTTP " + std::to_string(status);
    Log(name + ": " + *error);
    return false;
  }
  std::lock_guard lock(g_cover_write);  // one cover writer at a time (Download's thread too)
  const std::string temp = path + ".download";
  if (!WriteTextFile(temp, picture))
  {
    *error = "the picture couldn't be written";
    return false;
  }
  ImageRGBA image;
  if (LoadPng(temp, &image, max_w, max_h) && SaveCover(image, path))
  {
    std::remove(temp.c_str());
    return true;
  }
  std::remove(path.c_str());
  if (std::rename(temp.c_str(), path.c_str()) == 0)
    return true;
  *error = "the picture couldn't be written";
  return false;
}
}  // namespace CoverDownload
