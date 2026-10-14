// ps5fwdgen - Host tests for the YouTube theme-music client.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parsers run on answers shaped like YouTube's (trimmed from live
// /sw.js_data, /youtubei/v1/search and /youtubei/v1/player answers captured
// on 2026-10-13); the network is a scripted stub, so the request flow, the
// visitor renewal and the download are checked without the internet.

#include "core/json.hpp"
#include "net/http.hpp"
#include "net/youtube.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

const char *kSwData =
    ")]}'\n"
    R"([["yt.sw.adr",null,[[["pt","BR",null,"177.37.153.2",null,null,null,null,null,null,null,)"
    R"("Apple","","CgtHbUVHTlFR%3D%3D",null,1]]]]])";

const char *kSearch = R"({"contents":{"twoColumnSearchResultsRenderer":{"primaryContents":{
 "sectionListRenderer":{"contents":[{"itemSectionRenderer":{"contents":[
  {"videoRenderer":{"videoId":"dh1qqLbuBww",
    "thumbnail":{"thumbnails":[{"url":"https://i.ytimg.com/vi/dh1qqLbuBww/hqdefault.jpg?sqp=x","width":360}]},
    "title":{"runs":[{"text":"Castlevania: Symphony of the Night - "},{"text":"Dracula's Castle"}]},
    "ownerText":{"runs":[{"text":"EiZSeri0us"}]},
    "lengthText":{"accessibility":{},"simpleText":"9:28"}}},
  {"shelfRenderer":{"title":{"simpleText":"People also watched"},"content":{"verticalListRenderer":{"items":[
    {"videoRenderer":{"videoId":"cyWTgLf-piE","title":{"runs":[{"text":"Dracula Castle"}]},
      "longBylineText":{"runs":[{"text":"Castlevania Sound Team - Topic"}]},
      "lengthText":{"simpleText":"1:54"}}}]}}}},
  {"videoRenderer":{"videoId":"live1","title":{"runs":[{"text":"24/7 radio"}]},"ownerText":{"runs":[{"text":"Radio"}]}}},
  {"videoRenderer":{"videoId":"0w7ff_CqK-E","title":{"runs":[{"text":"Full OST"}]},
    "ownerText":{"runs":[{"text":"Someone"}]},"lengthText":{"simpleText":"2:15:41"}}},
  {"channelRenderer":{"channelId":"UC1"}}
 ]}}]}}}}})";

const char *kPlayer = R"({"playabilityStatus":{"status":"OK","playableInEmbed":true},
 "streamingData":{"adaptiveFormats":[
  {"itag":137,"url":"https://rr1.googlevideo.com/videoplayback?itag=137","mimeType":"video/mp4; codecs=\"avc1.640028\"","bitrate":4000000},
  {"itag":251,"url":"https://rr1.googlevideo.com/videoplayback?itag=251","mimeType":"audio/webm; codecs=\"opus\"","bitrate":140000,"audioSampleRate":"48000","contentLength":"3100000"},
  {"itag":139,"url":"https://rr1.googlevideo.com/videoplayback?itag=139","mimeType":"audio/mp4; codecs=\"mp4a.40.5\"","bitrate":49000,"audioSampleRate":"22050","contentLength":"1100000"},
  {"itag":141,"signatureCipher":"s=abc&url=x","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","bitrate":260000,"audioSampleRate":"44100"},
  {"itag":140,"url":"https://rr1.googlevideo.com/videoplayback?itag=140","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","bitrate":130447,"audioSampleRate":"44100","contentLength":"2936448"}
 ]}})";

const char *kBotCheck =
    R"({"playabilityStatus":{"status":"LOGIN_REQUIRED","reason":"Sign in to confirm you're not a bot"}})";

// ---- scripted network ---------------------------------------------------------
std::map<std::string, std::vector<net::Response>> g_routes; // URL -> answers, in order
std::vector<std::string> g_requests;
std::vector<std::string> g_bodies;
// The audio file the fake googlevideo serves, by Range; it can fail on demand.
std::string g_file;
int g_fail_next = 0;    // this many range requests time out first
long g_page_status = 0; // when set, answer every range with a page instead
std::vector<std::pair<std::size_t, std::size_t>> g_ranges; // offset, length asked
std::mutex g_mutex; // get_range runs on several threads at once
int g_in_flight = 0;
int g_max_in_flight = 0;

