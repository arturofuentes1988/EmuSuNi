// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// emusuni-netd's world chat (01.07, the owner: "un chat mundial (general)"): src/NetplayProtocol.h's WORLD CHAT. Part
// of emusuni-netd.cpp (included before its OnPacket: its namespace and helpers).
//
// In memory: the last WORLD_KEPT lines and each player's limits, by its code (an HMAC of its chat key with the servers'
// secret: never the key, never an address). Limits: one line every WORLD_EVERY s and WORLD_MINUTE a minute per player
// and per address (one that changes its key), WORLD_ALL_RATE a second in the whole server; the questions and the
// reports their own.
//
// On disk (the owner's decision: "30 días, sin IP") - the main server with --stats-dir, stats.db's chat_log -, each
// line CHAT_LOG_DAYS days for moderation, then deleted: its time (UTC), where ("world", or a room's code:
// RoomChatCopy), the nickname as shown, the player's code, the country as shown ("" none), the app's version, the
// phrase (and its parameter) or the text, and a keyed hash of its address: HMAC-SHA256 with a salt of CHAT_SALT_DAYS
// days, the salt in
// --chat-salt-dir (the main server's: /var/lib/emusuni-netd/chat-salt, 700, its env by scripts/server/deploy.sh -
// without it the --secret-file's folder, which only a server run by hand can write: the systemd unit's ProtectSystem=
// strict leaves /etc/emusuni-netd read only. Never the repository's nor stats.db's folder - the owner's panel reads that
// one, and an address can't be found from its hash without the salt), made by the server, each one deleted once no
// line of it is left. Never the address. Nothing stored without a salt's folder it can write (the log says "keeps no
// line"), nor a line from an app that didn't show the notice (WORLD_NOTICE_SEEN).
// The log has counts only, never a line's text.
//
// The owner's panel (server/reports_panel.py) reads chat_log; its blocks come back in --chat-blocks FILE, read again
// when it changes: "code CODE UNTIL" and "ip HASH SALT UNTIL" lines (UNTIL a Unix time, 0 always; a hash's block lasts
// as long as its salt at most). A report marks its line (chat_log.reported), nothing of who reported.

constexpr double WORLD_EVERY = 3.0;        // s between a player's lines
constexpr double WORLD_MINUTE = 20.0;      // a player's lines a minute
constexpr double WORLD_ALL_RATE = 10.0;    // the server's lines a second
constexpr double WORLD_ASK_RATE = 1.0, WORLD_ASK_BURST = 4.0;  // an address's questions
constexpr double WORLD_REPORT_RATE = 5.0 / 3600.0, WORLD_REPORT_BURST = 5.0;  // an address's reports
constexpr int WORLD_FIRST = 20;            // the lines a question since 0 gets (the newest ones)
constexpr int CHAT_LOG_DAYS = 30, CHAT_SALT_DAYS = 30;
constexpr int MAX_WORLD_PHRASE = 64;       // (a newer app's phrase numbers: its own to show)

struct WorldEntry
{
  WorldLine line;
  double at = 0.0;
  int64_t row = 0;  // its chat_log row (0: not stored)
};
std::deque<WorldEntry> s_world;
uint32_t s_world_serial = 0;
struct WorldPlayer
{
  Bucket every, minute;
  double last = 0.0;
};
std::unordered_map<std::string, WorldPlayer> s_world_players;  // by code
Bucket s_world_all;
struct WorldCounts
{
  uint64_t lines = 0, refused = 0, reports = 0, asks = 0, room_copies = 0;
} s_world_counts;

std::string s_chat_salt_dir, s_chat_blocks_path;
struct ChatSalt
{
  int64_t period = -1;
  std::vector<uint8_t> key;
} s_salt;
bool s_salt_said = false;

// (WallNow: emusuni-netd-stats.h's - the tests' clock offset too.)
int64_t SaltPeriod(int64_t wall)
{
  return wall / (int64_t{CHAT_SALT_DAYS} * 86400);
}

std::string SaltPath(int64_t period)
{
  return s_chat_salt_dir + "/chat-salt-" + std::to_string(period);
}

