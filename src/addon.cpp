#include <kodi/General.h>
#include <kodi/addon-instance/PVR.h>
#include <kodi/gui/dialogs/OK.h>
#include <kodi/Filesystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xtream_client.h"
#include "dispatcharr_client.h"
#include "recording/growing_recorded_stream.h"
#include "recording/remote_file_recorded_stream.h"
#include "recording/native_catchup_live_stream.h"
#include "recording/epg_recording_match.h"

// Platform-specific time functions
#ifdef _WIN32
  // Windows doesn't have localtime_r, use localtime_s instead
  static inline std::tm* localtime_r_compat(const time_t* timer, std::tm* buf)
  {
    return (localtime_s(buf, timer) == 0) ? buf : nullptr;
  }
  #define localtime_r localtime_r_compat
  
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
constexpr auto kEpgRefreshInterval = std::chrono::minutes(60);
constexpr auto kEpgRefreshRetryInterval = std::chrono::minutes(5);
constexpr auto kRecordingPollInterval = std::chrono::seconds(10);

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

uint64_t DeterministicHash64(std::string_view s)
{
  // FNV-1a 64-bit for stability across processes/platforms.
  constexpr uint64_t kPrime = 1099511628211ULL;
  constexpr uint64_t kOffset = 14695981039346656037ULL;
  uint64_t h = kOffset;
  for (unsigned char c : s)
  {
    h ^= static_cast<uint64_t>(c);
    h *= kPrime;
  }
  return h;
}

std::string HashHex(std::string_view s)
{
  const uint64_t h = DeterministicHash64(s);
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return std::string(buf);
}

bool ReadAll(kodi::vfs::CFile& file, std::string& out)
{
  out.clear();
  char buf[16 * 1024];
  while (true)
  {
    const ssize_t n = file.Read(buf, sizeof(buf));
    if (n <= 0)
      break;
    out.append(buf, static_cast<size_t>(n));
  }
  return true;
}

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

std::string TranslateSpecial(const std::string& url)
{
  try
  {
    return kodi::vfs::TranslateSpecialProtocol(url);
  }
  catch (...)
  {
    return {};
  }
}

constexpr uint32_t kCacheMagic = 0x31435458; // 'XTC1' little-endian

void AppendU32(std::string& out, uint32_t v)
{
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void AppendI32(std::string& out, int32_t v)
{
  AppendU32(out, static_cast<uint32_t>(v));
}

void AppendU64(std::string& out, uint64_t v)
{
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool ReadU32(const std::string& in, size_t& off, uint32_t& out)
{
  if (off + 4 > in.size())
    return false;
  out = static_cast<uint32_t>(static_cast<unsigned char>(in[off])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(in[off + 1])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(in[off + 2])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(in[off + 3])) << 24);
  off += 4;
  return true;
}

bool ReadI32(const std::string& in, size_t& off, int32_t& out)
{
  uint32_t u = 0;
  if (!ReadU32(in, off, u))
    return false;
  out = static_cast<int32_t>(u);
  return true;
}

bool ReadU64(const std::string& in, size_t& off, uint64_t& out)
{
  if (off + 8 > in.size())
    return false;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= (static_cast<uint64_t>(static_cast<unsigned char>(in[off + i])) << (8 * i));
  off += 8;
  out = v;
  return true;
}

bool ReadFileToString(const std::string& path, std::string& out)
{
  out.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  f.seekg(0, std::ios::end);
  const std::streamoff sz = f.tellg();
  if (sz <= 0)
    return false;
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(sz));
  f.read(&out[0], sz);
  return f.good();
}

bool WriteStringToFileAtomic(const std::string& path, const std::string& data)
{
  try
  {
    const std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path());
    const std::string tmp = path + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f)
        return false;
      f.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!f.good())
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
      // Fallback for platforms where rename over existing isn't atomic.
      std::filesystem::remove(path, ec);
      ec.clear();
      std::filesystem::rename(tmp, path, ec);
      if (ec)
        return false;
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
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

std::vector<std::string> SplitPatterns(const std::string& raw)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : raw)
  {
    if (c == ',' || c == '\n' || c == '\r')
    {
      const std::string t = Trim(cur);
      if (!t.empty())
        out.push_back(ToLower(t));
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  const std::string t = Trim(cur);
  if (!t.empty())
    out.push_back(ToLower(t));
  return out;
}

bool LooksLikeChannelSeparator(const std::string& name)
{
  int run = 0;
  for (unsigned char ch : name)
  {
    if (ch == '#')
    {
      ++run;
      if (run >= 4)
        return true;
      continue;
    }
    run = 0;
  }
  return false;
}

// Case-insensitive wildcard match: '*' matches any sequence.
bool WildcardMatchLower(const std::string& patternLower, const std::string& textLower)
{
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string::npos;
  size_t match = 0;
  while (t < textLower.size())
  {
    if (p < patternLower.size() && (patternLower[p] == textLower[t]))
    {
      ++p;
      ++t;
      continue;
    }
    if (p < patternLower.size() && patternLower[p] == '*')
    {
      star = p++;
      match = t;
      continue;
    }
    if (star != std::string::npos)
    {
      p = star + 1;
      t = ++match;
      continue;
    }
    return false;
  }
  while (p < patternLower.size() && patternLower[p] == '*')
    ++p;
  return p == patternLower.size();
}

bool PatternMatchesLower(const std::string& patternLower, const std::string& textLower)
{
  if (patternLower.empty())
    return false;
  if (patternLower.find('*') != std::string::npos)
    return WildcardMatchLower(patternLower, textLower);
  // If no wildcard is present, treat the pattern as a substring match for usability.
  return textLower.find(patternLower) != std::string::npos;
}

bool ShouldFilterOut(const std::vector<std::string>& patternsLower, const std::string& name)
{
  if (patternsLower.empty())
    return false;
  const std::string nameLower = ToLower(name);
  for (const auto& pat : patternsLower)
  {
    if (pat.empty())
      continue;
    if (PatternMatchesLower(pat, nameLower))
      return true;
  }
  return false;
}

// --- Native API translation helpers -------------------------------------
// Adapts dispatcharr::Client's native channel/group/EPG data into the same
// xtream::LiveCategory/LiveStream/ChannelEpg shapes the rest of this file
// already knows how to filter, group, cache, and match EPG against, so
// everything downstream of the fetch is shared between api_mode "xtream"
// and "native" unchanged.

bool FetchNativeCategoriesAndStreams(dispatcharr::Client& client,
                                     std::vector<xtream::LiveCategory>& categories,
                                     std::vector<xtream::LiveStream>& streams,
                                     std::string& errorDetails)
{
  std::vector<dispatcharr::ChannelGroup> groups;
  if (!client.FetchChannelGroups(groups))
  {
    errorDetails = "failed to fetch channel groups";
    return false;
  }

  std::vector<dispatcharr::DispatchChannel> nativeChannels;
  if (!client.FetchChannels(nativeChannels))
  {
    errorDetails = "failed to fetch channels";
    return false;
  }

  // Best-effort: a failed logo fetch just means channels load without icons.
  std::map<int, std::string> logoUrlsById;
  client.FetchLogos(logoUrlsById);

  categories.clear();
  categories.reserve(groups.size());
  for (const auto& g : groups)
  {
    xtream::LiveCategory c;
    c.id = g.id;
    c.name = g.name;
    categories.push_back(std::move(c));
  }

  streams.clear();
  streams.reserve(nativeChannels.size());
  for (const auto& ch : nativeChannels)
  {
    xtream::LiveStream s;
    s.id = ch.id;
    s.categoryId = ch.groupId;
    s.number = ch.channelNumber;
    s.name = ch.name;
    s.epgChannelId = ch.tvgId;
    s.tvArchive = ch.isCatchup;
    s.tvArchiveDuration = ch.catchupDays;
    s.uuid = ch.uuid;
    if (ch.logoId > 0)
    {
      const auto logoIt = logoUrlsById.find(ch.logoId);
      if (logoIt != logoUrlsById.end())
        s.icon = logoIt->second;
    }
    streams.push_back(std::move(s));
  }

  return true;
}

bool FetchNativeEpg(dispatcharr::Client& client,
                    const std::vector<xtream::LiveStream>& streams,
                    std::vector<xtream::ChannelEpg>& out)
{
  std::unordered_map<std::string, int> tvgIdToStreamId;
  tvgIdToStreamId.reserve(streams.size());
  for (const auto& s : streams)
  {
    if (!s.epgChannelId.empty())
      tvgIdToStreamId[s.epgChannelId] = s.id;
  }

  // Wide enough to cover typical forward EPG guide depth plus the longest
  // realistic catch-up archive window; refreshed hourly by the same EPG
  // worker thread that drives the Xtream XMLTV path (kEpgRefreshInterval).
  const time_t now = std::time(nullptr);
  const time_t start = now - (14 * 24 * 3600);
  const time_t end = now + (7 * 24 * 3600);

  std::vector<dispatcharr::EpgProgram> programs;
  if (!client.FetchEpgGrid(start, end, programs))
    return false;

  std::unordered_map<std::string, xtream::ChannelEpg> epgMap;
  for (const auto& p : programs)
  {
    const auto idIt = tvgIdToStreamId.find(p.tvgId);
    if (idIt == tvgIdToStreamId.end())
      continue;

    const std::string channelIdStr = std::to_string(idIt->second);
    xtream::ChannelEpg& target = epgMap[channelIdStr];
    target.id = channelIdStr;

    xtream::EpgEntry entry;
    entry.channelId = channelIdStr;
    entry.startTime = p.startTime;
    entry.endTime = p.endTime;
    entry.title = p.title;
    entry.description = p.description;
    entry.episodeName = p.subtitle;
    target.entries[p.startTime] = std::move(entry);
  }

  out.clear();
  out.reserve(epgMap.size());
  for (auto& kv : epgMap)
    out.push_back(std::move(kv.second));
  return true;
}

}

class ATTR_DLL_LOCAL CXtreamCodesPVRClient final : public kodi::addon::CInstancePVRClient
{
public:
  explicit CXtreamCodesPVRClient(const kodi::addon::IInstanceInfo& instance)
    : CInstancePVRClient(instance)
  {
    kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: instance created");
    StartEpgWorkerThread();
    const std::string cacheDirectory =
        TranslateSpecial("special://temp/pvr.dispatcharr/recorded-stream");
    if (!cacheDirectory.empty())
      dispatcharr::recording::GrowingRecordedStream::CleanupStaleFiles(cacheDirectory);
    StartRecordingMonitorThread();
    StartBootstrapThread();
  }

  ~CXtreamCodesPVRClient() override
  {
    if (m_activeRecordedStream)
      m_activeRecordedStream->Close();
    m_stopRequested = true;
    m_cv.notify_all();
    m_epgCv.notify_all();
    m_recordingCv.notify_all();
    if (m_bootstrap.joinable())
      m_bootstrap.join();
    if (m_worker.joinable())
      m_worker.join();
    if (m_epgWorker.joinable())
      m_epgWorker.join();
    if (m_recordingMonitor.joinable())
      m_recordingMonitor.join();
  }

