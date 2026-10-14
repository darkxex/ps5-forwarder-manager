// ps5fwdgen - Find a game's theme music on YouTube and fetch its audio.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Talks to YouTube's own InnerTube API (www.youtube.com/youtubei/v1, the API
// YouTube's apps use), with no key and no third-party server:
//   - visitor: GET  /sw.js_data          -> a visitorData id for this session
//   - search:  POST /youtubei/v1/search  (WEB client, videos only)
//                   "<game> Theme Music", as SDH-GameThemeMusic searches
//   - formats: POST /youtubei/v1/player  (VISIONOS client: direct stream URLs,
//                   no JavaScript signature and no PO token, as yt-dlp's
//                   default JS-less client)
//   - audio:   GET  <googlevideo URL> in Range chunks (as yt-dlp does), each
//                   with its own retry, giving up only when a chunk stalls.
// The stream is AAC in MP4 (itag 140 and friends), because the AT9 converter
// does not read WebM, and only as much of it as the clip needs is fetched: a
// forwarder's music is at most ~87 s, and YouTube's fragmented MP4 stays
// playable when cut short.
// All calls block; run them on a worker thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace yt
{

struct Video
{
    std::string id;
    std::string title;
    std::string author;
    long seconds = 0; // 0 when unknown
    // Where a JPEG of it can be fetched, best first: 480x360 (the 16:9
    // picture letterboxed), then 320x180, then the search result's own.
    std::vector<std::string> thumbs;
};

// An audio-only stream from a video's adaptiveFormats.
struct AudioFormat
{
    std::string url;
    std::string mime; // "audio/mp4; codecs=..."
    long bitrate = 0;
    long sample_rate = 0;
    long length = 0; // bytes (contentLength), 0 when unknown
};

struct Result
{
    bool ok = false;
    std::string error;  // set when ok is false (shown to the user)
    std::string detail; // more for the log (HTTP status, YouTube's reason)
};

// What SDH-GameThemeMusic appends to the game's name.
constexpr const char *kDefaultSuffix = " Theme Music";
// Longer videos are skipped, as SDH's yt-dlp resolver does (20 minutes).
constexpr long kMaxSeconds = 20 * 60;

// Search YouTube for videos.
Result search(const std::string &term, std::vector<Video> &out);

// Bytes per Range request when downloading audio.
constexpr std::size_t kChunkBytes = 256 * 1024;

// Called while audio downloads: bytes so far, out of total (worker thread).
using Progress = std::function<void(std::size_t done, std::size_t total)>;

// Called with the file's bytes in order as they arrive, for decoding while
// the rest downloads (worker thread; calls never overlap).
using Sink = std::function<void(const unsigned char *data, std::size_t size)>;

// Download the start of a video's best AAC audio stream to memory: enough
// for `seconds` of audio at the stream's bitrate, never more than max_bytes.
// filename receives a name with the right extension (<id>.m4a). When cancel
// becomes true the download stops early and fails with "cancelled".
Result download_audio(const std::string &video_id, double seconds, std::size_t max_bytes,
                      std::vector<unsigned char> &audio, std::string &filename,
                      const Progress &progress = {}, const Sink &sink = {},
                      const std::atomic<bool> *cancel = nullptr);

// The bytes to fetch for `seconds` of a stream at bitrate (bits/s), with
// room for the MP4 header and bitrate swings; max_bytes when unknown.
std::size_t bytes_for(long bitrate, double seconds, std::size_t max_bytes);

// ---- parsing and requests, exposed for the host tests --------------------------
// The visitorData id in a /sw.js_data answer (percent-decoded); "" if absent.
std::string parse_visitor_data(std::string_view body);
// "9:28" / "1:02:03" -> seconds; 0 when it is not a clock.
long parse_clock(std::string_view text);
// The videos in a /youtubei/v1/search answer (every videoRenderer, live
// streams and videos over kMaxSeconds left out). False when it is not JSON.
bool parse_search(std::string_view body, std::vector<Video> &out);
// The best AAC (audio/mp4) stream in a /youtubei/v1/player answer: the
// highest sample rate, then the highest bitrate, as SDH picks. On failure
// error says why (YouTube's own reason when the video is not playable).
bool pick_audio(std::string_view body, AudioFormat &out, std::string *error = nullptr);
// The JSON bodies sent to /search and /player.
std::string search_request(const std::string &term, const std::string &visitor);
std::string player_request(const std::string &video_id, const std::string &visitor);

} // namespace yt
