// ps5fwdgen - Find a game's theme music on YouTube and fetch its audio.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "net/youtube.hpp"

#include "core/json.hpp"
#include "net/http.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>

namespace yt
{

namespace
{

constexpr const char *kVisitorUrl = "https://www.youtube.com/sw.js_data";
constexpr const char *kSearchUrl = "https://www.youtube.com/youtubei/v1/search?prettyPrint=false";
constexpr const char *kPlayerUrl = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
// The search answers as the website does.
constexpr const char *kWebVersion = "2.20260708.05.00";
// Search filter "Type: Video" (protobuf {2: {2: 1}}, base64).
constexpr const char *kVideosOnly = "EgIQAQ==";
// The player answers as YouTube on Apple Vision Pro: its stream URLs need no
// signature deciphering and no proof-of-origin token (yt-dlp's 'visionos').
constexpr const char *kVisionClient =
    R"("clientName":"VISIONOS","clientVersion":"1.02","deviceMake":"Apple",)"
    R"("deviceModel":"RealityDevice17,1","osName":"visionOS","osVersion":"26.5.23O471",)"
    R"("userAgent":"Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 )"
    R"((KHTML, like Gecko) Version/26.0 Safari/605.1.15")";

std::mutex g_mutex;
std::string g_visitor; // fetched once, renewed when YouTube asks for a sign-in

std::string_view as_text(const std::vector<unsigned char> &body)
{
    return {reinterpret_cast<const char *>(body.data()), body.size()};
}

// InnerTube sends some numbers as strings ("audioSampleRate": "44100").
long number(const json::Value &value)
{
    if (value.is_string())
        return std::strtol(std::string(value.as_string()).c_str(), nullptr, 10);
    return value.as_int();
}

// {"runs":[{"text":"a"},{"text":"b"}]} or {"simpleText":"ab"} -> "ab".
std::string text_of(const json::Value &value)
{
    if (value.get("simpleText").is_string())
        return std::string(value.get("simpleText").as_string());
    std::string out;
    const json::Value &runs = value.get("runs");
    for (std::size_t i = 0; i < runs.size(); ++i)
        out += runs.at(i).get("text").as_string();
    return out;
}

// An MP4 file starts with an "ftyp" box; anything else is not audio.
bool looks_mp4(const std::vector<unsigned char> &body)
{
    return body.size() > 1024 && body[4] == 'f' && body[5] == 't' && body[6] == 'y' &&
           body[7] == 'p';
}

std::string percent_decode(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            out.push_back(static_cast<char>(
                std::strtol(std::string(text.substr(i + 1, 2)).c_str(), nullptr, 16)));
            i += 2;
        }
        else
            out.push_back(text[i]);
    }
    return out;
}

// A visitorData id, fetched once (renew drops the cached one first).
std::string visitor(bool renew)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!renew && !g_visitor.empty())
            return g_visitor;
    }
    const net::Response response = net::get(kVisitorUrl, {}, 15000);
    std::string id = response.status == 200 ? parse_visitor_data(as_text(response.body)) : "";
    std::lock_guard<std::mutex> lock(g_mutex);
    g_visitor = id;
    return id;
}

std::string transport_error(const net::Response &response)
{
    // The console's DNS gave no address (or a blocklist's 0.0.0.0).
    if (response.status == 0 && response.error.find("resolve host") != std::string::npos)
        return "the console's DNS does not resolve YouTube; check the PS5's DNS settings";
    if (response.status == 0)
        return response.error.empty() ? "network error" : response.error;
    return "HTTP " + std::to_string(response.status);
}

