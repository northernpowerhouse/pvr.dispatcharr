#pragma once

#include "recorded_stream.h"

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>

namespace dispatcharr { class Client; }

namespace dispatcharr::recording
{

// Drives native catch-up (time-shift) playback through Kodi's raw
// OpenLiveStream/ReadLiveStream/SeekLiveStream byte-callback path instead of
// PVR_STREAM_PROPERTY_STREAMURL. Reuses the IRecordedStream interface - it's
// just Open/Close/Read/Seek/Length, and this class isn't a recording, but
// duplicating an identical interface isn't worth it.
//
// Why this exists: Dispatcharr's /proxy/catchup/ proxy supports real HTTP
// Range seeking within ONE session, but a byte-offset seek lands mid-MPEG-TS
// with a PCR/PTS discontinuity Kodi's built-in ffmpeg demuxer doesn't
// recover from cleanly (confirmed: large audio sync errors, stalled
// position). inputstream.ffmpegdirect's reopen-on-seek catchup mode avoids
// that discontinuity, but its catchup_url_format_string is pure client-side
// URL substitution - it can't mint a fresh session first, and reusing one
// session's session_id with a different `start` is silently ignored by the
// server regardless of how the session was minted (confirmed directly
// against Dispatcharr with curl, not just observed in Kodi - a repeated
// identical `start` and a `start` 40 minutes later both returned
// byte-identical content on the same session_id).
//
// The only thing that reliably re-anchors is a genuinely new session from
// POST /api/catchup/sessions/. This class does that itself: it mints a new
// session whenever Seek() lands far enough from the current session's
// anchor, fetching each session from its own byte 0 (continuous, no
// discontinuity), while presenting one continuous logical byte stream to
// Kodi's demuxer via a bytes-per-second estimate derived from the first
// session's own reported Content-Range total.
class NativeCatchupLiveStream : public IRecordedStream
{
public:
  explicit NativeCatchupLiveStream(Client& client);

  bool Open(const std::string& channelUuid, time_t programStart, time_t programEnd);
  void Close() override;
  int Read(unsigned char* buffer, unsigned int size) override;
  int64_t Seek(int64_t offset, int whence) override;
  int64_t Length() const override;
  bool IsOpen() const override;

private:
  // (Re)anchors playback at wallClockTarget if it's far enough from the
  // current session's anchor to be worth minting a fresh session;
  // logicalBytePos is the Kodi-visible byte position the new anchor
  // corresponds to.
  bool EnsureSessionNear(time_t wallClockTarget, int64_t logicalBytePos);

  Client& m_client;
  mutable std::mutex m_mutex;
  std::string m_channelUuid;
  time_t m_programStart = 0;
  time_t m_programEnd = 0;
  double m_bytesPerSecond = 0.0;

  std::string m_sessionId;
  time_t m_sessionAnchorWallClock = 0;
  int64_t m_sessionAnchorBytePos = 0;

  int64_t m_position = 0;
  int64_t m_length = -1;
  bool m_open = false;
};

} // namespace dispatcharr::recording
