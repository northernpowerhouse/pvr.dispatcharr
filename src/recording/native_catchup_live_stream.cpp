#include "native_catchup_live_stream.h"

#include "../dispatcharr_client.h"

#include <kodi/General.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace dispatcharr::recording
{

namespace
{
// Below this, a seek is treated as routine demuxer buffering/probing near
// the current position rather than a real user seek, so it's absorbed into
// the existing session instead of minting a new one for every small nudge.
constexpr time_t kReanchorThresholdSeconds = 20;
} // namespace

NativeCatchupLiveStream::NativeCatchupLiveStream(Client& client) : m_client(client) {}

bool NativeCatchupLiveStream::Open(const std::string& channelUuid, time_t programStart, time_t programEnd)
{
  Close();

  if (channelUuid.empty() || programEnd <= programStart)
    return false;

  const int durationMinutes = std::max(1, static_cast<int>((programEnd - programStart) / 60));

  dispatcharr::CatchupSession session;
  if (!m_client.CreateCatchupSession(channelUuid, programStart, durationMinutes, session))
    return false;

  // A minimal ranged read both confirms the session is reachable and yields
  // the session's total size via Content-Range, from which a bytes-per-
  // second estimate is derived for mapping future byte-offset seeks to
  // wall-clock time.
  std::string probe;
  int64_t totalLength = 0;
  if (!m_client.FetchCatchupStreamRange(channelUuid, session.sessionId, 0, 1, probe, totalLength) ||
      totalLength <= 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  m_channelUuid = channelUuid;
  m_programStart = programStart;
  m_programEnd = programEnd;
  m_bytesPerSecond = static_cast<double>(totalLength) / static_cast<double>(programEnd - programStart);
  m_sessionId = session.sessionId;
  m_sessionAnchorWallClock = programStart;
  m_sessionAnchorBytePos = 0;
  m_position = 0;
  m_length = totalLength;
  m_open = true;
  kodi::Log(ADDON_LOG_INFO,
            "pvr.dispatcharr: NativeCatchupLiveStream::Open session=%s totalLength=%lld bytesPerSecond=%.1f",
            m_sessionId.c_str(), static_cast<long long>(totalLength), m_bytesPerSecond);
  return true;
}

void NativeCatchupLiveStream::Close()
{
  std::string sessionId;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    sessionId = m_sessionId;
    m_open = false;
    m_sessionId.clear();
    m_position = 0;
    m_length = -1;
    m_bytesPerSecond = 0.0;
  }
  // Best-effort; sessions also expire server-side on their own idle TTL.
  if (!sessionId.empty())
    m_client.DeleteCatchupSession(sessionId);
}

bool NativeCatchupLiveStream::EnsureSessionNear(time_t wallClockTarget, int64_t logicalBytePos)
{
  std::string channelUuid;
  time_t programStart = 0;
  time_t programEnd = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sessionId.empty() &&
        std::abs(static_cast<long long>(wallClockTarget - m_sessionAnchorWallClock)) <= kReanchorThresholdSeconds)
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharr: NativeCatchupLiveStream::EnsureSessionNear absorbed (within threshold), keeping session=%s",
                m_sessionId.c_str());
      return true;
    }
    channelUuid = m_channelUuid;
    programStart = m_programStart;
    programEnd = m_programEnd;
  }

  time_t clampedTarget = wallClockTarget;
  if (clampedTarget < programStart) clampedTarget = programStart;
  if (clampedTarget >= programEnd) clampedTarget = programEnd - 1;

  const int durationMinutes = std::max(1, static_cast<int>((programEnd - clampedTarget) / 60));

  // Network call made without holding m_mutex.
  dispatcharr::CatchupSession session;
  if (!m_client.CreateCatchupSession(channelUuid, clampedTarget, durationMinutes, session))
    return false;

  kodi::Log(ADDON_LOG_INFO,
            "pvr.dispatcharr: NativeCatchupLiveStream re-anchoring: newSession=%s wallClockTarget=%lld bytePos=%lld",
            session.sessionId.c_str(), static_cast<long long>(clampedTarget), static_cast<long long>(logicalBytePos));

  std::lock_guard<std::mutex> lock(m_mutex);
  m_sessionId = session.sessionId;
  m_sessionAnchorWallClock = clampedTarget;
  m_sessionAnchorBytePos = logicalBytePos;
  return true;
}

int NativeCatchupLiveStream::Read(unsigned char* buffer, unsigned int size)
{
  if (!buffer || size == 0)
    return 0;

  std::string channelUuid;
  std::string sessionId;
  int64_t sessionRelativeOffset = 0;
  int64_t wanted = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_open)
      return -1;
    if (m_position >= m_length)
      return 0;
    channelUuid = m_channelUuid;
    sessionId = m_sessionId;
    sessionRelativeOffset = m_position - m_sessionAnchorBytePos;
    wanted = std::min<int64_t>(static_cast<int64_t>(size), m_length - m_position);
  }

  if (sessionRelativeOffset < 0)
  {
    // Shouldn't happen (EnsureSessionNear always anchors at-or-before the
    // current position), but guard against reading before the session's own
    // byte 0 rather than sending a malformed negative Range.
    return -1;
  }

  std::string data;
  int64_t reportedLength = 0;
  if (!m_client.FetchCatchupStreamRange(channelUuid, sessionId, sessionRelativeOffset, wanted, data, reportedLength))
    return -1;
  if (data.empty())
    return 0;

  std::memcpy(buffer, data.data(), data.size());

  std::lock_guard<std::mutex> lock(m_mutex);
  m_position += static_cast<int64_t>(data.size());
  return static_cast<int>(data.size());
}

int64_t NativeCatchupLiveStream::Seek(int64_t offset, int whence)
{
  int64_t target = 0;
  time_t wallClockTarget = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_open || m_bytesPerSecond <= 0.0)
      return -1;

    int64_t base = 0;
    if (whence == SEEK_CUR)
      base = m_position;
    else if (whence == SEEK_END)
      base = m_length;
    else if (whence != SEEK_SET)
      return -1;

    target = base + offset;
    if (target < 0) target = 0;
    if (target > m_length) target = m_length;

    wallClockTarget = m_programStart + static_cast<time_t>(static_cast<double>(target) / m_bytesPerSecond);
  }

  // EnsureSessionNear may make a network call; must not hold m_mutex here.
  if (!EnsureSessionNear(wallClockTarget, target))
    return -1;

  std::lock_guard<std::mutex> lock(m_mutex);
  m_position = target;
  return m_position;
}

int64_t NativeCatchupLiveStream::Length() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_length;
}

bool NativeCatchupLiveStream::IsOpen() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_open;
}

} // namespace dispatcharr::recording
