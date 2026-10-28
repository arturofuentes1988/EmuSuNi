// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Net.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "Net.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>
#include <orbis/Sysmodule.h>

#include "Log.h"

// libSceNet, declared here: the 2022 OpenOrbis headers give the resolver no prototypes.
extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetResolverCreate(const char* name, int memid, int flags);
int sceNetResolverDestroy(int rid);
int sceNetResolverStartNtoa(int rid, const char* hostname, uint32_t* addr, int timeout, int retry,
                            int flags);
}

namespace Net
{
namespace
{
using Clock = std::chrono::steady_clock;

std::mutex s_init_mutex;
int s_pool = -1;
constexpr size_t DEFAULT_MAX_BYTES = size_t{64} << 20;
constexpr size_t ERROR_BODY_MAX = 64 << 10;  // Download: an error page's body kept in Response::body

void Log(const std::string& text)
{
  LogLine("net: " + text);
}

bool Init(std::string* error)
{
  std::lock_guard lock(s_init_mutex);
  if (s_pool >= 0)
    return true;
  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
  const int init = sceNetInit();
  s_pool = sceNetPoolCreate("emusuni-net", 64 * 1024, 0);
  char line[160];
  snprintf(line, sizeof(line), "init %#x, pool %#x; %s", static_cast<unsigned>(init), static_cast<unsigned>(s_pool),
           curl_version());
  Log(line);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  if (s_pool < 0)
  {
    *error = "the PS4 network is not available";
    return false;
  }
  return true;
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

std::string Upper(std::string text)
{
  for (char& c : text)
  {
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  }
  return text;
}

// "1.2.3.4": no lookup needed.
bool IsAddress(const std::string& host)
{
  int dots = 0;
  for (const char c : host)
  {
    if (c == '.')
      dots++;
    else if (c < '0' || c > '9')
      return false;
  }
  return dots == 3;
}

std::string Resolve(const std::string& host, std::string* error)
{
  if (IsAddress(host))
    return host;
  const int resolver = sceNetResolverCreate("emusuni-net", s_pool, 0);
  if (resolver < 0)
  {
    *error = "the PS4 network is not available";
    return {};
  }
  uint32_t address = 0;
  const int result = sceNetResolverStartNtoa(resolver, host.c_str(), &address, 5 * 1000 * 1000, 2, 0);
  sceNetResolverDestroy(resolver);
  if (result < 0)
  {
    char line[160];
    snprintf(line, sizeof(line), "could not look up %s (%#x): is the PS4 online?", host.c_str(),
             static_cast<unsigned>(result));
    *error = line;
    return {};
  }
  char text[32];
  snprintf(text, sizeof(text), "%u.%u.%u.%u", address & 0xFF, (address >> 8) & 0xFF,
           (address >> 16) & 0xFF, (address >> 24) & 0xFF);
  return text;
}

// "https://host[:port]/path" -> "https", host, port. Only http and https.
bool SplitUrl(const std::string& url, std::string* scheme, std::string* host, int* port)
{
  const size_t colon = url.find("://");
  if (colon == std::string::npos)
    return false;
  *scheme = Lower(url.substr(0, colon));
  if (*scheme != "http" && *scheme != "https")
    return false;
  const size_t start = colon + 3;
  const size_t end = url.find_first_of("/?#", start);
  std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  if (const size_t at = authority.rfind('@'); at != std::string::npos)
    authority = authority.substr(at + 1);  // (user:password@: not used, never looked up)
  *port = *scheme == "https" ? 443 : 80;
  const size_t port_colon = authority.rfind(':');
  if (port_colon != std::string::npos)
  {
    const std::string digits = authority.substr(port_colon + 1);
    char* stop = nullptr;
    const long value = std::strtol(digits.c_str(), &stop, 10);
    if (digits.empty() || !stop || *stop != '\0' || value <= 0 || value > 65535)
      return false;
    *port = static_cast<int>(value);
    authority = authority.substr(0, port_colon);
  }
  *host = Lower(authority);
  return !host->empty() && host->find_first_of("[]") == std::string::npos;
}

// "Name: value" -> "name".
std::string HeaderName(const std::string& header)
{
  const size_t colon = header.find(':');
  std::string name = colon == std::string::npos ? header : header.substr(0, colon);
  while (!name.empty() && name.back() == ' ')
    name.pop_back();
  return Lower(name);
}

// What the final answer's body goes to.
struct Target
{
  // The body starts: its status and the place of its first byte (a 206's Content-Range, else 0); false stops it.
  std::function<bool(long status, int64_t offset)> begin;
  std::function<bool(const char* data, size_t size)> data;
};

struct Transfer
{
  CURL* curl = nullptr;
  const Request* request = nullptr;
  const Target* target = nullptr;
  bool begun = false;      // the first write of this hop
  bool keep = false;       // this hop's body goes to the target
  bool stopped = false;    // the target, the progress or the cancel flag said stop
  long status = 0;
  int64_t offset = 0;      // the body's first byte's place in the whole (206)
  int64_t done = 0;        // this hop's body bytes
  int64_t total = -1;      // the whole body's size, when known
  std::vector<std::string> headers;  // this hop's answer's
  Clock::time_point last_progress{};
};

int64_t ContentRangeStart(const std::vector<std::string>& headers, int64_t* total)
{
  for (const std::string& header : headers)
  {
    if (HeaderName(header) != "content-range")
      continue;
    // "Content-Range: bytes 1000-1999/2000"
    const size_t bytes = header.find("bytes");
    if (bytes == std::string::npos)
      return -1;
    const char* p = header.c_str() + bytes + 5;
    char* stop = nullptr;
    const long long start = std::strtoll(p, &stop, 10);
    if (stop == p || *stop != '-')
      return -1;
    const char* slash = std::strchr(stop, '/');
    if (slash && slash[1] != '*')
    {
      char* end = nullptr;
      const long long whole = std::strtoll(slash + 1, &end, 10);
      if (end != slash + 1 && whole > 0)
        *total = whole;
    }
    return start;
  }
  return -1;
}

void Begin(Transfer* t)
{
  t->begun = true;
  curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &t->status);
  const bool redirect = t->status >= 300 && t->status < 400;
  t->keep = !redirect && ((t->status >= 200 && t->status < 300) || (t->request->error_body && t->status >= 400));
  curl_off_t length = -1;
  curl_easy_getinfo(t->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
  t->offset = 0;
  t->total = length;
  if (t->status == 206)
  {
    int64_t whole = -1;
    const int64_t start = ContentRangeStart(t->headers, &whole);
    t->offset = start > 0 ? start : 0;
    t->total = whole > 0 ? whole : (length >= 0 ? t->offset + length : -1);
  }
  if (!t->keep)
    return;
  if (t->status >= 200 && t->status < 300 && t->request->expected)
    *t->request->expected = t->total;
  if (t->target->begin && !t->target->begin(t->status, t->offset))
    t->stopped = true;
}

size_t Write(char* data, size_t size, size_t count, void* user)
{
  auto* t = static_cast<Transfer*>(user);
  const size_t bytes = size * count;
  if (!t->begun)
    Begin(t);
  if (t->stopped)
    return 0;
  if (!t->keep)
    return bytes;  // a redirect's or an error page's body
  if (t->target->data && !t->target->data(data, bytes))
  {
    t->stopped = true;
    return 0;
  }
  t->done += static_cast<int64_t>(bytes);
  return bytes;
}

size_t Header(char* data, size_t size, size_t count, void* user)
{
  auto* t = static_cast<Transfer*>(user);
  const size_t bytes = size * count;
  std::string line(data, bytes);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  if (line.compare(0, 5, "HTTP/") == 0)
    t->headers.clear();  // a new answer (an interim 100's headers are dropped)
  else if (!line.empty())
    t->headers.push_back(line);
  return bytes;
}

int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  auto* t = static_cast<Transfer*>(user);
  if (t->stopped)
    return 1;
  if (t->request->cancel && t->request->cancel->load())
  {
    t->stopped = true;
    return 1;
  }
  if (!t->request->progress)
    return 0;
  const Clock::time_point now = Clock::now();
  if (now - t->last_progress < std::chrono::milliseconds(100))
    return 0;
  t->last_progress = now;
  const int64_t done = t->keep ? t->offset + t->done : 0;
  if (!t->request->progress(done, t->keep ? t->total : -1))
  {
    t->stopped = true;
    return 1;
  }
  return 0;
}

std::string DefaultUserAgent()
{
  return std::string("EmuSuNi/") + App::VERSION + " (PS4)";
}

std::string DefaultCaFile()
{
  return std::string(App::ASSETS_DIR) + "/xmb/cacert.pem";
}

// The request done, redirects followed; the final answer's body to the target.
Response Perform(const Request& request, const Target& target, Session* session, void** session_curl,
                 std::vector<std::pair<std::string, std::string>>* session_hosts)
{
  Response response;
  auto fail = [&response](const std::string& error) {
    response.error = error;
    response.plain = Plain(response.status, error);
    return response;
  };
  {
    std::string error;
    if (!Init(&error))
      return fail(error);
  }
  std::string url = request.url;
  std::string method = request.method.empty() ? (request.body.empty() ? "GET" : "POST") : Upper(request.method);
  bool with_body = method != "GET" && method != "HEAD";
  std::string first_host;
  const Clock::time_point start = Clock::now();
  for (int hop = 0;; hop++)
  {
    if (hop > std::max(0, request.max_redirects))
      return fail("too many redirects");
    std::string scheme, host;
    int port = 0;
    if (!SplitUrl(url, &scheme, &host, &port))
      return fail("bad address: " + url);
    if (hop == 0)
      first_host = host;
    std::string address, error;
    if (session_hosts)
    {
      for (const auto& [known, known_address] : *session_hosts)
      {
        if (known == host)
          address = known_address;
      }
    }
    if (address.empty())
    {
      address = Resolve(host, &error);
      if (address.empty())
        return fail(error);  // (not logged here: offline, every cover would say it)
      if (session_hosts)
        session_hosts->emplace_back(host, address);
    }
    long timeout_ms = 0;
    if (request.timeout_seconds > 0)
    {
      const long long spent =
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
      timeout_ms = static_cast<long>(request.timeout_seconds * 1000 - spent);
      if (timeout_ms <= 0)
        return fail("Timeout was reached (" + std::to_string(request.timeout_seconds) + " s)");
    }
    CURL* curl = session_curl && *session_curl ? static_cast<CURL*>(*session_curl) : curl_easy_init();
    if (!curl)
      return fail("curl couldn't start");
    if (session_curl)
    {
      if (*session_curl)
        curl_easy_reset(curl);  // (the connection, its TLS session and the hosts stay)
      *session_curl = curl;
    }
    Transfer transfer;
    transfer.curl = curl;
    transfer.request = &request;
    transfer.target = &target;
    const std::string resolve = host + ":" + std::to_string(port) + ":" + address;
    curl_slist* resolves = curl_slist_append(nullptr, resolve.c_str());
    curl_slist* headers = nullptr;
    bool has_type = false;
    for (const std::string& header : request.headers)
    {
      const std::string name = HeaderName(header);
      if (host != first_host && (name == "authorization" || name == "cookie"))
        continue;  // (not sent on to another host)
      has_type = has_type || name == "content-type";
      headers = curl_slist_append(headers, header.c_str());
    }
    if (with_body)
    {
      if (!has_type)
      {
        const std::string type = "Content-Type: " + (request.content_type.empty() ?
                                                         std::string("application/x-www-form-urlencoded") :
                                                         request.content_type);
        headers = curl_slist_append(headers, type.c_str());
      }
      headers = curl_slist_append(headers, "Expect:");  // (no 100-continue wait)
    }
    const std::string agent = request.user_agent.empty() ? DefaultUserAgent() : request.user_agent;
    const std::string ca = request.ca_file.empty() ? DefaultCaFile() : request.ca_file;
    const std::string range = request.resume_from > 0 ? std::to_string(request.resume_from) + "-" : std::string();
    char detail[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolves);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);  // each hop is looked up here
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::max(1L, request.connect_seconds));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, std::max(1L, request.stall_seconds));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, detail);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, Header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    if (method == "GET")
    {
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    else if (method == "HEAD")
    {
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    else
    {
      if (method == "POST")
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
      else
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
    }
    if (!range.empty())
      curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char* location = nullptr;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
    const std::string next = location ? location : "";
    if (code == CURLE_OK && !transfer.begun)
      Begin(&transfer);  // (an answer without a body: its status, size and headers)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_RESOLVE, nullptr);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, nullptr);
    curl_slist_free_all(headers);
    curl_slist_free_all(resolves);
    if (!session_curl)
      curl_easy_cleanup(curl);
    response.status = status;
    response.url = url;
    response.headers = std::move(transfer.headers);
    response.redirects = hop;
    if (transfer.keep)
    {
      response.bytes = transfer.offset + transfer.done;
      response.size = transfer.total;
    }
    if (transfer.stopped)
    {
      response.status = 0;
      return fail("cancelled");
    }
    if (code != CURLE_OK)
    {
      std::string error = std::string(curl_easy_strerror(code)) + (detail[0] ? std::string(": ") + detail : "");
      Log(host + ": " + error);
      response.status = 0;
      return fail(error);
    }
    if (transfer.keep && request.progress)
      request.progress(response.bytes, response.size);  // (the last call: all of it; too late to stop)
    if (status >= 300 && status < 400 && !next.empty())
    {
      std::string next_scheme, next_host;
      int next_port = 0;
      if (!SplitUrl(next, &next_scheme, &next_host, &next_port))
      {
        response.status = 0;
        return fail("the server sent the request on to a bad address: " + next);
      }
      if (scheme == "https" && next_scheme != "https")
      {
        response.status = 0;
        return fail("the server sent the request on to an insecure (http) address: " + next);
      }
      // 303, and 301/302 after a POST: a GET without the body (as browsers and curl do).
      if (method != "GET" && method != "HEAD" && (status == 303 || ((status == 301 || status == 302) && method == "POST")))
      {
        method = "GET";
        with_body = false;
      }
      url = next;
      continue;
    }
    if (!response.ok())
      response.plain = Plain(response.status, response.error);
    return response;
  }
}
}  // namespace