  void SetSettingsOverride(const xtream::Settings& settings)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settingsOverride = settings;
    m_hasSettingsOverride = true;
    // Also update m_xtreamSettings so that immediate operations (like catchup URL generation)
    // use the latest settings without waiting for a full reload
    m_xtreamSettings = settings;
  }

  void ClearSettingsOverride()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hasSettingsOverride = false;
  }

  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override
  {
    capabilities.SetSupportsTV(true);
    capabilities.SetSupportsRadio(false);

    // Xtream live categories -> Kodi channel groups.
    // With large channel counts, groups are split alphabetically to keep each group's
    // member count manageable, preventing UI blocking on GetChannelGroupMembers().
    capabilities.SetSupportsChannelGroups(true);

    // EPG support via XMLTV from Xtream Codes server
    capabilities.SetSupportsEPG(true);

    // Regular live/recording playback provides a STREAMURL and lets Kodi
    // handle it as usual. This capability only additionally enables
    // OpenLiveStream/ReadLiveStream/SeekLiveStream/LengthLiveStream as an
    // opt-in per-stream alternative - selected only for native catchup (see
    // GetEPGTagStreamProperties/GetChannelStreamProperties), which returns
    // no STREAMURL specifically to route into that path instead.
    capabilities.SetHandlesInputStream(true);

    // DVR/Recording support via Dispatcharr backend
    capabilities.SetSupportsRecordings(true);
    capabilities.SetSupportsTimers(true);

    return PVR_ERROR_NO_ERROR;
  }

  void TriggerKodiRefreshThrottled()
  {
    const auto now = std::chrono::steady_clock::now();
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const int64_t last = m_lastRefreshTriggerMs.load();
    if (last != 0 && (ms - last) < 2000)
      return;
    m_lastRefreshTriggerMs.store(ms);
    // Refresh channels first, then groups. This prevents Kodi from trying to import
    // group members against an empty/stale channel map.
    TriggerChannelUpdate();
    TriggerChannelGroupsUpdate();
  }

  void RequestReloadNow()
  {
    // Schedules (but does not block on) a background reload immediately.
    EnsureLoaded();
  }

  PVR_ERROR GetBackendName(std::string& name) override
  {
    name = "Dispatcharr PVR Backend";
    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetBackendVersion(std::string& version) override
  {
    version = ADDON_VERSION;
    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetConnectionString(std::string& connection) override
  {
    xtream::Settings s = xtream::LoadSettings();
    connection = s.server;
    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetChannelsAmount(int& amount) override
  {
    EnsureLoaded();
    std::shared_ptr<const ChannelList> channels;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      channels = m_channels;
    }
    amount = channels ? static_cast<int>(channels->size()) : 0;
    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override
  {
    if (radio)
      return PVR_ERROR_NO_ERROR;

    EnsureLoaded();
    const auto t0 = std::chrono::steady_clock::now();

    std::shared_ptr<const ChannelList> channels;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      channels = m_channels;
    }
    if (!channels)
      return PVR_ERROR_NO_ERROR;

    for (const auto& ch : *channels)
      results.Add(ch);

    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (ms > 500)
      kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: GetChannels returned %zu in %lld ms", channels->size(), static_cast<long long>(ms));

    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) override
  {
    if (deleted)
      return PVR_ERROR_NO_ERROR;

    if (!m_dispatcharrClient)
      return PVR_ERROR_SERVER_ERROR;

    std::vector<dispatcharr::Recording> recordings;
    if (!m_dispatcharrClient->FetchRecordings(recordings))
    {
       // If fetch fails (e.g. auth error, or server not supporting it), 
       // just log and return OK with empty list to avoiding nagging user?
       // Or return SERVER_ERROR.
       kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Failed to fetch recordings");
       return PVR_ERROR_SERVER_ERROR;
    }

    // Filter out future recordings?
    // Dispatcharr "recordings" endpoint returns ALL (past and future/scheduled).
    // Kodi `GetRecordings` expects completed or in-progress recordings. 
    // Future ones should go to `GetTimers`.
    // We filter by status: only show "completed" or "recording" (in-progress)
    // "scheduled" recordings go to the timers list, not recordings list.

    for (const auto& r : recordings)
    {
       // Active recordings are playable through Dispatcharr's HLS endpoint.
       if (r.status != "completed" && r.status != "interrupted" &&
           r.status != "recording")
         continue;

       kodi::addon::PVRRecording rec;
       rec.SetRecordingId(std::to_string(r.id));
       rec.SetTitle(r.title.empty() ? "Unknown Recording" : r.title);
       rec.SetPlot(r.plot);
       rec.SetRecordingTime(r.startTime);
       const time_t effectiveEnd = r.status == "recording"
                                       ? std::min(std::time(nullptr), r.endTime)
                                       : r.endTime;
       int duration = static_cast<int>(effectiveEnd - r.startTime);
       rec.SetDuration(duration > 0 ? duration : 0);
       // Stream URL is provided via GetRecordingStreamProperties
       const int kodiChannelUid = r.kodiChannelUid != 0
                                      ? r.kodiChannelUid
                                      : ResolveKodiChannelUid(r.channelId);
       if (kodiChannelUid > 0)
         rec.SetChannelUid(kodiChannelUid);
       const unsigned int kodiEpgUid = r.kodiEpgUid != EPG_TAG_INVALID_UID
                                           ? r.kodiEpgUid
                                           : ResolveRecordingEpgUid(r, kodiChannelUid);
       if (kodiEpgUid != EPG_TAG_INVALID_UID)
         rec.SetEPGEventId(kodiEpgUid);
       // Set poster image if available
       if (!r.iconPath.empty()) {
           rec.SetIconPath(r.iconPath);
           rec.SetThumbnailPath(r.iconPath);
           rec.SetFanartPath(r.iconPath);
       } 
       
       results.Add(rec);
    }
    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) override
  {
      if (!m_dispatcharrClient) return PVR_ERROR_SERVER_ERROR;
      try {
        int id = std::stoi(recording.GetRecordingId());
        if (m_dispatcharrClient->DeleteRecording(id))
            return PVR_ERROR_NO_ERROR;
      } catch (...) {}
      return PVR_ERROR_FAILED;
  }

  PVR_ERROR GetRecordingStreamProperties(
      const kodi::addon::PVRRecording& recording,
      std::vector<kodi::addon::PVRStreamProperty>& properties) override
  {
    // Deliberately return no STREAMURL property: a static Dispatcharr URL
    // (even with a token baked in) can't survive the JWT expiring mid-
    // playback, since Kodi's player talks to it directly and never calls
    // back into the addon to re-authenticate. Returning no URL instead
    // selects Kodi's native recorded-stream API (Open/Read/Seek/Length
    // RecordedStream below), which goes through Client::Request() and
    // therefore gets the existing 401 retry-and-reauth handling for free.
    if (!m_dispatcharrClient)
      return PVR_ERROR_SERVER_ERROR;

    const std::string recordingId = recording.GetRecordingId();
    int id = 0;
    try
    {
      id = std::stoi(recordingId);
    }
    catch (...)
    {
      kodi::Log(ADDON_LOG_ERROR,
                "pvr.dispatcharr: Invalid recording ID '%s'",
                recordingId.c_str());
      return PVR_ERROR_INVALID_PARAMETERS;
    }

    dispatcharr::Recording recordingInfo;
    if (!m_dispatcharrClient->GetRecording(id, recordingInfo))
    {
      kodi::Log(ADDON_LOG_ERROR,
                "pvr.dispatcharr: Failed to prepare playback for recording %s",
                recordingId.c_str());
      return PVR_ERROR_SERVER_ERROR;
    }

    return PVR_ERROR_NO_ERROR;
  }

  bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override
  {
    if (!m_dispatcharrClient)
      return false;
    int id = 0;
    try { id = std::stoi(recording.GetRecordingId()); }
    catch (...) { return false; }

    dispatcharr::Recording info;
    if (!m_dispatcharrClient->GetRecording(id, info))
      return false;

    if (info.status == "recording")
    {
      const std::string cacheDirectory =
          TranslateSpecial("special://temp/pvr.dispatcharr/recorded-stream");
      if (cacheDirectory.empty())
        return false;
      auto growing = std::make_unique<dispatcharr::recording::GrowingRecordedStream>(
          *m_dispatcharrClient);
      if (!growing->Open(id, cacheDirectory))
        return false;
      m_activeRecordedStream = std::move(growing);
      return true;
    }

    // Completed/interrupted: stream the file directly via authenticated
    // HTTP Range requests instead of handing Kodi a URL with a fixed token.
    auto remoteFile = std::make_unique<dispatcharr::recording::RemoteFileRecordedStream>(
        *m_dispatcharrClient);
    if (!remoteFile->Open(id))
      return false;
    m_activeRecordedStream = std::move(remoteFile);
    return true;
  }

  void CloseRecordedStream() override
  {
    if (m_activeRecordedStream)
      m_activeRecordedStream->Close();
    m_activeRecordedStream.reset();
  }

  int ReadRecordedStream(unsigned char* buffer, unsigned int size) override
  {
    return m_activeRecordedStream ? m_activeRecordedStream->Read(buffer, size) : -1;
  }

  int64_t SeekRecordedStream(int64_t position, int whence) override
  {
    return m_activeRecordedStream ? m_activeRecordedStream->Seek(position, whence) : -1;
  }

  int64_t LengthRecordedStream() override
  {
    return m_activeRecordedStream ? m_activeRecordedStream->Length() : -1;
  }

  PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) override
  {
      using namespace kodi::addon;
      // Type 1: One-Time Recording (manual, time-based)
      // Used when user manually specifies start/end times
      {
          PVRTimerType t;
          t.SetId(1);
          t.SetDescription("One-Time Recording (Manual)");
          t.SetAttributes(
              PVR_TIMER_TYPE_IS_MANUAL |
              PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE |
              PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
              PVR_TIMER_TYPE_SUPPORTS_START_TIME |
              PVR_TIMER_TYPE_SUPPORTS_END_TIME
          );
          types.push_back(t);
      }
      // Type 2: Series Recording (EPG-based, repeating)
      // NOTE: Series rules in Dispatcharr are EPG-based (tvg_id + title match)
      // They do NOT use start/end times - the EPG determines when to record
      {
          PVRTimerType t;
          t.SetId(2);
          t.SetDescription("Series Recording (New Episodes)");
          t.SetAttributes(
              PVR_TIMER_TYPE_IS_REPEATING |
              PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE |
              PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
              PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH |
              PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL
          );
          types.push_back(t);
      }
        // Type 5: Series Recording (All Episodes)
        {
          PVRTimerType t;
          t.SetId(5);
          t.SetDescription("Series Recording (All Episodes)");
          t.SetAttributes(
            PVR_TIMER_TYPE_IS_REPEATING |
            PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE |
            PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
            PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH |
            PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL
          );
          types.push_back(t);
        }
      // Type 3: Recurring Manual (manual, repeating, weekday-based)
      {
          PVRTimerType t;
          t.SetId(3);
          t.SetDescription("Recurring Manual");
          t.SetAttributes(
              PVR_TIMER_TYPE_IS_MANUAL |
              PVR_TIMER_TYPE_IS_REPEATING |
              PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE |
              PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
              PVR_TIMER_TYPE_SUPPORTS_START_TIME |
              PVR_TIMER_TYPE_SUPPORTS_END_TIME |
              PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY |
              PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS
          );
          types.push_back(t);
      }
      // Type 4: One-Time Recording (EPG-based)
      // CRITICAL: This is used when user clicks "Record" on an EPG program
      // Without this type, Kodi falls back to creating a local reminder instead!
      {
          PVRTimerType t;
          t.SetId(4);
          t.SetDescription("One-Time Recording (EPG)");
          t.SetAttributes(
              PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE |
              PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE |
              PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
              PVR_TIMER_TYPE_SUPPORTS_START_TIME |
              PVR_TIMER_TYPE_SUPPORTS_END_TIME
          );
          types.push_back(t);
      }
      return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) override
  {
      if (!m_dispatcharrClient) return PVR_ERROR_SERVER_ERROR;

      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: GetTimers called");

      // 1. Series Rules (Type 2)
      std::vector<dispatcharr::SeriesRule> series;
      if (m_dispatcharrClient->FetchSeriesRules(series)) {
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: GetTimers - fetched %zu series rules", series.size());
          unsigned int seriesIdx = 0;
          for (const auto& s : series) {
              kodi::addon::PVRTimer t;
              // Use index offset by 10000 for series rules
              t.SetClientIndex(10000 + seriesIdx);
              t.SetTitle(s.title.empty() ? "All Shows" : s.title);
              if (ToLower(s.mode) == "all")
                t.SetTimerType(5);
              else
                t.SetTimerType(2);
              t.SetSummary(std::string("Mode: ") + s.mode + " (TVG: " + s.tvgId + ")");
              t.SetState(PVR_TIMER_STATE_SCHEDULED);
              results.Add(t);
              seriesIdx++;
          }
      }

      // 2. Recurring Rules (Type 3)
      std::vector<dispatcharr::RecurringRule> recurring;
      if (m_dispatcharrClient->FetchRecurringRules(recurring)) {
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: GetTimers - fetched %zu recurring rules", recurring.size());
          for (const auto& r : recurring) {
              kodi::addon::PVRTimer t;
              // Use the rule ID offset by 20000 to avoid collision with series IDs
              t.SetClientIndex(static_cast<unsigned int>(20000 + r.id));
              t.SetTitle(r.name.empty() ? "Recurring" : r.name);
              t.SetTimerType(3);
              // Map Dispatcharr channel ID back to Kodi channel UID
              int kodiUid = ResolveKodiChannelUid(r.channelId);
              if (kodiUid > 0) {
                  t.SetClientChannelUid(kodiUid);
              }
              t.SetState(r.enabled ? PVR_TIMER_STATE_SCHEDULED : PVR_TIMER_STATE_DISABLED);
              // Approximate next occurrence logic omitted for brevity, 
              // just showing it exists.
              t.SetStartTime(time(nullptr) + 86400); 
              results.Add(t);
          }
      }

      // 3. Scheduled Recordings (manual type 1 or EPG-based type 4)
      std::vector<dispatcharr::Recording> recs;
      if (m_dispatcharrClient->FetchRecordings(recs)) {
          int timerCount = 0;
          for (const auto& r : recs) {
              // Only show scheduled or in-progress recordings in timers list
              // Completed recordings go to GetRecordings, not here
              kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: GetTimers - recording id=%d, status='%s', title='%s', channel=%d",
                        r.id, r.status.c_str(), r.title.c_str(), r.channelId);
              if (r.status != "scheduled" && r.status != "recording") 
                  continue;
              timerCount++;
              
              kodi::addon::PVRTimer t;
              // Use the recording ID offset by 30000 to avoid collision
              t.SetClientIndex(static_cast<unsigned int>(30000 + r.id));
              t.SetTitle(r.title);
              const int kodiUid = r.kodiChannelUid != 0
                                      ? r.kodiChannelUid
                                      : ResolveKodiChannelUid(r.channelId);
              const unsigned int kodiEpgUid = r.kodiEpgUid != EPG_TAG_INVALID_UID
                                                  ? r.kodiEpgUid
                                                  : ResolveRecordingEpgUid(r, kodiUid);
              if (kodiEpgUid != EPG_TAG_INVALID_UID) {
                  t.SetTimerType(4);
                  t.SetEPGUid(kodiEpgUid);
              } else {
                  t.SetTimerType(1);
              }
              // Map Dispatcharr channel ID back to Kodi channel UID
              kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: GetTimers - recording id=%d mapped channel %d -> kodiUid %d",
                        r.id, r.channelId, kodiUid);
              if (kodiUid > 0) {
                  t.SetClientChannelUid(kodiUid);
              }
              t.SetStartTime(r.startTime);
              t.SetEndTime(r.endTime);
              // Set appropriate state based on status
              if (r.status == "recording") {
                  t.SetState(PVR_TIMER_STATE_RECORDING);
              } else {
                  t.SetState(PVR_TIMER_STATE_SCHEDULED);
              }
              results.Add(t);
          }
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: GetTimers - fetched %zu recordings, %d as timers", recs.size(), timerCount);
      }

      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: GetTimers complete");
      return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer) override
  {
      if (!m_dispatcharrClient) return PVR_ERROR_SERVER_ERROR;

      unsigned int typeId = timer.GetTimerType();
      int chanUid = timer.GetClientChannelUid();
      
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer called - type=%u, channel=%d, title='%s'",
                typeId, chanUid, timer.GetTitle().c_str());
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer - start=%ld, end=%ld",
                (long)timer.GetStartTime(), (long)timer.GetEndTime());
      
      // Look up Dispatcharr's TVG ID for the channel (not Xtream's epg_channel_id)
      std::string dispatchTvgId;
      std::string xtreamTvgId;
      int channelNumber = 0;
      
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         if (m_streams) {
             for(const auto& s : *m_streams) {
                 if (static_cast<unsigned int>(s.id) == chanUid) {
                     xtreamTvgId = s.epgChannelId;
                     channelNumber = s.number;  // Use the channel number from Xtream stream
                     break;
                 }
             }
         }
      }
      
      // Get the actual Dispatcharr tvg_id using the channel number (not the Kodi UID/stream_id)
      // Kodi UID is the Xtream stream_id, but Dispatcharr uses channel_number for mapping
      if (channelNumber > 0) {
          dispatchTvgId = m_dispatcharrClient->GetDispatchTvgId(channelNumber);
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: Mapped Kodi UID %u -> channel_number %d -> Dispatcharr tvg_id '%s'",
                    chanUid, channelNumber, dispatchTvgId.c_str());
      }

      if (typeId == 2 || typeId == 5) // Series
      {
          if (dispatchTvgId.empty()) {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Cannot add series rule, no Dispatcharr TVG ID found for Kodi channel %u (Xtream tvg_id='%s')", 
                        chanUid, xtreamTvgId.c_str());
              return PVR_ERROR_FAILED;
          }
          std::string seriesMode = (typeId == 5) ? "all" : "new";
          std::string title = timer.GetTitle(); 
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer (series) - calling Dispatcharr API POST /api/channels/series-rules/ with tvg_id='%s' (Xtream had '%s'), title='%s', mode='%s'",
              dispatchTvgId.c_str(), xtreamTvgId.c_str(), title.c_str(), seriesMode.c_str());
          // If title is empty?
          if (m_dispatcharrClient->AddSeriesRule(dispatchTvgId, title, seriesMode)) {
              kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer (series) - Dispatcharr API returned success");
              TriggerTimerUpdate();
              return PVR_ERROR_NO_ERROR;
          } else {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: AddTimer (series) - Dispatcharr API returned failure");
          }
      }
      else if (typeId == 3) // Recurring
      {
          // Map channel number to Dispatcharr channel ID
          if (channelNumber == 0) {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Cannot add recurring rule, no channel number found for Kodi UID %u", chanUid);
              return PVR_ERROR_FAILED;
          }
          int dispatchChannelId = m_dispatcharrClient->GetDispatchChannelId(channelNumber);
          if (dispatchChannelId < 0) {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Cannot add recurring rule, no Dispatcharr channel found for channel number %d", channelNumber);
              return PVR_ERROR_FAILED;
          }
          
          dispatcharr::RecurringRule r;
          r.channelId = dispatchChannelId;
          r.name = timer.GetTitle();
          
          // Map timer.GetStartTime() (time_t) to HH:MM:SS
          // Kodi passes absolute time for the FIRST occurrence.
          time_t start = timer.GetStartTime();
          time_t end = timer.GetEndTime();
          
          // Use localtime_r or copy the struct, as localtime uses a static buffer
          struct tm tmStart, tmEnd;
          localtime_r(&start, &tmStart);
          localtime_r(&end, &tmEnd);
          
          char buf[10];
          strftime(buf, sizeof(buf), "%H:%M:%S", &tmStart);
          r.startTime = buf;
          strftime(buf, sizeof(buf), "%H:%M:%S", &tmEnd);
          r.endTime = buf;
          
          r.daysOfWeek = {0,1,2,3,4,5,6}; // Default to daily if not specified? 
          // Kodi PVRTimer doesn't easily expose weekdays unless we parse Weekdays attribute?
          // For now, default to ALL days if creating generic recurring. 
          // Real implementation would look at `timer.GetWeekdays()`.
          
          // API requires dates now
          r.startDate = "2026-01-01"; // Dummy defaults as we don't present UI for date ranges in Kodi easily
          r.endDate = "2030-01-01";
          
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer (recurring) - calling Dispatcharr API POST /api/channels/recurring-rules/ with channel=%d, name='%s', time=%s-%s",
                    r.channelId, r.name.c_str(), r.startTime.c_str(), r.endTime.c_str());
          if (m_dispatcharrClient->AddRecurringRule(r)) {
              kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer (recurring) - Dispatcharr API returned success");
              TriggerTimerUpdate();
              return PVR_ERROR_NO_ERROR;
          } else {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: AddTimer (recurring) - Dispatcharr API returned failure");
          }
      }
      else // One-shot (Type 1 manual, Type 4 EPG-based, or default)
      {
          // Map channel number to Dispatcharr channel ID
          if (channelNumber == 0) {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Cannot schedule recording, no channel number found for Kodi UID %u", chanUid);
              return PVR_ERROR_FAILED;
          }
          int dispatchChannelId = m_dispatcharrClient->GetDispatchChannelId(channelNumber);
          if (dispatchChannelId < 0) {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: Cannot schedule recording, no Dispatcharr channel found for channel number %d", channelNumber);
              return PVR_ERROR_FAILED;
          }
          
          const char* typeStr = (typeId == 4) ? "EPG one-shot" : "manual one-shot";
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer (%s) - calling Dispatcharr API POST /api/channels/recordings/ with channel=%d, title='%s'",
                    typeStr, dispatchChannelId, timer.GetTitle().c_str());
          const unsigned int epgUid = (typeId == 4) ? timer.GetEPGUid() : EPG_TAG_INVALID_UID;
          const int kodiChannelUid = (typeId == 4) ? chanUid : 0;
          if (m_dispatcharrClient->ScheduleRecording(dispatchChannelId,
                                                     timer.GetStartTime(),
                                                     timer.GetEndTime(),
                                                     timer.GetTitle(),
                                                     epgUid,
                                                     kodiChannelUid)) {
              kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: AddTimer (one-shot) - Dispatcharr API returned success, calling TriggerTimerUpdate");
              TriggerTimerUpdate();
              TriggerRecordingUpdate();
              return PVR_ERROR_NO_ERROR;
          } else {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: AddTimer (one-shot) - Dispatcharr API returned failure");
          }
      }

      return PVR_ERROR_FAILED;
  }

  PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool force) override
  {
      if (!m_dispatcharrClient) return PVR_ERROR_SERVER_ERROR;

      unsigned int clientIndex = timer.GetClientIndex();
      
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: DeleteTimer called - clientIndex=%u, title='%s', force=%d",
                clientIndex, timer.GetTitle().c_str(), force);
      
      // Determine type based on ID range:
      // 10000-19999 = series rules
      // 20000-29999 = recurring rules  
      // 30000+ = scheduled recordings
      
      if (clientIndex >= 30000) {
          // Scheduled recording - ID is clientIndex - 30000
          int recId = static_cast<int>(clientIndex - 30000);
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: DeleteTimer (one-shot) - calling Dispatcharr API DELETE /api/channels/recordings/%d/", recId);
          if (m_dispatcharrClient->DeleteRecording(recId)) {
              kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: DeleteTimer (one-shot) - Dispatcharr API returned success");
              TriggerTimerUpdate();
              return PVR_ERROR_NO_ERROR;
          } else {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: DeleteTimer (one-shot) - Dispatcharr API returned failure");
          }
      } else if (clientIndex >= 20000) {
          // Recurring rule - ID is clientIndex - 20000
          int ruleId = static_cast<int>(clientIndex - 20000);
          kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: DeleteTimer (recurring) - calling Dispatcharr API DELETE /api/channels/recurring-rules/%d/", ruleId);
          if (m_dispatcharrClient->DeleteRecurringRule(ruleId)) {
              kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: DeleteTimer (recurring) - Dispatcharr API returned success");
              TriggerTimerUpdate();
              return PVR_ERROR_NO_ERROR;
          } else {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: DeleteTimer (recurring) - Dispatcharr API returned failure");
          }
      } else if (clientIndex >= 10000) {
          // Series rule - need to look up by index since we use a counter
          // This is tricky - we'd need to store a mapping. For now, fetch and match by position.
          std::vector<dispatcharr::SeriesRule> series;
          if (m_dispatcharrClient->FetchSeriesRules(series)) {
              size_t idx = clientIndex - 10000;
              if (idx < series.size()) {
                  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: DeleteTimer (series) - calling Dispatcharr API DELETE /api/channels/series-rules/%s/",
                            series[idx].tvgId.c_str());
                  if (m_dispatcharrClient->DeleteSeriesRule(series[idx].tvgId)) {
                      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr: DeleteTimer (series) - Dispatcharr API returned success");
                      TriggerTimerUpdate();
                      return PVR_ERROR_NO_ERROR;
                  } else {
                      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: DeleteTimer (series) - Dispatcharr API returned failure");
                  }
              } else {
                  kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: DeleteTimer (series) - index %zu out of range (size=%zu)", idx, series.size());
              }
          } else {
              kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: DeleteTimer (series) - failed to fetch series rules list");
          }
      }

      return PVR_ERROR_FAILED;
  }

  PVR_ERROR GetChannelGroupsAmount(int& amount) override
  {
    EnsureLoaded();

    std::shared_ptr<const std::vector<std::string>> groupNames;
    bool groupsReady = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      groupNames = m_groupNamesOrdered;
      groupsReady = m_groupsReady;
    }
    // Only return groups if they're ready; prevents Kodi from trying to access group members
    // before they've been populated, which can cause UI blocking with large channel counts.
    amount = (groupsReady && groupNames) ? static_cast<int>(groupNames->size()) : 0;
    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override
  {
    if (radio)
      return PVR_ERROR_NO_ERROR;

    EnsureLoaded();

    const auto t0 = std::chrono::steady_clock::now();

    std::shared_ptr<const std::vector<std::string>> groupNames;
    bool groupsReady = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      groupNames = m_groupNamesOrdered;
      groupsReady = m_groupsReady;
    }
    // Only return groups if they're ready; prevents Kodi from trying to access group members
    // before they've been populated, which can cause UI blocking with large channel counts.
    if (!groupsReady || !groupNames)
      return PVR_ERROR_NO_ERROR;

    unsigned int pos = 1;
    for (const auto& name : *groupNames)
    {
      kodi::addon::PVRChannelGroup group;
      group.SetIsRadio(false);
      group.SetGroupName(name);
      group.SetPosition(pos++);
      results.Add(group);
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (ms > 500)
      kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: GetChannelGroups returned %zu in %lld ms", groupNames->size(), static_cast<long long>(ms));

    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                   kodi::addon::PVRChannelGroupMembersResultSet& results) override
  {
    EnsureLoaded();

    const auto t0 = std::chrono::steady_clock::now();

    std::shared_ptr<const GroupMembersMap> members;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      members = m_groupMembers;
    }
    if (!members)
      return PVR_ERROR_NO_ERROR;

    const std::string groupName = group.GetGroupName();
    auto it = members->find(groupName);
    if (it == members->end())
      return PVR_ERROR_NO_ERROR;

    for (const auto& member : it->second)
    {
      kodi::addon::PVRChannelGroupMember kodiMember;
      kodiMember.SetGroupName(groupName);
      kodiMember.SetChannelUniqueId(member.channelUid);
      kodiMember.SetChannelNumber(member.channelNumber);
      kodiMember.SetSubChannelNumber(member.subChannelNumber);
      results.Add(kodiMember);
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (ms > 500)
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr: GetChannelGroupMembers('%s') returned %zu in %lld ms",
                groupName.c_str(), it->second.size(), static_cast<long long>(ms));

    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel& channel,
                                      std::vector<kodi::addon::PVRStreamProperty>& properties) override
  {
    EnsureLoaded();

    std::shared_ptr<const UidToStreamMap> uidToStream;
    std::shared_ptr<const std::vector<xtream::LiveStream>> streams;
    xtream::Settings settings;
    std::string streamFormat;
    PendingCatchup pendingCatchup;
    bool hasPendingCatchup = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const unsigned int channelUid = channel.GetUniqueId();
      const auto now = std::chrono::steady_clock::now();
      const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

      // A pending native catchup open (stashed by GetEPGTagStreamProperties)
      // selects Kodi's raw OpenLiveStream/ReadLiveStream/SeekLiveStream path:
      // return empty properties (no STREAMURL) and leave the entry for
      // OpenLiveStream to consume.
      const auto nativeIt = m_pendingNativeCatchupOpenByChannel.find(channelUid);
      if (nativeIt != m_pendingNativeCatchupOpenByChannel.end() && nativeIt->second.expiresAtMs >= nowMs)
        return PVR_ERROR_NO_ERROR;

      uidToStream = m_uidToStreamId;
      streams = m_streams;
      settings = m_xtreamSettings;
      streamFormat = m_streamFormat;
      // Check if there's a pending catchup URL for this channel
      auto pendingIt = m_pendingCatchupByChannel.find(channelUid);
      if (pendingIt != m_pendingCatchupByChannel.end())
      {
        if (pendingIt->second.expiresAtMs >= nowMs && !pendingIt->second.url.empty())
        {
          pendingCatchup = pendingIt->second;
          hasPendingCatchup = true;
          // Store as active catchup for GetStreamTimes/CanSeekStream/IsRealTimeStream
          m_activeCatchup = pendingIt->second;
          m_activeCatchupChannelUid = channelUid;
        }
        // Clear the pending state after consuming (or if expired)
        m_pendingCatchupByChannel.erase(pendingIt);
      }
      else
      {
        // Starting a non-catchup (live) stream - clear any active catchup state
        m_activeCatchup = PendingCatchup{};
        m_activeCatchupChannelUid = 0;
      }
    }
    if (!uidToStream)
      return PVR_ERROR_UNKNOWN;

    const std::string streamMimeType = (ToLower(streamFormat) == "hls")
                                        ? "application/vnd.apple.mpegurl"
                                        : "video/mp2t";

    // If we have a pending catchup URL from GetEPGTagStreamProperties, use it
    if (hasPendingCatchup)
    {
      kodi::Log(ADDON_LOG_INFO, "GetChannelStreamProperties: using CATCHUP URL = %s", pendingCatchup.url.c_str());
      
      if (pendingCatchup.useFFmpegDirect && !pendingCatchup.templateUrl.empty())
      {
        // Use inputstream.ffmpegdirect with catchup mode for seeking support
        properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
        properties.emplace_back("inputstream-player", "videodefaultplayer");
        properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "catchup");
        properties.emplace_back("inputstream.ffmpegdirect.default_url", pendingCatchup.url);
        properties.emplace_back("inputstream.ffmpegdirect.catchup_url_format_string", pendingCatchup.templateUrl);
        properties.emplace_back("inputstream.ffmpegdirect.catchup_buffer_start_time", std::to_string(pendingCatchup.adjustedStart));
        properties.emplace_back("inputstream.ffmpegdirect.catchup_buffer_end_time", std::to_string(pendingCatchup.programEnd));
        properties.emplace_back("inputstream.ffmpegdirect.catchup_terminates", "true");
        properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "false");
        properties.emplace_back("inputstream.ffmpegdirect.timezone_shift", "0");
        kodi::Log(ADDON_LOG_INFO, "GetChannelStreamProperties: using inputstream.ffmpegdirect catchup mode");
      }
      
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, pendingCatchup.url);
      properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");
      properties.emplace_back(PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE, "false");
      properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, streamMimeType);
      return PVR_ERROR_NO_ERROR;
    }
    kodi::Log(ADDON_LOG_INFO, "GetChannelStreamProperties: no pending catchup URL for channel %u, using LIVE", channel.GetUniqueId());

    const unsigned int uid = channel.GetUniqueId();
    auto it = uidToStream->find(uid);
    if (it == uidToStream->end())
      return PVR_ERROR_UNKNOWN;

    if (settings.apiMode == "native")
    {
      std::string uuid;
      if (streams)
      {
        for (const auto& s : *streams)
        {
          if (static_cast<unsigned int>(s.id) == uid)
          {
            uuid = s.uuid;
            break;
          }
        }
      }
      if (uuid.empty())
        return PVR_ERROR_UNKNOWN;

      std::shared_ptr<dispatcharr::Client> dispatchClient;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        dispatchClient = m_dispatcharrClient;
      }
      if (!dispatchClient)
        return PVR_ERROR_SERVER_ERROR;

      const std::string nativeUrl = dispatchClient->BuildLiveStreamUrl(uuid);
      if (nativeUrl.empty())
        return PVR_ERROR_UNKNOWN;

      kodi::Log(ADDON_LOG_DEBUG, "GetChannelStreamProperties: using native LIVE URL = %s", nativeUrl.c_str());
      properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG);
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, nativeUrl);
      properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
      // Dispatcharr's stream proxy always serves MPEG-TS, regardless of the
      // Xtream-path stream_format setting.
      properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "video/mp2t");
      return PVR_ERROR_NO_ERROR;
    }

    const int streamId = it->second;
    const std::string url = xtream::BuildLiveStreamUrl(settings, streamId, streamFormat);
    if (url.empty())
      return PVR_ERROR_UNKNOWN;

    kodi::Log(ADDON_LOG_DEBUG, "GetChannelStreamProperties: using LIVE URL = %s", url.c_str());

    // For live streams, use Kodi's built-in ffmpeg inputstream.
    // We cannot use inputstream.ffmpegdirect for channel switching because it has a bug:
    // When m_reopen=true (which happens during transport stream reopens), ffmpegdirect
    // skips avformat_find_stream_info() regardless of is_realtime_stream setting.
    // This causes GetStreamIds() to return empty, breaking channel switching.
    // 
    // Kodi's built-in inputstream (PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG) handles
    // TS streams properly and supports channel switching. This is also what pvr.iptvsimple
    // uses as a fallback for HLS/TS streams.
    properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG);
    kodi::Log(ADDON_LOG_INFO, "GetChannelStreamProperties: using Kodi's built-in ffmpeg inputstream");
    
    properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, url);
    properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
    properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, streamMimeType);
    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results) override
  {
    EnsureLoaded();
    RequestEpgRefreshIfStale();

    std::shared_ptr<const std::vector<xtream::ChannelEpg>> epgData;
    std::shared_ptr<const UidToStreamMap> uidToStream;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      epgData = m_epgData;
      uidToStream = m_uidToStreamId;
    }

    if (!epgData || !uidToStream)
      return PVR_ERROR_NO_ERROR;

    // Find the stream ID for this channel UID
    auto uidIt = uidToStream->find(static_cast<unsigned int>(channelUid));
    if (uidIt == uidToStream->end())
      return PVR_ERROR_NO_ERROR;

    const int streamId = uidIt->second;
    const std::string streamIdStr = std::to_string(streamId);

    // Find EPG for this channel (match by stream ID as channel ID)
    const xtream::ChannelEpg* channelEpg = nullptr;
    for (const auto& epg : *epgData)
    {
      if (epg.id == streamIdStr)
      {
        channelEpg = &epg;
        break;
      }
    }

    if (!channelEpg || channelEpg->entries.empty())
      return PVR_ERROR_NO_ERROR;

    // Add EPG entries within the requested time window
    for (const auto& kv : channelEpg->entries)
    {
      const auto& entry = kv.second;
      
      // Skip entries outside the requested window
      if (entry.endTime < start || entry.startTime > end)
        continue;

      kodi::addon::PVREPGTag tag;
      tag.SetUniqueBroadcastId(static_cast<unsigned int>(entry.startTime));
      tag.SetUniqueChannelId(static_cast<unsigned int>(channelUid));
      tag.SetTitle(entry.title);
      tag.SetPlot(entry.description);
      tag.SetStartTime(entry.startTime);
      tag.SetEndTime(entry.endTime);
      
      if (!entry.episodeName.empty())
        tag.SetEpisodeName(entry.episodeName);
      if (!entry.iconPath.empty())
        tag.SetIconPath(entry.iconPath);
      if (entry.genreType > 0)
      {
        tag.SetGenreType(entry.genreType);
        tag.SetGenreSubType(entry.genreSubType);
      }
      else if (!entry.genreString.empty())
      {
        tag.SetGenreType(EPG_GENRE_USE_STRING);
        tag.SetGenreDescription(entry.genreString);
      }
      if (entry.year > 0)
        tag.SetYear(entry.year);
      if (entry.starRating > 0)
        tag.SetStarRating(entry.starRating);
      if (entry.seasonNumber >= 0)
        tag.SetSeriesNumber(entry.seasonNumber);
      if (entry.episodeNumber >= 0)
        tag.SetEpisodeNumber(entry.episodeNumber);

      results.Add(tag);
    }

    return PVR_ERROR_NO_ERROR;
  }

  PVR_ERROR IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& isPlayable) override
  {
    isPlayable = false;
    EnsureLoaded();
    
    kodi::Log(ADDON_LOG_DEBUG, "IsEPGTagPlayable: channel=%u, start=%ld, end=%ld", 
              tag.GetUniqueChannelId(), tag.GetStartTime(), tag.GetEndTime());

    std::shared_ptr<const std::vector<xtream::LiveStream>> streams;
    xtream::Settings settings;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      streams = m_streams;
      settings = m_xtreamSettings;
    }

    if (!streams)
      return PVR_ERROR_NO_ERROR;

    const unsigned int channelUid = tag.GetUniqueChannelId();
    const time_t startTime = tag.GetStartTime();
    const time_t endTime = tag.GetEndTime();
    const time_t now = std::time(nullptr);

    // Check if program is in the past or currently airing
    const bool isPast = endTime < now;
    const bool isOngoing = startTime <= now && now < endTime;
    
    // Future programs cannot be played
    if (startTime > now)
      return PVR_ERROR_NO_ERROR;
    
    // Only allow ongoing programs if play-from-start is enabled
    if (isOngoing && !settings.enablePlayFromStart)
      return PVR_ERROR_NO_ERROR;

    // Find the stream for this channel
    for (const auto& stream : *streams)
    {
      if (static_cast<unsigned int>(stream.id) == channelUid)
      {
        kodi::Log(ADDON_LOG_DEBUG, "IsEPGTagPlayable: found stream %d, tvArchive=%d, duration=%d",
                  stream.id, stream.tvArchive, stream.tvArchiveDuration);
        
        // Check if stream has catchup/archive support
        if (stream.tvArchive && stream.tvArchiveDuration > 0)
        {
          // Check if the program is within the archive window
          const time_t archiveCutoff = now - (stream.tvArchiveDuration * 24 * 3600); // duration is in days
          if (endTime >= archiveCutoff)
          {
            isPlayable = true;
            kodi::Log(ADDON_LOG_DEBUG, "IsEPGTagPlayable: PLAYABLE!");
          }
        }
        break;
      }
    }

    return PVR_ERROR_NO_ERROR;
  }

  bool CanSeekStream() override
  {
    if (m_activeRecordedStream && m_activeRecordedStream->IsOpen())
      return true;
    if (m_activeNativeLiveCatchupStream && m_activeNativeLiveCatchupStream->IsOpen())
      return true;
    // Catchup streams support seeking via HTTP range requests
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeCatchupChannelUid != 0 && m_activeCatchup.programStart > 0;
  }

  bool CanPauseStream() override
  {
    return (m_activeRecordedStream && m_activeRecordedStream->IsOpen()) ||
           (m_activeNativeLiveCatchupStream && m_activeNativeLiveCatchupStream->IsOpen());
  }

  void PauseStream(bool paused) override
  {
    (void)paused;
  }

  bool IsRealTimeStream() override
  {
    if (m_activeRecordedStream && m_activeRecordedStream->IsOpen())
      return false;
    if (m_activeNativeLiveCatchupStream && m_activeNativeLiveCatchupStream->IsOpen())
      return false;
    // When playing catchup, this is NOT a realtime stream
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeCatchupChannelUid == 0;
  }

  PVR_ERROR GetStreamTimes(kodi::addon::PVRStreamTimes& times) override
  {
    if (m_activeRecordedStream && m_activeRecordedStream->IsOpen())
    {
      const int64_t duration = m_activeRecordedStream->DurationMicroseconds();
      // A finished recording's duration isn't known ahead of demuxing (only
      // its byte length is); let Kodi derive it from the stream itself.
      if (duration <= 0)
        return PVR_ERROR_NOT_IMPLEMENTED;
      times.SetStartTime(0);
      times.SetPTSStart(0);
      times.SetPTSBegin(0);
      times.SetPTSEnd(duration);
      return PVR_ERROR_NO_ERROR;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Check if we have an active catchup stream
    if (m_activeCatchupChannelUid != 0 && 
        m_activeCatchup.programStart > 0 && 
        m_activeCatchup.programEnd > m_activeCatchup.programStart)
    {
      // Set timing information for seeking
      times.SetStartTime(m_activeCatchup.programStart);
      times.SetPTSStart(0); // Start at beginning
      times.SetPTSBegin(0); // Can seek to beginning
      
      // Duration in microseconds
      const int64_t durationSec = m_activeCatchup.programEnd - m_activeCatchup.programStart;
      times.SetPTSEnd(durationSec * 1000000LL); // Convert to microseconds
      
      kodi::Log(ADDON_LOG_DEBUG, "GetStreamTimes: start=%ld, end=%ld, duration=%lld sec",
                m_activeCatchup.programStart, m_activeCatchup.programEnd, durationSec);
      return PVR_ERROR_NO_ERROR;
    }
    
    return PVR_ERROR_NOT_IMPLEMENTED;
  }

  // Only ever selected for native catchup - see the "pending native catchup
  // open" checks in GetChannelStreamProperties/GetEPGTagStreamProperties
  // above. Regular live channel playback (any api_mode) always returns a
  // STREAMURL and never reaches OpenLiveStream/ReadLiveStream/SeekLiveStream/
  // LengthLiveStream at all.
  bool OpenLiveStream(const kodi::addon::PVRChannel& channel) override
  {
    PendingNativeCatchupOpen pending;
    bool hasPending = false;
    std::shared_ptr<dispatcharr::Client> dispatchClient;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const unsigned int channelUid = channel.GetUniqueId();
      const auto now = std::chrono::steady_clock::now();
      const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      auto it = m_pendingNativeCatchupOpenByChannel.find(channelUid);
      if (it != m_pendingNativeCatchupOpenByChannel.end())
      {
        if (it->second.expiresAtMs >= nowMs)
        {
          pending = it->second;
          hasPending = true;
        }
        m_pendingNativeCatchupOpenByChannel.erase(it);
      }
      dispatchClient = m_dispatcharrClient;
    }

    if (!hasPending || !dispatchClient)
    {
      kodi::Log(ADDON_LOG_WARNING, "OpenLiveStream: called with no pending native catchup open for channel %u",
                channel.GetUniqueId());
      return false;
    }

    kodi::Log(ADDON_LOG_INFO, "OpenLiveStream: opening native catchup uuid=%s start=%ld end=%ld",
              pending.channelUuid.c_str(), pending.programStart, pending.programEnd);

    auto stream = std::make_unique<dispatcharr::recording::NativeCatchupLiveStream>(*dispatchClient);
    if (!stream->Open(pending.channelUuid, pending.programStart, pending.programEnd))
    {
      kodi::Log(ADDON_LOG_ERROR, "OpenLiveStream: NativeCatchupLiveStream::Open failed");
      return false;
    }

    m_activeNativeLiveCatchupStream = std::move(stream);
    return true;
  }

  void CloseLiveStream() override
  {
    if (m_activeNativeLiveCatchupStream)
    {
      m_activeNativeLiveCatchupStream->Close();
      m_activeNativeLiveCatchupStream.reset();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    // Clear all active stream state that may differ between channels
    m_activeCatchup = PendingCatchup{};
    m_activeCatchupChannelUid = 0;
    kodi::Log(ADDON_LOG_DEBUG, "CloseLiveStream: cleared active stream state");
  }

  int ReadLiveStream(unsigned char* buffer, unsigned int size) override
  {
    return m_activeNativeLiveCatchupStream ? m_activeNativeLiveCatchupStream->Read(buffer, size) : -1;
  }

  int64_t SeekLiveStream(int64_t position, int whence) override
  {
    return m_activeNativeLiveCatchupStream ? m_activeNativeLiveCatchupStream->Seek(position, whence) : -1;
  }

  int64_t LengthLiveStream() override
  {
    return m_activeNativeLiveCatchupStream ? m_activeNativeLiveCatchupStream->Length() : -1;
  }

  PVR_ERROR GetEPGTagStreamProperties(const kodi::addon::PVREPGTag& tag,
                                     std::vector<kodi::addon::PVRStreamProperty>& properties) override
  {
    EnsureLoaded();
    
    kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties CALLED: channel=%u, start=%ld, end=%ld",
              tag.GetUniqueChannelId(), tag.GetStartTime(), tag.GetEndTime());

    std::shared_ptr<const std::vector<xtream::LiveStream>> streams;
    xtream::Settings settings;
    std::string streamFormat;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      streams = m_streams;
      settings = m_xtreamSettings;
      streamFormat = m_streamFormat;
    }

    if (!streams)
      return PVR_ERROR_UNKNOWN;

    const unsigned int channelUid = tag.GetUniqueChannelId();
    const time_t startTime = tag.GetStartTime();
    const time_t endTime = tag.GetEndTime();
    kodi::Log(ADDON_LOG_INFO,
          "GetEPGTagStreamProperties: catchup offset hours=%d, start=%ld, end=%ld",
          settings.catchupStartOffsetHours, startTime, endTime);

    // Find the stream for this channel
    for (const auto& stream : *streams)
    {
      if (static_cast<unsigned int>(stream.id) == channelUid)
      {
        if (!stream.tvArchive)
          return PVR_ERROR_UNKNOWN;

        // Prevent attempting catchup for future programmes
        const time_t nowTs = std::time(nullptr);
        if (startTime > nowTs)
        {
          kodi::Log(ADDON_LOG_WARNING,
                    "GetEPGTagStreamProperties: programme start is in the future; refusing catchup");
          return PVR_ERROR_UNKNOWN;
        }

        // Build catchup URL (use 'now' as end for ongoing programmes)
        const bool isOngoing = (endTime > nowTs);
        const time_t effectiveEnd = isOngoing ? nowTs : endTime;

        if (settings.apiMode == "native")
        {
          if (stream.uuid.empty())
            return PVR_ERROR_UNKNOWN;

          // Deliberately return no STREAMURL and no ffmpegdirect properties.
          // Confirmed directly against the server (not just observed in
          // Kodi) that neither a plain Range-seek on one session nor
          // ffmpegdirect's reopen-with-a-new-`start` on one session actually
          // moves playback - the server always keeps serving the original
          // bind position. The only thing that reliably re-anchors is a
          // genuinely new session per seek, which requires an addon-side API
          // call ffmpegdirect's client-side URL substitution can't make.
          // Returning empty properties (bar EPGPLAYBACKASLIVE) here selects
          // Kodi's raw OpenLiveStream/ReadLiveStream/SeekLiveStream
          // byte-callback path instead (see NativeCatchupLiveStream), the
          // same way GetRecordingStreamProperties returning no STREAMURL
          // selects OpenRecordedStream below.
          //
          // EPGPLAYBACKASLIVE=true is required here: without it Kodi tries
          // to open the raw pvr://guide/... EPG-tag URI directly instead of
          // falling through to the channel's stream properties/OpenLiveStream
          // (confirmed - omitting it produced "CVideoPlayer::OpenInputStream
          // - error opening [pvr://guide/...]" instead of ever reaching
          // OpenLiveStream).
          const auto nowSteady = std::chrono::steady_clock::now();
          const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    nowSteady.time_since_epoch()).count();
          {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingNativeCatchupOpenByChannel[channelUid] =
                PendingNativeCatchupOpen{stream.uuid, startTime, effectiveEnd, nowMs + 30000};
          }
          (void)isOngoing;
          properties.emplace_back(PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE, "true");
          return PVR_ERROR_NO_ERROR;
        }

        const std::string url = xtream::BuildCatchupUrl(settings, stream.id, startTime, effectiveEnd, streamFormat);
        kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: catchup URL = %s, isOngoing=%d", url.c_str(), isOngoing);
        
        if (url.empty())
        {
          kodi::Log(ADDON_LOG_ERROR, "GetEPGTagStreamProperties: catchup URL is EMPTY, returning ERROR");
          return PVR_ERROR_UNKNOWN;
        }

        // Store the catchup URL for GetChannelStreamProperties to use
        // Kodi will call GetChannelStreamProperties after this, and we need to provide the catchup URL there
        const auto now = std::chrono::steady_clock::now();
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        
        const std::string streamMimeType = (ToLower(streamFormat) == "hls")
                    ? "application/vnd.apple.mpegurl"
                    : "video/mp2t";
        
        // Optionally use inputstream.ffmpegdirect for better seeking support
        if (settings.useFFmpegDirect)
        {
          // Apply the same catchup offset that BuildCatchupUrl applies
          // (clamped to 0 if negative, converted from hours to seconds)
          int offsetHours = settings.catchupStartOffsetHours;
          if (offsetHours < 0)
            offsetHours = 0;
          const time_t offsetSeconds = offsetHours * 3600;
          const time_t adjustedStartTime = startTime + offsetSeconds;
          
          // Calculate programme duration in minutes from the adjusted start
          const int programDurationMinutes = static_cast<int>((effectiveEnd - adjustedStartTime) / 60);
          
          // Build a URL template with ffmpegdirect placeholders for seeking
          const std::string templateUrl = xtream::BuildCatchupUrlTemplate(
              settings, stream.id, programDurationMinutes, streamFormat);
          
          if (templateUrl.empty())
          {
            kodi::Log(ADDON_LOG_ERROR, "GetEPGTagStreamProperties: catchup URL template is EMPTY");
            return PVR_ERROR_UNKNOWN;
          }
          
          kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: catchup URL template = %s", templateUrl.c_str());
          
          // Store all the info needed for GetChannelStreamProperties to set up ffmpegdirect
          {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingCatchupByChannel[channelUid] = PendingCatchup{
              url, templateUrl, nowMs + 30000, startTime, effectiveEnd, adjustedStartTime, true
            };
          }
          
          properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, "inputstream.ffmpegdirect");
          // Force VideoPlayer to be used (required for proper PVR channel handling)
          properties.emplace_back("inputstream-player", "videodefaultplayer");
          // Use catchup mode with URL template - ffmpegdirect substitutes placeholders when seeking
          properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "catchup");
          // The default URL is used for initial playback (concrete URL with actual start time)
          properties.emplace_back("inputstream.ffmpegdirect.default_url", url);
          // The catchup URL format string contains placeholders for seeking
          properties.emplace_back("inputstream.ffmpegdirect.catchup_url_format_string", templateUrl);
          // Buffer boundaries in epoch seconds (adjusted for catchup offset to match the concrete URL)
          properties.emplace_back("inputstream.ffmpegdirect.catchup_buffer_start_time", std::to_string(adjustedStartTime));
          properties.emplace_back("inputstream.ffmpegdirect.catchup_buffer_end_time", std::to_string(effectiveEnd));
          
          // For ongoing programs: fixed buffer but don't terminate (continues past buffer end)
          // For finished programs: fixed buffer, terminate at end
          // Keep is_realtime_stream=false for both to prevent seeking beyond buffer_end_time
          if (isOngoing)
          {
            properties.emplace_back("inputstream.ffmpegdirect.catchup_terminates", "false");
            properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "false");
            kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: ongoing program - catchup continues past buffer end");
          }
          else
          {
            properties.emplace_back("inputstream.ffmpegdirect.catchup_terminates", "true");
            properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "false");
          }
          
          // Calculate local timezone offset from UTC
          // ffmpegdirect formula: FormatDateTime(offset - m_timezoneShift, ...) then SafeLocaltime()
          // - offset is the UTC epoch we want
          // - SafeLocaltime() adds local timezone offset
          // - So we pass POSITIVE local offset to subtract it before localtime adds it back
          // Example for UTC+1 (offset 3600):
          //   seekTime 23:33 UTC (epoch X) - 3600 = epoch X-3600 (22:33 UTC)
          //   SafeLocaltime adds +1 hour = 23:33 local (which displays as 23:33) ✓
          time_t now = std::time(nullptr);
          std::tm utcTm = {};
          std::tm localTm = {};
