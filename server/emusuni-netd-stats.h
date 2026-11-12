// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// emusuni-netd's anonymous statistics (00.23): how many use EmuSuNi, from which countries, with which versions, what
// they play, how many have it open right now. Part of emusuni-netd.cpp: included inside its namespace, after its
// SHA-256, its token bucket and its AddressState (one build, one file to compile).
//
// What comes in: the app's heartbeat (NetProto::Heartbeat; src/UsageStats.h) when it starts and every 5 minutes while
// it is open - a random install id the app made itself (never the PS4's or a user's), the app's version, the PS4's
// language, menu / playing a console / online, the update checks and an update since the last one. Players turn it off
// in Settings -> Anonymous Statistics: then nothing is sent.
//
// What is kept, and how (all of it in <stats-dir>/stats.db, SQLite: server/emusuni-netd-db.h):
//  - The address is only looked up in the country database (DB-IP's IP to Country Lite, --geoip: CC BY 4.0, "IP
//    Geolocation by DB-IP", https://db-ip.com) when the packet comes in, and dropped: no address is stored or logged.
//    (The per-address rate limits are the server's memory for 10 minutes, as for every request.)
//  - A day's unique installs: a set of the ids hashed with that day's random salt (SHA-256), in memory and in the
//    table seen (so a restart keeps the day) - the set and the salt are deleted when the day ends (UTC).
//  - Uniques over several days (the week's, the month's): HyperLogLog sketches (16384 registers; 2048 for each country,
//    version, language, console and state, 256 for each game) of the ids hashed with a salt kept in the database -
//    registers, not ids: no id can be read back from them. Their estimates are within about 1-2% (a day's are exact).
//  - Online now: the installs heard in the last 6 minutes, in memory only (meta "live": the counts every minute).
//  - day, day_label, day_update: each day's counts - active installs, new ones, heartbeats, update checks, the updates
//    installed (from -> to), the peak online - by country, version, language, console, state and game, with sketches.
//  - downloads: GitHub's download counts of each release's .pkg, a row a day (record-downloads, deploy.sh's daily
//    timer, from GitHub's public API).
//  - The game played: a label "<console>.<c|s|d>-<id>" (a cartridge's CRC32, an arcade set, a disc's code - never a
//    file's name; anything else dropped), counted as installs and hours; its title only from titles.txt (build-titles,
//    from libretro-database's and FinalBurn Neo's lists), public only once PUBLIC_GAME_INSTALLS installs played it.
//  - meta "rooms" (with "live"): the public rooms - waiting or playing - their console, the game's CRC32, the host's
//    time zone, players; the private ones counted. No nickname, no code.
//  - Opt-in profiles only (STATS_PROFILE: Settings -> Online Play -> Online Profile, off unless turned on): that
//    install's minutes, sessions and online matches by game and console, its country, and - public ones only - its
//    nickname, keyed by its id hashed with a salt of the server's; a code (ProfileReply) opens it on the website.
//    Deleted when the app asks (Delete My Profile), or PROFILE_KEPT_DAYS after its last heartbeat.
//
//  - (00.95) The public page counts only the versions published on GitHub (record-downloads keeps them: meta
//    "published", the releases' tags but the pre-releases, "v00.22" -> "00.22"; none known: none): a published
//    version's heartbeat is counted twice - in the day of every version (report, the owner's panel) and in the
//    published ones' own (pub_ tables, with their own set of installs) -, online now and the rooms by version too,
//    a profile's last version kept. public-json and public-profiles read only those.
//
//   emusuni-netd report [--stats-dir DIR] [--days 30] [--json] [--titles FILE]   all of it, as text or JSON
//   emusuni-netd public-json [--stats-dir DIR] [--titles FILE]   the public page's numbers (publish-stats.sh)
//   emusuni-netd public-profiles --out DIR [--stats-dir DIR] [--titles FILE]   the profiles' sealed files, rankings
//   emusuni-netd profile CODE [--delete]                          one profile (support: shown, or deleted)
//   emusuni-netd build-titles OUT console:kind:DAT...             titles.txt (update-titles.sh)
//   curl -s https://api.github.com/repos/OWNER/REPO/releases?per_page=100 |
//       emusuni-netd record-downloads [--stats-dir DIR]
//
// Abuse: a heartbeat needs the cookie (no spoofed address). From one address: STATS_BURST heartbeats at once, then one
// every 5 s; NEW_IDS_BURST installs not seen yet that day at once, then 20 an hour (more are answered, not counted). A
// heartbeat adds play time once a minute at most; the sets and maps are capped (beyond them: answered, not counted).

constexpr double STATS_RATE = 0.2, STATS_BURST = 30.0;               // heartbeats an address may send
constexpr double NEW_IDS_RATE = 20.0 / 3600.0, NEW_IDS_BURST = 20.0;  // installs not seen today, from one address
constexpr double ONLINE_WINDOW = 360.0;  // s: an install heard this recently has the app open
double s_beat_min_gap = 50.0;  // s: a heartbeat sooner after the last one counted adds no play time (tests: --beat-gap)
constexpr uint16_t HEARTBEAT_EVERY = 300;
constexpr size_t MAX_LIVE = 500000, MAX_SEEN = 2000000, MAX_LABELS = 300, MAX_CONSOLE_BITS = 64;
constexpr int SKETCH_ALL = 14, SKETCH_LABEL = 11, SKETCH_GAME = 8;
constexpr size_t MAX_GAME_LABELS = 20000;  // games counted in a day (more: "other")
constexpr size_t GAMES_A_DAY = 32;         // an install's games counted in a day
constexpr int PUBLIC_GAME_INSTALLS = 3;    // a game's title is public once this many installs played it
constexpr int PROFILE_KEPT_DAYS = 400;     // an opt-in profile unseen this long is deleted
constexpr double PROFILES_RATE = 5.0 / 3600.0, PROFILES_BURST = 5.0;  // new profiles from one address
double s_live_every = 60.0, s_save_every = 300.0;  // (live.txt, the day's file; the PC test's builds set them)
constexpr int UPDATE_CHECKS_A_DAY = 24;  // an install's update checks counted in a day at most

// ---- HyperLogLog: unique counts that add up over days without keeping the ids ----
struct Hll
{
  int p = SKETCH_ALL;
  std::vector<uint8_t> reg;
  explicit Hll(int bits = SKETCH_ALL) : p(bits), reg(size_t{1} << bits, 0) {}
  void Add(uint64_t h)
  {
    const size_t index = static_cast<size_t>(h >> (64 - p));
    const uint64_t rest = (h << p) | (uint64_t{1} << (p - 1));
    const uint8_t rank = static_cast<uint8_t>(__builtin_clzll(rest) + 1);
    reg[index] = std::max(reg[index], rank);
  }
  void Merge(const Hll& o)
  {
    if (o.p != p)
      return;
    for (size_t i = 0; i < reg.size(); i++)
      reg[i] = std::max(reg[i], o.reg[i]);
  }
  double Estimate() const
  {
    const double m = static_cast<double>(reg.size());
    double sum = 0.0;
    size_t zeros = 0;
    for (const uint8_t r : reg)
    {
      sum += std::ldexp(1.0, -static_cast<int>(r));
      zeros += r == 0 ? 1 : 0;
    }
    const double alpha = 0.7213 / (1.0 + 1.079 / m);
    double e = alpha * m * m / sum;
    if (e <= 2.5 * m && zeros > 0)
      e = m * std::log(m / static_cast<double>(zeros));  // (linear counting: exact enough for small counts)
    return e;
  }
  // One word, no spaces: "d" and a letter a register ('0' + its value), or - few registers set - "s" and each set one
  // as its index (4 hex digits) and its letter.
  std::string Text() const
  {
    size_t set = 0;
    for (const uint8_t r : reg)
      set += r != 0 ? 1 : 0;
    if (set * 5 < reg.size())
    {
      std::string s = "s";
      char entry[24];
      for (size_t i = 0; i < reg.size(); i++)
      {
        if (reg[i] == 0)
          continue;
        std::snprintf(entry, sizeof(entry), "%04x%c", static_cast<unsigned>(i),
                      static_cast<char>('0' + std::min<int>(reg[i], 64)));
        s += entry;
      }
      return s;
    }
    std::string s(reg.size() + 1, '0');
    s[0] = 'd';
    for (size_t i = 0; i < reg.size(); i++)
      s[i + 1] = static_cast<char>('0' + std::min<int>(reg[i], 64));
    return s;
  }
  bool FromText(const std::string& s)
  {
    std::fill(reg.begin(), reg.end(), 0);
    if (!s.empty() && s[0] == 'd' && s.size() == reg.size() + 1)
    {
      for (size_t i = 0; i < reg.size(); i++)
        reg[i] = static_cast<uint8_t>(std::clamp(s[i + 1] - '0', 0, 64));
      return true;
    }
    if (s.empty() || s[0] != 's' || (s.size() - 1) % 5 != 0)
      return false;
    for (size_t at = 1; at + 5 <= s.size(); at += 5)
    {
      const size_t index = std::strtoul(s.substr(at, 4).c_str(), nullptr, 16);
      if (index < reg.size())
        reg[index] = static_cast<uint8_t>(std::clamp(s[at + 4] - '0', 0, 64));
    }
    return true;
  }
};

// ---- Time (UTC days) ----
int64_t s_clock_offset = 0;  // (the PC test's builds only: --clock-offset, a day later without waiting a day)

int64_t WallNow()
{
  return static_cast<int64_t>(std::time(nullptr)) + s_clock_offset;
}

std::string TimeText(int64_t t, const char* format)
{
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  char text[32];
  std::strftime(text, sizeof(text), format, &tm);
  return text;
}

std::string DateOf(int64_t t)
{
  return TimeText(t, "%Y-%m-%d");
}

// ---- The country database: DB-IP's IP to Country Lite CSV ("first,last,CC" a line; IPv6 lines skipped) ----
struct GeoRange
{
  uint32_t lo = 0, hi = 0;
  char cc[2] = {'?', '?'};
};
std::vector<GeoRange> s_geo;
std::string s_geo_path;
std::time_t s_geo_mtime = 0;

bool ParseIp(const std::string& text, uint32_t* ip)
{
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 ||
      d > 255)
    return false;
  *ip = (a << 24) | (b << 16) | (c << 8) | d;
  return true;
}

bool LoadGeo(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    return false;
  std::vector<GeoRange> ranges;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#' || line.find(':') != std::string::npos)
      continue;
    line.erase(std::remove(line.begin(), line.end(), '"'), line.end());
    std::string field[3];
    size_t at = 0;
    int n = 0;
    for (; n < 3 && at <= line.size(); n++)
    {
      const size_t comma = std::min(line.find(',', at), line.size());
      field[n] = line.substr(at, comma - at);
      at = comma + 1;
    }
    GeoRange r;
    if (n < 3 || !ParseIp(field[0], &r.lo) || !ParseIp(field[1], &r.hi) || r.hi < r.lo || field[2].size() < 2)
      continue;
    r.cc[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(field[2][0])));
    r.cc[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(field[2][1])));
    if (!std::isalpha(static_cast<unsigned char>(r.cc[0])) || !std::isalpha(static_cast<unsigned char>(r.cc[1])))
      continue;
    ranges.push_back(r);
  }
  std::sort(ranges.begin(), ranges.end(), [](const GeoRange& a, const GeoRange& b) { return a.lo < b.lo; });
  // Neighbours of one country joined: fewer ranges, the same answers.
  std::vector<GeoRange> merged;
  for (const GeoRange& r : ranges)
  {
    if (!merged.empty() && merged.back().cc[0] == r.cc[0] && merged.back().cc[1] == r.cc[1] &&
        merged.back().hi != UINT32_MAX && merged.back().hi + 1 >= r.lo)
      merged.back().hi = std::max(merged.back().hi, r.hi);
    else
      merged.push_back(r);
  }
  if (merged.empty())
    return false;
  s_geo.swap(merged);
  return true;
}

// The country of an address ("??": not known). The address goes no further than this.
std::string CountryOf(uint32_t ip)
{
  auto it = std::upper_bound(s_geo.begin(), s_geo.end(), ip, [](uint32_t v, const GeoRange& r) { return v < r.lo; });
  if (it == s_geo.begin())
    return "??";
  --it;
  if (ip > it->hi || (it->cc[0] == 'Z' && it->cc[1] == 'Z'))
    return "??";
  return std::string(it->cc, 2);
}

// The database read again when its file changed (deploy.sh's monthly timer replaces it).
void CheckGeo(bool force)
{
  if (s_geo_path.empty())
    return;
  struct stat st{};
  if (stat(s_geo_path.c_str(), &st) != 0)
  {
    if (force)
      std::printf("stats: the country database %s isn't there: countries \"??\"\n", s_geo_path.c_str());
    return;
  }
  if (!force && st.st_mtime == s_geo_mtime)
    return;
  s_geo_mtime = st.st_mtime;
  if (LoadGeo(s_geo_path))
    std::printf("stats: the country database read (%zu ranges)\n", s_geo.size());
  else
    std::printf("stats: the country database %s couldn't be read\n", s_geo_path.c_str());
}

// ---- A day's counts ----
struct Label
{
  uint64_t installs = 0;  // unique installs that day
  uint64_t beats = 0;     // heartbeats counted (consoles, games: x5 minutes is the time played)
  Hll sketch;
  explicit Label(int precision = SKETCH_LABEL) : sketch(precision) {}
};

