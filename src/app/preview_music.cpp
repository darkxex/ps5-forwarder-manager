// ps5fwdgen - Plays a forwarder's selection music on a mixer music deck.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/preview_music.hpp"

#include <algorithm>
#include <cmath>

namespace fwd
{

void PreviewMusic::init(hui::audio::Mixer &mixer)
{
    mixer_ = &mixer;
    mixer_->swap_stream(kSlot, &rings_[active_ring_]);
    mixer_->set_stream_gain(kSlot, 0.0f, 0.0f);
}

bool PreviewMusic::play(const at9::Decoded &clip)
{
    if (mixer_ == nullptr || !clip.ok() || clip.frames == 0 || clip.channels <= 0 ||
        clip.sample_rate <= 0)
        return false;

    // To 48 kHz stereo S16: duplicate mono, drop extra channels, and resample
    // linearly when the rate differs (the website converts at 48 kHz, so this
    // is a fallback).
    const std::size_t in_frames = clip.frames;
    const int in_channels = clip.channels;
    const std::size_t out_frames =
        clip.sample_rate == hui::audio::kSampleRate
            ? in_frames
            : static_cast<std::size_t>(static_cast<double>(in_frames) * hui::audio::kSampleRate /
                                       clip.sample_rate);
    if (out_frames == 0)
        return false;
    std::vector<std::int16_t> pcm(out_frames * 2);
    const double step = static_cast<double>(in_frames) / static_cast<double>(out_frames);
    for (std::size_t i = 0; i < out_frames; ++i)
    {
        const double src = static_cast<double>(i) * step;
        const std::size_t a = std::min(static_cast<std::size_t>(src), in_frames - 1);
        const std::size_t b = std::min(a + 1, in_frames - 1);
        const float t = static_cast<float>(src - static_cast<double>(a));
        for (int ch = 0; ch < 2; ++ch)
        {
            const int sc = in_channels == 1 ? 0 : std::min(ch, in_channels - 1);
            const float sa = clip.pcm[a * static_cast<std::size_t>(in_channels) + sc];
            const float sb = clip.pcm[b * static_cast<std::size_t>(in_channels) + sc];
            pcm[i * 2 + ch] = static_cast<std::int16_t>(sa + (sb - sa) * t);
        }
    }

    // Start on the other, empty ring so no tail of the previous clip plays.
    active_ring_ = (active_ring_ + 1) % rings_.size();
    rings_[active_ring_].drain();
    stream_ = false;
    paused_ = false;
    pcm_ = std::move(pcm);
    frames_ = out_frames;
    position_ = 0;
    playing_ = true;
    pump(); // prime before the deck becomes audible
    mixer_->swap_stream(kSlot, &rings_[active_ring_]);
    mixer_->set_stream_gain(kSlot, 1.0f, 0.5f);
    return true;
}

void PreviewMusic::stop()
{
    if (mixer_ == nullptr || !playing_)
        return;
    mixer_->set_stream_gain(kSlot, 0.0f, 0.3f);
    playing_ = false;
    position_ = 0;
    stream_ = false;
    paused_ = false;
}

void PreviewMusic::pump()
{
    if (!playing_ || paused_ || frames_ == 0)
        return;
    hui::audio::StreamRing &ring = rings_[active_ring_];
    std::size_t space = ring.space();
    while (space > 0)
    {
        const std::size_t chunk = std::min<std::size_t>({space, 2048u, frames_ - position_});
        if (chunk == 0)
            break; // a stream waiting for more audio, or at its end
        scratch_.resize(chunk * 2);
        for (std::size_t i = 0; i < chunk * 2; ++i)
            scratch_[i] = static_cast<float>(pcm_[(position_ * 2) + i]) / 32768.0f;
        const std::size_t written = ring.write(scratch_.data(), chunk);
        if (written == 0)
            break;
        position_ += written;
        if (position_ >= frames_ && !stream_)
            position_ = 0; // loop, like the console's selection music
        space -= written;
    }
}

// ---- streaming ----------------------------------------------------------------

void PreviewMusic::restart_at(std::size_t frame)
{
    // A fresh ring: whatever the old one still holds is not heard again.
    active_ring_ = (active_ring_ + 1) % rings_.size();
    rings_[active_ring_].drain();
    position_ = std::min(frame, frames_);
    pump();
    mixer_->swap_stream(kSlot, &rings_[active_ring_]);
}

void PreviewMusic::stream_start()
{
    if (mixer_ == nullptr)
        return;
    pcm_.clear();
    frames_ = 0;
    stream_ = true;
    ended_ = false;
    paused_ = false;
    playing_ = true;
    resample_phase_ = 0.0;
    last_[0] = last_[1] = 0;
    restart_at(0);
    mixer_->set_stream_gain(kSlot, 1.0f, 0.2f);
}

void PreviewMusic::stream_append(const std::int16_t *pcm, std::size_t frames, int channels,
                                 int rate)
{
    if (!stream_ || frames == 0 || channels <= 0 || rate <= 0)
        return;
    // Linear resampling to 48 kHz stereo; the phase carries over between
    // calls so the joins are seamless.
    const double step = static_cast<double>(rate) / hui::audio::kSampleRate;
    const auto sample = [&](std::size_t frame, int ch) -> std::int16_t
    {
        const int sc = channels == 1 ? 0 : std::min(ch, channels - 1);
        return pcm[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(sc)];
    };
    double at = resample_phase_; // position in this call's input, may start at -1..0
    while (at < static_cast<double>(frames) - 1.0)
    {
        const double floor = std::floor(at);
        const float t = static_cast<float>(at - floor);
        const auto i = static_cast<long long>(floor);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float a = i < 0 ? last_[ch] : sample(static_cast<std::size_t>(i), ch);
            const float b = sample(static_cast<std::size_t>(i + 1), ch);
            pcm_.push_back(static_cast<std::int16_t>(a + (b - a) * t));
        }
        at += step;
    }
    resample_phase_ = at - static_cast<double>(frames);
    last_[0] = sample(frames - 1, 0);
    last_[1] = sample(frames - 1, 1);
    frames_ = pcm_.size() / 2;
}

