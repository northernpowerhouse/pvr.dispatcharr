#include "dispatcharr_client.h"

#include <kodi/General.h>
#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// libcurl write callback for response data
namespace {

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
  size_t totalSize = size * nmemb;
  std::string* str = static_cast<std::string*>(userp);
  str->append(static_cast<char*>(contents), totalSize);
  return totalSize;
}

static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userp)
{
  size_t totalSize = size * nitems;
  std::string* str = static_cast<std::string*>(userp);
  str->append(buffer, totalSize);
  return totalSize;
}

} // anonymous namespace

namespace dispatcharr
{

namespace
{

// ----------------------------------------------------------------------------
// JSON Parsing Helpers (Ported/Adapted from xtream_client.cpp)
// ----------------------------------------------------------------------------

bool FindKeyPos(std::string_view obj, const std::string& key, size_t& outPos)
{
  const std::string needle = "\"" + key + "\"";
  outPos = obj.find(needle);
  return outPos != std::string::npos;
}

bool ParseIntAt(std::string_view obj, size_t pos, int& out)
{
  while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos])))
    ++pos;
  if (pos >= obj.size())
    return false;

  if (obj[pos] == '"')
  {
    ++pos;
    bool neg = false;
    if (pos < obj.size() && obj[pos] == '-') { neg = true; ++pos; }
    long long v = 0;
    bool any = false;
    while (pos < obj.size() && std::isdigit(static_cast<unsigned char>(obj[pos]))) {
      any = true;
      v = v * 10 + (obj[pos] - '0');
      ++pos;
    }
    if (!any) return false;
    out = static_cast<int>(neg ? -v : v);
    return true;
  }

  bool neg = false;
  if (obj[pos] == '-') { neg = true; ++pos; }
  long long v = 0;
  bool any = false;
  while (pos < obj.size() && std::isdigit(static_cast<unsigned char>(obj[pos]))) {
    any = true;
    v = v * 10 + (obj[pos] - '0');
    ++pos;
  }
  if (!any) return false;
  out = static_cast<int>(neg ? -v : v);
  return true;
}

bool ExtractIntField(std::string_view obj, const std::string& key, int& out)
{
  size_t pos = 0;
  if (!FindKeyPos(obj, key, pos))
    return false;
  pos = obj.find(':', pos);
  if (pos == std::string::npos)
    return false;
  return ParseIntAt(obj, pos + 1, out);
}

bool ExtractBoolField(std::string_view obj, const std::string& key, bool& out)
{
  size_t pos = 0;
  if (!FindKeyPos(obj, key, pos)) return false;
  pos = obj.find(':', pos);
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;
  if (pos >= obj.size()) return false;

  if (obj.substr(pos, 4) == "true") { out = true; return true; }
  if (obj.substr(pos, 5) == "false") { out = false; return true; }
  if (obj[pos] == '1') { out = true; return true; }
  if (obj[pos] == '0') { out = false; return true; }
  return false;
}

bool ExtractStringField(std::string_view obj, const std::string& key, std::string& out)
{
  size_t pos = 0;
  if (!FindKeyPos(obj, key, pos)) return false;
  pos = obj.find(':', pos);
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;
  if (pos >= obj.size() || obj[pos] != '"') return false;
  ++pos;

  std::string s;
  s.reserve(64);
  bool escape2 = false;
  for (; pos < obj.size(); ++pos)
  {
    const char c = obj[pos];
    if (escape2)
    {
      // Minimal check for generic escapes for brevity
      switch(c) {
        case '"': s.push_back('"'); break;
        case '\\': s.push_back('\\'); break;
        case 'n': s.push_back('\n'); break;
        case 'r': s.push_back('\r'); break;
        case 't': s.push_back('\t'); break;
        default: s.push_back(c); break; 
      }
      escape2 = false;
      continue;
    }
    if (c == '\\') { escape2 = true; continue; }
    if (c == '"') { out = s; return true; }
    s.push_back(c);
  }
  return false;
}

// Extract a raw JSON object string { ... } or array [ ... ] corresponding to a key
bool ExtractRawJsonField(std::string_view obj, const std::string& key, std::string_view& out)
{
  size_t pos = 0;
  if (!FindKeyPos(obj, key, pos)) return false;
  pos = obj.find(':', pos);
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;
  if (pos >= obj.size()) return false;

  char openChar = obj[pos];
  char closeChar = (openChar == '[') ? ']' : '}';
  if (openChar != '[' && openChar != '{') return false;

  int depth = 0;
  size_t start = pos;
  for (; pos < obj.size(); ++pos)
  {
    if (obj[pos] == openChar) depth++;
    else if (obj[pos] == closeChar) {
      depth--;
      if (depth == 0) {
        out = obj.substr(start, pos - start + 1);
        return true;
      }
    }
  }
  return false;
}

// Helper to escape JSON strings for write
std::string JsonEscape(const std::string& input)
{
  std::string output;
  for (char c : input) {
    switch (c) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output += c; break;
    }
  }
  return output;
}