net::Response text_response(long status, const std::string &text)
{
    net::Response response;
    response.status = status;
    response.body.assign(text.begin(), text.end());
    return response;
}

net::Response route(const std::string &url)
{
    g_requests.push_back(url);
    auto found = g_routes.find(url);
    if (found == g_routes.end() || found->second.empty())
    {
        net::Response refused;
        refused.error = "could not connect";
        return refused;
    }
    net::Response response = found->second.front();
    if (found->second.size() > 1)
        found->second.erase(found->second.begin());
    return response;
}

constexpr const char *kSw = "https://www.youtube.com/sw.js_data";
constexpr const char *kSearchUrl = "https://www.youtube.com/youtubei/v1/search?prettyPrint=false";
constexpr const char *kPlayerUrl = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
constexpr const char *kStream = "https://rr1.googlevideo.com/videoplayback?itag=140";

void test_parsers()
{
    CHECK(yt::parse_visitor_data(kSwData) == "CgtHbUVHTlFR==");
    CHECK(yt::parse_visitor_data("<html>").empty());

    CHECK(yt::parse_clock("9:28") == 568);
    CHECK(yt::parse_clock("2:15:41") == 8141);
    CHECK(yt::parse_clock("0:59") == 59);
    CHECK(yt::parse_clock("") == 0);
    CHECK(yt::parse_clock("LIVE") == 0);

    std::vector<yt::Video> videos;
    CHECK(yt::parse_search(kSearch, videos));
    // The live stream (no length) and the 2-hour OST are left out; the one
    // inside a shelf is found.
    CHECK(videos.size() == 2);
    if (videos.size() == 2)
    {
        CHECK(videos[0].id == "dh1qqLbuBww");
        CHECK(videos[0].title == "Castlevania: Symphony of the Night - Dracula's Castle");
        CHECK(videos[0].author == "EiZSeri0us");
        CHECK(videos[0].seconds == 568);
        CHECK(videos[0].thumbs.size() == 3);
        CHECK(videos[0].thumbs.size() == 3 &&
              videos[0].thumbs[0] == "https://i.ytimg.com/vi/dh1qqLbuBww/hqdefault.jpg" &&
              videos[0].thumbs[1] == "https://i.ytimg.com/vi/dh1qqLbuBww/mqdefault.jpg");
        CHECK(videos[1].id == "cyWTgLf-piE");
        CHECK(videos[1].author == "Castlevania Sound Team - Topic"); // from the byline
        CHECK(videos[1].seconds == 114);
    }
    CHECK(!yt::parse_search("not json", videos));

    yt::AudioFormat format;
    std::string error;
    CHECK(yt::pick_audio(kPlayer, format, &error));
    CHECK(format.url == kStream); // AAC 44.1 kHz with a plain URL; not the ciphered one
    CHECK(format.bitrate == 130447);
    CHECK(format.sample_rate == 44100);
    CHECK(format.length == 2936448);
    CHECK(!yt::pick_audio(kBotCheck, format, &error));
    CHECK(error == "Sign in to confirm you're not a bot");
    CHECK(
        !yt::pick_audio(R"({"playabilityStatus":{"status":"OK"},"streamingData":{"adaptiveFormats":[
        {"url":"u","mimeType":"audio/webm; codecs=\"opus\""}]}})",
                        format, &error));
    CHECK(error == "no AAC audio for this video");
}

void test_requests()
{
    // The bodies are valid JSON with the term, the id and the visitor in place.
    json::Value body;
    CHECK(json::parse(yt::search_request("Halo \"3\" Theme Music", "VD=="), body, nullptr));
    CHECK(body.get("query").as_string() == "Halo \"3\" Theme Music");
    CHECK(body.get("params").as_string() == "EgIQAQ==");
    CHECK(body.get("context").get("client").get("clientName").as_string() == "WEB");
    CHECK(body.get("context").get("client").get("visitorData").as_string() == "VD==");
    CHECK(json::parse(yt::player_request("dh1qqLbuBww", "VD=="), body, nullptr));
    CHECK(body.get("videoId").as_string() == "dh1qqLbuBww");
    CHECK(body.get("context").get("client").get("clientName").as_string() == "VISIONOS");
    CHECK(body.get("context").get("client").get("visitorData").as_string() == "VD==");
}

