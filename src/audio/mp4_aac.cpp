// ps5fwdgen - Decode AAC audio from a fragmented MP4 as its bytes arrive.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/mp4_aac.hpp"

#include <neaacdec.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace fwd::aac
{

namespace
{

std::uint32_t be32(const unsigned char *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t be64(const unsigned char *p)
{
    return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

bool is(const unsigned char *type, const char *name)
{
    return std::memcmp(type, name, 4) == 0;
}

// A box inside a parent: its type, payload and the size of its header.
struct Box
{
    const unsigned char *type = nullptr;
    const unsigned char *body = nullptr;
    std::size_t body_size = 0;
};

// Walks the boxes in [data, data + size); false at the end or on a bad size.
bool next_box(const unsigned char *&data, std::size_t &size, Box &box)
{
    if (size < 8)
        return false;
    std::uint64_t length = be32(data);
    std::size_t header = 8;
    if (length == 1)
    {
        if (size < 16)
            return false;
        length = be64(data + 8);
        header = 16;
    }
    else if (length == 0)
    {
        length = size;
    }
    if (length < header || length > size)
        return false;
    box.type = data + 4;
    box.body = data + header;
    box.body_size = static_cast<std::size_t>(length) - header;
    data += length;
    size -= static_cast<std::size_t>(length);
    return true;
}

// The child box of the given type, searched among [data, data + size).
bool find_child(const unsigned char *data, std::size_t size, const char *name, Box &out)
{
    Box box;
    while (next_box(data, size, box))
    {
        if (is(box.type, name))
        {
            out = box;
            return true;
        }
    }
    return false;
}

// An MPEG-4 descriptor length: up to four bytes of seven bits each.
bool descriptor_length(const unsigned char *&p, const unsigned char *end, std::size_t &length)
{
    length = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (p >= end)
            return false;
        const unsigned char byte = *p++;
        length = (length << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0)
            return true;
    }
    return true;
}

// The AudioSpecificConfig inside an esds box's ES_Descriptor.
bool esds_config(const unsigned char *body, std::size_t size, std::vector<unsigned char> &out)
{
    const unsigned char *p = body + 4; // version and flags
    const unsigned char *end = body + size;
    if (p >= end || *p++ != 0x03) // ES_Descriptor
        return false;
    std::size_t length = 0;
    if (!descriptor_length(p, end, length) || end - p < 3)
        return false;
    p += 2; // ES_ID
    const unsigned char flags = *p++;
    if (flags & 0x80)
        p += 2; // dependsOn_ES_ID
    if (flags & 0x40)
    {
        if (p >= end)
            return false;
        p += 1 + *p; // URL
    }
    if (flags & 0x20)
        p += 2;                   // OCR_ES_Id
    if (p >= end || *p++ != 0x04) // DecoderConfigDescriptor
        return false;
    if (!descriptor_length(p, end, length) || end - p < 13)
        return false;
    p += 13;                      // objectType, streamType, bufferSize, maxBitrate, avgBitrate
    if (p >= end || *p++ != 0x05) // DecoderSpecificInfo
        return false;
    if (!descriptor_length(p, end, length) || static_cast<std::size_t>(end - p) < length ||
        length == 0)
        return false;
    out.assign(p, p + length);
    return true;
}

} // namespace

Mp4Stream::~Mp4Stream()
{
    if (decoder_ != nullptr)
        NeAACDecClose(static_cast<NeAACDecHandle>(decoder_));
}

bool Mp4Stream::fail(std::string why)
{
    error_ = std::move(why);
    return false;
}

bool Mp4Stream::feed(const unsigned char *data, std::size_t size, std::vector<std::int16_t> &pcm)
{
    if (!error_.empty())
        return false;
    buffer_.insert(buffer_.end(), data, data + size);
    const bool ok = parse(pcm);
    return ok && error_.empty();
}

bool Mp4Stream::parse(std::vector<std::int16_t> &pcm)
{
    std::size_t used = 0; // bytes of buffer_ consumed in this call
    for (;;)
    {
        const std::uint64_t at = base_ + used;
        const std::size_t have = buffer_.size() - used;
        if (skip_ > 0)
        {
            const std::size_t drop = static_cast<std::size_t>(std::min<std::uint64_t>(skip_, have));
            used += drop;
            skip_ -= drop;
            if (skip_ > 0)
                break;
            continue;
        }
        // Inside an mdat: decode each sample once all its bytes are here.
        if (mdat_end_ != 0)
        {
            if (at >= mdat_end_ || next_sample_ >= samples_.size())
            {
                // Past the samples this fragment described: drop the rest of
                // the mdat and wait for the next moof.
                const std::uint64_t rest = mdat_end_ > at ? mdat_end_ - at : 0;
                mdat_end_ = 0;
                skip_ = rest;
                continue;
            }
            if (at < data_at_)
            {
                skip_ = data_at_ - at; // padding before the first sample
                continue;
            }
            const std::uint32_t sample = samples_[next_sample_];
            if (have < sample)
                break;
            if (!decode(buffer_.data() + used, sample, pcm))
                return false;
            used += sample;
            data_at_ += sample;
            ++next_sample_;
            continue;
        }
        // A box header.
        if (have < 8)
            break;
        const unsigned char *head = buffer_.data() + used;
        std::uint64_t length = be32(head);
        std::size_t header = 8;
        if (length == 1)
        {
            if (have < 16)
                break;
            length = be64(head + 8);
            header = 16;
        }
        if (length < header)
            return fail("bad MP4 box");
        const unsigned char *type = head + 4;
        if (at == 0 && !is(type, "ftyp") && !is(type, "styp") && !is(type, "moov"))
            return fail("not an MP4 file");
        if (is(type, "sidx") && duration_ == 0.0)
        {
            if (length > 1u << 20)
                return fail("MP4 index too large");
            if (have < length)
                break;
            read_sidx(head + header, static_cast<std::size_t>(length) - header);
            used += static_cast<std::size_t>(length);
            continue;
        }
        if (is(type, "mdat"))
        {
            if (samples_.empty())
                return fail(decoder_ == nullptr ? "audio data before its header"
                                                : "audio data without a fragment header");
            used += header;
            mdat_end_ = at + length;
            if (data_at_ == 0)
                data_at_ = at + header;
            continue;
        }
        if (is(type, "moov") || is(type, "moof"))
        {
            if (length > 16u << 20)
                return fail("MP4 header too large");
            if (have < length)
                break; // wait for the whole box
            const bool ok =
                is(type, "moov")
                    ? read_moov(head + header, static_cast<std::size_t>(length) - header)
                    : read_moof(head + header, static_cast<std::size_t>(length) - header, at);
            if (!ok)
                return false;
            used += static_cast<std::size_t>(length);
            continue;
        }
        // ftyp, sidx, styp, free...: not needed.
        skip_ = length;
    }
    if (used > 0)
    {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(used));
        base_ += used;
    }
    return true;
}

bool Mp4Stream::read_moov(const unsigned char *body, std::size_t size)
{
    // moov > trak > mdia > (mdhd, minf > stbl > stsd > mp4a > esds). The
    // first track with an mp4a entry is the audio.
    const unsigned char *p = body;
    std::size_t left = size;
    Box trak;
    while (next_box(p, left, trak))
    {
        if (!is(trak.type, "trak"))
            continue;
        Box mdia, minf, stbl, stsd, mdhd;
        if (!find_child(trak.body, trak.body_size, "mdia", mdia) ||
            !find_child(mdia.body, mdia.body_size, "minf", minf) ||
            !find_child(minf.body, minf.body_size, "stbl", stbl) ||
            !find_child(stbl.body, stbl.body_size, "stsd", stsd) || stsd.body_size < 8)
            continue;
        // stsd: version/flags, entry count, then sample entries.
        const unsigned char *entries = stsd.body + 8;
        std::size_t entries_size = stsd.body_size - 8;
        Box entry;
        if (!next_box(entries, entries_size, entry) || !is(entry.type, "mp4a") ||
            entry.body_size < 28)
            continue;
        Box esds;
        if (!find_child(entry.body + 28, entry.body_size - 28, "esds", esds) ||
            !esds_config(esds.body, esds.body_size, config_))
            return fail("no AAC decoder config");
        if (find_child(mdia.body, mdia.body_size, "mdhd", mdhd) && mdhd.body_size >= 24)
        {
            const bool v1 = mdhd.body[0] == 1;
            timescale_ = be32(mdhd.body + (v1 ? 20 : 12));
            const std::uint64_t length = v1 ? be64(mdhd.body + 24) : be32(mdhd.body + 16);
            if (timescale_ > 0)
                duration_ = static_cast<double>(length) / timescale_;
        }
        NeAACDecHandle handle = NeAACDecOpen();
        if (handle == nullptr)
            return fail("AAC decoder unavailable");
        NeAACDecConfigurationPtr settings = NeAACDecGetCurrentConfiguration(handle);
        settings->outputFormat = FAAD_FMT_16BIT;
        settings->downMatrix = 1; // more than two channels: down to stereo
        NeAACDecSetConfiguration(handle, settings);
        unsigned long rate = 0;
        unsigned char channels = 0;
        if (NeAACDecInit2(handle, config_.data(), static_cast<unsigned long>(config_.size()), &rate,
                          &channels) < 0)
        {
            NeAACDecClose(handle);
            return fail("AAC decoder config refused");
        }
        decoder_ = handle;
        return true;
    }
    return fail("no AAC track in the file");
}

void Mp4Stream::read_sidx(const unsigned char *body, std::size_t size)
{
    // The index of the fragments: their durations add up to the track's
    // length, for files whose header leaves it out.
    if (size < 12)
        return;
    const bool v1 = body[0] == 1;
    const std::uint32_t timescale = be32(body + 8);
    std::size_t p = 12 + (v1 ? 16 : 8) + 2; // earliest time, first offset, reserved
    if (timescale == 0 || size < p + 2)
        return;
    const std::uint32_t count = static_cast<std::uint32_t>(body[p] << 8 | body[p + 1]);
    p += 2;
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < count && size >= p + 12; ++i, p += 12)
        total += be32(body + p + 4);
    duration_ = static_cast<double>(total) / timescale;
}