time_t ParseIsoTime(const std::string& iso)
{
  // Dispatcharr returns timezone-aware ISO 8601 values. Convert them to UTC;
  // mktime() would incorrectly interpret the fields in Kodi's local timezone.
  if (iso.size() < 19)
    return 0;

  struct tm tm = {};
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
             &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
             &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
    return 0;

  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
#ifdef _WIN32
  time_t result = _mkgmtime(&tm);
#else
  time_t result = timegm(&tm);
#endif
  if (result == static_cast<time_t>(-1))
    return 0;

  // A positive offset means the wall clock is ahead of UTC, so subtract it.
  if (iso.size() >= 25 && (iso[19] == '+' || iso[19] == '-') && iso[22] == ':')
  {
    int offsetHours = 0;
    int offsetMinutes = 0;
    if (sscanf(iso.c_str() + 20, "%2d:%2d", &offsetHours, &offsetMinutes) == 2 &&
        offsetHours <= 23 && offsetMinutes <= 59)
    {
      const time_t offset = static_cast<time_t>(offsetHours * 3600 + offsetMinutes * 60);
      result += (iso[19] == '+') ? -offset : offset;
    }
  }

  return result;
}

std::string TimeToIso(time_t t)
{
  struct tm* tm = gmtime(&t);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
  return std::string(buf);
}

// Iterates top-level objects in a JSON array string
template<typename Fn>
bool ForEachObjectInArray(std::string_view jsonArray, Fn&& fn)
{
  size_t i = 0;
  const size_t n = jsonArray.size();
  while (i < n && std::isspace(static_cast<unsigned char>(jsonArray[i]))) ++i;
  if (i >= n || jsonArray[i] != '[') return false;

  int depth = 0;
  size_t objStart = std::string::npos;
  bool inString = false;
  bool escape = false;

  for (; i < n; ++i)
  {
    const char c = jsonArray[i];
    if (inString) {
      if (escape) escape = false;
      else if (c == '\\') escape = true;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') { inString = true; continue; }
    if (c == '{') {
      if (depth == 0) objStart = i;
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0 && objStart != std::string::npos) {
        fn(jsonArray.substr(objStart, i - objStart + 1));
        objStart = std::string::npos;
      }
    } else if (c == ']' && depth == 0) {
      break; 
    }
  }
  return true;
}

// Reduces an absolute URL (as returned in a DRF "next" pagination link, which
// may advertise a different host/port than our configured server) down to
// just its path+query, so pagination and playback URLs always get re-issued
// against the server the user actually configured. Mirrors the same trick
// already used for HLS playlist URIs in DownloadRecordingSegment().
std::string StripToPathAndQuery(const std::string& urlOrPath)
{
  if (urlOrPath.rfind("http://", 0) != 0 && urlOrPath.rfind("https://", 0) != 0)
    return urlOrPath;
  const size_t scheme = urlOrPath.find("://");
  const size_t path = urlOrPath.find('/', scheme + 3);
  if (path == std::string::npos)
    return "";
  return urlOrPath.substr(path);
}

} // namespace


Client::Client(const DvrSettings& settings) : m_settings(settings)
{
}

std::string Client::GetBaseUrl() const
{
  std::string server = m_settings.server;
  
  // Strip trailing slashes
  while (!server.empty() && server.back() == '/')
    server.pop_back();
  
  // Check if server already has a protocol prefix
  bool hasProtocol = (server.find("http://") == 0 || server.find("https://") == 0);
  
  std::stringstream ss;
  if (!hasProtocol) {
    ss << "http://";
  }
  ss << server;
  
  // Check if port is already in the server string (after protocol)
  bool hasPort = false;
  size_t hostStart = hasProtocol ? server.find("://") + 3 : 0;
  std::string hostPart = server.substr(hostStart);
  if (hostPart.find(':') != std::string::npos) {
    hasPort = true;
  }
  
  // Add port if needed
  if (!hasPort && m_settings.port > 0 && m_settings.port != 80) {
    ss << ":" << m_settings.port;
  }
  
  return ss.str();
}