#ifdef _WIN32
          gmtime_s(&utcTm, &now);
          localtime_s(&localTm, &now);
#else
          gmtime_r(&now, &utcTm);
          localtime_r(&now, &localTm);
#endif
          // Calculate offset: local - UTC (positive for UTC+ timezones)
          time_t utcTime = timegm(&utcTm);
          time_t localAsUtc = timegm(&localTm);
          int timezoneOffsetSecs = static_cast<int>(localAsUtc - utcTime);
          // Pass POSITIVE offset - ffmpegdirect subtracts this before calling localtime
          properties.emplace_back("inputstream.ffmpegdirect.timezone_shift", std::to_string(timezoneOffsetSecs));
          kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: timezone_shift=%d (local offset from UTC)", timezoneOffsetSecs);
          
          // Use the concrete URL for initial stream open
          properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, url);
          
          kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: using inputstream.ffmpegdirect catchup mode with URL template");
        }
        else
        {
          // Store basic catchup info without ffmpegdirect
          {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingCatchupByChannel[channelUid] = PendingCatchup{
              url, "", nowMs + 30000, startTime, effectiveEnd, startTime, false
            };
          }
          properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, url);
        }
        kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: stored catchup URL for channel %u", channelUid);
        kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: added STREAMURL property");
        properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");
        properties.emplace_back(PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE, "false");
        properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, streamMimeType);
        kodi::Log(ADDON_LOG_INFO, "GetEPGTagStreamProperties: returning SUCCESS with %d properties", (int)properties.size());
        return PVR_ERROR_NO_ERROR;
      }
    }

    return PVR_ERROR_UNKNOWN;
  }