void collect_videos(const json::Value &node, std::vector<Video> &out)
{
    if (node.is_array())
    {
        for (const json::Value &item : node.items())
            collect_videos(item, out);
        return;
    }
    if (!node.is_object())
        return;
    if (const json::Value *renderer = node.find("videoRenderer"))
    {
        Video video;
        video.id = std::string(renderer->get("videoId").as_string());
        video.title = text_of(renderer->get("title"));
        video.author = text_of(renderer->get("ownerText"));
        if (video.author.empty())
            video.author = text_of(renderer->get("longBylineText"));
        video.seconds = parse_clock(text_of(renderer->get("lengthText")));
        // No length: a live stream or an upcoming premiere, nothing to fetch.
        if (video.id.empty() || video.seconds <= 0 || video.seconds > kMaxSeconds)
            return;
        // 480x360 (16:9 picture letterboxed in 4:3), then 320x180.
        video.thumbs.push_back("https://i.ytimg.com/vi/" + video.id + "/hqdefault.jpg");
        video.thumbs.push_back("https://i.ytimg.com/vi/" + video.id + "/mqdefault.jpg");
        const json::Value &thumbs = renderer->get("thumbnail").get("thumbnails");
        if (thumbs.size() > 0)
        {
            std::string url(thumbs.at(0).get("url").as_string());
            if (url.rfind("//", 0) == 0)
                url = "https:" + url;
            if (url.rfind("https://", 0) == 0 &&
                std::find(video.thumbs.begin(), video.thumbs.end(), url) == video.thumbs.end())
                video.thumbs.push_back(std::move(url));
        }
        out.push_back(std::move(video));
        return;
    }
    for (const auto &member : node.members())
        collect_videos(member.second, out);
}

// Connections used at once to fetch audio chunks.
constexpr std::size_t kParallel = 4;

// A chunk at offset arrived: 206, or 200 when the server ignored the Range
// on the first chunk (the body is then the file from its start).
bool chunk_ok(const net::Response &chunk, std::size_t offset)
{
    return chunk.status == 206 || (chunk.status == 200 && offset == 0);
}

// The shared state of one chunked download.
struct Fetch
{
    std::string url;
    std::size_t total = 0;
    std::vector<net::Response> chunks; // index-aligned to the chunk number
    std::atomic<std::size_t> next{0};  // the next chunk to take
    std::atomic<std::size_t> done{0};  // bytes received so far
    std::atomic<bool> failed{false};   // a chunk failed for good: stop taking more
    const Progress *progress = nullptr;
    const Sink *sink = nullptr; // gets the bytes in file order
    const std::atomic<bool> *cancel = nullptr;
    std::mutex progress_mutex; // serializes progress and sink calls
    std::vector<char> arrived; // index-aligned to chunks
    std::size_t flushed = 0;   // chunks handed to the sink
    bool sink_done = false;    // the sink saw a failed or short chunk
};

bool cancelled(const Fetch &fetch)
{
    return fetch.cancel != nullptr && fetch.cancel->load();
}

// Hands every chunk that is next in order, and has arrived, to the sink.
// Called with progress_mutex held.
void flush_in_order(Fetch &fetch)
{
    while (!fetch.sink_done && fetch.flushed < fetch.chunks.size() &&
           fetch.arrived[fetch.flushed] != 0)
    {
        const std::size_t i = fetch.flushed++;
        const net::Response &chunk = fetch.chunks[i];
        const std::size_t offset = i * kChunkBytes;
        if (!chunk_ok(chunk, offset))
        {
            fetch.sink_done = true;
            break;
        }
        if (!chunk.body.empty())
            (*fetch.sink)(chunk.body.data(), chunk.body.size());
        if (chunk.body.size() < std::min(kChunkBytes, fetch.total - offset))
            fetch.sink_done = true; // the end of the file
    }
}

void *fetch_worker(void *arg)
{
    auto *fetch = static_cast<Fetch *>(arg);
    for (;;)
    {
        const std::size_t i = fetch->next.fetch_add(1);
        if (i >= fetch->chunks.size() || fetch->failed.load() || cancelled(*fetch))
            return nullptr;
        const std::size_t offset = i * kChunkBytes;
        const std::size_t want = std::min(kChunkBytes, fetch->total - offset);
        net::Response chunk;
        for (int attempt = 0; attempt < 3 && !cancelled(*fetch); ++attempt)
        {
            // A chunk that fails is asked again.
            chunk = net::get_range(fetch->url, offset, want);
            if (chunk_ok(chunk, offset))
                break;
        }
        if (!chunk_ok(chunk, offset))
            fetch->failed.store(true);
        const std::size_t done = fetch->done.fetch_add(chunk.body.size()) + chunk.body.size();
        std::lock_guard<std::mutex> lock(fetch->progress_mutex);
        fetch->chunks[i] = std::move(chunk);
        fetch->arrived[i] = 1;
        if (fetch->sink != nullptr && *fetch->sink)
            flush_in_order(*fetch);
        if (fetch->progress != nullptr && *fetch->progress)
            (*fetch->progress)(std::min(done, fetch->total), fetch->total);
    }
}

