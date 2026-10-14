// ps5fwdgen - Plays a forwarder's selection music on a mixer music deck, for
// the edit screen's live preview.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Owned by the App (so the rings outlive any screen; the audio thread keeps
// reading a ring for a few grains after a swap). Decoded PCM is kept as
// 48 kHz stereo S16 and fed to the mixer about a second ahead, looping, the
// way the console loops snd0.at9 on the home screen.
//
// It also streams, for the theme-music search: stream_start(), then PCM is
// appended while it plays (a track still downloading), with pause, seek, the
// play position and a spectrum of what is audible, for a visualizer.
#pragma once

#include "audio/at9.hpp"
#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fwd
{

class PreviewMusic
{
  public:
    // The mixer's audio thread may already be running; the deck is attached
    // with a thread-safe swap.
    void init(hui::audio::Mixer &mixer);

    // Starts looping the clip (any channel count / rate; converted here).
    // Fades the previous clip out. False when the clip is unusable.
    bool play(const at9::Decoded &clip);
    // Fades out and stops feeding the deck.
    void stop();
    // Once per frame, from the main thread.
    void pump();

    bool playing() const
    {
        return playing_;
    }

    // ---- streaming ----
    // Starts an empty, unlooped stream; audio is added with stream_append.
    void stream_start();
    // More audio for the stream: interleaved 16-bit, any rate and channels
    // (converted to 48 kHz stereo here, keeping the rate across calls).
    void stream_append(const std::int16_t *pcm, std::size_t frames, int channels, int rate);
    // No more audio will come; the stream stops at its end.
    void stream_end();
    bool streaming() const
    {
        return stream_;
    }
    // Paused streams keep their place; resuming plays from there.
    void set_paused(bool paused);
    bool paused() const
    {
        return paused_;
    }
    // Jump to a time (clamped to what has arrived so far).
    void seek(double seconds);
    // What is audible now, and how much has arrived (seconds).
    double position() const;
    double buffered() const;
    // True when playback caught up with the download.
    bool starved() const;
    // True when a finished stream played to its end.
    bool finished() const;
    // Loudness of `bars` bands (low to high, 0..1) around the audible
    // position, for a visualizer. All zero when nothing plays.
    void spectrum(float *levels, int bars) const;

  private:
    static constexpr std::size_t kSlot = 1; // deck 0 is left for app music
    static constexpr std::size_t kRingFrames = 1u << 16;

    hui::audio::Mixer *mixer_ = nullptr;
    // Two rings so a new clip starts on an empty one while the audio thread
    // finishes with the other.
    std::array<hui::audio::StreamRing, 2> rings_{hui::audio::StreamRing{kRingFrames},
                                                 hui::audio::StreamRing{kRingFrames}};
    std::size_t active_ring_ = 0;
    std::vector<std::int16_t> pcm_; // stereo 48 kHz
    std::size_t frames_ = 0;
    std::size_t position_ = 0;
    bool playing_ = false;
    std::vector<float> scratch_;
    // Streaming.
    bool stream_ = false;
    bool ended_ = false; // stream_end() was called
    bool paused_ = false;
    double resample_phase_ = 0.0; // carried between stream_append calls
    std::int16_t last_[2] = {0, 0};
    void restart_at(std::size_t frame);
};

} // namespace fwd