private:
  unsigned int ResolveRecordingEpgUid(const dispatcharr::Recording& recording,
                                      int kodiChannelUid)
  {
    std::shared_ptr<const std::vector<xtream::ChannelEpg>> epgData;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      epgData = m_epgData;
    }
    if (!epgData)
      return EPG_TAG_INVALID_UID;

    const unsigned int uid = dispatcharr::recording::MatchRecordingToEpg(
        *epgData, kodiChannelUid, recording.startTime, recording.endTime, recording.title);
    if (uid != EPG_TAG_INVALID_UID)
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr: Matched external recording %d to Kodi EPG event %u on channel %d",
                recording.id, uid, kodiChannelUid);
    return uid;
  }

  int ResolveKodiChannelUid(int dispatcharrChannelId)
  {
    // The Dispatcharr client maps its internal ID back to a channel number,
    // not to Kodi's client channel UID. Kodi's UID is the Xtream stream ID.
    const int channelNumber = m_dispatcharrClient->GetKodiChannelUid(dispatcharrChannelId);
    if (channelNumber <= 0)
      return -1;

    std::shared_ptr<const std::vector<xtream::LiveStream>> streams;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      streams = m_streams;
    }

    if (streams)
    {
      for (const auto& stream : *streams)
      {
        if (stream.number == channelNumber)
          return stream.id;
      }
    }

    kodi::Log(ADDON_LOG_WARNING,
              "pvr.dispatcharr: No Kodi channel UID found for Dispatcharr channel %d (number %d)",
              dispatcharrChannelId, channelNumber);
    return -1;
  }

  struct GroupMember
  {
    unsigned int channelUid = 0;
    unsigned int channelNumber = 0;
    unsigned int subChannelNumber = 0;
  };

  struct CacheChannel
  {
    unsigned int uid = 0;
    int categoryId = 0;
    unsigned int channelNumber = 0;
    std::string name;
  };

  std::string CachePath() const
  {
    return TranslateSpecial("special://profile/addon_data/pvr.dispatcharr/channels.cache");
  }

  bool TryLoadCacheForSignature(const std::string& signature)
  {
    const std::string path = CachePath();
    if (path.empty())
      return false;

    std::string blob;
    if (!ReadFileToString(path, blob))
      return false;

    size_t off = 0;
    uint32_t magic = 0;
    if (!ReadU32(blob, off, magic) || magic != kCacheMagic)
      return false;

    uint32_t sigLen = 0;
    if (!ReadU32(blob, off, sigLen) || off + sigLen > blob.size())
      return false;
    const std::string sigOnDisk = blob.substr(off, sigLen);
    off += sigLen;
    if (sigOnDisk != signature)
      return false;

    uint64_t ts = 0;
    if (!ReadU64(blob, off, ts))
      return false;

    uint32_t catCount = 0;
    if (!ReadU32(blob, off, catCount))
      return false;

    std::unordered_map<int, std::string> categoryIdToName;
    categoryIdToName.reserve(static_cast<size_t>(catCount));
    for (uint32_t i = 0; i < catCount; ++i)
    {
      int32_t id = 0;
      uint32_t nameLen = 0;
      if (!ReadI32(blob, off, id) || !ReadU32(blob, off, nameLen) || off + nameLen > blob.size())
        return false;
      std::string name = blob.substr(off, nameLen);
      off += nameLen;
      if (id > 0 && !name.empty())
        categoryIdToName.emplace(static_cast<int>(id), std::move(name));
    }

    uint32_t chCount = 0;
    if (!ReadU32(blob, off, chCount))
      return false;

    std::vector<kodi::addon::PVRChannel> channels;
    channels.reserve(static_cast<size_t>(chCount));
    std::unordered_map<unsigned int, int> uidToStreamId;
    uidToStreamId.reserve(static_cast<size_t>(chCount));
    std::vector<int> channelCategoryIds;
    channelCategoryIds.reserve(static_cast<size_t>(chCount));

    for (uint32_t i = 0; i < chCount; ++i)
    {
      uint32_t uid = 0;
      int32_t catId = 0;
      uint32_t chNum = 0;
      uint32_t nameLen = 0;
      if (!ReadU32(blob, off, uid) || !ReadI32(blob, off, catId) || !ReadU32(blob, off, chNum) ||
          !ReadU32(blob, off, nameLen) || off + nameLen > blob.size())
        return false;
      std::string name = blob.substr(off, nameLen);
      off += nameLen;
      if (uid == 0 || name.empty())
        continue;

      kodi::addon::PVRChannel ch;
      ch.SetUniqueId(uid);
      ch.SetIsRadio(false);
      ch.SetChannelName(name);
      ch.SetChannelNumber(static_cast<int>(chNum));
      channels.push_back(std::move(ch));
      uidToStreamId.emplace(uid, static_cast<int>(uid));
      channelCategoryIds.push_back(static_cast<int>(catId));
    }

    std::unordered_map<std::string, std::vector<GroupMember>> groupMembers;
    for (size_t i = 0; i < channels.size(); ++i)
    {
      const unsigned int uid = channels[i].GetUniqueId();
      const unsigned int chNum = static_cast<unsigned int>(channels[i].GetChannelNumber());
      const int catId = channelCategoryIds[i];
      auto catIt = categoryIdToName.find(catId);
      if (catIt == categoryIdToName.end())
        continue;
      GroupMember gm;
      gm.channelUid = uid;
      gm.channelNumber = chNum;
      gm.subChannelNumber = 0;
      groupMembers[catIt->second].push_back(gm);
    }

    std::vector<std::pair<int, std::string>> cats;
    cats.reserve(categoryIdToName.size());
    for (const auto& kv : categoryIdToName)
      cats.emplace_back(kv.first, kv.second);
    std::sort(cats.begin(), cats.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::string> groupNamesOrdered;
    groupNamesOrdered.reserve(cats.size());
    for (const auto& kv : cats)
    {
      const auto memIt = groupMembers.find(kv.second);
      if (memIt == groupMembers.end() || memIt->second.empty())
        continue;
      groupNamesOrdered.push_back(kv.second);
    }

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      // Only seed from cache if we don't already have data.
      if (m_channels && !m_channels->empty())
        return false;
      m_channels = std::make_shared<ChannelList>(std::move(channels));
      m_uidToStreamId = std::make_shared<UidToStreamMap>(std::move(uidToStreamId));
      m_groupMembers = std::make_shared<GroupMembersMap>(std::move(groupMembers));
      m_groupNamesOrdered = std::make_shared<std::vector<std::string>>(std::move(groupNamesOrdered));
    }

    kodi::Log(ADDON_LOG_INFO,
              "pvr.dispatcharr: seeded channels from cache (%u channels, ts=%llu)",
              chCount, static_cast<unsigned long long>(ts));
    return true;
  }

  void SaveCache(const std::string& signature,
                 const std::vector<xtream::LiveCategory>& categories,
                 const std::vector<CacheChannel>& cacheChannels)
  {
    const std::string path = CachePath();
    if (path.empty())
      return;

    std::string blob;
    blob.reserve(64 + signature.size() + cacheChannels.size() * 64);
    AppendU32(blob, kCacheMagic);
    AppendU32(blob, static_cast<uint32_t>(signature.size()));
    blob.append(signature);
    const uint64_t ts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count());
    AppendU64(blob, ts);

    std::vector<std::pair<int, std::string>> cats;
    cats.reserve(categories.size());
    for (const auto& c : categories)
    {
      if (c.id <= 0 || c.name.empty())
        continue;
      cats.emplace_back(c.id, c.name);
    }
    AppendU32(blob, static_cast<uint32_t>(cats.size()));
    for (const auto& kv : cats)
    {
      AppendI32(blob, static_cast<int32_t>(kv.first));
      AppendU32(blob, static_cast<uint32_t>(kv.second.size()));
      blob.append(kv.second);
    }

    AppendU32(blob, static_cast<uint32_t>(cacheChannels.size()));
    for (const auto& c : cacheChannels)
    {
      AppendU32(blob, static_cast<uint32_t>(c.uid));
      AppendI32(blob, static_cast<int32_t>(c.categoryId));
      AppendU32(blob, static_cast<uint32_t>(c.channelNumber));
      AppendU32(blob, static_cast<uint32_t>(c.name.size()));
      blob.append(c.name);
    }

    (void)WriteStringToFileAtomic(path, blob);
  }

  void StartEpgWorkerThread()
  {
    m_epgWorker = std::thread([this]() {
      while (true)
      {
        uint64_t gen = 0;
        xtream::Settings settings;
        std::shared_ptr<dispatcharr::Client> dispatchClient;
        std::shared_ptr<const std::vector<xtream::LiveStream>> streams;

        {
          std::unique_lock<std::mutex> lock(m_mutex);
          m_epgCv.wait(lock, [this]() { return m_stopRequested || m_epgRefreshRequested; });
          if (m_stopRequested)
            return;

          m_epgRefreshRequested = false;
          m_epgRefreshInProgress = true;
          gen = m_generation.load();
          settings = m_xtreamSettings;
          dispatchClient = m_dispatcharrClient;
          streams = m_streams;
        }

        if (!streams)
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          m_epgRefreshInProgress = false;
          continue;
        }

        std::vector<xtream::ChannelEpg> parsedEpg;
        bool parsed = false;
        std::string fetchDetails;
        if (settings.apiMode == "native")
        {
          parsed = dispatchClient && FetchNativeEpg(*dispatchClient, *streams, parsedEpg);
          if (!parsed)
            fetchDetails = "failed to fetch/translate native EPG grid";
        }
        else
        {
          std::string xmltvData;
          const xtream::FetchResult fetchResult = xtream::FetchXMLTVEpg(settings, xmltvData);
          parsed = fetchResult.ok && xtream::ParseXMLTV(xmltvData, *streams, parsedEpg);
          fetchDetails = fetchResult.details;
        }
        std::vector<unsigned int> channelUids;

        {
          std::lock_guard<std::mutex> lock(m_mutex);
          m_epgRefreshInProgress = false;

          // A settings/channel reload invalidates results from an older request.
          if (m_stopRequested || gen != m_generation.load())
            continue;

          if (parsed)
          {
            m_epgData = std::make_shared<std::vector<xtream::ChannelEpg>>(std::move(parsedEpg));
            m_lastSuccessfulEpgRefresh = std::chrono::steady_clock::now();
            if (m_uidToStreamId)
            {
              channelUids.reserve(m_uidToStreamId->size());
              for (const auto& entry : *m_uidToStreamId)
                channelUids.push_back(entry.first);
            }
          }
        }

        if (!parsed)
        {
          kodi::Log(ADDON_LOG_WARNING, "pvr.dispatcharr: failed to refresh EPG data: %s",
                    fetchDetails.c_str());
          continue;
        }

        kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: refreshed EPG for %zu channels",
                  channelUids.size());
        // Kodi callbacks must not be made while holding m_mutex.
        for (const unsigned int channelUid : channelUids)
          TriggerEpgUpdate(channelUid);
      }
    });
  }

  void RequestEpgRefreshIfStale()
  {
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopRequested || !m_dataLoaded || !m_streams || m_epgRefreshInProgress ||
          m_epgRefreshRequested)
        return;

      const auto now = std::chrono::steady_clock::now();
      if (m_epgData && m_lastSuccessfulEpgRefresh != std::chrono::steady_clock::time_point{} &&
          now - m_lastSuccessfulEpgRefresh < kEpgRefreshInterval)
        return;
      if (m_lastEpgRefreshAttempt != std::chrono::steady_clock::time_point{} &&
          now - m_lastEpgRefreshAttempt < kEpgRefreshRetryInterval)
        return;

      m_lastEpgRefreshAttempt = now;
      m_epgRefreshRequested = true;
      notify = true;
    }

    if (notify)
      m_epgCv.notify_one();
  }

  void StartWorkerThread()
  {
    bool shouldStart = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_workerStarted)
      {
        m_workerStarted = true;
        shouldStart = true;
      }
    }

    if (!shouldStart)
      return;

    m_worker = std::thread([this]() {
      while (true)
      {
        uint64_t gen = 0;
        xtream::Settings settings;
        std::shared_ptr<dispatcharr::Client> dispatchClient;
        std::string streamFormat;
        std::string channelNumbering;
        std::string filterRaw;
        std::string categoryFilterMode;
        std::string categoryFilterRaw;
        bool filterChannelSeparators = true;

        {
          std::unique_lock<std::mutex> lock(m_mutex);
          m_cv.wait(lock, [this]() { return m_stopRequested || m_workRequested; });
          if (m_stopRequested)
            return;

          // Consume the current work request. If a new request comes in while we're
          // loading, EnsureLoaded() will set m_workRequested=true again.
          m_workRequested = false;

          gen = m_generation.load();
          settings = m_xtreamSettings;
          dispatchClient = m_dispatcharrClient;
          streamFormat = m_streamFormat;
          channelNumbering = m_channelNumbering;
          filterRaw = m_filterPatternsRaw;
          categoryFilterMode = m_categoryFilterMode;
          categoryFilterRaw = m_categoryFilterPatternsRaw;
          filterChannelSeparators = m_filterChannelSeparators;
        }

        kodi::QueueNotification(QUEUE_INFO, ADDON_NAME, "Loading channels...");
        const auto t0 = std::chrono::steady_clock::now();

        const std::vector<std::string> patterns = SplitPatterns(filterRaw);
        const std::vector<std::string> categoryPatterns = SplitPatterns(categoryFilterRaw);
        const std::string categoryModeLower = ToLower(categoryFilterMode);
        const bool wantsUncategorized = (!categoryPatterns.empty() &&
                                         (categoryModeLower == "include" || categoryModeLower == "exclude") &&
                                         ShouldFilterOut(categoryPatterns, "Uncategorized"));

        auto failLoad = [&](const std::string& details) {
          {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_loading = false;
            m_dataLoaded = false;
            m_workRequested = false;
          }
          kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: failed to load channels (%s)", details.c_str());
          kodi::QueueNotification(QUEUE_ERROR, ADDON_NAME,
                                 (std::string("Channel load failed: ") + details).c_str());
        };

        std::vector<xtream::LiveCategory> categories;
        std::vector<xtream::LiveStream> streams;

        if (settings.apiMode == "native")
        {
          std::string nativeError = dispatchClient ? std::string() : std::string("Dispatcharr client not initialised");
          if (!nativeError.empty() || !FetchNativeCategoriesAndStreams(*dispatchClient, categories, streams, nativeError))
          {
            failLoad(nativeError);
            continue;
          }

          // If settings changed while we were loading, discard results and immediately loop.
          if (m_stopRequested || gen != m_generation.load())
            continue;
        }
        else
        {
          const xtream::FetchResult catsRes = xtream::FetchLiveCategories(settings, categories);

          // If settings changed while we were loading, discard results and immediately loop.
          if (m_stopRequested || gen != m_generation.load())
            continue;

          if (!catsRes.ok)
          {
            failLoad(catsRes.details);
            continue;
          }

          // Stream fetch strategy:
          // - When category filtering is inactive (or includes "Uncategorized"), prefer single-call all streams.
          // - When category filtering is active and the kept set is small, fetch streams per category.
          if (categoryPatterns.empty() || categoryModeLower == "all" || wantsUncategorized)
          {
            const xtream::FetchResult sRes = xtream::FetchLiveStreams(settings, 0, streams);
            if (!sRes.ok)
            {
              failLoad(sRes.details);
              continue;
            }
          }
          else
          {
            std::vector<int> keepCatIds;
            keepCatIds.reserve(categories.size());
            for (const auto& c : categories)
            {
              if (c.id <= 0 || c.name.empty())
                continue;
              const bool match = ShouldFilterOut(categoryPatterns, c.name);
              if (categoryModeLower == "include")
              {
                if (match)
                  keepCatIds.push_back(c.id);
              }
              else if (categoryModeLower == "exclude")
              {
                if (!match)
                  keepCatIds.push_back(c.id);
              }
            }

            const size_t totalCats = categories.size();
            const bool usePerCategory = (keepCatIds.size() <= 20) ||
                                        (totalCats > 0 && keepCatIds.size() * 4 <= totalCats);
            if (!usePerCategory)
            {
              const xtream::FetchResult sRes = xtream::FetchLiveStreams(settings, 0, streams);
              if (!sRes.ok)
              {
                failLoad(sRes.details);
                continue;
              }
            }
            else
            {
              streams.clear();
              std::vector<xtream::LiveStream> tmp;
              for (const int catId : keepCatIds)
              {
                tmp.clear();
                const xtream::FetchResult sRes = xtream::FetchLiveStreams(settings, catId, tmp);
                if (!sRes.ok)
                {
                  // Fallback to single call.
                  streams.clear();
                  const xtream::FetchResult sRes2 = xtream::FetchLiveStreams(settings, 0, streams);
                  if (!sRes2.ok)
                  {
                    failLoad(sRes2.details);
                    goto fetched;
                  }
                  break;
                }
                streams.insert(streams.end(), tmp.begin(), tmp.end());
              }
            }
          }
        }

      fetched:

        // If settings changed while we were loading, discard results and immediately loop.
        if (m_stopRequested || gen != m_generation.load())
          continue;

        std::unordered_map<int, std::string> categoryIdToName;
        categoryIdToName.reserve(categories.size());
        for (const auto& c : categories)
        {
          if (c.id <= 0)
            continue;
          if (c.name.empty())
            continue;
          categoryIdToName.emplace(c.id, c.name);
        }

        // patterns/categoryPatterns/categoryModeLower already computed above

        auto SanitizeChannelName = [](const std::string& in) -> std::string {
          // Trim leading/trailing whitespace
          auto ltrim = [](const std::string& s) {
            size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
              ++i;
            return s.substr(i);
          };
          auto rtrim = [](const std::string& s) {
            if (s.empty())
              return s;
            size_t j = s.size();
            while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1])))
              --j;
            return s.substr(0, j);
          };

          std::string s = rtrim(ltrim(in));

          // Decode a few common HTML entities providers often embed
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

          // Strip literal unicode escape-code text: uXXXX or \uXXXX
          auto hexVal = [](char ch) -> int {
            if (ch >= '0' && ch <= '9')
              return ch - '0';
            if (ch >= 'a' && ch <= 'f')
              return 10 + (ch - 'a');
            if (ch >= 'A' && ch <= 'F')
              return 10 + (ch - 'A');
            return -1;
          };

          std::string out;
          out.reserve(s.size());
          for (size_t i = 0; i < s.size();)
          {
            if (s[i] == '\\' && i + 5 < s.size() && s[i + 1] == 'u')
            {
              const int h1 = hexVal(s[i + 2]);
              const int h2 = hexVal(s[i + 3]);
              const int h3 = hexVal(s[i + 4]);
              const int h4 = hexVal(s[i + 5]);
              if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0)
              {
                i += 6;
                continue;
              }
            }
            if (s[i] == 'u' && i + 4 < s.size())
            {
              const int h1 = hexVal(s[i + 1]);
              const int h2 = hexVal(s[i + 2]);
              const int h3 = hexVal(s[i + 3]);
              const int h4 = hexVal(s[i + 4]);
              if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0)
              {
                i += 5;
                continue;
              }
            }
            out.push_back(s[i]);
            ++i;
          }

          // Collapse whitespace runs
          std::string collapsed;
          collapsed.reserve(out.size());
          bool prevSpace = false;
          for (unsigned char ch : out)
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

          return rtrim(ltrim(collapsed));
        };

        std::vector<kodi::addon::PVRChannel> channels;
        channels.reserve(streams.size());
        std::unordered_map<unsigned int, int> uidToStreamId;
        uidToStreamId.reserve(streams.size());
        std::unordered_map<std::string, std::vector<GroupMember>> groupMembers;
        std::vector<std::string> groupNamesOrdered;
        std::vector<CacheChannel> cacheChannels;
        cacheChannels.reserve(streams.size());

        constexpr size_t kIconEnableThreshold = 800; // keep Kodi responsive on very large lists
        const bool allowIcons = streams.size() <= kIconEnableThreshold;

        int sequentialChannelNumber = 1;
        const std::string channelNumberingLower = ToLower(channelNumbering);

        size_t totalValid = 0;
        for (const auto& s : streams)
        {
          if (s.id <= 0)
            continue;
          if (s.name.empty())
            continue;

          if (filterChannelSeparators && LooksLikeChannelSeparator(s.name))
            continue;

          ++totalValid;

          // Category filtering is applied before channel-name filtering.
          if (!categoryPatterns.empty() && (categoryModeLower == "include" || categoryModeLower == "exclude"))
          {
            std::string categoryName;
            auto catItForFilter = categoryIdToName.find(s.categoryId);
            if (catItForFilter != categoryIdToName.end())
              categoryName = catItForFilter->second;
            if (categoryName.empty())
              categoryName = "Uncategorized";

            const bool categoryMatches = ShouldFilterOut(categoryPatterns, categoryName);
            if (categoryModeLower == "include" && !categoryMatches)
              continue;
            if (categoryModeLower == "exclude" && categoryMatches)
              continue;
          }

          if (ShouldFilterOut(patterns, s.name))
            continue;

          kodi::addon::PVRChannel ch;
          ch.SetUniqueId(static_cast<unsigned int>(s.id));
          ch.SetIsRadio(false);
          const std::string chName = SanitizeChannelName(s.name);
          ch.SetChannelName(chName);

          int channelNumber = sequentialChannelNumber;
          if (channelNumberingLower == "provider" && s.number > 0)
            channelNumber = s.number;
          ch.SetChannelNumber(channelNumber);

          if (allowIcons && !s.icon.empty())
            ch.SetIconPath(s.icon);

          channels.push_back(std::move(ch));
          uidToStreamId.emplace(static_cast<unsigned int>(s.id), s.id);

          CacheChannel cc;
          cc.uid = static_cast<unsigned int>(s.id);
          cc.categoryId = s.categoryId;
          cc.channelNumber = static_cast<unsigned int>(channelNumber);
          cc.name = chName;
          cacheChannels.push_back(std::move(cc));

          auto catIt = categoryIdToName.find(s.categoryId);
          if (catIt != categoryIdToName.end())
          {
            GroupMember gm;
            gm.channelUid = static_cast<unsigned int>(s.id);
            gm.channelNumber = static_cast<unsigned int>(channelNumber);
            gm.subChannelNumber = 0;
            groupMembers[catIt->second].push_back(gm);
          }

          ++sequentialChannelNumber;
        }

        // Add category-based groups
        for (const auto& c : categories)
        {
          auto catIt = categoryIdToName.find(c.id);
          if (catIt == categoryIdToName.end())
            continue;
          const auto memIt = groupMembers.find(catIt->second);
          if (memIt == groupMembers.end() || memIt->second.empty())
            continue;
          groupNamesOrdered.push_back(catIt->second);
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        {
          std::lock_guard<std::mutex> lock(m_mutex);
          if (m_stopRequested || gen != m_generation.load())
            continue;

          m_channels = std::make_shared<ChannelList>(std::move(channels));
          m_uidToStreamId = std::make_shared<UidToStreamMap>(std::move(uidToStreamId));
          m_groupMembers = std::make_shared<GroupMembersMap>(std::move(groupMembers));
          m_groupNamesOrdered = std::make_shared<std::vector<std::string>>(std::move(groupNamesOrdered));
          m_streams = std::make_shared<std::vector<xtream::LiveStream>>(streams);

          m_xtreamSettings = settings;
          m_streamFormat = streamFormat;
          m_loading = false;
          m_dataLoaded = true;
          m_groupsReady = true;
        }

        kodi::Log(ADDON_LOG_INFO,
                  "pvr.dispatcharr: loaded %zu channels in %zu categories (%lld ms)",
                  m_channels ? m_channels->size() : 0u, categories.size(), static_cast<long long>(ms));

        const size_t loaded = m_channels ? m_channels->size() : 0u;
        std::string msg = std::string("Loaded ") + std::to_string(loaded) + " channels";
        kodi::QueueNotification(QUEUE_INFO, ADDON_NAME, msg.c_str());

        // Best-effort cache write so startup can seed channels immediately.
        SaveCache(m_settingsSignature, categories, cacheChannels);

        // EPG has its own refresh worker so XMLTV network/parsing work never blocks
        // this channel loader or Kodi's PVR callback threads.
        RequestEpgRefreshIfStale();

        // Always refresh groups after reload so Kodi drops stale groups/members.
        TriggerChannelUpdate();
        TriggerChannelGroupsUpdate();
      }
    });
  }

  void StartRecordingMonitorThread()
  {
    m_recordingMonitor = std::thread([this]() {
      std::string clientSignature;
      std::unique_ptr<dispatcharr::Client> client;
      std::unordered_map<int, std::string> previousStatuses;

      while (!m_stopRequested)
      {
        xtream::Settings settings;
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          settings = m_xtreamSettings;
        }

        const std::string password = !settings.dispatcharrPassword.empty()
                                         ? settings.dispatcharrPassword
                                         : settings.password;
        const std::string signature = settings.server + "\n" +
                                      std::to_string(settings.port) + "\n" +
                                      settings.username + "\n" + password;
        if (!settings.server.empty() && !settings.username.empty() && !password.empty())
        {
          if (!client || signature != clientSignature)
          {
            dispatcharr::DvrSettings ds;
            ds.server = settings.server;
            ds.port = settings.port;
            ds.username = settings.username;
            ds.password = password;
            ds.timeoutSeconds = settings.timeoutSeconds;
            client = std::make_unique<dispatcharr::Client>(ds);
            clientSignature = signature;
            previousStatuses.clear();
          }

          std::vector<dispatcharr::Recording> recordings;
          if (client->FetchRecordings(recordings))
          {
            std::unordered_map<int, std::string> currentStatuses;
            bool changed = false;
            for (const auto& recording : recordings)
            {
              currentStatuses.emplace(recording.id, recording.status);
              const auto old = previousStatuses.find(recording.id);
              if (old == previousStatuses.end())
              {
                changed = true;
              }
              else if (old->second != recording.status)
              {
                changed = true;
                kodi::Log(ADDON_LOG_INFO,
                          "pvr.dispatcharr: recording %d changed status from '%s' to '%s'",
                          recording.id, old->second.c_str(), recording.status.c_str());
              }
            }
            changed = changed || currentStatuses.size() != previousStatuses.size();

            // Notify only for actual collection/status transitions. Triggering
            // from GetTimers(), or on every poll while active, creates a refresh
            // feedback loop that can cancel playback while Kodi starts it.
            if (changed)
            {
              TriggerTimerUpdate();
              TriggerRecordingUpdate();
            }
            previousStatuses = std::move(currentStatuses);
          }
        }

        std::unique_lock<std::mutex> lock(m_recordingMutex);
        m_recordingCv.wait_for(lock, kRecordingPollInterval,
                              [this]() { return m_stopRequested.load(); });
      }
    });
  }

  void StartBootstrapThread()
  {
    // Kodi can create the PVR instance before settings are fully available.
    // Ensure we attempt to load once credentials become readable.
    if (m_bootstrap.joinable())
      m_bootstrap.join();

    m_bootstrap = std::thread([this]() {
      // Try for a short window after startup; stop once loading begins.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!m_stopRequested && std::chrono::steady_clock::now() < deadline)
      {
        EnsureLoaded();

        bool done = false;
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          done = m_loading || m_dataLoaded;
        }
        if (done)
          return;

        std::this_thread::sleep_for(std::chrono::milliseconds(750));
      }
    });
  }

  void EnsureLoaded()
  {
    // Never block Kodi UI/PVR thread on a large HTTP+parse operation.
    // Instead, schedule a background load if needed and serve cached data (or 0) meanwhile.

    xtream::Settings xt;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_hasSettingsOverride)
        xt = m_settingsOverride;
      else
        xt = xtream::LoadSettings();
    }

    const bool haveCreds = !Trim(xt.server).empty() && !Trim(xt.username).empty() && !Trim(xt.password).empty() &&
                           (xt.port > 0 && xt.port <= 65535);
    if (!haveCreds)
    {
      bool shouldWarn = false;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_warnedMissingCreds)
        {
          m_warnedMissingCreds = true;
          shouldWarn = true;
        }
      }
      if (shouldWarn)
      {
        kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr: credentials missing or invalid; skipping load");
        kodi::QueueNotification(QUEUE_ERROR, ADDON_NAME,
                                "Xtream Codes credentials are missing or invalid. Please update settings.");
      }
      return;
    }

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_warnedMissingCreds = false;
    }

    std::string streamFormat;
    kodi::addon::GetSettingString("stream_format", streamFormat);
    if (streamFormat.empty())
      streamFormat = "ts";

    std::string channelNumbering;
    kodi::addon::GetSettingString("channel_numbering", channelNumbering);
    if (channelNumbering.empty())
      channelNumbering = "sequential";

    std::string filterRaw;
    kodi::addon::GetSettingString("channel_filter_patterns", filterRaw);

    bool filterChannelSeparators = true;
    kodi::addon::GetSettingBoolean("filter_channel_separators", filterChannelSeparators);

    std::string categoryFilterMode;
    kodi::addon::GetSettingString("category_filter_mode", categoryFilterMode);
    if (categoryFilterMode.empty())
      categoryFilterMode = "all";

    std::string categoryFilterRaw;
    kodi::addon::GetSettingString("category_filter_patterns", categoryFilterRaw);

    // Kodi can fail to initialize addon settings for binary addons early during startup.
    // In that case, GetSettingString() can return defaults/empties even though settings are
    // persisted in addon_data. Heuristic: if everything looks like defaults, load from
    // addon_data/settings.xml.
    const bool looksLikeDefaults = (ToLower(categoryFilterMode) == "all") &&
                                  Trim(categoryFilterRaw).empty() &&
                                  Trim(filterRaw).empty();

    if (looksLikeDefaults)
    {
      std::string xml;
      if (ReadVfsTextFile("special://profile/addon_data/pvr.dispatcharr/settings.xml", xml))
      {
        std::string tmp;
        if (ExtractSettingValue(xml, "stream_format", tmp) && !tmp.empty())
          streamFormat = tmp;
        if (ExtractSettingValue(xml, "channel_numbering", tmp) && !tmp.empty())
          channelNumbering = tmp;

        if (ExtractSettingValue(xml, "channel_filter_patterns", tmp))
          filterRaw = tmp;
        if (ExtractSettingValue(xml, "category_filter_mode", tmp) && !tmp.empty())
          categoryFilterMode = tmp;
        if (ExtractSettingValue(xml, "category_filter_patterns", tmp))
          categoryFilterRaw = tmp;
      }
    }

    if (categoryFilterMode.empty())
      categoryFilterMode = "all";

    const std::string sig = xt.server + ":" + std::to_string(xt.port) + "/" + xt.username + "/" +
                HashHex(xt.password) + "|api=" + ToLower(xt.apiMode) + "|fmt=" + ToLower(streamFormat) + "|num=" +
                ToLower(channelNumbering) + "|flt=" + HashHex(filterRaw) + "|catmode=" +
                ToLower(categoryFilterMode) + "|catflt=" + HashHex(categoryFilterRaw) + "|sep=" +
                (filterChannelSeparators ? "1" : "0");

    if (m_cacheSignatureAttempted != sig)
    {
      m_cacheSignatureAttempted = sig;
      (void)TryLoadCacheForSignature(sig);
    }

    bool shouldStart = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_dataLoaded && sig == m_settingsSignature && !m_loading)
        return;
      if (m_loading && sig == m_settingsSignature)
        return;

      m_settingsSignature = sig;
      m_loading = true;
      m_dataLoaded = false;
      m_groupsReady = false;
      m_epgData.reset();
      m_lastSuccessfulEpgRefresh = {};
      m_lastEpgRefreshAttempt = {};

      m_xtreamSettings = std::move(xt);

      // Initialize Dispatcharr Client
      dispatcharr::DvrSettings ds;
      ds.server = m_xtreamSettings.server;
      ds.port = m_xtreamSettings.port;
      ds.username = m_xtreamSettings.username;
      // Use specific dispatcharr password if provided, else fall back to main password
      ds.password = !m_xtreamSettings.dispatcharrPassword.empty() 
                      ? m_xtreamSettings.dispatcharrPassword 
                      : m_xtreamSettings.password;
      ds.timeoutSeconds = m_xtreamSettings.timeoutSeconds;
      m_dispatcharrClient = std::make_shared<dispatcharr::Client>(ds);

      m_streamFormat = ToLower(streamFormat);
      m_channelNumbering = ToLower(channelNumbering);
      m_filterPatternsRaw = filterRaw;
      m_categoryFilterMode = ToLower(categoryFilterMode);
      m_categoryFilterPatternsRaw = categoryFilterRaw;
      m_filterChannelSeparators = filterChannelSeparators;

      ++m_generation;
      m_workRequested = true;
      shouldStart = true;
    }

    if (shouldStart)
    {
      StartWorkerThread();
      m_cv.notify_one();
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::condition_variable m_epgCv;
  std::thread m_worker;
  std::thread m_epgWorker;
  std::thread m_bootstrap;
  std::thread m_recordingMonitor;
  std::mutex m_recordingMutex;
  std::condition_variable m_recordingCv;
  std::atomic<bool> m_stopRequested{false};
  std::atomic<uint64_t> m_generation{0};
  std::atomic<int64_t> m_lastRefreshTriggerMs{0};
  bool m_workerStarted = false;
  bool m_workRequested = false;
  bool m_loading = false;
  bool m_dataLoaded = false;
  bool m_groupsReady = false;
  bool m_epgRefreshRequested = false;
  bool m_epgRefreshInProgress = false;
  std::chrono::steady_clock::time_point m_lastSuccessfulEpgRefresh;
  std::chrono::steady_clock::time_point m_lastEpgRefreshAttempt;
  std::string m_settingsSignature;
  bool m_hasSettingsOverride = false;
  xtream::Settings m_settingsOverride;
  xtream::Settings m_xtreamSettings;
  // shared_ptr (not unique_ptr): the channel/EPG worker threads take a copy
  // of this under m_mutex when native api_mode is active, so the Client must
  // stay alive for as long as an in-flight fetch holds a reference, even if
  // a settings reload replaces m_dispatcharrClient concurrently.
  std::shared_ptr<dispatcharr::Client> m_dispatcharrClient;
  std::unique_ptr<dispatcharr::recording::IRecordedStream> m_activeRecordedStream;
  std::unique_ptr<dispatcharr::recording::NativeCatchupLiveStream> m_activeNativeLiveCatchupStream;
  std::string m_streamFormat;
  std::string m_channelNumbering;
  std::string m_filterPatternsRaw;
  std::string m_categoryFilterMode;
  std::string m_categoryFilterPatternsRaw;
  bool m_filterChannelSeparators = true;
  bool m_warnedMissingCreds = false;
  using ChannelList = std::vector<kodi::addon::PVRChannel>;
  using UidToStreamMap = std::unordered_map<unsigned int, int>;
  using GroupMembersMap = std::unordered_map<std::string, std::vector<GroupMember>>;

  std::shared_ptr<const ChannelList> m_channels;
  std::shared_ptr<const UidToStreamMap> m_uidToStreamId;
  std::shared_ptr<const std::vector<std::string>> m_groupNamesOrdered;
  std::shared_ptr<const GroupMembersMap> m_groupMembers;
  std::shared_ptr<const std::vector<xtream::ChannelEpg>> m_epgData;
  std::shared_ptr<const std::vector<xtream::LiveStream>> m_streams;

  // Catchup playback state - set by GetEPGTagStreamProperties, consumed by GetChannelStreamProperties
  struct PendingCatchup
  {
    std::string url;
    std::string templateUrl;  // For ffmpegdirect catchup mode
    int64_t expiresAtMs = 0;
    time_t programStart = 0;
    time_t programEnd = 0;
    time_t adjustedStart = 0;  // Start time adjusted for catchup offset
    bool useFFmpegDirect = false;
  };
  std::unordered_map<unsigned int, PendingCatchup> m_pendingCatchupByChannel;

  // Native catchup: set by GetEPGTagStreamProperties (native api_mode),
  // consumed by OpenLiveStream. Unlike PendingCatchup above, this selects
  // Kodi's raw OpenLiveStream/ReadLiveStream/SeekLiveStream byte-callback
  // path (no STREAMURL at all) - see NativeCatchupLiveStream for why.
  struct PendingNativeCatchupOpen
  {
    std::string channelUuid;
    time_t programStart = 0;
    time_t programEnd = 0;
    int64_t expiresAtMs = 0;
  };
  std::unordered_map<unsigned int, PendingNativeCatchupOpen> m_pendingNativeCatchupOpenByChannel;

  // Active catchup playback - persists during playback for GetStreamTimes/CanSeekStream/IsRealTimeStream
  PendingCatchup m_activeCatchup;
  unsigned int m_activeCatchupChannelUid = 0;

  std::string m_cacheSignatureAttempted;

  size_t m_lastEnsureLogHash = 0;
};

