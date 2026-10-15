// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Net.h (Dolphin Emulator Project, GPL-2.0+).

// HTTPS for the menu (box art): curl with the PS4's own host name lookup, since the C library has
// no DNS configuration on the PS4. Every host, including those reached through redirects, is
// looked up with libSceNet's resolver and handed to curl.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Net
{
struct Request
{
  std::string url;
  std::vector<std::string> headers;  // "Name: value"
  std::string ca_file;               // certificate bundle; the check is always on
  long timeout_seconds = 30;
};

// Receives the body of the final (2xx) answer; false stops the transfer.
using Sink = std::function<bool(const char* data, size_t size)>;

// Many requests to the same hosts: one connection kept open (no new TLS handshake each time) and
// each host looked up once.
class Session
{
public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

private:
  friend long Get(const Request&, const Sink&, std::string*, Session*);
  void* m_curl = nullptr;
  std::vector<std::pair<std::string, std::string>> m_hosts;  // host, address
};

// GET with redirects followed. Returns the final HTTP status, or 0 with *error set.
long Get(const Request& request, const Sink& sink, std::string* error, Session* session = nullptr);
// The whole body as a string (limit: max_bytes).
long GetText(const Request& request, std::string* body, size_t max_bytes, std::string* error,
             Session* session = nullptr);
// "a b&c" -> "a%20b%26c"
std::string UrlEncode(const std::string& text);
}  // namespace Net