// This period's salt: read, or made (32 random bytes, the server's user only). False: no folder, or it can't be.
double s_salt_failed_at = -1e9;
bool CurrentSalt(ChatSalt** out)
{
  if (s_chat_salt_dir.empty())
    return false;
  const int64_t period = SaltPeriod(WallNow());
  if (s_salt.period == period && !s_salt.key.empty())
  {
    *out = &s_salt;
    return true;
  }
  if (Now() - s_salt_failed_at < 60.0)
    return false;  // (tried a moment ago: Members asks at every keepalive)
  const std::string path = SaltPath(period);
  std::vector<uint8_t> key(32);
  bool ok = false;
  if (FILE* f = std::fopen(path.c_str(), "rb"))
  {
    ok = std::fread(key.data(), 1, key.size(), f) == key.size();
    std::fclose(f);
  }
  else
  {
    ok = getrandom(key.data(), key.size(), 0) == static_cast<ssize_t>(key.size());
    const int fd = ok ? open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
    ok = fd >= 0 && write(fd, key.data(), key.size()) == static_cast<ssize_t>(key.size());
    if (fd >= 0)
      close(fd);
    if (ok)
      std::printf("world chat: a new salt for the addresses' hashes (period %lld)\n", static_cast<long long>(period));
  }
  if (!ok)
  {
    if (!s_salt_said)
      std::fprintf(stderr, "world chat: the salt %s can't be read or made: no line stored\n", path.c_str());
    s_salt_said = true;
    s_salt_failed_at = Now();
    return false;
  }
  s_salt.period = period;
  s_salt.key = key;
  *out = &s_salt;
  return true;
}

// Whether the lines are kept (WORLD_STORED): the statistics' database and the salt.
bool ChatStored()
{
  ChatSalt* salt = nullptr;
  return s_stats.on && s_stats.db && CurrentSalt(&salt);
}
// (The rooms' chat's copies: the same - Members' third extra says it to the room.)
bool RoomChatStored()
{
  return !s_relay_only && ChatStored();
}

// An address's keyed hash (16 hex digits of HMAC-SHA256(salt, its 4 bytes)).
std::string AddressHash(uint32_t ip, const ChatSalt& salt)
{
  const uint8_t bytes[4] = {static_cast<uint8_t>(ip >> 24), static_cast<uint8_t>(ip >> 16),
                            static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip)};
  uint8_t mac[32];
  HmacKey(salt.key.data(), salt.key.size(), bytes, sizeof(bytes), mac);
  char hex[17];
  for (int i = 0; i < 8; i++)
    std::snprintf(hex + i * 2, 3, "%02x", mac[i]);
  return hex;
}

// ---- The owner's blocks (--chat-blocks) ----
struct ChatBlock
{
  bool ip = false;
  std::string value;
  int64_t salt = -1;
  int64_t until = 0;  // 0: always
};
std::vector<ChatBlock> s_chat_blocks;
int64_t s_chat_blocks_mtime = -1;
double s_chat_blocks_checked = -1e9;

void LoadChatBlocks()
{
  if (s_chat_blocks_path.empty() || Now() - s_chat_blocks_checked < 10.0)
    return;
  s_chat_blocks_checked = Now();
  struct stat st;
  if (stat(s_chat_blocks_path.c_str(), &st) != 0)
  {
    s_chat_blocks.clear();
    s_chat_blocks_mtime = -1;
    return;
  }
  // (Its time to the nanosecond and its size: the panel may write it twice in one second.)
  const int64_t signature = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 +
                            static_cast<int64_t>(st.st_mtim.tv_nsec) + static_cast<int64_t>(st.st_size) * 7919;
  if (signature == s_chat_blocks_mtime)
    return;
  s_chat_blocks_mtime = signature;
  std::ifstream in(s_chat_blocks_path);
  std::vector<ChatBlock> blocks;
  std::string line;
  while (std::getline(in, line) && blocks.size() < 100000)
  {
    std::istringstream words(line);
    std::string kind;
    ChatBlock b;
    if (!(words >> kind) || kind.empty() || kind[0] == '#')
      continue;
    if (kind == "code" && words >> b.value >> b.until)
      blocks.push_back(b);
    else if (kind == "ip" && words >> b.value >> b.salt >> b.until)
    {
      b.ip = true;
      blocks.push_back(b);
    }
  }
  s_chat_blocks.swap(blocks);
  std::printf("world chat: %zu blocks read\n", s_chat_blocks.size());
}