Client::HttpResponse Client::Request(const std::string& method,
                                     const std::string& endpoint,
                                     const std::string& jsonBody,
                                     bool retryAuth,
                                     const std::string& rangeHeader)
{
  std::lock_guard<std::recursive_mutex> requestLock(m_requestMutex);
  HttpResponse resp;
  std::string url = GetBaseUrl() + endpoint;
  
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: Request %s %s", method.c_str(), url.c_str());
  if (!jsonBody.empty()) {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: Request body: %s", jsonBody.c_str());
  }
  
  CURL* curl = curl_easy_init();
  if (!curl) {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Failed to init curl");
    resp.statusCode = 0;
    return resp;
  }
  
  // Set URL
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  
  // Set timeout
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_settings.timeoutSeconds));
  
  // Set write callback
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);

  // Set headers
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  if (!m_accessToken.empty()) {
    std::string authHeader = "Authorization: Bearer " + m_accessToken;
    headers = curl_slist_append(headers, authHeader.c_str());
  }
  if (!rangeHeader.empty()) {
    std::string rangeHeaderLine = "Range: " + rangeHeader;
    headers = curl_slist_append(headers, rangeHeaderLine.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  
  // Set method and body
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
  } else if (method == "DELETE") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else if (method == "PUT") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
  }
  // GET is the default
  
  // Perform request
  CURLcode res = curl_easy_perform(curl);
  
  if (res == CURLE_OK) {
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    resp.statusCode = static_cast<int>(httpCode);
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: Response code: %d", resp.statusCode);
    if (endpoint.find("/hls/seg_") == std::string::npos && rangeHeader.empty())
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: Response: %s", resp.body.substr(0, 500).c_str());
  } else {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: curl_easy_perform failed: %s", curl_easy_strerror(res));
    resp.statusCode = 0;
  }
  
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  // If Dispatcharr rejects the cached access token, obtain a new one and retry
  // the request exactly once.
  if (resp.statusCode == 401 && retryAuth && !m_accessToken.empty())
  {
    kodi::Log(ADDON_LOG_INFO,
              "pvr.dispatcharr: Access token rejected; re-authenticating and retrying request");
    m_accessToken.clear();

    if (EnsureToken())
      return Request(method, endpoint, jsonBody, false, rangeHeader);
  }
  
  return resp;
}

bool Client::EnsureToken()
{
  std::lock_guard<std::recursive_mutex> requestLock(m_requestMutex);
  if (!m_accessToken.empty()) return true;
  
  std::stringstream ss;
  ss << "{\"username\":\"" << JsonEscape(m_settings.username) 
     << "\",\"password\":\"" << JsonEscape(m_settings.password) << "\"}";
  
  std::string jsonBody = ss.str();
  std::string url = GetBaseUrl() + "/api/accounts/token/";
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: EnsureToken - URL: %s", url.c_str());
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: EnsureToken - Username: %s", m_settings.username.c_str());
  
  // Use libcurl directly for authentication
  CURL* curl = curl_easy_init();
  if (!curl) {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: EnsureToken - Failed to init curl");
    return false;
  }
  
  std::string responseBody;
  
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_settings.timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
  
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
  
  CURLcode res = curl_easy_perform(curl);
  
  bool success = false;
  if (res == CURLE_OK) {
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: EnsureToken - HTTP code: %ld", httpCode);
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: EnsureToken - Response body length: %zu", responseBody.size());
    
    if (httpCode == 200) {
      std::string token;
      if (ExtractStringField(responseBody, "access", token) && !token.empty()) {
        m_accessToken = token;
        kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: EnsureToken - Successfully extracted access token (length: %zu)", token.size());
        success = true;
      } else {
        kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: EnsureToken - Failed to extract access token from response");
      }
    } else {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: EnsureToken - HTTP error %ld: %s", httpCode, responseBody.c_str());
    }
  } else {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: EnsureToken - curl_easy_perform failed: %s", curl_easy_strerror(res));
  }
  
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  
  if (!success) {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Failed to authenticate user %s", m_settings.username.c_str());
  }
  return success;
}

bool Client::FetchSeriesRules(std::vector<SeriesRule>& outRules)
{
  if (!EnsureToken()) return false;
  
  auto resp = Request("GET", "/api/channels/series-rules/");
  if (resp.statusCode != 200) return false;
  
  outRules.clear();
  // Expecting {"rules": [...]}
  std::string_view bodyView(resp.body);
  std::string_view rulesArray;
  
  if (ExtractRawJsonField(bodyView, "rules", rulesArray)) {
    ForEachObjectInArray(rulesArray, [&](std::string_view obj){
      SeriesRule r;
      if (ExtractStringField(obj, "tvg_id", r.tvgId)) {
        ExtractStringField(obj, "title", r.title);
        ExtractStringField(obj, "mode", r.mode);
        outRules.push_back(r);
      }
    });
    return true;
  }
  
  return false;
}

bool Client::AddSeriesRule(const std::string& tvgId, const std::string& title, const std::string& mode)
{
  if (!EnsureToken()) return false;
  
  std::stringstream ss;
  ss << "{\"tvg_id\":\"" << JsonEscape(tvgId) << "\"";
  if (!title.empty()) ss << ",\"title\":\"" << JsonEscape(title) << "\"";
  if (!mode.empty()) ss << ",\"mode\":\"" << JsonEscape(mode) << "\"";
  ss << "}";
  
  auto resp = Request("POST", "/api/channels/series-rules/", ss.str());
  // Django REST Framework returns 201 Created with the created object
  bool success = (resp.statusCode == 200 || resp.statusCode == 201) && (resp.body.find("\"id\"") != std::string::npos || resp.body.find("\"tvg_id\"") != std::string::npos);
  kodi::Log(success ? ADDON_LOG_DEBUG : ADDON_LOG_ERROR, 
            "pvr.dispatcharr: AddSeriesRule result - success=%d, body=%s", 
            success, resp.body.substr(0, 200).c_str());
  return success;
}