class ATTR_DLL_LOCAL CXtreamCodesAddon final : public kodi::addon::CAddonBase
{
public:
  CXtreamCodesAddon() = default;

  ADDON_STATUS SetSetting(const std::string& settingName,
                          const kodi::addon::CSettingValue& settingValue) override
  {
    const bool isConnectionSetting = (settingName == "server") || (settingName == "port") ||
                                     (settingName == "username") || (settingName == "password") ||
                                     (settingName == "timeout_seconds");

    const bool isReloadAffectingSetting = isConnectionSetting ||
                       (settingName == "stream_format") ||
                       (settingName == "channel_numbering") ||
                       (settingName == "channel_filter_patterns") ||
                       (settingName == "category_filter_mode") ||
                       (settingName == "category_filter_patterns") ||
                       (settingName == "filter_channel_separators");

    auto haveMinCredentials = [](const xtream::Settings& s) -> bool {
      if (Trim(s.server).empty() || Trim(s.username).empty() || Trim(s.password).empty())
        return false;
      if (s.port <= 0 || s.port > 65535)
        return false;
      return true;
    };

    // Cache latest values as Kodi reports them, so actions (like Test connection)
    // can use the current UI values even if Kodi hasn't persisted them yet.
    if (settingName == "server")
      m_cachedSettings.server = settingValue.GetString();
    else if (settingName == "port")
      m_cachedSettings.port = settingValue.GetInt();
    else if (settingName == "username")
      m_cachedSettings.username = settingValue.GetString();
    else if (settingName == "password")
      m_cachedSettings.password = settingValue.GetString();
    else if (settingName == "timeout_seconds")
      m_cachedSettings.timeoutSeconds = settingValue.GetInt();
    else if (settingName == "catchup_start_offset_hours")
      m_cachedSettings.catchupStartOffsetHours = settingValue.GetInt();
    else if (settingName == "enable_user_agent_spoofing")
      m_cachedSettings.enableUserAgentSpoofing = settingValue.GetBoolean();
    else if (settingName == "custom_user_agent")
      m_cachedSettings.customUserAgent = settingValue.GetString();
    else if (settingName == "xmltv_max_size_mb")
      m_cachedSettings.xmltvMaxSizeMb = settingValue.GetInt();

    m_hasCachedSettings = true;

    // Keep the active PVR instance in sync with the latest UI values so the loader
    // doesn't read stale/empty settings right after the user hits Test.
    if (m_pvrClient)
      m_pvrClient->SetSettingsOverride(m_cachedSettings);

    // Kodi can cache an empty or stale channel list if PVR starts before credentials are set.
    // For connection/streaming settings we refresh immediately; for filters we refresh only
    // when the user presses the Apply button.
    if (isReloadAffectingSetting && m_pvrClient)
    {
      const xtream::Settings s = m_hasCachedSettings ? m_cachedSettings : xtream::LoadSettings();
      if (haveMinCredentials(s))
      {
        kodi::Log(ADDON_LOG_INFO,
                  "pvr.dispatcharr: settings changed (%s) -> trigger channel refresh",
                  settingName.c_str());
        m_pvrClient->TriggerKodiRefreshThrottled();
      }
    }

    // Other settings will be applied lazily on next PVR callback.
    return ADDON_STATUS_OK;
  }

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    if (instance.IsType(ADDON_INSTANCE_PVR))
    {
      auto* client = new CXtreamCodesPVRClient(instance);
      hdl = client;
      m_pvrClient = client;

      // Seed the PVR instance with the best-known settings so its loader
      // doesn't read empty values during early startup.
      const xtream::Settings s = m_hasCachedSettings ? m_cachedSettings : xtream::LoadSettings();
      m_pvrClient->SetSettingsOverride(s);

      // If settings are already valid on disk, trigger an initial refresh so
      // Kodi calls into the client and the loader starts automatically at boot.
      auto haveMinCredentials = [](const xtream::Settings& st) -> bool {
        if (Trim(st.server).empty() || Trim(st.username).empty() || Trim(st.password).empty())
          return false;
        if (st.port <= 0 || st.port > 65535)
          return false;
        return true;
      };
      if (haveMinCredentials(s))
      {
        m_pvrClient->TriggerChannelUpdate();
        m_pvrClient->TriggerChannelGroupsUpdate();
      }
      return ADDON_STATUS_OK;
    }
    return ADDON_STATUS_NOT_IMPLEMENTED;
  }

  void DestroyInstance(const kodi::addon::IInstanceInfo& instance, const KODI_ADDON_INSTANCE_HDL hdl) override
  {
    if (instance.IsType(ADDON_INSTANCE_PVR) && hdl == m_pvrClient)
      m_pvrClient = nullptr;
  }

private:
  CXtreamCodesPVRClient* m_pvrClient = nullptr;
  bool m_hasCachedSettings = false;
  xtream::Settings m_cachedSettings;
};

ADDONCREATOR(CXtreamCodesAddon)
