// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Log reports (00.91): the player sends EmuSuNi's logs to its creator - Settings -> Send Log, or Yes when the app asks
// at the start after it closed by itself - and gets a 6-letter code to write in a report on GitHub or Telegram.
// - What goes: data_dir's emusuni.log (this session), emusuni-1.log (the one before: the one that closed, after a
//   crash) and the end of crash.log (its last 256 KiB: the newest crashes; after them, 01.05, xmb-trace-1.log when the
//   start before this one closed before its log began - a crash before main(): at most 64 KiB, only the program's own
//   start, under "=== xmb-trace-1.log: ..."), with the app's version, made smaller
//   (zlib). First the player's own RetroAchievements user name and online nickname are taken out of them (each becomes
//   "<ra-user>" / "<player>"); the server takes out the rest - the other players' nicknames, addresses, room codes,
//   the PS4's user ids (server/emusuni-reports.py) - and keeps only the country, never the address, for 90 days.
// - Where: EmuSuNi's own server, online play's main server (Netplay::MainServers: netplay.ini, the GitHub list, the
//   built-in address - a new address needs no update), HTTPS on TCP 443: POST /report. Its certificate is its own
//   (scripts/server/make-reports-key.sh): the app trusts its key only (PINS, Net::Request::pinned_key: the server's and a
//   spare's), whatever the address or the PS4's clock. The answer is "code ABCDEF".
// - When to ask (CrashToAsk): crash.log grew since the last start that looked (data_dir/report.ini's crash_seen), or
//   the start watchdog restarted EmuSuNi (src/StartGuard.h). Asked once per crash; sent only with Yes.
// The sending is a thread's; nothing waits for it. Nothing here throws.

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace LogReport
{
enum class State
{
  Idle,
  Sending,
  Sent,    // Status::code
  Failed,  // Status::plain
};
struct Status
{
  State state = State::Idle;
  std::string code;   // the server's code ("ABCDEF")
  std::string plain;  // why it failed, in plain words (English, for PS4Lang::T)
  int serial = 0;     // changes with each new state
};

struct Ask
{
  std::string data_dir;
  std::vector<std::pair<std::string, std::string>> names;  // a name to take out, what it becomes ("<player>")
  std::function<std::string()> server;  // the server's address ("host" or "host:port": the host is taken), on the thread
};
// Sends the logs on a thread (one under way: nothing more).
void Send(const Ask& ask);
Status GetStatus();

// At the menu's start: whether to ask about a crash. The crash counts as asked about once answered (CrashAnswered) or
// once a send succeeded.
bool CrashToAsk(const std::string& data_dir, bool watchdog_restarted);
void CrashAnswered(const std::string& data_dir);

// ---- The pieces the thread runs (the PC's test runs them too: local/herramientas/prueba-reportes) ----
// The request's body: "EMUSUNI-REPORT 1", the version, each file ("file <name> <bytes>" and its bytes), "end"; deflated.
std::string MakeBody(const std::vector<std::pair<std::string, std::string>>& files, const std::string& version);
// The files as sent: those of data_dir there (crash.log's end only), the names taken out.
std::vector<std::pair<std::string, std::string>> GatherFiles(const Ask& ask);
// The code in the server's answer ("code ABCDEF"); "" when there is none.
std::string CodeOf(const std::string& answer);
// Why a send failed, for the screen: the server's status (0: no answer) and curl's error.
std::string FailureText(long status, const std::string& error);
// The address to post to, from the server's ("host:7777" -> "https://host/report"); "" when there is none.
std::string ReportUrl(const std::string& server);
}  // namespace LogReport
