// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// emusuni-netd's players' ranking (01.07, the owner: "un ranking de jugadores: horas en partidas, país, consola más
// jugada, 7 días y desde siempre"): src/NetplayProtocol.h's TopPlayers. Part of emusuni-netd.cpp (included before its
// OnPacket).
//
// Each ranked player's time in matches (Create's and Join's player block: PLAYER_RANKED - "Appear in the Ranking:
// Yes", the app's default -), by its code (the chat's: an HMAC of its chat key, never the key or an address): from the
// match's start (Lock) - or its late join - to its end, its leaving, its connection lost, the room closing. Not the
// time in the room's menu, nor a spectator's. The main server with --stats-dir only (stats.db):
//  - player_time: a row by code, day (UTC) and console, its seconds - PLAYER_DAYS days (the 7 days' ranking);
//  - player_total: a row by code and console, its seconds since ever;
//  - players: each code's last nickname (CleanNick's, as the rooms show it) and country (only when it shows it).
// Without PLAYER_RANKED nothing of that player is counted or kept.

constexpr int PLAYER_DAYS = 30;
double s_player_min_seconds = 60.0;  // (a ranking's player: a minute at least - the PC test's build: --player-min)

void PlayerTime(const Room& room, Slot& slot, double until)
{
  const double from = slot.match_from;
  slot.match_from = 0.0;
  if (!slot.used || slot.rank_code.empty() || from <= 0.0 || !s_stats.on || !s_stats.db)
    return;
  const double end = until > 0.0 ? until : Now();
  const int64_t seconds = static_cast<int64_t>(end - from);
  if (seconds <= 0)
    return;
  // The console: the room's; a Choose Together match's, its game's (its Lock's label: "snes.c-...").
  std::string console = room.game.console == TOGETHER ? room.chosen.substr(0, room.chosen.find('.')) :
                                                        room.game.console;
  console = Token(console, MAX_CONSOLE);
  if (console.empty())
    return;
  const int64_t wall = WallNow();
  char day[16];
  const time_t t = static_cast<time_t>(wall);
  std::strftime(day, sizeof(day), "%Y-%m-%d", std::gmtime(&t));
  Stmt a(s_stats.db, "INSERT INTO player_time(code, day, console, seconds) VALUES (?, ?, ?, ?)"
                     " ON CONFLICT(code, day, console) DO UPDATE SET seconds = seconds + excluded.seconds");
  a.Text(1, slot.rank_code).Text(2, day).Text(3, console).Int(4, seconds).Run();
  Stmt b(s_stats.db, "INSERT INTO player_total(code, console, seconds) VALUES (?, ?, ?)"
                     " ON CONFLICT(code, console) DO UPDATE SET seconds = seconds + excluded.seconds");
  b.Text(1, slot.rank_code).Text(2, console).Int(3, seconds).Run();
  Stmt c(s_stats.db, "INSERT OR REPLACE INTO players(code, nick, country, last_seen) VALUES (?, ?, ?, ?)");
  c.Text(1, slot.rank_code).Text(2, CleanNick(slot.info.nick)).Text(3, slot.info.country).Int(4, wall).Run();
  static double last_prune = -1e9;
  if (Now() - last_prune >= 3600.0)
  {
    last_prune = Now();
    const time_t old = static_cast<time_t>(wall - int64_t{PLAYER_DAYS} * 86400);
    char old_day[16];
    std::strftime(old_day, sizeof(old_day), "%Y-%m-%d", std::gmtime(&old));
    Stmt prune(s_stats.db, "DELETE FROM player_time WHERE day < ?");
    prune.Text(1, old_day).Run();
  }
}

void PlayersTime(Room& room)
{
  for (Slot& s : room.members)
    PlayerTime(room, s);
}

// ---- TopPlayers ----
struct TopPlayer
{
  std::string nick, country, console;
  int64_t seconds = 0;
};
struct TopPlayersCount
{
  std::vector<TopPlayer> players;
  double at = -1e9;
};
std::map<int, TopPlayersCount> s_top_players;  // by days (0: since ever)

void CountTopPlayers(int days, TopPlayersCount* out)
{
  out->players.clear();
  out->at = Now();
  if (!s_stats.on || !s_stats.db)
    return;
  const time_t since = static_cast<time_t>(WallNow() - int64_t{days} * 86400 + 86400);
  char since_day[16];
  std::strftime(since_day, sizeof(since_day), "%Y-%m-%d", std::gmtime(&since));
  // (The days: today and the 6 before - player_time; since ever: player_total.)
  const std::string table = days > 0 ? "player_time" : "player_total";
  const std::string where = days > 0 ? " WHERE day >= ?" : "";
  Stmt q(s_stats.db, ("SELECT code, SUM(seconds) AS s FROM " + table + where + " GROUP BY code HAVING s >= ? "
                      "ORDER BY s DESC, code LIMIT ?").c_str());
  const int arg = days > 0 ? 2 : 1;
  if (days > 0)
    q.Text(1, since_day);
  q.Int(arg, static_cast<int64_t>(s_player_min_seconds));
  q.Int(arg + 1, TOP_PLAYERS);
  std::vector<std::pair<std::string, int64_t>> codes;
  while (q.Row())
    codes.emplace_back(q.ColText(0), q.ColInt(1));
  for (const auto& [code, seconds] : codes)
  {
    TopPlayer p;
    p.seconds = seconds;
    Stmt who(s_stats.db, "SELECT nick, country FROM players WHERE code = ?");
    who.Text(1, code);
    if (who.Row())
    {
      p.nick = CleanNick(who.ColText(0));
      p.country = CleanCountry(who.ColText(1));
    }
    Stmt most(s_stats.db, ("SELECT console, SUM(seconds) AS s FROM " + table + " WHERE code = ?" +
                           (days > 0 ? " AND day >= ?" : "") + " GROUP BY console ORDER BY s DESC LIMIT 1")
                              .c_str());
    most.Text(1, code);
    if (days > 0)
      most.Text(2, since_day);
    if (most.Row())
      p.console = most.ColText(0);
    out->players.push_back(std::move(p));
  }
}

void OnTopPlayers(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  int days = r.U8();
  if (!r.Ok() || !state.lists.Take(Now(), LISTS_RATE, LISTS_BURST))
    return;
  days = days == 0 ? 0 : TOP_DAYS;
  TopPlayersCount& count = s_top_players[days];
  if (Now() - count.at > 600.0)
    CountTopPlayers(days, &count);
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::TopPlayersList);
  w.U32(nonce);
  w.U8(static_cast<uint8_t>(days));
  const size_t count_at = w.Size();
  w.U8(0);
  size_t n = 0;
  for (const TopPlayer& p : count.players)
  {
    uint8_t one[128];
    Writer o(one, sizeof(one));
    o.Str(p.nick, MAX_NICK);
    o.Str(p.country, COUNTRY_SIZE);
    o.U32(static_cast<uint32_t>(std::min<int64_t>(p.seconds, 0xFFFFFFFF)));
    o.Str(p.console, MAX_CONSOLE);
    if (!o.Ok() || w.Size() + o.Size() > MAX_PACKET)
      break;
    w.Bytes(one, o.Size());
    n++;
  }
  buf[count_at] = static_cast<uint8_t>(n);
  if (w.Ok())
    Send(from, buf, w.Size());
}
