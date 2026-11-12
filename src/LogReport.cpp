// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "LogReport.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <pthread.h>
#include <sys/stat.h>
#include <zlib.h>

#include "Log.h"
#include "Net.h"

namespace LogReport
{
namespace
{
// The server's key and the spare one (scripts/server/make-reports-key.sh: their pin.txt). The private keys are the
// owner's: never in the app or on GitHub.
constexpr const char* PINS = "sha256//BXVkfZDFebd6hqe5Jgbv7Wo6gSd/Dk/vshDpzguwF4U=;"
                             "sha256//+jlb1ZDt+H3q/THrRGj+OZ6oUSNvpa/RqtyDnVeZYNg=";
constexpr size_t LOG_MOST = 4 << 20;     // a log's end at most (they are ~0.3 MB)
constexpr size_t CRASH_MOST = 256 << 10;  // crash.log's end (it grows with each crash)
constexpr size_t TRACE_MOST = 64 << 10;   // (01.05) a start's trace before its log (~2 KB)
constexpr const char* LOGS[] = {"emusuni.log", "emusuni-1.log", "crash.log"};
// (01.05) The heading of the start's trace in crash.log's part (EarlyTrace).
constexpr const char* TRACE_HEADING = "\n=== xmb-trace-1.log: the start before this one, closed before its log began\n";

std::mutex s_mutex;  // the status, and report.ini
Status s_status;
bool s_busy = false;

int64_t SizeOf(const std::string& path)
{
  struct stat st = {};
  return stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : 0;
}

// A file's end: at most `most` bytes, from the start of a line.
std::string Tail(const std::string& path, size_t most)
{
  std::string text = ReadTextFile(path);
  if (text.size() > most)
  {
    size_t from = text.size() - most;
    const size_t line = text.find('\n', from);
    from = line == std::string::npos ? from : line + 1;
    text = "[... the start left out: " + std::to_string(from) + " bytes]\n" + text.substr(from);
  }
  return text;
}

void Replace(std::string* text, const std::string& name, const std::string& label)
{
  if (name.size() < 3)
    return;  // (a name of one or two letters would hit everything)
  for (size_t at = text->find(name); at != std::string::npos; at = text->find(name, at + label.size()))
    text->replace(at, name.size(), label);
}

// (01.05) xmb-trace-1.log, the trace of the start before this one (port/ps4_crashlog.cpp: each start of the XMB moves
// xmb-trace.log there), when that start ended before main() began its log - a crash in a static constructor (the v00.23
// reports CXNNDN, U7USF6, FMC33M, QEREZH: ParaLLEl-RSP's JIT without executable memory) has a crash.log entry and no
// log of its own; its trace says what the start did: the heap's and the mmaps' lines. Empty otherwise: a start that
// reached main() has its log (emusuni-1.log), and its trace (the log's lines once more) isn't sent. A trace from before
// main() holds only the program's own start - memory sizes, addresses in the program - never the player's name, an
// address on the network or a room's code: those come with the log's lines, all after main()'s first one.
std::string EarlyTrace(const std::string& data_dir)
{
  const std::string path = data_dir + "/xmb-trace-1.log";
  if (!FileExists(path))
    return {};
  const std::string text = Tail(path, TRACE_MOST);
  // main()'s first line ("[   0.120] EmuSuNi 01.05 starting", src/main.cpp): the start reached its log.
  if (text.find(std::string("] ") + App::NAME + " ") != std::string::npos ||
      text.find_first_not_of(" \n") == std::string::npos)
    return {};
  return text;
}

// report.ini's crash_seen: crash.log's size when it was last looked at (-1: never).
int64_t CrashSeen(const std::string& data_dir)
{
  const std::string text = ReadTextFile(data_dir + "/report.ini");
  const size_t at = text.find("crash_seen=");
  return at == std::string::npos ? -1 : std::atoll(text.c_str() + at + 11);
}

void SetCrashSeen(const std::string& data_dir, int64_t size)
{
  WriteTextFile(data_dir + "/report.ini", "# EmuSuNi: crash.log's size when the player was last asked about a crash "
                                          "(src/LogReport.h)\ncrash_seen=" + std::to_string(size) + "\n");
}

void SetStatus(State state, const std::string& code, const std::string& plain)
{
  std::lock_guard lock(s_mutex);
  s_status.state = state;
  s_status.code = code;
  s_status.plain = plain;
  s_status.serial++;
}

void Run(Ask ask)
{
  const std::string url = ReportUrl(ask.server ? ask.server() : std::string());
  if (url.empty())
  {
    LogLine("report: no server known: nothing sent");
    SetStatus(State::Failed, "", "EmuSuNi's server isn't known: check the PS4's internet connection.");
  }
  else
  {
    const auto files = GatherFiles(ask);
    size_t bytes = 0;
    for (const auto& [name, text] : files)
      bytes += text.size();
    Net::Request request;
    request.url = url;
    request.method = "POST";
    request.body = MakeBody(files, App::VERSION);
    request.content_type = "application/octet-stream";
    request.headers = {std::string("X-EmuSuNi-Version: ") + App::VERSION};
    request.pinned_key = PINS;
    request.timeout_seconds = 90;
    request.connect_seconds = 10;
    request.stall_seconds = 30;
    request.error_body = true;
    request.max_bytes = 4096;
    LogF("report: sending %zu file(s), %zu KB (%zu KB made smaller)", files.size(), bytes / 1024,
         request.body.size() / 1024);
    const Net::Response answer = Net::Fetch(request);
    const std::string code = answer.ok() ? CodeOf(answer.body) : std::string();
    if (!code.empty())
    {
      LogLine("report: sent, its code " + code);
      {
        std::lock_guard lock(s_mutex);
        SetCrashSeen(ask.data_dir, SizeOf(ask.data_dir + "/crash.log"));
      }
      SetStatus(State::Sent, code, "");
    }
    else
    {
      LogF("report: not sent (HTTP %ld%s%s)", answer.status, answer.error.empty() ? "" : ": ", answer.error.c_str());
      SetStatus(State::Failed, "", FailureText(answer.ok() ? 500 : answer.status, answer.error));
    }
  }
  std::lock_guard lock(s_mutex);
  s_busy = false;
}
}  // namespace

void Send(const Ask& ask)
{
  {
    std::lock_guard lock(s_mutex);
    if (s_busy)
      return;
    s_busy = true;
  }
  SetStatus(State::Sending, "", "");
  // (pthread, not std::thread: a thread that can't start throws there, and an exception closes the app on the PS4.)
  Ask* copy = new Ask(ask);
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  const int started = pthread_create(
      &thread, &attr,
      [](void* arg) -> void* {
        Ask* ask = static_cast<Ask*>(arg);
        Run(std::move(*ask));
        delete ask;
        return nullptr;
      },
      copy);
  pthread_attr_destroy(&attr);
  if (started != 0)
  {
    delete copy;
    LogLine("report: the thread couldn't start: nothing sent");
    SetStatus(State::Failed, "", "EmuSuNi's server didn't answer: try again later.");
    std::lock_guard lock(s_mutex);
    s_busy = false;
  }
}

Status GetStatus()
{
  std::lock_guard lock(s_mutex);
  return s_status;
}

bool CrashToAsk(const std::string& data_dir, bool watchdog_restarted)
{
  std::lock_guard lock(s_mutex);
  const int64_t size = SizeOf(data_dir + "/crash.log"), seen = CrashSeen(data_dir);
  if (seen < 0 || size < seen)
    SetCrashSeen(data_dir, size);  // (the first start that looks: the crashes of before aren't asked about)
  const bool ask = watchdog_restarted || (seen >= 0 && size > seen);
  if (ask)
    LogF("report: EmuSuNi closed by itself last time (%s): the player is asked to send the log",
         watchdog_restarted ? "the start watchdog restarted it" : "crash.log grew");
  return ask;
}

void CrashAnswered(const std::string& data_dir)
{
  std::lock_guard lock(s_mutex);
  SetCrashSeen(data_dir, SizeOf(data_dir + "/crash.log"));
}

std::string MakeBody(const std::vector<std::pair<std::string, std::string>>& files, const std::string& version)
{
  std::string raw = "EMUSUNI-REPORT 1\nversion " + version + "\n";
  for (const auto& [name, text] : files)
    raw += "file " + name + " " + std::to_string(text.size()) + "\n" + text + "\n";
  raw += "end\n";
  uLongf size = compressBound(static_cast<uLong>(raw.size()));
  std::string body(size, '\0');
  if (compress2(reinterpret_cast<Bytef*>(&body[0]), &size, reinterpret_cast<const Bytef*>(raw.data()),
                static_cast<uLong>(raw.size()), 9) != Z_OK)
    return {};
  body.resize(size);
  return body;
}

std::vector<std::pair<std::string, std::string>> GatherFiles(const Ask& ask)
{
  std::vector<std::pair<std::string, std::string>> files;
  for (const char* name : LOGS)
  {
    const std::string path = ask.data_dir + "/" + name;
    if (!FileExists(path))
      continue;
    const bool crash = std::string(name) == "crash.log";
    std::string text = Tail(path, crash ? CRASH_MOST : LOG_MOST);
    // (01.05) A start that closed before its log: its trace after crash.log's entries, in that file's part - the
    // server takes only these three files (server/emusuni-reports.py's NAMES), and a fourth would lose the report.
    if (crash)
    {
      const std::string trace = EarlyTrace(ask.data_dir);
      if (!trace.empty())
        text += std::string(TRACE_HEADING) + trace;
    }
    for (const auto& [taken, label] : ask.names)
      Replace(&text, taken, label);
    files.emplace_back(name, std::move(text));
  }
  return files;
}

std::string CodeOf(const std::string& answer)
{
  const size_t at = answer.find("code ");
  if (at == std::string::npos || answer.size() < at + 11)
    return {};
  const std::string code = answer.substr(at + 5, 6);
  for (char c : code)
  {
    if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9')))
      return {};
  }
  return code;
}

std::string FailureText(long status, const std::string& error)
{
  if (error.find("pinned") != std::string::npos || error.find("public key") != std::string::npos)
    return "The server didn't prove it is EmuSuNi's: nothing was sent.";
  if (status == 429)
    return "Many logs were sent from this connection today: try again tomorrow.";
  if (status == 413)
    return "The log is too big to send.";
  if (status == 507 || status == 503)
    return "EmuSuNi's server is full right now: try again later.";
  if (status >= 400 && status < 500)
    return "EmuSuNi's server didn't take it.";
  if (status >= 500)
    return "EmuSuNi's server had a problem: try again later.";
  if (error.find("look up") != std::string::npos || error.find("network is not") != std::string::npos)
    return "No internet connection: check the PS4's network.";
  return "EmuSuNi's server didn't answer: try again later.";
}

std::string ReportUrl(const std::string& server)
{
  std::string host = server;
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos)
    host.erase(colon);
  if (host.empty() || host.find_first_of("/ []") != std::string::npos)
    return {};
  return "https://" + host + "/report";
}
}  // namespace LogReport