// The seconds left of a block of that player or address (-1: none, 0xFFFF: always).
int BlockedSeconds(const std::string& code, const std::string& hash, int64_t salt)
{
  LoadChatBlocks();
  const int64_t wall = WallNow();
  int64_t left = -1;
  for (const ChatBlock& b : s_chat_blocks)
  {
    const bool match = b.ip ? (!hash.empty() && b.value == hash && b.salt == salt) : b.value == code;
    if (!match || (b.until != 0 && b.until <= wall))
      continue;
    if (b.until == 0)
      return 0xFFFF;
    left = std::max(left, b.until - wall);
  }
  return left < 0 ? -1 : static_cast<int>(std::min<int64_t>(left, 0xFFFE));
}

// A line kept (chat_log): where, the line as shown, its app's version, its address's hash and salt. Its row.
int64_t StoreChatLine(const std::string& place, const WorldLine& line, const std::string& version,
                      const std::string& hash, int64_t salt)
{
  Stmt q(s_stats.db, "INSERT INTO chat_log(at, place, nick, code, country, version, phrase, param, text, ip_hash, salt)"
                     " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  q.Int(1, WallNow()).Text(2, place).Text(3, line.nick).Text(4, line.code).Text(5, line.country).Text(6, version);
  q.Int(7, line.phrase).Text(8, line.param).Text(9, line.text).Text(10, hash).Int(11, salt).Run();
  return sqlite3_last_insert_rowid(s_stats.db);
}

// A player's code from its chat key (the world chat's and the rooms' copies': the same player, the same code).
std::string ChatCode(const uint8_t* key)
{
  uint8_t mac[32];
  uint8_t keyed[6 + WORLD_KEY_SIZE];
  std::memcpy(keyed, "world:", 6);
  std::memcpy(keyed + 6, key, WORLD_KEY_SIZE);
  Hmac(keyed, sizeof(keyed), mac);
  return PlayerCode(mac);
}

// (01.07) A room's chat line's copy (RoomChatCopy): from a member of it (its token), once its notice was shown, at
// the room chat's own pace - kept with the room's code as its place, the member's nickname and country as the room
// shows them. Nothing kept without the salt or the database; no answer.
void OnRoomChatCopy(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t flags = r.U8();
  const uint8_t* key = r.Bytes(WORLD_KEY_SIZE);
  WorldLine line;
  line.phrase = r.U8();
  if (line.phrase)
    line.param = CleanText(r.Str(MAX_PHRASE_PARAM), MAX_PHRASE_PARAM, "");
  else
    line.text = CleanChat(r.Str(CHAT_BYTES));
  if (!slot || !r.Ok() || !key || slot->info.id >= MAX_MEMBERS || !(flags & WORLD_NOTICE_SEEN) ||
      (line.phrase == 0 && line.text.empty()) || line.phrase > MAX_WORLD_PHRASE)
    return;
  if (!slot->chat.Take(Now(), 0.5, 3.0))
    return;
  ChatSalt* salt = nullptr;
  if (!s_stats.on || !s_stats.db || !CurrentSalt(&salt))
    return;
  line.code = ChatCode(key);
  line.nick = slot->info.nick;
  line.country = slot->info.country;
  line.param = line.phrase == PHRASE_FROM ? line.country : std::string();  // (as the others in the room fill it)
  if (line.phrase == PHRASE_FROM && line.param.empty())
    return;
  StoreChatLine(room->code, line, slot->info.app_version, AddressHash(ntohl(from.sin_addr.s_addr), *salt),
                salt->period);
  s_world_counts.room_copies++;
}

// ---- The requests ----
void SendWorldSaid(const sockaddr_in& to, uint32_t nonce, WorldResult result, uint32_t serial, int seconds,
                   const std::pair<uint64_t, uint32_t>& key)
{
  uint8_t buf[32];
  Writer w(buf, sizeof(buf));
  w.Header(Type::WorldSaid);
  w.U32(nonce);
  w.U8(static_cast<uint8_t>(result));
  w.U32(serial);
  w.U16(static_cast<uint16_t>(std::clamp(seconds, 0, 0xFFFF)));
  s_replies[key] = {std::vector<uint8_t>(buf, buf + w.Size()), Now()};
  Send(to, buf, w.Size());
  if (result != WorldResult::Sent)
    s_world_counts.refused++;
}

void OnWorldChat(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const uint32_t since = r.U32();
  const double now = Now();
  if (!r.Ok() || !state.world_asks.Take(now, WORLD_ASK_RATE, WORLD_ASK_BURST))
    return;
  s_world_counts.asks++;
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::WorldLines);
  w.U32(nonce);
  w.U32(s_world_serial);
  w.U8(ChatStored() ? WORLD_STORED : 0);
  NetCaps shows;
  shows.caps = CAP_COUNTRY;
  w.Str(ShownCountry(from, 0, shows), COUNTRY_SIZE);  // ((01.07) its "I'm from ..." - the app offers it with one only)
  const size_t count_at = w.Size();
  w.U8(0);
  size_t first = 0;
  if (since == 0 || since > s_world_serial)
    first = s_world.size() > static_cast<size_t>(WORLD_FIRST) ? s_world.size() - WORLD_FIRST : 0;
  else
  {
    while (first < s_world.size() && s_world[first].line.serial <= since)
      first++;
  }
  int count = 0;
  for (size_t i = first; i < s_world.size() && count < 255; i++)
  {
    WorldLine line = s_world[i].line;
    line.age = static_cast<uint16_t>(std::clamp(now - s_world[i].at, 0.0, 65535.0));
    uint8_t one[MAX_PACKET];
    Writer o(one, sizeof(one));
    WriteWorldLine(o, line);
    if (!o.Ok() || w.Size() + o.Size() > MAX_PACKET)
      break;  // (asked again from the last one sent)
    w.Bytes(one, o.Size());
    count++;
  }
  buf[count_at] = static_cast<uint8_t>(count);
  if (w.Ok())
    Send(from, buf, w.Size());
}