bool Client::DeleteSeriesRule(const std::string& tvgId)
{
  if (!EnsureToken()) return false;
  // URL encode? Assuming tvgId is safe-ish or basic chars
  auto resp = Request("DELETE", "/api/channels/series-rules/" + tvgId + "/");
  // HTTP 204 No Content is the correct response for DELETE
  bool success = (resp.statusCode == 200 || resp.statusCode == 204);
  kodi::Log(success ? ADDON_LOG_DEBUG : ADDON_LOG_ERROR,
            "pvr.dispatcharr: DeleteSeriesRule result - success=%d, statusCode=%d",
            success, resp.statusCode);
  return success;
}

bool Client::FetchRecurringRules(std::vector<RecurringRule>& outRules)
{
  if (!EnsureToken()) return false;
  auto resp = Request("GET", "/api/channels/recurring-rules/");
  if (resp.statusCode != 200) return false;
  
  outRules.clear();
  ForEachObjectInArray(resp.body, [&](std::string_view obj){
    RecurringRule r;
    if (ExtractIntField(obj, "id", r.id)) {
      ExtractIntField(obj, "channel", r.channelId);
      ExtractStringField(obj, "name", r.name);
      ExtractStringField(obj, "start_time", r.startTime);
      ExtractStringField(obj, "end_time", r.endTime);
      ExtractStringField(obj, "start_date", r.startDate);
      ExtractStringField(obj, "end_date", r.endDate);
      ExtractBoolField(obj, "enabled", r.enabled);
      
      // Manually parse days_of_week
      size_t daysPos = 0;
      if (FindKeyPos(obj, "days_of_week", daysPos)) {
         size_t arrStart = obj.find('[', daysPos);
         size_t arrEnd = obj.find(']', arrStart);
         if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string_view arrRaw = std::string_view(obj).substr(arrStart+1, arrEnd - arrStart - 1);
            size_t subPos = 0;
            while (subPos < arrRaw.size()) {
                if (std::isdigit(arrRaw[subPos])) {
                    r.daysOfWeek.push_back(arrRaw[subPos] - '0');
                }
                subPos++;
            }
         }
      }
      outRules.push_back(r);
    }
  });
  return true;
}

bool Client::AddRecurringRule(const RecurringRule& rule)
{
  if (!EnsureToken()) return false;
  std::stringstream ss;
  ss << "{\"channel\":" << rule.channelId 
     << ",\"name\":\"" << JsonEscape(rule.name) << "\""
     << ",\"start_time\":\"" << rule.startTime << "\""
     << ",\"end_time\":\"" << rule.endTime << "\""
     << ",\"start_date\":\"" << rule.startDate << "\""
     << ",\"end_date\":\"" << rule.endDate << "\""
     << ",\"enabled\":true"
     << ",\"days_of_week\":[";
  for(size_t i=0; i<rule.daysOfWeek.size(); ++i) {
    ss << rule.daysOfWeek[i];
    if (i < rule.daysOfWeek.size()-1) ss << ",";
  }
  ss << "]}";
  
  auto resp = Request("POST", "/api/channels/recurring-rules/", ss.str());
  // HTTP 201 Created is the correct success response for POST
  bool success = (resp.statusCode == 200 || resp.statusCode == 201) && resp.body.find("\"id\"") != std::string::npos;
  kodi::Log(success ? ADDON_LOG_DEBUG : ADDON_LOG_ERROR, 
            "pvr.dispatcharr: AddRecurringRule result - success=%d, body=%s", 
            success, resp.body.substr(0, 200).c_str());
  return success;
}

bool Client::DeleteRecurringRule(int id)
{
  if (!EnsureToken()) return false;
  auto resp = Request("DELETE", "/api/channels/recurring-rules/" + std::to_string(id) + "/");
  // HTTP 204 No Content is the correct response for DELETE
  bool success = (resp.statusCode == 200 || resp.statusCode == 204);
  kodi::Log(success ? ADDON_LOG_DEBUG : ADDON_LOG_ERROR,
            "pvr.dispatcharr: DeleteRecurringRule result - success=%d, statusCode=%d",
            success, resp.statusCode);
  return success;
}