void run_fetch(Fetch &fetch)
{
    std::vector<pthread_t> threads;
    const std::size_t count = std::min(kParallel, fetch.chunks.size());
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512u << 10);
    for (std::size_t i = 1; i < count; ++i)
    {
        pthread_t thread;
        if (pthread_create(&thread, &attr, fetch_worker, &fetch) == 0)
            threads.push_back(thread);
    }
    pthread_attr_destroy(&attr);
    fetch_worker(&fetch); // this thread works too
    for (const pthread_t thread : threads)
        pthread_join(thread, nullptr);
}

} // namespace

std::string parse_visitor_data(std::string_view body)
{
    // ")]}'\n[["yt.sw.adr",null,[[["pt","BR",...,"<visitorData>",...]]]]]": the
    // id sits at [0][2][0][0][13], as YouTube's own service worker reads it.
    const std::size_t start = body.find('[');
    if (start == std::string_view::npos)
        return {};
    json::Value root;
    if (!json::parse(body.substr(start), root, nullptr))
        return {};
    const json::Value &id = root.at(0).at(2).at(0).at(0).at(13);
    return id.is_string() ? percent_decode(id.as_string()) : std::string();
}

long parse_clock(std::string_view text)
{
    long total = 0;
    long part = 0;
    bool digits = false;
    for (const char c : text)
    {
        if (c >= '0' && c <= '9')
        {
            part = part * 10 + (c - '0');
            digits = true;
        }
        else if (c == ':' && digits)
        {
            total = total * 60 + part;
            part = 0;
            digits = false;
        }
        else
            return 0;
    }
    return digits ? total * 60 + part : 0;
}

bool parse_search(std::string_view body, std::vector<Video> &out)
{
    out.clear();
    json::Value root;
    if (!json::parse(body, root, nullptr) || !root.is_object())
        return false;
    collect_videos(root, out);
    return true;
}

bool pick_audio(std::string_view body, AudioFormat &out, std::string *error)
{
    json::Value root;
    if (!json::parse(body, root, nullptr) || !root.is_object())
    {
        if (error != nullptr)
            *error = "unexpected answer from YouTube";
        return false;
    }
    const json::Value &status = root.get("playabilityStatus");
    const std::string_view state = status.get("status").as_string();
    if (state != "OK")
    {
        if (error != nullptr)
        {
            const std::string reason(status.get("reason").as_string());
            *error = reason.empty() ? std::string(state) : reason;
        }
        return false;
    }
    const json::Value &formats = root.get("streamingData").get("adaptiveFormats");
    bool found = false;
    bool any_audio = false;
    for (std::size_t i = 0; i < formats.size(); ++i)
    {
        const json::Value &format = formats.at(i);
        const std::string_view type = format.get("mimeType").as_string();
        if (type.rfind("audio/", 0) != 0)
            continue;
        any_audio = true;
        if (type.rfind("audio/mp4", 0) != 0)
            continue; // WebM/Opus: the converter cannot read it
        AudioFormat candidate;
        candidate.url = std::string(format.get("url").as_string());
        if (candidate.url.empty())
            continue; // a ciphered URL needs YouTube's JavaScript
        candidate.mime = std::string(type);
        candidate.bitrate = number(format.get("bitrate"));
        candidate.sample_rate = number(format.get("audioSampleRate"));
        candidate.length = number(format.get("contentLength"));
        const bool better =
            !found || candidate.sample_rate > out.sample_rate ||
            (candidate.sample_rate == out.sample_rate && candidate.bitrate > out.bitrate);
        if (better)
        {
            out = std::move(candidate);
            found = true;
        }
    }
    if (!found && error != nullptr)
        *error = any_audio ? "no AAC audio for this video" : "no audio for this video";
    return found;
}

std::string search_request(const std::string &term, const std::string &visitor_id)
{
    return std::string(R"({"context":{"client":{"clientName":"WEB","clientVersion":")") +
           kWebVersion + R"(","hl":"en","gl":"US","visitorData":)" + json::quote(visitor_id) +
           R"(}},"query":)" + json::quote(term) + R"(,"params":")" + kVideosOnly + R"("})";
}