bool Mp4Stream::read_moof(const unsigned char *body, std::size_t size, std::uint64_t at)
{
    if (decoder_ == nullptr)
        return fail("fragment before the file header");
    samples_.clear();
    next_sample_ = 0;
    data_at_ = 0;
    Box traf;
    if (!find_child(body, size, "traf", traf))
        return fail("fragment without a track");
    std::uint64_t base = at; // default-base-is-moof
    Box tfhd;
    if (find_child(traf.body, traf.body_size, "tfhd", tfhd) && tfhd.body_size >= 8)
    {
        const std::uint32_t flags = be32(tfhd.body) & 0xffffff;
        std::size_t p = 8; // version/flags, track_ID
        if (flags & 0x01)
        {
            if (tfhd.body_size >= p + 8)
                base = be64(tfhd.body + p);
            p += 8;
        }
        if (flags & 0x02)
            p += 4; // sample_description_index
        if (flags & 0x08)
            p += 4; // default_sample_duration
        if ((flags & 0x10) && tfhd.body_size >= p + 4)
            default_size_ = be32(tfhd.body + p);
    }
    Box trun;
    if (!find_child(traf.body, traf.body_size, "trun", trun) || trun.body_size < 8)
        return fail("fragment without samples");
    const std::uint32_t flags = be32(trun.body) & 0xffffff;
    const std::uint32_t count = be32(trun.body + 4);
    std::size_t p = 8;
    if (flags & 0x01)
    {
        if (trun.body_size < p + 4)
            return fail("bad fragment");
        data_at_ = base + static_cast<std::int32_t>(be32(trun.body + p));
        p += 4;
    }
    if (flags & 0x04)
        p += 4; // first_sample_flags
    std::size_t per_sample = 0;
    for (const std::uint32_t bit : {0x100u, 0x200u, 0x400u, 0x800u})
        if (flags & bit)
            per_sample += 4;
    if (count > 1u << 20 || trun.body_size < p + per_sample * count)
        return fail("bad fragment");
    samples_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::size_t q = p + per_sample * i;
        if (flags & 0x100)
            q += 4; // duration
        const std::uint32_t sample = (flags & 0x200) ? be32(trun.body + q) : default_size_;
        if (sample == 0 || sample > 1u << 16)
            return fail("bad AAC frame size");
        samples_.push_back(sample);
    }
    return true;
}

bool Mp4Stream::decode(const unsigned char *frame, std::size_t size, std::vector<std::int16_t> &pcm)
{
    NeAACDecFrameInfo info{};
    void *out =
        NeAACDecDecode(static_cast<NeAACDecHandle>(decoder_), &info,
                       const_cast<unsigned char *>(frame), static_cast<unsigned long>(size));
    if (info.error != 0)
        return true; // one bad frame is skipped, not fatal
    if (out == nullptr || info.samples == 0 || info.channels == 0)
        return true;
    sample_rate_ = static_cast<int>(info.samplerate);
    channels_ = info.channels;
    const auto *samples = static_cast<const std::int16_t *>(out);
    pcm.insert(pcm.end(), samples, samples + info.samples);
    return true;
}

} // namespace fwd::aac