bool Client::FetchChannels(std::vector<DispatchChannel>& outChannels)
{
  if (!EnsureToken()) return false;
  outChannels.clear();

  // /api/channels/channels/ is paginated (DRF envelope: count/next/previous/
  // results), so a single request only returns the first page's worth of
  // channels. Follow "next" until exhausted. A large page_size keeps this to
  // one or two requests for a typical few-thousand-channel catalogue.
  std::string endpoint = "/api/channels/channels/?page_size=500";
  while (!endpoint.empty())
  {
    auto resp = Request("GET", endpoint);
    if (resp.statusCode != 200) return !outChannels.empty();

    std::string_view bodyView(resp.body);
    std::string_view arrayView;
    const bool paginated = ExtractRawJsonField(bodyView, "results", arrayView);
    std::string_view toIterate = paginated ? arrayView : bodyView;

    ForEachObjectInArray(toIterate, [&](std::string_view obj){
      DispatchChannel ch;
      if (ExtractIntField(obj, "id", ch.id)) {
        // channel_number is a float in the API, but we'll read as int
        ExtractIntField(obj, "channel_number", ch.channelNumber);
        ExtractStringField(obj, "name", ch.name);
        ExtractStringField(obj, "uuid", ch.uuid);
        ExtractStringField(obj, "tvg_id", ch.tvgId);
        ExtractIntField(obj, "channel_group_id", ch.groupId);
        ExtractIntField(obj, "logo_id", ch.logoId);
        ExtractBoolField(obj, "is_catchup", ch.isCatchup);
        ExtractIntField(obj, "catchup_days", ch.catchupDays);
        outChannels.push_back(ch);
      }
    });

    if (!paginated) break;

    std::string nextUrl;
    if (!ExtractStringField(bodyView, "next", nextUrl) || nextUrl.empty()) break;
    endpoint = StripToPathAndQuery(nextUrl);
    if (endpoint.empty()) break;
  }
  return true;
}

bool Client::FetchChannelGroups(std::vector<ChannelGroup>& outGroups)
{
  if (!EnsureToken()) return false;
  outGroups.clear();

  std::string endpoint = "/api/channels/groups/?page_size=500";
  while (!endpoint.empty())
  {
    auto resp = Request("GET", endpoint);
    if (resp.statusCode != 200) return !outGroups.empty();

    std::string_view bodyView(resp.body);
    std::string_view arrayView;
    const bool paginated = ExtractRawJsonField(bodyView, "results", arrayView);
    std::string_view toIterate = paginated ? arrayView : bodyView;

    ForEachObjectInArray(toIterate, [&](std::string_view obj){
      ChannelGroup g;
      if (ExtractIntField(obj, "id", g.id)) {
        ExtractStringField(obj, "name", g.name);
        outGroups.push_back(g);
      }
    });

    if (!paginated) break;

    std::string nextUrl;
    if (!ExtractStringField(bodyView, "next", nextUrl) || nextUrl.empty()) break;
    endpoint = StripToPathAndQuery(nextUrl);
    if (endpoint.empty()) break;
  }
  return true;
}

bool Client::FetchLogos(std::map<int, std::string>& outLogoUrlsById)
{
  if (!EnsureToken()) return false;
  outLogoUrlsById.clear();

  std::string endpoint = "/api/channels/logos/?page_size=500";
  while (!endpoint.empty())
  {
    auto resp = Request("GET", endpoint);
    if (resp.statusCode != 200) return !outLogoUrlsById.empty();

    std::string_view bodyView(resp.body);
    std::string_view arrayView;
    const bool paginated = ExtractRawJsonField(bodyView, "results", arrayView);
    std::string_view toIterate = paginated ? arrayView : bodyView;

    ForEachObjectInArray(toIterate, [&](std::string_view obj){
      int id = 0;
      if (ExtractIntField(obj, "id", id)) {
        // Prefer Dispatcharr's cached copy over the (possibly unreachable)
        // origin logo URL.
        std::string url;
        if (!ExtractStringField(obj, "cache_url", url) || url.empty())
          ExtractStringField(obj, "url", url);
        if (!url.empty())
          outLogoUrlsById[id] = (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
              ? url : GetBaseUrl() + url;
      }
    });

    if (!paginated) break;

    std::string nextUrl;
    if (!ExtractStringField(bodyView, "next", nextUrl) || nextUrl.empty()) break;
    endpoint = StripToPathAndQuery(nextUrl);
    if (endpoint.empty()) break;
  }
  return true;
}

std::string Client::BuildLiveStreamUrl(const std::string& channelUuid)
{
  if (!EnsureToken()) return "";
  // JWTs use the URL-safe base64 alphabet plus '.' separators, so no
  // percent-encoding is needed here.
  return GetBaseUrl() + "/proxy/ts/stream/" + channelUuid + "?token=" + m_accessToken;
}

bool Client::FetchEpgGrid(time_t start, time_t end, std::vector<EpgProgram>& outPrograms)
{
  if (!EnsureToken()) return false;
  outPrograms.clear();

  std::ostringstream endpoint;
  endpoint << "/api/epg/grid/?start=" << TimeToIso(start) << "&end=" << TimeToIso(end);
  auto resp = Request("GET", endpoint.str());
  if (resp.statusCode != 200) return false;

  std::string_view bodyView(resp.body);
  std::string_view dataArray;
  if (!ExtractRawJsonField(bodyView, "data", dataArray)) return false;

  ForEachObjectInArray(dataArray, [&](std::string_view obj){
    EpgProgram p;
    if (ExtractStringField(obj, "tvg_id", p.tvgId) && !p.tvgId.empty()) {
      ExtractStringField(obj, "title", p.title);
      ExtractStringField(obj, "sub_title", p.subtitle);
      ExtractStringField(obj, "description", p.description);
      std::string sVal;
      if (ExtractStringField(obj, "start_time", sVal)) p.startTime = ParseIsoTime(sVal);
      if (ExtractStringField(obj, "end_time", sVal)) p.endTime = ParseIsoTime(sVal);
      outPrograms.push_back(p);
    }
  });
  return true;
}

bool Client::CreateCatchupSession(const std::string& channelUuid,
                                  time_t programStart,
                                  int durationMinutes,
                                  CatchupSession& outSession)
{
  if (!EnsureToken()) return false;

  std::stringstream ss;
  ss << "{\"channel_uuid\":\"" << JsonEscape(channelUuid) << "\""
     << ",\"start\":\"" << TimeToIso(programStart) << "\"";
  if (durationMinutes > 0)
    ss << ",\"duration\":" << durationMinutes;
  ss << "}";

  auto resp = Request("POST", "/api/catchup/sessions/", ss.str());
  if (resp.statusCode != 201) {
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr: CreateCatchupSession failed - statusCode=%d, body=%s",
              resp.statusCode, resp.body.substr(0, 200).c_str());
    return false;
  }

  std::string sessionId, playbackUrl;
  if (!ExtractStringField(resp.body, "session_id", sessionId) ||
      !ExtractStringField(resp.body, "playback_url", playbackUrl)) {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: CreateCatchupSession - malformed response: %s",
              resp.body.substr(0, 200).c_str());
    return false;
  }

  int expiresAt = 0;
  ExtractIntField(resp.body, "expires_at", expiresAt);

  outSession.sessionId = sessionId;
  outSession.playbackUrl = (playbackUrl.rfind("http://", 0) == 0 || playbackUrl.rfind("https://", 0) == 0)
      ? playbackUrl : GetBaseUrl() + playbackUrl;
  outSession.expiresAt = static_cast<time_t>(expiresAt);
  return true;
}