std::string Response::Header(const std::string& name) const
{
  const std::string wanted = Lower(name);
  for (const std::string& header : headers)
  {
    if (HeaderName(header) != wanted)
      continue;
    const size_t colon = header.find(':');
    size_t at = colon == std::string::npos ? header.size() : colon + 1;
    while (at < header.size() && (header[at] == ' ' || header[at] == '\t'))
      at++;
    return header.substr(at);
  }
  return {};
}

std::string UrlEncode(const std::string& text)
{
  std::string out;
  for (const unsigned char c : text)
  {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~')
    {
      out += static_cast<char>(c);
    }
    else
    {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

Session::~Session()
{
  if (m_curl)
    curl_easy_cleanup(static_cast<CURL*>(m_curl));
}

Response Stream(const Request& request, const Sink& sink, Session* session)
{
  Target target;
  target.data = sink;
  return Perform(request, target, session, session ? &session->m_curl : nullptr,
                 session ? &session->m_hosts : nullptr);
}

Response Fetch(const Request& request, Session* session)
{
  std::string body;
  bool too_big = false;
  const size_t most = request.max_bytes > 0 ? request.max_bytes : DEFAULT_MAX_BYTES;
  Response response = Stream(
      request,
      [&](const char* data, size_t size) {
        if (body.size() + size > most)
        {
          too_big = true;
          return false;
        }
        body.append(data, size);
        return true;
      },
      session);
  if (too_big)
  {
    response.status = 0;
    response.error = "the answer is too big (over " + std::to_string(most) + " bytes)";
    response.plain = Plain(0, response.error);
    body.clear();
  }
  response.body = std::move(body);
  return response;
}

Response Download(const Request& request, const std::string& path, Session* session)
{
  const std::string part = path + ".part";
  Request asked = request;
  // A part to go on with: only if it really has those bytes.
  if (asked.resume_from > 0)
  {
    struct stat info = {};
    if (stat(part.c_str(), &info) != 0 || static_cast<int64_t>(info.st_size) != asked.resume_from)
    {
      Log("download: " + part + " isn't " + std::to_string(asked.resume_from) + " bytes: from the start");
      asked.resume_from = 0;
    }
  }
  FILE* file = std::fopen(part.c_str(), asked.resume_from > 0 ? "r+b" : "wb");
  if (!file)
  {
    Response response;
    response.error = "couldn't create " + part + ": " + std::strerror(errno);
    response.plain = Plain(0, response.error);
    Log("download: " + response.error);
    return response;
  }
  std::string write_error, error_body;
  bool to_file = false;
  int64_t written = asked.resume_from;  // the part's bytes (a failed attempt keeps them for the next one)
  Target target;
  target.begin = [&](long status, int64_t offset) {
    to_file = status >= 200 && status < 300;
    if (!to_file)
      return true;  // an error page (error_body): into Response::body
    if (status == 206 && offset == asked.resume_from && asked.resume_from > 0)
    {
      written = offset;
      return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
    }
    if (status == 206)
    {
      write_error = "the server sent another part of the file (from byte " + std::to_string(offset) + ")";
      return false;
    }
    // The whole file (the server didn't take the Range): from the start.
    written = 0;
    std::fflush(file);
    if (ftruncate(fileno(file), 0) != 0 || std::fseek(file, 0, SEEK_SET) != 0)
    {
      write_error = "couldn't write " + part + ": " + std::strerror(errno);
      return false;
    }
    return true;
  };
  target.data = [&](const char* data, size_t size) {
    if (!to_file)
    {
      if (error_body.size() < ERROR_BODY_MAX)
        error_body.append(data, std::min(size, ERROR_BODY_MAX - error_body.size()));
      return true;
    }
    if (std::fwrite(data, 1, size, file) != size)
    {
      write_error = "couldn't write " + part + ": " + std::strerror(errno);
      return false;
    }
    written += static_cast<int64_t>(size);
    return true;
  };
  Response response = Perform(asked, target, session, session ? &session->m_curl : nullptr,
                              session ? &session->m_hosts : nullptr);
  bool closed = std::fflush(file) == 0;
  closed = fsync(fileno(file)) == 0 && closed;
  closed = std::fclose(file) == 0 && closed;
  if (!closed && write_error.empty() && to_file)
    write_error = "couldn't write " + part + ": " + std::strerror(errno);
  response.body = std::move(error_body);
  if (!write_error.empty())
  {
    response.status = 0;
    response.error = write_error;
    response.plain = Plain(0, write_error);
    Log("download: " + write_error);
  }
  response.bytes = written;
  const bool whole = response.ok() && to_file && (response.size < 0 || written == response.size);
  if (response.ok() && to_file && !whole)
  {
    response.status = 0;
    response.error = "the connection was cut (" + std::to_string(written) + " of " + std::to_string(response.size) +
                     " bytes)";
    response.plain = Plain(0, response.error);
  }
  if (whole)
  {
    std::remove(path.c_str());
    if (std::rename(part.c_str(), path.c_str()) != 0)
    {
      response.status = 0;
      response.error = "couldn't write " + path + ": " + std::strerror(errno);
      response.plain = Plain(0, response.error);
      Log("download: " + response.error);
    }
  }
  else if (written <= 0)
  {
    std::remove(part.c_str());  // (nothing to go on with)
  }
  return response;
}

long Get(const Request& request, const Sink& sink, std::string* error, Session* session)
{
  const Response response = Stream(request, sink, session);
  *error = response.error;
  return response.error.empty() ? response.status : 0;
}

std::string Plain(long status, const std::string& error)
{
  auto has = [&error](const char* text) { return error.find(text) != std::string::npos; };
  if (!error.empty())
  {
    if (has("could not look up"))
      return "no internet connection (the PS4 couldn't find the server)";
    if (has("network is not available"))
      return "the PS4's network isn't available";
    if (has("cancelled"))
      return "stopped";
    if (has("couldn't write") || has("couldn't create"))
      return "the storage is full or can't be written";
    if (has("too big"))
      return "the server's answer is too big";
    if (has("Timeout") || has("timed out") || has("too slow"))
      return "the server took too long to answer";
    if (has("BADCERT_EXPIRED") || has("BADCERT_FUTURE"))
      return "the secure connection failed: check the PS4's date and time";
    if (has("BADCERT") || has("peer certificate") || has("SSL certificate problem"))
      return "the server's certificate isn't valid";
    if (has("SSL") || has("TLS") || has("certificate") || has("mbedTLS") || has("insecure"))
      return "the secure connection failed";
    if (has("connect"))  // "Couldn't connect to server", "Failed to connect"
      return "the server didn't answer";
    if (has("receiving") || has("Recv failure") || has("artial file") || has("Send failure") || has("reset") ||
        has("was cut"))
      return "the connection was cut";
    return error;  // (curl's own words: rare)
  }
  if (status == 404 || status == 410)
    return "not on the server";
  if (status == 401 || status == 403 || status == 429)
    return "the server refused it";
  if (status >= 500)
    return "the server had a problem: try again later";
  if (status <= 0)
    return "the server didn't answer";
  return "the server answered with an error";
}

long GetText(const Request& request, std::string* body, size_t max_bytes, std::string* error,
             Session* session)
{
  body->clear();
  return Get(
      request,
      [&](const char* data, size_t size) {
        body->append(data, size);
        return body->size() <= max_bytes;
      },
      error, session);
}
}  // namespace Net
