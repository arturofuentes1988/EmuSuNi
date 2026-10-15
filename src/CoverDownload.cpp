// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "CoverDownload.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <sstream>

#include <orbis/libkernel.h>

#include "Image.h"
#include "Log.h"
#include "Net.h"
#include "Saves.h"

namespace CoverDownload
{
namespace
{
constexpr const char* REPO = "libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System";
// libretro's thumbnail server, the one RetroArch uses: the same pictures as the GitHub project, over
// plain HTTP. The PS4's TLS fails on raw.githubusercontent.com ("RSA - The PKCS#1 verification
// failed" in every handshake), so GitHub is only the second choice.
constexpr const char* SERVER = "http://thumbnails.libretro.com/";
constexpr const char* SYSTEM = "Nintendo - Super Nintendo Entertainment System";
// The size the menu shows a cover at (the Library's selected one): bigger pictures are made
// smaller once, when they are downloaded, instead of every time the menu reads them.
constexpr uint32_t COVER_W = 600, COVER_H = 430;

// The pictures tried, in order: the box, then the game's title screen (better than no picture).
struct Kind
{
  const char* folder;  // libretro's folder
  const char* cache;   // its list of names, kept a week in covers/
  const char* label;   // for the log
};
constexpr Kind KINDS[] = {{"Named_Boxarts", ".libretro-thumbnails.txt", "box"},
                          {"Named_Titles", ".libretro-titles.txt", "title screen"}};
constexpr int KIND_COUNT = 2;

std::string FolderUrl(const Kind& kind)
{
  return std::string(SERVER) + Net::UrlEncode(SYSTEM) + "/" + kind.folder + "/";
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

// The names of a kind of pictures (without ".png"): from the list kept in covers/ (a week), else
// from the server's folder listing, else from GitHub.
std::vector<std::string> Names(const Kind& kind, const std::string& covers_dir, const std::string& ca_file,
                               std::string* error, Net::Session* session)
{
  const std::string cache = covers_dir + "/" + kind.cache;
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
    Log(std::to_string(names.size()) + " " + kind.label + " pictures listed by " + where);
  };
  {
    Net::Request request;
    request.url = FolderUrl(kind);
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
    request.url = std::string("https://api.github.com/repos/") + REPO + "/git/trees/" + branch;
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
  request.url = std::string("https://api.github.com/repos/") + REPO + "/git/trees/" + sha;
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
  const std::string* best = nullptr;
  int best_score = -1000;
  for (const std::string* name : candidates)
  {
    int score = 0;
    if (!game.region.empty())
    {
      const std::string first = game.region.substr(0, game.region.find(','));
      if (name->find("(" + first) != std::string::npos || name->find(", " + first) != std::string::npos)
        score += 10;
    }
    if (name->find("(USA") != std::string::npos)
      score += 2;  // a fallback when the region doesn't match
    for (const char* bad : {"(Beta", "(Proto", "(Sample", "(Demo", "(Pirate", "(Unl"})
    {
      if (name->find(bad) != std::string::npos)
        score -= 5;
    }
    for (const char* rerelease : {"Virtual Console", "Switch Online", "Classic Mini", "Evercade"})
    {
      if (name->find(rerelease) != std::string::npos)
        score -= 3;
    }
    if (score > best_score)
    {
      best_score = score;
      best = name;
    }
  }
  return best;
}
}  // namespace

Result Download(const std::vector<Library::Game>& games, const std::string& covers_dir,
                const std::string& ca_file, const Progress& progress, const Saved& saved,
                const std::atomic<bool>& pause, const std::atomic<bool>& cancel)
{
  Result result;
  MakeDirs(covers_dir);
  // The games without a picture (a cover put there by hand counts: <file name> or <title>.png).
  std::vector<const Library::Game*> wanted;
  for (const Library::Game& game : games)
  {
    if (FileExists(covers_dir + "/" + game.id + ".png") || FileExists(covers_dir + "/" + game.title + ".png"))
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
  std::vector<std::string> names[KIND_COUNT];
  std::unique_ptr<NameIndex> indexes[KIND_COUNT];
  names[0] = Names(KINDS[0], covers_dir, ca_file, &result.error, &session);
  if (names[0].empty())
  {
    result.failed = total;
    Log("no list: " + result.error);
    return result;
  }
  indexes[0] = std::make_unique<NameIndex>(names[0]);
  bool github = true;  // GitHub as the second choice, until its connection fails
  for (int i = 0; i < total; i++)
  {
    while (pause && !cancel)
      sceKernelUsleep(200000);
    if (cancel)
      break;
    const Library::Game& game = *wanted[i];
    if (progress)
      progress(i, total, game.title);
    // The box, else the title screen (its list read the first time a game needs one).
    const std::string* name = nullptr;
    const Kind* kind = nullptr;
    std::string how;
    for (int k = 0; k < KIND_COUNT && !name; k++)
    {
      if (!indexes[k])
      {
        std::string error;
        names[k] = Names(KINDS[k], covers_dir, ca_file, &error, &session);
        indexes[k] = std::make_unique<NameIndex>(names[k]);
      }
      name = Pick(indexes[k]->Find(game.id, &how), game);
      kind = &KINDS[k];
    }
    if (!name)
    {
      result.not_found++;
      Log(game.id + ": no picture found");
      continue;
    }
    if (kind != &KINDS[0] || how != "same name")
      Log(game.id + " -> " + *name + " (" + kind->label + ", " + how + ")");
    const std::string target = covers_dir + "/" + game.id + ".png";
    auto is_png = [](const std::string& data) { return data.size() > 8 && data.compare(1, 3, "PNG") == 0; };
    Net::Request request;
    request.url = FolderUrl(*kind) + Net::UrlEncode(*name) + ".png";
    std::string picture, error;
    long status = Net::GetText(request, &picture, 8 << 20, &error, &session);
    if ((status != 200 || !is_png(picture)) && github)
    {
      Log(game.id + ": thumbnails.libretro.com: " + (error.empty() ? "HTTP " + std::to_string(status) : error));
      request.ca_file = ca_file;
      request.url = std::string("https://raw.githubusercontent.com/") + REPO + "/master/" + kind->folder + "/" +
                    Net::UrlEncode(*name) + ".png";
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
      Log(game.id + ": " + (error.empty() ? "HTTP " + std::to_string(status) : error));
      continue;
    }
    // Written as it came, then made smaller (when it is bigger than the menu shows it).
    const std::string temp = target + ".download";
    bool written = WriteTextFile(temp, picture);
    if (written)
    {
      ImageRGBA image;
      if (LoadPng(temp, &image, COVER_W, COVER_H) && SavePng(target, image))
        std::remove(temp.c_str());
      else
        written = std::rename(temp.c_str(), target.c_str()) == 0;
    }
    if (written)
    {
      result.downloaded++;
      if (saved)
        saved(game.id, target);
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
}  // namespace CoverDownload
