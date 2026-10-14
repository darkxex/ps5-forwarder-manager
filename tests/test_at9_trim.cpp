// ps5fwdgen - Host tests for cutting selection music to the format's length.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The input mirrors the chunks the forwarder site's converter writes (fmt
// WAVE_FORMAT_EXTENSIBLE/ATRAC9 with 384-byte superframes of 1024 samples at
// 48 kHz, fact with a 256-sample encoder delay, smpl with one loop, data).

#include "audio/at9.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

void put16(std::vector<unsigned char> &out, std::uint32_t v)
{
    out.push_back(static_cast<unsigned char>(v));
    out.push_back(static_cast<unsigned char>(v >> 8));
}
void put32(std::vector<unsigned char> &out, std::uint32_t v)
{
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}
std::uint32_t get32(const std::vector<unsigned char> &in, std::size_t at)
{
    return static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1]) << 8) |
           (static_cast<std::uint32_t>(in[at + 2]) << 16) |
           (static_cast<std::uint32_t>(in[at + 3]) << 24);
}
std::size_t find(const std::vector<unsigned char> &in, const char *id)
{
    for (std::size_t i = 12; i + 4 <= in.size(); ++i)
        if (std::memcmp(in.data() + i, id, 4) == 0)
            return i;
    return 0;
}

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kDelay = 256;
constexpr std::uint32_t kBlock = 384;

std::vector<unsigned char> make_at9(std::uint32_t samples)
{
    const std::uint32_t blocks = (samples + kDelay + 1023) / 1024;
    std::vector<unsigned char> f;
    f.insert(f.end(), {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'});
    f.insert(f.end(), {'f', 'm', 't', ' '});
    put32(f, 52);
    put16(f, 0xfffe);
    put16(f, 2);
    put32(f, kRate);
    put32(f, 18000);
    put16(f, kBlock);
    put16(f, 0);
    put16(f, 34);
    put16(f, 1024);
    put32(f, 3);
    const unsigned char guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                    0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
    f.insert(f.end(), guid, guid + 16);
    put32(f, 1);
    f.insert(f.end(), {0xfe, 0x74, 0x0b, 0xf0, 0, 0, 0, 0});
    f.insert(f.end(), {'f', 'a', 'c', 't'});
    put32(f, 12);
    put32(f, samples);
    put32(f, 256);
    put32(f, kDelay);
    f.insert(f.end(), {'s', 'm', 'p', 'l'});
    put32(f, 60);
    for (int i = 0; i < 7; ++i)
        put32(f, 0);
    put32(f, 1);  // one loop
    put32(f, 24); // sampler data
    put32(f, 0);
    put32(f, 0);
    put32(f, kDelay);
    put32(f, samples + kDelay - 1);
    put32(f, 0);
    put32(f, 0);
    f.insert(f.end(), {'d', 'a', 't', 'a'});
    put32(f, blocks * kBlock);
    for (std::uint32_t i = 0; i < blocks * kBlock; ++i)
        f.push_back(static_cast<unsigned char>(i * 7));
    const std::uint32_t riff = static_cast<std::uint32_t>(f.size() - 8);
    std::memcpy(f.data() + 4, &riff, 4);
    return f;
}

void test_long_clip_is_cut()
{
    const std::vector<unsigned char> in = make_at9(128 * kRate); // two minutes and change
    CHECK(fwd::at9::duration(in.data(), in.size()) > 127.9);
    const std::vector<unsigned char> out = fwd::at9::trim(in, 85.0);
    CHECK(out.size() < in.size());
    CHECK(get32(out, 4) == out.size() - 8);
    const std::size_t fact = find(out, "fact");
    CHECK(fact != 0 && get32(out, fact + 8) == 85 * kRate);
    const std::size_t smpl = find(out, "smpl");
    CHECK(smpl != 0 && get32(out, smpl + 8 + 36 + 8) == kDelay);
    CHECK(smpl != 0 && get32(out, smpl + 8 + 36 + 12) == 85 * kRate + kDelay - 1);
    const std::size_t data = find(out, "data");
    const std::uint32_t data_size = get32(out, data + 4);
    CHECK(data_size % kBlock == 0);
    CHECK(data_size / kBlock == (85 * kRate + kDelay + 1023) / 1024);
    CHECK(data + 8 + data_size == out.size());
    // The kept data is the start of the original, byte for byte.
    CHECK(std::memcmp(out.data() + data + 8, in.data() + data + 8, data_size) == 0);
    CHECK(fwd::at9::duration(out.data(), out.size()) == 85.0);
}

void test_short_clip_is_kept()
{
    const std::vector<unsigned char> in = make_at9(30 * kRate);
    CHECK(fwd::at9::trim(in, 85.0) == in);
}

void test_other_files_are_kept()
{
    const std::vector<unsigned char> junk = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'};
    CHECK(fwd::at9::trim(junk, 85.0) == junk);
    const std::vector<unsigned char> empty;
    CHECK(fwd::at9::trim(empty, 85.0).empty());
    CHECK(fwd::at9::duration(junk.data(), junk.size()) == 0.0);
}

// A real file (the app's own selection music): it decodes, and so does a cut
// of it. Under ASan this also guards the decoder against reading past the
// last superframe, which ends the file buffer.
void test_real_file_decodes()
{
    std::vector<unsigned char> file;
    if (std::FILE *f = std::fopen("sce_sys/snd0.at9", "rb"))
    {
        unsigned char buffer[65536];
        std::size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
            file.insert(file.end(), buffer, buffer + got);
        std::fclose(f);
    }
    CHECK(!file.empty());
    const fwd::at9::Decoded whole = fwd::at9::decode(file.data(), file.size());
    CHECK(whole.ok());
    CHECK(whole.sample_rate == 48000);
    const double seconds = fwd::at9::duration(file.data(), file.size());
    CHECK(seconds > 2.0);
    const std::vector<unsigned char> cut = fwd::at9::trim(file, seconds / 2.0);
    CHECK(cut.size() < file.size());
    const fwd::at9::Decoded half = fwd::at9::decode(cut.data(), cut.size());
    CHECK(half.ok());
    CHECK(half.frames > 0 && half.frames < whole.frames);
}

} // namespace

int main()
{
    test_long_clip_is_cut();
    test_short_clip_is_kept();
    test_other_files_are_kept();
    test_real_file_decodes();
    if (failures != 0)
    {
        std::fprintf(stderr, "test_at9_trim: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_at9_trim: all passed\n");
    return 0;
}
