// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// emusuni-netd's statistics store (00.23): SQLite (the system's libsqlite3), one file - <stats-dir>/stats.db - in WAL
// mode (the server writes while report, public-json, public-profiles and record-downloads read). Part of
// emusuni-netd.cpp: included by emusuni-netd-stats.h after its Day and Label.
//
//   meta(key, value)                     salts, today's date, live and rooms snapshots, export marks
//   day(date, ...counts, sketch)         a day's counts (UTC) and its HyperLogLog sketch
//   day_label(date, kind, value, installs, beats, sketch)   by country, version, language, state, console, game
//   day_update(date, from_version, to_version, installs)
//   seen(key, consoles, states, checks, updated, games)     TODAY's installs, hashed with today's salt: emptied with
//                                                           the day (it only stops an install counting twice)
//   downloads(date, tag, total)          GitHub's .pkg downloads, a row a release a day
//   profile(id, code, ...)               opt-in online profiles only (Settings -> Online Play -> Online Profile):
//   profile_game, profile_console        their minutes, sessions, online matches - keyed by the install id hashed
//                                        with a salt of this server's; deleted with "Delete My Profile"
//   pub_day, pub_day_label, pub_day_update, pub_seen   (00.95) the same, counted only from the versions published on
//                                        GitHub (meta "published", record-downloads): the public page's (public-json)
// A server of the text files before (day-*.txt, downloads.txt, today.bin, salt) moves them in at its first start
// (StoreMigrateFiles) and keeps them in migrated-files/.