// A kind's sketches: the games' small (thousands of them a day), the others' finer.
int SketchOf(const std::string& kind)
{
  return kind == "game" ? SKETCH_GAME : SKETCH_LABEL;
}

struct Day
{
  std::string date;
  uint64_t active = 0, fresh = 0, heartbeats = 0, update_checks = 0, uncounted = 0;
  uint64_t peak = 0;
  std::string peak_at;
  Hll all{SKETCH_ALL};
  std::map<std::string, std::map<std::string, Label>> labels;  // kind -> value -> counts
  std::map<std::pair<std::string, std::string>, uint64_t> updates;  // (from, to) -> installs
};

const char* const LABEL_KINDS[] = {"country", "version", "language", "state", "console", "game",
                                   "community"};  // ((01.04) the community online modes: the owner's only)

bool ParseDay(const std::string& text, Day* d)
{
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("emusuni-stats-day ", 0) != 0)
    return false;
  while (std::getline(in, line))
  {
    std::istringstream words(line);
    std::string key;
    words >> key;
    if (key == "date")
      words >> d->date;
    else if (key == "active")
      words >> d->active;
    else if (key == "new")
      words >> d->fresh;
    else if (key == "heartbeats")
      words >> d->heartbeats;
    else if (key == "update_checks")
      words >> d->update_checks;
    else if (key == "uncounted")
      words >> d->uncounted;
    else if (key == "peak")
    {
      words >> d->peak >> d->peak_at;
      if (d->peak_at == "-")
        d->peak_at.clear();
    }
    else if (key == "all")
    {
      std::string s;
      words >> s;
      d->all.FromText(s);
    }
    else if (key == "label")
    {
      std::string kind, value, sketch;
      if (words >> kind)
      {
        Label l(SketchOf(kind));
        if (words >> value >> l.installs >> l.beats >> sketch)
        {
          l.sketch.FromText(sketch);
          d->labels[kind].insert_or_assign(value, l);
        }
      }
    }
    else if (key == "update")
    {
      std::string from, to;
      uint64_t n = 0;
      if (words >> from >> to >> n)
        d->updates[{from, to}] = n;
    }
  }
  return !d->date.empty();
}

