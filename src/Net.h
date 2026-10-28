// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Net.h (Dolphin Emulator Project, GPL-2.0+).

// The app's network: HTTP and HTTPS through curl, with the PS4's own host name lookup (the C library has no DNS
// configuration on the PS4: every host, those reached through redirects too, is looked up with libSceNet's
// resolver and handed to curl).
//
// HTTPS (00.73): Mbed TLS 2.16.6 with the fix of its RSA arithmetic (scripts/build-mbedtls.sh; before it every RSA
// server failed on the PS4). The server's certificate is always checked - against the certificate bundle
// (Request::ca_file; empty: the app's, /app0/xmb/cacert.pem, Mozilla's roots) and against the host name - and a bad
// one is refused ("the server's certificate isn't valid"). TLS 1.2 (Mbed TLS 2.16 has no 1.3), RSA and ECDSA
// servers. A certificate's dates are checked too, so the PS4's clock must be right. A redirect from https:// to
// http:// is refused; Authorization and Cookie headers aren't sent on to another host.
//
// Every call blocks until the answer is in or the transfer fails: call it from a thread of your own, never on the
// menu's or a game's frame. Calls without a Session may run on several threads at once; a Session is one thread's
// at a time. Nothing here throws: a failure is status 0 and an error.
//
//   Net::Request request;
//   request.url = "https://retroachievements.org/dorequest.php";
//   request.method = "POST";                                    // "GET" (empty: GET), "POST", "HEAD", ...
//   request.body = "r=login2&u=...";                            // the request's body, sent as is
//   request.content_type = "application/x-www-form-urlencoded"; // (the default when there is a body)
//   request.headers = {"Accept: application/json"};             // more headers, "Name: value"
//   request.user_agent = "EmuSuNi/00.73 (PS4) rcheevos/12.5.0";  // empty: "EmuSuNi/<version> (PS4)"
//   request.timeout_seconds = 30;                               // the whole transfer (0: no limit)
//   request.error_body = true;                                  // keep a 4xx/5xx answer's body too
//   const Net::Response answer = Net::Fetch(request);           // the body in answer.body
//   if (!answer.ok())
//     LogLine("... HTTP " + std::to_string(answer.status) + ": " + answer.error);  // answer.plain: for the screen
//
//   request.progress = [](int64_t done, int64_t total) { ...; return true; };  // false stops it ("stopped")
//   Net::Download(request, "/data/EmuSuNi/file.bin");           // the body into a file (through file.bin.part)
//
// Redirects (301, 302, 303, 307, 308) are followed, up to Request::max_redirects: 307 and 308 as they were asked,
// 303 (and 301 or 302 after a POST) as a GET without the body, as browsers do.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Net
{
// A transfer's progress: the answer's body bytes so far and its whole size (-1 while the server hasn't said it).
// Called on the calling thread, at least about once a second while the transfer runs; return false to stop it.
using Progress = std::function<bool(int64_t done, int64_t total)>;

struct Request
{
  std::string url;                   // "http://..." or "https://..."
  std::string method;                // "GET" (also when empty), "POST", "HEAD", "PUT", "DELETE"...
  std::vector<std::string> headers;  // more request headers, "Name: value"
  std::string body;                  // the request's body (POST, PUT...), sent as is
  std::string content_type;          // the body's Content-Type (empty with a body: application/x-www-form-urlencoded)
  std::string user_agent;            // empty: "EmuSuNi/<version> (PS4)"
  std::string ca_file;               // https: the certificate bundle; empty: the app's (/app0/xmb/cacert.pem)
  // The whole transfer's limit, redirects included; 0: none (a big file on a slow connection). Either way a transfer
  // that moves nothing for stall_seconds stops ("the server took too long to answer").
  long timeout_seconds = 30;
  long connect_seconds = 15;  // each connection's
  long stall_seconds = 60;
  int max_redirects = 8;
  bool error_body = false;     // also the body of a 4xx/5xx answer (else only a 2xx answer's body comes)
  size_t max_bytes = 0;        // Fetch: the most body kept in memory (0: 64 MiB); a longer answer fails
  int64_t resume_from = 0;     // Download: <path>.part already has these bytes: asked for the rest (Range)
  Progress progress;           // optional
  std::atomic<bool>* cancel = nullptr;       // optional: set it (from any thread) to stop the transfer
  std::atomic<int64_t>* expected = nullptr;  // optional: the body's size once the server says it (else -1)
};

struct Response
{
  long status = 0;            // the final answer's HTTP status (200, 404...); 0: no answer (error says why)
  std::string body;           // Fetch: the final answer's body (a 4xx/5xx's only with Request::error_body)
  std::string error;          // what failed, in curl's and Mbed TLS's words, for the log; empty when an answer came
  std::string plain;          // when !ok(): why, in plain words for the screen (English, for PS4Lang::T)
  std::string url;            // the final address, after the redirects
  std::vector<std::string> headers;  // the final answer's headers, "Name: value"
  int64_t bytes = 0;          // the body's bytes received (Download: written, resume_from included)
  int64_t size = -1;          // the whole body's size when the server said it, else -1
  int redirects = 0;          // how many were followed
  bool ok() const { return status >= 200 && status < 300 && error.empty(); }
  // A header of the final answer by its name (any case), "" when it isn't there.
  std::string Header(const std::string& name) const;
};

// The body of the final (2xx) answer as it comes (Get, Stream); false stops the transfer.
using Sink = std::function<bool(const char* data, size_t size)>;

// Many requests to the same hosts from one thread: one connection kept open (no new TLS handshake each time) and
// each host looked up once.
class Session
{
public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

private:
  friend Response Stream(const Request&, const Sink&, Session*);
  friend Response Download(const Request&, const std::string&, Session*);
  void* m_curl = nullptr;
  std::vector<std::pair<std::string, std::string>> m_hosts;  // host, address
};

// A request, its answer's body in memory (Response::body, at most Request::max_bytes).
Response Fetch(const Request& request, Session* session = nullptr);
// A request, its answer's body into a file: written to <path>.part, which becomes <path> once the whole body of a
// 2xx answer came (else it stays, for resume_from, unless nothing came). A write that fails (a full drive) stops it:
// error "couldn't write ...", plain "the storage is full or can't be written".
Response Download(const Request& request, const std::string& path, Session* session = nullptr);
// A request, its answer's body handed to sink as it comes (the 2xx answer's; a 4xx/5xx's too with error_body).
Response Stream(const Request& request, const Sink& sink, Session* session = nullptr);

// (Before 00.73.) GET with redirects followed: the final HTTP status, or 0 with *error set.
long Get(const Request& request, const Sink& sink, std::string* error, Session* session = nullptr);
// The whole body as a string (limit: max_bytes).
long GetText(const Request& request, std::string* body, size_t max_bytes, std::string* error,
             Session* session = nullptr);
// "a b&c" -> "a%20b%26c"
std::string UrlEncode(const std::string& text);
// Why a request failed, in plain words for the screen (English, for PS4Lang::T; the details stay in the log): its
// status (0: no answer) and its error. "no internet connection (the PS4 couldn't find the server)", "the server took
// too long to answer", "the server didn't answer", "the connection was cut", "the secure connection failed", "the
// server's certificate isn't valid", "the secure connection failed: check the PS4's date and time", "not on the
// server", "the server refused it", "the server had a problem: try again later", "the storage is full or can't be
// written", "stopped"...
std::string Plain(long status, const std::string& error);
}  // namespace Net