void test_flow()
{
    g_routes.clear();
    g_requests.clear();
    g_bodies.clear();
    g_routes[kSw] = {text_response(200, kSwData)};
    g_routes[kSearchUrl] = {text_response(200, kSearch)};
    std::vector<yt::Video> videos;
    const yt::Result found = yt::search("Castlevania Theme Music", videos);
    CHECK(found.ok);
    CHECK(videos.size() == 2);
    CHECK(g_requests.size() == 2 && g_requests[0] == kSw); // the visitor id comes first
    CHECK(!g_bodies.empty() && g_bodies.back().find("CgtHbUVHTlFR==") != std::string::npos);

    // The visitor id is kept: the next search goes straight to YouTube.
    g_requests.clear();
    CHECK(yt::search("Halo Theme Music", videos).ok);
    CHECK(g_requests.size() == 1 && g_requests[0] == kSearchUrl);

    // Download: a bot check first, a renewed visitor id, then the stream in
    // Range chunks; one chunk times out once and is asked again.
    g_requests.clear();
    g_routes[kPlayerUrl] = {text_response(200, kBotCheck), text_response(200, kPlayer)};
    g_file.assign(2936448, '\0'); // the stream's contentLength
    g_file.replace(4, 4, "ftyp");
    for (std::size_t i = 8; i < g_file.size(); ++i)
        g_file[i] = static_cast<char>(i * 31);
    g_ranges.clear();
    g_fail_next = 0;
    g_page_status = 0;
    std::vector<unsigned char> audio;
    std::string filename;
    std::vector<std::pair<std::size_t, std::size_t>> reported;
    const yt::Progress progress = [&reported](std::size_t done, std::size_t total)
    { reported.emplace_back(done, total); };
    g_fail_next = 1; // one chunk request stalls once and is asked again
    const yt::Result got =
        yt::download_audio("dh1qqLbuBww", 85.0, 2000000, audio, filename, progress);
    CHECK(got.ok);
    CHECK(filename == "dh1qqLbuBww.m4a");
    CHECK(g_requests.size() >= 3 && g_requests[1] == kSw); // player, sw.js_data, player
    CHECK(g_max_in_flight > 1 && g_max_in_flight <= 4);    // chunks on parallel connections
    // 85 s at 130 kbps, with headroom: ~1.73 MB, under the 2 MB cap.
    const std::size_t want = yt::bytes_for(130447, 85.0, 2000000);
    CHECK(want > 1600000 && want < 2000000);
    CHECK(audio.size() == want);
    CHECK(std::memcmp(audio.data(), g_file.data(), audio.size()) == 0); // in order, no gaps
    const std::size_t chunks = (want + yt::kChunkBytes - 1) / yt::kChunkBytes;
    CHECK(g_ranges.size() == chunks + 1); // one retry
    std::sort(g_ranges.begin(), g_ranges.end());
    CHECK(!g_ranges.empty() && g_ranges.front().first == 0 &&
          g_ranges.front().second == yt::kChunkBytes);
    CHECK(!reported.empty() && reported.front().first == 0 && reported.front().second == want);
    std::size_t most = 0;
    for (const auto &step : reported)
        most = std::max(most, step.first);
    CHECK(most == want); // the workers report in any order; the total is reached
    CHECK(yt::bytes_for(0, 85.0, 2000000) == 2000000);

    // The sink sees the file's bytes in order, whatever order chunks finish in.
    std::vector<unsigned char> streamed;
    std::size_t sink_calls = 0;
    const yt::Sink sink = [&](const unsigned char *data, std::size_t size)
    {
        streamed.insert(streamed.end(), data, data + size);
        ++sink_calls;
    };
    CHECK(yt::download_audio("dh1qqLbuBww", 85.0, 2000000, audio, filename, {}, sink).ok);
    CHECK(streamed == audio);
    CHECK(sink_calls == chunks);

    // Cancelling stops taking chunks and fails the download.
    std::atomic<bool> cancel{false};
    g_ranges.clear();
    const yt::Result stopped = yt::download_audio(
        "dh1qqLbuBww", 85.0, 2000000, audio, filename,
        [&cancel](std::size_t done, std::size_t)
        {
            if (done > 0)
                cancel.store(true);
        },
        {}, &cancel);
    CHECK(!stopped.ok);
    CHECK(stopped.error == "cancelled");
    CHECK(g_ranges.size() < chunks);

    // A short file ends the download early, at its own length.
    g_file.resize(300000);
    g_ranges.clear();
    CHECK(yt::download_audio("dh1qqLbuBww", 85.0, 2000000, audio, filename).ok);
    CHECK(audio.size() == 300000);

    // A chunk that keeps timing out fails the download, saying how far it got.
    g_file.assign(2936448, '\0');
    g_file.replace(4, 4, "ftyp");
    g_fail_next = 100;
    const yt::Result stalled = yt::download_audio("dh1qqLbuBww", 85.0, 2000000, audio, filename);
    CHECK(!stalled.ok);
    CHECK(stalled.error.find("the audio download failed (") == 0);
    CHECK(stalled.detail.find("0 of ") == 0);
    g_fail_next = 0;

    // A refusal YouTube repeats is reported with its reason.
    g_routes[kPlayerUrl] = {text_response(200, kBotCheck)};
    const yt::Result refused = yt::download_audio("dh1qqLbuBww", 85.0, 2000000, audio, filename);
    CHECK(!refused.ok);
    CHECK(refused.error == "YouTube will not play this video: Sign in to confirm you're not a bot");

    // A page instead of audio is refused.
    g_routes[kPlayerUrl] = {text_response(200, kPlayer)};
    g_page_status = 200;
    const yt::Result page = yt::download_audio("dh1qqLbuBww", 85.0, 2000000, audio, filename);
    CHECK(!page.ok);
    CHECK(page.error == "the audio download failed (not audio)");
    g_page_status = 0;

    // A DNS blocklist on the console.
    net::Response unresolved;
    unresolved.error = "Could not resolve host: www.youtube.com";
    g_routes[kSearchUrl] = {unresolved};
    const yt::Result blocked = yt::search("Halo", videos);
    CHECK(!blocked.ok);
    CHECK(blocked.error == "YouTube search failed (the console's DNS does not resolve YouTube; "
                           "check the PS5's DNS settings)");

    // No network.
    g_routes.clear();
    const yt::Result offline = yt::search("Halo", videos);
    CHECK(!offline.ok);
    CHECK(offline.error == "YouTube search failed (could not connect)");
}

} // namespace