std::string ReadWhole(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Written whole or not at all (a .tmp renamed over it).
bool WriteWhole(const std::string& path, const std::string& data)
{
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out)
      return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

#include "emusuni-netd-db.h"

// ---- The live state ----
struct Seen  // an install seen today (its key: the id hashed with the day's salt)
{
  uint64_t consoles = 0;  // bits: the day's consoles it played (Stats::console_bits)
  uint8_t states = 0;     // bits: the states it was in
  uint8_t communities = 0;  // (01.04) bits: the community online modes it used (COMMUNITY_MODES; seen's states << 8)
  uint8_t checks = 0;     // update checks counted
  bool updated = false;   // an update counted
  double last_beat = -1e9;
  std::vector<uint32_t> games;  // the games it played today (GameHash of their labels), GAMES_A_DAY at most
};

// (01.04) The community online modes a game can be connected to while it plays - servers that aren't ours, which the
// app sees in its cores' own log lines (src/UsageStats.cpp: CoreLine) - in the heartbeat's community: only the mode's
// name, never an address. Anything else is dropped. The owner's panel shows them (label "community"), not the public
// page. At most 8 (Seen::communities).
const char* const COMMUNITY_MODES[] = {"psp-adhoc", "psp-infra", "dcnet", "wfc-wiimmfi", "wfc-wiilink"};
int CommunityBit(const std::string& mode)
{
  for (size_t i = 0; i < sizeof(COMMUNITY_MODES) / sizeof(COMMUNITY_MODES[0]); i++)
  {
    if (mode == COMMUNITY_MODES[i])
      return static_cast<int>(i);
  }
  return -1;
}

uint32_t GameHash(const std::string& label)
{
  uint32_t h = 2166136261u;  // (FNV-1a)
  for (const char c : label)
    h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return h;
}

struct LiveEntry  // an install heard in the last minutes (in memory only)
{
  double last = 0.0;
  StatsState state = StatsState::Menu;
  std::string console, country, version;
};

struct StatsData
{
  bool on = false;
  std::string dir;
  sqlite3* db = nullptr;                // stats.db (emusuni-netd-db.h)
  std::unordered_set<uint64_t> dirty;   // today's installs whose counts changed since the last save
  uint8_t profile_salt[32] = {};        // kept: the opt-in profiles' ids are the install ids hashed with it
  uint8_t sketch_salt[32] = {};  // kept (stats folder): the sketches of every day must hash an id the same way
  uint8_t day_salt[32] = {};     // today's (today.bin), gone with the day
  uint8_t live_salt[32] = {};    // this process's
  Day day;
  std::vector<std::string> console_bits;  // today's consoles, in the order first seen
  std::unordered_map<uint64_t, Seen> seen;
  std::unordered_map<uint64_t, LiveEntry> live;
  double last_save = 0.0, last_live = 0.0, last_prune = 0.0, last_geo = 0.0;
  uint64_t beats_since_log = 0;
  // (00.95) The versions published on GitHub (meta "published": record-downloads, from its releases - no test
  // version, no draft) and today's counts of their heartbeats alone (the public page's), with their own set.
  std::set<std::string> published;
  double last_published = -1e9;
  Day pub;
  std::unordered_map<uint64_t, Seen> pub_seen;
  std::unordered_set<uint64_t> pub_dirty;
} s_stats;

// (00.95) The published versions as meta "published" says them ("00.22 00.23": the releases' tags without their v).
// None said yet: none (never every version).
std::set<std::string> PublishedOf(sqlite3* db)
{
  std::set<std::string> out;
  std::istringstream words(db ? StoreMeta(db, "published") : std::string());
  std::string v;
  while (words >> v)
    out.insert(v);
  return out;
}

bool Published(const std::string& version)
{
  return s_stats.published.count(version) != 0;
}

uint64_t SaltedKey(const uint8_t salt[32], const uint8_t id[INSTALL_ID_SIZE])
{
  Sha256 h;
  h.Add(salt, 32);
  h.Add(id, INSTALL_ID_SIZE);
  uint8_t out[32];
  h.Finish(out);
  uint64_t key = 0;
  std::memcpy(&key, out, 8);
  return key;
}

void RandomBytes(uint8_t* out, size_t n)
{
  for (size_t i = 0; i < n; i += 8)
  {
    const uint64_t r = Random64();
    std::memcpy(out + i, &r, std::min<size_t>(8, n - i));
  }
}

std::string Hex(const uint8_t* p, size_t n)
{
  static const char digits[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++)
  {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
  }
  return s;
}

bool FromHex(const std::string& text, uint8_t* out, size_t n)
{
  if (text.size() < n * 2)
    return false;
  for (size_t i = 0; i < n; i++)
  {
    unsigned v = 0;
    if (std::sscanf(text.c_str() + i * 2, "%2x", &v) != 1)
      return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

// A label's text: letters, digits, '.', '-', '_' only, at most `limit` ("" when nothing is left).
std::string Token(const std::string& in, size_t limit)
{
  std::string out;
  for (const char c : in)
  {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')
      out += c;
    if (out.size() >= limit)
      break;
  }
  return out;
}

// A heartbeat's game as a label, "" when it isn't one of the three forms (anything else is dropped): "c:" and a CRC32
// in 8 lowercase hex digits, "s:" and an arcade set's name (a-z, 0-9, _), "d:" and a disc's code (A-Z, 0-9, -).
std::string GameLabel(const std::string& console, const std::string& game)
{
  if (console.empty() || console == "?" || game.size() < 3 || game[1] != ':')
    return {};
  const std::string id = game.substr(2);
  bool ok = false;
  if (game[0] == 'c')
    ok = id.size() == 8 && std::all_of(id.begin(), id.end(), [](char c) {
           return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f');
         });
  else if (game[0] == 's')
    ok = id.size() <= 20 && std::all_of(id.begin(), id.end(), [](char c) {
           return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'z') || c == '_';
         });
  else if (game[0] == 'd')
    ok = id.size() >= 4 && id.size() <= 12 && std::all_of(id.begin(), id.end(), [](char c) {
           return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'A' && c <= 'Z') || c == '-';
         });
  return ok ? console + "." + game[0] + "-" + id : std::string();
}

const char* StateName(StatsState s)
{
  switch (s)
  {
  case StatsState::Menu:
    return "menu";
  case StatsState::Playing:
    return "playing";
  case StatsState::Room:
    return "online-room";
  case StatsState::PlayingOnline:
    return "online-match";
  }
  return "menu";
}

Label& LabelOf(Day& day, const std::string& kind, const std::string& value)
{
  auto& values = day.labels[kind];
  const size_t most = kind == "game" ? MAX_GAME_LABELS : MAX_LABELS;
  const bool full = values.size() >= most && values.find(value) == values.end();
  return values.try_emplace(full ? std::string("other") : value, Label(SketchOf(kind))).first->second;
}

// Today's set (only the installs whose counts changed since the last save) and today's day, salt and consoles' order:
// a restart goes on with them.
// The installs of a set changed since the last save, into its table (seen, pub_seen).
void SaveSeen(sqlite3* db, const char* table, const std::unordered_map<uint64_t, Seen>& seen,
              std::unordered_set<uint64_t>& dirty)
{
  Stmt put(db, (std::string("INSERT OR REPLACE INTO ") + table + " VALUES(?, ?, ?, ?, ?, ?)").c_str());
  for (const uint64_t key : dirty)
  {
    const auto it = seen.find(key);
    if (it == seen.end())
      continue;
    const Seen& s = it->second;
    std::string games;
    char hex[16];
    for (size_t i = 0; i < s.games.size(); i++)
    {
      std::snprintf(hex, sizeof(hex), "%s%x", i ? "," : "", s.games[i]);
      games += hex;
    }
    // ((01.04) The community modes in the states' column, above its 8 bits: no column more - a server of before
    // reads the states alone.)
    put.Int(1, static_cast<int64_t>(key)).Int(2, static_cast<int64_t>(s.consoles))
        .Int(3, s.states | (static_cast<int64_t>(s.communities) << 8)).Int(4, s.checks)
        .Int(5, s.updated ? 1 : 0).Text(6, games).Run();
  }
  dirty.clear();
}

// ---- (01.04) The requests refused a day, by request and why (SendRefused; refusal_day: numbers only) ----
std::map<std::tuple<std::string, std::string, std::string>, uint64_t> s_refusals;  // (date, request, why): not saved yet

const char* RefusalName(Refusal why)
{
  switch (why)
  {
  case Refusal::NoRoom: return "no-room";
  case Refusal::Full: return "full";
  case Refusal::Version: return "version";
  case Refusal::TooMany: return "too-many";
  case Refusal::ServerFull: return "server-full";
  case Refusal::BadRequest: return "bad-request";
  case Refusal::Started: return "started";
  case Refusal::BadCookie: return "bad-cookie";
  case Refusal::Kicked: return "kicked";
  case Refusal::NoWatch: return "no-watch";  // (01.05)
  }
  return "other";
}

const char* RequestName(Type type)
{
  switch (type)
  {
  case Type::Hello: return "hello";
  case Type::Create: return "create";
  case Type::Join: return "join";
  case Type::List: return "list";
  case Type::ListAll: return "list-all";
  case Type::Info: return "info";
  default: return "other";
  }
}

void CountRefusal(Type request, Refusal why)
{
  if (!s_stats.on || s_refusals.size() > 1000)
    return;
  s_refusals[{DateOf(WallNow()), RequestName(request), RefusalName(why)}]++;
}

void SaveRefusals(sqlite3* db)
{
  for (const auto& [key, n] : s_refusals)
  {
    Stmt q(db, "INSERT INTO refusal_day(date, request, why, n) VALUES (?, ?, ?, ?) "
               "ON CONFLICT(date, request, why) DO UPDATE SET n = n + excluded.n");
    q.Text(1, std::get<0>(key)).Text(2, std::get<1>(key)).Text(3, std::get<2>(key)).Int(4, static_cast<int64_t>(n)).Run();
  }
  s_refusals.clear();
}

void SaveDay()
{
  sqlite3* db = s_stats.db;
  if (!s_stats.on || !db || s_stats.day.date.empty())
    return;
  StoreExec(db, "BEGIN");
  SaveRefusals(db);  // (01.04)
  StoreSaveDay(db, s_stats.day);
  SaveSeen(db, "seen", s_stats.seen, s_stats.dirty);
  StoreSaveDay(db, s_stats.pub, "pub_");  // (00.95)
  SaveSeen(db, "pub_seen", s_stats.pub_seen, s_stats.pub_dirty);
  std::string consoles;
  for (const std::string& c : s_stats.console_bits)
    consoles += c + " ";
  StoreSetMeta(db, "today", s_stats.day.date);
  StoreSetMeta(db, "today_salt", Hex(s_stats.day_salt, 32));
  StoreSetMeta(db, "today_consoles", consoles);
  StoreExec(db, "COMMIT");
}

void NewDay(const std::string& date)
{
  s_stats.day = Day();
  s_stats.day.date = date;
  s_stats.seen.clear();
  s_stats.dirty.clear();
  s_stats.pub = Day();
  s_stats.pub.date = date;
  s_stats.pub_seen.clear();
  s_stats.pub_dirty.clear();
  s_stats.console_bits.clear();
  RandomBytes(s_stats.day_salt, 32);
}

// A new day (UTC): the one ending saved for good, its set and salt deleted; profiles a year and more unseen too.
void CheckDay()
{
  const std::string today = DateOf(WallNow());
  if (today == s_stats.day.date)
    return;
  if (!s_stats.day.date.empty() && s_stats.db)
  {
    StoreExec(s_stats.db, "BEGIN");
    StoreSaveDay(s_stats.db, s_stats.day);
    StoreSaveDay(s_stats.db, s_stats.pub, "pub_");  // (00.95)
    StoreExec(s_stats.db, "COMMIT");
    std::printf("stats: %s ended: %llu active installs (%llu new), peak %llu online\n", s_stats.day.date.c_str(),
                static_cast<unsigned long long>(s_stats.day.active), static_cast<unsigned long long>(s_stats.day.fresh),
                static_cast<unsigned long long>(s_stats.day.peak));
  }
  NewDay(today);
  if (s_stats.db)
  {
    StoreExec(s_stats.db, "DELETE FROM seen");
    StoreExec(s_stats.db, "DELETE FROM pub_seen");
    Stmt old(s_stats.db, "SELECT id FROM profile WHERE last_seen < ?");
    old.Int(1, WallNow() - PROFILE_KEPT_DAYS * 86400);
    std::vector<std::string> gone;
    while (old.Row())
      gone.emplace_back(static_cast<const char*>(sqlite3_column_blob(old.s, 0)),
                        static_cast<size_t>(sqlite3_column_bytes(old.s, 0)));
    for (const std::string& id : gone)
      StoreDeleteProfile(s_stats.db, id.data(), static_cast<int>(id.size()));
    if (!gone.empty())
      std::printf("stats: %zu profiles unseen for %d days deleted\n", gone.size(), PROFILE_KEPT_DAYS);
  }
  SaveDay();
}

// The text files of a server before SQLite (day-*.txt, downloads.txt, today.bin, salt) into stats.db, then kept in
// migrated-files/.
void StoreMigrateFiles(sqlite3* db, const std::string& dir)
{
  std::vector<std::string> files;
  if (DIR* folder = opendir(dir.c_str()))
  {
    while (const dirent* entry = readdir(folder))
    {
      const std::string n = entry->d_name;
      if (n == "salt" || n == "downloads.txt" || n == "today.bin" || n == "live.txt" || n == "rooms.txt" ||
          (n.rfind("day-", 0) == 0 && n.size() > 8 && n.compare(n.size() - 4, 4, ".txt") == 0))
        files.push_back(n);
    }
    closedir(folder);
  }
  std::sort(files.begin(), files.end());
  if (files.empty())
    return;
  StoreExec(db, "BEGIN");
  int days = 0;
  for (const std::string& name : files)
  {
    const std::string text = ReadWhole(dir + "/" + name);
    if (name == "salt")
    {
      uint8_t salt[32];
      if (FromHex(text, salt, 32) && StoreMeta(db, "sketch_salt").empty())
        StoreSetMeta(db, "sketch_salt", Hex(salt, 32));
    }
    else if (name == "downloads.txt")
    {
      std::istringstream in(text);
      std::string date, tag;
      uint64_t n = 0;
      Stmt put(db, "INSERT OR REPLACE INTO downloads VALUES(?, ?, ?)");
      while (in >> date >> tag >> n)
        put.Text(1, date).Text(2, tag).Int(3, static_cast<int64_t>(n)).Run();
    }
    else if (name == "today.bin")
    {
      std::istringstream in(text);
      std::string line, date, salt, consoles;
      if (std::getline(in, line) && line == "emusuni-today 1" && std::getline(in, date) && std::getline(in, salt) &&
          std::getline(in, consoles))
      {
        StoreSetMeta(db, "today", date);
        StoreSetMeta(db, "today_salt", salt);
        StoreSetMeta(db, "today_consoles", consoles);
        Stmt put(db, "INSERT OR REPLACE INTO seen VALUES(?, ?, ?, ?, ?, ?)");
        while (std::getline(in, line))
        {
          unsigned long long key = 0, bits = 0;
          unsigned states = 0, checks = 0;
          int updated = 0;
          char games[1024] = {};
          const int fields =
              std::sscanf(line.c_str(), "%llx %llx %x %x %d %1023s", &key, &bits, &states, &checks, &updated, games);
          if (fields >= 5)
            put.Int(1, static_cast<int64_t>(key)).Int(2, static_cast<int64_t>(bits)).Int(3, states).Int(4, checks)
                .Int(5, updated).Text(6, fields == 6 && games[0] != '-' ? games : "").Run();
        }
      }
    }
    else if (name.rfind("day-", 0) == 0)
    {
      Day d;
      if (ParseDay(text, &d))
      {
        StoreSaveDay(db, d);
        days++;
      }
    }
  }
  StoreExec(db, "COMMIT");
  mkdir((dir + "/migrated-files").c_str(), 0750);
  for (const std::string& name : files)
    std::rename((dir + "/" + name).c_str(), (dir + "/migrated-files/" + name).c_str());
  std::printf("stats: the text files before SQLite moved into stats.db (%d days; the files kept in %s/migrated-files)\n",
              days, dir.c_str());
}

bool StatsStart(const std::string& dir)
{
  s_stats.dir = dir;
  s_stats.db = StoreOpen(dir);
  sqlite3* db = s_stats.db;
  if (!db)
    return false;
  StoreMigrateFiles(db, dir);
  // The sketches' salt and the profiles' salt: made once, kept.
  for (auto [key, salt] : {std::pair<const char*, uint8_t*>{"sketch_salt", s_stats.sketch_salt},
                           std::pair<const char*, uint8_t*>{"profile_salt", s_stats.profile_salt}})
  {
    if (!FromHex(StoreMeta(db, key), salt, 32))
    {
      RandomBytes(salt, 32);
      StoreSetMeta(db, key, Hex(salt, 32));
    }
  }
  if (!FromHex(StoreMeta(db, "sketch_salt"), s_stats.sketch_salt, 32))
  {
    std::fprintf(stderr, "stats: %s/stats.db can't be written\n", dir.c_str());
    return false;
  }
  RandomBytes(s_stats.live_salt, 32);
  const std::string today = DateOf(WallNow());
  NewDay(today);
  // Today's counts and set from before a restart (00.95: the published versions' too).
  Day saved;
  if (StoreLoadDay(db, today, &saved))
    s_stats.day = saved;
  if (StoreLoadDay(db, today, &saved, "pub_"))
    s_stats.pub = saved;
  s_stats.published = PublishedOf(db);
  s_stats.last_published = Now();
  const auto load_seen = [db](const char* table, std::unordered_map<uint64_t, Seen>* seen) {
    Stmt q(db, (std::string("SELECT key, consoles, states, checks, updated, games FROM ") + table).c_str());
    while (q.Row())
    {
      Seen s;
      s.consoles = static_cast<uint64_t>(q.ColInt(1));
      s.states = static_cast<uint8_t>(q.ColInt(2));
      s.communities = static_cast<uint8_t>(q.ColInt(2) >> 8);  // (01.04)
      s.checks = static_cast<uint8_t>(q.ColInt(3));
      s.updated = q.ColInt(4) != 0;
      std::istringstream list(q.ColText(5));
      std::string one;
      while (std::getline(list, one, ',') && s.games.size() < GAMES_A_DAY)
        if (!one.empty())
          s.games.push_back(static_cast<uint32_t>(std::strtoul(one.c_str(), nullptr, 16)));
      (*seen)[static_cast<uint64_t>(q.ColInt(0))] = std::move(s);
    }
  };
  if (StoreMeta(db, "today") == today && FromHex(StoreMeta(db, "today_salt"), s_stats.day_salt, 32))
  {
    std::istringstream words(StoreMeta(db, "today_consoles"));
    std::string c;
    while (words >> c)
      s_stats.console_bits.push_back(c);
    load_seen("seen", &s_stats.seen);
    load_seen("pub_seen", &s_stats.pub_seen);
  }
  else
  {
    StoreExec(db, "DELETE FROM seen");  // (another day's: its salt goes)
    StoreExec(db, "DELETE FROM pub_seen");
    if (s_stats.day.active > 0)
      std::printf("stats: today's set wasn't saved: installs seen again today count again\n");
  }
  s_stats.on = true;
  SaveDay();
  CheckGeo(true);
  Stmt profiles(db, "SELECT COUNT(*), SUM(public) FROM profile");
  const bool counted = profiles.Row();
  std::printf("stats: on, in %s/stats.db (today: %llu active installs so far; %lld online profiles, %lld public)\n",
              dir.c_str(), static_cast<unsigned long long>(s_stats.day.active),
              static_cast<long long>(counted ? profiles.ColInt(0) : 0), static_cast<long long>(counted ? profiles.ColInt(1) : 0));
  std::string list;
  for (const std::string& v : s_stats.published)
    list += " " + v;
  std::printf("stats: the public page counts the versions published on GitHub only:%s\n",
              list.empty() ? " none known yet (record-downloads)" : list.c_str());
  return true;
}

uint32_t StatsOnline()
{
  return static_cast<uint32_t>(std::min<size_t>(s_stats.live.size(), UINT32_MAX));
}

// ---- The opt-in profiles: kept only for an install that asks (STATS_PROFILE), keyed by its id hashed with the
// profiles' salt; deleted when it asks (STATS_DELETE) or after PROFILE_KEPT_DAYS unseen. ----
struct ProfileReply
{
  ProfileState state = ProfileState::None;
  std::string code;
};

std::string ProfileId(const uint8_t id[INSTALL_ID_SIZE])
{
  Sha256 h;
  h.Add(s_stats.profile_salt, 32);
  h.Add(id, INSTALL_ID_SIZE);
  uint8_t out[32];
  h.Finish(out);
  return std::string(reinterpret_cast<const char*>(out), 16);
}

// A heartbeat of a profile's install: the profile made (its code) the first time, its nickname (public ones only),
// country and last time kept; a game's minutes (the time since its last heartbeat, 5 minutes at most), sessions (a
// game started, or after 15 minutes without one) and online matches added.
ProfileReply ProfileBeat(const Heartbeat& h, const std::string& pid, const std::string& country, StatsState state,
                         const std::string& console, const std::string& game, bool counted, AddressState& address)
{
  ProfileReply reply;
  sqlite3* db = s_stats.db;
  if (!db)
    return reply;
  const int64_t now = WallNow();
  std::string code, last_game;
  int64_t last_beat = 0, last_state = 0;
  {
    Stmt q(db, "SELECT code, last_beat, last_game, last_state FROM profile WHERE id = ?");
    q.Blob(1, pid.data(), static_cast<int>(pid.size()));
    if (q.Row())
    {
      code = q.ColText(0);
      last_beat = q.ColInt(1);
      last_game = q.ColText(2);
      last_state = q.ColInt(3);
    }
  }
  if (code.empty())
  {
    if (!address.profiles.Take(Now(), PROFILES_RATE, PROFILES_BURST))
      return reply;
    for (int tries = 0; tries < 5 && code.empty(); tries++)
    {
      const std::string fresh = NewProfileCode();
      Stmt add(db, "INSERT INTO profile(id, code, public, nick, country, created, last_seen, last_beat, last_game, "
                   "last_state, minutes, sessions, online_matches) VALUES(?, ?, 0, '', ?, ?, ?, 0, '', 0, 0, 0, 0)");
      add.Blob(1, pid.data(), static_cast<int>(pid.size())).Text(2, fresh).Text(3, country).Int(4, now).Int(5, now);
      if (add.Run())
        code = fresh;
    }
    if (code.empty())
      return reply;
  }
  const bool is_public = (h.flags & STATS_PUBLIC) != 0;
  StoreExec(db, "BEGIN");
  {
    // (00.95) Its version too: the public page shows the profiles of the published versions only.
    Stmt q(db, "UPDATE profile SET public = ?, nick = ?, country = ?, last_seen = ?, version = ? WHERE id = ?");
    q.Int(1, is_public ? 1 : 0).Text(2, is_public ? CleanNick(h.nick) : std::string()).Text(3, country).Int(4, now)
        .Text(5, Token(h.app_version, MAX_APP_VERSION)).Blob(6, pid.data(), static_cast<int>(pid.size())).Run();
  }
  const bool playing = state == StatsState::Playing || state == StatsState::PlayingOnline;
  if (counted && playing && !console.empty() && console != "?")
  {
    const int64_t since = now - last_beat;
    const int64_t minutes = last_beat == 0 || since >= 600 ? 5 : std::clamp<int64_t>((since + 30) / 60, 1, 5);
    const bool session = since >= 900 || game != last_game;
    const bool match = state == StatsState::PlayingOnline &&
                       (session || last_state != static_cast<int64_t>(StatsState::PlayingOnline));
    Stmt q(db, "UPDATE profile SET minutes = minutes + ?, sessions = sessions + ?, online_matches = online_matches + ?, "
               "last_beat = ?, last_game = ?, last_state = ? WHERE id = ?");
    q.Int(1, minutes).Int(2, session ? 1 : 0).Int(3, match ? 1 : 0).Int(4, now).Text(5, game)
        .Int(6, static_cast<int64_t>(state)).Blob(7, pid.data(), static_cast<int>(pid.size())).Run();
    Stmt c(db, "INSERT INTO profile_console VALUES(?, ?, ?) ON CONFLICT(profile, console) DO UPDATE SET "
               "minutes = minutes + excluded.minutes");
    c.Blob(1, pid.data(), static_cast<int>(pid.size())).Text(2, console).Int(3, minutes).Run();
    if (!game.empty())
    {
      Stmt g(db, "INSERT INTO profile_game VALUES(?, ?, ?, ?, ?, ?, ?) ON CONFLICT(profile, game) DO UPDATE SET "
                 "minutes = minutes + excluded.minutes, sessions = sessions + excluded.sessions, "
                 "online_matches = online_matches + excluded.online_matches, last_played = excluded.last_played");
      g.Blob(1, pid.data(), static_cast<int>(pid.size())).Text(2, game).Text(3, console).Int(4, minutes)
          .Int(5, session ? 1 : 0).Int(6, match ? 1 : 0).Int(7, now).Run();
    }
  }
  else if (counted)
  {
    Stmt q(db, "UPDATE profile SET last_beat = ?, last_game = '', last_state = ? WHERE id = ?");
    q.Int(1, now).Int(2, static_cast<int64_t>(state)).Blob(3, pid.data(), static_cast<int>(pid.size())).Run();
  }
  StoreExec(db, "COMMIT");
  reply.state = ProfileState::Kept;
  reply.code = code;
  return reply;
}

// (00.95) What a heartbeat says, as a day counts it.
struct Beat
{
  uint64_t sketch_key = 0, day_key = 0;
  std::string country, version, language, console, game, from;
  std::string community;  // (01.04) a COMMUNITY_MODES' one, else 
  StatsState state = StatsState::Menu;
  bool playing = false, fresh = false;
  int update_checks = 0;
};

// A heartbeat in a day's counts - every version's (the owner's) or (00.95) the published ones' (the public page's) -,
// each with its own set of today's installs (seen) and those changed since the last save (dirty). True: counted as a
// heartbeat (not too soon after that install's last one: its play time).
bool CountBeat(Day& day, std::unordered_map<uint64_t, Seen>& seen_map, std::unordered_set<uint64_t>& dirty,
               const Beat& b, double now)
{
  auto seen = seen_map.find(b.day_key);
  if (seen == seen_map.end())
  {
    seen = seen_map.emplace(b.day_key, Seen()).first;
    day.active++;
    day.all.Add(b.sketch_key);
    const auto count = [&](const char* kind, const std::string& value) {
      Label& l = LabelOf(day, kind, value);
      l.installs++;
      l.sketch.Add(b.sketch_key);
    };
    count("country", b.country);
    count("version", b.version);
    count("language", b.language);
    if (b.fresh)
      day.fresh++;
  }
  Seen& s = seen->second;
  const bool counted_beat = now - s.last_beat >= s_beat_min_gap;
  if (counted_beat)
  {
    s.last_beat = now;
    day.heartbeats++;
  }
  const uint8_t state_bit = static_cast<uint8_t>(1u << static_cast<unsigned>(b.state));
  Label& state_label = LabelOf(day, "state", StateName(b.state));
  if (!(s.states & state_bit))
  {
    s.states |= state_bit;
    state_label.installs++;
    state_label.sketch.Add(b.sketch_key);
  }
  if (counted_beat)
    state_label.beats++;
  if (b.playing)
  {
    size_t bit = std::find(s_stats.console_bits.begin(), s_stats.console_bits.end(), b.console) -
                 s_stats.console_bits.begin();
    if (bit == s_stats.console_bits.size() && s_stats.console_bits.size() < MAX_CONSOLE_BITS)
      s_stats.console_bits.push_back(b.console);
    Label& l = LabelOf(day, "console", bit < MAX_CONSOLE_BITS ? b.console : std::string("other"));
    if (bit < MAX_CONSOLE_BITS && !(s.consoles & (uint64_t{1} << bit)))
    {
      s.consoles |= uint64_t{1} << bit;
      l.installs++;
      l.sketch.Add(b.sketch_key);
    }
    if (counted_beat)
      l.beats++;
  }
  // (01.04) The community online mode connected while it played: its installs and hours.
  if (const int bit = CommunityBit(b.community); bit >= 0)
  {
    Label& l = LabelOf(day, "community", b.community);
    if (!(s.communities & (1u << bit)))
    {
      s.communities |= static_cast<uint8_t>(1u << bit);
      l.installs++;
      l.sketch.Add(b.sketch_key);
    }
    if (counted_beat)
      l.beats++;
  }
  // The game: its label "<console>.<kind>-<id>" ("snes.c-7655bb55", "arcade.s-mslug", "ps1.d-SCUS-94426").
  if (!b.game.empty())
  {
    Label& l = LabelOf(day, "game", b.game);
    const uint32_t hash = GameHash(b.game);
    if (std::find(s.games.begin(), s.games.end(), hash) == s.games.end() && s.games.size() < GAMES_A_DAY)
    {
      s.games.push_back(hash);
      l.installs++;
      l.sketch.Add(b.sketch_key);
    }
    if (counted_beat)
      l.beats++;
  }
  // Update checks (at most UPDATE_CHECKS_A_DAY an install) and an update installed (once a day). (The public day's:
  // from a published version only - no test version's name.)
  const int checks = std::min<int>(b.update_checks, UPDATE_CHECKS_A_DAY - s.checks);
  if (checks > 0)
  {
    s.checks = static_cast<uint8_t>(s.checks + checks);
    day.update_checks += static_cast<uint64_t>(checks);
  }
  if (!b.from.empty() && !s.updated && b.from != b.version && (&day != &s_stats.pub || Published(b.from)))
  {
    s.updated = true;
    if (day.updates.size() < MAX_LABELS || day.updates.count({b.from, b.version}))
      day.updates[{b.from, b.version}]++;
  }
  dirty.insert(b.day_key);
  return counted_beat;
}

// A heartbeat: counted (or not: the limits), its address used for its country only; its profile's, if it has one.
// (00.95) A published version's counted twice: in the day of every version and in the published versions' own.
ProfileReply StatsRecord(const Heartbeat& h, uint32_t ip, AddressState& address)
{
  ProfileReply reply;
  const std::string pid = ProfileId(h.id);
  if ((h.flags & STATS_DELETE) && s_stats.db)
  {
    StoreDeleteProfile(s_stats.db, pid.data(), static_cast<int>(pid.size()));
    reply.state = ProfileState::Deleted;
  }
  if (h.flags & STATS_NO_COUNT)
    return reply;
  CheckDay();
  const double now = Now();
  Day& day = s_stats.day;
  const std::string country = CountryOf(ip);
  const std::string version = Token(h.app_version, MAX_APP_VERSION).empty() ? "?" : Token(h.app_version, MAX_APP_VERSION);
  const std::string language = Token(h.language, MAX_LANGUAGE).empty() ? "?" : Token(h.language, MAX_LANGUAGE);
  const StatsState state = static_cast<uint8_t>(h.state) <= 3 ? h.state : StatsState::Menu;
  const bool playing = state == StatsState::Playing || state == StatsState::PlayingOnline;
  std::string console = playing ? Token(h.console, MAX_CONSOLE) : std::string();
  std::transform(console.begin(), console.end(), console.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (playing && console.empty())
    console = "?";
  const uint64_t sketch_key = SaltedKey(s_stats.sketch_salt, h.id);
  const uint64_t day_key = SaltedKey(s_stats.day_salt, h.id);
  const uint64_t live_key = SaltedKey(s_stats.live_salt, h.id);

  // Online now (in memory only).
  auto live = s_stats.live.find(live_key);
  if (live == s_stats.live.end() && s_stats.live.size() < MAX_LIVE)
    live = s_stats.live.emplace(live_key, LiveEntry()).first;
  if (live != s_stats.live.end())
  {
    live->second.last = now;
    live->second.state = state;
    live->second.console = console;
    live->second.country = country;
    live->second.version = version;
  }
  if (s_stats.live.size() > day.peak)
  {
    day.peak = s_stats.live.size();
    day.peak_at = TimeText(WallNow(), "%H:%M");
  }

  // The day's counts (an install not seen today: the limits first).
  if (s_stats.seen.find(day_key) == s_stats.seen.end() &&
      (s_stats.seen.size() >= MAX_SEEN || !address.new_ids.Take(now, NEW_IDS_RATE, NEW_IDS_BURST)))
  {
    day.uncounted++;
    return reply;
  }
  Beat b;
  b.sketch_key = sketch_key;
  b.day_key = day_key;
  b.country = country;
  b.version = version;
  b.language = language;
  b.console = console;
  b.state = state;
  b.playing = playing;
  b.fresh = (h.flags & STATS_NEW_INSTALL) != 0;
  b.update_checks = h.update_checks;
  b.from = Token(h.updated_from, MAX_APP_VERSION);
  b.game = playing ? GameLabel(console, h.game) : std::string();
  b.community = playing && CommunityBit(h.community) >= 0 ? h.community : std::string();  // (01.04)
  const bool counted_beat = CountBeat(day, s_stats.seen, s_stats.dirty, b, now);
  if (counted_beat)
    s_stats.beats_since_log++;
  if (Published(version))
    CountBeat(s_stats.pub, s_stats.pub_seen, s_stats.pub_dirty, b, now);  // (00.95)
  if ((h.flags & STATS_PROFILE) && !(h.flags & STATS_DELETE))
    reply = ProfileBeat(h, pid, country, state, console, b.game, counted_beat, address);
  return reply;
}

// (00.95) And the published versions' alone - "public N" and "pub-by kind value n": the public page's.
std::string LiveText(double now)
{
  std::map<std::string, std::map<std::string, uint64_t>> by, pub_by;
  uint64_t pub = 0;
  for (const auto& [key, e] : s_stats.live)
  {
    by["state"][StateName(e.state)]++;
    by["country"][e.country]++;
    by["version"][e.version]++;
    if (!e.console.empty())
      by["console"][e.console]++;
    if (!Published(e.version))
      continue;
    pub++;
    pub_by["state"][StateName(e.state)]++;
    pub_by["country"][e.country]++;
    pub_by["version"][e.version]++;
    if (!e.console.empty())
      pub_by["console"][e.console]++;
  }
  (void)now;
  std::string out = "emusuni-stats-live 1\ntime " + std::to_string(WallNow()) + "\nonline " +
                    std::to_string(s_stats.live.size()) + "\npublic " + std::to_string(pub) + "\n";
  for (const auto& [kind, values] : by)
  {
    for (const auto& [value, n] : values)
      out += "by " + kind + " " + value + " " + std::to_string(n) + "\n";
  }
  for (const auto& [kind, values] : pub_by)
  {
    for (const auto& [value, n] : values)
      out += "pub-by " + kind + " " + value + " " + std::to_string(n) + "\n";
  }
  return out;
}

// A time zone as the rooms carry it ("UTC+1", "UTC-3:30"), else "?".
std::string ZoneText(const std::string& zone)
{
  int hours = 0, minutes = 0;
  char sign = 0, tail = 0;
  const int n = std::sscanf(zone.c_str(), "UTC%c%d:%d%c", &sign, &hours, &minutes, &tail);
  if (zone == "UTC+0" || zone == "UTC-0")
    return "UTC+0";
  if ((n == 2 || n == 3) && (sign == '+' || sign == '-') && hours >= 0 && hours <= 14 && minutes >= 0 && minutes < 60)
    return zone.substr(0, 12);
  return "?";
}

// rooms.txt (written with live.txt): the public rooms waiting for players - their console, their game's CRC32 (a
// title only if the lists know it: never the room's own text), the host's time zone, players. No nickname, no code.
// The rooms as the public page shows them: each public one (waiting, or its match going on) - console, the game's
// CRC32, the host's time zone, players -, the private ones only counted. No nickname, no code.
// (00.95) Each public room's EmuSuNi version at its line's end, and the private ones counted by version too
// ("private-version <version> <open> <playing>"): the public page shows the published versions' only.
std::string RoomsText()
{
  std::string out = "emusuni-stats-rooms 2\n";
  int count = 0, private_open = 0, private_playing = 0;
  std::map<std::string, std::pair<int, int>> private_by;  // version -> open, playing
  for (const auto& [code, room] : s_rooms)
  {
    const std::string version = Token(room.app_version, MAX_APP_VERSION).empty() ? "?" :
                                                                                     Token(room.app_version, MAX_APP_VERSION);
    if (!room.is_public)
    {
      (room.locked ? private_playing : private_open)++;
      if (private_by.size() < MAX_LABELS || private_by.count(version))
        (room.locked ? private_by[version].second : private_by[version].first)++;
      continue;
    }
    if (count >= 200)
      continue;
    std::string zone = "?";
    for (const Slot& s : room.members)
    {
      if (s.used && s.info.id == 0)
        zone = ZoneText(s.info.region);
    }
    std::string console = room.game.console == TOGETHER ? "together" : Token(room.game.console, MAX_CONSOLE);
    if (console.empty())
      continue;
    char line[200];
    std::snprintf(line, sizeof(line), "room %d %s %08x %s %d %d %s\n", room.locked ? 1 : 0, console.c_str(),
                  room.game.crc, zone.c_str(), room.Count(), static_cast<int>(room.max_members), version.c_str());
    out += line;
    count++;
  }
  out += "private " + std::to_string(private_open) + " " + std::to_string(private_playing) + "\n";
  for (const auto& [version, n] : private_by)
    out += "private-version " + version + " " + std::to_string(n.first) + " " + std::to_string(n.second) + "\n";
  return out;
}

// Every second: the installs not heard for 6 minutes gone from online, the files written now and then, a new day.
void StatsTick(bool stopping)
{
  if (!s_stats.on)
    return;
  const double now = Now();
  CheckDay();
  if (now - s_stats.last_prune >= 10.0 || stopping)
  {
    s_stats.last_prune = now;
    uint64_t pub = 0;
    for (auto it = s_stats.live.begin(); it != s_stats.live.end();)
    {
      if (now - it->second.last > ONLINE_WINDOW)
      {
        it = s_stats.live.erase(it);
        continue;
      }
      pub += Published(it->second.version) ? 1 : 0;
      ++it;
    }
    // (00.95) The published versions' peak online (every 10 s).
    if (pub > s_stats.pub.peak)
    {
      s_stats.pub.peak = pub;
      s_stats.pub.peak_at = TimeText(WallNow(), "%H:%M");
    }
  }
  // (00.95) The published versions, read again every minute (record-downloads writes them once a day).
  if (now - s_stats.last_published >= 60.0 && s_stats.db)
  {
    s_stats.last_published = now;
    const std::set<std::string> list = PublishedOf(s_stats.db);
    if (list != s_stats.published)
    {
      std::string said;
      for (const std::string& v : list)
        said += " " + v;
      std::printf("stats: the published versions now:%s\n", said.empty() ? " none" : said.c_str());
      s_stats.published = list;
    }
  }
  if (now - s_stats.last_live >= s_live_every || stopping)
  {
    s_stats.last_live = now;
    if (stopping)
      s_stats.live.clear();  // (stopped: nobody is counted online)
    if (s_stats.db)
    {
      StoreExec(s_stats.db, "BEGIN");
      StoreSetMeta(s_stats.db, "live", LiveText(now));
      StoreSetMeta(s_stats.db, "rooms", stopping ? std::string("emusuni-stats-rooms 1\n") : RoomsText());
      StoreExec(s_stats.db, "COMMIT");
    }
  }
  if (now - s_stats.last_save >= s_save_every || stopping)
  {
    s_stats.last_save = now;
    SaveDay();
  }
  if (now - s_stats.last_geo >= 600.0)
  {
    s_stats.last_geo = now;
    CheckGeo(false);
  }
}

std::string StatsLogText()
{
  if (!s_stats.on)
    return "";
  std::string text = "; stats: " + std::to_string(s_stats.live.size()) + " online, " +
                     std::to_string(s_stats.day.active) + " active today, " +
                     std::to_string(s_stats.beats_since_log) + " heartbeats";
  s_stats.beats_since_log = 0;
  return text;
}

// ---- record-downloads: GitHub's releases (its API's JSON on stdin) -> downloads.txt ----
struct Json
{
  enum Kind
  {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
  } kind = Null;
  double number = 0.0;
  bool boolean = false;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;
  const Json* Get(const std::string& key) const
  {
    for (const auto& [k, v] : fields)
    {
      if (k == key)
        return &v;
    }
    return nullptr;
  }
};

class JsonReader
{
public:
  explicit JsonReader(const std::string& s) : m_s(s) {}
  bool More()
  {
    Space();
    return m_i < m_s.size();
  }
  bool Value(Json& out, int depth = 0)
  {
    Space();
    if (m_i >= m_s.size() || depth > 64)
      return false;
    const char c = m_s[m_i];
    if (c == '{')
    {
      out.kind = Json::Object;
      m_i++;
      Space();
      if (Peek('}'))
        return true;
      while (true)
      {
        Json key;
        Space();
        if (!String(key.text) || !Expect(':'))
          return false;
        Json value;
        if (!Value(value, depth + 1))
          return false;
        out.fields.emplace_back(key.text, std::move(value));
        Space();
        if (Peek('}'))
          return true;
        if (!Expect(','))
          return false;
      }
    }
    if (c == '[')
    {
      out.kind = Json::Array;
      m_i++;
      Space();
      if (Peek(']'))
        return true;
      while (true)
      {
        Json value;
        if (!Value(value, depth + 1))
          return false;
        out.items.push_back(std::move(value));
        Space();
        if (Peek(']'))
          return true;
        if (!Expect(','))
          return false;
      }
    }
    if (c == '"')
    {
      out.kind = Json::String;
      return String(out.text);
    }
    if (m_s.compare(m_i, 4, "true") == 0 || m_s.compare(m_i, 5, "false") == 0)
    {
      out.kind = Json::Bool;
      out.boolean = m_s[m_i] == 't';
      m_i += out.boolean ? 4 : 5;
      return true;
    }
    if (m_s.compare(m_i, 4, "null") == 0)
    {
      m_i += 4;
      return true;
    }
    const char* start = m_s.c_str() + m_i;
    char* end = nullptr;
    out.number = std::strtod(start, &end);
    if (end == start)
      return false;
    out.kind = Json::Number;
    m_i += static_cast<size_t>(end - start);
    return true;
  }

private:
  void Space()
  {
    while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i])))
      m_i++;
  }
  bool Peek(char c)
  {
    Space();
    if (m_i < m_s.size() && m_s[m_i] == c)
    {
      m_i++;
      return true;
    }
    return false;
  }
  bool Expect(char c) { return Peek(c); }
  bool String(std::string& out)
  {
    if (m_i >= m_s.size() || m_s[m_i] != '"')
      return false;
    m_i++;
    while (m_i < m_s.size())
    {
      const char c = m_s[m_i++];
      if (c == '"')
        return true;
      if (c != '\\')
      {
        out += c;
        continue;
      }
      if (m_i >= m_s.size())
        return false;
      const char e = m_s[m_i++];
      switch (e)
      {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        out += '\r';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'u':
      {
        if (m_i + 4 > m_s.size())
          return false;
        const unsigned code = static_cast<unsigned>(std::strtoul(m_s.substr(m_i, 4).c_str(), nullptr, 16));
        m_i += 4;
        if (code < 0x80)
          out += static_cast<char>(code);
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
        break;
      }
      default:
        out += e;  // " \ /
      }
    }
    return false;
  }
  const std::string& m_s;
  size_t m_i = 0;
};

