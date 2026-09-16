#pragma once

#include <string>
#include <vector>
#include <map>
#include <ctime>

namespace xtream
{
struct Settings
{
  std::string server;
  int port = 80;
  std::string username;
  std::string password;
  std::string dispatcharrPassword; // Separate password for API
  int timeoutSeconds = 30;

  // "xtream" (default, provider-compatibility layer) or "native" (Dispatcharr's
  // own REST API - channels/groups/EPG/live/catchup all sourced from
  // dispatcharr::Client instead of the Xtream Codes emulation layer below).
  std::string apiMode = "xtream";

  bool enableUserAgentSpoofing = false;
  std::string customUserAgent;
  
  int catchupStartOffsetHours = 0;
  bool enablePlayFromStart = true;
  bool useFFmpegDirect = false;

  int xmltvMaxSizeMb = 300; // Ceiling for the XMLTV EPG download, in megabytes.
};

struct TestResult
{
  bool ok = false;
  std::string details;
};

struct LiveCategory
{
  int id = 0;
  std::string name;
};

struct LiveStream
{
  int id = 0;
  int categoryId = 0;
  int number = 0;
  std::string name;
  std::string icon;
  std::string epgChannelId; // XMLTV channel id from provider (if available)

  // Catchup/Archive support
  bool tvArchive = false;
  int tvArchiveDuration = 0; // Duration in days

  // Dispatcharr channel UUID. Only populated when Settings::apiMode == "native"
  // (the Xtream compatibility path has no concept of it) - needed to build
  // native live/catchup URLs via dispatcharr::Client.
  std::string uuid;
};

struct EpgEntry
{
  std::string channelId;      // Maps to stream ID or tvg-id
  time_t startTime = 0;
  time_t endTime = 0;
  std::string title;
  std::string description;
  std::string episodeName;    // Sub-title
  std::string iconPath;
  std::string genreString;
  int genreType = 0;
  int genreSubType = 0;
  int year = 0;
  int starRating = 0;
  int seasonNumber = -1;
  int episodeNumber = -1;
};

struct ChannelEpg
{
  std::string id;                          // Channel ID (tvg-id or stream ID)
  std::string displayName;                 // Channel display name
  std::string iconPath;                    // Channel icon from EPG
  std::map<time_t, EpgEntry> entries;      // EPG entries keyed by start time
};

struct FetchResult
{
  bool ok = false;
  std::string details;
};

Settings LoadSettings();
TestResult TestConnection(const Settings& settings);

FetchResult FetchLiveCategories(const Settings& settings, std::vector<LiveCategory>& out);
FetchResult FetchLiveStreams(const Settings& settings, int categoryId, std::vector<LiveStream>& out);
FetchResult FetchAllLiveStreams(const Settings& settings,
                                std::vector<LiveCategory>& categories,
                                std::vector<LiveStream>& streams);

std::string BuildLiveStreamUrl(const Settings& settings, int streamId, const std::string& streamFormat);
std::string BuildCatchupUrl(const Settings& settings, int streamId, time_t startTime, time_t endTime, const std::string& streamFormat);
std::string BuildCatchupUrlTemplate(const Settings& settings, int streamId, int durationMinutes, const std::string& streamFormat);

// EPG/XMLTV functions
FetchResult FetchXMLTVEpg(const Settings& settings, std::string& xmltvData);
bool ParseXMLTV(const std::string& xmltvData, 
                const std::vector<LiveStream>& streams,
                std::vector<ChannelEpg>& channelEpgs);
} // namespace xtream
