// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// emusuni-netd's ELO ranking (01.91, the owner: "un ranking ELO de los 10 juegos más jugados en línea", the winner
// read "Automático por juego"): src/NetplayProtocol.h's EloResult, EloCounted, EloTop, EloList. Part of
// emusuni-netd.cpp (included after the players' ranking).
//
// Each console of a ranked match (src/Elo.h: its game's memory says who won) tells the main server once per match:
// the game, the frame, the port that won and the two ports' members. The match counts only when both members' consoles
// said the same, within ELO_PAIR_SECONDS (a lone report - a cheat, a console that missed it - counts nothing), both
// ranked ("Appear in the Ranking: Yes": their code, the chat's - an HMAC of its chat key, never an address). The main
// server with --stats-dir only (stats.db):
//  - elo: a row by game and code - its rating (ELO_START at the first match, ELO_K), its wins, its losses, when last;
//  - elo_pair_day: the same two players' matches of a game in a day (UTC), ELO_PAIR_DAY at most counted (a pair
//    playing for points against itself gains little anyway: the rating's change gets smaller);
//  - players (the players' ranking's): each code's last nickname and country (only when it shows it).
// A list shows the players with ELO_MIN_MATCHES matches or more, the best first, and the asker's own place.

int s_elo_min_matches = ELO_MIN_MATCHES;  // (the PC test's build: --elo-min)

struct EloReport
{
  std::string room;   // the room's code
  int match = 0;      // the room's matches then (Lock): the same match
  uint8_t member = 0;
  std::string game;
  uint32_t frame = 0;
  uint8_t winner = 0, port1 = 0, port2 = 0;
  double at = 0.0;
};
std::vector<EloReport> s_elo_reports;  // each waiting for the other player's (ELO_PAIR_SECONDS at most)

bool EloTables()
{
  static bool made = false;
  if (!s_stats.on || !s_stats.db)
    return false;
  if (!made)
  {
    StoreExec(s_stats.db,
              "CREATE TABLE IF NOT EXISTS elo(game TEXT, code TEXT, rating REAL, wins INT, losses INT, last INT,"
              " PRIMARY KEY(game, code)) WITHOUT ROWID;"
              "CREATE TABLE IF NOT EXISTS elo_pair_day(day TEXT, game TEXT, a TEXT, b TEXT, n INT,"
              " PRIMARY KEY(day, game, a, b)) WITHOUT ROWID;");
    made = true;
  }
  return true;
}

// The console a game's key is of ("snes-ki": snes; "arc-mk": arcade).
std::string EloConsole(const std::string& game)
{
  return game.compare(0, 5, "snes-") == 0 ? "snes" : game.compare(0, 4, "arc-") == 0 ? "arcade" : "";
}

struct EloRating
{
  double rating = ELO_START;
  int64_t wins = 0, losses = 0;
};

EloRating EloOf(const std::string& game, const std::string& code)
{
  EloRating e;
  Stmt q(s_stats.db, "SELECT rating, wins, losses FROM elo WHERE game = ? AND code = ?");
  q.Text(1, game).Text(2, code);
  if (q.Row())
  {
    e.rating = sqlite3_column_double(q.s, 0);
    e.wins = q.ColInt(1);
    e.losses = q.ColInt(2);
  }
  return e;
}

void SendEloCounted(const Slot& slot, const std::string& game, EloOutcome outcome, const EloRating& a, int da,
                    const EloRating& b, int db)
{
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  w.Header(Type::EloCounted);
  w.Str(game, MAX_ELO_GAME);
  w.U8(static_cast<uint8_t>(outcome));
  w.U16(static_cast<uint16_t>(std::clamp(std::lround(a.rating), 0L, 65535L)));
  w.I16(static_cast<int16_t>(da));
  w.U16(static_cast<uint16_t>(std::clamp(std::lround(b.rating), 0L, 65535L)));
  w.I16(static_cast<int16_t>(db));
  if (w.Ok())
    Send(slot.addr, buf, w.Size());
}

