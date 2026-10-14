// ps5fwdgen - Host harness for the theme-music search (YouTube -> AT9).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the app's real client code on the build host, with a system-libcurl
// stand-in for net/http.cpp: search YouTube, download the first
// result's audio, convert it with the forwarder site, cut it to the format's
// length and decode it with the app's ATRAC9 decoder.
//
//   build: see the comment at the end of this file
//   run:   music_host "<game name>" [out.at9]
#include "audio/at9.hpp"
#include "net/at9_convert.hpp"
#include "net/http.hpp"
#include "net/youtube.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace net
{
namespace
{
struct Sink
{
    std::vector<unsigned char> *out;
    std::size_t max;
    bool full;
};
size_t on_data(char *d, size_t s, size_t n, void *u)
{
    auto *sink = static_cast<Sink *>(u);
    const std::size_t total = s * n;
    const std::size_t room = sink->max == 0 ? total : sink->max - sink->out->size();
    const std::size_t take = total < room ? total : room;
    sink->out->insert(sink->out->end(), reinterpret_cast<unsigned char *>(d),
                      reinterpret_cast<unsigned char *>(d) + take);
    if (sink->max != 0 && sink->out->size() >= sink->max)
    {
        sink->full = true;
        return take == total ? total : 0;
    }
    return total;
}
Response perform(CURL *e, Sink &sink)
{
    Response r;
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "ps5fwdgen/1.0");
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &sink);
    const CURLcode c = curl_easy_perform(e);
    if (c == CURLE_OK || (c == CURLE_WRITE_ERROR && sink.full))
        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &r.status);
    else
        r.error = curl_easy_strerror(c);
    r.body = std::move(*sink.out);
    curl_easy_cleanup(e);
    return r;
}
} // namespace

void global_init()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}
Response get(const std::string &url, const std::string &, long)
{
    std::fprintf(stderr, "  GET %s\n", url.c_str());
    std::vector<unsigned char> body;
    Sink sink{&body, 0, false};
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url.c_str());
    return perform(e, sink);
}
Response get_range(const std::string &url, std::size_t offset, std::size_t length, long timeout_ms,
                   long stall_ms)
{
    std::fprintf(stderr, "  GET range %zu+%zu\n", offset, length);
    std::vector<unsigned char> body;
    Sink sink{&body, length, false};
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url.c_str());
    const std::string range = std::to_string(offset) + "-" + std::to_string(offset + length - 1);
    curl_easy_setopt(e, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, (stall_ms + 999) / 1000);
    return perform(e, sink);
}
Response post_json(const std::string &url, const std::string &body, long)
{
    std::fprintf(stderr, "  POST %s (%zu bytes of JSON)\n", url.c_str(), body.size());
    std::vector<unsigned char> out;
    Sink sink{&out, 0, false};
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url.c_str());
    curl_easy_setopt(e, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, headers);
    Response r = perform(e, sink);
    curl_slist_free_all(headers);
    return r;
}
Response post_file(const std::string &url, const std::string &field, const std::string &filename,
                   const std::vector<unsigned char> &data, long)
{
    std::fprintf(stderr, "  POST %s (%s, %zu bytes)\n", url.c_str(), filename.c_str(), data.size());
    std::vector<unsigned char> body;
    Sink sink{&body, 0, false};
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url.c_str());
    curl_mime *mime = curl_mime_init(e);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, field.c_str());
    curl_mime_filename(part, filename.c_str());
    curl_mime_data(part, reinterpret_cast<const char *>(data.data()), data.size());
    curl_easy_setopt(e, CURLOPT_MIMEPOST, mime);
    Response r = perform(e, sink);
    curl_mime_free(mime);
    return r;
}
} // namespace net

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s \"<game name>\" [out.at9]\n", argv[0]);
        return 2;
    }
    net::global_init();
    const std::string game = argv[1];
    const std::string out_path = argc > 2 ? argv[2] : "music_host.at9";

    std::printf("search: %s%s\n", game.c_str(), yt::kDefaultSuffix);
    std::vector<yt::Video> videos;
    const yt::Result found = yt::search(game + yt::kDefaultSuffix, videos);
    if (!found.ok)
    {
        std::printf("search failed: %s\n", found.error.c_str());
        return 1;
    }
    std::printf("%zu videos\n", videos.size());
    for (std::size_t i = 0; i < videos.size() && i < 5; ++i)
        std::printf("  %s  %4lds  %s — %s\n", videos[i].id.c_str(), videos[i].seconds,
                    videos[i].title.c_str(), videos[i].author.c_str());
    if (videos.empty())
        return 1;

    std::vector<unsigned char> audio;
    std::string filename;
    const yt::Result got = yt::download_audio(videos[0].id, fwd::at9::kMaxMusicSeconds,
                                              net::kAt9UploadLimit, audio, filename);
    if (!got.ok)
    {
        std::printf("download failed: %s\n", got.error.c_str());
        return 1;
    }
    std::printf("downloaded %s: %zu bytes\n", filename.c_str(), audio.size());

    std::vector<unsigned char> at9;
    std::string error;
    if (!net::convert_to_at9(audio, filename, at9, error))
    {
        std::printf("convert failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("converted: %zu bytes, %.1f s\n", at9.size(),
                fwd::at9::duration(at9.data(), at9.size()));
    at9 = fwd::at9::trim(at9, fwd::at9::kMaxMusicSeconds);
    const fwd::at9::Decoded clip = fwd::at9::decode(at9.data(), at9.size());
    std::printf("trimmed: %zu bytes, %.1f s; decode %s: %zu frames, %d Hz, %d ch\n", at9.size(),
                fwd::at9::duration(at9.data(), at9.size()), clip.ok() ? "ok" : clip.error.c_str(),
                clip.frames, clip.sample_rate, clip.channels);
    if (std::FILE *f = std::fopen(out_path.c_str(), "wb"))
    {
        std::fwrite(at9.data(), 1, at9.size(), f);
        std::fclose(f);
        std::printf("wrote %s\n", out_path.c_str());
    }
    return clip.ok() ? 0 : 1;
}

// Build (from the repo root; libatrac9 objects as `make test-at9-trim` makes):
//   clang++ -std=c++20 -O2 -Isrc -I<curl include> tools/music_host.cpp src/net/youtube.cpp \
//     src/net/at9_convert.cpp src/core/json.cpp src/audio/at9.cpp build/tests/libatrac9/*.o \
//     -L<curl lib> -lcurl -o build/music_host