bool EndsWithPkg(const std::string& name)
{
  if (name.size() < 4)
    return false;
  std::string tail = name.substr(name.size() - 4);
  std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return tail == ".pkg";
}

int RecordDownloads(const std::string& dir)
{
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  JsonReader reader(input);
  std::map<std::string, uint64_t> totals;  // tag -> the .pkg's downloads
  std::set<std::string> published;         // (00.95) the published releases' versions: "v00.22" -> "00.22"
  int releases = 0;
  while (reader.More())
  {
    Json page;
    if (!reader.Value(page))
    {
      std::fprintf(stderr, "record-downloads: GitHub's answer couldn't be read\n");
      return 1;
    }
    std::vector<const Json*> list;
    if (page.kind == Json::Array)
      for (const Json& r : page.items)
        list.push_back(&r);
    else if (page.kind == Json::Object)
      list.push_back(&page);
    for (const Json* r : list)
    {
      const Json* tag = r->Get("tag_name");
      const Json* draft = r->Get("draft");
      const Json* assets = r->Get("assets");
      if (!tag || tag->kind != Json::String || (draft && draft->boolean) || !assets || assets->kind != Json::Array)
        continue;
      const std::string name = Token(tag->text, 32);
      if (name.empty())
        continue;
      releases++;
      // (00.95) A published version: a release that isn't a test one (GitHub's "pre-release"), its tag "vXX.YY" -
      // the version its heartbeats say (the public page counts those only).
      const Json* test = r->Get("prerelease");
      if (!(test && test->boolean) && name.size() >= 2 && name[0] == 'v' &&
          std::all_of(name.begin() + 1, name.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; }))
        published.insert(name.substr(1));
      uint64_t sum = 0;
      for (const Json& a : assets->items)
      {
        const Json* file = a.Get("name");
        const Json* count = a.Get("download_count");
        if (file && file->kind == Json::String && EndsWithPkg(file->text) && count && count->kind == Json::Number)
          sum += static_cast<uint64_t>(std::max(0.0, count->number));
      }
      totals[name] += sum;
    }
  }
  if (releases == 0)
  {
    std::fprintf(stderr, "record-downloads: no release in GitHub's answer (nothing written)\n");
    return 1;
  }
  mkdir(dir.c_str(), 0750);
  const std::string today = DateOf(WallNow());
  sqlite3* db = StoreOpen(dir);
  if (!db)
    return 1;
  StoreExec(db, "BEGIN");
  {
    Stmt clear(db, "DELETE FROM downloads WHERE date = ?");
    clear.Text(1, today).Run();
  }
  uint64_t all = 0;
  {
    Stmt put(db, "INSERT OR REPLACE INTO downloads VALUES(?, ?, ?)");
    for (const auto& [tag, n] : totals)
    {
      put.Text(1, today).Text(2, tag).Int(3, static_cast<int64_t>(n)).Run();
      all += n;
    }
  }
  // (00.95) The published versions (meta "published", which the server reads every minute): only from an answer
  // that has them - otherwise the last good list stays, never every version.
  std::string list;
  for (const std::string& v : published)
    list += (list.empty() ? "" : " ") + v;
  if (!list.empty())
  {
    StoreSetMeta(db, "published", list);
    StoreSetMeta(db, "published_date", today);
  }
  StoreExec(db, "COMMIT");
  sqlite3_close(db);
  std::printf("record-downloads: %d releases, %llu downloads of their .pkg in all (%s); published: %s\n", releases,
              static_cast<unsigned long long>(all), today.c_str(),
              list.empty() ? "none in this answer (the last list kept)" : list.c_str());
  return 0;
}