void PreviewMusic::stream_end()
{
    ended_ = true;
}

void PreviewMusic::set_paused(bool paused)
{
    if (mixer_ == nullptr || !playing_ || paused == paused_)
        return;
    if (paused)
    {
        // Keep the audible place; what the ring still holds is dropped.
        const std::size_t audible =
            position_ - std::min(position_, rings_[active_ring_].available());
        paused_ = true;
        mixer_->set_stream_gain(kSlot, 0.0f, 0.08f);
        position_ = audible;
    }
    else
    {
        paused_ = false;
        restart_at(position_);
        mixer_->set_stream_gain(kSlot, 1.0f, 0.08f);
    }
}

void PreviewMusic::seek(double seconds)
{
    if (mixer_ == nullptr || !playing_)
        return;
    const auto frame = static_cast<std::size_t>(std::max(0.0, seconds) * hui::audio::kSampleRate);
    if (paused_)
        position_ = std::min(frame, frames_);
    else
        restart_at(frame);
}

double PreviewMusic::position() const
{
    if (!playing_)
        return 0.0;
    const std::size_t queued = paused_ ? 0 : rings_[active_ring_].available();
    return static_cast<double>(position_ - std::min(position_, queued)) / hui::audio::kSampleRate;
}

double PreviewMusic::buffered() const
{
    return static_cast<double>(frames_) / hui::audio::kSampleRate;
}

bool PreviewMusic::starved() const
{
    return stream_ && playing_ && !paused_ && !ended_ && position_ >= frames_ &&
           rings_[active_ring_].available() == 0;
}

bool PreviewMusic::finished() const
{
    return stream_ && ended_ && position_ >= frames_ && rings_[active_ring_].available() == 0;
}

void PreviewMusic::spectrum(float *levels, int bars) const
{
    for (int i = 0; i < bars; ++i)
        levels[i] = 0.0f;
    if (!playing_ || paused_ || frames_ == 0 || bars <= 0)
        return;
    // A Goertzel filter per band over the last ~21 ms that are audible, at
    // log-spaced frequencies from 60 Hz to 12 kHz (mono mix).
    constexpr std::size_t kWindow = 1024;
    const auto audible = static_cast<std::size_t>(position() * hui::audio::kSampleRate);
    if (audible < kWindow || audible > frames_)
        return;
    const std::size_t start = audible - kWindow;
    // A Hann window keeps the bands from leaking into each other.
    static const std::vector<double> window = []
    {
        std::vector<double> w(kWindow);
        for (std::size_t i = 0; i < kWindow; ++i)
            w[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) /
                                        static_cast<double>(kWindow - 1));
        return w;
    }();
    for (int b = 0; b < bars; ++b)
    {
        const double hz =
            60.0 * std::pow(12000.0 / 60.0, static_cast<double>(b) / std::max(1, bars - 1));
        const double coeff = 2.0 * std::cos(2.0 * 3.14159265358979 * hz / hui::audio::kSampleRate);
        double s1 = 0.0;
        double s2 = 0.0;
        for (std::size_t i = 0; i < kWindow; ++i)
        {
            const std::size_t at = (start + i) * 2;
            const double x = (pcm_[at] + pcm_[at + 1]) * (0.5 / 32768.0);
            const double s0 = x * window[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        const double magnitude = std::sqrt(std::max(0.0, power)) / (kWindow * 0.25);
        // To a 0..1 level on a 48 dB scale, with a gentle lift for the highs.
        const double db = 20.0 * std::log10(magnitude + 1e-9) + 6.0 * b / std::max(1, bars - 1);
        levels[b] = static_cast<float>(std::clamp((db + 54.0) / 48.0, 0.0, 1.0));
    }
}

} // namespace fwd