bool Client::DeleteCatchupSession(const std::string& sessionId)
{
  if (!EnsureToken()) return false;
  auto resp = Request("DELETE", "/api/catchup/sessions/" + sessionId + "/");
  // HTTP 204 No Content is the correct response for DELETE
  bool success = (resp.statusCode == 200 || resp.statusCode == 204);
  kodi::Log(success ? ADDON_LOG_DEBUG : ADDON_LOG_WARNING,
            "pvr.dispatcharr: DeleteCatchupSession result - success=%d, statusCode=%d",
            success, resp.statusCode);
  return success;
}

bool Client::CreateCatchupUrls(const std::string& channelUuid,
                               time_t programStart,
                               int durationMinutes,
                               CatchupUrls& outUrls)
{
  CatchupSession session;
  if (!CreateCatchupSession(channelUuid, programStart, durationMinutes, session))
    return false;

  const std::string base = GetBaseUrl() + "/proxy/catchup/" + channelUuid +
      "?session_id=" + session.sessionId + "&token=" + m_accessToken + "&start=";

  outUrls.defaultUrl = base + TimeToIso(programStart);
  // ffmpegdirect substitutes {Y}/{m}/{d}/{H}/{M} with the seek target's
  // date/time components (same placeholder syntax as the Xtream catchup
  // template) and re-requests this URL - same session_id, new `start` -
  // on every seek.
  outUrls.templateUrl = base + "{Y}-{m}-{d}T{H}:{M}:00Z";
  return true;
}

bool Client::EnsureChannelMapping()
{
  if (!m_channelNumberToDispatchId.empty()) return true;
  
  std::vector<DispatchChannel> channels;
  if (!FetchChannels(channels)) {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Failed to fetch channels for mapping");
    return false;
  }
  
  for (const auto& ch : channels) {
    m_channelNumberToDispatchId[ch.channelNumber] = ch.id;
    m_dispatchIdToChannelNumber[ch.id] = ch.channelNumber;
    m_kodiUidToDispatchTvgId[ch.channelNumber] = ch.tvgId;
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: Channel mapping: number %d -> Dispatcharr ID %d, tvg_id='%s'", 
              ch.channelNumber, ch.id, ch.tvgId.c_str());
  }
  
  kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: Built channel mapping with %zu channels", 
            m_channelNumberToDispatchId.size());
  return true;
}

int Client::GetDispatchChannelId(int kodiChannelUid)
{
  if (!EnsureChannelMapping()) return -1;
  
  // The Kodi channel UID is the Xtream stream ID.
  // Dispatcharr channels may have channel_number set to match.
  // We'll try to find by channel number first.
  auto it = m_channelNumberToDispatchId.find(kodiChannelUid);
  if (it != m_channelNumberToDispatchId.end()) {
    return it->second;
  }
  
  kodi::Log(ADDON_LOG_WARNING, "pvr.dispatcharr: No Dispatcharr channel found for Kodi UID %d", kodiChannelUid);
  return -1;
}