sqlite3* StoreOpen(const std::string& dir)
{
  mkdir(dir.c_str(), 0750);
  sqlite3* db = nullptr;
  if (sqlite3_open_v2((dir + "/stats.db").c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK)
  {
    std::fprintf(stderr, "stats: %s/stats.db can't be opened: %s\n", dir.c_str(), db ? sqlite3_errmsg(db) : "?");
    sqlite3_close(db);
    return nullptr;
  }
  sqlite3_busy_timeout(db, 5000);
  const char* schema =
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=NORMAL;"
      "PRAGMA cache_size=-8000;"  // (8 MB: a 1 GB VM)
      "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
      "CREATE TABLE IF NOT EXISTS day(date TEXT PRIMARY KEY, active INT, fresh INT, heartbeats INT, update_checks INT,"
      " uncounted INT, peak INT, peak_at TEXT, sketch TEXT);"
      "CREATE TABLE IF NOT EXISTS day_label(date TEXT, kind TEXT, value TEXT, installs INT, beats INT, sketch TEXT,"
      " PRIMARY KEY(date, kind, value)) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS day_update(date TEXT, from_version TEXT, to_version TEXT, installs INT,"
      " PRIMARY KEY(date, from_version, to_version)) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS seen(key INTEGER PRIMARY KEY, consoles INT, states INT, checks INT, updated INT,"
      " games TEXT);"
      "CREATE TABLE IF NOT EXISTS downloads(date TEXT, tag TEXT, total INT, PRIMARY KEY(date, tag)) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS profile(id BLOB PRIMARY KEY, code TEXT UNIQUE, public INT, nick TEXT, country TEXT,"
      " created INT, last_seen INT, last_beat INT, last_game TEXT, last_state INT, minutes INT, sessions INT,"
      " online_matches INT);"
      "CREATE TABLE IF NOT EXISTS profile_game(profile BLOB, game TEXT, console TEXT, minutes INT, sessions INT,"
      " online_matches INT, last_played INT, PRIMARY KEY(profile, game)) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS profile_console(profile BLOB, console TEXT, minutes INT,"
      " PRIMARY KEY(profile, console)) WITHOUT ROWID;"
      // (00.95) The published versions' own counts.
      "CREATE TABLE IF NOT EXISTS pub_day(date TEXT PRIMARY KEY, active INT, fresh INT, heartbeats INT,"
      " update_checks INT, uncounted INT, peak INT, peak_at TEXT, sketch TEXT);"
      "CREATE TABLE IF NOT EXISTS pub_day_label(date TEXT, kind TEXT, value TEXT, installs INT, beats INT, sketch TEXT,"
      " PRIMARY KEY(date, kind, value)) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS pub_day_update(date TEXT, from_version TEXT, to_version TEXT, installs INT,"
      " PRIMARY KEY(date, from_version, to_version)) WITHOUT ROWID;"
      "CREATE TABLE IF NOT EXISTS pub_seen(key INTEGER PRIMARY KEY, consoles INT, states INT, checks INT, updated INT,"
      " games TEXT);";
  char* error = nullptr;
  if (sqlite3_exec(db, schema, nullptr, nullptr, &error) != SQLITE_OK)
  {
    std::fprintf(stderr, "stats: stats.db's tables: %s\n", error ? error : "?");
    sqlite3_free(error);
    sqlite3_close(db);
    return nullptr;
  }
  // (00.95) A profile's last version (the public page shows only the published versions'): a column more for a
  // stats.db of before.
  bool has_version = false;
  {
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(profile)", -1, &q, nullptr) == SQLITE_OK)
    {
      while (sqlite3_step(q) == SQLITE_ROW)
      {
        const unsigned char* name = sqlite3_column_text(q, 1);
        has_version = has_version || (name && std::strcmp(reinterpret_cast<const char*>(name), "version") == 0);
      }
    }
    sqlite3_finalize(q);
  }
  if (!has_version)
    sqlite3_exec(db, "ALTER TABLE profile ADD COLUMN version TEXT DEFAULT ''", nullptr, nullptr, nullptr);
  return db;
}

// A statement, finalized when it goes.
struct Stmt
{
  sqlite3_stmt* s = nullptr;
  Stmt(sqlite3* db, const char* sql)
  {
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
    {
      std::fprintf(stderr, "stats: %s: %s\n", sql, sqlite3_errmsg(db));
      s = nullptr;
    }
  }
  ~Stmt() { sqlite3_finalize(s); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  Stmt& Text(int i, const std::string& v)
  {
    if (s)
      sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& Int(int i, int64_t v)
  {
    if (s)
      sqlite3_bind_int64(s, i, v);
    return *this;
  }
  Stmt& Blob(int i, const void* p, int n)
  {
    if (s)
      sqlite3_bind_blob(s, i, p, n, SQLITE_TRANSIENT);
    return *this;
  }
  bool Row() { return s && sqlite3_step(s) == SQLITE_ROW; }
  bool Run()
  {
    if (!s)
      return false;
    const int r = sqlite3_step(s);
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return r == SQLITE_DONE || r == SQLITE_ROW;
  }
  void Reset()
  {
    if (s)
    {
      sqlite3_reset(s);
      sqlite3_clear_bindings(s);
    }
  }
  int64_t ColInt(int i) { return sqlite3_column_int64(s, i); }
  std::string ColText(int i)
  {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? reinterpret_cast<const char*>(t) : "";
  }
  bool ColNull(int i) { return sqlite3_column_type(s, i) == SQLITE_NULL; }
};

void StoreExec(sqlite3* db, const char* sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK)
  {
    std::fprintf(stderr, "stats: %s: %s\n", sql, error ? error : "?");
    sqlite3_free(error);
  }
}

std::string StoreMeta(sqlite3* db, const std::string& key)
{
  Stmt q(db, "SELECT value FROM meta WHERE key = ?");
  q.Text(1, key);
  return q.Row() ? q.ColText(0) : std::string();
}

void StoreSetMeta(sqlite3* db, const std::string& key, const std::string& value)
{
  Stmt q(db, "INSERT INTO meta(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  q.Text(1, key).Text(2, value).Run();
}

// A day's counts, whole (its labels and updates replaced). (00.95) prefix "pub_": the published versions' tables.
void StoreSaveDay(sqlite3* db, const Day& d, const std::string& prefix = "")
{
  {
    Stmt q(db, ("INSERT OR REPLACE INTO " + prefix + "day VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)").c_str());
    q.Text(1, d.date).Int(2, static_cast<int64_t>(d.active)).Int(3, static_cast<int64_t>(d.fresh))
        .Int(4, static_cast<int64_t>(d.heartbeats)).Int(5, static_cast<int64_t>(d.update_checks))
        .Int(6, static_cast<int64_t>(d.uncounted)).Int(7, static_cast<int64_t>(d.peak)).Text(8, d.peak_at)
        .Text(9, d.all.Text()).Run();
  }
  Stmt clear_labels(db, ("DELETE FROM " + prefix + "day_label WHERE date = ?").c_str());
  clear_labels.Text(1, d.date).Run();
  Stmt label(db, ("INSERT INTO " + prefix + "day_label VALUES(?, ?, ?, ?, ?, ?)").c_str());
  for (const auto& [kind, values] : d.labels)
  {
    for (const auto& [value, l] : values)
      label.Text(1, d.date).Text(2, kind).Text(3, value).Int(4, static_cast<int64_t>(l.installs))
          .Int(5, static_cast<int64_t>(l.beats)).Text(6, l.sketch.Text()).Run();
  }
  Stmt clear_updates(db, ("DELETE FROM " + prefix + "day_update WHERE date = ?").c_str());
  clear_updates.Text(1, d.date).Run();
  Stmt update(db, ("INSERT INTO " + prefix + "day_update VALUES(?, ?, ?, ?)").c_str());
  for (const auto& [pair, n] : d.updates)
    update.Text(1, d.date).Text(2, pair.first).Text(3, pair.second).Int(4, static_cast<int64_t>(n)).Run();
}

bool StoreLoadDay(sqlite3* db, const std::string& date, Day* d, const std::string& prefix = "")
{
  *d = Day();
  d->date = date;
  Stmt q(db, ("SELECT active, fresh, heartbeats, update_checks, uncounted, peak, peak_at, sketch FROM " + prefix +
              "day WHERE date = ?").c_str());
  q.Text(1, date);
  if (!q.Row())
    return false;
  d->active = static_cast<uint64_t>(q.ColInt(0));
  d->fresh = static_cast<uint64_t>(q.ColInt(1));
  d->heartbeats = static_cast<uint64_t>(q.ColInt(2));
  d->update_checks = static_cast<uint64_t>(q.ColInt(3));
  d->uncounted = static_cast<uint64_t>(q.ColInt(4));
  d->peak = static_cast<uint64_t>(q.ColInt(5));
  d->peak_at = q.ColText(6);
  d->all.FromText(q.ColText(7));
  Stmt labels(db, ("SELECT kind, value, installs, beats, sketch FROM " + prefix + "day_label WHERE date = ?").c_str());
  labels.Text(1, date);
  while (labels.Row())
  {
    const std::string kind = labels.ColText(0);
    Label l(SketchOf(kind));
    l.installs = static_cast<uint64_t>(labels.ColInt(2));
    l.beats = static_cast<uint64_t>(labels.ColInt(3));
    l.sketch.FromText(labels.ColText(4));
    d->labels[kind].insert_or_assign(labels.ColText(1), l);
  }
  Stmt updates(db, ("SELECT from_version, to_version, installs FROM " + prefix + "day_update WHERE date = ?").c_str());
  updates.Text(1, date);
  while (updates.Row())
    d->updates[{updates.ColText(0), updates.ColText(1)}] = static_cast<uint64_t>(updates.ColInt(2));
  return true;
}

// GitHub's downloads as downloads.txt had them: "date tag total" a line.
std::string StoreDownloadsText(sqlite3* db)
{
  std::string out;
  Stmt q(db, "SELECT date, tag, total FROM downloads ORDER BY date, tag");
  while (q.Row())
    out += q.ColText(0) + " " + q.ColText(1) + " " + std::to_string(q.ColInt(2)) + "\n";
  return out;
}

// ---- The opt-in profiles ----
// A profile code: 16 of the room codes' letters (no 0/O, 1/I/L), 4 by 4 - about 79 bits: not guessed.
std::string NewProfileCode()
{
  std::string code;
  for (int i = 0; i < 16; i++)
  {
    if (i > 0 && i % 4 == 0)
      code += '-';
    code += CODE_ALPHABET[Random64() % (sizeof(CODE_ALPHABET) - 1)];
  }
  return code;
}

// A code as typed: capitals, the dashes put back ("" when it isn't one).
std::string NormalizeProfileCode(const std::string& typed)
{
  std::string letters;
  for (const char c : typed)
  {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (std::strchr(CODE_ALPHABET, u) && u != 0)
      letters += u;
    else if (c != '-' && c != ' ')
      return {};
  }
  if (letters.size() != 16)
    return {};
  return letters.substr(0, 4) + "-" + letters.substr(4, 4) + "-" + letters.substr(8, 4) + "-" + letters.substr(12, 4);
}

void StoreDeleteProfile(sqlite3* db, const void* id, int n)
{
  StoreExec(db, "BEGIN");
  for (const char* sql : {"DELETE FROM profile_game WHERE profile = ?", "DELETE FROM profile_console WHERE profile = ?",
                          "DELETE FROM profile WHERE id = ?"})
  {
    Stmt q(db, sql);
    q.Blob(1, id, n).Run();
  }
  StoreExec(db, "COMMIT");
}