// Both consoles said the same: the match counted (or not: a player not ranked, the pair's day full).
void EloCount(Room& room, const EloReport& r)
{
  Slot& s1 = room.members[r.port1];
  Slot& s2 = room.members[r.port2];
  const std::string& c1 = s1.rank_code;
  const std::string& c2 = s2.rank_code;
  EloRating e1 = EloOf(r.game, c1), e2 = EloOf(r.game, c2);
  auto answer = [&](EloOutcome outcome, int d1, int d2) {
    SendEloCounted(s1, r.game, outcome, e1, d1, e2, d2);
    SendEloCounted(s2, r.game, outcome, e1, d1, e2, d2);
  };
  if (c1.empty() || c2.empty() || c1 == c2)
  {
    std::printf("elo: room %s: %s: a match not counted (a player not ranked)\n", room.code.c_str(), r.game.c_str());
    answer(EloOutcome::NotRanked, 0, 0);
    return;
  }
  const int64_t wall = WallNow();
  char day[16];
  const time_t t = static_cast<time_t>(wall);
  std::strftime(day, sizeof(day), "%Y-%m-%d", std::gmtime(&t));
  const std::string& a = std::min(c1, c2);
  const std::string& b = std::max(c1, c2);
  int64_t n = 0;
  {
    Stmt q(s_stats.db, "SELECT n FROM elo_pair_day WHERE day = ? AND game = ? AND a = ? AND b = ?");
    q.Text(1, day).Text(2, r.game).Text(3, a).Text(4, b);
    if (q.Row())
      n = q.ColInt(0);
  }
  if (n >= ELO_PAIR_DAY)
  {
    std::printf("elo: room %s: %s: the same two players' %lld matches today: not counted\n", room.code.c_str(),
                r.game.c_str(), static_cast<long long>(n + 1));
    answer(EloOutcome::TooMany, 0, 0);
    return;
  }
  // The expected score of port 1's player, then each rating moved by ELO_K times the surprise.
  const double expected1 = 1.0 / (1.0 + std::pow(10.0, (e2.rating - e1.rating) / 400.0));
  const double score1 = r.winner == 0 ? 1.0 : 0.0;
  const double change = ELO_K * (score1 - expected1);
  const EloRating before1 = e1, before2 = e2;
  e1.rating += change;
  e2.rating -= change;
  (r.winner == 0 ? e1.wins : e1.losses)++;
  (r.winner == 0 ? e2.losses : e2.wins)++;
  Stmt up(s_stats.db, "INSERT OR REPLACE INTO elo(game, code, rating, wins, losses, last) VALUES (?, ?, ?, ?, ?, ?)");
  for (const auto& [code, e] : {std::pair<const std::string&, const EloRating&>{c1, e1}, {c2, e2}})
  {
    up.Text(1, r.game).Text(2, code);
    sqlite3_bind_double(up.s, 3, e.rating);
    up.Int(4, e.wins).Int(5, e.losses).Int(6, wall).Run();
  }
  Stmt pair(s_stats.db, "INSERT INTO elo_pair_day(day, game, a, b, n) VALUES (?, ?, ?, ?, 1)"
                        " ON CONFLICT(day, game, a, b) DO UPDATE SET n = n + 1");
  pair.Text(1, day).Text(2, r.game).Text(3, a).Text(4, b).Run();
  Stmt who(s_stats.db, "INSERT OR REPLACE INTO players(code, nick, country, last_seen) VALUES (?, ?, ?, ?)");
  for (const Slot* s : {&s1, &s2})
    who.Text(1, s->rank_code).Text(2, CleanNick(s->info.nick)).Text(3, s->info.country).Int(4, wall).Run();
  static double last_prune = -1e9;
  if (Now() - last_prune >= 3600.0)
  {
    last_prune = Now();
    Stmt prune(s_stats.db, "DELETE FROM elo_pair_day WHERE day < ?");
    prune.Text(1, day).Run();
  }
  const int d1 = static_cast<int>(std::lround(e1.rating) - std::lround(before1.rating));
  const int d2 = static_cast<int>(std::lround(e2.rating) - std::lround(before2.rating));
  std::printf("elo: room %s: %s: %s %s %s - %ld (%+d) / %ld (%+d)\n", room.code.c_str(), r.game.c_str(),
              CleanNick(s1.info.nick).c_str(), r.winner == 0 ? "beat" : "lost to", CleanNick(s2.info.nick).c_str(),
              std::lround(e1.rating), d1, std::lround(e2.rating), d2);
  answer(EloOutcome::Counted, d1, d2);
}