int Client::GetKodiChannelUid(int dispatchChannelId)
{
  if (!EnsureChannelMapping()) return -1;
  
  auto it = m_dispatchIdToChannelNumber.find(dispatchChannelId);
  if (it != m_dispatchIdToChannelNumber.end()) {
    return it->second;
  }
  
  kodi::Log(ADDON_LOG_WARNING, "pvr.dispatcharr: No Kodi channel found for Dispatcharr ID %d", dispatchChannelId);
  return -1;
}

std::string Client::GetDispatchTvgId(int kodiChannelUid)
{
  if (!EnsureChannelMapping()) return "";
  
  auto it = m_kodiUidToDispatchTvgId.find(kodiChannelUid);
  if (it != m_kodiUidToDispatchTvgId.end()) {
    return it->second;
  }
  
  kodi::Log(ADDON_LOG_WARNING, "pvr.dispatcharr: No Dispatcharr tvg_id found for Kodi UID %d", kodiChannelUid);
  return "";
}

bool Client::FetchRecordings(std::vector<Recording>& outRecordings)
{
  if (!EnsureToken()) return false;
  auto resp = Request("GET", "/api/channels/recordings/");
  if (resp.statusCode != 200) return false;
  
  outRecordings.clear();
  ForEachObjectInArray(resp.body, [&](std::string_view obj){
    Recording r;
    if (ExtractIntField(obj, "id", r.id)) {
      ExtractIntField(obj, "channel", r.channelId);
      
      std::string sVal;
      if (ExtractStringField(obj, "start_time", sVal)) r.startTime = ParseIsoTime(sVal);
      if (ExtractStringField(obj, "end_time", sVal)) r.endTime = ParseIsoTime(sVal);
      
      std::string_view customProps;
      if (ExtractRawJsonField(obj, "custom_properties", customProps)) {
          std::string_view programObj;
          if (ExtractRawJsonField(customProps, "program", programObj)) {
              ExtractStringField(programObj, "title", r.title);
              ExtractStringField(programObj, "description", r.plot);
          }
          // Extract status: "scheduled", "recording", "completed", "interrupted"
          ExtractStringField(customProps, "status", r.status);
          // Extract poster_url for cover art
          ExtractStringField(customProps, "poster_url", r.iconPath);

          std::string kodiEpgUid;
          if (ExtractStringField(customProps, "kodi_epg_uid", kodiEpgUid)) {
              const bool digitsOnly = !kodiEpgUid.empty() &&
                  std::all_of(kodiEpgUid.begin(), kodiEpgUid.end(),
                              [](unsigned char c) { return std::isdigit(c) != 0; });
              errno = 0;
              char* end = nullptr;
              const unsigned long value = std::strtoul(kodiEpgUid.c_str(), &end, 10);
              if (digitsOnly && errno == 0 && end && *end == '\0' &&
                  value <= std::numeric_limits<unsigned int>::max()) {
                  r.kodiEpgUid = static_cast<unsigned int>(value);
              } else {
                  kodi::Log(ADDON_LOG_WARNING,
                            "pvr.dispatcharr: Invalid Kodi EPG UID for recording %d",
                            r.id);
              }
          }

          std::string kodiChannelUid;
          if (ExtractStringField(customProps, "kodi_channel_uid", kodiChannelUid)) {
              const bool digitsOnly = !kodiChannelUid.empty() &&
                  std::all_of(kodiChannelUid.begin(), kodiChannelUid.end(),
                              [](unsigned char c) { return std::isdigit(c) != 0; });
              errno = 0;
              char* end = nullptr;
              const unsigned long value = std::strtoul(kodiChannelUid.c_str(), &end, 10);
              if (digitsOnly && errno == 0 && end && *end == '\0' && value > 0 &&
                  value <= static_cast<unsigned long>(std::numeric_limits<int>::max())) {
                  r.kodiChannelUid = static_cast<int>(value);
              } else {
                  kodi::Log(ADDON_LOG_WARNING,
                            "pvr.dispatcharr: Invalid Kodi channel UID for recording %d",
                            r.id);
              }
          }
      }
      
      // Default to "scheduled" if status is missing (e.g. newly created recordings
      // may not have status set yet in the API response)
      if (r.status.empty()) {
          r.status = "scheduled";
      }
      
      outRecordings.push_back(r);
    }
  });
  return true;
}

bool Client::GetRecording(int id, Recording& outRecording)
{
  std::vector<Recording> recordings;
  if (!FetchRecordings(recordings))
    return false;
  const auto it = std::find_if(recordings.begin(), recordings.end(),
                               [id](const Recording& value) { return value.id == id; });
  if (it == recordings.end())
    return false;
  outRecording = *it;
  return true;
}

bool Client::FetchActiveRecordingManifest(int id, std::string& outManifest)
{
  outManifest.clear();
  if (!EnsureToken())
    return false;
  const auto response = Request(
      "GET", "/api/channels/recordings/" + std::to_string(id) + "/hls/index.m3u8");
  if (response.statusCode != 200 || response.body.find("#EXTM3U") == std::string::npos)
    return false;
  outManifest = response.body;
  return true;
}