void OnWorldSay(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const auto reply = std::make_pair(AddressKey(from), nonce);
  if (auto it = s_replies.find(reply); it != s_replies.end())
  {
    Send(from, it->second.packet.data(), it->second.packet.size());  // (its answer lost: the same, not a line more)
    return;
  }
  const std::string version = Token(r.Str(MAX_APP_VERSION), MAX_APP_VERSION);
  const std::string nick = CleanNick(r.Str(MAX_NICK));
  const uint8_t flags = r.U8();
  const uint8_t* key = r.Bytes(WORLD_KEY_SIZE);
  WorldLine line;
  line.phrase = r.U8();
  if (line.phrase)
    line.param = CleanText(r.Str(MAX_PHRASE_PARAM), MAX_PHRASE_PARAM, "");
  else
    line.text = CleanChat(r.Str(CHAT_BYTES));
  if (!r.Ok() || !key || (line.phrase == 0 && line.text.empty()) || line.phrase > MAX_WORLD_PHRASE)
  {
    SendWorldSaid(from, nonce, WorldResult::Bad, 0, 0, reply);
    return;
  }
  if (!(flags & WORLD_NOTICE_SEEN))
  {
    SendWorldSaid(from, nonce, WorldResult::NoNotice, 0, 0, reply);
    return;
  }
  // The player's code: never its key (kept nowhere), never its address.
  line.code = ChatCode(key);
  const uint32_t ip = ntohl(from.sin_addr.s_addr);
  ChatSalt* salt = nullptr;
  const bool stored = s_stats.on && s_stats.db && CurrentSalt(&salt);
  const std::string hash = salt ? AddressHash(ip, *salt) : std::string();
  const int blocked = BlockedSeconds(line.code, hash, salt ? salt->period : -1);
  if (blocked >= 0)
  {
    SendWorldSaid(from, nonce, WorldResult::Blocked, 0, blocked, reply);
    return;
  }
  const double now = Now();
  WorldPlayer& player = s_world_players[line.code];
  player.last = now;
  const double every = std::max(player.every.Wait(now, 1.0 / WORLD_EVERY, 1.0),
                                state.world_every.Wait(now, 1.0 / WORLD_EVERY, 1.0));
  const double minute = std::max(player.minute.Wait(now, WORLD_MINUTE / 60.0, WORLD_MINUTE),
                                 state.world_minute.Wait(now, WORLD_MINUTE / 60.0, WORLD_MINUTE));
  if (minute > 0.0)
  {
    SendWorldSaid(from, nonce, WorldResult::TooMany, 0, static_cast<int>(std::ceil(minute)), reply);
    return;
  }
  if (every > 0.0)
  {
    SendWorldSaid(from, nonce, WorldResult::TooSoon, 0, static_cast<int>(std::ceil(every)), reply);
    return;
  }
  if (!s_world_all.Take(now, WORLD_ALL_RATE, WORLD_ALL_RATE))
  {
    SendWorldSaid(from, nonce, WorldResult::Busy, 0, 1, reply);
    return;
  }
  player.every.Take(now, 1.0 / WORLD_EVERY, 1.0);
  player.minute.Take(now, WORLD_MINUTE / 60.0, WORLD_MINUTE);
  state.world_every.Take(now, 1.0 / WORLD_EVERY, 1.0);
  state.world_minute.Take(now, WORLD_MINUTE / 60.0, WORLD_MINUTE);
  line.nick = nick;
  NetCaps shows;
  shows.caps = CAP_COUNTRY;  // (an app of the world chat has "Show My Country")
  line.country = ShownCountry(from, flags, shows);
  // ((01.07) "I'm from ...": its country is the one the line shows - never the sayer's word; none, not said.)
  line.param = line.phrase == PHRASE_FROM ? line.country : std::string();
  if (line.phrase == PHRASE_FROM && line.param.empty())
  {
    SendWorldSaid(from, nonce, WorldResult::Bad, 0, 0, reply);
    return;
  }
  line.serial = ++s_world_serial;
  WorldEntry entry;
  entry.line = line;
  entry.at = now;
  if (stored)
    entry.row = StoreChatLine("world", line, version, hash, salt->period);
  s_world.push_back(std::move(entry));
  while (s_world.size() > static_cast<size_t>(WORLD_KEPT))
    s_world.pop_front();
  s_world_counts.lines++;
  SendWorldSaid(from, nonce, WorldResult::Sent, line.serial, 0, reply);
}

