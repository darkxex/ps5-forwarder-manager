// ps5fwdgen - Decode a forwarder's selection music (RIFF/ATRAC9) to PCM.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console only ships hardware ATRAC9 decoding behind an undocumented
// ABI, so the edit screen's live preview decodes snd0.at9 in software with
// LibAtrac9 (src/third_party/libatrac9). A whole file is decoded up front;
// selection music is at most 87 s, typically 15 s.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fwd::at9
{

struct Decoded
{
    std::vector<std::int16_t> pcm; // interleaved, `channels` per frame
    int channels = 0;
    int sample_rate = 0;
    std::size_t frames = 0; // per channel
    std::string error;      // "" on success
    bool ok() const
    {
        return error.empty();
    }
};

// data is a RIFF WAVE file whose fmt chunk is WAVE_FORMAT_EXTENSIBLE with the
// ATRAC9 sub-format (what the website's converter and the console write).
Decoded decode(const unsigned char *data, std::size_t size);

// The forwarder format allows "at most about 87 seconds" of selection music
// (external/ps5-forwarder-format/SPEC.md); kept a little under that.
constexpr double kMaxMusicSeconds = 85.0;

// Cuts an ATRAC9 WAVE to at most max_seconds, at a superframe boundary, and
// rewrites the fact sample count, the smpl loop end and the chunk sizes to
// match, so the console loops the shorter clip. Returns the file unchanged
// when it is already short enough or is not an ATRAC9 WAVE.
std::vector<unsigned char> trim(const std::vector<unsigned char> &file, double max_seconds);

// The clip's length in seconds from its fact chunk (0 when unknown).
double duration(const unsigned char *data, std::size_t size);

} // namespace fwd::at9