// ---- report ----
std::unordered_map<std::string, std::string> LoadTitles(const std::string& path);  // (below)
std::string JsonText(const std::string& s)
{
  std::string out = "\"";
  for (const char c : s)
  {
    if (c == '"' || c == '\\')
      out += '\\';
    if (static_cast<unsigned char>(c) < 0x20)
      continue;
    out += c;
  }
  return out + "\"";
}

uint64_t Round(double v)
{
  return static_cast<uint64_t>(std::llround(std::max(0.0, v)));
}

int StatsReport(const std::string& dir, int days, bool json, const std::string& titles_path)
{
  sqlite3* db = StoreOpen(dir);
  if (!db)
    return 1;
  struct Closer
  {
    sqlite3* db;
    ~Closer() { sqlite3_close(db); }
  } closer{db};

  days = std::clamp(days, 1, 3660);
  const int64_t now = WallNow();
  // The days asked for, the newest first, and 30 days at least for the weekly and monthly uniques.
  const int span = std::max(days, 30);
  std::vector<Day> list;  // index i: i days ago (date "" when there is no file)
  for (int i = 0; i < span; i++)
  {
    Day d;
    const std::string date = DateOf(now - static_cast<int64_t>(i) * 86400);
    if (!StoreLoadDay(db, date, &d))
      d.date = date;
    list.push_back(std::move(d));
  }
  auto unique = [&](int count) {
    Hll h(SKETCH_ALL);
    for (int i = 0; i < count && i < static_cast<int>(list.size()); i++)
      h.Merge(list[static_cast<size_t>(i)].all);
    return Round(h.Estimate());
  };
  // The period's labels: unique installs (the sketches merged), heartbeats added up.
  struct Sum
  {
    Hll sketch;
    uint64_t beats = 0, exact = 0;
    explicit Sum(int p = SKETCH_LABEL) : sketch(p) {}
  };
  std::map<std::string, std::map<std::string, Sum>> labels;
  std::map<std::pair<std::string, std::string>, uint64_t> updates;
  uint64_t fresh = 0, checks = 0, beats = 0, uncounted = 0, updated = 0;
  for (int i = 0; i < days; i++)
  {
    const Day& d = list[static_cast<size_t>(i)];
    fresh += d.fresh;
    checks += d.update_checks;
    beats += d.heartbeats;
    uncounted += d.uncounted;
    for (const auto& [kind, values] : d.labels)
    {
      for (const auto& [value, l] : values)
      {
        Sum& s = labels[kind].try_emplace(value, SketchOf(kind)).first->second;
        s.sketch.Merge(l.sketch);
        s.beats += l.beats;
        s.exact += l.installs;
      }
    }
    for (const auto& [pair, n] : d.updates)
    {
      updates[pair] += n;
      updated += n;
    }
  }
  // Online now: live.txt (written every minute by the running server).
  std::map<std::string, std::vector<std::pair<std::string, uint64_t>>> live_by;
  int64_t live_time = 0;
  uint64_t online = 0;
  {
    std::istringstream in(StoreMeta(db, "live"));
    std::string line;
    while (std::getline(in, line))
    {
      std::istringstream words(line);
      std::string key, kind, value;
      uint64_t n = 0;
      words >> key;
      if (key == "time")
        words >> live_time;
      else if (key == "online")
        words >> online;
      else if (key == "by" && (words >> kind >> value >> n))
        live_by[kind].push_back({value, n});
    }
  }
  const bool live_fresh = live_time > 0 && now - live_time <= 180;
  // GitHub's downloads: the newest day's totals, and how many more than `days` days before.
  std::map<std::string, std::map<std::string, uint64_t>> downloads;  // date -> tag -> total
  {
    std::istringstream in(StoreDownloadsText(db));
    std::string date, tag;
    uint64_t n = 0;
    while (in >> date >> tag >> n)
      downloads[date][tag] = n;
  }
  std::string downloads_date;
  std::vector<std::tuple<std::string, uint64_t, int64_t>> by_tag;  // tag, total, more in the period (-1: unknown)
  if (!downloads.empty())
  {
    downloads_date = downloads.rbegin()->first;
    const std::string before = DateOf(now - static_cast<int64_t>(days) * 86400);
    const std::map<std::string, uint64_t>* base = nullptr;
    for (const auto& [date, tags] : downloads)
    {
      if (date <= before)
        base = &tags;
    }
    for (const auto& [tag, n] : downloads.rbegin()->second)
    {
      int64_t more = -1;
      if (base)
      {
        const auto at = base->find(tag);
        more = static_cast<int64_t>(n) - static_cast<int64_t>(at == base->end() ? 0 : at->second);
      }
      by_tag.emplace_back(tag, n, more);
    }
    std::sort(by_tag.begin(), by_tag.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
  }
  auto sorted = [&](const std::string& kind) {
    std::vector<std::tuple<std::string, uint64_t, uint64_t>> out;  // value, installs, beats
    const auto at = labels.find(kind);
    if (at == labels.end())
      return out;
    for (const auto& [value, s] : at->second)
      out.emplace_back(value, days == 1 ? s.exact : Round(s.sketch.Estimate()), s.beats);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
      return std::get<1>(a) != std::get<1>(b) ? std::get<1>(a) > std::get<1>(b) : std::get<0>(a) < std::get<0>(b);
    });
    return out;
  };
  const uint64_t period = days == 1 ? list[0].active : unique(days);
  const char* attribution = "IP Geolocation by DB-IP (https://db-ip.com)";
  const auto game_titles = LoadTitles(titles_path);
  if (json)
  {
    std::string o = "{\n";
    o += "  \"generated\": " + JsonText(TimeText(now, "%Y-%m-%dT%H:%M:%SZ")) + ",\n";
    o += "  \"days\": " + std::to_string(days) + ",\n";
    o += "  \"live\": {\"time\": " + JsonText(live_time ? TimeText(live_time, "%Y-%m-%dT%H:%M:%SZ") : "") +
         ", \"current\": " + (live_fresh ? "true" : "false") + ", \"online_now\": " + std::to_string(online);
    for (const auto& [kind, values] : live_by)
    {
      o += ", \"by_" + kind + "\": {";
      for (size_t i = 0; i < values.size(); i++)
        o += (i ? ", " : "") + JsonText(values[i].first) + ": " + std::to_string(values[i].second);
      o += "}";
    }
    o += "},\n";
    o += "  \"active\": {\"today\": " + std::to_string(list[0].active) + ", \"yesterday\": " +
         std::to_string(list[1].active) + ", \"last_7_days\": " + std::to_string(unique(7)) + ", \"last_30_days\": " +
         std::to_string(unique(30)) + ", \"period\": " + std::to_string(period) + "},\n";
    o += "  \"new_installs\": " + std::to_string(fresh) + ",\n";
    o += "  \"heartbeats\": " + std::to_string(beats) + ",\n";
    o += "  \"uncounted_heartbeats\": " + std::to_string(uncounted) + ",\n";
    o += "  \"update_checks\": " + std::to_string(checks) + ",\n";
    o += "  \"updates_installed\": " + std::to_string(updated) + ",\n";
    o += "  \"updates\": [";
    bool first = true;
    for (const auto& [pair, n] : updates)
    {
      o += std::string(first ? "" : ", ") + "{\"from\": " + JsonText(pair.first) + ", \"to\": " + JsonText(pair.second) +
           ", \"installs\": " + std::to_string(n) + "}";
      first = false;
    }
    o += "],\n";
    for (const char* kind : LABEL_KINDS)
    {
      o += "  \"by_" + std::string(kind) + "\": [";
      const auto rows = sorted(kind);
      for (size_t i = 0; i < rows.size(); i++)
      {
        o += (i ? ", " : "") + std::string("{\"") + kind + "\": " + JsonText(std::get<0>(rows[i])) +
             ", \"installs\": " + std::to_string(std::get<1>(rows[i]));
        if (std::string(kind) == "console" || std::string(kind) == "state" || std::string(kind) == "game" ||
            std::string(kind) == "community")
        {
          char hours[32];
          std::snprintf(hours, sizeof(hours), "%.1f", static_cast<double>(std::get<2>(rows[i])) * 5.0 / 60.0);
          o += std::string(", \"hours\": ") + hours;
        }
        if (std::string(kind) == "game")
        {
          const auto title = game_titles.find(std::get<0>(rows[i]));
          o += ", \"title\": " + (title == game_titles.end() ? std::string("null") : JsonText(title->second));
        }
        o += "}";
      }
      o += "],\n";
    }
    o += "  \"daily\": [";
    for (int i = days - 1; i >= 0; i--)
    {
      const Day& d = list[static_cast<size_t>(i)];
      uint64_t up = 0;
      for (const auto& [pair, n] : d.updates)
        up += n;
      o += std::string(i == days - 1 ? "" : ", ") + "{\"date\": " + JsonText(d.date) + ", \"active\": " +
           std::to_string(d.active) + ", \"new\": " + std::to_string(d.fresh) + ", \"peak_online\": " +
           std::to_string(d.peak) + ", \"update_checks\": " + std::to_string(d.update_checks) +
           ", \"updates_installed\": " + std::to_string(up) + "}";
    }
    o += "],\n";
    o += "  \"downloads\": {\"date\": " + JsonText(downloads_date) + ", \"by_version\": [";
    for (size_t i = 0; i < by_tag.size(); i++)
    {
      o += (i ? ", " : "") + std::string("{\"tag\": ") + JsonText(std::get<0>(by_tag[i])) + ", \"total\": " +
           std::to_string(std::get<1>(by_tag[i]));
      if (std::get<2>(by_tag[i]) >= 0)
        o += ", \"period\": " + std::to_string(std::get<2>(by_tag[i]));
      o += "}";
    }
    o += "]},\n";
    o += "  \"attribution\": " + JsonText(attribution) + "\n}\n";
    std::fputs(o.c_str(), stdout);
    return 0;
  }
  std::printf("EmuSuNi usage, the last %d day%s (UTC; unique installs over several days are estimates)\n", days,
              days == 1 ? "" : "s");
  std::printf("Countries: %s\n\n", attribution);
  if (live_time == 0)
    std::printf("Online now: unknown (no live.txt: is the server running with --stats-dir?)\n");
  else
    std::printf("Online now: %llu (%s UTC%s)\n", static_cast<unsigned long long>(online),
                TimeText(live_time, "%Y-%m-%d %H:%M").c_str(), live_fresh ? "" : " - old: the server isn't running?");
  for (const auto& [kind, values] : live_by)
  {
    std::printf("  %-8s", kind.c_str());
    for (const auto& [value, n] : values)
      std::printf(" %s %llu", value.c_str(), static_cast<unsigned long long>(n));
    std::printf("\n");
  }
  std::printf("\nActive installs: today %llu, yesterday %llu, last 7 days %llu, last 30 days %llu, these %d days %llu\n",
              static_cast<unsigned long long>(list[0].active), static_cast<unsigned long long>(list[1].active),
              static_cast<unsigned long long>(unique(7)), static_cast<unsigned long long>(unique(30)), days,
              static_cast<unsigned long long>(period));
  std::printf("New installs: %llu   Heartbeats: %llu (%llu not counted: limits)\n", static_cast<unsigned long long>(fresh),
              static_cast<unsigned long long>(beats), static_cast<unsigned long long>(uncounted));
  std::printf("Update checks: %llu   Updates installed: %llu\n", static_cast<unsigned long long>(checks),
              static_cast<unsigned long long>(updated));
  for (const auto& [pair, n] : updates)
    std::printf("  %s -> %s: %llu\n", pair.first.c_str(), pair.second.c_str(), static_cast<unsigned long long>(n));
  const char* titles[] = {"Countries", "Versions", "PS4 languages", "States (installs, hours)",
                          "Consoles played (installs, hours)", "Games played (installs, hours; the top 40)",
                          "Community online modes (installs, hours)"};
  for (size_t k = 0; k < 7; k++)
  {
    auto rows = sorted(LABEL_KINDS[k]);
    if (k == 5)
    {
      std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return std::get<2>(a) > std::get<2>(b); });
      if (rows.size() > 40)
        rows.resize(40);
    }
    std::printf("\n%s:\n", titles[k]);
    if (rows.empty())
      std::printf("  -\n");
    for (const auto& [value, installs, b] : rows)
    {
      if (k == 5)
      {
        const auto title = game_titles.find(value);
        std::printf("  %-26s %8llu  %8.1f h  %s\n", value.c_str(), static_cast<unsigned long long>(installs),
                    static_cast<double>(b) * 5.0 / 60.0, title == game_titles.end() ? "(no title)" : title->second.c_str());
      }
      else if (k >= 3)
        std::printf("  %-14s %8llu  %8.1f h\n", value.c_str(), static_cast<unsigned long long>(installs),
                    static_cast<double>(b) * 5.0 / 60.0);
      else
        std::printf("  %-14s %8llu\n", value.c_str(), static_cast<unsigned long long>(installs));
    }
  }
  std::printf("\nDay         active   new  peak online  update checks  updates\n");
  for (int i = days - 1; i >= 0; i--)
  {
    const Day& d = list[static_cast<size_t>(i)];
    uint64_t up = 0;
    for (const auto& [pair, n] : d.updates)
      up += n;
    std::printf("%s %7llu %5llu %12llu %14llu %8llu\n", d.date.c_str(), static_cast<unsigned long long>(d.active),
                static_cast<unsigned long long>(d.fresh), static_cast<unsigned long long>(d.peak),
                static_cast<unsigned long long>(d.update_checks), static_cast<unsigned long long>(up));
  }
  std::printf("\nGitHub downloads of the .pkg%s:\n",
              downloads_date.empty() ? " (none recorded yet: record-downloads)" : (" (" + downloads_date + ")").c_str());
  for (const auto& [tag, n, more] : by_tag)
  {
    if (more >= 0)
      std::printf("  %-12s %8llu  (+%lld in these %d days)\n", tag.c_str(), static_cast<unsigned long long>(n),
                  static_cast<long long>(more), days);
    else
      std::printf("  %-12s %8llu\n", tag.c_str(), static_cast<unsigned long long>(n));
  }
  return 0;
}