void OnEloResult(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t version = r.U8();
  EloReport rep;
  rep.game = r.Str(MAX_ELO_GAME);
  rep.frame = r.U32();
  rep.winner = r.U8();
  rep.port1 = r.U8();
  rep.port2 = r.U8();
  // (A player's console only - never a spectator's -, of one of the two ports, in a match under way of that game's
  // console; both ports' members in the room.)
  if (!slot || !r.Ok() || version < ELO_VERSION || !IsEloGame(rep.game) || rep.winner > 1 ||
      rep.port1 >= MAX_MEMBERS || rep.port2 >= MAX_MEMBERS || rep.port1 == rep.port2 || slot->info.id >= MAX_MEMBERS ||
      (slot->info.id != rep.port1 && slot->info.id != rep.port2) || !room->members[rep.port1].used ||
      !room->members[rep.port2].used || !room->locked || !EloTables())
    return;
  const std::string console =
      room->game.console == TOGETHER ? room->chosen.substr(0, room->chosen.find('.')) : room->game.console;
  if (console != EloConsole(rep.game))
    return;
  const double now = Now();
  rep.room = room->code;
  rep.match = room->matches;
  rep.member = slot->info.id;
  rep.at = now;
  // The other's, of the same match and game, near that frame: the pair.
  s_elo_reports.erase(std::remove_if(s_elo_reports.begin(), s_elo_reports.end(),
                                     [&](const EloReport& e) { return now - e.at > ELO_PAIR_SECONDS; }),
                      s_elo_reports.end());
  for (auto it = s_elo_reports.begin(); it != s_elo_reports.end(); ++it)
  {
    if (it->room != rep.room || it->match != rep.match || it->game != rep.game)
      continue;
    const uint32_t gap = it->frame > rep.frame ? it->frame - rep.frame : rep.frame - it->frame;
    if (gap > ELO_PAIR_FRAMES)
      continue;
    if (it->member == rep.member)
      return;  // (said again: the first stays)
    const EloReport other = *it;
    s_elo_reports.erase(it);
    if (other.winner != rep.winner || other.port1 != rep.port1 || other.port2 != rep.port2)
    {
      std::printf("elo: room %s: %s: the two consoles saw two results (port %d won / port %d won): not counted\n",
                  rep.room.c_str(), rep.game.c_str(), other.winner + 1, rep.winner + 1);
      const EloRating e1 = EloOf(rep.game, room->members[rep.port1].rank_code);
      const EloRating e2 = EloOf(rep.game, room->members[rep.port2].rank_code);
      SendEloCounted(room->members[rep.port1], rep.game, EloOutcome::Different, e1, 0, e2, 0);
      SendEloCounted(room->members[rep.port2], rep.game, EloOutcome::Different, e1, 0, e2, 0);
      return;
    }
    EloCount(*room, rep);
    return;
  }
  if (s_elo_reports.size() >= 512)
    s_elo_reports.erase(s_elo_reports.begin());
  s_elo_reports.push_back(rep);
  if (s_verbose)
    std::printf("elo: room %s: %s: member %d says port %d won (frame %u): waiting for the other\n", rep.room.c_str(),
                rep.game.c_str(), rep.member, rep.winner + 1, rep.frame);
}

// ---- EloTop ----
struct EloTopPlayer
{
  std::string code, nick, country;
  double rating = 0.0;
  int64_t wins = 0, losses = 0;
};
struct EloTopCount
{
  std::vector<EloTopPlayer> players;
  int64_t ranked = 0;
  double at = -1e9;
};
std::map<std::string, EloTopCount> s_elo_top;  // by game