namespace net
{
void global_init()
{
}
Response get(const std::string &url, const std::string &, long)
{
    return route(url);
}
Response get_range(const std::string &url, std::size_t offset, std::size_t length, long, long)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_max_in_flight = std::max(g_max_in_flight, ++g_in_flight);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // let the others overlap
    std::unique_lock<std::mutex> lock(g_mutex);
    --g_in_flight;
    g_requests.push_back(url);
    g_ranges.emplace_back(offset, length);
    Response response;
    if (url != kStream)
    {
        response.error = "could not connect";
        return response;
    }
    if (g_fail_next > 0)
    {
        --g_fail_next;
        response.curl_code = 28;
        response.error = "Operation too slow. Less than 1024 bytes/sec transferred the last 20 "
                         "seconds";
        return response;
    }
    if (g_page_status != 0)
        return text_response(g_page_status, "<!doctype html>" + std::string(2000, ' '));
    response.status = 206;
    if (offset < g_file.size())
        response.body.assign(
            g_file.begin() + static_cast<std::ptrdiff_t>(offset),
            g_file.begin() + static_cast<std::ptrdiff_t>(std::min(g_file.size(), offset + length)));
    return response;
}
Response post_json(const std::string &url, const std::string &body, long)
{
    g_bodies.push_back(body);
    return route(url);
}
} // namespace net

int main()
{
    test_parsers();
    test_requests();
    test_flow();
    if (failures != 0)
    {
        std::fprintf(stderr, "test_youtube: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_youtube: all passed\n");
    return 0;
}