std::string Lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// ---- Game titles: titles.txt ("<label>\t<title>" a line), made by build-titles from the lists of libretro-database
// (No-Intro's cartridges, Redump's discs; CC BY-SA 4.0) and FinalBurn Neo's sets (scripts/server/update-titles.sh).
// A heartbeat's game is a checksum or a code: its title comes only from these lists, never from an app. ----
std::unordered_map<std::string, std::string> LoadTitles(const std::string& path)
{
  std::unordered_map<std::string, std::string> titles;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
  {
    const size_t tab = line.find('\t');
    if (tab != std::string::npos && tab > 0 && tab + 1 < line.size())
      titles.emplace(line.substr(0, tab), line.substr(tab + 1));
  }
  return titles;
}

// A list's name as a title: its tags ("(USA)", "[b]") left out, "Legend of Zelda, The - ..." as "The Legend of
// Zelda - ...", no control characters or angle brackets, 80 bytes at most.
std::string CleanTitle(const std::string& name)
{
  std::string t = name.substr(0, std::min(name.find(" ("), name.find(" [")));
  for (const char* article : {", The", ", An", ", A"})
  {
    const size_t at = t.find(article);
    const size_t len = std::strlen(article);
    if (at != std::string::npos && (at + len == t.size() || t.compare(at + len, 3, " - ") == 0))
    {
      t = std::string(article + 2) + " " + t.substr(0, at) + t.substr(at + len);
      break;
    }
  }
  std::string out;
  for (const char c : t)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u != 0x7F && c != '<' && c != '>')
      out += c;
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  if (out.size() > 80)
  {
    size_t n = 80;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80)
      n--;
    out.resize(n);
  }
  return out;
}

// clrmamepro's DAT: words, quoted strings, and parentheses.
struct DatTokens
{
  const std::string& s;
  size_t i = 0;
  bool Next(std::string* out)
  {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      i++;
    if (i >= s.size())
      return false;
    out->clear();
    if (s[i] == '(' || s[i] == ')')
    {
      *out = s[i++];
      return true;
    }
    if (s[i] == '"')
    {
      i++;
      while (i < s.size() && s[i] != '"')
        *out += s[i++];
      i++;
      return true;
    }
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' && s[i] != ')')
      *out += s[i++];
    return true;
  }
};

