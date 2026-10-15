// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Net.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "Net.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

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
std::mutex s_init_mutex;
int s_pool = -1;

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
  char line[96];
  snprintf(line, sizeof(line), "init %#x, pool %#x", static_cast<unsigned>(init),
           static_cast<unsigned>(s_pool));
  Log(line);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  if (s_pool < 0)
  {
    *error = "the PS4 network is not available";
    return false;
  }
  return true;
}

std::string Resolve(const std::string& host, std::string* error)
{
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

// "https://host[:port]/path" -> host, port.
bool SplitUrl(const std::string& url, std::string* host, int* port)
{
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos)
    return false;
  const size_t start = scheme + 3;
  const size_t end = url.find_first_of("/?#", start);
  std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  *port = url.compare(0, 5, "https") == 0 ? 443 : 80;
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos)
  {
    *port = std::atoi(authority.c_str() + colon + 1);
    authority = authority.substr(0, colon);
  }
  *host = authority;
  return !host->empty() && *port > 0;
}

struct Transfer
{
  CURL* curl = nullptr;
  const Sink* sink = nullptr;
  bool stopped = false;
};

size_t Write(char* data, size_t size, size_t count, void* user)
{
  auto* transfer = static_cast<Transfer*>(user);
  long status = 0;
  curl_easy_getinfo(transfer->curl, CURLINFO_RESPONSE_CODE, &status);
  if (status < 200 || status >= 300)
    return size * count;  // a redirect's or an error page's body
  if (!(*transfer->sink)(data, size * count))
  {
    transfer->stopped = true;
    return 0;
  }
  return size * count;
}
}  // namespace

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

long Get(const Request& request, const Sink& sink, std::string* error, Session* session)
{
  if (!Init(error))
    return 0;
  std::string url = request.url;
  for (int hop = 0; hop < 6; hop++)
  {
    std::string host;
    int port = 0;
    if (!SplitUrl(url, &host, &port))
    {
      *error = "bad address: " + url;
      return 0;
    }
    std::string address;
    if (session)
    {
      for (const auto& [known, known_address] : session->m_hosts)
      {
        if (known == host)
          address = known_address;
      }
    }
    if (address.empty())
    {
      address = Resolve(host, error);
      if (address.empty())
        return 0;
      if (session)
        session->m_hosts.emplace_back(host, address);
    }
    CURL* curl = session && session->m_curl ? static_cast<CURL*>(session->m_curl) : curl_easy_init();
    if (session)
      session->m_curl = curl;
    Transfer transfer{curl, &sink};
    const std::string resolve = host + ":" + std::to_string(port) + ":" + address;
    curl_slist* resolves = curl_slist_append(nullptr, resolve.c_str());
    curl_slist* headers = nullptr;
    for (const std::string& header : request.headers)
      headers = curl_slist_append(headers, header.c_str());
    char detail[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolves);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EmuSuNi-PS4");
    curl_easy_setopt(curl, CURLOPT_CAINFO, request.ca_file.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);  // each hop is looked up here
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, detail);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char* location = nullptr;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
    const std::string next = location ? location : "";
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_RESOLVE, nullptr);
    curl_slist_free_all(headers);
    curl_slist_free_all(resolves);
    if (!session)
      curl_easy_cleanup(curl);
    if (transfer.stopped)
    {
      *error = "cancelled";
      return 0;
    }
    if (code != CURLE_OK)
    {
      *error = std::string(curl_easy_strerror(code)) + (detail[0] ? std::string(": ") + detail : "");
      Log(host + ": " + *error);
      return 0;
    }
    if (status >= 300 && status < 400 && !next.empty())
    {
      url = next;
      continue;
    }
    return status;
  }
  *error = "too many redirects";
  return 0;
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
