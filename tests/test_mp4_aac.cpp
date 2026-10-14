// ps5fwdgen - Host tests for decoding AAC from a fragmented MP4 as it arrives.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/fixtures/tone440_frag.m4a is laid out like YouTube's audio streams
// (ftyp, moov, sidx, then moof + mdat pairs): 3 s of a 440 Hz tone, AAC-LC,
// 44.1 kHz stereo. tone440_plain.m4a is the same tone as a plain MP4.

#include "audio/mp4_aac.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
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

std::vector<unsigned char> read_file(const char *path)
{
    std::vector<unsigned char> out;
    if (std::FILE *file = std::fopen(path, "rb"))
    {
        unsigned char buffer[65536];
        std::size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
            out.insert(out.end(), buffer, buffer + got);
        std::fclose(file);
    }
    return out;
}

// Feeds the file in pieces of `step` bytes (1 = byte by byte).
bool feed_in_pieces(fwd::aac::Mp4Stream &stream, const std::vector<unsigned char> &file,
                    std::size_t step, std::vector<std::int16_t> &pcm)
{
    for (std::size_t at = 0; at < file.size(); at += step)
        if (!stream.feed(file.data() + at, std::min(step, file.size() - at), pcm))
            return false;
    return true;
}

// The tone's pitch, from the left channel's upward zero crossings.
double pitch(const std::vector<std::int16_t> &pcm, int channels, int rate)
{
    int crossings = 0;
    const std::size_t frames = pcm.size() / static_cast<std::size_t>(channels);
    const std::size_t from = frames / 4; // skip the encoder's fade-in
    const std::size_t to = frames * 3 / 4;
    for (std::size_t i = from + 1; i < to; ++i)
        if (pcm[(i - 1) * channels] < 0 && pcm[i * channels] >= 0)
            ++crossings;
    return crossings * static_cast<double>(rate) / static_cast<double>(to - from);
}

void test_decodes_in_any_pieces()
{
    const std::vector<unsigned char> file = read_file("tests/fixtures/tone440_frag.m4a");
    CHECK(!file.empty());
    std::vector<std::int16_t> whole;
    for (const std::size_t step : {std::size_t{1}, std::size_t{7}, std::size_t{4096}, file.size()})
    {
        fwd::aac::Mp4Stream stream;
        std::vector<std::int16_t> pcm;
        CHECK(feed_in_pieces(stream, file, step, pcm));
        CHECK(stream.error().empty());
        CHECK(stream.sample_rate() == 44100);
        CHECK(stream.channels() == 2);
        CHECK(std::fabs(stream.duration() - 3.0) < 0.1);
        // 3 s of audio, give or take the encoder's priming frames.
        const double seconds = static_cast<double>(pcm.size()) / 2.0 / 44100.0;
        CHECK(seconds > 2.9 && seconds < 3.1);
        CHECK(std::fabs(pitch(pcm, 2, 44100) - 440.0) < 5.0);
        if (whole.empty())
            whole = pcm;
        else
            CHECK(pcm == whole); // the same samples, however the bytes arrive
    }
}

void test_partial_file_decodes_its_start()
{
    // A download still in progress: only the first fragment has arrived.
    std::vector<unsigned char> file = read_file("tests/fixtures/tone440_frag.m4a");
    file.resize(32 + 738 + 76 + 276 + 4000); // ftyp, moov, sidx, moof, half an mdat
    fwd::aac::Mp4Stream stream;
    std::vector<std::int16_t> pcm;
    CHECK(stream.feed(file.data(), file.size(), pcm));
    const double seconds = static_cast<double>(pcm.size()) / 2.0 / 44100.0;
    CHECK(seconds > 0.2 && seconds < 1.0);
}

void test_refuses_what_it_cannot_read()
{
    fwd::aac::Mp4Stream plain;
    std::vector<std::int16_t> pcm;
    const std::vector<unsigned char> file = read_file("tests/fixtures/tone440_plain.m4a");
    CHECK(!file.empty());
    CHECK(!plain.feed(file.data(), file.size(), pcm));
    CHECK(plain.error() == "audio data before its header" ||
          plain.error() == "audio data without a fragment header");

    fwd::aac::Mp4Stream junk;
    const std::string page = "<!doctype html><html><body>not audio</body></html>";
    std::vector<unsigned char> bytes(page.begin(), page.end());
    CHECK(!junk.feed(bytes.data(), bytes.size(), pcm));
    CHECK(!junk.error().empty());
}

} // namespace

int main()
{
    test_decodes_in_any_pieces();
    test_partial_file_decodes_its_start();
    test_refuses_what_it_cannot_read();
    if (failures != 0)
    {
        std::fprintf(stderr, "test_mp4_aac: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_mp4_aac: all passed\n");
    return 0;
}