// build-titles OUT console:kind:DAT...: kind c (a cartridge's CRC32), d (a disc's serial), s (an arcade set: its
// .zip's name, and that .zip's CRC32 for the rooms).
int BuildTitles(const std::string& out_path, const std::vector<std::string>& specs)
{
  std::map<std::string, std::string> titles;
  for (const std::string& spec : specs)
  {
    const size_t a = spec.find(':');
    const size_t b = a == std::string::npos ? a : spec.find(':', a + 1);
    if (b == std::string::npos)
    {
      std::fprintf(stderr, "build-titles: %s isn't console:kind:file\n", spec.c_str());
      return 2;
    }
    const std::string console = spec.substr(0, a), kind = spec.substr(a + 1, b - a - 1), path = spec.substr(b + 1);
    const std::string text = ReadWhole(path);
    if (text.empty())
    {
      std::fprintf(stderr, "build-titles: %s can't be read\n", path.c_str());
      continue;
    }
    DatTokens t{text};
    std::string word;
    const size_t before = titles.size();
    // game ( name "..." serial "..." rom ( name "..." crc XXXXXXXX serial "..." ) ... )
    while (t.Next(&word))
    {
      if (word != "game" && word != "machine")
        continue;
      if (!t.Next(&word) || word != "(")
        continue;
      std::string name, key;
      std::vector<std::string> crcs, serials, sets;
      int depth = 1;
      while (depth > 0 && t.Next(&word))
      {
        if (word == "(")
        {
          depth++;
          key.clear();
          continue;
        }
        if (word == ")")
        {
          depth--;
          key.clear();
          continue;
        }
        if (key.empty())
        {
          key = word;
          continue;
        }
        if (key == "name" && depth == 1)
          name = word;
        else if (key == "name" && depth == 2 && word.size() > 4 && Lower(word.substr(word.size() - 4)) == ".zip")
          sets.push_back(word.substr(0, word.size() - 4));
        else if (key == "crc" && depth == 2)
          crcs.push_back(word);
        else if (key == "serial")
          serials.push_back(word);
        key.clear();
      }
      const std::string title = CleanTitle(name);
      if (title.empty())
        continue;
      if (kind == "c")
      {
        if (crcs.size() == 1)  // (a cartridge: one file)
          titles.emplace(GameLabel(console, "c:" + Lower(Token(crcs[0], 8))), title);
      }
      else if (kind == "d")
      {
        for (const std::string& list : serials)
        {
          std::string one;
          for (const char c : list + ",")
          {
            if (c == ',' || c == ' ' || c == '/')
            {
              const std::string label = GameLabel(console, "d:" + one);
              if (!label.empty())
                titles.emplace(label, title);
              one.clear();
            }
            else
              one += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          }
        }
      }
      else if (kind == "s")
      {
        for (const std::string& set : sets)
          titles.emplace(GameLabel(console, "s:" + Lower(set)), title);
        if (crcs.size() == 1)
          titles.emplace(GameLabel(console, "c:" + Lower(Token(crcs[0], 8))), title);
      }
    }
    titles.erase("");
    std::printf("build-titles: %s (%s, %s): %zu titles\n", path.c_str(), console.c_str(), kind.c_str(),
                titles.size() - before);
  }
  std::string out;
  for (const auto& [label, title] : titles)
    out += label + "\t" + title + "\n";
  if (titles.empty() || !WriteWhole(out_path, out))
  {
    std::fprintf(stderr, "build-titles: nothing written to %s\n", out_path.c_str());
    return 1;
  }
  std::printf("build-titles: %zu titles in %s\n", titles.size(), out_path.c_str());
  return 0;
}

// ---- public-json: the public page's numbers - aggregates only: no id, address, nickname, room code or file name;
// a game's title only from the lists and only once PUBLIC_GAME_INSTALLS installs played it (publish-stats.sh). ----
std::string Hours(uint64_t beats)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(beats) * 5.0 / 60.0);
  return text;
}

int PublicJson(const std::string& dir, const std::string& titles_path)
{
  sqlite3* db = StoreOpen(dir);
  if (!db)
    return 1;
  struct Closer
  {
    sqlite3* db;
    ~Closer() { sqlite3_close(db); }
  } closer{db};

  const int64_t now = WallNow();
  const auto titles = LoadTitles(titles_path);
  // (00.95) The published versions' counts only (pub_ tables, live's "public", their rooms): no test version's
  // players, countries, games or rooms on the public page.
  const std::set<std::string> published = PublishedOf(db);
  std::vector<Day> list;  // index i: i days ago
  for (int i = 0; i < 90; i++)
  {
    Day d;
    const std::string date = DateOf(now - static_cast<int64_t>(i) * 86400);
    if (!StoreLoadDay(db, date, &d, "pub_"))
      d.date = date;
    list.push_back(std::move(d));
  }
  auto unique = [&](int days) {
    Hll h(SKETCH_ALL);
    for (int i = 0; i < days; i++)
      h.Merge(list[static_cast<size_t>(i)].all);
    return Round(h.Estimate());
  };
  // A kind's labels over `days`: installs (the sketches merged), heartbeats added up.
  struct Sum
  {
    Hll sketch;
    uint64_t beats = 0;
    explicit Sum(int p) : sketch(p) {}
  };
  auto sums = [&](const std::string& kind, int days) {
    std::map<std::string, Sum> out;
    for (int i = 0; i < days; i++)
    {
      const auto at = list[static_cast<size_t>(i)].labels.find(kind);
      if (at == list[static_cast<size_t>(i)].labels.end())
        continue;
      for (const auto& [value, l] : at->second)
      {
        Sum& s = out.try_emplace(value, SketchOf(kind)).first->second;
        s.sketch.Merge(l.sketch);
        s.beats += l.beats;
      }
    }
    return out;
  };
  auto rows = [&](const std::string& kind, const char* field, bool hours, size_t most) {
    std::vector<std::tuple<uint64_t, std::string, uint64_t>> v;  // installs, value, beats
    for (const auto& [value, s] : sums(kind, 30))
    {
      if (value != "other" && value != "?" && value != "??")
        v.emplace_back(Round(s.sketch.Estimate()), value, s.beats);
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
      return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) > std::get<0>(b) : std::get<1>(a) < std::get<1>(b);
    });
    std::string o = "[";
    for (size_t i = 0; i < v.size() && i < most; i++)
    {
      o += std::string(i ? ", " : "") + "{\"" + field + "\": " + JsonText(std::get<1>(v[i])) +
           ", \"installs\": " + std::to_string(std::get<0>(v[i]));
      if (hours)
        o += ", \"hours\": " + Hours(std::get<2>(v[i]));
      o += "}";
    }
    return o + "]";
  };
  // Online now (live.txt, written every minute by the server: older than 10 minutes, it isn't running).
  int64_t live_time = 0;
  uint64_t online = 0;
  std::map<std::string, std::map<std::string, uint64_t>> live_by;
  {
    std::istringstream in(StoreMeta(db, "live"));
    std::string line;
    while (std::getline(in, line))
    {
      std::istringstream words(line);
      std::string key, kind, value;
      uint64_t n = 0;
      words >> key;
      if (key == "time")
        words >> live_time;
      else if (key == "public")
        words >> online;  // (00.95: the published versions' - "online" is every version's)
      else if (key == "pub-by" && (words >> kind >> value >> n))
        live_by[kind][value] = n;
    }
  }
  const bool live_ok = live_time > 0 && now - live_time <= 600;
  auto object = [](const std::map<std::string, uint64_t>& m) {
    std::string o = "{";
    bool first = true;
    for (const auto& [k, n] : m)
    {
      if (k == "?" || k == "??")
        continue;
      o += std::string(first ? "" : ", ") + JsonText(k) + ": " + std::to_string(n);
      first = false;
    }
    return o + "}";
  };
  // Top games over `days`: by hours played, each title once (its versions together), 3 installs or more.
  struct GameSum
  {
    std::string console, title;
    Hll sketch{SKETCH_GAME};
    uint64_t beats = 0;
  };
  auto top_games = [&](int days) {
    std::map<std::string, GameSum> games;  // console + title
    for (const auto& [label, s] : sums("game", days))
    {
      const auto title = titles.find(label);
      if (title == titles.end())
        continue;
      const std::string console = label.substr(0, label.find('.'));
      GameSum& g = games[console + "\t" + title->second];
      g.console = console;
      g.title = title->second;
      g.sketch.Merge(s.sketch);
      g.beats += s.beats;
    }
    std::vector<GameSum> top;
    for (const auto& [key, g] : games)
    {
      if (Round(g.sketch.Estimate()) >= static_cast<uint64_t>(PUBLIC_GAME_INSTALLS))
        top.push_back(g);
    }
    std::sort(top.begin(), top.end(), [](const GameSum& a, const GameSum& b) {
      return a.beats != b.beats ? a.beats > b.beats : a.title < b.title;
    });
    std::string o = "[";
    for (size_t i = 0; i < top.size() && i < 25; i++)
      o += std::string(i ? ", " : "") + "{\"console\": " + JsonText(top[i].console) + ", \"title\": " +
           JsonText(top[i].title) + ", \"installs\": " + std::to_string(Round(top[i].sketch.Estimate())) +
           ", \"hours\": " + Hours(top[i].beats) + "}";
    return o + "]";
  };
  // GitHub's downloads: each day's totals.
  std::map<std::string, std::map<std::string, uint64_t>> downloads;  // date -> tag -> total
  {
    std::istringstream in(StoreDownloadsText(db));
    std::string date, tag;
    uint64_t n = 0;
    while (in >> date >> tag >> n)
      downloads[date][tag] = n;
  }
  // The rooms (the server's snapshot): the public ones, the game's title only when the lists know its CRC32; the
  // private ones counted.
  std::string rooms = "[";
  int private_open = 0, private_playing = 0;
  {
    std::istringstream in(StoreMeta(db, "rooms"));
    std::string line;
    int n = 0;
    while (live_ok && std::getline(in, line) && n < 100)
    {
      std::istringstream words(line);
      std::string key, console, crc, zone, version;
      int started = 0, players = 0, most = 0;
      words >> key;
      // (00.95) The published versions' rooms only: the private ones counted by version, a public one's version at
      // its line's end (a line without one: a server of before, not shown).
      if (key == "private-version")
      {
        int open = 0, playing = 0;
        if ((words >> version >> open >> playing) && published.count(version))
        {
          private_open += open;
          private_playing += playing;
        }
        continue;
      }
      if (!(words >> started >> console >> crc >> zone >> players >> most >> version) || key != "room" ||
          !published.count(version))
        continue;
      const bool together = console == "together";
      const auto title = together ? titles.end() : titles.find(GameLabel(Token(console, MAX_CONSOLE), "c:" + Lower(crc)));
      const std::string region = ZoneText(zone);
      rooms += std::string(n++ ? ", " : "") + "{\"console\": " + JsonText(together ? "" : Token(console, MAX_CONSOLE)) +
               ", \"together\": " + (together ? "true" : "false") + ", \"game\": " +
               (title == titles.end() ? std::string("null") : JsonText(title->second)) + ", \"region\": " +
               JsonText(region == "?" ? "" : region) + ", \"players\": " + std::to_string(std::clamp(players, 0, 4)) +
               ", \"max\": " + std::to_string(std::clamp(most, 0, 4)) + ", \"playing\": " +
               (started ? "true" : "false") + "}";
    }
  }
  rooms += "]";
  std::string o = "{\n";
  o += "  \"generated\": " + JsonText(TimeText(now, "%Y-%m-%dT%H:%M:%SZ")) + ",\n";
  // (00.95) The versions counted: those published on GitHub.
  o += "  \"published_versions\": [";
  bool first_version = true;
  for (const std::string& v : published)
  {
    o += std::string(first_version ? "" : ", ") + JsonText(v);
    first_version = false;
  }
  o += "],\n";
  o += "  \"online\": {\"now\": " + std::to_string(live_ok ? online : 0) + ", \"current\": " +
       (live_ok ? "true" : "false") + ", \"by_console\": " +
       object(live_ok ? live_by["console"] : std::map<std::string, uint64_t>()) + ", \"by_country\": " +
       object(live_ok ? live_by["country"] : std::map<std::string, uint64_t>()) + "},\n";
  o += "  \"active\": {\"today\": " + std::to_string(list[0].active) + ", \"last_7_days\": " + std::to_string(unique(7)) +
       ", \"last_30_days\": " + std::to_string(unique(30)) + "},\n";
  o += "  \"countries_30d\": " + rows("country", "country", false, 250) + ",\n";
  o += "  \"versions_30d\": " + rows("version", "version", false, 30) + ",\n";
  o += "  \"consoles_30d\": " + rows("console", "console", true, 30) + ",\n";
  o += "  \"top_games_7d\": " + top_games(7) + ",\n";
  o += "  \"top_games_30d\": " + top_games(30) + ",\n";
  // The days (the last 90, the oldest first): active installs, new ones, the peak online.
  uint64_t fresh_30 = 0;
  for (int i = 0; i < 30; i++)
    fresh_30 += list[static_cast<size_t>(i)].fresh;
  o += "  \"new_installs_30d\": " + std::to_string(fresh_30) + ",\n";
  o += "  \"daily\": [";
  bool first_day = true;
  for (int i = 89; i >= 0; i--)
  {
    const Day& d = list[static_cast<size_t>(i)];
    if (first_day && d.active == 0)
      continue;  // (the days before the first one with installs)
    o += std::string(first_day ? "" : ", ") + "{\"date\": " + JsonText(d.date) + ", \"active\": " +
         std::to_string(d.active) + ", \"new\": " + std::to_string(d.fresh) + ", \"peak\": " + std::to_string(d.peak) +
         "}";
    first_day = false;
  }
  o += "],\n";
  uint64_t total = 0;
  if (!downloads.empty())
  {
    for (const auto& [tag, n] : downloads.rbegin()->second)
      total += n;
  }
  o += "  \"downloads\": {\"total\": " + std::to_string(total) + ", \"date\": " +
       JsonText(downloads.empty() ? "" : downloads.rbegin()->first) + ", \"by_version\": [";
  if (!downloads.empty())
  {
    bool first = true;
    for (auto it = downloads.rbegin()->second.rbegin(); it != downloads.rbegin()->second.rend(); ++it)
    {
      o += std::string(first ? "" : ", ") + "{\"tag\": " + JsonText(it->first) + ", \"total\": " +
           std::to_string(it->second) + "}";
      first = false;
    }
  }
  o += "], \"history\": [";
  // (The last 180 days: each day's total and its versions'.)
  size_t skip = downloads.size() > 180 ? downloads.size() - 180 : 0;
  bool first = true;
  for (const auto& [date, tags] : downloads)
  {
    if (skip > 0)
    {
      skip--;
      continue;
    }
    uint64_t sum = 0;
    std::string by = "{";
    bool first_tag = true;
    for (const auto& [tag, n] : tags)
    {
      sum += n;
      by += std::string(first_tag ? "" : ", ") + JsonText(tag) + ": " + std::to_string(n);
      first_tag = false;
    }
    o += std::string(first ? "" : ", ") + "{\"date\": " + JsonText(date) + ", \"total\": " + std::to_string(sum) +
         ", \"by_version\": " + by + "}}";
    first = false;
  }
  o += "]},\n";
  o += "  \"public_rooms\": " + rooms + ",\n";
  o += "  \"private_rooms\": {\"open\": " + std::to_string(private_open) + ", \"playing\": " +
       std::to_string(private_playing) + "},\n";
  o += "  \"attribution\": {\"geolocation\": \"IP Geolocation by DB-IP\", \"geolocation_url\": \"https://db-ip.com\", "
       "\"geolocation_license\": \"CC BY 4.0\", \"titles\": \"libretro-database (No-Intro, Redump; CC BY-SA 4.0), "
       "FinalBurn Neo\"}\n}\n";
  std::fputs(o.c_str(), stdout);
  return 0;
}

