// ps5fwdgen - Decode AAC audio from a fragmented MP4 as its bytes arrive.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// YouTube's audio-only AAC streams (itag 140 and friends) are fragmented MP4:
// ftyp, moov (the decoder config in stsd/mp4a/esds), sidx, then moof + mdat
// pairs. This reads them incrementally, so playback can start while the rest
// of the file is still downloading: feed() takes the next bytes of the file,
// and every AAC frame whose bytes are complete is decoded at once (FAAD2,
// from PacBrew). Plain (non-fragmented) MP4 is not supported.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fwd::aac
{

class Mp4Stream
{
  public:
    Mp4Stream() = default;
    ~Mp4Stream();
    Mp4Stream(const Mp4Stream &) = delete;
    Mp4Stream &operator=(const Mp4Stream &) = delete;

    // The next bytes of the file, in order. Decoded audio is appended to pcm
    // (interleaved signed 16-bit, channels() per frame). False once the file
    // turns out to be unusable; error() then says why.
    bool feed(const unsigned char *data, std::size_t size, std::vector<std::int16_t> &pcm);

    // Known after the first decoded frame (0 before).
    int sample_rate() const
    {
        return sample_rate_;
    }
    int channels() const
    {
        return channels_;
    }
    const std::string &error() const
    {
        return error_;
    }
    // The track's length from its media header, in seconds (0 if unknown).
    double duration() const
    {
        return duration_;
    }

  private:
    bool parse(std::vector<std::int16_t> &pcm);
    bool read_moov(const unsigned char *box, std::size_t size);
    bool read_moof(const unsigned char *box, std::size_t size, std::uint64_t at);
    void read_sidx(const unsigned char *box, std::size_t size);
    bool decode(const unsigned char *frame, std::size_t size, std::vector<std::int16_t> &pcm);
    bool fail(std::string why);

    std::vector<unsigned char> buffer_; // bytes not yet consumed
    std::uint64_t base_ = 0;            // file offset of buffer_[0]
    std::uint64_t skip_ = 0;            // bytes still to drop (a box being skipped)
    void *decoder_ = nullptr;           // NeAACDecHandle
    std::vector<unsigned char> config_; // AudioSpecificConfig from esds
    int sample_rate_ = 0;
    int channels_ = 0;
    double duration_ = 0.0;
    std::uint32_t timescale_ = 0;
    std::string error_;

    // The fragment being read: sizes of its samples, where they start in the
    // file, and how far through them decoding is.
    std::vector<std::uint32_t> samples_;
    std::size_t next_sample_ = 0;
    std::uint64_t data_at_ = 0;      // file offset of the next sample's first byte
    std::uint64_t mdat_end_ = 0;     // file offset just past the current mdat (0: none)
    std::uint32_t default_size_ = 0; // tfhd default_sample_size
};

} // namespace fwd::aac