void OnWorldReport(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const uint32_t serial = r.U32();
  if (!r.Ok() || !state.world_reports.Take(Now(), WORLD_REPORT_RATE, WORLD_REPORT_BURST))
    return;
  for (const WorldEntry& e : s_world)
  {
    if (e.line.serial != serial)
      continue;
    s_world_counts.reports++;
    if (e.row > 0 && s_stats.on && s_stats.db)
    {
      Stmt q(s_stats.db, "UPDATE chat_log SET reported = reported + 1 WHERE id = ?");
      q.Int(1, e.row).Run();
    }
    return;
  }
}

// Every hour: the lines older than CHAT_LOG_DAYS deleted, the salts no line is left of deleted; the players' limits
// silent for 10 minutes forgotten.
void ChatHousekeeping()
{
  static double last = -1e9;
  const double now = Now();
  for (auto it = s_world_players.begin(); it != s_world_players.end();)
    it = now - it->second.last > 600.0 ? s_world_players.erase(it) : std::next(it);
  if (now - last < 3600.0)
    return;
  last = now;
  if (!s_stats.on || !s_stats.db)
    return;
  Stmt prune(s_stats.db, "DELETE FROM chat_log WHERE at < ?");
  prune.Int(1, WallNow() - int64_t{CHAT_LOG_DAYS} * 86400).Run();
  if (s_chat_salt_dir.empty())
    return;
  const int64_t current = SaltPeriod(WallNow());
  DIR* dir = opendir(s_chat_salt_dir.c_str());
  if (!dir)
    return;
  std::vector<int64_t> old;
  while (dirent* e = readdir(dir))
  {
    const std::string name = e->d_name;
    if (name.rfind("chat-salt-", 0) != 0)
      continue;
    const int64_t period = std::atoll(name.c_str() + 10);
    if (period < current && std::to_string(period) == name.substr(10))
      old.push_back(period);
  }
  closedir(dir);
  for (const int64_t period : old)
  {
    Stmt q(s_stats.db, "SELECT COUNT(*) FROM chat_log WHERE salt = ?");
    q.Int(1, period);
    if (q.Row() && q.ColInt(0) == 0 && std::remove(SaltPath(period).c_str()) == 0)
      std::printf("world chat: the salt of period %lld deleted (no line of it left)\n", static_cast<long long>(period));
  }
}

std::string WorldLogText()
{
  std::string text = "; world chat: " + std::to_string(s_world_counts.lines) + " lines, " +
                     std::to_string(s_world_counts.refused) + " refused, " + std::to_string(s_world_counts.reports) +
                     " reported, " + std::to_string(s_world_counts.asks) + " questions; rooms' chat: " +
                     std::to_string(s_world_counts.room_copies) + " lines kept";
  s_world_counts = WorldCounts();
  return text;
}
