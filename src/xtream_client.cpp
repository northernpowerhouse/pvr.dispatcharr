#include "xtream_client.h"

#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/addon-instance/PVR.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Platform-specific time functions
#ifdef _WIN32
  // Windows doesn't have gmtime_r, use gmtime_s instead
  static inline std::tm* gmtime_r_compat(const time_t* timer, std::tm* buf)
  {
    return (gmtime_s(buf, timer) == 0) ? buf : nullptr;
  }
  #define gmtime_r gmtime_r_compat
  
  // Windows doesn't have timegm, use _mkgmtime instead
  #define timegm _mkgmtime
#endif

namespace
{
constexpr const char* kDefaultAddonUserAgent = "DispatcharrKodiAddon";
constexpr size_t kMaxHttpBodyBytes = 50 * 1024 * 1024; // cap JSON API responses to protect memory
// XMLTV guides legitimately run much larger than JSON API responses (many
// channels x many days of uncompressed program data). Their ceiling is
// user-configurable (Settings::xmltvMaxSizeMb) rather than a fixed constant.

std::string Trim(std::string s)
{
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
    s.erase(s.begin());
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
    s.pop_back();
  return s;
}

std::string ToLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

void ParseProgrammeGenre(const pugi::xml_node& programmeNode, xtream::EpgEntry& entry)
{
  std::vector<std::string> categories;
  for (const auto& categoryNode : programmeNode.children("category"))
  {
    const std::string category = Trim(categoryNode.child_value());
    if (category.empty())
      continue;

    // Kodi uses EPG_STRING_TOKEN_SEPARATOR (a comma) between free-text genres.
    if (!entry.genreString.empty())
      entry.genreString += EPG_STRING_TOKEN_SEPARATOR;
    entry.genreString += category;
    categories.push_back(ToLower(category));
  }

  const auto hasAny = [&categories](std::initializer_list<const char*> values) {
    for (const auto& category : categories)
    {
      for (const char* value : values)
      {
        if (category == value)
          return true;
      }
    }
    return false;
  };

  // Prefer specific categories over generic XMLTV categories such as "Series".
  // Sports has the highest priority because feeds often also label games as series.
  if (hasAny({"football", "soccer"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_FOOTBALL_SOCCER;
  }
  else if (hasAny({"tennis"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_TENNIS_SQUASH;
  }
  else if (hasAny({"volleyball", "basketball"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_TEAMSPORTS;
  }
  else if (hasAny({"track/field"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_ATHLETICS;
  }
  else if (hasAny({"auto racing", "motorcycle racing"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_MOTORSPORT;
  }
  else if (hasAny({"sailing"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_WATERSPORT;
  }
  else if (hasAny({"sports talk"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_SPORTS_MAGAZINES;
  }
  else if (hasAny({"sports", "playoff sports", "golf", "fishing"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPORTS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPORTS_GENERAL;
  }
  else if (hasAny({"weather"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_NEWSCURRENTAFFAIRS_WEATHER;
  }
  else if (hasAny({"newsmagazine"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_NEWSCURRENTAFFAIRS_MAGAZINE;
  }
  else if (hasAny({"news", "political news satire & talk"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS;
    entry.genreSubType = hasAny({"interview", "debate"})
                             ? EPG_EVENT_CONTENTSUBMASK_NEWSCURRENTAFFAIRS_DISCUSSION_INTERVIEW_DEBATE
                             : EPG_EVENT_CONTENTSUBMASK_NEWSCURRENTAFFAIRS_GENERAL;
  }
  else if (hasAny({"children", "animated"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_CHILDRENYOUTH;
    entry.genreSubType = hasAny({"animated"})
                             ? EPG_EVENT_CONTENTSUBMASK_CHILDRENYOUTH_CARTOONS_PUPPETS
                             : EPG_EVENT_CONTENTSUBMASK_CHILDRENYOUTH_GENERAL;
  }
  else if (hasAny({"game show"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SHOW;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SHOW_GAMESHOW_QUIZ_CONTEST;
  }
  else if (hasAny({"variety"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SHOW;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SHOW_VARIETY_SHOW;
  }
  else if (hasAny({"talk", "interview", "debate"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SHOW;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SHOW_TALK_SHOW;
  }
  else if (hasAny({"reality", "competition reality", "entertainment"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SHOW;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SHOW_GENERAL;
  }
  else if (hasAny({"crime", "crime drama", "mystery"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_MOVIEDRAMA;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_MOVIEDRAMA_DETECTIVE_THRILLER;
  }
  else if (hasAny({"action", "adventure"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_MOVIEDRAMA;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_MOVIEDRAMA_ADVENTURE_WESTERN_WAR;
  }
  else if (hasAny({"comedy", "sitcom"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_MOVIEDRAMA;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_MOVIEDRAMA_COMEDY;
  }
  else if (hasAny({"soap"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_MOVIEDRAMA;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_MOVIEDRAMA_SOAP_MELODRAMA_FOLKLORIC;
  }
  else if (hasAny({"romance"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_MOVIEDRAMA;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_MOVIEDRAMA_ROMANCE;
  }
  else if (hasAny({"movie", "drama", "docudrama"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_MOVIEDRAMA;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_MOVIEDRAMA_GENERAL;
  }
  else if (hasAny({"music", "dance"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_MUSICBALLETDANCE_GENERAL;
  }
  else if (hasAny({"religious"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_ARTSCULTURE;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_ARTSCULTURE_RELIGION;
  }
  else if (hasAny({"politics", "public affairs", "community", "consumer", "law"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SOCIALPOLITICALECONOMICS_GENERAL;
  }
  else if (hasAny({"nature", "animals", "environment"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_EDUCATIONALSCIENCE_NATURE_ANIMALS_ENVIRONMENT;
  }
  else if (hasAny({"medical", "health"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_EDUCATIONALSCIENCE_MEDICINE_PHYSIOLOGY_PSYCHOLOGY;
  }
  else if (hasAny({"documentary", "educational", "science", "history", "american history"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_EDUCATIONALSCIENCE_GENERAL;
  }
  else if (hasAny({"travel"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_LEISUREHOBBIES;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_LEISUREHOBBIES_TOURISM_TRAVEL;
  }
  else if (hasAny({"shopping"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_LEISUREHOBBIES;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_LEISUREHOBBIES_ADVERTISEMENT_SHOPPING;
  }
  else if (hasAny({"house/garden", "home improvement"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_LEISUREHOBBIES;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_LEISUREHOBBIES_GARDENING;
  }
  else if (hasAny({"pets", "fashion", "how-to", "self improvement"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_LEISUREHOBBIES;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_LEISUREHOBBIES_GENERAL;
  }
  else if (hasAny({"special"}))
  {
    entry.genreType = EPG_EVENT_CONTENTMASK_SPECIAL;
    entry.genreSubType = EPG_EVENT_CONTENTSUBMASK_SPECIAL_GENERAL;
  }
}

std::string NormalizeChannelNameForEpg(const std::string& in)
{
  // Trim and collapse whitespace
  std::string s = Trim(in);
  if (s.empty())
    return s;

  // Decode a few common HTML entities
  auto replace_all = [](std::string& t, const char* from, const char* to) {
    const std::string a(from);
    const std::string b(to);
    size_t pos = 0;
    while ((pos = t.find(a, pos)) != std::string::npos)
    {
      t.replace(pos, a.size(), b);
      pos += b.size();
    }
  };
  replace_all(s, "&amp;", "&");
  replace_all(s, "&quot;", "\"");
  replace_all(s, "&#039;", "'");
  replace_all(s, "&lt;", "<");
  replace_all(s, "&gt;", ">");

  std::string collapsed;
  collapsed.reserve(s.size());
  bool prevSpace = false;
  for (unsigned char ch : s)
  {
    if (std::isspace(ch))
    {
      if (!prevSpace)
        collapsed.push_back(' ');
      prevSpace = true;
      continue;
    }
    prevSpace = false;
    collapsed.push_back(static_cast<char>(ch));
  }

  collapsed = Trim(collapsed);

  // Strip category prefix like "UK | Channel" -> "Channel"
  const std::string sep = " | ";
  const size_t pos = collapsed.rfind(sep);
  if (pos != std::string::npos && pos + sep.size() < collapsed.size())
    collapsed = Trim(collapsed.substr(pos + sep.size()));

  return collapsed;
}

bool IsUnreserved(unsigned char c)
{
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
    return true;
  switch (c)
  {
    case '-':
    case '_':
    case '.':
    case '~':
      return true;
    default:
      return false;
  }
}

std::string UrlEncode(const std::string& s)
{
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s)
  {
    if (IsUnreserved(c))
    {
      out.push_back(static_cast<char>(c));
      continue;
    }
    out.push_back('%');
    out.push_back(kHex[(c >> 4) & 0xF]);
    out.push_back(kHex[c & 0xF]);
  }
  return out;
}

std::string NormalizeServer(const std::string& raw)
{
  std::string s = Trim(raw);
  while (!s.empty() && s.back() == '/')
    s.pop_back();
  return s;
}

std::string BuildBaseUrl(const xtream::Settings& settings)
{
  const std::string server = NormalizeServer(settings.server);
  if (server.empty())
    return {};

  // If the user already included a scheme, trust it.
  if (server.rfind("http://", 0) == 0 || server.rfind("https://", 0) == 0)
  {
    // Only append port when server doesn't already include one.
    // Keep it simple: if it contains "://" and then another ':' after that, assume it already has a port.
    const auto schemePos = server.find("://");
    const auto hostPart = (schemePos == std::string::npos) ? server : server.substr(schemePos + 3);
    if (hostPart.find(':') != std::string::npos)
      return server;
    std::string out = server;
    if (settings.port > 0)
      out += ":" + std::to_string(settings.port);
    return out;
  }
  std::string out = std::string("http://") + server;
  if (settings.port > 0)
    out += ":" + std::to_string(settings.port);
  return out;
}

std::string BuildPlayerApiUrl(const xtream::Settings& settings)
{
  const std::string base = BuildBaseUrl(settings);
  if (base.empty())
    return {};
  std::string url = base + "/player_api.php?username=" + UrlEncode(settings.username) +
                    "&password=" + UrlEncode(settings.password);
  return url;
}

std::string BuildPlayerApiUrlWithAction(const xtream::Settings& settings, const std::string& action)
{
  const std::string base = BuildPlayerApiUrl(settings);
  if (base.empty())
    return {};
  return base + "&action=" + UrlEncode(action);
}

std::string EffectiveUserAgent(const xtream::Settings& settings)
{
  if (!settings.enableUserAgentSpoofing)
    return {};

  const std::string ua = Trim(settings.customUserAgent);
  return ua.empty() ? std::string(kDefaultAddonUserAgent) : ua;
}

std::string AppendUserAgentHeader(const std::string& url, const xtream::Settings& settings)
{
  const std::string ua = EffectiveUserAgent(settings);
  if (ua.empty())
    return url;
  return url + "|User-Agent=" + UrlEncode(ua);
}

std::string RedactUrlCredentials(const std::string& url)
{
  // Avoid logging usernames/passwords by default.
  // Example: ...player_api.php?username=USER&password=PASS&action=...
  std::string out = url;
  const auto redactParam = [&](const char* key) {
    const std::string needle = std::string(key) + "=";
    size_t pos = 0;
    while (true)
    {
      pos = out.find(needle, pos);
      if (pos == std::string::npos)
        return;
      const size_t valueStart = pos + needle.size();
      size_t valueEnd = out.find('&', valueStart);
      if (valueEnd == std::string::npos)
        valueEnd = out.size();
      out.replace(valueStart, valueEnd - valueStart, "***");
      pos = valueStart + 3;
    }
  };

  redactParam("username");
  redactParam("password");
  return out;
}

bool ReadAll(kodi::vfs::CFile& file, std::string& out, size_t maxBytes)
{
  out.clear();
  char buf[16 * 1024];
  while (true)
  {
    // Use int for portability across compilers (MSVC).
    const int n = file.Read(buf, sizeof(buf));
    if (n <= 0)
      break;
    out.append(buf, static_cast<size_t>(n));
    if (out.size() > maxBytes)
      return false;
  }
  return true;
}

bool ReadAll(kodi::vfs::CFile& file, std::string& out)
{
  return ReadAll(file, out, std::numeric_limits<size_t>::max());
}

bool IsHttpStatusOk(const std::string& protocol)
{
  // protocol string looks like "HTTP/1.1 200 OK" or empty on some transports.
  const size_t firstSpace = protocol.find(' ');
  if (firstSpace == std::string::npos)
    return false;
  const size_t secondSpace = protocol.find(' ', firstSpace + 1);
  if (secondSpace == std::string::npos || secondSpace <= firstSpace + 1)
    return false;

  const std::string codeStr = protocol.substr(firstSpace + 1, secondSpace - firstSpace - 1);
  try
  {
    const int code = std::stoi(codeStr);
    return code >= 200 && code < 300;
  }
  catch (...)
  {
    return false;
  }
}

struct HttpResult
{
  bool ok = false;
  std::string protocol;
  std::string body;
};

bool ReadVfsTextFile(const std::string& url, std::string& out)
{
  out.clear();
  kodi::vfs::CFile file;
  file.CURLCreate(url);
  if (!file.CURLOpen(0))
    return false;
  ReadAll(file, out);
  return true;
}

bool ExtractSettingValue(const std::string& xml, const char* id, std::string& out)
{
  out.clear();
  const std::string needle = std::string("<setting id=\"") + id + "\"";
  size_t pos = xml.find(needle);
  if (pos == std::string::npos)
    return false;

  pos = xml.find('>', pos);
  if (pos == std::string::npos)
    return false;
  ++pos;

  // Handle self-closing settings e.g. <setting id="x" default="true" />
  if (pos < xml.size() && xml[pos - 1] == '/' && xml[pos] == '>')
    return true;

  const size_t end = xml.find("</setting>", pos);
  if (end == std::string::npos)
    return false;

  out = Trim(xml.substr(pos, end - pos));
  return true;
}

bool ExtractSettingInt(const std::string& xml, const char* id, int& out)
{
  std::string s;
  if (!ExtractSettingValue(xml, id, s))
    return false;
  if (s.empty())
    return false;
  try
  {
    out = std::stoi(s);
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool ExtractSettingBool(const std::string& xml, const char* id, bool& out)
{
  std::string s;
  if (!ExtractSettingValue(xml, id, s))
    return false;
  if (s.empty())
    return false;
  const std::string v = ToLower(Trim(s));
  if (v == "true" || v == "1" || v == "yes")
  {
    out = true;
    return true;
  }
  if (v == "false" || v == "0" || v == "no")
  {
    out = false;
    return true;
  }
  return false;
}

HttpResult HttpGet(const std::string& url,
                   const std::string& userAgent,
                   int timeoutSeconds,
                   size_t maxBodyBytes = kMaxHttpBodyBytes)
{
  HttpResult result;

  const std::string redacted = RedactUrlCredentials(url);
  kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: HTTP GET %s", redacted.c_str());

  kodi::vfs::CFile file;
  file.CURLCreate(url);

  if (!userAgent.empty())
    file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "user-agent", userAgent);

  if (timeoutSeconds > 0)
  {
    const std::string t = std::to_string(timeoutSeconds);
    file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "connection-timeout", t);
    file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "timeout", t);
  }

  // Be tolerant of providers that redirect.
  file.CURLAddOption(ADDON_CURL_OPTION_PROTOCOL, "followlocation", "1");

  if (!file.CURLOpen(0))
    return result;

  result.protocol = file.GetPropertyValue(ADDON_FILE_PROPERTY_RESPONSE_PROTOCOL, "");
  if (!ReadAll(file, result.body, maxBodyBytes))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: HTTP response exceeded %zu bytes for %s",
              maxBodyBytes, redacted.c_str());
    result.protocol = result.protocol.empty() ? std::string("Body too large") : result.protocol;
    return result;
  }

  const bool looksHttpOk = IsHttpStatusOk(result.protocol);
  result.ok = looksHttpOk;
  if (!result.ok && result.protocol.empty())
    result.protocol = result.body.empty() ? std::string("Empty response") : std::string("Unexpected response");
  return result;
}

// Iterate JSON object spans from a top-level array of objects.
// Avoids allocating/copying one std::string per object (important for 5k-40k channels).
template<typename Fn>
bool ForEachTopLevelObjectSpan(const std::string& json, Fn&& fn)
{
  const auto n = json.size();
  size_t i = 0;
  while (i < n && std::isspace(static_cast<unsigned char>(json[i])))
    ++i;
  if (i >= n || json[i] != '[')
    return false;

  bool inString = false;
  bool escape = false;
  int depth = 0;
  size_t objStart = std::string::npos;
  bool any = false;

  for (; i < n; ++i)
  {
    const char c = json[i];
    if (inString)
    {
      if (escape)
      {
        escape = false;
        continue;
      }
      if (c == '\\')
      {
        escape = true;
        continue;
      }
      if (c == '"')
      {
        inString = false;
        continue;
      }
      continue;
    }

    if (c == '"')
    {
      inString = true;
      continue;
    }

    if (c == '{')
    {
      if (depth == 0)
        objStart = i;
      ++depth;
      continue;
    }
    if (c == '}')
    {
      --depth;
      if (depth == 0 && objStart != std::string::npos)
      {
        any = true;
        fn(objStart, i + 1);
        objStart = std::string::npos;
      }
      continue;
    }
  }

  return any;
}

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

  // Some providers return numeric fields as strings.
  if (obj[pos] == '"')
  {
    ++pos;
    bool neg = false;
    if (pos < obj.size() && obj[pos] == '-')
    {
      neg = true;
      ++pos;
    }
    long long v = 0;
    bool any = false;
    while (pos < obj.size() && std::isdigit(static_cast<unsigned char>(obj[pos])))
    {
      any = true;
      v = v * 10 + (obj[pos] - '0');
      ++pos;
    }
    if (!any)
      return false;
    out = static_cast<int>(neg ? -v : v);
    return true;
  }

  bool neg = false;
  if (obj[pos] == '-')
  {
    neg = true;
    ++pos;
  }
  long long v = 0;
  bool any = false;
  while (pos < obj.size() && std::isdigit(static_cast<unsigned char>(obj[pos])))
  {
    any = true;
    v = v * 10 + (obj[pos] - '0');
    ++pos;
  }
  if (!any)
    return false;
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
  if (!FindKeyPos(obj, key, pos))
    return false;
  pos = obj.find(':', pos);
  if (pos == std::string::npos)
    return false;
  ++pos;
  while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos])))
    ++pos;
  if (pos >= obj.size())
    return false;

  // Check for boolean literals
  if (obj.substr(pos, 4) == "true")
  {
    out = true;
    return true;
  }
  if (obj.substr(pos, 5) == "false")
  {
    out = false;
    return true;
  }

  // Check for numeric representation (1 = true, 0 = false)
  if (obj[pos] == '1')
  {
    out = true;
    return true;
  }
  if (obj[pos] == '0')
  {
    out = false;
    return true;
  }

  // Check for string representation
  if (obj[pos] == '"')
  {
    ++pos;
    if (pos < obj.size())
    {
      if (obj[pos] == '1')
      {
        out = true;
        return true;
      }
      if (obj[pos] == '0')
      {
        out = false;
        return true;
      }
    }
  }

  return false;
}

bool ExtractStringField(std::string_view obj, const std::string& key, std::string& out)
{
  size_t pos = 0;
  if (!FindKeyPos(obj, key, pos))
    return false;
  pos = obj.find(':', pos);
  if (pos == std::string::npos)
    return false;
  ++pos;
  while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos])))
    ++pos;
  if (pos >= obj.size() || obj[pos] != '"')
    return false;
  ++pos;

  std::string s;
  s.reserve(64);
  bool escape2 = false;
  for (; pos < obj.size(); ++pos)
  {
    const char c = obj[pos];
    if (escape2)
    {
      // Minimal escape handling; keep unknown escapes verbatim.
      auto hexVal = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
          return ch - '0';
        if (ch >= 'a' && ch <= 'f')
          return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F')
          return 10 + (ch - 'A');
        return -1;
      };

      auto appendUtf8 = [](std::string& dst, uint32_t cp) {
        if (cp <= 0x7F)
        {
          dst.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF)
        {
          dst.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
          dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF)
        {
          dst.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
          dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
          dst.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
          dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
          dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
      };

      switch (c)
      {
        case '"':
        case '\\':
        case '/':
          s.push_back(c);
          break;
        case 'b':
          s.push_back('\b');
          break;
        case 'f':
          s.push_back('\f');
          break;
        case 'n':
          s.push_back('\n');
          break;
        case 'r':
          s.push_back('\r');
          break;
        case 't':
          s.push_back('\t');
          break;
        case 'u':
        {
          // Parse \uXXXX sequence, optionally a surrogate pair.
          if (pos + 4 < obj.size())
          {
            int h1 = hexVal(obj[pos + 1]);
            int h2 = hexVal(obj[pos + 2]);
            int h3 = hexVal(obj[pos + 3]);
            int h4 = hexVal(obj[pos + 4]);
            if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0)
            {
              uint32_t cu = static_cast<uint32_t>((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
              pos += 4; // advance to last hex digit; loop ++pos moves past it

              // Handle surrogate pair: \uD800-\uDBFF followed by \uDC00-\uDFFF
              if (cu >= 0xD800 && cu <= 0xDBFF)
              {
                if (pos + 2 + 4 < obj.size() && obj[pos + 1] == '\\' && obj[pos + 2] == 'u')
                {
                  int l1 = hexVal(obj[pos + 3]);
                  int l2 = hexVal(obj[pos + 4]);
                  int l3 = hexVal(obj[pos + 5]);
                  int l4 = hexVal(obj[pos + 6]);
                  uint32_t lo = 0;
                  if (l1 >= 0 && l2 >= 0 && l3 >= 0 && l4 >= 0)
                  {
                    lo = static_cast<uint32_t>((l1 << 12) | (l2 << 8) | (l3 << 4) | l4);
                  }
                  if (lo >= 0xDC00 && lo <= 0xDFFF)
                  {
                    uint32_t hi = cu - 0xD800;
                    lo -= 0xDC00;
                    uint32_t cp = 0x10000 + ((hi << 10) | lo);
                    appendUtf8(s, cp);
                    pos += 6; // we were at last hex of first; skip \\ 'u' and 4 hex of second
                    break;
                  }
                }
              }

              // Non-surrogate or invalid pair: just emit first code unit
              if (cu >= 0xD800 && cu <= 0xDFFF)
              {
                // Lone surrogate: replace with replacement char
                appendUtf8(s, 0xFFFD);
              }
              else
              {
                appendUtf8(s, cu);
              }
              break;
            }
          }
          // Fallback if malformed: keep as literal 'u'
          s.push_back('u');
          break;
        }
        default:
          s.push_back(c);
          break;
      }
      escape2 = false;
      continue;
    }
    if (c == '\\')
    {
      escape2 = true;
      continue;
    }
    if (c == '"')
    {
      out = s;
      return true;
    }
    s.push_back(c);
  }

  return false;
}

xtream::TestResult MakeSimpleGetAndSniffJson(const std::string& url,
                                            const std::string& userAgent,
                                            int timeoutSeconds)
{
  xtream::TestResult result;

  const HttpResult http = HttpGet(url, userAgent, timeoutSeconds);
  if (!http.ok)
  {
    result.ok = false;
    result.details = http.protocol.empty() ? "Failed to open URL" : http.protocol;
    return result;
  }

  const std::string protocol = http.protocol;
  const std::string& body = http.body;

  // Heuristic: typical Xtream response includes user_info/server_info.
  const std::string bodyLower = ToLower(body);
  const bool looksXtream =
      (bodyLower.find("\"user_info\"") != std::string::npos) ||
      (bodyLower.find("\"server_info\"") != std::string::npos) ||
      (bodyLower.find("\"auth\":1") != std::string::npos);

  const bool looksHttpOk = (protocol.find(" 200 ") != std::string::npos) || (protocol.find(" 201 ") != std::string::npos);

  if (looksXtream || looksHttpOk)
  {
    result.ok = true;
    if (!protocol.empty())
      result.details = protocol;
    else
      result.details = "OK";
    return result;
  }

  result.ok = false;
  if (!protocol.empty())
    result.details = protocol;
  else if (!body.empty())
    result.details = "Unexpected response";
  else
    result.details = "Empty response";
  return result;
}
} // namespace

namespace xtream
{
Settings LoadSettings()
{
  Settings s;
  kodi::addon::GetSettingString("server", s.server);
  kodi::addon::GetSettingInt("port", s.port);
  kodi::addon::GetSettingString("username", s.username);
  kodi::addon::GetSettingString("password", s.password);
  kodi::addon::GetSettingString("dispatcharr_password", s.dispatcharrPassword);
  kodi::addon::GetSettingInt("timeout_seconds", s.timeoutSeconds);
  kodi::addon::GetSettingString("api_mode", s.apiMode);
  if (s.apiMode.empty())
    s.apiMode = "xtream";
  kodi::addon::GetSettingInt("catchup_start_offset_hours", s.catchupStartOffsetHours);
  kodi::addon::GetSettingBoolean("enable_user_agent_spoofing", s.enableUserAgentSpoofing);
  kodi::addon::GetSettingString("custom_user_agent", s.customUserAgent);
  kodi::addon::GetSettingBoolean("enable_play_from_start", s.enablePlayFromStart);
  kodi::addon::GetSettingBoolean("use_ffmpegdirect", s.useFFmpegDirect);
  kodi::addon::GetSettingInt("xmltv_max_size_mb", s.xmltvMaxSizeMb);

  // Kodi sometimes doesn't transfer settings to binary addons early during startup.
  // Always read persisted settings.xml from addon_data and overlay any values found.
  {
    std::string xml;
    if (ReadVfsTextFile("special://profile/addon_data/pvr.dispatcharr/settings.xml", xml))
    {
      std::string tmp;
      if (ExtractSettingValue(xml, "server", tmp))
        s.server = tmp;
      ExtractSettingInt(xml, "port", s.port);
      if (ExtractSettingValue(xml, "username", tmp))
        s.username = tmp;
      if (ExtractSettingValue(xml, "password", tmp))
        s.password = tmp;
      if (ExtractSettingValue(xml, "dispatcharr_password", tmp))
        s.dispatcharrPassword = tmp;
      ExtractSettingInt(xml, "timeout_seconds", s.timeoutSeconds);
      if (ExtractSettingValue(xml, "api_mode", tmp) && !tmp.empty())
        s.apiMode = tmp;
      ExtractSettingInt(xml, "catchup_start_offset_hours", s.catchupStartOffsetHours);
      ExtractSettingBool(xml, "enable_user_agent_spoofing", s.enableUserAgentSpoofing);
      if (ExtractSettingValue(xml, "custom_user_agent", tmp))
        s.customUserAgent = tmp;
      ExtractSettingBool(xml, "enable_play_from_start", s.enablePlayFromStart);
      ExtractSettingBool(xml, "use_ffmpegdirect", s.useFFmpegDirect);
      ExtractSettingInt(xml, "xmltv_max_size_mb", s.xmltvMaxSizeMb);
    }
  }
  return s;
}

TestResult TestConnection(const Settings& settings)
{
  if (Trim(settings.server).empty())
    return {false, "Server is empty"};
  if (settings.port <= 0 || settings.port > 65535)
    return {false, "Port is invalid"};
  if (Trim(settings.username).empty())
    return {false, "Username is empty"};
  if (Trim(settings.password).empty())
    return {false, "Password is empty"};

  const std::string url = BuildPlayerApiUrl(settings);
  if (url.empty())
    return {false, "Failed to build API URL"};

  const std::string ua = EffectiveUserAgent(settings);
  const auto res = MakeSimpleGetAndSniffJson(url, ua, settings.timeoutSeconds);
  return res;
}

FetchResult FetchLiveCategories(const Settings& settings, std::vector<LiveCategory>& out)
{
  out.clear();

  const std::string url = BuildPlayerApiUrlWithAction(settings, "get_live_categories");
  if (url.empty())
    return {false, "Failed to build categories URL"};

  const std::string ua = EffectiveUserAgent(settings);
  const HttpResult http = HttpGet(url, ua, settings.timeoutSeconds);
  if (!http.ok)
    return {false, http.protocol.empty() ? std::string("Failed to fetch categories") : http.protocol};

  if (!ForEachTopLevelObjectSpan(http.body, [&](size_t start, size_t end) {
        std::string_view obj(http.body.data() + start, end - start);
        LiveCategory c;
        if (!ExtractIntField(obj, "category_id", c.id))
          return;
        ExtractStringField(obj, "category_name", c.name);
        out.push_back(std::move(c));
      }))
    return {false, "Categories response was not a JSON array"};

  if (out.empty())
    return {false, "No categories parsed"};

  return {true, http.protocol.empty() ? std::string("OK") : http.protocol};
}

FetchResult FetchLiveStreams(const Settings& settings, int categoryId, std::vector<LiveStream>& out)
{
  out.clear();

  std::string url = BuildPlayerApiUrlWithAction(settings, "get_live_streams");
  if (url.empty())
    return {false, "Failed to build streams URL"};

  if (categoryId > 0)
  {
    url += "&category_id=" + std::to_string(categoryId);
  }

  const std::string ua = EffectiveUserAgent(settings);
  const HttpResult http = HttpGet(url, ua, settings.timeoutSeconds);
  if (!http.ok)
    return {false, http.protocol.empty() ? std::string("Failed to fetch streams") : http.protocol};

  if (!ForEachTopLevelObjectSpan(http.body, [&](size_t start, size_t end) {
        std::string_view obj(http.body.data() + start, end - start);
        LiveStream s;
        if (!ExtractIntField(obj, "stream_id", s.id))
          return;
        ExtractIntField(obj, "category_id", s.categoryId);
        ExtractIntField(obj, "num", s.number);
        ExtractStringField(obj, "name", s.name);
        ExtractStringField(obj, "stream_icon", s.icon);
        ExtractStringField(obj, "epg_channel_id", s.epgChannelId);
        ExtractBoolField(obj, "tv_archive", s.tvArchive);
        ExtractIntField(obj, "tv_archive_duration", s.tvArchiveDuration);
        out.push_back(std::move(s));
      }))
    return {false, "Streams response was not a JSON array"};

  if (out.empty())
    return {false, "No streams parsed"};

  return {true, http.protocol.empty() ? std::string("OK") : http.protocol};
}

FetchResult FetchAllLiveStreams(const Settings& settings,
                                std::vector<LiveCategory>& categories,
                                std::vector<LiveStream>& streams)
{
  categories.clear();
  streams.clear();

  std::vector<LiveCategory> cats;
  const FetchResult catsRes = FetchLiveCategories(settings, cats);
  if (!catsRes.ok)
    return catsRes;

  // Prefer single-call variant: vastly faster and scales to 40k+ channels.
  std::vector<LiveStream> streamsAll;
  const FetchResult allRes = FetchLiveStreams(settings, 0, streamsAll);
  if (allRes.ok)
  {
    categories = std::move(cats);
    streams = std::move(streamsAll);
    return {true, allRes.details};
  }

  std::vector<LiveStream> all;
  for (const auto& c : cats)
  {
    std::vector<LiveStream> s;
    const FetchResult r = FetchLiveStreams(settings, c.id, s);
    if (!r.ok)
    {
      return r;
    }
    all.insert(all.end(), s.begin(), s.end());
  }

  categories = std::move(cats);
  streams = std::move(all);
  return {true, catsRes.details};
}

std::string BuildLiveStreamUrl(const Settings& settings, int streamId, const std::string& streamFormat)
{
  const std::string base = BuildBaseUrl(settings);
  if (base.empty() || streamId <= 0)
    return {};

  std::string ext = ".ts";
  if (ToLower(streamFormat) == "hls")
    ext = ".m3u8";

  std::string url = base + "/live/" + UrlEncode(settings.username) + "/" +
                    UrlEncode(settings.password) + "/" + std::to_string(streamId) + ext;
  return AppendUserAgentHeader(url, settings);
}

std::string BuildCatchupUrl(const Settings& settings, int streamId, time_t startTime, time_t endTime, const std::string& streamFormat)
{
  const std::string base = BuildBaseUrl(settings);
  if (base.empty() || streamId <= 0 || startTime <= 0 || endTime <= startTime)
    return {};

  // Apply catchup start offset (convert hours to seconds; clamp negative to 0)
  int offsetHours = settings.catchupStartOffsetHours;
  if (offsetHours < 0)
  {
    kodi::Log(ADDON_LOG_WARNING, "BuildCatchupUrl: negative catchupStartOffsetHours=%d; clamping to 0", offsetHours);
    offsetHours = 0;
  }
  time_t adjustedStartTime = startTime + (offsetHours * 3600);
  
  // Calculate duration in minutes
  const int durationMinutes = static_cast<int>((endTime - adjustedStartTime) / 60);
  if (durationMinutes <= 0)
    return {};

  // The Xtream server expects times in the same timezone as the EPG data.
  // The EPG timestamps from Kodi are Unix timestamps (UTC epoch seconds).
  // The Xtream API returns EPG times in UTC, so we use gmtime to format.
  // No timezone adjustment is needed - the timestamp is already correct.
  
  // Format start time as YYYY-MM-DD:HH-MM (use gmtime_r for thread safety)
  std::tm tmBuf = {};
  std::tm* tm = gmtime_r(&adjustedStartTime, &tmBuf);
  if (!tm)
    return {};

  char timeBuffer[32];
  std::snprintf(timeBuffer, sizeof(timeBuffer), "%04d-%02d-%02d:%02d-%02d",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min);

  std::string ext = ".ts";
  if (ToLower(streamFormat) == "hls")
    ext = ".m3u8";

  // Format: server:port/timeshift/user/pass/durationinminutes/YYYY-MM-DD:HH-MM/streamid.ts
  std::string url = base + "/timeshift/" + UrlEncode(settings.username) + "/" +
                    UrlEncode(settings.password) + "/" + std::to_string(durationMinutes) + "/" +
                    std::string(timeBuffer) + "/" + std::to_string(streamId) + ext;
  return AppendUserAgentHeader(url, settings);
}

std::string BuildCatchupUrlTemplate(const Settings& settings, int streamId, int durationMinutes, const std::string& streamFormat)
{
  // Build a URL template with ffmpegdirect placeholders for catchup seeking.
  // ffmpegdirect will substitute {Y}, {m}, {d}, {H}, {M} with the seek position's date/time.
  const std::string base = BuildBaseUrl(settings);
  if (base.empty() || streamId <= 0 || durationMinutes <= 0)
    return {};

  std::string ext = ".ts";
  if (ToLower(streamFormat) == "hls")
    ext = ".m3u8";

  // Format: server:port/timeshift/user/pass/durationinminutes/{Y}-{m}-{d}:{H}-{M}/streamid.ts
  // The placeholders {Y}, {m}, {d}, {H}, {M} are substituted by ffmpegdirect when seeking.
  std::string url = base + "/timeshift/" + UrlEncode(settings.username) + "/" +
                    UrlEncode(settings.password) + "/" + std::to_string(durationMinutes) + "/" +
                    "{Y}-{m}-{d}:{H}-{M}/" + std::to_string(streamId) + ext;
  return AppendUserAgentHeader(url, settings);
}

FetchResult FetchXMLTVEpg(const Settings& settings, std::string& xmltvData)
{
  xmltvData.clear();

  const std::string base = BuildBaseUrl(settings);
  if (base.empty())
    return {false, "Failed to build base URL"};

  // Build XMLTV URL: http://domain:port/xmltv.php?username=X&password=Y
  std::string url = base + "/xmltv.php?username=" + UrlEncode(settings.username) +
                    "&password=" + UrlEncode(settings.password);

  const std::string ua = EffectiveUserAgent(settings);
  const int configuredMb = settings.xmltvMaxSizeMb > 0 ? settings.xmltvMaxSizeMb : 300;
  const size_t maxBytes = static_cast<size_t>(configuredMb) * 1024 * 1024;
  const HttpResult http = HttpGet(url, ua, settings.timeoutSeconds, maxBytes);

  if (!http.ok)
    return {false, http.protocol.empty() ? std::string("Failed to fetch XMLTV") : http.protocol};

  xmltvData = http.body;
  
  // Basic validation - check if it looks like XML
  if (xmltvData.empty())
    return {false, "XMLTV response is empty"};
    
  if (xmltvData.find("<?xml") == std::string::npos && xmltvData.find("<tv") == std::string::npos)
    return {false, "XMLTV response doesn't appear to be XML"};

  return {true, http.protocol.empty() ? std::string("OK") : http.protocol};
}

bool ParseXMLTV(const std::string& xmltvData, 
                const std::vector<LiveStream>& streams,
                std::vector<ChannelEpg>& channelEpgs)
{
  channelEpgs.clear();

  if (xmltvData.empty())
    return false;

  // Parse XML
  pugi::xml_document doc;
  std::vector<char> xmlBuffer(xmltvData.begin(), xmltvData.end());
  xmlBuffer.push_back('\0');
  pugi::xml_parse_result result = doc.load_buffer_inplace(
      xmlBuffer.data(),
      xmlBuffer.size() - 1,
      pugi::parse_default | pugi::parse_declaration);
  
  if (!result)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Failed to parse XMLTV: %s (offset: %d)", 
              result.description(), static_cast<int>(result.offset));
    return false;
  }

  const auto& tvNode = doc.child("tv");
  if (!tvNode)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: XMLTV missing <tv> root element");
    return false;
  }

  // Create a lookup map of stream ID to stream name for matching
  std::unordered_map<int, std::string> streamIdToName;
  std::unordered_map<std::string, std::vector<int>> streamNameToIds;
  std::unordered_map<std::string, std::vector<int>> xmltvIdToStreamIds;
  for (const auto& stream : streams)
  {
    if (stream.id > 0)
    {
      streamIdToName[stream.id] = stream.name;
      const std::string normName = NormalizeChannelNameForEpg(stream.name);
      if (!normName.empty())
        streamNameToIds[ToLower(normName)].push_back(stream.id);

      if (!stream.epgChannelId.empty())
        xmltvIdToStreamIds[stream.epgChannelId].push_back(stream.id);
    }
  }

  // First pass: Parse channel elements and match to our streams
  std::unordered_map<std::string, ChannelEpg> epgMap;
  std::unordered_map<std::string, std::vector<std::string>> xmltvIdToMappedIds;
  int totalXmltvChannels = 0;
  int mappedByNumericId = 0;
  int mappedByEpgId = 0;
  int mappedByName = 0;
  int unmapped = 0;
  
  for (const auto& channelNode : tvNode.children("channel"))
  {
    const char* idAttr = channelNode.attribute("id").value();
    if (!idAttr || idAttr[0] == '\0')
      continue;

    ChannelEpg epg;
    const std::string xmltvId = idAttr;
    totalXmltvChannels++;
    
    // Get display name
    const auto& displayNameNode = channelNode.child("display-name");
    std::string displayNameNormalized;
    if (displayNameNode)
    {
      epg.displayName = displayNameNode.child_value();
      displayNameNormalized = NormalizeChannelNameForEpg(epg.displayName);
    }
    
    // Get icon
    const auto& iconNode = channelNode.child("icon");
    if (iconNode)
    {
      const char* srcAttr = iconNode.attribute("src").value();
      if (srcAttr && srcAttr[0] != '\0')
        epg.iconPath = srcAttr;
    }

    // Map XMLTV channel ID or display-name to stream ID for Kodi EPG lookup
    std::vector<std::string> mappedIds;
    bool matched = false;

    // Prefer explicit epg_channel_id mapping from stream list
    const auto epgIdIt = xmltvIdToStreamIds.find(xmltvId);
    if (epgIdIt != xmltvIdToStreamIds.end() && !epgIdIt->second.empty())
    {
      for (int streamId : epgIdIt->second)
        mappedIds.push_back(std::to_string(streamId));
      matched = true;
      mappedByEpgId++;
    }

    char* end = nullptr;
    const long numericId = std::strtol(xmltvId.c_str(), &end, 10);
    if (!matched && end && *end == '\0' && numericId > 0)
    {
      const int streamId = static_cast<int>(numericId);
      if (streamIdToName.find(streamId) != streamIdToName.end())
      {
        mappedIds.push_back(std::to_string(streamId));
        matched = true;
        mappedByNumericId++;
      }
    }

    if (!matched && !displayNameNormalized.empty())
    {
      const auto nameIt = streamNameToIds.find(ToLower(displayNameNormalized));
      if (nameIt != streamNameToIds.end() && !nameIt->second.empty())
      {
        for (int streamId : nameIt->second)
          mappedIds.push_back(std::to_string(streamId));
        matched = true;
        mappedByName++;
      }
    }

    if (!matched)
      unmapped++;

    if (mappedIds.empty())
    {
      // Keep XMLTV id to allow debugging but it won't match any stream id.
      epg.id = xmltvId;
      epgMap[epg.id] = epg;
      xmltvIdToMappedIds[xmltvId] = {xmltvId};
      continue;
    }

    xmltvIdToMappedIds[xmltvId] = mappedIds;
    for (const auto& mappedId : mappedIds)
    {
      ChannelEpg& target = epgMap[mappedId];
      if (target.id.empty())
        target.id = mappedId;
      if (target.displayName.empty())
        target.displayName = epg.displayName;
      if (target.iconPath.empty())
        target.iconPath = epg.iconPath;
    }
  }

  kodi::Log(ADDON_LOG_INFO,
            "pvr.dispatcharr: XMLTV channel mapping: total=%d, epg_id=%d, numeric=%d, name=%d, unmapped=%d",
            totalXmltvChannels, mappedByEpgId, mappedByNumericId, mappedByName, unmapped);

  // Second pass: Parse programme elements
  int programmeCount = 0;
  
  for (const auto& programmeNode : tvNode.children("programme"))
  {
    const char* channelAttr = programmeNode.attribute("channel").value();
    if (!channelAttr || channelAttr[0] == '\0')
      continue;

    const std::string xmltvChannelId = channelAttr;
    const auto mapIt = xmltvIdToMappedIds.find(xmltvChannelId);
    if (mapIt == xmltvIdToMappedIds.end() || mapIt->second.empty())
      continue;
    
    EpgEntry entry;
    entry.channelId = xmltvChannelId;

    // Parse start and stop times (format: YYYYMMDDHHmmss +TZ)
    const char* startAttr = programmeNode.attribute("start").value();
    const char* stopAttr = programmeNode.attribute("stop").value();
    
    if (startAttr && startAttr[0] != '\0')
    {
      // Parse XMLTV time format: 20260121120000 +0000
      struct tm tm = {};
      int tzHours = 0, tzMins = 0;
      char tzSign = '+';
      if (sscanf(startAttr, "%4d%2d%2d%2d%2d%2d %c%2d%2d", 
                 &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                 &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                 &tzSign, &tzHours, &tzMins) >= 6)
      {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = 0;
        // timegm interprets the parsed time as UTC
        // But the parsed time is actually in the specified timezone (e.g., +0100)
        // To get the actual UTC time: we need to subtract the offset
        // Example: "20:00 +0100" means 20:00 in UTC+1 = 19:00 in UTC
        // timegm gives us 20:00 UTC, so we subtract 1 hour to get 19:00 UTC
        entry.startTime = timegm(&tm);
        int tzOffsetSeconds = (tzHours * 3600 + tzMins * 60);
        if (tzSign == '-')
          tzOffsetSeconds = -tzOffsetSeconds;
        entry.startTime -= tzOffsetSeconds;
      }
    }
    
    if (stopAttr && stopAttr[0] != '\0')
    {
      struct tm tm = {};
      int tzHours = 0, tzMins = 0;
      char tzSign = '+';
      if (sscanf(stopAttr, "%4d%2d%2d%2d%2d%2d %c%2d%2d", 
                 &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                 &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                 &tzSign, &tzHours, &tzMins) >= 6)
      {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = 0;
        entry.endTime = timegm(&tm);
        int tzOffsetSeconds = (tzHours * 3600 + tzMins * 60);
        if (tzSign == '-')
          tzOffsetSeconds = -tzOffsetSeconds;
        entry.endTime -= tzOffsetSeconds;
      }
    }

    // Skip entries with invalid times
    if (entry.startTime == 0 || entry.endTime == 0 || entry.endTime <= entry.startTime)
      continue;

    // Parse title
    const auto& titleNode = programmeNode.child("title");
    if (titleNode)
      entry.title = titleNode.child_value();

    // Parse description
    const auto& descNode = programmeNode.child("desc");
    if (descNode)
      entry.description = descNode.child_value();

    // Parse sub-title (episode name)
    const auto& subTitleNode = programmeNode.child("sub-title");
    if (subTitleNode)
      entry.episodeName = subTitleNode.child_value();

    // Parse icon
    const auto& iconNode = programmeNode.child("icon");
    if (iconNode)
    {
      const char* srcAttr = iconNode.attribute("src").value();
      if (srcAttr && srcAttr[0] != '\0')
        entry.iconPath = srcAttr;
    }

    // Preserve all XMLTV categories and map recognized values to Kodi's DVB genres.
    ParseProgrammeGenre(programmeNode, entry);

    // Add entry to each mapped stream's EPG (keyed by start time)
    for (const auto& mappedId : mapIt->second)
    {
      auto epgIt = epgMap.find(mappedId);
      if (epgIt == epgMap.end())
        continue;
      epgIt->second.entries[entry.startTime] = entry;
      programmeCount++;
    }
  }

  // Convert map to vector (only channels with EPG entries)
  for (const auto& kv : epgMap)
  {
    if (!kv.second.entries.empty())
      channelEpgs.push_back(kv.second);
  }

  kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: Parsed XMLTV - %d channels, %d programmes",
            static_cast<int>(channelEpgs.size()), programmeCount);

  return !channelEpgs.empty();
}
} // namespace xtream