std::string player_request(const std::string &video_id, const std::string &visitor_id)
{
    return std::string(R"({"context":{"client":{)") + kVisionClient +
           R"(,"hl":"en","gl":"US","visitorData":)" + json::quote(visitor_id) + R"(}},"videoId":)" +
           json::quote(video_id) + R"(,"contentCheckOk":true,"racyCheckOk":true})";
}

std::size_t bytes_for(long bitrate, double seconds, std::size_t max_bytes)
{
    if (bitrate <= 0 || seconds <= 0.0)
        return max_bytes;
    const double want = static_cast<double>(bitrate) / 8.0 * seconds * 1.2 + 64.0 * 1024.0;
    return want < static_cast<double>(max_bytes) ? static_cast<std::size_t>(want) : max_bytes;
}

Result search(const std::string &term, std::vector<Video> &out)
{
    out.clear();
    const net::Response response = net::post_json(kSearchUrl, search_request(term, visitor(false)));
    if (response.status != 200)
        return {false, "YouTube search failed (" + transport_error(response) + ")", {}};
    if (!parse_search(as_text(response.body), out))
        return {false, "unexpected answer from YouTube", {}};
    return {true, {}, {}};
}

Result download_audio(const std::string &video_id, double seconds, std::size_t max_bytes,
                      std::vector<unsigned char> &audio, std::string &filename,
                      const Progress &progress, const Sink &sink, const std::atomic<bool> *cancel)
{
    audio.clear();
    AudioFormat format;
    std::string error;
    bool picked = false;
    // A sign-in request usually means a stale visitor id: renew it once.
    for (int attempt = 0; attempt < 2 && !picked; ++attempt)
    {
        const net::Response info =
            net::post_json(kPlayerUrl, player_request(video_id, visitor(attempt > 0)));
        if (info.status != 200)
            return {false, "YouTube did not answer (" + transport_error(info) + ")", {}};
        picked = pick_audio(as_text(info.body), format, &error);
        if (!picked && error.find("Sign in") == std::string::npos)
            break;
    }
    if (!picked)
        return {false, "YouTube will not play this video: " + error, error};

    // Only as much as the clip needs, and never past the end of the file.
    std::size_t total = bytes_for(format.bitrate, seconds, max_bytes);
    if (format.length > 0 && static_cast<std::size_t>(format.length) < total)
        total = static_cast<std::size_t>(format.length);
    if (progress)
        progress(0, total);

    // The chunks, fetched on kParallel connections at once: YouTube may hold
    // each connection to about playback speed, so one at a time could take
    // longer than the clip lasts.
    Fetch fetch;
    fetch.url = format.url;
    fetch.total = total;
    fetch.chunks.resize((total + kChunkBytes - 1) / kChunkBytes);
    fetch.progress = &progress;
    fetch.sink = &sink;
    fetch.cancel = cancel;
    fetch.arrived.assign(fetch.chunks.size(), 0);
    run_fetch(fetch);
    if (cancelled(fetch))
        return {false, "cancelled", {}};

    // Put them together in order; the first chunk that failed is the error.
    for (std::size_t i = 0; i < fetch.chunks.size(); ++i)
    {
        const net::Response &chunk = fetch.chunks[i];
        const std::size_t offset = i * kChunkBytes;
        const std::size_t want = std::min(kChunkBytes, total - offset);
        std::string why;
        if (!chunk_ok(chunk, offset))
            why =
                chunk.status == 0 ? transport_error(chunk) : "HTTP " + std::to_string(chunk.status);
        else if (offset == 0 && !looks_mp4(chunk.body))
            why = "not audio";
        if (!why.empty())
            return {false, "the audio download failed (" + why + ")",
                    std::to_string(offset) + " of " + std::to_string(total) + " bytes"};
        audio.insert(audio.end(), chunk.body.begin(), chunk.body.end());
        if (chunk.body.size() < want)
            break; // the end of the file
    }
    if (audio.size() > total)
        audio.resize(total);
    filename = video_id + ".m4a";
    return {true, {}, {}};
}

} // namespace yt