// ---- public-profiles: the opt-in profiles' files for the public page, and its rankings (publish-stats.sh) ----
// Each profile is one file, p/<name>.bin: its name and its bytes come from its code (SHA-256 of it), so the file of a
// profile can only be found and read with the code - the repository lists opaque names of unreadable files. Its
// content: "ESP1" and the profile's JSON XOR a SHA-256 keystream of the code (site/profile.js reads it back). A file
// is written only when its profile changed (the same profile, the same bytes: git sends nothing for it), and deleted
// with its profile. rankings.json has the public profiles only: their nickname, country and hours.
std::string Sha256Of(const std::string& text)
{
  Sha256 h;
  h.Add(text.data(), text.size());
  uint8_t out[32];
  h.Finish(out);
  return std::string(reinterpret_cast<const char*>(out), 32);
}

std::string ProfileFileName(const std::string& code)
{
  const std::string digest = Sha256Of("emusuni-profile-file:" + code);
  return Hex(reinterpret_cast<const uint8_t*>(digest.data()), 16) + ".bin";
}

std::string SealProfile(const std::string& code, const std::string& plain)
{
  std::string out = "ESP1" + plain;
  for (size_t at = 0; at < plain.size(); at += 32)
  {
    const std::string block = Sha256Of("emusuni-profile-key:" + code + ":" + std::to_string(at / 32));
    for (size_t k = 0; k < 32 && at + k < plain.size(); k++)
      out[4 + at + k] = static_cast<char>(out[4 + at + k] ^ block[k]);
  }
  return out;
}

std::string HoursOfMinutes(int64_t minutes)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(minutes) / 60.0);
  return text;
}

// A profile as its page shows it (JSON). `top_game`: its most played game's title ("" when none or not known).
std::string ProfileJson(sqlite3* db, const std::string& id, const std::unordered_map<std::string, std::string>& titles,
                        std::string* top_game, std::string* top_console)
{
  Stmt p(db, "SELECT public, nick, country, created, last_seen, minutes, sessions, online_matches FROM profile "
             "WHERE id = ?");
  p.Blob(1, id.data(), static_cast<int>(id.size()));
  if (!p.Row())
    return {};
  const bool is_public = p.ColInt(0) != 0;
  std::string o = "{\"v\": 1, \"public\": " + std::string(is_public ? "true" : "false") + ", \"nick\": " +
                  (is_public && !p.ColText(1).empty() ? JsonText(p.ColText(1)) : std::string("null")) +
                  ", \"country\": " + JsonText(p.ColText(2) == "??" ? "" : p.ColText(2)) + ", \"since\": " +
                  JsonText(DateOf(p.ColInt(3))) + ", \"last_seen\": " + JsonText(DateOf(p.ColInt(4))) +
                  ", \"hours\": " + HoursOfMinutes(p.ColInt(5)) + ", \"sessions\": " + std::to_string(p.ColInt(6)) +
                  ", \"online_matches\": " + std::to_string(p.ColInt(7)) + ", \"consoles\": [";
  Stmt c(db, "SELECT console, minutes FROM profile_console WHERE profile = ? ORDER BY minutes DESC, console");
  c.Blob(1, id.data(), static_cast<int>(id.size()));
  bool first = true;
  while (c.Row())
  {
    if (first && top_console)
      *top_console = c.ColText(0);
    o += std::string(first ? "" : ", ") + "{\"console\": " + JsonText(c.ColText(0)) + ", \"hours\": " +
         HoursOfMinutes(c.ColInt(1)) + "}";
    first = false;
  }
  o += "], \"games\": [";
  Stmt g(db, "SELECT game, console, minutes, sessions, online_matches, last_played FROM profile_game "
             "WHERE profile = ? ORDER BY minutes DESC, game LIMIT 100");
  g.Blob(1, id.data(), static_cast<int>(id.size()));
  first = true;
  while (g.Row())
  {
    const auto title = titles.find(g.ColText(0));
    if (first && top_game)
      *top_game = title == titles.end() ? "" : g.ColText(1) + "\t" + title->second;
    o += std::string(first ? "" : ", ") + "{\"console\": " + JsonText(g.ColText(1)) + ", \"title\": " +
         (title == titles.end() ? std::string("null") : JsonText(title->second)) + ", \"hours\": " +
         HoursOfMinutes(g.ColInt(2)) + ", \"sessions\": " + std::to_string(g.ColInt(3)) + ", \"online_matches\": " +
         std::to_string(g.ColInt(4)) + ", \"last_played\": " + JsonText(DateOf(g.ColInt(5))) + "}";
    first = false;
  }
  return o + "]}";
}

int PublicProfiles(const std::string& dir, const std::string& titles_path, const std::string& out_dir)
{
  sqlite3* db = StoreOpen(dir);
  if (!db)
    return 1;
  struct Closer
  {
    sqlite3* db;
    ~Closer() { sqlite3_close(db); }
  } closer{db};
  const auto titles = LoadTitles(titles_path);
  mkdir(out_dir.c_str(), 0750);
  const std::string folder = out_dir + "/p";
  mkdir(folder.c_str(), 0750);
  std::unordered_set<std::string> stale;
  if (DIR* listing = opendir(folder.c_str()))
  {
    while (const dirent* entry = readdir(listing))
    {
      const std::string name = entry->d_name;
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0)
        stale.insert(name);
    }
    closedir(listing);
  }
  struct Ranked
  {
    std::string nick, country, game_console, game;
    int64_t minutes = 0;
  };
  std::vector<Ranked> ranked;
  int profiles = 0, written = 0, unpublished = 0;
  // (00.95) Only the profiles whose last version is a published one: a test version's profile has no file (its file
  // of before deleted) and isn't ranked.
  const std::set<std::string> published = PublishedOf(db);
  Stmt all(db, "SELECT id, code, public, nick, country, minutes, version FROM profile");
  while (all.Row())
  {
    const std::string id(static_cast<const char*>(sqlite3_column_blob(all.s, 0)),
                         static_cast<size_t>(sqlite3_column_bytes(all.s, 0)));
    const std::string code = all.ColText(1);
    if (!published.count(all.ColText(6)))
    {
      unpublished++;
      continue;
    }
    std::string top_game;
    const std::string json = ProfileJson(db, id, titles, &top_game, nullptr);
    if (json.empty() || code.empty())
      continue;
    profiles++;
    const std::string name = ProfileFileName(code);
    stale.erase(name);
    const std::string sealed = SealProfile(code, json);
    if (ReadWhole(folder + "/" + name) != sealed)
    {
      WriteWhole(folder + "/" + name, sealed);
      written++;
    }
    if (all.ColInt(2) != 0)
    {
      Ranked r;
      r.nick = all.ColText(3).empty() ? "Player" : all.ColText(3);
      r.country = all.ColText(4) == "??" ? "" : all.ColText(4);
      r.minutes = all.ColInt(5);
      const size_t tab = top_game.find('\t');
      if (tab != std::string::npos)
      {
        r.game_console = top_game.substr(0, tab);
        r.game = top_game.substr(tab + 1);
      }
      ranked.push_back(r);
    }
  }
  for (const std::string& name : stale)
    std::remove((folder + "/" + name).c_str());
  // The rankings: the most hours, in each country, on each console (public profiles only).
  std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    return a.minutes != b.minutes ? a.minutes > b.minutes : a.nick < b.nick;
  });
  auto entry = [](const Ranked& r, int64_t minutes) {
    return "{\"nick\": " + JsonText(r.nick) + ", \"country\": " + JsonText(r.country) + ", \"hours\": " +
           HoursOfMinutes(minutes) + ", \"top_game\": " +
           (r.game.empty() ? std::string("null")
                           : "{\"console\": " + JsonText(r.game_console) + ", \"title\": " + JsonText(r.game) + "}") +
           "}";
  };
  std::string o = "{\n  \"generated\": " + JsonText(TimeText(WallNow(), "%Y-%m-%dT%H:%M:%SZ")) +
                  ",\n  \"public_profiles\": " + std::to_string(ranked.size()) + ",\n  \"top\": [";
  for (size_t i = 0; i < ranked.size() && i < 100; i++)
    o += std::string(i ? ", " : "") + entry(ranked[i], ranked[i].minutes);
  o += "],\n  \"by_country\": {";
  std::map<std::string, std::vector<const Ranked*>> countries;
  for (const Ranked& r : ranked)
  {
    if (!r.country.empty() && countries[r.country].size() < 20)
      countries[r.country].push_back(&r);
  }
  bool first = true;
  for (const auto& [country, list] : countries)
  {
    o += std::string(first ? "" : ", ") + JsonText(country) + ": [";
    for (size_t i = 0; i < list.size(); i++)
      o += std::string(i ? ", " : "") + entry(*list[i], list[i]->minutes);
    o += "]";
    first = false;
  }
  o += "},\n  \"by_console\": {";
  std::map<std::string, std::vector<std::pair<Ranked, int64_t>>> consoles;
  {
    Stmt q(db, "SELECT p.nick, p.country, c.console, c.minutes, p.version FROM profile_console c JOIN profile p ON "
               "p.id = c.profile WHERE p.public = 1 ORDER BY c.console, c.minutes DESC");
    while (q.Row())
    {
      if (!published.count(q.ColText(4)))
        continue;  // (00.95)
      auto& list = consoles[q.ColText(2)];
      if (list.size() >= 20)
        continue;
      Ranked r;
      r.nick = q.ColText(0).empty() ? "Player" : q.ColText(0);
      r.country = q.ColText(1) == "??" ? "" : q.ColText(1);
      list.emplace_back(r, q.ColInt(3));
    }
  }
  first = true;
  for (const auto& [console, list] : consoles)
  {
    o += std::string(first ? "" : ", ") + JsonText(console) + ": [";
    for (size_t i = 0; i < list.size(); i++)
      o += std::string(i ? ", " : "") + entry(list[i].first, list[i].second);
    o += "]";
    first = false;
  }
  o += "}\n}\n";
  WriteWhole(out_dir + "/rankings.json", o);
  std::printf("public-profiles: %d profiles (%zu public), %d files written, %zu deleted; %d of test versions left out\n",
              profiles, ranked.size(), written, stale.size(), unpublished);
  return 0;
}

// profile CODE [--delete]: a profile as its page shows it, or deleted (a player who lost the PS4 asks by its code).
int ProfileCommand(const std::string& dir, const std::string& typed, bool remove, const std::string& titles_path)
{
  const std::string code = NormalizeProfileCode(typed);
  if (code.empty())
  {
    std::fprintf(stderr, "profile: %s isn't a profile code (16 letters and digits)\n", typed.c_str());
    return 2;
  }
  sqlite3* db = StoreOpen(dir);
  if (!db)
    return 1;
  std::string id;
  {
    Stmt q(db, "SELECT id FROM profile WHERE code = ?");
    q.Text(1, code);
    if (q.Row())
      id.assign(static_cast<const char*>(sqlite3_column_blob(q.s, 0)), static_cast<size_t>(sqlite3_column_bytes(q.s, 0)));
  }
  int result = 0;
  if (id.empty())
  {
    std::fprintf(stderr, "profile: no profile with the code %s\n", code.c_str());
    result = 1;
  }
  else if (remove)
  {
    StoreDeleteProfile(db, id.data(), static_cast<int>(id.size()));
    std::printf("profile %s deleted (its file goes with the next publish)\n", code.c_str());
  }
  else
  {
    std::printf("%s\n", ProfileJson(db, id, LoadTitles(titles_path), nullptr, nullptr).c_str());
  }
  sqlite3_close(db);
  return result;
}
