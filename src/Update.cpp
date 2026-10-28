// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Update.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <thread>

#include <mbedtls/sha256.h>

#include "Library.h"
#include "Log.h"
#include "Net.h"
#include "UpdateInstall.h"

extern "C" int emusuni_disk_space_known(const char* path, int64_t* out);  // port/ps4_diskfree.c

namespace Update
{
namespace
{
// GitHub's answers: the newest full release, or the newest releases (test versions too).
constexpr const char* LATEST_URL = "https://api.github.com/repos/arturofuentes1988/EmuSuNi/releases/latest";
constexpr const char* LIST_URL = "https://api.github.com/repos/arturofuentes1988/EmuSuNi/releases?per_page=15";
constexpr int64_t DAY = 24 * 3600;
constexpr int64_t MIB = 1024 * 1024;
constexpr const char* TITLE_ID = "EMSN00001";  // (the packages of scripts/package.sh)

void Log(const std::string& text)
{
  LogLine("update: " + text);
}

std::string Lower(std::string text)
{
  for (char& c : text)
  {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

bool EndsWith(const std::string& text, const std::string& end)
{
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

std::string Trim(const std::string& text)
{
  size_t start = 0, end = text.size();
  while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
    start++;
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    end--;
  return text.substr(start, end - start);
}

// ---- A small JSON reader (GitHub's answers): never throws, any input ----
struct Json
{
  enum class Type
  {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
  };
  Type type = Type::Null;
  bool boolean = false;
  std::string text;  // a string's, a number's digits
  std::vector<Json> items;  // an array's; an object's values (keys: their names)
  std::vector<std::string> keys;

  const Json* Field(const char* name) const
  {
    if (type != Type::Object)
      return nullptr;
    for (size_t i = 0; i < keys.size() && i < items.size(); i++)
    {
      if (keys[i] == name)
        return &items[i];
    }
    return nullptr;
  }
  std::string String(const char* name) const
  {
    const Json* field = Field(name);
    return field && field->type == Type::String ? field->text : std::string();
  }
  bool Bool(const char* name) const
  {
    const Json* field = Field(name);
    return field && field->type == Type::Bool && field->boolean;
  }
  int64_t Integer(const char* name, int64_t fallback) const
  {
    const Json* field = Field(name);
    if (!field || field->type != Type::Number)
      return fallback;
    char* stop = nullptr;
    const long long value = std::strtoll(field->text.c_str(), &stop, 10);
    return stop && stop != field->text.c_str() ? static_cast<int64_t>(value) : fallback;
  }
};

class JsonReader
{
public:
  explicit JsonReader(const std::string& text) : m_p(text.c_str()), m_end(text.c_str() + text.size()) {}
  bool Read(Json* out)
  {
    Space();
    if (!Value(out, 0))
      return false;
    Space();
    return m_p == m_end;
  }

private:
  const char* m_p;
  const char* m_end;

  void Space()
  {
    while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
      m_p++;
  }
  bool Word(const char* word)
  {
    const size_t length = std::strlen(word);
    if (static_cast<size_t>(m_end - m_p) < length || std::memcmp(m_p, word, length) != 0)
      return false;
    m_p += length;
    return true;
  }
  static void Utf8(uint32_t code, std::string* out)
  {
    if (code < 0x80)
    {
      *out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
      *out += static_cast<char>(0xC0 | (code >> 6));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
      *out += static_cast<char>(0xE0 | (code >> 12));
      *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
      *out += static_cast<char>(0xF0 | (code >> 18));
      *out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }
  bool Hex4(uint32_t* code)
  {
    if (m_end - m_p < 4)
      return false;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
    {
      const char c = m_p[i];
      value <<= 4;
      if (c >= '0' && c <= '9')
        value |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        value |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        value |= static_cast<uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    m_p += 4;
    *code = value;
    return true;
  }
  bool StringValue(std::string* out)
  {
    if (m_p >= m_end || *m_p != '"')
      return false;
    m_p++;
    while (m_p < m_end)
    {
      const char c = *m_p++;
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20)
        return false;
      if (c != '\\')
      {
        *out += c;
        continue;
      }
      if (m_p >= m_end)
        return false;
      const char e = *m_p++;
      switch (e)
      {
      case '"': *out += '"'; break;
      case '\\': *out += '\\'; break;
      case '/': *out += '/'; break;
      case 'b': *out += '\b'; break;
      case 'f': *out += '\f'; break;
      case 'n': *out += '\n'; break;
      case 'r': *out += '\r'; break;
      case 't': *out += '\t'; break;
      case 'u':
      {
        uint32_t code = 0;
        if (!Hex4(&code))
          return false;
        if (code >= 0xD800 && code <= 0xDBFF)
        {
          uint32_t low = 0;
          if (m_end - m_p >= 6 && m_p[0] == '\\' && m_p[1] == 'u')
          {
            m_p += 2;
            if (!Hex4(&low))
              return false;
            code = low >= 0xDC00 && low <= 0xDFFF ? 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
          }
          else
          {
            code = 0xFFFD;
          }
        }
        else if (code >= 0xDC00 && code <= 0xDFFF)
        {
          code = 0xFFFD;
        }
        Utf8(code, out);
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }
  bool Number(Json* out)
  {
    const char* start = m_p;
    if (m_p < m_end && *m_p == '-')
      m_p++;
    if (m_p >= m_end || *m_p < '0' || *m_p > '9')
      return false;
    while (m_p < m_end && ((*m_p >= '0' && *m_p <= '9') || *m_p == '.' || *m_p == 'e' || *m_p == 'E' ||
                           *m_p == '+' || *m_p == '-'))
      m_p++;
    out->type = Json::Type::Number;
    out->text.assign(start, m_p);
    return true;
  }
  bool Value(Json* out, int depth)
  {
    if (depth > 48 || m_p >= m_end)
      return false;
    switch (*m_p)
    {
    case '{':
    {
      m_p++;
      out->type = Json::Type::Object;
      Space();
      if (m_p < m_end && *m_p == '}')
      {
        m_p++;
        return true;
      }
      while (true)
      {
        Space();
        std::string key;
        if (!StringValue(&key))
          return false;
        Space();
        if (m_p >= m_end || *m_p != ':')
          return false;
        m_p++;
        Space();
        Json value;
        if (!Value(&value, depth + 1))
          return false;
        out->keys.push_back(std::move(key));
        out->items.push_back(std::move(value));
        Space();
        if (m_p < m_end && *m_p == ',')
        {
          m_p++;
          continue;
        }
        if (m_p < m_end && *m_p == '}')
        {
          m_p++;
          return true;
        }
        return false;
      }
    }
    case '[':
    {
      m_p++;
      out->type = Json::Type::Array;
      Space();
      if (m_p < m_end && *m_p == ']')
      {
        m_p++;
        return true;
      }
      while (true)
      {
        Space();
        Json value;
        if (!Value(&value, depth + 1))
          return false;
        out->items.push_back(std::move(value));
        Space();
        if (m_p < m_end && *m_p == ',')
        {
          m_p++;
          continue;
        }
        if (m_p < m_end && *m_p == ']')
        {
          m_p++;
          return true;
        }
        return false;
      }
    }
    case '"':
      out->type = Json::Type::String;
      return StringValue(&out->text);
    case 't':
      out->type = Json::Type::Bool;
      out->boolean = true;
      return Word("true");
    case 'f':
      out->type = Json::Type::Bool;
      return Word("false");
    case 'n':
      return Word("null");
    default:
      return Number(out);
    }
  }
};

bool IsHex64(const std::string& text)
{
  if (text.size() != 64)
    return false;
  for (const char c : text)
  {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

std::string VersionText(int number)
{
  char text[16];
  snprintf(text, sizeof(text), "%02d.%02d", number / 100, number % 100);
  return text;
}

// One release of GitHub's answer: false when it isn't one the app can take (*why).
bool ReadRelease(const Json& item, bool tests, Release* out, std::string* why)
{
  if (item.type != Json::Type::Object)
  {
    *why = "not a release";
    return false;
  }
  Release release;
  release.tag = item.String("tag_name");
  if (item.Bool("draft"))
  {
    *why = release.tag + ": a draft";
    return false;
  }
  release.prerelease = item.Bool("prerelease");
  if (release.prerelease && !tests)
  {
    *why = release.tag + ": a test version";
    return false;
  }
  const int number = VersionNumber(release.tag);
  if (number < 0)
  {
    *why = "\"" + release.tag + "\": not a version (vNN.NN)";
    return false;
  }
  release.version = VersionText(number);
  release.title = item.String("name");
  release.notes = item.String("body");
  release.page = item.String("html_url");
  release.important = IsImportant(release.notes);
  const Json* assets = item.Field("assets");
  const Json* pkg = nullptr;
  if (assets && assets->type == Json::Type::Array)
  {
    for (const Json& asset : assets->items)
    {
      const std::string name = asset.String("name");
      const std::string state = asset.String("state");
      if (!EndsWith(Lower(name), ".pkg") || (!state.empty() && state != "uploaded") ||
          asset.String("browser_download_url").empty())
        continue;
      // Several: EmuSuNi's own first.
      if (!pkg || (Lower(pkg->String("name")).find("emusuni") == std::string::npos &&
                   Lower(name).find("emusuni") != std::string::npos))
        pkg = &asset;
    }
  }
  if (!pkg)
  {
    *why = release.tag + ": no .pkg";
    return false;
  }
  release.pkg_name = pkg->String("name");
  release.pkg_url = pkg->String("browser_download_url");
  release.pkg_size = pkg->Integer("size", -1);
  const std::string digest = pkg->String("digest");
  if (Lower(digest).compare(0, 7, "sha256:") == 0 && IsHex64(digest.substr(7)))
    release.sha256 = Lower(digest.substr(7));
  // A .sha256 file for it: "<pkg>.sha256" first, else any.
  for (const Json& asset : assets->items)
  {
    const std::string name = Lower(asset.String("name"));
    const std::string url = asset.String("browser_download_url");
    if (url.empty() || !EndsWith(name, ".sha256"))
      continue;
    if (name == Lower(release.pkg_name) + ".sha256" || release.sha256_url.empty())
      release.sha256_url = url;
  }
  *out = std::move(release);
  return true;
}

// Emoji and the like (the menu's fonts have none of them): 4-byte UTF-8 characters, U+2600-27BF (symbols and
// dingbats), U+2B00-2BFF, U+FE0F and U+200D, left out.
std::string WithoutEmoji(const std::string& text)
{
  std::string out;
  for (size_t i = 0; i < text.size();)
  {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + length > text.size())
      length = text.size() - i;
    bool keep = length != 4;
    if (length == 3)
    {
      const uint32_t code = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                            (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      keep = !((code >= 0x2600 && code <= 0x27BF) || (code >= 0x2B00 && code <= 0x2BFF) || code == 0xFE0F ||
               code == 0x200D || code == 0x20E3);
    }
    if (keep)
      out.append(text, i, length);
    i += length;
  }
  return out;
}

// Markdown's marks off a line: links' text kept, **, __, ` and the important-update marker taken off.
std::string PlainLine(std::string line)
{
  // [text](address) -> text
  for (size_t open = line.find('['); open != std::string::npos; open = line.find('[', open + 1))
  {
    const size_t close = line.find("](", open);
    const size_t end = close == std::string::npos ? std::string::npos : line.find(')', close);
    if (close == std::string::npos || end == std::string::npos)
      break;
    line = line.substr(0, open) + line.substr(open + 1, close - open - 1) + line.substr(end + 1);
  }
  for (const char* mark : {"**", "__", "`"})
  {
    for (size_t at; (at = line.find(mark)) != std::string::npos;)
      line.erase(at, std::strlen(mark));
  }
  for (const char* marker : {"[obligatoria]", "[mandatory]"})
  {
    for (size_t at; (at = Lower(line).find(marker)) != std::string::npos;)
      line.erase(at, std::strlen(marker));
  }
  line = WithoutEmoji(line);
  // Spaces: one between words.
  std::string out;
  for (const char c : line)
  {
    if (c == ' ' && (out.empty() || out.back() == ' '))
      continue;
    out += c;
  }
  return Trim(out);
}

int64_t FileSize(const std::string& path)
{
  struct stat info = {};
  return stat(path.c_str(), &info) == 0 ? static_cast<int64_t>(info.st_size) : -1;
}

std::string Escape(const std::string& text)
{
  std::string out;
  for (const char c : text)
  {
    if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c != '\r')
      out += c;
  }
  return out;
}

std::string Unescape(const std::string& text)
{
  std::string out;
  for (size_t i = 0; i < text.size(); i++)
  {
    if (text[i] == '\\' && i + 1 < text.size())
    {
      out += text[i + 1] == 'n' ? '\n' : text[i + 1];
      i++;
    }
    else
    {
      out += text[i];
    }
  }
  return out;
}
}  // namespace

// ---- The rules ----

int VersionNumber(const std::string& text)
{
  size_t i = 0;
  if (i < text.size() && (text[i] == 'v' || text[i] == 'V'))
    i++;
  int major = 0, digits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 3)
  {
    major = major * 10 + (text[i] - '0');
    i++;
    digits++;
  }
  if (digits == 0 || i >= text.size() || text[i] != '.')
    return -1;
  i++;
  if (i + 2 > text.size() || text[i] < '0' || text[i] > '9' || text[i + 1] < '0' || text[i + 1] > '9')
    return -1;
  const int minor = (text[i] - '0') * 10 + (text[i + 1] - '0');
  i += 2;
  // After it: nothing, or a suffix ("-test", "+1", " beta"), not more digits or another ".NN".
  if (i < text.size() && !(text[i] == '-' || text[i] == '+' || text[i] == '_' || text[i] == ' '))
    return -1;
  return major * 100 + minor;
}

bool IsNewer(const std::string& a, const std::string& b)
{
  const int x = VersionNumber(a), y = VersionNumber(b);
  return x >= 0 && y >= 0 && x > y;
}

bool ParseReleases(const std::string& json, bool tests, Release* out, std::string* error)
{
  Json root;
  if (!JsonReader(json).Read(&root))
  {
    *error = "GitHub's answer isn't JSON";
    return false;
  }
  std::vector<const Json*> items;
  if (root.type == Json::Type::Array)
  {
    for (const Json& item : root.items)
      items.push_back(&item);
  }
  else if (root.type == Json::Type::Object)
  {
    items.push_back(&root);
  }
  bool found = false;
  int best = -1;
  std::string why;
  for (const Json* item : items)
  {
    Release release;
    std::string reason;
    if (!ReadRelease(*item, tests, &release, &reason))
    {
      why += (why.empty() ? "" : "; ") + reason;
      continue;
    }
    const int number = VersionNumber(release.version);
    // The highest version; a full release before a test version of the same number.
    if (!found || number > best || (number == best && out->prerelease && !release.prerelease))
    {
      *out = std::move(release);
      best = number;
      found = true;
    }
  }
  if (!found)
    *error = items.empty() ? std::string("no release in GitHub's answer") : "no version with a .pkg (" + why + ")";
  return found;
}

bool IsImportant(const std::string& notes)
{
  const std::string lower = Lower(notes);
  return lower.find("[obligatoria]") != std::string::npos || lower.find("[mandatory]") != std::string::npos;
}

std::vector<std::string> NoteLines(const std::string& notes, size_t count)
{
  std::vector<std::string> lines;
  size_t start = 0;
  while (lines.size() < count && start <= notes.size())
  {
    const size_t end = std::min(notes.find('\n', start), notes.size());
    std::string line = Trim(notes.substr(start, end - start));
    start = end + 1;
    if (line.empty() || line[0] == '|' || line[0] == '<' || line.compare(0, 2, "![") == 0 ||
        line.find_first_not_of("-*_= ") == std::string::npos)
      continue;  // a table, HTML, a picture, a rule
    bool heading = false;
    while (!line.empty() && line[0] == '#')
    {
      line.erase(0, 1);
      heading = true;
    }
    while (!line.empty() && line[0] == '>')
      line.erase(0, 1);
    line = Trim(line);
    if (line.size() > 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ')
      line = "- " + Trim(line.substr(2));
    line = PlainLine(line);
    if (line.empty() || line == "-")
      continue;
    // A heading that only says the version ("EmuSuNi v00.74"): the notice says it already.
    if (heading)
    {
      std::string rest = Lower(line);
      for (const char* word : {"emusuni", "version", "versión"})
      {
        for (size_t at; (at = rest.find(word)) != std::string::npos;)
          rest.erase(at, std::strlen(word));
      }
      if (rest.find_first_not_of("v0123456789. -") == std::string::npos)
        continue;
    }
    lines.push_back(line);
  }
  return lines;
}

std::string HashFromText(const std::string& text)
{
  for (size_t i = 0; i + 64 <= text.size(); i++)
  {
    if (i > 0 && std::isxdigit(static_cast<unsigned char>(text[i - 1])))
      continue;
    const std::string run = text.substr(i, 64);
    if (IsHex64(run) && (i + 64 == text.size() || !std::isxdigit(static_cast<unsigned char>(text[i + 64]))))
      return Lower(run);
  }
  return {};
}

std::string FileSha256(const std::string& path, void (*progress)(int64_t done, void* user), void* user)
{
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return {};
  constexpr size_t CHUNK = 256 * 1024;
  std::unique_ptr<unsigned char[]> buffer(new (std::nothrow) unsigned char[CHUNK]);
  if (!buffer)
  {
    std::fclose(file);
    return {};
  }
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool good = mbedtls_sha256_starts_ret(&context, 0) == 0;
  int64_t done = 0;
  for (size_t got; good && (got = std::fread(buffer.get(), 1, CHUNK, file)) > 0;)
  {
    good = mbedtls_sha256_update_ret(&context, buffer.get(), got) == 0;
    done += static_cast<int64_t>(got);
    if (progress)
      progress(done, user);
  }
  good = good && !std::ferror(file);
  std::fclose(file);
  unsigned char digest[32];
  good = good && mbedtls_sha256_finish_ret(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  if (!good)
    return {};
  char hex[65];
  for (int i = 0; i < 32; i++)
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  return hex;
}

std::string SaveMemory(const Memory& memory)
{
  const Release& r = memory.found;
  std::string text = "# EmuSuNi's updates (src/Update.h): the last check of GitHub and what it found\n";
  auto put = [&text](const char* key, const std::string& value) { text += std::string(key) + "=" + Escape(value) + "\n"; };
  put("checked", std::to_string(memory.checked));
  put("etag", memory.etag);
  put("etag_url", memory.etag_url);
  put("tests", memory.tests ? "1" : "0");
  put("tag", r.tag);
  put("version", r.version);
  put("title", r.title);
  put("page", r.page);
  put("prerelease", r.prerelease ? "1" : "0");
  put("important", r.important ? "1" : "0");
  put("pkg_name", r.pkg_name);
  put("pkg_url", r.pkg_url);
  put("pkg_size", std::to_string(r.pkg_size));
  put("sha256", r.sha256);
  put("sha256_url", r.sha256_url);
  put("pending", memory.pending);
  put("pending_version", memory.pending_version);
  put("pending_time", std::to_string(memory.pending_time));
  put("notes", r.notes);  // (last: the longest)
  return text;
}

Memory LoadMemory(const std::string& text)
{
  Memory memory;
  Release& r = memory.found;
  auto number = [](const std::string& value, int64_t fallback) {
    char* stop = nullptr;
    const long long n = std::strtoll(value.c_str(), &stop, 10);
    return stop && stop != value.c_str() ? static_cast<int64_t>(n) : fallback;
  };
  size_t start = 0;
  while (start < text.size())
  {
    const size_t end = std::min(text.find('\n', start), text.size());
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq), value = Unescape(line.substr(eq + 1));
    if (key == "checked") memory.checked = number(value, 0);
    else if (key == "etag") memory.etag = value;
    else if (key == "etag_url") memory.etag_url = value;
    else if (key == "tests") memory.tests = value == "1";
    else if (key == "tag") r.tag = value;
    else if (key == "version") r.version = value;
    else if (key == "title") r.title = value;
    else if (key == "page") r.page = value;
    else if (key == "prerelease") r.prerelease = value == "1";
    else if (key == "important") r.important = value == "1";
    else if (key == "pkg_name") r.pkg_name = value;
    else if (key == "pkg_url") r.pkg_url = value;
    else if (key == "pkg_size") r.pkg_size = number(value, -1);
    else if (key == "sha256") r.sha256 = IsHex64(value) ? Lower(value) : std::string();
    else if (key == "sha256_url") r.sha256_url = value;
    else if (key == "pending") memory.pending = value;
    else if (key == "pending_version") memory.pending_version = value;
    else if (key == "pending_time") memory.pending_time = number(value, 0);
    else if (key == "notes") r.notes = value;
  }
  if (VersionNumber(r.version) < 0)
    r = Release{};  // (a damaged file: as if never checked)
  return memory;
}

bool CheckDue(const Memory& memory, int64_t now)
{
  return memory.checked <= 0 || now - memory.checked >= DAY || now < memory.checked - DAY;
}

bool HasNewer(const Memory& memory, const std::string& running)
{
  if (memory.found.tag.empty() || (memory.found.prerelease && !memory.tests))
    return false;
  return IsNewer(memory.found.version, running);
}

bool NoticeDue(const Memory& memory, const std::string& running, bool closed, bool check_due, bool check_answered)
{
  return !closed && (!check_due || check_answered) && HasNewer(memory, running);
}

int64_t SpaceNeeded(int64_t size, bool internal)
{
  const int64_t pkg = size > 0 ? size : 400 * MIB;
  return internal ? pkg * 2 + 64 * MIB : pkg + 16 * MIB;
}

// ---- The app's side ----

namespace
{
std::mutex s_mutex;
Status s_status;
Memory s_memory;
std::string s_data_dir;
std::string s_url;  // update-url.txt's address, else ""
pthread_t s_thread{};
bool s_thread_made = false;      // s_thread is to be joined
bool s_busy = false;             // the thread's job runs
std::atomic<bool> s_cancel{false};
bool s_started = false;
bool s_notice_shown = false;     // this session
bool s_due_at_start = false;     // a check was due at the start
bool s_start_answered = false;   // and it answered
std::chrono::steady_clock::time_point s_last_progress;

std::string MemoryPath()
{
  return s_data_dir + "/update.ini";
}

// (Under s_mutex.)
void Changed()
{
  s_status.serial++;
}

void Publish()
{
  s_status.newest = s_memory.found;
  s_status.newer = HasNewer(s_memory, s_status.running);
  s_status.important = s_status.newer && s_memory.found.important;
  s_status.checked = s_memory.checked > 0;
  s_status.checked_at = s_memory.checked;
  s_status.tests = s_memory.tests;
  Changed();
}

void SaveLocked()
{
  if (!WriteTextFile(MemoryPath(), SaveMemory(s_memory)))
    Log("couldn't write " + MemoryPath());
}

void SetPhase(Phase phase, const std::string& message, const std::string& detail = {})
{
  std::lock_guard lock(s_mutex);
  s_status.phase = phase;
  s_status.message = message;
  s_status.detail = detail;
  Changed();
}

std::string Mb(int64_t bytes)
{
  char text[32];
  snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / MIB);
  return text;
}

// The address of GitHub's answer.
std::string ApiUrl(bool tests)
{
  if (!s_url.empty())
    return s_url;
  return tests ? LIST_URL : LATEST_URL;
}

// The check (on the thread): GitHub's newest version, remembered.
void CheckJob(bool asked)
{
  Memory memory;
  std::string running;
  {
    std::lock_guard lock(s_mutex);
    memory = s_memory;
    running = s_status.running;
  }
  const std::string url = ApiUrl(memory.tests);
  Log(std::string("checking for a version newer than ") + running + " (" + (asked ? "asked" : "the day's check") +
      ", test versions " + (memory.tests ? "included" : "left out") + "): " + url);
  Net::Request request;
  request.url = url;
  request.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"};
  const bool conditional = memory.etag_url == url && !memory.etag.empty() && !memory.found.tag.empty();
  if (conditional)
    request.headers.push_back("If-None-Match: " + memory.etag);
  request.timeout_seconds = 25;
  request.cancel = &s_cancel;
  request.max_bytes = 4 << 20;
  const auto start = std::chrono::steady_clock::now();
  const Net::Response answer = Net::Fetch(request);
  const long long ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  const std::string limit = answer.Header("X-RateLimit-Remaining");
  std::string message, detail;
  bool answered = false;
  if (answer.status == 304 && conditional)
  {
    answered = true;
    Log("GitHub: nothing new since the last check (304, " + std::to_string(ms) + " ms" +
        (limit.empty() ? "" : ", " + limit + " checks left this hour") + ")");
  }
  else if (answer.ok())
  {
    answered = true;
    Release release;
    std::string error;
    if (ParseReleases(answer.body, memory.tests, &release, &error))
    {
      memory.found = release;
      Log("GitHub: newest " + release.tag + (release.prerelease ? " (a test version)" : "") +
          (release.important ? " [important]" : "") + ": " + release.pkg_name + ", " + std::to_string(release.pkg_size) +
          " bytes, " + (release.sha256.empty() ? std::string("no SHA-256") : "SHA-256 " + release.sha256) +
          (release.sha256_url.empty() ? "" : ", a .sha256 file") + " (" + std::to_string(answer.body.size()) +
          " bytes in " + std::to_string(ms) + " ms" + (limit.empty() ? "" : ", " + limit + " checks left this hour") +
          ")");
    }
    else
    {
      memory.found = Release{};
      Log("GitHub: " + error);
    }
    memory.etag = answer.Header("ETag");
    memory.etag_url = url;
  }
  else if (answer.status == 404)
  {
    answered = true;
    memory.found = Release{};
    memory.etag.clear();
    Log("GitHub: no release yet (404)");
  }
  else if ((answer.status == 403 || answer.status == 429) && limit == "0")
  {
    message = "GitHub's limit of checks was reached: try again in an hour";
    Log("GitHub: the hour's limit of checks for this connection was reached (HTTP " + std::to_string(answer.status) +
        ", it resets at " + answer.Header("X-RateLimit-Reset") + ")");
  }
  else
  {
    message = "The check failed: " + (answer.plain.empty() ? Net::Plain(answer.status, answer.error) : answer.plain);
    Log("the check failed (HTTP " + std::to_string(answer.status) + ", " +
        (answer.error.empty() ? std::string("no error") : answer.error) + ")");
  }
  std::lock_guard lock(s_mutex);
  if (answered)
  {
    memory.checked = static_cast<int64_t>(std::time(nullptr));
    memory.tests = s_memory.tests;  // (changed meanwhile: the next check follows it)
    memory.pending = s_memory.pending;
    memory.pending_version = s_memory.pending_version;
    memory.pending_time = s_memory.pending_time;
    s_memory = memory;
    SaveLocked();
    if (!asked)
      s_start_answered = true;
    const bool newer = HasNewer(s_memory, running);
    message = newer ? "Version " + s_memory.found.version + " is available" :
              s_memory.found.tag.empty() ? std::string("There is no version on GitHub yet") :
                                           std::string("You have the newest version");
    Log(newer ? "a newer version: " + s_memory.found.version + " (this one: " + running + ")" :
                "no newer version (this one: " + running + ")");
  }
  s_status.phase = Phase::Checked;
  s_status.message = message;
  s_status.detail = detail;
  Publish();
}

// Where the .pkg goes: a USB drive's PKG folder with room for it (not one that looks like the PS4's own storage), else
// the internal storage's update folder. False (*error) when none has room.
bool PickFolder(int64_t size, std::string* folder, bool* internal, std::string* error)
{
  for (const std::string& drive : Library::UsbDrives())
  {
    if (Library::LooksLikePs4Storage(drive))
      continue;
    int64_t free = -1;
    const bool known = emusuni_disk_space_known(drive.c_str(), &free) != 0;
    const int64_t need = SpaceNeeded(size, false);
    if (known && free < need)
    {
      Log(drive + ": not enough room (" + Mb(free) + " MiB free, " + Mb(need) + " MiB needed)");
      continue;
    }
    std::string pkg = Library::FindEntry(drive, "PKG");
    if (pkg.empty())
      pkg = drive + "/PKG";
    MakeDirs(pkg);
    struct stat info = {};
    if (stat(pkg.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
    {
      Log("couldn't make " + pkg);
      continue;
    }
    Log("the .pkg goes to " + pkg + (known ? " (" + Mb(free) + " MiB free)" : " (its free space unknown)"));
    *folder = pkg;
    *internal = false;
    return true;
  }
  const std::string update = s_data_dir + "/update";
  MakeDirs(update);
  int64_t free = -1;
  const bool known = emusuni_disk_space_known(s_data_dir.c_str(), &free) != 0;
  const int64_t need = SpaceNeeded(size, true);
  if (known && free < need)
  {
    *error = "Not enough free space: " + Mb(need) + " MB needed, " + Mb(free) + " MB free";
    Log("the internal storage: not enough room (" + Mb(free) + " MiB free, " + Mb(need) + " MiB needed, the .pkg " +
        "and the PS4's copy of it)");
    return false;
  }
  Log("no USB drive with room: the .pkg goes to " + update + (known ? " (" + Mb(free) + " MiB free)" : ""));
  *folder = update;
  *internal = true;
  return true;
}

// A name of its own for the file: the asset's when it is plain, else EmuSuNi-vNN.NN.pkg.
std::string FileName(const Release& release)
{
  bool plain = !release.pkg_name.empty() && release.pkg_name.size() < 100;
  for (const char c : release.pkg_name)
  {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
          c == '_'))
      plain = false;
  }
  return plain ? release.pkg_name : "EmuSuNi-v" + release.version + ".pkg";
}

void HashProgress(int64_t done, void*)
{
  std::lock_guard lock(s_mutex);
  s_status.done = done;
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last_progress >= std::chrono::milliseconds(200))
  {
    s_last_progress = now;
    Changed();
  }
}

// Whether the file is the release's .pkg: a PS4 package (its magic) checked by GitHub's SHA-256, else the release's
// .sha256 file, else its size. *how says by what.
bool Verify(const std::string& path, const Release& release, std::string* how, std::string* error)
{
  const int64_t size = FileSize(path);
  {
    std::lock_guard lock(s_mutex);
    s_status.phase = Phase::Verifying;
    s_status.message = "Checking the download...";
    s_status.done = 0;
    s_status.total = size;
    Changed();
  }
  if (release.pkg_size > 0 && size != release.pkg_size)
  {
    *error = "its size is " + std::to_string(size) + " bytes, GitHub says " + std::to_string(release.pkg_size);
    return false;
  }
  {
    // A PS4 package (its magic) of EmuSuNi (its content id, at 0x40: "IV0000-EMSN00001_00-...").
    unsigned char header[0x64] = {};
    FILE* file = std::fopen(path.c_str(), "rb");
    const bool read = file && std::fread(header, 1, sizeof(header), file) == sizeof(header);
    if (file)
      std::fclose(file);
    if (!read || std::memcmp(header, "\x7F" "CNT", 4) != 0)
    {
      *error = "it isn't a PS4 package";
      return false;
    }
    const std::string content(reinterpret_cast<const char*>(header + 0x40), 0x24);
    if (content.find(std::string("-") + TITLE_ID + "_") == std::string::npos)
    {
      *error = "it isn't EmuSuNi's package (" + std::string(content.c_str()) + ")";
      return false;
    }
  }
  std::string want = release.sha256;
  *how = "GitHub's SHA-256";
  if (want.empty() && !release.sha256_url.empty())
  {
    Net::Request request;
    request.url = release.sha256_url;
    request.timeout_seconds = 30;
    request.max_bytes = 64 * 1024;
    request.cancel = &s_cancel;
    const Net::Response answer = Net::Fetch(request);
    want = answer.ok() ? HashFromText(answer.body) : std::string();
    if (want.empty())
    {
      *error = "the release's .sha256 file couldn't be read (" +
               (answer.ok() ? std::string("no SHA-256 in it") : answer.plain) + ")";
      return false;
    }
    *how = "the release's .sha256 file";
  }
  if (want.empty())
  {
    *how = "its size only (the release gives no SHA-256)";
    if (release.pkg_size <= 0)
    {
      *error = "the release gives neither its SHA-256 nor its size";
      return false;
    }
    return true;
  }
  const std::string got = FileSha256(path, HashProgress, nullptr);
  if (got != want)
  {
    *error = "its SHA-256 is " + (got.empty() ? std::string("unreadable") : got) + ", " + *how + " is " + want;
    return false;
  }
  *how += " " + got;
  return true;
}

void Fail(const std::string& message, const std::string& detail = {})
{
  SetPhase(Phase::Failed, message, detail);
}

// The update (on the thread): download, check, hand over to the installer.
void UpdateJob()
{
  Release release;
  {
    std::lock_guard lock(s_mutex);
    release = s_memory.found;
  }
  if (release.pkg_url.empty())
  {
    Fail("There is no version to download");
    return;
  }
  std::string folder, error;
  bool internal = false;
  if (!PickFolder(release.pkg_size, &folder, &internal, &error))
  {
    Fail("Not enough free space for the update", error);
    return;
  }
  const std::string path = folder + "/" + FileName(release);
  const std::string part = path + ".part";
  std::string how;
  // Downloaded before but not installed (or the install failed): checked again, not downloaded again.
  bool have = false;
  if (FileSize(path) == release.pkg_size && release.pkg_size > 0)
  {
    std::string reason;
    have = Verify(path, release, &how, &reason);
    Log(have ? path + " is there already (" + how + "): not downloaded again" :
               path + " is there but isn't it (" + reason + "): downloaded again");
    if (!have)
      std::remove(path.c_str());
  }
  if (!have)
  {
    int64_t resume = FileSize(part);
    if (resume < 0 || (release.pkg_size > 0 && resume >= release.pkg_size))
    {
      std::remove(part.c_str());
      resume = 0;
    }
    {
      std::lock_guard lock(s_mutex);
      s_status.phase = Phase::Downloading;
      s_status.message = "Downloading version " + release.version + "...";
      s_status.detail = path;
      s_status.done = resume;
      s_status.total = release.pkg_size;
      s_status.saved = path;
      Changed();
    }
    Log("downloading " + release.pkg_url + " to " + path + " (" + std::to_string(release.pkg_size) + " bytes" +
        (resume > 0 ? ", going on from byte " + std::to_string(resume) : "") + ")");
    const auto start = std::chrono::steady_clock::now();
    const int64_t first = resume;
    Net::Response answer;
    for (int attempt = 0; attempt < 4; attempt++)
    {
      Net::Request request;
      request.url = release.pkg_url;
      request.timeout_seconds = 0;
      request.stall_seconds = 60;
      request.cancel = &s_cancel;
      request.resume_from = resume;
      request.progress = [](int64_t done, int64_t total) {
        std::lock_guard lock(s_mutex);
        s_status.done = done;
        if (total > 0)
          s_status.total = total;
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last_progress >= std::chrono::milliseconds(250))
        {
          s_last_progress = now;
          Changed();
        }
        return true;
      };
      answer = Net::Download(request, path);
      if (answer.ok() || s_cancel)
        break;
      if (answer.status == 416)
      {
        std::remove(part.c_str());
        resume = 0;
        continue;
      }
      if (answer.status >= 400 || answer.plain == "the storage is full or can't be written")
        break;  // (the server's answer, or the drive: not tried again)
      resume = std::max<int64_t>(0, FileSize(part));
      Log("the download was interrupted at byte " + std::to_string(resume) + " (" + answer.error + "): trying again");
      for (int i = 0; i < 30 && !s_cancel; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (s_cancel)
        break;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (s_cancel)
    {
      Log("the download was stopped at byte " + std::to_string(std::max<int64_t>(0, FileSize(part))) +
          " (it goes on from there next time)");
      SetPhase(Phase::Checked, "The download was stopped");
      return;
    }
    if (!answer.ok())
    {
      Log("the download failed (HTTP " + std::to_string(answer.status) + ", " + answer.error + ")");
      Fail("The download failed: " + (answer.plain.empty() ? Net::Plain(answer.status, answer.error) : answer.plain));
      return;
    }
    char speed[64];
    snprintf(speed, sizeof(speed), "%.0f s, %.0f KiB/s", seconds,
             seconds > 0 ? static_cast<double>(answer.bytes - first) / 1024.0 / seconds : 0.0);
    Log("downloaded " + std::to_string(answer.bytes) + " bytes (" + speed + ", " + std::to_string(answer.redirects) +
        " redirects, from " + answer.url.substr(0, answer.url.find('?')) + ")");
    std::string reason;
    if (!Verify(path, release, &how, &reason))
    {
      std::remove(path.c_str());
      Log("the download isn't the release's .pkg: " + reason + " (deleted)");
      Fail("The download is damaged: try again",
           reason == "it isn't a PS4 package" ? "What came isn't a PS4 package (does the network ask to sign in?)" :
           reason.rfind("it isn't EmuSuNi's package", 0) == 0 ? "The release's package isn't EmuSuNi's." :
           reason.find(".sha256 file") != std::string::npos ? "The release's .sha256 file couldn't be read." :
                                                               "It doesn't match the version on GitHub.");
      return;
    }
  }
  Log(path + " checked: " + how);
  // To the PS4's installer.
  {
    std::lock_guard lock(s_mutex);
    s_status.phase = Phase::Installing;
    s_status.message = "Handing the update to the PS4...";
    s_status.saved = path;
    Changed();
  }
  // Not installed from here: kept for GoldHEN's Package Installer (the PS4's Debug Settings), which lists the USB
  // drives and /data/pkg - one on the internal storage is moved there (the same partition: at once) and deleted once
  // the new version runs. Where it is now.
  auto keep = [&]() {
    std::string where = path;
    if (internal)
    {
      const size_t slash = s_data_dir.rfind('/');
      const std::string folder = (slash == std::string::npos || slash == 0 ? std::string() : s_data_dir.substr(0, slash)) +
                                 "/pkg";
      MakeDirs(folder);
      const std::string moved = folder + "/" + FileName(release);
      std::remove(moved.c_str());
      if (std::rename(path.c_str(), moved.c_str()) == 0)
        where = moved;
      std::lock_guard lock(s_mutex);
      s_memory.pending = where;
      s_memory.pending_version = release.version;
      s_memory.pending_time = 0;  // (kept until the new version runs: the player installs it)
      SaveLocked();
    }
    std::lock_guard lock(s_mutex);
    s_status.saved = where;
    return where;
  };
  std::string install_error;
  if (!Install::Start(path, release.version, s_data_dir, &install_error))
  {
    const std::string where = keep();
    Log("the PS4's installer couldn't be asked: " + install_error + "; the .pkg stays at " + where);
    SetPhase(Phase::Saved, "The update couldn't be installed from EmuSuNi",
             "Saved as " + where + ": install it with GoldHEN's Package Installer (Debug Settings > Game).");
    return;
  }
  Log("handed to the PS4's installer: " + path);
  if (internal)
  {
    std::lock_guard lock(s_mutex);
    s_memory.pending = path;  // (deleted once installed; else at the next start of the new version)
    s_memory.pending_version = release.version;
    s_memory.pending_time = static_cast<int64_t>(std::time(nullptr));
    SaveLocked();
  }
  {
    std::lock_guard lock(s_mutex);
    s_status.phase = Phase::Installing;
    s_status.message = "Installing version " + release.version + ": see the PS4's notifications";
    s_status.detail = "When it is installed, close EmuSuNi and open it again.";
    s_status.done = 0;
    s_status.total = -1;
    Changed();
  }
  // The PS4's copy of it, followed (its progress in the panel).
  std::string state;
  const Install::Result result = Install::Follow(release.pkg_size, &s_cancel, &state, [](int64_t done, int64_t total) {
    std::lock_guard lock(s_mutex);
    s_status.done = done;
    s_status.total = total;
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last_progress >= std::chrono::milliseconds(250))
    {
      s_last_progress = now;
      Changed();
    }
  });
  Log("the installation: " + state);
  if (result == Install::Result::Failed)
  {
    const std::string where = keep();
    SetPhase(Phase::Failed, "The PS4 couldn't install the update",
             "Saved as " + where + ": install it with GoldHEN's Package Installer (Debug Settings > Game).");
    return;
  }
  if (result == Install::Result::Unknown)
    return;  // (the message stays: see the PS4's notifications; an internal .pkg goes at the next start)
  // The internal storage's .pkg: deleted once the PS4 has all of it (the owner keeps no packages there).
  if (internal && std::remove(path.c_str()) == 0)
  {
    Log("deleted " + path + " (the PS4 has it)");
    std::lock_guard lock(s_mutex);
    s_memory.pending.clear();
    SaveLocked();
  }
  SetPhase(Phase::Installed, "Version " + release.version + " is ready",
           "Close EmuSuNi to finish installing it, then open it again.");
}

// One job at a time on the thread (a finished one's thread joined first).
// (pthread, not std::thread: a thread that can't be made is an error code, never an exception. Its stack is the
// default one, which libthr keeps for the next thread of that size when it ends.)
void (*s_job)(bool) = nullptr;
bool s_job_argument = false;

void* Thread(void*)
{
  s_job(s_job_argument);
  std::lock_guard done(s_mutex);
  s_busy = false;
  Changed();
  return nullptr;
}

bool Run(void (*job)(bool), bool argument)
{
  std::lock_guard lock(s_mutex);
  if (s_busy)
    return false;
  if (s_thread_made)
  {
    pthread_join(s_thread, nullptr);  // (finished: s_busy is false)
    s_thread_made = false;
  }
  s_job = job;
  s_job_argument = argument;
  s_cancel = false;
  s_busy = true;
  const int made = pthread_create(&s_thread, nullptr, Thread, nullptr);
  if (made != 0)
  {
    s_busy = false;
    s_status.phase = Phase::Failed;
    s_status.message = "The update's thread couldn't start";
    Changed();
    Log("pthread_create failed: " + std::to_string(made));
    return false;
  }
  s_thread_made = true;
  return true;
}

void CheckRun(bool asked)
{
  CheckJob(asked);
}

void UpdateRun(bool)
{
  UpdateJob();
}
}  // namespace

void Start(const std::string& data_dir, const std::string& running)
{
  {
    std::lock_guard lock(s_mutex);
    if (s_started)
      return;
    s_started = true;
    s_data_dir = data_dir;
    s_status.running = running;
    s_memory = LoadMemory(ReadTextFile(MemoryPath()));
    const std::string url = Trim(ReadTextFile(data_dir + "/update-url.txt"));
    s_url = url.substr(0, url.find('\n'));
    s_url = Trim(s_url);
    if (!s_url.empty())
      Log("checks go to " + s_url + " (update-url.txt)");
    // A .pkg left on the internal storage: deleted once installed (this version or a newer one runs), or a day after
    // it was handed to the PS4's installer (one kept for GoldHEN's Package Installer stays until it is installed).
    if (!s_memory.pending.empty())
    {
      const bool installed = !IsNewer(s_memory.pending_version, running);
      const int64_t age = static_cast<int64_t>(std::time(nullptr)) - s_memory.pending_time;
      if (installed || (s_memory.pending_time > 0 && (age > DAY || age < 0)))
      {
        const bool gone = std::remove(s_memory.pending.c_str()) == 0 || FileSize(s_memory.pending) < 0;
        Log("deleted " + s_memory.pending + (installed ? " (installed: this is " + running + ")" : " (a day old)") +
            (gone ? "" : ": couldn't be deleted"));
        std::remove((s_memory.pending + ".part").c_str());
        s_memory.pending.clear();
        SaveLocked();
      }
    }
    s_due_at_start = CheckDue(s_memory, static_cast<int64_t>(std::time(nullptr)));
    if (s_due_at_start)
    {
      s_status.phase = Phase::Checking;
      s_status.message = "Checking for updates...";
    }
    else
    {
      // What the last check found.
      s_status.phase = Phase::Checked;
      s_status.message = HasNewer(s_memory, running) ? "Version " + s_memory.found.version + " is available" :
                         s_memory.found.tag.empty()  ? std::string() :
                                                       std::string("You have the newest version");
    }
    Publish();
    char line[200];
    snprintf(line, sizeof(line), "this is %s; last check %s; known newest %s%s; test versions %s",
             running.c_str(), s_memory.checked > 0 ? "remembered" : "never", s_memory.found.tag.empty() ? "none" :
             s_memory.found.tag.c_str(), s_status.newer ? " (newer)" : "", s_memory.tests ? "on" : "off");
    Log(line);
    if (!s_due_at_start)
      Log("checked less than a day ago: no check now");
  }
  if (s_due_at_start)
    Run(CheckRun, false);
}

void Check()
{
  {
    std::lock_guard lock(s_mutex);
    if (!s_started || s_busy)
      return;
    s_status.phase = Phase::Checking;
    s_status.message = "Checking for updates...";
    s_status.detail.clear();
    Changed();
  }
  if (!Run(CheckRun, true))
  {
    std::lock_guard lock(s_mutex);
    Changed();
  }
}

void StartUpdate()
{
  {
    std::lock_guard lock(s_mutex);
    if (!s_started || s_busy || !HasNewer(s_memory, s_status.running))
      return;
    s_status.phase = Phase::Downloading;
    s_status.message = "Downloading version " + s_memory.found.version + "...";
    s_status.detail.clear();
    s_status.done = 0;
    s_status.total = s_memory.found.pkg_size;
    Changed();
  }
  Log("the player chose Update (" + Get().newest.tag + ")");
  Run(UpdateRun, false);
}

void Cancel()
{
  s_cancel = true;
}

void SetTests(bool on)
{
  {
    std::lock_guard lock(s_mutex);
    if (!s_started || s_memory.tests == on)
      return;
    s_memory.tests = on;
    s_memory.etag.clear();  // (another answer)
    SaveLocked();
    Publish();
  }
  Log(std::string("test versions ") + (on ? "included" : "left out"));
  Check();
}

Status Get()
{
  std::lock_guard lock(s_mutex);
  Status status = s_status;
  status.busy = s_busy;
  return status;
}

int Serial()
{
  std::lock_guard lock(s_mutex);
  return s_status.serial;
}

bool NoticeWanted()
{
  std::lock_guard lock(s_mutex);
  // (Not while the thread works, nor once an update was started: the player chose already.)
  return s_started && !s_busy && s_status.phase == Phase::Checked &&
         NoticeDue(s_memory, s_status.running, s_notice_shown, s_due_at_start, s_start_answered);
}

void NoticeShown()
{
  std::lock_guard lock(s_mutex);
  if (!s_notice_shown)
    Log(std::string("the notice of ") + s_memory.found.version + (s_memory.found.important ? " (important)" : "") +
        " shown");
  s_notice_shown = true;
}

void Stop()
{
  s_cancel = true;
  pthread_t thread{};
  bool made;
  {
    std::lock_guard lock(s_mutex);
    made = s_thread_made;
    thread = s_thread;
    s_thread_made = false;
  }
  if (made)
    pthread_join(thread, nullptr);
}
}  // namespace Update