bool Client::DownloadRecordingSegment(int id, const std::string& uri, std::string& outData)
{
  outData.clear();
  if (!EnsureToken())
    return false;

  std::string endpoint = uri;
  if (endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0)
  {
    // Dispatcharr may advertise its public URL without the explicit/default
    // port present in the add-on setting. Extract only the path and fetch it
    // from our configured server, so credentials can never be sent to the
    // playlist-provided authority.
    const size_t scheme = endpoint.find("://");
    const size_t path = endpoint.find('/', scheme + 3);
    if (path == std::string::npos)
      return false;
    endpoint.erase(0, path);
  }
  else if (endpoint.empty() || endpoint.front() != '/')
    endpoint = "/api/channels/recordings/" + std::to_string(id) + "/hls/" + endpoint;

  // Authorization headers are refreshed automatically by Request(). Discard
  // any short-lived token copied into the playlist URI.
  const size_t query = endpoint.find('?');
  if (query != std::string::npos)
    endpoint.erase(query);
  const std::string allowedPrefix = "/api/channels/recordings/" +
                                    std::to_string(id) + "/hls/";
  if (endpoint.rfind(allowedPrefix, 0) != 0 || endpoint.find("..") != std::string::npos)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr: Refusing unexpected recording segment path '%s'",
              endpoint.c_str());
    return false;
  }
  const auto response = Request("GET", endpoint);
  if (response.statusCode != 200 || response.body.empty())
    return false;
  outData = response.body;
  return true;
}

bool Client::FetchRecordingFileRange(int id, int64_t offset, int64_t length,
                                     std::string& outData, int64_t& outTotalLength)
{
  outData.clear();
  outTotalLength = 0;
  if (length <= 0)
    return false;
  if (!EnsureToken())
    return false;

  std::ostringstream range;
  range << "bytes=" << offset << "-" << (offset + length - 1);
  const auto response = Request(
      "GET", "/api/channels/recordings/" + std::to_string(id) + "/file/", "", true, range.str());
  if (response.statusCode != 200 && response.statusCode != 206)
    return false;

  // Parse "Content-Range: bytes 0-0/12345" to learn the recording's total
  // size. std::stoll stops at the first non-digit character, so trailing
  // "\r\n" and any headers after it are harmlessly ignored.
  const size_t headerPos = response.headers.find("Content-Range:");
  if (headerPos != std::string::npos)
  {
    const size_t slash = response.headers.find('/', headerPos);
    if (slash != std::string::npos)
    {
      try { outTotalLength = std::stoll(response.headers.substr(slash + 1)); }
      catch (...) { outTotalLength = 0; }
    }
  }
  if (outTotalLength <= 0)
  {
    // Server ignored the Range request (e.g. HTTP 200) and returned the
    // whole file; treat what we got as the complete recording.
    outTotalLength = static_cast<int64_t>(response.body.size());
  }

  outData = std::move(response.body);
  return true;
}

bool Client::DeleteRecording(int id)
{
  if (!EnsureToken()) return false;
  auto resp = Request("DELETE", "/api/channels/recordings/" + std::to_string(id) + "/");
  // HTTP 204 No Content is the correct response for DELETE
  bool success = (resp.statusCode == 200 || resp.statusCode == 204);
  kodi::Log(success ? ADDON_LOG_DEBUG : ADDON_LOG_ERROR,
            "pvr.dispatcharr: DeleteRecording result - success=%d, statusCode=%d",
            success, resp.statusCode);
  return success;
}

bool Client::ScheduleRecording(int channelId,
                               time_t startTime,
                               time_t endTime,
                               const std::string& title,
                               unsigned int kodiEpgUid,
                               int kodiChannelUid)
{
  if (!EnsureToken()) return false;
  std::stringstream ss;
  ss << "{\"channel\":" << channelId 
     << ",\"start_time\":\"" << TimeToIso(startTime) << "\""
     << ",\"end_time\":\"" << TimeToIso(endTime) << "\""
     << ",\"custom_properties\":{\"program\":{\"title\":\"" << JsonEscape(title) << "\"}";
  if (kodiEpgUid != 0 && kodiChannelUid != 0)
  {
    ss << ",\"kodi_epg_uid\":\"" << kodiEpgUid << "\""
       << ",\"kodi_channel_uid\":\"" << kodiChannelUid << "\"";
  }
  ss << "}}";
  
  auto resp = Request("POST", "/api/channels/recordings/", ss.str());
  // HTTP 201 Created is the correct success response for POST
  bool success = (resp.statusCode == 200 || resp.statusCode == 201);
  kodi::Log(success ? ADDON_LOG_DEBUG : ADDON_LOG_ERROR,
            "pvr.dispatcharr: ScheduleRecording result - success=%d, statusCode=%d, body=%s",
            success, resp.statusCode, resp.body.substr(0, 200).c_str());
  return success;
}

} // namespace dispatcharr