void CountEloTop(const std::string& game, EloTopCount* out)
{
  out->players.clear();
  out->ranked = 0;
  out->at = Now();
  {
    Stmt n(s_stats.db, "SELECT COUNT(*) FROM elo WHERE game = ? AND wins + losses >= ?");
    n.Text(1, game).Int(2, s_elo_min_matches);
    if (n.Row())
      out->ranked = n.ColInt(0);
  }
  Stmt q(s_stats.db, "SELECT e.code, e.rating, e.wins, e.losses, p.nick, p.country FROM elo e LEFT JOIN players p"
                     " ON p.code = e.code WHERE e.game = ? AND e.wins + e.losses >= ?"
                     " ORDER BY e.rating DESC, e.wins DESC, e.code LIMIT ?");
  q.Text(1, game).Int(2, s_elo_min_matches).Int(3, ELO_TOP);
  while (q.Row())
  {
    EloTopPlayer p;
    p.code = q.ColText(0);
    p.rating = sqlite3_column_double(q.s, 1);
    p.wins = q.ColInt(2);
    p.losses = q.ColInt(3);
    p.nick = CleanNick(q.ColText(4));
    p.country = CleanCountry(q.ColText(5));
    out->players.push_back(std::move(p));
  }
}

void OnEloTop(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const std::string game = r.Str(MAX_ELO_GAME);
  const uint8_t* key = r.Bytes(WORLD_KEY_SIZE);
  if (!r.Ok() || !key || !IsEloGame(game) || !state.lists.Take(Now(), LISTS_RATE, LISTS_BURST) || !EloTables())
    return;
  EloTopCount& count = s_elo_top[game];
  if (Now() - count.at > 30.0)
    CountEloTop(game, &count);
  // Our own place (a question about our code only: the answer says nothing of anyone else's).
  const std::string code = ChatCode(key);
  const EloRating mine = EloOf(game, code);
  int64_t position = 0;
  if (mine.wins + mine.losses >= s_elo_min_matches)
  {
    Stmt q(s_stats.db, "SELECT COUNT(*) FROM elo WHERE game = ? AND wins + losses >= ? AND rating > ?");
    q.Text(1, game).Int(2, s_elo_min_matches);
    sqlite3_bind_double(q.s, 3, mine.rating);
    if (q.Row())
      position = q.ColInt(0) + 1;
  }
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::EloList);
  w.U32(nonce);
  w.Str(game, MAX_ELO_GAME);
  w.U16(static_cast<uint16_t>(std::min<int64_t>(count.ranked, 0xFFFF)));
  const size_t count_at = w.Size();
  w.U8(0);
  size_t n = 0;
  const size_t tail = 8;  // (our own place, after the list)
  for (const EloTopPlayer& p : count.players)
  {
    uint8_t one[128];
    Writer o(one, sizeof(one));
    o.Str(p.nick, MAX_NICK);
    o.Str(p.country, COUNTRY_SIZE);
    o.U16(static_cast<uint16_t>(std::clamp(std::lround(p.rating), 0L, 65535L)));
    o.U16(static_cast<uint16_t>(std::min<int64_t>(p.wins, 0xFFFF)));
    o.U16(static_cast<uint16_t>(std::min<int64_t>(p.losses, 0xFFFF)));
    if (!o.Ok() || w.Size() + o.Size() + tail > MAX_PACKET)
      break;
    w.Bytes(one, o.Size());
    n++;
  }
  buf[count_at] = static_cast<uint8_t>(n);
  w.U16(static_cast<uint16_t>(std::min<int64_t>(position, 0xFFFF)));
  w.U16(static_cast<uint16_t>(std::clamp(std::lround(mine.rating), 0L, 65535L)));
  w.U16(static_cast<uint16_t>(std::min<int64_t>(mine.wins, 0xFFFF)));
  w.U16(static_cast<uint16_t>(std::min<int64_t>(mine.losses, 0xFFFF)));
  if (w.Ok())
    Send(from, buf, w.Size());
}
